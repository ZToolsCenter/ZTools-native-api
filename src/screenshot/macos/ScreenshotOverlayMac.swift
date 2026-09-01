import Foundation
import AppKit
import ApplicationServices

// MARK: - 覆盖层与选区（macOS）
//
// 本文件承载会话主体（绘制扩展见 ScreenshotPaintMac.swift）：
// - 多屏覆盖层：每个 NSScreen 一个无边框透明 NSWindow，共享同一会话状态单例
// - 手动泵主循环：NSApp.nextEvent/sendEvent 驱动 AppKit 事件直至会话收束（取色器模式）
// - 选区状态机：Idle → Selecting → Confirmed → (Resizing | Moving) → Done/Cancelled
//  （对齐 Windows internal.h CaptureState）
// - 鼠标/键盘交互由覆盖层 NSView 处理；ESC 与右键取消另有 CGEventTap 兜底（失焦仍可取消）
// - 窗口吸附：CGWindowListCopyWindowInfo 枚举 + Z 序命中（对齐 Windows EnumWindowsForCapture）
// - 确认输出：底图按选区物理像素裁剪 → 物理尺寸合成（Retina 2x）→ PNG → NSPasteboard → 契约回调
//
// 坐标系约定：会话内全部状态使用 CG 全局坐标（左上原点、逻辑点、整数点），
// 与 Windows 回调的"虚拟屏绝对坐标"语义对齐；覆盖层视图 isFlipped=true，本地坐标与 CG 同向。

// MARK: - 状态机（对齐 Windows internal.h）

/// 截图状态机（对齐 Windows internal.h 的 CaptureState）。
/// 使用 Idle/Selecting/Confirmed/Resizing/Moving/Done/Cancelled、
/// Drawing（CS_Drawing：正在绘制矢量/马赛克标注）、
/// TextEditing（CS_TextEditing：正在输入文字，会话子状态——见 ScreenshotTextMac.swift）、
/// longCapturing（CS_LongCapturing：长截图滚动捕获中，覆盖层隐藏、由
/// ScreenshotLongCaptureSession 接管；取消长截图 → 回 confirmed，完成 → 整会话收束）。
enum ScreenshotCaptureState {
    case idle        // CS_Idle：等待选择（hover 窗口高亮 / 拖拽开始）
    case selecting   // CS_Selecting：正在拖拽框选
    case confirmed   // CS_Confirmed：已确认选区，可调整/拖动/微调/标注编辑（编辑工具栏）
    case resizing    // CS_Resizing：正在拖拽手柄调整选区（标准 8 手柄或圆角手柄）
    case moving      // CS_Moving：正在整体拖动选区
    case drawing     // CS_Drawing：正在绘制标注（矩形/圆/箭头/画笔/马赛克）
    case textEditing // CS_TextEditing：正在输入文字（ESC=清缓冲回确认态，Enter=提交）
    case longCapturing // CS_LongCapturing：长截图滚动捕获中（覆盖层隐藏、蒙版+小地图+工具栏接管）
    case done        // CS_Done：已确认输出
    case cancelled   // CS_Cancelled：已取消（ESC / 右键）
}

/// 选区调整手柄（对齐 Windows internal.h 的 ResizeHandle 枚举 raw 值）。
/// 8/9 为箭头端点手柄（仅箭头标注使用）。
enum ScreenshotResizeHandle: Int {
    case none = -1
    case left = 0
    case right = 1
    case top = 2
    case bottom = 3
    case topLeft = 4
    case topRight = 5
    case bottomLeft = 6
    case bottomRight = 7
    case arrowStart = 8   // 箭头起点端点手柄（仅箭头用，拖动改起点）
    case arrowEnd = 9     // 箭头终点端点手柄（仅箭头用，拖动改终点）
    case cornerTL = 10   // 选区左上角内倒角手柄（拖动改圆角半径，不改变选区矩形）
    case cornerTR = 11   // 右上角内倒角手柄
    case cornerBL = 12   // 左下角内倒角手柄
    case cornerBR = 13   // 右下角内倒角手柄

    /// 是否为选区圆角内倒角手柄（对齐 Windows IsCornerRadiusHandle）。
    var isCorner: Bool {
        return self == .cornerTL || self == .cornerTR || self == .cornerBL || self == .cornerBR
    }

    /// 四个圆角手柄的固定遍历顺序（命中/靠近探测用，对齐 Windows 数组顺序 TL/TR/BL/BR）。
    static let cornerCases: [ScreenshotResizeHandle] = [.cornerTL, .cornerTR, .cornerBL, .cornerBR]
}

/// 采样像素颜色（RGB 各 8bit；放大镜 HEX/RGB 文本与取色/回显共用；Equatable 供回显查找）。
struct ScreenshotRGB: Equatable {
    let r: UInt8
    let g: UInt8
    let b: UInt8
}

/// 线程安全的取消标志：CGEventTap 回调在 tap 自有后台线程置位，泵循环（主线程）逐拍消费。
final class ScreenshotAtomicFlag {
    private let lock = NSLock()
    private var value = false

    /// 当前是否已置位。
    var isSet: Bool {
        lock.lock()
        defer { lock.unlock() }
        return value
    }

    /// 置位（幂等）。
    func set() {
        lock.lock()
        value = true
        lock.unlock()
    }

    /// 复位（幂等）。
    func reset() {
        lock.lock()
        value = false
        lock.unlock()
    }
}

// MARK: - 窗口吸附枚举（对齐 Windows capture_windows.cpp EnumWindowsForCapture / FindWindowAtPoint）

/// 吸附候选窗口快照（对齐 Windows internal.h 的 SCWindowInfo）：rect 为 CG 全局逻辑坐标
/// （kCGWindowBounds 即 top-left 原点的全局点坐标，无需换算）。
struct ScreenshotSnapWindow {
    let rect: CGRect
    let title: String
    let windowNumber: Int
}

/// 枚举吸附候选窗口（对齐 Windows EnumWindowsForCapture 过滤语义，窗口吸附的数据源）：
/// - `.optionOnScreenOnly`：等价 IsWindowVisible（不可见窗口天然排除）
/// - `.excludeDesktopElements` + layer==0：排除桌面/壁纸与悬浮球等特殊层级（等价排除桌面窗口）
/// - 常规 activationPolicy：排除工具窗/输入法等辅助进程（等价 WS_EX_TOOLWINDOW 过滤）
/// - 排除自身进程窗口（覆盖层不入候选；对齐 Windows 排除自身窗口）
/// - 排除空标题（对齐 titleLen==0 的空壳窗口过滤）
/// - 排除过小窗口（对齐 w<50 || h<50）
/// 返回顺序保持 CG Z 序（front-to-back）；Windows 的 cloaked 幽灵窗在 macOS 无对应概念，
/// 由 layer==0 + on-screen + 常规策略过滤近似覆盖。
/// - Returns: 候选窗口数组（front-to-back）
func enumerateSnapWindows() -> [ScreenshotSnapWindow] {
    let options: CGWindowListOption = [.optionOnScreenOnly, .excludeDesktopElements]
    guard let list = CGWindowListCopyWindowInfo(options, kCGNullWindowID) as? [[String: Any]] else {
        return []
    }
    var result: [ScreenshotSnapWindow] = []
    for info in list {
        guard let layer = info[kCGWindowLayer as String] as? Int, layer == 0 else { continue }
        guard let pid = info[kCGWindowOwnerPID as String] as? Int, pid > 0, pid != getpid() else { continue }
        guard NSRunningApplication(processIdentifier: pid_t(pid))?.activationPolicy == .regular else { continue }
        guard let title = info[kCGWindowName as String] as? String, !title.isEmpty else { continue }
        guard let boundsValue = info[kCGWindowBounds as String] as? [String: Any],
              let bounds = CGRect(dictionaryRepresentation: boundsValue as CFDictionary) else { continue }
        // 过小窗口过滤（Windows w<50||h<50；零/负尺寸防御一并排除）
        if bounds.width < 50 || bounds.height < 50 { continue }
        // 取整到整数点，保证吸附矩形与选区坐标系一致（整数点运算）
        let rect = CGRect(x: bounds.minX.rounded(), y: bounds.minY.rounded(),
                          width: bounds.width.rounded(), height: bounds.height.rounded())
        let windowNumber = info[kCGWindowNumber as String] as? Int ?? 0
        result.append(ScreenshotSnapWindow(rect: rect, title: title, windowNumber: windowNumber))
    }
    return result
}

/// 查找鼠标下方的候选窗口（Windows FindWindowAtPoint 移植）：按枚举 Z 序返回首个命中项
/// 索引（即最前面的窗口）；无命中返回 -1。
/// - Parameters:
///   - windows: 候选窗口数组（enumerateSnapWindows 产物）
///   - point: 鼠标 CG 全局坐标
/// - Returns: 命中索引；无命中 -1
func findWindowAtPoint(_ windows: [ScreenshotSnapWindow], _ point: CGPoint) -> Int {
    for (i, w) in windows.enumerated() {
        if scPointInRect(point, w.rect) { return i }
    }
    return -1
}

// MARK: - 覆盖层窗口与视图

/// 覆盖层窗口：无边框透明窗默认不可成为 key window，覆写为可 key 以接收键盘
/// （方向键微调 / Enter 确认 / 无 event tap 时的 ESC 取消）。
final class OverlayScreenshotWindow: NSWindow {
    override var canBecomeKey: Bool { return true }
}

/// 覆盖层自绘视图：底图 + 蒙版 + 选区交互浮层的绘制载体，并把鼠标/键盘事件换算为
/// CG 全局坐标后转发给会话（多屏共享同一会话状态）。
final class OverlayScreenshotView: NSView {
    let session: ScreenshotOverlaySession
    /// 本窗口左上角在 CG 全局坐标系的位置（逻辑点）；本地坐标 = CG 全局坐标 - cgOrigin。
    let cgOrigin: CGPoint

    /// 本视图覆盖的 CG 全局矩形（宽高取视图 bounds，与所在屏幕一致）。
    var cgFrame: CGRect {
        return CGRect(origin: cgOrigin, size: bounds.size)
    }

    init(session: ScreenshotOverlaySession, cgOrigin: CGPoint, frame: NSRect) {
        self.session = session
        self.cgOrigin = cgOrigin
        super.init(frame: frame)
    }

    required init?(coder: NSCoder) {
        fatalError("OverlayScreenshotView is created programmatically only")
    }

    // 视图翻转：本地坐标与 CG 全局坐标同向（top-left 原点、Y 向下），换算只需平移
    override var isFlipped: Bool { return true }

    // 接收键盘事件（方向键微调 / Enter 确认 / 无 event tap 时的 ESC 取消）：
    // NSView 默认不接受 first responder，必须覆写，否则窗口成为 key 后键盘事件无人响应
    override var acceptsFirstResponder: Bool { return true }

    // 首击穿透（acceptsFirstMouse）：会话由热键在其他应用前台时触发，macOS 14+ 协作式
    // 激活可能失败——覆盖层窗口照常显示（orderFrontRegardless 不依赖激活）但 App 未激活。
    // 该状态下 AppKit 把非 key 窗口上的首次 mouseDown 当"激活点击"吞掉（只用于激活应用，
    // 不投递给视图），本次拖拽全程的 mouseDragged/mouseUp 因会话仍处 Idle 全部无效 →
    // 表现为"进入截图后无法拖拽选区"（偶发：会话启动时激活成功则一切正常）。
    // 覆写后首击穿透直达本视图，实测 macOS 15 未激活状态下 mouseDown/mouseUp 正常送达，
    // 拖拽选区恢复；键盘（方向键/Enter）仍依赖激活成功，失败时由 CGEventTap 的
    // ESC/右键兜底，与修复前行为一致。
    override func acceptsFirstMouse(for event: NSEvent?) -> Bool { return true }

    /// 覆盖层绘制（脏区局部重绘；底图按脏区裁剪，禁止整图重采样）
    override func draw(_ dirtyRect: NSRect) {
        guard let cgContext = NSGraphicsContext.current?.cgContext else { return }
        session.paint(context: cgContext, view: self, dirtyLocal: dirtyRect)
    }

