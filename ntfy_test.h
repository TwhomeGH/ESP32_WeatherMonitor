#pragma once
#include <WebServer.h>
void setupNtfyTest(WebServer& server);

// 是否至少有一組已啟用的 ntfy 目標。
bool ntfyConfigured();

// 依序發送至所有已啟用目標。全部成功才回傳 true；-1000 發送忙碌，-1001 部分成功。
bool ntfyPublish(const String& title, const String& message, const String& tags, int& httpCode);
