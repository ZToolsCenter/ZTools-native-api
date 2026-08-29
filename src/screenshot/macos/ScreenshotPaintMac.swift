import Foundation
import AppKit
import CoreGraphics

// MARK: - 覆盖层常量（Windows 出处集中标注）
//
// 全部常量与 Windows src/screenshot/internal.h 及 overlay_ui_windows.cpp 对齐。
// macOS 差异说明：Windows 按 dpiScale 缩放这些基准值（1080p → 4K 下手柄/面板同步放大）；
// macOS 的逻辑点坐标系本身已与分辨率无关（NSScreen.backingScaleFactor 是像素密度而非
// UI 缩放），因此常量恒以逻辑点使用，Retina 与低分屏下视觉大小一致。
enum SC {
    /// 最小选区尺寸（internal.h: SC_MIN_SELECTION）
    static let minSelection: CGFloat = 10
    /// resize 手柄边长 / 命中半宽（internal.h: SC_HANDLE_SIZE）
    static let handleSize: CGFloat = 10
    /// 圆角拖拽手柄距选区角的静止内缩距离（internal.h: SC_CORNER_KNOB_INSET）
    static let cornerKnobInset: CGFloat = 18
    /// 倒角手柄"靠近"感应余量：命中框外再扩此距离即显示该角手柄（internal.h: SC_CORNER_PROXIMITY）
    static let cornerProximity: CGFloat = 14
    /// 选区外遮罩强度（internal.h: SC_MASK_ALPHA，微信风格：选区内清晰、外暗化）
    static let maskAlpha: CGFloat = 120.0 / 255.0
    /// 放大镜面板边长（internal.h: SC_PANEL_WIDTH/HEIGHT）
    static let panelSize: CGFloat = 140
    /// 面板内放大镜区高度（internal.h: SC_MAGNIFIER_HEIGHT）
    static let magnifierHeight: CGFloat = 74
    /// 面板与鼠标/选区的间距（internal.h: SC_PANEL_MARGIN）
    static let panelMargin: CGFloat = 15
    /// 面板/尺寸标签圆角（internal.h: SC_PANEL_CORNER_RADIUS）
    static let panelCornerRadius: CGFloat = 8
    /// 放大镜放大倍数（internal.h: SC_ZOOM_FACTOR）
    static let zoomFactor: CGFloat = 4
    /// 面板内边距（overlay_ui_windows.cpp CalcPanelMetrics.borderPad 基准）
    static let panelBorderPad: CGFloat = 2
    /// 面板文字内边距（CalcPanelMetrics.labelPad 基准）
    static let panelLabelPad: CGFloat = 6
    /// 尺寸标签/信息面板字号（CalcPanelMetrics fontPx 基准 12）
    static let fontPx: CGFloat = 12
    /// 尺寸标签水平内边距（CalcPanelMetrics sizeLabelPadX 基准）
    static let sizeLabelPadX: CGFloat = 12
    /// 尺寸标签垂直内边距（CalcPanelMetrics sizeLabelPadY 基准）
    static let sizeLabelPadY: CGFloat = 4
    /// 尺寸标签与参考矩形间距（CalcPanelMetrics sizeLabelGap 基准）
    static let sizeLabelGap: CGFloat = 5

    /// 强调蓝：选区/高亮/手柄/准星（internal.h: SC_THEME_ACCENT_BLUE RGB(0x00,0x88,0xFF)）
    static let accentBlue = NSColor(srgbRed: 0x00 / 255.0, green: 0x88 / 255.0, blue: 0xFF / 255.0, alpha: 1.0)
    /// 面板/标签底色（session_windows.cpp SCGdiResources::Init 的 bgBrush RGB(52,52,53)）
    static let panelBg = NSColor(srgbRed: 52.0 / 255.0, green: 52.0 / 255.0, blue: 53.0 / 255.0, alpha: 1.0)
    /// 面板/标签描边（borderPen RGB(102,102,102)）
    static let panelBorder = NSColor(srgbRed: 102.0 / 255.0, green: 102.0 / 255.0, blue: 102.0 / 255.0, alpha: 1.0)

    /// 手柄脏区扩张余量（overlay_ui_windows.cpp CalcHandleMetrics.handleMargin = handleSize/2 + 4）
    static var handleMargin: CGFloat { handleSize / 2 + 4 }

    /// 保存对话框模态期间浮层族的临时降级层级（对齐 Windows 弹保存对话框前摘除
    /// TOPMOST——output_windows.cpp PromptSaveFilePath 的 HWND_NOTOPMOST）。取
    /// NSModalPanelWindowLevel − 1：模态保存面板之下、普通应用窗口之上，覆盖层与工具栏
    /// 保持可见、仅对话框浮于其上（用户可见行为最接近 Windows）。
    static let saveModalDuckLevel = NSWindow.Level(rawValue: NSWindow.Level.modalPanel.rawValue - 1)
}

// MARK: - 几何辅助（对齐 Windows overlay_ui_windows.cpp 的 RECT 工具族）

/// 两矩形并集的外包矩形；任一方为 nil/空时返回另一方（对齐 UnionRectSafe 的零矩形安全语义）。
func scUnionRect(_ a: CGRect?, _ b: CGRect?) -> CGRect? {
    switch (a, b) {
    case (nil, nil): return nil
    case (let x, nil): return x
    case (nil, let y): return y
    case (let x?, let y?): return x.union(y)
    }
}

/// 矩形外扩 margin（对齐 InflateRectBy）。
func scInflate(_ r: CGRect, _ margin: CGFloat) -> CGRect {
    return r.insetBy(dx: -margin, dy: -margin)
}

/// 点是否在矩形内（对齐 PointInRect 的 [left,right) 半开区间语义，CGRect.contains 同约定）。
func scPointInRect(_ p: CGPoint, _ r: CGRect) -> Bool {
    return p.x >= r.minX && p.x < r.maxX && p.y >= r.minY && p.y < r.maxY
}

// MARK: - 手柄命中与选区调整（对齐 Windows overlay_ui_windows.cpp / wndproc_windows.cpp）

