import Foundation
import AppKit
import CoreGraphics

// MARK: - 矢量标注（macOS；Windows annotations_windows.cpp 移植）
//
// 覆盖矩形/椭圆/箭头/画笔四类矢量标注的数据模型、绘制、命中判定、选中手柄、
// 拖动/缩放变换与撤销/重做快照栈。文字与马赛克的数据字段已落在
// ScreenshotAnnotation 的对应字段组，实现见 ScreenshotTextMac/ScreenshotMosaicMac.swift。
//
// 坐标系约定：所有标注统一用「绝对 CG 全局坐标」（逻辑点、左上原点，与
// session.selection / session.mouse 同坐标系），对齐 Windows internal.h Annotation 的
// 「绝对虚拟屏幕坐标」语义——选区移动/缩放时标注位置固定不动。

// MARK: - 常量（Windows 出处集中标注）

extension SC {
    /// 撤销/重做快照栈最大深度（internal.h: SC_UNDO_MAX_DEPTH = 50；超限裁掉最老快照）
    static let undoMaxDepth = 50
    /// 粗细预设，逻辑像素线宽（internal.h: SC_THICK_PRESETS = { 1, 2, 4 }）
    static let thickPresets: [Int] = [1, 2, 4]
    /// 子菜单粗细圆点预览直径（internal.h: SC_THICK_DOT_SIZES = { 5, 10, 16 }，仅界面显示）
    static let thickDotSizes: [CGFloat] = [5, 10, 16]
    /// 默认粗细档：中粗（internal.h: SC_DEFAULT_THICK_IDX = 1）
    static let defaultThickIdx = 1
    /// 颜色预设八色（internal.h: SC_COLOR_PRESETS）
    static let colorPresets: [ScreenshotRGB] = [
        ScreenshotRGB(r: 0xE5, g: 0x39, b: 0x35),   // 红
        ScreenshotRGB(r: 0xFB, g: 0x8C, b: 0x00),   // 橙
        ScreenshotRGB(r: 0xFD, g: 0xD8, b: 0x35),   // 黄
        ScreenshotRGB(r: 0x43, g: 0xA0, b: 0x47),   // 绿
        ScreenshotRGB(r: 0x00, g: 0xAC, b: 0xC1),   // 青
        ScreenshotRGB(r: 0x1E, g: 0x88, b: 0xE5),   // 蓝
        ScreenshotRGB(r: 0xFF, g: 0xFF, b: 0xFF),   // 白
        ScreenshotRGB(r: 0x33, g: 0x33, b: 0x33),   // 黑
    ]
    /// 默认颜色档：红（internal.h: SC_DEFAULT_COLOR_IDX = 0）
    static let defaultColorIdx = 0
    /// 选中标注手柄描边红（overlay_paint_windows.cpp：白色圆手柄 + 红 1px 描边 RGB(229,57,53)）
    static let annotationHandleStroke = NSColor(srgbRed: 229.0 / 255.0, green: 57.0 / 255.0,
                                                blue: 53.0 / 255.0, alpha: 1.0)
}

// MARK: - 数据模型（对齐 Windows internal.h Annotation）

/// 标注类型（对齐 Windows internal.h AnnotationType）。
enum ScreenshotAnnotationType: Int {
    case rect    // AT_Rect 矩形（空心描边）
    case circle  // AT_Circle 圆形/椭圆（空心描边）
    case arrow   // AT_Arrow 箭头（机翼状锥形多边形填充）
    case brush   // AT_Brush 画笔（自由路径，圆头圆接）
    case text    // AT_Text 文字（thickness = 字号，text 为内容，x1/y1 为锚点）
    case mosaic  // AT_Mosaic 马赛克（reveal-mask 蒙版标注，不可选中/不可拖动）
}

/// 折线采样点（整数点，对齐 Windows POINT 的 int 语义）。
struct ScreenshotAnnotationPoint {
    var x: Int
    var y: Int
}

/// 标注数据模型（对齐 Windows internal.h 的 Annotation 字段；绝对 CG 全局坐标，
/// 选区移动/缩放标注不动）。
struct ScreenshotAnnotation {
    var type: ScreenshotAnnotationType
    var color: ScreenshotRGB
    var thickness: Int = 1                       // 逻辑像素线宽（文字标注时为字号）
    /// Rect/Circle/Arrow 的起止端点（绝对坐标）；Brush 仅用 pts
    var x1: Int = 0
    var y1: Int = 0
    var x2: Int = 0
    var y2: Int = 0
    /// Brush 自由路径（绝对坐标，逐点追加）；Mosaic 涂抹模式的路径（绝对坐标）
    var pts: [ScreenshotAnnotationPoint] = []
    // ---- 文字/马赛克字段（对齐 Windows internal.h Annotation 的对应成员）----
    var text: String = ""            // AT_Text 的文字内容
    var mosaicRect: Bool = false     // 马赛克 true=框选区域 / false=涂抹轨迹
    var mosaicSize: Int = 0          // 马赛克块大小（逻辑像素；提交时固化，导出仍用会话当前档）
    var brushRadius: Int = 0         // 马赛克涂抹半径（逻辑像素，仅涂抹模式有效）

    /// 空白标注（撤销快照/初始化占位用）。
    static let empty = ScreenshotAnnotation(type: .rect, color: ScreenshotRGB(r: 0, g: 0, b: 0))
}

// MARK: - 几何辅助（对齐 annotations_windows.cpp 的距离/包围盒函数族）

/// 点 (px,py) 到线段 (ax,ay)-(bx,by) 的最短距离（对齐 PointToSegmentDist；画笔/箭头命中用）。
func scPointToSegmentDist(_ px: Double, _ py: Double,
                          _ ax: Double, _ ay: Double, _ bx: Double, _ by: Double) -> Double {
    let dx = bx - ax
    let dy = by - ay
    let lenSq = dx * dx + dy * dy
    var t = 0.0
    if lenSq > 1e-9 {
        t = ((px - ax) * dx + (py - ay) * dy) / lenSq
        t = max(0, min(1, t))
    }
    let cx = ax + t * dx
    let cy = ay + t * dy
    let ex = px - cx
    let ey = py - cy
    return (ex * ex + ey * ey).squareRoot()
}

