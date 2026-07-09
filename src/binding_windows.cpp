#include <napi.h>
#include <windows.h>
#include <windowsx.h>  // For GET_X_LPARAM, GET_Y_LPARAM
#include <psapi.h>
#include <commctrl.h>      // For image list
#include <commoncontrols.h> // For IImageList
#include <thread>
#include <chrono>
#include <string>
#include <atomic>
#include <algorithm>   // For std::min, std::max
#include <map>         // For key mapping
#include <vector>      // For input events
#include <memory>      // For std::unique_ptr, std::addressof
#include <cstddef>
#include <cctype>
#include <cwchar>
#include <cwctype>
#include <mutex>
#include <condition_variable>
#include <set>
#include <sstream>
#include <shellapi.h>  // For DragQueryFile, SHGetFileInfoW
#include <shlobj.h>    // For SHLoadIndirectString, IApplicationActivationManager
#include <shobjidl.h>  // For IApplicationActivationManager
#include <exdisp.h>    // For IShellWindows, IWebBrowserApp (COM Explorer 路径查询)
#include <uiautomation.h> // For browser URL reading via UI Automation
#include <appmodel.h>  // For package APIs
#include <shlwapi.h>   // For PathCombineW
#include <dwmapi.h>    // For DwmGetWindowAttribute
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "uiautomationcore.lib")

#include "screenshot_windows.h"

// DWMWA_CLOAKED 在较新的 Windows SDK 中定义，为了兼容性手动定义
#ifndef DWMWA_CLOAKED
#define DWMWA_CLOAKED 14
#endif

// GDI+ 需要 min/max
namespace Gdiplus {
    using std::min;
    using std::max;
}
#include <gdiplus.h>
#pragma comment(lib, "gdiplus.lib")

// DROPFILES 已由 shlobj.h 提供，不再需要手动定义

// 取消与自定义函数名冲突的Windows宏
#ifdef GetActiveWindow
#undef GetActiveWindow
#endif

// 全局变量 - 剪贴板监控
static HWND g_hwnd = NULL;
static std::thread g_messageThread;
static std::atomic<bool> g_isMonitoring(false);
static std::atomic<bool> g_isPaused(false);  // 新增：暂停状态
static napi_threadsafe_function g_tsfn = nullptr;

// 剪贴板防抖：Edge 等浏览器复制时会分多次写入不同格式，
// 每次写入都触发 WM_CLIPBOARDUPDATE，使用定时器合并为一次回调
#define CLIPBOARD_DEBOUNCE_TIMER_ID 1
#define CLIPBOARD_DEBOUNCE_MS 100

// 全局变量 - 窗口监控
static HWINEVENTHOOK g_winEventHook = NULL;
static HWINEVENTHOOK g_winEventHookTitle = NULL;
static std::atomic<bool> g_isWindowMonitoring(false);
static napi_threadsafe_function g_windowTsfn = nullptr;
static std::thread g_windowMessageThread;
static HWND g_lastMonitoredWindow = NULL;
static std::string g_lastMonitoredTitle;

// 全局变量 - 鼠标监控
static HHOOK g_mouseHook = NULL;
static std::atomic<bool> g_isMouseMonitoring(false);
static napi_threadsafe_function g_mouseTsfn = nullptr;
static std::thread g_mouseMessageThread;
static std::string g_mouseButtonType;
static int g_mouseLongPressMs = 0;
static std::atomic<bool> g_mouseButtonPressed(false);
static std::chrono::steady_clock::time_point g_mousePressStartTime;
static std::atomic<bool> g_mouseLongPressTriggered(false);
static bool g_mouseNeedReplay = false;
static std::atomic<bool> g_mouseReplayOnRelease(false);
#define MOUSE_REPLAY_MAGIC 0x5A544F4F

// 全局变量 - 取色器
static HWND g_colorPickerWindow = NULL;
static std::atomic<bool> g_isColorPickerActive(false);
static napi_threadsafe_function g_colorPickerTsfn = nullptr;
static std::thread g_colorPickerThread;
static HDC g_colorPickerMemDC = NULL;
static HBITMAP g_colorPickerBitmap = NULL;
static std::string g_colorPickerResult;
static HHOOK g_colorPickerMouseHook = NULL;
static HHOOK g_colorPickerKeyboardHook = NULL;
static std::atomic<bool> g_colorPickerCallbackCalled(false);

struct OptimizedShortcutDefinition {
    std::string shortcut;
    WORD mainKey = 0;
    UINT hotkeyModifiers = 0;
};

struct OptimizedShortcutTrigger {
    std::string shortcut;
    bool primed = false;
};

struct OptimizedShortcutRegistrationResult {
    bool success = true;
    std::string error;
};

struct OptimizedShortcutRefreshRequest {
    std::string shortcut;
    bool shouldBeRegistered = true;
};

// 全局变量 - 优化截图快捷键监听
static std::atomic<bool> g_isOptimizedShortcutListening(false);
static napi_threadsafe_function g_optimizedShortcutTsfn = nullptr;
static std::thread g_optimizedShortcutThread;
static std::mutex g_optimizedShortcutMutex;
static std::vector<OptimizedShortcutDefinition> g_optimizedShortcuts;
static std::map<int, OptimizedShortcutDefinition> g_activeOptimizedShortcutIds;
static std::atomic<DWORD> g_optimizedShortcutThreadId(0);
static std::mutex g_optimizedShortcutRefreshResultMutex;
static std::condition_variable g_optimizedShortcutRefreshResultCv;
static bool g_optimizedShortcutRefreshPending = false;
static bool g_optimizedShortcutRefreshCompleted = false;
static OptimizedShortcutRefreshRequest g_optimizedShortcutRefreshRequest;
static OptimizedShortcutRegistrationResult g_optimizedShortcutRefreshResult;

// 窗口过程（处理剪贴板消息）
LRESULT CALLBACK ClipboardWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CLIPBOARDUPDATE:
            // 剪贴板变化，使用防抖：重置定时器，延迟触发回调
            // 这样如果短时间内收到多次 WM_CLIPBOARDUPDATE（如 Edge 复制地址栏），
            // 只会在最后一次变化后触发一次回调
            SetTimer(hwnd, CLIPBOARD_DEBOUNCE_TIMER_ID, CLIPBOARD_DEBOUNCE_MS, NULL);
            return 0;
        case WM_TIMER:
            if (wParam == CLIPBOARD_DEBOUNCE_TIMER_ID) {
                KillTimer(hwnd, CLIPBOARD_DEBOUNCE_TIMER_ID);
                // 仅在未暂停时触发回调
                if (g_tsfn != nullptr && !g_isPaused) {
                    napi_call_threadsafe_function(g_tsfn, nullptr, napi_tsfn_nonblocking);
                }
            }
            return 0;
        case WM_DESTROY:
            KillTimer(hwnd, CLIPBOARD_DEBOUNCE_TIMER_ID);
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

// 在主线程调用 JS 回调
void CallJs(napi_env env, napi_value js_callback, void* context, void* data) {
    if (env != nullptr && js_callback != nullptr) {
        napi_value global;
        napi_get_global(env, &global);
        napi_call_function(env, global, js_callback, 0, nullptr, nullptr);
    }
}

