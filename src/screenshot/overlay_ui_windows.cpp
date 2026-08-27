// 截图模块：覆盖层 UI 布局与绘制（metrics、弹窗、工具栏、信息面板、手柄、遮罩）
#include "internal.h"

// AlphaBlend 需要 msimg32
#pragma comment(lib, "msimg32.lib")

// 是否为圆角（内倒角）手柄：四个角共用同一套拖拽逻辑（改 selectionCornerRadius）

bool IsCornerRadiusHandle(int h) {
    return h == RH_CornerRadiusTL || h == RH_CornerRadiusTR ||
           h == RH_CornerRadiusBL || h == RH_CornerRadiusBR;
}

SCPanelMetrics CalcPanelMetrics(double dpiScale) {
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

SCToolbarMetrics CalcToolbarMetrics(double dpiScale) {
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

// 按当前 DPI 计算手柄几何：选区/标注 resize 手柄与圆角手柄尺寸同步缩放。

SCHandleMetrics CalcHandleMetrics(double dpiScale) {
    auto scale = [&](int v) { return (int)(v * dpiScale + 0.5); };
    SCHandleMetrics m;
    m.handleSize = scale(SC_HANDLE_SIZE);
    m.cornerKnobInset = scale(SC_CORNER_KNOB_INSET);
    m.handleMargin = m.handleSize / 2 + 4;
    m.cornerProximity = scale(SC_CORNER_PROXIMITY);
    return m;
}

// ==================== 粗细/颜色子菜单 ====================

// 按 DPI 计算子菜单几何（单行布局）。
// 布局：[粗细圆点×3] sepGap | 分隔线 | sepGap [颜色圆点×8]
// 单元格尺寸与工具栏按钮一致（= SC_TOOLBAR_BTN），单元格间距 = SC_TOOLBAR_GAP，视觉对齐。
// 粗细圆点本身大小随预设值变化，颜色圆点固定直径，均居中在单元格内。

SCPopupMetrics CalcPopupMetrics(double dpiScale) {
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

// 子菜单单元格间距：把工具栏按钮间距 SC_TOOLBAR_GAP 按单元格尺寸等比折算后四舍五入。
// 布局计算 / 命中测试 / 绘制三条路径必须共用同一公式，否则命中框与可见单元格错位。

static int PopupCellGap(const SCPopupMetrics& m) {
    return (int)(SC_TOOLBAR_GAP * (m.cell / (double)SC_POPUP_CELL) + 0.5);
}

// COLORREF 主题色 -> 不透明 Gdiplus::Color 的封装 ScOpaqueColor 见 internal.h 主题色常量组。

// 子菜单选中态单元格背景：单元格大小的圆角矩形填充 bg + 带透明灰描边（alpha 90）。
// 马赛克子菜单与普通粗细/颜色子菜单两处绘制共用同一外观（原先各持一份 lambda）。

static void DrawPopupCellBg(Gdiplus::Graphics& graphics, const SCPopupMetrics& m,
                            int contentTop, int cellLeft, const Gdiplus::Color& bg) {
    Gdiplus::GraphicsPath p;
    AddRoundedRect(p, cellLeft, contentTop, m.cell, m.cell, m.cell / 4);
    Gdiplus::SolidBrush bgBrush(bg);
    graphics.FillPath(&bgBrush, &p);
    // 背景边缘描边（带透明灰，alpha 90）
    Gdiplus::Pen edgePen(Gdiplus::Color(90, 160, 160, 160), 1.0f);
    graphics.DrawPath(&edgePen, &p);
}

// 子菜单总宽/高（单行）。单元格间距沿用工具栏按钮间距 SC_TOOLBAR_GAP。

static void CalcPopupSize(const SCPopupMetrics& m, int& outW, int& outH) {
    int cellGap = PopupCellGap(m);
    // 粗细组宽
    int thickW = SC_THICK_COUNT * m.cell + (SC_THICK_COUNT - 1) * cellGap;
    // 颜色组宽
    int colorW = SC_COLOR_COUNT * m.cell + (SC_COLOR_COUNT - 1) * cellGap;
    int contentW = thickW + m.sepGap * 2 + 1 + colorW;  // 1 = 分隔线宽度
    outW = contentW + m.pad * 2 + m.border * 2;
    outH = m.cell + m.pad * 2 + m.border * 2;
}

// toolbarRect / out 均为相对虚拟屏幕坐标；pw/ph 为子菜单尺寸。

// 计算子菜单位置（通用）：贴工具栏下方，放不下则上方，左右钳制到所在显示器内。
// 翻转/钳制边界取「工具栏所在显示器」而非整虚拟屏幕（多屏异分辨率，见
// GetMonitorBoundsForRect 注释），否则低分屏上子菜单会翻出本屏。

void CalcPopupPlacement(const RECT& toolbarRect,
                               int virtualX, int virtualY,
                               int virtualW, int virtualH,
                               const SCPopupMetrics& m, int pw, int ph, RECT& out) {
    // 边界基准：工具栏所在显示器（失败回退整虚拟屏幕）
    RECT tbAbs = { toolbarRect.left + virtualX, toolbarRect.top + virtualY,
                   toolbarRect.right + virtualX, toolbarRect.bottom + virtualY };
    RECT mon = {};
    int bLeft = 0, bTop = 0, bRight = virtualW, bBottom = virtualH;
    if (GetMonitorBoundsForRect(tbAbs, mon)) {
        bLeft = mon.left - virtualX;     bTop = mon.top - virtualY;
        bRight = mon.right - virtualX;   bBottom = mon.bottom - virtualY;
    }
    // 水平：与工具栏左对齐
    int x = toolbarRect.left;
    // 垂直：优先工具栏下方
    int y = toolbarRect.bottom + m.margin;
    if (y + ph > bBottom) {
        y = toolbarRect.top - m.margin - ph;
    }
    // 左右钳制
    if (x + pw > bRight) x = bRight - pw - m.margin;
    if (x < bLeft) x = bLeft + m.margin;

    out.left = x;
    out.top = y;
    out.right = x + pw;
    out.bottom = y + ph;
}

// toolbarRect / out 均为相对虚拟屏幕坐标（与工具栏一致）。

// 计算子菜单位置：贴工具栏下方，放不下则上方，左右钳制到所在显示器内。

void CalcPopupPosition(const RECT& toolbarRect,
                              int virtualX, int virtualY,
                              int virtualW, int virtualH,
                              const SCPopupMetrics& m, RECT& out) {
    int pw, ph;
    CalcPopupSize(m, pw, ph);
    CalcPopupPlacement(toolbarRect, virtualX, virtualY, virtualW, virtualH, m, pw, ph, out);
}

// 马赛克子菜单几何常量
// 模式切换组：2 个单元格（涂抹 / 框选）；块大小组：3 个单元格。

static const int SC_MOSAIC_MODE_COUNT = 2;   // 涂抹、框选

// 涂抹模式用半径圆点表示，块大小用马赛克方块网格表示（绘制时按预设值缩放）

// 计算马赛克子菜单总宽/高（单行）。
// 布局：[模式×2] sepGap | 分隔线 | sepGap [块大小×3] sepGap | 分隔线 | sepGap [涂抹半径×3]

void CalcMosaicPopupSize(const SCPopupMetrics& m, int& outW, int& outH) {
    int cellGap = PopupCellGap(m);
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

int HitTestMosaicPopup(int x, int y, const RECT& popupRect, const SCPopupMetrics& m) {
    if (!PointInRect(x, y, popupRect)) return 0;
    int contentLeft = popupRect.left + m.border + m.pad;
    int contentTop = popupRect.top + m.border + m.pad;
    int cellGap = PopupCellGap(m);
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

void DrawMosaicPopup(HDC hdc, const RECT& popupRect,
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
        int cellGap = PopupCellGap(m);

        auto cellColor = [&](bool sel) -> Gdiplus::Color {
            return sel ? ScOpaqueColor(SC_THEME_TOOLBAR_BLUE)
                       : ScOpaqueColor(SC_THEME_ICON_DARK);
        };

        // 模式组
        for (int i = 0; i < SC_MOSAIC_MODE_COUNT; i++) {
            int cellLeft = contentLeft + i * (m.cell + cellGap);
            bool sel = (i == modeIdx);
            if (sel) DrawPopupCellBg(graphics, m, contentTop, cellLeft,
                                     ScOpaqueColor(SC_THEME_SEL_BG));
            int cx = cellLeft + m.cell / 2;
            Gdiplus::Color c = cellColor(sel);
            if (i == 0) {
                // 涂抹模式：画一个画笔/毛刷图标（一条波浪线 + 圆头）
                Gdiplus::Pen pen(c, (Gdiplus::REAL)2.0f);
                pen.SetLineJoin(Gdiplus::LineJoinRound);
                pen.SetStartCap(Gdiplus::LineCapRound);
                pen.SetEndCap(Gdiplus::LineCapRound);
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
            if (sel) DrawPopupCellBg(graphics, m, contentTop, cellLeft,
                                     ScOpaqueColor(SC_THEME_SEL_BG));
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
            if (sel) DrawPopupCellBg(graphics, m, contentTop, cellLeft,
                                     ScOpaqueColor(SC_THEME_SEL_BG));
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

int HitTestPopup(int x, int y, const RECT& popupRect, const SCPopupMetrics& m) {
    if (!PointInRect(x, y, popupRect)) return 0;

    // 内容左边界（绝对）
    int contentLeft = popupRect.left + m.border + m.pad;
    int contentTop = popupRect.top + m.border + m.pad;
    int cellGap = PopupCellGap(m);
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

void DrawPopup(HDC hdc, const RECT& popupRect,
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
        int cellGap = PopupCellGap(m);

        // 选中态背景：单元格大小的圆角矩形（公共绘制见 DrawPopupCellBg）。
        // 粗细沿用主工具栏选中高亮底（不透明浅蓝，SC_THEME_SEL_BG）；
        // 颜色用带透明（alpha 80）的选中色。

        // 第一组：文字工具显示字号（不同大小 'A'），矢量工具显示粗细（圆点直径区分）
        int firstCount = isTextTool ? SC_FONT_COUNT : SC_THICK_COUNT;
        int firstX0 = contentLeft;
        for (int i = 0; i < firstCount; i++) {
            int cellLeft = firstX0 + i * (m.cell + cellGap);
            if (i == firstIdx) {
                // 与主工具栏选中态一致的不透明浅蓝底
                DrawPopupCellBg(graphics, m, contentTop, cellLeft,
                                ScOpaqueColor(SC_THEME_SEL_BG));
            }
            Gdiplus::Color iconC = (i == firstIdx)
                ? ScOpaqueColor(SC_THEME_TOOLBAR_BLUE)
                : ScOpaqueColor(SC_THEME_ICON_DARK);
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
                DrawPopupCellBg(graphics, m, contentTop, cellLeft,
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

// 计算浮窗位置（优先右下，超出则翻转）

void CalcPanelPosition(int mx, int my, int vx, int vy, int vw, int vh,
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

// 调整选区时放大镜面板位置：放在被拖手柄的"外侧"，避免遮挡正在调整的选区。
// 左/右手柄置选区左/右外侧，顶/底置上下外侧，四角置对角外侧；边手柄在垂直方向居中。
// 贴屏边放不下则翻到对侧，仍放不下则贴屏边（覆盖选区可接受，属边界情况）。

void CalcResizePanelPosition(int handle, const RECT& sel,
    int vx, int vy, int vw, int vh, const SCPanelMetrics& m, int& px, int& py) {
    bool movesL = handle == RH_Left || handle == RH_TopLeft || handle == RH_BottomLeft;
    bool movesR = handle == RH_Right || handle == RH_TopRight || handle == RH_BottomRight;
    bool movesT = handle == RH_Top || handle == RH_TopLeft || handle == RH_TopRight;
    bool movesB = handle == RH_Bottom || handle == RH_BottomLeft || handle == RH_BottomRight;
    if (movesL)      px = sel.left - m.w - m.margin;
    else if (movesR) px = sel.right + m.margin;
    else             px = (sel.left + sel.right) / 2 - m.w / 2;
    if (movesT)      py = sel.top - m.h - m.margin;
    else if (movesB) py = sel.bottom + m.margin;
    else             py = (sel.top + sel.bottom) / 2 - m.h / 2;
    // 外侧放不下 -> 翻到对侧；对侧仍放不下 -> 贴屏边
    if (px < vx)            px = sel.right + m.margin;
    if (px + m.w > vx + vw) px = sel.left - m.w - m.margin;
    if (px < vx)            px = vx + m.margin;
    if (px + m.w > vx + vw) px = vx + vw - m.w - m.margin;
    if (py < vy)            py = sel.bottom + m.margin;
    if (py + m.h > vy + vh) py = sel.top - m.h - m.margin;
    if (py < vy)            py = vy + m.margin;
    if (py + m.h > vy + vh) py = vy + vh - m.h - m.margin;
}

// 从预截屏位图恢复脏区域到后台缓冲

void RestoreDirtyRegion(HDC backDC, HDC memDC, const RECT& dirty, double dpiScale) {
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

RECT InflateRectBy(const RECT& r, int margin) {
    return { r.left - margin, r.top - margin, r.right + margin, r.bottom + margin };
}

// 矩形是否有效（宽高 > 0）

bool IsValidRect(const RECT& r) {
    return r.right > r.left && r.bottom > r.top;
}

// 两矩形并集的外包矩形（安全处理零矩形：若一方无效则返回另一方）。
// 用于计算"旧位置 ∪ 新位置"的脏区域外包。

RECT UnionRectSafe(const RECT& a, const RECT& b) {
    bool va = IsValidRect(a), vb = IsValidRect(b);
    if (!va && !vb) return {0, 0, 0, 0};
    if (!va) return b;
    if (!vb) return a;
    return { (std::min)(a.left, b.left), (std::min)(a.top, b.top),
             (std::max)(a.right, b.right), (std::max)(a.bottom, b.bottom) };
}

// ---- 绘制函数 ----

// 绘制放大镜 + 鼠标信息面板
// memDC 内是整个虚拟屏幕的物理尺寸位图，采样窗口以鼠标（mx,my）为中心；
// 四边均把源区钳回位图内：左/上钳起点、右/下收窄采样宽高，
// 钳制后采样宽高非正时跳过放大镜绘制，避免向 StretchBlt 传非法参数。
// virtualW/virtualH 为虚拟屏逻辑尺寸，配合 dpiScale 还原位图物理总宽高。

void DrawInfoPanel(HDC hdc, int panelX, int panelY, COLORREF color,
    HDC memDC, int vx, int vy, int mx, int my, double dpiScale,
    const SCGdiResources& gdi, const SCPanelMetrics& m,
    int virtualW, int virtualH) {
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

    // 源区钳制：左/上越界把起点钳回 0；右/下越界按位图物理总宽高收窄采样宽高，
    // 保证 [src, src+size) 完全落在 memDC 位图内（贴边时 StretchBlt 越界采样属未定义渲染）。
    int srcXPoS = (std::max)(srcXPhysical, 0);
    int srcYPoS = (std::max)(srcYPhysical, 0);
    int totalWPhysical = (int)(virtualW * dpiScale + 0.5);
    int totalHPhysical = (int)(virtualH * dpiScale + 0.5);
    int sampleWPhysical = (std::min)(srcWPhysical, totalWPhysical - srcXPoS);
    int sampleHPhysical = (std::min)(srcHPhysical, totalHPhysical - srcYPoS);

    // 采样宽高非正（如鼠标坐标异常越过物理位图右/下边界）则放弃放大镜绘制
    if (sampleWPhysical > 0 && sampleHPhysical > 0) {
        StretchBlt(hdc, magX, magY, magW, magH, memDC,
            srcXPoS, srcYPoS, sampleWPhysical, sampleHPhysical, SRCCOPY);
    }

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

RECT DrawSizeLabel(HDC hdc, int width, int height,
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

RECT DrawSelection(HDC hdc, int x1, int y1, int x2, int y2,
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

void DrawWindowHighlight(HDC hdc, const RECT& rect, int vx, int vy, const SCGdiResources& gdi) {
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
// radius>0 时额外给选区四角的"角帽"（方框内、圆角弧外）叠同色遮罩，
// 使圆角内不残留清晰直角三角；角帽用 GDI+ 抗锯齿路径填充，与 AA 圆角边框一致。

void DrawDimMask(HDC backDC, const SCGdiResources& gdi,
    int selLeft, int selTop, int selRight, int selBottom,
    int virtualW, int virtualH, int radius) {
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

    // 圆角角帽：选区方框内、圆角弧外的四角区域，叠加同色遮罩。
    // 用 GDI+ GraphicsPath 在每角构造"外两条直边 + 内凹四分之一弧"的封闭图形，
    // FillPath 填充得到角帽，弧边抗锯齿。遮罩色与上面 AlphaBlend 一致（黑+SC_MASK_ALPHA）。
    if (radius > 0 && selRight > selLeft && selBottom > selTop) {
        int x = selLeft, y = selTop;
        int w = selRight - selLeft;
        int h = selBottom - selTop;
        int r = (std::min)(radius, (std::min)(w, h) / 2);
        if (r >= 1) {
            Gdiplus::Graphics graphics(backDC);
            graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            Gdiplus::SolidBrush dimBrush(Gdiplus::Color(SC_MASK_ALPHA, 0, 0, 0));
            Gdiplus::GraphicsPath capPath;
            // 左上角：上边 → 内凹弧 → 左边
            capPath.AddLine(x, y, x + r, y);
            capPath.AddArc(x, y, r * 2, r * 2, 270.0f, -90.0f);
            capPath.AddLine(x, y + r, x, y);
            capPath.CloseFigure();
            // 右上角：上边 → 右边 → 内凹弧
            capPath.AddLine(x + w - r, y, x + w, y);
            capPath.AddLine(x + w, y, x + w, y + r);
            capPath.AddArc(x + w - r * 2, y, r * 2, r * 2, 0.0f, -90.0f);
            capPath.CloseFigure();
            // 右下角：右边 → 下边 → 内凹弧
            capPath.AddLine(x + w, y + h - r, x + w, y + h);
            capPath.AddLine(x + w, y + h, x + w - r, y + h);
            capPath.AddArc(x + w - r * 2, y + h - r * 2, r * 2, r * 2, 90.0f, -90.0f);
            capPath.CloseFigure();
            // 左下角：左边 → 下边 → 内凹弧
            capPath.AddLine(x, y + h - r, x, y + h);
            capPath.AddLine(x, y + h, x + r, y + h);
            capPath.AddArc(x, y + h - r * 2, r * 2, r * 2, 90.0f, 90.0f);
            capPath.CloseFigure();
            graphics.FillPath(&dimBrush, &capPath);
        }
    }
}

// 规范化矩形（保证 left<right, top<bottom）

// ---- 确认态辅助函数 ----

RECT NormalizeRect(const RECT& r) {
    RECT n;
    n.left = (std::min)(r.left, r.right);
    n.right = (std::max)(r.left, r.right);
    n.top = (std::min)(r.top, r.bottom);
    n.bottom = (std::max)(r.top, r.bottom);
    return n;
}

// 点是否在矩形内

bool PointInRect(int x, int y, const RECT& r) {
    return x >= r.left && x < r.right && y >= r.top && y < r.bottom;
}

// 圆角手柄中心位置：选区四个角内侧，沿各自对角线内移 d = clamp(inset + radius, 0, maxR)。
// radius 增大时四个手柄同步沿对角线向中心滑动（轨迹始终在该角的对角线上）；
// 半径与手柄位置一一对应，松手后 radius 保持，手柄即停在原地（不回弹到静止位）。
// inset 为 DPI 缩放后的静止内缩（radius=0 时手柄位置，避开角 resize 手柄）；
// corner 取 RH_CornerRadiusTL/TR/BL/BR。sel 可为绝对坐标（命中测试用）或选区相对坐标（绘制用）。

static void CornerRadiusHandleCenter(const RECT& sel, int inset, int radius, int corner, int& cx, int& cy) {
    int w = sel.right - sel.left;
    int h = sel.bottom - sel.top;
    int maxR = (std::min)(w, h) / 2;
    if (maxR < 0) maxR = 0;
    int d = inset + radius;
    if (d > maxR) d = maxR;   // 不越过中心 / 不出选区
    if (d < 0) d = 0;
    switch (corner) {
        case RH_CornerRadiusTL: cx = sel.left + d;  cy = sel.top + d;    break;
        case RH_CornerRadiusTR: cx = sel.right - d; cy = sel.top + d;    break;
        case RH_CornerRadiusBL: cx = sel.left + d;  cy = sel.bottom - d; break;
        case RH_CornerRadiusBR: cx = sel.right - d; cy = sel.bottom - d; break;
        default:                cx = sel.left + d;  cy = sel.top + d;    break;
    }
}

// 命中测试圆角手柄（绝对坐标）。依次测试四个角，返回命中的角手柄或 RH_None。
// 命中框大小沿用 handleSize，与 resize 手柄一致；手柄位置随 radius 移动，故命中也按 radius 计算。

int HitTestCornerRadiusHandle(int x, int y, const RECT& sel, int handleSize, int inset, int radius) {
    int corners[] = { RH_CornerRadiusTL, RH_CornerRadiusTR, RH_CornerRadiusBL, RH_CornerRadiusBR };
    for (int c : corners) {
        int cx, cy;
        CornerRadiusHandleCenter(sel, inset, radius, c, cx, cy);
        RECT box = { cx - handleSize, cy - handleSize, cx + handleSize, cy + handleSize };
        if (PointInRect(x, y, box)) return c;
    }
    return RH_None;
}

// 找出鼠标"靠近"的倒角手柄（绝对坐标）：感应区 = 命中框外再扩 proximityMargin。
// 感应区大于命中区，使手柄在鼠标靠近（尚未进入命中框）时即显现；仅返回最近的那一个角，
// 选区四角通常互不相邻，足够大时鼠标只会靠近其一。

int FindNearestCornerRadiusHandle(int x, int y, const RECT& sel,
                                         int handleSize, int inset, int radius,
                                         int proximityMargin) {
    int corners[] = { RH_CornerRadiusTL, RH_CornerRadiusTR, RH_CornerRadiusBL, RH_CornerRadiusBR };
    int sense = handleSize + proximityMargin;  // 感应半宽 = 命中半宽 + 靠近余量
    int best = RH_None;
    int bestDist = 0x7FFFFFFF;  // 切比雪夫距离，越小越近
    for (int c : corners) {
        int cx, cy;
        CornerRadiusHandleCenter(sel, inset, radius, c, cx, cy);
        int dx = x - cx; if (dx < 0) dx = -dx;
        int dy = y - cy; if (dy < 0) dy = -dy;
        if (dx <= sense && dy <= sense) {
            int dist = (dx > dy) ? dx : dy;
            if (dist < bestDist) { bestDist = dist; best = c; }
        }
    }
    return best;
}

// 倒角手柄附近脏区（backDC 相对坐标）：以手柄中心为基点，扩 handleMargin 覆盖半径+描边/抗锯齿。
// 用于鼠标靠近/离开手柄时局部重绘，避免全屏刷新。

RECT CornerHandleDirtyRect(const CaptureContext* ctx, int corner) {
    RECT r = {0, 0, 0, 0};
    if (!IsCornerRadiusHandle(corner)) return r;
    int cx, cy;
    CornerRadiusHandleCenter(ctx->selection, ctx->handleMetrics.cornerKnobInset,
                             ctx->selectionCornerRadius, corner, cx, cy);
    int m = ctx->handleMetrics.handleMargin;
    r.left = cx - m - ctx->virtualX;
    r.top = cy - m - ctx->virtualY;
    r.right = cx + m + 1 - ctx->virtualX;
    r.bottom = cy + m + 1 - ctx->virtualY;
    return r;
}

// 命中测试调整手柄，返回 ResizeHandle（绝对坐标）。handleSize 为 DPI 缩放后的命中半宽。

int HitTestHandle(int x, int y, const RECT& sel, int handleSize) {
    int hs = handleSize;
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

LPCWSTR HandleCursor(int handle) {
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
        // 圆角手柄：对角线方向拖拽（向内增大半径），按所在角选对应对角光标。
        case RH_CornerRadiusTL:
        case RH_CornerRadiusBR:
            return (LPCWSTR)IDC_SIZENWSE;
        case RH_CornerRadiusTR:
        case RH_CornerRadiusBL:
            return (LPCWSTR)IDC_SIZENESW;
        default:
            return (LPCWSTR)IDC_ARROW;
    }
}

// 取包含参考矩形的显示器物理边界（MonitorFromRect 最近匹配兜底，绝对坐标）。
// 多屏异分辨率下工具栏/子菜单的上下翻转判定必须以此为界：整个虚拟屏幕包围盒会被
// 高分屏拉大，低分屏上选区已触本屏底边仍会被误判为"下方放得下"，导致工具栏该
// 上置时不上置。枚举/获取失败返回 false（调用方回退整虚拟屏幕，单屏时两者等价）。

bool GetMonitorBoundsForRect(const RECT& refAbs, RECT& out) {
    HMONITOR hmon = MonitorFromRect(&refAbs, MONITOR_DEFAULTTONEAREST);
    if (!hmon) return false;
    MONITORINFO mi = {};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfo(hmon, &mi)) return false;
    out = mi.rcMonitor;
    return true;
}

// 计算工具栏位置：优先选区下方，放不下则上方（上置），上下都放不下则贴近底部。
// 垂直/水平边界取「选区所在显示器」而非整个虚拟屏幕（多屏异分辨率见
// GetMonitorBoundsForRect 注释）。selRel/out 均为相对虚拟屏幕坐标。

void CalcToolbarPosition(const RECT& selRel, int virtualX, int virtualY,
                                int virtualW, int virtualH,
                                const SCToolbarMetrics& m, RECT& out) {
    int tw = CalcToolbarWidth(m);
    int th = m.h;
    int margin = m.margin;

    // 选区绝对坐标 -> 所在显示器边界（转回相对坐标参与计算）；失败回退整虚拟屏幕
    RECT selAbs = { selRel.left + virtualX, selRel.top + virtualY,
                    selRel.right + virtualX, selRel.bottom + virtualY };
    RECT mon = {};
    int bLeft = 0, bTop = 0, bRight = virtualW, bBottom = virtualH;
    if (GetMonitorBoundsForRect(selAbs, mon)) {
        bLeft = mon.left - virtualX;     bTop = mon.top - virtualY;
        bRight = mon.right - virtualX;   bBottom = mon.bottom - virtualY;
    }

    // 默认水平居中于选区，下方
    int x = selRel.left + ((selRel.right - selRel.left) - tw) / 2;
    int y = selRel.bottom + margin;

    // 下方放不下 -> 上方（选区触到所在显示器底边即触发）
    if (y + th > bBottom) {
        y = selRel.top - margin - th;
    }
    // 上方也放不下（选区纵向占满屏幕），贴近底部（选区内底边）
    if (y < bTop) {
        y = selRel.bottom - margin - th;
        if (y < selRel.top) y = selRel.top + margin;
        // 极端兜底：钳回显示器范围内（选区+工具栏高过屏幕等病态情形）
        if (y + th > bBottom) y = bBottom - th;
        if (y < bTop) y = bTop;
    }
    // 水平边界约束（同显示器内）
    if (x + tw > bRight) x = bRight - tw - margin;
    if (x < bLeft) x = bLeft + margin;

    out.left = x;
    out.top = y;
    out.right = x + tw;
    out.bottom = y + th;
}

// 命中测试工具栏按钮（相对虚拟屏幕坐标），返回值约定：
//   -1          = 未命中
//   SC_TB_GRIP  = 第 0 格「6 点拖拽把手」（按住可拖动工具栏）
//   >=0         = ToolButton 序号（按钮自第 1 格起排布，与 DrawToolbar 同式）

int HitTestToolbar(int x, int y, const RECT& toolbarRect, const SCToolbarMetrics& m) {
    if (!PointInRect(x, y, toolbarRect)) return -1;
    int idx = (x - toolbarRect.left - m.border - m.pad) / (m.btn + m.gap);
    if (idx < 0 || idx > TB_Count) return -1;
    if (idx == 0) return SC_TB_GRIP;   // 第 0 格 = 拖拽把手
    return idx - 1;                    // 其后依次为各工具按钮
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

// 绘制选区调整手柄（8 个），传入相对坐标矩形。handleSize 为 DPI 缩放后的边长。

void DrawResizeHandles(HDC hdc, const RECT& selRel, int handleSize) {
    int hs = handleSize;
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
    Gdiplus::SolidBrush fillBrush(ScOpaqueColor(SC_THEME_ACCENT_BLUE));
    Gdiplus::Pen borderPen(Gdiplus::Color(255, 255, 255, 255), 1.0f);
    Gdiplus::GraphicsPath path;
    for (auto& p : pts) {
        // hs×hs 匹配原 GDI Rectangle(l,t,r,b) 的 [l,r-1]×[t,b-1] 像素范围（2*half=hs）。
        path.AddRectangle(Gdiplus::Rect(p[0] - half, p[1] - half, hs, hs));
    }
    graphics.FillPath(&fillBrush, &path);
    graphics.DrawPath(&borderPen, &path);
}

// 绘制单个倒角拖拽手柄（选区四角内侧其一）：白底圆 + 蓝环 + 朝向选区角的四分之一弧点缀。
// 圆形与方形 resize 手柄区分；GDI+ 抗锯齿。selRel 为选区相对 backDC 坐标。
// corner 指定画哪一个角（RH_CornerRadiusTL/TR/BL/BR），仅绘制该角手柄（默认隐藏，靠近/拖拽时调用方传入）。
// 沿对角线内移 d = clamp(inset+radius, 0, maxR)：radius 增大时沿对角线向中心滑动，轨迹恒在对角线上；
// 松手后 radius 保持，手柄停在原地（不回弹到静止位 inset）。
// handleSize/inset 为 DPI 缩放后值；radius = ctx->selectionCornerRadius。

void DrawCornerRadiusHandle(HDC hdc, const RECT& selRel, int handleSize, int inset, int radius, int corner) {
    if (!IsCornerRadiusHandle(corner)) return;
    int half = handleSize / 2;
    int gr = (std::max)(1, half - 2);
    int cx, cy;
    CornerRadiusHandleCenter(selRel, inset, radius, corner, cx, cy);
    // 弧起始角（GDI+ 顺时针，0°=右/3 点）：落在朝向选区角的象限。
    //   TL: 180°→270°（左上象限）  TR: 270°→0°（右上象限）
    //   BR:   0°→90°（右下象限）  BL:  90°→180°（左下象限）
    float start = 0.0f;
    switch (corner) {
        case RH_CornerRadiusTL: start = 180.0f; break;
        case RH_CornerRadiusTR: start = 270.0f; break;
        case RH_CornerRadiusBR: start =   0.0f; break;
        case RH_CornerRadiusBL: start =  90.0f; break;
    }
    Gdiplus::Graphics graphics(hdc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::SolidBrush fillBrush(Gdiplus::Color(255, 255, 255, 255));
    Gdiplus::Pen ringPen(ScOpaqueColor(SC_THEME_ACCENT_BLUE), 1.5f);
    Gdiplus::Pen glyphPen(ScOpaqueColor(SC_THEME_ACCENT_BLUE), 1.0f);
    Gdiplus::GraphicsPath path;
    path.AddEllipse(cx - half, cy - half, half * 2, half * 2);
    graphics.FillPath(&fillBrush, &path);
    graphics.DrawPath(&ringPen, &path);
    graphics.DrawArc(&glyphPen, cx - gr, cy - gr, gr * 2, gr * 2, start, 90.0f);
}

// 绘制确认态选区边框（细蓝框）。radius<1 为直角（沿用 GDI Rectangle）；
// radius≥1 改用 GDI+ 圆角路径描边，抗锯齿且与 selectionPen（蓝 1px）一致。

void DrawConfirmedBorder(HDC hdc, const RECT& selRel, const SCGdiResources& gdi, int radius) {
    if (radius < 1) {
        HGDIOBJ oldPen = SelectObject(hdc, gdi.selectionPen);
        HGDIOBJ oldBrush = SelectObject(hdc, GetStockObject(NULL_BRUSH));
        Rectangle(hdc, selRel.left, selRel.top, selRel.right, selRel.bottom);
        SelectObject(hdc, oldPen);
        SelectObject(hdc, oldBrush);
        return;
    }
    Gdiplus::Graphics graphics(hdc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::Pen pen(ScOpaqueColor(SC_THEME_ACCENT_BLUE), 1.0f);
    Gdiplus::GraphicsPath path;
    AddRoundedRect(path, selRel.left, selRel.top,
        selRel.right - selRel.left, selRel.bottom - selRel.top, radius);
    graphics.DrawPath(&pen, &path);
}

// 用 GDI+ 圆角矩形路径填充 outPath（抗锯齿绘制的基础）。x,y,w,h 为整数像素矩形，
// radius 为圆角半径（自动钳制为不超过短边一半，避免重叠畸变）。
// GDI+ 的 GraphicsPath 不可拷贝，故用 out 参数而非返回值。

void AddRoundedRect(Gdiplus::GraphicsPath& outPath, int x, int y, int w, int h, int radius) {
    int r = (std::min)(radius, (std::min)(w, h) / 2);
    if (r < 1) r = 1;
    Gdiplus::Rect rect(x, y, w, h);
    outPath.AddArc(rect.X, rect.Y, r * 2, r * 2, 180, 90);
    outPath.AddArc(rect.GetRight() - r * 2, rect.Y, r * 2, r * 2, 270, 90);
    outPath.AddArc(rect.GetRight() - r * 2, rect.GetBottom() - r * 2, r * 2, r * 2, 0, 90);
    outPath.AddArc(rect.X, rect.GetBottom() - r * 2, r * 2, r * 2, 90, 90);
    outPath.CloseFigure();
}

// 绘制工具栏最左「6 点拖拽把手」：2 列 × 3 排共 6 个小圆点，居中于第 0 格单元格。
// hover（或拖拽中）时铺与按钮一致的浅蓝圆角底提示可拖；可拖拽光标由 OnSetCursor 切换。
// cell 为第 0 格的按钮区矩形（含上下留白定位，由调用方算好）。

static void DrawToolbarGrip(HDC hdc, const RECT& cellRect, int hoverBtn,
                            const SCToolbarMetrics& m) {
    Gdiplus::Graphics graphics(hdc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);

    bool hot = (hoverBtn == SC_TB_GRIP);
    if (hot) {
        Gdiplus::GraphicsPath hlPath;
        int hlRadius = m.btn / 8;
        int hlInset = 2;
        int hlSize = m.btn - hlInset * 2;
        AddRoundedRect(hlPath, cellRect.left + hlInset, cellRect.top + hlInset,
                       hlSize, hlSize, hlRadius);
        Gdiplus::SolidBrush hlBrush(ScOpaqueColor(SC_THEME_HOVER_BG));
        graphics.FillPath(&hlBrush, &hlPath);
    }

    // 圆点几何随按钮格尺寸（DPI）缩放：列距 ±11%、行距 0/±16%、点半径 ~5%
    float cx = (cellRect.left + cellRect.right) * 0.5f;
    float cy = (cellRect.top + cellRect.bottom) * 0.5f;
    float colGap = m.btn * 0.11f;
    float rowGap = m.btn * 0.16f;
    float r = (std::max)(1.2f, m.btn * 0.05f);
    Gdiplus::SolidBrush dotBrush(Gdiplus::Color(255, 165, 165, 165));
    for (int row = -1; row <= 1; row++) {
        for (int col = -1; col <= 1; col += 2) {
            float dx = cx + col * colGap;
            float dy = cy + row * rowGap;
            graphics.FillEllipse(&dotBrush, dx - r, dy - r, r * 2, r * 2);
        }
    }
}

// 几何与图标尺寸均按 metrics（DPI 缩放）计算；图标从 icons 缓存取。

// 绘制悬浮工具栏（白底圆角 + 最左拖拽把手 + 按钮图标 + 分组分隔线）

void DrawToolbar(HDC hdc, const RECT& toolbarRect,
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

            // 第 0 格为拖拽把手，各按钮自第 1 格起排布（与 HitTestToolbar 映射一致）
            int bx = toolbarRect.left + m.border + m.pad + (i + 1) * (m.btn + m.gap);
            int by = toolbarRect.top + btnPad;
            // 选中态：主题蓝 #3B8BF2 叠白底 ~15% 的预混合浅蓝（225,237,253）
            // hover 态：极浅蓝（235,243,255）
            Gdiplus::Color hlColor = active
                ? ScOpaqueColor(SC_THEME_SEL_BG)
                : ScOpaqueColor(SC_THEME_HOVER_BG);
            Gdiplus::SolidBrush hlBrush(hlColor);
            Gdiplus::GraphicsPath hlPath;
            AddRoundedRect(hlPath, bx + hlInset, by + hlInset, hlSize, hlSize, hlRadius);
            graphics.FillPath(&hlBrush, &hlPath);
        }
    }

    // 最左「6 点拖拽把手」：hover 铺浅蓝底 + 灰色圆点（GDI+ 抗锯齿）
    RECT gripCell = {
        toolbarRect.left + m.border + m.pad,
        toolbarRect.top + btnPad,
        toolbarRect.left + m.border + m.pad + m.btn,
        toolbarRect.top + btnPad + m.btn
    };
    DrawToolbarGrip(hdc, gripCell, hoverBtn, m);

    // ---- 第二遍：GDI 绘制分隔线 + 图标（位图直接 AlphaBlend，无需 AA）----
    for (int i = 0; i < TB_Count; i++) {
        int bx = toolbarRect.left + m.border + m.pad + (i + 1) * (m.btn + m.gap);
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

// ==================== 工具栏 title 式 tooltip ====================
// 网页 title 属性同款交互：图标悬停 ~0.5s 停顿后出现深色圆角小气泡，移开/点击立即消失。
// 覆盖层窗口不接收稳定鼠标流（无焦点、依赖轮询），停顿判定由会话空闲循环每拍调用
// TickToolbarTooltip 维护；气泡画进 backDC（OnPaint 末尾经 DrawToolbarTooltip），
// 显示/隐藏各自失效气泡矩形，与局部刷新管线兼容。视觉与长截图工具栏 tooltip 一致。

static const int SC_TIP_DELAY_MS = 500;   // 悬停多久后显示（网页 title 同款节奏）

static const int SC_TIP_GAP = 6;          // 气泡与锚点按钮的间距

static const int SC_TIP_PAD_X = 8;        // 气泡水平内边距

static const int SC_TIP_PAD_Y = 5;        // 气泡垂直内边距

static const int SC_TIP_RADIUS = 4;       // 气泡圆角半径

// 各按钮的 tooltip 文案（分隔线返回 nullptr）

static const wchar_t* ToolbarButtonTip(int btn) {
    switch (btn) {
    case SC_TB_GRIP:     return L"拖动工具栏";
    case TB_Drag:        return L"拖拽";
    case TB_Rect:        return L"矩形";
    case TB_Circle:      return L"圆形";
    case TB_Arrow:       return L"箭头";
    case TB_Brush:       return L"画笔";
    case TB_Mosaic:      return L"马赛克";
    case TB_Text:        return L"文字";
    case TB_Translate:   return L"翻译";
    case TB_LongCapture: return L"长截图";
    case TB_Undo:        return L"撤销";
    case TB_Redo:        return L"重做";
    case TB_Save:        return L"保存到本地";
    case TB_Cancel:      return L"取消";
    case TB_Confirm:     return L"确定";
    default:             return nullptr;
    }
}

// 按文本测量气泡尺寸（屏幕 DC + 同款字体；GDI DrawText 测量即可，绘制走 GDI+）

static SIZE MeasureTipBubbleSize(const wchar_t* text, double ds) {
    SIZE sz = {0, 0};
    HDC screen = GetDC(NULL);
    if (!screen) return sz;
    auto sc = [ds](int v) { return (int)(v * ds + 0.5); };
    HFONT fnt = CreateFontW(-(int)(12 * ds + 0.5), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                            DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, SC_FONT_FACE);
    HGDIOBJ oldF = SelectObject(screen, fnt);
    RECT tr = {0, 0, 0, 0};
    DrawTextW(screen, text, -1, &tr, DT_CALCRECT | DT_SINGLELINE);
    SelectObject(screen, oldF);
    DeleteObject(fnt);
    ReleaseDC(NULL, screen);
    sz.cx = (tr.right - tr.left) + sc(SC_TIP_PAD_X) * 2;
    sz.cy = (tr.bottom - tr.top) + sc(SC_TIP_PAD_Y) * 2;
    return sz;
}

// 收起气泡：失效旧矩形（含抗锯齿边缘余量）。tipBtn/tipDwellSince 由调用方维护。

static void HideToolbarTooltip(CaptureContext* ctx, HWND overlayWnd) {
    if (!ctx->tipShown) return;
    ctx->tipShown = false;
    RECT r = InflateRectBy(ctx->tipBubbleRect, 2);
    InvalidateRect(overlayWnd, &r, FALSE);
}

// tooltip 停顿轮询（会话空闲循环每拍调用）：光标所在工具栏按钮即目标；
// 目标变化（含离开/按下左键视为无目标）立即收起并重新计时；同一目标停顿满
// SC_TIP_DELAY_MS 且尚未显示时计算气泡矩形并失效触发绘制。

void TickToolbarTooltip(CaptureContext* ctx, HWND overlayWnd) {
    if (!ctx || !overlayWnd) return;
    // 仅在工具栏可见且 hover 有意义的确认态/文字编辑态显示；按下左键期间视为无目标
    bool toolbarVisible = (ctx->state == CS_Confirmed || ctx->state == CS_TextEditing);
    int btn = -1;
    if (toolbarVisible && !(GetAsyncKeyState(VK_LBUTTON) & 0x8000)) {
        POINT pt;
        if (GetCursorPos(&pt)) {
            btn = HitTestToolbar(pt.x - ctx->virtualX, pt.y - ctx->virtualY,
                                 ctx->toolbarRect, ctx->toolbarMetrics);
        }
    }
    DWORD now = GetTickCount();
    if (btn != ctx->tipBtn) {
        HideToolbarTooltip(ctx, overlayWnd);   // 目标切换/离开：收起并重新停顿
        ctx->tipBtn = btn;
        ctx->tipDwellSince = now;
    } else if (btn >= 0 && !ctx->tipShown
               && now - ctx->tipDwellSince >= (DWORD)SC_TIP_DELAY_MS) {
        const wchar_t* text = ToolbarButtonTip(btn);
        if (text && *text) {
            double ds = ctx->dpiScale;
            auto sc = [ds](int v) { return (int)(v * ds + 0.5); };
            const SCToolbarMetrics& m = ctx->toolbarMetrics;
            // 锚点 = 目标所在单元格矩形（把手占第 0 格、按钮自第 1 格起，与
            // HitTestToolbar/DrawToolbar 映射一致），backDC 相对坐标
            int cell = (btn == SC_TB_GRIP) ? 0 : btn + 1;
            RECT anchor;
            anchor.left = ctx->toolbarRect.left + m.border + m.pad + cell * (m.btn + m.gap);
            anchor.top = ctx->toolbarRect.top + (m.h - m.btn) / 2;
            anchor.right = anchor.left + m.btn;
            anchor.bottom = anchor.top + m.btn;
            SIZE sz = MeasureTipBubbleSize(text, ds);
            int gap = sc(SC_TIP_GAP);
            int x = (anchor.left + anchor.right) / 2 - sz.cx / 2;
            if (x < sc(4)) x = sc(4);
            if (x + sz.cx > ctx->virtualW - sc(4)) x = ctx->virtualW - sc(4) - sz.cx;
            // 优先上方，放不下转下方（工具栏在选区上方时）
            int y = anchor.top - gap - sz.cy;
            if (y < sc(4)) y = anchor.bottom + gap;
            ctx->tipText = text;
            ctx->tipBubbleRect = {x, y, x + sz.cx, y + sz.cy};
            ctx->tipShown = true;
            InvalidateRect(overlayWnd, &ctx->tipBubbleRect, FALSE);
        }
    }
}

// 绘制当前在屏的气泡（深色圆角底 + 白色居中文本，GDI+ 抗锯齿；
// OnPaint 在工具栏/子菜单之后调用，保证气泡位于最上层）

void DrawToolbarTooltip(HDC hdc, CaptureContext* ctx) {
    if (!ctx || !ctx->tipShown || ctx->tipText.empty()) return;
    double ds = ctx->dpiScale;
    auto sc = [ds](int v) { return (int)(v * ds + 0.5); };
    const RECT& rc = ctx->tipBubbleRect;
    Gdiplus::Graphics graphics(hdc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::GraphicsPath path;
    AddRoundedRect(path, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top,
                   sc(SC_TIP_RADIUS));
    Gdiplus::SolidBrush bg(Gdiplus::Color(255, 41, 41, 41));
    graphics.FillPath(&bg, &path);
    Gdiplus::FontFamily ff(SC_FONT_FACE);
    Gdiplus::Font fnt(&ff, (Gdiplus::REAL)(12 * ds + 0.5),
                      Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
    Gdiplus::StringFormat sf;
    sf.SetAlignment(Gdiplus::StringAlignmentCenter);
    sf.SetLineAlignment(Gdiplus::StringAlignmentCenter);
    Gdiplus::RectF layout((Gdiplus::REAL)rc.left, (Gdiplus::REAL)rc.top,
                          (Gdiplus::REAL)(rc.right - rc.left),
                          (Gdiplus::REAL)(rc.bottom - rc.top));
    Gdiplus::SolidBrush white(Gdiplus::Color(255, 255, 255, 255));
    graphics.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);
    graphics.DrawString(ctx->tipText.c_str(), -1, &fnt, layout, &sf, &white);
}
