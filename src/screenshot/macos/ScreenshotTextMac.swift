import Foundation
import AppKit
import CoreGraphics
import CoreText

// MARK: - 文字标注与编辑器（macOS）
//
// Windows 版文字链路（annotations_windows.cpp / overlay_input_windows.cpp / overlay_paint_windows.cpp）：
// - 输入态 = CS_TextEditing 会话子状态：textBuf 缓冲 + 锚点 + 插入符 + 选择区间，全自绘
// - IME：OnImeComposition 取 GCS_RESULTSTR 上屏串插入缓冲；可打印字符走 OnChar
// - 提交：6 条统一提交路径（见 commitPendingText 注释），CommitPendingText 构造参数完全一致
// - 测量：GDI+ MeasureString 紧凑字形包围盒；边框 = 字形盒 ± padding 4
//
// macOS 等价实现：
// - 覆盖层 NSView 实现 NSTextInputClient（本文件底部扩展）：insertText 承接「可打印字符 +
//   IME 上屏」两条入口（AppKit 把两者统一路由到该回调，语义合并 Windows OnChar 与
//   GCS_RESULTSTR）；setMarkedText/unmarkText 承接组词串（marked text）自绘下划线，
//   firstRect(forCharacterRange:) 返回插入符屏幕坐标供候选窗跟随
// - 测量用 Core Text（NSFont/CTLine）：字形紧凑包围盒 = useGlyphPathBounds；逐前缀累计宽度
//   与 Windows MeasureCharWidthsGdip 的 widths[i] 语义一致（前 i 个 UTF-16 单元的右缘）
// - 键系（Backspace/Delete/Left/Right/Home/End/Enter/ESC）自管，行为逐条对齐 OnKeyDown
//
// 字体差异说明：Windows 用微软雅黑（SC_FONT_FACE）；macOS 无该字体，使用系统字体
// （systemFont，CJK 回退苹方），度量数值与 Windows 不逐像素一致，计算结构完全同构。

// MARK: - 常量（Windows 出处集中标注）

extension SC {
    /// 文字字号预设，逻辑像素；文字标注 thickness 即字号（internal.h: SC_FONT_SIZES = { 16, 24, 36 }）
    static let fontSizes: [Int] = [16, 24, 36]
    /// 默认字号档：中号（internal.h: SC_DEFAULT_FONT_IDX = 1）
    static let defaultFontIdx = 1
    /// 文字标注最小字号（annotations_windows.cpp MeasureTextAnnotation：fontPx < 8 → 8）
    static let textMinFontSize = 8
    /// 输入框边框 padding（overlay_input_windows.cpp / overlay_paint_windows.cpp：padding = 4，
    /// 命中区与绘制边框完全一致）
    static let textPadding: CGFloat = 4
    /// 空缓冲时输入框的最小字形宽（overlay_paint_windows.cpp：glyphW 兜底 20）
    static let textMinGlyphW: CGFloat = 20
    /// 插入符闪烁间隔（session_windows.cpp 空闲循环：now - textCaretLastBlink >= 500 切换可见性）
    static let caretBlinkInterval: TimeInterval = 0.5
    /// 文字选择高亮底色（internal.h textSelBrush RGB(51,153,255)，AlphaBlend 常量 alpha 100）
    static let textSelBg = NSColor(srgbRed: 51.0 / 255.0, green: 153.0 / 255.0,
                                   blue: 255.0 / 255.0, alpha: 100.0 / 255.0)
    /// 文字选中边框蓝（internal.h annTextSelPen PS_SOLID 2 RGB(0,136,255)，与强调蓝同值）
    static let textSelectedBorder = SC.accentBlue
}

/// 空 NSRange（marked range 未激活语义，等价 Windows 无选择区间）。
let scNotFoundRange = NSRange(location: NSNotFound, length: 0)

// MARK: - 文字度量（对齐 MeasureTextGdip / MeasureCharWidthsGdip / CalcCaretPosFromMouse）

/// 文字度量结果：紧凑字形包围盒（相对锚点偏移 + 紧凑宽高）与逐字符累计宽度。
/// widths[i] = 前 i 个 UTF-16 单元的右缘（widths[0]=0），与渲染进度同源，
/// 光标 x = 锚点 x + widths[i]、选中高亮区间 = widths[s]..widths[e]。
struct ScreenshotTextMetrics {
    var offX: CGFloat = 0      // 字形左缘相对锚点偏移（通常 ≈0，侧边 bearing）
    var offY: CGFloat = 0      // 字形顶缘相对锚点偏移（布局顶 → 字形顶的字体内部空隙）
    var inkW: CGFloat = 0      // 字形紧凑宽
    var inkH: CGFloat = 0      // 字形紧凑高
    var widths: [CGFloat] = [0]
}

