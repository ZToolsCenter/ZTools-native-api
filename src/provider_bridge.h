// ZTools Provider 桥接层
//
// 让原生层（任意 native 线程上的 C++/Swift 代码）能够调用 JS 侧注册的方法，
// 典型用途是调用宿主应用（如 ZTools 主进程）提供的 provider 能力（翻译 / OCR 等）。
//
// 工作方式：
// 1. JS 侧调用 startProviderBridge(dispatcher) 注册分发函数，签名为
//    (type: string, inputJson: string, seq: number) => void，
//    JS 负责执行实际逻辑，并按 seq 通过 resolveProviderBridge / rejectProviderBridge 回传结果；
// 2. 原生侧有两种发起方式（共用同一张 pending 表与 seq 序号空间，互不冲突）：
//    a. 同步：Invoke(type, inputJson, timeoutMs)——发起线程阻塞等待结果（带超时）；
//    b. 异步：InvokeAsync(requestId, type, inputJson, callback, userData)——立即返回
//       受理与否，结果由 JS 侧回传时经 callback(requestId, ok, value, error, userData)
//       异步送达（调用线程通常是 JS 线程）；请求可经 CancelAsync(requestId) 取消，
//       取消/超时后晚到的 JS 结果按 seq 查表落空被静默丢弃，绝不重复回调。
//       超时本身不在此层实现（无独立定时器），由调用方（如 Swift 会话定时器
//       逐拍扫描 deadline 后调 CancelAsync）按需驱动；
// 3. JS 执行完成后回传结果 JSON 字符串，原生侧拿到 InvokeResult / 异步回调。
//
// 线程约束：
// - Invoke（同步）严禁在 JS 主线程调用（阻塞等待会与调度回调互相等待造成死锁），
//   内部检测到 JS 线程调用时立即返回错误；
// - InvokeAsync（异步）非阻塞，可在包括 JS 主线程在内的任意线程调用；
//   callback 可能从 JS 线程触发，实现方只允许做线程安全的转投（如入队），
//   不得在 callback 里直接操作 UI；
// - 其余导出函数（start / stop / resolve / reject 等）按常规 N-API 导出在 JS 线程上调用。

#ifndef ZTOOLS_PROVIDER_BRIDGE_H_
#define ZTOOLS_PROVIDER_BRIDGE_H_

#include <atomic>
#include <chrono>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

#include <napi.h>

#include "logger.h"

