# ztools-event-hook

跨平台全局鼠标和键盘事件钩子库，用于 Node.js。

## 功能特性

- ✅ 全局鼠标事件监听（左键/右键按下/抬起）
- ✅ 全局键盘事件监听（按键按下/抬起，修饰键状态变化）
- ✅ 支持 macOS 和 Windows 平台
- ✅ 区分左右修饰键（Left Shift, Right Shift 等）
- ✅ 识别特殊按键（Fn, CapsLock, Tab, ` 等）
- ✅ 过滤未知按键（Unknown 键不触发回调）

## 安装

```bash
npm install
```

## 构建

```bash
# 构建项目（纯 C++ 实现，无需 Swift）
npm run build

# 清理构建文件
npm run clean
```

## 使用方法

```javascript
const EventHook = require('./index');

const hook = new EventHook();

// 监听鼠标和键盘事件
hook.start(3, (...args) => {
  // 判断事件类型
  if (args.length === 3 && typeof args[0] === 'number' && typeof args[1] === 'number') {
    // 鼠标事件: [eventCode, x, y]
    const [eventCode, x, y] = args;
    console.log(`鼠标事件: ${eventCode} @ (${x}, ${y})`);
  } else if (args.length === 6 && typeof args[0] === 'string') {
    // 键盘事件: [keyName, shiftKey, ctrlKey, altKey, metaKey, flagsChange]
    const [keyName, shiftKey, ctrlKey, altKey, metaKey, flagsChange] = args;
    console.log(`键盘事件: ${keyName}`, { shiftKey, ctrlKey, altKey, metaKey, flagsChange });
  }
});

// 停止监听
hook.stop();
```

## API

### EventHook

#### `start(effect, callback)`

启动事件钩子。

**参数：**
- `effect` (number): 事件类型
  - `1`: 仅监听鼠标事件
  - `2`: 仅监听键盘事件
  - `3`: 同时监听鼠标和键盘事件
- `callback` (Function): 事件回调函数

**回调参数：**

**鼠标事件：**
- macOS: `callback(eventCode: number, x: number, y: number)`
- Windows: `callback(eventCode: number, x?: number, y?: number)`

**键盘事件：**
- `callback(keyName: string, shiftKey: boolean, ctrlKey: boolean, altKey: boolean, metaKey: boolean, flagsChange: boolean)`

#### `stop()`

停止事件钩子。

#### `isHooking` (getter)

是否正在监听。

## 事件代码

### 鼠标事件代码

**macOS:**
- `1`: 左键按下
- `2`: 左键抬起
- `3`: 右键按下
- `4`: 右键抬起

**Windows:**
- `0x0201`: 左键按下 (WM_LBUTTONDOWN)
- `0x0202`: 左键抬起 (WM_LBUTTONUP)
- `0x0204`: 右键按下 (WM_RBUTTONDOWN)
- `0x0205`: 右键抬起 (WM_RBUTTONUP)

### 键盘事件

**按键名称：**
- 字母键: `A`, `B`, `C`, ...
- 数字键: `0`, `1`, `2`, ...
- 功能键: `F1`, `F2`, ..., `F12`
- 特殊键: `Enter`, `Tab`, `Space`, `Delete`, `Escape`, `CapsLock`, `` ` ``, `Fn`
- 方向键: `Left`, `Right`, `Up`, `Down`
- 修饰键: `Left Shift`, `Right Shift`, `Left Control`, `Right Control`, `Left Option`, `Right Option`, `Left Command`, `Right Command` (macOS) / `Left Alt`, `Right Alt`, `Left Win`, `Right Win` (Windows)

## 权限要求

### macOS

需要辅助功能权限：
1. 系统偏好设置 → 隐私与安全性 → 辅助功能
2. 将你的应用或终端添加到允许列表
3. 首次调用会自动提示授权

### Windows

无需特殊权限。

## 测试

```bash
# 编译测试
npm run test:compile

# 功能测试
npm run test
```

## License

MIT

