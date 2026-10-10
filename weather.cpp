#include "weather.h"
#include "ntfy_ca.h"
#include <NetworkClientSecure.h>
#include <HTTPClient.h>

namespace {
// 在字串中找 "key": 之後的數值。
bool numberAfter(const String& s, const char* key, double& out) {
  const int i = s.indexOf(String('"') + key + "\":");
  if (i < 0) return false;
  const char* p = s.c_str() + i + strlen(key) + 3;
  char* end = nullptr;
  out = strtod(p, &end);
  return end != p;
}

// 在字串中找 "key":"value" 的字串值。
bool stringAfter(const String& s, const char* key, String& out) {
  const String pattern = String('"') + key + "\":\"";
  const int i = s.indexOf(pattern);
  if (i < 0) return false;
  const int j = s.indexOf('"', i + pattern.length());
  if (j < 0) return false;
  out = s.substring(i + pattern.length(), j);
  return true;
}
}

String weatherCondition(int code) {
  if (code == 0) return "晴朗";
  if (code >= 1 && code <= 3) { const char* v[] = {"晴時多雲", "局部多雲", "陰天"}; return v[code - 1]; }
  if (code == 45 || code == 48) return "有霧";
  if (code >= 51 && code <= 57) return "毛毛雨";
  if (code >= 61 && code <= 67) return "下雨";
  if (code == 71 || code == 73 || code == 75 || code == 77 || code == 85 || code == 86) return "降雪";
  if (code >= 80 && code <= 82) return "陣雨";
  if (code >= 95) return "雷雨";
  return "天氣狀態未知";
}

String weatherMessage(const WeatherData& data) {
  const char* dirs[] = {"北", "東北", "東", "東南", "南", "西南", "西", "西北"};
  const String dir = dirs[((data.windDirection + 22) / 45) % 8];
  const String cond = weatherCondition(data.code);
  char buf[192];
  snprintf(buf, sizeof(buf), "%s，氣溫 %.1f°C，濕度 %d%%，風 %.1f m/s %s",
    cond.c_str(), data.temperature, data.humidity, data.windSpeed, dir.c_str());
  return String(buf);
}

bool fetchWeather(double latitude, double longitude, WeatherData& out, String& error) {
  NetworkClientSecure client;
  client.setCACert(NTFY_CA);
  client.setHandshakeTimeout(10);
  HTTPClient http;
  http.setConnectTimeout(8000);
  http.setTimeout(8000);
  const String url = "https://api.open-meteo.com/v1/forecast?latitude=" + String(latitude, 6) +
    "&longitude=" + String(longitude, 6) +
    "&current=temperature_2m,relative_humidity_2m,weather_code,wind_speed_10m,wind_direction_10m" +
    "&wind_speed_unit=ms&timezone=Asia%2FTaipei";
  if (!http.begin(client, url)) { error = "無法建立連線"; return false; }
  const int status = http.GET();
  if (status != 200) { http.end(); error = "天氣服務回應 " + String(status); return false; }
  const String body = http.getString();
  http.end();
  const int cut = body.indexOf("\"current\":{");
  if (cut < 0) { error = "天氣資料格式異常"; return false; }
  const String current = body.substring(cut);
  double temperature, humidity, code, windSpeed, windDirection;
  String time;
  if (!numberAfter(current, "temperature_2m", temperature) ||
      !numberAfter(current, "relative_humidity_2m", humidity) ||
      !numberAfter(current, "weather_code", code) ||
      !numberAfter(current, "wind_speed_10m", windSpeed) ||
      !numberAfter(current, "wind_direction_10m", windDirection) ||
      !stringAfter(current, "time", time)) {
    error = "天氣資料不完整"; return false;
  }
  out.temperature = (float)temperature;
  out.humidity = (int)humidity;
  out.code = (int)code;
  out.windSpeed = (float)windSpeed;
  out.windDirection = (int)windDirection;
  out.time = time;
  return true;
}
