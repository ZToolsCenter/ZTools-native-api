const { WindowManager } = require('..')

console.log('=== 鼠标模拟测试 ===\n')

// 检查平台
const platform = WindowManager.getPlatform()
console.log(`当前平台: ${platform}\n`)

// ========== 参数校验测试（立即执行） ==========

console.log('--- 参数校验测试 ---\n')

function testParamValidation(name, fn) {
  try {
    fn()
    console.log(`  ✗ ${name} — 预期抛出异常但没有`)
  } catch (e) {
    if (e instanceof TypeError) {
      console.log(`  ✓ ${name} — TypeError: ${e.message}`)
    } else {
      console.log(`  ✗ ${name} — 错误类型不对: ${e.constructor.name}: ${e.message}`)
    }
  }
}

// simulateMouseMove 参数校验
testParamValidation('simulateMouseMove() 无参数', () => WindowManager.simulateMouseMove())
testParamValidation('simulateMouseMove("a", 1) x 非数字', () => WindowManager.simulateMouseMove('a', 1))
testParamValidation('simulateMouseMove(1, "b") y 非数字', () => WindowManager.simulateMouseMove(1, 'b'))
testParamValidation('simulateMouseMove(null, 1) x 为 null', () => WindowManager.simulateMouseMove(null, 1))

// simulateMouseClick 参数校验
testParamValidation('simulateMouseClick() 无参数', () => WindowManager.simulateMouseClick())
testParamValidation('simulateMouseClick("a", 1) x 非数字', () => WindowManager.simulateMouseClick('a', 1))

// simulateMouseDoubleClick 参数校验
testParamValidation('simulateMouseDoubleClick() 无参数', () => WindowManager.simulateMouseDoubleClick())
testParamValidation('simulateMouseDoubleClick(1) 缺少 y', () => WindowManager.simulateMouseDoubleClick(1))

// simulateMouseRightClick 参数校验
testParamValidation('simulateMouseRightClick() 无参数', () => WindowManager.simulateMouseRightClick())
testParamValidation('simulateMouseRightClick(undefined, 1) x 为 undefined', () => WindowManager.simulateMouseRightClick(undefined, 1))

console.log('\n--- 参数校验测试完成 ---\n')

// ========== 功能测试（交互式，需要用户观察） ==========

console.log('⚠️  注意：以下测试会移动鼠标并模拟点击！')
console.log('请确保当前没有重要操作，避免意外点击。')
console.log('\n测试将在 3 秒后开始...\n')

setTimeout(() => {
  console.log('--- 功能测试 ---\n')

  // 测试1: 鼠标移动
  console.log('【测试 1】鼠标移动')
  console.log('  移动到 (100, 100)')
  let result = WindowManager.simulateMouseMove(100, 100)
  console.log(`  结果: ${result ? '✓ 成功' : '✗ 失败'}`)
  console.log('')

  // 测试2: 移动到其他位置
  setTimeout(() => {
    console.log('【测试 2】鼠标移动到 (500, 300)')
    const result = WindowManager.simulateMouseMove(500, 300)
    console.log(`  结果: ${result ? '✓ 成功' : '✗ 失败'}`)
    console.log('')
  }, 1000)

  // 测试3: 鼠标左键单击（在空白区域）
  setTimeout(() => {
    console.log('【测试 3】鼠标左键单击 (500, 500)')
    const result = WindowManager.simulateMouseClick(500, 500)
    console.log(`  结果: ${result ? '✓ 成功' : '✗ 失败'}`)
    console.log('')
  }, 2000)

  // 测试4: 鼠标双击
  setTimeout(() => {
    console.log('【测试 4】鼠标左键双击 (500, 500)')
    const result = WindowManager.simulateMouseDoubleClick(500, 500)
    console.log(`  结果: ${result ? '✓ 成功' : '✗ 失败'}`)
    console.log('')
  }, 3000)

  // 测试5: 鼠标右键单击
  setTimeout(() => {
    console.log('【测试 5】鼠标右键单击 (500, 500)')
    const result = WindowManager.simulateMouseRightClick(500, 500)
    console.log(`  结果: ${result ? '✓ 成功' : '✗ 失败'}`)
    console.log('')
  }, 4000)

  // 测试6: 边界值 — 原点
  setTimeout(() => {
    console.log('【测试 6】边界值 — 移动到 (0, 0)')
    const result = WindowManager.simulateMouseMove(0, 0)
    console.log(`  结果: ${result ? '✓ 成功' : '✗ 失败'}`)
    console.log('')
  }, 5000)

  // 测试7: 浮点数坐标
  setTimeout(() => {
    console.log('【测试 7】浮点数坐标 — 移动到 (300.5, 200.7)')
    const result = WindowManager.simulateMouseMove(300.5, 200.7)
    console.log(`  结果: ${result ? '✓ 成功' : '✗ 失败'}`)
    console.log('')
  }, 6000)

  // 测试8: 连续移动轨迹
  setTimeout(() => {
    console.log('【测试 8】连续移动轨迹 — 画一条对角线')
    let successCount = 0
    const steps = 20
    for (let i = 0; i <= steps; i++) {
      const x = 200 + (i * 15)
      const y = 200 + (i * 10)
      const r = WindowManager.simulateMouseMove(x, y)
      if (r) successCount++
    }
    console.log(`  结果: ${successCount}/${steps + 1} 步成功`)
    console.log('')
  }, 7000)

  // 总结
  setTimeout(() => {
    console.log('='.repeat(50))
    console.log('✅ 所有鼠标模拟测试完成')
    console.log('='.repeat(50))
    process.exit(0)
  }, 8500)
}, 3000)

// 处理 Ctrl+C
process.on('SIGINT', () => {
  console.log('\n\n用户中断测试')
  process.exit(0)
})