/// 测量文字的紧凑字形包围盒与逐前缀累计宽度（对齐 MeasureTextGdip + MeasureCharWidthsGdip）。
/// macOS 用 Core Text：整体行取 useGlyphPathBounds（字形墨迹紧凑盒）；
/// 逐前缀宽度按前缀串测量（O(n²)，文字标注为短文本，实测可接受；Windows 用
/// MeasureCharacterRanges 批量测，语义等价）。
/// 空文本：offX/offY/inkW = 0、inkH = fontPx、widths = [0]（对齐 Windows 空缓冲兜底）。
/// - Parameters:
///   - text: 待测文本
///   - fontPx: 字号（逻辑像素）
/// - Returns: 度量结果
func scMeasureTextMetrics(_ text: String, fontPx: CGFloat) -> ScreenshotTextMetrics {
    var m = ScreenshotTextMetrics()
    let font = NSFont.systemFont(ofSize: fontPx)
    guard !text.isEmpty else {
        m.inkH = fontPx
        m.widths = [0]
        return m
    }
    let attr = NSAttributedString(string: text, attributes: [.font: font])
    let line = CTLineCreateWithAttributedString(attr)
    var ascent: CGFloat = 0, descent: CGFloat = 0, leading: CGFloat = 0
    _ = CTLineGetTypographicBounds(line, &ascent, &descent, &leading)
    let ink = CTLineGetBoundsWithOptions(line, .useGlyphPathBounds)
    if ink.width > 0 && ink.height > 0 {
        m.offX = ink.minX
        m.offY = ascent - ink.maxY        // 基线 = 布局顶 + ascent，墨迹顶 = 布局顶 + offY
        m.inkW = ink.width
        m.inkH = ink.height
    } else {
        m.inkH = fontPx
    }
    var widths: [CGFloat] = [0]
    let ns = text as NSString
    for i in 1...ns.length {
        let prefix = attr.attributedSubstring(from: NSRange(location: 0, length: i))
        let pl = CTLineCreateWithAttributedString(prefix)
        widths.append(CGFloat(CTLineGetTypographicBounds(pl, nil, nil, nil)))
    }
    m.widths = widths
    return m
}

/// 绘制一行文字（顶部左对齐到锚点；对齐 DrawOneAnnotation AT_Text 分支的布局语义）。
/// 上下文必须为左上原点（覆盖层视图天然翻转；导出合成由调用方翻转）。
/// Core Text 在翻转上下文的标准绘制法：平移到基线后本地 scale(1,-1)。
/// - Parameters:
///   - ctx: 目标上下文（左上原点）
///   - text: 文本
///   - fontPx: 字号（逻辑像素）
///   - color: 文字颜色
///   - x/y: 锚点（布局顶左角）
func scDrawTextLine(_ ctx: CGContext, text: String, fontPx: CGFloat, color: NSColor,
                    x: CGFloat, y: CGFloat) {
    guard !text.isEmpty else { return }
    let attr = NSAttributedString(string: text, attributes: [.font: NSFont.systemFont(ofSize: fontPx),
                                                             .foregroundColor: color])
    let line = CTLineCreateWithAttributedString(attr)
    var ascent: CGFloat = 0, descent: CGFloat = 0, leading: CGFloat = 0
    _ = CTLineGetTypographicBounds(line, &ascent, &descent, &leading)
    ctx.saveGState()
    ctx.textMatrix = .identity
    ctx.translateBy(x: x, y: y + ascent)   // 基线 = 布局顶 + ascent
    ctx.scaleBy(x: 1, y: -1)
    ctx.textPosition = .zero
    CTLineDraw(line, ctx)
    ctx.restoreGState()
}

/// 测量文字标注的包围盒（对齐 MeasureTextAnnotation）：字形紧凑盒 ± padding 4，
/// 与编辑态输入框/命中区完全一致，保证选中边框 = resize 内容约束边界。
/// 空文字/非文字标注返回零矩形（对齐 Windows 空返回 { x1,y1,x1,y1 }）。
func scMeasureTextAnnotationBox(_ a: ScreenshotAnnotation) -> CGRect {
    guard a.type == .text, !a.text.isEmpty else {
        return CGRect(x: CGFloat(a.x1), y: CGFloat(a.y1), width: 0, height: 0)
    }
    let fontPx = CGFloat(max(a.thickness, SC.textMinFontSize))
    let m = scMeasureTextMetrics(a.text, fontPx: fontPx)
    let left = CGFloat(a.x1) + m.offX.rounded(.down) - SC.textPadding
    let top = CGFloat(a.y1) + m.offY.rounded(.down) - SC.textPadding
    let right = CGFloat(a.x1) + (m.offX + m.inkW).rounded(.up) + SC.textPadding
    let bottom = CGFloat(a.y1) + (m.offY + m.inkH).rounded(.up) + SC.textPadding
    return CGRect(x: left, y: top, width: right - left, height: bottom - top)
}

/// 命中测试文字标注（对齐 HitTestTextAnnotations）：从顶层向底层遍历，
/// 包围盒（含 padding 4）整体命中。马赛克不可选中由通用命中跳过，文字走本函数优先判定。
/// - Parameters:
///   - anns: 标注数组
///   - point: 鼠标 CG 全局坐标
/// - Returns: 命中索引；未命中 -1
func scHitTestTextAnnotations(_ anns: [ScreenshotAnnotation], _ point: CGPoint) -> Int {
    for i in stride(from: anns.count - 1, through: 0, by: -1) {
        if anns[i].type == .text, scPointInRect(point, scMeasureTextAnnotationBox(anns[i])) {
            return i
        }
    }
    return -1
}

