#pragma once
#include <string>
#include <string_view>
#include <vector>
#include <cstdlib>
#include <cstdio>
#include <cmath>
#include <cstdint>

/** 台新官網資料解析；僅讀取文字，不執行下載的 JavaScript。 */
namespace taishin_rates {
struct Rate { std::string code; double spot; double cash; };
struct Result { uint32_t timestamp = 0; std::vector<Rate> rates; };

/** 串流篩選官網文字，只保留時間與第一份桌面報價表，捨棄樣式及重複手機版。 */
class Collector {
public:
  std::string script;
  bool write(char value) {
    if (failed || ++received > 98304) { failed = true; return false; }
    if (value == '\n') return flush();
    if (line.size() >= 4096) { failed = true; return false; }
    line += value;
    return true;
  }
  /** 下載結束後處理末行；缺少完整表格一律拒絕。 */
  bool finish() { return flush() && complete; }
private:
  std::string line;
  size_t received = 0;
  bool table = false;
  bool complete = false;
  bool failed = false;
  bool flush() {
    if (failed) return false;
    if (!complete && line.find(R"(<table class=\"right\">)") != std::string::npos) table = true;
    const bool date = line.find(R"(\u66F4\u65B0\u6642\u9593)") != std::string::npos ||
      line.find("更新時間") != std::string::npos;
    if (date || table) {
      if (script.size() + line.size() + 1 > 24576) { failed = true; return false; }
      script += line;
      script += '\n';
    }
    if (table && line.find(R"(<\/table>)") != std::string::npos) { table = false; complete = true; }
    line.clear();
    return true;
  }
};

/** 將官網 script 中的跳脫字元還原為文字；錯誤跳脫直接拒絕。 */
inline bool unescape(std::string_view input, std::string& output) {
  output.clear();
  output.reserve(input.size());
  for (size_t i = 0; i < input.size(); ++i) {
    unsigned char c = input[i];
    if (c != '\\') { output += c; continue; }
    if (++i == input.size()) return false;
    c = input[i];
    if (c == 'u') {
      if (i + 4 >= input.size()) return false;
      unsigned value = 0;
      for (int j = 0; j < 4; ++j) {
        const char digit = input[++i];
        const int n = digit >= '0' && digit <= '9' ? digit - '0' :
          digit >= 'a' && digit <= 'f' ? digit - 'a' + 10 :
          digit >= 'A' && digit <= 'F' ? digit - 'A' + 10 : -1;
        if (n < 0) return false;
        value = value * 16 + n;
      }
      if (!value || (value >= 0xd800 && value <= 0xdfff)) return false;
      if (value < 0x80) output += static_cast<char>(value);
      else {
        if (value >= 0x800) output += static_cast<char>(0xe0 | (value >> 12));
        output += static_cast<char>((value < 0x800 ? 0xc0 : 0x80) | ((value >> 6) & (value < 0x800 ? 31 : 63)));
        output += static_cast<char>(0x80 | (value & 63));
      }
    } else if (c == 'n' || c == 'r' || c == 't') output += ' ';
    else if (c == '/' || c == '\\' || c == '\'' || c == '"') output += c;
    else return false;
  }
  return true;
}

/** 驗證日期並換算 UTC epoch；offset 為來源時區秒數。 */
inline uint32_t epoch(int y, int m, int d, int h, int minute, int second, int offset = 0) {
  if (y < 2025 || y > 2100 || m < 1 || m > 12 || d < 1 || h < 0 || h > 23 ||
      minute < 0 || minute > 59 || second < 0 || second > 59) return 0;
  const int days[] = {31,28,31,30,31,30,31,31,30,31,30,31};
  if (d > days[m - 1] + (m == 2 && y % 4 == 0 && (y % 100 != 0 || y % 400 == 0))) return 0;
  // Gregorian civil date algorithm; avoids the host/device timezone changing parsing.
  y -= m <= 2;
  const int era = y / 400;
  const unsigned year = y - era * 400;
  const unsigned day = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const int64_t unixDays = era * 146097 + year * 365 + year / 4 - year / 100 + day - 719468;
  const int64_t stamp = unixDays * 86400 + h * 3600 + minute * 60 + second - offset;
  return stamp > 0 && stamp <= UINT32_MAX ? static_cast<uint32_t>(stamp) : 0;
}

/** 取 td 的第一個價格，略過漲跌圖示；無牌價以 0 表示，不猜測價格。 */
inline double price(const std::string& cell) {
  std::string text;
  bool tag = false;
  for (char c : cell) {
    if (c == '<') { tag = true; continue; }
    if (c == '>') { tag = false; continue; }
    if (!tag) text += c;
  }
  const size_t start = text.find_first_not_of(" \t\r\n");
  if (start == std::string::npos) return 0;
  char* end = nullptr;
  const double value = std::strtod(text.c_str() + start, &end);
  if (!std::isfinite(value) || value <= 0 || value > 1000000 ||
      (*end && *end != ' ' && *end != '\r' && *end != '\n')) return 0;
  return value;
}

/** 解析固定四欄順序的桌面表格，以同行幣別連結對應價格，防止錯列。 */
inline bool parse(std::string_view script, Result& result) {
  result = {};
  if (script.size() > 65536) return false;
  std::string html;
  // 只拼接 document.writeln 的字串常值，避免把程式碼混入價格。
  size_t cursor = 0;
  while ((cursor = script.find("document.writeln('", cursor)) != std::string::npos) {
    cursor += 18;
    const size_t end = script.find("');", cursor);
    if (end == std::string::npos) return false;
    std::string line;
    if (!unescape(script.substr(cursor, end - cursor), line)) return false;
    html += line;
    cursor = end + 3;
  }
  const size_t date = html.find("更新時間 : ");
  int y, m, d, h, minute, second;
  if (date == std::string::npos || std::sscanf(html.c_str() + date + std::string("更新時間 : ").size(),
      "%d/%d/%d %d:%d:%d", &y, &m, &d, &h, &minute, &second) != 6) return false;
  result.timestamp = epoch(y, m, d, h, minute, second, 28800);
  if (!result.timestamp) return false;
  cursor = html.find("<table class=\"right\">");
  if (cursor == std::string::npos) return false;
  const size_t tableEnd = html.find("</table>", cursor);
  if (tableEnd == std::string::npos) return false;
  const char* headers[] = {"即期買入", "即期賣出", "現鈔買入", "現鈔賣出"};
  for (const char* header : headers) {
    cursor = html.find(header, cursor);
    if (cursor == std::string::npos || cursor >= tableEnd) return false;
    cursor += std::string(header).size();
  }
  while ((cursor = html.find("<tr>", cursor)) != std::string::npos && cursor < tableEnd) {
    const size_t end = html.find("</tr>", cursor);
    if (end == std::string::npos || end > tableEnd) return false;
    const std::string row = html.substr(cursor, end - cursor);
    cursor = end + 5;
    const size_t codeStart = row.find("queryhistory('");
    if (codeStart == std::string::npos) continue;
    const std::string code = row.substr(codeStart + 14, 3);
    if (row.substr(codeStart + 17, 2) != "')") return false;
    if (code == "XAU") continue;
    for (char c : code) if (c < 'A' || c > 'Z') return false;
    if (code.size() != 3 || code == "TWD") return false;
    for (const auto& existing : result.rates) if (existing.code == code) return false;
    double values[4] = {};
    size_t cell = 0;
    for (int i = 0; i < 4; ++i) {
      cell = row.find("<td", cell);
      if (cell == std::string::npos) return false;
      cell = row.find('>', cell);
      const size_t cellEnd = row.find("</td>", cell);
      if (cell == std::string::npos || cellEnd == std::string::npos) return false;
      values[i] = price(row.substr(cell + 1, cellEnd - cell - 1));
      cell = cellEnd + 5;
    }
    if (!values[1] || (values[0] && values[0] > values[1]) || (values[2] && values[2] > values[3])) return false;
    result.rates.push_back({code, values[1], values[3]});
    if (result.rates.size() > 32) return false;
  }
  for (const auto& rate : result.rates) if (rate.code == "USD") return true;
  return false;
}

/** Retry-After 支援秒數與 HTTP-date，過去日期不增加等待時間。 */
inline uint32_t retryAfter(const std::string& text, uint32_t now) {
  if (text.empty()) return 0;
  char* end = nullptr;
  const double seconds = std::strtod(text.c_str(), &end);
  if (end != text.c_str() && !*end && std::isfinite(seconds) && seconds >= 0)
    return static_cast<uint32_t>(seconds > UINT32_MAX - now ? UINT32_MAX - now : seconds);
  char weekday[4] = {}, month[4] = {}, zone[4] = {};
  int d, y, h, m, s;
  if (std::sscanf(text.c_str(), "%3[^,], %d %3s %d %d:%d:%d %3s", weekday, &d, month, &y, &h, &m, &s, zone) != 8 ||
      std::string(zone) != "GMT") return 0;
  const std::string months = "JanFebMarAprMayJunJulAugSepOctNovDec";
  const size_t index = months.find(month);
  if (index == std::string::npos || index % 3) return 0;
  const uint32_t until = epoch(y, index / 3 + 1, d, h, m, s);
  return until > now ? until - now : 0;
}

/** 成功 15 分鐘，失敗 30/60/120/240 分鐘；伺服器要求可延長上限。 */
inline uint32_t delaySeconds(unsigned failures, uint32_t serverDelay, uint32_t jitter) {
  const uint32_t base = failures == 0 ? 900 : 1800U << (failures > 4 ? 3 : failures - 1);
  const uint32_t delay = base > serverDelay ? base : serverDelay;
  return delay > UINT32_MAX - 60 ? delay : delay + jitter % 61;
}
}
