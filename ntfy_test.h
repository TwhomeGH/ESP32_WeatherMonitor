#pragma once
#include <WebServer.h>
void setupNtfyTest(WebServer& server);

// 是否已設定有效的 ntfy 主題。
bool ntfyConfigured();

// 以目前儲存的主題／權杖同步發送一則 ntfy 通知。回傳是否 2xx，httpCode 為狀態碼（無連線為負值）。
bool ntfyPublish(const String& title, const String& message, const String& tags, int& httpCode);
