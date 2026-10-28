#include "Bte/Bindings/BacktestSessionVm.h"
#include "Bte/Bindings/ResultCatalogSnapshot.h"
#include "Bte/Bindings/ResultReplay.h"

#include "Bte/Core/Bar.h"
#include "Bte/Data/ReleaseSnapshot.h"

#include "BacktestSessionVmTestHooks.h"
#include "ReleaseSnapshotTestHooks.h"

#include <QByteArray>
#include <QTemporaryDir>
#include <QTest>

#include <algorithm>
#include <array>
#include <cfenv>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace {

bte::core::Bar makeBar(const int day, const double open, const double close) {
  return bte::core::Bar{
      .ts = bte::core::Timestamp{std::chrono::sys_days{std::chrono::year{2024} /
                                                       1 / day}},
      .open = open,
      .high = std::max(open, close) + 1.0,
      .low = std::min(open, close) - 1.0,
      .close = close,
      .volume = 1'000'000.0,
  };
}

class ScopedEnvironmentVariable final {
public:
  ScopedEnvironmentVariable(const QByteArray &name, const QByteArray &value)
      : name_{name}, wasSet_{qEnvironmentVariableIsSet(name.constData())},
        previous_{qgetenv(name.constData())},
        setSuccessfully_{qputenv(name.constData(), value)} {}

  ~ScopedEnvironmentVariable() {
    if (wasSet_) {
      qputenv(name_.constData(), previous_);
    } else {
      qunsetenv(name_.constData());
    }
  }

  [[nodiscard]] bool setSuccessfully() const noexcept {
    return setSuccessfully_;
  }

private:
  QByteArray name_;
  bool wasSet_ = false;
  QByteArray previous_;
  bool setSuccessfully_ = false;
};

class BacktestSessionVmTest final : public QObject {
  Q_OBJECT

private slots:
  void runPresentsFilledStarterBacktest();
  void invalidCapitalReturnsError();
  void oneBarPresentsNoFutureDataOutcome();
  void insufficientCashPresentsRejectedOutcome();
  void capitalNormalizationDoesNotDependOnFloatingPointRoundingMode();
  void capitalOutsideSafeIntegerRangeReturnsError();
  void capitalImmediatelyBelowSafeIntegerLimitIsAccepted();
  void invalidQuantityAndEmptyBarsPreserveEngineErrors();
  void configuredRunPreservesDataSourceError();
  void persistedRunPreservesHistoryLookupCancellation();
  void persistedRunUsesPreviousUtcDataDayCloseFromSnapshot();
  void persistedRunRejectsAnEmptySnapshotSelection();
  void persistedRunPreservesHistoricalDataDayLookupCancellation();
  void filledSnapshotPreservesNextBarTimestamp();
  void capitalUsesHalfEvenRoundingAtTheMinimumBoundary();
  void selectablePlanPresentsBothBuyAndSellFills();
  void selectablePlanPresentsNoSignalOutcome();
  void invalidSelectablePlanPreservesCompileError();
  void configuredRunComposesTrackedDataAndEngine();
  void pythonWarmupSelectsActualBarsBeyondTheInitialCalendarWindow();
  void pythonWarmupRejectsInvalidHistoryLimitsAndDates();
  void pythonPreparationRejectsInvalidInputsBeforeRuntimeDiscovery();
  void persistedPythonRunCanBeSavedAndReplayed();
  void draftLifecycleBoundariesReturnStructuredResults();
  void catalogSnapshotTranslatesEntriesAndEveryRecoveryReason();
  void draftCatalogKeepsRecoveryNoticesWithoutMutatingLegacyFiles();
};

