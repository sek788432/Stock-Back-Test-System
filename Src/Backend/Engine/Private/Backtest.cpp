#include "Bte/Engine/Backtest.h"

// IWYU pragma: no_include <math>

#include "Bte/Core/CanonicalJson.h"
#include "Bte/Core/Result.h"
#include "Bte/Core/Time.h"
#include "Bte/Data/ReleaseSnapshot.h"
#include "Bte/Indicators/StreamingIndicator.h"
#include "Bte/Results/ResultStore.h"
#if defined(BTE_ENABLE_TEST_HOOKS)
#include "BacktestTestHooks.h"
#include <atomic>
#endif

#include <algorithm> // IWYU pragma: keep
#include <array>
#include <cmath>
#include <compare>
#include <cstdint>
#include <limits>
#include <memory>
#include <nlohmann/detail/json_ref.hpp>
#include <nlohmann/json.hpp>
#include <nlohmann/json_fwd.hpp>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace bte::engine {
namespace {

#if defined(BTE_ENABLE_TEST_HOOKS)
std::atomic<std::uint64_t> &backtestStartCountStorage() {
  static std::atomic<std::uint64_t> value{0};
  return value;
}
#endif

constexpr auto nanodollarsPerDollar = 1'000'000'000.0;
constexpr auto microsharesPerShare = 1'000'000.0;
constexpr auto int64ExclusiveUpperBound = 9'223'372'036'854'775'808.0;
constexpr auto priceToMoneyDivisor = std::int64_t{1000};
constexpr auto slippageDivisor = std::int64_t{10'000};

core::Error invalidArgument(std::string message) {
  return core::makeError(core::ErrorCode::invalidArgument, std::move(message));
}

core::Error cancelled() {
  return core::makeError(core::ErrorCode::cancelled, "backtest was cancelled");
}

core::Result<std::int64_t> checkedPriceNanodollars(const double price) {
  const auto scaledPrice = price * nanodollarsPerDollar;
  if (!std::isfinite(scaledPrice) || scaledPrice <= 0.0 ||
      scaledPrice >= int64ExclusiveUpperBound) {
    return invalidArgument("bar price is outside the supported range");
  }
  const auto integral = std::floor(scaledPrice);
  const auto fraction = scaledPrice - integral;
  auto rounded = integral;
  if (fraction > 0.5 || (fraction == 0.5 && std::fmod(integral, 2.0) != 0.0)) {
    rounded += 1.0;
  }
  if (rounded < 1.0 || rounded >= int64ExclusiveUpperBound) {
    return invalidArgument("bar price is outside the supported range");
  }
  return static_cast<std::int64_t>(rounded);
}

core::Result<std::int64_t> checkedVolumeMicroshares(const double volume) {
  const auto scaledVolume = volume * microsharesPerShare;
  if (!std::isfinite(scaledVolume) || scaledVolume < 0.0 ||
      scaledVolume >= int64ExclusiveUpperBound) {
    return invalidArgument("bar volume is outside the supported range");
  }
  const auto integral = std::floor(scaledVolume);
  const auto fraction = scaledVolume - integral;
  auto rounded = integral;
  if (fraction > 0.5 || (fraction == 0.5 && std::fmod(integral, 2.0) != 0.0)) {
    rounded += 1.0;
  }
  return static_cast<std::int64_t>(rounded);
}

core::Result<std::vector<data::SnapshotBar>>
normalizeBars(const std::vector<core::Bar> &bars) {
  std::vector<data::SnapshotBar> normalized;
  normalized.reserve(bars.size());
  for (const auto &bar : bars) {
    const auto open = checkedPriceNanodollars(bar.open);
    const auto high = checkedPriceNanodollars(bar.high);
    const auto low = checkedPriceNanodollars(bar.low);
    const auto close = checkedPriceNanodollars(bar.close);
    const auto volume = checkedVolumeMicroshares(bar.volume);
    if (!open.ok() || !high.ok() || !low.ok() || !close.ok() || !volume.ok()) {
      return invalidArgument("Backtest bars cannot be normalized exactly");
    }
    normalized.push_back({.timestamp = bar.ts,
                          .openNanodollars = open.value(),
                          .highNanodollars = high.value(),
                          .lowNanodollars = low.value(),
                          .closeNanodollars = close.value(),
                          .volumeMicroshares = volume.value()});
  }
  return normalized;
}

core::Result<std::int64_t> checkedAdd(const std::int64_t left,
                                      const std::int64_t right) {
  if (right > 0 && left > std::numeric_limits<std::int64_t>::max() - right) {
    return invalidArgument("accounting value exceeds the supported range");
  }
  return left + right;
}

core::Result<std::int64_t> checkedMultiply(const std::int64_t left,
                                           const std::int64_t right) {
  if (left != 0 && right > std::numeric_limits<std::int64_t>::max() / left) {
    return invalidArgument("accounting value exceeds the supported range");
  }
  return left * right;
}

core::Result<std::int64_t>
moneyForWholeShares(const std::int64_t priceNanodollars,
                    const std::int64_t quantityShares) {
  auto base =
      checkedMultiply(priceNanodollars / priceToMoneyDivisor, quantityShares);
  if (!base.ok()) {
    return base.error();
  }

  const auto remainderProduct =
      (priceNanodollars % priceToMoneyDivisor) * quantityShares;
  auto remainderMoney = remainderProduct / priceToMoneyDivisor;
  const auto roundingRemainder = remainderProduct % priceToMoneyDivisor;
  if (roundingRemainder > priceToMoneyDivisor / 2 ||
      (roundingRemainder == priceToMoneyDivisor / 2 &&
       base.value() % 2 != remainderMoney % 2)) {
    ++remainderMoney;
  }
  return checkedAdd(base.value(), remainderMoney);
}

core::Result<std::int64_t>
buyPriceWithDefaultSlippage(const std::int64_t openNanodollars) {
  const auto slippage = openNanodollars / slippageDivisor +
                        (openNanodollars % slippageDivisor == 0 ? 0 : 1);
  return checkedAdd(openNanodollars, slippage);
}

core::Result<std::int64_t>
sellPriceWithDefaultSlippage(const std::int64_t openNanodollars) {
  const auto slippage = openNanodollars / slippageDivisor +
                        (openNanodollars % slippageDivisor == 0 ? 0 : 1);
  const auto adjusted = openNanodollars - slippage;
  if (adjusted <= 0) {
    return invalidArgument(
        "sell price is outside the supported range after slippage");
  }
  return adjusted;
}

core::Result<void> validateActualBars(const std::vector<core::Bar> &bars) {
  for (std::size_t index = 0; index < bars.size(); ++index) {
    const auto &bar = bars[index];
    if (!bar.isValid() || !std::isfinite(bar.volume)) {
      return invalidArgument("all bars must satisfy OHLCV invariants");
    }
    for (const auto price :
         std::array{bar.open, bar.high, bar.low, bar.close}) {
      if (!checkedPriceNanodollars(price).ok()) {
        return invalidArgument(
            "all bar prices must fit the fixed-point engine range");
      }
    }
    if (index > 0 && bars[index - 1].ts >= bar.ts) {
      return invalidArgument("bars must have strictly increasing timestamps");
    }
  }
  return {};
}

core::Result<void> validateWarmupBars(const std::vector<core::Bar> &bars,
                                      const core::Timestamp firstRunTimestamp) {
  if (bars.size() >
      static_cast<std::size_t>(indicators::maximumIndicatorPeriod)) {
    return invalidArgument(
        "strategy warm-up history exceeds 10000 actual bars");
  }
  for (std::size_t index = 0; index < bars.size(); ++index) {
    const auto &bar = bars[index];
    if (!bar.isValid() || !std::isfinite(bar.volume)) {
      return invalidArgument(
          "all strategy history bars must satisfy OHLCV invariants");
    }
    for (const auto price :
         std::array{bar.open, bar.high, bar.low, bar.close}) {
      if (!checkedPriceNanodollars(price).ok()) {
        return invalidArgument("all strategy history prices must fit the "
                               "fixed-point engine range");
      }
    }
    if (bar.ts >= firstRunTimestamp ||
        (index > 0 && bars[index - 1].ts >= bar.ts)) {
      return invalidArgument("strategy history bars must be strictly "
                             "increasing and precede the run");
    }
  }
  return {};
}

core::Result<std::int64_t> validateRequest(const BacktestRequest &request) {
  if (request.initialCapitalMicrodollars <= 0) {
    return invalidArgument("initial capital must be positive");
  }
  if (request.quantityShares <= 0 ||
      request.quantityShares > maximumStarterQuantityShares) {
    return invalidArgument("quantity must be between 1 and 1000000000 shares");
  }
  if (request.bars.empty()) {
    return invalidArgument("at least one bar is required");
  }
  if (request.symbol.empty()) {
    return invalidArgument("backtest symbol is required");
  }
  const auto actualBars = validateActualBars(request.bars);
  if (!actualBars.ok()) {
    return actualBars.error();
  }
  const auto historyBars =
      validateWarmupBars(request.historyBars, request.bars.front().ts);
  if (!historyBars.ok()) {
    return historyBars.error();
  }
  return request.initialCapitalMicrodollars;
}

BacktestResult makeInitialResult(const BacktestRequest &request) {
  const auto finalPrice =
      checkedPriceNanodollars(request.bars.back().close).value();
  return BacktestResult{
      .orderStatus = StarterOrderStatus::cancelledNoFutureMarketData,
      .fill = {},
      .fills = {},
      .initialCapitalMicrodollars = request.initialCapitalMicrodollars,
      .cashMicrodollars = request.initialCapitalMicrodollars,
      .marketValueMicrodollars = 0,
      .equityMicrodollars = request.initialCapitalMicrodollars,
      .pnlMicrodollars = 0,
      .finalPriceNanodollars = finalPrice,
      .positionShares = 0,
      .barsProcessed = 0,
      .canonicalRecords = {},
      .status = results::RunStatus::running,
      .terminalError = {},
  };
}

void appendRecord(BacktestResult &result, results::CanonicalRecord record) {
  record.sequence = result.canonicalRecords.size();
  result.canonicalRecords.push_back(std::move(record));
}

void appendOrderRecord(BacktestResult &result, const BacktestRequest &request,
                       const core::Bar &bar, const BacktestOrderSide side,
                       const std::int64_t quantityShares) {
  appendRecord(result, {.timestamp = bar.ts,
                        .symbol = request.symbol,
                        .family = results::RecordFamily::order,
                        .side = side == BacktestOrderSide::buy
                                    ? results::OrderSide::buy
                                    : results::OrderSide::sell,
                        .quantityShares = quantityShares,
                        .priceNanodollars = {},
                        .amountMicrodollars = {},
                        .cashMicrodollars = {},
                        .marketValueMicrodollars = {},
                        .equityMicrodollars = {},
                        .positionShares = {},
                        .text = {}});
}

void appendFillRecord(BacktestResult &result, const BacktestRequest &request,
                      const BacktestFill &fill) {
  appendRecord(result, {.timestamp = fill.timestamp,
                        .symbol = request.symbol,
                        .family = results::RecordFamily::fill,
                        .side = fill.side == BacktestOrderSide::buy
                                    ? results::OrderSide::buy
                                    : results::OrderSide::sell,
                        .quantityShares = fill.quantityShares,
                        .priceNanodollars = fill.priceNanodollars,
                        .amountMicrodollars = fill.amountMicrodollars,
                        .cashMicrodollars = {},
                        .marketValueMicrodollars = {},
                        .equityMicrodollars = {},
                        .positionShares = {},
                        .text = {}});
}

core::Result<void>
appendSlippageCostRecord(BacktestResult &result, const BacktestRequest &request,
                         const BacktestFill &fill,
                         const std::int64_t openNanodollars) {
  const auto priceDifference = fill.side == BacktestOrderSide::buy
                                   ? fill.priceNanodollars - openNanodollars
                                   : openNanodollars - fill.priceNanodollars;
  const auto cost = moneyForWholeShares(priceDifference, fill.quantityShares);
  // Request validation bounds both price and quantity, making this overflow
  // guard unreachable for an accepted fill. GCOVR_EXCL_START
  if (!cost.ok()) {
    return cost.error();
  }
  // GCOVR_EXCL_STOP
  appendRecord(result, {.timestamp = fill.timestamp,
                        .symbol = request.symbol,
                        .family = results::RecordFamily::cost,
                        .side = fill.side == BacktestOrderSide::buy
                                    ? results::OrderSide::buy
                                    : results::OrderSide::sell,
                        .quantityShares = fill.quantityShares,
                        .priceNanodollars = {},
                        .amountMicrodollars = cost.value(),
                        .cashMicrodollars = {},
                        .marketValueMicrodollars = {},
                        .equityMicrodollars = {},
                        .positionShares = {},
                        .text = "slippage"});
  return {};
}

void appendDividendWarning(BacktestResult &result,
                           const BacktestRequest &request) {
  appendRecord(result,
               {.timestamp = request.bars.front().ts,
                .symbol = request.symbol,
                .family = results::RecordFamily::warning,
                .side = results::OrderSide::none,
                .quantityShares = {},
                .priceNanodollars = {},
                .amountMicrodollars = {},
                .cashMicrodollars = {},
                .marketValueMicrodollars = {},
                .equityMicrodollars = {},
                .positionShares = {},
                .text = "dividendAccounting=excluded; price-return only"});
}

void appendTerminalDiagnostic(BacktestResult &result,
                              const BacktestRequest &request,
                              const core::Timestamp timestamp,
                              const std::string &reason) {
  appendRecord(result, {.timestamp = timestamp,
                        .symbol = request.symbol,
                        .family = results::RecordFamily::terminalDiagnostic,
                        .side = results::OrderSide::none,
                        .quantityShares = {},
                        .priceNanodollars = {},
                        .amountMicrodollars = {},
                        .cashMicrodollars = {},
                        .marketValueMicrodollars = {},
                        .equityMicrodollars = {},
                        .positionShares = {},
                        .text = reason});
}

core::Result<void> updatePortfolioMark(BacktestResult &result,
                                       const core::Bar &bar) {
  const auto close = checkedPriceNanodollars(bar.close);
  // Pre-run validation checks every close with the same conversion.
  // GCOVR_EXCL_START
  if (!close.ok()) {
    return close.error();
  }
  // GCOVR_EXCL_STOP
  result.finalPriceNanodollars = close.value();
  result.marketValueMicrodollars = 0;
  if (result.positionShares > 0) {
    const auto marketValue =
        moneyForWholeShares(close.value(), result.positionShares);
    if (!marketValue.ok()) {
      return marketValue.error();
    }
    result.marketValueMicrodollars = marketValue.value();
  }
  const auto equity =
      checkedAdd(result.cashMicrodollars, result.marketValueMicrodollars);
  if (!equity.ok()) {
    return equity.error();
  }
  result.equityMicrodollars = equity.value();
  result.pnlMicrodollars =
      result.equityMicrodollars - result.initialCapitalMicrodollars;
  return {};
}

void appendPortfolioRecord(BacktestResult &result,
                           const BacktestRequest &request,
                           const core::Bar &bar) {
  appendRecord(result,
               {.timestamp = bar.ts,
                .symbol = request.symbol,
                .family = results::RecordFamily::portfolio,
                .side = results::OrderSide::none,
                .quantityShares = {},
                .priceNanodollars = {},
                .amountMicrodollars = {},
                .cashMicrodollars = result.cashMicrodollars,
                .marketValueMicrodollars = result.marketValueMicrodollars,
                .equityMicrodollars = result.equityMicrodollars,
                .pnlMicrodollars = result.pnlMicrodollars,
                .positionShares = result.positionShares,
                .text = {}});
}

void recordFill(BacktestResult &result, const BacktestFill &fill) {
  result.fills.push_back(fill);
  if (!result.fill.has_value()) {
    result.fill = fill;
  }
}

struct PendingOrderExecution {
  std::optional<BacktestFill> fill;
  bool rejectedInsufficientCash = false;
};

struct OpenOrderExecution {
  std::int64_t quantityShares = 0;
  std::int64_t openNanodollars = 0;
};

core::Result<PendingOrderExecution>
executeBuyAtOpen(BacktestResult &result, const core::Bar &bar,
                 const OpenOrderExecution &execution) {
  const auto price = buyPriceWithDefaultSlippage(execution.openNanodollars);
  if (!price.ok()) {
    return price.error();
  }
  const auto amount =
      moneyForWholeShares(price.value(), execution.quantityShares);
  if (!amount.ok()) {
    return amount.error();
  }
  if (amount.value() > result.cashMicrodollars) {
    return PendingOrderExecution{
        .fill = {},
        .rejectedInsufficientCash = true,
    };
  }
  result.cashMicrodollars -= amount.value();
  result.positionShares = execution.quantityShares;
  return PendingOrderExecution{.fill = BacktestFill{
                                   .timestamp = bar.ts,
                                   .side = BacktestOrderSide::buy,
                                   .quantityShares = execution.quantityShares,
                                   .priceNanodollars = price.value(),
                                   .amountMicrodollars = amount.value(),
                               }};
}

core::Result<PendingOrderExecution>
executeSellAtOpen(BacktestResult &result, const core::Bar &bar,
                  const std::int64_t openNanodollars) {
  const auto price = sellPriceWithDefaultSlippage(openNanodollars);
  if (!price.ok()) {
    return price.error();
  }
  const auto proceeds =
      moneyForWholeShares(price.value(), result.positionShares);
  if (!proceeds.ok()) {
    return proceeds.error();
  }
  const auto cash = checkedAdd(result.cashMicrodollars, proceeds.value());
  if (!cash.ok()) {
    return cash.error();
  }
  const auto quantity = result.positionShares;
  result.cashMicrodollars = cash.value();
  result.positionShares = 0;
  return PendingOrderExecution{.fill = BacktestFill{
                                   .timestamp = bar.ts,
                                   .side = BacktestOrderSide::sell,
                                   .quantityShares = quantity,
                                   .priceNanodollars = price.value(),
                                   .amountMicrodollars = proceeds.value(),
                               }};
}

core::Result<PendingOrderExecution>
executePendingOrder(const BacktestOrderSide side, BacktestResult &result,
                    const core::Bar &bar, const std::int64_t quantityShares) {
  const auto open = checkedPriceNanodollars(bar.open).value();
  if (side == BacktestOrderSide::buy) {
    return executeBuyAtOpen(
        result, bar,
        {.quantityShares = quantityShares, .openNanodollars = open});
  }
  return executeSellAtOpen(result, bar, open);
}

StarterOrderStatus strategyOrderStatus(const BacktestResult &result,
                                       const bool rejectedOrder,
                                       const bool pendingOrder) noexcept {
  if (!result.fills.empty()) {
    return StarterOrderStatus::filled;
  }
  if (rejectedOrder) {
    return StarterOrderStatus::rejectedInsufficientCash;
  }
  if (pendingOrder) {
    return StarterOrderStatus::cancelledNoFutureMarketData;
  }
  return StarterOrderStatus::completedNoSignal;
}

core::Error rejectedStrategyCommand(std::string message) {
  return core::makeError(core::ErrorCode::strategyCommandRejected,
                         "StrategyCommandRejected: " + std::move(message));
}

class StarterMarketBuyStrategy final : public strategy::Strategy {
public:
  explicit StarterMarketBuyStrategy(const std::int64_t quantityShares)
      : quantityShares_(quantityShares) {}

  [[nodiscard]] core::Result<strategy::StrategyCommandBatch>
  onMarket(const strategy::StrategyContext &context,
           const core::CancellationToken &) override {
    if (submitted_) {
      return strategy::StrategyCommandBatch{};
    }
    submitted_ = true;
    return strategy::StrategyCommandBatch{{
        .kind = strategy::StrategyCommandKind::buyMarket,
        .symbol = std::string{context.symbol},
        .quantityShares = quantityShares_,
    }};
  }

private:
  std::int64_t quantityShares_ = 0;
  bool submitted_ = false;
};

struct RuntimeIndicator final {
  std::string name;
  indicators::StreamingIndicator stream;
  std::optional<indicators::IndicatorValue> latest;
};

[[nodiscard]] bool isValidIndicatorName(const std::string_view name) {
  if (name.empty() || name.size() > 64) {
    return false;
  }
  const auto isLetter = [](const char character) {
    return (character >= 'A' && character <= 'Z') ||
           (character >= 'a' && character <= 'z');
  };
  return isLetter(name.front()) &&
         std::ranges::all_of(name.substr(1), [&](const char character) {
           return isLetter(character) ||
                  (character >= '0' && character <= '9') || character == '_';
         });
}

[[nodiscard]] core::Result<std::vector<RuntimeIndicator>>
createRuntimeIndicators(const strategy::StrategyRequirements &requirements) {
  if (requirements.maxHistoryBars >
      static_cast<std::size_t>(indicators::maximumIndicatorPeriod)) {
    return invalidArgument("strategy history budget exceeds 10000 actual bars");
  }
  std::vector<RuntimeIndicator> result;
  result.reserve(requirements.indicators.size());
  std::set<std::string> names;
  for (const auto &registration : requirements.indicators) {
    const auto &definition = registration.definition;
    if (!isValidIndicatorName(registration.name) ||
        !names.insert(registration.name).second ||
        definition.kind != indicators::IndicatorKind::sma ||
        definition.output != indicators::IndicatorOutput::value ||
        definition.field != indicators::BarField::close ||
        definition.period < 1 ||
        definition.period > indicators::maximumIndicatorPeriod ||
        requirements.maxHistoryBars <
            static_cast<std::size_t>(definition.period)) {
      return invalidArgument("strategy indicator registrations require unique "
                             "close SMA names and a sufficient history budget");
    }
    auto stream = indicators::StreamingIndicator::create(definition);
    if (!stream.ok()) {
      return stream.error();
    }
    result.push_back(RuntimeIndicator{.name = registration.name,
                                      .stream = std::move(stream).value(),
                                      .latest = std::nullopt});
  }
  return result;
}

} // namespace

#if defined(BTE_ENABLE_TEST_HOOKS)
namespace testing {

std::uint64_t backtestStartCount() noexcept {
  return backtestStartCountStorage().load();
}

void recordBacktestStart() noexcept {
  backtestStartCountStorage().fetch_add(1);
}

} // namespace testing
#endif

struct BacktestExecution::Impl final {
  Impl(BacktestRequest requestValue, BacktestResult resultValue,
       std::unique_ptr<strategy::Strategy> strategyValue,
       std::vector<RuntimeIndicator> runtimeIndicatorsValue,
       const std::size_t maxHistoryBarsValue)
      : request(std::move(requestValue)), result(std::move(resultValue)),
        strategy(std::move(strategyValue)),
        runtimeIndicators(std::move(runtimeIndicatorsValue)),
        maxHistoryBars(maxHistoryBarsValue) {
    indicatorValues.reserve(runtimeIndicators.size());
    for (const auto &indicator : runtimeIndicators) {
      indicatorValues.push_back(
          {.name = indicator.name, .value = indicator.latest});
    }
  }

