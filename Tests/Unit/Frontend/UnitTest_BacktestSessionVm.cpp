#include "Bte/Bindings/BacktestSessionVm.h"
#include "Bte/Bindings/ResultCatalogSnapshot.h"

#include "Bte/Core/Bar.h"
#include "Bte/Data/ReleaseSnapshot.h"

#include "BacktestSessionVmTestHooks.h"
#include "ReleaseSnapshotTestHooks.h"

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
  void persistedRunPreservesHistoricalDataDayLookupCancellation();
  void filledSnapshotPreservesNextBarTimestamp();
  void capitalUsesHalfEvenRoundingAtTheMinimumBoundary();
  void selectablePlanPresentsBothBuyAndSellFills();
  void selectablePlanPresentsNoSignalOutcome();
  void invalidSelectablePlanPreservesCompileError();
  void configuredRunComposesTrackedDataAndEngine();
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