/// 计算 8 个 resize 手柄的判定命中（overlay_ui_windows.cpp HitTestHandle 移植）。
/// - Parameters:
///   - point: 鼠标 CG 全局坐标
///   - selection: 当前选区（CG 全局坐标）
///   - handleSize: 命中半宽（SC.handleSize）
/// - Returns: 命中的手柄；未命中返回 .none
func scHitTestHandle(_ point: CGPoint, _ selection: CGRect, _ handleSize: CGFloat) -> ScreenshotResizeHandle {
    let cx = selection.midX
    let cy = selection.midY
    // 8 个手柄的判定矩形（顺序与 Windows ResizeHandle 一致）
    let tests: [(CGPoint, ScreenshotResizeHandle)] = [
        (CGPoint(x: selection.minX, y: cy), .left),
        (CGPoint(x: selection.maxX, y: cy), .right),
        (CGPoint(x: cx, y: selection.minY), .top),
        (CGPoint(x: cx, y: selection.maxY), .bottom),
        (CGPoint(x: selection.minX, y: selection.minY), .topLeft),
        (CGPoint(x: selection.maxX, y: selection.minY), .topRight),
        (CGPoint(x: selection.minX, y: selection.maxY), .bottomLeft),
        (CGPoint(x: selection.maxX, y: selection.maxY), .bottomRight),
    ]
    for (anchor, handle) in tests {
        let box = CGRect(x: anchor.x - handleSize, y: anchor.y - handleSize,
                         width: handleSize * 2, height: handleSize * 2)
        if scPointInRect(point, box) { return handle }
    }
    return .none
}

/// 圆角手柄中心位置：选区四角内侧，沿各自对角线内移 d = clamp(inset + radius, 0, maxR)
/// （overlay_ui_windows.cpp CornerRadiusHandleCenter 移植；radius 增大时四角同步向中心滑动）。
/// - Parameters:
///   - selection: 选区矩形
///   - inset: 静止内缩距离（SC.cornerKnobInset）
///   - radius: 当前选区圆角半径
///   - corner: 目标角手柄
/// - Returns: 手柄中心（CG 全局坐标）；corner 非角手柄时返回选区左上角
func scCornerKnobCenter(_ selection: CGRect, _ inset: CGFloat, _ radius: CGFloat,
                        _ corner: ScreenshotResizeHandle) -> CGPoint {
    let maxR = max(0, min(selection.width, selection.height) / 2)
    var d = inset + radius
    if d > maxR { d = maxR }   // 不越过中心 / 不出选区
    if d < 0 { d = 0 }
    switch corner {
    case .cornerTL: return CGPoint(x: selection.minX + d, y: selection.minY + d)
    case .cornerTR: return CGPoint(x: selection.maxX - d, y: selection.minY + d)
    case .cornerBL: return CGPoint(x: selection.minX + d, y: selection.maxY - d)
    case .cornerBR: return CGPoint(x: selection.maxX - d, y: selection.maxY - d)
    default: return CGPoint(x: selection.minX + d, y: selection.minY + d)
    }
}

/// 命中测试圆角手柄（overlay_ui_windows.cpp HitTestCornerRadiusHandle 移植）。
/// 命中框半宽沿用 handleSize；手柄位置随 radius 移动，故命中也按 radius 计算。
func scHitTestCornerKnob(_ point: CGPoint, _ selection: CGRect, _ handleSize: CGFloat,
                         _ inset: CGFloat, _ radius: CGFloat) -> ScreenshotResizeHandle {
    for corner in ScreenshotResizeHandle.cornerCases {
        let center = scCornerKnobCenter(selection, inset, radius, corner)
        let box = CGRect(x: center.x - handleSize, y: center.y - handleSize,
                         width: handleSize * 2, height: handleSize * 2)
        if scPointInRect(point, box) { return corner }
    }
    return .none
}

/// 找出鼠标"靠近"的圆角手柄（overlay_ui_windows.cpp FindNearestCornerRadiusHandle 移植）：
/// 感应半宽 = 命中半宽 + proximityMargin，切比雪夫距离取最近的一个角。
func scFindNearestCornerKnob(_ point: CGPoint, _ selection: CGRect, _ handleSize: CGFloat,
                             _ inset: CGFloat, _ radius: CGFloat,
                             _ proximityMargin: CGFloat) -> ScreenshotResizeHandle {
    let sense = handleSize + proximityMargin
    var best: ScreenshotResizeHandle = .none
    var bestDist = CGFloat.greatestFiniteMagnitude
    for corner in ScreenshotResizeHandle.cornerCases {
        let center = scCornerKnobCenter(selection, inset, radius, corner)
        let dx = abs(point.x - center.x)
        let dy = abs(point.y - center.y)
        if dx <= sense && dy <= sense {
            let dist = max(dx, dy)
            if dist < bestDist {
                bestDist = dist
                best = corner
            }
        }
    }
    return best
}

/// 取调整手柄在选区上的锚点（wndproc_windows.cpp GetResizeHandleAnchor 移植）：
/// 左右手柄取边中点，顶/底取中点，四角取角点；作为放大镜焦点。
func scGetResizeHandleAnchor(_ handle: ScreenshotResizeHandle, _ selection: CGRect) -> CGPoint {
    switch handle {
    case .left: return CGPoint(x: selection.minX, y: selection.midY)
    case .right: return CGPoint(x: selection.maxX, y: selection.midY)
    case .top: return CGPoint(x: selection.midX, y: selection.minY)
    case .bottom: return CGPoint(x: selection.midX, y: selection.maxY)
    case .topLeft: return CGPoint(x: selection.minX, y: selection.minY)
    case .topRight: return CGPoint(x: selection.maxX, y: selection.minY)
    case .bottomLeft: return CGPoint(x: selection.minX, y: selection.maxY)
    case .bottomRight: return CGPoint(x: selection.maxX, y: selection.maxY)
    default: return CGPoint(x: selection.midX, y: selection.midY)
    }
}