/// 根据鼠标位置计算插入符位置（对齐 CalcCaretPosFromMouse）：逐边界取 x 距离最近者。
/// - Parameters:
///   - text: 编辑缓冲
///   - fontPx: 字号
///   - textX: 锚点 x
///   - mouseX: 鼠标 x（同坐标系）
/// - Returns: 插入符位置（UTF-16 单元偏移）
func scCalcCaretPosFromMouse(_ text: String, fontPx: CGFloat, textX: CGFloat, mouseX: CGFloat) -> Int {
    if text.isEmpty { return 0 }
    let m = scMeasureTextMetrics(text, fontPx: fontPx)
    var best = 0
    var bestDist = CGFloat.greatestFiniteMagnitude
    for i in 0..<m.widths.count {
        let d = abs(textX + m.widths[i] - mouseX)
        if d < bestDist {
            bestDist = d
            best = i
        }
    }
    return best
}

// MARK: - 会话扩展（文字编辑状态机：进入/提交/键系/IME/渲染/闪烁）

extension ScreenshotOverlaySession {
    /// 当前字号（fontSizeIdx 防越界兜底默认档）。
    func currentFontPx() -> CGFloat {
        let idx = (fontSizeIdx >= 0 && fontSizeIdx < SC.fontSizes.count) ? fontSizeIdx : SC.defaultFontIdx
        return CGFloat(SC.fontSizes[idx])
    }

    // MARK: 进入/退出编辑（对齐 OnLButtonDown TB_Text 分支 + CommitPendingTextAndExitEditing）

    /// 进入文字编辑态（文字工具激活 + 点击选区内空白）：清缓冲、锚点=点击点、插入符归零；
    /// 清除文字/非文字选中（对齐 Windows 进入输入态的清理序列），子菜单保持打开。
    /// - Parameter point: 点击点（CG 全局坐标）
    func beginTextEditing(at point: CGPoint) {
        textBuf = ""
        textAnchorX = point.x
        textAnchorY = point.y
        textCaretPos = 0
        textSelStart = -1
        textSelEnd = -1
        textDraggingSelection = false
        textMarkedRange = scNotFoundRange
        textCaretVisible = true
        textCaretLastBlink = ProcessInfo.processInfo.systemUptime
        hoveredTextAnnotation = -1
        selectedTextAnnotation = -1
        draggingTextAnnotation = -1
        selectedAnnotation = -1
        hasCurDrawing = false
        state = .textEditing
        textEditingFlag.set()   // event tap 对 ESC 放行（编辑态 ESC = 清缓冲，不取消截图）
        invalidateAll()
    }

    /// 把输入缓冲固化为一条文字标注并入栈历史（缓冲为空则无任何副作用）。
    /// 六条提交路径（1 编辑态点字号格 / 2 编辑态点颜色格 / 3 编辑态点工具栏按钮 /
    /// 4 编辑态选区内换位 / 5 编辑态点选区外退出 / 6 Enter 键）构造参数完全一致，
    /// 统一走本函数避免逐处手抄漂移（对齐 overlay_input_windows.cpp CommitPendingText）。
    func commitPendingText() {
        guard !textBuf.isEmpty else { return }
        pushAnnotationHistory()
        var a = ScreenshotAnnotation(type: .text,
                                     color: SC.colorPresets[drawColorIdx],
                                     thickness: SC.fontSizes[fontSizeIdx])
        a.x1 = Int(textAnchorX)
        a.y1 = Int(textAnchorY)
        a.text = textBuf
        annotations.append(a)
    }

    /// 提交文字后的完整编辑出口：清缓冲/光标/选择区间/组词串并回确认态
    ///（对齐 CommitPendingTextAndExitEditing；Enter 与选区内换位路径不经过本函数，
    /// 各自保持 Windows 原有清理序列）。
    func commitPendingTextAndExitEditing() {
        commitPendingText()
        textBuf = ""
        textCaretPos = 0
        textSelStart = -1
        textSelEnd = -1
        textMarkedRange = scNotFoundRange
        textEditingFlag.reset()
        state = .confirmed
        invalidateAll()
    }

    // MARK: 鼠标（对齐 OnLButtonDown / OnMouseMove / OnLButtonUp 的 CS_TextEditing 分支）

    /// 文字编辑态左键按下（工具栏/子菜单为独立浮层窗口，其点击提交路径在
    /// ScreenshotToolbarController 中处理，此处只承接覆盖层命中）：
    /// 1) 输入框内 → 光标定位 + 开始拖选；2) 选区内其他位置 → 提交并开始新输入（路径 4）；
    /// 3) 选区外 → 提交并退出编辑态（路径 5）。
    /// - Parameter point: 鼠标 CG 全局坐标
    func handleTextEditingMouseDown(_ point: CGPoint) {
        // 1) 输入框命中：命中区与绘制边框完全一致（同一计算，否则点边框附近会误判框外）
        let box = textEditingInputBox()
        if scPointInRect(point, box) {
            let caret = scCalcCaretPosFromMouse(textBuf, fontPx: currentFontPx(),
                                                textX: textAnchorX, mouseX: point.x)
            textCaretPos = caret
            textSelStart = caret
            textSelEnd = caret
            textDraggingSelection = true
            invalidateTextLine()
            return
        }
        // 2) 选区内其他位置：提交当前文字，开始新输入（保持编辑态；提交路径 4）
        if selection.contains(point) {
            commitPendingText()
            textBuf = ""
            textAnchorX = point.x
            textAnchorY = point.y
            textCaretPos = 0
            textSelStart = -1
            textSelEnd = -1
            textMarkedRange = scNotFoundRange
            invalidateAll()
            return
        }
        // 3) 选区外：提交并退出文字编辑态（提交路径 5）
        commitPendingTextAndExitEditing()
    }

