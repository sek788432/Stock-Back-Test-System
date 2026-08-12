#include "Bte/Bindings/BacktestSessionVm.h"

// IWYU pragma: no_include <math>

#include "Bte/Bindings/ReplayDataLoader.h"
#include "Bte/Engine/Backtest.h"

#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>

namespace bte::bindings {
namespace {

constexpr auto microdollarsPerDollar = 1'000'000.0;
constexpr auto nanodollarsPerDollar = 1'000'000'000.0;
constexpr auto int64ExclusiveUpperBound = 9'223'372'036'854'775'808.0;

core::Result<std::int64_t>
normalizedCapitalMicrodollars(const double initialCapital) {
  if (!std::isfinite(initialCapital) || initialCapital <= 0.0) {
    return core::makeError(core::ErrorCode::invalidArgument,
                           "initial capital is outside the supported range");
  }

  const auto scaledCapital = initialCapital * microdollarsPerDollar;
  if (!std::isfinite(scaledCapital) || scaledCapital <= 0.0 ||
      scaledCapital >= int64ExclusiveUpperBound) {
    return core::makeError(core::ErrorCode::invalidArgument,
                           "initial capital is outside the supported range");
  }

  const auto integral = std::floor(scaledCapital);
  const auto fraction = scaledCapital - integral;
  auto rounded = integral;
  if (fraction > 0.5 || (fraction == 0.5 && std::fmod(integral, 2.0) != 0.0)) {
    rounded += 1.0;
  }
  if (rounded < 1.0 || rounded >= int64ExclusiveUpperBound) {
    return core::makeError(core::ErrorCode::invalidArgument,
                           "initial capital is outside the supported range");
  }
  return static_cast<std::int64_t>(rounded);
}

BacktestOutcome toViewOutcome(const engine::StarterOrderStatus status) {
  switch (status) {
  case engine::StarterOrderStatus::filled:
    return BacktestOutcome::filled;
  case engine::StarterOrderStatus::rejectedInsufficientCash:
    return BacktestOutcome::rejectedInsufficientCash;
  case engine::StarterOrderStatus::cancelledNoFutureMarketData:
    return BacktestOutcome::cancelledNoFutureMarketData;
  case engine::StarterOrderStatus::completedNoSignal:
    return BacktestOutcome::completedNoSignal;
  }
  return BacktestOutcome::cancelledNoFutureMarketData;
}

BacktestFillSide toViewSide(const engine::BacktestOrderSide side) {
  switch (side) {
  case engine::BacktestOrderSide::buy:
    return BacktestFillSide::buy;
  case engine::BacktestOrderSide::sell:
    return BacktestFillSide::sell;
  }
  return BacktestFillSide::buy;
}

double moneyToDollars(const std::int64_t microdollars) {
  return static_cast<double>(microdollars) / microdollarsPerDollar;
}

double priceToDollars(const std::int64_t nanodollars) {
  return static_cast<double>(nanodollars) / nanodollarsPerDollar;
}

} // namespace

core::Result<BacktestSnapshot>
// The scalar inputs are intentionally kept explicit at this starter seam.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
runBacktestSession(std::vector<core::Bar> bars, const double initialCapital,
                   const std::int64_t quantityShares,
                   const core::CancellationToken &cancellation) {
  return runBacktestSession(std::move(bars), initialCapital, quantityShares,
                            std::nullopt, cancellation);
}

core::Result<BacktestSnapshot>
// The scalar inputs are intentionally kept explicit at this starter seam.
runBacktestSession(
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
    std::vector<core::Bar> bars, const double initialCapital,
    const std::int64_t quantityShares,
    std::optional<strategy::SelectableStrategyPlan> selectableStrategy,
    const core::CancellationToken &cancellation) {
  const auto normalizedCapital = normalizedCapitalMicrodollars(initialCapital);
  if (!normalizedCapital.ok()) {
    return normalizedCapital.error();
  }

  const auto request = engine::BacktestRequest{
      .bars = std::move(bars),
      .initialCapitalMicrodollars = normalizedCapital.value(),
      .quantityShares = quantityShares,
      .selectableStrategy = std::move(selectableStrategy),
  };
  auto engineResult = engine::runBacktest(request, cancellation);
  if (!engineResult.ok()) {
    return engineResult.error();
  }

  const auto &result = engineResult.value();
  auto snapshot = BacktestSnapshot{
      .outcome = toViewOutcome(result.orderStatus),
      .fill = {},
      .fills = {},
      .initialCapital = moneyToDollars(result.initialCapitalMicrodollars),
      .cash = moneyToDollars(result.cashMicrodollars),
      .marketValue = moneyToDollars(result.marketValueMicrodollars),
      .equity = moneyToDollars(result.equityMicrodollars),
      .pnl = moneyToDollars(result.pnlMicrodollars),
      .finalPrice = priceToDollars(result.finalPriceNanodollars),
      .positionShares = result.positionShares,
      .barsProcessed = result.barsProcessed,
  };
  snapshot.fills.reserve(result.fills.size());
  for (const auto &fill : result.fills) {
    snapshot.fills.push_back({
        .timestamp = fill.timestamp,
        .side = toViewSide(fill.side),
        .quantityShares = fill.quantityShares,
        .price = priceToDollars(fill.priceNanodollars),
        .amount = moneyToDollars(fill.amountMicrodollars),
    });
  }
  if (!snapshot.fills.empty()) {
    snapshot.fill = snapshot.fills.front();
  }
  return snapshot;
}

core::Result<BacktestSnapshot>
runBacktestConfiguration(const BacktestConfiguration &configuration,
                         const core::CancellationToken &cancellation) {
  auto bars = loadBacktestBars(configuration.symbol, configuration.schema,
                               configuration.startDate, configuration.endDate,
                               cancellation);
  if (!bars.ok()) {
    return bars.error();
  }
  return runBacktestSession(std::move(bars).value(),
                            configuration.initialCapital,
                            configuration.quantityShares,
                            configuration.selectableStrategy, cancellation);
}

} // namespace bte::bindings
