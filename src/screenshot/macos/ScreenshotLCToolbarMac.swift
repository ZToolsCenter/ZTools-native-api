import Foundation
import AppKit
import CoreGraphics

// MARK: - 长截图工具栏（macOS；
//         Windows 基准 lc_toolbar_ui_windows.cpp）
//
// 选区底部悬浮工具栏（独立 TOPMOST 弹窗，位于灰蒙版之上），从左到右（图标按钮）：
//   [6点把手] [预览宽×高] | [方向] [自动滚动] [裁剪] | [保存到本地] [取消] [完成并复制]
// Windows 为 WS_EX_LAYERED 单窗口（popover 展开时窗口整体伸缩、ULW 原子提交）；macOS
// 改为「底条窗口 + 独立 popover 窗口 + tooltip 窗口」三浮层（编辑工具栏同款架构），
// 视觉与交互逐条对齐：
// - 布局常量（逻辑像素）：条高 44、按钮 32、间距 2、尺寸标签 104、内边距 6、分隔线 13、
//   圆角 8、选区间距 8、popover 高 44 / pad 6 / cell 32 / gap 2（LC_BAR_*/LC_POP_*）
// - 方向/裁剪二级 popover：悬停 300ms 展开（LC_POP_OPEN_DWELL_MS，扫过不误触）、鼠标离开
//   「锚点按钮∪popover」250ms 宽限收起（LC_POP_CLOSE_GRACE_MS）、点击开合并解除悬停武装
//   （popHoverDisarm 语义）；展开方向永远避让选区（LongCaptureMenuOpenBelow）——菜单浮层
//   绝不进入选区画面，否则会被逐帧采样采进拼接内容、污染重叠识别基准
// - 方向锁定：已拼接多帧（frameCount > 1）后方向按钮置灰、popover 不再展开（两个方向的
//   内容坐标系不同，混拼必然错位）；自动滚动图标随方向切换 V/H 变体
// - 裁剪 badge：已裁剪（cropped，含待剔除区间登记）时裁剪按钮 active 高亮
// - title 式 tooltip：悬停 500ms（LC_TIP_DELAY_MS）深色圆角气泡，锚定目标上方/下方
// - 按钮按下-抬起同目标校验（防「进入长截图瞬间残留的鼠标抬起」误触自动滚动等按钮）
//
// 事件模型：NOACTIVATE 浮层无焦点，hover/菜单/tooltip 全部由泵循环 100ms UI 节拍轮询
// （NSEvent.mouseLocation / pressedMouseButtons，对齐 Windows LongCaptureToolbarUiTick 的
// GetAsyncKeyState + GetCursorPos 轮询）；点击由浮层视图 mouseDown/Up 承接。

// MARK: - 常量（Windows 出处集中标注）

/// 工具栏几何（lc_toolbar_ui_windows.cpp LC_BAR_*，逻辑像素）。
private let LC_BAR_H: CGFloat = 44          // 工具栏高度（图标按钮 32 + 上下内边距 6）
private let LC_BAR_BTN: CGFloat = 32        // 图标按钮宽度（正方形 cell）
private let LC_BAR_GAP: CGFloat = 2         // 相邻图标按钮间距
private let LC_SIZE_W: CGFloat = 104        // 预览宽×高标签占位宽
private let LC_BAR_PAD: CGFloat = 6         // 左右内边距
private let LC_BAR_SEP_W: CGFloat = 13      // 分隔线占位宽（含两侧间距）
private let LC_BAR_RADIUS: CGFloat = 8      // 圆角半径
private let LC_BAR_MARGIN: CGFloat = 8      // 选区到工具栏间距
private let LC_MENU_GAP: CGFloat = 6        // popover 与工具栏间距

/// 裁剪/方向 popover 几何（lc_toolbar_ui_windows.cpp LC_POP_*）。
private let LC_POP_H: CGFloat = 44          // popover 面板高（图标 cell 32 + 上下内边距 6）
private let LC_POP_PAD: CGFloat = 6         // popover 面板内边距
private let LC_POP_CELL: CGFloat = 32       // popover 图标 cell 宽（与工具栏按钮同尺寸）
private let LC_POP_CELL_GAP: CGFloat = 2    // popover 图标 cell 间距

/// 二级 popover 悬停展开/宽限收起/tooltip 延时（lc_toolbar_ui_windows.cpp）。
let LC_POP_OPEN_DWELL_MS: UInt64 = 300      // 锚点按钮悬停多久后展开（悬停意图判定，扫过不误触）
let LC_POP_CLOSE_GRACE_MS: UInt64 = 250     // 鼠标离开「锚点∪popover」多久后收起（跨间隙宽限）
let LC_TIP_DELAY_MS: UInt64 = 500           // 图标悬停多久后显示 tooltip（网页 title 同款节奏）

/// 图标三态色（lc_toolbar_ui_windows.cpp LCIconCache：dark RGB(60,60,60) / blue RGB(9,105,218) /
/// gray RGB(178,178,178)；hover/active 底 = SC_THEME_HOVER_BG/SEL_BG）。
private let lcIconDark = NSColor(srgbRed: 60 / 255.0, green: 60 / 255.0, blue: 60 / 255.0, alpha: 1)
private let lcIconBlue = NSColor(srgbRed: 9 / 255.0, green: 105 / 255.0, blue: 218 / 255.0, alpha: 1)
private let lcIconGray = NSColor(srgbRed: 178 / 255.0, green: 178 / 255.0, blue: 178 / 255.0, alpha: 1)
private let lcHoverBg = NSColor(srgbRed: 235 / 255.0, green: 243 / 255.0, blue: 255 / 255.0, alpha: 1)
private let lcActiveBg = NSColor(srgbRed: 225 / 255.0, green: 237 / 255.0, blue: 253 / 255.0, alpha: 1)
private let lcBorderCol = NSColor(srgbRed: 210 / 255.0, green: 210 / 255.0, blue: 210 / 255.0, alpha: 1)
private let lcSepCol = NSColor(srgbRed: 230 / 255.0, green: 230 / 255.0, blue: 230 / 255.0, alpha: 1)
private let lcSizeLabelCol = NSColor(srgbRed: 130 / 255.0, green: 130 / 255.0, blue: 130 / 255.0, alpha: 1)
private let lcGripDotCol = NSColor(srgbRed: 165 / 255.0, green: 165 / 255.0, blue: 165 / 255.0, alpha: 1)

