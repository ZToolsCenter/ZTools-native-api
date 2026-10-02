# ZTools Native API

macOS 和 Windows 原生 API 的 Node.js 封装，使用 Swift + Win32 API + Node-API (N-API) 实现。

## ✨ 功能

1. **剪贴板变动监控** - 实时监听剪贴板内容变化
2. **窗口激活监控** - 实时监听窗口切换事件
3. **获取当前窗口** - 获取当前激活窗口的应用名和标识符
4. **设置激活窗口** - 根据标识符激活指定应用
5. **键盘模拟** - 模拟键盘按键和快捷键（支持修饰键）
6. **粘贴模拟** - 模拟 Cmd+V (macOS) / Ctrl+V (Windows)
7. **区域截图** - 选区截图并自动保存到剪贴板（双平台全功能：选区 + 编辑标注 + 圆角导出 + 保存对话框）
8. **长截图** - 手动滚动捕获拼接长图（双平台全功能：特征匹配拼接、自动滚动、小地图、裁剪）
9. **获取选中内容** - 获取当前选中的文本、文件或图像（支持 Cursor/VS Code 等编辑器）
10. **鼠标监控** - 实时监听鼠标移动、点击事件
11. **鼠标模拟** - 模拟鼠标移动、点击操作
12. **取色器** - 全屏取色工具
13. **设置文件窗口地址栏** - 跳转 Finder/Explorer 或文件选择对话框到指定路径
14. **Provider 桥接** - 让原生层（任意 native 线程上的 C++/Swift 代码）调用 JS 侧注册的方法（如宿主应用的翻译 / OCR 提供商）
15. **截图翻译** - 编辑态工具栏「翻译」按钮：原生编排 OCR（行级坐标）→ 聚类成段 → 逐段翻译 → 译文覆盖原文字区域（依赖 Provider 桥接，Windows/macOS）

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

// 7. 区域截图（双平台全功能：选区 + 编辑标注 + 长截图）
const { ScreenCapture } = require('ztools-native-api');

// 默认：拖拽选区松手即出图
ScreenCapture.start((result) => {
  if (result.success) {
    console.log(`截图成功！尺寸: ${result.width} x ${result.height}`);
    console.log('截图已保存到剪贴板，可按 Ctrl+V / Cmd+V 粘贴');
  } else {
    console.log('截图失败或已取消:', result.error || '');
  }
});
// 双平台流程一致：全屏暗化覆盖层 → 拖拽选区（或单击智能吸附窗口）→ 出图；
// 按 ESC / 点右键取消；autoConfirm=false 可停留在编辑态进行标注/长截图
// macOS 需要屏幕录制权限（+辅助功能权限），详见下文 API 说明

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
  - **macOS**: `{appName, bundleId, title, app, x, y, width, height, appPath, pid, isFullscreen} | null`
  - **Windows**: `{appName, processId, pid, title, app, x, y, width, height, appPath, isFullscreen} | null`

macOS 的 `isFullscreen` 直接来自焦点窗口的辅助功能属性 `AXFullScreen`。系统不支持该属性或未授予辅助功能权限时，该字段可能缺失。Windows 的 `isFullscreen` 由 Win32 原生层根据窗口与显示器边界、窗口状态和样式判定。

**示例**:
```javascript
// macOS
{ appName: 'Safari', bundleId: 'com.apple.Safari', isFullscreen: true }

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

#### `WindowManager.setAddressBar(target, address)`
设置文件资源管理器/Finder 或文件选择对话框的地址栏位置
- **参数**:
  - `target` - 目标窗口
    - **Windows**: `hwnd` 数字，或 `WindowManager.getActiveWindow()` 返回的窗口对象
    - **macOS**: `bundleId` / `pid`，或 `WindowManager.getActiveWindow()` 返回的窗口对象
  - `address` (string) - 要跳转的文件路径或 `file:///` 地址
- **返回**: `boolean` - 是否设置成功
- **平台**: ✅ Windows 和 macOS