// 启动剪贴板监控
Napi::Value StartMonitor(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();

    if (info.Length() < 1 || !info[0].IsFunction()) {
        Napi::TypeError::New(env, "Expected a callback function").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    if (g_isMonitoring) {
        Napi::Error::New(env, "Monitor already started").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    if (g_tsfn != nullptr) {
        Napi::Error::New(env, "Monitor already started").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    // 创建线程安全函数
    napi_value callback = info[0];
    napi_value resource_name;
    napi_create_string_utf8(env, "ClipboardCallback", NAPI_AUTO_LENGTH, &resource_name);

    napi_create_threadsafe_function(
        env,
        callback,
        nullptr,
        resource_name,
        0,
        1,
        nullptr,
        nullptr,
        nullptr,
        CallJs,
        &g_tsfn
    );

    g_isMonitoring = true;

    // 启动消息循环线程
    g_messageThread = std::thread([]() {
        // 注册窗口类
        WNDCLASSW wc = {0};
        wc.lpfnWndProc = ClipboardWndProc;
        wc.hInstance = GetModuleHandle(NULL);
        wc.lpszClassName = L"ZToolsClipboardMonitor";

        if (!RegisterClassW(&wc)) {
            return;
        }

        // 创建隐藏的消息窗口
        g_hwnd = CreateWindowW(
            L"ZToolsClipboardMonitor",
            L"ZToolsClipboardMonitor",
            0, 0, 0, 0, 0,
            HWND_MESSAGE,  // 消息窗口
            NULL, GetModuleHandle(NULL), NULL
        );

        if (g_hwnd == NULL) {
            UnregisterClassW(L"ZToolsClipboardMonitor", GetModuleHandle(NULL));
            return;
        }

        // 注册剪贴板监听
        if (!AddClipboardFormatListener(g_hwnd)) {
            DestroyWindow(g_hwnd);
            UnregisterClassW(L"ZToolsClipboardMonitor", GetModuleHandle(NULL));
            return;
        }

        // 消息循环
        MSG msg;
        while (g_isMonitoring && GetMessageW(&msg, NULL, 0, 0)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        // 清理
        RemoveClipboardFormatListener(g_hwnd);
        DestroyWindow(g_hwnd);
        UnregisterClassW(L"ZToolsClipboardMonitor", GetModuleHandle(NULL));
        g_hwnd = NULL;
    });

    return env.Undefined();
}

// 停止剪贴板监控
Napi::Value StopMonitor(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();

    g_isMonitoring = false;
    g_isPaused = false;  // 重置暂停状态

    if (g_hwnd != NULL) {
        PostMessageW(g_hwnd, WM_QUIT, 0, 0);
    }

    if (g_messageThread.joinable()) {
        g_messageThread.join();
    }

    if (g_tsfn != nullptr) {
        napi_release_threadsafe_function(g_tsfn, napi_tsfn_release);
        g_tsfn = nullptr;
    }

    return env.Undefined();
}

// 暂停剪贴板监控（不触发回调，但保持监控线程运行）
Napi::Value PauseMonitor(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    g_isPaused = true;
    return env.Undefined();
}

// 恢复剪贴板监控
Napi::Value ResumeMonitor(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    g_isPaused = false;
    return env.Undefined();
}

// ==================== 窗口监控功能 ====================

// 窗口信息结构（用于线程安全传递）
struct WindowInfo {
    DWORD processId;
    std::string appName;
    std::string title;
    std::string app;
    std::string appPath;
    std::string className;  // 窗口类名（CabinetWClass/Progman/WorkerW 等，用于识别 Explorer 窗口类型）
    uint64_t hwnd;          // 窗口句柄（用于 COM IShellWindows 查询 Explorer 目录路径）
    int x;
    int y;
    int width;
    int height;
};

// 获取窗口信息的辅助函数
WindowInfo* GetWindowInfo(HWND hwnd) {
    if (hwnd == NULL) {
        return nullptr;
    }

    WindowInfo* info = new WindowInfo();

    // 获取进程 ID
    GetWindowThreadProcessId(hwnd, &info->processId);

    // 获取窗口位置和大小
    RECT rect;
    if (GetWindowRect(hwnd, &rect)) {
        info->x = rect.left;
        info->y = rect.top;
        info->width = rect.right - rect.left;
        info->height = rect.bottom - rect.top;
    } else {
        info->x = 0;
        info->y = 0;
        info->width = 0;
        info->height = 0;
    }

    // 获取窗口标题
    int titleLength = GetWindowTextLengthW(hwnd);
    if (titleLength > 0) {
        std::wstring wTitle(titleLength + 1, L'\0');
        GetWindowTextW(hwnd, &wTitle[0], titleLength + 1);
        wTitle.resize(titleLength);

        // 转换为 UTF-8
        int size = WideCharToMultiByte(CP_UTF8, 0, wTitle.c_str(), -1, NULL, 0, NULL, NULL);
        if (size > 0) {
            info->title.resize(size - 1);
            WideCharToMultiByte(CP_UTF8, 0, wTitle.c_str(), -1, &info->title[0], size, NULL, NULL);
        }
    }

    // 获取窗口类名（CabinetWClass = Explorer 窗口, Progman/WorkerW = 桌面）
    WCHAR classNameBuf[256] = {0};
    int classLen = GetClassNameW(hwnd, classNameBuf, 256);
    if (classLen > 0) {
        int cnSize = WideCharToMultiByte(CP_UTF8, 0, classNameBuf, -1, NULL, 0, NULL, NULL);
        if (cnSize > 0) {
            info->className.resize(cnSize - 1);
            WideCharToMultiByte(CP_UTF8, 0, classNameBuf, -1, &info->className[0], cnSize, NULL, NULL);
        }
    }
    // 保存窗口句柄，用于后续 COM 查询
    info->hwnd = (uint64_t)hwnd;

    // 获取进程句柄
    HANDLE hProcess = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, info->processId);
    if (hProcess) {
        // 获取可执行文件路径
        WCHAR path[MAX_PATH] = {0};
        if (GetModuleFileNameExW(hProcess, NULL, path, MAX_PATH)) {
            // 保存完整路径到 appPath 字段
            std::wstring fullPath(path);
            int pathSize = WideCharToMultiByte(CP_UTF8, 0, fullPath.c_str(), -1, NULL, 0, NULL, NULL);
            if (pathSize > 0) {
                info->appPath.resize(pathSize - 1);
                WideCharToMultiByte(CP_UTF8, 0, fullPath.c_str(), -1, &info->appPath[0], pathSize, NULL, NULL);
            }

            // 提取文件名（去掉路径）
            size_t lastSlash = fullPath.find_last_of(L"\\");
            std::wstring fileNameWithExt = (lastSlash != std::wstring::npos)
                ? fullPath.substr(lastSlash + 1)
                : fullPath;

            // 保存完整程序名（包括 .exe）到 app 字段
            int appSize = WideCharToMultiByte(CP_UTF8, 0, fileNameWithExt.c_str(), -1, NULL, 0, NULL, NULL);
            if (appSize > 0) {
                info->app.resize(appSize - 1);
                WideCharToMultiByte(CP_UTF8, 0, fileNameWithExt.c_str(), -1, &info->app[0], appSize, NULL, NULL);
            }

            // 去掉 .exe 扩展名用于 appName
            std::wstring fileName = fileNameWithExt;
            size_t lastDot = fileName.find_last_of(L".");
            if (lastDot != std::wstring::npos) {
                fileName = fileName.substr(0, lastDot);
            }

            // 转换为 UTF-8
            int size = WideCharToMultiByte(CP_UTF8, 0, fileName.c_str(), -1, NULL, 0, NULL, NULL);
            if (size > 0) {
                info->appName.resize(size - 1);
                WideCharToMultiByte(CP_UTF8, 0, fileName.c_str(), -1, &info->appName[0], size, NULL, NULL);
            }
        }
        CloseHandle(hProcess);
    }

    return info;
}

// 在主线程调用 JS 回调（窗口监控）
void CallWindowJs(napi_env env, napi_value js_callback, void* context, void* data) {
    if (env != nullptr && js_callback != nullptr && data != nullptr) {
        WindowInfo* info = static_cast<WindowInfo*>(data);

        // 创建返回对象
        napi_value result;
        napi_create_object(env, &result);

        napi_value processId;
        napi_create_uint32(env, info->processId, &processId);
        napi_set_named_property(env, result, "processId", processId);

        napi_value pid;
        napi_create_uint32(env, info->processId, &pid);
        napi_set_named_property(env, result, "pid", pid);

        napi_value appName;
        napi_create_string_utf8(env, info->appName.c_str(), NAPI_AUTO_LENGTH, &appName);
        napi_set_named_property(env, result, "appName", appName);

        napi_value title;
        napi_create_string_utf8(env, info->title.c_str(), NAPI_AUTO_LENGTH, &title);
        napi_set_named_property(env, result, "title", title);

        napi_value app;
        napi_create_string_utf8(env, info->app.c_str(), NAPI_AUTO_LENGTH, &app);
        napi_set_named_property(env, result, "app", app);

        napi_value appPath;
        napi_create_string_utf8(env, info->appPath.c_str(), NAPI_AUTO_LENGTH, &appPath);
        napi_set_named_property(env, result, "appPath", appPath);

        napi_value x;
        napi_create_int32(env, info->x, &x);
        napi_set_named_property(env, result, "x", x);

        napi_value y;
        napi_create_int32(env, info->y, &y);
        napi_set_named_property(env, result, "y", y);

        napi_value width;
        napi_create_int32(env, info->width, &width);
        napi_set_named_property(env, result, "width", width);

        napi_value height;
        napi_create_int32(env, info->height, &height);
        napi_set_named_property(env, result, "height", height);

        // 窗口类名（CabinetWClass/Progman/WorkerW 等，用于识别 Explorer 窗口类型）
        napi_value className;
        napi_create_string_utf8(env, info->className.c_str(), NAPI_AUTO_LENGTH, &className);
        napi_set_named_property(env, result, "className", className);

        // 窗口句柄（用于 COM IShellWindows 查询 Explorer 目录路径）
        napi_value hwndVal;
        napi_create_double(env, (double)info->hwnd, &hwndVal);
        napi_set_named_property(env, result, "hwnd", hwndVal);

        // 调用回调
        napi_value global;
        napi_get_global(env, &global);
        napi_call_function(env, global, js_callback, 1, &result, nullptr);

        delete info;
    }
}

// 窗口事件回调
void CALLBACK WinEventProc(
    HWINEVENTHOOK hWinEventHook,
    DWORD event,
    HWND hwnd,
    LONG idObject,
    LONG idChild,
    DWORD dwEventThread,
    DWORD dwmsEventTime
) {
    if (g_windowTsfn == nullptr) {
        return;
    }

    // 处理前台窗口切换事件
    if (event == EVENT_SYSTEM_FOREGROUND) {
        // 更新当前监控的窗口
        g_lastMonitoredWindow = hwnd;

        // 获取窗口信息
        WindowInfo* info = GetWindowInfo(hwnd);
        if (info != nullptr) {
            g_lastMonitoredTitle = info->title;
            // 通过线程安全函数传递到 JS
            napi_call_threadsafe_function(g_windowTsfn, info, napi_tsfn_nonblocking);
        }
    }
    // 处理窗口标题变化事件
    else if (event == EVENT_OBJECT_NAMECHANGE && idObject == OBJID_WINDOW) {
        // 只处理当前前台窗口的标题变化
        HWND foregroundWindow = GetForegroundWindow();
        if (hwnd == foregroundWindow && hwnd == g_lastMonitoredWindow) {
            // 获取新的窗口信息
            WindowInfo* info = GetWindowInfo(hwnd);
            if (info != nullptr) {
                // 检查标题是否真的变化了
                if (info->title != g_lastMonitoredTitle) {
                    g_lastMonitoredTitle = info->title;
                    // 通过线程安全函数传递到 JS
                    napi_call_threadsafe_function(g_windowTsfn, info, napi_tsfn_nonblocking);
                } else {
                    // 标题没变化，释放内存
                    delete info;
                }
            }
        }
    }
}

// 窗口监控消息循环线程
void WindowMonitorThread() {
    // 设置前台窗口切换事件钩子
    g_winEventHook = SetWinEventHook(
        EVENT_SYSTEM_FOREGROUND,
        EVENT_SYSTEM_FOREGROUND,
        NULL,
        WinEventProc,
        0,
        0,
        WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS
    );

    if (g_winEventHook == NULL) {
        g_isWindowMonitoring = false;
        return;
    }

    // 设置窗口标题变化事件钩子
    g_winEventHookTitle = SetWinEventHook(
        EVENT_OBJECT_NAMECHANGE,
        EVENT_OBJECT_NAMECHANGE,
        NULL,
        WinEventProc,
        0,
        0,
        WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS
    );

    if (g_winEventHookTitle == NULL) {
        // 如果标题钩子设置失败，清理前台钩子
        UnhookWinEvent(g_winEventHook);
        g_winEventHook = NULL;
        g_isWindowMonitoring = false;
        return;
    }

    // 运行消息循环
    MSG msg;
    while (g_isWindowMonitoring && GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    // 清理钩子
    if (g_winEventHook != NULL) {
        UnhookWinEvent(g_winEventHook);
        g_winEventHook = NULL;
    }
    if (g_winEventHookTitle != NULL) {
        UnhookWinEvent(g_winEventHookTitle);
        g_winEventHookTitle = NULL;
    }
}

// 启动窗口监控
Napi::Value StartWindowMonitor(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();

    if (info.Length() < 1 || !info[0].IsFunction()) {
        Napi::TypeError::New(env, "Expected a callback function").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    if (g_isWindowMonitoring) {
        Napi::Error::New(env, "Window monitor already started").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    Napi::Function callback = info[0].As<Napi::Function>();
    napi_value resource_name;
    napi_create_string_utf8(env, "WindowMonitor", NAPI_AUTO_LENGTH, &resource_name);

    // 创建线程安全函数
    napi_status status = napi_create_threadsafe_function(
        env,
        callback,
        nullptr,
        resource_name,
        0,
        1,
        nullptr,
        nullptr,
        nullptr,
        CallWindowJs,
        &g_windowTsfn
    );

    if (status != napi_ok) {
        Napi::Error::New(env, "Failed to create threadsafe function").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    g_isWindowMonitoring = true;

    // 启动消息循环线程（钩子将在线程内设置）
    g_windowMessageThread = std::thread(WindowMonitorThread);

    // 等待一小段时间确保线程启动
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // 检查是否成功启动
    if (!g_isWindowMonitoring) {
        if (g_windowMessageThread.joinable()) {
            g_windowMessageThread.join();
        }
        napi_release_threadsafe_function(g_windowTsfn, napi_tsfn_release);
        g_windowTsfn = nullptr;
        Napi::Error::New(env, "Failed to set window event hook").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    // 立即回调当前激活的窗口
    HWND currentWindow = GetForegroundWindow();
    if (currentWindow != NULL) {
        g_lastMonitoredWindow = currentWindow;
        WindowInfo* info = GetWindowInfo(currentWindow);
        if (info != nullptr) {
            g_lastMonitoredTitle = info->title;
            napi_call_threadsafe_function(g_windowTsfn, info, napi_tsfn_nonblocking);
        }
    }

    return env.Undefined();
}

// 停止窗口监控
Napi::Value StopWindowMonitor(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();

    if (!g_isWindowMonitoring) {
        return env.Undefined();
    }

    g_isWindowMonitoring = false;

    // 停止消息循环（钩子会在线程内自动清理）
    if (g_windowMessageThread.joinable()) {
        PostThreadMessage(GetThreadId(g_windowMessageThread.native_handle()), WM_QUIT, 0, 0);
        g_windowMessageThread.join();
    }

    // 释放线程安全函数
    if (g_windowTsfn != nullptr) {
        napi_release_threadsafe_function(g_windowTsfn, napi_tsfn_release);
        g_windowTsfn = nullptr;
    }

    // 重置跟踪变量
    g_lastMonitoredWindow = NULL;
    g_lastMonitoredTitle.clear();

    return env.Undefined();
}

// ==================== 窗口信息获取 ====================


// 获取当前激活窗口
Napi::Value GetActiveWindowInfo(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();

    // 获取前台窗口句柄
    HWND hwnd = GetForegroundWindow();
    if (hwnd == NULL) {
        return env.Null();
    }

    Napi::Object result = Napi::Object::New(env);

    // 获取进程 ID
    DWORD processId = 0;
    GetWindowThreadProcessId(hwnd, &processId);
    result.Set("processId", Napi::Number::New(env, processId));
    result.Set("pid", Napi::Number::New(env, processId));

    // 获取窗口位置和大小
    RECT rect;
    if (GetWindowRect(hwnd, &rect)) {
        result.Set("x", Napi::Number::New(env, rect.left));
        result.Set("y", Napi::Number::New(env, rect.top));
        result.Set("width", Napi::Number::New(env, rect.right - rect.left));
        result.Set("height", Napi::Number::New(env, rect.bottom - rect.top));
    } else {
        result.Set("x", Napi::Number::New(env, 0));
        result.Set("y", Napi::Number::New(env, 0));
        result.Set("width", Napi::Number::New(env, 0));
        result.Set("height", Napi::Number::New(env, 0));
    }

    // 获取窗口标题
    int titleLength = GetWindowTextLengthW(hwnd);
    if (titleLength > 0) {
        std::wstring wTitle(titleLength + 1, L'\0');
        GetWindowTextW(hwnd, &wTitle[0], titleLength + 1);
        wTitle.resize(titleLength);

        // 转换为 UTF-8
        int size = WideCharToMultiByte(CP_UTF8, 0, wTitle.c_str(), -1, NULL, 0, NULL, NULL);
        if (size > 0) {
            std::string titleUtf8(size - 1, 0);
            WideCharToMultiByte(CP_UTF8, 0, wTitle.c_str(), -1, &titleUtf8[0], size, NULL, NULL);
            result.Set("title", Napi::String::New(env, titleUtf8));
        }
    }

    // 获取进程句柄
    HANDLE hProcess = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, processId);
    if (hProcess) {
        // 获取可执行文件路径
        WCHAR path[MAX_PATH] = {0};
        if (GetModuleFileNameExW(hProcess, NULL, path, MAX_PATH)) {
            // 保存完整路径到 appPath 字段
            std::wstring fullPath(path);
            int pathSize = WideCharToMultiByte(CP_UTF8, 0, fullPath.c_str(), -1, NULL, 0, NULL, NULL);
            if (pathSize > 0) {
                std::string pathUtf8(pathSize - 1, 0);
                WideCharToMultiByte(CP_UTF8, 0, fullPath.c_str(), -1, &pathUtf8[0], pathSize, NULL, NULL);
                result.Set("appPath", Napi::String::New(env, pathUtf8));
            }

            // 提取文件名（去掉路径）
            size_t lastSlash = fullPath.find_last_of(L"\\");
            std::wstring fileNameWithExt = (lastSlash != std::wstring::npos)
                ? fullPath.substr(lastSlash + 1)
                : fullPath;

            // 保存完整程序名（包括 .exe）到 app 字段
            int appSize = WideCharToMultiByte(CP_UTF8, 0, fileNameWithExt.c_str(), -1, NULL, 0, NULL, NULL);
            if (appSize > 0) {
                std::string appUtf8(appSize - 1, 0);
                WideCharToMultiByte(CP_UTF8, 0, fileNameWithExt.c_str(), -1, &appUtf8[0], appSize, NULL, NULL);
                result.Set("app", Napi::String::New(env, appUtf8));
            }

            // 去掉 .exe 扩展名用于 appName
            std::wstring fileName = fileNameWithExt;
            size_t lastDot = fileName.find_last_of(L".");
            if (lastDot != std::wstring::npos) {
                fileName = fileName.substr(0, lastDot);
            }

            // 转换为 UTF-8
            int size = WideCharToMultiByte(CP_UTF8, 0, fileName.c_str(), -1, NULL, 0, NULL, NULL);
            if (size > 0) {
                std::string appNameUtf8(size - 1, 0);
                WideCharToMultiByte(CP_UTF8, 0, fileName.c_str(), -1, &appNameUtf8[0], size, NULL, NULL);
                result.Set("appName", Napi::String::New(env, appNameUtf8));
            }
        }
        CloseHandle(hProcess);
    }

    // 获取窗口类名（CabinetWClass = Explorer 窗口, Progman/WorkerW = 桌面）
    WCHAR activeClassNameBuf[256] = {0};
    int activeClassLen = GetClassNameW(hwnd, activeClassNameBuf, 256);
    if (activeClassLen > 0) {
        int cnSize = WideCharToMultiByte(CP_UTF8, 0, activeClassNameBuf, -1, NULL, 0, NULL, NULL);
        if (cnSize > 0) {
            std::string classNameUtf8(cnSize - 1, 0);
            WideCharToMultiByte(CP_UTF8, 0, activeClassNameBuf, -1, &classNameUtf8[0], cnSize, NULL, NULL);
            result.Set("className", Napi::String::New(env, classNameUtf8));
        }
    }
    // 窗口句柄（用于 COM IShellWindows 查询 Explorer 目录路径）
    result.Set("hwnd", Napi::Number::New(env, (double)(uint64_t)hwnd));

    return result;
}

// 枚举窗口回调
struct EnumWindowsCallbackArgs {
    DWORD targetProcessId;
    HWND foundWindow;
};

BOOL CALLBACK EnumWindowsCallback(HWND hwnd, LPARAM lParam) {
    EnumWindowsCallbackArgs* args = (EnumWindowsCallbackArgs*)lParam;

    // 只处理可见的顶级窗口
    if (!IsWindowVisible(hwnd)) {
        return TRUE;
    }

    // 跳过工具窗口
    if (GetWindowLong(hwnd, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) {
        return TRUE;
    }

    // 获取窗口的进程 ID
    DWORD processId;
    GetWindowThreadProcessId(hwnd, &processId);

    // 找到匹配的进程
    if (processId == args->targetProcessId) {
        args->foundWindow = hwnd;
        return FALSE;  // 停止枚举
    }

    return TRUE;  // 继续枚举
}

// 激活窗口（使用 AttachThreadInput + 组合API 强制切换到前台）
Napi::Value ActivateWindow(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();

    if (info.Length() < 1 || !info[0].IsNumber()) {
        Napi::TypeError::New(env, "Expected processId number").ThrowAsJavaScriptException();
        return Napi::Boolean::New(env, false);
    }

    DWORD processId = info[0].As<Napi::Number>().Uint32Value();

    // 枚举所有窗口查找目标进程的窗口
    EnumWindowsCallbackArgs args = { processId, NULL };
    EnumWindows(EnumWindowsCallback, (LPARAM)&args);

    if (args.foundWindow == NULL) {
        return Napi::Boolean::New(env, false);
    }

    HWND hwnd = args.foundWindow;

    // 如果窗口最小化，先恢复
    if (IsIconic(hwnd)) {
        ShowWindow(hwnd, SW_RESTORE);
    }

    // 获取当前前台窗口的线程ID
    HWND foregroundWnd = GetForegroundWindow();
    DWORD foregroundThreadId = GetWindowThreadProcessId(foregroundWnd, NULL);
    DWORD targetThreadId = GetWindowThreadProcessId(hwnd, NULL);
    DWORD currentThreadId = GetCurrentThreadId();

    // 附加到前台窗口的线程（绕过Windows前台窗口限制）
    BOOL attached1 = FALSE;
    BOOL attached2 = FALSE;

    if (foregroundThreadId != targetThreadId) {
        attached1 = AttachThreadInput(foregroundThreadId, targetThreadId, TRUE);
    }
    if (currentThreadId != targetThreadId && currentThreadId != foregroundThreadId) {
        attached2 = AttachThreadInput(currentThreadId, targetThreadId, TRUE);
    }

    // 组合使用多个激活函数确保窗口切换到前台
    BringWindowToTop(hwnd);
    SetForegroundWindow(hwnd);
    SetActiveWindow(hwnd);
    SetFocus(hwnd);

    // 分离线程输入
    if (attached1) {
        AttachThreadInput(foregroundThreadId, targetThreadId, FALSE);
    }
    if (attached2) {
        AttachThreadInput(currentThreadId, targetThreadId, FALSE);
    }

    // 验证是否成功
    HWND newForeground = GetForegroundWindow();
    return Napi::Boolean::New(env, newForeground == hwnd);
}

// ==================== 剪贴板文件功能 ====================

// 获取剪贴板中的文件列表
Napi::Value GetClipboardFiles(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    Napi::Array result = Napi::Array::New(env);

    // 尝试打开剪贴板（带重试机制，解决 Windows 11 剪贴板占用问题）
    const int maxRetries = 5;
    const int retryDelayMs = 50;
    BOOL clipboardOpened = FALSE;

    for (int i = 0; i < maxRetries; i++) {
        // 尝试使用消息窗口句柄或NULL
        HWND hwndOwner = g_hwnd != NULL ? g_hwnd : NULL;
        clipboardOpened = OpenClipboard(hwndOwner);

        if (clipboardOpened) {
            break;  // 成功打开
        }

        // 如果不是最后一次重试，等待后重试
        if (i < maxRetries - 1) {
            Sleep(retryDelayMs);
        }
    }

    // 打开剪贴板失败
    if (!clipboardOpened) {
        // Windows 11: 剪贴板可能被系统或其他程序占用
        return result;  // 返回空数组
    }

    // 检查剪贴板中是否有文件
    if (!IsClipboardFormatAvailable(CF_HDROP)) {
        CloseClipboard();
        return result;  // 返回空数组
    }

    // 获取文件句柄
    HDROP hDrop = (HDROP)GetClipboardData(CF_HDROP);
    if (hDrop == NULL) {
        CloseClipboard();
        return result;  // 返回空数组
    }

    // 获取文件数量
    UINT fileCount = DragQueryFileW(hDrop, 0xFFFFFFFF, NULL, 0);

    // 遍历所有文件
    for (UINT i = 0; i < fileCount; i++) {
        // 获取文件路径长度
        UINT pathLength = DragQueryFileW(hDrop, i, NULL, 0);
        if (pathLength == 0) {
            continue;
        }

        // 获取文件路径
        std::wstring wPath(pathLength + 1, L'\0');
        DragQueryFileW(hDrop, i, &wPath[0], pathLength + 1);
        wPath.resize(pathLength);

        // 转换为 UTF-8
        int utf8Size = WideCharToMultiByte(CP_UTF8, 0, wPath.c_str(), -1, NULL, 0, NULL, NULL);
        if (utf8Size <= 0) {
            continue;
        }

        std::string utf8Path(utf8Size - 1, 0);
        WideCharToMultiByte(CP_UTF8, 0, wPath.c_str(), -1, &utf8Path[0], utf8Size, NULL, NULL);

        // 提取文件名
        size_t lastSlash = utf8Path.find_last_of("\\/");
        std::string fileName = (lastSlash != std::string::npos)
            ? utf8Path.substr(lastSlash + 1)
            : utf8Path;

        // 检查是否是目录
        DWORD fileAttrs = GetFileAttributesW(wPath.c_str());
        bool isDirectory = (fileAttrs != INVALID_FILE_ATTRIBUTES) &&
                          (fileAttrs & FILE_ATTRIBUTE_DIRECTORY);

        // 创建文件信息对象
        Napi::Object fileInfo = Napi::Object::New(env);
        fileInfo.Set("path", Napi::String::New(env, utf8Path));
        fileInfo.Set("name", Napi::String::New(env, fileName));
        fileInfo.Set("isDirectory", Napi::Boolean::New(env, isDirectory));

        // 添加到结果数组
        result.Set(i, fileInfo);
    }

    CloseClipboard();
    return result;
}

// 设置剪贴板中的文件列表
Napi::Value SetClipboardFiles(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();

    // 参数验证：需要一个数组参数
    if (info.Length() < 1 || !info[0].IsArray()) {
        Napi::TypeError::New(env, "Expected an array of file paths or file objects").ThrowAsJavaScriptException();
        return Napi::Boolean::New(env, false);
    }

    Napi::Array filesArray = info[0].As<Napi::Array>();
    uint32_t fileCount = filesArray.Length();

    if (fileCount == 0) {
        Napi::Error::New(env, "File array cannot be empty").ThrowAsJavaScriptException();
        return Napi::Boolean::New(env, false);
    }

    // 提取文件路径
    std::vector<std::wstring> filePaths;
    for (uint32_t i = 0; i < fileCount; i++) {
        Napi::Value item = filesArray[i];
        std::string pathStr;

        // 支持两种格式：
        // 1. 直接是字符串路径
        // 2. 对象 { path: "..." }
        if (item.IsString()) {
            pathStr = item.As<Napi::String>().Utf8Value();
        } else if (item.IsObject()) {
            Napi::Object obj = item.As<Napi::Object>();
            if (obj.Has("path")) {
                Napi::Value pathValue = obj.Get("path");
                if (pathValue.IsString()) {
                    pathStr = pathValue.As<Napi::String>().Utf8Value();
                }
            }
        }

        if (pathStr.empty()) {
            continue;
        }

        // 转换为宽字符
        int wideSize = MultiByteToWideChar(CP_UTF8, 0, pathStr.c_str(), -1, NULL, 0);
        if (wideSize > 0) {
            std::wstring widePath(wideSize - 1, L'\0');
            MultiByteToWideChar(CP_UTF8, 0, pathStr.c_str(), -1, &widePath[0], wideSize);
            filePaths.push_back(widePath);
        }
    }

    if (filePaths.empty()) {
        Napi::Error::New(env, "No valid file paths provided").ThrowAsJavaScriptException();
        return Napi::Boolean::New(env, false);
    }

    // 计算所需内存大小
    size_t totalSize = sizeof(DROPFILES);
    for (const auto& path : filePaths) {
        totalSize += (path.length() + 1) * sizeof(wchar_t);  // 每个路径加一个 null
    }
    totalSize += sizeof(wchar_t);  // 结尾的双 null

    // 分配全局内存
    HGLOBAL hGlobal = GlobalAlloc(GHND | GMEM_SHARE, totalSize);
    if (hGlobal == NULL) {
        Napi::Error::New(env, "Failed to allocate memory").ThrowAsJavaScriptException();
        return Napi::Boolean::New(env, false);
    }

    // 锁定内存
    void* pData = GlobalLock(hGlobal);
    if (pData == NULL) {
        GlobalFree(hGlobal);
        Napi::Error::New(env, "Failed to lock memory").ThrowAsJavaScriptException();
        return Napi::Boolean::New(env, false);
    }

    // 填充 DROPFILES 结构
    DROPFILES* pDropFiles = (DROPFILES*)pData;
    pDropFiles->pFiles = sizeof(DROPFILES);  // 文件列表偏移量
    pDropFiles->pt.x = 0;
    pDropFiles->pt.y = 0;
    pDropFiles->fNC = FALSE;
    pDropFiles->fWide = TRUE;  // 使用 Unicode

    // 填充文件路径列表
    wchar_t* pFilePaths = (wchar_t*)((BYTE*)pData + sizeof(DROPFILES));
    for (const auto& path : filePaths) {
        wcscpy(pFilePaths, path.c_str());
        pFilePaths += path.length() + 1;
    }
    *pFilePaths = L'\0';  // 结尾的双 null

    // 解锁内存
    GlobalUnlock(hGlobal);

    // 尝试打开剪贴板（带重试机制，解决 Windows 11 剪贴板占用问题）
    const int maxRetries = 5;
    const int retryDelayMs = 50;
    BOOL clipboardOpened = FALSE;

    for (int i = 0; i < maxRetries; i++) {
        // 尝试使用消息窗口句柄或NULL
        HWND hwndOwner = g_hwnd != NULL ? g_hwnd : NULL;
        clipboardOpened = OpenClipboard(hwndOwner);

        if (clipboardOpened) {
            break;  // 成功打开
        }

        // 如果不是最后一次重试，等待后重试
        if (i < maxRetries - 1) {
            Sleep(retryDelayMs);
        }
    }

    // 打开剪贴板失败
    if (!clipboardOpened) {
        GlobalFree(hGlobal);
        Napi::Error::New(env, "Failed to open clipboard after retries").ThrowAsJavaScriptException();
        return Napi::Boolean::New(env, false);
    }

    // 清空剪贴板
    EmptyClipboard();

    // 设置剪贴板数据
    HANDLE hResult = SetClipboardData(CF_HDROP, hGlobal);

    // 关闭剪贴板
    CloseClipboard();

    if (hResult == NULL) {
        GlobalFree(hGlobal);
        return Napi::Boolean::New(env, false);
    }

    // 注意：成功后不要释放 hGlobal，剪贴板会接管内存
    return Napi::Boolean::New(env, true);
}

// ==================== 鼠标监控功能 ====================

// 检查回调返回值中的 shouldBlock 并触发重放
void CheckMouseShouldBlock(napi_env env, napi_value value) {
    if (value == nullptr) return;

    napi_valuetype valueType;
    napi_typeof(env, value, &valueType);
    if (valueType != napi_object) return;

    napi_value shouldBlockVal;
    napi_status propStatus = napi_get_named_property(env, value, "shouldBlock", &shouldBlockVal);
    if (propStatus != napi_ok || shouldBlockVal == nullptr) return;

    napi_valuetype sbType;
    napi_typeof(env, shouldBlockVal, &sbType);
    if (sbType != napi_boolean) return;

    bool shouldBlock;
    napi_get_value_bool(env, shouldBlockVal, &shouldBlock);
    if (!shouldBlock) {
        if (g_mouseButtonPressed) {
            // 长按模式：按钮仍被按下，标记在释放时重放
            g_mouseReplayOnRelease = true;
        } else {
            // 点击模式或按钮已释放，立即重放
            g_mouseNeedReplay = true;
        }
    }
}

// Promise.then() 回调：异步回调 resolve 后检查 shouldBlock
napi_value OnMousePromiseResolved(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1];
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

    if (argc > 0) {
        CheckMouseShouldBlock(env, argv[0]);
    }

    napi_value undefined;
    napi_get_undefined(env, &undefined);
    return undefined;
}

// 在主线程调用 JS 回调（鼠标事件）
void CallMouseJs(napi_env env, napi_value js_callback, void* context, void* data) {
    if (env != nullptr && js_callback != nullptr) {
        napi_value global;
        napi_get_global(env, &global);
        napi_value result;
        napi_status status = napi_call_function(env, global, js_callback, 0, nullptr, &result);

        // 检查回调返回值：如果返回 {shouldBlock: false}，则重放被拦截的事件
        if (status == napi_ok && result != nullptr) {
            napi_valuetype resultType;
            napi_typeof(env, result, &resultType);

            if (resultType == napi_object) {
                // 检查是否为 Promise/thenable（有 .then 方法）
                napi_value thenFunc;
                napi_get_named_property(env, result, "then", &thenFunc);
                napi_valuetype thenType;
                napi_typeof(env, thenFunc, &thenType);

                if (thenType == napi_function) {
                    // 异步回调：通过 .then() 获取 resolve 值
                    napi_value resolveCallback;
                    napi_create_function(env, "onResolved", NAPI_AUTO_LENGTH,
                                         OnMousePromiseResolved, nullptr, &resolveCallback);
                    napi_call_function(env, result, thenFunc, 1, &resolveCallback, nullptr);
                } else {
                    // 同步回调：直接检查 shouldBlock
                    CheckMouseShouldBlock(env, result);
                }
            }
        }
    }
}

// 鼠标钩子回调函数
LRESULT CALLBACK MouseHookProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode >= 0 && g_isMouseMonitoring) {
        MSLLHOOKSTRUCT* pMouseStruct = (MSLLHOOKSTRUCT*)lParam;

        // 跳过自己通过 SendInput 重放的事件（通过 dwExtraInfo 标记识别）
        if (pMouseStruct->dwExtraInfo == MOUSE_REPLAY_MAGIC) {
            return CallNextHookEx(g_mouseHook, nCode, wParam, lParam);
        }

        bool shouldBlock = false;

        // 根据按钮类型处理不同的鼠标事件
        if (g_mouseButtonType == "middle") {
            if (wParam == WM_MBUTTONDOWN) {
                g_mouseButtonPressed = true;
                g_mousePressStartTime = std::chrono::steady_clock::now();
                g_mouseLongPressTriggered = false;
                shouldBlock = true;
            } else if (wParam == WM_MBUTTONUP) {
                if (g_mouseButtonPressed) {
                    g_mouseButtonPressed = false;
                    if (g_mouseLongPressMs == 0) {
                        shouldBlock = true;
                        if (!g_mouseLongPressTriggered && g_mouseTsfn != nullptr) {
                            napi_call_threadsafe_function(g_mouseTsfn, nullptr, napi_tsfn_nonblocking);
                        }
                    } else {
                        shouldBlock = true;
                        if (!g_mouseLongPressTriggered) {
                            g_mouseNeedReplay = true;
                        } else if (g_mouseReplayOnRelease) {
                            g_mouseReplayOnRelease = false;
                            g_mouseNeedReplay = true;
                        }
                    }
                }
            }
        } else if (g_mouseButtonType == "right") {
            if (wParam == WM_RBUTTONDOWN) {
                g_mouseButtonPressed = true;
                g_mousePressStartTime = std::chrono::steady_clock::now();
                g_mouseLongPressTriggered = false;
                shouldBlock = true;
            } else if (wParam == WM_RBUTTONUP) {
                if (g_mouseButtonPressed) {
                    g_mouseButtonPressed = false;
                    shouldBlock = true;
                    if (!g_mouseLongPressTriggered) {
                        g_mouseNeedReplay = true;
                    } else if (g_mouseReplayOnRelease) {
                        g_mouseReplayOnRelease = false;
                        g_mouseNeedReplay = true;
                    }
                }
            }
        } else if (g_mouseButtonType == "back") {
            if (wParam == WM_XBUTTONDOWN) {
                WORD xButton = GET_XBUTTON_WPARAM(pMouseStruct->mouseData);
                if (xButton == XBUTTON1) {
                    g_mouseButtonPressed = true;
                    g_mousePressStartTime = std::chrono::steady_clock::now();
                    g_mouseLongPressTriggered = false;
                    shouldBlock = true;
                }
            } else if (wParam == WM_XBUTTONUP) {
                WORD xButton = GET_XBUTTON_WPARAM(pMouseStruct->mouseData);
                if (xButton == XBUTTON1) {
                    if (g_mouseButtonPressed) {
                        g_mouseButtonPressed = false;
                        if (g_mouseLongPressMs == 0) {
                            shouldBlock = true;
                            if (!g_mouseLongPressTriggered && g_mouseTsfn != nullptr) {
                                napi_call_threadsafe_function(g_mouseTsfn, nullptr, napi_tsfn_nonblocking);
                            }
                        } else {
                            shouldBlock = true;
                            if (!g_mouseLongPressTriggered) {
                                g_mouseNeedReplay = true;
                            } else if (g_mouseReplayOnRelease) {
                                g_mouseReplayOnRelease = false;
                                g_mouseNeedReplay = true;
                            }
                        }
                    }
                }
            }
        } else if (g_mouseButtonType == "forward") {
            if (wParam == WM_XBUTTONDOWN) {
                WORD xButton = GET_XBUTTON_WPARAM(pMouseStruct->mouseData);
                if (xButton == XBUTTON2) {
                    g_mouseButtonPressed = true;
                    g_mousePressStartTime = std::chrono::steady_clock::now();
                    g_mouseLongPressTriggered = false;
                    shouldBlock = true;
                }
            } else if (wParam == WM_XBUTTONUP) {
                WORD xButton = GET_XBUTTON_WPARAM(pMouseStruct->mouseData);
                if (xButton == XBUTTON2) {
                    if (g_mouseButtonPressed) {
                        g_mouseButtonPressed = false;
                        if (g_mouseLongPressMs == 0) {
                            shouldBlock = true;
                            if (!g_mouseLongPressTriggered && g_mouseTsfn != nullptr) {
                                napi_call_threadsafe_function(g_mouseTsfn, nullptr, napi_tsfn_nonblocking);
                            }
                        } else {
                            shouldBlock = true;
                            if (!g_mouseLongPressTriggered) {
                                g_mouseNeedReplay = true;
                            } else if (g_mouseReplayOnRelease) {
                                g_mouseReplayOnRelease = false;
                                g_mouseNeedReplay = true;
                            }
                        }
                    }
                }
            }
        }

        // 如果需要屏蔽事件，返回1
        if (shouldBlock) {
            return 1;
        }
    }

    return CallNextHookEx(g_mouseHook, nCode, wParam, lParam);
}

// 鼠标监控线程（检查长按）
void MouseMonitorThread() {
    // 设置低级鼠标钩子
    g_mouseHook = SetWindowsHookExW(WH_MOUSE_LL, MouseHookProc, GetModuleHandle(NULL), 0);

    if (g_mouseHook == NULL) {
        g_isMouseMonitoring = false;
        return;
    }

    // 消息循环
    MSG msg;
    while (g_isMouseMonitoring) {
        // 等待消息到达或超时（用于长按检测），有消息时立即返回，无延迟
        MsgWaitForMultipleObjects(0, NULL, FALSE, 10, QS_ALLINPUT);

        // 处理所有待处理消息
        while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                g_isMouseMonitoring = false;
                break;
            }
        }

        // 长按未触发时，从消息循环中重放原始点击（不在钩子回调中调用 SendInput）
        if (g_mouseNeedReplay) {
            g_mouseNeedReplay = false;
            INPUT inputs[2] = {};
            if (g_mouseButtonType == "middle") {
                inputs[0].type = INPUT_MOUSE;
                inputs[0].mi.dwFlags = MOUSEEVENTF_MIDDLEDOWN;
                inputs[0].mi.dwExtraInfo = MOUSE_REPLAY_MAGIC;
                inputs[1].type = INPUT_MOUSE;
                inputs[1].mi.dwFlags = MOUSEEVENTF_MIDDLEUP;
                inputs[1].mi.dwExtraInfo = MOUSE_REPLAY_MAGIC;
                SendInput(2, inputs, sizeof(INPUT));
            } else if (g_mouseButtonType == "right") {
                inputs[0].type = INPUT_MOUSE;
                inputs[0].mi.dwFlags = MOUSEEVENTF_RIGHTDOWN;
                inputs[0].mi.dwExtraInfo = MOUSE_REPLAY_MAGIC;
                inputs[1].type = INPUT_MOUSE;
                inputs[1].mi.dwFlags = MOUSEEVENTF_RIGHTUP;
                inputs[1].mi.dwExtraInfo = MOUSE_REPLAY_MAGIC;
                SendInput(2, inputs, sizeof(INPUT));
            } else if (g_mouseButtonType == "back") {
                inputs[0].type = INPUT_MOUSE;
                inputs[0].mi.dwFlags = MOUSEEVENTF_XDOWN;
                inputs[0].mi.mouseData = XBUTTON1;
                inputs[0].mi.dwExtraInfo = MOUSE_REPLAY_MAGIC;
                inputs[1].type = INPUT_MOUSE;
                inputs[1].mi.dwFlags = MOUSEEVENTF_XUP;
                inputs[1].mi.mouseData = XBUTTON1;
                inputs[1].mi.dwExtraInfo = MOUSE_REPLAY_MAGIC;
                SendInput(2, inputs, sizeof(INPUT));
            } else if (g_mouseButtonType == "forward") {
                inputs[0].type = INPUT_MOUSE;
                inputs[0].mi.dwFlags = MOUSEEVENTF_XDOWN;
                inputs[0].mi.mouseData = XBUTTON2;
                inputs[0].mi.dwExtraInfo = MOUSE_REPLAY_MAGIC;
                inputs[1].type = INPUT_MOUSE;
                inputs[1].mi.dwFlags = MOUSEEVENTF_XUP;
                inputs[1].mi.mouseData = XBUTTON2;
                inputs[1].mi.dwExtraInfo = MOUSE_REPLAY_MAGIC;
                SendInput(2, inputs, sizeof(INPUT));
            }
        }

        // 检查长按
        if (g_mouseLongPressMs > 0 && g_mouseButtonPressed && !g_mouseLongPressTriggered) {
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - g_mousePressStartTime).count();

            if (elapsed >= g_mouseLongPressMs) {
                g_mouseLongPressTriggered = true;
                if (g_mouseTsfn != nullptr) {
                    napi_call_threadsafe_function(g_mouseTsfn, nullptr, napi_tsfn_nonblocking);
                }
            }
        }
    }

    // 清理钩子
    if (g_mouseHook != NULL) {
        UnhookWindowsHookEx(g_mouseHook);
        g_mouseHook = NULL;
    }
}

// 启动鼠标监控
Napi::Value StartMouseMonitor(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();

    // 参数1：buttonType（字符串）
    if (info.Length() < 1 || !info[0].IsString()) {
        Napi::TypeError::New(env, "Expected buttonType as first argument (string)").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    // 参数2：longPressMs（数字）
    if (info.Length() < 2 || !info[1].IsNumber()) {
        Napi::TypeError::New(env, "Expected longPressMs as second argument (number)").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    // 参数3：callback（函数）
    if (info.Length() < 3 || !info[2].IsFunction()) {
        Napi::TypeError::New(env, "Expected callback function as third argument").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    if (g_isMouseMonitoring) {
        Napi::Error::New(env, "Mouse monitor already started").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    g_mouseButtonType = info[0].As<Napi::String>().Utf8Value();
    g_mouseLongPressMs = info[1].As<Napi::Number>().Int32Value();

    // 验证按钮类型
    if (g_mouseButtonType != "middle" && g_mouseButtonType != "right" &&
        g_mouseButtonType != "back" && g_mouseButtonType != "forward") {
        Napi::TypeError::New(env, "buttonType must be one of: middle, right, back, forward").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    // 验证 longPressMs
    if (g_mouseLongPressMs < 0) {
        Napi::TypeError::New(env, "longPressMs must be a non-negative number").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    // 右键只支持长按
    if (g_mouseButtonType == "right" && g_mouseLongPressMs == 0) {
        Napi::TypeError::New(env, "'right' button only supports long press (longPressMs must be > 0)").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    // 创建线程安全函数
    napi_value callback = info[2];
    napi_value resource_name;
    napi_create_string_utf8(env, "MouseCallback", NAPI_AUTO_LENGTH, &resource_name);

    napi_status status = napi_create_threadsafe_function(
        env,
        callback,
        nullptr,
        resource_name,
        0,
        1,
        nullptr,
        nullptr,
        nullptr,
        CallMouseJs,
        &g_mouseTsfn
    );

    if (status != napi_ok) {
        Napi::Error::New(env, "Failed to create threadsafe function").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    // 重置状态
    g_mouseButtonPressed = false;
    g_mouseLongPressTriggered = false;
    g_mouseReplayOnRelease = false;
    g_isMouseMonitoring = true;

    // 启动监控线程
    g_mouseMessageThread = std::thread(MouseMonitorThread);

    return env.Undefined();
}

// 停止鼠标监控
Napi::Value StopMouseMonitor(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();

    if (!g_isMouseMonitoring) {
        return env.Undefined();
    }

    g_isMouseMonitoring = false;

    // 等待线程结束
    if (g_mouseMessageThread.joinable()) {
        g_mouseMessageThread.join();
    }

    // 释放线程安全函数
    if (g_mouseTsfn != nullptr) {
        napi_release_threadsafe_function(g_mouseTsfn, napi_tsfn_release);
        g_mouseTsfn = nullptr;
    }

    // 重置状态
    g_mouseButtonPressed = false;
    g_mouseLongPressTriggered = false;
    g_mouseNeedReplay = false;
    g_mouseReplayOnRelease = false;
    g_mouseButtonType.clear();
    g_mouseLongPressMs = 0;

    return env.Undefined();
}

// ==================== 键盘模拟功能 ====================

// 将键名映射为 Windows Virtual Key Code
WORD GetVirtualKeyCode(const std::string& key) {
    static std::map<std::string, WORD> keyMap = {
        // 字母键
        {"a", 'A'}, {"b", 'B'}, {"c", 'C'}, {"d", 'D'}, {"e", 'E'}, {"f", 'F'},
        {"g", 'G'}, {"h", 'H'}, {"i", 'I'}, {"j", 'J'}, {"k", 'K'}, {"l", 'L'},
        {"m", 'M'}, {"n", 'N'}, {"o", 'O'}, {"p", 'P'}, {"q", 'Q'}, {"r", 'R'},
        {"s", 'S'}, {"t", 'T'}, {"u", 'U'}, {"v", 'V'}, {"w", 'W'}, {"x", 'X'},
        {"y", 'Y'}, {"z", 'Z'},

        // 数字键
        {"0", '0'}, {"1", '1'}, {"2", '2'}, {"3", '3'}, {"4", '4'},
        {"5", '5'}, {"6", '6'}, {"7", '7'}, {"8", '8'}, {"9", '9'},

        // 功能键
        {"f1", VK_F1}, {"f2", VK_F2}, {"f3", VK_F3}, {"f4", VK_F4},
        {"f5", VK_F5}, {"f6", VK_F6}, {"f7", VK_F7}, {"f8", VK_F8},
        {"f9", VK_F9}, {"f10", VK_F10}, {"f11", VK_F11}, {"f12", VK_F12},

        // 特殊键
        {"return", VK_RETURN}, {"enter", VK_RETURN}, {"tab", VK_TAB},
        {"space", VK_SPACE}, {"backspace", VK_BACK}, {"delete", VK_DELETE},
        {"escape", VK_ESCAPE}, {"esc", VK_ESCAPE},

        // 方向键
        {"left", VK_LEFT}, {"right", VK_RIGHT}, {"up", VK_UP}, {"down", VK_DOWN},

        // 其他键
        {"minus", VK_OEM_MINUS}, {"-", VK_OEM_MINUS},
        {"equal", VK_OEM_PLUS}, {"=", VK_OEM_PLUS},
        {"leftbracket", VK_OEM_4}, {"[", VK_OEM_4},
        {"rightbracket", VK_OEM_6}, {"]", VK_OEM_6},
        {"backslash", VK_OEM_5}, {"\\", VK_OEM_5},
        {"semicolon", VK_OEM_1}, {";", VK_OEM_1},
        {"quote", VK_OEM_7}, {"'", VK_OEM_7},
        {"comma", VK_OEM_COMMA}, {",", VK_OEM_COMMA},
        {"period", VK_OEM_PERIOD}, {".", VK_OEM_PERIOD},
        {"slash", VK_OEM_2}, {"/", VK_OEM_2},
        {"grave", VK_OEM_3}, {"`", VK_OEM_3}
    };

    // 转换为小写
    std::string lowerKey = key;
    std::transform(lowerKey.begin(), lowerKey.end(), lowerKey.begin(), ::tolower);

    auto it = keyMap.find(lowerKey);
    if (it != keyMap.end()) {
        return it->second;
    }

    return 0;  // 未知键
}

// 把字符串按分隔符拆分为片段。
std::vector<std::string> SplitShortcutString(const std::string& value, char delimiter) {
    std::vector<std::string> parts;
    std::stringstream stream(value);
    std::string segment;
    while (std::getline(stream, segment, delimiter)) {
        parts.push_back(segment);
    }
    return parts;
}

// 规范化快捷键片段，统一大小写和别名。
std::string NormalizeShortcutToken(std::string token) {
    token.erase(std::remove_if(token.begin(), token.end(), [](unsigned char ch) {
        return std::isspace(ch) != 0;
    }), token.end());
    std::transform(token.begin(), token.end(), token.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });

    if (token == "commandorcontrol") {
        return "ctrl";
    }
    if (token == "command" || token == "cmd" || token == "meta" || token == "super" || token == "windows" || token == "win") {
        return "command";
    }
    if (token == "control") {
        return "ctrl";
    }
    if (token == "option") {
        return "alt";
    }
    if (token == "return") {
        return "enter";
    }
    return token;
}

// ==================== 优化截图快捷键监听 ====================

static constexpr UINT WM_OPTIMIZED_SHORTCUT_REFRESH = WM_APP + 101;

// 将快捷键字符串解析为 RegisterHotKey 所需的定义。
bool TryParseOptimizedShortcut(const std::string& shortcut, OptimizedShortcutDefinition& outDefinition) {
    const std::vector<std::string> parts = SplitShortcutString(shortcut, '+');
    if (parts.empty()) {
        return false;
    }

    OptimizedShortcutDefinition definition;
    definition.shortcut = shortcut;

    for (const std::string& rawPart : parts) {
        const std::string token = NormalizeShortcutToken(rawPart);
        if (token.empty()) {
            continue;
        }

        if (token == "shift") {
            definition.hotkeyModifiers |= MOD_SHIFT;
            continue;
        }
        if (token == "ctrl") {
            definition.hotkeyModifiers |= MOD_CONTROL;
            continue;
        }
        if (token == "alt") {
            definition.hotkeyModifiers |= MOD_ALT;
            continue;
        }
        if (token == "command") {
            definition.hotkeyModifiers |= MOD_WIN;
            continue;
        }

        if (definition.mainKey != 0) {
            return false;
        }

        definition.mainKey = GetVirtualKeyCode(token);
        if (definition.mainKey == 0) {
            return false;
        }
    }

    if (definition.mainKey == 0) {
        return false;
    }

    outDefinition = definition;
    return true;
}

// 取消当前线程上已注册的优化截图快捷键。
void UnregisterOptimizedShortcutsOnListenerThread() {
    for (const auto& entry : g_activeOptimizedShortcutIds) {
        UnregisterHotKey(NULL, entry.first);
    }
    g_activeOptimizedShortcutIds.clear();
}

// 格式化优化快捷键注册失败信息，便于回传给 JS 层展示。
std::string BuildOptimizedShortcutRegistrationError(const std::string& shortcut, DWORD errorCode) {
    std::ostringstream ss;
    ss << "RegisterHotKey failed for " << shortcut;

    if (errorCode != 0) {
        ss << " (error " << errorCode << ")";

        LPSTR buffer = nullptr;
        const DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
        const DWORD length = FormatMessageA(flags, NULL, errorCode, 0, reinterpret_cast<LPSTR>(&buffer), 0, NULL);
        if (length > 0 && buffer != nullptr) {
            std::string message(buffer, length);
            while (!message.empty() && (message.back() == '\r' || message.back() == '\n' || message.back() == ' ' || message.back() == '\t')) {
                message.pop_back();
            }
            if (!message.empty()) {
                ss << ": " << message;
            }
            LocalFree(buffer);
        }
    }

    return ss.str();
}

// 结束一次等待中的优化快捷键 refresh，并把结果通知给调用线程。
void CompleteOptimizedShortcutRefreshResult(const OptimizedShortcutRegistrationResult& result) {
    std::lock_guard<std::mutex> lock(g_optimizedShortcutRefreshResultMutex);
    if (!g_optimizedShortcutRefreshPending) {
        return;
    }

    g_optimizedShortcutRefreshResult = result;
    g_optimizedShortcutRefreshPending = false;
    g_optimizedShortcutRefreshCompleted = true;
    g_optimizedShortcutRefreshRequest = OptimizedShortcutRefreshRequest{};
    g_optimizedShortcutRefreshResultCv.notify_all();
}

// 构造优化快捷键注册结果对象，统一返回给 JS 层。
Napi::Object CreateOptimizedShortcutRegistrationResult(Napi::Env env, bool success, const std::string& error = "") {
    Napi::Object result = Napi::Object::New(env);
    result.Set("success", Napi::Boolean::New(env, success));
    if (!error.empty()) {
        result.Set("error", Napi::String::New(env, error));
    }
    return result;
}

// 按最新配置重新注册当前线程上的优化截图快捷键。
void RefreshOptimizedShortcutsOnListenerThread() {
    std::vector<OptimizedShortcutDefinition> shortcuts;
    {
        std::lock_guard<std::mutex> lock(g_optimizedShortcutMutex);
        shortcuts = g_optimizedShortcuts;
    }

    std::string refreshTarget;
    bool shouldBeRegistered = true;
    bool shouldReport = false;
    {
        std::lock_guard<std::mutex> lock(g_optimizedShortcutRefreshResultMutex);
        shouldReport = g_optimizedShortcutRefreshPending;
        refreshTarget = g_optimizedShortcutRefreshRequest.shortcut;
        shouldBeRegistered = g_optimizedShortcutRefreshRequest.shouldBeRegistered;
    }

    OptimizedShortcutRegistrationResult refreshResult;
    refreshResult.success = !shouldReport;
    bool targetSeen = false;

    UnregisterOptimizedShortcutsOnListenerThread();

    int nextHotkeyId = 1;
    for (const auto& definition : shortcuts) {
        UINT modifiers = definition.hotkeyModifiers | MOD_NOREPEAT;
        const bool registered = RegisterHotKey(NULL, nextHotkeyId, modifiers, definition.mainKey);
        if (registered) {
            g_activeOptimizedShortcutIds[nextHotkeyId] = definition;
            nextHotkeyId++;
        }

        if (shouldReport && definition.shortcut == refreshTarget) {
            targetSeen = true;
            if (registered) {
                refreshResult.success = true;
                refreshResult.error.clear();
            } else {
                refreshResult.success = false;
                refreshResult.error = BuildOptimizedShortcutRegistrationError(definition.shortcut, GetLastError());
            }
        }
    }

    if (shouldReport) {
        if (!targetSeen) {
            if (shouldBeRegistered) {
                refreshResult.success = false;
                refreshResult.error = "optimized shortcut refresh target not found: " + refreshTarget;
            } else {
                refreshResult.success = true;
                refreshResult.error.clear();
            }
        }
        CompleteOptimizedShortcutRefreshResult(refreshResult);
    }
}

// 回到 JS 主线程通知命中的优化快捷键。
void CallOptimizedShortcutJs(napi_env env, napi_value js_callback, void* context, void* data) {
    OptimizedShortcutTrigger* trigger = static_cast<OptimizedShortcutTrigger*>(data);
    if (!trigger) {
        return;
    }

    if (env != nullptr && js_callback != nullptr) {
        napi_value global;
        napi_get_global(env, &global);

        napi_value payload;
        napi_create_object(env, &payload);

        napi_value shortcutValue;
        napi_create_string_utf8(env, trigger->shortcut.c_str(), NAPI_AUTO_LENGTH, &shortcutValue);
        napi_set_named_property(env, payload, "shortcut", shortcutValue);

        napi_value primedValue;
        napi_get_boolean(env, trigger->primed, &primedValue);
        napi_set_named_property(env, payload, "primed", primedValue);

        napi_call_function(env, global, js_callback, 1, &payload, nullptr);
    }

    delete trigger;
}

// native 优化快捷键消息循环线程。
void OptimizedShortcutThread() {
    g_optimizedShortcutThreadId = GetCurrentThreadId();
    MSG msg;
    PeekMessage(&msg, NULL, WM_USER, WM_USER, PM_NOREMOVE);
    RefreshOptimizedShortcutsOnListenerThread();

    while (g_isOptimizedShortcutListening && GetMessage(&msg, NULL, 0, 0)) {
        if (msg.message == WM_HOTKEY) {
            const int hotkeyId = static_cast<int>(msg.wParam);
            auto it = g_activeOptimizedShortcutIds.find(hotkeyId);
            if (it != g_activeOptimizedShortcutIds.end()) {
                OptimizedShortcutTrigger* trigger = new OptimizedShortcutTrigger();
                trigger->shortcut = it->second.shortcut;
                trigger->primed = PrimeScreenshotFrameNow();
                if (g_optimizedShortcutTsfn != nullptr) {
                    napi_call_threadsafe_function(g_optimizedShortcutTsfn, trigger, napi_tsfn_nonblocking);
                } else {
                    delete trigger;
                }
            }
            continue;
        }

        if (msg.message == WM_OPTIMIZED_SHORTCUT_REFRESH) {
            RefreshOptimizedShortcutsOnListenerThread();
            continue;
        }

        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    UnregisterOptimizedShortcutsOnListenerThread();
    g_optimizedShortcutThreadId = 0;
}

// 确保 native 优化快捷键监听已经启动。
Napi::Value EnsureOptimizedShortcutListener(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();

    if (info.Length() < 1 || !info[0].IsFunction()) {
        Napi::TypeError::New(env, "Expected callback function as first argument").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    if (g_isOptimizedShortcutListening) {
        return env.Undefined();
    }

    napi_value callback = info[0];
    napi_value resource_name;
    napi_create_string_utf8(env, "OptimizedShortcutCallback", NAPI_AUTO_LENGTH, &resource_name);

    napi_status status = napi_create_threadsafe_function(
        env,
        callback,
        nullptr,
        resource_name,
        0,
        1,
        nullptr,
        nullptr,
        nullptr,
        CallOptimizedShortcutJs,
        &g_optimizedShortcutTsfn
    );

    if (status != napi_ok) {
        Napi::Error::New(env, "Failed to create optimized shortcut threadsafe function").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    g_isOptimizedShortcutListening = true;
    g_optimizedShortcutThread = std::thread(OptimizedShortcutThread);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    if (!g_isOptimizedShortcutListening || g_optimizedShortcutThreadId == 0) {
        if (g_optimizedShortcutThread.joinable()) {
            g_optimizedShortcutThread.join();
        }
        napi_release_threadsafe_function(g_optimizedShortcutTsfn, napi_tsfn_release);
        g_optimizedShortcutTsfn = nullptr;
        Napi::Error::New(env, "Failed to start optimized shortcut listener").ThrowAsJavaScriptException();
    }

    return env.Undefined();
}

// 停止 native 优化快捷键监听并释放资源。
Napi::Value StopOptimizedShortcutListener(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();

    if (!g_isOptimizedShortcutListening) {
        std::lock_guard<std::mutex> lock(g_optimizedShortcutMutex);
        g_optimizedShortcuts.clear();
        g_activeOptimizedShortcutIds.clear();
        CompleteOptimizedShortcutRefreshResult({false, "optimized shortcut listener stopped"});
        return env.Undefined();
    }

    g_isOptimizedShortcutListening = false;

    const DWORD threadId = g_optimizedShortcutThreadId;
    if (threadId != 0) {
        PostThreadMessage(threadId, WM_QUIT, 0, 0);
    }
    if (g_optimizedShortcutThread.joinable()) {
        g_optimizedShortcutThread.join();
    }

    if (g_optimizedShortcutTsfn != nullptr) {
        napi_release_threadsafe_function(g_optimizedShortcutTsfn, napi_tsfn_release);
        g_optimizedShortcutTsfn = nullptr;
    }

    std::lock_guard<std::mutex> lock(g_optimizedShortcutMutex);
    g_optimizedShortcuts.clear();
    g_activeOptimizedShortcutIds.clear();
    CompleteOptimizedShortcutRefreshResult({false, "optimized shortcut listener stopped"});
    return env.Undefined();
}

// 注册单个受管优化截图快捷键。
Napi::Value RegisterOptimizedShortcut(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();

    if (info.Length() < 1 || !info[0].IsString()) {
        Napi::TypeError::New(env, "Expected shortcut as first argument").ThrowAsJavaScriptException();
        return CreateOptimizedShortcutRegistrationResult(env, false, "Expected shortcut as first argument");
    }

    const std::string shortcut = info[0].As<Napi::String>().Utf8Value();
    OptimizedShortcutDefinition definition;
    if (!TryParseOptimizedShortcut(shortcut, definition)) {
        Napi::Error::New(env, "Unsupported optimized shortcut: " + shortcut).ThrowAsJavaScriptException();
        return CreateOptimizedShortcutRegistrationResult(env, false, "Unsupported optimized shortcut: " + shortcut);
    }

    {
        std::lock_guard<std::mutex> lock(g_optimizedShortcutMutex);
        auto existing = std::find_if(g_optimizedShortcuts.begin(), g_optimizedShortcuts.end(), [&](const OptimizedShortcutDefinition& item) {
            return item.shortcut == shortcut;
        });
        if (existing != g_optimizedShortcuts.end()) {
            *existing = definition;
        } else {
            g_optimizedShortcuts.push_back(definition);
        }
    }

    const DWORD threadId = g_optimizedShortcutThreadId;
    if (threadId == 0) {
        return CreateOptimizedShortcutRegistrationResult(env, false, "optimized shortcut listener is not running");
    }

    {
        std::lock_guard<std::mutex> lock(g_optimizedShortcutRefreshResultMutex);
        g_optimizedShortcutRefreshPending = true;
        g_optimizedShortcutRefreshCompleted = false;
        g_optimizedShortcutRefreshRequest = {shortcut, true};
        g_optimizedShortcutRefreshResult = OptimizedShortcutRegistrationResult{};
    }

    if (!PostThreadMessage(threadId, WM_OPTIMIZED_SHORTCUT_REFRESH, 0, 0)) {
        const DWORD errorCode = GetLastError();
        CompleteOptimizedShortcutRefreshResult({false, BuildOptimizedShortcutRegistrationError(shortcut, errorCode)});
        return CreateOptimizedShortcutRegistrationResult(env, false, BuildOptimizedShortcutRegistrationError(shortcut, errorCode));
    }

    std::unique_lock<std::mutex> lock(g_optimizedShortcutRefreshResultMutex);
    const bool completed = g_optimizedShortcutRefreshResultCv.wait_for(
        lock,
        std::chrono::milliseconds(1000),
        []() { return g_optimizedShortcutRefreshCompleted; }
    );

    if (!completed) {
        g_optimizedShortcutRefreshPending = false;
        g_optimizedShortcutRefreshCompleted = false;
        g_optimizedShortcutRefreshRequest = OptimizedShortcutRefreshRequest{};
        return CreateOptimizedShortcutRegistrationResult(env, false, "optimized shortcut refresh timed out");
    }

    const OptimizedShortcutRegistrationResult result = g_optimizedShortcutRefreshResult;
    g_optimizedShortcutRefreshCompleted = false;
    return CreateOptimizedShortcutRegistrationResult(env, result.success, result.error);
}

// 注销单个受管优化截图快捷键。
Napi::Value UnregisterOptimizedShortcut(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();

    if (info.Length() < 1 || !info[0].IsString()) {
        Napi::TypeError::New(env, "Expected shortcut as first argument").ThrowAsJavaScriptException();
        return CreateOptimizedShortcutRegistrationResult(env, false, "Expected shortcut as first argument");
    }

    const std::string shortcut = info[0].As<Napi::String>().Utf8Value();
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(g_optimizedShortcutMutex);
        const auto beforeSize = g_optimizedShortcuts.size();
        g_optimizedShortcuts.erase(
            std::remove_if(g_optimizedShortcuts.begin(), g_optimizedShortcuts.end(), [&](const OptimizedShortcutDefinition& item) {
                return item.shortcut == shortcut;
            }),
            g_optimizedShortcuts.end()
        );
        changed = beforeSize != g_optimizedShortcuts.size();
    }

    if (!changed) {
        return CreateOptimizedShortcutRegistrationResult(env, true);
    }

    const DWORD threadId = g_optimizedShortcutThreadId;
    if (threadId == 0) {
        return CreateOptimizedShortcutRegistrationResult(env, true);
    }

    {
        std::lock_guard<std::mutex> lock(g_optimizedShortcutRefreshResultMutex);
        g_optimizedShortcutRefreshPending = true;
        g_optimizedShortcutRefreshCompleted = false;
        g_optimizedShortcutRefreshRequest = {shortcut, false};
        g_optimizedShortcutRefreshResult = OptimizedShortcutRegistrationResult{};
    }

    if (!PostThreadMessage(threadId, WM_OPTIMIZED_SHORTCUT_REFRESH, 0, 0)) {
        const DWORD errorCode = GetLastError();
        CompleteOptimizedShortcutRefreshResult({false, BuildOptimizedShortcutRegistrationError(shortcut, errorCode)});
        return CreateOptimizedShortcutRegistrationResult(env, false, BuildOptimizedShortcutRegistrationError(shortcut, errorCode));
    }

    std::unique_lock<std::mutex> lock(g_optimizedShortcutRefreshResultMutex);
    const bool completed = g_optimizedShortcutRefreshResultCv.wait_for(
        lock,
        std::chrono::milliseconds(1000),
        []() { return g_optimizedShortcutRefreshCompleted; }
    );

    if (!completed) {
        g_optimizedShortcutRefreshPending = false;
        g_optimizedShortcutRefreshCompleted = false;
        g_optimizedShortcutRefreshRequest = OptimizedShortcutRefreshRequest{};
        return CreateOptimizedShortcutRegistrationResult(env, false, "optimized shortcut refresh timed out");
    }

    const OptimizedShortcutRegistrationResult result = g_optimizedShortcutRefreshResult;
    g_optimizedShortcutRefreshCompleted = false;
    return CreateOptimizedShortcutRegistrationResult(env, result.success, result.error);
}

// 读取当前受管优化截图快捷键数量。
Napi::Value GetOptimizedShortcutCount(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    std::lock_guard<std::mutex> lock(g_optimizedShortcutMutex);
    return Napi::Number::New(env, static_cast<double>(g_optimizedShortcuts.size()));
}

// ==================== 获取选中内容功能 ====================

// UI Automation 接口（延迟加载）
#include <uiautomation.h>
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "crypt32.lib")  // For CryptBinaryToStringA

// ==================== 剪贴板内容读取辅助函数 ====================

// 读取剪贴板文本内容
std::string GetClipboardTextContent() {
    std::string result;

    if (!OpenClipboard(NULL)) {
        return result;
    }

    // 尝试读取 Unicode 文本
    if (IsClipboardFormatAvailable(CF_UNICODETEXT)) {
        HANDLE hData = GetClipboardData(CF_UNICODETEXT);
        if (hData != NULL) {
            wchar_t* pszText = static_cast<wchar_t*>(GlobalLock(hData));
            if (pszText != NULL) {
                // 使用 wcslen 获取实际长度，避免越界写入
                int wideLen = static_cast<int>(wcslen(pszText));
                int utf8Size = WideCharToMultiByte(CP_UTF8, 0, pszText, wideLen, nullptr, 0, nullptr, nullptr);
                if (utf8Size > 0) {
                    result.resize(utf8Size);
                    WideCharToMultiByte(CP_UTF8, 0, pszText, wideLen, &result[0], utf8Size, nullptr, nullptr);
                }
                GlobalUnlock(hData);
            }
        }
    }
    // 回退到 ANSI 文本
    else if (IsClipboardFormatAvailable(CF_TEXT)) {
        HANDLE hData = GetClipboardData(CF_TEXT);
        if (hData != NULL) {
            char* pszText = static_cast<char*>(GlobalLock(hData));
            if (pszText != NULL) {
                result = pszText;
                GlobalUnlock(hData);
            }
        }
    }

    CloseClipboard();
    return result;
}

// 读取剪贴板图像内容（返回 base64 编码的 PNG）
std::string GetClipboardImageContent() {
    std::string result;

    if (!OpenClipboard(NULL)) {
        return result;
    }

    HBITMAP hBitmap = NULL;
    bool mustDeleteBitmap = false; // 标记是否需要删除 hBitmap

    // 尝试获取位图
    if (IsClipboardFormatAvailable(CF_BITMAP)) {
        hBitmap = static_cast<HBITMAP>(GetClipboardData(CF_BITMAP));
    } else if (IsClipboardFormatAvailable(CF_DIB)) {
        HANDLE hDIB = GetClipboardData(CF_DIB);
        if (hDIB != NULL) {
            BITMAPINFO* pBMI = static_cast<BITMAPINFO*>(GlobalLock(hDIB));
            if (pBMI != NULL) {
                HDC hDC = GetDC(NULL);
                void* pBits = reinterpret_cast<BYTE*>(pBMI) + pBMI->bmiHeader.biSize;
                hBitmap = CreateDIBitmap(hDC, &pBMI->bmiHeader, CBM_INIT, pBits, pBMI, DIB_RGB_COLORS);
                ReleaseDC(NULL, hDC);
                GlobalUnlock(hDIB);
                mustDeleteBitmap = true; // 标记需要删除
            }
        }
    }

    if (hBitmap != NULL) {
        // 使用 GDI+ 将位图转换为 PNG base64
        Gdiplus::Bitmap* bitmap = Gdiplus::Bitmap::FromHBITMAP(hBitmap, NULL);
        if (bitmap != NULL) {
            IStream* pStream = NULL;
            if (CreateStreamOnHGlobal(NULL, TRUE, &pStream) == S_OK) {
                CLSID pngClsid;
                CLSIDFromString(L"{557CF406-1A04-11D3-9A73-0000F81EF32E}", &pngClsid);

                if (bitmap->Save(pStream, &pngClsid, NULL) == Gdiplus::Ok) {
                    HGLOBAL hGlobal = NULL;
                    if (GetHGlobalFromStream(pStream, &hGlobal) == S_OK) {
                        SIZE_T size = GlobalSize(hGlobal);
                        void* pData = GlobalLock(hGlobal);
                        if (pData != NULL) {
                            // Base64 编码
                            DWORD base64Size = 0;
                            CryptBinaryToStringA(static_cast<BYTE*>(pData), size, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, NULL, &base64Size);
                            if (base64Size > 0) {
                                result.resize(base64Size);
                                CryptBinaryToStringA(static_cast<BYTE*>(pData), size, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, &result[0], &base64Size);
                                result.resize(base64Size - 1); // 移除 null terminator
                            }
                            GlobalUnlock(hGlobal);
                        }
                    }
                }
                pStream->Release();
            }
            delete bitmap;
        }

        // 根据标记决定是否删除 hBitmap
        if (mustDeleteBitmap) {
            DeleteObject(hBitmap);
        }
    }

    CloseClipboard();
    return result;
}

// 读取剪贴板文件列表
std::vector<std::string> GetClipboardFilesList() {
    std::vector<std::string> result;

    if (!OpenClipboard(NULL)) {
        return result;
    }

    if (IsClipboardFormatAvailable(CF_HDROP)) {
        HDROP hDrop = static_cast<HDROP>(GetClipboardData(CF_HDROP));
        if (hDrop != NULL) {
            UINT fileCount = DragQueryFileW(hDrop, 0xFFFFFFFF, NULL, 0);
            for (UINT i = 0; i < fileCount; i++) {
                UINT pathLen = DragQueryFileW(hDrop, i, NULL, 0);
                if (pathLen > 0) {
                    std::wstring wPath(pathLen, L'\0');
                    DragQueryFileW(hDrop, i, &wPath[0], pathLen + 1);

                    // 使用实际长度进行精确转换，避免越界写入
                    int wideLen = static_cast<int>(wPath.length());
                    int utf8Size = WideCharToMultiByte(CP_UTF8, 0, wPath.c_str(), wideLen, nullptr, 0, nullptr, nullptr);
                    if (utf8Size > 0) {
                        std::string utf8Path(utf8Size, '\0');
                        WideCharToMultiByte(CP_UTF8, 0, wPath.c_str(), wideLen, &utf8Path[0], utf8Size, nullptr, nullptr);
                        result.push_back(utf8Path);
                    }
                }
            }
        }
    }

    CloseClipboard();
    return result;
}

// 模拟复制操作（Ctrl + C）
bool SimulateCopyOperation() {
    INPUT inputs[4] = {};

    // 1. 按下 Ctrl 键
    inputs[0].type = INPUT_KEYBOARD;
    inputs[0].ki.wVk = VK_CONTROL;
    inputs[0].ki.dwFlags = 0;

    // 2. 按下 C 键
    inputs[1].type = INPUT_KEYBOARD;
    inputs[1].ki.wVk = 'C';
    inputs[1].ki.dwFlags = 0;

    // 3. 释放 C 键
    inputs[2].type = INPUT_KEYBOARD;
    inputs[2].ki.wVk = 'C';
    inputs[2].ki.dwFlags = KEYEVENTF_KEYUP;

    // 4. 释放 Ctrl 键
    inputs[3].type = INPUT_KEYBOARD;
    inputs[3].ki.wVk = VK_CONTROL;
    inputs[3].ki.dwFlags = KEYEVENTF_KEYUP;

    UINT result = SendInput(4, inputs, sizeof(INPUT));
    return result == 4;
}

// ==================== 获取选中内容（增强版）====================

// 尝试使用 UI Automation 获取选中文本
std::string TryGetSelectedTextViaUIAutomation() {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool comInitialized = SUCCEEDED(hr);

    IUIAutomation* pAutomation = nullptr;
    IUIAutomationElement* pFocusedElement = nullptr;
    IUIAutomationTextPattern* pTextPattern = nullptr;
    IUIAutomationTextRangeArray* pSelection = nullptr;
    std::string selectedText;

    do {
        hr = CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER,
                              __uuidof(IUIAutomation), (void**)&pAutomation);
        if (FAILED(hr) || !pAutomation) break;

        hr = pAutomation->GetFocusedElement(&pFocusedElement);
        if (FAILED(hr) || !pFocusedElement) break;

        IUnknown* pPatternUnk = nullptr;
        hr = pFocusedElement->GetCurrentPatternAs(UIA_TextPatternId, __uuidof(IUIAutomationTextPattern),
                                                   (void**)&pPatternUnk);
        if (FAILED(hr) || !pPatternUnk) {
            hr = pFocusedElement->GetCurrentPatternAs(UIA_TextPattern2Id, __uuidof(IUIAutomationTextPattern),
                                                       (void**)&pPatternUnk);
            if (FAILED(hr) || !pPatternUnk) break;
        }

        pTextPattern = static_cast<IUIAutomationTextPattern*>(pPatternUnk);

        hr = pTextPattern->GetSelection(&pSelection);
        if (FAILED(hr) || !pSelection) break;

        int selectionCount = 0;
        hr = pSelection->get_Length(&selectionCount);
        if (FAILED(hr) || selectionCount == 0) break;

        for (int i = 0; i < selectionCount; i++) {
            IUIAutomationTextRange* pRange = nullptr;
            hr = pSelection->GetElement(i, &pRange);
            if (SUCCEEDED(hr) && pRange) {
                BSTR bstrText = nullptr;
                hr = pRange->GetText(-1, &bstrText);
                if (SUCCEEDED(hr) && bstrText) {
                    // 使用 SysStringLen 获取实际长度，避免越界写入
                    int wideLen = static_cast<int>(SysStringLen(bstrText));
                    int utf8Size = WideCharToMultiByte(CP_UTF8, 0, bstrText, wideLen, nullptr, 0, nullptr, nullptr);
                    if (utf8Size > 0) {
                        std::string utf8Text(utf8Size, '\0');
                        WideCharToMultiByte(CP_UTF8, 0, bstrText, wideLen, &utf8Text[0], utf8Size, nullptr, nullptr);
                        if (!selectedText.empty()) selectedText += "\n";
                        selectedText += utf8Text;
                    }
                    SysFreeString(bstrText);
                }
                pRange->Release();
            }
        }
    } while (false);

    if (pSelection) pSelection->Release();
    if (pTextPattern) pTextPattern->Release();
    if (pFocusedElement) pFocusedElement->Release();
    if (pAutomation) pAutomation->Release();
    if (comInitialized) CoUninitialize();

    return selectedText;
}

// 获取选中内容（Windows 实现）
Napi::Value GetSelectedContent(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    Napi::Array result = Napi::Array::New(env);

    // 方法1：尝试 UI Automation（适用于标准 Windows 控件）
    std::string uiaText = TryGetSelectedTextViaUIAutomation();
    if (!uiaText.empty()) {
        Napi::Object item = Napi::Object::New(env);
        item.Set("type", "text");
        item.Set("data", uiaText);
        result.Set(uint32_t(0), item);
        return result;
    }

    // 方法2：回退到剪贴板方法（适用于 Electron/Chromium 应用）
    // 暂停监控以防止触发自身事件
    bool wasMonitoring = g_isMonitoring && !g_isPaused;
    if (wasMonitoring) {
        g_isPaused = true;
    }

    // 保存原剪贴板内容
    std::string originalText = GetClipboardTextContent();
    std::string originalImage = GetClipboardImageContent();
    std::vector<std::string> originalFiles = GetClipboardFilesList();

    // 清空剪贴板
    if (OpenClipboard(NULL)) {
        EmptyClipboard();
        CloseClipboard();
    }

    // 模拟 Ctrl+C
    if (SimulateCopyOperation()) {
        // 等待剪贴板更新
        Sleep(100);

        // 读取新的剪贴板内容
        std::string newText = GetClipboardTextContent();
        std::string newImage = GetClipboardImageContent();
        std::vector<std::string> newFiles = GetClipboardFilesList();

        uint32_t index = 0;

        // 检查文本
        if (!newText.empty() && newText != originalText) {
            Napi::Object item = Napi::Object::New(env);
            item.Set("type", "text");
            item.Set("data", newText);
            result.Set(index++, item);
        }

        // 检查文件
        if (!newFiles.empty()) {
            bool isDifferent = (newFiles.size() != originalFiles.size());
            if (!isDifferent) {
                for (size_t i = 0; i < newFiles.size(); i++) {
                    if (newFiles[i] != originalFiles[i]) {
                        isDifferent = true;
                        break;
                    }
                }
            }

            if (isDifferent) {
                Napi::Object item = Napi::Object::New(env);
                item.Set("type", "file");
                Napi::Array fileArray = Napi::Array::New(env);
                for (size_t i = 0; i < newFiles.size(); i++) {
                    fileArray.Set(uint32_t(i), newFiles[i]);
                }
                item.Set("data", fileArray);
                result.Set(index++, item);
            }
        }

        // 检查图像
        if (!newImage.empty() && newImage != originalImage) {
            Napi::Object item = Napi::Object::New(env);
            item.Set("type", "image");
            item.Set("data", newImage);
            item.Set("format", "png");
            item.Set("encoding", "base64");
            result.Set(index++, item);
        }
    }

    // 恢复原剪贴板内容
    if (OpenClipboard(NULL)) {
        EmptyClipboard();

        // 恢复文本
        if (!originalText.empty()) {
            int wideSize = MultiByteToWideChar(CP_UTF8, 0, originalText.c_str(), -1, nullptr, 0);
            if (wideSize > 0) {
                HGLOBAL hGlobal = GlobalAlloc(GMEM_MOVEABLE, wideSize * sizeof(wchar_t));
                if (hGlobal != NULL) {
                    wchar_t* pData = static_cast<wchar_t*>(GlobalLock(hGlobal));
                    if (pData != NULL) {
                        MultiByteToWideChar(CP_UTF8, 0, originalText.c_str(), -1, pData, wideSize);
                        GlobalUnlock(hGlobal);
                        if (SetClipboardData(CF_UNICODETEXT, hGlobal) == NULL) {
                            GlobalFree(hGlobal); // 失败时释放内存
                        }
                    } else {
                        GlobalFree(hGlobal); // GlobalLock 失败时释放内存
                    }
                }
            }
        }

        // 恢复文件列表
        if (!originalFiles.empty()) {
            size_t totalSize = sizeof(DROPFILES);
            for (const auto& file : originalFiles) {
                int wideSize = MultiByteToWideChar(CP_UTF8, 0, file.c_str(), -1, nullptr, 0);
                totalSize += wideSize * sizeof(wchar_t);
            }
            totalSize += sizeof(wchar_t); // 额外的 null terminator

            HGLOBAL hGlobal = GlobalAlloc(GMEM_MOVEABLE, totalSize);
            if (hGlobal != NULL) {
                DROPFILES* pDropFiles = static_cast<DROPFILES*>(GlobalLock(hGlobal));
                if (pDropFiles != NULL) {
                    pDropFiles->pFiles = sizeof(DROPFILES);
                    pDropFiles->pt.x = 0;
                    pDropFiles->pt.y = 0;
                    pDropFiles->fNC = FALSE;
                    pDropFiles->fWide = TRUE;

                    wchar_t* pData = reinterpret_cast<wchar_t*>(reinterpret_cast<BYTE*>(pDropFiles) + sizeof(DROPFILES));
                    size_t remainingChars = (totalSize - sizeof(DROPFILES)) / sizeof(wchar_t);
                    for (const auto& file : originalFiles) {
                        int wideSize = MultiByteToWideChar(CP_UTF8, 0, file.c_str(), -1, pData, static_cast<int>(remainingChars));
                        pData += wideSize;
                        remainingChars -= wideSize;
                    }
                    *pData = L'\0';

                    GlobalUnlock(hGlobal);
                    if (SetClipboardData(CF_HDROP, hGlobal) == NULL) {
                        GlobalFree(hGlobal); // 失败时释放内存
                    }
                } else {
                    GlobalFree(hGlobal); // GlobalLock 失败时释放内存
                }
            }
        }

        CloseClipboard();
    }

    // 恢复监控状态
    if (wasMonitoring) {
        // 延迟恢复，避免立即触发监听
        Sleep(50);
        g_isPaused = false;
    }

    return result;
}

// 模拟粘贴操作（Ctrl + V）
Napi::Value SimulatePaste(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();

    // 创建输入事件数组
    INPUT inputs[4] = {};

    // 1. 按下 Ctrl 键
    inputs[0].type = INPUT_KEYBOARD;
    inputs[0].ki.wVk = VK_CONTROL;
    inputs[0].ki.dwFlags = 0;

    // 2. 按下 V 键
    inputs[1].type = INPUT_KEYBOARD;
    inputs[1].ki.wVk = 'V';
    inputs[1].ki.dwFlags = 0;

    // 3. 释放 V 键
    inputs[2].type = INPUT_KEYBOARD;
    inputs[2].ki.wVk = 'V';
    inputs[2].ki.dwFlags = KEYEVENTF_KEYUP;

    // 4. 释放 Ctrl 键
    inputs[3].type = INPUT_KEYBOARD;
    inputs[3].ki.wVk = VK_CONTROL;
    inputs[3].ki.dwFlags = KEYEVENTF_KEYUP;

    // 发送输入事件
    UINT result = SendInput(4, inputs, sizeof(INPUT));

    // 返回是否成功（应该发送了4个事件）
    return Napi::Boolean::New(env, result == 4);
}

// 模拟键盘按键
Napi::Value SimulateKeyboardTap(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();

    // 参数1：key（必需）
    if (info.Length() < 1 || !info[0].IsString()) {
        Napi::TypeError::New(env, "Expected key as first argument (string)").ThrowAsJavaScriptException();
        return Napi::Boolean::New(env, false);
    }

    std::string key = info[0].As<Napi::String>().Utf8Value();

    // 获取虚拟键码
    WORD vkCode = GetVirtualKeyCode(key);
    if (vkCode == 0) {
        Napi::Error::New(env, "Unknown key: " + key).ThrowAsJavaScriptException();
        return Napi::Boolean::New(env, false);
    }

    // 解析修饰键（可选参数）
    std::vector<WORD> modifierKeys;
    for (size_t i = 1; i < info.Length(); i++) {
        if (info[i].IsString()) {
            std::string modifier = info[i].As<Napi::String>().Utf8Value();
            std::transform(modifier.begin(), modifier.end(), modifier.begin(), ::tolower);

            if (modifier == "shift") {
                modifierKeys.push_back(VK_SHIFT);
            } else if (modifier == "ctrl" || modifier == "control") {
                modifierKeys.push_back(VK_CONTROL);
            } else if (modifier == "alt") {
                modifierKeys.push_back(VK_MENU);
            } else if (modifier == "meta" || modifier == "win" || modifier == "windows") {
                modifierKeys.push_back(VK_LWIN);
            }
        }
    }

    // 计算需要的输入事件数量
    size_t eventCount = modifierKeys.size() * 2 + 2;  // 每个修饰键按下+释放，主键按下+释放
    std::vector<INPUT> inputs(eventCount);
    size_t idx = 0;

    // 1. 按下所有修饰键
    for (WORD modKey : modifierKeys) {
        inputs[idx].type = INPUT_KEYBOARD;
        inputs[idx].ki.wVk = modKey;
        inputs[idx].ki.dwFlags = 0;
        idx++;
    }

    // 2. 按下主键
    inputs[idx].type = INPUT_KEYBOARD;
    inputs[idx].ki.wVk = vkCode;
    inputs[idx].ki.dwFlags = 0;
    idx++;

    // 3. 释放主键
    inputs[idx].type = INPUT_KEYBOARD;
    inputs[idx].ki.wVk = vkCode;
    inputs[idx].ki.dwFlags = KEYEVENTF_KEYUP;
    idx++;

    // 4. 释放所有修饰键（逆序）
    for (auto it = modifierKeys.rbegin(); it != modifierKeys.rend(); ++it) {
        inputs[idx].type = INPUT_KEYBOARD;
        inputs[idx].ki.wVk = *it;
        inputs[idx].ki.dwFlags = KEYEVENTF_KEYUP;
        idx++;
    }

    // 发送输入事件
    UINT result = SendInput(static_cast<UINT>(eventCount), inputs.data(), sizeof(INPUT));

    // 返回是否成功
    return Napi::Boolean::New(env, result == eventCount);
}

// 模拟鼠标移动
Napi::Value SimulateMouseMove(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();

    if (info.Length() < 2 || !info[0].IsNumber() || !info[1].IsNumber()) {
        Napi::TypeError::New(env, "Expected x and y as number arguments").ThrowAsJavaScriptException();
        return Napi::Boolean::New(env, false);
    }

    double x = info[0].As<Napi::Number>().DoubleValue();
    double y = info[1].As<Napi::Number>().DoubleValue();

    // 获取虚拟屏幕（包含所有显示器）的尺寸和偏移
    int screenWidth = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int screenHeight = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    int screenLeft = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int screenTop = GetSystemMetrics(SM_YVIRTUALSCREEN);

    if (screenWidth == 0 || screenHeight == 0) {
        return Napi::Boolean::New(env, false);
    }

    // 将屏幕像素坐标转换为归一化坐标（0-65535）
    INPUT input = {0};
    input.type = INPUT_MOUSE;
    input.mi.dx = static_cast<LONG>(((x - screenLeft) * 65535.0) / (screenWidth - 1));
    input.mi.dy = static_cast<LONG>(((y - screenTop) * 65535.0) / (screenHeight - 1));
    input.mi.dwFlags = MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK | MOUSEEVENTF_MOVE;

    UINT result = SendInput(1, &input, sizeof(INPUT));
    return Napi::Boolean::New(env, result == 1);
}

// 模拟鼠标左键单击
Napi::Value SimulateMouseClick(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();

    if (info.Length() < 2 || !info[0].IsNumber() || !info[1].IsNumber()) {
        Napi::TypeError::New(env, "Expected x and y as number arguments").ThrowAsJavaScriptException();
        return Napi::Boolean::New(env, false);
    }

    double x = info[0].As<Napi::Number>().DoubleValue();
    double y = info[1].As<Napi::Number>().DoubleValue();

    int screenWidth = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int screenHeight = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    int screenLeft = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int screenTop = GetSystemMetrics(SM_YVIRTUALSCREEN);

    if (screenWidth == 0 || screenHeight == 0) {
        return Napi::Boolean::New(env, false);
    }

    LONG normX = static_cast<LONG>(((x - screenLeft) * 65535.0) / (screenWidth - 1));
    LONG normY = static_cast<LONG>(((y - screenTop) * 65535.0) / (screenHeight - 1));

    INPUT inputs[3] = {};

    // 移动到目标位置
    inputs[0].type = INPUT_MOUSE;
    inputs[0].mi.dx = normX;
    inputs[0].mi.dy = normY;
    inputs[0].mi.dwFlags = MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK | MOUSEEVENTF_MOVE;

    // 按下左键
    inputs[1].type = INPUT_MOUSE;
    inputs[1].mi.dx = normX;
    inputs[1].mi.dy = normY;
    inputs[1].mi.dwFlags = MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK | MOUSEEVENTF_LEFTDOWN;

    // 释放左键
    inputs[2].type = INPUT_MOUSE;
    inputs[2].mi.dx = normX;
    inputs[2].mi.dy = normY;
    inputs[2].mi.dwFlags = MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK | MOUSEEVENTF_LEFTUP;

    UINT result = SendInput(3, inputs, sizeof(INPUT));
    return Napi::Boolean::New(env, result == 3);
}

// 模拟鼠标左键双击
Napi::Value SimulateMouseDoubleClick(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();

    if (info.Length() < 2 || !info[0].IsNumber() || !info[1].IsNumber()) {
        Napi::TypeError::New(env, "Expected x and y as number arguments").ThrowAsJavaScriptException();
        return Napi::Boolean::New(env, false);
    }

    double x = info[0].As<Napi::Number>().DoubleValue();
    double y = info[1].As<Napi::Number>().DoubleValue();

    int screenWidth = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int screenHeight = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    int screenLeft = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int screenTop = GetSystemMetrics(SM_YVIRTUALSCREEN);

    if (screenWidth == 0 || screenHeight == 0) {
        return Napi::Boolean::New(env, false);
    }

    LONG normX = static_cast<LONG>(((x - screenLeft) * 65535.0) / (screenWidth - 1));
    LONG normY = static_cast<LONG>(((y - screenTop) * 65535.0) / (screenHeight - 1));

    INPUT inputs[5] = {};

    // 移动到目标位置
    inputs[0].type = INPUT_MOUSE;
    inputs[0].mi.dx = normX;
    inputs[0].mi.dy = normY;
    inputs[0].mi.dwFlags = MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK | MOUSEEVENTF_MOVE;

    // 第一次点击
    inputs[1].type = INPUT_MOUSE;
    inputs[1].mi.dx = normX;
    inputs[1].mi.dy = normY;
    inputs[1].mi.dwFlags = MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK | MOUSEEVENTF_LEFTDOWN;

    inputs[2].type = INPUT_MOUSE;
    inputs[2].mi.dx = normX;
    inputs[2].mi.dy = normY;
    inputs[2].mi.dwFlags = MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK | MOUSEEVENTF_LEFTUP;

    // 第二次点击
    inputs[3].type = INPUT_MOUSE;
    inputs[3].mi.dx = normX;
    inputs[3].mi.dy = normY;
    inputs[3].mi.dwFlags = MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK | MOUSEEVENTF_LEFTDOWN;

    inputs[4].type = INPUT_MOUSE;
    inputs[4].mi.dx = normX;
    inputs[4].mi.dy = normY;
    inputs[4].mi.dwFlags = MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK | MOUSEEVENTF_LEFTUP;

    UINT result = SendInput(5, inputs, sizeof(INPUT));
    return Napi::Boolean::New(env, result == 5);
}

// 模拟鼠标右键单击
Napi::Value SimulateMouseRightClick(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();

    if (info.Length() < 2 || !info[0].IsNumber() || !info[1].IsNumber()) {
        Napi::TypeError::New(env, "Expected x and y as number arguments").ThrowAsJavaScriptException();
        return Napi::Boolean::New(env, false);
    }

    double x = info[0].As<Napi::Number>().DoubleValue();
    double y = info[1].As<Napi::Number>().DoubleValue();

    int screenWidth = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int screenHeight = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    int screenLeft = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int screenTop = GetSystemMetrics(SM_YVIRTUALSCREEN);

    if (screenWidth == 0 || screenHeight == 0) {
        return Napi::Boolean::New(env, false);
    }

    LONG normX = static_cast<LONG>(((x - screenLeft) * 65535.0) / (screenWidth - 1));
    LONG normY = static_cast<LONG>(((y - screenTop) * 65535.0) / (screenHeight - 1));

    INPUT inputs[3] = {};

    // 移动到目标位置
    inputs[0].type = INPUT_MOUSE;
    inputs[0].mi.dx = normX;
    inputs[0].mi.dy = normY;
    inputs[0].mi.dwFlags = MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK | MOUSEEVENTF_MOVE;

    // 按下右键
    inputs[1].type = INPUT_MOUSE;
    inputs[1].mi.dx = normX;
    inputs[1].mi.dy = normY;
    inputs[1].mi.dwFlags = MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK | MOUSEEVENTF_RIGHTDOWN;

    // 释放右键
    inputs[2].type = INPUT_MOUSE;
    inputs[2].mi.dx = normX;
    inputs[2].mi.dy = normY;
    inputs[2].mi.dwFlags = MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK | MOUSEEVENTF_RIGHTUP;

    UINT result = SendInput(3, inputs, sizeof(INPUT));
    return Napi::Boolean::New(env, result == 3);
}

// ==================== UWP 应用功能 ====================

// 辅助函数：宽字符串转 UTF-8
static std::string WideToUtf8(const std::wstring& wstr) {
    if (wstr.empty()) return "";
    int size = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, NULL, 0, NULL, NULL);
    if (size <= 0) return "";
    std::string utf8(size - 1, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, &utf8[0], size, NULL, NULL);
    return utf8;
}

// 辅助函数：解码 XML 实体（&amp; &#xHHHH; &#DDD; 等）
static std::wstring DecodeXmlEntities(const std::wstring& input) {
    std::wstring result;
    result.reserve(input.size());
    size_t i = 0;
    while (i < input.size()) {
        if (input[i] == L'&') {
            size_t semi = input.find(L';', i + 1);
            if (semi != std::wstring::npos && semi - i < 12) {
                std::wstring entity = input.substr(i + 1, semi - i - 1);
                if (entity == L"amp") {
                    result += L'&';
                } else if (entity == L"lt") {
                    result += L'<';
                } else if (entity == L"gt") {
                    result += L'>';
                } else if (entity == L"quot") {
                    result += L'"';
                } else if (entity == L"apos") {
                    result += L'\'';
                } else if (entity.size() > 1 && entity[0] == L'#') {
                    // 数字字符引用
                    unsigned long codePoint = 0;
                    if (entity[1] == L'x' || entity[1] == L'X') {
                        // 十六进制 &#xHHHH;
                        codePoint = wcstoul(entity.c_str() + 2, nullptr, 16);
                    } else {
                        // 十进制 &#DDDD;
                        codePoint = wcstoul(entity.c_str() + 1, nullptr, 10);
                    }
                    if (codePoint > 0 && codePoint <= 0xFFFF) {
                        result += static_cast<wchar_t>(codePoint);
                    } else if (codePoint > 0xFFFF && codePoint <= 0x10FFFF) {
                        // UTF-16 代理对
                        codePoint -= 0x10000;
                        result += static_cast<wchar_t>(0xD800 + (codePoint >> 10));
                        result += static_cast<wchar_t>(0xDC00 + (codePoint & 0x3FF));
                    } else {
                        // 无效的代码点，保留原样
                        result += input.substr(i, semi - i + 1);
                    }
                } else {
                    // 未知实体，保留原样
                    result += input.substr(i, semi - i + 1);
                }
                i = semi + 1;
                continue;
            }
        }
        result += input[i];
        i++;
    }
    return result;
}

// 辅助函数：解析 ms-resource 间接字符串
static std::wstring ResolveIndirectString(const std::wstring& raw, const std::wstring& packageFullName = L"", const std::wstring& msResource = L"") {
    // 如果是 @{ 开头的间接字符串，使用 SHLoadIndirectString 解析
    if (!raw.empty() && raw[0] == L'@') {
        WCHAR resolved[512] = {0};
        HRESULT hr = SHLoadIndirectString(raw.c_str(), resolved, 512, NULL);
        if (SUCCEEDED(hr) && resolved[0] != L'\0') {
            return std::wstring(resolved);
        }
    }
    // 如果提供了 packageFullName 和 ms-resource，尝试构造间接字符串解析
    if (!packageFullName.empty() && !msResource.empty()) {
        // 尝试1: 直接用原始 ms-resource
        // @{PackageFullName?ms-resource://PackageName/Resources/Name}
        std::wstring indirectStr = L"@{" + packageFullName + L"?" + msResource + L"}";
        WCHAR resolved[512] = {0};
        HRESULT hr = SHLoadIndirectString(indirectStr.c_str(), resolved, 512, NULL);
        if (SUCCEEDED(hr) && resolved[0] != L'\0') {
            return std::wstring(resolved);
        }

        // 尝试2: 如果是短格式 ms-resource:Name，补全为 ms-resource:///Resources/Name
        if (msResource.find(L"ms-resource:") == 0 && msResource.find(L"ms-resource://") == std::wstring::npos) {
            std::wstring resourceName = msResource.substr(12); // 去掉 "ms-resource:"
            // 可能包含路径如 ms-resource:Clipchamp/AppName
            std::wstring fullResource = L"ms-resource:///Resources/" + resourceName;
            indirectStr = L"@{" + packageFullName + L"?" + fullResource + L"}";
            memset(resolved, 0, sizeof(resolved));
            hr = SHLoadIndirectString(indirectStr.c_str(), resolved, 512, NULL);
            if (SUCCEEDED(hr) && resolved[0] != L'\0') {
                return std::wstring(resolved);
            }

            // 尝试3: ms-resource:///Name（不加 Resources 前缀）
            fullResource = L"ms-resource:///" + resourceName;
            indirectStr = L"@{" + packageFullName + L"?" + fullResource + L"}";
            memset(resolved, 0, sizeof(resolved));
            hr = SHLoadIndirectString(indirectStr.c_str(), resolved, 512, NULL);
            if (SUCCEEDED(hr) && resolved[0] != L'\0') {
                return std::wstring(resolved);
            }
        }
    }
    // 如果以 @ 开头且无法解析，返回空字符串（表示解析失败）
    if (!raw.empty() && raw[0] == L'@') {
        return L"";
    }
    return raw;
}

// 辅助函数：查找应用图标的最佳路径
static std::wstring FindBestLogo(const std::wstring& installLocation, const std::wstring& logoRelPath) {
    if (installLocation.empty() || logoRelPath.empty()) return L"";

    // 按照优先级尝试不同的 scale 后缀
    const wchar_t* scales[] = {
        L".scale-100", L".scale-125", L".scale-150",
        L".scale-200", L".scale-400"
    };

    // 尝试 targetsize 变体
    const wchar_t* sizes[] = {
        L".targetsize-48", L".targetsize-64", L".targetsize-96",
        L".targetsize-256", L".targetsize-32", L".targetsize-24",
        L".targetsize-16"
    };

    auto tryResolvePath = [&](const std::wstring& fullPath) -> std::wstring {
        if (GetFileAttributesW(fullPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
            return fullPath;
        }

        // UWP 图标常见的缩放后缀变体
        // 例如 Assets\StoreLogo.png -> Assets\StoreLogo.scale-100.png
        size_t dotPos = fullPath.find_last_of(L'.');
        if (dotPos == std::wstring::npos) return L"";

        std::wstring basePath = fullPath.substr(0, dotPos);
        std::wstring ext = fullPath.substr(dotPos);

        for (const wchar_t* scale : scales) {
            std::wstring candidate = basePath + scale + ext;
            if (GetFileAttributesW(candidate.c_str()) != INVALID_FILE_ATTRIBUTES) {
                return candidate;
            }
        }

        for (const wchar_t* sz : sizes) {
            std::wstring candidate = basePath + sz + ext;
            if (GetFileAttributesW(candidate.c_str()) != INVALID_FILE_ATTRIBUTES) {
                return candidate;
            }
        }

        for (const wchar_t* sz : sizes) {
            std::wstring candidate = basePath + sz + L"_altform-unplated" + ext;
            if (GetFileAttributesW(candidate.c_str()) != INVALID_FILE_ATTRIBUTES) {
                return candidate;
            }
        }

        return L"";
    };

    std::vector<std::wstring> relativeCandidates;
    relativeCandidates.push_back(logoRelPath);

    // 某些包的 manifest 写的是 PRODUCTION\Logo.png，但实际文件位于 images\PRODUCTION\...
    if (logoRelPath.find(L"images\\") != 0 && logoRelPath.find(L"Images\\") != 0) {
        relativeCandidates.push_back(L"images\\" + logoRelPath);
        relativeCandidates.push_back(L"Images\\" + logoRelPath);
    }

    for (const auto& relativePath : relativeCandidates) {
        std::wstring resolved = tryResolvePath(installLocation + L"\\" + relativePath);
        if (!resolved.empty()) {
            return resolved;
        }
    }

    return L"";
}

// 辅助函数：从包全名提取 PackageFamilyName
// 包全名格式: Name_Version_Arch_ResourceId_PublisherId 或 Name_Version_Arch__PublisherId
// PackageFamilyName: Name_PublisherId
static std::wstring GetPackageFamilyNameFromFullName(const std::wstring& fullName) {
    // 找到第一个下划线（Name 结尾）
    size_t firstUnderscore = fullName.find(L'_');
    if (firstUnderscore == std::wstring::npos) return fullName;

    std::wstring name = fullName.substr(0, firstUnderscore);

    // 找到最后一个下划线后面的 PublisherId
    size_t lastUnderscore = fullName.find_last_of(L'_');
    if (lastUnderscore == std::wstring::npos || lastUnderscore == firstUnderscore) return fullName;

    std::wstring publisherId = fullName.substr(lastUnderscore + 1);

    return name + L"_" + publisherId;
}

// 辅助函数：简单 XML 属性提取
static std::wstring GetXmlAttribute(const std::wstring& xml, const std::wstring& tag, const std::wstring& attr) {
    // 查找 <tag ... attr="value" ...>
    size_t searchPos = 0;
    while (searchPos < xml.size()) {
        size_t tagStart = xml.find(L"<" + tag, searchPos);
        if (tagStart == std::wstring::npos) break;

        size_t tagEnd = xml.find(L'>', tagStart);
        if (tagEnd == std::wstring::npos) break;

        std::wstring tagContent = xml.substr(tagStart, tagEnd - tagStart + 1);
        std::wstring attrSearch = attr + L"=\"";
        size_t attrPos = tagContent.find(attrSearch);
        if (attrPos != std::wstring::npos) {
            size_t valueStart = attrPos + attrSearch.length();
            size_t valueEnd = tagContent.find(L'"', valueStart);
            if (valueEnd != std::wstring::npos) {
                return tagContent.substr(valueStart, valueEnd - valueStart);
            }
        }
        searchPos = tagEnd + 1;
    }
    return L"";
}

// 辅助函数：读取文件内容为宽字符串
static std::wstring ReadFileToWString(const std::wstring& path) {
    HANDLE hFile = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (hFile == INVALID_HANDLE_VALUE) return L"";

    DWORD fileSize = GetFileSize(hFile, NULL);
    if (fileSize == INVALID_FILE_SIZE || fileSize == 0) {
        CloseHandle(hFile);
        return L"";
    }

    std::vector<char> buffer(fileSize + 1, 0);
    DWORD bytesRead = 0;
    if (!ReadFile(hFile, buffer.data(), fileSize, &bytesRead, NULL)) {
        CloseHandle(hFile);
        return L"";
    }
    CloseHandle(hFile);

    // 将 UTF-8 转换为宽字符串
    int wideLen = MultiByteToWideChar(CP_UTF8, 0, buffer.data(), bytesRead, NULL, 0);
    if (wideLen <= 0) return L"";

    std::wstring result(wideLen, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, buffer.data(), bytesRead, &result[0], wideLen);
    return result;
}

// 获取 UWP 应用列表
Napi::Value GetUwpApps(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    Napi::Array result = Napi::Array::New(env);

    // 打开注册表枚举已安装的 UWP 包
    HKEY hKeyRepo = NULL;
    LONG regResult = RegOpenKeyExW(
        HKEY_CURRENT_USER,
        L"Software\\Classes\\Local Settings\\Software\\Microsoft\\Windows\\CurrentVersion\\AppModel\\Repository\\Packages",
        0, KEY_READ, &hKeyRepo
    );

    if (regResult != ERROR_SUCCESS) {
        return result;
    }

    DWORD subKeyCount = 0;
    RegQueryInfoKeyW(hKeyRepo, NULL, NULL, NULL, &subKeyCount, NULL, NULL, NULL, NULL, NULL, NULL, NULL);

    uint32_t appIndex = 0;

    for (DWORD i = 0; i < subKeyCount; i++) {
        WCHAR subKeyName[512] = {0};
        DWORD subKeyNameLen = 512;
        if (RegEnumKeyExW(hKeyRepo, i, subKeyName, &subKeyNameLen, NULL, NULL, NULL, NULL) != ERROR_SUCCESS) {
            continue;
        }

        // 打开包子键
        HKEY hKeyPkg = NULL;
        if (RegOpenKeyExW(hKeyRepo, subKeyName, 0, KEY_READ, &hKeyPkg) != ERROR_SUCCESS) {
            continue;
        }

        // 读取 PackageRootFolder（安装路径）
        WCHAR installLocation[1024] = {0};
        DWORD installLocSize = sizeof(installLocation);
        if (RegQueryValueExW(hKeyPkg, L"PackageRootFolder", NULL, NULL, (LPBYTE)installLocation, &installLocSize) != ERROR_SUCCESS) {
            RegCloseKey(hKeyPkg);
            continue;
        }

        // 读取 DisplayName（可能是 @{...?ms-resource:...} 间接字符串）
        WCHAR displayName[512] = {0};
        DWORD displayNameSize = sizeof(displayName);
        RegQueryValueExW(hKeyPkg, L"DisplayName", NULL, NULL, (LPBYTE)displayName, &displayNameSize);

        RegCloseKey(hKeyPkg);

        // 读取 AppxManifest.xml
        std::wstring manifestPath = std::wstring(installLocation) + L"\\AppxManifest.xml";
        std::wstring manifestContent = ReadFileToWString(manifestPath);
        if (manifestContent.empty()) {
            continue;
        }

        // 跳过没有 <Applications> 的框架包
        if (manifestContent.find(L"<Applications>") == std::wstring::npos) {
            continue;
        }

        // 提取 PackageFamilyName
        std::wstring packageFullName(subKeyName);
        std::wstring familyName = GetPackageFamilyNameFromFullName(packageFullName);

        // 解析 DisplayName
        // 先尝试从 manifest 中获取 DisplayName 的 ms-resource 用于更好的解析
        std::wstring manifestDisplayName = GetXmlAttribute(manifestContent, L"Properties", L"");
        // 从 <DisplayName> 标签中获取值
        size_t dnStart = manifestContent.find(L"<DisplayName>");
        size_t dnEnd = manifestContent.find(L"</DisplayName>");
        std::wstring msResourceName;
        if (dnStart != std::wstring::npos && dnEnd != std::wstring::npos) {
            dnStart += 13; // len("<DisplayName>")
            msResourceName = DecodeXmlEntities(manifestContent.substr(dnStart, dnEnd - dnStart));
        }

        std::wstring resolvedName = ResolveIndirectString(std::wstring(displayName), packageFullName, msResourceName);
        if (resolvedName.empty() && !msResourceName.empty()) {
            // 再尝试用 manifest 的 ms-resource
            resolvedName = ResolveIndirectString(L"", packageFullName, msResourceName);
        }
        if (resolvedName.empty()) {
            resolvedName = familyName;
        }
        // 解码包级别名称中可能存在的 XML 实体
        resolvedName = DecodeXmlEntities(resolvedName);

        // 从 manifest 中提取所有 Application 条目
        size_t searchPos = 0;
        while (searchPos < manifestContent.size()) {
            size_t appTagStart = manifestContent.find(L"<Application ", searchPos);
            if (appTagStart == std::wstring::npos) break;

            // 找到这个 Application 标签结束的位置
            size_t appBlockEnd = manifestContent.find(L"</Application>", appTagStart);
            if (appBlockEnd == std::wstring::npos) {
                // 可能是自闭合标签
                appBlockEnd = manifestContent.find(L"/>", appTagStart);
                if (appBlockEnd == std::wstring::npos) break;
                appBlockEnd += 2;
            } else {
                appBlockEnd += 14; // len("</Application>")
            }

            std::wstring appBlock = manifestContent.substr(appTagStart, appBlockEnd - appTagStart);

            // 提取 Application Id
            std::wstring appId = GetXmlAttribute(appBlock, L"Application", L"Id");
            if (appId.empty()) {
                searchPos = appBlockEnd;
                continue;
            }
            std::wstring executableRelPath = GetXmlAttribute(appBlock, L"Application", L"Executable");

            // 检查 AppListEntry 属性，跳过标记为 "none" 的内部入口
            std::wstring appListEntry = GetXmlAttribute(appBlock, L"uap:VisualElements", L"AppListEntry");
            if (appListEntry.empty()) {
                appListEntry = GetXmlAttribute(appBlock, L"VisualElements", L"AppListEntry");
            }
            if (appListEntry == L"none") {
                searchPos = appBlockEnd;
                continue;
            }

            // 构建 AppUserModelID: PackageFamilyName!ApplicationId
            std::wstring aumid = familyName + L"!" + appId;

            // 优先从 Application 的 VisualElements 中读取 DisplayName（每个入口可能不同）
            std::wstring appDisplayName;
            std::wstring veDisplayName = GetXmlAttribute(appBlock, L"uap:VisualElements", L"DisplayName");
            if (veDisplayName.empty()) {
                veDisplayName = GetXmlAttribute(appBlock, L"VisualElements", L"DisplayName");
            }
            if (!veDisplayName.empty()) {
                // 先解码 XML 实体（如 &amp; &#x7535; 等）
                veDisplayName = DecodeXmlEntities(veDisplayName);
                // 可能是 ms-resource:XXX 格式，需要解析
                if (veDisplayName.find(L"ms-resource:") == 0) {
                    appDisplayName = ResolveIndirectString(L"", packageFullName, veDisplayName);
                } else {
                    appDisplayName = veDisplayName;
                }
            }
            // 如果 VisualElements 中解析失败，回退到包级别名称
            if (appDisplayName.empty()) {
                appDisplayName = resolvedName;
            }

            // 提取图标路径（从 VisualElements 或 uap:VisualElements）
            std::wstring logoRelPath;
            // 先尝试 Square44x44Logo（应用列表图标）
            logoRelPath = GetXmlAttribute(appBlock, L"uap:VisualElements", L"Square44x44Logo");
            if (logoRelPath.empty()) {
                logoRelPath = GetXmlAttribute(appBlock, L"VisualElements", L"Square44x44Logo");
            }
            // 如果没有 44x44，尝试 150x150
            if (logoRelPath.empty()) {
                logoRelPath = GetXmlAttribute(appBlock, L"uap:VisualElements", L"Square150x150Logo");
                if (logoRelPath.empty()) {
                    logoRelPath = GetXmlAttribute(appBlock, L"VisualElements", L"Square150x150Logo");
                }
            }

            // 查找实际的图标文件
            std::wstring iconFullPath = FindBestLogo(std::wstring(installLocation), logoRelPath);
            if (iconFullPath.empty()
                && !executableRelPath.empty()
                && std::wstring(installLocation).find(L"\\WindowsApps\\") != std::wstring::npos) {
                std::wstring executableFullPath = std::wstring(installLocation) + L"\\" + executableRelPath;
                if (GetFileAttributesW(executableFullPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
                    iconFullPath = executableFullPath;
                }
            }

            // 跳过没有图标的应用（通常是系统基础设施组件，如 Win32WebViewHost）
            if (iconFullPath.empty()) {
                searchPos = appBlockEnd;
                continue;
            }

            // 创建应用信息对象
            Napi::Object appInfo = Napi::Object::New(env);
            appInfo.Set("name", Napi::String::New(env, WideToUtf8(appDisplayName)));
            appInfo.Set("appId", Napi::String::New(env, WideToUtf8(aumid)));
            appInfo.Set("icon", Napi::String::New(env, WideToUtf8(iconFullPath)));
            appInfo.Set("installLocation", Napi::String::New(env, WideToUtf8(std::wstring(installLocation))));

            result.Set(appIndex++, appInfo);

            searchPos = appBlockEnd;
        }
    }

    RegCloseKey(hKeyRepo);
    return result;
}

// 启动 UWP 应用
Napi::Value LaunchUwpApp(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();

    if (info.Length() < 1 || !info[0].IsString()) {
        Napi::TypeError::New(env, "Expected appId (AppUserModelID) as first argument").ThrowAsJavaScriptException();
        return Napi::Boolean::New(env, false);
    }

    std::string appIdUtf8 = info[0].As<Napi::String>().Utf8Value();

    // 转换为宽字符
    int wideSize = MultiByteToWideChar(CP_UTF8, 0, appIdUtf8.c_str(), -1, NULL, 0);
    if (wideSize <= 0) {
        return Napi::Boolean::New(env, false);
    }
    std::wstring appIdWide(wideSize - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, appIdUtf8.c_str(), -1, &appIdWide[0], wideSize);

    // 使用 IApplicationActivationManager 启动 UWP 应用
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    IApplicationActivationManager* paam = nullptr;
    HRESULT hr = CoCreateInstance(
        CLSID_ApplicationActivationManager,
        nullptr,
        CLSCTX_LOCAL_SERVER,
        IID_IApplicationActivationManager,
        (void**)&paam
    );

    if (FAILED(hr) || paam == nullptr) {
        CoUninitialize();
        return Napi::Boolean::New(env, false);
    }

    DWORD pid = 0;
    hr = paam->ActivateApplication(appIdWide.c_str(), nullptr, AO_NONE, &pid);

    paam->Release();
    CoUninitialize();

    return Napi::Boolean::New(env, SUCCEEDED(hr));
}

// ==================== 应用图标提取 ====================

// GDI+ 初始化/反初始化 RAII
class GdiPlusInit {
public:
    GdiPlusInit() {
        Gdiplus::GdiplusStartupInput startupInput;
        Gdiplus::GdiplusStartup(std::addressof(this->token), std::addressof(startupInput), nullptr);
    }
    ~GdiPlusInit() { Gdiplus::GdiplusShutdown(this->token); }
private:
    GdiPlusInit(const GdiPlusInit&);
    GdiPlusInit& operator=(const GdiPlusInit&);
    ULONG_PTR token;
};

struct IStreamDeleter {
    void operator()(IStream* pStream) const { pStream->Release(); }
};

// 从 HICON 创建带 Alpha 通道的 Bitmap
static std::unique_ptr<Gdiplus::Bitmap> CreateBitmapFromIcon(
    HICON hIcon, std::vector<std::int32_t>& buffer) {
    ICONINFO iconInfo = {0};
    GetIconInfo(hIcon, std::addressof(iconInfo));

    BITMAP bm = {0};
    GetObject(iconInfo.hbmColor, sizeof(bm), std::addressof(bm));

    std::unique_ptr<Gdiplus::Bitmap> bitmap;

    if (bm.bmBitsPixel == 32) {
        auto hDC = GetDC(nullptr);

        BITMAPINFO bmi = {0};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = bm.bmWidth;
        bmi.bmiHeader.biHeight = -bm.bmHeight;
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        auto nBits = bm.bmWidth * bm.bmHeight;
        buffer.resize(nBits);
        GetDIBits(hDC, iconInfo.hbmColor, 0, bm.bmHeight,
                  std::addressof(buffer[0]), std::addressof(bmi), DIB_RGB_COLORS);

        auto hasAlpha = false;
        for (std::int32_t i = 0; i < nBits; i++) {
            if ((buffer[i] & 0xFF000000) != 0) {
                hasAlpha = true;
                break;
            }
        }

        if (!hasAlpha) {
            std::vector<std::int32_t> maskBits(nBits);
            GetDIBits(hDC, iconInfo.hbmMask, 0, bm.bmHeight,
                      std::addressof(maskBits[0]), std::addressof(bmi), DIB_RGB_COLORS);
            for (std::int32_t i = 0; i < nBits; i++) {
                if (maskBits[i] == 0) {
                    buffer[i] |= 0xFF000000;
                }
            }
        }

        bitmap.reset(new Gdiplus::Bitmap(
            bm.bmWidth, bm.bmHeight, bm.bmWidth * sizeof(std::int32_t),
            PixelFormat32bppARGB,
            static_cast<BYTE*>(static_cast<void*>(std::addressof(buffer[0])))));

        ReleaseDC(nullptr, hDC);
    } else {
        bitmap.reset(Gdiplus::Bitmap::FromHICON(hIcon));
    }

    DeleteObject(iconInfo.hbmColor);
    DeleteObject(iconInfo.hbmMask);

    return bitmap;
}

// 获取 PNG 编码器 CLSID
int GetPngEncoderClsid(CLSID* pClsid) {
    UINT num = 0u;
    UINT size = 0u;
    Gdiplus::GetImageEncodersSize(std::addressof(num), std::addressof(size));
    if (size == 0u) return -1;

    std::unique_ptr<Gdiplus::ImageCodecInfo> pImageCodecInfo(
        static_cast<Gdiplus::ImageCodecInfo*>(static_cast<void*>(new BYTE[size])));
    if (pImageCodecInfo == nullptr) return -1;

    Gdiplus::GetImageEncoders(num, size, pImageCodecInfo.get());

    for (UINT i = 0u; i < num; i++) {
        if (std::wcscmp(pImageCodecInfo.get()[i].MimeType, L"image/png") == 0) {
            *pClsid = pImageCodecInfo.get()[i].Clsid;
            return (int)i;
        }
    }
    return -1;
}

// 将 HICON 转换为 PNG 字节数组
static std::vector<unsigned char> HIconToPNG(HICON hIcon) {
    GdiPlusInit init;

    std::vector<std::int32_t> buffer;
    auto bitmap = CreateBitmapFromIcon(hIcon, buffer);

    CLSID encoder;
    if (GetPngEncoderClsid(std::addressof(encoder)) == -1) {
        return std::vector<unsigned char>{};
    }

    IStream* tmp;
    if (CreateStreamOnHGlobal(nullptr, TRUE, std::addressof(tmp)) != S_OK) {
        return std::vector<unsigned char>{};
    }
    std::unique_ptr<IStream, IStreamDeleter> pStream{tmp};

    if (bitmap->Save(pStream.get(), std::addressof(encoder), nullptr) != Gdiplus::Status::Ok) {
        return std::vector<unsigned char>{};
    }

    STATSTG stg = {0};
    LARGE_INTEGER offset = {0};
    if (pStream->Stat(std::addressof(stg), STATFLAG_NONAME) != S_OK ||
        pStream->Seek(offset, STREAM_SEEK_SET, nullptr) != S_OK) {
        return std::vector<unsigned char>{};
    }

    std::vector<unsigned char> result(static_cast<std::size_t>(stg.cbSize.QuadPart));
    ULONG ul;
    if (pStream->Read(std::addressof(result[0]),
                      static_cast<ULONG>(stg.cbSize.QuadPart), std::addressof(ul)) != S_OK ||
        stg.cbSize.QuadPart != ul) {
        return std::vector<unsigned char>{};
    }

    return result;
}

// .lnk 快捷方式解析结果
struct LnkIconInfo {
    std::wstring targetPath;    // 快捷方式目标路径
    std::wstring iconLocation;  // 自定义图标路径
    int iconIndex;              // 自定义图标索引
    DWORD targetAttributes;     // 目标文件属性（来自 .lnk 存储的数据）
};

// 解析 .lnk 快捷方式（使用独立 STA 线程，IShellLink 需要 COM STA）
static LnkIconInfo ResolveLnkInfo(const std::wstring& lnkPath) {
    LnkIconInfo info = { L"", L"", 0, 0 };

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    IShellLinkW* pShellLink = nullptr;
    IPersistFile* pPersistFile = nullptr;

    HRESULT hr = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_IShellLinkW, reinterpret_cast<void**>(&pShellLink));
    if (SUCCEEDED(hr) && pShellLink) {
        hr = pShellLink->QueryInterface(IID_IPersistFile, reinterpret_cast<void**>(&pPersistFile));
        if (SUCCEEDED(hr) && pPersistFile) {
            hr = pPersistFile->Load(lnkPath.c_str(), STGM_READ);
            if (SUCCEEDED(hr)) {
                // 获取自定义图标位置
                WCHAR iconPath[MAX_PATH] = {0};
                int iconIdx = 0;
                hr = pShellLink->GetIconLocation(iconPath, MAX_PATH, &iconIdx);
                if (SUCCEEDED(hr) && iconPath[0] != L'\0') {
                    // 展开环境变量（如 %SystemRoot%）
                    WCHAR expandedIconPath[MAX_PATH] = {0};
                    DWORD expandedLen = ExpandEnvironmentStringsW(iconPath, expandedIconPath, MAX_PATH);
                    if (expandedLen > 0 && expandedLen <= MAX_PATH) {
                        info.iconLocation = expandedIconPath;
                    } else {
                        info.iconLocation = iconPath;
                    }
                    info.iconIndex = iconIdx;
                }

                // 获取目标路径（使用默认标志以展开环境变量）
                WCHAR targetPath[MAX_PATH] = {0};
                WIN32_FIND_DATAW findData = {0};
                hr = pShellLink->GetPath(targetPath, MAX_PATH, &findData, 0);
                if (SUCCEEDED(hr) && targetPath[0] != L'\0') {
                    info.targetPath = targetPath;
                    info.targetAttributes = findData.dwFileAttributes;
                }
            }
            pPersistFile->Release();
        }
        pShellLink->Release();
    }

    CoUninitialize();
    return info;
}

// 判断文件扩展名是否为 .lnk（不区分大小写）
static bool IsLnkFile(const std::wstring& path) {
    if (path.size() < 4) return false;
    std::wstring ext = path.substr(path.size() - 4);
    for (auto& c : ext) c = towlower(c);
    return ext == L".lnk";
}

// 判断是否为网络路径（UNC 路径或映射的网络驱动器）
static bool IsNetworkPath(const std::wstring& path) {
    // UNC 路径: \\server\share\...
    if (path.size() >= 2 && path[0] == L'\\' && path[1] == L'\\') {
        return true;
    }
    // 映射的网络驱动器: Z:\...
    if (path.size() >= 3 && iswalpha(path[0]) && path[1] == L':' && (path[2] == L'\\' || path[2] == L'/')) {
        std::wstring root = path.substr(0, 3);
        return GetDriveTypeW(root.c_str()) == DRIVE_REMOTE;
    }
    return false;
}

// 从文件路径提取图标 (PNG Buffer)
// 参数: path (string), size (number: 16 | 32 | 64 | 256)
static std::vector<unsigned char> ExtractIconFromPath(std::wstring widePath, int size) {
    // 如果是 .lnk 快捷方式，解析自定义图标或目标路径
    DWORD targetAttrs = 0;
    if (IsLnkFile(widePath)) {
        LnkIconInfo lnkInfo = ResolveLnkInfo(widePath);

        // 优先使用快捷方式自定义图标（PrivateExtractIconsW 直接提取，无叠加箭头）
        // 跳过网络路径上的图标文件，避免网络不可达时长时间阻塞
        if (!lnkInfo.iconLocation.empty() && !IsNetworkPath(lnkInfo.iconLocation)) {
            HICON hIcon = nullptr;
            UINT extracted = PrivateExtractIconsW(
                lnkInfo.iconLocation.c_str(), lnkInfo.iconIndex,
                size, size, &hIcon, nullptr, 1, 0);
            if (extracted > 0 && hIcon) {
                auto pngData = HIconToPNG(hIcon);
                DestroyIcon(hIcon);
                return pngData;
            }
        }

        // 回退：使用目标路径（避免 SHGetFileInfoW 对 .lnk 叠加箭头）
        if (!lnkInfo.targetPath.empty()) {
            widePath = lnkInfo.targetPath;
            targetAttrs = lnkInfo.targetAttributes;
        }
    }

    UINT flag = SHGFI_ICON;

    switch (size) {
        case 16:
            flag |= SHGFI_SMALLICON;
            break;
        case 32:
            flag |= SHGFI_LARGEICON;
            break;
        case 64:
        case 256:
            flag |= SHGFI_SYSICONINDEX;
            break;
        default:
            flag |= SHGFI_LARGEICON;
            break;
    }

    SHFILEINFOW sfi = {0};
    HICON hIcon = nullptr;

    // 网络路径优化：使用 SHGFI_USEFILEATTRIBUTES 根据扩展名获取关联图标，避免网络 I/O
    bool isNetwork = IsNetworkPath(widePath);
    if (isNetwork) {
        DWORD fileAttr = (targetAttrs != 0) ? targetAttrs : FILE_ATTRIBUTE_NORMAL;
        auto hr = SHGetFileInfoW(widePath.c_str(), fileAttr,
            std::addressof(sfi), sizeof(sfi), flag | SHGFI_USEFILEATTRIBUTES);
        if (hr == 0) {
            return std::vector<unsigned char>{};
        }
    } else {
        auto hr = SHGetFileInfoW(widePath.c_str(), 0, std::addressof(sfi), sizeof(sfi), flag);
        if (hr == 0) {
            // 回退：文件不存在或路径无效时，根据扩展名获取关联图标
            memset(&sfi, 0, sizeof(sfi));
            hr = SHGetFileInfoW(widePath.c_str(), FILE_ATTRIBUTE_NORMAL,
                std::addressof(sfi), sizeof(sfi), flag | SHGFI_USEFILEATTRIBUTES);
            if (hr == 0) {
                return std::vector<unsigned char>{};
            }
        }
    }

    if (size == 16 || size == 32) {
        hIcon = sfi.hIcon;
    } else {
        HIMAGELIST* imageList;
        HRESULT hrImg = SHGetImageList(
            size == 64 ? SHIL_EXTRALARGE : SHIL_JUMBO,
            IID_IImageList,
            static_cast<void**>(static_cast<void*>(std::addressof(imageList))));

        if (FAILED(hrImg)) {
            DestroyIcon(sfi.hIcon);
            return std::vector<unsigned char>{};
        }

        hrImg = static_cast<IImageList*>(static_cast<void*>(imageList))
            ->GetIcon(sfi.iIcon, ILD_TRANSPARENT, std::addressof(hIcon));

        DestroyIcon(sfi.hIcon);

        if (FAILED(hrImg)) {
            return std::vector<unsigned char>{};
        }
    }

    auto pngData = HIconToPNG(hIcon);
    DestroyIcon(hIcon);
    return pngData;
}

class IconWorker : public Napi::AsyncWorker {
    public:
        IconWorker(const std::wstring& path, Napi::Env env, Napi::Promise::Deferred deferred)
            : Napi::AsyncWorker(env), path_(path), deferred_(deferred) {}
        void Execute() override {
            result_ = ExtractIconFromPath(path_,32);
        }
        void OnOK() override {
            if (result_.empty()) {
                auto emptyBuffer = Napi::Buffer<unsigned char>::New(Env(), 0);
                deferred_.Resolve(emptyBuffer);
                return;
            }
            auto buffer = Napi::Buffer<unsigned char>::Copy(
                Env(), result_.data(), result_.size());
            deferred_.Resolve(buffer);
        }
        void OnError(const Napi::Error& e) override {
            deferred_.Reject(e.Value());
        }
    private:
        std::wstring path_;
        std::vector<unsigned char> result_;
        Napi::Promise::Deferred deferred_;
};

// N-API: getFileIcon(path: string, size?: number) => Buffer<PNG>
Napi::Value GetFileIcon(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();

    if (info.Length() < 1 || !info[0].IsString()) {
        Napi::TypeError::New(env, "Expected file path (string) as first argument").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    std::string path = info[0].As<Napi::String>().Utf8Value();
    int size = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, NULL, 0);
    std::wstring wpath(size - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, &wpath[0], size);

    auto deferred = Napi::Promise::Deferred::New(env);
    auto* worker = new IconWorker(wpath, env,deferred);
    worker->Queue();

    return deferred.Promise();
}

// ==================== MUI 资源字符串解析 ====================

// 从 DLL/MUI 文件加载字符串资源
static std::wstring LoadStringFromModule(const std::wstring& modulePath, UINT resourceId) {
    HMODULE hMod = LoadLibraryExW(modulePath.c_str(), nullptr, LOAD_LIBRARY_AS_DATAFILE);
    if (!hMod) return std::wstring();

    WCHAR buf[1024] = {0};
    int len = LoadStringW(hMod, resourceId, buf, 1024);
    FreeLibrary(hMod);

    if (len > 0) return std::wstring(buf, len);
    return std::wstring();
}

// 解析单个 MUI 引用字符串，如 @%SystemRoot%\system32\shell32.dll,-22067
static std::wstring ResolveSingleMui(const std::wstring& muiRef) {
    if (muiRef.empty() || muiRef[0] != L'@') return std::wstring();

    std::wstring rest = muiRef.substr(1);

    // 找最后一个逗号分隔 dll 路径和资源 ID
    auto commaPos = rest.rfind(L',');
    if (commaPos == std::wstring::npos) return std::wstring();

    std::wstring dllRaw = rest.substr(0, commaPos);
    std::wstring idStr = rest.substr(commaPos + 1);

    // 解析资源 ID（可能是负数）
    if (!idStr.empty() && idStr[0] == L'-') {
        idStr = idStr.substr(1);
    }
    UINT resourceId = 0;
    for (auto c : idStr) {
        if (c < L'0' || c > L'9') return std::wstring();
        resourceId = resourceId * 10 + (c - L'0');
    }

    // 展开环境变量
    WCHAR expandedPath[MAX_PATH] = {0};
    ExpandEnvironmentStringsW(dllRaw.c_str(), expandedPath, MAX_PATH);

    // 拆分目录和文件名
    std::wstring fullPath(expandedPath);
    std::wstring dir, fileName;
    auto lastSlash = fullPath.rfind(L'\\');
    if (lastSlash != std::wstring::npos) {
        dir = fullPath.substr(0, lastSlash);
        fileName = fullPath.substr(lastSlash + 1);
    } else {
        fileName = fullPath;
    }

    // 获取用户首选 UI 语言列表（含回退链，如 zh-CN -> zh -> en-US）
    ULONG numLangs = 0;
    ULONG bufSize = 0;
    GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &numLangs, nullptr, &bufSize);
    if (bufSize > 0) {
        std::vector<WCHAR> langBuf(bufSize);
        GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &numLangs, langBuf.data(), &bufSize);

        // 遍历每种语言，尝试加载对应的 .mui 文件
        const WCHAR* p = langBuf.data();
        while (*p) {
            std::wstring muiPath = dir + L"\\" + p + L"\\" + fileName + L".mui";
            std::wstring result = LoadStringFromModule(muiPath, resourceId);
            if (!result.empty()) return result;
            p += wcslen(p) + 1;
        }
    }

    // 回退：直接从 DLL 本体加载
    return LoadStringFromModule(fullPath, resourceId);
}

// N-API: resolveMuiStrings(refs: string[]) => { [ref: string]: string }
Napi::Value ResolveMuiStrings(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();

    if (info.Length() < 1 || !info[0].IsArray()) {
        Napi::TypeError::New(env, "Expected an array of MUI reference strings").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    Napi::Array refs = info[0].As<Napi::Array>();
    Napi::Object result = Napi::Object::New(env);

    for (uint32_t i = 0; i < refs.Length(); i++) {
        Napi::Value val = refs[i];
        if (!val.IsString()) continue;

        std::string refUtf8 = val.As<Napi::String>().Utf8Value();

        // UTF-8 转宽字符
        int wideSize = MultiByteToWideChar(CP_UTF8, 0, refUtf8.c_str(), -1, NULL, 0);
        if (wideSize <= 0) continue;
        std::wstring refWide(wideSize - 1, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, refUtf8.c_str(), -1, &refWide[0], wideSize);

        std::wstring resolved = ResolveSingleMui(refWide);
        if (resolved.empty()) continue;

        // 宽字符转 UTF-8
        int utf8Size = WideCharToMultiByte(CP_UTF8, 0, resolved.c_str(), -1, NULL, 0, NULL, NULL);
        if (utf8Size <= 0) continue;
        std::string resolvedUtf8(utf8Size - 1, 0);
        WideCharToMultiByte(CP_UTF8, 0, resolved.c_str(), -1, &resolvedUtf8[0], utf8Size, NULL, NULL);

        result.Set(refUtf8, Napi::String::New(env, resolvedUtf8));
    }

    return result;
}

// ============ 取色器实现 ============

// 取色器结果结构
struct ColorPickerResult {
    bool success;
    std::string hex;
};

// 取色器回调（在主线程调用 JS）
void CallColorPickerJs(napi_env env, napi_value js_callback, void* context, void* data) {
    if (env != nullptr && js_callback != nullptr && data != nullptr) {
        ColorPickerResult* result = static_cast<ColorPickerResult*>(data);

        Napi::Env napiEnv(env);
        Napi::Object obj = Napi::Object::New(napiEnv);
        obj.Set("success", Napi::Boolean::New(napiEnv, result->success));

        if (result->success) {
            obj.Set("hex", Napi::String::New(napiEnv, result->hex));
        } else {
            obj.Set("hex", napiEnv.Null());
        }

        napi_value argv[1] = { obj };
        napi_value global;
        napi_get_global(env, &global);
        napi_call_function(env, global, js_callback, 1, argv, nullptr);

        delete result;
    }
}

// 获取屏幕上指定位置的像素颜色
COLORREF GetPixelColorAt(HDC memDC, int x, int y) {
    return GetPixel(memDC, x, y);
}

// 捕获鼠标周围 9x9 像素的颜色
void CapturePixelsAroundCursor(HDC memDC, int mouseX, int mouseY, COLORREF colors[9][9], COLORREF& centerColor) {
    const int gridSize = 9;
    const int halfGrid = gridSize / 2;

    for (int row = 0; row < gridSize; row++) {
        for (int col = 0; col < gridSize; col++) {
            int px = mouseX - halfGrid + col;
            int py = mouseY - halfGrid + row;
            colors[row][col] = GetPixelColorAt(memDC, px, py);

            if (row == halfGrid && col == halfGrid) {
                centerColor = colors[row][col];
            }
        }
    }
}

// 全局变量存储当前颜色（用于钩子访问）
static COLORREF g_currentPixelColors[9][9];
static COLORREF g_currentCenterColor = RGB(0, 0, 0);
static char g_currentHexColor[8] = "#000000";

// 取色器鼠标钩子
LRESULT CALLBACK ColorPickerMouseProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode >= 0 && g_isColorPickerActive && !g_colorPickerCallbackCalled) {
        if (wParam == WM_LBUTTONDOWN) {
            // 左键点击 - 确认取色
            if (g_colorPickerTsfn != nullptr) {
                // 使用 CAS 确保只调用一次
                bool expected = false;
                if (g_colorPickerCallbackCalled.compare_exchange_strong(expected, true)) {
                    ColorPickerResult* result = new ColorPickerResult();
                    result->success = true;
                    result->hex = g_currentHexColor;
                    napi_call_threadsafe_function(g_colorPickerTsfn, result, napi_tsfn_nonblocking);

                    g_isColorPickerActive = false;
                    if (g_colorPickerWindow != NULL) {
                        PostMessage(g_colorPickerWindow, WM_CLOSE, 0, 0);
                    }
                }
            }
            return 1; // 拦截事件
        }
    }
    return CallNextHookEx(g_colorPickerMouseHook, nCode, wParam, lParam);
}

// 取色器键盘钩子
LRESULT CALLBACK ColorPickerKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode >= 0 && g_isColorPickerActive && !g_colorPickerCallbackCalled) {
        if (wParam == WM_KEYDOWN) {
            KBDLLHOOKSTRUCT* pKbd = (KBDLLHOOKSTRUCT*)lParam;
            if (pKbd->vkCode == VK_ESCAPE) {
                // ESC 键 - 取消
                if (g_colorPickerTsfn != nullptr) {
                    // 使用 CAS 确保只调用一次
                    bool expected = false;
                    if (g_colorPickerCallbackCalled.compare_exchange_strong(expected, true)) {
                        ColorPickerResult* result = new ColorPickerResult();
                        result->success = false;
                        result->hex = "";
                        napi_call_threadsafe_function(g_colorPickerTsfn, result, napi_tsfn_nonblocking);

                        g_isColorPickerActive = false;
                        if (g_colorPickerWindow != NULL) {
                            PostMessage(g_colorPickerWindow, WM_CLOSE, 0, 0);
                        }
                    }
                }
                return 1; // 拦截事件
            }
        }
    }
    return CallNextHookEx(g_colorPickerKeyboardHook, nCode, wParam, lParam);
}

// 取色器窗口过程
LRESULT CALLBACK ColorPickerWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE: {
            // 设置定时器，30 FPS 更新
            SetTimer(hwnd, 1, 33, NULL);
            return 0;
        }

        case WM_TIMER: {
            if (wParam == 1 && g_isColorPickerActive) {
                // 获取鼠标位置
                POINT pt;
                GetCursorPos(&pt);

                // 捕获像素
                CapturePixelsAroundCursor(g_colorPickerMemDC, pt.x, pt.y, g_currentPixelColors, g_currentCenterColor);

                // 转换为 HEX
                sprintf_s(g_currentHexColor, "#%02X%02X%02X",
                    GetRValue(g_currentCenterColor),
                    GetGValue(g_currentCenterColor),
                    GetBValue(g_currentCenterColor));

                // 更新窗口位置（跟随鼠标）
                const int offsetX = 20;
                const int offsetY = 20;
                const int windowWidth = 144;  // 9 * 16
                const int windowHeight = 172; // 144 + 28

                int newX = pt.x + offsetX;
                int newY = pt.y + offsetY;

                // 屏幕边界检测
                int screenWidth = GetSystemMetrics(SM_CXSCREEN);
                int screenHeight = GetSystemMetrics(SM_CYSCREEN);

                if (newX + windowWidth > screenWidth) {
                    newX = pt.x - offsetX - windowWidth;
                }
                if (newY + windowHeight > screenHeight) {
                    newY = pt.y - offsetY - windowHeight;
                }
                if (newX < 0) newX = 0;
                if (newY < 0) newY = 0;

                SetWindowPos(hwnd, HWND_TOPMOST, newX, newY, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);

                // 重绘窗口
                InvalidateRect(hwnd, NULL, FALSE);
            }
            return 0;
        }

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);

            // 使用双缓冲绘制
            HDC memDC = CreateCompatibleDC(hdc);
            RECT clientRect;
            GetClientRect(hwnd, &clientRect);
            HBITMAP memBitmap = CreateCompatibleBitmap(hdc, clientRect.right, clientRect.bottom);
            HBITMAP oldBitmap = (HBITMAP)SelectObject(memDC, memBitmap);

            const int gridSize = 9;
            const int cellSize = 16;
            const int totalGridWidth = gridSize * cellSize;
            const int labelHeight = 28;

            // 绘制背景
            HBRUSH bgBrush = CreateSolidBrush(RGB(217, 217, 217));
            FillRect(memDC, &clientRect, bgBrush);
            DeleteObject(bgBrush);

            // 绘制 9x9 像素网格
            for (int row = 0; row < gridSize; row++) {
                for (int col = 0; col < gridSize; col++) {
                    RECT cellRect = {
                        col * cellSize,
                        row * cellSize,
                        (col + 1) * cellSize,
                        (row + 1) * cellSize
                    };

                    // 填充颜色
                    HBRUSH brush = CreateSolidBrush(g_currentPixelColors[row][col]);
                    FillRect(memDC, &cellRect, brush);
                    DeleteObject(brush);

                    // 绘制网格线
                    HPEN pen = CreatePen(PS_SOLID, 1, RGB(191, 191, 191));
                    HPEN oldPen = (HPEN)SelectObject(memDC, pen);
                    MoveToEx(memDC, cellRect.left, cellRect.top, NULL);
                    LineTo(memDC, cellRect.right, cellRect.top);
                    LineTo(memDC, cellRect.right, cellRect.bottom);
                    LineTo(memDC, cellRect.left, cellRect.bottom);
                    LineTo(memDC, cellRect.left, cellRect.top);
                    SelectObject(memDC, oldPen);
                    DeleteObject(pen);
                }
            }

            // 绘制中心十字准星
            RECT centerRect = {
                4 * cellSize,
                4 * cellSize,
                5 * cellSize,
                5 * cellSize
            };

            // 外层黑框
            HPEN blackPen = CreatePen(PS_SOLID, 2, RGB(0, 0, 0));
            HPEN oldPen = (HPEN)SelectObject(memDC, blackPen);
            SelectObject(memDC, GetStockObject(NULL_BRUSH));
            Rectangle(memDC, centerRect.left - 1, centerRect.top - 1, centerRect.right + 1, centerRect.bottom + 1);
            SelectObject(memDC, oldPen);
            DeleteObject(blackPen);

            // 内层白框
            HPEN whitePen = CreatePen(PS_SOLID, 1, RGB(255, 255, 255));
            oldPen = (HPEN)SelectObject(memDC, whitePen);
            Rectangle(memDC, centerRect.left, centerRect.top, centerRect.right, centerRect.bottom);
            SelectObject(memDC, oldPen);
            DeleteObject(whitePen);

            // 绘制 HEX 标签区域
            RECT labelRect = { 0, totalGridWidth, totalGridWidth, totalGridWidth + labelHeight };
            HBRUSH labelBrush = CreateSolidBrush(RGB(38, 38, 38));
            FillRect(memDC, &labelRect, labelBrush);
            DeleteObject(labelBrush);

            // 绘制 HEX 文本
            SetBkMode(memDC, TRANSPARENT);
            SetTextColor(memDC, RGB(255, 255, 255));
            HFONT font = CreateFontW(13, 0, 0, 0, FW_MEDIUM, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
            HFONT oldFont = (HFONT)SelectObject(memDC, font);

            RECT textRect = { 0, totalGridWidth + 5, totalGridWidth, totalGridWidth + labelHeight };
            DrawTextA(memDC, g_currentHexColor, -1, &textRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

            SelectObject(memDC, oldFont);
            DeleteObject(font);

            // 复制到屏幕
            BitBlt(hdc, 0, 0, clientRect.right, clientRect.bottom, memDC, 0, 0, SRCCOPY);

            SelectObject(memDC, oldBitmap);
            DeleteObject(memBitmap);
            DeleteDC(memDC);

            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_DESTROY: {
            KillTimer(hwnd, 1);
            PostQuitMessage(0);
            return 0;
        }
    }

    return DefWindowProc(hwnd, msg, wParam, lParam);
}

// 取色器线程函数
void ColorPickerThreadFunc() {
    // 捕获整个屏幕到内存 DC
    HDC screenDC = GetDC(NULL);
    int screenWidth = GetSystemMetrics(SM_CXSCREEN);
    int screenHeight = GetSystemMetrics(SM_CYSCREEN);

    g_colorPickerMemDC = CreateCompatibleDC(screenDC);
    g_colorPickerBitmap = CreateCompatibleBitmap(screenDC, screenWidth, screenHeight);
    SelectObject(g_colorPickerMemDC, g_colorPickerBitmap);
    BitBlt(g_colorPickerMemDC, 0, 0, screenWidth, screenHeight, screenDC, 0, 0, SRCCOPY);
    ReleaseDC(NULL, screenDC);

    // 注册窗口类
    WNDCLASSEXW wc = {0};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.lpfnWndProc = ColorPickerWndProc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.hCursor = LoadCursor(NULL, IDC_CROSS);
    wc.lpszClassName = L"ZToolsColorPicker";

    if (!RegisterClassExW(&wc)) {
        DeleteDC(g_colorPickerMemDC);
        DeleteObject(g_colorPickerBitmap);
        g_colorPickerMemDC = NULL;
        g_colorPickerBitmap = NULL;
        g_isColorPickerActive = false;
        return;
    }

    // 创建窗口
    const int windowWidth = 144;  // 9 * 16
    const int windowHeight = 172; // 144 + 28

    POINT pt;
    GetCursorPos(&pt);

    g_colorPickerWindow = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED,
        L"ZToolsColorPicker",
        L"Color Picker",
        WS_POPUP,
        pt.x + 20, pt.y + 20, windowWidth, windowHeight,
        NULL, NULL, GetModuleHandle(NULL), NULL
    );

    if (g_colorPickerWindow == NULL) {
        UnregisterClassW(L"ZToolsColorPicker", GetModuleHandle(NULL));
        DeleteDC(g_colorPickerMemDC);
        DeleteObject(g_colorPickerBitmap);
        g_colorPickerMemDC = NULL;
        g_colorPickerBitmap = NULL;
        g_isColorPickerActive = false;
        return;
    }

    // 设置窗口透明度和圆角
    SetLayeredWindowAttributes(g_colorPickerWindow, 0, 255, LWA_ALPHA);

    // 显示窗口
    ShowWindow(g_colorPickerWindow, SW_SHOW);
    SetForegroundWindow(g_colorPickerWindow);

    // 安装全局钩子
    g_colorPickerMouseHook = SetWindowsHookExW(WH_MOUSE_LL, ColorPickerMouseProc, GetModuleHandle(NULL), 0);
    g_colorPickerKeyboardHook = SetWindowsHookExW(WH_KEYBOARD_LL, ColorPickerKeyboardProc, GetModuleHandle(NULL), 0);

    if (g_colorPickerMouseHook == NULL || g_colorPickerKeyboardHook == NULL) {
        // 钩子安装失败，清理并退出
        if (g_colorPickerMouseHook) {
            UnhookWindowsHookEx(g_colorPickerMouseHook);
            g_colorPickerMouseHook = NULL;
        }
        if (g_colorPickerKeyboardHook) {
            UnhookWindowsHookEx(g_colorPickerKeyboardHook);
            g_colorPickerKeyboardHook = NULL;
        }
        DestroyWindow(g_colorPickerWindow);
        UnregisterClassW(L"ZToolsColorPicker", GetModuleHandle(NULL));
        DeleteDC(g_colorPickerMemDC);
        DeleteObject(g_colorPickerBitmap);
        g_colorPickerMemDC = NULL;
        g_colorPickerBitmap = NULL;
        g_colorPickerWindow = NULL;
        g_isColorPickerActive = false;
        return;
    }

    // 消息循环
    MSG msg;
    while (g_isColorPickerActive && GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    // 卸载钩子
    if (g_colorPickerMouseHook) {
        UnhookWindowsHookEx(g_colorPickerMouseHook);
        g_colorPickerMouseHook = NULL;
    }
    if (g_colorPickerKeyboardHook) {
        UnhookWindowsHookEx(g_colorPickerKeyboardHook);
        g_colorPickerKeyboardHook = NULL;
    }

    // 清理
    if (g_colorPickerMemDC) {
        DeleteDC(g_colorPickerMemDC);
        g_colorPickerMemDC = NULL;
    }
    if (g_colorPickerBitmap) {
        DeleteObject(g_colorPickerBitmap);
        g_colorPickerBitmap = NULL;
    }
    UnregisterClassW(L"ZToolsColorPicker", GetModuleHandle(NULL));
    g_colorPickerWindow = NULL;
    g_isColorPickerActive = false;

    // 释放 TSFN（在线程结束时释放）
    if (g_colorPickerTsfn != nullptr) {
        napi_release_threadsafe_function(g_colorPickerTsfn, napi_tsfn_release);
        g_colorPickerTsfn = nullptr;
    }
}

// 启动取色器
Napi::Value StartColorPicker(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();

    if (info.Length() < 1 || !info[0].IsFunction()) {
        Napi::TypeError::New(env, "Expected a callback function").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    if (g_isColorPickerActive) {
        Napi::Error::New(env, "Color picker already active").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    // 确保旧线程已经结束
    if (g_colorPickerThread.joinable()) {
        g_colorPickerThread.join();
    }

    // 重置状态
    g_colorPickerCallbackCalled = false;

    // 创建线程安全函数
    napi_value callback = info[0];
    napi_value resource_name;
    napi_create_string_utf8(env, "ColorPickerCallback", NAPI_AUTO_LENGTH, &resource_name);

    napi_create_threadsafe_function(
        env,
        callback,
        nullptr,
        resource_name,
        0,
        1,
        nullptr,
        nullptr,
        nullptr,
        CallColorPickerJs,
        &g_colorPickerTsfn
    );

    g_isColorPickerActive = true;

    // 启动取色器线程
    g_colorPickerThread = std::thread(ColorPickerThreadFunc);

    return env.Undefined();
}

// 停止取色器
Napi::Value StopColorPicker(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();

    if (!g_isColorPickerActive) {
        return env.Undefined();
    }

    g_isColorPickerActive = false;

    if (g_colorPickerWindow != NULL) {
        PostMessage(g_colorPickerWindow, WM_CLOSE, 0, 0);
    }

    if (g_colorPickerThread.joinable()) {
        g_colorPickerThread.join();
    }

    // 注意：不在这里释放 TSFN，因为可能已经在钩子回调中被使用
    // TSFN 会在线程清理时被释放

    return env.Undefined();
}

// Unicode 字符输入
Napi::Value UnicodeType(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();

    if (info.Length() < 1 || !info[0].IsString()) {
        Napi::TypeError::New(env, "Expected text as first argument (string)").ThrowAsJavaScriptException();
        return Napi::Boolean::New(env, false);
    }

    std::string text = info[0].As<Napi::String>().Utf8Value();

    // UTF-8 转 UTF-16
    int wideSize = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, NULL, 0);
    if (wideSize <= 0) {
        return Napi::Boolean::New(env, false);
    }
    std::wstring wtext(wideSize - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, &wtext[0], wideSize);

    std::vector<INPUT> inputs;
    for (wchar_t ch : wtext) {
        INPUT down = {};
        down.type = INPUT_KEYBOARD;
        down.ki.wScan = ch;
        down.ki.dwFlags = KEYEVENTF_UNICODE;
        inputs.push_back(down);

        INPUT up = {};
        up.type = INPUT_KEYBOARD;
        up.ki.wScan = ch;
        up.ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
        inputs.push_back(up);
    }

    UINT result = SendInput(static_cast<UINT>(inputs.size()), inputs.data(), sizeof(INPUT));
    return Napi::Boolean::New(env, result == inputs.size());
}

// ==================== Explorer 路径查询 ====================

/**
 * 通过 COM IShellWindows 查询指定窗口句柄对应的 Explorer 文件夹路径
 * 
 * 工作原理：
 * 1. 枚举所有 Shell 窗口（IShellWindows）
 * 2. 通过 HWND 匹配目标 Explorer 窗口
 * 3. 获取该窗口的 LocationURL（file:/// 格式的路径）
 * 
 * @param hwnd - 目标窗口句柄（从 WindowInfo.hwnd 获取）
 * @returns file:/// 格式的路径字符串，失败返回 null
 */
Napi::Value GetExplorerFolderPath(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();

    // 参数校验：需要一个 number 类型的 hwnd
    if (info.Length() < 1 || !info[0].IsNumber()) {
        Napi::TypeError::New(env, "hwnd (number) is required").ThrowAsJavaScriptException();
        return env.Null();
    }

    // 获取目标窗口句柄
    uint64_t hwndValue = (uint64_t)info[0].As<Napi::Number>().Int64Value();
    HWND targetHwnd = (HWND)hwndValue;

    // 初始化 COM（STA 模式，与 Electron 主线程兼容）
    HRESULT hrInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    // S_OK 和 S_FALSE 都表示本次 CoInitializeEx 成功，需要配对 CoUninitialize
    bool needUninit = (hrInit == S_OK || hrInit == S_FALSE);
    if (FAILED(hrInit)) {
        return env.Null();
    }

    std::string result;

    // 创建 ShellWindows COM 对象，枚举所有打开的 Explorer 窗口
    IShellWindows* shellWindows = nullptr;
    HRESULT hr = CoCreateInstance(
        CLSID_ShellWindows, nullptr, CLSCTX_ALL,
        IID_IShellWindows, (void**)&shellWindows
    );

    if (SUCCEEDED(hr) && shellWindows) {
        long count = 0;
        shellWindows->get_Count(&count);

        // 遍历所有 Shell 窗口，查找匹配的 HWND
        for (long i = 0; i < count; i++) {
            VARIANT idx;
            idx.vt = VT_I4;
            idx.lVal = i;

            IDispatch* disp = nullptr;
            hr = shellWindows->Item(idx, &disp);
            if (FAILED(hr) || !disp) continue;

            // 查询 IWebBrowserApp 接口（Explorer 窗口实现该接口）
            IWebBrowserApp* browser = nullptr;
            hr = disp->QueryInterface(IID_IWebBrowserApp, (void**)&browser);
            disp->Release();

            if (FAILED(hr) || !browser) continue;

            // 比对窗口句柄（HWND）
            SHANDLE_PTR browserHwndPtr = 0;
            browser->get_HWND(&browserHwndPtr);
            HWND browserHwnd = (HWND)browserHwndPtr;

            if (browserHwnd == targetHwnd) {
                // 找到匹配窗口，获取当前目录的 URL
                BSTR url = nullptr;
                hr = browser->get_LocationURL(&url);
                if (SUCCEEDED(hr) && url) {
                    // 将 BSTR (UTF-16) 转换为 UTF-8 字符串
                    int len = SysStringLen(url);
                    if (len > 0) {
                        int size = WideCharToMultiByte(CP_UTF8, 0, url, len, NULL, 0, NULL, NULL);
                        if (size > 0) {
                            result.resize(size);
                            WideCharToMultiByte(CP_UTF8, 0, url, len, &result[0], size, NULL, NULL);
                        }
                    }
                    SysFreeString(url);
                }
                browser->Release();
                break;
            }

            browser->Release();
        }

        shellWindows->Release();
    }

    // 仅在本次调用初始化 COM 时才反初始化
    if (needUninit) {
        CoUninitialize();
    }

    // 返回路径或 null
    if (result.empty()) {
        return env.Null();
    }
    return Napi::String::New(env, result);
}

std::wstring Utf8ToWideString(const std::string& input);
std::string WideToUtf8String(const std::wstring& input);

static std::string FileUrlToPath(const std::wstring& fileUrl) {
    DWORD pathLength = 32768;
    std::wstring path(pathLength, L'\0');
    HRESULT hr = PathCreateFromUrlW(fileUrl.c_str(), &path[0], &pathLength, 0);
    if (FAILED(hr) || pathLength == 0) {
        return std::string();
    }
    path.resize(pathLength);
    return WideToUtf8String(path);
}

/**
 * 获取所有打开的文件资源管理器窗口的结构化信息。
 *
 * 工作原理：
 * 1. 初始化当前线程的 COM STA 环境，枚举所有 Shell 窗口（IShellWindows）
 * 2. 读取每个窗口的 HWND 与 LocationURL，并仅保留 file:// URL
 * 3. 补充顶级窗口标题和类名，调用方可用 hwnd 精确定位目标窗口
 *
 * @returns Array<object> - Explorer 窗口信息数组；COM 初始化失败或无窗口时返回空数组
 */
Napi::Value GetAllExplorerWindows(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();

    HRESULT hrInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool needUninit = (hrInit == S_OK || hrInit == S_FALSE);

    std::vector<Napi::Object> results;

    if (FAILED(hrInit)) {
        return Napi::Array::New(env, 0);
    }

    IShellWindows* shellWindows = nullptr;
    HRESULT hr = CoCreateInstance(
        CLSID_ShellWindows, nullptr, CLSCTX_ALL,
        IID_IShellWindows, (void**)&shellWindows
    );

    if (SUCCEEDED(hr) && shellWindows) {
        long count = 0;
        shellWindows->get_Count(&count);

        for (long i = 0; i < count; i++) {
            VARIANT idx;
            VariantInit(&idx);
            idx.vt = VT_I4;
            idx.lVal = i;

            IDispatch* disp = nullptr;
            hr = shellWindows->Item(idx, &disp);
            VariantClear(&idx);
            if (FAILED(hr) || !disp) continue;

            IWebBrowserApp* browser = nullptr;
            hr = disp->QueryInterface(IID_IWebBrowserApp, (void**)&browser);
            disp->Release();

            if (FAILED(hr) || !browser) continue;

            SHANDLE_PTR browserHwndPtr = 0;
            browser->get_HWND(&browserHwndPtr);
            HWND browserHwnd = reinterpret_cast<HWND>(browserHwndPtr);

            BSTR url = nullptr;
            hr = browser->get_LocationURL(&url);
            if (SUCCEEDED(hr) && url) {
                std::wstring urlWide(url, SysStringLen(url));
                std::string urlStr = WideToUtf8String(urlWide);
                if (urlStr.rfind("file://", 0) == 0 && browserHwnd && IsWindow(browserHwnd)) {
                    WCHAR title[512] = {0};
                    WCHAR className[256] = {0};
                    GetWindowTextW(browserHwnd, title, 512);
                    GetClassNameW(browserHwnd, className, 256);

                    Napi::Object item = Napi::Object::New(env);
                    item.Set("platform", Napi::String::New(env, "win32"));
                    item.Set("kind", Napi::String::New(env, "windows-explorer"));
                    item.Set("preciseTarget", Napi::Boolean::New(env, true));
                    item.Set("hwnd", Napi::Number::New(env, reinterpret_cast<uint64_t>(browserHwnd)));
                    item.Set("url", Napi::String::New(env, urlStr));
                    const std::string pathStr = FileUrlToPath(urlWide);
                    if (!pathStr.empty()) {
                        item.Set("path", Napi::String::New(env, pathStr));
                    }
                    item.Set("title", Napi::String::New(env, WideToUtf8String(title)));
                    item.Set("className", Napi::String::New(env, WideToUtf8String(className)));
                    item.Set("app", Napi::String::New(env, "explorer.exe"));
                    results.push_back(item);
                }
                SysFreeString(url);
            }

            browser->Release();
        }

        shellWindows->Release();
    }

    if (needUninit) {
        CoUninitialize();
    }

    Napi::Array resultArray = Napi::Array::New(env, results.size());
    for (size_t i = 0; i < results.size(); i++) {
        resultArray[i] = results[i];
    }
    return resultArray;
}

struct ChildClassSearchContext {
    const wchar_t** classNames;
    size_t classCount;
    bool found;
};

/**
 * 枚举子窗口并查找指定窗口类名。
 *
 * 用于确认 #32770 对话框是否真的是 Shell 文件选择对话框：普通系统对话框也会使用
 * #32770 顶级类名，只有包含地址栏/文件列表相关子控件时才允许写入地址。
 */
static BOOL CALLBACK FindChildClassProc(HWND child, LPARAM lParam) {
    ChildClassSearchContext* context = reinterpret_cast<ChildClassSearchContext*>(lParam);
    if (!context || context->found) {
        return FALSE;
    }

    WCHAR className[256] = {0};
    int classLen = GetClassNameW(child, className, 256);
    if (classLen > 0) {
        for (size_t i = 0; i < context->classCount; i++) {
            if (wcscmp(className, context->classNames[i]) == 0) {
                context->found = true;
                return FALSE;
            }
        }
    }

    EnumChildWindows(child, FindChildClassProc, lParam);
    return !context->found;
}

/**
 * 判断窗口是否包含 Shell 文件对话框的典型子控件。
 *
 * 现代和旧版 Windows 文件选择对话框的内部类名略有差异，因此同时匹配面包屑地址栏、
 * Shell 文件视图和 DirectUI 文件视图，尽量覆盖打开/保存/选择文件夹等场景。
 */
static bool HasFileDialogChildControls(HWND hwnd) {
    const wchar_t* fileDialogClasses[] = {
        L"Breadcrumb Parent",
        L"Address Band Root",
        L"SHELLDLL_DefView",
        L"DUIViewWndClassName"
    };
    ChildClassSearchContext context = { fileDialogClasses, 4, false };
    EnumChildWindows(hwnd, FindChildClassProc, reinterpret_cast<LPARAM>(&context));
    return context.found;
}

/**
 * 判断目标窗口是否是允许修改地址栏的文件定位窗口。
 *
 * 仅放行 Explorer 顶级窗口和常见文件选择对话框，避免把传入地址写入浏览器、编辑器
 * 或其它普通应用的输入框。文件对话框可能属于任意宿主进程，因此额外允许 #32770
 * 对话框类名；Explorer 则通过 CabinetWClass/ExploreWClass 识别。
 */
static bool IsFileLocationWindow(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) {
        return false;
    }

    WCHAR className[256] = {0};
    int classLen = GetClassNameW(hwnd, className, 256);
    if (classLen <= 0) {
        return false;
    }

    if (wcscmp(className, L"CabinetWClass") == 0 ||
        wcscmp(className, L"ExploreWClass") == 0) {
        return true;
    }

    if (wcscmp(className, L"#32770") == 0) {
        return HasFileDialogChildControls(hwnd);
    }

    return false;
}

Napi::Value IsFileLocationWindowBinding(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    if (info.Length() < 1 || !info[0].IsNumber()) {
        Napi::TypeError::New(env, "hwnd (number) is required").ThrowAsJavaScriptException();
        return Napi::Boolean::New(env, false);
    }

    uint64_t hwndValue = static_cast<uint64_t>(info[0].As<Napi::Number>().Int64Value());
    HWND hwnd = reinterpret_cast<HWND>(hwndValue);
    return Napi::Boolean::New(env, IsFileLocationWindow(hwnd));
}

/**
 * 将目标窗口切到前台并获得键盘输入焦点。
 *
 * Windows 对跨进程 SetForegroundWindow 有限制，这里临时附加当前线程、前台线程和目标
 * 窗口线程的输入队列，确保后续 Ctrl+L、文本输入和 Enter 被发送到指定窗口。
 */
static bool FocusTargetWindow(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) {
        return false;
    }

    if (IsIconic(hwnd)) {
        ShowWindow(hwnd, SW_RESTORE);
    }

    HWND foregroundWnd = GetForegroundWindow();
    DWORD foregroundThreadId = GetWindowThreadProcessId(foregroundWnd, NULL);
    DWORD targetThreadId = GetWindowThreadProcessId(hwnd, NULL);
    DWORD currentThreadId = GetCurrentThreadId();

    BOOL attachedForeground = FALSE;
    BOOL attachedCurrent = FALSE;

    if (foregroundThreadId != targetThreadId) {
        attachedForeground = AttachThreadInput(foregroundThreadId, targetThreadId, TRUE);
    }
    if (currentThreadId != targetThreadId && currentThreadId != foregroundThreadId) {
        attachedCurrent = AttachThreadInput(currentThreadId, targetThreadId, TRUE);
    }

    BringWindowToTop(hwnd);
    SetForegroundWindow(hwnd);
    SetActiveWindow(hwnd);
    SetFocus(hwnd);

    if (attachedForeground) {
        AttachThreadInput(foregroundThreadId, targetThreadId, FALSE);
    }
    if (attachedCurrent) {
        AttachThreadInput(currentThreadId, targetThreadId, FALSE);
    }

    return GetForegroundWindow() == hwnd;
}

static bool NavigateExplorerWindow(HWND targetHwnd, const std::wstring& address) {
    HRESULT hrInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool needUninit = (hrInit == S_OK || hrInit == S_FALSE);
    if (FAILED(hrInit)) {
        return false;
    }

    bool success = false;
    IShellWindows* shellWindows = nullptr;
    HRESULT hr = CoCreateInstance(
        CLSID_ShellWindows, nullptr, CLSCTX_ALL,
        IID_IShellWindows, reinterpret_cast<void**>(&shellWindows)
    );

    if (SUCCEEDED(hr) && shellWindows) {
        long count = 0;
        shellWindows->get_Count(&count);

        for (long i = 0; i < count; i++) {
            VARIANT idx;
            VariantInit(&idx);
            idx.vt = VT_I4;
            idx.lVal = i;

            IDispatch* disp = nullptr;
            hr = shellWindows->Item(idx, &disp);
            if (FAILED(hr) || !disp) continue;

            IWebBrowser2* browser = nullptr;
            hr = disp->QueryInterface(IID_IWebBrowser2, reinterpret_cast<void**>(&browser));
            disp->Release();

            if (FAILED(hr) || !browser) continue;

            SHANDLE_PTR browserHwndPtr = 0;
            hr = browser->get_HWND(&browserHwndPtr);
            if (SUCCEEDED(hr) && reinterpret_cast<HWND>(browserHwndPtr) == targetHwnd) {
                VARIANT url;
                VariantInit(&url);
                url.vt = VT_BSTR;
                url.bstrVal = SysAllocString(address.c_str());

                VARIANT empty;
                VariantInit(&empty);
                hr = browser->Navigate2(&url, &empty, &empty, &empty, &empty);
                success = SUCCEEDED(hr);

                VariantClear(&url);
                browser->Release();
                break;
            }

            browser->Release();
        }

        shellWindows->Release();
    }

    if (needUninit) {
        CoUninitialize();
    }

    return success;
}

/**
 * 发送 Ctrl+L 快捷键，用于聚焦 Explorer 或文件对话框的地址栏。
 *
 * 该快捷键是 Windows Shell 文件定位界面的通用入口；如果窗口类型已被
 * IsFileLocationWindow 限制为文件定位窗口，使用它比遍历不同版本 Shell UIA 树更稳定。
 */
static bool SendFocusAddressBarShortcut() {
    INPUT inputs[4] = {};
    inputs[0].type = INPUT_KEYBOARD;
    inputs[0].ki.wVk = VK_CONTROL;

    inputs[1].type = INPUT_KEYBOARD;
    inputs[1].ki.wVk = 'L';

    inputs[2].type = INPUT_KEYBOARD;
    inputs[2].ki.wVk = 'L';
    inputs[2].ki.dwFlags = KEYEVENTF_KEYUP;

    inputs[3].type = INPUT_KEYBOARD;
    inputs[3].ki.wVk = VK_CONTROL;
    inputs[3].ki.dwFlags = KEYEVENTF_KEYUP;

    return SendInput(4, inputs, sizeof(INPUT)) == 4;
}

/**
 * 使用 KEYEVENTF_UNICODE 输入任意 UTF-16 文本。
 *
 * 直接发送 Unicode 字符可以覆盖中文、空格和非 ASCII 路径，避免键盘布局影响。
 */
static bool SendUnicodeText(const std::wstring& text) {
    if (text.empty()) {
        return false;
    }

    std::vector<INPUT> inputs;
    inputs.reserve(text.size() * 2);
    for (wchar_t ch : text) {
        INPUT down = {};
        down.type = INPUT_KEYBOARD;
        down.ki.wScan = ch;
        down.ki.dwFlags = KEYEVENTF_UNICODE;
        inputs.push_back(down);

        INPUT up = {};
        up.type = INPUT_KEYBOARD;
        up.ki.wScan = ch;
        up.ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
        inputs.push_back(up);
    }

    UINT sent = SendInput(static_cast<UINT>(inputs.size()), inputs.data(), sizeof(INPUT));
    return sent == inputs.size();
}

/**
 * 发送一个虚拟键的按下和释放事件。
 *
 * 主要用于在地址栏文本写入后发送 Enter，让 Explorer 或文件对话框跳转到目标地址。
 */
static bool SendVirtualKeyTap(WORD vk) {
    INPUT inputs[2] = {};
    inputs[0].type = INPUT_KEYBOARD;
    inputs[0].ki.wVk = vk;
    inputs[1].type = INPUT_KEYBOARD;
    inputs[1].ki.wVk = vk;
    inputs[1].ki.dwFlags = KEYEVENTF_KEYUP;
    return SendInput(2, inputs, sizeof(INPUT)) == 2;
}

/**
 * 将指定 Explorer 或文件选择对话框的地址栏设置为传入地址。
 *
 * 参数：
 * 1. hwnd: number - 目标文件资源管理器或文件选择对话框顶级窗口句柄
 * 2. address: string - 目标文件夹路径或 file:/// URL
 *
 * 返回 true 表示快捷键、文本输入和跳转键都发送成功；目标窗口不是受支持类型、
 * 不存在或无法获得焦点时返回 false。调用方可用 getActiveWindow() 的 hwnd 字段作为目标。
 */
Napi::Value SetAddressBar(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();

    if (info.Length() < 2 || !info[0].IsNumber() || !info[1].IsString()) {
        Napi::TypeError::New(env, "hwnd (number) and address (string) are required").ThrowAsJavaScriptException();
        return Napi::Boolean::New(env, false);
    }

    uint64_t hwndValue = static_cast<uint64_t>(info[0].As<Napi::Number>().Int64Value());
    HWND targetHwnd = reinterpret_cast<HWND>(hwndValue);
    std::string addressUtf8 = info[1].As<Napi::String>().Utf8Value();
    std::wstring address = Utf8ToWideString(addressUtf8);

    if (address.empty() || !IsFileLocationWindow(targetHwnd)) {
        return Napi::Boolean::New(env, false);
    }

    WCHAR className[256] = {0};
    GetClassNameW(targetHwnd, className, 256);
    if (wcscmp(className, L"CabinetWClass") == 0 ||
        wcscmp(className, L"ExploreWClass") == 0) {
        return Napi::Boolean::New(env, NavigateExplorerWindow(targetHwnd, address));
    }

    if (!FocusTargetWindow(targetHwnd)) {
        return Napi::Boolean::New(env, false);
    }

    Sleep(60);
    bool success = SendFocusAddressBarShortcut();
    Sleep(80);
    success = SendUnicodeText(address) && success;
    Sleep(30);
    success = SendVirtualKeyTap(VK_RETURN) && success;

    return Napi::Boolean::New(env, success);
}

// ==================== 浏览器 URL 查询 ====================

std::wstring Utf8ToWideString(const std::string& input) {
    if (input.empty()) {
        return std::wstring();
    }

    int size = MultiByteToWideChar(CP_UTF8, 0, input.c_str(), -1, nullptr, 0);
    if (size <= 0) {
        return std::wstring();
    }

    std::wstring result(size - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, input.c_str(), -1, &result[0], size);
    return result;
}

std::string WideToUtf8String(const std::wstring& input) {
    if (input.empty()) {
        return std::string();
    }

    int size = WideCharToMultiByte(CP_UTF8, 0, input.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        return std::string();
    }

    std::string result(size - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, input.c_str(), -1, &result[0], size, nullptr, nullptr);
    return result;
}

std::wstring ToLowerWideString(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch) {
        return static_cast<wchar_t>(std::towlower(ch));
    });
    return value;
}

