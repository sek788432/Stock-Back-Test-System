#include "Bte/Engine/Backtest.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>

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
validateRequest(const BacktestRequest &request,
                const core::CancellationToken cancellation) {
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

} // namespace

core::Result<BacktestResult>
runBacktest(const BacktestRequest &request,
            const core::CancellationToken cancellation) {
  const auto validation = validateRequest(request, cancellation);
  if (!validation.ok()) {
    return validation.error();
  }
  if (cancellation.isCancellationRequested()) {
    return cancelled();
  }

  auto result = BacktestResult{
      .initialCapitalMicrodollars = request.initialCapitalMicrodollars,
      .cashMicrodollars = request.initialCapitalMicrodollars,
      .equityMicrodollars = request.initialCapitalMicrodollars,
      .barsProcessed = request.bars.size(),
  };

  const auto finalPrice = checkedPriceNanodollars(request.bars.back().close);
  if (!finalPrice.ok()) {
    return finalPrice.error();
  }
  result.finalPriceNanodollars = finalPrice.value();

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
      moneyForWholeShares(finalPrice.value(), request.quantityShares);
  if (!marketValue.ok()) {
    return marketValue.error();
  }
  result.orderStatus = StarterOrderStatus::filled;
  result.fill = BacktestFill{
      .timestamp = request.bars[1].ts,
      .quantityShares = request.quantityShares,
      .priceNanodollars = fillPrice.value(),
      .amountMicrodollars = fillAmount.value(),
  };
  result.cashMicrodollars =
      request.initialCapitalMicrodollars - fillAmount.value();
  result.marketValueMicrodollars = marketValue.value();
  const auto equity =
      checkedAdd(result.cashMicrodollars, result.marketValueMicrodollars);
  if (!equity.ok()) {
    return equity.error();
  }
  result.equityMicrodollars = equity.value();
  result.pnlMicrodollars =
      result.equityMicrodollars - result.initialCapitalMicrodollars;
  return result;
}

} // namespace bte::engine