**限制**:
- Windows 仅允许 Explorer 顶级窗口和包含 Shell 文件视图的文件选择对话框
- macOS 仅允许 Finder 和当前焦点为系统文件选择对话框的应用窗口
- 需要目标窗口可被激活；macOS 文件选择对话框场景需要辅助功能权限

**示例**:
```javascript
const current = WindowManager.getActiveWindow();
WindowManager.setAddressBar(current, process.platform === 'win32'
  ? 'C:\\Users\\username\\Documents'
  : '/Users/username/Documents'
);
```

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

#### `ScreenCapture.prime()`
预抓取当前虚拟屏幕帧（macOS 为所有显示器的并集）。`start()` 会优先消费未过期的预抓帧（2 秒内有效），过期/未命中时现场重抓。
- **返回**: `boolean` - 是否抓取成功（macOS 未授权屏幕录制时返回 false，但不弹授权框——授权框只在 `start()` 会话内出现）
- **平台**: ✅ Windows 和 macOS

#### `ScreenCapture.start(options, callback)`
启动区域截图
- **参数**:
  - `options` (Object，可选):
    - `autoConfirm` (boolean，默认 `true`) - 选区确定后直接出图，跳过编辑态
    - `longCapture.interval` (number，50~2000ms，默认 250) - 长截图采样防抖间隔（拼接无帧数/像素上限，可持续合并至用户主动结束）
  - `callback(result)` - 截图完成时的回调函数
    - `result.success` (boolean) - 是否成功截图
    - `result.x` / `result.y` (number) - 选区左上角（成功时；屏幕全局逻辑坐标，左上原点）
    - `result.x2` / `result.y2` (number) - 选区右下角（成功时）
    - `result.width` (number) - 截图宽度（成功时）
    - `result.height` (number) - 截图高度（成功时）
    - `result.base64` (string) - 截图 PNG 的 base64，带 `data:image/png;base64,` 前缀（成功时；已同时写入剪贴板）
    - `result.error` (string，可选) - 失败原因（macOS 屏幕录制权限不足时为 `'screen recording permission required'`）
- **平台**: ✅ Windows 和 macOS（全功能对等）

**平台差异**：
- **交互流程一致**：全屏暗化覆盖层 + 拖拽选区/单击智能窗口吸附 → autoConfirm=true 松手直接出图，
  autoConfirm=false 进入编辑态（工具栏 16 按钮、矩形/圆形/箭头/画笔/文字/马赛克标注、撤销/重做、
  选区圆角手柄）→ 确定/保存/取消/长截图/翻译（依赖 Provider 桥接，见下文）
- **macOS**:
  - 需要屏幕录制权限（未授权时首次 `start()` 弹系统授权框，拒绝后回调 `{ success: false, error: ... }`）
  - 另需辅助功能权限：ESC/右键兜底取消（覆盖层失焦时仍可取消）、长截图滚轮观察与自动滚动（CGEventTap）
  - `start()` 为**非阻塞**调用：创建会话后立即返回，结果经 callback 异步送达；会话期间
    AppKit 事件与 Node/libuv 事件循环均由宿主进程自己的主事件循环驱动（Electron 主进程
    天然满足），会话中可正常使用定时器/异步逻辑（如定时调用 `abortLongCapture()`）。
    纯 Node 宿主不驱动 macOS 主事件循环，无法运行截图会话——交互式验收请用
    `npx electron test/electron-host.cjs test/test-screenshot.js`
  - 选区坐标为屏幕全局逻辑坐标（左上原点）；输出图像为逻辑尺寸（Retina 下内部按物理像素捕获后缩回）
- **Windows**: 全屏遮罩 + 拖拽选区 + 编辑标注 + 长截图全功能（坐标系为虚拟屏绝对坐标）
- 会话进行中重复调用 `start()` 会抛出 `Error('Screenshot already in progress')`（双平台一致）

**示例**:
```javascript
ScreenCapture.start((result) => {
  if (result.success) {
    console.log(`截图成功！尺寸: ${result.width}x${result.height}`);
    // 截图已在剪贴板中，可按 Ctrl+V / Cmd+V 粘贴
  } else {
    console.log('截图失败或已取消:', result.error || '');
  }
});
```