  struct PendingOrder final {
    BacktestOrderSide side = BacktestOrderSide::buy;
    std::int64_t quantityShares = 0;
  };

  struct SliceCheckpoint final {
    StarterOrderStatus orderStatus = StarterOrderStatus::completedNoSignal;
    std::optional<BacktestFill> firstFill;
    std::size_t fillCount = 0;
    std::int64_t cashMicrodollars = 0;
    std::int64_t marketValueMicrodollars = 0;
    std::int64_t equityMicrodollars = 0;
    std::int64_t pnlMicrodollars = 0;
    std::int64_t finalPriceNanodollars = 0;
    std::int64_t positionShares = 0;
    std::size_t recordCount = 0;
    std::optional<PendingOrder> pendingOrder;
    std::optional<std::int64_t> currentMarkPriceNanodollars;
    bool rejectedOrder = false;
  };

  BacktestRequest request;
  BacktestResult result;
  std::unique_ptr<strategy::Strategy> strategy;
  std::vector<RuntimeIndicator> runtimeIndicators;
  std::vector<strategy::StrategyIndicatorValue> indicatorValues;
  std::size_t maxHistoryBars = 0;
  bool warmed = false;
  std::optional<PendingOrder> pendingOrder = std::nullopt;
  std::optional<std::int64_t> currentMarkPriceNanodollars = std::nullopt;
  std::size_t nextBar = 0;
  bool started = false;
  bool rejectedOrder = false;
  std::unique_ptr<results::ResultWriter> ownedWriter = nullptr;
  results::ResultWriter *recordingWriter = nullptr;
  std::size_t committedRecords = 0;
  std::optional<core::Error> persistenceError;
  bool finalized = false;
  std::vector<strategy::StrategyDiagnostic> terminalDiagnostics;
  std::uint64_t omittedDiagnosticBytes = 0;
  std::uint64_t omittedDiagnosticEntries = 0;
  bool truncationNoticeAppended = false;
  bool strategyTerminalDiagnosticAppended = false;

