/**
 * 取色器验收脚本（Windows）。
 *
 * 用法（本目录下）：
 *   <electron> test/electron-host.cjs test/verify-color-picker.js
 * 例（npx electron / 本地已装的 electron 均可）：
 *   npx electron test/electron-host.cjs test/verify-color-picker.js
 *
 * 之所以要过 Electron 宿主：`startColorPicker` 的回调走 N-API threadsafe function，
 * 纯 Node 主线程被 Atomics.wait 同步阻塞时派发不出去，会一直等到超时。
 *
 * 断言：
 *   1. 副屏取色 —— 副屏与主屏不重叠时，副屏上的颜色必须取得回来
 *                  （旧实现在这里恒返回 #FFFFFF）
 *   2. 实时取样 —— 取色器启动之后再改屏幕内容，必须反映到结果里（DDA 实时采样；
 *                  旧快照实现里画面停在启动那一刻）
 *
 * 两块屏由运行时探测，不写死本机坐标：挑一块与主屏矩形不重叠的显示器，
 * 取它的中心点。没有这样的显示器时跳过断言 1 和 2（断言 2 依赖副屏窗口）并说明原因。
 *
 * 坐标：原生侧（GetCursorPos / DDA 帧）用的是**物理像素**，BrowserWindow 的 x/y 是
 * **DIP**，所以摆窗口前要过一次 screenToDipPoint。
 *
 * 跑之前屏幕必须处于解锁且可见状态，否则取到的是锁屏画面，结果无意义；
 * 脚本会先用主屏上的参照色块自检，发现屏幕不可见时直接报错退出。
 */
'use strict';

const { app, BrowserWindow, screen } = require('electron');
const path = require('path');
const fs = require('fs');

const addon = require(path.join(__dirname, '..', 'build', 'Release', 'ztools_native.node'));
const { ColorPicker } = require('..');

const BOX_DIP = 600;
const COLOR_A = '#FF00FF';
const COLOR_B = '#00C8FF';
const PRIMARY_FILL = '#00FF88';

/**
 * 在物理像素与 DIP 之间折算。Electron 给的是 DIP，原生侧要的是物理像素，
 * 两个方向都经 screen 的换算接口，不自己拿 scaleFactor 乘（多屏 scale 不同）。
 *
 * @param {{x:number,y:number}} point
 * @returns {{x:number,y:number}}
 */
const toPhysical = (point) => screen.dipToScreenPoint({ x: point.x, y: point.y });

/**
 * 选出一块与主屏**不重叠**的显示器（触发旧实现副屏 bug 的几何条件）。
 * 没有这样的显示器时返回 null，由调用方跳过对应断言。
 *
 * @returns {{label:string, center:{x:number,y:number}}|null}
 */
function findNonOverlappingDisplay() {
  const primary = screen.getPrimaryDisplay().bounds;
  const primaryDip = toPhysical({ x: primary.x, y: primary.y });
  const primaryW = primary.width * primary.scaleFactor;
  const primaryH = primary.height * primary.scaleFactor;
  const overlaps = (r) =>
    r.left < primaryDip.x + primaryW && r.right > primaryDip.x &&
    r.top < primaryDip.y + primaryH && r.bottom > primaryDip.y;

  for (const display of screen.getAllDisplays()) {
    if (display.id === screen.getPrimaryDisplay().id) continue;
    const dip = display.bounds;
    const origin = toPhysical({ x: dip.x, y: dip.y });
    const rect = {
      left: origin.x,
      right: origin.x + dip.width * display.scaleFactor,
      top: origin.y,
      bottom: origin.y + dip.height * display.scaleFactor,
    };
    if (overlaps(rect)) continue; // 与主屏有交集，取色不会越界，测不出问题
    return {
      label: 'secondary display ' + display.id,
      center: { x: Math.round((rect.left + rect.right) / 2), y: Math.round((rect.top + rect.bottom) / 2) },
    };
  }
  return null;
}

/**
 * 走真实代码路径取一次色：移动鼠标，等放大镜跟上，再点左键确认。
 *
 * @param {number} x 物理像素 x
 * @param {number} y 物理像素 y
 * @param {() => Promise<void>|void} [duringPick] 取色过程中（点击之前）执行的动作
 * @returns {Promise<string>} 取到的色值
 */
function pickAt(x, y, duringPick) {
  return new Promise((resolve, reject) => {
    const timer = setTimeout(() => {
      try { ColorPicker.stop(); } catch { /* ignore */ }
      reject(new Error('超时：取色回调没有触发'));
    }, 20000);

    ColorPicker.start((result) => {
      clearTimeout(timer);
      if (!result || !result.success || !result.hex) {
        reject(new Error('取色被取消：' + JSON.stringify(result)));
        return;
      }
      resolve(result.hex);
    });

    const settle = () => new Promise((r) => setTimeout(r, 300));

    (async () => {
      addon.simulateMouseMove(x, y);
      await settle();
      addon.simulateMouseMove(x, y);
      await settle();
      if (duringPick) await duringPick();
      addon.simulateMouseMove(x, y);
      await settle();
      addon.simulateMouseClick(x, y);
    })().catch(reject);
  });
}