/// 钳制选区圆角半径到 [0, min(w,h)/2]（wndproc_windows.cpp ClampCornerRadius 移植）。
/// 选区尺寸变化（确认/调整/移动）后调用，避免半径越界导致手柄命中与渲染不一致。
func scClampCornerRadius(_ radius: CGFloat, _ selection: CGRect) -> CGFloat {
    let maxR = max(0, min(selection.width, selection.height) / 2)
    return min(max(radius, 0), maxR)
}

/// 约束单轴 resize 的活动端坐标（wndproc_windows.cpp ConstrainResizeActiveCoordinate 移植）。
/// 固定端始终保持按下时的位置；已有标注时优先保证内容不被裁掉（已接入标注包围盒）；
/// 松开时（enforceMinSize）按活动端当前所在侧补足最小尺寸，穿越后不带动固定端漂移。
/// - Parameters:
///   - rawActive/originalActive: 当前候选值与按下时的活动端坐标（同轴）
///   - fixed: 固定端坐标（同轴）
///   - screenMin/screenMax: 虚拟屏幕在该轴上的边界
///   - contentMin/contentMax: 标注内容包围盒在该轴上的边界（nil = 无内容约束，区域截图恒 nil）
///   - enforceMinSize: 仅松开时传 true（沿最终方向补足最小尺寸）
/// - Returns: 约束后的活动端坐标
private func scConstrainResizeActiveCoordinate(rawActive: CGFloat, originalActive: CGFloat, fixed: CGFloat,
                                               screenMin: CGFloat, screenMax: CGFloat,
                                               contentMin: CGFloat?, contentMax: CGFloat?,
                                               enforceMinSize: Bool) -> CGFloat {
    var active = max(screenMin, min(rawActive, screenMax))

    if let cMin = contentMin, let cMax = contentMax {
        if fixed <= cMin {
            // 内容位于固定端低侧：活动端必须覆盖内容高边，不能穿越后把内容留在选区外。
            active = max(active, cMax)
        } else if fixed >= cMax {
            // 内容位于固定端高侧：活动端必须覆盖内容低边。
            active = min(active, cMin)
        } else {
            // 固定端落在内容内部时不存在可完整覆盖内容的单侧区间，保留按下时活动端。
            active = originalActive
        }
        active = max(screenMin, min(active, screenMax))
    }

    if enforceMinSize {
        // 活动端恰好落在固定端时沿按下时的方向补足，避免释放后方向不确定。
        let onLowSide = active < fixed || (active == fixed && originalActive < fixed)
        if onLowSide {
            active = min(active, fixed - SC.minSelection)
        } else {
            active = max(active, fixed + SC.minSelection)
        }
        // 固定端靠近虚拟屏幕边缘时目标侧可能不足最小尺寸；边界优先且固定端不动。
        active = max(screenMin, min(active, screenMax))
    }

    return active
}

/// 从鼠标按下时的选区快照计算本帧 resize 结果（wndproc_windows.cpp ResizeSelectionFromHandle 移植）。
/// 每个活动轴只更新对应手柄端，固定边/固定对角点始终取 startSelection；活动端可穿过固定端，
/// 最后规范化。contentBounds 非空时限制活动端以保留已有标注（由会话传入真实标注包围盒）。
/// - Parameters:
///   - startSelection: 按下时的选区快照（CG 全局坐标）
///   - handle: 活动手柄
///   - dx/dy: 鼠标位移 + 键盘微调累计
///   - virtualBounds: 虚拟屏边界（钳制）
///   - contentBounds: 标注内容包围盒（nil = 无内容约束）
///   - enforceMinSize: 仅松开时传 true（沿最终方向补足最小尺寸）
/// - Returns: 本帧选区矩形（已规范化）
func scResizeSelectionFromHandle(_ startSelection: CGRect, _ handle: ScreenshotResizeHandle,
                                 _ dx: CGFloat, _ dy: CGFloat, _ virtualBounds: CGRect,
                                 _ contentBounds: CGRect?, _ enforceMinSize: Bool) -> CGRect {
    var resized = startSelection
    let standardContent = contentBounds?.standardized

    let movesLeft = handle == .left || handle == .topLeft || handle == .bottomLeft
    let movesRight = handle == .right || handle == .topRight || handle == .bottomRight
    let movesTop = handle == .top || handle == .topLeft || handle == .topRight
    let movesBottom = handle == .bottom || handle == .bottomLeft || handle == .bottomRight

    // 注意 RECT↔CGRect 语义差：Windows 直接改 left/top 而保持 right/bottom（宽度随之收缩）；
    // CGRect 改 origin 恒保持 size，故活动端在左/上时须同时收缩 size（固定边 = startSelection）。
    if movesLeft {
        let active = scConstrainResizeActiveCoordinate(
            rawActive: startSelection.minX + dx, originalActive: startSelection.minX, fixed: startSelection.maxX,
            screenMin: virtualBounds.minX, screenMax: virtualBounds.maxX,
            contentMin: standardContent?.minX, contentMax: standardContent?.maxX,
            enforceMinSize: enforceMinSize)
        resized.origin.x = active
        resized.size.width = startSelection.maxX - active
    } else if movesRight {
        resized.size.width = scConstrainResizeActiveCoordinate(
            rawActive: startSelection.maxX + dx, originalActive: startSelection.maxX, fixed: startSelection.minX,
            screenMin: virtualBounds.minX, screenMax: virtualBounds.maxX,
            contentMin: standardContent?.minX, contentMax: standardContent?.maxX,
            enforceMinSize: enforceMinSize) - resized.minX
    }

    if movesTop {
        let active = scConstrainResizeActiveCoordinate(
            rawActive: startSelection.minY + dy, originalActive: startSelection.minY, fixed: startSelection.maxY,
            screenMin: virtualBounds.minY, screenMax: virtualBounds.maxY,
            contentMin: standardContent?.minY, contentMax: standardContent?.maxY,
            enforceMinSize: enforceMinSize)
        resized.origin.y = active
        resized.size.height = startSelection.maxY - active
    } else if movesBottom {
        resized.size.height = scConstrainResizeActiveCoordinate(
            rawActive: startSelection.maxY + dy, originalActive: startSelection.maxY, fixed: startSelection.minY,
            screenMin: virtualBounds.minY, screenMax: virtualBounds.maxY,
            contentMin: standardContent?.minY, contentMax: standardContent?.maxY,
            enforceMinSize: enforceMinSize) - resized.minY
    }

    return resized.standardized
}

