import Foundation
import AppKit
import CoreGraphics

// MARK: - 截图模块（macOS）
//
// 本文件是 macOS 截图子系统的平台层落点（会话/覆盖层/绘制拆分在同目录其余文件）：
// - CaptureBackend 抽象 + CGWindowListCreateImage 首期实现（接口按 ScreenCaptureKit 形状设计）
// - 屏幕录制权限预检/请求
// - prime() 预抓帧（2 秒 TTL、互斥锁保护、锁内所有权转移，对齐 Windows capture_windows.cpp）
// - start() 闭环：权限 →（预抓帧或现场重抓）整屏底图 → 多屏覆盖层选区会话
//   （ScreenshotOverlayMac.swift：手动泵主循环 + 选区状态机）→ 确认时按选区裁剪底图
//   → PNG 编码 → NSPasteboard 写入 → 经 C++ screenshotTsfn 回调契约结果
// - abortLongCapture() 中止标记：锁内置标志 set/consume/reset（长截图采样循环
//   在检查点消费并按失败结果收束会话，对齐 Windows LongCaptureAbort 语义）
//
// 坐标系约定：会话内统一 CG 全局坐标（左上原点、逻辑点），
// 回调的 x/y/x2/y2/width/height 均为逻辑尺寸；base64 图像为物理像素（Retina 2x，
// PNG 携带 DPI 元数据供看图应用按逻辑尺寸显示，见 ScreenshotOutputMac.swift）。

// C 风格回调：截图会话结果（JSON 字符串；生命周期仅限本次调用，C++ 层负责复制）
public typealias ScreenshotResultCallback = @convention(c) (UnsafePointer<CChar>?) -> Void

// 预抓帧 TTL（对齐 Windows internal.h 的 SC_PRIMED_FRAME_TTL = 2 秒）
private let SC_PRIMED_FRAME_TTL_NANOS: UInt64 = 2_000_000_000

// MARK: - 基础类型

/// 一帧屏幕捕获结果：物理像素图像 + 还原逻辑坐标所需的元数据。
/// 坐标为 CG 全局坐标（左上原点、逻辑点），与 Windows 回调的虚拟屏绝对坐标语义对齐。
/// （跨文件共享：覆盖层会话按此裁剪底图/放大镜采样。）
struct CapturedFrame {
    let image: CGImage           // 物理像素位图（Retina 下为 2x）
    let origin: CGPoint          // 虚拟屏并集左上角（CG 全局逻辑坐标）
    let logicalSize: CGSize      // 逻辑尺寸（回调契约的 width/height 即此值）
    let scale: CGFloat           // 物理/逻辑缩放比（Retina = 2.0；混合 DPI 下为并集整体比例）
    let capturedAtNanos: UInt64  // 单调时钟抓取时刻（DispatchTime.uptimeNanoseconds），TTL 判定用
}

/// 捕获内容过滤器（对齐 ScreenCaptureKit 的 SCContentFilter 形状）。
/// rect 为 CG 全局逻辑坐标目标区域；excludingWindowNumbers 为需排除的自身窗口号
/// （蒙版/面板等覆盖层窗口，语义对齐 SCK 的 excludingWindows）。
private struct CaptureContentFilter {
    let rect: CGRect
    let excludingWindowNumbers: [CGWindowID]
}

/// 捕获配置（对齐 ScreenCaptureKit 的 SCStreamConfiguration 形状，仅含分辨率与光标两项）。
private struct CaptureConfiguration {
    /// true = Retina 下输出物理像素（等价 CGWindowListCreateImage 的 .bestResolution）
    var bestResolution: Bool = true
    /// 是否包含鼠标光标。CG 后端天然不捕获光标（截图工具期望行为）；
    /// 升级 ScreenCaptureKit 时需显式传 showsCursor=false 保持行为一致。
    var showsCursor: Bool = false
}

// MARK: - 捕获后端

