#include "notify.h"
#include "weather.h"
#include "ntfy_test.h"
#include "wifi_manager.h"
#include "auth.h"
#include <WiFi.h>
#include <Preferences.h>
#include <time.h>

namespace {
struct WeatherSchedule {
  uint8_t enabled;        // 0/1
  uint8_t mode;           // 0 每 N 小時、1 每天、2 每週
  uint8_t intervalHours;  // 1..24
  uint8_t startHour;      // 0..23（每 N 小時的起算整點）
  uint8_t dailyHour, dailyMinute;
  uint8_t weeklyMask;     // bit0=一 .. bit6=日
  uint8_t weeklyHour, weeklyMinute;
  uint32_t lastSent;      // 上次排程發送的 epoch（0=無）
};

Preferences prefs;
WeatherSchedule config = {};
bool ready = false;
bool busy = false;
uint32_t lastCheck = 0;
uint32_t lastAttempt = 0;
char resultMessage[240] = "尚未發送天氣通知";
int resultCode = 0;
portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;

bool clockReady() { return time(nullptr) > 1735689600; }

void loadDefaults() {
  config = {};
  config.intervalHours = 3;
  config.startHour = 0;
  config.dailyHour = 8; config.dailyMinute = 0;
  config.weeklyMask = 0x1F; config.weeklyHour = 8; config.weeklyMinute = 0;
}

void saveConfig() { if (ready) prefs.putBytes("weather", &config, sizeof(config)); }

void setResult(int code, const String& message) {
  portENTER_CRITICAL(&lock);
  resultCode = code;
  strlcpy(resultMessage, message.c_str(), sizeof(resultMessage));
  busy = false;
  portEXIT_CRITICAL(&lock);
}

String resultText(int code) {
  if (code >= 200 && code < 300) return "ntfy 已接受天氣通知，請確認 Android 是否收到。";
  if (code == 401 || code == 403) return "ntfy 認證或主題權限失敗，請檢查存取權杖與主題。";
  if (code == 429) return "ntfy 限流，請稍後再試。";
  if (code < 0) return "連線／TLS 失敗或逾時；請確認 ESP32 可上網且已完成校時。";
  return "ntfy 未接受通知，狀態碼 " + String(code) + "。";
}

// 依目前設定與裝置時鐘，計算下一次預計發送時間（0=不適用）。
time_t nextSendTime() {
  if (!config.enabled || !clockReady()) return 0;
  const time_t now = time(nullptr);
  struct tm base;
  localtime_r(&now, &base);
  if (config.mode == 1) {
    struct tm t = base;
    t.tm_hour = config.dailyHour; t.tm_min = config.dailyMinute; t.tm_sec = 0; t.tm_isdst = -1;
    time_t candidate = mktime(&t);
    if (candidate <= now) candidate += 86400;
    return candidate;
  }
  if (config.mode == 2) {
    for (int d = 0; d <= 7; ++d) {
      struct tm t = base;
      t.tm_mday += d; t.tm_hour = config.weeklyHour; t.tm_min = config.weeklyMinute; t.tm_sec = 0; t.tm_isdst = -1;
      const time_t candidate = mktime(&t);
      struct tm wd;
      localtime_r(&candidate, &wd);
      const int bit = (wd.tm_wday == 0) ? 6 : wd.tm_wday - 1;
      if ((config.weeklyMask & (1 << bit)) && candidate > now) return candidate;
    }
    return 0;
  }
  for (int d = 0; d <= 8; ++d) {
    struct tm t = base;
    t.tm_mday += d; t.tm_hour = 0; t.tm_min = 0; t.tm_sec = 0; t.tm_isdst = -1;
    const time_t midnight = mktime(&t);
    for (int h = 0; h < 24; ++h) {
      if (((h - config.startHour + 24) % config.intervalHours) != 0) continue;
      const time_t candidate = midnight + (time_t)h * 3600;
      if (candidate > now) return candidate;
    }
  }
  return 0;
}

String formatTime(time_t value) {
  struct tm local;
  localtime_r(&value, &local);
  char buf[24];
  strftime(buf, sizeof(buf), "%m/%d %H:%M", &local);
  return String(buf);
}

void weatherTask(void*) {
  double latitude, longitude;
  String name;
  if (!getWeatherLocation(latitude, longitude, name)) { setResult(-1, "尚未設定天氣地區，請先在設定頁設定。"); vTaskDelete(nullptr); return; }
  WeatherData data;
  String error;
  if (!fetchWeather(latitude, longitude, data, error)) { setResult(-1, "取得天氣失敗：" + error); vTaskDelete(nullptr); return; }
  int code = -1;
  ntfyPublish("天氣 · " + name, weatherMessage(data), "[\"cloud\"]", code);
  setResult(code, resultText(code));
  vTaskDelete(nullptr);
}

bool startWeatherSend() {
  portENTER_CRITICAL(&lock);
  const bool sending = busy;
  portEXIT_CRITICAL(&lock);
  if (sending) return false;
  portENTER_CRITICAL(&lock);
  busy = true; resultCode = 0;
  strlcpy(resultMessage, "發送中…", sizeof(resultMessage));
  portEXIT_CRITICAL(&lock);
  if (xTaskCreate(weatherTask, "weather-notify", 16384, nullptr, 1, nullptr) != pdPASS) {
    setResult(-1, "記憶體不足，未發送天氣通知。");
    return false;
  }
  return true;
}
}

