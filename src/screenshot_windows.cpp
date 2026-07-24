#include <napi.h>
#include <node_api.h>
#include <windows.h>
#include <windowsx.h>  // For GET_X_LPARAM, GET_Y_LPARAM
#include <dwmapi.h>
#include <imm.h>       // For IME (Input Method Editor) support
#include <commdlg.h>   // For GetSaveFileNameW（保存对话框）
#include <shlobj.h>    // For SHGetKnownFolderPath（已知文件夹路径，如图片库）
#include <thread>
#include <atomic>
#include <algorithm>   // For std::min, std::max
#include <vector>
#include <string>
#include <cmath>      // For std::sqrt, std::fabs
#include <mutex>
#include <chrono>

// DWMWA_CLOAKED 在较新的 Windows SDK 中定义，为了兼容性手动定义
#ifndef DWMWA_CLOAKED
#define DWMWA_CLOAKED 14
#endif

// GDI+ 需要 min/max
namespace Gdiplus {
    using std::min;
    using std::max;
}
#include <gdiplus.h>

// AlphaBlend 需要 msimg32
#pragma comment(lib, "msimg32.lib")

#include "screenshot_windows.h"

// ---- nanosvg：SVG 光栅化（单文件库，宏实例化）----
// 两个 .h 必须在同一编译单元用宏实例化一次；这里在 screenshot_windows.cpp 内实例化。
#define NANOSVG_IMPLEMENTATION
#include "third_party/nanosvg.h"
#define NANOSVGRAST_IMPLEMENTATION
#include "third_party/nanosvgrast.h"

// ---- 截图工具栏图标 SVG 文本（构建期由 scripts/gen-icons.js 从 src/assets 生成）----
#include "generated/icon_svgs.h"

// 全局变量 - 区域截图
static HWND g_screenshotOverlayWindow = NULL;
static std::atomic<bool> g_isCapturing(false);
static napi_threadsafe_function g_screenshotTsfn = nullptr;
static std::thread g_screenshotThread;
// 自动确认模式：选区完成后直接出图，跳过编辑态（工具栏/标注）。
// 由 startRegionCaptureWithPrimedFrame 的 options.autoConfirm 设置，
// 会话开始时拷贝进 CaptureContext 供窗口过程读取。
static std::atomic<bool> g_autoConfirm(false);
static const auto SC_PRIMED_FRAME_TTL = std::chrono::milliseconds(500);

struct PrimedScreenshotFrame {
    HBITMAP bitmap = NULL;
    int vx = 0;
    int vy = 0;
    int vw = 0;
    int vh = 0;
    double dpiScale = 1.0;
    std::chrono::steady_clock::time_point capturedAt{};
    bool valid = false;
};

static PrimedScreenshotFrame g_primedScreenshotFrame;
static std::mutex g_primedScreenshotFrameMutex;

static void ReleasePrimedScreenshotFrameLocked() {
    if (g_primedScreenshotFrame.bitmap) {
        DeleteObject(g_primedScreenshotFrame.bitmap);
        g_primedScreenshotFrame.bitmap = NULL;
    }
    g_primedScreenshotFrame.vx = 0;
    g_primedScreenshotFrame.vy = 0;
    g_primedScreenshotFrame.vw = 0;
    g_primedScreenshotFrame.vh = 0;
    g_primedScreenshotFrame.dpiScale = 1.0;
    g_primedScreenshotFrame.capturedAt = std::chrono::steady_clock::time_point{};
    g_primedScreenshotFrame.valid = false;
}

// ==================== 区域截图功能（预截屏 + 双缓冲架构） ====================

// 截图常量
static const int SC_PANEL_WIDTH = 140;
static const int SC_PANEL_HEIGHT = 140;
static const int SC_MAGNIFIER_HEIGHT = 74;
static const int SC_PANEL_MARGIN = 15;
static const int SC_PANEL_CORNER_RADIUS = 8;
static const int SC_ZOOM_FACTOR = 4;

// 选区外遮罩：微信风格，选区内部保持清晰，外部覆盖半透明黑色
// 取值 0~255，数值越大越暗（0 = 无遮罩，255 = 全黑）
static const BYTE SC_MASK_ALPHA = 120;

// 截图状态枚举
enum CaptureState {
    CS_Idle,        // 等待选择（hover 窗口/拖拽开始）
    CS_Selecting,   // 正在拖拽框选
    CS_Confirmed,   // 已确认选区，可调整/拖动/打开工具栏
    CS_Resizing,    // 正在拖拽手柄调整选区
    CS_Moving,      // 正在整体拖动选区
    CS_Drawing,     // 正在绘制标注（矩形/圆/箭头/画笔）
    CS_TextEditing, // 正在输入文字
    CS_Done,
    CS_Cancelled
};

// 选区调整手柄（8 个方向）
enum ResizeHandle {
    RH_None = -1,
    RH_Left = 0,
    RH_Right = 1,
    RH_Top = 2,
    RH_Bottom = 3,
    RH_TopLeft = 4,
    RH_TopRight = 5,
    RH_BottomLeft = 6,
    RH_BottomRight = 7,
    RH_ArrowStart = 8,   // 箭头起点端点手柄（仅箭头用，拖动改起点）
    RH_ArrowEnd = 9      // 箭头终点端点手柄（仅箭头用，拖动改终点）
};

// 工具栏按钮
enum ToolButton {
    TB_Drag = 0,        // 拖拽
    TB_Rect,            // 矩形
    TB_Circle,          // 圆形（含椭圆）
    TB_Arrow,           // 箭头
    TB_Brush,           // 画笔
    TB_Mosaic,          // 马赛克
    TB_Text,            // 文字
    TB_Translate,       // 翻译
    TB_Separator1,      // 分隔线
    TB_Undo,            // 撤销
    TB_Redo,            // 重做
    TB_Separator2,      // 分隔线
    TB_Save,            // 保存到本地
    TB_Cancel,          // 取消
    TB_Confirm,         // 确定
    TB_Count
};

// ==================== 标注绘制（矩形/圆/箭头/画笔） ====================
// 所有标注统一用「选区相对逻辑坐标」存储（相对 selection.left/top 的偏移）：
//   - 实时渲染时：backDC 局部坐标 = curSelRect.left + relX
//   - 合成进 PNG 时：finalDC 原点正好是选区原点，标注直接画在 (relX, relY)
//   - 移动选区时：relX/relY 不变，标注自动跟随，无需额外换算
enum AnnotationType {
    AT_Rect,
    AT_Circle,
    AT_Arrow,
    AT_Brush,
    AT_Text,
    AT_Mosaic              // 马赛克（框选区域 或 鼠标涂抹）
};

struct Annotation {
    AnnotationType type;
    COLORREF color;
    int thickness;          // 逻辑像素（矢量=线宽；文字=字号）
    // 绝对虚拟屏幕坐标（与 ctx->mouseX/selection 同坐标系）。
    // 用绝对坐标而非选区相对，保证选区缩放/移动时标注位置固定不动。
    int x1, y1, x2, y2;     // Rect / Circle / Arrow 的起止（绝对坐标）；AT_Text 的 x1/y1 为文字锚点；
                            // AT_Mosaic 框选模式的矩形起止（绝对坐标）
    std::vector<POINT> pts; // Brush 自由路径（绝对坐标）；AT_Mosaic 涂抹模式的路径（绝对坐标）
    std::wstring text;      // AT_Text 的文字内容
    // ---- AT_Mosaic 专用 ----
    bool mosaicRect;        // true=框选区域马赛克；false=鼠标涂抹马赛克
    int mosaicSize;         // 马赛克块大小（逻辑像素）
    int brushRadius;        // 涂抹半径（逻辑像素，仅涂抹模式有效）

    // ---- 文字测量缓存（仅 AT_Text 有效）----
    // 缓存"相对锚点的字形偏移与尺寸"（与 GDI+ MeasureString 同源）。
    // 有效性条件 = (text, fontPx) 未变；锚点(x1,y1)变化不影响缓存值（外部加偏移即可），
    // 故 TransformAnnotationByBox 的 AT_Text 分支（仅平移锚点）无需失效缓存。
    // textCacheValid=false 表示未计算或已失效，下次 MeasureTextAnnotation 会重算并回填。
    bool textCacheValid;
    int textCacheFontPx;        // 生成缓存时的 fontPx（= thickness），用于校验
    float textCacheOffX, textCacheOffY;  // 字形左上角相对锚点的偏移
    float textCacheW, textCacheH;        // 字形紧凑宽高
};

// 粗细预设（逻辑像素，实际绘制粗细，渲染时乘 dpiScale）
static const int SC_THICK_PRESETS[] = { 1, 2, 4 };
static const int SC_THICK_COUNT = sizeof(SC_THICK_PRESETS) / sizeof(SC_THICK_PRESETS[0]);
static const int SC_DEFAULT_THICK_IDX = 1;  // 默认中粗
// 子菜单圆点预览直径（逻辑像素，仅用于界面显示，与实际绘制粗细解耦）
static const int SC_THICK_DOT_SIZES[] = { 5, 10, 16 };
static const int SC_THICK_DOT_COUNT = sizeof(SC_THICK_DOT_SIZES) / sizeof(SC_THICK_DOT_SIZES[0]);

// 文字字号预设（逻辑像素），文字工具激活时子菜单第一组显示
static const int SC_FONT_SIZES[] = { 16, 24, 36 };
static const int SC_FONT_COUNT = sizeof(SC_FONT_SIZES) / sizeof(SC_FONT_SIZES[0]);
static const int SC_DEFAULT_FONT_IDX = 1;  // 默认中号
static const wchar_t* SC_FONT_FACE = L"微软雅黑";

// 马赛克块大小预设（逻辑像素），马赛克工具子菜单显示
static const int SC_MOSAIC_SIZES[] = { 6, 10, 16 };
static const int SC_MOSAIC_COUNT = sizeof(SC_MOSAIC_SIZES) / sizeof(SC_MOSAIC_SIZES[0]);
static const int SC_DEFAULT_MOSAIC_IDX = 1;  // 默认中等块
// 涂抹半径预设（逻辑像素），马赛克涂抹模式使用，与画笔粗细预设共用同一组子菜单第二组无效，
// 这里单独定义便于扩展。半径越大涂抹范围越宽。
static const int SC_MOSAIC_RADIUS[] = { 12, 22, 36 };
static const int SC_MOSAIC_RADIUS_COUNT = sizeof(SC_MOSAIC_RADIUS) / sizeof(SC_MOSAIC_RADIUS[0]);
static const int SC_DEFAULT_MOSAIC_RADIUS_IDX = 1;  // 默认中等半径

// 颜色预设
static const COLORREF SC_COLOR_PRESETS[] = {
    RGB(0xE5, 0x39, 0x35),  // 红
    RGB(0xFB, 0x8C, 0x00),  // 橙
    RGB(0xFD, 0xD8, 0x35),  // 黄
    RGB(0x43, 0xA0, 0x47),  // 绿
    RGB(0x00, 0xAC, 0xC1),  // 青
    RGB(0x1E, 0x88, 0xE5),  // 蓝
    RGB(0xFF, 0xFF, 0xFF),  // 白
    RGB(0x33, 0x33, 0x33),  // 黑
};
static const int SC_COLOR_COUNT = sizeof(SC_COLOR_PRESETS) / sizeof(SC_COLOR_PRESETS[0]);
static const int SC_DEFAULT_COLOR_IDX = 0;  // 默认红

// 判断某工具按钮是否为可绘制矢量工具
static bool IsVectorTool(int btn) {
    return btn == TB_Rect || btn == TB_Circle || btn == TB_Arrow || btn == TB_Brush;
}

static bool IsDragTool(int btn) {
    return btn == TB_Drag;
}

static bool CanShowStylePopupTool(int btn) {
    return IsVectorTool(btn) || btn == TB_Text;
}

// ToolButton -> AnnotationType
static AnnotationType ToolToAnnotationType(int btn) {
    switch (btn) {
        case TB_Rect:   return AT_Rect;
        case TB_Circle: return AT_Circle;
        case TB_Arrow:  return AT_Arrow;
        case TB_Brush:  return AT_Brush;
        default:        return AT_Rect;
    }
}

// AnnotationType -> ToolButton（ToolToAnnotationType 的逆映射）。
// 用途：选中已有标注时，工具栏回显该标注对应的工具按钮（高亮 + 打开粗细/颜色子菜单）。
// AT_Mosaic 无对应工具按钮（马赛克标注不可选中），返回 -1。
static int AnnotationTypeToTool(AnnotationType t) {
    switch (t) {
        case AT_Rect:   return TB_Rect;
        case AT_Circle: return TB_Circle;
        case AT_Arrow:  return TB_Arrow;
        case AT_Brush:  return TB_Brush;
        case AT_Text:   return TB_Text;
        default:        return -1;  // AT_Mosaic 等无对应工具按钮
    }
}

// 手柄/工具栏几何常量
static const int SC_HANDLE_SIZE = 8;        // 调整手柄边长
static const int SC_TOOLBAR_BTN = 32;       // 按钮尺寸（正方形）
static const int SC_TOOLBAR_PAD = 6;        // 按钮↔工具栏边缘内边距（四边一致）
static const int SC_TOOLBAR_H = SC_TOOLBAR_BTN + SC_TOOLBAR_PAD * 2;  // 工具栏高度 = 按钮 + 上下内边距
static const int SC_TOOLBAR_GAP = 1;        // 按钮间距
static const int SC_TOOLBAR_RADIUS = 8;     // 工具栏圆角
static const int SC_TOOLBAR_MARGIN = 6;     // 选区到工具栏间距
static const int SC_TOOLBAR_BORDER = 1;     // 工具栏边框
static const int SC_MIN_SELECTION = 10;     // 最小选区尺寸

// 子菜单几何常量（100% DPI 基准值，运行时按 dpiScale 缩放）
// 单行布局：[粗细圆点×3] | [分隔线] | [颜色圆点×8]，无文案。
// 单元格（点击区 + 选中背景区）大小与工具栏按钮一致，便于视觉对齐。
static const int SC_POPUP_CELL = SC_TOOLBAR_BTN;  // 单元格尺寸（= 工具栏按钮大小）
static const int SC_POPUP_PAD = 4;           // 内边距
static const int SC_POPUP_RADIUS = 8;        // 圆角
static const int SC_POPUP_COLOR_DOT = 18;    // 颜色圆点直径（图标本身）
static const int SC_POPUP_SEP_GAP = 6;       // 分隔线两侧间距
static const int SC_POPUP_SEP_H = 20;        // 分隔线高度
static const int SC_POPUP_BORDER = 1;        // 边框
static const int SC_POPUP_MARGIN = 4;        // 工具栏与子菜单间距

// 子菜单几何（DPI 缩放后）
struct SCPopupMetrics {
    int pad;
    int radius;
    int cell;         // 单元格尺寸（点击区 + 选中背景区，= 工具栏按钮大小）
    int colorDot;     // 颜色圆点直径（图标本身）
    int sepGap;       // 分隔线两侧间距
    int sepH;         // 分隔线高度
    int border;
    int margin;
};

// ---- 信息面板 DPI 缩放几何 ----
struct SCPanelMetrics {
    int w;
    int h;
    int magnifierH;
    int margin;
    int radius;
    int fontPx;
    int crosshair;
    int borderPad;
    int labelPad;
    int sizeLabelPadX;
    int sizeLabelPadY;
    int sizeLabelGap;
};

static SCPanelMetrics CalcPanelMetrics(double dpiScale) {
    auto scale = [&](int v) { return (int)(v * dpiScale + 0.5); };
    SCPanelMetrics m;
    m.w = scale(SC_PANEL_WIDTH);
    m.h = scale(SC_PANEL_HEIGHT);
    m.magnifierH = scale(SC_MAGNIFIER_HEIGHT);
    m.margin = scale(SC_PANEL_MARGIN);
    m.radius = scale(SC_PANEL_CORNER_RADIUS);
    m.fontPx = scale(12);
    m.crosshair = (std::max)(1, scale(1));
    m.borderPad = (std::max)(1, scale(2));
    m.labelPad = scale(6);
    m.sizeLabelPadX = scale(12);
    m.sizeLabelPadY = scale(4);
    m.sizeLabelGap = scale(5);
    return m;
}

// 窗口信息
struct SCWindowInfo {
    HWND hwnd;
    RECT rect;
    std::wstring title;
};

// 截图结果结构
struct ScreenshotResult {
    bool success;
    int x;
    int y;
    int x2;
    int y2;
    int width;
    int height;
    std::string base64;
};

// GDI 资源缓存
struct SCGdiResources {
    HBRUSH bgBrush;
    HPEN borderPen;
    HPEN crosshairPen;
    HPEN selectionPen;
    HPEN highlightPen;
    HFONT smallFont;
    int smallFontPx;
    int crosshairWidth;
    // 选区外遮罩缓冲（虚拟屏幕大小，纯黑 + 常量 alpha），用于 AlphaBlend
    HDC maskDC;
    HBITMAP maskBitmap;
    // ---- P2 性能优化：固定样式 Pen/Brush 会话级缓存，避免每帧 Create/Delete ----
    // 工具栏分隔线笔（DrawToolbar）。
    HPEN toolbarSepPen;         // PS_SOLID, 1, RGB(230,230,230)
    // 文字选择高亮画刷（AlphaBlend 半透明选区底色）。
    HBRUSH textSelBrush;        // RGB(51,153,255)
    // 悬停/选中标注边框：蓝色虚线笔（悬停文字/非文字标注 + 选中非文字标注共用）。
    HPEN annHoverPen;           // PS_DASH, 1, RGB(0,136,255)
    // 选中文字标注边框：蓝色实线粗笔（与 selectionPen 的宽度 1 区别）。
    HPEN annTextSelPen;         // PS_SOLID, 2, RGB(0,136,255)

    void Init(int fontPx = 12, int crosshairPx = 1) {
        if (fontPx < 8) fontPx = 8;
        if (crosshairPx < 1) crosshairPx = 1;
        smallFontPx = fontPx;
        crosshairWidth = crosshairPx;
        bgBrush = CreateSolidBrush(RGB(52, 52, 53));
        borderPen = CreatePen(PS_SOLID, 0, RGB(102, 102, 102));
        crosshairPen = CreatePen(PS_SOLID, crosshairWidth, RGB(0, 136, 255));
        selectionPen = CreatePen(PS_SOLID, 1, RGB(0, 136, 255));
        highlightPen = CreatePen(PS_SOLID, 3, RGB(0, 136, 255));
        // 创建字体
        LOGFONTW lf = {};
        lf.lfHeight = -smallFontPx;
        lf.lfCharSet = DEFAULT_CHARSET;
        wcscpy_s(lf.lfFaceName, L"微软雅黑");
        smallFont = CreateFontIndirectW(&lf);
        maskDC = NULL;
        maskBitmap = NULL;
        // P2：预建固定样式 Pen/Brush，会话内复用。
        toolbarSepPen = CreatePen(PS_SOLID, 1, RGB(230, 230, 230));
        textSelBrush = CreateSolidBrush(RGB(51, 153, 255));
        annHoverPen = CreatePen(PS_DASH, 1, RGB(0, 136, 255));
        annTextSelPen = CreatePen(PS_SOLID, 2, RGB(0, 136, 255));
    }

    // 创建遮罩缓冲（纯黑位图，配合常量 alpha 实现 40%+ 半透明遮罩）
    // 须在 CaptureContext 虚拟屏幕尺寸确定后调用
    void InitMask(int virtualW, int virtualH) {
        HDC screenDC = GetDC(NULL);
        if (!screenDC) return;
        maskDC = CreateCompatibleDC(screenDC);
        if (maskDC) {
            maskBitmap = CreateCompatibleBitmap(screenDC, virtualW, virtualH);
            if (maskBitmap) {
                SelectObject(maskDC, maskBitmap);
                // 填充纯黑（AlphaBlend 用常量 alpha，源颜色为黑）
                RECT rc = { 0, 0, virtualW, virtualH };
                HBRUSH black = CreateSolidBrush(RGB(0, 0, 0));
                FillRect(maskDC, &rc, black);
                DeleteObject(black);
            } else {
                DeleteDC(maskDC);
                maskDC = NULL;
            }
        }
        ReleaseDC(NULL, screenDC);
    }

    void Cleanup() {
        if (bgBrush) { DeleteObject(bgBrush); bgBrush = NULL; }
        if (borderPen) { DeleteObject(borderPen); borderPen = NULL; }
        if (crosshairPen) { DeleteObject(crosshairPen); crosshairPen = NULL; }
        if (selectionPen) { DeleteObject(selectionPen); selectionPen = NULL; }
        if (highlightPen) { DeleteObject(highlightPen); highlightPen = NULL; }
        if (smallFont) { DeleteObject(smallFont); smallFont = NULL; }
        if (maskBitmap) { DeleteObject(maskBitmap); maskBitmap = NULL; }
        if (maskDC) { DeleteDC(maskDC); maskDC = NULL; }
        // P2：释放缓存的固定样式 Pen/Brush。
        if (toolbarSepPen) { DeleteObject(toolbarSepPen); toolbarSepPen = NULL; }
        if (textSelBrush) { DeleteObject(textSelBrush); textSelBrush = NULL; }
        if (annHoverPen) { DeleteObject(annHoverPen); annHoverPen = NULL; }
        if (annTextSelPen) { DeleteObject(annTextSelPen); annTextSelPen = NULL; }
    }
};

// ---- 工具栏 DPI 缩放几何 ----
// 基础逻辑尺寸（100% DPI）按 dpiScale 放大，保证 1080p → 4K 下工具栏尺寸与图标同步。
// 基础值与原 SC_TOOLBAR_* 常量保持一致，便于回归。
struct SCToolbarMetrics {
    int btn;       // 按钮边长
    int h;         // 工具栏高度
    int gap;       // 按钮间距
    int pad;       // 按钮↔工具栏边缘内边距（四边一致）
    int radius;    // 圆角半径
    int margin;    // 选区到工具栏间距
    int border;    // 工具栏边框宽度
    int iconSize;  // 图标光栅化尺寸（物理像素）
};

static SCToolbarMetrics CalcToolbarMetrics(double dpiScale) {
    auto scale = [&](int v) { return (int)(v * dpiScale + 0.5); };
    SCToolbarMetrics m;
    m.btn = scale(SC_TOOLBAR_BTN);
    m.h = scale(SC_TOOLBAR_H);
    m.gap = scale(SC_TOOLBAR_GAP);
    m.pad = scale(SC_TOOLBAR_PAD);
    m.radius = scale(SC_TOOLBAR_RADIUS);
    m.margin = scale(SC_TOOLBAR_MARGIN);
    m.border = scale(SC_TOOLBAR_BORDER);
    // 图标视觉内容约占按钮 ~72%，留出内边距；额外 +2px 余量提升抗锯齿质量
    m.iconSize = scale(SC_TOOLBAR_BTN - 8) + 2;
    return m;
}

// ---- SVG 图标光栅化 + 缓存 ----
// 将单个 SVG 文本光栅化为 32bpp 预乘 ARGB HBITMAP，尺寸 px×px。
// 把 SVG 中的 currentColor 替换为目标 color（normal/active 两色复用同一文本）。
static HBITMAP RenderSvgToBitmap(const char* svgText, COLORREF color, int px) {
    if (!svgText || px <= 0) return NULL;

    // 1. 替换 currentColor -> #RRGGBB（nanosvg 解析会改写 buffer，需可写副本）
    char colorHex[8];
    sprintf_s(colorHex, sizeof(colorHex), "#%02X%02X%02X",
              color & 0xFF, (color >> 8) & 0xFF, (color >> 16) & 0xFF);
    std::string svg(svgText);
    const std::string token = "currentColor";
    size_t pos = 0;
    while ((pos = svg.find(token, pos)) != std::string::npos) {
        svg.replace(pos, token.size(), colorHex);
        pos += 6;
    }

    // 2. 解析（nsvgParse 会就地修改传入字符串）
    NSVGimage* image = nsvgParse(&svg[0], "px", 96.0f);
    if (!image) return NULL;

    // 3. 光栅化到 RGBA 缓冲（非预乘）
    std::vector<unsigned char> rgba(px * px * 4, 0);
    NSVGrasterizer* rast = nsvgCreateRasterizer();
    if (!rast) { nsvgDelete(image); return NULL; }
    // 内容缩放到 72% 并居中，四周各留 14% 内边距，避免图标顶满按钮。
    // nanosvg 解析后 shape 已在 image->width 坐标系（viewBox 已折算），
    // 故 scale = px * 0.72 / image->width，偏移 tx = ty = px * 0.14。
    const float contentScale = 0.72f;
    const float pad = (1.0f - contentScale) * 0.5f;
    float refSize = (image->width > 0) ? image->width
                  : (image->height > 0) ? image->height : 24.0f;
    float scale = (float)px * contentScale / refSize;
    float tx = px * pad;
    float ty = px * pad;
    nsvgRasterize(rast, image, tx, ty, scale, rgba.data(), px, px, px * 4);
    nsvgDeleteRasterizer(rast);
    nsvgDelete(image);

    // 4. RGBA -> 预乘 BGRA（用于 AlphaBlend，避免黑边）
    //    nanosvg 输出 RGBA 字节序 [R,G,B,A]；而 32bpp BI_RGB DIB 内存布局为
    //    BGRA（像素值 0xAARRGGBB 在小端内存里是 B,G,R,A）。故预乘时需把 R、B
    //    对调写入，否则 memcpy 后通道会反，蓝色被画成黄色（灰度色看不出来）。
    for (int i = 0; i < px * px; i++) {
        unsigned char r = rgba[i * 4 + 0];
        unsigned char g = rgba[i * 4 + 1];
        unsigned char b = rgba[i * 4 + 2];
        unsigned char a = rgba[i * 4 + 3];
        // 预乘后按 DIB 的 BGRA 字节序写入
        rgba[i * 4 + 0] = (unsigned char)((b * a + 127) / 255);  // B
        rgba[i * 4 + 1] = (unsigned char)((g * a + 127) / 255);  // G
        rgba[i * 4 + 2] = (unsigned char)((r * a + 127) / 255);  // R
        rgba[i * 4 + 3] = a;                                      // A
    }

    // 5. 创建 32bpp ARGB HBITMAP
    HDC screenDC = GetDC(NULL);
    if (!screenDC) return NULL;
    HDC memDC = CreateCompatibleDC(screenDC);
    if (!memDC) { ReleaseDC(NULL, screenDC); return NULL; }

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = px;
    bmi.bmiHeader.biHeight = -px;  // 自上而下，避免垂直翻转
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(memDC, &bmi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (bmp && bits) {
        memcpy(bits, rgba.data(), px * px * 4);
    }
    DeleteDC(memDC);
    ReleaseDC(NULL, screenDC);
    return bmp;
}

// 工具栏图标位图缓存：按当前 DPI 渲染一次，dark/white 两色版本。
// dark = normal/hover 图标色，white = active（蓝底）图标色。
struct SCIconCache {
    bool inited;
    int iconSize;
    HBITMAP dark[TB_Count];    // 普通态：深灰图标
    HBITMAP active[TB_Count];  // 选中态：主题蓝图标（搭配浅蓝高亮底）

    SCIconCache() : inited(false), iconSize(0) {
        for (int i = 0; i < TB_Count; i++) { dark[i] = NULL; active[i] = NULL; }
    }

    void Init(int physicalIconSize) {
        if (inited) Cleanup();
        iconSize = physicalIconSize;
        COLORREF darkColor = RGB(60, 60, 60);
        // 选中态图标用主题蓝 #3B8BF2，与浅蓝高亮底搭配
        COLORREF activeColor = RGB(0x3B, 0x8B, 0xF2);
        for (int i = 0; i < TB_Count; i++) {
            // 分隔线（TB_Separator1/2）及未映射的项：kIconSvgs[i] 为 nullptr，跳过
            if (kIconSvgs[i]) {
                dark[i] = RenderSvgToBitmap(kIconSvgs[i], darkColor, iconSize);
                active[i] = RenderSvgToBitmap(kIconSvgs[i], activeColor, iconSize);
            }
        }
        inited = true;
    }

    // 取按钮位图：isActive 时用主题蓝版本，其余用深灰
    HBITMAP Get(int btn, bool isActive) const {
        if (btn < 0 || btn >= TB_Count) return NULL;
        return isActive ? active[btn] : dark[btn];
    }

    void Cleanup() {
        for (int i = 0; i < TB_Count; i++) {
            if (dark[i]) { DeleteObject(dark[i]); dark[i] = NULL; }
            if (active[i]) { DeleteObject(active[i]); active[i] = NULL; }
        }
        inited = false;
    }
};

// 计算工具栏宽度（所有按钮 + 间距 + 左右内边距 + 边框），按 metrics 缩放
static int CalcToolbarWidth(const SCToolbarMetrics& m) {
    return TB_Count * (m.btn + m.gap) - m.gap + m.pad * 2 + m.border * 2;
}

// 前向声明：PointInRect / AddRoundedRect 定义在本文件靠后位置，子菜单命中/绘制需要提前使用。
static bool PointInRect(int x, int y, const RECT& r);
static void AddRoundedRect(Gdiplus::GraphicsPath& outPath, int x, int y, int w, int h, int radius);

// ==================== 粗细/颜色子菜单 ====================

// 按 DPI 计算子菜单几何（单行布局）。
// 布局：[粗细圆点×3] sepGap | 分隔线 | sepGap [颜色圆点×8]
// 单元格尺寸与工具栏按钮一致（= SC_TOOLBAR_BTN），单元格间距 = SC_TOOLBAR_GAP，视觉对齐。
// 粗细圆点本身大小随预设值变化，颜色圆点固定直径，均居中在单元格内。
static SCPopupMetrics CalcPopupMetrics(double dpiScale) {
    auto scale = [&](int v) { return (int)(v * dpiScale + 0.5); };
    SCPopupMetrics m;
    m.pad = scale(SC_POPUP_PAD);
    m.radius = scale(SC_POPUP_RADIUS);
    m.cell = scale(SC_POPUP_CELL);
    m.colorDot = scale(SC_POPUP_COLOR_DOT);
    m.sepGap = scale(SC_POPUP_SEP_GAP);
    m.sepH = scale(SC_POPUP_SEP_H);
    m.border = scale(SC_POPUP_BORDER);
    m.margin = scale(SC_POPUP_MARGIN);
    return m;
}

// 子菜单总宽/高（单行）。单元格间距沿用工具栏按钮间距 SC_TOOLBAR_GAP。
static void CalcPopupSize(const SCPopupMetrics& m, int& outW, int& outH) {
    int cellGap = (int)(SC_TOOLBAR_GAP * (m.cell / (double)SC_POPUP_CELL) + 0.5);
    // 粗细组宽
    int thickW = SC_THICK_COUNT * m.cell + (SC_THICK_COUNT - 1) * cellGap;
    // 颜色组宽
    int colorW = SC_COLOR_COUNT * m.cell + (SC_COLOR_COUNT - 1) * cellGap;
    int contentW = thickW + m.sepGap * 2 + 1 + colorW;  // 1 = 分隔线宽度
    outW = contentW + m.pad * 2 + m.border * 2;
    outH = m.cell + m.pad * 2 + m.border * 2;
}

// 计算子菜单位置（通用）：贴工具栏下方，放不下则上方，左右钳制到虚拟屏幕内。
// toolbarRect / out 均为相对虚拟屏幕坐标；pw/ph 为子菜单尺寸。
static void CalcPopupPlacement(const RECT& toolbarRect,
                               int virtualW, int virtualH,
                               const SCPopupMetrics& m, int pw, int ph, RECT& out) {
    // 水平：与工具栏左对齐
    int x = toolbarRect.left;
    // 垂直：优先工具栏下方
    int y = toolbarRect.bottom + m.margin;
    if (y + ph > virtualH) {
        y = toolbarRect.top - m.margin - ph;
    }
    // 左右钳制
    if (x + pw > virtualW) x = virtualW - pw - m.margin;
    if (x < 0) x = m.margin;

    out.left = x;
    out.top = y;
    out.right = x + pw;
    out.bottom = y + ph;
}

// 计算子菜单位置：贴工具栏下方，放不下则上方，左右钳制到虚拟屏幕内。
// toolbarRect / out 均为相对虚拟屏幕坐标（与工具栏一致）。
static void CalcPopupPosition(const RECT& toolbarRect,
                              int virtualW, int virtualH,
                              const SCPopupMetrics& m, RECT& out) {
    int pw, ph;
    CalcPopupSize(m, pw, ph);
    CalcPopupPlacement(toolbarRect, virtualW, virtualH, m, pw, ph, out);
}

// 马赛克子菜单几何常量
// 模式切换组：2 个单元格（涂抹 / 框选）；块大小组：3 个单元格。
static const int SC_MOSAIC_MODE_COUNT = 2;   // 涂抹、框选
// 涂抹模式用半径圆点表示，块大小用马赛克方块网格表示（绘制时按预设值缩放）

// 计算马赛克子菜单总宽/高（单行）。
// 布局：[模式×2] sepGap | 分隔线 | sepGap [块大小×3] sepGap | 分隔线 | sepGap [涂抹半径×3]
static void CalcMosaicPopupSize(const SCPopupMetrics& m, int& outW, int& outH) {
    int cellGap = (int)(SC_TOOLBAR_GAP * (m.cell / (double)SC_POPUP_CELL) + 0.5);
    int modeW = SC_MOSAIC_MODE_COUNT * m.cell + (SC_MOSAIC_MODE_COUNT - 1) * cellGap;
    int sizeW = SC_MOSAIC_COUNT * m.cell + (SC_MOSAIC_COUNT - 1) * cellGap;
    int radiusW = SC_MOSAIC_RADIUS_COUNT * m.cell + (SC_MOSAIC_RADIUS_COUNT - 1) * cellGap;
    // 两组分隔线，每组分隔线宽 = sepGap*2 + 1
    int contentW = modeW + (m.sepGap * 2 + 1) + sizeW + (m.sepGap * 2 + 1) + radiusW;
    outW = contentW + m.pad * 2 + m.border * 2;
    outH = m.cell + m.pad * 2 + m.border * 2;
}

// 命中测试马赛克子菜单，返回值约定：
//   +1       = 涂抹模式；+2 = 框选模式
//   +101..   = 第 N 个块大小（100 + sizeIdx + 1）
//   +201..   = 第 N 个涂抹半径（200 + radiusIdx + 1）
//    0 = 未命中
static int HitTestMosaicPopup(int x, int y, const RECT& popupRect, const SCPopupMetrics& m) {
    if (!PointInRect(x, y, popupRect)) return 0;
    int contentLeft = popupRect.left + m.border + m.pad;
    int contentTop = popupRect.top + m.border + m.pad;
    int cellGap = (int)(SC_TOOLBAR_GAP * (m.cell / (double)SC_POPUP_CELL) + 0.5);
    if (y < contentTop || y >= contentTop + m.cell) return 0;

    // 模式组
    for (int i = 0; i < SC_MOSAIC_MODE_COUNT; i++) {
        int ix = contentLeft + i * (m.cell + cellGap);
        if (x >= ix && x < ix + m.cell) return i + 1;  // +1=涂抹 +2=框选
    }
    int modeEndX = contentLeft + SC_MOSAIC_MODE_COUNT * m.cell
        + (SC_MOSAIC_MODE_COUNT - 1) * cellGap;
    int sizeStartX = modeEndX + m.sepGap * 2 + 1;
    if (x < sizeStartX) return 0;  // 第一条分隔线区域
    // 块大小组
    int sizeEndX = sizeStartX + SC_MOSAIC_COUNT * m.cell
        + (SC_MOSAIC_COUNT - 1) * cellGap;
    for (int i = 0; i < SC_MOSAIC_COUNT; i++) {
        int ix = sizeStartX + i * (m.cell + cellGap);
        if (x >= ix && x < ix + m.cell) return 100 + i + 1;
    }
    // 涂抹半径组
    int radiusStartX = sizeEndX + m.sepGap * 2 + 1;
    if (x < radiusStartX) return 0;  // 第二条分隔线区域
    for (int i = 0; i < SC_MOSAIC_RADIUS_COUNT; i++) {
        int ix = radiusStartX + i * (m.cell + cellGap);
        if (x >= ix && x < ix + m.cell) return 200 + i + 1;
    }
    return 0;
}

// 绘制马赛克子菜单（单行）。
// modeIdx：当前模式（0=涂抹 1=框选）；sizeIdx：当前块大小索引；radiusIdx：涂抹半径索引。
static void DrawMosaicPopup(HDC hdc, const RECT& popupRect,
                            int modeIdx, int sizeIdx, int radiusIdx,
                            const SCPopupMetrics& m) {
    // GDI+ 已由会话级 InitGdipResources 启动，此处直接使用（Graphics 按 hdc 新建）。
    {
        Gdiplus::Graphics graphics(hdc);
        graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);

        int pw = popupRect.right - popupRect.left;
        int ph = popupRect.bottom - popupRect.top;

        // 白色圆角背景 + 浅灰边框
        Gdiplus::SolidBrush whiteBrush(Gdiplus::Color(255, 255, 255, 255));
        Gdiplus::GraphicsPath bgPath;
        AddRoundedRect(bgPath, popupRect.left, popupRect.top, pw, ph, m.radius);
        graphics.FillPath(&whiteBrush, &bgPath);
        Gdiplus::Pen borderPen(Gdiplus::Color(255, 210, 210, 210), (Gdiplus::REAL)m.border);
        graphics.DrawPath(&borderPen, &bgPath);

        int contentLeft = popupRect.left + m.border + m.pad;
        int contentTop = popupRect.top + m.border + m.pad;
        int midY = contentTop + m.cell / 2;
        int cellGap = (int)(SC_TOOLBAR_GAP * (m.cell / (double)SC_POPUP_CELL) + 0.5);

        auto drawCellBg = [&](int cellLeft) {
            Gdiplus::GraphicsPath p;
            AddRoundedRect(p, cellLeft, contentTop, m.cell, m.cell, m.cell / 4);
            Gdiplus::SolidBrush bgBrush(Gdiplus::Color(255, 225, 237, 253));
            graphics.FillPath(&bgBrush, &p);
            Gdiplus::Pen edgePen(Gdiplus::Color(90, 160, 160, 160), 1.0f);
            graphics.DrawPath(&edgePen, &p);
        };
        auto cellColor = [&](bool sel) -> Gdiplus::Color {
            return sel ? Gdiplus::Color(255, 0x3B, 0x8B, 0xF2)
                       : Gdiplus::Color(255, 0x33, 0x33, 0x33);
        };

        // 模式组
        for (int i = 0; i < SC_MOSAIC_MODE_COUNT; i++) {
            int cellLeft = contentLeft + i * (m.cell + cellGap);
            bool sel = (i == modeIdx);
            if (sel) drawCellBg(cellLeft);
            int cx = cellLeft + m.cell / 2;
            Gdiplus::Color c = cellColor(sel);
            if (i == 0) {
                // 涂抹模式：画一个画笔/毛刷图标（一条波浪线 + 圆头）
                Gdiplus::Pen pen(c, (Gdiplus::REAL)2.0f);
                pen.SetLineJoin(Gdiplus::LineJoinRound);
                pen.SetStartCap(Gdiplus::LineCapRound);
                pen.SetEndCap(Gdiplus::LineCapRound);
                int r = (int)(m.cell * 0.22);
                // 自由曲线（模拟涂抹轨迹）
                Gdiplus::PointF curve[] = {
                    Gdiplus::PointF((float)(cx - m.cell * 0.28), (float)(midY + m.cell * 0.18)),
                    Gdiplus::PointF((float)(cx - m.cell * 0.10), (float)(midY - m.cell * 0.18)),
                    Gdiplus::PointF((float)(cx + m.cell * 0.10), (float)(midY + m.cell * 0.18)),
                    Gdiplus::PointF((float)(cx + m.cell * 0.28), (float)(midY - m.cell * 0.18)),
                };
                graphics.DrawLines(&pen, curve, 4);
            } else {
                // 框选模式：画一个虚线矩形
                Gdiplus::Pen pen(c, (Gdiplus::REAL)2.0f);
                int r = (int)(m.cell * 0.24);
                graphics.DrawRectangle(&pen, cx - r, midY - r, r * 2, r * 2);
            }
        }

        // 分隔线
        int modeEndX = contentLeft + SC_MOSAIC_MODE_COUNT * m.cell
            + (SC_MOSAIC_MODE_COUNT - 1) * cellGap;
        int sepX = modeEndX + m.sepGap;
        Gdiplus::Pen sepPen(Gdiplus::Color(255, 220, 220, 220), 1.0f);
        graphics.DrawLine(&sepPen, sepX, midY - m.sepH / 2, sepX, midY + m.sepH / 2);

        // 块大小组：每个单元格画一个 N×N 的马赛克方块网格，块越大网格越粗
        int sizeStartX = sepX + m.sepGap + 1;
        for (int i = 0; i < SC_MOSAIC_COUNT; i++) {
            int cellLeft = sizeStartX + i * (m.cell + cellGap);
            bool sel = (i == sizeIdx);
            if (sel) drawCellBg(cellLeft);
            Gdiplus::Color c = cellColor(sel);
            int cx = cellLeft + m.cell / 2;
            // 网格区域边长（占单元格约 0.6）
            int gridHalf = (int)(m.cell * 0.26);
            int gridSize = gridHalf * 2;
            int gx = cx - gridHalf;
            int gy = midY - gridHalf;
            // 块数随预设递增：i=0 -> 2x2, i=1 -> 3x3, i=2 -> 4x4
            int n = 2 + i;
            int cellSz = gridSize / n;
            if (cellSz < 1) cellSz = 1;
            Gdiplus::SolidBrush b(c);
            // 交错填充模拟马赛克质感（棋盘格）
            for (int ry = 0; ry < n; ry++) {
                for (int rx = 0; rx < n; rx++) {
                    if (((rx + ry) & 1) == 0) {
                        graphics.FillRectangle(&b, gx + rx * cellSz, gy + ry * cellSz,
                                               cellSz, cellSz);
                    }
                }
            }
            // 网格描边（未填充格用半透明）
            Gdiplus::Pen gridPen(Gdiplus::Color(sel ? 200 : 120,
                                                c.GetRed(), c.GetGreen(), c.GetBlue()),
                                 1.0f);
            for (int k = 0; k <= n; k++) {
                graphics.DrawLine(&gridPen, gx + k * cellSz, gy,
                                  gx + k * cellSz, gy + n * cellSz);
                graphics.DrawLine(&gridPen, gx, gy + k * cellSz,
                                  gx + n * cellSz, gy + k * cellSz);
            }
        }

        // 第二条分隔线
        int sizeEndX = sizeStartX + SC_MOSAIC_COUNT * m.cell
            + (SC_MOSAIC_COUNT - 1) * cellGap;
        int sep2X = sizeEndX + m.sepGap;
        graphics.DrawLine(&sepPen, sep2X, midY - m.sepH / 2, sep2X, midY + m.sepH / 2);

        // 涂抹半径组：用不同直径的圆点表示半径大小（类似画笔粗细）。
        // 仅涂抹模式下有意义；框选模式下置灰但仍可点击（切换后立即生效）。
        int radiusStartX = sep2X + m.sepGap + 1;
        bool radiusEnabled = (modeIdx == 0);
        for (int i = 0; i < SC_MOSAIC_RADIUS_COUNT; i++) {
            int cellLeft = radiusStartX + i * (m.cell + cellGap);
            bool sel = (i == radiusIdx) && radiusEnabled;
            if (sel) drawCellBg(cellLeft);
            Gdiplus::Color c = cellColor(sel);
            int cx = cellLeft + m.cell / 2;
            // 圆点直径随预设递增：小/中/大
            int dotD = (int)(SC_MOSAIC_RADIUS[i] * 0.5 * (m.cell / (double)SC_POPUP_CELL) + 0.5);
            if (dotD < 5) dotD = 5;
            if (dotD > m.cell - 4) dotD = m.cell - 4;
            int r = dotD / 2;
            Gdiplus::Color drawC = radiusEnabled ? c
                : Gdiplus::Color(160, c.GetRed(), c.GetGreen(), c.GetBlue());
            Gdiplus::SolidBrush brush(drawC);
            graphics.FillEllipse(&brush, cx - r, midY - r, r * 2, r * 2);
        }
    }
}