/// 捕获后端抽象：接口按 ScreenCaptureKit 的「内容过滤 + 配置 → 图像」形状设计，
/// 首期实现为 CGWindowListCreateImage（10.15 可用、已被取色器验证、支持
/// .optionOnScreenBelowWindow 排除自身窗口）。CGWindowListCreateImage 在 macOS 14+
/// 标记 deprecated 但仍可用（deployment target 10.15）；SCK（12.3+）升级时仅替换
/// 后端实现，会话层不变。
private protocol CaptureBackend {
    /// 截取整屏底图：所有 NSScreen 的并集（等价 Windows 虚拟屏），Retina 下输出物理像素。
    /// - Returns: 捕获帧；失败返回 nil（对齐 Windows 抓帧失败绝不把黑图报成功的语义，矩阵 #48）
    func captureVirtualScreenBase() -> CapturedFrame?

    /// 按内容过滤器抓取一帧（覆盖层底图 / 长截图实况抓帧共用入口）。
    /// - Parameters:
    ///   - filter: 目标区域与需排除的窗口
    ///   - configuration: 分辨率/光标配置
    /// - Returns: 物理像素 CGImage；失败返回 nil
    func captureImage(filter: CaptureContentFilter, configuration: CaptureConfiguration) -> CGImage?
}

/// CGWindowListCreateImage 后端实现。
private struct CGWindowListCaptureBackend: CaptureBackend {
    func captureVirtualScreenBase() -> CapturedFrame? {
        guard let bounds = ScreenshotGeometry.virtualScreenBounds() else { return nil }
        guard let image = captureImage(
            filter: CaptureContentFilter(rect: bounds, excludingWindowNumbers: []),
            configuration: CaptureConfiguration()
        ) else { return nil }

        let scale = bounds.width > 0 ? CGFloat(image.width) / bounds.width : 1.0
        return CapturedFrame(
            image: image,
            origin: bounds.origin,
            logicalSize: bounds.size,
            scale: scale,
            capturedAtNanos: DispatchTime.now().uptimeNanoseconds
        )
    }

    func captureImage(filter: CaptureContentFilter, configuration: CaptureConfiguration) -> CGImage? {
        // 排除窗口：CGWindowListCreateImage 只支持「取某窗口之下」的单窗口排除——
        // 蒙版窗口是全屏最上层窗口，"其下"即用户内容（取色器 capturePixelsAroundCursor
        // 已验证该手法）；多窗口排除需 ScreenCaptureKit 的 excludingWindows。
        let option: CGWindowListOption
        let windowID: CGWindowID
        if let exclude = filter.excludingWindowNumbers.first {
            option = .optionOnScreenBelowWindow
            windowID = exclude
        } else {
            option = .optionOnScreenOnly
            windowID = kCGNullWindowID
        }

        let imageOption: CGWindowImageOption = configuration.bestResolution
            ? .bestResolution
            : .nominalResolution
        return CGWindowListCreateImage(filter.rect, option, windowID, imageOption)
    }
}

// MARK: - 坐标换算

/// 坐标换算工具（会话内统一 CG 全局坐标、左上原点、逻辑点）。
/// （跨文件共享：覆盖层窗口定位、鼠标事件换算、蒙版绘制均依赖。）
enum ScreenshotGeometry {
    /// NS 全局坐标的 Y 轴翻转基准：主屏（frame.origin == .zero 的屏幕）的逻辑高度。
    static func primaryScreenHeight() -> CGFloat {
        let screens = NSScreen.screens
        return (screens.first { $0.frame.origin == .zero }?.frame.height)
            ?? (screens.first?.frame.height ?? 0)
    }

    /// 计算所有 NSScreen 的并集（等价 Windows 虚拟屏），返回 CG 全局逻辑坐标矩形。
    /// NSScreen.frame 为 NS 坐标（主屏左下原点、Y 向上），需翻转为 CG 左上原点。
    /// - Returns: 虚拟屏并集矩形；无屏幕或退化矩形时返回 nil
    static func virtualScreenBounds() -> CGRect? {
        let screens = NSScreen.screens
        guard !screens.isEmpty else { return nil }

        var minX = CGFloat.greatestFiniteMagnitude
        var minY = CGFloat.greatestFiniteMagnitude
        var maxX = -CGFloat.greatestFiniteMagnitude
        var maxY = -CGFloat.greatestFiniteMagnitude
        for screen in screens {
            let cg = cgFrame(of: screen)
            minX = min(minX, cg.minX)
            minY = min(minY, cg.minY)
            maxX = max(maxX, cg.maxX)
            maxY = max(maxY, cg.maxY)
        }

        let bounds = CGRect(x: minX, y: minY, width: maxX - minX, height: maxY - minY)
        guard bounds.width > 0, bounds.height > 0 else { return nil }
        return bounds
    }

