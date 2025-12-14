#include <napi.h>
#include <CoreGraphics/CoreGraphics.h>
#include <ApplicationServices/ApplicationServices.h>
#include <Carbon/Carbon.h>
#include <string>
#include <thread>
#include <atomic>
#include <map>
#include <chrono>

// 全局变量
static CFMachPortRef g_eventTap = nullptr;
static CFRunLoopSourceRef g_runLoopSource = nullptr;
static napi_threadsafe_function g_eventHookTsfn = nullptr;
static std::atomic<bool> g_isEventHooking(false);
static int g_eventHookEffect = 0;  // 1=鼠标, 2=键盘, 3=两者
static std::thread g_eventHookThread;

// 事件数据结构
struct EventData {
    int type;  // 1=鼠标, 2=键盘
    union {
        struct {
            int eventCode;
            int x;
            int y;
        } mouse;
        struct {
            char keyName[64];
            bool shiftKey;
            bool ctrlKey;
            bool altKey;
            bool metaKey;
            bool flagsChange;
        } keyboard;
    } data;
};

// 将 keyCode 转换为键名
std::string GetKeyName(CGKeyCode keyCode) {
    static std::map<CGKeyCode, std::string> keyMap = {
        // 字母键
        {0, "A"}, {11, "B"}, {8, "C"}, {2, "D"}, {14, "E"}, {3, "F"}, {5, "G"}, {4, "H"},
        {34, "I"}, {38, "J"}, {40, "K"}, {37, "L"}, {46, "M"}, {45, "N"}, {31, "O"},
        {35, "P"}, {12, "Q"}, {15, "R"}, {1, "S"}, {17, "T"}, {32, "U"}, {9, "V"},
        {13, "W"}, {7, "X"}, {16, "Y"}, {6, "Z"},
        
        // 数字键
        {29, "0"}, {18, "1"}, {19, "2"}, {20, "3"}, {21, "4"}, {23, "5"},
        {22, "6"}, {26, "7"}, {28, "8"}, {25, "9"},
        
        // 功能键
        {122, "F1"}, {120, "F2"}, {99, "F3"}, {118, "F4"}, {96, "F5"}, {97, "F6"},
        {98, "F7"}, {100, "F8"}, {101, "F9"}, {109, "F10"}, {103, "F11"}, {111, "F12"},
        
        // 特殊键
        {36, "Return"}, {48, "Tab"}, {49, "Space"}, {51, "Backspace"},
        {53, "Escape"}, {50, "`"}, {57, "CapsLock"}, {63, "Fn"},
        
        // 符号键
        {27, "-"},      // Minus
        {24, "="},      // Equals
        {33, "["},      // Left Bracket
        {30, "]"},      // Right Bracket
        {42, "\\"},     // Backslash (需要转义)
        {41, ";"},      // Semicolon
        {39, "'"},      // Quote
        {43, ","},      // Comma
        {47, "."},      // Period
        {44, "/"},      // Slash
        
        // 方向键
        {123, "Left"}, {124, "Right"}, {125, "Down"}, {126, "Up"},
        
        // 修饰键（区分左右）
        {56, "Left Shift"}, {60, "Right Shift"},
        {58, "Left Option"}, {61, "Right Option"},
        {59, "Left Control"}, {62, "Right Control"},
        {55, "Left Command"}, {54, "Right Command"}
    };
    
    auto it = keyMap.find(keyCode);
    if (it != keyMap.end()) {
        return it->second;
    }
    
    return "Unknown";
}

// 判断是否是修饰键
bool IsModifierKey(CGKeyCode keyCode) {
    // 修饰键的 keyCode: 56(Left Shift), 60(Right Shift), 58(Left Option), 61(Right Option),
    // 59(Left Control), 62(Right Control), 55(Left Command), 54(Right Command)
    return keyCode == 56 || keyCode == 60 || keyCode == 58 || keyCode == 61 ||
           keyCode == 59 || keyCode == 62 || keyCode == 55 || keyCode == 54;
}

