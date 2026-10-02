import Foundation
import AppKit
import CoreGraphics
import CoreText
import ImageIO

// MARK: - 截图翻译子系统（macOS；Windows 基准 translate_windows.cpp）
//
// 本文件承载截图翻译的 macOS 实现（对齐 Windows translate_windows.cpp 全流程）：
// - 原生桥函数指针：binding_mac.cpp 库加载时经 ztoolsRegisterNativeBridge 注入
//   provider 异步调用 / 请求取消 / 就绪查询三个 C 包装；结果回投经本文件导出的
//   ztoolsSwiftProviderResultHandler（bridge 异步回调 → 全局邮箱）送回会话主线程；
// - 状态机（全异步，由 provider 回调与会话定时器 deadline 扫描推进，会话主线程独占）：
//   Idle --点击「翻译」--> OcrPending --OCR 回调--> Clustering（瞬态）
//   --聚类完成--> TranslationPending --末段回调--> Layout（瞬态）--> Shown；
//   任何阶段失败 → Idle（错误气泡 4s TTL）；进行中任何 pending 态可取消 → Idle；
//   Shown 再点 = 切换退出（清覆盖、取消按钮激活，不重跑）；
// - 异步 RPC：Swift 发起 InvokeAsync(requestId, type, payload) 后立即返回，JS 侧
//   完成后按 requestId 异步回投；requestId/jobId 全局自增唯一，晚到结果（已超时/
//   已取消/任务已换代）经 requestId 反查落空直接丢弃，绝不污染当前 UI 状态；
// - 翻译队列：段落聚类完成后入 pending 队列，按 maxConcurrency=2 有限并发逐段
//   翻译（不要求严格顺序返回，requestId→paragraphIndex 写回，渲染按原始顺序）；
// - 超时：每个在飞请求带 deadline（会话定时器逐拍扫描），到时 CancelRequest(requestId)
//   + 按失败收束该段（底层 JS Promise/网络无法真取消，但晚到回调被桥丢弃）；
// - 取消：会话收尾 / 退出翻译态时 cancelJob（全部在飞请求取消、队列清空）；
// - 渲染：白底圆角面板盖原段落 + 译文按段落区域自适应字号折行排版（CoreText
//   度量；字号 = min(能装进段框的最大字号, 原文行距对应字号)，每段独立随其
//   框高与行数变化），OnPaint 画在标注之上、选区边框之下；导出合成进最终
//   图像（物理分辨率重渲染）。
//
// 线程模型（非阻塞会话）：
// - 截图会话为非阻塞生命周期对象：start 后 JS 主线程立即返回，Node/libuv/V8 与
//   AppKit 事件全部由宿主进程自己的主事件循环驱动（Electron 主进程的 Chromium
//   消息泵同时运转 NSApp 与 libuv），截图模块不再泵 uv_run/V8 微任务；
// - Swift 发起 InvokeAsync 后立即返回，宿主事件循环自行推进 JS provider 的
//   Promise 链 / 定时器 / 网络 I/O，并把结果经 TSFN 派发回投；
// - 结果回投：provider 回调（JS 线程触发；Electron 主进程内 JS 线程即主线程）
//   → 全局结果邮箱（NSLock，上限防膨胀）→ 会话 16ms 定时器 tick drain
//   → 状态机推进 / 晚到结果丢弃。UI 状态变更全部发生在会话主线程，
//   无跨线程共享可变状态。
//
// 字体差异：Windows 用微软雅黑；macOS 系统字体（CJK 回落苹方），度量数值不逐像素
// 一致，排版计算结构同构（与 ScreenshotTextMac.swift 同款处理）。
//
// provider 约定（与 Windows 完全一致，宿主 JS 侧 ProviderBridge.start 的 handler）：
//   ocr:        入参 {"image":"data:image/png;base64,..."}（选区裁剪，物理像素）
//          =>  {"text":"..","blocks":[{"text":..,"left":..,"top":..,"right":..
//               "bottom":..},..]}，blocks 为行级对象（提交图像内像素坐标）；
//               仅回 text / blocks 为字符串数组（无坐标）时走整图兜底
//   translation: 入参 {"text":"段落文本"} => {"text":"译文"}
//
// 坐标系：块坐标为 CG 全局逻辑坐标（左上原点、整数点，对齐选区坐标系）。

// MARK: - 原生桥函数指针（binding_mac.cpp 库加载时注入，进程级一次）

/// provider 桥异步调用签名（C 侧 ZtoolsProviderInvokeAsyncForSwift）：
/// 返回 1 已受理（结果将经 ztoolsSwiftProviderResultHandler 异步回投，恰好一次）/
/// 0 立即拒绝（*errorOut 为 malloc 的可读错误，不会触发回投；调用方 free）。
/// 非阻塞，可在任意线程（含会话主线程 = JS 主线程）调用。
public typealias ScNativeProviderInvokeAsyncFn = @convention(c) (
    UInt64, UnsafePointer<CChar>?, UnsafePointer<CChar>?,
    UnsafeMutablePointer<UnsafeMutablePointer<CChar>?>?
) -> Int32
/// provider 请求取消签名（C 侧 ZtoolsProviderCancelRequestForSwift）：
/// 移除请求登记，此后晚到的 JS 结果被桥静默丢弃（不触发回投）；幂等。
public typealias ScNativeProviderCancelFn = @convention(c) (UInt64) -> Void
/// provider 桥就绪查询签名（C 侧 ZtoolsProviderIsReadyForSwift）。
public typealias ScNativeProviderReadyFn = @convention(c) () -> Int32
/// provider 结果回投签名（Swift 侧 ztoolsSwiftProviderResultHandler，C 桥调用）：
/// requestId 匹配发起方；ok=1 时 valueJson 为结果 JSON，否则 error 为可读错误。
/// 触发线程 = 桥回调来源线程（通常为 JS 线程），实现只做线程安全入队。
public typealias ScNativeProviderResultFn = @convention(c) (
    UInt64, Int32, UnsafePointer<CChar>?, UnsafePointer<CChar>?
) -> Void

/// 注入的原生桥函数指针（模块级；未注入 = .node 侧为旧版本，翻译降级为错误气泡）。
var scNativeProviderInvokeAsync: ScNativeProviderInvokeAsyncFn? = nil
var scNativeProviderCancel: ScNativeProviderCancelFn? = nil
var scNativeProviderReady: ScNativeProviderReadyFn? = nil

/// 原生桥注册入口（binding_mac.cpp 的 LoadSwiftLibrary 在 dlsym 后调用，进程一次）。
/// - Parameters:
///   - invokeAsync: provider 桥异步调用包装
///   - cancel: provider 请求取消包装
///   - ready: provider 桥就绪查询包装
/// - Returns: 1 已注册（指针任一为空时仍注册非空者并返回 0）
@_cdecl("ztoolsRegisterNativeBridge")
public func ztoolsRegisterNativeBridge(
    _ invokeAsync: ScNativeProviderInvokeAsyncFn?,
    _ cancel: ScNativeProviderCancelFn?,
    _ ready: ScNativeProviderReadyFn?
) -> Int32 {
    scNativeProviderInvokeAsync = invokeAsync
    scNativeProviderCancel = cancel
    scNativeProviderReady = ready
    return (invokeAsync != nil && cancel != nil && ready != nil) ? 1 : 0
}

// MARK: - provider 结果邮箱（桥回调线程 → 会话主线程的唯一交接点）

/// 一次异步 provider 调用的回投结果（值拷贝，跨线程无共享状态）。
struct ScProviderAsyncResult {
    let requestId: UInt64
    let ok: Bool
    let valueJson: String
    let error: String
    let deliveredAt: TimeInterval   // systemUptime（诊断耗时用）
}

/// 全局结果邮箱：ztoolsSwiftProviderResultHandler（桥回调线程，通常为 JS 线程）
/// 只做加锁入队；会话定时器逐拍 drain 后按 requestId 路由。容量上限防「会话已收
/// 尾且无后续消费者」的残留膨胀——超限丢最旧并记日志。
final class ScProviderResultMailbox {
    static let shared = ScProviderResultMailbox()
    private let lock = NSLock()
    private var queue: [ScProviderAsyncResult] = []
    private let capacity = 64

    /// 入队一条结果（桥回调线程调用；超限丢最旧）。
    func post(_ result: ScProviderAsyncResult) {
        lock.lock()
        queue.append(result)
        if queue.count > capacity {
            queue.removeFirst(queue.count - capacity)
        }
        lock.unlock()
    }

    /// 取走全部积压结果（会话主线程调用，读后清空）。
    func drain() -> [ScProviderAsyncResult] {
        lock.lock()
        defer { lock.unlock() }
        let r = queue
        queue.removeAll()
        return r
    }
}