    // ---- 鼠标/键盘事件 → 会话 ----

    /// 事件位置 → CG 全局逻辑坐标（整数点，对齐 Windows 的 int 鼠标坐标）。
    private func cgPoint(from event: NSEvent) -> CGPoint {
        let local = convert(event.locationInWindow, from: nil)
        return CGPoint(x: (local.x + cgOrigin.x).rounded(), y: (local.y + cgOrigin.y).rounded())
    }

    /// 左键按下：Idle 开始框选 / Confirmed 双击确认·手柄调整·整体拖动
    override func mouseDown(with event: NSEvent) {
        session.handleMouseDown(cgPoint(from: event), clickCount: event.clickCount)
    }

    /// 左键拖动：Selecting 更新终点 / Resizing 调整选区或圆角 / Moving 整体平移
    override func mouseDragged(with event: NSEvent) {
        session.handleMouseDragged(cgPoint(from: event))
    }

    /// 左键抬起：Selecting 收束（吸附退化链/规范化/autoConfirm）/ Resizing 补足最小尺寸 / Moving 收束
    override func mouseUp(with event: NSEvent) {
        session.handleMouseUp(cgPoint(from: event))
    }

    /// 右键：取消会话（对齐 Windows WM_RBUTTONDOWN；event tap 活跃时事件已被 tap 吞掉，
    /// 本 handler 是无辅助功能权限时（tap 创建失败）的兜底取消路径）
    override func rightMouseDown(with event: NSEvent) {
        session.cancelSession()
    }

    /// 鼠标移动（无按键）：Idle hover 高亮/取色 / Confirmed 圆角手柄靠近探测
    override func mouseMoved(with event: NSEvent) {
        session.handleMouseMoved(cgPoint(from: event))
    }

    /// 光标更新：按状态与命中切换系统光标（十字/resize/手型/箭头）
    override func cursorUpdate(with event: NSEvent) {
        session.updateCursor()
    }

    /// 键盘：方向键微调 / Enter 确认 / ESC 取消 / 文字编辑键系（event tap 兜底缺位时的直接
    /// 路径）。文字编辑态优先经 inputContext 路由 NSTextInputClient（IME 组词/上屏）。
    override func keyDown(with event: NSEvent) {
        session.handleKeyDown(event, textInputContext: inputContext)
    }
}

// MARK: - ESC/右键 CGEventTap 兜底（取色器模式）

/// 当前活跃的覆盖层 event tap（CGEventTap 回调为 C 函数指针不能捕获上下文，
/// 以模块级弱持有桥接；会话收口时置 nil）。
private var overlayTapCurrent: ScreenshotOverlayEventTap?

/// CGEventTap 回调（C 函数指针兼容；无捕获）：转发给当前活跃 tap 实例处理。
private func screenshotOverlayEventTapCallback(
    _ proxy: CGEventTapProxy, _ type: CGEventType, _ event: CGEvent, _ userInfo: UnsafeMutableRawPointer?
) -> Unmanaged<CGEvent>? {
    guard let current = overlayTapCurrent else {
        return Unmanaged.passUnretained(event)
    }
    return current.handleEvent(type: type, event: event)
}

/// 覆盖层 CGEventTap 兜底（对齐 Windows 失焦后 GetAsyncKeyState 轮询兜底，矩阵 #49）：
/// 会话期间拦截 ESC keyDown 与右键按下并置取消标志（泵循环 ≤16ms 内消费收束），
/// 解决覆盖层失焦时 ESC/右键仍可取消。文字编辑态例外：ESC 放行给覆盖层视图
///（NSTextInputClient 键系路径清缓冲回确认态——不是取消截图）。
/// 长截图态例外：ESC 置 longCancelFlag 取消长截图（整会话 {success:false} 收束，
/// 对齐 Windows RunLongCapture 主循环的 GetAsyncKeyState(VK_ESCAPE) 轮询分支）；右键放行
///（Windows CS_LongCapturing 期间 OnRButtonDown 直接忽略，矩阵语义一致）。
/// 回调运行在 tap 自有后台线程，仅读原子标志与吞事件，绝不触碰 NSWindow/NSView。
/// 启动前置检查见会话 start()（无辅助功能权限时打印明确错误）。
final class ScreenshotOverlayEventTap {
    /// 共享取消标志（与会话交换的唯一通道）。
    let cancelFlag: ScreenshotAtomicFlag
    /// 文字编辑态标志（编辑态 ESC 放行不取消；由泵循环/状态迁移在主线程同步）。
    let textEditingFlag: ScreenshotAtomicFlag
    /// 保存对话框模态标志（模态期间 ESC/右键放行给 NSSavePanel 自消费，不触发会话取消；
    /// 主线程保存流在 runModal 前后同步置位/复位——对齐 Windows GetSaveFileNameW 模态
    /// 循环期间消息不达覆盖层 WndProc 的语义）。
    let saveModalFlag: ScreenshotAtomicFlag
    /// 长截图态标志（置位期间 ESC → longCancelFlag 而非取消整个会话；主线程同步）。
    let longCaptureFlag: ScreenshotAtomicFlag
    /// 长截图取消标志（长截图态下 ESC 置位；泵循环消费 → 取消长截图，整会话失败收束）。
    let longCancelFlag: ScreenshotAtomicFlag
    private var tap: CFMachPort?
    private var runLoopSource: CFRunLoopSource?
    private var runLoop: CFRunLoop?
    private var stopped = false

    init(cancelFlag: ScreenshotAtomicFlag, textEditingFlag: ScreenshotAtomicFlag,
         saveModalFlag: ScreenshotAtomicFlag,
         longCaptureFlag: ScreenshotAtomicFlag = ScreenshotAtomicFlag(),
         longCancelFlag: ScreenshotAtomicFlag = ScreenshotAtomicFlag()) {
        self.cancelFlag = cancelFlag
        self.textEditingFlag = textEditingFlag
        self.saveModalFlag = saveModalFlag
        self.longCaptureFlag = longCaptureFlag
        self.longCancelFlag = longCancelFlag
    }

    /// 在后台线程创建 event tap 并运行其 RunLoop（照抄取色器启动模式：
    /// .cgSessionEventTap + headInsert + .defaultTap 拦截模式 + 被系统禁用时自动重启）。
    func start() {
        DispatchQueue.global(qos: .userInteractive).async { [weak self] in
            guard let self = self else { return }
            let eventMask: CGEventMask = (1 << CGEventType.keyDown.rawValue)
                | (1 << CGEventType.rightMouseDown.rawValue)
            guard let tap = CGEvent.tapCreate(
                tap: .cgSessionEventTap,
                place: .headInsertEventTap,
                options: .defaultTap,
                eventsOfInterest: eventMask,
                callback: screenshotOverlayEventTapCallback,
                userInfo: nil
            ) else {
                print("Error: Failed to create screenshot overlay event tap. Check accessibility permissions.")
                return
            }
            guard let source = CFMachPortCreateRunLoopSource(kCFAllocatorDefault, tap, 0) else {
                print("Error: Failed to create run loop source for screenshot overlay event tap")
                CFMachPortInvalidate(tap)
                return
            }
            self.tap = tap
            self.runLoopSource = source
            self.runLoop = CFRunLoopGetCurrent()
            if self.stopped {
                // 会话在 tap 就绪前已收口：就地清理，避免泄漏（stop 已跑过空清理）
                self.stop()
                return
            }
            // source 必须挂进当前 RunLoop，tap 的 mach port 才会被调度读取——缺失时
            // tap 创建并 enable 成功也永远收不到回调（失焦 ESC/右键兜底与长截图态
            // ESC 取消静默失效；对齐 ZToolsNative.swift 取色器/鼠标监听的标准启动序）
            CFRunLoopAddSource(CFRunLoopGetCurrent(), source, .commonModes)
            overlayTapCurrent = self
            CGEvent.tapEnable(tap: tap, enable: true)
            CFRunLoopRun()
        }
    }

    /// 停止并释放 event tap（主线程会话收口时调用；CFRunLoopStop 线程安全）。
    func stop() {
        stopped = true
        overlayTapCurrent = nil
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

    /// tap 回调主体：ESC keyDown（虚拟键码 53）与右键按下 → 置取消标志并吞掉事件；
    /// 保存对话框模态期间按键全部放行（NSSavePanel 自消费 ESC=取消对话框）；
    /// tap 被系统超时禁用（0xFFFFFFFE/0xFFFFFFFF）时重新启用（取色器同款处理）。
    /// - Parameters:
    ///   - type: 事件类型
    ///   - event: 原始事件
    /// - Returns: 放行事件；拦截时返回 nil
    func handleEvent(type: CGEventType, event: CGEvent) -> Unmanaged<CGEvent>? {
        if type.rawValue == 0xFFFFFFFE || type.rawValue == 0xFFFFFFFF {
            if let tap = tap {
                CGEvent.tapEnable(tap: tap, enable: true)
            }
            return Unmanaged.passUnretained(event)
        }
        // 保存对话框模态期间：ESC/右键放行（面板自带 ESC=取消对话框语义；Windows
        // GetSaveFileNameW 模态循环期间消息不达覆盖层 WndProc 的等价处理）
        if saveModalFlag.isSet {
            return Unmanaged.passUnretained(event)
        }
        switch type {
        case .keyDown:
            if event.getIntegerValueField(.keyboardEventKeycode) == 53 {   // ESC
                if longCaptureFlag.isSet {
                    // 长截图态：ESC = 取消长截图（整会话 {success:false} 收束，对齐
                    // Windows RunLongCapture 主循环 GetAsyncKeyState(VK_ESCAPE) 检查点
                    // → abortFlag → LongCaptureEmitFailure；吞掉事件避免同时触达选区下
                    // 的目标应用——macOS 蒙版整窗点击穿透、无前台窗口承接）
                    longCancelFlag.set()
                    return nil
                }
                if textEditingFlag.isSet {
                    // 文字编辑态：放行（覆盖层视图 keyDown → 清缓冲回确认态，非取消截图）
                    return Unmanaged.passUnretained(event)
                }
                cancelFlag.set()
                return nil
            }
            return Unmanaged.passUnretained(event)
        case .rightMouseDown:
            // 长截图态：右键忽略（Windows overlay_input_windows.cpp CS_LongCapturing
            // 分支 return 0；放行交还系统，保持底层应用右键菜单等常规行为）
            if longCaptureFlag.isSet {
                return Unmanaged.passUnretained(event)
            }
            // 右键取消（对齐 Windows WM_RBUTTONDOWN）
            cancelFlag.set()
            return nil
        default:
            return Unmanaged.passUnretained(event)
        }
    }
}

// MARK: - 覆盖层会话（状态机 + 输入 + 生命周期 + 输出）

/// 覆盖层选区会话主体。每个 NSScreen 一个覆盖层窗口共享本实例；选区用 CG 全局
/// 逻辑坐标跨屏统一表达，事件按窗口换算，绘制把 CG 坐标平移为窗口本地坐标。
/// 生命周期：runOverlayCaptureSession 创建 → start()（窗口 + event tap）→ runEventPump()
/// 手动泵直至 confirmSelection/cancelSession 收束 → finish() 完整清理并回调恰好一次。
final class ScreenshotOverlaySession {
    // ---- 会话配置与基础设施（以下成员供 ScreenshotPaintMac.swift 的绘制扩展跨文件访问）----
    let options: ScreenshotSessionOptions
    let baseFrame: CapturedFrame            // 常驻底图（物理像素 CGImage，禁止整图重采样）
    let virtualBounds: CGRect               // 虚拟屏并集（CG 全局逻辑坐标）
    var state: ScreenshotCaptureState = .idle   // internal setter：标注绘制/交互由同模块扩展写入

    // ---- 覆盖层窗口（每 NSScreen 一个）----
    private var windows: [NSWindow] = []
    private var views: [OverlayScreenshotView] = []
    private var eventTap: ScreenshotOverlayEventTap?

    // ---- 选区状态（CG 全局逻辑坐标，整数点对齐 Windows int 坐标语义）----
    private let callback: ScreenshotResultCallback
    private var hasFinished = false
    let cancelFlag = ScreenshotAtomicFlag() // event tap 兜底取消标志

