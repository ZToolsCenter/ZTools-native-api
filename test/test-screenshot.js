const { ScreenCapture } = require('..');

console.log('=== 区域截图测试 ===\n');
console.log('说明：');
console.log('1. 程序将创建全屏半透明遮罩');
console.log('2. 鼠标会变成十字光标');
console.log('3. 拖拽鼠标选择要截图的区域');
console.log('4. 释放鼠标后截图会自动保存到剪贴板');
console.log('5. 按 ESC 键可以取消截图');
console.log('\n准备启动...\n');

setTimeout(() => {
    console.log('启动区域截图...\n');

    ScreenCapture.start({ autoConfirm: false }, (result) => {
        console.log('截图完成！');
        console.log('结果:', result);

        if (result.success) {
            console.log(`\n✅ 截图成功！`);
            console.log(`   尺寸: ${result.width} x ${result.height}`);
            console.log(`   截图已保存到剪贴板，可以按 Ctrl+V 粘贴`);
        } else {
            console.log('\n❌ 截图已取消或失败');
        }

        console.log('\n测试完成！');
        process.exit(0);
    });
}, 1000);

// 防止程序退出
setTimeout(() => {
    console.log('\n⏱️  超时：60秒内未完成截图，程序退出');
    process.exit(1);
}, 60000);
