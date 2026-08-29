// 长截图子系统：选区底部工具栏 UI（工具栏/popover/tooltip/图标缓存/自动滚动/裁剪菜单）。
// 拆分自 long_capture_windows.cpp 的「长截图工具栏」段。
// 工具栏是独立 TOPMOST 分层弹窗（WS_EX_LAYERED），消息由 RunLongCapture 泵循环分发，
// 全部视觉内容由 LongCaptureToolbarRender 整幅渲染后 UpdateLayeredWindow 原子提交。
#include "internal.h"
#include "long_capture_internal.h"
#include "../../generated/icon_svgs.h"   // 工具栏图标 SVG 文本（构建期由 scripts/gen-icons.js 生成）

// ==================== 长截图工具栏（选区底部悬浮窗口） ====================
// 小地图面板只保留预览；全部操作集中到本工具栏，从左到右（图标按钮）：
//   [预览宽×高] | [方向▾] [自动滚动] [裁剪▾] | [保存到本地] [取消] [完成并复制]
// 按钮样式与基础截图工具栏一致（白底圆角条 + 浅蓝 hover 高亮 + 图标三态）。
// 方向与裁剪带二级菜单（悬停锚点按钮片刻即展开、移开自动收起，点击亦可开合；
// 方向：纵向/横向，默认纵向，已拼接多帧后锁定；裁剪：丢弃选区上方/下方内容、重置），
// 其余为直接动作。工具栏是独立 TOPMOST 弹窗（位于灰蒙版之上），消息由 RunLongCapture
// 的泵循环分发，因此所有交互与采样主循环同线程执行，无需跨线程同步（finish/abort/save
// 仍走原子量）。二级菜单展开方向永远避让选区（见 LongCaptureMenuOpenBelow），
// 菜单浮层绝不进入选区画面——否则会被逐帧采样采进拼接内容、污染重叠识别基准。


// 布局常量（逻辑像素，绘制/命中测试时按 dpiScale 缩放）

static const int LC_BAR_H = 44;        // 工具栏高度（图标按钮 32 + 上下内边距 6）

static const int LC_BAR_BTN_W = 32;    // 图标按钮宽度（正方形 cell）

static const int LC_BAR_BTN_H = 32;    // 图标按钮高度

static const int LC_BAR_GAP = 2;       // 相邻图标按钮间距

static const int LC_SIZE_W = 104;      // 预览宽×高标签占位宽

static const int LC_BAR_PAD = 6;       // 左右内边距

static const int LC_BAR_SEP_W = 13;    // 分隔线占位宽（含两侧间距）

static const int LC_BAR_RADIUS = 8;    // 圆角半径

static const int LC_BAR_MARGIN = 8;    // 选区到工具栏间距

static const int LC_MENU_GAP = 6;      // 子菜单与工具栏间距

static const int LC_POP_H = 44;        // 裁剪 popover 面板高（图标 cell 32 + 上下内边距 6）

static const int LC_POP_PAD = 6;       // 裁剪 popover 面板内边距

static const int LC_POP_CELL_W = 32;   // 裁剪 popover 图标 cell 宽（与工具栏按钮同尺寸）

static const int LC_POP_CELL_GAP = 2;  // 裁剪 popover 图标 cell 间距

static const int LC_POP_OPEN_DWELL_MS = 300;   // 方向/裁剪锚点按钮悬停多久后展开 popover（悬停意图判定，扫过不误触）

static const int LC_POP_CLOSE_GRACE_MS = 250;  // 鼠标离开「锚点按钮∪popover」多久后收起（跨间隙宽限）

static const int LC_TIP_DELAY_MS = 500;        // 图标悬停多久后显示 title 式 tooltip（网页 title 同款节奏）

static const int LC_TIMER_AUTOSCROLL = 1;  // 自动滚动定时器 id

static const int LC_TIMER_UI = 2;          // UI 维护定时器 id（子菜单外点关闭等）

// 连续平滑滚动的注入参数：高频小步长取代旧「低频大跳步」（每 600~1200ms 注入一次
// 两整档增量），页面不再一顿一顿地跳，而是持续匀速滑动。平均速度约 2.7 档/s，略低于
// 旧的 3.3 档/s——用稍慢的速度换取连续性；想再慢/快只需同比例调整这两个常量。

static const int LC_AUTOSCROLL_TICK_MS = 31;    // 注入节拍 ms（落在系统定时器 ~15.6ms 粒度的两格上）

static const int LC_AUTOSCROLL_STEP_DELTA = 10; // 单次注入滚轮增量（亚档位：<WHEEL_DELTA=120，
                                                // 目标应用按像素比例累积，等效逐帧微位移）

const int LC_AUTOSCROLL_STOP_FAILS = 3;     // 自动滚动连续硬失败采样轮数上限（自动停止防丢内容）

// 工具栏项目（顺序即布局顺序；分隔线不可点击）

enum LongToolbarItem {
    LTI_Grip = 0,     // 拖拽把手（6 点图标：hover 四向箭头光标，按住拖动工具栏窗口）
    LTI_Size,         // 预览宽×高标签（纯展示）
    LTI_Sep1,         // 分隔线
    LTI_Direction,    // 方向图标按钮（悬停/点击展开二级菜单：纵向/横向，默认纵向；已拼接多帧后锁定）
    LTI_AutoScroll,   // 自动滚动开关（开启态高亮；图标随方向切换为纵向/横向变体）
    LTI_Crop,         // 裁剪图标按钮（悬停/点击展开图标 popover：丢弃当前选区位置以上/以下内容，可选重置）
    LTI_Sep3,         // 分隔线
    LTI_Save,         // 保存到本地（保存对话框 + 落盘，不进剪贴板）
    LTI_Cancel,       // 取消
    LTI_Finish,       // 完成并复制（拼接结果复制到剪贴板并回调）
    LTI_Count
};

// 工具栏窗口客户区布局（DPI 缩放）：底条 + 可选的上方/下方二级菜单（方向/裁剪共用）

struct LongToolbarLayout {
    RECT bar;                 // 工具栏底条
    RECT items[LTI_Count];    // 各项目（分隔线为其占位矩形）
    RECT menu;                // 二级菜单面板（方向=整宽文字行面板；裁剪=紧凑图标 popover，展开时有效）
    int menuRows;             // 二级菜单项数（方向=行数；裁剪=图标 cell 数）
    bool menuBelow;           // 二级菜单在底条下方（展开方向避让选区，见 LongCaptureMenuOpenBelow）
};


// 二级菜单项数（= popover 图标 cell 数）：方向恒 2（纵向/横向）；裁剪 2 +（已裁剪时）重置

static int LongCaptureMenuRows(const LongCaptureContext* c) {
    if (!c) return 0;
    if (c->menuKind == LCM_Direction) return 2;
    return c->cropped ? 3 : 2;   // LCM_Crop
}

// 二级菜单的锚点按钮（popover 水平居中对齐、悬停展开与离开收起均围绕锚点判定）

static int LongCaptureMenuAnchorItem(LCMenuKind kind) {
    return kind == LCM_Crop ? LTI_Crop : LTI_Direction;
}

// 悬停的底条按钮将展开的二级菜单：裁剪恒可展开；方向在已拼接多帧（frameCount>1）后
// 锁定（两个方向的内容坐标系不同，混拼必然错位），锁定期间悬停/点击均不展开。

static LCMenuKind LongCaptureHoverMenuKind(const LongCaptureContext* c, int hv) {
    if (!c) return LCM_None;
    if (hv == LTI_Crop) return LCM_Crop;
    if (hv == LTI_Direction && c->frameCount.load() <= 1) return LCM_Direction;
    return LCM_None;
}

// 二级菜单面板高度（逻辑像素）：方向/裁剪均为单行图标 popover（cell 32 + 上下内边距 6）

static int LongCaptureMenuHeightLogi(const LongCaptureContext* c) {
    if (!c || c->menuKind == LCM_None) return 0;
    return LC_POP_H;
}

// 二级菜单项文案（popover 图标 cell 的 tooltip 文本）：方向=纵向/横向；
// 裁剪=丢弃上方(纵向)/左侧(横向)、丢弃下方/右侧、重置。

static const wchar_t* LongCaptureMenuRowLabel(const LongCaptureContext* c, int row) {
    if (!c) return L"";
    if (c->menuKind == LCM_Direction) {
        static const wchar_t* dir[2] = {L"纵向", L"横向"};
        return (row >= 0 && row < 2) ? dir[row] : L"";
    }
    static const wchar_t* cropV[3] = {L"丢弃选区上方内容", L"丢弃选区下方内容", L"重置裁剪"};
    static const wchar_t* cropH[3] = {L"丢弃选区左侧内容", L"丢弃选区右侧内容", L"重置裁剪"};
    const wchar_t* const* labels = c->horizontal ? cropH : cropV;
    int n = c->cropped ? 3 : 2;
    return (row >= 0 && row < n) ? labels[row] : L"";
}

// 二级菜单行可用性：方向菜单中「非当前方向」的行在已拼接多帧（frameCount>1）后禁用
//（两个方向的内容坐标系不同，混拼必然错位，宁可要求用户取消重来）；当前方向行保持
// 可用（点击仅收起菜单）。裁剪行始终可用（只收紧输出行窗口，不碰拼接/匹配状态）。

static bool LongCaptureMenuRowEnabled(const LongCaptureContext* c, int row) {
    if (!c) return false;
    if (c->menuKind == LCM_Direction) {
        int current = c->horizontal ? 1 : 0;
        return row == current || c->frameCount.load() <= 1;
    }
    return true;
}

// 预览宽×高标签（逻辑像素，含裁剪窗口；横向模式宽高已回转）
// 显示空间逻辑尺寸换算统一为：固定轴取 cropRect（逻辑源，无 /ds 舍入往返误差），
// 滚动轴取 rows / ds（物理行数 ÷ scale）。与 LongCapturePanelUpdate/PanelPreviewRect
// 同一公式，消除原先三种写法并存。

