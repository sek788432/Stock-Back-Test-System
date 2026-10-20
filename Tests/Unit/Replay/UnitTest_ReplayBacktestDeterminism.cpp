#include "Bte/Bindings/BacktestSessionVm.h"
#include "Bte/Bindings/ResultReplay.h"
#include "Bte/Data/ReleaseSnapshot.h"
#include "Bte/Engine/Backtest.h"
#include "Bte/Results/ResultStore.h"
#include "Bte/Strategy/SelectableStrategy.h"

#include <gtest/gtest.h>

#include <QDate>
#include <QString>
#include <QTemporaryDir>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace {

struct PersistedDataDayRun final {
  bte::bindings::BacktestSnapshot backtest;
  bte::bindings::PersistedBacktestStorage storage;
  bte::strategy::SelectableStrategyPlan plan;
};

void expectBacktestFilledOnNextSelectedBar(
    const bte::bindings::BacktestSnapshot &backtest,
    const bte::core::Timestamp expectedFillTimestamp) {
  EXPECT_EQ(backtest.barsProcessed, 3U);
  ASSERT_EQ(backtest.fills.size(), 1U);
  EXPECT_EQ(backtest.fills.front().timestamp, expectedFillTimestamp);
  EXPECT_DOUBLE_EQ(backtest.fills.front().price, 103.0103);
}

void expectReplayContainsSelectedCandles(
    const bte::bindings::ResultReplay &replay,
    const std::array<bte::core::Timestamp, 3> &expectedTimestamps) {
  ASSERT_EQ(replay.totalFrames(), 3U);
  EXPECT_EQ(replay.timeline()[0].candle.ts, expectedTimestamps[0]);
  EXPECT_EQ(replay.timeline()[1].candle.ts, expectedTimestamps[1]);
  EXPECT_EQ(replay.timeline()[2].candle.ts, expectedTimestamps[2]);
}

void expectFillMarkerUsesNextSelectedBar(
    const bte::bindings::ResultReplay &replay,
    const bte::core::Timestamp expectedFillTimestamp) {
  EXPECT_TRUE(replay.timeline().front().fills.empty());
  ASSERT_EQ(replay.timeline()[1].fills.size(), 1U);
  EXPECT_EQ(replay.timeline()[1].fills.front().timestamp,
            expectedFillTimestamp);
}

void expectPersistedTimelineContainsOnlySelectedBars(
    const bte::results::OpenedResult &opened, const PersistedDataDayRun &run,
    const bte::core::Timestamp firstSelectedTimestamp) {
  EXPECT_EQ(opened.descriptor.strategyHash,
            bte::strategy::canonicalStrategyHash(run.plan));
  EXPECT_EQ(std::ranges::count_if(
                opened.records,
                [](const auto &record) {
                  return record.family == bte::results::RecordFamily::portfolio;
                }),
            3);
  const auto order =
      std::ranges::find_if(opened.records, [](const auto &record) {
        return record.family == bte::results::RecordFamily::order;
      });
  ASSERT_NE(order, opened.records.end());
  EXPECT_EQ(order->timestamp, firstSelectedTimestamp);
  for (const auto &record : opened.records) {
    EXPECT_GE(record.timestamp, firstSelectedTimestamp);
  }
}

class ReplayBacktestDeterminism : public testing::Test {
protected:
  void SetUp() override {
    ASSERT_TRUE(directory_.isValid());
    root_ = directory_.path().toStdString();
  }

  static bte::core::Timestamp hour(const int value) {
    return bte::core::Timestamp{
        std::chrono::sys_days{std::chrono::year{2024} / 1 / 2} +
        std::chrono::hours{value}};
  }

  static bte::strategy::SelectableStrategyPlan alternatingPlan() {
    return {
        .buy =
            {.logic = bte::strategy::ConditionLogic::all,
             .conditions = {{.source = bte::strategy::ConditionSource::barField,
                             .comparison = bte::strategy::Comparison::lessThan,
                             .threshold = 100.0}}},
        .sell = {.logic = bte::strategy::ConditionLogic::all,
                 .conditions = {{.source =
                                     bte::strategy::ConditionSource::barField,
                                 .comparison =
                                     bte::strategy::Comparison::greaterThan,
                                 .threshold = 100.0}}},
    };
  }

  [[nodiscard]] bte::core::Result<bte::data::SnapshotSelection>
  multiFillSelection(const int endHour = 13) const {
    const auto built = bte::data::buildReleaseSnapshot({
        .sourceDirectory = std::filesystem::path{BTE_TEST_SOURCE_DIR} /
                           "Tests/Fixtures/ReplayMultiFill",
        .storeDirectory = root_ / "Data",
        .symbols = {"SYN"},
        .rowsPerSegment = 3,
        .calendarHash = std::string(64, 'a'),
        .splitManifestHash = std::string(64, 'b'),
    });
    if (!built.ok()) {
      return built.error();
    }
    auto reader = bte::data::ReleaseSnapshotReader::open(
        root_ / "Data", built.value().snapshotId);
    if (!reader.ok()) {
      return reader.error();
    }
    return reader.value()->select({
        .symbols = {"SYN"},
        .range = {.start = hour(1), .end = hour(endHour)},
        .timeframe = "ohlcv-1h",
    });
  }