/// 圆角矩形路径（本地辅助；工具栏 roundedRectPath 同款实现）。
private func lcRoundedRectPath(_ rect: CGRect, _ radius: CGFloat) -> CGPath {
    return CGPath(roundedRect: rect, cornerWidth: radius, cornerHeight: radius, transform: nil)
}

/// 鼠标当前位置（CG 全局逻辑坐标；NSEvent.mouseLocation 为 NS 坐标需 Y 翻转）。
private func lcCurrentMouseCG() -> CGPoint {
    return ScreenshotGeometry.cgPoint(fromNS: NSEvent.mouseLocation)
}

// MARK: - 工具栏项目与二级菜单

/// 工具栏项目（顺序即布局顺序；分隔线不可点击；对齐 lc_toolbar_ui_windows.cpp LongToolbarItem）。
private enum LCItem: Int, CaseIterable {
    case grip = 0        // 拖拽把手（6 点图标）
    case size            // 预览宽×高标签（纯展示）
    case sep1            // 分隔线
    case direction       // 方向（悬停/点击展开 popover：纵向/横向；已拼接多帧后锁定）
    case autoScroll      // 自动滚动开关（开启态高亮；图标随方向切换 V/H 变体）
    case crop            // 裁剪（悬停/点击展开 popover；badge = 已裁剪高亮）
    case sep3            // 分隔线
    case save            // 保存到本地
    case cancel          // 取消
    case finish          // 完成并复制
}

/// 二级菜单种类（对齐 LCMenuKind）。
private enum LCMenuKindEquatable: Equatable {
    case none
    case direction
    case crop
}

// MARK: - 浮层视图（事件转发）

/// 工具栏底条自绘视图：绘制委托控制器；点击/拖动换算 CG 坐标转发。
final class ScreenshotLCToolbarView: NSView {
    unowned let controller: ScreenshotLCToolbarController

    init(controller: ScreenshotLCToolbarController, frame: NSRect) {
        self.controller = controller
        super.init(frame: frame)
    }

    required init?(coder: NSCoder) {
        fatalError("ScreenshotLCToolbarView is created programmatically only")
    }

    override var isFlipped: Bool { return true }

    // 首击穿透：与 OverlayScreenshotView.acceptsFirstMouse 同因——App 未激活（协作式激活
    // 失败）时，非 key 浮层窗口的首次点击会被 AppKit 当"激活点击"吞掉，长截图工具栏
    // 按钮（完成/取消/方向锁定等）第一次点按无响应；覆写后首击直达本视图。
    override func acceptsFirstMouse(for event: NSEvent?) -> Bool { return true }

    override func draw(_ dirtyRect: NSRect) {
        guard let ctx = NSGraphicsContext.current?.cgContext else { return }
        controller.drawBar(ctx)
    }

    /// 本地坐标 → CG 全局坐标（视图 (0,0) = 底条矩形左上角）。
    private func cgPoint(from event: NSEvent) -> CGPoint {
        let local = convert(event.locationInWindow, from: nil)
        return CGPoint(x: (local.x + controller.barRect.minX).rounded(),
                       y: (local.y + controller.barRect.minY).rounded())
    }

    override func mouseDown(with event: NSEvent) {
        controller.handleBarMouseDown(cgPoint(from: event))
    }

    override func mouseDragged(with event: NSEvent) {
        controller.handleBarMouseDragged(cgPoint(from: event))
    }

    override func mouseUp(with event: NSEvent) {
        controller.handleBarMouseUp(cgPoint(from: event))
    }
}

/// popover 自绘视图：绘制委托控制器；点击换算 CG 坐标做 cell 命中。
final class ScreenshotLCPopoverView: NSView {
    unowned let controller: ScreenshotLCToolbarController

    init(controller: ScreenshotLCToolbarController, frame: NSRect) {
        self.controller = controller
        super.init(frame: frame)
    }

    required init?(coder: NSCoder) {
        fatalError("ScreenshotLCPopoverView is created programmatically only")
    }

    override var isFlipped: Bool { return true }

    // 首击穿透：同 ScreenshotLCToolbarView——App 未激活时 popover 首次点击会被吞掉。
    override func acceptsFirstMouse(for event: NSEvent?) -> Bool { return true }

    override func draw(_ dirtyRect: NSRect) {
        guard let ctx = NSGraphicsContext.current?.cgContext else { return }
        controller.drawPopover(ctx)
    }

    override func mouseDown(with event: NSEvent) {
        let local = convert(event.locationInWindow, from: nil)
        let cg = CGPoint(x: (local.x + controller.popoverRect.minX).rounded(),
                         y: (local.y + controller.popoverRect.minY).rounded())
        controller.handlePopoverMouseDown(cg)
    }
}

/// 长截图 tooltip 气泡视图：深色圆角底 + 白色居中文本（TooltipPanelView 同款视觉）。
final class ScreenshotLCTipView: NSView {
    let text: String

    init(text: String, frame: NSRect) {
        self.text = text
        super.init(frame: frame)
    }

    required init?(coder: NSCoder) {
        fatalError("ScreenshotLCTipView is created programmatically only")
    }

    override var isFlipped: Bool { return true }

    override func draw(_ dirtyRect: NSRect) {
        guard let ctx = NSGraphicsContext.current?.cgContext else { return }
        let path = lcRoundedRectPath(bounds, 4)   // 圆角 4（LongCaptureTooltipRender）
        ctx.addPath(path)
        ctx.setFillColor(SC.tipBg.cgColor)        // RGB(41,41,41)
        ctx.fillPath()
        let attr = NSAttributedString(string: text, attributes: [
            .font: NSFont.systemFont(ofSize: SC.fontPx),
            .foregroundColor: NSColor.white,
        ])
        let size = attr.size()
        attr.draw(at: NSPoint(x: (bounds.width - ceil(size.width)) / 2,
                              y: (bounds.height - ceil(size.height)) / 2))
    }
}

// MARK: - 工具栏控制器

/// 长截图工具栏控制器：底条/popover/tooltip 三浮层 + 全部 UI 状态，按钮动作回话给
/// ScreenshotLongCaptureSession（finish/save/abort 标志 + 自动滚动/方向/裁剪）。
/// 100ms UI 节拍由长截图会话泵循环驱动（uiTick）。
final class ScreenshotLCToolbarController {
    private weak var session: ScreenshotLongCaptureSession?

    // ---- 三浮层窗口 ----
    private(set) var window: ScreenshotPanelWindow?
    private var barView: ScreenshotLCToolbarView?
    private(set) var popoverWindow: ScreenshotPanelWindow?
    private var popoverView: ScreenshotLCPopoverView?
    private(set) var tipWindow: ScreenshotPanelWindow?
    private var tipDwelling = false

