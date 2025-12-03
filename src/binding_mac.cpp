#include <napi.h>
#include <dlfcn.h>
#include <string>
#include <vector>
#include <cstdlib>

// Swift 动态库函数类型定义
typedef void (*ClipboardCallback)();  // 无参数回调
typedef void (*WindowCallback)(const char*);  // 带JSON字符串参数回调
typedef void (*StartMonitorFunc)(ClipboardCallback);
typedef void (*StopMonitorFunc)();
typedef void (*StartWindowMonitorFunc)(WindowCallback);
typedef void (*StopWindowMonitorFunc)();
typedef char* (*GetActiveWindowFunc)();
typedef int (*ActivateWindowFunc)(const char*);

// 全局变量
static void* swiftLibHandle = nullptr;
static napi_threadsafe_function tsfn = nullptr;
static napi_threadsafe_function windowTsfn = nullptr;
static StartMonitorFunc startMonitorFunc = nullptr;
static StopMonitorFunc stopMonitorFunc = nullptr;
static StartWindowMonitorFunc startWindowMonitorFunc = nullptr;
static StopWindowMonitorFunc stopWindowMonitorFunc = nullptr;
static GetActiveWindowFunc getActiveWindowFunc = nullptr;
static ActivateWindowFunc activateWindowFunc = nullptr;

// 在主线程调用 JS 回调
void CallJs(napi_env env, napi_value js_callback, void* context, void* data) {
    if (env != nullptr && js_callback != nullptr) {
        // 不传递任何参数，只调用回调
        napi_value global;
        napi_get_global(env, &global);
        napi_call_function(env, global, js_callback, 0, nullptr, nullptr);
    }
}

// Swift 回调 -> 推送到线程安全队列
void OnClipboardChanged() {
    if (tsfn != nullptr) {
        // 不需要传递数据
        napi_call_threadsafe_function(tsfn, nullptr, napi_tsfn_nonblocking);
    }
}

// 在主线程调用 JS 回调（窗口监控，带JSON参数）
void CallWindowJs(napi_env env, napi_value js_callback, void* context, void* data) {
    if (env != nullptr && js_callback != nullptr && data != nullptr) {
        char* jsonStr = static_cast<char*>(data);

        // 解析JSON字符串为对象
        Napi::Env napiEnv(env);
        Napi::Object result = Napi::Object::New(napiEnv);

        std::string jsonString(jsonStr);
        free(jsonStr);

        // 查找 "appName":"xxx"
        size_t appNamePos = jsonString.find("\"appName\":\"");
        if (appNamePos != std::string::npos) {
            size_t start = appNamePos + 11;
            size_t end = jsonString.find("\"", start);
            if (end != std::string::npos) {
                std::string appName = jsonString.substr(start, end - start);
                result.Set("appName", Napi::String::New(napiEnv, appName));
            }
        }

        // 查找 "bundleId":"xxx"
        size_t bundleIdPos = jsonString.find("\"bundleId\":\"");
        if (bundleIdPos != std::string::npos) {
            size_t start = bundleIdPos + 12;
            size_t end = jsonString.find("\"", start);
            if (end != std::string::npos) {
                std::string bundleId = jsonString.substr(start, end - start);
                result.Set("bundleId", Napi::String::New(napiEnv, bundleId));
            }
        }

        // 调用回调
        napi_value global;
        napi_get_global(env, &global);
        napi_value resultValue = result;
        napi_call_function(env, global, js_callback, 1, &resultValue, nullptr);
    }
}

// Swift 窗口回调 -> 推送到线程安全队列
void OnWindowChanged(const char* jsonStr) {
    if (windowTsfn != nullptr && jsonStr != nullptr) {
        // 复制字符串
        char* jsonCopy = strdup(jsonStr);
        napi_call_threadsafe_function(windowTsfn, jsonCopy, napi_tsfn_nonblocking);
    }
}

