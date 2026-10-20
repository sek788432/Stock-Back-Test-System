#include "Bte/Engine/Backtest.h"

#include "Bte/Core/Time.h"
#include "Bte/Data/ReleaseSnapshot.h"
#include "Bte/Results/ResultStore.h"
#include "ResultStoreTestHooks.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <string>
#include <utility>
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

  void TearDown() override {
    bte::results::testing::clearFailure();
    std::filesystem::remove_all(root_);
  }

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
        .previousDataDays = {},
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

  [[nodiscard]] static bte::core::Result<bte::results::OpenedResult>
  publishAndOpen(bte::results::ResultStore &store,
                 const std::string &resultId) {
    auto published = store.publishDraft(resultId);
    if (!published.ok()) {
      return published.error();
    }
    return store.openResult(resultId);
  }

  std::filesystem::path root_;
  bte::data::DataSelectionIdentity selection_;
};

TEST_F(RecordedBacktestTest, recordedAdvancePersistsOnlyCompleteBarPrefixes) {
  for (const auto completedBars : {0U, 1U, 2U}) {
    const auto storeRoot = root_ / ("Prefix" + std::to_string(completedBars));
    auto store = bte::results::ResultStore::open(storeRoot, root_ / "Data");
    ASSERT_TRUE(store.ok()) << store.error().message;
    auto execution = bte::engine::BacktestExecution::startRecorded(
        request(), *store.value(), descriptor());
    ASSERT_TRUE(execution.ok()) << execution.error().message;
    for (auto bar = 0U; bar < completedBars; ++bar) {
      ASSERT_TRUE(execution.value()->advance().ok());
    }
    auto expected = execution.value()->result().canonicalRecords;
    if (completedBars == 0U) {
      expected.clear();
    }
    std::move(execution).value().reset();
    std::move(store).value().reset();
    auto recovered = bte::results::ResultStore::open(storeRoot, root_ / "Data");
    ASSERT_TRUE(recovered.ok()) << recovered.error().message;
    const auto drafts = recovered.value()->listDrafts();
    ASSERT_TRUE(drafts.ok()) << drafts.error().message;
    ASSERT_EQ(drafts.value().size(), 1U);
    const auto opened =
        publishAndOpen(*recovered.value(), drafts.value().front().resultId);
    ASSERT_TRUE(opened.ok()) << opened.error().message;
    EXPECT_EQ(opened.value().status, bte::results::RunStatus::interrupted);
    EXPECT_EQ(opened.value().records, expected);
    EXPECT_FALSE(opened.value().summary.finalEquityMicrodollars.has_value());
    EXPECT_FALSE(opened.value().summary.pnlMicrodollars.has_value());
  }
}

TEST_F(RecordedBacktestTest,
       appendFailureNeverExposesOrRetriesAnUncommittedBar) {
  for (const auto committedBars : {0U, 1U}) {
    const auto storeRoot = root_ / ("Failure" + std::to_string(committedBars));
    auto store = bte::results::ResultStore::open(storeRoot, root_ / "Data");
    ASSERT_TRUE(store.ok());
    auto execution = bte::engine::BacktestExecution::startRecorded(
        request(), *store.value(), descriptor());
    ASSERT_TRUE(execution.ok()) << execution.error().message;
    if (committedBars != 0U) {
      ASSERT_TRUE(execution.value()->advance().ok());
    }
    auto previous = execution.value()->result();
    if (committedBars == 0U) {
      previous.canonicalRecords.clear();
    }
    bte::results::testing::failNext(
        bte::results::testing::FailurePoint::recordTransaction);
    const auto failed = execution.value()->advance();
    ASSERT_FALSE(failed.ok());
    EXPECT_EQ(failed.error().code, bte::core::ErrorCode::internal);
    const auto &current = execution.value()->result();
    EXPECT_EQ(current.barsProcessed, committedBars);
    EXPECT_EQ(current.cashMicrodollars, previous.cashMicrodollars);
    EXPECT_EQ(current.equityMicrodollars, previous.equityMicrodollars);
    EXPECT_EQ(current.positionShares, previous.positionShares);
    EXPECT_EQ(current.fills.size(), previous.fills.size());
    EXPECT_EQ(current.canonicalRecords, previous.canonicalRecords);
    EXPECT_EQ(current.status, bte::results::RunStatus::failed);
    ASSERT_TRUE(current.terminalError.has_value());
    EXPECT_EQ(current.terminalError->code, bte::core::ErrorCode::internal);
    EXPECT_FALSE(execution.value()->advance().ok());
    EXPECT_FALSE(execution.value()->finalizeRecorded().ok());
    std::move(execution).value().reset();
    auto recovered = bte::results::ResultStore::open(storeRoot, root_ / "Data");
    ASSERT_TRUE(recovered.ok()) << recovered.error().message;
    const auto drafts = recovered.value()->listDrafts();
    ASSERT_TRUE(drafts.ok());
    ASSERT_EQ(drafts.value().size(), 1U);
    const auto opened =
        publishAndOpen(*recovered.value(), drafts.value().front().resultId);
    ASSERT_TRUE(opened.ok()) << opened.error().message;
    EXPECT_EQ(opened.value().records, previous.canonicalRecords);
    EXPECT_EQ(opened.value().status, bte::results::RunStatus::interrupted);
  }
}

