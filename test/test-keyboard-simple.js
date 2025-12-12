const { WindowManager } = require('..');

console.log('=== simulateKeyboardTap 简单示例 ===\n');

console.log('请打开一个文本编辑器，测试将在 2 秒后开始...\n');

setTimeout(() => {
  // 基本用法：输入单个字母
  WindowManager.simulateKeyboardTap('a');
  console.log('✓ 输入: a');

  setTimeout(() => {
    // 使用修饰键：Shift + A（输入大写 A）
    WindowManager.simulateKeyboardTap('a', 'shift');
    console.log('✓ 输入: A (shift+a)');
  }, 300);

  setTimeout(() => {
    // 使用 Command/Ctrl 修饰键（复制）
    const modifier = process.platform === 'darwin' ? 'meta' : 'ctrl';
    WindowManager.simulateKeyboardTap('c', modifier);
    console.log(`✓ 执行: ${modifier}+c (复制)`);
  }, 600);

  setTimeout(() => {
    // 使用多个修饰键
    const modifier = process.platform === 'darwin' ? 'meta' : 'ctrl';
    WindowManager.simulateKeyboardTap('s', modifier, 'shift');
    console.log(`✓ 执行: ${modifier}+shift+s (另存为)`);
  }, 900);

  setTimeout(() => {
    // 特殊键：Enter
    WindowManager.simulateKeyboardTap('return');
    console.log('✓ 输入: Enter');
  }, 1200);

  setTimeout(() => {
    console.log('\n完成！');
  }, 1500);

}, 2000);
