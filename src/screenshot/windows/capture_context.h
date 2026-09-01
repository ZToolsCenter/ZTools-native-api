// 截图模块会话上下文（Windows）：CaptureContext 完整定义——区域截图编辑态的全部
// 状态机字段（选区/工具栏/标注/文字输入/马赛克/GDI+ 会话资源）。
// 由 internal.h 二次拆分而来（纯移动不改逻辑）；依赖同目录 sc_types.h / sc_annotations.h，
// 可独立包含，亦经 internal.h 伞头获得。
#pragma once

#include <windows.h>

// GDI+ 需要 min/max（与 internal.h 伞头的同款注入；重复 using 声明合法）
namespace Gdiplus {
    using std::min;
    using std::max;
}
#include <gdiplus.h>

#include "sc_types.h"
#include "sc_annotations.h"

#include <deque>
#include <string>
#include <vector>

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