// 命中测试子菜单，返回值约定：
//   +1..+SC_THICK_COUNT = 第 N 个粗细
//   -1..-SC_COLOR_COUNT = 第 N 个颜色（取负为索引+1）
//    0 = 未命中（包括点在分隔线上）
static int HitTestPopup(int x, int y, const RECT& popupRect, const SCPopupMetrics& m) {
    if (!PointInRect(x, y, popupRect)) return 0;

    // 内容左边界（绝对）
    int contentLeft = popupRect.left + m.border + m.pad;
    int contentTop = popupRect.top + m.border + m.pad;
    int cellGap = (int)(SC_TOOLBAR_GAP * (m.cell / (double)SC_POPUP_CELL) + 0.5);
    // y 必须在单元格高度内
    if (y < contentTop || y >= contentTop + m.cell) return 0;

    // 粗细组：contentLeft 起
    int thickX0 = contentLeft;
    for (int i = 0; i < SC_THICK_COUNT; i++) {
        int ix = thickX0 + i * (m.cell + cellGap);
        if (x >= ix && x < ix + m.cell) return i + 1;
    }
    int thickEndX = thickX0 + SC_THICK_COUNT * m.cell
        + (SC_THICK_COUNT - 1) * cellGap;
    // 分隔线区域（不命中）
    int colorStartX = thickEndX + m.sepGap * 2 + 1;
    if (x < colorStartX) return 0;

    // 颜色组
    for (int i = 0; i < SC_COLOR_COUNT; i++) {
        int ix = colorStartX + i * (m.cell + cellGap);
        if (x >= ix && x < ix + m.cell) return -(i + 1);
    }
    return 0;
}

// 绘制子菜单（GDI+ 抗锯齿白底圆角，单行布局）。
// 第一组：isTextTool 时显示字号（不同大小 'A'），否则显示粗细（圆点直径区分）。
// 第二组：颜色（固定直径圆点）。中间一条竖直分隔线。
static void DrawPopup(HDC hdc, const RECT& popupRect,
                      int colorIdx, int firstIdx, bool isTextTool,
                      const SCPopupMetrics& m) {
    // GDI+ 已由会话级 InitGdipResources 启动，此处直接使用（Graphics 按 hdc 新建）。
    {
        Gdiplus::Graphics graphics(hdc);
        graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);

        int pw = popupRect.right - popupRect.left;
        int ph = popupRect.bottom - popupRect.top;

        // 白色圆角背景
        Gdiplus::SolidBrush whiteBrush(Gdiplus::Color(255, 255, 255, 255));
        Gdiplus::GraphicsPath bgPath;
        AddRoundedRect(bgPath, popupRect.left, popupRect.top, pw, ph, m.radius);
        graphics.FillPath(&whiteBrush, &bgPath);
        // 浅灰边框
        Gdiplus::Pen borderPen(Gdiplus::Color(255, 210, 210, 210), (Gdiplus::REAL)m.border);
        graphics.DrawPath(&borderPen, &bgPath);

        int contentLeft = popupRect.left + m.border + m.pad;
        int contentTop = popupRect.top + m.border + m.pad;
        int midY = contentTop + m.cell / 2;
        int cellGap = (int)(SC_TOOLBAR_GAP * (m.cell / (double)SC_POPUP_CELL) + 0.5);

        // 选中态背景：单元格大小的圆角矩形。
        // 粗细沿用主工具栏选中高亮底（不透明浅蓝 RGB(225,237,253)）；
        // 颜色用带透明（alpha 80）的选中色。统一加一条带透明灰色描边。
        auto drawCellBg = [&](int cellLeft, const Gdiplus::Color& bg) {
            Gdiplus::GraphicsPath p;
            AddRoundedRect(p, cellLeft, contentTop, m.cell, m.cell, m.cell / 4);
            Gdiplus::SolidBrush bgBrush(bg);
            graphics.FillPath(&bgBrush, &p);
            // 背景边缘描边（带透明灰，alpha 90）
            Gdiplus::Pen edgePen(Gdiplus::Color(90, 160, 160, 160), 1.0f);
            graphics.DrawPath(&edgePen, &p);
        };

        // 第一组：文字工具显示字号（不同大小 'A'），矢量工具显示粗细（圆点直径区分）
        int firstCount = isTextTool ? SC_FONT_COUNT : SC_THICK_COUNT;
        int firstX0 = contentLeft;
        for (int i = 0; i < firstCount; i++) {
            int cellLeft = firstX0 + i * (m.cell + cellGap);
            if (i == firstIdx) {
                // 与主工具栏选中态一致的不透明浅蓝底
                drawCellBg(cellLeft, Gdiplus::Color(255, 225, 237, 253));
            }
            Gdiplus::Color iconC = (i == firstIdx)
                ? Gdiplus::Color(255, 0x3B, 0x8B, 0xF2)
                : Gdiplus::Color(255, 0x33, 0x33, 0x33);
            int cx = cellLeft + m.cell / 2;
            if (isTextTool) {
                // 字号：用不同大小的字母 'A' 表示，居中绘制。
                // 缩放系数：最大字号（36）额外缩小到 0.62，前两个字号（16/24）保持 0.72，
                // 避免最大字号的 'A' 仍偏大溢出。
                double sizeScale = (i == SC_FONT_COUNT - 1) ? 0.62 : 0.72;
                int fontPx = (int)(SC_FONT_SIZES[i] * (m.cell / (double)SC_POPUP_CELL) * sizeScale + 0.5);
                if (fontPx < 6) fontPx = 6;
                Gdiplus::FontFamily fontFamily(SC_FONT_FACE);
                Gdiplus::FontStyle fs = Gdiplus::FontStyleRegular;
                // 'A' 像素高 = fontPx，按高度反推 emSize（GDI+ 用 em）
                Gdiplus::Font font(&fontFamily, (Gdiplus::REAL)fontPx, fs, Gdiplus::UnitPixel);
                Gdiplus::SolidBrush b(iconC);
                Gdiplus::StringFormat sf;
                sf.SetAlignment(Gdiplus::StringAlignmentCenter);
                sf.SetLineAlignment(Gdiplus::StringAlignmentCenter);
                Gdiplus::RectF cellRect((Gdiplus::REAL)cellLeft, (Gdiplus::REAL)contentTop,
                                        (Gdiplus::REAL)m.cell, (Gdiplus::REAL)m.cell);
                graphics.DrawString(L"A", 1, &font, cellRect, &sf, &b);
            } else {
                // 粗细：圆点直径取自 SC_THICK_DOT_SIZES
                int dotD = (int)(SC_THICK_DOT_SIZES[i] * (m.cell / (double)SC_POPUP_CELL) + 0.5);
                if (dotD < 4) dotD = 4;
                if (dotD > m.cell) dotD = m.cell;
                int r = dotD / 2;
                Gdiplus::SolidBrush brush(iconC);
                graphics.FillEllipse(&brush, cx - r, midY - r, r * 2, r * 2);
            }
        }

        // 分隔线
        int firstEndX = firstX0 + firstCount * m.cell
            + (firstCount - 1) * cellGap;
        int sepX = firstEndX + m.sepGap;
        Gdiplus::Pen sepPen(Gdiplus::Color(255, 220, 220, 220), 1.0f);
        graphics.DrawLine(&sepPen, sepX, midY - m.sepH / 2, sepX, midY + m.sepH / 2);

        // 颜色组：选中时背景变为带透明的选中色
        int colorStartX = sepX + m.sepGap + 1;
        for (int i = 0; i < SC_COLOR_COUNT; i++) {
            COLORREF c = SC_COLOR_PRESETS[i];
            int cellLeft = colorStartX + i * (m.cell + cellGap);
            int cx = cellLeft + m.cell / 2;
            int r = m.colorDot / 2;
            if (i == colorIdx) {
                drawCellBg(cellLeft,
                    Gdiplus::Color(80, GetRValue(c), GetGValue(c), GetBValue(c)));
            }
            Gdiplus::Color gc(GetRValue(c), GetGValue(c), GetBValue(c));
            Gdiplus::SolidBrush brush(gc);
            graphics.FillEllipse(&brush, cx - r, midY - r, r * 2, r * 2);
            // 圆点本身始终保留极浅描边（白色块可见性），选中也保留
            Gdiplus::Pen outline(Gdiplus::Color(255, 220, 220, 220), 1.0f);
            graphics.DrawEllipse(&outline, cx - r, midY - r, r * 2, r * 2);
        }
    }
}

// 截图上下文
struct CaptureContext {
    CaptureState state;
    // 自动确认模式：选区确定后直接提取并完成截图，不进入编辑态（工具栏/标注）。
    // 仅在 WM_LBUTTONUP 的 CS_Selecting 分支生效。
    bool autoConfirm;
    int virtualX, virtualY, virtualW, virtualH;
    int startX, startY, endX, endY;
    int mouseX, mouseY;
    COLORREF currentColor;
    std::vector<SCWindowInfo> windows;
    int hoveredWindow; // -1 = none
    // 预截屏
    HBITMAP screenBitmap;
    HDC memDC;
    // 双缓冲
    HDC backDC;
    HBITMAP backBitmap;
    // 脏区域追踪
    RECT lastPanelRect;
    RECT lastSelectionRect;
    RECT lastLabelRect;
    RECT lastHighlightRect;
    RECT lastToolbarRect;
    RECT lastPopupRect;
    // P1 局部刷新用：上帧光标/被操作标注/正在绘制标注的包围盒（供 InvalidateRect 计算旧位置）
    RECT lastCaretRect;             // 上帧文字光标矩形（backDC 坐标），hasLastCaret=false 表示无效
    bool hasLastCaret;
    RECT lastAnnotationBox;         // 上帧被拖拽/缩放标注的包围盒（绝对虚拟屏幕坐标）
    bool hasLastAnnotationBox;
    RECT lastDrawingBox;            // 上帧 curDrawing 包围盒（绝对虚拟屏幕坐标）
    bool hasLastDrawingBox;
    bool needFullRedraw;
    // DPI
    double dpiScale;
    // GDI 资源
    SCGdiResources gdi;
    SCPanelMetrics panelMetrics;

    // ---- 确认态：可调整选区 ----
    // 已确认的选区（绝对屏幕坐标）
    RECT selection;
    // 当前正在拖拽的手柄（CS_Resizing 时有效），CS_Confirmed 下表示 hover 手柄
    int resizeHandle;
    // 整体拖动/调整起点（绝对屏幕坐标）
    int dragStartX, dragStartY;
    RECT dragStartSelection;

    // ---- 悬浮工具栏 ----
    // 工具栏矩形（相对虚拟屏幕坐标，绘制用）
    RECT toolbarRect;
    // 工具栏 hover 按钮，-1 = none
    int hoverToolbarBtn;
    // 当前激活的工具（高亮显示，仅界面）
    int activeTool;
    // 当前子菜单/参数面板对应的工具来源；拖拽工具下选中覆盖物时可继续回显其参数。
    int popupTool;
    // 工具栏图标位图缓存（按 DPI 预渲染，dark/white 双色）
    SCIconCache iconCache;
    // 当前 DPI 下的工具栏几何（缓存，避免每次绘制重算）
    SCToolbarMetrics toolbarMetrics;

    // ---- 标注绘制 ----
    std::vector<Annotation> annotations;   // 已提交标注
    std::vector<std::vector<Annotation>> undoStack;
    std::vector<std::vector<Annotation>> redoStack;
    Annotation curDrawing;                 // CS_Drawing 中正在绘制的标注
    bool hasCurDrawing;                    // curDrawing 是否有效
    int drawColorIdx;                      // 当前选中颜色索引
    int drawThickIdx;                      // 当前选中粗细索引（矢量工具）
    int fontSizeIdx;                       // 当前选中字号索引（文字工具）
    // 马赛克工具属性
    int mosaicSizeIdx;                     // 当前选中马赛克块大小索引
    int mosaicRadiusIdx;                   // 当前选中涂抹半径索引
    bool mosaicRectMode;                   // true=框选区域模式；false=涂抹模式
    // 涂抹模式光标：用系统光标机制（SetCursor）显示半径圆，由 OS 跟随鼠标，
    // 无 WM_PAINT 重绘延迟（之前的 overlay 圆走 MOUSEMOVE→InvalidateRect→WM_PAINT 链路，
    // 全屏重绘开销大导致不跟手）。按半径预设预生成彩色光标并缓存。
    HCURSOR mosaicBrushCursors[3];         // 对应 SC_MOSAIC_RADIUS_COUNT 个半径预设的光标
    bool mosaicBrushCursorsInited;
    // ---- 马赛克渲染（reveal-mask 模型，消除不连续感）----
    // 预先把整张截图按当前块大小马赛克化得到 mosaicBase（逻辑像素，与 backDC 同尺寸）。
    // 马赛克标注只是「蒙版」：涂抹=路径圆形区域、框选=矩形区域，揭示其背后的 mosaicBase。
    // 这样任意区域、任意顺序叠加都连续无缝；切换块大小时只需重建 base，已揭示区域自动更新。
    // mosaicBase 覆盖整虚拟屏幕（绝对坐标），与选区无关，resize/move 无需重建。
    HDC mosaicBaseDC;
    HBITMAP mosaicBaseBitmap;
    int mosaicBaseW, mosaicBaseH;          // base 尺寸（= 虚拟屏幕逻辑尺寸）
    int mosaicBaseBlockPx;                 // 生成 base 时的块大小（检测变更触发重建）
    // 涂抹模式增量绘制：记录上一帧最后绘制的路径点索引（reveal 模型下未使用，保留扩展）。
    int mosaicDrawLastIdx;
    // 粗细/颜色子菜单
    bool popupOpen;
    RECT popupRect;
    SCPopupMetrics popupMetrics;

    // ---- 文字输入（CS_TextEditing）----
    std::wstring textBuf;                  // 正在输入的文字缓冲
    int textAnchorX, textAnchorY;          // 文字锚点（绝对虚拟屏幕坐标）
    int textCaretPos;                      // 插入符在 textBuf 中的 wchar 位置
    bool textCaretVisible;                 // 光标是否可见（闪烁控制）
    DWORD textCaretLastBlink;              // 上次光标闪烁时间（毫秒）
    int textSelStart;                      // 文字选择起始位置（-1 表示无选择）
    int textSelEnd;                        // 文字选择结束位置
    bool textDraggingSelection;            // 是否正在拖动选择文字
    int hoveredTextAnnotation;             // 悬浮命中的文字标注索引（-1 表示无，仅用于光标/即时反馈）
    int selectedTextAnnotation;            // 已选中的文字标注索引（-1 表示无，持久保持直到点空白）
    int draggingTextAnnotation;            // 正在拖动的文字标注索引（-1 表示无）
    int textDragStartX, textDragStartY;    // 文字拖动起始位置
    // ---- 非文字标注的选中/拖拽/缩放（与文字机制互斥：选中非文字时清文字选中，反之亦然）----
    int hoveredAnnotation;                 // 悬浮命中的非文字标注索引（-1=无，用于虚线框/光标即时反馈）
    int selectedAnnotation;                // 已选中的非文字标注索引（-1=无，持久保持直到点空白/进入其他操作）
    int draggingAnnotation;                // 正在拖拽的非文字标注索引（-1=无）
    int resizingAnnotation;                // 正在缩放的非文字标注索引（-1=无）
    int annotationResizeHandle;            // 当前缩放手柄（RH_None=无；CS_Resizing 时为四角之一）
    int annotationDragStartX, annotationDragStartY;  // 鼠标按下位置（绝对坐标，拖拽/缩放共用）
    Annotation dragStartAnnotation;                   // 按下时标注快照（拖拽时还原+平移）
    bool annotationOpHistoryPushed;
    RECT annotationResizeStartBox;                    // 按下时包围盒（缩放时基准）

    // ---- GDI+ 会话级资源（性能优化：会话内单次 Startup/Shutdown）----
    // 原实现每个绘制/测量函数各自 GdiplusStartup/Shutdown，每帧 WM_PAINT 触发 6~10 次昂贵的
    // GDI+ 初始化，是拖拽卡顿的主因。由于所有 GDI+ 调用均在 ScreenshotCaptureThread 单线程内，
    // 改为会话开始 Startup 一次、结束 Shutdown 一次。FontFamily(SC_FONT_FACE) 与
    // StringFormat(总是 Near/Near) 为常量；Font 仅依赖 fontPx（文字字号仅 SC_FONT_SIZES 三档），
    // 均缓存复用。Graphics 仍每次按 hdc 新建（必须，因为绑定不同 DC）。
    ULONG_PTR gdipToken;                  // GDI+ 启动令牌（0 = 未初始化）
    Gdiplus::GdiplusStartupInput gdipStartupInput;
    bool gdipInited;                      // GDI+ 是否已 Startup
    Gdiplus::FontFamily* gdipFontFamily;  // SC_FONT_FACE，会话内唯一
    Gdiplus::StringFormat* gdipStrFmt;    // Near/Near，会话内唯一
    Gdiplus::Font* gdipFonts[3];          // 按 SC_FONT_SIZES 预建的 Font（索引对齐 SC_FONT_COUNT）
};

// 截图上下文指针（窗口过程使用）
static CaptureContext* g_captureCtx = nullptr;

static void PushAnnotationHistory(CaptureContext* ctx) {
    ctx->undoStack.push_back(ctx->annotations);
    ctx->redoStack.clear();
}

static void ResetAnnotationInteraction(CaptureContext* ctx) {
    ctx->selectedTextAnnotation = -1;
    ctx->hoveredTextAnnotation = -1;
    ctx->draggingTextAnnotation = -1;
    ctx->selectedAnnotation = -1;
    ctx->hoveredAnnotation = -1;
    ctx->draggingAnnotation = -1;
    ctx->resizingAnnotation = -1;
    ctx->annotationResizeHandle = RH_None;
    ctx->annotationOpHistoryPushed = false;
    ctx->hasLastAnnotationBox = false;
}

static bool UndoAnnotations(CaptureContext* ctx) {
    if (ctx->undoStack.empty()) return false;
    ctx->redoStack.push_back(ctx->annotations);
    ctx->annotations = ctx->undoStack.back();
    ctx->undoStack.pop_back();
    ResetAnnotationInteraction(ctx);
    return true;
}

static bool RedoAnnotations(CaptureContext* ctx) {
    if (ctx->redoStack.empty()) return false;
    ctx->undoStack.push_back(ctx->annotations);
    ctx->annotations = ctx->redoStack.back();
    ctx->redoStack.pop_back();
    ResetAnnotationInteraction(ctx);
    return true;
}

// ==================== GDI+ 会话级资源管理（性能优化） ====================
// 所有 GDI+ 调用均在 ScreenshotCaptureThread 单线程内，故可在会话开始 Startup 一次、
// 结束 Shutdown 一次，避免每个绘制/测量函数反复 Startup/Shutdown（每帧 WM_PAINT 触发 6~10 次）。

// 前置声明：InitGdipResources 在分配失败时复用 Shutdown 清理。
static void ShutdownGdipResources(CaptureContext* ctx);

// 初始化会话级 GDI+ 资源：Startup + 预建可复用的 FontFamily/StringFormat/Font。
// 必须在任何 GDI+ 调用（含 InitMosaicBrushCursors）之前调用。
// 成功返回 true；失败（Startup 失败）返回 false，调用方应中止会话。
static bool InitGdipResources(CaptureContext* ctx) {
    ctx->gdipToken = 0;
    ctx->gdipInited = false;
    ctx->gdipFontFamily = nullptr;
    ctx->gdipStrFmt = nullptr;
    for (int i = 0; i < SC_FONT_COUNT; i++) ctx->gdipFonts[i] = nullptr;

    if (Gdiplus::GdiplusStartup(&ctx->gdipToken, &ctx->gdipStartupInput, NULL) != Gdiplus::Ok) {
        return false;
    }
    ctx->gdipInited = true;

    // 注意：GDI+ 类继承自 GdiplusBase，其 operator new 不接受 std::nothrow 参数，
    // 故用普通 new（GDI+ 对象通过 GetLastStatus() 而非空指针报告失败）。
    ctx->gdipFontFamily = new Gdiplus::FontFamily(SC_FONT_FACE);
    ctx->gdipStrFmt = new Gdiplus::StringFormat();
    if (!ctx->gdipFontFamily || !ctx->gdipStrFmt
        || ctx->gdipFontFamily->GetLastStatus() != Gdiplus::Ok) {
        ShutdownGdipResources(ctx); return false;
    }
    ctx->gdipStrFmt->SetAlignment(Gdiplus::StringAlignmentNear);
    ctx->gdipStrFmt->SetLineAlignment(Gdiplus::StringAlignmentNear);

    // 按文字字号预设预建 Font（与 SC_FONT_SIZES 一一对应）
    for (int i = 0; i < SC_FONT_COUNT; i++) {
        ctx->gdipFonts[i] = new Gdiplus::Font(
            ctx->gdipFontFamily, (Gdiplus::REAL)SC_FONT_SIZES[i],
            Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
        if (!ctx->gdipFonts[i] || ctx->gdipFonts[i]->GetLastStatus() != Gdiplus::Ok) {
            ShutdownGdipResources(ctx); return false;
        }
    }
    return true;
}

// 释放会话级 GDI+ 资源：先 delete 所有 GDI+ 对象，再 Shutdown（对象须在 Shutdown 前析构）。
// 安全可重入：重复调用无副作用。
static void ShutdownGdipResources(CaptureContext* ctx) {
    // Font 先于 FontFamily 析构（Font 内部引用 FontFamily）
    for (int i = 0; i < SC_FONT_COUNT; i++) {
        delete ctx->gdipFonts[i];
        ctx->gdipFonts[i] = nullptr;
    }
    delete ctx->gdipStrFmt;
    ctx->gdipStrFmt = nullptr;
    delete ctx->gdipFontFamily;
    ctx->gdipFontFamily = nullptr;

    if (ctx->gdipInited) {
        Gdiplus::GdiplusShutdown(ctx->gdipToken);
        ctx->gdipInited = false;
        ctx->gdipToken = 0;
    }
}

// 取 fontPx 对应的缓存 Font。文字标注字号仅取自 SC_FONT_SIZES，故优先查缓存表；
// 若 fontPx 不在预设表内（理论上不会发生），用 thread_local 临时对象兜底，避免泄漏。
// 返回的 Font 所有权归会话/兜底存储，调用方不得 delete。
static Gdiplus::Font* GetGdipFont(CaptureContext* ctx, int fontPx) {
    for (int i = 0; i < SC_FONT_COUNT; i++) {
        if (SC_FONT_SIZES[i] == fontPx && ctx->gdipFonts[i]) {
            return ctx->gdipFonts[i];
        }
    }
    // 兜底：thread_local 临时 Font（仅当前线程有效，覆盖极少见的非预设字号）
    static thread_local Gdiplus::Font* sFallback = nullptr;
    static thread_local int sFallbackPx = -1;
    static thread_local Gdiplus::FontFamily* sFallbackFam = nullptr;
    if (!sFallbackFam) sFallbackFam = new Gdiplus::FontFamily(SC_FONT_FACE);
    if (!sFallback || sFallbackPx != fontPx) {
        delete sFallback;
        sFallbackPx = fontPx;
        sFallback = new Gdiplus::Font(sFallbackFam, (Gdiplus::REAL)fontPx,
                                      Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
    }
    return sFallback;
}

// Base64 编码表
static const char base64_chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

// Base64 编码
static std::string Base64Encode(const BYTE* data, size_t len) {
    std::string result;
    result.reserve(((len + 2) / 3) * 4);
    for (size_t i = 0; i < len; i += 3) {
        unsigned int b = (data[i] << 16) | ((i + 1 < len ? data[i + 1] : 0) << 8) | (i + 2 < len ? data[i + 2] : 0);
        result.push_back(base64_chars[(b >> 18) & 0x3F]);
        result.push_back(base64_chars[(b >> 12) & 0x3F]);
        result.push_back(i + 1 < len ? base64_chars[(b >> 6) & 0x3F] : '=');
        result.push_back(i + 2 < len ? base64_chars[b & 0x3F] : '=');
    }
    return result;
}

// ---- 工具函数 ----

// 获取 DPI 缩放因子
static double GetDpiScaleFactor() {
    typedef UINT (WINAPI *GetDpiForSystemProc)();
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32) {
        auto proc = (GetDpiForSystemProc)GetProcAddress(user32, "GetDpiForSystem");
        if (proc) {
            UINT dpi = proc();
            double scale = dpi / 96.0;
            if (scale < 0.5) scale = 0.5;
            if (scale > 4.0) scale = 4.0;
            return scale;
        }
    }
    return 1.0;
}

// 显示器枚举回调数据
struct MonitorEnumData {
    LONG minLeft, minTop, maxRight, maxBottom;
    double totalDpiScale;
    int monitorCount;
};

// 显示器枚举回调
static BOOL CALLBACK MonitorEnumProc(HMONITOR hMonitor, HDC hdcMonitor, LPRECT lprcMonitor, LPARAM dwData) {
    MonitorEnumData* data = reinterpret_cast<MonitorEnumData*>(dwData);

    // 获取显示器的物理尺寸
    MONITORINFOEXW mi;
    mi.cbSize = sizeof(MONITORINFOEXW);
    if (GetMonitorInfoW(hMonitor, &mi)) {
        // 在 DPI 感知模式下，rcMonitor 已经是物理像素坐标
        data->minLeft = (std::min)(data->minLeft, mi.rcMonitor.left);
        data->minTop = (std::min)(data->minTop, mi.rcMonitor.top);
        data->maxRight = (std::max)(data->maxRight, mi.rcMonitor.right);
        data->maxBottom = (std::max)(data->maxBottom, mi.rcMonitor.bottom);
        data->monitorCount++;

        // 获取显示器 DPI
        typedef HRESULT(WINAPI* GetDpiForMonitorProc)(HMONITOR, int, UINT*, UINT*);
        HMODULE shcore = LoadLibraryW(L"shcore.dll");
        if (shcore) {
            auto getDpiForMonitor = (GetDpiForMonitorProc)GetProcAddress(shcore, "GetDpiForMonitor");
            if (getDpiForMonitor) {
                UINT dpiX, dpiY;
                if (SUCCEEDED(getDpiForMonitor(hMonitor, 0/*MDT_EFFECTIVE_DPI*/, &dpiX, &dpiY))) {
                    data->totalDpiScale = (std::max)(data->totalDpiScale, dpiX / 96.0);
                }
            }
            FreeLibrary(shcore);
        }
    }
    return TRUE;
}

// 截取整个虚拟屏幕到物理尺寸位图
static bool CaptureVirtualScreen(HDC& outMemDC, HBITMAP& outBitmap,
    int& vx, int& vy, int& vw, int& vh, double& dpiScale) {
    // 获取逻辑坐标的虚拟屏幕尺寸
    vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);

    // 枚举所有显示器获取物理像素边界
    MonitorEnumData enumData = { INT_MAX, INT_MAX, INT_MIN, INT_MIN, 1.0, 0 };
    EnumDisplayMonitors(NULL, NULL, MonitorEnumProc, reinterpret_cast<LPARAM>(&enumData));

    // 计算物理尺寸（使用枚举得到的实际物理像素边界）
    int physVx = enumData.minLeft;
    int physVy = enumData.minTop;
    int physVw = enumData.maxRight - enumData.minLeft;
    int physVh = enumData.maxBottom - enumData.minTop;

    // 如果枚举失败，回退到 DPI 缩放计算
    if (physVw <= 0 || physVh <= 0 || enumData.monitorCount == 0) {
        physVx = (int)(vx * dpiScale);
        physVy = (int)(vy * dpiScale);
        physVw = (int)(vw * dpiScale + 0.5);
        physVh = (int)(vh * dpiScale + 0.5);
    }

    HDC screenDC = GetDC(NULL);
    if (!screenDC) return false;

    outMemDC = CreateCompatibleDC(screenDC);
    if (!outMemDC) { ReleaseDC(NULL, screenDC); return false; }

    outBitmap = CreateCompatibleBitmap(screenDC, physVw, physVh);
    if (!outBitmap) { DeleteDC(outMemDC); ReleaseDC(NULL, screenDC); return false; }

    SelectObject(outMemDC, outBitmap);

    // 直接 BitBlt 物理像素（在 DPI 感知模式下，屏幕 DC 和坐标都是物理像素级别）
    BitBlt(outMemDC, 0, 0, physVw, physVh, screenDC, physVx, physVy, SRCCOPY | CAPTUREBLT);

    // 更新返回的 dpiScale 为实际的物理/逻辑比例
    // 这样后续的坐标转换才能正确
    if (vw > 0 && vh > 0) {
        dpiScale = (double)physVw / vw;
    }

    ReleaseDC(NULL, screenDC);
    return true;
}

// 创建双缓冲
static bool CreateBackBuffer(HDC& outDC, HBITMAP& outBmp, int w, int h) {
    HDC screenDC = GetDC(NULL);
    if (!screenDC) return false;
    outDC = CreateCompatibleDC(screenDC);
    if (!outDC) { ReleaseDC(NULL, screenDC); return false; }
    outBmp = CreateCompatibleBitmap(screenDC, w, h);
    if (!outBmp) { DeleteDC(outDC); ReleaseDC(NULL, screenDC); return false; }
    SelectObject(outDC, outBmp);
    ReleaseDC(NULL, screenDC);
    return true;
}

// 立即抓取当前虚拟屏幕首帧，供后续截图流程复用。
bool PrimeScreenshotFrameNow() {
    HDC memDC = NULL;
    HBITMAP bitmap = NULL;
    int vx = 0, vy = 0, vw = 0, vh = 0;
    double dpiScale = 1.0;
    if (!CaptureVirtualScreen(memDC, bitmap, vx, vy, vw, vh, dpiScale)) {
        return false;
    }

    if (memDC) {
        DeleteDC(memDC);
    }

    std::lock_guard<std::mutex> lock(g_primedScreenshotFrameMutex);
    ReleasePrimedScreenshotFrameLocked();
    g_primedScreenshotFrame.bitmap = bitmap;
    g_primedScreenshotFrame.vx = vx;
    g_primedScreenshotFrame.vy = vy;
    g_primedScreenshotFrame.vw = vw;
    g_primedScreenshotFrame.vh = vh;
    g_primedScreenshotFrame.dpiScale = dpiScale;
    g_primedScreenshotFrame.capturedAt = std::chrono::steady_clock::now();
    g_primedScreenshotFrame.valid = true;
    return true;
}

static bool ConsumePrimedScreenshotFrame(HDC& outMemDC, HBITMAP& outBitmap,
    int& vx, int& vy, int& vw, int& vh, double& dpiScale) {
    std::lock_guard<std::mutex> lock(g_primedScreenshotFrameMutex);
    if (!g_primedScreenshotFrame.valid || !g_primedScreenshotFrame.bitmap) {
        return false;
    }

    const auto now = std::chrono::steady_clock::now();
    if (now - g_primedScreenshotFrame.capturedAt > SC_PRIMED_FRAME_TTL) {
        ReleasePrimedScreenshotFrameLocked();
        return false;
    }

    HDC screenDC = GetDC(NULL);
    if (!screenDC) {
        ReleasePrimedScreenshotFrameLocked();
        return false;
    }

    outMemDC = CreateCompatibleDC(screenDC);
    ReleaseDC(NULL, screenDC);
    if (!outMemDC) {
        ReleasePrimedScreenshotFrameLocked();
        return false;
    }

    SelectObject(outMemDC, g_primedScreenshotFrame.bitmap);
    outBitmap = g_primedScreenshotFrame.bitmap;
    vx = g_primedScreenshotFrame.vx;
    vy = g_primedScreenshotFrame.vy;
    vw = g_primedScreenshotFrame.vw;
    vh = g_primedScreenshotFrame.vh;
    dpiScale = g_primedScreenshotFrame.dpiScale;
    g_primedScreenshotFrame.bitmap = NULL;
    g_primedScreenshotFrame.valid = false;
    return true;
}

static bool AcquireScreenshotBase(HDC& outMemDC, HBITMAP& outBitmap,
    int& vx, int& vy, int& vw, int& vh, double& dpiScale) {
    if (ConsumePrimedScreenshotFrame(outMemDC, outBitmap, vx, vy, vw, vh, dpiScale)) {
        return true;
    }
    return CaptureVirtualScreen(outMemDC, outBitmap, vx, vy, vw, vh, dpiScale);
}

// 从预截屏位图读取像素颜色（逻辑坐标）
static COLORREF GetPixelColorFromBitmap(HDC memDC, int x, int y, int vx, int vy, double dpiScale) {
    int lx = x - vx;
    int ly = y - vy;
    int px = (int)(lx * dpiScale + 0.5);
    int py = (int)(ly * dpiScale + 0.5);
    return GetPixel(memDC, px, py);
}

// COLORREF 转 HEX/RGB 字符串
static void ColorrefToStrings(COLORREF color, char* hexBuf, char* rgbBuf) {
    int r = color & 0xFF;
    int g = (color >> 8) & 0xFF;
    int b = (color >> 16) & 0xFF;
    sprintf_s(hexBuf, 32, "#%02X%02X%02X", r, g, b);
    sprintf_s(rgbBuf, 32, "%d, %d, %d", r, g, b);
}

