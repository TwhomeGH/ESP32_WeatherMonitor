#include "ntfy_test.h"
#include "ntfy_ca.h"
#include "auth.h"
#include <WiFi.h>
#include <NetworkClientSecure.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <esp_system.h>
#include <time.h>

namespace {
// 保留舊單一目標的二進位布局，供升級時移轉 Preferences 設定。
struct Config {
  char topic[65];   // 最多 64 個主題字元，另留字串結尾。
  char token[257];  // 最多 256 個權杖字元；API 不回傳此內容。
};
// 手動測試開始時的設定副本；之後修改目標不會改變已排入的通知。
struct TestRequest {
  Config config;
  char id[17];    // 16 位十六進位識別碼，供手機訊息與 Web 結果對照。
  char name[65];  // 結果顯示使用的目標名稱。
};
Preferences prefs;
constexpr size_t MAX_TARGETS = 8;
struct Target {
  uint32_t id;     // 0 表示空槽；有效 ID 不隨陣列位置改變。
  char name[65];  // 顯示名稱，最多 64 UTF-8 位元組。
  Config config;
  bool enabled;  // 是否接收排程／群發；停用後仍可單獨測試。
};
// 整包寫入 targets-v1；調整布局時須新增版本並處理資料移轉。
struct Settings {
  uint32_t nextId;  // 遞增配置 ID，避免舊網頁誤操作新建目標。
  Target targets[MAX_TARGETS];
};
Settings settings = {1, {}};
portMUX_TYPE settingsLock = portMUX_INITIALIZER_UNLOCKED;
bool publishing = false;
// 在短暫鎖定期間取得一致副本；實際 HTTPS 發送在鎖外進行。
Settings snapshot() {
  Settings copy;
  portENTER_CRITICAL(&settingsLock); copy = settings; portEXIT_CRITICAL(&settingsLock);
  return copy;
}
// 手動測試與排程共用發送閘門，限制同時僅有一個通知發送工作。
bool acquirePublish() {
  portENTER_CRITICAL(&settingsLock);
  bool available = !publishing;
  if (available) publishing = true;
  portEXIT_CRITICAL(&settingsLock);
  return available;
}
void releasePublish() {
  portENTER_CRITICAL(&settingsLock); publishing = false; portEXIT_CRITICAL(&settingsLock);
}
int findTarget(const Settings& data, const String& id) {
  for (size_t i = 0; i < MAX_TARGETS; ++i)
    if (data.targets[i].id && String(data.targets[i].id) == id) return i;
  return -1;
}
TestRequest request = {};
bool ready = false, busy = false, hasAttempt = false;
uint32_t lastAttempt = 0;
char resultMessage[384] = "尚未發送測試通知";
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
  if (authIsAuthorized(server)) return true;
  server.send(403, "text/plain; charset=utf-8", "管理密碼錯誤。");
  return false;
}

String jsonEscape(const String& value) {
  String out = "\"";
  for (size_t i = 0; i < value.length(); ++i) {
    const char c = value[i];
    if (c == '"' || c == '\\') { out += '\\'; out += c; }
    else if ((unsigned char)c < 0x20) out += ' ';
    else out += c;
  }
  return out + '"';
}

int publishWith(const Config& cfg, const String& title, const String& message, const String& tags) {
  int code = -1;
  NetworkClientSecure client;
  client.setCACert(NTFY_CA);
  client.setHandshakeTimeout(10);
  HTTPClient http;
  http.setConnectTimeout(8000);
  http.setTimeout(8000);
  if (http.begin(client, "https://ntfy.sh/")) {
    http.addHeader("Content-Type", "application/json; charset=utf-8");
    if (cfg.token[0]) http.addHeader("Authorization", String("Bearer ") + cfg.token);
    const String body = "{\"topic\":\"" + String(cfg.topic) + "\",\"title\":" + jsonEscape(title) +
      ",\"message\":" + jsonEscape(message) + ",\"tags\":" + tags + "}";
    code = http.POST(body);
    http.end();
  }
  return code;
}

