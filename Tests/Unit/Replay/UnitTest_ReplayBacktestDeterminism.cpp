#include "Bte/Bindings/BacktestSessionVm.h"
#include "Bte/Data/ReleaseSnapshot.h"
#include "Bte/Results/ResultStore.h"

#include <gtest/gtest.h>

#include <QDate>

#include <filesystem>
#include <string>

namespace {

class ReplayBacktestDeterminism : public testing::Test {
protected:
  void SetUp() override { std::filesystem::remove_all(root_); }
  void TearDown() override { std::filesystem::remove_all(root_); }

  const std::filesystem::path root_ = std::filesystem::temp_directory_path() /
                                      "bte-replay-backtest-determinism";
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
}

} // namespace