TEST_F(RecordedBacktestTest,
       recordedFinalizationPreservesCompatibilityRecordsAndHash) {
  auto store = bte::results::ResultStore::open(root_ / "Store", root_ / "Data");
  ASSERT_TRUE(store.ok());
  auto oldExecution = bte::engine::BacktestExecution::start(request());
  ASSERT_TRUE(oldExecution.ok());
  ASSERT_TRUE(oldExecution.value()->runToCompletion().ok());
  auto oldWriter = store.value()->begin(descriptor());
  ASSERT_TRUE(oldWriter.ok());
  const auto compatibility =
      oldExecution.value()->finalizeAndRecord(*oldWriter.value());
  ASSERT_TRUE(compatibility.ok());
  // Independent struct.pack/hashlib schema-2 framing of this literal two-bar
  // starter ledger; pacing and writer ownership are excluded from identity.
  EXPECT_EQ(compatibility.value().persisted.canonicalResultHash,
            "c3c3d2d29713595293c2753f930118ecca78914d2e2265f8ce5f9b97e29dfbc3");
  auto recorded = bte::engine::BacktestExecution::startRecorded(
      request(), *store.value(), descriptor());
  ASSERT_TRUE(recorded.ok());
  EXPECT_FALSE(recorded.value()->finalizeRecorded().ok());
  ASSERT_TRUE(recorded.value()->runToCompletion().ok());
  const auto outcome = recorded.value()->finalizeRecorded();
  ASSERT_TRUE(outcome.ok()) << outcome.error().message;
  EXPECT_EQ(outcome.value().persisted.canonicalResultHash,
            compatibility.value().persisted.canonicalResultHash);
  EXPECT_EQ(recorded.value()->result().canonicalRecords,
            oldExecution.value()->result().canonicalRecords);
  EXPECT_FALSE(recorded.value()->finalizeRecorded().ok());
}

TEST_F(RecordedBacktestTest,
       bothBatchRecordingRoutesStopAfterTheFirstCommittedBar) {
  for (const auto borrowed : {false, true}) {
    const auto storeRoot = root_ / (borrowed ? "Borrowed" : "Owned");
    auto store = bte::results::ResultStore::open(storeRoot, root_ / "Data");
    ASSERT_TRUE(store.ok());
    auto writer =
        borrowed
            ? store.value()->begin(descriptor())
            : bte::core::Result<std::unique_ptr<bte::results::ResultWriter>>{
                  std::unique_ptr<bte::results::ResultWriter>{}};
    ASSERT_TRUE(writer.ok());
    bte::results::testing::failAfter(
        bte::results::testing::FailurePoint::recordTransaction, 1);
    const auto outcome =
        borrowed ? bte::engine::runBacktestAndRecord(request(), *writer.value())
                 : bte::engine::runBacktestAndRecord(request(), *store.value(),
                                                     descriptor());
    ASSERT_FALSE(outcome.ok());
    EXPECT_EQ(outcome.error().code, bte::core::ErrorCode::internal);
    std::move(writer).value().reset();
    auto recovered = bte::results::ResultStore::open(storeRoot, root_ / "Data");
    ASSERT_TRUE(recovered.ok());
    const auto drafts = recovered.value()->listDrafts();
    ASSERT_TRUE(drafts.ok());
    ASSERT_EQ(drafts.value().size(), 1U);
    const auto opened =
        publishAndOpen(*recovered.value(), drafts.value().front().resultId);
    ASSERT_TRUE(opened.ok()) << opened.error().message;
    ASSERT_EQ(opened.value().records.size(), 3U);
    EXPECT_EQ(opened.value().records.back().family,
              bte::results::RecordFamily::portfolio);
    EXPECT_EQ(opened.value().records.back().timestamp,
              request().bars.front().ts);
  }
}

TEST_F(RecordedBacktestTest, recordedTerminalTailRollsBackWithItsMetadata) {
  for (const auto staleMark : {false, true}) {
    const auto storeRoot = root_ / (staleMark ? "StaleTail" : "CancelledTail");
    auto store = bte::results::ResultStore::open(storeRoot, root_ / "Data");
    ASSERT_TRUE(store.ok());
    auto candidate = request();
    if (staleMark) {
      candidate.requiredFinalMarkTimestamp =
          timestamp("2024-01-02 01:00:00+00:00");
    }
    auto execution = bte::engine::BacktestExecution::startRecorded(
        candidate, *store.value(), descriptor());
    ASSERT_TRUE(execution.ok());
    ASSERT_TRUE(execution.value()->advance().ok());
    auto prefix = execution.value()->result().canonicalRecords;
    if (staleMark) {
      ASSERT_TRUE(execution.value()->advance().ok());
      prefix = execution.value()->result().canonicalRecords;
      ASSERT_EQ(prefix.back().family,
                bte::results::RecordFamily::terminalDiagnostic);
      prefix.pop_back();
    } else {
      bte::core::CancellationSource cancellation;
      cancellation.requestCancellation();
      ASSERT_FALSE(execution.value()->advance(cancellation.token()).ok());
    }
    // BEGIN succeeds; COMMIT fails after tail, summary and metadata writes.
    bte::results::testing::failAfter(
        bte::results::testing::FailurePoint::sqlExecution, 1);
    const auto failed = execution.value()->finalizeRecorded();
    ASSERT_FALSE(failed.ok());
    std::move(execution).value().reset();
    auto recovered = bte::results::ResultStore::open(storeRoot, root_ / "Data");
    ASSERT_TRUE(recovered.ok()) << recovered.error().message;
    const auto drafts = recovered.value()->listDrafts();
    ASSERT_TRUE(drafts.ok());
    ASSERT_EQ(drafts.value().size(), 1U);
    const auto opened =
        publishAndOpen(*recovered.value(), drafts.value().front().resultId);
    ASSERT_TRUE(opened.ok()) << opened.error().message;
    EXPECT_EQ(opened.value().records, prefix);
    EXPECT_EQ(opened.value().status, bte::results::RunStatus::interrupted);
  }
}

