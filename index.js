const os = require('os');

// 根据平台加载对应的原生模块
const addon = require('./build/Release/ztools_native.node');
const platform = os.platform();

class ClipboardMonitor {
  constructor() {
    this._callback = null;
    this._isMonitoring = false;
  }

  /**
   * 启动剪贴板监控
   * @param {Function} callback - 剪贴板变化时的回调函数（无参数）
   */
  start(callback) {
    if (this._isMonitoring) {
      throw new Error('Monitor is already running');
    }

    if (typeof callback !== 'function') {
      throw new TypeError('Callback must be a function');
    }

    this._callback = callback;
    this._isMonitoring = true;

    addon.startMonitor(() => {
      if (this._callback) {
        this._callback();
      }
    });
  }

  /**
   * 停止剪贴板监控
   */
  stop() {
    if (!this._isMonitoring) {
      return;
    }

    addon.stopMonitor();
    this._isMonitoring = false;
    this._callback = null;
  }

  /**
   * 是否正在监控
   */
  get isMonitoring() {
    return this._isMonitoring;
  }

  /**
   * 获取剪贴板中的文件列表
   * @returns {Array<{path: string, name: string, isDirectory: boolean}>} 文件列表
   * - path: 文件完整路径
   * - name: 文件名
   * - isDirectory: 是否是目录
   */
  static getClipboardFiles() {
    if (platform === 'win32') {
      return addon.getClipboardFiles();
    } else if (platform === 'darwin') {
      // macOS 暂不支持
      throw new Error('getClipboardFiles is not yet supported on macOS');
    }
    return [];
  }

  /**
   * 设置剪贴板中的文件列表
   * @param {Array<string|{path: string}>} files - 文件路径数组
   * - 支持直接传递字符串路径数组: ['C:\\file1.txt', 'C:\\file2.txt']
   * - 支持传递对象数组: [{path: 'C:\\file1.txt'}, {path: 'C:\\file2.txt'}]
   * @returns {boolean} 是否设置成功
   * @example
   * // 使用字符串数组
   * ClipboardMonitor.setClipboardFiles(['C:\\test.txt', 'C:\\folder']);
   *
   * // 使用对象数组（兼容 getClipboardFiles 的返回格式）
   * const files = ClipboardMonitor.getClipboardFiles();
   * ClipboardMonitor.setClipboardFiles(files);
   */
  static setClipboardFiles(files) {
    if (!Array.isArray(files)) {
      throw new TypeError('files must be an array');
    }

    if (files.length === 0) {
      throw new Error('files array cannot be empty');
    }

    if (platform === 'win32') {
      return addon.setClipboardFiles(files);
    } else if (platform === 'darwin') {
      // macOS 暂不支持
      throw new Error('setClipboardFiles is not yet supported on macOS');
    }
    return false;
  }
}

class WindowMonitor {
  constructor() {
    this._callback = null;
    this._isMonitoring = false;
  }

  /**
   * 启动窗口监控
   * @param {Function} callback - 窗口切换时的回调函数
   * - macOS: {
   *     appName: string,
   *     bundleId: string,
   *     title: string,
   *     app: string,
   *     x: number,
   *     y: number,
   *     width: number,
   *     height: number,
   *     appPath: string,
   *     pid: number
   *   }
   * - Windows: {
   *     appName: string,
   *     processId: number,
   *     pid: number,
   *     title: string,
   *     app: string,
   *     x: number,
   *     y: number,
   *     width: number,
   *     height: number,
   *     appPath: string
   *   }
   */
  start(callback) {
    if (this._isMonitoring) {
      throw new Error('Window monitor is already running');
    }

    if (typeof callback !== 'function') {
      throw new TypeError('Callback must be a function');
    }

    this._callback = callback;
    this._isMonitoring = true;

    addon.startWindowMonitor((windowInfo) => {
      if (this._callback) {
        this._callback(windowInfo);
      }
    });
  }

  /**
   * 停止窗口监控
   */
  stop() {
    if (!this._isMonitoring) {
      return;
    }

    addon.stopWindowMonitor();
    this._isMonitoring = false;
    this._callback = null;
  }

  /**
   * 是否正在监控
   */
  get isMonitoring() {
    return this._isMonitoring;
  }
}


// 窗口管理类
class WindowManager {
  /**
   * 获取当前激活的窗口信息
   * @returns {{appName: string, bundleId?: string, title?: string, app?: string, x?: number, y?: number, width?: number, height?: number, appPath?: string, pid?: number, processId?: number}|null} 窗口信息对象
   * - macOS: { appName, bundleId, title, app, x, y, width, height, appPath, pid }
   * - Windows: { appName, processId, pid, title, app, x, y, width, height, appPath }
   */
  static getActiveWindow() {
    const result = addon.getActiveWindow();
    if (!result || result.error) {
      return null;
    }
    return result;
  }

