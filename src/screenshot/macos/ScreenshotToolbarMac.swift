import Foundation
import AppKit

// MARK: - 工具栏 / 子菜单 / tooltip（macOS）
//
// Windows 版把工具栏/子菜单/tooltip 画进覆盖层 backDC（overlay_ui_windows.cpp）；macOS
// 覆盖层是每屏一个的独立 NSWindow（无法跨窗口绘制），故改为三个独立无边框面板窗口：
// - 工具栏窗口：白底圆角 16 格按钮条 + 最左 6 点拖拽把手（toolbarPlaced 语义）
// - 子菜单窗口：矢量 = [粗细×3]｜[颜色×8]；文字 = [字号×3]｜[颜色×8]；
//   马赛克 = [涂抹|框选]｜[块大小×3]｜[涂抹半径×3]（单行，按来源工具取布局）
// - tooltip 窗口：悬停 500ms 深色圆角气泡（pumpTick 轮询驱动，对齐 TickToolbarTooltip）
//
// 窗口规格（面板族）：borderless、canJoinAllSpaces/fullScreenAuxiliary、
// canBecomeKey=false（点击仍可接收 mouseDown，但不抢键盘焦点——键盘仍归覆盖层视图）。
// 层级比覆盖层高 1~3 级：覆盖层窗口点击会抬到同层最前，浮层若同层会被整屏覆盖层
// 视图盖住（覆盖层窗口铺满每块屏幕），故工具栏/子菜单/tooltip 依次抬高。
//
// 坐标系：内部全部使用 CG 全局逻辑坐标，窗口定位时翻转为 NS 坐标。
// 工具栏/子菜单窗口视图均 isFlipped，本地 (0,0) 即各自矩形左上角。

// MARK: - 常量（Windows 出处集中标注）

extension SC {
    // ---- 工具栏几何（internal.h: SC_TOOLBAR_*）----
    /// 按钮尺寸（正方形；SC_TOOLBAR_BTN = 32）
    static let toolbarBtn: CGFloat = 32
    /// 按钮↔工具栏边缘内边距（四边一致；SC_TOOLBAR_PAD = 6）
    static let toolbarPad: CGFloat = 6
    /// 工具栏高度 = 按钮 + 上下内边距（SC_TOOLBAR_H = 44）
    static let toolbarH: CGFloat = toolbarBtn + toolbarPad * 2
    /// 按钮间距（SC_TOOLBAR_GAP = 1）
    static let toolbarGap: CGFloat = 1
    /// 工具栏圆角（SC_TOOLBAR_RADIUS = 8）
    static let toolbarRadius: CGFloat = 8
    /// 选区到工具栏间距（SC_TOOLBAR_MARGIN = 6）
    static let toolbarMargin: CGFloat = 6
    /// 工具栏边框（SC_TOOLBAR_BORDER = 1）
    static let toolbarBorderW: CGFloat = 1
    /// 图标绘制逻辑边长（CalcToolbarMetrics.iconSize = btn - 8 + 2 = 26）
    static let toolbarIconSize: CGFloat = 26

    // ---- 工具栏主题色（internal.h 主题色常量组 + icons/overlay_ui 各绘制点）----
    /// 工具栏选中态图标蓝（SC_THEME_TOOLBAR_BLUE = #3B8BF2，配浅蓝高亮底）
    static let toolbarBlue = NSColor(srgbRed: 0x3B / 255.0, green: 0x8B / 255.0, blue: 0xF2 / 255.0, alpha: 1.0)
    /// 图标普通态深灰（icons_windows.cpp darkColor = RGB(60,60,60)）
    static let iconDark = NSColor(srgbRed: 60.0 / 255.0, green: 60.0 / 255.0, blue: 60.0 / 255.0, alpha: 1.0)
    /// 选中态浅蓝高亮底（SC_THEME_SEL_BG = RGB(225,237,253)，主题蓝叠白底 ~15% 预混合）
    static let toolbarSelBg = NSColor(srgbRed: 225.0 / 255.0, green: 237.0 / 255.0, blue: 253.0 / 255.0, alpha: 1.0)
    /// hover 态极浅蓝底（SC_THEME_HOVER_BG = RGB(235,243,255)）
    static let toolbarHoverBg = NSColor(srgbRed: 235.0 / 255.0, green: 243.0 / 255.0, blue: 255.0 / 255.0, alpha: 1.0)
    /// 工具栏/子菜单 1px 浅灰边框（DrawToolbar/DrawPopup borderPen = RGB(210,210,210)）
    static let toolbarBorderCol = NSColor(srgbRed: 210.0 / 255.0, green: 210.0 / 255.0, blue: 210.0 / 255.0, alpha: 1.0)
    /// 工具栏分隔线（gdi.toolbarSepPen = RGB(230,230,230)）
    static let toolbarSepCol = NSColor(srgbRed: 230.0 / 255.0, green: 230.0 / 255.0, blue: 230.0 / 255.0, alpha: 1.0)
    /// 把手 6 点圆点（DrawToolbarGrip dotBrush = RGB(165,165,165)）
    static let gripDotCol = NSColor(srgbRed: 165.0 / 255.0, green: 165.0 / 255.0, blue: 165.0 / 255.0, alpha: 1.0)

    // ---- 子菜单几何（internal.h: SC_POPUP_*；单元格 = 工具栏按钮便于视觉对齐）----
    /// 单元格尺寸（SC_POPUP_CELL = SC_TOOLBAR_BTN = 32）
    static let popupCell: CGFloat = 32
    /// 内边距（SC_POPUP_PAD = 4）
    static let popupPad: CGFloat = 4
    /// 圆角（SC_POPUP_RADIUS = 8）
    static let popupRadius: CGFloat = 8
    /// 颜色圆点直径（SC_POPUP_COLOR_DOT = 18）
    static let popupColorDot: CGFloat = 18
    /// 分隔线两侧间距（SC_POPUP_SEP_GAP = 6）
    static let popupSepGap: CGFloat = 6
    /// 分隔线高度（SC_POPUP_SEP_H = 20）
    static let popupSepH: CGFloat = 20
    /// 边框（SC_POPUP_BORDER = 1）
    static let popupBorderW: CGFloat = 1
    /// 工具栏与子菜单间距（SC_POPUP_MARGIN = 4）
    static let popupMargin: CGFloat = 4
    /// 子菜单图标默认深灰（SC_THEME_ICON_DARK = #333333，与预设「黑」同值）
    static let popupIconDark = NSColor(srgbRed: 0x33 / 255.0, green: 0x33 / 255.0, blue: 0x33 / 255.0, alpha: 1.0)
    /// 分隔线颜色（DrawPopup sepPen = RGB(220,220,220)）
    static let popupSepCol = NSColor(srgbRed: 220.0 / 255.0, green: 220.0 / 255.0, blue: 220.0 / 255.0, alpha: 1.0)

    // ---- 工具栏 tooltip（overlay_ui_windows.cpp: SC_TIP_*）----
    /// 悬停多久后显示（SC_TIP_DELAY_MS = 500，网页 title 同款节奏）
    static let tipDelaySec: TimeInterval = 0.5
    /// 气泡与锚点按钮的间距（SC_TIP_GAP = 6）
    static let tipGap: CGFloat = 6
    /// 气泡水平内边距（SC_TIP_PAD_X = 8）
    static let tipPadX: CGFloat = 8
    /// 气泡垂直内边距（SC_TIP_PAD_Y = 5）
    static let tipPadY: CGFloat = 5
    /// 气泡圆角半径（SC_TIP_RADIUS = 4）
    static let tipRadius: CGFloat = 4
    /// 气泡深色底（DrawToolbarTooltip bg = RGB(41,41,41)）
    static let tipBg = NSColor(srgbRed: 41.0 / 255.0, green: 41.0 / 255.0, blue: 41.0 / 255.0, alpha: 1.0)
    /// 气泡定位的屏幕边距（TickToolbarTooltip 内 4px 钳制基准）
    static let tipEdgeClamp: CGFloat = 4
}

// MARK: - 工具栏按钮枚举（对齐 Windows internal.h ToolButton）

/// 工具栏按钮（对齐 Windows internal.h 的 ToolButton 枚举 raw 值；最左「6 点拖拽把手」
/// 不占枚举位，命中码用负值区分，对齐 SC_TB_GRIP = -2）。
enum ScreenshotToolButton: Int {
    case drag = 0        // TB_Drag 拖拽
    case rect            // TB_Rect 矩形
    case circle          // TB_Circle 圆形（含椭圆）
    case arrow           // TB_Arrow 箭头
    case brush           // TB_Brush 画笔
    case mosaic          // TB_Mosaic 马赛克
    case text            // TB_Text 文字
    case translate       // TB_Translate 翻译（占位，无点击处理，与 Windows 一致）
    case longCapture     // TB_LongCapture 长截图
    case separator1      // TB_Separator1 分隔线
    case undo            // TB_Undo 撤销
    case redo            // TB_Redo 重做
    case separator2      // TB_Separator2 分隔线
    case save            // TB_Save 保存到本地
    case cancel          // TB_Cancel 取消
    case confirm         // TB_Confirm 确定