/// provider 结果回投入口（C 桥调用，线程 = 桥回调来源线程，通常为 JS 线程）。
/// 只做字符串拷贝 + 入邮箱；绝不在此线程触碰任何 UI / 会话状态。
@_cdecl("ztoolsSwiftProviderResultHandler")
public func ztoolsSwiftProviderResultHandler(
    _ requestId: UInt64, _ ok: Int32,
    _ valueJson: UnsafePointer<CChar>?, _ error: UnsafePointer<CChar>?
) {
    let result = ScProviderAsyncResult(
        requestId: requestId,
        ok: ok == 1,
        valueJson: valueJson.map { String(cString: $0) } ?? "",
        error: error.map { String(cString: $0) } ?? "",
        deliveredAt: ProcessInfo.processInfo.systemUptime)
    ScProviderResultMailbox.shared.post(result)
}

// MARK: - requestId / jobId 分配与结构化日志

/// 全局 id 分配器（NSLock 保护；低频调用，锁开销可忽略）。
private let scTranslateIdLock = NSLock()
private var scTranslateNextJobId: UInt64 = 0
private var scTranslateNextRequestId: UInt64 = 0

/// 分配下一个翻译任务号（进程内单调递增，跨会话唯一）。
func scAllocateTranslateJobId() -> UInt64 {
    scTranslateIdLock.lock()
    defer { scTranslateIdLock.unlock() }
    scTranslateNextJobId += 1
    return scTranslateNextJobId
}

/// 分配下一个 provider 请求号（进程内单调递增，跨会话唯一）。
func scAllocateTranslateRequestId() -> UInt64 {
    scTranslateIdLock.lock()
    defer { scTranslateIdLock.unlock() }
    scTranslateNextRequestId += 1
    return scTranslateNextRequestId
}

/// 请求生命周期日志（[translate][job=J][req=R][type][para=P] EVENT detail）。
/// 任何一次「翻译卡住」都能凭 jobId/requestId 定位到具体阶段。
func scTLog(_ event: String, job: UInt64? = nil, req: UInt64? = nil,
            type: String? = nil, para: Int? = nil, _ detail: String = "") {
    var line = "[translate]"
    if let j = job { line += "[job=\(j)]" }
    if let r = req { line += "[req=\(r)]" }
    if let t = type { line += "[\(t)]" }
    if let p = para { line += "[para=\(p)]" }
    line += " \(event)"
    if !detail.isEmpty { line += " \(detail)" }
    print(line)
}

// MARK: - 聚类算法 C ABI 镜像（translate_bridge_mac.h 的 Swift 同构布局）

/// 聚类输入行（镜像 TcLineC；text 指针仅在 tcClusterLines 调用期间有效）。
struct TcLineC {
    var text: UnsafePointer<CChar>? = nil
    var x: Double = 0
    var y: Double = 0
    var w: Double = 0
    var h: Double = 0
}

/// 聚类产出的段落（镜像 TcParagraphC；text/lineIdx 由 C 层分配，随释放函数回收）。
struct TcParagraphC {
    var x: Double = 0
    var y: Double = 0
    var w: Double = 0
    var h: Double = 0
    var lineHeight: Double = 0
    var text: UnsafeMutablePointer<CChar>? = nil
    var lineIdx: UnsafeMutablePointer<Int32>? = nil
    var lineCount: Int32 = 0
}

/// 聚类结果（镜像 TcClusterResultC）。
struct TcClusterResultC {
    var paragraphCount: Int32 = 0
    var paragraphs: UnsafeMutablePointer<TcParagraphC>? = nil
    var medianLineH: Double = 0
    var medianPitch: Double = 0
    var boundaryCount: Int32 = 0
}

@_silgen_name("tc_abi_version")
private func tcAbiVersionC() -> Int32
@_silgen_name("tc_cluster_lines")
private func tcClusterLinesC(_ lines: UnsafePointer<TcLineC>?, _ lineCount: Int32,
                             _ out: UnsafeMutablePointer<TcClusterResultC>?) -> Int32
@_silgen_name("tc_cluster_free_result")
private func tcClusterFreeResultC(_ result: UnsafeMutablePointer<TcClusterResultC>?)

// MARK: - 常量（Windows translate_windows.cpp 出处集中标注）

extension SC {
    /// OCR provider 调用超时（translate_windows.cpp SC_OCR_TIMEOUT_MS）
    static let translateOcrTimeoutMs: UInt32 = 30000
    /// 单段翻译 provider 调用超时（SC_TRANSLATE_LINE_TIMEOUT_MS）
    static let translateLineTimeoutMs: UInt32 = 15000
    /// 段落翻译有限并发上限（并发请求出队列补充；尊重 provider 限流）
    static let translateMaxConcurrency = 2
    /// 错误状态气泡自动隐藏时长（SC_TRANSLATE_ERROR_TTL_MS）
    static let translateErrorTtlSec: TimeInterval = 4.0
    /// 译文面板内边距 / 圆角（sctranslatedraw PANEL_PAD / PANEL_RADIUS）
    static let translatePanelPad: CGFloat = 4
    static let translatePanelRadius: CGFloat = 4
    /// 译文字号搜索区间（sctranslatedraw FONT_MIN_PX / FONT_MAX_PX）
    static let translateFontMinPx = 9
    static let translateFontMaxPx = 48
    /// 整图兜底块（无行级特征）的默认字号上限（sctranslatedraw FALLBACK_FONT_PX）
    static let translateFallbackFontPx: CGFloat = 16
    /// 状态气泡内边距与字号（SetTranslateStatus padX=8/padY=5、12px 字体）
    static let translateBubblePadX: CGFloat = 8
    static let translateBubblePadY: CGFloat = 5
    static let translateBubbleFontPx: CGFloat = 12
    static let translateBubbleRadius: CGFloat = 4
}

// MARK: - 翻译状态与数据结构

/// 翻译覆盖状态机（异步化后的显式状态；对齐设计文档，UI 语义对齐 Windows 的
/// TRL_Idle/Busy/Shown 三态）。状态推进只发生在会话主线程：由 provider 回调
/// （邮箱 drain）或 deadline 扫描驱动，不存在「等待某个同步函数返回」的推进。
/// Clustering / Layout 为会话主线程内同步完成的瞬态（跨拍不持久，仅用于日志与防御）。
enum ScreenshotTranslateState {
    case idle                // 无覆盖
    case ocrPending          // OCR 请求在飞（进度气泡）
    case clustering          // OCR 已回：行聚类成段落（瞬态）
    case translationPending  // 段落翻译队列推进中（进度气泡）
    case layout              // 全部段落已回：坐标换算 + 构块（瞬态）
    case shown               // 译文覆盖展示中

    /// 是否处于「进行中」聚合态（对应旧 TRL_Busy 的门禁/进度气泡语义）。
    var isBusy: Bool {
        return self == .ocrPending || self == .clustering
            || self == .translationPending || self == .layout
    }
}

/// 一个译文覆盖块（对齐 Windows sc_types.h 的 TranslateBlock）：
/// box 为 CG 全局逻辑坐标（整数点）；origLineCount 为原段落行数（行距对齐
/// 字号搜索的输入；整图兜底块 origLineCount = 0 → 用默认字号上限）；
/// fit* 为排版缓存（键 = 绘制时框宽高，几何不变每帧复用）。
struct ScreenshotTranslateBlock {
    var box = CGRect.null
    var text = ""
    var origLineCount = 0
    // 排版缓存
    var fitKeyW = 0
    var fitKeyH = 0
    var fitFontPx = 0
    var fitLineH: CGFloat = 0
    var fitLines: [String] = []
}

// MARK: - 异步翻译任务（Job Manager，会话主线程独占读写）

/// 工作任务入参（点击时打包；全部为值类型，跨线程无共享状态）。
struct ScreenshotTranslateJobInput {
    let imageBase64: String    // data:image/png;base64,...（选区裁剪，物理像素）
    let imageW: Int
    let imageH: Int
    let selection: CGRect      // 选区（CG 全局逻辑坐标）
    let scale: CGFloat         // 物理/逻辑缩放比（baseFrame.scale）
}

/// 一个在飞的 provider 请求（requestId 驱动的生命周期载体）。
struct ScTranslateRequest {
    enum Kind {
        case ocr                                   // OCR 全图识别
        case paragraph(Int)                        // 聚类段落翻译（下标为段落序）
        case wholeText                             // 整图兜底单段翻译
    }
    let requestId: UInt64
    let kind: Kind
    let startedAt: TimeInterval    // systemUptime（耗时日志）
    let deadline: TimeInterval     // 到期后会话定时器取消该请求（毫秒精度足够）
}