#### `ScreenCapture.abortLongCapture()`
中止进行中的长截图滚动捕获。滚动捕获会以失败结果（`success: false`）回调后结束（ESC/工具栏取消同语义：取消 = 失败收束）；无进行中的长截图时为安全空操作。
- **平台**: ✅ Windows 和 macOS。macOS 的 `start()` 为非阻塞调用，会话期间本进程定时器/异步逻辑照常运转，可直接在进程内调用

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

### Provider 桥接（原生层调用 JS 方法）

让原生层（任意 native 线程上的 C++/Swift 代码）反向调用 JS 侧注册的方法，并获得异步结果。
典型场景：宿主应用把 provider 能力（如 `providerManager.invoke('translation' / 'ocr')`）
交给原生模块，原生业务（截图工具条、事件钩子等）在 native 线程上直接发起调用。

JS 侧注册 handler：

```javascript
const { ProviderBridge } = require('ztools-native-api');

// 注册后，原生层即可调用这里注册的能力；返回值自动 JSON 序列化回传
ProviderBridge.start(async (type, input) => {
  switch (type) {
    case 'translation':
      return { text: '你好' };
    case 'ocr':
      // 截图翻译依赖行级坐标块，见下文「截图翻译」
      return { text: '识别到的文字', blocks: [{ text: 'Hello', left: 10, top: 5, right: 90, bottom: 25 }] };
    default:
      throw new Error('unknown provider type: ' + type); // 原生侧收到该错误
  }
});

// 从 JS 侧验证链路（走真实原生线程路径）：
const result = await ProviderBridge.invokeFromNative('translation', { text: 'hello' });

// 不再需要时停止（等待中的原生调用会立即收到错误）
ProviderBridge.stop();
```

原生侧（C++）调用方式（见 `src/provider_bridge.h`）：

```cpp
#include "provider_bridge.h"

// 在任意 native 线程上（严禁在 JS 主线程调用，内部会拒绝）：
ztools_provider_bridge::InvokeResult r =
    ztools_provider_bridge::Invoke("translation", "{\"text\":\"hello\"}", 5000);
if (r.ok) {
  // r.value 为 JS handler 返回值的 JSON 字符串
}
```

约束与行为：
- 原生调用阻塞等待 JS 结果，超时（默认 15s，可指定）返回 `timed out`；迟到的 JS 结果会被丢弃
- handler 抛错 / reject 会以错误字符串回传原生侧
- 支持并发调用（按 `seq` 配对请求与结果）；`stop()` 会立即让等待中的调用失败
- 数据以 JSON 字符串穿越桥接，请保持入参/返回值可 JSON 序列化

除同步 `Invoke` 外，桥接层另提供**异步 RPC**（macOS 截图翻译的生产通路）：原生侧
`InvokeAsync(requestId, type, inputJson, callback, userData)` 发起后立即返回，结果由
JS 侧回传时经 callback 异步送达（恰好一次）；`CancelAsync(requestId)` 取消请求，
此后 JS 晚到的结果按 `seq` 查表落空被静默丢弃。异步 API 非阻塞，可在包括 JS 主线程
在内的任意线程调用；超时由调用方驱动（deadline 扫描 + CancelAsync）。从 JS 侧可走
真实通路验证：

```javascript
// 异步通路验证（原生 InvokeAsync + requestId + 看门狗超时 + CancelAsync 丢晚到）：
const result = await ProviderBridge.invokeFromNativeAsync('translation', { text: 'hello' }, 1001, 5000);
ProviderBridge.cancelFromNative(1001);   // 取消在飞请求（幂等）
```

对应的原生侧用法（见 `src/provider_bridge.h`）：

```cpp
// 异步：立即返回受理与否，结果经 callback(requestId, ok, value, error) 回投
ztools_provider_bridge::InvokeAccept a =
    ztools_provider_bridge::InvokeAsync(1001, "translation", "{\"text\":\"hi\"}",
                                        MyCallback, myUserData);
if (!a.accepted) { /* 不会触发回调，当场收尾 */ }
ztools_provider_bridge::CancelAsync(1001);   // 取消：晚到结果被丢弃
```

