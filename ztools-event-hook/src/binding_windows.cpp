#include <napi.h>
#define NOMINMAX  // 防止 Windows.h 定义 min/max 宏
#include <windows.h>
#include <windowsx.h>  // For GET_X_LPARAM, GET_Y_LPARAM
#include <thread>
#include <chrono>
#include <string>
#include <atomic>
#include <map>         // For key mapping

// 全局变量 - 事件钩子
static HHOOK g_mouseHook = NULL;
static HHOOK g_keyboardHook = NULL;
static std::atomic<bool> g_isEventHooking(false);
static napi_threadsafe_function g_eventHookTsfn = nullptr;
static std::thread g_eventHookThread;
static int g_eventHookEffect = 0;  // 1=鼠标, 2=键盘, 3=两者

// ==================== 事件钩子功能 ====================

// 事件数据结构
struct MouseEventData {
    int eventCode;
    int x;
    int y;
};

struct KeyboardEventData {
    char keyName[64];
    bool shiftKey;
    bool ctrlKey;
    bool altKey;
    bool metaKey;
    bool flagsChange;
};

struct EventData {
    int type;  // 1=鼠标, 2=键盘
    union {
        MouseEventData mouse;
        KeyboardEventData keyboard;
    } data;
};

// 鼠标钩子回调函数
LRESULT CALLBACK MouseHookProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode >= 0 && g_isEventHooking && (g_eventHookEffect & 0x01) != 0) {
        if (g_eventHookTsfn != nullptr) {
            MSLLHOOKSTRUCT* pMouseStruct = (MSLLHOOKSTRUCT*)lParam;
            int eventCode = 0;
            
            switch (wParam) {
                case WM_LBUTTONDOWN:
                    eventCode = 0x0201;
                    break;
                case WM_LBUTTONUP:
                    eventCode = 0x0202;
                    break;
                case WM_RBUTTONDOWN:
                    eventCode = 0x0204;
                    break;
                case WM_RBUTTONUP:
                    eventCode = 0x0205;
                    break;
            }
            
            if (eventCode != 0) {
                EventData* eventData = new EventData();
                eventData->type = 1;  // 鼠标事件
                eventData->data.mouse.eventCode = eventCode;
                eventData->data.mouse.x = pMouseStruct->pt.x;
                eventData->data.mouse.y = pMouseStruct->pt.y;
                
                napi_call_threadsafe_function(g_eventHookTsfn, eventData, napi_tsfn_nonblocking);
            }
        }
    }
    
    return CallNextHookEx(g_mouseHook, nCode, wParam, lParam);
}

// 判断是否是修饰键
bool IsModifierKey(WORD vkCode) {
    return vkCode == VK_LSHIFT || vkCode == VK_RSHIFT ||
           vkCode == VK_LCONTROL || vkCode == VK_RCONTROL ||
           vkCode == VK_LMENU || vkCode == VK_RMENU ||
           vkCode == VK_LWIN || vkCode == VK_RWIN ||
           vkCode == VK_SHIFT || vkCode == VK_CONTROL || vkCode == VK_MENU;
}