/// 一个翻译任务：OCR → 聚类 → 段落有限并发翻译 → 构块。
/// 仅会话主线程读写（邮箱是唯一跨线程入口），无锁；晚到结果经 requestId 反查
/// inFlight 落空被丢弃（任务已取消/已失败/已换代时自然免疫）。
final class ScreenshotTranslateJob {
    let jobId: UInt64
    let input: ScreenshotTranslateJobInput
    let startedAt = ProcessInfo.processInfo.systemUptime

    /// 在飞请求（requestId → 请求信息；回投/超时/取消的统一登记表）。
    var inFlight: [UInt64: ScTranslateRequest] = [:]

    // ---- 翻译阶段状态（聚类完成后填充）----
    var paragraphs: [ScClusterParagraph] = []  // 聚类段落（渲染序）
    var isWholeFallback = false              // 整图兜底（无坐标 OCR）单段模式
    var wholeFallbackText = ""               // 整图兜底待译文本（OCR 回投时写入）
    var paraResults: [Int: String] = [:]     // 段落序 → 译文（不要求按序返回）
    var paraFailed = 0                        // 失败/超时/空译文段数
    var lastError = ""                        // 最近一次失败原因（全失败时报错用）
    var nextPara = 0                          // 待发射的下一个段落下标

    init(jobId: UInt64, input: ScreenshotTranslateJobInput) {
        self.jobId = jobId
        self.input = input
    }

    /// 翻译阶段是否已收束（队列发完 + 无在飞）。
    var translationDrained: Bool {
        return isWholeFallback
            ? inFlight.isEmpty
            : nextPara >= paragraphs.count && inFlight.isEmpty
    }
}

// MARK: - provider 异步调用辅助（会话主线程）

/// provider 桥是否就绪（对齐 Windows IsReady 门禁：未注册时给出可操作提示）。
func scProviderBridgeReady() -> Bool {
    guard let ready = scNativeProviderReady else { return false }
    return ready() == 1
}

/// 发射一个异步 provider 请求（立即返回；结果经邮箱回投，由会话定时器路由）。
/// - Parameters:
///   - job: 归属任务（登记 inFlight）
///   - kind: 请求种类（决定超时时长与回投路由）
///   - type: 能力类型（"ocr" / "translation"）
///   - payload: 入参对象（JSONSerialization 序列化）
///   - now: 单调时钟（deadline 登记）
/// - Returns: 已发射的请求；桥未就绪/受理被拒时返回 nil（调用方按失败收束）
func scLaunchProviderRequest(job: ScreenshotTranslateJob, kind: ScTranslateRequest.Kind,
                             type: String, payload: [String: Any],
                             now: TimeInterval) -> ScTranslateRequest? {
    guard let invoke = scNativeProviderInvokeAsync else {
        scTLog("REQUEST_START", job: job.jobId, type: type, "rejected: 翻译服务未就绪（原生桥未注册）")
        return nil
    }
    var isOcrKind = false
    if case .ocr = kind { isOcrKind = true }
    let timeoutMs: UInt32 = isOcrKind ? SC.translateOcrTimeoutMs
                                      : SC.translateLineTimeoutMs
    let requestId = scAllocateTranslateRequestId()
    let paraIdx: Int?
    if case .paragraph(let i) = kind { paraIdx = i } else { paraIdx = nil }
    scTLog("REQUEST_START", job: job.jobId, req: requestId, type: type, para: paraIdx,
           "timeout=\(timeoutMs)ms")

    guard let inputData = try? JSONSerialization.data(withJSONObject: payload),
          let inputJson = String(data: inputData, encoding: .utf8) else {
        scTLog("REQUEST_START", job: job.jobId, req: requestId, type: type,
               "rejected: 请求构造失败")
        return nil
    }
    let request = ScTranslateRequest(requestId: requestId, kind: kind,
                                     startedAt: now, deadline: now + Double(timeoutMs) / 1000.0)
    var errorPtr: UnsafeMutablePointer<CChar>? = nil
    let accepted = invoke(requestId, type, inputJson, &errorPtr)
    defer { if let p = errorPtr { free(p) } }
    guard accepted == 1 else {
        let message = errorPtr.map { String(cString: $0) } ?? "provider 调用失败"
        scTLog("REQUEST_START", job: job.jobId, req: requestId, type: type,
               "rejected: \(message)")
        return nil
    }
    job.inFlight[requestId] = request
    return request
}

/// 解析 provider 回投的 JSON 根对象（失败返回 nil）。
func scParseProviderJson(_ json: String) -> Any? {
    guard let data = json.data(using: .utf8) else { return nil }
    return try? JSONSerialization.jsonObject(with: data)
}

/// 从 translation 回投 JSON 提取译文（{"text":"译文"}；缺失/为空返回 nil）。
func scExtractTranslatedText(_ root: Any?) -> String? {
    guard let obj = root as? [String: Any], let out = obj["text"] as? String else {
        return nil
    }
    return out.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty ? nil : out
}

/// OCR 文本行（提交图像内像素坐标；对齐 Windows OcrRawBlock；text 先为原文，
/// 翻译成功后替换为译文；origLineCount 为原段落行数，由聚类成员行统计，
/// 渲染层行距对齐字号搜索的输入）。
struct ScOcrRawBlock {
    var text = ""
    var x: Double = 0
    var y: Double = 0
    var w: Double = 0
    var h: Double = 0
    var origLineCount = 0
}

// MARK: - OCR 响应解析（对齐 Windows ParseOcrBlocks）

/// 从 ocr 响应解析文本行（对齐 ParseOcrBlocks，兼容 x/y/width/height 命名与
/// blocks 为纯字符串数组的历史契约）：带坐标对象行 → outLines；字符串行 /
/// 缺坐标行 → outPlain（整图兜底）；空白文本行直接丢弃。
/// - Parameter root: OCR 响应反序列化的根对象
/// - Returns: 带坐标行 + 无坐标文本
func scParseOcrBlocks(_ root: Any?) -> (lines: [ScOcrRawBlock], plain: [String]) {
    var lines: [ScOcrRawBlock] = []
    var plain: [String] = []
    guard let obj = root as? [String: Any], let blocks = obj["blocks"] as? [Any] else {
        return (lines, plain)
    }
    for b in blocks {
        if let s = b as? String {
            if !s.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty {
                plain.append(s)
            }
            continue
        }
        guard let bo = b as? [String: Any], let t = bo["text"] as? String else { continue }
        if t.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty { continue }
        var rb = ScOcrRawBlock()
        rb.text = t
        if let v = bo["left"] as? Double { rb.x = v }
        if let v = bo["top"] as? Double { rb.y = v }
        if let v = bo["x"] as? Double { rb.x = v }
        if let v = bo["y"] as? Double { rb.y = v }
        if rb.w <= 0, let v = bo["right"] as? Double { rb.w = v - rb.x }
        if rb.h <= 0, let v = bo["bottom"] as? Double { rb.h = v - rb.y }
        if rb.w <= 0, let v = bo["width"] as? Double { rb.w = v }
        if rb.h <= 0, let v = bo["height"] as? Double { rb.h = v }
        if rb.w > 0 && rb.h > 0 {
            lines.append(rb)
        } else {
            plain.append(rb.text)   // 有文本无坐标：并兜底文本
        }
    }
    return (lines, plain)
}

// MARK: - 任务阶段处理（会话主线程；由 provider 回调驱动，纯数据处理无 UI 触碰）

/// OCR 回投的处置结论：进入段落翻译 / 整图兜底单段 / 任务失败。
enum ScOcrOutcome {
    case paragraphs(clustered: ScClusterOutcome)   // 行聚类完成 → 段落队列
    case wholeFallback                              // OCR 无坐标 → 整段翻译（文本已存 job）
    case failed(error: String)
}

