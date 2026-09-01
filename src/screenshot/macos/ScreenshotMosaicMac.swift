import Foundation
import AppKit
import CoreGraphics

// MARK: - 马赛克子系统（macOS；
// Windows mosaic_windows.cpp reveal-mask 模型移植）
//
// 核心模型（对齐 Windows）：
// - 会话内把整屏底图按当前块大小预像素化为 mosaicBase（1 缩略像素 = blockPx 逻辑像素）；
//   仅在「块大小或屏幕尺寸变化」时重建，且首次需要马赛克的绘制才生成（延迟预处理）
// - 马赛克标注只是「蒙版」：框选 = 矩形 path、涂抹 = 沿路径 step=radius/2 的圆并集 path
//   （相邻圆重叠 ≥50%，快速移动无缝隙）；渲染 = 底图 + mosaicBase 经蒙版裁剪叠加
// - 马赛克标注不可选中/不可拖动；块大小 {6,10,16}、涂抹半径 {12,22,36}
// - 导出时从裁剪底图现场重算（不依赖会话内缓存），对齐 CompositeAnnotations 序列
//
// 像素化实现：Windows 用两趟 StretchBlt（HALFTONE 缩小 + COLORONCOLOR 最近邻放大）；
// macOS 用手写平均块采样（纯 CPU）产出 1px/块 缩略图，渲染时以最近邻放大铺回
//（interpolationQuality = .none 等价 COLORONCOLOR），视觉效果对齐 HALFTONE 两趟。

// MARK: - 常量（Windows 出处集中标注）

extension SC {
    /// 马赛克块大小预设，逻辑像素（internal.h: SC_MOSAIC_SIZES = { 6, 10, 16 }）
    static let mosaicSizes: [Int] = [6, 10, 16]
    /// 默认块大小档：中块（internal.h: SC_DEFAULT_MOSAIC_IDX = 1）
    static let defaultMosaicIdx = 1
    /// 涂抹半径预设，逻辑像素（internal.h: SC_MOSAIC_RADIUS = { 12, 22, 36 }）
    static let mosaicRadii: [Int] = [12, 22, 36]
    /// 默认涂抹半径档：中半径（internal.h: SC_DEFAULT_MOSAIC_RADIUS_IDX = 1）
    static let defaultMosaicRadiusIdx = 1
    /// 马赛克子菜单模式数（overlay_ui_windows.cpp: SC_MOSAIC_MODE_COUNT = 2，涂抹/框选）
    static let mosaicModeCount = 2
    /// 涂抹轨迹圆并集步长系数（BuildMosaicMaskRegion：step = max(1, radius * 0.5)，
    /// 相邻圆重叠 ≥50% 保证无缝隙）
    static let mosaicBrushStepFactor: Double = 0.5
    /// 马赛克子菜单命中码基址（HitTestMosaicPopup：+1/+2 模式；100+i+1 块大小；200+i+1 半径）
    static let mosaicHitSizeBase = 100
    static let mosaicHitRadiusBase = 200
    /// 圆环光标位图外边距（CreateMosaicBrushCursor：pad = 3）与最小边长（size < 16 → 16）
    static let mosaicCursorPad = 3
    static let mosaicCursorMinSize = 16
    /// base 重建的块大小下限（RebuildMosaicBase：blockPx < 2 → 2）
    static let mosaicMinBlockPx = 2
}

// MARK: - 手写平均块采样（纯 CPU）

/// 马赛克 base 缓存：源底图按块平均采样得到的缩略图（1 像素 = blockPx 逻辑像素的纯色块），
/// coverRect 为其覆盖的 CG 全局逻辑区域（渲染时放大铺回该矩形）。
final class ScreenshotMosaicBase {
    let image: CGImage
    let blockPx: Int
    let coverRect: CGRect

    init(image: CGImage, blockPx: Int, coverRect: CGRect) {
        self.image = image
        self.blockPx = blockPx
        self.coverRect = coverRect
    }
}

