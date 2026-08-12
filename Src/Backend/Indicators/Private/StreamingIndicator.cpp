#include "Bte/Indicators/StreamingIndicator.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <math.h>
#include <memory>
#include <stdlib.h>
#include <string>
#include <utility>
#include <vector>

namespace bte::indicators {

bool isValidBarField(const BarField field) noexcept {
  return field >= BarField::open && field <= BarField::volume;
}

IndicatorValue barFieldValue(const core::Bar &bar,
                             const BarField field) noexcept {
  switch (field) {
  case BarField::open:
    return IndicatorValue{.value = bar.open, .domain = NumericDomain::price};
  case BarField::high:
    return IndicatorValue{.value = bar.high, .domain = NumericDomain::price};
  case BarField::low:
    return IndicatorValue{.value = bar.low, .domain = NumericDomain::price};
  case BarField::close:
    return IndicatorValue{.value = bar.close, .domain = NumericDomain::price};
  case BarField::volume:
    return IndicatorValue{.value = bar.volume, .domain = NumericDomain::volume};
  }
  return IndicatorValue{.value = 0.0, .domain = NumericDomain::scalar};
}

NumericDomain
indicatorOutputDomain(const IndicatorDefinition &definition) noexcept {
  switch (definition.kind) {
  case IndicatorKind::rsi:
  case IndicatorKind::adx:
  case IndicatorKind::stochastic:
  case IndicatorKind::roc:
    return NumericDomain::percent;
  case IndicatorKind::bollingerBands:
    return definition.output == IndicatorOutput::width ||
                   definition.output == IndicatorOutput::percentB
               ? NumericDomain::scalar
               : NumericDomain::price;
  case IndicatorKind::obv:
    return NumericDomain::volume;
  case IndicatorKind::barField:
    return definition.field == BarField::volume ? NumericDomain::volume
                                                : NumericDomain::price;
  case IndicatorKind::sma:
  case IndicatorKind::ema:
  case IndicatorKind::wma:
  case IndicatorKind::macd:
  case IndicatorKind::atr:
  case IndicatorKind::donchian:
  case IndicatorKind::vwap:
  case IndicatorKind::momentum:
  case IndicatorKind::trueRange:
    return NumericDomain::price;
  }
  return NumericDomain::scalar;
}

namespace {

class FixedRing final {
public:
  explicit FixedRing(const std::size_t capacity) : values_(capacity) {}

  void push(const double value) noexcept {
    if (count_ < values_.size()) {
      values_[(head_ + count_) % values_.size()] = value;
      ++count_;
      return;
    }
    values_[head_] = value;
    head_ = (head_ + 1U) % values_.size();
  }

  [[nodiscard]] double oldest() const noexcept { return values_[head_]; }

  [[nodiscard]] std::size_t size() const noexcept { return count_; }

  [[nodiscard]] std::size_t capacity() const noexcept { return values_.size(); }

  void reset() noexcept {
    head_ = 0;
    count_ = 0;
  }

private:
  std::vector<double> values_;
  std::size_t head_ = 0;
  std::size_t count_ = 0;
};

class MonotonicWindow final {
public:
  MonotonicWindow(const std::size_t period, const bool minimum)
      : values_(period), minimum_(minimum), period_(period) {}

  void push(const std::int64_t index, const double value) noexcept {
    const auto expired = index - static_cast<std::int64_t>(period_);
    while (count_ > 0U && values_[head_].index <= expired) {
      head_ = (head_ + 1U) % values_.size();
      --count_;
    }
    while (count_ > 0U && shouldRemove(values_[backIndex()].value, value)) {
      --count_;
    }
    values_[(head_ + count_) % values_.size()] = {.index = index,
                                                  .value = value};
    ++count_;
  }

  [[nodiscard]] double front() const noexcept { return values_[head_].value; }

  void reset() noexcept {
    head_ = 0;
    count_ = 0;
  }

private:
  struct Entry {
    std::int64_t index = 0;
    double value = 0.0;
  };

  [[nodiscard]] std::size_t backIndex() const noexcept {
    return (head_ + count_ - 1U) % values_.size();
  }

  [[nodiscard]] bool shouldRemove(const double existing,
                                  const double incoming) const noexcept {
    return minimum_ ? existing >= incoming : existing <= incoming;
  }