  [[nodiscard]] core::Result<void>
  appendStrategyDiagnostic(const strategy::StrategyDiagnostic &diagnostic,
                           const core::Timestamp timestamp) {
    nlohmann::json payload;
    auto family = results::RecordFamily::log;
    if (diagnostic.kind == strategy::StrategyDiagnosticKind::log) {
      const auto fields =
          core::parseCanonicalJson(diagnostic.canonicalFieldsJson);
      if (!fields.ok() || !fields.value().is_object() ||
          diagnostic.hook.empty() || diagnostic.level.empty()) {
        return core::makeError(
            core::ErrorCode::strategyProtocolViolation,
            "Strategy returned a malformed structured diagnostic.");
      }
      payload = nlohmann::json{{"fields", fields.value()},
                               {"hook", diagnostic.hook},
                               {"level", diagnostic.level},
                               {"message", diagnostic.message}};
    } else {
      family = results::RecordFamily::terminalDiagnostic;
      payload = nlohmann::json{
          {"errorCode", diagnostic.errorCode},
          {"hook", diagnostic.hook},
          {"message", diagnostic.message},
          {"omittedBytes", diagnostic.omittedBytes},
          {"omittedEntries", diagnostic.omittedEntries},
          {"sourceColumn", diagnostic.sourceColumn.has_value()
                               ? nlohmann::json(diagnostic.sourceColumn.value())
                               : nlohmann::json(nullptr)},
          {"sourceLine", diagnostic.sourceLine.has_value()
                             ? nlohmann::json(diagnostic.sourceLine.value())
                             : nlohmann::json(nullptr)},
          {"traceback", diagnostic.traceback},
      };
      strategyTerminalDiagnosticAppended = true;
    }
    const auto canonical = core::serializeCanonicalJson(payload);
    if (!canonical.ok()) {
      return canonical.error();
    }
    appendRecord(result, {.timestamp = timestamp,
                          .symbol = request.symbol,
                          .family = family,
                          .side = results::OrderSide::none,
                          .text = canonical.value()});
    if (diagnostic.omittedBytes > 0 || diagnostic.omittedEntries > 0) {
      result.diagnosticsComplete = false;
    }
    return {};
  } // GCOVR_EXCL_LINE: the compiler emits no executable instruction here.