    /// 编辑态拖动选择文字：按鼠标位置更新选择终点与插入符（对齐 OnMouseMove 分支）。
    /// - Parameter point: 鼠标 CG 全局坐标
    func handleTextEditingDragged(_ point: CGPoint) {
        guard textDraggingSelection else { return }
        let caret = scCalcCaretPosFromMouse(textBuf, fontPx: currentFontPx(),
                                            textX: textAnchorX, mouseX: point.x)
        textSelEnd = caret
        textCaretPos = caret
        invalidateTextLine()
    }

    /// 编辑态左键抬起：结束拖选，范围退化（起=终）时清除选择（对齐 OnLButtonUp 分支）。
    func handleTextEditingMouseUp() {
        guard textDraggingSelection else { return }
        textDraggingSelection = false
        if textSelStart == textSelEnd {
            textSelStart = -1
            textSelEnd = -1
        }
    }

    /// 编辑态输入框矩形（CG 全局坐标）：字形紧凑包围盒 + padding 4，空缓冲最小宽 20。
    /// 与绘制边框同一公式（对齐 overlay_input_windows.cpp inputBox / overlay_paint_windows.cpp
    /// 边框计算），保证点击边框附近不误判框外。
    func textEditingInputBox() -> CGRect {
        let fontPx = currentFontPx()
        let m = scMeasureTextMetrics(textBuf, fontPx: fontPx)
        let inkW = (m.inkW > SC.textMinGlyphW || !textBuf.isEmpty) ? m.inkW : SC.textMinGlyphW
        let inkH = m.inkH > 0 ? m.inkH : fontPx
        let inkLeft = textAnchorX + m.offX
        let inkTop = textAnchorY + m.offY
        let left = (inkLeft - SC.textPadding).rounded(.down)
        let top = (inkTop - SC.textPadding).rounded(.down)
        return CGRect(x: left, y: top,
                      width: (inkLeft + inkW + SC.textPadding).rounded(.up) - left,
                      height: (inkTop + inkH + SC.textPadding).rounded(.up) - top)
    }

    // MARK: 键系（对齐 OnKeyDown 的 CS_TextEditing 分支）

    /// 文字编辑态键盘处理。IME 与可打印字符优先交给 NSTextInputContext
    ///（路由到 NSTextInputClient 的 insertText/setMarkedText），其余键系自管：
    /// Enter=提交（路径 6）、ESC=清缓冲回确认态（不取消截图）、Backspace=删插入符前一字符
    ///（Windows VK_BACK 不处理选区语义，照搬）、ForwardDelete=删插入符处字符、
    /// Left/Right/Home/End=移动插入符。返回 true 表示事件已消费。
    /// - Parameters:
    ///   - event: 键盘事件
    ///   - inputContext: 覆盖层视图的输入上下文（nil 时跳过 IME 路由）
    /// - Returns: 是否已消费
    func handleTextEditingKeyDown(_ event: NSEvent, inputContext: NSTextInputContext?) -> Bool {
        if let ctx = inputContext, ctx.handleEvent(event) { return true }
        let len = (textBuf as NSString).length
        switch event.keyCode {
        case 36, 76:
            // 提交路径 6：Enter 提交（Windows Enter 路径历史上不清 textSelStart/End，照搬）
            commitPendingText()
            textBuf = ""
            textCaretPos = 0
            textMarkedRange = scNotFoundRange
            exitTextEditingState()
            return true
        case 53:
            // ESC：清缓冲回确认态（不是取消截图，再次 ESC 才取消）
            textBuf = ""
            textCaretPos = 0
            textMarkedRange = scNotFoundRange
            exitTextEditingState()
            return true
        case 51:
            // Backspace：删插入符前一字符（对齐 VK_BACK：仅删一字符，不处理选择区间）
            if textCaretPos > 0 && textCaretPos <= len {
                textBuf = (textBuf as NSString).replacingCharacters(
                    in: NSRange(location: textCaretPos - 1, length: 1), with: "")
                textCaretPos -= 1
                invalidateTextLine()
            }
            return true
        case 117:
            // Forward Delete：删插入符处字符（对齐 VK_DELETE 文字分支）
            if textCaretPos < len {
                textBuf = (textBuf as NSString).replacingCharacters(
                    in: NSRange(location: textCaretPos, length: 1), with: "")
                invalidateTextLine()
            }
            return true
        case 123:
            // Left：插入符左移一格（对齐 VK_LEFT）
            if textCaretPos > 0 {
                textCaretPos -= 1
                invalidateTextLine()
            }
            return true
        case 124:
            // Right：插入符右移一格（对齐 VK_RIGHT）
            if textCaretPos < len {
                textCaretPos += 1
                invalidateTextLine()
            }
            return true
        case 115:
            // Home：插入符到行首（对齐 VK_HOME）
            textCaretPos = 0
            invalidateTextLine()
            return true
        case 119:
            // End：插入符到行尾（对齐 VK_END）
            textCaretPos = len
            invalidateTextLine()
            return true
        case 125, 126:
            return true   // Up/Down：单行无移动语义，吞掉（对齐 Windows 无分支恒消费）
        default:
            return true   // 其余按键在编辑态恒消费（对齐 OnKeyDown 恒返回 0）
        }
    }