    /// 单个 NSScreen 的 CG 全局逻辑坐标帧（左上原点）：供每屏一个覆盖层窗口定位与
    /// 鼠标事件/绘制坐标换算（多屏覆盖层坐标系）。
    static func cgFrame(of screen: NSScreen) -> CGRect {
        let frame = screen.frame
        let cgTop = primaryScreenHeight() - frame.maxY
        return CGRect(x: frame.minX, y: cgTop, width: frame.width, height: frame.height)
    }

    /// NS 全局坐标点 → CG 全局坐标点（Y 翻转；取色器同款换算）。
    static func cgPoint(fromNS nsPoint: NSPoint) -> CGPoint {
        return CGPoint(x: nsPoint.x, y: primaryScreenHeight() - nsPoint.y)
    }
}

// MARK: - 预抓帧缓存

/// 预抓帧缓存：对齐 Windows capture_windows.cpp 的 g_primedScreenshotFrame 语义——
/// 2 秒 TTL、互斥锁保护、锁内所有权转移（写方写入、读方消费后原帧即失效）。
private final class PrimedFrameStore {
    private let lock = NSLock()
    private var frame: CapturedFrame?

    /// 立即抓取整屏底图写入缓存。
    /// 抓帧在锁外执行（避免持锁做耗时系统调用），锁内仅做所有权转移。
    /// - Returns: 抓帧成功返回 true
    func refresh(backend: CaptureBackend) -> Bool {
        guard let newFrame = backend.captureVirtualScreenBase() else { return false }
        lock.lock()
        frame = newFrame
        lock.unlock()
        return true
    }

    /// 消费缓存帧：未过期时移出并返回（锁内所有权转移，消费后缓存失效）；
    /// 未命中或超过 TTL 时清空缓存并返回 nil，调用方（start 会话）应现场重抓
    /// （对齐 Windows ConsumePrimedScreenshotFrame 的过期释放语义）。
    func consume() -> CapturedFrame? {
        lock.lock()
        defer { lock.unlock() }
        guard let cached = frame else { return nil }
        frame = nil
        let elapsed = DispatchTime.now().uptimeNanoseconds &- cached.capturedAtNanos
        guard elapsed <= SC_PRIMED_FRAME_TTL_NANOS else { return nil }
        return cached
    }

    /// 丢弃缓存帧（预留给会话异常清理路径；普通路径由 consume 转移所有权）。
    func discard() {
        lock.lock()
        frame = nil
        lock.unlock()
    }
}

// MARK: - 会话状态

// 会话重入保护（C++ 层有对应 g_screenshotInProgress 拦截 JS 侧重复 start，此处兜底直接 FFI 调用）。
// （跨文件共享：覆盖层会话 finish 时复位。）
let screenshotStateLock = NSLock()
var screenshotSessionActive = false

// 预抓帧缓存与捕获后端（进程级单例；覆盖层会话消费底图）
private let primedFrameStore = PrimedFrameStore()
private let screenshotBackend: CaptureBackend = CGWindowListCaptureBackend()

// 长截图会话中止状态（锁内置 abortFlag，与清理互斥防 use-after-free）。
// 对齐 Windows LongCaptureAbort（lc_session_windows.cpp）：JS 线程置位（request），长截图采样
// 循环在检查点消费（consume，读后自动复位），会话创建时重置防跨会话粘滞（reset）。
let longCaptureAbortLock = NSLock()
private var longCaptureAbortRequested = false

/// 置位长截图中止标志（abortLongCapture 导出的实现体；可在任意线程调用）。
func requestLongCaptureAbort() {
    longCaptureAbortLock.lock()
    longCaptureAbortRequested = true
    longCaptureAbortLock.unlock()
}

