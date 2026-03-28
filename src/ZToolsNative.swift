import Foundation
import Cocoa
import ApplicationServices

// C 风格回调函数类型（无参数）
public typealias ClipboardCallback = @convention(c) () -> Void

// C 风格回调函数类型（带JSON字符串参数）
public typealias WindowCallback = @convention(c) (UnsafePointer<CChar>?) -> Void

// 全局监控状态
private var clipboardMonitorQueue: DispatchQueue?
private var isClipboardMonitoring = false

// 窗口监控状态
private var windowMonitorObserver: NSObjectProtocol?
private var windowMonitorQueue: DispatchQueue?
private var isWindowMonitoring = false
private var lastBundleId: String = ""
private var lastProcessId: pid_t = 0

// MARK: - Clipboard Monitor

/// 启动剪贴板监控
/// - Parameter callback: 当剪贴板变化时调用的 C 回调函数
@_cdecl("startClipboardMonitor")
public func startClipboardMonitor(_ callback: ClipboardCallback?) {
    guard let callback = callback else {
        print("Error: callback is nil")
        return
    }

    // 防止重复启动
    guard !isClipboardMonitoring else {
        print("Warning: Clipboard monitor already running")
        return
    }

    isClipboardMonitoring = true
    let pasteboard = NSPasteboard.general
    var changeCount = pasteboard.changeCount

    // 创建专用队列
    clipboardMonitorQueue = DispatchQueue(label: "com.ztools.clipboard.monitor", qos: .utility)

    clipboardMonitorQueue?.async {
        print("Clipboard monitor started")

        while isClipboardMonitoring {
            usleep(500_000) // 0.5 秒检查一次

            let currentCount = pasteboard.changeCount
            if currentCount != changeCount {
                changeCount = currentCount

                // 只通知变化事件，不传递内容
                callback()
            }
        }

        print("Clipboard monitor stopped")
    }
}

/// 停止剪贴板监控
@_cdecl("stopClipboardMonitor")
public func stopClipboardMonitor() {
    isClipboardMonitoring = false
    clipboardMonitorQueue = nil
}

// MARK: - Window Management

/// 获取窗口标题（使用 Accessibility API）
private func getWindowTitle(for pid: pid_t) -> String {
    let app = AXUIElementCreateApplication(pid)
    var windowValue: AnyObject?
    
    // 获取应用的焦点窗口
    let result = AXUIElementCopyAttributeValue(app, kAXFocusedWindowAttribute as CFString, &windowValue)
    
    if result == .success, let window = windowValue {
        var titleValue: AnyObject?
        let titleResult = AXUIElementCopyAttributeValue(window as! AXUIElement, kAXTitleAttribute as CFString, &titleValue)
        
        if titleResult == .success, let title = titleValue as? String {
            return title
        }
    }
    
    return ""
}

/// 获取窗口边界（位置和尺寸）
private func getWindowBounds(for pid: pid_t) -> CGRect {
    let app = AXUIElementCreateApplication(pid)
    var windowValue: AnyObject?
    
    // 获取应用的焦点窗口
    let result = AXUIElementCopyAttributeValue(app, kAXFocusedWindowAttribute as CFString, &windowValue)
    
    if result == .success, let window = windowValue {
        var positionValue: AnyObject?
        var sizeValue: AnyObject?
        
        // 获取位置
        let posResult = AXUIElementCopyAttributeValue(window as! AXUIElement, kAXPositionAttribute as CFString, &positionValue)
        // 获取尺寸
        let sizeResult = AXUIElementCopyAttributeValue(window as! AXUIElement, kAXSizeAttribute as CFString, &sizeValue)
        
        var position = CGPoint.zero
        var size = CGSize.zero
        
        if posResult == .success, let posValue = positionValue {
            AXValueGetValue(posValue as! AXValue, .cgPoint, &position)
        }
        if sizeResult == .success, let szValue = sizeValue {
            AXValueGetValue(szValue as! AXValue, .cgSize, &size)
        }
        
        return CGRect(origin: position, size: size)
    }
    
    return .zero
}

/// 获取应用的.app格式名称（非本地化，英文名称）
private func getAppName(from app: NSRunningApplication) -> String {
    // 优先使用 bundle URL 获取实际的 .app 文件夹名称（非本地化）
    if let bundleURL = app.bundleURL {
        let appFileName = bundleURL.lastPathComponent
        // 如果已经包含.app后缀，直接返回
        if appFileName.hasSuffix(".app") {
            return appFileName
        }
        return "\(appFileName).app"
    }
    
    // 如果无法获取 bundleURL，回退到使用本地化名称
    if let localizedName = app.localizedName, !localizedName.isEmpty {
        return "\(localizedName).app"
    }
    
    // 默认返回 Unknown.app
    return "Unknown.app"
}

/// 获取当前激活窗口的信息（JSON 格式）
/// - Returns: JSON 字符串包含 appName、bundleId、title、app、x、y、width、height、appPath 和 pid，需要调用者 free
@_cdecl("getActiveWindow")
public func getActiveWindow() -> UnsafeMutablePointer<CChar>? {
    // 获取当前激活的应用
    guard let frontmostApp = NSWorkspace.shared.frontmostApplication else {
        return strdup("{\"error\":\"No frontmost application\"}")
    }

    let appName = frontmostApp.localizedName ?? "Unknown"
    let bundleId = frontmostApp.bundleIdentifier ?? "unknown.bundle.id"
    let pid = frontmostApp.processIdentifier
    let windowTitle = getWindowTitle(for: pid)
    let app = getAppName(from: frontmostApp)
    let appPath = frontmostApp.bundleURL?.path ?? ""
    let bounds = getWindowBounds(for: pid)

    // 构建 JSON 字符串
    let jsonString = """
    {"appName":"\(escapeJSON(appName))","bundleId":"\(escapeJSON(bundleId))","title":"\(escapeJSON(windowTitle))","app":"\(escapeJSON(app))","x":\(Int(bounds.origin.x)),"y":\(Int(bounds.origin.y)),"width":\(Int(bounds.size.width)),"height":\(Int(bounds.size.height)),"appPath":"\(escapeJSON(appPath))","pid":\(pid)}
    """

    return strdup(jsonString)
}

/// 根据 bundleId 激活应用窗口
/// - Parameter bundleId: 应用的 bundle identifier
/// - Returns: 是否激活成功 (1: 成功, 0: 失败)
@_cdecl("activateWindow")
public func activateWindow(_ bundleId: UnsafePointer<CChar>?) -> Int32 {
    guard let bundleId = bundleId else {
        return 0
    }

    let bundleIdString = String(cString: bundleId)

    // 查找并激活应用
    let runningApps = NSRunningApplication.runningApplications(withBundleIdentifier: bundleIdString)
    if let app = runningApps.first {
        let success = app.activate(options: [.activateAllWindows, .activateIgnoringOtherApps])
        return success ? 1 : 0
    }

    return 0
}

// MARK: - Window Monitor