    /// 按钮总数（internal.h: TB_Count）
    static let count = 16
    /// 「6 点拖拽把手」命中码（internal.h: SC_TB_GRIP = -2；取负值与按钮索引区分）
    static let gripHit = -2

    /// 是否为分隔线格（绘制竖线、无命中）。
    var isSeparator: Bool { self == .separator1 || self == .separator2 }

    /// 是否为可绘制矢量工具（对齐 IsVectorTool）。
    var isVectorTool: Bool {
        self == .rect || self == .circle || self == .arrow || self == .brush
    }

    /// 对应的标注类型（仅矢量工具有效；对齐 ToolToAnnotationType）。
    var annotationType: ScreenshotAnnotationType {
        switch self {
        case .rect: return .rect
        case .circle: return .circle
        case .arrow: return .arrow
        case .brush: return .brush
        default: return .rect
        }
    }
}

/// 标注类型 → 工具按钮（对齐 AnnotationTypeToTool 的逆映射；马赛克不可选中、
/// 无对应工具按钮返回 nil，等价 Windows 的 -1）。
func scAnnotationTypeToTool(_ t: ScreenshotAnnotationType) -> ScreenshotToolButton? {
    switch t {
    case .rect: return .rect
    case .circle: return .circle
    case .arrow: return .arrow
    case .brush: return .brush
    case .text: return .text
    case .mosaic: return nil
    }
}

extension ScreenshotToolButton {
    /// 按钮 → 图标枚举的显式映射（分隔线无图标返回 nil）。注意 ToolButton raw 值含
    /// 分隔线占位（9/12）而 SCToolbarIcon raw 值连续，二者不能直接 rawValue 互查。
    var toolbarIcon: SCToolbarIcon? {
        switch self {
        case .drag: return .drag
        case .rect: return .rect
        case .circle: return .circle
        case .arrow: return .arrow
        case .brush: return .brush
        case .mosaic: return .mosaic
        case .text: return .text
        case .translate: return .translate
        case .longCapture: return .longCapture
        case .undo: return .undo
        case .redo: return .redo
        case .save: return .save
        case .cancel: return .cancel
        case .confirm: return .confirm
        default: return nil   // 分隔线
        }
    }
}

// MARK: - 面板窗口与视图

/// 浮层面板窗口（工具栏/子菜单/tooltip 共用）：无边框、不可 key/main——点击仍可接收
/// mouseDown（AppKit 把点击派发给该窗口），但不抢键盘焦点（键盘仍归覆盖层视图）。
final class ScreenshotPanelWindow: NSWindow {
    override var canBecomeKey: Bool { return false }
    override var canBecomeMain: Bool { return false }
}

/// 工具栏自绘视图：把事件换算为 CG 全局坐标转发给控制器，绘制委托控制器完成。
final class ToolbarPanelView: NSView {
    unowned let controller: ScreenshotToolbarController

    init(controller: ScreenshotToolbarController, frame: NSRect) {
        self.controller = controller
        super.init(frame: frame)
    }

    required init?(coder: NSCoder) {
        fatalError("ToolbarPanelView is created programmatically only")
    }

    override var isFlipped: Bool { return true }   // 本地坐标与 CG 同向（左上原点、Y 向下）

    // 首击穿透：与 OverlayScreenshotView.acceptsFirstMouse 同因——App 未激活（协作式激活
    // 失败）时，非 key 浮层窗口的首次点击会被 AppKit 当"激活点击"吞掉，工具栏按钮
    // 第一次点按无响应；覆写后首击直达本视图（浮层自身不抢 key，仅放行鼠标事件）。
    override func acceptsFirstMouse(for event: NSEvent?) -> Bool { return true }

    override func draw(_ dirtyRect: NSRect) {
        guard let ctx = NSGraphicsContext.current?.cgContext else { return }
        controller.drawToolbar(ctx)
    }

    /// 本地坐标 → CG 全局坐标（视图 (0,0) = 工具栏矩形左上角）。
    private func cgPoint(from event: NSEvent) -> CGPoint {
        let local = convert(event.locationInWindow, from: nil)
        return CGPoint(x: (local.x + controller.toolbarRect.minX).rounded(),
                       y: (local.y + controller.toolbarRect.minY).rounded())
    }

    override func mouseDown(with event: NSEvent) {
        controller.handleToolbarMouseDown(cgPoint(from: event))
    }

    override func mouseDragged(with event: NSEvent) {
        controller.handleToolbarMouseDragged(cgPoint(from: event))
    }

    override func mouseUp(with event: NSEvent) {
        controller.handleToolbarMouseUp()
    }
}

/// 子菜单自绘视图：绘制委托控制器完成；点击换算 CG 坐标做命中码测试。
final class PopupPanelView: NSView {
    unowned let controller: ScreenshotToolbarController

    init(controller: ScreenshotToolbarController, frame: NSRect) {
        self.controller = controller
        super.init(frame: frame)
    }

    required init?(coder: NSCoder) {
        fatalError("PopupPanelView is created programmatically only")
    }

    override var isFlipped: Bool { return true }

    // 首击穿透：同 ToolbarPanelView——App 未激活时子菜单首次点击会被当"激活点击"吞掉。
    override func acceptsFirstMouse(for event: NSEvent?) -> Bool { return true }

    override func draw(_ dirtyRect: NSRect) {
        guard let ctx = NSGraphicsContext.current?.cgContext else { return }
        controller.drawPopup(ctx)
    }

    override func mouseDown(with event: NSEvent) {
        let local = convert(event.locationInWindow, from: nil)
        let cg = CGPoint(x: (local.x + controller.popupRect.minX).rounded(),
                         y: (local.y + controller.popupRect.minY).rounded())
        controller.handlePopupMouseDown(cg)
    }
}

/// tooltip 气泡自绘视图：深色圆角底 + 白色居中文本（对齐 DrawToolbarTooltip）。
final class TooltipPanelView: NSView {
    let text: String

    init(text: String, frame: NSRect) {
        self.text = text
        super.init(frame: frame)
    }

    required init?(coder: NSCoder) {
        fatalError("TooltipPanelView is created programmatically only")
    }

    override var isFlipped: Bool { return true }

    override func draw(_ dirtyRect: NSRect) {
        guard let ctx = NSGraphicsContext.current?.cgContext else { return }
        let path = CGPath(roundedRect: bounds, cornerWidth: SC.tipRadius,
                          cornerHeight: SC.tipRadius, transform: nil)
        ctx.addPath(path)
        ctx.setFillColor(SC.tipBg.cgColor)
        ctx.fillPath()

        // 白色居中文本（12px 系统字体，对齐 DrawToolbarTooltip 的 12*ds 字号）
        let attr = NSAttributedString(string: text, attributes: [
            .font: NSFont.systemFont(ofSize: SC.fontPx),
            .foregroundColor: NSColor.white,
        ])
        let size = attr.size()
        attr.draw(at: NSPoint(x: (bounds.width - ceil(size.width)) / 2,
                              y: (bounds.height - ceil(size.height)) / 2))
    }
}

// MARK: - 浮层位置计算（对齐 overlay_ui_windows.cpp Calc* 系列）

/// 取包含参考矩形的显示器 CG 边界（GetMonitorBoundsForRect 移植）：按相交面积取最大，
/// 无相交退化为包含参考矩形中心的屏。多屏异分辨率下工具栏/子菜单的上下翻转与左右
/// 钳制必须以此为界（整虚拟屏包络会被高分屏拉大，低分屏误判"下方放得下"）。
/// - Parameter rect: 参考矩形（CG 全局坐标）
/// - Returns: 显示器 CG 边界；查询失败返回 nil（调用方回退虚拟屏）
func scMonitorBounds(for rect: CGRect) -> CGRect? {
    var best: (CGRect, CGFloat)?
    for screen in NSScreen.screens {
        let cg = ScreenshotGeometry.cgFrame(of: screen)
        let inter = cg.intersection(rect)
        let area = inter.isNull ? 0 : inter.width * inter.height
        if area > 0 && (best == nil || area > best!.1) {
            best = (cg, area)
        }
    }
    if let b = best { return b.0 }
    let center = CGPoint(x: rect.midX, y: rect.midY)
    for screen in NSScreen.screens {
        let cg = ScreenshotGeometry.cgFrame(of: screen)
        if cg.contains(center) { return cg }
    }
    return nil
}

