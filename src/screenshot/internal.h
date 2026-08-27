// 截图模块共享内部头：类型、常量、全局变量与跨文件函数声明（由 screenshot_windows.cpp 拆分而来）
#pragma once

#include <napi.h>
#include <node_api.h>
#include <windows.h>
#include <windowsx.h>  // For GET_X_LPARAM, GET_Y_LPARAM
#include <dwmapi.h>
// 冷门头 <imm.h>/<commdlg.h>/<shlobj.h> 不在此集中包含（本头类型声明均不依赖其符号），
// 仅由唯一使用方 .cpp 自行引入：imm.h → overlay_input_windows.cpp（IME 输入法）、
// commdlg.h → output_windows.cpp（GetSaveFileNameW 保存对话框）、
// shlobj.h → output_windows.cpp（SHGetKnownFolderPath 已知文件夹路径）。
#include <thread>
#include <atomic>
#include <algorithm>   // For std::min, std::max
#include <vector>
#include <deque>
#include <string>
#include <cmath>      // For std::sqrt, std::fabs
#include <mutex>
#include <chrono>
#include <cstdint>    // For INT64_MIN / INT64_MAX（长截图裁剪内容坐标哨兵）
#include <utility>     // For std::move（最近帧历史的匹配数据转移）

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


#include "screenshot_windows.h"

static const auto SC_PRIMED_FRAME_TTL = std::chrono::seconds(2);

struct PrimedScreenshotFrame {
    HBITMAP bitmap = NULL;
    int vx = 0;
    int vy = 0;
    int vw = 0;
    int vh = 0;
    double dpiScale = 1.0;   // 单一 scale 模型的已知限制见 CaptureContext.dpiScale 注释（CR-023）
    std::chrono::steady_clock::time_point capturedAt{};
    bool valid = false;
};

// 截图常量

// ==================== 区域截图功能（预截屏 + 双缓冲架构） ====================

static const int SC_PANEL_WIDTH = 140;

static const int SC_PANEL_HEIGHT = 140;

static const int SC_MAGNIFIER_HEIGHT = 74;

static const int SC_PANEL_MARGIN = 15;

static const int SC_PANEL_CORNER_RADIUS = 8;

static const int SC_ZOOM_FACTOR = 4;

// 取值 0~255，数值越大越暗（0 = 无遮罩，255 = 全黑）

// 选区外遮罩：微信风格，选区内部保持清晰，外部覆盖半透明黑色

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
    CS_LongCapturing, // 长截图滚动捕获进行中（独立于编辑态，由 RunLongCapture 驱动）
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
    RH_ArrowEnd = 9,     // 箭头终点端点手柄（仅箭头用，拖动改终点）
    RH_CornerRadiusTL = 10, // 选区左上角内倒角手柄（拖动改选区圆角半径，不改变选区矩形）
    RH_CornerRadiusTR = 11, // 选区右上角内倒角手柄
    RH_CornerRadiusBL = 12, // 选区左下角内倒角手柄
    RH_CornerRadiusBR = 13  // 选区右下角内倒角手柄
};

// 工具栏最左「6 点拖拽把手」单元格的命中返回值：取负值与按钮索引区分，
// 既有调用方以 >=0 判定按钮，天然排除把手（把手按住 = 拖动工具栏，非工具按钮）。
static const int SC_TB_GRIP = -2;

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
    TB_LongCapture,     // 长截图（滚动捕获入口，复用当前选区）
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

// 所有标注统一用「绝对虚拟屏幕坐标」存储（与 ctx->mouseX/selection 同坐标系）：
//   - 用绝对坐标而非选区相对，保证选区缩放/移动时标注位置固定不动
//   - 实时渲染时：backDC 局部坐标 = 绝对坐标 + ox/oy，ox/oy = -virtualX/-virtualY
//   - 合成进 PNG 时：finalDC 局部坐标 = 绝对坐标 + ox/oy，ox/oy = -rect.left/-rect.top

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
// inline constexpr 数组：跨 TU 唯一实例（替代 static const 每 TU 一份的拷贝），值与原一致。

inline constexpr int SC_THICK_PRESETS[] = { 1, 2, 4 };

inline constexpr int SC_THICK_COUNT = sizeof(SC_THICK_PRESETS) / sizeof(SC_THICK_PRESETS[0]);

static const int SC_DEFAULT_THICK_IDX = 1;  // 默认中粗

// 子菜单圆点预览直径（逻辑像素，仅用于界面显示，与实际绘制粗细解耦）

inline constexpr int SC_THICK_DOT_SIZES[] = { 5, 10, 16 };

inline constexpr int SC_THICK_DOT_COUNT = sizeof(SC_THICK_DOT_SIZES) / sizeof(SC_THICK_DOT_SIZES[0]);

// 文字字号预设（逻辑像素），文字工具激活时子菜单第一组显示

inline constexpr int SC_FONT_SIZES[] = { 16, 24, 36 };

inline constexpr int SC_FONT_COUNT = sizeof(SC_FONT_SIZES) / sizeof(SC_FONT_SIZES[0]);

// 粗细档数与字号档数必须一致：子菜单第一组共用单元格，绘制/命中按 isTextTool 在两套预设
// 间二选一（overlay_ui/overlay_input 均以 SC_THICK_COUNT/SC_FONT_COUNT 为循环上界）。
static_assert(SC_THICK_COUNT == SC_FONT_COUNT,
    "SC_THICK_COUNT must match SC_FONT_COUNT: submenu 第一组共享单元格");

static const int SC_DEFAULT_FONT_IDX = 1;  // 默认中号

static const wchar_t* SC_FONT_FACE = L"微软雅黑";

// 马赛克块大小预设（逻辑像素），马赛克工具子菜单显示

inline constexpr int SC_MOSAIC_SIZES[] = { 6, 10, 16 };

inline constexpr int SC_MOSAIC_COUNT = sizeof(SC_MOSAIC_SIZES) / sizeof(SC_MOSAIC_SIZES[0]);

static const int SC_DEFAULT_MOSAIC_IDX = 1;  // 默认中等块

// 这里单独定义便于扩展。半径越大涂抹范围越宽。

// 涂抹半径预设（逻辑像素），马赛克涂抹模式使用，与画笔粗细预设共用同一组子菜单第二组无效，

inline constexpr int SC_MOSAIC_RADIUS[] = { 12, 22, 36 };

inline constexpr int SC_MOSAIC_RADIUS_COUNT = sizeof(SC_MOSAIC_RADIUS) / sizeof(SC_MOSAIC_RADIUS[0]);

static const int SC_DEFAULT_MOSAIC_RADIUS_IDX = 1;  // 默认中等半径

// 颜色预设

inline constexpr COLORREF SC_COLOR_PRESETS[] = {
    RGB(0xE5, 0x39, 0x35),  // 红
    RGB(0xFB, 0x8C, 0x00),  // 橙
    RGB(0xFD, 0xD8, 0x35),  // 黄
    RGB(0x43, 0xA0, 0x47),  // 绿
    RGB(0x00, 0xAC, 0xC1),  // 青
    RGB(0x1E, 0x88, 0xE5),  // 蓝
    RGB(0xFF, 0xFF, 0xFF),  // 白
    RGB(0x33, 0x33, 0x33),  // 黑
};

inline constexpr int SC_COLOR_COUNT = sizeof(SC_COLOR_PRESETS) / sizeof(SC_COLOR_PRESETS[0]);

static const int SC_DEFAULT_COLOR_IDX = 0;  // 默认红

// ==================== 主题色常量 ====================
// 多个绘制文件共用的固定色值集中于此（此前以 RGB 字面量散落 overlay_ui / icons /
// long_capture 各处，易各自漂移）；各值与抽常量前逐字节一致。
// GDI 侧直接传 COLORREF；GDI+ 侧经下方 ScOpaqueColor 展开通道（透明度各异的
// 使用点自行以 GetR/G/BValue 构造）。

static const COLORREF SC_THEME_ACCENT_BLUE  = RGB(0x00, 0x88, 0xFF);  // 强调蓝：选区/标注边框、resize 手柄、放大镜准星
static const COLORREF SC_THEME_TOOLBAR_BLUE = RGB(0x3B, 0x8B, 0xF2);  // 工具栏选中态图标蓝 #3B8BF2（配浅蓝高亮底）
static const COLORREF SC_THEME_ICON_DARK    = RGB(0x33, 0x33, 0x33);  // 子菜单图标默认深灰 #333333（与预设「黑」同值）
static const COLORREF SC_THEME_SEL_BG       = RGB(225, 237, 253);     // 选中态浅蓝高亮底（工具栏主题蓝叠白底 ~15% 预混合色）
static const COLORREF SC_THEME_HOVER_BG     = RGB(235, 243, 255);     // hover 态极浅蓝底

// 不透明封装：COLORREF 主题色 -> alpha=255 的 Gdiplus::Color（通道按位展开，值不变）。

static inline Gdiplus::Color ScOpaqueColor(COLORREF c) {
    return Gdiplus::Color(255, GetRValue(c), GetGValue(c), GetBValue(c));
}

