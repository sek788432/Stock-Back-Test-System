#include "Bte/Engine/Backtest.h"

#include "Bte/Core/Result.h"

#include <array>
#include <cmath>
#include <compare>
#include <cstdint>
#include <limits>
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
  if (!std::isfinite(price) || price <= 0.0) {
    return invalidArgument("bar price is outside the supported range");
  }
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

core::Result<std::int64_t> checkedSubtract(const std::int64_t left,
                                           const std::int64_t right) {
  if (right > 0 && left < std::numeric_limits<std::int64_t>::min() + right) {
    return invalidArgument("accounting value exceeds the supported range");
  }
  return left - right;
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
  const auto adjusted = checkedSubtract(openNanodollars, slippage);
  if (!adjusted.ok()) {
    return adjusted.error();
  }
  if (adjusted.value() <= 0) {
    return invalidArgument(
        "sell price is outside the supported range after slippage");
  }
  return adjusted.value();
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

core::Result<BacktestResult> makeInitialResult(const BacktestRequest &request) {
  const auto finalPrice = checkedPriceNanodollars(request.bars.back().close);
  if (!finalPrice.ok()) {
    return finalPrice.error();
  }
  return BacktestResult{
      .orderStatus = StarterOrderStatus::cancelledNoFutureMarketData,
      .fill = {},
      .fills = {},
      .initialCapitalMicrodollars = request.initialCapitalMicrodollars,
      .cashMicrodollars = request.initialCapitalMicrodollars,
      .marketValueMicrodollars = 0,
      .equityMicrodollars = request.initialCapitalMicrodollars,
      .pnlMicrodollars = 0,
      .finalPriceNanodollars = finalPrice.value(),
      .positionShares = 0,
      .barsProcessed = request.bars.size(),
  };
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
  const auto remainingCash =
      checkedSubtract(result.cashMicrodollars, amount.value());
  if (!remainingCash.ok()) {
    return remainingCash.error();
  }
  result.cashMicrodollars = remainingCash.value();
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
  if (result.positionShares == 0) {
    return PendingOrderExecution{};
  }
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
  const auto open = checkedPriceNanodollars(bar.open);
  if (!open.ok()) {
    return open.error();
  }
  if (side == BacktestOrderSide::buy) {
    return executeBuyAtOpen(
        result, bar,
        {.quantityShares = quantityShares, .openNanodollars = open.value()});
  }
  return executeSellAtOpen(result, bar, open.value());
}

core::Result<bool> updateFinalPortfolio(BacktestResult &result) {
  if (result.positionShares > 0) {
    const auto marketValue = moneyForWholeShares(result.finalPriceNanodollars,
                                                 result.positionShares);
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
  const auto pnl = checkedSubtract(result.equityMicrodollars,
                                   result.initialCapitalMicrodollars);
  if (!pnl.ok()) {
    return pnl.error();
  }
  result.pnlMicrodollars = pnl.value();
  return true;
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
  auto initialized = makeInitialResult(request);
  if (!initialized.ok()) {
    return initialized.error();
  }
  auto result = std::move(initialized).value();
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
      }
      pendingOrder.reset();
    }
    const auto signal = strategy.value()->onBar(bar);
    if (!signal.ok()) {
      return signal.error();
    }
    pendingOrder = nextOrderFor(signal.value(), result.positionShares);
  }

  const auto finalPortfolio = updateFinalPortfolio(result);
  if (!finalPortfolio.ok()) {
    return finalPortfolio.error();
  }
  result.orderStatus =
      selectableStatus(result, rejectedOrder, pendingOrder.has_value());
  return result;
}

core::Result<BacktestResult>
runStarterBacktest(const BacktestRequest &request) {
  auto initialized = makeInitialResult(request);
  if (!initialized.ok()) {
    return initialized.error();
  }
  auto result = std::move(initialized).value();
  if (request.bars.size() == 1U) {
    return result;
  }

  const auto nextOpen = checkedPriceNanodollars(request.bars[1].open);
  if (!nextOpen.ok()) {
    return nextOpen.error();
  }
  const auto fillPrice = buyPriceWithDefaultSlippage(nextOpen.value());
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
    return result;
  }

  const auto marketValue =
      moneyForWholeShares(result.finalPriceNanodollars, request.quantityShares);
  if (!marketValue.ok()) {
    return marketValue.error();
  }
  const auto fill = BacktestFill{
      .timestamp = request.bars[1].ts,
      .side = BacktestOrderSide::buy,
      .quantityShares = request.quantityShares,
      .priceNanodollars = fillPrice.value(),
      .amountMicrodollars = fillAmount.value(),
  };
  const auto remainingCash =
      checkedSubtract(request.initialCapitalMicrodollars, fillAmount.value());
  if (!remainingCash.ok()) {
    return remainingCash.error();
  }
  result.orderStatus = StarterOrderStatus::filled;
  recordFill(result, fill);
  result.cashMicrodollars = remainingCash.value();
  result.marketValueMicrodollars = marketValue.value();
  result.positionShares = request.quantityShares;
  const auto finalPortfolio = updateFinalPortfolio(result);
  if (!finalPortfolio.ok()) {
    return finalPortfolio.error();
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
  if (cancellation.isCancellationRequested()) {
    return cancelled();
  }
  if (request.selectableStrategy.has_value()) {
    return runSelectableBacktest(request, *request.selectableStrategy,
                                 cancellation);
  }
  return runStarterBacktest(request);
}

} // namespace bte::engine
