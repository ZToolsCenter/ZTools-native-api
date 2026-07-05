#pragma once

#include <napi.h>
#include <windows.h>

// 区域截图入口（在 Init 中注册为 "startRegionCapture" 导出）
Napi::Value StartRegionCapture(const Napi::CallbackInfo& info);
Napi::Value PrimeScreenshotFrame(const Napi::CallbackInfo& info);
Napi::Value StartRegionCaptureWithPrimedFrame(const Napi::CallbackInfo& info);

// 供其他原生模块在截图触发前预抓取首帧。
bool PrimeScreenshotFrameNow();

// 获取 PNG 编码器 CLSID
// 实际定义在 binding_windows.cpp（应用图标提取模块也在使用），截图模块复用
int GetPngEncoderClsid(CLSID* pClsid);
