#pragma once
#include <WebServer.h>

void setupWiFiManager(WebServer& server);
void serviceWiFiManager();

// 共用天氣地區（供天氣通知使用）。name 為空代表尚未設定。
bool getWeatherLocation(double& latitude, double& longitude, String& name);
