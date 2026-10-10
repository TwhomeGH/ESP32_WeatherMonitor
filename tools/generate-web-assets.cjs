// 編輯 USER_GUIDE.md 或 web/ 後，在上傳前執行 node tools/generate-web-assets.cjs。
// 或直接用 node tools/flash.cjs 一次完成「產生 assets → 編譯 → 上傳」。
//
// 文件（USER_GUIDE.md / FUTURES_PLAN.md）在產生時就用 marked 轉成 HTML（含 alert 區塊），
// 裝置端不再需要 marked / DOMPurify。所有資產以 gzip 壓縮後嵌入 docs_assets.h，
// 路由以 Content-Encoding: gzip 傳送（瀏覽器會自動解壓）。
const fs = require('node:fs');
const path = require('node:path');
const zlib = require('node:zlib');

const root = path.resolve(__dirname, '..');
const marked = require(path.join(root, 'web', 'marked.umd.js'));

const ALERT_LABELS = { TIP: '提示', NOTE: '注意', IMPORTANT: '重要', WARNING: '警告', CAUTION: '小心' };

// 同時接受 GitHub 的 > [!TIP] 及獨立一行的 ![TIP] 簡寫。
function normalizeAlerts(markdown) {
  const lines = markdown.replace(/\r\n/g, '\n').split('\n');
  let fence = '', fenceLength = 0, shorthand = false;
  return lines.map(line => {
    const match = line.match(/^\s{0,3}(`{3,}|~{3,})/);
    if (match) {
      if (!fence) { fence = match[1][0]; fenceLength = match[1].length; }
      else if (match[1][0] === fence && match[1].length >= fenceLength) fence = '';
      shorthand = false;
      return line;
    }
    if (fence) return line;
    const short = line.match(/^!\[(TIP|NOTE|IMPORTANT|WARNING|CAUTION)\]\s*$/i);
    if (short) { shorthand = true; return '> [!' + short[1].toUpperCase() + ']'; }
    if (!line.trim()) shorthand = false;
    if (shorthand) return '> ' + line;
    return line.replace(/^(\s*>\s*)!\[(TIP|NOTE|IMPORTANT|WARNING|CAUTION)\]/i, '$1[!$2]');
  }).join('\n');
}

function renderDoc(markdown) {
  const html = marked.parse(normalizeAlerts(markdown));
  return html.replace(/<blockquote>\s*<p>\[!(TIP|NOTE|IMPORTANT|WARNING|CAUTION)\]\s*/g,
    (match, kind) => '<blockquote class="alert-' + kind.toLowerCase() +
      '"><div class="alert-title">' + ALERT_LABELS[kind] + '</div><p>');
}

const guide = fs.readFileSync(path.join(root, 'USER_GUIDE.md'), 'utf8');
const plan = fs.readFileSync(path.join(root, 'FUTURES_PLAN.md'), 'utf8');

const assets = {
  DOC_MARKDOWN: guide, PLAN_MARKDOWN: plan,
  DOC_RENDERED: renderDoc(guide), PLAN_RENDERED: renderDoc(plan),
  DOC_PAGE: fs.readFileSync(path.join(root, 'web', 'docs.html'), 'utf8'),
  DOC_SCRIPT: fs.readFileSync(path.join(root, 'web', 'docs.js'), 'utf8'),
  WEATHER_SCRIPT: fs.readFileSync(path.join(root, 'web', 'weather.js'), 'utf8'),
  SETTINGS_PAGE: fs.readFileSync(path.join(root, 'web', 'settings.html'), 'utf8'),
  SETTINGS_SCRIPT: fs.readFileSync(path.join(root, 'web', 'settings.js'), 'utf8')
};

let output = '#pragma once\n#include <Arduino.h>\n' +
  '// 自動產生，請修改來源檔並執行 tools/generate-web-assets.cjs。\n' +
  '// 內容為 gzip 壓縮後的 byte 陣列，路由需以 Content-Encoding: gzip 與長度傳送。\n';
let total = 0;
for (const [name, content] of Object.entries(assets)) {
  const gz = zlib.gzipSync(Buffer.from(content, 'utf8'), { level: 9 });
  total += gz.length;
  output += `const unsigned char ${name}[] PROGMEM = {` + Array.from(gz).join(',') + `};\n`;
  output += `const size_t ${name}_LEN = ${gz.length};\n`;
}
fs.writeFileSync(path.join(root, 'docs_assets.h'), output);
console.log(`Generated docs_assets.h (${Object.keys(assets).length} assets, ${total} bytes gzipped)`);

module.exports = { normalizeAlerts, renderDoc };
