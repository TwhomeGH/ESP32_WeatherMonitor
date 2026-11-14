#pragma once
#include <cmath>
#include <cstdint>

namespace exchange_rules {
/** 判斷來源時間是否可信且仍在有效期內；每日資料最多容許 48 小時。 */
inline bool fresh(uint32_t sourceTime, uint32_t now) {
  return sourceTime > 1735689600U && sourceTime <= now && now - sourceTime <= 172800U;
}

/** TWD 基準回傳「每台幣可換外幣」，介面需其倒數「每外幣需要台幣」。 */
inline double twdPerUnit(double foreignPerTwd) {
  return std::isfinite(foreignPerTwd) && foreignPerTwd > 0 ? 1.0 / foreignPerTwd : 0;
}

/** 低於或等於門檻即符合；使用未四捨五入的值，不接受零、NaN 或無窮大。 */
inline bool reached(double price, double threshold) {
  return std::isfinite(price) && std::isfinite(threshold) && price > 0 && threshold > 0 && price <= threshold;
}

/** 超過門檻加回復百分比後才重新武裝，避免門檻附近反覆通知。 */
inline bool recovered(double price, double threshold, double marginPercent) {
  return std::isfinite(price) && std::isfinite(threshold) && std::isfinite(marginPercent) &&
    threshold > 0 && marginPercent >= 0 && price > threshold * (1.0 + marginPercent / 100.0);
}

/**
 * 顯示用的小數位數；到價比較一律使用原值，不經過這裡。
 * 每單位外幣價格 1 以上固定 2 位；1 以下保留 4 位有效數字，
 * 避免日圓、韓元等低價幣別被壓成 0.21、0.02；最多 6 位。
 */
inline int displayDecimals(double value) {
  if (!std::isfinite(value) || value <= 0 || value >= 1) return 2;
  const int decimals = 3 - static_cast<int>(std::floor(std::log10(value)));
  return decimals > 6 ? 6 : decimals;
}

struct Decision {
  bool evaluated;
  bool notify;
  bool armed;
};

/**
 * 純規則判斷；lastEvaluated/lastNoticeDay/armed 由呼叫端持久化。
 * 僅在啟用、有效且比上次更新的報價進行判斷；台灣日界線每天最多通知一次。
 * 回傳是否已判斷、是否應通知及新的武裝狀態；本函式沒有網路或儲存副作用。
 */
inline Decision decide(double price, double threshold, double margin, bool enabled, bool armed,
                       uint32_t sourceTime, uint32_t now, uint32_t lastEvaluated, uint32_t lastNoticeDay) {
  if (!enabled || !fresh(sourceTime, now) || sourceTime <= lastEvaluated ||
      !std::isfinite(price) || price <= 0 || !std::isfinite(threshold) || threshold <= 0) {
    return {false, false, armed};
  }
  const uint32_t day = (now + 8 * 3600) / 86400;
  const bool trigger = armed && lastNoticeDay != day && reached(price, threshold);
  const bool nextArmed = trigger ? false : armed || recovered(price, threshold, margin);
  return {true, trigger, nextArmed};
}
}
