#pragma once
#include <WebServer.h>

// 以裝置管理密碼初始化驗證（保存一份密碼副本並載入既有憑證）。
void setupAuth(const String& adminPassword);

// 請求是否通過驗證：管理密碼（admin 參數）或有效且未過期的 Bearer 憑證。
bool authIsAuthorized(WebServer& server);

// 註冊驗證相關路由：POST /api/login、GET /api/admin、POST /api/logout。
void registerAuthRoutes(WebServer& server);
