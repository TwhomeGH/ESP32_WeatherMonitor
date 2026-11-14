#pragma once
#include <WebServer.h>
void setupNtfyTest(WebServer& server);

// 是否至少有一組已啟用的 ntfy 目標。
bool ntfyConfigured();

// 依序發送至所有已啟用目標。全部成功才回傳 true；-1000 發送忙碌，-1001 部分成功。
bool ntfyPublish(const String& title, const String& message, const String& tags, int& httpCode);

/** 檢查指定穩定 ID 是否存在且啟用；不回傳主題或權杖。 */
bool ntfyTargetEnabled(uint32_t id);

/**
 * 僅送至指定且啟用的目標，不會回退成群發。
 * 回傳是否 2xx；httpCode：-1000 忙碌、-1002 目標已刪除或停用，其他為 HTTP／傳輸錯誤。
 * title/message/tags 格式與 ntfyPublish 相同；不自動重試。
 */
bool ntfyPublishTo(uint32_t id, const String& title, const String& message, const String& tags, int& httpCode);
