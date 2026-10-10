
#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <esp_heap_caps.h>
#include <esp_ota_ops.h>

WebServer server(80);

#include "monitor_page.h"
#include "wifi_manager.h"
#include "docs_assets.h"


void handleStatus() {
  uint32_t totalHeap =
    heap_caps_get_total_size(MALLOC_CAP_8BIT);

  uint32_t freeHeap =
    heap_caps_get_free_size(MALLOC_CAP_8BIT);

  uint32_t largest =
    heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);

  uint32_t minHeap =
    heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT);

  const esp_partition_t* runningPartition = esp_ota_get_running_partition();
  const uint32_t appTotal = runningPartition ? runningPartition->size : 0;
  const uint32_t appUsed = ESP.getSketchSize();
  const uint32_t appFree = appTotal > appUsed ? appTotal - appUsed : 0;
  const String ip = WiFi.localIP().toString();

  char json[1024];

  snprintf(json, sizeof(json),
    "{"
    "\"chip\":\"%s\","
    "\"revision\":%u,"
    "\"cores\":%u,"
    "\"cpuMHz\":%u,"
    "\"totalHeap\":%lu,"
    "\"freeHeap\":%lu,"
    "\"largestBlock\":%lu,"
    "\"minHeap\":%lu,"
    "\"flash\":%lu,"
    "\"appTotal\":%lu,"
    "\"appUsed\":%lu,"
    "\"appFree\":%lu,"
    "\"uptime\":%lu,"
    "\"sdk\":\"%s\","
    "\"ip\":\"%s\""
    "}",
    ESP.getChipModel(),
    (unsigned)ESP.getChipRevision(),
    (unsigned)ESP.getChipCores(),
    (unsigned)ESP.getCpuFreqMHz(),
    (unsigned long)totalHeap,
    (unsigned long)freeHeap,
    (unsigned long)largest,
    (unsigned long)minHeap,
    (unsigned long)ESP.getFlashChipSize(),
    (unsigned long)appTotal,
    (unsigned long)appUsed,
    (unsigned long)appFree,
    (unsigned long)(millis() / 1000),
    ESP.getSdkVersion(),
    ip.c_str()
  );

  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", json);
}


// 以 gzip 傳送已壓縮的 PROGMEM 資產（呼叫前可先設定 Cache-Control 等標頭）。
static void sendGzip(const char* type, const unsigned char* data, const size_t length) {
  server.sendHeader("Content-Encoding", "gzip");
  server.setContentLength(length);
  server.send(200, type, "");
  server.sendContent_P(reinterpret_cast<PGM_P>(data), length);
}

void setup() {
    Serial.begin(115200);

    setupWiFiManager(server);

    // WebServer 初始化
    server.on("/", HTTP_GET, []() {
      server.sendHeader("Cache-Control", "no-store");
      server.send_P(200, "text/html; charset=utf-8", PAGE);
    });

    server.on("/settings", HTTP_GET, []() {
      server.sendHeader("Cache-Control", "no-store");
      sendGzip("text/html; charset=utf-8", SETTINGS_PAGE, SETTINGS_PAGE_LEN);
    });

    server.on("/api/status", HTTP_GET, handleStatus);
    server.on("/docs", HTTP_GET, []() {
      server.sendHeader("Cache-Control", "no-store");
      sendGzip("text/html; charset=utf-8", DOC_PAGE, DOC_PAGE_LEN);
    });
    server.on("/docs.rendered", HTTP_GET, []() {
      server.sendHeader("Cache-Control", "no-store");
      sendGzip("text/html; charset=utf-8", DOC_RENDERED, DOC_RENDERED_LEN);
    });
    server.on("/futures.rendered", HTTP_GET, []() {
      server.sendHeader("Cache-Control", "no-store");
      sendGzip("text/html; charset=utf-8", PLAN_RENDERED, PLAN_RENDERED_LEN);
    });
    server.on("/docs.md", HTTP_GET, []() {
      server.sendHeader("Cache-Control", "no-store");
      sendGzip("text/markdown; charset=utf-8", DOC_MARKDOWN, DOC_MARKDOWN_LEN);
    });
    server.on("/futures.md", HTTP_GET, []() {
      server.sendHeader("Cache-Control", "no-store");
      sendGzip("text/markdown; charset=utf-8", PLAN_MARKDOWN, PLAN_MARKDOWN_LEN);
    });
    server.on("/assets/docs.js", HTTP_GET, []() { server.sendHeader("Cache-Control", "no-store"); sendGzip("application/javascript; charset=utf-8", DOC_SCRIPT, DOC_SCRIPT_LEN); });
    server.on("/assets/weather.js", HTTP_GET, []() { server.sendHeader("Cache-Control", "no-store"); sendGzip("application/javascript; charset=utf-8", WEATHER_SCRIPT, WEATHER_SCRIPT_LEN); });
    server.on("/assets/settings.js", HTTP_GET, []() { server.sendHeader("Cache-Control", "no-store"); sendGzip("application/javascript; charset=utf-8", SETTINGS_SCRIPT, SETTINGS_SCRIPT_LEN); });

    server.begin();

    Serial.println("ESP32 Monitor Ready! IP:");
    Serial.println(WiFi.localIP());
}

void loop() {
  serviceWiFiManager();
  server.handleClient();
  delay(10);
}