// MARK: - 浮层位置计算（对齐 Windows overlay_ui_windows.cpp Calc* 系列）

/// 计算放大镜面板位置（overlay_ui_windows.cpp CalcPanelPosition 移植）：
/// 优先鼠标右下，超出虚拟屏则翻转，仍越界贴屏边。
/// - Parameters:
///   - mouse: 鼠标 CG 全局坐标
///   - virtual: 虚拟屏并集（CG 全局逻辑坐标）
/// - Returns: 面板矩形（CG 全局逻辑坐标，SC.panelSize 边长）
func scCalcPanelRect(_ mouse: CGPoint, _ virtual: CGRect) -> CGRect {
    var px = mouse.x + SC.panelMargin
    var py = mouse.y + SC.panelMargin
    if px + SC.panelSize > virtual.maxX { px = mouse.x - SC.panelSize - SC.panelMargin }
    if py + SC.panelSize > virtual.maxY { py = mouse.y - SC.panelSize - SC.panelMargin }
    if px < virtual.minX { px = virtual.minX + SC.panelMargin }
    if py < virtual.minY { py = virtual.minY + SC.panelMargin }
    return CGRect(x: px, y: py, width: SC.panelSize, height: SC.panelSize)
}

/// 调整选区时放大镜面板位置（overlay_ui_windows.cpp CalcResizePanelPosition 移植）：
/// 放在被拖手柄的"外侧"避免遮挡选区；外侧放不下翻对侧，对侧仍放不下贴屏边。
/// - Parameters:
///   - handle: 活动手柄
///   - selection: 当前选区
///   - virtual: 虚拟屏并集
/// - Returns: 面板矩形（CG 全局逻辑坐标）
func scCalcResizePanelRect(_ handle: ScreenshotResizeHandle, _ selection: CGRect, _ virtual: CGRect) -> CGRect {
    let movesL = handle == .left || handle == .topLeft || handle == .bottomLeft
    let movesR = handle == .right || handle == .topRight || handle == .bottomRight
    let movesT = handle == .top || handle == .topLeft || handle == .topRight
    let movesB = handle == .bottom || handle == .bottomLeft || handle == .bottomRight
    var px: CGFloat
    var py: CGFloat
    if movesL {
        px = selection.minX - SC.panelSize - SC.panelMargin
    } else if movesR {
        px = selection.maxX + SC.panelMargin
    } else {
        px = selection.midX - SC.panelSize / 2
    }
    if movesT {
        py = selection.minY - SC.panelSize - SC.panelMargin
    } else if movesB {
        py = selection.maxY + SC.panelMargin
    } else {
        py = selection.midY - SC.panelSize / 2
    }
    // 外侧放不下 -> 翻到对侧；对侧仍放不下 -> 贴屏边
    if px < virtual.minX { px = selection.maxX + SC.panelMargin }
    if px + SC.panelSize > virtual.maxX { px = selection.minX - SC.panelSize - SC.panelMargin }
    if px < virtual.minX { px = virtual.minX + SC.panelMargin }
    if px + SC.panelSize > virtual.maxX { px = virtual.maxX - SC.panelSize - SC.panelMargin }
    if py < virtual.minY { py = selection.maxY + SC.panelMargin }
    if py + SC.panelSize > virtual.maxY { py = selection.minY - SC.panelSize - SC.panelMargin }
    if py < virtual.minY { py = virtual.minY + SC.panelMargin }
    if py + SC.panelSize > virtual.maxY { py = virtual.maxY - SC.panelSize - SC.panelMargin }
    return CGRect(x: px, y: py, width: SC.panelSize, height: SC.panelSize)
}

/// 计算尺寸标签矩形（overlay_ui_windows.cpp DrawSizeLabel 的定位部分抽取为纯几何函数，
/// 绘制与失效共用同一几何，保证脏区与实际绘制一致）：默认在参考矩形上方，放不下翻到
/// 参考矩形内左上角，再钳制在虚拟屏内。
/// - Parameters:
///   - text: 标签文本（如 "1280 × 800"）
///   - refRect: 参考矩形（窗口/选区，CG 全局坐标）
///   - virtual: 虚拟屏并集
/// - Returns: 标签矩形；文本为空返回 nil
func scCalcSizeLabelRect(_ text: String, _ refRect: CGRect, _ virtual: CGRect) -> CGRect? {
    guard !text.isEmpty else { return nil }
    let textSize = scLabelAttributedString(text).size()
    let labelW = ceil(textSize.width) + SC.sizeLabelPadX * 2
    let labelH = ceil(textSize.height) + SC.sizeLabelPadY

    var lx = refRect.minX
    var ly = refRect.minY - labelH - SC.sizeLabelGap
    if ly < virtual.minY {
        lx = refRect.minX + SC.sizeLabelGap
        ly = refRect.minY + SC.sizeLabelGap
        if lx + labelW > virtual.maxX { lx = virtual.maxX - labelW - SC.sizeLabelGap }
        if ly + labelH > virtual.maxY { ly = virtual.maxY - labelH - SC.sizeLabelGap }
        if lx + labelW > refRect.maxX { lx = refRect.maxX - labelW - SC.sizeLabelGap }
        if ly + labelH > refRect.maxY { ly = refRect.maxY - labelH - SC.sizeLabelGap }
    }
    if lx < virtual.minX { lx = virtual.minX }
    if ly < virtual.minY { ly = virtual.minY }
    if lx + labelW > virtual.maxX { lx = virtual.maxX - labelW }
    if ly + labelH > virtual.maxY { ly = virtual.maxY - labelH }
    return CGRect(x: lx, y: ly, width: labelW, height: labelH)
}

