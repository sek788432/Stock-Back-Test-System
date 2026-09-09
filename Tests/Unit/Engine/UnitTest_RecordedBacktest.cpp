#include "Bte/Engine/Backtest.h"

#include "Bte/Core/Time.h"
#include "Bte/Data/ReleaseSnapshot.h"
#include "Bte/Results/ResultStore.h"
#include "ResultStoreTestHooks.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <limits>
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
      batchStore.value()->openResult(batch.value().persisted.resultId);
  const auto pacedOpened =
      pacedStore.value()->openResult(paced.value().persisted.resultId);
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
  auto opened = store.value()->openResult(recorded.value().persisted.resultId);
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
  auto opened = store.value()->openResult(recorded.value().persisted.resultId);
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
  auto opened = store.value()->openResult(recorded.value().persisted.resultId);
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

  auto opened = store.value()->openResult(recorded.value().persisted.resultId);
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
  auto opened = store.value()->openResult(recorded.value().persisted.resultId);
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
  auto opened = store.value()->openResult(recorded.value().persisted.resultId);
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
      store.value()->openResult(retried.value().persisted.resultId);
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
  EXPECT_EQ(listed.value().size(), 1U);
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
      store.value()->openResult(recorded.value().persisted.resultId);
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
      store.value()->openResult(recorded.value().persisted.resultId);
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

} // namespace
