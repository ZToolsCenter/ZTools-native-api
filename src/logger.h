// ZTools 原生层日志模块（header-only，Windows / macOS 共用）
//
// 目标：让原生层（N-API 绑定、截图会话、监控线程等）不再像黑盒，
// 关键调用、状态变化与失败路径都能落到临时目录里的日志文件，便于用户侧排查。
//
// 行为约定：
// - 日志文件：<系统临时目录>/ztools-native.log
//   Windows 为 GetTempPathW（%TMP%/%TEMP%），macOS 为 $TMPDIR（兜底 /tmp）；
// - 大小限制：当前文件不超过 10MB；写满时整体轮转为 ztools-native.log.old
//   （先删除旧 .old 再改名；改名失败时退化为截断重开），任何时刻最多两个文件；
// - 等级：trace < debug < info < warn < error，另有 off 表示完全关闭。
//   默认 info；可用环境变量 ZTOOLS_LOG_LEVEL（进程启动前设置）或 JS 侧
//   setLogLevel()（运行时覆盖）控制，等级过滤在格式化之前完成，开销极低；
// - 线程安全：内部一把互斥锁串行化写入，任意 native 线程（监控回调线程、
//   截图会话线程等）均可直接调用；每条日志单次写入并 fflush，崩溃前已写内容不丢；
// - 格式：2026-09-22 12:34:56.789 [info] [tid 12345] [tag] message；
//   多进程（宿主 + 测试脚本）同时写同一文件时按行追加，偶发交错可接受。
//
// 用法（原生代码）：
//   #include "logger.h"
//   ZLOG_INFO("clipboard", "monitor started");
//   ZLOG_DEBUG("screenshot", "session requested (autoConfirm=%d)", autoConfirm);
//   if (ztools_log::Enabled(ztools_log::kDebug)) { ... }  // 昂贵参数构造可先用此门控

#ifndef ZTOOLS_LOGGER_H_
#define ZTOOLS_LOGGER_H_

#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#else
#include <cstdlib>
#include <ctime>
#include <sys/time.h>
#include <unistd.h>
#endif

#if defined(__GNUC__) || defined(__clang__)
// 编译期检查 Write 的 printf 格式串与实参是否匹配（fmt 为第 3 个参数）
#define ZTOOLS_LOG_PRINTF_ATTR __attribute__((format(printf, 3, 4)))
#else
#define ZTOOLS_LOG_PRINTF_ATTR
#endif

