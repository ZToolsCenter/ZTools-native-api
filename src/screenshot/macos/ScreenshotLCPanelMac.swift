import Foundation
import AppKit
import CoreGraphics

// MARK: - 长截图小地图面板（macOS；
//         Windows 基准 lc_panel_ui_windows.cpp）
//
// 独立 NSWindow 承载只读预览（Windows 为 WS_EX_LAYERED 分层弹窗 + UpdateLayeredWindow
// 原子提交；macOS 为 borderless 透明自绘窗口 + setNeedsDisplay，刷新时机对齐）：
// - 深色圆角底 RGB(52,52,53)、1px 描边 RGB(102,102,102)、圆角半径 8（LongCapturePanelRender）
// - 两级增量缩略列：算法层按固定列宽增量维护缩略缓冲（lc_read_thumb_rows 对齐
//   LongCaptureRebuildThumb 的合并序——reverse(headRev) + body；横向模式回转为显示空间，
//   对齐 LongCaptureRebuildThumbDisplay），面板只按裁剪行窗口重采样绘制
// - 三层视口标注：灰外环 = 已捕获完整范围；蓝实线 = committed（最新提交帧精确位置，
//   60/255 半透明衬底）；橙虚线 = tentative（预计视口位置，|tentative−committed| ≥
//   LC_TRACK_MIN_STEP 才显示，36/255 半透明衬底，画4空3 虚线段）
// - 停靠退化：选区右侧 → 左侧 → 下方 → 上方（LongCaptureCreatePanel 退化链）
// - 高度只增不减、上限屏高 45%（LongCapturePanelUpdate），且避让长截图工具栏与选区顶边
//
// 刷新时机（LongCapturePanelUpdate / InvalidateRect 对齐）：拼接成功（contentChanged =
// 尺寸重算 + 重绘）、tentative 跟踪变化（trackingChanged = 仅重绘）、裁剪应用
//（contentChanged）。全部由泵循环在主线程驱动（CATransaction 隐式提交）。

// MARK: - 面板视图

/// 小地图自绘视图：把事件隔离（只读预览），绘制委托控制器完成。
final class ScreenshotLCPanelView: NSView {
    unowned let controller: ScreenshotLCPanelController

    init(controller: ScreenshotLCPanelController, frame: NSRect) {
        self.controller = controller
        super.init(frame: frame)
    }

    required init?(coder: NSCoder) {
        fatalError("ScreenshotLCPanelView is created programmatically only")
    }

    override var isFlipped: Bool { return true }   // 本地坐标与 CG 全局坐标同向

    override func draw(_ dirtyRect: NSRect) {
        guard let ctx = NSGraphicsContext.current?.cgContext else { return }
        controller.render(ctx)
    }
}

// MARK: - 面板控制器

/// 小地图面板控制器：持有面板窗口与停靠/生长状态，渲染几何与视口框换算全部对齐
/// lc_panel_ui_windows.cpp（LongCapturePanelPreviewRect / LongCaptureViewportRectAt /
/// LongCapturePanelRender / LongCapturePanelUpdate / LongCaptureCreatePanel）。
final class ScreenshotLCPanelController {
    private weak var session: ScreenshotLongCaptureSession?

    /// 面板窗口（长截图浮层族：level 高于蒙版，保证「蒙版之下」抓帧只含用户内容）。
    private(set) var window: ScreenshotPanelWindow?
    private var view: ScreenshotLCPanelView?
    /// 面板矩形（CG 全局逻辑坐标；顶边锚定、向下生长）。
    var panelRect: CGRect = .null
    /// 面板是否停靠在选区上方（水平无空间退化链的产物；生长不得越过选区顶边防入画）。
    var panelAbove = false

    /// 面板窗口内容色（对齐 LongCapturePanelRender：底 RGB(52,52,53)、描边 RGB(102,102,102)）。
    private static let bgColor = NSColor(srgbRed: 52 / 255.0, green: 52 / 255.0, blue: 53 / 255.0, alpha: 1)
    private static let borderColor = NSColor(srgbRed: 102 / 255.0, green: 102 / 255.0, blue: 102 / 255.0, alpha: 1)
    /// 三层视口标注色（kRingC/kBlueC/kOrngC，LongCapturePanelRender）。
    private static let ringColor = NSColor(srgbRed: 190 / 255.0, green: 190 / 255.0, blue: 195 / 255.0, alpha: 1)
    private static let committedColor = NSColor(srgbRed: 0x2F / 255.0, green: 0x7E / 255.0, blue: 0xE5 / 255.0, alpha: 1)
    private static let tentativeColor = NSColor(srgbRed: 0xE8 / 255.0, green: 0xA3 / 255.0, blue: 0x3C / 255.0, alpha: 1)