/// 手写平均块采样（纯 CPU；对齐 MosaicBlitRect 的 HALFTONE 缩小视觉）：
/// 源物理像素 → 每 blockPx 逻辑块求像素平均 → 1px/块 的 RGBA 缩略图。
/// 渲染端以最近邻放大（.none 插值）铺回，等价 Windows「HALFTONE 缩小 + COLORONCOLOR
/// 最近邻放大」两趟的视觉效果。
/// 实现说明：分带读取源图（每带 stripRows 物理行）避免整屏像素缓冲常驻；
/// 跨带的块用部分和累积至完整覆盖后写出平均色。缩略网格与 logicalRect 左上角对齐
///（覆盖层 base 对齐虚拟屏原点、导出对齐选区原点，与 Windows dstX0/srcAbsX0 语义一致）。
/// - Parameters:
///   - source: 源物理像素图像
///   - logicalRect: 需要马赛克化的逻辑区域（CG 全局坐标；块网格以其左上角对齐）
///   - scale: 物理/逻辑缩放比（Retina = 2.0）
///   - blockPx: 块大小（逻辑像素，<1 视为 1）
/// - Returns: 缩略块图（尺寸 = ceil(logicalRect 尺寸 / blockPx)）；失败返回 nil
func scBuildMosaicReducedImage(source: CGImage, logicalRect: CGRect, scale: CGFloat, blockPx: Int) -> CGImage? {
    let block = max(1, blockPx)
    guard scale > 0, logicalRect.width >= 1, logicalRect.height >= 1 else { return nil }
    let logicalW = Int(logicalRect.width.rounded(.up))
    let logicalH = Int(logicalRect.height.rounded(.up))
    let reducedW = (logicalW + block - 1) / block
    let reducedH = (logicalH + block - 1) / block
    guard reducedW > 0, reducedH > 0 else { return nil }

    let physW = source.width
    let physH = source.height
    guard physW > 0, physH > 0 else { return nil }

    var reduced = [UInt8](repeating: 0, count: reducedW * reducedH * 4)
    // 部分和缓冲：块未完整覆盖前跨带累积（r/g/b/count 四通道交错）
    var partial = [Double](repeating: 0, count: reducedW * reducedH * 4)

    let blockPhys = CGFloat(block) * scale
    let stripRows = 128
    var stripBuf = [UInt8](repeating: 0, count: physW * min(stripRows, physH) * 4)
    let colorSpace = CGColorSpaceCreateDeviceRGB()
    let bitmapInfo = CGImageAlphaInfo.premultipliedLast.rawValue | CGBitmapInfo.byteOrder32Big.rawValue

    var stripTop = 0
    while stripTop < physH {
        let rows = min(stripRows, physH - stripTop)
        guard let crop = source.cropping(to: CGRect(x: 0, y: stripTop, width: physW, height: rows)) else {
            return nil
        }
        let drawn = stripBuf.withUnsafeMutableBytes { ptr -> Bool in
            guard let ctx = CGContext(data: ptr.baseAddress, width: physW, height: rows,
                                      bitsPerComponent: 8, bytesPerRow: physW * 4,
                                      space: colorSpace, bitmapInfo: bitmapInfo) else { return false }
            ctx.interpolationQuality = .none
            ctx.draw(crop, in: CGRect(x: 0, y: 0, width: CGFloat(physW), height: CGFloat(rows)))
            return true
        }
        guard drawn else { return nil }

        stripBuf.withUnsafeMutableBufferPointer { buf in
            for by in 0..<reducedH {
                let y0 = CGFloat(by) * blockPhys
                let y1 = min(CGFloat(physH), CGFloat(by + 1) * blockPhys)
                let iy0 = max(Int(y0.rounded(.down)), stripTop)
                let iy1 = min(Int(y1.rounded(.up)), stripTop + rows, physH)
                if iy1 <= iy0 { continue }
                for bx in 0..<reducedW {
                    let x0 = CGFloat(bx) * blockPhys
                    let x1 = min(CGFloat(physW), CGFloat(bx + 1) * blockPhys)
                    let ix0 = Int(x0.rounded(.down))
                    let ix1 = min(Int(x1.rounded(.up)), physW)
                    if ix1 <= ix0 { continue }
                    // 累加块 ∩ 本带的物理像素
                    var sr = 0.0, sg = 0.0, sb = 0.0, cnt = 0.0
                    for y in iy0..<iy1 {
                        let rowOff = (y - stripTop) * physW * 4
                        var xOff = ix0 * 4
                        for _ in ix0..<ix1 {
                            sr += Double(buf[rowOff + xOff])
                            sg += Double(buf[rowOff + xOff + 1])
                            sb += Double(buf[rowOff + xOff + 2])
                            xOff += 4
                            cnt += 1
                        }
                    }
                    let base = (by * reducedW + bx) * 4
                    partial[base] += sr
                    partial[base + 1] += sg
                    partial[base + 2] += sb
                    partial[base + 3] += cnt
                    // 块的物理行范围已全部处理 → 写出平均色（底为不透明截图，alpha 固定 255）
                    if iy1 >= Int(y1.rounded(.up)) {
                        let c = partial[base + 3]
                        if c > 0 {
                            reduced[base] = UInt8(max(0, min(255, (partial[base] / c).rounded())))
                            reduced[base + 1] = UInt8(max(0, min(255, (partial[base + 1] / c).rounded())))
                            reduced[base + 2] = UInt8(max(0, min(255, (partial[base + 2] / c).rounded())))
                            reduced[base + 3] = 255
                        }
                    }
                }
            }
        }
        stripTop += rows
    }

    guard let outCtx = CGContext(data: &reduced, width: reducedW, height: reducedH,
                                 bitsPerComponent: 8, bytesPerRow: reducedW * 4,
                                 space: colorSpace, bitmapInfo: bitmapInfo) else { return nil }
    return outCtx.makeImage()
}

