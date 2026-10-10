#pragma once
#include <WebServer.h>

// 註冊天氣通知排程相關路由（/api/notify/weather、/test）。
void setupNotify(WebServer& server);

// 由 loop() 定期呼叫：到點時抓天氣並透過 ntfy 發送。
void serviceNotify();