TEST_F(RecordedBacktestTest, recordedCancellationFinalizesItsTailExactlyOnce) {
  auto store = bte::results::ResultStore::open(root_ / "Store", root_ / "Data");
  ASSERT_TRUE(store.ok());
  auto execution = bte::engine::BacktestExecution::startRecorded(
      request(), *store.value(), descriptor());
  ASSERT_TRUE(execution.ok());
  ASSERT_TRUE(execution.value()->advance().ok());
  bte::core::CancellationSource cancellation;
  cancellation.requestCancellation();
  ASSERT_FALSE(execution.value()->advance(cancellation.token()).ok());
  bte::results::testing::failNext(
      bte::results::testing::FailurePoint::hashFinalization);
  EXPECT_FALSE(execution.value()->finalizeRecorded().ok());
  const auto finalized = execution.value()->finalizeRecorded();
  ASSERT_TRUE(finalized.ok()) << finalized.error().message;
  EXPECT_FALSE(execution.value()->finalizeRecorded().ok());
  const auto opened =
      publishAndOpen(*store.value(), finalized.value().persisted.resultId);
  ASSERT_TRUE(opened.ok()) << opened.error().message;
  EXPECT_EQ(opened.value().status, bte::results::RunStatus::canceled);
  ASSERT_EQ(opened.value().records.size(), 4U);
  EXPECT_EQ(opened.value().records.back().family,
            bte::results::RecordFamily::terminalDiagnostic);
  EXPECT_EQ(opened.value().records,
            execution.value()->result().canonicalRecords);
  EXPECT_FALSE(opened.value().summary.finalEquityMicrodollars.has_value());
}

TEST_F(RecordedBacktestTest,
       completedRunFinalizesDraftBeforeExplicitPublication) {
  auto store = bte::results::ResultStore::open(root_ / "Store", root_ / "Data");
  ASSERT_TRUE(store.ok()) << store.error().message;
  auto writer = store.value()->begin(descriptor());
  ASSERT_TRUE(writer.ok()) << writer.error().message;

  auto recorded = bte::engine::runBacktestAndRecord(request(), *writer.value());

  ASSERT_TRUE(recorded.ok()) << recorded.error().message;
  ASSERT_TRUE(recorded.value().backtest.has_value());
  EXPECT_EQ(recorded.value().status, bte::results::RunStatus::completed);
  const auto beforePublish = store.value()->list();
  ASSERT_TRUE(beforePublish.ok());
  EXPECT_TRUE(beforePublish.value().empty());
  const auto published =
      store.value()->publishDraft(recorded.value().persisted.resultId);
  ASSERT_TRUE(published.ok()) << published.error().message;
  auto opened =
      publishAndOpen(*store.value(), recorded.value().persisted.resultId);
  ASSERT_TRUE(opened.ok()) << opened.error().message;
  EXPECT_EQ(opened.value().records,
            recorded.value().backtest->canonicalRecords);
  EXPECT_EQ(opened.value().summary.finalEquityMicrodollars,
            recorded.value().backtest->equityMicrodollars);
}

TEST_F(RecordedBacktestTest,
       pacedAndBatchExecutionPersistIdenticalFunctionalResults) {
  auto batchStore =
      bte::results::ResultStore::open(root_ / "BatchStore", root_ / "Data");
  ASSERT_TRUE(batchStore.ok()) << batchStore.error().message;
  auto batchWriter = batchStore.value()->begin(descriptor());
  ASSERT_TRUE(batchWriter.ok()) << batchWriter.error().message;
  auto batch =
      bte::engine::runBacktestAndRecord(request(), *batchWriter.value());
  ASSERT_TRUE(batch.ok()) << batch.error().message;

  auto pacedStore =
      bte::results::ResultStore::open(root_ / "PacedStore", root_ / "Data");
  ASSERT_TRUE(pacedStore.ok()) << pacedStore.error().message;
  auto pacedWriter = pacedStore.value()->begin(descriptor());
  ASSERT_TRUE(pacedWriter.ok()) << pacedWriter.error().message;
  auto execution = bte::engine::BacktestExecution::start(request());
  ASSERT_TRUE(execution.ok()) << execution.error().message;
  while (execution.value()->status() == bte::results::RunStatus::running) {
    ASSERT_TRUE(execution.value()->advance().ok());
  }
  auto paced = execution.value()->finalizeAndRecord(*pacedWriter.value());
  ASSERT_TRUE(paced.ok()) << paced.error().message;

  const auto batchOpened =
      publishAndOpen(*batchStore.value(), batch.value().persisted.resultId);
  const auto pacedOpened =
      publishAndOpen(*pacedStore.value(), paced.value().persisted.resultId);
  ASSERT_TRUE(batchOpened.ok()) << batchOpened.error().message;
  ASSERT_TRUE(pacedOpened.ok()) << pacedOpened.error().message;
  EXPECT_EQ(batchOpened.value().records, pacedOpened.value().records);
  EXPECT_EQ(batchOpened.value().canonicalResultHash,
            pacedOpened.value().canonicalResultHash);
}

