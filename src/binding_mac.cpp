#include <cstdlib>
#include <dlfcn.h>
#include <napi.h>
#include <string>
#include <vector>

// Swift 动态库函数类型定义
typedef void (*ClipboardCallback)();          // 无参数回调
typedef void (*WindowCallback)(const char *); // 带JSON字符串参数回调
typedef void (*StartMonitorFunc)(ClipboardCallback);
typedef void (*StopMonitorFunc)();
typedef void (*StartWindowMonitorFunc)(WindowCallback);
typedef void (*StopWindowMonitorFunc)();
typedef char *(*GetActiveWindowFunc)();
typedef int (*ActivateWindowFunc)(const char *);
typedef int (*SimulatePasteFunc)(); // 模拟粘贴功能
typedef int (*SimulateKeyboardTapFunc)(const char *,
                                       const char *); // 模拟键盘按键功能
typedef int (*UnicodeTypeFunc)(const char *);              // Unicode 字符输入
typedef int (*SetClipboardFilesFunc)(const char *);        // 设置剪贴板文件
typedef void (*MouseEventCB)(const char *);                       // 鼠标事件回调
typedef void (*StartMouseMonitorFunc)(const char *, int, MouseEventCB); // 启动鼠标监控
typedef void (*StopMouseMonitorFunc)();                            // 停止鼠标监控
typedef void (*ReplayMouseEventsFunc)();                           // 重放鼠标事件
typedef int (*SimulateMouseMoveFunc)(double, double);              // 模拟鼠标移动
typedef int (*SimulateMouseClickFunc)(double, double);             // 模拟鼠标单击
typedef int (*SimulateMouseDoubleClickFunc)(double, double);       // 模拟鼠标双击
typedef int (*SimulateMouseRightClickFunc)(double, double);        // 模拟鼠标右击
typedef void (*ColorPickerCB)(const char *);                       // 取色器回调
typedef void (*StartColorPickerFunc)(ColorPickerCB);               // 启动取色器
typedef void (*StopColorPickerFunc)();                             // 停止取色器

// 全局变量
static void *swiftLibHandle = nullptr;
static napi_threadsafe_function tsfn = nullptr;
static napi_threadsafe_function windowTsfn = nullptr;
static StartMonitorFunc startMonitorFunc = nullptr;
static StopMonitorFunc stopMonitorFunc = nullptr;
static StartWindowMonitorFunc startWindowMonitorFunc = nullptr;
static StopWindowMonitorFunc stopWindowMonitorFunc = nullptr;
static GetActiveWindowFunc getActiveWindowFunc = nullptr;
static ActivateWindowFunc activateWindowFunc = nullptr;
static SimulatePasteFunc simulatePasteFunc = nullptr; // 模拟粘贴函数
static SimulateKeyboardTapFunc simulateKeyboardTapFunc =
    nullptr; // 模拟键盘按键函数
static UnicodeTypeFunc unicodeTypeFunc = nullptr; // Unicode 字符输入函数
static SetClipboardFilesFunc setClipboardFilesFunc = nullptr; // 设置剪贴板文件函数
static napi_threadsafe_function mouseTsfn = nullptr;
static StartMouseMonitorFunc startMouseMonitorFunc = nullptr;
static StopMouseMonitorFunc stopMouseMonitorFunc = nullptr;
static ReplayMouseEventsFunc replayMouseEventsFunc = nullptr;
static SimulateMouseMoveFunc simulateMouseMoveFunc = nullptr;
static SimulateMouseClickFunc simulateMouseClickFunc = nullptr;
static SimulateMouseDoubleClickFunc simulateMouseDoubleClickFunc = nullptr;
static SimulateMouseRightClickFunc simulateMouseRightClickFunc = nullptr;
static napi_threadsafe_function colorPickerTsfn = nullptr;
static StartColorPickerFunc startColorPickerFunc = nullptr;
static StopColorPickerFunc stopColorPickerFunc = nullptr;