/// 使用 Core Graphics API 获取当前激活的应用（最可靠）
private func getFrontmostAppUsingCG() -> (pid: pid_t, bundleId: String, appName: String, windowTitle: String, app: String, appPath: String, bounds: CGRect)? {
    // 获取所有窗口列表，按层级排序
    let options = CGWindowListOption(arrayLiteral: .optionOnScreenOnly, .excludeDesktopElements)
    guard let windowList = CGWindowListCopyWindowInfo(options, kCGNullWindowID) as? [[String: Any]] else {
        return nil
    }

    // 找到最前面的窗口（layer 最小）
    for window in windowList {
        // 跳过没有 owner PID 的窗口
        guard let pid = window[kCGWindowOwnerPID as String] as? pid_t,
              pid > 0 else {
            continue
        }

        // 跳过窗口层级为 0 的（通常是系统 UI）
        if let layer = window[kCGWindowLayer as String] as? Int, layer == 0 {
            // 获取该窗口所属的应用信息
            if let runningApp = NSRunningApplication(processIdentifier: pid) {
                // 只返回有UI的普通应用
                if runningApp.activationPolicy == .regular {
                    let bundleId = runningApp.bundleIdentifier ?? "unknown.bundle.id"
                    let appName = runningApp.localizedName ?? "Unknown"
                    let windowTitle = getWindowTitle(for: pid)
                    let app = getAppName(from: runningApp)
                    let appPath = runningApp.bundleURL?.path ?? ""
                    let bounds = getWindowBounds(for: pid)
                    return (pid: pid, bundleId: bundleId, appName: appName, windowTitle: windowTitle, app: app, appPath: appPath, bounds: bounds)
                }
            }
        }
    }

    return nil
}

/// 启动窗口激活监控（使用 Core Graphics API + 轮询）
/// - Parameter callback: 窗口切换时调用的回调，传递JSON字符串
@_cdecl("startWindowMonitor")
public func startWindowMonitor(_ callback: WindowCallback?) {
    guard let callback = callback else {
        print("Error: window callback is nil")
        return
    }

    // 防止重复启动
    guard !isWindowMonitoring else {
        print("Warning: Window monitor already running")
        return
    }

    isWindowMonitoring = true

    // 获取初始窗口并立即回调一次
    if let appInfo = getFrontmostAppUsingCG() {
        lastProcessId = appInfo.pid
        lastBundleId = appInfo.bundleId

        // 立即回调初始窗口状态
        let jsonString = """
        {"appName":"\(escapeJSON(appInfo.appName))","bundleId":"\(escapeJSON(appInfo.bundleId))","title":"\(escapeJSON(appInfo.windowTitle))","app":"\(escapeJSON(appInfo.app))","x":\(Int(appInfo.bounds.origin.x)),"y":\(Int(appInfo.bounds.origin.y)),"width":\(Int(appInfo.bounds.size.width)),"height":\(Int(appInfo.bounds.size.height)),"appPath":"\(escapeJSON(appInfo.appPath))","pid":\(appInfo.pid)}
        """
        jsonString.withCString { cString in
            callback(cString)
        }
    }

    // 创建专用队列进行轮询
    windowMonitorQueue = DispatchQueue(label: "com.ztools.window.monitor", qos: .utility)

    windowMonitorQueue?.async {
        print("Window monitor started")

        while isWindowMonitoring {
            usleep(500_000) // 每 0.5 秒检查一次（性能优化：减少 CPU 占用）

            // 使用 Core Graphics API 获取当前激活的应用
            guard let appInfo = getFrontmostAppUsingCG() else {
                continue
            }

            let currentPid = appInfo.pid
            let currentBundleId = appInfo.bundleId
            let appName = appInfo.appName
            let windowTitle = appInfo.windowTitle
            let app = appInfo.app
            let appPath = appInfo.appPath
            let bounds = appInfo.bounds

            // 检测到窗口切换（使用 PID 比较更可靠）
            if currentPid != lastProcessId {
                lastProcessId = currentPid
                lastBundleId = currentBundleId

                // 构建JSON字符串
                let jsonString = """
                {"appName":"\(escapeJSON(appName))","bundleId":"\(escapeJSON(currentBundleId))","title":"\(escapeJSON(windowTitle))","app":"\(escapeJSON(app))","x":\(Int(bounds.origin.x)),"y":\(Int(bounds.origin.y)),"width":\(Int(bounds.size.width)),"height":\(Int(bounds.size.height)),"appPath":"\(escapeJSON(appPath))","pid":\(currentPid)}
                """

                // 调用回调
                jsonString.withCString { cString in
                    callback(cString)
                }
            }
        }

        print("Window monitor stopped")
    }
}

/// 停止窗口激活监控
@_cdecl("stopWindowMonitor")
public func stopWindowMonitor() {
    guard isWindowMonitoring else { return }

    isWindowMonitoring = false
    windowMonitorQueue = nil
    lastBundleId = ""
    lastProcessId = 0

    // 清理观察者（如果有的话）
    if let observer = windowMonitorObserver {
        NSWorkspace.shared.notificationCenter.removeObserver(observer)
        windowMonitorObserver = nil
    }

    print("Window monitor stopped")
}

// MARK: - Keyboard Simulation

/// 模拟粘贴操作（Command + V）
/// - Returns: 是否成功 (1: 成功, 0: 失败)
@_cdecl("simulatePaste")
public func simulatePaste() -> Int32 {
    // 检查辅助功能权限
    let options: NSDictionary = [kAXTrustedCheckOptionPrompt.takeUnretainedValue() as String: true]
    let accessEnabled = AXIsProcessTrustedWithOptions(options)

    if !accessEnabled {
        print("Error: Accessibility permission not granted")
        return 0
    }

    // V 键的 keyCode
    let vKeyCode: CGKeyCode = 9

    // 创建事件源
    guard let eventSource = CGEventSource(stateID: .hidSystemState) else {
        print("Error: Failed to create event source")
        return 0
    }

    // 1. 按下 Command+V 键
    guard let cmdDownEvent = CGEvent(keyboardEventSource: eventSource, virtualKey: vKeyCode, keyDown: true) else {
        return 0
    }
    cmdDownEvent.flags = .maskCommand

    // 2. 释放 V 键（带 Command 修饰符）
    guard let cmdUpEvent = CGEvent(keyboardEventSource: eventSource, virtualKey: vKeyCode, keyDown: false) else {
        return 0
    }
    cmdUpEvent.flags = .maskCommand

    // 发送事件
    cmdDownEvent.post(tap: .cghidEventTap)

    // 短暂延迟（10毫秒）
    usleep(10_000)

    cmdUpEvent.post(tap: .cghidEventTap)

    print("Paste simulation executed")
    return 1
}

/// 将键名转换为 macOS keyCode
private func getKeyCode(for key: String) -> CGKeyCode? {
    let keyMap: [String: CGKeyCode] = [
        // 字母键
        "a": 0, "b": 11, "c": 8, "d": 2, "e": 14, "f": 3, "g": 5, "h": 4,
        "i": 34, "j": 38, "k": 40, "l": 37, "m": 46, "n": 45, "o": 31,
        "p": 35, "q": 12, "r": 15, "s": 1, "t": 17, "u": 32, "v": 9,
        "w": 13, "x": 7, "y": 16, "z": 6,

        // 数字键
        "0": 29, "1": 18, "2": 19, "3": 20, "4": 21, "5": 23,
        "6": 22, "7": 26, "8": 28, "9": 25,

        // 功能键
        "f1": 122, "f2": 120, "f3": 99, "f4": 118, "f5": 96, "f6": 97,
        "f7": 98, "f8": 100, "f9": 101, "f10": 109, "f11": 103, "f12": 111,

        // 特殊键
        "return": 36, "enter": 36, "tab": 48, "space": 49, "delete": 51,
        "escape": 53, "esc": 53, "backspace": 51,

        // 方向键
        "left": 123, "right": 124, "down": 125, "up": 126,

        // 其他键
        "minus": 27, "-": 27,
        "equal": 24, "=": 24,
        "leftbracket": 33, "[": 33,
        "rightbracket": 30, "]": 30,
        "backslash": 42, "\\": 42,
        "semicolon": 41, ";": 41,
        "quote": 39, "'": 39,
        "comma": 43, ",": 43,
        "period": 47, ".": 47,
        "slash": 44, "/": 44,
        "grave": 50, "`": 50
    ]

    return keyMap[key.lowercased()]
}

