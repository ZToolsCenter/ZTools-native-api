#!/bin/bash

set -e

echo "🔨 Building Swift dynamic library (Universal Binary)..."

# 进入项目根目录
cd "$(dirname "$0")/.."

# 创建 lib 目录
mkdir -p lib

# 前置：生成图标资源（icon_svgs.swift 是 SWIFT_SOURCES 之一，缺失会导致 swiftc 失败；
# 脚本幂等，npm run build 链路中会重复执行但开销可忽略）
node scripts/gen-icons.js

# 检测当前架构
ARCH=$(uname -m)
echo "📱 当前架构: $ARCH"

# 长截图算法层 C++ 对象（lc_match_core + lc_stitch_state
# 纯算法层与 lc_bridge C ABI shim，clang++ 编出 .o 后随 swiftc -emit-library 链入
# dylib；Swift 侧经 src/screenshot/macos/LCBridgeMac.swift 的 @_silgen_name 调用。
# 注意：本段与 .github/workflows/build.yml「Build Swift library」步骤内联实现逐字一致
# （两处编译参数必须同步修改）。Apple clang 的 C++ 不支持 -Osize，取等价的 -Oz。
# -I：算法层头文件目录（lc_bridge_mac.cpp 位于 macos/，需检索 ../algo 的
# long_capture_internal.h / lc_platform.h）。
LC_CXX_SOURCES=(
  src/screenshot/algo/lc_match_core.cpp
  src/screenshot/algo/lc_stitch_state.cpp
  src/screenshot/macos/lc_bridge_mac.cpp
)
LC_CXX_FLAGS=(-c -std=c++17 -Oz -DNDEBUG -I src/screenshot/algo)

echo "🔧 Compiling long-capture algorithm layer objects (arm64)..."
mkdir -p build/lc-obj-arm64
LC_OBJS_ARM64=()
for src in "${LC_CXX_SOURCES[@]}"; do
  obj="build/lc-obj-arm64/$(basename "${src%.cpp}").o"
  clang++ "${LC_CXX_FLAGS[@]}" -arch arm64 -mmacosx-version-min=11.0 -o "$obj" "$src"
  LC_OBJS_ARM64+=("$obj")
done

echo "🔧 Compiling long-capture algorithm layer objects (x86_64)..."
mkdir -p build/lc-obj-x86_64
LC_OBJS_X86_64=()
for src in "${LC_CXX_SOURCES[@]}"; do
  obj="build/lc-obj-x86_64/$(basename "${src%.cpp}").o"
  clang++ "${LC_CXX_FLAGS[@]}" -arch x86_64 -mmacosx-version-min=10.15 -o "$obj" "$src"
  LC_OBJS_X86_64+=("$obj")
done

# 构建 Universal Binary（同时支持 arm64 和 x86_64）
# Swift 源文件清单（多文件单模块编译；新增截图等模块文件时必须同步修改
# .github/workflows/build.yml 中内联的 swiftc 步骤，两处保持一致）
SWIFT_SOURCES=(
  src/ZToolsNative.swift
  src/screenshot/macos/ScreenshotMac.swift
  src/screenshot/macos/ScreenshotOverlayMac.swift
  src/screenshot/macos/ScreenshotPaintMac.swift
  src/screenshot/macos/ScreenshotAnnotationsMac.swift
  src/screenshot/macos/ScreenshotToolbarMac.swift
  src/screenshot/macos/ScreenshotTextMac.swift
  src/screenshot/macos/ScreenshotMosaicMac.swift
  src/screenshot/macos/ScreenshotOutputMac.swift
  src/screenshot/macos/ScreenshotLongCaptureMac.swift
  src/screenshot/macos/ScreenshotLCPanelMac.swift
  src/screenshot/macos/ScreenshotLCToolbarMac.swift
  src/screenshot/macos/LCBridgeMac.swift
  src/generated/icon_svgs.swift
)

echo "🔧 Building arm64 version..."
swiftc -emit-library \
  -o lib/libZToolsNative_arm64.dylib \
  "${SWIFT_SOURCES[@]}" \
  "${LC_OBJS_ARM64[@]}" \
  -framework Cocoa \
  -lc++ \
  -target arm64-apple-macosx11.0 \
  -Osize

echo "🔧 Building x86_64 version..."
swiftc -emit-library \
  -o lib/libZToolsNative_x86_64.dylib \
  "${SWIFT_SOURCES[@]}" \
  "${LC_OBJS_X86_64[@]}" \
  -framework Cocoa \
  -lc++ \
  -target x86_64-apple-macosx10.15 \
  -Osize

echo "🔗 Creating Universal Binary..."
lipo -create \
  lib/libZToolsNative_arm64.dylib \
  lib/libZToolsNative_x86_64.dylib \
  -output lib/libZToolsNative.dylib

# 清理临时文件
rm lib/libZToolsNative_arm64.dylib lib/libZToolsNative_x86_64.dylib
rm -rf build/lc-obj-arm64 build/lc-obj-x86_64

# 验证 Universal Binary
echo "✅ Swift library built successfully: lib/libZToolsNative.dylib"
lipo -info lib/libZToolsNative.dylib
