const os = require('os');

// 根据平台加载对应的原生模块
const addon = require('./build/Release/ztools_event_hook.node');
const platform = os.platform();

// 事件钩子类
class EventHook {
  constructor() {
    this._callback = null;
    this._isHooking = false;
  }

  /**
   * 启动事件钩子
   * @param {number} effect - 事件类型
   * - 1: 仅监听鼠标事件
   * - 2: 仅监听键盘事件
   * - 3: 同时监听鼠标和键盘事件
   * @param {Function} callback - 事件回调函数
   * 
   * 鼠标事件回调参数：
   * - macOS: callback(eventCode: number, x: number, y: number)
   * - Windows: callback(eventCode: number, x?: number, y?: number)
   * 
   * 键盘事件回调参数：
   * - callback(keyName: string, shiftKey: boolean, ctrlKey: boolean, altKey: boolean, metaKey: boolean, flagsChange: boolean)
   */
  start(effect, callback) {
    if (this._isHooking) {
      throw new Error('Event hook is already running');
    }

    if (typeof effect !== 'number' || effect < 1 || effect > 3) {
      throw new TypeError('effect must be 1 (mouse), 2 (keyboard), or 3 (both)');
    }

    if (typeof callback !== 'function') {
      throw new TypeError('Callback must be a function');
    }

    this._callback = callback;
    this._isHooking = true;

    addon.hookEvent(effect, (...args) => {
      if (this._callback) {
        this._callback(...args);
      }
    });
  }

  /**
   * 停止事件钩子
   */
  stop() {
    if (!this._isHooking) {
      return;
    }

    addon.unhookEvent();
    this._isHooking = false;
    this._callback = null;
  }

  /**
   * 是否正在监听
   */
  get isHooking() {
    return this._isHooking;
  }
}

// 导出
module.exports = EventHook;
module.exports.default = EventHook;