bool StartsWithWideString(const std::wstring& value, const std::wstring& prefix) {
    return value.size() >= prefix.size() &&
           std::equal(prefix.begin(), prefix.end(), value.begin());
}


struct WindowsShortcutEntry {
    std::wstring name;
    std::wstring path;
    std::wstring icon;
    std::wstring targetPath;
    std::wstring sourceType;
};

struct UrlShortcutInfo {
    bool valid = false;
    std::wstring url;
    std::wstring iconFile;
};

static std::wstring JoinWindowsPath(const std::wstring& base, const std::wstring& name) {
    if (base.empty()) return name;
    wchar_t last = base[base.size() - 1];
    if (last == L'\\' || last == L'/') {
        return base + name;
    }
    return base + L"\\" + name;
}

static std::wstring GetFileNameWithoutExtension(const std::wstring& fileName) {
    size_t slash = fileName.find_last_of(L"\\/");
    std::wstring base = slash == std::wstring::npos ? fileName : fileName.substr(slash + 1);
    size_t dot = base.find_last_of(L'.');
    if (dot == std::wstring::npos || dot == 0) {
        return base;
    }
    return base.substr(0, dot);
}

static std::wstring GetExtensionLower(const std::wstring& fileName) {
    size_t dot = fileName.find_last_of(L'.');
    if (dot == std::wstring::npos) {
        return L"";
    }
    return ToLowerWideString(fileName.substr(dot));
}