// 撤销栈最大深度（快照份数）。撤销历史是整份标注的深拷贝，不限深会随操作数平方级累积内存
// （每笔操作全量复制一次），超出后由 PushAnnotationHistory 裁掉最老快照。

static const int SC_UNDO_MAX_DEPTH = 50;

// 手柄/工具栏几何常量

static const int SC_HANDLE_SIZE = 10;       // 调整手柄边长（100% DPI 基准，运行时按 dpiScale 缩放）

static const int SC_TOOLBAR_BTN = 32;       // 按钮尺寸（正方形）

static const int SC_TOOLBAR_PAD = 6;        // 按钮↔工具栏边缘内边距（四边一致）

static const int SC_TOOLBAR_H = SC_TOOLBAR_BTN + SC_TOOLBAR_PAD * 2;  // 工具栏高度 = 按钮 + 上下内边距

static const int SC_TOOLBAR_GAP = 1;        // 按钮间距

static const int SC_TOOLBAR_RADIUS = 8;     // 工具栏圆角

static const int SC_TOOLBAR_MARGIN = 6;     // 选区到工具栏间距

static const int SC_TOOLBAR_BORDER = 1;     // 工具栏边框

static const int SC_MIN_SELECTION = 10;     // 最小选区尺寸

static const int SC_CORNER_KNOB_INSET = 18; // 圆角拖拽手柄距选区角的内缩距离（100% DPI 基准，运行时按 dpiScale 缩放）

static const int SC_CORNER_PROXIMITY = 14;  // 倒角手柄"靠近"感应余量：在命中框外再扩此距离即显示该角手柄（100% DPI 基准）

// 手柄几何（DPI 缩放后）。选区/标注 resize 手柄与圆角手柄共用同一套尺寸，
// 保证 1080p → 4K 下手柄与工具栏/图标同步放大，避免高 DPI 下手柄过小。
struct SCHandleMetrics {
    int handleSize;       // 手柄边长（绘制 + 命中框半宽基准）
    int cornerKnobInset;  // 圆角手柄距选区角的内缩距离
    int handleMargin;     // 脏区扩张余量 = handleSize/2 + 4（覆盖手柄半径 + 描边/抗锯齿）
    int cornerProximity;  // 倒角手柄靠近感应余量（命中框外扩展距离，鼠标进入即显示该角手柄）
};

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
    HBRUSH bgBrush = NULL;
    HPEN borderPen = NULL;
    HPEN crosshairPen = NULL;
    HPEN selectionPen = NULL;
    HPEN highlightPen = NULL;
    HFONT smallFont = NULL;
    int smallFontPx = 0;
    int crosshairWidth = 0;
    // 选区外遮罩缓冲（虚拟屏幕大小，纯黑 + 常量 alpha），用于 AlphaBlend
    HDC maskDC = NULL;
    HBITMAP maskBitmap = NULL;
    // ---- P2 性能优化：固定样式 Pen/Brush 会话级缓存，避免每帧 Create/Delete ----
    // 工具栏分隔线笔（DrawToolbar）。
    HPEN toolbarSepPen = NULL;     // PS_SOLID, 1, RGB(230,230,230)
    // 文字选择高亮画刷（AlphaBlend 半透明选区底色）。
    HBRUSH textSelBrush = NULL;    // RGB(51,153,255)
    // 悬停/选中标注边框：蓝色虚线笔（悬停文字/非文字标注 + 选中非文字标注共用）。
    HPEN annHoverPen = NULL;       // PS_DASH, 1, RGB(0,136,255)
    // 选中文字标注边框：蓝色实线粗笔（与 selectionPen 的宽度 1 区别）。
    HPEN annTextSelPen = NULL;     // PS_SOLID, 2, RGB(0,136,255)

    // 创建/释放方法体下沉到 session_windows.cpp（CR-022：唯一使用方在会话层，
    // 从头文件内联定义改为外部定义，调用方签名/语义不变）。
    void Init(int fontPx = 12, int crosshairPx = 1);
    // 创建遮罩缓冲（纯黑位图，配合常量 alpha 实现 40%+ 半透明遮罩）
    // 须在 CaptureContext 虚拟屏幕尺寸确定后调用
    void InitMask(int virtualW, int virtualH);
    void Cleanup();
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

    // Init/Cleanup 依赖 nanosvg 光栅化（kIconSvgs/RenderSvgToBitmap），实现在 icons_windows.cpp
    void Init(int physicalIconSize);
    void Cleanup();

    // 取按钮位图：isActive 时用主题蓝版本，其余用深灰
    HBITMAP Get(int btn, bool isActive) const {
        if (btn < 0 || btn >= TB_Count) return NULL;
        return isActive ? active[btn] : dark[btn];
    }
};

// 截图上下文

struct CaptureContext {
    CaptureState state = CS_Idle;
    // 自动确认模式：选区确定后直接提取并完成截图，不进入编辑态（工具栏/标注）。
    // 仅在 WM_LBUTTONUP 的 CS_Selecting 分支生效。
    bool autoConfirm = false;
    int virtualX = 0, virtualY = 0, virtualW = 0, virtualH = 0;
    int startX = 0, startY = 0, endX = 0, endY = 0;
    int mouseX = 0, mouseY = 0;
    COLORREF currentColor = 0;
    std::vector<SCWindowInfo> windows;
    int hoveredWindow = -1; // -1 = none
    // 预截屏
    HBITMAP screenBitmap = NULL;
    HDC memDC = NULL;
    // 双缓冲
    HDC backDC = NULL;
    HBITMAP backBitmap = NULL;
    // 脏区域追踪
    RECT lastPanelRect = {};
    RECT lastSelectionRect = {};
    RECT lastLabelRect = {};
    RECT lastHighlightRect = {};
    RECT lastToolbarRect = {};
    RECT lastPopupRect = {};
    // P1 局部刷新用：上帧光标/被操作标注/正在绘制标注的包围盒（供 InvalidateRect 计算旧位置）
    RECT lastCaretRect = {};       // 上帧文字光标矩形（backDC 坐标），hasLastCaret=false 表示无效
    bool hasLastCaret = false;
    RECT lastAnnotationBox = {};  // 上帧被拖拽/缩放标注的包围盒（绝对虚拟屏幕坐标）
    bool hasLastAnnotationBox = false;
    RECT lastDrawingBox = {};     // 上帧 curDrawing 包围盒（绝对虚拟屏幕坐标）
    bool hasLastDrawingBox = false;
    bool needFullRedraw = false;
    // DPI 缩放因子（逻辑像素 → 物理像素 = 乘以 dpiScale；物理 → 逻辑 = 除以）。
    // 【已知限制 —— 单一 scale 模型】CaptureVirtualScreen 把所有显示器的物理并集
    // BitBlt 进一张连续物理位图，再令 dpiScale = physVw / vw（物理并集宽 / 逻辑并集宽）。
    // 单显示器（含系统级统一 DPI 缩放）下该值精确；混合 DPI 多显示器下它是各屏 scale
    // 的加权混合值，无法还原为任一具体显示器——选区跨越不同 DPI 的屏幕时，按此单一
    // scale 做逻辑↔物理换算会产生系统性像素偏移（越界采样、坐标错位）。
    // 正确修复需升级为 per-monitor 模型：每个逻辑↔物理换算点按鼠标/选区所在显示器
    // 用 MonitorFromPoint+GetMonitorInfo+GetDpiForMonitor 取各自 scale，并改造
    // CaptureVirtualScreen 的整屏 BitBlt 与位图布局以保留各屏原始 DPI 采样（而非混合
    // 进单一连续网格）。该改动触及捕获/存储/渲染全链路且必须经真实多屏异 DPI 环境验证，
    // 当前任务禁运行时测试约束下无法安全实施，故仅标注已知限制 + 统一各换算点的写法。
    // 见 docs/CODE-REVIEW-ROADMAP.md CR-023。
    double dpiScale = 1.0;
    // GDI 资源
    SCGdiResources gdi;
    SCPanelMetrics panelMetrics;

    // ---- 确认态：可调整选区 ----
    // 已确认的选区（绝对屏幕坐标）
    RECT selection = {};
    // 当前正在拖拽的手柄（CS_Resizing 时有效），CS_Confirmed 下表示 hover 手柄
    int resizeHandle = RH_None;
    // 整体拖动/调整起点（绝对屏幕坐标）
    int dragStartX = 0, dragStartY = 0;
    RECT dragStartSelection = {};
    // 选区圆角半径（0=直角；上限=min(w,h)/2，由 ClampCornerRadius 保证）
    int selectionCornerRadius = 0;
    // 圆角手柄拖拽起始半径（RH_CornerRadiusTL/TR/BL/BR 拖拽用，增量映射）
    int dragStartRadius = 0;
    // 当前"靠近/拖拽"的倒角手柄角（RH_CornerRadiusTL/TR/BL/BR 之一；RH_None=未靠近）。
    // 仅鼠标靠近某角或正拖拽某角时显示该角一个倒角手柄，其余时刻隐藏。
    int hoveredCornerHandle = RH_None;
    // 键盘方向键微调累计位移（CS_Resizing 时叠加到鼠标位移上，松开时一并固化）
    int kbDX = 0;
    int kbDY = 0;

