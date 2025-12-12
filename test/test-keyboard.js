const { WindowManager } = require('..');

console.log('=== 键盘模拟测试 ===\n');

// 检查平台
const platform = WindowManager.getPlatform();
console.log(`当前平台: ${platform}\n`);

// 测试前提示
console.log('⚠️  注意：这个测试会模拟键盘输入！');
console.log('请打开一个文本编辑器（如记事本、VS Code、TextEdit等）');
console.log('并将光标放在编辑区域中。');
console.log('\n测试将在 3 秒后开始...\n');

// 延迟3秒开始测试
setTimeout(() => {
  console.log('--- 开始测试 ---\n');

  // 测试1: 单个字母键
  console.log('测试1: 输入字母 "hello"');
  const letters = ['h', 'e', 'l', 'l', 'o'];
  letters.forEach((letter, index) => {
    setTimeout(() => {
      const result = WindowManager.simulateKeyboardTap(letter);
      console.log(`  ${letter}: ${result ? '✓' : '✗'}`);
    }, index * 200);
  });

  // 测试2: 空格键
  setTimeout(() => {
    console.log('\n测试2: 输入空格');
    const result = WindowManager.simulateKeyboardTap('space');
    console.log(`  space: ${result ? '✓' : '✗'}`);
  }, 1200);

  // 测试3: 单词 "world"
  setTimeout(() => {
    console.log('\n测试3: 输入字母 "world"');
    const letters2 = ['w', 'o', 'r', 'l', 'd'];
    letters2.forEach((letter, index) => {
      setTimeout(() => {
        const result = WindowManager.simulateKeyboardTap(letter);
        console.log(`  ${letter}: ${result ? '✓' : '✗'}`);
      }, index * 200);
    });
  }, 1500);

  // 测试4: Enter键
  setTimeout(() => {
    console.log('\n测试4: 按下 Enter 键');
    const result = WindowManager.simulateKeyboardTap('return');
    console.log(`  return: ${result ? '✓' : '✗'}`);
  }, 2700);

  // 测试5: 带 Shift 修饰键的字母（大写）
  setTimeout(() => {
    console.log('\n测试5: 输入大写 "HELLO" (使用 Shift)');
    const letters3 = ['h', 'e', 'l', 'l', 'o'];
    letters3.forEach((letter, index) => {
      setTimeout(() => {
        const result = WindowManager.simulateKeyboardTap(letter, 'shift');
        console.log(`  ${letter.toUpperCase()}: ${result ? '✓' : '✗'}`);
      }, index * 200);
    });
  }, 3000);

  // 测试6: 带 Meta 修饰键（复制操作）
  setTimeout(() => {
    console.log('\n测试6: 按下 Meta+C (复制)');
    const metaKey = platform === 'darwin' ? 'meta' : 'ctrl';
    const result = WindowManager.simulateKeyboardTap('c', metaKey);
    console.log(`  ${metaKey}+C: ${result ? '✓' : '✗'}`);
  }, 4200);

  // 测试7: 带多个修饰键
  setTimeout(() => {
    console.log('\n测试7: 按下 Meta+Shift+S');
    const metaKey = platform === 'darwin' ? 'meta' : 'ctrl';
    const result = WindowManager.simulateKeyboardTap('s', metaKey, 'shift');
    console.log(`  ${metaKey}+Shift+S: ${result ? '✓' : '✗'}`);
  }, 4500);

  // 测试8: Tab 键
  setTimeout(() => {
    console.log('\n测试8: 按下 Tab 键');
    const result = WindowManager.simulateKeyboardTap('tab');
    console.log(`  Tab: ${result ? '✓' : '✗'}`);
  }, 4800);

  // 测试9: 方向键
  setTimeout(() => {
    console.log('\n测试9: 按下方向键 (左、右、上、下)');
    const arrows = ['left', 'right', 'up', 'down'];
    arrows.forEach((arrow, index) => {
      setTimeout(() => {
        const result = WindowManager.simulateKeyboardTap(arrow);
        console.log(`  ${arrow}: ${result ? '✓' : '✗'}`);
      }, index * 200);
    });
  }, 5100);

  // 测试10: 数字键
  setTimeout(() => {
    console.log('\n测试10: 输入数字 "12345"');
    const numbers = ['1', '2', '3', '4', '5'];
    numbers.forEach((num, index) => {
      setTimeout(() => {
        const result = WindowManager.simulateKeyboardTap(num);
        console.log(`  ${num}: ${result ? '✓' : '✗'}`);
      }, index * 200);
    });
  }, 5900);

  // 测试完成
  setTimeout(() => {
    console.log('\n--- 测试完成 ---');
    console.log('\n如果文本编辑器中正确显示了以下内容，说明测试成功：');
    console.log('hello world');
    console.log('HELLO');
    console.log('12345');
    console.log('\n注意: 某些快捷键（如 Meta+C, Meta+Shift+S）可能会触发应用程序功能。');
  }, 7100);

}, 3000);
