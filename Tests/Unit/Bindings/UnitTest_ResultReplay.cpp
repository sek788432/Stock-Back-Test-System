#include "Bte/Bindings/BacktestSessionVm.h"
#include "Bte/Bindings/ResultReplay.h"
#include "Bte/Bindings/ResultReplayRequests.h"

#include "Bte/Core/Cancellation.h"
#include "Bte/Core/Time.h"
#include "Bte/Data/ReleaseSnapshot.h"
#include "Bte/Results/ResultStore.h"

#include "ResultReplayRequestsTestHooks.h"
#include "ResultStoreTestHooks.h"

#include <gtest/gtest.h>
#include <sqlite3.h>

#include <QCoreApplication>
#include <QDate>
#include <QEventLoop>
#include <QTimer>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <thread>
#include <vector>

#if defined(__APPLE__) || defined(__linux__)
#include <sys/resource.h>
#endif

namespace {

std::int64_t peakResidentKibibytes() {
#if defined(__APPLE__) || defined(__linux__)
  rusage usage{};
  if (getrusage(RUSAGE_SELF, &usage) != 0) {
    return -1;
  }
#if defined(__APPLE__)
  return usage.ru_maxrss / 1'024;
#else
  return usage.ru_maxrss;
#endif
#else
  return -1;
#endif
}

void executeSql(const std::filesystem::path &path, const std::string &sql) {
  sqlite3 *rawDatabase = nullptr;
  ASSERT_EQ(sqlite3_open(path.string().c_str(), &rawDatabase), SQLITE_OK);
  const auto database = std::unique_ptr<sqlite3, decltype(&sqlite3_close)>{
      rawDatabase, &sqlite3_close};
  ASSERT_EQ(
      sqlite3_exec(database.get(), sql.c_str(), nullptr, nullptr, nullptr),
      SQLITE_OK);
}

class ResultReplayTest : public testing::Test {
protected:
  void SetUp() override {
    root_ =
        std::filesystem::temp_directory_path() /
        ("bte-result-replay-" +
         std::string{
             testing::UnitTest::GetInstance()->current_test_info()->name()});
    std::filesystem::remove_all(root_);
    std::filesystem::create_directories(root_ / "Source");
    std::ofstream source{root_ / "Source" / "SYN.csv"};
    source << "symbol,ts,open,high,low,close,volume,schemaName\n"
              "SYN,2024-01-01 23:00:00+00:00,100,102,99,101,1200,ohlcv-1h\n"
              "SYN,2024-01-02 00:00:00+00:00,101,104,100,103,2300,ohlcv-1h\n"
              "SYN,2024-01-02 01:00:00+00:00,103,105,102,104,3400,ohlcv-1h\n";
    source.close();
    auto built = bte::data::buildReleaseSnapshot({
        .sourceDirectory = root_ / "Source",
        .storeDirectory = root_ / "Data",
        .symbols = {"SYN"},
        .rowsPerSegment = 2,
        .calendarHash = std::string(64, 'a'),
        .splitManifestHash = std::string(64, 'b'),
    });
    ASSERT_TRUE(built.ok()) << built.error().message;
    segments_ = built.value().segments;
    snapshotId_ = built.value().snapshotId;
    auto reader =
        bte::data::ReleaseSnapshotReader::open(root_ / "Data", snapshotId_);
    ASSERT_TRUE(reader.ok()) << reader.error().message;
    auto selected = reader.value()->select({
        .symbols = {"SYN"},
        .range = {.start = timestamp("2024-01-01 23:00:00+00:00"),
                  .end = timestamp("2024-01-02 02:00:00+00:00")},
        .timeframe = "ohlcv-1h",
    });
    ASSERT_TRUE(selected.ok()) << selected.error().message;
    selection_ = std::move(selected).value().identity;
    const auto storage = bte::bindings::PersistedBacktestStorage{
        .resultStore = root_ / "Store",
        .dataStore = root_ / "Data",
        .snapshotId = snapshotId_,
        .strategyHash = std::string(64, 'c'),
    };
    auto recorded = bte::bindings::runPersistedBacktestConfiguration(
        {.symbol = "SYN",
         .schema = "ohlcv-1h",
         .startDate = QDate{2024, 1, 1},
         .endDate = QDate{2024, 1, 2},
         .initialCapital = 2'000,
         .quantityShares = 10,
         .selectableStrategy = {}},
        storage);
    ASSERT_TRUE(recorded.ok()) << recorded.error().message;
    ASSERT_EQ(recorded.value().resultId.size(), 32);
    ASSERT_EQ(recorded.value().canonicalResultHash.size(), 64);
    resultId_ = recorded.value().resultId;
    const auto published =
        bte::bindings::publishBacktestDraft(storage, resultId_);
    ASSERT_TRUE(published.ok()) << published.error().message;
    EXPECT_EQ(published.value(), resultId_);
    const auto repeated =
        bte::bindings::publishBacktestDraft(storage, resultId_);
    ASSERT_TRUE(repeated.ok()) << repeated.error().message;
    EXPECT_EQ(repeated.value(), resultId_);
  }

  void TearDown() override {
    bte::bindings::testing::clearResultReplayRequestHooks();
    bte::results::testing::clearFailure();
    std::filesystem::remove_all(root_);
  }

  static bte::core::Timestamp timestamp(const std::string &text) {
    return bte::core::time::parseIso8601(text).value();
  }

  bte::core::Result<bte::results::FinalizedResult>
  persist(const std::vector<bte::results::CanonicalRecord> &records,
          const bte::results::RunStatus status,
          const std::string &terminalReason = {}) const {
    auto store =
        bte::results::ResultStore::open(root_ / "CustomStore", root_ / "Data");
    if (!store.ok()) {
      return store.error();
    }
    auto writer = store.value()->begin({
        .universe = {"SYN"},
        .range = {.start = timestamp("2024-01-01 23:00:00+00:00"),
                  .end = timestamp("2024-01-02 02:00:00+00:00")},
        .initialCapitalMicrodollars = 2'000'000'000,
        .strategyId = "replay-boundary-fixture",
        .strategyHash = std::string(64, 'c'),
        .dataSelection = selection_,
    });
    if (!writer.ok()) {
      return writer.error();
    }
    if (!records.empty()) {
      auto appended = writer.value()->append(records);
      if (!appended.ok()) {
        return appended.error();
      }
    }
    const auto summary =
        status == bte::results::RunStatus::completed
            ? bte::results::FinalSummary{.finalEquityMicrodollars =
                                             2'000'000'000,
                                         .pnlMicrodollars = 0}
            : bte::results::FinalSummary{};
    return writer.value()->finalizeAndPromote(status, summary, terminalReason);
  }