    // ---- 悬浮工具栏 ----
    // 工具栏矩形（相对虚拟屏幕坐标，绘制用）
    RECT toolbarRect = {};
    // 用户按住最左「6 点把手」拖动过后置位：此后 OnPaint 直接沿用 toolbarRect，
    // 不再随选区自动重算（会话内拖动/缩放选区时工具栏保持用户放置的位置）
    bool toolbarPlaced = false;
    // 正在拖动工具栏（把手左键按下未松开）：MOUSEMOVE 平移 toolbarRect 并局部刷新
    bool toolbarDragging = false;
    int toolbarDragStartX = 0, toolbarDragStartY = 0;  // 按下时鼠标位置（绝对屏幕坐标）
    RECT toolbarDragStartRect = {};                    // 按下时的工具栏矩形（相对坐标）
    // 工具栏 hover 按钮，-1 = none（SC_TB_GRIP = 悬停在拖拽把手上）
    int hoverToolbarBtn = -1;
    // ---- 工具栏 title 式 tooltip（网页 title 同款：悬停停顿出现深色圆角气泡）----
    // 由会话空闲循环轮询维护（TickToolbarTooltip），气泡画进 backDC（DrawToolbarTooltip）
    int tipBtn = -1;                 // 当前停顿目标按钮（-1 = 无；分隔线无 tooltip）
    DWORD tipDwellSince = 0;         // 光标进入目标按钮的起始时刻（毫秒）
    bool tipShown = false;           // 气泡当前是否在屏（负责自身矩形的失效重绘）
    RECT tipBubbleRect = {};         // 气泡矩形（backDC 相对坐标）
    std::wstring tipText;            // 气泡文本
    // 当前激活的工具（高亮显示，仅界面）
    int activeTool = -1;
    // 当前子菜单/参数面板对应的工具来源；拖拽工具下选中覆盖物时可继续回显其参数。
    int popupTool = -1;
    // 工具栏图标位图缓存（按 DPI 预渲染，dark/white 双色）
    SCIconCache iconCache;
    // 当前 DPI 下的工具栏几何（缓存，避免每次绘制重算）
    SCToolbarMetrics toolbarMetrics;
    // 当前 DPI 下的手柄几何（缓存：选区/标注 resize 手柄 + 圆角手柄）
    SCHandleMetrics handleMetrics;

    // ---- 标注绘制 ----
    std::vector<Annotation> annotations;   // 已提交标注
    // 撤销/重做快照栈（队首=最老）。undoStack 由 PushAnnotationHistory 写入并限深 SC_UNDO_MAX_DEPTH；
    // 重做路径（UndoAnnotations→redoStack / RedoAnnotations→undoStack）每次入栈前必有一次对应的
    // 出栈，数学上不会超过同一上限。
    std::deque<std::vector<Annotation>> undoStack;
    std::deque<std::vector<Annotation>> redoStack;
    Annotation curDrawing;                 // CS_Drawing 中正在绘制的标注
    bool hasCurDrawing = false;             // curDrawing 是否有效
    int drawColorIdx = 0;                   // 当前选中颜色索引
    int drawThickIdx = 0;                   // 当前选中粗细索引（矢量工具）
    int fontSizeIdx = 0;                    // 当前选中字号索引（文字工具）
    // 马赛克工具属性
    int mosaicSizeIdx = 0;                  // 当前选中马赛克块大小索引
    int mosaicRadiusIdx = 0;                // 当前选中涂抹半径索引
    bool mosaicRectMode = false;           // true=框选区域模式；false=涂抹模式
    // 涂抹模式光标：用系统光标机制（SetCursor）显示半径圆，由 OS 跟随鼠标，
    // 无 WM_PAINT 重绘延迟（之前的 overlay 圆走 MOUSEMOVE→InvalidateRect→WM_PAINT 链路，
    // 全屏重绘开销大导致不跟手）。按半径预设预生成彩色光标并缓存。
    HCURSOR mosaicBrushCursors[SC_MOSAIC_RADIUS_COUNT] = {};  // 对应 SC_MOSAIC_RADIUS_COUNT 个半径预设的光标
    bool mosaicBrushCursorsInited = false;
    // ---- 马赛克渲染（reveal-mask 模型，消除不连续感）----
    // 预先把整张截图按当前块大小马赛克化得到 mosaicBase（逻辑像素，与 backDC 同尺寸）。
    // 马赛克标注只是「蒙版」：涂抹=路径圆形区域、框选=矩形区域，揭示其背后的 mosaicBase。
    // 这样任意区域、任意顺序叠加都连续无缝；切换块大小时只需重建 base，已揭示区域自动更新。
    // mosaicBase 覆盖整虚拟屏幕（绝对坐标），与选区无关，resize/move 无需重建。
    HDC mosaicBaseDC = NULL;
    HBITMAP mosaicBaseBitmap = NULL;
    int mosaicBaseW = 0, mosaicBaseH = 0;          // base 尺寸（= 虚拟屏幕逻辑尺寸）
    int mosaicBaseBlockPx = 0;                 // 生成 base 时的块大小（检测变更触发重建）
    // 涂抹模式增量绘制：记录上一帧最后绘制的路径点索引（reveal 模型下未使用，保留扩展）。
    int mosaicDrawLastIdx = 0;
    // 粗细/颜色子菜单
    bool popupOpen = false;
    RECT popupRect = {};
    SCPopupMetrics popupMetrics;

    // ---- 文字输入（CS_TextEditing）----
    std::wstring textBuf;                  // 正在输入的文字缓冲
    int textAnchorX = 0, textAnchorY = 0;          // 文字锚点（绝对虚拟屏幕坐标）
    int textCaretPos = 0;                      // 插入符在 textBuf 中的 wchar 位置
    bool textCaretVisible = false;                 // 光标是否可见（闪烁控制）
    DWORD textCaretLastBlink = 0;              // 上次光标闪烁时间（毫秒）
    int textSelStart = -1;                      // 文字选择起始位置（-1 表示无选择）
    int textSelEnd = -1;                        // 文字选择结束位置
    bool textDraggingSelection = false;            // 是否正在拖动选择文字
    int hoveredTextAnnotation = -1;             // 悬浮命中的文字标注索引（-1 表示无，仅用于光标/即时反馈）
    int selectedTextAnnotation = -1;            // 已选中的文字标注索引（-1 表示无，持久保持直到点空白）
    int draggingTextAnnotation = -1;            // 正在拖动的文字标注索引（-1 表示无）
    int textDragStartX = 0, textDragStartY = 0;    // 文字拖动起始位置
    // ---- 非文字标注的选中/拖拽/缩放（与文字机制互斥：选中非文字时清文字选中，反之亦然）----
    int hoveredAnnotation = -1;                 // 悬浮命中的非文字标注索引（-1=无，用于虚线框/光标即时反馈）
    int selectedAnnotation = -1;                // 已选中的非文字标注索引（-1=无，持久保持直到点空白/进入其他操作）
    int draggingAnnotation = -1;                // 正在拖拽的非文字标注索引（-1=无）
    int resizingAnnotation = -1;                // 正在缩放的非文字标注索引（-1=无）
    int annotationResizeHandle = RH_None;            // 当前缩放手柄（RH_None=无；CS_Resizing 时为四角之一）
    int annotationDragStartX = 0, annotationDragStartY = 0;  // 鼠标按下位置（绝对坐标，拖拽/缩放共用）
    Annotation dragStartAnnotation;                   // 按下时标注快照（拖拽时还原+平移）
    bool annotationOpHistoryPushed = false;
    RECT annotationResizeStartBox = {};                    // 按下时包围盒（缩放时基准）

    // ---- GDI+ 会话级资源（性能优化：会话内单次 Startup/Shutdown）----
    // 原实现每个绘制/测量函数各自 GdiplusStartup/Shutdown，每帧 WM_PAINT 触发 6~10 次昂贵的
    // GDI+ 初始化，是拖拽卡顿的主因。由于所有 GDI+ 调用均在 ScreenshotCaptureThread 单线程内，
    // 改为会话开始 Startup 一次、结束 Shutdown 一次。FontFamily(SC_FONT_FACE) 与
    // StringFormat(总是 Near/Near) 为常量；Font 仅依赖 fontPx（文字字号仅 SC_FONT_SIZES 三档），
    // 均缓存复用。Graphics 仍每次按 hdc 新建（必须，因为绑定不同 DC）。
    ULONG_PTR gdipToken = 0;                  // GDI+ 启动令牌（0 = 未初始化）
    Gdiplus::GdiplusStartupInput gdipStartupInput;
    bool gdipInited = false;                      // GDI+ 是否已 Startup
    Gdiplus::FontFamily* gdipFontFamily = nullptr;  // SC_FONT_FACE，会话内唯一
    Gdiplus::StringFormat* gdipStrFmt = nullptr;    // Near/Near，会话内唯一
    Gdiplus::Font* gdipFonts[3] = {};          // 按 SC_FONT_SIZES 预建的 Font（索引对齐 SC_FONT_COUNT）
};

