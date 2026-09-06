#pragma once

#include "Bte/Core/Bar.h"
#include "Bte/Core/Cancellation.h"
#include "Bte/Core/Result.h"
#include "Bte/Strategy/SelectableStrategy.h"

#include <QDate>
#include <QString>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace bte::bindings {

enum class BacktestOutcome : std::uint8_t {
  filled,
  rejectedInsufficientCash,
  cancelledNoFutureMarketData,
  completedNoSignal,
};

enum class BacktestFillSide : std::uint8_t { buy, sell };

struct BacktestFillSnapshot {
  core::Timestamp timestamp;
  BacktestFillSide side = BacktestFillSide::buy;
  std::int64_t quantityShares = 0;
  double price = 0.0;
  double amount = 0.0;
};

struct BacktestSnapshot {
  BacktestOutcome outcome = BacktestOutcome::cancelledNoFutureMarketData;
  std::optional<BacktestFillSnapshot> fill;
  std::vector<BacktestFillSnapshot> fills;
  double initialCapital = 0.0;
  double cash = 0.0;
  double marketValue = 0.0;
  double equity = 0.0;
  double pnl = 0.0;
  double finalPrice = 0.0;
  std::int64_t positionShares = 0;
  std::size_t barsProcessed = 0;
  std::string resultId;
  std::string canonicalResultHash;
};

struct BacktestConfiguration {
  QString symbol;
  QString schema;
  QDate startDate;
  QDate endDate;
  double initialCapital = 0.0;
  std::int64_t quantityShares = 0;
  std::optional<strategy::SelectableStrategyPlan> selectableStrategy;
};

struct PersistedBacktestStorage {
  std::filesystem::path resultStore;
  std::filesystem::path dataStore;
  std::string snapshotId;
  std::string strategyHash;
};

[[nodiscard]] core::Result<BacktestSnapshot>
runBacktestSession(std::vector<core::Bar> bars, double initialCapital,
                   std::int64_t quantityShares,
                   const core::CancellationToken &cancellation = {});

[[nodiscard]] core::Result<BacktestSnapshot> runBacktestSession(
    std::vector<core::Bar> bars, double initialCapital,
    std::int64_t quantityShares,
    std::optional<strategy::SelectableStrategyPlan> selectableStrategy,
    const core::CancellationToken &cancellation = {});

[[nodiscard]] core::Result<BacktestSnapshot>
runBacktestConfiguration(const BacktestConfiguration &configuration,
                         const core::CancellationToken &cancellation = {});

[[nodiscard]] core::Result<BacktestSnapshot> runPersistedBacktestConfiguration(
    const BacktestConfiguration &configuration,
    const PersistedBacktestStorage &storage,
    const core::CancellationToken &cancellation = {});

} // namespace bte::bindings
