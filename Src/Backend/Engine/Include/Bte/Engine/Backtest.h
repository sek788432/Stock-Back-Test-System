#pragma once

#include "Bte/Core/Bar.h"
#include "Bte/Core/Cancellation.h"
#include "Bte/Core/Result.h"
#include "Bte/Strategy/SelectableStrategy.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace bte::engine {

inline constexpr std::int64_t maximumStarterQuantityShares = 1'000'000'000;

enum class StarterOrderStatus : std::uint8_t {
  filled,
  rejectedInsufficientCash,
  cancelledNoFutureMarketData,
  completedNoSignal,
};

enum class BacktestOrderSide : std::uint8_t { buy, sell };

struct BacktestRequest {
  std::vector<core::Bar> bars;
  std::int64_t initialCapitalMicrodollars = 0;
  std::int64_t quantityShares = 0;
  std::optional<strategy::SelectableStrategyPlan> selectableStrategy;
};

struct BacktestFill {
  core::Timestamp timestamp;
  BacktestOrderSide side = BacktestOrderSide::buy;
  std::int64_t quantityShares = 0;
  std::int64_t priceNanodollars = 0;
  std::int64_t amountMicrodollars = 0;
};

struct BacktestResult {
  StarterOrderStatus orderStatus =
      StarterOrderStatus::cancelledNoFutureMarketData;
  std::optional<BacktestFill> fill;
  std::vector<BacktestFill> fills;
  std::int64_t initialCapitalMicrodollars = 0;
  std::int64_t cashMicrodollars = 0;
  std::int64_t marketValueMicrodollars = 0;
  std::int64_t equityMicrodollars = 0;
  std::int64_t pnlMicrodollars = 0;
  std::int64_t finalPriceNanodollars = 0;
  std::int64_t positionShares = 0;
  std::size_t barsProcessed = 0;
};

/// Runs either the compatibility starter order or a typed Selectable Conditions
/// plan. Signals become market orders eligible only at the next actual bar.
[[nodiscard]] core::Result<BacktestResult>
runBacktest(const BacktestRequest &request,
            const core::CancellationToken &cancellation = {});

} // namespace bte::engine
