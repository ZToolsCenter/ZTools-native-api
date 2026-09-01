#pragma once

#include <napi.h>
#include <windows.h>

// 区域截图入口（在 Init 中注册为 "startRegionCapture" 导出）
Napi::Value StartRegionCapture(const Napi::CallbackInfo& info);
Napi::Value PrimeScreenshotFrame(const Napi::CallbackInfo& info);
Napi::Value StartRegionCaptureWithPrimedFrame(const Napi::CallbackInfo& info);
// 中止进行中的长截图滚动捕获（在 Init 中注册为 "abortLongCapture" 导出）
Napi::Value AbortLongCapture(const Napi::CallbackInfo& info);

// 供其他原生模块在截图触发前预抓取首帧。
bool PrimeScreenshotFrameNow();

// 获取 PNG 编码器 CLSID（定义在 screenshot/output_windows.cpp，截图模块与 binding 应用图标模块共用）
int GetPngEncoderClsid(CLSID* pClsid);