    var mouse: CGPoint = .zero                          // 最近鼠标位置
    var currentColor = ScreenshotRGB(r: 0, g: 0, b: 0)  // 放大镜采样色（Idle/Selecting 取鼠标处；Resizing 取手柄锚点）
    private var snapWindows: [ScreenshotSnapWindow] = []  // 吸附候选窗口（会话开始时枚举一次）
    private var hoveredWindowIndex = -1                 // Idle hover 命中的候选窗口（-1 = 无）

    // 拖拽创建选区
    private var dragStartPoint: CGPoint = .zero
    private var dragCurrentPoint: CGPoint = .zero

    // 确认态（对齐 CaptureContext.selection / resizeHandle / selectionCornerRadius / kbDX/kbDY）
    var selection = CGRect.null
    var selectionCornerRadius: CGFloat = 0              // 选区圆角半径（0 = 直角；上限 min(w,h)/2）
    var resizeHandle: ScreenshotResizeHandle = .none    // CS_Resizing 活动手柄
    var hoveredCornerHandle: ScreenshotResizeHandle = .none  // 确认态"靠近"的圆角手柄
    private var dragStartSelection = CGRect.null        // 按下时选区快照（resize/move 基准）
    private var handleDragStartPoint: CGPoint = .zero   // 按下时鼠标位置
    private var cornerDragStartRadius: CGFloat = 0      // 圆角手柄拖拽起始半径
    private var kbDX: CGFloat = 0                       // 方向键微调累计 X（Resizing 时叠加鼠标位移）
    private var kbDY: CGFloat = 0                       // 方向键微调累计 Y

    // ---- 矢量标注（对齐 CaptureContext 的标注字段，坐标为绝对 CG 全局坐标）----
    var annotations: [ScreenshotAnnotation] = []        // 已提交标注
    var undoStack: [[ScreenshotAnnotation]] = []        // 撤销快照栈（队首=最老，深度 SC.undoMaxDepth）
    var redoStack: [[ScreenshotAnnotation]] = []        // 重做快照栈
    var curDrawing = ScreenshotAnnotation.empty         // .drawing 中正在绘制的标注
    var hasCurDrawing = false                           // curDrawing 是否有效
    var selectedAnnotation = -1                         // 选中的标注索引（-1=无，持久保持）
    var draggingAnnotation = -1                         // 正在拖动的标注索引（-1=无）
    var resizingAnnotation = -1                         // 正在缩放的标注索引（-1=无）
    var annotationResizeHandle: ScreenshotResizeHandle = .none  // 标注缩放活动手柄
    var annotationDragStartPoint: CGPoint = .zero       // 按下时鼠标位置（拖拽/缩放共用）
    var dragStartAnnotation = ScreenshotAnnotation.empty // 按下时标注快照（还原+平移基准）
    var annotationResizeStartBox = CGRect.zero          // 按下时包围盒（缩放基准）
    var annotationOpHistoryPushed = false               // 本次拖拽是否已入历史（首次位移才入栈）
    /// 上帧标注操作（拖拽/缩放/绘制）的包围盒（.null = 无缓存）。标注拖拽/绘制热路径
    /// 按「上帧盒 ∪ 本帧盒」局部失效（对齐 Windows InvalidateAnnotationOp 的
    /// lastAnnotationBox ∪ curBox 语义，annotations_windows.cpp L842-853），逐帧链式
    /// 覆盖上一位置防残影；进入/退出拖拽态时复位（性能审计：整窗失效局部化）。
    var lastAnnotationOpBox = CGRect.null
    var activeTool: ScreenshotToolButton? = nil         // 当前激活工具（对齐 activeTool；确认态默认 drag）
    var drawColorIdx = SC.defaultColorIdx               // 当前选中颜色索引（子菜单）
    var drawThickIdx = SC.defaultThickIdx               // 当前选中粗细索引（子菜单）

    // ---- 文字输入（对齐 CaptureContext 的 CS_TextEditing 字段组，坐标为 CG 全局）----
    var textBuf = ""                                    // 正在输入的文字缓冲（UTF-16 单元语义）
    var textAnchorX: CGFloat = 0                        // 文字锚点（绝对 CG 全局坐标）
    var textAnchorY: CGFloat = 0
    var textCaretPos = 0                                // 插入符位置（UTF-16 单元偏移）
    var textCaretVisible = true                         // 光标是否可见（500ms 闪烁控制）
    var textCaretLastBlink: TimeInterval = 0            // 上次闪烁切换时刻（单调时钟秒）
    var textSelStart = -1                               // 文字选择起始（-1 = 无选择）
    var textSelEnd = -1                                 // 文字选择结束
    var textDraggingSelection = false                   // 是否正在拖动选择文字
    var textMarkedRange = NSRange(location: NSNotFound, length: 0)  // IME 组词区间
    var hoveredAnnotation = -1                          // 悬停非文字标注索引（-1 = 无；字段对齐 Windows，
                                                        //  视觉高亮已并入实时命中，暂仅记录）
    var hoveredTextAnnotation = -1                      // 悬停文字标注索引（-1 = 无）
    var selectedTextAnnotation = -1                     // 已选中文字标注索引（-1 = 无，持久保持）
    var draggingTextAnnotation = -1                     // 正在拖动的文字标注索引（-1 = 无）
    var textDragStartPoint: CGPoint = .zero             // 文字拖动按下点（CG 全局）
    var textDragStartAnchor: CGPoint = .zero            // 按下时标注锚点快照
    var fontSizeIdx = SC.defaultFontIdx                 // 当前选中字号索引（文字工具子菜单）
    /// 上帧插入符矩形（CG 全局坐标；isNull = 无缓存），供闪烁/键系局部失效（对齐 lastCaretRect）
    var lastCaretRect = CGRect.null
    /// 编辑态 ESC 放行标志（event tap 后台线程读取；主线程在状态迁移/泵循环同步）
    let textEditingFlag = ScreenshotAtomicFlag()
    /// 保存对话框模态标志（模态期间 event tap 放行 ESC/右键给保存面板；保存流在
    /// runModal 前后置位/复位，见 ScreenshotOutputMac.swift 的 saveSelectionToFile）
    let saveModalFlag = ScreenshotAtomicFlag()
    /// 长截图态标志（event tap 据此把 ESC 路由到 longCancelFlag；泵循环同步置位/复位）
    let longCaptureFlag = ScreenshotAtomicFlag()
    /// 长截图取消标志（长截图态下 ESC 由 event tap 置位；泵循环消费 → 取消长截图整会话失败收束）
    let longCancelFlag = ScreenshotAtomicFlag()
    /// 长截图滚动捕获会话（进入长截图时创建，收束/取消后置 nil。
    /// 定义见 ScreenshotLongCaptureMac.swift）
    var longCapture: ScreenshotLongCaptureSession?

    // ---- 马赛克（对齐 CaptureContext 的马赛克字段组）----
    var mosaicSizeIdx = SC.defaultMosaicIdx             // 当前选中块大小索引
    var mosaicRadiusIdx = SC.defaultMosaicRadiusIdx     // 当前选中涂抹半径索引
    var mosaicRectMode = false                          // true=框选区域模式；false=涂抹模式
    /// 会话内 mosaicBase 缓存（整屏按当前块大小预像素化；块大小变化才重建）
    var mosaicBaseCache: ScreenshotMosaicBase? = nil
    /// 工具栏/子菜单/tooltip 浮层族控制器（生命周期挂会话 start/finish；lazy 便于引用 self）
    lazy var toolbar = ScreenshotToolbarController(session: self)

    // 脏区追踪（上帧浮层并集；局部失效与绘制共用几何，等价 Windows last*Rect 语义）
    private var lastIdleOverlayRect: CGRect?

    // 保存对话框模态期间被降级的窗口层级快照（恢复用，见 duckOverlayLevelsForSaveModal）
    private var savedOverlayLevels: [ObjectIdentifier: NSWindow.Level] = [:]

    init(options: ScreenshotSessionOptions, callback: ScreenshotResultCallback,
         baseFrame: CapturedFrame, virtualBounds: CGRect) {
        self.options = options
        self.callback = callback
        self.baseFrame = baseFrame
        self.virtualBounds = virtualBounds
        // 窗口吸附枚举（Windows 在会话开始 EnumWindowsForCapture 一次；此时覆盖层尚未创建，
        // 候选天然不含自身窗口）
        self.snapWindows = enumerateSnapWindows()
    }

    /// 会话是否仍在进行（finish 后置 false；所有事件入口以此守卫防收束后残余事件写入状态）。
    var isRunning: Bool { return !hasFinished }

    // MARK: 生命周期

    /// 创建多屏覆盖层窗口并显示，启动 ESC/右键 event tap 兜底。
    /// - Returns: true 会话就绪（随后 runEventPump）；false 初始化失败（内部已 FailFast 回调并复位标志）
    func start() -> Bool {
        guard setupOverlayWindows() else {
            finish("{\"success\":false,\"error\":\"failed to create overlay windows\"}")
            return false
        }
        // CGEventTap 依赖辅助功能权限。启动时显式检查并打印明确错误；
        // 无权限时 tap 创建失败，ESC/右键兜底自动降级为覆盖层自身 keyDown/rightMouseDown 处理。
        let trusted = AXIsProcessTrustedWithOptions(
            [kAXTrustedCheckOptionPrompt.takeUnretainedValue() as String: false] as CFDictionary)
        if !trusted {
            print("Error: Accessibility permission not granted - screenshot overlay ESC/right-click fallback (CGEventTap) unavailable")
        }
        eventTap = ScreenshotOverlayEventTap(cancelFlag: cancelFlag, textEditingFlag: textEditingFlag,
                                             saveModalFlag: saveModalFlag,
                                             longCaptureFlag: longCaptureFlag,
                                             longCancelFlag: longCancelFlag)
        eventTap?.start()
        return true
    }

    /// 手动泵主循环（取色器模式扩展）：Node 主线程不跑 NSRunLoop，
    /// 会话期间由本循环驱动 AppKit 事件分发（窗口绘制/鼠标/键盘），直至确认或取消收束。
    /// nextEvent 带超时返回，保证泵循环能逐拍消费 CGEventTap 兜底取消标志
    ///（对齐 Windows 空闲循环的 GetAsyncKeyState 轮询节奏）。
    ///
    /// 功耗审计结论：16ms 超时是有意的事件等待节奏而非忙等——无事件时线程阻塞在
    /// nextEvent 上（空闲唤醒率上限 62.5/s），且泵循环仅在有截图会话期间运行（本就阻塞
    /// JS 主线程的瞬态交互期）。不能拉长超时换功耗：cancelFlag/event tap 兜底取消、
    /// 插入符闪烁、长截图采样轮都依赖 ≤16ms 的逐拍消费（event tap 注释的契约），
    /// 拉长即违反响应性兜底；取色器用阻塞式 nextEvent(until: nil) 是其无轮询任务的
    /// 特例，不适用本会话。
    func runEventPump() {
        while isRunning {
            autoreleasepool {
                if let event = NSApp.nextEvent(matching: .any, until: Date(timeIntervalSinceNow: 0.016),
                                               inMode: .default, dequeue: true) {
                    NSApp.sendEvent(event)
                }
                pumpTick()
            }
        }
    }