// ==================== 长截图（手动滚动捕获） ====================

// 进入方式：在编辑态（CS_Confirmed）点工具栏「长截图」按钮 → BeginLongCapture
// 隐式切换到 CS_LongCapturing，通过 PostMessage(WM_LONGCAPTURE_RUN) 在覆盖层
// 窗口过程中执行 RunLongCapture。
//
// 快照选区 → 隐藏全屏覆盖层 → 创建侧边预览面板（缩略小地图 + 完成/取消）→
// 滚轮停稳后采样一帧并增量拼接到小地图；向下滚追加到底部，向上滚前插到头部。
//
// 交互形态为手动模式：用户自己在选区上滚动鼠标滚轮（Raw Input 被动观察，不拦截输入），
//
// 独立于 CaptureContext 编辑态状态机的轻量上下文。复用截图会话的线程与
// threadsafe 回调（g_screenshotTsfn），但滚动/拼接逻辑全部在此处实现，
// 不触碰 CaptureContext 的标注/工具栏机制。

#define WM_LONGCAPTURE_RUN (WM_APP + 200)

// 行细节量 = 行内相邻采样列的灰度跳变数——空白/均匀区细节为 0，其上的“匹配”不可信
// （在任何位移都能“匹配”，只会污染评分），以行权重（细节量截断到 0~4）参与评分，
// 空白行天然不计分；整帧细节总量过低则无对齐依据。
//
// 匹配用帧数据：量化灰度行（h × cols）+ 每行匹配权重。

// 多 ROI 独立验证的列带数量（左/中/右）。列带把采样列均分为三段，
// 每段独立产出 Top-N 候选后做跨 ROI 加权聚类，防单个区域误匹配决定拼接结果。
static const int LC_ROI_BANDS = 3;

// 多尺度垂直结构 profile 的列桶数：每行按采样列均分为若干桶取平均量化灰度，
// 再做 4/8 行滑动聚合，供候选位移的结构一致性辅助验证（LongCaptureProfileCorrelation）。
static const int LC_PROFILE_BUCKETS = 8;

struct LongMatchData {
    std::vector<uint8_t> gray;      // 4bit 量化灰度，行距 cols
    std::vector<uint8_t> weight;    // 每行匹配权重 0~4（= min(4, 行细节量）)
    std::vector<uint8_t> bandWeight[LC_ROI_BANDS]; // 每行各列 ROI（左/中/右）权重 0~4
    std::vector<uint16_t> edge;     // 每行一维垂直结构强度：相邻采样列量化灰度差绝对值之和
                                    //（Weak 档匹配的边缘结构特征，见 LongCaptureEdgeCorrelation）
    std::vector<float> profile4;    // 4 行聚合垂直结构 profile：h 行 × LC_PROFILE_BUCKETS 列桶，
                                    // 桶值 = 窗口内行桶均值的平均（多尺度结构一致性验证用）
    std::vector<float> profile8;    // 8 行聚合 profile（同上，更粗尺度）
    int cols = 0;
    int bandStart[LC_ROI_BANDS] = {0, 0, 0};  // 各 ROI 起始采样列
    int bandCols[LC_ROI_BANDS] = {0, 0, 0};   // 各 ROI 采样列数
    int h = 0;
    int detailSum = 0;              // 全帧行细节量总和（整帧近乎均匀时对齐不可信）
};

// 匹配模式（双模式识别）：按候选位移的重叠量分档，overlap 越小要求证据越强。
enum class LongCaptureMatchMode {
    Normal,       // 常规匹配：overlap >= max(24, 15% 视口高)，沿用现有阈值与多数派 ROI 聚合
    WeakOverlap   // 弱重叠/大跳变匹配：overlap >= max(32, 5% 视口高)，以显著更严格的
                  // 交叉验证（3/3 ROI 全票 + 边缘结构相关 + 动态高置信度 + 延迟确认）换取可识别性
};

// 匹配状态（三值结论）：只有 SUCCESS 允许进入拼接提交。
// LOW_CONFIDENCE / FAILED 一律按“本帧被拒绝”处理（重试/计失败），
// 绝不允许用兜底 offset、上一帧推导 offset 或整帧插入的方式“继续流程”。
enum LongMatchStatus {
    LC_MATCH_FAILED = 0,        // 无可信对齐（无候选峰 / ROI 相互矛盾）
    LC_MATCH_LOW_CONFIDENCE,    // 有候选但置信度不足（宽松档/单 ROI/置信度低于阈值）
    LC_MATCH_SUCCESS            // 可信对齐（严格档 + 多 ROI 一致 + 置信度达标）
};

// 识别失败的细分类别（可观测性增强：失败不再统一记为 FAILED，归类见 DetectPass /
// TryStitch 各拒绝点）。只用于失败归因、调试日志与重试节奏选择（瞬态 vs 结构性），
// 不参与任何验收阈值判定；成功路径一律保持 None。
enum class LCFailReason {
    None = 0,
    NoCandidate,          // NO_CANDIDATE：全部 ROI 无候选 / 整帧无对齐依据
    CandidateWeak,        // CANDIDATE_WEAK：有候选但整体匹配或综合置信度不足
    PeakAmbiguous,        // PEAK_AMBIGUOUS：峰值分离度不足（周期性/重复内容歧义）
    GlobalMismatch,       // GLOBAL_MISMATCH：全宽富验证不可评（重叠区近乎全空白）
    SeamMismatch,         // SEAM_MISMATCH：接缝窗证据不足
    SpatialMismatch,      // SPATIAL_MISMATCH：top/middle/bottom 空间一致性崩塌
    ContinuityMismatch,   // CONTINUITY_MISMATCH：匹配分布碎片化（连续性不足）
    ProfileMismatch,      // PROFILE_MISMATCH：多尺度垂直结构不一致
    RoiInconsistent,      // ROI_INCONSISTENT：跨 ROI 候选冲突 / 聚类支持不足
    OffsetImplausible,    // OFFSET_IMPLAUSIBLE：offset 被历史合理性校验拒绝
    DirectionConflict,    // DIRECTION_CONFLICT：最佳候选与最近滚动方向相反
    FrameUnstable         // FRAME_UNSTABLE：稳定性检测未过（本帧未进入正式匹配）
};

// 识别阶段输出（纯检测结果）：未经 offset 合理性校验前不得用于提交。
struct LongMatchOutcome {
    LongMatchStatus status = LC_MATCH_FAILED;
    LongCaptureMatchMode mode = LongCaptureMatchMode::Normal; // 本次候选的匹配档位
    int offset = 0;                  // 候选位移 d（>0 向下滚，<0 向上滚；|d|≤1 视为未滚动）
    int overlap = 0;                 // 候选位移对应的重叠行数（= h − |d|）
    float overall = 0.0f;            // 全宽重叠区加权匹配率
    float seam = 0.0f;               // 接缝窗加权匹配率
    float top = 0.0f;                // 重叠区上 1/3 加权匹配率（该段无有效行时记 0）
    float middle = 0.0f;             // 重叠区中 1/3 加权匹配率
    float bottom = 0.0f;             // 重叠区下 1/3 加权匹配率
    float spatial = 0.0f;            // top/middle/bottom 空间一致性综合分（局部假匹配在此崩塌）
    float continuity = 0.0f;         // 匹配分布连续性（最长连续段占比 + 匹配率 − 断点罚）
    float profileScore = 0.0f;       // 4/8 行聚合 profile 多尺度垂直结构一致度
    float edgeCorrelation = 0.0f;    // 重叠区行边缘结构强度的归一化相关度（Weak 档强证据）
    float peakGap = 0.0f;            // 与次优候选的综合分差（归一化；1 = 无竞争峰）
    float roiWeighted = 0.0f;        // 跨 ROI 加权证据（按 ROI 信息量加权融合的支持度）
    float confidence = 0.0f;         // 综合置信度（全宽匹配 + 空间/连续性 + 多尺度结构 +
                                     //  ROI 加权证据 + 峰值分离度的加权组合，见 DetectPass）
    LCFailReason reason = LCFailReason::None; // 拒绝原因分类（成功时保持 None；仅归因/日志/重试节奏用）
    float textureRatio = 0.0f;       // 重叠区有效纹理行占比（证据量计价，进入综合置信度）
    int bandOffsets[LC_ROI_BANDS] = {0, 0, 0};   // 各 ROI 独立求得的位移
    bool bandValid[LC_ROI_BANDS] = {false, false, false}; // 该 ROI 是否产出可信候选
    int validBandCount = 0;          // 产出候选的 ROI 数
    int agreeCount = 0;              // 最终候选所在聚类的支持 ROI 数（加权融合后）
};

