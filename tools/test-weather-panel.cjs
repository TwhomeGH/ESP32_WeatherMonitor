// node --test tools/test-weather-panel.cjs；需 jsdom。驗證首頁天氣卡套用地名顯示策略。
const {test} = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const {JSDOM} = require('jsdom');
const root = path.resolve(__dirname, '..');

/** 建立首頁天氣卡所需的最小 DOM，天氣查詢以未完成的 Promise 代替。 */
function panel(t, location) {
  const dom = new JSDOM(`<!doctype html><body>
    <h2 id="weatherTitle">網路天氣</h2>
    <p id="weatherPlace" hidden></p>
    <div id="weatherSummary"></div>
    <p id="weatherStatus"></p>
    <span id="temperature"></span><span id="humidity"></span>
    <span id="windSpeed"></span><span id="windDirection"></span><span id="weatherTime"></span>
  </body>`, {runScripts: 'outside-only', url: 'http://esp32.test'});
  t.after(() => dom.window.close());
  const window = dom.window;
  window.setTimeout = () => 0;
  window.el = id => window.document.getElementById(id);
  // 地區查詢回傳指定內容；Open-Meteo 查詢保持未完成，避免測試依賴外網。
  window.fetch = url => String(url).includes('/api/weather-location')
    ? Promise.resolve({ok: true, json: async () => location})
    : new Promise(() => {});
  window.eval(fs.readFileSync(path.join(root, 'web/weather.js'), 'utf8'));
  return {window, el: window.el};
}

const tick = () => new Promise(resolve => setImmediate(resolve));
/** 等待非同步的地區查詢與 DOM 更新完成。 */
const settle = async () => { for (let i = 0; i < 5; i++) await tick(); };

test('略過地名時標題只留網路天氣，並標示完整查詢位置', async t => {
  const {el} = panel(t, {name: '新北市土城區', displayName: '', nameMode: 3, latitude: 24.967, longitude: 121.436});
  await settle();
  assert.equal(el('weatherTitle').textContent, '網路天氣');
  assert.equal(el('weatherPlace').textContent, '查詢位置：新北市土城區');
  assert.equal(el('weatherPlace').hidden, false);
});

test('只報縣市時套用縮短名稱並保留完整查詢位置', async t => {
  const {el} = panel(t, {name: '新北市土城區', displayName: '新北市', nameMode: 1, latitude: 24.967, longitude: 121.436});
  await settle();
  assert.equal(el('weatherTitle').textContent, '新北市 · 網路天氣');
  assert.equal(el('weatherPlace').textContent, '查詢位置：新北市土城區');
  assert.equal(el('weatherPlace').hidden, false);
});

test('完整地名時不重複顯示查詢位置', async t => {
  const {el} = panel(t, {name: '新北市土城區', displayName: '新北市土城區', nameMode: 0, latitude: 24.967, longitude: 121.436});
  await settle();
  assert.equal(el('weatherTitle').textContent, '新北市土城區 · 網路天氣');
  assert.equal(el('weatherPlace').hidden, true);
});
