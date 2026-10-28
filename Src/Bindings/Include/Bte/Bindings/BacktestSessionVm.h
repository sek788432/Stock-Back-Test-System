#pragma once

#include "Bte/Bindings/ResultCatalogSnapshot.h"
#include "Bte/Core/Bar.h"
#include "Bte/Core/Cancellation.h"
#include "Bte/Core/Result.h"
#include "Bte/Results/ReproducibilityStore.h"
#include "Bte/Results/ResultStore.h"
#include "Bte/Strategy/SelectableStrategy.h"

#include <QDate>
#include <QString>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
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
  std::string reproducibilityDiagnostic;
  results::RunStatus status = results::RunStatus::completed;
  std::string terminalReason;
  std::vector<std::string> strategyDiagnostics;
  bool reproducibilityVerified = false;
};

struct PythonStrategyConfiguration {
  std::string sourceUtf8;
  std::string canonicalParametersJson;
  bool sourceConsentGranted = false;
};

struct BacktestConfiguration {
  QString symbol;
  QString schema;
  QDate startDate;
  QDate endDate;
  double initialCapital = 0.0;
  std::int64_t quantityShares = 0;
  std::optional<strategy::SelectableStrategyPlan> selectableStrategy =
      std::nullopt;
  std::optional<PythonStrategyConfiguration> pythonStrategy = std::nullopt;
};

struct PersistedBacktestStorage {
  std::filesystem::path resultStore;
  std::filesystem::path dataStore;
  std::string snapshotId;
  // Kept for source compatibility; provenance is derived canonically from the
  // actual Strategy configuration and this legacy caller value is ignored.
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

struct PythonWorkerPaths {
  std::filesystem::path resourcesRoot;
  std::filesystem::path workerBootstrapPath;
};

[[nodiscard]] core::Result<void>
validatePythonStrategy(const PythonStrategyConfiguration &configuration,
                       const PythonWorkerPaths &workerPaths,
                       const core::CancellationToken &cancellation = {});

using PythonVerificationProgressObserver =
    std::function<void(const results::VerificationEvidence &)>;
[[nodiscard]] core::Result<results::VerificationEvidence>
verifyPersistedPythonBacktestConfiguration(
    const BacktestConfiguration &configuration,
    const PersistedBacktestStorage &storage,
    const std::filesystem::path &applicationResourcesRoot,
    const std::filesystem::path &workerBootstrapPath,
    const PythonVerificationProgressObserver &observeProgress = {},
    const core::CancellationToken &cancellation = {});

[[nodiscard]] core::Result<std::string>
publishBacktestDraft(const PersistedBacktestStorage &storage,
                     const std::string &resultId,
                     const std::string &displayName = {},
                     const core::CancellationToken &cancellation = {});

[[nodiscard]] core::Result<ResultCatalogSnapshot>
listBacktestDraftCatalog(const PersistedBacktestStorage &storage,
                         const core::CancellationToken &cancellation = {});

[[nodiscard]] core::Result<std::vector<std::string>>
listBacktestDrafts(const PersistedBacktestStorage &storage,
                   const core::CancellationToken &cancellation = {});

[[nodiscard]] core::Result<void>
discardBacktestDraft(const PersistedBacktestStorage &storage,
                     const std::string &resultId,
                     const core::CancellationToken &cancellation = {});

} // namespace bte::bindings