// MARK: - 马赛克专属圆环光标（mosaic_windows.cpp CreateMosaicBrushCursor / InitMosaicBrushCursors 移植）

/// 涂抹模式圆环光标缓存：半径圆（白描边底 + 深色虚线内圈 + 中心十字准星），
/// 热区居中，圆环直径随当前涂抹半径。OS 跟随鼠标，无重绘延迟。
enum ScreenshotMosaicCursors {
    private static var cache: [Int: NSCursor] = [:]   // key = (半径 << 8) | scale（scale 为屏 backing 倍数 1/2/3）

    /// 取指定半径与屏缩放倍数的圆环光标（首次构建后缓存；会话收口可 reset 释放）。
    /// 位图显式按目标屏 backingScaleFactor 渲染（Retina = @2x）再以逻辑尺寸包装：
    /// 用 NSImage(size:flipped:drawingHandler:) 惰性渲染生成的是 1x 位图，Retina 屏
    /// 光标视觉尺寸减半（环直径只有涂抹揭示圆的一半，涂抹时表现为「小环 + 大揭示圆」
    /// 两个圆框）；scale 必须由调用方按鼠标所在屏传入（NSScreen.main 在覆盖层未激活
    /// 会话中不可靠，返回 nil 会错误兜底到 1x）。
    /// - Parameters:
    ///   - radius: 涂抹半径（逻辑像素）
    ///   - scale: 目标屏物理/逻辑倍数（≥1；retina = 2）
    /// - Returns: 圆环光标（热区居中）
    static func cursor(radius: Int, scale: Int) -> NSCursor {
        let key = (radius << 8) | max(1, scale)
        if let cached = cache[key] { return cached }
        let pad = SC.mosaicCursorPad
        var size = (radius + pad) * 2
        if size < SC.mosaicCursorMinSize { size = SC.mosaicCursorMinSize }
        let r = CGFloat(radius)
        let darkCol = NSColor(srgbRed: 30.0 / 255.0, green: 30.0 / 255.0, blue: 30.0 / 255.0, alpha: 1.0)
        let sf = CGFloat(max(1, scale))
        let px = Int(CGFloat(size) * sf)
        let rep = NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: px, pixelsHigh: px,
                                   bitsPerSample: 8, samplesPerPixel: 4, hasAlpha: true,
                                   isPlanar: false, colorSpaceName: .deviceRGB,
                                   bytesPerRow: 0, bitsPerPixel: 0)
        let image: NSImage
        if let rep = rep {
            rep.size = NSSize(width: size, height: size)   // 逻辑尺寸（位图像素 = size×scale）
            NSGraphicsContext.saveGraphicsState()
            NSGraphicsContext.current = NSGraphicsContext(bitmapImageRep: rep)
            // 绘制坐标系 = 逻辑点（size×size），与旧版绘制代码一致
            if let ctx = NSGraphicsContext.current?.cgContext {
                let cx = CGFloat(size) / 2
                let cy = CGFloat(size) / 2
                let ring = CGRect(x: cx - r, y: cy - r, width: r * 2, height: r * 2)
                // 外圈：白色描边底（保证暗背景可见）
                ctx.setStrokeColor(NSColor.white.cgColor)
                ctx.setLineWidth(3)
                ctx.strokeEllipse(in: ring)
                // 内圈：深色虚线描边
                ctx.setStrokeColor(darkCol.cgColor)
                ctx.setLineWidth(1.5)
                ctx.setLineDash(phase: 0, lengths: [4, 3])
                ctx.strokeEllipse(in: ring)
                ctx.setLineDash(phase: 0, lengths: [])
                // 中心十字准星（长度 = min(6, radius)）
                let cl = CGFloat(min(6, radius))
                ctx.setStrokeColor(darkCol.cgColor)
                ctx.setLineWidth(1)
                ctx.move(to: CGPoint(x: cx - cl, y: cy))
                ctx.addLine(to: CGPoint(x: cx + cl, y: cy))
                ctx.move(to: CGPoint(x: cx, y: cy - cl))
                ctx.addLine(to: CGPoint(x: cx, y: cy + cl))
                ctx.strokePath()
            }
            NSGraphicsContext.restoreGraphicsState()
            image = NSImage(cgImage: rep.cgImage!, size: NSSize(width: size, height: size))
        } else {
            // 位图分配失败兜底：退回旧惰性渲染路径（环径可能失真，仅防御性保留）
            image = NSImage(size: NSSize(width: size, height: size), flipped: false) { _ in
                guard let ctx = NSGraphicsContext.current?.cgContext else { return false }
                let cx = CGFloat(size) / 2
                let cy = CGFloat(size) / 2
                let ring = CGRect(x: cx - r, y: cy - r, width: r * 2, height: r * 2)
                ctx.setStrokeColor(NSColor.white.cgColor)
                ctx.setLineWidth(3)
                ctx.strokeEllipse(in: ring)
                ctx.setStrokeColor(darkCol.cgColor)
                ctx.setLineWidth(1.5)
                ctx.setLineDash(phase: 0, lengths: [4, 3])
                ctx.strokeEllipse(in: ring)
                ctx.setLineDash(phase: 0, lengths: [])
                let cl = CGFloat(min(6, radius))
                ctx.setStrokeColor(darkCol.cgColor)
                ctx.setLineWidth(1)
                ctx.move(to: CGPoint(x: cx - cl, y: cy))
                ctx.addLine(to: CGPoint(x: cx + cl, y: cy))
                ctx.move(to: CGPoint(x: cx, y: cy - cl))
                ctx.addLine(to: CGPoint(x: cx, y: cy + cl))
                ctx.strokePath()
                return true
            }
        }
        let cursor = NSCursor(image: image, hotSpot: NSPoint(x: CGFloat(size) / 2, y: CGFloat(size) / 2))
        cache[key] = cursor
        return cursor
    }

    /// 释放缓存（会话收口时调用；光标随会话生命周期重建）。
    static func reset() {
        cache.removeAll()
    }
}

