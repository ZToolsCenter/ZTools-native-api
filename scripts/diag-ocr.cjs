// 诊断脚本：在本机直接加载 dist 里安装的微信 OCR addon + 新 preload 的 ocrRecognize，
// 对真实图片跑一次识别，打印返回的 blocks 结构（验证坐标字段是否到位）。
'use strict'
const fs = require('node:fs')
const path = require('node:path')

const ROOT = path.resolve(__dirname, '..')
const PLUGIN = path.resolve(ROOT, '..', 'ZTools-collection', 'f-provider', 'dist')
const src = fs.readFileSync(path.join(PLUGIN, 'preload', 'services.js'), 'utf8')

const sandboxWindow = { __registered: {} }
sandboxWindow.ztools = {
  registerProvider: (k, h) => { sandboxWindow.__registered[k] = h },
}
new Function('sandboxWindow', 'require', `
  const window = sandboxWindow
  const ztools = window.ztools
  ${src}
`)(sandboxWindow, require)

const services = sandboxWindow.services
// 诊断环境无宿主 userData：直接把 native 目录指向插件 dist 内置 addon（与宿主
// 下载到 userData 后的布局同构），绕过 getPath 解析。
services._nativeDir = () => path.join(PLUGIN, 'native')
services.getTranslateSettings = () => ({ appID: 'x', appKey: 'y', appSecret: 'z', model: '', systemPrompt: '' })

async function main() {
  const image = path.join(ROOT, 'test', 'test-ocr.png')
  const ocr = await services.ocrRecognize(image)
  console.log('text =', JSON.stringify(ocr.text))
  console.log('blocks =', JSON.stringify(ocr.blocks, null, 2))
  console.log('confidence =', ocr.confidence)
  services.ocrDispose()
}

main().catch((e) => { console.error('FAILED:', e); process.exit(1) })
