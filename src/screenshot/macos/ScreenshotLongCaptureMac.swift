import Foundation
import AppKit
import CoreGraphics
import ApplicationServices

// MARK: - 长截图会话（macOS；Windows 基准 lc_session_windows.cpp）
//
// 本文件承载长截图会话主体（小地图见 ScreenshotLCPanelMac.swift、工具栏/裁剪 popover
// 见 ScreenshotLCToolbarMac.swift）：
// - 进入/退出：编辑态工具栏「长截图」按钮 → 隐藏覆盖层 → 每屏灰蒙版（点击穿透 + 选区描边 +
//   采样区全透明内缩 2px）→ 小地图 + 长截图工具栏 → 滚轮观察 tap（对齐 lc_session_windows.cpp
//   BeginLongCapture / lc_panel_ui_windows.cpp EnterLongCaptureMask）
// - 采样主循环：采样三条件（停稳防抖 / 滚动中主动节拍 / Weak 待复核）+ 重试梯（稳定性闸门 /
//   瞬态快重采样 / Normal 5 档 / Weak 6 档），参数全部经 lc_get_algo_consts 取自算法层，
//   绝不放宽匹配条件，失败帧不拼接不污染不自动完成（对齐 lc_session_windows.cpp RunLongCapture）
// - 实况抓帧：CGWindowListCreateImage(.optionOnScreenBelowWindow, 蒙版窗口号)（取色器已验证的排除自身窗口手法）→ 选区内缩 2px 区域 → BGRA 物理像素缓冲 →
//   lc_init_baseline 首帧 / lc_try_stitch 后续帧（对齐 lc_frame_io_windows.cpp 的帧布局与转置）
// - autoScroll：31ms 定时注入亚档位滚轮增量（对齐 lc_toolbar_ui_windows.cpp LongCaptureAutoScrollTick）
// - 终止全集：完成 / 取消 / ESC / abortLongCapture（对齐 lc_session_windows.cpp 主循环各 break 分支；
//   拼接无帧数/像素上限，可持续合并至用户主动结束）
//
// 线程模型：长截图会话由覆盖层会话的手动泵（pumpTick → lcTick）在主线程驱动，算法层调用
// 全部串行（LCAlgorithmSession 线程约定）；CGEventTap 回调线程只写滚轮缓冲（锁内），
// 泵循环逐拍消费（沿用覆盖层会话的事件模式）。
//
// 与 Windows 的既知语义差异（均为有意映射，其余逐条对齐）：
// - Windows 在独立捕获线程用阻塞式 LongCaptureWaitMessages 等待重试间隔；macOS 无第二
//   消息循环，重试梯以「下次尝试时刻」在泵循环 tick 中推进，UI 同期保持响应。
// - autoScroll 注入语义差异见 tickAutoScroll 注释。

// MARK: - 常量（Windows 出处集中标注）

/// 自动滚动注入节拍 ms（lc_toolbar_ui_windows.cpp LC_AUTOSCROLL_TICK_MS = 31，落在系统定时器
/// ~15.6ms 粒度的两格上）。
private let LC_AUTOSCROLL_TICK_MS: UInt64 = 31

/// 自动滚动单次注入滚轮增量（对齐 lc_toolbar_ui_windows.cpp LC_AUTOSCROLL_STEP_DELTA = 10 的
/// 亚档位语义换算：Windows 单位为 WHEEL_DELTA=120 的 1/12 档/拍，按典型页面 ~100px/档
/// 折算 ≈ 8px/拍（≈258px/s 连续匀速滚动）。macOS 用 .pixel 单位注入——.line 单位整数
/// 增量最小 1 行 ≈ 1 档，无法表达 1/12 档的亚档位增量；pixel 增量被现代应用按精确
/// 滚动累积，视觉上即为连续滚动，与 Windows 注释语义一致）。
private let LC_AUTOSCROLL_STEP_DELTA: Int32 = 8

/// 自动滚动连续硬失败采样轮数上限（lc_toolbar_ui_windows.cpp LC_AUTOSCROLL_STOP_FAILS = 3；
/// 自动停止防丢内容）。
private let LC_AUTOSCROLL_STOP_FAILS = 3

/// Windows 滚轮一格的 delta 单位（winuser.h WHEEL_DELTA = 120）：macOS 滚轮事件以「行」
/// 为单位（1 行 ≈ 1 notch），软先验累计时归一到 WHEEL_DELTA 语义（见 drainWheel）。
private let LC_WHEEL_DELTA_UNITS: Int32 = 120

/// 蒙版样式（lc_panel_ui_windows.cpp LONG_MASK_GRAY = 44 / LONG_MASK_ALPHA = 0xA0，预乘 ARGB）。
let LC_MASK_GRAY_COMPONENT: CGFloat = 44
let LC_MASK_ALPHA: CGFloat = 0xA0

/// 小地图面板布局（lc_panel_ui_windows.cpp LC_PANEL_W = 232 / LC_PANEL_PAD = 8 /
/// LC_PANEL_MIN_H = 48；定义在面板文件共用，此处给工具栏避让计算引用）。
let LC_PANEL_WIDTH: CGFloat = 232
let LC_PANEL_PAD_PX: CGFloat = 8

/// 面板生长上限：屏高（虚拟屏高）的 45%（lc_panel_ui_windows.cpp LongCapturePanelUpdate capH）。
let LC_PANEL_MAX_HEIGHT_RATIO: CGFloat = 0.45

/// 当前单调时钟毫秒（采样/注入/UI 各节拍的计时基准；等价 Windows GetTickCount）。
func lcNowMs() -> UInt64 {
    return DispatchTime.now().uptimeNanoseconds / 1_000_000
}

/// BGRA 缓冲（0xAARRGGBB，内存字节序 B,G,R,A——与 lc_frame_io_windows.cpp 的 32bpp DIB 帧布局
/// 一致）→ CGImage。reverse of captureFrame 的 CGContext 转换；小地图预览与最终输出共用。
/// - Parameters:
///   - bgra: 像素缓冲（count 必须等于 width*height）
///   - width/height: 图像尺寸（像素）
/// - Returns: CGImage；参数非法返回 nil
func lcMakeCGImage(bgra: [UInt32], width: Int, height: Int) -> CGImage? {
    guard width >= 1, height >= 1, bgra.count == width * height else { return nil }
    var buf = bgra
    return buf.withUnsafeMutableBytes { raw -> CGImage? in
        guard let base = raw.baseAddress,
              let ctx = CGContext(data: base, width: width, height: height,
                                  bitsPerComponent: 8, bytesPerRow: width * 4,
                                  space: CGColorSpaceCreateDeviceRGB(),
                                  bitmapInfo: CGImageAlphaInfo.premultipliedFirst.rawValue
                                            | CGBitmapInfo.byteOrder32Little.rawValue) else { return nil }
        return ctx.makeImage()
    }
}

/// 把 CG 全局逻辑矩形翻转为 NS 窗口 frame（NS 原点在主屏左下、Y 向上；浮层同款换算）。
/// - Parameter rect: CG 全局逻辑坐标矩形
/// - Returns: NSWindow.setFrame 用的 NSRect
func lcNSRect(fromCG rect: CGRect) -> NSRect {
    let top = ScreenshotGeometry.primaryScreenHeight() - rect.minY
    return NSRect(x: rect.minX, y: top - rect.height, width: rect.width, height: rect.height)
}

// MARK: - 滚轮观察缓冲（tap 回调线程只写、泵循环消费）

/// 滚轮事件原始增量条目（tap 回调线程合并写入；方向解析延迟到泵消费时按当前模式进行，
/// 避免 tap 线程读会话状态）。line* 为整数行增量（滚轮一格 = ±1 行），point* 为像素级
/// 增量（触控板亚行滚动），shift = 事件携带的 Shift 修饰键。
struct ScreenshotLCWheelSample {
    var lineV: Int32 = 0
    var lineH: Int32 = 0
    var pointV: Double = 0
    var pointH: Double = 0
    var shift = false
    var tickMs: UInt64 = 0
}

/// 滚轮观察共享缓冲（NSLock 保护；等价 Windows Raw Input 广播 → 面板 WM_INPUT 的
/// 「只被动接收、置标志/累加缓冲，不拦截输入」语义）。
final class ScreenshotLCWheelBuffer {
    private let lock = NSLock()
    private var has = false
    private var sample = ScreenshotLCWheelSample()