// 枚举窗口回调
static BOOL CALLBACK SCEnumWindowsProc(HWND hwnd, LPARAM lParam) {
    auto* windows = reinterpret_cast<std::vector<SCWindowInfo>*>(lParam);

    if (!IsWindowVisible(hwnd)) return TRUE;

    LONG_PTR exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if (exStyle & WS_EX_TOOLWINDOW) return TRUE;

    LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    if (style == 0) return TRUE;

    // 检查是否为幽灵窗口（cloaked window）
    // 幽灵窗口虽然 IsWindowVisible 返回 true，但实际上不可见
    BOOL isCloaked = FALSE;
    HRESULT hrCloaked = DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &isCloaked, sizeof(isCloaked));
    if (SUCCEEDED(hrCloaked) && isCloaked) {
        return TRUE; // 跳过幽灵窗口
    }

    // 获取窗口类名以进行额外过滤
    const int MAX_CLASS_NAME = 256;
    WCHAR className[MAX_CLASS_NAME] = {0};
    int classNameLen = GetClassNameW(hwnd, className, MAX_CLASS_NAME);

    // 过滤某些特殊的系统窗口类
    if (classNameLen > 0) {
        // Windows 输入法相关窗口（如 Microsoft Text Input Application）
        if (wcscmp(className, L"Windows.UI.Core.CoreWindow") == 0) {
            // 对于 CoreWindow，再次确认是否真的可见（通过检查是否有有效的可视化区域）
            RECT clientRect;
            if (!GetClientRect(hwnd, &clientRect)) return TRUE;

            // 如果客户区太小，很可能是输入法等后台窗口
            int clientW = clientRect.right - clientRect.left;
            int clientH = clientRect.bottom - clientRect.top;
            if (clientW < 100 || clientH < 100) return TRUE;
        }

        // 过滤 ApplicationFrameWindow 的空壳窗口
        // UWP 应用在未激活时可能留下空的 ApplicationFrameWindow
        if (wcscmp(className, L"ApplicationFrameWindow") == 0) {
            // 检查窗口是否被最小化或隐藏
            if (IsIconic(hwnd)) return TRUE;

            // 检查是否真的有内容（通过检查窗口透明度或其他属性）
            BYTE opacity = 255;
            DWORD cloakedReason = 0;
            DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloakedReason, sizeof(cloakedReason));
            if (cloakedReason != 0) return TRUE;
        }
    }

    int titleLen = GetWindowTextLengthW(hwnd);
    if (titleLen == 0) return TRUE;

    std::wstring title(titleLen + 1, L'\0');
    GetWindowTextW(hwnd, &title[0], titleLen + 1);
    title.resize(titleLen);

    if (hwnd == GetDesktopWindow()) return TRUE;

    // 使用 DWM 获取精确边界
    RECT rect = {};
    HRESULT hr = DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &rect, sizeof(rect));
    if (FAILED(hr)) {
        if (!GetWindowRect(hwnd, &rect)) return TRUE;
    }

    int w = rect.right - rect.left;
    int h = rect.bottom - rect.top;
    if (w < 50 || h < 50) return TRUE;

    SCWindowInfo info;
    info.hwnd = hwnd;
    info.rect = rect;
    info.title = title;
    windows->push_back(info);
    return TRUE;
}

// 枚举窗口
static std::vector<SCWindowInfo> EnumWindowsForCapture() {
    std::vector<SCWindowInfo> windows;
    EnumWindows(SCEnumWindowsProc, reinterpret_cast<LPARAM>(&windows));
    return windows;
}

// 查找鼠标下方的窗口
static int FindWindowAtPoint(const std::vector<SCWindowInfo>& windows, int x, int y) {
    for (size_t i = 0; i < windows.size(); i++) {
        const RECT& r = windows[i].rect;
        if (x >= r.left && x < r.right && y >= r.top && y < r.bottom)
            return (int)i;
    }
    return -1;
}

// 计算浮窗位置（优先右下，超出则翻转）
static void CalcPanelPosition(int mx, int my, int vx, int vy, int vw, int vh,
                              const SCPanelMetrics& m, int& px, int& py) {
    int sr = vx + vw;
    int sb = vy + vh;
    px = mx + m.margin;
    py = my + m.margin;
    if (px + m.w > sr) px = mx - m.w - m.margin;
    if (py + m.h > sb) py = my - m.h - m.margin;
    if (px < vx) px = vx + m.margin;
    if (py < vy) py = vy + m.margin;
}

// 从预截屏位图恢复脏区域到后台缓冲
static void RestoreDirtyRegion(HDC backDC, HDC memDC, const RECT& dirty, double dpiScale) {
    int w = dirty.right - dirty.left;
    int h = dirty.bottom - dirty.top;
    if (w <= 0 || h <= 0) return;
    int x = (std::max)((int)dirty.left, 0);
    int y = (std::max)((int)dirty.top, 0);
    w = dirty.right - x;
    h = dirty.bottom - y;
    if (dpiScale > 1.01 || dpiScale < 0.99) {
        int px = (int)(x * dpiScale + 0.5);
        int py = (int)(y * dpiScale + 0.5);
        int pw = (int)(w * dpiScale + 0.5);
        int ph = (int)(h * dpiScale + 0.5);
        StretchBlt(backDC, x, y, w, h, memDC, px, py, pw, ph, SRCCOPY);
    } else {
        BitBlt(backDC, x, y, w, h, memDC, x, y, SRCCOPY);
    }
}

// 扩展矩形
static RECT InflateRectBy(const RECT& r, int margin) {
    return { r.left - margin, r.top - margin, r.right + margin, r.bottom + margin };
}

// 矩形是否有效（宽高 > 0）
static bool IsValidRect(const RECT& r) {
    return r.right > r.left && r.bottom > r.top;
}

// 两矩形并集的外包矩形（安全处理零矩形：若一方无效则返回另一方）。
// 用于计算"旧位置 ∪ 新位置"的脏区域外包。
static RECT UnionRectSafe(const RECT& a, const RECT& b) {
    bool va = IsValidRect(a), vb = IsValidRect(b);
    if (!va && !vb) return {0, 0, 0, 0};
    if (!va) return b;
    if (!vb) return a;
    return { (std::min)(a.left, b.left), (std::min)(a.top, b.top),
             (std::max)(a.right, b.right), (std::max)(a.bottom, b.bottom) };
}

// ---- 绘制函数 ----

// 绘制放大镜 + 鼠标信息面板
static void DrawInfoPanel(HDC hdc, int panelX, int panelY, COLORREF color,
    HDC memDC, int vx, int vy, int mx, int my, double dpiScale,
    const SCGdiResources& gdi, const SCPanelMetrics& m) {
    HGDIOBJ oldBrush = SelectObject(hdc, gdi.bgBrush);
    HGDIOBJ oldPen = SelectObject(hdc, gdi.borderPen);

    // 圆角矩形背景
    RoundRect(hdc, panelX, panelY, panelX + m.w, panelY + m.h,
        m.radius, m.radius);

    // 放大镜：从物理尺寸位图取像素
    int srcW = m.w / SC_ZOOM_FACTOR;
    int srcH = m.magnifierH / SC_ZOOM_FACTOR;
    int mxLogical = mx - vx;
    int myLogical = my - vy;
    int mxPhysical = (int)(mxLogical * dpiScale + 0.5);
    int myPhysical = (int)(myLogical * dpiScale + 0.5);
    int srcWPhysical = (int)(srcW * dpiScale + 0.5);
    int srcHPhysical = (int)(srcH * dpiScale + 0.5);
    int srcXPhysical = mxPhysical - srcWPhysical / 2;
    int srcYPhysical = myPhysical - srcHPhysical / 2;

    int magX = panelX + m.borderPad;
    int magY = panelY + m.borderPad;
    int magW = m.w - m.borderPad * 2;
    int magH = m.magnifierH - m.borderPad;

    StretchBlt(hdc, magX, magY, magW, magH, memDC,
        (std::max)(srcXPhysical, 0), (std::max)(srcYPhysical, 0),
        srcWPhysical, srcHPhysical, SRCCOPY);

    // 十字准星
    SelectObject(hdc, gdi.crosshairPen);
    int cx = magX + magW / 2;
    int cy = magY + magH / 2;
    MoveToEx(hdc, magX, cy, NULL); LineTo(hdc, magX + magW, cy);
    MoveToEx(hdc, cx, magY, NULL); LineTo(hdc, cx, magY + magH);

    // 文字信息
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(255, 255, 255));
    HGDIOBJ oldFont = SelectObject(hdc, gdi.smallFont);

    char hexBuf[32], rgbBuf[32];
    ColorrefToStrings(color, hexBuf, rgbBuf);
    char posBuf[64];
    sprintf_s(posBuf, "%d, %d", mx, my);

    int labelX = panelX + m.labelPad;
    int valueRightX = panelX + m.w - m.labelPad;

    // 获取文字高度
    SIZE textSize;
    GetTextExtentPoint32W(hdc, L"测试", 2, &textSize);
    int lineH = textSize.cy;
    int infoY = panelY + m.h - m.labelPad - lineH * 3;

    // 辅助：右对齐绘制
    auto drawRightAligned = [&](const wchar_t* text, int len, int rx, int ry) {
        SIZE sz;
        GetTextExtentPoint32W(hdc, text, len, &sz);
        TextOutW(hdc, rx - sz.cx, ry, text, len);
    };

    // 坐标
    TextOutW(hdc, labelX, infoY, L"坐标", 2);
    std::wstring posW(posBuf, posBuf + strlen(posBuf));
    drawRightAligned(posW.c_str(), (int)posW.size(), valueRightX, infoY);

    // HEX
    TextOutW(hdc, labelX, infoY + lineH, L"HEX", 3);
    std::wstring hexW(hexBuf, hexBuf + strlen(hexBuf));
    drawRightAligned(hexW.c_str(), (int)hexW.size(), valueRightX, infoY + lineH);

    // RGB
    TextOutW(hdc, labelX, infoY + lineH * 2, L"RGB", 3);
    std::wstring rgbW(rgbBuf, rgbBuf + strlen(rgbBuf));
    drawRightAligned(rgbW.c_str(), (int)rgbW.size(), valueRightX, infoY + lineH * 2);

    SelectObject(hdc, oldFont);
    SelectObject(hdc, oldBrush);
    SelectObject(hdc, oldPen);
}

// 绘制尺寸标签，返回标签矩形
static RECT DrawSizeLabel(HDC hdc, int width, int height,
    int refLeft, int refTop, int refRight, int refBottom,
    int virtualW, int virtualH, const SCGdiResources& gdi, const SCPanelMetrics& m) {
    RECT empty = {0, 0, 0, 0};
    if (width < 0 || height < 0) return empty;

    wchar_t sizeBuf[64];
    swprintf_s(sizeBuf, L"%d × %d", width, height);
    int sizeLen = (int)wcslen(sizeBuf);

    HGDIOBJ oldFont = SelectObject(hdc, gdi.smallFont);
    SIZE textSize;
    GetTextExtentPoint32W(hdc, sizeBuf, sizeLen, &textSize);

    int labelW = textSize.cx + m.sizeLabelPadX * 2;
    int labelH = textSize.cy + m.sizeLabelPadY;

    int lx = refLeft;
    int ly = refTop - labelH - m.sizeLabelGap;
    if (ly < 0) {
        lx = refLeft + m.sizeLabelGap;
        ly = refTop + m.sizeLabelGap;
        if (lx + labelW > virtualW) lx = virtualW - labelW - m.sizeLabelGap;
        if (ly + labelH > virtualH) ly = virtualH - labelH - m.sizeLabelGap;
        if (lx + labelW > refRight) lx = refRight - labelW - m.sizeLabelGap;
        if (ly + labelH > refBottom) ly = refBottom - labelH - m.sizeLabelGap;
    }
    if (lx < 0) lx = 0;
    if (ly < 0) ly = 0;
    if (lx + labelW > virtualW) lx = virtualW - labelW;
    if (ly + labelH > virtualH) ly = virtualH - labelH;

    HGDIOBJ oldBrush = SelectObject(hdc, gdi.bgBrush);
    HGDIOBJ oldPen = SelectObject(hdc, gdi.borderPen);
    RoundRect(hdc, lx, ly, lx + labelW, ly + labelH, m.radius, m.radius);

    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(255, 255, 255));
    TextOutW(hdc, lx + m.sizeLabelPadX, ly + m.borderPad, sizeBuf, sizeLen);

    SelectObject(hdc, oldFont);
    SelectObject(hdc, oldBrush);
    SelectObject(hdc, oldPen);

    RECT result = { lx, ly, lx + labelW, ly + labelH };
    return result;
}

// 绘制选区矩形边框 + 尺寸标签
static RECT DrawSelection(HDC hdc, int x1, int y1, int x2, int y2,
    int vx, int vy, int vw, int vh, const SCGdiResources& gdi, const SCPanelMetrics& m) {
    int left = (std::min)(x1, x2) - vx;
    int top = (std::min)(y1, y2) - vy;
    int right = (std::max)(x1, x2) - vx;
    int bottom = (std::max)(y1, y2) - vy;

    HGDIOBJ oldPen = SelectObject(hdc, gdi.selectionPen);
    HGDIOBJ oldBrush = SelectObject(hdc, GetStockObject(NULL_BRUSH));
    Rectangle(hdc, left, top, right, bottom);

    int sizeW = right - left;
    int sizeH = bottom - top;
    RECT labelRect = DrawSizeLabel(hdc, sizeW, sizeH, left, top, right, bottom, vw, vh, gdi, m);

    SelectObject(hdc, oldPen);
    SelectObject(hdc, oldBrush);
    return labelRect;
}

// 绘制窗口高亮边框
static void DrawWindowHighlight(HDC hdc, const RECT& rect, int vx, int vy, const SCGdiResources& gdi) {
    int left = rect.left - vx;
    int top = rect.top - vy;
    int right = rect.right - vx;
    int bottom = rect.bottom - vy;

    HGDIOBJ oldPen = SelectObject(hdc, gdi.highlightPen);
    HGDIOBJ oldBrush = SelectObject(hdc, GetStockObject(NULL_BRUSH));
    Rectangle(hdc, left, top, right, bottom);
    SelectObject(hdc, oldPen);
    SelectObject(hdc, oldBrush);
}

// 绘制选区外遮罩（微信风格）
// 在 backDC 上对"选区外部"区域 AlphaBlend 一层半透明黑色，
// 选区内部不绘制，保持原始截图清晰。
// rect 为相对虚拟屏幕的逻辑坐标（已减去 virtualX/virtualY）。
static void DrawDimMask(HDC backDC, const SCGdiResources& gdi,
    int selLeft, int selTop, int selRight, int selBottom,
    int virtualW, int virtualH) {
    if (!gdi.maskDC || !gdi.maskBitmap) return;

    BLENDFUNCTION blend;
    blend.BlendOp = AC_SRC_OVER;
    blend.BlendFlags = 0;
    blend.SourceConstantAlpha = SC_MASK_ALPHA;
    blend.AlphaFormat = 0;  // 不使用 per-pixel alpha，仅用常量 alpha

    // 将虚拟屏幕按选区划分为四周四块，逐块 AlphaBlend 遮罩
    // 上：x ∈ [0, vw], y ∈ [0, selTop]
    if (selTop > 0) {
        AlphaBlend(backDC, 0, 0, virtualW, selTop,
            gdi.maskDC, 0, 0, virtualW, selTop, blend);
    }
    // 下：x ∈ [0, vw], y ∈ [selBottom, vh]
    if (selBottom < virtualH) {
        AlphaBlend(backDC, 0, selBottom, virtualW, virtualH - selBottom,
            gdi.maskDC, 0, selBottom, virtualW, virtualH - selBottom, blend);
    }
    // 左：x ∈ [0, selLeft], y ∈ [selTop, selBottom]
    if (selLeft > 0 && selBottom > selTop) {
        AlphaBlend(backDC, 0, selTop, selLeft, selBottom - selTop,
            gdi.maskDC, 0, selTop, selLeft, selBottom - selTop, blend);
    }
    // 右：x ∈ [selRight, vw], y ∈ [selTop, selBottom]
    if (selRight < virtualW && selBottom > selTop) {
        AlphaBlend(backDC, selRight, selTop, virtualW - selRight, selBottom - selTop,
            gdi.maskDC, selRight, selTop, virtualW - selRight, selBottom - selTop, blend);
    }
}

// ---- 确认态辅助函数 ----

// 规范化矩形（保证 left<right, top<bottom）
static RECT NormalizeRect(const RECT& r) {
    RECT n;
    n.left = (std::min)(r.left, r.right);
    n.right = (std::max)(r.left, r.right);
    n.top = (std::min)(r.top, r.bottom);
    n.bottom = (std::max)(r.top, r.bottom);
    return n;
}

// 点是否在矩形内
static bool PointInRect(int x, int y, const RECT& r) {
    return x >= r.left && x < r.right && y >= r.top && y < r.bottom;
}

// 命中测试调整手柄，返回 ResizeHandle（绝对坐标）
static int HitTestHandle(int x, int y, const RECT& sel) {
    int hs = SC_HANDLE_SIZE;
    int cx = (sel.left + sel.right) / 2;
    int cy = (sel.top + sel.bottom) / 2;
    // 8 个手柄的判定矩形（顺序与 ResizeHandle 一致）
    struct { int hx, hy; int handle; } tests[] = {
        { sel.left,  cy,        RH_Left },
        { sel.right, cy,        RH_Right },
        { cx,        sel.top,   RH_Top },
        { cx,        sel.bottom, RH_Bottom },
        { sel.left,  sel.top,   RH_TopLeft },
        { sel.right, sel.top,   RH_TopRight },
        { sel.left,  sel.bottom, RH_BottomLeft },
        { sel.right, sel.bottom, RH_BottomRight },
    };
    for (auto& t : tests) {
        RECT box = { t.hx - hs, t.hy - hs, t.hx + hs, t.hy + hs };
        if (PointInRect(x, y, box)) return t.handle;
    }
    return RH_None;
}

// 根据手柄返回对应的系统鼠标光标
static LPCWSTR HandleCursor(int handle) {
    switch (handle) {
        case RH_Left:
        case RH_Right:
            return (LPCWSTR)IDC_SIZEWE;
        case RH_Top:
        case RH_Bottom:
            return (LPCWSTR)IDC_SIZENS;
        case RH_TopLeft:
        case RH_BottomRight:
            return (LPCWSTR)IDC_SIZENWSE;
        case RH_TopRight:
        case RH_BottomLeft:
            return (LPCWSTR)IDC_SIZENESW;
        case RH_ArrowStart:
        case RH_ArrowEnd:
            // 箭头端点拖拽：固定四向箭头，与悬停态一致
            return (LPCWSTR)IDC_SIZEALL;
        default:
            return (LPCWSTR)IDC_ARROW;
    }
}

// 计算工具栏位置：优先选区下方，放不下则上方，再放不下则选区内底部
static void CalcToolbarPosition(const RECT& selRel, int virtualW, int virtualH,
                                const SCToolbarMetrics& m, RECT& out) {
    int tw = CalcToolbarWidth(m);
    int th = m.h;
    int margin = m.margin;

    // 默认水平居中于选区，下方
    int x = selRel.left + ((selRel.right - selRel.left) - tw) / 2;
    int y = selRel.bottom + margin;

    // 下方放不下 -> 上方
    if (y + th > virtualH) {
        y = selRel.top - margin - th;
    }
    // 上方也放不下（选区很高），放选区内底部
    if (y < 0) {
        y = selRel.bottom - margin - th;
        if (y < selRel.top) y = selRel.top + margin;
    }
    // 水平边界约束
    if (x + tw > virtualW) x = virtualW - tw - margin;
    if (x < 0) x = margin;

    out.left = x;
    out.top = y;
    out.right = x + tw;
    out.bottom = y + th;
}

// 命中测试工具栏按钮，返回 ToolButton 序号（相对虚拟屏幕坐标），-1 = none
static int HitTestToolbar(int x, int y, const RECT& toolbarRect, const SCToolbarMetrics& m) {
    if (!PointInRect(x, y, toolbarRect)) return -1;
    int idx = (x - toolbarRect.left - m.border - m.pad) / (m.btn + m.gap);
    if (idx < 0 || idx >= TB_Count) return -1;
    return idx;
}

// 绘制单个工具图标：从 SCIconCache 取预渲染位图，AlphaBlend 居中绘制。
// cx,cy 为按钮中心；iconSize 为缓存位图边长（物理像素）。
static void DrawToolbarIcon(HDC hdc, int cx, int cy, int btn, bool active,
    const SCIconCache& icons) {
    HBITMAP bmp = icons.Get(btn, active);
    if (!bmp) return;

    int sz = icons.iconSize;
    int x = cx - sz / 2;
    int y = cy - sz / 2;

    HDC srcDC = CreateCompatibleDC(hdc);
    if (!srcDC) return;
    HGDIOBJ oldBmp = SelectObject(srcDC, bmp);

    BLENDFUNCTION blend;
    blend.BlendOp = AC_SRC_OVER;
    blend.BlendFlags = 0;
    blend.SourceConstantAlpha = 255;
    blend.AlphaFormat = AC_SRC_ALPHA;  // 使用 per-pixel alpha（位图已预乘）
    AlphaBlend(hdc, x, y, sz, sz, srcDC, 0, 0, sz, sz, blend);

    SelectObject(srcDC, oldBmp);
    DeleteDC(srcDC);
}

// 绘制选区调整手柄（8 个），传入相对坐标矩形
static void DrawResizeHandles(HDC hdc, const RECT& selRel) {
    int hs = SC_HANDLE_SIZE;
    int half = hs / 2;
    int cx = (selRel.left + selRel.right) / 2;
    int cy = (selRel.top + selRel.bottom) / 2;
    int pts[][2] = {
        { selRel.left,  cy },
        { selRel.right, cy },
        { cx, selRel.top },
        { cx, selRel.bottom },
        { selRel.left,  selRel.top },
        { selRel.right, selRel.top },
        { selRel.left,  selRel.bottom },
        { selRel.right, selRel.bottom },
    };
    // GDI+ 抗锯齿绘制：方块边缘像素半透明过渡，消除 GDI Rectangle 的硬锯齿。
    // 颜色沿用原 GDI 资源：蓝色填充 RGB(0,136,255) + 白色 1px 描边 RGB(255,255,255)。
    Gdiplus::Graphics graphics(hdc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::SolidBrush fillBrush(Gdiplus::Color(255, 0, 136, 255));
    Gdiplus::Pen borderPen(Gdiplus::Color(255, 255, 255, 255), 1.0f);
    Gdiplus::GraphicsPath path;
    for (auto& p : pts) {
        // hs×hs 匹配原 GDI Rectangle(l,t,r,b) 的 [l,r-1]×[t,b-1] 像素范围（2*half=hs）。
        path.AddRectangle(Gdiplus::Rect(p[0] - half, p[1] - half, hs, hs));
    }
    graphics.FillPath(&fillBrush, &path);
    graphics.DrawPath(&borderPen, &path);
}

// 绘制确认态选区边框（细蓝框）
static void DrawConfirmedBorder(HDC hdc, const RECT& selRel, const SCGdiResources& gdi) {
    HGDIOBJ oldPen = SelectObject(hdc, gdi.selectionPen);
    HGDIOBJ oldBrush = SelectObject(hdc, GetStockObject(NULL_BRUSH));
    Rectangle(hdc, selRel.left, selRel.top, selRel.right, selRel.bottom);
    SelectObject(hdc, oldPen);
    SelectObject(hdc, oldBrush);
}

// 用 GDI+ 圆角矩形路径填充 outPath（抗锯齿绘制的基础）。x,y,w,h 为整数像素矩形，
// radius 为圆角半径（自动钳制为不超过短边一半，避免重叠畸变）。
// GDI+ 的 GraphicsPath 不可拷贝，故用 out 参数而非返回值。
static void AddRoundedRect(Gdiplus::GraphicsPath& outPath, int x, int y, int w, int h, int radius) {
    int r = (std::min)(radius, (std::min)(w, h) / 2);
    if (r < 1) r = 1;
    Gdiplus::Rect rect(x, y, w, h);
    outPath.AddArc(rect.X, rect.Y, r * 2, r * 2, 180, 90);
    outPath.AddArc(rect.GetRight() - r * 2, rect.Y, r * 2, r * 2, 270, 90);
    outPath.AddArc(rect.GetRight() - r * 2, rect.GetBottom() - r * 2, r * 2, r * 2, 0, 90);
    outPath.AddArc(rect.X, rect.GetBottom() - r * 2, r * 2, r * 2, 90, 90);
    outPath.CloseFigure();
}

// 绘制悬浮工具栏（白底圆角 + 按钮图标 + 分组分隔线）
// 几何与图标尺寸均按 metrics（DPI 缩放）计算；图标从 icons 缓存取。
static void DrawToolbar(HDC hdc, const RECT& toolbarRect,
    int hoverBtn, int activeTool, const SCGdiResources& gdi,
    const SCToolbarMetrics& m, const SCIconCache& icons) {
    // 按钮在工具栏高度内的垂直留白
    int btnPad = (m.h - m.btn) / 2;

    // ---- 第一遍：GDI+ 抗锯齿绘制工具栏圆角背景 + 按钮圆角高亮 ----
    // GDI 的 RoundRect/FillRect 不支持抗锯齿，圆角边缘有锯齿，故改用 GDI+。
    // GDI+ 已由会话级 InitGdipResources 启动，此处直接使用。
    {
        Gdiplus::Graphics graphics(hdc);
        graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);

        int tw = toolbarRect.right - toolbarRect.left;
        int th = toolbarRect.bottom - toolbarRect.top;

        // 白色圆角背景填充（圆角外保持透明，露出后方截图）
        Gdiplus::SolidBrush whiteBrush(Gdiplus::Color(255, 255, 255, 255));
        Gdiplus::GraphicsPath bgPath;
        AddRoundedRect(bgPath, toolbarRect.left, toolbarRect.top, tw, th, m.radius);
        graphics.FillPath(&whiteBrush, &bgPath);

        // 1px 浅灰边框
        Gdiplus::Pen borderPen(Gdiplus::Color(255, 210, 210, 210), (Gdiplus::REAL)m.border);
        graphics.DrawPath(&borderPen, &bgPath);

        // 各按钮圆角高亮（hover/active）
        // 按钮高亮圆角半径：约为按钮边长的 1/8，视觉柔和
        int hlRadius = m.btn / 8;
        int hlInset = 2;
        int hlSize = m.btn - hlInset * 2;
        for (int i = 0; i < TB_Count; i++) {
            if (i == TB_Separator1 || i == TB_Separator2) continue;
            bool hover = (i == hoverBtn);
            bool active = (i == activeTool);
            if (!hover && !active) continue;

            int bx = toolbarRect.left + m.border + m.pad + i * (m.btn + m.gap);
            int by = toolbarRect.top + btnPad;
            // 选中态：主题蓝 #3B8BF2 叠白底 ~15% 的预混合浅蓝（225,237,253）
            // hover 态：极浅蓝（235,243,255）
            Gdiplus::Color hlColor = active
                ? Gdiplus::Color(255, 225, 237, 253)
                : Gdiplus::Color(255, 235, 243, 255);
            Gdiplus::SolidBrush hlBrush(hlColor);
            Gdiplus::GraphicsPath hlPath;
            AddRoundedRect(hlPath, bx + hlInset, by + hlInset, hlSize, hlSize, hlRadius);
            graphics.FillPath(&hlBrush, &hlPath);
        }
    }

    // ---- 第二遍：GDI 绘制分隔线 + 图标（位图直接 AlphaBlend，无需 AA）----
    for (int i = 0; i < TB_Count; i++) {
        int bx = toolbarRect.left + m.border + m.pad + i * (m.btn + m.gap);
        int by = toolbarRect.top + btnPad;
        RECT btnRect = { bx, by, bx + m.btn, by + m.btn };

        bool isSep = (i == TB_Separator1 || i == TB_Separator2);
        if (isSep) {
            // 分隔线
            int sx = bx + m.btn / 2;
            int sepInset = m.btn / 8 + 2;
            HGDIOBJ op = SelectObject(hdc, gdi.toolbarSepPen);
            MoveToEx(hdc, sx, btnRect.top + sepInset, NULL);
            LineTo(hdc, sx, btnRect.bottom - sepInset);
            SelectObject(hdc, op);
            continue;
        }

        bool active = (i == activeTool);
        int cx = bx + m.btn / 2;
        int cy = by + m.btn / 2;
        DrawToolbarIcon(hdc, cx, cy, i, active, icons);
    }
}

// ==================== 标注绘制（GDI+ 抗锯齿） ====================

// 箭头头：在 (ex,ey) 方向（从 sx,sy 指来）画一个填充三角，大小随 thickness。
// ox/oy 为整体偏移（覆盖层渲染时加 curSelRect.left/top；合成时为 0）。
static void DrawArrowHead(Gdiplus::Graphics& graphics, Gdiplus::Brush& brush,
                          float sx, float sy, float ex, float ey,
                          float thickness, float ox, float oy) {
    float dx = ex - sx;
    float dy = ey - sy;
    float len = std::sqrt(dx * dx + dy * dy);
    if (len < 1.0f) return;
    // 单位向量（箭头朝向）
    float ux = dx / len;
    float uy = dy / len;
    // 箭头长度/半宽 ∝ thickness
    float headLen = thickness * 3.0f + 6.0f;
    float headHalfW = thickness * 1.8f + 4.0f;
    if (headLen > len) headLen = len * 0.6f;

    // 箭尖
    float tipX = ex + ox, tipY = ey + oy;
    // 底部中心（沿箭头方向后退 headLen）
    float baseX = ex - ux * headLen + ox;
    float baseY = ey - uy * headLen + oy;
    // 法线方向
    float nx = -uy, ny = ux;
    // 两翼
    float w1X = baseX + nx * headHalfW, w1Y = baseY + ny * headHalfW;
    float w2X = baseX - nx * headHalfW, w2Y = baseY - ny * headHalfW;

    Gdiplus::PointF pts[3] = {
        Gdiplus::PointF(tipX, tipY),
        Gdiplus::PointF(w1X, w1Y),
        Gdiplus::PointF(w2X, w2Y),
    };
    graphics.FillPolygon(&brush, pts, 3);
}

// 绘制单条标注（不含 clip）。
// 标注坐标为绝对虚拟屏幕坐标；ox/oy 为绘制偏移，把绝对坐标换算到目标 DC 的局部坐标：
//   - 覆盖层（backDC）：ox/oy = -virtualX/-virtualY（backDC 原点 = 虚拟屏幕左上角）。
//   - 合成（finalDC）：ox/oy = -rect.left/-rect.top（finalDC 原点 = 选区左上角）。
static void DrawOneAnnotation(Gdiplus::Graphics& graphics, const Annotation& a,
                              float ox, float oy) {
    Gdiplus::Color c(GetRValue(a.color), GetGValue(a.color), GetBValue(a.color));
    float thick = (float)a.thickness;
    if (thick < 1.0f) thick = 1.0f;
    Gdiplus::Pen pen(c, thick);
    pen.SetLineJoin(Gdiplus::LineJoinRound);
    pen.SetStartCap(Gdiplus::LineCapRound);
    pen.SetEndCap(Gdiplus::LineCapRound);
    Gdiplus::SolidBrush brush(c);

    switch (a.type) {
        case AT_Rect: {
            float x = a.x1 + ox;
            float y = a.y1 + oy;
            float w = (float)(a.x2 - a.x1);
            float h = (float)(a.y2 - a.y1);
            graphics.DrawRectangle(&pen, (std::min)(x, x + w), (std::min)(y, y + h),
                                   std::fabs(w), std::fabs(h));
            break;
        }
        case AT_Circle: {
            float x = a.x1 + ox;
            float y = a.y1 + oy;
            float w = (float)(a.x2 - a.x1);
            float h = (float)(a.y2 - a.y1);
            graphics.DrawEllipse(&pen, (std::min)(x, x + w), (std::min)(y, y + h),
                                 std::fabs(w), std::fabs(h));
            break;
        }
        case AT_Arrow: {
            float sx = a.x1 + ox;
            float sy = a.y1 + oy;
            float ex = a.x2 + ox;
            float ey = a.y2 + oy;
            float dx = ex - sx, dy = ey - sy;
            float len = std::sqrt(dx * dx + dy * dy);
            if (len < 1.0f) break;
            // 箭头几何：机翼状——底边内凹（向尖端方向凹入 notch），两翼后掠，
            // 从箭身末端张开，呈「细尾→锥杆→张开两翼→尖」的箭矢形态。
            float headLen = thick * 4.0f + 8.0f;
            float headHalfW = thick * 2.4f + 5.0f;
            float notch = headLen * 0.4f;        // 内凹深度：底边中点向尖端凹入
            if (headLen > len) headLen = len * 0.6f;
            float ux = dx / len, uy = dy / len;
            float nx = -uy, ny = ux;
            // 箭头底部中心（沿箭头方向后退 headLen）与内凹点
            float baseX = ex - ux * headLen;
            float baseY = ey - uy * headLen;
            float notchX = baseX + ux * notch;
            float notchY = baseY + uy * notch;
            // 箭身：起点细、终点粗的锥形。终点延伸至内凹点并略超出（overlap），
            // 由箭头覆盖重叠区，避免抗锯齿在拼接处留细缝；终点宽 < 两翼宽，
            // 使两翼从箭身末端明显张开、内凹缺口清晰可见。
            float startHalfW = (std::max)(thick * 0.5f, 0.75f);
            float endHalfW = headHalfW * 0.55f;
            float overlap = 1.5f;
            float bodyEndX = notchX + ux * overlap;
            float bodyEndY = notchY + uy * overlap;
            Gdiplus::PointF body[4] = {
                Gdiplus::PointF(sx + nx * startHalfW, sy + ny * startHalfW),
                Gdiplus::PointF(sx - nx * startHalfW, sy - ny * startHalfW),
                Gdiplus::PointF(bodyEndX - nx * endHalfW, bodyEndY - ny * endHalfW),
                Gdiplus::PointF(bodyEndX + nx * endHalfW, bodyEndY + ny * endHalfW),
            };
            graphics.FillPolygon(&brush, body, 4);
            // 机翼状箭头：尖 → 右翼 → 内凹点 → 左翼（底边内凹而非平直三角）
            Gdiplus::PointF head[4] = {
                Gdiplus::PointF(ex, ey),
                Gdiplus::PointF(baseX + nx * headHalfW, baseY + ny * headHalfW),
                Gdiplus::PointF(notchX, notchY),
                Gdiplus::PointF(baseX - nx * headHalfW, baseY - ny * headHalfW),
            };
            graphics.FillPolygon(&brush, head, 4);
            break;
        }
        case AT_Brush: {
            if (a.pts.size() < 2) break;
            std::vector<Gdiplus::PointF> pts;
            pts.reserve(a.pts.size());
            for (const POINT& p : a.pts) {
                pts.push_back(Gdiplus::PointF((float)(p.x) + ox, (float)(p.y) + oy));
            }
            graphics.DrawLines(&pen, pts.data(), (INT)pts.size());
            break;
        }
        case AT_Text: {
            if (a.text.empty()) break;
            // 字号 = thickness；用 GDI+ 字体绘制，顶部对齐到锚点 y1
            int fontPx = a.thickness;
            if (fontPx < 8) fontPx = 8;
            Gdiplus::FontFamily fontFamily(SC_FONT_FACE);
            Gdiplus::Font font(&fontFamily, (Gdiplus::REAL)fontPx, Gdiplus::FontStyleRegular,
                                Gdiplus::UnitPixel);
            Gdiplus::SolidBrush textBrush(c);
            Gdiplus::StringFormat sf;
            sf.SetAlignment(Gdiplus::StringAlignmentNear);
            sf.SetLineAlignment(Gdiplus::StringAlignmentNear);
            // 用 MeasureString 得到合适布局矩形（避免被裁）
            Gdiplus::RectF layoutRect((Gdiplus::REAL)(a.x1 + ox), (Gdiplus::REAL)(a.y1 + oy),
                                      10000.0f, (Gdiplus::REAL)(fontPx * 2));
            graphics.DrawString(a.text.c_str(), -1, &font, layoutRect, &sf, &textBrush);
            break;
        }
    }
}

// ==================== 马赛克渲染 ====================
// 马赛克原理：将原始屏幕位图（srcDC = memDC，物理像素）一次缩小到按 blockPx
// 计算的低分辨率位图，再以最近邻一次放大到目标 DC，避免逐块执行大量 GDI 调用。
// 目标 DC（targetDC）为逻辑像素（backDC / finalDC），源 DC（srcDC）为物理像素（memDC），
// 二者通过 dpiScale 换算：srcX = (absX - virtualX) * dpiScale。

// 对单个矩形区域批量生成马赛克并绘制到 targetDC。
// dstX0/dstY0/dstW/dstH 是目标逻辑像素矩形；srcAbsX0/srcAbsY0 是其在虚拟屏幕中的
// 绝对逻辑坐标。低分辨率尺寸按 blockPx 向上取整，右/下不足整块的边缘由最后一格覆盖。
// 返回 true 表示缩小和最近邻放大均成功；失败时不修改缓存有效性，调用方可跳过揭示。
static bool MosaicBlitRect(HDC targetDC, HDC srcDC,
                           int dstX0, int dstY0, int dstW, int dstH,
                           int srcAbsX0, int srcAbsY0,
                           int blockPx, int virtualX, int virtualY, double dpiScale) {
    if (!targetDC || !srcDC || dstW <= 0 || dstH <= 0 || blockPx < 1 || dpiScale <= 0) {
        return false;
    }

    // 每个低分辨率像素对应约一个 blockPx 逻辑像素块；向上取整覆盖非整块边缘。
    int reducedW = (dstW + blockPx - 1) / blockPx;
    int reducedH = (dstH + blockPx - 1) / blockPx;
    if (reducedW <= 0 || reducedH <= 0) return false;

    // 源位图是物理像素，目标与马赛克块大小是逻辑像素，统一在这里完成坐标换算。
    int srcX = (int)((srcAbsX0 - virtualX) * dpiScale + 0.5);
    int srcY = (int)((srcAbsY0 - virtualY) * dpiScale + 0.5);
    int srcW = (int)(dstW * dpiScale + 0.5);
    int srcH = (int)(dstH * dpiScale + 0.5);
    if (srcW < 1) srcW = 1;
    if (srcH < 1) srcH = 1;

    HDC screenDC = GetDC(NULL);
    if (!screenDC) return false;
    HDC reducedDC = CreateCompatibleDC(screenDC);
    HBITMAP reducedBmp = reducedDC
        ? CreateCompatibleBitmap(screenDC, reducedW, reducedH)
        : NULL;
    ReleaseDC(NULL, screenDC);
    if (!reducedDC || !reducedBmp) {
        if (reducedBmp) DeleteObject(reducedBmp);
        if (reducedDC) DeleteDC(reducedDC);
        return false;
    }

    HGDIOBJ oldReducedBmp = SelectObject(reducedDC, reducedBmp);
    if (!oldReducedBmp || oldReducedBmp == HGDI_ERROR) {
        DeleteObject(reducedBmp);
        DeleteDC(reducedDC);
        return false;
    }

    // 第一次 StretchBlt：以 HALFTONE 将整个源区域压缩到低分辨率马赛克采样图。
    int oldReducedMode = GetStretchBltMode(reducedDC);
    POINT oldBrushOrigin = {0, 0};
    bool reducedModeReady = oldReducedMode != 0
        && SetStretchBltMode(reducedDC, HALFTONE) != 0
        && SetBrushOrgEx(reducedDC, 0, 0, &oldBrushOrigin);
    bool reducedOk = reducedModeReady
        && StretchBlt(reducedDC, 0, 0, reducedW, reducedH,
                      srcDC, srcX, srcY, srcW, srcH, SRCCOPY);
    if (reducedModeReady) {
        SetBrushOrgEx(reducedDC, oldBrushOrigin.x, oldBrushOrigin.y, NULL);
    }
    if (oldReducedMode != 0) SetStretchBltMode(reducedDC, oldReducedMode);

    // 第二次 StretchBlt：最近邻放大，保持每个低分辨率采样点为纯色块。
    int oldTargetMode = GetStretchBltMode(targetDC);
    bool targetModeReady = reducedOk && oldTargetMode != 0
        && SetStretchBltMode(targetDC, COLORONCOLOR) != 0;
    bool expandedOk = targetModeReady
        && StretchBlt(targetDC, dstX0, dstY0, dstW, dstH,
                      reducedDC, 0, 0, reducedW, reducedH, SRCCOPY);
    if (oldTargetMode != 0) SetStretchBltMode(targetDC, oldTargetMode);

    SelectObject(reducedDC, oldReducedBmp);
    DeleteObject(reducedBmp);
    DeleteDC(reducedDC);
    return reducedOk && expandedOk;
}


