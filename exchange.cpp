#include "exchange.h"
#include "exchange_rules.h"
#include "exchange_ca.h"
#include "ntfy_test.h"
#include "auth.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <NetworkClientSecure.h>
#include <Preferences.h>
#include <cJSON.h>
#include <time.h>

namespace {
constexpr size_t MAX_RATES = 200;
constexpr size_t MAX_TIMES = 6;
constexpr size_t MAX_TARGETS = 8;

// 此結構以單一 NVS blob 提交設定與去重狀態；改布局需升版。
struct Config {
  uint32_t version;
  char currency[4];
  bool enabled;
  bool armed;
  double threshold;
  double margin;
  uint16_t minutes[MAX_TIMES];
  uint8_t timeCount;
  uint8_t targetCount;
  uint32_t targets[MAX_TARGETS];
  uint32_t lastEvaluated;
  uint32_t lastNoticeDay;
  uint32_t lastSlot;
};
struct Rate { char code[4]; double price; };
struct Quote {
  Rate rates[MAX_RATES];
  uint16_t count;
  uint32_t sourceTime;
  uint32_t nextUpdate;
  uint32_t fetchedAt;
};
enum class Action { Refresh, Evaluate, Test };
Preferences prefs;
Config config = {};
Quote quote = {};
bool ready = false;
bool busy = false;
bool attempted = false;
uint32_t lastAttemptMs = 0;
uint32_t lastCheckMs = 0;
uint32_t lastManualTestMs = 0;
bool hasManualTest = false;
char statusMessage[384] = "尚未取得參考匯率";
portMUX_TYPE stateLock = portMUX_INITIALIZER_UNLOCKED;

/** 限制下載大小，避免錯誤頁或來源異常耗盡裝置記憶體。 */
class LimitedBody : public Stream {
public:
  String text;
  size_t write(uint8_t value) override {
    if (text.length() >= 16384) return 0;
    return text.concat(static_cast<char>(value)) ? 1 : 0;
  }
  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }
};

bool isBusy() {
  portENTER_CRITICAL(&stateLock);
  const bool sending = busy;
  portEXIT_CRITICAL(&stateLock);
  return sending;
}

bool clockReady() { return time(nullptr) > 1735689600; }

void setStatus(const String& message) {
  portENTER_CRITICAL(&stateLock);
  strlcpy(statusMessage, message.c_str(), sizeof(statusMessage));
  portEXIT_CRITICAL(&stateLock);
}

Config configSnapshot() {
  Config copy;
  portENTER_CRITICAL(&stateLock);
  copy = config;
  portEXIT_CRITICAL(&stateLock);
  return copy;
}

/** 先持久化再更新記憶體，失敗時不可通知，避免重開機後重複觸發。 */
bool save(const Config& next) {
  if (!ready || prefs.putBytes("state-v1", &next, sizeof(next)) != sizeof(next)) return false;
  portENTER_CRITICAL(&stateLock);
  config = next;
  portEXIT_CRITICAL(&stateLock);
  return true;
}

bool validCode(const char* code) {
  if (!code || strlen(code) != 3) return false;
  for (int i = 0; i < 3; ++i) if (code[i] < 'A' || code[i] > 'Z') return false;
  return true;
}

double priceFor(const Quote& data, const char* code) {
  for (size_t i = 0; i < data.count; ++i) if (!strcmp(data.rates[i].code, code)) return data.rates[i].price;
  return 0;
}

/** 顯示用文字；到價比較仍使用原始 double，避免四捨五入改變判斷。 */
String rateText(double value) {
  return String(value, exchange_rules::displayDecimals(value));
}

