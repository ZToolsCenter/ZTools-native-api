# ZTools Native API

macOS 和 Windows 原生 API 的 Node.js 封装，使用 Swift + Win32 API + Node-API (N-API) 实现。

## ✨ 功能

1. **剪贴板变动监控** - 实时监听剪贴板内容变化
2. **窗口激活监控** - 实时监听窗口切换事件
3. **获取当前窗口** - 获取当前激活窗口的应用名和标识符
4. **设置激活窗口** - 根据标识符激活指定应用
5. **键盘模拟** - 模拟键盘按键和快捷键（支持修饰键）
6. **粘贴模拟** - 模拟 Cmd+V (macOS) / Ctrl+V (Windows)
7. **区域截图** - 选区截图并自动保存到剪贴板（Windows）
8. **获取选中内容** - 获取当前选中的文本、文件或图像（支持 Cursor/VS Code 等编辑器）
9. **鼠标监控** - 实时监听鼠标移动、点击事件
10. **鼠标模拟** - 模拟鼠标移动、点击操作
11. **取色器** - 全屏取色工具

## 🔧 系统要求

### macOS
- macOS 10.15+
- Node.js 16.0+
- Swift 5.0+
- Xcode Command Line Tools

### Windows
- Windows 10+
- Node.js 16.0+
- Visual Studio Build Tools 或 Visual Studio 2019+

## 📦 安装

```bash
npm install
npm run build
```

## 🚀 使用方法

### 基础示例

```javascript
const { ClipboardMonitor, WindowMonitor, WindowManager } = require('ztools-native-api');

// 1. 剪贴板变动监控（跨平台一致）
const clipboardMonitor = new ClipboardMonitor();
clipboardMonitor.start(() => {
  console.log('剪贴板变化了！');
});

// 停止监控
clipboardMonitor.stop();

// 2. 窗口激活监控（实时监听窗口切换）
const windowMonitor = new WindowMonitor();
windowMonitor.start((windowInfo) => {
  console.log('窗口切换:', windowInfo);
  // macOS => { appName: 'Safari', bundleId: 'com.apple.Safari' }
  // Windows => { appName: 'chrome', processId: 12345 }
});

// 停止监控
windowMonitor.stop();

// 3. 获取当前激活窗口
const activeWindow = WindowManager.getActiveWindow();
console.log(activeWindow);
// macOS => { appName: '终端', bundleId: 'com.apple.Terminal' }
// Windows => { appName: 'chrome', processId: 12345 }

// 3. 激活指定窗口
// macOS: 使用 bundleId (string)
WindowManager.activateWindow('com.apple.Safari');

// Windows: 使用 processId (number)
WindowManager.activateWindow(12345);

// 5. 模拟键盘按键
// 基本用法：输入单个字母
WindowManager.simulateKeyboardTap('a');

// 使用修饰键：输入大写字母
WindowManager.simulateKeyboardTap('a', 'shift');

// 模拟快捷键：Cmd+C (macOS) / Ctrl+C (Windows)
const modifier = process.platform === 'darwin' ? 'meta' : 'ctrl';
WindowManager.simulateKeyboardTap('c', modifier);

// 使用多个修饰键：Cmd+Shift+S (macOS) / Ctrl+Shift+S (Windows)
WindowManager.simulateKeyboardTap('s', modifier, 'shift');

// 特殊键：Enter、Tab、方向键等
WindowManager.simulateKeyboardTap('return');
WindowManager.simulateKeyboardTap('tab');
WindowManager.simulateKeyboardTap('left');

// 6. 模拟粘贴操作
WindowManager.simulatePaste();

// 7. 区域截图（仅 Windows）
const { ScreenCapture } = require('ztools-native-api');

ScreenCapture.start((result) => {
  if (result.success) {
    console.log(`截图成功！尺寸: ${result.width} x ${result.height}`);
    console.log('截图已保存到剪贴板，可按 Ctrl+V 粘贴');
  } else {
    console.log('截图已取消');
  }
});
// 操作：拖拽选择区域后释放鼠标，或按 ESC 取消

// 8. 获取选中内容（支持文本、文件、图像）
const { getSelectedContent } = require('ztools-native-api');

// 在任意应用中选中内容后调用
const contents = getSelectedContent();
contents.forEach(item => {
  switch (item.type) {
    case 'text':
      console.log('选中的文本:', item.data);
      break;
    case 'file':
      console.log('选中的文件:', item.data);
      break;
    case 'image':
      console.log('选中的图像 (base64):', item.data.substring(0, 50) + '...');
      break;
  }
});
```

### 跨平台兼容示例

