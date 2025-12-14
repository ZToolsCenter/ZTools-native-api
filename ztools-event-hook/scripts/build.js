#!/usr/bin/env node
const { execSync } = require('child_process');
const os = require('os');

const platform = os.platform();

console.log(`🔨 Building for ${platform}...\n`);

try {
  // 编译 C++ 原生模块
  console.log('📦 Running node-gyp rebuild...');
  execSync('node-gyp rebuild', { stdio: 'inherit' });

  console.log('\n✅ Build successful!');
} catch (error) {
  console.error('\n❌ Build failed:', error.message);
  process.exit(1);
}