// 获取当前 .node 文件所在目录
std::string GetModuleDirectory() {
    Dl_info info;
    // 获取当前函数的地址，从而定位到 .node 文件
    if (dladdr((void*)GetModuleDirectory, &info) && info.dli_fname) {
        std::string path(info.dli_fname);
        size_t lastSlash = path.find_last_of('/');
        if (lastSlash != std::string::npos) {
            return path.substr(0, lastSlash);
        }
    }
    return ".";
}

// 加载 Swift 动态库
bool LoadSwiftLibrary(Napi::Env env) {
    if (swiftLibHandle != nullptr) {
        return true;  // 已加载
    }

    std::string moduleDir = GetModuleDirectory();

    // 尝试多个路径（优先级从高到低）
    std::vector<std::string> paths = {
        // 1. .node 文件同目录（最常见的部署方式）
        moduleDir + "/libZToolsNative.dylib",
        // 2. .node 文件的上级 lib 目录
        moduleDir + "/../lib/libZToolsNative.dylib",
        // 3. 当前工作目录的 lib 子目录（开发环境）
        "./lib/libZToolsNative.dylib",
        // 4. 当前工作目录（开发环境备选）
        "./libZToolsNative.dylib",
        // 5. 相对路径备选
        "../lib/libZToolsNative.dylib"
    };

    std::string lastError;
    for (const auto& path : paths) {
        swiftLibHandle = dlopen(path.c_str(), RTLD_NOW);
        if (swiftLibHandle != nullptr) {
            break;
        }
        lastError = dlerror();
    }

    if (swiftLibHandle == nullptr) {
        std::string errorMsg = "Failed to load Swift library.\n";
        errorMsg += "Module directory: " + moduleDir + "\n";
        errorMsg += "Tried paths:\n";
        for (const auto& path : paths) {
            errorMsg += "  - " + path + "\n";
        }
        errorMsg += "Last error: " + lastError;

        Napi::Error::New(env, errorMsg).ThrowAsJavaScriptException();
        return false;
    }

    // 加载函数符号
    startMonitorFunc = (StartMonitorFunc)dlsym(swiftLibHandle, "startClipboardMonitor");
    stopMonitorFunc = (StopMonitorFunc)dlsym(swiftLibHandle, "stopClipboardMonitor");
    startWindowMonitorFunc = (StartWindowMonitorFunc)dlsym(swiftLibHandle, "startWindowMonitor");
    stopWindowMonitorFunc = (StopWindowMonitorFunc)dlsym(swiftLibHandle, "stopWindowMonitor");
    getActiveWindowFunc = (GetActiveWindowFunc)dlsym(swiftLibHandle, "getActiveWindow");
    activateWindowFunc = (ActivateWindowFunc)dlsym(swiftLibHandle, "activateWindow");

    if (!startMonitorFunc || !stopMonitorFunc || !startWindowMonitorFunc ||
        !stopWindowMonitorFunc || !getActiveWindowFunc || !activateWindowFunc) {
        Napi::Error::New(env, "Failed to load Swift functions").ThrowAsJavaScriptException();
        dlclose(swiftLibHandle);
        swiftLibHandle = nullptr;
        return false;
    }

    return true;
}

