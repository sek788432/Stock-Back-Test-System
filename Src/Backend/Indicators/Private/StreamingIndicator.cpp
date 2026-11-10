#include "Bte/Indicators/StreamingIndicator.h"

// IWYU pragma: no_include <math>
// IWYU pragma: no_include <stdlib.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <numeric>
#ifdef __APPLE__
#include <string>
#endif
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
  const auto inputDomain = definition.field == BarField::volume
                               ? NumericDomain::volume
                               : NumericDomain::price;
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
               : inputDomain;
  case IndicatorKind::obv:
    return NumericDomain::volume;
  case IndicatorKind::barField:
  case IndicatorKind::sma:
  case IndicatorKind::ema:
  case IndicatorKind::wma:
  case IndicatorKind::macd:
  case IndicatorKind::vwap:
  case IndicatorKind::momentum:
    return inputDomain;
  case IndicatorKind::atr:
  case IndicatorKind::donchian:
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

// Positive sums keep the binary exponent separate, so products and totals can
// exceed double's range without discarding a small volume's large-price term.
class ScaledPositiveSum final {
public:
  [[nodiscard]] static ScaledPositiveSum value(const double input) noexcept {
    ScaledPositiveSum result;
    result.mantissa_ = std::frexp(input, &result.exponent_);
    return result;
  }

  [[nodiscard]] static ScaledPositiveSum product(const double first,
                                                 const double second) noexcept {
    const auto left = value(first);
    const auto right = value(second);
    auto result = value(left.mantissa_ * right.mantissa_);
    result.exponent_ += left.exponent_ + right.exponent_;
    return result;
  }

  [[nodiscard]] ScaledPositiveSum
  plus(const ScaledPositiveSum other) const noexcept {
    if (empty()) {
      return other;
    }
    if (other.empty()) {
      return *this;
    }
    const auto exponent = std::max(exponent_, other.exponent_);
    auto result =
        value(std::ldexp(mantissa_, exponent_ - exponent) +
              std::ldexp(other.mantissa_, other.exponent_ - exponent));
    result.exponent_ += exponent;
    return result;
  }

  [[nodiscard]] bool empty() const noexcept { return mantissa_ == 0.0; }

  [[nodiscard]] double
  dividedBy(const ScaledPositiveSum divisor) const noexcept {
    return std::ldexp(mantissa_ / divisor.mantissa_,
                      exponent_ - divisor.exponent_);
  }

private:
  double mantissa_ = 0.0;
  int exponent_ = 0;
};

// Two prefix stacks discard expired samples without subtracting nearly equal
// large totals. Each sample transfers once, keeping updates amortized O(1).
class RollingStatistics final {
public:
  struct Statistics {
    std::size_t count = 0;
    double mean = 0.0;
    double origin = 0.0;
    double meanOffset = 0.0;
    double deviation = 0.0;
    double weightedMean = 0.0;
    ScaledPositiveSum volumeTotal;
    ScaledPositiveSum volumePriceTotal;
    double maximumVolumePrice = 0.0;
  };

  RollingStatistics(const std::size_t capacity, const IndicatorKind kind)
      : capacity_(capacity), kind_(kind) {
    if (kind == IndicatorKind::sma || kind == IndicatorKind::wma ||
        kind == IndicatorKind::bollingerBands || kind == IndicatorKind::vwap) {
      incoming_.resize(capacity);
      outgoing_.resize(capacity);
    }
  }

  void push(const double value, const double volume = 0.0) noexcept {
    if (size_ == capacity_) {
      if (outgoingCount_ == 0U) {
        while (incomingCount_ != 0U) {
          --incomingCount_;
          const auto transferredValue = incoming_[incomingCount_].value;
          const auto transferredVolume = incoming_[incomingCount_].volume;
          const auto leaf = single(transferredValue, transferredVolume);
          const auto aggregate =
              outgoingCount_ == 0U
                  ? leaf
                  : combine(leaf, outgoing_[outgoingCount_ - 1U].aggregate);
          outgoing_[outgoingCount_] = {transferredValue, transferredVolume,
                                       aggregate};
          ++outgoingCount_;
        }
      }
      --outgoingCount_;
      --size_;
    }
    const auto leaf = single(value, volume);
    const auto aggregate =
        incomingCount_ == 0U
            ? leaf
            : combine(incoming_[incomingCount_ - 1U].aggregate, leaf);
    incoming_[incomingCount_] = {value, volume, aggregate};
    ++incomingCount_;
    ++size_;
  }