  [[nodiscard]] core::Result<void>
  collectStrategyDiagnostics(const core::Timestamp timestamp,
                             const bool finalize) {
    auto batch = strategy->takeDiagnostics();
    result.diagnosticsComplete = result.diagnosticsComplete && batch.complete;
    omittedDiagnosticBytes += batch.omittedLogBytes;
    omittedDiagnosticEntries += batch.omittedLogEntries;
    if (!batch.complete || batch.omittedLogBytes > 0 ||
        batch.omittedLogEntries > 0) {
      result.diagnosticsComplete = false;
    }
    for (auto &diagnostic : batch.records) {
      if (diagnostic.omittedBytes > 0 || diagnostic.omittedEntries > 0) {
        result.diagnosticsComplete = false;
      }
      omittedDiagnosticBytes += diagnostic.omittedBytes;
      if (diagnostic.kind == strategy::StrategyDiagnosticKind::terminal) {
        terminalDiagnostics.push_back(std::move(diagnostic));
      } else {
        const auto appended = appendStrategyDiagnostic(diagnostic, timestamp);
        if (!appended.ok()) {
          result.diagnosticsComplete = false;
          return appended.error();
        }
      }
    }
    if (!finalize) {
      return {};
    }
    if (!truncationNoticeAppended &&
        (!result.diagnosticsComplete || omittedDiagnosticBytes > 0 ||
         omittedDiagnosticEntries > 0)) {
      const nlohmann::json notice{
          {"fields",
           {{"omitted_bytes", std::to_string(omittedDiagnosticBytes)},
            {"omitted_entries", std::to_string(omittedDiagnosticEntries)}}},
          {"hook", "engine"},
          {"level", "warning"},
          {"message", "DiagnosticsTruncated"},
      };
      const auto canonical = core::serializeCanonicalJson(notice);
      if (!canonical.ok()) {
        return canonical.error();
      }
      appendRecord(result, {.timestamp = timestamp,
                            .symbol = request.symbol,
                            .family = results::RecordFamily::log,
                            .side = results::OrderSide::none,
                            .text = canonical.value()});
      truncationNoticeAppended = true;
    }
    for (const auto &diagnostic : terminalDiagnostics) {
      const auto appended = appendStrategyDiagnostic(diagnostic, timestamp);
      if (!appended.ok()) {
        result.diagnosticsComplete = false;
        return appended.error();
      }
    }
    terminalDiagnostics.clear();
    return {};
  }

