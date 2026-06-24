#!/usr/bin/env node
const { execSync } = require('child_process');
const os = require('os');

const platform = os.platform();

console.log(`🔨 Building for ${platform}...\n`);

try {
  // Step 1: 把 SVG 资源生成成 C 头文件（截图工具栏图标）
  console.log('🎨 Generating icon header from SVG...');
  execSync('node scripts/gen-icons.js', { stdio: 'inherit' });

  // Step 2: 编译 C++ 原生模块 (跨平台)
  console.log('📦 Running node-gyp rebuild...');
  execSync('npx node-gyp rebuild', { stdio: 'inherit' });

  // Step 3: macOS 需要额外编译 Swift
  if (platform === 'darwin') {
    console.log('\n🍎 Building Swift library for macOS...');
    execSync('npm run build:swift', { stdio: 'inherit' });
  } else if (platform === 'win32') {
    console.log('\n🪟 Windows build complete (no Swift needed)');
  } else {
    console.warn(`\n⚠️  Platform ${platform} is not officially supported`);
  }

  console.log('\n✅ Build successful!');
} catch (error) {
  console.error('\n❌ Build failed:', error.message);
  process.exit(1);
}
