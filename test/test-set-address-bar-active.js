const { WindowManager } = require('../index.js');
const os = require('os');
const path = require('path');

const targetAddress = os.homedir();

console.log('测试 3 秒后获取当前 active 窗口并设置地址为用户目录...\n');
console.log(`目标地址: ${targetAddress}`);
console.log('提示: 请在 3 秒内激活一个文件资源管理器/Finder 窗口或文件选择对话框。\n');

let remaining = 3;
const countdown = setInterval(() => {
  process.stdout.write(`\r剩余时间: ${remaining} 秒...`);
  remaining -= 1;

  if (remaining < 0) {
    clearInterval(countdown);
  }
}, 1000);

setTimeout(() => {
  clearInterval(countdown);
  process.stdout.write('\r');

  try {
    const activeWindow = WindowManager.getActiveWindow();

    if (!activeWindow) {
      console.log('❌ 获取当前 active 窗口失败');
      process.exit(1);
    }

    console.log('当前 active 窗口:');
    console.log(`  应用: ${activeWindow.appName || activeWindow.app || '未知'}`);
    console.log(`  标题: ${activeWindow.title || '无'}`);

    if (process.platform === 'win32') {
      console.log(`  HWND: ${activeWindow.hwnd}`);
    } else if (process.platform === 'darwin') {
      console.log(`  Bundle ID: ${activeWindow.bundleId || '无'}`);
    }

    const success = WindowManager.setAddressBar(activeWindow, targetAddress);

    if (success) {
      console.log(`\n✅ 设置地址成功: ${path.normalize(targetAddress)}`);
      process.exit(0);
    }

    console.log('\n❌ 设置地址失败，请确认当前 active 窗口是文件资源管理器/Finder 或文件选择对话框');
    process.exit(1);
  } catch (error) {
    console.error('\n❌ 错误:', error.message);
    console.error(error.stack);
    process.exit(1);
  }
}, 3000);

process.on('SIGINT', () => {
  clearInterval(countdown);
  console.log('\n\n用户中断测试');
  process.exit(130);
});
