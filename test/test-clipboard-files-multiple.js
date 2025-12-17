const { ClipboardMonitor } = require('../index');

console.log('\n========================================');
console.log('  剪贴板多次访问测试（Windows 11兼容性）');
console.log('========================================\n');

async function delay(ms) {
  return new Promise(resolve => setTimeout(resolve, ms));
}

async function runTest() {
  try {
    console.log('【测试 1】连续多次读取剪贴板');
    console.log('─'.repeat(60));

    // 连续读取 10 次，测试重试机制
    const iterations = 10;
    let successCount = 0;
    let failCount = 0;

    for (let i = 1; i <= iterations; i++) {
      try {
        const files = ClipboardMonitor.getClipboardFiles();
        console.log(`第 ${i} 次读取: ✅ 成功 (${files.length} 个文件)`);
        successCount++;

        // 打印前 3 个文件的信息
        if (files.length > 0 && i === 1) {
          console.log('  文件列表:');
          files.slice(0, 3).forEach((file, index) => {
            const type = file.isDirectory ? '📁' : '📄';
            console.log(`    ${type} ${file.name}`);
          });
          if (files.length > 3) {
            console.log(`    ... 还有 ${files.length - 3} 个文件`);
          }
        }
      } catch (error) {
        console.log(`第 ${i} 次读取: ❌ 失败 - ${error.message}`);
        failCount++;
      }

      // 短暂延迟，模拟实际使用场景
      if (i < iterations) {
        await delay(100);
      }
    }

    console.log('');
    console.log('【结果统计】');
    console.log('─'.repeat(60));
    console.log(`总计: ${iterations} 次`);
    console.log(`成功: ${successCount} 次 (${(successCount/iterations*100).toFixed(1)}%)`);
    console.log(`失败: ${failCount} 次 (${(failCount/iterations*100).toFixed(1)}%)`);

    if (failCount > 0) {
      console.log('\n⚠️  提示：如果多次失败，请检查：');
      console.log('  1. 剪贴板中是否有文件（在文件资源管理器中复制文件）');
      console.log('  2. 是否有其他程序占用剪贴板');
      console.log('  3. Windows 11 剪贴板历史功能是否干扰（Win+V）');
    }

    console.log('\n【测试 2】读取-写入-读取循环测试');
    console.log('─'.repeat(60));

    // 读取
    const originalFiles = ClipboardMonitor.getClipboardFiles();
    console.log(`步骤 1: 读取剪贴板 - ${originalFiles.length} 个文件`);

    if (originalFiles.length > 0) {
      await delay(100);

      // 写入
      const writeSuccess = ClipboardMonitor.setClipboardFiles(originalFiles);
      console.log(`步骤 2: 写入剪贴板 - ${writeSuccess ? '✅ 成功' : '❌ 失败'}`);

      await delay(100);

      // 再次读取验证
      const verifyFiles = ClipboardMonitor.getClipboardFiles();
      console.log(`步骤 3: 验证读取 - ${verifyFiles.length} 个文件`);

      if (verifyFiles.length === originalFiles.length) {
        console.log('✅ 读写循环测试通过！');
      } else {
        console.log(`⚠️  文件数量不一致: ${originalFiles.length} -> ${verifyFiles.length}`);
      }
    } else {
      console.log('⚠️  剪贴板中没有文件，跳过读写循环测试');
      console.log('   提示: 在文件资源管理器中复制一些文件后重新运行测试');
    }

    console.log('\n【测试 3】快速连续访问测试（压力测试）');
    console.log('─'.repeat(60));

    // 快速连续访问 20 次，无延迟
    const rapidIterations = 20;
    let rapidSuccess = 0;

    const startTime = Date.now();
    for (let i = 0; i < rapidIterations; i++) {
      try {
        const files = ClipboardMonitor.getClipboardFiles();
        rapidSuccess++;
      } catch (error) {
        // 忽略错误，只统计成功率
      }
    }
    const endTime = Date.now();
    const duration = endTime - startTime;

    console.log(`快速访问 ${rapidIterations} 次:`);
    console.log(`  成功: ${rapidSuccess}/${rapidIterations} 次 (${(rapidSuccess/rapidIterations*100).toFixed(1)}%)`);
    console.log(`  耗时: ${duration}ms (平均 ${(duration/rapidIterations).toFixed(1)}ms/次)`);

    if (rapidSuccess === rapidIterations) {
      console.log('  ✅ 压力测试通过！重试机制工作正常');
    } else if (rapidSuccess >= rapidIterations * 0.9) {
      console.log('  ⚠️  90% 以上成功，基本正常');
    } else {
      console.log('  ❌ 成功率较低，可能存在问题');
    }

    console.log('\n========================================');
    console.log('测试完成！');
    console.log('========================================\n');

  } catch (error) {
    console.error('❌ 测试过程中出错:', error.message);
    console.error(error.stack);
    process.exit(1);
  }
}

// 启动测试
console.log('提示: 请先在文件资源管理器中复制一些文件（Ctrl+C）');
console.log('按 Ctrl+C 可以随时退出测试\n');

// 处理 Ctrl+C
process.on('SIGINT', () => {
  console.log('\n\n用户中断测试');
  process.exit(0);
});

// 延迟启动，让用户看到提示
setTimeout(() => {
  runTest();
}, 500);