  [[nodiscard]] Statistics latest() const noexcept {
    const auto older = outgoingCount_ == 0U
                           ? Statistics{}
                           : outgoing_[outgoingCount_ - 1U].aggregate;
    const auto newer = incomingCount_ == 0U
                           ? Statistics{}
                           : incoming_[incomingCount_ - 1U].aggregate;
    return combine(older, newer);
  }

  void reset() noexcept {
    incomingCount_ = 0;
    outgoingCount_ = 0;
    size_ = 0;
  }

private:
  struct Entry {
    double value = 0.0;
    double volume = 0.0;
    Statistics aggregate;
  };

  [[nodiscard]] Statistics single(const double value,
                                  const double volume) const noexcept {
    Statistics result{
        .count = 1, .mean = value, .origin = value, .weightedMean = value};
    if (kind_ == IndicatorKind::vwap) {
      result.volumeTotal = ScaledPositiveSum::value(volume);
      result.volumePriceTotal = ScaledPositiveSum::product(value, volume);
      result.maximumVolumePrice = volume == 0.0 ? 0.0 : value;
    }
    return result;
  }

  [[nodiscard]] static double blend(const double older, const double newer,
                                    const double olderWeight,
                                    const double newerWeight) noexcept {
    // Both weights are positive masses of nonempty sample groups.
    const auto total = olderWeight + newerWeight;
    // Interpolate from the larger weight, so a tiny weight on a large value
    // is not lost by rounding its complementary fraction to one.
    return olderWeight >= newerWeight
               ? std::lerp(older, newer, newerWeight / total)
               : std::lerp(newer, older, olderWeight / total);
  }

  [[nodiscard]] Statistics combine(const Statistics &older,
                                   const Statistics &newer) const noexcept {
    if (older.count == 0U) {
      return newer;
    }
    if (newer.count == 0U) {
      return older;
    }
    const auto olderCount = static_cast<double>(older.count);
    const auto newerCount = static_cast<double>(newer.count);
    const auto count = older.count + newer.count;
    Statistics result{
        .count = count,
        .mean = blend(older.mean, newer.mean, olderCount, newerCount),
    };
    switch (kind_) {
    case IndicatorKind::bollingerBands: {
      const auto newerFraction = newerCount / static_cast<double>(count);
      const auto olderFraction = olderCount / static_cast<double>(count);
      const auto shiftedNewerMean =
          (newer.origin - older.origin) + newer.meanOffset;
      const auto difference = shiftedNewerMean - older.meanOffset;
      result.origin = older.origin;
      result.meanOffset =
          std::lerp(older.meanOffset, shiftedNewerMean, newerFraction);
      result.deviation =
          std::hypot(older.deviation * std::sqrt(olderFraction),
                     newer.deviation * std::sqrt(newerFraction),
                     difference * std::sqrt(olderFraction * newerFraction));
      break;
    }
    case IndicatorKind::wma: {
      const auto olderMass = olderCount * (olderCount + 1.0) / 2.0;
      const auto newerMass = newerCount * (newerCount + 1.0) / 2.0;
      const auto offsetMass = olderCount * newerCount;
      const auto shiftedMean =
          blend(newer.weightedMean, newer.mean, newerMass, offsetMass);
      result.weightedMean = blend(older.weightedMean, shiftedMean, olderMass,
                                  newerMass + offsetMass);
      break;
    }
    case IndicatorKind::vwap: {
      result.volumeTotal = older.volumeTotal.plus(newer.volumeTotal);
      result.volumePriceTotal =
          older.volumePriceTotal.plus(newer.volumePriceTotal);
      result.maximumVolumePrice =
          std::max(older.maximumVolumePrice, newer.maximumVolumePrice);
      break;
    }
    default:
      break;
    }
    return result;
  }

  std::size_t capacity_ = 1;
  IndicatorKind kind_ = IndicatorKind::sma;
  std::size_t size_ = 0;
  std::size_t incomingCount_ = 0;
  std::size_t outgoingCount_ = 0;
  std::vector<Entry> incoming_;
  std::vector<Entry> outgoing_;
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
      seedMean_ =
          std::lerp(seedMean_, input, 1.0 / static_cast<double>(seed_.size()));
      if (seed_.size() != static_cast<std::size_t>(period_)) {
        return std::nullopt;
      }
      value_ = seedMean_;
      return value_;
    }
    const auto alpha = 2.0 / (static_cast<double>(period_) + 1.0);
    value_ = std::lerp(*value_, input, alpha);
    return value_;
  }

  void reset() noexcept {
    seed_.reset();
    seedMean_ = 0.0;
    value_.reset();
  }

private:
  std::int32_t period_ = 1;
  FixedRing seed_;
  double seedMean_ = 0.0;
  std::optional<double> value_;
};

class WilderState final {
public:
  explicit WilderState(const std::int32_t period)
      : period_(period), seed_(static_cast<std::size_t>(period)) {}