TEST_F(RecordedBacktestTest,
       cancellationAfterAProcessedSlicePromotesTrustworthyDiagnostics) {
  auto store = bte::results::ResultStore::open(root_ / "Store", root_ / "Data");
  ASSERT_TRUE(store.ok()) << store.error().message;
  auto writer = store.value()->begin(descriptor());
  ASSERT_TRUE(writer.ok()) << writer.error().message;
  auto execution = bte::engine::BacktestExecution::start(request());
  ASSERT_TRUE(execution.ok()) << execution.error().message;
  ASSERT_TRUE(execution.value()->advance().ok());
  bte::core::CancellationSource cancellation;
  cancellation.requestCancellation();

  const auto stopped = execution.value()->advance(cancellation.token());
  ASSERT_FALSE(stopped.ok());
  EXPECT_EQ(stopped.error().code, bte::core::ErrorCode::cancelled);
  auto recorded = execution.value()->finalizeAndRecord(*writer.value());
  ASSERT_TRUE(recorded.ok()) << recorded.error().message;
  EXPECT_EQ(recorded.value().status, bte::results::RunStatus::canceled);
  auto opened =
      publishAndOpen(*store.value(), recorded.value().persisted.resultId);
  ASSERT_TRUE(opened.ok()) << opened.error().message;
  ASSERT_EQ(opened.value().records.size(), 4U);
  EXPECT_EQ(opened.value().records[0].family,
            bte::results::RecordFamily::warning);
  EXPECT_EQ(opened.value().records[1].family,
            bte::results::RecordFamily::order);
  EXPECT_EQ(opened.value().records[2].family,
            bte::results::RecordFamily::portfolio);
  EXPECT_EQ(opened.value().records[3].family,
            bte::results::RecordFamily::terminalDiagnostic);
  EXPECT_FALSE(opened.value().summary.finalEquityMicrodollars.has_value());
}

TEST_F(RecordedBacktestTest,
       recordedFinalizationRejectsWriterSwitchAndUnrecordedFinalization) {
  auto beginStore =
      bte::results::ResultStore::open(root_ / "BeginFailure", root_ / "Data");
  ASSERT_TRUE(beginStore.ok()) << beginStore.error().message;
  bte::results::testing::failNext(
      bte::results::testing::FailurePoint::databaseOpen);
  const auto failedStart = bte::engine::BacktestExecution::startRecorded(
      request(), *beginStore.value(), descriptor());
  bte::results::testing::clearFailure();
  ASSERT_FALSE(failedStart.ok());
  EXPECT_EQ(failedStart.error().code, bte::core::ErrorCode::internal);

  auto store =
      bte::results::ResultStore::open(root_ / "Recorded", root_ / "Data");
  auto otherStore =
      bte::results::ResultStore::open(root_ / "Other", root_ / "Data");
  ASSERT_TRUE(store.ok()) << store.error().message;
  ASSERT_TRUE(otherStore.ok()) << otherStore.error().message;
  auto execution = bte::engine::BacktestExecution::startRecorded(
      request(), *store.value(), descriptor());
  ASSERT_TRUE(execution.ok()) << execution.error().message;
  auto otherWriter = otherStore.value()->begin(descriptor());
  ASSERT_TRUE(otherWriter.ok()) << otherWriter.error().message;
  while (execution.value()->status() == bte::results::RunStatus::running) {
    ASSERT_TRUE(execution.value()->advance().ok());
  }

  const auto switched =
      execution.value()->finalizeAndRecord(*otherWriter.value());
  ASSERT_FALSE(switched.ok());
  EXPECT_EQ(switched.error().code, bte::core::ErrorCode::invalidArgument);
  EXPECT_TRUE(execution.value()->finalizeRecorded().ok());

  auto unrecorded = bte::engine::BacktestExecution::start(request());
  ASSERT_TRUE(unrecorded.ok()) << unrecorded.error().message;
  while (unrecorded.value()->status() == bte::results::RunStatus::running) {
    ASSERT_TRUE(unrecorded.value()->advance().ok());
  }
  const auto unrecordedFinalization = unrecorded.value()->finalizeRecorded();
  ASSERT_FALSE(unrecordedFinalization.ok());
  EXPECT_EQ(unrecordedFinalization.error().code,
            bte::core::ErrorCode::invalidArgument);
}

TEST_F(RecordedBacktestTest,
       operationalFailureAfterAProcessedSliceRetainsEarlierRecords) {
  auto store = bte::results::ResultStore::open(root_ / "Store", root_ / "Data");
  ASSERT_TRUE(store.ok()) << store.error().message;
  auto writer = store.value()->begin(descriptor());
  ASSERT_TRUE(writer.ok()) << writer.error().message;
  auto failing = request();
  failing.bars[1].open = 1.0;
  failing.bars[1].high = 9'000'000'000.0;
  failing.bars[1].low = 1.0;
  failing.bars[1].close = 9'000'000'000.0;
  failing.initialCapitalMicrodollars = std::numeric_limits<std::int64_t>::max();
  failing.quantityShares = 1;

  auto recorded = bte::engine::runBacktestAndRecord(failing, *writer.value());

  ASSERT_TRUE(recorded.ok()) << recorded.error().message;
  EXPECT_EQ(recorded.value().status, bte::results::RunStatus::failed);
  auto opened =
      publishAndOpen(*store.value(), recorded.value().persisted.resultId);
  ASSERT_TRUE(opened.ok()) << opened.error().message;
  ASSERT_EQ(opened.value().records.size(), 4U);
  EXPECT_EQ(opened.value().records[0].family,
            bte::results::RecordFamily::warning);
  EXPECT_EQ(opened.value().records[1].family,
            bte::results::RecordFamily::order);
  EXPECT_EQ(opened.value().records[2].family,
            bte::results::RecordFamily::portfolio);
  EXPECT_EQ(opened.value().records.back().family,
            bte::results::RecordFamily::terminalDiagnostic);
}