/// OCR 回投处理（对齐旧 worker 流程的 1/2 前置段）：解析文本行 → 空文本守卫 →
/// 聚类（瞬态 clustering）；无坐标走整图兜底。副作用：成功时填充 job 的
/// paragraphs/isWholeFallback（段落路径）。
/// - Parameters:
///   - job: 当前任务（成功时写入聚类产物）
///   - request: 触发本处理的在飞请求（日志用）
///   - result: provider 回投
/// - Returns: 处置结论（failed 时带中文错误）
func scProcessOcrResult(job: ScreenshotTranslateJob, request: ScTranslateRequest,
                        result: ScProviderAsyncResult) -> ScOcrOutcome {
    if !result.ok {
        return .failed(error: "文字识别失败：" + result.error)
    }
    guard let root = scParseProviderJson(result.valueJson) else {
        return .failed(error: "识别结果解析失败")
    }
    let (lines, plainTexts) = scParseOcrBlocks(root)
    if lines.isEmpty && plainTexts.isEmpty {
        // 仅整段 text（无 blocks，纯文本 AI 识别契约）仍可走 2b 整图兜底；
        // 连 text 也无内容才是真正的「未识别到文字」（对齐 Windows 同款守卫）
        let wholeText = (root as? [String: Any])?["text"] as? String ?? ""
        if wholeText.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty {
            return .failed(error: "未识别到文字")
        }
    }

    if !lines.isEmpty {
        // ---- 文本行聚类成段落（逐行翻译拆散段落上下文、逐行覆盖渲染碎面板，
        // 故先聚类再以段落为单位翻译与覆盖；单段失败跳过，全失败才报错）----
        let clustered = scClusterLinesCocoa(lines)
        scTLog("CLUSTER", job: job.jobId,
               "\(lines.count) lines -> \(clustered.paragraphs.count) paragraphs "
               + "(medianH=\(clustered.medianLineH), pitch=\(clustered.medianPitch), "
               + "boundaries=\(clustered.boundaryCount))")
        if clustered.paragraphs.isEmpty {
            return .failed(error: "未识别到文字")
        }
        job.paragraphs = clustered.paragraphs
        return .paragraphs(clustered: clustered)
    }

    // ---- 整图兜底：OCR 未给坐标（如纯文本 AI 识别）时整段翻译 ----
    job.isWholeFallback = true
    var wholeText = ""
    if let obj = root as? [String: Any], let t = obj["text"] as? String,
       !t.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty {
        wholeText = t
    } else {
        wholeText = plainTexts.joined(separator: "\n")
    }
    job.wholeFallbackText = wholeText
    return .wholeFallback
}

/// 依据任务翻译产物构建译文原始块（图像物理像素坐标系；段落行数随块带下，
/// 供渲染层行距对齐的字号搜索用）：段落路径按原始段落顺序、仅含有译文者；
/// 整图兜底为整选区单块。
/// - Parameter job: 已收束的翻译任务
/// - Returns: 原始块数组 + 全失败标记（有失败时附带最近错误）
func scCollectTranslatedRawBlocks(_ job: ScreenshotTranslateJob)
        -> (blocks: [ScOcrRawBlock], allFailed: Bool, lastError: String) {
    var translated: [ScOcrRawBlock] = []
    if job.isWholeFallback {
        guard let outText = job.paraResults[0] else {
            return ([], true, job.lastError)
        }
        var fb = ScOcrRawBlock()
        fb.x = 0
        fb.y = 0
        fb.w = Double(job.input.imageW)
        fb.h = Double(job.input.imageH)
        fb.text = outText
        translated.append(fb)
        return (translated, false, "")
    }
    for (i, para) in job.paragraphs.enumerated() {
        guard let outText = job.paraResults[i] else { continue }
        var tb = ScOcrRawBlock()
        tb.x = para.x
        tb.y = para.y
        tb.w = para.w
        tb.h = para.h
        // 原段落行数（渲染层行距对齐字号搜索的输入）
        tb.origLineCount = para.lineCount
        tb.text = outText
        translated.append(tb)
    }
    return (translated, translated.isEmpty && job.paraFailed > 0, job.lastError)
}

/// 坐标换算：图像像素（物理）→ 选区绝对逻辑坐标，并夹回选区内
///（对齐旧 worker 流程第 3 步；丢弃换算后过小的块与空文本块）。
/// - Parameters:
///   - translated: 译文原始块（图像物理像素坐标系）
///   - input: 任务入参（选区/缩放比）
/// - Returns: 展示块（CG 全局逻辑坐标）
func scConvertToDisplayBlocks(_ translated: [ScOcrRawBlock],
                              _ input: ScreenshotTranslateJobInput) -> [ScreenshotTranslateBlock] {
    var blocks: [ScreenshotTranslateBlock] = []
    let ds = input.scale > 0.01 ? input.scale : 1.0
    for rb in translated {
        let x = input.selection.minX + CGFloat(rb.x / ds).rounded()
        let y = input.selection.minY + CGFloat(rb.y / ds).rounded()
        let w = CGFloat(rb.w / ds).rounded()
        let h = CGFloat(rb.h / ds).rounded()
        let clipped = CGRect(x: x, y: y, width: w, height: h).intersection(input.selection)
        if clipped.isNull || clipped.width < 2 || clipped.height < 2 { continue }

        var tb = ScreenshotTranslateBlock()
        tb.box = clipped
        tb.text = rb.text
        if tb.text.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty { continue }
        tb.origLineCount = rb.origLineCount
        blocks.append(tb)
    }
    return blocks
}

/// Swift 侧聚类段落（C ABI 产物的免拷贝视图，文本已转 String、下标已转 [Int]）。
struct ScClusterParagraph {
    var x: Double = 0
    var y: Double = 0
    var w: Double = 0
    var h: Double = 0
    var lineHeight: Double = 0
    var text: String = ""
    var lineIdx: [Int] = []
    var lineCount: Int = 0
}

struct ScClusterOutcome {
    var paragraphs: [ScClusterParagraph] = []
    var medianLineH: Double = 0
    var medianPitch: Double = 0
    var boundaryCount: Int = 0
}

/// 经 C ABI 调用 TcClusterLines（algo/translate_cluster.cpp）把 OCR 行聚成段落。
/// 输入行的 text 以 strdup 暂存保证调用期间指针有效，调用后立即释放；
/// 段落数组的 C 侧内存随 tc_cluster_free_result 整体回收。
/// - Parameter lines: OCR 带坐标文本行
/// - Returns: 段落数组 + 全局统计（失败返回空段落）
func scClusterLinesCocoa(_ lines: [ScOcrRawBlock]) -> ScClusterOutcome {
    var outcome = ScClusterOutcome()
    // strdup 暂存 C 字符串（Swift String 缓冲不保证稳定地址/结尾符）
    var cTexts: [UnsafeMutablePointer<CChar>] = []
    cTexts.reserveCapacity(lines.count)
    for rb in lines {
        rb.text.utf8CString.withUnsafeBufferPointer { buf in
            // 带 NUL 的 UTF-8 → strdup（baseAddress 对空串亦非 nil，指向 NUL）
            if let base = buf.baseAddress {
                cTexts.append(strdup(base))
            } else {
                cTexts.append(strdup(""))
            }
        }
    }
    defer { for p in cTexts { free(p) } }

    var cLines: [TcLineC] = []
    cLines.reserveCapacity(lines.count)
    for (i, rb) in lines.enumerated() {
        cLines.append(TcLineC(text: UnsafePointer(cTexts[i]), x: rb.x, y: rb.y, w: rb.w, h: rb.h))
    }
    var result = TcClusterResultC()
    guard tcClusterLinesC(&cLines, Int32(cLines.count), &result) == 1 else {
        return outcome
    }
    defer { tcClusterFreeResultC(&result) }
    outcome.medianLineH = result.medianLineH
    outcome.medianPitch = result.medianPitch
    outcome.boundaryCount = Int(result.boundaryCount)
    if let paras = result.paragraphs {
        for i in 0..<Int(result.paragraphCount) {
            let p = paras[i]
            var para = ScClusterParagraph()
            para.x = p.x
            para.y = p.y
            para.w = p.w
            para.h = p.h
            para.lineHeight = p.lineHeight
            para.text = p.text.map { String(cString: $0) } ?? ""
            para.lineCount = Int(p.lineCount)
            if let idx = p.lineIdx {
                para.lineIdx = (0..<Int(p.lineCount)).map { Int(idx[$0]) }
            }
            outcome.paragraphs.append(para)
        }
    }
    return outcome
}

// MARK: - 译文排版引擎（对齐 sctranslatedraw：CoreText 度量 + 段落区域自适应字号）

/// 某一候选字号下的整段真实排版测量（对齐 LayoutMeasure）。
struct ScLayoutMeasure {
    var lines: [String] = []
    var lineH: CGFloat = 0
    var totalH: CGFloat = 0
    var fitsHeight = false
}

/// 字体行度量（NSFont.systemFont 固定字号；CoreText 排版度量同源）。
private func scTranslateFontMetrics(_ fontPx: CGFloat) -> (lineH: CGFloat, ascent: CGFloat) {
    let attr = NSAttributedString(string: "Ag",
                                  attributes: [.font: NSFont.systemFont(ofSize: fontPx)])
    let line = CTLineCreateWithAttributedString(attr)
    var ascent: CGFloat = 0, descent: CGFloat = 0, leading: CGFloat = 0
    _ = CTLineGetTypographicBounds(line, &ascent, &descent, &leading)
    var lineH = ascent + descent + leading
    if lineH < 1 { lineH = fontPx }
    return (lineH, ascent)
}