    init(session: ScreenshotLongCaptureSession) {
        self.session = session
    }

    // MARK: 生命周期

    /// 创建面板窗口：停靠选区右侧（空间不足退左侧，再退化到选区下方/上方），初始高度按
    /// 选区采样裁剪等比（对齐 LongCaptureCreatePanel；45% 屏高上限同步生效）。
    func create() {
        guard let session = session, panelRect.isNull, window == nil else { return }
        let pad = LC_PANEL_PAD_PX
        let margin: CGFloat = 12
        let winW = LC_PANEL_WIDTH
        // 首帧即整个选区：按采样裁剪等比计算初始预览高，避免面板先闪空再放大
        let selW = max(1, session.cropRect.width)
        let selH = max(1, session.cropRect.height)
        let scale0 = (winW - pad * 2) / selW
        var prevH0 = (selH * scale0).rounded()
        let capH = session.virtualBounds.height * LC_PANEL_MAX_HEIGHT_RATIO
        if prevH0 > capH { prevH0 = capH }
        let winH = pad * 2 + prevH0

        let virtual = session.virtualBounds
        // 停靠退化链：右 → 左 → 下 → 上
        var x = session.selection.maxX + margin
        var y = session.selection.minY
        if x + winW > virtual.maxX {
            x = session.selection.minX - winW - margin
        }
        if x < virtual.minX {
            // 水平无空间（选区接近全屏宽）：退化为选区下方/上方，避免面板覆盖选区入画
            x = min(max(virtual.minX + 4, session.selection.midX - winW / 2),
                    virtual.maxX - winW - 4)
            y = session.selection.maxY + margin
            if y + winH > virtual.maxY {
                y = session.selection.minY - winH - margin
                panelAbove = true
            }
        }
        if y + winH > virtual.maxY { y = virtual.maxY - winH - 4 }
        if y < virtual.minY { y = virtual.minY + 4 }
        panelRect = CGRect(x: x, y: y, width: winW, height: winH)

        let win = ScreenshotPanelWindow(
            contentRect: lcNSRect(fromCG: panelRect), styleMask: .borderless,
            backing: .buffered, defer: false)
        // 层级 = 蒙版 +1（蒙版 screenSaver+1）：面板在蒙版之上、抓帧排除范围之内
        win.level = NSWindow.Level(rawValue: NSWindow.Level.screenSaver.rawValue + 2)
        win.isOpaque = false
        win.backgroundColor = .clear
        win.hasShadow = false
        win.collectionBehavior = [.canJoinAllSpaces, .fullScreenAuxiliary]
        win.isReleasedWhenClosed = false
        let panelView = ScreenshotLCPanelView(
            controller: self, frame: NSRect(origin: .zero, size: panelRect.size))
        win.contentView = panelView
        window = win
        view = panelView
        win.orderFrontRegardless()
    }

    /// 销毁面板窗口（长截图收束/取消时调用；幂等）。
    func destroy() {
        window?.orderOut(nil)
        window?.contentView = nil
        window = nil
        view = nil
        panelRect = .null
        panelAbove = false
    }

    // MARK: 刷新入口

    /// 拼接内容变化（Stitched / 裁剪应用 / 方向切换）：重算面板高度（只增不减）并重绘
    /// （对齐 LongCapturePanelUpdate + InvalidateRect）。
    func contentChanged() {
        growHeightIfNeeded()
        view?.needsDisplay = true
    }

    /// tentative 跟踪状态变化（多跳恢复/预测推进/候选否决回退）：仅重绘，不触发尺寸重算
    /// （对齐 lc_session_windows.cpp 主循环的 trackingRevision 变化 → InvalidateRect 分支）。
    func trackingChanged() {
        view?.needsDisplay = true
    }

    /// 方向切换后完全重置（尺寸随新方向重算）。
    func resetForDirectionChange() {
        contentChanged()
    }