TEST_F(RecordedBacktestTest,
       staleRequiredFinalMarkPromotesIncompleteResultWithoutMetrics) {
  auto store = bte::results::ResultStore::open(root_ / "Store", root_ / "Data");
  ASSERT_TRUE(store.ok()) << store.error().message;
  auto writer = store.value()->begin(descriptor());
  ASSERT_TRUE(writer.ok()) << writer.error().message;
  auto incomplete = request();
  incomplete.requiredFinalMarkTimestamp =
      timestamp("2024-01-02 01:00:00+00:00");

  auto recorded =
      bte::engine::runBacktestAndRecord(incomplete, *writer.value());

  ASSERT_TRUE(recorded.ok()) << recorded.error().message;
  EXPECT_EQ(recorded.value().status, bte::results::RunStatus::incomplete);
  ASSERT_TRUE(recorded.value().backtest.has_value());
  EXPECT_EQ(recorded.value().backtest->barsProcessed, 2U);
  auto opened =
      publishAndOpen(*store.value(), recorded.value().persisted.resultId);
  ASSERT_TRUE(opened.ok()) << opened.error().message;
  EXPECT_EQ(opened.value().terminalReason, "StaleFinalMark");
  EXPECT_EQ(opened.value().records.back().family,
            bte::results::RecordFamily::terminalDiagnostic);
  EXPECT_FALSE(opened.value().summary.finalEquityMicrodollars.has_value());
}

TEST_F(RecordedBacktestTest,
       openedResultDeclaresSupportedAndUnsupportedEngineCapabilities) {
  auto store = bte::results::ResultStore::open(root_ / "Store", root_ / "Data");
  ASSERT_TRUE(store.ok()) << store.error().message;
  auto writer = store.value()->begin(descriptor());
  ASSERT_TRUE(writer.ok()) << writer.error().message;
  auto recorded = bte::engine::runBacktestAndRecord(request(), *writer.value());
  ASSERT_TRUE(recorded.ok()) << recorded.error().message;

  auto opened =
      publishAndOpen(*store.value(), recorded.value().persisted.resultId);
  ASSERT_TRUE(opened.ok()) << opened.error().message;
  EXPECT_TRUE(opened.value().capabilities.orders);
  EXPECT_TRUE(opened.value().capabilities.fills);
  EXPECT_TRUE(opened.value().capabilities.postSlicePortfolio);
  EXPECT_TRUE(opened.value().capabilities.slippageCosts);
  EXPECT_TRUE(opened.value().capabilities.warnings);
  EXPECT_TRUE(opened.value().capabilities.terminalDiagnostics);
  EXPECT_FALSE(opened.value().capabilities.trades);
  EXPECT_FALSE(opened.value().capabilities.metrics);
  EXPECT_FALSE(opened.value().capabilities.strategyLogs);
  EXPECT_FALSE(opened.value().capabilities.indicatorSnapshots);
  EXPECT_FALSE(opened.value().capabilities.corporateActions);
  EXPECT_FALSE(opened.value().capabilities.margin);
}

TEST_F(RecordedBacktestTest,
       noSignalRunPersistsOnlyWarningAndPostSliceCheckpoints) {
  auto store = bte::results::ResultStore::open(root_ / "Store", root_ / "Data");
  ASSERT_TRUE(store.ok()) << store.error().message;
  auto writer = store.value()->begin(descriptor());
  ASSERT_TRUE(writer.ok()) << writer.error().message;
  auto noSignal = request();
  noSignal.selectableStrategy = bte::strategy::SelectableStrategyPlan{
      .buy = {.conditions = {bte::strategy::Condition{
                  .source = bte::strategy::ConditionSource::barField,
                  .comparison = bte::strategy::Comparison::greaterThan,
                  .threshold = 1'000.0,
              }}},
      .sell = {},
  };

  auto recorded = bte::engine::runBacktestAndRecord(noSignal, *writer.value());

  ASSERT_TRUE(recorded.ok()) << recorded.error().message;
  auto opened =
      publishAndOpen(*store.value(), recorded.value().persisted.resultId);
  ASSERT_TRUE(opened.ok()) << opened.error().message;
  ASSERT_EQ(opened.value().records.size(), 3U);
  EXPECT_EQ(opened.value().records[0].family,
            bte::results::RecordFamily::warning);
  EXPECT_EQ(opened.value().records[1].family,
            bte::results::RecordFamily::portfolio);
  EXPECT_EQ(opened.value().records[2].family,
            bte::results::RecordFamily::portfolio);
}

