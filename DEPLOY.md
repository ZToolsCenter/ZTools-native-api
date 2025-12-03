# 部署指南

## 文件说明

本项目编译后会生成两个关键文件：

1. **`build/Release/ztools_native.node`** - Node.js 原生插件（C++ binding）
2. **`lib/libZToolsNative.dylib`** - Swift 动态库

## 部署到其他项目

### 方式 1：放在同一目录（推荐）✅

将两个文件放在**同一目录**下：

```
your-project/
├── resources/
│   └── lib/
│       └── mac/
│           ├── ztools_native.node
│           └── libZToolsNative.dylib    ��� 必须在同一目录
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
// 生产环��（打包后）
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

### macOS 权限（可选）

部分功能可能需要：
- **辅助功能权限**：窗口监控功能
- **屏幕录制权限**：某些窗口信息获取

授权方式：
```
系统设置 → 隐私与安全性 → 辅助功能
```

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
