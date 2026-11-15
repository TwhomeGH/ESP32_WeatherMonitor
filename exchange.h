#pragma once
#include <WebServer.h>

/** 載入匯率設定並註冊 API；server 為已啟用管理驗證的共用 WebServer。 */
void setupExchange(WebServer& server);

/** 主迴圈呼叫；遵守查詢與退避期限，背景取得台新新報價後判斷到價。 */
void serviceExchange();