  std::vector<Entry> values_;
  bool minimum_ = true;
  std::size_t period_ = 1;
  std::size_t head_ = 0;
  std::size_t count_ = 0;
};

class EmaState final {
public:
  explicit EmaState(const std::int32_t period)
      : period_(period), seed_(static_cast<std::size_t>(period)) {}

  [[nodiscard]] std::optional<double> update(const double input) noexcept {
    if (!value_.has_value()) {
      seed_.push(input);
      seedSum_ += input;
      if (seed_.size() != static_cast<std::size_t>(period_)) {
        return std::nullopt;
      }
      value_ = seedSum_ / static_cast<double>(period_);
      return value_;
    }
    const auto alpha = 2.0 / (static_cast<double>(period_) + 1.0);
    value_ = alpha * input + (1.0 - alpha) * *value_;
    return value_;
  }

  void reset() noexcept {
    seed_.reset();
    seedSum_ = 0.0;
    value_.reset();
  }

private:
  std::int32_t period_ = 1;
  FixedRing seed_;
  double seedSum_ = 0.0;
  std::optional<double> value_;
};

class WilderState final {
public:
  explicit WilderState(const std::int32_t period)
      : period_(period), seed_(static_cast<std::size_t>(period)) {}

  [[nodiscard]] std::optional<double> update(const double input) noexcept {
    if (!value_.has_value()) {
      seed_.push(input);
      seedSum_ += input;
      if (seed_.size() != static_cast<std::size_t>(period_)) {
        return std::nullopt;
      }
      value_ = seedSum_ / static_cast<double>(period_);
      return value_;
    }
    value_ = ((*value_ * static_cast<double>(period_ - 1)) + input) /
             static_cast<double>(period_);
    return value_;
  }