/// 计算工具栏位置（CalcToolbarPosition 移植）：选区下方 6px 居中 → 放不下上方 →
/// 再放不下选区内底边；垂直/水平边界取「选区所在显示器」而非整虚拟屏。
/// - Parameters:
///   - selection: 选区（CG 全局坐标）
///   - virtual: 虚拟屏并集（回退边界）
///   - monitor: 选区所在显示器边界（nil 时回退 virtual）
/// - Returns: 工具栏矩形（CG 全局坐标）
func scCalcToolbarRect(selection: CGRect, virtual: CGRect, monitor: CGRect?) -> CGRect {
    let tw = ScreenshotToolbarController.toolbarWidth
    let th = SC.toolbarH
    let margin = SC.toolbarMargin
    let bounds = monitor ?? virtual

    // 默认水平居中于选区，下方
    var x = selection.minX + (selection.width - tw) / 2
    var y = selection.maxY + margin
    // 下方放不下 -> 上方（选区触到所在显示器底边即触发）
    if y + th > bounds.maxY {
        y = selection.minY - margin - th
    }
    // 上方也放不下（选区纵向占满屏幕），贴近底部（选区内底边）
    if y < bounds.minY {
        y = selection.maxY - margin - th
        if y < selection.minY { y = selection.minY + margin }
        // 极端兜底：钳回显示器范围内（选区+工具栏高过屏幕等病态情形）
        if y + th > bounds.maxY { y = bounds.maxY - th }
        if y < bounds.minY { y = bounds.minY }
    }
    // 水平边界约束（同显示器内；跨屏时钳制到选区所在屏）
    if x + tw > bounds.maxX { x = bounds.maxX - tw - margin }
    if x < bounds.minX { x = bounds.minX + margin }
    return CGRect(x: x, y: y, width: tw, height: th)
}

/// 子菜单总尺寸（CalcPopupSize / CalcMosaicPopupSize 移植；按子菜单来源工具取布局）：
/// - 矢量/文字：单行 [粗细×3 或 字号×3]｜分隔线｜[颜色×8]
/// - 马赛克：单行 [涂抹|框选]｜分隔线｜[块大小×3]｜分隔线｜[涂抹半径×3]
/// 单元格间距沿用工具栏按钮间距，cell == SC_POPUP_CELL 时 PopupCellGap = 1。
func scCalcPopupSize(for tool: ScreenshotToolButton?) -> CGSize {
    let cellGap = SC.toolbarGap
    let sepW = SC.popupSepGap * 2 + 1   // 每组分隔线宽 = sepGap*2 + 1
    let contentW: CGFloat
    switch tool {
    case .mosaic:
        let modeW = CGFloat(SC.mosaicModeCount) * SC.popupCell + CGFloat(SC.mosaicModeCount - 1) * cellGap
        let sizeW = CGFloat(SC.mosaicSizes.count) * SC.popupCell + CGFloat(SC.mosaicSizes.count - 1) * cellGap
        let radiusW = CGFloat(SC.mosaicRadii.count) * SC.popupCell + CGFloat(SC.mosaicRadii.count - 1) * cellGap
        contentW = modeW + sepW + sizeW + sepW + radiusW
    default:
        let firstCount = (tool == .text) ? CGFloat(SC.fontSizes.count) : CGFloat(SC.thickPresets.count)
        let colorCount = CGFloat(SC.colorPresets.count)
        let firstW = firstCount * SC.popupCell + (firstCount - 1) * cellGap
        let colorW = colorCount * SC.popupCell + (colorCount - 1) * cellGap
        contentW = firstW + sepW + colorW
    }
    let w = contentW + SC.popupPad * 2 + SC.popupBorderW * 2
    let h = SC.popupCell + SC.popupPad * 2 + SC.popupBorderW * 2
    return CGSize(width: w, height: h)
}

/// 计算子菜单位置（CalcPopupPlacement 移植）：贴工具栏下方，放不下翻上方，
/// 左右钳制到工具栏所在显示器。尺寸按子菜单来源工具取布局。
/// - Parameters:
///   - toolbarRect: 工具栏矩形（CG 全局坐标）
///   - virtual: 虚拟屏并集（回退边界）
///   - monitor: 工具栏所在显示器边界（nil 时回退 virtual）
///   - tool: 子菜单来源工具（决定总尺寸）
/// - Returns: 子菜单矩形（CG 全局坐标）
func scCalcPopupRect(toolbarRect: CGRect, virtual: CGRect, monitor: CGRect?,
                     tool: ScreenshotToolButton?) -> CGRect {
    let size = scCalcPopupSize(for: tool)
    let margin = SC.popupMargin
    let bounds = monitor ?? virtual
    // 水平：与工具栏左对齐；垂直：优先工具栏下方
    var x = toolbarRect.minX
    var y = toolbarRect.maxY + margin
    if y + size.height > bounds.maxY {
        y = toolbarRect.minY - margin - size.height
    }
    if x + size.width > bounds.maxX { x = bounds.maxX - size.width - margin }
    if x < bounds.minX { x = bounds.minX + margin }
    return CGRect(origin: CGPoint(x: x, y: y), size: size)
}

// MARK: - 工具栏控制器

/// 工具栏/子菜单/tooltip 面板族控制器：持有三个独立 NSWindow 与全部 UI 状态，
/// 按钮动作回话给会话（ScreenshotOverlaySession）。生命周期挂会话 start/finish：
/// 首次进入确认态时创建窗口，finish 统一销毁。
/// 状态字段命名对齐 CaptureContext：toolbarRect/toolbarPlaced/toolbarDragging/
/// hoverToolbarBtn/tipBtn/tipShown/popupOpen/popupTool/popupRect。
final class ScreenshotToolbarController {
    /// 控制器可访问的会话（会话持有控制器，控制器反向 weak 防环）。
    private weak var session: ScreenshotOverlaySession?

    // ---- 三个浮层窗口（懒创建；finish 统一销毁）----
    private var toolbarWindow: ScreenshotPanelWindow?
    private var toolbarView: ToolbarPanelView?
    private var popupWindow: ScreenshotPanelWindow?
    private var popupView: PopupPanelView?
    private var tipWindow: ScreenshotPanelWindow?
    // 保存对话框模态期间被降级的浮层窗口层级快照（恢复用，见 setPanelLevelBelowModal）
    private var savedPanelLevels: [ObjectIdentifier: NSWindow.Level] = [:]

    // ---- 工具栏状态（CG 全局坐标）----
    /// 工具栏矩形（CG 全局坐标；绘制/命中/窗口定位共用）
    var toolbarRect: CGRect = .null
    /// 用户按住把手拖动过后置位：此后不再随选区自动重算位置（对齐 toolbarPlaced 语义）
    var toolbarPlaced = false
    /// 正在拖动工具栏（把手按下未松开）
    var toolbarDragging = false
    private var toolbarDragStartPoint: CGPoint = .zero    // 按下时鼠标位置（CG）
    private var toolbarDragStartRect: CGRect = .null      // 按下时的工具栏矩形
    /// hover 按钮（ScreenshotToolButton.rawValue；-1 无；SC_TB_GRIP 码 = 把手）
    var hoverBtn = -1
    /// activeTool 高亮（会话状态的镜像引用入口，直接读 session）

    // ---- 子菜单状态 ----
    /// 子菜单是否打开
    var popupOpen = false
    /// 子菜单对应的工具来源（对齐 popupTool；仅矢量工具）
    var popupTool: ScreenshotToolButton?
    /// 子菜单矩形（CG 全局坐标）
    var popupRect: CGRect = .null

    // ---- tooltip 状态（对齐 tipBtn/tipDwellSince/tipShown/tipBubbleRect/tipText）----
    private var tipBtn = -1                  // 当前停顿目标（-1 无；把手用 gripHit 码）
    private var tipDwellSince: TimeInterval = 0
    private var tipShown = false
    private var tipText = ""

    /// 工具栏总宽（CalcToolbarWidth 移植）：把手格 + 全部按钮格 + 间距 + 内边距 + 边框。
    /// 最左为把手单元格（与按钮同宽），各按钮自第 1 格起排布。
    static var toolbarWidth: CGFloat {
        return CGFloat(ScreenshotToolButton.count + 1) * (SC.toolbarBtn + SC.toolbarGap)
            - SC.toolbarGap + SC.toolbarPad * 2 + SC.toolbarBorderW * 2
    }

    init(session: ScreenshotOverlaySession) {
        self.session = session
    }

    /// 会话是否进行中（事件入口守卫，收束后残余事件不写状态）。
    private var isRunning: Bool { session?.isRunning ?? false }

    // MARK: 窗口创建/销毁/显隐