static void LongCaptureOutputSizeLabel(const LongCaptureContext* c, wchar_t* buf, size_t n) {
    double ds = c ? c->dpiScale : 1.0;
    int rowStart = 0, rowEnd = 0;
    LongCaptureOutputRows(c, rowStart, rowEnd);
    int rows = rowEnd - rowStart;
    if (rows <= 0 || !c) { swprintf_s(buf, n, L"0 × 0"); return; }
    // 固定轴（纵向=宽、横向=高）取 cropRect 逻辑尺寸；滚动轴取 rows/ds 逻辑尺寸。
    double cropW = (double)(c->cropRect.right - c->cropRect.left);
    double cropH = (double)(c->cropRect.bottom - c->cropRect.top);
    double w = c->horizontal ? rows / ds : cropW;
    double h = c->horizontal ? cropH : rows / ds;
    swprintf_s(buf, n, L"%d × %d", (int)(w + 0.5), (int)(h + 0.5));
}

// 工具栏客户区布局（DPI 缩放）：图标按钮从左到右排布，垂直居中于底条；
// 二级菜单展开时窗口整体向避让选区的一侧扩展（menuBelow 决定菜单在底条上/下）。
// 方向与裁剪均为紧凑图标 popover 面板，水平居中对齐各自的锚点按钮
//（items 排布完成后计算，故面板水平位置在末尾收敛）。

static LongToolbarLayout LongCaptureToolbarLayout(LongCaptureContext* c, int cw, int ch) {
    LongToolbarLayout L = {};
    double ds = c ? c->dpiScale : 1.0;
    auto sc = [ds](int v) { return (int)(v * ds + 0.5); };
    int barH = sc(LC_BAR_H), pad = sc(LC_BAR_PAD), btnH = sc(LC_BAR_BTN_H);
    bool popMenu = c && c->menuKind != LCM_None;
    int menuRows = (c && c->menuKind != LCM_None) ? LongCaptureMenuRows(c) : 0;
    int menuH = sc(LongCaptureMenuHeightLogi(c));
    int menuGap = sc(LC_MENU_GAP);
    L.menuRows = menuRows;
    L.menuBelow = c ? c->menuBelow : false;
    if (menuRows > 0 && !L.menuBelow) {
        // 菜单在上、底条在下（窗口整体向上扩展）
        L.menu = {0, 0, cw, menuH};
        L.bar = {0, menuH + menuGap, cw, ch};
    } else if (menuRows > 0) {
        // 菜单在下、底条在上（窗口整体向下扩展）
        L.bar = {0, 0, cw, barH};
        L.menu = {0, barH + menuGap, cw, barH + menuGap + menuH};
    } else {
        L.bar = {0, 0, cw, ch};
    }
    // 各项目从左到右排布：图标按钮为等宽正方形 cell，图标间留小间距；
    // 分隔线占位宽自带两侧间距，不再额外加 gap
    int x = L.bar.left + pad;
    auto place = [&](int idx, int w) {
        L.items[idx].left = x;
        L.items[idx].top = L.bar.top + (barH - btnH) / 2;
        L.items[idx].right = x + w;
        L.items[idx].bottom = L.items[idx].top + btnH;
        x += w + sc(LC_BAR_GAP);
    };
    auto placeSep = [&](int idx) {
        L.items[idx].left = x;
        L.items[idx].top = L.bar.top + sc(10);
        L.items[idx].right = x + sc(LC_BAR_SEP_W);
        L.items[idx].bottom = L.bar.bottom - sc(10);
        x += sc(LC_BAR_SEP_W);
    };
    place(LTI_Grip, sc(LC_BAR_BTN_W));   // 最左 6 点拖拽把手
    place(LTI_Size, sc(LC_SIZE_W));
    placeSep(LTI_Sep1);
    place(LTI_Direction, sc(LC_BAR_BTN_W));
    place(LTI_AutoScroll, sc(LC_BAR_BTN_W));
    place(LTI_Crop, sc(LC_BAR_BTN_W));
    placeSep(LTI_Sep3);
    place(LTI_Save, sc(LC_BAR_BTN_W));
    place(LTI_Cancel, sc(LC_BAR_BTN_W));
    place(LTI_Finish, sc(LC_BAR_BTN_W));
    // 图标 popover：面板水平居中于锚点按钮（方向→方向按钮，裁剪→裁剪按钮）并夹在
    // 工具栏宽度内（垂直位置已由上方菜单带决定）
    if (popMenu) {
        int anchor = LongCaptureMenuAnchorItem(c->menuKind);
        int pw = menuRows * sc(LC_POP_CELL_W) + (menuRows - 1) * sc(LC_POP_CELL_GAP)
               + sc(LC_POP_PAD) * 2;
        int cxBtn = (L.items[anchor].left + L.items[anchor].right) / 2;
        int px = cxBtn - pw / 2;
        if (px < 0) px = 0;
        if (px + pw > cw) px = cw - pw;
        L.menu.left = px;
        L.menu.right = px + pw;
    }
    return L;
}

// 工具栏窗口总宽（逻辑像素 × dpiScale），与 LongCaptureToolbarLayout 的横向排布严格一致

static int LongCaptureToolbarWindowWidth(LongCaptureContext* c) {
    double ds = c ? c->dpiScale : 1.0;
    auto sc = [ds](int v) { return (int)(v * ds + 0.5); };
    int cell = LC_BAR_BTN_W + LC_BAR_GAP;
    int content = LC_BAR_BTN_W + LC_BAR_GAP   // 最左 6 点拖拽把手格
                + LC_SIZE_W + LC_BAR_GAP + LC_BAR_SEP_W
                + cell * 2 + LC_BAR_BTN_W + LC_BAR_SEP_W
                + cell * 2 + LC_BAR_BTN_W;
    return sc(LC_BAR_PAD * 2 + content);
}

static int LongCaptureHitTestToolbar(int x, int y, const LongToolbarLayout& L) {
    for (int i = 0; i < LTI_Count; i++) {
        if (i == LTI_Sep1 || i == LTI_Sep3) continue;
        if (PointInRect(x, y, L.items[i])) return i;
    }
    return -1;
}

// 二级菜单 popover 图标 cell 矩形（menu 面板内从左到右等宽排布，与工具栏按钮同尺寸；
// 方向/裁剪共用同一套布局与命中机制）

static RECT LongCapturePopoverCellRect(const LongCaptureContext* c, const LongToolbarLayout& L, int i) {
    double ds = c ? c->dpiScale : 1.0;
    int pad = (int)(LC_POP_PAD * ds + 0.5);
    int cell = (int)(LC_POP_CELL_W * ds + 0.5);
    int gap = (int)(LC_POP_CELL_GAP * ds + 0.5);
    return {L.menu.left + pad + i * (cell + gap), L.menu.top + pad,
            L.menu.left + pad + i * (cell + gap) + cell, L.menu.top + pad + cell};
}

// 命中二级菜单 popover 图标 cell（-1=不在 popover 内）

static int LongCaptureHitTestPopover(LongCaptureContext* c, int x, int y, const LongToolbarLayout& L) {
    if (L.menuRows <= 0) return -1;
    for (int i = 0; i < L.menuRows; i++) {
        if (PointInRect(x, y, LongCapturePopoverCellRect(c, L, i))) return i;
    }
    return -1;
}


// ---- 长截图工具栏图标缓存（dark/blue/white/gray 四色 × LCI_Count，按会话 DPI 光栅化）----

enum LCIconId {
    LCI_DirectionV = 0,   // 纵向方向（竖版矩形+下箭头）
    LCI_DirectionH,       // 横向方向（横版矩形+右箭头）
    LCI_AutoScrollV,      // 自动滚动·纵向（鼠标+下箭头）
    LCI_AutoScrollH,      // 自动滚动·横向（鼠标+右箭头）
    LCI_Crop,             // 裁剪
    LCI_Save,             // 保存到本地（下载图标）
    LCI_Cancel,           // 取消（×）
    LCI_Confirm,          // 完成并复制（✓）
    LCI_CropDiscardTop,    // 丢弃上方（箭头抵上边界线）
    LCI_CropDiscardBottom, // 丢弃下方
    LCI_CropDiscardLeft,   // 丢弃左侧（横向模式变体）
    LCI_CropDiscardRight,  // 丢弃右侧（横向模式变体）
    LCI_CropReset,         // 重置裁剪（逆时针还原）
    LCI_Count
};

// 图标位图缓存：创建工具栏时按 DPI 光栅化一次（与主截图工具栏 SCIconCache 同思路）。
// dark=常态，blue=悬停/开启态（与文字激活色一致），gray=禁用（方向锁定）。

struct LCIconCache {
    bool inited;
    int px;               // 光栅化边长（物理像素）
    HBITMAP dark[LCI_Count];
    HBITMAP blue[LCI_Count];
    HBITMAP gray[LCI_Count];

    LCIconCache() : inited(false), px(0) {
        for (int i = 0; i < LCI_Count; i++)
            dark[i] = blue[i] = gray[i] = NULL;
    }

    // 按物理像素边长光栅化全部图标（尺寸变化时先释放旧位图；光栅化实现见 icons_windows.cpp）
    void Init(int physicalPx) {
        if (inited && px == physicalPx) return;
        Cleanup();
        static const char* svgs[LCI_Count] = {
            kIconSvg_DirectionV, kIconSvg_DirectionH,
            kIconSvg_AutoScrollV, kIconSvg_AutoScrollH,
            kIconSvg_CropIcon, kIconSvg_Save, kIconSvg_Cancel, kIconSvg_Confirm,
            kIconSvg_CropDiscardTop, kIconSvg_CropDiscardBottom,
            kIconSvg_CropDiscardLeft, kIconSvg_CropDiscardRight, kIconSvg_CropReset,
        };
        for (int i = 0; i < LCI_Count; i++) {
            if (!svgs[i]) continue;
            dark[i]  = RenderSvgToBitmap(svgs[i], RGB(60, 60, 60), physicalPx);
            blue[i]  = RenderSvgToBitmap(svgs[i], RGB(9, 105, 218), physicalPx);
            gray[i]  = RenderSvgToBitmap(svgs[i], RGB(178, 178, 178), physicalPx);
        }
        px = physicalPx;
        inited = true;
    }

