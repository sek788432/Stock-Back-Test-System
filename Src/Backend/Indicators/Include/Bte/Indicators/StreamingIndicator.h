#pragma once

#include "Bte/Core/Bar.h"
#include "Bte/Core/Result.h"

#include <cstdint>
#include <memory>
#include <optional>

namespace bte::indicators {

inline constexpr std::int32_t maximumIndicatorPeriod = 10'000;

enum class IndicatorKind : std::uint8_t {
  sma,
  ema,
  wma,
  rsi,
  macd,
  bollingerBands,
  atr,
  adx,
  stochastic,
  donchian,
  vwap,
  obv,
  roc,
  momentum,
  trueRange,
  barField,
};

enum class IndicatorOutput : std::uint8_t {
  value,
  signal,
  histogram,
  upper,
  middle,
  lower,
  width,
  percentB,
  positiveDirectionalIndex,
  negativeDirectionalIndex,
  percentK,
  percentD,
};

enum class BarField : std::uint8_t { open, high, low, close, volume };

/// A domain is part of an executable value so condition compilation can reject
/// comparisons such as a price against a volume threshold.
enum class NumericDomain : std::uint8_t { price, volume, percent, scalar };

struct IndicatorValue {
  double value = 0.0;
  NumericDomain domain = NumericDomain::price;
};

struct IndicatorDefinition {
  IndicatorKind kind = IndicatorKind::sma;
  std::int32_t period = 14;
  std::int32_t secondaryPeriod = 0;
  std::int32_t signalPeriod = 0;
  IndicatorOutput output = IndicatorOutput::value;
  BarField field = BarField::close;
};

[[nodiscard]] bool isValidBarField(BarField field) noexcept;
[[nodiscard]] IndicatorValue barFieldValue(const core::Bar &bar,
                                           BarField field) noexcept;
[[nodiscard]] NumericDomain
indicatorOutputDomain(const IndicatorDefinition &definition) noexcept;

/// A configured, allocation-free-after-construction technical indicator.
/// `update` consumes one chronological actual bar and returns no value until
/// the chosen formula is sufficiently warmed.
class StreamingIndicator final {
public:
  [[nodiscard]] static core::Result<StreamingIndicator>
  create(const IndicatorDefinition &definition);

  ~StreamingIndicator();
  StreamingIndicator(StreamingIndicator &&) noexcept;
  StreamingIndicator &operator=(StreamingIndicator &&) noexcept;
  StreamingIndicator(const StreamingIndicator &) = delete;
  StreamingIndicator &operator=(const StreamingIndicator &) = delete;

  [[nodiscard]] core::Result<std::optional<IndicatorValue>>
  update(const core::Bar &bar);
  [[nodiscard]] std::optional<IndicatorValue> latest() const noexcept;
  [[nodiscard]] std::int64_t consumedBars() const noexcept;
  [[nodiscard]] const IndicatorDefinition &definition() const noexcept;
  void reset() noexcept;

private:
  struct Impl;

  explicit StreamingIndicator(std::unique_ptr<Impl> impl) noexcept;

  std::unique_ptr<Impl> impl_;
};

} // namespace bte::indicators