/** 將固定來源的 JSON 轉為報價；只接受 TWD 基準及可信的每日時間戳。 */
bool decodeQuote(const String& body, Quote& result, String& error) {
  cJSON* json = cJSON_Parse(body.c_str());
  if (!json) { error = "匯率來源不是有效 JSON"; return false; }
  const cJSON* status = cJSON_GetObjectItemCaseSensitive(json, "result");
  const cJSON* base = cJSON_GetObjectItemCaseSensitive(json, "base_code");
  const cJSON* updated = cJSON_GetObjectItemCaseSensitive(json, "time_last_update_unix");
  const cJSON* next = cJSON_GetObjectItemCaseSensitive(json, "time_next_update_unix");
  const cJSON* rates = cJSON_GetObjectItemCaseSensitive(json, "rates");
  const uint32_t now = static_cast<uint32_t>(time(nullptr));
  bool valid = cJSON_IsString(status) && !strcmp(status->valuestring, "success") &&
    cJSON_IsString(base) && !strcmp(base->valuestring, "TWD") && cJSON_IsNumber(updated) &&
    cJSON_IsNumber(next) && cJSON_IsObject(rates) && updated->valuedouble > 1735689600 &&
    updated->valuedouble <= now && next->valuedouble > updated->valuedouble &&
    next->valuedouble <= updated->valuedouble + 172800;
  if (valid) {
    result = {};
    result.sourceTime = static_cast<uint32_t>(updated->valuedouble);
    result.nextUpdate = static_cast<uint32_t>(next->valuedouble);
    result.fetchedAt = now;
    const cJSON* item = nullptr;
    cJSON_ArrayForEach(item, rates) {
      if (!validCode(item->string) || !cJSON_IsNumber(item) || !std::isfinite(item->valuedouble) || item->valuedouble <= 0) {
        valid = false;
        break;
      }
      if (!strcmp(item->string, "TWD")) {
        if (fabs(item->valuedouble - 1) > 0.000001) valid = false;
        continue;
      }
      if (result.count >= MAX_RATES) { valid = false; break; }
      Rate& rate = result.rates[result.count++];
      strlcpy(rate.code, item->string, sizeof(rate.code));
      rate.price = exchange_rules::twdPerUnit(item->valuedouble);
      if (!std::isfinite(rate.price) || rate.price <= 0) valid = false;
    }
    const cJSON* twd = cJSON_GetObjectItemCaseSensitive(rates, "TWD");
    valid = valid && cJSON_IsNumber(twd) && result.count > 0 && exchange_rules::fresh(result.sourceTime, now);
  }
  cJSON_Delete(json);
  if (!valid) error = "來源基準、幣別、數值或時間不正確／已過期；不發送到價通知";
  return valid;
}

/** 使用已驗證的根憑證抓取；快取未到來源更新時間時不重複下載。 */
bool loadQuote(Quote& data, String& error) {
  portENTER_CRITICAL(&stateLock);
  data = quote;
  portEXIT_CRITICAL(&stateLock);
  const uint32_t now = static_cast<uint32_t>(time(nullptr));
  if (data.count && now < data.nextUpdate && exchange_rules::fresh(data.sourceTime, now)) return true;
  // 包含失敗重試的節流；手動操作也不能繞過。
  if (attempted && millis() - lastAttemptMs < 3600000U) {
    error = "來源尚未到下次可抓取時間（一小時內不重複請求）；保留舊資料，不重新判斷";
    return false;
  }
  attempted = true;
  lastAttemptMs = millis();
  NetworkClientSecure client;
  client.setCACert(FX_CA);
  client.setHandshakeTimeout(10);
  HTTPClient http;
  http.setConnectTimeout(8000);
  http.setTimeout(8000);
  if (!http.begin(client, "https://open.er-api.com/v6/latest/TWD")) {
    error = "無法建立匯率 HTTPS 連線";
    return false;
  }
  const int code = http.GET();
  if (code != 200) {
    error = "匯率來源連線失敗，狀態碼 " + String(code);
    http.end();
    return false;
  }
  LimitedBody body;
  const int bytes = http.writeToStream(&body);
  http.end();
  if (bytes <= 0 || !decodeQuote(body.text, data, error)) {
    if (error.isEmpty()) error = "來源回應過大、不完整或下載逾時";
    return false;
  }
  portENTER_CRITICAL(&stateLock);
  const bool backwards = quote.sourceTime && data.sourceTime < quote.sourceTime;
  if (!backwards) quote = data;
  portEXIT_CRITICAL(&stateLock);
  if (backwards) { error = "來源報價時間倒退，保留較新的資料"; return false; }
  return true;
}