### 截图翻译（编辑态工具栏「翻译」按钮，依赖 Provider 桥接）

编辑态（`autoConfirm: false`）工具栏的「翻译」按钮：点击后原生层自动完成
**OCR 识别选区文字 → 空间聚类还原段落 → 逐段翻译 → 译文覆盖原文字区域**，确认/保存导出的
图像同样包含译文覆盖。覆盖展示中再次点击「翻译」为切换退出（清掉译文覆盖、取消按钮激活，
不重跑）；需要重译时再点一次重新发起。

编排发生在原生层：先调 type=`ocr` 拿到带行级坐标（top/left/right/bottom）的文本行数组，
再把文本行按空间关系聚类还原成段落（`src/screenshot/algo/translate_cluster.cpp`，跨平台
共用：行高/行距估计 → 候选相邻 → 多维特征判定连续/边界 → 聚合 + 孤行吸收，解决 OCR 按行
输出导致的段落信息丢失），然后对每个段落分别调一次 type=`translation`（段落文本已按 CJK/拉丁
规则拼接；macOS 以有限并发逐段调用、Windows 串行调用以尊重 provider 限流），最后在原生侧把译文与对应段落的成员行
联合框合并成可渲染块、按段落版面特征适配字号渲染。OCR/翻译渠道沿用用户在设置页
选择的默认提供商——宿主只需把桥接类型透传给 providerManager：

```javascript
ProviderBridge.start(async (type, input) => {
  // 宿主透传（示意）：OCR/翻译渠道由用户在设置页选择的默认 provider 决定
  if (type === 'ocr') return providerManager.invoke('ocr', input);
  if (type === 'translation') return providerManager.invoke('translation', input);
  throw new Error('unknown provider type: ' + type);
});
```

provider 约定（原生侧已按此实现）：

```javascript
// type = 'ocr'：入参为选区裁剪图（物理像素分辨率）
//   { image: 'data:image/png;base64,...' }
// 返回行级文本块，坐标为提交图像内像素坐标（兼容 x/y/width/height 命名）：
{
  text: 'Hello World\n第二行',
  blocks: [
    { text: 'Hello World', left: 24, top: 33, right: 395, bottom: 66 },
    { text: '第二行', left: 24, top: 80, right: 260, bottom: 112 },
  ],
  confidence: 0.98,
}

// type = 'translation'：入参单条文本（from/to 缺省用 provider 默认）
//   { text: 'Hello World' }
// 返回：
{ text: '你好，世界' }
```

- OCR 未返回坐标（blocks 为字符串数组或仅有 text，如纯文本 AI 识别）时走整图兜底：
  整段文本一次翻译，以整个选区为单一覆盖面板
- 单段失败/超时跳过该段（日志可见），全部失败才整体报错；macOS 为有限并发
  （同时在飞 ≤2，完成即补发），Windows 保持逐段串行（尊重 provider 限流）
- macOS 全链路为**异步 RPC**（requestId 驱动）：每个请求有唯一 `jobId`/`requestId`，
  晚到结果（已超时/已取消/任务已换代）按 requestId 匹配落空直接丢弃，绝不污染当前 UI

交互细节：
- 点击「翻译」→ 选区上方出现「正在识别并翻译…」进度气泡（OCR 超时 30 秒、单段翻译超时 15 秒）
- 成功 → 每个段落以白底圆角面板盖住原文并排入译文，译文字号按段落独立搜索
  （以原字号估计为锚点，可行性二分 + 候选真实排版测量评分，取综合视觉密度最接近原段落者；
  装不下时向下扩高并省略号截断）
