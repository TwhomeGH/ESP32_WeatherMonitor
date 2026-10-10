#include "auth.h"
#include <Preferences.h>
#include <esp_system.h>
#include <time.h>
#include <cstring>

namespace {
constexpr uint32_t TOKEN_TTL = 7UL * 24 * 3600;  // 憑證有效 7 天
constexpr size_t MAX_TOKENS = 4;                 // 最多保留 4 組（多裝置）
constexpr size_t TOKEN_LEN = 32;                 // 十六進位字元數

struct Token { char value[TOKEN_LEN + 1]; uint32_t expiresAt; };
struct TokenStore { uint8_t count; Token items[MAX_TOKENS]; };

Preferences prefs;
String admin;
TokenStore store = {};
bool ready = false;

// NTP 校時完成後才可信；2025-01-01 為門檻。
bool clockReady() { return time(nullptr) > 1735689600; }

void save() { if (ready) prefs.putBytes("tokens", &store, sizeof(store)); }

void prune() {
  if (!clockReady()) return;
  const uint32_t now = (uint32_t)time(nullptr);
  size_t w = 0;
  for (size_t i = 0; i < store.count; ++i) if (store.items[i].expiresAt > now) store.items[w++] = store.items[i];
  if (w != store.count) { store.count = (uint8_t)w; save(); }
}

String randomToken() {
  char out[TOKEN_LEN + 1];
  for (size_t i = 0; i < TOKEN_LEN; i += 8) snprintf(out + i, 9, "%08lx", (unsigned long)esp_random());
  out[TOKEN_LEN] = 0;
  return String(out);
}

bool validToken(const String& value) {
  if (value.length() != TOKEN_LEN) return false;
  const uint32_t now = (uint32_t)time(nullptr);
  for (size_t i = 0; i < store.count; ++i)
    if (value.equals(store.items[i].value) && (!clockReady() || store.items[i].expiresAt > now)) return true;
  return false;
}

String bearer(WebServer& server) {
  const String header = server.header("Authorization");
  return header.startsWith("Bearer ") ? header.substring(7) : String();
}

void issue(const String& value) {
  if (store.count >= MAX_TOKENS) {  // 滿了就汰換最舊的
    for (size_t i = 1; i < MAX_TOKENS; ++i) store.items[i - 1] = store.items[i];
    store.count = MAX_TOKENS - 1;
  }
  strlcpy(store.items[store.count].value, value.c_str(), sizeof(store.items[store.count].value));
  store.items[store.count].expiresAt = (uint32_t)time(nullptr) + TOKEN_TTL;
  store.count++;
  save();
}

void revoke(const String& value) {
  for (size_t i = 0; i < store.count; ++i) {
    if (value.equals(store.items[i].value)) {
      for (size_t j = i + 1; j < store.count; ++j) store.items[j - 1] = store.items[j];
      store.count--;
      save();
      return;
    }
  }
}
}

void setupAuth(const String& adminPassword) {
  admin = adminPassword;
  ready = prefs.begin("monitor-auth", false);
  if (ready && prefs.getBytesLength("tokens") == sizeof(store)) {
    prefs.getBytes("tokens", &store, sizeof(store));
    if (store.count > MAX_TOKENS) store.count = MAX_TOKENS;
  } else {
    store = {};
  }
  prune();
}

bool authIsAuthorized(WebServer& server) {
  if (!admin.isEmpty() && server.hasArg("admin") && server.arg("admin") == admin) return true;
  return validToken(bearer(server));
}

void registerAuthRoutes(WebServer& server) {
  server.on("/api/login", HTTP_POST, [&server]() {
    server.sendHeader("Cache-Control", "no-store");
    if (server.arg("admin") != admin) { server.send(403, "text/plain; charset=utf-8", "管理密碼錯誤。"); return; }
    if (!clockReady()) { server.send(503, "text/plain; charset=utf-8", "裝置尚未完成網路校時，暫以密碼操作；稍後會自動取得憑證。"); return; }
    prune();
    const String token = randomToken();
    issue(token);
    server.send(200, "application/json", "{\"token\":\"" + token + "\",\"expiresIn\":" + String(TOKEN_TTL) + "}");
  });
  server.on("/api/admin", HTTP_GET, [&server]() {
    server.sendHeader("Cache-Control", "no-store");
    if (authIsAuthorized(server)) server.send(200, "text/plain; charset=utf-8", "OK");
    else server.send(403, "text/plain; charset=utf-8", "未授權。");
  });
  server.on("/api/logout", HTTP_POST, [&server]() {
    server.sendHeader("Cache-Control", "no-store");
    revoke(bearer(server));
    server.send(204, "text/plain; charset=utf-8", "");
  });
}