  [[nodiscard]] bte::core::Result<bte::engine::RecordedBacktestOutcome>
  recordSelection(const bte::data::SnapshotSelection &selection,
                  const bte::strategy::SelectableStrategyPlan &plan) const {
    auto bars = std::vector<bte::core::Bar>{};
    bars.reserve(selection.bars.size());
    for (const auto &bar : selection.bars) {
      bars.push_back({
          .ts = bar.timestamp,
          .open = static_cast<double>(bar.openNanodollars) / 1'000'000'000.0,
          .high = static_cast<double>(bar.highNanodollars) / 1'000'000'000.0,
          .low = static_cast<double>(bar.lowNanodollars) / 1'000'000'000.0,
          .close = static_cast<double>(bar.closeNanodollars) / 1'000'000'000.0,
          .volume = static_cast<double>(bar.volumeMicroshares) / 1'000'000.0,
      });
    }
    auto store =
        bte::results::ResultStore::open(root_ / "Results", root_ / "Data");
    if (!store.ok()) {
      return store.error();
    }
    auto recorded = bte::engine::runBacktestAndRecord(
        {.bars = std::move(bars),
         .symbol = "SYN",
         .initialCapitalMicrodollars = 10'000'000'000,
         .quantityShares = 10,
         .selectableStrategy = plan},
        *store.value(),
        {.universe = {"SYN"},
         .range = {.start = selection.bars.front().timestamp,
                   .end =
                       selection.bars.back().timestamp + std::chrono::hours{1}},
         .initialCapitalMicrodollars = 10'000'000'000,
         .strategyId = "selectable-conditions",
         .strategyHash = bte::strategy::canonicalStrategyHash(plan),
         .dataSelection = selection.identity});
    if (!recorded.ok()) {
      return recorded.error();
    }
    auto published =
        store.value()->publishDraft(recorded.value().persisted.resultId);
    if (!published.ok()) {
      return published.error();
    }
    return recorded;
  }

  [[nodiscard]] bte::core::Result<PersistedDataDayRun>
  runPersistedDataDayBacktest() const {
    const auto built = bte::data::buildReleaseSnapshot({
        .sourceDirectory = std::filesystem::path{BTE_TEST_SOURCE_DIR} /
                           "Tests/Fixtures/Replay",
        .storeDirectory = root_ / "Data",
        .symbols = {"SYN"},
        .rowsPerSegment = 2,
        .calendarHash = std::string(64, 'a'),
        .splitManifestHash = std::string(64, 'b'),
    });
    if (!built.ok()) {
      return built.error();
    }
    const auto plan = bte::strategy::SelectableStrategyPlan{
        .buy = {.logic = bte::strategy::ConditionLogic::all,
                .conditions =
                    {
                        {.source = bte::strategy::ConditionSource::
                             previousDataDayCloseChangePercent,
                         .comparison =
                             bte::strategy::Comparison::greaterThanOrEqual,
                         .threshold = 1.0,
                         .thresholdDomain =
                             bte::indicators::NumericDomain::percent},
                        {.source = bte::strategy::ConditionSource::barField,
                         .comparison = bte::strategy::Comparison::lessThan,
                         .threshold = 102.0,
                         .barField = bte::indicators::BarField::open},
                    }},
        .sell = {},
    };
    const auto configuration = bte::bindings::BacktestConfiguration{
        .symbol = "SYN",
        .schema = "ohlcv-1h",
        .startDate = QDate{2024, 1, 2},
        .endDate = QDate{2024, 1, 2},
        .initialCapital = 2'000.0,
        .quantityShares = 10,
        .selectableStrategy = plan,
    };
    const auto storage = bte::bindings::PersistedBacktestStorage{
        .resultStore = root_ / "Results",
        .dataStore = root_ / "Data",
        .snapshotId = built.value().snapshotId,
        .strategyHash = std::string(64, 'd'),
    };
    auto backtest = bte::bindings::runPersistedBacktestConfiguration(
        configuration, storage);
    if (!backtest.ok()) {
      return backtest.error();
    }
    return PersistedDataDayRun{
        .backtest = std::move(backtest).value(),
        .storage = storage,
        .plan = plan,
    };
  }

