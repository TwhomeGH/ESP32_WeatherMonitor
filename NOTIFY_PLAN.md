# 通知系統設計（期貨 + 天氣）

**狀態：天氣通知（手動 + 基本排程）已實作；期貨開/收盤提醒與「跟隨期貨」尚未實作。** ntfy 設定與手動測試、`weather` 抓取、`notify` 排程已在韌體中。

本功能讓 ESP32 自行排程並透過 ntfy 推播兩類通知：**期貨開/收盤提醒**與**天氣通知**，即使監控網頁關閉也能運作。它是時間提醒工具，不涉及報價判讀、自動下單或交易建議。

## 目標與已決定

- 來源兩種：**期貨**（開/收盤）、**天氣**。
- 觸發兩種：① **跟隨期貨**開/收盤；② **獨立排程**（每 N 小時／每天 HH:MM／每週 週X HH:MM）。
- **共用同一個 ntfy 主題**（一組 topic／token），以訊息 title／tags 區分來源。
- 「每 N 小時」**預設整點對齊**（例如 00:00 起每 3 小時），**也可另設起點時點**。
- 設定由 Web 操作，存 ESP32（NVS），斷電保留。
- 時間一律 **Asia/Taipei（+8，無日光節約）**。

## 名詞

- **事件（event）**：某個時刻「該做什麼」。
- **觸發（trigger）**：決定何時產生事件的規則（interval／daily／weekly／market-event）。
- **內容（content）**：送出的訊息類型（期貨／天氣）。

## 設計原則

- 裝置自行抓取與推播；不依賴瀏覽器開啟。
- 校時未完成、或狀態未知（交易日不明）→ **不送**。
- **去重**：同一分鐘只送一次。
- **持久化**：重開機不重送已送出的時段（NVS 記 `last-sent`）。
- 送出失敗**不自動重試**（與現行手動測試一致）；補送規則見「邊界」。

## 觸發模型

### A. 期貨事件（依交易日曆）

- 開盤前 **X** 分鐘、收盤前 **Y** 分鐘（各自設定，預設各 15 分鐘）。
- 僅在**交易日**觸發；交易日判斷見 [`FUTURES_PLAN.md`](FUTURES_PLAN.md)（商品：TX／MTX／TMF，日盤）。
- 到期契約是否需要另送，見 FUTURES_PLAN 的「實作前待確認」。

### B. 天氣排程

- **跟隨期貨**：在期貨開/收盤同一時刻送天氣。
- **每 N 小時**：預設整點對齊；可另設起點時點。
- **每天 HH:MM**。
- **每週 週X（可多選）HH:MM**。

> [!IMPORTANT]
> 「跟隨期貨」的天氣通知**依賴** `market_calendar`，也就是與期貨功能共用同一套交易日判斷與排程。

**待確認**：

1. 每種排程是否允許**多組**（例如每天兩個時間）？
2. 「每 N 小時」的 N 範圍（例如 1–12）與預設值。
3. 「跟隨期貨」時，該時刻要**只送期貨、只送天氣，或兩者**（合併成一則或分送兩則）？

## 事件 → 內容

| 事件 | 內容 | 訊息要點 |
| --- | --- | --- |
| 期貨開盤/收盤 | 期貨 | 商品、開/收盤、提前幾分鐘、時間 |
| 天氣（任何觸發） | 天氣 | 地區名、溫度、濕度、風速風向、天氣描述 |

## 天氣資料來源（裝置端新增）

現在天氣是**瀏覽器**抓的；要由裝置推播，裝置需自行抓取：

- 以 NVS 的共用地區（`weatherLocation`：latitude／longitude／name）向 Open-Meteo 抓 `current`。
- 欄位與瀏覽器一致：`temperature_2m`、`relative_humidity_2m`、`weather_code`、`wind_speed_10m`、`wind_direction_10m`、`time`。
- **TLS**：已驗證 Open-Meteo 憑證鏈終止於 **ISRG Root X1**，與 ntfy 用的是同一張，直接沿用 `web/isrgrootx1.pem`（即 `ntfy_ca.h` 的 `NTFY_CA`）。
- **JSON 解析**：建議引入 ArduinoJson；或針對此固定結構自寫精簡解析（避免額外相依）。

## ntfy 訊息格式（共用主題）

沿用既有 `POST https://ntfy.sh/`（`Content-Type: application/json`）與選填 `Authorization: Bearer <token>`。

| 來源 | title | tags | message |
| --- | --- | --- | --- |
| 期貨 | `期貨提醒` | `[\"chart_with_upwards_trend\"]` | 商品／開收盤／時間 |
| 天氣 | `天氣` | `[\"cloud\"]` | 地區／溫度／濕度／風／描述 |

## 設定與 API

- NVS：沿用 `monitor-ntfy`（topic／token），新增排程欄位（或另開 `monitor-notify`）。
- API（草稿）：
  - `GET /api/ntfy`：狀態（topic、tokenConfigured、校時、busy、last-sent）。（已有）
  - `POST /api/ntfy`：存 topic／token。（已有）
  - `POST /api/ntfy/test`：手動測試。（已有）
  - `GET/POST /api/notify/schedule`：期貨／天氣排程設定。（新增）
- 設定頁新增「通知排程」區：期貨開/收盤開關與提前分鐘、天氣排程型態與時間、共用主題。

## 模組拆分（建議）

| 模組 | 職責 |
| --- | --- |
| `schedule.cpp/.h` | 規則 → 事件；每分鐘檢查；去重；NVS `last-sent` |
| `market_calendar.cpp/.h` | 交易日與時段（見 FUTURES_PLAN） |
| `weather_fetch.cpp/.h` | Open-Meteo 抓取 + 解析 |
| `notifier.cpp/.h` | 組訊息 + ntfy 發送（可由 `ntfy_test` 擴充） |
| `web/notify.js` + 設定頁 | 通知排程設定 UI |

## 邊界與失敗處理

- 校時未完成（`time()` 早於某基準）→ 不觸發。
- 斷網或抓取失敗 → 記錄，依政策不重送（是否套用 FUTURES_PLAN 的「5 分鐘補送窗」待確認）。
- 重開機：讀 NVS `last-sent`，避免同一時段重送。
- 時區固定 +8（`configTime(8*3600, 0, ...)`）。

## 與現有功能的關係

- ntfy 手動測試（已實作）→ 擴充為排程發送。
- 天氣顯示（瀏覽器端）**維持不變**；裝置端另抓一份供通知用（顯示與通知各抓一次，暫不共用）。

## 驗收項目（草稿）

- [ ] 校時未完成不觸發。
- [ ] 每天／每週／每 N 小時在正確的 Asia/Taipei 時刻各觸發一次，且同一分鐘不重複。
- [ ] 「跟隨期貨」僅在交易日、正確的開/收盤時刻觸發。
- [ ] 重開機後不重送已送過的時段。
- [ ] 共用主題下，期貨與天氣訊息可用 title／tags 分辨。
- [ ] 手動測試不受排程限制。