    /// 面板生长（LongCapturePanelUpdate 逐式移植）：按「显示空间」等比换算预览高，
    /// 屏幕下沿 / 工具栏顶边 / 选区顶边（上方停靠时）三重约束 + 屏高 45% 上限 +
    /// 最小高 48；高度只增不减（向下生长，顶边锚定）。
    private func growHeightIfNeeded() {
        guard let session = session, !panelRect.isNull, window != nil,
              let st = session.algo?.state, st.stitchH > 0, st.physW > 0 else { return }
        let pad = LC_PANEL_PAD_PX
        let winW = panelRect.width
        let availW = winW - pad * 2
        let win = session.outputRowWindow
        let rows = win.bottom - win.top
        guard rows > 0, availW >= 1 else { return }
        // 显示空间逻辑尺寸统一公式：固定轴取 cropRect 逻辑尺寸（无 /ds 舍入
        // 往返误差），滚动轴取 rows / scale
        let cropW = session.cropRect.width
        let cropH = session.cropRect.height
        let dispWLogical = max(session.horizontal ? CGFloat(rows) / session.scale : cropW, 1)
        let dispHLogical = session.horizontal ? cropH : CGFloat(rows) / session.scale
        let prevH = (dispHLogical * (availW / dispWLogical)).rounded()
        // 屏幕下沿约束：预览不超过面板顶部以下剩余空间与屏高 45%
        var roomH = session.virtualBounds.maxY - 4 - panelRect.minY - pad * 2
        // 选区底部工具栏避让：水平范围与面板重叠时，面板生长不得越过工具栏顶边
        if let tb = session.toolbar?.barRect,
           tb.minX < panelRect.maxX + 8, tb.maxX > panelRect.minX - 8, tb.minY > panelRect.minY {
            let byTb = tb.minY - 8 - panelRect.minY - pad * 2
            if byTb < roomH { roomH = byTb }
        }
        // 面板在选区上方时：生长不得越过选区顶边（否则面板入画）
        if panelAbove {
            let bySel = session.selection.minY - 12 - panelRect.minY - pad * 2
            if bySel < roomH { roomH = bySel }
        }
        var capH = session.virtualBounds.height * LC_PANEL_MAX_HEIGHT_RATIO   // 屏高 45% 上限
        if roomH < capH { capH = roomH }
        if capH < 48 { capH = 48 }                                            // LC_PANEL_MIN_H
        var prevHCapped = prevH
        if prevHCapped > capH { prevHCapped = capH }
        let newH = pad * 2 + prevHCapped
        if newH > panelRect.height {
            // 只增不减：顶边锚定向下生长（Windows SetWindowPos(SWP_NOMOVE) 语义）
            panelRect = CGRect(x: panelRect.minX, y: panelRect.minY, width: winW, height: newH)
            window?.setFrame(lcNSRect(fromCG: panelRect), display: true)
            view?.frame = NSRect(origin: .zero, size: panelRect.size)
        }
    }

    // MARK: 渲染几何（LongCapturePanelPreviewRect / LongCaptureViewportRectAt 移植）

    /// 预览图目标矩形（面板本地坐标）：拼接结果等比缩放，水平居中、垂直居中于面板内边距。
    /// 尺寸取「显示空间」：纵向 = physW×行数；横向 = 行数×physW（回转后宽高互换），
    /// 行数含裁剪窗口（裁掉的部分不进预览）。
    private func previewRect(in bounds: CGRect) -> CGRect {
        guard let session = session, let st = session.algo?.state,
              st.stitchH > 0, st.physW > 0 else { return .zero }
        let pad = LC_PANEL_PAD_PX
        let availW = bounds.width - pad * 2
        let availH = bounds.height - pad * 2
        guard availW >= 1, availH >= 1 else { return .zero }
        let win = session.outputRowWindow
        let rows = win.bottom - win.top
        guard rows > 0 else { return .zero }
        let dispW: CGFloat = session.horizontal ? CGFloat(rows) : CGFloat(st.physW)
        let dispH: CGFloat = session.horizontal ? CGFloat(st.physW) : CGFloat(rows)
        let scale = min(availW / dispW, availH / dispH)
        let pw = (dispW * scale).rounded()
        let ph = (dispH * scale).rounded()
        let left = (bounds.width - pw) / 2
        let top = pad + (availH - ph) / 2
        return CGRect(x: left, y: top, width: pw, height: ph)
    }

