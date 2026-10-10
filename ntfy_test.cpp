#include "ntfy_test.h"
#include "ntfy_ca.h"
#include <WiFi.h>
#include <NetworkClientSecure.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <esp_system.h>
#include <time.h>

namespace {
struct Config { char topic[65]; char token[257]; };
struct TestRequest { Config config; char id[17]; };
Preferences prefs;
Config settings = {};
TestRequest request = {};
bool ready = false, busy = false, hasAttempt = false;
uint32_t lastAttempt = 0;
String managementPassword;
char resultMessage[240] = "尚未發送測試通知";
int resultCode = 0;
portMUX_TYPE resultLock = portMUX_INITIALIZER_UNLOCKED;

bool validTopic(const String& value) {
  if (value.isEmpty() || value.length() > 64) return false;
  for (size_t i = 0; i < value.length(); ++i) {
    const char c = value[i];
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_')) return false;
  }
  return true;
}

bool authorize(WebServer& server) {
  server.sendHeader("Cache-Control", "no-store");
  if (server.arg("admin") == managementPassword) return true;
  server.send(403, "text/plain; charset=utf-8", "管理密碼錯誤。");
  return false;
}

void publishTest(void*) {
  int code = -1;
  {
    NetworkClientSecure client;
    client.setCACert(NTFY_CA);
    client.setHandshakeTimeout(10);
    HTTPClient http;
    http.setConnectTimeout(8000);
    http.setTimeout(8000);
    if (http.begin(client, "https://ntfy.sh/")) {
      http.addHeader("Content-Type", "application/json; charset=utf-8");
      if (request.config.token[0]) http.addHeader("Authorization", String("Bearer ") + request.config.token);
      const String body = "{\"topic\":\"" + String(request.config.topic) +
        "\",\"title\":\"ESP32 測試通知\",\"message\":\"這是 ESP32 Monitor 手動測試，不是正式開收盤提醒。測試識別碼：" +
        String(request.id) + "\",\"tags\":[\"test_tube\"]}";
      code = http.POST(body);
      http.end();
    }
  }
  const char* message = code >= 200 && code < 300 ? "ntfy 已接受，請確認 Android 是否收到；此狀態不代表手機已顯示。" :
    code == 401 || code == 403 ? "認證或主題權限失敗，請檢查存取權杖與訂閱主題。" :
    code == 429 ? "ntfy 限流，請稍後再手動測試。" :
    code < 0 ? "連線／TLS 失敗或逾時，結果可能不明；請先查看手機再決定是否重送。" :
    "ntfy 未接受通知，請查看 HTTP 狀態碼及設定。";
  memset(request.config.token, 0, sizeof(request.config.token));
  portENTER_CRITICAL(&resultLock);
  resultCode = code;
  strlcpy(resultMessage, message, sizeof(resultMessage));
  busy = false;
  portEXIT_CRITICAL(&resultLock);
  vTaskDelete(nullptr);
}
}