void publishTest(void*) {
  const int code = publishWith(request.config, "ESP32 測試通知",
    "這是 ESP32 Monitor 手動測試，不是正式開收盤提醒。測試識別碼：" + String(request.id), "[\"test_tube\"]");
  const char* message = code >= 200 && code < 300 ? "ntfy 已接受，請確認 Android 是否收到；此狀態不代表手機已顯示。" :
    code == 401 || code == 403 ? "認證或主題權限失敗，請檢查存取權杖與訂閱主題。" :
    code == 429 ? "ntfy 限流，請稍後再手動測試。" :
    code < 0 ? "連線／TLS 失敗或逾時，結果可能不明；請先查看手機再決定是否重送。" :
    "ntfy 未接受通知，請查看 HTTP 狀態碼及設定。";
  memset(request.config.token, 0, sizeof(request.config.token));
  portENTER_CRITICAL(&resultLock);
  resultCode = code;
  snprintf(resultMessage, sizeof(resultMessage), "%s：%s", request.name, message);
  busy = false;
  portEXIT_CRITICAL(&resultLock);
  releasePublish();
  vTaskDelete(nullptr);
}
}

bool ntfyConfigured() {
  const Settings data = snapshot();
  for (const Target& t : data.targets) if (t.id && t.enabled && validTopic(t.config.topic)) return true;
  return false;
}

bool ntfyPublish(const String& title, const String& message, const String& tags, int& httpCode) {
  if (!acquirePublish()) { httpCode = -1000; return false; }
  const Settings data = snapshot();
  int sent = 0, failed = 0, lastError = -1;
  for (const Target& t : data.targets) {
    if (!t.id || !t.enabled) continue;
    const int code = publishWith(t.config, title, message, tags);
    if (code >= 200 && code < 300) ++sent;
    else { ++failed; lastError = code; }
  }
  releasePublish();
  // -1001 表示部分成功，禁止把它當成整批成功或自動整批重試。
  httpCode = failed ? (sent ? -1001 : lastError) : (sent ? 200 : -1);
  return sent && !failed;
}