    /// 退出编辑态回确认态（Enter/ESC 路径共用收尾；不清理选择区间，对齐 Windows 原序列）。
    private func exitTextEditingState() {
        textEditingFlag.reset()
        state = .confirmed
        invalidateAll()
    }

    // MARK: NSTextInputClient（IME：组词串 + 上屏；对齐 OnChar / OnImeComposition）

    /// 插入已提交文本（覆盖层视图 insertText 回调转发）：直接键入与 IME 上屏共用入口
    ///（AppKit 统一路由，语义合并 Windows OnChar 与 GCS_RESULTSTR）。过滤控制字符
    ///（ch >= 32 && ch != 127）；IME 上屏时先移除组词串再插入。Windows 的 OnChar 插入后
    /// 保留选择区间（不自动删除选中文字），照搬同语义。
    /// - Parameters:
    ///   - aString: 待插入字符串（String 或 NSAttributedString）
    ///   - replacementRange: IME 指定的替换区间（直接插入路径忽略，与 Windows 一致）
    func textInsertText(_ aString: Any, replacementRange: NSRange) {
        guard state == .textEditing, isRunning else { return }
        let raw: String?
        switch aString {
        case let s as String: raw = s
        case let attr as NSAttributedString: raw = attr.string
        default: raw = nil
        }
        guard var s = raw, !s.isEmpty else { return }
        s = String(String.UnicodeScalarView(s.unicodeScalars.filter { $0.value >= 32 && $0.value != 127 }))
        guard !s.isEmpty else { return }
        removeMarkedTextFromBuffer()
        textInsertAtCaret(s)
        textMarkedRange = scNotFoundRange
        invalidateTextLine()
    }

    /// 组词串更新（覆盖层视图 setMarkedText 回调转发）：移除旧组词串 → replacementRange
    /// 有效时替换该区间，否则在插入符处插入 → 登记新 markedRange。IME 建议的组词内选中段
    ///（selectedRange 参数）自绘编辑器不实现，忽略；组词串随缓冲一起被键系/渲染单路径处理。
    /// - Parameters:
    ///   - aString: 组词串（String 或 NSAttributedString）
    ///   - replacementRange: IME 指定的替换区间（NSNotFound = 在插入符处插入）
    func textSetMarkedText(_ aString: Any, replacementRange: NSRange) {
        guard state == .textEditing, isRunning else { return }
        let raw: String?
        switch aString {
        case let s as String: raw = s
        case let attr as NSAttributedString: raw = attr.string
        default: raw = nil
        }
        guard let s = raw else { return }
        removeMarkedTextFromBuffer()
        let insertLen = (s as NSString).length
        guard insertLen > 0 else { return }
        let ns = textBuf as NSString
        if replacementRange.location != NSNotFound, replacementRange.length > 0,
           NSMaxRange(replacementRange) <= ns.length {
            let loc = max(0, min(replacementRange.location, ns.length))
            let len = min(replacementRange.length, ns.length - loc)
            textBuf = ns.replacingCharacters(in: NSRange(location: loc, length: len), with: s)
            textCaretPos = loc + insertLen
            textMarkedRange = NSRange(location: loc, length: insertLen)
        } else {
            let pos = max(0, min(textCaretPos, ns.length))
            textBuf = ns.replacingCharacters(in: NSRange(location: pos, length: 0), with: s)
            textCaretPos = pos + insertLen
            textMarkedRange = NSRange(location: pos, length: insertLen)
        }
        invalidateTextLine()
    }

    /// 组词终止（覆盖层视图 unmarkText 回调转发）：保留组词文本（已被用户确认的输入），
    /// 仅清除 marked 标记。
    func textUnmark() {
        guard state == .textEditing else { return }
        textMarkedRange = scNotFoundRange
        invalidateTextLine()
    }

    /// 移除缓冲中的组词串并把插入符退回组词起点（无组词串时无副作用）。
    private func removeMarkedTextFromBuffer() {
        guard textMarkedRange.location != NSNotFound, textMarkedRange.length > 0 else { return }
        let ns = textBuf as NSString
        let loc = max(0, min(textMarkedRange.location, ns.length))
        let len = min(textMarkedRange.length, ns.length - loc)
        if len > 0 {
            textBuf = ns.replacingCharacters(in: NSRange(location: loc, length: len), with: "")
        }
        textCaretPos = loc
        textMarkedRange = scNotFoundRange
    }

    /// 在插入符处插入文本（UTF-16 单元安全）。
    private func textInsertAtCaret(_ s: String) {
        let ns = textBuf as NSString
        let pos = max(0, min(textCaretPos, ns.length))
        textBuf = ns.replacingCharacters(in: NSRange(location: pos, length: 0), with: s)
        textCaretPos = pos + (s as NSString).length
    }

    // MARK: NSTextInputClient 查询（selectedRange/markedRange/firstRect 等）