// 启动监控
Napi::Value StartMonitor(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();

    if (!LoadSwiftLibrary(env)) {
        return env.Undefined();
    }

    if (info.Length() < 1 || !info[0].IsFunction()) {
        Napi::TypeError::New(env, "Expected a callback function").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    if (tsfn != nullptr) {
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
        &tsfn
    );

    // 启动 Swift 监控
    startMonitorFunc(OnClipboardChanged);

    return env.Undefined();
}

// 停止监控
Napi::Value StopMonitor(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();

    if (stopMonitorFunc != nullptr) {
        stopMonitorFunc();
    }

    if (tsfn != nullptr) {
        napi_release_threadsafe_function(tsfn, napi_tsfn_release);
        tsfn = nullptr;
    }

    return env.Undefined();
}

// 获取当前激活窗口
Napi::Value GetActiveWindow(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();

    if (!LoadSwiftLibrary(env)) {
        return env.Null();
    }

    char* jsonStr = getActiveWindowFunc();
    if (jsonStr == nullptr) {
        return env.Null();
    }

    // 解析 JSON 字符串
    std::string jsonString(jsonStr);
    free(jsonStr);

    // 手动解析简单的 JSON（避免引入额外依赖）
    Napi::Object result = Napi::Object::New(env);

    // 查找 "appName":"xxx"
    size_t appNamePos = jsonString.find("\"appName\":\"");
    if (appNamePos != std::string::npos) {
        size_t start = appNamePos + 11;  // 跳过 "appName":"
        size_t end = jsonString.find("\"", start);
        if (end != std::string::npos) {
            std::string appName = jsonString.substr(start, end - start);
            result.Set("appName", Napi::String::New(env, appName));
        }
    }

    // 查找 "bundleId":"xxx"
    size_t bundleIdPos = jsonString.find("\"bundleId\":\"");
    if (bundleIdPos != std::string::npos) {
        size_t start = bundleIdPos + 12;  // 跳过 "bundleId":"
        size_t end = jsonString.find("\"", start);
        if (end != std::string::npos) {
            std::string bundleId = jsonString.substr(start, end - start);
            result.Set("bundleId", Napi::String::New(env, bundleId));
        }
    }

    // 检查是否有错误
    size_t errorPos = jsonString.find("\"error\":\"");
    if (errorPos != std::string::npos) {
        size_t start = errorPos + 9;
        size_t end = jsonString.find("\"", start);
        if (end != std::string::npos) {
            std::string error = jsonString.substr(start, end - start);
            result.Set("error", Napi::String::New(env, error));
        }
    }

    return result;
}

// 激活指定窗口
Napi::Value ActivateWindow(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();

    if (!LoadSwiftLibrary(env)) {
        return Napi::Boolean::New(env, false);
    }

    if (info.Length() < 1 || !info[0].IsString()) {
        Napi::TypeError::New(env, "Expected a bundleId string").ThrowAsJavaScriptException();
        return Napi::Boolean::New(env, false);
    }

    std::string bundleId = info[0].As<Napi::String>().Utf8Value();
    int success = activateWindowFunc(bundleId.c_str());
    return Napi::Boolean::New(env, success == 1);
}

// 启动窗口监控
Napi::Value StartWindowMonitor(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();

    if (!LoadSwiftLibrary(env)) {
        return env.Undefined();
    }

    if (info.Length() < 1 || !info[0].IsFunction()) {
        Napi::TypeError::New(env, "Expected a callback function").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    if (windowTsfn != nullptr) {
        Napi::Error::New(env, "Window monitor already started").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    // 创建线程安全函数
    napi_value callback = info[0];
    napi_value resource_name;
    napi_create_string_utf8(env, "WindowCallback", NAPI_AUTO_LENGTH, &resource_name);

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
        CallWindowJs,
        &windowTsfn
    );

    // 启动 Swift 窗口监控
    startWindowMonitorFunc(OnWindowChanged);

    return env.Undefined();
}

// 停止窗口监控
Napi::Value StopWindowMonitor(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();

    if (stopWindowMonitorFunc != nullptr) {
        stopWindowMonitorFunc();
    }

    if (windowTsfn != nullptr) {
        napi_release_threadsafe_function(windowTsfn, napi_tsfn_release);
        windowTsfn = nullptr;
    }

    return env.Undefined();
}


// 模块初始化
Napi::Object Init(Napi::Env env, Napi::Object exports) {
    exports.Set("startMonitor", Napi::Function::New(env, StartMonitor));
    exports.Set("stopMonitor", Napi::Function::New(env, StopMonitor));
    exports.Set("startWindowMonitor", Napi::Function::New(env, StartWindowMonitor));
    exports.Set("stopWindowMonitor", Napi::Function::New(env, StopWindowMonitor));
    exports.Set("getActiveWindow", Napi::Function::New(env, GetActiveWindow));
    exports.Set("activateWindow", Napi::Function::New(env, ActivateWindow));
    return exports;
}

NODE_API_MODULE(ztools_native, Init)