    /// 写入一次滚轮事件（多次未消费事件合并：行/像素增量累加、方向键时刻取最新）。
    func push(_ s: ScreenshotLCWheelSample) {
        lock.lock()
        defer { lock.unlock() }
        if has {
            sample.lineV &+= s.lineV
            sample.lineH &+= s.lineH
            sample.pointV += s.pointV
            sample.pointH += s.pointH
            sample.shift = s.shift
            sample.tickMs = s.tickMs
        } else {
            sample = s
            has = true
        }
    }

    /// 取出并清空缓冲（泵循环消费；无未消费事件返回 nil）。
    func take() -> ScreenshotLCWheelSample? {
        lock.lock()
        defer { lock.unlock() }
        guard has else { return nil }
        has = false
        return sample
    }

    /// 清空缓冲（方向切换重置会话时调用，防旧方向增量污染新先验）。
    func clear() {
        lock.lock()
        has = false
        lock.unlock()
    }
}

// MARK: - 滚轮观察 event tap（listen-only，等价 RIDEV_INPUTSINK）

/// 当前活跃的长截图滚轮 tap（CGEventTap 回调为 C 函数指针不能捕获上下文，模块级桥接）。
private var lcWheelTapCurrent: ScreenshotLCWheelTap?

/// 长截图滚轮观察 tap 回调（C 函数指针兼容；无捕获）。
private func lcWheelTapCallback(
    _ proxy: CGEventTapProxy, _ type: CGEventType, _ event: CGEvent, _ userInfo: UnsafeMutableRawPointer?
) -> Unmanaged<CGEvent>? {
    guard let current = lcWheelTapCurrent else {
        return Unmanaged.passUnretained(event)
    }
    return current.handleEvent(type: type, event: event)
}

/// 长截图滚轮观察 tap（对齐 lc_frame_io_windows.cpp LongCaptureRegisterWheelObserver 的 Raw Input
/// RIDEV_INPUTSINK：非前台也接收广播、不拦截输入）。macOS 用 `.listenOnly` CGEventTap 监听
/// scrollWheel——事件原样放行（照常送达选区下的目标窗口），这里只被动解析方向与时机。
/// tap 回调线程只把原始增量写入共享缓冲（ScreenshotLCWheelBuffer），泵循环逐拍消费。
final class ScreenshotLCWheelTap {
    private let buffer: ScreenshotLCWheelBuffer
    private var tap: CFMachPort?
    private var runLoopSource: CFRunLoopSource?
    private var runLoop: CFRunLoop?
    private var stopped = false

    /// - Parameter buffer: 会话侧共享缓冲（tap 生命周期内由会话持有）
    init(buffer: ScreenshotLCWheelBuffer) {
        self.buffer = buffer
    }

    /// 在后台线程创建 listen-only tap 并运行其 RunLoop（照抄取色器/覆盖层 tap 启动模式）。
    func start() {
        DispatchQueue.global(qos: .userInteractive).async { [weak self] in
            guard let self = self else { return }
            let mask: CGEventMask = (1 << CGEventType.scrollWheel.rawValue)
            guard let tap = CGEvent.tapCreate(
                tap: .cgSessionEventTap,
                place: .headInsertEventTap,
                options: .listenOnly,   // 只观察不拦截：等价 RIDEV_INPUTSINK 非前台广播
                eventsOfInterest: mask,
                callback: lcWheelTapCallback,
                userInfo: nil
            ) else {
                print("Error: Failed to create long-capture wheel observer tap. Check accessibility permissions.")
                return
            }
            guard let source = CFMachPortCreateRunLoopSource(kCFAllocatorDefault, tap, 0) else {
                print("Error: Failed to create run loop source for long-capture wheel tap")
                CFMachPortInvalidate(tap)
                return
            }
            self.tap = tap
            self.runLoopSource = source
            self.runLoop = CFRunLoopGetCurrent()
            if self.stopped {
                // 会话在 tap 就绪前已收口：就地清理，避免泄漏
                self.stop()
                return
            }
            // source 必须挂进当前 RunLoop，tap 的 mach port 才会被调度读取——缺失时
            // tap 创建并 enable 成功也永远收不到回调（滚轮观察静默失效，采样轮永不触发；
            // 对齐 ZToolsNative.swift 取色器/鼠标监听的标准启动序：AddSource → Enable → Run）
            CFRunLoopAddSource(CFRunLoopGetCurrent(), source, .commonModes)
            lcWheelTapCurrent = self
            CGEvent.tapEnable(tap: tap, enable: true)
            CFRunLoopRun()
        }
    }

    /// 停止并释放 tap（会话收口时调用；CFRunLoopStop 线程安全）。
    func stop() {
        stopped = true
        lcWheelTapCurrent = nil
        if let tap = tap {
            CGEvent.tapEnable(tap: tap, enable: false)
        }
        if let source = runLoopSource, let runLoop = runLoop {
            CFRunLoopRemoveSource(runLoop, source, .commonModes)
        }
        if let tap = tap {
            CFMachPortInvalidate(tap)
        }
        if let runLoop = runLoop {
            CFRunLoopStop(runLoop)
        }
        tap = nil
        runLoopSource = nil
        runLoop = nil
    }

    /// tap 回调主体：解析滚轮事件原始增量写入缓冲后原样放行（listen-only 恒放行）；
    /// tap 被系统超时禁用（0xFFFFFFFE/0xFFFFFFFF）时重新启用（取色器同款处理）。
    /// - Parameters:
    ///   - type: 事件类型
    ///   - event: 原始事件
    /// - Returns: 恒为放行的事件（本 tap 不消费任何输入）
    func handleEvent(type: CGEventType, event: CGEvent) -> Unmanaged<CGEvent>? {
        if type.rawValue == 0xFFFFFFFE || type.rawValue == 0xFFFFFFFF {
            if let tap = tap {
                CGEvent.tapEnable(tap: tap, enable: true)
            }
            return Unmanaged.passUnretained(event)
        }
        if type == .scrollWheel {
            var s = ScreenshotLCWheelSample()
            // 纵向：Axis1 行增量（整数）+ 像素级增量（触控板亚行滚动）
            s.lineV = Int32(clamping: event.getIntegerValueField(.scrollWheelEventDeltaAxis1))
            s.lineH = Int32(clamping: event.getIntegerValueField(.scrollWheelEventDeltaAxis2))
            s.pointV = event.getDoubleValueField(.scrollWheelEventPointDeltaAxis1)
            s.pointH = event.getDoubleValueField(.scrollWheelEventPointDeltaAxis2)
            s.shift = event.flags.contains(.maskShift)
            s.tickMs = lcNowMs()
            buffer.push(s)
        }
        return Unmanaged.passUnretained(event)
    }
}

// MARK: - 长截图灰蒙版视图

/// 长截图灰蒙版自绘视图（对齐 lc_panel_ui_windows.cpp EnterLongCaptureMask 的三层绘制）：
/// 1) 整屏半透明灰 RGB(44,44,44) alpha 0xA0；2) 选区蓝色描边（路径 = 采样区向外偏移
/// 2px 即选区矩形，2.5px 居中描边）；3) 采样裁剪区清全透明（SourceCopy 语义，透出实况
/// 桌面；放在描边之后，擦除描边内半圈越界的抗锯齿像素，保证抓屏取样范围内绝无蒙版像素）。
final class ScreenshotLCMaskView: NSView {
    /// 本窗口左上角的 CG 全局坐标（本地坐标 = CG 全局坐标 - cgOrigin）。
    let cgOrigin: CGPoint
    /// 选区矩形（CG 全局逻辑坐标）。
    let selection: CGRect
    /// 采样裁剪矩形 = 选区每边内缩 2px（CG 全局逻辑坐标）。
    let cropRect: CGRect
    /// 描边路径圆角半径（选区圆角外扩 2px；对齐 Windows AddRoundedRect 的 radius 参数）。
    let cornerRadius: CGFloat

    init(cgOrigin: CGPoint, selection: CGRect, cropRect: CGRect, cornerRadius: CGFloat, frame: NSRect) {
        self.cgOrigin = cgOrigin
        self.selection = selection
        self.cropRect = cropRect
        self.cornerRadius = cornerRadius
        super.init(frame: frame)
    }

    required init?(coder: NSCoder) {
        fatalError("ScreenshotLCMaskView is created programmatically only")
    }

    override var isFlipped: Bool { return true }   // 本地坐标与 CG 全局坐标同向

