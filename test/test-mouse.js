const { MouseMonitor } = require('../index');

// 配置：修改这两个参数来测试不同的按钮和模式
const buttonType = 'right';   // 'middle' | 'right' | 'back' | 'forward'
const longPressMs = 200;       // 0=点击, >0=长按阈值(ms)

const mode = longPressMs > 0 ? `长按(${longPressMs}ms)` : '点击';

console.log('=== 鼠标监听测试 ===');
console.log(`按钮: ${buttonType}`);
console.log(`模式: ${mode}`);
console.log('');
console.log('按 Ctrl+C 停止');
console.log('---');

MouseMonitor.start(buttonType, longPressMs, async () => {
  const time = new Date().toLocaleTimeString();
  console.log(`[${time}] 事件触发`);
  return { shouldBlock: false }
});

process.on('SIGINT', () => {
  console.log('\n停止鼠标监听...');
  MouseMonitor.stop();
  console.log('已停止');
  process.exit(0);
});