  /**
   * 根据标识符激活指定应用的窗口
   * @param {string|number} identifier - 应用标识符
   * - macOS: bundleId (string)
   * - Windows: processId (number)
   * @returns {boolean} 是否激活成功
   */
  static activateWindow(identifier) {
    if (platform === 'darwin') {
      // macOS: bundleId 是字符串
      if (typeof identifier !== 'string') {
        throw new TypeError('On macOS, identifier must be a bundleId (string)');
      }
    } else if (platform === 'win32') {
      // Windows: processId 是数字
      if (typeof identifier !== 'number') {
        throw new TypeError('On Windows, identifier must be a processId (number)');
      }
    }
    return addon.activateWindow(identifier);
  }

  /**
   * 获取当前平台
   * @returns {string} 'darwin' | 'win32'
   */
  static getPlatform() {
    return platform;
  }

  /**
   * 模拟粘贴操作（Command+V on macOS, Ctrl+V on Windows）
   * @returns {boolean} 是否成功
   */
  static simulatePaste() {
    return addon.simulatePaste();
  }

  /**
   * 模拟键盘按键
   * @param {string} key - 要模拟的按键
   * @param {...string} modifiers - 修饰键（shift、ctrl、alt、meta）
   * @returns {boolean} 是否成功
   * @example
   * // 模拟按下字母 'a'
   * WindowManager.simulateKeyboardTap('a');
   *
   * // 模拟 Command+C (macOS) 或 Ctrl+C (Windows)
   * WindowManager.simulateKeyboardTap('c', 'meta');
   *
   * // 模拟 Shift+Tab
   * WindowManager.simulateKeyboardTap('tab', 'shift');
   *
   * // 模拟 Command+Shift+S (macOS)
   * WindowManager.simulateKeyboardTap('s', 'meta', 'shift');
   */
  static simulateKeyboardTap(key, ...modifiers) {
    if (typeof key !== 'string' || !key) {
      throw new TypeError('key must be a non-empty string');
    }
    return addon.simulateKeyboardTap(key, ...modifiers);
  }
}

class MouseMonitor {
  static _callback = null;
  static _isMonitoring = false;

  /**
   * 启动鼠标监控
   * @param {string} buttonType - 按钮类型：'middle' | 'right' | 'back' | 'forward'
   * @param {number} longPressMs - 长按阈值（毫秒）
   *   - 0: 监听点击（mouseUp 时触发）
   *   - >0: 监听长按（按住达到该时长后触发）
   *   - 注意：'right' 只支持长按（longPressMs 必须 > 0）
   * @param {Function} callback - 鼠标事件回调函数（无参数）
   *   回调函数可以返回一个对象 { shouldBlock: boolean }
   *   - 不返回值或返回 undefined: 阻止原生事件（默认行为）
   *   - 返回 { shouldBlock: false }: 不阻止原生事件（事件会被重放）
   *   - 返回 { shouldBlock: true }: 阻止原生事件
   */
  static start(buttonType, longPressMs, callback) {
    if (MouseMonitor._isMonitoring) {
      throw new Error('Mouse monitor is already running');
    }

    const validButtons = ['middle', 'right', 'back', 'forward'];
    if (!validButtons.includes(buttonType)) {
      throw new TypeError(`buttonType must be one of: ${validButtons.join(', ')}`);
    }

    if (typeof longPressMs !== 'number' || longPressMs < 0) {
      throw new TypeError('longPressMs must be a non-negative number');
    }

    if (buttonType === 'right' && longPressMs === 0) {
      throw new TypeError("'right' button only supports long press (longPressMs must be > 0)");
    }

    if (typeof callback !== 'function') {
      throw new TypeError('Callback must be a function');
    }

    MouseMonitor._callback = callback;
    MouseMonitor._isMonitoring = true;

    addon.startMouseMonitor(buttonType, longPressMs, () => {
      if (MouseMonitor._callback) {
        return MouseMonitor._callback();
      }
    });
  }

  /**
   * 停止鼠标监控
   */
  static stop() {
    if (!MouseMonitor._isMonitoring) {
      return;
    }

    addon.stopMouseMonitor();
    MouseMonitor._isMonitoring = false;
    MouseMonitor._callback = null;
  }

  /**
   * 是否正在监控
   */
  static get isMonitoring() {
    return MouseMonitor._isMonitoring;
  }
}

// 取色器类
class ColorPicker {
  static _callback = null;
  static _isActive = false;

  /**
   * 启动取色器
   * 进入取色模式后，鼠标附近会出现 9x9 像素放大网格
   * 点击鼠标左键确认取色，按 ESC 键取消
   *
   * @param {Function} callback - 取色完成时的回调函数
   * - 成功: { success: true, hex: '#59636E' }
   * - 取消: { success: false, hex: null }
   *
   * @example
   * ColorPicker.start((result) => {
   *   if (result.success) {
   *     console.log('选中的颜色:', result.hex);
   *   } else {
   *     console.log('取色已取消');
   *   }
   * });
   */
  static start(callback) {
    if (ColorPicker._isActive) {
      throw new Error('Color picker is already active');
    }

    if (typeof callback !== 'function') {
      throw new TypeError('Callback must be a function');
    }

    ColorPicker._callback = callback;
    ColorPicker._isActive = true;

    addon.startColorPicker((result) => {
      // 资源会在 C++ 线程结束时自动清理，不需要手动调用 stopColorPicker
      ColorPicker._isActive = false;
      if (ColorPicker._callback) {
        const cb = ColorPicker._callback;
        ColorPicker._callback = null;
        cb(result);
      }
    });
  }

