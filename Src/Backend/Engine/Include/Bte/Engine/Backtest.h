#pragma once

#include "Bte/Core/Bar.h"
#include "Bte/Core/Cancellation.h"
#include "Bte/Core/Result.h"
#include "Bte/Results/ResultStore.h"
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
  std::string symbol = "PRIMARY";
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
  std::vector<results::CanonicalRecord> canonicalRecords;
};

struct RecordedBacktestOutcome {
  std::optional<BacktestResult> backtest;
  results::FinalizedResult persisted;
  results::RunStatus status = results::RunStatus::incomplete;
  std::optional<core::Error> terminalError;
};

/// Runs either the compatibility starter order or a typed Selectable Conditions
/// plan. Signals become market orders eligible only at the next actual bar.
[[nodiscard]] core::Result<BacktestResult>
runBacktest(const BacktestRequest &request,
            const core::CancellationToken &cancellation = {});

/// Executes an already-described run, persists its authoritative timeline, and
/// promotes either a completed Result or a typed diagnostic Result.
[[nodiscard]] core::Result<RecordedBacktestOutcome>
runBacktestAndRecord(const BacktestRequest &request,
                     results::ResultWriter &writer,
                     const core::CancellationToken &cancellation = {});

} // namespace bte::engine