// 键盘钩子回调函数
LRESULT CALLBACK KeyboardHookProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode >= 0 && g_isEventHooking && (g_eventHookEffect & 0x02) != 0) {
        if (g_eventHookTsfn != nullptr) {
            KBDLLHOOKSTRUCT* pKeyboardStruct = (KBDLLHOOKSTRUCT*)lParam;
            WORD vkCode = pKeyboardStruct->vkCode;
            bool isKeyUp = (lParam & 0x80000000) != 0;  // 检查是否是按键弹起
            
            // 如果不是修饰键的弹起事件，只处理按下事件
            if (isKeyUp && !IsModifierKey(vkCode)) {
                return CallNextHookEx(g_keyboardHook, nCode, wParam, lParam);
            }
            
            // 获取键名
            std::string keyName = GetKeyNameFromVK(vkCode);
            
            // 如果键名是 "Unknown"，不进行回调
            if (keyName == "Unknown") {
                return CallNextHookEx(g_keyboardHook, nCode, wParam, lParam);
            }
            
            // 对于修饰键，左侧不带 "Left" 前缀，右侧带 "Right" 前缀（与 macOS 保持一致）
            if (keyName == "Left Control") {
                keyName = "Control";
            } else if (keyName == "Left Shift") {
                keyName = "Shift";
            } else if (keyName == "Left Alt") {
                keyName = "Alt";
            } else if (keyName == "Left Win") {
                keyName = "Win";
            }
            // Right Control, Right Shift, Right Alt, Right Win 保持不变
            
            // 检查修饰键状态
            bool shiftKey = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
            bool ctrlKey = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
            bool altKey = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
            bool metaKey = (GetAsyncKeyState(VK_LWIN) & 0x8000) != 0 || (GetAsyncKeyState(VK_RWIN) & 0x8000) != 0;
            
            // 如果当前事件的键是修饰键，则排除该修饰键的状态
            // 因为这是该修饰键本身的状态变化事件，而不是其他键的按下事件
            if (keyName == "Control" || keyName == "Right Control") {
                ctrlKey = false;
            } else if (keyName == "Shift" || keyName == "Right Shift") {
                shiftKey = false;
            } else if (keyName == "Alt" || keyName == "Right Alt") {
                altKey = false;
            } else if (keyName == "Win" || keyName == "Right Win") {
                metaKey = false;
            }
            
            // flagsChange: 只有修饰键的状态变化事件才是 true
            // 对于修饰键的按下/弹起事件，flagsChange 为 true
            // 对于其他按键，flagsChange 为 false
            bool flagsChange = IsModifierKey(vkCode);
            
            EventData* eventData = new EventData();
            eventData->type = 2;  // 键盘事件
            strncpy_s(eventData->data.keyboard.keyName, sizeof(eventData->data.keyboard.keyName), keyName.c_str(), _TRUNCATE);
            eventData->data.keyboard.shiftKey = shiftKey;
            eventData->data.keyboard.ctrlKey = ctrlKey;
            eventData->data.keyboard.altKey = altKey;
            eventData->data.keyboard.metaKey = metaKey;
            eventData->data.keyboard.flagsChange = flagsChange;
            
            napi_call_threadsafe_function(g_eventHookTsfn, eventData, napi_tsfn_nonblocking);
        }
    }
    
    return CallNextHookEx(g_keyboardHook, nCode, wParam, lParam);
}