// 单帧采样结论（识别→校验→提交管线的对外语义）。
enum class LCSampleOutcome {
    Stitched,    // 已安全提交拼接且新增了拼接行（唯一扩展累计拼接内容的结局）
    Repositioned,// 匹配成功但新视口完全落在已捕获内容范围内（反向回滚未越过捕获边界）：
                 // 只推进当前视口基准（lastFrame/committedContentTop）并移动小地图当前
                 // 区域标注，不新增行、不计帧数——已捕获内容的重复帧绝不再次拼接
    NoChange,    // 匹配成功但内容未滚动（d=0）；与“匹配失败”“滚动到底”均独立
    WeakPending, // Weak 候选首次成立：只登记待复核候选（pendingMatch），不提交、不改任何累计状态
    WeakRejected,// Weak 候选被复核否决（第二次候选不一致/置信度跌破门槛/offset 异常）：
                 // 与“完全无候选”的硬失败不同，不计入普通失败计数，走独立重试预算
    Unstable,    // 稳定性检测未过（页面仍处滚动/重绘/懒加载过渡）：本帧不进入正式匹配，
                 // 不提交、不修改任何累计状态（含跟踪/历史/Weak 候选），由采样主循环
                 // 短延迟后重新采样，等待页面稳定
    Failed       // 硬失败（无候选/验证崩塌/Normal offset 异常）；累计状态保持原样
};

// Weak Match 的“延迟确认提交”候选（防污染核心机制）：首次弱重叠可信候选只登记、
// 不提交；下一次稳定采样必须独立复现一致候选（|Δoffset| ≤ LC_WEAK_CONFIRM_OFFSET_TOL
// 且置信度不跌破动态门槛）才允许 Commit。候选被否决/放弃时只清除本结构，
// 绝不触碰累计拼接状态（body/headRev/lastFrame/lastMatch/offsetHistory）。
struct LongCapturePendingMatch {
    bool valid = false;
    int offset = 0;
    float confidence = 0.0f;
    LongCaptureMatchMode mode = LongCaptureMatchMode::Normal;
};

// 最近帧历史条目（多跳匹配恢复的基准池）。内容坐标 = 以首帧视口顶为原点、不随头部
// 前插平移的滚动空间坐标（拼接图内位置 = contentY + headRows）。
// 只保存匹配所需数据（量化灰度/行权重/ROI 权重/profile 等，约 1MB 级/帧），
// 不保存完整位图；环形容量见 LC_HISTORY_FRAMES（long_capture_windows.cpp）。
// contentY 语义：已提交帧精确（可作跨帧提交链锚点）；未提交帧为 tentative 估计。
struct LongCaptureFrameHistory {
    int frameId = -1;                // 采样序号（识别 lastFrame 直连条目 / 日志关联）
    LongMatchData match;             // 该帧匹配数据（LongCaptureBuildMatchData 全量产物）
    int64_t contentY = 0;            // 视口顶内容坐标（已提交=精确；未提交=估计）
    bool committed = false;          // 已提交帧：位置精确、可作提交链锚点
    bool validForMatching = true;    // 近乎空白的帧不可作多跳匹配基准
};

// 长截图工具栏二级菜单类型：方向与裁剪均为图标 popover（同一套展开/绘制/命中机制，
// long_capture_windows.cpp），popover 悬停各 cell 有 title 式 tooltip
enum LCMenuKind {
    LCM_None = 0,     // 无展开菜单
    LCM_Direction,    // 方向 popover：纵向/横向图标 cell，悬停/点击方向按钮展开（已拼接多帧后锁定）
    LCM_Crop          // 裁剪 popover：悬停/点击裁剪按钮展开的图标浮层（丢弃上方/下方内容，随方向切换左右变体；已裁剪时含重置）
};

struct LongCaptureContext {
    // 选项（由 start() 的 options.longCapture 注入，会话开始时拷贝）
    int maxFrames = 100;    // 最大拼接帧数（防无限增长，默认与 CaptureContext.lcMaxFrames 一致）
    int interval = 250;     // 滚轮停止后等待内容稳定的毫秒数（采样防抖）

    // 虚拟屏幕（逻辑坐标）与 DPI
    int vx = 0, vy = 0, vw = 0, vh = 0;
    double dpiScale = 1.0;   // 单一 scale 模型的已知限制见 CaptureContext.dpiScale 注释（CR-023）

    // 选区（逻辑虚拟屏幕坐标，已规范化）
    RECT selection = {};

    // 采样裁剪矩形（逻辑虚拟屏幕坐标）：选区每边内缩 LC_CROP_INSET_LOGI，
    // 避开选区框描边与边缘抗锯齿像素污染拼接结果；滚轮过滤与结果 rect 均以此为准。
    RECT cropRect = {};

    // 选区物理像素裁剪参数（相对虚拟屏幕原点的物理偏移 + 尺寸）。
    // 纵向模式 physW/physH 即帧尺寸；横向模式（horizontal=true）帧缓冲为屏幕采样的
    // 转置（physW=capH、physH=capW），下游匹配/拼接/缩略图管线与纵向完全同构。
    int physX = 0, physY = 0, physW = 0, physH = 0;
    // 屏幕采样 DIB 尺寸（未转置的物理宽高；方向切换只交换 physW/physH，DIB 不变）
    int capW = 0, capH = 0;
    // 虚拟屏幕物理原点（屏幕 DC 坐标，DPI 感知下为物理像素），直接区域采样时作 BitBlt 源偏移
    int physOriginX = 0, physOriginY = 0;
    // 专用 DIB 段：每帧采样的可复用目标（避免反复新建位图 + GetDIBits 回读）
    HDC dibDC = NULL;
    HBITMAP dibBmp = NULL;
    void* dibBits = nullptr;
    int dibW = 0, dibH = 0;

    // 预览面板放置在选区上方（空间兜底时的退化形态）：向下生长受选区顶边约束，
    // 避免面板长大后重新覆盖选区导致自身入画
    bool panelAbove = false;

    // 拼接结果：物理像素 BGRA，宽度恒为 physW；高度 stitchH 增长。
    // 分两段存储以让拼接保持 O(新增行)：向上滚新增行存 headRev（倒序，reverse 后为显示顺序），
    // 向下滚新增行存 body（正序）；实际图像 = reverse(headRev) + body。
    std::vector<uint32_t> headRev;
    std::vector<uint32_t> body;
    int headRows = 0, bodyRows = 0;
    int stitchH = 0;
    // 最近一次采样的视口帧（相邻帧重叠检测基准，缓冲随拼接迭代复用）
    std::vector<uint32_t> lastFrame;
    // lastFrame 的模糊对齐数据（两帧采样之间 lastFrame 不变，重叠检测直接复用，
    // 只在拼接成功后随 lastFrame 旋转更新）
    LongMatchData lastMatch;

    // 面板两级缩略图：先按固定列宽（面板预览宽）增量缩列，绘制时再合并缩行。
    // 分段与拼接缓冲同构（thumbHeadRev 倒序 / thumbBody 正序），thumbMerged 为绘制用临时合并缓冲。
    std::vector<uint32_t> thumbHeadRev;
    std::vector<uint32_t> thumbBody;
    std::vector<uint32_t> thumbMerged;
    int thumbW = 0;        // 缩略图列宽（≤ physW）
    int thumbHeadH = 0;    // 头部段行数（合并后显示于顶部）
    int thumbH = 0;        // 缩略图总行数
    bool thumbDirty = false;

    // 滚轮观察状态（面板 WM_INPUT 经本线程消息泵分发，与主循环同线程，无需原子）
    bool wheelPending = false;
    DWORD lastWheelTick = 0;
    DWORD lastSampleTick = 0;   // 最近一次采样轮触发时刻（滚动中主动采样节拍用；首帧后初始化）
    int lastDir = 0;        // +1 最近一次向下滚（追加底部）；-1 向上滚（前插头部）

    // —— 拼接失败恢复与 offset 合理性校验状态 ——
    // 这些字段只在「成功提交拼接」后更新；识别失败/被拒绝的帧绝不写入，
    // 保证单帧误识别无法通过污染匹配基准或位移历史把错误扩散到后续帧。
    std::vector<int> offsetHistory;   // 最近若干次成功拼接的 |d|（px，长度上限见 LC_OFFSET_HISTORY_LEN）
    int noChangeCount = 0;            // 连续「内容未变化」采样数（仅匹配成功且 d=0 时递增；与失败完全独立）
    bool reachedBottom = false;       // 滚动到底（由连续多次内容未变化确认；匹配失败绝不置位）

    // —— Weak（低重叠大跳变）延迟确认与独立重试状态 ——
    // pendingMatch 只在本结构内暂存候选，任何未确认路径都不修改累计拼接状态；
    // weakTries 计连续未决的 Weak 采样轮数，达 LC_WEAK_MAX_TRIES 即放弃当前候选链重新观察。
    LongCapturePendingMatch pendingMatch; // 待复核的 Weak 候选（有效时驱动无滚轮的继续采样）
    int weakTries = 0;                    // 连续未决的 Weak 候选采样轮数
    int sampleIndex = 0;                  // 采样序号（LC_DEBUG_LOG 调试日志用）