static bool StartsWithLower(const std::wstring& value, const std::wstring& prefix) {
    return value.size() >= prefix.size() && std::equal(prefix.begin(), prefix.end(), value.begin());
}

static std::wstring GetIniStringValue(const wchar_t* section, const wchar_t* key, const std::wstring& filePath) {
    DWORD size = 1024;
    std::vector<wchar_t> buffer(size);
    DWORD copied = 0;

    for (;;) {
        copied = GetPrivateProfileStringW(section, key, L"", buffer.data(), size, filePath.c_str());
        if (copied < size - 1) {
            break;
        }
        size *= 2;
        buffer.assign(size, L'\0');
        if (size > 65536) {
            break;
        }
    }

    if (copied == 0) {
        return L"";
    }
    return std::wstring(buffer.data(), copied);
}

static UrlShortcutInfo ParseUrlShortcutFile(const std::wstring& filePath) {
    UrlShortcutInfo info;
    std::wstring url = GetIniStringValue(L"InternetShortcut", L"URL", filePath);
    if (url.empty()) {
        return info;
    }

    std::wstring lowerUrl = ToLowerWideString(url);
    if (StartsWithLower(lowerUrl, L"http://") || StartsWithLower(lowerUrl, L"https://")) {
        return info;
    }

    info.valid = true;
    info.url = url;
    info.iconFile = GetIniStringValue(L"InternetShortcut", L"IconFile", filePath);
    return info;
}

