# ESP32 Weather Monitor（ESP32 天氣監控）

以 ESP32 提供的本地監控網頁：顯示硬體狀態與 Open-Meteo 網路天氣，並可設定 Wi-Fi、天氣地區與（規劃中）期貨與天氣通知。

## 功能

- **儀表板**：晶片、記憶體、Flash、執行時間等狀態，約每 2 秒更新。
- **網路天氣**：Open-Meteo，顯示溫度、濕度、風速與風向（每 10 分鐘更新）。
- **設定頁** `/settings`：集中管理 Wi-Fi 與共用天氣地區。
- **說明頁** `/docs`：內建使用說明，離線（無網際網路）也可閱讀。
- **首次開機**：無預設 Wi-Fi 憑證，會自動開啟備援熱點 `ESP32-Setup-<MAC>` 供設定；管理密碼印於序列埠（115200）並儲存於裝置。

## 硬體

- ESP32（4MB Flash）。
- Arduino ESP32 core 3.3.x。

## 建置與上傳

- **Arduino IDE**：開發板 `ESP32 Dev Module`，Partition Scheme 選 `No FS 4MB (2MB APP x2)`，上傳。
- **命令列一鍵**：

  ```sh
  node tools/flash.cjs
  ```

  會先產生 Web 資產再編譯上傳，並自動使用 Arduino IDE 內建的 arduino-cli（開發板取自 `sketch.yaml`、序列埠自動偵測）。只想編譯用 `--compile-only`。

- 修改 `web/` 內資源或 `.md` 文件後，需重新產生 `docs_assets.h`：`tools/flash.cjs` 會自動處理，或執行 `node tools/generate-web-assets.cjs`。
- **VS Code IntelliSense**：先執行一次 `node tools/flash.cjs --compile-only`，會產生 `build/compile_commands.json`；`.vscode/` 已設好指向它，標頭檔（如 `WebServer.h`）即可被正確解析。

## 文件

- 使用說明：`USER_GUIDE.md`
- 通知系統設計（期貨 + 天氣）：`NOTIFY_PLAN.md`
- 期貨提醒規劃：`FUTURES_PLAN.md`
- 待辦與規劃：`TODO.md`

## 授權

MIT，詳見 [`LICENSE`](LICENSE)。
