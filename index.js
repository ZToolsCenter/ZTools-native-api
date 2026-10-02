const os = require('os');

// 根据平台加载对应的原生模块
const addon = require('./build/Release/ztools_native.node');
const platform = os.platform();

/**
 * 原生层日志管理。
 *
 * 原生模块（N-API 绑定、截图会话、监控线程等）的关键调用、状态变化与失败路径
 * 都会写入系统临时目录下的 ztools-native.log（不超过 10MB，写满后整体轮转为
 * ztools-native.log.old）。通过本类可查询/修改日志等级、获取日志文件路径，
 * 也可以让 JS 侧把日志写进同一个文件（统一时间线，便于与原生日志对齐排查）。
 *
 * 等级：trace < debug < info < warn < error（另有 off = 完全关闭）。
 * 默认 info；可用环境变量 ZTOOLS_LOG_LEVEL（进程启动前设置）或 setLevel()
 * （运行时覆盖）控制。
 */
class Logger {
  /**
   * 支持的日志等级列表
   * @returns {string[]} ['trace', 'debug', 'info', 'warn', 'error', 'off']
   */
  static get levels() {
    return ['trace', 'debug', 'info', 'warn', 'error', 'off'];
  }

  /**
   * 运行时设置日志输出等级（覆盖 ZTOOLS_LOG_LEVEL 环境变量）
   * @param {string} level - 'trace' | 'debug' | 'info' | 'warn' | 'error' | 'off'
   * @throws {TypeError} level 不是合法等级字符串时抛出
   */
  static setLevel(level) {
    if (typeof level !== 'string' || !Logger.levels.includes(level.toLowerCase())) {
      throw new TypeError(`level must be one of: ${Logger.levels.join(', ')}`);
    }
    addon.setLogLevel(level.toLowerCase());
  }

  /**
   * 查询当前日志输出等级
   * @returns {string} 当前等级字符串
   */
  static getLevel() {
    return addon.getLogLevel();
  }

  /**
   * 查询某等级当前是否会被写入
   * @param {string} level - 等级字符串
   * @returns {boolean} 该等级日志当前是否输出
   */
  static isLevelEnabled(level) {
    if (typeof level !== 'string' || !Logger.levels.includes(level.toLowerCase())) {
      throw new TypeError(`level must be one of: ${Logger.levels.join(', ')}`);
    }
    return addon.isLogLevelEnabled(level.toLowerCase());
  }

  /**
   * 获取原生日志文件完整路径（系统临时目录下 ztools-native.log）
   * @returns {string} 日志文件绝对路径
   * @example
   * console.log('原生日志:', Logger.getPath());
   */
  static getPath() {
    return addon.getLogFilePath();
  }