    /// 三层蒙版绘制（绘制次序与 Windows 逐条对齐，见类注释）。
    override func draw(_ dirtyRect: NSRect) {
        guard let ctx = NSGraphicsContext.current?.cgContext else { return }
        // 1) 整屏半透明灰（LONG_MASK_GRAY=44 / LONG_MASK_ALPHA=0xA0，lc_panel_ui_windows.cpp）
        ctx.setFillColor(NSColor(srgbRed: LC_MASK_GRAY_COMPONENT / 255.0,
                                 green: LC_MASK_GRAY_COMPONENT / 255.0,
                                 blue: LC_MASK_GRAY_COMPONENT / 255.0,
                                 alpha: LC_MASK_ALPHA / 255.0).cgColor)
        ctx.fill(bounds)
        // 2) 选区蓝描边（SC_THEME_ACCENT_BLUE = RGB(0x00,0x88,0xFF)，2.5px，internal.h；
        //    路径 = 选区矩形 = 采样区向外偏移 2px，圆角半径同步外扩 2px）
        let localSel = selection.offsetBy(dx: -cgOrigin.x, dy: -cgOrigin.y)
        let radius = min(max(cornerRadius, 0), min(localSel.width, localSel.height) / 2) + 2
        let path = CGPath(roundedRect: localSel, cornerWidth: radius, cornerHeight: radius, transform: nil)
        ctx.addPath(path)
        ctx.setStrokeColor(NSColor(srgbRed: 0, green: 0x88 / 255.0, blue: 1.0, alpha: 1).cgColor)
        ctx.setLineWidth(2.5)
        ctx.strokePath()
        // 3) 采样裁剪区清全透明（SourceCopy：整像素擦除该矩形全部通道，透出实况桌面）
        let localCrop = cropRect.offsetBy(dx: -cgOrigin.x, dy: -cgOrigin.y)
        ctx.setBlendMode(.copy)
        ctx.setFillColor(NSColor.clear.cgColor)
        ctx.fill(localCrop)
        ctx.setBlendMode(.normal)
    }
}

// MARK: - 长截图会话主体

/// 长截图滚动捕获会话：持有灰蒙版窗口组、小地图/工具栏控制器、滚轮观察 tap、
/// 算法层会话与全部会话侧簿记字段（lc_session_windows.cpp 中归属会话层的 wheelPending/lastDir/
/// noChangeCount/reachedBottom/weakTries/frameCount/autoFailStreak 等；拼接累计状态全部
/// 在算法层 LCAlgorithmSession 内，会话层绝不直接触碰）。生命周期：beginLongCapture 创建
/// → start()（蒙版/首帧/浮层/tap）→ lcTick() 由覆盖层泵循环逐拍驱动 → 终止条件收束
/// （完成/保存成功 → success 回调；取消/ESC/abort/失败 → 整会话 {success:false} 收束，
/// 对齐 wndproc_windows.cpp WM_LONGCAPTURE_RUN 结束后 ctx->state = CS_Done + DestroyWindow）。
final class ScreenshotLongCaptureSession {
    private weak var overlay: ScreenshotOverlaySession?

    // ---- 会话配置（解析链路透传；对齐 g_lcInterval）----
    let intervalMs: Int

    // ---- 几何（CG 全局逻辑坐标）----
    /// 编辑态选区（CG 全局逻辑坐标，整数点）。
    let selection: CGRect
    /// 采样裁剪矩形 = 选区每边内缩 2px（CalcSampleCrop 语义，防选区描边入画，矩阵 #36）。
    let cropRect: CGRect
    /// 选区所在屏的物理/逻辑缩放比（多屏时抓选区所在屏，见 init 注释）。
    let scale: CGFloat
    /// 抓帧物理尺寸（未转置；横向模式帧缓冲转置后 physW/physH 互换，对齐 lc->capW/capH）。
    let capW: Int
    let capH: Int

    /// 是否横向模式（进入时恒为纵向；方向切换经工具栏，对齐 lc->horizontal）。
    private(set) var horizontal = false
    /// 帧缓冲尺寸（横向模式为 capW/capH 的转置，对齐 lc->physW/physH）。
    private(set) var physW: Int
    private(set) var physH: Int

    /// 蒙版窗口号（实况抓帧排除自身浮层的基准窗口；start() 创建蒙版后写入，
    /// 此后只读。0 = 未就绪——抓帧前置条件校验会拒绝抓帧）。
    private var maskNumber: CGWindowID = 0

    // ---- 算法层会话（拼接累计状态唯一归属；交互只经 LCBridgeMac.swift）----
    private(set) var algo: LCAlgorithmSession?

    // ---- 会话侧簿记（对齐 LongCaptureContext 会话层字段，lc_session_windows.cpp）----
    /// 滚轮停稳防抖待采样标志（WM_INPUT 置位、停稳采样消费）。
    var wheelPending = false
    /// 最近滚轮方向（+1 向下 / -1 向上 / 0 未知；喂给 lc_try_stitch 的 dir）。
    var lastDir: Int32 = 0
    /// 最近滚轮事件时刻 ms（停稳判定基准）。
    var lastWheelTickMs: UInt64 = 0
    /// 上次采样轮结束时刻 ms（滚动中主动节拍基准）。
    var lastSampleTickMs: UInt64 = 0
    /// 连续 NoChange 计数（达到 LC_BOTTOM_CONFIRM_SAMPLES 确认到底）。
    var noChangeCount = 0
    /// 已确认滚动到底（autoScroll 自动停止条件之一）。
    var reachedBottom = false
    /// Weak 候选独立采样轮计数（耗尽即放弃候选链）。
    var weakTries = 0
    /// 已拼接帧数（首帧 = 1；仅用于方向锁定判定，不设上限、不触发自动完成）。
    var frameCount = 0
    /// 自动滚动开启中。
    var autoScroll = false
    /// 自动滚动连续硬失败计数（达到 LC_AUTOSCROLL_STOP_FAILS 自动停止）。
    var autoFailStreak = 0

    // ---- 采样轮内重试梯状态（对齐 RunLongCapture 轮内变量）----
    private var inSampleRound = false
    private var normalTries = 0
    private var weakRetries = 0
    private var stableWaits = 0
    private var quickResamples = 0
    private var nextAttemptMs: UInt64 = 0
    private var trackRevBefore = 0
    private var lastFailReason: LCFailReason = .none
    private var lastResult: LCTryStitchResult?

    // ---- 工具栏动作标志（工具栏控制器在主线程泵内置位；lc->finishFlag/saveFlag/abortFlag）----
    var finishRequested = false
    var saveRequested = false

    // ---- UI 与观察器 ----
    private var maskWindows: [NSWindow] = []
    private(set) var panel: ScreenshotLCPanelController?
    private(set) var toolbar: ScreenshotLCToolbarController?
    private var wheelTap: ScreenshotLCWheelTap?
    private let wheelBuffer = ScreenshotLCWheelBuffer()

    /// 自动滚动上次注入时刻 ms（31ms 节拍基准；SetTimer 等价物）。
    private var lastAutoScrollTickMs: UInt64 = 0
    /// 工具栏 UI 维护节拍上次触发时刻 ms（100ms 轮询，LC_TIMER_UI 等价物）。
    private var lastUiTickMs: UInt64 = 0
    /// 长截图主循环是否已收束（防二次收束）。
    private var ended = false

    /// 小地图缩略列宽（物理像素；对齐 lc->thumbW = min(面板预览内宽, physW)）。
    let thumbW: Int

    // MARK: 初始化（对齐 BeginLongCapture 的上下文构造段）

    /// 计算几何并构造会话（不抓屏、不建窗口）。选区过小（内缩后宽/高 < 1 物理像素）返回 nil
    /// （对齐 Windows BeginLongCapture 的 delete lc; return——会话停留在编辑态，无回调）。
    /// 多屏映射：Windows 为单虚拟屏单一 dpiScale；macOS 选区可能跨屏且各屏缩放不同，
    /// 本实现取「采样裁剪矩形中心所在屏」的 backingScaleFactor 作为采样/输出比例
    /// （与区域截图确认输出路径 baseFrame.scale 的单比例模型一致，混合 DPI 跨屏选区为
    /// 已知近似，与 Windows 侧同级别局限一致）。
    init?(overlay: ScreenshotOverlaySession, selection sel: CGRect) {
        self.overlay = overlay
        self.intervalMs = overlay.options.longCaptureIntervalMs
        let sel = sel.standardized
        self.selection = sel

        // 采样裁剪 = 选区每边内缩（lc_panel_ui_windows.cpp CalcSampleCrop 逐式移植；
        // inset 取算法层常量 LC_CROP_INSET_LOGI = 2，防漂移）
        let inset = CGFloat(LCAlgoConsts.shared.cropInsetLogical)
        let cropL = min(sel.maxX - 1, sel.minX + inset)
        let cropT = min(sel.maxY - 1, sel.minY + inset)
        let cropR = max(cropL + 1, sel.maxX - inset)
        let cropB = max(cropT + 1, sel.maxY - inset)
        let crop = CGRect(x: cropL, y: cropT, width: cropR - cropL, height: cropB - cropT)
        self.cropRect = crop

        // 选区所在屏与缩放比（裁剪矩形中心命中；无命中退主屏）
        let center = CGPoint(x: crop.midX, y: crop.midY)
        let screen = NSScreen.screens.first {
            ScreenshotGeometry.cgFrame(of: $0).contains(center)
        } ?? NSScreen.screens.first
        guard let screen = screen else { return nil }
        self.scale = screen.backingScaleFactor

        // 抓帧物理尺寸（(int)(v*ds + 0.5) 语义）；过小 = 选区过小
        let w = Int((crop.width * scale) + 0.5)
        let h = Int((crop.height * scale) + 0.5)
        guard w >= 1, h >= 1 else { return nil }
        self.capW = w
        self.capH = h
        self.physW = w          // 进入时恒为纵向（横向 physW/physH 随方向切换互换）
        self.physH = h

        // 小地图缩略列宽 = 面板预览内宽（物理像素），不超过帧宽（lc_session_windows.cpp BeginLongCapture）
        let previewPx = Int(((LC_PANEL_WIDTH - 2 * LC_PANEL_PAD_PX) * scale) + 0.5)
        self.thumbW = min(max(1, previewPx), physW)
    }