    /// 当前选中/插入符区间（IME 候选定位用）：有选择返回选择区间，否则插入符空区间。
    func textSelectedRange() -> NSRange {
        if textSelStart >= 0 && textSelEnd >= 0 && textSelStart != textSelEnd {
            let loc = min(textSelStart, textSelEnd)
            return NSRange(location: loc, length: abs(textSelEnd - textSelStart))
        }
        return NSRange(location: max(0, min(textCaretPos, (textBuf as NSString).length)), length: 0)
    }

    /// 当前组词区间（无组词时为 NSNotFound 空区间）。
    /// （命名避开会话同名存储属性 textMarkedRange。）
    func textCurrentMarkedRange() -> NSRange {
        return textMarkedRange
    }

    /// 是否有组词串。
    func textHasMarkedText() -> Bool {
        return textMarkedRange.location != NSNotFound && textMarkedRange.length > 0
    }

    /// 取区间内的属性子串（部分 IME 查询用；带当前字号字体）。
    /// - Parameters:
    ///   - range: 请求区间（UTF-16 单元）
    ///   - actualRange: 回填实际可用区间（nil 可忽略）
    /// - Returns: 属性子串；区间无效返回 nil
    func textAttributedSubstring(for range: NSRange, actualRange: NSRangePointer?) -> NSAttributedString? {
        let ns = textBuf as NSString
        guard range.location != NSNotFound, range.length > 0, range.location < ns.length else { return nil }
        let r = NSRange(location: range.location,
                        length: min(range.length, ns.length - range.location))
        actualRange?.pointee = r
        return NSAttributedString(string: ns.substring(with: r),
                                  attributes: [.font: NSFont.systemFont(ofSize: currentFontPx())])
    }

    /// 插入符矩形（CG 全局坐标）：候选窗跟随与闪烁失效共用（宽 2px 对齐 Windows 2px 光标笔）。
    func textCaretRectGlobal() -> CGRect {
        let m = scMeasureTextMetrics(textBuf, fontPx: currentFontPx())
        let idx = max(0, min(textCaretPos, m.widths.count - 1))
        let inkH = m.inkH > 0 ? m.inkH : currentFontPx()
        return CGRect(x: textAnchorX + m.widths[idx], y: textAnchorY + m.offY,
                      width: 2, height: inkH.rounded(.up))
    }

    /// 命中点 → 字符索引（覆盖层视图 characterIndex(for:) 转发）。
    /// - Parameter point: 命中点（CG 全局坐标）
    /// - Returns: 最近字符边界（UTF-16 单元偏移）
    func textCharacterIndex(at point: CGPoint) -> Int {
        return scCalcCaretPosFromMouse(textBuf, fontPx: currentFontPx(),
                                       textX: textAnchorX, mouseX: point.x)
    }

    // MARK: 闪烁 / 局部失效 / 渲染

    /// 泵循环逐拍任务：编辑态插入符 500ms 闪烁（对齐 session_windows.cpp 空闲循环分支），
    /// 仅失效光标附近区域。
    /// - Parameter now: 单调时钟（ProcessInfo.systemUptime）
    func tickTextCaret(now: TimeInterval) {
        guard state == .textEditing else { return }
        if now - textCaretLastBlink >= SC.caretBlinkInterval {
            textCaretVisible.toggle()
            textCaretLastBlink = now
            if !lastCaretRect.isNull {
                invalidate(scInflate(lastCaretRect, 2))
            } else {
                invalidateAll()
            }
        }
    }

    /// 使文字行区域失效（对齐 InvalidateTextLine）：选区宽度 × 上帧光标行高（选区宽度是
    /// 文字行宽的安全上界）；无光标缓存时全屏失效兜底。
    func invalidateTextLine() {
        if lastCaretRect.isNull {
            invalidateAll()
            return
        }
        let line = CGRect(x: selection.minX, y: lastCaretRect.minY,
                          width: selection.width, height: lastCaretRect.height)
        invalidate(scInflate(line, 4))
    }