    // —— Tentative 视觉跟踪状态（与正式拼接状态完全解耦的"预计当前位置"层）——
    // 正式拼接位置（committed）= committedContentTop，仅 LongCaptureCommitStitch 推进；
    // 预计当前位置（tentative）= tentativeContentTop，由视觉验证（多跳匹配/零位移对齐/
    // 全同帧）直接设定或轻量预测推进。FAILED / LOW_CONFIDENCE 帧只允许触碰本组字段，
    // 绝不修改 body/headRev/stitchH、lastFrame/lastMatch、offsetHistory 与拼接缓冲。
    // 防漂移约束：相对 committed 漂移上限（2×视口高）+ 连续无视觉依据冻结
    //（见 LongCaptureTrackingSetVisual / LongCaptureTrackingAdvancePredicted）。
    int64_t committedContentTop = 0;   // lastFrame 视口顶（内容坐标；CommitStitch 内 += d）
    int64_t tentativeContentTop = 0;   // 预计当前视口顶（内容坐标；SUCCESS 后与 committed 对齐）
    bool tentativeValid = false;       // 是否已建立可信跟踪（小地图据此显示虚线预计框）
    float tentativeConfidence = 0.0f;  // tentative 置信度（视觉验证来源高，预测链逐帧衰减）
    int trackUnreliableStreak = 0;     // 连续无视觉依据采样数（达阈值冻结预测推进）
    int trackingRevision = 0;          // 跟踪状态变更计数（RunLongCapture 据此刷新小地图）
    int lastCommittedFrameId = 0;      // 最新已提交帧序号（多跳回溯时跳过 hop1 直连基准）
    std::vector<LongCaptureFrameHistory> frameHistory; // 最近帧环形历史（容量 LC_HISTORY_FRAMES）
    std::vector<int> weakCandidateOffsets;             // 最近弱重叠候选位移（Weak 时间一致性样本）

    // —— 帧稳定性检测（进入正式 DetectMatch 前的准入闸门，纯诊断层）——
    // stableRef* = 上一次抓帧的 4bit 灰度稀疏采样，供与本次抓帧对比判断页面是否仍在
    // 快速变化（滚动动画/重绘/懒加载过渡）。每次抓帧后滚动更新；只在稳定性闸门启用
    // 且参考帧新鲜（间隔 ≤ LC_STABLE_REF_MAX_GAP）时参与判定。绝不参与 offset 评分、
    // 不触碰任何累计拼接状态（含跟踪/历史/Weak 候选）。
    std::vector<uint8_t> stableRefGray;
    int stableRefCols = 0;
    int stableRefH = 0;
    DWORD stableRefTick = 0;
    bool stableRefValid = false;

    // —— 失败可观测性：最近一次拒绝的原因分类与最佳候选证据快照 ——
    // 仅调试日志与采样主循环的重试节奏选择（瞬态 vs 结构性）使用，不参与任何判定。
    LCFailReason lastFailReason = LCFailReason::None;
    LongMatchOutcome lastReject;

    // —— 滚轮 delta 软先验（仅用于候选排序加分，绝不作为期望区间硬约束） ——
    // wheelAccumDelta 在面板 WM_INPUT 累计，成功提交时折算 px/notch 并 EMA 平滑；
    // 失败采样绝不更新估计。Windows/浏览器/平滑滚动/触控板都使位移与 notch 非线性相关，
    // 因此先验只轻微影响同档候选的排序先后（LC_WHEEL_PRIOR_BONUS），不改变任何阈值。
    int wheelAccumDelta = 0;          // 自上次成功提交以来累计的滚轮增量（带符号，WHEEL_DELTA=120）
    float pixelsPerWheelNotch = 0.0f; // 「成功像素位移 ↔ 滚轮 notch」在线估计（0 = 尚无估计）

    // 控制信号（跨线程：abortLongCapture 由 JS 线程设置）
    std::atomic<bool> abortFlag{false};
    std::atomic<bool> finishFlag{false};   // 用户点「完成并复制」
    std::atomic<int> frameCount{0};

    // —— 选区底部工具栏（宽高标签 / 方向 / 自动滚动 / 裁剪 / 保存 / 取消 / 完成并复制）——
    bool horizontal = false;     // 长截图方向：false=纵向（默认）；true=横向（帧缓冲转置复用纵向管线）
    bool autoScroll = false;     // 自动滚动（默认关闭）：开启时光标一次性移到选区中心，此后定时器只注入滚轮
                                  //（SendInput 按光标下方窗口路由；单拍不动鼠标，见 LongCaptureAutoScrollTick）
    std::atomic<bool> saveFlag{false};  // 用户点「保存到本地」：主循环内弹保存对话框并直接落盘
    // —— 裁剪状态（全部用「内容坐标」表达：拼接图行 = 内容坐标 + headRows）——
    // 内容坐标不随头部前插/主体追加平移，裁剪线因此永远锚定在页面的同一位置，
    // 不会因继续滚动导致窗口错位（旧的绝对拼接行号在前插后会整体漂移）。
    // 裁剪动作只收紧输出行窗口并登记「待剔除区间」，绝不立即修改拼接缓冲与匹配基准；
    // 待剔除区间在第一次朝该方向的成功提交时物理删除（见 CommitStitch 入口的延迟剔除），
    // 删除后该侧边界重新开放——继续滚动的新增内容直接续接在裁剪线之后继续拼图。
    int64_t cropTopY = INT64_MIN;      // 输出窗口上界（内容坐标）；INT64_MIN = 该侧开放（未裁剪）
    int64_t cropBottomY = INT64_MAX;   // 输出窗口下界（内容坐标）；INT64_MAX = 该侧开放（未裁剪）
    bool cropPendTop = false;          // 待剔除「上方已捕获内容」（丢弃上方登记）：下一次 d<0 提交时删除
    int64_t cropPendTopLo = 0, cropPendTopHi = 0;     // 待删内容区间 [lo, hi)
    bool cropPendBottom = false;       // 待剔除「下方已捕获内容」（丢弃下方登记）：下一次 d>0 提交时删除
    int64_t cropPendBottomLo = 0, cropPendBottomHi = 0;
    bool cropped = false;              // 是否存在任何生效的裁剪约束（重置菜单项/图标徽标显示用；
                                       // 含尚未触发的待剔除区间，触发或重置后自动回收）
    int tbHover = -1;            // 工具栏 hover 项（LongToolbarItem，-1=无）
    int tbPressItem = -1;        // 工具栏按下项（WM_LBUTTONDOWN 命中，-1=无）：UP 必须命中与
                                 // DOWN 相同的目标才触发点击——编辑工具栏「长截图」按钮在按下
                                 // 瞬间创建本工具栏，残留的松开事件绝不能误触恰好同位的按钮
    int tbPressMenuRow = -1;     // 二级菜单 popover 按下的 cell（-1=无；语义同 tbPressItem）
    bool tbDragging = false;     // 正在拖动工具栏窗口（按住最左 6 点把手，鼠标捕获中）
    int tbDragGrabDX = 0, tbDragGrabDY = 0;  // 按下点相对工具栏窗口左上角的偏移（物理像素）
    LCMenuKind menuKind = LCM_None;  // 展开中的二级菜单类型（方向/裁剪均为图标 popover）
    int menuHover = -1;          // 二级菜单 popover hover 的图标 cell（-1=无）
    bool menuBelow = false;      // 二级菜单绘制在工具栏下方（朝避让选区的一侧展开，见 LongCaptureSetMenu）
    LCMenuKind popHoverDisarm = LCM_None;  // 「悬停展开」被解除武装的菜单锚点（LCM_None=全部武装）：
                                           // 点击收起其 popover 后置为该菜单，光标移出锚点前不再
                                           // 因悬停重开，防止点击收起与悬停展开互相打架（方向/裁剪通用）
    std::vector<uint32_t> thumbDisplay;  // 横向模式显示用回转缩略图（纵向直接用 thumbMerged）
    int thumbDisplayW = 0, thumbDisplayH = 0;
    bool thumbDisplayDirty = false;
    int autoFailStreak = 0;      // 自动滚动中连续硬失败采样轮数（达上限自动停止，防丢内容）

    // 输出结果（物理像素）
    int outWidth = 0, outHeight = 0;
    std::string base64;
    bool success = false;
};

// 显示器枚举回调数据

struct MonitorEnumData {
    LONG minLeft, minTop, maxRight, maxBottom;
    double totalDpiScale;
    int monitorCount;
    HMODULE shcore;  // 外层一次 LoadLibraryW("shcore.dll") 的句柄，回调内复用 GetDpiForMonitor（避免每显示器重复 Load）
};

// 采样裁剪相对选区每边内缩（逻辑像素）：避开选区框描边与边缘抗锯齿，防止污染拼接内容。

static const int LC_CROP_INSET_LOGI = 2;

// ---- 可变全局变量（定义见各归属 .cpp）----
// 访问规则（CR-022：此前线程归属散落在各字段注释，现集中声明）：
//   线程模型——本模块仅两条线程访问这些全局：JS 线程（NAPI 导出 start/abort 等）与
//   截图捕获线程（ScreenshotCaptureThread，串行创建所有覆盖层窗口并派发其消息，
//   各窗口 wndproc 与绘制函数均在该线程内执行，互斥天然串行）。