    // MARK: 生命周期

    /// 启动长截图会话：隐藏覆盖层 → 创建灰蒙版 → 算法层会话 + 首帧基准 → 小地图/工具栏
    /// → 滚轮观察 tap。任一关键步骤失败时 FailFast 收束整个截图会话（对齐 Windows
    /// LongCaptureInitFirstFrame 失败 → LongCaptureEmitFailure → 会话清理，矩阵 #48）。
    /// - Returns: true 会话就绪（state 置 .longCapturing 后由泵循环驱动）；false 已收束
    func start() -> Bool {
        guard let ov = overlay else { return false }

        // 0) 重置 JS 中止标志（会话创建时重置，防上次会话遗留置位误收束）
        resetLongCaptureAbort()

        // 1) 隐藏覆盖层与编辑工具栏（Windows ShowWindow(SW_HIDE)：甜甜圈设计不再使用，
        //    由独立灰蒙版接管；编辑工具栏为独立浮层窗口需一并隐藏）
        ov.hideOverlayWindowsForLongCapture()
        ov.toolbar.syncVisibility(false)

        // 2) 灰蒙版：每 NSScreen 一个 borderless 窗口（与覆盖层同规格），
        //    整窗点击穿透（ignoresMouseEvents=true）+ 选区描边 + 采样区全透明
        guard createMaskWindows() else {
            finishWholeSession(payload: ov.failurePayload("failed to create long capture mask"))
            return false
        }

        // 3) 算法层会话（进入时恒纵向；缩略图列宽随帧宽钳制由 bridge 完成）
        do {
            algo = try LCAlgorithmSession(
                width: physW, height: physH,
                config: LCAlgorithmSession.Config(
                    interval: intervalMs, thumbW: thumbW, horizontal: false))
        } catch {
            finishWholeSession(payload: ov.failurePayload("failed to init long capture algorithm session"))
            return false
        }

        // 4) 首帧基准：进入长截图时选区内的当前内容即首屏（既作主体段也作重叠检测基准；
        //    LongCaptureInitFirstFrame 失败 = 抓帧失败 → FailFast，绝不把黑图报成功）
        if !initBaseline() {
            finishWholeSession(payload: "{\"success\":false}")
            return false
        }
        lastSampleTickMs = lcNowMs()

        // 5) 小地图面板 + 长截图工具栏（后于蒙版创建保证位于其上；蒙版点击穿透不影响交互）
        panel = ScreenshotLCPanelController(session: self)
        panel?.create()
        toolbar = ScreenshotLCToolbarController(session: self)
        toolbar?.create()

        // 6) 滚轮观察 tap（listen-only；注册失败不致命——仅失去自动采样时机来源，
        //    用户仍可用 autoScroll 注入的滚轮驱动采样。Windows 注册失败即 EmitFailure，
        //    macOS listen-only 与 Raw Input 的失败面不同，降级为继续会话）
        let tap = ScreenshotLCWheelTap(buffer: wheelBuffer)
        tap.start()
        wheelTap = tap

        // 7) 面板初始内容 + 工具栏初始宽×高标签（对齐 RunLongCapture 开头的两次刷新）
        panel?.contentChanged()
        toolbar?.refreshAll()
        return true
    }

    /// 创建每屏灰蒙版窗口（规格与覆盖层一致：borderless、screenSaver+1、
    /// canJoinAllSpaces/fullScreenAuxiliary；整窗点击穿透 = ignoresMouseEvents）。
    /// - Returns: 创建成功与否（无屏幕或选区所在屏窗口创建失败返回 false）
    private func createMaskWindows() -> Bool {
        let screens = NSScreen.screens
        guard !screens.isEmpty else { return false }
        let center = CGPoint(x: cropRect.midX, y: cropRect.midY)
        for screen in screens {
            let cgFrame = ScreenshotGeometry.cgFrame(of: screen)
            let window = ScreenshotPanelWindow(
                contentRect: screen.frame, styleMask: .borderless, backing: .buffered, defer: false)
            window.level = NSWindow.Level(rawValue: NSWindow.Level.screenSaver.rawValue + 1)
            window.isOpaque = false
            window.backgroundColor = .clear
            window.hasShadow = false
            window.collectionBehavior = [.canJoinAllSpaces, .fullScreenAuxiliary]
            window.isReleasedWhenClosed = false
            window.ignoresMouseEvents = true   // 整窗点击穿透（Windows WS_EX_TRANSPARENT 等价）
            let view = ScreenshotLCMaskView(
                cgOrigin: cgFrame.origin, selection: selection, cropRect: cropRect,
                cornerRadius: overlay?.selectionCornerRadius ?? 0,
                frame: NSRect(origin: .zero, size: cgFrame.size))
            window.contentView = view
            maskWindows.append(window)
            // 记录选区所在屏的蒙版窗口号（实况抓帧排除基准）
            if cgFrame.contains(center) {
                maskNumber = CGWindowID(window.windowNumber)
            }
        }
        for w in maskWindows {
            w.orderFrontRegardless()
        }
        return maskNumber != 0
    }

    /// 抓取首帧并初始化算法层基准（对齐 lc_frame_io_windows.cpp LongCaptureInitFirstFrame）。
    /// - Returns: 首帧抓取/初始化成功与否
    private func initBaseline() -> Bool {
        guard let algo = algo, let frame = captureFrame() else { return false }
        do {
            try algo.initBaseline(bgra: frame)
        } catch {
            return false
        }
        frameCount = 1
        return true
    }

    // MARK: 泵循环驱动（Windows RunLongCapture 主循环的 tick 化等价物）

    /// 泵循环逐拍入口（覆盖层 pumpTick 在 .longCapturing 态调用）：检查点消费 + 采样轮推进
    /// + autoScroll 节拍 + UI 维护节拍。单次调用内不做任何阻塞等待（重试梯以「下次尝试
    /// 时刻」表达，等价 Windows LongCaptureWaitMessages 的有界等待）。
    func lcTick() {
        guard !ended, !overlayFinished() else { return }
        let now = lcNowMs()

        // 检查点 1：abortLongCapture（JS 线程置位）→ 整会话失败收束（LongCaptureAbort 语义）
        if consumeLongCaptureAbort() {
            finishWholeSession(payload: "{\"success\":false}")
            return
        }
        // 检查点 2：完成并复制（工具栏 LTI_Finish → finishFlag）
        if finishRequested {
            finishRequested = false
            finishAndCopy()
            return
        }
        // 检查点 3：保存到本地（工具栏 LTI_Save → saveFlag；取消对话框则继续捕获）
        if saveRequested {
            saveRequested = false
            handleSave()
            if ended { return }
        }

        // 消费滚轮观察缓冲（tap 线程只写缓冲，方向解析在泵线程按当前模式进行）
        drainWheel(now: now)

        // 采样轮（三条件触发 + 重试梯推进 + 结局簿记）
        pumpSampleRound(now: now)
        if ended { return }

        // 自动滚动节拍（31ms 注入；LongCaptureAutoScrollTick 等价）
        tickAutoScroll(now: now)

        // 工具栏 UI 维护节拍（100ms 轮询：popover 悬停展开/宽限收起/tooltip/菜单外点关闭）
        if now - lastUiTickMs >= 100 {
            lastUiTickMs = now
            toolbar?.uiTick()
        }
    }

    /// 会话是否已被收束（覆盖层 finish 后残余 tick 防御）。
    private func overlayFinished() -> Bool {
        return overlay?.isRunning == false
    }

