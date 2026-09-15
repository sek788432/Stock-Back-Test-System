#include "Bte/Bindings/ResultReplay.h"
#include "Bte/Core/Time.h"
#include "Bte/Data/ReleaseSnapshot.h"
#include "Bte/Results/ResultStore.h"

#include <gtest/gtest.h>

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
                        .pnlMicrodollars = 0,
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
                        .pnlMicrodollars = 0,
                        .positionShares = 10},
        CanonicalRecord{.sequence = 3,
                        .timestamp = timestamp("2024-01-02 01:00:00+00:00"),
                        .symbol = "SYN",
                        .family = RecordFamily::portfolio,
                        .cashMicrodollars = 970'000'000,
                        .marketValueMicrodollars = 1'040'000'000,
                        .equityMicrodollars = 2'010'000'000,
                        .pnlMicrodollars = 10'000'000,
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
                        .pnlMicrodollars = 20'000'000,
                        .positionShares = 0},
        CanonicalRecord{.sequence = 6,
                        .timestamp = timestamp("2024-01-03 00:00:00+00:00"),
                        .symbol = "SYN",
                        .family = RecordFamily::portfolio,
                        .cashMicrodollars = 2'020'000'000,
                        .marketValueMicrodollars = 0,
                        .equityMicrodollars = 2'020'000'000,
                        .pnlMicrodollars = 20'000'000,
                        .positionShares = 0},
    };
  }

  struct PresentedSequence final {
    std::vector<bte::bindings::ResultReplayFrame> frames;
    std::vector<int> progress;

    bool operator==(const PresentedSequence &) const = default;
  };

  static PresentedSequence snapshots(bte::bindings::ResultReplay &replay) {
    PresentedSequence sequence;
    if (replay.current() == nullptr) {
      return sequence;
    }
    sequence.frames.push_back(*replay.current());
    sequence.progress.push_back(replay.progressPercent());
    while (replay.stepForward()) {
      sequence.frames.push_back(*replay.current());
      sequence.progress.push_back(replay.progressPercent());
    }
    return sequence;
  }

  static std::vector<bte::bindings::ResultReplayFrame> expectedHourly() {
    using bte::bindings::ResultReplayFill;
    using bte::bindings::ResultReplayFrame;
    using bte::bindings::ResultReplayPortfolio;
    return {
        ResultReplayFrame{
            .candle = {.ts = timestamp("2024-01-01 23:00:00+00:00"),
                       .open = 100.0,
                       .high = 102.0,
                       .low = 99.0,
                       .close = 101.0,
                       .volume = 1'200.0},
            .fills = {},
            .portfolio = {.cash = 2'000.0,
                          .positionShares = 0,
                          .marketValue = 0.0,
                          .equity = 2'000.0,
                          .pnl = 0.0},
            .partialUtcDay = false},
        ResultReplayFrame{
            .candle = {.ts = timestamp("2024-01-02 00:00:00+00:00"),
                       .open = 101.0,
                       .high = 104.0,
                       .low = 100.0,
                       .close = 103.0,
                       .volume = 2'300.0},
            .fills = {ResultReplayFill{
                .timestamp = timestamp("2024-01-02 00:00:00+00:00"),
                .isBuy = true,
                .quantityShares = 10,
                .price = 103.0,
                .amount = 1'030.0}},
            .portfolio = {.cash = 970.0,
                          .positionShares = 10,
                          .marketValue = 1'030.0,
                          .equity = 2'000.0,
                          .pnl = 0.0},
            .partialUtcDay = false},
        ResultReplayFrame{
            .candle = {.ts = timestamp("2024-01-02 01:00:00+00:00"),
                       .open = 103.0,
                       .high = 105.0,
                       .low = 102.0,
                       .close = 104.0,
                       .volume = 3'400.0},
            .fills = {},
            .portfolio = {.cash = 970.0,
                          .positionShares = 10,
                          .marketValue = 1'040.0,
                          .equity = 2'010.0,
                          .pnl = 10.0},
            .partialUtcDay = false},
        ResultReplayFrame{
            .candle = {.ts = timestamp("2024-01-02 02:00:00+00:00"),
                       .open = 104.0,
                       .high = 107.0,
                       .low = 103.0,
                       .close = 106.0,
                       .volume = 4'500.0},
            .fills = {ResultReplayFill{
                .timestamp = timestamp("2024-01-02 02:00:00+00:00"),
                .isBuy = false,
                .quantityShares = 10,
                .price = 105.0,
                .amount = 1'050.0}},
            .portfolio = {.cash = 2'020.0,
                          .positionShares = 0,
                          .marketValue = 0.0,
                          .equity = 2'020.0,
                          .pnl = 20.0},
            .partialUtcDay = false},
        ResultReplayFrame{
            .candle = {.ts = timestamp("2024-01-03 00:00:00+00:00"),
                       .open = 106.0,
                       .high = 108.0,
                       .low = 105.0,
                       .close = 107.0,
                       .volume = 5'600.0},
            .fills = {},
            .portfolio = {.cash = 2'020.0,
                          .positionShares = 0,
                          .marketValue = 0.0,
                          .equity = 2'020.0,
                          .pnl = 20.0},
            .partialUtcDay = false},
    };
  }

  static std::vector<bte::bindings::ResultReplayFrame> expectedDaily() {
    auto expected = expectedHourly();
    return {
        {.candle = {.ts = timestamp("2024-01-01 00:00:00+00:00"),
                    .open = 100.0,
                    .high = 102.0,
                    .low = 99.0,
                    .close = 101.0,
                    .volume = 1'200.0},
         .fills = {},
         .portfolio = expected[0].portfolio,
         .partialUtcDay = true},
        {.candle = {.ts = timestamp("2024-01-02 00:00:00+00:00"),
                    .open = 101.0,
                    .high = 107.0,
                    .low = 100.0,
                    .close = 106.0,
                    .volume = 10'200.0},
         .fills = {expected[1].fills[0], expected[3].fills[0]},
         .portfolio = expected[3].portfolio,
         .partialUtcDay = true},
        {.candle = {.ts = timestamp("2024-01-03 00:00:00+00:00"),
                    .open = 106.0,
                    .high = 108.0,
                    .low = 105.0,
                    .close = 107.0,
                    .volume = 5'600.0},
         .fills = {},
         .portfolio = expected[4].portfolio,
         .partialUtcDay = true},
    };
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
  const auto hourly = snapshots(*replay.value());
  EXPECT_EQ(hourly.frames, expectedHourly());
  EXPECT_EQ(hourly.progress, (std::vector<int>{20, 40, 60, 80, 100}));
  ASSERT_TRUE(replay.value()->seek(1));
  ASSERT_NE(replay.value()->current(), nullptr);
  EXPECT_EQ(*replay.value()->current(), expectedHourly()[1]);
  EXPECT_EQ(replay.value()->progressPercent(), 40);

  auto daily = bte::bindings::ResultReplay::open(
      root_ / "Results", root_ / "Data", resultId_,
      bte::bindings::ResultReplayTimeframe::dailyUtc);
  ASSERT_TRUE(daily.ok()) << daily.error().message;
  const auto days = snapshots(*daily.value());
  EXPECT_EQ(days.frames, expectedDaily());
  EXPECT_EQ(days.progress, (std::vector<int>{33, 66, 100}));
  ASSERT_TRUE(daily.value()->seek(1));
  ASSERT_NE(daily.value()->current(), nullptr);
  EXPECT_EQ(*daily.value()->current(), expectedDaily()[1]);
  EXPECT_EQ(daily.value()->progressPercent(), 66);
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

  const auto firstSequence = snapshots(*first.value());
  const auto secondSequence = snapshots(*second.value());
  EXPECT_EQ(firstSequence, secondSequence);
  EXPECT_EQ(firstSequence.frames, expectedHourly());
  EXPECT_EQ(firstSequence.progress, (std::vector<int>{20, 40, 60, 80, 100}));
}

TEST_F(ReplayFixture, diagnosticReplayStopsAtLastTrustworthyFrame) {
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
      .strategyId = "issue10-diagnostic",
      .strategyHash = std::string(64, 'e'),
      .dataSelection = selected.value().identity,
  });
  ASSERT_TRUE(writer.ok()) << writer.error().message;
  auto records = literalTimeline();
  records.resize(4);
  ASSERT_TRUE(writer.value()->append(records).ok());
  const std::string terminalReason = "strategy stopped after trusted prefix";
  auto finalized = writer.value()->finalizeAndPromote(
      bte::results::RunStatus::failed, {}, terminalReason);
  ASSERT_TRUE(finalized.ok()) << finalized.error().message;

  auto replay = bte::bindings::ResultReplay::open(
      root_ / "Results", root_ / "Data", finalized.value().resultId,
      bte::bindings::ResultReplayTimeframe::hourly);
  ASSERT_TRUE(replay.ok()) << replay.error().message;
  const auto diagnostic = snapshots(*replay.value());
  const auto expected = expectedHourly();
  ASSERT_EQ(diagnostic.frames.size(), 3U);
  EXPECT_EQ(diagnostic.frames, (std::vector<bte::bindings::ResultReplayFrame>{
                                   expected[0], expected[1], expected[2]}));
  EXPECT_EQ(diagnostic.progress, (std::vector<int>{33, 66, 100}));
  EXPECT_EQ(replay.value()->terminalReason(), terminalReason);
  EXPECT_FALSE(replay.value()->stepForward());
}

} // namespace