/// 点 (px,py) 到折线 pts 的最短距离（对齐 PointToPolylineDist；单点退化为点距）。
func scPointToPolylineDist(_ px: Double, _ py: Double, _ pts: [ScreenshotAnnotationPoint]) -> Double {
    if pts.isEmpty { return 1e18 }
    if pts.count == 1 {
        let ex = px - Double(pts[0].x)
        let ey = py - Double(pts[0].y)
        return (ex * ex + ey * ey).squareRoot()
    }
    var best = 1e18
    for i in 0..<(pts.count - 1) {
        let d = scPointToSegmentDist(px, py,
                                     Double(pts[i].x), Double(pts[i].y),
                                     Double(pts[i + 1].x), Double(pts[i + 1].y))
        if d < best { best = d }
    }
    return best
}

/// 箭头多边形几何（对齐 DrawOneAnnotation AT_Arrow 分支的机翼状参数）：
/// 箭身 = 起点→终点的锥形四边形（终点延伸至内凹点并略超出 overlap，由箭头覆盖重叠区
/// 避免抗锯齿细缝）；箭头 = 尖→右翼→内凹点→左翼（底边内凹而非平直三角）。
/// 参数照搬：headLen = thick*4+8、headHalfW = thick*2.4+5、notch = headLen*0.4、
/// endHalfW = headHalfW*0.55、startHalfW = max(thick*0.5, 0.75)、overlap = 1.5。
/// - Parameters:
///   - a: 箭头标注
///   - ox/oy: 绘制偏移（把绝对坐标换算到目标上下文局部坐标）
/// - Returns: (箭身四边形, 箭头四边形)；箭头长度 < 1px 返回 nil（与 Windows len<1 break 一致）
func scArrowGeometry(_ a: ScreenshotAnnotation, ox: CGFloat, oy: CGFloat) -> (body: [CGPoint], head: [CGPoint])? {
    let sx = CGFloat(a.x1) + ox
    let sy = CGFloat(a.y1) + oy
    let ex = CGFloat(a.x2) + ox
    let ey = CGFloat(a.y2) + oy
    let dx = ex - sx
    let dy = ey - sy
    let len = (dx * dx + dy * dy).squareRoot()
    if len < 1.0 { return nil }

    let thick = max(CGFloat(a.thickness), 1)
    var headLen = thick * 4.0 + 8.0
    let headHalfW = thick * 2.4 + 5.0
    let notch = headLen * 0.4                     // 内凹深度：底边中点向尖端凹入
    if headLen > len { headLen = len * 0.6 }
    let ux = dx / len
    let uy = dy / len
    let nx = -uy
    let ny = ux
    // 箭头底部中心（沿箭头方向后退 headLen）与内凹点
    let baseX = ex - ux * headLen
    let baseY = ey - uy * headLen
    let notchX = baseX + ux * notch
    let notchY = baseY + uy * notch
    // 箭身：起点细、终点粗的锥形；终点宽 < 两翼宽，两翼从箭身末端明显张开
    let startHalfW = max(thick * 0.5, 0.75)
    let endHalfW = headHalfW * 0.55
    let overlap: CGFloat = 1.5
    let bodyEndX = notchX + ux * overlap
    let bodyEndY = notchY + uy * overlap

    let body = [
        CGPoint(x: sx + nx * startHalfW, y: sy + ny * startHalfW),
        CGPoint(x: sx - nx * startHalfW, y: sy - ny * startHalfW),
        CGPoint(x: bodyEndX - nx * endHalfW, y: bodyEndY - ny * endHalfW),
        CGPoint(x: bodyEndX + nx * endHalfW, y: bodyEndY + ny * endHalfW),
    ]
    let head = [
        CGPoint(x: ex, y: ey),
        CGPoint(x: baseX + nx * headHalfW, y: baseY + ny * headHalfW),
        CGPoint(x: notchX, y: notchY),
        CGPoint(x: baseX - nx * headHalfW, y: baseY - ny * headHalfW),
    ]
    return (body, head)
}

/// 箭头标注包围盒（对齐 MeasureArrowAnnotationBounds）：全部几何控制点外包 + 2px 余量。
func scMeasureArrowBounds(_ a: ScreenshotAnnotation) -> CGRect {
    var minX = Double(min(a.x1, a.x2))
    var minY = Double(min(a.y1, a.y2))
    var maxX = Double(max(a.x1, a.x2))
    var maxY = Double(max(a.y1, a.y2))

    let sx = Double(a.x1), sy = Double(a.y1), ex = Double(a.x2), ey = Double(a.y2)
    let dx = ex - sx, dy = ey - sy
    let len = (dx * dx + dy * dy).squareRoot()
    if len >= 1.0 {
        let thick = Double(max(a.thickness, 1))
        var headLen = thick * 4.0 + 8.0
        let headHalfW = thick * 2.4 + 5.0
        if headLen > len { headLen = len * 0.6 }
        let notch = headLen * 0.4
        let ux = dx / len, uy = dy / len
        let nx = -uy, ny = ux
        let baseX = ex - ux * headLen, baseY = ey - uy * headLen
        let notchX = baseX + ux * notch, notchY = baseY + uy * notch
        let startHalfW = max(thick * 0.5, 0.75)
        let endHalfW = headHalfW * 0.55
        let overlap = 1.5
        let bodyEndX = notchX + ux * overlap, bodyEndY = notchY + uy * overlap

        func expand(_ x: Double, _ y: Double) {
            if x < minX { minX = x }
            if y < minY { minY = y }
            if x > maxX { maxX = x }
            if y > maxY { maxY = y }
        }
        expand(sx + nx * startHalfW, sy + ny * startHalfW)
        expand(sx - nx * startHalfW, sy - ny * startHalfW)
        expand(bodyEndX - nx * endHalfW, bodyEndY - ny * endHalfW)
        expand(bodyEndX + nx * endHalfW, bodyEndY + ny * endHalfW)
        expand(baseX + nx * headHalfW, baseY + ny * headHalfW)
        expand(notchX, notchY)
        expand(baseX - nx * headHalfW, baseY - ny * headHalfW)
    }

    let margin = 2.0
    return CGRect(x: (minX - margin).rounded(.down), y: (minY - margin).rounded(.down),
                  width: (maxX + margin).rounded(.up) - (minX - margin).rounded(.down),
                  height: (maxY + margin).rounded(.up) - (minY - margin).rounded(.down))
}

