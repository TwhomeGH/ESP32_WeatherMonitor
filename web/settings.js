const el = id => document.getElementById(id);

async function fetchTimed(url, options = {}) {
  const controller = new AbortController();
  const timeout = setTimeout(() => controller.abort(), 15000);
  try { return await fetch(url, {...options, signal:controller.signal}); }
  finally { clearTimeout(timeout); }
}

function validLocation(value) {
  return value && typeof value.name === 'string' && value.name.trim() &&
    Number.isFinite(value.latitude) && Math.abs(value.latitude) <= 90 &&
    Number.isFinite(value.longitude) && Math.abs(value.longitude) <= 180;
}

let wifiInitialized = false, wifiSubmitting = false;
async function refreshWiFi() {
  const controller = new AbortController();
  const timer = setTimeout(() => controller.abort(), 8000);
  try {
    const res = await fetch('/api/wifi', {cache:'no-store', signal:controller.signal});
    if (!res.ok) throw Error('Wi-Fi status unavailable');
    const d = await res.json();
    if (!wifiInitialized) { el('wifiSsid').value = d.ssid; wifiInitialized = true; }
    el('wifiState').textContent = d.message + ' · 已儲存 SSID：' + d.ssid + (d.connected ? ' · IP：' + d.ip : '');
    el('wifiRecovery').textContent = '備援熱點：' + d.apName + (d.apActive ? '（已開啟）' : '（切換網路或連線失敗時開啟）');
    el('wifiSave').disabled = d.busy || wifiSubmitting;
  } catch (e) {
    el('wifiState').textContent = '無法取得連線狀態；若正在切換網路，請連接備援熱點查看結果。';
  } finally { clearTimeout(timer); setTimeout(refreshWiFi, 3000); }
}
el('wifiForm').addEventListener('submit', async event => {
  event.preventDefault();
  if (wifiSubmitting) return;
  wifiSubmitting = true;
  el('wifiSave').disabled = true;
  el('wifiResult').textContent = '正在送出設定…';
  const controller = new AbortController();
  const timer = setTimeout(() => controller.abort(), 10000);
  try {
    const body = new URLSearchParams({ssid:el('wifiSsid').value,password:el('wifiPassword').value,admin:el('wifiAdmin').value});
    const res = await fetch('/api/wifi', {method:'POST',body,signal:controller.signal});
    const result = await res.text();
    el('wifiResult').textContent = result;
    if (res.ok) { el('wifiPassword').value = ''; el('wifiAdmin').value = ''; }
  } catch (e) {
    el('wifiResult').textContent = '未收到回應，設定可能已送達。請先連接備援熱點查看結果，勿立即重送。';
  } finally { clearTimeout(timer); wifiSubmitting = false; el('wifiSave').disabled = false; }
});

let locationInitialized = false;
async function refreshLocation() {
  try {
    const response = await fetchTimed('/api/weather-location', {cache:'no-store'});
    if (!response.ok) throw Error('Location unavailable');
    const next = await response.json();
    if (!locationInitialized) {
      el('locationName').value = next.name;
      el('latitude').value = next.latitude;
      el('longitude').value = next.longitude;
      locationInitialized = true;
    }
    el('locationCurrent').textContent = 'ESP32 共用地區：' + next.name + '（' + next.latitude + ', ' + next.longitude + '）';
  } catch (error) {
    el('locationCurrent').textContent = '無法讀取 ESP32 的地區設定，稍後自動重試。';
  } finally { setTimeout(refreshLocation, 5000); }
}

el('locationSearchForm').addEventListener('submit', async event => {
  event.preventDefault();
  const query = el('locationQuery').value.trim();
  if (query.length < 2) { el('locationResult').textContent = '請輸入至少兩個字。'; return; }
  el('locationSearch').disabled = true;
  el('locationResults').replaceChildren();
  el('locationResult').textContent = '正在搜尋地區…';
  try {
    const params = new URLSearchParams({name:query,count:10,language:'zh',format:'json'});
    if (el('locationCountry').value) params.set('countryCode', el('locationCountry').value);
    const response = await fetchTimed('https://geocoding-api.open-meteo.com/v1/search?' + params);
    if (!response.ok) throw Error('Search failed');
    const data = await response.json();
    const results = Array.isArray(data.results) ? data.results : [];
    results.forEach(place => {
      if (!validLocation(place)) return;
      const name = [...new Set([place.name,place.admin1,place.country].filter(Boolean))].join(' · ');
      const button = document.createElement('button');
      button.type = 'button';
      button.textContent = name + '（' + place.latitude + ', ' + place.longitude + '）';
      button.addEventListener('click', () => {
        el('locationName').value = name.slice(0,32);
        el('latitude').value = place.latitude;
        el('longitude').value = place.longitude;
        el('locationResult').textContent = '已填入地區，請輸入管理密碼並儲存。';
      });
      el('locationResults').append(button);
    });
    el('locationResult').textContent = el('locationResults').children.length ? '請選擇正確地區，再儲存。' : '找不到地區；可嘗試英文名稱、附近城市或手動輸入經緯度。';
  } catch (error) { el('locationResult').textContent = '搜尋失敗，請檢查網際網路連線；仍可手動輸入經緯度。'; }
  finally { el('locationSearch').disabled = false; }
});