// 覆盖层主窗口句柄：捕获线程独占。start() 时在捕获线程创建，消息循环内读写，
// WM_DESTROY 末尾由捕获线程置 NULL（wndproc_windows.cpp）。JS 线程不直接访问。
extern HWND g_screenshotOverlayWindow;

// 截图进行中标志（atomic）：跨线程。捕获线程 start 入口置 true、各退出路径置 false；
// JS 线程 start() 入口读它拒绝重入（已进行中则直接返回）。无锁，依赖 atomic 可见性。
extern std::atomic<bool> g_isCapturing;

// 截图结果回传 JS 的 threadsafe function：跨线程但写时序确定。JS 线程在
// StartRegionCaptureWithPrimedFrame 创建并初始化引用计数；此后由捕获线程独占
// 调用/释放（EmitScreenshotResult 唯一调用点 + ReleaseScreenshotTsfn 唯一释放点，
// 均在捕获线程内），JS 线程不再触碰。CR-002 收口后访问点唯一。
extern napi_threadsafe_function g_screenshotTsfn;

// 截图捕获线程对象：JS 线程独占。start() 创建后立即 detach，此后不再访问
// （依赖 g_isCapturing 而非 join 判定会话状态，故无需 join；线程自然退出）。
extern std::thread g_screenshotThread;

// 自动确认模式（atomic）：跨线程。JS 线程 start() 写入；捕获线程在会话开始时
// 一次性 load 进 CaptureContext.autoConfirm，会话期内不再读此全局。
extern std::atomic<bool> g_autoConfirm;

// 预截首帧（及保护其读写的互斥锁）：跨线程，受 g_primedScreenshotFrameMutex 保护。
// JS 线程（PrimeScreenshotFrameNow/PrimeScreenshotFrame）与捕获线程（会话开始消费）
// 均在锁内读写 bitmap 与 valid 等字段；bitmap 句柄所有权在锁内转移（写方创建、
// 读方消费后置 invalid）。锁外不得解引用 bitmap。
extern PrimedScreenshotFrame g_primedScreenshotFrame;
extern std::mutex g_primedScreenshotFrameMutex;

// 截图上下文指针：捕获线程独占。会话开始置 &ctx（栈上局部变量）、结束置 nullptr；
// 窗口过程/绘制函数读它取 GDI+ 会话级资源（仅捕获线程内执行）。会话期外恒为 nullptr，
// 各读点均带 nullptr 守卫。JS 线程不访问。
extern CaptureContext* g_captureCtx;

// 长截图上下文指针（atomic）：跨线程，受 g_longCtxMutex 保护指针生命周期。
extern std::atomic<LongCaptureContext*> g_longCtx;
// 保护 g_longCtx 指针的生命周期：JS 线程 LongCaptureAbort 的 load/abortFlag 写，
// 与捕获线程 WM_LONGCAPTURE_RUN 清理段的 store(nullptr)/delete 互斥（定义见
// session_windows.cpp）。锁内只做指针读写与原子标志写，禁止 SendMessage 等
// 可能死锁的调用；abortFlag 本身保持 atomic，由锁保证写入时对象仍存活。
extern std::mutex g_longCtxMutex;

// 长截图会话窗口句柄（控制面板/工具栏/蒙版）：捕获线程独占。BeginLongCapture
// 时由捕获线程创建，wndproc 清理段（捕获线程）销毁并置 NULL。JS 线程不访问。
extern HWND g_longControlWindow;
extern HWND g_longToolbarWindow;
extern HWND g_longMaskWindow;

// 长截图参数：跨线程，但时序天然串行——JS 线程 start() 写入（先按默认重置再按 JS
// 覆盖，消除跨会话粘滞），捕获线程 BeginLongCapture 一次性读入 LongCaptureContext。
// start() 写入发生在 g_screenshotThread 创建之前，捕获线程读在创建之后，happens-before
// 由线程创建建立，无需额外同步。
extern int g_lcMaxFrames;
extern int g_lcInterval;

// ---- 跨文件函数声明 ----
bool IsCornerRadiusHandle(int h);
bool IsVectorTool(int btn);
bool IsDragTool(int btn);
bool CanShowStylePopupTool(int btn);
AnnotationType ToolToAnnotationType(int btn);
int AnnotationTypeToTool(AnnotationType t);
SCPanelMetrics CalcPanelMetrics(double dpiScale);
SCToolbarMetrics CalcToolbarMetrics(double dpiScale);
SCHandleMetrics CalcHandleMetrics(double dpiScale);
HBITMAP RenderSvgToBitmap(const char* svgText, COLORREF color, int px);
int CalcToolbarWidth(const SCToolbarMetrics& m);
SCPopupMetrics CalcPopupMetrics(double dpiScale);
// 多屏异分辨率布局基准：取包含参考矩形（绝对坐标）的显示器物理边界。
// 工具栏/子菜单的上下翻转判定必须用所在显示器自身的边界——整个虚拟屏幕包围盒
// 会被高分屏拉大，低分屏上选区已触本屏底边仍会被误判为"下方放得下"而不翻转。
bool GetMonitorBoundsForRect(const RECT& refAbs, RECT& out);
void CalcPopupPlacement(const RECT& toolbarRect, int virtualX, int virtualY,
                        int virtualW, int virtualH, const SCPopupMetrics& m, int pw, int ph, RECT& out);
void CalcPopupPosition(const RECT& toolbarRect, int virtualX, int virtualY,
                       int virtualW, int virtualH, const SCPopupMetrics& m, RECT& out);
void CalcMosaicPopupSize(const SCPopupMetrics& m, int& outW, int& outH);
int HitTestMosaicPopup(int x, int y, const RECT& popupRect, const SCPopupMetrics& m);
void DrawMosaicPopup(HDC hdc, const RECT& popupRect, int modeIdx, int sizeIdx, int radiusIdx, const SCPopupMetrics& m);
int HitTestPopup(int x, int y, const RECT& popupRect, const SCPopupMetrics& m);
void DrawPopup(HDC hdc, const RECT& popupRect, int colorIdx, int firstIdx, bool isTextTool, const SCPopupMetrics& m);
void PushAnnotationHistory(CaptureContext* ctx);
bool UndoAnnotations(CaptureContext* ctx);
bool RedoAnnotations(CaptureContext* ctx);
bool InitGdipResources(CaptureContext* ctx);
Gdiplus::Font* GetGdipFont(CaptureContext* ctx, int fontPx);
double GetDpiScaleFactor();
BOOL CALLBACK MonitorEnumProc(HMONITOR hMonitor, HDC hdcMonitor, LPRECT lprcMonitor, LPARAM dwData);
bool CreateBackBuffer(HDC& outDC, HBITMAP& outBmp, int w, int h);
bool PrimeScreenshotFrameNow();
bool AcquireScreenshotBase(HDC& outMemDC, HBITMAP& outBitmap, int& vx, int& vy, int& vw, int& vh, double& dpiScale);
COLORREF GetPixelColorFromBitmap(HDC memDC, int x, int y, int vx, int vy, double dpiScale);
void ColorrefToStrings(COLORREF color, char* hexBuf, char* rgbBuf);
std::vector<SCWindowInfo> EnumWindowsForCapture();
int FindWindowAtPoint(const std::vector<SCWindowInfo>& windows, int x, int y);
void CalcPanelPosition(int mx, int my, int vx, int vy, int vw, int vh, const SCPanelMetrics& m, int& px, int& py);
void CalcResizePanelPosition(int handle, const RECT& sel, int vx, int vy, int vw, int vh, const SCPanelMetrics& m, int& px, int& py);
void RestoreDirtyRegion(HDC backDC, HDC memDC, const RECT& dirty, double dpiScale);
RECT InflateRectBy(const RECT& r, int margin);
bool IsValidRect(const RECT& r);
RECT UnionRectSafe(const RECT& a, const RECT& b);
void DrawInfoPanel(HDC hdc, int panelX, int panelY, COLORREF color, HDC memDC, int vx, int vy, int mx, int my, double dpiScale, const SCGdiResources& gdi, const SCPanelMetrics& m, int virtualW, int virtualH);
RECT DrawSizeLabel(HDC hdc, int width, int height, int refLeft, int refTop, int refRight, int refBottom, int virtualW, int virtualH, const SCGdiResources& gdi, const SCPanelMetrics& m);
RECT DrawSelection(HDC hdc, int x1, int y1, int x2, int y2, int vx, int vy, int vw, int vh, const SCGdiResources& gdi, const SCPanelMetrics& m);
void DrawWindowHighlight(HDC hdc, const RECT& rect, int vx, int vy, const SCGdiResources& gdi);
void DrawDimMask(HDC backDC, const SCGdiResources& gdi, int selLeft, int selTop, int selRight, int selBottom, int virtualW, int virtualH, int radius);
RECT NormalizeRect(const RECT& r);
bool PointInRect(int x, int y, const RECT& r);
int HitTestCornerRadiusHandle(int x, int y, const RECT& sel, int handleSize, int inset, int radius);
int FindNearestCornerRadiusHandle(int x, int y, const RECT& sel, int handleSize, int inset, int radius, int proximityMargin);
RECT CornerHandleDirtyRect(const CaptureContext* ctx, int corner);
int HitTestHandle(int x, int y, const RECT& sel, int handleSize);
LPCWSTR HandleCursor(int handle);
void CalcToolbarPosition(const RECT& selRel, int virtualX, int virtualY,
                         int virtualW, int virtualH, const SCToolbarMetrics& m, RECT& out);
