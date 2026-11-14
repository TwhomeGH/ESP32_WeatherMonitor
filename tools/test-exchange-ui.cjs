// node --test tools/test-exchange-ui.cjs；需 jsdom。所有 API 都是記憶體替身，不發送通知。
const {test} = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const {JSDOM} = require('jsdom');
const root = path.resolve(__dirname, '..');
const tick = () => new Promise(resolve => setImmediate(resolve));

/** 每個案例建立獨立設定頁與伺服器狀態，載入真正的匯率 UI 程式。 */
async function page(t, overrides = {}) {
  const dom = new JSDOM(fs.readFileSync(path.join(root, 'web/settings.html'), 'utf8'), {
    runScripts: 'outside-only', url: 'http://esp32.test'
  });
  t.after(() => dom.window.close());
  const window = dom.window;
  const api = {
    calls: [], reject: false, failRead: false,
    state: {
      currency: 'USD', enabled: false, armed: true, threshold: 0, margin: 0.5,
      times: '09:30', targets: [1], currencies: ['USD', 'EUR', 'JPY'], price: 32, priceText: '32',
      sourceTime: 1800000000, fetchedAt: 1800000100, nextUpdate: 1800086400,
      fresh: true, clockReady: true, targetsReady: true, busy: false, message: '待命', ...overrides
    }
  };
  const polls = [];
  window.setTimeout = callback => polls.push(callback);
  window.fetchTimed = async (url, options = {}) => {
    if (options.method === 'POST') {
      const body = Object.fromEntries(options.body);
      api.calls.push({url, body});
      if (api.reject) return {ok: false, status: 500, text: async () => '儲存失敗'};
      if (url === '/api/exchange') {
        const changed = body.currency !== api.state.currency;
        Object.assign(api.state, {
          currency: body.currency, enabled: !changed && body.enabled === 'true',
          threshold: changed ? 0 : Number(body.threshold), times: body.times,
          targets: body.targets ? body.targets.split(',').map(Number) : [],
          margin: Number(body.margin)
        });
      }
      return {ok: true, text: async () => '設定已接受'};
    }
    if (api.failRead) throw Error('offline');
    return {ok: true, json: async () => url === '/api/ntfy'
      ? {targets: [{id: 1, name: '爸爸', enabled: true}, {id: 2, name: '停用裝置', enabled: false}]}
      : structuredClone(api.state)};
  };
  window.eval(`
    var adminField = document.getElementById('adminPassword');
    var adminToken = 'test';
    function authHeaders() { return {Authorization: 'Bearer test'}; }
    function noteForbidden() {}
  `);
  window.eval(fs.readFileSync(path.join(root, 'web/exchange.js'), 'utf8'));
  await tick();
  const field = id => window.document.getElementById(id);
  const submit = async () => {
    field('fxForm').dispatchEvent(new window.Event('submit', {cancelable: true}));
    await tick();
  };
  return {window, api, field, submit, poll: () => polls.at(-1)()};
}

test('清楚顯示每美元需要的台幣，預設提醒關閉', async t => {
  const {field, api} = await page(t);
  assert.equal(field('fxCurrency').value, 'USD');
  assert.equal(field('fxEnabled').checked, false);
  assert.equal(field('fxQuote').textContent, '1 USD = 32 TWD');
  assert.equal(api.calls.length, 0);
});

test('直接顯示裝置格式化後的報價，低價幣別不被壓成 2 位', async t => {
  const {field} = await page(t, {currency: 'JPY', price: 0.2130123, priceText: '0.2130'});
  assert.equal(field('fxQuote').textContent, '1 JPY = 0.2130 TWD');
});

test('沒有報價時不顯示任何數值', async t => {
  const {field} = await page(t, {price: null, priceText: null});
  assert.equal(field('fxQuote').textContent, '尚未取得 USD 報價');
});

test('更換幣別清空門檻並停用提醒，必須先儲存', async t => {
  const {window, field, api, submit} = await page(t, {threshold: 32, enabled: true});
  field('fxCurrency').value = 'JPY';
  field('fxCurrency').dispatchEvent(new window.Event('change'));
  assert.equal(field('fxThreshold').value, '');
  assert.equal(field('fxEnabled').checked, false);
  assert.equal(field('fxEnabled').disabled, true);
  assert.equal(field('fxTest').disabled, true);
  await submit();
  assert.equal(api.calls[0].body.currency, 'JPY');
  assert.equal(api.calls[0].body.threshold, '0');
  assert.equal(api.calls[0].body.enabled, 'false');
});

test('儲存只提交所選啟用目標，排除停用裝置', async t => {
  const {api, submit} = await page(t, {targets: [1, 2]});
  await submit();
  assert.equal(api.calls[0].body.targets, '1');
  assert.equal(api.calls[0].url, '/api/exchange');
});

test('立即更新與測試使用不同端點，更新不發送測試', async t => {
  const {field, api} = await page(t);
  field('fxRefresh').click();
  await tick();
  assert.equal(api.calls.length, 1);
  assert.equal(api.calls[0].url, '/api/exchange/refresh');
});

test('提醒關閉仍允許手動測試已儲存目標', async t => {
  const {field, api} = await page(t);
  assert.equal(field('fxTest').disabled, false);
  field('fxTest').click();
  await tick();
  assert.equal(api.calls[0].url, '/api/exchange/test');
});

for (const [name, overrides] of [
  ['資料過期', {fresh: false}], ['尚未校時', {clockReady: false}],
  ['目標刪除或停用', {targetsReady: false}], ['背景工作忙碌', {busy: true}]
]) {
  test(`${name}時禁止測試`, async t => {
    const {field, api} = await page(t, overrides);
    assert.equal(field('fxTest').disabled, true);
    field('fxTest').click();
    assert.equal(api.calls.length, 0);
  });
}

test('儲存失敗保留未儲存內容與停用目標限制', async t => {
  const {window, field, api, submit} = await page(t);
  field('fxThreshold').value = '31';
  field('fxThreshold').dispatchEvent(new window.Event('input'));
  api.reject = true;
  await submit();
  assert.match(field('fxResult').textContent, /儲存失敗/);
  assert.equal(field('fxThreshold').value, '31');
  assert.equal(field('fxTargets').querySelector('input[value="2"]').disabled, true);
  assert.equal(field('fxTest').disabled, true);
});


test('短暫斷線後重新取得狀態，不覆蓋尚未儲存的門檻', async t => {
  const {window, field, api, poll} = await page(t);
  field('fxThreshold').value = '30.5';
  field('fxThreshold').dispatchEvent(new window.Event('input'));
  api.failRead = true;
  await poll();
  assert.equal(field('fxSave').disabled, true);
  api.failRead = false;
  await poll();
  assert.equal(field('fxThreshold').value, '30.5');
  assert.equal(field('fxTest').disabled, true);
});


test('匯率卡片位於主版面內，避免逸出容器與腳本先於元素載入', async t => {
  const {window} = await page(t);
  const document = window.document;
  const card = document.getElementById('exchange');
  assert(document.querySelector('main .grid').contains(card));
  const script = document.querySelector('script[src="/assets/exchange.js"]');
  assert(card.compareDocumentPosition(script) & window.Node.DOCUMENT_POSITION_FOLLOWING);
});