    /// 内容坐标 → 预览像素的视口框（对齐 LongCaptureViewportRectAt）：
    /// contentTop + headRows = 拼接图内位置，等比缩放并钳制进裁剪后的预览范围。
    /// 移动轴贴合预览区两端时该侧边界再外扩 1px（蓝框在极值处完整封边，语义 = 当前
    /// 视图之外无已捕获内容）；固定轴（纵向=左右、横向=上下）恒向外扩 1px——扩出的
    /// 框线恰落在整体描边环上，整像素压住描边。横向模式的「行位置」映射为水平位置。
    private func viewportRectAt(preview: CGRect, contentTop: Int64) -> CGRect {
        guard let session = session, let st = session.algo?.state,
              st.stitchH > 0, st.physW > 0,
              preview.width > 0, preview.height > 0 else { return .zero }
        let win = session.outputRowWindow
        let rowStart = win.top
        let rowEnd = win.bottom
        let rows = rowEnd - rowStart
        guard rows > 0 else { return .zero }
        if rows <= Int64(st.physH) {
            // 未拼接滚动：整体即当前区域（四边均贴端外扩）
            return preview.insetBy(dx: -1, dy: -1)
        }
        var topStitch = contentTop + Int64(st.headRows)   // 内容坐标 → 拼接坐标
        if topStitch < rowStart { topStitch = rowStart }
        var tailLimit = rowEnd - Int64(st.physH)
        if tailLimit < rowStart { tailLimit = rowStart }
        if topStitch > tailLimit { topStitch = tailLimit }
        let atHead = topStitch <= rowStart                // 带抵输出窗口头端
        let atTail = topStitch >= tailLimit               // 带抵尾端
        if !session.horizontal {
            let scale = preview.height / CGFloat(rows)
            var vhPx = (CGFloat(st.physH) * scale).rounded()
            if vhPx < 1 { vhPx = 1 }
            let topPx = preview.minY + (CGFloat(topStitch - rowStart) * scale).rounded()
            return CGRect(x: preview.minX - 1, y: topPx - (atHead ? 1 : 0),
                          width: preview.width + 2,
                          height: vhPx + (atHead ? 1 : 0) + (atTail ? 1 : 0))
        } else {
            let scale = preview.width / CGFloat(rows)
            var vwPx = (CGFloat(st.physH) * scale).rounded()
            if vwPx < 1 { vwPx = 1 }
            let leftPx = preview.minX + (CGFloat(topStitch - rowStart) * scale).rounded()
            return CGRect(x: leftPx - (atHead ? 1 : 0), y: preview.minY - 1,
                          width: vwPx + (atHead ? 1 : 0) + (atTail ? 1 : 0),
                          height: preview.height + 2)
        }
    }

    /// 已确认（committed）视口框：最新一次提交帧的精确位置（LongCaptureViewportRect）。
    private func committedRect(preview: CGRect) -> CGRect {
        guard let st = session?.algo?.state else { return .zero }
        return viewportRectAt(preview: preview, contentTop: st.committedContentTop)
    }

    // MARK: 渲染（LongCapturePanelRender 移植）

    /// 在已翻转（top-left 原点）的上下文中绘制 CGImage：CGImage 行序为 top-down 而
    /// CGContextDrawImage 按未翻转坐标绘制，需翻转 Y 轴避免上下颠倒（ScreenshotPaintMac
    /// 的同名会话方法为实例方法，面板侧本地实现同款翻转）。
    private func drawCGImageFlipped(_ ctx: CGContext, _ image: CGImage, in rect: CGRect) {
        ctx.saveGState()
        ctx.translateBy(x: rect.midX, y: rect.midY)
        ctx.scaleBy(x: 1, y: -1)
        ctx.draw(image, in: CGRect(x: -rect.width / 2, y: -rect.height / 2,
                                   width: rect.width, height: rect.height))
        ctx.restoreGState()
    }