// 在主线程调用 JS 回调
void CallJs(napi_env env, napi_value js_callback, void *context, void *data) {
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

// 辅助函数：从JSON字符串中解析数字值
int parseJsonNumber(const std::string &jsonString, const std::string &key) {
  std::string searchKey = "\"" + key + "\":";
  size_t pos = jsonString.find(searchKey);
  if (pos != std::string::npos) {
    size_t start = pos + searchKey.length();
    size_t end = start;
    // 查找数字的结束位置（遇到逗号、大括号或引号）
    while (end < jsonString.length() && jsonString[end] != ',' &&
           jsonString[end] != '}' && jsonString[end] != '"') {
      end++;
    }
    if (end > start) {
      try {
        return std::stoi(jsonString.substr(start, end - start));
      } catch (...) {
        return 0;
      }
    }
  }
  return 0;
}

// 在主线程调用 JS 回调（窗口监控，带JSON参数）
void CallWindowJs(napi_env env, napi_value js_callback, void *context,
                  void *data) {
  if (env != nullptr && js_callback != nullptr && data != nullptr) {
    char *jsonStr = static_cast<char *>(data);

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

    // 查找 "title":"xxx"
    size_t titlePos = jsonString.find("\"title\":\"");
    if (titlePos != std::string::npos) {
      size_t start = titlePos + 9;
      size_t end = jsonString.find("\"", start);
      if (end != std::string::npos) {
        std::string title = jsonString.substr(start, end - start);
        result.Set("title", Napi::String::New(napiEnv, title));
      }
    }

    // 查找 "app":"xxx"
    size_t appPos = jsonString.find("\"app\":\"");
    if (appPos != std::string::npos) {
      size_t start = appPos + 7;
      size_t end = jsonString.find("\"", start);
      if (end != std::string::npos) {
        std::string app = jsonString.substr(start, end - start);
        result.Set("app", Napi::String::New(napiEnv, app));
      }
    }

    // 解析数字字段
    result.Set("x",
               Napi::Number::New(napiEnv, parseJsonNumber(jsonString, "x")));
    result.Set("y",
               Napi::Number::New(napiEnv, parseJsonNumber(jsonString, "y")));
    result.Set("width", Napi::Number::New(
                            napiEnv, parseJsonNumber(jsonString, "width")));
    result.Set("height", Napi::Number::New(
                             napiEnv, parseJsonNumber(jsonString, "height")));
    result.Set("pid",
               Napi::Number::New(napiEnv, parseJsonNumber(jsonString, "pid")));

    // 查找 "appPath":"xxx"
    size_t appPathPos = jsonString.find("\"appPath\":\"");
    if (appPathPos != std::string::npos) {
      size_t start = appPathPos + 11;
      size_t end = jsonString.find("\"", start);
      if (end != std::string::npos) {
        std::string appPath = jsonString.substr(start, end - start);
        result.Set("appPath", Napi::String::New(napiEnv, appPath));
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
void OnWindowChanged(const char *jsonStr) {
  if (windowTsfn != nullptr && jsonStr != nullptr) {
    // 复制字符串
    char *jsonCopy = strdup(jsonStr);
    napi_call_threadsafe_function(windowTsfn, jsonCopy, napi_tsfn_nonblocking);
  }
}

// 获取当前 .node 文件所在目录
std::string GetModuleDirectory() {
  Dl_info info;
  // 获取当前函数的地址，从而定位到 .node 文件
  if (dladdr((void *)GetModuleDirectory, &info) && info.dli_fname) {
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
    return true; // 已加载
  }

  std::string moduleDir = GetModuleDirectory();

  // 尝试多个路径（优先级从高到低）
  std::vector<std::string> paths = {// 1. .node 文件同目录（最常见的部署方式）
                                    moduleDir + "/libZToolsNative.dylib",
                                    // 2. .node 文件的上级 lib 目录
                                    moduleDir + "/../lib/libZToolsNative.dylib",
                                    // 3. 当前工作目录的 lib 子目录（开发环境）
                                    "./lib/libZToolsNative.dylib",
                                    // 4. 当前工作目录（开发环境备选）
                                    "./libZToolsNative.dylib",
                                    // 5. 相对路径备选
                                    "../lib/libZToolsNative.dylib"};

  std::string lastError;
  for (const auto &path : paths) {
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
    for (const auto &path : paths) {
      errorMsg += "  - " + path + "\n";
    }
    errorMsg += "Last error: " + lastError;

    Napi::Error::New(env, errorMsg).ThrowAsJavaScriptException();
    return false;
  }

  // 加载函数符号
  startMonitorFunc =
      (StartMonitorFunc)dlsym(swiftLibHandle, "startClipboardMonitor");
  stopMonitorFunc =
      (StopMonitorFunc)dlsym(swiftLibHandle, "stopClipboardMonitor");
  startWindowMonitorFunc =
      (StartWindowMonitorFunc)dlsym(swiftLibHandle, "startWindowMonitor");
  stopWindowMonitorFunc =
      (StopWindowMonitorFunc)dlsym(swiftLibHandle, "stopWindowMonitor");
  getActiveWindowFunc =
      (GetActiveWindowFunc)dlsym(swiftLibHandle, "getActiveWindow");
  activateWindowFunc =
      (ActivateWindowFunc)dlsym(swiftLibHandle, "activateWindow");
  simulatePasteFunc = (SimulatePasteFunc)dlsym(swiftLibHandle, "simulatePaste");
  simulateKeyboardTapFunc =
      (SimulateKeyboardTapFunc)dlsym(swiftLibHandle, "simulateKeyboardTap");
  unicodeTypeFunc =
      (UnicodeTypeFunc)dlsym(swiftLibHandle, "unicodeType");
  setClipboardFilesFunc =
      (SetClipboardFilesFunc)dlsym(swiftLibHandle, "setClipboardFiles");
  startMouseMonitorFunc =
      (StartMouseMonitorFunc)dlsym(swiftLibHandle, "startMouseMonitor");
  stopMouseMonitorFunc =
      (StopMouseMonitorFunc)dlsym(swiftLibHandle, "stopMouseMonitor");
  replayMouseEventsFunc =
      (ReplayMouseEventsFunc)dlsym(swiftLibHandle, "replayMouseEvents");
  simulateMouseMoveFunc =
      (SimulateMouseMoveFunc)dlsym(swiftLibHandle, "simulateMouseMove");
  simulateMouseClickFunc =
      (SimulateMouseClickFunc)dlsym(swiftLibHandle, "simulateMouseClick");
  simulateMouseDoubleClickFunc =
      (SimulateMouseDoubleClickFunc)dlsym(swiftLibHandle, "simulateMouseDoubleClick");
  simulateMouseRightClickFunc =
      (SimulateMouseRightClickFunc)dlsym(swiftLibHandle, "simulateMouseRightClick");
  startColorPickerFunc =
      (StartColorPickerFunc)dlsym(swiftLibHandle, "startColorPicker");
  stopColorPickerFunc =
      (StopColorPickerFunc)dlsym(swiftLibHandle, "stopColorPicker");

  if (!startMonitorFunc || !stopMonitorFunc || !startWindowMonitorFunc ||
      !stopWindowMonitorFunc || !getActiveWindowFunc || !activateWindowFunc ||
      !simulatePasteFunc || !simulateKeyboardTapFunc || !unicodeTypeFunc ||
      !simulateMouseMoveFunc || !simulateMouseClickFunc ||
      !simulateMouseDoubleClickFunc || !simulateMouseRightClickFunc ||
      !startMouseMonitorFunc || !stopMouseMonitorFunc ||
      !startColorPickerFunc || !stopColorPickerFunc ||
      !setClipboardFilesFunc) {
    Napi::Error::New(env, "Failed to load Swift functions")
        .ThrowAsJavaScriptException();
    dlclose(swiftLibHandle);
    swiftLibHandle = nullptr;
    return false;
  }

  return true;
}

// 启动监控
Napi::Value StartMonitor(const Napi::CallbackInfo &info) {
  Napi::Env env = info.Env();

  if (!LoadSwiftLibrary(env)) {
    return env.Undefined();
  }

  if (info.Length() < 1 || !info[0].IsFunction()) {
    Napi::TypeError::New(env, "Expected a callback function")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  if (tsfn != nullptr) {
    Napi::Error::New(env, "Monitor already started")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  // 创建线程安全函数
  napi_value callback = info[0];
  napi_value resource_name;
  napi_create_string_utf8(env, "ClipboardCallback", NAPI_AUTO_LENGTH,
                          &resource_name);

  napi_create_threadsafe_function(env, callback, nullptr, resource_name, 0, 1,
                                  nullptr, nullptr, nullptr, CallJs, &tsfn);

  // 启动 Swift 监控
  startMonitorFunc(OnClipboardChanged);

  return env.Undefined();
}

// 停止监控
Napi::Value StopMonitor(const Napi::CallbackInfo &info) {
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
Napi::Value GetActiveWindow(const Napi::CallbackInfo &info) {
  Napi::Env env = info.Env();

  if (!LoadSwiftLibrary(env)) {
    return env.Null();
  }

  char *jsonStr = getActiveWindowFunc();
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
    size_t start = appNamePos + 11; // 跳过 "appName":"
    size_t end = jsonString.find("\"", start);
    if (end != std::string::npos) {
      std::string appName = jsonString.substr(start, end - start);
      result.Set("appName", Napi::String::New(env, appName));
    }
  }

  // 查找 "bundleId":"xxx"
  size_t bundleIdPos = jsonString.find("\"bundleId\":\"");
  if (bundleIdPos != std::string::npos) {
    size_t start = bundleIdPos + 12; // 跳过 "bundleId":"
    size_t end = jsonString.find("\"", start);
    if (end != std::string::npos) {
      std::string bundleId = jsonString.substr(start, end - start);
      result.Set("bundleId", Napi::String::New(env, bundleId));
    }
  }

  // 查找 "title":"xxx"
  size_t titlePos = jsonString.find("\"title\":\"");
  if (titlePos != std::string::npos) {
    size_t start = titlePos + 9; // 跳过 "title":"
    size_t end = jsonString.find("\"", start);
    if (end != std::string::npos) {
      std::string title = jsonString.substr(start, end - start);
      result.Set("title", Napi::String::New(env, title));
    }
  }

  // 查找 "app":"xxx"
  size_t appPos = jsonString.find("\"app\":\"");
  if (appPos != std::string::npos) {
    size_t start = appPos + 7; // 跳过 "app":"
    size_t end = jsonString.find("\"", start);
    if (end != std::string::npos) {
      std::string app = jsonString.substr(start, end - start);
      result.Set("app", Napi::String::New(env, app));
    }
  }

  // 解析数字字段
  result.Set("x", Napi::Number::New(env, parseJsonNumber(jsonString, "x")));
  result.Set("y", Napi::Number::New(env, parseJsonNumber(jsonString, "y")));
  result.Set("width",
             Napi::Number::New(env, parseJsonNumber(jsonString, "width")));
  result.Set("height",
             Napi::Number::New(env, parseJsonNumber(jsonString, "height")));
  result.Set("pid", Napi::Number::New(env, parseJsonNumber(jsonString, "pid")));

  // 查找 "appPath":"xxx"
  size_t appPathPos = jsonString.find("\"appPath\":\"");
  if (appPathPos != std::string::npos) {
    size_t start = appPathPos + 11; // 跳过 "appPath":"
    size_t end = jsonString.find("\"", start);
    if (end != std::string::npos) {
      std::string appPath = jsonString.substr(start, end - start);
      result.Set("appPath", Napi::String::New(env, appPath));
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
Napi::Value ActivateWindow(const Napi::CallbackInfo &info) {
  Napi::Env env = info.Env();

  if (!LoadSwiftLibrary(env)) {
    return Napi::Boolean::New(env, false);
  }

  if (info.Length() < 1 || !info[0].IsString()) {
    Napi::TypeError::New(env, "Expected a bundleId string")
        .ThrowAsJavaScriptException();
    return Napi::Boolean::New(env, false);
  }

  std::string bundleId = info[0].As<Napi::String>().Utf8Value();
  int success = activateWindowFunc(bundleId.c_str());
  return Napi::Boolean::New(env, success == 1);
}

// 启动窗口监控
Napi::Value StartWindowMonitor(const Napi::CallbackInfo &info) {
  Napi::Env env = info.Env();

  if (!LoadSwiftLibrary(env)) {
    return env.Undefined();
  }

  if (info.Length() < 1 || !info[0].IsFunction()) {
    Napi::TypeError::New(env, "Expected a callback function")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  if (windowTsfn != nullptr) {
    Napi::Error::New(env, "Window monitor already started")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  // 创建线程安全函数
  napi_value callback = info[0];
  napi_value resource_name;
  napi_create_string_utf8(env, "WindowCallback", NAPI_AUTO_LENGTH,
                          &resource_name);

  napi_create_threadsafe_function(env, callback, nullptr, resource_name, 0, 1,
                                  nullptr, nullptr, nullptr, CallWindowJs,
                                  &windowTsfn);

  // 启动 Swift 窗口监控
  startWindowMonitorFunc(OnWindowChanged);

  return env.Undefined();
}

// 停止窗口监控
Napi::Value StopWindowMonitor(const Napi::CallbackInfo &info) {
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

// 模拟粘贴
Napi::Value SimulatePaste(const Napi::CallbackInfo &info) {
  Napi::Env env = info.Env();

  if (!LoadSwiftLibrary(env)) {
    return Napi::Boolean::New(env, false);
  }

  int success = simulatePasteFunc();
  return Napi::Boolean::New(env, success == 1);
}

// 模拟键盘按键
Napi::Value SimulateKeyboardTap(const Napi::CallbackInfo &info) {
  Napi::Env env = info.Env();

  if (!LoadSwiftLibrary(env)) {
    return Napi::Boolean::New(env, false);
  }

  // 参数1：key（必需）
  if (info.Length() < 1 || !info[0].IsString()) {
    Napi::TypeError::New(env, "Expected key as first argument (string)")
        .ThrowAsJavaScriptException();
    return Napi::Boolean::New(env, false);
  }

  std::string key = info[0].As<Napi::String>().Utf8Value();

  // 参数2+：modifiers（可选）
  std::string modifiers = "";
  if (info.Length() > 1) {
    // 收集所有修饰键参数
    std::vector<std::string> modifierList;
    for (size_t i = 1; i < info.Length(); i++) {
      if (info[i].IsString()) {
        modifierList.push_back(info[i].As<Napi::String>().Utf8Value());
      }
    }

    // 用逗号连接
    if (!modifierList.empty()) {
      for (size_t i = 0; i < modifierList.size(); i++) {
        if (i > 0)
          modifiers += ",";
        modifiers += modifierList[i];
      }
    }
  }

  const char *modifiersPtr = modifiers.empty() ? nullptr : modifiers.c_str();
  int success = simulateKeyboardTapFunc(key.c_str(), modifiersPtr);
  return Napi::Boolean::New(env, success == 1);
}

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
  if (!shouldBlock && replayMouseEventsFunc != nullptr) {
    replayMouseEventsFunc();
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

// 模拟鼠标移动
Napi::Value SimulateMouseMove(const Napi::CallbackInfo &info) {
  Napi::Env env = info.Env();

  if (!LoadSwiftLibrary(env)) {
    return Napi::Boolean::New(env, false);
  }

  if (info.Length() < 2 || !info[0].IsNumber() || !info[1].IsNumber()) {
    Napi::TypeError::New(env, "Expected x and y as number arguments")
        .ThrowAsJavaScriptException();
    return Napi::Boolean::New(env, false);
  }

  double x = info[0].As<Napi::Number>().DoubleValue();
  double y = info[1].As<Napi::Number>().DoubleValue();
  int success = simulateMouseMoveFunc(x, y);
  return Napi::Boolean::New(env, success == 1);
}

// 模拟鼠标左键单击
Napi::Value SimulateMouseClick(const Napi::CallbackInfo &info) {
  Napi::Env env = info.Env();

  if (!LoadSwiftLibrary(env)) {
    return Napi::Boolean::New(env, false);
  }

  if (info.Length() < 2 || !info[0].IsNumber() || !info[1].IsNumber()) {
    Napi::TypeError::New(env, "Expected x and y as number arguments")
        .ThrowAsJavaScriptException();
    return Napi::Boolean::New(env, false);
  }

  double x = info[0].As<Napi::Number>().DoubleValue();
  double y = info[1].As<Napi::Number>().DoubleValue();
  int success = simulateMouseClickFunc(x, y);
  return Napi::Boolean::New(env, success == 1);
}

// 模拟鼠标左键双击
Napi::Value SimulateMouseDoubleClick(const Napi::CallbackInfo &info) {
  Napi::Env env = info.Env();

  if (!LoadSwiftLibrary(env)) {
    return Napi::Boolean::New(env, false);
  }

  if (info.Length() < 2 || !info[0].IsNumber() || !info[1].IsNumber()) {
    Napi::TypeError::New(env, "Expected x and y as number arguments")
        .ThrowAsJavaScriptException();
    return Napi::Boolean::New(env, false);
  }

  double x = info[0].As<Napi::Number>().DoubleValue();
  double y = info[1].As<Napi::Number>().DoubleValue();
  int success = simulateMouseDoubleClickFunc(x, y);
  return Napi::Boolean::New(env, success == 1);
}

// 模拟鼠标右键单击
Napi::Value SimulateMouseRightClick(const Napi::CallbackInfo &info) {
  Napi::Env env = info.Env();

  if (!LoadSwiftLibrary(env)) {
    return Napi::Boolean::New(env, false);
  }

  if (info.Length() < 2 || !info[0].IsNumber() || !info[1].IsNumber()) {
    Napi::TypeError::New(env, "Expected x and y as number arguments")
        .ThrowAsJavaScriptException();
    return Napi::Boolean::New(env, false);
  }

  double x = info[0].As<Napi::Number>().DoubleValue();
  double y = info[1].As<Napi::Number>().DoubleValue();
  int success = simulateMouseRightClickFunc(x, y);
  return Napi::Boolean::New(env, success == 1);
}

// 在主线程调用 JS 回调（鼠标事件）
void CallMouseJs(napi_env env, napi_value js_callback, void *context,
                 void *data) {
  if (env != nullptr && js_callback != nullptr && data != nullptr) {
    char *eventType = static_cast<char *>(data);
    Napi::Env napiEnv(env);
    Napi::Object callbackArg = Napi::Object::New(napiEnv);
    callbackArg.Set("type", Napi::String::New(napiEnv, eventType));
    free(eventType);

    napi_value global;
    napi_get_global(env, &global);
    napi_value argValue = callbackArg;
    napi_value result;
    napi_status status = napi_call_function(env, global, js_callback, 1, &argValue, &result);

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

// Swift 鼠标回调 -> 推送到线程安全队列
void OnMouseEvent(const char *eventType) {
  if (mouseTsfn != nullptr && eventType != nullptr) {
    char *copy = strdup(eventType);
    napi_call_threadsafe_function(mouseTsfn, copy, napi_tsfn_nonblocking);
  }
}

// 启动鼠标监控
Napi::Value StartMouseMonitor(const Napi::CallbackInfo &info) {
  Napi::Env env = info.Env();

  if (!LoadSwiftLibrary(env)) {
    return env.Undefined();
  }

  // 参数1：buttonType（字符串）
  if (info.Length() < 1 || !info[0].IsString()) {
    Napi::TypeError::New(
        env, "Expected buttonType as first argument (string)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  // 参数2：longPressMs（数字）
  if (info.Length() < 2 || !info[1].IsNumber()) {
    Napi::TypeError::New(env,
                         "Expected longPressMs as second argument (number)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  // 参数3：callback（函数）
  if (info.Length() < 3 || !info[2].IsFunction()) {
    Napi::TypeError::New(env,
                         "Expected callback function as third argument")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  if (mouseTsfn != nullptr) {
    Napi::Error::New(env, "Mouse monitor already started")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  std::string buttonType = info[0].As<Napi::String>().Utf8Value();
  int longPressMs = info[1].As<Napi::Number>().Int32Value();

  napi_value callback = info[2];
  napi_value resource_name;
  napi_create_string_utf8(env, "MouseCallback", NAPI_AUTO_LENGTH,
                          &resource_name);

  napi_create_threadsafe_function(env, callback, nullptr, resource_name, 0, 1,
                                  nullptr, nullptr, nullptr, CallMouseJs,
                                  &mouseTsfn);

  startMouseMonitorFunc(buttonType.c_str(), longPressMs, OnMouseEvent);

  return env.Undefined();
}

// 停止鼠标监控
Napi::Value StopMouseMonitor(const Napi::CallbackInfo &info) {
  Napi::Env env = info.Env();

  if (stopMouseMonitorFunc != nullptr) {
    stopMouseMonitorFunc();
  }

  if (mouseTsfn != nullptr) {
    napi_release_threadsafe_function(mouseTsfn, napi_tsfn_release);
    mouseTsfn = nullptr;
  }

  return env.Undefined();
}

// 在主线程调用 JS 回调（取色器结果）
void CallColorPickerJs(napi_env env, napi_value js_callback, void *context,
                       void *data) {
  if (env != nullptr && js_callback != nullptr && data != nullptr) {
    char *jsonStr = static_cast<char *>(data);
    Napi::Env napiEnv(env);
    Napi::Object result = Napi::Object::New(napiEnv);

    std::string jsonString(jsonStr);
    free(jsonStr);

    // 解析 "success":true/false
    if (jsonString.find("\"success\":true") != std::string::npos) {
      result.Set("success", Napi::Boolean::New(napiEnv, true));
    } else {
      result.Set("success", Napi::Boolean::New(napiEnv, false));
    }

    // 解析 "hex":"#XXXXXX"
    size_t hexPos = jsonString.find("\"hex\":\"");
    if (hexPos != std::string::npos) {
      size_t start = hexPos + 7;
      size_t end = jsonString.find("\"", start);
      if (end != std::string::npos) {
        std::string hex = jsonString.substr(start, end - start);
        result.Set("hex", Napi::String::New(napiEnv, hex));
      }
    } else {
      result.Set("hex", napiEnv.Null());
    }

    napi_value global;
    napi_get_global(env, &global);
    napi_value resultValue = result;
    napi_call_function(env, global, js_callback, 1, &resultValue, nullptr);
  }
}

// Swift 取色器回调 -> 推送到线程安全队列
void OnColorPicked(const char *jsonStr) {
  if (colorPickerTsfn != nullptr && jsonStr != nullptr) {
    char *jsonCopy = strdup(jsonStr);
    napi_call_threadsafe_function(colorPickerTsfn, jsonCopy,
                                  napi_tsfn_nonblocking);
  }
}

// 启动取色器
Napi::Value StartColorPicker(const Napi::CallbackInfo &info) {
  Napi::Env env = info.Env();

  if (!LoadSwiftLibrary(env)) {
    return env.Undefined();
  }

  if (info.Length() < 1 || !info[0].IsFunction()) {
    Napi::TypeError::New(env, "Expected a callback function")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  if (colorPickerTsfn != nullptr) {
    Napi::Error::New(env, "Color picker already active")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  napi_value callback = info[0];
  napi_value resource_name;
  napi_create_string_utf8(env, "ColorPickerCallback", NAPI_AUTO_LENGTH,
                          &resource_name);

  napi_create_threadsafe_function(env, callback, nullptr, resource_name, 0, 1,
                                  nullptr, nullptr, nullptr, CallColorPickerJs,
                                  &colorPickerTsfn);

  startColorPickerFunc(OnColorPicked);

  return env.Undefined();
}

// 设置剪贴板文件列表
Napi::Value SetClipboardFiles(const Napi::CallbackInfo &info) {
  Napi::Env env = info.Env();

  if (!LoadSwiftLibrary(env)) {
    return Napi::Boolean::New(env, false);
  }

  if (info.Length() < 1 || !info[0].IsArray()) {
    Napi::TypeError::New(env, "Expected array of file paths as first argument")
        .ThrowAsJavaScriptException();
    return Napi::Boolean::New(env, false);
  }

  Napi::Array arr = info[0].As<Napi::Array>();
  if (arr.Length() == 0) {
    return Napi::Boolean::New(env, false);
  }

  // 将路径数组拼接为换行符分隔的字符串
  std::string paths;
  for (uint32_t i = 0; i < arr.Length(); i++) {
    Napi::Value item = arr.Get(i);
    std::string p;
    if (item.IsString()) {
      p = item.As<Napi::String>().Utf8Value();
    } else if (item.IsObject()) {
      Napi::Object obj = item.As<Napi::Object>();
      if (obj.Has("path") && obj.Get("path").IsString()) {
        p = obj.Get("path").As<Napi::String>().Utf8Value();
      }
    }
    if (!p.empty()) {
      if (!paths.empty()) paths += "\n";
      paths += p;
    }
  }

  if (paths.empty()) {
    return Napi::Boolean::New(env, false);
  }

  int success = setClipboardFilesFunc(paths.c_str());
  return Napi::Boolean::New(env, success == 1);
}

// Unicode 字符输入
Napi::Value UnicodeType(const Napi::CallbackInfo &info) {
  Napi::Env env = info.Env();

  if (!LoadSwiftLibrary(env)) {
    return Napi::Boolean::New(env, false);
  }

  if (info.Length() < 1 || !info[0].IsString()) {
    Napi::TypeError::New(env, "Expected text as first argument (string)")
        .ThrowAsJavaScriptException();
    return Napi::Boolean::New(env, false);
  }

  std::string text = info[0].As<Napi::String>().Utf8Value();
  int success = unicodeTypeFunc(text.c_str());
  return Napi::Boolean::New(env, success == 1);
}

// 停止取色器
Napi::Value StopColorPicker(const Napi::CallbackInfo &info) {
  Napi::Env env = info.Env();

  if (stopColorPickerFunc != nullptr) {
    stopColorPickerFunc();
  }

  if (colorPickerTsfn != nullptr) {
    napi_release_threadsafe_function(colorPickerTsfn, napi_tsfn_release);
    colorPickerTsfn = nullptr;
  }

  return env.Undefined();
}

// 模块初始化
Napi::Object Init(Napi::Env env, Napi::Object exports) {
  exports.Set("startMonitor", Napi::Function::New(env, StartMonitor));
  exports.Set("stopMonitor", Napi::Function::New(env, StopMonitor));
  exports.Set("startWindowMonitor",
              Napi::Function::New(env, StartWindowMonitor));
  exports.Set("stopWindowMonitor", Napi::Function::New(env, StopWindowMonitor));
  exports.Set("getActiveWindow", Napi::Function::New(env, GetActiveWindow));
  exports.Set("activateWindow", Napi::Function::New(env, ActivateWindow));
  exports.Set("simulatePaste", Napi::Function::New(env, SimulatePaste));
  exports.Set("simulateKeyboardTap",
              Napi::Function::New(env, SimulateKeyboardTap));
  exports.Set("simulateMouseMove",
              Napi::Function::New(env, SimulateMouseMove));
  exports.Set("simulateMouseClick",
              Napi::Function::New(env, SimulateMouseClick));
  exports.Set("simulateMouseDoubleClick",
              Napi::Function::New(env, SimulateMouseDoubleClick));
  exports.Set("simulateMouseRightClick",
              Napi::Function::New(env, SimulateMouseRightClick));
  exports.Set("startMouseMonitor",
              Napi::Function::New(env, StartMouseMonitor));
  exports.Set("stopMouseMonitor", Napi::Function::New(env, StopMouseMonitor));
  exports.Set("startColorPicker",
              Napi::Function::New(env, StartColorPicker));
  exports.Set("stopColorPicker", Napi::Function::New(env, StopColorPicker));
  exports.Set("unicodeType", Napi::Function::New(env, UnicodeType));
  exports.Set("setClipboardFiles", Napi::Function::New(env, SetClipboardFiles));
  return exports;
}

NODE_API_MODULE(ztools_native, Init)