/// 单个标注包围盒（对齐 MeasureAnnotationBounds；绝对 CG 全局坐标，完整包住可见区域，
/// 供选中框/缩放手柄定位与选区内容约束使用）。
func scMeasureAnnotationBounds(_ a: ScreenshotAnnotation) -> CGRect {
    switch a.type {
    case .rect, .circle:
        return CGRect(x: min(a.x1, a.x2), y: min(a.y1, a.y2),
                      width: abs(a.x2 - a.x1), height: abs(a.y2 - a.y1))
    case .arrow:
        return scMeasureArrowBounds(a)
    case .brush:
        guard !a.pts.isEmpty else { return .zero }
        var minX = a.pts[0].x, minY = a.pts[0].y, maxX = a.pts[0].x, maxY = a.pts[0].y
        for p in a.pts {
            minX = min(minX, p.x); minY = min(minY, p.y)
            maxX = max(maxX, p.x); maxY = max(maxY, p.y)
        }
        return CGRect(x: minX, y: minY, width: maxX - minX, height: maxY - minY)
    case .text:
        // 文字：字形紧凑包围盒 + padding 4（与选中边框/命中区完全一致，对齐
        // MeasureTextAnnotation——复用同一测量保证 resize 约束 = 视觉边框）
        return scMeasureTextAnnotationBox(a)
    case .mosaic:
        // 马赛克：框选 = 两对角点；涂抹 = 路径包围盒 ± 半径（对齐 MeasureAnnotationBounds）
        if a.mosaicRect {
            return CGRect(x: min(a.x1, a.x2), y: min(a.y1, a.y2),
                          width: abs(a.x2 - a.x1), height: abs(a.y2 - a.y1))
        }
        guard !a.pts.isEmpty else { return .zero }
        var minX = a.pts[0].x, minY = a.pts[0].y, maxX = a.pts[0].x, maxY = a.pts[0].y
        for p in a.pts {
            minX = min(minX, p.x); minY = min(minY, p.y)
            maxX = max(maxX, p.x); maxY = max(maxY, p.y)
        }
        let r = a.brushRadius
        return CGRect(x: minX - r, y: minY - r, width: (maxX - minX) + r * 2, height: (maxY - minY) + r * 2)
    }
}

/// 全部标注内容的包围盒（对齐 CalcAnnotationsBounds；绝对 CG 全局坐标）。
/// 用于限制选区缩放：选区不可缩小到裁掉已添加内容。
/// - Returns: 包围盒；无标注返回 nil（无内容约束）
func scCalcAnnotationsBounds(_ anns: [ScreenshotAnnotation]) -> CGRect? {
    guard !anns.isEmpty else { return nil }
    var result: CGRect?
    for a in anns {
        let box = scMeasureAnnotationBounds(a)
        result = scUnionRect(result, box)
    }
    return result
}

// MARK: - 命中判定（对齐 HitTestAnnotation / HitTestAnnotationResizeHandle）

/// 命中测试标注，返回索引（对齐 HitTestAnnotation）：从顶层（数组末尾，绘制最上层）
/// 向底层遍历，命中第一个即返回（与视觉 z 序一致）。容差按线宽自适应：
/// 细线给 6px 余量、粗线给半个线宽 + 2px。矩形/椭圆仅命中轮廓（空心语义），
/// 箭头命中主轴线段，画笔命中折线。
/// - Parameters:
///   - anns: 标注数组
///   - point: 鼠标 CG 全局坐标
/// - Returns: 命中索引；未命中 -1
func scHitTestAnnotation(_ anns: [ScreenshotAnnotation], _ point: CGPoint) -> Int {
    for i in stride(from: anns.count - 1, through: 0, by: -1) {
        let a = anns[i]
        // 马赛克区域不可选中、不可拖拽，直接跳过命中测试（对齐 HitTestAnnotation 顶部 continue）
        if a.type == .mosaic { continue }
        let tol = max(6.0, Double(a.thickness) / 2.0 + 2.0)
        let x = Double(point.x)
        let y = Double(point.y)
        switch a.type {
        case .rect:
            // 仅命中矩形四条边轮廓（空心框），内部空白不选中
            let d1 = scPointToSegmentDist(x, y, Double(a.x1), Double(a.y1), Double(a.x2), Double(a.y1))
            let d2 = scPointToSegmentDist(x, y, Double(a.x2), Double(a.y2), Double(a.x1), Double(a.y2))
            let d3 = scPointToSegmentDist(x, y, Double(a.x1), Double(a.y2), Double(a.x1), Double(a.y1))
            let d4 = scPointToSegmentDist(x, y, Double(a.x2), Double(a.y1), Double(a.x2), Double(a.y2))
            if min(min(d1, d2), min(d3, d4)) <= tol { return i }
        case .circle:
            // 椭圆轮廓命中：归一化径向距离 r≈1，(r-1)*min(a,b) 换算回像素（保守足够）
            let cx = (Double(a.x1) + Double(a.x2)) * 0.5
            let cy = (Double(a.y1) + Double(a.y2)) * 0.5
            let aax = abs(Double(a.x2) - Double(a.x1)) * 0.5
            let aay = abs(Double(a.y2) - Double(a.y1)) * 0.5
            if aax < 0.5 && aay < 0.5 {
                let ex = x - cx, ey = y - cy
                if (ex * ex + ey * ey).squareRoot() <= tol { return i }
            } else if aax < 0.5 {
                if scPointToSegmentDist(x, y, cx, Double(a.y1), cx, Double(a.y2)) <= tol { return i }
            } else if aay < 0.5 {
                if scPointToSegmentDist(x, y, Double(a.x1), cy, Double(a.x2), cy) <= tol { return i }
            } else {
                let r = ((x - cx) / aax) * ((x - cx) / aax) + ((y - cy) / aay) * ((y - cy) / aay)
                let minAxis = min(aax, aay)
                if abs(r.squareRoot() - 1.0) * minAxis <= tol { return i }
            }
        case .arrow:
            if scPointToSegmentDist(x, y, Double(a.x1), Double(a.y1), Double(a.x2), Double(a.y2)) <= tol {
                return i
            }
        case .brush:
            if scPointToPolylineDist(x, y, a.pts) <= tol { return i }
        case .text:
            // 文字标注：包围盒（含 padding 4）整体命中（对齐 HitTestAnnotation AT_Text 分支）
            if scPointInRect(point, scMeasureTextAnnotationBox(a)) { return i }
        case .mosaic:
            break   // 已在循环顶部跳过，不可达（防御性保留分支）
        }
    }
    return -1
}