/// 模拟键盘按键
/// - Parameters:
///   - key: 要按的键
///   - modifiers: 修饰键字符串（逗号分隔，如 "shift,ctrl" 或空字符串）
/// - Returns: 是否成功 (1: 成功, 0: 失败)
@_cdecl("simulateKeyboardTap")
public func simulateKeyboardTap(_ key: UnsafePointer<CChar>?, _ modifiers: UnsafePointer<CChar>?) -> Int32 {
    // 检查辅助功能权限
    let options: NSDictionary = [kAXTrustedCheckOptionPrompt.takeUnretainedValue() as String: true]
    let accessEnabled = AXIsProcessTrustedWithOptions(options)

    if !accessEnabled {
        print("Error: Accessibility permission not granted")
        return 0
    }

    guard let key = key else {
        print("Error: key is nil")
        return 0
    }

    let keyString = String(cString: key)

    // 获取键码
    guard let keyCode = getKeyCode(for: keyString) else {
        print("Error: Unknown key '\(keyString)'")
        return 0
    }

    // 解析修饰键
    var flags = CGEventFlags()
    if let modifiers = modifiers {
        let modifiersString = String(cString: modifiers)
        if !modifiersString.isEmpty {
            let modifierList = modifiersString.split(separator: ",").map { $0.trimmingCharacters(in: .whitespaces).lowercased() }

            for modifier in modifierList {
                switch modifier {
                case "shift":
                    flags.insert(.maskShift)
                case "ctrl", "control":
                    flags.insert(.maskControl)
                case "alt", "option":
                    flags.insert(.maskAlternate)
                case "meta", "cmd", "command":
                    flags.insert(.maskCommand)
                default:
                    print("Warning: Unknown modifier '\(modifier)'")
                }
            }
        }
    }

    // 创建事件源
    guard let eventSource = CGEventSource(stateID: .hidSystemState) else {
        print("Error: Failed to create event source")
        return 0
    }

    // 按下键
    guard let keyDownEvent = CGEvent(keyboardEventSource: eventSource, virtualKey: keyCode, keyDown: true) else {
        print("Error: Failed to create key down event")
        return 0
    }
    keyDownEvent.flags = flags

    // 释放键
    guard let keyUpEvent = CGEvent(keyboardEventSource: eventSource, virtualKey: keyCode, keyDown: false) else {
        print("Error: Failed to create key up event")
        return 0
    }
    keyUpEvent.flags = flags

    // 发送事件
    keyDownEvent.post(tap: .cghidEventTap)
    usleep(10_000) // 10ms 延迟
    keyUpEvent.post(tap: .cghidEventTap)

    print("Keyboard tap simulation executed: \(keyString) with modifiers: \(modifiers != nil ? String(cString: modifiers!) : "none")")
    return 1
}

// MARK: - Clipboard Files

/// 设置剪贴板中的文件列表（文件路径以换行符分隔的 C 字符串）
/// - Parameter pathsCStr: 以换行符分隔的文件路径列表
/// - Returns: 1 成功，0 失败
@_cdecl("setClipboardFiles")
public func setClipboardFiles(_ pathsCStr: UnsafePointer<CChar>?) -> Int32 {
    guard let pathsCStr = pathsCStr else { return 0 }
    let pathsString = String(cString: pathsCStr)
    let paths = pathsString.split(separator: "\n").map(String.init)
    guard !paths.isEmpty else { return 0 }

    let pasteboard = NSPasteboard.general
    pasteboard.clearContents()

    var urls: [URL] = []
    for p in paths {
        let url = URL(fileURLWithPath: p)
        // 验证文件存在
        if FileManager.default.fileExists(atPath: p) {
            urls.append(url)
        }
    }

    guard !urls.isEmpty else { return 0 }

    let success = pasteboard.writeObjects(urls as [NSPasteboardWriting])
    return success ? 1 : 0
}

// MARK: - Unicode Type

@_cdecl("unicodeType")
public func unicodeType(_ text: UnsafePointer<CChar>?) -> Int32 {
    guard let text = text else { return 0 }
    let str = String(cString: text)
    guard !str.isEmpty else { return 0 }

    guard let eventSource = CGEventSource(stateID: .hidSystemState) else { return 0 }

    let chars = Array(str.utf16)
    guard let downEvent = CGEvent(keyboardEventSource: eventSource, virtualKey: 0, keyDown: true) else { return 0 }
    downEvent.keyboardSetUnicodeString(stringLength: chars.count, unicodeString: chars)
    downEvent.post(tap: .cghidEventTap)

    usleep(10_000) // 10ms 延迟

    guard let upEvent = CGEvent(keyboardEventSource: eventSource, virtualKey: 0, keyDown: false) else { return 0 }
    upEvent.post(tap: .cghidEventTap)

    return 1
}

// MARK: - Mouse Monitor

public typealias MouseCallback = @convention(c) (UnsafePointer<CChar>?) -> Void

private var mouseMonitorCallback: MouseCallback? = nil
private var mouseEventTap: CFMachPort? = nil
private var mouseRunLoopSource: CFRunLoopSource? = nil
private var mouseMonitorRunLoop: CFRunLoop? = nil
private var isMouseMonitoring = false

// 当前监听配置
private var mouseButtonType: String = ""   // "left", "middle", "right", "back", "forward"
private var mouseLongPressMs: Int = 0      // 0=点击, >0=长按阈值
private var mouseIsLongPress: Bool = false // 是否为长按模式
private var mouseEventTypeName: String = "" // 回调事件名（如 "middleClick", "backLongPress"）

// 按钮目标编号（CGEvent buttonNumber: 0=left, 1=right, 2=middle, 3=back, 4=forward）
private var mouseTargetButton: Int64 = -1
private var mouseTargetIsRight: Bool = false // 是否监听右键（右键事件类型不同）
private var mouseTargetIsLeft: Bool = false  // 是否监听左键（左键事件类型不同）

// 按钮状态（通过 mouseLock 保护跨线程访问）
private var mouseLock = NSLock()
private var btnIsDown = false
private var btnLongPressFired = false
private var btnTimer: DispatchWorkItem? = nil
private var storedMouseDownEvent: CGEvent? = nil // 存储原始 mouseDown 用于重放
private var storedMouseUpEvent: CGEvent? = nil   // 存储原始 mouseUp 用于可能的重放
private var mouseReplayOnRelease = false         // 长按模式下标记是否在释放时重放

private let mouseTimerQueue = DispatchQueue(label: "com.ztools.mouse.timer", qos: .userInteractive)

// 重放标记：用于识别我们重新注入的事件，避免二次拦截
private let MOUSE_REPLAY_MARKER: Int64 = 0x5A544F4F4C53