namespace ztools_provider_bridge {

/** 原生侧一次 provider 调用的结果。 */
struct InvokeResult {
  bool ok = false;    // 是否成功拿到 JS 侧结果
  std::string value;  // 成功时的结果 JSON 字符串
  std::string error;  // 失败原因（未启动 / 正在关闭 / 队列不可用 / 超时 / JS 侧报错等）
};

/**
 * 异步调用结果回调。InvokeAsync 受理后恰好触发一次：
 * JS 侧回传（resolve/reject，通常在 JS 线程）或 stopProviderBridge 收尾时。
 * CancelAsync 主动取消不触发回调（取消方已知状态），晚到的 JS 回传被静默丢弃。
 * 回调线程不保证是原生发起线程，实现方必须线程安全（只做入队/转投，禁止碰 UI）。
 */
using InvokeAsyncCallback = void (*)(uint64_t requestId, bool ok, const char* valueJson,
                                     const char* error, void* userData);

namespace internal {

/** 投递给 JS 线程的一次调用请求（BridgeCallJs 处理完后释放）。
 *  requestId > 0 表示异步调用（结果按 requestId 回投）；0 表示同步调用（走 promise）。 */
struct InvokeJob {
  uint64_t seq;
  uint64_t requestId;
  std::string type;
  std::string inputJson;
};

/** invokeProviderFromNative 的回调载荷（NativeInvokeCallJs 处理完后释放）。 */
struct NativeInvokePayload {
  napi_threadsafe_function tsfn;
  bool ok;
  std::string value;
  std::string error;
};

/** invokeProviderAsyncFromNative 的回调载荷（NativeAsyncCallJs 处理完后释放）。 */
struct NativeAsyncPayload {
  napi_threadsafe_function tsfn;
  bool ok;
  std::string value;
  std::string error;
};

/** invokeProviderAsyncFromNative 的上下文（异步回调与超时看门狗竞争一次性所有权）。 */
struct NativeAsyncContext {
  uint64_t requestId;
  uint32_t timeoutMs;
  napi_threadsafe_function cbTsfn;
  std::atomic<bool> delivered;
};

/** pending 表条目：同步调用（promise 非空）或异步调用（requestId > 0）。 */
struct PendingEntry {
  std::shared_ptr<std::promise<InvokeResult>> promise;  // 同步路径：等待方 future
  uint64_t requestId = 0;             // 异步路径：调用方请求号（0 = 同步）
  InvokeAsyncCallback callback = nullptr;
  void* userData = nullptr;
  napi_threadsafe_function tsfn = nullptr;  // 异步路径：受理时 acquire 的 TSFN 引用，
                                            // 条目终结（回传/取消/停止）时配对释放
};

// 桥接全局状态：本头文件在各平台只被 binding 主编译单元包含一次，
// 使用函数内静态保证唯一切片，避免 ODR 问题
inline std::mutex& Mutex() {
  static std::mutex m;
  return m;
}
inline napi_threadsafe_function& Tsfn() {
  static napi_threadsafe_function tsfn = nullptr;
  return tsfn;
}
inline std::thread::id& JsThreadId() {
  static std::thread::id tid;
  return tid;
}
inline std::atomic<uint64_t>& NextSeq() {
  static std::atomic<uint64_t> seq(0);
  return seq;
}
inline std::map<uint64_t, PendingEntry>& Pending() {
  static std::map<uint64_t, PendingEntry> pending;
  return pending;
}
// 异步 requestId → seq 反查（取消与去重用；seq 是对 JS dispatcher 的统一序号）
inline std::map<uint64_t, uint64_t>& AsyncSeqIndex() {
  static std::map<uint64_t, uint64_t> index;
  return index;
}

// 在 JS 线程上执行：取出请求，调用 JS 分发函数 dispatcher(type, inputJson, seq)
inline void BridgeCallJs(napi_env env, napi_value js_callback, void* /*context*/, void* data) {
  auto* job = static_cast<InvokeJob*>(data);
  if (job == nullptr) {
    return;
  }
  if (env != nullptr && js_callback != nullptr) {
    ZLOG_DEBUG("provider", "dispatch seq=%llu req=%llu type=\"%s\" (%zu bytes)",
               static_cast<unsigned long long>(job->seq),
               static_cast<unsigned long long>(job->requestId), job->type.c_str(),
               job->inputJson.size());
    napi_value global = nullptr;
    napi_get_global(env, &global);
    napi_value argv[3];
    napi_create_string_utf8(env, job->type.c_str(), NAPI_AUTO_LENGTH, &argv[0]);
    napi_create_string_utf8(env, job->inputJson.c_str(), NAPI_AUTO_LENGTH, &argv[1]);
    napi_create_double(env, static_cast<double>(job->seq), &argv[2]);
    napi_call_function(env, global, js_callback, 3, argv, nullptr);
  }
  delete job;
}

// 原子摘取指定 seq 的 pending 条目并同步清理异步反查表；不存在（已超时 / 已取消 /
// 已停止）时返回 false——晚到的 JS 回传正是靠此落空被静默丢弃。
// out 可为 nullptr（只需移除登记、不取条目内容的调用方）。
inline bool TakePending(uint64_t seq, PendingEntry* out) {
  uint64_t asyncRequestId = 0;
  {
    std::lock_guard<std::mutex> lock(Mutex());
    auto it = Pending().find(seq);
    if (it == Pending().end()) {
      return false;
    }
    asyncRequestId = it->second.requestId;
    if (out != nullptr) {
      *out = it->second;
    }
    Pending().erase(it);
  }
  if (asyncRequestId != 0) {
    std::lock_guard<std::mutex> lock(Mutex());
    AsyncSeqIndex().erase(asyncRequestId);
  }
  return true;
}

// 终结一个异步条目（调用方已持有摘出的条目，无锁）：释放 TSFN 引用并触发回调
inline void DeliverAsync(PendingEntry& entry, bool ok, const std::string& value,
                         const std::string& error) {
  if (entry.tsfn != nullptr) {
    napi_release_threadsafe_function(entry.tsfn, napi_tsfn_release);
    entry.tsfn = nullptr;
  }
  if (entry.callback != nullptr) {
    entry.callback(entry.requestId, ok, value.c_str(), error.c_str(), entry.userData);
  }
}

}  // namespace internal

// ==================== 原生侧调用入口（供本插件内的原生模块使用） ====================

/** 桥接是否已启动（JS 侧已注册分发函数）。 */
inline bool IsReady() {
  std::lock_guard<std::mutex> lock(internal::Mutex());
  return internal::Tsfn() != nullptr;
}

/**
 * 原生线程调用 JS 侧 provider 方法并阻塞等待结果。
 * @param type 能力类型（如 "translation" / "ocr"，由 JS 侧自行约定）
 * @param inputJson 入参 JSON 字符串
 * @param timeoutMs 等待 JS 侧结果的超时时间（毫秒）
 * @returns 调用结果；失败时 ok=false 且 error 说明原因
 */
inline InvokeResult Invoke(const std::string& type, const std::string& inputJson,
                           uint32_t timeoutMs) {
  napi_threadsafe_function tsfn = nullptr;
  {
    std::lock_guard<std::mutex> lock(internal::Mutex());
    tsfn = internal::Tsfn();
    if (tsfn == nullptr) {
      ZLOG_WARN("provider", "invoke \"%s\" rejected: bridge not started", type.c_str());
      return InvokeResult{false, "", "provider bridge not started"};
    }
    // JS 线程上阻塞等待会与 BridgeCallJs 互相等待，直接拒绝
    if (std::this_thread::get_id() == internal::JsThreadId()) {
      ZLOG_WARN("provider", "invoke \"%s\" rejected: called on JS main thread", type.c_str());
      return InvokeResult{false, "", "InvokeProvider must not be called on the JS main thread"};
    }
    // 增加引用计数，防止等待期间被 stopProviderBridge 释放底层函数
    if (napi_acquire_threadsafe_function(tsfn) != napi_ok) {
      ZLOG_WARN("provider", "invoke \"%s\" rejected: bridge is closing", type.c_str());
      return InvokeResult{false, "", "provider bridge is closing"};
    }
  }

  ZLOG_DEBUG("provider", "invoke \"%s\" (timeout %ums, input %zu bytes)", type.c_str(),
             timeoutMs, inputJson.size());
  const uint64_t seq = internal::NextSeq().fetch_add(1) + 1;
  auto promise = std::make_shared<std::promise<InvokeResult>>();
  auto future = promise->get_future();
  {
    std::lock_guard<std::mutex> lock(internal::Mutex());
    internal::PendingEntry& entry = internal::Pending()[seq];
    entry.promise = promise;
  }

  auto* job = new internal::InvokeJob{seq, 0, type, inputJson};
  if (napi_call_threadsafe_function(tsfn, job, napi_tsfn_nonblocking) != napi_ok) {
    // 桥接正在关闭，请求未入队（CallJs 不会处理该 job），此处代为清理
    ZLOG_WARN("provider", "invoke \"%s\" failed: queue unavailable", type.c_str());
    internal::TakePending(seq, nullptr);
    delete job;
    napi_release_threadsafe_function(tsfn, napi_tsfn_release);
    return InvokeResult{false, "", "provider bridge queue unavailable"};
  }

  InvokeResult result;
  if (future.wait_for(std::chrono::milliseconds(timeoutMs)) == std::future_status::timeout) {
    // 超时后移除登记；JS 迟到回传的结果会被 TakePending 丢弃
    ZLOG_ERROR("provider", "invoke \"%s\" timed out after %ums", type.c_str(), timeoutMs);
    internal::TakePending(seq, nullptr);
    result = InvokeResult{false, "", "provider invocation timed out"};
  } else {
    result = future.get();
  }
  napi_release_threadsafe_function(tsfn, napi_tsfn_release);
  if (result.ok) {
    ZLOG_DEBUG("provider", "invoke \"%s\" ok (result %zu bytes)", type.c_str(),
               result.value.size());
  } else {
    ZLOG_WARN("provider", "invoke \"%s\" failed: %s", type.c_str(), result.error.c_str());
  }
  return result;
}

/** InvokeAsync 的受理结果：accepted=false 时不会触发回调，error 说明拒绝原因。 */
struct InvokeAccept {
  bool accepted = false;
  std::string error;
};

/**
 * 原生侧异步调用 JS 侧 provider 方法：立即返回受理与否，不阻塞等待。
 * 与同步 Invoke 共用同一 seq 序号空间（对 JS dispatcher 的 seq 互不冲突）；
 * 结果经 callback 异步送达（通常在 JS 线程触发），恰好一次。
 * 非阻塞（只做查表 + TSFN 入队），可在包括 JS 主线程在内的任意线程调用。
 * @param requestId 调用方请求号（须在调用方上下文内唯一，重复将被拒绝）
 * @param type 能力类型（如 "translation" / "ocr"，由 JS 侧自行约定）
 * @param inputJson 入参 JSON 字符串
 * @param callback 结果回调（必填；触发线程不保证是发起线程）
 * @param userData 回调透传指针（桥不解释、不持有所有权）
 * @returns 受理结果；拒绝时 callback 不触发，调用方当场收尾
 */
inline InvokeAccept InvokeAsync(uint64_t requestId, const std::string& type,
                                const std::string& inputJson,
                                InvokeAsyncCallback callback, void* userData) {
  if (callback == nullptr) {
    return InvokeAccept{false, "async callback required"};
  }
  napi_threadsafe_function tsfn = nullptr;
  {
    std::lock_guard<std::mutex> lock(internal::Mutex());
    tsfn = internal::Tsfn();
    if (tsfn == nullptr) {
      ZLOG_WARN("provider", "async invoke req=%llu \"%s\" rejected: bridge not started",
                static_cast<unsigned long long>(requestId), type.c_str());
      return InvokeAccept{false, "provider bridge not started"};
    }
    if (internal::AsyncSeqIndex().count(requestId) != 0) {
      return InvokeAccept{false, "duplicate requestId"};
    }
    // 持有 TSFN 引用直至条目终结（回传/取消/停止时释放），防止等待期间被
    // stopProviderBridge 释放底层函数
    if (napi_acquire_threadsafe_function(tsfn) != napi_ok) {
      ZLOG_WARN("provider", "async invoke req=%llu \"%s\" rejected: bridge is closing",
                static_cast<unsigned long long>(requestId), type.c_str());
      return InvokeAccept{false, "provider bridge is closing"};
    }
  }

  const uint64_t seq = internal::NextSeq().fetch_add(1) + 1;
  {
    std::lock_guard<std::mutex> lock(internal::Mutex());
    internal::PendingEntry& entry = internal::Pending()[seq];
    entry.requestId = requestId;
    entry.callback = callback;
    entry.userData = userData;
    entry.tsfn = tsfn;
    internal::AsyncSeqIndex()[requestId] = seq;
  }
  ZLOG_DEBUG("provider", "async invoke req=%llu seq=%llu \"%s\" dispatched (%zu bytes)",
             static_cast<unsigned long long>(requestId), static_cast<unsigned long long>(seq),
             type.c_str(), inputJson.size());

  auto* job = new internal::InvokeJob{seq, requestId, type, inputJson};
  if (napi_call_threadsafe_function(tsfn, job, napi_tsfn_nonblocking) != napi_ok) {
    // 桥接正在关闭，请求未入队（CallJs 不会处理该 job），此处代为清理
    ZLOG_WARN("provider", "async invoke req=%llu \"%s\" failed: queue unavailable",
              static_cast<unsigned long long>(requestId), type.c_str());
    internal::PendingEntry leftover;
    internal::TakePending(seq, &leftover);
    if (leftover.tsfn != nullptr) {
      napi_release_threadsafe_function(leftover.tsfn, napi_tsfn_release);
    }
    delete job;
    return InvokeAccept{false, "provider bridge queue unavailable"};
  }
  return InvokeAccept{true, ""};
}

/**
 * 取消一个进行中的异步请求：登记立即移除，此后 JS 晚到的回传按 seq 查表落空、
 * 被静默丢弃，绝不触发 callback（取消方已知状态，无需再通知）。
 * 幂等：请求不存在 / 已完成 / 已取消时返回 false。
 * 注意：这不会终止 JS 侧已经在跑的 Promise / 网络请求（桥无法深入 JS 执行），
 * 只保证取消方的生命周期不受晚到结果影响。
 * @param requestId 调用方请求号
 * @returns 是否确由本次调用完成取消
 */
inline bool CancelAsync(uint64_t requestId) {
  internal::PendingEntry entry;
  {
    std::lock_guard<std::mutex> lock(internal::Mutex());
    auto indexIt = internal::AsyncSeqIndex().find(requestId);
    if (indexIt == internal::AsyncSeqIndex().end()) {
      return false;
    }
    const uint64_t seq = indexIt->second;
    auto it = internal::Pending().find(seq);
    if (it == internal::Pending().end()) {
      internal::AsyncSeqIndex().erase(indexIt);
      return false;
    }
    entry = it->second;
    internal::Pending().erase(it);
    internal::AsyncSeqIndex().erase(indexIt);
  }
  if (entry.tsfn != nullptr) {
    napi_release_threadsafe_function(entry.tsfn, napi_tsfn_release);
  }
  ZLOG_DEBUG("provider", "async invoke req=%llu cancelled (late result will be discarded)",
             static_cast<unsigned long long>(requestId));
  return true;
}

// ==================== N-API 导出（在 JS 线程上被调用） ====================

/**
 * 启动桥接：注册 JS 分发函数。
 * @param info[0] dispatcher: (type: string, inputJson: string, seq: number) => void，
 *                JS 侧负责执行逻辑并按 seq 调 resolveProviderBridge / rejectProviderBridge
 * @returns 无返回值；参数非法或重复启动时抛出 JS 异常
 */
inline Napi::Value StartProviderBridge(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 1 || !info[0].IsFunction()) {
    Napi::TypeError::New(env, "Expected a dispatcher function").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  {
    std::lock_guard<std::mutex> lock(internal::Mutex());
    if (internal::Tsfn() != nullptr) {
      Napi::Error::New(env, "Provider bridge already started").ThrowAsJavaScriptException();
      return env.Undefined();
    }
  }

  napi_value callback = info[0];
  napi_value resource_name;
  napi_create_string_utf8(env, "ProviderBridgeCallback", NAPI_AUTO_LENGTH, &resource_name);

  napi_threadsafe_function tsfn = nullptr;
  if (napi_create_threadsafe_function(env, callback, nullptr, resource_name, 0, 1, nullptr,
                                      nullptr, nullptr, internal::BridgeCallJs,
                                      &tsfn) != napi_ok) {
    Napi::Error::New(env, "Failed to create provider bridge").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  {
    std::lock_guard<std::mutex> lock(internal::Mutex());
    internal::Tsfn() = tsfn;
    // 记录 JS 主线程 id，供 Invoke 做死锁防护
    internal::JsThreadId() = std::this_thread::get_id();
  }
  ZLOG_INFO("provider", "bridge started");
  return env.Undefined();
}

/**
 * 停止桥接：所有还在等待的原生调用立即以错误结束，已入队的请求仍会被处理完。
 * 同步等待线程经 promise 唤醒；异步请求经 callback 收到 "provider bridge stopped"
 * （回调线程 = 本函数调用线程，即 JS 线程）。
 * @returns 无返回值
 */
inline Napi::Value StopProviderBridge(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  napi_threadsafe_function tsfn = nullptr;
  std::map<uint64_t, internal::PendingEntry> pending;
  {
    std::lock_guard<std::mutex> lock(internal::Mutex());
    tsfn = internal::Tsfn();
    internal::Tsfn() = nullptr;
    pending.swap(internal::Pending());
    internal::AsyncSeqIndex().clear();
  }
  size_t asyncCount = 0;
  for (auto& entry : pending) {
    if (entry.second.promise != nullptr) {
      // 让同步等待线程立即失败，而不是挂到超时
      entry.second.promise->set_value(InvokeResult{false, "", "provider bridge stopped"});
    } else {
      asyncCount++;
      internal::DeliverAsync(entry.second, false, "", "provider bridge stopped");
    }
  }
  ZLOG_INFO("provider", "bridge stopped (%zu sync / %zu async pending call(s) failed)",
            pending.size() - asyncCount, asyncCount);
  if (tsfn != nullptr) {
    napi_release_threadsafe_function(tsfn, napi_tsfn_release);
  }
  return env.Undefined();
}

/**
 * JS 侧回传成功结果。
 * @param info[0] seq 请求序号（与分发出的 seq 一致）
 * @param info[1] resultJson 结果 JSON 字符串
 * @returns 无返回值；seq 已超时 / 已取消或不存在时静默丢弃
 */
inline Napi::Value ResolveProviderBridge(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 2 || !info[0].IsNumber() || !info[1].IsString()) {
    Napi::TypeError::New(env, "Expected (seq: number, resultJson: string)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  const uint64_t seq = static_cast<uint64_t>(info[0].As<Napi::Number>().DoubleValue());
  const std::string value = info[1].As<Napi::String>().Utf8Value();
  internal::PendingEntry entry;
  if (!internal::TakePending(seq, &entry)) {
    // 已超时 / 已取消 / 已停止：晚到结果按约定静默丢弃
    ZLOG_DEBUG("provider", "late resolve seq=%llu discarded", static_cast<unsigned long long>(seq));
    return env.Undefined();
  }
  if (entry.promise != nullptr) {
    entry.promise->set_value(InvokeResult{true, value, ""});
  } else {
    ZLOG_DEBUG("provider", "async invoke req=%llu ok (result %zu bytes)",
               static_cast<unsigned long long>(entry.requestId), value.size());
    internal::DeliverAsync(entry, true, value, "");
  }
  return env.Undefined();
}

/**
 * JS 侧回传失败。
 * @param info[0] seq 请求序号（与分发出的 seq 一致）
 * @param info[1] error 错误说明
 * @returns 无返回值；seq 已超时 / 已取消或不存在时静默丢弃
 */
inline Napi::Value RejectProviderBridge(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 2 || !info[0].IsNumber() || !info[1].IsString()) {
    Napi::TypeError::New(env, "Expected (seq: number, error: string)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  const uint64_t seq = static_cast<uint64_t>(info[0].As<Napi::Number>().DoubleValue());
  const std::string error = info[1].As<Napi::String>().Utf8Value();
  internal::PendingEntry entry;
  if (!internal::TakePending(seq, &entry)) {
    ZLOG_DEBUG("provider", "late reject seq=%llu discarded", static_cast<unsigned long long>(seq));
    return env.Undefined();
  }
  if (entry.promise != nullptr) {
    entry.promise->set_value(InvokeResult{false, "", error});
  } else {
    ZLOG_DEBUG("provider", "async invoke req=%llu failed: %s",
               static_cast<unsigned long long>(entry.requestId), error.c_str());
    internal::DeliverAsync(entry, false, "", error);
  }
  return env.Undefined();
}

/**
 * 查询桥接是否就绪。
 * @returns 就绪返回 true，未启动（或已停止）返回 false
 */
inline Napi::Value IsProviderBridgeReady(const Napi::CallbackInfo& info) {
  return Napi::Boolean::New(info.Env(), IsReady());
}

// ==================== 供 JS 侧验证桥接通路的回调式入口 ====================

// 把原生线程上的调用结果带回 JS 线程并调用回调
inline void NativeInvokeCallJs(napi_env env, napi_value js_callback, void* /*context*/,
                               void* data) {
  auto* payload = static_cast<internal::NativeInvokePayload*>(data);
  if (payload == nullptr) {
    return;
  }
  if (env != nullptr && js_callback != nullptr) {
    napi_value global = nullptr;
    napi_get_global(env, &global);
    napi_value argv[2] = {nullptr, nullptr};
    if (payload->ok) {
      napi_get_null(env, &argv[0]);
      napi_create_string_utf8(env, payload->value.c_str(), NAPI_AUTO_LENGTH, &argv[1]);
    } else {
      napi_create_string_utf8(env, payload->error.c_str(), NAPI_AUTO_LENGTH, &argv[0]);
      napi_get_null(env, &argv[1]);
    }
    napi_call_function(env, global, js_callback, 2, argv, nullptr);
  }
  // 与创建时的 initial_thread_count=1 配对，由消费方释放；随后释放载荷
  napi_release_threadsafe_function(payload->tsfn, napi_tsfn_release);
  delete payload;
}

/**
 * 在独立原生线程上发起一次 provider 调用，完成后在 JS 线程回调结果。
 * 走的是与 Invoke 完全相同的真实通路，主要用于从 JS 验证桥接；
 * 真正的原生业务代码应直接调用 Invoke。
 * @param info[0] type 能力类型
 * @param info[1] inputJson 入参 JSON 字符串
 * @param info[2] timeoutMs 超时毫秒数（可选，默认 15000）
 * @param info[3] callback: (error: string | null, resultJson: string | null) => void
 * @returns 无返回值
 */
inline Napi::Value InvokeProviderFromNative(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 4 || !info[0].IsString() || !info[1].IsString() || !info[3].IsFunction()) {
    Napi::TypeError::New(env,
                         "Expected (type: string, inputJson: string, timeoutMs?: number, "
                         "callback: function)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  const std::string type = info[0].As<Napi::String>().Utf8Value();
  const std::string inputJson = info[1].As<Napi::String>().Utf8Value();
  const uint32_t timeoutMs =
      info.Length() >= 3 && info[2].IsNumber() ? info[2].As<Napi::Number>().Uint32Value() : 15000u;

  napi_value callback = info[3];
  napi_value resource_name;
  napi_create_string_utf8(env, "ProviderBridgeNativeInvoke", NAPI_AUTO_LENGTH, &resource_name);

  napi_threadsafe_function tsfn = nullptr;
  if (napi_create_threadsafe_function(env, callback, nullptr, resource_name, 0, 1, nullptr,
                                      nullptr, nullptr, NativeInvokeCallJs,
                                      &tsfn) != napi_ok) {
    Napi::Error::New(env, "Failed to create native invoke callback")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  // 生产者线程只负责投递；TSFN 由 NativeInvokeCallJs（消费方）释放
  std::thread([type, inputJson, timeoutMs, tsfn]() {
    InvokeResult result = Invoke(type, inputJson, timeoutMs);
    auto* payload = new internal::NativeInvokePayload{tsfn, result.ok, result.value, result.error};
    if (napi_call_threadsafe_function(tsfn, payload, napi_tsfn_nonblocking) != napi_ok) {
      // 投递失败时 CallJs 不会执行，此处代为释放，避免泄漏
      napi_release_threadsafe_function(tsfn, napi_tsfn_release);
      delete payload;
    }
  }).detach();

  return env.Undefined();
}

// ==================== 供 JS 侧验证异步桥接通路的回调式入口 ====================

// 把异步调用结果带回 JS 线程并调用回调（与 NativeInvokeCallJs 同构）
inline void NativeAsyncCallJs(napi_env env, napi_value js_callback, void* /*context*/,
                              void* data) {
  auto* payload = static_cast<internal::NativeAsyncPayload*>(data);
  if (payload == nullptr) {
    return;
  }
  if (env != nullptr && js_callback != nullptr) {
    napi_value global = nullptr;
    napi_get_global(env, &global);
    napi_value argv[2] = {nullptr, nullptr};
    if (payload->ok) {
      napi_get_null(env, &argv[0]);
      napi_create_string_utf8(env, payload->value.c_str(), NAPI_AUTO_LENGTH, &argv[1]);
    } else {
      napi_create_string_utf8(env, payload->error.c_str(), NAPI_AUTO_LENGTH, &argv[0]);
      napi_get_null(env, &argv[1]);
    }
    napi_call_function(env, global, js_callback, 2, argv, nullptr);
  }
  // 与创建时的 initial_thread_count=1 配对，由消费方释放；随后释放载荷
  napi_release_threadsafe_function(payload->tsfn, napi_tsfn_release);
  delete payload;
}

// 异步完成回投（桥触发，通常在 JS 线程）：一次性所有权竞争（delivered 原子置位），
// 赢者把结果经 ctx->cbTsfn 送回 JS 并释放 ctx
inline void NativeAsyncDeliver(internal::NativeAsyncContext* ctx, bool ok,
                               const char* value, const char* error) {
  bool expected = false;
  if (!ctx->delivered.compare_exchange_strong(expected, true)) {
    return;  // 另一方（超时看门狗 / 桥回调）已投递
  }
  auto* payload =
      new internal::NativeAsyncPayload{ctx->cbTsfn, ok, value ? value : "", error ? error : ""};
  if (napi_call_threadsafe_function(ctx->cbTsfn, payload, napi_tsfn_nonblocking) != napi_ok) {
    napi_release_threadsafe_function(ctx->cbTsfn, napi_tsfn_release);
    delete payload;
  }
  delete ctx;
}

// 桥异步回调适配（InvokeAsync 的 callback，userData = NativeAsyncContext*）
inline void NativeAsyncBridgeCallback(uint64_t requestId, bool ok, const char* valueJson,
                                      const char* error, void* userData) {
  (void)requestId;
  auto* ctx = static_cast<internal::NativeAsyncContext*>(userData);
  if (ctx == nullptr) {
    return;
  }
  NativeAsyncDeliver(ctx, ok, valueJson, error);
}

/**
 * 在当前线程（含 JS 线程）发起一次异步 provider 调用，结果经 callback 回到 JS。
 * 与 macOS 截图翻译的生产通路完全一致（InvokeAsync + 看门狗超时 + CancelAsync
 * 丢弃晚到结果），用于从 JS 验证异步桥接；真正的原生业务代码应直接调用
 * InvokeAsync。
 * @param info[0] type 能力类型
 * @param info[1] inputJson 入参 JSON 字符串
 * @param info[2] requestId 请求号（调用方保证唯一，用于 cancelProviderAsyncFromNative 取消）
 * @param info[3] timeoutMs 超时毫秒数（到时取消请求并以 timeout 错误回调；
 *                 JS 侧晚到的 resolve/reject 被桥丢弃）
 * @param info[4] callback: (error: string | null, resultJson: string | null) => void
 * @returns boolean：true 已受理（结果将经 callback 异步送达）；false 立即拒绝
 *          （callback 不会被调用）
 */
inline Napi::Value InvokeProviderAsyncFromNative(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 5 || !info[0].IsString() || !info[1].IsString() ||
      !info[2].IsNumber() || !info[3].IsNumber() || !info[4].IsFunction()) {
    Napi::TypeError::New(env,
                         "Expected (type: string, inputJson: string, requestId: number, "
                         "timeoutMs: number, callback: function)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  const std::string type = info[0].As<Napi::String>().Utf8Value();
  const std::string inputJson = info[1].As<Napi::String>().Utf8Value();
  const uint64_t requestId = static_cast<uint64_t>(info[2].As<Napi::Number>().Int64Value());
  const uint32_t timeoutMs = info[3].As<Napi::Number>().Uint32Value();
  if (requestId == 0) {
    Napi::TypeError::New(env, "requestId must be a positive number")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  napi_value callback = info[4];
  napi_value resource_name;
  napi_create_string_utf8(env, "ProviderBridgeNativeInvokeAsync", NAPI_AUTO_LENGTH,
                          &resource_name);
  napi_threadsafe_function cbTsfn = nullptr;
  if (napi_create_threadsafe_function(env, callback, nullptr, resource_name, 0, 1, nullptr,
                                      nullptr, nullptr, NativeAsyncCallJs,
                                      &cbTsfn) != napi_ok) {
    Napi::Error::New(env, "Failed to create native async callback")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  auto* ctx = new internal::NativeAsyncContext{requestId, timeoutMs, cbTsfn, false};

  InvokeAccept accept = InvokeAsync(requestId, type, inputJson, NativeAsyncBridgeCallback, ctx);
  if (!accept.accepted) {
    // 拒绝路径：桥不会回调，此处代为释放（TSFN 引用由本函数自建自释）
    napi_release_threadsafe_function(cbTsfn, napi_tsfn_release);
    delete ctx;
    Napi::Error::New(env, accept.error).ThrowAsJavaScriptException();
    return Napi::Boolean::New(env, false);
  }

  // 超时看门狗（镜像 Swift 泵循环的 deadline 扫描）：到时取消请求，晚到的 JS
  // 回传因登记已移除被桥静默丢弃
  std::thread([ctx]() {
    std::this_thread::sleep_for(std::chrono::milliseconds(ctx->timeoutMs));
    if (!CancelAsync(ctx->requestId)) {
      return;  // 已完成（回调已投递或正在投递），ctx 归属对方
    }
    NativeAsyncDeliver(ctx, false, "", "provider invocation timed out");
  }).detach();

  return Napi::Boolean::New(env, true);
}

/**
 * 取消一个经 invokeProviderAsyncFromNative 发起的异步请求（幂等）。
 * @param info[0] requestId 请求号
 * @returns boolean：是否确由本次调用完成取消（已回调 / 已取消时为 false）
 */
inline Napi::Value CancelProviderAsyncFromNative(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 1 || !info[0].IsNumber()) {
    Napi::TypeError::New(env, "Expected (requestId: number)").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  const uint64_t requestId = static_cast<uint64_t>(info[0].As<Napi::Number>().Int64Value());
  return Napi::Boolean::New(env, CancelAsync(requestId));
}

}  // namespace ztools_provider_bridge

#endif  // ZTOOLS_PROVIDER_BRIDGE_H_
