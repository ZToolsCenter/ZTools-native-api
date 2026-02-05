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
   *     windowTitle: string, 
   *     app: string,
   *     x: number,
   *     y: number,
   *     width: number,
   *     height: number,
   *     appPath: string,
   *     pid: number
   *   }
   * - Windows: { appName: string, processId: number, windowTitle: string, app: string }
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
   * @returns {{appName: string, bundleId?: string, windowTitle?: string, app?: string, x?: number, y?: number, width?: number, height?: number, appPath?: string, pid?: number, processId?: number}|null} 窗口信息对象
   * - macOS: { appName, bundleId, windowTitle, app, x, y, width, height, appPath, pid }
   * - Windows: { appName, processId, windowTitle, app }
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

// 导出所有类
module.exports = {
  ClipboardMonitor,
  WindowMonitor,
  WindowManager,
  ScreenCapture
};

// 为了向后兼容，默认导出 ClipboardMonitor
module.exports.default = ClipboardMonitor;
