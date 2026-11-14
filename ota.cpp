#include "ota.h"
#include "auth.h"
#include <ArduinoOTA.h>
#include <WiFi.h>

namespace {
constexpr uint16_t OTA_PORT = 3232;
const char* const OTA_HOSTNAME = "esp32-monitor";

bool started = false;

/** 第一次連上網路後啟動；只有成功才標記，失敗會在下次迴圈重試。 */
bool start() {
  const String& password = authAdminPassword();
  if (password.isEmpty()) return false;
  ArduinoOTA.setHostname(OTA_HOSTNAME);
  ArduinoOTA.setPort(OTA_PORT);
  ArduinoOTA.setPassword(password.c_str());
  ArduinoOTA.onStart([]() { Serial.println("OTA 開始：寫入韌體，請勿斷電"); });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("OTA 進度：%u%%\r", total ? progress * 100 / total : 0);
  });
  ArduinoOTA.onEnd([]() { Serial.println("\nOTA 完成，重新開機"); });
  ArduinoOTA.onError([](ota_error_t error) { Serial.printf("OTA 錯誤：%u\n", static_cast<unsigned>(error)); });
  ArduinoOTA.begin();
  return true;
}
}

void serviceOTA() {
  if (!started) {
    if (WiFi.status() != WL_CONNECTED) return;
    started = start();
    if (!started) return;
    Serial.println(String("OTA 已就緒：") + OTA_HOSTNAME + ".local:" + OTA_PORT + "（密碼為裝置管理密碼）");
    return;
  }
  ArduinoOTA.handle();
}