// ==================== 马赛克渲染（reveal-mask 模型） ====================
// 核心：仅在存在已提交马赛克或正在绘制马赛克时，才把整张虚拟屏幕按当前块大小
// 生成 mosaicBase。马赛克标注只是「蒙版」——涂抹=路径圆形区域、框选=矩形区域——
// 揭示其背后的 mosaicBase。任意区域、任意顺序叠加都连续无缝；切换块大小时延迟到
// 下一次实际需要马赛克的绘制再重建，避免普通截图确认态承担无用的整屏预处理。

// 释放马赛克 base 资源
static void FreeMosaicBase(CaptureContext* ctx) {
    if (ctx->mosaicBaseDC) { DeleteDC(ctx->mosaicBaseDC); ctx->mosaicBaseDC = NULL; }
    if (ctx->mosaicBaseBitmap) { DeleteObject(ctx->mosaicBaseBitmap); ctx->mosaicBaseBitmap = NULL; }
    ctx->mosaicBaseW = 0;
    ctx->mosaicBaseH = 0;
    ctx->mosaicBaseBlockPx = 0;
}

// 用 GDI+ 把单色位图转为带透明通道的 32bpp HBITMAP（用于光标）。
static HBITMAP ColorBitmapFromBitmap(Gdiplus::Bitmap& bmp) {
    HBITMAP hBmp = NULL;
    Gdiplus::Color bg(0, 0, 0, 0);  // 透明背景
    bmp.GetHBITMAP(bg, &hBmp);
    return hBmp;
}

// 生成单个涂抹光标：半径圆（白底 + 深色描边 + 中心十字）。
// size 为光标位图边长（逻辑像素）；hotspot 在中心。
static HCURSOR CreateMosaicBrushCursor(int radius) {
    int pad = 3;
    int size = (radius + pad) * 2;
    if (size < 16) size = 16;

    HCURSOR result = NULL;
    // GDI+ 已由会话级 InitGdipResources 启动（在 InitMosaicBrushCursors 调用前完成）。
    {
        Gdiplus::Bitmap bmp(size, size, PixelFormat32bppARGB);
        {
            Gdiplus::Graphics g(&bmp);
            g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            int cx = size / 2;
            int cy = size / 2;
            // 外圈：白色描边底（保证暗背景可见）
            Gdiplus::Pen whitePen(Gdiplus::Color(255, 255, 255, 255), 3.0f);
            g.DrawEllipse(&whitePen, cx - radius, cy - radius, radius * 2, radius * 2);
            // 内圈：深色虚线描边
            Gdiplus::Pen darkPen(Gdiplus::Color(255, 30, 30, 30), 1.5f);
            darkPen.SetDashStyle(Gdiplus::DashStyleDash);
            g.DrawEllipse(&darkPen, cx - radius, cy - radius, radius * 2, radius * 2);
            // 中心十字（准星）
            Gdiplus::Pen crossPen(Gdiplus::Color(255, 30, 30, 30), 1.0f);
            int cl = (std::min)(6, radius);
            g.DrawLine(&crossPen, cx - cl, cy, cx + cl, cy);
            g.DrawLine(&crossPen, cx, cy - cl, cx, cy + cl);
        }
        HBITMAP hColor = ColorBitmapFromBitmap(bmp);

        // 掩码位图（全黑，使用彩色光标时掩码可忽略，但 CreateIcon 要求非空）
        HDC screenDC = GetDC(NULL);
        HDC maskDC = CreateCompatibleDC(screenDC);
        HBITMAP hMask = CreateCompatibleBitmap(screenDC, size, size);
        HGDIOBJ oldMask = SelectObject(maskDC, hMask);
        PatBlt(maskDC, 0, 0, size, size, BLACKNESS);
        SelectObject(maskDC, oldMask);
        DeleteDC(maskDC);
        ReleaseDC(NULL, screenDC);

        ICONINFO ii = {0};
        ii.fIcon = FALSE;
        ii.xHotspot = size / 2;
        ii.yHotspot = size / 2;
        ii.hbmMask = hMask;
        ii.hbmColor = hColor;
        result = CreateIconIndirect(&ii);
        DeleteObject(hMask);
        DeleteObject(hColor);
    }
    return result;
}

// 初始化涂抹光标缓存（按 DPI 缩放半径）。
static void InitMosaicBrushCursors(CaptureContext* ctx) {
    if (ctx->mosaicBrushCursorsInited) return;
    for (int i = 0; i < SC_MOSAIC_RADIUS_COUNT; i++) {
        ctx->mosaicBrushCursors[i] = CreateMosaicBrushCursor(SC_MOSAIC_RADIUS[i]);
    }
    ctx->mosaicBrushCursorsInited = true;
}

static void FreeMosaicBrushCursors(CaptureContext* ctx) {
    for (int i = 0; i < SC_MOSAIC_RADIUS_COUNT; i++) {
        if (ctx->mosaicBrushCursors[i]) { DestroyIcon(ctx->mosaicBrushCursors[i]); ctx->mosaicBrushCursors[i] = NULL; }
    }
    ctx->mosaicBrushCursorsInited = false;
}

// 生成会话级马赛克底图：将整个虚拟屏幕批量缩小后最近邻放大到离屏位图。
// 缓存坐标与 backDC 一致（原点 = 虚拟屏幕左上角），尺寸 = virtualW×virtualH，
// 与选区无关：选区移动/缩放不需要重建，仅在虚拟屏幕尺寸或块大小变化时重建。
// 先在临时 GDI 对象中完整生成，成功后再替换旧缓存；失败会保留“无有效缓存”状态。
static bool RebuildMosaicBase(CaptureContext* ctx) {
    int w = ctx->virtualW;
    int h = ctx->virtualH;
    if (w <= 0 || h <= 0) {
        FreeMosaicBase(ctx);
        return false;
    }

    int blockPx = SC_MOSAIC_SIZES[ctx->mosaicSizeIdx];
    if (blockPx < 2) blockPx = 2;

    HDC screenDC = GetDC(NULL);
    if (!screenDC) {
        FreeMosaicBase(ctx);
        return false;
    }
    HDC newDC = CreateCompatibleDC(screenDC);
    HBITMAP newBitmap = newDC ? CreateCompatibleBitmap(screenDC, w, h) : NULL;
    ReleaseDC(NULL, screenDC);
    if (!newDC || !newBitmap) {
        if (newBitmap) DeleteObject(newBitmap);
        if (newDC) DeleteDC(newDC);
        FreeMosaicBase(ctx);
        return false;
    }

    HGDIOBJ oldNewBitmap = SelectObject(newDC, newBitmap);
    if (!oldNewBitmap || oldNewBitmap == HGDI_ERROR) {
        DeleteObject(newBitmap);
        DeleteDC(newDC);
        FreeMosaicBase(ctx);
        return false;
    }

    // 整虚拟屏幕按 blockPx 马赛克化：base 原点 = 虚拟屏幕左上角，源绝对坐标 = virtualX/virtualY。
    bool generated = MosaicBlitRect(newDC, ctx->memDC, 0, 0, w, h,
                                    ctx->virtualX, ctx->virtualY, blockPx,
                                    ctx->virtualX, ctx->virtualY, ctx->dpiScale);
    if (!generated) {
        SelectObject(newDC, oldNewBitmap);
        DeleteObject(newBitmap);
        DeleteDC(newDC);
        FreeMosaicBase(ctx);
        return false;
    }

    FreeMosaicBase(ctx);
    ctx->mosaicBaseDC = newDC;
    ctx->mosaicBaseBitmap = newBitmap;
    ctx->mosaicBaseW = w;
    ctx->mosaicBaseH = h;
    ctx->mosaicBaseBlockPx = blockPx;
    return true;
}

// 判断现有马赛克缓存是否缺失，或尺寸/块大小已与当前会话不一致。
// 此函数只检查缓存键；调用方必须先通过 HasMosaicToRender 确认当前帧确实需要马赛克。
static bool MosaicBaseNeedsRebuild(const CaptureContext* ctx) {
    int blockPx = SC_MOSAIC_SIZES[ctx->mosaicSizeIdx];
    if (blockPx < 2) blockPx = 2;
    bool blockChanged = (blockPx != ctx->mosaicBaseBlockPx);
    bool sizeChanged = (ctx->mosaicBaseW != ctx->virtualW || ctx->mosaicBaseH != ctx->virtualH);
    return blockChanged || sizeChanged || !ctx->mosaicBaseDC;
}

static bool HasMosaicToRender(const std::vector<Annotation>& annotations, const Annotation* curDrawing) {
    if (curDrawing && curDrawing->type == AT_Mosaic) return true;
    for (const Annotation& a : annotations) {
        if (a.type == AT_Mosaic) return true;
    }
    return false;
}

// 把单条马赛克标注对应的蒙版构建为目标 DC 局部坐标下的 HRGN。
// ox/oy 将标注的绝对屏幕坐标转换到目标坐标系：覆盖层使用 -virtualX/-virtualY，
// 导出位图使用 -selection.left/-selection.top。
static HRGN BuildMosaicMaskRegion(const Annotation& a, float ox, float oy) {
    if (a.mosaicRect) {
        int absL = (std::min)(a.x1, a.x2);
        int absT = (std::min)(a.y1, a.y2);
        int absR = (std::max)(a.x1, a.x2);
        int absB = (std::max)(a.y1, a.y2);
        return CreateRectRgn((int)(absL + ox + 0.5f), (int)(absT + oy + 0.5f),
                             (int)(absR + ox + 0.5f), (int)(absB + oy + 0.5f));
    } else {
        // 涂抹：把整条路径变为连续的「胶囊」区域（保证快速移动时不留空隙）。
        // 做法：沿相邻点之间的线段以不超过 radius/2 的步长插值取点，每个点画一个圆并并入区域，
        // 相邻圆重叠从而形成无缝的粗笔触轨迹。
        int radius = a.brushRadius;
        if (radius < 1) radius = 1;
        HRGN rgn = CreateRectRgn(0, 0, 0, 0);
        if (a.pts.empty()) return rgn;
        // 步长：半径的一半，保证相邻圆重叠 ≥50%，无视觉缝隙
        double step = (std::max)(1.0, radius * 0.5);

        auto addCircle = [&](double cx, double cy) {
            int ix = (int)(cx + 0.5);
            int iy = (int)(cy + 0.5);
            HRGN circle = CreateEllipticRgn(ix - radius, iy - radius,
                                            ix + radius, iy + radius);
            CombineRgn(rgn, rgn, circle, RGN_OR);
            DeleteObject(circle);
        };

        // 第一个点
        addCircle(a.pts[0].x + ox, a.pts[0].y + oy);
        for (size_t i = 1; i < a.pts.size(); i++) {
            double x0 = a.pts[i - 1].x + ox;
            double y0 = a.pts[i - 1].y + oy;
            double x1 = a.pts[i].x + ox;
            double y1 = a.pts[i].y + oy;
            double dx = x1 - x0, dy = y1 - y0;
            double segLen = std::sqrt(dx * dx + dy * dy);
            if (segLen < 0.5) {
                addCircle(x1, y1);
                continue;
            }
            int n = (int)(segLen / step + 0.5);
            if (n < 1) n = 1;
            for (int k = 1; k <= n; k++) {
                double t = (double)k / n;
                addCircle(x0 + dx * t, y0 + dy * t);
            }
        }
        return rgn;
    }
}

// 揭示马赛克：把 mosaicBase 中由 masks（已提交标注）+ curDrawing（正在绘制）覆盖的区域
// BitBlt 到 targetDC。mosaicBase 与 targetDC 使用相同的目标坐标系：覆盖层以虚拟屏幕
// 左上角为原点，导出位图以截图选区左上角为原点。
// contentBounds 是目标 DC 中允许显示马赛克的内容矩形；最终蒙版会与其求交，确保笔刷半径
// 或历史标注不会越过截图选区。ox/oy 用于把标注绝对坐标换算到目标局部坐标。
static void RevealMosaicToTarget(HDC targetDC, HDC mosaicBase,
                                 const std::vector<Annotation>& annotations,
                                 const Annotation* curDrawing,
                                 const RECT& contentBounds,
                                 float ox, float oy) {
    if (!targetDC || !mosaicBase
        || contentBounds.right <= contentBounds.left
        || contentBounds.bottom <= contentBounds.top) {
        return;
    }

    // 合并所有马赛克标注的蒙版区域（目标局部坐标）
    HRGN mask = CreateRectRgn(0, 0, 0, 0);
    bool any = false;
    for (const Annotation& a : annotations) {
        if (a.type != AT_Mosaic) continue;
        HRGN r = BuildMosaicMaskRegion(a, ox, oy);
        CombineRgn(mask, mask, r, RGN_OR);
        DeleteObject(r);
        any = true;
    }
    if (curDrawing && curDrawing->type == AT_Mosaic) {
        HRGN r = BuildMosaicMaskRegion(*curDrawing, ox, oy);
        CombineRgn(mask, mask, r, RGN_OR);
        DeleteObject(r);
        any = true;
    }
    if (any) {
        // 先把马赛克蒙版裁到截图内容矩形，再与调用方已有的 dirtyRect 裁剪区求交。
        // contentBounds 采用半开区间；因此边框所在的矩形外沿不会被马赛克写入。
        HRGN contentRgn = CreateRectRgn(contentBounds.left, contentBounds.top,
                                        contentBounds.right, contentBounds.bottom);
        CombineRgn(mask, mask, contentRgn, RGN_AND);
        DeleteObject(contentRgn);

        // 用 SaveDC 保护调用方的裁剪区（P1 局部帧时为 dirtyRect），揭示结束后完整恢复。
        int saved = SaveDC(targetDC);
        // mask 与现有裁剪区（dirtyRect）求交，揭示只发生在 dirtyRect∩选区∩蒙版。
        ExtSelectClipRgn(targetDC, mask, RGN_AND);
        // base 与 targetDC 同坐标系，1:1 拷贝。
        BitBlt(targetDC, contentBounds.left, contentBounds.top,
               contentBounds.right - contentBounds.left,
               contentBounds.bottom - contentBounds.top,
               mosaicBase, contentBounds.left, contentBounds.top, SRCCOPY);
        if (saved) RestoreDC(targetDC, saved);
    }
    DeleteObject(mask);
}

// 覆盖层渲染矢量/文字标注（不含马赛克，马赛克由 reveal-mask 单独处理）。
// selRel：选区在 backDC 局部坐标的矩形；标注为绝对虚拟屏幕坐标，偏移 = -virtualX/-virtualY。
// 用 SetClip 限制到选区内部，避免画到遮罩区。
static void DrawAnnotations(HDC hdc, const RECT& selRel, int virtualX, int virtualY,
                            const std::vector<Annotation>& annotations,
                            const Annotation* curDrawing) {
    // GDI+ 已由会话级 InitGdipResources 启动，此处直接使用（Graphics 按 hdc 新建）。
    {
        Gdiplus::Graphics graphics(hdc);
        graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        // 限制绘制范围在选区内
        Gdiplus::Rect clipRect(selRel.left, selRel.top,
                               selRel.right - selRel.left,
                               selRel.bottom - selRel.top);
        // 限制绘制范围在选区内（与选区矩形求交；局部帧时还会与 backDC 裁剪区 dirtyRect 求交，
        // 因 Graphics(backDC) 继承 GDI 裁剪区，CombineModeIntersect 保证标注不超出 dirtyRect∩选区）。
        graphics.SetClip(clipRect, Gdiplus::CombineModeIntersect);

        float ox = (float)-virtualX;
        float oy = (float)-virtualY;

        for (const Annotation& a : annotations) {
            if (a.type == AT_Mosaic) continue;  // 马赛克单独渲染
            DrawOneAnnotation(graphics, a, ox, oy);
        }
        if (curDrawing && curDrawing->type != AT_Mosaic) {
            DrawOneAnnotation(graphics, *curDrawing, ox, oy);
        }
    }
}


// 合成标注进最终 PNG：finalDC 原点 = 选区左上角，故偏移 = -rect.left/-rect.top。
// srcDC = memDC（原始屏幕位图，物理像素），用于马赛克像素化取源。
// mosaicBlockPx：马赛克块大小（与编辑器当前全局块大小保持一致，保证导出与所见一致）。
static void CompositeAnnotations(HDC finalDC, HDC srcDC,
                                 const std::vector<Annotation>& annotations,
                                 const RECT& rect, int virtualX, int virtualY,
                                 double dpiScale, int mosaicBlockPx) {
    if (annotations.empty()) return;

    // 马赛克先渲染到底图上，后续矢量/文字标注保持清晰覆盖在其上方。
    if (HasMosaicToRender(annotations, nullptr)) {
        int blockPx = mosaicBlockPx;
        if (blockPx < 2) blockPx = 2;
        int w = rect.right - rect.left;
        int h = rect.bottom - rect.top;
        HDC screenDC = GetDC(NULL);
        if (screenDC) {
            HDC baseDC = CreateCompatibleDC(screenDC);
            HBITMAP baseBmp = baseDC ? CreateCompatibleBitmap(screenDC, w, h) : NULL;
            if (baseDC && baseBmp) {
                HGDIOBJ oldBase = SelectObject(baseDC, baseBmp);
                bool generated = oldBase && oldBase != HGDI_ERROR
                    && MosaicBlitRect(baseDC, srcDC, 0, 0, w, h,
                                      rect.left, rect.top, blockPx,
                                      virtualX, virtualY, dpiScale);
                if (generated) {
                    // 揭示蒙版区域（finalDC 原点 = 选区左上角，base 同为选区相对，ox=-rect.left）
                    float ox = (float)-rect.left;
                    float oy = (float)-rect.top;
                    RevealMosaicToTarget(finalDC, baseDC, annotations, nullptr,
                                         RECT{0, 0, w, h}, ox, oy);
                }
                if (oldBase && oldBase != HGDI_ERROR) SelectObject(baseDC, oldBase);
            }
            if (baseBmp) DeleteObject(baseBmp);
            if (baseDC) DeleteDC(baseDC);
            ReleaseDC(NULL, screenDC);
        }
    }

    // GDI+ 已由会话级 InitGdipResources 启动，此处直接使用。
    {
        Gdiplus::Graphics graphics(finalDC);
        graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        float ox = (float)-rect.left;
        float oy = (float)-rect.top;
        for (const Annotation& a : annotations) {
            if (a.type == AT_Mosaic) continue;  // 马赛克单独渲染
            DrawOneAnnotation(graphics, a, ox, oy);
        }
    }
}

// 将 HBITMAP 转换为 PNG base64 字符串
static std::string BitmapToBase64Png(HBITMAP hBitmap) {
    // GDI+ 已由会话级 InitGdipResources 启动，此处直接使用。
    std::string result;
    {
        Gdiplus::Bitmap* bmp = Gdiplus::Bitmap::FromHBITMAP(hBitmap, NULL);
        if (bmp) {
            CLSID pngClsid;
            if (GetPngEncoderClsid(&pngClsid) >= 0) {
                IStream* stream = NULL;
                CreateStreamOnHGlobal(NULL, TRUE, &stream);
                if (stream && bmp->Save(stream, &pngClsid, NULL) == Gdiplus::Ok) {
                    HGLOBAL hMem = NULL;
                    GetHGlobalFromStream(stream, &hMem);
                    size_t len = GlobalSize(hMem);
                    BYTE* ptr = (BYTE*)GlobalLock(hMem);
                    if (ptr && len > 0) {
                        result = "data:image/png;base64," + Base64Encode(ptr, len);
                    }
                    GlobalUnlock(hMem);
                }
                if (stream) stream->Release();
            }
            delete bmp;
        }
    }
    return result;
}

// 保存位图到剪贴板
static bool SaveBitmapToClipboard(HBITMAP hBitmap) {
    if (!OpenClipboard(NULL)) return false;
    EmptyClipboard();
    BITMAP bm;
    GetObject(hBitmap, sizeof(BITMAP), &bm);
    HBITMAP hCopy = (HBITMAP)CopyImage(hBitmap, IMAGE_BITMAP, bm.bmWidth, bm.bmHeight, LR_COPYRETURNORG);
    SetClipboardData(CF_BITMAP, hCopy);
    CloseClipboard();
    return true;
}

// 从预截屏位图提取区域，生成 base64 并复制到剪贴板。
// anns：可选的标注列表，会合成进最终 PNG（选区相对坐标，finalDC 原点 = 选区原点）。
static ScreenshotResult* ExtractRegionResult(HDC memDC, const RECT& rect,
    int vx, int vy, double dpiScale, const std::vector<Annotation>& anns) {
    ScreenshotResult* result = new ScreenshotResult();
    result->success = false;
    int width = rect.right - rect.left;
    int height = rect.bottom - rect.top;
    result->x = rect.left;
    result->y = rect.top;
    result->x2 = rect.right;
    result->y2 = rect.bottom;
    result->width = width;
    result->height = height;

    if (width <= 0 || height <= 0) return result;

    // 从物理尺寸位图提取区域
    int lx = rect.left - vx;
    int ly = rect.top - vy;
    int px = (int)(lx * dpiScale + 0.5);
    int py = (int)(ly * dpiScale + 0.5);
    int pw = (int)(width * dpiScale + 0.5);
    int ph = (int)(height * dpiScale + 0.5);

    HDC screenDC = GetDC(NULL);
    HDC regionDC = CreateCompatibleDC(screenDC);
    HBITMAP regionBmp = CreateCompatibleBitmap(screenDC, pw, ph);
    SelectObject(regionDC, regionBmp);
    BitBlt(regionDC, 0, 0, pw, ph, memDC, px, py, SRCCOPY);

    // 如果有 DPI 缩放，缩放回逻辑尺寸
    HBITMAP finalBmp = regionBmp;
    HDC finalDC = regionDC;
    if (dpiScale > 1.01 || dpiScale < 0.99) {
        HDC scaledDC = CreateCompatibleDC(screenDC);
        HBITMAP scaledBmp = CreateCompatibleBitmap(screenDC, width, height);
        SelectObject(scaledDC, scaledBmp);
        SetStretchBltMode(scaledDC, HALFTONE);
        SetBrushOrgEx(scaledDC, 0, 0, NULL);
        StretchBlt(scaledDC, 0, 0, width, height, regionDC, 0, 0, pw, ph, SRCCOPY);
        DeleteDC(regionDC);
        DeleteObject(regionBmp);
        finalBmp = scaledBmp;
        finalDC = scaledDC;
    }

    // 合成标注进最终图像（finalDC 原点 = 选区原点，标注为绝对坐标，偏移 = -rect.left/top）
    CompositeAnnotations(finalDC, memDC, anns, rect, vx, vy, dpiScale,
                         SC_MOSAIC_SIZES[g_captureCtx ? g_captureCtx->mosaicSizeIdx : SC_DEFAULT_MOSAIC_IDX]);

    // 生成 base64
    result->base64 = BitmapToBase64Png(finalBmp);
    // 复制到剪贴板
    result->success = SaveBitmapToClipboard(finalBmp);

    DeleteDC(finalDC);
    DeleteObject(finalBmp);
    ReleaseDC(NULL, screenDC);

    return result;
}

// ---- 窗口过程和线程 ----

// 生成默认保存文件名：Screenshot_YYYYMMDD_HHMMSS.png
static std::wstring MakeDefaultScreenshotName() {
    time_t now = time(NULL);
    struct tm lt;
    localtime_s(&lt, &now);
    wchar_t buf[64];
    wsprintfW(buf, L"Screenshot_%04d%02d%02d_%02d%02d%02d.png",
              lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday,
              lt.tm_hour, lt.tm_min, lt.tm_sec);
    return std::wstring(buf);
}

// 弹出系统保存对话框，返回用户选择的文件完整路径（含 .png 后缀）；
// 用户取消或失败时返回空字符串。
// hwndOwner：父窗口句柄（截图覆盖层），用于模态居中。
// 注意：覆盖层是 WS_EX_TOPMOST 全屏窗口，通用对话框可能被遮挡。
//       弹出前临时移除其 TOPMOST（让对话框自然置顶），关闭后恢复，保证对话框可见可交互。
static std::wstring PromptSaveFilePath(HWND hwndOwner) {
    // 默认目录：图片库（FOLDERID_Pictures），获取失败则退化为桌面
    wchar_t* defaultDir = nullptr;
    HRESULT hr = SHGetKnownFolderPath(FOLDERID_Pictures, 0, NULL, &defaultDir);
    std::wstring initDir;
    if (SUCCEEDED(hr) && defaultDir) {
        initDir = defaultDir;
        CoTaskMemFree(defaultDir);
    } else {
        wchar_t* desktop = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, NULL, &desktop)) && desktop) {
            initDir = desktop;
            CoTaskMemFree(desktop);
        }
    }

    std::wstring defaultName = MakeDefaultScreenshotName();

    wchar_t fileBuf[MAX_PATH] = {0};
    wcsncpy_s(fileBuf, MAX_PATH, defaultName.c_str(), _TRUNCATE);

    // 临时取消覆盖层 TOPMOST，确保保存对话框显示在最上层
    if (hwndOwner) {
        SetWindowPos(hwndOwner, HWND_NOTOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }

    OPENFILENAMEW ofn = {0};
    ofn.lStructSize = sizeof(OPENFILENAMEW);
    ofn.hwndOwner = hwndOwner;
    ofn.lpstrFile = fileBuf;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrFilter = L"PNG 图像 (*.png)\0*.png\0";
    ofn.nFilterIndex = 1;
    ofn.lpstrDefExt = L"png";  // 用户未输扩展名时自动补 .png
    if (!initDir.empty()) {
        ofn.lpstrInitialDir = initDir.c_str();
    }
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY
                | OFN_NOCHANGEDIR;

    BOOL ok = GetSaveFileNameW(&ofn);

    // 恢复覆盖层 TOPMOST（用户取消保存时需要回到置顶全屏状态）
    if (hwndOwner) {
        SetWindowPos(hwndOwner, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }

    if (ok) {
        return std::wstring(fileBuf);
    }
    return std::wstring();
}

// 将已合成标注的选区 HBITMAP 保存为 PNG 文件。
// 返回 true 表示保存成功。
static bool SaveRegionToPngFile(HDC memDC, const RECT& rect, int vx, int vy,
                                double dpiScale, const std::vector<Annotation>& anns,
                                const std::wstring& filePath) {
    int width = rect.right - rect.left;
    int height = rect.bottom - rect.top;
    if (width <= 0 || height <= 0 || filePath.empty()) return false;

    // 复用 ExtractRegionResult 的合成逻辑生成 finalBmp（含标注、按逻辑尺寸）
    int lx = rect.left - vx;
    int ly = rect.top - vy;
    int px = (int)(lx * dpiScale + 0.5);
    int py = (int)(ly * dpiScale + 0.5);
    int pw = (int)(width * dpiScale + 0.5);
    int ph = (int)(height * dpiScale + 0.5);

    HDC screenDC = GetDC(NULL);
    HDC regionDC = CreateCompatibleDC(screenDC);
    HBITMAP regionBmp = CreateCompatibleBitmap(screenDC, pw, ph);
    SelectObject(regionDC, regionBmp);
    BitBlt(regionDC, 0, 0, pw, ph, memDC, px, py, SRCCOPY);

    HBITMAP finalBmp = regionBmp;
    HDC finalDC = regionDC;
    if (dpiScale > 1.01 || dpiScale < 0.99) {
        HDC scaledDC = CreateCompatibleDC(screenDC);
        HBITMAP scaledBmp = CreateCompatibleBitmap(screenDC, width, height);
        SelectObject(scaledDC, scaledBmp);
        SetStretchBltMode(scaledDC, HALFTONE);
        SetBrushOrgEx(scaledDC, 0, 0, NULL);
        StretchBlt(scaledDC, 0, 0, width, height, regionDC, 0, 0, pw, ph, SRCCOPY);
        DeleteDC(regionDC);
        DeleteObject(regionBmp);
        finalBmp = scaledBmp;
        finalDC = scaledDC;
    }
    CompositeAnnotations(finalDC, memDC, anns, rect, vx, vy, dpiScale,
                         SC_MOSAIC_SIZES[g_captureCtx ? g_captureCtx->mosaicSizeIdx : SC_DEFAULT_MOSAIC_IDX]);

    // 用 GDI+ 保存为 PNG 文件（GDI+ 已由会话级 InitGdipResources 启动）
    bool ok = false;
    {
        Gdiplus::Bitmap* bmp = Gdiplus::Bitmap::FromHBITMAP(finalBmp, NULL);
        if (bmp) {
            CLSID pngClsid;
            if (GetPngEncoderClsid(&pngClsid) >= 0) {
                ok = (bmp->Save(filePath.c_str(), &pngClsid, NULL) == Gdiplus::Ok);
            }
            delete bmp;
        }
    }

    DeleteDC(finalDC);
    DeleteObject(finalBmp);
    ReleaseDC(NULL, screenDC);
    return ok;
}

// 在主线程调用 JS 回调（截图完成）
static void CallScreenshotJs(napi_env env, napi_value js_callback, void* context, void* data) {
    if (env != nullptr && js_callback != nullptr && data != nullptr) {
        ScreenshotResult* result = static_cast<ScreenshotResult*>(data);

        napi_value resultObj;
        napi_create_object(env, &resultObj);

        napi_value success;
        napi_get_boolean(env, result->success, &success);
        napi_set_named_property(env, resultObj, "success", success);

        if (result->success) {
            napi_value x, y, x2, y2, width, height, base64;
            napi_create_int32(env, result->x, &x);
            napi_set_named_property(env, resultObj, "x", x);
            napi_create_int32(env, result->y, &y);
            napi_set_named_property(env, resultObj, "y", y);
            napi_create_int32(env, result->x2, &x2);
            napi_set_named_property(env, resultObj, "x2", x2);
            napi_create_int32(env, result->y2, &y2);
            napi_set_named_property(env, resultObj, "y2", y2);
            napi_create_int32(env, result->width, &width);
            napi_set_named_property(env, resultObj, "width", width);
            napi_create_int32(env, result->height, &height);
            napi_set_named_property(env, resultObj, "height", height);
            napi_create_string_utf8(env, result->base64.c_str(), result->base64.size(), &base64);
            napi_set_named_property(env, resultObj, "base64", base64);
        }

        napi_value global;
        napi_get_global(env, &global);
        napi_call_function(env, global, js_callback, 1, &resultObj, nullptr);
        delete result;
    }
}

// 进入确认态：规范化选区并切换状态
static void EnterConfirmed(CaptureContext* ctx, const RECT& sel) {
    RECT n = NormalizeRect(sel);
    // 约束到虚拟屏幕内
    if (n.left < ctx->virtualX) { n.right += ctx->virtualX - n.left; n.left = ctx->virtualX; }
    if (n.top < ctx->virtualY) { n.bottom += ctx->virtualY - n.top; n.top = ctx->virtualY; }
    if (n.right > ctx->virtualX + ctx->virtualW) n.right = ctx->virtualX + ctx->virtualW;
    if (n.bottom > ctx->virtualY + ctx->virtualH) n.bottom = ctx->virtualY + ctx->virtualH;
    // 最小尺寸保护
    if (n.right - n.left < SC_MIN_SELECTION) n.right = n.left + SC_MIN_SELECTION;
    if (n.bottom - n.top < SC_MIN_SELECTION) n.bottom = n.top + SC_MIN_SELECTION;
    ctx->selection = n;
    ctx->resizeHandle = RH_None;
    if (ctx->activeTool < 0) ctx->activeTool = TB_Drag;
    if (ctx->popupTool < 0) ctx->popupTool = -1;
    ctx->state = CS_Confirmed;
    ctx->needFullRedraw = true;
}

// 计算所有标注内容的包围盒（选区相对逻辑坐标）。
// 用于限制选区缩放：选区不可缩小到裁掉已添加内容。
// 返回 false 表示无标注（无约束）。
// 计算所有标注内容的包围盒（绝对虚拟屏幕坐标）。
// 用于限制选区缩放：选区不可缩小到裁掉已添加内容。
// 返回 false 表示无标注（无约束）。hdc 用于测量文字宽高。
// 前置声明：文字包围盒复用 MeasureTextAnnotation（定义在下方）。
// 参数为非常量引用：命中失败时会回填文字测量缓存（textCache* 字段）。
static RECT MeasureTextAnnotation(HDC hdc, Annotation& a);

static RECT MeasureArrowAnnotationBounds(const Annotation& a) {
    float sx = (float)a.x1;
    float sy = (float)a.y1;
    float ex = (float)a.x2;
    float ey = (float)a.y2;
    float minX = (std::min)(sx, ex);
    float minY = (std::min)(sy, ey);
    float maxX = (std::max)(sx, ex);
    float maxY = (std::max)(sy, ey);

    float thick = (float)a.thickness;
    if (thick < 1.0f) thick = 1.0f;
    float dx = ex - sx;
    float dy = ey - sy;
    float len = std::sqrt(dx * dx + dy * dy);
    if (len >= 1.0f) {
        float headLen = thick * 4.0f + 8.0f;
        float headHalfW = thick * 2.4f + 5.0f;
        if (headLen > len) headLen = len * 0.6f;
        float notch = headLen * 0.4f;
        float ux = dx / len;
        float uy = dy / len;
        float nx = -uy;
        float ny = ux;
        float baseX = ex - ux * headLen;
        float baseY = ey - uy * headLen;
        float notchX = baseX + ux * notch;
        float notchY = baseY + uy * notch;
        float startHalfW = (std::max)(thick * 0.5f, 0.75f);
        float endHalfW = headHalfW * 0.55f;
        float overlap = 1.5f;
        float bodyEndX = notchX + ux * overlap;
        float bodyEndY = notchY + uy * overlap;

        auto expand = [&](float x, float y) {
            if (x < minX) minX = x;
            if (y < minY) minY = y;
            if (x > maxX) maxX = x;
            if (y > maxY) maxY = y;
        };
        expand(sx + nx * startHalfW, sy + ny * startHalfW);
        expand(sx - nx * startHalfW, sy - ny * startHalfW);
        expand(bodyEndX - nx * endHalfW, bodyEndY - ny * endHalfW);
        expand(bodyEndX + nx * endHalfW, bodyEndY + ny * endHalfW);
        expand(baseX + nx * headHalfW, baseY + ny * headHalfW);
        expand(notchX, notchY);
        expand(baseX - nx * headHalfW, baseY - ny * headHalfW);
    }

    const float margin = 2.0f;
    return { (int)floorf(minX - margin), (int)floorf(minY - margin),
             (int)ceilf(maxX + margin), (int)ceilf(maxY + margin) };
}
// 非常量引用：AT_Text 分支会经 MeasureTextAnnotation 回填文字测量缓存。
static bool CalcAnnotationsBounds(std::vector<Annotation>& anns, RECT& out, HDC hdc) {
    if (anns.empty()) return false;
    int minL = INT_MAX, minT = INT_MAX, maxR = INT_MIN, maxB = INT_MIN;
    auto expand = [&](int x, int y) {
        if (x < minL) minL = x;
        if (y < minT) minT = y;
        if (x > maxR) maxR = x;
        if (y > maxB) maxB = y;
    };
    for (Annotation& a : anns) {
        if (a.type == AT_Brush) {
            for (const POINT& p : a.pts) expand(p.x, p.y);
        } else if (a.type == AT_Mosaic) {
            if (a.mosaicRect) {
                // 框选模式：矩形角点
                expand(a.x1, a.y1);
                expand(a.x2, a.y2);
            } else {
                // 涂抹模式：路径包围盒 + 半径
                int r = a.brushRadius;
                for (const POINT& p : a.pts) {
                    expand(p.x - r, p.y - r);
                    expand(p.x + r, p.y + r);
                }
            }
        } else if (a.type == AT_Text) {
            // 复用 MeasureTextAnnotation：与选中时的外边框（含 padding）完全一致，
            // 保证 resize 选区时文字包围盒约束 = 视觉选中边框，不会被裁掉。
            RECT r = MeasureTextAnnotation(hdc, a);
            expand(r.left, r.top);
            expand(r.right, r.bottom);
        } else if (a.type == AT_Arrow) {
            RECT r = MeasureArrowAnnotationBounds(a);
            expand(r.left, r.top);
            expand(r.right, r.bottom);
        } else {
            expand(a.x1, a.y1);
            expand(a.x2, a.y2);
        }
    }
    if (minL == INT_MAX) return false;
    out.left = minL;
    out.top = minT;
    out.right = maxR;
    out.bottom = maxB;
    return true;
}

// 约束单轴 resize 的活动端坐标，固定端始终保持按下时的位置。
// rawActive/originalActive 分别是当前候选值和按下时的活动端；screenMin/screenMax 是
// 虚拟屏幕在该轴上的绝对边界。已有标注时优先保证内容不被裁掉；松开时可按活动端
// 当前所在侧补足最小尺寸，整个过程只移动活动端，因此穿越后不会带动固定端漂移。
static int ConstrainResizeActiveCoordinate(int rawActive, int originalActive, int fixed,
                                           int screenMin, int screenMax,
                                           bool hasContent, int contentMin, int contentMax,
                                           bool enforceMinSize) {
    int active = (std::max)(screenMin, (std::min)(rawActive, screenMax));

    if (hasContent) {
        if (fixed <= contentMin) {
            // 内容位于固定端高侧：活动端必须覆盖内容高边，不能穿越后把内容留在选区外。
            active = (std::max)(active, contentMax);
        } else if (fixed >= contentMax) {
            // 内容位于固定端低侧：活动端必须覆盖内容低边。
            active = (std::min)(active, contentMin);
        } else {
            // 固定端落在内容内部时不存在可完整覆盖内容的单侧区间，保留按下时活动端。
            active = originalActive;
        }
        active = (std::max)(screenMin, (std::min)(active, screenMax));
    }

    if (enforceMinSize) {
        // 活动端恰好落在固定端时沿按下时的方向补足，避免释放后方向不确定。
        bool onLowSide = active < fixed || (active == fixed && originalActive < fixed);
        if (onLowSide) {
            active = (std::min)(active, fixed - SC_MIN_SELECTION);
        } else {
            active = (std::max)(active, fixed + SC_MIN_SELECTION);
        }
        // 固定端靠近虚拟屏幕边缘时，目标侧可能不足最小尺寸；此时边界优先且固定端不动。
        active = (std::max)(screenMin, (std::min)(active, screenMax));
    }

    return active;
}

// 从鼠标按下时的选区快照计算本帧 resize 结果（绝对虚拟屏幕坐标）。
// 每个活动轴都只更新对应手柄端，固定边/固定对角点始终取 startSelection；活动端可穿过
// 固定端，最后仅为绘制和导出调用一次 NormalizeRect。contentBounds 在 hasContent=true 时
// 限制活动端以保留已有标注；enforceMinSize 仅用于松开时沿最终方向补足最小尺寸。
static RECT ResizeSelectionFromHandle(const RECT& startSelection, int handle, int dx, int dy,
                                      const RECT& virtualBounds,
                                      bool hasContent, const RECT& contentBounds,
                                      bool enforceMinSize) {
    RECT resized = startSelection;

    bool movesLeft = handle == RH_Left || handle == RH_TopLeft || handle == RH_BottomLeft;
    bool movesRight = handle == RH_Right || handle == RH_TopRight || handle == RH_BottomRight;
    bool movesTop = handle == RH_Top || handle == RH_TopLeft || handle == RH_TopRight;
    bool movesBottom = handle == RH_Bottom || handle == RH_BottomLeft || handle == RH_BottomRight;

    if (movesLeft) {
        resized.left = ConstrainResizeActiveCoordinate(
            startSelection.left + dx, startSelection.left, startSelection.right,
            virtualBounds.left, virtualBounds.right,
            hasContent, contentBounds.left, contentBounds.right, enforceMinSize);
    } else if (movesRight) {
        resized.right = ConstrainResizeActiveCoordinate(
            startSelection.right + dx, startSelection.right, startSelection.left,
            virtualBounds.left, virtualBounds.right,
            hasContent, contentBounds.left, contentBounds.right, enforceMinSize);
    }

    if (movesTop) {
        resized.top = ConstrainResizeActiveCoordinate(
            startSelection.top + dy, startSelection.top, startSelection.bottom,
            virtualBounds.top, virtualBounds.bottom,
            hasContent, contentBounds.top, contentBounds.bottom, enforceMinSize);
    } else if (movesBottom) {
        resized.bottom = ConstrainResizeActiveCoordinate(
            startSelection.bottom + dy, startSelection.bottom, startSelection.top,
            virtualBounds.top, virtualBounds.bottom,
            hasContent, contentBounds.top, contentBounds.bottom, enforceMinSize);
    }

    return NormalizeRect(resized);
}

// 测量文字标注的包围盒（绝对虚拟屏幕坐标）
// 用 GDI+ MeasureString 测量，与提交态 DrawString 渲染同源：
//   - 字形左上角相对锚点 (x1,y1) 有内部偏移 (offX, offY)
//   - 宽高为紧凑字形范围（不含 GDI GetTextExtentPoint32W 的尾部 overhang）
//   - 边框左右 padding 对称，完整包住文字
// 性能：测量结果只依赖 (text, fontPx)，与锚点无关，故缓存"相对锚点的偏移/尺寸"。
//   命中缓存（textCacheValid && textCacheFontPx == fontPx）时直接复用，跳过 GDI+ 测量；
//   锚点变化（TransformAnnotationByBox 平移）不影响缓存有效性。
//   缓存对象（FontFamily/StringFormat/Font）取自会话级 InitGdipResources（经 g_captureCtx）。
static RECT MeasureTextAnnotation(HDC hdc, Annotation& a) {
    RECT rect = { a.x1, a.y1, a.x1, a.y1 };
    if (a.type != AT_Text || a.text.empty()) return rect;

    int fontPx = a.thickness;
    if (fontPx < 8) fontPx = 8;

    float offX = 0, offY = 0, w = 0, h = 0;
    // 缓存命中：直接复用相对锚点的偏移/尺寸（与 fontPx 校验）
    if (a.textCacheValid && a.textCacheFontPx == fontPx) {
        offX = a.textCacheOffX;
        offY = a.textCacheOffY;
        w = a.textCacheW;
        h = a.textCacheH;
    } else if (g_captureCtx && g_captureCtx->gdipInited
               && g_captureCtx->gdipFontFamily && g_captureCtx->gdipStrFmt) {
        // 缓存未命中：做一次 GDI+ 测量并回填缓存
        Gdiplus::Graphics graphics(hdc);
        graphics.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAlias);
        Gdiplus::Font* font = GetGdipFont(g_captureCtx, fontPx);
        Gdiplus::StringFormat* sf = g_captureCtx->gdipStrFmt;
        Gdiplus::RectF origin(0, 0, 0, 0);
        Gdiplus::RectF bounds;
        graphics.MeasureString(a.text.c_str(), (INT)a.text.size(), font, origin, sf, &bounds);
        offX = bounds.X;
        offY = bounds.Y;
        w = bounds.Width;
        h = bounds.Height;
        a.textCacheValid = true;
        a.textCacheFontPx = fontPx;
        a.textCacheOffX = offX;
        a.textCacheOffY = offY;
        a.textCacheW = w;
        a.textCacheH = h;
    }

    const int padding = 4;
    // 字形左上角（绝对坐标）= 锚点 + GDI+ 内部偏移
    int glyphLeft = a.x1 + (int)floorf(offX);
    int glyphTop  = a.y1 + (int)floorf(offY);
    int glyphRight = a.x1 + (int)ceilf(offX + w);
    int glyphBottom = a.y1 + (int)ceilf(offY + h);
    rect.left   = glyphLeft - padding;
    rect.top    = glyphTop - padding;
    rect.right  = glyphRight + padding;
    rect.bottom = glyphBottom + padding;
    return rect;
}