namespace ztools_log {

// 日志等级：数值越小越详细；过滤规则为「消息等级 >= 当前等级」才写入。
enum Level {
  kTrace = 0,
  kDebug = 1,
  kInfo = 2,
  kWarn = 3,
  kError = 4,
  kOff = 5,  // 仅供等级设置使用，普通消息不会以此等级发出
};

// 解析等级字符串（大小写不敏感，兼容 warning/none 别名）；无法识别返回 false 且不改动 out
inline bool LevelFromString(const char* name, Level* out) {
  if (name == nullptr || out == nullptr) {
    return false;
  }
  std::string v;
  for (const char* p = name; *p != '\0'; ++p) {
    v.push_back((*p >= 'A' && *p <= 'Z') ? (char)(*p - 'A' + 'a') : *p);
  }
  Level parsed;
  if (v == "trace") parsed = kTrace;
  else if (v == "debug") parsed = kDebug;
  else if (v == "info") parsed = kInfo;
  else if (v == "warn" || v == "warning") parsed = kWarn;
  else if (v == "error") parsed = kError;
  else if (v == "off" || v == "none") parsed = kOff;
  else return false;
  *out = parsed;
  return true;
}

// 等级枚举 → 可读字符串（getLogLevel 导出与日志行前缀共用）
inline const char* LevelToString(Level level) {
  switch (level) {
    case kTrace: return "trace";
    case kDebug: return "debug";
    case kInfo: return "info";
    case kWarn: return "warn";
    case kError: return "error";
    default: return "off";
  }
}

namespace internal {

// 单个日志文件上限：超过后整体轮转为 .old 重新开写（当前文件保证不超过该值）
const uint64_t kMaxFileBytes = 10ULL * 1024 * 1024;
// 单条日志格式化后的上限字节数，防御性截断超大内容（如误传的 base64）
const size_t kMaxMessageBytes = 8192;

inline std::mutex& FileMutex() {
  static std::mutex m;
  return m;
}

// 当前等级（原子读写；Write 的过滤判断无锁）
inline std::atomic<int>& LevelRef() {
  static std::atomic<int> level(kInfo);
  return level;
}

// JS 侧是否已显式 SetLevel（显式设置后环境变量不再覆盖）
inline std::atomic<bool>& LevelExplicit() {
  static std::atomic<bool> flag(false);
  return flag;
}

// 环境变量是否已应用（仅在 FileMutex 保护下访问）
inline bool& EnvApplied() {
  static bool flag = false;
  return flag;
}

// 日志文件运行状态（所有字段仅在 FileMutex 保护下访问）
struct FileState {
  FILE* fp = nullptr;
  uint64_t bytes = 0;    // 当前文件已写字节数（打开时按现有文件初始化）
  std::string pathUtf8;  // 完整路径（UTF-8，供 JS 展示）
#ifdef _WIN32
  std::wstring pathWide;  // 完整路径（供 _wfopen/_wrename 使用）
#endif
  bool openFailedReported = false;  // 打开失败只向 stderr 报一次，避免刷屏
};

inline FileState& State() {
  static FileState s;
  return s;
}

// UTF-16 → UTF-8（仅 Windows，用于把临时目录路径转成可展示/传给 JS 的形式）
#ifdef _WIN32
inline std::string WideToUtf8(const std::wstring& w) {
  if (w.empty()) return std::string();
  int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
  std::string out;
  if (n > 0) {
    out.resize((size_t)n);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &out[0], n, nullptr, nullptr);
  }
  return out;
}
#endif

// 计算日志文件完整路径（不打开文件；结果缓存，只在首次调用时求值）
inline void ComputePathLocked() {
  FileState& s = State();
  if (!s.pathUtf8.empty()) {
    return;
  }
#ifdef _WIN32
  wchar_t tempDir[MAX_PATH] = L"";
  DWORD n = GetTempPathW(MAX_PATH, tempDir);
  if (n == 0 || n >= MAX_PATH) {
    n = 0;
    tempDir[0] = L'\0';
  }
  s.pathWide = std::wstring(tempDir) + L"ztools-native.log";
  s.pathUtf8 = WideToUtf8(s.pathWide);
#else
  const char* tmp = std::getenv("TMPDIR");
  std::string dir = (tmp != nullptr && tmp[0] != '\0') ? tmp : "/tmp";
  if (dir[dir.size() - 1] != '/') {
    dir += '/';
  }
  s.pathUtf8 = dir + "ztools-native.log";
#endif
}

// 以追加模式打开日志文件；成功时把已写字节数同步为当前文件大小
inline bool OpenFileLocked() {
  FileState& s = State();
  if (s.fp != nullptr) {
    return true;
  }
  ComputePathLocked();
#ifdef _WIN32
  s.fp = _wfopen(s.pathWide.c_str(), L"ab");
#else
  s.fp = std::fopen(s.pathUtf8.c_str(), "ab");
#endif
  if (s.fp != nullptr) {
    if (std::fseek(s.fp, 0, SEEK_END) == 0) {
      const long size = std::ftell(s.fp);
      s.bytes = size > 0 ? (uint64_t)size : 0;
    } else {
      s.bytes = 0;
    }
  }
  return s.fp != nullptr;
}

// 轮转：关闭当前文件 → 删除旧 .old → 当前文件改名为 .old → 以截断模式重建。
// 改名失败（极少见，如其他进程持有 .old）时退化为直接截断重建，保证不超上限。
inline void RotateLocked() {
  FileState& s = State();
  if (s.fp != nullptr) {
    std::fclose(s.fp);
    s.fp = nullptr;
  }
  s.bytes = 0;
  ComputePathLocked();
#ifdef _WIN32
  const std::wstring oldPath = s.pathWide + L".old";
  _wremove(oldPath.c_str());
  _wrename(s.pathWide.c_str(), oldPath.c_str());
  s.fp = _wfopen(s.pathWide.c_str(), L"wb");
#else
  const std::string oldPath = s.pathUtf8 + ".old";
  std::remove(oldPath.c_str());
  std::rename(s.pathUtf8.c_str(), oldPath.c_str());
  s.fp = std::fopen(s.pathUtf8.c_str(), "wb");
#endif
}

// 当前本地时间 "YYYY-MM-DD HH:MM:SS.mmm"
inline void FormatTimestamp(char* buf, size_t bufSize) {
#ifdef _WIN32
  SYSTEMTIME st;
  GetLocalTime(&st);
  std::snprintf(buf, bufSize, "%04u-%02u-%02u %02u:%02u:%02u.%03u",
                (unsigned)st.wYear, (unsigned)st.wMonth, (unsigned)st.wDay,
                (unsigned)st.wHour, (unsigned)st.wMinute, (unsigned)st.wSecond,
                (unsigned)st.wMilliseconds);
#else
  struct timeval tv;
  // gettimeofday 为 POSIX 全局命名空间函数（std::gettimeofday 非标准，macOS 工具链不提供）
  gettimeofday(&tv, nullptr);
  struct tm tmv;
  localtime_r(&tv.tv_sec, &tmv);
  std::snprintf(buf, bufSize, "%04d-%02d-%02d %02d:%02d:%02d.%03d",
                tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
                tmv.tm_hour, tmv.tm_min, tmv.tm_sec, (int)(tv.tv_usec / 1000));
#endif
}

// 当前线程短标识（仅用于区分并发日志来源；Windows 用系统线程 id）
inline unsigned CurrentThreadId() {
#ifdef _WIN32
  return (unsigned)GetCurrentThreadId();
#else
  const size_t h = std::hash<std::thread::id>()(std::this_thread::get_id());
  return (unsigned)(h & 0xFFFFFFFFu);
#endif
}

// 当前进程 id（会话起始行会打印，便于区分宿主与测试进程）
inline unsigned ProcessId() {
#ifdef _WIN32
  return (unsigned)GetCurrentProcessId();
#else
  return (unsigned)getpid();
#endif
}

// 应用环境变量 ZTOOLS_LOG_LEVEL（trace/debug/info/warn/error/off，大小写不敏感）。
// 仅应用一次；JS 侧已显式 SetLevel 时跳过。须在 FileMutex 保护下调用。
inline void ApplyEnvLevelLocked() {
  if (EnvApplied() || LevelExplicit().load(std::memory_order_relaxed)) {
    return;
  }
  EnvApplied() = true;
  const char* value = std::getenv("ZTOOLS_LOG_LEVEL");
  if (value == nullptr || value[0] == '\0') {
    return;
  }
  Level parsed = kInfo;
  if (LevelFromString(value, &parsed)) {
    LevelRef().store(parsed, std::memory_order_relaxed);
  }
}

}  // namespace internal

