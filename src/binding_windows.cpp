#include <napi.h>
#include <windows.h>
#include <windowsx.h>  // For GET_X_LPARAM, GET_Y_LPARAM
#include <psapi.h>
#include <thread>
#include <chrono>
#include <string>
#include <atomic>
#include <algorithm>   // For std::min, std::max
#include <map>         // For key mapping
#include <vector>      // For input events
#include <shellapi.h>  // For DragQueryFile

// 如果 DROPFILES 未定义，手动定义
#ifndef DROPFILES
typedef struct _DROPFILES {
    DWORD pFiles;
    POINT pt;
    BOOL fNC;
    BOOL fWide;
} DROPFILES, *LPDROPFILES;
#endif

// 取消与自定义函数名冲突的Windows宏
#ifdef GetActiveWindow
#undef GetActiveWindow
#endif

// 全局变量 - 剪贴板监控
static HWND g_hwnd = NULL;
static std::thread g_messageThread;
static std::atomic<bool> g_isMonitoring(false);
static napi_threadsafe_function g_tsfn = nullptr;

// 全局变量 - 窗口监控
static HWINEVENTHOOK g_winEventHook = NULL;
static std::atomic<bool> g_isWindowMonitoring(false);
static napi_threadsafe_function g_windowTsfn = nullptr;
static std::thread g_windowMessageThread;

// 全局变量 - 区域截图
static HWND g_screenshotOverlayWindow = NULL;
static std::atomic<bool> g_isCapturing(false);
static napi_threadsafe_function g_screenshotTsfn = nullptr;
static std::thread g_screenshotThread;
static POINT g_selectionStart = {0, 0};
static POINT g_selectionEnd = {0, 0};
static bool g_isSelecting = false;

// 窗口过程（处理剪贴板消息）
LRESULT CALLBACK ClipboardWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CLIPBOARDUPDATE:
            // 剪贴板变化，通知 JS
            if (g_tsfn != nullptr) {
                napi_call_threadsafe_function(g_tsfn, nullptr, napi_tsfn_nonblocking);
            }
            return 0;
        case WM_DESTROY:
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

// ==================== 窗口监控功能 ====================

// 窗口信息结构（用于线程安全传递）
struct WindowInfo {
    DWORD processId;
    std::string appName;
};