/// 消费长截图中止标志（长截图采样循环检查点调用；读后自动复位，保证一次中止只收束一次）。
/// - Returns: 自上次消费以来是否被请求过中止
func consumeLongCaptureAbort() -> Bool {
    longCaptureAbortLock.lock()
    defer { longCaptureAbortLock.unlock() }
    if longCaptureAbortRequested {
        longCaptureAbortRequested = false
        return true
    }
    return false
}

/// 重置长截图中止标志（长截图会话创建时调用；防上一次会话遗留的置位误杀新会话）。
func resetLongCaptureAbort() {
    longCaptureAbortLock.lock()
    longCaptureAbortRequested = false
    longCaptureAbortLock.unlock()
}

// MARK: - 会话选项

/// 截图会话选项（JS options 的解析产物；C++ 层已按 Windows 语义钳制，此处二次校验兜底）。
/// （跨文件共享：覆盖层会话读取 autoConfirm。）
struct ScreenshotSessionOptions {
    /// 选区确定后直接出图，跳过编辑态（覆盖层生效：松手/单击吸附 → 直接确认输出）
    var autoConfirm = true
    /// 长截图滚轮停稳防抖间隔（50~2000ms，默认 250；长截图生效）
    var longCaptureIntervalMs = 250

    /// 从 C++ 层传入的 options JSON 解析；钳制范围与 Windows session_windows.cpp 一致，
    /// 非法/越界值回落默认值（不做静默贴边钳制，对齐 Windows 行为）。
    static func parse(from jsonString: String?) -> ScreenshotSessionOptions {
        var options = ScreenshotSessionOptions()
        guard let jsonString = jsonString,
              let data = jsonString.data(using: .utf8),
              let object = try? JSONSerialization.jsonObject(with: data) as? [String: Any] else {
            return options
        }
        if let autoConfirm = object["autoConfirm"] as? Bool {
            options.autoConfirm = autoConfirm
        }
        if let longCapture = object["longCapture"] as? [String: Any] {
            if let interval = longCapture["interval"] as? Int, (50...2000).contains(interval) {
                options.longCaptureIntervalMs = interval
            }
        }
        return options
    }
}

// MARK: - 会话入口（覆盖层与选区）

/// 启动覆盖层选区会话的统一入口：权限预检 → NSApplication 初始化 → 底图获取（预抓帧优先）
/// → 窗口吸附枚举 → 多屏覆盖层 + 手动泵主循环（ScreenshotOverlayMac.swift）。
/// 任一前置失败都恰好回调一次 {success:false, error:...}（FailFast 语义）；
/// 会话正常结束（确认/取消）由覆盖层会话负责回调并复位重入标志。
/// - Parameters:
///   - options: 已解析的会话选项（autoConfirm / longCapture 参数均生效）
///   - callback: C++ 层注册的结果回调（JSON 字符串参数）
func runOverlayCaptureSession(options: ScreenshotSessionOptions, callback: ScreenshotResultCallback) {
    // 单一出口：结果 JSON 回调一次并复位会话标志（重入保护随之解除）
    func finish(_ payload: String) {
        payload.withCString { cStr in
            callback(cStr)
        }
        screenshotStateLock.lock()
        screenshotSessionActive = false
        screenshotStateLock.unlock()
    }

    func failurePayload(_ error: String) -> String {
        return "{\"success\":false,\"error\":\"\(error)\"}"
    }

    // 0) AppKit 主线程硬要求：NSWindow 创建与手动泵事件循环只能在主线程执行
    //    （线程模型：start() 的 N-API 调用线程即 AppKit 主线程）。
    guard Thread.isMainThread else {
        finish(failurePayload("screenshot session must run on the main thread"))
        return
    }

    // 1) 屏幕录制权限：预检未过先请求（弹系统授权框），仍失败按契约回调。
    //    注意 CGWindowListCreateImage 在未授权时并不报错，而是返回缺窗口内容的"伪底图"，
    //    因此必须在抓帧前硬性预检，绝不把无窗口内容的图当成功输出。
    if !CGPreflightScreenCaptureAccess() {
        _ = CGRequestScreenCaptureAccess()
        if !CGPreflightScreenCaptureAccess() {
            finish(failurePayload("screen recording permission required"))
            return
        }
    }

    // 2) NSApplication 初始化（取色器模式：accessory policy；Node 主线程不跑 NSRunLoop，
    //    覆盖层窗口由会话内的手动泵循环驱动）
    let app = NSApplication.shared
    app.setActivationPolicy(.accessory)
    app.finishLaunching()

    // 3) 底图获取：优先消费 prime() 预抓帧（未过期），过期/未命中时现场重抓
    //    （对齐 Windows AcquireScreenshotBase：预抓帧命中即用，否则 CaptureVirtualScreen 兜底）
    guard let baseFrame = primedFrameStore.consume() ?? screenshotBackend.captureVirtualScreenBase() else {
        finish(failurePayload("failed to capture screen"))
        return
    }

    guard let virtualBounds = ScreenshotGeometry.virtualScreenBounds() else {
        finish(failurePayload("failed to query screen layout"))
        return
    }

    // 4) 覆盖层选区会话（会话内手动泵直至确认/取消；结束前回调恰好一次）
    let session = ScreenshotOverlaySession(
        options: options,
        callback: callback,
        baseFrame: baseFrame,
        virtualBounds: virtualBounds
    )
    guard session.start() else {
        // 会话初始化失败：start 内部已 FailFast 回调并复位标志
        return
    }
    session.runEventPump()
}