void setupNtfyTest(WebServer& server) {
  ready = prefs.begin("monitor-ntfy", false);
  if (ready && prefs.isKey("targets-v1")) {
    if (prefs.getBytesLength("targets-v1") == sizeof(settings)) {
      prefs.getBytes("targets-v1", &settings, sizeof(settings));
      for (Target& t : settings.targets) {
        t.name[64] = 0; t.config.topic[64] = 0; t.config.token[256] = 0;
        if (t.id && !validTopic(t.config.topic)) t = {};
      }
    } else ready = false; // 不以舊設定覆蓋未知格式。
  } else if (ready && prefs.getBytesLength("config") == sizeof(Config)) {
    Target& t = settings.targets[0];
    prefs.getBytes("config", &t.config, sizeof(Config));
    t.config.topic[64] = 0; t.config.token[256] = 0;
    if (validTopic(t.config.topic)) {
      t.id = settings.nextId++; t.enabled = true;
      strlcpy(t.name, "原有通知目標", sizeof(t.name));
      ready = prefs.putBytes("targets-v1", &settings, sizeof(settings)) == sizeof(settings);
      if (ready) prefs.remove("config");
    }
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
    const Settings data = snapshot();
    String targets = "[";
    for (const Target& t : data.targets) {
      if (!t.id) continue;
      if (targets.length() > 1) targets += ',';
      targets += "{\"id\":" + String(t.id) + ",\"name\":" + jsonEscape(t.name) +
        ",\"topic\":" + jsonEscape(t.config.topic) + ",\"enabled\":" + (t.enabled ? "true" : "false") +
        ",\"tokenConfigured\":" + (t.config.token[0] ? "true" : "false") + "}";
    }
    targets += ']';
    server.send(200, "application/json", "{\"targets\":" + targets +
      ",\"limit\":8,\"busy\":" + String(sending ? "true" : "false") +
      ",\"clockReady\":" + String(time(nullptr) > 1735689600 ? "true" : "false") +
      ",\"code\":" + String(code) + ",\"message\":" + jsonEscape(message) + "}");
  });
  server.on("/api/ntfy", HTTP_POST, [&server]() {
    if (!authorize(server)) return;
    if (!ready) { server.send(503, "text/plain; charset=utf-8", "設定儲存區無法使用。"); return; }
    Settings next = snapshot();
    const String id = server.arg("id");
    int index = findTarget(next, id);
    const bool deleting = server.arg("action") == "delete";
    if ((id.length() || deleting) && index < 0) { server.send(404, "text/plain; charset=utf-8", "目標不存在，請重新整理。"); return; }
    if (deleting) next.targets[index] = {};
    else {
      const String name = server.arg("name"), topic = server.arg("topic"), token = server.arg("token");
      bool valid = !name.isEmpty() && name.length() <= 64 && token.length() <= 256;
      for (size_t i = 0; i < name.length(); ++i) if ((unsigned char)name[i] < 32) valid = false;
      for (size_t i = 0; i < token.length(); ++i) if ((unsigned char)token[i] < 33 || (unsigned char)token[i] > 126) valid = false;
      if (!valid || !validTopic(topic)) { server.send(400, "text/plain; charset=utf-8", "名稱限 64 UTF-8 位元組；主題限 1–64 個英數字、底線或連字號；請檢查權杖格式。"); return; }
      for (size_t i = 0; i < MAX_TARGETS; ++i)
        if ((int)i != index && next.targets[i].id && topic == next.targets[i].config.topic) {
          server.send(409, "text/plain; charset=utf-8", "此主題已存在；多支手機可訂閱同一主題。"); return;
        }
      if (index < 0) {
        for (size_t i = 0; i < MAX_TARGETS; ++i) if (!next.targets[i].id) { index = i; break; }
        if (index < 0 || !next.nextId) { server.send(409, "text/plain; charset=utf-8", "最多可設定 8 組通知目標。"); return; }
        next.targets[index] = {};
        next.targets[index].id = next.nextId++;
      }
      Target& t = next.targets[index];
      strlcpy(t.name, name.c_str(), sizeof(t.name));
      strlcpy(t.config.topic, topic.c_str(), sizeof(t.config.topic));
      t.enabled = server.arg("enabled") == "true";
      if (server.arg("clearToken") == "true") memset(t.config.token, 0, sizeof(t.config.token));
      else if (!token.isEmpty()) strlcpy(t.config.token, token.c_str(), sizeof(t.config.token));
    }
    if (prefs.putBytes("targets-v1", &next, sizeof(next)) != sizeof(next)) { server.send(500, "text/plain; charset=utf-8", "儲存失敗。"); return; }
    prefs.remove("config"); // 已提交新格式後才清除舊單一目標設定。
    portENTER_CRITICAL(&settingsLock); settings = next; portEXIT_CRITICAL(&settingsLock);
    server.send(200, "text/plain; charset=utf-8", deleting ? "目標已刪除。已開始的通知仍可能送達。" : "目標已儲存，請選取目標進行測試。");
  });
  server.on("/api/ntfy/test", HTTP_POST, [&server]() {
    if (!authorize(server)) return;
    const Settings data = snapshot();
    const int index = findTarget(data, server.arg("id"));
    if (index < 0) { server.send(400, "text/plain; charset=utf-8", "請先儲存 ntfy 主題。"); return; }
    if (WiFi.status() != WL_CONNECTED) { server.send(503, "text/plain; charset=utf-8", "ESP32 尚未連上路由器。"); return; }
    if (time(nullptr) <= 1735689600) { server.send(503, "text/plain; charset=utf-8", "正在同步網路時間，完成後才能驗證 HTTPS 憑證，請稍後再試。"); return; }
    portENTER_CRITICAL(&resultLock);
    const bool sending = busy;
    portEXIT_CRITICAL(&resultLock);
    if (sending || (hasAttempt && millis() - lastAttempt < 10000)) { server.send(429, "text/plain; charset=utf-8", "正在發送或距離上次不足 10 秒，請稍後再試。"); return; }
    if (!acquirePublish()) { server.send(409, "text/plain; charset=utf-8", "其他通知正在發送，請稍後再試。"); return; }
    request.config = data.targets[index].config;
    strlcpy(request.name, data.targets[index].name, sizeof(request.name));
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
      releasePublish();
      server.send(503, "text/plain; charset=utf-8", "記憶體不足，無法啟動通知工作。"); return;
    }
    hasAttempt = true; lastAttempt = millis();
    server.send(202, "text/plain; charset=utf-8", "測試通知已排入發送，請查看下方結果並確認手機。識別碼：" + String(request.id));
  });
}
