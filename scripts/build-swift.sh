#!/bin/bash

set -e

echo "🔨 Building Swift dynamic library (Universal Binary)..."

# 进入项目根目录
cd "$(dirname "$0")/.."

# 创建 lib 目录
mkdir -p lib

# 检测当前架构
ARCH=$(uname -m)
echo "📱 当前架构: $ARCH"

# 构建 Universal Binary（同时支持 arm64 和 x86_64）
echo "🔧 Building arm64 version..."
swiftc -emit-library \
  -o lib/libZToolsNative_arm64.dylib \
  src/ZToolsNative.swift \
  -framework Cocoa \
  -target arm64-apple-macosx11.0 \
  -Osize

echo "🔧 Building x86_64 version..."
swiftc -emit-library \
  -o lib/libZToolsNative_x86_64.dylib \
  src/ZToolsNative.swift \
  -framework Cocoa \
  -target x86_64-apple-macosx10.15 \
  -Osize

echo "🔗 Creating Universal Binary..."
lipo -create \
  lib/libZToolsNative_arm64.dylib \
  lib/libZToolsNative_x86_64.dylib \
  -output lib/libZToolsNative.dylib

# 清理临时文件
rm lib/libZToolsNative_arm64.dylib lib/libZToolsNative_x86_64.dylib

# 验证 Universal Binary
echo "✅ Swift library built successfully: lib/libZToolsNative.dylib"
lipo -info lib/libZToolsNative.dylib

