#include "Bte/Bindings/BacktestSessionVm.h"

#include "Bte/Core/Bar.h"

#include <QTest>

#include <algorithm>
#include <cfenv>
#include <chrono>
#include <cmath>
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
  void filledSnapshotPreservesNextBarTimestamp();
  void capitalUsesHalfEvenRoundingAtTheMinimumBoundary();
  void selectablePlanPresentsBothBuyAndSellFills();
  void selectablePlanPresentsNoSignalOutcome();
  void invalidSelectablePlanPreservesCompileError();
  void configuredRunComposesTrackedDataAndEngine();
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

} // namespace

QTEST_MAIN(BacktestSessionVmTest)

#include "UnitTest_BacktestSessionVm.moc"