    /// 编辑态覆盖层渲染（对齐 OnPaint CS_TextEditing 分支；调用点在 paintConfirmedOverlay，
    /// 已提交标注由 paintAnnotationsLayer 先行绘制）：
    /// 顺序 = 文字 → 输入框边框 → 选择高亮 → 组词下划线 → 插入符（对齐 Windows 绘制次序）。
    func paintTextEditingLayer(ctx: CGContext, view: OverlayScreenshotView) {
        guard state == .textEditing else { return }
        let fontPx = currentFontPx()
        let c = SC.colorPresets[drawColorIdx]
        let textColor = NSColor(srgbRed: CGFloat(c.r) / 255.0, green: CGFloat(c.g) / 255.0,
                                blue: CGFloat(c.b) / 255.0, alpha: 1.0)
        let textX = textAnchorX - view.cgOrigin.x
        let textY = textAnchorY - view.cgOrigin.y
        let m = scMeasureTextMetrics(textBuf, fontPx: fontPx)

        // 1) 文字（与提交态 scDrawAnnotation 同一渲染函数，输入/提交视觉一致）
        if !textBuf.isEmpty {
            scDrawTextLine(ctx, text: textBuf, fontPx: fontPx, color: textColor, x: textX, y: textY)
        }

        // 2) 输入框边框：字形紧凑包围盒 + padding 4（空缓冲最小宽 20，高度兜底字号；
        //    与命中区 textEditingInputBox 同一公式，保证点击边框附近不误判框外）
        let inkH = m.inkH > 0 ? m.inkH : fontPx
        let glyphTop = textY + m.offY
        let box = textEditingInputBox().offsetBy(dx: -view.cgOrigin.x, dy: -view.cgOrigin.y)
        ctx.setStrokeColor(textColor.cgColor)
        ctx.setLineWidth(1)
        ctx.stroke(box)

        // 3) 选择高亮：区间 [widths[s], widths[e]] × 字形高（alpha 100 叠加，对齐 AlphaBlend）
        if textSelStart >= 0 && textSelEnd >= 0 && textSelStart != textSelEnd,
           textSelStart < m.widths.count, textSelEnd < m.widths.count {
            let s = min(textSelStart, textSelEnd)
            let e = max(textSelStart, textSelEnd)
            let selRect = CGRect(x: textX + m.widths[s], y: glyphTop,
                                 width: m.widths[e] - m.widths[s], height: inkH.rounded(.up))
            if selRect.width > 0 {
                ctx.setFillColor(SC.textSelBg.cgColor)
                ctx.fill(selRect)
            }
        }

        // 4) 组词串（marked text）下划线：区间宽度差画在字形底部下方（macOS 特有自绘，
        //    Windows 无组词显示——GCS_RESULTSTR 直接上屏）
        if textHasMarkedText() {
            let s = textMarkedRange.location
            let e = s + textMarkedRange.length
            if s >= 0, e < m.widths.count, m.widths[e] > m.widths[s] {
                let underline = CGRect(x: textX + m.widths[s], y: glyphTop + inkH + 2,
                                       width: m.widths[e] - m.widths[s], height: 1.5)
                ctx.setFillColor(textColor.cgColor)
                ctx.fill(underline)
            }
        }

        // 5) 插入符：x = 锚点 + widths[caret]，高 = 字形高；几何始终缓存（含不可见帧，
        //    对齐 lastCaretRect 语义）供闪烁/键系局部刷新
        let caretIdx = max(0, min(textCaretPos, m.widths.count - 1))
        let caretX = textX + m.widths[caretIdx]
        lastCaretRect = CGRect(x: (caretX - 1) + view.cgOrigin.x,
                               y: glyphTop + view.cgOrigin.y,
                               width: 4, height: inkH.rounded(.up))
        if textCaretVisible {
            ctx.setFillColor(textColor.cgColor)
            ctx.fill(CGRect(x: caretX, y: glyphTop, width: 2, height: inkH.rounded(.up)))
        }
    }

    // MARK: 文字标注选中/拖动/回显（对齐 OnLButtonDown 文字命中分支）

    /// 选中文字标注的脏区矩形（清选中视觉时局部失效用；未选中返回 nil）。
    func selectedTextAnnotationDirtyRect() -> CGRect? {
        guard selectedTextAnnotation >= 0 && selectedTextAnnotation < annotations.count else { return nil }
        return scInflate(scMeasureTextAnnotationBox(annotations[selectedTextAnnotation]), SC.handleMargin)
    }

    /// 选中文字标注并进入拖动（确认态点击命中文字）：清非文字选中、文字工具回显
    ///（activeTool=文字、子菜单开、字号/颜色回显），随后拖动改锚点。
    /// - Parameters:
    ///   - index: 命中的文字标注索引
    ///   - point: 按下点（CG 全局坐标）
    func selectTextAnnotation(_ index: Int, at point: CGPoint) {
        guard index >= 0 && index < annotations.count else { return }
        let dirty = selectedTextAnnotationDirtyRect() ?? selectedAnnotationDirtyRect()
        selectedAnnotation = -1
        hoveredAnnotation = -1
        selectedTextAnnotation = index
        hoveredTextAnnotation = index
        // 工具栏回显：文字工具高亮 + 字号/颜色子菜单回显该标注参数（对齐 EchoFontIdx/EchoColorIdx）
        activeTool = .text
        toolbar.openPopup(for: .text)
        scEchoFontIdx(annotations[index].thickness)
        scEchoColorIdx(annotations[index].color)
        draggingTextAnnotation = index
        textDragStartPoint = point
        textDragStartAnchor = CGPoint(x: CGFloat(annotations[index].x1), y: CGFloat(annotations[index].y1))
        annotationOpHistoryPushed = false
        lastAnnotationOpBox = .null   // 进入文字标注拖拽：复位上帧脏区链（局部失效）
        invalidate(dirty)
        invalidateAll()
    }

    /// 拖动文字标注：对按下时锚点快照做位移后写回；首次实际位移才入历史
    ///（对齐 OnMouseMove draggingTextAnnotation 分支）。
    /// - Parameter point: 鼠标 CG 全局坐标
    func applyTextAnnotationMoveDrag(_ point: CGPoint) {
        guard draggingTextAnnotation >= 0 && draggingTextAnnotation < annotations.count else { return }
        let idx = draggingTextAnnotation
        let dx = Int((point.x - textDragStartPoint.x).rounded())
        let dy = Int((point.y - textDragStartPoint.y).rounded())
        if !annotationOpHistoryPushed && (dx != 0 || dy != 0) {
            pushAnnotationHistory()
            annotationOpHistoryPushed = true
        }
        annotations[idx].x1 = Int(textDragStartAnchor.x) + dx
        annotations[idx].y1 = Int(textDragStartAnchor.y) + dy
        // 局部脏区 = 上帧盒 ∪ 本帧盒，外扩手柄余量（性能审计：对齐 Windows
        // InvalidateAnnotationOp 的局部失效，拖拽热路径不逐帧全屏重绘）
        invalidateAnnotationOpLocal(newBox: scMeasureAnnotationBounds(annotations[idx]))
    }