int HitTestToolbar(int x, int y, const RECT& toolbarRect, const SCToolbarMetrics& m);
void DrawResizeHandles(HDC hdc, const RECT& selRel, int handleSize);
void DrawCornerRadiusHandle(HDC hdc, const RECT& selRel, int handleSize, int inset, int radius, int corner);
void DrawConfirmedBorder(HDC hdc, const RECT& selRel, const SCGdiResources& gdi, int radius);
void AddRoundedRect(Gdiplus::GraphicsPath& outPath, int x, int y, int w, int h, int radius);
void DrawToolbar(HDC hdc, const RECT& toolbarRect, int hoverBtn, int activeTool, const SCGdiResources& gdi, const SCToolbarMetrics& m, const SCIconCache& icons);
// 工具栏 title 式 tooltip：Tick 由会话空闲循环轮询（维护停顿/显示状态并失效气泡矩形），
// Draw 在 OnPaint 工具栏之后调用（气泡画进 backDC，盖在工具栏/子菜单之上）。
void TickToolbarTooltip(CaptureContext* ctx, HWND overlayWnd);
void DrawToolbarTooltip(HDC hdc, CaptureContext* ctx);
bool MosaicBlitRect(HDC targetDC, HDC srcDC, int dstX0, int dstY0, int dstW, int dstH, int srcAbsX0, int srcAbsY0, int blockPx, int virtualX, int virtualY, double dpiScale);
void FreeMosaicBase(CaptureContext* ctx);
void InitMosaicBrushCursors(CaptureContext* ctx);
void FreeMosaicBrushCursors(CaptureContext* ctx);
bool RebuildMosaicBase(CaptureContext* ctx);
bool MosaicBaseNeedsRebuild(const CaptureContext* ctx);
bool HasMosaicToRender(const std::vector<Annotation>& annotations, const Annotation* curDrawing);
void RevealMosaicToTarget(HDC targetDC, HDC mosaicBase, const std::vector<Annotation>& annotations, const Annotation* curDrawing, const RECT& contentBounds, float ox, float oy);
void DrawAnnotations(HDC hdc, const RECT& selRel, int virtualX, int virtualY, const std::vector<Annotation>& annotations, const Annotation* curDrawing);
void CompositeAnnotations(HDC finalDC, HDC srcDC, const std::vector<Annotation>& annotations, const RECT& rect, int virtualX, int virtualY, double dpiScale, int mosaicBlockPx);
std::string BitmapToBase64Png(HBITMAP hBitmap);
bool SaveBitmapToClipboard(HBITMAP hBitmap);
// 将 HBITMAP 编码为 PNG：可选产出 base64 / 原始字节，可选直接写入文件（单次编码）。
bool EncodeHBitmapPng(HBITMAP hBitmap, std::string* base64Out, std::string* rawOut,
                      const wchar_t* filePath);
// 将 HALFTONE 缩放模式设置到目标 DC（output_windows.cpp / long_capture_windows.cpp 共用）。
// HALFTONE 在做下采样缩放时比默认 COLORONCOLOR 质量更好，但 BrushOrg 会被 StretchBlt 用到，
// 因此同时把画刷原点复位到 (0,0) 避免抖动（MSDN 推荐配套调用）。
void SetHalftoneStretchMode(HDC dc);

ScreenshotResult* ExtractRegionResult(HDC memDC, const RECT& rect, int vx, int vy,
    double dpiScale, const std::vector<Annotation>& anns, int radius, int mosaicSizeIdx);
// 统一的 ScreenshotResult 发射口（CR-017）：字段参数化构造结果并经截图会话 TSFN 回传 JS。
// 内部统一处理守卫：TSFN 未就绪或 napi_tsfn_nonblocking 因队列满返回非 napi_ok 时，
// 自行 delete 分配的 result 防泄漏（CallScreenshotJs 只在成功入队时才 delete）。
// 仅允许在截图线程内调用。各发射点（确认/取消/保存/ESC/长截图完成等）统一走此函数。
// success=false 时坐标/尺寸/base64 全置 0/空（与取消路径语义一致）。
void EmitScreenshotResult(bool success, int x = 0, int y = 0, int x2 = 0, int y2 = 0,
                          int width = 0, int height = 0, const std::string& base64 = "");
// 会话初始化失败快速回传：构造 {success:false} 结果并经 EmitScreenshotResult 回传 JS，
// 唤醒 await 方避免永久挂起（CR-002 早退路径统一收口点）。
void FailFast();
void BeginLongCapture(CaptureContext* ctx, HWND overlayHwnd);
bool RunLongCapture(LongCaptureContext* c);
// 单帧「识别→offset 校验→（Weak 档）延迟确认→提交」管线（RunLongCapture 采样流程与单元测试共用入口）：
// Normal 档识别 SUCCESS 且 offset 通过历史合理性校验即提交；Weak 档（低重叠大跳变）首次可信候选
// 只登记 pendingMatch（WeakPending），下一次稳定采样独立复现一致候选才提交（Stitched）。
// 任何拒绝（Failed/WeakPending/WeakRejected/Unstable）都不修改累计拼接状态。
// allowStabilityGate：进入正式 DetectMatch 前启用轻量帧稳定性闸门（采样主循环传入
// true；默认 false 保持单元测试/合成帧直连注入的旧行为完全不变）。闸门未过时返回
// Unstable——本帧不匹配、不提交、不改任何状态，调用方短延迟后重新采样。
LCSampleOutcome LongCaptureTryStitch(LongCaptureContext* c, std::vector<uint32_t>& curr, int dir,
                                     bool allowStabilityGate = false);
void LongCaptureAbort();
std::wstring PromptSaveFilePath(HWND hwndOwner);
bool SaveRegionToPngFile(HDC memDC, const RECT& rect, int vx, int vy, double dpiScale,
    const std::vector<Annotation>& anns, const std::wstring& filePath, int radius, int mosaicSizeIdx);
void ClampCornerRadius(CaptureContext* ctx);
bool CalcAnnotationsBounds(std::vector<Annotation>& anns, RECT& out, HDC hdc);
RECT MeasureTextAnnotation(HDC hdc, Annotation& a);
int HitTestTextAnnotations(std::vector<Annotation>& anns, int x, int y, HDC hdc);
RECT MeasureAnnotationBounds(Annotation& a, HDC hdc);
int HitTestAnnotation(std::vector<Annotation>& anns, int x, int y, HDC hdc);
int HitTestAnnotationResizeHandle(const Annotation& a, int x, int y, HDC hdc, int handleSize);
void TransformAnnotationByBox(Annotation& a, const RECT& oldBox, const RECT& newBox);
void MeasureTextGdip(HDC hdc, const std::wstring& text, int fontPx, float& outOffsetX, float& outOffsetY, float& outW, float& outH);
int CalcCaretPosFromMouse(HDC hdc, const std::wstring& text, int fontPx, int textX, int mouseX);
void InvalidateAnnotationOp(HWND hwnd, CaptureContext* ctx, const RECT& curBox);
RECT CalcSelectionDirty(CaptureContext* ctx, bool includeToolbar);
void InvalidateTextLine(HWND hwnd, CaptureContext* ctx);

// ---- 选区状态机辅助（实现在 wndproc_windows.cpp，消息处理共用）----
void EnterConfirmed(CaptureContext* ctx, const RECT& sel);
RECT ResizeSelectionFromHandle(const RECT& startSelection, int handle, int dx, int dy,
                               const RECT& virtualBounds,
                               bool hasContent, const RECT& contentBounds,
                               bool enforceMinSize);
void GetResizeHandleAnchor(int handle, const RECT& sel, int& ax, int& ay);
void ApplyResizeSelection(HWND hwnd, CaptureContext* ctx);
bool HandleSelectionNudgeKey(HWND hwnd, CaptureContext* ctx, WPARAM vk);

// ---- 覆盖层消息处理（实现在 overlay_paint/overlay_input，由 WndProc 分发）----
LRESULT OnPaint(HWND hwnd, CaptureContext* ctx);
LRESULT OnLButtonDown(HWND hwnd, CaptureContext* ctx);
LRESULT OnMouseMove(HWND hwnd, CaptureContext* ctx);
LRESULT OnLButtonUp(HWND hwnd, CaptureContext* ctx);
LRESULT OnKeyDown(HWND hwnd, WPARAM wParam, CaptureContext* ctx);
LRESULT OnImeComposition(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, CaptureContext* ctx);
LRESULT OnChar(HWND hwnd, WPARAM wParam, CaptureContext* ctx);
LRESULT OnSetCursor(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, CaptureContext* ctx);

LRESULT CALLBACK ScreenshotOverlayWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
void ScreenshotCaptureThread();