  void reset() noexcept {
    seed_.reset();
    seedSum_ = 0.0;
    value_.reset();
  }

private:
  std::int32_t period_ = 1;
  FixedRing seed_;
  double seedSum_ = 0.0;
  std::optional<double> value_;
};

[[nodiscard]] bool isIndicatorKind(const IndicatorKind kind) noexcept {
  return kind >= IndicatorKind::sma && kind <= IndicatorKind::barField;
}

[[nodiscard]] bool isIndicatorOutput(const IndicatorOutput output) noexcept {
  return output >= IndicatorOutput::value &&
         output <= IndicatorOutput::percentD;
}

[[nodiscard]] bool hasValidPeriod(const std::int32_t period) noexcept {
  return period >= 1 && period <= maximumIndicatorPeriod;
}

[[nodiscard]] std::size_t
windowCapacity(const IndicatorDefinition &definition) noexcept {
  const auto period = static_cast<std::size_t>(std::max(definition.period, 1));
  return definition.kind == IndicatorKind::roc ||
                 definition.kind == IndicatorKind::momentum
             ? period + 1U
             : period;
}

[[nodiscard]] std::size_t
secondaryWindowCapacity(const IndicatorDefinition &definition) noexcept {
  if (definition.kind != IndicatorKind::stochastic) {
    return static_cast<std::size_t>(std::max(definition.period, 1));
  }
  return static_cast<std::size_t>(
      definition.signalPeriod == 0 ? 3 : definition.signalPeriod);
}

[[nodiscard]] bool
outputAllowed(const IndicatorDefinition &definition) noexcept {
  switch (definition.kind) {
  case IndicatorKind::macd:
    return definition.output == IndicatorOutput::value ||
           definition.output == IndicatorOutput::signal ||
           definition.output == IndicatorOutput::histogram;
  case IndicatorKind::bollingerBands:
    return definition.output == IndicatorOutput::upper ||
           definition.output == IndicatorOutput::middle ||
           definition.output == IndicatorOutput::lower ||
           definition.output == IndicatorOutput::width ||
           definition.output == IndicatorOutput::percentB;
  case IndicatorKind::adx:
    return definition.output == IndicatorOutput::value ||
           definition.output == IndicatorOutput::positiveDirectionalIndex ||
           definition.output == IndicatorOutput::negativeDirectionalIndex;
  case IndicatorKind::stochastic:
    return definition.output == IndicatorOutput::percentK ||
           definition.output == IndicatorOutput::percentD;
  case IndicatorKind::donchian:
    return definition.output == IndicatorOutput::upper ||
           definition.output == IndicatorOutput::middle ||
           definition.output == IndicatorOutput::lower;
  default:
    return definition.output == IndicatorOutput::value;
  }
}

[[nodiscard]] core::Result<IndicatorDefinition>
validateDefinition(const IndicatorDefinition &definition) {
  if (!isIndicatorKind(definition.kind) ||
      !isIndicatorOutput(definition.output) ||
      !isValidBarField(definition.field)) {
    return core::makeError(
        core::ErrorCode::invalidArgument,
        "indicator definition contains an unknown enum value");
  }
  if (definition.kind != IndicatorKind::barField &&
      !hasValidPeriod(definition.period)) {
    return core::makeError(core::ErrorCode::invalidArgument,
                           "indicator period must be between 1 and 10000");
  }
  if (definition.kind == IndicatorKind::macd &&
      (!hasValidPeriod(definition.secondaryPeriod) ||
       !hasValidPeriod(definition.signalPeriod) ||
       definition.period >= definition.secondaryPeriod)) {
    return core::makeError(core::ErrorCode::invalidArgument,
                           "MACD requires positive fast, slow, and signal "
                           "periods with fast below slow");
  }
  if (definition.kind == IndicatorKind::stochastic &&
      definition.signalPeriod != 0 &&
      !hasValidPeriod(definition.signalPeriod)) {
    return core::makeError(
        core::ErrorCode::invalidArgument,
        "stochastic signal period must be between 1 and 10000");
  }
  if (!outputAllowed(definition)) {
    return core::makeError(core::ErrorCode::invalidArgument,
                           "indicator output is incompatible with its kind");
  }
  return definition;
}

[[nodiscard]] core::Result<core::Bar> validateBar(const core::Bar &bar) {
  if (!bar.isValid() || !std::isfinite(bar.volume)) {
    return core::makeError(core::ErrorCode::invalidArgument,
                           "indicator input must satisfy OHLCV invariants");
  }
  return bar;
}

} // namespace

struct StreamingIndicator::Impl final {
  explicit Impl(const IndicatorDefinition &configuredDefinition)
      : definition(configuredDefinition),
        window(windowCapacity(configuredDefinition)),
        highWindow(
            static_cast<std::size_t>(std::max(configuredDefinition.period, 1)),
            false),
        lowWindow(
            static_cast<std::size_t>(std::max(configuredDefinition.period, 1)),
            true),
        secondaryWindow(secondaryWindowCapacity(configuredDefinition)),
        ema(configuredDefinition.period),
        fastEma(std::max(configuredDefinition.period, 1)),
        slowEma(std::max(configuredDefinition.secondaryPeriod, 1)),
        signalEma(std::max(configuredDefinition.signalPeriod, 1)),
        gainWilder(std::max(configuredDefinition.period, 1)),
        lossWilder(std::max(configuredDefinition.period, 1)),
        atrWilder(std::max(configuredDefinition.period, 1)),
        plusDmWilder(std::max(configuredDefinition.period, 1)),
        minusDmWilder(std::max(configuredDefinition.period, 1)),
        adxWilder(std::max(configuredDefinition.period, 1)) {}

  [[nodiscard]] std::optional<double> update(const core::Bar &bar) noexcept {
    ++consumedBars;
    const auto input = barFieldValue(bar, definition.field).value;
    std::optional<double> next;
    switch (definition.kind) {
    case IndicatorKind::sma:
      next = updateSma(input);
      break;
    case IndicatorKind::ema:
      next = ema.update(input);
      break;
    case IndicatorKind::wma:
      next = updateWma(input);
      break;
    case IndicatorKind::rsi:
      next = updateRsi(input);
      break;
    case IndicatorKind::macd:
      next = updateMacd(input);
      break;
    case IndicatorKind::bollingerBands:
      next = updateBollinger(input);
      break;
    case IndicatorKind::atr:
      next = updateAtr(bar);
      break;
    case IndicatorKind::adx:
      next = updateAdx(bar);
      break;
    case IndicatorKind::stochastic:
      next = updateStochastic(bar);
      break;
    case IndicatorKind::donchian:
      next = updateDonchian(bar);
      break;
    case IndicatorKind::vwap:
      next = updateVwap(bar);
      break;
    case IndicatorKind::obv:
      next = updateObv(bar);
      break;
    case IndicatorKind::roc:
      next = updateRoc(input, true);
      break;
    case IndicatorKind::momentum:
      next = updateRoc(input, false);
      break;
    case IndicatorKind::trueRange:
      next = updateTrueRange(bar);
      break;
    case IndicatorKind::barField:
      next = input;
      break;
    }
    latest = next;
    return next;
  }