/**
 * 在指定物理坐标上摆一个纯色窗口。
 *
 * @param {{x:number,y:number}} point 物理像素坐标
 * @param {string} fill 填色
 * @returns {import('electron').BrowserWindow}
 */
function makeBox(point, fill) {
  const dip = screen.screenToDipPoint({ x: point.x, y: point.y });
  const win = new BrowserWindow({
    x: Math.round(dip.x - BOX_DIP / 2),
    y: Math.round(dip.y - BOX_DIP / 2),
    width: BOX_DIP,
    height: BOX_DIP,
    frame: false,
    skipTaskbar: true,
    focusable: false,
  });
  win.setAlwaysOnTop(true, 'screen-saver');
  paint(win, fill);
  win.show();
  return win;
}

/**
 * 给窗口换一个纯色页面。
 *
 * @param {import('electron').BrowserWindow} win 窗口
 * @param {string} fill 填色
 * @returns {void}
 */
function paint(win, fill) {
  win.loadURL('data:text/html,' + encodeURIComponent(
    '<body style="margin:0;overflow:hidden;background:' + fill + '"></body>') + '#' + Date.now());
}

/**
 * 判断取到的颜色是否「大致是」参照色。只用来确认屏幕可见，
 * 高 DPI 下窗口合成会被冲淡（1.25 倍缩放的屏上 #00FF88 实测取到 #77FA8B），
 * 所以不比精确值，只看主色通道有没有压过其它两个通道。
 *
 * @param {string} picked 取到的色值
 * @param {'green'|'magenta'|'cyan'} hue 参照色的主色
 * @returns {boolean}
 */
function looksLike(picked, hue) {
  const [r, g, b] = [1, 3, 5].map((i) => parseInt(String(picked).slice(i, i + 2), 16));
  if ([r, g, b].some(Number.isNaN)) return false;
  if (hue === 'green') return g > 180 && g - r > 40 && g - b > 40;
  if (hue === 'magenta') return r > 120 && b > 120 && g < r - 40 && g < b - 40;
  return b > 120 && g > 120 && r < b - 40;
}

async function main() {
  const report = { platform: process.platform, probes: [] };
  const finish = () => {
    report.passed = report.probes.filter((p) => p.pass).length;
    report.total = report.probes.filter((p) => typeof p.pass === 'boolean').length;
    fs.writeFileSync(path.join(app.getPath('temp'), 'ztools-picker-verify.json'),
      JSON.stringify(report, null, 2));
    console.log(JSON.stringify(report, null, 2));
    app.exit(report.total > 0 && report.passed === report.total ? 0 : 1);
  };

  // 参照点：主屏中心。窗口盖住它，取不到对应色说明屏幕不可见（锁屏 / 关屏）
  const primary = screen.getPrimaryDisplay().bounds;
  const primaryCenter = toPhysical({ x: primary.x + primary.width / 2, y: primary.y + primary.height / 2 });
  const primaryPoint = { x: Math.round(primaryCenter.x), y: Math.round(primaryCenter.y) };

  makeBox(primaryPoint, PRIMARY_FILL);
  await new Promise((r) => setTimeout(r, 2000));

  const primaryRef = await pickAt(primaryPoint.x, primaryPoint.y);
  report.primaryReference = { point: primaryPoint, picked: primaryRef, expected: PRIMARY_FILL };
  if (!looksLike(primaryRef, 'green')) {
    report.environment = '屏幕上看不到测试窗口（锁屏 / 关屏 / 未解锁？），取到的是 '
      + primaryRef + '，本次结果无效';
    fs.writeFileSync(path.join(app.getPath('temp'), 'ztools-picker-verify.json'),
      JSON.stringify(report, null, 2));
    console.error(report.environment);
    app.exit(2);
    return;
  }

  // 1. 副屏取色：只在存在「与主屏不重叠的显示器」时才有意义
  const secondary = findNonOverlappingDisplay();
  let secondaryBox = null;
  if (!secondary) {
    report.probes.push({
      label: 'secondary monitor',
      skipped: '没有与主屏不重叠的显示器，副屏取色无从断言',
    });
  } else {
    secondaryBox = makeBox(secondary.center, COLOR_A);
    try {
      const picked = await pickAt(secondary.center.x, secondary.center.y);
      report.probes.push({
        label: secondary.label,
        point: secondary.center,
        expected: COLOR_A,
        picked,
        pass: looksLike(picked, 'magenta'),
      });
    } catch (err) {
      report.probes.push({
        label: secondary.label, point: secondary.center,
        error: String(err.message || err), pass: false,
      });
    }
  }

  // 2. 实时取样：取色器启动后再改屏幕内容，结果必须跟着变
  if (secondaryBox) {
    try {
      const picked = await pickAt(secondary.center.x, secondary.center.y, () => {
        paint(secondaryBox, COLOR_B);
        return new Promise((r) => setTimeout(r, 700));
      });
      report.probes.push({
        label: 'live sampling',
        point: secondary.center,
        expected: COLOR_B,
        picked,
        pass: looksLike(picked, 'cyan'),
      });
    } catch (err) {
      report.probes.push({
        label: 'live sampling', point: secondary.center,
        error: String(err.message || err), pass: false,
      });
    }
  } else {
    report.probes.push({ label: 'live sampling', skipped: '缺少可用的副屏探针点' });
  }

  finish();
}

app.whenReady().then(main);