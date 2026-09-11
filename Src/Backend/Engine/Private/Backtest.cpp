#include "Bte/Engine/Backtest.h"

// IWYU pragma: no_include <math>

#include "Bte/Core/Result.h"
#include "Bte/Core/Time.h"
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
                       const core::Bar &bar, const BacktestOrderSide side) {
  appendRecord(result, {.timestamp = bar.ts,
                        .symbol = request.symbol,
                        .family = results::RecordFamily::order,
                        .side = side == BacktestOrderSide::buy
                                    ? results::OrderSide::buy
                                    : results::OrderSide::sell,
                        .quantityShares = request.quantityShares,
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

core::Result<void> appendPortfolioRecord(BacktestResult &result,
                                         const BacktestRequest &request,
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

} // namespace

struct BacktestExecution::Impl final {
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
    std::optional<BacktestOrderSide> pendingOrder;
    bool rejectedOrder = false;
  };

  BacktestRequest request;
  BacktestResult result;
  std::unique_ptr<strategy::SelectableStrategy> strategy;
  std::optional<BacktestOrderSide> pendingOrder = std::nullopt;
  std::size_t nextBar = 0;
  bool rejectedOrder = false;
  bool recordsWritten = false;
  bool finalized = false;

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
    rejectedOrder = saved.rejectedOrder;
  }

  [[nodiscard]] core::Result<void> processStarter(const core::Bar &bar) {
    if (nextBar == 0U) {
      appendOrderRecord(result, request, bar, BacktestOrderSide::buy);
    } else if (nextBar == 1U) {
      const auto open = checkedPriceNanodollars(bar.open).value();
      auto execution = executePendingOrder(BacktestOrderSide::buy, result, bar,
                                           request.quantityShares);
      if (!execution.ok()) {
        return execution.error();
      }
      const auto executionResult = std::move(execution).value();
      if (executionResult.rejectedInsufficientCash) {
        result.orderStatus = StarterOrderStatus::rejectedInsufficientCash;
      } else if (executionResult.fill.has_value()) {
        result.orderStatus = StarterOrderStatus::filled;
        const auto &fill = executionResult.fill.value();
        recordFill(result, fill);
        appendFillRecord(result, request, fill);
        auto cost = appendSlippageCostRecord(result, request, fill, open);
        // appendSlippageCostRecord retains its defensive Result contract even
        // though validated starter inputs cannot overflow. GCOVR_EXCL_START
        if (!cost.ok()) {
          return cost.error();
        }
        // GCOVR_EXCL_STOP
      }
    }
    return appendPortfolioRecord(result, request, bar);
  }

  [[nodiscard]] core::Result<void> processSelectable(const core::Bar &bar) {
    if (pendingOrder.has_value()) {
      const auto open = checkedPriceNanodollars(bar.open).value();
      auto execution = executePendingOrder(*pendingOrder, result, bar,
                                           request.quantityShares);
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
        // appendSlippageCostRecord retains its defensive Result contract even
        // though validated strategy inputs cannot overflow. GCOVR_EXCL_START
        if (!cost.ok()) {
          return cost.error();
        }
        // GCOVR_EXCL_STOP
      }
      pendingOrder.reset();
    }
    auto signal = strategy->onBar(bar);
    if (!signal.ok()) {
      return signal.error();
    }
    pendingOrder = nextOrderFor(signal.value(), result.positionShares);
    if (pendingOrder.has_value()) {
      appendOrderRecord(result, request, bar, *pendingOrder);
    }
    return appendPortfolioRecord(result, request, bar);
  }

  void fail(const core::Error &error, const core::Timestamp timestamp) {
    result.status = error.code == core::ErrorCode::cancelled
                        ? results::RunStatus::canceled
                        : results::RunStatus::failed;
    result.terminalError = error;
    appendTerminalDiagnostic(result, request, timestamp, error.message);
  }

  void finish() {
    if (strategy != nullptr) {
      result.orderStatus =
          selectableStatus(result, rejectedOrder, pendingOrder.has_value());
    }
    if (request.requiredFinalMarkTimestamp.has_value() &&
        request.bars.back().ts != *request.requiredFinalMarkTimestamp) {
      const auto error =
          core::makeError(core::ErrorCode::dataUnavailable, "StaleFinalMark");
      result.status = results::RunStatus::incomplete;
      result.terminalError = error;
      appendTerminalDiagnostic(result, request, request.bars.back().ts,
                               error.message);
      return;
    }
    result.status = results::RunStatus::completed;
  }
};