    // MARK: 滚轮消费（lc_panel_ui_windows.cpp WM_INPUT 分支逐条照搬）

    /// 解析并消费滚轮观察缓冲：纵向模式消费纵滚轮（忽略横滚轮）；横向模式消费横滚轮与
    /// Shift+纵滚轮。方向解析在泵线程按当前模式进行（tap 线程只缓冲原始增量）。
    /// 行增量按 1 行 = 1 notch 归一到 Windows WHEEL_DELTA=120 计入软先验（只参与候选
    /// 排序加分，绝不约束搜索范围）；亚行像素级滚动（触控板慢滚，行增量 = 0）只驱动
    /// 采样时机与方向，不计入 notch 先验，保持 px/notch 估计的量纲诚实。
    private func drainWheel(now: UInt64) {
        guard let e = wheelBuffer.take() else { return }
        var dir: Int32 = 0
        var accum: Int32 = 0
        var consume = false
        if !horizontal {
            // 纵向模式：正 delta = 向上滚（d<0 方向）；忽略横滚轮
            if e.lineV != 0 {
                dir = e.lineV > 0 ? -1 : 1
                accum = e.lineV * LC_WHEEL_DELTA_UNITS
                consume = true
            } else if e.pointV != 0 {
                dir = e.pointV > 0 ? -1 : 1
                consume = true
            }
        } else if e.lineH != 0 {
            // 横滚轮：正 delta = 向右滚 = 追加尾部（accum 取反对齐 wheelAccumDelta 符号约定）
            dir = e.lineH > 0 ? 1 : -1
            accum = -e.lineH * LC_WHEEL_DELTA_UNITS
            consume = true
        } else if e.pointH != 0 {
            // 亚行像素级横滚（触控板慢滚 / 本会话 autoScroll 注入的 pixel 增量）：
            // 只驱动采样时机与方向，不计入 notch 先验
            dir = e.pointH > 0 ? 1 : -1
            consume = true
        } else if e.shift && e.lineV != 0 {
            // Shift+滚轮向下 = 向右滚（多数应用把 Shift+滚轮翻译为水平滚动）
            dir = e.lineV < 0 ? 1 : -1
            accum = e.lineV * LC_WHEEL_DELTA_UNITS
            consume = true
        } else if e.shift && e.pointV != 0 {
            dir = e.pointV < 0 ? 1 : -1
            consume = true
        }
        guard consume else { return }
        wheelPending = true
        lastDir = dir
        lastWheelTickMs = e.tickMs
        if accum != 0 {
            algo?.accumulateWheelDelta(delta: accum)
        }
    }

    // MARK: 采样轮（lc_session_windows.cpp RunLongCapture 的采样三条件 + 重试梯）

    /// 采样轮推进：三条件任一满足则开启一轮（停稳防抖 / 滚动中主动节拍 / Weak 待复核），
    /// 轮内按重试梯推进（稳定性闸门 → 瞬态快重采样 → Normal/Weak 重试档），绝不放宽匹配
    /// 条件；重试耗尽仍硬失败 → 本轮放弃：不拼接、不污染基准、绝不自动完成。
    private func pumpSampleRound(now: UInt64) {
        let consts = LCAlgoConsts.shared
        if !inSampleRound {
            // 触发三条件（lc_session_windows.cpp L202-208 逐式移植）：
            //   · 停稳采样：滚轮 interval 内无新事件——捕获滚动尾段，消费 wheelPending；
            //   · 滚动中主动采样：距上次采样已达 min(interval, LC_SCROLL_SAMPLE_MAX_GAP)，
            //     不等停稳主动抓帧拼接（不消费 wheelPending，停稳后仍补收尾采样）；
            //   · Weak 待复核：存在 pendingMatch 时即使没有新滚轮也继续采样。
            let scrollGap = min(intervalMs, consts.scrollSampleMaxGap)
            let settle = wheelPending && now - lastWheelTickMs >= UInt64(intervalMs)
            let midScroll = wheelPending && !settle && now - lastSampleTickMs >= UInt64(scrollGap)
            let weakRecheck = (algo?.state.pendingValid ?? false)
            guard settle || midScroll || weakRecheck else { return }
            if settle { wheelPending = false }
            trackRevBefore = algo?.state.trackingRevision ?? 0
            normalTries = 0
            weakRetries = 0
            stableWaits = 0
            quickResamples = 0
            lastFailReason = .none
            lastResult = nil
            inSampleRound = true
            nextAttemptMs = now
        }
        guard now >= nextAttemptMs else { return }

        // 抓帧 → 识别 → 校验 →（Weak 档延迟确认）→（全部通过才）提交（LongCaptureTryStitch）。
        // 抓帧失败按瞬态故障走 Normal 重试梯（Windows：BitBlt 失败同样落入 Failed 分支）；
        // lastFailReason 清零防上一轮失败分类误触发快重采样分支。
        var oc: LCSampleOutcome = .failed
        if let algo = algo, let frame = captureFrame() {
            do {
                // 稳定性闸门仅在等待预算内启用（自动滚动持续注入时页面恒处微滚动，
                // 闸门注定不放行，直接跳过以免白耗重试预算——对齐 RunLongCapture 传参）
                let allowGate = !autoScroll && stableWaits < consts.stableMaxWaits
                let result = try algo.tryStitch(bgra: frame, direction: lastDir, allowStabilityGate: allowGate)
                oc = result.outcome
                lastFailReason = result.failReason
                lastResult = result
            } catch {
                oc = .failed
                lastFailReason = .none
            }
        }

        // 结局分流 + 轮内重试梯（语义 =「等待页面稳定」，绝不放宽匹配条件）
        switch oc {
        case .stitched, .repositioned:
            // 已提交（新增行）或已重定位（回滚到已捕获范围内）：本轮即告结束，重试只会
            // 对着新基准匹配到相同内容
            endSampleRound(outcome: oc, now: now)
            return
        case .weakPending, .weakRejected:
            // Weak 候选：延迟确认需要更多次稳定采样，间隔走 weak 档
            if weakRetries >= consts.weakRetryAttempts {
                endSampleRound(outcome: oc, now: now)
                return
            }
            nextAttemptMs = now + UInt64(consts.retryDelayWeak[weakRetries])
            weakRetries += 1
        case .unstable:
            // 稳定性未过：短延迟后重新采样（预算独立于 Normal/Weak 重试梯）
            if stableWaits >= consts.stableMaxWaits {
                endSampleRound(outcome: oc, now: now)
                return
            }
            nextAttemptMs = now + UInt64(consts.stableRetryDelay[stableWaits])
            stableWaits += 1
        case .failed:
            if quickResamples < consts.quickResamples
                && (lastFailReason == .noCandidate || lastFailReason == .globalMismatch) {
                // 瞬态失败（无候选/全宽不可评——多为抓在滚动/重绘过渡帧）：几十毫秒级
                // 快重采样等页面稳定，而不是放宽匹配条件；不消耗主重试预算
                nextAttemptMs = now + UInt64(consts.resampleDelayQuick[quickResamples])
                quickResamples += 1
            } else if normalTries >= consts.sampleAttempts - 1 {
                endSampleRound(outcome: oc, now: now)
                return
            } else {
                nextAttemptMs = now + UInt64(consts.retryDelayNormal[normalTries])
                normalTries += 1
            }
        case .noChange:
            // 内容未变化：与「匹配失败」完全独立的事件；按常规重试等待渲染稳定后重试
            if normalTries >= consts.sampleAttempts - 1 {
                endSampleRound(outcome: oc, now: now)
                return
            }
            nextAttemptMs = now + UInt64(consts.retryDelayNormal[normalTries])
            normalTries += 1
        }
    }