el('locationForm').addEventListener('submit', async event => {
  event.preventDefault();
  const next = {name:el('locationName').value.trim(),latitude:Number(el('latitude').value),longitude:Number(el('longitude').value)};
  if (!el('latitude').value.trim() || !el('longitude').value.trim() || !validLocation(next)) { el('locationResult').textContent = '請輸入有效地區名稱與經緯度。'; return; }
  el('locationSave').disabled = true;
  try {
    const response = await fetchTimed('/api/weather-location', {method:'POST',body:new URLSearchParams({...next,admin:el('locationAdmin').value})});
    el('locationResult').textContent = await response.text();
    if (response.ok) el('locationAdmin').value = '';
  } catch (error) { el('locationResult').textContent = '未收到回應；請查看上方 ESP32 共用地區，確認是否已儲存。'; }
  finally { el('locationSave').disabled = false; }
});

refreshWiFi();
refreshLocation();

let ntfyInitialized = false, ntfySubmitting = false, ntfySavedTopic = '';
let ntfyReady = false, ntfyBusy = false;
function updateNtfyButtons() {
  el('ntfySave').disabled = ntfySubmitting || ntfyBusy;
  el('ntfyTest').disabled = ntfySubmitting || ntfyBusy || !ntfyReady || !ntfySavedTopic ||
    el('ntfyTopic').value !== ntfySavedTopic || !!el('ntfyToken').value || el('ntfyClearToken').checked;
}
['ntfyTopic','ntfyToken','ntfyClearToken'].forEach(id => el(id).addEventListener('input', updateNtfyButtons));
async function refreshNtfy() {
  try {
    const response = await fetchTimed('/api/ntfy', {cache:'no-store'});
    if (!response.ok) throw Error('Status unavailable');
    const d = await response.json();
    ntfySavedTopic = d.topic;
    ntfyReady = d.clockReady;
    ntfyBusy = d.busy;
    if (!ntfyInitialized) { el('ntfyTopic').value = d.topic; ntfyInitialized = true; }
    el('ntfyStatus').textContent = d.message + (d.code ? '（狀態碼 ' + d.code + '）' : '') +
      (!d.clockReady ? ' · 正在同步網路時間，完成後才能測試 HTTPS 通知。' : '');
    el('ntfyTokenState').textContent = (d.tokenConfigured ? '已儲存權杖；' : '未設定權杖；') + '留空保留現有設定。';
  } catch (error) {
    ntfyReady = false;
    el('ntfyStatus').textContent = '無法讀取通知狀態，稍後自動重試。';
  } finally { updateNtfyButtons(); setTimeout(refreshNtfy, 3000); }
}
el('ntfyForm').addEventListener('submit', async event => {
  event.preventDefault();
  if (ntfySubmitting || ntfyBusy) return;
  ntfySubmitting = true; updateNtfyButtons();
  try {
    const response = await fetchTimed('/api/ntfy', {method:'POST',body:new URLSearchParams({
      topic:el('ntfyTopic').value,token:el('ntfyToken').value,
      clearToken:String(el('ntfyClearToken').checked),admin:el('ntfyAdmin').value})});
    el('ntfyResult').textContent = await response.text();
    if (response.ok) {
      ntfySavedTopic = el('ntfyTopic').value;
      el('ntfyToken').value = ''; el('ntfyClearToken').checked = false;
    }
  } catch (error) { el('ntfyResult').textContent = '未收到儲存回應，請等候狀態更新後再試。'; }
  finally { ntfySubmitting = false; updateNtfyButtons(); }
});
el('ntfyTest').addEventListener('click', async () => {
  if (el('ntfyTest').disabled) return;
  if (!el('ntfyAdmin').value) { el('ntfyResult').textContent = '請輸入裝置管理密碼。'; return; }
  ntfySubmitting = true; updateNtfyButtons();
  try {
    const response = await fetchTimed('/api/ntfy/test', {method:'POST',body:new URLSearchParams({admin:el('ntfyAdmin').value})});
    el('ntfyResult').textContent = await response.text();
    if (response.ok) { ntfyBusy = true; el('ntfyAdmin').value = ''; }
  } catch (error) {
    el('ntfyResult').textContent = '未收到回應，測試可能已送達；請先查看手機，不會自動重送。';
  } finally { ntfySubmitting = false; updateNtfyButtons(); }
});
refreshNtfy();