  [[nodiscard]] bte::core::Result<PersistedDataDayRun>
  runPersistedHistoricalDataDayBacktest() const {
    const auto built = bte::data::buildReleaseSnapshot({
        .sourceDirectory = std::filesystem::path{BTE_TEST_SOURCE_DIR} /
                           "Tests/Fixtures/Replay",
        .storeDirectory = root_ / "Data",
        .symbols = {"SYN"},
        .rowsPerSegment = 2,
        .calendarHash = std::string(64, 'a'),
        .splitManifestHash = std::string(64, 'b'),
    });
    if (!built.ok()) {
      return built.error();
    }
    const auto plan = bte::strategy::SelectableStrategyPlan{
        .buy = {.logic = bte::strategy::ConditionLogic::all,
                .conditions = {{
                    .source =
                        bte::strategy::ConditionSource::dataDayGainPercent,
                    .comparison = bte::strategy::Comparison::greaterThanOrEqual,
                    .threshold = 1.9,
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
        .initialCapital = 2'000.0,
        .quantityShares = 10,
        .selectableStrategy = plan,
    };
    const auto storage = bte::bindings::PersistedBacktestStorage{
        .resultStore = root_ / "Results",
        .dataStore = root_ / "Data",
        .snapshotId = built.value().snapshotId,
        .strategyHash = std::string(64, 'e'),
    };
    auto backtest = bte::bindings::runPersistedBacktestConfiguration(
        configuration, storage);
    if (!backtest.ok()) {
      return backtest.error();
    }
    return PersistedDataDayRun{
        .backtest = std::move(backtest).value(),
        .storage = storage,
        .plan = plan,
    };
  }

  QTemporaryDir directory_{QString::fromStdString(
      (std::filesystem::temp_directory_path() / "bte-replay-backtest-XXXXXX")
          .string())};
  std::filesystem::path root_;
};

TEST_F(ReplayBacktestDeterminism,
       identicalBacktestsHaveDistinctIdsAndEqualRecordsAndHashes) {

  const auto built = bte::data::buildReleaseSnapshot({
      .sourceDirectory =
          std::filesystem::path{BTE_TEST_SOURCE_DIR} / "Tests/Fixtures/Replay",
      .storeDirectory = root_ / "Data",
      .symbols = {"SYN"},
      .rowsPerSegment = 2,
      .calendarHash = std::string(64, 'a'),
      .splitManifestHash = std::string(64, 'b'),
  });
  ASSERT_TRUE(built.ok()) << built.error().message;
  const auto configuration = bte::bindings::BacktestConfiguration{
      .symbol = "SYN",
      .schema = "ohlcv-1h",
      .startDate = QDate{2024, 1, 1},
      .endDate = QDate{2024, 1, 3},
      .initialCapital = 2'000.0,
      .quantityShares = 10,
      .selectableStrategy = {},
  };
  const auto storage = bte::bindings::PersistedBacktestStorage{
      .resultStore = root_ / "Results",
      .dataStore = root_ / "Data",
      .snapshotId = built.value().snapshotId,
      .strategyHash = std::string(64, 'd'),
  };

  const auto first =
      bte::bindings::runPersistedBacktestConfiguration(configuration, storage);
  const auto second =
      bte::bindings::runPersistedBacktestConfiguration(configuration, storage);
  ASSERT_TRUE(first.ok()) << first.error().message;
  ASSERT_TRUE(second.ok()) << second.error().message;
  const auto drafts = bte::bindings::listBacktestDrafts(storage);
  ASSERT_TRUE(drafts.ok()) << drafts.error().message;
  ASSERT_EQ(drafts.value().size(), 2U);
  EXPECT_NE(std::ranges::find(drafts.value(), first.value().resultId),
            drafts.value().end());
  EXPECT_NE(std::ranges::find(drafts.value(), second.value().resultId),
            drafts.value().end());
  const auto discardedRun =
      bte::bindings::runPersistedBacktestConfiguration(configuration, storage);
  ASSERT_TRUE(discardedRun.ok()) << discardedRun.error().message;
  ASSERT_TRUE(bte::bindings::discardBacktestDraft(storage,
                                                  discardedRun.value().resultId)
                  .ok());
  const auto afterDiscard = bte::bindings::listBacktestDrafts(storage);
  ASSERT_TRUE(afterDiscard.ok());
  EXPECT_EQ(afterDiscard.value().size(), 2U);
  ASSERT_TRUE(
      bte::bindings::publishBacktestDraft(storage, first.value().resultId)
          .ok());
  ASSERT_TRUE(
      bte::bindings::publishBacktestDraft(storage, second.value().resultId)
          .ok());
  EXPECT_NE(first.value().resultId, second.value().resultId);
  EXPECT_EQ(first.value().canonicalResultHash,
            second.value().canonicalResultHash);

  auto store =
      bte::results::ResultStore::open(root_ / "Results", root_ / "Data");
  ASSERT_TRUE(store.ok()) << store.error().message;
  auto firstOpened = store.value()->openResult(first.value().resultId);
  auto secondOpened = store.value()->openResult(second.value().resultId);
  ASSERT_TRUE(firstOpened.ok()) << firstOpened.error().message;
  ASSERT_TRUE(secondOpened.ok()) << secondOpened.error().message;
  EXPECT_EQ(firstOpened.value().records, secondOpened.value().records);
  EXPECT_EQ(
      firstOpened.value().descriptor.strategyHash,
      bte::strategy::canonicalStrategyHash(configuration.selectableStrategy));
  EXPECT_NE(firstOpened.value().descriptor.strategyHash, storage.strategyHash);
}

TEST_F(ReplayBacktestDeterminism,
       persistedDataDayPredicateReplaysOnlySelectedBars) {
  auto runResult = runPersistedDataDayBacktest();
  ASSERT_TRUE(runResult.ok()) << runResult.error().message;
  const auto run = std::move(runResult).value();
  expectBacktestFilledOnNextSelectedBar(run.backtest, hour(1));

  const auto published =
      bte::bindings::publishBacktestDraft(run.storage, run.backtest.resultId);
  ASSERT_TRUE(published.ok()) << published.error().message;
  auto replay = bte::bindings::ResultReplay::open(
      run.storage.resultStore, run.storage.dataStore, run.backtest.resultId,
      bte::bindings::ResultReplayTimeframe::hourly);
  ASSERT_TRUE(replay.ok()) << replay.error().message;
  expectReplayContainsSelectedCandles(*replay.value(),
                                      {hour(0), hour(1), hour(2)});
  expectFillMarkerUsesNextSelectedBar(*replay.value(), hour(1));

  auto store = bte::results::ResultStore::open(run.storage.resultStore,
                                               run.storage.dataStore);
  ASSERT_TRUE(store.ok()) << store.error().message;
  const auto opened = store.value()->openResult(run.backtest.resultId);
  ASSERT_TRUE(opened.ok()) << opened.error().message;
  expectPersistedTimelineContainsOnlySelectedBars(opened.value(), run, hour(0));
}

TEST_F(ReplayBacktestDeterminism,
       historicalDataDayWarmupPersistsAndReplaysOnlySelectedBars) {
  auto runResult = runPersistedHistoricalDataDayBacktest();
  ASSERT_TRUE(runResult.ok()) << runResult.error().message;
  const auto run = std::move(runResult).value();
  expectBacktestFilledOnNextSelectedBar(run.backtest, hour(1));

  auto repeatedRunResult = runPersistedHistoricalDataDayBacktest();
  ASSERT_TRUE(repeatedRunResult.ok()) << repeatedRunResult.error().message;
  EXPECT_EQ(run.backtest.canonicalResultHash,
            repeatedRunResult.value().backtest.canonicalResultHash);

  const auto published =
      bte::bindings::publishBacktestDraft(run.storage, run.backtest.resultId);
  ASSERT_TRUE(published.ok()) << published.error().message;
  auto replay = bte::bindings::ResultReplay::open(
      run.storage.resultStore, run.storage.dataStore, run.backtest.resultId,
      bte::bindings::ResultReplayTimeframe::hourly);
  ASSERT_TRUE(replay.ok()) << replay.error().message;
  expectReplayContainsSelectedCandles(*replay.value(),
                                      {hour(0), hour(1), hour(2)});
  expectFillMarkerUsesNextSelectedBar(*replay.value(), hour(1));

  auto store = bte::results::ResultStore::open(run.storage.resultStore,
                                               run.storage.dataStore);
  ASSERT_TRUE(store.ok()) << store.error().message;
  const auto opened = store.value()->openResult(run.backtest.resultId);
  ASSERT_TRUE(opened.ok()) << opened.error().message;
  expectPersistedTimelineContainsOnlySelectedBars(opened.value(), run, hour(0));
}

TEST_F(ReplayBacktestDeterminism,
       selectableBacktestGeneratesFiveRoundTripsAtNextActualOpens) {
  const auto selection = multiFillSelection();
  ASSERT_TRUE(selection.ok()) << selection.error().message;
  ASSERT_EQ(selection.value().bars.size(), 12U);
  EXPECT_EQ(selection.value().bars.front().timestamp, hour(1));
  EXPECT_EQ(selection.value().bars.back().timestamp, hour(12));
  const auto recorded = recordSelection(selection.value(), alternatingPlan());
  ASSERT_TRUE(recorded.ok()) << recorded.error().message;
  ASSERT_TRUE(recorded.value().backtest.has_value());
  const auto &backtest = *recorded.value().backtest;
  EXPECT_EQ(recorded.value().status, bte::results::RunStatus::completed);
  EXPECT_EQ(backtest.barsProcessed, 12U);
  ASSERT_EQ(backtest.fills.size(), 10U);
  constexpr auto fillHours = std::array{3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
  for (std::size_t index = 0; index < fillHours.size(); ++index) {
    SCOPED_TRACE(index);
    const auto &fill = backtest.fills[index];
    const auto isBuy = index % 2 == 0;
    EXPECT_EQ(fill.timestamp, hour(fillHours[index]));
    EXPECT_EQ(fill.side, isBuy ? bte::engine::BacktestOrderSide::buy
                               : bte::engine::BacktestOrderSide::sell);
    EXPECT_EQ(fill.quantityShares, 10);
    EXPECT_EQ(fill.priceNanodollars, isBuy ? 100'010'000'000 : 99'990'000'000);
    EXPECT_EQ(fill.amountMicrodollars, isBuy ? 1'000'100'000 : 999'900'000);
  }
  EXPECT_EQ(backtest.positionShares, 0);
  EXPECT_EQ(backtest.cashMicrodollars, 9'999'000'000);
  EXPECT_EQ(backtest.marketValueMicrodollars, 0);
  EXPECT_EQ(backtest.equityMicrodollars, 9'999'000'000);
  EXPECT_EQ(backtest.pnlMicrodollars, -1'000'000);
}

TEST_F(ReplayBacktestDeterminism,
       selectableResultReplaysExactCandlesFillsCostsAndEveryPortfolioSlice) {
  const auto selection = multiFillSelection();
  ASSERT_TRUE(selection.ok()) << selection.error().message;
  const auto first = recordSelection(selection.value(), alternatingPlan());
  const auto second = recordSelection(selection.value(), alternatingPlan());
  ASSERT_TRUE(first.ok()) << first.error().message;
  ASSERT_TRUE(second.ok()) << second.error().message;
  EXPECT_NE(first.value().persisted.resultId,
            second.value().persisted.resultId);
  EXPECT_EQ(first.value().persisted.canonicalResultHash,
            second.value().persisted.canonicalResultHash);
  auto store =
      bte::results::ResultStore::open(root_ / "Results", root_ / "Data");
  ASSERT_TRUE(store.ok()) << store.error().message;
  const auto firstOpened =
      store.value()->openResult(first.value().persisted.resultId);
  const auto secondOpened =
      store.value()->openResult(second.value().persisted.resultId);
  ASSERT_TRUE(firstOpened.ok()) << firstOpened.error().message;
  ASSERT_TRUE(secondOpened.ok()) << secondOpened.error().message;
  EXPECT_EQ(firstOpened.value().records, secondOpened.value().records);
  EXPECT_EQ(firstOpened.value().descriptor.dataSelection,
            selection.value().identity);
  EXPECT_EQ(firstOpened.value().summary.finalEquityMicrodollars, 9'999'000'000);
  EXPECT_EQ(firstOpened.value().summary.pnlMicrodollars, -1'000'000);

  struct ExpectedSlice {
    int utcHour;
    std::int64_t cash;
    std::int64_t position;
    std::int64_t marketValue;
    std::int64_t equity;
    std::int64_t pnl;
  };
  // Independent worked ledger: each $1000.10 buy / $999.90 sell loses $0.20.
  constexpr auto slices = std::array{
      ExpectedSlice{1, 10'000'000'000, 0, 0, 10'000'000'000, 0},
      ExpectedSlice{2, 10'000'000'000, 0, 0, 10'000'000'000, 0},
      ExpectedSlice{3, 8'999'900'000, 10, 1'010'000'000, 10'009'900'000,
                    9'900'000},
      ExpectedSlice{4, 9'999'800'000, 0, 0, 9'999'800'000, -200'000},
      ExpectedSlice{5, 8'999'700'000, 10, 1'010'000'000, 10'009'700'000,
                    9'700'000},
      ExpectedSlice{6, 9'999'600'000, 0, 0, 9'999'600'000, -400'000},
      ExpectedSlice{7, 8'999'500'000, 10, 1'010'000'000, 10'009'500'000,
                    9'500'000},
      ExpectedSlice{8, 9'999'400'000, 0, 0, 9'999'400'000, -600'000},
      ExpectedSlice{9, 8'999'300'000, 10, 1'010'000'000, 10'009'300'000,
                    9'300'000},
      ExpectedSlice{10, 9'999'200'000, 0, 0, 9'999'200'000, -800'000},
      ExpectedSlice{11, 8'999'100'000, 10, 1'010'000'000, 10'009'100'000,
                    9'100'000},
      ExpectedSlice{12, 9'999'000'000, 0, 0, 9'999'000'000, -1'000'000},
  };
  auto portfolioIndex = std::size_t{0};
  auto costIndex = std::size_t{0};
  auto fillIndex = std::size_t{0};
  for (const auto &record : firstOpened.value().records) {
    SCOPED_TRACE(record.sequence);
    if (record.family == bte::results::RecordFamily::portfolio) {
      ASSERT_LT(portfolioIndex, slices.size());
      const auto &expected = slices[portfolioIndex++];
      EXPECT_EQ(record.timestamp, hour(expected.utcHour));
      EXPECT_EQ(record.cashMicrodollars, expected.cash);
      EXPECT_EQ(record.positionShares, expected.position);
      EXPECT_EQ(record.marketValueMicrodollars, expected.marketValue);
      EXPECT_EQ(record.equityMicrodollars, expected.equity);
      EXPECT_EQ(record.pnlMicrodollars, expected.pnl);
    } else if (record.family == bte::results::RecordFamily::cost ||
               record.family == bte::results::RecordFamily::fill) {
      const auto isCost = record.family == bte::results::RecordFamily::cost;
      auto &index = isCost ? costIndex : fillIndex;
      ASSERT_LT(index, 10U);
      const auto isBuy = index % 2 == 0;
      EXPECT_EQ(record.timestamp, hour(static_cast<int>(index) + 3));
      EXPECT_EQ(record.symbol, "SYN");
      EXPECT_EQ(record.side, isBuy ? bte::results::OrderSide::buy
                                   : bte::results::OrderSide::sell);
      EXPECT_EQ(record.quantityShares, 10);
      if (isCost) {
        EXPECT_EQ(record.amountMicrodollars, 100'000);
        EXPECT_EQ(record.text, "slippage");
      } else {
        EXPECT_EQ(record.priceNanodollars,
                  isBuy ? 100'010'000'000 : 99'990'000'000);
        EXPECT_EQ(record.amountMicrodollars,
                  isBuy ? 1'000'100'000 : 999'900'000);
      }
      ++index;
    }
  }
  EXPECT_EQ(portfolioIndex, 12U);
  EXPECT_EQ(costIndex, 10U);
  EXPECT_EQ(fillIndex, 10U);

  for (const auto timeframe :
       {bte::bindings::ResultReplayTimeframe::hourly,
        bte::bindings::ResultReplayTimeframe::dailyUtc}) {
    auto replay = bte::bindings::ResultReplay::open(
        root_ / "Results", root_ / "Data", first.value().persisted.resultId,
        timeframe);
    auto repeated = bte::bindings::ResultReplay::open(
        root_ / "Results", root_ / "Data", second.value().persisted.resultId,
        timeframe);
    ASSERT_TRUE(replay.ok()) << replay.error().message;
    ASSERT_TRUE(repeated.ok()) << repeated.error().message;
    const auto hourly =
        timeframe == bte::bindings::ResultReplayTimeframe::hourly;
    ASSERT_EQ(replay.value()->totalFrames(), hourly ? 12U : 1U);
    ASSERT_EQ(repeated.value()->totalFrames(), replay.value()->totalFrames());
    auto replayFillIndex = std::size_t{0};
    for (std::size_t index = 0; index < replay.value()->totalFrames();
         ++index) {
      SCOPED_TRACE(index);
      ASSERT_TRUE(replay.value()->seek(index));
      ASSERT_TRUE(repeated.value()->seek(index));
      ASSERT_NE(replay.value()->current(), nullptr);
      const auto &frame = *replay.value()->current();
      EXPECT_EQ(frame, *repeated.value()->current());
      EXPECT_EQ(frame.candle.ts,
                hour(hourly ? static_cast<int>(index) + 1 : 0));
      EXPECT_DOUBLE_EQ(frame.candle.open, 100.0);
      EXPECT_DOUBLE_EQ(frame.candle.high, 102.0);
      EXPECT_DOUBLE_EQ(frame.candle.low, 98.0);
      EXPECT_DOUBLE_EQ(frame.candle.close,
                       hourly && index % 2 == 0 ? 101.0 : 99.0);
      EXPECT_DOUBLE_EQ(frame.candle.volume, hourly ? 1000.0 : 12000.0);
      EXPECT_EQ(frame.partialUtcDay, !hourly);
      ASSERT_EQ(frame.fills.size(), hourly ? (index < 2 ? 0U : 1U) : 10U);
      for (const auto &fill : frame.fills) {
        EXPECT_EQ(fill.timestamp, hour(static_cast<int>(replayFillIndex) + 3));
        EXPECT_EQ(fill.isBuy, replayFillIndex % 2 == 0);
        EXPECT_EQ(fill.quantityShares, 10);
        EXPECT_DOUBLE_EQ(fill.price, fill.isBuy ? 100.01 : 99.99);
        EXPECT_DOUBLE_EQ(fill.amount, fill.isBuy ? 1000.10 : 999.90);
        ++replayFillIndex;
      }
      const auto &expected = hourly ? slices[index] : slices.back();
      EXPECT_DOUBLE_EQ(frame.portfolio.cash,
                       static_cast<double>(expected.cash) / 1'000'000.0);
      EXPECT_EQ(frame.portfolio.positionShares, expected.position);
      EXPECT_DOUBLE_EQ(frame.portfolio.marketValue,
                       static_cast<double>(expected.marketValue) / 1'000'000.0);
      EXPECT_DOUBLE_EQ(frame.portfolio.equity,
                       static_cast<double>(expected.equity) / 1'000'000.0);
      EXPECT_DOUBLE_EQ(frame.portfolio.pnl,
                       static_cast<double>(expected.pnl) / 1'000'000.0);
    }
    EXPECT_EQ(replayFillIndex, 10U);
    EXPECT_EQ(replay.value()->progressPercent(), 100);
    EXPECT_FALSE(replay.value()->stepForward());
    const auto terminal = *replay.value()->current();
    EXPECT_FALSE(replay.value()->seek(replay.value()->totalFrames()));
    EXPECT_EQ(*replay.value()->current(), terminal);
    if (hourly) {
      EXPECT_TRUE(replay.value()->stepBack());
      EXPECT_EQ(replay.value()->current()->candle.ts, hour(11));
      EXPECT_EQ(replay.value()->current()->portfolio.positionShares, 10);
      EXPECT_TRUE(replay.value()->stepForward());
      EXPECT_EQ(*replay.value()->current(), terminal);
    } else {
      EXPECT_FALSE(replay.value()->stepBack());
    }
    EXPECT_TRUE(replay.value()->seek(0));
  }
}

TEST_F(ReplayBacktestDeterminism,
       exclusiveEndRemovesFifthSellAndKeepsTheActualOpenPosition) {
  const auto selection = multiFillSelection(12);
  ASSERT_TRUE(selection.ok()) << selection.error().message;
  ASSERT_EQ(selection.value().bars.size(), 11U);
  EXPECT_EQ(selection.value().bars.back().timestamp, hour(11));
  const auto recorded = recordSelection(selection.value(), alternatingPlan());
  ASSERT_TRUE(recorded.ok()) << recorded.error().message;
  ASSERT_TRUE(recorded.value().backtest.has_value());
  const auto &backtest = *recorded.value().backtest;
  ASSERT_EQ(backtest.fills.size(), 9U);
  constexpr auto hours = std::array{3, 4, 5, 6, 7, 8, 9, 10, 11};
  for (std::size_t index = 0; index < hours.size(); ++index) {
    EXPECT_EQ(backtest.fills[index].timestamp, hour(hours[index]));
    EXPECT_EQ(backtest.fills[index].side,
              index % 2 == 0 ? bte::engine::BacktestOrderSide::buy
                             : bte::engine::BacktestOrderSide::sell);
  }
  EXPECT_EQ(backtest.positionShares, 10);
  EXPECT_EQ(backtest.cashMicrodollars, 8'999'100'000);
  EXPECT_EQ(backtest.marketValueMicrodollars, 1'010'000'000);
  EXPECT_EQ(backtest.equityMicrodollars, 10'009'100'000);
  EXPECT_EQ(backtest.pnlMicrodollars, 9'100'000);
  for (const auto timeframe :
       {bte::bindings::ResultReplayTimeframe::hourly,
        bte::bindings::ResultReplayTimeframe::dailyUtc}) {
    auto replay = bte::bindings::ResultReplay::open(
        root_ / "Results", root_ / "Data", recorded.value().persisted.resultId,
        timeframe);
    ASSERT_TRUE(replay.ok()) << replay.error().message;
    ASSERT_TRUE(replay.value()->seek(replay.value()->totalFrames() - 1));
    const auto &frame = *replay.value()->current();
    EXPECT_DOUBLE_EQ(frame.candle.close, 101.0);
    EXPECT_EQ(frame.portfolio.positionShares, 10);
    EXPECT_DOUBLE_EQ(frame.portfolio.cash, 8999.10);
    EXPECT_DOUBLE_EQ(frame.portfolio.marketValue, 1010.0);
    EXPECT_DOUBLE_EQ(frame.portfolio.equity, 10009.10);
    EXPECT_DOUBLE_EQ(frame.portfolio.pnl, 9.10);
    ASSERT_FALSE(frame.fills.empty());
    EXPECT_EQ(frame.fills.back().timestamp, hour(11));
    EXPECT_TRUE(frame.fills.back().isBuy);
    EXPECT_FALSE(replay.value()->stepForward());
    EXPECT_FALSE(replay.value()->seek(replay.value()->totalFrames()));
    if (timeframe == bte::bindings::ResultReplayTimeframe::dailyUtc) {
      EXPECT_EQ(replay.value()->totalFrames(), 1U);
      EXPECT_EQ(frame.fills.size(), 9U);
      EXPECT_DOUBLE_EQ(frame.candle.volume, 11000.0);
      EXPECT_TRUE(frame.partialUtcDay);
    } else {
      EXPECT_EQ(replay.value()->totalFrames(), 11U);
      EXPECT_EQ(frame.candle.ts, hour(11));
      EXPECT_EQ(frame.fills.size(), 1U);
    }
  }
}

TEST_F(ReplayBacktestDeterminism,
       lastSelectedBuySignalDoesNotUseTheExcludedNextOpen) {
  const auto selection = multiFillSelection(3);
  ASSERT_TRUE(selection.ok()) << selection.error().message;
  ASSERT_EQ(selection.value().bars.size(), 2U);
  const auto recorded = recordSelection(selection.value(), alternatingPlan());
  ASSERT_TRUE(recorded.ok()) << recorded.error().message;
  ASSERT_TRUE(recorded.value().backtest.has_value());
  const auto &backtest = *recorded.value().backtest;
  EXPECT_EQ(recorded.value().status, bte::results::RunStatus::completed);
  EXPECT_EQ(backtest.orderStatus,
            bte::engine::StarterOrderStatus::cancelledNoFutureMarketData);
  EXPECT_TRUE(backtest.fills.empty());
  EXPECT_EQ(backtest.positionShares, 0);
  EXPECT_EQ(backtest.cashMicrodollars, 10'000'000'000);
  EXPECT_EQ(backtest.pnlMicrodollars, 0);
  auto store =
      bte::results::ResultStore::open(root_ / "Results", root_ / "Data");
  ASSERT_TRUE(store.ok()) << store.error().message;
  const auto opened =
      store.value()->openResult(recorded.value().persisted.resultId);
  ASSERT_TRUE(opened.ok()) << opened.error().message;
  auto orderCount = std::size_t{0};
  for (const auto &record : opened.value().records) {
    EXPECT_NE(record.family, bte::results::RecordFamily::fill);
    EXPECT_NE(record.family, bte::results::RecordFamily::cost);
    if (record.family == bte::results::RecordFamily::order) {
      ++orderCount;
      EXPECT_EQ(record.timestamp, hour(2));
      EXPECT_EQ(record.side, bte::results::OrderSide::buy);
      EXPECT_EQ(record.quantityShares, 10);
    }
  }
  EXPECT_EQ(orderCount, 1U); // The flat sell signal is not an order or fill.
}

TEST_F(ReplayBacktestDeterminism,
       invalidSelectablePlanCreatesNoStagedOrCatalogResult) {
  const auto selection = multiFillSelection();
  ASSERT_TRUE(selection.ok()) << selection.error().message;
  const auto recorded = recordSelection(selection.value(), {});
  ASSERT_FALSE(recorded.ok());
  EXPECT_EQ(recorded.error().code, bte::core::ErrorCode::strategyCompileFailed);
  // Assert before reopening: recovery must not hide a forbidden staged write.
  EXPECT_TRUE(std::filesystem::is_empty(root_ / "Results" / "Staging"));
  EXPECT_TRUE(std::filesystem::is_empty(root_ / "Results" / "Results"));
  EXPECT_TRUE(std::filesystem::is_empty(root_ / "Results" / "Quarantine"));
  auto store =
      bte::results::ResultStore::open(root_ / "Results", root_ / "Data");
  ASSERT_TRUE(store.ok()) << store.error().message;
  const auto listed = store.value()->list();
  ASSERT_TRUE(listed.ok()) << listed.error().message;
  EXPECT_TRUE(listed.value().empty());
  EXPECT_TRUE(std::filesystem::is_empty(root_ / "Results" / "Staging"));
  EXPECT_TRUE(std::filesystem::is_empty(root_ / "Results" / "Results"));
  EXPECT_TRUE(std::filesystem::is_empty(root_ / "Results" / "Quarantine"));
}

TEST_F(ReplayBacktestDeterminism,
       noSignalSelectionPersistsAndReplaysZeroTradesAndUnchangedPortfolio) {
  const auto selection = multiFillSelection();
  ASSERT_TRUE(selection.ok()) << selection.error().message;
  auto plan = alternatingPlan();
  plan.buy.conditions.front().threshold = 98.0;
  plan.sell.conditions.front().threshold = 102.0;
  const auto recorded = recordSelection(selection.value(), plan);
  ASSERT_TRUE(recorded.ok()) << recorded.error().message;
  ASSERT_TRUE(recorded.value().backtest.has_value());
  EXPECT_EQ(recorded.value().backtest->orderStatus,
            bte::engine::StarterOrderStatus::completedNoSignal);
  EXPECT_TRUE(recorded.value().backtest->fills.empty());
  EXPECT_EQ(recorded.value().backtest->barsProcessed, 12U);
  EXPECT_EQ(recorded.value().backtest->pnlMicrodollars, 0);
  for (const auto timeframe :
       {bte::bindings::ResultReplayTimeframe::hourly,
        bte::bindings::ResultReplayTimeframe::dailyUtc}) {
    auto replay = bte::bindings::ResultReplay::open(
        root_ / "Results", root_ / "Data", recorded.value().persisted.resultId,
        timeframe);
    ASSERT_TRUE(replay.ok()) << replay.error().message;
    ASSERT_EQ(replay.value()->totalFrames(),
              timeframe == bte::bindings::ResultReplayTimeframe::hourly ? 12U
                                                                        : 1U);
    for (std::size_t index = 0; index < replay.value()->totalFrames();
         ++index) {
      ASSERT_TRUE(replay.value()->seek(index));
      const auto &frame = *replay.value()->current();
      EXPECT_TRUE(frame.fills.empty());
      EXPECT_EQ(frame.portfolio.positionShares, 0);
      EXPECT_DOUBLE_EQ(frame.portfolio.cash, 10000.0);
      EXPECT_DOUBLE_EQ(frame.portfolio.marketValue, 0.0);
      EXPECT_DOUBLE_EQ(frame.portfolio.equity, 10000.0);
      EXPECT_DOUBLE_EQ(frame.portfolio.pnl, 0.0);
    }
  }
}

} // namespace