    /// 底条矩形（CG 全局逻辑坐标；菜单开合不改变底条屏幕位置——独立 popover 架构下
    /// 天然成立；小地图避让与生长约束读取本值）。
    private(set) var barRect: CGRect = .null
    /// popover 矩形（CG 全局逻辑坐标；展开时有效）。
    private(set) var popoverRect: CGRect = .null

    // ---- UI 状态（对齐 LcUiState + LongCaptureToolbarUiTick 的文件级 static）----
    private var hoverItem = -1                       // 悬停的底条项目（LCItem.rawValue；-1 无）
    private var menuHover = -1                       // 悬停的 popover cell（-1 无）
    private var pressItem = -1                       // 按下目标（UP 同目标校验）
    private var pressMenuRow = -1
    private var dragging = false                     // 把手拖拽中
    private var dragGrabDX: CGFloat = 0              // 抓取偏移（收起菜单后计算，防菜单高计入）
    private var dragGrabDY: CGFloat = 0
    private var lDown = false                        // 全局左键状态（UiTick 轮询）
    private var menuKind: LCMenuKindEquatable = .none
    private var menuBelow = false                    // popover 展开方向（避让选区）
    private var popHoverDisarm: LCMenuKindEquatable = .none   // 点击收起后的悬停武装解除
    private var popHoverSinceMs: UInt64 = 0          // 悬停锚点起始时刻（0=不在）
    private var popLeaveSinceMs: UInt64 = 0          // 离开「锚点∪popover」起始时刻（0=未离开）
    private var tipTarget = -1                       // tooltip 停顿目标（底条 item 或 100+kind*10+cell）
    private var tipSinceMs: UInt64 = 0
    private var tipShown = false

    init(session: ScreenshotLongCaptureSession) {
        self.session = session
    }

    // MARK: 布局（LongCaptureToolbarLayout 的底条段移植）

    /// 各项目矩形（底条本地坐标）：图标按钮为等宽正方形 cell 垂直居中，分隔线占位宽
    /// 自带两侧间距。
    private func layoutItemRects() -> [CGRect] {
        var rects = [CGRect](repeating: .zero, count: LCItem.allCases.count)
        var x = LC_BAR_PAD
        let btnTop = (LC_BAR_H - LC_BAR_BTN) / 2
        for item in LCItem.allCases {
            switch item {
            case .grip, .direction, .autoScroll, .crop, .save, .cancel, .finish:
                rects[item.rawValue] = CGRect(x: x, y: btnTop, width: LC_BAR_BTN, height: LC_BAR_BTN)
                x += LC_BAR_BTN + LC_BAR_GAP
            case .size:
                rects[item.rawValue] = CGRect(x: x, y: btnTop, width: LC_SIZE_W, height: LC_BAR_BTN)
                x += LC_SIZE_W + LC_BAR_GAP
            case .sep1, .sep3:
                rects[item.rawValue] = CGRect(x: x, y: 10, width: LC_BAR_SEP_W, height: LC_BAR_H - 20)
                x += LC_BAR_SEP_W
            }
        }
        return rects
    }

    /// 底条总宽（与 layoutItemRects 的横向排布严格一致；对齐 LongCaptureToolbarWindowWidth）。
    private var barWidth: CGFloat {
        return layoutItemRects().map { $0.maxX }.max().map { $0 + LC_BAR_PAD } ?? 0
    }

    /// 二级菜单 popover 行数（= 图标 cell 数）：方向恒 2（纵向/横向）；裁剪 2 +（已裁剪时）重置。
    private func menuRows() -> Int {
        guard let session = session else { return 0 }
        if menuKind == .direction { return 2 }
        if menuKind == .crop { return session.isCropped ? 3 : 2 }
        return 0
    }

    /// 二级菜单的锚点项目（popover 水平居中对齐、悬停展开与离开收起均围绕锚点判定）。
    private var menuAnchorItem: Int {
        return menuKind == .crop ? LCItem.crop.rawValue : LCItem.direction.rawValue
    }

    /// 悬停的底条按钮将展开的二级菜单：裁剪恒可展开；方向在已拼接多帧（frameCount>1）后
    /// 锁定（锁定期间悬停/点击均不展开）。
    private func hoverMenuKind(_ hv: Int) -> LCMenuKindEquatable {
        guard let session = session else { return .none }
        if hv == LCItem.crop.rawValue { return .crop }
        if hv == LCItem.direction.rawValue && session.frameCount <= 1 { return .direction }
        return .none
    }

    /// popover 第 i 个 cell 矩形（popover 本地坐标：面板内从左到右等宽排布）。
    private func popoverCellRect(_ i: Int) -> CGRect {
        let left = LC_POP_PAD + CGFloat(i) * (LC_POP_CELL + LC_POP_CELL_GAP)
        return CGRect(x: left, y: LC_POP_PAD, width: LC_POP_CELL, height: LC_POP_CELL)
    }

    /// 命中二级菜单 popover 图标 cell（-1 = 不在 popover 内；point 为 CG 全局坐标）。
    private func hitTestPopover(_ point: CGPoint) -> Int {
        guard menuRows() > 0, !popoverRect.isNull else { return -1 }
        guard scPointInRect(point, popoverRect) else { return -1 }
        let local = CGPoint(x: point.x - popoverRect.minX, y: point.y - popoverRect.minY)
        for i in 0..<menuRows() {
            if scPointInRect(local, popoverCellRect(i)) { return i }
        }
        return -1
    }

    /// 命中底条项目（-1 = 无；分隔线不可点击；point 为 CG 全局坐标）。
    private func hitTestBar(_ point: CGPoint) -> Int {
        guard !barRect.isNull, scPointInRect(point, barRect) else { return -1 }
        let local = CGPoint(x: point.x - barRect.minX, y: point.y - barRect.minY)
        let rects = layoutItemRects()
        for item in LCItem.allCases {
            if item == .sep1 || item == .sep3 { continue }
            if scPointInRect(local, rects[item.rawValue]) { return item.rawValue }
        }
        return -1
    }

    // MARK: 生命周期

