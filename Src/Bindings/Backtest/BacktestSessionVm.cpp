#include "Bte/Bindings/BacktestSessionVm.h"

// IWYU pragma: no_include <math>

#include "Bte/Bindings/ReplayDataLoader.h"
#include "Bte/Data/ReleaseSnapshot.h"
#include "Bte/Engine/Backtest.h"
#include "Bte/Results/ResultStore.h"

#include <algorithm> // IWYU pragma: keep
#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
#include <string> // IWYU pragma: keep
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
  if (status == engine::StarterOrderStatus::filled) {
    return BacktestOutcome::filled;
  }
  if (status == engine::StarterOrderStatus::rejectedInsufficientCash) {
    return BacktestOutcome::rejectedInsufficientCash;
  }
  if (status == engine::StarterOrderStatus::cancelledNoFutureMarketData) {
    return BacktestOutcome::cancelledNoFutureMarketData;
  }
  return BacktestOutcome::completedNoSignal;
}

BacktestFillSide toViewSide(const engine::BacktestOrderSide side) {
  return side == engine::BacktestOrderSide::buy ? BacktestFillSide::buy
                                                : BacktestFillSide::sell;
}

double moneyToDollars(const std::int64_t microdollars) {
  return static_cast<double>(microdollars) / microdollarsPerDollar;
}

double priceToDollars(const std::int64_t nanodollars) {
  return static_cast<double>(nanodollars) / nanodollarsPerDollar;
}

core::Timestamp timestampFromDate(const QDate date) {
  using namespace std::chrono;
  return core::Timestamp{
      sys_days{year{date.year()} / month{static_cast<unsigned>(date.month())} /
               day{static_cast<unsigned>(date.day())}}};
}

BacktestSnapshot toSnapshot(const engine::BacktestResult &result) {
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
      .resultId = {},
      .canonicalResultHash = {},
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

} // namespace

core::Result<BacktestSnapshot>
// The scalar inputs are intentionally kept explicit at this starter seam.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): stable public seam
runBacktestSession(std::vector<core::Bar> bars, const double initialCapital,
                   const std::int64_t quantityShares,
                   const core::CancellationToken &cancellation) {
  return runBacktestSession(std::move(bars), initialCapital, quantityShares,
                            std::nullopt, cancellation);
}

core::Result<BacktestSnapshot>
// The scalar inputs are intentionally kept explicit at this starter seam.
runBacktestSession(
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): stable public seam
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

  return toSnapshot(engineResult.value());
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

core::Result<BacktestSnapshot>
runPersistedBacktestConfiguration(const BacktestConfiguration &configuration,
                                  const PersistedBacktestStorage &storage,
                                  const core::CancellationToken &cancellation) {
  if (configuration.schema != "ohlcv-1h") {
    return core::makeError(core::ErrorCode::schemaMismatch,
                           "Backtest execution accepts ohlcv-1h only");
  }
  const auto exclusiveEnd = configuration.endDate.addDays(1);
  if (!configuration.startDate.isValid() || !configuration.endDate.isValid() ||
      configuration.startDate > configuration.endDate ||
      !exclusiveEnd.isValid() || storage.strategyHash.size() != 64) {
    return core::makeError(core::ErrorCode::invalidArgument,
                           "Persisted Backtest configuration is invalid");
  }
  auto capital = normalizedCapitalMicrodollars(configuration.initialCapital);
  if (!capital.ok()) {
    return capital.error();
  }
  auto reader = data::ReleaseSnapshotReader::open(
      storage.dataStore, storage.snapshotId, cancellation);
  if (!reader.ok()) {
    return reader.error();
  }
  auto selected = reader.value()->select(
      {.symbols = {configuration.symbol.toStdString()},
       .range = {.start = timestampFromDate(configuration.startDate),
                 .end = timestampFromDate(exclusiveEnd)},
       .timeframe = "ohlcv-1h"},
      cancellation);
  if (!selected.ok()) {
    return selected.error();
  }
  if (selected.value().bars.empty()) {
    return core::makeError(core::ErrorCode::dataUnavailable,
                           "Backtest Data Selection is empty");
  }
  std::vector<core::Bar> bars;
  bars.reserve(selected.value().bars.size());
  for (const auto &bar : selected.value().bars) {
    bars.push_back({
        .ts = bar.timestamp,
        .open = priceToDollars(bar.openNanodollars),
        .high = priceToDollars(bar.highNanodollars),
        .low = priceToDollars(bar.lowNanodollars),
        .close = priceToDollars(bar.closeNanodollars),
        .volume = static_cast<double>(bar.volumeMicroshares) / 1'000'000.0,
    });
  }
  auto store =
      results::ResultStore::open(storage.resultStore, storage.dataStore);
  if (!store.ok()) {
    return store.error();
  }
  auto writer = store.value()->begin({
      .universe = {configuration.symbol.toStdString()},
      .range = {.start = timestampFromDate(configuration.startDate),
                .end = timestampFromDate(exclusiveEnd)},
      .initialCapitalMicrodollars = capital.value(),
      .strategyId = configuration.selectableStrategy.has_value()
                        ? "selectable-conditions"
                        : "starter",
      .strategyHash = storage.strategyHash,
      .dataSelection = selected.value().identity,
  });
  if (!writer.ok()) {
    return writer.error();
  }
  auto recorded = engine::runBacktestAndRecord(
      {.bars = std::move(bars),
       .symbol = configuration.symbol.toStdString(),
       .initialCapitalMicrodollars = capital.value(),
       .quantityShares = configuration.quantityShares,
       .selectableStrategy = configuration.selectableStrategy},
      *writer.value(), cancellation);
  if (!recorded.ok()) {
    return recorded.error();
  }
  if (!recorded.value().backtest.has_value()) {
    return recorded.value().terminalError.value_or(
        core::makeError(core::ErrorCode::internal, "Recorded Backtest failed"));
  }
  // NOLINTNEXTLINE(bugprone-unchecked-optional-access): guarded above
  auto snapshot = toSnapshot(recorded.value().backtest.value());
  snapshot.resultId = recorded.value().persisted.resultId;
  snapshot.canonicalResultHash = recorded.value().persisted.canonicalResultHash;
  return snapshot;
}

} // namespace bte::bindings