// 获取窗口信息的辅助函数
WindowInfo* GetWindowInfo(HWND hwnd) {
    if (hwnd == NULL) {
        return nullptr;
    }

    WindowInfo* info = new WindowInfo();

    // 获取进程 ID
    GetWindowThreadProcessId(hwnd, &info->processId);

    // 获取进程句柄
    HANDLE hProcess = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, info->processId);
    if (hProcess) {
        // 获取可执行文件路径
        WCHAR path[MAX_PATH] = {0};
        if (GetModuleFileNameExW(hProcess, NULL, path, MAX_PATH)) {
            // 提取文件名（去掉路径）
            std::wstring fullPath(path);
            size_t lastSlash = fullPath.find_last_of(L"\\");
            std::wstring fileName = (lastSlash != std::wstring::npos)
                ? fullPath.substr(lastSlash + 1)
                : fullPath;

            // 去掉 .exe 扩展名
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

        napi_value appName;
        napi_create_string_utf8(env, info->appName.c_str(), NAPI_AUTO_LENGTH, &appName);
        napi_set_named_property(env, result, "appName", appName);

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
    // 只处理前台窗口切换事件
    if (event == EVENT_SYSTEM_FOREGROUND && g_windowTsfn != nullptr) {
        // 获取窗口信息
        WindowInfo* info = GetWindowInfo(hwnd);
        if (info != nullptr) {
            // 通过线程安全函数传递到 JS
            napi_call_threadsafe_function(g_windowTsfn, info, napi_tsfn_nonblocking);
        }
    }
}

// 窗口监控消息循环线程
void WindowMonitorThread() {
    // 在此线程中设置窗口事件钩子
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
        WindowInfo* info = GetWindowInfo(currentWindow);
        if (info != nullptr) {
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

    // 获取进程句柄
    HANDLE hProcess = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, processId);
    if (hProcess) {
        // 获取可执行文件路径
        WCHAR path[MAX_PATH] = {0};
        if (GetModuleFileNameExW(hProcess, NULL, path, MAX_PATH)) {
            // 提取文件名（去掉路径）
            std::wstring fullPath(path);
            size_t lastSlash = fullPath.find_last_of(L"\\");
            std::wstring fileName = (lastSlash != std::wstring::npos)
                ? fullPath.substr(lastSlash + 1)
                : fullPath;

            // 去掉 .exe 扩展名
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

// ==================== 区域截图功能 ====================

// 截图结果结构
struct ScreenshotResult {
    bool success;
    int width;
    int height;
};

// 保存截图到剪贴板
bool SaveBitmapToClipboard(HBITMAP hBitmap) {
    if (!OpenClipboard(NULL)) {
        return false;
    }

    EmptyClipboard();
    
    // 复制 HBITMAP（剪贴板会负责释放）
    BITMAP bm;
    GetObject(hBitmap, sizeof(BITMAP), &bm);
    
    HBITMAP hBitmapCopy = (HBITMAP)CopyImage(hBitmap, IMAGE_BITMAP, bm.bmWidth, bm.bmHeight, LR_COPYRETURNORG);
    
    SetClipboardData(CF_BITMAP, hBitmapCopy);
    CloseClipboard();
    
    return true;
}

// 截取屏幕区域并保存到剪贴板
ScreenshotResult* CaptureScreenRegion(int x, int y, int width, int height) {
    ScreenshotResult* result = new ScreenshotResult();
    result->success = false;
    result->width = width;
    result->height = height;

    if (width <= 0 || height <= 0) {
        return result;
    }

    // 获取屏幕 DC
    HDC screenDC = GetDC(NULL);
    HDC memDC = CreateCompatibleDC(screenDC);
    
    // 创建位图
    HBITMAP bitmap = CreateCompatibleBitmap(screenDC, width, height);
    HBITMAP oldBitmap = (HBITMAP)SelectObject(memDC, bitmap);
    
    // 复制屏幕内容
    BitBlt(memDC, 0, 0, width, height, screenDC, x, y, SRCCOPY);
    
    // 保存到剪贴板
    result->success = SaveBitmapToClipboard(bitmap);
    
    // 清理
    SelectObject(memDC, oldBitmap);
    DeleteObject(bitmap);
    DeleteDC(memDC);
    ReleaseDC(NULL, screenDC);
    
    return result;
}

// 在主线程调用 JS 回调（截图完成）
void CallScreenshotJs(napi_env env, napi_value js_callback, void* context, void* data) {
    if (env != nullptr && js_callback != nullptr && data != nullptr) {
        ScreenshotResult* result = static_cast<ScreenshotResult*>(data);
        
        // 创建返回对象
        napi_value resultObj;
        napi_create_object(env, &resultObj);
        
        napi_value success;
        napi_get_boolean(env, result->success, &success);
        napi_set_named_property(env, resultObj, "success", success);
        
        if (result->success) {
            napi_value width;
            napi_create_int32(env, result->width, &width);
            napi_set_named_property(env, resultObj, "width", width);
            
            napi_value height;
            napi_create_int32(env, result->height, &height);
            napi_set_named_property(env, resultObj, "height", height);
        }
        
        // 调用回调
        napi_value global;
        napi_get_global(env, &global);
        napi_call_function(env, global, js_callback, 1, &resultObj, nullptr);
        
        delete result;
    }
}

// 使用 UpdateLayeredWindow 绘制选区遮罩（支持像素级透明度）
void DrawSelectionOverlay(HWND hwnd) {
    // 获取窗口尺寸
    RECT clientRect;
    GetClientRect(hwnd, &clientRect);
    int width = clientRect.right - clientRect.left;
    int height = clientRect.bottom - clientRect.top;
    
    // 创建内存 DC 和位图
    HDC screenDC = GetDC(NULL);
    HDC memDC = CreateCompatibleDC(screenDC);
    HBITMAP memBitmap = CreateCompatibleBitmap(screenDC, width, height);
    HBITMAP oldBitmap = (HBITMAP)SelectObject(memDC, memBitmap);
    
    // 创建 32 位 BGRA 位图用于 alpha 通道
    BITMAPINFO bmi = {0};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = -height;  // 负值表示自顶向下
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    
    void* pvBits = nullptr;
    HBITMAP hbmAlpha = CreateDIBSection(memDC, &bmi, DIB_RGB_COLORS, &pvBits, NULL, 0);
    HBITMAP oldAlphaBitmap = (HBITMAP)SelectObject(memDC, hbmAlpha);
    
    // 填充位图数据
    BYTE* pixels = (BYTE*)pvBits;
    
    if (g_isSelecting) {
        // 计算选区矩形
        int selLeft = (std::min)(g_selectionStart.x, g_selectionEnd.x);
        int selTop = (std::min)(g_selectionStart.y, g_selectionEnd.y);
        int selRight = (std::max)(g_selectionStart.x, g_selectionEnd.x);
        int selBottom = (std::max)(g_selectionStart.y, g_selectionEnd.y);
        
        // 填充每个像素
        for (int y = 0; y < height; y++) {
            for (int x = 0; x < width; x++) {
                int offset = (y * width + x) * 4;
                
                // 判断是否在选区内
                bool inSelection = (x >= selLeft && x < selRight && y >= selTop && y < selBottom);
                
                if (inSelection) {
                    // 选区内：完全透明
                    pixels[offset + 0] = 0;  // B
                    pixels[offset + 1] = 0;  // G
                    pixels[offset + 2] = 0;  // R
                    pixels[offset + 3] = 0;  // A (完全透明)
                } else {
                    // 选区外：半透明黑色
                    pixels[offset + 0] = 0;  // B
                    pixels[offset + 1] = 0;  // G
                    pixels[offset + 2] = 0;  // R
                    pixels[offset + 3] = 128;  // A (50% 透明)
                }
            }
        }
        
        // 绘制选区边框
        HPEN borderPen = CreatePen(PS_SOLID, 2, RGB(0, 120, 215));
        HPEN oldPen = (HPEN)SelectObject(memDC, borderPen);
        HBRUSH oldBrush = (HBRUSH)SelectObject(memDC, GetStockObject(NULL_BRUSH));
        
        Rectangle(memDC, selLeft, selTop, selRight, selBottom);
        
        SelectObject(memDC, oldPen);
        SelectObject(memDC, oldBrush);
        DeleteObject(borderPen);
    } else {
        // 没有选区时，整个屏幕半透明黑色
        for (int i = 0; i < width * height * 4; i += 4) {
            pixels[i + 0] = 0;    // B
            pixels[i + 1] = 0;    // G
            pixels[i + 2] = 0;    // R
            pixels[i + 3] = 128;  // A
        }
    }
    
    // 使用 UpdateLayeredWindow 更新窗口
    POINT ptSrc = {0, 0};
    POINT ptDst = {0, 0};
    SIZE sizeWnd = {width, height};
    BLENDFUNCTION blend = {0};
    blend.BlendOp = AC_SRC_OVER;
    blend.BlendFlags = 0;
    blend.SourceConstantAlpha = 255;
    blend.AlphaFormat = AC_SRC_ALPHA;
    
    UpdateLayeredWindow(hwnd, screenDC, &ptDst, &sizeWnd, memDC, &ptSrc, 0, &blend, ULW_ALPHA);
    
    // 清理
    SelectObject(memDC, oldAlphaBitmap);
    DeleteObject(hbmAlpha);
    SelectObject(memDC, oldBitmap);
    DeleteObject(memBitmap);
    DeleteDC(memDC);
    ReleaseDC(NULL, screenDC);
}

// 截图遮罩窗口过程
LRESULT CALLBACK ScreenshotOverlayWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_LBUTTONDOWN: {
            // 记录起始点
            g_selectionStart.x = GET_X_LPARAM(lParam);
            g_selectionStart.y = GET_Y_LPARAM(lParam);
            g_selectionEnd = g_selectionStart;
            g_isSelecting = true;
            SetCapture(hwnd);
            DrawSelectionOverlay(hwnd);  // 立即更新显示
            return 0;
        }
        
        case WM_MOUSEMOVE: {
            if (g_isSelecting) {
                // 更新终点
                g_selectionEnd.x = GET_X_LPARAM(lParam);
                g_selectionEnd.y = GET_Y_LPARAM(lParam);
                DrawSelectionOverlay(hwnd);  // 立即更新显示
            }
            return 0;
        }
        
        case WM_LBUTTONUP: {
            if (g_isSelecting) {
                g_isSelecting = false;
                ReleaseCapture();
                
                // 计算选区
                int x = (std::min)(g_selectionStart.x, g_selectionEnd.x);
                int y = (std::min)(g_selectionStart.y, g_selectionEnd.y);
                int width = abs(g_selectionEnd.x - g_selectionStart.x);
                int height = abs(g_selectionEnd.y - g_selectionStart.y);
                
                // 隐藏窗口
                ShowWindow(hwnd, SW_HIDE);
                Sleep(100);  // 等待窗口完全隐藏
                
                // 执行截图
                ScreenshotResult* result = CaptureScreenRegion(x, y, width, height);
                
                // 通过线程安全函数回调 JS
                if (g_screenshotTsfn != nullptr) {
                    napi_call_threadsafe_function(g_screenshotTsfn, result, napi_tsfn_nonblocking);
                }
                
                // 关闭窗口
                DestroyWindow(hwnd);
            }
            return 0;
        }
        
        case WM_KEYDOWN: {
            if (wParam == VK_ESCAPE) {
                // ESC 取消截图
                g_isSelecting = false;
                
                // 回调失败结果
                if (g_screenshotTsfn != nullptr) {
                    ScreenshotResult* result = new ScreenshotResult();
                    result->success = false;
                    result->width = 0;
                    result->height = 0;
                    napi_call_threadsafe_function(g_screenshotTsfn, result, napi_tsfn_nonblocking);
                }
                
                DestroyWindow(hwnd);
            }
            return 0;
        }
        
        case WM_DESTROY: {
            g_screenshotOverlayWindow = NULL;
            g_isCapturing = false;
            PostQuitMessage(0);
            return 0;
        }
    }
    
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

// 截图线程（创建遮罩窗口）
void ScreenshotCaptureThread() {
    // 设置 DPI 感知（Per-Monitor V2）
    // 这样可以正确处理高 DPI 显示器
    typedef DPI_AWARENESS_CONTEXT (WINAPI *SetThreadDpiAwarenessContextProc)(DPI_AWARENESS_CONTEXT);
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32) {
        SetThreadDpiAwarenessContextProc setDpiProc = 
            (SetThreadDpiAwarenessContextProc)GetProcAddress(user32, "SetThreadDpiAwarenessContext");
        if (setDpiProc) {
            setDpiProc(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        }
    }
    
    // 注册窗口类
    WNDCLASSEXW wc = {0};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.lpfnWndProc = ScreenshotOverlayWndProc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.hCursor = LoadCursor(NULL, IDC_CROSS);  // 十字光标
    wc.hbrBackground = CreateSolidBrush(RGB(0, 0, 0));
    wc.lpszClassName = L"ZToolsScreenshotOverlay";
    
    if (!RegisterClassExW(&wc)) {
        g_isCapturing = false;
        return;
    }
    
    // 获取虚拟屏幕尺寸（支持多显示器）
    int screenX = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int screenY = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int screenWidth = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int screenHeight = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    
    // 创建全屏分层窗口（覆盖所有显示器）
    g_screenshotOverlayWindow = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TOOLWINDOW,
        L"ZToolsScreenshotOverlay",
        L"Screenshot Overlay",
        WS_POPUP,
        screenX, screenY, screenWidth, screenHeight,
        NULL, NULL, GetModuleHandle(NULL), NULL
    );
    
    if (g_screenshotOverlayWindow == NULL) {
        UnregisterClassW(L"ZToolsScreenshotOverlay", GetModuleHandle(NULL));
        g_isCapturing = false;
        return;
    }
    
    // 显示窗口
    ShowWindow(g_screenshotOverlayWindow, SW_SHOW);
    SetForegroundWindow(g_screenshotOverlayWindow);
    
    // 初始绘制（没有选区时的半透明遮罩）
    DrawSelectionOverlay(g_screenshotOverlayWindow);
    
    // 消息循环
    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    
    // 清理
    UnregisterClassW(L"ZToolsScreenshotOverlay", GetModuleHandle(NULL));
    g_isCapturing = false;
}

// 启动区域截图
Napi::Value StartRegionCapture(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    
    if (g_isCapturing) {
        Napi::Error::New(env, "Screenshot already in progress").ThrowAsJavaScriptException();
        return env.Undefined();
    }
    
    // 可选的回调函数
    if (info.Length() > 0 && info[0].IsFunction()) {
        Napi::Function callback = info[0].As<Napi::Function>();
        napi_value resource_name;
        napi_create_string_utf8(env, "ScreenshotCallback", NAPI_AUTO_LENGTH, &resource_name);
        
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
            CallScreenshotJs,
            &g_screenshotTsfn
        );
        
        if (status != napi_ok) {
            Napi::Error::New(env, "Failed to create threadsafe function").ThrowAsJavaScriptException();
            return env.Undefined();
        }
    }
    
    g_isCapturing = true;
    g_isSelecting = false;
    
    // 启动截图线程
    g_screenshotThread = std::thread(ScreenshotCaptureThread);
    g_screenshotThread.detach();
    
    return env.Undefined();
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

// 模块初始化
Napi::Object Init(Napi::Env env, Napi::Object exports) {
    exports.Set("startMonitor", Napi::Function::New(env, StartMonitor));
    exports.Set("stopMonitor", Napi::Function::New(env, StopMonitor));
    exports.Set("startWindowMonitor", Napi::Function::New(env, StartWindowMonitor));
    exports.Set("stopWindowMonitor", Napi::Function::New(env, StopWindowMonitor));
    exports.Set("getActiveWindow", Napi::Function::New(env, GetActiveWindowInfo));
    exports.Set("activateWindow", Napi::Function::New(env, ActivateWindow));
    exports.Set("simulatePaste", Napi::Function::New(env, SimulatePaste));
    exports.Set("simulateKeyboardTap", Napi::Function::New(env, SimulateKeyboardTap));
    exports.Set("startRegionCapture", Napi::Function::New(env, StartRegionCapture));
    exports.Set("getClipboardFiles", Napi::Function::New(env, GetClipboardFiles));
    exports.Set("setClipboardFiles", Napi::Function::New(env, SetClipboardFiles));
    return exports;
}

NODE_API_MODULE(ztools_native, Init)