    /// 泵循环逐拍任务：消费兜底取消标志 + 工具栏浮层族的状态/位置同步与 tooltip 轮询
    ///（tooltip 定时进 pumpTick，走泵循环定时任务位）+ 编辑态插入符 500ms 闪烁
    ///（对齐 Windows 空闲循环 GetTickCount 分支）+ 编辑态标志同步（event tap ESC 放行）。
    private func pumpTick() {
        guard isRunning else { return }
        // 长截图态：泵循环驱动长截图采样/autoScroll/浮层刷新（ScreenshotLongCaptureMac.swift
        // 的 lcTick；对齐 Windows RunLongCapture 在覆盖层窗口过程内自泵消息的语义）。
        // ESC（event tap → longCancelFlag）取消长截图：整会话 {success:false} 收束
        //（对齐 lc_session_windows.cpp abortFlag → LongCaptureEmitFailure）；cancelFlag 兜底同义。
        if state == .longCapturing {
            if cancelFlag.isSet || longCancelFlag.isSet {
                cancelFlag.reset()
                longCancelFlag.reset()
                cancelLongCaptureSession()
            } else {
                longCapture?.lcTick()
            }
            return
        }
        longCancelFlag.reset()
        if cancelFlag.isSet {
            cancelFlag.reset()
            cancelSession()
            return
        }
        // 文字编辑态标志同步（≥16ms 延迟内 event tap 对 ESC 放行）
        if state == .textEditing {
            textEditingFlag.set()
        } else {
            textEditingFlag.reset()
        }
        // 编辑态插入符 500ms 闪烁（局部失效光标区域）
        tickTextCaret(now: ProcessInfo.processInfo.systemUptime)
        // 工具栏可见性随状态同步（对齐 OnPaint：Confirmed/Moving/Drawing/TextEditing 显示，
        // Resizing 隐藏）；!toolbarPlaced 时随选区自动重算位置（toolbarPlaced 语义）。
        let toolbarVisible = (state == .confirmed || state == .moving || state == .drawing
                              || state == .textEditing)
        toolbar.syncVisibility(toolbarVisible)
        if toolbarVisible {
            toolbar.syncPlacement()
        }
        // hover 高亮 + tooltip 500ms 停顿轮询 + 浮层区域光标接管
        toolbar.tick(now: ProcessInfo.processInfo.systemUptime)
    }

    /// 创建覆盖层窗口（每 NSScreen 一个；规格见 ScreenshotToolbarMac.swift 文件头「窗口规格」）并做初始 hover/取色。
    /// - Returns: 创建成功与否（无屏幕等极端环境返回 false）
    private func setupOverlayWindows() -> Bool {
        let screens = NSScreen.screens
        guard !screens.isEmpty else { return false }
        let initialMouse = ScreenshotGeometry.cgPoint(fromNS: NSEvent.mouseLocation)
        mouse = initialMouse
        currentColor = samplePixelColor(at: initialMouse) ?? ScreenshotRGB(r: 0, g: 0, b: 0)
        hoveredWindowIndex = findWindowAtPoint(snapWindows, initialMouse)

        for screen in screens {
            let cgFrame = ScreenshotGeometry.cgFrame(of: screen)
            let window = OverlayScreenshotWindow(
                contentRect: screen.frame, styleMask: .borderless, backing: .buffered, defer: false)
            window.level = NSWindow.Level(rawValue: NSWindow.Level.screenSaver.rawValue + 1)
            window.isOpaque = false
            window.backgroundColor = .clear
            window.hasShadow = false
            window.collectionBehavior = [.canJoinAllSpaces, .fullScreenAuxiliary]
            window.isReleasedWhenClosed = false
            window.acceptsMouseMovedEvents = true
            // 不设 ignoresMouseEvents：覆盖层需接收拖拽
            let view = OverlayScreenshotView(
                session: self, cgOrigin: cgFrame.origin,
                frame: NSRect(origin: .zero, size: cgFrame.size))
            window.contentView = view
            // 键盘事件入口：显式指定初始 first responder，保证窗口成为 key 后
            // keyDown（方向键/ESC/文字编辑键系）直达覆盖层视图（NSTextInputClient 宿主）
            window.initialFirstResponder = view
            windows.append(window)
            views.append(view)
        }
        for w in windows {
            w.orderFrontRegardless()
        }
        // 鼠标所在屏的窗口做 key（键盘事件入口；点击其他屏窗口时 AppKit 自动换 key）
        if let keyIndex = views.firstIndex(where: { $0.cgFrame.contains(initialMouse) }) {
            windows[keyIndex].makeKeyAndOrderFront(nil)
        }
        NSApp.activate(ignoringOtherApps: true)
        invalidateAll()
        return true
    }

    /// 会话统一出口：停 event tap → 销毁长截图会话 → 销毁工具栏浮层 → 销毁窗口
    /// → 回调（恰好一次）→ 复位重入标志（会话结束顺序；对齐 Windows 捕获线程末尾清理）。
    /// （internal：ScreenshotOutputMac.swift 的保存流 saveSelectionToFile 复用。）
    func finish(_ payload: String) {
        guard !hasFinished else { return }
        hasFinished = true
        NSCursor.arrow.set()
        // 长截图会话随会话收束销毁（abort/save/finish 收束路径已在 LC 侧先行清理，
        // 此处兜底防残余窗口/滚轮 tap 泄漏）
        longCapture?.lcTeardown()
        longCapture = nil
        eventTap?.stop()
        eventTap = nil
        toolbar.destroy()
        ScreenshotMosaicCursors.reset()   // 圆环光标随会话生命周期释放
        mosaicBaseCache = nil             // 马赛克 base 随会话释放
        for window in windows {
            window.orderOut(nil)
            window.contentView = nil
        }
        windows.removeAll()
        views.removeAll()
        payload.withCString { cStr in
            callback(cStr)
        }
        screenshotStateLock.lock()
        screenshotSessionActive = false
        screenshotStateLock.unlock()
    }

    /// 构造失败回调 JSON（macOS 契约新增可选 error 字段，不改既有字段）。
    /// （internal：ScreenshotOutputMac.swift 的保存流复用。）
    func failurePayload(_ error: String) -> String {
        return "{\"success\":false,\"error\":\"\(error)\"}"
    }

    /// 覆盖层与工具栏浮层族窗口层级临时降级/恢复（保存对话框弹出前/关闭后调用）。
    /// 对齐 Windows PromptSaveFilePath：弹出前 SetWindowPos(HWND_NOTOPMOST) 让保存对话框
    /// 显示在最上层、关闭后恢复 HWND_TOPMOST（output_windows.cpp L608-609/L632-636/L654-657）。
    /// macOS 方案：覆盖层与工具栏浮层族在 screenSaver+n 层（约 1001+），而模态保存面板在
    /// NSModalPanelWindowLevel（8），不降级会被完全遮挡；统一临时下调到 SC.saveModalDuckLevel
    /// （模态面板之下一档、普通应用窗口之上）——用户可见行为最接近 Windows：覆盖层暗化蒙版
    /// 与工具栏保持可见，仅保存对话框浮于其上（选 level 下调而非 orderOut，避免桌面闪烁）。
    /// - Parameter lowered: true 降级（记录原层级）；false 恢复（会话已收口时窗口已销毁，安全 no-op）
    func duckOverlayLevelsForSaveModal(_ lowered: Bool) {
        if lowered {
            savedOverlayLevels.removeAll()
            for window in windows {
                savedOverlayLevels[ObjectIdentifier(window)] = window.level
                window.level = SC.saveModalDuckLevel
            }
        } else {
            for window in windows {
                if let level = savedOverlayLevels[ObjectIdentifier(window)] {
                    window.level = level
                }
            }
            savedOverlayLevels.removeAll()
        }
        // 工具栏/子菜单/tooltip 浮层族同步降级/恢复（Windows 工具栏画在覆盖层窗口内，
        // 摘 TOPMOST 一并生效；macOS 为独立窗口需同步处理）
        toolbar.setPanelLevelBelowModal(lowered)
    }

    // MARK: 长截图窗口切换（实现见 ScreenshotLongCaptureMac.swift）

    /// 隐藏全部覆盖层窗口（进入长截图时调用；会话与窗口对象保留至会话收束统一销毁）。
    /// 对齐 Windows EnterLongCapture 的 ShowWindow(overlayHwnd, SW_HIDE)——覆盖层被独立
    /// 灰蒙版 + 小地图 + 工具栏接管（lc_session_windows.cpp BeginLongCapture 注释）。
    func hideOverlayWindowsForLongCapture() {
        for window in windows {
            window.orderOut(nil)
        }
    }

    // MARK: 取消与确认

    /// 取消会话（ESC / 右键；对齐 Windows OnKeyDown VK_ESCAPE 与 WM_RBUTTONDOWN）：
    /// 回调 {success:false}（取消语义无坐标/图像字段，与 Windows 取消回调一致）。
    func cancelSession() {
        guard isRunning else { return }
        state = .cancelled
        finish("{\"success\":false}")
    }

    /// 确认输出（对齐 Windows 工具栏确定/Enter/双击/autoConfirm 松手共用的 ExtractRegionResult
    /// 路径）：底图按选区物理像素裁剪 → 物理尺寸合成（Retina 2x，不再缩回逻辑尺寸）→ 合成
    /// 矢量标注 → 圆角蒙版（radius>0）→ PNG 编码 → NSPasteboard 写入 → 回调
    /// {success:true, x, y, x2, y2, width, height, base64}（坐标契约仍为 CG 全局逻辑坐标）。
    /// radius>0 走圆角透明导出（alpha 通道 PNG 写剪贴板，
    /// 对齐 Windows BuildRoundedArgbFinal 语义）；任一环节失败均按 {success:false, error:...}
    /// 收口（失败不输出黑图）。编码/编码产物构建统一走 ScreenshotOutputMac.swift 的
    /// buildFinalPngOutput（与保存路径共用，长截图复用）。
    /// （工具栏确定按钮经 handleToolbarButton 调用，故为 internal。）
    func confirmSelection() {
        guard isRunning else { return }
        let sel = selection.standardized
        guard sel.width >= 1, sel.height >= 1 else {
            cancelSession()
            return
        }

        // 物理像素裁剪（对齐 ExtractRegionResult：物理 = (逻辑 - 虚拟屏原点) * dpiScale，钳制在位图内）
        guard let cropped = cropSelectionPhysical(sel) else {
            finish(failurePayload("failed to crop screenshot region"))
            return
        }

        // 统一输出：物理尺寸合成（Retina 2x，底图 1:1）→ 合成标注 → 圆角蒙版（radius>0）
        // → 单次 PNG 编码（同时产出 base64 与文件字节；半径取会话当前选区圆角，0 = 直角）
        guard let output = buildFinalPngOutput(cropped: cropped, sel: sel,
                                               cornerRadius: selectionCornerRadius) else {
            finish(failurePayload("failed to encode screenshot"))
            return
        }
        guard writePngToPasteboard(output.pngData) else {
            finish(failurePayload("failed to copy screenshot to clipboard"))
            return
        }

        state = .done
        finish(scSuccessPayloadJSON(sel: sel, base64: output.base64))
    }

    // MARK: 脏区失效（setNeedsDisplay 局部失效对齐 Windows InvalidateRect）

    /// 局部失效：CG 全局矩形换算到每个覆盖层视图本地后 setNeedsDisplay(rect)。
    /// rect 传 nil 时全屏失效（等价 Windows InvalidateRect(NULL)），蒙版边界变化的
    /// Selecting/Resizing/Moving 全程使用。
    func invalidate(_ rect: CGRect?) {
        for view in views {
            let local: CGRect
            if let rect = rect {
                let clipped = rect.intersection(view.cgFrame)
                if clipped.isNull || clipped.width <= 0 || clipped.height <= 0 { continue }
                local = clipped.offsetBy(dx: -view.cgOrigin.x, dy: -view.cgOrigin.y)
            } else {
                local = view.bounds
            }
            view.setNeedsDisplay(local)
        }
    }

    /// 全屏失效（所有覆盖层视图整体重绘）。
    func invalidateAll() {
        invalidate(nil)
    }

    /// 标注拖拽/缩放热路径的局部失效（overlay_input_windows.cpp InvalidateAnnotationOp
    /// 调用点等价，性能审计）：脏区 = 上帧标注盒（lastAnnotationOpBox）∪ 本帧盒，
    /// 外扩 handleMargin（覆盖选中手柄、描边与抗锯齿）。调用后把本帧盒写回
    /// lastAnnotationOpBox 形成逐帧链式脏区，拖拽轨迹上一位置随之清除不留残影
    ///（AppKit 合并同帧内多次 setNeedsDisplay，语义与 Windows WM_PAINT 缓存更新等价）。
    /// - Parameter newBox: 本帧标注几何包围盒（未外扩，CG 全局坐标）
    func invalidateAnnotationOpLocal(newBox: CGRect) {
        let dirty = scUnionRect(lastAnnotationOpBox.isNull ? nil : lastAnnotationOpBox, newBox)
            ?? newBox
        lastAnnotationOpBox = newBox
        invalidate(scInflate(dirty, SC.handleMargin))
    }