// 命中测试文字标注，返回标注索引（-1 表示未命中）
// 非常量引用：MeasureTextAnnotation 会回填文字测量缓存。
static int HitTestTextAnnotations(std::vector<Annotation>& anns, int x, int y, HDC hdc) {
    for (int i = (int)anns.size() - 1; i >= 0; i--) {
        if (anns[i].type == AT_Text) {
            RECT rect = MeasureTextAnnotation(hdc, anns[i]);
            if (PointInRect(x, y, rect)) {
                return i;
            }
        }
    }
    return -1;
}

// ==================== 通用标注几何（选中/拖拽/缩放） ====================
// 以下函数把「仅文字标注」具备的 hover/选中/拖拽/缩放能力推广到所有标注类型。
// 坐标系与 Annotation 一致：绝对虚拟屏幕坐标（与 ctx->mouseX/selection 同帧）。

// 点 (px,py) 到线段 (ax,ay)-(bx,by) 的最短距离（像素）。
// 用于画笔/箭头/马赛克涂抹等线条型标注的命中检测。
static double PointToSegmentDist(double px, double py, double ax, double ay, double bx, double by) {
    double dx = bx - ax, dy = by - ay;
    double lenSq = dx * dx + dy * dy;
    double t = 0;
    if (lenSq > 1e-9) {
        t = ((px - ax) * dx + (py - ay) * dy) / lenSq;
        if (t < 0) t = 0;
        else if (t > 1) t = 1;
    }
    double cx = ax + t * dx;
    double cy = ay + t * dy;
    double ex = px - cx, ey = py - cy;
    return std::sqrt(ex * ex + ey * ey);
}

// 点 (px,py) 到折线 pts 的最短距离（像素）。
static double PointToPolylineDist(double px, double py, const std::vector<POINT>& pts) {
    if (pts.empty()) return 1e18;
    if (pts.size() == 1) {
        double ex = px - pts[0].x, ey = py - pts[0].y;
        return std::sqrt(ex * ex + ey * ey);
    }
    double best = 1e18;
    for (size_t i = 0; i + 1 < pts.size(); i++) {
        double d = PointToSegmentDist(px, py, pts[i].x, pts[i].y, pts[i + 1].x, pts[i + 1].y);
        if (d < best) best = d;
    }
    return best;
}

// 计算单个标注的包围盒（绝对虚拟屏幕坐标）。
// 返回的矩形完整包住标注可见区域（含线宽/半径/文字 padding），用于选中框与四角 resize 手柄定位。
// hdc 仅在 AT_Text 时用于 GDI+ 测量文字宽高。
// 非常量引用：AT_Text 分支会经 MeasureTextAnnotation 回填文字测量缓存。
static RECT MeasureAnnotationBounds(Annotation& a, HDC hdc) {
    RECT r = { 0, 0, 0, 0 };
    auto setBox = [&](int x1, int y1, int x2, int y2) {
        r.left = (std::min)(x1, x2);
        r.top = (std::min)(y1, y2);
        r.right = (std::max)(x1, x2);
        r.bottom = (std::max)(y1, y2);
    };
    switch (a.type) {
        case AT_Rect:
        case AT_Circle:
        case AT_Mosaic:
            if (a.type == AT_Mosaic && !a.mosaicRect) {
                // 涂抹模式：路径包围盒 + 半径
                if (a.pts.empty()) { r = { 0,0,0,0 }; return r; }
                int rad = a.brushRadius;
                int minL = INT_MAX, minT = INT_MAX, maxR = INT_MIN, maxB = INT_MIN;
                for (const POINT& p : a.pts) {
                    if (p.x < minL) minL = p.x;
                    if (p.x > maxR) maxR = p.x;
                    if (p.y < minT) minT = p.y;
                    if (p.y > maxB) maxB = p.y;
                }
                r.left = minL - rad; r.top = minT - rad;
                r.right = maxR + rad; r.bottom = maxB + rad;
                return r;
            }
            // Rect/Circle/MosaicRect：以两个对角点为包围盒
            setBox(a.x1, a.y1, a.x2, a.y2);
            return r;
        case AT_Arrow:
            return MeasureArrowAnnotationBounds(a);
        case AT_Brush: {
            if (a.pts.empty()) { r = { 0,0,0,0 }; return r; }
            int minL = INT_MAX, minT = INT_MAX, maxR = INT_MIN, maxB = INT_MIN;
            for (const POINT& p : a.pts) {
                if (p.x < minL) minL = p.x;
                if (p.x > maxR) maxR = p.x;
                if (p.y < minT) minT = p.y;
                if (p.y > maxB) maxB = p.y;
            }
            r.left = minL; r.top = minT; r.right = maxR; r.bottom = maxB;
            return r;
        }
        case AT_Text:
            return MeasureTextAnnotation(hdc, a);
    }
    return r;
}

// 命中测试任意标注，返回索引（-1 表示未命中）。
// 从顶层（数组末尾，绘制最上层）向底层遍历，命中第一个即返回（与视觉 z-order 一致）。
// 容差按线宽自适应：细线给 6px 余量，粗线给半个线宽，避免细线难以点中。
// 非常量引用：AT_Text/AT_Mosaic 分支会回填文字测量缓存。
static int HitTestAnnotation(std::vector<Annotation>& anns, int x, int y, HDC hdc) {
    for (int i = (int)anns.size() - 1; i >= 0; i--) {
        Annotation& a = anns[i];
        // 马赛克区域（框选/涂抹）不可选中、不可拖拽，直接跳过命中测试。
        if (a.type == AT_Mosaic) continue;
        double tol = (std::max)(6.0, a.thickness / 2.0 + 2.0);
        switch (a.type) {
            case AT_Rect: {
                // 仅命中矩形四条边轮廓（空心框），内部空白不选中。
                // 到任一边线段的最短距离 ≤ tol 即命中（容差向外扩散几像素辅助探测）。
                double d1 = PointToSegmentDist((double)x, (double)y, a.x1, a.y1, a.x2, a.y1);
                double d2 = PointToSegmentDist((double)x, (double)y, a.x2, a.y2, a.x1, a.y2);
                double d3 = PointToSegmentDist((double)x, (double)y, a.x1, a.y2, a.x1, a.y1);
                double d4 = PointToSegmentDist((double)x, (double)y, a.x2, a.y1, a.x2, a.y2);
                double dm = (std::min)((std::min)(d1, d2), (std::min)(d3, d4));
                if (dm <= tol) return i;
                break;
            }
            case AT_Circle: {
                // 椭圆（由包围盒定义）轮廓命中：用归一化径向距离近似。
                // r = hypot((x-cx)/a, (y-cy)/b)，r≈1 即落在椭圆上。
                double cx = (a.x1 + a.x2) * 0.5;
                double cy = (a.y1 + a.y2) * 0.5;
                double aax = std::fabs((double)a.x2 - a.x1) * 0.5;
                double aay = std::fabs((double)a.y2 - a.y1) * 0.5;
                if (aax < 0.5 && aay < 0.5) {
                    // 退化为点：直接点距
                    double ex = x - cx, ey = y - cy;
                    if (std::sqrt(ex * ex + ey * ey) <= tol) return i;
                } else if (aax < 0.5) {
                    // 退化为竖直线段
                    if (PointToSegmentDist((double)x, (double)y, cx, a.y1, cx, a.y2) <= tol) return i;
                } else if (aay < 0.5) {
                    // 退化为水平线段
                    if (PointToSegmentDist((double)x, (double)y, a.x1, cy, a.x2, cy) <= tol) return i;
                } else {
                    double r = std::sqrt(((x - cx) / aax) * ((x - cx) / aax)
                                         + ((y - cy) / aay) * ((y - cy) / aay));
                    // (r-1)*min(a,b) 把归一化距离换算回像素（用短半轴近似像素半径，保守且足够命中探测）
                    double minAxis = (std::min)(aax, aay);
                    if (std::fabs(r - 1.0) * minAxis <= tol) return i;
                }
                break;
            }
            case AT_Arrow: {
                if (PointToSegmentDist((double)x, (double)y, a.x1, a.y1, a.x2, a.y2) <= tol)
                    return i;
                break;
            }
            case AT_Brush: {
                if (PointToPolylineDist((double)x, (double)y, a.pts) <= tol) return i;
                break;
            }
            case AT_Mosaic: {
                if (a.mosaicRect) {
                    RECT box = MeasureAnnotationBounds(a, hdc);
                    if (PointInRect(x, y, box)) return i;
                } else {
                    // 涂抹：路径 + 半径
                    if (PointToPolylineDist((double)x, (double)y, a.pts) <= a.brushRadius) return i;
                }
                break;
            }
            case AT_Text: {
                RECT box = MeasureTextAnnotation(hdc, a);
                if (PointInRect(x, y, box)) return i;
                break;
            }
        }
    }
    return -1;
}

// 命中测试标注包围盒的 8 个手柄（4 角 + 4 边中点），返回 ResizeHandle 或 RH_None。
// 容差沿用选区手柄的 SC_HANDLE_SIZE，保证与选区手柄一致的可点击范围。
static int HitTestAnnotationHandle(int x, int y, const RECT& box) {
    int hs = SC_HANDLE_SIZE;
    int cx = (box.left + box.right) / 2;
    int cy = (box.top + box.bottom) / 2;
    struct { int hx, hy; int handle; } tests[] = {
        { box.left,  box.top,    RH_TopLeft },
        { box.right, box.top,    RH_TopRight },
        { box.left,  box.bottom, RH_BottomLeft },
        { box.right, box.bottom, RH_BottomRight },
        { cx,        box.top,    RH_Top },
        { cx,        box.bottom, RH_Bottom },
        { box.left,  cy,         RH_Left },
        { box.right, cy,         RH_Right },
    };
    for (auto& t : tests) {
        RECT hitBox = { t.hx - hs, t.hy - hs, t.hx + hs, t.hy + hs };
        if (PointInRect(x, y, hitBox)) return t.handle;
    }
    return RH_None;
}

// 命中测试箭头的起点/终点端点手柄，返回 RH_ArrowStart / RH_ArrowEnd / RH_None。
// 箭头只允许拖拽两个端点（而非四角包围盒缩放），故单独命中 a.x1,y1 / a.x2,y2。
// 容差沿用 SC_HANDLE_SIZE，与其它标注手柄可点击范围一致。
static int HitTestArrowEndpoints(int x, int y, const Annotation& a) {
    int hs = SC_HANDLE_SIZE;
    struct { int hx, hy; int handle; } tests[] = {
        { a.x1, a.y1, RH_ArrowStart },
        { a.x2, a.y2, RH_ArrowEnd   },
    };
    for (auto& t : tests) {
        RECT hitBox = { t.hx - hs, t.hy - hs, t.hx + hs, t.hy + hs };
        if (PointInRect(x, y, hitBox)) return t.handle;
    }
    return RH_None;
}

// 按标注类型返回其缩放手柄命中（统一入口，供 LButtonDown/MouseMove/SETCURSOR 复用）：
//   箭头 = 起点/终点 2 端点；矩形/圆 = 包围盒 8 手柄（4 角 + 4 边中点）；
//   画笔/马赛克 = 无手柄（RH_None，不可缩放，画笔仅可整体拖动）。
// hdc 仅用于测量文字包围盒（此处文字不会进入，但签名与 HitTestAnnotation 对齐便于扩展）。
static int HitTestAnnotationResizeHandle(const Annotation& a, int x, int y, HDC hdc) {
    switch (a.type) {
        case AT_Arrow:
            return HitTestArrowEndpoints(x, y, a);
        case AT_Rect:
        case AT_Circle: {
            RECT box = MeasureAnnotationBounds(const_cast<Annotation&>(a), hdc);
            return HitTestAnnotationHandle(x, y, box);
        }
        default:
            // AT_Brush / AT_Mosaic / AT_Text 无缩放手柄
            return RH_None;
    }
}

// 按包围盒变换（平移 + 等比/非等比缩放）映射标注所有坐标。
// oldBox -> newBox：标注内每个点 p 映射为 newBox.left + (p-oldBox.left)*sx 等。
// 用于四角 resize（画笔/涂抹路径按包围盒整体缩放，保持形状比例）。
// sx/sy 防 0：旧宽/高为 0 时退化为平移。
static void TransformAnnotationByBox(Annotation& a, const RECT& oldBox, const RECT& newBox) {
    double oldW = oldBox.right - oldBox.left;
    double oldH = oldBox.bottom - oldBox.top;
    double newW = newBox.right - newBox.left;
    double newH = newBox.bottom - newBox.top;
    double sx = (oldW > 0.5) ? newW / oldW : 1.0;
    double sy = (oldH > 0.5) ? newH / oldH : 1.0;
    auto mapX = [&](int v) { return (int)(newBox.left + (v - oldBox.left) * sx + 0.5); };
    auto mapY = [&](int v) { return (int)(newBox.top + (v - oldBox.top) * sy + 0.5); };
    switch (a.type) {
        case AT_Rect:
        case AT_Circle:
        case AT_Arrow:
        case AT_Mosaic:
            if (a.type == AT_Mosaic && !a.mosaicRect) {
                for (POINT& p : a.pts) { p.x = mapX(p.x); p.y = mapY(p.y); }
            } else {
                a.x1 = mapX(a.x1); a.y1 = mapY(a.y1);
                a.x2 = mapX(a.x2); a.y2 = mapY(a.y2);
            }
            break;
        case AT_Brush:
            for (POINT& p : a.pts) { p.x = mapX(p.x); p.y = mapY(p.y); }
            break;
        case AT_Text:
            // 文字不经过此路径（走原 draggingTextAnnotation 平移），此处兜底平移锚点
            a.x1 = mapX(a.x1); a.y1 = mapY(a.y1);
            break;
    }
}

// ==================== GDI+ 文字测量（与提交态渲染同源） ====================
// 提交态文字用 GDI+ DrawString 渲染（StringAlignmentNear 顶部左对齐）。
// 为保证输入态边框/光标/命中与提交态视觉一致，输入态也必须用 GDI+ 测量。
// 旧实现用 GDI GetTextExtentPoint32W，其宽度含尾部 overhang（右侧多出留白），
// 且垂直基线与 GDI+ 不同（导致文字偏靠下），故统一改用 GDI+。

// 测量文字的紧凑包围盒：返回 DrawString(StringAlignmentNear/Near) 下字形相对锚点的偏移与尺寸。
//   outOffsetX/outOffsetY：字形左上角相对锚点 (0,0) 的偏移（通常 X≈0，Y 为字体内部顶部 leading）。
//   outW/outH：字形紧凑宽高。
// 这样边框 = 锚点 + offset ± padding，可左右对称、紧贴字形。
static void MeasureTextGdip(HDC hdc, const std::wstring& text, int fontPx,
                             float& outOffsetX, float& outOffsetY, float& outW, float& outH) {
    outOffsetX = 0; outOffsetY = 0; outW = 0; outH = 0;
    if (text.empty()) {
        outH = (float)fontPx;
        return;
    }
    // GDI+ 已由会话级 InitGdipResources 启动；FontFamily/StringFormat/Font 复用会话缓存。
    if (!g_captureCtx || !g_captureCtx->gdipInited
        || !g_captureCtx->gdipFontFamily || !g_captureCtx->gdipStrFmt) return;
    Gdiplus::Graphics graphics(hdc);
    graphics.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAlias);
    Gdiplus::Font* font = GetGdipFont(g_captureCtx, fontPx);
    Gdiplus::StringFormat* sf = g_captureCtx->gdipStrFmt;
    // MeasureString 返回与 DrawString 一致的布局包围盒（含 GDI+ 的字体内部间距）。
    Gdiplus::RectF origin(0, 0, 0, 0);
    Gdiplus::RectF bounds;
    graphics.MeasureString(text.c_str(), (INT)text.size(), font, origin, sf, &bounds);
    outOffsetX = bounds.X;
    outOffsetY = bounds.Y;
    outW = bounds.Width;
    outH = bounds.Height;
}

// 测量逐字符累计宽度（用于光标定位/选中高亮）。
// widths[i] 表示前 i 个字符（text[0..i-1]）的累计紧凑宽度，widths[0]=0。
// 与 DrawString 渲染进度一致，光标 x = textX + widths[i]。
// P2 优化：原实现逐前缀循环调 MeasureString（O(n²)），改用 MeasureCharacterRanges
// 一次测出各前缀边界（O(n)）。MeasureCharacterRanges 会通过 SetMeasurableCharacterRanges
// 修改 StringFormat，故不能就地改会话共享的 ctx->gdipStrFmt，这里栈上复制一份使用。
static void MeasureCharWidthsGdip(HDC hdc, const std::wstring& text, int fontPx,
                                   std::vector<float>& widths) {
    widths.assign(text.size() + 1, 0.0f);
    if (text.empty()) return;
    // GDI+ 已由会话级 InitGdipResources 启动；FontFamily/StringFormat/Font 复用会话缓存。
    if (!g_captureCtx || !g_captureCtx->gdipInited
        || !g_captureCtx->gdipFontFamily || !g_captureCtx->gdipStrFmt) return;
    Gdiplus::Graphics graphics(hdc);
    graphics.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAlias);
    Gdiplus::Font* font = GetGdipFont(g_captureCtx, fontPx);
    // 复制会话级 StringFormat（Near/Near），在其上设置可测范围，避免污染共享对象。
    Gdiplus::StringFormat localSf(g_captureCtx->gdipStrFmt);

    const int n = (int)text.size();
    // 布局矩形足够大以容纳整串，避免 Near/Near 文本换行或裁剪。
    Gdiplus::RectF layoutRect(0, 0, 1000000.0f, 1000000.0f);

    // SetMeasurableCharacterRanges 单次可设范围数受 GDI+ 内部缓冲限制，分批处理。
    const int kBatch = 16;
    Gdiplus::CharacterRange ranges[kBatch];
    Gdiplus::Region regions[kBatch];
    for (int start = 1; start <= n; start += kBatch) {
        int cnt = (n - start + 1 < kBatch) ? (n - start + 1) : kBatch;
        for (int j = 0; j < cnt; j++) {
            int i = start + j;              // 前缀长度 i (1..n)
            ranges[j].First = 0;
            ranges[j].Length = i;
        }
        if (localSf.SetMeasurableCharacterRanges(cnt, ranges) != Gdiplus::Ok) continue;
        if (graphics.MeasureCharacterRanges(text.c_str(), n, font, layoutRect,
                                             &localSf, cnt, regions) != Gdiplus::Ok) continue;
        for (int j = 0; j < cnt; j++) {
            int i = start + j;
            Gdiplus::RectF bounds;
            // 前 i 个字符区域的右边缘 = 原 MeasureString(prefix) 的 bounds.X + bounds.Width。
            if (regions[j].GetBounds(&bounds, &graphics) == Gdiplus::Ok) {
                widths[i] = bounds.GetRight();
            }
        }
    }
}

// 根据鼠标位置计算光标在文字中的位置（字符索引）
// 用 GDI+ 逐字符测量，与 DrawString 渲染进度一致。
static int CalcCaretPosFromMouse(HDC hdc, const std::wstring& text, int fontPx, int textX, int mouseX) {
    if (text.empty()) return 0;

    std::vector<float> widths;
    MeasureCharWidthsGdip(hdc, text, fontPx, widths);

    int bestPos = 0;
    float bestDist = FLT_MAX;
    for (size_t i = 0; i <= text.size(); i++) {
        float x = (float)textX + widths[i];
        float dist = fabsf(x - (float)mouseX);
        if (dist < bestDist) {
            bestDist = dist;
            bestPos = (int)i;
        }
    }
    return bestPos;
}

// P1 脏区域辅助：使"被拖拽/缩放标注的旧位置 ∪ 新位置"区域无效。
// 用于 resizingAnnotation/draggingAnnotation/draggingTextAnnotation 的 MOUSEMOVE。
// curBox 为当前帧标注包围盒（绝对虚拟屏幕坐标，由调用方用 MeasureAnnotationBounds 算）；
// 与上帧缓存的 lastAnnotationBox 求并集后转 backDC 坐标并扩大。lastAnnotationBox 由 WM_PAINT 更新。
// 扩大量需覆盖选中态的 resize 手柄：手柄圆心贴在包围盒边缘，半径 = SC_HANDLE_SIZE/2，
// 故 inflate 量取 SC_HANDLE_SIZE/2 + 余量，确保旧/新手柄范围都在脏区内，避免拖拽时残留手柄痕迹。
static void InvalidateAnnotationOp(HWND hwnd, CaptureContext* ctx, const RECT& curBox) {
    RECT b;
    if (ctx->hasLastAnnotationBox) {
        b = UnionRectSafe(ctx->lastAnnotationBox, curBox);
    } else {
        b = curBox;
    }
    b.left -= ctx->virtualX; b.top -= ctx->virtualY;
    b.right -= ctx->virtualX; b.bottom -= ctx->virtualY;
    const int handleMargin = SC_HANDLE_SIZE / 2 + 4;  // 手柄半径 + 描边/抗锯齿余量
    InvalidateRect(hwnd, &InflateRectBy(b, handleMargin), FALSE);
}

// 选中/取消选中覆盖物时的精确脏区计算。
// 根据当前 selectedAnnotation / selectedTextAnnotation 的包围盒算出需重绘的脏区
// （含 resize 手柄半径余量），可选合并工具栏 + popup 旧位置（切换不同类型工具时）。
// 调用时机：必须在调用方清空 selected*/hovered* 之前调用，否则读不到旧选中项的包围盒。
// 坐标基准：返回 backDC/客户区坐标（已减 virtualX/Y），可直接用于 InvalidateRect(hwnd, &r, FALSE)。
// 参数 includeToolbar：true 时并集工具栏矩形和 popup 矩形（覆盖 activeTool 切换导致的高亮按钮
//   位移 + popup 打开/关闭/切换），用于「切换到不同类型工具」场景。
// 返回值可能为无效矩形（{0,0,0,0}）：表示当前无任何选中项，调用方据此决定是否全屏重绘兜底。
static RECT CalcSelectionDirty(CaptureContext* ctx, bool includeToolbar) {
    RECT dirty = {0, 0, 0, 0};
    const int handleMargin = SC_HANDLE_SIZE / 2 + 4;  // 与 InvalidateAnnotationOp 一致的手柄半径余量

    // 非文字标注选中项的包围盒
    if (ctx->selectedAnnotation >= 0 && ctx->selectedAnnotation < (int)ctx->annotations.size()) {
        RECT r = MeasureAnnotationBounds(ctx->annotations[ctx->selectedAnnotation], ctx->backDC);
        r.left -= ctx->virtualX; r.top -= ctx->virtualY;
        r.right -= ctx->virtualX; r.bottom -= ctx->virtualY;
        dirty = UnionRectSafe(dirty, InflateRectBy(r, handleMargin));
    }
    // 文字标注选中项的包围盒（MeasureTextAnnotation 含文字 padding）
    if (ctx->selectedTextAnnotation >= 0 && ctx->selectedTextAnnotation < (int)ctx->annotations.size()) {
        RECT r = MeasureTextAnnotation(ctx->backDC, ctx->annotations[ctx->selectedTextAnnotation]);
        r.left -= ctx->virtualX; r.top -= ctx->virtualY;
        r.right -= ctx->virtualX; r.bottom -= ctx->virtualY;
        dirty = UnionRectSafe(dirty, InflateRectBy(r, handleMargin));
    }
    // 切换不同类型工具：activeTool 变化后高亮按钮位移，popup 可能打开/关闭/切换类型，
    // 将整条工具栏和当前 popup 都纳入脏区（宁大勿小，避免按钮高亮残影）。
    if (includeToolbar) {
        if (IsValidRect(ctx->toolbarRect)) {
            dirty = UnionRectSafe(dirty, InflateRectBy(ctx->toolbarRect, 4));
        }
        if (IsValidRect(ctx->popupRect)) {
            dirty = UnionRectSafe(dirty, InflateRectBy(ctx->popupRect, 4));
        }
    }
    return dirty;
}

// P1 脏区域辅助：使文字行区域无效（用于文字编辑态的键盘输入/删除/光标移动）。
// 文字行 y 范围取自上帧光标高度（lastCaretRect），x 范围覆盖整个选区宽度
// （文字必然在选区内，选区宽度是文字行可能宽度的安全上界）。lastCaret 无效时全屏。
// 坐标基准：backDC 坐标 = 客户区坐标（已减 virtualX/Y），与 InvalidateRect 一致。
static void InvalidateTextLine(HWND hwnd, CaptureContext* ctx) {
    if (!ctx->hasLastCaret) {
        InvalidateRect(hwnd, NULL, FALSE);
        return;
    }
    // 文字行矩形：选区宽度 × 光标行高（backDC 坐标）
    RECT line = {
        ctx->selection.left - ctx->virtualX,
        ctx->lastCaretRect.top,
        ctx->selection.right - ctx->virtualX,
        ctx->lastCaretRect.bottom
    };
    InvalidateRect(hwnd, &InflateRectBy(line, 4), FALSE);
}