    void Cleanup() {
        for (int i = 0; i < LCI_Count; i++) {
            if (dark[i])  { DeleteObject(dark[i]);  dark[i] = NULL; }
            if (blue[i])  { DeleteObject(blue[i]);  blue[i] = NULL; }
            if (gray[i])  { DeleteObject(gray[i]);  gray[i] = NULL; }
        }
        px = 0;
        inited = false;
    }
};

static LCIconCache s_lcIcons;   // 长截图工具栏图标（单会话独占使用，随工具栏窗口销毁释放）


// 把缓存图标位图居中绘制到按钮矩形（位图为预乘 ARGB，AlphaBlend per-pixel alpha）

static void LongCaptureDrawIcon(HDC hdc, const RECT& rc, HBITMAP bmp, int px) {
    if (!bmp || px <= 0) return;
    int x = (rc.left + rc.right - px) / 2;
    int y = (rc.top + rc.bottom - px) / 2;
    HDC srcDC = CreateCompatibleDC(hdc);
    if (!srcDC) return;
    HGDIOBJ old = SelectObject(srcDC, bmp);
    BLENDFUNCTION blend = {};
    blend.BlendOp = AC_SRC_OVER;
    blend.SourceConstantAlpha = 255;
    blend.AlphaFormat = AC_SRC_ALPHA;
    AlphaBlend(hdc, x, y, px, px, srcDC, 0, 0, px, px, blend);
    SelectObject(srcDC, old);
    DeleteDC(srcDC);
}

// 绘制图标按钮三态：hover/active 浅蓝圆角底 + 蓝色图标，disabled 灰图标，常态深灰图标。
// 分层渲染目标：圆角底画进 Graphics（32bpp 预乘 ARGB 表面），图标经 AlphaBlend 写入
// iconDC（GDI AlphaBlend 对预乘 ARGB 源/目标均做正确 alpha 混合，GDI+ 位图拷贝会丢 alpha）。

static void LongCaptureDrawIconButton(Gdiplus::Graphics* g, HDC iconDC, const RECT& rc, LCIconId id,
                                      bool hover, bool active, bool disabled, double ds) {
    if (!disabled && (hover || active)) {
        Gdiplus::GraphicsPath path;
        AddRoundedRect(path, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top,
                       (int)(6 * ds + 0.5));
        Gdiplus::SolidBrush brush(Gdiplus::Color(255,
            active ? 225 : 235, active ? 237 : 243, active ? 253 : 255));
        g->FillPath(&brush, &path);
    }
    HBITMAP bmp = disabled ? s_lcIcons.gray[id]
                : (hover || active) ? s_lcIcons.blue[id]
                : s_lcIcons.dark[id];
    LongCaptureDrawIcon(iconDC, rc, bmp, s_lcIcons.px);
}

// 绘制工具栏最左「6 点拖拽把手」：2 列 × 3 排共 6 个小圆点，居中于把手单元格。
// hover/拖拽中铺与按钮一致的浅蓝圆角底、圆点转主题蓝；可拖光标由 WM_SETCURSOR 切换。

static void LongCaptureDrawGrip(Gdiplus::Graphics* g, const RECT& rc, bool hot, double ds) {
    if (hot) {
        Gdiplus::GraphicsPath path;
        AddRoundedRect(path, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top,
                       (int)(6 * ds + 0.5));
        Gdiplus::SolidBrush brush(ScOpaqueColor(SC_THEME_HOVER_BG));
        g->FillPath(&brush, &path);
    }
    // 圆点几何随单元格尺寸（DPI）缩放：列距 ±11%、行距 0/±16%、点半径 ~5%
    float cw = (float)(rc.right - rc.left);
    float cx = ((float)rc.left + (float)rc.right) * 0.5f;
    float cy = ((float)rc.top + (float)rc.bottom) * 0.5f;
    float colGap = cw * 0.11f;
    float rowGap = cw * 0.16f;
    float r = (std::max)(1.2f, cw * 0.05f);
    Gdiplus::SolidBrush dot(hot ? Gdiplus::Color(255, 9, 105, 218)
                                : Gdiplus::Color(255, 165, 165, 165));
    for (int row = -1; row <= 1; row++) {
        for (int col = -1; col <= 1; col += 2) {
            float dx = cx + col * colGap;
            float dy = cy + row * rowGap;
            g->FillEllipse(&dot, dx - r, dy - r, r * 2, r * 2);
        }
    }
}

// 二级菜单 popover 第 i 个 cell 的图标：方向 = 纵向/横向变体；裁剪 = 丢弃起点/终点
//（纵向=上方/下方，横向=左侧/右侧），已裁剪时末位追加重置（与 cell 序/tooltip 一致）

static LCIconId LongCaptureMenuCellIcon(const LongCaptureContext* c, int i) {
    if (c && c->menuKind == LCM_Direction) return i == 0 ? LCI_DirectionV : LCI_DirectionH;
    if (c && c->cropped && i == LongCaptureMenuRows(c) - 1) return LCI_CropReset;
    if (c && c->horizontal) return i == 0 ? LCI_CropDiscardLeft : LCI_CropDiscardRight;
    return i == 0 ? LCI_CropDiscardTop : LCI_CropDiscardBottom;
}


// ---- 工具栏分层渲染（WS_EX_LAYERED + UpdateLayeredWindow）----
// 旧实现直接在 WM_PAINT 里把底条/菜单逐元素画到屏幕 DC：鼠标扫过按钮时整窗重绘
// 可见中间态（闪动）；窗口外形用 SetWindowRgn 圆角区域裁剪，1bit 掩码无抗锯齿
//（圆角锯齿）。改为整幅渲染进 32bpp 预乘 ARGB 后备 DIB 后一次 UpdateLayeredWindow
// 原子提交：提交瞬间完成、画面无中间帧（消闪动），圆角/边缘经 alpha 通道抗锯齿
//（消锯齿），底条与 popover 之间的条带保持全透明（透出桌面且鼠标穿透，等价于旧的
// 联合窗口区域）。灰蒙版窗口（EnterLongCaptureMask）即同款分层渲染先例。

// 工具栏后备表面（随工具栏窗口销毁释放，见 LongCaptureToolbarWndProc 的 WM_DESTROY）
static HDC s_lcTbSurfDC = NULL;
static HBITMAP s_lcTbSurfBmp = NULL;
static void* s_lcTbSurfBits = nullptr;
static int s_lcTbSurfW = 0, s_lcTbSurfH = 0;



// 分层表面上绘制单行文本（GDI 文本函数不写 alpha 通道，分层窗口必须走 GDI+）：
// 水平/垂直居中于 rc，不换行；默认超宽省略号截断（宽×高标签固定槽位的兜底），
// allowEllipsis=false 时关闭截断——tooltip 全文展示用，气泡尺寸已按同款测量预留，
// 不应出现「…」。字体微软雅黑 12px×ds，抗锯齿网格适配。
static void LongCaptureDrawSurfaceText(Gdiplus::Graphics* g, const wchar_t* text,
                                       const RECT& rc, double ds, const Gdiplus::Color& color,
                                       bool allowEllipsis = true) {
    if (!g || !text || !*text) return;
    Gdiplus::FontFamily ff(SC_FONT_FACE);
    Gdiplus::Font fnt(&ff, (Gdiplus::REAL)(12 * ds + 0.5),
                      Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
    Gdiplus::StringFormat sf;
    sf.SetAlignment(Gdiplus::StringAlignmentCenter);
    sf.SetLineAlignment(Gdiplus::StringAlignmentCenter);
    sf.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);
    sf.SetTrimming(allowEllipsis ? Gdiplus::StringTrimmingEllipsisCharacter
                                 : Gdiplus::StringTrimmingNone);
    Gdiplus::RectF layout((Gdiplus::REAL)rc.left, (Gdiplus::REAL)rc.top,
                          (Gdiplus::REAL)(rc.right - rc.left),
                          (Gdiplus::REAL)(rc.bottom - rc.top));
    Gdiplus::SolidBrush br(color);
    g->SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);
    g->DrawString(text, -1, &fnt, layout, &sf, &br);
}


