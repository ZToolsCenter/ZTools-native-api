// ZTools 日志模块的 N-API 绑定层（header-only，Windows / macOS 绑定共用）
//
// 把 src/logger.h 的能力暴露给 JS 侧：
// - setLogLevel(level)        运行时设置输出等级，覆盖环境变量
// - getLogLevel()             查询当前输出等级
// - getLogFilePath()          查询日志文件完整路径（宿主可展示给用户）
// - isLogLevelEnabled(level)  查询某等级当前是否会被写入
// - logWrite(level, tag, msg) JS 侧向同一日志文件写入一条日志
//                             （与原生日志同一文件/格式，得到统一时间线）
//
// 本头文件应只被各平台 binding 主编译单元包含一次（内部均为 inline 函数，
// 依赖 napi.h 与 logger.h）。

#ifndef ZTOOLS_LOGGER_BINDING_H_
#define ZTOOLS_LOGGER_BINDING_H_

#include <napi.h>

#include <string>

#include "logger.h"

namespace ztools_log_binding {

/**
 * 设置日志输出等级。
 * @param info[0] level: "trace" | "debug" | "info" | "warn" | "error" | "off"（大小写不敏感）
 * @returns 无返回值；level 非法时抛出 JS 异常
 */
inline Napi::Value SetLogLevel(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 1 || !info[0].IsString()) {
    Napi::TypeError::New(env, "Expected a level string (trace/debug/info/warn/error/off)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  const std::string level = info[0].As<Napi::String>().Utf8Value();
  ztools_log::Level parsed;
  if (!ztools_log::LevelFromString(level.c_str(), &parsed)) {
    Napi::TypeError::New(env, "level must be one of: trace, debug, info, warn, error, off")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  ztools_log::SetLevel(parsed);
  ZLOG_INFO("logger", "level set to %s via JS", ztools_log::LevelToString(parsed));
  return env.Undefined();
}

/**
 * 查询当前日志输出等级。
 * @returns 当前等级字符串（"trace"/"debug"/"info"/"warn"/"error"/"off"）
 */
inline Napi::Value GetLogLevel(const Napi::CallbackInfo& info) {
  return Napi::String::New(info.Env(), ztools_log::LevelToString(ztools_log::GetLevel()));
}

/**
 * 查询日志文件完整路径（系统临时目录下 ztools-native.log）。
 * @returns 日志文件绝对路径（UTF-8）
 */
inline Napi::Value GetLogFilePath(const Napi::CallbackInfo& info) {
  return Napi::String::New(info.Env(), ztools_log::FilePath());
}

/**
 * 查询某等级当前是否会被写入（受 setLogLevel / 环境变量影响）。
 * @param info[0] level: 等级字符串
 * @returns boolean；level 非法时抛出 JS 异常
 */
inline Napi::Value IsLogLevelEnabled(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 1 || !info[0].IsString()) {
    Napi::TypeError::New(env, "Expected a level string (trace/debug/info/warn/error/off)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  const std::string level = info[0].As<Napi::String>().Utf8Value();
  ztools_log::Level parsed;
  if (!ztools_log::LevelFromString(level.c_str(), &parsed)) {
    Napi::TypeError::New(env, "level must be one of: trace, debug, info, warn, error, off")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  return Napi::Boolean::New(env, ztools_log::Enabled(parsed));
}

/**
 * JS 侧向原生日志文件写入一条日志（与原生日志同一文件、同一格式）。
 * @param info[0] level: 等级字符串（低于当前输出等级时丢弃）
 * @param info[1] tag: 短模块名（如 "app"）
 * @param info[2] message: 消息内容（不建议包含超大文本/敏感信息）
 * @returns 无返回值；参数非法时抛出 JS 异常
 */
inline Napi::Value LogWrite(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 3 || !info[0].IsString() || !info[1].IsString() || !info[2].IsString()) {
    Napi::TypeError::New(env, "Expected (level: string, tag: string, message: string)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  const std::string level = info[0].As<Napi::String>().Utf8Value();
  const std::string tag = info[1].As<Napi::String>().Utf8Value();
  const std::string message = info[2].As<Napi::String>().Utf8Value();
  ztools_log::Level parsed;
  if (!ztools_log::LevelFromString(level.c_str(), &parsed) || parsed == ztools_log::kOff) {
    Napi::TypeError::New(env, "level must be one of: trace, debug, info, warn, error")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  ztools_log::Write(parsed, tag.c_str(), "%s", message.c_str());
  return env.Undefined();
}

/**
 * 把日志管理导出注册到模块 exports（各平台 Init 中调用）。
 * @param env N-API 环境
 * @param exports 模块导出对象
 */
inline void Register(Napi::Env env, Napi::Object exports) {
  exports.Set("setLogLevel", Napi::Function::New(env, SetLogLevel));
  exports.Set("getLogLevel", Napi::Function::New(env, GetLogLevel));
  exports.Set("getLogFilePath", Napi::Function::New(env, GetLogFilePath));
  exports.Set("isLogLevelEnabled", Napi::Function::New(env, IsLogLevelEnabled));
  exports.Set("logWrite", Napi::Function::New(env, LogWrite));
}

}  // namespace ztools_log_binding

#endif  // ZTOOLS_LOGGER_BINDING_H_
