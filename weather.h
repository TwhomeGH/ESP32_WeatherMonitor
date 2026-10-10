#pragma once
#include <Arduino.h>

struct WeatherData {
  float temperature = 0;
  int humidity = 0;
  int code = -1;
  float windSpeed = 0;
  int windDirection = 0;
  String time;
};

// 以 Open-Meteo 取得指定座標的即時天氣（TLS 憑證鏈終止於 ISRG Root X1，沿用 ntfy 的 CA）。
bool fetchWeather(double latitude, double longitude, WeatherData& out, String& error);

// WMO weather_code 的中文描述（與網頁顯示一致）。
String weatherCondition(int code);

// 組出天氣訊息內文，例如「晴朗，氣溫 25.3°C，濕度 68%，風 2.1 m/s 西南」。
String weatherMessage(const WeatherData& data);