/// 是否 CJK 字符（对齐 IsCjkForWrap，UTF-16 码元粗判）：换行算法据此允许在
/// 任意 CJK 字符后断行。
private func scIsCjkForWrap(_ c: unichar) -> Bool {
    return (c >= 0x2E80 && c <= 0x9FFF)      // 部首/中日韩部首/拼音/基本区
        || (c >= 0x3000 && c <= 0x303F)      // 中日韩标点
        || (c >= 0xF900 && c <= 0xFAFF)      // 兼容表意
        || (c >= 0xFF00 && c <= 0xFFEF)      // 全角形式
}

/// 前缀累宽：widths[i] = 前 i 个 UTF-16 单元的渲染宽度（含尾随空格，对齐
/// WrapText 的 MeasureTrailingSpaces 前缀测量；逐前缀 CTLine 测量，短段落可接受）。
private func scTranslatePrefixWidths(_ text: String, fontPx: CGFloat) -> [CGFloat] {
    let ns = text as NSString
    let attr = NSAttributedString(string: text, attributes: [.font: NSFont.systemFont(ofSize: fontPx)])
    if ns.length == 0 { return [0] }
    var widths: [CGFloat] = [0]
    widths.reserveCapacity(ns.length + 1)
    for i in 1...ns.length {
        let prefix = attr.attributedSubstring(from: NSRange(location: 0, length: i))
        let line = CTLineCreateWithAttributedString(prefix)
        widths.append(CGFloat(CTLineGetTypographicBounds(line, nil, nil, nil)))
    }
    return widths
}

/// 逐字符宽度贪心折行（对齐 WrapText）：断点 = CJK 字符后 / 空格后，拉丁长词
/// 整体不下断时回退硬断；返回的行不含换行符。maxW 为可用文本宽度（逻辑像素）。
private func scTranslateWrapText(_ text: String, fontPx: CGFloat, maxW: CGFloat,
                                 prefixWidths: [CGFloat]) -> [String] {
    let ns = text as NSString
    let n = ns.length
    if n == 0 || maxW <= 1 {
        return n > 0 ? [text] : []
    }
    var lines: [String] = []
    var lineStart = 0
    var lastBreak = 0
    var hasBreak = false
    for i in 0..<n {
        let c = ns.character(at: i)
        if c == 0x20 || scIsCjkForWrap(c) {
            lastBreak = i + 1
            hasBreak = true
        }
        let lineW = prefixWidths[i + 1] - prefixWidths[lineStart]
        if lineW <= maxW { continue }
        // 溢出：优先在行内最后一个可断点断开（可断点在行首则硬断在当前字符前）
        if hasBreak && lastBreak > lineStart {
            lines.append(ns.substring(with: NSRange(location: lineStart, length: lastBreak - lineStart)))
            lineStart = lastBreak
        } else if i > lineStart {
            lines.append(ns.substring(with: NSRange(location: lineStart, length: i - lineStart)))
            lineStart = i
        } else {
            lines.append(ns.substring(with: NSRange(location: lineStart, length: 1)))
            lineStart = i + 1
        }
        hasBreak = false
        lastBreak = lineStart
    }
    if lineStart < n {
        lines.append(ns.substring(with: NSRange(location: lineStart, length: n - lineStart)))
    }
    if lines.isEmpty { lines.append(text) }
    return lines
}

/// 测单行渲染宽度（先去行尾空白，避免状态气泡按含尾随空格的宽度虚增）。
/// 状态气泡（setTranslateStatus）的宽度测量用。
private func scTranslateMeasureLineWidth(_ line: String, fontPx: CGFloat) -> CGFloat {
    var s = Substring(line)
    while let last = s.last, last == " " || last == "\t" { s.removeLast() }
    if s.isEmpty { return 0 }
    let attr = NSAttributedString(string: String(s),
                                  attributes: [.font: NSFont.systemFont(ofSize: fontPx)])
    let cl = CTLineCreateWithAttributedString(attr)
    return CGFloat(CTLineGetTypographicBounds(cl, nil, nil, nil))
}

/// 候选字号真实排版测量（对齐 MeasureAt）：按段宽折行 → 行高/总高 → 高度
/// 可行性。fitH 为高度可行域 = 段框高（非「框高 − 双内边距」：OCR 框紧贴
/// 字形，单行段减去双内边距后连最小字号一行都放不下，字号会被钉死在下限；
/// 内边距成为垂直余量，由绘制侧垂直居中吸收）。
private func scTranslateMeasureAt(_ text: String, fontPx: Int,
                                  availW: CGFloat, fitH: CGFloat) -> ScLayoutMeasure {
    var m = ScLayoutMeasure()
    let fpx = CGFloat(fontPx)
    let (lineH, _) = scTranslateFontMetrics(fpx)
    m.lines = scTranslateWrapText(text, fontPx: fpx, maxW: availW,
                                  prefixWidths: scTranslatePrefixWidths(text, fontPx: fpx))
    m.lineH = lineH
    m.totalH = CGFloat(m.lines.count) * lineH
    m.fitsHeight = m.totalH <= fitH + 0.5
    return m
}

/// 字体排版行高比（typographic 行高 ÷ 字号，系统字体 ≈ 1.2）：把「原文行距」
/// 换算为字号的除数（行距 ÷ 行高比 = 与该行距观感一致的字号）。运行时用探针
/// 字号实测一次，不依赖具体字体的硬编码行距。
private func scTranslateLineHeightRatio() -> CGFloat {
    let probe: CGFloat = 24
    let (lineH, _) = scTranslateFontMetrics(probe)
    return lineH > 1 ? lineH / probe : 1.2
}

/// 段落字号搜索（对齐 FitParagraph；每段独立，绝不整图统一字号）：
/// 字号由段落自身几何决定，不依赖「OCR 行框 → 原字号」的换算假设（行框语义
/// 随 provider 而异，紧贴字形 / 整字高框都有，乘系数估算会在整字高框上系统性
/// 高估，且同页正文行高一致导致各段字号被钉到同一个高估值——过大且统一）：
///   1) 可行性硬约束 = 真实排版总高不超段框高（fitH）；字号对总高单调（字号
///      越大行高越大且行数不减），故先二分出最大可行字号 maxFit（译文比原文
///      长时收缩到装得下为止）；
///   2) 行距对齐上限 cap = 段框高 ÷ 原文行数 ÷ 行高比：译文的行距与原段落
///      行距一致，短译文（折行后行数少于原文）不会被放大去撑满段框；
///   3) 字号 = min(maxFit, cap)。段框高与行数每段不同，字号随段落区域自适应。
/// 整图兜底块（origLineCount = 0，无行级特征）用默认字号上限；连下限字号都
/// 装不下时退回下限，溢出由绘制侧兜底。
/// - Parameters:
///   - text: 段落译文
///   - origLineCount: 原段落行数（聚类成员行数；兜底块为 0）
///   - availW: 可用文本宽度（段框宽 − 双内边距）
///   - fitH: 高度可行域（段框高，见 scTranslateMeasureAt）
/// - Returns: 最终字号 + 该字号下的完整测量（供最终排版）
private func scTranslateFitParagraph(_ text: String, origLineCount: Int,
                                     availW: CGFloat, fitH: CGFloat)
        -> (fontPx: Int, measure: ScLayoutMeasure) {
    let fontMin = SC.translateFontMinPx
    let fontMax = SC.translateFontMaxPx
    let atMin = scTranslateMeasureAt(text, fontPx: fontMin, availW: availW, fitH: fitH)
    if !atMin.fitsHeight {
        return (fontMin, atMin)   // 下限也装不下：退回下限，溢出走兜底
    }
    var maxFit = fontMin
    var lo = fontMin, hi = fontMax
    while lo <= hi {
        let mid = (lo + hi) / 2
        if scTranslateMeasureAt(text, fontPx: mid, availW: availW, fitH: fitH).fitsHeight {
            maxFit = mid
            lo = mid + 1
        } else {
            hi = mid - 1
        }
    }
    var cap = fontMax
    if origLineCount > 0 {
        cap = Int((fitH / CGFloat(origLineCount) / scTranslateLineHeightRatio()).rounded(.down))
    } else {
        cap = Int(SC.translateFallbackFontPx)   // 兜底块无行级特征：默认字号上限
    }
    if cap < fontMin { cap = fontMin }
    if cap > fontMax { cap = fontMax }
    let fontPx = min(maxFit, cap)
    return (fontPx, scTranslateMeasureAt(text, fontPx: fontPx, availW: availW, fitH: fitH))
}

// MARK: - 译文面板与气泡绘制（覆盖层 OnPaint 与导出合成共用）

