#include "exchange.h"
#include "exchange_rules.h"
#include "exchange_ca.h"
#include "ntfy_test.h"
#include "auth.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <NetworkClientSecure.h>
#include <Preferences.h>
#include "taishin_rates.h"
#include <esp_system.h>
#include <time.h>

namespace {
constexpr size_t MAX_RATES = 32;
constexpr size_t MAX_TARGETS = 8;

// 此結構以單一 NVS blob 提交設定與去重狀態；改布局需升版。
struct Config {
  uint32_t version;
  char currency[4];
  bool enabled;
  bool armed;
  double threshold;
  double margin;
  uint8_t targetCount;
  uint32_t targets[MAX_TARGETS];
  uint32_t lastEvaluated;
  uint32_t lastNoticeDay;
  bool cash;
};
struct Rate { char code[4]; double price; double cash; };
struct Quote {
  Rate rates[MAX_RATES];
  uint16_t count;
  uint32_t sourceTime;
  uint32_t fetchedAt;
};
// 報價快取另存一個版本化結構，避免與設定布局互相影響。
struct StoredQuote {
  uint32_t version;
  uint16_t count;
  uint32_t sourceTime;
  uint32_t fetchedAt;
  Rate rates[MAX_RATES];
};
enum class Action { Refresh, Evaluate, Test };
Preferences prefs;
Config config = {};
Quote quote = {};
bool ready = false;
bool busy = false;
// 開機時從 NVS 還原的報價；成功重新查詢後清除，讓畫面能標示「上次開機資料」。
bool restored = false;
struct RetryState {
  uint32_t failures;
  uint32_t nextAttempt;
  char error[192];
};
RetryState retry = {};

uint32_t lastCheckMs = 0;
uint32_t lastManualTestMs = 0;
bool hasManualTest = false;
char statusMessage[384] = "尚未取得台新牌告匯率";
portMUX_TYPE stateLock = portMUX_INITIALIZER_UNLOCKED;

/** 保存節流期限，重開機與手動更新都不能提早重試。 */
void recordAttempt(bool success, uint32_t serverDelay, const String& error) {
  RetryState next;
  portENTER_CRITICAL(&stateLock);
  next = retry;
  portEXIT_CRITICAL(&stateLock);
  next.failures = success ? 0 : (next.failures < 1000 ? next.failures + 1 : 1000);
  const uint32_t now = static_cast<uint32_t>(time(nullptr));
  const uint32_t delay = taishin_rates::delaySeconds(next.failures, serverDelay, esp_random());
  next.nextAttempt = delay > UINT32_MAX - now ? UINT32_MAX : now + delay;
  strlcpy(next.error, success ? "" : error.c_str(), sizeof(next.error));
  portENTER_CRITICAL(&stateLock);
  retry = next;
  portEXIT_CRITICAL(&stateLock);
  if (prefs.putBytes("retry-v2", &next, sizeof(next)) != sizeof(next)) {
    portENTER_CRITICAL(&stateLock);
    strlcpy(retry.error, "退避狀態儲存失敗，重開機後期限可能遺失", sizeof(retry.error));
    portEXIT_CRITICAL(&stateLock);
  }
}

/** 保留查詢階段，重啟後仍可判斷是連線、下載或解析途中中斷。 */
void recordPhase(const char* phase) {
  prefs.putString("fx-phase", phase);
  Serial.printf("[exchange] %s; heap=%u; largest=%u\n", phase,
    ESP.getFreeHeap(), ESP.getMaxAllocHeap());
}

bool validCode(const char* code);

/** 將最近一次成功報價寫入 NVS；開機後可先顯示，不必等下一次查詢。 */
void saveQuote(const Quote& source) {
  StoredQuote stored = {};
  stored.version = 1;
  stored.count = source.count;
  stored.sourceTime = source.sourceTime;
  stored.fetchedAt = source.fetchedAt;
  memcpy(stored.rates, source.rates, sizeof(Rate) * source.count);
  if (prefs.putBytes("quote-v1", &stored, sizeof(stored)) != sizeof(stored))
    Serial.println("[exchange] 報價快取儲存失敗");
}

/** 開機先載入上次成功報價；仍需通過新鮮度檢查才會觸發通知。 */
void restoreQuote() {
  if (!ready || prefs.getBytesLength("quote-v1") != sizeof(StoredQuote)) return;
  StoredQuote stored;
  if (prefs.getBytes("quote-v1", &stored, sizeof(stored)) != sizeof(stored) ||
      stored.version != 1 || stored.count > MAX_RATES || !stored.sourceTime) return;
  Quote loaded = {};
  for (size_t i = 0; i < stored.count; ++i) {
    const Rate& rate = stored.rates[i];
    if (!validCode(rate.code) || !std::isfinite(rate.price) || rate.price <= 0 ||
        !std::isfinite(rate.cash) || rate.cash < 0) return;
    loaded.rates[loaded.count++] = rate;
  }
  loaded.sourceTime = stored.sourceTime;
  loaded.fetchedAt = stored.fetchedAt;
  portENTER_CRITICAL(&stateLock);
  quote = loaded;
  restored = true;
  portEXIT_CRITICAL(&stateLock);
}

/** 限制下載大小，避免錯誤頁或來源異常耗盡裝置記憶體。 */
class LimitedBody : public Stream {
public:
  taishin_rates::Collector collector;
  size_t write(uint8_t value) override {
    return collector.write(static_cast<char>(value)) ? 1 : 0;
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
  if (!ready || prefs.putBytes("state-v2", &next, sizeof(next)) != sizeof(next)) return false;
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

double priceFor(const Quote& data, const char* code, bool cash = false) {
  for (size_t i = 0; i < data.count; ++i) if (!strcmp(data.rates[i].code, code)) return cash ? data.rates[i].cash : data.rates[i].price;
  return 0;
}

/** 顯示用文字；到價比較仍使用原始 double，避免四捨五入改變判斷。 */
String rateText(double value) {
  return String(value, exchange_rules::displayDecimals(value));
}

/** 邊界值顯示：最多 6 位小數並去掉尾零；比較仍用未四捨五入的原值。 */
String boundaryText(double value) {
  if (!std::isfinite(value) || value <= 0) return "—";
  String text = String(value, 6);
  while (text.indexOf('.') >= 0 && text.endsWith("0")) text.remove(text.length() - 1);
  if (text.endsWith(".")) text.remove(text.length() - 1);
  return text;
}

/** 只解析台新官網報價文字，不執行來源腳本；過期或未來時間拒絕採用。 */
bool decodeQuote(std::string_view body, Quote& result, String& error) {
  taishin_rates::Result parsed;
  const uint32_t now = static_cast<uint32_t>(time(nullptr));
  if (!taishin_rates::parse(body, parsed) ||
      !exchange_rules::fresh(parsed.timestamp, now)) {
    error = "台新報價格式、價格或時間不正確／已過期";
    return false;
  }
  result = {};
  result.sourceTime = parsed.timestamp;
  result.fetchedAt = now;
  for (const auto& item : parsed.rates) {
    Rate& rate = result.rates[result.count++];
    strlcpy(rate.code, item.code.c_str(), sizeof(rate.code));
    rate.price = item.spot;
    rate.cash = item.cash;
  }
  return true;
}

/** 取得有效快取或查詢台新；所有網路失敗共用持久化退避。 */
bool loadQuote(Quote& data, String& error) {
  RetryState waiting;
  portENTER_CRITICAL(&stateLock);
  data = quote;
  waiting = retry;
  portEXIT_CRITICAL(&stateLock);
  const uint32_t now = static_cast<uint32_t>(time(nullptr));
  if (now < waiting.nextAttempt) {
    if (!waiting.failures && data.count && exchange_rules::fresh(data.sourceTime, now)) return true;
    error = "匯率查詢冷卻／退避中，剩餘 " + String(waiting.nextAttempt - now) + " 秒；" + waiting.error;
    return false;
  }
  if (WiFi.status() != WL_CONNECTED) { error = "Wi-Fi 尚未連線"; return false; }
  // 先寫入保守期限：工作途中重開機也不會立即再次連線。
  recordAttempt(false, 0, "上次匯率查詢中斷或尚未完成");
  // 暫時預約不算真正失敗，完成後使用原始次數遞增。
  portENTER_CRITICAL(&stateLock);
  retry.failures = waiting.failures;
  portEXIT_CRITICAL(&stateLock);
  recordPhase("建立 HTTPS 連線");
  NetworkClientSecure client;
  client.setCACert(FX_CA);
  client.setHandshakeTimeout(10);
  HTTPClient http;
  http.setConnectTimeout(8000);
  http.setTimeout(8000);
  uint32_t serverDelay = 0;
  bool success = false;
  if (!http.begin(client, "https://www.taishinbank.com.tw/eServiceA/transactionrate/transactionrateExport.jsp?no=5")) {
    error = "無法建立台新 HTTPS 連線";
  } else {
    const char* headers[] = {"Retry-After"};
    http.collectHeaders(headers, 1);
    recordPhase("TLS 握手／等待 HTTP 回應");
    const int code = http.GET();
    if (code == 429 || code == 503)
      serverDelay = taishin_rates::retryAfter(http.header("Retry-After").c_str(), now);
    if (code != 200) error = "台新來源連線失敗，狀態碼 " + String(code);
    else if (http.getSize() > 98304) error = "台新回應超出大小限制";
    else {
      recordPhase("下載並篩選報價表");
      LimitedBody body;
      const int bytes = http.writeToStream(&body);
      // 先釋放 TLS 連線緩衝，再建立解析文字，降低 ESP32 的記憶體峰值。
      http.end();
      recordPhase("解析報價表");
      success = bytes > 0 && body.collector.finish() && decodeQuote(body.collector.script, data, error);
      if (!success && error.isEmpty()) error = "來源回應過大、不完整或下載逾時";
    }
    http.end();
  }
  if (success) {
    portENTER_CRITICAL(&stateLock);
    const bool backwards = quote.sourceTime && data.sourceTime < quote.sourceTime;
    if (!backwards) quote = data;
    portEXIT_CRITICAL(&stateLock);
    if (backwards) { success = false; error = "來源時間倒退，保留較新的資料"; }
    else {
      saveQuote(data);
      portENTER_CRITICAL(&stateLock);
      restored = false;
      portEXIT_CRITICAL(&stateLock);
    }
  }
  recordAttempt(success, serverDelay, error);
  recordPhase(success ? "查詢完成" : "查詢失敗，等待重試");
  return success;
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
  const String body = String("台新銀行") + (rule.cash ? "現鈔賣出" : "即期賣出") + "（牌告參考，優惠以交易畫面為準）：1 " + String(rule.currency) + " = " + rateText(price) +
    " TWD\n設定門檻：≤ " + rateText(rule.threshold) + " TWD\n來源時間：" + reportTime(sourceTime) +
    "\nhttps://www.taishinbank.com.tw/TSB/personal/deposit/lookup/realtime/index.html";
  String result = test ? "測試結果：" : "到價通知結果：";
  for (size_t i = 0; i < rule.targetCount; ++i) {
    int code = 0;
    const bool accepted = ntfyPublishTo(rule.targets[i], title, body, "[\"chart_with_downwards_trend\"]", code);
    result += "目標 " + String(rule.targets[i]) + (accepted ? " 已接受；" : " 失敗(") +
      (accepted ? String() : String(code) + ")；");
  }
  setStatus(result + "請確認手機；不自動重送。");
}

void exchangeWork(void* parameter) {
  const Action action = static_cast<Action>(reinterpret_cast<uintptr_t>(parameter));
  Config rule = configSnapshot();
  Quote current;
  String error;
  if (!loadQuote(current, error)) {
    setStatus(error);
  } else {
    const double price = priceFor(current, rule.currency, rule.cash);
    if (!price) {
      setStatus("來源目前未提供選定幣別，請更換設定");
    } else if (action == Action::Test) {
      sendNotice(rule, price, current.sourceTime, true);
    } else if (action == Action::Refresh) {
      setStatus("台新牌告匯率已更新／使用有效快取；手動更新不發送到價通知");
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
 }

/** 背景工作必須捕捉配置失敗，不能讓例外終止整台裝置。 */
void exchangeTask(void* parameter) {
  try {
    exchangeWork(parameter);
  } catch (const std::bad_alloc&) {
    recordAttempt(false, 0, "匯率查詢記憶體不足，已停止本次工作");
    setStatus("匯率查詢記憶體不足；保留舊報價並依退避期限重試");
  } catch (...) {
    recordAttempt(false, 0, "匯率查詢發生例外，已停止本次工作");
    setStatus("匯率查詢發生例外，請查看查詢階段");
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
  setStatus("正在取得／檢查台新牌告匯率…");
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
  RetryState waiting;
  bool restoredQuote;
  portENTER_CRITICAL(&stateLock);
  current = quote;
  waiting = retry;
  sending = busy;
  restoredQuote = restored;
  memcpy(message, statusMessage, sizeof(message));
  portEXIT_CRITICAL(&stateLock);
  const double price = priceFor(current, rule.currency, rule.cash);
  const uint32_t now = static_cast<uint32_t>(time(nullptr));
  const bool clock = clockReady();
  const uint32_t today = (now + 8 * 3600) / 86400;
  const double rearm = rule.threshold * (1.0 + rule.margin / 100.0);
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
    ",\"lastPhase\":" + jsonText(prefs.getString("fx-phase", "尚未查詢")) +
    ",\"resetReason\":" + String(static_cast<unsigned>(esp_reset_reason())) +
    ",\"kind\":" + jsonText(rule.cash ? "cash" : "spot") +
    ",\"failures\":" + String(waiting.failures) + ",\"lastError\":" + jsonText(waiting.error) +
    ",\"retryIn\":" + String(waiting.nextAttempt > now ? waiting.nextAttempt - now : 0) +
    ",\"enabled\":" + (rule.enabled ? "true" : "false") + ",\"armed\":" + (rule.armed ? "true" : "false") +
    ",\"threshold\":" + String(rule.threshold, 8) +
    ",\"thresholdText\":" + jsonText(rateText(rule.threshold)) + ",\"margin\":" + String(rule.margin, 4) +
    ",\"rearmPrice\":" + String(rearm, 8) + ",\"rearmText\":" + jsonText(boundaryText(rearm)) +
    ",\"targets\":" + targets + ",\"currencies\":" + currencies +
    ",\"price\":" + (price ? String(price, 8) : String("null")) +
    ",\"priceText\":" + (price ? jsonText(rateText(price)) : String("null")) +
    ",\"sourceTime\":" + String(current.sourceTime) + ",\"fetchedAt\":" + String(current.fetchedAt) +
    ",\"nextUpdate\":" + String(waiting.nextAttempt) +
    ",\"fresh\":" + (price && exchange_rules::fresh(current.sourceTime, now) ? "true" : "false") +
    ",\"restored\":" + (restoredQuote ? "true" : "false") +
    ",\"clockReady\":" + (clock ? "true" : "false") +
    ",\"notifiedToday\":" + (clock && rule.lastNoticeDay == today ? "true" : "false") +
    ",\"evaluated\":" + (price && rule.lastEvaluated >= current.sourceTime ? "true" : "false") +
    ",\"nextCheck\":" + jsonText(waiting.nextAttempt ? reportTime(waiting.nextAttempt) : "等待連線與校時") +
    ",\"targetsReady\":" + (targetsReady ? "true" : "false") +
    ",\"busy\":" + (sending ? "true" : "false") + ",\"message\":" + jsonText(message) + "}");
}

void updateSettings(WebServer& server) {
  if (!authIsAuthorized(server)) { server.send(403, "text/plain; charset=utf-8", "請先登入管理設定"); return; }
  if (isBusy()) { server.send(409, "text/plain; charset=utf-8", "匯率工作進行中，請稍後儲存"); return; }
  Config next = configSnapshot();
  const String code = server.arg("currency");
  const String kind = server.arg("kind");
  if (kind != "spot" && kind != "cash") { server.send(400, "text/plain; charset=utf-8", "請選擇即期或現鈔賣出"); return; }
  const bool changed = code != next.currency || next.cash != (kind == "cash");
  next.cash = kind == "cash";
  double threshold, margin;
  if (!validCode(code.c_str()) || code == "TWD" ||
      !number(server.arg("threshold"), threshold) || threshold < 0 || threshold > 1000000 ||
      !number(server.arg("margin"), margin) || margin < 0 || margin > 100 ||
      !parseTargets(server.arg("targets"), next)) {
    server.send(400, "text/plain; charset=utf-8", "請檢查幣別、門檻、回復幅度、啟用的通知目標"); return;
  }
  Quote current;
  portENTER_CRITICAL(&stateLock);
  current = quote;
  portEXIT_CRITICAL(&stateLock);
  if ((code != "USD" || current.count) && !priceFor(current, code.c_str(), next.cash)) {
    server.send(400, "text/plain; charset=utf-8", "來源未提供此幣別，請先更新台新牌告匯率"); return;
  }
  next.enabled = server.arg("enabled") == "true";
  // 更換幣別的同一次儲存必須先清空舊門檻、停用提醒，避免沿用美元價格。
  if (changed) {
    next.enabled = false;
    threshold = 0;
    next.armed = true;
    next.lastEvaluated = 0;
  }
  // 允許先儲存啟用設定；沒有有效報價時不會判斷，避免使用者被迫分兩次儲存。
  if (next.enabled && (threshold <= 0 || !next.targetCount)) {
    const char* reason = threshold <= 0 ? "請設定正數到價門檻" : "請至少選擇一個啟用的通知目標";
    server.send(400, "text/plain; charset=utf-8", reason); return;
  }
  strlcpy(next.currency, code.c_str(), sizeof(next.currency));
  next.threshold = threshold;
  next.margin = margin;
  if (!save(next)) { server.send(500, "text/plain; charset=utf-8", "儲存失敗，未變更規則"); return; }
  server.send(200, "text/plain; charset=utf-8", changed ? "幣別／價別已更換，門檻已清空且提醒停用；請重新設定價格再啟用" : "匯率設定已儲存，之後取得新報價時檢查；儲存不會立即發送通知");
}
}

void setupExchange(WebServer& server) {
  ready = prefs.begin("monitor-fx", false);
  config.version = 2;
  strlcpy(config.currency, "USD", sizeof(config.currency));
  config.armed = true;
  config.margin = 0.5;
  if (ready && prefs.getBytesLength("state-v2") == sizeof(Config)) {
    Config stored;
    prefs.getBytes("state-v2", &stored, sizeof(stored));
    if (stored.version == 2 && stored.currency[3] == 0 && validCode(stored.currency) &&
        stored.targetCount <= MAX_TARGETS &&
        std::isfinite(stored.threshold) && stored.threshold >= 0 && std::isfinite(stored.margin) && stored.margin >= 0 && stored.margin <= 100) {
      if (strcmp(stored.currency, "TWD")) config = stored;
    }
  }
  if (ready && prefs.getBytesLength("retry-v2") == sizeof(RetryState)) {
    prefs.getBytes("retry-v2", &retry, sizeof(retry));
    retry.error[sizeof(retry.error) - 1] = 0;
    if (retry.failures > 1000) retry = {};
  }
  restoreQuote();
  if (restored) setStatus("顯示上次開機前的報價，等待重新查詢");
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
  uint32_t nextAttempt;
  portENTER_CRITICAL(&stateLock);
  nextAttempt = retry.nextAttempt;
  portEXIT_CRITICAL(&stateLock);
  if (static_cast<uint32_t>(time(nullptr)) >= nextAttempt) start(Action::Evaluate);
}
