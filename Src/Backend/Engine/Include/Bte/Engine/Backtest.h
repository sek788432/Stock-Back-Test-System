#pragma once

#include "Bte/Core/Bar.h"
#include "Bte/Core/Cancellation.h"
#include "Bte/Core/Result.h"
#include "Bte/Results/ResultStore.h"
#include "Bte/Strategy/SelectableStrategy.h"
#include "Bte/Strategy/Strategy.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace bte::engine {

inline constexpr std::int64_t maximumStarterQuantityShares =
    strategy::maximumStrategyQuantityShares;

enum class StarterOrderStatus : std::uint8_t {
  filled,
  rejectedInsufficientCash,
  cancelledNoFutureMarketData,
  completedNoSignal,
};

enum class BacktestOrderSide : std::uint8_t { buy, sell };

struct BacktestRequest {
  std::vector<core::Bar> bars;
  /// Optional actual pre-range bars for a strategy's bounded warm-up/history.
  /// They are never executed, persisted, or shown in Replay.
  // NOLINTNEXTLINE(readability-redundant-member-init): aggregate defaults.
  std::vector<core::Bar> historyBars{};
  std::string symbol = "PRIMARY";
  std::int64_t initialCapitalMicrodollars = 0;
  std::int64_t quantityShares = 0;
  std::optional<strategy::SelectableStrategyPlan> selectableStrategy =
      std::nullopt;
  /// Immutable snapshot context for Task 17; it is never processed as a bar.
  std::optional<strategy::PreviousDataDayClose> previousDataDayClose =
      std::nullopt;
  /// Bounded immutable UTC data-day history; warm-up summaries are never bars.
  // NOLINTNEXTLINE(readability-redundant-member-init): aggregate defaults.
  std::vector<strategy::PreviousDataDaySummary> previousDataDays{};
  /// When supplied, a different last actual bar produces an Incomplete
  /// StaleFinalMark diagnostic instead of eligible final metrics.
  std::optional<core::Timestamp> requiredFinalMarkTimestamp = std::nullopt;
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
  results::RunStatus status = results::RunStatus::running;
  bool diagnosticsComplete = true;
  std::optional<core::Error> terminalError;
};

struct RecordedBacktestOutcome {
  std::optional<BacktestResult> backtest;
  results::FinalizedResult persisted;
  results::RunStatus status = results::RunStatus::incomplete;
  std::optional<core::Error> terminalError;
};

/// Owns one validated engine run. Batch execution calls runToCompletion;
/// Paced execution calls advance once for each user-authorized slice. Both
/// paths use the same state transition and canonical-record implementation.
class BacktestExecution final {
private:
  struct ConstructionKey final {};
  struct Impl;

public:
  using RecordedResultObserver =
      std::function<core::Result<void>(const std::string &)>;
  explicit BacktestExecution(ConstructionKey, std::unique_ptr<Impl> impl);
  ~BacktestExecution();
  BacktestExecution(const BacktestExecution &) = delete;
  BacktestExecution &operator=(const BacktestExecution &) = delete;
  BacktestExecution(BacktestExecution &&) = delete;
  BacktestExecution &operator=(BacktestExecution &&) = delete;

  [[nodiscard]] static core::Result<std::unique_ptr<BacktestExecution>>
  start(BacktestRequest request);
  /// Starts the same Engine lifecycle with a caller-owned typed Strategy.
  /// The request must not also contain a Selectable Conditions plan.
  [[nodiscard]] static core::Result<std::unique_ptr<BacktestExecution>>
  start(BacktestRequest request, std::unique_ptr<strategy::Strategy> strategy);
  /// Owns a durable writer; advance commits each complete bar before progress.
  [[nodiscard]] static core::Result<std::unique_ptr<BacktestExecution>>
  startRecorded(BacktestRequest request, const results::ResultStore &store,
                const results::RunDescriptor &descriptor,
                const core::CancellationToken &cancellation = {},
                const RecordedResultObserver &onResultCreated = {});
  [[nodiscard]] static core::Result<std::unique_ptr<BacktestExecution>>
  startRecorded(BacktestRequest request,
                std::unique_ptr<strategy::Strategy> strategy,
                const results::ResultStore &store,
                const results::RunDescriptor &descriptor,
                const core::CancellationToken &cancellation = {},
                const RecordedResultObserver &onResultCreated = {});
  [[nodiscard]] core::Result<void>
  advance(const core::CancellationToken &cancellation = {});
  [[nodiscard]] core::Result<void>
  runToCompletion(const core::CancellationToken &cancellation = {});
  [[nodiscard]] results::RunStatus status() const noexcept;
  [[nodiscard]] const BacktestResult &result() const noexcept;
  [[nodiscard]] const std::optional<core::Error> &
  terminalError() const noexcept;
  [[nodiscard]] core::Result<RecordedBacktestOutcome>
  finalizeAndRecord(results::ResultWriter &writer);
  [[nodiscard]] core::Result<RecordedBacktestOutcome> finalizeRecorded();

private:
  [[nodiscard]] static core::Result<std::unique_ptr<BacktestExecution>>
  startWithStrategy(BacktestRequest request,
                    std::unique_ptr<strategy::Strategy> strategy,
                    bool useLegacySelection);
  [[nodiscard]] static core::Result<std::unique_ptr<BacktestExecution>>
  startRecordedWithStrategy(BacktestRequest request,
                            std::unique_ptr<strategy::Strategy> strategy,
                            const results::ResultStore &store,
                            const results::RunDescriptor &descriptor,
                            const core::CancellationToken &cancellation,
                            const RecordedResultObserver &onResultCreated,
                            bool useLegacySelection);
  // The borrowed writer is attached only inside this synchronous entry point.
  friend core::Result<RecordedBacktestOutcome>
  runBacktestAndRecord(const BacktestRequest &request,
                       results::ResultWriter &writer,
                       const core::CancellationToken &cancellation);
  std::unique_ptr<Impl> impl_;
};

/// Runs either the compatibility starter order or a typed Selectable Conditions
/// plan. Signals become market orders eligible only at the next actual bar.
[[nodiscard]] core::Result<BacktestResult>
runBacktest(const BacktestRequest &request,
            const core::CancellationToken &cancellation = {});

/// Executes an already-described run and finalizes its authoritative timeline
/// as a durable completed or diagnostic draft for explicit publication.
[[nodiscard]] core::Result<RecordedBacktestOutcome>
runBacktestAndRecord(const BacktestRequest &request,
                     results::ResultWriter &writer,
                     const core::CancellationToken &cancellation = {});

/// Preferred complete recording seam. Validation and Strategy construction
/// finish before the writer is created, so rejected configurations leave no
/// staged, draft, or published Result artifact.
[[nodiscard]] core::Result<RecordedBacktestOutcome>
runBacktestAndRecord(const BacktestRequest &request,
                     const results::ResultStore &store,
                     const results::RunDescriptor &descriptor,
                     const core::CancellationToken &cancellation = {});

/// Executes and records a caller-supplied typed Strategy using the same
/// validation-before-writer and per-slice durability rules.
[[nodiscard]] core::Result<RecordedBacktestOutcome> runBacktestAndRecord(
    const BacktestRequest &request,
    std::unique_ptr<strategy::Strategy> strategy,
    const results::ResultStore &store, const results::RunDescriptor &descriptor,
    const core::CancellationToken &cancellation = {},
    const BacktestExecution::RecordedResultObserver &onResultCreated = {});

} // namespace bte::engine
