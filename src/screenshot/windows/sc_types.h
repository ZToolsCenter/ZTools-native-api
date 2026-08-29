// 截图模块基础类型与常量（Windows）：会话状态枚举、工具按钮、预截首帧缓冲、
// 工具栏/子菜单/面板/手柄几何（常量 + DPI 缩放结构）、GDI 资源缓存、窗口信息、
// 截图结果结构、显示器枚举数据等跨文件共享类型。
// 由 internal.h 二次拆分而来（纯移动不改逻辑）；可独立包含，亦经 internal.h 伞头获得。
#pragma once

#include <windows.h>
#include <chrono>
#include <string>

static const auto SC_PRIMED_FRAME_TTL = std::chrono::seconds(2);

struct PrimedScreenshotFrame {
    HBITMAP bitmap = NULL;
    int vx = 0;
    int vy = 0;
    int vw = 0;
    int vh = 0;
    double dpiScale = 1.0;   // 单一 scale 模型的已知限制见 CaptureContext.dpiScale 注释
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

    // 创建/释放方法体下沉到 session_windows.cpp（唯一使用方在会话层，
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
// 显示器枚举回调数据

struct MonitorEnumData {
    LONG minLeft, minTop, maxRight, maxBottom;
    double totalDpiScale;
    int monitorCount;
    HMODULE shcore;  // 外层一次 LoadLibraryW("shcore.dll") 的句柄，回调内复用 GetDpiForMonitor（避免每显示器重复 Load）
};