// 截图覆盖层窗口过程（双缓冲渲染）
static LRESULT CALLBACK ScreenshotOverlayWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    CaptureContext* ctx = g_captureCtx;
    if (!ctx) return DefWindowProc(hwnd, msg, wParam, lParam);

    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        HDC backDC = ctx->backDC;

        // 计算浮窗位置
        int panelX, panelY;
        CalcPanelPosition(ctx->mouseX, ctx->mouseY,
            ctx->virtualX, ctx->virtualY, ctx->virtualW, ctx->virtualH, ctx->panelMetrics, panelX, panelY);
        // 转为相对坐标
        int panelXRel = panelX - ctx->virtualX;
        int panelYRel = panelY - ctx->virtualY;

        RECT curPanelRect = { panelXRel, panelYRel,
            panelXRel + ctx->panelMetrics.w, panelYRel + ctx->panelMetrics.h };

        // 当前选区矩形
        RECT curSelRect = {0,0,0,0};
        if (ctx->state == CS_Selecting) {
            curSelRect.left = (std::min)(ctx->startX, ctx->endX) - ctx->virtualX;
            curSelRect.top = (std::min)(ctx->startY, ctx->endY) - ctx->virtualY;
            curSelRect.right = (std::max)(ctx->startX, ctx->endX) - ctx->virtualX;
            curSelRect.bottom = (std::max)(ctx->startY, ctx->endY) - ctx->virtualY;
        } else if (ctx->state == CS_Confirmed || ctx->state == CS_Resizing
                   || ctx->state == CS_Moving || ctx->state == CS_Drawing
                   || ctx->state == CS_TextEditing) {
            curSelRect.left = ctx->selection.left - ctx->virtualX;
            curSelRect.top = ctx->selection.top - ctx->virtualY;
            curSelRect.right = ctx->selection.right - ctx->virtualX;
            curSelRect.bottom = ctx->selection.bottom - ctx->virtualY;
        }

        // 当前高亮窗口矩形
        RECT curHlRect = {0,0,0,0};
        if (ctx->state == CS_Idle && ctx->hoveredWindow >= 0 && ctx->hoveredWindow < (int)ctx->windows.size()) {
            const RECT& wr = ctx->windows[ctx->hoveredWindow].rect;
            curHlRect = { wr.left - ctx->virtualX, wr.top - ctx->virtualY,
                wr.right - ctx->virtualX, wr.bottom - ctx->virtualY };
        }

        double ds = ctx->dpiScale;
        int physW = (int)(ctx->virtualW * ds + 0.5);
        int physH = (int)(ctx->virtualH * ds + 0.5);

        // P1 脏区域优化：仅"选区正在移动/缩放"的状态（Selecting/Resizing/Moving）
        // 因遮罩边界大范围变化，必须整屏恢复背景。其余 confirmedMode 状态（Confirmed/Drawing/
        // TextEditing）选区静止，可用 ps.rcPaint 局部刷新（由各调用点 InvalidateRect 传精确矩形）。
        if (ctx->state == CS_Selecting || ctx->state == CS_Resizing || ctx->state == CS_Moving) {
            ctx->needFullRedraw = true;
        }

        // 本帧脏区域：fullFrame 时为全屏，否则取 ps.rcPaint（由 InvalidateRect 矩形决定）。
        // 客户区坐标 = backDC 坐标（窗口原点 = 虚拟屏幕左上角），可直接用于背景恢复与裁剪。
        bool fullFrame = ctx->needFullRedraw;
        RECT dirtyRect;
        HRGN dirtyRgn = NULL;
        if (fullFrame) {
            dirtyRect = { 0, 0, ctx->virtualW, ctx->virtualH };
        } else {
            // ps.rcPaint 在 InvalidateRect(NULL) 时为整个客户区（等价全屏），传矩形时为该矩形。
            dirtyRect = ps.rcPaint;
            // 限定到虚拟屏幕范围内（防御性：ps.rcPaint 理论上不会超出客户区=虚拟屏幕）
            if (dirtyRect.left < 0) dirtyRect.left = 0;
            if (dirtyRect.top < 0) dirtyRect.top = 0;
            if (dirtyRect.right > ctx->virtualW) dirtyRect.right = ctx->virtualW;
            if (dirtyRect.bottom > ctx->virtualH) dirtyRect.bottom = ctx->virtualH;
        }

        // 恢复背景：fullFrame 全屏恢复；局部帧只恢复 dirtyRect（其余区域保留上帧最终画面）。
        if (fullFrame) {
            if (ds > 1.01 || ds < 0.99) {
                StretchBlt(backDC, 0, 0, ctx->virtualW, ctx->virtualH,
                    ctx->memDC, 0, 0, physW, physH, SRCCOPY);
            } else {
                BitBlt(backDC, 0, 0, ctx->virtualW, ctx->virtualH,
                    ctx->memDC, 0, 0, SRCCOPY);
            }
            ctx->needFullRedraw = false;
        } else {
            // 局部恢复脏区域为原始背景（后续渲染管线会对该区域重画遮罩/标注等，
            // 因 AlphaBlend 作用于已恢复的清晰背景，结果与全屏渲染一致）。
            RestoreDirtyRegion(backDC, ctx->memDC, dirtyRect, ds);
            // 设置裁剪区：后续所有 GDI/GDI+ 绘制自动限制在 dirtyRect 内，
            // 避免重绘未变化区域（GDI+ Graphics(backDC) 会继承此裁剪区）。
            dirtyRgn = CreateRectRgn(dirtyRect.left, dirtyRect.top,
                                     dirtyRect.right, dirtyRect.bottom);
            SelectClipRgn(backDC, dirtyRgn);
        }

        // 绘制窗口高亮（Idle 状态）
        if (ctx->state == CS_Idle) {
            if (ctx->hoveredWindow >= 0 && ctx->hoveredWindow < (int)ctx->windows.size()) {
                // 高亮悬停的窗口
                DrawWindowHighlight(backDC, ctx->windows[ctx->hoveredWindow].rect,
                    ctx->virtualX, ctx->virtualY, ctx->gdi);
            } else {
                // 没有匹配到窗口时，高亮鼠标所在的屏幕
                POINT pt = { ctx->mouseX, ctx->mouseY };
                HMONITOR hMonitor = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
                if (hMonitor) {
                    MONITORINFO monitorInfo;
                    monitorInfo.cbSize = sizeof(MONITORINFO);
                    if (GetMonitorInfo(hMonitor, &monitorInfo)) {
                        DrawWindowHighlight(backDC, monitorInfo.rcMonitor,
                            ctx->virtualX, ctx->virtualY, ctx->gdi);
                    }
                }
            }
        }

        // ---- 确认/调整/移动/绘制态：遮罩 + 边框 + 手柄 + 标注 + 工具栏 ----
        RECT curLabelRect = {0,0,0,0};
        RECT curToolbarRect = {0,0,0,0};
        RECT curPopupRect = {0,0,0,0};
        bool confirmedMode = (ctx->state == CS_Confirmed || ctx->state == CS_Resizing
                              || ctx->state == CS_Moving || ctx->state == CS_Drawing
                              || ctx->state == CS_TextEditing);
        if (confirmedMode) {
            // 选区外遮罩（选区内部保持清晰）
            DrawDimMask(backDC, ctx->gdi,
                curSelRect.left, curSelRect.top, curSelRect.right, curSelRect.bottom,
                ctx->virtualW, ctx->virtualH);
            // 已提交标注 + 正在绘制的标注（绘制范围 clip 在选区内）
            // 调整选区时也保持显示，便于看清内容是否会被裁掉。
            if (ctx->state == CS_Confirmed || ctx->state == CS_Drawing || ctx->state == CS_Resizing) {
                const Annotation* cur = ctx->hasCurDrawing ? &ctx->curDrawing : nullptr;
                // 马赛克（reveal-mask 模型）：只有当前帧确实包含马赛克内容时才延迟生成 base，
                // 普通截图确认态不再承担整张虚拟屏幕的马赛克预处理。
                if (HasMosaicToRender(ctx->annotations, cur)) {
                    if (MosaicBaseNeedsRebuild(ctx)) RebuildMosaicBase(ctx);
                    if (ctx->mosaicBaseDC) {
                        // 全屏缓存与 backDC 均以虚拟屏幕左上角为原点；把标注绝对坐标转换到
                        // backDC 局部坐标，并用选区矩形硬裁剪马赛克笔刷的可见范围。
                        RevealMosaicToTarget(backDC, ctx->mosaicBaseDC,
                                             ctx->annotations, cur, curSelRect,
                                             (float)-ctx->virtualX, (float)-ctx->virtualY);
                    }
                }
                DrawAnnotations(backDC, curSelRect, ctx->virtualX, ctx->virtualY, ctx->annotations, cur);
                // 缓存正在绘制标注的包围盒（绝对虚拟屏幕坐标），供 CS_Drawing 局部刷新计算旧位置
                if (ctx->state == CS_Drawing && ctx->hasCurDrawing) {
                    ctx->lastDrawingBox = MeasureAnnotationBounds(ctx->curDrawing, backDC);
                    ctx->hasLastDrawingBox = true;
                } else if (ctx->state != CS_Drawing) {
                    ctx->hasLastDrawingBox = false;
                }
                // 正在拖拽的矩形马赛克：叠加虚线边框提示当前框选范围（backDC 绝对坐标）。
                // 涂抹模式不画矩形边框（其范围由预览圆体现）。
                if (ctx->state == CS_Drawing && ctx->hasCurDrawing
                    && ctx->curDrawing.type == AT_Mosaic && ctx->curDrawing.mosaicRect) {
                    int rx1 = (std::min)(ctx->curDrawing.x1, ctx->curDrawing.x2);
                    int ry1 = (std::min)(ctx->curDrawing.y1, ctx->curDrawing.y2);
                    int rx2 = (std::max)(ctx->curDrawing.x1, ctx->curDrawing.x2);
                    int ry2 = (std::max)(ctx->curDrawing.y1, ctx->curDrawing.y2);
                    // GDI+ 已由会话级 InitGdipResources 启动，此处直接使用。
                    {
                        Gdiplus::Graphics graphics(backDC);
                        graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
                        // 虚线笔：白色底 + 深色虚线，保证在任意背景上可见
                        Gdiplus::Pen whitePen(Gdiplus::Color(255, 255, 255, 255), 3.0f);
                        graphics.DrawRectangle(&whitePen, (float)rx1, (float)ry1,
                                               (float)(rx2 - rx1), (float)(ry2 - ry1));
                        Gdiplus::Pen dashPen(Gdiplus::Color(255, 0x1E, 0x88, 0xE5), 1.5f);
                        dashPen.SetDashStyle(Gdiplus::DashStyleDash);
                        graphics.DrawRectangle(&dashPen, (float)rx1, (float)ry1,
                                               (float)(rx2 - rx1), (float)(ry2 - ry1));
                    }
                }
            }
            // 文字编辑态：绘制输入光标和选中文字标注的边框
            if (ctx->state == CS_TextEditing) {
                if (HasMosaicToRender(ctx->annotations, nullptr)) {
                    if (MosaicBaseNeedsRebuild(ctx)) RebuildMosaicBase(ctx);
                    if (ctx->mosaicBaseDC) {
                        RevealMosaicToTarget(backDC, ctx->mosaicBaseDC,
                                             ctx->annotations, nullptr, curSelRect,
                                             (float)-ctx->virtualX, (float)-ctx->virtualY);
                    }
                }
                // 绘制已提交的标注
                DrawAnnotations(backDC, curSelRect, ctx->virtualX, ctx->virtualY, ctx->annotations, nullptr);

                // 绘制当前输入的文字和光标（统一用 GDI+，与提交态 DrawString 完全一致，
                // 避免旧 GDI TextOutW 导致的文字偏靠下、右侧间距偏大的问题）
                // 注意：测量与绘制共用同一个 GDI+ Graphics 作用域，避免在 backDC 上反复
                // Startup/Shutdown 及创建多个 Graphics 对象引发的状态混乱/崩溃。
                int fontPx = SC_FONT_SIZES[ctx->fontSizeIdx];
                COLORREF textColor = SC_COLOR_PRESETS[ctx->drawColorIdx];

                // 文字锚点转换为相对坐标
                int textX = ctx->textAnchorX - ctx->virtualX;
                int textY = ctx->textAnchorY - ctx->virtualY;

                // 单次 GDI+ 作用域：完成测量（整体包围盒 + 逐字符宽度）与文字绘制。
                // GDI+ 已由会话级 InitGdipResources 启动；FontFamily/StringFormat/Font 复用会话缓存。
                // 关键：测量与绘制必须在同一 Graphics 作用域内（注释 3245-3247），故仍用内层 {} 包住。
                float offX = 0, offY = 0, textW = 0, textH = 0;
                std::vector<float> charWidths;
                {
                    Gdiplus::Graphics graphics(backDC);
                    graphics.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAlias);
                    // 复用会话缓存的 FontFamily/StringFormat/Font（指针非空，InitGdipResources 保证）
                    Gdiplus::Font* font = GetGdipFont(ctx, fontPx);
                    Gdiplus::StringFormat* sf = ctx->gdipStrFmt;

                    // 整体紧凑包围盒
                    Gdiplus::RectF origin(0, 0, 0, 0);
                    Gdiplus::RectF bounds;
                    if (!ctx->textBuf.empty()) {
                        graphics.MeasureString(ctx->textBuf.c_str(), (INT)ctx->textBuf.size(),
                                               font, origin, sf, &bounds);
                        offX = bounds.X;
                        offY = bounds.Y;
                        textW = bounds.Width;
                        textH = bounds.Height;
                    } else {
                        textH = (float)fontPx;
                    }
                    // 逐字符累计宽度（与 DrawString 渲染进度一致）
                    charWidths.assign(ctx->textBuf.size() + 1, 0.0f);
                    if (!ctx->textBuf.empty()) {
                        std::wstring sub;
                        sub.reserve(ctx->textBuf.size());
                        for (size_t i = 1; i <= ctx->textBuf.size(); i++) {
                            sub.assign(ctx->textBuf, 0, i);
                            graphics.MeasureString(sub.c_str(), (INT)sub.size(),
                                                   font, origin, sf, &bounds);
                            charWidths[i] = bounds.X + bounds.Width;
                        }
                    }

                    // 绘制文字（与提交态一致：顶部左对齐到锚点）
                    if (!ctx->textBuf.empty()) {
                        Gdiplus::SolidBrush textBrush(Gdiplus::Color(GetRValue(textColor),
                                                                     GetGValue(textColor),
                                                                     GetBValue(textColor)));
                        Gdiplus::RectF layoutRect((float)textX, (float)textY, 10000.0f,
                                                  (float)(fontPx * 2));
                        graphics.DrawString(ctx->textBuf.c_str(), -1, font, layoutRect,
                                            sf, &textBrush);
                    }
                }

                // 字形可见区域的左上角（含 GDI+ 内部偏移）
                float glyphLeft = (float)textX + offX;
                float glyphTop  = (float)textY + offY;
                // 文字可见宽度（最小给一个占位宽度，空输入框也有合理大小）
                float glyphW = (textW > 20.0f || !ctx->textBuf.empty()) ? textW : 20.0f;
                float glyphH = (textH > 0 ? textH : (float)fontPx);

                // 绘制边框：左右对称 padding，上下基于字形紧凑高度。
                // 用字形可见区域 + padding，保证左侧与右侧间距一致，并完整包住文字。
                const float padding = 4.0f;
                int boxLeft   = (int)floorf(glyphLeft - padding);
                int boxTop    = (int)floorf(glyphTop - padding);
                int boxRight  = (int)ceilf(glyphLeft + glyphW + padding);
                int boxBottom = (int)ceilf(glyphTop + glyphH + padding);
                HPEN boxPen = CreatePen(PS_SOLID, 1, textColor);
                HGDIOBJ oldPen2 = SelectObject(backDC, boxPen);
                HGDIOBJ oldBrush2 = SelectObject(backDC, GetStockObject(NULL_BRUSH));
                Rectangle(backDC, boxLeft, boxTop, boxRight, boxBottom);
                SelectObject(backDC, oldBrush2);
                SelectObject(backDC, oldPen2);
                DeleteObject(boxPen);

                // 绘制文字选择高亮（基于 GDI+ 字符宽度）
                if (ctx->textSelStart >= 0 && ctx->textSelEnd >= 0 && ctx->textSelStart != ctx->textSelEnd
                    && ctx->textSelStart < (int)charWidths.size()
                    && ctx->textSelEnd < (int)charWidths.size()) {
                    int selStart = (std::min)(ctx->textSelStart, ctx->textSelEnd);
                    int selEnd = (std::max)(ctx->textSelStart, ctx->textSelEnd);

                    float selLeftF = (float)textX + charWidths[selStart];
                    float selRightF = (float)textX + charWidths[selEnd];
                    int selLeft = (int)floorf(selLeftF);
                    int selRight = (int)ceilf(selRightF);
                    int selW = selRight - selLeft;
                    int selY = (int)floorf(glyphTop);
                    int selH = (int)ceilf(glyphH);
                    if (selW > 0 && selH > 0) {
                        BLENDFUNCTION blend = { AC_SRC_OVER, 0, 100, 0 };
                        HDC tempDC = CreateCompatibleDC(backDC);
                        HBITMAP tempBmp = CreateCompatibleBitmap(backDC, selW, selH);
                        SelectObject(tempDC, tempBmp);
                        RECT tempRect = {0, 0, selW, selH};
                        FillRect(tempDC, &tempRect, ctx->gdi.textSelBrush);
                        AlphaBlend(backDC, selLeft, selY, selW, selH, tempDC, 0, 0, selW, selH, blend);
                        DeleteDC(tempDC);
                        DeleteObject(tempBmp);
                    }
                }

                // 绘制光标（闪烁效果，基于 GDI+ 字符宽度，高度对齐字形）
                // 始终缓存光标几何（即使本帧不可见），供光标闪烁/方向键的 InvalidateRect 局部刷新。
                if (ctx->textCaretPos >= 0 && ctx->textCaretPos < (int)charWidths.size()) {
                    float caretXF = (float)textX + charWidths[ctx->textCaretPos];
                    int caretX = (int)floorf(caretXF);
                    int caretY2 = (int)floorf(glyphTop);
                    int caretH = (int)ceilf(glyphH);
                    // 缓存光标矩形（backDC 坐标，含 2px 宽度），供局部刷新
                    ctx->lastCaretRect = { caretX - 1, caretY2, caretX + 3, caretY2 + caretH };
                    ctx->hasLastCaret = true;
                    if (ctx->textCaretVisible) {
                        HPEN caretPen = CreatePen(PS_SOLID, 2, textColor);
                        HGDIOBJ oldPenC = SelectObject(backDC, caretPen);
                        MoveToEx(backDC, caretX, caretY2, NULL);
                        LineTo(backDC, caretX, caretY2 + caretH);
                        SelectObject(backDC, oldPenC);
                        DeleteObject(caretPen);
                    }
                }
            }
            // 确认态和文字编辑态：文字标注的选中边框
            // - selectedTextAnnotation：已选中的标注（点击后持久保持），实线高亮边框
            // 注：文字悬浮辅助边框已移除（确认态下悬浮高亮与选中边框语义重叠且干扰视觉）。
            if (ctx->state == CS_Confirmed || ctx->state == CS_TextEditing) {
                // 已选中：实线蓝色边框（醒目）
                if (ctx->selectedTextAnnotation >= 0
                    && ctx->selectedTextAnnotation < (int)ctx->annotations.size()) {
                    RECT annRect = MeasureTextAnnotation(backDC, ctx->annotations[ctx->selectedTextAnnotation]);
                    // 缓存选中文字标注包围盒（绝对坐标），供拖动时局部刷新算旧位置
                    ctx->lastAnnotationBox = annRect;
                    ctx->hasLastAnnotationBox = true;
                    annRect.left -= ctx->virtualX;
                    annRect.top -= ctx->virtualY;
                    annRect.right -= ctx->virtualX;
                    annRect.bottom -= ctx->virtualY;
                    HGDIOBJ oldPenS = SelectObject(backDC, ctx->gdi.annTextSelPen);
                    HGDIOBJ oldBrushS = SelectObject(backDC, GetStockObject(NULL_BRUSH));
                    Rectangle(backDC, annRect.left, annRect.top, annRect.right, annRect.bottom);
                    SelectObject(backDC, oldBrushS);
                    SelectObject(backDC, oldPenS);
                }
                // 注：文字悬浮辅助边框已移除（确认态下悬浮高亮与选中边框语义重叠且干扰视觉）。
            }
            // 非文字标注的选中/悬浮边框（确认态/文字编辑态）
            // - selectedAnnotation：已选中，按工具类型差异化渲染（见下方分支）
            // 注：鼠标悬浮辅助包围盒已移除（确认态下悬浮高亮与选中边框语义重叠且干扰视觉）。
            if (ctx->state == CS_Confirmed || ctx->state == CS_TextEditing) {
                // 选中态（按工具类型差异化）：
                //   矩形：无包围盒 + 8 手柄（4 角 + 4 边中点）
                //   圆形：蓝色虚线包围盒 + 8 手柄（4 角 + 4 边中点）
                //   箭头：无包围盒 + 2 端点手柄
                //   画笔：蓝色虚线包围盒 + 无手柄（仅可整体拖动）
                // 手柄统一为白色圆形 + 红色描边（GDI+ 抗锯齿绘制，见下方）。
                if (ctx->selectedAnnotation >= 0
                    && ctx->selectedAnnotation < (int)ctx->annotations.size()
                    && ctx->annotations[ctx->selectedAnnotation].type != AT_Text) {
                    RECT box = MeasureAnnotationBounds(ctx->annotations[ctx->selectedAnnotation], backDC);
                    // 缓存选中标注包围盒（绝对坐标），供拖拽/缩放时局部刷新算旧位置
                    ctx->lastAnnotationBox = box;
                    ctx->hasLastAnnotationBox = true;
                    box.left -= ctx->virtualX; box.top -= ctx->virtualY;
                    box.right -= ctx->virtualX; box.bottom -= ctx->virtualY;
                    const Annotation& selA = ctx->annotations[ctx->selectedAnnotation];

                    // 包围盒：仅圆形/画笔画蓝色虚线框；矩形/箭头不画。
                    if (selA.type == AT_Circle || selA.type == AT_Brush) {
                        HGDIOBJ oldPenA = SelectObject(backDC, ctx->gdi.annHoverPen);
                        HGDIOBJ oldBrushA = SelectObject(backDC, GetStockObject(NULL_BRUSH));
                        Rectangle(backDC, box.left, box.top, box.right, box.bottom);
                        SelectObject(backDC, oldBrushA);
                        SelectObject(backDC, oldPenA);
                    }

                    // 收集手柄坐标（backDC 局部坐标）。画笔无手柄；箭头取 2 端点；
                    // 矩形/圆形取 8 个（4 角 + 4 边中点）。
                    int handles[8][2];
                    int handleCount = 0;
                    if (selA.type == AT_Arrow) {
                        handles[0][0] = selA.x1 - ctx->virtualX;
                        handles[0][1] = selA.y1 - ctx->virtualY;
                        handles[1][0] = selA.x2 - ctx->virtualX;
                        handles[1][1] = selA.y2 - ctx->virtualY;
                        handleCount = 2;
                    } else if (selA.type == AT_Rect || selA.type == AT_Circle) {
                        int cx = (box.left + box.right) / 2;
                        int cy = (box.top + box.bottom) / 2;
                        // 4 角
                        handles[0][0] = box.left;  handles[0][1] = box.top;
                        handles[1][0] = box.right; handles[1][1] = box.top;
                        handles[2][0] = box.left;  handles[2][1] = box.bottom;
                        handles[3][0] = box.right; handles[3][1] = box.bottom;
                        // 4 边中点
                        handles[4][0] = cx;        handles[4][1] = box.top;
                        handles[5][0] = cx;        handles[5][1] = box.bottom;
                        handles[6][0] = box.left;  handles[6][1] = cy;
                        handles[7][0] = box.right; handles[7][1] = cy;
                        handleCount = 8;
                    }
                    // 白色圆形手柄（红色描边），半径 = SC_HANDLE_SIZE/2，与原方块视觉大小一致。
                    // GDI+ 抗锯齿绘制（GDI Ellipse 边缘有硬锯齿）：白色填充 + 红色 1px 描边。
                    if (handleCount > 0) {
                        int half = SC_HANDLE_SIZE / 2;
                        Gdiplus::Graphics graphics(backDC);
                        graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
                        Gdiplus::SolidBrush cBrush(Gdiplus::Color(255, 255, 255, 255));
                        Gdiplus::Pen cPen(Gdiplus::Color(255, 229, 57, 53), 1.0f);
                        Gdiplus::GraphicsPath handlePath;
                        for (int i = 0; i < handleCount; i++) {
                            // 直径 2*half 与原 GDI Ellipse(x-half,y-half,x+half,y+half)
                            // 的像素覆盖一致（GDI 右下边 exclusive，直径 = 2*half）。
                            handlePath.AddEllipse(handles[i][0] - half, handles[i][1] - half,
                                                  2 * half, 2 * half);
                        }
                        graphics.FillPath(&cBrush, &handlePath);
                        graphics.DrawPath(&cPen, &handlePath);
                    }
                }
            }
            // 确认态边框和调整手柄最后绘制，避免马赛克及其他标注覆盖交互轮廓。
            DrawConfirmedBorder(backDC, curSelRect, ctx->gdi);
            // 拖拽选区/调整选区/文字编辑时不绘制手柄，避免遮挡；确认态/绘制标注时显示。
            if (ctx->state == CS_Confirmed || ctx->state == CS_Drawing) {
                DrawResizeHandles(backDC, curSelRect);
            }
            // 悬浮工具栏 + 粗细/颜色子菜单
            // 整体拖动选区(CS_Moving)时保持显示并实时跟随；调整选区(CS_Resizing)时仍隐藏，避免手柄附近抖动。
            // 绘制标注(CS_Drawing)/文字编辑(CS_TextEditing)时保持显示，便于随时查看/切换工具与样式。
            if (ctx->state == CS_Confirmed || ctx->state == CS_Moving || ctx->state == CS_Drawing || ctx->state == CS_TextEditing) {
                CalcToolbarPosition(curSelRect, ctx->virtualW, ctx->virtualH,
                    ctx->toolbarMetrics, curToolbarRect);
                ctx->toolbarRect = curToolbarRect;
                // hover 命中（相对坐标），确认态和文字编辑态均可更新 hover
                if (ctx->state == CS_Confirmed || ctx->state == CS_TextEditing) {
                    int mxRel = ctx->mouseX - ctx->virtualX;
                    int myRel = ctx->mouseY - ctx->virtualY;
                    ctx->hoverToolbarBtn = HitTestToolbar(mxRel, myRel, curToolbarRect, ctx->toolbarMetrics);
                } else {
                    ctx->hoverToolbarBtn = -1;
                }
                DrawToolbar(backDC, curToolbarRect, ctx->hoverToolbarBtn, ctx->activeTool,
                    ctx->gdi, ctx->toolbarMetrics, ctx->iconCache);
                int popupTool = ctx->popupTool;
                // 马赛克子菜单：模式切换 + 块大小
                if (ctx->popupOpen && popupTool == TB_Mosaic) {
                    int mpw, mph;
                    CalcMosaicPopupSize(ctx->popupMetrics, mpw, mph);
                    CalcPopupPlacement(curToolbarRect, ctx->virtualW, ctx->virtualH,
                        ctx->popupMetrics, mpw, mph, curPopupRect);
                    ctx->popupRect = curPopupRect;
                    int modeIdx = ctx->mosaicRectMode ? 1 : 0;
                    DrawMosaicPopup(backDC, curPopupRect, modeIdx, ctx->mosaicSizeIdx,
                        ctx->mosaicRadiusIdx, ctx->popupMetrics);
                }
                // 粗细/颜色子菜单：文字工具激活时始终显示（含文字编辑态）
                else if (ctx->popupOpen && CanShowStylePopupTool(popupTool)) {
                    CalcPopupPosition(curToolbarRect, ctx->virtualW, ctx->virtualH,
                        ctx->popupMetrics, curPopupRect);
                    ctx->popupRect = curPopupRect;
                    bool isText = (popupTool == TB_Text);
                    int firstIdx = isText ? ctx->fontSizeIdx : ctx->drawThickIdx;
                    DrawPopup(backDC, curPopupRect, ctx->drawColorIdx, firstIdx, isText,
                        ctx->popupMetrics);
                }
            } else {
                ctx->hoverToolbarBtn = -1;
            }
            // 确认态不绘制放大镜/尺寸标签
        } else {
            // ---- Idle/Selecting 态：原有逻辑 ----
            // 绘制选区外遮罩（微信风格，仅 Selecting 状态），选区内部保持清晰
            if (ctx->state == CS_Selecting) {
                DrawDimMask(backDC, ctx->gdi,
                    curSelRect.left, curSelRect.top, curSelRect.right, curSelRect.bottom,
                    ctx->virtualW, ctx->virtualH);
            }

            // 绘制选区或窗口尺寸标签
            if (ctx->state == CS_Selecting) {
                curLabelRect = DrawSelection(backDC, ctx->startX, ctx->startY, ctx->endX, ctx->endY,
                    ctx->virtualX, ctx->virtualY, ctx->virtualW, ctx->virtualH, ctx->gdi, ctx->panelMetrics);
            } else if (ctx->state == CS_Idle) {
                RECT screenRect;
                int ww, wh;

                if (ctx->hoveredWindow >= 0 && ctx->hoveredWindow < (int)ctx->windows.size()) {
                    // 显示悬停窗口的尺寸
                    const RECT& wr = ctx->windows[ctx->hoveredWindow].rect;
                    ww = wr.right - wr.left;
                    wh = wr.bottom - wr.top;
                    screenRect = wr;
                } else {
                    // 没有匹配到窗口时，显示当前屏幕的尺寸
                    POINT pt = { ctx->mouseX, ctx->mouseY };
                    HMONITOR hMonitor = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
                    if (hMonitor) {
                        MONITORINFO monitorInfo;
                        monitorInfo.cbSize = sizeof(MONITORINFO);
                        if (GetMonitorInfo(hMonitor, &monitorInfo)) {
                            screenRect = monitorInfo.rcMonitor;
                            ww = screenRect.right - screenRect.left;
                            wh = screenRect.bottom - screenRect.top;
                        }
                    }
                }

                curLabelRect = DrawSizeLabel(backDC, ww, wh,
                    screenRect.left - ctx->virtualX, screenRect.top - ctx->virtualY,
                    screenRect.right - ctx->virtualX, screenRect.bottom - ctx->virtualY,
                    ctx->virtualW, ctx->virtualH, ctx->gdi, ctx->panelMetrics);
            }

            // 绘制放大镜信息面板
            DrawInfoPanel(backDC, panelXRel, panelYRel, ctx->currentColor,
                ctx->memDC, ctx->virtualX, ctx->virtualY,
                ctx->mouseX, ctx->mouseY, ctx->dpiScale, ctx->gdi, ctx->panelMetrics);
        }

        // 更新脏区域追踪
        ctx->lastPanelRect = curPanelRect;
        ctx->lastSelectionRect = curSelRect;
        ctx->lastLabelRect = curLabelRect;
        ctx->lastHighlightRect = curHlRect;
        ctx->lastToolbarRect = curToolbarRect;
        ctx->lastPopupRect = curPopupRect;

        // 取消裁剪区（局部帧设置的），避免影响后续 GDI 操作
        if (dirtyRgn) {
            SelectClipRgn(backDC, NULL);
            DeleteObject(dirtyRgn);
        }

        // 后台缓冲 -> 窗口（局部帧只拷脏区域，fullFrame 拷全屏）
        BitBlt(hdc, dirtyRect.left, dirtyRect.top,
               dirtyRect.right - dirtyRect.left, dirtyRect.bottom - dirtyRect.top,
               backDC, dirtyRect.left, dirtyRect.top, SRCCOPY);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_LBUTTONDOWN: {
        if (ctx->state == CS_Idle) {
            // 开始新的框选
            ctx->startX = ctx->mouseX;
            ctx->startY = ctx->mouseY;
            ctx->endX = ctx->mouseX;
            ctx->endY = ctx->mouseY;
            ctx->state = CS_Selecting;
            ctx->needFullRedraw = true;
        } else if (ctx->state == CS_TextEditing) {
            // 文字编辑态：允许点击工具栏/子菜单操作，或点击当前输入框内选择文字
            int mxRel = ctx->mouseX - ctx->virtualX;
            int myRel = ctx->mouseY - ctx->virtualY;

            // 命中粗细/颜色子菜单：切换属性，提交文字，回到确认态
            if (ctx->popupOpen) {
                int hit = HitTestPopup(mxRel, myRel, ctx->popupRect, ctx->popupMetrics);
                if (hit > 0) {
                    // 字号切换
                    ctx->fontSizeIdx = hit - 1;
                    // 提交当前文字（如果有），应用新属性给选中的文字标注
                    if (!ctx->textBuf.empty()) {
                        Annotation textAnnotation = {};
                        textAnnotation.type = AT_Text;
                        textAnnotation.color = SC_COLOR_PRESETS[ctx->drawColorIdx];
                        textAnnotation.thickness = SC_FONT_SIZES[ctx->fontSizeIdx];
                        textAnnotation.x1 = ctx->textAnchorX;
                        textAnnotation.y1 = ctx->textAnchorY;
                        textAnnotation.text = ctx->textBuf;
                        PushAnnotationHistory(ctx);
                        ctx->annotations.push_back(textAnnotation);
                    }
                    ctx->textBuf.clear();
                    ctx->textCaretPos = 0;
                    ctx->textSelStart = -1;
                    ctx->textSelEnd = -1;
                    ctx->state = CS_Confirmed;
                    ctx->needFullRedraw = true;
                    InvalidateRect(hwnd, NULL, FALSE);
                    return 0;
                }
                if (hit < 0) {
                    // 颜色切换
                    ctx->drawColorIdx = -hit - 1;
                    // 提交当前文字
                    if (!ctx->textBuf.empty()) {
                        Annotation textAnnotation = {};
                        textAnnotation.type = AT_Text;
                        textAnnotation.color = SC_COLOR_PRESETS[ctx->drawColorIdx];
                        textAnnotation.thickness = SC_FONT_SIZES[ctx->fontSizeIdx];
                        textAnnotation.x1 = ctx->textAnchorX;
                        textAnnotation.y1 = ctx->textAnchorY;
                        textAnnotation.text = ctx->textBuf;
                        PushAnnotationHistory(ctx);
                        ctx->annotations.push_back(textAnnotation);
                    }
                    ctx->textBuf.clear();
                    ctx->textCaretPos = 0;
                    ctx->textSelStart = -1;
                    ctx->textSelEnd = -1;
                    ctx->state = CS_Confirmed;
                    ctx->needFullRedraw = true;
                    InvalidateRect(hwnd, NULL, FALSE);
                    return 0;
                }
            }

            // 点击工具栏按钮：先提交当前文字，再回到确认态
            int toolbarBtn = HitTestToolbar(mxRel, myRel, ctx->toolbarRect, ctx->toolbarMetrics);
            if (toolbarBtn >= 0 && toolbarBtn != TB_Separator1 && toolbarBtn != TB_Separator2) {
                // 先提交当前文字（如果有）
                if (!ctx->textBuf.empty()) {
                    Annotation textAnnotation = {};
                    textAnnotation.type = AT_Text;
                    textAnnotation.color = SC_COLOR_PRESETS[ctx->drawColorIdx];
                    textAnnotation.thickness = SC_FONT_SIZES[ctx->fontSizeIdx];
                    textAnnotation.x1 = ctx->textAnchorX;
                    textAnnotation.y1 = ctx->textAnchorY;
                    textAnnotation.text = ctx->textBuf;
                    PushAnnotationHistory(ctx);
                    ctx->annotations.push_back(textAnnotation);
                }
                ctx->textBuf.clear();
                ctx->textCaretPos = 0;
                ctx->textSelStart = -1;
                ctx->textSelEnd = -1;
                ctx->state = CS_Confirmed;
                ctx->needFullRedraw = true;
                InvalidateRect(hwnd, NULL, FALSE);
                return 0;
            }

            // 点击当前输入框内：开始文字选择或移动光标
            // 命中区与绘制边框完全一致（用 GDI+ 紧凑度量），否则点边框附近算"框外"会提交文字。
            int textX = ctx->textAnchorX - ctx->virtualX;
            int textY = ctx->textAnchorY - ctx->virtualY;
            int fontPx = SC_FONT_SIZES[ctx->fontSizeIdx];

            float offX = 0, offY = 0, textW = 0, textH = 0;
            MeasureTextGdip(ctx->backDC, ctx->textBuf, fontPx, offX, offY, textW, textH);
            float glyphLeft = (float)textX + offX;
            float glyphTop  = (float)textY + offY;
            float glyphW = (textW > 20.0f || !ctx->textBuf.empty()) ? textW : 20.0f;
            float glyphH = (textH > 0 ? textH : (float)fontPx);

            const float padding = 4.0f;
            RECT inputBox = {
                (int)floorf(glyphLeft - padding),
                (int)floorf(glyphTop - padding),
                (int)ceilf(glyphLeft + glyphW + padding),
                (int)ceilf(glyphTop + glyphH + padding)
            };

            if (PointInRect(mxRel, myRel, inputBox)) {
                // 点击输入框内：计算光标位置并开始选择
                int caretPos = CalcCaretPosFromMouse(ctx->backDC, ctx->textBuf, fontPx, textX, mxRel);

                ctx->textCaretPos = caretPos;
                ctx->textSelStart = caretPos;
                ctx->textSelEnd = caretPos;
                ctx->textDraggingSelection = true;
                InvalidateRect(hwnd, NULL, FALSE);
                return 0;
            }

            // 点击选区内其他位置：提交当前文字，开始新的输入
            if (PointInRect(ctx->mouseX, ctx->mouseY, ctx->selection)) {
                if (!ctx->textBuf.empty()) {
                    Annotation textAnnotation = {};
                    textAnnotation.type = AT_Text;
                    textAnnotation.color = SC_COLOR_PRESETS[ctx->drawColorIdx];
                    textAnnotation.thickness = SC_FONT_SIZES[ctx->fontSizeIdx];
                    textAnnotation.x1 = ctx->textAnchorX;
                    textAnnotation.y1 = ctx->textAnchorY;
                    textAnnotation.text = ctx->textBuf;
                    PushAnnotationHistory(ctx);
                    ctx->annotations.push_back(textAnnotation);
                }
                ctx->textBuf.clear();
                ctx->textAnchorX = ctx->mouseX;
                ctx->textAnchorY = ctx->mouseY;
                ctx->textCaretPos = 0;
                ctx->textSelStart = -1;
                ctx->textSelEnd = -1;
                ctx->needFullRedraw = true;
                InvalidateRect(hwnd, NULL, FALSE);
                return 0;
            }

            // 点击选区外：提交文字并退出文字编辑态
            if (!ctx->textBuf.empty()) {
                Annotation textAnnotation = {};
                textAnnotation.type = AT_Text;
                textAnnotation.color = SC_COLOR_PRESETS[ctx->drawColorIdx];
                textAnnotation.thickness = SC_FONT_SIZES[ctx->fontSizeIdx];
                textAnnotation.x1 = ctx->textAnchorX;
                textAnnotation.y1 = ctx->textAnchorY;
                textAnnotation.text = ctx->textBuf;
                PushAnnotationHistory(ctx);
                ctx->annotations.push_back(textAnnotation);
            }
            ctx->textBuf.clear();
            ctx->textCaretPos = 0;
            ctx->textSelStart = -1;
            ctx->textSelEnd = -1;
            ctx->state = CS_Confirmed;
            ctx->needFullRedraw = true;
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        } else if (ctx->state == CS_Confirmed) {
            // 实时命中测试（不依赖 hover 缓存值）。
            // 远程桌面（RDP）下 WM_MOUSEMOVE 常被节流/合并，hoverToolbarBtn/hoveredTextAnnotation
            // 可能滞后于实际鼠标位置。若继续用缓存判断，点击文字标注时可能误命中残留的工具栏
            // 索引并提前 return，导致文字选中/拖拽分支永远执行不到。
            int mxRel = ctx->mouseX - ctx->virtualX;
            int myRel = ctx->mouseY - ctx->virtualY;
            int b = HitTestToolbar(mxRel, myRel, ctx->toolbarRect, ctx->toolbarMetrics);

            // 点击工具栏按钮（实时命中）
            if (b >= 0 && b != TB_Separator1 && b != TB_Separator2) {
                // 选中态保留规则（与 Figma/PowerPoint 一致）：
                //   - 点击"与已选中标注同类型"的绘制工具按钮（含文字）-> 保留选中，仅切换子菜单开合
                //     （例：选中矩形后点矩形按钮 -> 关闭/重开粗细颜色子菜单，矩形仍选中）。
                //   - 点击"不同类型"的绘制工具按钮 -> 切换到该绘制工具，取消当前选中
                //     （子菜单参数将用于后续绘制，不再作用于旧选中元素，避免参数错配）。
                //   - 点击确认/取消/保存/撤销/重做等"与选中元素无关"的按钮 -> 取消所有选中。
                bool isDrawToolBtn = IsVectorTool(b) || b == TB_Mosaic || b == TB_Text || b == TB_Drag;
                bool matchesSelection = false;
                if (isDrawToolBtn) {
                    if (ctx->selectedAnnotation >= 0 && ctx->selectedAnnotation < (int)ctx->annotations.size()
                        && AnnotationTypeToTool(ctx->annotations[ctx->selectedAnnotation].type) == b) {
                        matchesSelection = true;
                    }
                    if (ctx->selectedTextAnnotation >= 0 && ctx->selectedTextAnnotation < (int)ctx->annotations.size()
                        && b == TB_Text) {
                        matchesSelection = true;
                    }
                }
                bool hadAnnSel = (ctx->selectedAnnotation >= 0);
                bool hadTextSel = (ctx->selectedTextAnnotation >= 0);
                if (!matchesSelection) {
                    // 不匹配选中项：取消所有选中（切换到其它工具或执行无关操作）。
                    // 切换到不同类型工具时 activeTool 必变，高亮按钮位移 + popup 可能切换，
                    // 故脏区须含工具栏+popup（includeToolbar=true）。需在清状态前算脏区。
                    RECT dirty = CalcSelectionDirty(ctx, true /*includeToolbar*/);
                    ctx->selectedAnnotation = -1;
                    ctx->hoveredAnnotation = -1;
                    ctx->selectedTextAnnotation = -1;
                    ctx->hoveredTextAnnotation = -1;
                    if (hadAnnSel || hadTextSel) {
                        if (IsValidRect(dirty)) {
                            InvalidateRect(hwnd, &dirty, FALSE);
                        } else {
                            InvalidateRect(hwnd, NULL, FALSE);
                        }
                    }
                }
                // 确定：提取选区并完成截图
                if (b == TB_Confirm) {
                    ScreenshotResult* result = ExtractRegionResult(ctx->memDC, ctx->selection,
                        ctx->virtualX, ctx->virtualY, ctx->dpiScale, ctx->annotations);
                    if (g_screenshotTsfn != nullptr) {
                        napi_call_threadsafe_function(g_screenshotTsfn, result, napi_tsfn_nonblocking);
                    }
                    ctx->state = CS_Done;
                    DestroyWindow(hwnd);
                    return 0;
                }
                // 取消：回调失败并关闭
                if (b == TB_Cancel) {
                    if (g_screenshotTsfn != nullptr) {
                        ScreenshotResult* result = new ScreenshotResult();
                        result->success = false;
                        napi_call_threadsafe_function(g_screenshotTsfn, result, napi_tsfn_nonblocking);
                    }
                    ctx->state = CS_Cancelled;
                    DestroyWindow(hwnd);
                    return 0;
                }
                // 矢量工具：切换激活态 + 打开/关闭粗细颜色子菜单
                if (b == TB_Rect || b == TB_Circle || b == TB_Arrow || b == TB_Brush) {
                    if (ctx->activeTool == b) {
                        // 再次点同一工具：关闭工具与子菜单
                        ctx->activeTool = -1;
                        ctx->popupTool = -1;
                        ctx->popupOpen = false;
                    } else {
                        ctx->activeTool = b;
                        ctx->popupTool = b;
                        ctx->popupOpen = true;
                        // 匹配选中项时回显该标注的粗细/颜色到子菜单
                        if (matchesSelection && ctx->selectedAnnotation >= 0
                            && ctx->selectedAnnotation < (int)ctx->annotations.size()) {
                            const Annotation& selA = ctx->annotations[ctx->selectedAnnotation];
                            for (int i = 0; i < SC_THICK_COUNT; i++) {
                                if (SC_THICK_PRESETS[i] == selA.thickness) { ctx->drawThickIdx = i; break; }
                            }
                            for (int i = 0; i < SC_COLOR_COUNT; i++) {
                                if (SC_COLOR_PRESETS[i] == selA.color) { ctx->drawColorIdx = i; break; }
                            }
                        }
                    }
                    InvalidateRect(hwnd, NULL, FALSE);
                    return 0;
                }
                // 文字工具：切换激活态 + 打开/关闭子菜单（字号+颜色）
                if (b == TB_Text) {
                    if (ctx->activeTool == b) {
                        // 再次点同一工具：关闭工具与子菜单
                        ctx->activeTool = -1;
                        ctx->popupTool = -1;
                        ctx->popupOpen = false;
                    } else {
                        ctx->activeTool = b;
                        ctx->popupTool = b;
                        ctx->popupOpen = true;
                        // 匹配选中文字时回显该标注的字号/颜色到子菜单
                        if (matchesSelection && ctx->selectedTextAnnotation >= 0
                            && ctx->selectedTextAnnotation < (int)ctx->annotations.size()) {
                            const Annotation& selT = ctx->annotations[ctx->selectedTextAnnotation];
                            for (int i = 0; i < SC_FONT_COUNT; i++) {
                                if (SC_FONT_SIZES[i] == selT.thickness) { ctx->fontSizeIdx = i; break; }
                            }
                            for (int i = 0; i < SC_COLOR_COUNT; i++) {
                                if (SC_COLOR_PRESETS[i] == selT.color) { ctx->drawColorIdx = i; break; }
                            }
                        }
                    }
                    InvalidateRect(hwnd, NULL, FALSE);
                    return 0;
                }
                // 马赛克工具：切换激活态 + 打开/关闭子菜单（模式+块大小）
                if (b == TB_Mosaic) {
                    if (ctx->activeTool == b) {
                        // 再次点同一工具：关闭工具与子菜单
                        ctx->activeTool = -1;
                        ctx->popupTool = -1;
                        ctx->popupOpen = false;
                    } else {
                        ctx->activeTool = b;
                        ctx->popupTool = b;
                        ctx->popupOpen = true;
                    }
                    InvalidateRect(hwnd, NULL, FALSE);
                    return 0;
                }
                if (b == TB_Drag) {
                    if (ctx->activeTool == b) {
                        ctx->activeTool = -1;
                        ctx->popupTool = -1;
                        ctx->popupOpen = false;
                    } else {
                        ctx->activeTool = b;
                        if (ctx->selectedTextAnnotation >= 0 && ctx->selectedTextAnnotation < (int)ctx->annotations.size()) {
                            ctx->popupTool = TB_Text;
                            const Annotation& selT = ctx->annotations[ctx->selectedTextAnnotation];
                            for (int i = 0; i < SC_FONT_COUNT; i++) {
                                if (SC_FONT_SIZES[i] == selT.thickness) { ctx->fontSizeIdx = i; break; }
                            }
                            for (int i = 0; i < SC_COLOR_COUNT; i++) {
                                if (SC_COLOR_PRESETS[i] == selT.color) { ctx->drawColorIdx = i; break; }
                            }
                            ctx->popupOpen = true;
                        } else if (ctx->selectedAnnotation >= 0 && ctx->selectedAnnotation < (int)ctx->annotations.size()) {
                            const Annotation& selA = ctx->annotations[ctx->selectedAnnotation];
                            ctx->popupTool = AnnotationTypeToTool(selA.type);
                            for (int i = 0; i < SC_THICK_COUNT; i++) {
                                if (SC_THICK_PRESETS[i] == selA.thickness) { ctx->drawThickIdx = i; break; }
                            }
                            for (int i = 0; i < SC_COLOR_COUNT; i++) {
                                if (SC_COLOR_PRESETS[i] == selA.color) { ctx->drawColorIdx = i; break; }
                            }
                            ctx->popupOpen = CanShowStylePopupTool(ctx->popupTool);
                        } else {
                            ctx->popupTool = -1;
                            ctx->popupOpen = false;
                        }
                    }
                    InvalidateRect(hwnd, NULL, FALSE);
                    return 0;
                }
                // 撤销：恢复上一份标注快照
                if (b == TB_Undo) {
                    if (UndoAnnotations(ctx)) {
                        InvalidateRect(hwnd, NULL, FALSE);
                    }
                    return 0;
                }
                // 重做：恢复下一份标注快照
                if (b == TB_Redo) {
                    if (RedoAnnotations(ctx)) {
                        InvalidateRect(hwnd, NULL, FALSE);
                    }
                    return 0;
                }
                // 保存到本地：弹出系统保存对话框，保存为 PNG 后关闭截图
                if (b == TB_Save) {
                    std::wstring filePath = PromptSaveFilePath(hwnd);
                    if (!filePath.empty()) {
                        bool saved = SaveRegionToPngFile(ctx->memDC, ctx->selection,
                            ctx->virtualX, ctx->virtualY, ctx->dpiScale,
                            ctx->annotations, filePath);
                        // 无论保存成功与否，均关闭截图窗口（用户已选择保存路径）
                        // 通过回调告知 JS 结果（成功/失败），不回传路径
                        ScreenshotResult* result = new ScreenshotResult();
                        result->success = saved;
                        if (saved) {
                            result->x = ctx->selection.left;
                            result->y = ctx->selection.top;
                            result->x2 = ctx->selection.right;
                            result->y2 = ctx->selection.bottom;
                            result->width = ctx->selection.right - ctx->selection.left;
                            result->height = ctx->selection.bottom - ctx->selection.top;
                        }
                        if (g_screenshotTsfn != nullptr) {
                            napi_call_threadsafe_function(g_screenshotTsfn, result, napi_tsfn_nonblocking);
                        }
                        ctx->state = CS_Done;
                        DestroyWindow(hwnd);
                    }
                    // 用户取消保存对话框：不关闭，留在编辑态
                    return 0;
                }
                return 0;
            }

            // 命中马赛克子菜单（模式切换 + 块大小 + 涂抹半径）
            // 马赛克标注本身不可选中，且这些选项只影响后续绘制、不作用于已选中的其它元素，
            // 故均属于"非拖拽/resize 动作"-> 取消当前选中态。
            if (ctx->popupOpen && ctx->popupTool == TB_Mosaic) {
                int hit = HitTestMosaicPopup(mxRel, myRel, ctx->popupRect, ctx->popupMetrics);
                // 清除选中态并在清空前计算脏区（马赛克 popup 不改 activeTool，仅清覆盖物选中）。
                // 返回 dirty（backDC 坐标）；调用方据 IsValidRect 决定局部或全屏 invalidate。
                auto clearSel = [&]() {
                    RECT dirty = CalcSelectionDirty(ctx, false /*includeToolbar*/);
                    ctx->selectedAnnotation = -1;
                    ctx->selectedTextAnnotation = -1;
                    ctx->hoveredAnnotation = -1;
                    ctx->hoveredTextAnnotation = -1;
                    return dirty;
                };
                if (hit == 1) {
                    ctx->mosaicRectMode = false;  // 涂抹模式
                    { RECT d = clearSel();
                      if (IsValidRect(d)) InvalidateRect(hwnd, &d, FALSE); else InvalidateRect(hwnd, NULL, FALSE); }
                    return 0;
                }
                if (hit == 2) {
                    ctx->mosaicRectMode = true;   // 框选模式
                    { RECT d = clearSel();
                      if (IsValidRect(d)) InvalidateRect(hwnd, &d, FALSE); else InvalidateRect(hwnd, NULL, FALSE); }
                    return 0;
                }
                if (hit >= 101 && hit < 200) {
                    ctx->mosaicSizeIdx = hit - 101;
                    { RECT d = clearSel();
                      if (IsValidRect(d)) InvalidateRect(hwnd, &d, FALSE); else InvalidateRect(hwnd, NULL, FALSE); }
                    return 0;
                }
                if (hit >= 201) {
                    ctx->mosaicRadiusIdx = hit - 201;
                    { RECT d = clearSel();
                      if (IsValidRect(d)) InvalidateRect(hwnd, &d, FALSE); else InvalidateRect(hwnd, NULL, FALSE); }
                    return 0;
                }
            }

            // 命中粗细/颜色子菜单
            if (ctx->popupOpen) {
                int hit = HitTestPopup(mxRel, myRel, ctx->popupRect, ctx->popupMetrics);
                if (hit > 0) {
                    // 第一组：文字工具时为字号索引，矢量工具时为粗细索引
                    if (ctx->popupTool == TB_Text) {
                        ctx->fontSizeIdx = hit - 1;
                        // 如果选中了文字标注，修改其字号（选中文字时子菜单改动作用于该标注）
                        if (ctx->selectedTextAnnotation >= 0 && ctx->selectedTextAnnotation < (int)ctx->annotations.size()) {
                            int newSize = SC_FONT_SIZES[ctx->fontSizeIdx];
                            if (ctx->annotations[ctx->selectedTextAnnotation].thickness != newSize) {
                                PushAnnotationHistory(ctx);
                                ctx->annotations[ctx->selectedTextAnnotation].thickness = newSize;
                                ctx->annotations[ctx->selectedTextAnnotation].textCacheValid = false;
                            }
                        }
                    } else {
                        ctx->drawThickIdx = hit - 1;
                        // 矢量工具改粗细：若已选中矢量标注，则作用于该标注（保持选中）；
                        // 否则作用于后续绘制。
                        if (ctx->selectedAnnotation >= 0 && ctx->selectedAnnotation < (int)ctx->annotations.size()) {
                            int newThickness = SC_THICK_PRESETS[ctx->drawThickIdx];
                            if (ctx->annotations[ctx->selectedAnnotation].thickness != newThickness) {
                                PushAnnotationHistory(ctx);
                                ctx->annotations[ctx->selectedAnnotation].thickness = newThickness;
                            }
                        }
                    }
                    InvalidateRect(hwnd, NULL, FALSE);
                    return 0;
                }
                if (hit < 0) {
                    ctx->drawColorIdx = -hit - 1;
                    // 如果选中了文字标注，修改其颜色（选中文字时颜色改动作用于该标注）
                    if (ctx->popupTool == TB_Text && ctx->selectedTextAnnotation >= 0
                        && ctx->selectedTextAnnotation < (int)ctx->annotations.size()) {
                        COLORREF newColor = SC_COLOR_PRESETS[ctx->drawColorIdx];
                        if (ctx->annotations[ctx->selectedTextAnnotation].color != newColor) {
                            PushAnnotationHistory(ctx);
                            ctx->annotations[ctx->selectedTextAnnotation].color = newColor;
                        }
                    }
                    // 矢量工具改颜色：若已选中矢量标注，则作用于该标注（保持选中）；
                    // 否则作用于后续绘制。
                    if (ctx->popupTool != TB_Text && ctx->selectedAnnotation >= 0
                        && ctx->selectedAnnotation < (int)ctx->annotations.size()) {
                        COLORREF newColor = SC_COLOR_PRESETS[ctx->drawColorIdx];
                        if (ctx->annotations[ctx->selectedAnnotation].color != newColor) {
                            PushAnnotationHistory(ctx);
                            ctx->annotations[ctx->selectedAnnotation].color = newColor;
                        }
                    }
                    InvalidateRect(hwnd, NULL, FALSE);
                    return 0;
                }
            }

            // ===== 通用标注交互（选中/拖拽/缩放，优先于绘制工具）=====
            // 1) 已选中标注的四角 resize 手柄命中 -> 进入缩放
            //    （须在普通命中之前：手柄贴在选中框角上，可能与标注本体重叠）
            //    不复用 CS_Resizing（该状态会改 ctx->selection 选区），用 resizingAnnotation 标志
            //    在 CS_Confirmed 下独立处理，与文字拖拽(draggingTextAnnotation)机制对称。
            if (ctx->selectedAnnotation >= 0 && ctx->selectedAnnotation < (int)ctx->annotations.size()
                && !IsDragTool(ctx->activeTool)) {
                Annotation& sel = ctx->annotations[ctx->selectedAnnotation];
                RECT box = MeasureAnnotationBounds(sel, ctx->backDC);
                // 按类型命中缩放手柄：箭头=2 端点；矩形/圆=8 手柄；画笔=无（仅可拖动）
                int handle = HitTestAnnotationResizeHandle(sel, ctx->mouseX, ctx->mouseY, ctx->backDC);
                if (handle != RH_None) {
                    ctx->resizingAnnotation = ctx->selectedAnnotation;
                    ctx->annotationResizeHandle = handle;
                    ctx->annotationDragStartX = ctx->mouseX;
                    ctx->annotationDragStartY = ctx->mouseY;
                    ctx->annotationResizeStartBox = box;
                    ctx->dragStartAnnotation = ctx->annotations[ctx->selectedAnnotation];
                    ctx->annotationOpHistoryPushed = false;
                    ctx->needFullRedraw = true;
                    return 0;
                }
            }
            // 2) 文字标注命中 -> 优先选中并可拖动
            //    文字与画笔/矩形等覆盖物重叠时，优先进入文字选中逻辑，避免被非文字命中分支吞掉。
            int hitText = HitTestTextAnnotations(ctx->annotations, ctx->mouseX, ctx->mouseY, ctx->backDC);
            if (hitText >= 0) {
                if (ctx->activeTool == TB_Text) {
                    // 选中文字标注，保持确认态。
                    // selectedTextAnnotation 持久保持选中态（与 hover 解耦），鼠标移开仍高亮，
                    // 直到点击空白或进入其他操作才清除。
                    // 精确脏区：清掉上一个选中项（可能为非文字/另一文字）的选中边框。
                    // 须在赋新选中前算脏区，否则读不到旧选中项的包围盒。
                    RECT dirty = CalcSelectionDirty(ctx, false /*includeToolbar*/);
                    ctx->selectedAnnotation = -1;
                    ctx->hoveredAnnotation = -1;
                    ctx->selectedTextAnnotation = hitText;
                    ctx->hoveredTextAnnotation = hitText;
                    // 工具栏回显：文字工具保持高亮，并继续回显文字参数。
                    ctx->activeTool = TB_Text;
                    ctx->popupTool = TB_Text;
                    int curSize = ctx->annotations[hitText].thickness;
                    for (int i = 0; i < SC_FONT_COUNT; i++) {
                        if (SC_FONT_SIZES[i] == curSize) { ctx->fontSizeIdx = i; break; }
                    }
                    COLORREF curColor = ctx->annotations[hitText].color;
                    for (int i = 0; i < SC_COLOR_COUNT; i++) {
                        if (SC_COLOR_PRESETS[i] == curColor) { ctx->drawColorIdx = i; break; }
                    }
                    ctx->popupOpen = true;
                    ctx->draggingTextAnnotation = hitText;
                    ctx->textDragStartX = ctx->mouseX;
                    ctx->textDragStartY = ctx->mouseY;
                    ctx->dragStartX = ctx->annotations[hitText].x1;
                    ctx->dragStartY = ctx->annotations[hitText].y1;
                    ctx->annotationOpHistoryPushed = false;
                    if (IsValidRect(dirty)) {
                        InvalidateRect(hwnd, &dirty, FALSE);
                    } else {
                        InvalidateRect(hwnd, NULL, FALSE);
                    }
                    return 0;
                } else {
                    // 非文字工具或空态下：切换选中目标到文字。
                    const int handleMargin = SC_HANDLE_SIZE / 2 + 4;
                    RECT dirty = CalcSelectionDirty(ctx, true /*includeToolbar*/);
                    RECT newBox = MeasureTextAnnotation(ctx->backDC, ctx->annotations[hitText]);
                    newBox.left -= ctx->virtualX; newBox.top -= ctx->virtualY;
                    newBox.right -= ctx->virtualX; newBox.bottom -= ctx->virtualY;
                    dirty = UnionRectSafe(dirty, InflateRectBy(newBox, handleMargin));
                    ctx->draggingTextAnnotation = hitText;
                    ctx->hoveredAnnotation = -1;
                    ctx->selectedAnnotation = -1;
                    ctx->hoveredTextAnnotation = hitText;
                    ctx->selectedTextAnnotation = hitText;
                    // 工具栏回显：切到文字工具按钮，打开字号/颜色子菜单，同步该标注参数。
                    ctx->activeTool = TB_Text;
                    ctx->popupTool = TB_Text;
                    ctx->popupOpen = true;
                    int curSize = ctx->annotations[hitText].thickness;
                    for (int i = 0; i < SC_FONT_COUNT; i++) {
                        if (SC_FONT_SIZES[i] == curSize) { ctx->fontSizeIdx = i; break; }
                    }
                    COLORREF curColor = ctx->annotations[hitText].color;
                    for (int i = 0; i < SC_COLOR_COUNT; i++) {
                        if (SC_COLOR_PRESETS[i] == curColor) { ctx->drawColorIdx = i; break; }
                    }
                    ctx->textDragStartX = ctx->mouseX;
                    ctx->textDragStartY = ctx->mouseY;
                    ctx->dragStartX = ctx->annotations[hitText].x1;
                    ctx->dragStartY = ctx->annotations[hitText].y1;
                    ctx->annotationOpHistoryPushed = false;
                    if (IsValidRect(dirty)) {
                        InvalidateRect(hwnd, &dirty, FALSE);
                    } else {
                        InvalidateRect(hwnd, NULL, FALSE);
                    }
                    return 0;
                }
            }

            // 3) 任意非文字标注命中 -> 选中并可拖拽
            //    工具激活时也优先选中已有对象（与 Figma/PowerPoint 一致），点空白才绘制。
            int hitAnn = HitTestAnnotation(ctx->annotations, ctx->mouseX, ctx->mouseY, ctx->backDC);
            if (hitAnn >= 0 && ctx->annotations[hitAnn].type != AT_Text) {
                // 切换选中目标：脏区 = 旧选中项（清掉其边框/手柄）∪ 新目标（显示新边框/手柄）
                // ∪ 工具栏+popup（activeTool 可能变化导致高亮按钮位移）。
                // 须在改 selectedAnnotation 之前算旧选中脏区。
                const int handleMargin = SC_HANDLE_SIZE / 2 + 4;
                RECT dirty = CalcSelectionDirty(ctx, true /*includeToolbar*/);
                RECT newBox = MeasureAnnotationBounds(ctx->annotations[hitAnn], ctx->backDC);
                newBox.left -= ctx->virtualX; newBox.top -= ctx->virtualY;
                newBox.right -= ctx->virtualX; newBox.bottom -= ctx->virtualY;
                dirty = UnionRectSafe(dirty, InflateRectBy(newBox, handleMargin));
                ctx->selectedAnnotation = hitAnn;
                ctx->hoveredAnnotation = hitAnn;
                // 与文字选中互斥：选中非文字时清文字选中
                ctx->selectedTextAnnotation = -1;
                ctx->hoveredTextAnnotation = -1;
                // 工具栏回显：保持原有回显逻辑。
                // 同步当前粗细/颜色索引为该标注的值，子菜单高亮与选中元素一致（回显参数）。
                const Annotation& hitA = ctx->annotations[hitAnn];
                ctx->activeTool = AnnotationTypeToTool(hitA.type);
                ctx->popupTool = ctx->activeTool;
                ctx->popupOpen = true;
                for (int i = 0; i < SC_THICK_COUNT; i++) {
                    if (SC_THICK_PRESETS[i] == hitA.thickness) { ctx->drawThickIdx = i; break; }
                }
                for (int i = 0; i < SC_COLOR_COUNT; i++) {
                    if (SC_COLOR_PRESETS[i] == hitA.color) { ctx->drawColorIdx = i; break; }
                }
                // 进入拖拽模式：按下即可移动（按下未移动时，松开仅保持选中态）
                ctx->draggingAnnotation = hitAnn;
                ctx->annotationDragStartX = ctx->mouseX;
                ctx->annotationDragStartY = ctx->mouseY;
                ctx->dragStartAnnotation = ctx->annotations[hitAnn];
                ctx->annotationOpHistoryPushed = false;
                // 触发重绘：needFullRedraw 仅是 WM_PAINT 内的提示，必须有 InvalidateRect 才会触发 WM_PAINT。
                if (IsValidRect(dirty)) {
                    InvalidateRect(hwnd, &dirty, FALSE);
                } else {
                    InvalidateRect(hwnd, NULL, FALSE);
                }
                return 0;
            }

            // 矢量工具激活时，点击选区内部/工具栏外 -> 开始绘制
            // 子菜单保持打开，绘制中可继续看到当前粗细/颜色。
            // 注意：上面通用标注命中分支已拦截「点中已有标注」的情况，此处仅点空白才进入。
            if (IsVectorTool(ctx->activeTool) && PointInRect(ctx->mouseX, ctx->mouseY, ctx->selection)) {
                // 点空白绘制新标注：清除已有选中态
                ctx->selectedAnnotation = -1;
                ctx->hoveredAnnotation = -1;
                ctx->hasCurDrawing = true;
                ctx->curDrawing = {};
                ctx->curDrawing.type = ToolToAnnotationType(ctx->activeTool);
                ctx->curDrawing.color = SC_COLOR_PRESETS[ctx->drawColorIdx];
                ctx->curDrawing.thickness = SC_THICK_PRESETS[ctx->drawThickIdx];
                // 起点用绝对虚拟屏幕坐标
                ctx->curDrawing.x1 = ctx->mouseX;
                ctx->curDrawing.y1 = ctx->mouseY;
                ctx->curDrawing.x2 = ctx->curDrawing.x1;
                ctx->curDrawing.y2 = ctx->curDrawing.y1;
                if (ctx->curDrawing.type == AT_Brush) {
                    POINT p = { ctx->curDrawing.x1, ctx->curDrawing.y1 };
                    ctx->curDrawing.pts.push_back(p);
                }
                ctx->state = CS_Drawing;
                ctx->needFullRedraw = true;
                return 0;
            }

            // 马赛克工具激活时，点击选区内部 -> 开始马赛克绘制
            // 子菜单保持打开，绘制中可继续看到当前模式/块大小。
            if (ctx->activeTool == TB_Mosaic && PointInRect(ctx->mouseX, ctx->mouseY, ctx->selection)) {
                // 点空白绘制新标注：清除已有选中态
                ctx->selectedAnnotation = -1;
                ctx->hoveredAnnotation = -1;
                ctx->hasCurDrawing = true;
                ctx->curDrawing = {};
                ctx->curDrawing.type = AT_Mosaic;
                ctx->curDrawing.color = 0;  // 马赛克无颜色
                ctx->curDrawing.mosaicRect = ctx->mosaicRectMode;
                ctx->curDrawing.mosaicSize = SC_MOSAIC_SIZES[ctx->mosaicSizeIdx];
                ctx->curDrawing.brushRadius = SC_MOSAIC_RADIUS[ctx->mosaicRadiusIdx];
                ctx->curDrawing.x1 = ctx->mouseX;
                ctx->curDrawing.y1 = ctx->mouseY;
                ctx->curDrawing.x2 = ctx->curDrawing.x1;
                ctx->curDrawing.y2 = ctx->curDrawing.y1;
                if (!ctx->mosaicRectMode) {
                    // 涂抹模式：记录路径起点（揭示由 WM_PAINT 统一处理）
                    POINT p = { ctx->curDrawing.x1, ctx->curDrawing.y1 };
                    ctx->curDrawing.pts.push_back(p);
                }
                ctx->state = CS_Drawing;
                ctx->needFullRedraw = true;
                return 0;
            }

            // 文字工具激活时：点击选区内空白 -> 进入输入态
            if (ctx->activeTool == TB_Text) {
                if (PointInRect(ctx->mouseX, ctx->mouseY, ctx->selection)) {
                    ctx->textBuf.clear();
                    ctx->textAnchorX = ctx->mouseX;
                    ctx->textAnchorY = ctx->mouseY;
                    ctx->textCaretPos = 0;
                    ctx->textSelStart = -1;
                    ctx->textSelEnd = -1;
                    // 精确脏区：清掉上一个选中项的选中边框/手柄。须在清状态前算。
                    RECT dirty = CalcSelectionDirty(ctx, false /*includeToolbar*/);
                    ctx->state = CS_TextEditing;
                    // 进入输入态时清除文字/非文字标注选中，避免残留选中边框
                    ctx->hoveredTextAnnotation = -1;
                    ctx->selectedTextAnnotation = -1;
                    ctx->selectedAnnotation = -1;
                    ctx->hoveredAnnotation = -1;
                    // 保持子菜单打开，清空绘制标志
                    // ctx->popupOpen 保持不变
                    ctx->hasCurDrawing = false;
                    // 文字工具保持激活，避免工具栏视觉状态丢失
                    // ctx->activeTool 保持为 TB_Text
                    // 不置 needFullRedraw：进入编辑态仅改变选中边框，用局部脏区即可，避免全屏重绘。
                    if (IsValidRect(dirty)) {
                        InvalidateRect(hwnd, &dirty, FALSE);
                    } else {
                        InvalidateRect(hwnd, NULL, FALSE);
                    }
                    return 0;
                }
            }

            // 命中调整手柄 -> 进入 Resizing
            int h = HitTestHandle(ctx->mouseX, ctx->mouseY, ctx->selection);
            if (h != RH_None) {
                ctx->selectedTextAnnotation = -1;  // 进入手柄调整，清除文字选中
                ctx->selectedAnnotation = -1;      // 清除非文字标注选中
                ctx->hoveredAnnotation = -1;
                ctx->resizeHandle = h;
                ctx->dragStartX = ctx->mouseX;
                ctx->dragStartY = ctx->mouseY;
                ctx->dragStartSelection = ctx->selection;
                ctx->state = CS_Resizing;
                ctx->needFullRedraw = true;
                return 0;
            }
            // 点击选区内部 -> 整体拖动（已有标注内容时禁止，避免标注与背景错位）
            if (PointInRect(ctx->mouseX, ctx->mouseY, ctx->selection)) {
                if (!ctx->annotations.empty()) {
                    // 已有预处理内容：只能通过手柄改范围，不允许整体拖动。
                    // 点空白（选中态的拖动被禁止）-> 取消当前选中。
                    if (ctx->selectedAnnotation >= 0 || ctx->selectedTextAnnotation >= 0) {
                        // 精确脏区：清掉选中项的选中边框/手柄。须在清状态前算。
                        RECT dirty = CalcSelectionDirty(ctx, false /*includeToolbar*/);
                        ctx->selectedTextAnnotation = -1;
                        ctx->selectedAnnotation = -1;
                        ctx->hoveredAnnotation = -1;
                        ctx->hoveredTextAnnotation = -1;
                        if (IsValidRect(dirty)) {
                            InvalidateRect(hwnd, &dirty, FALSE);
                        } else {
                            InvalidateRect(hwnd, NULL, FALSE);
                        }
                    }
                    return 0;
                }
                ctx->selectedTextAnnotation = -1;  // 进入选区拖动，清除文字选中
                ctx->selectedAnnotation = -1;      // 清除非文字标注选中
                ctx->hoveredAnnotation = -1;
                ctx->hoveredTextAnnotation = -1;
                ctx->dragStartX = ctx->mouseX;
                ctx->dragStartY = ctx->mouseY;
                ctx->dragStartSelection = ctx->selection;
                ctx->state = CS_Moving;
                ctx->needFullRedraw = true;
                return 0;
            }
            // 点击选区外空白：清除文字/非文字标注选中，进入确认态后不可重新框选，忽略点击
            {
                RECT dirty = CalcSelectionDirty(ctx, false /*includeToolbar*/);
                ctx->selectedTextAnnotation = -1;
                ctx->selectedAnnotation = -1;
                ctx->hoveredAnnotation = -1;
                ctx->hoveredTextAnnotation = -1;
                if (IsValidRect(dirty)) {
                    InvalidateRect(hwnd, &dirty, FALSE);
                } else {
                    InvalidateRect(hwnd, NULL, FALSE);
                }
            }
            return 0;
        }
        return 0;
    }

    case WM_MOUSEMOVE: {
        POINT pt;
        GetCursorPos(&pt);
        bool moved = (pt.x != ctx->mouseX || pt.y != ctx->mouseY);
        ctx->mouseX = pt.x;
        ctx->mouseY = pt.y;
        // P2 优化：currentColor 仅用于放大镜信息面板（DrawInfoPanel），而该面板只
        // 在 CS_Idle/CS_Selecting 态绘制。其余状态读不到 currentColor，故取色惰性化，
        // 仅在这两种态更新像素色，避免每帧无谓的 GetPixel（DC 锁定读取）开销。
        // 启动时已取一次初值（见线程函数），其余态保持上次值即可。
        if (ctx->state == CS_Idle || ctx->state == CS_Selecting) {
            ctx->currentColor = GetPixelColorFromBitmap(ctx->memDC,
                ctx->mouseX, ctx->mouseY, ctx->virtualX, ctx->virtualY, ctx->dpiScale);
        }

        if (ctx->state == CS_Selecting) {
            ctx->endX = ctx->mouseX;
            ctx->endY = ctx->mouseY;
            InvalidateRect(hwnd, NULL, FALSE);
        } else if (ctx->state == CS_Idle) {
            int newHovered = FindWindowAtPoint(ctx->windows, ctx->mouseX, ctx->mouseY);
            ctx->hoveredWindow = newHovered;
            // 像素信息浮窗跟随鼠标：刷新旧面板位置 ∪ 新面板位置（放大镜跟随，两块都需重绘）。
            // 新面板位置在此预算（与 WM_PAINT 的 CalcPanelPosition 同源）。
            int npx, npy;
            CalcPanelPosition(ctx->mouseX, ctx->mouseY,
                ctx->virtualX, ctx->virtualY, ctx->virtualW, ctx->virtualH, ctx->panelMetrics, npx, npy);
            RECT newPanel = { npx - ctx->virtualX, npy - ctx->virtualY,
                              npx - ctx->virtualX + ctx->panelMetrics.w, npy - ctx->virtualY + ctx->panelMetrics.h };
            RECT dirty = InflateRectBy(UnionRectSafe(ctx->lastPanelRect, newPanel), 2);
            // 窗口高亮变化也纳入（hoveredWindow 切换时旧/新高亮框）
            if (newHovered >= 0 && newHovered < (int)ctx->windows.size()) {
                RECT hr = ctx->windows[newHovered].rect;
                hr.left -= ctx->virtualX; hr.top -= ctx->virtualY;
                hr.right -= ctx->virtualX; hr.bottom -= ctx->virtualY;
                dirty = UnionRectSafe(dirty, InflateRectBy(hr, 5));
            }
            dirty = UnionRectSafe(dirty, InflateRectBy(ctx->lastHighlightRect, 5));
            InvalidateRect(hwnd, &dirty, FALSE);
        } else if (ctx->state == CS_Resizing) {
            // 每帧都从按下时的完整快照重算：活动端可以越过固定端并翻转，固定端不会
            // 因上一帧 NormalizeRect 后的 left/right、top/bottom 角色交换而漂移。
            const RECT& startSelection = ctx->dragStartSelection;
            int dx = pt.x - ctx->dragStartX;
            int dy = pt.y - ctx->dragStartY;
            RECT virtualBounds = {
                ctx->virtualX, ctx->virtualY,
                ctx->virtualX + ctx->virtualW, ctx->virtualY + ctx->virtualH
            };
            RECT contentBounds = {0, 0, 0, 0};
            bool hasContent = CalcAnnotationsBounds(ctx->annotations, contentBounds, ctx->backDC);
            // 拖动过程中不强制最小尺寸，保证穿越固定点时连续；鼠标释放时再按最终方向补足。
            ctx->selection = ResizeSelectionFromHandle(
                startSelection, ctx->resizeHandle, dx, dy, virtualBounds,
                hasContent, contentBounds, false);
            InvalidateRect(hwnd, NULL, FALSE);
        } else if (ctx->state == CS_Moving) {
            // 整体平移
            int dx = pt.x - ctx->dragStartX;
            int dy = pt.y - ctx->dragStartY;
            int sw = ctx->dragStartSelection.right - ctx->dragStartSelection.left;
            int sh = ctx->dragStartSelection.bottom - ctx->dragStartSelection.top;
            int nl = ctx->dragStartSelection.left + dx;
            int nt = ctx->dragStartSelection.top + dy;
            // 约束到虚拟屏幕
            if (nl < ctx->virtualX) nl = ctx->virtualX;
            if (nt < ctx->virtualY) nt = ctx->virtualY;
            if (nl + sw > ctx->virtualX + ctx->virtualW) nl = ctx->virtualX + ctx->virtualW - sw;
            if (nt + sh > ctx->virtualY + ctx->virtualH) nt = ctx->virtualY + ctx->virtualH - sh;
            ctx->selection.left = nl;
            ctx->selection.top = nt;
            ctx->selection.right = nl + sw;
            ctx->selection.bottom = nt + sh;
            InvalidateRect(hwnd, NULL, FALSE);
        } else if (ctx->state == CS_Drawing) {
            // 更新正在绘制的标注终点/路径（选区相对坐标）
            if (ctx->hasCurDrawing) {
                // 终点用绝对坐标，钳制到选区内
                int selL = ctx->selection.left, selR = ctx->selection.right;
                int selT = ctx->selection.top, selB = ctx->selection.bottom;
                int ax = (std::max)(selL, (std::min)(ctx->mouseX, selR));
                int ay = (std::max)(selT, (std::min)(ctx->mouseY, selB));
                if (ctx->curDrawing.type == AT_Brush) {
                    POINT p = { ax, ay };
                    ctx->curDrawing.pts.push_back(p);
                } else if (ctx->curDrawing.type == AT_Mosaic && !ctx->curDrawing.mosaicRect) {
                    // 马赛克涂抹模式：记录路径点。揭示由 WM_PAINT 统一处理（reveal-mask 模型，
                    // 每帧从预计算的 base 揭示蒙版区域，无需增量绘制）。
                    POINT p = { ax, ay };
                    ctx->curDrawing.pts.push_back(p);
                } else {
                    ctx->curDrawing.x2 = ax;
                    ctx->curDrawing.y2 = ay;
                }
                // 局部刷新：上帧 curDrawing 包围盒 ∪ 本帧鼠标点附近（标注都在选区内）。
                // 坐标转 backDC（减 virtualX/Y）。lastDrawingBox 由 WM_PAINT 每帧更新。
                RECT mouseBox = { ax - ctx->virtualX - 8, ay - ctx->virtualY - 8,
                                  ax - ctx->virtualX + 8, ay - ctx->virtualY + 8 };
                RECT drawDirty;
                if (ctx->hasLastDrawingBox) {
                    RECT ldb = { ctx->lastDrawingBox.left - ctx->virtualX,
                                 ctx->lastDrawingBox.top - ctx->virtualY,
                                 ctx->lastDrawingBox.right - ctx->virtualX,
                                 ctx->lastDrawingBox.bottom - ctx->virtualY };
                    drawDirty = InflateRectBy(UnionRectSafe(ldb, mouseBox), 4);
                } else {
                    drawDirty = InflateRectBy(mouseBox, 4);
                }
                InvalidateRect(hwnd, &drawDirty, FALSE);
            }
        } else if (ctx->state == CS_TextEditing) {
            // 文字编辑态：拖动选择文字
            if (ctx->textDraggingSelection) {
                int mxRel = ctx->mouseX - ctx->virtualX;
                int textX = ctx->textAnchorX - ctx->virtualX;
                int fontPx = SC_FONT_SIZES[ctx->fontSizeIdx];
                int caretPos = CalcCaretPosFromMouse(ctx->backDC, ctx->textBuf, fontPx, textX, mxRel);

                ctx->textSelEnd = caretPos;
                ctx->textCaretPos = caretPos;
                InvalidateTextLine(hwnd, ctx);
            }
        } else if (ctx->resizingAnnotation >= 0) {
            // 非文字标注缩放：四角手柄走包围盒变换；箭头端点手柄仅平移单个端点
            int idx = ctx->resizingAnnotation;
            int dx = ctx->mouseX - ctx->annotationDragStartX;
            int dy = ctx->mouseY - ctx->annotationDragStartY;
            if (!ctx->annotationOpHistoryPushed && (dx != 0 || dy != 0)) {
                PushAnnotationHistory(ctx);
                ctx->annotationOpHistoryPushed = true;
            }
            // 从按下时快照还原再变换，避免累积浮点误差
            ctx->annotations[idx] = ctx->dragStartAnnotation;
            Annotation& a = ctx->annotations[idx];
            if (ctx->annotationResizeHandle == RH_ArrowStart
                || ctx->annotationResizeHandle == RH_ArrowEnd) {
                // 箭头端点拖拽：仅移动对应端点，另一端点保持快照值不变
                if (ctx->annotationResizeHandle == RH_ArrowStart) {
                    a.x1 = ctx->dragStartAnnotation.x1 + dx;
                    a.y1 = ctx->dragStartAnnotation.y1 + dy;
                } else {
                    a.x2 = ctx->dragStartAnnotation.x2 + dx;
                    a.y2 = ctx->dragStartAnnotation.y2 + dy;
                }
            } else {
                // 包围盒缩放：4 角 + 4 边中点手柄，根据拖拽手柄更新包围盒，
                // 再按包围盒变换映射标注坐标（矩形/圆均支持 8 手柄）。
                const RECT& o = ctx->annotationResizeStartBox;
                RECT n = o;
                switch (ctx->annotationResizeHandle) {
                    case RH_TopLeft:     n.left = o.left + dx; n.top = o.top + dy; break;
                    case RH_TopRight:    n.right = o.right + dx; n.top = o.top + dy; break;
                    case RH_BottomLeft:  n.left = o.left + dx; n.bottom = o.bottom + dy; break;
                    case RH_BottomRight: n.right = o.right + dx; n.bottom = o.bottom + dy; break;
                    case RH_Left:        n.left = o.left + dx; break;
                    case RH_Right:       n.right = o.right + dx; break;
                    case RH_Top:         n.top = o.top + dy; break;
                    case RH_Bottom:      n.bottom = o.bottom + dy; break;
                }
                // 防翻转：保证 right>left、bottom>top（至少 1px）
                n = NormalizeRect(n);
                if (n.right - n.left < 2) n.right = n.left + 2;
                if (n.bottom - n.top < 2) n.bottom = n.top + 2;
                TransformAnnotationByBox(a, o, n);
            }
            InvalidateAnnotationOp(hwnd, ctx, MeasureAnnotationBounds(ctx->annotations[idx], ctx->backDC));
        } else if (ctx->draggingAnnotation >= 0) {
            // 非文字标注整体拖拽：对按下时快照做 dx/dy 平移后写回（避免累积误差）
            int idx = ctx->draggingAnnotation;
            int dx = ctx->mouseX - ctx->annotationDragStartX;
            int dy = ctx->mouseY - ctx->annotationDragStartY;
            if (!ctx->annotationOpHistoryPushed && (dx != 0 || dy != 0)) {
                PushAnnotationHistory(ctx);
                ctx->annotationOpHistoryPushed = true;
            }
            ctx->annotations[idx] = ctx->dragStartAnnotation;
            Annotation& a = ctx->annotations[idx];
            switch (a.type) {
                case AT_Rect:
                case AT_Circle:
                case AT_Arrow:
                case AT_Mosaic:
                    if (a.type == AT_Mosaic && !a.mosaicRect) {
                        for (POINT& p : a.pts) { p.x += dx; p.y += dy; }
                    } else {
                        a.x1 += dx; a.y1 += dy; a.x2 += dx; a.y2 += dy;
                    }
                    break;
                case AT_Brush:
                    for (POINT& p : a.pts) { p.x += dx; p.y += dy; }
                    break;
                case AT_Text:
                    a.x1 += dx; a.y1 += dy;
                    break;
            }
            InvalidateAnnotationOp(hwnd, ctx, MeasureAnnotationBounds(ctx->annotations[idx], ctx->backDC));
        } else if (ctx->draggingTextAnnotation >= 0) {
            // 拖动文字标注位置
            int dx = ctx->mouseX - ctx->textDragStartX;
            int dy = ctx->mouseY - ctx->textDragStartY;
            if (!ctx->annotationOpHistoryPushed && (dx != 0 || dy != 0)) {
                PushAnnotationHistory(ctx);
                ctx->annotationOpHistoryPushed = true;
            }
            ctx->annotations[ctx->draggingTextAnnotation].x1 = ctx->dragStartX + dx;
            ctx->annotations[ctx->draggingTextAnnotation].y1 = ctx->dragStartY + dy;
            InvalidateAnnotationOp(hwnd, ctx, MeasureAnnotationBounds(ctx->annotations[ctx->draggingTextAnnotation], ctx->backDC));
        } else if (ctx->state == CS_Confirmed) {
            // hover 手柄/工具栏/文字/非文字标注变化需重绘以更新光标提示与高亮
            int h = HitTestHandle(ctx->mouseX, ctx->mouseY, ctx->selection);
            int mxRel = ctx->mouseX - ctx->virtualX;
            int myRel = ctx->mouseY - ctx->virtualY;
            int tb = HitTestToolbar(mxRel, myRel, ctx->toolbarRect, ctx->toolbarMetrics);
            int ht = HitTestTextAnnotations(ctx->annotations, ctx->mouseX, ctx->mouseY, ctx->backDC);
            // 非文字标注 hover：优先用选中项的手柄命中（箭头=端点；矩形/圆=8 手柄；画笔=无），否则普通命中
            int ha = -1;
            if (ctx->selectedAnnotation >= 0 && ctx->selectedAnnotation < (int)ctx->annotations.size()) {
                Annotation& sel = ctx->annotations[ctx->selectedAnnotation];
                bool onHandle = (HitTestAnnotationResizeHandle(sel, ctx->mouseX, ctx->mouseY,
                    ctx->backDC) != RH_None);
                if (onHandle) {
                    ha = ctx->selectedAnnotation;  // 悬停在手柄上视为悬停选中项（光标由 SETCURSOR 处理）
                }
            }
            if (ha < 0) {
                ha = HitTestAnnotation(ctx->annotations, ctx->mouseX, ctx->mouseY, ctx->backDC);
            }
            // 仅当 hover 状态真正变化时才重绘（去掉纯 moved 无变化的重绘，减少无意义全屏帧）。
            // 脏区域 = 各变化项的旧位置 ∪ 新位置（工具栏/标注边框高亮变化）。
            if (h != ctx->resizeHandle || tb != ctx->hoverToolbarBtn
                || ht != ctx->hoveredTextAnnotation || ha != ctx->hoveredAnnotation) {
                RECT dirty = {0,0,0,0};
                // 工具栏 hover 变化：整条工具栏（按钮高亮）
                if (tb != ctx->hoverToolbarBtn) {
                    dirty = UnionRectSafe(dirty, InflateRectBy(ctx->toolbarRect, 2));
                }
                // 文字标注 hover 变化：旧 ∪ 新 标注盒（backDC 坐标）
                if (ht != ctx->hoveredTextAnnotation) {
                    if (ctx->hoveredTextAnnotation >= 0 && ctx->hoveredTextAnnotation < (int)ctx->annotations.size()) {
                        RECT r = MeasureTextAnnotation(ctx->backDC, ctx->annotations[ctx->hoveredTextAnnotation]);
                        r.left -= ctx->virtualX; r.top -= ctx->virtualY;
                        r.right -= ctx->virtualX; r.bottom -= ctx->virtualY;
                        dirty = UnionRectSafe(dirty, InflateRectBy(r, 3));
                    }
                    if (ht >= 0 && ht < (int)ctx->annotations.size()) {
                        RECT r = MeasureTextAnnotation(ctx->backDC, ctx->annotations[ht]);
                        r.left -= ctx->virtualX; r.top -= ctx->virtualY;
                        r.right -= ctx->virtualX; r.bottom -= ctx->virtualY;
                        dirty = UnionRectSafe(dirty, InflateRectBy(r, 3));
                    }
                }
                // 非文字标注 hover 变化：旧 ∪ 新 标注盒
                // 选中态标注在 hover 进入/离开时会显示/隐藏 resize 手柄，手柄贴在包围盒边缘外，
                // inflate 量需覆盖手柄半径（SC_HANDLE_SIZE/2 + 余量），否则手柄痕迹残留。
                if (ha != ctx->hoveredAnnotation) {
                    const int handleMargin = SC_HANDLE_SIZE / 2 + 4;
                    if (ctx->hoveredAnnotation >= 0 && ctx->hoveredAnnotation < (int)ctx->annotations.size()) {
                        RECT r = MeasureAnnotationBounds(ctx->annotations[ctx->hoveredAnnotation], ctx->backDC);
                        r.left -= ctx->virtualX; r.top -= ctx->virtualY;
                        r.right -= ctx->virtualX; r.bottom -= ctx->virtualY;
                        dirty = UnionRectSafe(dirty, InflateRectBy(r, handleMargin));
                    }
                    if (ha >= 0 && ha < (int)ctx->annotations.size()) {
                        RECT r = MeasureAnnotationBounds(ctx->annotations[ha], ctx->backDC);
                        r.left -= ctx->virtualX; r.top -= ctx->virtualY;
                        r.right -= ctx->virtualX; r.bottom -= ctx->virtualY;
                        dirty = UnionRectSafe(dirty, InflateRectBy(r, handleMargin));
                    }
                }
                ctx->resizeHandle = h;
                ctx->hoverToolbarBtn = tb;
                ctx->hoveredTextAnnotation = ht;
                ctx->hoveredAnnotation = ha;
                if (IsValidRect(dirty)) {
                    InvalidateRect(hwnd, &dirty, FALSE);
                } else {
                    // 兜底（理论上不会到这里）
                    InvalidateRect(hwnd, NULL, FALSE);
                }
            }
        }
        return 0;
    }

    case WM_LBUTTONUP: {
        if (ctx->state == CS_Selecting) {
            int w = abs(ctx->endX - ctx->startX);
            int h = abs(ctx->endY - ctx->startY);

            RECT finalRect;
            if (w <= 1 && h <= 1) {
                // 点击 -> 使用悬停窗口矩形
                int idx = FindWindowAtPoint(ctx->windows, ctx->mouseX, ctx->mouseY);
                if (idx >= 0) {
                    finalRect = ctx->windows[idx].rect;
                } else {
                    // 匹配不到窗口时，默认选区为鼠标所在的屏幕
                    POINT pt = { ctx->mouseX, ctx->mouseY };
                    HMONITOR hMonitor = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
                    if (hMonitor) {
                        MONITORINFO monitorInfo;
                        monitorInfo.cbSize = sizeof(MONITORINFO);
                        if (GetMonitorInfo(hMonitor, &monitorInfo)) {
                            finalRect = monitorInfo.rcMonitor;
                        } else {
                            // 获取失败，降级为虚拟屏幕
                            finalRect = { ctx->virtualX, ctx->virtualY,
                                          ctx->virtualX + ctx->virtualW, ctx->virtualY + ctx->virtualH };
                        }
                    } else {
                        // 获取显示器失败，降级为虚拟屏幕
                        finalRect = { ctx->virtualX, ctx->virtualY,
                                      ctx->virtualX + ctx->virtualW, ctx->virtualY + ctx->virtualH };
                    }
                }
            } else {
                finalRect.left = (std::min)(ctx->startX, ctx->endX);
                finalRect.top = (std::min)(ctx->startY, ctx->endY);
                finalRect.right = (std::max)(ctx->startX, ctx->endX);
                finalRect.bottom = (std::max)(ctx->startY, ctx->endY);
            }

            // 进入确认态（可调整/拖动/工具栏），而非直接完成
            EnterConfirmed(ctx, finalRect);
            // 自动确认模式：跳过编辑态，直接提取选区并完成截图
            if (ctx->autoConfirm) {
                ScreenshotResult* result = ExtractRegionResult(ctx->memDC, finalRect,
                    ctx->virtualX, ctx->virtualY, ctx->dpiScale, ctx->annotations);
                if (g_screenshotTsfn != nullptr) {
                    napi_call_threadsafe_function(g_screenshotTsfn, result, napi_tsfn_nonblocking);
                } else {
                    delete result;
                }
                ctx->state = CS_Done;
                DestroyWindow(hwnd);
                return 0;
            }
            InvalidateRect(hwnd, NULL, FALSE);
        } else if (ctx->state == CS_Resizing) {
            // resize 结束：从按下快照和最终鼠标位置重算，并只沿活动端当前所在侧补足最小尺寸。
            // 不能复用 EnterConfirmed 的固定向右/下扩张，否则穿越后会移动按下时的固定点。
            int dx = ctx->mouseX - ctx->dragStartX;
            int dy = ctx->mouseY - ctx->dragStartY;
            RECT virtualBounds = {
                ctx->virtualX, ctx->virtualY,
                ctx->virtualX + ctx->virtualW, ctx->virtualY + ctx->virtualH
            };
            RECT contentBounds = {0, 0, 0, 0};
            bool hasContent = CalcAnnotationsBounds(ctx->annotations, contentBounds, ctx->backDC);
            ctx->selection = ResizeSelectionFromHandle(
                ctx->dragStartSelection, ctx->resizeHandle, dx, dy, virtualBounds,
                hasContent, contentBounds, true);
            ctx->resizeHandle = RH_None;
            ctx->state = CS_Confirmed;
            ctx->needFullRedraw = true;
            InvalidateRect(hwnd, NULL, FALSE);
        } else if (ctx->state == CS_Moving) {
            // 整体拖动结束仍可走通用确认流程；移动不会改变 resize 固定点语义。
            EnterConfirmed(ctx, ctx->selection);
            InvalidateRect(hwnd, NULL, FALSE);
        } else if (ctx->state == CS_Drawing) {
            // 绘制结束 -> 提交标注（仅在有有效尺寸时）
            bool valid = false;
            if (ctx->hasCurDrawing) {
                if (ctx->curDrawing.type == AT_Brush) {
                    valid = ctx->curDrawing.pts.size() >= 2;
                } else if (ctx->curDrawing.type == AT_Mosaic) {
                    if (ctx->curDrawing.mosaicRect) {
                        // 框选模式：需要有效矩形尺寸
                        valid = (abs(ctx->curDrawing.x2 - ctx->curDrawing.x1) >= 2
                              || abs(ctx->curDrawing.y2 - ctx->curDrawing.y1) >= 2);
                    } else {
                        // 涂抹模式：至少 1 个点（单击也能产生一个马赛克圆）
                        valid = ctx->curDrawing.pts.size() >= 1;
                    }
                } else {
                    valid = (abs(ctx->curDrawing.x2 - ctx->curDrawing.x1) >= 2
                          || abs(ctx->curDrawing.y2 - ctx->curDrawing.y1) >= 2);
                }
            }
            if (valid) {
                PushAnnotationHistory(ctx);
                ctx->annotations.push_back(ctx->curDrawing);
                // reveal-mask 模型：base 与标注无关，下一帧 WM_PAINT 自动把新蒙版揭示出来。
            }
            ctx->hasCurDrawing = false;
            ctx->curDrawing = {};
            ctx->mosaicDrawLastIdx = 0;
            ctx->state = CS_Confirmed;
            ctx->needFullRedraw = true;
        } else if (ctx->state == CS_TextEditing) {
            // 文字选择结束
            if (ctx->textDraggingSelection) {
                ctx->textDraggingSelection = false;
                // 如果选择范围相同，清除选择
                if (ctx->textSelStart == ctx->textSelEnd) {
                    ctx->textSelStart = -1;
                    ctx->textSelEnd = -1;
                }
            }
        } else if (ctx->resizingAnnotation >= 0) {
            // 非文字标注缩放结束：先清缩放标志，再刷新最后位置与上帧位置的并集。
            // WM_PAINT 将按最终确认态重画选中手柄，避免沿用拖拽态光标/外观。
            int idx = ctx->resizingAnnotation;
            RECT finalBox = MeasureAnnotationBounds(ctx->annotations[idx], ctx->backDC);
            ctx->resizingAnnotation = -1;
            ctx->annotationResizeHandle = RH_None;
            ctx->annotationOpHistoryPushed = false;
            InvalidateAnnotationOp(hwnd, ctx, finalBox);
        } else if (ctx->draggingAnnotation >= 0) {
            // 非文字标注拖拽结束：先退出拖拽态再补刷最终脏区，不退化为全屏重绘。
            int idx = ctx->draggingAnnotation;
            RECT finalBox = MeasureAnnotationBounds(ctx->annotations[idx], ctx->backDC);
            ctx->draggingAnnotation = -1;
            ctx->annotationOpHistoryPushed = false;
            InvalidateAnnotationOp(hwnd, ctx, finalBox);
        } else if (ctx->draggingTextAnnotation >= 0) {
            // 文字拖动结束：先退出拖动态，再按最终文字边框区域完成局部刷新。
            int idx = ctx->draggingTextAnnotation;
            RECT finalBox = MeasureAnnotationBounds(ctx->annotations[idx], ctx->backDC);
            ctx->draggingTextAnnotation = -1;
            ctx->annotationOpHistoryPushed = false;
            InvalidateAnnotationOp(hwnd, ctx, finalBox);
        }
        return 0;
    }

    case WM_LBUTTONDBLCLK: {
        // 确认态下双击选区内部 -> 确认截图
        if ((ctx->state == CS_Confirmed) && PointInRect(ctx->mouseX, ctx->mouseY, ctx->selection)) {
            ScreenshotResult* result = ExtractRegionResult(ctx->memDC, ctx->selection,
                ctx->virtualX, ctx->virtualY, ctx->dpiScale, ctx->annotations);
            if (g_screenshotTsfn != nullptr) {
                napi_call_threadsafe_function(g_screenshotTsfn, result, napi_tsfn_nonblocking);
            }
            ctx->state = CS_Done;
            DestroyWindow(hwnd);
        }
        return 0;
    }

    case WM_RBUTTONDOWN: {
        ctx->state = CS_Cancelled;
        // 回调失败结果
        if (g_screenshotTsfn != nullptr) {
            ScreenshotResult* result = new ScreenshotResult();
            result->success = false;
            result->x = 0; result->y = 0; result->x2 = 0; result->y2 = 0;
            result->width = 0; result->height = 0;
            napi_call_threadsafe_function(g_screenshotTsfn, result, napi_tsfn_nonblocking);
        }
        DestroyWindow(hwnd);
        return 0;
    }

    case WM_KEYDOWN: {
        if (wParam == VK_ESCAPE) {
            // 文字编辑态：ESC 取消输入，回到确认态
            if (ctx->state == CS_TextEditing) {
                ctx->textBuf.clear();
                ctx->textCaretPos = 0;
                ctx->state = CS_Confirmed;
                ctx->needFullRedraw = true;
                InvalidateRect(hwnd, NULL, FALSE);
                return 0;
            }
            // 其它状态：ESC 取消截图
            ctx->state = CS_Cancelled;
            if (g_screenshotTsfn != nullptr) {
                ScreenshotResult* result = new ScreenshotResult();
                result->success = false;
                result->x = 0; result->y = 0; result->x2 = 0; result->y2 = 0;
                result->width = 0; result->height = 0;
                napi_call_threadsafe_function(g_screenshotTsfn, result, napi_tsfn_nonblocking);
            }
            DestroyWindow(hwnd);
        } else if (wParam == VK_RETURN) {
            // 文字编辑态：Enter 提交文字
            if (ctx->state == CS_TextEditing) {
                if (!ctx->textBuf.empty()) {
                    Annotation textAnnotation = {};
                    textAnnotation.type = AT_Text;
                    textAnnotation.color = SC_COLOR_PRESETS[ctx->drawColorIdx];
                    textAnnotation.thickness = SC_FONT_SIZES[ctx->fontSizeIdx];
                    textAnnotation.x1 = ctx->textAnchorX;
                    textAnnotation.y1 = ctx->textAnchorY;
                    textAnnotation.text = ctx->textBuf;
                    PushAnnotationHistory(ctx);
                    ctx->annotations.push_back(textAnnotation);
                }
                ctx->textBuf.clear();
                ctx->textCaretPos = 0;
                ctx->state = CS_Confirmed;
                ctx->needFullRedraw = true;
                InvalidateRect(hwnd, NULL, FALSE);
                return 0;
            }
            // 确认态：Enter 确认截图
            if (ctx->state == CS_Confirmed) {
                ScreenshotResult* result = ExtractRegionResult(ctx->memDC, ctx->selection,
                    ctx->virtualX, ctx->virtualY, ctx->dpiScale, ctx->annotations);
                if (g_screenshotTsfn != nullptr) {
                    napi_call_threadsafe_function(g_screenshotTsfn, result, napi_tsfn_nonblocking);
                }
                ctx->state = CS_Done;
                DestroyWindow(hwnd);
            }
        } else if (wParam == VK_BACK) {
            // 文字编辑态：退格删除（文字内容变化，整行重排，刷新文字行区域）
            if (ctx->state == CS_TextEditing && ctx->textCaretPos > 0) {
                ctx->textBuf.erase(ctx->textCaretPos - 1, 1);
                ctx->textCaretPos--;
                InvalidateTextLine(hwnd, ctx);
                return 0;
            }
        } else if (wParam == VK_DELETE) {
            // 文字编辑态：Delete 删除
            if (ctx->state == CS_TextEditing && ctx->textCaretPos < (int)ctx->textBuf.size()) {
                ctx->textBuf.erase(ctx->textCaretPos, 1);
                InvalidateTextLine(hwnd, ctx);
                return 0;
            }
            // 确认态：Delete 删除当前选中的覆盖物
            if (ctx->state == CS_Confirmed) {
                if (ctx->selectedTextAnnotation >= 0
                    && ctx->selectedTextAnnotation < (int)ctx->annotations.size()) {
                    RECT dirty = CalcSelectionDirty(ctx, true /*includeToolbar*/);
                    PushAnnotationHistory(ctx);
                    ctx->annotations.erase(ctx->annotations.begin() + ctx->selectedTextAnnotation);
                    ctx->selectedTextAnnotation = -1;
                    ctx->hoveredTextAnnotation = -1;
                    ctx->draggingTextAnnotation = -1;
                    ctx->activeTool = TB_Drag;
                    ctx->popupTool = -1;
                    ctx->popupOpen = false;
                    if (IsValidRect(dirty)) {
                        InvalidateRect(hwnd, &dirty, FALSE);
                    } else {
                        InvalidateRect(hwnd, NULL, FALSE);
                    }
                    return 0;
                }
                if (ctx->selectedAnnotation >= 0
                    && ctx->selectedAnnotation < (int)ctx->annotations.size()) {
                    RECT dirty = CalcSelectionDirty(ctx, true /*includeToolbar*/);
                    PushAnnotationHistory(ctx);
                    ctx->annotations.erase(ctx->annotations.begin() + ctx->selectedAnnotation);
                    ctx->selectedAnnotation = -1;
                    ctx->hoveredAnnotation = -1;
                    ctx->draggingAnnotation = -1;
                    ctx->resizingAnnotation = -1;
                    ctx->annotationResizeHandle = RH_None;
                    ctx->activeTool = TB_Drag;
                    ctx->popupTool = -1;
                    ctx->popupOpen = false;
                    if (IsValidRect(dirty)) {
                        InvalidateRect(hwnd, &dirty, FALSE);
                    } else {
                        InvalidateRect(hwnd, NULL, FALSE);
                    }
                    return 0;
                }
            }
        } else if (wParam == VK_LEFT) {
            // 文字编辑态：左箭头移动光标（仅光标位置变化）
            if (ctx->state == CS_TextEditing && ctx->textCaretPos > 0) {
                ctx->textCaretPos--;
                InvalidateTextLine(hwnd, ctx);
                return 0;
            }
        } else if (wParam == VK_RIGHT) {
            // 文字编辑态：右箭头移动光标
            if (ctx->state == CS_TextEditing && ctx->textCaretPos < (int)ctx->textBuf.size()) {
                ctx->textCaretPos++;
                InvalidateTextLine(hwnd, ctx);
                return 0;
            }
        } else if (wParam == VK_HOME) {
            // 文字编辑态：Home 移动到行首
            if (ctx->state == CS_TextEditing) {
                ctx->textCaretPos = 0;
                InvalidateTextLine(hwnd, ctx);
                return 0;
            }
        } else if (wParam == VK_END) {
            // 文字编辑态：End 移动到行尾
            if (ctx->state == CS_TextEditing) {
                ctx->textCaretPos = (int)ctx->textBuf.size();
                InvalidateTextLine(hwnd, ctx);
                return 0;
            }
        }
        return 0;
    }

    case WM_IME_COMPOSITION: {
        // 处理中文输入法（IME）输入
        if (ctx->state == CS_TextEditing) {
            if (lParam & GCS_RESULTSTR) {
                HIMC hIMC = ImmGetContext(hwnd);
                if (hIMC) {
                    // 获取输入法完成的字符串长度
                    LONG len = ImmGetCompositionStringW(hIMC, GCS_RESULTSTR, NULL, 0);
                    if (len > 0) {
                        // 分配缓冲区并获取字符串
                        std::wstring result(len / sizeof(wchar_t), L'\0');
                        ImmGetCompositionStringW(hIMC, GCS_RESULTSTR, &result[0], len);
                        result.resize(len / sizeof(wchar_t));

                        // 插入到当前光标位置
                        ctx->textBuf.insert(ctx->textCaretPos, result);
                        ctx->textCaretPos += (int)result.size();
                        InvalidateTextLine(hwnd, ctx);
                    }
                    ImmReleaseContext(hwnd, hIMC);
                }
                return 0;
            }
        }
        return DefWindowProc(hwnd, msg, wParam, lParam);
    }

    case WM_CHAR: {
        // 文字编辑态：接收字符输入（ASCII 和直接输入）
        if (ctx->state == CS_TextEditing) {
            wchar_t ch = (wchar_t)wParam;
            // 过滤控制字符（除了可打印字符）
            // 忽略 IME 相关的控制字符
            if (ch >= 32 && ch != 127) {
                ctx->textBuf.insert(ctx->textCaretPos, 1, ch);
                ctx->textCaretPos++;
                InvalidateTextLine(hwnd, ctx);
            }
            return 0;
        }
        return 0;
    }

    case WM_SETCURSOR: {
        // 马赛克涂抹模式（确认态/绘制态）：用预生成的半径圆光标（OS 跟随，无重绘延迟）
        if (ctx->activeTool == TB_Mosaic && !ctx->mosaicRectMode
            && (ctx->state == CS_Confirmed || ctx->state == CS_Drawing)
            && ctx->mosaicBrushCursorsInited
            && ctx->mosaicRadiusIdx >= 0 && ctx->mosaicRadiusIdx < SC_MOSAIC_RADIUS_COUNT
            && ctx->mosaicBrushCursors[ctx->mosaicRadiusIdx]) {
            // 工具栏/子菜单上仍用箭头/手型
            int mxRel = ctx->mouseX - ctx->virtualX;
            int myRel = ctx->mouseY - ctx->virtualY;
            if (PointInRect(mxRel, myRel, ctx->toolbarRect)) {
                SetCursor(LoadCursorW(NULL, (LPCWSTR)IDC_ARROW));
                return TRUE;
            }
            if (ctx->popupOpen && PointInRect(mxRel, myRel, ctx->popupRect)) {
                SetCursor(LoadCursorW(NULL, (LPCWSTR)IDC_HAND));
                return TRUE;
            }
            if (PointInRect(ctx->mouseX, ctx->mouseY, ctx->selection)) {
                SetCursor(ctx->mosaicBrushCursors[ctx->mosaicRadiusIdx]);
                return TRUE;
            }
        }
        // 文字编辑中：I-beam 光标
        if (ctx->state == CS_TextEditing) {
            SetCursor(LoadCursorW(NULL, (LPCWSTR)IDC_IBEAM));
            return TRUE;
        }
        // 绘制中：十字光标
        if (ctx->state == CS_Drawing) {
            SetCursor(LoadCursorW(NULL, (LPCWSTR)IDC_CROSS));
            return TRUE;
        }
        // 非文字标注缩放中：对应四角 resize 光标
        if (ctx->resizingAnnotation >= 0) {
            SetCursor(LoadCursorW(NULL, HandleCursor(ctx->annotationResizeHandle)));
            return TRUE;
        }
        // 非文字标注拖拽中：四向箭头光标
        if (ctx->draggingAnnotation >= 0) {
            SetCursor(LoadCursorW(NULL, (LPCWSTR)IDC_SIZEALL));
            return TRUE;
        }
        // 拖动文字标注中：四向箭头光标
        if (ctx->draggingTextAnnotation >= 0) {
            SetCursor(LoadCursorW(NULL, (LPCWSTR)IDC_SIZEALL));
            return TRUE;
        }
        // 确认态：根据 hover 位置切换 resize/move/箭头/十字光标
        if (ctx->state == CS_Confirmed) {
            // 工具栏或子菜单 -> 箭头
            int mxRel = ctx->mouseX - ctx->virtualX;
            int myRel = ctx->mouseY - ctx->virtualY;
            if (PointInRect(mxRel, myRel, ctx->toolbarRect)) {
                SetCursor(LoadCursorW(NULL, (LPCWSTR)IDC_ARROW));
                return TRUE;
            }
            if (ctx->popupOpen && PointInRect(mxRel, myRel, ctx->popupRect)) {
                SetCursor(LoadCursorW(NULL, (LPCWSTR)IDC_HAND));
                return TRUE;
            }
            // 手柄 -> 对应 resize 光标
            int h = HitTestHandle(ctx->mouseX, ctx->mouseY, ctx->selection);
            if (h != RH_None) {
                SetCursor(LoadCursorW(NULL, HandleCursor(h)));
                return TRUE;
            }
            // 已选中的非文字标注手柄 -> 对应 resize 光标（缩放入口）
            //   箭头=起点/终点端点手柄（固定四向箭头）；矩形/圆=8 手柄（方向自适应）；画笔=无
            // 独立命中测试，不依赖 hovered 缓存（RDP 节流下缓存会滞后）
            if (ctx->selectedAnnotation >= 0 && ctx->selectedAnnotation < (int)ctx->annotations.size()
                && ctx->annotations[ctx->selectedAnnotation].type != AT_Text) {
                Annotation& sel = ctx->annotations[ctx->selectedAnnotation];
                int handle = HitTestAnnotationResizeHandle(sel, ctx->mouseX, ctx->mouseY, ctx->backDC);
                if (handle != RH_None) {
                    SetCursor(LoadCursorW(NULL, HandleCursor(handle)));
                    return TRUE;
                }
            }
            // 非文字标注悬停 -> 四向箭头（可拖动/选中），与下方文字悬停判定并列
            {
                int hit = HitTestAnnotation(ctx->annotations, ctx->mouseX, ctx->mouseY, ctx->backDC);
                if (hit >= 0 && ctx->annotations[hit].type != AT_Text) {
                    SetCursor(LoadCursorW(NULL, (LPCWSTR)IDC_SIZEALL));
                    return TRUE;
                }
            }
            // 文字标注悬停 -> 四向箭头（可拖动/选中）
            // 注意：此处必须独立做命中测试，不能依赖 ctx->hoveredTextAnnotation。
            // 在远程桌面（RDP）下鼠标移动事件常被节流/合并，WM_MOUSEMOVE 更新
            // hoveredTextAnnotation 存在滞后，导致 WM_SETCURSOR 看到过期值。
            // 文字工具未激活时，悬停已确认文字 -> 拖动光标；
            // 文字工具激活时，悬停已确认文字 -> 仍为拖动光标（可选中改属性）。
            if (HitTestTextAnnotations(ctx->annotations, ctx->mouseX, ctx->mouseY, ctx->backDC) >= 0) {
                SetCursor(LoadCursorW(NULL, (LPCWSTR)IDC_SIZEALL));
                return TRUE;
            }
            // 选区内部：
            //   矢量/文字/马赛克工具激活 -> 十字；
            //   已有标注内容 -> 箭头（禁止整体拖动）；
            //   否则 -> 移动光标
            if (PointInRect(ctx->mouseX, ctx->mouseY, ctx->selection)) {
                if (IsVectorTool(ctx->activeTool) || ctx->activeTool == TB_Text
                    || ctx->activeTool == TB_Mosaic) {
                    SetCursor(LoadCursorW(NULL, (LPCWSTR)IDC_CROSS));
                } else if (!ctx->annotations.empty()) {
                    SetCursor(LoadCursorW(NULL, (LPCWSTR)IDC_ARROW));
                } else {
                    SetCursor(LoadCursorW(NULL, (LPCWSTR)IDC_SIZEALL));
                }
                return TRUE;
            }
            // 选区外 -> 箭头
            SetCursor(LoadCursorW(NULL, (LPCWSTR)IDC_ARROW));
            return TRUE;
        }
        if (ctx->state == CS_Resizing) {
            SetCursor(LoadCursorW(NULL, HandleCursor(ctx->resizeHandle)));
            return TRUE;
        }
        if (ctx->state == CS_Moving) {
            SetCursor(LoadCursorW(NULL, (LPCWSTR)IDC_SIZEALL));
            return TRUE;
        }
        return DefWindowProc(hwnd, msg, wParam, lParam);
    }

    case WM_DESTROY: {
        g_screenshotOverlayWindow = NULL;
        PostQuitMessage(0);
        return 0;
    }
    }

    return DefWindowProc(hwnd, msg, wParam, lParam);
}