    /// 整幅渲染小地图面板：清透明 → 深色圆角底 + 内缩描边 → 缩略小地图（裁剪行窗口内
    /// 重采样）→ 视口框三层标注（半透明衬底 / 实线蓝框 / 虚线橙框）。
    /// - Parameter ctx: 视图 CG 上下文（isFlipped，本地坐标 = 面板本地）
    func render(_ ctx: CGContext) {
        guard let session = session else { return }
        let bounds = view?.bounds ?? .zero
        guard bounds.width >= 1, bounds.height >= 1 else { return }

        // 1) 深色圆角底 + 内缩整像素描边（半径 8；描边路径整体内缩 0.5px，1px 笔画
        //    完整落在边界像素带内——对齐 LongCapturePanelRender 的抗锯齿/圆角外残留修复）
        let radius: CGFloat = 8
        let bgPath = CGPath(roundedRect: bounds, cornerWidth: radius, cornerHeight: radius, transform: nil)
        ctx.addPath(bgPath)
        ctx.setFillColor(Self.bgColor.cgColor)
        ctx.fillPath()
        let borderRect = bounds.insetBy(dx: 0.5, dy: 0.5)
        let borderPath = CGPath(roundedRect: borderRect,
                                cornerWidth: max(radius - 0.5, 1), cornerHeight: max(radius - 0.5, 1),
                                transform: nil)
        ctx.addPath(borderPath)
        ctx.setStrokeColor(Self.borderColor.cgColor)
        ctx.setLineWidth(1)
        ctx.strokePath()

        // 2) 缩略小地图：先按固定列宽增量缩列的缩略缓冲（算法层维护），按裁剪行窗口
        //    重采样绘制（避免逐帧重读拼接大缓冲；横向模式面板已回转为显示空间）
        let preview = previewRect(in: bounds)
        if preview.width > 0, preview.height > 0,
           let thumb = session.readPreviewThumbImage() {
            ctx.saveGState()
            ctx.clip(to: preview)                 // 先裁剪再绘制，防高质量插值渗到描边上
            ctx.interpolationQuality = .high
            drawCGImageFlipped(ctx, thumb, in: preview)
            ctx.restoreGState()
        }
        guard preview.width > 0, preview.height > 0,
              let st = session.algo?.state, st.stitchH > 0 else { return }

        // 3) 三层视口标注
        let vp = committedRect(preview: preview)
        let vpValid = vp.width > 0 && vp.height > 0
        // tentative 框显示判定：tentativeValid 且与 committed 差 ≥ LC_TRACK_MIN_STEP
        //（小于该位移为噪声不推进，lc_panel_ui_windows.cpp 同款；常量经 lc_get_algo_consts 取用）
        var tp = CGRect.zero
        var tpShow = false
        if st.tentativeValid {
            var tdiff = st.tentativeContentTop - st.committedContentTop
            if tdiff < 0 { tdiff = -tdiff }
            if tdiff >= Int64(LCAlgoConsts.shared.trackMinStep) {
                tp = viewportRectAt(preview: preview, contentTop: st.tentativeContentTop)
                tpShow = tp.width > 0 && tp.height > 0
            }
        }
        // 半透明衬底只铺描边内侧、向内收 1px；先画衬底再叠不透明描边
        if vpValid {
            ctx.setFillColor(Self.committedColor.withAlphaComponent(60.0 / 255.0).cgColor)
            ctx.fill(vp.insetBy(dx: 1, dy: 1))
        }
        if tpShow {
            ctx.setFillColor(Self.tentativeColor.withAlphaComponent(36.0 / 255.0).cgColor)
            ctx.fill(tp.insetBy(dx: 1, dy: 1))
        }
        // 1) 整体环：预览图外沿相邻一像素（左右列 left-1/right、上下行 top-1/bottom）
        ctx.setFillColor(Self.ringColor.cgColor)
        ctx.fill(CGRect(x: preview.minX - 1, y: preview.minY - 1, width: 1, height: preview.height + 2))
        ctx.fill(CGRect(x: preview.maxX, y: preview.minY - 1, width: 1, height: preview.height + 2))
        ctx.fill(CGRect(x: preview.minX - 1, y: preview.minY - 1, width: preview.width + 2, height: 1))
        ctx.fill(CGRect(x: preview.minX - 1, y: preview.maxY, width: preview.width + 2, height: 1))
        // 2) 视口蓝框：四条边界条各占一整像素行/列——固定轴两条恰压住整体环
        if vpValid {
            ctx.setFillColor(Self.committedColor.cgColor)
            ctx.fill(CGRect(x: vp.minX, y: vp.minY, width: vp.width, height: 1))
            ctx.fill(CGRect(x: vp.minX, y: vp.maxY - 1, width: vp.width, height: 1))
            ctx.fill(CGRect(x: vp.minX, y: vp.minY + 1, width: 1, height: vp.height - 2))
            ctx.fill(CGRect(x: vp.maxX - 1, y: vp.minY + 1, width: 1, height: vp.height - 2))
        }
        // 3) tentative 橙色虚线框：四边按「画4空3」分段（超出已捕获范围时贴边停驻）
        if tpShow {
            ctx.setFillColor(Self.tentativeColor.cgColor)
            func dashH(_ y: CGFloat, _ x0: CGFloat, _ x1: CGFloat) {
                var x = x0
                while x < x1 {
                    ctx.fill(CGRect(x: x, y: y, width: min(4, x1 - x), height: 1))
                    x += 7
                }
            }
            func dashV(_ x: CGFloat, _ y0: CGFloat, _ y1: CGFloat) {
                var y = y0
                while y < y1 {
                    ctx.fill(CGRect(x: x, y: y, width: 1, height: min(4, y1 - y)))
                    y += 7
                }
            }
            dashH(tp.minY, tp.minX, tp.maxX)
            dashH(tp.maxY - 1, tp.minX, tp.maxX)
            dashV(tp.minX, tp.minY + 1, tp.maxY - 1)
            dashV(tp.maxX - 1, tp.minY + 1, tp.maxY - 1)
        }
    }
}