    // MARK: 底图采样（放大镜/取色共用；对齐 GetPixelColorFromBitmap 与 DrawInfoPanel 源区钳制）

    /// 从常驻底图 CGImage 裁剪以 focus 为中心的采样区（物理像素，四边钳制在位图内）。
    /// 左/上越界把起点钳回 0，右/下收窄采样宽高（对齐 DrawInfoPanel 的源区钳制语义）。
    /// - Parameters:
    ///   - focus: 采样焦点（鼠标或活动手柄锚点，CG 全局坐标）
    ///   - srcLogicalW/srcLogicalH: 采样区逻辑尺寸（绘制前除以放大倍数）
    /// - Returns: 裁剪出的物理像素 CGImage；采样区非正（焦点越界）返回 nil
    func cropBaseImage(around focus: CGPoint, srcLogicalW: CGFloat, srcLogicalH: CGFloat) -> CGImage? {
        let scale = baseFrame.scale
        let imgW = CGFloat(baseFrame.image.width)
        let imgH = CGFloat(baseFrame.image.height)
        let wPhys = (srcLogicalW * scale).rounded(.down)
        let hPhys = (srcLogicalH * scale).rounded(.down)
        guard wPhys > 0, hPhys > 0 else { return nil }
        let fx = ((focus.x - baseFrame.origin.x) * scale).rounded(.down)
        let fy = ((focus.y - baseFrame.origin.y) * scale).rounded(.down)
        let x0 = max(0, fx - wPhys / 2)
        let y0 = max(0, fy - hPhys / 2)
        let w = min(wPhys, imgW - x0)
        let h = min(hPhys, imgH - y0)
        guard w > 0, h > 0 else { return nil }
        return baseFrame.image.cropping(to: CGRect(x: x0, y: y0, width: w, height: h))
    }

    /// 采样指定点的像素颜色（对齐 GetPixelColorFromBitmap：物理 = (逻辑 - 原点) * scale）。
    /// - Parameter point: CG 全局逻辑坐标
    /// - Returns: RGB 颜色；点越界返回 nil（调用方保留上一次颜色）
    func samplePixelColor(at point: CGPoint) -> ScreenshotRGB? {
        let scale = baseFrame.scale
        let px = ((point.x - baseFrame.origin.x) * scale).rounded(.down)
        let py = ((point.y - baseFrame.origin.y) * scale).rounded(.down)
        guard px >= 0, py >= 0, px < CGFloat(baseFrame.image.width), py < CGFloat(baseFrame.image.height) else {
            return nil
        }
        guard let cropped = baseFrame.image.cropping(to: CGRect(x: px, y: py, width: 1, height: 1)) else {
            return nil
        }
        var pixel = [UInt8](repeating: 0, count: 4)
        let ok = pixel.withUnsafeMutableBytes { ptr -> Bool in
            guard let ctx = CGContext(
                data: ptr.baseAddress, width: 1, height: 1, bitsPerComponent: 8, bytesPerRow: 4,
                space: CGColorSpaceCreateDeviceRGB(),
                bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue | CGBitmapInfo.byteOrder32Big.rawValue
            ) else { return false }
            ctx.draw(cropped, in: CGRect(x: 0, y: 0, width: 1, height: 1))
            return true
        }
        guard ok else { return nil }
        return ScreenshotRGB(r: pixel[0], g: pixel[1], b: pixel[2])
    }

    // MARK: 浮层几何（绘制与局部失效共用，保证脏区与实际绘制一致）

    /// 拖拽创建中的实时选区（规范化，对齐 OnPaint Selecting 分支的 min/max 计算）。
    func currentDragRect() -> CGRect {
        return CGRect(
            x: min(dragStartPoint.x, dragCurrentPoint.x),
            y: min(dragStartPoint.y, dragCurrentPoint.y),
            width: abs(dragCurrentPoint.x - dragStartPoint.x),
            height: abs(dragCurrentPoint.y - dragStartPoint.y))
    }

    /// Idle 态浮层几何：高亮框（hover 窗口，否则鼠标所在屏——对齐 OnPaint CS_Idle 退化链）
    /// + 尺寸标签（W×H）+ 跟随放大镜面板。绘制与失效共用同一几何来源。
    func idleOverlayRects() -> (highlight: CGRect?, label: CGRect?, labelText: String, panel: CGRect) {
        var highlight: CGRect?
        var labelText = ""
        if hoveredWindowIndex >= 0 && hoveredWindowIndex < snapWindows.count {
            let r = snapWindows[hoveredWindowIndex].rect
            highlight = r
            labelText = "\(Int(r.width.rounded())) × \(Int(r.height.rounded()))"
        } else if let screen = screenContaining(mouse) {
            let cg = ScreenshotGeometry.cgFrame(of: screen)
            highlight = cg
            labelText = "\(Int(cg.width.rounded())) × \(Int(cg.height.rounded()))"
        }
        let label = labelText.isEmpty ? nil : highlight.flatMap { scCalcSizeLabelRect(labelText, $0, virtualBounds) }
        return (highlight, label, labelText, scCalcPanelRect(mouse, virtualBounds))
    }

    /// Idle 态浮层并集（局部失效用）。
    func idleOverlayRectsUnion() -> CGRect? {
        let r = idleOverlayRects()
        return scUnionRect(scUnionRect(r.highlight, r.label), r.panel)
    }

    /// 查找包含指定点的屏幕（鼠标所在屏退化链与单屏退化用）。
    private func screenContaining(_ point: CGPoint) -> NSScreen? {
        return NSScreen.screens.first { ScreenshotGeometry.cgFrame(of: $0).contains(point) }
    }

    /// 圆角手柄的脏区矩形（以手柄中心为基点外扩 handleMargin，覆盖半径 + 描边/抗锯齿；
    /// 对齐 Windows CornerHandleDirtyRect）。
    private func cornerKnobDirtyRect(_ corner: ScreenshotResizeHandle) -> CGRect? {
        guard corner != .none else { return nil }
        let center = scCornerKnobCenter(selection, SC.cornerKnobInset, selectionCornerRadius, corner)
        return scInflate(CGRect(origin: center, size: .zero), SC.handleMargin)
    }

    // MARK: 鼠标事件（OverlayScreenshotView 转发；坐标已换算为 CG 全局整数点）

    /// 左键按下（对齐 Windows OnLButtonDown 的基础子集）。
    /// - Parameters:
    ///   - point: 鼠标 CG 全局坐标
    ///   - clickCount: 点击计数（2 = 双击，确认态双击选区内确认）
    func handleMouseDown(_ point: CGPoint, clickCount: Int) {
        guard isRunning else { return }
        mouse = point
        switch state {
        case .idle:
            // 开始新的框选；新框选从直角开始（对齐 OnLButtonDown CS_Idle 分支）
            dragStartPoint = point
            dragCurrentPoint = point
            selectionCornerRadius = 0
            state = .selecting
            lastIdleOverlayRect = nil
            invalidateAll()
        case .textEditing:
            // 文字编辑态：输入框拖选 / 选区内换位（提交路径 4）/ 选区外退出（提交路径 5）。
            // 工具栏/子菜单为独立浮层窗口，其点击提交路径（1/2/3）在工具栏控制器承接。
            handleTextEditingMouseDown(point)
        case .confirmed:
            // 双击选区内确认（对齐 WM_LBUTTONDBLCLK：确认态 + 选区内 → 确认截图）
            if clickCount >= 2 && selection.contains(point) {
                confirmSelection()
                return
            }
            // ---- 标注交互（对齐 overlay_input_windows.cpp OnLButtonDown CS_Confirmed 分支顺序）----
            // 1) 已选中标注的缩放手柄命中 → 进入标注缩放（先于普通命中：手柄贴在选中框上；
            //    拖拽按钮下不响应，纯选择模式）
            if selectedAnnotation >= 0 && selectedAnnotation < annotations.count && activeTool != .drag {
                let handle = scHitTestAnnotationHandle(annotations[selectedAnnotation], point, SC.handleSize)
                if handle != .none {
                    resizingAnnotation = selectedAnnotation
                    annotationResizeHandle = handle
                    annotationDragStartPoint = point
                    annotationResizeStartBox = scMeasureAnnotationBounds(annotations[selectedAnnotation])
                    dragStartAnnotation = annotations[selectedAnnotation]
                    annotationOpHistoryPushed = false
                    lastAnnotationOpBox = .null   // 进入标注缩放：复位上帧脏区链
                    invalidateAll()
                    return
                }
            }
            // 2) 文字标注命中 → 优先选中并可拖动（先于非文字命中，避免覆盖物重叠时被吞掉）。
            //    selectedTextAnnotation 持久保持选中；文字工具回显（activeTool=.text + 子菜单 +
            //    字号/颜色回显，对齐 EchoFontIdx/EchoColorIdx 分支）。
            let hitText = scHitTestTextAnnotations(annotations, point)
            if hitText >= 0 {
                selectTextAnnotation(hitText, at: point)
                return
            }
            // 3) 任意非文字标注命中（z 序取最上）→ 选中并进入拖拽 + 工具栏回显
            //    （工具激活时也优先选中已有对象，点空白才绘制）
            let hitAnn = scHitTestAnnotation(annotations, point)
            if hitAnn >= 0 && annotations[hitAnn].type != .text {
                let dirty = scUnionRect(selectedAnnotationDirtyRect(), selectedTextAnnotationDirtyRect())
                selectedTextAnnotation = -1
                draggingTextAnnotation = -1
                selectedAnnotation = hitAnn
                draggingAnnotation = hitAnn
                annotationDragStartPoint = point
                dragStartAnnotation = annotations[hitAnn]
                annotationOpHistoryPushed = false
                lastAnnotationOpBox = .null   // 进入标注拖拽：复位上帧脏区链
                // 工具栏回显：切换到该标注对应工具，子菜单回显粗细/颜色（对齐回显分支）
                let hitA = annotations[hitAnn]
                activeTool = scAnnotationTypeToTool(hitA.type)
                if let tool = activeTool {
                    toolbar.openPopup(for: tool)
                }
                scEchoThickIdx(hitA.thickness)
                scEchoColorIdx(hitA.color)
                if let dirty = dirty { invalidate(dirty) }
                invalidateAll()
                return
            }
            // 4) 矢量工具激活 + 选区内点空白 → 开始绘制新标注（子菜单保持打开，
            //    当前粗细/颜色在绘制开始时固化）
            if let tool = activeTool, tool.isVectorTool, selection.contains(point) {
                beginAnnotationDrawing(at: point)
                return
            }
            // 5) 马赛克工具激活 + 选区内点空白 → 开始马赛克绘制（框选/涂抹按子菜单模式）
            if activeTool == .mosaic, selection.contains(point) {
                beginMosaicDrawing(at: point)
                return
            }
            // 6) 文字工具激活 + 选区内点空白 → 进入文字编辑态（清选中，子菜单保持打开）
            if activeTool == .text, selection.contains(point) {
                beginTextEditing(at: point)
                return
            }
            // 7) 8 个 resize 手柄命中 → Resizing（实时命中，不依赖 hover 缓存；
            //    进入手柄调整时清除标注选中，对齐 Windows 同分支行为）
            let handle = scHitTestHandle(point, selection, SC.handleSize)
            if handle != .none {
                let dirty = scUnionRect(selectedAnnotationDirtyRect(), selectedTextAnnotationDirtyRect())
                clearAnnotationSelection()
                invalidate(dirty)
                resizeHandle = handle
                handleDragStartPoint = point
                dragStartSelection = selection
                kbDX = 0
                kbDY = 0
                state = .resizing
                invalidateAll()
                return
            }
            // 圆角手柄命中 → 圆角调整（复用 CS_Resizing + 角手柄，记录拖拽起始半径）
            let corner = scHitTestCornerKnob(point, selection, SC.handleSize, SC.cornerKnobInset, selectionCornerRadius)
            if corner != .none {
                let dirty = scUnionRect(selectedAnnotationDirtyRect(), selectedTextAnnotationDirtyRect())
                clearAnnotationSelection()
                invalidate(dirty)
                resizeHandle = corner
                handleDragStartPoint = point
                dragStartSelection = selection
                cornerDragStartRadius = selectionCornerRadius
                kbDX = 0
                kbDY = 0
                state = .resizing
                invalidateAll()
                return
            }
            // 选区内 → 整体拖动（有标注内容时禁用，避免标注与背景错位——对齐 Windows；
            // 此时点空白仅取消当前选中）
            if selection.contains(point) {
                if !canDragSelection() {
                    if selectedAnnotation >= 0 || selectedTextAnnotation >= 0 {
                        let dirty = scUnionRect(selectedAnnotationDirtyRect(), selectedTextAnnotationDirtyRect())
                        clearAnnotationSelection()
                        invalidate(dirty)
                    }
                    return
                }
                clearAnnotationSelection()
                handleDragStartPoint = point
                dragStartSelection = selection
                state = .moving
                invalidateAll()
                return
            }
            // 选区外点击：清除标注选中（确认态不可重新框选，对齐 Windows 选区外分支）
            if selectedAnnotation >= 0 || selectedTextAnnotation >= 0 {
                let dirty = scUnionRect(selectedAnnotationDirtyRect(), selectedTextAnnotationDirtyRect())
                clearAnnotationSelection()
                invalidate(dirty)
            }
        default:
            break
        }
        updateCursor()
    }

