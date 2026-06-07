#include <cstdlib>
#include <dlfcn.h>
#include <napi.h>
#include <string>
#include <vector>
#include <unistd.h>  // For usleep

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
typedef void *(*FetchFileIconFunc)(const char *, size_t *);        // 获取文件图标 PNG
typedef char *(*GetAllFinderWindowsFunc)();                        // 获取所有 Finder 窗口
typedef int (*SetAddressBarFunc)(const char *, const char *);       // 设置 Finder/文件对话框地址

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
static FetchFileIconFunc fetchFileIconFunc = nullptr;
static GetAllFinderWindowsFunc getAllFinderWindowsFunc = nullptr;
static SetAddressBarFunc setAddressBarFunc = nullptr;
static bool g_isPaused = false; // 剪贴板监控暂停状态

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
  if (tsfn != nullptr && !g_isPaused) {
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

Napi::Value ParseJsonValue(Napi::Env env, const std::string &jsonString) {
  Napi::Object json = env.Global().Get("JSON").As<Napi::Object>();
  Napi::Function parse = json.Get("parse").As<Napi::Function>();
  return parse.Call(json, {Napi::String::New(env, jsonString)});
}

// 在主线程调用 JS 回调（窗口监控，带JSON参数）
void CallWindowJs(napi_env env, napi_value js_callback, void *context,
                  void *data) {
  if (env != nullptr && js_callback != nullptr && data != nullptr) {
    char *jsonStr = static_cast<char *>(data);
    Napi::Env napiEnv(env);
    std::string jsonString(jsonStr);
    free(jsonStr);

    napi_value global;
    napi_get_global(env, &global);
    Napi::Value parsed = ParseJsonValue(napiEnv, jsonString);
    napi_value resultValue = parsed;
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
  fetchFileIconFunc = (FetchFileIconFunc)dlsym(swiftLibHandle, "fetchFileIcon");
  getAllFinderWindowsFunc =
      (GetAllFinderWindowsFunc)dlsym(swiftLibHandle, "getAllFinderWindows");
  setAddressBarFunc =
      (SetAddressBarFunc)dlsym(swiftLibHandle, "setAddressBar");

  if (!startMonitorFunc || !stopMonitorFunc || !startWindowMonitorFunc ||
      !stopWindowMonitorFunc || !getActiveWindowFunc || !activateWindowFunc ||
      !simulatePasteFunc || !simulateKeyboardTapFunc || !unicodeTypeFunc ||
      !simulateMouseMoveFunc || !simulateMouseClickFunc ||
      !simulateMouseDoubleClickFunc || !simulateMouseRightClickFunc ||
      !startMouseMonitorFunc || !stopMouseMonitorFunc ||
      !startColorPickerFunc || !stopColorPickerFunc ||
      !setClipboardFilesFunc || !fetchFileIconFunc) {
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

  g_isPaused = false; // 重置暂停状态

  return env.Undefined();
}

// 暂停剪贴板监控
Napi::Value PauseMonitor(const Napi::CallbackInfo &info) {
  Napi::Env env = info.Env();
  g_isPaused = true;
  return env.Undefined();
}

// 恢复剪贴板监控
Napi::Value ResumeMonitor(const Napi::CallbackInfo &info) {
  Napi::Env env = info.Env();
  g_isPaused = false;
  return env.Undefined();
}

// ==================== 获取选中内容（Mac 实现）====================

// 注意：以下函数使用 popen 调用外部命令（pbpaste, osascript）来操作剪贴板
//
// 性能考虑：
// - 这些是低频操作（用户主动触发 getSelectedContent 时才调用）
// - 相比高频的剪贴板监控（已由 Swift 库处理），性能影响可接受
//
// 未来优化方向：
// - 将此文件重命名为 .mm 并使用 Objective-C++ 直接调用 NSPasteboard API
// - 或在 Swift 库中实现这些功能并通过 FFI 调用
//
// 当前实现的优点：
// - 简单可维护，无需额外的 Objective-C++ 编译配置
// - 与 Swift 库保持清晰的职责分离

// 获取剪贴板文本内容
std::string GetPasteboardText() {
  FILE* pipe = popen("pbpaste", "r");
  if (!pipe) return "";

  std::string result;
  char buffer[256];
  while (fgets(buffer, sizeof(buffer), pipe)) {
    result += buffer;
  }
  pclose(pipe);
  return result;
}

// 获取剪贴板文件列表
std::vector<std::string> GetPasteboardFiles() {
  std::vector<std::string> result;

  // 先检查剪贴板是否真的包含文件 URL 类型，避免普通文本被 AppleScript 强制转成伪路径。
  FILE* infoPipe = popen("osascript -e 'clipboard info'", "r");
  if (!infoPipe) return result;

  bool hasFileUrl = false;
  char infoBuffer[1024];
  while (fgets(infoBuffer, sizeof(infoBuffer), infoPipe)) {
    std::string line = infoBuffer;
    if (line.find("«class furl»") != std::string::npos) {
      hasFileUrl = true;
      break;
    }
  }
  pclose(infoPipe);

  if (!hasFileUrl) {
    return result;
  }

  // 使用 osascript 获取文件列表
  FILE* pipe = popen("osascript -e 'try' -e 'set theList to (the clipboard as «class furl») as list' -e 'set output to \"\"' -e 'repeat with aFile in theList' -e 'set output to output & POSIX path of aFile & linefeed' -e 'end repeat' -e 'return output' -e 'end try'", "r");
  if (!pipe) return result;

  char buffer[1024];
  while (fgets(buffer, sizeof(buffer), pipe)) {
    std::string line = buffer;
    // 移除换行符
    if (!line.empty() && line[line.length() - 1] == '\n') {
      line.erase(line.length() - 1);
    }
    if (!line.empty()) {
      result.push_back(line);
    }
  }
  pclose(pipe);
  return result;
}

// 获取剪贴板图像（base64 PNG）
std::string GetPasteboardImage() {
  // 使用临时文件保存图像
  std::string tmpFile = "/tmp/ztools_clipboard_image.png";

  // 使用 osascript 保存剪贴板图像为 PNG
  std::string cmd = "osascript -e 'try' -e 'set imgData to the clipboard as «class PNGf»' -e 'set outFile to open for access POSIX file \"" + tmpFile + "\" with write permission' -e 'set eof outFile to 0' -e 'write imgData to outFile' -e 'close access outFile' -e 'end try'";
  int ret = system(cmd.c_str());

  if (ret != 0) return "";

  // 读取文件并转换为 base64
  FILE* file = fopen(tmpFile.c_str(), "rb");
  if (!file) return "";

  fseek(file, 0, SEEK_END);
  long size = ftell(file);
  fseek(file, 0, SEEK_SET);

  if (size <= 0) {
    fclose(file);
    unlink(tmpFile.c_str());
    return "";
  }

  std::vector<unsigned char> buffer(size);
  fread(buffer.data(), 1, size, file);
  fclose(file);
  unlink(tmpFile.c_str());

  // Base64 编码
  const char* base64_chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string result;
  int idx = 0;
  unsigned char char_array_3[3];
  unsigned char char_array_4[4];

  for (unsigned char c : buffer) {
    char_array_3[idx++] = c;
    if (idx == 3) {
      char_array_4[0] = (char_array_3[0] & 0xfc) >> 2;
      char_array_4[1] = ((char_array_3[0] & 0x03) << 4) + ((char_array_3[1] & 0xf0) >> 4);
      char_array_4[2] = ((char_array_3[1] & 0x0f) << 2) + ((char_array_3[2] & 0xc0) >> 6);
      char_array_4[3] = char_array_3[2] & 0x3f;

      for (int k = 0; k < 4; k++) {
        result += base64_chars[char_array_4[k]];
      }
      idx = 0;
    }
  }

  if (idx) {
    for (int j = idx; j < 3; j++) {
      char_array_3[j] = '\0';
    }

    char_array_4[0] = (char_array_3[0] & 0xfc) >> 2;
    char_array_4[1] = ((char_array_3[0] & 0x03) << 4) + ((char_array_3[1] & 0xf0) >> 4);
    char_array_4[2] = ((char_array_3[1] & 0x0f) << 2) + ((char_array_3[2] & 0xc0) >> 6);

    for (int j = 0; j < idx + 1; j++) {
      result += base64_chars[char_array_4[j]];
    }

    while (idx++ < 3) {
      result += '=';
    }
  }

  return result;
}

// 获取选中内容（Mac 实现 - 使用模拟复制）
Napi::Value GetSelectedContent(const Napi::CallbackInfo &info) {
  Napi::Env env = info.Env();
  Napi::Array result = Napi::Array::New(env);

  if (!LoadSwiftLibrary(env)) {
    return result;
  }

  // 暂停监控以防止触发自身事件
  bool wasMonitoring = (tsfn != nullptr && !g_isPaused);
  if (wasMonitoring) {
    g_isPaused = true;
  }

  // 保存原剪贴板内容
  std::string originalText = GetPasteboardText();
  std::vector<std::string> originalFiles = GetPasteboardFiles();
  std::string originalImage = GetPasteboardImage();

  // 清空剪贴板
  system("pbcopy < /dev/null");

  // 模拟 Cmd+C（使用 simulateKeyboardTap）
  if (simulateKeyboardTapFunc != nullptr) {
    simulateKeyboardTapFunc("c", "meta");

    // 等待剪贴板更新
    usleep(100000); // 100ms

    // 读取新的剪贴板内容
    std::string newText = GetPasteboardText();
    std::vector<std::string> newFiles = GetPasteboardFiles();
    std::string newImage = GetPasteboardImage();

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
  if (!originalText.empty()) {
    // 转义单引号
    std::string escapedText = originalText;
    size_t pos = 0;
    while ((pos = escapedText.find("'", pos)) != std::string::npos) {
      escapedText.replace(pos, 1, "'\\''");
      pos += 4;
    }
    std::string cmd = "printf '%s' '" + escapedText + "' | pbcopy";
    system(cmd.c_str());
  } else if (!originalFiles.empty()) {
    // 恢复文件列表比较复杂，这里简化处理
    // 实际应用中可能需要更完善的实现
  }

  // 恢复监控状态
  if (wasMonitoring) {
    usleep(50000); // 50ms 延迟
    g_isPaused = false;
  }

  return result;
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

  std::string jsonString(jsonStr);
  free(jsonStr);
  return ParseJsonValue(env, jsonString);
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

class FileIconWorker : public Napi::AsyncWorker {
public:
  FileIconWorker(const std::string &input, Napi::Env env,
                 Napi::Promise::Deferred deferred)
      : Napi::AsyncWorker(env), input_(input), deferred_(deferred) {}

  ~FileIconWorker() override {
    if (data_ != nullptr) {
      free(data_);
      data_ = nullptr;
    }
  }

  void Execute() override {
    if (fetchFileIconFunc == nullptr) {
      SetError("fetchFileIcon is not available");
      return;
    }

    size_t length = 0;
    void *result = fetchFileIconFunc(input_.c_str(), &length);
    if (result == nullptr || length == 0) {
      SetError("get file icon failed");
      return;
    }

    data_ = result;
    length_ = length;
  }

  void OnOK() override {
    Napi::Buffer<char> buffer = Napi::Buffer<char>::Copy(
        Env(), reinterpret_cast<const char *>(data_), length_);
    deferred_.Resolve(buffer);
  }

  void OnError(const Napi::Error &e) override { deferred_.Reject(e.Value()); }

private:
  std::string input_;
  void *data_ = nullptr;
  size_t length_ = 0;
  Napi::Promise::Deferred deferred_;
};

Napi::Value GetFileIcon(const Napi::CallbackInfo &info) {
  Napi::Env env = info.Env();

  if (!LoadSwiftLibrary(env)) {
    return env.Undefined();
  }

  if (info.Length() < 1 || !info[0].IsString()) {
    Napi::TypeError::New(env, "Expected file path or type as first argument (string)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  std::string input = info[0].As<Napi::String>().Utf8Value();
  if (input.empty()) {
    Napi::TypeError::New(env, "Expected non-empty file path or type")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  auto deferred = Napi::Promise::Deferred::New(env);
  auto *worker = new FileIconWorker(input, env, deferred);
  worker->Queue();
  return deferred.Promise();
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

/**
 * 获取所有打开的 Finder 窗口的结构化信息。
 */
Napi::Value GetAllExplorerWindows(const Napi::CallbackInfo &info) {
  Napi::Env env = info.Env();

  if (!LoadSwiftLibrary(env)) {
    return env.Undefined();
  }

  if (getAllFinderWindowsFunc == nullptr) {
    Napi::Error::New(env, "getAllFinderWindows is not available")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  char *jsonResult = getAllFinderWindowsFunc();
  if (jsonResult == nullptr) {
    return Napi::Array::New(env, 0);
  }

  std::string jsonStr(jsonResult);
  free(jsonResult);
  return ParseJsonValue(env, jsonStr);
}

/**
 * 设置 Finder 或文件选择对话框等文件定位窗口的地址。
 *
 * 第一个参数接受 bundleId 字符串或 pid 数字，C++ 层统一转为字符串传给 Swift；
 * Swift 会限制目标为 Finder 或常见文件选择对话框所属应用，避免修改普通浏览器地址栏。
 *
 * @returns boolean - 地址设置成功返回 true，目标不支持或系统权限不足返回 false
 */
Napi::Value SetAddressBar(const Napi::CallbackInfo &info) {
  Napi::Env env = info.Env();

  if (!LoadSwiftLibrary(env)) {
    return Napi::Boolean::New(env, false);
  }

  if (setAddressBarFunc == nullptr) {
    Napi::Error::New(env, "setAddressBar is not available")
        .ThrowAsJavaScriptException();
    return Napi::Boolean::New(env, false);
  }

  if (info.Length() < 2 || (!info[0].IsString() && !info[0].IsNumber() && !info[0].IsObject()) || !info[1].IsString()) {
    Napi::TypeError::New(env, "target (object, bundleId or pid) and address (string) are required")
        .ThrowAsJavaScriptException();
    return Napi::Boolean::New(env, false);
  }

  std::string target;
  if (info[0].IsString()) {
    target = info[0].As<Napi::String>().Utf8Value();
  } else if (info[0].IsNumber()) {
    target = std::to_string(info[0].As<Napi::Number>().Int64Value());
  } else {
    Napi::Object json = env.Global().Get("JSON").As<Napi::Object>();
    Napi::Function stringify = json.Get("stringify").As<Napi::Function>();
    target = stringify.Call(json, {info[0]}).As<Napi::String>().Utf8Value();
  }

  std::string address = info[1].As<Napi::String>().Utf8Value();
  if (target.empty() || address.empty()) {
    return Napi::Boolean::New(env, false);
  }

  int success = setAddressBarFunc(target.c_str(), address.c_str());
  return Napi::Boolean::New(env, success == 1);
}

// 模块初始化
Napi::Object Init(Napi::Env env, Napi::Object exports) {
  exports.Set("startMonitor", Napi::Function::New(env, StartMonitor));
  exports.Set("stopMonitor", Napi::Function::New(env, StopMonitor));
  exports.Set("pauseMonitor", Napi::Function::New(env, PauseMonitor));
  exports.Set("resumeMonitor", Napi::Function::New(env, ResumeMonitor));
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
  exports.Set("getFileIcon", Napi::Function::New(env, GetFileIcon));
  exports.Set("getAllExplorerWindows", Napi::Function::New(env, GetAllExplorerWindows));
  exports.Set("setAddressBar", Napi::Function::New(env, SetAddressBar));
  exports.Set("getSelectedContent", Napi::Function::New(env, GetSelectedContent));
  return exports;
}

NODE_API_MODULE(ztools_native, Init)