void setupNotify(WebServer& server) {
  ready = prefs.begin("monitor-notify", false);
  if (ready && prefs.getBytesLength("weather") == sizeof(config)) {
    prefs.getBytes("weather", &config, sizeof(config));
    if (config.mode > 2) config.mode = 0;
    if (config.intervalHours < 1 || config.intervalHours > 24) config.intervalHours = 3;
    if (config.startHour > 23) config.startHour = 0;
  } else {
    loadDefaults();
    saveConfig();
  }

  server.on("/api/notify/weather", HTTP_GET, [&server]() {
    char message[sizeof(resultMessage)];
    bool sending;
    int code;
    portENTER_CRITICAL(&lock);
    sending = busy; code = resultCode;
    memcpy(message, resultMessage, sizeof(message));
    portEXIT_CRITICAL(&lock);
    server.sendHeader("Cache-Control", "no-store");
    const time_t next = nextSendTime();
    server.send(200, "application/json",
      String("{\"enabled\":") + (config.enabled ? "true" : "false") +
      ",\"mode\":" + String(config.mode) +
      ",\"intervalHours\":" + String(config.intervalHours) +
      ",\"startHour\":" + String(config.startHour) +
      ",\"dailyHour\":" + String(config.dailyHour) + ",\"dailyMinute\":" + String(config.dailyMinute) +
      ",\"weeklyMask\":" + String(config.weeklyMask) +
      ",\"weeklyHour\":" + String(config.weeklyHour) + ",\"weeklyMinute\":" + String(config.weeklyMinute) +
      ",\"lastSent\":" + String(config.lastSent) +
      ",\"nextSend\":" + String((uint32_t)next) +
      ",\"nextText\":\"" + (next ? formatTime(next) : String()) + "\"" +
      ",\"configured\":" + String(ntfyConfigured() ? "true" : "false") +
      ",\"clockReady\":" + String(clockReady() ? "true" : "false") +
      ",\"busy\":" + String(sending ? "true" : "false") +
      ",\"code\":" + String(code) + ",\"message\":\"" + String(message) + "\"}");
  });
  server.on("/api/notify/weather", HTTP_POST, [&server]() {
    if (!authIsAuthorized(server)) { server.send(403, "text/plain; charset=utf-8", "管理密碼錯誤。"); return; }
    if (!ready) { server.send(503, "text/plain; charset=utf-8", "設定儲存區無法使用。"); return; }
    const int mode = server.arg("mode").toInt();
    const int ih = server.arg("intervalHours").toInt();
    const int sh = server.arg("startHour").toInt();
    const int dh = server.arg("dailyHour").toInt(), dm = server.arg("dailyMinute").toInt();
    const int wh = server.arg("weeklyHour").toInt(), wm = server.arg("weeklyMinute").toInt();
    const int mask = server.arg("weeklyMask").toInt();
    if (mode < 0 || mode > 2 || ih < 1 || ih > 24 || sh < 0 || sh > 23 || dh < 0 || dh > 23 ||
        dm < 0 || dm > 59 || wh < 0 || wh > 23 || wm < 0 || wm > 59 || mask < 0 || mask > 127) {
      server.send(400, "text/plain; charset=utf-8", "排程數值超出範圍。"); return;
    }
    WeatherSchedule next = config;
    next.enabled = server.arg("enabled") == "true" ? 1 : 0;
    next.mode = (uint8_t)mode;
    next.intervalHours = (uint8_t)ih; next.startHour = (uint8_t)sh;
    next.dailyHour = (uint8_t)dh; next.dailyMinute = (uint8_t)dm;
    next.weeklyMask = (uint8_t)mask; next.weeklyHour = (uint8_t)wh; next.weeklyMinute = (uint8_t)wm;
    if (prefs.putBytes("weather", &next, sizeof(next)) != sizeof(next)) { server.send(500, "text/plain; charset=utf-8", "儲存失敗。"); return; }
    config = next;
    const time_t nextAt = nextSendTime();
    server.send(200, "text/plain; charset=utf-8",
      nextAt ? "天氣通知排程已儲存；下一次預計通知：" + formatTime(nextAt) + "。" : "天氣通知排程已儲存。");
  });
  server.on("/api/notify/weather/test", HTTP_POST, [&server]() {
    if (!authIsAuthorized(server)) { server.send(403, "text/plain; charset=utf-8", "管理密碼錯誤。"); return; }
    if (!ntfyConfigured()) { server.send(400, "text/plain; charset=utf-8", "請先在上方儲存 ntfy 主題。"); return; }
    if (WiFi.status() != WL_CONNECTED) { server.send(503, "text/plain; charset=utf-8", "ESP32 尚未連上路由器。"); return; }
    if (!clockReady()) { server.send(503, "text/plain; charset=utf-8", "正在同步網路時間，稍後再試。"); return; }
    if (busy || (lastAttempt && millis() - lastAttempt < 10000)) { server.send(429, "text/plain; charset=utf-8", "正在發送或距離上次不足 10 秒，請稍後再試。"); return; }
    if (!startWeatherSend()) { server.send(503, "text/plain; charset=utf-8", "無法啟動發送工作。"); return; }
    lastAttempt = millis();
    server.send(202, "text/plain; charset=utf-8", "天氣通知已排入發送，請查看下方結果並確認手機。");
  });
}

void serviceNotify() {
  const uint32_t nowMs = millis();
  if (nowMs - lastCheck < 20000) return;
  lastCheck = nowMs;
  if (!config.enabled || !clockReady() || busy || !ntfyConfigured()) return;
  const time_t t = time(nullptr);
  struct tm local;
  localtime_r(&t, &local);
  bool due = false;
  if (config.mode == 0) {
    due = local.tm_min == 0 && ((local.tm_hour - config.startHour + 24) % config.intervalHours) == 0;
  } else if (config.mode == 1) {
    due = local.tm_hour == config.dailyHour && local.tm_min == config.dailyMinute;
  } else {
    const int bit = (local.tm_wday == 0) ? 6 : local.tm_wday - 1;  // 週一=0 .. 週日=6
    due = (config.weeklyMask & (1 << bit)) && local.tm_hour == config.weeklyHour && local.tm_min == config.weeklyMinute;
  }
  if (!due) return;
  if (config.lastSent && (uint32_t)t - config.lastSent < 90) return;  // 同一時段去重
  config.lastSent = (uint32_t)t;
  saveConfig();
  startWeatherSend();
}