    /// 左键拖动（对齐 Windows OnMouseMove 的基础子集）。
    /// - Parameter point: 鼠标 CG 全局坐标
    func handleMouseDragged(_ point: CGPoint) {
        guard isRunning else { return }
        mouse = point
        switch state {
        case .selecting:
            // 更新框选终点；蒙版边界大范围变化，全屏失效（对齐 Selecting 的 InvalidateRect(NULL)）
            dragCurrentPoint = point
            invalidateAll()
        case .resizing:
            if resizeHandle.isCorner {
                // 圆角调整：手柄沿所在角对角线滑动，四角同步（共用同一 radius）。
                // 取鼠标位移在该角对角线方向的投影（两向内分量之和/2）作为半径增量，
                // 垂直于对角线的位移被忽略，手柄轨迹恒为对角线（对齐 CS_Resizing 圆角分支）。
                let dx = point.x - handleDragStartPoint.x
                let dy = point.y - handleDragStartPoint.y
                let inX: CGFloat
                let inY: CGFloat
                switch resizeHandle {
                case .cornerTL: inX = dx; inY = dy       // 右下为内
                case .cornerTR: inX = -dx; inY = dy      // 左下为内
                case .cornerBL: inX = dx; inY = -dy      // 右上为内
                case .cornerBR: inX = -dx; inY = -dy     // 左上为内
                default: inX = dx; inY = dy
                }
                let diagDelta = (inX + inY) / 2
                selectionCornerRadius = scClampCornerRadius(cornerDragStartRadius + diagDelta, selection)
                invalidateAll()
            } else {
                // 标准手柄：每帧从按下快照重算，活动端可穿越固定端翻转（拖拽中不强制最小尺寸）
                applyResizeSelection(enforceMinSize: false)
            }
        case .moving:
            // 整体平移：按"按下矩形 + 鼠标位移"重算并钳制在虚拟屏内（对齐 CS_Moving 分支）
            let dx = point.x - handleDragStartPoint.x
            let dy = point.y - handleDragStartPoint.y
            var nl = dragStartSelection.minX + dx
            var nt = dragStartSelection.minY + dy
            if nl < virtualBounds.minX { nl = virtualBounds.minX }
            if nt < virtualBounds.minY { nt = virtualBounds.minY }
            if nl + dragStartSelection.width > virtualBounds.maxX {
                nl = virtualBounds.maxX - dragStartSelection.width
            }
            if nt + dragStartSelection.height > virtualBounds.maxY {
                nt = virtualBounds.maxY - dragStartSelection.height
            }
            selection = CGRect(origin: CGPoint(x: nl, y: nt), size: dragStartSelection.size)
            // 未拖放过的工具栏实时跟随选区（对齐 OnPaint 的 CalcToolbarPosition 分支）
            toolbar.syncPlacement()
            invalidateAll()
        case .drawing:
            // 更新进行中标注终点/路径（终点钳制选区内、画笔/涂抹马赛克逐点追加，对齐分支）
            updateAnnotationDrawing(point)
        case .textEditing:
            // 文字编辑态：拖动选择文字（对齐 OnMouseMove CS_TextEditing 分支）
            handleTextEditingDragged(point)
        case .confirmed:
            // 标注拖拽/缩放（对齐 OnMouseMove 的 resizing/dragging 分支）：
            // 状态保持 confirmed，由索引标志区分（与文字拖拽机制对称，Windows 同款）
            if resizingAnnotation >= 0 {
                applyAnnotationResizeDrag(point)
            } else if draggingAnnotation >= 0 {
                applyAnnotationMoveDrag(point)
            } else if draggingTextAnnotation >= 0 {
                // 拖动文字标注（对齐 OnMouseMove draggingTextAnnotation 分支）
                applyTextAnnotationMoveDrag(point)
            }
        default:
            break
        }
        updateCursor()
    }

    /// 左键抬起（对齐 Windows OnLButtonUp 的基础子集；autoConfirm 仅在 Selecting 分支生效）。
    /// - Parameter point: 鼠标 CG 全局坐标
    func handleMouseUp(_ point: CGPoint) {
        guard isRunning else { return }
        mouse = point
        switch state {
        case .selecting:
            let w = abs(dragCurrentPoint.x - dragStartPoint.x)
            let h = abs(dragCurrentPoint.y - dragStartPoint.y)
            var finalRect: CGRect
            if w <= 1 && h <= 1 {
                // 拖动 ≤1×1px 视为单击 → 智能窗口吸附退化链：
                // 候选窗口（Z 序首个命中）→ 鼠标所在屏幕 → 虚拟屏（对齐 FindWindowAtPoint + MonitorFromPoint）
                finalRect = snapRectAtPoint(dragCurrentPoint)
            } else {
                finalRect = currentDragRect()
            }
            enterConfirmed(finalRect)
            if options.autoConfirm {
                // autoConfirm：松手（或单击吸附确定）→ 直接确认输出，不进编辑态
                confirmSelection()
                return
            }
        case .resizing:
            if resizeHandle.isCorner {
                // 圆角调整结束：钳制半径即足够（选区矩形未变），回确认态并重算靠近角
                selectionCornerRadius = scClampCornerRadius(selectionCornerRadius, selection)
                resizeHandle = .none
                hoveredCornerHandle = scFindNearestCornerKnob(mouse, selection, SC.handleSize,
                                                              SC.cornerKnobInset, selectionCornerRadius, SC.cornerProximity)
                state = .confirmed
                invalidateAll()
            } else {
                // resize 结束：只沿活动端当前所在侧补足最小尺寸（不能复用 EnterConfirmed 的
                // 固定向右/下扩张，否则穿越后会移动按下时的固定点）；键盘微调一并固化。
                applyResizeSelection(enforceMinSize: true)
                kbDX = 0
                kbDY = 0
                resizeHandle = .none
                hoveredCornerHandle = .none
                state = .confirmed
                // 选区尺寸变化后半径可能越界，钳制
                selectionCornerRadius = scClampCornerRadius(selectionCornerRadius, selection)
                invalidateAll()
            }
        case .moving:
            // 整体拖动结束仍走确认流程（对齐 CS_Moving 分支的 EnterConfirmed）
            enterConfirmed(selection)
        case .drawing:
            // 绘制结束 → 有效尺寸/路径提交并 Push 历史（对齐 OnLButtonUp CS_Drawing 分支；
            // 马赛克框选 ≥2px / 涂抹 ≥1 点的有效性判定在 finishAnnotationDrawing 内）
            finishAnnotationDrawing()
        case .textEditing:
            // 文字选择结束（对齐 OnLButtonUp CS_TextEditing 分支）
            handleTextEditingMouseUp()
        case .confirmed:
            // 标注拖拽/缩放收束：退出拖拽态并按最终包围盒刷新（对齐 OnLButtonUp 分支，
            // 含 draggingTextAnnotation）。局部脏区 = 上帧盒 ∪ 最终盒（性能审计：
            // 对齐 Windows 收束路径 InvalidateAnnotationOp(finalBox) 的局部刷新，不退化为全屏重绘）。
            if resizingAnnotation >= 0 || draggingAnnotation >= 0 || draggingTextAnnotation >= 0 {
                var finalBox = CGRect.null
                if resizingAnnotation >= 0 && resizingAnnotation < annotations.count {
                    finalBox = scMeasureAnnotationBounds(annotations[resizingAnnotation])
                } else if draggingAnnotation >= 0 && draggingAnnotation < annotations.count {
                    finalBox = scMeasureAnnotationBounds(annotations[draggingAnnotation])
                } else if draggingTextAnnotation >= 0 && draggingTextAnnotation < annotations.count {
                    finalBox = scMeasureAnnotationBounds(annotations[draggingTextAnnotation])
                }
                resizingAnnotation = -1
                draggingAnnotation = -1
                draggingTextAnnotation = -1
                annotationResizeHandle = .none
                annotationOpHistoryPushed = false
                if finalBox.isNull {
                    // 防御兜底：索引非法拿不到最终包围盒时退回全屏刷新（正常路径不可达）
                    lastAnnotationOpBox = .null
                    invalidateAll()
                } else {
                    let dirty = scInflate(scUnionRect(lastAnnotationOpBox.isNull ? nil : lastAnnotationOpBox,
                                                      finalBox) ?? finalBox, SC.handleMargin)
                    lastAnnotationOpBox = .null
                    invalidate(dirty)
                }
            }
        default:
            break
        }
        updateCursor()
    }

    /// 鼠标移动（无按键，对齐 Windows OnMouseMove 的 Idle/Confirmed hover 分支）。
    /// - Parameter point: 鼠标 CG 全局坐标
    func handleMouseMoved(_ point: CGPoint) {
        guard isRunning else { return }
        mouse = point
        switch state {
        case .idle:
            // hover 窗口命中 + 采样取色 + 放大镜跟随：旧 ∪ 新浮层并集局部失效。
            // 节流审计结论（放大镜/面板跟随）：Windows overlay_ui_windows.cpp 无显式
            // 30fps 节流常量，跟随 = WM_MOUSEMOVE 事件驱动 + 脏区失效（其 RDP 注释强调
            // 实时命中、不依赖 hover 缓存）。macOS 对齐同语义：跟随仅由鼠标事件驱动、
            // 放大镜区域随本并集局部重绘，无定时器轮询、无丢帧合并；长截图侧小地图/
            // 工具栏均为状态变化按需刷新（contentChanged/trackingChanged/uiTick 100ms）。
            let oldUnion = lastIdleOverlayRect
            hoveredWindowIndex = findWindowAtPoint(snapWindows, point)
            currentColor = samplePixelColor(at: point) ?? currentColor
            let newUnion = idleOverlayRectsUnion()
            if let dirty = scUnionRect(oldUnion, newUnion) {
                invalidate(scInflate(dirty, 5))   // 余量覆盖 3px 高亮描边与抗锯齿
            }
            lastIdleOverlayRect = newUnion
        case .confirmed:
            // 圆角手柄"靠近"探测：感应区比命中框大一圈，靠近即显示该角手柄；
            // 出现/消失/换角时旧 ∪ 新手柄位置局部重绘（对齐 hoveredCornerHandle 更新逻辑）
            let near = scFindNearestCornerKnob(point, selection, SC.handleSize,
                                               SC.cornerKnobInset, selectionCornerRadius, SC.cornerProximity)
            if near != hoveredCornerHandle {
                var dirty = cornerKnobDirtyRect(hoveredCornerHandle)
                dirty = scUnionRect(dirty, cornerKnobDirtyRect(near))
                hoveredCornerHandle = near
                invalidate(dirty)
            }
        default:
            break
        }
        updateCursor()
    }