  [[nodiscard]] SliceCheckpoint checkpoint() const {
    return {
        .orderStatus = result.orderStatus,
        .firstFill = result.fill,
        .fillCount = result.fills.size(),
        .cashMicrodollars = result.cashMicrodollars,
        .marketValueMicrodollars = result.marketValueMicrodollars,
        .equityMicrodollars = result.equityMicrodollars,
        .pnlMicrodollars = result.pnlMicrodollars,
        .finalPriceNanodollars = result.finalPriceNanodollars,
        .positionShares = result.positionShares,
        .recordCount = result.canonicalRecords.size(),
        .pendingOrder = pendingOrder,
        .currentMarkPriceNanodollars = currentMarkPriceNanodollars,
        .rejectedOrder = rejectedOrder,
    };
  }

  void restore(const SliceCheckpoint &saved) {
    result.orderStatus = saved.orderStatus;
    result.fill = saved.firstFill;
    result.fills.resize(saved.fillCount);
    result.cashMicrodollars = saved.cashMicrodollars;
    result.marketValueMicrodollars = saved.marketValueMicrodollars;
    result.equityMicrodollars = saved.equityMicrodollars;
    result.pnlMicrodollars = saved.pnlMicrodollars;
    result.finalPriceNanodollars = saved.finalPriceNanodollars;
    result.positionShares = saved.positionShares;
    result.canonicalRecords.resize(saved.recordCount);
    pendingOrder = saved.pendingOrder;
    currentMarkPriceNanodollars = saved.currentMarkPriceNanodollars;
    rejectedOrder = saved.rejectedOrder;
  }

  [[nodiscard]] strategy::StrategyContext
  contextFor(std::optional<core::Bar> bar = std::nullopt) const {
    return {.symbol = request.symbol,
            .bar = bar,
            .cashMicrodollars = result.cashMicrodollars,
            .equityMicrodollars = result.equityMicrodollars,
            .positionShares = result.positionShares,
            .markPriceNanodollars = currentMarkPriceNanodollars,
            .warmupHistory = request.historyBars,
            .indicatorValues = indicatorValues};
  }

  [[nodiscard]] core::Result<void>
  updateIndicators(const core::Bar &bar,
                   const core::CancellationToken &cancellation) {
    for (std::size_t index = 0; index < runtimeIndicators.size(); ++index) {
      if (cancellation.isCancellationRequested()) {
        return cancelled();
      }
      auto updated = runtimeIndicators[index].stream.update(bar);
      if (!updated.ok()) {
        return updated.error();
      }
      runtimeIndicators[index].latest = updated.value();
      indicatorValues[index].value = updated.value();
    }
    return {};
  }

  [[nodiscard]] core::Result<void>
  warmup(const core::CancellationToken &cancellation) {
    if (warmed) {
      return {};
    }
    for (const auto &bar : request.historyBars) {
      const auto updated = updateIndicators(bar, cancellation);
      if (!updated.ok()) {
        return updated.error();
      }
    }
    warmed = true;
    return {};
  }