  void reset() noexcept {
    consumedBars = 0;
    latest.reset();
    window.reset();
    highWindow.reset();
    lowWindow.reset();
    secondaryWindow.reset();
    ema.reset();
    fastEma.reset();
    slowEma.reset();
    signalEma.reset();
    gainWilder.reset();
    lossWilder.reset();
    atrWilder.reset();
    plusDmWilder.reset();
    minusDmWilder.reset();
    adxWilder.reset();
    sum = 0.0;
    sumSquares = 0.0;
    weightedSum = 0.0;
    secondarySum = 0.0;
    volumeSum = 0.0;
    previousInput.reset();
    previousBar.reset();
    obv = 0.0;
  }

  [[nodiscard]] std::optional<double> updateSma(const double input) noexcept {
    if (window.size() == window.capacity()) {
      sum -= window.oldest();
    }
    window.push(input);
    sum += input;
    if (window.size() != window.capacity()) {
      return std::nullopt;
    }
    return sum / static_cast<double>(definition.period);
  }

  [[nodiscard]] std::optional<double> updateWma(const double input) noexcept {
    const auto wasFull = window.size() == window.capacity();
    const auto previousSum = sum;
    if (wasFull) {
      sum -= window.oldest();
      weightedSum -= previousSum;
      weightedSum += static_cast<double>(definition.period) * input;
    } else {
      weightedSum += static_cast<double>(window.size() + 1U) * input;
    }
    window.push(input);
    sum += input;
    if (!wasFull && window.size() != window.capacity()) {
      return std::nullopt;
    }
    const auto denominator = static_cast<double>(definition.period) *
                             static_cast<double>(definition.period + 1) / 2.0;
    return weightedSum / denominator;
  }

  [[nodiscard]] std::optional<double> updateRsi(const double input) noexcept {
    const auto previous = std::exchange(previousInput, input);
    if (!previous.has_value()) {
      return std::nullopt;
    }
    const auto change = input - *previous;
    const auto averageGain = gainWilder.update(std::max(change, 0.0));
    const auto averageLoss = lossWilder.update(std::max(-change, 0.0));
    if (!averageGain.has_value() || !averageLoss.has_value()) {
      return std::nullopt;
    }
    if (*averageLoss == 0.0) {
      return *averageGain == 0.0 ? 50.0 : 100.0;
    }
    const auto relativeStrength = *averageGain / *averageLoss;
    return 100.0 - 100.0 / (1.0 + relativeStrength);
  }

  [[nodiscard]] std::optional<double> updateMacd(const double input) noexcept {
    const auto fast = fastEma.update(input);
    const auto slow = slowEma.update(input);
    if (!fast.has_value() || !slow.has_value()) {
      return std::nullopt;
    }
    const auto line = *fast - *slow;
    const auto signal = signalEma.update(line);
    switch (definition.output) {
    case IndicatorOutput::value:
      return line;
    case IndicatorOutput::signal:
      return signal;
    case IndicatorOutput::histogram:
      return signal.has_value() ? std::optional<double>{line - *signal}
                                : std::nullopt;
    default:
      return std::nullopt;
    }
  }

  [[nodiscard]] std::optional<double>
  updateBollinger(const double input) noexcept {
    if (window.size() == window.capacity()) {
      const auto removed = window.oldest();
      sum -= removed;
      sumSquares -= removed * removed;
    }
    window.push(input);
    sum += input;
    sumSquares += input * input;
    if (window.size() != window.capacity()) {
      return std::nullopt;
    }
    const auto mean = sum / static_cast<double>(definition.period);
    const auto variance = std::max(
        0.0, sumSquares / static_cast<double>(definition.period) - mean * mean);
    const auto deviation = std::sqrt(variance);
    const auto upper = mean + 2.0 * deviation;
    const auto lower = mean - 2.0 * deviation;
    switch (definition.output) {
    case IndicatorOutput::upper:
      return upper;
    case IndicatorOutput::middle:
      return mean;
    case IndicatorOutput::lower:
      return lower;
    case IndicatorOutput::width:
      return mean == 0.0 ? std::nullopt
                         : std::optional<double>{(upper - lower) / mean};
    case IndicatorOutput::percentB:
      return upper == lower
                 ? std::optional<double>{0.5}
                 : std::optional<double>{(input - lower) / (upper - lower)};
    default:
      return std::nullopt;
    }
  }

