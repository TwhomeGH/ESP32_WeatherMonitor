#pragma once
#include <WebServer.h>

void setupWiFiManager(WebServer& server);
void serviceWiFiManager();

// 取得共用座標與通知地名；略過地名時 name 可為空，回傳值表示地區是否有效。
bool getWeatherLocation(double& latitude, double& longitude, String& name);
