#include "wifi_manager.h"
#include "ntfy_test.h"
#include <WiFi.h>
#include <Preferences.h>
#include <esp_system.h>
#include <math.h>

namespace {
struct Credentials { char ssid[33]; char password[65]; };
Preferences preferences;
Credentials saved = {}, candidate = {};
struct WeatherLocation { double latitude; double longitude; char name[97]; };
WeatherLocation weatherLocation = {24.967, 121.436, "新北市土城區"};
String adminPassword, apName;
String message = "正在連接 Wi-Fi";
bool storageReady = false, connecting = false, testing = false, queued = false, apActive = false, hasCredentials = false;
uint32_t started = 0, queuedAt = 0;
constexpr uint32_t CONNECT_TIMEOUT = 20000, RETRY_INTERVAL = 30000;

String jsonString(const String& value) {
  String result = "\"";
  for (size_t i = 0; i < value.length(); ++i) {
    const unsigned char c = value[i];
    if (c == '"' || c == '\\') { result += '\\'; result += char(c); }
    else if (c < 32) { char escaped[7]; snprintf(escaped, sizeof(escaped), "\\u%04x", c); result += escaped; }
    else result += char(c);
  }
  return result + '"';
}

bool startRecovery() {
  if (apActive) return true;
  WiFi.mode(WIFI_AP_STA);
  apActive = WiFi.softAP(apName.c_str(), adminPassword.c_str());
  if (apActive) {
    Serial.println("備援熱點：" + apName + "，網址：http://192.168.4.1/");
  }
  return apActive;
}

void connectTo(const Credentials& credentials) {
  WiFi.disconnect();
  WiFi.begin(credentials.ssid, credentials.password);
  started = millis();
  connecting = true;
}

bool validCredentials(const String& ssid, const String& password) {
  if (ssid.isEmpty() || ssid.length() > 32) return false;
  for (size_t i = 0; i < ssid.length(); ++i) if (ssid[i] == 0) return false;
  for (size_t i = 0; i < password.length(); ++i) if (password[i] == 0) return false;
  if (password.length() == 64) {
    for (size_t i = 0; i < 64; ++i) if (!isxdigit(static_cast<unsigned char>(password[i]))) return false;
    return true;
  }
  return password.isEmpty() || (password.length() >= 8 && password.length() <= 63);
}

bool validLocation(double latitude, double longitude) {
  return isfinite(latitude) && isfinite(longitude) && latitude >= -90 && latitude <= 90 && longitude >= -180 && longitude <= 180;
}

bool parseCoordinate(const String& value, double& result) {
  if (value.isEmpty()) return false;
  char* end = nullptr;
  result = strtod(value.c_str(), &end);
  return end == value.c_str() + value.length() && end != value.c_str() && isfinite(result);
}
}

