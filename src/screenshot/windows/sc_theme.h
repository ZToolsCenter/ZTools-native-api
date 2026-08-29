// 截图模块主题色常量（Windows）：多个绘制文件共用的固定色值与 COLORREF →
// Gdiplus::Color 的不透明展开。由 internal.h 二次拆分而来（纯移动不改逻辑）；
// 可独立包含，亦经 internal.h 伞头获得。
#pragma once

#include <windows.h>

// GDI+ 需要 min/max（与 internal.h 伞头的同款注入；重复 using 声明合法）
namespace Gdiplus {
    using std::min;
    using std::max;
}
#include <gdiplus.h>

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
