// 執行：node --test tools/test-weather-ui.cjs（需 jsdom）。
// 使用真實設定頁及通知區段，API 為記憶體替身；不發送實際通知。
const { test } = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const { JSDOM } = require('jsdom');
const root = path.resolve(__dirname, '..');
const source = fs.readFileSync(path.join(root, 'web/settings.js'), 'utf8');

/** 建立互不干擾的設定頁，回傳操作介面與 API 呼叫紀錄。 */
async function createPage(t, overrides = {}) {
  const dom = new JSDOM(fs.readFileSync(path.join(root, 'web/settings.html'), 'utf8'), {
    runScripts: 'outside-only', url: 'http://esp32.test'
  });
  t.after(() => dom.window.close());
  const window = dom.window;
  const posts = [];
  const state = {
    enabled: false, mode: 0, intervalHours: 3, startHour: 0,
    dailyHour: 8, dailyMinute: 0, weeklyHour: 8, weeklyMinute: 0,
    weeklyMask: 31, configured: true, clockReady: true, busy: false,
    nextSend: 0, nextText: '', message: '待命', ...overrides
  };
  window.setTimeout = () => 0;
  window.fetchTimed = async (url, options = {}) => {
    if (options.method === 'POST') {
      posts.push({url, body: Object.fromEntries(options.body)});
      return {ok: true, text: async () => '已接受'};
    }
    return {ok: true, json: async () => state};
  };
  const labelStart = source.indexOf("let weatherLabelSavedMode");
  const labelEnd = source.indexOf("el('locationSearchForm').addEventListener");
  const scheduleStart = source.indexOf('let weatherNotifyInitialized');
  assert(labelStart >= 0 && labelEnd > labelStart && scheduleStart >= 0);
  window.eval(`
    var el = id => document.getElementById(id);
    var adminField = el('adminPassword');
    var adminToken = 'test-token';
    function authHeaders() { return {Authorization: 'Bearer test-token'}; }
    function noteForbidden() {}
  ` + source.slice(labelStart, labelEnd) + source.slice(scheduleStart));
  await new Promise(resolve => setImmediate(resolve));
  return {window, posts, element: id => window.document.getElementById(id)};
}

test('排程關閉仍可單獨發送真實天氣測試', async t => {
  const {element, posts} = await createPage(t);
  assert.equal(element('weatherEnabled').checked, false);
  assert.equal(element('weatherNotifyTest').disabled, false);
  assert.equal(element('weatherNotifyTest').textContent, '發送測試天氣通知');
  element('weatherNotifyTest').click();
  await new Promise(resolve => setImmediate(resolve));
  assert.equal(posts.length, 1);
  assert.equal(posts[0].url, '/api/notify/weather/test');
});

for (const [name, overrides] of [
  ['沒有啟用的 ntfy 目標', {configured: false}],
  ['尚未完成校時', {clockReady: false}],
  ['背景通知正在發送', {busy: true}]
]) {
  test(`${name}時停用測試並提供原因`, async t => {
    const {element, posts} = await createPage(t, overrides);
    assert.equal(element('weatherNotifyTest').disabled, true);
    assert.notEqual(element('weatherNotifyTest').title, '');
    element('weatherNotifyTest').click();
    assert.equal(posts.length, 0);
  });
}

test('四種地名選項只提交顯示偏好，不修改座標或自動發通知', async t => {
  const {window, element, posts} = await createPage(t);
  assert.equal(element('weatherNameMode').options.length, 4);
  for (const mode of ['0', '1', '2', '3']) {
    element('weatherNameMode').value = mode;
    element('weatherLabelForm').dispatchEvent(new window.Event('submit', {cancelable: true}));
    await new Promise(resolve => setImmediate(resolve));
    assert.equal(posts.at(-1).url, '/api/weather-label');
    assert.equal(posts.at(-1).body.mode, mode);
    assert.equal(posts.at(-1).body.latitude, undefined);
  }
  assert.equal(posts.length, 4);
});


test('地名儲存成功顯示綠色，改選後立即顯示尚未儲存', async t => {
  const {window, element} = await createPage(t);
  element('weatherNameMode').value = '1';
  element('weatherLabelForm').dispatchEvent(new window.Event('submit', {cancelable: true}));
  await new Promise(resolve => setImmediate(resolve));
  assert(element('weatherLabelResult').classList.contains('ok'));
  assert.match(element('weatherLabelResult').textContent, /已儲存並套用/);
  element('weatherNameMode').value = '2';
  element('weatherNameMode').dispatchEvent(new window.Event('change'));
  assert(element('weatherLabelResult').classList.contains('warning'));
  assert.match(element('weatherLabelResult').textContent, /尚未儲存/);
});

test('地名儲存失敗呈紅色，不能顯示已生效', async t => {
  const {window, element} = await createPage(t);
  window.fetchTimed = async () => ({ok: false, text: async () => '儲存區無法使用'});
  element('weatherLabelForm').dispatchEvent(new window.Event('submit', {cancelable: true}));
  await new Promise(resolve => setImmediate(resolve));
  assert(element('weatherLabelResult').classList.contains('error'));
  assert.match(element('weatherLabelResult').textContent, /儲存失敗/);
  assert.equal(element('weatherNameMode').disabled, false);
});

test('地名儲存回應不明呈黃色，不誤判成功或失敗', async t => {
  const {window, element} = await createPage(t);
  window.fetchTimed = async () => { throw Error('timeout'); };
  element('weatherLabelForm').dispatchEvent(new window.Event('submit', {cancelable: true}));
  await new Promise(resolve => setImmediate(resolve));
  assert(element('weatherLabelResult').classList.contains('warning'));
  assert.match(element('weatherLabelResult').textContent, /無法確認是否生效/);
});