void setupNtfyTest(WebServer& server, const String& adminPassword) {
  managementPassword = adminPassword;
  ready = prefs.begin("monitor-ntfy", false);
  if (ready && prefs.getBytesLength("config") == sizeof(settings)) {
    prefs.getBytes("config", &settings, sizeof(settings));
    settings.topic[64] = 0;
    settings.token[256] = 0;
    if (!validTopic(settings.topic)) memset(&settings, 0, sizeof(settings));
  }
  configTime(8 * 3600, 0, "pool.ntp.org", "time.google.com");
  server.on("/api/ntfy", HTTP_GET, [&server]() {
    char message[sizeof(resultMessage)];
    bool sending;
    int code;
    portENTER_CRITICAL(&resultLock);
    sending = busy; code = resultCode;
    memcpy(message, resultMessage, sizeof(message));
    portEXIT_CRITICAL(&resultLock);
    server.sendHeader("Cache-Control", "no-store");
    // 固定訊息與 topic 不含 JSON 特殊字元；不回傳權杖與管理密碼。
    server.send(200, "application/json", "{\"topic\":\"" + String(settings.topic) +
      "\",\"tokenConfigured\":" + String(settings.token[0] ? "true" : "false") +
      ",\"busy\":" + String(sending ? "true" : "false") +
      ",\"clockReady\":" + String(time(nullptr) > 1735689600 ? "true" : "false") +
      ",\"code\":" + String(code) + ",\"message\":\"" + String(message) + "\"}");
  });
  server.on("/api/ntfy", HTTP_POST, [&server]() {
    if (!authorize(server)) return;
    if (!ready) { server.send(503, "text/plain; charset=utf-8", "設定儲存區無法使用。"); return; }
    const String topic = server.arg("topic"), token = server.arg("token");
    bool tokenValid = token.length() <= 256;
    for (size_t i = 0; i < token.length(); ++i) if ((unsigned char)token[i] < 33 || (unsigned char)token[i] > 126) tokenValid = false;
    if (!validTopic(topic) || !tokenValid) { server.send(400, "text/plain; charset=utf-8", "主題需為 1–64 個英數字、底線或連字號；權杖格式不正確。"); return; }
    Config next = settings;
    strlcpy(next.topic, topic.c_str(), sizeof(next.topic));
    if (server.arg("clearToken") == "true") memset(next.token, 0, sizeof(next.token));
    else if (!token.isEmpty()) strlcpy(next.token, token.c_str(), sizeof(next.token));
    if (prefs.putBytes("config", &next, sizeof(next)) != sizeof(next)) { server.send(500, "text/plain; charset=utf-8", "儲存失敗。"); return; }
    settings = next;
    server.send(200, "text/plain; charset=utf-8", "設定已儲存。請在 Android ntfy App 訂閱相同主題，再按發送測試通知。");
  });
  server.on("/api/ntfy/test", HTTP_POST, [&server]() {
    if (!authorize(server)) return;
    if (!validTopic(settings.topic)) { server.send(400, "text/plain; charset=utf-8", "請先儲存 ntfy 主題。"); return; }
    if (WiFi.status() != WL_CONNECTED) { server.send(503, "text/plain; charset=utf-8", "ESP32 尚未連上路由器。"); return; }
    if (time(nullptr) <= 1735689600) { server.send(503, "text/plain; charset=utf-8", "正在同步網路時間，完成後才能驗證 HTTPS 憑證，請稍後再試。"); return; }
    portENTER_CRITICAL(&resultLock);
    const bool sending = busy;
    portEXIT_CRITICAL(&resultLock);
    if (sending || (hasAttempt && millis() - lastAttempt < 10000)) { server.send(429, "text/plain; charset=utf-8", "正在發送或距離上次不足 10 秒，請稍後再試。"); return; }
    request.config = settings;
    snprintf(request.id, sizeof(request.id), "%08lx%08lx", (unsigned long)esp_random(), (unsigned long)esp_random());
    portENTER_CRITICAL(&resultLock);
    busy = true; resultCode = 0;
    strlcpy(resultMessage, "發送中…", sizeof(resultMessage));
    portEXIT_CRITICAL(&resultLock);
    if (xTaskCreate(publishTest, "ntfy-test", 12288, nullptr, 1, nullptr) != pdPASS) {
      portENTER_CRITICAL(&resultLock);
      busy = false;
      strlcpy(resultMessage, "記憶體不足，未發送通知。", sizeof(resultMessage));
      portEXIT_CRITICAL(&resultLock);
      memset(request.config.token, 0, sizeof(request.config.token));
      server.send(503, "text/plain; charset=utf-8", "記憶體不足，無法啟動通知工作。"); return;
    }
    hasAttempt = true; lastAttempt = millis();
    server.send(202, "text/plain; charset=utf-8", "測試通知已排入發送，請查看下方結果並確認手機。識別碼：" + String(request.id));
  });
}