private func notifyMouseEvent() {
    guard let callback = mouseMonitorCallback else { return }
    mouseEventTypeName.withCString { cStr in
        callback(cStr)
    }
}

/// CGEventTap 回调函数（拦截模式）
func mouseEventTapHandler(_ proxy: CGEventTapProxy, _ type: CGEventType, _ event: CGEvent, _ userInfo: UnsafeMutableRawPointer?) -> Unmanaged<CGEvent>? {
    guard isMouseMonitoring else {
        return Unmanaged.passUnretained(event)
    }

    // 处理系统禁用事件（tap 超时被系统关闭时重新启用）
    if type.rawValue == 0xFFFFFFFE || type.rawValue == 0xFFFFFFFF {
        if let tap = mouseEventTap {
            CGEvent.tapEnable(tap: tap, enable: true)
        }
        return Unmanaged.passUnretained(event)
    }

    // 跳过重放事件（我们自己注入的带标记的事件，直接放行）
    if event.getIntegerValueField(.eventSourceUserData) == MOUSE_REPLAY_MARKER {
        return Unmanaged.passUnretained(event)
    }

    // 判断是否是目标按钮事件
    var isTargetDown = false
    var isTargetUp = false

    if mouseTargetIsLeft {
        isTargetDown = (type == .leftMouseDown)
        isTargetUp = (type == .leftMouseUp)
    } else if mouseTargetIsRight {
        isTargetDown = (type == .rightMouseDown)
        isTargetUp = (type == .rightMouseUp)
    } else {
        if type == .otherMouseDown {
            isTargetDown = (event.getIntegerValueField(.mouseEventButtonNumber) == mouseTargetButton)
        } else if type == .otherMouseUp {
            isTargetUp = (event.getIntegerValueField(.mouseEventButtonNumber) == mouseTargetButton)
        }
    }

    // 非目标事件，放行
    if !isTargetDown && !isTargetUp {
        return Unmanaged.passUnretained(event)
    }

    // ── 目标按钮按下 ──
    if isTargetDown {
        mouseLock.lock()
        btnIsDown = true
        btnLongPressFired = false
        storedMouseDownEvent = event.copy()
        btnTimer?.cancel()

        if mouseIsLongPress {
            let timer = DispatchWorkItem {
                mouseLock.lock()
                guard btnIsDown else {
                    mouseLock.unlock()
                    return
                }
                btnLongPressFired = true
                mouseLock.unlock()
                notifyMouseEvent()
            }
            btnTimer = timer
            mouseTimerQueue.asyncAfter(deadline: .now() + .milliseconds(mouseLongPressMs), execute: timer)
        }
        mouseLock.unlock()
        return nil // 拦截 mouseDown，阻止默认行为
    }

    // ── 目标按钮释放 ──
    if isTargetUp {
        mouseLock.lock()
        let wasDown = btnIsDown
        let longPressFired = btnLongPressFired
        let replayOnRelease = mouseReplayOnRelease
        let storedDown = storedMouseDownEvent
        btnTimer?.cancel()
        btnTimer = nil
        btnIsDown = false
        btnLongPressFired = false
        mouseReplayOnRelease = false

        if mouseIsLongPress || !wasDown {
            // 长按模式或非配对事件，清理存储的事件
            storedMouseDownEvent = nil
        }
        // 点击模式 + wasDown: 保留 storedMouseDownEvent 用于可能的重放

        mouseLock.unlock()

        if !wasDown {
            return Unmanaged.passUnretained(event) // 非配对事件，放行
        }

        if mouseIsLongPress {
            if longPressFired {
                if replayOnRelease {
                    // JS 回调返回 {shouldBlock: false}，重放事件
                    if let downEvent = storedDown {
                        downEvent.setIntegerValueField(.eventSourceUserData, value: MOUSE_REPLAY_MARKER)
                        downEvent.post(tap: .cgSessionEventTap)
                    }
                    if let upEvent = event.copy() {
                        upEvent.setIntegerValueField(.eventSourceUserData, value: MOUSE_REPLAY_MARKER)
                        upEvent.post(tap: .cgSessionEventTap)
                    }
                }
                // 长按已触发回调，吞掉 mouseUp（除非 replayOnRelease 已重放）
                return nil
            } else {
                // 未达到长按阈值，重放原始 mouseDown + mouseUp（恢复默认行为）
                if let downEvent = storedDown {
                    downEvent.setIntegerValueField(.eventSourceUserData, value: MOUSE_REPLAY_MARKER)
                    downEvent.post(tap: .cgSessionEventTap)
                }
                if let upEvent = event.copy() {
                    upEvent.setIntegerValueField(.eventSourceUserData, value: MOUSE_REPLAY_MARKER)
                    upEvent.post(tap: .cgSessionEventTap)
                }
                return nil // 拦截当前 mouseUp，重放的带标记事件会被放行
            }
        } else {
            // 点击模式：存储 mouseUp 事件用于可能的重放，触发回调
            mouseLock.lock()
            storedMouseUpEvent = event.copy()
            mouseLock.unlock()
            notifyMouseEvent()
            return nil
        }
    }

    return Unmanaged.passUnretained(event)
}

/// 启动鼠标监控
/// - Parameters:
///   - buttonType: 按钮类型（"left", "middle", "right", "back", "forward"）
///   - longPressMs: 长按阈值（毫秒），0 表示监听点击，>0 表示监听长按
///   - callback: 事件回调，传递事件类型字符串
@_cdecl("startMouseMonitor")
public func startMouseMonitor(_ buttonType: UnsafePointer<CChar>?, _ longPressMs: Int32, _ callback: MouseCallback?) {
    guard let callback = callback, let buttonType = buttonType else {
        print("Error: mouse callback or buttonType is nil")
        return
    }

    guard !isMouseMonitoring else {
        print("Warning: Mouse monitor already running")
        return
    }

    let button = String(cString: buttonType)
    mouseButtonType = button
    mouseLongPressMs = Int(longPressMs)
    mouseIsLongPress = longPressMs > 0
    mouseMonitorCallback = callback
    isMouseMonitoring = true

    // 确定目标按钮和事件名
    switch button {
    case "left":
        mouseTargetButton = 0
        mouseTargetIsLeft = true
        mouseTargetIsRight = false
        mouseEventTypeName = "leftLongPress"
    case "middle":
        mouseTargetButton = 2
        mouseTargetIsLeft = false
        mouseTargetIsRight = false
        mouseEventTypeName = mouseIsLongPress ? "middleLongPress" : "middleClick"
    case "right":
        mouseTargetButton = 1
        mouseTargetIsLeft = false
        mouseTargetIsRight = true
        mouseEventTypeName = "rightLongPress"
    case "back":
        mouseTargetButton = 3
        mouseTargetIsLeft = false
        mouseTargetIsRight = false
        mouseEventTypeName = mouseIsLongPress ? "backLongPress" : "backClick"
    case "forward":
        mouseTargetButton = 4
        mouseTargetIsLeft = false
        mouseTargetIsRight = false
        mouseEventTypeName = mouseIsLongPress ? "forwardLongPress" : "forwardClick"
    default:
        print("Error: Unknown button type '\(button)'")
        isMouseMonitoring = false
        return
    }

    DispatchQueue.global(qos: .userInteractive).async {
        // 只监听目标按钮对应的事件类型
        var eventMask: CGEventMask = 0
        if mouseTargetIsLeft {
            eventMask = (1 << CGEventType.leftMouseDown.rawValue) |
                        (1 << CGEventType.leftMouseUp.rawValue)
        } else if mouseTargetIsRight {
            eventMask = (1 << CGEventType.rightMouseDown.rawValue) |
                        (1 << CGEventType.rightMouseUp.rawValue)
        } else {
            eventMask = (1 << CGEventType.otherMouseDown.rawValue) |
                        (1 << CGEventType.otherMouseUp.rawValue)
        }

        guard let tap = CGEvent.tapCreate(
            tap: .cgSessionEventTap,
            place: .headInsertEventTap,
            options: .defaultTap,
            eventsOfInterest: eventMask,
            callback: mouseEventTapHandler,
            userInfo: nil
        ) else {
            print("Error: Failed to create mouse event tap. Check accessibility permissions.")
            isMouseMonitoring = false
            return
        }

        mouseEventTap = tap

        guard let source = CFMachPortCreateRunLoopSource(kCFAllocatorDefault, tap, 0) else {
            print("Error: Failed to create run loop source")
            CFMachPortInvalidate(tap)
            mouseEventTap = nil
            isMouseMonitoring = false
            return
        }

        mouseRunLoopSource = source
        mouseMonitorRunLoop = CFRunLoopGetCurrent()
        CFRunLoopAddSource(mouseMonitorRunLoop!, source, .commonModes)
        CGEvent.tapEnable(tap: tap, enable: true)

        print("Mouse monitor started: \(mouseEventTypeName)")
        CFRunLoopRun()
        print("Mouse monitor run loop ended")
    }
}

