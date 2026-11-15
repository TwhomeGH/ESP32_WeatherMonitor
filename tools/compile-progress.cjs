'use strict';
const {spawn} = require('node:child_process');
const fs = require('node:fs');
const {StringDecoder} = require('node:string_decoder');

/** 從 verbose 編譯輸出辨識階段；無法辨識時保留原階段，不猜測百分比。 */
function describe(line) {
  const source = line.match(/([^\\/\s"]+\.(?:cpp|c|S|ino\.cpp))(?=["\s]|$)/);
  if (source) return '處理 ' + source[1];
  if (/Detecting libraries|Alternatives for|ResolveLibrary/.test(line)) return '偵測函式庫';
  if (/Generating function prototypes/.test(line)) return '產生函式宣告';
  if (/Compiling sketch/.test(line)) return '編譯專案';
  if (/Compiling libraries/.test(line)) return '編譯函式庫';
  if (/Compiling core/.test(line)) return '編譯核心';
  if (/Linking everything|\.elf/.test(line)) return '連結韌體';
  if (/Sketch uses|Global variables use/.test(line)) return line;
  return null;
}

/**
 * 非同步執行編譯，保存完整記錄並定期顯示耗時與無輸出時間。
 * @param {string} command 編譯器執行檔。
 * @param {string[]} args 傳給編譯器的參數，不經 shell。
 * @param {object} options logPath、verbose、info；heartbeatMs 可供測試縮短間隔。
 * @returns {Promise<number>} 子程序結束碼；啟動失敗或信號終止回傳 1。
 */
function runCompile(command, args, {logPath, verbose = false, info = console.log, heartbeatMs = 10000}) {
  return new Promise(resolve => {
    const log = fs.createWriteStream(logPath);
    let logError = false;
    let exitResult = null;
    let interrupted = false;
    log.on('error', error => {
      logError = true;
      info('編譯記錄無法寫入：' + error.message);
      if (exitResult !== null) resolve(exitResult);
    });
    const started = Date.now();
    let lastOutput = started;
    let stage = '啟動編譯工具';
    const child = spawn(command, args, {stdio: ['inherit', 'pipe', 'pipe'], windowsHide: true});
    info('編譯程序 PID：' + (child.pid ?? '尚未啟動'));
    info('完整編譯記錄：' + logPath);

    const report = () => {
      const elapsed = Math.floor((Date.now() - started) / 1000);
      const quiet = Math.floor((Date.now() - lastOutput) / 1000);
      info('已耗時 ' + elapsed + ' 秒｜最後階段：' + stage + '｜' + quiet + ' 秒沒有新輸出' +
        (quiet >= 60 ? '；可能仍在運算或等待，僅憑無輸出無法判定卡住。可查看記錄，或按 Ctrl+C 中止。' : ''));
    };
    const timer = setInterval(report, heartbeatMs);
    for (const stream of [child.stdout, child.stderr]) {
      const decoder = new StringDecoder('utf8');
      let pending = '';
      const line = value => {
        const next = describe(value);
        if (next && next !== stage) { stage = next; if (!verbose) info(stage); }
        // 摘要模式仍保留錯誤、警告及容量結果。
        if (!verbose && /error:|warning:|Error |exit status|Sketch uses|Global variables use/i.test(value))
          process.stderr.write(value + '\n');
      };
      stream.on('data', chunk => {
        lastOutput = Date.now();
        if (!logError) log.write(chunk);
        if (verbose) (stream === child.stderr ? process.stderr : process.stdout).write(chunk);
        pending += decoder.write(chunk);
        const lines = pending.split(/\r\n|\n|\r/);
        pending = lines.pop();
        lines.forEach(line);
        if (pending.length > 65536) { line(pending); pending = ''; }
      });
      stream.on('end', () => { pending += decoder.end(); if (pending) line(pending); });
    }
    // 使用者 Ctrl+C 時只終止本工具啟動的編譯樹，不影響其他 IDE 工作。
    const interrupt = () => {
      interrupted = true;
      info('正在中止本次編譯…');
      if (child.pid && process.platform === 'win32') {
        const kill = spawn('taskkill', ['/PID', String(child.pid), '/T', '/F'], {stdio: 'ignore', windowsHide: true});
        kill.on('error', () => child.kill());
      } else child.kill('SIGINT');
    };
    process.once('SIGINT', interrupt);
    child.on('error', error => info('無法啟動編譯工具：' + error.message));
    child.on('close', (code, signal) => {
      clearInterval(timer);
      process.removeListener('SIGINT', interrupt);
      const result = interrupted ? 130 : signal || code === null ? 1 : code;
      exitResult = result;
      info('編譯' + (result === 0 ? '完成' : '失敗／中止') + '，耗時 ' +
        Math.floor((Date.now() - started) / 1000) + ' 秒，結束碼 ' + result);
      if (logError) resolve(result);
      else log.end(() => resolve(result));
    });
  });
}

module.exports = {runCompile, describe};