    /// 创建底条窗口：选区下方居中（放不下退上方、再退选区内底部），避让右侧小地图面板
    /// （对齐 LongCaptureCreateToolbar 的放置边界与退化链）。
    func create() {
        guard let session = session, barRect.isNull, window == nil else { return }
        let w = barWidth
        let h = LC_BAR_H
        let margin = LC_BAR_MARGIN
        // 放置边界取「选区所在显示器」（多屏异分辨率时整虚拟屏包络会被高分屏拉大）
        let bounds = scMonitorBounds(for: session.selection) ?? session.virtualBounds
        let sel = session.selection
        var x = sel.midX - w / 2
        var y = sel.maxY + margin
        // 选区下方放不下 → 上方
        if y + h > bounds.maxY { y = sel.minY - margin - h }
        // 上方也放不下 → 贴近底部（选区内底边），并钳回显示器范围兜底
        if y < bounds.minY {
            y = sel.maxY - margin - h
            if y < sel.minY { y = sel.minY + margin }
            if y + h > bounds.maxY { y = bounds.maxY - h }
            if y < bounds.minY { y = bounds.minY }
        }
        if x + w > bounds.maxX - 4 { x = bounds.maxX - w - 4 }
        if x < bounds.minX + 4 { x = bounds.minX + 4 }
        // 小地图面板避让：面板与工具栏矩形重叠时把工具栏左移到面板左侧
        if let pr = session.panel?.panelRect, !pr.isNull,
           pr.minX < x + w, pr.maxX > x, pr.minY < y + h, pr.maxY > y {
            let nx = pr.minX - margin - w
            if nx >= session.virtualBounds.minX + 4 { x = nx }
        }
        barRect = CGRect(x: x, y: y, width: w, height: h)
        let win = ScreenshotPanelWindow(
            contentRect: lcNSRect(fromCG: barRect), styleMask: .borderless,
            backing: .buffered, defer: false)
        // 层级 = 小地图 +1（小地图 = 蒙版 +1）：整族在蒙版之上、抓帧排除范围之内
        win.level = NSWindow.Level(rawValue: NSWindow.Level.screenSaver.rawValue + 3)
        win.isOpaque = false
        win.backgroundColor = .clear
        win.hasShadow = false
        win.collectionBehavior = [.canJoinAllSpaces, .fullScreenAuxiliary]
        win.isReleasedWhenClosed = false
        let view = ScreenshotLCToolbarView(
            controller: self, frame: NSRect(origin: .zero, size: barRect.size))
        win.contentView = view
        window = win
        barView = view
        win.orderFrontRegardless()
    }

    /// 销毁全部浮层（长截图收束/取消时调用；幂等）。
    func destroy() {
        for win in [window, popoverWindow, tipWindow] {
            win?.orderOut(nil)
            win?.contentView = nil
        }
        window = nil
        barView = nil
        popoverWindow = nil
        popoverView = nil
        tipWindow = nil
        barRect = .null
        popoverRect = .null
        hoverItem = -1
        menuHover = -1
        pressItem = -1
        pressMenuRow = -1
        dragging = false
        menuKind = .none
        popHoverDisarm = .none
        popHoverSinceMs = 0
        popLeaveSinceMs = 0
        tipTarget = -1
        tipShown = false
    }

    // MARK: 刷新入口

    /// 全量重绘（hover/菜单开合/宽×高标签/自动滚动开关/裁剪 badge 等任何状态变化后调用；
    /// 对齐 LongCaptureToolbarRepaint）。
    func refreshAll() {
        barView?.needsDisplay = true
        if menuKind != .none { popoverView?.needsDisplay = true }
    }

    /// 收起二级菜单（保存模态等场景；对齐 LongCaptureSetMenu(c, LCM_None) 的收起段）。
    func closeMenu() {
        openMenu(.none)
    }

    // MARK: 二级菜单开合

    /// 展开二级菜单（.direction/.crop）或收起（.none）。独立 popover 架构：底条位置不动，
    /// popover 窗口按锚点水平居中 + 避让选区方向显示/隐藏。菜单间直接切换时先隐藏旧浮层。
    /// 菜单开/关/切换同时收起 tooltip 并重置停顿（对齐 LongCaptureSetMenu 的 TooltipCancel）。
    private func openMenu(_ kind: LCMenuKindEquatable) {
        guard let session = session else { return }
        guard menuKind != kind else { return }
        let oldKind = menuKind
        menuKind = kind
        menuHover = -1
        hideTip()
        tipTarget = -1
        if kind == .none {
            popoverWindow?.orderOut(nil)
            popoverRect = .null
            barView?.needsDisplay = true
            return
        }
        // 展开方向避让选区（LongCaptureMenuOpenBelow）：底条在选区下方 → 向下展开；
        // 在上方 → 向上；重叠兜底形态选屏幕空余较大的一侧；仅当远离侧放不下才翻转
        let rows = menuRows()
        let pw = LC_POP_PAD * 2 + CGFloat(rows) * LC_POP_CELL + CGFloat(max(rows - 1, 0)) * LC_POP_CELL_GAP
        let ph = LC_POP_H
        var below: Bool
        let sel = session.selection
        if barRect.minY >= sel.maxY - 2 { below = true }
        else if barRect.maxY <= sel.minY + 2 { below = false }
        else { below = (barRect.minY - session.virtualBounds.minY)
                < (session.virtualBounds.maxY - barRect.maxY) }
        let mon = scMonitorBounds(for: barRect) ?? session.virtualBounds
        if below && barRect.maxY + ph + LC_MENU_GAP > mon.maxY { below = false }
        else if !below && barRect.minY - ph - LC_MENU_GAP < mon.minY { below = true }
        menuBelow = below
        // 水平居中对齐锚点按钮并夹在底条范围内
        let rects = layoutItemRects()
        let anchorLocal = rects[menuAnchorItem]
        let anchorCenterX = barRect.minX + anchorLocal.midX
        var px = anchorCenterX - pw / 2
        if px < barRect.minX { px = barRect.minX }
        if px + pw > barRect.maxX { px = barRect.maxX - pw }
        let py = below ? barRect.maxY + LC_MENU_GAP : barRect.minY - LC_MENU_GAP - ph
        popoverRect = CGRect(x: px, y: py, width: pw, height: ph)
        if popoverWindow == nil {
            let win = ScreenshotPanelWindow(
                contentRect: lcNSRect(fromCG: popoverRect), styleMask: .borderless,
                backing: .buffered, defer: false)
            win.level = NSWindow.Level(rawValue: NSWindow.Level.screenSaver.rawValue + 4)
            win.isOpaque = false
            win.backgroundColor = .clear
            win.hasShadow = false
            win.collectionBehavior = [.canJoinAllSpaces, .fullScreenAuxiliary]
            win.isReleasedWhenClosed = false
            let view = ScreenshotLCPopoverView(
                controller: self, frame: NSRect(origin: .zero, size: popoverRect.size))
            win.contentView = view
            popoverWindow = win
            popoverView = view
        } else {
            popoverWindow?.setFrame(lcNSRect(fromCG: popoverRect), display: true)
            popoverView?.frame = NSRect(origin: .zero, size: popoverRect.size)
        }
        _ = oldKind
        popoverWindow?.orderFrontRegardless()
        barView?.needsDisplay = true
    }