/// 停止鼠标监控
@_cdecl("stopMouseMonitor")
public func stopMouseMonitor() {
    guard isMouseMonitoring else { return }

    isMouseMonitoring = false

    // 清理状态
    mouseLock.lock()
    btnTimer?.cancel()
    btnTimer = nil
    // 如果有未完成的按下事件，重放它以恢复默认行为
    if btnIsDown, let storedDown = storedMouseDownEvent {
        storedDown.setIntegerValueField(.eventSourceUserData, value: MOUSE_REPLAY_MARKER)
        storedDown.post(tap: .cgSessionEventTap)
    }
    btnIsDown = false
    btnLongPressFired = false
    storedMouseDownEvent = nil
    storedMouseUpEvent = nil
    mouseReplayOnRelease = false
    mouseLock.unlock()

    // 停止事件监听
    if let tap = mouseEventTap {
        CGEvent.tapEnable(tap: tap, enable: false)
    }

    if let source = mouseRunLoopSource, let runLoop = mouseMonitorRunLoop {
        CFRunLoopRemoveSource(runLoop, source, .commonModes)
    }

    if let runLoop = mouseMonitorRunLoop {
        CFRunLoopStop(runLoop)
    }

    if let tap = mouseEventTap {
        CFMachPortInvalidate(tap)
    }

    mouseRunLoopSource = nil
    mouseEventTap = nil
    mouseMonitorRunLoop = nil
    mouseMonitorCallback = nil

    print("Mouse monitor stopped")
}

/// 重放被拦截的鼠标事件（由 C++ 层在 JS 回调返回 {shouldBlock: false} 时调用）
@_cdecl("replayMouseEvents")
public func replayMouseEvents() {
    mouseLock.lock()
    let downEvent = storedMouseDownEvent
    let upEvent = storedMouseUpEvent

    if upEvent != nil {
        // 点击模式：mouseDown 和 mouseUp 都已存储，立即重放
        storedMouseDownEvent = nil
        storedMouseUpEvent = nil
        mouseLock.unlock()

        if let down = downEvent {
            down.setIntegerValueField(.eventSourceUserData, value: MOUSE_REPLAY_MARKER)
            down.post(tap: .cgSessionEventTap)
        }
        if let up = upEvent {
            up.setIntegerValueField(.eventSourceUserData, value: MOUSE_REPLAY_MARKER)
            up.post(tap: .cgSessionEventTap)
        }
    } else if btnIsDown {
        // 长按模式：按钮仍被按下，标记在释放时重放
        mouseReplayOnRelease = true
        mouseLock.unlock()
    } else {
        // 无可重放的事件
        storedMouseDownEvent = nil
        mouseLock.unlock()
    }
}

// MARK: - Color Picker

public typealias ColorPickerCallback = @convention(c) (UnsafePointer<CChar>?) -> Void

// 取色器状态
private var colorPickerCallback: ColorPickerCallback? = nil
private var colorPickerWindow: NSWindow? = nil
private var colorPickerView: ColorPickerGridView? = nil
private var isColorPickerActive = false
private var lastColorPickerUpdateTime: TimeInterval = 0
private let colorPickerUpdateInterval: TimeInterval = 1.0 / 30.0 // 30 FPS
private var colorPickerEventTap: CFMachPort? = nil
private var colorPickerRunLoopSource: CFRunLoopSource? = nil
private var colorPickerRunLoop: CFRunLoop? = nil

// Core Graphics 私有函数（用于从后台线程安全移动窗口）
// CGSMoveWindow 接受 CGPoint 指针（不是两个 float），直接与 WindowServer 通信，无线程限制
@_silgen_name("CGSMainConnectionID")
private func CGSMainConnectionID() -> Int32
@_silgen_name("CGSMoveWindow")
private func CGSMoveWindow(_ cid: Int32, _ wid: UInt32, _ point: inout CGPoint) -> Int32

/// 9x9 像素放大网格视图（使用 CALayer 直接绘制，线程安全）
private class ColorPickerGridView: NSView {
    var pixelColors: [[NSColor]] = []
    var centerHexColor: String = "#000000"
    let gridSize = 9
    let cellSize: CGFloat = 16
    let labelHeight: CGFloat = 28

    override var isFlipped: Bool { return true }

    override func draw(_ dirtyRect: NSRect) {
        super.draw(dirtyRect)

        guard let context = NSGraphicsContext.current?.cgContext else { return }

        let totalGridWidth = CGFloat(gridSize) * cellSize

        // 绘制背景（浅灰色边框效果）
        context.setFillColor(NSColor(white: 0.85, alpha: 1.0).cgColor)
        context.fill(CGRect(x: 0, y: 0, width: totalGridWidth, height: totalGridWidth))

        // 绘制 9x9 像素网格
        for row in 0..<gridSize {
            for col in 0..<gridSize {
                let color: NSColor
                if row < pixelColors.count && col < pixelColors[row].count {
                    color = pixelColors[row][col]
                } else {
                    color = .white
                }

                let rect = CGRect(
                    x: CGFloat(col) * cellSize,
                    y: CGFloat(row) * cellSize,
                    width: cellSize,
                    height: cellSize
                )

                // 填充像素颜色
                context.setFillColor(color.cgColor)
                context.fill(rect.insetBy(dx: 0.5, dy: 0.5))

                // 绘制网格线
                context.setStrokeColor(NSColor(white: 0.75, alpha: 0.5).cgColor)
                context.setLineWidth(0.5)
                context.stroke(rect)
            }
        }

        // 绘制中心十字准星（双层边框）
        let centerRect = CGRect(
            x: 4 * cellSize,
            y: 4 * cellSize,
            width: cellSize,
            height: cellSize
        )
        // 外层黑框
        context.setStrokeColor(NSColor.black.cgColor)
        context.setLineWidth(1.5)
        context.stroke(centerRect.insetBy(dx: -0.5, dy: -0.5))
        // 内层白框
        context.setStrokeColor(NSColor.white.cgColor)
        context.setLineWidth(1.0)
        context.stroke(centerRect.insetBy(dx: 0.5, dy: 0.5))

        // 绘制 HEX 标签区域
        let labelRect = CGRect(x: 0, y: totalGridWidth, width: totalGridWidth, height: labelHeight)
        context.setFillColor(NSColor(white: 0.15, alpha: 0.9).cgColor)
        context.fill(labelRect)

        // 绘制 HEX 文本
        let paragraphStyle = NSMutableParagraphStyle()
        paragraphStyle.alignment = .center
        let attributes: [NSAttributedString.Key: Any] = [
            .font: NSFont.monospacedSystemFont(ofSize: 13, weight: .medium),
            .foregroundColor: NSColor.white,
            .paragraphStyle: paragraphStyle
        ]
        let textRect = CGRect(x: 0, y: totalGridWidth + 5, width: totalGridWidth, height: labelHeight - 4)
        (centerHexColor as NSString).draw(in: textRect, withAttributes: attributes)
    }