BacktestExecution::BacktestExecution(ConstructionKey,
                                     std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}

BacktestExecution::~BacktestExecution() = default;

core::Result<std::unique_ptr<BacktestExecution>>
BacktestExecution::start(BacktestRequest request) {
  const auto validation = validateRequest(request, {});
  if (!validation.ok()) {
    return validation.error();
  }
  if (request.requiredFinalMarkTimestamp.has_value() &&
      *request.requiredFinalMarkTimestamp < request.bars.back().ts) {
    return invalidArgument(
        "required final mark cannot precede the last actual bar");
  }
  std::unique_ptr<strategy::SelectableStrategy> strategy;
  if (request.selectableStrategy.has_value()) {
    auto created =
        strategy::SelectableStrategy::create(*request.selectableStrategy);
    if (!created.ok()) {
      return created.error();
    }
    strategy = std::move(created).value();
  }
  auto result = makeInitialResult(request);
  result.fills.reserve(request.bars.size());
  appendDividendWarning(result, request);
  auto implementation = std::make_unique<Impl>(Impl{
      .request = std::move(request),
      .result = std::move(result),
      .strategy = std::move(strategy),
  });
  return std::make_unique<BacktestExecution>(ConstructionKey{},
                                             std::move(implementation));
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
  const auto saved = impl_->checkpoint();
  auto processed =
      impl_->strategy == nullptr
          ? impl_->processStarter(impl_->request.bars[impl_->nextBar])
          : impl_->processSelectable(impl_->request.bars[impl_->nextBar]);
  if (!processed.ok()) {
    impl_->restore(saved);
    impl_->fail(processed.error(), timestamp);
    return processed.error();
  }
  ++impl_->nextBar;
  impl_->result.barsProcessed = impl_->nextBar;
  if (impl_->nextBar == impl_->request.bars.size()) {
    impl_->finish();
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
  if (impl_->result.status == results::RunStatus::running || impl_->finalized) {
    return invalidArgument("only one terminal backtest result can be recorded");
  }
  if (!impl_->recordsWritten) {
    auto appended = writer.append(impl_->result.canonicalRecords);
    if (!appended.ok()) {
      return appended.error();
    }
    impl_->recordsWritten = true;
  }
  const auto completed = impl_->result.status == results::RunStatus::completed;
  const auto summary =
      completed ? results::FinalSummary{.finalEquityMicrodollars =
                                            impl_->result.equityMicrodollars,
                                        .pnlMicrodollars =
                                            impl_->result.pnlMicrodollars}
                : results::FinalSummary{};
  std::string reason;
  if (const auto &terminalError = impl_->result.terminalError;
      terminalError.has_value()) {
    reason = terminalError.value().message;
  }
  auto finalized =
      writer.finalizeAndPromote(impl_->result.status, summary, reason);
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
  const auto ignored = execution.value()->runToCompletion(cancellation);
  return execution.value()->finalizeAndRecord(writer);
}

core::Result<RecordedBacktestOutcome>
runBacktestAndRecord(const BacktestRequest &request,
                     const results::ResultStore &store,
                     const results::RunDescriptor &descriptor,
                     const core::CancellationToken &cancellation) {
  if (cancellation.isCancellationRequested()) {
    return cancelled();
  }
  auto execution = BacktestExecution::start(request);
  if (!execution.ok()) {
    return execution.error();
  }
  if (descriptor.universe != std::vector<std::string>{request.symbol} ||
      descriptor.initialCapitalMicrodollars !=
          request.initialCapitalMicrodollars ||
      request.bars.front().ts < descriptor.range.start ||
      request.bars.back().ts >= descriptor.range.end) {
    return invalidArgument(
        "Result descriptor does not identify the validated Backtest request");
  }
  auto writer = store.begin(descriptor);
  if (!writer.ok()) {
    return writer.error();
  }
  const auto ignored = execution.value()->runToCompletion(cancellation);
  return execution.value()->finalizeAndRecord(*writer.value());
}

} // namespace bte::engine
