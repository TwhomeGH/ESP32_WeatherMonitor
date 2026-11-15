'use strict';
const {test} = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const {runCompile, describe} = require('./compile-progress.cjs');

test('辨識編譯檔案與連結階段，不捏造進度百分比', () => {
  assert.equal(describe('g++ -c "C:/project/weather.cpp" -o weather.o'), '處理 weather.cpp');
  assert.equal(describe('Linking everything together...'), '連結韌體');
  assert.equal(describe('unknown output'), null);
});

test('安靜的子程序仍回報耗時，完整輸出存檔並傳回失敗碼', async () => {
  const logPath = path.resolve('build/test-compile-progress.log');
  fs.mkdirSync(path.dirname(logPath), {recursive: true});
  const messages = [];
  const result = await runCompile(process.execPath, ['-e',
    'console.log("Compiling sketch..."); setTimeout(() => {console.log("完成中文輸出"); process.exitCode=7;}, 200);'], {
    logPath, heartbeatMs: 30, info: message => messages.push(message)
  });
  assert.equal(result, 7);
  assert(messages.some(message => /已耗時.*沒有新輸出/.test(message)));
  assert(messages.some(message => /編譯專案/.test(message)));
  assert.match(fs.readFileSync(logPath, 'utf8'), /完成中文輸出/);
});

test('啟動失敗會結束並回報，不留下心跳計時器', async () => {
  const messages = [];
  const result = await runCompile(path.resolve('build/nonexistent-compiler.exe'), [], {
    logPath: path.resolve('build/test-compile-start-error.log'), info: message => messages.push(message)
  });
  assert.notEqual(result, 0);
  assert(messages.some(message => /無法啟動/.test(message)));
});