/// 在已翻转（左上原点）的上下文中绘制单行译文（Core Text 标准翻转绘制法）。
private func scTranslateDrawLine(_ ctx: CGContext, _ line: String, fontPx: CGFloat,
                                 color: NSColor, x: CGFloat, baselineY: CGFloat) {
    guard !line.isEmpty else { return }
    let attr = NSAttributedString(string: line, attributes: [
        .font: NSFont.systemFont(ofSize: fontPx),
        .foregroundColor: color,
    ])
    let cl = CTLineCreateWithAttributedString(attr)
    ctx.saveGState()
    ctx.textMatrix = .identity
    ctx.translateBy(x: x, y: baselineY)
    ctx.scaleBy(x: 1, y: -1)
    ctx.textPosition = .zero
    CTLineDraw(cl, ctx)
    ctx.restoreGState()
}

/// 译文面板配色（对齐 DrawBlockPanel / DrawTranslateStatusBubble）。
private enum ScTranslateStyle {
    static let panelFill = NSColor(srgbRed: 1, green: 1, blue: 1, alpha: 250.0 / 255.0)
    static let panelBorder = NSColor(srgbRed: 205.0 / 255.0, green: 205.0 / 255.0,
                                     blue: 205.0 / 255.0, alpha: 1)
    static let panelText = NSColor(srgbRed: 51.0 / 255.0, green: 51.0 / 255.0,
                                   blue: 51.0 / 255.0, alpha: 1)
    static let bubbleBg = NSColor(srgbRed: 41.0 / 255.0, green: 41.0 / 255.0,
                                  blue: 41.0 / 255.0, alpha: 1)
    static let bubbleText = NSColor.white
    static let bubbleTextError = NSColor(srgbRed: 1, green: 160.0 / 255.0,
                                         blue: 160.0 / 255.0, alpha: 1)
}

/// 绘制单个译文覆盖面板（对齐 DrawBlockPanel）：白底圆角面板盖住原段落区域，
/// 整段译文按搜索出的字号折行排版；字号搜索结果按「绘制时框宽高」缓存在块内
/// （选区不变时每帧复用）；连下限字号都装不下时面板才向下扩展（不超 clip）并
/// 截断末行。tb.box/clip 为绝对逻辑坐标，ox/oy 为绘制平移（覆盖层 = -视图原点，
/// 导出 = -选区左上角），交集与排版均在平移后坐标系内进行。
/// - Parameters:
///   - ctx: 已翻转（左上原点）的目标上下文
///   - tb: 译文块（排版缓存随调用写回）
///   - clip: 选区矩形（绝对逻辑坐标，约束面板向下扩展的边界）
///   - ox/oy: 绘制平移量
func scTranslateDrawBlockPanel(_ ctx: CGContext, _ tb: inout ScreenshotTranslateBlock,
                               clip: CGRect, ox: CGFloat, oy: CGFloat) {
    // 与裁剪边界（选区）求交：选区缩小后落在界外的块整体跳过，跨界块按交集绘制
    let box = tb.box.offsetBy(dx: ox, dy: oy)
    let clipOff = clip.offsetBy(dx: ox, dy: oy)
    let boxI = box.intersection(clipOff)
    if boxI.isNull { return }
    let boxW = boxI.width
    let boxH = boxI.height
    if boxW < 2 || boxH < 2 { return }
    let keyW = Int(boxW.rounded(.down))
    let keyH = Int(boxH.rounded(.down))

    let pad = SC.translatePanelPad
    var availW = boxW - pad * 2
    if availW < 8 { availW = 8 }
    // 高度可行域 = 段框高（非框高 − 双内边距，见 scTranslateMeasureAt）；
    // 垂直内边距由绘制侧垂直居中吸收
    let fitH = boxH
    let maxPanelBottom = clipOff.maxY

    // ---- 字号搜索（带缓存：键 = 绘制时框宽高，几何变化才重排）----
    if tb.fitKeyW != keyW || tb.fitKeyH != keyH || tb.fitLines.isEmpty
        || tb.fitFontPx < SC.translateFontMinPx {
        let (fontPx, m) = scTranslateFitParagraph(tb.text, origLineCount: tb.origLineCount,
                                                  availW: availW, fitH: fitH)
        tb.fitKeyW = keyW
        tb.fitKeyH = keyH
        tb.fitFontPx = fontPx
        tb.fitLineH = m.lineH
        tb.fitLines = m.lines
    }

    let fontPx = tb.fitFontPx
    let lineH = tb.fitLineH > 0.5 ? tb.fitLineH : CGFloat(fontPx)
    let lines = tb.fitLines

    // 面板高度：文字总高不超过段框高（字号搜索的可行性约束）时严格取原段落高
    // （覆盖框即原段框，保持原版面占位），文字垂直居中、垂直内边距让位；
    // 下限字号仍溢出（可行性早退的兜底路径）才向下扩展到裁剪边界
    let textH = CGFloat(lines.count) * lineH
    var panelH = boxH
    if textH > panelH {
        panelH = textH + pad * 2
        if boxI.minY + panelH > maxPanelBottom { panelH = maxPanelBottom - boxI.minY }
        if panelH < boxH { panelH = boxH }
    }
    let padY = max(0, (panelH - textH) / 2)

    // 面板：近实心白底盖住原文字 + 浅灰描边
    let panel = CGRect(x: boxI.minX, y: boxI.minY, width: boxW, height: panelH)
    let path = CGPath(roundedRect: panel, cornerWidth: SC.translatePanelRadius,
                      cornerHeight: SC.translatePanelRadius, transform: nil)
    ctx.addPath(path)
    ctx.setFillColor(ScTranslateStyle.panelFill.cgColor)
    ctx.fillPath()
    ctx.addPath(path)
    ctx.setStrokeColor(ScTranslateStyle.panelBorder.cgColor)
    ctx.setLineWidth(1)
    ctx.strokePath()

    // 逐行绘制译文：默认全显（可行字号必在框内），仅兜底扩高被裁剪边界
    // 压回时按剩余高度截断，末行截断追加「…」
    let (_, ascent) = scTranslateFontMetrics(CGFloat(fontPx))
    var visible = lines.count
    if CGFloat(visible) * lineH > panelH - padY * 2 + 0.5 {
        visible = Int(((panelH - padY * 2) / lineH).rounded(.down))
        if visible < 1 { visible = 1 }
    }
    var y = panel.minY + padY
    for i in 0..<lines.count where i < visible {
        var line = lines[i]
        if i == visible - 1 && visible < lines.count {
            line += "…"   // 还有未展示的行：当前行末尾追加省略号
        }
        scTranslateDrawLine(ctx, line, fontPx: CGFloat(fontPx), color: ScTranslateStyle.panelText,
                            x: panel.minX + pad, baselineY: y + ascent)
        y += lineH
    }
}

/// 绘制状态气泡（对齐 DrawTranslateStatusBubble）：深色圆角底 + 居中文本
/// （错误态淡红），样式与 Windows 工具栏 tooltip 同款。上下文须已翻转。
/// - Parameters:
///   - ctx: 已翻转（左上原点）的目标上下文
///   - rect: 气泡矩形（平移后坐标系）
///   - text: 气泡文本
///   - isError: 是否错误态（淡红文本）
func scTranslateDrawStatusBubble(_ ctx: CGContext, rect: CGRect, text: String, isError: Bool) {
    let path = CGPath(roundedRect: rect, cornerWidth: SC.translateBubbleRadius,
                      cornerHeight: SC.translateBubbleRadius, transform: nil)
    ctx.addPath(path)
    ctx.setFillColor(ScTranslateStyle.bubbleBg.cgColor)
    ctx.fillPath()

    let fontPx = SC.translateBubbleFontPx
    let attr = NSAttributedString(string: text, attributes: [
        .font: NSFont.systemFont(ofSize: fontPx),
        .foregroundColor: isError ? ScTranslateStyle.bubbleTextError : ScTranslateStyle.bubbleText,
    ])
    let cl = CTLineCreateWithAttributedString(attr)
    let textW = CGFloat(CTLineGetTypographicBounds(cl, nil, nil, nil))
    let (lineH, ascent) = scTranslateFontMetrics(fontPx)
    scTranslateDrawLine(ctx, text, fontPx: fontPx,
                        color: isError ? ScTranslateStyle.bubbleTextError : ScTranslateStyle.bubbleText,
                        x: rect.midX - textW / 2,
                        baselineY: rect.midY - lineH / 2 + ascent)
}

// MARK: - 会话扩展（点击入口 / 会话定时器 / 结果路由 / 绘制与导出合成）

extension ScreenshotOverlaySession {

