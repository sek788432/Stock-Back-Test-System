#include "Bte/Core/Bar.h"

#include <cmath>

namespace bte::core {

std::optional<double> typicalPrice(const Bar &bar) noexcept {
  if (!bar.isValid()) {
    return std::nullopt;
  }
  return (bar.high + bar.low + bar.close) / 3.0;
}

std::optional<double> medianPrice(const Bar &bar) noexcept {
  if (!bar.isValid()) {
    return std::nullopt;
  }
  return (bar.high + bar.low) / 2.0;
}

std::optional<double> trueRange(const Bar &bar,
                                std::optional<double> prevClose) noexcept {
  if (!bar.isValid()) {
    return std::nullopt;
  }
  const double highLow = bar.high - bar.low;
  if (!prevClose.has_value()) {
    return highLow;
  }
  const double prev = *prevClose;
  return std::max(
      {highLow, std::fabs(bar.high - prev), std::fabs(bar.low - prev)});
}

std::optional<double> trueRange(const Bar &bar) noexcept {
  return trueRange(bar, std::nullopt);
}

} // namespace bte::core