    /// 采样轮收束：按最终结局更新会话侧簿记 + 终止条件检查 + autoScroll 自动停止
    /// （对齐 lc_session_windows.cpp L247-300 的采样轮后处理，逐分支移植）。
    private func endSampleRound(outcome: LCSampleOutcome, now: UInt64) {
        inSampleRound = false
        lastSampleTickMs = now
        switch outcome {
        case .stitched:
            noChangeCount = 0
            weakTries = 0
            frameCount += 1
            panel?.contentChanged()      // 面板尺寸重算 + 重绘（LongCapturePanelUpdate）
            toolbar?.refreshAll()        // 宽×高标签刷新（LongCaptureToolbarRepaint）
        case .repositioned:
            // 反向回滚未越出已捕获边界：无新增行——不计帧数、不重算面板尺寸
            noChangeCount = 0
            weakTries = 0
        case .noChange:
            // 连续多次未变化才确认滚动到底；匹配失败绝不计入该计数
            weakTries = 0
            noChangeCount += 1
            if noChangeCount >= LCAlgoConsts.shared.bottomConfirmSamples {
                reachedBottom = true
            }
        case .weakPending, .weakRejected:
            // 「发现了候选，但 Weak 验证/复核不足」：不计入任何终止条件；独立预算耗尽
            // 即放弃当前候选链，从干净基准重新观察
            noChangeCount = 0
            weakTries += 1
            if weakTries >= LCAlgoConsts.shared.weakMaxTries {
                weakTries = 0
                algo?.abandonWeakChain()   // pendingMatch 作废 + 时间一致性样本清空
            }
        case .unstable, .failed:
            // 硬失败（重试耗尽仍无可信对齐）：不拼接、不污染基准、也绝不自动完成
            noChangeCount = 0
        }
        // 自动滚动的自动停止：确认滚动到底，或连续多次硬失败（内容可能已滚出可匹配
        // 范围，继续注入滚轮只会丢内容）时停止注入——会话保留，由用户处置
        if autoScroll {
            if outcome == .failed {
                autoFailStreak += 1
                if autoFailStreak >= LC_AUTOSCROLL_STOP_FAILS {
                    setAutoScroll(false)
                }
            } else if outcome != .unstable && outcome != .weakPending {
                autoFailStreak = 0
            }
            if reachedBottom {
                setAutoScroll(false)
            }
        }
        // tentative 跟踪状态在本轮发生变化（多跳恢复/预测推进/候选否决回退）：
        // 小地图按「预计位置」刷新——正式拼接未变，不触发面板尺寸重算
        if let algo = algo, algo.state.trackingRevision != trackRevBefore {
            panel?.trackingChanged()
        }
        lastResult = nil
    }

    // MARK: 实况抓帧（lc_frame_io_windows.cpp LongCaptureCaptureFrameBuf 的 macOS 等价物）

    /// 抓取选区当前视口帧并转为算法层 BGRA 缓冲：
    /// 1) CGWindowListCreateImage(.optionOnScreenBelowWindow, 蒙版窗口号)——蒙版是选区
    ///    所在屏的最上层窗口，「其下」即用户内容（小地图/工具栏层级更高同样被排除，
    ///    CGWindowListCreateImage 在 macOS 14+ deprecated 但可用）；
    /// 2) 转换为 BGRA 物理像素缓冲（premultipliedFirst + byteOrder32Little = 内存字节序
    ///    B,G,R,A，对齐 lc_frame_io 的 32bpp 自上而下 DIB 帧布局）；
    /// 3) 横向模式帧缓冲转置（capW×capH → capH×capW = physW×physH，水平滚动位移映射为
    ///    垂直位移，整条管线与纵向同构——lc_frame_io_windows.cpp 同款 T[r][q] = S[q][r]）。
    /// - Returns: physW×physH 的 BGRA 帧缓冲；抓帧/转换失败返回 nil（绝不返回残帧）
    private func captureFrame() -> [UInt32]? {
        guard maskNumber != 0 else { return nil }
        guard let img = CGWindowListCreateImage(
            cropRect, .optionOnScreenBelowWindow, maskNumber, .bestResolution) else { return nil }
        // 帧内容写入 capW×capH（未转置）BGRA 缓冲
        var raw = [UInt32](repeating: 0, count: capW * capH)
        let ok = raw.withUnsafeMutableBytes { ptr -> Bool in
            guard let base = ptr.baseAddress,
                  let ctx = CGContext(data: base, width: capW, height: capH,
                                      bitsPerComponent: 8, bytesPerRow: capW * 4,
                                      space: CGColorSpaceCreateDeviceRGB(),
                                      bitmapInfo: CGImageAlphaInfo.premultipliedFirst.rawValue
                                                | CGBitmapInfo.byteOrder32Little.rawValue) else { return false }
            ctx.interpolationQuality = .none
            ctx.draw(img, in: CGRect(x: 0, y: 0, width: CGFloat(capW), height: CGFloat(capH)))
            return true
        }
        guard ok else { return nil }
        guard horizontal else { return raw }
        // 横向模式：帧缓冲转置（对齐 lc_frame_io_windows.cpp 的 T(行 r<capW, 列 q<capH) = S(行 q, 列 r)）
        var transposed = [UInt32](repeating: 0, count: capW * capH)
        for r in 0..<capW {
            for q in 0..<capH {
                transposed[r * capH + q] = raw[q * capW + r]
            }
        }
        return transposed
    }

    // MARK: autoScroll（lc_toolbar_ui_windows.cpp LongCaptureSetAutoScroll / LongCaptureAutoScrollTick）

    /// 开关自动滚动。开启时一次性把光标移到采样区中心（后续每拍只注入滚轮、不再改动
    /// 鼠标位置——光标停驻选区中心 = 注入事件持续路由到选区下的目标窗口；用户仍可随时
    /// 手动移开鼠标接管操作）。注入节拍固定 31ms 高频小步长（连续平滑滚动），用户
    /// interval 只继续作为采样防抖参数——拼接管线按实际位移自适应，与注入节奏解耦。
    func setAutoScroll(_ on: Bool) {
        guard autoScroll != on else { return }
        autoScroll = on
        autoFailStreak = 0
        if on {
            noChangeCount = 0
            reachedBottom = false
            // 一次性把光标移到选区中心（对齐 SetCursorPos 到采样区物理中心；CG 全局
            // 坐标即 macOS 的全局显示坐标，与 Windows SetCursorPos 的屏幕坐标同语义）
            CGWarpMouseCursorPosition(CGPoint(x: cropRect.midX, y: cropRect.midY))
            lastAutoScrollTickMs = 0
        }
        toolbar?.refreshAll()
    }

    /// 自动滚动注入节拍（31ms；LongCaptureAutoScrollTick 逐条对齐）：
    /// - 停止条件：已确认到底 / 中止 / 完成 / 保存请求 → 停止注入（会话保留由用户处置）；
    /// - 注入内容：纵向 = 向下滚轮（负增量，向下滚 = 追加尾部）；横向 = 向右滚轮（正增量）；
    /// - 滚动路由语义差异：Windows SendInput 按「光标下窗口」路由且有「滚动非活动窗口」
    ///   系统保证；macOS 无该保证——CGEventPost 到 HID 层后由系统路由到光标下窗口
    ///  （Safari/Chrome/Electron 等主流应用支持）。
    /// 注入的滚轮同样被本会话的滚轮 tap 捕获，采样主循环走既有的「滚动中主动采样」路径。
    private func tickAutoScroll(now: UInt64) {
        guard autoScroll else { return }
        if reachedBottom || saveRequested || finishRequested {
            setAutoScroll(false)
            return
        }
        guard now - lastAutoScrollTickMs >= LC_AUTOSCROLL_TICK_MS else { return }
        lastAutoScrollTickMs = now
        let event: CGEvent?
        if horizontal {
            // 向右滚 = 追加尾部（+8px 亚档位增量；两轴事件用带横轴的构造器，pixel 单位）
            event = CGEvent(scrollWheelEvent2Source: nil, units: .pixel,
                            wheelCount: 2, wheel1: Int32(0), wheel2: LC_AUTOSCROLL_STEP_DELTA, wheel3: Int32(0))
        } else {
            // 向下滚 = 追加尾部（-8px 亚档位增量；正增量 = 向上滚）
            event = CGEvent(scrollWheelEvent2Source: nil, units: .pixel,
                            wheelCount: 2, wheel1: -LC_AUTOSCROLL_STEP_DELTA, wheel2: Int32(0), wheel3: Int32(0))
        }
        event?.post(tap: .cghidEventTap)
    }

    // MARK: 裁剪与方向（工具栏回调；算法交互只经 LCAlgorithmSession）

    /// 应用裁剪（对齐 lc_toolbar_ui_windows.cpp LongCaptureApplyCrop 的会话侧调用）：
    /// row = 0 丢弃上方（横向 = 左侧）/ 1 丢弃下方（横向 = 右侧）/ 2 重置（仅已裁剪时）。
    /// 登记只收紧输出行窗口并记录待剔除区间；物理删行由下一次朝该方向的成功提交触发
    /// （CommitStitch 入口的延迟剔除，算法层已实现）。应用后刷新小地图与工具栏。
    func applyCrop(row: Int32) {
        guard let algo = algo, !ended else { return }
        let applied = (try? algo.applyCrop(row: row)) ?? false
        guard applied else { return }
        panel?.contentChanged()      // 面板尺寸随输出行窗口重算（LongCapturePanelUpdate）
        toolbar?.refreshAll()        // badge 高亮（cropped → 裁剪按钮 active 态）
    }