String reportTime(uint32_t value) {
  time_t stamp = value;
  tm local;
  localtime_r(&stamp, &local);
  char text[24];
  strftime(text, sizeof(text), "%Y/%m/%d %H:%M", &local);
  return text;
}

/** 回報每一個所選目標的結果；刪除、停用或傳輸失敗都不改寄給其他人。 */
void sendNotice(const Config& rule, double price, uint32_t sourceTime, bool test) {
  const String title = String(test ? "測試匯率通知" : "匯率到價提醒") + " · " + rule.currency + "/TWD";
  const String body = "參考匯率（非銀行成交價）：1 " + String(rule.currency) + " = " + rateText(price) +
    " TWD\n設定門檻：≤ " + rateText(rule.threshold) + " TWD\n來源時間：" + reportTime(sourceTime) +
    "\nRates By Exchange Rate API\nhttps://www.exchangerate-api.com";
  String result = test ? "測試結果：" : "到價通知結果：";
  for (size_t i = 0; i < rule.targetCount; ++i) {
    int code = 0;
    const bool accepted = ntfyPublishTo(rule.targets[i], title, body, "[\"chart_with_downwards_trend\"]", code);
    result += "目標 " + String(rule.targets[i]) + (accepted ? " 已接受；" : " 失敗(") +
      (accepted ? String() : String(code) + ")；");
  }
  setStatus(result + "請確認手機；不自動重送。");
}

void exchangeTask(void* parameter) {
  const Action action = static_cast<Action>(reinterpret_cast<uintptr_t>(parameter));
  Config rule = configSnapshot();
  Quote current;
  String error;
  if (!loadQuote(current, error)) {
    setStatus(error);
  } else {
    const double price = priceFor(current, rule.currency);
    if (!price) {
      setStatus("來源目前未提供選定幣別，請更換設定");
    } else if (action == Action::Test) {
      sendNotice(rule, price, current.sourceTime, true);
    } else if (action == Action::Refresh) {
      setStatus("參考匯率已更新／使用有效快取；手動更新不發送到價通知");
    } else if (!rule.enabled || current.sourceTime <= rule.lastEvaluated) {
      setStatus("來源報價沒有更新，不重複判斷到價");
    } else {
      const uint32_t now = static_cast<uint32_t>(time(nullptr));
      const auto decision = exchange_rules::decide(price, rule.threshold, rule.margin, rule.enabled,
        rule.armed, current.sourceTime, now, rule.lastEvaluated, rule.lastNoticeDay);
      const bool trigger = decision.notify;
      if (decision.evaluated) rule.lastEvaluated = current.sourceTime;
      rule.armed = decision.armed;
      if (trigger) rule.lastNoticeDay = (now + 8 * 3600) / 86400;
      // 提交在傳送之前：斷電或部分失敗不造成整批重送。
      if (!save(rule)) setStatus("去重狀態儲存失敗，未發送通知");
      else if (trigger) sendNotice(rule, price, current.sourceTime, false);
      else setStatus(rule.armed ? "已檢查，尚未到價" : "已檢查，等待回升超過回復幅度後才再提醒");
    }
  }
  portENTER_CRITICAL(&stateLock);
  busy = false;
  portEXIT_CRITICAL(&stateLock);
  vTaskDelete(nullptr);
}

