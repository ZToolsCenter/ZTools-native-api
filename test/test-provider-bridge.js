/**
 * Provider 桥接层端到端测试
 *
 * 验证链路：JS 发起 invokeFromNative → 原生线程 → 线程安全函数调度回 JS 线程
 * → JS handler 执行 → 结果按 seq 回传原生 → 原生线程返回 → JS Promise resolve
 *
 * 运行：node test/test-provider-bridge.js
 */
const assert = require('assert');
const { ProviderBridge } = require('..');

async function testNotStarted() {
  assert.strictEqual(ProviderBridge.isReady(), false, 'bridge should not be ready before start');
  await assert.rejects(
    () => ProviderBridge.invokeFromNative('echo', {}, 1000),
    /provider bridge not started/,
    'invoke before start should fail'
  );
  console.log('✓ 未启动时调用给出明确错误');
}

async function testEcho() {
  const seen = [];
  ProviderBridge.start(async (type, input) => {
    seen.push({ type, input });
    if (type === 'echo') {
      return { echoed: input, from: 'js-handler' };
    }
    if (type === 'slow') {
      await new Promise((resolve) => setTimeout(resolve, 200));
      return { delayed: true };
    }
    if (type === 'fail') {
      throw new Error('handler exploded');
    }
    throw new Error('unknown type: ' + type);
  });

  assert.strictEqual(ProviderBridge.isReady(), true, 'bridge should be ready after start');

  const result = await ProviderBridge.invokeFromNative('echo', { text: 'hello', n: 42 });
  assert.deepStrictEqual(result, { echoed: { text: 'hello', n: 42 }, from: 'js-handler' });
  assert.strictEqual(seen.length, 1);
  assert.strictEqual(seen[0].type, 'echo');
  console.log('✓ 原生线程 → JS handler → 原生线程 的往返调用成功，数据完整');
}

async function testConcurrency() {
  const inputs = [1, 2, 3, 4, 5, 6, 7, 8];
  const results = await Promise.all(
    inputs.map((n) => ProviderBridge.invokeFromNative('echo', { n }))
  );
  results.forEach((r, i) => {
    assert.strictEqual(r.echoed.n, inputs[i], `concurrent call ${i} payload mismatch`);
  });
  console.log('✓ 并发调用按 seq 正确配对，无串扰');
}

async function testHandlerError() {
  await assert.rejects(
    () => ProviderBridge.invokeFromNative('fail', {}, 5000),
    /handler exploded/,
    'handler error should propagate'
  );
  console.log('✓ handler 抛错正确回传为调用失败');
}

async function testTimeout() {
  await assert.rejects(
    () => ProviderBridge.invokeFromNative('slow', {}, 50),
    /timed out/,
    'slow handler with short timeout should time out'
  );
  // 超时后再用足够长的超时调一次，确认桥接未被超时破坏（迟到结果被丢弃）
  const ok = await ProviderBridge.invokeFromNative('slow', {}, 5000);
  assert.deepStrictEqual(ok, { delayed: true });
  console.log('✓ 超时正确返回，且超时不影响后续调用');
}

async function testStop() {
  ProviderBridge.stop();
  assert.strictEqual(ProviderBridge.isReady(), false, 'bridge should not be ready after stop');

  // stop 后重新 start 仍可正常工作
  ProviderBridge.start(async () => ({ ok: true }));
  const result = await ProviderBridge.invokeFromNative('anything', {});
  assert.deepStrictEqual(result, { ok: true });
  ProviderBridge.stop();
  console.log('✓ stop 后状态清理正确，且支持重新启动');
}

(async () => {
  await testNotStarted();
  await testEcho();
  await testConcurrency();
  await testHandlerError();
  await testTimeout();
  await testStop();
  console.log('\n全部 Provider 桥接测试通过 🎉');
  process.exit(0);
})().catch((err) => {
  console.error('\n测试失败:', err);
  process.exit(1);
});