static std::map<std::wstring, std::wstring> ReadLocalizedDisplayNames(const std::wstring& dirPath) {
    std::map<std::wstring, std::wstring> result;
    std::wstring iniPath = JoinWindowsPath(dirPath, L"desktop.ini");

    DWORD attrs = GetFileAttributesW(iniPath.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        return result;
    }

    DWORD size = 8192;
    std::vector<wchar_t> buffer(size);
    DWORD copied = 0;

    for (;;) {
        copied = GetPrivateProfileSectionW(L"LocalizedFileNames", buffer.data(), size, iniPath.c_str());
        if (copied < size - 2) {
            break;
        }
        size *= 2;
        buffer.assign(size, L'\0');
        if (size > 262144) {
            break;
        }
    }

    if (copied == 0) {
        return result;
    }

    const wchar_t* item = buffer.data();
    while (*item != L'\0') {
        std::wstring line(item);
        size_t eq = line.find(L'=');
        if (eq != std::wstring::npos && eq > 0) {
            std::wstring fileName = line.substr(0, eq);
            std::wstring value = line.substr(eq + 1);
            std::wstring localizedName;

            if (!value.empty() && value[0] == L'@') {
                localizedName = ResolveIndirectString(value);
            } else {
                localizedName = value;
            }

            if (!localizedName.empty()) {
                std::wstring fullPath = ToLowerWideString(JoinWindowsPath(dirPath, fileName));
                result[fullPath] = localizedName;
            }
        }
        item += line.size() + 1;
    }

    return result;
}