/// 命中测试标注缩放手柄（对齐 HitTestAnnotationResizeHandle，统一入口）：
/// 箭头 = 起点/终点 2 端点手柄；矩形/圆 = 包围盒 8 手柄（4 角 + 4 边中点，顺序与
/// HitTestAnnotationHandle 一致）；画笔 = 无手柄（仅可整体拖动）。
/// 容差沿用选区手柄的 handleSize，保证与选区手柄一致的可点击范围。
/// - Parameters:
///   - a: 目标标注
///   - point: 鼠标 CG 全局坐标
///   - handleSize: 命中半宽（SC.handleSize）
/// - Returns: 命中的手柄；未命中 .none
func scHitTestAnnotationHandle(_ a: ScreenshotAnnotation, _ point: CGPoint,
                               _ handleSize: CGFloat) -> ScreenshotResizeHandle {
    func hitBox(_ hx: Int, _ hy: Int, _ handle: ScreenshotResizeHandle) -> ScreenshotResizeHandle {
        let box = CGRect(x: CGFloat(hx) - handleSize, y: CGFloat(hy) - handleSize,
                         width: handleSize * 2, height: handleSize * 2)
        return scPointInRect(point, box) ? handle : .none
    }

    switch a.type {
    case .arrow:
        // 箭头只允许拖拽两个端点（而非四角包围盒缩放），单独命中两端点
        let start = hitBox(a.x1, a.y1, .arrowStart)
        if start != .none { return start }
        return hitBox(a.x2, a.y2, .arrowEnd)
    case .rect, .circle:
        let box = scMeasureAnnotationBounds(a)
        let cx = box.midX
        let cy = box.midY
        let tests: [(CGPoint, ScreenshotResizeHandle)] = [
            (CGPoint(x: box.minX, y: box.minY), .topLeft),
            (CGPoint(x: box.maxX, y: box.minY), .topRight),
            (CGPoint(x: box.minX, y: box.maxY), .bottomLeft),
            (CGPoint(x: box.maxX, y: box.maxY), .bottomRight),
            (CGPoint(x: cx, y: box.minY), .top),
            (CGPoint(x: cx, y: box.maxY), .bottom),
            (CGPoint(x: box.minX, y: cy), .left),
            (CGPoint(x: box.maxX, y: cy), .right),
        ]
        for (anchor, handle) in tests {
            let hit = CGRect(x: anchor.x - handleSize, y: anchor.y - handleSize,
                             width: handleSize * 2, height: handleSize * 2)
            if scPointInRect(point, hit) { return handle }
        }
        return .none
    case .brush:
        return .none   // 画笔无缩放手柄（蓝虚线包围盒选中，仅可整体拖动）
    case .text, .mosaic:
        // 文字/马赛克无缩放手柄（文字仅可整体拖动；马赛克不可选中，对齐 default 分支）
        return .none
    }
}

// MARK: - 变换（对齐 TransformAnnotationByBox）

/// 按包围盒变换映射标注所有坐标（对齐 TransformAnnotationByBox）：oldBox → newBox，
/// 标注内每个点 p 映射为 newBox.minX + (p - oldBox.minX) * sx。用于矩形/椭圆的
/// 8 手柄缩放（含边中点，非等比）；画笔无手柄不经过此路径。
/// sx/sy 防 0：旧宽/高为 0 时退化为平移。
func scTransformAnnotationByBox(_ a: inout ScreenshotAnnotation, oldBox: CGRect, newBox: CGRect) {
    let sx = oldBox.width > 0.5 ? newBox.width / oldBox.width : 1.0
    let sy = oldBox.height > 0.5 ? newBox.height / oldBox.height : 1.0
    func mapX(_ v: Int) -> Int {
        return Int((newBox.minX + (CGFloat(v) - oldBox.minX) * sx + 0.5).rounded(.down))
    }
    func mapY(_ v: Int) -> Int {
        return Int((newBox.minY + (CGFloat(v) - oldBox.minY) * sy + 0.5).rounded(.down))
    }
    switch a.type {
    case .rect, .circle, .arrow:
        a.x1 = mapX(a.x1); a.y1 = mapY(a.y1)
        a.x2 = mapX(a.x2); a.y2 = mapY(a.y2)
    case .brush, .mosaic:
        // 画笔/涂抹马赛克：路径按包围盒整体缩放（涂抹马赛克不可选中，兜底保留）
        for i in a.pts.indices {
            a.pts[i].x = mapX(a.pts[i].x)
            a.pts[i].y = mapY(a.pts[i].y)
        }
    case .text:
        // 文字不经过缩放路径（走锚点平移），此处兜底平移锚点（对齐 AT_Text 分支）
        a.x1 = mapX(a.x1); a.y1 = mapY(a.y1)
    }
}

