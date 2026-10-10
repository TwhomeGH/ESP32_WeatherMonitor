# TODO / 待辦與規劃

本檔記錄之後「可能」要做的事，不是承諾時程。完成後請刪除或標記。

## 期貨日盤提醒（主要）

- 完整設計見 [`FUTURES_PLAN.md`](FUTURES_PLAN.md)。ntfy 手動測試已完成，**交易日排程尚未實作**。
- **待確認**：
  1. 是否需要「到期契約」收盤提醒 → 需指定契約（小台週契約／月契約要分清楚）。
  2. ntfy 伺服器、訂閱主題，以及是否使用需要權限的主題。
- **待實作**：交易日曆、排程與推播、設定頁 UI（商品／開收盤開關／提前分鐘數）。

## OTA 熱更新（選配）

- 分割區已保留雙 app 槽（`PartitionScheme=no_fs`），隨時可加入 `ArduinoOTA`。
- 需在 `.ino` 加 `ArduinoOTA.begin()`（setup）與 `ArduinoOTA.handle()`（loop），並決定 OTA 密碼來源（可沿用裝置管理密碼）。
- 注意：OTA 需要雙槽，與 `huge_app`（單槽）互斥；維持 `no_fs` 即可。

## 裝置管理密碼

- 目前為開機自動產生的隨機值，**只能從序列埠取得、無法更改**。
- 可新增「變更管理密碼」UI（同時更新 NVS 的 `admin` 與備援熱點密碼）。

## 網頁 / 資產

- 儀表板 `/`（`monitor_page.h` 的 `PAGE`）目前**未 gzip**（約 8.8KB）；其餘資產已 gzip，可考慮一併壓縮。
- 目前為 HTTP；若需要更高安全性，可評估 HTTPS（自簽憑證）並重新檢視客戶端是否還需要額外的清理。

## 藍牙（構想）

- BLE 配網、藍牙溫濕度計、近距離控制等，見 [`USER_GUIDE.md`](USER_GUIDE.md) 的「藍牙可以怎麼利用」。

## 文件維護

- README、`USER_GUIDE.md`、本 TODO 隨功能更新；`FUTURES_PLAN.md` 為期貨功能的主設計文件。
