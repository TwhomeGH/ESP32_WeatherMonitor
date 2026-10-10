#pragma once
#include <Arduino.h>

// 獨立標頭可避免 Arduino 將 JavaScript 誤判為 C++ 而插入宣告。
// Web UI：存放在 Flash，減少 RAM 佔用
const char PAGE[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="zh-Hant">
<head>
<meta charset="UTF-8">
<meta name="viewport"
      content="width=device-width,initial-scale=1">
<title>ESP32 Monitor</title>
<style>
* { box-sizing: border-box; }

body {
  margin: 0;
  padding: 24px;
  background: #0b1120;
  color: #e2e8f0;
  font-family: system-ui, sans-serif;
}

main {
  max-width: 900px;
  margin: auto;
}

header {
  display: flex;
  justify-content: space-between;
  align-items: center;
  margin-bottom: 24px;
}

h1 { font-size: 24px; margin: 0; }
h2 { font-size: 15px; margin: 0 0 12px; }

.subtitle, .muted {
  color: #94a3b8;
  font-size: 13px;
}

.status {
  padding: 7px 12px;
  border-radius: 30px;
  background: #064e3b;
  color: #6ee7b7;
  font-size: 12px;
}

.grid {
  display: grid;
  grid-template-columns: repeat(2, 1fr);
  gap: 14px;
}

.card {
  background: #172033;
  border: 1px solid #293548;
  border-radius: 14px;
  padding: 20px;
}

.value {
  font-size: 28px;
  font-weight: 700;
  margin-top: 10px;
  overflow-wrap: anywhere;
}

.unit {
  color: #94a3b8;
  font-size: 13px;
}

.bar {
  height: 10px;
  border-radius: 10px;
  background: #334155;
  overflow: hidden;
  margin-top: 16px;
}

.fill {
  height: 100%;
  width: 0%;
  background: #38bdf8;
  transition: width .4s;
}

.wide {
  grid-column: 1 / -1;
}

.details {
  display: grid;
  grid-template-columns: 1fr auto;
  gap: 12px;
  font-size: 14px;
}

.details span:nth-child(odd) {
  color: #94a3b8;
}

footer {
  margin-top: 22px;
  color: #64748b;
  font-size: 12px;
}

@media(max-width: 540px) {
  body { padding: 14px; }
  .grid { grid-template-columns: 1fr; }
  .wide { grid-column: auto; }
  .value { font-size: 25px; }
}
html { color-scheme: dark; }

.btn {
  display: inline-block;
  margin-top: 14px;
  padding: 10px 14px;
  border-radius: 8px;
  background: #38bdf8;
  color: #0b1120;
  font-weight: 700;
  text-decoration: none;
  transition: background .2s;
}

.btn:hover { background: #7dd3fc; }
</style>
</head>
<body>
<main>
<header>
  <div>
    <h1>ESP32 Monitor</h1>
    <div class="subtitle">Hardware Dashboard v1.1 · <a href="/docs" style="color:#7dd3fc">使用說明</a> · <a href="/settings" style="color:#7dd3fc">設定</a></div>
  </div>
  <div id="status" class="status">Connecting</div>
</header>

<section class="grid">
  <div class="card wide">
    <h2>Wi-Fi 狀態</h2>
    <p id="wifiState" class="muted">正在讀取連線狀態…</p>
    <p id="wifiRecovery" class="muted"></p>
    <a class="btn" href="/settings">變更 Wi-Fi／天氣地區設定 →</a>
  </div>
  <div class="card wide">
    <h2 id="weatherTitle">網路天氣</h2>
    <div class="value" id="weatherSummary">讀取中…</div>
    <p class="muted" id="weatherStatus" role="status">正在取得天氣資料</p>
    <div class="details">
      <span>溫度</span><strong id="temperature">--</strong>
      <span>相對濕度</span><strong id="humidity">--</strong>
      <span>風速（10 公尺高度）</span><strong id="windSpeed">--</strong>
      <span>風向（來向）</span><strong id="windDirection">--</strong>
      <span>資料時間（台灣）</span><strong id="weatherTime">--</strong>
    </div>
    <p class="muted">每 10 分鐘更新 · 所選地區模式估計，非現場感測器實測。開啟此頁的裝置需要可連上網際網路 · <a href="/settings" style="color:#7dd3fc">更換地區</a></p>
    <a href="https://open-meteo.com/" target="_blank" rel="noopener noreferrer" style="color:#7dd3fc">天氣資料：Open-Meteo</a>
  </div>

  <div class="card">
    <h2>CPU 時脈</h2>
    <div class="value">
      <span id="cpu">--</span>
      <span class="unit">MHz</span>
    </div>
    <div class="muted" id="cores">--</div>
    <p class="muted">時脈通常固定，不代表 CPU 使用率。</p>
  </div>

  <div class="card">
    <h2>Free Heap</h2>
    <div class="value">
      <span id="free">--</span>
      <span class="unit">KB</span>
    </div>
    <div class="muted" id="largest">--</div>
  </div>

  <div class="card wide">
    <h2>Heap Memory</h2>
    <div class="value">
      <span id="usage">--</span>
      <span class="unit">%</span>
    </div>
    <div class="bar">
      <div id="bar" class="fill"></div>
    </div>
    <p class="muted" id="heapDetail">--</p>
  </div>

  <div class="card">
    <h2>Flash 實體總容量</h2>
    <div class="value">
      <span id="flash">--</span>
      <span class="unit">MiB</span>
    </div>
  </div>

  <div class="card">
    <h2>Uptime</h2>
    <div class="value" id="uptime">--</div>
  </div>

  <div class="card wide">
    <h2>程式儲存空間</h2>
    <div class="value"><span id="storageUsage">--</span><span class="unit"> %</span></div>
    <div class="bar"><div id="storageBar" class="fill"></div></div>
    <p class="muted" id="storageDetail">--</p>
    <p class="muted">已用量為目前韌體大小；剩餘量僅指目前程式分區，不代表可儲存檔案的空間。Flash 另含系統、更新或資料分區。</p>
  </div>

  <div class="card wide">
    <h2>System Information</h2>
    <div class="details">
      <span>Chip</span><strong id="chip">--</strong>
      <span>Revision</span><strong id="rev">--</strong>
      <span>SDK</span><strong id="sdk">--</strong>
      <span>Wi-Fi IP</span><strong id="ip">--</strong>
      <span>Wi-Fi 模式</span><strong id="wifiMode">--</strong>
      <span>Min Free Heap</span><strong id="minHeap">--</strong>
    </div>
  </div>
</section>

<footer>
  每 2 秒更新 · <span id="lastUpdate">尚未取得監控資料</span>
</footer>
</main>

<script>
const el = id => document.getElementById(id);
async function refreshWiFi() {
  const controller = new AbortController();
  const timer = setTimeout(() => controller.abort(), 8000);
  try {
    const res = await fetch('/api/wifi', {cache:'no-store', signal:controller.signal});
    if (!res.ok) throw Error('Wi-Fi status unavailable');
    const d = await res.json();
    el('wifiState').textContent = d.message + ' · 已儲存 SSID：' + d.ssid + (d.connected ? ' · IP：' + d.ip : '');
    el('wifiRecovery').textContent = '備援熱點：' + d.apName + (d.apActive ? '（已開啟）' : '（切換網路或連線失敗時開啟）');
    el('wifiMode').textContent = d.apActive ? '路由器連線＋備援熱點' : '連接路由器（STA）';
  } catch (e) {
    el('wifiState').textContent = '無法取得連線狀態；若正在切換網路，請連接備援熱點查看結果。';
  } finally { clearTimeout(timer); setTimeout(refreshWiFi, 3000); }
}
refreshWiFi();

function uptime(seconds) {
  const d = Math.floor(seconds / 86400);
  const h = Math.floor(seconds % 86400 / 3600);
  const m = Math.floor(seconds % 3600 / 60);
  const s = seconds % 60;
  return d ? `${d}d ${h}h ${m}m` :
         `${h}h ${m}m ${s}s`;
}

async function refresh() {
  try {
    const res = await fetch('/api/status', {
      cache: 'no-store'
    });
    if (!res.ok) throw Error('HTTP ' + res.status);

    const d = await res.json();
    const kb = n => (n / 1024).toFixed(1);

    el('cpu').textContent = d.cpuMHz;
    el('cores').textContent = d.cores + ' cores';
    el('free').textContent = kb(d.freeHeap);
    el('largest').textContent =
      'Largest block: ' + kb(d.largestBlock) + ' KB';

    const used = d.totalHeap - d.freeHeap;
    const percent = d.totalHeap > 0
      ? used / d.totalHeap * 100 : 0;

    el('usage').textContent = percent.toFixed(1);
    el('bar').style.width = percent + '%';
    el('heapDetail').textContent =
      kb(used) + ' / ' + kb(d.totalHeap) + ' KB';

    el('flash').textContent =
      (d.flash / 1048576).toFixed(1);

    const storagePercent = d.appTotal > 0 ? d.appUsed / d.appTotal * 100 : 0;
    el('storageUsage').textContent = d.appTotal > 0 ? storagePercent.toFixed(1) : '--';
    el('storageBar').style.width = Math.min(100, storagePercent) + '%';
    el('storageDetail').textContent = d.appTotal > 0
      ? '已用 ' + kb(d.appUsed) + ' / 總計 ' + kb(d.appTotal) +
        ' KiB · 剩餘 ' + kb(d.appFree) + ' KiB'
      : '無法取得目前程式分區容量';

    el('uptime').textContent = uptime(d.uptime);
    el('chip').textContent = d.chip;
    el('rev').textContent = d.revision;
    el('sdk').textContent = d.sdk;
    el('ip').textContent = d.ip;
    el('minHeap').textContent =
      kb(d.minHeap) + ' KB';

    el('status').textContent = 'Online';
    el('lastUpdate').textContent = '最後更新：' + new Date().toLocaleTimeString('zh-TW');
  } catch (e) {
    el('status').textContent = 'Offline';
    console.error(e);
  }
}

refresh();
setInterval(refresh, 2000);
</script>
<script src="/assets/weather.js"></script>
</body>
</html>
)rawliteral";
