#include "Bte/Bindings/BacktestSessionVm.h"

// IWYU pragma: no_include <math>

#include "../Python/PythonWorkerStrategy.h"
#include "../Python/RuntimeProfile.h"
#include "Bte/Bindings/ReplayDataLoader.h"
#include "Bte/Core/CanonicalJson.h"
#include "Bte/Core/Digest.h"
#include "Bte/Data/ReleaseSnapshot.h"
#include "Bte/Engine/Backtest.h"
#include "Bte/Results/ReproducibilityCoordinator.h"
#include "Bte/Results/ResultStore.h"
#if defined(BTE_ENABLE_TEST_HOOKS)
#include "BacktestSessionVmTestHooks.h"
#endif

#include <algorithm> // IWYU pragma: keep
#if defined(BTE_ENABLE_TEST_HOOKS)
#include <atomic>
#endif
#include <chrono>
#include <cmath>
#include <compare> // IWYU pragma: keep
#include <cstdint>
#include <iterator>
#include <map>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <span>
#include <string> // IWYU pragma: keep
#include <utility>
#include <vector>

#include <QCoreApplication>
#include <QtCore/qtenvironmentvariables.h>

// libstdc++ attributes the ranges algorithm's default projection to this
// header.
#if defined(__GLIBCXX__)
#include <functional>
#endif