// MARK: - 绘制（对齐 DrawOneAnnotation / OnPaint 选中视觉）

/// 绘制单条标注（不含裁剪；对齐 DrawOneAnnotation，GDI+ 抗锯齿 → CGContext 抗锯齿）。
/// 标注坐标为绝对 CG 全局坐标；ox/oy 把绝对坐标换算到目标上下文局部坐标：
/// 覆盖层视图 ox/oy = -view.cgOrigin；导出合成 ox/oy = -选区左上角。
/// 调用方需保证上下文为左上原点（覆盖层视图天然翻转；raw CGContext 由调用方自行翻转）。
func scDrawAnnotation(_ ctx: CGContext, _ a: ScreenshotAnnotation, ox: CGFloat, oy: CGFloat) {
    let color = NSColor(srgbRed: CGFloat(a.color.r) / 255.0, green: CGFloat(a.color.g) / 255.0,
                        blue: CGFloat(a.color.b) / 255.0, alpha: 1.0).cgColor
    let thick = max(CGFloat(a.thickness), 1)

    switch a.type {
    case .rect:
        // 矩形：空心描边（w/h 取绝对值，起点取 min 对齐 DrawRectangle）
        let rect = CGRect(x: min(CGFloat(a.x1), CGFloat(a.x2)) + ox,
                          y: min(CGFloat(a.y1), CGFloat(a.y2)) + oy,
                          width: abs(CGFloat(a.x2 - a.x1)), height: abs(CGFloat(a.y2 - a.y1)))
        ctx.setStrokeColor(color)
        ctx.setLineWidth(thick)
        ctx.setLineJoin(.round)
        ctx.stroke(rect)
    case .circle:
        // 椭圆：空心描边（由包围盒定义）
        let rect = CGRect(x: min(CGFloat(a.x1), CGFloat(a.x2)) + ox,
                          y: min(CGFloat(a.y1), CGFloat(a.y2)) + oy,
                          width: abs(CGFloat(a.x2 - a.x1)), height: abs(CGFloat(a.y2 - a.y1)))
        ctx.setStrokeColor(color)
        ctx.setLineWidth(thick)
        ctx.strokeEllipse(in: rect)
    case .arrow:
        // 箭头：锥形箭身 + 机翼状箭头（底边内凹），两段多边形填充
        guard let geo = scArrowGeometry(a, ox: ox, oy: oy) else { return }
        ctx.setFillColor(color)
        for poly in [geo.body, geo.head] {
            ctx.addPath(scPolygonPath(poly))
            ctx.fillPath()
        }
    case .brush:
        // 画笔：路径圆头圆接连线（对齐 pen LineCapRound/LineJoinRound）
        guard a.pts.count >= 2 else { return }
        let path = CGMutablePath()
        path.move(to: CGPoint(x: CGFloat(a.pts[0].x) + ox, y: CGFloat(a.pts[0].y) + oy))
        for p in a.pts.dropFirst() {
            path.addLine(to: CGPoint(x: CGFloat(p.x) + ox, y: CGFloat(p.y) + oy))
        }
        ctx.setStrokeColor(color)
        ctx.setLineWidth(thick)
        ctx.setLineCap(.round)
        ctx.setLineJoin(.round)
        ctx.addPath(path)
        ctx.strokePath()
    case .text:
        // 文字：顶部左对齐到锚点（x1,y1），字号 = thickness（下限 8，对齐 DrawOneAnnotation
        // AT_Text 分支）；马赛克不做矢量绘制（reveal-mask 单独渲染，对齐 DrawAnnotations 跳过）
        if a.text.isEmpty { return }
        scDrawTextLine(ctx, text: a.text, fontPx: max(CGFloat(a.thickness), CGFloat(SC.textMinFontSize)),
                       color: NSColor(srgbRed: CGFloat(a.color.r) / 255.0, green: CGFloat(a.color.g) / 255.0,
                                      blue: CGFloat(a.color.b) / 255.0, alpha: 1.0),
                       x: CGFloat(a.x1) + ox, y: CGFloat(a.y1) + oy)
    case .mosaic:
        break   // 马赛克由 paintMosaicLayer / 合成马赛克层单独渲染
    }
}