    /// 工具栏「翻译」按钮点击入口（对齐 BeginTranslateOverlay，会话主线程调用）：
    /// 裁剪选区 → PNG base64 → 转入 OcrPending 并发射异步 OCR 请求后立即返回
    /// （不等待 Provider；Node 侧由宿主事件循环自行驱动）。重复点击忽略（进行中）/
    /// 退出翻译状态并清掉覆盖（已展示，与矩形/文字等工具一致的切换语义）。
    func beginTranslateOverlay() {
        guard !translateState.isBusy else { return }   // 进行中忽略重复点击
        if translateState == .shown {
            // 展示中再次点击：退出翻译状态（清除译文覆盖并取消按钮激活），不重跑
            translateBlocks.removeAll()
            translateState = .idle
            activeTool = nil
            invalidateAll()
            return
        }
        // 桥接就绪检查（宿主未注册 ProviderBridge 时给出可操作提示）
        guard scProviderBridgeReady() else {
            setTranslateStatus("翻译服务未就绪（未注册 ProviderBridge）", isError: true)
            invalidateAll()
            return
        }
        let sel = selection.standardized
        guard sel.width > 0, sel.height > 0 else { return }

        // ---- 物理分辨率裁剪选区（预截屏底图 → PNG base64，本线程完成）----
        guard let cropped = cropSelectionPhysical(sel),
              let dataUri = scEncodePngDataUri(cropped) else {
            setTranslateStatus("选区图像提取失败", isError: true)
            invalidateAll()
            return
        }

        // ---- 转入进行中：高亮按钮 + 进度气泡 + 发射 OCR 请求后立即返回 ----
        translateState = .ocrPending
        activeTool = .translate
        setTranslateStatus("正在识别并翻译…", isError: false)
        invalidateAll()

        let input = ScreenshotTranslateJobInput(
            imageBase64: dataUri,
            imageW: cropped.width,
            imageH: cropped.height,
            selection: CGRect(x: sel.minX.rounded(), y: sel.minY.rounded(),
                              width: sel.width.rounded(), height: sel.height.rounded()),
            scale: baseFrame.scale)
        let job = ScreenshotTranslateJob(jobId: scAllocateTranslateJobId(), input: input)
        translateJob = job
        scTLog("JOB_START", job: job.jobId,
               "image \(input.imageW)x\(input.imageH), sel \(input.selection), scale \(input.scale)")
        guard scLaunchProviderRequest(job: job, kind: .ocr, type: "ocr",
                                      payload: ["image": input.imageBase64],
                                      now: ProcessInfo.processInfo.systemUptime) != nil else {
            failTranslateJob(job, error: "翻译服务调用失败")   // 具体原因见 REQUEST_START 日志
            return
        }
    }

    /// 会话定时器逐拍任务（对齐 TickTranslateStatus + WM_SCREENSHOT_TRANSLATE_RESULT 接管）：
    /// 1) 邮箱 drain：provider 异步回投经 requestId 路由到当前任务（晚到/未知丢弃）；
    /// 2) deadline 扫描：在飞请求超时 → CancelRequest + 按失败收束该请求；
    /// 3) 错误气泡 TTL 到期收起（局部失效）。
    /// - Parameter now: 单调时钟（ProcessInfo.systemUptime）
    func tickTranslate(now: TimeInterval) {
        guard isRunning else { return }
        handleProviderResults(now: now)
        if let job = translateJob, translateState.isBusy {
            scanTranslateDeadlines(job: job, now: now)
        }
        if translateStatusShown && translateStatusError
            && now - translateStatusAt >= SC.translateErrorTtlSec {
            let rect = scInflate(translateStatusRect, 2)
            translateStatusShown = false
            translateStatusText = ""
            invalidate(rect)
        }
    }

    /// 邮箱 drain + 逐条路由（会话主线程）。requestId 不在当前任务在飞表 = 晚到/已取消/
    /// 已换代 → 丢弃（REQUEST_DISCARD）。
    private func handleProviderResults(now: TimeInterval) {
        let results = ScProviderResultMailbox.shared.drain()
        guard !results.isEmpty else { return }
        for result in results {
            guard let job = translateJob, translateState.isBusy,
                  let request = job.inFlight[result.requestId] else {
                scTLog("REQUEST_DISCARD", req: result.requestId,
                       "no active request (late/cancelled/job-replaced), ok=\(result.ok)")
                continue
            }
            job.inFlight.removeValue(forKey: result.requestId)   // 恰好一次消费
            let elapsedMs = Int((result.deliveredAt - request.startedAt) * 1000)
            let paraIdx: Int?
            if case .paragraph(let i) = request.kind { paraIdx = i } else { paraIdx = nil }
            var reqType = "translation"
            if case .ocr = request.kind { reqType = "ocr" }
            scTLog(result.ok ? "REQUEST_CALLBACK ok" : "REQUEST_CALLBACK error",
                   job: job.jobId, req: result.requestId,
                   type: reqType, para: paraIdx,
                   "elapsed=\(elapsedMs)ms")
            switch request.kind {
            case .ocr:
                handleOcrCallback(job: job, request: request, result: result, now: now)
            case .paragraph, .wholeText:
                handleTranslationCallback(job: job, request: request, result: result)
            }
        }
    }

    /// OCR 回投处理：聚类/兜底分流（clustering 瞬态）→ 进入 TranslationPending 并
    /// 首轮泵队列；失败直接收束任务。
    private func handleOcrCallback(job: ScreenshotTranslateJob, request: ScTranslateRequest,
                                   result: ScProviderAsyncResult, now: TimeInterval) {
        guard translateState == .ocrPending else {
            scTLog("REQUEST_DISCARD", job: job.jobId, req: request.requestId,
                   "ocr result after state left ocrPending")
            return   // 防御：状态已被接管（超时收束/取消后晚到）
        }
        switch scProcessOcrResult(job: job, request: request, result: result) {
        case .paragraphs:
            translateState = .clustering           // 瞬态：聚类已在 scProcessOcrResult 同步完成
            translateState = .translationPending
            pumpTranslationQueue(job: job, now: now)
        case .wholeFallback:
            translateState = .translationPending
            pumpTranslationQueue(job: job, now: now)
        case .failed(let error):
            failTranslateJob(job, error: error)
        }
    }

    /// 段落翻译回投处理：按 requestId→paragraphIndex 写回（不要求按序返回）；
    /// 单段失败/空译文跳过（对齐「单段失败跳过，全失败才报错」）。
    private func handleTranslationCallback(job: ScreenshotTranslateJob,
                                           request: ScTranslateRequest,
                                           result: ScProviderAsyncResult) {
        let index: Int
        switch request.kind {
        case .paragraph(let i): index = i
        case .wholeText: index = 0
        case .ocr: return   // 不可达（路由已分流）
        }
        if !result.ok {
            job.paraFailed += 1
            job.lastError = result.error
            scTLog("PROVIDER_ERROR", job: job.jobId, req: result.requestId,
                   type: "translation", para: index, result.error)
        } else if let text = scExtractTranslatedText(scParseProviderJson(result.valueJson)) {
            job.paraResults[index] = text
        } else {
            job.paraFailed += 1
            job.lastError = "翻译结果缺少 text 字段"
            scTLog("PROVIDER_ERROR", job: job.jobId, req: result.requestId,
                   type: "translation", para: index, job.lastError)
        }
        // 队列补发 + 收束检查（收束在 pumpTranslationQueue 内统一判定）
        pumpTranslationQueue(job: job, now: ProcessInfo.processInfo.systemUptime)
    }

    /// 翻译队列泵：从 pending 队列补发请求维持 maxConcurrency 有限并发；全部收束
    /// （发完 + 无在飞）时进入 Layout 瞬态并最终化任务。
    private func pumpTranslationQueue(job: ScreenshotTranslateJob, now: TimeInterval) {
        guard translateState == .translationPending, translateJob?.jobId == job.jobId else {
            return   // 防御：任务已取消/失败/换代
        }
        while job.inFlight.count < SC.translateMaxConcurrency {
            if job.isWholeFallback {
                // 兜底单段：仅首次进入时发射一次
                if !job.inFlight.isEmpty || job.paraResults[0] != nil || job.paraFailed > 0 {
                    break
                }
                let text = job.wholeFallbackText
                if scLaunchProviderRequest(job: job, kind: .wholeText, type: "translation",
                                           payload: ["text": text], now: now) == nil {
                    job.paraFailed += 1
                    job.lastError = "翻译服务调用失败"
                }
                break
            }
            guard job.nextPara < job.paragraphs.count else { break }
            let index = job.nextPara
            job.nextPara += 1
            if scLaunchProviderRequest(job: job, kind: .paragraph(index), type: "translation",
                                       payload: ["text": job.paragraphs[index].text],
                                       now: now) == nil {
                // 受理失败按该段失败收束（不阻断其余段落；具体原因见 REQUEST_START 日志）
                job.paraFailed += 1
                job.lastError = "翻译服务调用失败"
            }
        }
        if job.translationDrained {
            finishTranslationJob(job)
        }
    }