/// 尺寸标签/信息面板共用的白色文字属性串（12px 系统字体，对齐 Windows smallFont 基准）。
func scLabelAttributedString(_ text: String) -> NSAttributedString {
    return NSAttributedString(string: text, attributes: [
        .font: NSFont.systemFont(ofSize: SC.fontPx),
        .foregroundColor: NSColor.white,
    ])
}

// MARK: - 覆盖层绘制（ScreenshotOverlaySession 的绘制扩展；对齐 overlay_paint_windows.cpp）

extension ScreenshotOverlaySession {
    /// 覆盖层绘制主入口（由每个覆盖层 NSView 的 draw(_:) 调用）。
    /// 流程对齐 Windows OnPaint：先按脏区从常驻底图 CGImage 裁剪恢复背景（拖拽全程禁止
    /// 整图重采样），再按状态绘制覆盖层内容（AppKit 已把上下文裁剪到脏区）。
    /// - Parameters:
    ///   - ctx: 视图 CG 上下文（视图已翻转，本地坐标与 CG 同向、Y 向下）
    ///   - view: 当前绘制的覆盖层视图
    ///   - dirtyLocal: 脏区（视图本地坐标）
    func paint(context ctx: CGContext, view: OverlayScreenshotView, dirtyLocal: CGRect) {
        // 本帧脏区 → CG 全局逻辑坐标（本地与 CG 同向，平移换算），钳制到本屏与虚拟屏
        let dirtyCG = dirtyLocal
            .offsetBy(dx: view.cgOrigin.x, dy: view.cgOrigin.y)
            .intersection(view.cgFrame)
            .intersection(virtualBounds)
        guard !dirtyCG.isNull, dirtyCG.width > 0, dirtyCG.height > 0 else { return }

        // 1) 背景恢复：按脏区从底图裁剪绘制
        drawBaseImage(ctx: ctx, view: view, dirtyCG: dirtyCG)

        // 2) 覆盖层内容（按状态）
        switch state {
        case .idle:
            paintIdleOverlay(ctx: ctx, view: view)
        case .selecting:
            paintSelectingOverlay(ctx: ctx, view: view)
        case .confirmed, .resizing, .moving, .drawing, .textEditing:
            paintConfirmedOverlay(ctx: ctx, view: view)
        default:
            break
        }
    }

    // ---- 底图 ----

    /// 把底图 CGImage 中与脏区对应的部分绘制到视图（ScreenshotPaint 底图恢复步骤）。
    /// 脏区映射为底图物理像素矩形 → cropping 裁剪（写时复制，无整图重采样）→ 1:1 绘制。
    private func drawBaseImage(ctx: CGContext, view: OverlayScreenshotView, dirtyCG: CGRect) {
        let scale = baseFrame.scale
        let origin = baseFrame.origin
        let imgW = CGFloat(baseFrame.image.width)
        let imgH = CGFloat(baseFrame.image.height)
        // 脏区 → 底图物理像素（左上原点），四边钳制在位图内
        let px0 = max(0, ((dirtyCG.minX - origin.x) * scale).rounded(.down))
        let py0 = max(0, ((dirtyCG.minY - origin.y) * scale).rounded(.down))
        let px1 = min(imgW, ((dirtyCG.maxX - origin.x) * scale).rounded(.up))
        let py1 = min(imgH, ((dirtyCG.maxY - origin.y) * scale).rounded(.up))
        guard px1 > px0, py1 > py0 else { return }

        guard let cropped = baseFrame.image.cropping(to: CGRect(x: px0, y: py0, width: px1 - px0, height: py1 - py0)) else { return }
        // 物理裁剪区左上角对应的 CG 点 → 视图本地坐标（视图与 CG 同向，平移即可）
        let dstRect = CGRect(
            x: (origin.x + px0 / scale) - view.cgOrigin.x,
            y: (origin.y + py0 / scale) - view.cgOrigin.y,
            width: (px1 - px0) / scale,
            height: (py1 - py0) / scale)
        ctx.interpolationQuality = .none   // 同分辨率 blit（单屏等 scale 时物理像素 1:1 对齐）
        scDrawCGImage(ctx, cropped, in: dstRect)
    }

    /// 在已翻转（top-left 原点）的视图上下文中绘制 CGImage。
    /// CGImage 行序为 top-down 而 CGContextDrawImage 按未翻转坐标绘制，需翻转 Y 轴避免上下颠倒。
    /// （跨文件共享：马赛克 base 揭示与导出合成复用。）
    func scDrawCGImage(_ ctx: CGContext, _ image: CGImage, in rect: CGRect) {
        ctx.saveGState()
        ctx.translateBy(x: rect.midX, y: rect.midY)
        ctx.scaleBy(x: 1.0, y: -1.0)
        ctx.draw(image, in: CGRect(x: -rect.width / 2, y: -rect.height / 2, width: rect.width, height: rect.height))
        ctx.restoreGState()
    }

    // ---- Idle 态（hover 高亮 + 尺寸标签 + 放大镜）----

    /// Idle 态绘制（对齐 OnPaint CS_Idle 分支）：hover 窗口 3px 蓝框高亮 + 尺寸标签（W×H）
    /// + 跟随放大镜面板；无命中窗口时高亮/标注鼠标所在屏幕（Windows 的 MonitorFromPoint 退化）。
    private func paintIdleOverlay(ctx: CGContext, view: OverlayScreenshotView) {
        let rects = idleOverlayRects()
        if let highlight = rects.highlight {
            scStrokeRect(ctx, highlight.offsetBy(dx: -view.cgOrigin.x, dy: -view.cgOrigin.y),
                         color: SC.accentBlue, width: 3)   // 对齐 DrawWindowHighlight 的 3px highlightPen
        }
        if let label = rects.label {
            paintSizeLabel(ctx: ctx, view: view, rect: label, text: rects.labelText)
        }
        paintInfoPanel(ctx: ctx, view: view, panelRect: rects.panel,
                       focus: mouse, positionText: "\(Int(mouse.x.rounded())), \(Int(mouse.y.rounded()))")
    }