/// 绘制选中标注的视觉（对齐 overlay_paint_windows.cpp OnPaint 选中分支）：
/// 圆形/画笔画蓝色虚线包围盒（矩形/箭头/文字不画虚线框），手柄为白色圆形 + 红 1px 描边
///（箭头 2 端点 / 矩形·圆 8 个 / 画笔·文字·马赛克无）。文字标注为蓝色实线 2px 边框
///（对齐 annTextSelPen：PS_SOLID 2，RGB(0,136,255)，无手柄）；马赛克永不可选中。
/// - Parameters:
///   - ctx: 覆盖层上下文（左上原点）
///   - view: 当前绘制的覆盖层视图（绝对坐标 → 本地坐标）
///   - a: 选中的标注
func scPaintAnnotationSelection(_ ctx: CGContext, view: OverlayScreenshotView, _ a: ScreenshotAnnotation) {
    let box = scMeasureAnnotationBounds(a)
        .offsetBy(dx: -view.cgOrigin.x, dy: -view.cgOrigin.y)

    // 蓝色虚线包围盒：仅圆形/画笔（Windows：矩形/箭头/文字不画虚线框）
    if a.type == .circle || a.type == .brush {
        ctx.setStrokeColor(SC.accentBlue.cgColor)
        ctx.setLineWidth(1)
        ctx.setLineDash(phase: 0, lengths: [4, 4])   // 对齐 annHoverPen 的 PS_DASH 视觉
        ctx.stroke(box)
        ctx.setLineDash(phase: 0, lengths: [])
    }

    // 手柄集合：箭头取 2 端点；矩形/圆取 8 个（4 角 + 4 边中点）；画笔/文字无
    var handles: [CGPoint] = []
    switch a.type {
    case .arrow:
        handles = [
            CGPoint(x: CGFloat(a.x1) - view.cgOrigin.x, y: CGFloat(a.y1) - view.cgOrigin.y),
            CGPoint(x: CGFloat(a.x2) - view.cgOrigin.x, y: CGFloat(a.y2) - view.cgOrigin.y),
        ]
    case .rect, .circle:
        handles = [
            CGPoint(x: box.minX, y: box.minY), CGPoint(x: box.maxX, y: box.minY),
            CGPoint(x: box.minX, y: box.maxY), CGPoint(x: box.maxX, y: box.maxY),
            CGPoint(x: box.midX, y: box.minY), CGPoint(x: box.midX, y: box.maxY),
            CGPoint(x: box.minX, y: box.midY), CGPoint(x: box.maxX, y: box.midY),
        ]
    case .brush, .text:
        break
    case .mosaic:
        return   // 马赛克不可选中，防御性兜底
    }
    guard !handles.isEmpty else {
        // 文字标注：实线蓝色粗边框（对齐 annTextSelPen PS_SOLID 2 RGB(0,136,255)）
        if a.type == .text {
            ctx.setStrokeColor(SC.accentBlue.cgColor)
            ctx.setLineWidth(2)
            ctx.stroke(box)
        }
        return
    }
    let half = SC.handleSize / 2
    let circlePath = CGMutablePath()
    for h in handles {
        circlePath.addPath(CGPath(ellipseIn: CGRect(x: h.x - half, y: h.y - half,
                                                    width: half * 2, height: half * 2), transform: nil))
    }
    ctx.addPath(circlePath)
    ctx.setFillColor(NSColor.white.cgColor)
    ctx.fillPath()
    ctx.addPath(circlePath)
    ctx.setStrokeColor(SC.annotationHandleStroke.cgColor)
    ctx.setLineWidth(1)
    ctx.strokePath()
}

// MARK: - 会话扩展（历史栈 + 标注交互辅助）

extension ScreenshotOverlaySession {
    // ---- 撤销/重做（对齐 annotations_windows.cpp PushAnnotationHistory / Undo / Redo）----

    /// 压入一份整份标注数组的撤销快照（调用方在「变更前」调用）：Swift 数组为值类型，
    /// append 即得深拷贝快照；限深 SC.undoMaxDepth（50），入栈清空 redo 栈。
    func pushAnnotationHistory() {
        undoStack.append(annotations)
        if undoStack.count > SC.undoMaxDepth {
            undoStack.removeFirst()   // 裁掉最老快照，最近 50 步撤销不受影响
        }
        redoStack.removeAll()
        toolbar.refresh()
    }

    /// 撤销到上一份快照（对齐 UndoAnnotations）：当前状态入 redo 栈并整体替换标注，
    /// 随后清除进行中的标注交互态（ResetAnnotationInteraction 等价）。
    /// - Returns: 是否发生撤销（栈空返回 false，工具栏按钮据此保持置灰）
    func undoAnnotations() -> Bool {
        guard let last = undoStack.popLast() else { return false }
        redoStack.append(annotations)
        annotations = last
        resetAnnotationInteraction()
        invalidateAll()
        toolbar.refresh()
        return true
    }

    /// 重做到下一份快照（对齐 RedoAnnotations）：当前状态回入撤销栈并整体替换标注，
    /// 随后清除进行中的标注交互态。
    /// - Returns: 是否发生重做（栈空返回 false）
    func redoAnnotations() -> Bool {
        guard let next = redoStack.popLast() else { return false }
        undoStack.append(annotations)
        annotations = next
        resetAnnotationInteraction()
        invalidateAll()
        toolbar.refresh()
        return true
    }

    /// 清除进行中的标注交互态（对齐 ResetAnnotationInteraction；含文字字段）。
    func resetAnnotationInteraction() {
        selectedAnnotation = -1
        draggingAnnotation = -1
        resizingAnnotation = -1
        annotationResizeHandle = .none
        annotationOpHistoryPushed = false
        // 文字字段（selectedTextAnnotation / draggingTextAnnotation / hoveredTextAnnotation）
        selectedTextAnnotation = -1
        draggingTextAnnotation = -1
        hoveredTextAnnotation = -1
    }

    /// 清除当前标注选中态（点空白/切换工具/执行无关操作时调用；含文字选中，对齐
    /// Windows 同名清理序列）。
    func clearAnnotationSelection() {
        selectedAnnotation = -1
        draggingAnnotation = -1
        resizingAnnotation = -1
        annotationResizeHandle = .none
        selectedTextAnnotation = -1
        draggingTextAnnotation = -1
    }

    // ---- 标注拖动/缩放（对齐 overlay_input_windows.cpp 的 MOUSEMOVE 分支）----

