const { WindowManager } = require('../index.js');

console.log('测试获取所有文件资源管理器窗口的 URL...\n');

try {
  const urls = WindowManager.getAllExplorerWindows();

  console.log(`找到 ${urls.length} 个打开的文件资源管理器窗口：\n`);

  if (urls.length === 0) {
    console.log('没有打开的文件资源管理器窗口');
    console.log('\n提示：请打开一个或多个文件资源管理器窗口后再运行此测试');
  } else {
    urls.forEach((url, index) => {
      console.log(`[${index + 1}] ${url}`);
    });

    console.log('\n✅ 测试成功！');
  }
} catch (error) {
  console.error('❌ 错误:', error.message);
  console.error(error.stack);
}