TEST_F(RecordedBacktestTest,
       oneBarRunPersistsItsOrderAndOnlyProcessedCheckpoint) {
  auto store = bte::results::ResultStore::open(root_ / "Store", root_ / "Data");
  ASSERT_TRUE(store.ok()) << store.error().message;
  auto oneBarDescriptor = descriptor();
  oneBarDescriptor.range.end = timestamp("2024-01-02 00:00:00+00:00");
  oneBarDescriptor.dataSelection.spans.resize(1);
  auto writer = store.value()->begin(oneBarDescriptor);
  ASSERT_TRUE(writer.ok()) << writer.error().message;
  auto oneBar = request();
  oneBar.bars.resize(1);

  auto recorded = bte::engine::runBacktestAndRecord(oneBar, *writer.value());

  ASSERT_TRUE(recorded.ok()) << recorded.error().message;
  ASSERT_TRUE(recorded.value().backtest.has_value());
  EXPECT_EQ(recorded.value().backtest->barsProcessed, 1U);
  EXPECT_EQ(recorded.value().backtest->orderStatus,
            bte::engine::StarterOrderStatus::cancelledNoFutureMarketData);
  auto opened =
      publishAndOpen(*store.value(), recorded.value().persisted.resultId);
  ASSERT_TRUE(opened.ok()) << opened.error().message;
  ASSERT_EQ(opened.value().records.size(), 3U);
  EXPECT_EQ(opened.value().records[0].family,
            bte::results::RecordFamily::warning);
  EXPECT_EQ(opened.value().records[1].family,
            bte::results::RecordFamily::order);
  EXPECT_EQ(opened.value().records[2].family,
            bte::results::RecordFamily::portfolio);
}

TEST_F(RecordedBacktestTest,
       strategyValidationFailureDoesNotStartOrPromoteARun) {
  auto store = bte::results::ResultStore::open(root_ / "Store", root_ / "Data");
  ASSERT_TRUE(store.ok()) << store.error().message;
  auto invalid = request();
  invalid.selectableStrategy = bte::strategy::SelectableStrategyPlan{};

  auto recorded =
      bte::engine::runBacktestAndRecord(invalid, *store.value(), descriptor());

  ASSERT_FALSE(recorded.ok());
  EXPECT_EQ(recorded.error().code, bte::core::ErrorCode::strategyCompileFailed);
  auto listed = store.value()->list();
  ASSERT_TRUE(listed.ok()) << listed.error().message;
  EXPECT_TRUE(listed.value().empty());
  EXPECT_TRUE(std::filesystem::is_empty(root_ / "Store" / "Staging"));
}

TEST_F(RecordedBacktestTest,
       validationFailureDoesNotCreateOrMutateAPersistedTimeline) {
  auto store = bte::results::ResultStore::open(root_ / "Store", root_ / "Data");
  ASSERT_TRUE(store.ok()) << store.error().message;
  auto invalid = request();
  invalid.symbol.clear();

  const auto recorded =
      bte::engine::runBacktestAndRecord(invalid, *store.value(), descriptor());

  ASSERT_FALSE(recorded.ok());
  EXPECT_EQ(recorded.error().code, bte::core::ErrorCode::invalidArgument);
  auto listed = store.value()->list();
  ASSERT_TRUE(listed.ok()) << listed.error().message;
  EXPECT_TRUE(listed.value().empty());
  EXPECT_TRUE(std::filesystem::is_empty(root_ / "Store" / "Staging"));
}

TEST_F(RecordedBacktestTest,
       completedTimelinePropagatesAppendAndFinalizationFailures) {
  auto store = bte::results::ResultStore::open(root_ / "Store", root_ / "Data");
  ASSERT_TRUE(store.ok()) << store.error().message;

  auto appendWriter = store.value()->begin(descriptor());
  ASSERT_TRUE(appendWriter.ok()) << appendWriter.error().message;
  bte::results::testing::failNext(
      bte::results::testing::FailurePoint::statementPreparation);
  const auto appendFailure =
      bte::engine::runBacktestAndRecord(request(), *appendWriter.value());
  ASSERT_FALSE(appendFailure.ok());
  EXPECT_EQ(appendFailure.error().code, bte::core::ErrorCode::internal);

  auto finalizeWriter = store.value()->begin(descriptor());
  ASSERT_TRUE(finalizeWriter.ok()) << finalizeWriter.error().message;
  bte::results::testing::failNext(
      bte::results::testing::FailurePoint::hashFinalization);
  const auto finalizeFailure =
      bte::engine::runBacktestAndRecord(request(), *finalizeWriter.value());
  ASSERT_FALSE(finalizeFailure.ok());
  EXPECT_EQ(finalizeFailure.error().code, bte::core::ErrorCode::internal);
}

TEST_F(
    RecordedBacktestTest,
    pacedExecutionCanRetryAPrecommitFinalizationFailureWithoutDuplicateRecords) {
  auto store = bte::results::ResultStore::open(root_ / "Store", root_ / "Data");
  ASSERT_TRUE(store.ok()) << store.error().message;
  auto writer = store.value()->begin(descriptor());
  ASSERT_TRUE(writer.ok()) << writer.error().message;
  auto execution = bte::engine::BacktestExecution::start(request());
  ASSERT_TRUE(execution.ok()) << execution.error().message;
  ASSERT_TRUE(execution.value()->runToCompletion().ok());
  bte::results::testing::failNext(
      bte::results::testing::FailurePoint::hashFinalization);

  const auto failed = execution.value()->finalizeAndRecord(*writer.value());
  ASSERT_FALSE(failed.ok());
  EXPECT_EQ(failed.error().code, bte::core::ErrorCode::internal);
  const auto retried = execution.value()->finalizeAndRecord(*writer.value());
  ASSERT_TRUE(retried.ok()) << retried.error().message;
  const auto opened =
      publishAndOpen(*store.value(), retried.value().persisted.resultId);
  ASSERT_TRUE(opened.ok()) << opened.error().message;
  EXPECT_EQ(opened.value().records,
            execution.value()->result().canonicalRecords);
}

