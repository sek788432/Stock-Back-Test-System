#include "Bte/Engine/Backtest.h"

// IWYU pragma: no_include <math>

#include "Bte/Core/Result.h"
#include "Bte/Results/ResultStore.h"

#include <algorithm> // IWYU pragma: keep
#include <array>
#include <cmath>
#include <compare>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <utility>

namespace bte::engine {
namespace {

constexpr auto nanodollarsPerDollar = 1'000'000'000.0;
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
       remainderMoney % 2 != 0)) {
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

core::Result<std::int64_t>
validateRequest(const BacktestRequest &request,
                const core::CancellationToken &cancellation) {
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

  for (std::size_t index = 0; index < request.bars.size(); ++index) {
    if (cancellation.isCancellationRequested()) {
      return cancelled();
    }
    const auto &bar = request.bars[index];
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
    if (index > 0 && request.bars[index - 1].ts >= request.bars[index].ts) {
      return invalidArgument("bars must have strictly increasing timestamps");
    }
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
      .barsProcessed = request.bars.size(),
      .canonicalRecords = {},
  };
}

void appendRecord(BacktestResult &result, results::CanonicalRecord record) {
  record.sequence = result.canonicalRecords.size();
  result.canonicalRecords.push_back(std::move(record));
}

void appendOrderRecord(BacktestResult &result, const BacktestRequest &request,
                       const core::Bar &bar, const BacktestOrderSide side) {
  appendRecord(result, {.timestamp = bar.ts,
                        .symbol = request.symbol,
                        .family = results::RecordFamily::order,
                        .side = side == BacktestOrderSide::buy
                                    ? results::OrderSide::buy
                                    : results::OrderSide::sell,
                        .quantityShares = request.quantityShares});
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
                        .amountMicrodollars = fill.amountMicrodollars});
}