    // ---- Selecting 态（蒙版 + 选区边框 + 尺寸标签 + 放大镜）----

    /// Selecting 态绘制（对齐 OnPaint 非 confirmedMode 分支）：暗化蒙版（直角）+ 1px 蓝选区框
    /// + 尺寸标签（W×H）+ 跟随放大镜面板。
    private func paintSelectingOverlay(ctx: CGContext, view: OverlayScreenshotView) {
        let dragRect = currentDragRect()
        paintDimMask(ctx: ctx, view: view, selection: dragRect, radius: selectionCornerRadius)
        // 选区边框（对齐 DrawSelection 的 selectionPen 1px 蓝）
        scStrokeRect(ctx, dragRect.offsetBy(dx: -view.cgOrigin.x, dy: -view.cgOrigin.y),
                     color: SC.accentBlue, width: 1)
        let text = "\(Int(dragRect.width.rounded())) × \(Int(dragRect.height.rounded()))"
        if let labelRect = scCalcSizeLabelRect(text, dragRect, virtualBounds) {
            paintSizeLabel(ctx: ctx, view: view, rect: labelRect, text: text)
        }
        paintInfoPanel(ctx: ctx, view: view, panelRect: scCalcPanelRect(mouse, virtualBounds),
                       focus: mouse, positionText: "\(Int(mouse.x.rounded())), \(Int(mouse.y.rounded()))")
    }

    // ---- 确认/调整/移动态 ----

    /// 确认态族绘制（对齐 OnPaint confirmedMode 分支）：蒙版（含圆角角帽）+ 马赛克揭示层
    /// + 已提交/进行中标注 + 选中标注视觉 + 确认边框 + 8 个 resize 手柄 + 圆角手柄
    /// （靠近/拖拽时）+ 文字编辑层（编辑态）+ Resizing 标准手柄时的放大镜。
    /// 绘制/整体拖动中工具栏保持显示（独立浮层窗口，由 pumpTick 同步显隐），调整中隐藏。
    private func paintConfirmedOverlay(ctx: CGContext, view: OverlayScreenshotView) {
        paintDimMask(ctx: ctx, view: view, selection: selection, radius: selectionCornerRadius)

        // 马赛克揭示层（reveal-mask；在矢量/文字标注之下，对齐 OnPaint 的
        // RevealMosaicToTarget → DrawAnnotations 次序）
        paintMosaicLayer(ctx: ctx, view: view)

        // 已提交标注 + 正在绘制的标注（裁剪到选区内；调整选区时保持显示便于看清内容
        // 是否会被裁掉——对齐 OnPaint 的 Confirmed/Drawing/Resizing 三态绘制）
        paintAnnotationsLayer(ctx: ctx, view: view)

        // 文字编辑层：输入文字 + 边框 + 选择高亮 + 组词下划线 + 插入符
        //（对齐 OnPaint CS_TextEditing 分支）
        paintTextEditingLayer(ctx: ctx, view: view)

        // 确认边框（对齐 DrawConfirmedBorder：radius≥1 用圆角路径，否则 1px 直角框）
        paintConfirmedBorder(ctx: ctx, view: view)

        let draggingCorner = state == .resizing && resizeHandle.isCorner
        // 选区 resize 手柄：确认/调整/移动中显示；倒角手柄拖拽时隐藏（对齐 OnPaint 逻辑）
        if !draggingCorner {
            paintResizeHandles(ctx: ctx, view: view)
        }
        // 圆角手柄：默认隐藏，仅"鼠标靠近某角"（确认态）或"正拖拽某角"（Resizing）时显示该角一个
        var visibleCorner: ScreenshotResizeHandle = .none
        if draggingCorner {
            visibleCorner = resizeHandle
        } else if state == .confirmed {
            visibleCorner = hoveredCornerHandle
        }
        if visibleCorner != .none {
            paintCornerKnob(ctx: ctx, view: view, corner: visibleCorner)
        }

        // 调整选区（标准手柄）时显示放大镜：焦点取活动手柄锚点，面板置于选区外侧
        // （对齐 OnPaint CS_Resizing 分支的 CalcResizePanelPosition + DrawInfoPanel）
        if state == .resizing && !resizeHandle.isCorner {
            let anchor = scGetResizeHandleAnchor(resizeHandle, selection)
            let panelRect = scCalcResizePanelRect(resizeHandle, selection, virtualBounds)
            paintInfoPanel(ctx: ctx, view: view, panelRect: panelRect, focus: anchor,
                           positionText: "\(Int(anchor.x.rounded())), \(Int(anchor.y.rounded()))")
        }
    }

    // ---- 蒙版（DrawDimMask 移植）----