    // MARK: UI 维护节拍（100ms；LongCaptureToolbarUiTick 逐段移植）

    /// UI 维护节拍（由长截图会话泵循环每 100ms 调用）：
    /// 1) popover 展开时检测「窗口外左键按下」并关闭（底条∪popover 区域外的透底不算）；
    /// 2) 悬停意图：方向/裁剪锚点按钮停留 300ms 后展开（另一菜单已展开时直接切换；扫过
    ///    不误触）；离开「锚点∪popover」超过 250ms 后收起（宽限期足够跨过透底间隙）；
    /// 3) title 式 tooltip：悬停目标稳定 500ms 后显示（目标切换即重置停顿）。
    func uiTick() {
        guard session != nil, window != nil else { return }
        let now = lcNowMs()
        let mouse = lcCurrentMouseCG()
        // 全局左键按下沿检测（GetAsyncKeyState(VK_LBUTTON) 等价；NOACTIVATE 弹窗无焦点只能轮询）
        let down = NSEvent.pressedMouseButtons & 1 != 0
        let pressed = down && !lDown
        lDown = down

        let hv = hitTestBar(mouse)
        let cell = menuKind != .none ? hitTestPopover(mouse) : -1
        // 光标是否在可见区域内（底条 ∪ popover；其余区域透底不算窗内）
        let inside = scPointInRect(mouse, barRect)
            || (menuRows() > 0 && scPointInRect(mouse, popoverRect))
        if pressed && menuKind != .none && !inside {
            openMenu(.none)
            return
        }

        // hover 高亮同步（轮询驱动；拖拽中冻结）
        if !dragging {
            let newHover = down ? -1 : hv
            if newHover != hoverItem {
                hoverItem = newHover
                barView?.needsDisplay = true
            }
        }
        // popover cell hover 同步
        if menuKind != .none {
            let newMenuHover = down ? -1 : cell
            if newMenuHover != menuHover {
                menuHover = newMenuHover
                popoverView?.needsDisplay = true
            }
        }

        // —— 二级 popover：悬停展开 / 悬停切换 / 离开收起 ——
        // 点击收起过的锚点在光标移出该按钮前不再因悬停重开（popHoverDisarm）
        let hoverKind = hoverMenuKind(hv)
        if popHoverDisarm != .none && hv != menuAnchorDisarmItem() {
            popHoverDisarm = .none   // 离开被解除武装的锚点按钮即恢复悬停展开
        }
        if hoverKind != .none && hoverKind != menuKind && hoverKind != popHoverDisarm {
            if popHoverSinceMs == 0 { popHoverSinceMs = now }
            if now - popHoverSinceMs >= LC_POP_OPEN_DWELL_MS {
                openMenu(hoverKind)
                popHoverSinceMs = 0
            }
        } else {
            popHoverSinceMs = 0
        }
        if menuKind != .none {
            // 「使用中」判定：popover cell、当前锚点，或正悬停准备切换的另一锚点，都不算离开
            let usingPop = cell >= 0 || hv == menuAnchorItem || hoverKind != .none
            if usingPop {
                popLeaveSinceMs = 0
            } else {
                if popLeaveSinceMs == 0 { popLeaveSinceMs = now }
                if now - popLeaveSinceMs >= LC_POP_CLOSE_GRACE_MS {
                    openMenu(.none)
                    popLeaveSinceMs = 0
                }
            }
        } else {
            popLeaveSinceMs = 0
        }

        // —— title 式 tooltip：底条按钮（无菜单时）或 popover 图标 cell ——
        var target = -1
        if menuKind == .none && hv >= 0 && hv != LCItem.size.rawValue {
            target = hv
        } else if menuKind != .none && cell >= 0 {
            target = 100 + menuKindIndex * 10 + cell
        }
        if target != tipTarget {
            tipTarget = target
            tipSinceMs = now
            hideTip()
        } else if tipTarget >= 0 && !tipShown && now - tipSinceMs >= LC_TIP_DELAY_MS {
            let text: String?
            if tipTarget >= 100 {
                text = menuRowLabel(cell)
            } else {
                text = barItemTip(tipTarget)
            }
            if let text = text, !text.isEmpty {
                showTip(text, target: target)
            }
        }
        // 光标接管：把手格四向箭头提示可拖动（拖拽中 closedHand），其余箭头
        if dragging {
            NSCursor.closedHand.set()
        } else if scPointInRect(mouse, barRect), let rects = Optional(layoutItemRects()) {
            let local = CGPoint(x: mouse.x - barRect.minX, y: mouse.y - barRect.minY)
            if scPointInRect(local, rects[LCItem.grip.rawValue]) {
                NSCursor.openHand.set()
            } else {
                NSCursor.arrow.set()
            }
        } else if !tipShown {
            NSCursor.arrow.set()
        }
    }

    /// 解除武装的锚点项目码（popHoverDisarm 对应的 LCItem.rawValue；none 返回 -1 恒不匹配）。
    private func menuAnchorDisarmItem() -> Int {
        switch popHoverDisarm {
        case .crop: return LCItem.crop.rawValue
        case .direction: return LCItem.direction.rawValue
        case .none: return -1
        }
    }

    /// menuKind 的数值编码（tooltip 目标编码用；对齐 Windows 的 menuKind*10 段）。
    private var menuKindIndex: Int {
        switch menuKind {
        case .direction: return 1
        case .crop: return 2
        case .none: return 0
        }
    }

    // MARK: tooltip（LongCaptureTooltipShow/Hide 移植）

