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

/// 获取当前激活窗口的信息（JSON 格式）
/// - Returns: JSON 字符串包含 appName 和 bundleId，需要调用者 free
@_cdecl("getActiveWindow")
public func getActiveWindow() -> UnsafeMutablePointer<CChar>? {
    // 获取当前激活的应用
    guard let frontmostApp = NSWorkspace.shared.frontmostApplication else {
        return strdup("{\"error\":\"No frontmost application\"}")
    }

    let appName = frontmostApp.localizedName ?? "Unknown"
    let bundleId = frontmostApp.bundleIdentifier ?? "unknown.bundle.id"

    // 构建 JSON 字符串
    let jsonString = """
    {"appName":"\(escapeJSON(appName))","bundleId":"\(escapeJSON(bundleId))"}
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
private func getFrontmostAppUsingCG() -> (pid: pid_t, bundleId: String, appName: String)? {
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
            if let app = NSRunningApplication(processIdentifier: pid) {
                // 只返回有UI的普通应用
                if app.activationPolicy == .regular {
                    let bundleId = app.bundleIdentifier ?? "unknown.bundle.id"
                    let appName = app.localizedName ?? "Unknown"
                    return (pid: pid, bundleId: bundleId, appName: appName)
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

    // 获取初始窗口
    if let appInfo = getFrontmostAppUsingCG() {
        lastProcessId = appInfo.pid
        lastBundleId = appInfo.bundleId
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

            // 检测到窗口切换（使用 PID 比较更可靠）
            if currentPid != lastProcessId {
                lastProcessId = currentPid
                lastBundleId = currentBundleId

                // 构建JSON字符串
                let jsonString = """
                {"appName":"\(escapeJSON(appName))","bundleId":"\(escapeJSON(currentBundleId))"}
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
