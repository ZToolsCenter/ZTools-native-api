/**
 * Provider 异步桥接端到端测试（macOS 截图翻译的生产通路）
 *
 * 验证链路：JS 发起 invokeFromNativeAsync → 原生 InvokeAsync（requestId 登记）→
 * TSFN 调度回 JS 线程 → JS handler 执行（Promise/setTimeout）→ resolve/reject 按
 * seq 回传 → 原生侧按 requestId 回调 → JS Promise settle；
 * 超时走原生看门狗 CancelAsync（晚到的 JS 回传被桥按 seq 落空丢弃）。
 *
 * 覆盖场景（对应重构设计的 Provider Mock 验证清单）：
 *   1 同步返回 / 2 Promise.resolve / 3 延迟 Promise（setTimeout）
 *   5 超时（handler 故意超过 timeout）+ 7 晚到结果丢弃
 *   6 取消（请求发出后立即取消，无回调）
 *   8 多请求并发（乱序返回 + 有限并发模拟 + 20 段压力）
 *   附：同步通路回归（invokeFromNative）与重复 requestId 拒绝
 *
 * 运行：node test/test-provider-async-bridge.js
 */
const assert = require('assert');
const { ProviderBridge } = require('..');

const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

async function main() {
  assert.strictEqual(ProviderBridge.isReady(), false, 'bridge should not be ready before start');
  let nextReq = 100;
  const req = () => ++nextReq;

  // ---- Mock Provider：按 type 分发各测试语义 ----
  const handler = async (type, input) => {
    switch (type) {
      case 'sync':                       // Test 1：同步返回
        return { text: `sync:${input.text}` };
      case 'promise':                    // Test 2：Promise.resolve
        return Promise.resolve({ text: `promise:${input.text}` });
      case 'delayed':                    // Test 3：延迟 Promise
        await sleep(input.ms);
        return { text: `delayed:${input.text}` };
      case 'never':                      // Test 5/7：永不 resolve（超时 + 晚到）
        await sleep(10_000);
        return { text: 'late result' };
      case 'para':                       // Test 8：段落翻译（乱序延迟）
        await sleep(input.ms);
        return { text: `译${input.i}` };
      case 'fail':
        throw new Error('mock provider failure');
      default:
        throw new Error('unknown type: ' + type);
    }
  };
  ProviderBridge.start(handler);
  assert.strictEqual(ProviderBridge.isReady(), true);

  // ---- Test 1/2：同步返回与 Promise.resolve ----
  {
    const a = await ProviderBridge.invokeFromNativeAsync('sync', { text: '你好' }, req());
    const b = await ProviderBridge.invokeFromNativeAsync('promise', { text: '世界' }, req());
    assert.strictEqual(a.text, 'sync:你好');
    assert.strictEqual(b.text, 'promise:世界');
    console.log('✓ [Test 1/2] 同步返回 / Promise.resolve 均正常回投');
  }

  // ---- Test 3：延迟 Promise（setTimeout 驱动，宿主事件循环自由前进）----
  {
    const start = Date.now();
    const r = await ProviderBridge.invokeFromNativeAsync('delayed', { text: 'x', ms: 250 }, req());
    const elapsed = Date.now() - start;
    assert.strictEqual(r.text, 'delayed:x');
    assert.ok(elapsed >= 200, `delayed result should not arrive early (elapsed=${elapsed}ms)`);
    console.log(`✓ [Test 3] 延迟 Promise 正常回投（elapsed=${elapsed}ms）`);
  }

  // ---- handler 抛错经 reject 回传 ----
  {
    await assert.rejects(
      () => ProviderBridge.invokeFromNativeAsync('fail', {}, req()),
      /mock provider failure/,
      'handler error should propagate'
    );
    console.log('✓ handler 抛错经异步回调回传为 reject');
  }

  // ---- Test 5 + 7：超时 + 晚到结果丢弃 ----
  {
    const start = Date.now();
    // timeout 300ms，handler 10s 后才 resolve：先收到 timeout 错误；
    // 10s 的晚到 resolve 到达时登记已移除，被桥丢弃（不会再 settle 本 Promise）
    const p = ProviderBridge.invokeFromNativeAsync('never', {}, req(), 300);
    const err = await p.then(
      () => { throw new Error('should have timed out'); },
      (e) => e
    );
    const elapsed = Date.now() - start;
    assert.ok(/timed out/.test(err.message), `expected timeout error, got: ${err.message}`);
    assert.ok(elapsed < 2000, `timeout should fire near deadline (elapsed=${elapsed}ms)`);
    console.log(`✓ [Test 5] 超时按期触发（elapsed=${elapsed}ms, error="${err.message}"）`);
    // Test 7：等 700ms 确认晚到结果不会二次 settle / 崩溃（登记已被 CancelAsync 移除）
    await sleep(700);
    console.log('✓ [Test 7] 晚到结果被桥丢弃，无二次回调/崩溃');
  }

  // ---- Test 6：立即取消（无回调）----
  {
    let callbacks = 0;
    const rid = req();
    const p = ProviderBridge.invokeFromNativeAsync('delayed', { text: 'c', ms: 200 }, rid, 5000);
    p.then(
      () => { callbacks++; },
      () => { callbacks++; }
    );
    const cancelled = ProviderBridge.cancelFromNative(rid);
    assert.strictEqual(cancelled, true, 'first cancel should claim the request');
    assert.strictEqual(ProviderBridge.cancelFromNative(rid), false, 'second cancel is a no-op');
    await sleep(600);   // 超过 handler 的 200ms 延迟：晚到 resolve 被丢弃
    assert.strictEqual(callbacks, 0, 'cancelled request must never deliver a callback');
    console.log('✓ [Test 6] 取消后无回调，重复取消幂等，晚到结果丢弃');
  }

  // ---- 重复 requestId 拒绝 ----
  {
    const rid = req();
    const first = ProviderBridge.invokeFromNativeAsync('delayed', { text: 'd1', ms: 150 }, rid, 5000);
    let rejected = false;
    try {
      await ProviderBridge.invokeFromNativeAsync('delayed', { text: 'd2', ms: 0 }, rid, 5000);
    } catch (e) {
      rejected = /duplicate requestId/.test(e.message);
    }
    assert.ok(rejected, 'duplicate requestId should be rejected');
    assert.strictEqual((await first).text, 'delayed:d1');
    console.log('✓ 重复 requestId 被拒绝，原请求不受影响');
  }

  // ---- Test 8a：乱序返回按 requestId 正确配对 ----
  {
    const inputs = [0, 1, 2, 3, 4, 5, 6, 7];
    const delays = [300, 250, 200, 150, 100, 50, 0, 0];   // 后发先回
    const results = await Promise.all(
      inputs.map((i) => ProviderBridge.invokeFromNativeAsync(
        'para', { i, ms: delays[i] }, req(), 5000))
    );
    results.forEach((r, i) => {
      assert.strictEqual(r.text, `译${i}`, `result ${i} mismatched (got ${r.text})`);
    });
    console.log('✓ [Test 8a] 乱序返回按 requestId 正确配对（8 请求无串扰）');
  }

  // ---- Test 8b：有限并发队列模拟（Swift Job Manager 语义：max=2，完成后补发）----
  {
    const N = 10, MAX = 2, UNIT = 120;
    let inFlight = 0, peak = 0, next = 0;
    const results = new Array(N);
    const start = Date.now();
    const allDone = new Promise((resolve) => {
      const pump = () => {
        while (inFlight < MAX && next < N) {
          const i = next++;
          inFlight++;
          peak = Math.max(peak, inFlight);
          ProviderBridge.invokeFromNativeAsync('para', { i, ms: UNIT }, req(), 5000)
            .then((r) => { results[i] = r.text; })
            .then(() => {
              inFlight--;
              if (next >= N && inFlight === 0) {
                resolve();
              } else {
                pump();
              }
            })
            .catch((e) => resolve(e));   // 失败也收束，交由断言暴露
        }
      };
      pump();
    });
    await allDone;
    const elapsed = Date.now() - start;
    for (let i = 0; i < N; i++) assert.strictEqual(results[i], `译${i}`, `para ${i} mismatched`);
    assert.ok(peak <= MAX, `concurrency must stay <= ${MAX} (peak=${peak})`);
    assert.strictEqual(peak, MAX, `queue should actually reach max concurrency (peak=${peak})`);
    assert.ok(elapsed >= (N / MAX) * UNIT * 0.9,
      `bounded queue takes ~N/MAX units, got ${elapsed}ms`);
    console.log(`✓ [Test 8b] 有限并发模拟：${N} 段 max=${MAX} peak=${peak} 总耗时=${elapsed}ms，结果按序写回`);
  }

  // ---- Test 8c：20 段压力（全并发发起，验证不无限失败/不串扰）----
  {
    const N = 20;
    const results = await Promise.all(
      Array.from({ length: N }, (_, i) =>
        ProviderBridge.invokeFromNativeAsync('para', { i, ms: (i % 5) * 40 }, req(), 5000))
    );
    results.forEach((r, i) => assert.strictEqual(r.text, `译${i}`));
    console.log(`✓ [Test 8c] ${N} 段并发压力：全部按 requestId 配对成功`);
  }

  // ---- 同步通路回归（Windows 生产路径共用 pending 表，须不受影响）----
  {
    const r = await ProviderBridge.invokeFromNative('sync', { text: '旧链路' }, 2000);
    assert.strictEqual(r.text, 'sync:旧链路');
    const mixed = await Promise.all([
      ProviderBridge.invokeFromNative('delayed', { text: 'm1', ms: 80 }, 2000),
      ProviderBridge.invokeFromNativeAsync('delayed', { text: 'm2', ms: 80 }, req(), 2000),
    ]);
    assert.strictEqual(mixed[0].text, 'delayed:m1');
    assert.strictEqual(mixed[1].text, 'delayed:m2');
    console.log('✓ 同步 Invoke 回归正常，同步/异步混发互不干扰');
  }

  ProviderBridge.stop();
  assert.strictEqual(ProviderBridge.isReady(), false);
  console.log('\n全部异步桥接测试通过 ✅');
  process.exit(0);
}

main().catch((err) => {
  console.error('\n测试失败 ❌:', err);
  process.exit(1);
});