namespace bte::bindings {
namespace {

constexpr auto microdollarsPerDollar = 1'000'000.0;
constexpr auto nanodollarsPerDollar = 1'000'000'000.0;
constexpr auto int64ExclusiveUpperBound = 9'223'372'036'854'775'808.0;

#if defined(BTE_ENABLE_TEST_HOOKS)
std::atomic<core::CancellationSource *> cancelAfterDraftOpen = nullptr;

void triggerDraftOpenCancellation() noexcept {
  if (auto *const source = cancelAfterDraftOpen.exchange(nullptr);
      source != nullptr) {
    source->requestCancellation();
  }
}
#endif

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

bool planUsesPreviousDataDayClose(
    const std::optional<strategy::SelectableStrategyPlan> &plan) {
  if (!plan.has_value()) {
    return false;
  }
  const auto containsCondition = [](const strategy::ConditionGroup &group) {
    return std::ranges::any_of(group.conditions, [](const auto &condition) {
      return condition.source ==
             strategy::ConditionSource::previousDataDayCloseChangePercent;
    });
  };
  return containsCondition(plan->buy) || containsCondition(plan->sell);
}

std::int32_t planHistoricalDataDayLookback(
    const std::optional<strategy::SelectableStrategyPlan> &plan) {
  if (!plan.has_value()) {
    return 0;
  }
  auto maximum = std::int32_t{0};
  const auto update = [&maximum](const strategy::ConditionGroup &group) {
    for (const auto &condition : group.conditions) {
      if (condition.source == strategy::ConditionSource::dataDayGainPercent ||
          condition.source ==
              strategy::ConditionSource::periodHighDrawdownPercent ||
          condition.source ==
              strategy::ConditionSource::periodLowReboundPercent ||
          condition.source == strategy::ConditionSource::breakPeriodHigh ||
          condition.source == strategy::ConditionSource::breakPeriodLow) {
        maximum = std::max(maximum, condition.lookbackPeriod);
      }
    }
  };
  update(plan->buy);
  update(plan->sell);
  return maximum;
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
      .status = result.status,
      .terminalReason = result.terminalError.has_value()
                            ? result.terminalError->message
                            : std::string{},
      .strategyDiagnostics = {},
      .reproducibilityVerified = false,
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
  for (const auto &record : result.canonicalRecords) {
    if (record.family == results::RecordFamily::log ||
        record.family == results::RecordFamily::terminalDiagnostic) {
      snapshot.strategyDiagnostics.push_back(record.text);
    }
  }
  return snapshot;
}

struct PreparedPythonBacktest final {
  std::vector<core::Bar> bars;
  results::RunDescriptor descriptor;
  bindings::PythonStrategyConfiguration strategy;
  nlohmann::json parameters;
  python::RuntimeProfile runtime;
  std::filesystem::path dataStore;
  std::filesystem::path workerBootstrapPath;
  QDate startDate;
  std::string symbol;
  std::int64_t quantityShares = 0;
};

std::filesystem::path applicationResourcesRoot() {
  const auto overrideRoot = qEnvironmentVariable("BTE_PYTHON_RESOURCES_ROOT");
  if (!overrideRoot.isEmpty()) {
    return std::filesystem::path{overrideRoot.toStdString()};
  }
  auto applicationPath = std::filesystem::path{
      QCoreApplication::applicationDirPath().toStdString()};
  if (applicationPath.filename() == "MacOS" &&
      applicationPath.parent_path().filename() == "Contents") {
    return applicationPath.parent_path() / "Resources";
  }
  return applicationPath / "Resources";
}

std::filesystem::path
applicationWorkerBootstrapPath(const std::filesystem::path &resourcesRoot) {
  const auto overridePath = qEnvironmentVariable("BTE_PYTHON_WORKER_BOOTSTRAP");
  if (!overridePath.isEmpty()) {
    return std::filesystem::path{overridePath.toStdString()};
  }
  const auto packaged = resourcesRoot / "Python" / "worker_bootstrap.py";
  if (std::filesystem::is_regular_file(packaged)) {
    return packaged;
  }
#if defined(BTE_SOURCE_DIR)
  return std::filesystem::path{BTE_SOURCE_DIR} / "Src" / "Bindings" / "Python" /
         "worker_bootstrap.py";
#else
  return packaged;
#endif
}

core::Result<nlohmann::json>
canonicalPythonParameters(const PythonStrategyConfiguration &configuration) {
  if (configuration.sourceUtf8.empty() ||
      configuration.sourceUtf8.size() > 1024U * 1024U ||
      configuration.sourceUtf8.find('\0') != std::string::npos ||
      configuration.sourceUtf8.find('\r') != std::string::npos ||
      configuration.sourceUtf8.starts_with("\xef\xbb\xbf")) {
    return core::makeError(core::ErrorCode::invalidArgument,
                           "Python Strategy source is not normalized UTF-8");
  }
  core::Result<std::string> sourceJson = core::makeError(
      core::ErrorCode::invalidArgument, "Python Strategy source is invalid");
  try {
    sourceJson =
        core::serializeCanonicalJson(nlohmann::json(configuration.sourceUtf8));
  } catch (const nlohmann::json::exception &) {
    return core::makeError(core::ErrorCode::invalidArgument,
                           "Python Strategy source is not valid UTF-8");
  }
  if (!sourceJson.ok()) {
    return core::makeError(core::ErrorCode::invalidArgument,
                           "Python Strategy source is not valid UTF-8");
  }
  auto parameters =
      core::parseCanonicalJson(configuration.canonicalParametersJson);
  if (!parameters.ok() || !parameters.value().is_object()) {
    return core::makeError(core::ErrorCode::invalidArgument,
                           "Python Strategy parameters must be a JSON object");
  }
  auto canonical = core::serializeCanonicalJson(parameters.value());
  if (!canonical.ok() ||
      canonical.value() != configuration.canonicalParametersJson) {
    return core::makeError(core::ErrorCode::invalidArgument,
                           "Python Strategy parameters must be canonical JSON");
  }
  const auto quantity = parameters.value().find("quantity");
  if (quantity == parameters.value().end() || !quantity->is_number_integer() ||
      quantity->is_boolean()) {
    return core::makeError(
        core::ErrorCode::invalidArgument,
        "Python Strategy quantity must be a positive whole number");
  }
  const auto maximumQuantity =
      static_cast<std::uint64_t>(engine::maximumStarterQuantityShares);
  std::int64_t quantityValue = 0;
  if (quantity->is_number_unsigned()) {
    const auto unsignedQuantity = quantity->get<std::uint64_t>();
    if (unsignedQuantity == 0 || unsignedQuantity > maximumQuantity) {
      return core::makeError(
          core::ErrorCode::invalidArgument,
          "Python Strategy quantity must be a positive whole number");
    }
    quantityValue = static_cast<std::int64_t>(unsignedQuantity);
  } else {
    quantityValue = quantity->get<std::int64_t>();
    if (quantityValue < 1 ||
        quantityValue > engine::maximumStarterQuantityShares) {
      return core::makeError(
          core::ErrorCode::invalidArgument,
          "Python Strategy quantity must be a positive whole number");
    }
  }
  return std::move(parameters).value();
}

results::RuntimeProfileProvenance
resultRuntimeProvenance(const python::RuntimeProfile &runtime) {
  std::vector<results::RuntimePackageIdentity> packages;
  packages.reserve(runtime.packages.size());
  for (const auto &package : runtime.packages) {
    packages.push_back({.name = package.name,
                        .version = package.version,
                        .artifactHash = package.artifactHash});
  }
  return {
      .profileId = runtime.profileId,
      .canonicalManifestJson = runtime.canonicalManifestJson,
      .manifestHash = runtime.manifestHash,
      .pythonVersion = runtime.pythonVersion,
      .pythonAbi = runtime.pythonAbi,
      .packages = std::move(packages),
      .stockbtSourceHash = runtime.stockbtSourceHash,
  };
}

results::RunDescriptor makePythonRunDescriptor(
    const BacktestConfiguration &configuration,
    const PythonStrategyConfiguration &strategyConfiguration,
    const python::RuntimeProfile &runtime,
    const data::SnapshotSelection &selection,
    const std::int64_t initialCapitalMicrodollars) {
  static const std::map<std::string, results::ProvenanceAvailabilityState>
      recordedProvenance{
          {"aggregationPolicy", results::ProvenanceAvailabilityState::recorded},
          {"costs", results::ProvenanceAvailabilityState::recorded},
          {"dataSelection", results::ProvenanceAvailabilityState::recorded},
          {"engineBuild", results::ProvenanceAvailabilityState::recorded},
          {"numericPolicy", results::ProvenanceAvailabilityState::recorded},
          {"runtimePackages", results::ProvenanceAvailabilityState::recorded},
          {"runtimeProfile", results::ProvenanceAvailabilityState::recorded},
          {"seed", results::ProvenanceAvailabilityState::recorded},
          {"strategyApi", results::ProvenanceAvailabilityState::recorded},
          {"strategyArtifact", results::ProvenanceAvailabilityState::recorded},
          {"strategyMode", results::ProvenanceAvailabilityState::recorded},
          {"strategyTemplate", results::ProvenanceAvailabilityState::recorded},
          {"timeframe", results::ProvenanceAvailabilityState::recorded},
      };
  const auto sourceHash = core::sha256(strategyConfiguration.sourceUtf8);
  const auto endDate = configuration.endDate.addDays(1);
  const auto range = core::DateRange{
      .start = timestampFromDate(configuration.startDate),
      .end = timestampFromDate(endDate),
  };
  return {
      .schemaVersion = 3,
      .universe = {configuration.symbol.toStdString()},
      .range = range,
      .initialCapitalMicrodollars = initialCapitalMicrodollars,
      .strategyId = "python-script",
      .strategyHash = sourceHash,
      .dataSelection = selection.identity,
      .provenance =
          {
              .engineBuild = "stockBacktester/" BTE_PROJECT_VERSION,
              .timeframe = selection.identity.timeframe,
              .costProfile = "retailUsEquityV1",
              .commissionRatePartsPerBillion = 0,
              .slippageRatePartsPerBillion = 100'000,
              .seed = 0,
              .strategyMode = results::StrategyMode::python,
              .apiVersion = "1",
              .templateVersion = "1.0.0",
              .canonicalParametersJson =
                  strategyConfiguration.canonicalParametersJson,
              .sourceUtf8 = strategyConfiguration.sourceUtf8,
              .sourceHash = sourceHash,
              .runtime = resultRuntimeProvenance(runtime),
              .provenanceAvailability = recordedProvenance,
          },
  };
}

core::Result<PreparedPythonBacktest>
preparePythonBacktest(const BacktestConfiguration &configuration,
                      const PersistedBacktestStorage &storage,
                      const std::filesystem::path &resourcesRoot,
                      const std::filesystem::path &workerBootstrapPath,
                      const core::CancellationToken &cancellation) {
  if (!configuration.pythonStrategy.has_value() ||
      configuration.schema != "ohlcv-1h" ||
      !configuration.startDate.isValid() || !configuration.endDate.isValid() ||
      configuration.startDate > configuration.endDate ||
      !configuration.endDate.addDays(1).isValid()) {
    return core::makeError(core::ErrorCode::invalidArgument,
                           "Python Backtest configuration is invalid");
  }
  auto parameters = canonicalPythonParameters(*configuration.pythonStrategy);
  if (!parameters.ok()) {
    return parameters.error();
  }
  auto capital = normalizedCapitalMicrodollars(configuration.initialCapital);
  if (!capital.ok()) {
    return capital.error();
  }
  auto runtime = python::discoverManagedRuntime(resourcesRoot);
  if (!runtime.ok()) {
    return runtime.error();
  }
  auto reader = data::ReleaseSnapshotReader::open(
      storage.dataStore, storage.snapshotId, cancellation);
  if (!reader.ok()) {
    return reader.error();
  }
  const auto exclusiveEnd = configuration.endDate.addDays(1);
  auto selected = reader.value()->select(
      {.symbols = {configuration.symbol.toStdString()},
       .range = {.start = timestampFromDate(configuration.startDate),
                 .end = timestampFromDate(exclusiveEnd)},
       .timeframe = "ohlcv-1h"},
      cancellation);
  if (!selected.ok()) {
    return selected.error();
  }
  auto selection = std::move(selected).value();
  if (selection.bars.empty()) {
    return core::makeError(core::ErrorCode::dataUnavailable,
                           "Python Backtest Data Selection is empty");
  }
  std::vector<core::Bar> bars;
  bars.reserve(selection.bars.size());
  for (const auto &bar : selection.bars) {
    bars.push_back({
        .ts = bar.timestamp,
        .open = priceToDollars(bar.openNanodollars),
        .high = priceToDollars(bar.highNanodollars),
        .low = priceToDollars(bar.lowNanodollars),
        .close = priceToDollars(bar.closeNanodollars),
        .volume = static_cast<double>(bar.volumeMicroshares) / 1'000'000.0,
    });
  }
  auto descriptor =
      makePythonRunDescriptor(configuration, *configuration.pythonStrategy,
                              runtime.value(), selection, capital.value());
  const auto quantityShares =
      parameters.value().at("quantity").get<std::int64_t>();
  return PreparedPythonBacktest{
      .bars = std::move(bars),
      .descriptor = std::move(descriptor),
      .strategy = *configuration.pythonStrategy,
      .parameters = std::move(parameters).value(),
      .runtime = std::move(runtime).value(),
      .dataStore = storage.dataStore,
      .workerBootstrapPath = workerBootstrapPath,
      .startDate = configuration.startDate,
      .symbol = configuration.symbol.toStdString(),
      .quantityShares = quantityShares,
  };
}

core::Result<std::vector<core::Bar>> loadPythonWarmupBars(
    const std::filesystem::path &dataStore, const std::string &snapshotId,
    const std::string &symbol, const QDate startDate,
    const std::size_t maximumHistory,
    const core::CancellationToken &cancellation) {
  if (maximumHistory == 0 || maximumHistory > 10'000) {
    return core::makeError(core::ErrorCode::invalidArgument,
                           "Python Strategy history requirement is invalid");
  }
  if (!startDate.isValid()) {
    return core::makeError(core::ErrorCode::invalidArgument,
                           "Python Strategy warm-up range is invalid");
  }
  auto reader = data::ReleaseSnapshotReader::open(dataStore, snapshotId,
                                                   cancellation);
  if (!reader.ok()) {
    return reader.error();
  }
  const auto earliestDate = QDate{1, 1, 1};
  const auto availableDays = earliestDate.daysTo(startDate);
  auto lookbackDays = std::min<qint64>(417, availableDays);
  auto selectedHistory =
      core::Result<data::SnapshotSelection>{core::makeError(
          core::ErrorCode::internal,
          "Python Strategy warm-up selection was not attempted")};
  while (true) {
    if (cancellation.isCancellationRequested()) {
      return core::makeError(core::ErrorCode::cancelled,
                             "Python Strategy warm-up selection canceled");
    }
    const auto historyStart = startDate.addDays(-lookbackDays);
    if (!historyStart.isValid()) {
      return core::makeError(core::ErrorCode::invalidArgument,
                             "Python Strategy warm-up range is invalid");
    }
    selectedHistory = reader.value()->select(
        {.symbols = {symbol},
         .range = {.start = timestampFromDate(historyStart),
                   .end = timestampFromDate(startDate)},
         .timeframe = "ohlcv-1h"},
        cancellation);
    if (!selectedHistory.ok()) {
      return selectedHistory.error();
    }
    if (selectedHistory.value().bars.size() >= maximumHistory ||
        historyStart == earliestDate) {
      break;
    }
    const auto expandedLookback = std::min(availableDays, lookbackDays * 2);
    if (expandedLookback == lookbackDays) {
      break;
    }
    lookbackDays = expandedLookback;
  }
  const auto &historyBars = selectedHistory.value().bars;
  const auto firstHistory = historyBars.size() > maximumHistory
                                ? historyBars.size() - maximumHistory
                                : 0;
  std::vector<core::Bar> warmup;
  warmup.reserve(historyBars.size() - firstHistory);
  for (auto index = firstHistory; index < historyBars.size(); ++index) {
    const auto &bar = historyBars[index];
    warmup.push_back({
        .ts = bar.timestamp,
        .open = priceToDollars(bar.openNanodollars),
        .high = priceToDollars(bar.highNanodollars),
        .low = priceToDollars(bar.lowNanodollars),
        .close = priceToDollars(bar.closeNanodollars),
        .volume = static_cast<double>(bar.volumeMicroshares) / 1'000'000.0,
    });
  }
  return warmup;
}

core::Result<engine::RecordedBacktestOutcome> executePythonAttempt(
    const PreparedPythonBacktest &prepared,
    const PersistedBacktestStorage &storage,
    const core::CancellationToken &cancellation,
    const engine::BacktestExecution::RecordedResultObserver &onResultCreated) {
  auto warmupResult = loadPythonWarmupBars(
      prepared.dataStore, storage.snapshotId, prepared.symbol,
      prepared.startDate, 10'000, cancellation);
  if (!warmupResult.ok()) {
    return warmupResult.error();
  }
  auto warmup = std::move(warmupResult).value();
  auto strategy = python::PythonWorkerStrategy::create(
      prepared.runtime, prepared.workerBootstrapPath,
      prepared.strategy.sourceUtf8, prepared.parameters,
      prepared.strategy.sourceConsentGranted, cancellation, warmup);
  if (!strategy.ok()) {
    return strategy.error();
  }
  const auto maximumHistory = strategy.value()->requirements().maxHistoryBars;
  if (warmup.size() > maximumHistory) {
    warmup.erase(warmup.begin(),
                 warmup.end() - static_cast<std::ptrdiff_t>(maximumHistory));
  }
  auto store = results::ResultStore::open(storage.resultStore,
                                          storage.dataStore, cancellation);
  if (!store.ok()) {
    return store.error();
  }
  return engine::runBacktestAndRecord(
      {.bars = prepared.bars,
       .historyBars = std::move(warmup),
       .symbol = prepared.symbol,
       .initialCapitalMicrodollars =
           prepared.descriptor.initialCapitalMicrodollars,
       .quantityShares = prepared.quantityShares,
       .selectableStrategy = std::nullopt,
       .previousDataDayClose = std::nullopt,
       .previousDataDays = {},
       .requiredFinalMarkTimestamp = std::nullopt},
      std::move(strategy).value(), *store.value(), prepared.descriptor,
      cancellation, onResultCreated);
}

core::Result<BacktestSnapshot>
pythonOutcomeSnapshot(const engine::RecordedBacktestOutcome &recorded) {
  if (!recorded.backtest.has_value()) {
    return core::makeError(core::ErrorCode::internal,
                           "Python Backtest outcome is unavailable");
  }
  auto snapshot = toSnapshot(*recorded.backtest);
  snapshot.resultId = recorded.persisted.resultId;
  snapshot.canonicalResultHash = recorded.persisted.canonicalResultHash;
  snapshot.status = recorded.status;
  if (recorded.terminalError.has_value()) {
    snapshot.terminalReason = recorded.terminalError->message;
  }
  return snapshot;
}

} // namespace

#if defined(BTE_ENABLE_TEST_HOOKS)
namespace testing {

void cancelAfterNextDraftStoreOpen(core::CancellationSource &source) noexcept {
  cancelAfterDraftOpen.store(&source);
}

void clearDraftStoreOpenHook() noexcept { cancelAfterDraftOpen.store(nullptr); }

core::Result<std::vector<core::Bar>> selectPythonWarmupBars(
    const std::filesystem::path &dataStore, const std::string &snapshotId,
    const std::string &symbol, const QDate startDate,
    const std::size_t maximumHistory,
    const core::CancellationToken &cancellation) {
  return loadPythonWarmupBars(dataStore, snapshotId, symbol, startDate,
                              maximumHistory, cancellation);
}

} // namespace testing
#endif

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
      .previousDataDayClose = std::nullopt,
      .previousDataDays = {},
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
  if (configuration.pythonStrategy.has_value()) {
    return core::makeError(core::ErrorCode::invalidArgument,
                           "Python Strategy requires persisted Result storage");
  }
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
  if (configuration.pythonStrategy.has_value()) {
    const auto resourcesRoot = applicationResourcesRoot();
    const auto bootstrap = applicationWorkerBootstrapPath(resourcesRoot);
    auto prepared = preparePythonBacktest(configuration, storage, resourcesRoot,
                                          bootstrap, cancellation);
    if (!prepared.ok()) {
      return prepared.error();
    }
    auto recorded =
        executePythonAttempt(prepared.value(), storage, cancellation, {});
    if (!recorded.ok()) {
      return recorded.error();
    }
    auto snapshot = pythonOutcomeSnapshot(recorded.value());
    if (!snapshot.ok() ||
        recorded.value().status != results::RunStatus::completed) {
      return snapshot;
    }
    auto verificationStore =
        results::VerificationStore::open(storage.resultStore);
    if (!verificationStore.ok()) {
      return verificationStore.error();
    }
    auto verified = verificationStore.value()->hasVerifiedResult(
        prepared.value().descriptor, recorded.value().persisted);
    if (!verified.ok()) {
      return verified.error();
    }
    auto output = std::move(snapshot).value();
    output.reproducibilityVerified = verified.value();
    return output;
  }
  if (configuration.schema != "ohlcv-1h") {
    return core::makeError(core::ErrorCode::schemaMismatch,
                           "Backtest execution accepts ohlcv-1h only");
  }
  const auto exclusiveEnd = configuration.endDate.addDays(1);
  if (!configuration.startDate.isValid() || !configuration.endDate.isValid() ||
      configuration.startDate > configuration.endDate ||
      !exclusiveEnd.isValid()) {
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
  auto selection = std::move(selected).value();
  if (selection.bars.empty()) {
    return core::makeError(core::ErrorCode::dataUnavailable,
                           "Backtest Data Selection is empty");
  }
  std::optional<strategy::PreviousDataDayClose> previousDataDayClose;
  std::vector<strategy::PreviousDataDaySummary> previousDataDays;
  if (planUsesPreviousDataDayClose(configuration.selectableStrategy)) {
    const auto utcDateStart =
        core::Timestamp{std::chrono::floor<std::chrono::days>(
            selection.bars.front().timestamp)};
    auto history = reader.value()->lastCloseBeforeUtcDate(
        configuration.symbol.toStdString(), utcDateStart, cancellation);
    if (!history.ok()) {
      return history.error();
    }
    selection.preRangeDataDayClose = history.value();
    if (selection.preRangeDataDayClose.has_value()) {
      const auto &sourceClose = *selection.preRangeDataDayClose;
      previousDataDayClose = strategy::PreviousDataDayClose{
          .timestamp = sourceClose.timestamp,
          .close = priceToDollars(sourceClose.closeNanodollars),
      };
    }
  }
  const auto historyLookback =
      planHistoricalDataDayLookback(configuration.selectableStrategy);
  if (historyLookback > 0) {
    const auto utcDateStart =
        core::Timestamp{std::chrono::floor<std::chrono::days>(
            selection.bars.front().timestamp)};
    auto history = reader.value()->lastDataDaysBeforeUtcDate(
        configuration.symbol.toStdString(), utcDateStart,
        static_cast<std::size_t>(historyLookback), cancellation);
    if (!history.ok()) {
      return history.error();
    }
    selection.preRangeDataDays = history.value();
    previousDataDays.reserve(selection.preRangeDataDays.size());
    for (const auto &day : selection.preRangeDataDays) {
      previousDataDays.push_back(strategy::PreviousDataDaySummary{
          .utcDateStart = day.utcDateStart,
          .high = priceToDollars(day.highNanodollars),
          .low = priceToDollars(day.lowNanodollars),
          .close = priceToDollars(day.closeNanodollars),
      });
    }
  }
  std::vector<core::Bar> bars;
  bars.reserve(selection.bars.size());
  for (const auto &bar : selection.bars) {
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
  const auto descriptor = results::RunDescriptor{
      .universe = {configuration.symbol.toStdString()},
      .range = {.start = timestampFromDate(configuration.startDate),
                .end = timestampFromDate(exclusiveEnd)},
      .initialCapitalMicrodollars = capital.value(),
      .strategyId = configuration.selectableStrategy.has_value()
                        ? "selectable-conditions"
                        : "starter",
      .strategyHash =
          strategy::canonicalStrategyHash(configuration.selectableStrategy),
      .dataSelection = selection.identity,
  };
  auto recorded = engine::runBacktestAndRecord(
      {.bars = std::move(bars),
       .symbol = configuration.symbol.toStdString(),
       .initialCapitalMicrodollars = capital.value(),
       .quantityShares = configuration.quantityShares,
       .selectableStrategy = configuration.selectableStrategy,
       .previousDataDayClose = previousDataDayClose,
       .previousDataDays = std::move(previousDataDays)},
      *store.value(), descriptor, cancellation);
  if (!recorded.ok()) {
    return recorded.error();
  }
  if (recorded.value().status != results::RunStatus::completed) {
    return recorded.value().terminalError.value_or(core::makeError(
        core::ErrorCode::internal, "Recorded Backtest did not complete"));
  }
  // Successful completed recordings always carry their Backtest value.
  // GCOVR_EXCL_START
  if (!recorded.value().backtest.has_value()) {
    return core::makeError(core::ErrorCode::internal,
                           "Completed Backtest result is unavailable");
  }
  // GCOVR_EXCL_STOP
  // NOLINTNEXTLINE(bugprone-unchecked-optional-access): guarded above
  auto snapshot = toSnapshot(recorded.value().backtest.value());
  snapshot.resultId = recorded.value().persisted.resultId;
  snapshot.canonicalResultHash = recorded.value().persisted.canonicalResultHash;
  return snapshot;
}

core::Result<void>
validatePythonStrategy(const PythonStrategyConfiguration &configuration,
                       const std::filesystem::path &resourcesRoot,
                       const std::filesystem::path &workerBootstrapPath,
                       const core::CancellationToken &cancellation) {
  if (cancellation.isCancellationRequested()) {
    return core::makeError(core::ErrorCode::cancelled,
                           "Python Strategy validation was canceled");
  }
  auto parameters = canonicalPythonParameters(configuration);
  if (!parameters.ok()) {
    return parameters.error();
  }
  auto runtime = python::discoverManagedRuntime(resourcesRoot);
  if (!runtime.ok()) {
    return runtime.error();
  }
  auto strategy = python::PythonWorkerStrategy::create(
      runtime.value(), workerBootstrapPath, configuration.sourceUtf8,
      std::move(parameters).value(), configuration.sourceConsentGranted,
      cancellation);
  if (!strategy.ok()) {
    return strategy.error();
  }
  if (cancellation.isCancellationRequested()) {
    return core::makeError(core::ErrorCode::cancelled,
                           "Python Strategy validation was canceled");
  }
  return {};
}

core::Result<results::VerificationEvidence>
verifyPersistedPythonBacktestConfiguration(
    const BacktestConfiguration &configuration,
    const PersistedBacktestStorage &storage,
    const std::filesystem::path &resourcesRoot,
    const std::filesystem::path &workerBootstrapPath,
    const PythonVerificationProgressObserver &observeProgress,
    const core::CancellationToken &cancellation) {
  if (!configuration.pythonStrategy.has_value()) {
    return core::makeError(
        core::ErrorCode::invalidArgument,
        "Reproducibility verification requires Python Strategy");
  }
  auto prepared = preparePythonBacktest(configuration, storage, resourcesRoot,
                                        workerBootstrapPath, cancellation);
  if (!prepared.ok()) {
    return prepared.error();
  }
  auto verificationStore =
      results::VerificationStore::open(storage.resultStore);
  if (!verificationStore.ok()) {
    return verificationStore.error();
  }
  results::ReproducibilityCoordinator coordinator{*verificationStore.value()};
  const auto &ready = prepared.value();
  const results::VerificationAttemptRunner runAttempt =
      [&ready,
       &storage](const results::RunDescriptor &frozenDescriptor,
                 const core::CancellationToken &attemptCancellation,
                 const results::VerificationResultIdPublisher &publishResultId)
      -> core::Result<results::VerificationAttemptOutput> {
    auto recorded = executePythonAttempt(ready, storage, attemptCancellation,
                                         publishResultId);
    if (!recorded.ok()) {
      return recorded.error();
    }
    return results::VerificationAttemptOutput{
        .descriptor = frozenDescriptor,
        .result = recorded.value().persisted,
        .status = recorded.value().status,
    };
  };
  return coordinator.verify(prepared.value().descriptor, runAttempt,
                            cancellation, observeProgress);
}

core::Result<std::string>
publishBacktestDraft(const PersistedBacktestStorage &storage,
                     const std::string &resultId,
                     const std::string &displayName,
                     const core::CancellationToken &cancellation) {
  if (cancellation.isCancellationRequested()) {
    return core::makeError(core::ErrorCode::cancelled,
                           "Backtest save was cancelled");
  }
  auto store =
      results::ResultStore::open(storage.resultStore, storage.dataStore);
  if (!store.ok()) {
    return store.error();
  }
#if defined(BTE_ENABLE_TEST_HOOKS)
  triggerDraftOpenCancellation();
#endif
  if (cancellation.isCancellationRequested()) {
    return core::makeError(core::ErrorCode::cancelled,
                           "Backtest save was cancelled");
  }
  auto published = store.value()->publishDraft(resultId, displayName);
  if (!published.ok()) {
    return published.error();
  }
  return published.value().resultId;
}

core::Result<ResultCatalogSnapshot>
listBacktestDraftCatalog(const PersistedBacktestStorage &storage,
                         const core::CancellationToken &cancellation) {
  if (cancellation.isCancellationRequested()) {
    return core::makeError(core::ErrorCode::cancelled,
                           "Backtest draft list was cancelled");
  }
  if (!std::filesystem::exists(storage.resultStore)) {
    return ResultCatalogSnapshot{};
  }
  auto store = results::ResultStore::open(storage.resultStore,
                                          storage.dataStore, cancellation);
  if (!store.ok()) {
    return store.error();
  }
#if defined(BTE_ENABLE_TEST_HOOKS)
  triggerDraftOpenCancellation();
#endif
  auto drafts = store.value()->listDrafts(cancellation);
  if (!drafts.ok()) {
    return drafts.error();
  } // GCOVR_EXCL_LINE: the compiler emits no executable instruction here.
  return makeResultCatalogSnapshot(std::move(drafts).value(),
                                   store.value()->recoveryNotices());
}

core::Result<std::vector<std::string>>
listBacktestDrafts(const PersistedBacktestStorage &storage,
                   const core::CancellationToken &cancellation) {
  auto catalog = listBacktestDraftCatalog(storage, cancellation);
  if (!catalog.ok()) {
    return catalog.error();
  }
  std::vector<std::string> resultIds;
  resultIds.reserve(catalog.value().entries.size());
  std::ranges::transform(catalog.value().entries, std::back_inserter(resultIds),
                         &ResultCatalogEntry::resultId);
  return resultIds;
}

core::Result<void>
discardBacktestDraft(const PersistedBacktestStorage &storage,
                     const std::string &resultId,
                     const core::CancellationToken &cancellation) {
  if (cancellation.isCancellationRequested()) {
    return core::makeError(core::ErrorCode::cancelled,
                           "Backtest draft discard was cancelled");
  }
  auto store = results::ResultStore::open(storage.resultStore,
                                          storage.dataStore, cancellation);
  if (!store.ok()) {
    return store.error();
  }
#if defined(BTE_ENABLE_TEST_HOOKS)
  triggerDraftOpenCancellation();
#endif
  if (cancellation.isCancellationRequested()) {
    return core::makeError(core::ErrorCode::cancelled,
                           "Backtest draft discard was cancelled");
  }
  return store.value()->discardDraft(resultId);
}

} // namespace bte::bindings