// 截图线程（预截屏 + 双缓冲架构）
static void ScreenshotCaptureThread() {
    // 设置 DPI 感知
    typedef DPI_AWARENESS_CONTEXT (WINAPI *SetThreadDpiAwarenessContextProc)(DPI_AWARENESS_CONTEXT);
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32) {
        auto setDpiProc = (SetThreadDpiAwarenessContextProc)GetProcAddress(user32, "SetThreadDpiAwarenessContext");
        if (setDpiProc) {
            setDpiProc(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        }
    }

    double uiScale = GetDpiScaleFactor();
    double dpiScale = uiScale;

    // 预截屏整个虚拟屏幕
    HDC memDC = NULL;
    HBITMAP screenBitmap = NULL;
    int vx, vy, vw, vh;
    if (!AcquireScreenshotBase(memDC, screenBitmap, vx, vy, vw, vh, dpiScale)) {
        g_isCapturing = false;
        return;
    }

    // 创建双缓冲
    HDC backDC = NULL;
    HBITMAP backBmp = NULL;
    if (!CreateBackBuffer(backDC, backBmp, vw, vh)) {
        DeleteDC(memDC);
        DeleteObject(screenBitmap);
        g_isCapturing = false;
        return;
    }

    // 枚举窗口
    std::vector<SCWindowInfo> windows = EnumWindowsForCapture();

    // 初始化 GDI 资源
    SCPanelMetrics panelMetrics = CalcPanelMetrics(uiScale);
    SCGdiResources gdi;
    gdi.Init(panelMetrics.fontPx, panelMetrics.crosshair);
    // 创建选区外遮罩缓冲（需虚拟屏幕尺寸）
    gdi.InitMask(vw, vh);

    // 初始化上下文
    CaptureContext ctx = {};
    ctx.state = CS_Idle;
    ctx.autoConfirm = g_autoConfirm.load();
    ctx.virtualX = vx; ctx.virtualY = vy;
    ctx.virtualW = vw; ctx.virtualH = vh;
    ctx.startX = 0; ctx.startY = 0;
    ctx.endX = 0; ctx.endY = 0;
    ctx.hoveredWindow = -1;
    ctx.screenBitmap = screenBitmap;
    ctx.memDC = memDC;
    ctx.backDC = backDC;
    ctx.backBitmap = backBmp;
    ctx.lastPanelRect = {0,0,0,0};
    ctx.lastSelectionRect = {0,0,0,0};
    ctx.lastLabelRect = {0,0,0,0};
    ctx.lastHighlightRect = {0,0,0,0};
    ctx.lastToolbarRect = {0,0,0,0};
    ctx.lastPopupRect = {0,0,0,0};
    ctx.lastCaretRect = {0,0,0,0};
    ctx.hasLastCaret = false;
    ctx.lastAnnotationBox = {0,0,0,0};
    ctx.hasLastAnnotationBox = false;
    ctx.lastDrawingBox = {0,0,0,0};
    ctx.hasLastDrawingBox = false;
    ctx.selection = {0,0,0,0};
    ctx.resizeHandle = RH_None;
    ctx.dragStartX = 0; ctx.dragStartY = 0;
    ctx.dragStartSelection = {0,0,0,0};
    ctx.toolbarRect = {0,0,0,0};
    ctx.hoverToolbarBtn = -1;
    ctx.activeTool = -1;
    ctx.popupTool = -1;
    ctx.needFullRedraw = true;
    ctx.dpiScale = dpiScale;
    ctx.gdi = gdi;
    ctx.panelMetrics = panelMetrics;
    ctx.windows = std::move(windows);

    // 工具栏几何（按 DPI 缩放）+ 图标位图缓存（按 DPI 预渲染）
    ctx.toolbarMetrics = CalcToolbarMetrics(uiScale);
    ctx.iconCache.Init(ctx.toolbarMetrics.iconSize);
    // GDI+ 会话级初始化（必须在 InitMosaicBrushCursors 及任何 GDI+ 调用之前）：
    // 会话内单次 Startup，避免每帧反复初始化导致拖拽卡顿。
    if (!InitGdipResources(&ctx)) {
        gdi.Cleanup();
        ctx.iconCache.Cleanup();
        DeleteDC(backDC); DeleteObject(backBmp);
        DeleteDC(memDC); DeleteObject(screenBitmap);
        g_captureCtx = nullptr;
        g_isCapturing = false;
        return;
    }
    // 涂抹光标缓存（按半径预生成，DPI 缩放半径）
    for (int i = 0; i < SC_MOSAIC_RADIUS_COUNT; i++) ctx.mosaicBrushCursors[i] = NULL;
    ctx.mosaicBrushCursorsInited = false;
    InitMosaicBrushCursors(&ctx);
    // 子菜单几何（按 DPI 缩放）+ 标注绘制默认值
    ctx.popupMetrics = CalcPopupMetrics(uiScale);
    ctx.popupOpen = false;
    ctx.popupRect = {0,0,0,0};
    ctx.drawColorIdx = SC_DEFAULT_COLOR_IDX;
    ctx.drawThickIdx = SC_DEFAULT_THICK_IDX;
    ctx.fontSizeIdx = SC_DEFAULT_FONT_IDX;
    ctx.mosaicSizeIdx = SC_DEFAULT_MOSAIC_IDX;
    ctx.mosaicRadiusIdx = SC_DEFAULT_MOSAIC_RADIUS_IDX;
    ctx.mosaicRectMode = false;  // 默认涂抹模式
    ctx.mosaicBaseDC = NULL;
    ctx.mosaicBaseBitmap = NULL;
    ctx.mosaicBaseW = 0;
    ctx.mosaicBaseH = 0;
    ctx.mosaicBaseBlockPx = 0;
    ctx.mosaicDrawLastIdx = 0;
    ctx.hasCurDrawing = false;
    // 文字编辑初始化
    ctx.textBuf.clear();
    ctx.textAnchorX = 0;
    ctx.textAnchorY = 0;
    ctx.textCaretPos = 0;
    ctx.textCaretVisible = true;
    ctx.textCaretLastBlink = GetTickCount();
    ctx.textSelStart = -1;
    ctx.textSelEnd = -1;
    ctx.textDraggingSelection = false;
    ctx.hoveredTextAnnotation = -1;
    ctx.selectedTextAnnotation = -1;
    ctx.draggingTextAnnotation = -1;
    ctx.textDragStartX = 0;
    ctx.textDragStartY = 0;
    // 非文字标注选中/拖拽/缩放状态初始化（与文字机制互斥）
    ctx.hoveredAnnotation = -1;
    ctx.selectedAnnotation = -1;
    ctx.draggingAnnotation = -1;
    ctx.resizingAnnotation = -1;
    ctx.annotationResizeHandle = RH_None;
    ctx.annotationDragStartX = 0;
    ctx.annotationDragStartY = 0;
    ctx.annotationOpHistoryPushed = false;
    ctx.dragStartAnnotation = {};
    ctx.annotationResizeStartBox = { 0, 0, 0, 0 };

    // 获取初始鼠标位置和颜色
    POINT pt;
    GetCursorPos(&pt);
    ctx.mouseX = pt.x;
    ctx.mouseY = pt.y;
    ctx.currentColor = GetPixelColorFromBitmap(memDC, pt.x, pt.y, vx, vy, dpiScale);

    g_captureCtx = &ctx;

    // 注册窗口类
    WNDCLASSEXW wc = {0};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.lpfnWndProc = ScreenshotOverlayWndProc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);  // 默认鼠标样式
    wc.lpszClassName = L"ZToolsScreenshotOverlay";

    if (!RegisterClassExW(&wc)) {
        gdi.Cleanup();
        ctx.iconCache.Cleanup();
        DeleteDC(backDC); DeleteObject(backBmp);
        DeleteDC(memDC); DeleteObject(screenBitmap);
        g_captureCtx = nullptr;
        g_isCapturing = false;
        return;
    }

    // 创建普通 WS_POPUP 窗口（非分层窗口）
    g_screenshotOverlayWindow = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        L"ZToolsScreenshotOverlay",
        L"Screenshot Overlay",
        WS_POPUP,
        vx, vy, vw, vh,
        NULL, NULL, GetModuleHandle(NULL), NULL
    );

    if (g_screenshotOverlayWindow == NULL) {
        UnregisterClassW(L"ZToolsScreenshotOverlay", GetModuleHandle(NULL));
        gdi.Cleanup();
        ctx.iconCache.Cleanup();
        DeleteDC(backDC); DeleteObject(backBmp);
        DeleteDC(memDC); DeleteObject(screenBitmap);
        g_captureCtx = nullptr;
        g_isCapturing = false;
        return;
    }

    ShowWindow(g_screenshotOverlayWindow, SW_SHOW);
    SetForegroundWindow(g_screenshotOverlayWindow);

    // 消息循环
    MSG msg;
    while (true) {
        if (ctx.state == CS_Done || ctx.state == CS_Cancelled) break;

        if (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) break;
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        } else {
            // 文字编辑态：光标闪烁（每 500ms 切换）
            if (ctx.state == CS_TextEditing) {
                DWORD now = GetTickCount();
                if (now - ctx.textCaretLastBlink >= 500) {
                    ctx.textCaretVisible = !ctx.textCaretVisible;
                    ctx.textCaretLastBlink = now;
                    // 仅刷新光标区域（光标位置不变，只切换可见性）。首次无缓存时全屏。
                    if (ctx.hasLastCaret) {
                        RECT r = InflateRectBy(ctx.lastCaretRect, 2);
                        InvalidateRect(g_screenshotOverlayWindow, &r, FALSE);
                    } else {
                        InvalidateRect(g_screenshotOverlayWindow, NULL, FALSE);
                    }
                }
            }

            // 检查 ESC 键（窗口可能没有焦点）
            if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) {
                if (ctx.state != CS_Done && ctx.state != CS_Cancelled) {
                    ctx.state = CS_Cancelled;
                    if (g_screenshotTsfn != nullptr) {
                        ScreenshotResult* result = new ScreenshotResult();
                        result->success = false;
                        result->x = 0; result->y = 0; result->x2 = 0; result->y2 = 0;
                        result->width = 0; result->height = 0;
                        napi_call_threadsafe_function(g_screenshotTsfn, result, napi_tsfn_nonblocking);
                    }
                    DestroyWindow(g_screenshotOverlayWindow);
                    break;
                }
            }
            Sleep(1);
        }
    }

    // 清理
    g_captureCtx = nullptr;
    gdi.Cleanup();
    ctx.iconCache.Cleanup();
    FreeMosaicBase(&ctx);
    FreeMosaicBrushCursors(&ctx);
    DeleteDC(backDC); DeleteObject(backBmp);
    DeleteDC(memDC); DeleteObject(screenBitmap);
    // GDI+ 会话级资源最后释放（所有 GDI+ 调用均已结束后才可 Shutdown）
    ShutdownGdipResources(&ctx);
    UnregisterClassW(L"ZToolsScreenshotOverlay", GetModuleHandle(NULL));
    g_isCapturing = false;
}

