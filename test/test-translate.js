// 截图翻译（编辑态工具栏「翻译」按钮）交互式测试：真机手工验收。
//
// 验证目标：原生层「ocr（行级坐标）→ 文本行空间聚类成段落 → 逐段 translation」
// 编排链路（聚类算法的纯逻辑回归见 scripts/run-translate-cluster-selftest.cmd）。
//   1. 本脚本注册 mock ProviderBridge（不依赖真实 provider / 插件）；
//   2. 启动 autoConfirm:false 截图，框选任意区域 → 进入编辑态；
//   3. 点击工具栏「翻译」按钮 → 应出现「正在识别并翻译…」气泡，随后译文以
//      白底圆角面板覆盖在选区上（mock 坐标按提交图像尺寸生成，恒在选区内）；
//   4. 点「确定」导出：译文面板合成进最终图像；ESC/取消则放弃。
//   5. 控制台会打印桥接调用序列与每次 translation 的入参（可核对段落拼接结果：
//      英文段内空格连接、中文紧排直连）。
//
// 平台注意：
//   - Windows：截图 UI 跑在独立线程，JS 事件循环照常前进，本脚本开箱即用。
//   - macOS：截图会话为非阻塞生命周期对象，AppKit 事件与 Node 事件循环都依赖
//     宿主进程自己的主事件循环驱动——纯 Node 宿主不运转 macOS 主事件循环，
//     请用 Electron 宿主运行本测试（macOS 全功能截图翻译的验收环境）：
//       npx electron test/electron-host.cjs test/test-translate.js
//     （翻译为异步 RPC，requestId 驱动；桥接层语义（同步/Promise/延迟/超时/取消/
//     晚到丢弃/并发）另由 test/test-provider-async-bridge.js 覆盖，后者无需 Electron）。
//
// 运行方式：
//   node test/test-translate.js            # 默认：6 行英文 → 2 个段落
//                                         # （段间距 + 段末短行触发边界判定）
//   MODE=cjk node test/test-translate.js   # 中文紧排 + 第二段首行缩进 → 2 个段落
//   MODE=plain node test/test-translate.js # 无坐标模式：OCR 只回整段 text，
//                                         # 验证整图兜底（整选区单面板）
//
// 真实微信 OCR 契约（含坐标）请另跑：node scripts/diag-ocr.cjs
'use strict';

const { ScreenCapture, ProviderBridge, Logger } = require('..');

const MODE = process.env.MODE === 'cjk' ? 'cjk' : process.env.MODE === 'plain' ? 'plain' : 'para';
const calls = [];

// PNG IHDR 宽高（base64 data URI）：mock OCR 需要按提交图像像素尺寸生成行级坐标
function pngSize(dataUri) {
  const b64 = String(dataUri).replace(/^data:image\/[a-z]+;base64,/i, '');
  const buf = Buffer.from(b64, 'base64');
  if (buf.length < 24 || buf.readUInt32BE(0) !== 0x89504e47) return { width: 400, height: 200 };
  return { width: buf.readUInt32BE(16), height: buf.readUInt32BE(20) };
}

// 生成一个文本行 block（比例坐标 → 提交图像内像素坐标）
function mkBlock(text, l, t, r, b, W, H) {
  return {
    text,
    left: Math.round(l * W),
    top: Math.round(t * H),
    right: Math.round(r * W),
    bottom: Math.round(b * H),
  };
}

// 按模式生成行级布局（比例值）：行高 ~4.5% 图高、段内行距 ~6% 图高
function layoutFor(mode, W, H) {
  if (mode === 'cjk') {
    // 第一段 4 行紧排（vGap≈1.5% 图高）满宽；第二段首行缩进 15% 图宽且段间距
    // 收紧（vGap 2.5% < 0.9×行高，不触发段间距边界）→ 边界只能由缩进判定
    const rows1 = [0.08, 0.14, 0.2, 0.26].map((t, i) => mkBlock(`第${i + 1}行内容`, 0.1, t, 0.9, t + 0.045, W, H));
    const rows2 = [0.33, 0.39].map((t, i) => mkBlock(`缩进段${i + 1}`, 0.25, t, 0.85, t + 0.045, W, H));
    return [...rows1, ...rows2];
  }
  // 默认：英文两段。段内 pitch 6%、行高 4.5%（vGap 1.5%）；段间距 5.5%
  // （vGap 超过 0.9×行高 → 边界）且第一段末行右端仅 50%（段末短行信号）
  const rows1 = [0.08, 0.14, 0.2, 0.26].map((t, i) =>
    mkBlock(`English line ${i + 1} of the first`, 0.1, t, 0.9, t + 0.045, W, H));
  rows1[3].right = Math.round(0.5 * W); // 段末短行
  const rows2 = [0.36, 0.42].map((t, i) =>
    mkBlock(`Second para line ${i + 1}`, 0.1, t, 0.85, t + 0.045, W, H));
  return [...rows1, ...rows2];
}

const EXPECT_PARAS = MODE === 'plain' ? 1 : 2;

console.log('=== 截图翻译交互式测试（%s模式） ===\n',
  MODE === 'plain' ? '无坐标兜底' : MODE === 'cjk' ? '中文紧排+缩进' : '英文两段落');
console.log('说明：');
console.log('1. 框选任意区域进入编辑态（工具栏出现）');
console.log('2. 点击工具栏「翻译」按钮 → 等待译文覆盖面板出现');
console.log('3. 点「确定」导出（译文合成进图像）或 ESC 取消');
console.log(`4. 原生日志（tag=translate）会打印 cluster 统计：${Logger.getPath()}\n`);

ProviderBridge.start(async (type, input) => {
  calls.push(type);
  console.log(`[bridge] invoke type="${type}"`);

  if (type === 'ocr') {
    if (MODE === 'plain') {
      // 无坐标契约：只有整段 text（模拟纯文本 AI 识别）→ 原生走整图兜底
      return { text: 'Hello World, this is the fallback whole text.' };
    }
    const { width, height } = pngSize(input.image);
    console.log(`[bridge]   image ${width}x${height}`);
    return {
      text: 'mock',
      blocks: layoutFor(MODE, width, height),
      confidence: 0.99,
    };
  }
  if (type === 'translation') {
    console.log(`[bridge]   text="${input.text}"`);
    return { text: `【译】${input.text}` };
  }
  throw new Error(`unknown provider type: ${type}`);
});

setTimeout(() => {
  ScreenCapture.start({ autoConfirm: false }, (result) => {
    ProviderBridge.stop();

    console.log('\n截图会话结束:');
    if (result.success) {
      console.log(`✅ 导出成功 ${result.width}x${result.height}，译文面板应已合成进图像（base64 可另存查看）`);
    } else {
      console.log(`❌ 已取消/失败: ${result.error || ''}`);
    }
    const ocrCount = calls.filter((t) => t === 'ocr').length;
    const trCount = calls.filter((t) => t === 'translation').length;
    console.log(`桥接调用序列: ${calls.join(' -> ') || '(未点击翻译)'}`);
    console.log(`  ocr=${ocrCount} 次, translation=${trCount} 次`
      + `（行级模式应 1 次 ocr + ${EXPECT_PARAS} 次 translation（按段，非按行）；无坐标模式 1 + 1）`);
    process.exit(0);
  });
}, 1000);

setTimeout(() => {
  console.log('\n⏱️  超时：120 秒内未完成，程序退出');
  ProviderBridge.stop();
  process.exit(1);
}, 120000);
