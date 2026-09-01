// 截图模块标注类型与绘制参数预设（Windows）：统一标注结构（矩形/圆/箭头/画笔/文字/
// 马赛克）、粗细/字号/马赛克/颜色预设及其档数约束、撤销栈深度上限。
// 由 internal.h 二次拆分而来（纯移动不改逻辑）；可独立包含，亦经 internal.h 伞头获得。
#pragma once

#include <windows.h>
#include <string>
#include <vector>

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
// 撤销栈最大深度（快照份数）。撤销历史是整份标注的深拷贝，不限深会随操作数平方级累积内存
// （每笔操作全量复制一次），超出后由 PushAnnotationHistory 裁掉最老快照。

static const int SC_UNDO_MAX_DEPTH = 50;