    // MARK: 编辑态子菜单点击（提交路径 1/2 的索引设置步）

    /// 编辑态点击字号/颜色格：仅设置索引（新属性将用于随后的提交，对齐 Windows 先设索引
    /// 再 CommitPendingTextAndExitEditing 的次序），提交由工具栏控制器紧接着触发。
    /// - Parameter hit: 子菜单命中码（>0 字号 / <0 颜色）
    func applyTextEditingPopupSelection(_ hit: Int) {
        guard state == .textEditing, toolbar.popupTool == .text else { return }
        if hit > 0 && hit <= SC.fontSizes.count {
            fontSizeIdx = hit - 1
        } else if hit < 0 && -hit <= SC.colorPresets.count {
            drawColorIdx = -hit - 1
        }
    }
}

// MARK: - 参数回显（对齐 overlay_input_windows.cpp EchoFontIdx / EchoColorIdx / EchoThickIdx）

extension ScreenshotOverlaySession {
    /// 字号回显（会话内写回 fontSizeIdx；对齐 EchoFontIdx：顺序查找首个匹配，无匹配不变）。
    func scEchoFontIdx(_ fontSizePx: Int) {
        for i in SC.fontSizes.indices where SC.fontSizes[i] == fontSizePx {
            fontSizeIdx = i
            break
        }
        toolbar.refresh()
    }

    /// 颜色回显（会话内写回 drawColorIdx；对齐 EchoColorIdx）。
    func scEchoColorIdx(_ color: ScreenshotRGB) {
        for i in SC.colorPresets.indices where SC.colorPresets[i] == color {
            drawColorIdx = i
            break
        }
        toolbar.refresh()
    }

    /// 粗细回显（会话内写回 drawThickIdx；对齐 EchoThickIdx）。
    func scEchoThickIdx(_ thicknessPx: Int) {
        for i in SC.thickPresets.indices where SC.thickPresets[i] == thicknessPx {
            drawThickIdx = i
            break
        }
        toolbar.refresh()
    }
}

// MARK: - NSTextInputClient（覆盖层视图承接系统输入法契约）

/// 覆盖层视图的 NSTextInputClient 实现：AppKit 输入系统经 first responder（覆盖层视图）
/// 的 inputContext 与本会话交换文本。组词期间 IME 候选窗位置由
/// firstRect(forCharacterRange:) 提供（返回插入符屏幕坐标）。
extension OverlayScreenshotView: NSTextInputClient {
    /// 已提交文本插入（直接键入字符与 IME 上屏共用入口；对齐 OnChar + GCS_RESULTSTR）。
    func insertText(_ aString: Any, replacementRange: NSRange) {
        session.textInsertText(aString, replacementRange: replacementRange)
    }

    /// 组词串更新（拼音/日文 IME 组合过程；会话自绘下划线高亮）。
    func setMarkedText(_ aString: Any, selectedRange: NSRange, replacementRange: NSRange) {
        session.textSetMarkedText(aString, replacementRange: replacementRange)
    }

    /// 组词终止：保留组词文本并清除 marked 标记。
    func unmarkText() {
        session.textUnmark()
    }

    /// 当前选中/插入符区间。
    func selectedRange() -> NSRange {
        return session.textSelectedRange()
    }

    /// 当前组词区间。
    func markedRange() -> NSRange {
        return session.textCurrentMarkedRange()
    }

    /// 是否有组词串。
    func hasMarkedText() -> Bool {
        return session.textHasMarkedText()
    }

    /// 取区间属性子串。
    func attributedSubstring(forProposedRange range: NSRange,
                             actualRange: NSRangePointer?) -> NSAttributedString? {
        return session.textAttributedSubstring(for: range, actualRange: actualRange)
    }

    /// 支持的组词属性（最小集：字体；IME 仅用于查询展示）。
    func validAttributesForMarkedText() -> [NSAttributedString.Key] {
        return [.font]
    }

    /// IME 候选窗定位：返回插入符矩形的屏幕坐标（视图本地 → 窗口基底 → convertToScreen）。
    /// 拼音组词时候选窗跟随插入符的关键回调。
    func firstRect(forCharacterRange range: NSRange, actualRange: NSRangePointer?) -> NSRect {
        let cg = session.textCaretRectGlobal()
        let local = CGRect(x: cg.minX - cgOrigin.x, y: cg.minY - cgOrigin.y,
                           width: cg.width, height: cg.height)
        guard let window = window else { return NSRect.zero }
        return window.convertToScreen(convert(local, to: nil))
    }

    /// 命中点 → 字符索引（点选定位；参数为窗口基底坐标，换算 CG 后按 x 就近取边界）。
    func characterIndex(for point: NSPoint) -> Int {
        let local = convert(point, from: nil)
        let cg = CGPoint(x: (local.x + cgOrigin.x).rounded(), y: (local.y + cgOrigin.y).rounded())
        return session.textCharacterIndex(at: cg)
    }
}