void BacktestSessionVmTest::runPresentsFilledStarterBacktest() {
  const auto result = bte::bindings::runBacktestSession(
      {makeBar(2, 100.0, 100.0), makeBar(3, 110.0, 120.0)}, 2'000.0, 10);

  QVERIFY(result.ok());
  QCOMPARE(result.value().outcome, bte::bindings::BacktestOutcome::filled);
  QCOMPARE(result.value().positionShares, 10);
  QCOMPARE(result.value().cash, 899.89);
  QCOMPARE(result.value().marketValue, 1'200.0);
  QCOMPARE(result.value().equity, 2'099.89);
  QCOMPARE(result.value().pnl, 99.89);
  QCOMPARE(result.value().barsProcessed, 2U);
  QVERIFY(result.value().fill.has_value());
  QCOMPARE(result.value().fill->price, 110.011);
  QCOMPARE(result.value().fill->amount, 1'100.11);
  QCOMPARE(result.value().fill->timestamp, makeBar(3, 110.0, 120.0).ts);
}

void BacktestSessionVmTest::invalidCapitalReturnsError() {
  const auto bars =
      std::vector{makeBar(2, 100.0, 100.0), makeBar(3, 101.0, 102.0)};

  const auto negative = bte::bindings::runBacktestSession(bars, -1.0, 1);
  const auto notFinite = bte::bindings::runBacktestSession(
      bars, std::numeric_limits<double>::quiet_NaN(), 1);

  QVERIFY(!negative.ok());
  QVERIFY(!notFinite.ok());
  QCOMPARE(negative.error().code, bte::core::ErrorCode::invalidArgument);
  QCOMPARE(notFinite.error().code, bte::core::ErrorCode::invalidArgument);
}

void BacktestSessionVmTest::oneBarPresentsNoFutureDataOutcome() {
  const auto result =
      bte::bindings::runBacktestSession({makeBar(2, 100.0, 105.0)}, 1'000.0, 1);

  QVERIFY(result.ok());
  QCOMPARE(result.value().outcome,
           bte::bindings::BacktestOutcome::cancelledNoFutureMarketData);
  QCOMPARE(result.value().positionShares, 0);
  QCOMPARE(result.value().equity, 1'000.0);
  QVERIFY(!result.value().fill.has_value());
}

void BacktestSessionVmTest::insufficientCashPresentsRejectedOutcome() {
  const auto result = bte::bindings::runBacktestSession(
      {makeBar(2, 100.0, 100.0), makeBar(3, 200.0, 200.0)}, 100.0, 1);

  QVERIFY(result.ok());
  QCOMPARE(result.value().outcome,
           bte::bindings::BacktestOutcome::rejectedInsufficientCash);
  QCOMPARE(result.value().cash, 100.0);
  QCOMPARE(result.value().positionShares, 0);
  QVERIFY(!result.value().fill.has_value());
}

void BacktestSessionVmTest::
    capitalNormalizationDoesNotDependOnFloatingPointRoundingMode() {
  const auto originalMode = std::fegetround();
  QVERIFY(std::fesetround(FE_UPWARD) == 0);

  const auto result = bte::bindings::runBacktestSession(
      {makeBar(2, 2.0, 2.0), makeBar(3, 2.0, 2.0)}, 1'000.0000004, 1);

  QVERIFY(std::fesetround(originalMode) == 0);
  QVERIFY(result.ok());
  QCOMPARE(result.value().initialCapital, 1'000.0);
}

void BacktestSessionVmTest::capitalOutsideSafeIntegerRangeReturnsError() {
  const auto unsafeBoundary =
      static_cast<double>(std::numeric_limits<std::int64_t>::max()) /
      1'000'000.0;

  const auto result = bte::bindings::runBacktestSession(
      {makeBar(2, 2.0, 2.0), makeBar(3, 2.0, 2.0)}, unsafeBoundary, 1);

  QVERIFY(!result.ok());
  QCOMPARE(result.error().code, bte::core::ErrorCode::invalidArgument);
}

void BacktestSessionVmTest::
    capitalImmediatelyBelowSafeIntegerLimitIsAccepted() {
  const auto unsafeBoundary =
      static_cast<double>(std::numeric_limits<std::int64_t>::max()) /
      1'000'000.0;
  const auto safeCapital = std::nextafter(unsafeBoundary, 0.0);

  const auto result = bte::bindings::runBacktestSession(
      {makeBar(2, 2.0, 2.0), makeBar(3, 2.0, 2.0)}, safeCapital, 1);

  QVERIFY(result.ok());
}

void BacktestSessionVmTest::invalidQuantityAndEmptyBarsPreserveEngineErrors() {
  const auto bars = std::vector{makeBar(2, 2.0, 2.0), makeBar(3, 2.0, 2.0)};

  const auto invalidQuantity =
      bte::bindings::runBacktestSession(bars, 1'000.0, 0);
  const auto emptyBars = bte::bindings::runBacktestSession({}, 1'000.0, 1);

  QVERIFY(!invalidQuantity.ok());
  QVERIFY(!emptyBars.ok());
  QCOMPARE(invalidQuantity.error().code, bte::core::ErrorCode::invalidArgument);
  QCOMPARE(emptyBars.error().code, bte::core::ErrorCode::invalidArgument);
}

void BacktestSessionVmTest::configuredRunPreservesDataSourceError() {
  const auto result = bte::bindings::runBacktestConfiguration({
      .symbol = "NOT_A_TRACKED_SYMBOL",
      .schema = "ohlcv-1h",
      .startDate = QDate{2024, 1, 1},
      .endDate = QDate{2024, 1, 2},
      .initialCapital = 1'000.0,
      .quantityShares = 1,
      .selectableStrategy = {},
  });

  QVERIFY(!result.ok());
  QCOMPARE(result.error().code, bte::core::ErrorCode::notFound);
  QVERIFY(QString::fromStdString(result.error().message)
              .contains("NOT_A_TRACKED_SYMBOL"));
}

void BacktestSessionVmTest::persistedRunPreservesHistoryLookupCancellation() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const auto root = std::filesystem::path{directory.path().toStdString()};
  const auto sourceDirectory = root / "Source";
  std::filesystem::create_directories(sourceDirectory);
  std::ofstream source{sourceDirectory / "SYN.csv"};
  source << "symbol,ts,open,high,low,close,volume,schemaName\n"
            "SYN,2024-01-01 23:00:00+00:00,100,102,99,101,1200,ohlcv-1h\n"
            "SYN,2024-01-02 00:00:00+00:00,101,103,100,102,2300,ohlcv-1h\n";
  source.close();
  const auto built = bte::data::buildReleaseSnapshot({
      .sourceDirectory = sourceDirectory,
      .storeDirectory = root / "Data",
      .symbols = {"SYN"},
      .rowsPerSegment = 1,
      .calendarHash = std::string(64, 'a'),
      .splitManifestHash = std::string(64, 'b'),
  });
  QVERIFY2(built.ok(), built.error().message.c_str());
  const auto storage = bte::bindings::PersistedBacktestStorage{
      .resultStore = root / "Results",
      .dataStore = root / "Data",
      .snapshotId = built.value().snapshotId,
      .strategyHash = {},
  };
  const auto plan = bte::strategy::SelectableStrategyPlan{
      .buy = {.conditions = {bte::strategy::Condition{
                  .source = bte::strategy::ConditionSource::
                      previousDataDayCloseChangePercent,
                  .comparison = bte::strategy::Comparison::lessThanOrEqual,
                  .threshold = -5.0,
                  .thresholdDomain = bte::indicators::NumericDomain::percent,
              }}},
      .sell = {},
  };
  const auto configuration = bte::bindings::BacktestConfiguration{
      .symbol = "SYN",
      .schema = "ohlcv-1h",
      .startDate = QDate{2024, 1, 2},
      .endDate = QDate{2024, 1, 2},
      .initialCapital = 1'000.0,
      .quantityShares = 1,
      .selectableStrategy = plan,
  };

  bte::data::testing::failSnapshotAt(
      bte::data::testing::SnapshotFailurePoint::historyLookupCancellation);
  const auto result =
      bte::bindings::runPersistedBacktestConfiguration(configuration, storage);
  bte::data::testing::clearSnapshotFailure();

  QVERIFY(!result.ok());
  QCOMPARE(result.error().code, bte::core::ErrorCode::cancelled);
  QVERIFY(!std::filesystem::exists(storage.resultStore));
}

void BacktestSessionVmTest::
    persistedRunUsesPreviousUtcDataDayCloseFromSnapshot() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const auto root = std::filesystem::path{directory.path().toStdString()};
  const auto sourceDirectory = root / "Source";
  std::filesystem::create_directories(sourceDirectory);
  std::ofstream source{sourceDirectory / "SYN.csv"};
  source << "symbol,ts,open,high,low,close,volume,schemaName\n"
            "SYN,2024-01-01 23:00:00+00:00,100,101,99,100,1200,ohlcv-1h\n"
            "SYN,2024-01-02 00:00:00+00:00,95,96,94,95,2300,ohlcv-1h\n"
            "SYN,2024-01-02 01:00:00+00:00,96,98,95,97,3400,ohlcv-1h\n";
  source.close();
  const auto built = bte::data::buildReleaseSnapshot({
      .sourceDirectory = sourceDirectory,
      .storeDirectory = root / "Data",
      .symbols = {"SYN"},
      .rowsPerSegment = 1,
      .calendarHash = std::string(64, 'a'),
      .splitManifestHash = std::string(64, 'b'),
  });
  QVERIFY2(built.ok(), built.error().message.c_str());
  const auto storage = bte::bindings::PersistedBacktestStorage{
      .resultStore = root / "Results",
      .dataStore = root / "Data",
      .snapshotId = built.value().snapshotId,
      .strategyHash = {},
  };
  const auto plan = bte::strategy::SelectableStrategyPlan{
      .buy = {.conditions = {bte::strategy::Condition{
                  .source = bte::strategy::ConditionSource::
                      previousDataDayCloseChangePercent,
                  .comparison = bte::strategy::Comparison::lessThanOrEqual,
                  .threshold = -5.0,
                  .thresholdDomain = bte::indicators::NumericDomain::percent,
              }}},
      .sell = {},
  };
  const auto configuration = bte::bindings::BacktestConfiguration{
      .symbol = "SYN",
      .schema = "ohlcv-1h",
      .startDate = QDate{2024, 1, 2},
      .endDate = QDate{2024, 1, 2},
      .initialCapital = 1'000.0,
      .quantityShares = 1,
      .selectableStrategy = plan,
  };

  const auto result =
      bte::bindings::runPersistedBacktestConfiguration(configuration, storage);

  QVERIFY2(result.ok(), result.error().message.c_str());
  QCOMPARE(result.value().outcome, bte::bindings::BacktestOutcome::filled);
  QVERIFY(result.value().fill.has_value());
  QCOMPARE(result.value().fill->side, bte::bindings::BacktestFillSide::buy);
  QCOMPARE(result.value().fill->timestamp,
           bte::core::Timestamp{
               std::chrono::sys_days{std::chrono::year{2024} / 1 / 2} +
               std::chrono::hours{1}});
  QVERIFY(result.value().fill->price > 96.0);
}

void BacktestSessionVmTest::persistedRunRejectsAnEmptySnapshotSelection() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const auto root = std::filesystem::path{directory.path().toStdString()};
  const auto sourceDirectory = root / "Source";
  std::filesystem::create_directories(sourceDirectory);
  std::ofstream source{sourceDirectory / "SYN.csv"};
  source << "symbol,ts,open,high,low,close,volume,schemaName\n"
            "SYN,2024-01-02 00:00:00+00:00,100,101,99,100,1200,ohlcv-1h\n";
  source.close();
  const auto built = bte::data::buildReleaseSnapshot({
      .sourceDirectory = sourceDirectory,
      .storeDirectory = root / "Data",
      .symbols = {"SYN"},
      .rowsPerSegment = 1,
      .calendarHash = std::string(64, 'a'),
      .splitManifestHash = std::string(64, 'b'),
  });
  QVERIFY2(built.ok(), built.error().message.c_str());
  const auto storage = bte::bindings::PersistedBacktestStorage{
      .resultStore = root / "Results",
      .dataStore = root / "Data",
      .snapshotId = built.value().snapshotId,
      .strategyHash = {},
  };
  const auto configuration = bte::bindings::BacktestConfiguration{
      .symbol = "ABSENT",
      .schema = "ohlcv-1h",
      .startDate = QDate{2024, 1, 2},
      .endDate = QDate{2024, 1, 2},
      .initialCapital = 1'000.0,
      .quantityShares = 1,
      .selectableStrategy = {},
  };

  const auto result =
      bte::bindings::runPersistedBacktestConfiguration(configuration, storage);

  QVERIFY(!result.ok());
  QCOMPARE(result.error().code, bte::core::ErrorCode::dataUnavailable);
  QVERIFY(!std::filesystem::exists(storage.resultStore));
}

void BacktestSessionVmTest::
    persistedRunPreservesHistoricalDataDayLookupCancellation() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const auto root = std::filesystem::path{directory.path().toStdString()};
  const auto sourceDirectory = root / "Source";
  std::filesystem::create_directories(sourceDirectory);
  std::ofstream source{sourceDirectory / "SYN.csv"};
  source << "symbol,ts,open,high,low,close,volume,schemaName\n"
            "SYN,2024-01-01 23:00:00+00:00,100,102,99,101,1200,ohlcv-1h\n"
            "SYN,2024-01-02 00:00:00+00:00,101,103,100,102,2300,ohlcv-1h\n";
  source.close();
  const auto built = bte::data::buildReleaseSnapshot({
      .sourceDirectory = sourceDirectory,
      .storeDirectory = root / "Data",
      .symbols = {"SYN"},
      .rowsPerSegment = 1,
      .calendarHash = std::string(64, 'a'),
      .splitManifestHash = std::string(64, 'b'),
  });
  QVERIFY2(built.ok(), built.error().message.c_str());
  const auto storage = bte::bindings::PersistedBacktestStorage{
      .resultStore = root / "Results",
      .dataStore = root / "Data",
      .snapshotId = built.value().snapshotId,
      .strategyHash = {},
  };
  const auto plan = bte::strategy::SelectableStrategyPlan{
      .buy = {.conditions = {bte::strategy::Condition{
                  .source = bte::strategy::ConditionSource::dataDayGainPercent,
                  .comparison = bte::strategy::Comparison::greaterThanOrEqual,
                  .threshold = 1.0,
                  .thresholdDomain = bte::indicators::NumericDomain::percent,
                  .lookbackPeriod = 1,
              }}},
      .sell = {},
  };
  const auto configuration = bte::bindings::BacktestConfiguration{
      .symbol = "SYN",
      .schema = "ohlcv-1h",
      .startDate = QDate{2024, 1, 2},
      .endDate = QDate{2024, 1, 2},
      .initialCapital = 1'000.0,
      .quantityShares = 1,
      .selectableStrategy = plan,
  };

  bte::data::testing::failSnapshotAt(
      bte::data::testing::SnapshotFailurePoint::historyLookupCancellation);
  const auto result =
      bte::bindings::runPersistedBacktestConfiguration(configuration, storage);
  bte::data::testing::clearSnapshotFailure();

  QVERIFY(!result.ok());
  QCOMPARE(result.error().code, bte::core::ErrorCode::cancelled);
  QVERIFY(!std::filesystem::exists(storage.resultStore));
}

void BacktestSessionVmTest::filledSnapshotPreservesNextBarTimestamp() {
  const auto nextBar = makeBar(7, 110.0, 120.0);

  const auto result = bte::bindings::runBacktestSession(
      {makeBar(6, 100.0, 100.0), nextBar}, 2'000.0, 10);

  QVERIFY(result.ok());
  QVERIFY(result.value().fill.has_value());
  QCOMPARE(result.value().fill->timestamp, nextBar.ts);
}

void BacktestSessionVmTest::capitalUsesHalfEvenRoundingAtTheMinimumBoundary() {
  const auto bars = std::vector{makeBar(2, 2.0, 2.0), makeBar(3, 2.0, 2.0)};
  const auto tieToEven = bte::bindings::runBacktestSession(bars, 0.0000025, 1);
  const auto tieToOdd = bte::bindings::runBacktestSession(bars, 0.0000035, 1);
  const auto roundsToZero =
      bte::bindings::runBacktestSession(bars, 0.0000005, 1);
  const auto roundsToMinimum =
      bte::bindings::runBacktestSession(bars, 0.0000005000001, 1);

  QVERIFY(tieToEven.ok());
  QCOMPARE(tieToEven.value().initialCapital, 0.000002);
  QVERIFY(tieToOdd.ok());
  QCOMPARE(tieToOdd.value().initialCapital, 0.000004);
  QVERIFY(!roundsToZero.ok());
  QCOMPARE(roundsToZero.error().code, bte::core::ErrorCode::invalidArgument);
  QVERIFY(roundsToMinimum.ok());
  QCOMPARE(roundsToMinimum.value().initialCapital, 0.000001);
}

void BacktestSessionVmTest::selectablePlanPresentsBothBuyAndSellFills() {
  const auto plan = bte::strategy::SelectableStrategyPlan{
      .buy =
          {
              .conditions = {bte::strategy::Condition{
                  .source = bte::strategy::ConditionSource::closeChangePercent,
                  .comparison = bte::strategy::Comparison::greaterThan,
                  .threshold = 5.0,
                  .thresholdDomain = bte::indicators::NumericDomain::percent,
              }},
          },
      .sell =
          {
              .conditions = {bte::strategy::Condition{
                  .source = bte::strategy::ConditionSource::closeChangePercent,
                  .comparison = bte::strategy::Comparison::lessThan,
                  .threshold = -5.0,
                  .thresholdDomain = bte::indicators::NumericDomain::percent,
              }},
          },
  };
  const auto result = bte::bindings::runBacktestSession(
      {makeBar(2, 100.0, 100.0), makeBar(3, 110.0, 110.0),
       makeBar(4, 100.0, 100.0), makeBar(5, 90.0, 90.0)},
      2'000.0, 10, plan);

  QVERIFY(result.ok());
  QCOMPARE(result.value().outcome, bte::bindings::BacktestOutcome::filled);
  QCOMPARE(result.value().fills.size(), 2U);
  QCOMPARE(result.value().fills[0].side, bte::bindings::BacktestFillSide::buy);
  QCOMPARE(result.value().fills[1].side, bte::bindings::BacktestFillSide::sell);
  QCOMPARE(result.value().positionShares, 0);
  QCOMPARE(result.value().cash, 1'899.81);
}

void BacktestSessionVmTest::selectablePlanPresentsNoSignalOutcome() {
  const auto plan = bte::strategy::SelectableStrategyPlan{
      .buy =
          {
              .conditions = {bte::strategy::Condition{
                  .source = bte::strategy::ConditionSource::barField,
                  .comparison = bte::strategy::Comparison::greaterThan,
                  .threshold = 1'000.0,
              }},
          },
      .sell = {},
  };
  const auto result = bte::bindings::runBacktestSession(
      {makeBar(2, 100.0, 100.0), makeBar(3, 101.0, 101.0)}, 1'000.0, 1, plan);

  QVERIFY(result.ok());
  QCOMPARE(result.value().outcome,
           bte::bindings::BacktestOutcome::completedNoSignal);
  QVERIFY(result.value().fills.empty());
}

void BacktestSessionVmTest::invalidSelectablePlanPreservesCompileError() {
  const auto result = bte::bindings::runBacktestSession(
      {makeBar(2, 100.0, 100.0), makeBar(3, 101.0, 101.0)}, 1'000.0, 1,
      bte::strategy::SelectableStrategyPlan{});

  QVERIFY(!result.ok());
  QCOMPARE(result.error().code, bte::core::ErrorCode::strategyCompileFailed);
}

void BacktestSessionVmTest::configuredRunComposesTrackedDataAndEngine() {
  const auto result = bte::bindings::runBacktestConfiguration({
      .symbol = "AAPL",
      .schema = "ohlcv-1h",
      .startDate = QDate{2018, 5, 1},
      .endDate = QDate{2018, 5, 3},
      .initialCapital = 100'000.0,
      .quantityShares = 7,
      .selectableStrategy = {},
  });

  QVERIFY(result.ok());
  QCOMPARE(result.value().outcome, bte::bindings::BacktestOutcome::filled);
  QCOMPARE(result.value().positionShares, 7);
  QVERIFY(result.value().barsProcessed > 1U);
  QVERIFY(result.value().fill.has_value());
}

void BacktestSessionVmTest::
    pythonPreparationRejectsInvalidInputsBeforeRuntimeDiscovery() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const auto root = std::filesystem::path{directory.path().toStdString()};
  const auto storage = bte::bindings::PersistedBacktestStorage{
      .resultStore = root / "Results",
      .dataStore = root / "Data",
      .snapshotId = "missing-snapshot",
      .strategyHash = {},
  };
  const auto validStrategy = bte::bindings::PythonStrategyConfiguration{
      .sourceUtf8 = "valid source text",
      .canonicalParametersJson = R"({"quantity":1})",
      .sourceConsentGranted = true,
  };
  const auto makeConfiguration = [&validStrategy] {
    return bte::bindings::BacktestConfiguration{
        .symbol = "SYN",
        .schema = "ohlcv-1h",
        .startDate = QDate{2024, 1, 2},
        .endDate = QDate{2024, 1, 3},
        .initialCapital = 1'000.0,
        .quantityShares = 1,
        .selectableStrategy = {},
        .pythonStrategy = validStrategy,
    };
  };
  const auto resourcesRoot = root / "MissingRuntime";
  const auto bootstrapPath = root / "worker_bootstrap.py";

  auto nonPersisted = makeConfiguration();
  const auto nonPersistedResult =
      bte::bindings::runBacktestConfiguration(nonPersisted);
  QVERIFY(!nonPersistedResult.ok());
  QCOMPARE(nonPersistedResult.error().code,
           bte::core::ErrorCode::invalidArgument);

  auto persistedInvalid = makeConfiguration();
  persistedInvalid.schema = "ohlcv-5m";
  const auto persistedInvalidResult =
      bte::bindings::runPersistedBacktestConfiguration(persistedInvalid,
                                                       storage);
  QVERIFY(!persistedInvalidResult.ok());
  QCOMPARE(persistedInvalidResult.error().code,
           bte::core::ErrorCode::invalidArgument);

  auto noPythonStrategy = makeConfiguration();
  noPythonStrategy.pythonStrategy.reset();
  const auto noPython =
      bte::bindings::verifyPersistedPythonBacktestConfiguration(
          noPythonStrategy, storage, resourcesRoot, bootstrapPath);
  QVERIFY(!noPython.ok());
  QCOMPARE(noPython.error().code, bte::core::ErrorCode::invalidArgument);

  const std::array<std::tuple<QString, QDate, QDate>, 3> invalidConfigurations{{
      {QString{"ohlcv-5m"}, QDate{2024, 1, 2}, QDate{2024, 1, 3}},
      {QString{"ohlcv-1h"}, QDate{}, QDate{2024, 1, 3}},
      {QString{"ohlcv-1h"}, QDate{2024, 1, 4}, QDate{2024, 1, 3}},
  }};
  for (const auto &[schema, startDate, endDate] : invalidConfigurations) {
    auto configuration = makeConfiguration();
    configuration.schema = schema;
    configuration.startDate = startDate;
    configuration.endDate = endDate;
    const auto rejected =
        bte::bindings::verifyPersistedPythonBacktestConfiguration(
            configuration, storage, resourcesRoot, bootstrapPath);
    QVERIFY(!rejected.ok());
    QCOMPARE(rejected.error().code, bte::core::ErrorCode::invalidArgument);
  }

  auto cancelled = bte::core::CancellationSource{};
  cancelled.requestCancellation();
  const auto cancelledValidation = bte::bindings::validatePythonStrategy(
      validStrategy,
      {.resourcesRoot = resourcesRoot, .workerBootstrapPath = bootstrapPath},
      cancelled.token());
  QVERIFY(!cancelledValidation.ok());
  QCOMPARE(cancelledValidation.error().code, bte::core::ErrorCode::cancelled);

  auto invalidCapital = makeConfiguration();
  invalidCapital.initialCapital = 0.0;
  const auto rejectedCapital =
      bte::bindings::verifyPersistedPythonBacktestConfiguration(
          invalidCapital, storage, resourcesRoot, bootstrapPath);
  QVERIFY(!rejectedCapital.ok());
  QCOMPARE(rejectedCapital.error().code, bte::core::ErrorCode::invalidArgument);
  invalidCapital.initialCapital = std::numeric_limits<double>::quiet_NaN();
  const auto rejectedNonFiniteCapital =
      bte::bindings::verifyPersistedPythonBacktestConfiguration(
          invalidCapital, storage, resourcesRoot, bootstrapPath);
  QVERIFY(!rejectedNonFiniteCapital.ok());
  QCOMPARE(rejectedNonFiniteCapital.error().code,
           bte::core::ErrorCode::invalidArgument);

  auto invalidSource = makeConfiguration();
  const std::array invalidSources{
      std::string{},
      std::string{"source\0text", 11},
      std::string{"source\rtext"},
      std::string{"\xef\xbb\xbfsource"},
      std::string{"\xff", 1},
      std::string(std::size_t{1024} * 1024 + 1, 'x'),
  };
  for (const auto &source : invalidSources) {
    invalidSource.pythonStrategy->sourceUtf8 = source;
    const auto rejected =
        bte::bindings::verifyPersistedPythonBacktestConfiguration(
            invalidSource, storage, resourcesRoot, bootstrapPath);
    QVERIFY(!rejected.ok());
    QCOMPARE(rejected.error().code, bte::core::ErrorCode::invalidArgument);
  }

  auto invalidParameters = makeConfiguration();
  const std::array invalidParameterJson{
      std::string{"{"},
      std::string{"[]"},
      std::string{"{ \"quantity\":1}"},
      std::string{"{}"},
      std::string{R"({"quantity":true})"},
      std::string{R"({"quantity":0})"},
      std::string{R"({"quantity":-1})"},
      std::string{R"({"quantity":1000000001})"},
  };
  for (const auto &parameters : invalidParameterJson) {
    invalidParameters.pythonStrategy->canonicalParametersJson = parameters;
    const auto rejected =
        bte::bindings::verifyPersistedPythonBacktestConfiguration(
            invalidParameters, storage, resourcesRoot, bootstrapPath);
    QVERIFY(!rejected.ok());
    QCOMPARE(rejected.error().code, bte::core::ErrorCode::invalidArgument);
  }

  const auto unavailable =
      bte::bindings::verifyPersistedPythonBacktestConfiguration(
          makeConfiguration(), storage, resourcesRoot, bootstrapPath);
  QVERIFY(!unavailable.ok());
  QCOMPARE(unavailable.error().code, bte::core::ErrorCode::runtimeUnavailable);
  QVERIFY(!std::filesystem::exists(storage.resultStore));
}

void BacktestSessionVmTest::persistedPythonRunCanBeSavedAndReplayed() {
#if defined(BTE_TEST_MANAGED_RUNTIME_PROFILE_PATH)
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const auto root = std::filesystem::path{directory.path().toStdString()};
  const auto profileRoot =
      std::filesystem::path{BTE_TEST_MANAGED_RUNTIME_PROFILE_PATH};
  const auto resourcesRoot = root / "Resources";
  const auto runtimeProfiles = resourcesRoot / "RuntimeProfiles";
  std::filesystem::create_directories(runtimeProfiles);
  const auto packagedPythonDirectory = resourcesRoot / "Python";
  std::filesystem::create_directories(packagedPythonDirectory);
  const auto packagedBootstrap =
      packagedPythonDirectory / "worker_bootstrap.py";
  std::filesystem::copy_file(BTE_PYTHON_WORKER_BOOTSTRAP_PATH,
                             packagedBootstrap);
  std::error_code symlinkError;
  std::filesystem::create_directory_symlink(
      profileRoot, runtimeProfiles / profileRoot.filename(), symlinkError);
  QVERIFY2(!symlinkError, symlinkError.message().c_str());

  const auto resourceOverride = ScopedEnvironmentVariable{
      "BTE_PYTHON_RESOURCES_ROOT",
      QByteArray::fromStdString(resourcesRoot.string())};
  const auto bootstrapOverride =
      ScopedEnvironmentVariable{"BTE_PYTHON_WORKER_BOOTSTRAP", QByteArray{}};
  QVERIFY(resourceOverride.setSuccessfully());
  QVERIFY(bootstrapOverride.setSuccessfully());

  const auto sourceDirectory = root / "Source";
  std::filesystem::create_directories(sourceDirectory);
  std::ofstream sourceFile{sourceDirectory / "SYN.csv"};
  sourceFile << "symbol,ts,open,high,low,close,volume,schemaName\n"
                "SYN,2024-01-01 00:00:00+00:00,100,101,99,100,1200,ohlcv-1h\n"
                "SYN,2024-01-01 01:00:00+00:00,100,101,99,100,1200,ohlcv-1h\n"
                "SYN,2024-01-01 02:00:00+00:00,110,111,109,110,1200,ohlcv-1h\n"
                "SYN,2024-01-01 03:00:00+00:00,90,91,89,90,1200,ohlcv-1h\n"
                "SYN,2024-01-01 04:00:00+00:00,80,81,79,80,1200,ohlcv-1h\n";
  sourceFile.close();
  const auto built = bte::data::buildReleaseSnapshot({
      .sourceDirectory = sourceDirectory,
      .storeDirectory = root / "Data",
      .symbols = {"SYN"},
      .rowsPerSegment = 2,
      .calendarHash = std::string(64, 'a'),
      .splitManifestHash = std::string(64, 'b'),
  });
  QVERIFY2(built.ok(), built.error().message.c_str());

  const auto storage = bte::bindings::PersistedBacktestStorage{
      .resultStore = root / "Results",
      .dataStore = root / "Data",
      .snapshotId = built.value().snapshotId,
      .strategyHash = {},
  };
  std::filesystem::create_directories(storage.resultStore);
  std::ofstream verificationBlocker{storage.resultStore / "Verifications"};
  verificationBlocker << "verification store unavailable";
  verificationBlocker.close();

  const std::string sampleSource = R"python(import stockbt

STOCKBT_API_VERSION = "1"
STOCKBT_TEMPLATE_VERSION = "1.0.0"

def create_strategy(params):
    fast_period = params["fast_period"]
    slow_period = params["slow_period"]
    quantity = params["quantity"]

    class SmaCrossover:
        max_history_bars = slow_period
        indicators = {
            "fast": stockbt.SMA("fast", period=fast_period),
            "slow": stockbt.SMA("slow", period=slow_period),
        }

        def __init__(self):
            self.previous = None

        def on_market(self, ctx):
            fast = ctx.indicators["fast"]
            slow = ctx.indicators["slow"]
            if fast is None or slow is None:
                return
            previous = self.previous
            self.previous = (fast, slow)
            if previous is None:
                return
            previous_fast, previous_slow = previous
            position = ctx.portfolio.positions.get(ctx.symbol)
            shares = 0 if position is None else position.quantity_shares
            if shares == 0 and previous_fast <= previous_slow and fast > slow:
                ctx.orders.buy_market(ctx.symbol, quantity=quantity)
            elif shares > 0 and previous_fast >= previous_slow and fast < slow:
                ctx.orders.close_position(ctx.symbol)

    return SmaCrossover()
)python";
  const auto configuration = bte::bindings::BacktestConfiguration{
      .symbol = "SYN",
      .schema = "ohlcv-1h",
      .startDate = QDate{2024, 1, 1},
      .endDate = QDate{2024, 1, 1},
      .initialCapital = 1'000.0,
      .quantityShares = 1,
      .selectableStrategy = {},
      .pythonStrategy =
          bte::bindings::PythonStrategyConfiguration{
              .sourceUtf8 = sampleSource,
              .canonicalParametersJson =
                  R"({"fast_period":1,"quantity":1,"slow_period":2})",
              .sourceConsentGranted = true,
          },
  };

  const auto validation = bte::bindings::validatePythonStrategy(
      *configuration.pythonStrategy,
      {.resourcesRoot = resourcesRoot,
       .workerBootstrapPath = packagedBootstrap});
  QVERIFY2(validation.ok(),
           validation.ok() ? "" : validation.error().message.c_str());

  const auto run =
      bte::bindings::runPersistedBacktestConfiguration(configuration, storage);

  QVERIFY2(run.ok(), run.error().message.c_str());
  QCOMPARE(run.value().status, bte::results::RunStatus::completed);
  QVERIFY(!run.value().reproducibilityVerified);
  QVERIFY(run.value().reproducibilityDiagnostic.find(
              "Verification status is unavailable") != std::string::npos);
  QCOMPARE(run.value().fills.size(), 2U);
  QCOMPARE(run.value().fills[0].side, bte::bindings::BacktestFillSide::buy);
  QCOMPARE(run.value().fills[1].side, bte::bindings::BacktestFillSide::sell);
  QVERIFY(!run.value().resultId.empty());
  QVERIFY(!run.value().canonicalResultHash.empty());

  QVERIFY(std::filesystem::remove(storage.resultStore / "Verifications"));
  std::vector<std::size_t> verificationProgress;
  const auto verification =
      bte::bindings::verifyPersistedPythonBacktestConfiguration(
          configuration, storage, resourcesRoot,
          std::filesystem::path{BTE_PYTHON_WORKER_BOOTSTRAP_PATH},
          [&verificationProgress](
              const bte::results::VerificationEvidence &evidence) {
            verificationProgress.push_back(evidence.progressOrdinal);
          });
  QVERIFY2(verification.ok(),
           verification.ok() ? "" : verification.error().message.c_str());
  QCOMPARE(verification.value().status,
           bte::results::VerificationStatus::verified);
  QCOMPARE(verification.value().attempts.size(), 3U);
  QVERIFY(!verificationProgress.empty());
  QCOMPARE(verificationProgress.back(), 3U);

  const auto verifiedRun =
      bte::bindings::runPersistedBacktestConfiguration(configuration, storage);
  QVERIFY2(verifiedRun.ok(),
           verifiedRun.ok() ? "" : verifiedRun.error().message.c_str());
  QVERIFY(verifiedRun.value().reproducibilityVerified);

  const auto published = bte::bindings::publishBacktestDraft(
      storage, run.value().resultId, "Python SMA integration");
  QVERIFY2(published.ok(), published.error().message.c_str());
  const auto replay = bte::bindings::ResultReplay::open(
      storage.resultStore, storage.dataStore, published.value(),
      bte::bindings::ResultReplayTimeframe::hourly);
  QVERIFY2(replay.ok(), replay.error().message.c_str());
  QCOMPARE(replay.value()->resultId(), published.value());
  QCOMPARE(replay.value()->totalFrames(), 5U);
  QVERIFY(replay.value()->seek(3));
  QVERIFY(replay.value()->current() != nullptr);
  QCOMPARE(replay.value()->current()->fills.size(), 1U);
  QVERIFY(replay.value()->current()->fills.front().isBuy);
  QVERIFY(replay.value()->seek(4));
  QVERIFY(replay.value()->current() != nullptr);
  QCOMPARE(replay.value()->current()->fills.size(), 1U);
  QVERIFY(!replay.value()->current()->fills.front().isBuy);
#else
  QSKIP("The managed Python runtime profile is not enabled in this build.");
#endif
}

void BacktestSessionVmTest::
    pythonWarmupSelectsActualBarsBeyondTheInitialCalendarWindow() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const auto root = std::filesystem::path{directory.path().toStdString()};
  const auto sourceDirectory = root / "Source";
  std::filesystem::create_directories(sourceDirectory);
  std::ofstream source{sourceDirectory / "SYN.csv"};
  source << "symbol,ts,open,high,low,close,volume,schemaName\n"
            "SYN,2024-01-01 00:00:00+00:00,100,101,99,100,1200,ohlcv-1h\n"
            "SYN,2024-06-01 00:00:00+00:00,101,102,100,101,1200,ohlcv-1h\n"
            "SYN,2024-11-01 00:00:00+00:00,102,103,101,102,1200,ohlcv-1h\n"
            "SYN,2025-04-01 00:00:00+00:00,103,104,102,103,1200,ohlcv-1h\n"
            "SYN,2025-09-01 00:00:00+00:00,104,105,103,104,1200,ohlcv-1h\n";
  source.close();
  const auto built = bte::data::buildReleaseSnapshot({
      .sourceDirectory = sourceDirectory,
      .storeDirectory = root / "Data",
      .symbols = {"SYN"},
      .rowsPerSegment = 2,
      .calendarHash = std::string(64, 'a'),
      .splitManifestHash = std::string(64, 'b'),
  });
  QVERIFY2(built.ok(), built.error().message.c_str());

  const auto selected = bte::bindings::testing::selectPythonWarmupBars(
      root / "Data", built.value().snapshotId, "SYN", QDate{2025, 10, 1}, 5);
  QVERIFY2(selected.ok(),
           selected.ok() ? "" : selected.error().message.c_str());
  QCOMPARE(selected.value().size(), 5U);
  QCOMPARE(selected.value().front().ts,
           bte::core::Timestamp{
               std::chrono::sys_days{std::chrono::year{2024} / 1 / 1}});

  const auto bounded = bte::bindings::testing::selectPythonWarmupBars(
      root / "Data", built.value().snapshotId, "SYN", QDate{2025, 10, 1}, 2);
  QVERIFY2(bounded.ok(), bounded.ok() ? "" : bounded.error().message.c_str());
  QCOMPARE(bounded.value().size(), 2U);
  QCOMPARE(bounded.value().front().ts,
           bte::core::Timestamp{
               std::chrono::sys_days{std::chrono::year{2025} / 4 / 1}});
}

void BacktestSessionVmTest::pythonWarmupRejectsInvalidHistoryLimitsAndDates() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const auto root = std::filesystem::path{directory.path().toStdString()};
  const auto sourceDirectory = root / "Source";
  std::filesystem::create_directories(sourceDirectory);
  std::ofstream source{sourceDirectory / "SYN.csv"};
  source << "symbol,ts,open,high,low,close,volume,schemaName\n"
            "SYN,2024-01-01 00:00:00+00:00,100,101,99,100,1200,ohlcv-1h\n";
  source.close();
  const auto built = bte::data::buildReleaseSnapshot({
      .sourceDirectory = sourceDirectory,
      .storeDirectory = root / "Data",
      .symbols = {"SYN"},
      .rowsPerSegment = 1,
      .calendarHash = std::string(64, 'a'),
      .splitManifestHash = std::string(64, 'b'),
  });
  QVERIFY2(built.ok(), built.error().message.c_str());

  const auto zeroHistory = bte::bindings::testing::selectPythonWarmupBars(
      root / "Data", built.value().snapshotId, "SYN", QDate{2024, 1, 2}, 0);
  const auto excessiveHistory = bte::bindings::testing::selectPythonWarmupBars(
      root / "Data", built.value().snapshotId, "SYN", QDate{2024, 1, 2},
      10'001);
  const auto invalidDate = bte::bindings::testing::selectPythonWarmupBars(
      root / "Data", built.value().snapshotId, "SYN", QDate{}, 1);
  const auto missingSnapshot = bte::bindings::testing::selectPythonWarmupBars(
      root / "Data", "missing-snapshot", "SYN", QDate{2024, 1, 2}, 1);

  QVERIFY(!zeroHistory.ok());
  QCOMPARE(zeroHistory.error().code, bte::core::ErrorCode::invalidArgument);
  QVERIFY(!excessiveHistory.ok());
  QCOMPARE(excessiveHistory.error().code,
           bte::core::ErrorCode::invalidArgument);
  QVERIFY(!invalidDate.ok());
  QCOMPARE(invalidDate.error().code, bte::core::ErrorCode::invalidArgument);
  QVERIFY(!missingSnapshot.ok());

  bte::core::CancellationSource cancellation;
  cancellation.requestCancellation();
  const auto canceled = bte::bindings::testing::selectPythonWarmupBars(
      root / "Data", built.value().snapshotId, "SYN", QDate{2024, 1, 2}, 1,
      cancellation.token());
  QVERIFY(!canceled.ok());
  QCOMPARE(canceled.error().code, bte::core::ErrorCode::cancelled);

  const auto noHistory = bte::bindings::testing::selectPythonWarmupBars(
      root / "Data", built.value().snapshotId, "ABSENT", QDate{2024, 1, 2}, 1);
  QVERIFY2(noHistory.ok(),
           noHistory.ok() ? "" : noHistory.error().message.c_str());
  QVERIFY(noHistory.value().empty());
}

void BacktestSessionVmTest::draftLifecycleBoundariesReturnStructuredResults() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const auto storage = bte::bindings::PersistedBacktestStorage{
      .resultStore =
          std::filesystem::path{directory.path().toStdString()} / "Results",
      .dataStore =
          std::filesystem::path{directory.path().toStdString()} / "Data",
      .snapshotId = {},
      .strategyHash = {},
  };
  const auto empty = bte::bindings::listBacktestDrafts(storage);
  QVERIFY(empty.ok());
  QVERIFY(empty.value().empty());

  const auto missing =
      bte::bindings::discardBacktestDraft(storage, std::string(32, 'a'));
  QVERIFY(!missing.ok());
  QCOMPARE(missing.error().code, bte::core::ErrorCode::notFound);
  const auto invalid = bte::bindings::publishBacktestDraft(storage, "bad-id");
  QVERIFY(!invalid.ok());
  QCOMPARE(invalid.error().code, bte::core::ErrorCode::invalidArgument);

  auto invalidStorage = storage;
  invalidStorage.resultStore.clear();
  const auto unopenablePublish =
      bte::bindings::publishBacktestDraft(invalidStorage, std::string(32, 'a'));
  const auto unopenableDiscard =
      bte::bindings::discardBacktestDraft(invalidStorage, std::string(32, 'a'));
  QVERIFY(!unopenablePublish.ok());
  QVERIFY(!unopenableDiscard.ok());
  auto blockedStorage = storage;
  blockedStorage.resultStore =
      std::filesystem::path{directory.path().toStdString()} / "BlockedStore";
  std::ofstream blocked{blockedStorage.resultStore};
  blocked << "not a directory";
  blocked.close();
  const auto unopenableList = bte::bindings::listBacktestDrafts(blockedStorage);
  QVERIFY(!unopenableList.ok());

  bte::core::CancellationSource source;
  source.requestCancellation();
  const auto cancellation = source.token();
  const auto cancelledList =
      bte::bindings::listBacktestDrafts(storage, cancellation);
  const auto cancelledPublish = bte::bindings::publishBacktestDraft(
      storage, std::string(32, 'a'), {}, cancellation);
  const auto cancelledDiscard = bte::bindings::discardBacktestDraft(
      storage, std::string(32, 'a'), cancellation);
  QVERIFY(!cancelledList.ok());
  QVERIFY(!cancelledPublish.ok());
  QVERIFY(!cancelledDiscard.ok());
  QCOMPARE(cancelledList.error().code, bte::core::ErrorCode::cancelled);
  QCOMPARE(cancelledPublish.error().code, bte::core::ErrorCode::cancelled);
  QCOMPARE(cancelledDiscard.error().code, bte::core::ErrorCode::cancelled);

  bte::core::CancellationSource cancelDuringPublish;
  bte::bindings::testing::cancelAfterNextDraftStoreOpen(cancelDuringPublish);
  const auto interruptedPublish = bte::bindings::publishBacktestDraft(
      storage, std::string(32, 'a'), {}, cancelDuringPublish.token());
  bte::bindings::testing::clearDraftStoreOpenHook();
  QVERIFY(!interruptedPublish.ok());
  QCOMPARE(interruptedPublish.error().code, bte::core::ErrorCode::cancelled);

  bte::core::CancellationSource cancelDuringDiscard;
  bte::bindings::testing::cancelAfterNextDraftStoreOpen(cancelDuringDiscard);
  const auto interruptedDiscard = bte::bindings::discardBacktestDraft(
      storage, std::string(32, 'a'), cancelDuringDiscard.token());
  bte::bindings::testing::clearDraftStoreOpenHook();
  QVERIFY(!interruptedDiscard.ok());
  QCOMPARE(interruptedDiscard.error().code, bte::core::ErrorCode::cancelled);
}

void BacktestSessionVmTest::
    draftCatalogKeepsRecoveryNoticesWithoutMutatingLegacyFiles() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const auto root = std::filesystem::path{directory.path().toStdString()};
  const auto storage =
      bte::bindings::PersistedBacktestStorage{.resultStore = root / "Results",
                                              .dataStore = root / "Data",
                                              .snapshotId = {},
                                              .strategyHash = {}};
  const auto missing = bte::bindings::listBacktestDraftCatalog(storage);
  QVERIFY(missing.ok());
  QVERIFY(missing.value().entries.empty());
  QVERIFY(missing.value().notices.empty());
  QVERIFY(!std::filesystem::exists(storage.resultStore));
  bte::core::CancellationSource cancellation;
  cancellation.requestCancellation();
  const auto cancelled =
      bte::bindings::listBacktestDraftCatalog(storage, cancellation.token());
  QVERIFY(!cancelled.ok());
  QCOMPARE(cancelled.error().code, bte::core::ErrorCode::cancelled);

  std::filesystem::create_directories(storage.resultStore);
  bte::core::CancellationSource cancelDuringList;
  bte::bindings::testing::cancelAfterNextDraftStoreOpen(cancelDuringList);
  const auto interruptedList = bte::bindings::listBacktestDraftCatalog(
      storage, cancelDuringList.token());
  bte::bindings::testing::clearDraftStoreOpenHook();
  QVERIFY(!interruptedList.ok());
  QCOMPARE(interruptedList.error().code, bte::core::ErrorCode::cancelled);

  std::filesystem::create_directories(storage.resultStore / "Staging");
  const auto resultId = std::string(32, 'a');
  const auto legacyPath =
      storage.resultStore / "Staging" / (resultId + ".bteresult");
  {
    std::ofstream legacy{legacyPath};
    legacy << "unleased legacy staging";
  }
  const auto catalog = bte::bindings::listBacktestDraftCatalog(storage);
  QVERIFY2(catalog.ok(), catalog.error().message.c_str());
  QVERIFY(catalog.value().entries.empty());
  QCOMPARE(catalog.value().notices.size(), 1U);
  QCOMPARE(catalog.value().notices.front().resultId, resultId);
  QCOMPARE(catalog.value().notices.front().reason,
           bte::bindings::ResultRecoveryReason::legacyUnleased);
  QVERIFY(!catalog.value().notices.front().diagnostic.empty());
  std::ifstream legacy{legacyPath};
  std::string contents;
  std::getline(legacy, contents);
  QCOMPARE(contents, std::string{"unleased legacy staging"});
  const auto compatibility = bte::bindings::listBacktestDrafts(storage);
  QVERIFY(compatibility.ok());
  QVERIFY(compatibility.value().empty());
}

void BacktestSessionVmTest::
    catalogSnapshotTranslatesEntriesAndEveryRecoveryReason() {
  const std::array entries{
      bte::results::ResultSummary{
          .resultId = "running",
          .displayName = "Running result",
          .canonicalResultHash = "hash-running",
          .status = bte::results::RunStatus::running,
          .savedUtcMillis = 101,
          .available = true,
          .unavailableReason = "",
      },
      bte::results::ResultSummary{
          .resultId = "completed",
          .displayName = "Completed result",
          .canonicalResultHash = "hash-completed",
          .status = bte::results::RunStatus::completed,
          .savedUtcMillis = 102,
          .available = true,
          .unavailableReason = "",
      },
      bte::results::ResultSummary{
          .resultId = "failed",
          .displayName = "Failed result",
          .canonicalResultHash = "hash-failed",
          .status = bte::results::RunStatus::failed,
          .savedUtcMillis = 103,
          .available = false,
          .unavailableReason = "missing segment",
      },
      bte::results::ResultSummary{
          .resultId = "canceled",
          .displayName = "Canceled result",
          .canonicalResultHash = "hash-canceled",
          .status = bte::results::RunStatus::canceled,
          .savedUtcMillis = 104,
          .available = true,
          .unavailableReason = "",
      },
      bte::results::ResultSummary{
          .resultId = "interrupted",
          .displayName = "Interrupted result",
          .canonicalResultHash = "hash-interrupted",
          .status = bte::results::RunStatus::interrupted,
          .savedUtcMillis = 105,
          .available = false,
          .unavailableReason = "invalid lease",
      },
      bte::results::ResultSummary{
          .resultId = "incomplete",
          .displayName = "Incomplete result",
          .canonicalResultHash = "hash-incomplete",
          .status = bte::results::RunStatus::incomplete,
          .savedUtcMillis = 106,
          .available = false,
          .unavailableReason = "incomplete result",
      },
  };
  const std::array notices{
      bte::results::ResultRecoveryNotice{
          .resultId = "legacy",
          .reason = bte::results::ResultRecoveryReason::legacyUnleased,
          .diagnostic = "legacy diagnostic",
      },
      bte::results::ResultRecoveryNotice{
          .resultId = "lease",
          .reason = bte::results::ResultRecoveryReason::invalidLease,
          .diagnostic = "lease diagnostic",
      },
      bte::results::ResultRecoveryNotice{
          .resultId = "prefix",
          .reason = bte::results::ResultRecoveryReason::invalidPrefix,
          .diagnostic = "prefix diagnostic",
      },
      bte::results::ResultRecoveryNotice{
          .resultId = "references",
          .reason = bte::results::ResultRecoveryReason::missingReferences,
          .diagnostic = "references diagnostic",
      },
      bte::results::ResultRecoveryNotice{
          .resultId = "initialization",
          .reason = bte::results::ResultRecoveryReason::orphanedInitialization,
          .diagnostic = "initialization diagnostic",
      },
  };

  const auto catalog = bte::bindings::makeResultCatalogSnapshot(
      std::vector<bte::results::ResultSummary>{entries.begin(), entries.end()},
      notices);
  const std::array expectedStatuses{
      bte::bindings::ResultRunStatus::running,
      bte::bindings::ResultRunStatus::completed,
      bte::bindings::ResultRunStatus::failed,
      bte::bindings::ResultRunStatus::canceled,
      bte::bindings::ResultRunStatus::interrupted,
      bte::bindings::ResultRunStatus::incomplete,
  };
  QCOMPARE(catalog.entries.size(), entries.size());
  for (auto index = std::size_t{}; index < entries.size(); ++index) {
    const auto &actual = catalog.entries[index];
    const auto &expected = entries[index];
    QCOMPARE(actual.resultId, expected.resultId);
    QCOMPARE(actual.displayName, expected.displayName);
    QCOMPARE(actual.canonicalResultHash, expected.canonicalResultHash);
    QCOMPARE(actual.status, expectedStatuses[index]);
    QCOMPARE(actual.savedUtcMillis, expected.savedUtcMillis);
    QCOMPARE(actual.available, expected.available);
    QCOMPARE(actual.unavailableReason, expected.unavailableReason);
  }

  const std::array expectedReasons{
      bte::bindings::ResultRecoveryReason::legacyUnleased,
      bte::bindings::ResultRecoveryReason::invalidLease,
      bte::bindings::ResultRecoveryReason::invalidPrefix,
      bte::bindings::ResultRecoveryReason::missingReferences,
      bte::bindings::ResultRecoveryReason::orphanedInitialization,
  };
  QCOMPARE(catalog.notices.size(), expectedReasons.size());
  for (auto index = std::size_t{}; index < expectedReasons.size(); ++index) {
    QCOMPARE(catalog.notices[index].resultId, notices[index].resultId);
    QCOMPARE(catalog.notices[index].reason, expectedReasons[index]);
    QCOMPARE(catalog.notices[index].diagnostic, notices[index].diagnostic);
  }

  const auto invalidStatus = bte::bindings::toResultRunStatus(
      static_cast<bte::results::RunStatus>(255));
  QCOMPARE(invalidStatus, bte::bindings::ResultRunStatus::incomplete);
  const std::array invalidNotice{
      bte::results::ResultRecoveryNotice{
          .resultId = "unknown",
          .reason = static_cast<bte::results::ResultRecoveryReason>(255),
          .diagnostic = "unknown recovery reason",
      },
  };
  const auto fallback =
      bte::bindings::makeResultCatalogSnapshot({}, invalidNotice);
  QCOMPARE(fallback.notices.size(), 1U);
  QCOMPARE(fallback.notices.front().reason,
           bte::bindings::ResultRecoveryReason::invalidPrefix);
}

} // namespace

QTEST_MAIN(BacktestSessionVmTest)

#include "UnitTest_BacktestSessionVm.moc"