  [[nodiscard]] core::Result<void>
  processPendingOrder(const core::Bar &bar,
                      const core::CancellationToken &cancellation) {
    if (!pendingOrder.has_value()) {
      return {};
    }
    const auto order = pendingOrder.value();
    const auto open = checkedPriceNanodollars(bar.open).value();
    auto execution =
        executePendingOrder(order.side, result, bar, order.quantityShares);
    if (!execution.ok()) {
      return execution.error();
    }
    const auto executionResult = std::move(execution).value();
    rejectedOrder = rejectedOrder || executionResult.rejectedInsufficientCash;
    if (executionResult.fill.has_value()) {
      const auto &fill = executionResult.fill.value();
      recordFill(result, fill);
      appendFillRecord(result, request, fill);
      auto cost = appendSlippageCostRecord(result, request, fill, open);
      // Accepted prices and quantities fit the fixed-point cost operation.
      // GCOVR_EXCL_START
      if (!cost.ok()) {
        return cost.error();
      }
      // GCOVR_EXCL_STOP
      const auto notified =
          strategy->onFill({.timestamp = fill.timestamp,
                            .side = fill.side == BacktestOrderSide::buy
                                        ? strategy::StrategyFillSide::buy
                                        : strategy::StrategyFillSide::sell,
                            .quantityShares = fill.quantityShares,
                            .priceNanodollars = fill.priceNanodollars,
                            .amountMicrodollars = fill.amountMicrodollars},
                           contextFor(bar), cancellation);
      if (!notified.ok()) {
        return notified.error();
      }
      if (cancellation.isCancellationRequested()) {
        return cancelled();
      }
    }
    pendingOrder.reset();
    return {};
  }

  [[nodiscard]] core::Result<void>
  queueBuyCommand(const strategy::StrategyCommand &command,
                  const core::Bar &bar) {
    if (command.quantityShares <= 0 ||
        command.quantityShares > maximumStarterQuantityShares) {
      return rejectedStrategyCommand(
          "buy quantity is outside the supported whole-share range");
    }
    if (result.positionShares != 0) {
      return rejectedStrategyCommand("a market buy requires a flat position");
    }
    pendingOrder = PendingOrder{.side = BacktestOrderSide::buy,
                                .quantityShares = command.quantityShares};
    appendOrderRecord(result, request, bar, pendingOrder->side,
                      pendingOrder->quantityShares);
    return {};
  }

  [[nodiscard]] core::Result<void>
  queueCloseCommand(const strategy::StrategyCommand &command,
                    const core::Bar &bar) {
    if (command.quantityShares != 0) {
      return rejectedStrategyCommand(
          "a close-position command does not accept a quantity");
    }
    if (result.positionShares <= 0) {
      return rejectedStrategyCommand(
          "a close-position command requires a long position");
    }
    pendingOrder = PendingOrder{.side = BacktestOrderSide::sell,
                                .quantityShares = result.positionShares};
    appendOrderRecord(result, request, bar, pendingOrder->side,
                      pendingOrder->quantityShares);
    return {};
  }

  [[nodiscard]] core::Result<void>
  queueStrategyCommands(const strategy::StrategyCommandBatch &commands,
                        const core::Bar &bar) {
    if (commands.size() > strategy::maximumCommandsPerMarketCallback) {
      return rejectedStrategyCommand(
          "a market callback may submit at most one command");
    }
    if (commands.empty()) {
      return {};
    }
    const auto &command = commands.front();
    if (command.symbol != request.symbol) {
      return rejectedStrategyCommand(
          "command symbol must match the selected symbol");
    }
    if (command.kind == strategy::StrategyCommandKind::buyMarket) {
      return queueBuyCommand(command, bar);
    }
    if (command.kind == strategy::StrategyCommandKind::closePosition) {
      return queueCloseCommand(command, bar);
    }
    return rejectedStrategyCommand("command kind is unsupported");
  }

  [[nodiscard]] core::Result<void>
  processMarket(const core::Bar &bar,
                const core::CancellationToken &cancellation) {
    if (cancellation.isCancellationRequested()) {
      return cancelled();
    }
    const auto executed = processPendingOrder(bar, cancellation);
    if (!executed.ok()) {
      return executed.error();
    }
    const auto indicatorsUpdated = updateIndicators(bar, cancellation);
    if (!indicatorsUpdated.ok()) {
      return indicatorsUpdated.error();
    }
    const auto marked = updatePortfolioMark(result, bar);
    if (!marked.ok()) {
      return marked.error();
    }
    currentMarkPriceNanodollars = result.finalPriceNanodollars;
    if (cancellation.isCancellationRequested()) {
      return cancelled();
    }
    const auto decision = strategy->onMarket(contextFor(bar), cancellation);
    if (!decision.ok()) {
      return decision.error();
    }
    const auto queued = queueStrategyCommands(decision.value(), bar);
    if (!queued.ok()) {
      return queued.error();
    }
    const auto diagnostics = collectStrategyDiagnostics(bar.ts, false);
    if (!diagnostics.ok()) {
      return diagnostics.error();
    }
    appendPortfolioRecord(result, request, bar);
    return {};
  }

  void fail(const core::Error &error, const core::Timestamp timestamp) {
    strategy->onCancel();
    result.status = error.code == core::ErrorCode::cancelled
                        ? results::RunStatus::canceled
                        : results::RunStatus::failed;
    result.terminalError = error;
    const auto diagnostics = collectStrategyDiagnostics(timestamp, true);
    if (!strategyTerminalDiagnosticAppended) {
      appendTerminalDiagnostic(result, request, timestamp, error.message);
    }
    if (!diagnostics.ok()) {
      result.diagnosticsComplete = false;
    }
  }