// MARK: - 会话扩展（base 重建 / 蒙版路径 / 覆盖层渲染 / 导出合成 / 绘制流程）

extension ScreenshotOverlaySession {
    /// 是否存在需要渲染的马赛克内容（已提交或正在绘制；对齐 HasMosaicToRender）。
    func hasMosaicToRender() -> Bool {
        if hasCurDrawing && curDrawing.type == .mosaic { return true }
        return annotations.contains { $0.type == .mosaic }
    }

    /// 确保马赛克 base 缓存有效（对齐 MosaicBaseNeedsRebuild + RebuildMosaicBase）：
    /// 仅块大小或覆盖尺寸变化时重建；失败保留旧缓存（可能为 nil，调用方跳过揭示）。
    /// - Returns: 有效 base；生成失败且无旧缓存时返回 nil
    @discardableResult
    func ensureMosaicBase() -> ScreenshotMosaicBase? {
        let blockPx = max(SC.mosaicMinBlockPx, SC.mosaicSizes[mosaicSizeIdx])
        let cover = CGRect(origin: baseFrame.origin, size: baseFrame.logicalSize)
        if let cache = mosaicBaseCache, cache.blockPx == blockPx, cache.coverRect == cover {
            return cache
        }
        guard let image = scBuildMosaicReducedImage(source: baseFrame.image,
                                                    logicalRect: cover,
                                                    scale: baseFrame.scale,
                                                    blockPx: blockPx) else {
            return mosaicBaseCache
        }
        let base = ScreenshotMosaicBase(image: image, blockPx: blockPx, coverRect: cover)
        mosaicBaseCache = base
        return base
    }