// 将虚拟键码转换为键名
std::string GetKeyNameFromVK(WORD vkCode) {
    static std::map<WORD, std::string> keyMap = {
        // 字母键
        {'A', "A"}, {'B', "B"}, {'C', "C"}, {'D', "D"}, {'E', "E"}, {'F', "F"},
        {'G', "G"}, {'H', "H"}, {'I', "I"}, {'J', "J"}, {'K', "K"}, {'L', "L"},
        {'M', "M"}, {'N', "N"}, {'O', "O"}, {'P', "P"}, {'Q', "Q"}, {'R', "R"},
        {'S', "S"}, {'T', "T"}, {'U', "U"}, {'V', "V"}, {'W', "W"}, {'X', "X"},
        {'Y', "Y"}, {'Z', "Z"},
        
        // 数字键
        {'0', "0"}, {'1', "1"}, {'2', "2"}, {'3', "3"}, {'4', "4"},
        {'5', "5"}, {'6', "6"}, {'7', "7"}, {'8', "8"}, {'9', "9"},
        
        // 功能键
        {VK_F1, "F1"}, {VK_F2, "F2"}, {VK_F3, "F3"}, {VK_F4, "F4"},
        {VK_F5, "F5"}, {VK_F6, "F6"}, {VK_F7, "F7"}, {VK_F8, "F8"},
        {VK_F9, "F9"}, {VK_F10, "F10"}, {VK_F11, "F11"}, {VK_F12, "F12"},
        
        // 特殊键
        {VK_RETURN, "Enter"}, {VK_TAB, "Tab"}, {VK_SPACE, "Space"},
        {VK_BACK, "Backspace"}, {VK_DELETE, "Backspace"}, {VK_ESCAPE, "Escape"},
        {VK_CAPITAL, "CapsLock"}, {VK_OEM_3, "`"},  // Caps Lock 和 `
        
        // 符号键
        {VK_OEM_MINUS, "-"},      // Minus
        {VK_OEM_PLUS, "="},       // Equals
        {VK_OEM_4, "["},          // Left Bracket
        {VK_OEM_6, "]"},          // Right Bracket
        {VK_OEM_5, "\\"},         // Backslash
        {VK_OEM_1, ";"},          // Semicolon
        {VK_OEM_7, "'"},          // Quote
        {VK_OEM_COMMA, ","},      // Comma
        {VK_OEM_PERIOD, "."},     // Period
        {VK_OEM_2, "/"}           // Slash
        
        // 方向键
        {VK_LEFT, "Left"}, {VK_RIGHT, "Right"}, {VK_UP, "Up"}, {VK_DOWN, "Down"},
        
        // 修饰键（区分左右）
        {VK_LSHIFT, "Left Shift"}, {VK_RSHIFT, "Right Shift"},
        {VK_LCONTROL, "Left Control"}, {VK_RCONTROL, "Right Control"},
        {VK_LMENU, "Left Alt"}, {VK_RMENU, "Right Alt"},
        {VK_LWIN, "Left Win"}, {VK_RWIN, "Right Win"},
        // 兼容旧代码（如果只检测到 VK_SHIFT 等）
        {VK_SHIFT, "Shift"}, {VK_CONTROL, "Ctrl"}, {VK_MENU, "Alt"}
    };
    
    auto it = keyMap.find(vkCode);
    if (it != keyMap.end()) {
        return it->second;
    }
    
    // 尝试使用 MapVirtualKey 获取键名
    UINT scanCode = MapVirtualKeyA(vkCode, MAPVK_VK_TO_VSC);
    if (scanCode != 0) {
        char keyName[256] = {0};
        LONG lParam = (scanCode << 16);
        if (GetKeyNameTextA(lParam, keyName, sizeof(keyName)) > 0) {
            std::string result(keyName);
            // 清理键名（移除多余的空格）
            while (!result.empty() && result.back() == ' ') {
                result.pop_back();
            }
            if (!result.empty()) {
                return result;
            }
        }
    }
    
    return "Unknown";
}

// 在主线程调用 JS 回调（事件钩子）
void CallEventHookJs(napi_env env, napi_value js_callback, void* context, void* data) {
    if (env != nullptr && js_callback != nullptr && data != nullptr) {
        EventData* eventData = static_cast<EventData*>(data);
        
        napi_value global;
        napi_get_global(env, &global);
        
        if (eventData->type == 1) {
            // 鼠标事件：eventCode, x, y
            napi_value args[3];
            args[0] = Napi::Number::New(env, eventData->data.mouse.eventCode);
            args[1] = Napi::Number::New(env, eventData->data.mouse.x);
            args[2] = Napi::Number::New(env, eventData->data.mouse.y);
            napi_call_function(env, global, js_callback, 3, args, nullptr);
        } else if (eventData->type == 2) {
            // 键盘事件：keyName, shiftKey, ctrlKey, altKey, metaKey, flagsChange
            napi_value args[6];
            args[0] = Napi::String::New(env, std::string(eventData->data.keyboard.keyName));
            args[1] = Napi::Boolean::New(env, eventData->data.keyboard.shiftKey);
            args[2] = Napi::Boolean::New(env, eventData->data.keyboard.ctrlKey);
            args[3] = Napi::Boolean::New(env, eventData->data.keyboard.altKey);
            args[4] = Napi::Boolean::New(env, eventData->data.keyboard.metaKey);
            args[5] = Napi::Boolean::New(env, eventData->data.keyboard.flagsChange);
            napi_call_function(env, global, js_callback, 6, args, nullptr);
        }
        
        delete eventData;
    }
}

