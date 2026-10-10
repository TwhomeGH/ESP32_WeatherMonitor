# TODO / 待辦與規劃

本檔記錄之後「可能」要做的事，不是承諾時程。完成後請刪除或標記。

## 通知系統（期貨 + 天氣）（主要）

- 設計文件：[`NOTIFY_PLAN.md`](NOTIFY_PLAN.md)；期貨交易日細節另見 [`FUTURES_PLAN.md`](FUTURES_PLAN.md)。
- 已決定：天氣與期貨**共用一個 ntfy 主題**；「每 N 小時」預設整點對齊、可另設起點。
- **待確認**：每種排程是否多組、「跟隨期貨」時天氣與期貨要合併或分送、補送／重試策略；到期契約（見 FUTURES_PLAN）。
- **待實作**：`schedule`／`market_calendar`／`weather_fetch`／`notifier` 模組、設定頁通知 UI、`/api/notify/schedule`。

## OTA 熱更新（選配）

- 分割區已保留雙 app 槽（`PartitionScheme=no_fs`），隨時可加入 `ArduinoOTA`。
- 需在 `.ino` 加 `ArduinoOTA.begin()`（setup）與 `ArduinoOTA.handle()`（loop），並決定 OTA 密碼來源（可沿用裝置管理密碼）。
- 注意：OTA 需要雙槽，與 `huge_app`（單槽）互斥；維持 `no_fs` 即可。

## 裝置管理密碼 / 登入

- 管理密碼由開機自動產生，**只能從序列埠取得、無法更改**。
- 已實作登入憑證（`auth` 模組）：登入後發 7 天 token 存瀏覽器，免重複輸入；可登出或由有效期自動失效。
- 待辦：新增「變更管理密碼」UI（同時更新 NVS 的 `admin`、備援熱點密碼，並**作廢所有 token**）。

## 網頁 / 資產

- 儀表板 `/`（`monitor_page.h` 的 `PAGE`）目前**未 gzip**（約 8.8KB）；其餘資產已 gzip，可考慮一併壓縮。
- 目前為 HTTP；若需要更高安全性，可評估 HTTPS（自簽憑證）並重新檢視客戶端是否還需要額外的清理。

## 藍牙（構想）

- BLE 配網、藍牙溫濕度計、近距離控制等，見 [`USER_GUIDE.md`](USER_GUIDE.md) 的「藍牙可以怎麼利用」。

## 文件維護

- README、`USER_GUIDE.md`、本 TODO 隨功能更新；`FUTURES_PLAN.md` 為期貨功能的主設計文件。
