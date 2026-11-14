#pragma once
#include <WebServer.h>

/** 載入匯率設定並註冊 API；server 為已啟用管理驗證的共用 WebServer。 */
void setupExchange(WebServer& server);

/** 主迴圈呼叫；校時完成後啟動背景抓取、依台灣時間檢查排程，不阻塞 Web。 */
void serviceExchange();