  [[nodiscard]] std::optional<double> update(const double input) noexcept {
    if (!value_.has_value()) {
      seed_.push(input);
      seedMean_ =
          std::lerp(seedMean_, input, 1.0 / static_cast<double>(seed_.size()));
      if (seed_.size() != static_cast<std::size_t>(period_)) {
        return std::nullopt;
      }
      value_ = seedMean_;
      return value_;
    }
    value_ = std::lerp(*value_, input, 1.0 / static_cast<double>(period_));
    return value_;
  }

  void reset() noexcept {
    seed_.reset();
    seedMean_ = 0.0;
    value_.reset();
  }

private:
  std::int32_t period_ = 1;
  FixedRing seed_;
  double seedMean_ = 0.0;
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
        adxWilder(std::max(configuredDefinition.period, 1)),
        statisticsWindow(static_cast<std::size_t>(configuredDefinition.period),
                         configuredDefinition.kind) {}

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
    statisticsWindow.reset();
    secondarySum = 0.0;
    previousInput.reset();
    previousBar.reset();
    obv = 0.0;
  }

  [[nodiscard]] std::optional<double> updateSma(const double input) noexcept {
    statisticsWindow.push(input);
    const auto statistics = statisticsWindow.latest();
    if (statistics.count != static_cast<std::size_t>(definition.period)) {
      return std::nullopt;
    }
    return statistics.mean;
  }

  [[nodiscard]] std::optional<double> updateWma(const double input) noexcept {
    statisticsWindow.push(input);
    const auto statistics = statisticsWindow.latest();
    if (statistics.count != static_cast<std::size_t>(definition.period)) {
      return std::nullopt;
    }
    return statistics.weightedMean;
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
    if (definition.output == IndicatorOutput::value) {
      return line;
    }
    if (definition.output == IndicatorOutput::signal) {
      return signal;
    }
    return signal.has_value() ? std::optional<double>{line - *signal}
                              : std::nullopt;
  }

  [[nodiscard]] std::optional<double>
  updateBollinger(const double input) noexcept {
    statisticsWindow.push(input);
    const auto statistics = statisticsWindow.latest();
    if (statistics.count != static_cast<std::size_t>(definition.period)) {
      return std::nullopt;
    }
    const auto mean = statistics.mean;
    const auto deviation = statistics.deviation;
    const auto upper = mean + 2.0 * deviation;
    const auto lower = mean - 2.0 * deviation;
    if (definition.output == IndicatorOutput::upper) {
      return upper;
    }
    if (definition.output == IndicatorOutput::middle) {
      return mean;
    }
    if (definition.output == IndicatorOutput::lower) {
      return lower;
    }
    if (definition.output == IndicatorOutput::width) {
      return mean == 0.0 ? std::nullopt
                         : std::optional<double>{4.0 * (deviation / mean)};
    }
    return deviation == 0.0
               ? std::optional<double>{0.5}
               : std::optional<double>{0.5 + ((input - statistics.origin) -
                                              statistics.meanOffset) /
                                                 deviation / 4.0};
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
    const auto plusDi = 100.0 * (*smoothedPlus / *smoothedTr);
    const auto minusDi = 100.0 * (*smoothedMinus / *smoothedTr);
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
        high == low ? 50.0 : 100.0 * ((bar.close - low) / (high - low));
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
    if (definition.output == IndicatorOutput::upper) {
      return upper;
    }
    if (definition.output == IndicatorOutput::middle) {
      return std::midpoint(upper, lower);
    }
    return lower;
  }

  [[nodiscard]] std::optional<double>
  updateVwap(const core::Bar &bar) noexcept {
    const auto price = barFieldValue(bar, definition.field).value;
    statisticsWindow.push(price, bar.volume);
    const auto statistics = statisticsWindow.latest();
    if (statistics.count != static_cast<std::size_t>(definition.period) ||
        statistics.volumeTotal.empty()) {
      return std::nullopt;
    }
    // A positive weighted mean cannot exceed its largest contributing price.
    // Bound final quotient rounding, including a rounded overflow at DBL_MAX.
    return std::min(
        statistics.maximumVolumePrice,
        statistics.volumePriceTotal.dividedBy(statistics.volumeTotal));
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
    return percent ? std::optional<double>{100.0 *
                                           ((input - reference) / reference)}
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
  RollingStatistics statisticsWindow;
  double secondarySum = 0.0;
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
    return core::makeError(core::ErrorCode::internal, error.what());
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
  if (!std::isfinite(*updated)) {
    impl_->latest.reset();
    return core::makeError(core::ErrorCode::invalidArgument,
                           "indicator output must be finite");
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