    func updatePixels(_ colors: [[NSColor]], hex: String) {
        self.pixelColors = colors
        self.centerHexColor = hex
        self.needsDisplay = true
    }
}

/// 创建取色器浮动窗口（必须在有 NSApplication 的线程调用）
private func createColorPickerWindow() -> NSWindow {
    let gridSize = 9
    let cellSize: CGFloat = 16
    let totalGridWidth = CGFloat(gridSize) * cellSize  // 144
    let labelHeight: CGFloat = 28
    let windowWidth = totalGridWidth
    let windowHeight = totalGridWidth + labelHeight  // 172

    let window = NSWindow(
        contentRect: NSRect(x: 0, y: 0, width: windowWidth, height: windowHeight),
        styleMask: .borderless,
        backing: .buffered,
        defer: false
    )

    window.level = NSWindow.Level(rawValue: NSWindow.Level.screenSaver.rawValue + 1)
    window.isOpaque = false
    window.backgroundColor = .clear
    window.hasShadow = true
    window.ignoresMouseEvents = true
    window.collectionBehavior = [.canJoinAllSpaces, .fullScreenAuxiliary]
    window.isReleasedWhenClosed = false

    let view = ColorPickerGridView(frame: NSRect(x: 0, y: 0, width: windowWidth, height: windowHeight))
    window.contentView = view

    // 圆角
    view.wantsLayer = true
    view.layer?.cornerRadius = 6
    view.layer?.masksToBounds = true

    return window
}

/// 捕获鼠标周围的 9x9 像素颜色（可在任意线程调用）
private func capturePixelsAroundCursor() -> (colors: [[NSColor]], centerHex: String) {
    let gridSize = 9
    let halfGrid = gridSize / 2  // 4

    // 使用 CGEvent 获取鼠标位置（CG 坐标系：左上角原点）
    let cgMousePos = CGEvent(source: nil)?.location ?? .zero

    // 截取区域（逻辑坐标，9x9 点）
    let captureRect = CGRect(
        x: cgMousePos.x - CGFloat(halfGrid),
        y: cgMousePos.y - CGFloat(halfGrid),
        width: CGFloat(gridSize),
        height: CGFloat(gridSize)
    )

    // 使用 optionOnScreenBelowWindow 排除取色器自身窗口
    let windowID: CGWindowID
    if let win = colorPickerWindow {
        windowID = CGWindowID(win.windowNumber)
    } else {
        windowID = kCGNullWindowID
    }

    guard let cgImage = CGWindowListCreateImage(
        captureRect,
        .optionOnScreenBelowWindow,
        windowID,
        [.bestResolution]
    ) else {
        return (Array(repeating: Array(repeating: NSColor.white, count: gridSize), count: gridSize), "#000000")
    }

    let imageWidth = cgImage.width
    let imageHeight = cgImage.height

    // 创建位图上下文来读取像素
    let colorSpace = CGColorSpaceCreateDeviceRGB()
    let bytesPerPixel = 4
    let bytesPerRow = bytesPerPixel * imageWidth
    var pixelData = [UInt8](repeating: 0, count: imageWidth * imageHeight * bytesPerPixel)

    guard let bitmapContext = CGContext(
        data: &pixelData,
        width: imageWidth,
        height: imageHeight,
        bitsPerComponent: 8,
        bytesPerRow: bytesPerRow,
        space: colorSpace,
        bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue | CGBitmapInfo.byteOrder32Big.rawValue
    ) else {
        return (Array(repeating: Array(repeating: NSColor.white, count: gridSize), count: gridSize), "#000000")
    }

    bitmapContext.draw(cgImage, in: CGRect(x: 0, y: 0, width: imageWidth, height: imageHeight))

    var colors: [[NSColor]] = []
    var centerR: UInt8 = 0, centerG: UInt8 = 0, centerB: UInt8 = 0

    for row in 0..<gridSize {
        var rowColors: [NSColor] = []
        for col in 0..<gridSize {
            // 映射逻辑像素到物理像素
            let px = min(Int(CGFloat(col) * CGFloat(imageWidth) / CGFloat(gridSize)), imageWidth - 1)
            let py = min(Int(CGFloat(row) * CGFloat(imageHeight) / CGFloat(gridSize)), imageHeight - 1)

            let offset = (py * imageWidth + px) * bytesPerPixel
            let r = pixelData[offset]
            let g = pixelData[offset + 1]
            let b = pixelData[offset + 2]

            let color = NSColor(
                red: CGFloat(r) / 255.0,
                green: CGFloat(g) / 255.0,
                blue: CGFloat(b) / 255.0,
                alpha: 1.0
            )
            rowColors.append(color)

            if row == halfGrid && col == halfGrid {
                centerR = r
                centerG = g
                centerB = b
            }
        }
        colors.append(rowColors)
    }

    let hex = String(format: "#%02X%02X%02X", centerR, centerG, centerB)
    return (colors, hex)
}