static std::wstring ResolveShortcutTargetPath(const std::wstring& shortcutPath) {
    std::wstring targetPath;
    IShellLinkW* shellLink = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW,
                                  reinterpret_cast<void**>(&shellLink));
    if (FAILED(hr) || !shellLink) {
        return targetPath;
    }

    IPersistFile* persistFile = nullptr;
    hr = shellLink->QueryInterface(IID_IPersistFile, reinterpret_cast<void**>(&persistFile));
    if (SUCCEEDED(hr) && persistFile) {
        hr = persistFile->Load(shortcutPath.c_str(), STGM_READ);
        if (SUCCEEDED(hr)) {
            std::vector<wchar_t> pathBuffer(MAX_PATH * 4, L'\0');
            WIN32_FIND_DATAW findData = {};
            hr = shellLink->GetPath(pathBuffer.data(), static_cast<int>(pathBuffer.size()), &findData, SLGP_RAWPATH);
            if (SUCCEEDED(hr) && pathBuffer[0] != L'\0') {
                targetPath = pathBuffer.data();
            }
        }
        persistFile->Release();
    }

    shellLink->Release();
    return targetPath;
}

static bool ShouldSkipDirectoryName(const std::wstring& dirName, const std::vector<std::wstring>& skipFolders) {
    std::wstring lower = ToLowerWideString(dirName);
    for (const auto& skip : skipFolders) {
        if (lower == skip) {
            return true;
        }
    }
    return false;
}