    /// 确保三个浮层窗口已创建（首次进入确认态调用）。窗口规格见文件头注释。
    private func ensureWindows() {
        guard toolbarWindow == nil, session != nil else { return }

        let toolbarWin = ScreenshotPanelWindow(
            contentRect: NSRect(origin: .zero, size: toolbarRect.size),
            styleMask: .borderless, backing: .buffered, defer: false)
        toolbarWin.level = NSWindow.Level(rawValue: NSWindow.Level.screenSaver.rawValue + 2)
        toolbarWin.isOpaque = false
        toolbarWin.backgroundColor = .clear
        toolbarWin.hasShadow = false
        toolbarWin.collectionBehavior = [.canJoinAllSpaces, .fullScreenAuxiliary]
        toolbarWin.isReleasedWhenClosed = false
        let tView = ToolbarPanelView(controller: self, frame: NSRect(origin: .zero, size: toolbarRect.size))
        toolbarWin.contentView = tView
        toolbarWindow = toolbarWin
        toolbarView = tView

        let popupSize = scCalcPopupSize(for: popupTool)
        let popupWin = ScreenshotPanelWindow(
            contentRect: NSRect(origin: .zero, size: popupSize),
            styleMask: .borderless, backing: .buffered, defer: false)
        popupWin.level = NSWindow.Level(rawValue: NSWindow.Level.screenSaver.rawValue + 3)
        popupWin.isOpaque = false
        popupWin.backgroundColor = .clear
        popupWin.hasShadow = false
        popupWin.collectionBehavior = [.canJoinAllSpaces, .fullScreenAuxiliary]
        popupWin.isReleasedWhenClosed = false
        let pView = PopupPanelView(controller: self, frame: NSRect(origin: .zero, size: popupSize))
        popupWin.contentView = pView
        popupWindow = popupWin
        popupView = pView
    }

    /// 销毁全部浮层窗口（会话 finish 调用；orderOut + 置 nil 释放内容视图）。
    func destroy() {
        for win in [toolbarWindow, popupWindow, tipWindow] {
            win?.orderOut(nil)
            win?.contentView = nil
        }
        toolbarWindow = nil
        toolbarView = nil
        popupWindow = nil
        popupView = nil
        tipWindow = nil
        toolbarPlaced = false
        toolbarDragging = false
        hoverBtn = -1
        popupOpen = false
        popupTool = nil
        popupRect = .null
        tipBtn = -1
        tipShown = false
        toolbarRect = .null
    }

    /// 工具栏/子菜单/tooltip 浮层族窗口层级临时降级/恢复（保存对话框弹出前/关闭后调用；
    /// 由会话 duckOverlayLevelsForSaveModal 统一调度）。对齐 Windows 摘除 TOPMOST 语义：
    /// Windows 工具栏画在覆盖层窗口内随之失去 TOPMOST；macOS 浮层为独立窗口需同步降级，
    /// 否则保存面板（NSModalPanelWindowLevel）会被 screenSaver+n 层的浮层遮挡。
    /// - Parameter lowered: true 降级到 SC.saveModalDuckLevel（记录原层级）；
    ///                      false 恢复（窗口已销毁时安全 no-op）
    func setPanelLevelBelowModal(_ lowered: Bool) {
        let panels = [toolbarWindow, popupWindow, tipWindow].compactMap { $0 }
        if lowered {
            savedPanelLevels.removeAll()
            for panel in panels {
                savedPanelLevels[ObjectIdentifier(panel)] = panel.level
                panel.level = SC.saveModalDuckLevel
            }
        } else {
            for panel in panels {
                if let level = savedPanelLevels[ObjectIdentifier(panel)] {
                    panel.level = level
                }
            }
            savedPanelLevels.removeAll()
        }
    }

    /// 按会话状态同步浮层显隐（pumpTick 逐拍调用，等价 OnPaint 的状态分支）：
    /// 确认/整体拖动/绘制态显示工具栏（对齐 Windows Confirmed/Moving/Drawing 显示），
    /// 调整态隐藏（避免手柄附近抖动，对齐 OnPaint CS_Resizing 不绘制工具栏）。
    /// 首次显示时先定位再懒创建窗口（生命周期挂进会话确认态入口）。
    /// - Parameter visible: 是否应显示
    func syncVisibility(_ visible: Bool) {
        if visible {
            if toolbarWindow == nil {
                syncPlacement()   // 先算出工具栏矩形（窗口尺寸取自该矩形）
                ensureWindows()
            }
            guard let toolbarWindow = toolbarWindow, !toolbarRect.isNull else { return }
            if !toolbarWindow.isVisible {
                placeWindow(toolbarWindow, at: toolbarRect)
                toolbarWindow.orderFrontRegardless()
            }
        } else {
            if let toolbarWindow = toolbarWindow, toolbarWindow.isVisible {
                toolbarWindow.orderOut(nil)
            }
            closePopup()
            hideTip()
        }
    }

    /// 同步工具栏自动跟随位置（!toolbarPlaced 时随选区重算，对齐 OnPaint 的
    /// CalcToolbarPosition 分支）；子菜单锚定工具栏同步移动。位置无变化时不动窗口。
    func syncPlacement() {
        guard let session = session, !toolbarPlaced, !toolbarDragging,
              let monitor = scMonitorBounds(for: session.selection) else { return }
        let target = scCalcToolbarRect(selection: session.selection, virtual: session.virtualBounds,
                                       monitor: monitor)
        if target != toolbarRect {
            let wasOpen = popupOpen
            toolbarRect = target
            if let win = toolbarWindow {
                placeWindow(win, at: toolbarRect)
                toolbarWindow?.orderFrontRegardless()   // 覆盖层点击后同层抬升的防御
            }
            if wasOpen { syncPopupPlacement() }
        }
    }

    /// CG 全局矩形 → NS 窗口 frame（Y 翻转：NS 原点在主屏左下）。
    private func placeWindow(_ window: NSWindow, at rect: CGRect) {
        let top = ScreenshotGeometry.primaryScreenHeight() - rect.minY
        window.setFrame(NSRect(x: rect.minX, y: top - rect.height,
                               width: rect.width, height: rect.height), display: true)
    }

    // MARK: 子菜单

    /// 打开子菜单并锚定当前工具栏（对齐「工具切换打开子菜单」路径）。
    /// 支持矢量（粗细/颜色）、文字（字号/颜色）、马赛克（三段）三类子菜单。
    /// - Parameter tool: 子菜单来源工具
    func openPopup(for tool: ScreenshotToolButton) {
        guard tool.isVectorTool || tool == .text || tool == .mosaic else { return }
        popupTool = tool
        popupOpen = true
        ensureWindows()
        syncPopupPlacement()
    }

    /// 关闭子菜单。
    func closePopup() {
        guard popupOpen || popupTool != nil else { return }
        popupOpen = false
        popupTool = nil
        popupWindow?.orderOut(nil)
    }

    /// 重新计算子菜单位置并移动窗口（工具栏移动/打开时调用）；总尺寸随子菜单类型取布局
    ///（马赛克三段更宽，打开/切换时需重设窗口 frame）。
    private func syncPopupPlacement() {
        guard popupOpen, let monitor = scMonitorBounds(for: toolbarRect) else { return }
        popupRect = scCalcPopupRect(toolbarRect: toolbarRect,
                                    virtual: session?.virtualBounds ?? .zero,
                                    monitor: monitor, tool: popupTool)
        if let popupWindow = popupWindow {
            placeWindow(popupWindow, at: popupRect)
            popupWindow.orderFrontRegardless()
        }
    }

    // MARK: 命中测试（对齐 HitTestToolbar / HitTestPopup）

    /// 命中测试工具栏（HitTestToolbar 移植）。
    /// - Parameter point: 鼠标 CG 全局坐标
    /// - Returns: -1 未命中；ScreenshotToolButton.gripHit 把手；>=0 按钮 raw 值
    func hitTestToolbar(_ point: CGPoint) -> Int {
        guard scPointInRect(point, toolbarRect) else { return -1 }
        let idx = Int((point.x - toolbarRect.minX - SC.toolbarBorderW - SC.toolbarPad)
                        / (SC.toolbarBtn + SC.toolbarGap))
        if idx < 0 || idx > ScreenshotToolButton.count { return -1 }
        if idx == 0 { return ScreenshotToolButton.gripHit }   // 第 0 格 = 拖拽把手
        return idx - 1                                        // 其后依次为各工具按钮
    }

    /// 第 N 格（0=把手，按钮自 1 起）的按钮区矩形（CG 全局坐标；tooltip 锚点用）。
    private func cellRect(_ cell: Int) -> CGRect {
        let bx = toolbarRect.minX + SC.toolbarBorderW + SC.toolbarPad
            + CGFloat(cell) * (SC.toolbarBtn + SC.toolbarGap)
        let by = toolbarRect.minY + (SC.toolbarH - SC.toolbarBtn) / 2
        return CGRect(x: bx, y: by, width: SC.toolbarBtn, height: SC.toolbarBtn)
    }