/// 更新取色器位置和像素（从 CGEventTap 后台线程调用）
/// 窗口位置：CGSMoveWindow 直接与 WindowServer 通信，无 AppKit 线程限制
/// 视图内容：通过 CALayer 更新，避免从后台线程调用 NSView.display()
private func updateColorPicker() {
    guard isColorPickerActive else { return }

    guard let window = colorPickerWindow, let view = colorPickerView else { return }

    // 获取鼠标位置（NS 坐标系：左下角原点）
    let mouseLocation = NSEvent.mouseLocation

    let windowFrame = window.frame
    let offsetX: CGFloat = 20
    let offsetY: CGFloat = 20

    // 默认在鼠标右下方（NS 坐标系）
    var newOriginX = mouseLocation.x + offsetX
    var newOriginY = mouseLocation.y - offsetY - windowFrame.height

    // 屏幕边界检测
    let screen = NSScreen.screens.first(where: { NSMouseInRect(mouseLocation, $0.frame, false) }) ?? NSScreen.main ?? NSScreen.screens[0]
    let screenFrame = screen.visibleFrame

    if newOriginX + windowFrame.width > screenFrame.maxX {
        newOriginX = mouseLocation.x - offsetX - windowFrame.width
    }
    if newOriginY < screenFrame.minY {
        newOriginY = mouseLocation.y + offsetY
    }
    if newOriginX < screenFrame.minX {
        newOriginX = screenFrame.minX
    }

    // CGSMoveWindow 使用 CG 屏幕坐标系（左上角原点），需要从 NS 坐标系翻转 Y 轴
    // NS: origin = 窗口左下角, Y 向上
    // CG: origin = 窗口左上角, Y 向下
    let mainScreenHeight = NSScreen.screens[0].frame.height
    var cgsPoint = CGPoint(
        x: newOriginX,
        y: mainScreenHeight - newOriginY - windowFrame.height
    )

    let cid = CGSMainConnectionID()
    let wid = UInt32(window.windowNumber)
    _ = CGSMoveWindow(cid, wid, &cgsPoint)

    // 帧率节流：限制截屏频率
    let now = CACurrentMediaTime()
    if now - lastColorPickerUpdateTime >= colorPickerUpdateInterval {
        lastColorPickerUpdateTime = now

        // 截屏并更新像素（CGWindowListCreateImage 是线程安全的）
        let (colors, hex) = capturePixelsAroundCursor()

        // 通过 CALayer 更新视图内容（Core Animation 事务是线程安全的）
        // 关键：不能从后台线程调用 NSView.display()，那会导致窗口消失
        CATransaction.begin()
        CATransaction.setDisableActions(true)
        view.pixelColors = colors
        view.centerHexColor = hex
        view.layer?.setNeedsDisplay()
        view.layer?.displayIfNeeded()
        CATransaction.commit()
    }
}

/// CGEventTap 回调（拦截模式）— 在 colorPickerThread 的 RunLoop 中被调用
private func colorPickerEventTapHandler(_ proxy: CGEventTapProxy, _ type: CGEventType, _ event: CGEvent, _ userInfo: UnsafeMutableRawPointer?) -> Unmanaged<CGEvent>? {
    guard isColorPickerActive else {
        return Unmanaged.passUnretained(event)
    }

    // 处理系统禁用事件（tap 超时被系统关闭时重新启用）
    if type.rawValue == 0xFFFFFFFE || type.rawValue == 0xFFFFFFFF {
        if let tap = colorPickerEventTap {
            CGEvent.tapEnable(tap: tap, enable: true)
        }
        return Unmanaged.passUnretained(event)
    }

    switch type {
    case .mouseMoved, .leftMouseDragged, .rightMouseDragged, .otherMouseDragged:
        // 鼠标移动 → 更新取色器（在当前线程直接更新，该线程拥有 NSApp RunLoop）
        updateColorPicker()
        return Unmanaged.passUnretained(event)

    case .leftMouseDown:
        // 左键点击 → 确认取色
        isColorPickerActive = false // 立即标记，防止后续 mouseMoved 继续更新
        let (_, hex) = capturePixelsAroundCursor()
        let jsonString = "{\"success\":true,\"hex\":\"\(hex)\"}"
        jsonString.withCString { cStr in
            colorPickerCallback?(cStr)
        }
        // 只停事件 tap，不碰窗口（窗口清理由主线程的 stopColorPicker 负责）
        stopColorPickerEventTap()
        return nil // 拦截点击事件

    case .keyDown:
        // ESC 键 → 取消
        let keyCode = event.getIntegerValueField(.keyboardEventKeycode)
        if keyCode == 53 { // ESC
            isColorPickerActive = false
            let jsonString = "{\"success\":false,\"hex\":null}"
            jsonString.withCString { cStr in
                colorPickerCallback?(cStr)
            }
            stopColorPickerEventTap()
            return nil // 拦截 ESC
        }
        return Unmanaged.passUnretained(event)

    default:
        return Unmanaged.passUnretained(event)
    }
}

/// 启动取色器
@_cdecl("startColorPicker")
public func startColorPicker(_ callback: ColorPickerCallback?) {
    guard let callback = callback else {
        print("Error: color picker callback is nil")
        return
    }

    guard !isColorPickerActive else {
        print("Warning: Color picker already active")
        return
    }

    colorPickerCallback = callback
    isColorPickerActive = true

    // 确保 NSApplication 完整初始化（N-API 调用在主线程上）
    let app = NSApplication.shared
    app.setActivationPolicy(.accessory)
    app.finishLaunching()

    // 在主线程创建窗口（AppKit 要求 NSWindow 必须在主线程创建）
    let window = createColorPickerWindow()
    colorPickerWindow = window
    colorPickerView = window.contentView as? ColorPickerGridView

    // 初始定位
    do {
        let mouseLocation = NSEvent.mouseLocation
        let windowFrame = window.frame
        let offsetX: CGFloat = 20
        let offsetY: CGFloat = 20
        var newOrigin = NSPoint(
            x: mouseLocation.x + offsetX,
            y: mouseLocation.y - offsetY - windowFrame.height
        )
        let screen = NSScreen.screens.first(where: { NSMouseInRect(mouseLocation, $0.frame, false) }) ?? NSScreen.main ?? NSScreen.screens[0]
        let screenFrame = screen.visibleFrame
        if newOrigin.x + windowFrame.width > screenFrame.maxX {
            newOrigin.x = mouseLocation.x - offsetX - windowFrame.width
        }
        if newOrigin.y < screenFrame.minY {
            newOrigin.y = mouseLocation.y + offsetY
        }
        if newOrigin.x < screenFrame.minX {
            newOrigin.x = screenFrame.minX
        }
        window.setFrameOrigin(newOrigin)

        let (colors, hex) = capturePixelsAroundCursor()
        colorPickerView?.updatePixels(colors, hex: hex)
        colorPickerView?.display()
    }

    // 显示窗口
    window.orderFrontRegardless()
    window.display()
    NSApp.activate(ignoringOtherApps: true)

    // 手动泵 AppKit 事件循环，确保窗口提交到 WindowServer
    // Node.js 主线程不运行 NSRunLoop，必须手动处理一次才能让窗口真正显示
    while let event = NSApp.nextEvent(
        matching: .any,
        until: nil,
        inMode: .default,
        dequeue: true
    ) {
        NSApp.sendEvent(event)
    }

    print("Color picker window created on main thread")

    // 在后台线程启动 CGEventTap
    DispatchQueue.global(qos: .userInteractive).async {
        let eventMask: CGEventMask =
            (1 << CGEventType.mouseMoved.rawValue) |
            (1 << CGEventType.leftMouseDragged.rawValue) |
            (1 << CGEventType.rightMouseDragged.rawValue) |
            (1 << CGEventType.otherMouseDragged.rawValue) |
            (1 << CGEventType.leftMouseDown.rawValue) |
            (1 << CGEventType.keyDown.rawValue)

        guard let tap = CGEvent.tapCreate(
            tap: .cgSessionEventTap,
            place: .headInsertEventTap,
            options: .defaultTap,
            eventsOfInterest: eventMask,
            callback: { (proxy, type, event, userInfo) -> Unmanaged<CGEvent>? in
                return colorPickerEventTapHandler(proxy, type, event, userInfo)
            },
            userInfo: nil
        ) else {
            print("Error: Failed to create color picker event tap. Check accessibility permissions.")
            isColorPickerActive = false
            return
        }

        colorPickerEventTap = tap

        guard let source = CFMachPortCreateRunLoopSource(kCFAllocatorDefault, tap, 0) else {
            print("Error: Failed to create run loop source for color picker")
            CFMachPortInvalidate(tap)
            colorPickerEventTap = nil
            isColorPickerActive = false
            return
        }

        colorPickerRunLoopSource = source
        colorPickerRunLoop = CFRunLoopGetCurrent()
        CFRunLoopAddSource(colorPickerRunLoop!, source, .commonModes)
        CGEvent.tapEnable(tap: tap, enable: true)

        print("Color picker event tap started")
        CFRunLoopRun()
        print("Color picker event tap run loop ended")
    }

    print("Color picker started")
}