    /// 键盘事件（对齐 Windows OnKeyDown；ESC 优先走 event tap 兜底，tap 缺位时由覆盖层
    /// 自身 keyDown 承接。文字编辑态整段交给 handleTextEditingKeyDown：IME/可打印字符经
    /// NSTextInputClient，键系 Backspace/Delete/Left/Right/Home/End/Enter/ESC 自管）。
    /// - Parameters:
    ///   - event: 键盘事件
    ///   - textInputContext: 覆盖层视图的输入上下文（文字编辑态路由 IME 用）
    func handleKeyDown(_ event: NSEvent, textInputContext: NSTextInputContext? = nil) {
        guard isRunning else { return }
        // 文字编辑态：IME + 键系（含 ESC=清缓冲、Enter=提交，消费后不落入下方取消/确认分支）
        if state == .textEditing, handleTextEditingKeyDown(event, inputContext: textInputContext) {
            return
        }
        switch event.keyCode {
        case 36, 76:
            // Enter / 小键盘 Enter：确认态确认截图
            if state == .confirmed {
                confirmSelection()
            }
        case 123, 124, 125, 126:
            // 方向键微调（对齐 HandleSelectionNudgeKey）：Resizing 微调活动边，Confirmed 整体平移；
            // Shift 加速到 10px
            let step: CGFloat = event.modifierFlags.contains(.shift) ? 10 : 1
            let ddx: CGFloat
            let ddy: CGFloat
            switch event.keyCode {
            case 123: ddx = -step; ddy = 0    // Left
            case 124: ddx = step; ddy = 0     // Right
            case 125: ddx = 0; ddy = step     // Down
            default: ddx = 0; ddy = -step     // Up
            }
            if state == .resizing && !resizeHandle.isCorner {
                kbDX += ddx
                kbDY += ddy
                applyResizeSelection(enforceMinSize: false)
            } else if state == .confirmed {
                nudgeSelection(dx: ddx, dy: ddy)
            }
        case 51, 117:
            // Delete / Backspace（Windows VK_DELETE 语义；Mac 上两键都删便于操作）：
            // 确认态删除选中覆盖物（文字标注优先，入历史），随后回到纯选择模式并关闭子菜单
            //（对齐 OnKeyDown VK_DELETE 分支的 activeTool=TB_Drag 收尾）
            if state == .confirmed {
                if selectedTextAnnotation >= 0 && selectedTextAnnotation < annotations.count {
                    pushAnnotationHistory()
                    annotations.remove(at: selectedTextAnnotation)
                    clearAnnotationSelection()
                    activeTool = .drag
                    toolbar.closePopup()
                    invalidateAll()
                } else if selectedAnnotation >= 0 && selectedAnnotation < annotations.count {
                    pushAnnotationHistory()
                    annotations.remove(at: selectedAnnotation)
                    clearAnnotationSelection()
                    activeTool = .drag
                    toolbar.closePopup()
                    invalidateAll()
                }
            }
        case 53:
            // ESC：取消（event tap 活跃时该事件已被 tap 吞掉不会到达此处；
            // 文字编辑态的 ESC 已在上方分支清缓冲消费，不取消截图）
            cancelSession()
        default:
            break
        }
    }

    // MARK: 工具栏/子菜单动作（ScreenshotToolbarController 回话）

    /// 工具栏按钮点击（对齐 overlay_input_windows.cpp OnLButtonDown 的工具栏分支）：
    /// 选中态保留规则——点击「与已选中标注同类型」的工具按钮保留选中仅切换子菜单开合并
    /// 回显其参数；异类/无关按钮取消选中。
    /// - Parameter button: 命中的可用按钮
    func handleToolbarButton(_ button: ScreenshotToolButton) {
        guard isRunning, state == .confirmed else { return }
        switch button {
        case .confirm:
            confirmSelection()
            return
        case .cancel:
            cancelSession()
            return
        case .save:
            // 保存到本地（对齐 Windows TB_Save 分支）：取消对话框回编辑态；
            // 保存成功/失败均结束会话并回调（不写剪贴板），见 ScreenshotOutputMac.swift
            saveSelectionToFile()
            return
        case .undo:
            _ = undoAnnotations()
            return
        case .redo:
            _ = redoAnnotations()
            return
        case .drag, .rect, .circle, .arrow, .brush, .text:
            // 选中态保留规则：同类工具点击保留选中并回显；异类切换取消当前选中
            let matchesSelection: Bool
            if button == .text {
                // 文字按钮匹配「已选中文字标注」（对齐 TB_Text 分支的 matchesSelection）
                matchesSelection = selectedTextAnnotation >= 0 && selectedTextAnnotation < annotations.count
            } else if button == .drag {
                matchesSelection = false
            } else {
                matchesSelection = selectedAnnotation >= 0 && selectedAnnotation < annotations.count
                    && scAnnotationTypeToTool(annotations[selectedAnnotation].type) == button
            }
            if !matchesSelection && (selectedAnnotation >= 0 || selectedTextAnnotation >= 0) {
                let dirty = scUnionRect(selectedAnnotationDirtyRect(), selectedTextAnnotationDirtyRect())
                clearAnnotationSelection()
                invalidate(dirty)
            }
            if button == .drag {
                // 拖拽按钮 = 取消当前工具回到纯选择模式（再次点击切换开合）；
                // 仍有选中项时回显其参数（对齐 TB_Drag 分支：文字→字号/颜色，矢量→粗细/颜色）
                if activeTool == .drag {
                    activeTool = nil
                } else {
                    activeTool = .drag
                    if selectedTextAnnotation >= 0 && selectedTextAnnotation < annotations.count {
                        toolbar.openPopup(for: .text)
                        scEchoFontIdx(annotations[selectedTextAnnotation].thickness)
                        scEchoColorIdx(annotations[selectedTextAnnotation].color)
                    } else if selectedAnnotation >= 0 && selectedAnnotation < annotations.count {
                        let selA = annotations[selectedAnnotation]
                        if let tool = scAnnotationTypeToTool(selA.type) {
                            toolbar.openPopup(for: tool)
                        }
                        scEchoThickIdx(selA.thickness)
                        scEchoColorIdx(selA.color)
                    } else {
                        toolbar.closePopup()
                    }
                }
            } else if activeTool == button {
                // 再次点同一工具：关闭工具与子菜单（同类开/合子菜单规则）
                activeTool = nil
                toolbar.closePopup()
            } else {
                // 切换到该工具并打开子菜单（作用于后续绘制的新标注；
                // 匹配选中项时回显该标注的粗细/颜色或字号/颜色）
                activeTool = button
                toolbar.openPopup(for: button)
                if matchesSelection {
                    if button == .text, selectedTextAnnotation >= 0 && selectedTextAnnotation < annotations.count {
                        scEchoFontIdx(annotations[selectedTextAnnotation].thickness)
                        scEchoColorIdx(annotations[selectedTextAnnotation].color)
                    } else if selectedAnnotation >= 0 && selectedAnnotation < annotations.count {
                        scEchoThickIdx(annotations[selectedAnnotation].thickness)
                        scEchoColorIdx(annotations[selectedAnnotation].color)
                    }
                }
            }
            invalidateAll()
            toolbar.refresh()
        case .mosaic:
            // 马赛克工具：切换激活态 + 开合三段子菜单（涂抹|框选｜块大小｜涂抹半径）。
            // 马赛克不可选中 → 永不匹配选中项；选项只影响后续绘制，点击即取消当前选中
            //（对齐 TB_Mosaic 分支 + 马赛克 popup 的 clearSel 语义）。
            if selectedAnnotation >= 0 || selectedTextAnnotation >= 0 {
                let dirty = scUnionRect(selectedAnnotationDirtyRect(), selectedTextAnnotationDirtyRect())
                clearAnnotationSelection()
                invalidate(dirty)
            }
            if activeTool == .mosaic {
                activeTool = nil
                toolbar.closePopup()
            } else {
                activeTool = .mosaic
                toolbar.openPopup(for: .mosaic)
            }
            invalidateAll()
            toolbar.refresh()
        case .translate:
            // 翻译：占位图标，无点击处理（与 Windows TB_Translate 行为一致）
            break
        case .longCapture:
            // 长截图：进入长截图滚动捕获（隐藏覆盖层 → 灰蒙版 + 小地图 + 长截图
            // 工具栏接管；会话不销毁，完成/保存成功按成功收束、取消/ESC/abort 按失败收束）。
            // 实现见 ScreenshotLongCaptureMac.swift。
            beginLongCapture()
            return
        case .separator1, .separator2:
            // 分隔线：不响应（保存已启用，见上方 .save 分支）
            break
        }
    }

    /// 子菜单选择应用（矢量/文字命中码：+1..+3 粗细或字号 / -1..-8 颜色；马赛克命中码：
    /// +1/+2 模式、+101.. 块大小、+201.. 涂抹半径）。
    /// 选中已有标注时回显后修改直接作用于该标注（先 Push 历史，对齐 Windows 就地修改语义）；
    /// 无选中时作用于后续绘制的新标注。
    /// - Parameter hit: 子菜单命中码
    func applyPopupSelection(_ hit: Int) {
        guard isRunning, state == .confirmed, hit != 0, let tool = toolbar.popupTool else { return }
        switch tool {
        case .text:
            // 文字子菜单：第一组为字号（对齐 popupTool == TB_Text 分支）
            if hit > 0 && hit <= SC.fontSizes.count {
                fontSizeIdx = hit - 1
                // 选中文字标注 → 修改其字号（先 Push 历史）
                if selectedTextAnnotation >= 0 && selectedTextAnnotation < annotations.count {
                    let newSize = SC.fontSizes[fontSizeIdx]
                    if annotations[selectedTextAnnotation].thickness != newSize {
                        pushAnnotationHistory()
                        annotations[selectedTextAnnotation].thickness = newSize
                        invalidateAll()
                    }
                }
            } else if hit < 0 && -hit <= SC.colorPresets.count {
                drawColorIdx = -hit - 1
                // 选中文字标注 → 修改其颜色（先 Push 历史）
                if selectedTextAnnotation >= 0 && selectedTextAnnotation < annotations.count {
                    let newColor = SC.colorPresets[drawColorIdx]
                    if annotations[selectedTextAnnotation].color != newColor {
                        pushAnnotationHistory()
                        annotations[selectedTextAnnotation].color = newColor
                        invalidateAll()
                    }
                }
            }
        case .mosaic:
            // 马赛克专属三段子菜单（对齐 HitTestMosaicPopup 命中码与 clearSel 语义）：
            // 选项只影响后续绘制，均取消当前选中（脏区在清空前计算）
            if hit == 1 || hit == 2 || (hit >= SC.mosaicHitSizeBase + 1 && hit < SC.mosaicHitSizeBase + 1 + SC.mosaicSizes.count)
                || (hit >= SC.mosaicHitRadiusBase + 1 && hit < SC.mosaicHitRadiusBase + 1 + SC.mosaicRadii.count) {
                let dirty = scUnionRect(selectedAnnotationDirtyRect(), selectedTextAnnotationDirtyRect())
                clearAnnotationSelection()
                invalidate(dirty)
            }
            switch hit {
            case 1:
                mosaicRectMode = false   // 涂抹模式
            case 2:
                mosaicRectMode = true    // 框选模式
            default:
                if hit >= SC.mosaicHitSizeBase + 1, hit < SC.mosaicHitSizeBase + 1 + SC.mosaicSizes.count {
                    mosaicSizeIdx = hit - SC.mosaicHitSizeBase - 1   // 块大小
                } else if hit >= SC.mosaicHitRadiusBase + 1, hit < SC.mosaicHitRadiusBase + 1 + SC.mosaicRadii.count {
                    mosaicRadiusIdx = hit - SC.mosaicHitRadiusBase - 1   // 涂抹半径
                }
            }
        default:
            // 矢量子菜单：第一组粗细 / 第二组颜色（选中矢量标注时就地修改，先 Push 历史）
            if hit > 0 && hit <= SC.thickPresets.count {
                drawThickIdx = hit - 1
                if selectedAnnotation >= 0 && selectedAnnotation < annotations.count {
                    let newThickness = SC.thickPresets[drawThickIdx]
                    if annotations[selectedAnnotation].thickness != newThickness {
                        pushAnnotationHistory()
                        annotations[selectedAnnotation].thickness = newThickness
                        invalidateAll()
                    }
                }
            } else if hit < 0 && -hit <= SC.colorPresets.count {
                drawColorIdx = -hit - 1
                if selectedAnnotation >= 0 && selectedAnnotation < annotations.count {
                    let newColor = SC.colorPresets[drawColorIdx]
                    if annotations[selectedAnnotation].color != newColor {
                        pushAnnotationHistory()
                        annotations[selectedAnnotation].color = newColor
                        invalidateAll()
                    }
                }
            }
        }
        toolbar.refresh()
    }

