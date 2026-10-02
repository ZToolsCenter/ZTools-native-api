/**
 * Electron 宿主（macOS 截图会话的验收环境）
 *
 * macOS 截图会话为非阻塞生命周期对象：AppKit 事件（覆盖层鼠标/键盘/绘制）与
 * 周期任务（翻译回投 drain/插入符闪烁/长截图采样）都由宿主进程自己的主事件循环
 * 驱动。Electron 主进程的 Chromium 消息泵天然同时运转 NSApp 与 libuv，是这套
 * 架构的标准宿主；纯 Node 宿主不运转 macOS 主事件循环，无法运行截图会话。
 *
 * 用法（在仓库根目录）：
 *   npx electron test/electron-host.cjs test/test-translate.js
 *   npx electron test/electron-host.cjs test/test-screenshot.js
 *
 * 说明：
 * - 需要本地可用的 electron（npx 会按需拉取，或全局/项目内已安装）；
 * - 目标脚本照常按普通 Node 脚本编写（require('..') 等），无需感知 Electron；
 * - 进程退出交由目标脚本自身控制（截图测试在回调/超时里 process.exit）。
 */
'use strict';

const path = require('path');
const { app } = require('electron');

const target = process.argv[2];
if (!target) {
  console.error('用法: npx electron test/electron-host.cjs <target-script.js> [args...]');
  app.exit(1);
}

const targetPath = path.resolve(process.cwd(), target);
// 让目标脚本读到干净的 argv（[electron, target, ...rest]）
process.argv = [process.argv[0], targetPath, ...process.argv.slice(3)];

app.whenReady().then(() => {
  try {
    require(targetPath);
  } catch (err) {
    console.error('目标脚本加载失败:', err);
    app.exit(1);
  }
});