    /// 选区外暗化蒙版（overlay_paint_windows.cpp DrawDimMask 移植）：全黑 alpha 120/255 分
    /// 四块绘制（上/下/左/右，选区内保持清晰），radius>0 时用「方框−圆角框」偶奇填充补
    /// 四角"角帽"——与 Windows capPath（外两条直边 + 内凹四分之一弧）逐像素等价。
    private func paintDimMask(ctx: CGContext, view: OverlayScreenshotView, selection sel: CGRect, radius: CGFloat) {
        guard sel.width > 0, sel.height > 0 else { return }
        let dark = NSColor.black.withAlphaComponent(SC.maskAlpha).cgColor
        let local = { (r: CGRect) in r.offsetBy(dx: -view.cgOrigin.x, dy: -view.cgOrigin.y) }

        // 四块蒙版（等价 Windows 的四段 AlphaBlend：上/下全宽，左/右夹在选区上下之间）
        let top = CGRect(x: virtualBounds.minX, y: virtualBounds.minY,
                         width: virtualBounds.width, height: sel.minY - virtualBounds.minY)
        let bottom = CGRect(x: virtualBounds.minX, y: sel.maxY,
                            width: virtualBounds.width, height: virtualBounds.maxY - sel.maxY)
        let left = CGRect(x: virtualBounds.minX, y: sel.minY,
                          width: sel.minX - virtualBounds.minX, height: sel.height)
        let right = CGRect(x: sel.maxX, y: sel.minY,
                           width: virtualBounds.maxX - sel.maxX, height: sel.height)
        ctx.setFillColor(dark)
        for block in [top, bottom, left, right] where block.width > 0 && block.height > 0 {
            ctx.fill(local(block))
        }

        // 圆角角帽：方角框与圆角框的对称差 = 四个角帽区域（偶奇填充，弧边抗锯齿）。
        // 半径钳制不超过短边一半（对齐 DrawDimMask 的 min(radius, min(w,h)/2)）。
        let r = min(radius, min(sel.width, sel.height) / 2)
        guard r >= 1 else { return }
        let path = CGMutablePath()
        path.addRect(CGRect(x: 0, y: 0, width: sel.width, height: sel.height))
        path.addPath(CGPath(roundedRect: CGRect(x: 0, y: 0, width: sel.width, height: sel.height),
                            cornerWidth: r, cornerHeight: r, transform: nil))
        ctx.saveGState()
        ctx.translateBy(x: local(sel).minX, y: local(sel).minY)
        ctx.addPath(path)
        ctx.fillPath(using: .evenOdd)
        ctx.restoreGState()
    }

    // ---- 边框与手柄 ----

    /// 确认态选区边框（overlay_ui_windows.cpp DrawConfirmedBorder 移植）：
    /// radius<1 为 1px 直角蓝框；radius≥1 为圆角路径 1px 蓝框（抗锯齿）。
    private func paintConfirmedBorder(ctx: CGContext, view: OverlayScreenshotView) {
        let localSel = selection.offsetBy(dx: -view.cgOrigin.x, dy: -view.cgOrigin.y)
        if selectionCornerRadius < 1 {
            scStrokeRect(ctx, localSel, color: SC.accentBlue, width: 1)
            return
        }
        let r = min(selectionCornerRadius, min(localSel.width, localSel.height) / 2)
        let path = CGPath(roundedRect: localSel, cornerWidth: max(1, r), cornerHeight: max(1, r), transform: nil)
        ctx.addPath(path)
        ctx.setStrokeColor(SC.accentBlue.cgColor)
        ctx.setLineWidth(1)
        ctx.strokePath()
    }

    /// 8 个选区调整手柄（overlay_ui_windows.cpp DrawResizeHandles 移植）：
    /// 蓝色方块 + 白色 1px 描边，抗锯齿。
    private func paintResizeHandles(ctx: CGContext, view: OverlayScreenshotView) {
        let hs = SC.handleSize
        let half = hs / 2
        let localSel = selection.offsetBy(dx: -view.cgOrigin.x, dy: -view.cgOrigin.y)
        let cx = localSel.midX
        let cy = localSel.midY
        let anchors: [CGPoint] = [
            CGPoint(x: localSel.minX, y: cy), CGPoint(x: localSel.maxX, y: cy),
            CGPoint(x: cx, y: localSel.minY), CGPoint(x: cx, y: localSel.maxY),
            CGPoint(x: localSel.minX, y: localSel.minY), CGPoint(x: localSel.maxX, y: localSel.minY),
            CGPoint(x: localSel.minX, y: localSel.maxY), CGPoint(x: localSel.maxX, y: localSel.maxY),
        ]
        let path = CGMutablePath()
        for a in anchors {
            path.addRect(CGRect(x: a.x - half, y: a.y - half, width: hs, height: hs))
        }
        ctx.addPath(path)
        ctx.setFillColor(SC.accentBlue.cgColor)
        ctx.fillPath()
        ctx.addPath(path)
        ctx.setStrokeColor(NSColor.white.cgColor)
        ctx.setLineWidth(1)
        ctx.strokePath()
    }

    /// 单个圆角拖拽手柄（overlay_ui_windows.cpp DrawCornerRadiusHandle 移植）：
    /// 白底圆 + 蓝环 + 朝向选区角的四分之一弧点缀；中心沿对角线内移 d = clamp(inset+radius, 0, maxR)。
    private func paintCornerKnob(ctx: CGContext, view: OverlayScreenshotView, corner: ScreenshotResizeHandle) {
        let half = SC.handleSize / 2
        let gr = max(1, half - 2)
        let centerCG = scCornerKnobCenter(selection, SC.cornerKnobInset, selectionCornerRadius, corner)
        let center = CGPoint(x: centerCG.x - view.cgOrigin.x, y: centerCG.y - view.cgOrigin.y)
        let circle = CGPath(ellipseIn: CGRect(x: center.x - half, y: center.y - half,
                                              width: half * 2, height: half * 2), transform: nil)
        ctx.addPath(circle)
        ctx.setFillColor(NSColor.white.cgColor)
        ctx.fillPath()
        ctx.addPath(circle)
        ctx.setStrokeColor(SC.accentBlue.cgColor)
        ctx.setLineWidth(1.5)
        ctx.strokePath()

        // 四分之一弧点缀（朝向所在选区角）。用显式参数化的折线逼近弧段，避免翻转坐标系下
        // CG 弧方向歧义；r 从 0..π/2 采样，四角按象限镜像（视觉与 Windows DrawArc 一致）。
        let steps = 10
        let path = CGMutablePath()
        var first = true
        for i in 0...steps {
            let t = CGFloat(i) / CGFloat(steps)
            let cosT = cos(t * .pi / 2)
            let sinT = sin(t * .pi / 2)
            let dxSign: CGFloat
            let dySign: CGFloat
            switch corner {
            case .cornerTL: dxSign = -1; dySign = -1
            case .cornerTR: dxSign = 1; dySign = -1
            case .cornerBL: dxSign = -1; dySign = 1
            case .cornerBR: dxSign = 1; dySign = 1
            default: dxSign = -1; dySign = -1
            }
            let p = CGPoint(x: center.x + dxSign * gr * cosT, y: center.y + dySign * gr * sinT)
            if first {
                path.move(to: p)
                first = false
            } else {
                path.addLine(to: p)
            }
        }
        ctx.addPath(path)
        ctx.setStrokeColor(SC.accentBlue.cgColor)
        ctx.setLineWidth(1)
        ctx.strokePath()
    }