  [[nodiscard]] core::Result<void>
  finish(const core::CancellationToken &cancellation) {
    result.orderStatus =
        strategyOrderStatus(result, rejectedOrder, pendingOrder.has_value());
    if (request.requiredFinalMarkTimestamp.has_value() &&
        request.bars.back().ts != *request.requiredFinalMarkTimestamp) {
      const auto error =
          core::makeError(core::ErrorCode::dataUnavailable, "StaleFinalMark");
      strategy->onCancel();
      const auto diagnostics =
          collectStrategyDiagnostics(request.bars.back().ts, true);
      result.status = results::RunStatus::incomplete;
      result.terminalError = error;
      appendTerminalDiagnostic(result, request, request.bars.back().ts,
                               error.message);
      if (!diagnostics.ok()) {
        result.diagnosticsComplete = false;
      }
      return {};
    }
    if (cancellation.isCancellationRequested()) {
      const auto error = cancelled();
      fail(error, request.bars.back().ts);
      return error;
    }
    const auto finished =
        strategy->onFinish(contextFor(request.bars.back()), cancellation);
    if (!finished.ok()) {
      fail(finished.error(), request.bars.back().ts);
      return finished.error();
    }
    const auto diagnostics =
        collectStrategyDiagnostics(request.bars.back().ts, true);
    if (!diagnostics.ok()) {
      fail(diagnostics.error(), request.bars.back().ts);
      return diagnostics.error();
    }
    if (cancellation.isCancellationRequested()) {
      const auto error = cancelled();
      fail(error, request.bars.back().ts);
      return error;
    }
    result.status = results::RunStatus::completed;
    return {};
  }
};

BacktestExecution::BacktestExecution(ConstructionKey,
                                     std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}

BacktestExecution::~BacktestExecution() = default;

core::Result<std::unique_ptr<BacktestExecution>>
BacktestExecution::start(BacktestRequest request) {
  return startWithStrategy(std::move(request), nullptr, true);
}

core::Result<std::unique_ptr<BacktestExecution>>
BacktestExecution::start(BacktestRequest request,
                         std::unique_ptr<strategy::Strategy> strategy) {
  return startWithStrategy(std::move(request), std::move(strategy), false);
}

core::Result<std::unique_ptr<BacktestExecution>>
BacktestExecution::startWithStrategy(
    BacktestRequest request, std::unique_ptr<strategy::Strategy> strategy,
    const bool useLegacySelection) {
#if defined(BTE_ENABLE_TEST_HOOKS)
  testing::recordBacktestStart();
#endif
  const auto validation = validateRequest(request);
  if (!validation.ok()) {
    return validation.error();
  }
  if (request.requiredFinalMarkTimestamp.has_value() &&
      *request.requiredFinalMarkTimestamp < request.bars.back().ts) {
    return invalidArgument(
        "required final mark cannot precede the last actual bar");
  }
  if (useLegacySelection) {
    if (request.selectableStrategy.has_value()) {
      auto created = strategy::createSelectableStrategyAdapter(
          *request.selectableStrategy, request.quantityShares,
          request.previousDataDayClose, request.previousDataDays);
      if (!created.ok()) {
        return created.error();
      }
      strategy = std::move(created).value();
    } else {
      strategy =
          std::make_unique<StarterMarketBuyStrategy>(request.quantityShares);
    }
    request.selectableStrategy.reset();
  } else if (request.selectableStrategy.has_value()) {
    return invalidArgument(
        "a caller-supplied Strategy cannot also use a Selectable plan");
  }
  if (strategy == nullptr) {
    return invalidArgument("a caller-supplied Strategy is required");
  }
  const auto &requirements = strategy->requirements();
  if (request.historyBars.size() > requirements.maxHistoryBars) {
    return invalidArgument(
        "strategy history bars exceed the strategy's declared history budget");
  }
  auto runtimeIndicators = createRuntimeIndicators(requirements);
  if (!runtimeIndicators.ok()) {
    return runtimeIndicators.error();
  }
  auto result = makeInitialResult(request);
  result.fills.reserve(request.bars.size());
  appendDividendWarning(result, request);
  auto implementation = std::make_unique<Impl>(
      std::move(request), std::move(result), std::move(strategy),
      std::move(runtimeIndicators).value(), requirements.maxHistoryBars);
  return std::make_unique<BacktestExecution>(ConstructionKey{},
                                             std::move(implementation));
}

core::Result<std::unique_ptr<BacktestExecution>>
BacktestExecution::startRecorded(
    BacktestRequest request, const results::ResultStore &store,
    const results::RunDescriptor &descriptor,
    const core::CancellationToken &cancellation,
    const RecordedResultObserver &onResultCreated) {
  return startRecordedWithStrategy(std::move(request), nullptr, store,
                                   descriptor, cancellation, onResultCreated,
                                   true);
}

core::Result<std::unique_ptr<BacktestExecution>>
BacktestExecution::startRecorded(
    BacktestRequest request, std::unique_ptr<strategy::Strategy> strategy,
    const results::ResultStore &store, const results::RunDescriptor &descriptor,
    const core::CancellationToken &cancellation,
    const RecordedResultObserver &onResultCreated) {
  return startRecordedWithStrategy(std::move(request), std::move(strategy),
                                   store, descriptor, cancellation,
                                   onResultCreated, false);
} // GCOVR_EXCL_LINE: the compiler emits no executable instruction here.

core::Result<std::unique_ptr<BacktestExecution>>
BacktestExecution::startRecordedWithStrategy(
    BacktestRequest request, std::unique_ptr<strategy::Strategy> strategy,
    const results::ResultStore &store, const results::RunDescriptor &descriptor,
    const core::CancellationToken &cancellation,
    const RecordedResultObserver &onResultCreated,
    const bool useLegacySelection) {
  if (cancellation.isCancellationRequested()) {
    return cancelled();
  }
  auto execution = startWithStrategy(std::move(request), std::move(strategy),
                                     useLegacySelection);
  if (!execution.ok()) {
    return execution.error();
  }
  const auto &validated = execution.value()->impl_->request;
  if (descriptor.universe != std::vector<std::string>{validated.symbol} ||
      descriptor.initialCapitalMicrodollars !=
          validated.initialCapitalMicrodollars ||
      validated.bars.front().ts < descriptor.range.start ||
      validated.bars.back().ts >= descriptor.range.end) {
    return invalidArgument(
        "Result descriptor does not identify the validated Backtest request");
  }
  auto normalized = normalizeBars(validated.bars);
  if (!normalized.ok()) {
    return normalized.error();
  }
  auto exact = store.validateSelectedBars(descriptor.dataSelection,
                                          normalized.value(), cancellation);
  if (!exact.ok()) {
    return exact.error();
  }
  auto writer = store.begin(descriptor);
  if (!writer.ok()) {
    return writer.error();
  }
  auto &implementation = *execution.value()->impl_;
  implementation.ownedWriter = std::move(writer).value();
  implementation.recordingWriter = implementation.ownedWriter.get();
  if (onResultCreated) {
    const auto observed =
        onResultCreated(implementation.recordingWriter->resultId());
    if (!observed.ok()) {
      return observed.error();
    }
  }
  return std::move(execution).value();
}

core::Result<void>
BacktestExecution::advance(const core::CancellationToken &cancellation) {
  if (impl_->result.status != results::RunStatus::running) {
    return invalidArgument("backtest execution is already terminal");
  }
  const auto timestamp = impl_->request.bars[impl_->nextBar].ts;
  if (cancellation.isCancellationRequested()) {
    const auto error = cancelled();
    impl_->fail(error, timestamp);
    return error;
  }
  if (!impl_->started) {
    const auto started =
        impl_->strategy->onStart(impl_->contextFor(), cancellation);
    if (!started.ok()) {
      impl_->fail(started.error(), timestamp);
      return started.error();
    }
    impl_->started = true;
    if (cancellation.isCancellationRequested()) {
      const auto error = cancelled();
      impl_->fail(error, timestamp);
      return error;
    }
  }
  const auto warmed = impl_->warmup(cancellation);
  if (!warmed.ok()) {
    impl_->fail(warmed.error(), timestamp);
    return warmed.error();
  }
  const auto saved = impl_->checkpoint();
  auto processed =
      impl_->processMarket(impl_->request.bars[impl_->nextBar], cancellation);
  if (!processed.ok()) {
    impl_->restore(saved);
    impl_->fail(processed.error(), timestamp);
    return processed.error();
  }
  if (cancellation.isCancellationRequested()) {
    impl_->restore(saved);
    const auto error = cancelled();
    impl_->fail(error, timestamp);
    return error;
  }
  if (impl_->recordingWriter != nullptr) {
    // Only this completed bar's delta is copied; preceding records stay put.
    const auto records = std::span{impl_->result.canonicalRecords}.subspan(
        impl_->committedRecords);
    const auto batch =
        std::vector<results::CanonicalRecord>{records.begin(), records.end()};
    auto appended = impl_->recordingWriter->append(batch);
    if (!appended.ok()) {
      impl_->restore(saved);
      impl_->result.canonicalRecords.resize(impl_->committedRecords);
      impl_->result.status = results::RunStatus::failed;
      impl_->result.terminalError = appended.error();
      impl_->persistenceError = appended.error();
      // An uncertain append is never retried. Recovery owns the disk verdict.
      impl_->recordingWriter = nullptr;
      impl_->ownedWriter.reset();
      return appended.error();
    }
    impl_->committedRecords = impl_->result.canonicalRecords.size();
  }
  ++impl_->nextBar;
  impl_->result.barsProcessed = impl_->nextBar;
  if (impl_->nextBar == impl_->request.bars.size()) {
    const auto finished = impl_->finish(cancellation);
    if (!finished.ok()) {
      return finished.error();
    }
  }
  return {};
}

core::Result<void> BacktestExecution::runToCompletion(
    const core::CancellationToken &cancellation) {
  while (impl_->result.status == results::RunStatus::running) {
    auto advanced = advance(cancellation);
    if (!advanced.ok()) {
      return advanced.error();
    }
  }
  return {};
}

results::RunStatus BacktestExecution::status() const noexcept {
  return impl_->result.status;
}

const BacktestResult &BacktestExecution::result() const noexcept {
  return impl_->result;
}

const std::optional<core::Error> &
BacktestExecution::terminalError() const noexcept {
  return impl_->result.terminalError;
}

core::Result<RecordedBacktestOutcome>
BacktestExecution::finalizeAndRecord(results::ResultWriter &writer) {
  if (const auto persistenceError = impl_->persistenceError;
      persistenceError.has_value()) {
    return persistenceError.value();
  }
  if (impl_->result.status == results::RunStatus::running || impl_->finalized) {
    return invalidArgument("only one terminal backtest result can be recorded");
  }
  if (impl_->recordingWriter != nullptr && impl_->recordingWriter != &writer) {
    return invalidArgument("recorded execution cannot switch Result writers");
  }
  const auto completed = impl_->result.status == results::RunStatus::completed;
  results::FinalSummary summary{
      .finalEquityMicrodollars = std::nullopt,
      .pnlMicrodollars = std::nullopt,
      .diagnosticsComplete = impl_->result.diagnosticsComplete,
  };
  if (completed) {
    summary.finalEquityMicrodollars = impl_->result.equityMicrodollars;
    summary.pnlMicrodollars = impl_->result.pnlMicrodollars;
  }
  std::string reason;
  if (const auto &terminalError = impl_->result.terminalError;
      terminalError.has_value()) {
    reason = terminalError.value().message;
  }
  const auto tail =
      std::span<const results::CanonicalRecord>{impl_->result.canonicalRecords}
          .subspan(impl_->committedRecords);
  auto finalized =
      writer.finalizeDraft(impl_->result.status, summary, reason, tail);
  if (!finalized.ok()) {
    return finalized.error();
  }
  impl_->finalized = true;
  return RecordedBacktestOutcome{
      .backtest = impl_->result,
      .persisted = std::move(finalized).value(),
      .status = impl_->result.status,
      .terminalError = impl_->result.terminalError,
  };
}

core::Result<RecordedBacktestOutcome> BacktestExecution::finalizeRecorded() {
  if (const auto persistenceError = impl_->persistenceError;
      persistenceError.has_value()) {
    return persistenceError.value();
  }
  if (impl_->recordingWriter == nullptr) {
    return invalidArgument("Backtest execution is not recorded");
  }
  return finalizeAndRecord(*impl_->recordingWriter);
}

core::Result<BacktestResult>
runBacktest(const BacktestRequest &request,
            const core::CancellationToken &cancellation) {
  if (cancellation.isCancellationRequested()) {
    return cancelled();
  }
  auto execution = BacktestExecution::start(request);
  if (!execution.ok()) {
    return execution.error();
  }
  auto completed = execution.value()->runToCompletion(cancellation);
  if (!completed.ok()) {
    return completed.error();
  }
  return execution.value()->result();
}

core::Result<RecordedBacktestOutcome>
runBacktestAndRecord(const BacktestRequest &request,
                     results::ResultWriter &writer,
                     const core::CancellationToken &cancellation) {
  if (cancellation.isCancellationRequested()) {
    return cancelled();
  }
  auto execution = BacktestExecution::start(request);
  if (!execution.ok()) {
    return execution.error();
  }
  execution.value()->impl_->recordingWriter = &writer;
  const auto ignored = execution.value()->runToCompletion(cancellation);
  return execution.value()->finalizeAndRecord(writer);
}

core::Result<RecordedBacktestOutcome>
runBacktestAndRecord(const BacktestRequest &request,
                     const results::ResultStore &store,
                     const results::RunDescriptor &descriptor,
                     const core::CancellationToken &cancellation) {
  auto execution = BacktestExecution::startRecorded(request, store, descriptor,
                                                    cancellation);
  if (!execution.ok()) {
    return execution.error();
  }
  const auto ignored = execution.value()->runToCompletion(cancellation);
  return execution.value()->finalizeRecorded();
}

core::Result<RecordedBacktestOutcome> runBacktestAndRecord(
    const BacktestRequest &request,
    std::unique_ptr<strategy::Strategy> strategy,
    const results::ResultStore &store, const results::RunDescriptor &descriptor,
    const core::CancellationToken &cancellation,
    const BacktestExecution::RecordedResultObserver &onResultCreated) {
  auto execution = BacktestExecution::startRecorded(
      request, std::move(strategy), store, descriptor, cancellation,
      onResultCreated);
  if (!execution.ok()) {
    return execution.error();
  }
  const auto ignored = execution.value()->runToCompletion(cancellation);
  return execution.value()->finalizeRecorded();
}

} // namespace bte::engine