    /// 命中测试子菜单（HitTestPopup / HitTestMosaicPopup 移植，按子菜单来源工具分支）。
    /// 矢量/文字（字号）命中码约定：
    /// +1..+3 = 第 N 个粗细（文字工具时为第 N 个字号）；-1..-8 = 第 N 个颜色（负为索引+1）；
    /// 0 = 未命中（含点在分隔线上）。
    /// 马赛克命中码约定（HitTestMosaicPopup）：
    /// +1 = 涂抹模式；+2 = 框选模式；+101.. = 第 N 个块大小；+201.. = 第 N 个涂抹半径。
    /// - Parameter point: 鼠标 CG 全局坐标
    /// - Returns: 命中码
    func hitTestPopup(_ point: CGPoint) -> Int {
        guard scPointInRect(point, popupRect) else { return 0 }
        let contentLeft = popupRect.minX + SC.popupBorderW + SC.popupPad
        let contentTop = popupRect.minY + SC.popupBorderW + SC.popupPad
        let cellGap = SC.toolbarGap   // PopupCellGap（cell == SC_POPUP_CELL 时 = 工具栏间距）
        if point.y < contentTop || point.y >= contentTop + SC.popupCell { return 0 }

        // ---- 马赛克三段子菜单：[涂抹|框选]｜分隔线｜[块大小×3]｜分隔线｜[涂抹半径×3] ----
        if popupTool == .mosaic {
            // 模式组
            for i in 0..<SC.mosaicModeCount {
                let ix = contentLeft + CGFloat(i) * (SC.popupCell + cellGap)
                if point.x >= ix && point.x < ix + SC.popupCell { return i + 1 }   // +1 涂抹 +2 框选
            }
            let modeEndX = contentLeft + CGFloat(SC.mosaicModeCount) * SC.popupCell
                + CGFloat(SC.mosaicModeCount - 1) * cellGap
            let sizeStartX = modeEndX + SC.popupSepGap * 2 + 1
            if point.x < sizeStartX { return 0 }   // 第一条分隔线区域
            // 块大小组
            let sizeEndX = sizeStartX + CGFloat(SC.mosaicSizes.count) * SC.popupCell
                + CGFloat(SC.mosaicSizes.count - 1) * cellGap
            for i in SC.mosaicSizes.indices {
                let ix = sizeStartX + CGFloat(i) * (SC.popupCell + cellGap)
                if point.x >= ix && point.x < ix + SC.popupCell {
                    return SC.mosaicHitSizeBase + i + 1
                }
            }
            // 涂抹半径组
            let radiusStartX = sizeEndX + SC.popupSepGap * 2 + 1
            if point.x < radiusStartX { return 0 }   // 第二条分隔线区域
            for i in SC.mosaicRadii.indices {
                let ix = radiusStartX + CGFloat(i) * (SC.popupCell + cellGap)
                if point.x >= ix && point.x < ix + SC.popupCell {
                    return SC.mosaicHitRadiusBase + i + 1
                }
            }
            return 0
        }

        // ---- 矢量/文字单行子菜单：[粗细×3 或 字号×3]｜分隔线｜[颜色×8] ----
        // 第一组（文字工具时为字号索引，矢量工具时为粗细索引）
        let firstCount = (popupTool == .text) ? SC.fontSizes.count : SC.thickPresets.count
        let firstX0 = contentLeft
        for i in 0..<firstCount {
            let ix = firstX0 + CGFloat(i) * (SC.popupCell + cellGap)
            if point.x >= ix && point.x < ix + SC.popupCell { return i + 1 }
        }
        let firstEndX = firstX0 + CGFloat(firstCount) * SC.popupCell
            + CGFloat(firstCount - 1) * cellGap
        // 分隔线区域（不命中）
        let colorStartX = firstEndX + SC.popupSepGap * 2 + 1
        if point.x < colorStartX { return 0 }
        // 颜色组
        for i in SC.colorPresets.indices {
            let ix = colorStartX + CGFloat(i) * (SC.popupCell + cellGap)
            if point.x >= ix && point.x < ix + SC.popupCell { return -(i + 1) }
        }
        return 0
    }

    // MARK: 工具栏鼠标事件（ToolbarPanelView 转发）

    /// 工具栏左键按下：文字编辑态先提交文字并退出编辑（提交路径 3；对齐 Windows
    /// 编辑态工具栏分支——只提交退出，不处理本次按钮动作），随后按常规流程处理；
    /// 命中把手进入整体拖拽（置 toolbarPlaced，此后不再自动跟随选区）；
    /// 命中可用按钮 → 动作回话会话。按下左键期间 tooltip 收起（对齐 TickToolbarTooltip）。
    /// - Parameter point: 鼠标 CG 全局坐标
    func handleToolbarMouseDown(_ point: CGPoint) {
        guard isRunning else { return }
        // 文字编辑态：先提交当前文字（如有）并回确认态，本次按钮动作不执行
        //（用户需再次点击才能触发按钮，对齐 overlay_input_windows.cpp 编辑态工具栏分支）
        if session?.state == .textEditing {
            session?.commitPendingTextAndExitEditing()
            refresh()   // 提交文字入撤销栈：撤销按钮可用态变化需重绘
            return
        }
        let hit = hitTestToolbar(point)
        hideTip()
        if hit == ScreenshotToolButton.gripHit {
            // 最左「6 点把手」：进入工具栏整体拖拽（对齐 OnLButtonDown 的 SC_TB_GRIP 分支）；
            // 拖拽期间保持把手 hover 底（Windows 的 hover 缓存同款行为）
            toolbarDragging = true
            toolbarPlaced = true
            hoverBtn = ScreenshotToolButton.gripHit
            toolbarDragStartPoint = point
            toolbarDragStartRect = toolbarRect
            toolbarView?.needsDisplay = true
            return
        }
        guard hit >= 0, let button = ScreenshotToolButton(rawValue: hit),
              !button.isSeparator, isButtonEnabled(button) else { return }
        session?.handleToolbarButton(button)
    }

    /// 工具栏拖动中：按「按下矩形 + 鼠标位移」平移并钳制在虚拟屏内（对齐 OnMouseMove
    /// 的 toolbarDragging 分支；显式拖放允许跨屏，不受单显示器边界约束），
    /// 子菜单锚定工具栏同步移动。
    /// - Parameter point: 鼠标 CG 全局坐标
    func handleToolbarMouseDragged(_ point: CGPoint) {
        guard isRunning, toolbarDragging else { return }
        let dx = point.x - toolbarDragStartPoint.x
        let dy = point.y - toolbarDragStartPoint.y
        var n = toolbarDragStartRect
        n.origin.x += dx
        n.origin.y += dy
        let virtual = session?.virtualBounds ?? .zero
        if n.minX < virtual.minX { n.origin.x = virtual.minX }
        if n.minY < virtual.minY { n.origin.y = virtual.minY }
        if n.maxX > virtual.maxX { n.origin.x = virtual.maxX - n.width }
        if n.maxY > virtual.maxY { n.origin.y = virtual.maxY - n.height }
        toolbarRect = n
        if let win = toolbarWindow { placeWindow(win, at: toolbarRect) }
        if popupOpen { syncPopupPlacement() }
        toolbarView?.needsDisplay = true
    }

    /// 工具栏左键抬起：退出把手拖拽态（位置已在拖拽中固化，对齐 OnLButtonUp）。
    func handleToolbarMouseUp() {
        guard toolbarDragging else { return }
        toolbarDragging = false
        toolbarView?.needsDisplay = true
    }

    // MARK: 子菜单鼠标事件（PopupPanelView 转发）

    /// 子菜单左键按下：文字编辑态 = 提交路径 1/2（点字号/颜色格先设置索引再提交并退出
    /// 编辑态，新属性将用于随后的提交——对齐 Windows 先设索引再
    /// CommitPendingTextAndExitEditing 的次序）；确认态 = 命中码测试后交会话应用
    /// （有选中标注时就地修改，无选中作用于后续绘制）。
    /// - Parameter point: 鼠标 CG 全局坐标
    func handlePopupMouseDown(_ point: CGPoint) {
        guard isRunning, popupOpen else { return }
        let hit = hitTestPopup(point)
        guard hit != 0 else { return }
        if session?.state == .textEditing {
            // 编辑态：仅字号/颜色两路径（编辑态子菜单必为文字工具；马赛克子菜单
            // 不会在编辑态出现，防御性忽略）
            session?.applyTextEditingPopupSelection(hit)
            session?.commitPendingTextAndExitEditing()
            refresh()   // 字号/颜色选中格 + 撤销按钮可用态变化需重绘
            return
        }
        session?.applyPopupSelection(hit)
    }

    // MARK: 按钮可用态

