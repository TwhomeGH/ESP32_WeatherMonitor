#!/usr/bin/env node
'use strict';
// 一鍵建置／上傳：先產生 docs_assets.h，再用 arduino-cli 編譯並上傳。
//
// 用法：
//   node tools/flash.cjs                 產生 assets、編譯並上傳（自動偵測序列埠，FQBN 取 sketch.yaml）
//   node tools/flash.cjs --compile-only  只編譯，不上傳
//   node tools/flash.cjs --port COM5     指定序列埠
//   node tools/flash.cjs --fqbn esp32:esp32:esp32   指定開發板
//   node tools/flash.cjs -- --build-property=...    其後參數原樣傳給 arduino-cli
//
// 會自動使用 Arduino IDE 內建的 arduino-cli（不需另外安裝）；
// 也可用環境變數 ARDUINO_CLI 指定路徑。

const { spawnSync } = require('node:child_process');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');

const root = path.resolve(__dirname, '..');

function info(msg) { console.log('[flash] ' + msg); }
function fail(msg) { console.error('\n[flash] ' + msg); process.exit(1); }

function printHelp() {
  console.log([
    'node tools/flash.cjs [options] [-- <arduino-cli 參數>]',
    '',
    '  --compile-only, --no-upload  只編譯，不上傳',
    '  --port, -p <port>            指定序列埠（預設自動偵測）',
    '  --fqbn <fqbn>                指定開發板（預設讀 sketch.yaml 的 default_fqbn）',
    '  --help, -h                   顯示本說明',
  ].join('\n'));
}

// ---- 1. 產生 web assets ----
info('產生 web assets…');
require(path.join(__dirname, 'generate-web-assets.cjs'));

// ---- 2. 找 arduino-cli ----
function resolveCli() {
  const envCli = process.env.ARDUINO_CLI;
  if (envCli && fs.existsSync(envCli)) return { path: envCli, bundled: false };
  const probe = spawnSync('arduino-cli', ['version'], { stdio: 'ignore' });
  if (!probe.error) return { path: 'arduino-cli', bundled: false };
  const bundledPath = {
    win32: 'C:/Program Files/Arduino IDE/resources/app/lib/backend/resources/arduino-cli.exe',
    darwin: '/Applications/Arduino IDE.app/Contents/Resources/app/lib/backend/resources/arduino-cli',
  }[process.platform];
  for (const c of [bundledPath, '/usr/local/bin/arduino-cli', '/usr/bin/arduino-cli']) {
    if (c && fs.existsSync(c)) return { path: c, bundled: true };
  }
  return null;
}

const resolved = resolveCli();
if (!resolved) fail('找不到 arduino-cli。請安裝（https://arduino.github.io/arduino-cli/installation/），或以環境變數 ARDUINO_CLI 指定路徑。');
const cli = resolved.path;
info('使用 arduino-cli：' + cli);

// 使用 IDE 內建 cli 時，沿用 IDE 的設定（資料／函式庫目錄）。
const configArgs = (resolved.bundled && fs.existsSync(path.join(os.homedir(), '.arduinoIDE', 'arduino-cli.yaml')))
  ? ['--config-file', path.join(os.homedir(), '.arduinoIDE', 'arduino-cli.yaml')] : [];

// ---- 3. 解析參數 ----
const opts = { port: null, fqbn: null, upload: true, passthrough: [] };
const argv = process.argv.slice(2);
for (let i = 0; i < argv.length; i++) {
  const a = argv[i];
  if (a === '--') { opts.passthrough = argv.slice(i + 1); break; }
  else if (a === '--compile-only' || a === '--no-upload') opts.upload = false;
  else if (a === '--port' || a === '-p') opts.port = argv[++i];
  else if (a.startsWith('--port=')) opts.port = a.slice('--port='.length);
  else if (a === '--fqbn') opts.fqbn = argv[++i];
  else if (a.startsWith('--fqbn=')) opts.fqbn = a.slice('--fqbn='.length);
  else if (a === '--help' || a === '-h') { printHelp(); process.exit(0); }
  else fail('未知參數：' + a + '（--help 看用法）');
}

// ---- 4. 讀 sketch.yaml 預設值 ----
function sketchDefaults() {
  const p = path.join(root, 'sketch.yaml');
  const out = {};
  if (!fs.existsSync(p)) return out;
  for (const line of fs.readFileSync(p, 'utf8').split(/\r?\n/)) {
    const m = /^\s*(default_fqbn|default_port)\s*:\s*([^#]+?)\s*$/.exec(line);
    if (m) out[m[1]] = m[2].replace(/^["']|["']$/g, '');
  }
  return out;
}
const defaults = sketchDefaults();
const fqbn = opts.fqbn || defaults.default_fqbn;
if (!fqbn) fail('未指定開發板。請在 sketch.yaml 設定 default_fqbn，或加上 --fqbn <FQBN>。');

// ---- 5. 決定序列埠 ----
function detectPort() {
  const res = spawnSync(cli, [...configArgs, 'board', 'list', '--format', 'json'], { encoding: 'utf8' });
  if (res.status !== 0 || !res.stdout) return null;
  let data;
  try { data = JSON.parse(res.stdout); } catch { return null; }
  const entries = data.detected_ports || [];
  const matched = entries.filter(e => (e.matching_boards || []).length);
  if (matched.length === 1) return matched[0].port.address;
  const usb = entries.filter(e => /USB/i.test(e.port.protocol_label || ''));
  if (usb.length === 1) return usb[0].port.address;
  if (entries.length === 1) return entries[0].port.address;
  return null;
}

let port = opts.port || defaults.default_port;
if (!port && opts.upload) {
  port = detectPort();
  if (!port) fail('無法自動判斷序列埠（可能有多個裝置）。請用 --port <COM埠> 指定，或先關閉其他序列埠程式。');
}
if (opts.upload) info('開發板：' + fqbn + '，序列埠：' + port);
else info('開發板：' + fqbn + '（僅編譯，不上傳）');

// ---- 6. 編譯／上傳 ----
// 建置目錄放在使用者家目錄下的 ASCII 路徑：ESP32 工具鏈對非 ASCII 路徑（例如專案中的
// 中文資料夾）在連結階段會失敗，故不放專案內。編譯後再把 compile_commands.json 複製到
// 專案 build/，供 VS Code IntelliSense 使用（build/ 已列於 .gitignore）。
const buildPath = path.join(os.homedir(), '.arduino-build', path.basename(root));
fs.mkdirSync(buildPath, { recursive: true });
const args = [...configArgs, 'compile', '--fqbn', fqbn, '--build-path', buildPath];
if (opts.upload) args.push('-p', port, '--upload');
args.push(...opts.passthrough, root);

info((opts.upload ? '編譯並上傳' : '編譯') + '中…');
const run = spawnSync(cli, args, { stdio: 'inherit' });
if (run.error) fail('無法執行 arduino-cli：' + run.error.message);
// 匯出 IntelliSense 用的編譯命令（讓 VS Code 找到 WebServer.h 等標頭）。
try {
  const commands = path.join(buildPath, 'compile_commands.json');
  if (fs.existsSync(commands)) {
    fs.mkdirSync(path.join(root, 'build'), { recursive: true });
    fs.copyFileSync(commands, path.join(root, 'build', 'compile_commands.json'));
    info('已更新 build/compile_commands.json（VS Code IntelliSense 用）。');
  }
} catch (e) { info('略過 compile_commands.json 匯出：' + e.message); }
process.exit(run.status === null ? 1 : run.status);