core::Result<void> appendPortfolioRecord(BacktestResult &result,
                                         const BacktestRequest &request,
                                         const core::Bar &bar) {
  const auto close = checkedPriceNanodollars(bar.close);
  if (!close.ok()) {
    return close.error();
  }
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
  appendRecord(result,
               {.timestamp = bar.ts,
                .symbol = request.symbol,
                .family = results::RecordFamily::portfolio,
                .cashMicrodollars = result.cashMicrodollars,
                .marketValueMicrodollars = result.marketValueMicrodollars,
                .equityMicrodollars = result.equityMicrodollars,
                .positionShares = result.positionShares});
  return {};
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

StarterOrderStatus selectableStatus(const BacktestResult &result,
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

std::optional<BacktestOrderSide>
nextOrderFor(const strategy::SelectableStrategySignal &signal,
             const std::int64_t positionShares) noexcept {
  if (positionShares == 0) {
    return signal.buy ? std::optional<BacktestOrderSide>{BacktestOrderSide::buy}
                      : std::nullopt;
  }
  return signal.sell ? std::optional<BacktestOrderSide>{BacktestOrderSide::sell}
                     : std::nullopt;
}

core::Result<BacktestResult>
runSelectableBacktest(const BacktestRequest &request,
                      const strategy::SelectableStrategyPlan &plan,
                      const core::CancellationToken &cancellation) {
  auto result = makeInitialResult(request);
  auto strategy = strategy::SelectableStrategy::create(plan);
  if (!strategy.ok()) {
    return strategy.error();
  }

  std::optional<BacktestOrderSide> pendingOrder;
  auto rejectedOrder = false;
  result.fills.reserve(request.bars.size());
  for (const auto &bar : request.bars) {
    if (cancellation.isCancellationRequested()) {
      return cancelled();
    }
    if (pendingOrder.has_value()) {
      const auto execution = executePendingOrder(*pendingOrder, result, bar,
                                                 request.quantityShares);
      if (!execution.ok()) {
        return execution.error();
      }
      const auto completedExecution = execution.value();
      rejectedOrder =
          rejectedOrder || completedExecution.rejectedInsufficientCash;
      if (completedExecution.fill.has_value()) {
        recordFill(result, completedExecution.fill.value());
        appendFillRecord(result, request, completedExecution.fill.value());
      }
      pendingOrder.reset();
    }
    const auto signal = strategy.value()->onBar(bar);
    if (!signal.ok()) {
      return signal.error();
    }
    pendingOrder = nextOrderFor(signal.value(), result.positionShares);
    if (pendingOrder.has_value()) {
      appendOrderRecord(result, request, bar, *pendingOrder);
    }
    auto checkpoint = appendPortfolioRecord(result, request, bar);
    if (!checkpoint.ok()) {
      return checkpoint.error();
    }
  }
  result.orderStatus =
      selectableStatus(result, rejectedOrder, pendingOrder.has_value());
  return result;
}

core::Result<BacktestResult>
runStarterBacktest(const BacktestRequest &request) {
  auto result = makeInitialResult(request);
  appendOrderRecord(result, request, request.bars.front(),
                    BacktestOrderSide::buy);
  auto firstCheckpoint =
      appendPortfolioRecord(result, request, request.bars.front());
  if (!firstCheckpoint.ok()) {
    return firstCheckpoint.error();
  }
  if (request.bars.size() == 1U) {
    return result;
  }

  const auto nextOpen = checkedPriceNanodollars(request.bars[1].open).value();
  const auto fillPrice = buyPriceWithDefaultSlippage(nextOpen);
  if (!fillPrice.ok()) {
    return fillPrice.error();
  }
  const auto fillAmount =
      moneyForWholeShares(fillPrice.value(), request.quantityShares);
  if (!fillAmount.ok()) {
    return fillAmount.error();
  }
  if (fillAmount.value() > request.initialCapitalMicrodollars) {
    result.orderStatus = StarterOrderStatus::rejectedInsufficientCash;
    for (std::size_t index = 1; index < request.bars.size(); ++index) {
      auto checkpoint =
          appendPortfolioRecord(result, request, request.bars[index]);
      if (!checkpoint.ok()) {
        return checkpoint.error();
      }
    }
    return result;
  }
  const auto fill = BacktestFill{
      .timestamp = request.bars[1].ts,
      .side = BacktestOrderSide::buy,
      .quantityShares = request.quantityShares,
      .priceNanodollars = fillPrice.value(),
      .amountMicrodollars = fillAmount.value(),
  };
  result.orderStatus = StarterOrderStatus::filled;
  recordFill(result, fill);
  appendFillRecord(result, request, fill);
  result.cashMicrodollars =
      request.initialCapitalMicrodollars - fillAmount.value();
  result.positionShares = request.quantityShares;
  for (std::size_t index = 1; index < request.bars.size(); ++index) {
    auto checkpoint =
        appendPortfolioRecord(result, request, request.bars[index]);
    if (!checkpoint.ok()) {
      return checkpoint.error();
    }
  }
  return result;
}

} // namespace

core::Result<BacktestResult>
runBacktest(const BacktestRequest &request,
            const core::CancellationToken &cancellation) {
  const auto validation = validateRequest(request, cancellation);
  if (!validation.ok()) {
    return validation.error();
  }
  if (request.selectableStrategy.has_value()) {
    return runSelectableBacktest(request, *request.selectableStrategy,
                                 cancellation);
  }
  return runStarterBacktest(request);
}

core::Result<RecordedBacktestOutcome>
runBacktestAndRecord(const BacktestRequest &request,
                     results::ResultWriter &writer,
                     const core::CancellationToken &cancellation) {
  const auto validation = validateRequest(request, cancellation);
  if (!validation.ok()) {
    return validation.error();
  }

  auto executed = runBacktest(request, cancellation);
  if (!executed.ok()) {
    const auto status = executed.error().code == core::ErrorCode::cancelled
                            ? results::RunStatus::canceled
                            : results::RunStatus::failed;
    const auto diagnostic = results::CanonicalRecord{
        .sequence = 0,
        .timestamp = request.bars.front().ts,
        .symbol = request.symbol,
        .family = results::RecordFamily::terminalDiagnostic,
        .text = executed.error().message,
    };
    auto appended = writer.append({diagnostic});
    if (!appended.ok()) {
      return appended.error();
    }
    auto finalized =
        writer.finalizeAndPromote(status, {}, executed.error().message);
    if (!finalized.ok()) {
      return finalized.error();
    }
    return RecordedBacktestOutcome{
        .backtest = {},
        .persisted = std::move(finalized).value(),
        .status = status,
        .terminalError = executed.error(),
    };
  }

  auto appended = writer.append(executed.value().canonicalRecords);
  if (!appended.ok()) {
    return appended.error();
  }
  auto finalized = writer.finalizeAndPromote(
      results::RunStatus::completed,
      {.finalEquityMicrodollars = executed.value().equityMicrodollars,
       .pnlMicrodollars = executed.value().pnlMicrodollars});
  if (!finalized.ok()) {
    return finalized.error();
  }
  return RecordedBacktestOutcome{
      .backtest = std::move(executed).value(),
      .persisted = std::move(finalized).value(),
      .status = results::RunStatus::completed,
      .terminalError = {},
  };
}

} // namespace bte::engine