static void ResolveShortcutTargetsInParallel(std::vector<WindowsShortcutEntry>& entries) {
    if (entries.empty()) {
        return;
    }

    unsigned int workerCount = std::thread::hardware_concurrency();
    if (workerCount == 0) {
        workerCount = 4;
    }
    workerCount = std::min<unsigned int>(workerCount, 8);
    workerCount = std::max<unsigned int>(workerCount, 1);

    std::atomic<size_t> nextIndex(0);
    std::vector<std::thread> workers;
    workers.reserve(workerCount);

    for (unsigned int worker = 0; worker < workerCount; worker++) {
        workers.emplace_back([&entries, &nextIndex]() {
            HRESULT hrInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            bool needUninit = (hrInit == S_OK || hrInit == S_FALSE);

            for (;;) {
                size_t index = nextIndex.fetch_add(1);
                if (index >= entries.size()) {
                    break;
                }

                WindowsShortcutEntry& entry = entries[index];
                if (entry.sourceType != L"lnk") {
                    continue;
                }

                std::wstring targetPath = ResolveShortcutTargetPath(entry.path);
                if (!targetPath.empty() && GetExtensionLower(targetPath) == L".url") {
                    UrlShortcutInfo urlInfo = ParseUrlShortcutFile(targetPath);
                    if (!urlInfo.valid) {
                        entry.sourceType = L"skip";
                        continue;
                    }

                    entry.path = urlInfo.url;
                    entry.icon = urlInfo.iconFile.empty() ? entry.icon : urlInfo.iconFile;
                    entry.targetPath.clear();
                    entry.sourceType = L"lnk-url";
                    continue;
                }

                entry.targetPath = targetPath;
            }

            if (needUninit) {
                CoUninitialize();
            }
        });
    }

    for (auto& worker : workers) {
        if (worker.joinable()) {
            worker.join();
        }
    }

    entries.erase(
        std::remove_if(entries.begin(), entries.end(), [](const WindowsShortcutEntry& entry) {
            return entry.sourceType == L"skip";
        }),
        entries.end()
    );
}

