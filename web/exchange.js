/** 匯率設定控制器；共用設定頁登入，所有通知操作需明確按鈕或已儲存排程。 */
(() => {
  const field = id => document.getElementById(id);
  let loaded = false;
  let initialized = false;
  let saving = false;
  let state = null;
  let dirty = false;
  let currencyChanged = false;
  let generation = 0;
  const names = new Intl.DisplayNames(['zh-TW'], {type: 'currency'});

  function result(kind, message) {
    field('fxResult').className = 'status-box ' + kind;
    field('fxResult').textContent = message;
  }

  function buttons() {
    const locked = !loaded || saving || state?.busy;
    field('fxSave').disabled = locked;
    field('fxRefresh').disabled = locked || !state?.clockReady;
    field('fxTest').disabled = locked || dirty || !state?.clockReady || !state?.targetsReady || !state?.fresh;
  }

  function markDirty() {
    dirty = true;
    result('warning', '● 尚未儲存，裝置仍使用原有設定。');
    renderRule();
    buttons();
  }

  field('fxCurrency').addEventListener('change', () => {
    currencyChanged = field('fxCurrency').value !== state?.currency;
    field('fxThreshold').value = '';
    field('fxEnabled').checked = false;
    field('fxEnabled').disabled = currencyChanged;
    field('fxThreshold').disabled = currencyChanged;
    markDirty();
    if (currencyChanged) result('warning', '● 幣別已改選，請先儲存；原門檻不沿用，新幣別報價載入後再設定門檻。');
  });
  ['fxThreshold', 'fxMargin', 'fxTimes', 'fxEnabled'].forEach(id => {
    field(id).addEventListener('input', markDirty);
  });

  /** 安全建立選單與目標，不把來源或使用者名稱當 HTML 執行。 */
  function loadForm(data, targets) {
    const codes = [...new Set([data.currency, ...data.currencies])].sort();
    field('fxCurrency').replaceChildren(...codes.map(code => new Option(code + ' ' + names.of(code), code)));
    field('fxCurrency').value = data.currency;
    field('fxThreshold').value = data.threshold || '';
    field('fxMargin').value = data.margin;
    field('fxTimes').value = data.times;
    field('fxEnabled').checked = data.enabled;
    field('fxEnabled').disabled = false;
    field('fxThreshold').disabled = false;
    field('fxTargets').replaceChildren();
    for (const target of targets) {
      const label = document.createElement('label');
      const checkbox = document.createElement('input');
      checkbox.type = 'checkbox';
      checkbox.style.width = 'auto';
      checkbox.value = String(target.id);
      checkbox.checked = data.targets.includes(target.id);
      checkbox.disabled = !target.enabled;
      checkbox.addEventListener('change', markDirty);
      label.append(checkbox, document.createTextNode(' ' + target.name + (target.enabled ? '' : '（已停用）')));
      field('fxTargets').append(label);
    }
    const missing = data.targets.filter(id => !targets.some(target => target.id === id));
    if (missing.length) {
      const notice = document.createElement('p');
      notice.textContent = '已刪除的目標：' + missing.join(', ') + '；請重新選擇並儲存。';
      notice.className = 'warning';
      field('fxTargets').append(notice);
    }
    if (!targets.length) field('fxTargets').textContent = '請先在 ntfy 卡片新增並啟用通知目標。';
    dirty = false;
    currencyChanged = false;
  }

  function dateText(epoch) {
    return epoch ? new Date(epoch * 1000).toLocaleString('zh-TW', {timeZone: 'Asia/Taipei', hour12: false}) : '尚無資料';
  }

  /** 讀取失敗時不保留上一個規則，避免誤以為仍套用。 */
  function showUnavailable() {
    const rows = field('fxRule');
    rows.replaceChildren();
    const term = document.createElement('dt');
    term.textContent = '目前報價';
    const detail = document.createElement('dd');
    detail.textContent = '無法讀取設定';
    rows.append(term, detail);
    field('fxRuleNote').textContent = '連線恢復後會自動更新。';
    field('fxRuleDraft').hidden = true;
    field('fxRuleDraft').textContent = '';
  }

  /** 依裝置回傳的狀態顯示已套用規則；不在前端重算到價或武裝判斷。 */
  function renderRule() {
    if (!state) return;
    const data = state;
    const rows = field('fxRule');
    rows.replaceChildren();
    const add = (label, value) => {
      const term = document.createElement('dt');
      term.textContent = label;
      const detail = document.createElement('dd');
      detail.textContent = value;
      rows.append(term, detail);
    };
    if (!data.threshold) {
      add('觸發通知', '尚未設定門檻');
    } else {
      add('觸發通知', '報價 ≤ ' + data.thresholdText + ' TWD（等於也算）');
      add('重新武裝', '報價 > ' + data.rearmText + ' TWD 後才可能再次通知');
    }
    if (data.price === null) {
      add('目前報價', '尚未取得報價');
    } else {
      let relation = '';
      if (data.threshold) {
        const percent = (data.price / data.threshold - 1) * 100;
        relation = '，' + (percent <= 0 ? '低於' : '高於') + '門檻 ' + Math.abs(percent).toFixed(2) + '%';
      }
      add('目前報價', data.priceText + ' TWD' + relation + '｜' + (data.armed ? '已武裝' : '未武裝'));
    }
    add('檢查時間', data.times ? data.times.replace(/,/g, '、') + '｜下一次：' + data.nextCheck : '未設定');
    add('今日通知', data.notifiedToday ? '是；今日不會再通知' : '否');

    let note;
    if (!data.enabled) note = '提醒已停用；這些條件目前不會發送通知。';
    else if (data.price === null || !data.fresh) note = '目前沒有有效報價，不會判斷。';
    else if (data.notifiedToday) note = '今日已通知，需等明天後才會再通知。';
    else if (!data.armed) note = '尚未武裝：報價需先超過 ' + data.rearmText + ' TWD。';
    else if (data.threshold && data.price <= data.threshold) note = '已符合門檻；下一次檢查會通知。';
    else if (data.evaluated) note = '這份報價已判斷過，需等來源更新後才會重新判斷。';
    else note = '尚未到價；下一次檢查時再判斷。';
    field('fxRuleNote').textContent = note;

    // 未儲存的輸入只做客製試算；儲存後以裝置回傳的實際邊界為準。
    const draft = field('fxRuleDraft');
    const threshold = Number(field('fxThreshold').value);
    const margin = Number(field('fxMargin').value);
    if (!dirty) {
      draft.hidden = true;
      draft.textContent = '';
    } else if (!(threshold > 0) || !Number.isFinite(margin) || margin < 0) {
      draft.hidden = false;
      draft.textContent = '若儲存目前輸入：請先填入正數門檻與非負的回升幅度。';
    } else {
      const rearm = String(Number((threshold * (1 + margin / 100)).toFixed(6)));
      draft.hidden = false;
      draft.textContent = '若儲存目前輸入：報價 ≤ ' + String(Number(threshold.toFixed(6))) + ' 通知；> ' + rearm + ' 才重新武裝。';
    }
  }

  /** 只更新狀態；有未儲存編輯時不覆蓋表單。 */
  async function refresh(reset = false) {
    const requestGeneration = generation;
    try {
      const responses = await Promise.all([
        fetchTimed('/api/exchange', {cache: 'no-store'}),
        fetchTimed('/api/ntfy', {cache: 'no-store'})
      ]);
      if (responses.some(response => !response.ok)) throw Error('status');
      const [data, ntfy] = await Promise.all(responses.map(response => response.json()));
      if (requestGeneration !== generation || (saving && !reset)) return;
      state = data;
      if (!initialized || reset || !dirty) loadForm(data, ntfy.targets);
      initialized = true;
      loaded = true;
      // 顯示位數由裝置統一決定（rateText）；舊韌體沒有 priceText 時才退回原始值。
      const shown = data.priceText ?? data.price?.toLocaleString('zh-TW', {maximumFractionDigits: 8});
      field('fxQuote').textContent = data.price === null ? '尚未取得 ' + data.currency + ' 報價' :
        '1 ' + data.currency + ' = ' + shown + ' TWD';
      field('fxTime').textContent = '來源時間：' + dateText(data.sourceTime) + ' · 裝置取得：' + dateText(data.fetchedAt) +
        ' · 來源預計更新：' + dateText(data.nextUpdate);
      field('fxStatus').className = 'status-box ' + (data.busy ? 'pending' : !data.fresh || !data.enabled ? 'warning' : 'ok');
      field('fxStatus').textContent = (data.enabled ? '提醒已啟用' : '提醒未啟用') +
        (!data.clockReady ? ' · 尚未校時' : '') + (!data.fresh ? ' · 無有效報價／資料已過期' : '') +
        (!data.targetsReady ? ' · 通知目標未設定、停用或已刪除' : '') + ' · ' + data.message;
      renderRule();
    } catch (error) {
      loaded = false;
      field('fxStatus').className = 'status-box error';
      field('fxStatus').textContent = '無法讀取裝置匯率狀態；畫面可能為舊資料，稍後重試。';
      showUnavailable();
    } finally {
      buttons();
    }
  }

  function formValues() {
    return {
      currency: field('fxCurrency').value,
      threshold: field('fxThreshold').value || '0',
      margin: field('fxMargin').value,
      times: field('fxTimes').value.replace(/\s/g, ''),
      targets: [...field('fxTargets').querySelectorAll('input:checked:not(:disabled)')].map(input => input.value).join(','),
      enabled: String(field('fxEnabled').checked && !currencyChanged)
    };
  }

  /** 提交後等待裝置狀態回讀；回應不明時不自動重送測試。 */
  async function action(kind) {
    if (saving || state?.busy) return;
    const admin = adminField.value;
    if (!adminToken && !admin) {
      result('error', '✗ 請先在頁面上方登入管理設定。');
      return;
    }
    generation++;
    saving = true;
    buttons();
    const disabledBefore = new Map([...field('fxForm').querySelectorAll('input,select')].map(input => [input, input.disabled]));
    disabledBefore.forEach((disabled, input) => input.disabled = true);
    result('pending', '⏳ 正在提交…');
    try {
      // 在停用欄位前的 disabled 狀態會影響目標篩選，因此使用提交前擷取的資料。
      const values = kind === 'save' ? pendingValues : {};
      const response = await fetchTimed('/api/exchange' + (kind === 'save' ? '' : '/' + kind), {
        method: 'POST', headers: authHeaders(), body: new URLSearchParams({...values, admin})
      });
      noteForbidden(response);
      const message = await response.text();
      result(response.ok ? 'ok' : 'error', (response.ok ? '✓ ' : '✗ ') + message);
      if (response.ok) {
        if (kind === 'save') await refresh(true);
        else state.busy = true;
      }
    } catch (error) {
      result('warning', '⚠ 未收到回應，請重新整理確認；測試可能已送達，不會自動重送。');
    } finally {
      disabledBefore.forEach((disabled, input) => { if (input.isConnected) input.disabled = disabled; });
      // 狀態重繪會恢復停用目標及幣別變更的限制。
      saving = false;
      if (!dirty) await refresh();
      field('fxEnabled').disabled = currencyChanged;
      field('fxThreshold').disabled = currencyChanged;
      buttons();
    }
  }

  let pendingValues = {};
  field('fxForm').addEventListener('submit', event => {
    event.preventDefault();
    if (field('fxSave').disabled) return;
    pendingValues = formValues();
    action('save');
  });
  field('fxRefresh').addEventListener('click', () => { if (!field('fxRefresh').disabled) action('refresh'); });
  field('fxTest').addEventListener('click', () => { if (!field('fxTest').disabled) action('test'); });
  async function poll() {
    await refresh();
    setTimeout(poll, 5000);
  }
  poll();
})();