```javascript
const { WindowManager } = require('ztools-native-api');

// 获取当前窗口
const current = WindowManager.getActiveWindow();

// 跨平台激活窗口
if (WindowManager.getPlatform() === 'darwin') {
  // macOS
  WindowManager.activateWindow('com.apple.Safari');
} else if (WindowManager.getPlatform() === 'win32') {
  // Windows - 使用之前获取的 processId
  WindowManager.activateWindow(current.processId);
}
```

## 📖 API

### `ClipboardMonitor`

#### `start(callback)`
启动剪贴板监控
- **参数**: `callback()` - 剪贴板变化时的回调函数（无参数，只通知变化事件）
- **跨平台**: ✅ 一致

#### `stop()`
停止剪贴板监控
- **跨平台**: ✅ 一致

#### `isMonitoring`
只读属性，是否正在监控
- **跨平台**: ✅ 一致

---

### `WindowMonitor`

#### `start(callback)`
启动窗口激活监控
- **参数**: `callback(windowInfo)` - 窗口切换时的回调函数
  - **macOS**: `{appName: string, bundleId: string}`
  - **Windows**: `{appName: string, processId: number}`
- **跨平台**: ✅ API一致，返回值字段不同

#### `stop()`
停止窗口监控
- **跨平台**: ✅ 一致

#### `isMonitoring`
只读属性，是否正在监控
- **跨平台**: ✅ 一致

---

### `WindowManager`

#### `WindowManager.getActiveWindow()`
获取当前激活窗口
- **返回值**:
  - **macOS**: `{appName: string, bundleId: string} | null`
  - **Windows**: `{appName: string, processId: number} | null`

**示例**:
```javascript
// macOS
{ appName: 'Safari', bundleId: 'com.apple.Safari' }

// Windows
{ appName: 'chrome', processId: 12345 }
```

#### `WindowManager.activateWindow(identifier)`
激活指定应用窗口
- **参数**:
  - **macOS**: `bundleId` (string) - Bundle Identifier
  - **Windows**: `processId` (number) - 进程 ID
- **返回**: `boolean` - 是否激活成功

**示例**:
```javascript
// macOS
WindowManager.activateWindow('com.apple.Safari');

// Windows
WindowManager.activateWindow(12345);
```

#### `WindowManager.getPlatform()`
获取当前平台
- **返回**: `'darwin' | 'win32'`

#### `WindowManager.simulateKeyboardTap(key, ...modifiers)`
模拟键盘按键
- **参数**:
  - `key` (string) - 要按的键（如 'a', 'return', 'tab', 'left' 等）
  - `...modifiers` (string[]) - 修饰键（可选），支持 'shift', 'ctrl', 'alt', 'meta'
- **返回**: `boolean` - 是否成功
- **跨平台**: ✅ 一致（'meta' 在 macOS 上是 Command，在 Windows 上是 Win 键）

**支持的按键**:
- 字母: `a-z`
- 数字: `0-9`
- 功能键: `f1-f12`
- 特殊键: `return/enter`, `tab`, `space`, `backspace`, `delete`, `escape/esc`
- 方向键: `left`, `right`, `up`, `down`
- 符号键: `-`, `=`, `[`, `]`, `\`, `;`, `'`, `,`, `.`, `/`, `` ` ``

**支持的修饰键**:
- `shift` - Shift 键
- `ctrl/control` - Control 键
- `alt` - Alt 键（macOS 上是 Option）
- `meta` - Command 键（macOS）/ Windows 键（Windows）

**示例**:
```javascript
// 输入字母
WindowManager.simulateKeyboardTap('a');

// 输入大写字母（Shift + A）
WindowManager.simulateKeyboardTap('a', 'shift');

// 复制（Cmd+C / Ctrl+C）
const mod = process.platform === 'darwin' ? 'meta' : 'ctrl';
WindowManager.simulateKeyboardTap('c', mod);

// 多个修饰键（Cmd+Shift+S）
WindowManager.simulateKeyboardTap('s', mod, 'shift');

// 特殊键
WindowManager.simulateKeyboardTap('return');  // Enter
WindowManager.simulateKeyboardTap('tab');     // Tab
WindowManager.simulateKeyboardTap('left');    // 左方向键
```

**注意事项**:
- **macOS**: 需要授予"辅助功能"权限（首次调用时会提示）
- **Windows**: 无需特殊权限
- 建议在调用前确保目标窗口已激活

#### `WindowManager.simulatePaste()`
模拟粘贴操作（Cmd+V / Ctrl+V）
- **返回**: `boolean` - 是否成功
- **跨平台**: ✅ 一致

**示例**:
```javascript
WindowManager.simulatePaste();
```

---

### `ScreenCapture`