    /// 按钮是否可用（马赛克/文字可用；保存可用；长截图仍置灰；
    /// 撤销/重做按快照栈状态；翻译占位无点击处理但保留 hover/tooltip 与 Windows 一致）。
    /// 保存按钮启用规则对齐 Windows：overlay_input_windows.cpp 的 TB_Save 分支无任何
    /// 禁用判定（工具栏仅在确认态出现，编辑态恒可用），故此处恒返回 true。
    func isButtonEnabled(_ button: ScreenshotToolButton) -> Bool {
        switch button {
        case .longCapture:        return true    // 长截图入口（编辑态点击进入长截图模式）
        case .save:               return true    // 保存对话框（Windows 编辑态恒可用）
        case .undo:               return !(session?.undoStack.isEmpty ?? true)
        case .redo:               return !(session?.redoStack.isEmpty ?? true)
        default:                  return true    // mosaic/text/cancel/confirm/drag/矢量工具/translate(占位)
        }
    }

    /// 刷新工具栏与子菜单绘制（选中高亮/可用态/子菜单选中项等任何依赖会话状态的
    /// 视觉变化后调用；轻量局部重绘。子菜单选中格、马赛克半径置灰等均画在 popupView，
    /// 漏刷会表现为「点击选项高亮不动、看似无反应」；子菜单未创建/未打开时为无害 no-op）。
    func refresh() {
        toolbarView?.needsDisplay = true
        popupView?.needsDisplay = true
    }

    // MARK: pumpTick 轮询（hover + tooltip + 光标；对齐 TickToolbarTooltip 轮询模型）

    /// 当前鼠标 CG 全局坐标。
    private func currentMouseCG() -> CGPoint {
        return ScreenshotGeometry.cgPoint(fromNS: NSEvent.mouseLocation)
    }

    /// 泵循环逐拍轮询（对齐 TickToolbarTooltip：覆盖层不接收稳定鼠标流，停顿判定靠轮询）：
    /// 1) hover 高亮变化重绘工具栏；2) 停顿满 500ms 显示 tooltip 气泡；
    /// 3) 工具栏/子菜单区域接管光标（箭头/把手四向/子菜单手型）。
    /// - Parameter now: 单调时钟（ProcessInfo.systemUptime）
    func tick(now: TimeInterval) {
        guard isRunning, let toolbarWindow = toolbarWindow else { return }
        let mouse = currentMouseCG()
        var target = -1
        // 确认态与文字编辑态 hover 均有意义（对齐 TickToolbarTooltip 的
        // CS_Confirmed/CS_TextEditing 门）；按下左键期间视为无目标
        //（NSEvent.pressedMouseButtons bit0 = 左键）
        let hoverEligible = (session?.state == .confirmed || session?.state == .textEditing)
        if hoverEligible && toolbarWindow.isVisible && NSEvent.pressedMouseButtons & 1 == 0 {
            target = hitTestToolbar(mouse)
        }
        if target != tipBtn {
            hideTip()                      // 目标切换/离开：收起并重新停顿
            tipBtn = target
            tipDwellSince = now
        } else if target >= 0 && !tipShown && now - tipDwellSince >= SC.tipDelaySec {
            showTip(for: target)           // 同一目标停顿满延时：计算气泡并显示
        }

        // hover 高亮（仅可用按钮与把手响应；禁用格不铺 hover 底）。把手拖拽中冻结在
        // 把手位（对齐 Windows：拖拽分支提前返回，不重算 hoverToolbarBtn）
        if !toolbarDragging {
            var hover = -1
            if hoverEligible && toolbarWindow.isVisible
                && NSEvent.pressedMouseButtons & 1 == 0 {
                let hit = hitTestToolbar(mouse)
                if hit == ScreenshotToolButton.gripHit {
                    hover = hit
                } else if hit >= 0, let b = ScreenshotToolButton(rawValue: hit),
                          !b.isSeparator, isButtonEnabled(b) {
                    hover = hit
                }
            }
            if hover != hoverBtn {
                hoverBtn = hover
                toolbarView?.needsDisplay = true
            }
        }

        // 光标接管：工具栏/子菜单区域独立于覆盖层光标逻辑（对齐 OnSetCursor 的工具栏分支）；
        // 仅在对应窗口在屏时接管，避免隐藏期间残留矩形错误覆盖 resize 光标
        if toolbarDragging {
            NSCursor.closedHand.set()      // Windows IDC_SIZEALL（把手拖拽中）
        } else if toolbarWindow.isVisible && scPointInRect(mouse, toolbarRect) {
            if hitTestToolbar(mouse) == ScreenshotToolButton.gripHit {
                NSCursor.openHand.set()    // Windows IDC_SIZEALL（把手可拖动）
            } else {
                NSCursor.arrow.set()
            }
        } else if popupOpen, let popupWindow = popupWindow, popupWindow.isVisible,
                  scPointInRect(mouse, popupRect) {
            NSCursor.pointingHand.set()    // Windows IDC_HAND（子菜单）
        } else {
            session?.updateCursor()
        }
    }

    /// 显示目标按钮的 tooltip 气泡（对齐 TickToolbarTooltip 的显示分支）：
    /// 锚点 = 目标单元格；水平居中并钳制虚拟屏；优先上方，放不下转下方。
    /// - Parameter btn: 目标（按钮 raw 值或 gripHit 码）
    private func showTip(for btn: Int) {
        guard let text = tooltipText(for: btn), !text.isEmpty, let session = session else { return }
        let cell = (btn == ScreenshotToolButton.gripHit) ? 0 : btn + 1
        let anchor = cellRect(cell)
        // 按文本测量气泡尺寸（12px 系统字体 + 内边距，对齐 MeasureTipBubbleSize）
        let attr = NSAttributedString(string: text, attributes: [
            .font: NSFont.systemFont(ofSize: SC.fontPx),
        ])
        let textSize = attr.size()
        let w = ceil(textSize.width) + SC.tipPadX * 2
        let h = ceil(textSize.height) + SC.tipPadY * 2
        let virtual = session.virtualBounds
        var x = anchor.midX - w / 2
        if x < SC.tipEdgeClamp { x = SC.tipEdgeClamp }
        if x + w > virtual.maxX - SC.tipEdgeClamp { x = virtual.maxX - SC.tipEdgeClamp - w }
        // 优先上方，放不下转下方（工具栏在选区上方时）
        var y = anchor.minY - SC.tipGap - h
        if y < SC.tipEdgeClamp { y = anchor.maxY + SC.tipGap }

        // 复用/创建 tooltip 窗口并按气泡尺寸定位
        if tipWindow == nil {
            let win = ScreenshotPanelWindow(
                contentRect: NSRect(origin: .zero, size: CGSize(width: w, height: h)),
                styleMask: .borderless, backing: .buffered, defer: false)
            win.level = NSWindow.Level(rawValue: NSWindow.Level.screenSaver.rawValue + 4)
            win.isOpaque = false
            win.backgroundColor = .clear
            win.hasShadow = false
            win.collectionBehavior = [.canJoinAllSpaces, .fullScreenAuxiliary]
            win.isReleasedWhenClosed = false
            tipWindow = win
        }
        let win = tipWindow!
        win.contentView = TooltipPanelView(text: text,
                                           frame: NSRect(origin: .zero, size: CGSize(width: w, height: h)))
        placeWindow(win, at: CGRect(x: x, y: y, width: w, height: h))
        win.orderFrontRegardless()
        tipShown = true
        tipText = text
    }

    /// 收起 tooltip 气泡（目标变化/离开/按下左键/工具栏隐藏时调用）。
    private func hideTip() {
        guard tipShown else { return }
        tipShown = false
        tipText = ""
        tipWindow?.orderOut(nil)
    }

    /// 按钮的 tooltip 文案（ToolbarButtonTip 移植，全中文照搬；分隔线返回 nil）。
    /// - Parameter btn: 按钮 raw 值或 gripHit 码
    /// - Returns: tooltip 文本；分隔线/未知返回 nil
    private func tooltipText(for btn: Int) -> String? {
        if btn == ScreenshotToolButton.gripHit { return "拖动工具栏" }
        guard let b = ScreenshotToolButton(rawValue: btn) else { return nil }
        switch b {
        case .drag:        return "拖拽"
        case .rect:        return "矩形"
        case .circle:      return "圆形"
        case .arrow:       return "箭头"
        case .brush:       return "画笔"
        case .mosaic:      return "马赛克"
        case .text:        return "文字"
        case .translate:   return "翻译"
        case .longCapture: return "长截图"
        case .undo:        return "撤销"
        case .redo:        return "重做"
        case .save:        return "保存到本地"
        case .cancel:      return "取消"
        case .confirm:     return "确定"
        default:           return nil
        }
    }

    // MARK: 绘制（对齐 DrawToolbar / DrawPopup）