static void AddShortcutEntry(const std::wstring& dirPath,
                             const WIN32_FIND_DATAW& findData,
                             const std::map<std::wstring, std::wstring>& localizedNames,
                             std::vector<WindowsShortcutEntry>& entries) {
    const std::wstring fileName(findData.cFileName);
    const std::wstring fullPath = JoinWindowsPath(dirPath, fileName);
    const std::wstring ext = GetExtensionLower(fileName);

    if (ext != L".lnk" && ext != L".url") {
        return;
    }

    std::wstring lookupPath = ToLowerWideString(fullPath);
    auto nameIt = localizedNames.find(lookupPath);
    std::wstring appName = nameIt != localizedNames.end()
        ? nameIt->second
        : GetFileNameWithoutExtension(fileName);

    if (ext == L".url") {
        UrlShortcutInfo urlInfo = ParseUrlShortcutFile(fullPath);
        if (!urlInfo.valid) {
            return;
        }

        WindowsShortcutEntry entry;
        entry.name = appName;
        entry.path = urlInfo.url;
        entry.icon = urlInfo.iconFile.empty() ? fullPath : urlInfo.iconFile;
        entry.sourceType = L"url";
        entries.push_back(entry);
        return;
    }

    WindowsShortcutEntry entry;
    entry.name = appName;
    entry.path = fullPath;
    entry.icon = fullPath;
    entry.sourceType = L"lnk";
    entries.push_back(entry);
}

static void ScanShortcutDirectory(const std::wstring& dirPath,
                                  bool recursive,
                                  const std::vector<std::wstring>& skipFolders,
                                  std::vector<WindowsShortcutEntry>& entries) {
    DWORD attrs = GetFileAttributesW(dirPath.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES || !(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        return;
    }

    std::map<std::wstring, std::wstring> localizedNames = ReadLocalizedDisplayNames(dirPath);
    std::wstring pattern = JoinWindowsPath(dirPath, L"*");
    WIN32_FIND_DATAW findData = {};
    HANDLE findHandle = FindFirstFileW(pattern.c_str(), &findData);
    if (findHandle == INVALID_HANDLE_VALUE) {
        return;
    }

    do {
        std::wstring name(findData.cFileName);
        if (name == L"." || name == L"..") {
            continue;
        }

        if (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (recursive && !ShouldSkipDirectoryName(name, skipFolders)) {
                ScanShortcutDirectory(JoinWindowsPath(dirPath, name), true, skipFolders, entries);
            }
            continue;
        }

        AddShortcutEntry(dirPath, findData, localizedNames, entries);
    } while (FindNextFileW(findHandle, &findData));

    FindClose(findHandle);
}

static std::vector<std::wstring> NapiStringArrayToWideVector(Napi::Env env, const Napi::Value& value, const char* name) {
    if (!value.IsArray()) {
        Napi::TypeError::New(env, std::string(name) + " must be an array").ThrowAsJavaScriptException();
        return {};
    }

    Napi::Array array = value.As<Napi::Array>();
    std::vector<std::wstring> result;
    result.reserve(array.Length());
    for (uint32_t i = 0; i < array.Length(); i++) {
        Napi::Value item = array.Get(i);
        if (!item.IsString()) {
            continue;
        }
        result.push_back(Utf8ToWideString(item.As<Napi::String>().Utf8Value()));
    }
    return result;
}

Napi::Value ScanWindowsShortcuts(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    if (info.Length() < 3) {
        Napi::TypeError::New(env, "scanPaths, rootScanPaths and skipFolders are required").ThrowAsJavaScriptException();
        return Napi::Array::New(env);
    }

    std::vector<std::wstring> scanPaths = NapiStringArrayToWideVector(env, info[0], "scanPaths");
    std::vector<std::wstring> rootScanPaths = NapiStringArrayToWideVector(env, info[1], "rootScanPaths");
    std::vector<std::wstring> skipFolders = NapiStringArrayToWideVector(env, info[2], "skipFolders");
    for (auto& folder : skipFolders) {
        folder = ToLowerWideString(folder);
    }

    std::vector<WindowsShortcutEntry> entries;
    for (const auto& scanPath : scanPaths) {
        ScanShortcutDirectory(scanPath, true, skipFolders, entries);
    }
    for (const auto& rootPath : rootScanPaths) {
        ScanShortcutDirectory(rootPath, false, skipFolders, entries);
    }
    ResolveShortcutTargetsInParallel(entries);

    Napi::Array result = Napi::Array::New(env, entries.size());
    for (uint32_t i = 0; i < entries.size(); i++) {
        const auto& entry = entries[i];
        Napi::Object item = Napi::Object::New(env);
        item.Set("name", Napi::String::New(env, WideToUtf8String(entry.name)));
        item.Set("path", Napi::String::New(env, WideToUtf8String(entry.path)));
        item.Set("icon", Napi::String::New(env, WideToUtf8String(entry.icon)));
        if (!entry.targetPath.empty()) {
            item.Set("targetPath", Napi::String::New(env, WideToUtf8String(entry.targetPath)));
        }
        item.Set("sourceType", Napi::String::New(env, WideToUtf8String(entry.sourceType)));
        result.Set(i, item);
    }

    return result;
}
bool LooksLikeBrowserUrl(const std::wstring& value) {
    if (value.empty()) {
        return false;
    }

    std::wstring trimmed = value;
    trimmed.erase(trimmed.begin(), std::find_if(trimmed.begin(), trimmed.end(), [](wchar_t ch) {
        return !iswspace(ch);
    }));
    trimmed.erase(std::find_if(trimmed.rbegin(), trimmed.rend(), [](wchar_t ch) {
        return !iswspace(ch);
    }).base(), trimmed.end());

    if (trimmed.empty()) {
        return false;
    }

    const std::wstring lower = ToLowerWideString(trimmed);
    const std::wstring prefixes[] = {
        L"http://", L"https://", L"file:///", L"about:", L"chrome://",
        L"edge://", L"brave://", L"opera://", L"vivaldi://",
        L"moz-extension://", L"ftp://"
    };

    for (const auto& prefix : prefixes) {
        if (StartsWithWideString(lower, prefix)) {
            return true;
        }
    }

    return false;
}

bool IsBrowserAddressBarName(const std::wstring& name) {
    if (name.empty()) {
        return false;
    }

    const std::wstring lower = ToLowerWideString(name);
    const std::wstring keywords[] = {
        L"address and search bar",
        L"search or enter address",
        L"address bar",
        L"search with google or enter address",
        L"地址和搜索栏",
        L"地址栏",
        L"输入搜索词或网址"
    };

    for (const auto& keyword : keywords) {
        if (lower.find(keyword) != std::wstring::npos) {
            return true;
        }
    }

    return false;
}

std::wstring ReadElementValuePattern(IUIAutomationElement* element) {
    if (!element) {
        return std::wstring();
    }

    IUIAutomationValuePattern* valuePattern = nullptr;
    HRESULT hr = element->GetCurrentPatternAs(UIA_ValuePatternId, IID_IUIAutomationValuePattern,
                                             reinterpret_cast<void**>(&valuePattern));
    if (FAILED(hr) || !valuePattern) {
        return std::wstring();
    }

    BSTR value = nullptr;
    hr = valuePattern->get_CurrentValue(&value);
    valuePattern->Release();

    if (FAILED(hr) || !value) {
        return std::wstring();
    }

    std::wstring result(value, SysStringLen(value));
    SysFreeString(value);
    return result;
}

std::wstring TryExtractUrlFromAutomationElement(IUIAutomationElement* element) {
    if (!element) {
        return std::wstring();
    }

    CONTROLTYPEID controlType = 0;
    element->get_CurrentControlType(&controlType);

    BSTR elementName = nullptr;
    element->get_CurrentName(&elementName);
    std::wstring name = elementName ? std::wstring(elementName, SysStringLen(elementName)) : std::wstring();
    if (elementName) {
        SysFreeString(elementName);
    }

    const bool isLikelyAddressControl =
        controlType == UIA_EditControlTypeId ||
        controlType == UIA_ComboBoxControlTypeId ||
        controlType == UIA_DocumentControlTypeId ||
        controlType == UIA_PaneControlTypeId ||
        IsBrowserAddressBarName(name);

    if (!isLikelyAddressControl) {
        return std::wstring();
    }

    std::wstring value = ReadElementValuePattern(element);
    if (LooksLikeBrowserUrl(value)) {
        return value;
    }

    if (LooksLikeBrowserUrl(name)) {
        return name;
    }

    return std::wstring();
}

std::wstring FindBrowserUrlRecursive(IUIAutomation* automation,
                                     IUIAutomationTreeWalker* walker,
                                     IUIAutomationElement* element,
                                     int depth,
                                     int& visited) {
    if (!automation || !walker || !element || depth > 18 || visited > 600) {
        return std::wstring();
    }

    visited++;

    std::wstring currentValue = TryExtractUrlFromAutomationElement(element);
    if (!currentValue.empty()) {
        return currentValue;
    }

    IUIAutomationElement* child = nullptr;
    if (FAILED(walker->GetFirstChildElement(element, &child)) || !child) {
        return std::wstring();
    }

    std::wstring result;
    while (child) {
        result = FindBrowserUrlRecursive(automation, walker, child, depth + 1, visited);

        IUIAutomationElement* nextSibling = nullptr;
        HRESULT hr = walker->GetNextSiblingElement(child, &nextSibling);
        child->Release();
        child = nullptr;

        if (!result.empty()) {
            if (nextSibling) {
                nextSibling->Release();
            }
            return result;
        }

        if (FAILED(hr) || !nextSibling) {
            break;
        }

        child = nextSibling;
    }

    return std::wstring();
}

std::wstring ReadBrowserUrlByUIAutomation(HWND targetHwnd) {
    if (!targetHwnd) {
        return std::wstring();
    }

    IUIAutomation* automation = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_IUIAutomation, reinterpret_cast<void**>(&automation));
    if (FAILED(hr) || !automation) {
        return std::wstring();
    }

    IUIAutomationElement* rootElement = nullptr;
    hr = automation->ElementFromHandle(targetHwnd, &rootElement);
    if (FAILED(hr) || !rootElement) {
        automation->Release();
        return std::wstring();
    }

    IUIAutomationTreeWalker* controlWalker = nullptr;
    automation->get_ControlViewWalker(&controlWalker);

    int visited = 0;
    std::wstring result;
    if (controlWalker) {
        result = FindBrowserUrlRecursive(automation, controlWalker, rootElement, 0, visited);
        controlWalker->Release();
    }

    // ControlView 未找到时，再退化到 RawView 做一次宽松扫描。
    if (result.empty()) {
        IUIAutomationTreeWalker* rawWalker = nullptr;
        automation->get_RawViewWalker(&rawWalker);
        if (rawWalker) {
            visited = 0;
            result = FindBrowserUrlRecursive(automation, rawWalker, rootElement, 0, visited);
            rawWalker->Release();
        }
    }

    rootElement->Release();
    automation->Release();
    return result;
}

std::string GetShellWindowLocationUrl(HWND targetHwnd) {
    std::string result;

    IShellWindows* shellWindows = nullptr;
    HRESULT hr = CoCreateInstance(
        CLSID_ShellWindows, nullptr, CLSCTX_ALL,
        IID_IShellWindows, reinterpret_cast<void**>(&shellWindows)
    );

    if (FAILED(hr) || !shellWindows) {
        return result;
    }

    long count = 0;
    shellWindows->get_Count(&count);

    for (long i = 0; i < count; i++) {
        VARIANT idx;
        VariantInit(&idx);
        idx.vt = VT_I4;
        idx.lVal = i;

        IDispatch* disp = nullptr;
        hr = shellWindows->Item(idx, &disp);
        VariantClear(&idx);
        if (FAILED(hr) || !disp) {
            continue;
        }

        IWebBrowserApp* browser = nullptr;
        hr = disp->QueryInterface(IID_IWebBrowserApp, reinterpret_cast<void**>(&browser));
        disp->Release();

        if (FAILED(hr) || !browser) {
            continue;
        }

        SHANDLE_PTR browserHwndPtr = 0;
        browser->get_HWND(&browserHwndPtr);
        HWND browserHwnd = reinterpret_cast<HWND>(browserHwndPtr);

        if (browserHwnd == targetHwnd) {
            BSTR url = nullptr;
            hr = browser->get_LocationURL(&url);
            if (SUCCEEDED(hr) && url) {
                result = WideToUtf8String(std::wstring(url, SysStringLen(url)));
                SysFreeString(url);
            }
            browser->Release();
            break;
        }

        browser->Release();
    }

    shellWindows->Release();
    return result;
}

/**
 * 读取指定浏览器窗口当前 URL。
 *
 * 参数：
 * 1. browserName: string - 浏览器标识（如 chrome/msedge/firefox）
 * 2. hwnd: number - 目标窗口句柄
 * 3. callback: function - 接收读取结果，失败时传 null
 */
Napi::Value ReadBrowserWindowUrl(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();

    if (info.Length() < 3 || !info[0].IsString() || !info[1].IsNumber() || !info[2].IsFunction()) {
        Napi::TypeError::New(env, "browserName (string), hwnd (number) and callback (function) are required")
            .ThrowAsJavaScriptException();
        return env.Undefined();
    }

    const std::string browserName = info[0].As<Napi::String>().Utf8Value();
    const std::string browserNameLower = WideToUtf8String(ToLowerWideString(Utf8ToWideString(browserName)));
    const uint64_t hwndValue = static_cast<uint64_t>(info[1].As<Napi::Number>().Int64Value());
    HWND targetHwnd = reinterpret_cast<HWND>(hwndValue);
    Napi::Function callback = info[2].As<Napi::Function>();

    HRESULT hrInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool needUninit = (hrInit == S_OK || hrInit == S_FALSE);
    if (FAILED(hrInit)) {
        callback.Call({ env.Null() });
        return env.Undefined();
    }

    std::string result;

    // IE / 旧版 Edge 优先尝试 IWebBrowserApp.LocationURL。
    if (browserNameLower == "iexplore" || browserNameLower == "microsoftedge") {
        result = GetShellWindowLocationUrl(targetHwnd);
    }

    // Chromium / Firefox 以及 shell windows 失败后的回退都走 UI Automation。
    if (result.empty()) {
        const std::wstring uiaResult = ReadBrowserUrlByUIAutomation(targetHwnd);
        if (!uiaResult.empty()) {
            result = WideToUtf8String(uiaResult);
        }
    }

    if (needUninit) {
        CoUninitialize();
    }

    Napi::Value resultValue = result.empty() ? env.Null() : Napi::String::New(env, result);
    callback.Call({ resultValue });
    return env.Undefined();
}

// 模块初始化
Napi::Object Init(Napi::Env env, Napi::Object exports) {
    exports.Set("startMonitor", Napi::Function::New(env, StartMonitor));
    exports.Set("stopMonitor", Napi::Function::New(env, StopMonitor));
    exports.Set("pauseMonitor", Napi::Function::New(env, PauseMonitor));
    exports.Set("resumeMonitor", Napi::Function::New(env, ResumeMonitor));
    exports.Set("startWindowMonitor", Napi::Function::New(env, StartWindowMonitor));
    exports.Set("stopWindowMonitor", Napi::Function::New(env, StopWindowMonitor));
    exports.Set("getActiveWindow", Napi::Function::New(env, GetActiveWindowInfo));
    exports.Set("activateWindow", Napi::Function::New(env, ActivateWindow));
    exports.Set("simulatePaste", Napi::Function::New(env, SimulatePaste));
    exports.Set("simulateKeyboardTap", Napi::Function::New(env, SimulateKeyboardTap));
    exports.Set("simulateMouseMove", Napi::Function::New(env, SimulateMouseMove));
    exports.Set("simulateMouseClick", Napi::Function::New(env, SimulateMouseClick));
    exports.Set("simulateMouseDoubleClick", Napi::Function::New(env, SimulateMouseDoubleClick));
    exports.Set("simulateMouseRightClick", Napi::Function::New(env, SimulateMouseRightClick));
    exports.Set("ensureOptimizedShortcutListener", Napi::Function::New(env, EnsureOptimizedShortcutListener));
    exports.Set("stopOptimizedShortcutListener", Napi::Function::New(env, StopOptimizedShortcutListener));
    exports.Set("registerOptimizedShortcut", Napi::Function::New(env, RegisterOptimizedShortcut));
    exports.Set("unregisterOptimizedShortcut", Napi::Function::New(env, UnregisterOptimizedShortcut));
    exports.Set("getOptimizedShortcutCount", Napi::Function::New(env, GetOptimizedShortcutCount));
    exports.Set("startRegionCapture", Napi::Function::New(env, StartRegionCapture));
    exports.Set("primeScreenshotFrame", Napi::Function::New(env, PrimeScreenshotFrame));
    exports.Set("startRegionCaptureWithPrimedFrame", Napi::Function::New(env, StartRegionCaptureWithPrimedFrame));
    exports.Set("getClipboardFiles", Napi::Function::New(env, GetClipboardFiles));
    exports.Set("setClipboardFiles", Napi::Function::New(env, SetClipboardFiles));
    exports.Set("startMouseMonitor", Napi::Function::New(env, StartMouseMonitor));
    exports.Set("stopMouseMonitor", Napi::Function::New(env, StopMouseMonitor));
    exports.Set("startColorPicker", Napi::Function::New(env, StartColorPicker));
    exports.Set("stopColorPicker", Napi::Function::New(env, StopColorPicker));
    exports.Set("getUwpApps", Napi::Function::New(env, GetUwpApps));
    exports.Set("launchUwpApp", Napi::Function::New(env, LaunchUwpApp));
    exports.Set("getFileIcon", Napi::Function::New(env, GetFileIcon));
    exports.Set("resolveMuiStrings", Napi::Function::New(env, ResolveMuiStrings));
    exports.Set("scanWindowsShortcuts", Napi::Function::New(env, ScanWindowsShortcuts));
    exports.Set("unicodeType", Napi::Function::New(env, UnicodeType));
    // 通过 COM IShellWindows 查询 Explorer 窗口的当前文件夹路径
    exports.Set("getExplorerFolderPath", Napi::Function::New(env, GetExplorerFolderPath));
    // 获取所有打开的文件资源管理器窗口的 URL 列表
    exports.Set("getAllExplorerWindows", Napi::Function::New(env, GetAllExplorerWindows));
    // 设置 Explorer 或文件选择对话框地址栏
    exports.Set("setAddressBar", Napi::Function::New(env, SetAddressBar));
    // 判断窗口是否是可安全修改地址栏的文件定位窗口
    exports.Set("isFileLocationWindow", Napi::Function::New(env, IsFileLocationWindowBinding));
    // 读取指定浏览器窗口的当前 URL
    exports.Set("readBrowserWindowUrl", Napi::Function::New(env, ReadBrowserWindowUrl));
    exports.Set("getSelectedContent", Napi::Function::New(env, GetSelectedContent));
    return exports;
}

NODE_API_MODULE(ztools_native, Init)
