const { getSelectedContent } = require('../index.js');

console.log('=== 测试获取选中内容功能 ===\n');
console.log('支持文本、文件、图像三种类型');
console.log('请在任意应用中选中一些内容（文本/文件/图像）...');
console.log('将在 3 秒后自动获取选中的内容\n');

// 倒计时
let countdown = 3;
const timer = setInterval(() => {
  console.log(`${countdown}...`);
  countdown--;

  if (countdown === 0) {
    clearInterval(timer);
    console.log('\n正在获取选中的内容...\n');

    try {
      const contents = getSelectedContent();

      if (contents && contents.length > 0) {
        console.log(`✓ 成功获取 ${contents.length} 项内容:\n`);

        contents.forEach((item, index) => {
          console.log(`[${index + 1}] 类型: ${item.type}`);
          console.log('----------------------------------------');

          switch (item.type) {
            case 'text':
              console.log('文本内容:');
              console.log(item.data);
              console.log(`文本长度: ${item.data.length} 字符`);
              break;

            case 'file':
              console.log('文件列表:');
              item.data.forEach((file, i) => {
                console.log(`  ${i + 1}. ${file}`);
              });
              console.log(`文件数量: ${item.data.length}`);
              break;

            case 'image':
              console.log('图像信息:');
              console.log(`  格式: ${item.format}`);
              console.log(`  编码: ${item.encoding}`);
              console.log(`  数据长度: ${item.data.length} 字符 (base64)`);
              console.log(`  预览: ${item.data.substring(0, 50)}...`);
              break;

            default:
              console.log('未知类型:', item);
          }

          console.log('----------------------------------------\n');
        });
      } else {
        console.log('✗ 当前没有选中内容');
        console.log('提示: 请在任意应用中选中文本、文件或图像后再运行此测试');
      }
    } catch (error) {
      console.error('✗ 发生错误:', error.message);
      console.error(error.stack);
    }

    console.log('测试完成！');
    process.exit(0);
  }
}, 1000);