// ==================== 公共 API ====================

// 消息等级是否达到当前输出等级（供调用方跳过昂贵的参数构造）
inline bool Enabled(Level level) {
  return level >= internal::LevelRef().load(std::memory_order_relaxed);
}

// 运行时设置输出等级（JS 侧 setLogLevel 导出）；设置后环境变量不再生效
inline void SetLevel(Level level) {
  internal::LevelExplicit().store(true, std::memory_order_relaxed);
  internal::LevelRef().store(level, std::memory_order_relaxed);
}

// 当前输出等级
inline Level GetLevel() {
  return (Level)internal::LevelRef().load(std::memory_order_relaxed);
}

// 日志文件完整路径（UTF-8）。路径在首次调用时确定并缓存；不打开文件。
inline std::string FilePath() {
  std::lock_guard<std::mutex> lock(internal::FileMutex());
  internal::ComputePathLocked();
  return internal::State().pathUtf8;
}

// 模块加载时调用一次：应用 ZTOOLS_LOG_LEVEL 环境变量（未设置则保持默认 info）
inline void InitFromEnv() {
  std::lock_guard<std::mutex> lock(internal::FileMutex());
  internal::ApplyEnvLevelLocked();
}

// 写一条日志（线程安全，任意 native 线程可调用）。
// level 低于当前输出等级时直接丢弃；fmt 为 printf 风格；
// tag 建议用短模块名（如 "clipboard" / "screenshot" / "longcapture"）。
inline void Write(Level level, const char* tag, const char* fmt, ...) ZTOOLS_LOG_PRINTF_ATTR;