    /// 显示 tooltip：按文本测量定尺寸，锚定目标矩形——优先上方，放不下转下方；水平居中
    /// 并夹在虚拟屏幕内。窗口懒创建，复用至工具栏销毁。
    private func showTip(_ text: String, target: Int) {
        guard let session = session else { return }
        let padX = SC.tipPadX
        let padY = SC.tipPadY
        let attr = NSAttributedString(string: text, attributes: [
            .font: NSFont.systemFont(ofSize: SC.fontPx),
        ])
        let textSize = attr.size()
        let w = ceil(textSize.width) + padX * 2 + 2   // +2 抗锯齿边缘余量，杜绝触发截断
        let h = ceil(textSize.height) + padY * 2 + 2
        // 锚点 = 目标（底条 item / popover cell）在 CG 全局坐标中的矩形
        let anchor: CGRect
        if target >= 100 {
            let cellIdx = (target - 100) % 10
            let local = popoverCellRect(cellIdx)
            anchor = local.offsetBy(dx: popoverRect.minX, dy: popoverRect.minY)
        } else {
            let rects = layoutItemRects()
            anchor = rects[target].offsetBy(dx: barRect.minX, dy: barRect.minY)
        }
        let virtual = session.virtualBounds
        var x = anchor.midX - w / 2
        if x < SC.tipEdgeClamp { x = SC.tipEdgeClamp }
        if x + w > virtual.maxX - SC.tipEdgeClamp { x = virtual.maxX - SC.tipEdgeClamp - w }
        var y = anchor.minY - SC.tipGap - h
        if y < virtual.minY + SC.tipEdgeClamp { y = anchor.maxY + SC.tipGap }
        if tipWindow == nil {
            let win = ScreenshotPanelWindow(
                contentRect: NSRect(origin: .zero, size: CGSize(width: w, height: h)),
                styleMask: .borderless, backing: .buffered, defer: false)
            win.level = NSWindow.Level(rawValue: NSWindow.Level.screenSaver.rawValue + 5)
            win.isOpaque = false
            win.backgroundColor = .clear
            win.hasShadow = false
            win.collectionBehavior = [.canJoinAllSpaces, .fullScreenAuxiliary]
            win.isReleasedWhenClosed = false
            tipWindow = win
        }
        tipWindow?.contentView = ScreenshotLCTipView(
            text: text, frame: NSRect(origin: .zero, size: CGSize(width: w, height: h)))
        tipWindow?.setFrame(lcNSRect(fromCG: CGRect(x: x, y: y, width: w, height: h)), display: true)
        tipWindow?.orderFrontRegardless()
        tipShown = true
    }

    /// 收起 tooltip 并清零停顿（目标切换/点击/菜单开合时调用）。
    private func hideTip() {
        guard tipShown else { return }
        tipShown = false
        tipWindow?.orderOut(nil)
    }

    /// 底条按钮的 tooltip 文案（LongCaptureToolbarItemTip 移植，全中文照搬；nil = 无）。
    private func barItemTip(_ item: Int) -> String? {
        guard let session = session else { return nil }
        switch LCItem(rawValue: item) {
        case .grip: return "拖动工具栏"
        case .direction:
            return session.frameCount > 1 ? "滚动方向（已拼接多帧后锁定）" : "滚动方向"
        case .autoScroll: return "自动滚动"
        case .crop: return "裁剪"
        case .save: return "保存到本地"
        case .cancel: return "取消"
        case .finish: return "完成并复制"
        default: return nil
        }
    }

    /// popover cell 的 tooltip 文案（LongCaptureMenuRowLabel 移植）：方向 = 纵向/横向；
    /// 裁剪 = 丢弃上方(纵向)/左侧(横向)、丢弃下方/右侧、重置。
    private func menuRowLabel(_ row: Int) -> String {
        guard let session = session else { return "" }
        if menuKind == .direction {
            return row == 0 ? "纵向" : "横向"
        }
        if session.isCropped && row == menuRows() - 1 { return "重置裁剪" }
        if session.horizontal {
            return row == 0 ? "丢弃选区左侧内容" : "丢弃选区右侧内容"
        }
        return row == 0 ? "丢弃选区上方内容" : "丢弃选区下方内容"
    }

    // MARK: 鼠标事件（底条 / popover）

    /// 底条左键按下：记录按下目标（UP 必须命中同一目标才触发动作——关键防误触：编辑
    /// 工具栏「长截图」按钮按下瞬间进入长截图，本工具栏立即在附近生成，残留的松开事件
    /// 绝不能触发按钮）；把手进入拖拽（收起菜单后计算抓取偏移）。
    func handleBarMouseDown(_ point: CGPoint) {
        guard session != nil, !barRect.isNull else { return }
        hideTip()
        tipTarget = -1
        if hitTestBar(point) == LCItem.grip.rawValue {
            if menuKind != .none { openMenu(.none) }
            dragGrabDX = point.x - barRect.minX
            dragGrabDY = point.y - barRect.minY
            dragging = true
            pressItem = -1
            pressMenuRow = -1
            NSCursor.closedHand.set()
            return
        }
        pressItem = hitTestBar(point)
        pressMenuRow = -1
    }

    /// 把手拖拽中：跟随鼠标平移工具栏窗口并钳制在虚拟屏幕内（对齐 WM_MOUSEMOVE 拖拽分支）。
    func handleBarMouseDragged(_ point: CGPoint) {
        guard dragging, !barRect.isNull, let session = session else { return }
        var nx = point.x - dragGrabDX
        var ny = point.y - dragGrabDY
        let minX = session.virtualBounds.minX + 4
        let maxX = max(minX, session.virtualBounds.maxX - 4 - barRect.width)
        let minY = session.virtualBounds.minY
        let maxY = max(minY, session.virtualBounds.maxY - barRect.height)
        if nx < minX { nx = minX }
        if nx > maxX { nx = maxX }
        if ny < minY { ny = minY }
        if ny > maxY { ny = maxY }
        if nx != barRect.minX || ny != barRect.minY {
            barRect = CGRect(x: nx, y: ny, width: barRect.width, height: barRect.height)
            window?.setFrame(lcNSRect(fromCG: barRect), display: true)
        }
    }

    /// 底条左键抬起：把手拖拽结束；按钮点击要求「按下-抬起同目标」（不匹配直接吞掉）。
    func handleBarMouseUp(_ point: CGPoint) {
        guard session != nil else { return }
        if dragging {
            dragging = false
            pressItem = -1
            pressMenuRow = -1
            NSCursor.openHand.set()
            return
        }
        hideTip()   // 任何点击立即收起 tooltip 并清零停顿（网页 title 同款）
        tipTarget = -1
        let hit = hitTestBar(point)
        let sameTarget = hit >= 0 && hit == pressItem && pressMenuRow < 0
        pressItem = -1
        pressMenuRow = -1
        guard sameTarget, let session = session else { return }
        // 点击任一直接动作按钮时收起展开中的菜单（方向/裁剪按钮自身负责切换菜单状态）
        if hit != LCItem.direction.rawValue && hit != LCItem.crop.rawValue && menuKind != .none {
            openMenu(.none)
        }
        switch LCItem(rawValue: hit) {
        case .finish:
            session.requestFinish()
        case .cancel:
            session.requestCancel()
        case .save:
            session.requestSave()
        case .direction:
            // 点击方向：与裁剪同款开合（悬停展开见 uiTick）。已拼接多帧后方向锁定，
            // 点击不展开菜单
            if session.frameCount <= 1 {
                if menuKind == .direction {
                    openMenu(.none)
                    popHoverDisarm = .direction
                } else {
                    openMenu(.direction)
                }
            }
        case .autoScroll:
            session.setAutoScroll(!session.autoScroll)
        case .crop:
            // 点击裁剪：未展开则立即展开；已展开则收起并解除悬停武装（需移出按钮再进入
            // 才会因悬停重开，防止点击收起与悬停展开互相打架）
            if menuKind == .crop {
                openMenu(.none)
                popHoverDisarm = .crop
            } else {
                openMenu(.crop)
            }
        default:
            break
        }
    }

