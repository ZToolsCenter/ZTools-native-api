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

// MARK: - Helper Functions

/// 辅助函数：转义 JSON 字符串
private func escapeJSON(_ string: String) -> String {
    return string
        .replacingOccurrences(of: "\\", with: "\\\\")
        .replacingOccurrences(of: "\"", with: "\\\"")
        .replacingOccurrences(of: "\n", with: "\\n")
        .replacingOccurrences(of: "\r", with: "\\r")
        .replacingOccurrences(of: "\t", with: "\\t")
}
