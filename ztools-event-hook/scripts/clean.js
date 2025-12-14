#!/usr/bin/env node
const fs = require('fs');
const path = require('path');

const dirsToClean = [
  'build',
  'lib'
];

console.log('🧹 清理构建文件...\n');

dirsToClean.forEach(dir => {
  const dirPath = path.join(__dirname, '..', dir);
  if (fs.existsSync(dirPath)) {
    console.log(`删除目录: ${dir}`);
    fs.rmSync(dirPath, { recursive: true, force: true });
  }
});

console.log('\n✅ 清理完成！');

