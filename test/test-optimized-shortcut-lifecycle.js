const assert = require('assert');
const os = require('os');

if (os.platform() !== 'win32') {
  console.log('optimized shortcut lifecycle test skipped: Windows only');
  process.exit(0);
}

const addon = require('../build/Release/ztools_native.node');
const iterations = Number.parseInt(process.env.ZTOOLS_SHORTCUT_TEST_ITERATIONS || '500', 10);

const startedAt = Date.now();
for (let index = 0; index < iterations; index += 1) {
  addon.ensureOptimizedShortcutListener(() => {});
  addon.stopOptimizedShortcutListener();
  assert.strictEqual(addon.getOptimizedShortcutCount(), 0, `state leaked at iteration ${index}`);
}

addon.ensureOptimizedShortcutListener(() => {});
try {
  const registered = addon.registerOptimizedShortcut('Ctrl+Alt+Shift+F12');
  assert.strictEqual(registered.success, true, registered.error);
  assert.strictEqual(addon.getOptimizedShortcutCount(), 1);

  const removed = addon.unregisterOptimizedShortcut('Ctrl+Alt+Shift+F12');
  assert.strictEqual(removed.success, true, removed.error);
  assert.strictEqual(addon.getOptimizedShortcutCount(), 0);
} finally {
  addon.stopOptimizedShortcutListener();
}

console.log(`optimized shortcut lifecycle test passed: ${iterations} iterations in ${Date.now() - startedAt}ms`);