    /// 切换长截图方向（纵向 ⇄ 横向）。已拼接多帧（frameCount > 1）后禁用（坐标系不同
    /// 不可混拼，对齐 LongCaptureSwitchDirection）。macOS 实现 = 销毁并重建算法层会话
    /// （新帧缓冲尺寸）+ 完全重置会话侧簿记 + 重抓首帧（对齐 LongCaptureResetSession）。
    /// 重建失败按抓帧失败收束整个会话（FailFast）。
    func switchDirection() {
        guard frameCount <= 1, !ended, let ov = overlay else { return }
        horizontal.toggle()
        // 帧缓冲转置复用纵向管线：physW/physH 交换（capW/capH 抓帧尺寸不变）
        physW = horizontal ? capH : capW
        physH = horizontal ? capW : capH
        // 缩略图列宽随新帧宽重算：由新算法会话的 thumbW 配置承担（bridge 内钳制）
        algo = try? LCAlgorithmSession(
            width: physW, height: physH,
                config: LCAlgorithmSession.Config(
                    interval: intervalMs, thumbW: thumbW, horizontal: horizontal))
        guard algo != nil else {
            finishWholeSession(payload: ov.failurePayload("failed to reinit long capture session"))
            return
        }
        // 完全重置会话侧簿记（对齐 LongCaptureResetSession 的会话字段段）
        noChangeCount = 0
        reachedBottom = false
        weakTries = 0
        autoFailStreak = 0
        wheelPending = false
        lastDir = 0
        wheelBuffer.clear()
        setAutoScroll(false)
        if !initBaseline() {
            finishWholeSession(payload: "{\"success\":false}")
            return
        }
        lastSampleTickMs = lcNowMs()
        panel?.resetForDirectionChange()
        toolbar?.refreshAll()
    }

    // MARK: 终止收束（lc_session_windows.cpp 主循环各 break 分支 + 输出路径）

    /// 完成并复制（工具栏「完成并复制」按钮触发；对齐
    /// RunLongCapture 主循环结束后的输出块）：构建最终输出 → PNG → 剪贴板 → 成功回调
    /// （回调含 cropRect 坐标与拼接后逻辑尺寸）；任一环节失败按 {success:false} 收束。
    func finishAndCopy() {
        guard !ended, overlay != nil else { return }
        guard let out = buildFinalOutput() else {
            finishWholeSession(payload: "{\"success\":false}")
            return
        }
        guard writePngToPasteboard(out.pngData) else {
            finishWholeSession(payload: "{\"success\":false}")
            return
        }
        finishWholeSession(payload: lcSuccessPayload(base64: out.base64,
                                                     logicalW: out.logicalW, logicalH: out.logicalH))
    }

    /// 保存到本地（工具栏「保存」；对齐 RunLongCapture 主循环的 saveFlag 分支）：
    /// 停自动滚动 → 收起菜单 → 临时降浮层层级（Windows 摘除 TOPMOST 等价）→ 保存对话框
    /// → 取消则继续捕获（无回调）；选定路径则构建输出 → 原子落盘 → base64 回调
    /// （不进剪贴板）并收束整个会话。编码/落盘失败继续捕获（对齐 Windows 的 ok=false
    /// 不 break 分支语义）。
    private func handleSave() {
        guard !ended, let ov = overlay else { return }
        setAutoScroll(false)          // 模态对话框期间不得继续注入滚轮
        toolbar?.closeMenu()          // 收起展开中的二级菜单
        // 弹出前临时降浮层层级 + event tap 放行模态按键（保存流同款，Windows 摘 TOPMOST）
        duckLevels(true)
        ov.saveModalFlag.set()
        defer {
            ov.saveModalFlag.reset()
            duckLevels(false)
        }
        guard let path = scPromptSaveFilePath() else { return }   // 取消对话框 → 继续捕获
        guard let out = buildFinalOutput() else { return }        // 编码失败 → 继续捕获
        do {
            try out.pngData.write(to: URL(fileURLWithPath: path), options: .atomic)
        } catch {
            return                                                 // 落盘失败 → 继续捕获
        }
        // 保存成功：base64 回调（不进剪贴板）并收束整个会话（savedToFile 语义）
        finishWholeSession(payload: lcSuccessPayload(base64: out.base64,
                                                     logicalW: out.logicalW, logicalH: out.logicalH))
    }

    /// 取消长截图（工具栏取消 / ESC）→ 整会话失败收束（对齐 Windows：lc_session_windows.cpp
    /// 主循环 abortFlag → LongCaptureEmitFailure → EmitScreenshotResult(false)，
    /// wndproc WM_LONGCAPTURE_RUN 清理后 ctx->state = CS_Done + DestroyWindow——
    /// 取消不回编辑态）。销毁长截图浮层与滚轮 tap、复位中止标志，随后 finish 收口回调。
    func cancelSessionAsFailure() {
        finishWholeSession(payload: "{\"success\":false}")
    }

    /// 收束整个截图会话（abort / 完成 / 保存成功 / 失败路径的统一出口）：
    /// 销毁长截图浮层 → 覆盖层会话 finish（恰好一次回调 + 复位重入标志）。
    private func finishWholeSession(payload: String) {
        guard !ended else { return }
        ended = true
        setAutoScroll(false)
        lcTeardown()
        guard let ov = overlay else { return }
        ov.longCapture = nil
        ov.finish(payload)
    }

    /// 长截图浮层与观察器清理全集（收束与取消共用；幂等）：销毁蒙版/小地图/工具栏窗口、
    /// 停滚轮 tap、复位长截图态标志（对齐 wndproc_windows.cpp WM_LONGCAPTURE_RUN 清理段）。
    func lcTeardown() {
        wheelTap?.stop()
        wheelTap = nil
        toolbar?.destroy()
        toolbar = nil
        panel?.destroy()
        panel = nil
        for w in maskWindows {
            w.orderOut(nil)
            w.contentView = nil
        }
        maskWindows.removeAll()
        overlay?.longCaptureFlag.reset()
        resetLongCaptureAbort()
    }

    /// 保存对话框模态期间临时降浮层层级（蒙版/小地图/工具栏/popover/tooltip 全组；
    /// Windows LongCaptureSetTopmost(false) 的 macOS 等价——模态面板层级高于浮层族）。
    private var duckedLevels: [ObjectIdentifier: NSWindow.Level] = [:]
    private func duckLevels(_ lowered: Bool) {
        let panels = maskWindows + [panel?.window, toolbar?.window, toolbar?.popoverWindow,
                                    toolbar?.tipWindow].compactMap { $0 }
        if lowered {
            duckedLevels.removeAll()
            for w in panels {
                duckedLevels[ObjectIdentifier(w)] = w.level
                w.level = SC.saveModalDuckLevel
            }
        } else {
            for w in panels {
                if let level = duckedLevels[ObjectIdentifier(w)] {
                    w.level = level
                }
            }
            duckedLevels.removeAll()
        }
    }

    // MARK: 输出（lc_frame_io_windows.cpp LongCaptureBuildResultBitmap + LongCaptureBuildFinalBitmap）