    /// 构建马赛克标注的蒙版路径（对齐 BuildMosaicMaskRegion）：
    /// 框选 = 规范化矩形；涂抹 = 沿路径以 step = max(1, radius*0.5) 插值取点的圆并集
    ///（子路径同向，nonzero 填充即并集）。ox/oy 把绝对坐标换算到目标局部坐标。
    /// - Parameters:
    ///   - ox/oy: 绝对坐标 → 目标局部坐标偏移（覆盖层 = -cgOrigin；导出 = -选区左上角）
    ///   - includeCurDrawing: 是否并入正在绘制的马赛克（覆盖层 true / 导出 false）
    /// - Returns: 蒙版路径；无马赛克内容返回 nil
    func mosaicMaskPath(ox: CGFloat, oy: CGFloat, includeCurDrawing: Bool) -> CGPath? {
        var targets: [ScreenshotAnnotation] = annotations.filter { $0.type == .mosaic }
        if includeCurDrawing && hasCurDrawing && curDrawing.type == .mosaic {
            targets.append(curDrawing)
        }
        guard !targets.isEmpty else { return nil }

        func addCircle(_ path: CGMutablePath, _ cx: Double, _ cy: Double, _ radius: Int) {
            let r = CGFloat(radius)
            path.addEllipse(in: CGRect(x: CGFloat(cx) + ox - r, y: CGFloat(cy) + oy - r,
                                       width: r * 2, height: r * 2))
        }

        let path = CGMutablePath()
        for a in targets {
            if a.mosaicRect {
                // 框选：规范化矩形（对齐 absL/absT/absR/absB）
                path.addRect(CGRect(x: CGFloat(min(a.x1, a.x2)) + ox, y: CGFloat(min(a.y1, a.y2)) + oy,
                                    width: CGFloat(abs(a.x2 - a.x1)), height: CGFloat(abs(a.y2 - a.y1))))
            } else {
                // 涂抹：相邻点线段按 step 插值取点，每点一个圆（重叠 ≥50% 无缝隙）。
                // 同笔内逐段并集：每帧从全部路径点重建（与 Windows WM_PAINT 重建 Region 等价）。
                let radius = max(1, a.brushRadius)
                let step = max(1.0, Double(radius) * SC.mosaicBrushStepFactor)
                guard let first = a.pts.first else { continue }
                addCircle(path, Double(first.x), Double(first.y), radius)
                for i in 1..<a.pts.count {
                    let x0 = Double(a.pts[i - 1].x)
                    let y0 = Double(a.pts[i - 1].y)
                    let x1 = Double(a.pts[i].x)
                    let y1 = Double(a.pts[i].y)
                    let segLen = ((x1 - x0) * (x1 - x0) + (y1 - y0) * (y1 - y0)).squareRoot()
                    if segLen < 0.5 {
                        addCircle(path, x1, y1, radius)
                        continue
                    }
                    var n = Int((segLen / step).rounded())
                    if n < 1 { n = 1 }
                    for k in 1...n {
                        let t = Double(k) / Double(n)
                        addCircle(path, x0 + (x1 - x0) * t, y0 + (y1 - y0) * t, radius)
                    }
                }
            }
        }
        return path
    }