    // ---- 尺寸标签与信息面板 ----

    /// 尺寸标签（overlay_ui_windows.cpp DrawSizeLabel 移植）：深色圆角底 + 灰描边 + 白字。
    /// 定位几何与失效计算共用 scCalcSizeLabelRect（保证脏区与绘制一致）。
    private func paintSizeLabel(ctx: CGContext, view: OverlayScreenshotView, rect: CGRect, text: String) {
        let local = rect.offsetBy(dx: -view.cgOrigin.x, dy: -view.cgOrigin.y)
        let path = CGPath(roundedRect: local, cornerWidth: SC.panelCornerRadius,
                          cornerHeight: SC.panelCornerRadius, transform: nil)
        ctx.addPath(path)
        ctx.setFillColor(SC.panelBg.cgColor)
        ctx.fillPath()
        ctx.addPath(path)
        ctx.setStrokeColor(SC.panelBorder.cgColor)
        ctx.setLineWidth(1)
        ctx.strokePath()

        let attr = scLabelAttributedString(text)
        let textSize = attr.size()
        attr.draw(at: NSPoint(x: local.minX + SC.sizeLabelPadX,
                              y: local.minY + SC.sizeLabelPadY + (textSize.height - SC.fontPx) / 2))
    }

    /// 放大镜 + 坐标/HEX/RGB 信息面板（overlay_ui_windows.cpp DrawInfoPanel 移植）。
    /// 放大镜从常驻底图 CGImage 按物理像素裁剪采样（禁止整图重采样），4× 就近放大
    ///（对齐 Windows StretchBlt 的像素放大视觉），叠加 1px 蓝色十字准星。
    /// - Parameters:
    ///   - panelRect: 面板矩形（CG 全局坐标，140×140）
    ///   - focus: 采样焦点（鼠标或活动手柄锚点，CG 全局坐标）
    ///   - positionText: 坐标行文本
    private func paintInfoPanel(ctx: CGContext, view: OverlayScreenshotView,
                                panelRect: CGRect, focus: CGPoint, positionText: String) {
        let local = panelRect.offsetBy(dx: -view.cgOrigin.x, dy: -view.cgOrigin.y)
        let path = CGPath(roundedRect: local, cornerWidth: SC.panelCornerRadius,
                          cornerHeight: SC.panelCornerRadius, transform: nil)
        ctx.addPath(path)
        ctx.setFillColor(SC.panelBg.cgColor)
        ctx.fillPath()
        ctx.addPath(path)
        ctx.setStrokeColor(SC.panelBorder.cgColor)
        ctx.setLineWidth(1)
        ctx.strokePath()

        // 放大镜区域（对齐 DrawInfoPanel：magW = w - 2*borderPad，magH = magnifierH - borderPad）
        let magRect = CGRect(x: local.minX + SC.panelBorderPad,
                             y: local.minY + SC.panelBorderPad,
                             width: local.width - SC.panelBorderPad * 2,
                             height: SC.magnifierHeight - SC.panelBorderPad)

        // 采样窗口 = 放大区 / 倍数（Windows int 除法语义：140/4=35、74/4=18）
        let srcW = (SC.panelSize / SC.zoomFactor).rounded(.down)
        let srcH = (SC.magnifierHeight / SC.zoomFactor).rounded(.down)
        if let sample = cropBaseImage(around: focus, srcLogicalW: srcW, srcLogicalH: srcH) {
            ctx.saveGState()
            // 圆角面板内裁剪，避免放大图溢出面板圆角
            ctx.addPath(CGPath(roundedRect: magRect, cornerWidth: 2, cornerHeight: 2, transform: nil))
            ctx.clip()
            ctx.interpolationQuality = .none   // 就近放大，保留像素感（对齐 StretchBlt 视觉）
            scDrawCGImage(ctx, sample, in: magRect)
            ctx.restoreGState()
        }

        // 蓝色十字准星（对齐 crosshairPen：1px，横纵贯穿放大区）
        ctx.setStrokeColor(SC.accentBlue.cgColor)
        ctx.setLineWidth(1)
        ctx.move(to: CGPoint(x: magRect.minX, y: magRect.midY))
        ctx.addLine(to: CGPoint(x: magRect.maxX, y: magRect.midY))
        ctx.move(to: CGPoint(x: magRect.midX, y: magRect.minY))
        ctx.addLine(to: CGPoint(x: magRect.midX, y: magRect.maxY))
        ctx.strokePath()

        // 三行信息（对齐 DrawInfoPanel：左标签 + 右对齐值）
        let color = currentColor
        let lines: [(String, String)] = [
            ("坐标", positionText),
            ("HEX", String(format: "#%02X%02X%02X", color.r, color.g, color.b)),
            ("RGB", "\(color.r), \(color.g), \(color.b)"),
        ]
        let lineH = SC.fontPx + 3
        let infoY = local.maxY - SC.panelLabelPad - lineH * 3
        let labelX = local.minX + SC.panelLabelPad
        let valueRightX = local.maxX - SC.panelLabelPad
        for (i, line) in lines.enumerated() {
            let y = infoY + CGFloat(i) * lineH
            scLabelAttributedString(line.0).draw(at: NSPoint(x: labelX, y: y))
            let value = scLabelAttributedString(line.1)
            let valueSize = value.size()
            value.draw(at: NSPoint(x: valueRightX - ceil(valueSize.width), y: y))
        }
    }
}

// MARK: - 基础绘制小工具

/// 1px/3px 矩形描边（对齐 GDI Rectangle 描边；width 为线宽）。
private func scStrokeRect(_ ctx: CGContext, _ rect: CGRect, color: NSColor, width: CGFloat) {
    ctx.setStrokeColor(color.cgColor)
    ctx.setLineWidth(width)
    ctx.stroke(rect)
}
