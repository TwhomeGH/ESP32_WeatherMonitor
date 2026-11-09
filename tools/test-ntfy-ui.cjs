// 執行：node --test tools/test-ntfy-ui.cjs（需安裝 jsdom，或以 NODE_PATH 指定）。
// 範圍：真實設定頁 HTML + ntfy 前端邏輯；API 使用記憶體替身，不發送通知。
// 不涵蓋 ESP32 儲存、TLS、排程與手機實際收件。
const { test } = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const { JSDOM } = require('jsdom');

const root = path.resolve(__dirname, '..');
const html = fs.readFileSync(path.join(root, 'web/settings.html'), 'utf8');
const script = fs.readFileSync(path.join(root, 'web/settings.js'), 'utf8');
const start = script.indexOf('let ntfyInitialized');
const end = script.indexOf('let weatherNotifyInitialized');
assert(start >= 0 && end > start, '找不到 ntfy 程式區段，請更新測試載入方式');

function target(id = 1) {
  return { id, name: `目標 ${id}`, topic: `topic_${id}`, enabled: true, tokenConfigured: true };
}

/** 每個測試使用獨立 DOM 與 API 狀態，避免前一個案例影響下一個。 */
async function createPage(t, initialTargets = [target()]) {
  const dom = new JSDOM(html, { runScripts: 'outside-only', url: 'http://esp32.test' });
  t.after(() => dom.window.close());
  const window = dom.window;
  const element = id => window.document.getElementById(id);
  const api = { targets: structuredClone(initialTargets), posts: [], rejectWrites: false };
  let nextId = Math.max(0, ...initialTargets.map(item => item.id)) + 1;

  // 輪詢由測試手動控制；刪除確認預設按下「確定」。
  window.setTimeout = () => 0;
  window.confirm = () => true;
  window.fetchTimed = async (url, options = {}) => {
    if (options.method !== 'POST') {
      return {
        ok: true,
        json: async () => ({
          targets: structuredClone(api.targets), clockReady: true,
          busy: false, message: '待命', code: 0
        })
      };
    }

    const body = Object.fromEntries(options.body);
    api.posts.push({ url, ...body });
    if (api.rejectWrites) {
      return { ok: false, status: 403, text: async () => '管理密碼錯誤' };
    }
    if (url === '/api/ntfy' && body.action === 'delete') {
      api.targets = api.targets.filter(item => String(item.id) !== body.id);
    } else if (url === '/api/ntfy') {
      const previous = api.targets.find(item => String(item.id) === body.id);
      const saved = {
        id: previous?.id ?? nextId++, name: body.name, topic: body.topic,
        enabled: body.enabled === 'true',
        tokenConfigured: body.clearToken === 'true'
          ? false : Boolean(body.token || previous?.tokenConfigured)
      };
      api.targets = api.targets.filter(item => item.id !== saved.id);
      api.targets.push(saved);
    }
    return { ok: true, status: 200, text: async () => '成功' };
  };

  // 登入流程不在測試範圍內，只提供 ntfy 區段需要的共用介面。
  window.eval(`
    var el = id => document.getElementById(id);
    var adminField = el('adminPassword');
    var adminToken = 'test';
    function authHeaders() { return { Authorization: 'Bearer test' }; }
    function noteForbidden() {}
  `);
  window.eval(script.slice(start, end));
  await new Promise(resolve => setImmediate(resolve));

  function select(id) {
    element('ntfySelect').value = String(id);
    element('ntfySelect').dispatchEvent(new window.Event('change'));
  }
  function input(id, value) {
    element(id).value = value;
    element(id).dispatchEvent(new window.Event('input'));
  }
  return { window, element, api, select, input };
}

test('讀取設定後，自動選取第一組，允許測試已儲存目標', async t => {
  const { element } = await createPage(t);
  assert.equal(element('ntfySelect').value, '1');
  assert.equal(element('ntfyTest').disabled, false);
});

test('修改主題但尚未儲存時，禁止測試以免送往舊主題', async t => {
  const { element, input } = await createPage(t);
  input('ntfyTopic', 'unsaved_topic');
  assert.equal(element('ntfyTest').disabled, true);
});

test('切換至新增模式，清空未儲存的權杖且禁止刪除', async t => {
  const { element, select, input } = await createPage(t);
  input('ntfyToken', 'unsaved-token');
  select('');
  assert.equal(element('ntfyToken').value, '');
  assert.equal(element('ntfyDelete').disabled, true);
});

test('新增目標後，自動選取新目標並允許測試', async t => {
  const { window, element, api, select, input } = await createPage(t);
  select('');
  input('ntfyName', '媽媽的手機');
  input('ntfyTopic', 'mom');
  await window.ntfyAction('save');
  assert.equal(api.targets.length, 2);
  assert.equal(api.targets[1].name, '媽媽的手機');
  assert.equal(element('ntfySelect').value, '2');
  assert.equal(element('ntfyTest').disabled, false);
});

test('目標名稱中的 HTML 以文字顯示，不建立 HTML 元素', async t => {
  const maliciousName = '<img src=x onerror=alert(1)>';
  const { element } = await createPage(t, [{ ...target(), name: maliciousName }]);
  assert.equal(element('ntfySelect').querySelector('img'), null);
  assert.equal(element('ntfySelect').selectedOptions[0].textContent, maliciousName);
});

test('手動測試只提交所選目標的 ID', async t => {
  const { window, api, select } = await createPage(t, [target(1), target(2)]);
  select(2);
  await window.ntfyAction('test');
  assert.equal(api.posts.length, 1);
  assert.equal(api.posts[0].url, '/api/ntfy/test');
  assert.equal(api.posts[0].id, '2');
});

test('停用排程收件後，仍可手動測試該目標', async t => {
  const { window, element, api } = await createPage(t);
  element('ntfyEnabled').checked = false;
  await window.ntfyAction('save');
  assert.equal(api.targets[0].enabled, false);
  assert.equal(element('ntfyTest').disabled, false);
});

test('刪除被伺服器拒絕時，保留目標並顯示錯誤', async t => {
  const { window, element, api } = await createPage(t);
  api.rejectWrites = true;
  await window.ntfyAction('delete');
  assert.equal(api.targets.length, 1);
  assert.equal(element('ntfySelect').value, '1');
  assert.equal(element('ntfyResult').textContent, '管理密碼錯誤');
});

test('刪除成功後移除目標，停用測試按鈕', async t => {
  const { window, element, api } = await createPage(t);
  await window.ntfyAction('delete');
  assert.equal(api.targets.length, 0);
  assert.equal(element('ntfySelect').value, '');
  assert.equal(element('ntfyTest').disabled, true);
});

test('已有 8 組時禁止新增，但仍可編輯既有目標', async t => {
  const { element, select } = await createPage(t, Array.from({ length: 8 }, (_, i) => target(i + 1)));
  select('');
  assert.equal(element('ntfySave').disabled, true);
  select(1);
  assert.equal(element('ntfySave').disabled, false);
});
