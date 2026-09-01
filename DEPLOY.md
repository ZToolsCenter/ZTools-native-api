# 部署指南

## 文件说明

本项目编译后会生成两个关键文件：

1. **`build/Release/ztools_native.node`** - Node.js 原生插件（C++ binding）
2. **`lib/libZToolsNative.dylib`** - Swift 动态库（Universal Binary，arm64 + x86_64）

> 结构说明（macOS 截图子系统合入后核对）：长截图匹配算法层（`lc_match_core.cpp` /
> `lc_stitch_state.cpp`）与 C ABI shim（`lc_bridge_mac.cpp`）在构建期由 clang++ 编出 .o 后随
> `swiftc -emit-library` **静态链入 dylib 内部**，不产生额外文件——部署产物仍然只有上面
> 两个文件，无需调整打包清单。

## 部署到其他项目

### 方式 1：放在同一目录（推荐）✅

将两个文件放在**同一目录**下：

```
your-project/
├── resources/
│   └── lib/
│       └── mac/
│           ├── ztools_native.node
│           └── libZToolsNative.dylib    ← 必须在同一目录
```

**使用方法：**
```javascript
const addon = require('./resources/lib/mac/ztools_native.node');
```

**优点：**
- ✅ 路径简单明了
- ✅ 部署最可靠
- ✅ 支持 Electron 打包

---

### 方式 2：使用标准目录结构

```
your-project/
├── node_modules/
│   └── ztools-native-api/
│       ├── build/Release/
│       │   └── ztools_native.node
│       └── lib/
│           └── libZToolsNative.dylib
```

通过 npm 安装会自动使用这种结构。

---

## 路径查找顺序

.node 文件会按以下顺序查找 .dylib 文件：

1. ✅ `.node 文件所在目录`（最优先）
2. `.node 文件所在目录/../lib/`
3. `./lib/libZToolsNative.dylib`（当前工作目录）
4. `./libZToolsNative.dylib`
5. `../lib/libZToolsNative.dylib`

---

## Electron 打包示例

### 使用 electron-builder

在 `package.json` 中配置：

```json
{
  "build": {
    "extraResources": [
      {
        "from": "node_modules/ztools-native-api/build/Release/ztools_native.node",
        "to": "lib/mac/ztools_native.node"
      },
      {
        "from": "node_modules/ztools-native-api/lib/libZToolsNative.dylib",
        "to": "lib/mac/libZToolsNative.dylib"
      }
    ]
  }
}
```

### 运行时加载

```javascript
const path = require('path');
const { app } = require('electron');

// 开发环境
if (process.env.NODE_ENV === 'development') {
  const addon = require('ztools-native-api/build/Release/ztools_native.node');
}
// 生产环境（打包后）
else {
  const resourcePath = process.resourcesPath;
  const addonPath = path.join(resourcePath, 'lib/mac/ztools_native.node');
  const addon = require(addonPath);
}
```

---

## 故障排查

### 错误：`Failed to load Swift library`

**原因：** 找不到 `libZToolsNative.dylib` 文件

**解决方案：**

1. **检查文件是否存在：**
   ```bash
   ls -la path/to/ztools_native.node
   ls -la path/to/libZToolsNative.dylib
   ```

2. **确保两个文件在同一目录：**
   ```
   ✅ lib/mac/ztools_native.node
   ✅ lib/mac/libZToolsNative.dylib
   ```

3. **查看详细错误信息：**

   最新版本的错误信息会显示尝试的所有路径：
   ```
   Failed to load Swift library.
   Module directory: /path/to/your/app/lib/mac
   Tried paths:
     - /path/to/your/app/lib/mac/libZToolsNative.dylib
     - /path/to/your/app/lib/libZToolsNative.dylib
     - ...
   ```

---

## 权限说明

### macOS 权限

按功能需要授予（系统设置 → 隐私与安全性）：

- **辅助功能权限**：窗口监控、键盘模拟；截图功能的 ESC/右键兜底取消、长截图滚轮观察与自动滚动（未授权时截图仍可用，兜底能力降级）
- **屏幕录制权限**：区域截图/长截图（`ScreenCapture.start()` 首次调用会弹系统授权框，授权后可能需重启宿主进程；`prime()` 预检未授权时直接返回 false，不弹框）

授权方式：
```
系统设置 → 隐私与安全性 → 辅助功能 / 屏幕录制
```

### macOS 屏幕共享 / 远程桌面注意事项

截图捕获底层使用 `CGWindowListCreateImage`（macOS 14 起被系统标记 deprecated，目前仍可用；
升级 ScreenCaptureKit 的评估见 `docs/SCK_UPGRADE_EVALUATION.md`）。其内容来自 WindowServer
的合成结果，在屏幕共享 / 远程桌面会话（如 Safari 网页共享屏幕、macOS「屏幕共享」、第三方
远程桌面）下，合成路径与本地会话存在差异，且此类环境无法被 CI 覆盖——发布前建议开启屏幕
共享后人工回归以下场景：

- 权限预检与授权框弹出行为（`prime()` 不弹框、`start()` 弹框）
- 整屏底图与选区裁剪正确性（多屏 + Retina）
- 放大镜取色与坐标/HEX/RGB 显示
- 长截图滚动拼接正确率与到底判定
- ESC 兜底取消（覆盖层失焦时）

与 Windows 版的 RDP 场景对应：Windows 侧已针对 RDP 鼠标事件节流/合并做了实时命中测试
（不依赖 hover 缓存，见 `src/screenshot/overlay_input_windows.cpp` 注释）；macOS 侧交互为
NSView 事件 + 实时命中、同样不依赖 hover 缓存，具备对应鲁棒性，仍建议按上表人工回归。

---

## 完整示例

```javascript
const { ClipboardMonitor, WindowMonitor, WindowManager } = require('./lib/mac/ztools_native.node');

// 获取当前窗口
const activeWindow = WindowManager.getActiveWindow();
console.log(activeWindow); // { appName: 'Chrome', bundleId: 'com.google.Chrome' }

// 监听窗口切换
const windowMonitor = new WindowMonitor();
windowMonitor.start((info) => {
  console.log(`切换到: ${info.appName}`);
});

// 监听剪贴板
const clipboardMonitor = new ClipboardMonitor();
clipboardMonitor.start(() => {
  console.log('剪贴板已变化');
});

// 停止监听
windowMonitor.stop();
clipboardMonitor.stop();
```

---

## 技术细节

- **编译环境：** macOS + Xcode
- **Swift 版本：** 5.x
- **Node.js 版本：** >= 16.0.0
- **架构支持：** arm64 (Apple Silicon), x86_64 (Intel)

---

如有问题，请查看项目 Issues 或提交新 Issue。