    /// 构建最终输出：读裁剪行窗口拼接缓冲（headRev 倒序头部段 + body 正序主体段的双段
    /// 映射已由 lc_read_rows 解出）→ 横向模式回转（拼接空间 physW×rows → 显示空间
    /// rows×physW）→ 物理像素 1:1 输出（Retina 下不再缩回逻辑尺寸——缩回丢一半分辨率
    /// 导致发虚，与区域截图统一输出管线同口径）→ 单次 PNG 编码，
    /// PNG 携带 DPI 元数据供看图应用按逻辑尺寸显示。
    /// 长截图不合成编辑态标注、不做圆角蒙版——对齐 Windows LongCaptureBuildFinalBitmap
    /// （仅 BuildResultBitmap + 缩放，无 CompositeAnnotations 调用）。
    /// - Returns: PNG 文件字节 + base64 data URL + 最终逻辑尺寸；无内容/编码失败返回 nil
    func buildFinalOutput() -> (pngData: Data, base64: String, logicalW: Int, logicalH: Int)? {
        guard let algo = algo else { return nil }
        // 裁剪行窗口（未裁剪 = [0, stitchH)；裁剪只约束输出，不动拼接缓冲）
        guard let rows = try? algo.outputRows(), rows.bottom > rows.top else { return nil }
        let rowCount = Int(rows.bottom - rows.top)
        guard rowCount > 0, physW > 0,
              let merged = try? algo.readRows(start: Int(rows.top), count: rowCount) else { return nil }

        // 横向模式：转置回原方向（F(行 fy<physW, 列 fx<rows) = M(行 fx, 列 fy)）
        var finalBuf: [UInt32]
        let outWPhys: Int
        let outHPhys: Int
        if horizontal {
            outWPhys = rowCount
            outHPhys = physW
            finalBuf = [UInt32](repeating: 0, count: merged.count)
            for fy in 0..<outHPhys {
                for fx in 0..<outWPhys {
                    finalBuf[fy * outWPhys + fx] = merged[fx * physW + fy]
                }
            }
        } else {
            outWPhys = physW
            outHPhys = rowCount
            finalBuf = merged
        }
        guard let physImg = lcMakeCGImage(bgra: finalBuf, width: outWPhys, height: outHPhys) else {
            return nil
        }

        // 回调 width/height 契约仍为逻辑尺寸（对齐 BuildFinalBitmap 的 (int)(out/ds + 0.5) 取整）
        let lw = Int((Double(outWPhys) / Double(scale)) + 0.5)
        let lh = Int((Double(outHPhys) / Double(scale)) + 0.5)
        guard lw >= 1, lh >= 1 else { return nil }

        // 物理像素 1:1 编码（不重采样）
        guard let ctx = CGContext(
                data: nil, width: outWPhys, height: outHPhys,
                bitsPerComponent: 8, bytesPerRow: outWPhys * 4,
                space: CGColorSpaceCreateDeviceRGB(),
                bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue
                          | CGBitmapInfo.byteOrder32Big.rawValue) else { return nil }
        ctx.interpolationQuality = .none
        ctx.draw(physImg, in: CGRect(x: 0, y: 0, width: CGFloat(outWPhys), height: CGFloat(outHPhys)))
        guard let final = ctx.makeImage() else { return nil }
        let rep = NSBitmapImageRep(cgImage: final)
        // PNG 携带 DPI 元数据（Retina=144dpi）：看图应用按逻辑尺寸显示，等价系统截图
        // 行为；像素数据不变，仅元数据
        rep.size = NSSize(width: CGFloat(lw), height: CGFloat(lh))
        guard let pngData = rep.representation(using: .png, properties: [:]), !pngData.isEmpty else {
            return nil
        }
        return (pngData,
                "data:image/png;base64," + pngData.base64EncodedString(),
                lw, lh)
    }

    /// 构造长截图成功回调 JSON（契约字段与 Windows RunLongCapture 的 EmitScreenshotResult
    /// 一致：x/y/x2/y2 = 采样裁剪矩形（CG 全局逻辑坐标）；width/height = 拼接后输出图像的
    /// 逻辑尺寸——与区域截图不同，纵向长图的 height 是拼接总行数而非选区高）。
    private func lcSuccessPayload(base64: String, logicalW: Int, logicalH: Int) -> String {
        let x = Int(cropRect.minX.rounded())
        let y = Int(cropRect.minY.rounded())
        let x2 = Int(cropRect.maxX.rounded())
        let y2 = Int(cropRect.maxY.rounded())
        return "{"
            + "\"success\":true"
            + ",\"x\":\(x)"
            + ",\"y\":\(y)"
            + ",\"x2\":\(x2)"
            + ",\"y2\":\(y2)"
            + ",\"width\":\(logicalW)"
            + ",\"height\":\(logicalH)"
            + ",\"base64\":\"\(base64)\""
            + "}"
    }

    // MARK: 小地图数据出口（ScreenshotLCPanelMac.swift 使用）

    /// 虚拟屏并集（浮层放置钳制边界；CG 全局逻辑坐标）。
    var virtualBounds: CGRect { overlay?.virtualBounds ?? .null }

    /// 是否存在任何生效的裁剪约束（裁剪按钮 badge 高亮；对齐 LongCaptureContext.cropped，
    /// 含「已登记待剔除区间」的延迟剔除状态）。
    var isCropped: Bool { algo?.hasCropConstraint ?? false }

    /// 预览宽×高标签文本（LongCaptureOutputSizeLabel 移植：固定轴取 cropRect 逻辑尺寸、
    /// 滚动轴取 rows / scale；含裁剪窗口，横向模式宽高已回转）。
    var sizeLabelText: String {
        let win = outputRowWindow
        let rows = win.bottom - win.top
        guard rows > 0 else { return "0 × 0" }
        let w = horizontal ? Double(rows) / Double(scale) : Double(cropRect.width)
        let h = horizontal ? Double(cropRect.height) : Double(rows) / Double(scale)
        return "\(Int(w + 0.5)) × \(Int(h + 0.5))"
    }

    /// 工具栏「完成并复制」→ 置完成标志（主循环检查点收束；对齐 finishFlag）。
    func requestFinish() {
        finishRequested = true
    }

    /// 工具栏「取消」→ 取消长截图：整会话失败收束（对齐 Windows lc_toolbar_ui_windows.cpp
    /// LTI_Cancel → abortFlag 置位 → LongCaptureEmitFailure 路径）。
    func requestCancel() {
        cancelSessionAsFailure()
    }

    /// 工具栏「保存到本地」→ 置保存标志（主循环检查点处理；对齐 saveFlag）。
    func requestSave() {
        saveRequested = true
    }

    /// 当前输出行窗口（裁剪后的拼接行区间；小地图预览与视口框换算共用）。
    var outputRowWindow: (top: Int64, bottom: Int64) {
        guard let rows = try? algo?.outputRows() else {
            let h = Int64(algo?.state.stitchH ?? 0)
            return (0, h)
        }
        return rows
    }

    /// 读取当前输出行窗口的缩略图缓冲并转为显示空间 CGImage（小地图缩略列绘制用）：
    /// 纵向 = thumbW×rows 的纵向条；横向 = 回转后的 rows×thumbW（对齐 lc_frame_io_windows.cpp
    /// LongCaptureRebuildThumb / LongCaptureRebuildThumbDisplay 的合并与回转序）。
    func readPreviewThumbImage() -> CGImage? {
        guard let algo = algo else { return nil }
        let st = algo.state
        guard st.thumbW > 0, st.thumbH > 0 else { return nil }
        let win = outputRowWindow
        let rows = Int(win.bottom - win.top)
        guard rows > 0 else { return nil }
        let start = max(0, min(Int(win.top), st.thumbH - 1))
        let count = min(rows, st.thumbH - start)
        guard count > 0, let buf = try? algo.readThumbRows(start: start, count: count) else { return nil }
        guard horizontal else {
            return lcMakeCGImage(bgra: buf, width: st.thumbW, height: count)
        }
        // 横向模式：转置回原方向（thumbDisplay[y*h + x] = thumbMerged[x*w + y]，显示宽 = 行数）
        let w = st.thumbW
        let h = count
        var disp = [UInt32](repeating: 0, count: buf.count)
        for y in 0..<w {
            for x in 0..<h {
                disp[y * h + x] = buf[x * w + y]
            }
        }
        return lcMakeCGImage(bgra: disp, width: h, height: w)
    }
}

// MARK: - 覆盖层会话扩展（长截图入口 / ESC 退出）

extension ScreenshotOverlaySession {
    /// 进入长截图（编辑态工具栏「长截图」按钮；对齐 lc_session_windows.cpp BeginLongCapture 的
    /// 会话编排）：构造长截图会话并启动（隐藏覆盖层 → 灰蒙版 → 首帧 → 小地图/工具栏/
    /// 滚轮 tap）→ state = .longCapturing。选区过小或启动失败时会话停留在编辑态
    ///（失败路径内部已 FailFast 收束整个会话）。
    func beginLongCapture() {
        guard isRunning, state == .confirmed, longCapture == nil else { return }
        let sel = selection.standardized
        guard sel.width >= 1, sel.height >= 1 else { return }
        guard let lc = ScreenshotLongCaptureSession(overlay: self, selection: sel),
              lc.start() else {
            return
        }
        longCapture = lc
        state = .longCapturing
        longCaptureFlag.set()   // event tap 的 ESC 从此刻起路由到 longCancelFlag
    }

    /// 取消长截图（泵循环消费 ESC/兜底取消标志）：整会话按失败收束（对齐 Windows
    /// lc_session_windows.cpp 取消路径 abortFlag → LongCaptureEmitFailure → 会话 CS_Done 结束；
    /// 不回编辑态）。
    func cancelLongCaptureSession() {
        guard state == .longCapturing else { return }
        if let lc = longCapture {
            longCapture = nil
            lc.cancelSessionAsFailure()   // finishWholeSession 内部复位 state 并收口回调
        } else {
            // 防御：LC 会话缺席时兜底走覆盖层通用取消收口（正常路径不可达）
            state = .confirmed
            cancelSession()
        }
    }
}