// CGEventTap 回调函数
CGEventRef EventTapCallback(CGEventTapProxy proxy, CGEventType type, CGEventRef event, void* refcon) {
    if (!g_isEventHooking || g_eventHookTsfn == nullptr) {
        return event;
    }
    
    EventData* eventData = new EventData();
    
    // 处理鼠标事件
    if ((g_eventHookEffect & 0x01) != 0) {
        int eventCode = 0;
        CGPoint location = CGEventGetLocation(event);
        
        switch (type) {
            case kCGEventLeftMouseDown:
                eventCode = 1;
                break;
            case kCGEventLeftMouseUp:
                eventCode = 2;
                break;
            case kCGEventRightMouseDown:
                eventCode = 3;
                break;
            case kCGEventRightMouseUp:
                eventCode = 4;
                break;
            default:
                break;
        }
        
        if (eventCode != 0) {
            eventData->type = 1;  // 鼠标事件
            eventData->data.mouse.eventCode = eventCode;
            eventData->data.mouse.x = (int)location.x;
            eventData->data.mouse.y = (int)location.y;
            
            napi_call_threadsafe_function(g_eventHookTsfn, eventData, napi_tsfn_nonblocking);
            return event;
        }
    }
    
    // 处理键盘事件
    if ((g_eventHookEffect & 0x02) != 0) {
        CGKeyCode keyCode = 0;
        bool flagsChange = false;
        
        switch (type) {
            case kCGEventKeyDown:
                keyCode = (CGKeyCode)CGEventGetIntegerValueField(event, kCGKeyboardEventKeycode);
                flagsChange = false;
                break;
            case kCGEventKeyUp:
                // 只有修饰键才触发弹起事件
                keyCode = (CGKeyCode)CGEventGetIntegerValueField(event, kCGKeyboardEventKeycode);
                if (!IsModifierKey(keyCode)) {
                    delete eventData;
                    return event;  // 非修饰键的弹起事件不处理
                }
                flagsChange = false;
                break;
            case kCGEventFlagsChanged:
                keyCode = (CGKeyCode)CGEventGetIntegerValueField(event, kCGKeyboardEventKeycode);
                flagsChange = true;
                break;
            default:
                delete eventData;
                return event;
        }
        
        std::string keyName = GetKeyName(keyCode);
        
        // 如果键名是 "Unknown"，尝试根据 keyCode 判断修饰键和其他特殊键
        if (keyName == "Unknown") {
            switch (keyCode) {
                case 54: keyName = "Right Command"; break;
                case 55: keyName = "Left Command"; break;
                case 56: keyName = "Left Shift"; break;
                case 60: keyName = "Right Shift"; break;
                case 58: keyName = "Left Option"; break;
                case 61: keyName = "Right Option"; break;
                case 59: keyName = "Left Control"; break;
                case 62: keyName = "Right Control"; break;
                case 50: keyName = "`"; break;
                case 57: keyName = "CapsLock"; break;
                case 63: keyName = "Fn"; break;
                case 48: keyName = "Tab"; break;
                default:
                    delete eventData;
                    return event;
            }
        }
        
        // 对于修饰键，左侧不带 "Left" 前缀，右侧带 "Right" 前缀
        if (keyName == "Left Control") {
            keyName = "Control";
        } else if (keyName == "Left Shift") {
            keyName = "Shift";
        } else if (keyName == "Left Option") {
            keyName = "Option";
        } else if (keyName == "Left Command") {
            keyName = "Command";
        }
        // Right Control, Right Shift, Right Option, Right Command 保持不变
        
        // 检查修饰键状态
        // 对于 flagsChanged 事件，CGEventGetFlags 返回的是事件发生后的状态
        // 对于 keyDown/keyUp 事件，也使用事件中的 flags
        CGEventFlags flags = CGEventGetFlags(event);
        bool shiftKey = (flags & kCGEventFlagMaskShift) != 0;
        bool ctrlKey = (flags & kCGEventFlagMaskControl) != 0;
        bool altKey = (flags & kCGEventFlagMaskAlternate) != 0;
        bool metaKey = (flags & kCGEventFlagMaskCommand) != 0;
        
        // 如果当前事件的键是修饰键，则排除该修饰键的状态
        // 因为这是该修饰键本身的状态变化事件，而不是其他键的按下事件
        if (keyName == "Control" || keyName == "Right Control") {
            ctrlKey = false;
        } else if (keyName == "Shift" || keyName == "Right Shift") {
            shiftKey = false;
        } else if (keyName == "Option" || keyName == "Right Option") {
            altKey = false;
        } else if (keyName == "Command" || keyName == "Right Command") {
            metaKey = false;
        }
        
        eventData->type = 2;  // 键盘事件
        strncpy(eventData->data.keyboard.keyName, keyName.c_str(), sizeof(eventData->data.keyboard.keyName) - 1);
        eventData->data.keyboard.keyName[sizeof(eventData->data.keyboard.keyName) - 1] = '\0';
        eventData->data.keyboard.shiftKey = shiftKey;
        eventData->data.keyboard.ctrlKey = ctrlKey;
        eventData->data.keyboard.altKey = altKey;
        eventData->data.keyboard.metaKey = metaKey;
        eventData->data.keyboard.flagsChange = flagsChange;
        
        napi_call_threadsafe_function(g_eventHookTsfn, eventData, napi_tsfn_nonblocking);
        return event;
    }
    
    delete eventData;
    return event;
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

// 事件钩子运行循环线程
void EventHookThread() {
    // 确定要监听的事件类型
    CGEventMask eventMask = 0;
    if ((g_eventHookEffect & 0x01) != 0) {
        // 鼠标事件
        eventMask |= (1 << kCGEventLeftMouseDown);
        eventMask |= (1 << kCGEventLeftMouseUp);
        eventMask |= (1 << kCGEventRightMouseDown);
        eventMask |= (1 << kCGEventRightMouseUp);
    }
    if ((g_eventHookEffect & 0x02) != 0) {
        // 键盘事件
        eventMask |= (1 << kCGEventKeyDown);
        eventMask |= (1 << kCGEventKeyUp);
        eventMask |= (1 << kCGEventFlagsChanged);
    }
    
    // 创建事件钩子
    g_eventTap = CGEventTapCreate(
        kCGSessionEventTap,
        kCGHeadInsertEventTap,
        kCGEventTapOptionDefault,
        eventMask,
        EventTapCallback,
        nullptr
    );
    
    if (g_eventTap == nullptr) {
        g_isEventHooking = false;
        return;
    }
    
    // 创建运行循环源
    g_runLoopSource = CFMachPortCreateRunLoopSource(kCFAllocatorDefault, g_eventTap, 0);
    if (g_runLoopSource == nullptr) {
        CFRelease(g_eventTap);
        g_eventTap = nullptr;
        g_isEventHooking = false;
        return;
    }
    
    // 获取当前运行循环
    CFRunLoopRef runLoop = CFRunLoopGetCurrent();
    
    // 添加到运行循环
    CFRunLoopAddSource(runLoop, g_runLoopSource, kCFRunLoopCommonModes);
    
    // 启用事件钩子
    CGEventTapEnable(g_eventTap, true);
    
    // 运行运行循环
    CFRunLoopRun();
}

// 启动事件钩子
Napi::Value HookEvent(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    
    // 检查辅助功能权限
    CFDictionaryRef options = CFDictionaryCreate(
        nullptr,
        (const void**)&kAXTrustedCheckOptionPrompt,
        (const void**)&kCFBooleanTrue,
        1,
        &kCFTypeDictionaryKeyCallBacks,
        &kCFTypeDictionaryValueCallBacks
    );
    Boolean accessEnabled = AXIsProcessTrustedWithOptions(options);
    if (options) {
        CFRelease(options);
    }
    
    if (!accessEnabled) {
        Napi::Error::New(env, "Accessibility permission not granted for event hook").ThrowAsJavaScriptException();
        return env.Undefined();
    }
    
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
    
    // 启动事件钩子线程
    g_eventHookThread = std::thread(EventHookThread);
    
    // 等待一小段时间确保线程启动
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    
    // 检查是否成功启动
    if (!g_isEventHooking || g_eventTap == nullptr) {
        if (g_eventHookThread.joinable()) {
            g_eventHookThread.join();
        }
        napi_release_threadsafe_function(g_eventHookTsfn, napi_tsfn_release);
        g_eventHookTsfn = nullptr;
        Napi::Error::New(env, "Failed to start event hook").ThrowAsJavaScriptException();
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
    
    // 停止运行循环
    if (g_runLoopSource != nullptr) {
        CFRunLoopRef runLoop = CFRunLoopGetCurrent();
        CFRunLoopRemoveSource(runLoop, g_runLoopSource, kCFRunLoopCommonModes);
    }
    
    if (g_eventTap != nullptr) {
        CGEventTapEnable(g_eventTap, false);
    }
    
    // 停止运行循环
    if (g_runLoopSource != nullptr) {
        CFRunLoopStop(CFRunLoopGetCurrent());
    }
    
    // 等待线程结束
    if (g_eventHookThread.joinable()) {
        g_eventHookThread.join();
    }
    
    // 清理资源
    if (g_runLoopSource != nullptr) {
        CFRelease(g_runLoopSource);
        g_runLoopSource = nullptr;
    }
    
    if (g_eventTap != nullptr) {
        CFRelease(g_eventTap);
        g_eventTap = nullptr;
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
