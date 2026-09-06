#include "Bte/Bindings/BacktestSessionVm.h"
#include "Bte/Bindings/ResultReplay.h"
#include "Bte/Core/Time.h"
#include "Bte/Data/ReleaseSnapshot.h"
#include "Bte/Results/ResultStore.h"

#include <gtest/gtest.h>

#include <QDate>

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace {

class ReplayFixture : public testing::Test {
protected:
  void SetUp() override {
    root_ =
        std::filesystem::temp_directory_path() /
        ("bte-issue10-" +
         std::string{
             testing::UnitTest::GetInstance()->current_test_info()->name()});
    std::filesystem::remove_all(root_);
    const auto built = bte::data::buildReleaseSnapshot({
        .sourceDirectory = std::filesystem::path{BTE_TEST_SOURCE_DIR} /
                           "Tests/Fixtures/Replay",
        .storeDirectory = root_ / "Data",
        .symbols = {"SYN"},
        .rowsPerSegment = 2,
        .calendarHash = std::string(64, 'a'),
        .splitManifestHash = std::string(64, 'b'),
    });
    ASSERT_TRUE(built.ok()) << built.error().message;
    snapshotId_ = built.value().snapshotId;

    auto reader =
        bte::data::ReleaseSnapshotReader::open(root_ / "Data", snapshotId_);
    ASSERT_TRUE(reader.ok()) << reader.error().message;
    auto selected = reader.value()->select({
        .symbols = {"SYN"},
        .range = {.start = timestamp("2024-01-01 23:00:00+00:00"),
                  .end = timestamp("2024-01-03 01:00:00+00:00")},
        .timeframe = "ohlcv-1h",
    });
    ASSERT_TRUE(selected.ok()) << selected.error().message;

    auto store =
        bte::results::ResultStore::open(root_ / "Results", root_ / "Data");
    ASSERT_TRUE(store.ok()) << store.error().message;
    auto writer = store.value()->begin({
        .universe = {"SYN"},
        .range = {.start = timestamp("2024-01-01 23:00:00+00:00"),
                  .end = timestamp("2024-01-03 01:00:00+00:00")},
        .initialCapitalMicrodollars = 2'000'000'000,
        .strategyId = "issue10-literal",
        .strategyHash = std::string(64, 'c'),
        .dataSelection = selected.value().identity,
    });
    ASSERT_TRUE(writer.ok()) << writer.error().message;
    const auto appended = writer.value()->append(literalTimeline());
    ASSERT_TRUE(appended.ok()) << appended.error().message;
    auto finalized = writer.value()->finalizeAndPromote(
        bte::results::RunStatus::completed,
        {.finalEquityMicrodollars = 2'020'000'000,
         .pnlMicrodollars = 20'000'000});
    ASSERT_TRUE(finalized.ok()) << finalized.error().message;
    resultId_ = finalized.value().resultId;
  }

  void TearDown() override { std::filesystem::remove_all(root_); }

  static bte::core::Timestamp timestamp(const std::string &text) {
    return bte::core::time::parseIso8601(text).value();
  }

  static std::vector<bte::results::CanonicalRecord> literalTimeline() {
    using bte::results::CanonicalRecord;
    using bte::results::OrderSide;
    using bte::results::RecordFamily;
    return {
        CanonicalRecord{.sequence = 0,
                        .timestamp = timestamp("2024-01-01 23:00:00+00:00"),
                        .symbol = "SYN",
                        .family = RecordFamily::portfolio,
                        .cashMicrodollars = 2'000'000'000,
                        .marketValueMicrodollars = 0,
                        .equityMicrodollars = 2'000'000'000,
                        .positionShares = 0},
        CanonicalRecord{.sequence = 1,
                        .timestamp = timestamp("2024-01-02 00:00:00+00:00"),
                        .symbol = "SYN",
                        .family = RecordFamily::fill,
                        .side = OrderSide::buy,
                        .quantityShares = 10,
                        .priceNanodollars = 103'000'000'000,
                        .amountMicrodollars = 1'030'000'000},
        CanonicalRecord{.sequence = 2,
                        .timestamp = timestamp("2024-01-02 00:00:00+00:00"),
                        .symbol = "SYN",
                        .family = RecordFamily::portfolio,
                        .cashMicrodollars = 970'000'000,
                        .marketValueMicrodollars = 1'030'000'000,
                        .equityMicrodollars = 2'000'000'000,
                        .positionShares = 10},
        CanonicalRecord{.sequence = 3,
                        .timestamp = timestamp("2024-01-02 01:00:00+00:00"),
                        .symbol = "SYN",
                        .family = RecordFamily::portfolio,
                        .cashMicrodollars = 970'000'000,
                        .marketValueMicrodollars = 1'040'000'000,
                        .equityMicrodollars = 2'010'000'000,
                        .positionShares = 10},
        CanonicalRecord{.sequence = 4,
                        .timestamp = timestamp("2024-01-02 02:00:00+00:00"),
                        .symbol = "SYN",
                        .family = RecordFamily::fill,
                        .side = OrderSide::sell,
                        .quantityShares = 10,
                        .priceNanodollars = 105'000'000'000,
                        .amountMicrodollars = 1'050'000'000},
        CanonicalRecord{.sequence = 5,
                        .timestamp = timestamp("2024-01-02 02:00:00+00:00"),
                        .symbol = "SYN",
                        .family = RecordFamily::portfolio,
                        .cashMicrodollars = 2'020'000'000,
                        .marketValueMicrodollars = 0,
                        .equityMicrodollars = 2'020'000'000,
                        .positionShares = 0},
        CanonicalRecord{.sequence = 6,
                        .timestamp = timestamp("2024-01-03 00:00:00+00:00"),
                        .symbol = "SYN",
                        .family = RecordFamily::portfolio,
                        .cashMicrodollars = 2'020'000'000,
                        .marketValueMicrodollars = 0,
                        .equityMicrodollars = 2'020'000'000,
                        .positionShares = 0},
    };
  }

  static std::vector<bte::bindings::ResultReplayFrame>
  snapshots(bte::bindings::ResultReplay &replay) {
    std::vector<bte::bindings::ResultReplayFrame> frames;
    if (replay.current() == nullptr) {
      return frames;
    }
    frames.push_back(*replay.current());
    while (replay.stepForward()) {
      frames.push_back(*replay.current());
    }
    return frames;
  }

  std::filesystem::path root_;
  std::string snapshotId_;
  std::string resultId_;
};

TEST_F(ReplayFixture, replaySnapshotsMatchExpectedSequence) {
  const auto catalog =
      bte::bindings::ResultReplay::list(root_ / "Results", root_ / "Data");
  ASSERT_TRUE(catalog.ok()) << catalog.error().message;
  ASSERT_EQ(catalog.value().size(), 1U);
  EXPECT_EQ(catalog.value().front().resultId, resultId_);
  EXPECT_TRUE(catalog.value().front().available);

  auto replay = bte::bindings::ResultReplay::open(
      root_ / "Results", root_ / "Data", resultId_,
      bte::bindings::ResultReplayTimeframe::hourly);
  ASSERT_TRUE(replay.ok()) << replay.error().message;
  const auto frames = snapshots(*replay.value());
  ASSERT_EQ(frames.size(), 5U);
  EXPECT_EQ(frames[0].candle.ts, timestamp("2024-01-01 23:00:00+00:00"));
  EXPECT_DOUBLE_EQ(frames[0].candle.volume, 1200.0);
  EXPECT_DOUBLE_EQ(frames[0].portfolio.equity, 2'000.0);
  ASSERT_EQ(frames[1].fills.size(), 1U);
  EXPECT_TRUE(frames[1].fills[0].isBuy);
  EXPECT_DOUBLE_EQ(frames[1].fills[0].price, 103.0);
  EXPECT_EQ(frames[1].portfolio.positionShares, 10);
  EXPECT_DOUBLE_EQ(frames[2].portfolio.pnl, 10.0);
  ASSERT_EQ(frames[3].fills.size(), 1U);
  EXPECT_FALSE(frames[3].fills[0].isBuy);
  EXPECT_DOUBLE_EQ(frames[3].portfolio.cash, 2'020.0);
  EXPECT_EQ(frames[4].portfolio.positionShares, 0);
  EXPECT_DOUBLE_EQ(frames[4].candle.volume, 5600.0);
  EXPECT_EQ(replay.value()->progressPercent(), 100);

  auto daily = bte::bindings::ResultReplay::open(
      root_ / "Results", root_ / "Data", resultId_,
      bte::bindings::ResultReplayTimeframe::dailyUtc);
  ASSERT_TRUE(daily.ok()) << daily.error().message;
  const auto days = snapshots(*daily.value());
  ASSERT_EQ(days.size(), 3U);
  EXPECT_TRUE(days[0].partialUtcDay);
  EXPECT_DOUBLE_EQ(days[0].candle.volume, 1200.0);
  EXPECT_TRUE(days[1].partialUtcDay);
  EXPECT_DOUBLE_EQ(days[1].candle.open, 101.0);
  EXPECT_DOUBLE_EQ(days[1].candle.high, 107.0);
  EXPECT_DOUBLE_EQ(days[1].candle.low, 100.0);
  EXPECT_DOUBLE_EQ(days[1].candle.close, 106.0);
  EXPECT_DOUBLE_EQ(days[1].candle.volume, 10'200.0);
  ASSERT_EQ(days[1].fills.size(), 2U);
  EXPECT_TRUE(days[1].fills[0].isBuy);
  EXPECT_FALSE(days[1].fills[1].isBuy);
  EXPECT_DOUBLE_EQ(days[1].portfolio.equity, 2'020.0);
  EXPECT_TRUE(days[2].partialUtcDay);
}

TEST_F(ReplayFixture, replayTwiceProducesSameSnapshots) {
  auto first = bte::bindings::ResultReplay::open(
      root_ / "Results", root_ / "Data", resultId_,
      bte::bindings::ResultReplayTimeframe::hourly);
  auto second = bte::bindings::ResultReplay::open(
      root_ / "Results", root_ / "Data", resultId_,
      bte::bindings::ResultReplayTimeframe::hourly);
  ASSERT_TRUE(first.ok()) << first.error().message;
  ASSERT_TRUE(second.ok()) << second.error().message;

  EXPECT_EQ(snapshots(*first.value()), snapshots(*second.value()));
}

TEST_F(ReplayFixture, identicalBacktestsHaveDistinctIdsAndEqualHashes) {
  const auto configuration = bte::bindings::BacktestConfiguration{
      .symbol = "SYN",
      .schema = "ohlcv-1h",
      .startDate = QDate{2024, 1, 1},
      .endDate = QDate{2024, 1, 3},
      .initialCapital = 2'000.0,
      .quantityShares = 10,
  };
  const auto storage = bte::bindings::PersistedBacktestStorage{
      .resultStore = root_ / "Results",
      .dataStore = root_ / "Data",
      .snapshotId = snapshotId_,
      .strategyHash = std::string(64, 'd'),
  };
  const auto first =
      bte::bindings::runPersistedBacktestConfiguration(configuration, storage);
  const auto second =
      bte::bindings::runPersistedBacktestConfiguration(configuration, storage);
  ASSERT_TRUE(first.ok()) << first.error().message;
  ASSERT_TRUE(second.ok()) << second.error().message;
  EXPECT_NE(first.value().resultId, second.value().resultId);
  EXPECT_EQ(first.value().canonicalResultHash,
            second.value().canonicalResultHash);
}

} // namespace
