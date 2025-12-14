const fs = require('fs');
const path = require('path');
const { execSync } = require('child_process');

console.log('\n' + '='.repeat(60));
console.log('  编译测试');
console.log('='.repeat(60));
console.log('');

const platform = process.platform;
console.log(`平台: ${platform}\n`);

// 检查必要的文件
console.log('【步骤 1】检查源文件...');
const sourceFiles = {
  mac: 'src/binding_mac.cpp',
  windows: 'src/binding_windows.cpp',
  js: 'index.js'
};

let allFilesExist = true;
for (const [key, file] of Object.entries(sourceFiles)) {
  const filePath = path.join(__dirname, '..', file);
  if (fs.existsSync(filePath)) {
    console.log(`  ✅ ${file}`);
  } else {
    console.log(`  ❌ ${file} - 文件不存在`);
    allFilesExist = false;
  }
}

if (!allFilesExist) {
  console.log('\n❌ 源文件检查失败');
  process.exit(1);
}

console.log('\n【步骤 2】检查构建输出...');
const buildDir = path.join(__dirname, '..', 'build', 'Release');
const nodeFile = path.join(buildDir, 'ztools_event_hook.node');

if (fs.existsSync(nodeFile)) {
  console.log(`  ✅ ${nodeFile}`);
  const stats = fs.statSync(nodeFile);
  console.log(`     大小: ${(stats.size / 1024).toFixed(2)} KB`);
  console.log(`     修改时间: ${stats.mtime.toLocaleString()}`);
} else {
  console.log(`  ❌ ${nodeFile} - 文件不存在`);
  console.log('     提示: 请先运行 npm run build');
  process.exit(1);
}

console.log('\n【步骤 3】测试模块加载...');
try {
  const addon = require('../build/Release/ztools_event_hook.node');
  console.log('  ✅ 模块加载成功');
  
  // 检查导出的函数
  console.log('\n【步骤 4】检查导出的 API...');
  const requiredAPIs = [
    'hookEvent',
    'unhookEvent'
  ];
  
  let allAPIsExist = true;
  for (const api of requiredAPIs) {
    if (typeof addon[api] === 'function') {
      console.log(`  ✅ ${api}`);
    } else {
      console.log(`  ❌ ${api} - 未找到`);
      allAPIsExist = false;
    }
  }
  
  if (!allAPIsExist) {
    console.log('\n❌ API 检查失败');
    process.exit(1);
  }
  
  console.log('\n【步骤 5】测试 JavaScript 包装...');
  try {
    const EventHook = require('../index.js');
    if (typeof EventHook === 'function') {
      console.log('  ✅ EventHook 类已导出');
      
      // 测试实例化
      const hook = new EventHook();
      if (typeof hook.start === 'function' && typeof hook.stop === 'function') {
        console.log('  ✅ EventHook 实例方法正常');
      } else {
        console.log('  ❌ EventHook 实例方法缺失');
        process.exit(1);
      }
    } else {
      console.log('  ❌ EventHook 类未导出');
      process.exit(1);
    }
  } catch (error) {
    console.log(`  ❌ JavaScript 包装测试失败: ${error.message}`);
    process.exit(1);
  }
  
} catch (error) {
  console.log(`  ❌ 模块加载失败: ${error.message}`);
  console.log('\n提示:');
  console.log('  1. 确保已运行 npm run build');
  process.exit(1);
}

console.log('\n' + '='.repeat(60));
console.log('✅ 编译测试通过！');
console.log('='.repeat(60));
console.log('');