#### `ScreenCapture.start(callback)`
启动区域截图（仅 Windows）
- **参数**: `callback(result)` - 截图完成时的回调函数
  - `result.success` (boolean) - 是否成功截图
  - `result.width` (number) - 截图宽度（成功时）
  - `result.height` (number) - 截图高度（成功时）
- **平台**: ⚠️ 仅支持 Windows

**功能说明**：
- 调用后会创建全屏半透明黑色遮罩
- 鼠标变为十字光标
- 拖拽鼠标选择截图区域
- 释放鼠标后自动截图并保存到剪贴板
- 按 ESC 键可取消截图

**示例**:
```javascript
ScreenCapture.start((result) => {
  if (result.success) {
    console.log(`截图成功！尺寸: ${result.width}x${result.height}`);
    // 截图已在剪贴板中，可按 Ctrl+V 粘贴
  } else {
    console.log('截图已取消');
  }
});
```

---

### `getSelectedContent()`

#### `getSelectedContent()`
获取当前选中的内容（支持文本、文件、图像）
- **返回值**: `Array<{type: string, data: any}>` - 选中内容数组
  - `type`: 'text' | 'file' | 'image'
  - `data`: 根据类型不同：
    - text: 字符串
    - file: 文件路径字符串数组
    - image: base64 编码的 PNG 图像（带 format 和 encoding 字段）
- **平台**: ✅ Windows 和 macOS

**功能说明**：
- **Windows**: 优先使用 UI Automation API，回退到剪贴板方法
  - 适用于标准 Windows 控件和 Electron/Chromium 应用（Cursor、VS Code 等）
- **macOS**: 使用模拟复制方法（Cmd+C）
- 自动暂停内部的 clipboardMonitor，防止误触发监听自身发起的事件
- 操作后会恢复原剪贴板内容

**示例**:
```javascript
const { getSelectedContent } = require('ztools-native-api');

// 在用户选中内容后调用
const contents = getSelectedContent();

contents.forEach((item, index) => {
  console.log(`[${index + 1}] 类型: ${item.type}`);
  
  switch (item.type) {
    case 'text':
      console.log('文本内容:', item.data);
      console.log('文本长度:', item.data.length);
      break;
      
    case 'file':
      console.log('文件列表:');
      item.data.forEach((path, i) => {
        console.log(`  ${i + 1}. ${path}`);
      });
      break;
      
    case 'image':
      console.log('图像格式:', item.format);  // 'png'
      console.log('编码方式:', item.encoding); // 'base64'
      console.log('数据长度:', item.data.length);
      // 可以直接用于 <img src="data:image/png;base64,..." />
      break;
  }
});
```

**支持的应用**:
- ✅ Windows: 记事本、Word、Excel、Edge、Chrome、Firefox、VS Code、Notepad++、**Cursor**、**任何 Electron 应用**
- ✅ macOS: 所有支持标准复制快捷键（Cmd+C）的应用
- ✅ 支持文件资源管理器/Finder 中选中的文件
- ✅ 支持图像编辑器中选中的图像区域


## 🧪 测试

```bash
npm test

# 或运行特定测试
node test/test-keyboard.js         # 完整键盘测试
node test/test-keyboard-simple.js  # 简单键盘测试
node test/test-selected-content.js # 获取选中内容测试
```

## ⚠️ 平台差异

| 特性 | macOS | Windows |
|-----|-------|---------|
| **窗口标识符** | Bundle ID (稳定，如 `com.apple.Safari`) | Process ID (动态变化，如 `12345`) |
| **激活限制** | 较宽松 | 严格（需要线程附加 hack） |
| **剪贴板监控** | 轮询 `changeCount` | 消息循环 + `WM_CLIPBOARDUPDATE` |
| **键盘模拟** | ✅ 需要辅助功能权限 | ✅ 无需特殊权限 |
| **区域截图** | ❌ 暂不支持 | ✅ 支持（分层窗口 + GDI） |
| **获取选中内容** | ✅ 支持（模拟复制） | ✅ 支持（UI Automation + 剪贴板回退） |
| **权限要求** | 辅助功能权限（键盘模拟） | 无特殊要求 |

## 📝 注意事项

### macOS
- Bundle ID 是稳定的，应用重启后不变
- 推荐使用 Bundle ID 作为窗口标识
- **键盘模拟需要辅助功能权限**：
  - 系统偏好设置 → 隐私与安全性 → 辅助功能
  - 将你的应用或终端添加到允许列表
  - 首次调用会自动提示授权

### Windows
- Process ID 每次启动都会变化，不适合持久化存储
- 激活窗口可能受到 Windows 安全限制
- 建议结合应用名称 (`appName`) 进行窗口识别

## 📄 License

MIT