  [[nodiscard]] std::optional<double> updateAtr(const core::Bar &bar) noexcept {
    const auto range = trueRangeFor(bar);
    previousBar = bar;
    return atrWilder.update(range);
  }

  [[nodiscard]] std::optional<double> updateAdx(const core::Bar &bar) noexcept {
    const auto previous = std::exchange(previousBar, bar);
    if (!previous.has_value()) {
      return std::nullopt;
    }
    const auto upwardMove = bar.high - previous->high;
    const auto downwardMove = previous->low - bar.low;
    const auto plusDm =
        upwardMove > downwardMove && upwardMove > 0.0 ? upwardMove : 0.0;
    const auto minusDm =
        downwardMove > upwardMove && downwardMove > 0.0 ? downwardMove : 0.0;
    const auto smoothedTr = atrWilder.update(trueRangeFor(bar, previous));
    const auto smoothedPlus = plusDmWilder.update(plusDm);
    const auto smoothedMinus = minusDmWilder.update(minusDm);
    if (!smoothedTr.has_value() || !smoothedPlus.has_value() ||
        !smoothedMinus.has_value() || *smoothedTr == 0.0) {
      return std::nullopt;
    }
    const auto plusDi = 100.0 * *smoothedPlus / *smoothedTr;
    const auto minusDi = 100.0 * *smoothedMinus / *smoothedTr;
    if (definition.output == IndicatorOutput::positiveDirectionalIndex) {
      return plusDi;
    }
    if (definition.output == IndicatorOutput::negativeDirectionalIndex) {
      return minusDi;
    }
    const auto denominator = plusDi + minusDi;
    const auto directionalIndex =
        denominator == 0.0 ? 0.0
                           : 100.0 * std::abs(plusDi - minusDi) / denominator;
    return adxWilder.update(directionalIndex);
  }

  [[nodiscard]] std::optional<double>
  updateStochastic(const core::Bar &bar) noexcept {
    highWindow.push(consumedBars, bar.high);
    lowWindow.push(consumedBars, bar.low);
    if (consumedBars < definition.period) {
      return std::nullopt;
    }
    const auto high = highWindow.front();
    const auto low = lowWindow.front();
    const auto percentK =
        high == low ? 50.0 : 100.0 * (bar.close - low) / (high - low);
    if (secondaryWindow.size() == secondaryWindow.capacity()) {
      secondarySum -= secondaryWindow.oldest();
    }
    secondaryWindow.push(percentK);
    secondarySum += percentK;
    if (definition.output == IndicatorOutput::percentK) {
      return percentK;
    }
    if (secondaryWindow.size() != secondaryWindow.capacity()) {
      return std::nullopt;
    }
    return secondarySum / static_cast<double>(secondaryWindow.capacity());
  }

  [[nodiscard]] std::optional<double>
  updateDonchian(const core::Bar &bar) noexcept {
    highWindow.push(consumedBars, bar.high);
    lowWindow.push(consumedBars, bar.low);
    if (consumedBars < definition.period) {
      return std::nullopt;
    }
    const auto upper = highWindow.front();
    const auto lower = lowWindow.front();
    switch (definition.output) {
    case IndicatorOutput::upper:
      return upper;
    case IndicatorOutput::middle:
      return (upper + lower) / 2.0;
    case IndicatorOutput::lower:
      return lower;
    default:
      return std::nullopt;
    }
  }

  [[nodiscard]] std::optional<double>
  updateVwap(const core::Bar &bar) noexcept {
    const auto price = barFieldValue(bar, definition.field).value;
    const auto contribution = price * bar.volume;
    if (window.size() == window.capacity()) {
      const auto removedPrice = window.oldest();
      const auto removedVolume = secondaryWindow.oldest();
      sum -= removedPrice * removedVolume;
      volumeSum -= removedVolume;
    }
    window.push(price);
    secondaryWindow.push(bar.volume);
    sum += contribution;
    volumeSum += bar.volume;
    if (window.size() != window.capacity() || volumeSum == 0.0) {
      return std::nullopt;
    }
    return sum / volumeSum;
  }

