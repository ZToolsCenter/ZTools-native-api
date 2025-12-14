const EventHook = require('../index');
const os = require('os');

const platform = os.platform();
console.log('\n' + '='.repeat(60));
console.log(`  事件钩子测试 (${platform})`);
console.log('='.repeat(60));
console.log('');

// 检查平台
if (platform !== 'darwin' && platform !== 'win32') {
  console.log('❌ 此测试仅支持 macOS 和 Windows 平台');
  process.exit(1);
}

// 事件计数器
let mouseEvents = 0;
let keyboardEvents = 0;

// 创建事件钩子实例
const eventHook = new EventHook();

console.log('⚠️  注意：');
console.log('  - macOS 需要辅助功能权限');
console.log('  - 测试将监听鼠标和键盘事件');
console.log('  - 请移动鼠标、点击鼠标、按下键盘进行测试');
console.log('  - 测试将在 30 秒后自动停止');
console.log('');

// 延迟 2 秒开始测试
setTimeout(() => {
  console.log('--- 开始监听事件 ---\n');
  
  try {
    // 启动事件钩子（监听鼠标和键盘）
    eventHook.start(3, (...args) => {
      console.log('args', args);
      // 判断事件类型
      if (args.length === 3 && typeof args[0] === 'number' && typeof args[1] === 'number') {
        // 鼠标事件: [eventCode, x, y]
        mouseEvents++;
        const [eventCode, x, y] = args;
        const time = new Date().toLocaleTimeString();
        
        let eventName = '';
        if (platform === 'darwin') {
          switch (eventCode) {
            case 1: eventName = '左键按下'; break;
            case 2: eventName = '左键抬起'; break;
            case 3: eventName = '右键按下'; break;
            case 4: eventName = '右键抬起'; break;
            default: eventName = `未知(${eventCode})`;
          }
        } else {
          switch (eventCode) {
            case 0x0201: eventName = '左键按下'; break;
            case 0x0202: eventName = '左键抬起'; break;
            case 0x0204: eventName = '右键按下'; break;
            case 0x0205: eventName = '右键抬起'; break;
            default: eventName = `未知(0x${eventCode.toString(16)})`;
          }
        }
        
        console.log(`  [${time}] 🖱️  鼠标: ${eventName} @ (${x}, ${y})`);
      } else if (args.length === 6 && typeof args[0] === 'string') {
        // 键盘事件: [keyName, shiftKey, ctrlKey, altKey, metaKey, flagsChange]
        keyboardEvents++;
        const [keyName, shiftKey, ctrlKey, altKey, metaKey, flagsChange] = args;
        const time = new Date().toLocaleTimeString();
        
        const modifiers = [];
        if (shiftKey) modifiers.push('Shift');
        if (ctrlKey) modifiers.push('Ctrl');
        if (altKey) modifiers.push(platform === 'darwin' ? 'Option' : 'Alt');
        if (metaKey) modifiers.push(platform === 'darwin' ? 'Command' : 'Win');
        
        const modifierStr = modifiers.length > 0 ? ` [${modifiers.join('+')}]` : '';
        const flagsStr = flagsChange ? ' (修饰键变化)' : '';
        
        console.log(`  [${time}] ⌨️  键盘: ${keyName}${modifierStr}${flagsStr}`);
      }
    });
    
    console.log('✅ 事件钩子已启动\n');
  } catch (error) {
    console.error('❌ 启动事件钩子失败:', error.message);
    if (platform === 'darwin' && error.message.includes('permission')) {
      console.log('\n提示: 请在系统偏好设置中授予辅助功能权限');
    }
    process.exit(1);
  }
  
  // 倒计时显示
//   let remaining = 30;
//   const countdown = setInterval(() => {
//     process.stdout.write(`\r  剩余时间: ${remaining} 秒... (鼠标: ${mouseEvents}, 键盘: ${keyboardEvents})`);
//     remaining--;
//     if (remaining < 0) {
//       clearInterval(countdown);
//     }
//   }, 1000);
  
  // 30秒后停止监听
//   setTimeout(() => {
//     clearInterval(countdown);
//     process.stdout.write('\r');
//     console.log('');
//     console.log('='.repeat(60));
//     console.log('【测试结果】');
//     console.log(`  鼠标事件: ${mouseEvents} 个`);
//     console.log(`  键盘事件: ${keyboardEvents} 个`);
//     console.log('='.repeat(60));
//     console.log('');
//     console.log('✅ 测试完成，事件钩子已停止');
//     console.log('');
    
//     eventHook.stop();
//     process.exit(0);
//   }, 30000);
  
  // 处理 Ctrl+C
  process.on('SIGINT', () => {
    clearInterval(countdown);
    console.log('\n\n用户中断测试');
    console.log(`  鼠标事件: ${mouseEvents} 个`);
    console.log(`  键盘事件: ${keyboardEvents} 个`);
    eventHook.stop();
    process.exit(0);
  });
  
}, 2000);