TEST_F(RecordedBacktestTest,
       postCommitFinalizationFailureCannotCrashOnSameWriterRetry) {
  auto store = bte::results::ResultStore::open(root_ / "Store", root_ / "Data");
  ASSERT_TRUE(store.ok()) << store.error().message;
  auto writer = store.value()->begin(descriptor());
  ASSERT_TRUE(writer.ok()) << writer.error().message;
  auto execution = bte::engine::BacktestExecution::start(request());
  ASSERT_TRUE(execution.ok()) << execution.error().message;
  ASSERT_TRUE(execution.value()->runToCompletion().ok());
  bte::results::testing::failNext(bte::results::testing::FailurePoint::close);

  const auto failed = execution.value()->finalizeAndRecord(*writer.value());
  ASSERT_FALSE(failed.ok());
  EXPECT_EQ(failed.error().code, bte::core::ErrorCode::internal);
  const auto retried = execution.value()->finalizeAndRecord(*writer.value());
  ASSERT_FALSE(retried.ok());
  EXPECT_EQ(retried.error().code, bte::core::ErrorCode::invalidArgument);
  const auto appended = writer.value()->append({
      {.sequence = execution.value()->result().canonicalRecords.size(),
       .timestamp = timestamp("2024-01-02 01:00:00+00:00"),
       .symbol = "SYN",
       .family = bte::results::RecordFamily::log,
       .text = "must not append after post-commit failure"},
  });
  ASSERT_FALSE(appended.ok());
  EXPECT_EQ(appended.error().code, bte::core::ErrorCode::invalidArgument);
}

TEST_F(RecordedBacktestTest,
       diagnosticTimelinePropagatesAppendAndFinalizationFailures) {
  auto store = bte::results::ResultStore::open(root_ / "Store", root_ / "Data");
  ASSERT_TRUE(store.ok()) << store.error().message;
  auto failing = request();
  failing.bars[1].open = 9'000'000'000.0;
  failing.bars[1].high = 9'000'000'000.0;
  failing.bars[1].low = 9'000'000'000.0;
  failing.bars[1].close = 9'000'000'000.0;
  failing.initialCapitalMicrodollars = std::numeric_limits<std::int64_t>::max();
  failing.quantityShares = bte::engine::maximumStarterQuantityShares;

  auto appendWriter = store.value()->begin(descriptor());
  ASSERT_TRUE(appendWriter.ok()) << appendWriter.error().message;
  bte::results::testing::failNext(
      bte::results::testing::FailurePoint::statementPreparation);
  const auto appendFailure =
      bte::engine::runBacktestAndRecord(failing, *appendWriter.value());
  ASSERT_FALSE(appendFailure.ok());
  EXPECT_EQ(appendFailure.error().code, bte::core::ErrorCode::internal);

  auto finalizeWriter = store.value()->begin(descriptor());
  ASSERT_TRUE(finalizeWriter.ok()) << finalizeWriter.error().message;
  bte::results::testing::failNext(
      bte::results::testing::FailurePoint::hashFinalization);
  const auto finalizeFailure =
      bte::engine::runBacktestAndRecord(failing, *finalizeWriter.value());
  ASSERT_FALSE(finalizeFailure.ok());
  EXPECT_EQ(finalizeFailure.error().code, bte::core::ErrorCode::internal);
}

TEST_F(RecordedBacktestTest,
       recordingRequiresATerminalExecutionAndCanOnlySucceedOnce) {
  auto store = bte::results::ResultStore::open(root_ / "Store", root_ / "Data");
  ASSERT_TRUE(store.ok()) << store.error().message;
  auto writer = store.value()->begin(descriptor());
  ASSERT_TRUE(writer.ok()) << writer.error().message;
  auto execution = bte::engine::BacktestExecution::start(request());
  ASSERT_TRUE(execution.ok()) << execution.error().message;

  const auto running = execution.value()->finalizeAndRecord(*writer.value());

  ASSERT_FALSE(running.ok());
  EXPECT_EQ(running.error().code, bte::core::ErrorCode::invalidArgument);
  EXPECT_EQ(running.error().message,
            "only one terminal backtest result can be recorded");
  ASSERT_TRUE(execution.value()->runToCompletion().ok());
  const auto recorded = execution.value()->finalizeAndRecord(*writer.value());
  ASSERT_TRUE(recorded.ok()) << recorded.error().message;

  const auto repeated = execution.value()->finalizeAndRecord(*writer.value());

  ASSERT_FALSE(repeated.ok());
  EXPECT_EQ(repeated.error().code, bte::core::ErrorCode::invalidArgument);
  EXPECT_EQ(repeated.error().message,
            "only one terminal backtest result can be recorded");
  auto listed = store.value()->list();
  ASSERT_TRUE(listed.ok()) << listed.error().message;
  EXPECT_TRUE(listed.value().empty());
}

TEST_F(RecordedBacktestTest,
       canceledWriterEntryPointLeavesWriterAvailableForARealRun) {
  auto store = bte::results::ResultStore::open(root_ / "Store", root_ / "Data");
  ASSERT_TRUE(store.ok()) << store.error().message;
  auto writer = store.value()->begin(descriptor());
  ASSERT_TRUE(writer.ok()) << writer.error().message;
  bte::core::CancellationSource cancellation;
  cancellation.requestCancellation();

  const auto canceled = bte::engine::runBacktestAndRecord(
      request(), *writer.value(), cancellation.token());

  ASSERT_FALSE(canceled.ok());
  EXPECT_EQ(canceled.error().code, bte::core::ErrorCode::cancelled);
  const auto recorded =
      bte::engine::runBacktestAndRecord(request(), *writer.value());
  ASSERT_TRUE(recorded.ok()) << recorded.error().message;
  const auto opened =
      publishAndOpen(*store.value(), recorded.value().persisted.resultId);
  ASSERT_TRUE(opened.ok()) << opened.error().message;
  EXPECT_EQ(opened.value().records,
            recorded.value().backtest->canonicalRecords);
}