    /// 绘制工具栏（DrawToolbar 移植；视图本地坐标 = 工具栏矩形相对坐标）：
    /// 第一遍白底圆角背景 + hover/active 圆角高亮，再绘制把手 6 点、分隔线与图标。
    /// - Parameter ctx: 工具栏视图 CG 上下文（已翻转，左上原点）
    func drawToolbar(_ ctx: CGContext) {
        let w = toolbarRect.width
        let h = toolbarRect.height
        let btn = SC.toolbarBtn
        let btnPad = (SC.toolbarH - btn) / 2
        let activeToolRaw = session?.activeTool?.rawValue ?? -1

        // 白色圆角背景 + 1px 浅灰边框（圆角外保持透明，露出后方截图）
        let bgPath = roundedRectPath(CGRect(x: 0, y: 0, width: w, height: h), SC.toolbarRadius)
        ctx.addPath(bgPath)
        ctx.setFillColor(NSColor.white.cgColor)
        ctx.fillPath()
        ctx.addPath(bgPath)
        ctx.setStrokeColor(SC.toolbarBorderCol.cgColor)
        ctx.setLineWidth(SC.toolbarBorderW)
        ctx.strokePath()

        // 各按钮圆角高亮（hover 浅蓝 / active 选中浅蓝；禁用格不响应 hover）
        let hlRadius = btn / 8
        let hlInset: CGFloat = 2
        let hlSize = btn - hlInset * 2
        for i in 0..<ScreenshotToolButton.count {
            guard let button = ScreenshotToolButton(rawValue: i), !button.isSeparator else { continue }
            let isHover = (i == hoverBtn && isButtonEnabled(button))
            let isActive = (i == activeToolRaw)
            if !isHover && !isActive { continue }
            let bx = SC.toolbarBorderW + SC.toolbarPad + CGFloat(i + 1) * (btn + SC.toolbarGap)
            let by = btnPad
            let path = roundedRectPath(
                CGRect(x: bx + hlInset, y: by + hlInset, width: hlSize, height: hlSize), hlRadius)
            ctx.addPath(path)
            ctx.setFillColor((isActive ? SC.toolbarSelBg : SC.toolbarHoverBg).cgColor)
            ctx.fillPath()
        }

        // 最左「6 点拖拽把手」：hover 铺浅蓝底 + 灰色圆点（DrawToolbarGrip 移植）
        let gripCell = CGRect(x: SC.toolbarBorderW + SC.toolbarPad, y: btnPad, width: btn, height: btn)
        let gripHot = (hoverBtn == ScreenshotToolButton.gripHit)
        if gripHot {
            let path = roundedRectPath(
                CGRect(x: gripCell.minX + hlInset, y: gripCell.minY + hlInset,
                       width: hlSize, height: hlSize), hlRadius)
            ctx.addPath(path)
            ctx.setFillColor(SC.toolbarHoverBg.cgColor)
            ctx.fillPath()
        }
        // 圆点几何随按钮格尺寸缩放：列距 ±11%、行距 0/±16%、点半径 ~5%
        let gcx = gripCell.midX
        let gcy = gripCell.midY
        let colGap = btn * 0.11
        let rowGap = btn * 0.16
        let dotR = max(1.2, btn * 0.05)
        ctx.setFillColor(SC.gripDotCol.cgColor)
        for row in -1...1 {
            for col in [-1, 1] {
                let dx = gcx + CGFloat(col) * colGap
                let dy = gcy + CGFloat(row) * rowGap
                ctx.fillEllipse(in: CGRect(x: dx - dotR, y: dy - dotR, width: dotR * 2, height: dotR * 2))
            }
        }

        // 分隔线 + 图标（第二遍）
        for i in 0..<ScreenshotToolButton.count {
            guard let button = ScreenshotToolButton(rawValue: i) else { continue }
            let bx = SC.toolbarBorderW + SC.toolbarPad + CGFloat(i + 1) * (btn + SC.toolbarGap)
            let by = btnPad
            let cellFrame = CGRect(x: bx, y: by, width: btn, height: btn)

            if button.isSeparator {
                // 竖直分隔线（上下各缩进 btn/8 + 2）
                let sx = cellFrame.midX
                let sepInset = btn / 8 + 2
                ctx.setStrokeColor(SC.toolbarSepCol.cgColor)
                ctx.setLineWidth(1)
                ctx.move(to: CGPoint(x: sx, y: cellFrame.minY + sepInset))
                ctx.addLine(to: CGPoint(x: sx, y: cellFrame.maxY - sepInset))
                ctx.strokePath()
                continue
            }

            // 图标三态：active 主题蓝 / 普通深灰 / 禁用（含空撤销重做栈）半透明置灰
            let enabled = isButtonEnabled(button)
            let iconColor: NSColor
            if !enabled {
                iconColor = SC.iconDark.withAlphaComponent(0.35)
            } else if i == activeToolRaw {
                iconColor = SC.toolbarBlue
            } else {
                iconColor = SC.iconDark
            }
            guard let icon = button.toolbarIcon?.tinted(iconColor) else { continue }
            let size = SCToolbarIcon.iconPointSize
            icon.draw(in: NSRect(x: cellFrame.midX - size / 2, y: cellFrame.midY - size / 2,
                                 width: size, height: size))
        }
    }