bool start(Action action) {
  if (!clockReady() || WiFi.status() != WL_CONNECTED) return false;
  portENTER_CRITICAL(&stateLock);
  const bool available = !busy;
  if (available) busy = true;
  portEXIT_CRITICAL(&stateLock);
  if (!available) return false;
  setStatus("正在取得／檢查參考匯率…");
  if (xTaskCreate(exchangeTask, "exchange", 24576, reinterpret_cast<void*>(static_cast<uintptr_t>(action)), 1, nullptr) == pdPASS) return true;
  portENTER_CRITICAL(&stateLock);
  busy = false;
  portEXIT_CRITICAL(&stateLock);
  setStatus("記憶體不足，無法啟動匯率工作");
  return false;
}

bool number(const String& value, double& out) {
  if (value.isEmpty()) return false;
  char* end;
  out = strtod(value.c_str(), &end);
  return end == value.c_str() + value.length() && std::isfinite(out);
}

bool parseTimes(const String& value, Config& next) {
  next.timeCount = 0;
  int begin = 0;
  while (begin < (int)value.length()) {
    int end = value.indexOf(',', begin);
    if (end < 0) end = value.length();
    const String part = value.substring(begin, end);
    if (part.length() != 5 || part[2] != ':' || next.timeCount >= MAX_TIMES) return false;
    for (int i : {0, 1, 3, 4}) if (part[i] < '0' || part[i] > '9') return false;
    int hour = part.substring(0, 2).toInt(), minute = part.substring(3).toInt();
    if (hour > 23 || minute > 59) return false;
    const uint16_t at = hour * 60 + minute;
    for (size_t i = 0; i < next.timeCount; ++i) if (next.minutes[i] == at) return false;
    next.minutes[next.timeCount++] = at;
    begin = end + 1;
  }
  return next.timeCount > 0 && !value.endsWith(",");
}

bool parseTargets(const String& value, Config& next) {
  next.targetCount = 0;
  if (value.isEmpty()) return true;
  int begin = 0;
  while (begin < (int)value.length()) {
    int end = value.indexOf(',', begin);
    if (end < 0) end = value.length();
    const String part = value.substring(begin, end);
    double id;
    if (!number(part, id) || id < 1 || id > UINT32_MAX || floor(id) != id || next.targetCount >= MAX_TARGETS) return false;
    const uint32_t target = static_cast<uint32_t>(id);
    if (String(target) != part || !ntfyTargetEnabled(target)) return false;
    for (size_t i = 0; i < next.targetCount; ++i) if (next.targets[i] == target) return false;
    next.targets[next.targetCount++] = target;
    begin = end + 1;
  }
  return !value.endsWith(",");
}

String jsonText(const String& value) {
  String out = "\"";
  for (size_t i = 0; i < value.length(); ++i) {
    char c = value[i];
    if (c == '\\' || c == '"') out += '\\';
    out += static_cast<unsigned char>(c) < 32 ? ' ' : c;
  }
  return out + '"';
}