    // MARK: 选区状态机辅助

    /// 进入确认态（wndproc_windows.cpp EnterConfirmed 移植）：规范化选区、钳制虚拟屏边界、
    /// 补足最小尺寸（SC_MIN_SELECTION）、钳制圆角半径并切换状态。
    /// - Parameter rect: 候选选区（CG 全局坐标，未规范化）
    private func enterConfirmed(_ rect: CGRect) {
        var n = rect.standardized
        // 约束到虚拟屏幕内（Windows 语义：仅收缩边界，固定端不移动）
        if n.minX < virtualBounds.minX { n.origin.x = virtualBounds.minX }
        if n.minY < virtualBounds.minY { n.origin.y = virtualBounds.minY }
        if n.maxX > virtualBounds.maxX { n.size.width = virtualBounds.maxX - n.minX }
        if n.maxY > virtualBounds.maxY { n.size.height = virtualBounds.maxY - n.minY }
        // 最小尺寸保护（向右/下扩张）
        if n.width < SC.minSelection { n.size.width = SC.minSelection }
        if n.height < SC.minSelection { n.size.height = SC.minSelection }
        selection = scIntegralRect(n)
        resizeHandle = .none
        hoveredCornerHandle = .none
        selectionCornerRadius = scClampCornerRadius(selectionCornerRadius, selection)
        state = .confirmed
        invalidateAll()
        // 工具栏：确认态默认回到纯选择模式（对齐 EnterConfirmed 的 activeTool 缺省 TB_Drag），
        // 工具栏/子菜单浮层随状态机在 pumpTick 同步显隐与位置（首拍即出现）。
        if activeTool == nil { activeTool = .drag }
        toolbar.syncVisibility(true)
        toolbar.syncPlacement()
    }

    /// 从按下快照 + (鼠标位移 + 键盘微调) 实时重算选区，并刷新放大镜焦点像素色
    /// （wndproc_windows.cpp ApplyResizeSelection 移植；MOUSEMOVE 与方向键共用保证一致）。
    /// - Parameter enforceMinSize: 拖拽中 false；松开时 true（沿活动端当前侧补足最小尺寸）
    private func applyResizeSelection(enforceMinSize: Bool) {
        let dx = (mouse.x - handleDragStartPoint.x) + kbDX
        let dy = (mouse.y - handleDragStartPoint.y) + kbDY
        selection = scResizeSelectionFromHandle(dragStartSelection, resizeHandle, dx, dy,
                                                virtualBounds, annotationsContentBounds(), enforceMinSize)
        // 放大镜焦点取活动手柄锚点（随活动边移动，键盘微调时鼠标不动也能跟随）
        let anchor = scGetResizeHandleAnchor(resizeHandle, selection)
        currentColor = samplePixelColor(at: anchor) ?? currentColor
        invalidateAll()
    }

    /// 确认态方向键整体平移（对齐 HandleSelectionNudgeKey 的 CS_Confirmed 分支）：钳制虚拟屏。
    /// - Parameters:
    ///   - dx/dy: 位移（1px 或 Shift 10px）
    private func nudgeSelection(dx: CGFloat, dy: CGFloat) {
        var nl = selection.minX + dx
        var nt = selection.minY + dy
        if nl < virtualBounds.minX { nl = virtualBounds.minX }
        if nt < virtualBounds.minY { nt = virtualBounds.minY }
        if nl + selection.width > virtualBounds.maxX { nl = virtualBounds.maxX - selection.width }
        if nt + selection.height > virtualBounds.maxY { nt = virtualBounds.maxY - selection.height }
        selection = CGRect(origin: CGPoint(x: nl, y: nt), size: selection.size)
        invalidateAll()
    }

    /// 单击吸附退化链（对齐 Windows OnLButtonUp 单击分支）：候选窗口（Z 序首个命中）
    /// → 鼠标所在屏幕（MonitorFromPoint 等价）→ 虚拟屏兜底。
    /// - Parameter point: 鼠标 CG 全局坐标
    /// - Returns: 吸附矩形（CG 全局逻辑坐标）
    private func snapRectAtPoint(_ point: CGPoint) -> CGRect {
        let index = findWindowAtPoint(snapWindows, point)
        if index >= 0 {
            return snapWindows[index].rect
        }
        if let screen = screenContaining(point) {
            return ScreenshotGeometry.cgFrame(of: screen)
        }
        return virtualBounds
    }

    // MARK: 标注约束钩子

    /// 标注内容包围盒钩子（对齐 Windows CalcAnnotationsBounds）：返回已提交标注的内容
    /// 包围盒（绝对 CG 全局坐标），供 scResizeSelectionFromHandle 的内容约束分支使用
    /// （选区不可缩小到裁掉标注内容）。无标注时返回 nil（无内容约束）。
    private func annotationsContentBounds() -> CGRect? {
        return scCalcAnnotationsBounds(annotations)
    }

    /// 整体拖动可用性钩子：已有标注时禁用（标注与背景共享绝对坐标，整体拖动会错位，
    /// 对齐 Windows 标注存在时禁止 CS_Moving 的行为）。
    private func canDragSelection() -> Bool {
        return annotations.isEmpty
    }

    /// 选中标注的脏区矩形（选中视觉变化时局部失效：包围盒外扩手柄余量，覆盖
    /// 虚线框与手柄抗锯齿；未选中返回 nil）。（跨文件共享：文字选中切换清脏区复用。）
    func selectedAnnotationDirtyRect() -> CGRect? {
        guard selectedAnnotation >= 0 && selectedAnnotation < annotations.count else { return nil }
        let box = scMeasureAnnotationBounds(annotations[selectedAnnotation])
        return scInflate(box, SC.handleMargin)
    }

    // MARK: 光标（对齐 Windows OnSetCursor 的基础子集）

    /// 按状态与命中结果切换系统光标（对齐 Windows OnSetCursor 分支次序）：
    /// 马赛克涂抹圆环（涂抹模式 + 选区内）→ 文字态 IBEAM → 绘制态十字 →
    /// 标注缩放手柄方向 → 标注拖动/悬停手型 → 确认态兜底（选区内工具十字/有标注箭头/可整体
    /// 拖动手型/选区外箭头）。工具栏/子菜单区域的光标由 ScreenshotToolbarController.tick
    /// 接管（独立窗口），接管范围内不调用本函数。
    func updateCursor() {
        guard isRunning else { return }

        // 马赛克涂抹模式专属圆环光标（确认态/绘制态 + 选区内；对齐 OnSetCursor 的
        // mosaicBrushCursors 分支——该分支先于文字/绘制/确认态判定）
        if activeTool == .mosaic && !mosaicRectMode
            && (state == .confirmed || state == .drawing)
            && selection.contains(mouse) {
            currentMosaicCursor().set()
            return
        }
        // 文字编辑中：I-beam 光标（对齐 CS_TextEditing 分支）
        if state == .textEditing {
            NSCursor.iBeam.set()
            return
        }

        let cursor: NSCursor
        switch state {
        case .idle, .selecting:
            cursor = .crosshair
        case .resizing:
            cursor = scHandleCursor(resizeHandle)
        case .moving:
            cursor = .closedHand   // Windows IDC_SIZEALL（拖动中）
        case .drawing:
            cursor = .crosshair    // Windows IDC_CROSS（标注绘制中；马赛克框选同十字）
        case .confirmed:
            // 标注拖拽/缩放中的光标（对齐 resizingAnnotation/draggingAnnotation/
            // draggingTextAnnotation 分支：手柄方向光标 / 四向箭头）
            if resizingAnnotation >= 0 && resizingAnnotation < annotations.count {
                cursor = scHandleCursor(annotationResizeHandle)
            } else if draggingAnnotation >= 0 || draggingTextAnnotation >= 0 {
                cursor = .openHand   // Windows IDC_SIZEALL（标注/文字拖动中）
            } else {
                let handle = scHitTestHandle(mouse, selection, SC.handleSize)
                if handle != .none {
                    cursor = scHandleCursor(handle)
                } else {
                    let corner = scHitTestCornerKnob(mouse, selection, SC.handleSize,
                                                     SC.cornerKnobInset, selectionCornerRadius)
                    if corner != .none {
                        cursor = scHandleCursor(corner)
                    } else if selectedAnnotation >= 0 && selectedAnnotation < annotations.count
                                && activeTool != .drag {
                        // 已选中标注的手柄（箭头端点/矩形圆 8 手柄）→ 对应光标；标注悬停 → 四向
                        //（对齐 OnSetCursor 的选中标注手柄/悬停分支）
                        let annHandle = scHitTestAnnotationHandle(annotations[selectedAnnotation],
                                                                  mouse, SC.handleSize)
                        if annHandle != .none {
                            cursor = scHandleCursor(annHandle)
                        } else if scHitTestAnnotation(annotations, mouse) >= 0 {
                            cursor = .openHand   // Windows IDC_SIZEALL（标注可拖动/选中）
                        } else {
                            cursor = confirmedFallbackCursor()
                        }
                    } else if scHitTestAnnotation(annotations, mouse) >= 0 {
                        cursor = .openHand   // 悬停任意标注（含文字）即可选中/拖动
                    } else {
                        cursor = confirmedFallbackCursor()
                    }
                }
            }
        default:
            cursor = .arrow
        }
        cursor.set()
    }

    /// 确认态选区内/外的兜底光标（对齐 OnSetCursor 末段）：选区内矢量/文字/马赛克工具激活
    /// → 十字；有标注内容 → 箭头（整体拖动禁用）；否则手型（可整体拖动）；选区外箭头。
    private func confirmedFallbackCursor() -> NSCursor {
        if selection.contains(mouse) {
            if let tool = activeTool, tool.isVectorTool || tool == .text || tool == .mosaic {
                return .crosshair
            }
            if !annotations.isEmpty {
                return .arrow
            }
            return .openHand   // Windows IDC_SIZEALL（可整体拖动）
        }
        return .arrow
    }
}

/// 手柄对应的系统光标（Windows HandleCursor 移植）。
/// macOS 差异：系统无对角 resize 光标（Windows IDC_SIZENWSE/IDC_SIZENESW），
/// 四角与圆角手柄以十字光标替代。
/// - Parameter handle: 手柄
/// - Returns: 对应 NSCursor
private func scHandleCursor(_ handle: ScreenshotResizeHandle) -> NSCursor {
    switch handle {
    case .left, .right:
        return .resizeLeftRight     // IDC_SIZEWE
    case .top, .bottom:
        return .resizeUpDown        // IDC_SIZENS
    case .arrowStart, .arrowEnd:
        return .openHand            // IDC_SIZEALL（箭头端点拖拽，固定四向箭头）
    case .topLeft, .bottomRight, .topRight, .bottomLeft,
         .cornerTL, .cornerTR, .cornerBL, .cornerBR:
        return .crosshair           // 对角 resize 光标的 macOS 替代
    default:
        return .arrow
    }
}

/// 矩形各分量取整（对齐 Windows 的 int RECT 语义：坐标恒为整数点）。
private func scIntegralRect(_ rect: CGRect) -> CGRect {
    return CGRect(x: rect.minX.rounded(), y: rect.minY.rounded(),
                  width: rect.width.rounded(), height: rect.height.rounded())
}