// 事件钩子消息循环线程
void EventHookThread() {
    // 设置钩子
    if ((g_eventHookEffect & 0x01) != 0) {
        g_mouseHook = SetWindowsHookExW(WH_MOUSE_LL, MouseHookProc, GetModuleHandle(NULL), 0);
    }
    if ((g_eventHookEffect & 0x02) != 0) {
        g_keyboardHook = SetWindowsHookExW(WH_KEYBOARD_LL, KeyboardHookProc, GetModuleHandle(NULL), 0);
    }
    
    if (((g_eventHookEffect & 0x01) != 0 && g_mouseHook == NULL) ||
        ((g_eventHookEffect & 0x02) != 0 && g_keyboardHook == NULL)) {
        g_isEventHooking = false;
        return;
    }
    
    // 运行消息循环
    MSG msg;
    while (g_isEventHooking && GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    
    // 清理钩子
    if (g_mouseHook != NULL) {
        UnhookWindowsHookEx(g_mouseHook);
        g_mouseHook = NULL;
    }
    if (g_keyboardHook != NULL) {
        UnhookWindowsHookEx(g_keyboardHook);
        g_keyboardHook = NULL;
    }
}

// 启动事件钩子
Napi::Value HookEvent(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    
    // 参数1：effect（必需）
    if (info.Length() < 1 || !info[0].IsNumber()) {
        Napi::TypeError::New(env, "Expected effect as first argument (number: 1=mouse, 2=keyboard, 3=both)").ThrowAsJavaScriptException();
        return env.Undefined();
    }
    
    int effect = info[0].As<Napi::Number>().Int32Value();
    if (effect < 1 || effect > 3) {
        Napi::TypeError::New(env, "effect must be 1 (mouse), 2 (keyboard), or 3 (both)").ThrowAsJavaScriptException();
        return env.Undefined();
    }
    
    // 参数2：callback（必需）
    if (info.Length() < 2 || !info[1].IsFunction()) {
        Napi::TypeError::New(env, "Expected callback function as second argument").ThrowAsJavaScriptException();
        return env.Undefined();
    }
    
    if (g_isEventHooking) {
        Napi::Error::New(env, "Event hook already started").ThrowAsJavaScriptException();
        return env.Undefined();
    }
    
    // 创建线程安全函数
    napi_value callback = info[1];
    napi_value resource_name;
    napi_create_string_utf8(env, "EventHookCallback", NAPI_AUTO_LENGTH, &resource_name);
    
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
        CallEventHookJs,
        &g_eventHookTsfn
    );
    
    if (status != napi_ok) {
        Napi::Error::New(env, "Failed to create threadsafe function").ThrowAsJavaScriptException();
        return env.Undefined();
    }
    
    g_eventHookEffect = effect;
    g_isEventHooking = true;
    
    // 启动消息循环线程
    g_eventHookThread = std::thread(EventHookThread);
    
    // 等待一小段时间确保线程启动
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    
    // 检查是否成功启动
    if (!g_isEventHooking) {
        if (g_eventHookThread.joinable()) {
            g_eventHookThread.join();
        }
        napi_release_threadsafe_function(g_eventHookTsfn, napi_tsfn_release);
        g_eventHookTsfn = nullptr;
        Napi::Error::New(env, "Failed to set event hooks").ThrowAsJavaScriptException();
        return env.Undefined();
    }
    
    return env.Undefined();
}

// 停止事件钩子
Napi::Value UnhookEvent(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    
    if (!g_isEventHooking) {
        return env.Undefined();
    }
    
    g_isEventHooking = false;
    
    // 停止消息循环
    if (g_eventHookThread.joinable()) {
        PostThreadMessage(GetThreadId(g_eventHookThread.native_handle()), WM_QUIT, 0, 0);
        g_eventHookThread.join();
    }
    
    // 释放线程安全函数
    if (g_eventHookTsfn != nullptr) {
        napi_release_threadsafe_function(g_eventHookTsfn, napi_tsfn_release);
        g_eventHookTsfn = nullptr;
    }
    
    g_eventHookEffect = 0;
    
    return env.Undefined();
}

// 模块初始化
Napi::Object Init(Napi::Env env, Napi::Object exports) {
    exports.Set("hookEvent", Napi::Function::New(env, HookEvent));
    exports.Set("unhookEvent", Napi::Function::New(env, UnhookEvent));
    return exports;
}

NODE_API_MODULE(ztools_event_hook, Init)