    /// 绘制子菜单（DrawPopup / DrawMosaicPopup 移植；视图本地坐标 = 子菜单矩形相对坐标）：
    /// - 矢量：第一组粗细圆点（直径取 SC_THICK_DOT_SIZES）｜分隔线｜颜色圆点
    /// - 文字：第一组字号（不同大小 'A'，缩放 0.72/0.62 防溢出）｜分隔线｜颜色圆点
    /// - 马赛克：[涂抹波浪线|框选虚线框]｜[马赛克网格×3]｜[半径圆点×3（框选模式置灰）]
    /// 选中格铺选中浅蓝底（颜色组铺带透明 alpha 80 的选中色底）。
    /// - Parameter ctx: 子菜单视图 CG 上下文（已翻转，左上原点）
    func drawPopup(_ ctx: CGContext) {
        let pw = popupRect.width
        let ph = popupRect.height
        let cell = SC.popupCell
        let cellGap = SC.toolbarGap

        // 白色圆角背景 + 浅灰边框
        let bgPath = roundedRectPath(CGRect(x: 0, y: 0, width: pw, height: ph), SC.popupRadius)
        ctx.addPath(bgPath)
        ctx.setFillColor(NSColor.white.cgColor)
        ctx.fillPath()
        ctx.addPath(bgPath)
        ctx.setStrokeColor(SC.toolbarBorderCol.cgColor)
        ctx.setLineWidth(SC.popupBorderW)
        ctx.strokePath()

        let contentLeft = SC.popupBorderW + SC.popupPad
        let contentTop = SC.popupBorderW + SC.popupPad
        let midY = contentTop + cell / 2
        let colorIdx = session?.drawColorIdx ?? 0
        let thickIdx = session?.drawThickIdx ?? 0
        let fontIdx = session?.fontSizeIdx ?? SC.defaultFontIdx

        // 单元格选中背景（DrawPopupCellBg 移植：圆角底 + 带透明灰描边）。
        func cellBg(_ cellLeft: CGFloat, _ color: NSColor) {
            let path = roundedRectPath(
                CGRect(x: cellLeft, y: contentTop, width: cell, height: cell), cell / 4)
            ctx.addPath(path)
            ctx.setFillColor(color.cgColor)
            ctx.fillPath()
            ctx.addPath(path)
            ctx.setStrokeColor(NSColor(srgbRed: 160.0 / 255.0, green: 160.0 / 255.0,
                                       blue: 160.0 / 255.0, alpha: 90.0 / 255.0).cgColor)
            ctx.setLineWidth(1)
            ctx.strokePath()
        }
        // 1px 竖直分隔线（两侧 sepGap）。
        func separator(_ sepX: CGFloat) {
            ctx.setStrokeColor(SC.popupSepCol.cgColor)
            ctx.setLineWidth(1)
            ctx.move(to: CGPoint(x: sepX, y: midY - SC.popupSepH / 2))
            ctx.addLine(to: CGPoint(x: sepX, y: midY + SC.popupSepH / 2))
            ctx.strokePath()
        }

        // ---- 马赛克三段子菜单（DrawMosaicPopup 移植）----
        if popupTool == .mosaic {
            let modeIdx = session?.mosaicRectMode == true ? 1 : 0
            let sizeIdx = session?.mosaicSizeIdx ?? SC.defaultMosaicIdx
            let radiusIdx = session?.mosaicRadiusIdx ?? SC.defaultMosaicRadiusIdx
            let iconBlue = SC.toolbarBlue
            let iconDark = SC.popupIconDark

            // 模式组：涂抹 = 自由波浪线（圆头）；框选 = 虚线矩形
            for i in 0..<SC.mosaicModeCount {
                let cellLeft = contentLeft + CGFloat(i) * (cell + cellGap)
                let sel = (i == modeIdx)
                if sel { cellBg(cellLeft, SC.toolbarSelBg) }
                let c = sel ? iconBlue : iconDark
                let cx = cellLeft + cell / 2
                if i == 0 {
                    // 涂抹：一条波浪线模拟涂抹轨迹
                    ctx.setStrokeColor(c.cgColor)
                    ctx.setLineWidth(2)
                    ctx.setLineCap(.round)
                    ctx.setLineJoin(.round)
                    ctx.move(to: CGPoint(x: cx - cell * 0.28, y: midY + cell * 0.18))
                    ctx.addCurve(to: CGPoint(x: cx + cell * 0.10, y: midY + cell * 0.18),
                                 control1: CGPoint(x: cx - cell * 0.10, y: midY - cell * 0.18),
                                 control2: CGPoint(x: cx, y: midY + cell * 0.30))
                    ctx.addLine(to: CGPoint(x: cx + cell * 0.28, y: midY - cell * 0.18))
                    ctx.strokePath()
                } else {
                    // 框选：虚线矩形
                    let r = cell * 0.24
                    ctx.setStrokeColor(c.cgColor)
                    ctx.setLineWidth(2)
                    ctx.setLineDash(phase: 0, lengths: [4, 3])
                    ctx.stroke(CGRect(x: cx - r, y: midY - r, width: r * 2, height: r * 2))
                    ctx.setLineDash(phase: 0, lengths: [])
                }
            }

            // 第一条分隔线 + 块大小组（N×N 马赛克方块网格，块数随档位递增 2/3/4）
            let modeEndX = contentLeft + CGFloat(SC.mosaicModeCount) * cell
                + CGFloat(SC.mosaicModeCount - 1) * cellGap
            let sepX = modeEndX + SC.popupSepGap
            separator(sepX)
            let sizeStartX = sepX + SC.popupSepGap + 1
            for i in SC.mosaicSizes.indices {
                let cellLeft = sizeStartX + CGFloat(i) * (cell + cellGap)
                let sel = (i == sizeIdx)
                if sel { cellBg(cellLeft, SC.toolbarSelBg) }
                let c = sel ? iconBlue : iconDark
                let cx = cellLeft + cell / 2
                let gridHalf = cell * 0.26
                let gridSize = gridHalf * 2
                let gx = cx - gridHalf
                let gy = midY - gridHalf
                let n = 2 + i   // i=0 → 2×2，i=1 → 3×3，i=2 → 4×4
                let cellSz = max(1, gridSize / CGFloat(n))
                ctx.setFillColor(c.cgColor)
                for ry in 0..<n {
                    for rx in 0..<n where (rx + ry) % 2 == 0 {
                        ctx.fill(CGRect(x: gx + CGFloat(rx) * cellSz, y: gy + CGFloat(ry) * cellSz,
                                        width: cellSz, height: cellSz))
                    }
                }
                // 网格描边（半透明）
                ctx.setStrokeColor(c.withAlphaComponent(sel ? 0.78 : 0.47).cgColor)
                ctx.setLineWidth(1)
                for k in 0...n {
                    ctx.move(to: CGPoint(x: gx + CGFloat(k) * cellSz, y: gy))
                    ctx.addLine(to: CGPoint(x: gx + CGFloat(k) * cellSz, y: gy + CGFloat(n) * cellSz))
                    ctx.move(to: CGPoint(x: gx, y: gy + CGFloat(k) * cellSz))
                    ctx.addLine(to: CGPoint(x: gx + CGFloat(n) * cellSz, y: gy + CGFloat(k) * cellSz))
                }
                ctx.strokePath()
            }

            // 第二条分隔线 + 涂抹半径组（圆点直径随预设；框选模式下置灰仍可点击）
            let sizeEndX = sizeStartX + CGFloat(SC.mosaicSizes.count) * cell
                + CGFloat(SC.mosaicSizes.count - 1) * cellGap
            let sep2X = sizeEndX + SC.popupSepGap
            separator(sep2X)
            let radiusStartX = sep2X + SC.popupSepGap + 1
            let radiusEnabled = (modeIdx == 0)
            for i in SC.mosaicRadii.indices {
                let cellLeft = radiusStartX + CGFloat(i) * (cell + cellGap)
                let sel = (i == radiusIdx) && radiusEnabled
                if sel { cellBg(cellLeft, SC.toolbarSelBg) }
                let baseC = (i == radiusIdx) ? iconBlue : iconDark
                let c = radiusEnabled ? baseC : baseC.withAlphaComponent(0.63)
                let cx = cellLeft + cell / 2
                var dotD = CGFloat(SC.mosaicRadii[i]) * 0.5
                dotD = min(max(dotD, 5), cell - 4)   // 直径随预设递增，钳制 [5, cell-4]
                ctx.setFillColor(c.cgColor)
                ctx.fillEllipse(in: CGRect(x: cx - dotD / 2, y: midY - dotD / 2,
                                           width: dotD, height: dotD))
            }
            return
        }

        // ---- 矢量/文字单行子菜单 ----
        // 第一组：文字工具显示字号（不同大小 'A' 居中），矢量工具显示粗细（圆点直径区分）
        let firstCount = (popupTool == .text) ? SC.fontSizes.count : SC.thickPresets.count
        for i in 0..<firstCount {
            let cellLeft = contentLeft + CGFloat(i) * (cell + cellGap)
            let sel = (i == (popupTool == .text ? fontIdx : thickIdx))
            if sel { cellBg(cellLeft, SC.toolbarSelBg) }
            let iconC = sel ? SC.toolbarBlue : SC.popupIconDark
            let cx = cellLeft + cell / 2
            if popupTool == .text {
                // 字号：不同大小的字母 'A' 居中（最大档额外缩小防溢出，对齐 sizeScale 参数）
                let sizeScale: CGFloat = (i == SC.fontSizes.count - 1) ? 0.62 : 0.72
                let glyphPx = max(6, CGFloat(SC.fontSizes[i]) * sizeScale)
                let attr = NSAttributedString(string: "A", attributes: [
                    .font: NSFont.systemFont(ofSize: glyphPx),
                    .foregroundColor: iconC,
                ])
                let sz = attr.size()
                attr.draw(at: NSPoint(x: cx - sz.width / 2,
                                      y: midY - sz.height / 2 + (sz.height - glyphPx) / 2))
            } else {
                // 粗细：圆点直径取 SC_THICK_DOT_SIZES
                var dotD = SC.thickDotSizes[i]
                dotD = min(max(dotD, 4), cell)
                ctx.setFillColor(iconC.cgColor)
                ctx.fillEllipse(in: CGRect(x: cx - dotD / 2, y: midY - dotD / 2,
                                           width: dotD, height: dotD))
            }
        }

        // 竖直分隔线
        let firstEndX = contentLeft + CGFloat(firstCount) * cell + CGFloat(firstCount - 1) * cellGap
        let sepX = firstEndX + SC.popupSepGap
        separator(sepX)

        // 第二组：颜色（固定直径圆点 + 极浅描边；选中格铺带透明的选中色底）
        let colorStartX = sepX + SC.popupSepGap + 1
        for i in SC.colorPresets.indices {
            let c = SC.colorPresets[i]
            let color = NSColor(srgbRed: CGFloat(c.r) / 255.0, green: CGFloat(c.g) / 255.0,
                                blue: CGFloat(c.b) / 255.0, alpha: 1.0)
            let cellLeft = colorStartX + CGFloat(i) * (cell + cellGap)
            let cx = cellLeft + cell / 2
            let r = SC.popupColorDot / 2
            if i == colorIdx {
                cellBg(cellLeft, color.withAlphaComponent(80.0 / 255.0))
            }
            ctx.setFillColor(color.cgColor)
            ctx.fillEllipse(in: CGRect(x: cx - r, y: midY - r, width: r * 2, height: r * 2))
            // 圆点本身始终保留极浅描边（白色块可见性，选中也保留）
            ctx.setStrokeColor(SC.popupSepCol.cgColor)
            ctx.setLineWidth(1)
            ctx.strokeEllipse(in: CGRect(x: cx - r, y: midY - r, width: r * 2, height: r * 2))
        }
    }

    /// 圆角矩形路径（AddRoundedRect 移植；radius 自动钳制不超过短边一半）。
    private func roundedRectPath(_ rect: CGRect, _ radius: CGFloat) -> CGPath {
        let r = min(radius, min(rect.width, rect.height) / 2)
        return CGPath(roundedRect: rect, cornerWidth: max(1, r), cornerHeight: max(1, r), transform: nil)
    }
}
