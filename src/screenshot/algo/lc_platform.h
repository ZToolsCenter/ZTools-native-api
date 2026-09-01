// 长截图算法层平台兼容头（依赖剥离验证产物）。
//
// 用途：lc_match_core.cpp / lc_stitch_state.cpp（纯算法层）与 long_capture_internal.h
// 在非 Windows 平台（macOS，经 lc_bridge C ABI 链入 Swift dylib）编译时，替代
// internal.h 的 windows.h / napi / GDI+ 依赖链。_WIN32 下本头等价于 #include <windows.h>
// （透传，原行为超集）；Windows 构建路径不引用本头（两算法 .cpp 的 _WIN32 分支仍走
// internal.h），Windows 侧行为零影响。
//
// 依赖剥离验证结论（逐符号 grep + 人工核对，2026-08）：
//   · 类型别名：DWORD / BYTE（算法层常量与 LongCaptureContext.stableRefTick 使用）
//   · 宏：WHEEL_DELTA = 120（滚轮先验换算，winuser.h 同名宏）
//   · 真实平台 API：仅 GetTickCount()（lc_stitch_state.cpp 稳定性闸门时间戳，2 处调用）
//     —— 唯一非类型依赖，语义 = 单调毫秒时钟（uint32 自然回绕），非 Windows 用
//     clock_gettime(CLOCK_MONOTONIC) 等价实现（见 LcPlatformTickMs），算法逻辑零改动
//   · 仅 LC_DEBUG_LOG 调试分支：OutputDebugStringA（默认不编译；非 Windows 退化为 stderr）
//   · 声明专用类型：HWND / HDC / HBITMAP / RECT / LRESULT / CALLBACK / UINT / WPARAM /
//     LPARAM —— 只出现在 long_capture_internal.h 的 IO/UI 函数声明中，算法层从不调用，
//     给出占位定义仅满足类型完整性，不参与任何算法行为
//   · CaptureContext（编辑态会话上下文，含 GDI+ 成员）：仅以指针出现在 IO/UI 声明中，
//     非平台由使用方前向声明，无需完整定义
#ifndef LC_PLATFORM_H
#define LC_PLATFORM_H

#ifdef _WIN32

// Windows：透传原头（本头在 _WIN32 下必须保持原行为的严格超集）
#include <windows.h>

#else

#include <cstdint>
#include <cstdio>
#include <ctime>

// ---- Win32 基本整型别名（宽度与 Windows 侧逐一对齐：DWORD/UINT 为 32 位）----
typedef uint8_t  BYTE;
typedef uint16_t WORD;
typedef uint32_t DWORD;
typedef unsigned int UINT;
typedef int32_t  LONG;
typedef int      BOOL;

// ---- GDI/USER 句柄与几何结构（仅满足 IO/UI 函数声明的类型完整性；算法层不解引用）----
typedef void* HWND;
typedef void* HDC;
typedef void* HBITMAP;
typedef struct { int32_t left, top, right, bottom; } RECT;
typedef struct { int32_t x, y; } POINT;
typedef uint32_t COLORREF;

// ---- 窗口过程签名类型（声明专用）----
typedef intptr_t  LONG_PTR;
typedef uintptr_t UINT_PTR;
typedef UINT_PTR  WPARAM;
typedef LONG_PTR  LPARAM;
typedef LONG_PTR  LRESULT;
#define CALLBACK

// ---- 滚轮增量单位（winuser.h 同名宏，值 120；滚轮先验换算用）----
#ifndef WHEEL_DELTA
#define WHEEL_DELTA 120
#endif

// 非 Windows 平台的 GetTickCount 等价物：单调毫秒时钟（CLOCK_MONOTONIC，系统启动
// 以来计数；uint32 毫秒自然回绕，与 Win32 语义一致）。唯一调用方为
// lc_stitch_state.cpp 的稳定性闸门（LC_STABLE_REF_MAX_GAP = 600ms 量级的新鲜度判定），
// 任一单调毫秒时钟语义等价；macOS 的 CLOCK_MONOTONIC 不计入系统睡眠，对该用途无影响。
inline uint32_t LcPlatformTickMs() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u);
}
inline uint32_t GetTickCount() { return LcPlatformTickMs(); }

// 调试日志兼容（仅 LC_DEBUG_LOG 构建被调用；Windows 上经 OutputDebugStringA 输出到
// 调试器，非 Windows 退化为 stderr，供 macOS 侧用真实数据做阈值调优时启用）。
inline void OutputDebugStringA(const char* s) { fputs(s, stderr); }

#endif // _WIN32

#endif // LC_PLATFORM_H
