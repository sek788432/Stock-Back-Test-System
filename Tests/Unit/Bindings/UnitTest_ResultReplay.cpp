#include "Bte/Bindings/BacktestSessionVm.h"
#include "Bte/Bindings/ResultReplay.h"

#include "Bte/Core/Cancellation.h"
#include "Bte/Core/Time.h"
#include "Bte/Data/ReleaseSnapshot.h"
#include "Bte/Results/ResultStore.h"

#include "ResultStoreTestHooks.h"

#include <gtest/gtest.h>

#include <QDate>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
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
    auto recorded = bte::bindings::runPersistedBacktestConfiguration(
        {.symbol = "SYN",
         .schema = "ohlcv-1h",
         .startDate = QDate{2024, 1, 1},
         .endDate = QDate{2024, 1, 2},
         .initialCapital = 2'000,
         .quantityShares = 10},
        {.resultStore = root_ / "Store",
         .dataStore = root_ / "Data",
         .snapshotId = snapshotId_,
         .strategyHash = std::string(64, 'c')});
    ASSERT_TRUE(recorded.ok()) << recorded.error().message;
    ASSERT_EQ(recorded.value().resultId.size(), 32);
    ASSERT_EQ(recorded.value().canonicalResultHash.size(), 64);
    resultId_ = recorded.value().resultId;
  }

  void TearDown() override {
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

TEST_F(ResultReplayTest, dailyUtcAggregationSumsVolumeAndLabelsPartialBuckets) {
  auto replay = bte::bindings::ResultReplay::open(
      root_ / "Store", root_ / "Data", resultId_,
      bte::bindings::ResultReplayTimeframe::dailyUtc);
  ASSERT_TRUE(replay.ok()) << replay.error().message;
  ASSERT_EQ(replay.value()->totalFrames(), 2);
  ASSERT_NE(replay.value()->current(), nullptr);
  EXPECT_TRUE(replay.value()->current()->partialUtcDay);
  EXPECT_DOUBLE_EQ(replay.value()->current()->candle.volume, 1200);
  ASSERT_TRUE(replay.value()->stepForward());
  EXPECT_TRUE(replay.value()->current()->partialUtcDay);
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
}

TEST_F(ResultReplayTest, persistedBacktestRejectsUnsupportedSourceSchema) {
  const auto unsupported = bte::bindings::runPersistedBacktestConfiguration(
      {.symbol = "SYN",
       .schema = "ohlcv-1d",
       .startDate = QDate{2024, 1, 1},
       .endDate = QDate{2024, 1, 2},
       .initialCapital = 2'000,
       .quantityShares = 10},
      {.resultStore = root_ / "Store",
       .dataStore = root_ / "Data",
       .snapshotId = snapshotId_,
       .strategyHash = std::string(64, 'c')});
  ASSERT_FALSE(unsupported.ok());
  EXPECT_EQ(unsupported.error().code, bte::core::ErrorCode::schemaMismatch);
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

  auto invalidIdentity = validStorage;
  invalidIdentity.strategyHash = std::string(64, 'Z');
  expectError(validConfiguration, invalidIdentity,
              bte::core::ErrorCode::invalidArgument);

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
                .positionShares = 0},
               {.sequence = 1,
                .timestamp = terminalTimestamp,
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

  auto missing = bte::bindings::ResultReplay::open(
      root_ / "Store", root_ / "Data", std::string(32, 'd'),
      bte::bindings::ResultReplayTimeframe::hourly);
  ASSERT_FALSE(missing.ok());
  EXPECT_EQ(missing.error().code, bte::core::ErrorCode::notFound);
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
