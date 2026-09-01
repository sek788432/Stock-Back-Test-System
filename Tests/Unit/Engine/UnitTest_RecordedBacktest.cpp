#include "Bte/Engine/Backtest.h"

#include "Bte/Core/Time.h"
#include "Bte/Data/ReleaseSnapshot.h"
#include "Bte/Results/ResultStore.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

class RecordedBacktestTest : public testing::Test {
protected:
  void SetUp() override {
    root_ =
        std::filesystem::temp_directory_path() /
        ("bte-recorded-backtest-" +
         std::string{
             testing::UnitTest::GetInstance()->current_test_info()->name()});
    std::filesystem::remove_all(root_);
    std::filesystem::create_directories(root_ / "Source");
    std::ofstream source{root_ / "Source" / "SYN.csv"};
    source << "symbol,ts,open,high,low,close,volume,schemaName\n"
              "SYN,2024-01-01 23:00:00+00:00,100,102,99,101,1200,ohlcv-1h\n"
              "SYN,2024-01-02 00:00:00+00:00,101,103,100,102,2300,ohlcv-1h\n";
    source.close();
    auto built = bte::data::buildReleaseSnapshot({
        .sourceDirectory = root_ / "Source",
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
    auto selected = reader.value()->select({
        .symbols = {"SYN"},
        .range = {.start = timestamp("2024-01-01 23:00:00+00:00"),
                  .end = timestamp("2024-01-02 01:00:00+00:00")},
        .timeframe = "ohlcv-1h",
    });
    ASSERT_TRUE(selected.ok()) << selected.error().message;
    selection_ = std::move(selected).value().identity;
  }

  void TearDown() override { std::filesystem::remove_all(root_); }

  static bte::core::Timestamp timestamp(const std::string &text) {
    return bte::core::time::parseIso8601(text).value();
  }

  [[nodiscard]] bte::engine::BacktestRequest request() const {
    return {
        .bars = {{.ts = timestamp("2024-01-01 23:00:00+00:00"),
                  .open = 100,
                  .high = 102,
                  .low = 99,
                  .close = 101,
                  .volume = 1200},
                 {.ts = timestamp("2024-01-02 00:00:00+00:00"),
                  .open = 101,
                  .high = 103,
                  .low = 100,
                  .close = 102,
                  .volume = 2300}},
        .symbol = "SYN",
        .initialCapitalMicrodollars = 2'000'000'000,
        .quantityShares = 10,
    };
  }

  [[nodiscard]] bte::results::RunDescriptor descriptor() const {
    return {
        .universe = {"SYN"},
        .range = {.start = timestamp("2024-01-01 23:00:00+00:00"),
                  .end = timestamp("2024-01-02 01:00:00+00:00")},
        .initialCapitalMicrodollars = 2'000'000'000,
        .strategyId = "starter",
        .strategyHash = std::string(64, 'c'),
        .dataSelection = selection_,
    };
  }

  std::filesystem::path root_;
  bte::data::DataSelectionIdentity selection_;
};

TEST_F(RecordedBacktestTest,
       completedRunPromotesTheExactAuthoritativeEngineTimeline) {
  auto store = bte::results::ResultStore::open(root_ / "Store", root_ / "Data");
  ASSERT_TRUE(store.ok()) << store.error().message;
  auto writer = store.value()->begin(descriptor());
  ASSERT_TRUE(writer.ok()) << writer.error().message;

  auto recorded = bte::engine::runBacktestAndRecord(request(), *writer.value());

  ASSERT_TRUE(recorded.ok()) << recorded.error().message;
  ASSERT_TRUE(recorded.value().backtest.has_value());
  EXPECT_EQ(recorded.value().status, bte::results::RunStatus::completed);
  auto opened = store.value()->openResult(recorded.value().persisted.resultId);
  ASSERT_TRUE(opened.ok()) << opened.error().message;
  EXPECT_EQ(opened.value().records,
            recorded.value().backtest->canonicalRecords);
  EXPECT_EQ(opened.value().summary.finalEquityMicrodollars,
            recorded.value().backtest->equityMicrodollars);
}

TEST_F(RecordedBacktestTest,
       operationalStrategyFailurePromotesOnlyATypedDiagnosticTimeline) {
  auto store = bte::results::ResultStore::open(root_ / "Store", root_ / "Data");
  ASSERT_TRUE(store.ok()) << store.error().message;
  auto writer = store.value()->begin(descriptor());
  ASSERT_TRUE(writer.ok()) << writer.error().message;
  auto invalid = request();
  invalid.selectableStrategy = bte::strategy::SelectableStrategyPlan{};

  auto recorded = bte::engine::runBacktestAndRecord(invalid, *writer.value());

  ASSERT_TRUE(recorded.ok()) << recorded.error().message;
  EXPECT_FALSE(recorded.value().backtest.has_value());
  EXPECT_TRUE(recorded.value().terminalError.has_value());
  EXPECT_EQ(recorded.value().status, bte::results::RunStatus::failed);
  auto opened = store.value()->openResult(recorded.value().persisted.resultId);
  ASSERT_TRUE(opened.ok()) << opened.error().message;
  ASSERT_EQ(opened.value().records.size(), 1);
  EXPECT_EQ(opened.value().records.front().family,
            bte::results::RecordFamily::terminalDiagnostic);
  EXPECT_FALSE(opened.value().summary.finalEquityMicrodollars.has_value());
}

} // namespace