/// 停止取色器（从 N-API 主线程调用，负责窗口清理）
@_cdecl("stopColorPicker")
public func stopColorPicker() {
    // 停止事件监听（如果还没停的话）
    if isColorPickerActive {
        isColorPickerActive = false
        stopColorPickerEventTap()
    }

    // 窗口清理（在主线程执行，安全）
    colorPickerWindow?.orderOut(nil)
    colorPickerWindow = nil
    colorPickerView = nil
    colorPickerCallback = nil

    print("Color picker stopped")
}

/// 只停止事件监听（可从任何线程安全调用，不操作 NSWindow）
private func stopColorPickerEventTap() {
    if let tap = colorPickerEventTap {
        CGEvent.tapEnable(tap: tap, enable: false)
    }

    if let source = colorPickerRunLoopSource, let runLoop = colorPickerRunLoop {
        CFRunLoopRemoveSource(runLoop, source, .commonModes)
    }

    if let tap = colorPickerEventTap {
        CFMachPortInvalidate(tap)
    }

    if let runLoop = colorPickerRunLoop {
        CFRunLoopStop(runLoop)
    }

    colorPickerRunLoopSource = nil
    colorPickerEventTap = nil
    colorPickerRunLoop = nil
}

// MARK: - Helper Functions

// MARK: - Mouse Simulation

/// 模拟鼠标移动到指定屏幕位置
/// - Parameters:
///   - x: 距离屏幕左侧的位置（像素）
///   - y: 距离屏幕顶部的位置（像素）
/// - Returns: 是否成功 (1: 成功, 0: 失败)
@_cdecl("simulateMouseMove")
public func simulateMouseMove(_ x: Double, _ y: Double) -> Int32 {
    guard let eventSource = CGEventSource(stateID: .hidSystemState) else {
        print("Error: Failed to create event source")
        return 0
    }

    let point = CGPoint(x: x, y: y)
    guard let moveEvent = CGEvent(mouseEventSource: eventSource, mouseType: .mouseMoved, mouseCursorPosition: point, mouseButton: .left) else {
        print("Error: Failed to create mouse move event")
        return 0
    }

    moveEvent.post(tap: .cghidEventTap)
    return 1
}

/// 模拟鼠标左键单击
/// - Parameters:
///   - x: 距离屏幕左侧的位置（像素）
///   - y: 距离屏幕顶部的位置（像素）
/// - Returns: 是否成功 (1: 成功, 0: 失败)
@_cdecl("simulateMouseClick")
public func simulateMouseClick(_ x: Double, _ y: Double) -> Int32 {
    guard let eventSource = CGEventSource(stateID: .hidSystemState) else {
        print("Error: Failed to create event source")
        return 0
    }

    let point = CGPoint(x: x, y: y)

    guard let downEvent = CGEvent(mouseEventSource: eventSource, mouseType: .leftMouseDown, mouseCursorPosition: point, mouseButton: .left) else {
        return 0
    }
    guard let upEvent = CGEvent(mouseEventSource: eventSource, mouseType: .leftMouseUp, mouseCursorPosition: point, mouseButton: .left) else {
        return 0
    }

    downEvent.post(tap: .cghidEventTap)
    usleep(10_000)
    upEvent.post(tap: .cghidEventTap)
    return 1
}

/// 模拟鼠标左键双击
/// - Parameters:
///   - x: 距离屏幕左侧的位置（像素）
///   - y: 距离屏幕顶部的位置（像素）
/// - Returns: 是否成功 (1: 成功, 0: 失败)
@_cdecl("simulateMouseDoubleClick")
public func simulateMouseDoubleClick(_ x: Double, _ y: Double) -> Int32 {
    guard let eventSource = CGEventSource(stateID: .hidSystemState) else {
        print("Error: Failed to create event source")
        return 0
    }

    let point = CGPoint(x: x, y: y)

    // 第一次点击
    guard let down1 = CGEvent(mouseEventSource: eventSource, mouseType: .leftMouseDown, mouseCursorPosition: point, mouseButton: .left) else {
        return 0
    }
    guard let up1 = CGEvent(mouseEventSource: eventSource, mouseType: .leftMouseUp, mouseCursorPosition: point, mouseButton: .left) else {
        return 0
    }
    down1.setIntegerValueField(.mouseEventClickState, value: 1)
    up1.setIntegerValueField(.mouseEventClickState, value: 1)

    // 第二次点击（clickState = 2 表示双击）
    guard let down2 = CGEvent(mouseEventSource: eventSource, mouseType: .leftMouseDown, mouseCursorPosition: point, mouseButton: .left) else {
        return 0
    }
    guard let up2 = CGEvent(mouseEventSource: eventSource, mouseType: .leftMouseUp, mouseCursorPosition: point, mouseButton: .left) else {
        return 0
    }
    down2.setIntegerValueField(.mouseEventClickState, value: 2)
    up2.setIntegerValueField(.mouseEventClickState, value: 2)

    down1.post(tap: .cghidEventTap)
    usleep(10_000)
    up1.post(tap: .cghidEventTap)
    usleep(10_000)
    down2.post(tap: .cghidEventTap)
    usleep(10_000)
    up2.post(tap: .cghidEventTap)
    return 1
}

/// 模拟鼠标右键单击
/// - Parameters:
///   - x: 距离屏幕左侧的位置（像素）
///   - y: 距离屏幕顶部的位置（像素）
/// - Returns: 是否成功 (1: 成功, 0: 失败)
@_cdecl("simulateMouseRightClick")
public func simulateMouseRightClick(_ x: Double, _ y: Double) -> Int32 {
    guard let eventSource = CGEventSource(stateID: .hidSystemState) else {
        print("Error: Failed to create event source")
        return 0
    }

    let point = CGPoint(x: x, y: y)

    guard let downEvent = CGEvent(mouseEventSource: eventSource, mouseType: .rightMouseDown, mouseCursorPosition: point, mouseButton: .right) else {
        return 0
    }
    guard let upEvent = CGEvent(mouseEventSource: eventSource, mouseType: .rightMouseUp, mouseCursorPosition: point, mouseButton: .right) else {
        return 0
    }

    downEvent.post(tap: .cghidEventTap)
    usleep(10_000)
    upEvent.post(tap: .cghidEventTap)
    return 1
}

// MARK: - Helper Functions (Utilities)

/// 辅助函数：转义 JSON 字符串
private func escapeJSON(_ string: String) -> String {
    return string
        .replacingOccurrences(of: "\\", with: "\\\\")
        .replacingOccurrences(of: "\"", with: "\\\"")
        .replacingOccurrences(of: "\n", with: "\\n")
        .replacingOccurrences(of: "\r", with: "\\r")
        .replacingOccurrences(of: "\t", with: "\\t")
}