    /// 选中标注的缩放拖拽：箭头端点手柄仅平移对应端点；矩形/圆的 8 手柄走
    /// 「更新包围盒 → 防翻转钳制（≥2px）→ 按盒变换」。首次实际位移才入历史。
    /// - Parameter point: 鼠标 CG 全局坐标
    func applyAnnotationResizeDrag(_ point: CGPoint) {
        guard resizingAnnotation >= 0 && resizingAnnotation < annotations.count else { return }
        let idx = resizingAnnotation
        let dx = point.x - annotationDragStartPoint.x
        let dy = point.y - annotationDragStartPoint.y
        if !annotationOpHistoryPushed && (dx != 0 || dy != 0) {
            pushAnnotationHistory()
            annotationOpHistoryPushed = true
        }
        // 从按下时快照还原再变换，避免累积误差（对齐 annotations[idx] = dragStartAnnotation）
        annotations[idx] = dragStartAnnotation
        if annotationResizeHandle == .arrowStart || annotationResizeHandle == .arrowEnd {
            // 箭头端点拖拽：仅移动对应端点，另一端点保持快照值不变
            let sx = Int((point.x - annotationDragStartPoint.x).rounded())
            let sy = Int((point.y - annotationDragStartPoint.y).rounded())
            if annotationResizeHandle == .arrowStart {
                annotations[idx].x1 = dragStartAnnotation.x1 + sx
                annotations[idx].y1 = dragStartAnnotation.y1 + sy
            } else {
                annotations[idx].x2 = dragStartAnnotation.x2 + sx
                annotations[idx].y2 = dragStartAnnotation.y2 + sy
            }
        } else {
            // 包围盒缩放：按拖拽手柄更新包围盒，防翻转后整体变换（矩形/圆均支持 8 手柄）
            let o = annotationResizeStartBox
            var n = o
            switch annotationResizeHandle {
            case .topLeft: n.origin.x = o.minX + dx; n.origin.y = o.minY + dy
                            n.size.width = o.maxX - n.minX; n.size.height = o.maxY - n.minY
            case .topRight: n.size.width = o.width + dx; n.origin.y = o.minY + dy
                             n.size.height = o.maxY - n.minY
            case .bottomLeft: n.origin.x = o.minX + dx; n.size.width = o.maxX - n.minX
                               n.size.height = o.height + dy
            case .bottomRight: n.size.width = o.width + dx; n.size.height = o.height + dy
            case .left: n.origin.x = o.minX + dx; n.size.width = o.maxX - n.minX
            case .right: n.size.width = o.width + dx
            case .top: n.origin.y = o.minY + dy; n.size.height = o.maxY - n.minY
            case .bottom: n.size.height = o.height + dy
            default: break
            }
            // 防翻转：规范化后保证宽高至少 2px（对齐 NormalizeRect + min 2px）
            n = n.standardized
            if n.width < 2 { n.size.width = 2 }
            if n.height < 2 { n.size.height = 2 }
            scTransformAnnotationByBox(&annotations[idx], oldBox: o, newBox: n)
        }
        // 局部脏区 = 上帧盒 ∪ 本帧盒，外扩手柄余量（性能审计：对齐 Windows
        // InvalidateAnnotationOp 的局部失效；handleMargin 覆盖选中手柄、描边与抗锯齿）
        invalidateAnnotationOpLocal(newBox: scMeasureAnnotationBounds(annotations[idx]))
    }

    /// 选中标注的整体拖动：对按下时快照做 dx/dy 平移后写回（避免累积误差）。
    /// 首次实际位移才入历史（对齐 annotationOpHistoryPushed 语义）。
    /// - Parameter point: 鼠标 CG 全局坐标
    func applyAnnotationMoveDrag(_ point: CGPoint) {
        guard draggingAnnotation >= 0 && draggingAnnotation < annotations.count else { return }
        let idx = draggingAnnotation
        let dx = Int((point.x - annotationDragStartPoint.x).rounded())
        let dy = Int((point.y - annotationDragStartPoint.y).rounded())
        if !annotationOpHistoryPushed && (dx != 0 || dy != 0) {
            pushAnnotationHistory()
            annotationOpHistoryPushed = true
        }
        annotations[idx] = dragStartAnnotation
        switch annotations[idx].type {
        case .rect, .circle, .arrow:
            annotations[idx].x1 += dx; annotations[idx].y1 += dy
            annotations[idx].x2 += dx; annotations[idx].y2 += dy
        case .brush, .mosaic:
            // 画笔/涂抹马赛克：整体平移路径；框选马赛克：平移两对角点
            //（框选马赛克不可拖动，此分支防御性保留，对齐 Windows draggingAnnotation 分支）
            if annotations[idx].type == .mosaic && annotations[idx].mosaicRect {
                annotations[idx].x1 += dx; annotations[idx].y1 += dy
                annotations[idx].x2 += dx; annotations[idx].y2 += dy
            } else {
                for i in annotations[idx].pts.indices {
                    annotations[idx].pts[i].x += dx
                    annotations[idx].pts[i].y += dy
                }
            }
        case .text:
            // 文字：平移锚点（对齐 Windows draggingTextAnnotation 分支语义）
            annotations[idx].x1 += dx; annotations[idx].y1 += dy
        }
        // 局部脏区 = 上帧盒 ∪ 本帧盒，外扩手柄余量（性能审计：对齐 Windows
        // InvalidateAnnotationOp 的局部失效；逐帧链式覆盖上一位置防拖拽残影）
        invalidateAnnotationOpLocal(newBox: scMeasureAnnotationBounds(annotations[idx]))
    }

    // ---- 绘制流程（CS_Drawing；对齐 OnLButtonDown/OnMouseMove/OnLButtonUp 绘制分支）----

    /// 开始绘制新标注（确认态 + 矢量工具激活 + 选区内点空白）：清除选中态，以当前
    /// 子菜单粗细/颜色初始化进行中标注，画笔记录路径起点。
    /// - Parameter point: 起点（CG 全局坐标）
    func beginAnnotationDrawing(at point: CGPoint) {
        guard let tool = activeTool, tool.isVectorTool else { return }
        selectedAnnotation = -1
        selectedTextAnnotation = -1   // 绘制态不显示文字选中边框（Windows 该边框仅确认/编辑态绘制）
        hasCurDrawing = true
        curDrawing = ScreenshotAnnotation(
            type: tool.annotationType,
            color: SC.colorPresets[drawColorIdx],
            thickness: SC.thickPresets[drawThickIdx])
        curDrawing.x1 = Int(point.x)
        curDrawing.y1 = Int(point.y)
        curDrawing.x2 = curDrawing.x1
        curDrawing.y2 = curDrawing.y1
        if curDrawing.type == .brush {
            curDrawing.pts = [ScreenshotAnnotationPoint(x: curDrawing.x1, y: curDrawing.y1)]
        }
        state = .drawing
        invalidateAll()
    }