    /// 覆盖层马赛克揭示渲染（对齐 RevealMosaicToTarget）：mosaicBase 经蒙版路径裁剪叠加，
    /// 蒙版先与选区求交（笔刷半径/框选不越过选区）。绘制次序在矢量/文字标注之下
    ///（调用点位于 paintAnnotationsLayer 之前）。不依赖会话内缓存的调用方请走导出路径。
    func paintMosaicLayer(ctx: CGContext, view: OverlayScreenshotView) {
        guard state == .confirmed || state == .resizing || state == .drawing || state == .textEditing else { return }
        guard hasMosaicToRender(), let base = ensureMosaicBase(),
              let mask = mosaicMaskPath(ox: -view.cgOrigin.x, oy: -view.cgOrigin.y,
                                        includeCurDrawing: true) else { return }
        ctx.saveGState()
        ctx.interpolationQuality = .none   // 最近邻放大：每缩略像素一块纯色（对齐 COLORONCOLOR）
        // 蒙版 ∩ 选区（Windows contentBounds 半开区间语义：边框外沿不入画）
        ctx.addRect(selection.offsetBy(dx: -view.cgOrigin.x, dy: -view.cgOrigin.y))
        ctx.clip()
        ctx.addPath(mask)
        ctx.clip()
        // 缩略块图最近邻放大铺回覆盖范围（1 缩略像素 = blockPx 逻辑像素）
        scDrawCGImage(ctx, base.image,
                      in: base.coverRect.offsetBy(dx: -view.cgOrigin.x, dy: -view.cgOrigin.y))
        ctx.restoreGState()

        // 框选模式拖拽中的虚线预览（白底 + 蓝虚线双层矩形，对齐 OnPaint CS_Drawing 分支；
        // 涂抹模式不画矩形边框，范围由揭示圆体现）
        if state == .drawing && hasCurDrawing && curDrawing.type == .mosaic && curDrawing.mosaicRect {
            let r = CGRect(x: CGFloat(min(curDrawing.x1, curDrawing.x2)) - view.cgOrigin.x,
                           y: CGFloat(min(curDrawing.y1, curDrawing.y2)) - view.cgOrigin.y,
                           width: CGFloat(abs(curDrawing.x2 - curDrawing.x1)),
                           height: CGFloat(abs(curDrawing.y2 - curDrawing.y1)))
            ctx.setStrokeColor(NSColor.white.cgColor)
            ctx.setLineWidth(3)
            ctx.stroke(r)
            ctx.setStrokeColor(NSColor(srgbRed: 0x1E / 255.0, green: 0x88 / 255.0,
                                       blue: 0xE5 / 255.0, alpha: 1.0).cgColor)
            ctx.setLineWidth(1.5)
            ctx.setLineDash(phase: 0, lengths: [6, 4])
            ctx.stroke(r)
            ctx.setLineDash(phase: 0, lengths: [])
        }
    }

