// clang++ -std=c++17 tools/test-taishin-rates.cpp -o build/test-taishin-rates.exe
#include "../taishin_rates.h"
#include <cassert>
#include <fstream>
#include <iostream>
#include <iterator>

/** 建立小型官網格式樣本，驗證欄位對應，不依賴即時網路。 */
std::string fixture() {
  return "document.writeln('更新時間 : 2026/11/15 09:00:00');"
    "document.writeln('<table class=\"right\"><tr><td>即期買入</td><td>即期賣出</td><td>現鈔買入</td><td>現鈔賣出</td></tr>');"
    "document.writeln('<tr><td>32.1</td><td><i></i>32.5</td><td>32.0</td><td>32.8</td><td>queryhistory(\\'USD\\')</td></tr></table>');";
}

int main(int argc, char** argv) {
  using namespace taishin_rates;
  Result result;
  auto sample = fixture();
  assert(parse(sample, result));
  assert(result.timestamp == epoch(2026, 11, 15, 1, 0, 0));
  assert(result.rates.size() == 1 && result.rates[0].code == "USD");
  assert(result.rates[0].spot == 32.5 && result.rates[0].cash == 32.8);
  auto invalid = sample;
  invalid.replace(invalid.find("32.5"), 4, "NaN");
  assert(!parse(invalid, result));
  invalid = sample;
  invalid.replace(invalid.find("即期賣出"), std::string("即期賣出").size(), "改版");
  assert(!parse(invalid, result));
  assert(!parse(sample.substr(0, sample.size() - 10), result));
  assert(!parse("<html>Access denied</html>", result));
  assert(!epoch(2026, 2, 30, 0, 0, 0));
  std::string decoded;
  assert(unescape("\\u5373\\u671F\\/\\\"", decoded) && decoded == "即期/\"");
  std::cout << "PASS: quote kinds, escapes, malformed response and date validation\n";

  assert(delaySeconds(0, 0, 0) == 900);
  assert(delaySeconds(1, 0, 0) == 1800);
  assert(delaySeconds(2, 0, 0) == 3600);
  assert(delaySeconds(3, 0, 0) == 7200);
  assert(delaySeconds(4, 0, 0) == 14400);
  assert(delaySeconds(1000, 0, 60) == 14460);
  assert(delaySeconds(1, 20000, 60) == 20060);
  const auto now = epoch(2026, 11, 15, 1, 0, 0);
  assert(retryAfter("3600", now) == 3600);
  assert(retryAfter("Sun, 15 Nov 2026 02:00:00 GMT", now) == 3600);
  assert(retryAfter("Sun, 15 Nov 2026 00:00:00 GMT", now) == 0);
  assert(retryAfter("invalid", now) == 0);
  std::cout << "PASS: backoff cap, jitter, reset and both Retry-After formats\n";
  if (argc > 1) {
    std::ifstream file(argv[1]);
    const std::string live((std::istreambuf_iterator<char>(file)), {});
    assert(parse(live, result));
    std::cout << "PASS: downloaded official sample, currencies=" << result.rates.size() << "\n";
    const auto original = result;
    Collector collector;
    for (char c : live) assert(collector.write(c));
    assert(collector.finish());
    assert(parse(collector.script, result));
    assert(result.timestamp == original.timestamp && result.rates.size() == original.rates.size());
    for (size_t i = 0; i < result.rates.size(); ++i) {
      assert(result.rates[i].code == original.rates[i].code);
      assert(result.rates[i].spot == original.rates[i].spot);
      assert(result.rates[i].cash == original.rates[i].cash);
    }
    std::cout << "PASS: streaming capture " << collector.script.size() << " / " << live.size() << " bytes, same rates\n";
  }
  Collector tooLong;
  for (int i = 0; i < 4096; ++i) assert(tooLong.write('x'));
  assert(!tooLong.write('x') && !tooLong.finish());
  Collector truncated;
  for (char c : std::string("document.writeln('<table class=\\\"right\\\">');\n")) assert(truncated.write(c));
  assert(!truncated.finish());
}