    /// 更新进行中标注的终点/路径：终点钳制到选区内（对齐 CS_Drawing 的 max/min clamp），
    /// 画笔与涂抹马赛克逐点追加路径（框选马赛克更新终点）。
    /// - Parameter point: 鼠标 CG 全局坐标
    func updateAnnotationDrawing(_ point: CGPoint) {
        guard hasCurDrawing else { return }
        let ax = max(selection.minX, min(point.x, selection.maxX))
        let ay = max(selection.minY, min(point.y, selection.maxY))
        if curDrawing.type == .brush || (curDrawing.type == .mosaic && !curDrawing.mosaicRect) {
            curDrawing.pts.append(ScreenshotAnnotationPoint(x: Int(ax), y: Int(ay)))
            if curDrawing.type == .mosaic {
                // 涂抹马赛克局部脏区 = 本帧新圆邻域（揭示蒙版是圆并集只增不减，已揭示的
                // 前序区域不变；性能审计：沿用的整条路径包围盒随轨迹单调增长，长轨迹下
                // 逐帧重绘面积越来越大，揭示边界明显滞后于鼠标——「大圆不跟手」）。
                // 4px 外扩覆盖揭示 clip 边缘与抗锯齿。
                let r = CGFloat(max(1, curDrawing.brushRadius))
                invalidate(scInflate(CGRect(x: ax - r, y: ay - r, width: r * 2, height: r * 2), 4))
                return
            }
        } else {
            curDrawing.x2 = Int(ax)
            curDrawing.y2 = Int(ay)
        }
        // 局部脏区 = 正在绘制标注的当前包围盒外扩 4px（性能审计：对齐 Windows CS_Drawing
        // 分支 lastDrawingBox ∪ mouseBox 的局部失效；路径点/端点单调增长，当前包围盒恒
        // ⊇ 全部已画内容，4px 覆盖最大线宽一半与抗锯齿，快笔段亦不漏画）
        invalidate(scInflate(scMeasureAnnotationBounds(curDrawing), 4))
    }

    /// 结束绘制并提交（松手）：仅有效尺寸/路径入历史（矩形/椭圆/箭头 ≥2px，画笔 ≥2 点，
    /// 马赛克框选 ≥2px / 涂抹 ≥1 点——单击也产生一个马赛克圆），随后回到确认态并刷新工具栏
    /// 撤销可用态。
    func finishAnnotationDrawing() {
        var valid = false
        if hasCurDrawing {
            switch curDrawing.type {
            case .brush:
                valid = curDrawing.pts.count >= 2
            case .rect, .circle, .arrow:
                valid = abs(curDrawing.x2 - curDrawing.x1) >= 2 || abs(curDrawing.y2 - curDrawing.y1) >= 2
            case .mosaic:
                if curDrawing.mosaicRect {
                    valid = abs(curDrawing.x2 - curDrawing.x1) >= 2 || abs(curDrawing.y2 - curDrawing.y1) >= 2
                } else {
                    valid = curDrawing.pts.count >= 1
                }
            case .text:
                valid = false   // 文字不走绘制态（CS_TextEditing 单独提交流程）
            }
        }
        if valid {
            pushAnnotationHistory()
            annotations.append(curDrawing)
        }
        hasCurDrawing = false
        curDrawing = .empty
        state = .confirmed
        invalidateAll()
    }

    // ---- 绘制接线（paintConfirmedOverlay 调用）----

    /// 覆盖层绘制已提交标注 + 进行中标注（对齐 DrawAnnotations：绘制范围裁剪到选区内，
    /// 标注绝对坐标 → 视图本地偏移 = -cgOrigin）。确认/调整/绘制/文字编辑四态调用（移动态无标注）。
    func paintAnnotationsLayer(ctx: CGContext, view: OverlayScreenshotView) {
        guard state == .confirmed || state == .resizing || state == .drawing || state == .textEditing else { return }
        let localSel = selection.offsetBy(dx: -view.cgOrigin.x, dy: -view.cgOrigin.y)
        let ox = -view.cgOrigin.x
        let oy = -view.cgOrigin.y
        ctx.saveGState()
        // 限制绘制范围在选区内（对齐 graphics.SetClip(clipRect, CombineModeIntersect)）
        ctx.addRect(localSel)
        ctx.clip()
        for a in annotations {
            scDrawAnnotation(ctx, a, ox: ox, oy: oy)
        }
        if hasCurDrawing {
            scDrawAnnotation(ctx, curDrawing, ox: ox, oy: oy)
        }
        ctx.restoreGState()
        // 选中标注的视觉（虚线框/手柄）画在裁剪外，保证贴边手柄完整可见
        if selectedAnnotation >= 0 && selectedAnnotation < annotations.count {
            scPaintAnnotationSelection(ctx, view: view, annotations[selectedAnnotation])
        }
    }

    // ---- 确认输出合成 ----
    //「标注合成 + PNG 编码」已并入 ScreenshotOutputMac.swift 的统一输出管线
    // buildFinalPngOutput——确认输出与保存路径共用、长截图复用；原
    // compositeAnnotationsToPng 的合成逻辑（缩回逻辑尺寸 → 马赛克现场重算 →
    // 矢量/文字标注覆盖）已在其中原样保留，行为零变化。）
}

// MARK: - 基础小工具

/// 点集 → 闭合多边形路径（箭头多边形填充用；空点集返回单位空路径）。
func scPolygonPath(_ points: [CGPoint]) -> CGPath {
    let path = CGMutablePath()
    guard let first = points.first else { return path }
    path.move(to: first)
    for p in points.dropFirst() {
        path.addLine(to: p)
    }
    path.closeSubpath()
    return path
}