  /**
   * 向原生日志文件写入一条日志（与原生日志同一文件、同一格式）
   * @param {string} level - 'trace' | 'debug' | 'info' | 'warn' | 'error'
   * @param {string} tag - 短模块名（如 'app'）
   * @param {string} message - 消息内容（不建议包含超大文本/敏感信息）
   * @example
   * Logger.write('info', 'app', '截图流程开始');
   */
  static write(level, tag, message) {
    if (typeof level !== 'string' || !Logger.levels.slice(0, 5).includes(level.toLowerCase())) {
      throw new TypeError(`level must be one of: ${Logger.levels.slice(0, 5).join(', ')}`);
    }
    if (typeof tag !== 'string' || !tag) {
      throw new TypeError('tag must be a non-empty string');
    }
    if (typeof message !== 'string') {
      throw new TypeError('message must be a string');
    }
    addon.logWrite(level.toLowerCase(), tag, message);
  }
}

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
   * 暂停剪贴板监控（不触发回调，但保持监控线程运行）
   */
  pause() {
    if (!this._isMonitoring) {
      return;
    }
    addon.pauseMonitor();
  }

  /**
   * 恢复剪贴板监控
   */
  resume() {
    if (!this._isMonitoring) {
      return;
    }
    addon.resumeMonitor();
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

    if (platform === 'win32' || platform === 'darwin') {
      // Windows 与 macOS 原生层均已实现（macOS 经 Swift NSPasteboard 写入文件 URL）
      return addon.setClipboardFiles(files);
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
   * @returns {{appName: string, bundleId?: string, title?: string, app?: string, x?: number, y?: number, width?: number, height?: number, appPath?: string, pid?: number, processId?: number, isFullscreen?: boolean}|null} 窗口信息对象
   * - macOS: { appName, bundleId, title, app, x, y, width, height, appPath, pid, isFullscreen }
   * - Windows: { appName, processId, pid, title, app, x, y, width, height, appPath, isFullscreen }
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
  /**
   * 获取所有打开的文件资源管理器/Finder 窗口信息
   * @returns {Array<{platform?: string, kind?: string, preciseTarget?: boolean, hwnd?: number, windowId?: number, finderId?: number, pid?: number, bundleId?: string, app?: string, title?: string, className?: string, axRole?: string, axSubrole?: string, path?: string, url?: string}>}
   */
  static getAllExplorerWindows() {
    return addon.getAllExplorerWindows();
  }

  /**
   * 判断指定窗口是否是可安全修改地址栏的文件定位窗口
   * @param {number} hwnd - Windows 窗口句柄
   * @returns {boolean}
   */
  static isFileLocationWindow(hwnd) {
    if (platform !== 'win32') {
      throw new Error('isFileLocationWindow is only available on Windows');
    }
    if (typeof hwnd !== 'number' || !Number.isFinite(hwnd) || hwnd <= 0) {
      throw new TypeError('hwnd must be a positive number');
    }
    return addon.isFileLocationWindow(hwnd);
  }

  /**
   * 设置指定文件资源管理器/Finder 或文件选择对话框的地址栏位置
   * @param {Object|string|number} target - 目标窗口；Windows 支持 hwnd 数字或包含 hwnd 的窗口对象，macOS 支持 bundleId/pid 或窗口对象
   * @param {string} address - 要跳转的文件路径或 file:/// 地址
   * @returns {boolean} 是否设置成功
   * @example
   * const win = WindowManager.getActiveWindow();
   * WindowManager.setAddressBar(win, 'C:\\Users\\username\\Documents');
   */
  static setAddressBar(target, address) {
    if (typeof address !== 'string' || !address) {
      throw new TypeError('address must be a non-empty string');
    }

    let identifier = target;
    if (target && typeof target === 'object') {
      if (platform === 'win32') {
        identifier = target.hwnd;
      } else if (platform === 'darwin') {
        identifier = target;
      }
    }

    if (platform === 'win32') {
      if (typeof identifier !== 'number') {
        throw new TypeError('On Windows, target must be a hwnd number or a window object with hwnd');
      }
    } else if (platform === 'darwin') {
      if (typeof identifier !== 'string' && typeof identifier !== 'number') {
        throw new TypeError('On macOS, target must be a bundleId, pid, or window object with bundleId/pid');
      }
    }

    return addon.setAddressBar(identifier, address);
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
   * 预抓取当前虚拟屏幕帧（macOS 为所有显示器的并集）
   * 帧在 2 秒内有效：start() 会优先消费未过期的预抓帧，过期/未命中时现场重抓
   * @returns {boolean} 是否抓取成功
   */
  static prime() {
    return addon.primeScreenshotFrame();
  }

  /**
   * 启动区域截图
   * @param {Object|Function} [options] - 截图选项；直接传函数时按旧签名 start(callback) 处理
   * @param {boolean} [options.autoConfirm=true] - 选区确定后直接出图，跳过编辑态（工具栏/标注）
   * @param {Object} [options.longCapture] - 长截图（手动滚动捕获）参数，进入编辑态后点工具栏「长截图」按钮生效。
   *   点击后进入长截图预览界面：全屏黑色遮罩保留、选区框与底部工具栏保持展示（仅剩完成/取消，
   *   其他操作禁用）；选区内直通底层应用，滚轮滚动页面（鼠标按键被吞，防止误触），
   *   侧边小地图实时展示拼接长图（外框=已捕获区域，蓝色内框=当前可见区域，随滚动移动）；
   *   向下滚动追加新内容到底部、向上滚动前插到头部；点「完成」出图、「取消」或 ESC 中止；
   *   拼接无帧数/像素上限，可持续合并至用户主动结束
   * @param {number} [options.longCapture.interval=250] - 滚轮停止后等待内容稳定的毫秒数（50~2000，采样防抖；
   *   滚动进行中也会按不低于 min(interval, 250)ms 的节拍主动采样，保证相邻帧有大重叠区域）
   * 编辑态工具栏另有「翻译」按钮（Windows/macOS）：OCR 识别选区文字 → 翻译 → 译文覆盖原文字区域，
   * 依赖 ProviderBridge 注册默认启用的 ocr/translation provider（约定见 README「截图翻译」）；
   * 确认/保存导出的图像同样包含译文覆盖
   * @param {Function} [callback] - 截图完成时的回调函数
   * - 参数: { success: boolean, x?: number, y?: number, x2?: number, y2?: number, width?: number, height?: number, base64?: string, error?: string }
   * - success: 是否成功截图
   * - x/y: 选区左上角（成功时；屏幕全局逻辑坐标，左上原点）
   * - x2/y2: 选区右下角（成功时）
   * - width: 截图宽度（成功时；长截图为拼接后的总宽度）
   * - height: 截图高度（成功时；长截图为拼接后的总高度）
   * - base64: 截图 PNG 的 base64，带 data:image/png;base64, 前缀（成功时；已同时写入剪贴板）
   * - error: 失败原因（可选；macOS 屏幕录制权限不足时为 'screen recording permission required'）
   *
   * macOS 说明（macOS 已具备与 Windows 对等的全功能截图）：
   * - 需要屏幕录制权限，未授权时首次调用会弹出系统授权框，拒绝后回调 { success: false, error: ... }
   * - 选区 UI（暗化蒙版 + 拖拽框选/窗口吸附/放大镜）、编辑态（工具栏/矢量与文字标注/IME/
   *   马赛克/撤销重做/选区圆角）、圆角透明导出、保存对话框与长截图全子系统均已实现，
   *   行为与 Windows 版对齐；差异项见 README「平台差异」表
   * - 额外需要辅助功能权限：ESC/右键兜底取消、长截图滚轮观察与 autoScroll（CGEventTap）
   * - macOS 的 start() 为**非阻塞**调用：创建会话后立即返回，结果经 callback 异步送达；
   *   会话期间 AppKit 事件与 Node/libuv 事件循环均由宿主进程自身的主事件循环驱动
   *   （Electron 主进程天然满足），会话期间可正常使用本进程定时器/异步逻辑
   *   （如定时触发 abortLongCapture）。纯 Node 宿主不驱动 macOS 主事件循环，
   *   无法运行截图会话，请在 Electron 宿主中使用（见 README「平台差异」）
   *
   * @example
   * // 默认：框选/点选完成即出图，不再二次编辑
   * ScreenCapture.start((result) => { ... });
   *
   * // 进入编辑态：选区确定后停留在工具栏，可标注/调整
   * ScreenCapture.start({ autoConfirm: false }, (result) => { ... });
   *
   * // 编辑态 + 长截图：选区确定后点工具栏「长截图」按钮进入手动滚动捕获
   * ScreenCapture.start({
   *   autoConfirm: false,
   *   longCapture: { interval: 250 }
   * }, (result) => { ... });
   */
  static start(options, callback) {
    // 兼容旧签名 start(callback)
    if (typeof options === 'function') {
      callback = options;
      options = undefined;
    }

    if (typeof callback !== 'function') {
      throw new TypeError('Callback must be a function');
    }

    addon.startRegionCaptureWithPrimedFrame(options || {}, (result) => {
      callback(result);
    });
  }

  /**
   * 中止进行中的长截图滚动捕获（Windows / macOS 双平台）
   * 滚动捕获会以失败结果（success: false）回调后结束（ESC/取消同语义：取消 = 失败收束）；
   * 无进行中的长截图时为安全空操作，可在任意线程/进程调用（macOS start() 为非阻塞
   * 调用，会话期间本进程定时器/异步逻辑照常运转，可直接调用本方法）
   */
  static abortLongCapture() {
    addon.abortLongCapture();
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
   * 监听当前用户 UWP 包的安装、更新和卸载完成事件。
   * @param {(event: {type: 'install'|'update'|'uninstall', packageFullName: string}) => void} callback - 包变化回调
   * @returns {void} 无返回值
   * @throws {Error} 当前平台不是 Windows、回调无效或原生监听初始化失败时抛出
   */
  static startPackageMonitor(callback) {
    if (platform !== 'win32') {
      throw new Error('startPackageMonitor is only supported on Windows');
    }
    if (typeof callback !== 'function') {
      throw new TypeError('Callback must be a function');
    }
    addon.startUwpPackageMonitor(callback);
  }

  /**
   * 停止当前用户 UWP 包变化监听。
   * @returns {void} 无返回值
   */
  static stopPackageMonitor() {
    if (platform === 'win32') {
      addon.stopUwpPackageMonitor();
    }
  }

  /**
   * 获取当前用户已注册包的稳定快照。
   * @returns {string[]} 按包完整名排序的快照
   * @throws {Error} 当前平台不是 Windows 或包注册表不可读时抛出
   */
  static getPackageSnapshot() {
    if (platform !== 'win32') {
      throw new Error('getPackageSnapshot is only supported on Windows');
    }
    return addon.getUwpPackageSnapshot();
  }

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
   * @returns {{success: boolean, hresult: number, foregroundHresult: number, foregroundPermissionGranted: boolean, processId: number, stage: string}} 启动结果与前台权限诊断信息
   * @throws {TypeError} appId 不是非空字符串时抛出
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
class WindowsShortcutScanner {
  static scan(scanPaths, rootScanPaths, skipFolders) {
    if (platform !== 'win32') {
      throw new Error('WindowsShortcutScanner is only supported on Windows');
    }
    if (!Array.isArray(scanPaths) || !Array.isArray(rootScanPaths) || !Array.isArray(skipFolders)) {
      throw new TypeError('scanPaths, rootScanPaths and skipFolders must be arrays');
    }
    return addon.scanWindowsShortcuts(scanPaths, rootScanPaths, skipFolders);
  }
}
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

/**
 * 获取当前选中的内容（支持文本、文件、图像）
 *
 * 实现方式：
 * - Windows: 优先使用 UI Automation API，回退到剪贴板方法（适用于 Cursor/VS Code 等编辑器）
 * - macOS: 使用模拟复制方法（Cmd+C）
 *
 * 在模拟复制时会自动暂停内部的 clipboardMonitor，防止误触发监听自身发起的事件
 *
 * @returns {Array<{type: string, data: any}>} 选中内容数组
 * - type: 'text' | 'file' | 'image'
 * - data: 根据类型不同：
 *   - text: 字符串
 *   - file: 文件路径字符串数组
 *   - image: base64 编码的 PNG 图像（带 format 和 encoding 字段）
 *
 * @example
 * const contents = getSelectedContent();
 * contents.forEach(item => {
 *   switch (item.type) {
 *     case 'text':
 *       console.log('Selected text:', item.data);
 *       break;
 *     case 'file':
 *       console.log('Selected files:', item.data);
 *       break;
 *     case 'image':
 *       console.log('Selected image (base64):', item.data.substring(0, 50) + '...');
 *       break;
 *   }
 * });
 */
function getSelectedContent() {
  return addon.getSelectedContent();
}

function launchCuiShell(shell, currentDirectory) {
  if (platform !== 'win32') {
    throw new Error('launchCuiShell is only supported on Windows');
  }
  return addon.launchCuiShell(shell, currentDirectory);
}

/**
 * Provider 桥接层：让原生层（任意 native 线程上的 C++/Swift 代码）调用 JS 侧注册的方法。
 *
 * 典型用法（宿主侧，如 ZTools 主进程把 provider 能力交给原生层）：
 *   const { ProviderBridge } = require('ztools-native-api');
 *   ProviderBridge.start(async (type, input) => {
 *     // type: 'translation' | 'ocr' 等（由宿主与原生侧自行约定）
 *     // 返回值会以 JSON 序列化后回传给原生线程
 *     return await providerManager.invoke(type, input);
 *   });
 *
 * 内置约定（截图工具栏「翻译」按钮依赖以下两个类型，详见 README「截图翻译」）：
 * - ocr: { image } => { text, blocks: [{ text, left, top, right, bottom }] }
 *   （blocks 为行级坐标块；provider 只回整段 text 时原生自动走整图兜底）
 * - translation: { text } => { text }（OCR 多行结果由原生按聚类段落分别调用）
 *
 * 原生侧（C++/Swift）调用方式见 src/provider_bridge.h 的
 * ztools_provider_bridge::Invoke(type, inputJson, timeoutMs)。
 *
 * 注意：原生侧的 Invoke 严禁在 JS 主线程上调用（内部检测并直接报错），事件回调
 * 线程、消息循环线程等 native 线程均可安全调用。
 */
const ProviderBridge = {
  /**
   * 启动桥接，注册 JS 侧 handler。重复启动会抛错；stop 之后可重新 start。
   * @param {Function} handler - (type: string, input: any) => Promise<any>；
   *                              返回值（或 reject 的错误信息）会回传给发起调用的原生线程
   * @returns {void} 无返回值
   * @throws {TypeError} handler 不是函数时抛出
   * @throws {Error} 桥接已启动时抛出
   */
  start(handler) {
    if (typeof handler !== 'function') {
      throw new TypeError('ProviderBridge.start requires a handler function');
    }
    addon.startProviderBridge((type, inputJson, seq) => {
      // handler 同步启动：分发即开始执行（含同步抛错经 reject 回传），异步续体由
      // 宿主事件循环的常规回调边界推进并排空 microtask
      let result;
      try {
        result = handler(type, JSON.parse(inputJson));
      } catch (err) {
        addon.rejectProviderBridge(seq, err instanceof Error ? err.message : String(err));
        return;
      }
      Promise.resolve(result)
        .then((value) => {
          addon.resolveProviderBridge(seq, JSON.stringify(value === undefined ? null : value));
        })
        .catch((err) => {
          addon.rejectProviderBridge(seq, err instanceof Error ? err.message : String(err));
        });
    });
  },

  /**
   * 停止桥接并丢弃 handler。已在等待的原生调用会立即收到 "provider bridge stopped" 错误。
   * @returns {void} 无返回值
   */
  stop() {
    addon.stopProviderBridge();
  },

  /**
   * 查询桥接是否就绪。
   * @returns {boolean} 已 start 且未 stop 时为 true
   */
  isReady() {
    return addon.isProviderBridgeReady();
  },

  /**
   * 从 JS 侧发起一次"原生发起"的调用：走真实原生线程 → JS 线程的完整通路，
   * 用于验证桥接链路（原生业务代码应直接调用 C++ 的 Invoke，而不是本方法）。
   * @param {string} type - 能力类型
   * @param {any} input - 入参（自动 JSON 序列化传给 handler）
   * @param {number} [timeoutMs=15000] - 原生侧等待结果的超时毫秒数
   * @returns {Promise<any>} handler 的返回值；超时或 handler 抛错时 Promise 被 reject
   */
  invokeFromNative(type, input, timeoutMs = 15000) {
    return new Promise((resolve, reject) => {
      addon.invokeProviderFromNative(
        type,
        JSON.stringify(input === undefined ? null : input),
        timeoutMs,
        (err, resultJson) => {
          if (err) {
            reject(new Error(err));
            return;
          }
          try {
            resolve(JSON.parse(resultJson));
          } catch (parseErr) {
            reject(parseErr);
          }
        }
      );
    });
  },

  /**
   * 从 JS 侧发起一次"原生发起"的异步调用：走与 macOS 截图翻译完全相同的异步通路
   * （原生 InvokeAsync + requestId + 看门狗超时 + CancelAsync 丢弃晚到结果），
   * 用于验证异步桥接（原生业务代码应直接调用 C++ 的 InvokeAsync，而不是本方法）。
   * @param {string} type - 能力类型
   * @param {any} input - 入参（自动 JSON 序列化传给 handler）
   * @param {number} requestId - 请求号（调用方保证唯一；用于 cancelFromNative 取消）
   * @param {number} [timeoutMs=15000] - 超时毫秒数（到时取消请求并 reject；handler
   *                                      晚到的 resolve/reject 被原生侧丢弃）
   * @returns {Promise<any>} handler 的返回值；桥接未就绪/超时/handler 抛错时 reject
   */
  invokeFromNativeAsync(type, input, requestId, timeoutMs = 15000) {
    return new Promise((resolve, reject) => {
      let settled = false;
      const finish = (fn, arg) => {
        if (settled) return;   // 晚到结果（已超时/已取消）静默丢弃
        settled = true;
        fn(arg);
      };
      addon.invokeProviderAsyncFromNative(
        type,
        JSON.stringify(input === undefined ? null : input),
        requestId,
        timeoutMs,
        (err, resultJson) => {
          if (err) {
            finish(reject, new Error(err));
            return;
          }
          try {
            finish(resolve, JSON.parse(resultJson));
          } catch (parseErr) {
            finish(reject, parseErr);
          }
        }
      );
    });
  },

  /**
   * 取消一个经 invokeFromNativeAsync 发起的请求（幂等；已回调/已取消时为 false）。
   * 取消后 handler 晚到的 resolve/reject 被原生侧按 requestId 丢弃。
   * @param {number} requestId - 请求号
   * @returns {boolean} 是否确由本次调用完成取消
   */
  cancelFromNative(requestId) {
    return addon.cancelProviderAsyncFromNative(requestId);
  }
};

// 导出所有类
module.exports = {
  Logger,
  ClipboardMonitor,
  WindowMonitor,
  WindowManager,
  ScreenCapture,
  MouseMonitor,
  ColorPicker,
  IconExtractor,
  UwpManager,
  MuiResolver,
  WindowsShortcutScanner,
  ProviderBridge,
  getSelectedContent,
  launchCuiShell
};

// 为了向后兼容，默认导出 ClipboardMonitor
module.exports.default = ClipboardMonitor;