void getStatus(WebServer& server) {
  const Config rule = configSnapshot();
  Quote current;
  char message[sizeof(statusMessage)];
  bool sending;
  portENTER_CRITICAL(&stateLock);
  current = quote;
  sending = busy;
  memcpy(message, statusMessage, sizeof(message));
  portEXIT_CRITICAL(&stateLock);
  const double price = priceFor(current, rule.currency);
  String times;
  for (size_t i = 0; i < rule.timeCount; ++i) {
    char item[8];
    snprintf(item, sizeof(item), "%02u:%02u", rule.minutes[i] / 60, rule.minutes[i] % 60);
    if (i) times += ',';
    times += item;
  }
  String targets = "[";
  for (size_t i = 0; i < rule.targetCount; ++i) {
    if (i) targets += ',';
    targets += String(rule.targets[i]);
  }
  targets += ']';
  String currencies = "[";
  for (size_t i = 0; i < current.count; ++i) {
    if (i) currencies += ',';
    currencies += jsonText(current.rates[i].code);
  }
  currencies += ']';
  bool targetsReady = rule.targetCount > 0;
  for (size_t i = 0; i < rule.targetCount; ++i) if (!ntfyTargetEnabled(rule.targets[i])) targetsReady = false;
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", String("{\"currency\":") + jsonText(rule.currency) +
    ",\"enabled\":" + (rule.enabled ? "true" : "false") + ",\"armed\":" + (rule.armed ? "true" : "false") +
    ",\"threshold\":" + String(rule.threshold, 8) + ",\"margin\":" + String(rule.margin, 4) +
    ",\"times\":" + jsonText(times) + ",\"targets\":" + targets + ",\"currencies\":" + currencies +
    ",\"price\":" + (price ? String(price, 8) : String("null")) +
    ",\"priceText\":" + (price ? jsonText(rateText(price)) : String("null")) +
    ",\"sourceTime\":" + String(current.sourceTime) + ",\"fetchedAt\":" + String(current.fetchedAt) +
    ",\"nextUpdate\":" + String(current.nextUpdate) +
    ",\"fresh\":" + (price && exchange_rules::fresh(current.sourceTime, time(nullptr)) ? "true" : "false") +
    ",\"clockReady\":" + (clockReady() ? "true" : "false") +
    ",\"targetsReady\":" + (targetsReady ? "true" : "false") +
    ",\"busy\":" + (sending ? "true" : "false") + ",\"message\":" + jsonText(message) + "}");
}

void updateSettings(WebServer& server) {
  if (!authIsAuthorized(server)) { server.send(403, "text/plain; charset=utf-8", "請先登入管理設定"); return; }
  if (isBusy()) { server.send(409, "text/plain; charset=utf-8", "匯率工作進行中，請稍後儲存"); return; }
  Config next = configSnapshot();
  const String code = server.arg("currency");
  const bool changed = code != next.currency;
  double threshold, margin;
  if (!validCode(code.c_str()) || code == "TWD" ||
      !number(server.arg("threshold"), threshold) || threshold < 0 || threshold > 1000000 ||
      !number(server.arg("margin"), margin) || margin < 0 || margin > 100 ||
      !parseTimes(server.arg("times"), next) || !parseTargets(server.arg("targets"), next)) {
    server.send(400, "text/plain; charset=utf-8", "請檢查幣別、門檻、回復幅度、每日時間及啟用的通知目標"); return;
  }
  Quote current;
  portENTER_CRITICAL(&stateLock);
  current = quote;
  portEXIT_CRITICAL(&stateLock);
  if ((code != "USD" || current.count) && !priceFor(current, code.c_str())) {
    server.send(400, "text/plain; charset=utf-8", "來源未提供此幣別，請先更新參考匯率"); return;
  }
  next.enabled = server.arg("enabled") == "true";
  // 更換幣別的同一次儲存必須先清空舊門檻、停用提醒，避免沿用美元價格。
  if (changed) {
    next.enabled = false;
    threshold = 0;
    next.armed = true;
    next.lastEvaluated = 0;
  }
  if (next.enabled && (threshold <= 0 || !next.targetCount || !priceFor(current, code.c_str()) ||
      !exchange_rules::fresh(current.sourceTime, time(nullptr)))) {
    server.send(400, "text/plain; charset=utf-8", "啟用前需有有效報價、正數門檻及至少一個啟用的通知目標"); return;
  }
  strlcpy(next.currency, code.c_str(), sizeof(next.currency));
  next.threshold = threshold;
  next.margin = margin;
  if (!save(next)) { server.send(500, "text/plain; charset=utf-8", "儲存失敗，未變更規則"); return; }
  server.send(200, "text/plain; charset=utf-8", changed ? "幣別已更換，門檻已清空且提醒停用；請重新設定價格再啟用" : "匯率設定已儲存，之後依指定時間檢查；儲存不會立即發送通知");
}
}