void setupWiFiManager(WebServer& server) {
  storageReady = preferences.begin("monitor-wifi", false);
  if (storageReady && preferences.getBytesLength("weather") == sizeof(WeatherLocation)) {
    WeatherLocation stored = {};
    if (preferences.getBytes("weather", &stored, sizeof(stored)) == sizeof(stored) &&
        validLocation(stored.latitude, stored.longitude) && stored.name[0] && stored.name[96] == 0) weatherLocation = stored;
  }
  if (storageReady && preferences.getBytesLength("credentials") == sizeof(saved)) {
    preferences.getBytes("credentials", &saved, sizeof(saved));
    saved.ssid[32] = 0;
    saved.password[64] = 0;
  }
  hasCredentials = validCredentials(saved.ssid, saved.password);
  adminPassword = storageReady ? preferences.getString("admin", "") : "";
  if (adminPassword.length() < 12) {
    char secret[17];
    snprintf(secret, sizeof(secret), "%08lx%08lx", (unsigned long)esp_random(), (unsigned long)esp_random());
    adminPassword = secret;
    if (storageReady && preferences.putString("admin", adminPassword) == 0) storageReady = false;
  }
  char suffix[13];
  snprintf(suffix, sizeof(suffix), "%012llX", (unsigned long long)ESP.getEfuseMac());
  apName = String("ESP32-Setup-") + suffix;
  Serial.println("Wi-Fi 管理／備援熱點密碼：" + adminPassword);
  Serial.println("備援熱點名稱：" + apName);
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(false);
  if (hasCredentials) {
    connectTo(saved);
  } else {
    message = "尚未設定 Wi-Fi，請連接備援熱點後於設定頁完成設定";
    startRecovery();
  }

  setupNtfyTest(server, adminPassword);

  server.on("/api/weather-location", HTTP_GET, [&server]() {
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "application/json", "{\"name\":" + jsonString(weatherLocation.name) +
      ",\"latitude\":" + String(weatherLocation.latitude, 6) + ",\"longitude\":" + String(weatherLocation.longitude, 6) + "}");
  });
  server.on("/api/weather-location", HTTP_POST, [&server]() {
    server.sendHeader("Cache-Control", "no-store");
    if (server.arg("admin") != adminPassword) { server.send(403, "text/plain; charset=utf-8", "管理密碼錯誤。"); return; }
    if (!storageReady) { server.send(503, "text/plain; charset=utf-8", "設定儲存區無法使用。"); return; }
    WeatherLocation next = {};
    const String name = server.arg("name");
    bool validName = name.length() > 0 && name.length() <= 96;
    for (size_t i = 0; i < name.length(); ++i) if ((unsigned char)name[i] < 32) validName = false;
    if (!validName || !parseCoordinate(server.arg("latitude"), next.latitude) ||
        !parseCoordinate(server.arg("longitude"), next.longitude) || !validLocation(next.latitude, next.longitude)) {
      server.send(400, "text/plain; charset=utf-8", "請輸入地區名稱與有效經緯度（緯度 -90～90，經度 -180～180）。"); return;
    }
    strlcpy(next.name, name.c_str(), sizeof(next.name));
    if (preferences.putBytes("weather", &next, sizeof(next)) != sizeof(next)) {
      server.send(500, "text/plain; charset=utf-8", "儲存失敗，未更改地區。"); return;
    }
    weatherLocation = next;
    server.send(200, "text/plain; charset=utf-8", "天氣地區已儲存到 ESP32，所有裝置共用。");
  });

  server.on("/api/wifi", HTTP_GET, [&server]() {
    String json = "{\"ssid\":" + jsonString(saved.ssid) +
      ",\"connected\":" + String(WiFi.status() == WL_CONNECTED ? "true" : "false") +
      ",\"busy\":" + String(testing || queued ? "true" : "false") +
      ",\"apActive\":" + String(apActive ? "true" : "false") +
      ",\"apName\":" + jsonString(apName) +
      ",\"ip\":" + jsonString(WiFi.localIP().toString()) +
      ",\"message\":" + jsonString(message) + "}";
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "application/json", json);
  });
  server.on("/api/wifi", HTTP_POST, [&server]() {
    server.sendHeader("Cache-Control", "no-store");
    if (server.arg("admin") != adminPassword) {
      server.send(403, "text/plain; charset=utf-8", "管理密碼錯誤，請查看序列監控視窗（115200）。"); return;
    }
    if (!storageReady) { server.send(503, "text/plain; charset=utf-8", "設定儲存區無法使用，未變更網路。"); return; }
    if (testing || queued) { server.send(409, "text/plain; charset=utf-8", "正在測試新網路，請稍候。"); return; }
    const String ssid = server.arg("ssid"), password = server.arg("password");
    if (!server.hasArg("password") || !validCredentials(ssid, password)) {
      server.send(400, "text/plain; charset=utf-8", "SSID 需為 1–32 位元組；密碼需為 8–63 位元組或 64 位十六進位金鑰。開放網路可留空。"); return;
    }
    if (!startRecovery()) { server.send(503, "text/plain; charset=utf-8", "無法啟動備援熱點，未變更網路。"); return; }
    strlcpy(candidate.ssid, ssid.c_str(), sizeof(candidate.ssid));
    strlcpy(candidate.password, password.c_str(), sizeof(candidate.password));
    queued = true;
    queuedAt = millis();
    message = "即將試連新網路；成功才儲存，失敗保留原設定";
    server.send(202, "text/plain; charset=utf-8", "已接受設定，兩秒後試連。若頁面斷線，請連接 " + apName + "，密碼為管理密碼，再開啟 http://192.168.4.1/ 查看結果。");
  });
}

void serviceWiFiManager() {
  const uint32_t now = millis();
  if (queued) {
    if (now - queuedAt >= 2000) { queued = false; testing = true; connectTo(candidate); }
    return;
  }
  if (WiFi.status() == WL_CONNECTED) {
    if (connecting) {
      connecting = false;
      if (testing) {
        testing = false;
        if (preferences.putBytes("credentials", &candidate, sizeof(candidate)) == sizeof(candidate)) {
          saved = candidate;
          hasCredentials = true;
          message = "新網路連線成功，設定已儲存；備援熱點會保留至重新開機";
        } else {
          message = "儲存失敗，正在恢復原網路";
          connectTo(saved);
          return;
        }
        memset(candidate.password, 0, sizeof(candidate.password));
      } else if (!apActive) message = "Wi-Fi 已連線";
      Serial.println("Wi-Fi IP：http://" + WiFi.localIP().toString() + "/");
    }
    return;
  }
  if (connecting && now - started >= CONNECT_TIMEOUT) {
    connecting = false;
    WiFi.disconnect();
    startRecovery();
    if (testing) {
      testing = false;
      memset(candidate.password, 0, sizeof(candidate.password));
      message = "新網路連線失敗，未儲存；正在恢復原網路，可重新設定";
      if (hasCredentials) connectTo(saved);
    } else message = "Wi-Fi 連線失敗，備援熱點可設定；將持續重試已儲存的網路";
  } else if (!connecting && hasCredentials && now - started >= RETRY_INTERVAL) connectTo(saved);
}