  /**
   * 停止取色器（手动取消）
   */
  static stop() {
    if (!ColorPicker._isActive) {
      return;
    }

    addon.stopColorPicker();
    ColorPicker._isActive = false;
    ColorPicker._callback = null;
  }

  /**
   * 是否正在取色
   */
  static get isActive() {
    return ColorPicker._isActive;
  }
}

// 区域截图类
class ScreenCapture {
  /**
   * 启动区域截图
   * @param {Function} callback - 截图完成时的回调函数
   * - 参数: { success: boolean, width?: number, height?: number }
   * - success: 是否成功截图
   * - width: 截图宽度（成功时）
   * - height: 截图高度（成功时）
   */
  static start(callback) {
    if (platform === 'darwin') {
      // macOS 暂不支持
      throw new Error('ScreenCapture is not yet supported on macOS');
    }

    if (typeof callback !== 'function') {
      throw new TypeError('Callback must be a function');
    }

    addon.startRegionCapture((result) => {
      callback(result);
    });
  }
}

// 应用图标提取类
class IconExtractor {
  /**
   * 异步获取文件/应用的图标（PNG 格式 Buffer）
   * @param {string} filePath - 文件路径或类型（macOS 支持绝对路径、`folder`、`txt`、`pdf` 等）
   * @returns {Promise<Buffer>} Promise，resolve 为 PNG 格式的图标数据
   * @example
   * // 获取 exe 的 32x32 图标
   * const icon = await IconExtractor.getFileIcon('C:\\Windows\\notepad.exe');
   *
   * // 保存为文件
   * const fs = require('fs');
   * const icon = await IconExtractor.getFileIcon('C:\\Windows\\notepad.exe');
   * if (icon) fs.writeFileSync('icon.png', icon);
   */
  static getFileIcon(filePath) {
    if (platform !== 'win32' && platform !== 'darwin') {
      throw new Error('getFileIcon is only supported on Windows and macOS');
    }
    if (typeof filePath !== 'string' || !filePath) {
      throw new TypeError('filePath must be a non-empty string');
    }
    return addon.getFileIcon(filePath);
  }
}

// UWP 应用管理类
class UwpManager {
  /**
   * 获取已安装的 UWP 应用列表
   * @returns {Array<{name: string, appId: string, icon: string, installLocation: string}>} 应用列表
   * - name: 应用显示名称
   * - appId: AppUserModelID（用于启动应用）
   * - icon: 应用图标路径
   * - installLocation: 应用安装目录
   */
  static getUwpApps() {
    if (platform !== 'win32') {
      throw new Error('getUwpApps is only supported on Windows');
    }
    return addon.getUwpApps();
  }

  /**
   * 启动 UWP 应用
   * @param {string} appId - AppUserModelID（从 getUwpApps 获取）
   * @returns {boolean} 是否启动成功
   */
  static launchUwpApp(appId) {
    if (platform !== 'win32') {
      throw new Error('launchUwpApp is only supported on Windows');
    }
    if (typeof appId !== 'string' || !appId) {
      throw new TypeError('appId must be a non-empty string');
    }
    return addon.launchUwpApp(appId);
  }
}

// MUI 资源字符串解析类
class MuiResolver {
  /**
   * 批量解析 MUI 资源字符串
   * @param {string[]} refs - MUI 引用字符串数组，如 ['@%SystemRoot%\\system32\\shell32.dll,-22067']
   * @returns {{ [ref: string]: string }} 解析结果对象，key 为原始引用，value 为解析后的本地化字符串
   * @example
   * const result = MuiResolver.resolve([
   *   '@%SystemRoot%\\system32\\shell32.dll,-22067',
   *   '@%SystemRoot%\\system32\\shell32.dll,-21769'
   * ]);
   * // { '@%SystemRoot%\\system32\\shell32.dll,-22067': '文件资源管理器', ... }
   */
  static resolve(refs) {
    if (platform !== 'win32') {
      throw new Error('MuiResolver is only supported on Windows');
    }
    if (!Array.isArray(refs)) {
      throw new TypeError('refs must be an array of strings');
    }
    return addon.resolveMuiStrings(refs);
  }
}

// 导出所有类
module.exports = {
  ClipboardMonitor,
  WindowMonitor,
  WindowManager,
  ScreenCapture,
  MouseMonitor,
  ColorPicker,
  IconExtractor,
  UwpManager,
  MuiResolver
};

// 为了向后兼容，默认导出 ClipboardMonitor
module.exports.default = ClipboardMonitor;