    /// deadline 扫描：在飞请求到期 → CancelRequest（桥移除登记，晚到回投被丢弃）
    /// + 按请求种类收束失败路径（OCR 超时 → 任务失败；段落超时 → 该段失败并补发）。
    private func scanTranslateDeadlines(job: ScreenshotTranslateJob, now: TimeInterval) {
        var timedOut: [ScTranslateRequest] = []
        for (_, request) in job.inFlight where now >= request.deadline {
            timedOut.append(request)
        }
        guard !timedOut.isEmpty else { return }
        var jobFailed = false
        for request in timedOut {
            job.inFlight.removeValue(forKey: request.requestId)
            scNativeProviderCancel?(request.requestId)
            let elapsedMs = Int((now - request.startedAt) * 1000)
            let paraIdx: Int?
            if case .paragraph(let i) = request.kind { paraIdx = i } else { paraIdx = nil }
            var reqType = "translation"
            if case .ocr = request.kind { reqType = "ocr" }
            scTLog("REQUEST_TIMEOUT", job: job.jobId, req: request.requestId,
                   type: reqType, para: paraIdx,
                   "elapsed=\(elapsedMs)ms, cancelled (late result will be discarded)")
            switch request.kind {
            case .ocr:
                failTranslateJob(job, error: "文字识别超时")
                jobFailed = true
            case .paragraph, .wholeText:
                job.paraFailed += 1
                job.lastError = "翻译超时"
            }
        }
        guard !jobFailed else { return }
        // 段落超时后补发 + 收束检查
        pumpTranslationQueue(job: job, now: now)
    }

    /// 任务最终化（Layout 瞬态）：构块 + 坐标换算 → 成功进入 Shown；
    /// 全失败/空块 → 失败收束。
    private func finishTranslationJob(_ job: ScreenshotTranslateJob) {
        guard translateJob?.jobId == job.jobId else { return }   // 防御：已换代
        translateState = .layout
        let (raw, allFailed, lastError) = scCollectTranslatedRawBlocks(job)
        let blocks = scConvertToDisplayBlocks(raw, job.input)
        let elapsedMs = Int((ProcessInfo.processInfo.systemUptime - job.startedAt) * 1000)
        if allFailed {
            failTranslateJob(job, error: "翻译失败：" + lastError)
            return
        }
        if blocks.isEmpty {
            failTranslateJob(job, error: "未识别到文字")
            return
        }
        translateBlocks = blocks
        translateState = .shown
        translateJob = nil
        // 收起进度气泡（保留 activeTool 高亮：覆盖展示中按钮保持激活观感）
        translateStatusShown = false
        translateStatusText = ""
        scTLog("JOB_COMPLETE", job: job.jobId,
               "blocks=\(blocks.count) failedParas=\(job.paraFailed) elapsed=\(elapsedMs)ms")
        invalidateAll()
    }

    /// 任务失败收束：取消全部在飞请求（晚到回投经 requestId 反查落空丢弃）→ 状态
    /// 回 Idle + 错误气泡（TTL 自动消失）。
    private func failTranslateJob(_ job: ScreenshotTranslateJob, error: String) {
        cancelJobRequests(job)
        translateJob = nil
        translateState = .idle
        activeTool = nil
        scTLog("JOB_FAILED", job: job.jobId, error)
        setTranslateStatus(error, isError: true)
        invalidateAll()
    }

    /// 取消一个任务的全部在飞请求（桥侧登记移除；不触发回调，晚到结果被桥丢弃）。
    private func cancelJobRequests(_ job: ScreenshotTranslateJob) {
        for (_, request) in job.inFlight {
            scNativeProviderCancel?(request.requestId)
        }
        job.inFlight.removeAll()
    }

    /// 翻译任务取消（对齐 TeardownTranslateJobs）：会话收尾 / 退出翻译态时调用。
    /// 之后到达的回投经 requestId 反查（translateJob 已置 nil / 状态非进行中）落空，
    /// 在 handleProviderResults 中按晚到结果丢弃，绝不重新改变 UI 状态。
    func cancelTranslateJob() {
        guard let job = translateJob else { return }
        let inFlightCount = job.inFlight.count
        for (_, request) in job.inFlight {
            scNativeProviderCancel?(request.requestId)
        }
        job.inFlight.removeAll()
        translateJob = nil
        translateState = .idle
        scTLog("JOB_CANCEL", job: job.jobId, "\(inFlightCount) in-flight cancelled")
        // 邮箱残留（回投先于取消入队）随下一次 drain 丢弃；无会话内消费者时靠上限防膨胀
    }

    /// 设置状态气泡文本并计算气泡矩形（对齐 SetTranslateStatus）：水平居中于选区、
    /// 置于选区上沿外侧（放不下转选区内顶部），钳制虚拟屏内。调用方负责触发重绘。
    private func setTranslateStatus(_ text: String, isError: Bool) {
        translateStatusText = text
        translateStatusError = isError
        translateStatusShown = !text.isEmpty
        translateStatusRect = .null
        translateStatusAt = ProcessInfo.processInfo.systemUptime
        guard translateStatusShown else { return }

        let fontPx = SC.translateBubbleFontPx
        let textW = scTranslateMeasureLineWidth(text, fontPx: fontPx)
        let (lineH, _) = scTranslateFontMetrics(fontPx)
        let w = ceil(textW) + SC.translateBubblePadX * 2 + 2
        let h = ceil(lineH) + SC.translateBubblePadY * 2 + 2
        let sel = selection
        var x = sel.minX + (sel.width - w) / 2
        if x < 4 { x = 4 }
        if x + w > virtualBounds.maxX - 4 { x = virtualBounds.maxX - 4 - w }
        var y = sel.minY - h - 6
        if y < 4 { y = sel.minY + 6 }   // 上方放不下：转选区内顶部
        translateStatusRect = CGRect(x: x, y: y, width: w, height: h)
    }

    /// 译文覆盖层绘制入口（paintConfirmedOverlay 在标注/文字编辑层之后、确认边框
    /// 之前调用，对齐 Windows OnPaint 的 DrawTranslateOverlay 层序；确认/绘制/文字
    /// 编辑各态均保持展示）。
    /// - Parameters:
    ///   - ctx: 视图 CG 上下文（已翻转，左上原点）
    ///   - view: 当前绘制的覆盖层视图
    func paintTranslateOverlay(ctx: CGContext, view: OverlayScreenshotView) {
        let ox = -view.cgOrigin.x
        let oy = -view.cgOrigin.y
        if translateState == .shown && !translateBlocks.isEmpty {
            for i in translateBlocks.indices {
                scTranslateDrawBlockPanel(ctx, &translateBlocks[i], clip: selection, ox: ox, oy: oy)
            }
        }
        if translateStatusShown && !translateStatusText.isEmpty && !translateStatusRect.isNull {
            scTranslateDrawStatusBubble(ctx, rect: translateStatusRect.offsetBy(dx: ox, dy: oy),
                                        text: translateStatusText, isError: translateStatusError)
        }
    }

    /// 导出合成入口（对齐 CompositeTranslateBlocks，确认提取/保存落盘共用）：把译文
    /// 覆盖块合成进最终图像。调用方已完成上下文翻转（左上原点）与 scaleBy——
    /// 坐标系 = 选区左上角原点、逻辑点；排版缓存为一次性拷贝，不回写会话状态。
    /// - Parameters:
    ///   - ctx: 输出位图上下文（已翻转、已缩放）
    ///   - sel: 选区（CG 全局逻辑坐标）
    func compositeTranslateBlocks(_ ctx: CGContext, sel: CGRect) {
        guard translateState == .shown, !translateBlocks.isEmpty else { return }
        var blocks = translateBlocks
        for i in blocks.indices {
            scTranslateDrawBlockPanel(ctx, &blocks[i], clip: sel, ox: -sel.minX, oy: -sel.minY)
        }
    }
}

// MARK: - PNG base64 编码辅助

/// CGImage → PNG data URI（对齐 Windows BitmapToBase64Png 的编码角色；
/// ImageIO 单次编码，base64 拼接 data URL 前缀）。
/// - Parameter image: 物理像素图（选区裁剪产物）
/// - Returns: "data:image/png;base64,..."；编码失败返回 nil
func scEncodePngDataUri(_ image: CGImage) -> String? {
    guard let data = CFDataCreateMutable(nil, 0),
          let dest = CGImageDestinationCreateWithData(data, "public.png" as CFString, 1, nil)
    else { return nil }
    CGImageDestinationAddImage(dest, image, nil)
    guard CGImageDestinationFinalize(dest) else { return nil }
    return "data:image/png;base64," + (data as Data).base64EncodedString()
}