- 失败（OCR/翻译出错、未识别到文字、超时等）→ 深红文字错误气泡，约 4 秒后自动消失
- **平台**: ✅ Windows 和 macOS（全功能对等）。macOS 截图会话为**非阻塞生命周期对象**
  （`start()` 立即返回，AppKit 事件与周期任务由宿主进程主事件循环驱动，交互式验收
  请用 Electron 宿主，见上文「平台差异」）；翻译全链路为**异步 RPC**（requestId 驱动）：
  每个请求有唯一 `jobId`/`requestId`，provider 的 Promise/网络请求由宿主进程自身的
  事件循环驱动（Electron 主进程消息泵在会话期间持续运转 libuv 与 NSApp），原生侧
  不手动泵 `uv_run`/V8 微任务——纯 Node 宿主无法运行 macOS 截图会话，
  请在 Electron 宿主中使用截图翻译


## 🗒️ 原生日志（Logger）

原生模块不再「黑盒」：绑定层、截图会话、剪贴板/窗口/鼠标监控、快捷键、翻译（OCR/翻译
provider 调用）等关键调用、状态变化与失败路径都会写入日志文件，便于用户侧排查问题。

### 日志文件

- 位置：**系统临时目录**下的 `ztools-native.log`
  - Windows: `%TEMP%\ztools-native.log`（`GetTempPathW`）
  - macOS: `$TMPDIR/ztools-native.log`（兜底 `/tmp`）
- 大小限制：**当前文件最大 10MB**；写满时整体轮转为 `ztools-native.log.old`
  （先删除旧 `.old` 再改名，任何时刻最多两个文件）
- 每条日志单次写入并立即落盘，原生崩溃时已写内容不丢
- 格式：`2026-09-22 13:14:39.040 [info] [tid 30828] [core] === ztools_native loaded (pid=40144, win32) ===`
- 多进程（宿主 + 测试脚本）同时写同一文件时按行追加，偶发交错可接受

### 日志等级

`trace < debug < info < warn < error`（另有 `off` = 完全关闭），默认 `info`。
两种控制方式：

```bash
# 方式一：环境变量（进程启动前设置）
ZTOOLS_LOG_LEVEL=debug node app.js
```

```javascript
// 方式二：JS 运行时设置（覆盖环境变量）
const { Logger } = require('ztools-native-api');

Logger.setLevel('debug');            // 运行时切换等级
Logger.getLevel();                   // -> 'debug'
Logger.isLevelEnabled('trace');      // -> true
Logger.getPath();                    // -> 日志文件绝对路径（可展示给用户）
Logger.write('info', 'app', '与原生日志同一文件，统一时间线');
```

埋点覆盖（tag 说明）：

| tag | 内容 |
|-----|------|
| `core` | 模块加载（含 pid/平台）、Swift 动态库加载（macOS） |
| `clipboard` | 监控启停、暂停/恢复、防抖后的剪贴板变化 |
| `window` | 监控启停、前台切换/激活结果 |
| `mouse` | 监控启停、长按触发 |
| `shortcut` | 快捷键监听启停、注册/注销结果、热键触发 |
| `screenshot` | 会话请求/预抓帧/覆盖层创建/初始化失败/结果输出 |
| `longcapture` | 长截图开始/中止/完成（帧数与输出尺寸） |
| `translate` | OCR/翻译请求、provider 调用失败、结果块数 |
| `provider` | Provider 桥接启停、每次 invoke 的结果/超时 |
| `uwp` | 包监听启停、install/update/uninstall 事件、应用启动 |
| `logger` | 通过 JS setLogLevel 的等级变更 |

## 🧪 测试

```bash
npm test

# 或运行特定测试
node test/test-keyboard.js         # 完整键盘测试
node test/test-keyboard-simple.js  # 简单键盘测试
node test/test-selected-content.js # 获取选中内容测试
node test/test-provider-bridge.js  # Provider 桥接（原生层调用 JS 方法）测试
node test/test-provider-async-bridge.js  # Provider 异步桥接（requestId/超时/取消/晚到丢弃/并发）测试
node test/test-screenshot.js       # 区域截图交互测试（真机手工验收；macOS 用
                                   #   npx electron test/electron-host.cjs test/test-screenshot.js）
node test/test-translate.js        # 截图翻译交互测试（mock provider，真机手工验收，见脚本头说明；
                                   #   MODE=plain 环境变量切换无坐标兜底模式；macOS 用
                                   #   npx electron test/electron-host.cjs test/test-translate.js）
node test/test-logger.js           # 原生日志（等级/文件/轮转）测试
```