    /// popover 左键按下：裁剪 cell → 立即应用对应项（延迟剔除登记）；方向 cell → 可用时
    /// 切换方向（禁用 cell 仅收起）；随后收起菜单（对齐 WM_LBUTTONUP 的 menuRow 分支）。
    func handlePopoverMouseDown(_ point: CGPoint) {
        guard session != nil else { return }
        hideTip()
        tipTarget = -1
        let row = hitTestPopover(point)
        guard row >= 0, let session = session else { return }
        if menuKind == .crop {
            applyCropPopover(row: row)
        } else if menuKind == .direction {
            let wantHorizontal = (row == 1)
            if wantHorizontal != session.horizontal && menuRowEnabled(row) {
                session.switchDirection()
            }
        }
        openMenu(.none)
    }

    /// popover 行可用性（LongCaptureMenuRowEnabled 移植）：方向菜单中「非当前方向」的行
    /// 在已拼接多帧后禁用；裁剪行始终可用（只收紧输出行窗口，不碰拼接/匹配状态）。
    private func menuRowEnabled(_ row: Int) -> Bool {
        guard let session = session else { return false }
        if menuKind == .direction {
            let current = session.horizontal ? 1 : 0
            return row == current || session.frameCount <= 1
        }
        return true
    }

    /// 应用裁剪 popover 选项（会话转调 lc_apply_crop；行号语义见 ScreenshotLongCaptureMac
    /// 的 applyCrop —— 登记待剔除区间，物理删行由下次朝该方向成功提交触发）。
    private func applyCropPopover(row: Int) {
        guard let session = session else { return }
        // 行号 0/1 = 丢弃上方/下方（横向 = 左侧/右侧）；已裁剪时末行 = 重置
        let cropRow: Int32
        if session.isCropped && row == menuRows() - 1 {
            cropRow = 2   // LC_CROP_RESET
        } else {
            cropRow = row == 0 ? 0 : 1   // LC_CROP_DISCARD_TOP / LC_CROP_DISCARD_BOTTOM
        }
        session.applyCrop(row: cropRow)
    }

    // MARK: 绘制（LongCaptureToolbarRender 移植）

    /// 底条按钮图标（含方向/自动滚动的 V/H 变体；对齐渲染段的图标选择）。
    private func barIcon(_ item: LCItem) -> SCToolbarIcon? {
        guard let session = session else { return nil }
        switch item {
        case .direction: return session.horizontal ? .directionH : .directionV
        case .autoScroll: return session.horizontal ? .autoScrollH : .autoScrollV
        case .crop: return .cropIcon
        case .save: return .save
        case .cancel: return .cancel
        case .finish: return .confirm
        default: return nil
        }
    }

    /// 绘制底条（白底圆角条 + 浅灰描边 + 分隔线 + 尺寸标签 + 把手 6 点 + 图标三态）。
    /// - Parameter ctx: 底条视图 CG 上下文（已翻转，本地坐标 = 底条本地）
    func drawBar(_ ctx: CGContext) {
        guard let session = session else { return }
        let w = barRect.width
        let h = LC_BAR_H
        let rects = layoutItemRects()
        // 1) 白色圆角背景 + 1px 浅灰边框（圆角外透明透出桌面）
        let bgPath = lcRoundedRectPath(CGRect(x: 0, y: 0, width: w, height: h), LC_BAR_RADIUS)
        ctx.addPath(bgPath)
        ctx.setFillColor(NSColor.white.cgColor)
        ctx.fillPath()
        ctx.addPath(bgPath)
        ctx.setStrokeColor(lcBorderCol.cgColor)
        ctx.setLineWidth(1)
        ctx.strokePath()
        // 2) 分隔线（1px 竖线，上下各缩进 10）
        ctx.setStrokeColor(lcSepCol.cgColor)
        ctx.setLineWidth(1)
        for sep in [LCItem.sep1, .sep3] {
            let r = rects[sep.rawValue]
            let x = r.midX + 0.5   // 半像素偏移使整数坐标下恰好落在单像素列
            ctx.move(to: CGPoint(x: x, y: r.minY))
            ctx.addLine(to: CGPoint(x: x, y: r.maxY))
            ctx.strokePath()
        }
        // 3) 尺寸标签（预览宽×高，随拼接/裁剪实时变化；超宽省略号兜底）
        drawSizeLabel(ctx, rect: rects[LCItem.size.rawValue])
        // 4) 把手（6 点；hover/拖拽中铺浅蓝圆角底、圆点转主题蓝）
        drawGrip(ctx, rect: rects[LCItem.grip.rawValue],
                 hot: dragging || hoverItem == LCItem.grip.rawValue)
        // 5) 图标按钮三态
        let dirLocked = session.frameCount > 1   // 已拼接多帧：方向锁定
        drawIconButton(ctx, rect: rects[LCItem.direction.rawValue], icon: barIcon(.direction),
                       hover: hoverItem == LCItem.direction.rawValue || menuKind == .direction,
                       active: false, disabled: dirLocked)
        drawIconButton(ctx, rect: rects[LCItem.autoScroll.rawValue], icon: barIcon(.autoScroll),
                       hover: hoverItem == LCItem.autoScroll.rawValue,
                       active: session.autoScroll, disabled: false)
        drawIconButton(ctx, rect: rects[LCItem.crop.rawValue], icon: barIcon(.crop),
                       hover: hoverItem == LCItem.crop.rawValue || menuKind == .crop,
                       active: session.isCropped,   // badge 高亮：已裁剪（含待剔除区间）
                       disabled: false)
        drawIconButton(ctx, rect: rects[LCItem.save.rawValue], icon: barIcon(.save),
                       hover: hoverItem == LCItem.save.rawValue, active: false, disabled: false)
        drawIconButton(ctx, rect: rects[LCItem.cancel.rawValue], icon: barIcon(.cancel),
                       hover: hoverItem == LCItem.cancel.rawValue, active: false, disabled: false)
        drawIconButton(ctx, rect: rects[LCItem.finish.rawValue], icon: barIcon(.finish),
                       hover: hoverItem == LCItem.finish.rawValue, active: false, disabled: false)
    }