void setupExchange(WebServer& server) {
  ready = prefs.begin("monitor-fx", false);
  config.version = 1;
  strlcpy(config.currency, "USD", sizeof(config.currency));
  config.armed = true;
  config.margin = 0.5;
  config.timeCount = 1;
  config.minutes[0] = 9 * 60 + 30;
  if (ready && prefs.getBytesLength("state-v1") == sizeof(Config)) {
    Config stored;
    prefs.getBytes("state-v1", &stored, sizeof(stored));
    if (stored.version == 1 && stored.currency[3] == 0 && validCode(stored.currency) &&
        stored.timeCount > 0 && stored.timeCount <= MAX_TIMES && stored.targetCount <= MAX_TARGETS &&
        std::isfinite(stored.threshold) && stored.threshold >= 0 && std::isfinite(stored.margin) && stored.margin >= 0 && stored.margin <= 100) {
      bool validMinutes = true;
      for (size_t i = 0; i < stored.timeCount; ++i) if (stored.minutes[i] >= 1440) validMinutes = false;
      if (validMinutes && strcmp(stored.currency, "TWD")) config = stored;
    }
  }
  server.on("/api/exchange", HTTP_GET, [&server]() { getStatus(server); });
  server.on("/api/exchange", HTTP_POST, [&server]() { updateSettings(server); });
  const auto action = [&server](Action requested) {
    if (!authIsAuthorized(server)) { server.send(403, "text/plain; charset=utf-8", "請先登入管理設定"); return; }
    if (requested == Action::Test && !configSnapshot().targetCount) {
      server.send(400, "text/plain; charset=utf-8", "請先儲存通知目標"); return;
    }
    if (requested == Action::Test && hasManualTest && millis() - lastManualTestMs < 10000) {
      server.send(429, "text/plain; charset=utf-8", "兩次匯率測試至少間隔 10 秒"); return;
    }
    if (!start(requested)) { server.send(409, "text/plain; charset=utf-8", "請確認 Wi-Fi、網路校時及目前沒有匯率工作進行中"); return; }
    if (requested == Action::Test) {
      hasManualTest = true;
      lastManualTestMs = millis();
    }
    server.send(202, "text/plain; charset=utf-8", requested == Action::Test ? "測試已排入，將送至已儲存的所選目標" : "已排入更新；有效快取會直接使用，不發送到價通知");
  };
  server.on("/api/exchange/refresh", HTTP_POST, [action]() { action(Action::Refresh); });
  server.on("/api/exchange/test", HTTP_POST, [action]() { action(Action::Test); });
}

void serviceExchange() {
  if (millis() - lastCheckMs < 10000) return;
  lastCheckMs = millis();
  if (isBusy() || !ready || !clockReady() || WiFi.status() != WL_CONNECTED) return;
  Config rule = configSnapshot();
  const time_t now = time(nullptr);
  tm local;
  localtime_r(&now, &local);
  const uint16_t minute = local.tm_hour * 60 + local.tm_min;
  const uint32_t slot = static_cast<uint32_t>(now) / 60;
  if (rule.enabled && slot > rule.lastSlot) {
    for (size_t i = 0; i < rule.timeCount; ++i) if (rule.minutes[i] == minute) {
      rule.lastSlot = slot;
      if (!save(rule)) { setStatus("排程去重儲存失敗，本次不觸發"); return; }
      start(Action::Evaluate);
      return;
    }
  }
  // 即使提醒關閉，也保持觀察報價更新；抓取失敗最多每小時重試一次。
  uint32_t nextUpdate;
  portENTER_CRITICAL(&stateLock);
  nextUpdate = quote.nextUpdate;
  portEXIT_CRITICAL(&stateLock);
  if (!attempted || (millis() - lastAttemptMs >= 3600000U && (!nextUpdate || now >= nextUpdate))) {
    start(Action::Refresh);
  }
}
