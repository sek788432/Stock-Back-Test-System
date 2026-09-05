#pragma once

// IWYU pragma: no_include <math>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <optional>
#include <string>
#include <type_traits>

namespace bte::core {

using Timestamp = std::chrono::sys_time<std::chrono::milliseconds>;

/// Single OHLCV bar (UTC timestamp at bar close). See
/// Docs/Specs/BackendCore.md §3.
struct Bar {
  Timestamp ts;
  double open = 0.0;
  double high = 0.0;
  double low = 0.0;
  double close = 0.0;
  double volume = 0.0;

  [[nodiscard]] constexpr bool isValid() const noexcept {
    return std::isfinite(open) && std::isfinite(high) && std::isfinite(low) &&
           std::isfinite(close) && std::isfinite(volume) && open > 0.0 &&
           high >= std::max({open, close, low}) && low > 0.0 &&
           low <= std::min({open, close, high}) && volume >= 0.0 && high >= low;
  }
};

static_assert(std::is_trivially_copyable_v<Bar>);
static_assert(sizeof(Timestamp) + 5 * sizeof(double) == sizeof(Bar));

struct SymbolBar {
  std::string symbol;
  Bar bar{};
};

/// OHLC-derived scalars. Each returns `nullopt` when `!bar.isValid()`.
[[nodiscard]] std::optional<double> typicalPrice(const Bar &bar) noexcept;
[[nodiscard]] std::optional<double> medianPrice(const Bar &bar) noexcept;

/// Intrabar range (`high - low`) when `prevClose` is absent; full Wilder true
/// range when given.
[[nodiscard]] std::optional<double>
trueRange(const Bar &bar, std::optional<double> prevClose) noexcept;
[[nodiscard]] std::optional<double> trueRange(const Bar &bar) noexcept;

} // namespace bte::core