// MARK: - C 导出（binding_mac.cpp 经 dlsym 调用）

/// 供 JS 主动触发的整屏预抓帧（对齐 Windows PrimeScreenshotFrameNow / primeScreenshotFrame 导出）。
/// 抓帧在锁外执行、锁内所有权转移。未授权屏幕录制时直接失败且不弹授权框——
/// 授权框交互只在 start() 会话内发生，避免把缺窗口内容的"伪底图"写进缓存。
/// - Returns: 1 抓帧成功；0 失败（无权限 / 抓帧失败）
@_cdecl("primeScreenshotFrame")
public func primeScreenshotFrame() -> Int32 {
    if !CGPreflightScreenCaptureAccess() {
        return 0
    }
    return primedFrameStore.refresh(backend: screenshotBackend) ? 1 : 0
}

/// 启动区域截图会话（覆盖层与选区；对齐 Windows startRegionCaptureWithPrimedFrame 导出）。
/// 与 Windows 会话线程模型的差异：macOS 的 N-API 调用线程即 AppKit 主线程，覆盖层
/// 会话在本调用内以手动泵循环运行直至确认/取消，故本函数阻塞至会话结束。
/// - Parameters:
///   - optionsJson: C++ 层解析并钳制后的 options JSON（autoConfirm / longCapture 参数）
///   - callback: 结果回调；会话出口恰好回调一次（成功/失败均必达——FailFast 语义）
/// - Returns: 1 = 会话已受理（结果异步回调）；0 = 拒绝（重入/参数非法，不会回调）
@_cdecl("startRegionCaptureWithPrimedFrame")
public func startRegionCaptureWithPrimedFrame(
    _ optionsJson: UnsafePointer<CChar>?,
    _ callback: ScreenshotResultCallback?
) -> Int32 {
    guard let callback = callback else { return 0 }

    // 重入保护：会话进行中拒绝再次进入（C++ 层已对 JS 抛错，此处兜底直接 FFI 调用）
    screenshotStateLock.lock()
    if screenshotSessionActive {
        screenshotStateLock.unlock()
        return 0
    }
    screenshotSessionActive = true
    screenshotStateLock.unlock()

    let options = ScreenshotSessionOptions.parse(from: optionsJson.map { String(cString: $0) })
    runOverlayCaptureSession(options: options, callback: callback)
    return 1
}

/// 请求中止进行中的长截图滚动捕获（对齐 Windows LongCaptureAbort：锁内置 abortFlag，
/// 与清理互斥防 use-after-free；长截图采样循环在下一检查点（泵循环 tick）消费本标志，
/// 销毁长截图浮层并按失败结果收束整个会话回调 JS {success:false}）。
/// 无长截图会话时置位后即被下次会话创建的 reset 清除，等价 Windows 的空指针分支。
@_cdecl("abortLongCapture")
public func abortLongCapture() {
    requestLongCaptureAbort()
}
