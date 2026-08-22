#include "Bte/Engine/ResultSnapshotStore.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

std::string readAll(const std::filesystem::path &path) {
  std::ifstream input{path};
  std::ostringstream buffer;
  buffer << input.rdbuf();
  return buffer.str();
}

std::filesystem::path tempSnapshotDir() {
  auto dir =
      std::filesystem::temp_directory_path() / "bte-result-snapshot-tests";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  return dir;
}

bte::engine::ReplayResultSnapshot makeSnapshot() {
  return bte::engine::ReplayResultSnapshot{
      .symbol = "AAPL",
      .schemaName = "ohlcv-1d",
      .startIso8601 = "2024-01-01T00:00:00Z",
      .endIso8601 = "2024-06-30T00:00:00Z",
      .initialCapital = 100'000.0,
      .strategyName = "No strategy",
      .strategyArtifactHash = "none",
      .engineConfigHash = "default",
      .dataSnapshot = "csv-fixture",
      .generatedAtIso8601 = "2026-06-04T00:00:00Z",
      .metrics =
          bte::engine::ResultMetrics{
              .totalReturn = 0.125,
              .maxDrawdown = -0.045,
              .winRate = 0.6,
              .tradeCount = 2,
              .finalEquity = 112'500.0,
          },
      .equityCurve =
          {
              {.barIndex = 1, .equity = 100'000.0},
              {.barIndex = 2, .equity = 112'500.0},
          },
      .tradeLog =
          {
              {.barIndex = 1,
               .side = "BUY",
               .price = 100.0,
               .quantity = 10.0,
               .pnl = 0.0},
              {.barIndex = 2,
               .side = "SELL",
               .price = 112.5,
               .quantity = 10.0,
               .pnl = 125.0},
          },
  };
}

} // namespace

TEST(ResultSnapshotStoreTest,
     saveReplayResultSnapshot_writesDurableJsonArtifact) {
  const auto dir = tempSnapshotDir();
  const auto saved = bte::engine::saveReplayResultSnapshot(dir, makeSnapshot());

  ASSERT_TRUE(saved.ok()) << saved.error().message;
  ASSERT_TRUE(std::filesystem::exists(saved.value()));

  const auto json = readAll(saved.value());
  EXPECT_NE(json.find("\"symbol\":\"AAPL\""), std::string::npos);
  EXPECT_NE(json.find("\"schemaName\":\"ohlcv-1d\""), std::string::npos);
  EXPECT_NE(json.find("\"strategyArtifactHash\":\"none\""), std::string::npos);
  EXPECT_NE(json.find("\"finalEquity\":112500"), std::string::npos);
  EXPECT_NE(json.find("\"equityCurve\""), std::string::npos);
  EXPECT_NE(json.find("\"tradeLog\""), std::string::npos);
}

TEST(ResultSnapshotStoreTest,
     saveReplayResultSnapshot_escapesTextAndSupportsEmptyCollections) {
  const auto dir = tempSnapshotDir();
  auto snapshot = makeSnapshot();
  snapshot.symbol = "A\\B\"C\nD\rE\tF";
  snapshot.equityCurve.clear();
  snapshot.tradeLog.clear();

  const auto saved = bte::engine::saveReplayResultSnapshot(dir, snapshot);

  ASSERT_TRUE(saved.ok()) << saved.error().message;
  const auto json = readAll(saved.value());
  EXPECT_NE(json.find("\"symbol\":\"A\\\\B\\\"C\\nD\\rE\\tF\""),
            std::string::npos);
  EXPECT_NE(json.find("\"equityCurve\":[]"), std::string::npos);
  EXPECT_NE(json.find("\"tradeLog\":[]"), std::string::npos);
}

TEST(ResultSnapshotStoreTest,
     saveReplayResultSnapshot_reportsDirectoryAndFileCreationFailures) {
  const auto dir = tempSnapshotDir();
  const auto blockingFile = dir / "not-a-directory";
  std::ofstream{blockingFile} << "occupied";

  auto saved = bte::engine::saveReplayResultSnapshot(blockingFile / "child",
                                                     makeSnapshot());
  ASSERT_FALSE(saved.ok());
  EXPECT_EQ(saved.error().code, bte::core::ErrorCode::permissionDenied);
  EXPECT_NE(saved.error().message.find("Cannot create"), std::string::npos);

  const auto outputPath = dir / "2026-06-04T00-00-00Z-AAPL-none.json";
  std::filesystem::create_directories(outputPath);
  saved = bte::engine::saveReplayResultSnapshot(dir, makeSnapshot());
  ASSERT_FALSE(saved.ok());
  EXPECT_EQ(saved.error().code, bte::core::ErrorCode::permissionDenied);
  EXPECT_NE(saved.error().message.find("Cannot write"), std::string::npos);
}

TEST(ResultSnapshotStoreTest,
     listReplayResultSnapshots_extractsComparisonSummaries) {
  const auto dir = tempSnapshotDir();
  const auto saved = bte::engine::saveReplayResultSnapshot(dir, makeSnapshot());
  ASSERT_TRUE(saved.ok()) << saved.error().message;

  const auto summaries = bte::engine::listReplayResultSnapshots(dir);

  ASSERT_TRUE(summaries.ok()) << summaries.error().message;
  ASSERT_EQ(summaries.value().size(), 1U);
  EXPECT_EQ(summaries.value().front().symbol, "AAPL");
  EXPECT_EQ(summaries.value().front().schemaName, "ohlcv-1d");
  EXPECT_EQ(summaries.value().front().strategyName, "No strategy");
  EXPECT_DOUBLE_EQ(summaries.value().front().finalEquity, 112'500.0);
  EXPECT_EQ(summaries.value().front().tradeCount, 2);
}

TEST(ResultSnapshotStoreTest,
     listReplayResultSnapshots_handlesAbsentAndPartialSnapshotDirectories) {
  const auto dir = tempSnapshotDir();
  std::filesystem::remove_all(dir);

  auto summaries = bte::engine::listReplayResultSnapshots(dir);
  ASSERT_TRUE(summaries.ok()) << summaries.error().message;
  EXPECT_TRUE(summaries.value().empty());

  std::filesystem::create_directories(dir / "ignored.json");
  std::ofstream{dir / "ignored.txt"} << "not json";
  std::ofstream{dir / "partial.json"} << "{\"tradeCount\":3}";
  std::ofstream{dir / "unterminated.json"} << "{\"symbol\":\"unterminated}";

  summaries = bte::engine::listReplayResultSnapshots(dir);

  ASSERT_TRUE(summaries.ok()) << summaries.error().message;
  ASSERT_EQ(summaries.value().size(), 2U);
  EXPECT_TRUE(summaries.value()[0].symbol.empty());
  EXPECT_TRUE(summaries.value()[0].schemaName.empty());
  EXPECT_TRUE(summaries.value()[0].strategyName.empty());
  EXPECT_DOUBLE_EQ(summaries.value()[0].totalReturn, 0.0);
  EXPECT_EQ(summaries.value()[0].tradeCount, 3);
  EXPECT_TRUE(summaries.value()[1].symbol.empty());
  EXPECT_EQ(summaries.value()[1].tradeCount, 0);
}

TEST(ResultSnapshotStoreTest,
     listReplayResultSnapshots_rejectsAFileInsteadOfADirectory) {
  const auto dir = tempSnapshotDir();
  const auto file = dir / "not-a-directory";
  std::ofstream{file} << "occupied";

  const auto summaries = bte::engine::listReplayResultSnapshots(file);

  ASSERT_FALSE(summaries.ok());
  EXPECT_EQ(summaries.error().code, bte::core::ErrorCode::internal);
  EXPECT_NE(summaries.error().message.find("Cannot enumerate"),
            std::string::npos);
}

TEST(ResultSnapshotStoreTest,
     listReplayResultSnapshots_reportsDirectoryInspectionFailure) {
  const auto dir = tempSnapshotDir();
  const auto invalidPath = dir / std::string(4'096, 'x');

  const auto summaries = bte::engine::listReplayResultSnapshots(invalidPath);

  ASSERT_FALSE(summaries.ok());
  EXPECT_EQ(summaries.error().code, bte::core::ErrorCode::internal);
  EXPECT_NE(summaries.error().message.find("Cannot inspect"),
            std::string::npos);
}

TEST(ResultSnapshotStoreTest,
     listReplayResultSnapshots_reportsUnreadableSnapshotFile) {
  const auto dir = tempSnapshotDir();
  const auto file = dir / "unreadable.json";
  std::ofstream{file} << "{}";
  std::filesystem::permissions(file, std::filesystem::perms::none,
                               std::filesystem::perm_options::replace);

  const auto summaries = bte::engine::listReplayResultSnapshots(dir);

  std::filesystem::permissions(file, std::filesystem::perms::owner_all,
                               std::filesystem::perm_options::replace);
  ASSERT_FALSE(summaries.ok());
  EXPECT_EQ(summaries.error().code, bte::core::ErrorCode::permissionDenied);
  EXPECT_NE(summaries.error().message.find("Cannot read"), std::string::npos);
}

TEST(ResultSnapshotStoreTest,
     listReplayResultSnapshots_containsMalformedNumericInput) {
  for (const auto *const malformedValue : {"not-a-number", "1e999999"}) {
    const auto dir = tempSnapshotDir();
    std::ofstream{dir / "malformed.json"}
        << "{\"totalReturn\":" << malformedValue << "}";
    bte::core::Result<std::vector<bte::engine::ReplayResultSummary>> summaries{
        std::vector<bte::engine::ReplayResultSummary>{}};

    EXPECT_NO_THROW(summaries = bte::engine::listReplayResultSnapshots(dir));
    ASSERT_FALSE(summaries.ok());
    EXPECT_EQ(summaries.error().code, bte::core::ErrorCode::schemaMismatch);
    EXPECT_NE(summaries.error().message.find("Malformed result snapshot"),
              std::string::npos);
  }
}

TEST(ResultSnapshotStoreTest,
     listReplayResultSnapshots_sortsMultipleArtifactsByPath) {
  const auto dir = tempSnapshotDir();
  auto later = makeSnapshot();
  later.generatedAtIso8601 = "2026-06-05T00:00:00Z";
  later.strategyName = "Later";
  auto earlier = makeSnapshot();
  earlier.generatedAtIso8601 = "2026-06-03T00:00:00Z";
  earlier.strategyName = "Earlier";
  ASSERT_TRUE(bte::engine::saveReplayResultSnapshot(dir, later).ok());
  ASSERT_TRUE(bte::engine::saveReplayResultSnapshot(dir, earlier).ok());

  const auto summaries = bte::engine::listReplayResultSnapshots(dir);

  ASSERT_TRUE(summaries.ok()) << summaries.error().message;
  ASSERT_EQ(summaries.value().size(), 2U);
  EXPECT_EQ(summaries.value()[0].strategyName, "Earlier");
  EXPECT_EQ(summaries.value()[1].strategyName, "Later");
}

TEST(ResultComparisonTest,
     compareReplayResultSummaries_buildsSideBySideMetricRows) {
  const std::vector<bte::engine::ReplayResultSummary> summaries{
      {
          .path = {},
          .symbol = "AAPL",
          .schemaName = {},
          .strategyName = "Mean reversion",
          .totalReturn = 0.12,
          .maxDrawdown = -0.04,
          .winRate = 0.55,
          .tradeCount = 8,
          .finalEquity = 112'000.0,
      },
      {
          .path = {},
          .symbol = "AAPL",
          .schemaName = {},
          .strategyName = "Momentum",
          .totalReturn = 0.2,
          .maxDrawdown = -0.08,
          .winRate = 0.5,
          .tradeCount = 5,
          .finalEquity = 120'000.0,
      },
  };

  const auto rows = bte::engine::compareReplayResultSummaries(summaries);

  ASSERT_EQ(rows.size(), 5U);
  EXPECT_EQ(rows[0].metricName, "Final Equity");
  ASSERT_EQ(rows[0].values.size(), 2U);
  EXPECT_EQ(rows[0].values[0].label, "Mean reversion");
  EXPECT_EQ(rows[0].values[0].value, "112000");
  EXPECT_EQ(rows[0].values[1].label, "Momentum");
  EXPECT_EQ(rows[0].values[1].value, "120000");
  EXPECT_EQ(rows[1].metricName, "Total Return");
  EXPECT_EQ(rows[2].metricName, "Max Drawdown");
  EXPECT_EQ(rows[3].metricName, "Win Rate");
  EXPECT_EQ(rows[4].metricName, "Trade Count");
}
