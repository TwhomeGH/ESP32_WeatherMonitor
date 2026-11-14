// 執行：clang++ -std=c++17 tools/test-exchange-rules.cpp -o build/test-exchange-rules.exe
// 然後執行 build/test-exchange-rules.exe；直接測試韌體使用的純計算函式。
#include "../exchange_rules.h"
#include <cassert>
#include <iostream>
#include <limits>

int main() {
  using namespace exchange_rules;
  assert(std::abs(twdPerUnit(0.03125) - 32.0) < 0.00000001);
  assert(twdPerUnit(0) == 0);
  assert(twdPerUnit(-1) == 0);
  assert(twdPerUnit(std::numeric_limits<double>::quiet_NaN()) == 0);
  std::cout << "PASS: inverse quote and invalid values\n";

  assert(reached(32, 32));
  assert(reached(31.99, 32));
  assert(!reached(32.000001, 32));
  assert(!reached(0, 32));
  assert(!reached(32, 0));
  assert(!reached(std::numeric_limits<double>::infinity(), 32));
  std::cout << "PASS: inclusive threshold without display rounding\n";

  assert(!recovered(32, 32, 0));
  assert(recovered(32.01, 32, 0));
  assert(!recovered(32.1, 32, 0.5));
  assert(recovered(32.2, 32, 0.5));
  std::cout << "PASS: rearm only after recovery margin\n";

  assert(displayDecimals(32.9218107) == 2);
  assert(displayDecimals(1) == 2);
  assert(displayDecimals(999.999) == 2);
  assert(displayDecimals(0.9999) == 4);
  assert(displayDecimals(0.2130123) == 4);
  assert(displayDecimals(0.0234567) == 5);
  assert(displayDecimals(0.0001) == 6);
  assert(displayDecimals(0) == 2);
  assert(displayDecimals(-1) == 2);
  assert(displayDecimals(std::numeric_limits<double>::quiet_NaN()) == 2);
  std::cout << "PASS: display decimals keep small currencies readable\n";

  const uint32_t now = 1800000000U;
  assert(fresh(now - 86400, now));
  assert(fresh(now - 172800, now));
  assert(!fresh(now - 172801, now));
  assert(!fresh(now + 1, now));
  assert(!fresh(0, now));
  std::cout << "PASS: stale and future quotes rejected\n";

  auto first = decide(31, 32, 0.5, true, true, now, now, 0, 0);
  assert(first.evaluated && first.notify && !first.armed);
  auto duplicate = decide(31, 32, 0.5, true, false, now, now, now, (now + 28800) / 86400);
  assert(!duplicate.evaluated && !duplicate.notify);
  auto stillLow = decide(31, 32, 0.5, true, false, now + 86400, now + 86400, now, 0);
  assert(stillLow.evaluated && !stillLow.notify && !stillLow.armed);
  auto rise = decide(33, 32, 0.5, true, false, now + 86400, now + 86400, now, 0);
  assert(!rise.notify && rise.armed);
  auto dailyCap = decide(31, 32, 0.5, true, true, now, now, 0, (now + 28800) / 86400);
  assert(!dailyCap.notify);
  auto disabled = decide(31, 32, 0.5, false, true, now, now, 0, 0);
  assert(!disabled.evaluated && !disabled.notify);
  std::cout << "PASS: persistence replay, repeat quote, recovery, daily cap and disabled rules\n";
}