inline void Write(Level level, const char* tag, const char* fmt, ...) {
  if (!Enabled(level)) {
    return;
  }

  // 先格式化消息体（无锁；等级过滤已挡掉绝大部分调用）
  char message[internal::kMaxMessageBytes];
  va_list args;
  va_start(args, fmt);
  int n = std::vsnprintf(message, sizeof(message), fmt, args);
  va_end(args);
  if (n < 0) {
    return;
  }
  if ((size_t)n >= sizeof(message)) {
    n = (int)sizeof(message) - 1;  // 超长防御性截断
  }

  char ts[40];
  internal::FormatTimestamp(ts, sizeof(ts));

  char line[internal::kMaxMessageBytes + 128];
  int lineLen = std::snprintf(line, sizeof(line), "%s [%s] [tid %u] [%s] %.*s\n", ts,
                              LevelToString(level), internal::CurrentThreadId(),
                              tag != nullptr ? tag : "native", n, message);
  if (lineLen < 0) {
    return;
  }
  if ((size_t)lineLen >= sizeof(line)) {
    lineLen = (int)sizeof(line) - 1;
  }

  std::lock_guard<std::mutex> lock(internal::FileMutex());
  internal::ApplyEnvLevelLocked();
  internal::FileState& s = internal::State();
  if (!internal::OpenFileLocked()) {
    if (!s.openFailedReported) {
      s.openFailedReported = true;
      std::fprintf(stderr, "[ztools-native] failed to open log file: %s\n",
                   s.pathUtf8.c_str());
    }
    return;
  }
  if (s.bytes + (uint64_t)lineLen > internal::kMaxFileBytes) {
    internal::RotateLocked();
    if (s.fp == nullptr) {
      return;
    }
  }
  const size_t toWrite = (size_t)lineLen;
  if (std::fwrite(line, 1, toWrite, s.fp) == toWrite) {
    s.bytes += toWrite;
    // 逐条落盘：原生层崩溃时已写内容不丢；本模块日志量低，开销可接受
    std::fflush(s.fp);
  }
}

// 当前进程 id（公共 API，供绑定层打会话起始行）
inline unsigned ProcessId() {
  return internal::ProcessId();
}

}  // namespace ztools_log

// ==================== 日志宏（推荐调用形式） ====================

#define ZLOG_TRACE(tag, ...) ::ztools_log::Write(::ztools_log::kTrace, (tag), __VA_ARGS__)
#define ZLOG_DEBUG(tag, ...) ::ztools_log::Write(::ztools_log::kDebug, (tag), __VA_ARGS__)
#define ZLOG_INFO(tag, ...) ::ztools_log::Write(::ztools_log::kInfo, (tag), __VA_ARGS__)
#define ZLOG_WARN(tag, ...) ::ztools_log::Write(::ztools_log::kWarn, (tag), __VA_ARGS__)
#define ZLOG_ERROR(tag, ...) ::ztools_log::Write(::ztools_log::kError, (tag), __VA_ARGS__)

#endif  // ZTOOLS_LOGGER_H_