// 整幅渲染工具栏（底条 + 展开中的二级菜单 popover）并以 ULW 原子提交。
// dstX/dstY/w/h 为目标窗口矩形（屏幕物理坐标）：ULW 的 pptDst/psize 与内容一次提交，
// 菜单开合改变窗口尺寸时不存在「先伸缩后绘制」的中间帧。hover/active 状态全部取自
// LongCaptureContext，任何状态变化后重调本函数即可（经 LongCaptureToolbarRepaint）。
void LongCaptureToolbarRender(LongCaptureContext* c, int dstX, int dstY, int w, int h) {
    HWND tb = g_longToolbarWindow;
    if (!c || !tb || w <= 0 || h <= 0) return;
    if (!EnsureArgbSurface(s_lcTbSurfDC, s_lcTbSurfBmp, s_lcTbSurfBits,
                           s_lcTbSurfW, s_lcTbSurfH, w, h))
        return;
    LongToolbarLayout L = LongCaptureToolbarLayout(c, w, h);
    double ds = c->dpiScale;
    int radius = (int)(LC_BAR_RADIUS * ds + 0.5);
    {
        Gdiplus::Bitmap surf(w, h, w * 4, PixelFormat32bppPARGB, (BYTE*)s_lcTbSurfBits);
        Gdiplus::Graphics g(&surf);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        // 1) 清全透明（底条 ∪ popover 之外 alpha=0：透出桌面且鼠标穿透）
        g.SetCompositingMode(Gdiplus::CompositingModeSourceCopy);
        Gdiplus::SolidBrush clear(Gdiplus::Color(0, 0, 0, 0));
        g.FillRectangle(&clear, 0, 0, w, h);
        g.SetCompositingMode(Gdiplus::CompositingModeSourceOver);
        // 2) 二级菜单 popover 面板：白色圆角底 + 浅灰描边，内为单行图标 cell
        //   （方向=纵向/横向，当前方向 active 蓝底、锁定方向灰显；裁剪=丢弃/重置）
        if (L.menuRows > 0) {
            Gdiplus::GraphicsPath path;
            AddRoundedRect(path, L.menu.left, L.menu.top,
                           L.menu.right - L.menu.left, L.menu.bottom - L.menu.top, radius);
            Gdiplus::SolidBrush white(Gdiplus::Color(255, 255, 255, 255));
            g.FillPath(&white, &path);
            Gdiplus::Pen border(Gdiplus::Color(255, 210, 210, 210), 1.0f);
            g.DrawPath(&border, &path);
            int curDir = c->horizontal ? 1 : 0;
            for (int i = 0; i < L.menuRows; i++) {
                bool enabled = LongCaptureMenuRowEnabled(c, i);
                bool active = c->menuKind == LCM_Direction && i == curDir;
                LongCaptureDrawIconButton(&g, s_lcTbSurfDC, LongCapturePopoverCellRect(c, L, i),
                                          LongCaptureMenuCellIcon(c, i),
                                          enabled && i == c->menuHover, active, !enabled, ds);
            }
        }
        // 3) 底条背景：白色圆角 + 浅灰描边
        {
            Gdiplus::GraphicsPath path;
            AddRoundedRect(path, L.bar.left, L.bar.top,
                           L.bar.right - L.bar.left, L.bar.bottom - L.bar.top, radius);
            Gdiplus::SolidBrush white(Gdiplus::Color(255, 255, 255, 255));
            g.FillPath(&white, &path);
            Gdiplus::Pen border(Gdiplus::Color(255, 210, 210, 210), 1.0f);
            g.DrawPath(&border, &path);
        }
        // 4) 分隔线（GDI+ 1px 竖线；x+0.5 偏移使整数坐标下恰好落在单像素列）
        {
            Gdiplus::Pen sepPen(Gdiplus::Color(255, 230, 230, 230), 1.0f);
            const int seps[2] = {LTI_Sep1, LTI_Sep3};
            for (int s = 0; s < 2; s++) {
                const RECT& r = L.items[seps[s]];
                if (r.right <= r.left) continue;
                Gdiplus::REAL x = (Gdiplus::REAL)((r.left + r.right) / 2) + 0.5f;
                g.DrawLine(&sepPen, x, (Gdiplus::REAL)r.top, x, (Gdiplus::REAL)r.bottom);
            }
        }
        // 5) 尺寸标签（预览宽×高，随拼接/裁剪实时变化）
        {
            wchar_t label[64];
            LongCaptureOutputSizeLabel(c, label, 64);
            LongCaptureDrawSurfaceText(&g, label, L.items[LTI_Size], ds,
                                       Gdiplus::Color(255, 130, 130, 130));
        }
        // 6) 图标按钮（hover/active/disabled 三态；方向与自动滚动图标随当前方向切换变体）
        //    最左先画「6 点拖拽把手」（拖拽中保持热态高亮）
        bool dirLocked = c->frameCount.load() > 1;   // 已拼接多帧：方向锁定（坐标系不同不可混拼）
        LongCaptureDrawGrip(&g, L.items[LTI_Grip],
                            c->tbDragging || c->tbHover == LTI_Grip, ds);
        LongCaptureDrawIconButton(&g, s_lcTbSurfDC, L.items[LTI_Direction],
                                  c->horizontal ? LCI_DirectionH : LCI_DirectionV,
                                  c->tbHover == LTI_Direction || c->menuKind == LCM_Direction,
                                  false, dirLocked, ds);
        LongCaptureDrawIconButton(&g, s_lcTbSurfDC, L.items[LTI_AutoScroll],
                                  c->horizontal ? LCI_AutoScrollH : LCI_AutoScrollV,
                                  c->tbHover == LTI_AutoScroll, c->autoScroll, false, ds);
        LongCaptureDrawIconButton(&g, s_lcTbSurfDC, L.items[LTI_Crop], LCI_Crop,
                                  c->tbHover == LTI_Crop || c->menuKind == LCM_Crop,
                                  c->cropped, false, ds);
        LongCaptureDrawIconButton(&g, s_lcTbSurfDC, L.items[LTI_Save], LCI_Save,
                                  c->tbHover == LTI_Save, false, false, ds);
        LongCaptureDrawIconButton(&g, s_lcTbSurfDC, L.items[LTI_Cancel], LCI_Cancel,
                                  c->tbHover == LTI_Cancel, false, false, ds);
        LongCaptureDrawIconButton(&g, s_lcTbSurfDC, L.items[LTI_Finish], LCI_Confirm,
                                  c->tbHover == LTI_Finish, false, false, ds);
    }
    POINT src = {0, 0};
    SIZE sz = {w, h};
    POINT dst = {dstX, dstY};
    BLENDFUNCTION bf = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UpdateLayeredWindow(tb, NULL, &dst, &sz, s_lcTbSurfDC, &src, 0, &bf, ULW_ALPHA);
}


// 按当前窗口矩形重绘工具栏（所有视觉状态变化的统一重绘入口：
// hover 跟踪、菜单开合、宽×高标签刷新、自动滚动开关等）

void LongCaptureToolbarRepaint() {
    LongCaptureContext* c = g_longCtx.load();
    HWND tb = g_longToolbarWindow;
    if (!c || !tb) return;
    RECT wr;
    GetWindowRect(tb, &wr);
    LongCaptureToolbarRender(c, wr.left, wr.top, wr.right - wr.left, wr.bottom - wr.top);
}


// 工具栏底条按钮的 title 式 tooltip 文案（返回 nullptr 表示该项无 tooltip，如宽×高标签）

static const wchar_t* LongCaptureToolbarItemTip(const LongCaptureContext* c, int item) {
    if (!c) return nullptr;
    switch (item) {
    case LTI_Grip:       return L"拖动工具栏";
    case LTI_Direction:
        return c->frameCount.load() > 1 ? L"滚动方向（已拼接多帧后锁定）" : L"滚动方向";
    case LTI_AutoScroll: return L"自动滚动";
    case LTI_Crop:       return L"裁剪";
    case LTI_Save:       return L"保存到本地";
    case LTI_Cancel:     return L"取消";
    case LTI_Finish:     return L"完成并复制";
    default:             return nullptr;
    }
}


// 临时摘除/恢复长截图窗口组的置顶（保存对话框等系统弹窗需要真正置顶）