    /// 绘制尺寸标签文本（12px 系统字体，水平/垂直居中；对齐 LongCaptureDrawSurfaceText）。
    private func drawSizeLabel(_ ctx: CGContext, rect: CGRect) {
        guard let text = session?.sizeLabelText, !text.isEmpty else { return }
        let attr = NSAttributedString(string: text, attributes: [
            .font: NSFont.systemFont(ofSize: SC.fontPx),
            .foregroundColor: lcSizeLabelCol,
        ])
        let size = attr.size()
        // 超宽截断兜底（固定槽位 104pt）
        let maxW = rect.width
        let drawText = size.width > maxW ? truncated(text, width: maxW, attr: attr) : text
        drawText.draw(
            with: CGRect(x: rect.minX, y: rect.midY - size.height / 2,
                         width: maxW, height: ceil(size.height)),
            options: [.usesLineFragmentOrigin], attributes: [:])
    }

    /// 超宽文本按宽度截断（尺寸标签兜底；逐字符累计宽度，末尾补省略号）。
    private func truncated(_ text: String, width: CGFloat, attr: NSAttributedString) -> String {
        let font = NSFont.systemFont(ofSize: SC.fontPx)
        var result = text
        while result.count > 1 {
            let candidate = result + "…"
            let w = (candidate as NSString).size(withAttributes: [.font: font]).width
            if w <= width { return candidate }
            result = String(result.dropLast())
        }
        return "…"
    }

    /// 绘制把手「6 点拖拽」：2 列 × 3 排共 6 个小圆点居中；圆点几何随单元格尺寸缩放
    /// （列距 ±11%、行距 0/±16%、点半径 ~5%，对齐 LongCaptureDrawGrip）。
    private func drawGrip(_ ctx: CGContext, rect: CGRect, hot: Bool) {
        if hot {
            let path = lcRoundedRectPath(rect, 6)
            ctx.addPath(path)
            ctx.setFillColor(lcHoverBg.cgColor)
            ctx.fillPath()
        }
        let cw = rect.width
        let cx = rect.midX
        let cy = rect.midY
        let colGap = cw * 0.11
        let rowGap = cw * 0.16
        let r = max(1.2, cw * 0.05)
        ctx.setFillColor(hot ? lcIconBlue.cgColor : lcGripDotCol.cgColor)
        for row in -1...1 {
            for col in [-1, 1] {
                let dx = cx + CGFloat(col) * colGap
                let dy = cy + CGFloat(row) * rowGap
                ctx.fillEllipse(in: CGRect(x: dx - r, y: dy - r, width: r * 2, height: r * 2))
            }
        }
    }

    /// 绘制图标按钮三态（LongCaptureDrawIconButton 移植）：hover/active 浅蓝圆角底 +
    /// 蓝色图标，disabled 灰图标，常态深灰图标。
    private func drawIconButton(_ ctx: CGContext, rect: CGRect, icon: SCToolbarIcon?,
                                hover: Bool, active: Bool, disabled: Bool) {
        if !disabled && (hover || active) {
            let path = lcRoundedRectPath(rect, 6)
            ctx.addPath(path)
            ctx.setFillColor((active ? lcActiveBg : lcHoverBg).cgColor)
            ctx.fillPath()
        }
        let color = disabled ? lcIconGray : (hover || active) ? lcIconBlue : lcIconDark
        guard let image = icon?.tinted(color) else { return }
        let size = SCToolbarIcon.iconPointSize
        image.draw(in: NSRect(x: rect.midX - size / 2, y: rect.midY - size / 2,
                              width: size, height: size))
    }

    /// popover 第 i 个 cell 的图标（LongCaptureMenuCellIcon 移植）：方向 = 纵向/横向变体；
    /// 裁剪 = 丢弃起点/终点（纵向 = 上方/下方，横向 = 左侧/右侧），已裁剪时末位追加重置。
    private func popoverCellIcon(_ i: Int) -> SCToolbarIcon? {
        guard let session = session else { return nil }
        if menuKind == .direction { return i == 0 ? .directionV : .directionH }
        if session.isCropped && i == menuRows() - 1 { return .cropReset }
        if session.horizontal { return i == 0 ? .cropDiscardLeft : .cropDiscardRight }
        return i == 0 ? .cropDiscardTop : .cropDiscardBottom
    }

    /// 绘制二级菜单 popover（白色圆角底 + 浅灰描边 + 单行图标 cell；当前方向 active 蓝底、
    /// 锁定方向灰显；裁剪 cell 始终可用）。
    func drawPopover(_ ctx: CGContext) {
        let rows = menuRows()
        guard rows > 0, let session = session else { return }
        let pw = popoverRect.width
        let ph = popoverRect.height
        let bgPath = lcRoundedRectPath(CGRect(x: 0, y: 0, width: pw, height: ph), LC_BAR_RADIUS)
        ctx.addPath(bgPath)
        ctx.setFillColor(NSColor.white.cgColor)
        ctx.fillPath()
        ctx.addPath(bgPath)
        ctx.setStrokeColor(lcBorderCol.cgColor)
        ctx.setLineWidth(1)
        ctx.strokePath()
        let curDir = session.horizontal ? 1 : 0
        for i in 0..<rows {
            let enabled = menuRowEnabled(i)
            let active = menuKind == .direction && i == curDir
            let rect = popoverCellRect(i)
            if enabled && (i == menuHover || active) {
                let path = lcRoundedRectPath(rect, 6)
                ctx.addPath(path)
                ctx.setFillColor((active ? lcActiveBg : lcHoverBg).cgColor)
                ctx.fillPath()
            }
            let color = enabled ? ((i == menuHover || active) ? lcIconBlue : lcIconDark) : lcIconGray
            guard let image = popoverCellIcon(i)?.tinted(color) else { continue }
            let size = SCToolbarIcon.iconPointSize
            image.draw(in: NSRect(x: rect.midX - size / 2, y: rect.midY - size / 2,
                                  width: size, height: size))
        }
    }
}