    /// 导出合成：马赛克层从裁剪底图现场重算（不依赖会话内 base 缓存；对齐
    /// CompositeAnnotations 的 MosaicBlitRect + RevealMosaicToTarget 序列）。
    /// 块大小取会话当前档（Windows 导出用 mosaicBlockPx = 当前全局块大小，保证导出与所见一致）。
    /// 调用方上下文需为左上原点、原点 = 选区左上角（合成位图坐标系）。
    /// - Parameters:
    ///   - ctx: 合成位图上下文（已翻转，原点 = 选区左上角）
    ///   - cropped: 按选区裁剪的物理像素底图
    ///   - region: 选区（CG 全局逻辑坐标）
    func compositeMosaicLayer(_ ctx: CGContext, cropped: CGImage, region: CGRect) {
        guard annotations.contains(where: { $0.type == .mosaic }) else { return }
        let blockPx = max(SC.mosaicMinBlockPx, SC.mosaicSizes[mosaicSizeIdx])
        let localRect = CGRect(x: 0, y: 0, width: region.width, height: region.height)
        guard let reduced = scBuildMosaicReducedImage(source: cropped,
                                                      logicalRect: localRect,
                                                      scale: baseFrame.scale,
                                                      blockPx: blockPx),
              let mask = mosaicMaskPath(ox: -region.minX, oy: -region.minY,
                                        includeCurDrawing: false) else { return }
        ctx.saveGState()
        ctx.interpolationQuality = .none
        ctx.addRect(localRect)   // 蒙版 ∩ 选区（导出无选区外内容）
        ctx.clip()
        ctx.addPath(mask)
        ctx.clip()
        scDrawCGImage(ctx, reduced, in: localRect)
        ctx.restoreGState()
    }

    // MARK: 绘制流程（对齐 OnLButtonDown / OnMouseMove 的 TB_Mosaic 分支）

    /// 开始马赛克绘制（马赛克工具激活 + 点击选区内）：清除选中态，按当前子菜单
    /// 模式/块大小/半径固化参数；框选记录起点，涂抹记录路径起点。
    /// - Parameter point: 起点（CG 全局坐标）
    func beginMosaicDrawing(at point: CGPoint) {
        selectedAnnotation = -1
        hoveredAnnotation = -1
        selectedTextAnnotation = -1
        hoveredTextAnnotation = -1
        draggingTextAnnotation = -1
        hasCurDrawing = true
        curDrawing = ScreenshotAnnotation(type: .mosaic, color: ScreenshotRGB(r: 0, g: 0, b: 0))
        curDrawing.mosaicRect = mosaicRectMode
        curDrawing.mosaicSize = SC.mosaicSizes[mosaicSizeIdx]
        curDrawing.brushRadius = SC.mosaicRadii[mosaicRadiusIdx]
        curDrawing.x1 = Int(point.x)
        curDrawing.y1 = Int(point.y)
        curDrawing.x2 = curDrawing.x1
        curDrawing.y2 = curDrawing.y1
        if !mosaicRectMode {
            curDrawing.pts = [ScreenshotAnnotationPoint(x: curDrawing.x1, y: curDrawing.y1)]
        }
        state = .drawing
        invalidateAll()
    }

    /// 当前涂抹半径的圆环光标（半径档防越界兜底默认档）。位图倍数按鼠标所在屏的
    /// backingScaleFactor 取（NSScreen.main 在覆盖层未激活会话中不可靠），保证 Retina
    /// 屏光标环视觉直径与涂抹揭示圆一致。
    func currentMosaicCursor() -> NSCursor {
        let idx = (mosaicRadiusIdx >= 0 && mosaicRadiusIdx < SC.mosaicRadii.count)
            ? mosaicRadiusIdx : SC.defaultMosaicRadiusIdx
        let scale = NSScreen.screens.first { ScreenshotGeometry.cgFrame(of: $0).contains(mouse) }?
            .backingScaleFactor ?? NSScreen.main?.backingScaleFactor ?? 2
        return ScreenshotMosaicCursors.cursor(radius: SC.mosaicRadii[idx], scale: Int(scale))
    }
}