  std::filesystem::path root_;
  std::vector<std::string> segments_;
  std::string snapshotId_;
  std::string resultId_;
  bte::data::DataSelectionIdentity selection_;
};

TEST_F(ResultReplayTest,
       hourlyOpenImmediatelyPresentsFirstFrameAndIndexedStepsStaySynchronized) {
  auto replay = bte::bindings::ResultReplay::open(
      root_ / "Store", root_ / "Data", resultId_,
      bte::bindings::ResultReplayTimeframe::hourly);
  ASSERT_TRUE(replay.ok()) << replay.error().message;
  ASSERT_NE(replay.value()->current(), nullptr);
  EXPECT_EQ(replay.value()->current()->candle.ts,
            timestamp("2024-01-01 23:00:00+00:00"));
  EXPECT_EQ(replay.value()->currentIndex(), 0);
  EXPECT_EQ(replay.value()->totalFrames(), 3);
  EXPECT_EQ(replay.value()->timeline().size(), 3U);
  EXPECT_EQ(replay.value()->timeline().front().candle.ts,
            replay.value()->current()->candle.ts);

  ASSERT_TRUE(replay.value()->stepForward());
  ASSERT_EQ(replay.value()->current()->fills.size(), 1);
  EXPECT_TRUE(replay.value()->current()->fills.front().isBuy);
  EXPECT_EQ(replay.value()->current()->portfolio.positionShares, 10);
  EXPECT_TRUE(replay.value()->seek(2));
  EXPECT_EQ(replay.value()->progressPercent(), 100);
  EXPECT_TRUE(replay.value()->stepBack());
  EXPECT_EQ(replay.value()->currentIndex(), 1);
  EXPECT_FALSE(replay.value()->seek(3));
}

TEST_F(ResultReplayTest,
       realAaplResultPreservesSelectedBarsAndUtcHourlyDailyReplay) {
  using namespace std::chrono;
  const auto repositoryRoot = std::filesystem::path{__FILE__}
                                  .parent_path()
                                  .parent_path()
                                  .parent_path()
                                  .parent_path();
  const auto sourceDirectory = repositoryRoot / "StockData" / "Extracted";
  ASSERT_TRUE(std::filesystem::is_regular_file(sourceDirectory /
                                               "SourceManifest.btesource"));
  ASSERT_TRUE(std::filesystem::is_regular_file(sourceDirectory / "AAPL.csv"));

  const auto dataStore = root_ / "RealData";
  const auto resultStore = root_ / "RealResults";
  const auto built = bte::data::buildReleaseSnapshot({
      .sourceDirectory = sourceDirectory,
      .storeDirectory = dataStore,
      .symbols = {"AAPL"},
      .rowsPerSegment = 512,
      .calendarHash = std::string(64, 'a'),
      .splitManifestHash = std::string(64, 'b'),
  });
  ASSERT_TRUE(built.ok()) << built.error().message;
  EXPECT_EQ(built.value().barCount, 31'466U);

  const auto selectedRange = bte::core::DateRange{
      .start = timestamp("2018-05-01 00:00:00+00:00"),
      .end = timestamp("2026-05-07 00:00:00+00:00"),
  };
  auto snapshotReader = bte::data::ReleaseSnapshotReader::open(
      dataStore, built.value().snapshotId);
  ASSERT_TRUE(snapshotReader.ok()) << snapshotReader.error().message;
  auto expectedSelection = snapshotReader.value()->select({
      .symbols = {"AAPL"},
      .range = selectedRange,
      .timeframe = "ohlcv-1h",
  });
  ASSERT_TRUE(expectedSelection.ok()) << expectedSelection.error().message;
  ASSERT_EQ(expectedSelection.value().bars.size(), 31'466U);

  const auto storage = bte::bindings::PersistedBacktestStorage{
      .resultStore = resultStore,
      .dataStore = dataStore,
      .snapshotId = built.value().snapshotId,
      .strategyHash = std::string(64, 'c'),
  };
  const auto run = bte::bindings::runPersistedBacktestConfiguration(
      {.symbol = "AAPL",
       .schema = "ohlcv-1h",
       .startDate = QDate{2018, 5, 1},
       .endDate = QDate{2026, 5, 6},
       .initialCapital = 1'000'000,
       .quantityShares = 1,
       .selectableStrategy = {}},
      storage);
  ASSERT_TRUE(run.ok()) << run.error().message;

  auto store = bte::results::ResultStore::open(resultStore, dataStore);
  ASSERT_TRUE(store.ok()) << store.error().message;
  const auto published = store.value()->publishDraft(run.value().resultId);
  ASSERT_TRUE(published.ok()) << published.error().message;
  const auto opened = store.value()->openResultWithData(run.value().resultId);
  ASSERT_TRUE(opened.ok()) << opened.error().message;
  EXPECT_EQ(opened.value().selection.identity,
            expectedSelection.value().identity);
  EXPECT_EQ(opened.value().result.descriptor.range.start, selectedRange.start);
  EXPECT_EQ(opened.value().result.descriptor.range.end, selectedRange.end);
  ASSERT_EQ(opened.value().selection.bars.size(), 31'466U);
  EXPECT_EQ(opened.value().selection.bars, expectedSelection.value().bars);
  auto expectedFills = std::vector<bte::bindings::ResultReplayFill>{};
  for (const auto &record : opened.value().result.records) {
    if (record.family != bte::results::RecordFamily::fill) {
      continue;
    }
    ASSERT_TRUE(record.quantityShares.has_value());
    ASSERT_TRUE(record.priceNanodollars.has_value());
    ASSERT_TRUE(record.amountMicrodollars.has_value());
    ASSERT_TRUE(record.side == bte::results::OrderSide::buy ||
                record.side == bte::results::OrderSide::sell);
    EXPECT_GT(*record.quantityShares, 0);
    EXPECT_GT(*record.priceNanodollars, 0);
    EXPECT_GT(*record.amountMicrodollars, 0);
    expectedFills.push_back(bte::bindings::ResultReplayFill{
        .timestamp = record.timestamp,
        .isBuy = record.side == bte::results::OrderSide::buy,
        .quantityShares = *record.quantityShares,
        .price =
            static_cast<double>(*record.priceNanodollars) / 1'000'000'000.0,
        .amount = static_cast<double>(*record.amountMicrodollars) / 1'000'000.0,
        .barIndex = static_cast<std::size_t>(std::ranges::distance(
            expectedSelection.value().bars.begin(),
            std::ranges::lower_bound(expectedSelection.value().bars,
                                     record.timestamp, {},
                                     &bte::data::SnapshotBar::timestamp))),
    });
  }
  ASSERT_FALSE(expectedFills.empty())
      << "The saved AAPL Backtest must contain at least one fill";

  auto hourly = bte::bindings::ResultReplay::open(
      resultStore, dataStore, run.value().resultId,
      bte::bindings::ResultReplayTimeframe::hourly);
  ASSERT_TRUE(hourly.ok()) << hourly.error().message;
  ASSERT_EQ(hourly.value()->totalFrames(), 31'466U);
  ASSERT_EQ(hourly.value()->timeline().size(), 31'466U);
  auto hourlyFills = std::vector<bte::bindings::ResultReplayFill>{};
  for (std::size_t index = 0; index < expectedSelection.value().bars.size();
       ++index) {
    ASSERT_TRUE(hourly.value()->seek(index));
    const auto &frame = *hourly.value()->current();
    const auto &sourceBar = expectedSelection.value().bars[index];
    EXPECT_EQ(frame.candle.ts, sourceBar.timestamp);
    EXPECT_DOUBLE_EQ(frame.candle.open,
                     static_cast<double>(sourceBar.openNanodollars) /
                         1'000'000'000.0);
    EXPECT_DOUBLE_EQ(frame.candle.high,
                     static_cast<double>(sourceBar.highNanodollars) /
                         1'000'000'000.0);
    EXPECT_DOUBLE_EQ(frame.candle.low,
                     static_cast<double>(sourceBar.lowNanodollars) /
                         1'000'000'000.0);
    EXPECT_DOUBLE_EQ(frame.candle.close,
                     static_cast<double>(sourceBar.closeNanodollars) /
                         1'000'000'000.0);
    EXPECT_DOUBLE_EQ(frame.candle.volume,
                     static_cast<double>(sourceBar.volumeMicroshares) /
                         1'000'000.0);
    hourlyFills.insert(hourlyFills.end(), frame.fills.begin(),
                       frame.fills.end());
  }

  auto daily = bte::bindings::ResultReplay::open(
      resultStore, dataStore, run.value().resultId,
      bte::bindings::ResultReplayTimeframe::dailyUtc);
  ASSERT_TRUE(daily.ok()) << daily.error().message;
  auto expectedDaily = std::vector<bte::core::Bar>{};
  for (const auto &sourceBar : expectedSelection.value().bars) {
    const auto dataDay = bte::core::Timestamp{floor<days>(sourceBar.timestamp)};
    const auto open =
        static_cast<double>(sourceBar.openNanodollars) / 1'000'000'000.0;
    const auto high =
        static_cast<double>(sourceBar.highNanodollars) / 1'000'000'000.0;
    const auto low =
        static_cast<double>(sourceBar.lowNanodollars) / 1'000'000'000.0;
    const auto close =
        static_cast<double>(sourceBar.closeNanodollars) / 1'000'000'000.0;
    const auto volume =
        static_cast<double>(sourceBar.volumeMicroshares) / 1'000'000.0;
    if (expectedDaily.empty() || expectedDaily.back().ts != dataDay) {
      expectedDaily.push_back(bte::core::Bar{.ts = dataDay,
                                             .open = open,
                                             .high = high,
                                             .low = low,
                                             .close = close,
                                             .volume = volume});
      continue;
    }
    expectedDaily.back().high = std::max(expectedDaily.back().high, high);
    expectedDaily.back().low = std::min(expectedDaily.back().low, low);
    expectedDaily.back().close = close;
    expectedDaily.back().volume += volume;
  }
  ASSERT_EQ(expectedDaily.size(), 2'014U);
  ASSERT_EQ(daily.value()->totalFrames(), expectedDaily.size());
  ASSERT_EQ(daily.value()->timeline().size(), expectedDaily.size());
  auto dailyFills = std::vector<bte::bindings::ResultReplayFill>{};
  auto lastHourlyIndex = std::size_t{0};
  auto sawMayFourth = false;
  auto sawMayFifth = false;
  for (std::size_t index = 0; index < expectedDaily.size(); ++index) {
    ASSERT_TRUE(daily.value()->seek(index));
    const auto &frame = *daily.value()->current();
    EXPECT_EQ(frame.candle.ts, expectedDaily[index].ts);
    EXPECT_DOUBLE_EQ(frame.candle.open, expectedDaily[index].open);
    EXPECT_DOUBLE_EQ(frame.candle.high, expectedDaily[index].high);
    EXPECT_DOUBLE_EQ(frame.candle.low, expectedDaily[index].low);
    EXPECT_DOUBLE_EQ(frame.candle.close, expectedDaily[index].close);
    EXPECT_DOUBLE_EQ(frame.candle.volume, expectedDaily[index].volume);
    EXPECT_FALSE(frame.partialUtcDay);
    if (expectedDaily[index].ts == timestamp("2026-05-04 00:00:00+00:00")) {
      EXPECT_DOUBLE_EQ(frame.candle.open, 279.82);
      EXPECT_DOUBLE_EQ(frame.candle.high, 280.63);
      EXPECT_DOUBLE_EQ(frame.candle.low, 274.86);
      EXPECT_DOUBLE_EQ(frame.candle.close, 276.50);
      EXPECT_DOUBLE_EQ(frame.candle.volume, 14'649'894.0);
      sawMayFourth = true;
    }
    if (expectedDaily[index].ts == timestamp("2026-05-05 00:00:00+00:00")) {
      EXPECT_DOUBLE_EQ(frame.candle.open, 276.54);
      EXPECT_DOUBLE_EQ(frame.candle.high, 284.56);
      EXPECT_DOUBLE_EQ(frame.candle.low, 275.22);
      EXPECT_DOUBLE_EQ(frame.candle.close, 282.49);
      EXPECT_DOUBLE_EQ(frame.candle.volume, 17'512'103.0);
      sawMayFifth = true;
    }
    const auto dataDay = floor<days>(frame.candle.ts);
    const auto hourlyTimeline = hourly.value()->timeline();
    while (lastHourlyIndex + 1U < hourlyTimeline.size() &&
           floor<days>(hourlyTimeline[lastHourlyIndex + 1U].candle.ts) <=
               dataDay) {
      ++lastHourlyIndex;
    }
    EXPECT_EQ(frame.portfolio, hourlyTimeline[lastHourlyIndex].portfolio);
    dailyFills.insert(dailyFills.end(), frame.fills.begin(), frame.fills.end());
  }
  EXPECT_TRUE(sawMayFourth);
  EXPECT_TRUE(sawMayFifth);
  EXPECT_EQ(hourlyFills, expectedFills);
  EXPECT_EQ(dailyFills, expectedFills);
}

TEST_F(ResultReplayTest, openUsesExactPersistedSpansInsteadOfDescriptorRange) {
  auto reader =
      bte::data::ReleaseSnapshotReader::open(root_ / "Data", snapshotId_);
  ASSERT_TRUE(reader.ok()) << reader.error().message;
  auto narrow = reader.value()->select({
      .symbols = {"SYN"},
      .range = {.start = timestamp("2024-01-02 00:00:00+00:00"),
                .end = timestamp("2024-01-02 01:00:00+00:00")},
      .timeframe = "ohlcv-1h",
  });
  ASSERT_TRUE(narrow.ok()) << narrow.error().message;
  // Source has a bar before start and exactly at end; neither is selected.
  ASSERT_EQ(narrow.value().bars.size(), 1U);
  EXPECT_EQ(narrow.value().bars.front().timestamp,
            timestamp("2024-01-02 00:00:00+00:00"));
  auto store =
      bte::results::ResultStore::open(root_ / "ExactStore", root_ / "Data");
  ASSERT_TRUE(store.ok()) << store.error().message;
  auto writer = store.value()->begin({
      .universe = {"SYN"},
      .range = {.start = timestamp("2024-01-01 23:00:00+00:00"),
                .end = timestamp("2024-01-02 02:00:00+00:00")},
      .initialCapitalMicrodollars = 2'000'000'000,
      .strategyId = "exact-span-fixture",
      .strategyHash = std::string(64, 'c'),
      .dataSelection = narrow.value().identity,
  });
  ASSERT_TRUE(writer.ok()) << writer.error().message;
  ASSERT_TRUE(writer.value()
                  ->append({{.sequence = 0,
                             .timestamp = narrow.value().bars.front().timestamp,
                             .symbol = "SYN",
                             .family = bte::results::RecordFamily::portfolio,
                             .cashMicrodollars = 2'000'000'000,
                             .marketValueMicrodollars = 0,
                             .equityMicrodollars = 2'000'000'000,
                             .pnlMicrodollars = 123'000'000,
                             .positionShares = 0}})
                  .ok());
  auto finalized = writer.value()->finalizeAndPromote(
      bte::results::RunStatus::completed,
      {.finalEquityMicrodollars = 2'000'000'000,
       .pnlMicrodollars = 123'000'000});
  ASSERT_TRUE(finalized.ok()) << finalized.error().message;

  auto replay = bte::bindings::ResultReplay::open(
      root_ / "ExactStore", root_ / "Data", finalized.value().resultId,
      bte::bindings::ResultReplayTimeframe::hourly);
  ASSERT_TRUE(replay.ok()) << replay.error().message;
  ASSERT_EQ(replay.value()->totalFrames(), 1U);
  EXPECT_EQ(replay.value()->current()->candle.ts,
            timestamp("2024-01-02 00:00:00+00:00"));
  EXPECT_DOUBLE_EQ(replay.value()->current()->candle.open, 101);
  EXPECT_DOUBLE_EQ(replay.value()->current()->candle.volume, 2300);
  EXPECT_TRUE(replay.value()->current()->fills.empty());
  EXPECT_FALSE(replay.value()->stepForward());
  EXPECT_DOUBLE_EQ(replay.value()->current()->portfolio.pnl, 123.0);
  auto daily = bte::bindings::ResultReplay::open(
      root_ / "ExactStore", root_ / "Data", finalized.value().resultId,
      bte::bindings::ResultReplayTimeframe::dailyUtc);
  ASSERT_TRUE(daily.ok()) << daily.error().message;
  ASSERT_EQ(daily.value()->totalFrames(), 1U);
  ASSERT_NE(daily.value()->current(), nullptr);
  EXPECT_DOUBLE_EQ(daily.value()->current()->candle.open, 101);
  EXPECT_DOUBLE_EQ(daily.value()->current()->candle.high, 104);
  EXPECT_DOUBLE_EQ(daily.value()->current()->candle.low, 100);
  EXPECT_DOUBLE_EQ(daily.value()->current()->candle.close, 103);
  EXPECT_DOUBLE_EQ(daily.value()->current()->candle.volume, 2300);
  EXPECT_TRUE(daily.value()->current()->fills.empty());
  EXPECT_TRUE(daily.value()->current()->partialUtcDay);
  EXPECT_EQ(daily.value()->current()->portfolio,
            replay.value()->current()->portfolio);
  EXPECT_FALSE(daily.value()->stepForward());
}

TEST_F(ResultReplayTest, openUsesExactPersistedSpansAtTheExclusiveEndBoundary) {
  auto reader =
      bte::data::ReleaseSnapshotReader::open(root_ / "Data", snapshotId_);
  ASSERT_TRUE(reader.ok()) << reader.error().message;
  auto narrow = reader.value()->select({
      .symbols = {"SYN"},
      .range = {.start = timestamp("2024-01-02 01:00:00+00:00"),
                .end = timestamp("2024-01-02 02:00:00+00:00")},
      .timeframe = "ohlcv-1h",
  });
  ASSERT_TRUE(narrow.ok()) << narrow.error().message;
  ASSERT_EQ(narrow.value().bars.size(), 1U);
  auto store = bte::results::ResultStore::open(root_ / "EndBoundaryStore",
                                               root_ / "Data");
  ASSERT_TRUE(store.ok()) << store.error().message;
  auto writer = store.value()->begin({
      .universe = {"SYN"},
      .range = {.start = timestamp("2024-01-01 23:00:00+00:00"),
                .end = timestamp("2024-01-02 02:00:00+00:00")},
      .initialCapitalMicrodollars = 2'000'000'000,
      .strategyId = "exact-span-end-boundary",
      .strategyHash = std::string(64, 'c'),
      .dataSelection = narrow.value().identity,
  });
  ASSERT_TRUE(writer.ok()) << writer.error().message;
  ASSERT_TRUE(writer.value()
                  ->append({{.sequence = 0,
                             .timestamp = narrow.value().bars.front().timestamp,
                             .symbol = "SYN",
                             .family = bte::results::RecordFamily::portfolio,
                             .cashMicrodollars = 2'000'000'000,
                             .marketValueMicrodollars = 0,
                             .equityMicrodollars = 2'000'000'000,
                             .pnlMicrodollars = 123'000'000,
                             .positionShares = 0}})
                  .ok());
  const auto finalized = writer.value()->finalizeAndPromote(
      bte::results::RunStatus::completed,
      {.finalEquityMicrodollars = 2'000'000'000,
       .pnlMicrodollars = 123'000'000});
  ASSERT_TRUE(finalized.ok()) << finalized.error().message;

  auto replay = bte::bindings::ResultReplay::open(
      root_ / "EndBoundaryStore", root_ / "Data", finalized.value().resultId,
      bte::bindings::ResultReplayTimeframe::hourly);
  ASSERT_TRUE(replay.ok()) << replay.error().message;
  ASSERT_EQ(replay.value()->totalFrames(), 1U);
  EXPECT_EQ(replay.value()->current()->candle.ts,
            timestamp("2024-01-02 01:00:00+00:00"));
  EXPECT_EQ(replay.value()->current()->portfolio.pnl, 123.0);

  auto daily = bte::bindings::ResultReplay::open(
      root_ / "EndBoundaryStore", root_ / "Data", finalized.value().resultId,
      bte::bindings::ResultReplayTimeframe::dailyUtc);
  ASSERT_TRUE(daily.ok()) << daily.error().message;
  ASSERT_EQ(daily.value()->totalFrames(), 1U);
  ASSERT_NE(daily.value()->current(), nullptr);
  EXPECT_EQ(daily.value()->current()->candle.ts,
            timestamp("2024-01-02 00:00:00+00:00"));
  EXPECT_DOUBLE_EQ(daily.value()->current()->candle.open, 103);
  EXPECT_DOUBLE_EQ(daily.value()->current()->candle.high, 105);
  EXPECT_DOUBLE_EQ(daily.value()->current()->candle.low, 102);
  EXPECT_DOUBLE_EQ(daily.value()->current()->candle.close, 104);
  EXPECT_DOUBLE_EQ(daily.value()->current()->candle.volume, 3400);
  EXPECT_TRUE(daily.value()->current()->partialUtcDay);
}

TEST_F(ResultReplayTest, resultStoreRejectsMalformedCanonicalReplayPayloads) {
  const auto at = timestamp("2024-01-01 23:00:00+00:00");
  auto missingPnl = persist({{.sequence = 0,
                              .timestamp = at,
                              .symbol = "SYN",
                              .family = bte::results::RecordFamily::portfolio,
                              .cashMicrodollars = 2'000'000'000,
                              .marketValueMicrodollars = 0,
                              .equityMicrodollars = 2'000'000'000,
                              .positionShares = 0}},
                            bte::results::RunStatus::completed);
  ASSERT_FALSE(missingPnl.ok());
  EXPECT_EQ(missingPnl.error().code, bte::core::ErrorCode::invalidArgument);

  auto malformedFill = persist({{.sequence = 0,
                                 .timestamp = at,
                                 .symbol = "SYN",
                                 .family = bte::results::RecordFamily::fill,
                                 .side = bte::results::OrderSide::none,
                                 .quantityShares = 10,
                                 .priceNanodollars = 100'000'000'000,
                                 .amountMicrodollars = 1'000'000'000}},
                               bte::results::RunStatus::completed);
  ASSERT_FALSE(malformedFill.ok());
  EXPECT_EQ(malformedFill.error().code, bte::core::ErrorCode::invalidArgument);
}

TEST_F(ResultReplayTest,
       replayRejectsSpanAndRecordSymbolsOutsideDeclaredUniverse) {
  auto store =
      bte::results::ResultStore::open(root_ / "SymbolStore", root_ / "Data");
  ASSERT_TRUE(store.ok()) << store.error().message;
  auto mismatchedSpan = store.value()->begin({
      .universe = {"OTHER"},
      .range = {.start = timestamp("2024-01-01 23:00:00+00:00"),
                .end = timestamp("2024-01-02 02:00:00+00:00")},
      .initialCapitalMicrodollars = 2'000'000'000,
      .strategyId = "mismatched-span-fixture",
      .strategyHash = std::string(64, 'c'),
      .dataSelection = selection_,
  });
  EXPECT_FALSE(mismatchedSpan.ok());

  auto mismatchedRecord =
      persist({{.sequence = 0,
                .timestamp = timestamp("2024-01-01 23:00:00+00:00"),
                .symbol = "OTHER",
                .family = bte::results::RecordFamily::portfolio,
                .cashMicrodollars = 2'000'000'000,
                .marketValueMicrodollars = 0,
                .equityMicrodollars = 2'000'000'000,
                .pnlMicrodollars = 0,
                .positionShares = 0}},
              bte::results::RunStatus::completed);
  ASSERT_FALSE(mismatchedRecord.ok());
  EXPECT_EQ(mismatchedRecord.error().code,
            bte::core::ErrorCode::invalidArgument);
}

TEST_F(ResultReplayTest,
       dailyUtcAggregationSumsVolumeAndRecognizesCompleteSourceDays) {
  auto replay = bte::bindings::ResultReplay::open(
      root_ / "Store", root_ / "Data", resultId_,
      bte::bindings::ResultReplayTimeframe::dailyUtc);
  ASSERT_TRUE(replay.ok()) << replay.error().message;
  ASSERT_EQ(replay.value()->totalFrames(), 2);
  ASSERT_NE(replay.value()->current(), nullptr);
  EXPECT_FALSE(replay.value()->current()->partialUtcDay);
  EXPECT_DOUBLE_EQ(replay.value()->current()->candle.volume, 1200);
  ASSERT_TRUE(replay.value()->stepForward());
  EXPECT_FALSE(replay.value()->current()->partialUtcDay);
  EXPECT_DOUBLE_EQ(replay.value()->current()->candle.open, 101);
  EXPECT_DOUBLE_EQ(replay.value()->current()->candle.high, 105);
  EXPECT_DOUBLE_EQ(replay.value()->current()->candle.low, 100);
  EXPECT_DOUBLE_EQ(replay.value()->current()->candle.close, 104);
  EXPECT_DOUBLE_EQ(replay.value()->current()->candle.volume, 5700);
  ASSERT_EQ(replay.value()->current()->fills.size(), 1);
}

TEST_F(ResultReplayTest, cancellationAndMissingImmutableDataFailClosed) {
  bte::core::CancellationSource cancellation;
  cancellation.requestCancellation();
  const auto cancelled = bte::bindings::ResultReplay::open(
      root_ / "Store", root_ / "Data", resultId_,
      bte::bindings::ResultReplayTimeframe::hourly, cancellation.token());
  ASSERT_FALSE(cancelled.ok());
  EXPECT_EQ(cancelled.error().code, bte::core::ErrorCode::cancelled);

  ASSERT_FALSE(segments_.empty());
  std::filesystem::remove(root_ / "Data" / "Segments" /
                          (segments_.front() + ".btedata"));
  const auto unavailable = bte::bindings::ResultReplay::open(
      root_ / "Store", root_ / "Data", resultId_,
      bte::bindings::ResultReplayTimeframe::hourly);
  ASSERT_FALSE(unavailable.ok());
  EXPECT_EQ(unavailable.error().code,
            bte::core::ErrorCode::dataSnapshotUnavailable);

  const auto persistedBacktest =
      bte::bindings::runPersistedBacktestConfiguration(
          {.symbol = "SYN",
           .schema = "ohlcv-1h",
           .startDate = QDate{2024, 1, 1},
           .endDate = QDate{2024, 1, 2},
           .initialCapital = 2'000,
           .quantityShares = 10,
           .selectableStrategy = {}},
          {.resultStore = root_ / "MissingSegmentStore",
           .dataStore = root_ / "Data",
           .snapshotId = snapshotId_,
           .strategyHash = std::string(64, 'c')});
  ASSERT_FALSE(persistedBacktest.ok());
  EXPECT_EQ(persistedBacktest.error().code,
            bte::core::ErrorCode::dataSnapshotUnavailable);

  std::filesystem::remove(root_ / "Data" / "Snapshots" / snapshotId_ /
                          "Manifest.btesnapshot");
  const auto missingManifest = bte::bindings::ResultReplay::open(
      root_ / "Store", root_ / "Data", resultId_,
      bte::bindings::ResultReplayTimeframe::hourly);
  ASSERT_FALSE(missingManifest.ok());
  EXPECT_EQ(missingManifest.error().code,
            bte::core::ErrorCode::dataSnapshotUnavailable);
}

TEST_F(ResultReplayTest, multiSymbolResultsAreRejectedBeforeReplay) {
  std::filesystem::create_directories(root_ / "MultiSource");
  std::ofstream first{root_ / "MultiSource" / "AAA.csv"};
  first << "symbol,ts,open,high,low,close,volume,schemaName\n"
           "AAA,2024-01-01 23:00:00+00:00,10,11,9,10,100,ohlcv-1h\n";
  first.close();
  std::ofstream second{root_ / "MultiSource" / "BBB.csv"};
  second << "symbol,ts,open,high,low,close,volume,schemaName\n"
            "BBB,2024-01-01 23:00:00+00:00,20,21,19,20,200,ohlcv-1h\n";
  second.close();
  auto built = bte::data::buildReleaseSnapshot({
      .sourceDirectory = root_ / "MultiSource",
      .storeDirectory = root_ / "MultiData",
      .symbols = {"AAA", "BBB"},
      .rowsPerSegment = 2,
      .calendarHash = std::string(64, 'a'),
      .splitManifestHash = std::string(64, 'b'),
  });
  ASSERT_TRUE(built.ok()) << built.error().message;
  auto reader = bte::data::ReleaseSnapshotReader::open(
      root_ / "MultiData", built.value().snapshotId);
  ASSERT_TRUE(reader.ok()) << reader.error().message;
  const auto range =
      bte::core::DateRange{.start = timestamp("2024-01-01 23:00:00+00:00"),
                           .end = timestamp("2024-01-02 00:00:00+00:00")};
  auto selected = reader.value()->select(
      {.symbols = {"AAA", "BBB"}, .range = range, .timeframe = "ohlcv-1h"});
  ASSERT_TRUE(selected.ok()) << selected.error().message;
  auto store = bte::results::ResultStore::open(root_ / "MultiStore",
                                               root_ / "MultiData");
  ASSERT_TRUE(store.ok()) << store.error().message;
  auto writer = store.value()->begin({
      .universe = {"AAA", "BBB"},
      .range = range,
      .initialCapitalMicrodollars = 2'000'000'000,
      .strategyId = "multi-symbol-fixture",
      .strategyHash = std::string(64, 'c'),
      .dataSelection = selected.value().identity,
  });
  ASSERT_TRUE(writer.ok()) << writer.error().message;
  auto finalized = writer.value()->finalizeAndPromote(
      bte::results::RunStatus::completed,
      {.finalEquityMicrodollars = 2'000'000'000, .pnlMicrodollars = 0});
  ASSERT_TRUE(finalized.ok()) << finalized.error().message;

  const auto replay = bte::bindings::ResultReplay::open(
      root_ / "MultiStore", root_ / "MultiData", finalized.value().resultId,
      bte::bindings::ResultReplayTimeframe::hourly);

  ASSERT_FALSE(replay.ok());
  EXPECT_EQ(replay.error().code, bte::core::ErrorCode::schemaMismatch);
  EXPECT_EQ(replay.error().message,
            "Multi-symbol Result Replay is unsupported");
}

TEST_F(ResultReplayTest, persistedBacktestRejectsUnsupportedSourceSchema) {
  const auto unsupported = bte::bindings::runPersistedBacktestConfiguration(
      {.symbol = "SYN",
       .schema = "ohlcv-1d",
       .startDate = QDate{2024, 1, 1},
       .endDate = QDate{2024, 1, 2},
       .initialCapital = 2'000,
       .quantityShares = 10,
       .selectableStrategy = {}},
      {.resultStore = root_ / "Store",
       .dataStore = root_ / "Data",
       .snapshotId = snapshotId_,
       .strategyHash = std::string(64, 'c')});
  ASSERT_FALSE(unsupported.ok());
  EXPECT_EQ(unsupported.error().code, bte::core::ErrorCode::schemaMismatch);
}

TEST_F(ResultReplayTest, openRejectsForgedTimeframeAndIncompatiblePolicy) {
  const auto invalidTimeframe = bte::bindings::ResultReplay::open(
      root_ / "Store", root_ / "Data", resultId_,
      static_cast<bte::bindings::ResultReplayTimeframe>(255));
  ASSERT_FALSE(invalidTimeframe.ok());
  EXPECT_EQ(invalidTimeframe.error().code,
            bte::core::ErrorCode::schemaMismatch);

  executeSql(root_ / "Store" / "Results" / (resultId_ + ".bteresult"),
             "UPDATE result_meta SET aggregation_policy='unsupported'");
  const auto incompatible = bte::bindings::ResultReplay::open(
      root_ / "Store", root_ / "Data", resultId_,
      bte::bindings::ResultReplayTimeframe::hourly);
  ASSERT_FALSE(incompatible.ok());
  EXPECT_EQ(incompatible.error().code, bte::core::ErrorCode::schemaMismatch);
}

TEST_F(ResultReplayTest,
       invalidStrategyDoesNotCreateAStagedOrCatalogVisibleResult) {
  const auto validationStore = root_ / "ValidationStore";
  const auto invalid = bte::bindings::runPersistedBacktestConfiguration(
      {.symbol = "SYN",
       .schema = "ohlcv-1h",
       .startDate = QDate{2024, 1, 1},
       .endDate = QDate{2024, 1, 2},
       .initialCapital = 2'000,
       .quantityShares = 10,
       .selectableStrategy = bte::strategy::SelectableStrategyPlan{}},
      {.resultStore = validationStore,
       .dataStore = root_ / "Data",
       .snapshotId = snapshotId_,
       .strategyHash = std::string(64, 'c')});

  ASSERT_FALSE(invalid.ok());
  EXPECT_EQ(invalid.error().code, bte::core::ErrorCode::strategyCompileFailed);
  auto store = bte::results::ResultStore::open(validationStore, root_ / "Data");
  ASSERT_TRUE(store.ok()) << store.error().message;
  auto listed = store.value()->list();
  ASSERT_TRUE(listed.ok()) << listed.error().message;
  EXPECT_TRUE(listed.value().empty());
  EXPECT_TRUE(std::filesystem::is_empty(validationStore / "Staging"));
}

TEST_F(ResultReplayTest,
       operationalFailurePersistsDiagnosticButReturnsAnErrorToBacktestUi) {
  const auto sourceDirectory = root_ / "FailureSource";
  const auto dataDirectory = root_ / "FailureData";
  const auto resultDirectory = root_ / "FailureStore";
  std::filesystem::create_directories(sourceDirectory);
  std::ofstream source{sourceDirectory / "SYN.csv"};
  source
      << "symbol,ts,open,high,low,close,volume,schemaName\n"
         "SYN,2024-01-01 00:00:00+00:00,1,1,1,1,1,ohlcv-1h\n"
         "SYN,2024-01-01 01:00:00+00:00,1,9000000000,1,9000000000,1,ohlcv-1h\n";
  source.close();
  auto built = bte::data::buildReleaseSnapshot({
      .sourceDirectory = sourceDirectory,
      .storeDirectory = dataDirectory,
      .symbols = {"SYN"},
      .rowsPerSegment = 2,
      .calendarHash = std::string(64, 'a'),
      .splitManifestHash = std::string(64, 'b'),
  });
  ASSERT_TRUE(built.ok()) << built.error().message;
  const auto unsafeBoundary =
      static_cast<double>(std::numeric_limits<std::int64_t>::max()) /
      1'000'000.0;

  const auto failed = bte::bindings::runPersistedBacktestConfiguration(
      {.symbol = "SYN",
       .schema = "ohlcv-1h",
       .startDate = QDate{2024, 1, 1},
       .endDate = QDate{2024, 1, 1},
       .initialCapital = std::nextafter(unsafeBoundary, 0.0),
       .quantityShares = 1,
       .selectableStrategy = {}},
      {.resultStore = resultDirectory,
       .dataStore = dataDirectory,
       .snapshotId = built.value().snapshotId,
       .strategyHash = std::string(64, 'c')});

  ASSERT_FALSE(failed.ok());
  EXPECT_EQ(failed.error().code, bte::core::ErrorCode::invalidArgument);
  auto store = bte::results::ResultStore::open(resultDirectory, dataDirectory);
  ASSERT_TRUE(store.ok()) << store.error().message;
  auto listed = store.value()->list();
  ASSERT_TRUE(listed.ok()) << listed.error().message;
  EXPECT_TRUE(listed.value().empty());
  auto drafts = store.value()->listDrafts();
  ASSERT_TRUE(drafts.ok()) << drafts.error().message;
  ASSERT_EQ(drafts.value().size(), 1U);
  EXPECT_EQ(drafts.value().front().status, bte::results::RunStatus::failed);
  EXPECT_TRUE(drafts.value().front().available);
}

TEST_F(ResultReplayTest,
       persistedBacktestCompositionPropagatesEveryBoundaryFailure) {
  const auto validConfiguration = bte::bindings::BacktestConfiguration{
      .symbol = "SYN",
      .schema = "ohlcv-1h",
      .startDate = QDate{2024, 1, 1},
      .endDate = QDate{2024, 1, 2},
      .initialCapital = 2'000,
      .quantityShares = 10,
      .selectableStrategy = {},
  };
  const auto validStorage = bte::bindings::PersistedBacktestStorage{
      .resultStore = root_ / "BoundaryStore",
      .dataStore = root_ / "Data",
      .snapshotId = snapshotId_,
      .strategyHash = std::string(64, 'c'),
  };
  auto expectError = [&](const auto &configuration, const auto &storage,
                         const bte::core::ErrorCode expected) {
    const auto result = bte::bindings::runPersistedBacktestConfiguration(
        configuration, storage);
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.error().code, expected);
  };

  auto invalidDate = validConfiguration;
  invalidDate.startDate = QDate{};
  expectError(invalidDate, validStorage, bte::core::ErrorCode::invalidArgument);
  auto invalidCapital = validConfiguration;
  invalidCapital.initialCapital = 0;
  expectError(invalidCapital, validStorage,
              bte::core::ErrorCode::invalidArgument);
  auto missingSnapshot = validStorage;
  missingSnapshot.snapshotId = std::string(64, 'd');
  expectError(validConfiguration, missingSnapshot,
              bte::core::ErrorCode::dataSnapshotUnavailable);
  auto missingSymbol = validConfiguration;
  missingSymbol.symbol = "MISSING";
  expectError(missingSymbol, validStorage,
              bte::core::ErrorCode::dataUnavailable);
  auto emptySymbol = validConfiguration;
  emptySymbol.symbol.clear();
  expectError(emptySymbol, validStorage, bte::core::ErrorCode::dataUnavailable);
  auto emptyRange = validConfiguration;
  emptyRange.startDate = QDate{2030, 1, 1};
  emptyRange.endDate = QDate{2030, 1, 2};
  expectError(emptyRange, validStorage, bte::core::ErrorCode::dataUnavailable);

  const auto blockedStore = root_ / "BlockedBoundaryStore";
  std::ofstream blocked{blockedStore};
  blocked << "not a directory";
  blocked.close();
  auto storageFault = validStorage;
  storageFault.resultStore = blockedStore;
  expectError(validConfiguration, storageFault,
              bte::core::ErrorCode::permissionDenied);

  bte::results::testing::failNext(
      bte::results::testing::FailurePoint::hashFinalization);
  expectError(validConfiguration, validStorage, bte::core::ErrorCode::internal);
  bte::results::testing::clearFailure();

  auto strategyFailure = validConfiguration;
  strategyFailure.selectableStrategy = bte::strategy::SelectableStrategyPlan{};
  expectError(strategyFailure, validStorage,
              bte::core::ErrorCode::strategyCompileFailed);
}

TEST_F(ResultReplayTest,
       emptyAndTerminalTimelinesHaveSafeNavigationAndBoundedHistory) {
  auto empty = persist({}, bte::results::RunStatus::completed);
  ASSERT_TRUE(empty.ok()) << empty.error().message;
  auto replay = bte::bindings::ResultReplay::open(
      root_ / "CustomStore", root_ / "Data", empty.value().resultId,
      bte::bindings::ResultReplayTimeframe::hourly);
  ASSERT_TRUE(replay.ok()) << replay.error().message;
  EXPECT_EQ(replay.value()->resultId(), empty.value().resultId);
  EXPECT_EQ(replay.value()->current(), nullptr);
  EXPECT_EQ(replay.value()->currentIndex(), 0);
  EXPECT_EQ(replay.value()->totalFrames(), 0);
  EXPECT_EQ(replay.value()->progressPercent(), 0);
  EXPECT_TRUE(replay.value()->visibleWindow().empty());
  EXPECT_FALSE(replay.value()->stepForward());
  EXPECT_FALSE(replay.value()->stepBack());
  EXPECT_FALSE(replay.value()->seek(0));

  const auto terminalTimestamp = timestamp("2024-01-01 23:00:00+00:00");
  auto terminal =
      persist({{.sequence = 0,
                .timestamp = terminalTimestamp,
                .symbol = "SYN",
                .family = bte::results::RecordFamily::portfolio,
                .cashMicrodollars = 2'000'000'000,
                .marketValueMicrodollars = 0,
                .equityMicrodollars = 2'000'000'000,
                .pnlMicrodollars = 0,
                .positionShares = 0},
               {.sequence = 1,
                .timestamp = timestamp("2024-01-02 00:00:00+00:00"),
                .symbol = "SYN",
                .family = bte::results::RecordFamily::terminalDiagnostic,
                .text = "intentional strategy stop"}},
              bte::results::RunStatus::failed, "intentional strategy stop");
  ASSERT_TRUE(terminal.ok()) << terminal.error().message;
  auto failedReplay = bte::bindings::ResultReplay::open(
      root_ / "CustomStore", root_ / "Data", terminal.value().resultId,
      bte::bindings::ResultReplayTimeframe::hourly);
  ASSERT_TRUE(failedReplay.ok()) << failedReplay.error().message;
  EXPECT_EQ(failedReplay.value()->totalFrames(), 1);
  EXPECT_EQ(failedReplay.value()->terminalReason(),
            "intentional strategy stop");
  EXPECT_FALSE(failedReplay.value()->stepBack());

  auto failedWithoutCheckpoint = persist(
      {{.sequence = 0,
        .timestamp = terminalTimestamp,
        .symbol = "SYN",
        .family = bte::results::RecordFamily::terminalDiagnostic,
        .text = "failed before the first market slice"}},
      bte::results::RunStatus::failed, "failed before the first market slice");
  ASSERT_TRUE(failedWithoutCheckpoint.ok())
      << failedWithoutCheckpoint.error().message;
  auto emptyFailedReplay = bte::bindings::ResultReplay::open(
      root_ / "CustomStore", root_ / "Data",
      failedWithoutCheckpoint.value().resultId,
      bte::bindings::ResultReplayTimeframe::hourly);
  ASSERT_TRUE(emptyFailedReplay.ok()) << emptyFailedReplay.error().message;
  EXPECT_EQ(emptyFailedReplay.value()->totalFrames(), 0U);
  EXPECT_EQ(emptyFailedReplay.value()->current(), nullptr);
  EXPECT_EQ(emptyFailedReplay.value()->terminalReason(),
            "failed before the first market slice");
}

TEST_F(ResultReplayTest, barsBeforeTheFirstPortfolioCheckpointAreNotPresented) {
  auto persisted =
      persist({{.sequence = 0,
                .timestamp = timestamp("2024-01-02 00:00:00+00:00"),
                .symbol = "SYN",
                .family = bte::results::RecordFamily::portfolio,
                .cashMicrodollars = 2'000'000'000,
                .marketValueMicrodollars = 0,
                .equityMicrodollars = 2'000'000'000,
                .pnlMicrodollars = 0,
                .positionShares = 0}},
              bte::results::RunStatus::completed);
  ASSERT_TRUE(persisted.ok()) << persisted.error().message;

  auto hourly = bte::bindings::ResultReplay::open(
      root_ / "CustomStore", root_ / "Data", persisted.value().resultId,
      bte::bindings::ResultReplayTimeframe::hourly);
  ASSERT_TRUE(hourly.ok()) << hourly.error().message;
  ASSERT_EQ(hourly.value()->totalFrames(), 2);
  EXPECT_EQ(hourly.value()->current()->candle.ts,
            timestamp("2024-01-02 00:00:00+00:00"));

  auto daily = bte::bindings::ResultReplay::open(
      root_ / "CustomStore", root_ / "Data", persisted.value().resultId,
      bte::bindings::ResultReplayTimeframe::dailyUtc);
  ASSERT_TRUE(daily.ok()) << daily.error().message;
  ASSERT_EQ(daily.value()->totalFrames(), 1);
  EXPECT_EQ(daily.value()->current()->candle.ts,
            timestamp("2024-01-02 00:00:00+00:00"));
}

TEST_F(ResultReplayTest,
       sameDayDailyBucketExcludesBarsBeforeFirstTrustedCheckpoint) {
  for (const auto status :
       {bte::results::RunStatus::completed, bte::results::RunStatus::failed}) {
    SCOPED_TRACE(static_cast<int>(status));
    const auto trustedTimestamp = timestamp("2024-01-02 01:00:00+00:00");
    auto persisted = persist(
        {{.sequence = 0,
          .timestamp = trustedTimestamp,
          .symbol = "SYN",
          .family = bte::results::RecordFamily::portfolio,
          .cashMicrodollars = 2'000'000'000,
          .marketValueMicrodollars = 0,
          .equityMicrodollars = 2'000'000'000,
          .pnlMicrodollars = 0,
          .positionShares = 0}},
        status,
        status == bte::results::RunStatus::failed ? "trusted prefix" : "");
    ASSERT_TRUE(persisted.ok()) << persisted.error().message;

    auto hourly = bte::bindings::ResultReplay::open(
        root_ / "CustomStore", root_ / "Data", persisted.value().resultId,
        bte::bindings::ResultReplayTimeframe::hourly);
    ASSERT_TRUE(hourly.ok()) << hourly.error().message;
    ASSERT_EQ(hourly.value()->totalFrames(), 1U);
    ASSERT_NE(hourly.value()->current(), nullptr);
    EXPECT_EQ(hourly.value()->current()->candle.ts, trustedTimestamp);
    EXPECT_DOUBLE_EQ(hourly.value()->current()->candle.open, 103);
    EXPECT_DOUBLE_EQ(hourly.value()->current()->candle.volume, 3400);

    auto daily = bte::bindings::ResultReplay::open(
        root_ / "CustomStore", root_ / "Data", persisted.value().resultId,
        bte::bindings::ResultReplayTimeframe::dailyUtc);
    ASSERT_TRUE(daily.ok()) << daily.error().message;
    ASSERT_EQ(daily.value()->totalFrames(), 1U);
    ASSERT_NE(daily.value()->current(), nullptr);
    const auto frame = *daily.value()->current();
    EXPECT_EQ(frame.candle.ts, timestamp("2024-01-02 00:00:00+00:00"));
    EXPECT_DOUBLE_EQ(frame.candle.open, 103);
    EXPECT_DOUBLE_EQ(frame.candle.high, 105);
    EXPECT_DOUBLE_EQ(frame.candle.low, 102);
    EXPECT_DOUBLE_EQ(frame.candle.close, 104);
    EXPECT_DOUBLE_EQ(frame.candle.volume, 3400);
    EXPECT_TRUE(frame.partialUtcDay);
    EXPECT_TRUE(frame.fills.empty());
    EXPECT_EQ(frame.portfolio, hourly.value()->current()->portfolio);
    EXPECT_EQ(daily.value()->progressPercent(), 100);
    EXPECT_FALSE(daily.value()->stepForward());
    EXPECT_FALSE(daily.value()->stepBack());
    EXPECT_FALSE(daily.value()->seek(1));
    EXPECT_EQ(*daily.value()->current(), frame);
    ASSERT_TRUE(daily.value()->seek(0));
    EXPECT_EQ(daily.value()->visibleWindow(),
              std::vector<bte::bindings::ResultReplayFrame>{frame});

    auto repeated = bte::bindings::ResultReplay::open(
        root_ / "CustomStore", root_ / "Data", persisted.value().resultId,
        bte::bindings::ResultReplayTimeframe::dailyUtc);
    ASSERT_TRUE(repeated.ok()) << repeated.error().message;
    EXPECT_EQ(repeated.value()->visibleWindow(),
              daily.value()->visibleWindow());
  }
}

TEST_F(ResultReplayTest,
       dailyFillsExcludeRecordsBeforeTrustedSelectedBarWindow) {
  const auto trustedTimestamp = timestamp("2024-01-02 01:00:00+00:00");
  auto persisted =
      persist({{.sequence = 0,
                .timestamp = timestamp("2024-01-02 00:00:00+00:00"),
                .symbol = "SYN",
                .family = bte::results::RecordFamily::fill,
                .side = bte::results::OrderSide::buy,
                .quantityShares = 10,
                .priceNanodollars = 101'000'000'000,
                .amountMicrodollars = 1'010'000'000},
               {.sequence = 1,
                .timestamp = trustedTimestamp,
                .symbol = "SYN",
                .family = bte::results::RecordFamily::fill,
                .side = bte::results::OrderSide::sell,
                .quantityShares = 10,
                .priceNanodollars = 104'000'000'000,
                .amountMicrodollars = 1'040'000'000},
               {.sequence = 2,
                .timestamp = trustedTimestamp,
                .symbol = "SYN",
                .family = bte::results::RecordFamily::portfolio,
                .cashMicrodollars = 2'030'000'000,
                .marketValueMicrodollars = 0,
                .equityMicrodollars = 2'030'000'000,
                .pnlMicrodollars = 30'000'000,
                .positionShares = 0}},
              bte::results::RunStatus::failed, "trusted prefix");
  ASSERT_TRUE(persisted.ok()) << persisted.error().message;
  const auto expected = std::vector<bte::bindings::ResultReplayFill>{
      {.timestamp = trustedTimestamp,
       .isBuy = false,
       .quantityShares = 10,
       .price = 104,
       .amount = 1040,
       .barIndex = 2}};
  for (const auto timeframe :
       {bte::bindings::ResultReplayTimeframe::hourly,
        bte::bindings::ResultReplayTimeframe::dailyUtc}) {
    SCOPED_TRACE(static_cast<int>(timeframe));
    auto replay = bte::bindings::ResultReplay::open(
        root_ / "CustomStore", root_ / "Data", persisted.value().resultId,
        timeframe);
    ASSERT_TRUE(replay.ok()) << replay.error().message;
    ASSERT_EQ(replay.value()->totalFrames(), 1U);
    ASSERT_NE(replay.value()->current(), nullptr);
    EXPECT_EQ(replay.value()->current()->fills, expected);
    EXPECT_DOUBLE_EQ(replay.value()->current()->portfolio.cash, 2030);
    EXPECT_DOUBLE_EQ(replay.value()->current()->portfolio.pnl, 30);
    EXPECT_DOUBLE_EQ(replay.value()->current()->portfolio.realizedPnl, 30);
    EXPECT_DOUBLE_EQ(replay.value()->current()->portfolio.unrealizedPnl, 0);
    EXPECT_EQ(replay.value()->current()->barIndex, 2U);
    EXPECT_EQ(replay.value()->current()->fills.front().barIndex, 2U);
  }
}

TEST_F(ResultReplayTest, replayRejectsSellWithoutAnOpenLongPosition) {
  const auto firstTimestamp = timestamp("2024-01-01 23:00:00+00:00");
  auto persisted = persist({{.sequence = 0,
                             .timestamp = firstTimestamp,
                             .symbol = "SYN",
                             .family = bte::results::RecordFamily::portfolio,
                             .cashMicrodollars = 2'000'000'000,
                             .marketValueMicrodollars = 0,
                             .equityMicrodollars = 2'000'000'000,
                             .pnlMicrodollars = 0,
                             .positionShares = 0},
                            {.sequence = 1,
                             .timestamp = firstTimestamp,
                             .symbol = "SYN",
                             .family = bte::results::RecordFamily::fill,
                             .side = bte::results::OrderSide::sell,
                             .quantityShares = 1,
                             .priceNanodollars = 100'000'000'000,
                             .amountMicrodollars = 100'000'000}},
                           bte::results::RunStatus::completed);
  ASSERT_TRUE(persisted.ok()) << persisted.error().message;

  auto replay = bte::bindings::ResultReplay::open(
      root_ / "CustomStore", root_ / "Data", persisted.value().resultId,
      bte::bindings::ResultReplayTimeframe::hourly);
  ASSERT_FALSE(replay.ok());
  EXPECT_EQ(replay.error().code, bte::core::ErrorCode::schemaMismatch);
}

TEST_F(ResultReplayTest, partialLongCloseSplitsPnlByAverageCost) {
  const auto firstTimestamp = timestamp("2024-01-01 23:00:00+00:00");
  const auto secondTimestamp = timestamp("2024-01-02 00:00:00+00:00");
  const auto thirdTimestamp = timestamp("2024-01-02 01:00:00+00:00");
  auto store =
      bte::results::ResultStore::open(root_ / "CustomStore", root_ / "Data");
  ASSERT_TRUE(store.ok()) << store.error().message;
  auto writer = store.value()->begin({
      .universe = {"SYN"},
      .range = {.start = firstTimestamp,
                .end = timestamp("2024-01-02 02:00:00+00:00")},
      .initialCapitalMicrodollars = 2'000'000'000,
      .strategyId = "average-cost-partial-close",
      .strategyHash = std::string(64, 'c'),
      .dataSelection = selection_,
  });
  ASSERT_TRUE(writer.ok()) << writer.error().message;
  const std::vector<bte::results::CanonicalRecord> records{
      {.sequence = 0,
       .timestamp = firstTimestamp,
       .symbol = "SYN",
       .family = bte::results::RecordFamily::fill,
       .side = bte::results::OrderSide::buy,
       .quantityShares = 1,
       .priceNanodollars = 100'000'000'000,
       .amountMicrodollars = 100'000'000},
      {.sequence = 1,
       .timestamp = firstTimestamp,
       .symbol = "SYN",
       .family = bte::results::RecordFamily::portfolio,
       .cashMicrodollars = 1'900'000'000,
       .marketValueMicrodollars = 101'000'000,
       .equityMicrodollars = 2'001'000'000,
       .pnlMicrodollars = 1'000'000,
       .positionShares = 1},
      {.sequence = 2,
       .timestamp = secondTimestamp,
       .symbol = "SYN",
       .family = bte::results::RecordFamily::fill,
       .side = bte::results::OrderSide::buy,
       .quantityShares = 2,
       .priceNanodollars = 110'000'000'000,
       .amountMicrodollars = 220'000'000},
      {.sequence = 3,
       .timestamp = secondTimestamp,
       .symbol = "SYN",
       .family = bte::results::RecordFamily::portfolio,
       .cashMicrodollars = 1'680'000'000,
       .marketValueMicrodollars = 309'000'000,
       .equityMicrodollars = 1'989'000'000,
       .pnlMicrodollars = -11'000'000,
       .positionShares = 3},
      {.sequence = 4,
       .timestamp = thirdTimestamp,
       .symbol = "SYN",
       .family = bte::results::RecordFamily::fill,
       .side = bte::results::OrderSide::sell,
       .quantityShares = 1,
       .priceNanodollars = 120'000'000'000,
       .amountMicrodollars = 120'000'000},
      {.sequence = 5,
       .timestamp = thirdTimestamp,
       .symbol = "SYN",
       .family = bte::results::RecordFamily::portfolio,
       .cashMicrodollars = 1'800'000'000,
       .marketValueMicrodollars = 208'000'000,
       .equityMicrodollars = 2'008'000'000,
       .pnlMicrodollars = 8'000'000,
       .positionShares = 2},
  };
  ASSERT_TRUE(writer.value()->append(records).ok());
  auto finalized = writer.value()->finalizeAndPromote(
      bte::results::RunStatus::completed,
      {.finalEquityMicrodollars = 2'008'000'000, .pnlMicrodollars = 8'000'000});
  ASSERT_TRUE(finalized.ok()) << finalized.error().message;

  auto replay = bte::bindings::ResultReplay::open(
      root_ / "CustomStore", root_ / "Data", finalized.value().resultId,
      bte::bindings::ResultReplayTimeframe::hourly);
  ASSERT_TRUE(replay.ok()) << replay.error().message;
  ASSERT_TRUE(replay.value()->seek(2));
  ASSERT_NE(replay.value()->current(), nullptr);
  EXPECT_DOUBLE_EQ(replay.value()->current()->portfolio.pnl, 8);
  EXPECT_DOUBLE_EQ(replay.value()->current()->portfolio.realizedPnl, 13.333334);
  EXPECT_DOUBLE_EQ(replay.value()->current()->portfolio.unrealizedPnl,
                   -5.333334);
  ASSERT_EQ(replay.value()->current()->fills.size(), 1U);
  EXPECT_EQ(replay.value()->current()->fills.front().barIndex, 2U);
}

TEST_F(ResultReplayTest,
       dailyFillsMatchExactSelectedTimestampsAcrossInteriorGap) {
  std::filesystem::create_directories(root_ / "GapSource");
  std::ofstream source{root_ / "GapSource" / "SYN.csv"};
  source << "symbol,ts,open,high,low,close,volume,schemaName\n"
            "SYN,2024-01-02 00:00:00+00:00,100,101,99,100,1,ohlcv-1h\n"
            "SYN,2024-01-02 01:00:00+00:00,999,1000,998,999,2,ohlcv-1h\n"
            "SYN,2024-01-02 02:00:00+00:00,110,111,109,110,3,ohlcv-1h\n";
  source.close();
  auto built = bte::data::buildReleaseSnapshot({
      .sourceDirectory = root_ / "GapSource",
      .storeDirectory = root_ / "Data",
      .symbols = {"SYN"},
      .rowsPerSegment = 1,
      .calendarHash = std::string(64, 'a'),
      .splitManifestHash = std::string(64, 'b'),
  });
  ASSERT_TRUE(built.ok()) << built.error().message;
  auto reader = bte::data::ReleaseSnapshotReader::open(
      root_ / "Data", built.value().snapshotId);
  ASSERT_TRUE(reader.ok()) << reader.error().message;
  const auto range =
      bte::core::DateRange{.start = timestamp("2024-01-02 00:00:00+00:00"),
                           .end = timestamp("2024-01-03 00:00:00+00:00")};
  auto selected = reader.value()->select(
      {.symbols = {"SYN"}, .range = range, .timeframe = "ohlcv-1h"});
  ASSERT_TRUE(selected.ok()) << selected.error().message;
  ASSERT_EQ(selected.value().identity.spans.size(), 3U);
  auto store =
      bte::results::ResultStore::open(root_ / "GapStore", root_ / "Data");
  ASSERT_TRUE(store.ok()) << store.error().message;
  for (const bool sparse : {false, true}) {
    SCOPED_TRACE(sparse);
    auto identity = selected.value().identity;
    if (sparse) {
      identity.spans.erase(identity.spans.begin() + 1);
    }
    auto writer = store.value()->begin({
        .universe = {"SYN"},
        .range = range,
        .initialCapitalMicrodollars = 2'000'000'000,
        .strategyId = "interior-gap-fixture",
        .strategyHash = std::string(64, 'c'),
        .dataSelection = identity,
    });
    ASSERT_TRUE(writer.ok()) << writer.error().message;
    const std::vector<bte::results::CanonicalRecord> records{
        {.sequence = 0,
         .timestamp = range.start,
         .symbol = "SYN",
         .family = bte::results::RecordFamily::portfolio,
         .cashMicrodollars = 2'000'000'000,
         .marketValueMicrodollars = 0,
         .equityMicrodollars = 2'000'000'000,
         .pnlMicrodollars = 0,
         .positionShares = 0},
        {.sequence = 1,
         .timestamp = range.start,
         .symbol = "SYN",
         .family = bte::results::RecordFamily::fill,
         .side = bte::results::OrderSide::buy,
         .quantityShares = 1,
         .priceNanodollars = 100'000'000'000,
         .amountMicrodollars = 100'000'000},
        {.sequence = 2,
         .timestamp = range.start + std::chrono::hours{1},
         .symbol = "SYN",
         .family = bte::results::RecordFamily::fill,
         .side = bte::results::OrderSide::sell,
         .quantityShares = 1,
         .priceNanodollars = 999'000'000'000,
         .amountMicrodollars = 999'000'000},
        {.sequence = 3,
         .timestamp = range.start + std::chrono::hours{2},
         .symbol = "SYN",
         .family = bte::results::RecordFamily::fill,
         .side = bte::results::OrderSide::buy,
         .quantityShares = 2,
         .priceNanodollars = 110'000'000'000,
         .amountMicrodollars = 220'000'000},
        {.sequence = 4,
         .timestamp = range.start + std::chrono::hours{2},
         .symbol = "SYN",
         .family = bte::results::RecordFamily::fill,
         .side = bte::results::OrderSide::sell,
         .quantityShares = 2,
         .priceNanodollars = 110'000'000'000,
         .amountMicrodollars = 220'000'000}};
    ASSERT_TRUE(writer.value()->append(records).ok());
    auto finalized = writer.value()->finalizeAndPromote(
        bte::results::RunStatus::completed,
        {.finalEquityMicrodollars = 2'000'000'000, .pnlMicrodollars = 0});
    ASSERT_TRUE(finalized.ok()) << finalized.error().message;
    auto daily = bte::bindings::ResultReplay::open(
        root_ / "GapStore", root_ / "Data", finalized.value().resultId,
        bte::bindings::ResultReplayTimeframe::dailyUtc);
    ASSERT_TRUE(daily.ok()) << daily.error().message;
    ASSERT_EQ(daily.value()->totalFrames(), 1U);
    const auto &frame = *daily.value()->current();
    EXPECT_DOUBLE_EQ(frame.candle.open, 100);
    EXPECT_DOUBLE_EQ(frame.candle.high, sparse ? 111 : 1000);
    EXPECT_DOUBLE_EQ(frame.candle.low, 99);
    EXPECT_DOUBLE_EQ(frame.candle.close, 110);
    EXPECT_DOUBLE_EQ(frame.candle.volume, sparse ? 4 : 6);
    EXPECT_EQ(frame.partialUtcDay, sparse);
    const std::vector<bte::bindings::ResultReplayFill> expected{
        {.timestamp = range.start,
         .isBuy = true,
         .quantityShares = 1,
         .price = 100,
         .amount = 100,
         .barIndex = 0},
        {.timestamp = range.start + std::chrono::hours{2},
         .isBuy = true,
         .quantityShares = 2,
         .price = 110,
         .amount = 220,
         .barIndex = sparse ? 1U : 2U},
        {.timestamp = range.start + std::chrono::hours{2},
         .isBuy = false,
         .quantityShares = 2,
         .price = 110,
         .amount = 220,
         .barIndex = sparse ? 1U : 2U}};
    if (sparse) {
      EXPECT_EQ(frame.fills, expected);
    } else {
      ASSERT_EQ(frame.fills.size(), 4U);
      EXPECT_DOUBLE_EQ(frame.fills[1].price, 999);
    }
    auto hourly = bte::bindings::ResultReplay::open(
        root_ / "GapStore", root_ / "Data", finalized.value().resultId,
        bte::bindings::ResultReplayTimeframe::hourly);
    ASSERT_TRUE(hourly.ok()) << hourly.error().message;
    std::vector<bte::bindings::ResultReplayFill> hourlyFills;
    for (const auto &hour : hourly.value()->timeline()) {
      hourlyFills.insert(hourlyFills.end(), hour.fills.begin(),
                         hour.fills.end());
    }
    EXPECT_EQ(frame.fills, hourlyFills);
  }
}

TEST_F(ResultReplayTest, diagnosticReplayExcludesFillAfterLastCheckpoint) {
  const auto trustedTimestamp = timestamp("2024-01-02 00:00:00+00:00");
  auto persisted =
      persist({{.sequence = 0,
                .timestamp = trustedTimestamp,
                .symbol = "SYN",
                .family = bte::results::RecordFamily::portfolio,
                .cashMicrodollars = 2'000'000'000,
                .marketValueMicrodollars = 0,
                .equityMicrodollars = 2'000'000'000,
                .pnlMicrodollars = 0,
                .positionShares = 0},
               {.sequence = 1,
                .timestamp = timestamp("2024-01-02 01:00:00+00:00"),
                .symbol = "SYN",
                .family = bte::results::RecordFamily::fill,
                .side = bte::results::OrderSide::buy,
                .quantityShares = 10,
                .priceNanodollars = 103'000'000'000,
                .amountMicrodollars = 1'030'000'000}},
              bte::results::RunStatus::failed, "incomplete later slice");
  ASSERT_TRUE(persisted.ok()) << persisted.error().message;
  for (const auto timeframe :
       {bte::bindings::ResultReplayTimeframe::hourly,
        bte::bindings::ResultReplayTimeframe::dailyUtc}) {
    SCOPED_TRACE(static_cast<int>(timeframe));
    auto replay = bte::bindings::ResultReplay::open(
        root_ / "CustomStore", root_ / "Data", persisted.value().resultId,
        timeframe);
    ASSERT_TRUE(replay.ok()) << replay.error().message;
    ASSERT_EQ(replay.value()->totalFrames(), 1U);
    ASSERT_NE(replay.value()->current(), nullptr);
    EXPECT_EQ(replay.value()->current()->candle.ts, trustedTimestamp);
    EXPECT_DOUBLE_EQ(replay.value()->current()->candle.open, 101);
    EXPECT_DOUBLE_EQ(replay.value()->current()->candle.high, 104);
    EXPECT_DOUBLE_EQ(replay.value()->current()->candle.low, 100);
    EXPECT_DOUBLE_EQ(replay.value()->current()->candle.close, 103);
    EXPECT_DOUBLE_EQ(replay.value()->current()->candle.volume, 2300);
    EXPECT_TRUE(replay.value()->current()->fills.empty());
    EXPECT_DOUBLE_EQ(replay.value()->current()->portfolio.cash, 2000);
    EXPECT_EQ(replay.value()->terminalReason(), "incomplete later slice");
    EXPECT_FALSE(replay.value()->stepForward());
  }
}

TEST_F(ResultReplayTest, catalogBoundariesPropagateCancellationAndStoreFaults) {
  auto listed =
      bte::bindings::ResultReplay::list(root_ / "Store", root_ / "Data");
  ASSERT_TRUE(listed.ok()) << listed.error().message;
  ASSERT_EQ(listed.value().size(), 1);
  EXPECT_EQ(listed.value().front().resultId, resultId_);

  bte::core::CancellationSource cancellation;
  cancellation.requestCancellation();
  auto cancelled = bte::bindings::ResultReplay::list(
      root_ / "Store", root_ / "Data", cancellation.token());
  ASSERT_FALSE(cancelled.ok());
  EXPECT_EQ(cancelled.error().code, bte::core::ErrorCode::cancelled);

  std::ofstream blocked{root_ / "BlockedStore"};
  blocked << "not a directory";
  blocked.close();
  auto unavailable =
      bte::bindings::ResultReplay::list(root_ / "BlockedStore", root_ / "Data");
  ASSERT_FALSE(unavailable.ok());
  EXPECT_EQ(unavailable.error().code, bte::core::ErrorCode::permissionDenied);

  auto unavailableOpen = bte::bindings::ResultReplay::open(
      root_ / "BlockedStore", root_ / "Data", resultId_,
      bte::bindings::ResultReplayTimeframe::hourly);
  ASSERT_FALSE(unavailableOpen.ok());
  EXPECT_EQ(unavailableOpen.error().code,
            bte::core::ErrorCode::permissionDenied);

  auto missing = bte::bindings::ResultReplay::open(
      root_ / "Store", root_ / "Data", std::string(32, 'd'),
      bte::bindings::ResultReplayTimeframe::hourly);
  ASSERT_FALSE(missing.ok());
  EXPECT_EQ(missing.error().code, bte::core::ErrorCode::notFound);
}

TEST_F(ResultReplayTest,
       ownedRequestsPublishOnlyNewestGenerationAndStopAfterDestruction) {
  int argumentCount = 1;
  std::string executable = "bte_result_replay_tests";
  std::array arguments{executable.data(), static_cast<char *>(nullptr)};
  std::unique_ptr<QCoreApplication> application;
  if (QCoreApplication::instance() == nullptr) {
    application =
        std::make_unique<QCoreApplication>(argumentCount, arguments.data());
  }

  auto requests = std::make_unique<bte::bindings::ResultReplayRequests>(
      root_ / "Store", root_ / "Data");
  QEventLoop loop;
  auto catalogCallbacks = 0;
  requests->requestCatalog([&](auto catalog) {
    ++catalogCallbacks;
    EXPECT_TRUE(catalog.ok()) << catalog.error().message;
    if (catalog.ok()) {
      EXPECT_EQ(catalog.value().entries.size(), 1U);
      if (!catalog.value().entries.empty()) {
        EXPECT_EQ(catalog.value().entries.front().resultId, resultId_);
      }
    }
    loop.quit();
  });
  QTimer::singleShot(5'000, &loop, &QEventLoop::quit);
  loop.exec();
  ASSERT_EQ(catalogCallbacks, 1);

  auto staleCallbacks = 0;
  auto newestCallbacks = 0;
  requests->requestOpen(std::string(32, 'd'),
                        bte::bindings::ResultReplayTimeframe::hourly,
                        [&](auto) { ++staleCallbacks; });
  requests->requestOpen(resultId_, bte::bindings::ResultReplayTimeframe::hourly,
                        [&](auto opened) {
                          ++newestCallbacks;
                          EXPECT_TRUE(opened.ok()) << opened.error().message;
                          loop.quit();
                        });
  QTimer::singleShot(5'000, &loop, &QEventLoop::quit);
  loop.exec();
  EXPECT_EQ(staleCallbacks, 0);
  EXPECT_EQ(newestCallbacks, 1);

  auto callbackAfterDestruction = false;
  bte::bindings::testing::blockNextResultOpenUntilCancelled();
  requests->requestOpen(resultId_, bte::bindings::ResultReplayTimeframe::hourly,
                        [&](auto) { callbackAfterDestruction = true; });
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds{5};
  while (!bte::bindings::testing::resultOpenWorkerStarted() &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::yield();
  }
  ASSERT_TRUE(bte::bindings::testing::resultOpenWorkerStarted());
  requests.reset();
  QCoreApplication::processEvents();
  EXPECT_FALSE(callbackAfterDestruction);
}

TEST_F(ResultReplayTest, dailyAggregationRejectsFixedPointVolumeOverflow) {
  const auto sourceDirectory = root_ / "OverflowSource";
  const auto dataDirectory = root_ / "OverflowData";
  std::filesystem::create_directories(sourceDirectory);
  std::ofstream source{sourceDirectory / "SYN.csv"};
  source << "symbol,ts,open,high,low,close,volume,schemaName\n"
            "SYN,2024-01-01 00:00:00+00:00,100,102,99,101,"
            "5000000000000,ohlcv-1h\n"
            "SYN,2024-01-01 01:00:00+00:00,101,103,100,102,"
            "5000000000000,ohlcv-1h\n";
  source.close();
  auto built = bte::data::buildReleaseSnapshot({
      .sourceDirectory = sourceDirectory,
      .storeDirectory = dataDirectory,
      .symbols = {"SYN"},
      .rowsPerSegment = 2,
      .calendarHash = std::string(64, 'a'),
      .splitManifestHash = std::string(64, 'b'),
  });
  ASSERT_TRUE(built.ok()) << built.error().message;
  auto reader = bte::data::ReleaseSnapshotReader::open(
      dataDirectory, built.value().snapshotId);
  ASSERT_TRUE(reader.ok()) << reader.error().message;
  const auto range =
      bte::core::DateRange{.start = timestamp("2024-01-01 00:00:00+00:00"),
                           .end = timestamp("2024-01-01 02:00:00+00:00")};
  auto selected = reader.value()->select(
      {.symbols = {"SYN"}, .range = range, .timeframe = "ohlcv-1h"});
  ASSERT_TRUE(selected.ok()) << selected.error().message;
  auto store =
      bte::results::ResultStore::open(root_ / "OverflowStore", dataDirectory);
  ASSERT_TRUE(store.ok()) << store.error().message;
  auto writer = store.value()->begin({
      .universe = {"SYN"},
      .range = range,
      .initialCapitalMicrodollars = 2'000'000'000,
      .strategyId = "volume-overflow-fixture",
      .strategyHash = std::string(64, 'c'),
      .dataSelection = selected.value().identity,
  });
  ASSERT_TRUE(writer.ok()) << writer.error().message;
  ASSERT_TRUE(writer.value()
                  ->append({{.sequence = 0,
                             .timestamp = range.start,
                             .symbol = "SYN",
                             .family = bte::results::RecordFamily::portfolio,
                             .cashMicrodollars = 2'000'000'000,
                             .marketValueMicrodollars = 0,
                             .equityMicrodollars = 2'000'000'000,
                             .pnlMicrodollars = 0,
                             .positionShares = 0}})
                  .ok());
  auto finalized = writer.value()->finalizeAndPromote(
      bte::results::RunStatus::completed,
      {.finalEquityMicrodollars = 2'000'000'000, .pnlMicrodollars = 0});
  ASSERT_TRUE(finalized.ok()) << finalized.error().message;

  auto replay = bte::bindings::ResultReplay::open(
      root_ / "OverflowStore", dataDirectory, finalized.value().resultId,
      bte::bindings::ResultReplayTimeframe::dailyUtc);
  ASSERT_FALSE(replay.ok());
  EXPECT_EQ(replay.error().code, bte::core::ErrorCode::invalidArgument);
}

TEST_F(ResultReplayTest,
       tenThousandHourlyFramesUseIndexedSeekAndBoundedVisibleWindow) {
  constexpr auto barCount = std::size_t{10'000};
  const auto sourceDirectory = root_ / "LargeSource";
  const auto dataDirectory = root_ / "LargeData";
  std::filesystem::create_directories(sourceDirectory);
  std::ofstream source{sourceDirectory / "SYN.csv"};
  source << "symbol,ts,open,high,low,close,volume,schemaName\n";
  const auto firstTimestamp = timestamp("2024-01-01 00:00:00+00:00");
  for (std::size_t index = 0; index < barCount; ++index) {
    source << "SYN,"
           << bte::core::time::toIso8601(firstTimestamp +
                                         std::chrono::hours{index})
           << ",100,102,99,101," << 1'000 + index << ",ohlcv-1h\n";
  }
  source.close();
  auto built = bte::data::buildReleaseSnapshot({
      .sourceDirectory = sourceDirectory,
      .storeDirectory = dataDirectory,
      .symbols = {"SYN"},
      .rowsPerSegment = 1'000,
      .calendarHash = std::string(64, 'a'),
      .splitManifestHash = std::string(64, 'b'),
  });
  ASSERT_TRUE(built.ok()) << built.error().message;
  auto reader = bte::data::ReleaseSnapshotReader::open(
      dataDirectory, built.value().snapshotId);
  ASSERT_TRUE(reader.ok()) << reader.error().message;
  const auto range = bte::core::DateRange{.start = firstTimestamp,
                                          .end = firstTimestamp +
                                                 std::chrono::hours{barCount}};
  auto selected = reader.value()->select(
      {.symbols = {"SYN"}, .range = range, .timeframe = "ohlcv-1h"});
  ASSERT_TRUE(selected.ok()) << selected.error().message;
  auto store =
      bte::results::ResultStore::open(root_ / "LargeStore", dataDirectory);
  ASSERT_TRUE(store.ok()) << store.error().message;
  auto writer = store.value()->begin({
      .universe = {"SYN"},
      .range = range,
      .initialCapitalMicrodollars = 100'000'000'000,
      .strategyId = "performance-fixture",
      .strategyHash = std::string(64, 'c'),
      .dataSelection = selected.value().identity,
  });
  ASSERT_TRUE(writer.ok()) << writer.error().message;
  std::vector<bte::results::CanonicalRecord> records;
  records.reserve(barCount);
  for (std::size_t index = 0; index < barCount; ++index) {
    records.push_back({
        .sequence = index,
        .timestamp = firstTimestamp + std::chrono::hours{index},
        .symbol = "SYN",
        .family = bte::results::RecordFamily::portfolio,
        .cashMicrodollars = 100'000'000'000,
        .marketValueMicrodollars = 0,
        .equityMicrodollars = 100'000'000'000,
        .pnlMicrodollars = 0,
        .positionShares = 0,
    });
  }
  ASSERT_TRUE(writer.value()->append(records).ok());
  auto finalized = writer.value()->finalizeAndPromote(
      bte::results::RunStatus::completed,
      {.finalEquityMicrodollars = 100'000'000'000, .pnlMicrodollars = 0});
  ASSERT_TRUE(finalized.ok()) << finalized.error().message;

  const auto openedAt = std::chrono::steady_clock::now();
  auto replay = bte::bindings::ResultReplay::open(
      root_ / "LargeStore", dataDirectory, finalized.value().resultId,
      bte::bindings::ResultReplayTimeframe::hourly);
  const auto openMilliseconds =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - openedAt)
          .count();

  ASSERT_TRUE(replay.ok()) << replay.error().message;
  RecordProperty("openMilliseconds", openMilliseconds);
  EXPECT_EQ(replay.value()->totalFrames(), barCount);
  const auto seekAt = std::chrono::steady_clock::now();
  EXPECT_TRUE(replay.value()->seek(barCount - 1));
  const auto seekMicroseconds =
      std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now() - seekAt)
          .count();
  RecordProperty("seekMicroseconds", seekMicroseconds);
  EXPECT_EQ(replay.value()->visibleWindow().size(), 500);
  EXPECT_EQ(replay.value()->current()->candle.ts,
            firstTimestamp + std::chrono::hours{barCount - 1});
  EXPECT_TRUE(replay.value()->stepBack());
  EXPECT_EQ(replay.value()->currentIndex(), barCount - 2);
  ASSERT_TRUE(replay.value()->seek(0));
  const auto playbackAt = std::chrono::steady_clock::now();
  auto steps = std::size_t{0};
  while (replay.value()->stepForward()) {
    ++steps;
  }
  const auto playbackMicroseconds =
      std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now() - playbackAt)
          .count();
  RecordProperty("maximumPlaybackMicroseconds", playbackMicroseconds);
  RecordProperty("peakResidentKibibytes", peakResidentKibibytes());
  EXPECT_EQ(steps, barCount - 1);
}

} // namespace