// 启动区域截图
Napi::Value StartRegionCapture(const Napi::CallbackInfo& info) {
    return StartRegionCaptureWithPrimedFrame(info);
}

// 供 JS 主动触发首帧预抓取。
Napi::Value PrimeScreenshotFrame(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    const bool success = PrimeScreenshotFrameNow();
    return Napi::Boolean::New(env, success);
}

Napi::Value StartRegionCaptureWithPrimedFrame(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();

    if (g_isCapturing) {
        Napi::Error::New(env, "Screenshot already in progress").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    // 解析可选参数（顺序无关，便于后续扩展）：
    //   - 回调函数：截图完成后回调
    //   - 选项对象：目前支持 { autoConfirm: boolean }
    //              默认 autoConfirm=true，选区确定后直接出图，跳过编辑态；
    //              传 false 才进入编辑态（工具栏/标注）
    bool autoConfirm = true;
    for (int i = 0; i < (int)info.Length(); i++) {
        if (info[i].IsFunction()) {
            Napi::Function callback = info[i].As<Napi::Function>();
            napi_value resource_name;
            napi_create_string_utf8(env, "ScreenshotCallback", NAPI_AUTO_LENGTH, &resource_name);

            napi_status status = napi_create_threadsafe_function(
                env, callback, nullptr, resource_name,
                0, 1, nullptr, nullptr, nullptr,
                CallScreenshotJs, &g_screenshotTsfn
            );

            if (status != napi_ok) {
                Napi::Error::New(env, "Failed to create threadsafe function").ThrowAsJavaScriptException();
                return env.Undefined();
            }
        } else if (info[i].IsObject()) {
            Napi::Object opts = info[i].As<Napi::Object>();
            if (opts.Has("autoConfirm")) {
                Napi::Value v = opts.Get("autoConfirm");
                if (v.IsBoolean()) {
                    autoConfirm = v.As<Napi::Boolean>().Value();
                }
            }
        }
    }
    g_autoConfirm = autoConfirm;

    g_isCapturing = true;

    g_screenshotThread = std::thread(ScreenshotCaptureThread);
    g_screenshotThread.detach();

    return env.Undefined();
}