## ⚠️ 平台差异

| 特性 | macOS | Windows |
|-----|-------|---------|
| **窗口标识符** | Bundle ID (稳定，如 `com.apple.Safari`) | Process ID (动态变化，如 `12345`) |
| **激活限制** | 较宽松 | 严格（需要线程附加 hack） |
| **剪贴板监控** | 轮询 `changeCount` | 消息循环 + `WM_CLIPBOARDUPDATE` |
| **键盘模拟** | ✅ 需要辅助功能权限 | ✅ 无需特殊权限 |
| **区域截图** | ✅ 支持（全功能：选区 + 标注 + 圆角导出 + 保存） | ✅ 支持（全功能：选区 + 标注 + 圆角导出 + 保存） |
| **长截图** | ✅ 支持（拼接/自动滚动/小地图/裁剪） | ✅ 支持（拼接/自动滚动/小地图/裁剪） |
| **截图线程模型** | ✅ 非阻塞会话（`start()` 立即返回；AppKit 事件与 Node 事件循环均由宿主主事件循环驱动，需 Electron 等 NSApplication 宿主） | 独立捕获线程，`start()` 立即返回 |
| **截图标注** | ✅ 矩形/椭圆/箭头/画笔/文字（IME）/马赛克，行为对齐 | ✅ 同左 |
| **截图输出** | ✅ PNG base64 + 剪贴板（原生支持透明 alpha）+ 保存对话框 + 圆角透明导出 | ✅ 同左（圆角透明走 `CF_DIB(V4)+PNG` 双格式） |
| **获取选中内容** | ✅ 支持（模拟复制） | ✅ 支持（UI Automation + 剪贴板回退） |
| **权限要求** | 辅助功能权限（键盘模拟；截图的 ESC 兜底取消/滚轮观察/自动滚动）+ 屏幕录制权限（截图） | 无特殊要求 |

## 📝 注意事项

### macOS
- Bundle ID 是稳定的，应用重启后不变
- 推荐使用 Bundle ID 作为窗口标识
- **键盘模拟需要辅助功能权限**：
  - 系统偏好设置 → 隐私与安全性 → 辅助功能
  - 将你的应用或终端添加到允许列表
  - 首次调用会自动提示授权
- **区域截图需要屏幕录制权限**：
  - 系统设置 → 隐私与安全性 → 屏幕录制
  - 首次调用 `ScreenCapture.start()` 会自动弹出系统授权框；授权可能需要重启宿主进程后生效
  - 未授权时回调 `{ success: false, error: 'screen recording permission required' }`
  - `ScreenCapture.prime()` 预检未授权时直接返回 false，不弹授权框
- **截图完整体验还建议授予辅助功能权限**：
  - ESC/右键兜底取消（覆盖层失焦时仍可取消，未授权时降级为覆盖层自身按键处理）
  - 长截图的滚轮观察与自动滚动（CGEventTap）
- **macOS 截图会话为非阻塞生命周期对象**：`start()` 创建会话后立即返回，结果经回调异步送达；
  会话期间 AppKit 事件（覆盖层交互/绘制）与周期任务（翻译回投、插入符闪烁、长截图采样）
  均由宿主进程自己的主事件循环驱动——Electron 主进程天然满足；纯 Node 宿主不运转
  macOS 主事件循环，无法运行截图会话（交互式验收用 `npx electron test/electron-host.cjs …`）
- **交互测试脚本**：`node test/test-screenshot.js`（权限预检 / 成功回调契约 / 编辑态与
  ESC 取消 / 长截图 abort，全交互式，需在真机上按脚本指引执行；macOS 需 Electron 宿主）

### Windows
- Process ID 每次启动都会变化，不适合持久化存储
- 激活窗口可能受到 Windows 安全限制
- 建议结合应用名称 (`appName`) 进行窗口识别

## 📄 License

MIT