void LongCaptureSetTopmost(bool topmost) {
    HWND targets[3] = { g_longMaskWindow, g_longControlWindow, g_longToolbarWindow };
    for (HWND h : targets) {
        if (h) SetWindowPos(h, topmost ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
}


// 自动滚动注入：以固定节拍发送小幅滚轮增量实现连续平滑滚动、完全不移动光标。
// 光标已在开启时一次性移到选区中心（LongCaptureSetAutoScroll），SendInput 按
// 光标下方窗口路由（Win10+ 默认「滚动非活动窗口」），因此选区下的目标窗口持续被
// 滚动而鼠标位置全程不变。增量小于一个 notch（WHEEL_DELTA）：支持高分辨率滚轮的
// 应用（浏览器/Electron/现代 UI 均属此类）会按比例精确累积，视觉上即为连续滚动；
// 传统应用按内部余量规整累计，也不会再出现大跳步。注入的滚轮同样会被本会话的
// Raw Input 观察捕获，采样主循环走既有的「滚动中主动采样」路径，无需停稳。
static void LongCaptureAutoScrollTick(LongCaptureContext* c) {
    if (c->reachedBottom || c->abortFlag.load() || c->finishFlag.load() || c->saveFlag.load()) {
        LongCaptureSetAutoScroll(c, false);
        return;
    }
    INPUT inp = {};
    inp.type = INPUT_MOUSE;
    if (c->horizontal) {
        inp.mi.dwFlags = MOUSEEVENTF_HWHEEL;
        inp.mi.mouseData = LC_AUTOSCROLL_STEP_DELTA;   // 向右滚 = 追加尾部
    } else {
        inp.mi.dwFlags = MOUSEEVENTF_WHEEL;
        inp.mi.mouseData = -LC_AUTOSCROLL_STEP_DELTA;  // 向下滚 = 追加尾部
    }
    SendInput(1, &inp, sizeof(INPUT));
}


void LongCaptureSetAutoScroll(LongCaptureContext* c, bool on) {
    if (!c || c->autoScroll == on) return;
    c->autoScroll = on;
    c->autoFailStreak = 0;
    if (on) {
        c->noChangeCount = 0;
        c->reachedBottom = false;
        // 开启时一次性把光标移到选区中心：后续每拍只注入滚轮、不再改动鼠标位置
        //（光标停驻选区中心 = SendInput 持续路由到选区下的目标窗口；用户仍可随时
        // 手动移开鼠标接管操作）。
        int px = c->physOriginX + c->physX + c->capW / 2;
        int py = c->physOriginY + c->physY + c->capH / 2;
        SetCursorPos(px, py);
        // 注入节拍：固定的高频小步长（连续平滑滚动）。用户 interval 只继续作为采样
        // 防抖参数使用——拼接管线按实际位移自适应，与注入节奏彻底解耦。
        SetTimer(g_longToolbarWindow, LC_TIMER_AUTOSCROLL, LC_AUTOSCROLL_TICK_MS, NULL);
    } else {
        KillTimer(g_longToolbarWindow, LC_TIMER_AUTOSCROLL);
    }
    LongCaptureToolbarRepaint();
}


// 裁剪：收紧输出行窗口并登记「待剔除区间」（以当前 committed 视口为基准的内容坐标）。
// 此刻拼接缓冲与匹配基准完全不动——被裁掉的内容仍在缓冲里，「重置裁剪」可完整恢复；
// 此后第一次朝被裁方向的成功提交会物理删除该区间并把该侧边界重新开放，继续滚动的
// 新增内容从裁剪线直接续接拼图（见 CommitStitch 入口的延迟剔除）。横向模式天然成立：
// 帧缓冲转置复用纵向管线，「丢弃上方/下方」即「丢弃左侧/右侧」。
// 反复裁剪按「只收紧不放宽」合并：同侧锚点取更紧者，且每次都按最终生效锚点重新推导
// 待剔除区间（恒等闭包 = 捕获外沿 ↔ 裁剪线），不会因连续裁剪产生重叠或遗漏。

void LongCaptureApplyCrop(LongCaptureContext* c, int row) {
    if (!c || c->stitchH <= 0) return;
    int n = c->cropped ? 3 : 2;   // 裁剪菜单行数（末行=重置，仅已裁剪时存在）
    if (row < 0 || row >= n) return;
    if (row == n - 1 && c->cropped) {
        // 重置裁剪：清空全部锚点与待剔除区间（尚未执行的剔除随之作废、内容完整恢复；
        // 已触发执行的删除不可逆）
        c->cropPendTop = false;
        c->cropPendBottom = false;
        c->cropTopY = INT64_MIN;
        c->cropBottomY = INT64_MAX;
        c->cropped = false;
    } else {
        // 裁剪线锚定在当前视口边沿（内容坐标）；反向有效边界 = 已设置的锚点或捕获
        // 外沿中的更紧者，保证窗口至少保留一行
        int64_t vpTopY = c->committedContentTop;
        int64_t vpBottomY = vpTopY + c->physH;
        int64_t capTopY = -(int64_t)c->headRows;
        int64_t capBottomY = (int64_t)c->bodyRows;
        if (row == 0) {
            // 丢弃上方（横向 = 左侧）：窗口顶收到当前视口顶（越靠下 = 收得越紧）
            int64_t botEff = (std::min)(c->cropBottomY, capBottomY);
            int64_t cut = vpTopY < botEff - 1 ? vpTopY : botEff - 1;
            if (c->cropTopY == INT64_MIN || cut > c->cropTopY) c->cropTopY = cut;
            int64_t pendHi = c->cropTopY;
            c->cropPendTop = pendHi > capTopY;
            if (c->cropPendTop) {
                c->cropPendTopLo = capTopY;
                c->cropPendTopHi = pendHi;
            }
        } else {
            // 丢弃下方（横向 = 右侧）：窗口底收到当前视口底（越靠上 = 收得越紧）
            int64_t topEff = (std::max)(c->cropTopY, capTopY);
            int64_t cut = vpBottomY > topEff + 1 ? vpBottomY : topEff + 1;
            if (c->cropBottomY == INT64_MAX || cut < c->cropBottomY) c->cropBottomY = cut;
            int64_t pendLo = c->cropBottomY;
            c->cropPendBottom = capBottomY > pendLo;
            if (c->cropPendBottom) {
                c->cropPendBottomLo = pendLo;
                c->cropPendBottomHi = capBottomY;
            }
        }
        c->cropped = true;
    }
    LongCapturePanelUpdate(c);
    if (g_longControlWindow) InvalidateRect(g_longControlWindow, NULL, FALSE);
    LongCaptureToolbarRepaint();
}


// ---- title 式 tooltip（网页 title 属性同款交互：图标悬停 ~0.5s 出现，深色圆角小气泡，
//      锚定目标按钮上方/下方；工具栏按钮与裁剪 popover 图标共用；随工具栏窗口销毁释放）----

static HWND g_lcTooltipWindow = NULL;
static std::wstring s_lcTooltipText;   // 当前显示文本（窗口过程绘制时读取）

// tooltip 气泡内边距（逻辑像素，随 DPI 缩放；Show 与 WndProc 各自按同式计算）

static void LongCaptureTipPadding(double ds, int& padX, int& padY) {
    padX = (int)(8 * ds + 0.5);
    padY = (int)(5 * ds + 0.5);
}

// tooltip 窗口过程：分层窗口（WS_EX_LAYERED），内容由 LongCaptureTooltipRender 整幅
// 经 UpdateLayeredWindow 维护，WM_PAINT 仅清验证区（深色圆角底 + 白色文本在渲染函数里画）

static LRESULT CALLBACK LongCaptureTooltipWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_SETCURSOR: { SetCursor(LoadCursor(NULL, IDC_ARROW)); return TRUE; }
    case WM_NCHITTEST: return HTCLIENT;
    case WM_ERASEBKGND: return 1;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

// tooltip 后备表面（与工具栏表面同款机制；随工具栏销毁一并释放）
static HDC s_lcTipSurfDC = NULL;
static HBITMAP s_lcTipSurfBmp = NULL;
static void* s_lcTipSurfBits = nullptr;
static int s_lcTipSurfW = 0, s_lcTipSurfH = 0;

// tooltip 分层渲染：清透明 → 深色圆角底（圆角经 alpha 抗锯齿，无区域裁剪锯齿）→
// 白色居中文本（GDI+，GDI 文本不写 alpha 通道）→ ULW 原子提交。
// 注意：ULW 必须显式携带目标几何（dstX/dstY/w/h）——实测 NULL 几何（仅靠此前
// SetWindowPos 定位）时返回成功但 DWM 不合成该窗口，屏幕上不可见；
// 工具栏同款显式几何路径则始终正常。
static void LongCaptureTooltipRender(LongCaptureContext* c, int dstX, int dstY, int w, int h) {
    if (!g_lcTooltipWindow) return;
    if (!EnsureArgbSurface(s_lcTipSurfDC, s_lcTipSurfBmp, s_lcTipSurfBits,
                           s_lcTipSurfW, s_lcTipSurfH, w, h))
        return;
    double ds = c ? c->dpiScale : 1.0;
    {
        Gdiplus::Bitmap surf(w, h, w * 4, PixelFormat32bppPARGB, (BYTE*)s_lcTipSurfBits);
        Gdiplus::Graphics g(&surf);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.SetCompositingMode(Gdiplus::CompositingModeSourceCopy);
        Gdiplus::SolidBrush clear(Gdiplus::Color(0, 0, 0, 0));
        g.FillRectangle(&clear, 0, 0, w, h);
        g.SetCompositingMode(Gdiplus::CompositingModeSourceOver);
        Gdiplus::GraphicsPath path;
        AddRoundedRect(path, 0, 0, w, h, (int)(4 * ds + 0.5));
        Gdiplus::SolidBrush bg(Gdiplus::Color(255, 41, 41, 41));
        g.FillPath(&bg, &path);
        if (!s_lcTooltipText.empty()) {
            int padX, padY;
            LongCaptureTipPadding(ds, padX, padY);
            RECT tr = {padX, padY, w - padX, h - padY};
            LongCaptureDrawSurfaceText(&g, s_lcTooltipText.c_str(), tr, ds,
                                       Gdiplus::Color(255, 255, 255, 255), false);
        }
    }
    POINT src = {0, 0};
    SIZE sz = {w, h};
    POINT dst = {dstX, dstY};
    BLENDFUNCTION bf = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UpdateLayeredWindow(g_lcTooltipWindow, NULL, &dst, &sz, s_lcTipSurfDC, &src, 0, &bf, ULW_ALPHA);
}

// tooltip 是否正在显示（UiTick 判定「目标稳定且尚未显示」时用）

static bool LongCaptureTooltipVisible() {
    return g_lcTooltipWindow && IsWindowVisible(g_lcTooltipWindow);
}

// 显示 tooltip：按文本测量定尺寸，锚定目标矩形（屏幕坐标）——优先上方，放不下转下方；
// 水平居中于锚点并夹在虚拟屏幕内。窗口懒创建，复用至工具栏销毁。

static void LongCaptureTooltipShow(LongCaptureContext* c, const wchar_t* text, const RECT& anchor) {
    if (!c || !text || !*text) return;
    double ds = c->dpiScale;
    int padX, padY;
    LongCaptureTipPadding(ds, padX, padY);
    // 文本测量：必须与绘制同为 GDI+（MeasureString + 同款 Font/StringFormat）。
    // 同字号下 GDI DrawText 的测宽小于 GDI+ DrawString 的实际渲染宽度（GDI 无
    // overhang），按 GDI 测量定的气泡曾量窄，绘制端省略号把文案截成「…」。
    HDC screen = GetDC(NULL);
    if (!screen) return;
    Gdiplus::Graphics g(screen);
    Gdiplus::FontFamily ff(SC_FONT_FACE);
    Gdiplus::Font fnt(&ff, (Gdiplus::REAL)(12 * ds + 0.5),
                      Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
    Gdiplus::StringFormat sf;
    sf.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);
    Gdiplus::RectF layout(0.0f, 0.0f, 65536.0f, 65536.0f), bound;
    g.MeasureString(text, -1, &fnt, layout, &sf, &bound);
    ReleaseDC(NULL, screen);
    int w = (int)(bound.Width + 0.5f) + padX * 2 + 2;   // +2 抗锯齿边缘余量，杜绝触发截断
    int h = (int)(bound.Height + 0.5f) + padY * 2 + 2;
    int gap = (int)(6 * ds + 0.5);
    int vx = GetSystemMetrics(SM_XVIRTUALSCREEN), vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN), vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    int x = (anchor.left + anchor.right) / 2 - w / 2;
    if (x < vx + 4) x = vx + 4;
    if (x + w > vx + vw - 4) x = vx + vw - 4 - w;
    int y = anchor.top - h - gap;
    if (y < vy + 4) y = anchor.bottom + gap;
    if (!g_lcTooltipWindow) {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = LongCaptureTooltipWndProc;
        wc.hInstance = GetModuleHandle(NULL);
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.lpszClassName = L"ZToolsLcTooltip";
        RegisterClassExW(&wc);
        g_lcTooltipWindow = CreateWindowExW(
            WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED,
            L"ZToolsLcTooltip", L"", WS_POPUP, x, y, w, h, NULL, NULL, GetModuleHandle(NULL), NULL);
        if (!g_lcTooltipWindow) return;
    }
    s_lcTooltipText = text;
    // 与工具栏同款提交流程：ULW 原子提交内容+几何（必须显式携带，见 Render 注释），
    // 隐藏状态下先提交再显示；已显示时 ULW 即时更新位置/尺寸/内容。
    LongCaptureTooltipRender(c, x, y, w, h);
    if (!IsWindowVisible(g_lcTooltipWindow))
        ShowWindow(g_lcTooltipWindow, SW_SHOWNOACTIVATE);
}

// 收起 tooltip（不销毁窗口；下次满足停顿时重新显示）

static void LongCaptureTooltipHide() {
    if (LongCaptureTooltipVisible()) ShowWindow(g_lcTooltipWindow, SW_HIDE);
}

// tooltip 停顿判定状态（UiTick 轮询维护；Cancel 供点击路径重置，点击后需重新停顿才再显示）
// 目标编码：0..LTI_Count-1 = 底条按钮；100 + menuKind*10 + cell = 二级菜单 popover 的
// 图标 cell（方向/裁剪各自独立编号，跨菜单切换后同号 cell 不会误判为同一目标）

static int s_lcTipTarget = -1;
static DWORD s_lcTipSince = 0;

// 立即收起 tooltip 并清零停顿（点击任何位置后调用，网页 title 点击即隐藏同款）

static void LongCaptureTooltipCancel() {
    LongCaptureTooltipHide();
    s_lcTipTarget = -1;
    s_lcTipSince = 0;
}


// 计算二级菜单展开方向：朝「避让选区」的一侧展开。菜单浮层绝不能盖进选区——
// 选区内容正被逐帧采样做重叠识别拼接，浮层一旦入画即污染匹配基准（重叠识别对画面
// 突变极其敏感，污染帧虽会被安全拒绝，但会拖垮采样成功率甚至触发误判）。
// 规则：底条在选区下方 → 菜单向下展开；底条在选区上方 → 向上展开；底条与选区重叠的
// 兜底形态（选区几乎全屏时）选屏幕空余较大的一侧。仅当远离侧放不下时才翻到另一侧
//（此时可能盖到选区边角：菜单是点击即散的瞬态浮层，污染帧会被匹配管线安全拒绝）。

static bool LongCaptureMenuOpenBelow(LongCaptureContext* c, const RECT& bar) {
    double ds = c->dpiScale;
    int gap = (int)(LC_MENU_GAP * ds + 0.5);
    int menuH = (int)(LongCaptureMenuHeightLogi(c) * ds + 0.5);
    bool below;
    if (bar.top >= c->selection.bottom - 2) below = true;
    else if (bar.bottom <= c->selection.top + 2) below = false;
    else below = (bar.top - c->vy) < (c->vy + c->vh - bar.bottom);
    // 屏幕空余判定用「工具栏所在显示器」边界：多屏异分辨率时整虚拟屏幕包围盒
    // 被高分屏拉大，低分屏上会误判空余方向/放不下（与 CalcToolbarPosition 同因同修）
    RECT mon = {};
    int scrTop = c->vy, scrBottom = c->vy + c->vh;
    if (GetMonitorBoundsForRect(bar, mon)) { scrTop = mon.top; scrBottom = mon.bottom; }
    if (below && bar.bottom + menuH + gap > scrBottom) below = false;
    else if (!below && bar.top - menuH - gap < scrTop) below = true;
    return below;
}


// 展开二级菜单（LCM_Direction/LCM_Crop）或收起（LCM_None）：通过扩展/收缩工具栏窗口
// 实现，底条屏幕位置保持不动、只有菜单区伸缩；展开方向由 LongCaptureMenuOpenBelow
// 决定。菜单间直接切换（方向↔裁剪）时先按旧形态还原底条位置。
// 窗口伸缩与内容渲染由 LongCaptureToolbarRender 里的 UpdateLayeredWindow 一次原子提交
//（pptDst/psize 与位图同时更新，无「先伸缩后绘制」的中间帧）；底条与 popover 面板
// 之外的条带保持全透明（透底、鼠标穿透），popover 视觉上是脱离底条的独立浮层。
// 菜单开/关/切换同时收起 tooltip 并重置停顿（布局已变，旧锚点不再有效——尤其跨菜单
// 切换后 cell 编号相同的目标必须重新计时，避免陈旧目标吞掉新 tooltip）。

void LongCaptureSetMenu(LongCaptureContext* c, LCMenuKind kind) {
    if (!c || c->menuKind == kind) return;
    HWND tb = g_longToolbarWindow;
    LCMenuKind oldKind = c->menuKind;
    bool oldBelow = c->menuBelow;
    c->menuKind = kind;
    c->menuHover = -1;
    LongCaptureTooltipCancel();
    if (!tb) return;
    double ds = c->dpiScale;
    int barH = (int)(LC_BAR_H * ds + 0.5);
    int gap = (int)(LC_MENU_GAP * ds + 0.5);
    RECT wr;
    GetWindowRect(tb, &wr);
    // 底条在屏幕上的固定矩形（当前窗口可能含旧菜单区：menuBelow 时底条是顶部切片，
    // 否则是底部切片）
    RECT bar = wr;
    if (oldKind != LCM_None) {
        if (oldBelow) bar.bottom = wr.top + barH;
        else bar.top = wr.bottom - barH;
    }
    int newW = wr.right - wr.left;
    if (kind == LCM_None) {
        LongCaptureToolbarRender(c, bar.left, bar.top, newW, barH);
        return;
    }
    int menuH = (int)(LongCaptureMenuHeightLogi(c) * ds + 0.5);
    c->menuBelow = LongCaptureMenuOpenBelow(c, bar);
    int y = c->menuBelow ? bar.top : bar.top - menuH - gap;
    int newH = barH + menuH + gap;
    LongCaptureToolbarRender(c, bar.left, y, newW, newH);
}


// 方向切换后的完全重置：清空全部拼接/跟踪/历史状态并重抓首帧（首帧坐标系随方向改变）。

void LongCaptureResetSession(LongCaptureContext* c) {
    c->headRev.clear();
    c->body.clear();
    c->headRows = c->bodyRows = c->stitchH = 0;
    c->thumbHeadRev.clear();
    c->thumbBody.clear();
    c->thumbMerged.clear();
    c->thumbHeadH = c->thumbH = 0;
    c->thumbDirty = false;
    c->thumbDisplay.clear();
    c->thumbDisplayW = c->thumbDisplayH = 0;
    c->thumbDisplayDirty = false;
    c->lastFrame.clear();
    c->lastMatch = LongMatchData();
    c->frameHistory.clear();
    c->weakCandidateOffsets.clear();
    c->pendingMatch.valid = false;
    c->offsetHistory.clear();
    c->noChangeCount = 0;
    c->reachedBottom = false;
    c->weakTries = 0;
    c->wheelAccumDelta = 0;
    c->pixelsPerWheelNotch = 0.0f;
    c->wheelPending = false;
    c->stableRefValid = false;
    c->stableRefGray.clear();
    c->stableRefCols = c->stableRefH = 0;
    c->lastFailReason = LCFailReason::None;
    c->lastReject = LongMatchOutcome();
    c->cropPendTop = false;
    c->cropPendTopLo = c->cropPendTopHi = 0;
    c->cropPendBottom = false;
    c->cropPendBottomLo = c->cropPendBottomHi = 0;
    c->cropTopY = INT64_MIN;
    c->cropBottomY = INT64_MAX;
    c->cropped = false;
    c->autoFailStreak = 0;
    std::vector<uint32_t> buf;
    if (LongCaptureInitFirstFrame(c, buf)) {
        c->frameCount.store(1);
        c->lastSampleTick = GetTickCount();
    }
    LongCapturePanelUpdate(c);
    if (g_longControlWindow) InvalidateRect(g_longControlWindow, NULL, FALSE);
    LongCaptureToolbarRepaint();
}


// 切换长截图方向（纵向 ↔ 横向）。已拼接多帧（frameCount > 1）后禁用：
// 两个方向的内容坐标系不同，混拼必然产生错位，宁可要求用户取消重来。

void LongCaptureSwitchDirection(LongCaptureContext* c) {
    if (!c || c->frameCount.load() > 1) return;
    c->horizontal = !c->horizontal;
    // 帧缓冲转置复用纵向管线：physW/physH 交换（capW/capH 的屏幕采样 DIB 不变）
    c->physW = c->horizontal ? c->capH : c->capW;
    c->physH = c->horizontal ? c->capW : c->capH;
    // 缩略图列宽随新帧宽重算（面板预览内宽，不超过帧宽）
    int previewPx = (int)((LC_PANEL_W - 2 * LC_PANEL_PAD) * c->dpiScale + 0.5);
    c->thumbW = (std::min)((std::max)(1, previewPx), c->physW);
    LongCaptureResetSession(c);
}


// UI 维护节拍（100ms 轮询，NOACTIVATE 弹窗无焦点，只能轮询）：
// 1) 二级菜单展开时检测「窗口外左键按下」并关闭（按底条∪菜单面板区域精确判定，
//    联合区域外的透底条带视为窗外）；
// 2) 悬停意图：方向/裁剪锚点按钮停留 LC_POP_OPEN_DWELL_MS 后展开对应 popover（另一
//    菜单已展开时直接切换；扫过不误触）；鼠标离开「锚点按钮∪popover」超过
//    LC_POP_CLOSE_GRACE_MS 后收起（宽限期足够跨过按钮与浮层之间的透底间隙）；
// 3) title 式 tooltip：悬停目标稳定 LC_TIP_DELAY_MS 后显示（目标切换即重置停顿）。

// UiTick 的鼠标/二级菜单悬停计时状态：原为函数内 static，跨会话残留旧值会导致下一次
// 长截图复用上次状态（如左键按下标志误以为仍按下、popover 误计悬停/离开时长）。
// 提升为文件级 static 以便在工具栏窗口 WM_DESTROY 时复位（见 LongCaptureToolbarWndProc）。
static bool s_lcTbLDown = false;
static DWORD s_lcPopHoverSince = 0;   // 光标连续悬在某个二级菜单锚点按钮上的起始时刻（0=不在）
static DWORD s_lcPopLeaveSince = 0;   // 光标离开「锚点按钮∪popover」的起始时刻（0=未离开）

static void LongCaptureToolbarUiTick(LongCaptureContext* c, HWND tb) {
    bool down = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    bool pressed = down && !s_lcTbLDown;
    s_lcTbLDown = down;
    if (!c || !tb) return;
    POINT pt;
    GetCursorPos(&pt);
    POINT cl = pt;
    ScreenToClient(tb, &cl);
    RECT wr;
    GetWindowRect(tb, &wr);
    LongToolbarLayout L = LongCaptureToolbarLayout(c, wr.right - wr.left, wr.bottom - wr.top);
    int hv = LongCaptureHitTestToolbar(cl.x, cl.y, L);
    int cell = c->menuKind != LCM_None ? LongCaptureHitTestPopover(c, cl.x, cl.y, L) : -1;
    DWORD now = GetTickCount();
    // 光标是否在窗口可见区域内（底条 ∪ 菜单面板；联合区域外的透底条带不算）
    RECT barSc = L.bar, menuSc = L.menu;
    MapWindowPoints(tb, NULL, (POINT*)&barSc, 2);
    MapWindowPoints(tb, NULL, (POINT*)&menuSc, 2);
    bool inside = PtInRect(&barSc, pt) || (L.menuRows > 0 && PtInRect(&menuSc, pt));
    if (pressed && c->menuKind != LCM_None && !inside) {
        LongCaptureSetMenu(c, LCM_None);
        return;
    }
    // —— 二级 popover（方向/裁剪同款交互）：悬停展开 / 悬停切换 / 离开收起 ——
    // 点击收起过的锚点在光标移出该按钮前不再因悬停重开（popHoverDisarm，见 internal.h）
    LCMenuKind hoverKind = LongCaptureHoverMenuKind(c, hv);
    if (c->popHoverDisarm != LCM_None
        && hv != LongCaptureMenuAnchorItem(c->popHoverDisarm)) {
        c->popHoverDisarm = LCM_None;   // 离开被解除武装的锚点按钮即恢复悬停展开
    }
    if (hoverKind != LCM_None && hoverKind != c->menuKind
        && hoverKind != c->popHoverDisarm) {
        if (!s_lcPopHoverSince) s_lcPopHoverSince = now;
        if (now - s_lcPopHoverSince >= (DWORD)LC_POP_OPEN_DWELL_MS) {
            LongCaptureSetMenu(c, hoverKind);
            s_lcPopHoverSince = 0;
        }
    } else {
        s_lcPopHoverSince = 0;
    }
    if (c->menuKind != LCM_None) {
        // 「使用中」判定：popover cell、当前锚点，或正悬停准备切换的另一锚点，都不算离开
        bool usingPop = cell >= 0 || hv == LongCaptureMenuAnchorItem(c->menuKind)
                     || hoverKind != LCM_None;
        if (usingPop) {
            s_lcPopLeaveSince = 0;
        } else {
            if (!s_lcPopLeaveSince) s_lcPopLeaveSince = now;
            if (now - s_lcPopLeaveSince >= (DWORD)LC_POP_CLOSE_GRACE_MS) {
                LongCaptureSetMenu(c, LCM_None);
                s_lcPopLeaveSince = 0;
            }
        }
    } else {
        s_lcPopLeaveSince = 0;
    }
    // —— title 式 tooltip：底条按钮（无菜单时）或二级菜单 popover 图标 cell ——
    int tipTarget = -1;
    if (c->menuKind == LCM_None && hv >= 0 && hv != LTI_Size) tipTarget = hv;
    else if (c->menuKind != LCM_None && cell >= 0)
        tipTarget = 100 + (int)c->menuKind * 10 + cell;
    if (tipTarget != s_lcTipTarget) {
        s_lcTipTarget = tipTarget;
        s_lcTipSince = now;
        LongCaptureTooltipHide();
    } else if (s_lcTipTarget >= 0 && !LongCaptureTooltipVisible()
               && now - s_lcTipSince >= (DWORD)LC_TIP_DELAY_MS) {
        int cellIdx = (s_lcTipTarget - 100) % 10;
        RECT anchor = s_lcTipTarget >= 100
                    ? LongCapturePopoverCellRect(c, L, cellIdx)
                    : L.items[s_lcTipTarget];
        MapWindowPoints(tb, NULL, (POINT*)&anchor, 2);
        const wchar_t* text = s_lcTipTarget >= 100
                            ? LongCaptureMenuRowLabel(c, cellIdx)
                            : LongCaptureToolbarItemTip(c, s_lcTipTarget);
        if (text && *text) LongCaptureTooltipShow(c, text, anchor);
    }
}


// 工具栏窗口过程：hover 跟踪、点击分发、自动滚动/UI 定时器。窗口为分层
//（WS_EX_LAYERED），全部视觉内容由 LongCaptureToolbarRender 整幅渲染并经
// UpdateLayeredWindow 原子提交——WM_PAINT 不承载绘制（仅清验证区），
// 消除逐元素直接画屏的中间态闪动。

LRESULT CALLBACK LongCaptureToolbarWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: {
        // 分层窗口内容由 ULW 维护，这里只需清掉系统验证区
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_MOUSEMOVE: {
        LongCaptureContext* c = g_longCtx.load();
        if (!c) break;
        // 把手拖拽中（鼠标捕获在本窗口）：跟随鼠标平移工具栏窗口并钳制在虚拟
        // 屏幕内。分层窗口内容与位置无关，SetWindowPos 移动即可，无需重渲染。
        if (c->tbDragging && GetCapture() == hwnd) {
            POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            ClientToScreen(hwnd, &pt);
            double ds = c->dpiScale;
            auto sc = [ds](int v) { return (int)(v * ds + 0.5); };
            RECT wr;
            GetWindowRect(hwnd, &wr);
            int w = wr.right - wr.left, h = wr.bottom - wr.top;
            int nx = pt.x - c->tbDragGrabDX, ny = pt.y - c->tbDragGrabDY;
            int minX = c->vx + sc(4), minY = c->vy;
            int maxX = (std::max)(minX, c->vx + c->vw - sc(4) - w);
            int maxY = (std::max)(minY, c->vy + c->vh - h);
            if (nx < minX) nx = minX;
            if (nx > maxX) nx = maxX;
            if (ny < minY) ny = minY;
            if (ny > maxY) ny = maxY;
            if (nx != wr.left || ny != wr.top)
                SetWindowPos(hwnd, NULL, nx, ny, 0, 0,
                             SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        TRACKMOUSEEVENT tme = {sizeof(tme), TME_LEAVE, hwnd, 0};
        TrackMouseEvent(&tme);
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        RECT cr;
        GetClientRect(hwnd, &cr);
        LongToolbarLayout L = LongCaptureToolbarLayout(c, cr.right, cr.bottom);
        int hv = LongCaptureHitTestToolbar(x, y, L);
        int mh = c->menuKind != LCM_None ? LongCaptureHitTestPopover(c, x, y, L) : -1;
        if (hv != c->tbHover || mh != c->menuHover) {
            c->tbHover = hv;
            c->menuHover = mh;
            LongCaptureTooltipHide();   // 悬停目标切换立即收起 tooltip（停顿重置由 UiTick 轮询完成）
            LongCaptureToolbarRepaint();
        }
        return 0;
    }
    case WM_MOUSELEAVE: {
        LongCaptureContext* c = g_longCtx.load();
        if (c && (c->tbHover != -1 || c->menuHover != -1)) {
            c->tbHover = -1;
            c->menuHover = -1;
            LongCaptureToolbarRepaint();
        }
        return 0;
    }
    case WM_LBUTTONDOWN: {
        // 记录按下目标：随后的 WM_LBUTTONUP 必须命中同一目标才触发动作（标准按钮
        // 语义）。关键防误触场景：编辑工具栏「长截图」按钮在鼠标按下瞬间进入长截图，
        // 本工具栏立即在附近生成——若「自动滚动」等按钮恰与原按钮屏幕同位，先前那次
        // 点击残留的松开事件会直接落在它上面；没有先行 DOWN 记录的 UP 一律吞掉，
        // 自动滚动因此绝不会被「进入长截图即选中」。
        LongCaptureContext* c = g_longCtx.load();
        if (!c) break;
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        RECT cr;
        GetClientRect(hwnd, &cr);
        LongToolbarLayout L = LongCaptureToolbarLayout(c, cr.right, cr.bottom);
        // 按下最左「6 点把手」：收起二级菜单并进入工具栏窗口拖拽（捕获鼠标，
        // UP 时结束）。拖拽不是点击，清空按下记录避免残留配对误触按钮；
        // 抓取偏移必须在收起菜单之后计算——收起会同步收缩窗口（ULW 原子提交），
        // 菜单在上侧时窗口顶边位置会变，先取偏移会把菜单高度算进抓取点。
        if (LongCaptureHitTestToolbar(x, y, L) == LTI_Grip) {
            if (c->menuKind != LCM_None) LongCaptureSetMenu(c, LCM_None);
            RECT wr;
            GetWindowRect(hwnd, &wr);
            POINT pt = { x, y };
            ClientToScreen(hwnd, &pt);
            c->tbDragGrabDX = pt.x - wr.left;
            c->tbDragGrabDY = pt.y - wr.top;
            c->tbDragging = true;
            c->tbPressItem = -1;
            c->tbPressMenuRow = -1;
            LongCaptureTooltipCancel();
            SetCapture(hwnd);
            return 0;
        }
        c->tbPressItem = LongCaptureHitTestToolbar(x, y, L);
        c->tbPressMenuRow = c->menuKind != LCM_None ? LongCaptureHitTestPopover(c, x, y, L) : -1;
        return 0;
    }
    case WM_LBUTTONUP: {
        LongCaptureContext* c = g_longCtx.load();
        if (!c) break;
        // 把手拖拽结束：释放捕获并退出拖拽态，不落入按钮点击逻辑
        //（位置已由 MOUSEMOVE 的 SetWindowPos 固化）
        if (c->tbDragging) {
            c->tbDragging = false;
            c->tbPressItem = -1;
            c->tbPressMenuRow = -1;
            if (GetCapture() == hwnd) ReleaseCapture();
            return 0;
        }
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        RECT cr;
        GetClientRect(hwnd, &cr);
        LongToolbarLayout L = LongCaptureToolbarLayout(c, cr.right, cr.bottom);
        LongCaptureTooltipCancel();   // 任何点击立即收起 tooltip 并清零停顿（网页 title 同款）
        // 按下-抬起同目标校验（DOWN 记录见 WM_LBUTTONDOWN）：菜单 cell 要求同 cell，
        // 工具栏按钮要求同按钮且按下时不在 popover 上；不匹配直接吞掉本次 UP。
        int hit = LongCaptureHitTestToolbar(x, y, L);
        int menuRow = c->menuKind != LCM_None ? LongCaptureHitTestPopover(c, x, y, L) : -1;
        bool sameTarget = menuRow >= 0 ? menuRow == c->tbPressMenuRow
                          : hit >= 0 && hit == c->tbPressItem && c->tbPressMenuRow < 0;
        c->tbPressItem = -1;          // 一次按下-抬起周期结束，防止陈旧记录误配后续点击
        c->tbPressMenuRow = -1;
        if (!sameTarget) return 0;
        // 二级菜单项点击：popover 图标 cell → 裁剪立即应用对应项并收起；方向 → 切换
        // 纵向/横向（禁用 cell 仅收起），随后收起
        if (menuRow >= 0) {
            if (c->menuKind == LCM_Crop) {
                LongCaptureApplyCrop(c, menuRow);
            } else if (LongCaptureMenuRowEnabled(c, menuRow)) {
                bool wantHorizontal = (menuRow == 1);
                if (wantHorizontal != c->horizontal) LongCaptureSwitchDirection(c);
            }
            LongCaptureSetMenu(c, LCM_None);
            return 0;
        }
        // 点击任一直接动作按钮时收起展开中的菜单（方向/裁剪按钮自身负责切换菜单状态）
        if (hit >= 0 && hit != LTI_Direction && hit != LTI_Crop && c->menuKind != LCM_None) {
            LongCaptureSetMenu(c, LCM_None);
        }
        switch (hit) {
        case LTI_Finish:    c->finishFlag.store(true); break;
        case LTI_Cancel:    c->abortFlag.store(true); break;
        case LTI_Save:      if (!c->saveFlag.load()) c->saveFlag.store(true); break;
        case LTI_Direction:
            // 点击方向：与裁剪同款开合（悬停展开见 UiTick）。已拼接多帧后方向锁定
            //（两个方向的内容坐标系不可混拼），点击不展开菜单
            if (c->frameCount.load() <= 1) {
                if (c->menuKind == LCM_Direction) {
                    LongCaptureSetMenu(c, LCM_None);
                    c->popHoverDisarm = LCM_Direction;
                } else {
                    LongCaptureSetMenu(c, LCM_Direction);
                }
            }
            break;
        case LTI_AutoScroll: LongCaptureSetAutoScroll(c, !c->autoScroll); break;
        case LTI_Crop:
            // 点击裁剪：未展开则立即展开；已展开则收起并解除悬停武装（需移出按钮再
            // 进入才会因悬停重开，防止点击收起与悬停展开互相打架）
            if (c->menuKind == LCM_Crop) {
                LongCaptureSetMenu(c, LCM_None);
                c->popHoverDisarm = LCM_Crop;
            } else {
                LongCaptureSetMenu(c, LCM_Crop);
            }
            break;
        default: break;
        }
        return 0;
    }
    case WM_TIMER: {
        LongCaptureContext* c = g_longCtx.load();
        if (!c) break;
        if (wp == LC_TIMER_AUTOSCROLL) {
            if (c->autoScroll) LongCaptureAutoScrollTick(c);
        } else if (wp == LC_TIMER_UI) {
            LongCaptureToolbarUiTick(c, hwnd);
        }
        return 0;
    }
    case WM_SETCURSOR: {
        // 拖拽中或悬停「6 点把手」单元格：四向箭头提示可拖动；其余箭头
        bool sizeAll = false;
        POINT ptCursor;
        {
            LongCaptureContext* c = g_longCtx.load();
            if (c) {
                if (c->tbDragging) {
                    sizeAll = true;
                } else if (GetCursorPos(&ptCursor)) {
                    POINT cl = ptCursor;
                    ScreenToClient(hwnd, &cl);
                    RECT cr;
                    GetClientRect(hwnd, &cr);
                    LongToolbarLayout L = LongCaptureToolbarLayout(c, cr.right, cr.bottom);
                    sizeAll = (LongCaptureHitTestToolbar(cl.x, cl.y, L) == LTI_Grip);
                }
            }
        }
        SetCursor(LoadCursor(NULL, sizeAll ? IDC_SIZEALL : IDC_ARROW));
        return TRUE;
    }
    case WM_CAPTURECHANGED: {
        // 捕获被系统夺走（弹窗等）：安全退出拖拽态
        LongCaptureContext* c = g_longCtx.load();
        if (c && c->tbDragging && (HWND)lp != hwnd) {
            c->tbDragging = false;
            c->tbPressItem = -1;
            c->tbPressMenuRow = -1;
        }
        return 0;
    }
    case WM_NCHITTEST: return HTCLIENT;
    case WM_ERASEBKGND: return 1;
    case WM_DESTROY:
        KillTimer(hwnd, LC_TIMER_AUTOSCROLL);
        KillTimer(hwnd, LC_TIMER_UI);
        s_lcIcons.Cleanup();   // 工具栏图标缓存随窗口销毁释放（下次会话按新 DPI 重建）
        FreeArgbSurface(s_lcTbSurfDC, s_lcTbSurfBmp, s_lcTbSurfBits, s_lcTbSurfW, s_lcTbSurfH);
        if (g_lcTooltipWindow) {   // title 式 tooltip 窗口随工具栏一并销毁
            DestroyWindow(g_lcTooltipWindow);
            g_lcTooltipWindow = NULL;
        }
        FreeArgbSurface(s_lcTipSurfDC, s_lcTipSurfBmp, s_lcTipSurfBits, s_lcTipSurfW, s_lcTipSurfH);
        s_lcTooltipText.clear();
        s_lcTipTarget = -1;
        s_lcTipSince = 0;
        // 复位 UiTick 鼠标/二级菜单悬停计时状态（原为函数级 static 会跨会话残留，致使下次
        // 长截图复用旧状态），随工具栏窗口销毁清零
        s_lcTbLDown = false;
        s_lcPopHoverSince = 0;
        s_lcPopLeaveSince = 0;
        return 0;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}


// 创建工具栏窗口：选区下方居中（放不下退上方、再退选区内底部），避让右侧小地图面板。

HWND LongCaptureCreateToolbar(CaptureContext* ctx, LongCaptureContext* c) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = LongCaptureToolbarWndProc;
        wc.hInstance = GetModuleHandle(NULL);
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.lpszClassName = L"ZToolsLongCaptureToolbar";
        RegisterClassExW(&wc);
        registered = true;
    }
    double ds = ctx->dpiScale;
    auto sc = [ds](int v) { return (int)(v * ds + 0.5); };
    // 图标缓存：按当前 DPI 光栅化（与主截图工具栏同比例：按钮边长 − 8 后缩放）
    s_lcIcons.Init(sc(LC_BAR_BTN_W - 8) + 2);
    int w = LongCaptureToolbarWindowWidth(c);
    int h = sc(LC_BAR_H);
    int margin = sc(LC_BAR_MARGIN);
    // 放置边界取「选区所在显示器」（多屏异分辨率：整虚拟屏幕包围盒会被高分屏拉大，
    // 低分屏选区已触本屏底边仍会误判"下方放得下"而不上置——与编辑态
    // CalcToolbarPosition 同因同修）。下→上→贴近底部三级回退同款规则。
    RECT mon = {};
    int bLeft = ctx->virtualX, bTop = ctx->virtualY;
    int bRight = ctx->virtualX + ctx->virtualW, bBottom = ctx->virtualY + ctx->virtualH;
    if (GetMonitorBoundsForRect(c->selection, mon)) {
        bLeft = mon.left; bTop = mon.top; bRight = mon.right; bBottom = mon.bottom;
    }
    int x = (c->selection.left + c->selection.right) / 2 - w / 2;
    int y = c->selection.bottom + margin;
    // 选区下方放不下 -> 上方
    if (y + h > bBottom) y = c->selection.top - margin - h;
    // 上方也放不下 -> 贴近底部（选区内底边），并钳回显示器范围兜底
    if (y < bTop) {
        y = c->selection.bottom - margin - h;
        if (y < c->selection.top) y = c->selection.top + margin;
        if (y + h > bBottom) y = bBottom - h;
        if (y < bTop) y = bTop;
    }
    if (x + w > bRight - sc(4)) x = bRight - w - sc(4);
    if (x < bLeft + sc(4)) x = bLeft + sc(4);
    // 小地图面板避让：面板与工具栏矩形重叠时把工具栏左移到面板左侧
    if (g_longControlWindow) {
        RECT pr;
        if (GetWindowRect(g_longControlWindow, &pr)
            && pr.left < x + w && pr.right > x && pr.top < y + h && pr.bottom > y) {
            int nx = pr.left - margin - w;
            if (nx >= ctx->virtualX + sc(4)) x = nx;
        }
    }
    // 分层窗口（WS_EX_LAYERED）：内容整幅渲染后 UpdateLayeredWindow 提交（见
    // LongCaptureToolbarRender），圆角经 alpha 抗锯齿；不再用 SetWindowRgn 裁剪外形
    HWND tb = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED,
        L"ZToolsLongCaptureToolbar", L"LongCaptureToolbar", WS_POPUP, x, y, w, h,
        NULL, NULL, GetModuleHandle(NULL), NULL);
    if (tb) {
        ShowWindow(tb, SW_SHOWNOACTIVATE);
        LongCaptureToolbarRender(c, x, y, w, h);   // 分层窗口首帧内容（此前不可见）
        SetTimer(tb, LC_TIMER_UI, 100, NULL);
    }
    return tb;
}