  [[nodiscard]] std::optional<double> updateObv(const core::Bar &bar) noexcept {
    const auto previous = std::exchange(previousInput, bar.close);
    if (!previous.has_value()) {
      return 0.0;
    }
    if (bar.close > *previous) {
      obv += bar.volume;
    } else if (bar.close < *previous) {
      obv -= bar.volume;
    }
    return obv;
  }

  [[nodiscard]] std::optional<double> updateRoc(const double input,
                                                const bool percent) noexcept {
    window.push(input);
    if (window.size() != window.capacity()) {
      return std::nullopt;
    }
    const auto reference = window.oldest();
    if (reference == 0.0 && percent) {
      return std::nullopt;
    }
    return percent
               ? std::optional<double>{100.0 * (input - reference) / reference}
               : std::optional<double>{input - reference};
  }

  [[nodiscard]] std::optional<double>
  updateTrueRange(const core::Bar &bar) noexcept {
    const auto range = trueRangeFor(bar);
    previousBar = bar;
    return range;
  }

  [[nodiscard]] double trueRangeFor(const core::Bar &bar) const noexcept {
    return trueRangeFor(bar, previousBar);
  }

  [[nodiscard]] static double
  trueRangeFor(const core::Bar &bar,
               const std::optional<core::Bar> &previous) noexcept {
    if (!previous.has_value()) {
      return bar.high - bar.low;
    }
    return std::max({bar.high - bar.low, std::abs(bar.high - previous->close),
                     std::abs(bar.low - previous->close)});
  }

  IndicatorDefinition definition;
  FixedRing window;
  MonotonicWindow highWindow;
  MonotonicWindow lowWindow;
  FixedRing secondaryWindow;
  EmaState ema;
  EmaState fastEma;
  EmaState slowEma;
  EmaState signalEma;
  WilderState gainWilder;
  WilderState lossWilder;
  WilderState atrWilder;
  WilderState plusDmWilder;
  WilderState minusDmWilder;
  WilderState adxWilder;
  std::int64_t consumedBars = 0;
  std::optional<double> latest;
  std::optional<double> previousInput;
  std::optional<core::Bar> previousBar;
  double sum = 0.0;
  double sumSquares = 0.0;
  double weightedSum = 0.0;
  double secondarySum = 0.0;
  double volumeSum = 0.0;
  double obv = 0.0;
};

StreamingIndicator::StreamingIndicator(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}

StreamingIndicator::~StreamingIndicator() = default;
StreamingIndicator::StreamingIndicator(StreamingIndicator &&) noexcept =
    default;
StreamingIndicator &
StreamingIndicator::operator=(StreamingIndicator &&) noexcept = default;

core::Result<StreamingIndicator>
StreamingIndicator::create(const IndicatorDefinition &definition) {
  const auto validated = validateDefinition(definition);
  if (!validated.ok()) {
    return validated.error();
  }
  try {
    return StreamingIndicator{std::make_unique<Impl>(validated.value())};
  } catch (const std::exception &error) {
    return core::makeError(core::ErrorCode::internal,
                           std::string{"could not allocate indicator state: "} +
                               error.what());
  }
}

core::Result<std::optional<IndicatorValue>>
StreamingIndicator::update(const core::Bar &bar) {
  const auto validated = validateBar(bar);
  if (!validated.ok()) {
    return validated.error();
  }
  const auto updated = impl_->update(bar);
  if (!updated.has_value()) {
    return std::optional<IndicatorValue>{};
  }
  return std::optional<IndicatorValue>{IndicatorValue{
      .value = *updated,
      .domain = indicatorOutputDomain(impl_->definition),
  }};
}

std::optional<IndicatorValue> StreamingIndicator::latest() const noexcept {
  const auto latest = impl_->latest;
  if (!latest.has_value()) {
    return std::nullopt;
  }
  return IndicatorValue{
      .value = latest.value_or(0.0),
      .domain = indicatorOutputDomain(impl_->definition),
  };
}

std::int64_t StreamingIndicator::consumedBars() const noexcept {
  return impl_->consumedBars;
}

const IndicatorDefinition &StreamingIndicator::definition() const noexcept {
  return impl_->definition;
}

void StreamingIndicator::reset() noexcept { impl_->reset(); }

} // namespace bte::indicators
