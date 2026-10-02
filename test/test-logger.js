// 原生日志模块测试：日志文件、等级控制、JS 侧写入、轮转上限
// 运行：node test/test-logger.js
const fs = require('fs');
const path = require('path');
const os = require('os');
const { execFileSync } = require('child_process');

const { Logger, WindowManager } = require('..');

let passed = 0;
let failed = 0;

function check(name, condition, detail) {
  if (condition) {
    console.log(`✓ ${name}`);
    passed++;
  } else {
    console.error(`✗ ${name}${detail ? `: ${detail}` : ''}`);
    failed++;
  }
}

function expectThrows(name, fn) {
  try {
    fn();
    check(name, false, '未抛出异常');
  } catch (err) {
    check(name, err instanceof TypeError, `应抛 TypeError，实际: ${err}`);
  }
}

console.log('=== 原生日志模块测试 ===\n');

// 1) 日志文件路径：位于系统临时目录下
const logPath = Logger.getPath();
console.log(`日志文件: ${logPath}`);
check('getPath 返回字符串', typeof logPath === 'string' && logPath.length > 0);
check('getPath 指向临时目录', path.dirname(logPath) === path.resolve(path.dirname(logPath)) &&
  logPath.toLowerCase().startsWith(os.tmpdir().toLowerCase()));
check('日志文件名为 ztools-native.log', path.basename(logPath) === 'ztools-native.log');

// 2) 默认等级 info
check('默认等级为 info', Logger.getLevel() === 'info');
check('info 等级启用', Logger.isLevelEnabled('info') === true);
check('debug 等级默认关闭', Logger.isLevelEnabled('debug') === false);

// 3) 运行时切换等级
Logger.setLevel('debug');
check('setLevel(debug) 后 getLevel 为 debug', Logger.getLevel() === 'debug');
check('debug 等级启用', Logger.isLevelEnabled('debug') === true);

Logger.setLevel('off');
check('setLevel(off) 后任何等级都不输出', Logger.isLevelEnabled('error') === false);
Logger.write('error', 'test-logger', 'OFF-MARKER-不应出现');

Logger.setLevel('warn');
check('setLevel(warn) 后 warn 启用', Logger.isLevelEnabled('warn') === true);
check('setLevel(warn) 后 info 关闭', Logger.isLevelEnabled('info') === false);

Logger.setLevel('trace');
check('setLevel(trace) 后 trace 启用', Logger.isLevelEnabled('trace') === true);
Logger.setLevel('info');

// 4) 非法参数
expectThrows('setLevel(非法等级) 抛 TypeError', () => Logger.setLevel('verbose'));
expectThrows('isLevelEnabled(123) 抛 TypeError', () => Logger.isLevelEnabled(123));
expectThrows('write 缺少 tag 抛 TypeError', () => Logger.write('info', '', 'x'));
expectThrows('write(off) 抛 TypeError', () => Logger.write('off', 'app', 'x'));

// 5) 触发几条原生日志 + JS 侧写入
WindowManager.getActiveWindow();
Logger.write('info', 'test-logger', 'JS 侧写入测试');
Logger.write('error', 'test-logger', 'JS 侧 error 写入测试');

// 等待落盘（逐条 fflush，通常立即完成）
const deadline = Date.now() + 3000;
let content = '';
while (Date.now() < deadline) {
  try {
    content = fs.readFileSync(logPath, 'utf8');
  } catch (err) {
    content = '';
  }
  if (content.includes('JS 侧写入测试')) break;
  Atomics.wait(new Int32Array(new SharedArrayBuffer(4)), 0, 0, 100);
}

check('日志文件已创建', fs.existsSync(logPath));
check('会话起始行存在', content.includes('=== ztools_native loaded'));
check('包含日志等级标签', content.includes('[info]'));
check('包含 tag 字段', content.includes('[test-logger]'));
check('包含 JS 侧写入内容', content.includes('JS 侧写入测试'));
check('时间戳格式正确', /\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d{3}/.test(content));
check('off 等级期间的日志未写入', !content.includes('OFF-MARKER'));

// 6) 环境变量：子进程中设置 ZTOOLS_LOG_LEVEL=debug，getLevel 应读到 debug
const envDebug = { ...process.env, ZTOOLS_LOG_LEVEL: 'debug' };
const out = execFileSync(
  process.execPath,
  ['-e', 'const { Logger } = require(process.cwd()); console.log(Logger.getLevel());'],
  { env: envDebug, encoding: 'utf8', cwd: path.join(__dirname, '..') }
);
check('环境变量 ZTOOLS_LOG_LEVEL=debug 生效', out.trim() === 'debug');

// 7) 轮转：单条消息截断到 8KB，写入约 12MB 后应轮转出 .old 且当前文件不超上限
console.log('\n（轮转测试：写入约 12MB 日志，可能需要数秒...）');
Logger.setLevel('info');
const bigMessage = 'x'.repeat(200000); // 会被截断为单条约 8KB
for (let i = 0; i < 1500; i++) {
  Logger.write('info', 'rotate-test', bigMessage);
}
const oldPath = logPath + '.old';
check('轮转后生成 .old 文件', fs.existsSync(oldPath));
const currentSize = fs.statSync(logPath).size;
check(`当前文件不超过 10MB（实际 ${(currentSize / 1024 / 1024).toFixed(2)}MB）`,
  currentSize <= 10 * 1024 * 1024);
check('当前文件重新从新内容开始',
  !fs.readFileSync(logPath, 'utf8').includes('JS 侧写入测试'));

console.log(`\n=== 结果: ${passed} 通过, ${failed} 失败 ===`);
process.exit(failed > 0 ? 1 : 0);