TEST_F(RecordedBacktestTest,
       writerEntryPointPropagatesValidationBeforeAppendingRecords) {
  auto store = bte::results::ResultStore::open(root_ / "Store", root_ / "Data");
  ASSERT_TRUE(store.ok()) << store.error().message;
  auto writer = store.value()->begin(descriptor());
  ASSERT_TRUE(writer.ok()) << writer.error().message;
  auto invalid = request();
  invalid.symbol.clear();

  const auto rejected =
      bte::engine::runBacktestAndRecord(invalid, *writer.value());

  ASSERT_FALSE(rejected.ok());
  EXPECT_EQ(rejected.error().code, bte::core::ErrorCode::invalidArgument);
  const auto recorded =
      bte::engine::runBacktestAndRecord(request(), *writer.value());
  ASSERT_TRUE(recorded.ok()) << recorded.error().message;
  const auto opened =
      publishAndOpen(*store.value(), recorded.value().persisted.resultId);
  ASSERT_TRUE(opened.ok()) << opened.error().message;
  EXPECT_EQ(opened.value().records,
            recorded.value().backtest->canonicalRecords);
}

TEST_F(RecordedBacktestTest,
       canceledStoreEntryPointDoesNotCreateAResultArtifact) {
  auto store = bte::results::ResultStore::open(root_ / "Store", root_ / "Data");
  ASSERT_TRUE(store.ok()) << store.error().message;
  bte::core::CancellationSource cancellation;
  cancellation.requestCancellation();

  const auto canceled = bte::engine::runBacktestAndRecord(
      request(), *store.value(), descriptor(), cancellation.token());

  ASSERT_FALSE(canceled.ok());
  EXPECT_EQ(canceled.error().code, bte::core::ErrorCode::cancelled);
  auto listed = store.value()->list();
  ASSERT_TRUE(listed.ok()) << listed.error().message;
  EXPECT_TRUE(listed.value().empty());
  EXPECT_TRUE(std::filesystem::is_empty(root_ / "Store" / "Staging"));
}

TEST_F(RecordedBacktestTest, mismatchedDescriptorDoesNotCreateAResultArtifact) {
  auto store = bte::results::ResultStore::open(root_ / "Store", root_ / "Data");
  ASSERT_TRUE(store.ok()) << store.error().message;
  auto mismatched = descriptor();
  mismatched.universe = {"OTHER"};

  const auto rejected =
      bte::engine::runBacktestAndRecord(request(), *store.value(), mismatched);

  ASSERT_FALSE(rejected.ok());
  EXPECT_EQ(rejected.error().code, bte::core::ErrorCode::invalidArgument);
  EXPECT_EQ(
      rejected.error().message,
      "Result descriptor does not identify the validated Backtest request");
  auto listed = store.value()->list();
  ASSERT_TRUE(listed.ok()) << listed.error().message;
  EXPECT_TRUE(listed.value().empty());
  EXPECT_TRUE(std::filesystem::is_empty(root_ / "Store" / "Staging"));
}

TEST_F(RecordedBacktestTest,
       recordingRejectsBarsThatDifferFromTheImmutableSelection) {
  auto store = bte::results::ResultStore::open(root_ / "Store", root_ / "Data");
  ASSERT_TRUE(store.ok()) << store.error().message;
  auto altered = request();
  altered.bars.front().close = 100.5;

  const auto recorded =
      bte::engine::runBacktestAndRecord(altered, *store.value(), descriptor());

  ASSERT_FALSE(recorded.ok());
  EXPECT_EQ(recorded.error().code, bte::core::ErrorCode::invalidArgument);
  EXPECT_TRUE(std::filesystem::is_empty(root_ / "Store" / "Staging"));
  EXPECT_TRUE(std::filesystem::is_empty(root_ / "Store" / "Results"));
}

TEST_F(RecordedBacktestTest,
       recordingRejectsUnrepresentableAndDifferentlyRoundedVolume) {
  auto store = bte::results::ResultStore::open(root_ / "Store", root_ / "Data");
  ASSERT_TRUE(store.ok()) << store.error().message;
  auto unrepresentable = request();
  unrepresentable.bars.front().volume = std::numeric_limits<double>::max();

  const auto rejected = bte::engine::runBacktestAndRecord(
      unrepresentable, *store.value(), descriptor());

  ASSERT_FALSE(rejected.ok());
  EXPECT_EQ(rejected.error().code, bte::core::ErrorCode::invalidArgument);
  EXPECT_TRUE(std::filesystem::is_empty(root_ / "Store" / "Staging"));

  auto roundedStore =
      bte::results::ResultStore::open(root_ / "RoundedStore", root_ / "Data");
  ASSERT_TRUE(roundedStore.ok()) << roundedStore.error().message;
  auto differentlyRounded = request();
  differentlyRounded.bars.front().volume = 1'200.0000015;

  const auto mismatch = bte::engine::runBacktestAndRecord(
      differentlyRounded, *roundedStore.value(), descriptor());

  ASSERT_FALSE(mismatch.ok());
  EXPECT_EQ(mismatch.error().code, bte::core::ErrorCode::invalidArgument);
  EXPECT_TRUE(std::filesystem::is_empty(root_ / "RoundedStore" / "Staging"));
}

} // namespace
