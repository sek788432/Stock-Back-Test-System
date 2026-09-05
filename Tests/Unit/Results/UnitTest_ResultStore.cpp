#include "Bte/Results/ResultStore.h"

#include "Bte/Core/Time.h"
#include "Bte/Data/ReleaseSnapshot.h"

#include "ResultStoreTestHooks.h"
#include "SegmentRetentionTestHooks.h"

#include <gtest/gtest.h>
#include <sqlite3.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <ranges>
#include <string>
#include <utility>
#include <vector>

namespace {

void executeSql(const std::filesystem::path &path, const std::string &sql) {
  sqlite3 *rawDatabase = nullptr;
  ASSERT_EQ(sqlite3_open(path.string().c_str(), &rawDatabase), SQLITE_OK);
  const auto database = std::unique_ptr<sqlite3, decltype(&sqlite3_close)>{
      rawDatabase, &sqlite3_close};
  char *rawMessage = nullptr;
  const auto status =
      sqlite3_exec(database.get(), sql.c_str(), nullptr, nullptr, &rawMessage);
  const auto message =
      rawMessage == nullptr ? std::string{} : std::string{rawMessage};
  sqlite3_free(rawMessage);
  ASSERT_EQ(status, SQLITE_OK) << message;
}

class ResultStoreFixture : public testing::Test {
protected:
  void SetUp() override {
    root_ =
        std::filesystem::temp_directory_path() /
        ("bte-result-store-" +
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

  [[nodiscard]] bte::results::RunDescriptor descriptor() const {
    return {
        .universe = {"SYN"},
        .range = {.start = timestamp("2024-01-01 23:00:00+00:00"),
                  .end = timestamp("2024-01-02 01:00:00+00:00")},
        .initialCapitalMicrodollars = 100'000'000,
        .strategyId = "starter",
        .strategyHash = std::string(64, 'c'),
        .dataSelection = selection_,
    };
  }

  [[nodiscard]] std::vector<bte::results::CanonicalRecord> records() const {
    return {
        {.sequence = 0,
         .timestamp = timestamp("2024-01-01 23:00:00+00:00"),
         .symbol = "SYN",
         .family = bte::results::RecordFamily::portfolio,
         .cashMicrodollars = 100'000'000,
         .equityMicrodollars = 100'000'000},
        {.sequence = 1,
         .timestamp = timestamp("2024-01-02 00:00:00+00:00"),
         .symbol = "SYN",
         .family = bte::results::RecordFamily::fill,
         .side = bte::results::OrderSide::buy,
         .quantityShares = 10,
         .priceNanodollars = 101'010'000'000,
         .amountMicrodollars = 1'010'100'000},
        {.sequence = 2,
         .timestamp = timestamp("2024-01-02 00:00:00+00:00"),
         .symbol = "SYN",
         .family = bte::results::RecordFamily::portfolio,
         .cashMicrodollars = 98'989'900'000,
         .marketValueMicrodollars = 1'020'000'000,
         .equityMicrodollars = 100'009'900'000,
         .positionShares = 10},
    };
  }

  std::filesystem::path root_;
  bte::data::DataSelectionIdentity selection_;
};

TEST_F(ResultStoreFixture, rejectsInvalidStoreDescriptorAndLifecycleInputs) {
  const auto emptyRoot = bte::results::ResultStore::open({}, root_ / "Data");
  ASSERT_FALSE(emptyRoot.ok());
  EXPECT_EQ(emptyRoot.error().code, bte::core::ErrorCode::invalidArgument);
  const auto emptyData = bte::results::ResultStore::open(root_ / "Store", {});
  ASSERT_FALSE(emptyData.ok());

  auto store =
      bte::results::ResultStore::open(root_ / "ResultsStore", root_ / "Data");
  ASSERT_TRUE(store.ok()) << store.error().message;
  auto invalidDescriptor = descriptor();
  invalidDescriptor.universe.clear();
  const auto invalidBegin = store.value()->begin(invalidDescriptor);
  ASSERT_FALSE(invalidBegin.ok());
  EXPECT_EQ(invalidBegin.error().code, bte::core::ErrorCode::invalidArgument);

  for (const auto &invalidId : {std::string{}, std::string(32, 'z')}) {
    EXPECT_FALSE(store.value()->openResult(invalidId).ok());
    EXPECT_FALSE(store.value()->moveToTrash(invalidId).ok());
    EXPECT_FALSE(store.value()->restore(invalidId).ok());
    EXPECT_FALSE(store.value()->purge(invalidId).ok());
  }
  EXPECT_FALSE(store.value()->openResult(std::string(32, '0')).ok());
  EXPECT_FALSE(store.value()->purge(std::string(32, '0')).ok());
  EXPECT_FALSE(store.value()->importResult({}).ok());
  EXPECT_FALSE(store.value()->importResult(root_ / "missing.bteresult").ok());
}

TEST_F(ResultStoreFixture,
       descriptorValidationRejectsEveryIdentityAndSpanBoundary) {
  auto store =
      bte::results::ResultStore::open(root_ / "ResultsStore", root_ / "Data");
  ASSERT_TRUE(store.ok());
  std::vector<bte::results::RunDescriptor> invalid;
  auto add = [&](const auto &mutate) {
    auto candidate = descriptor();
    mutate(candidate);
    invalid.push_back(std::move(candidate));
  };
  add([](auto &value) { value.universe = {"SYN", "SYN"}; });
  add([](auto &value) { value.range.end = value.range.start; });
  add([](auto &value) { value.initialCapitalMicrodollars = 0; });
  add([](auto &value) { value.strategyId.clear(); });
  add([](auto &value) { value.strategyHash = "bad"; });
  add([](auto &value) { value.dataSelection.snapshotId = "bad"; });
  add([](auto &value) { value.dataSelection.timeframe = "ohlcv-1d"; });
  add([](auto &value) { value.dataSelection.profile.clear(); });
  add([](auto &value) { value.dataSelection.spans.clear(); });
  add([](auto &value) {
    value.dataSelection.spans.front().segmentOrdinal = 2;
  });
  add([](auto &value) { value.dataSelection.spans.front().symbol.clear(); });
  add([](auto &value) { value.dataSelection.spans.front().segmentId = "bad"; });
  add([](auto &value) {
    value.dataSelection.spans.front().segmentHash = std::string(64, 'f');
  });
  add([](auto &value) { value.dataSelection.spans.front().rowCount = 0; });
  add([](auto &value) {
    value.dataSelection.spans.front().firstTimestamp =
        value.dataSelection.spans.front().lastTimestamp +
        std::chrono::milliseconds{1};
  });
  for (std::size_t index = 0; index < invalid.size(); ++index) {
    const auto begun = store.value()->begin(invalid[index]);
    EXPECT_FALSE(begun.ok()) << index;
    EXPECT_EQ(begun.error().code, bte::core::ErrorCode::invalidArgument)
        << index;
  }
}

TEST_F(ResultStoreFixture, writerRejectsEmptyOutOfOrderAndPostFinalizeWrites) {
  auto store =
      bte::results::ResultStore::open(root_ / "ResultsStore", root_ / "Data");
  ASSERT_TRUE(store.ok()) << store.error().message;
  auto writer = store.value()->begin(descriptor());
  ASSERT_TRUE(writer.ok()) << writer.error().message;
  EXPECT_FALSE(writer.value()->append({}).ok());

  auto missingSymbol = records();
  missingSymbol.front().symbol.clear();
  EXPECT_FALSE(writer.value()->append(missingSymbol).ok());
  auto decreasing = records();
  decreasing[1].timestamp = timestamp("2023-12-31 23:00:00+00:00");
  EXPECT_FALSE(writer.value()->append(decreasing).ok());

  ASSERT_TRUE(writer.value()->append(records()).ok());
  EXPECT_FALSE(writer.value()
                   ->finalizeAndPromote(bte::results::RunStatus::completed, {})
                   .ok());
  EXPECT_FALSE(writer.value()
                   ->finalizeAndPromote(
                       bte::results::RunStatus::failed,
                       {.finalEquityMicrodollars = 1, .pnlMicrodollars = 1})
                   .ok());
  ASSERT_TRUE(
      writer.value()
          ->finalizeAndPromote(bte::results::RunStatus::completed,
                               {.finalEquityMicrodollars = 100'009'900'000,
                                .pnlMicrodollars = 9'900})
          .ok());
  EXPECT_FALSE(writer.value()->append(records()).ok());
  EXPECT_FALSE(writer.value()
                   ->finalizeAndPromote(bte::results::RunStatus::failed, {})
                   .ok());
}

TEST_F(ResultStoreFixture, persistedSchemaAndCanonicalMutationsFailClosed) {
  const auto makeResult = [&](const std::filesystem::path &storeRoot) {
    auto store = bte::results::ResultStore::open(storeRoot, root_ / "Data");
    EXPECT_TRUE(store.ok()) << store.error().message;
    auto writer = store.value()->begin(descriptor());
    EXPECT_TRUE(writer.ok()) << writer.error().message;
    EXPECT_TRUE(writer.value()->append(records()).ok());
    auto finalized = writer.value()->finalizeAndPromote(
        bte::results::RunStatus::completed,
        {.finalEquityMicrodollars = 100'009'900'000, .pnlMicrodollars = 9'900});
    EXPECT_TRUE(finalized.ok()) << finalized.error().message;
    return std::pair{std::move(store).value(), finalized.value().resultId};
  };

  const std::vector<std::string> mutations{
      "UPDATE result_meta SET schema_version=99",
      "UPDATE result_meta SET numeric_policy='float'",
      "UPDATE result_meta SET aggregation_policy='local-day'",
      "UPDATE result_meta SET result_id='invalid'",
      "UPDATE result_meta SET canonical_hash='invalid'",
      "UPDATE result_meta SET status=0",
      "DELETE FROM run_configuration",
      "DELETE FROM data_selection",
      "UPDATE data_spans SET row_count=0",
      "UPDATE canonical_records SET sequence=7 WHERE sequence=1",
      "UPDATE canonical_records SET timestamp_ms=0 WHERE sequence=1",
      "UPDATE summary SET pnl=pnl+1",
  };
  for (std::size_t index = 0; index < mutations.size(); ++index) {
    const auto storeRoot = root_ / ("Mutation" + std::to_string(index));
    auto [store, resultId] = makeResult(storeRoot);
    const auto resultPath = storeRoot / "Results" / (resultId + ".bteresult");
    executeSql(resultPath, mutations[index]);
    const auto opened = store->openResult(resultId);
    EXPECT_FALSE(opened.ok()) << mutations[index];
    EXPECT_EQ(opened.error().code, bte::core::ErrorCode::schemaMismatch)
        << mutations[index];
  }
}

TEST_F(ResultStoreFixture,
       missingPersistedTablesAndImmutableDataReturnStructuredFailures) {
  const std::vector<std::string> mutations{
      "DROP TABLE data_spans",
      "DROP TABLE canonical_records",
      "DROP TABLE summary",
  };
  for (std::size_t index = 0; index < mutations.size(); ++index) {
    const auto storeRoot = root_ / ("MissingTable" + std::to_string(index));
    auto store = bte::results::ResultStore::open(storeRoot, root_ / "Data");
    ASSERT_TRUE(store.ok());
    auto writer = store.value()->begin(descriptor());
    ASSERT_TRUE(writer.ok());
    ASSERT_TRUE(writer.value()->append(records()).ok());
    const auto finalized = writer.value()->finalizeAndPromote(
        bte::results::RunStatus::completed,
        {.finalEquityMicrodollars = 100'009'900'000, .pnlMicrodollars = 9'900});
    ASSERT_TRUE(finalized.ok());
    const auto path =
        storeRoot / "Results" / (finalized.value().resultId + ".bteresult");
    executeSql(path, mutations[index]);
    const auto opened = store.value()->openResult(finalized.value().resultId);
    EXPECT_FALSE(opened.ok()) << mutations[index];
    EXPECT_EQ(opened.error().code, bte::core::ErrorCode::schemaMismatch)
        << mutations[index];
  }

  auto store =
      bte::results::ResultStore::open(root_ / "MissingData", root_ / "Data");
  ASSERT_TRUE(store.ok());
  auto writer = store.value()->begin(descriptor());
  ASSERT_TRUE(writer.ok());
  ASSERT_TRUE(writer.value()->append(records()).ok());
  const auto finalized = writer.value()->finalizeAndPromote(
      bte::results::RunStatus::completed,
      {.finalEquityMicrodollars = 100'009'900'000, .pnlMicrodollars = 9'900});
  ASSERT_TRUE(finalized.ok());
  std::filesystem::remove_all(root_ / "Data" / "Snapshots" /
                              selection_.snapshotId);
  const auto opened = store.value()->openResult(finalized.value().resultId);
  ASSERT_FALSE(opened.ok());
  EXPECT_EQ(opened.error().code, bte::core::ErrorCode::dataSnapshotUnavailable);
}

TEST_F(ResultStoreFixture, beginAppendFinalizePromoteListsAndOpensResult) {
  auto store =
      bte::results::ResultStore::open(root_ / "ResultsStore", root_ / "Data");
  ASSERT_TRUE(store.ok()) << store.error().message;
  auto writer = store.value()->begin(descriptor());
  ASSERT_TRUE(writer.ok()) << writer.error().message;
  const auto resultId = writer.value()->resultId();
  ASSERT_EQ(resultId.size(), 32);
  ASSERT_TRUE(writer.value()->append(records()).ok());
  auto finalized = writer.value()->finalizeAndPromote(
      bte::results::RunStatus::completed,
      {.finalEquityMicrodollars = 100'009'900'000, .pnlMicrodollars = 9'900});
  ASSERT_TRUE(finalized.ok()) << finalized.error().message;
  EXPECT_EQ(finalized.value().resultId, resultId);
  EXPECT_EQ(finalized.value().canonicalResultHash.size(), 64);
  EXPECT_TRUE(std::filesystem::exists(root_ / "ResultsStore" / "Results" /
                                      (resultId + ".bteresult")));

  auto listed = store.value()->list();
  ASSERT_TRUE(listed.ok()) << listed.error().message;
  ASSERT_EQ(listed.value().size(), 1);
  EXPECT_EQ(listed.value().front().resultId, resultId);
  EXPECT_TRUE(listed.value().front().available);

  auto opened = store.value()->openResult(resultId);
  ASSERT_TRUE(opened.ok()) << opened.error().message;
  EXPECT_EQ(opened.value().records, records());
  EXPECT_EQ(opened.value().descriptor.dataSelection, selection_);
}

TEST_F(ResultStoreFixture,
       identicalFunctionalRunsHaveDistinctIdsAndEqualCanonicalHashes) {
  auto store =
      bte::results::ResultStore::open(root_ / "ResultsStore", root_ / "Data");
  ASSERT_TRUE(store.ok()) << store.error().message;
  std::vector<bte::results::FinalizedResult> finalized;
  for (int run = 0; run < 2; ++run) {
    auto writer = store.value()->begin(descriptor());
    ASSERT_TRUE(writer.ok()) << writer.error().message;
    ASSERT_TRUE(writer.value()->append(records()).ok());
    auto result = writer.value()->finalizeAndPromote(
        bte::results::RunStatus::completed,
        {.finalEquityMicrodollars = 100'009'900'000, .pnlMicrodollars = 9'900});
    ASSERT_TRUE(result.ok()) << result.error().message;
    finalized.push_back(std::move(result).value());
  }
  EXPECT_NE(finalized[0].resultId, finalized[1].resultId);
  EXPECT_EQ(finalized[0].canonicalResultHash, finalized[1].canonicalResultHash);
}

TEST_F(ResultStoreFixture,
       multiSymbolAndDiagnosticResultsRoundTripNullSummaryAndTerminalReason) {
  auto store =
      bte::results::ResultStore::open(root_ / "ResultsStore", root_ / "Data");
  ASSERT_TRUE(store.ok());
  auto multiSymbol = descriptor();
  multiSymbol.universe.push_back("ZZZ");
  auto writer = store.value()->begin(multiSymbol);
  ASSERT_TRUE(writer.ok());
  auto recordBatch = records();
  EXPECT_NE(recordBatch.front(), recordBatch.back());
  ASSERT_TRUE(writer.value()->append(recordBatch).ok());
  const auto finalized = writer.value()->finalizeAndPromote(
      bte::results::RunStatus::failed, {}, "strategy protocol failed");
  ASSERT_TRUE(finalized.ok());
  const auto opened = store.value()->openResult(finalized.value().resultId);
  ASSERT_TRUE(opened.ok());
  EXPECT_EQ(opened.value().descriptor.universe,
            (std::vector<std::string>{"SYN", "ZZZ"}));
  EXPECT_EQ(opened.value().terminalReason, "strategy protocol failed");
  EXPECT_FALSE(opened.value().summary.finalEquityMicrodollars.has_value());
  EXPECT_FALSE(opened.value().summary.pnlMicrodollars.has_value());
}

TEST_F(ResultStoreFixture, rejectsNonContiguousRecordsAndInvalidTerminalState) {
  auto store =
      bte::results::ResultStore::open(root_ / "ResultsStore", root_ / "Data");
  ASSERT_TRUE(store.ok()) << store.error().message;
  auto writer = store.value()->begin(descriptor());
  ASSERT_TRUE(writer.ok()) << writer.error().message;
  auto invalidRecords = records();
  invalidRecords[1].sequence = 7;
  const auto rejected = writer.value()->append(invalidRecords);
  ASSERT_FALSE(rejected.ok());
  EXPECT_EQ(rejected.error().code, bte::core::ErrorCode::invalidArgument);

  const auto invalidStatus =
      writer.value()->finalizeAndPromote(bte::results::RunStatus::running, {});
  ASSERT_FALSE(invalidStatus.ok());
  EXPECT_EQ(invalidStatus.error().code, bte::core::ErrorCode::invalidArgument);
}

TEST_F(ResultStoreFixture,
       trashRestoreAndPurgeManageCatalogAndSegmentLifetime) {
  auto store =
      bte::results::ResultStore::open(root_ / "ResultsStore", root_ / "Data");
  ASSERT_TRUE(store.ok()) << store.error().message;
  auto writer = store.value()->begin(descriptor());
  ASSERT_TRUE(writer.ok()) << writer.error().message;
  ASSERT_TRUE(writer.value()->append(records()).ok());
  auto finalized = writer.value()->finalizeAndPromote(
      bte::results::RunStatus::completed,
      {.finalEquityMicrodollars = 100'009'900'000, .pnlMicrodollars = 9'900});
  ASSERT_TRUE(finalized.ok()) << finalized.error().message;

  ASSERT_TRUE(store.value()->moveToTrash(finalized.value().resultId).ok());
  EXPECT_TRUE(
      std::filesystem::exists(root_ / "ResultsStore" / "Trash" /
                              (finalized.value().resultId + ".bteresult")));
  auto normalCatalog = store.value()->list();
  ASSERT_TRUE(normalCatalog.ok());
  EXPECT_TRUE(normalCatalog.value().empty());

  ASSERT_TRUE(store.value()->restore(finalized.value().resultId).ok());
  ASSERT_TRUE(store.value()->moveToTrash(finalized.value().resultId).ok());
  ASSERT_TRUE(store.value()->purge(finalized.value().resultId).ok());
  EXPECT_FALSE(
      std::filesystem::exists(root_ / "ResultsStore" / "Trash" /
                              (finalized.value().resultId + ".bteresult")));
  for (const auto &span : selection_.spans) {
    EXPECT_FALSE(std::filesystem::exists(root_ / "Data" / "Segments" /
                                         (span.segmentId + ".btedata")));
  }
}

TEST_F(ResultStoreFixture,
       corruptPromotedResultIsListedUnavailableAndCannotOpen) {
  auto store =
      bte::results::ResultStore::open(root_ / "ResultsStore", root_ / "Data");
  ASSERT_TRUE(store.ok()) << store.error().message;
  auto writer = store.value()->begin(descriptor());
  ASSERT_TRUE(writer.ok()) << writer.error().message;
  ASSERT_TRUE(writer.value()->append(records()).ok());
  auto finalized = writer.value()->finalizeAndPromote(
      bte::results::RunStatus::completed,
      {.finalEquityMicrodollars = 100'009'900'000, .pnlMicrodollars = 9'900});
  ASSERT_TRUE(finalized.ok()) << finalized.error().message;
  std::ofstream corrupt{root_ / "ResultsStore" / "Results" /
                            (finalized.value().resultId + ".bteresult"),
                        std::ios::binary | std::ios::trunc};
  corrupt << "corrupt";
  corrupt.close();

  auto listed = store.value()->list();
  ASSERT_TRUE(listed.ok());
  ASSERT_EQ(listed.value().size(), 1);
  EXPECT_FALSE(listed.value().front().available);
  const auto opened = store.value()->openResult(finalized.value().resultId);
  ASSERT_FALSE(opened.ok());
  EXPECT_EQ(opened.error().code, bte::core::ErrorCode::schemaMismatch);
}

TEST_F(ResultStoreFixture,
       restartRecoversTrustworthyRecordsAsInterruptedAndQuarantinesEmptyRuns) {
  std::string recoverableId;
  std::string emptyId;
  {
    auto store =
        bte::results::ResultStore::open(root_ / "ResultsStore", root_ / "Data");
    ASSERT_TRUE(store.ok()) << store.error().message;
    auto recoverable = store.value()->begin(descriptor());
    ASSERT_TRUE(recoverable.ok()) << recoverable.error().message;
    recoverableId = recoverable.value()->resultId();
    ASSERT_TRUE(recoverable.value()->append(records()).ok());
    auto empty = store.value()->begin(descriptor());
    ASSERT_TRUE(empty.ok()) << empty.error().message;
    emptyId = empty.value()->resultId();
  }

  auto recovered =
      bte::results::ResultStore::open(root_ / "ResultsStore", root_ / "Data");
  ASSERT_TRUE(recovered.ok()) << recovered.error().message;
  auto opened = recovered.value()->openResult(recoverableId);
  ASSERT_TRUE(opened.ok()) << opened.error().message;
  EXPECT_EQ(opened.value().status, bte::results::RunStatus::interrupted);
  EXPECT_EQ(opened.value().records, records());
  EXPECT_FALSE(opened.value().summary.finalEquityMicrodollars.has_value());
  EXPECT_TRUE(std::filesystem::exists(root_ / "ResultsStore" / "Quarantine" /
                                      (emptyId + ".bteresult")));
}

TEST_F(ResultStoreFixture,
       importCreatesValidatedCopyWithIndependentIdentityAndRetention) {
  auto store =
      bte::results::ResultStore::open(root_ / "ResultsStore", root_ / "Data");
  ASSERT_TRUE(store.ok()) << store.error().message;
  auto writer = store.value()->begin(descriptor());
  ASSERT_TRUE(writer.ok()) << writer.error().message;
  ASSERT_TRUE(writer.value()->append(records()).ok());
  auto original = writer.value()->finalizeAndPromote(
      bte::results::RunStatus::completed,
      {.finalEquityMicrodollars = 100'009'900'000, .pnlMicrodollars = 9'900});
  ASSERT_TRUE(original.ok()) << original.error().message;
  const auto source = root_ / "ResultsStore" / "Results" /
                      (original.value().resultId + ".bteresult");

  auto imported = store.value()->importResult(source);

  ASSERT_TRUE(imported.ok()) << imported.error().message;
  EXPECT_NE(imported.value().resultId, original.value().resultId);
  EXPECT_EQ(imported.value().canonicalResultHash,
            original.value().canonicalResultHash);
  EXPECT_TRUE(std::filesystem::exists(source));
  auto opened = store.value()->openResult(imported.value().resultId);
  ASSERT_TRUE(opened.ok()) << opened.error().message;
  EXPECT_EQ(opened.value().records, records());

  ASSERT_TRUE(store.value()->moveToTrash(imported.value().resultId).ok());
  ASSERT_TRUE(store.value()->purge(imported.value().resultId).ok());
  for (const auto &span : selection_.spans) {
    EXPECT_TRUE(std::filesystem::exists(root_ / "Data" / "Segments" /
                                        (span.segmentId + ".btedata")));
  }
}

TEST_F(ResultStoreFixture, promotionCollisionNeverOverwritesExistingArtifact) {
  auto store =
      bte::results::ResultStore::open(root_ / "ResultsStore", root_ / "Data");
  ASSERT_TRUE(store.ok()) << store.error().message;
  auto writer = store.value()->begin(descriptor());
  ASSERT_TRUE(writer.ok()) << writer.error().message;
  ASSERT_TRUE(writer.value()->append(records()).ok());
  const auto collision = root_ / "ResultsStore" / "Results" /
                         (writer.value()->resultId() + ".bteresult");
  {
    std::ofstream sentinel{collision};
    sentinel << "do-not-overwrite";
  }

  const auto promoted = writer.value()->finalizeAndPromote(
      bte::results::RunStatus::completed,
      {.finalEquityMicrodollars = 100'009'900'000, .pnlMicrodollars = 9'900});

  ASSERT_FALSE(promoted.ok());
  std::ifstream sentinel{collision};
  std::string content;
  std::getline(sentinel, content);
  EXPECT_EQ(content, "do-not-overwrite");
}

TEST_F(ResultStoreFixture, importRejectsCorruptAndUnknownSchemaSources) {
  auto store =
      bte::results::ResultStore::open(root_ / "ResultsStore", root_ / "Data");
  ASSERT_TRUE(store.ok()) << store.error().message;
  const auto corrupt = root_ / "corrupt.bteresult";
  {
    std::ofstream output{corrupt};
    output << "not sqlite";
  }

  const auto imported = store.value()->importResult(corrupt);

  ASSERT_FALSE(imported.ok());
  EXPECT_EQ(imported.error().code, bte::core::ErrorCode::schemaMismatch);
  EXPECT_TRUE(std::filesystem::is_empty(root_ / "ResultsStore" / "Results"));
}

TEST_F(ResultStoreFixture, trashAndRestoreCollisionsNeverClobberArtifacts) {
  auto store =
      bte::results::ResultStore::open(root_ / "ResultsStore", root_ / "Data");
  ASSERT_TRUE(store.ok()) << store.error().message;
  auto writer = store.value()->begin(descriptor());
  ASSERT_TRUE(writer.ok()) << writer.error().message;
  ASSERT_TRUE(writer.value()->append(records()).ok());
  auto finalized = writer.value()->finalizeAndPromote(
      bte::results::RunStatus::completed,
      {.finalEquityMicrodollars = 100'009'900'000, .pnlMicrodollars = 9'900});
  ASSERT_TRUE(finalized.ok()) << finalized.error().message;
  const auto filename = finalized.value().resultId + ".bteresult";
  const auto active = root_ / "ResultsStore" / "Results" / filename;
  const auto trash = root_ / "ResultsStore" / "Trash" / filename;
  {
    std::ofstream sentinel{trash};
    sentinel << "trash-sentinel";
  }

  const auto trashed = store.value()->moveToTrash(finalized.value().resultId);

  ASSERT_FALSE(trashed.ok());
  EXPECT_TRUE(std::filesystem::exists(active));
  std::ifstream trashInput{trash};
  std::string content;
  std::getline(trashInput, content);
  EXPECT_EQ(content, "trash-sentinel");

  std::filesystem::remove(trash);
  ASSERT_TRUE(store.value()->moveToTrash(finalized.value().resultId).ok());
  {
    std::ofstream sentinel{active};
    sentinel << "active-sentinel";
  }

  const auto restored = store.value()->restore(finalized.value().resultId);

  ASSERT_FALSE(restored.ok());
  EXPECT_TRUE(std::filesystem::exists(trash));
  std::ifstream activeInput{active};
  std::getline(activeInput, content);
  EXPECT_EQ(content, "active-sentinel");
}

TEST_F(ResultStoreFixture, injectedRecordAndHashFailuresRemainRetryable) {
  auto store =
      bte::results::ResultStore::open(root_ / "ResultsStore", root_ / "Data");
  ASSERT_TRUE(store.ok()) << store.error().message;
  auto writer = store.value()->begin(descriptor());
  ASSERT_TRUE(writer.ok()) << writer.error().message;

  bte::results::testing::failNext(
      bte::results::testing::FailurePoint::recordTransaction);
  const auto failedAppend = writer.value()->append(records());
  ASSERT_FALSE(failedAppend.ok());
  ASSERT_TRUE(writer.value()->append(records()).ok());

  bte::results::testing::failNext(
      bte::results::testing::FailurePoint::hashFinalization);
  const auto failedFinalize = writer.value()->finalizeAndPromote(
      bte::results::RunStatus::completed,
      {.finalEquityMicrodollars = 100'009'900'000, .pnlMicrodollars = 9'900});
  ASSERT_FALSE(failedFinalize.ok());
  const auto finalized = writer.value()->finalizeAndPromote(
      bte::results::RunStatus::completed,
      {.finalEquityMicrodollars = 100'009'900'000, .pnlMicrodollars = 9'900});
  EXPECT_TRUE(finalized.ok()) << finalized.error().message;
}

TEST_F(ResultStoreFixture,
       injectedLowLevelWriteFailuresRollbackAndRemainRetryable) {
  using bte::results::testing::FailurePoint;
  const std::vector<FailurePoint> appendFailures{
      FailurePoint::sqlExecution,           FailurePoint::statementPreparation,
      FailurePoint::integerBinding,         FailurePoint::textBinding,
      FailurePoint::optionalIntegerBinding, FailurePoint::statementExecution,
  };
  for (std::size_t index = 0; index < appendFailures.size(); ++index) {
    auto store = bte::results::ResultStore::open(
        root_ / ("AppendFault" + std::to_string(index)), root_ / "Data");
    ASSERT_TRUE(store.ok());
    auto writer = store.value()->begin(descriptor());
    ASSERT_TRUE(writer.ok());
    bte::results::testing::failNext(appendFailures[index]);
    const auto failed = writer.value()->append(records());
    ASSERT_FALSE(failed.ok()) << index;
    EXPECT_EQ(failed.error().code, bte::core::ErrorCode::internal) << index;
    EXPECT_TRUE(writer.value()->append(records()).ok()) << index;
  }

  const std::vector<FailurePoint> finalizationFailures{
      FailurePoint::sqlExecution,           FailurePoint::statementPreparation,
      FailurePoint::optionalIntegerBinding, FailurePoint::statementExecution,
      FailurePoint::integerBinding,         FailurePoint::textBinding,
  };
  for (std::size_t index = 0; index < finalizationFailures.size(); ++index) {
    auto store = bte::results::ResultStore::open(
        root_ / ("FinalizeFault" + std::to_string(index)), root_ / "Data");
    ASSERT_TRUE(store.ok());
    auto writer = store.value()->begin(descriptor());
    ASSERT_TRUE(writer.ok());
    ASSERT_TRUE(writer.value()->append(records()).ok());
    bte::results::testing::failNext(finalizationFailures[index]);
    const auto failed = writer.value()->finalizeAndPromote(
        bte::results::RunStatus::completed,
        {.finalEquityMicrodollars = 100'009'900'000, .pnlMicrodollars = 9'900});
    ASSERT_FALSE(failed.ok()) << index;
    EXPECT_EQ(failed.error().code, bte::core::ErrorCode::internal) << index;
    const auto retried = writer.value()->finalizeAndPromote(
        bte::results::RunStatus::completed,
        {.finalEquityMicrodollars = 100'009'900'000, .pnlMicrodollars = 9'900});
    EXPECT_TRUE(retried.ok()) << index << ": " << retried.error().message;
  }
}

TEST_F(ResultStoreFixture, injectedDatabaseAndSchemaFailuresFailClosed) {
  using bte::results::testing::FailurePoint;
  bte::results::testing::failNext(FailurePoint::databaseOpen);
  EXPECT_FALSE(
      bte::results::ResultStore::open(root_ / "OpenFault", root_ / "Data")
          .ok());

  const std::vector<FailurePoint> schemaFailures{
      FailurePoint::sqlExecution,       FailurePoint::statementPreparation,
      FailurePoint::integerBinding,     FailurePoint::textBinding,
      FailurePoint::statementExecution,
  };
  for (std::size_t index = 0; index < schemaFailures.size(); ++index) {
    auto store = bte::results::ResultStore::open(
        root_ / ("SchemaFault" + std::to_string(index)), root_ / "Data");
    ASSERT_TRUE(store.ok());
    bte::results::testing::failNext(schemaFailures[index]);
    const auto writer = store.value()->begin(descriptor());
    EXPECT_FALSE(writer.ok()) << index;
    EXPECT_EQ(writer.error().code, bte::core::ErrorCode::internal) << index;
  }
}

TEST_F(ResultStoreFixture,
       filesystemAndCatalogFaultsPreserveSourceAndDestinationArtifacts) {
  const auto blockedRoot = root_ / "BlockedRoot";
  std::ofstream blocked{blockedRoot};
  blocked << "not-a-directory";
  blocked.close();
  EXPECT_FALSE(
      bte::results::ResultStore::open(blockedRoot, root_ / "Data").ok());

  const auto catalogBlockedRoot = root_ / "CatalogBlocked";
  std::filesystem::create_directories(catalogBlockedRoot / "Catalog.sqlite3");
  EXPECT_FALSE(
      bte::results::ResultStore::open(catalogBlockedRoot, root_ / "Data").ok());

  auto store =
      bte::results::ResultStore::open(root_ / "ResultsStore", root_ / "Data");
  ASSERT_TRUE(store.ok());
  std::filesystem::remove_all(root_ / "ResultsStore" / "Results");
  std::ofstream resultsFile{root_ / "ResultsStore" / "Results"};
  resultsFile << "not-a-directory";
  resultsFile.close();
  EXPECT_FALSE(store.value()->list().ok());

  const auto purgeId = std::string(32, 'a');
  std::filesystem::create_directories(root_ / "ResultsStore" / "Trash" /
                                      (purgeId + ".bteresult") / "child");
  const auto purged = store.value()->purge(purgeId);
  ASSERT_FALSE(purged.ok());
  EXPECT_EQ(purged.error().code, bte::core::ErrorCode::internal);
}

TEST_F(ResultStoreFixture,
       injectedClosePromotionAndCatalogFailuresRecoverTerminalResult) {
  using bte::results::testing::FailurePoint;
  for (const auto point : {FailurePoint::close, FailurePoint::promotion,
                           FailurePoint::catalogVisibility}) {
    const auto suffix = std::to_string(static_cast<int>(point));
    const auto storeRoot = root_ / ("Store" + suffix);
    std::string resultId;
    {
      auto store = bte::results::ResultStore::open(storeRoot, root_ / "Data");
      ASSERT_TRUE(store.ok()) << store.error().message;
      auto writer = store.value()->begin(descriptor());
      ASSERT_TRUE(writer.ok()) << writer.error().message;
      resultId = writer.value()->resultId();
      ASSERT_TRUE(writer.value()->append(records()).ok());
      bte::results::testing::failNext(point);
      const auto failed = writer.value()->finalizeAndPromote(
          bte::results::RunStatus::completed,
          {.finalEquityMicrodollars = 100'009'900'000,
           .pnlMicrodollars = 9'900});
      ASSERT_FALSE(failed.ok());
    }
    bte::results::testing::clearFailure();

    auto recovered = bte::results::ResultStore::open(storeRoot, root_ / "Data");
    ASSERT_TRUE(recovered.ok()) << recovered.error().message;
    auto opened = recovered.value()->openResult(resultId);
    ASSERT_TRUE(opened.ok()) << opened.error().message;
    EXPECT_EQ(opened.value().status, bte::results::RunStatus::completed);
    EXPECT_EQ(opened.value().records, records());
  }
}

TEST_F(ResultStoreFixture, injectedSchemaCreationIsQuarantinedOnRestart) {
  bte::results::testing::failNext(
      bte::results::testing::FailurePoint::schemaCreation);
  auto store =
      bte::results::ResultStore::open(root_ / "ResultsStore", root_ / "Data");
  ASSERT_TRUE(store.ok()) << store.error().message;
  const auto failed = store.value()->begin(descriptor());
  ASSERT_FALSE(failed.ok());
  bte::results::testing::clearFailure();
  store =
      bte::results::ResultStore::open(root_ / "ResultsStore", root_ / "Data");
  ASSERT_TRUE(store.ok()) << store.error().message;
  EXPECT_TRUE(std::filesystem::is_empty(root_ / "ResultsStore" / "Staging"));
  EXPECT_FALSE(
      std::filesystem::is_empty(root_ / "ResultsStore" / "Quarantine"));
}

TEST_F(ResultStoreFixture,
       thousandEntryCatalogKeepsValidResultAvailableAndSortsDeterministically) {
  auto store =
      bte::results::ResultStore::open(root_ / "ResultsStore", root_ / "Data");
  ASSERT_TRUE(store.ok()) << store.error().message;
  auto writer = store.value()->begin(descriptor());
  ASSERT_TRUE(writer.ok()) << writer.error().message;
  ASSERT_TRUE(writer.value()->append(records()).ok());
  auto finalized = writer.value()->finalizeAndPromote(
      bte::results::RunStatus::completed,
      {.finalEquityMicrodollars = 100'009'900'000, .pnlMicrodollars = 9'900});
  ASSERT_TRUE(finalized.ok()) << finalized.error().message;
  for (std::size_t index = 0; index < 999; ++index) {
    const auto suffix = std::to_string(index);
    const auto resultId = std::string(32 - suffix.size(), '0') + suffix;
    std::ofstream unavailable{root_ / "ResultsStore" / "Results" /
                              (resultId + ".bteresult")};
    unavailable << "unavailable";
  }

  const auto listedAt = std::chrono::steady_clock::now();
  auto listed = store.value()->list();
  const auto listMilliseconds =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - listedAt)
          .count();

  ASSERT_TRUE(listed.ok()) << listed.error().message;
  RecordProperty("listMilliseconds", listMilliseconds);
  ASSERT_EQ(listed.value().size(), 1'000);
  EXPECT_EQ(std::ranges::count_if(
                listed.value(),
                [](const auto &summary) { return summary.available; }),
            1);
  EXPECT_TRUE(std::ranges::is_sorted(
      listed.value(), [](const auto &left, const auto &right) {
        if (left.savedUtcMillis != right.savedUtcMillis) {
          return left.savedUtcMillis > right.savedUtcMillis;
        }
        return left.resultId < right.resultId;
      }));
}

TEST_F(ResultStoreFixture,
       delayedFaultInjectionExercisesEverySchemaPersistenceStage) {
  using bte::results::testing::FailurePoint;
  const std::vector<std::pair<FailurePoint, std::uint32_t>> faultCounts{
      {FailurePoint::statementPreparation, 5},
      {FailurePoint::textBinding, 14},
      {FailurePoint::integerBinding, 16},
      {FailurePoint::statementExecution, 5},
  };
  for (const auto &[point, count] : faultCounts) {
    for (std::uint32_t occurrence = 0; occurrence < count; ++occurrence) {
      const auto suffix = std::to_string(static_cast<int>(point)) + "-" +
                          std::to_string(occurrence);
      auto store = bte::results::ResultStore::open(
          root_ / ("SchemaStage-" + suffix), root_ / "Data");
      ASSERT_TRUE(store.ok()) << suffix;
      bte::results::testing::failAfter(point, occurrence);
      const auto writer = store.value()->begin(descriptor());
      EXPECT_FALSE(writer.ok()) << suffix;
      bte::results::testing::clearFailure();
    }
  }
}

TEST_F(ResultStoreFixture,
       delayedFaultInjectionExercisesFinalizationValidationAndCatalogStages) {
  using bte::results::testing::FailurePoint;
  const std::vector<std::pair<FailurePoint, std::uint32_t>> faultCounts{
      {FailurePoint::databaseOpen, 2},
      {FailurePoint::sqlExecution, 3},
      {FailurePoint::statementPreparation, 10},
      {FailurePoint::textBinding, 4},
      {FailurePoint::integerBinding, 4},
      {FailurePoint::optionalIntegerBinding, 2},
      {FailurePoint::statementExecution, 3},
  };
  for (const auto &[point, count] : faultCounts) {
    for (std::uint32_t occurrence = 0; occurrence < count; ++occurrence) {
      const auto suffix = std::to_string(static_cast<int>(point)) + "-" +
                          std::to_string(occurrence);
      auto store = bte::results::ResultStore::open(
          root_ / ("FinalizeStage-" + suffix), root_ / "Data");
      ASSERT_TRUE(store.ok()) << suffix;
      auto writer = store.value()->begin(descriptor());
      ASSERT_TRUE(writer.ok()) << suffix;
      ASSERT_TRUE(writer.value()->append(records()).ok()) << suffix;
      bte::results::testing::failAfter(point, occurrence);
      const auto finalized = writer.value()->finalizeAndPromote(
          bte::results::RunStatus::completed,
          {.finalEquityMicrodollars = 100'009'900'000,
           .pnlMicrodollars = 9'900});
      EXPECT_FALSE(finalized.ok()) << suffix;
      bte::results::testing::clearFailure();
    }
  }
}

TEST_F(ResultStoreFixture,
       delayedFaultInjectionExercisesInterruptedRecoveryStages) {
  using bte::results::testing::FailurePoint;
  const std::vector<std::pair<FailurePoint, std::uint32_t>> faultCounts{
      {FailurePoint::databaseOpen, 5},
      {FailurePoint::sqlExecution, 6},
      {FailurePoint::statementPreparation, 17},
      {FailurePoint::textBinding, 4},
      {FailurePoint::integerBinding, 4},
      {FailurePoint::statementExecution, 3},
  };
  for (const auto &[point, count] : faultCounts) {
    for (std::uint32_t occurrence = 0; occurrence < count; ++occurrence) {
      const auto suffix = std::to_string(static_cast<int>(point)) + "-" +
                          std::to_string(occurrence);
      const auto storeRoot = root_ / ("RecoveryStage-" + suffix);
      {
        auto store = bte::results::ResultStore::open(storeRoot, root_ / "Data");
        ASSERT_TRUE(store.ok()) << suffix;
        auto writer = store.value()->begin(descriptor());
        ASSERT_TRUE(writer.ok()) << suffix;
        ASSERT_TRUE(writer.value()->append(records()).ok()) << suffix;
      }
      bte::results::testing::failAfter(point, occurrence);
      static_cast<void>(
          bte::results::ResultStore::open(storeRoot, root_ / "Data"));
      bte::results::testing::clearFailure();
    }
  }
}

TEST_F(ResultStoreFixture,
       delayedFaultInjectionExercisesImportIdentityAndCatalogStages) {
  using bte::results::testing::FailurePoint;
  auto sourceStore =
      bte::results::ResultStore::open(root_ / "SourceStore", root_ / "Data");
  ASSERT_TRUE(sourceStore.ok());
  auto writer = sourceStore.value()->begin(descriptor());
  ASSERT_TRUE(writer.ok());
  ASSERT_TRUE(writer.value()->append(records()).ok());
  auto finalized = writer.value()->finalizeAndPromote(
      bte::results::RunStatus::completed,
      {.finalEquityMicrodollars = 100'009'900'000, .pnlMicrodollars = 9'900});
  ASSERT_TRUE(finalized.ok());
  const auto source = root_ / "SourceStore" / "Results" /
                      (finalized.value().resultId + ".bteresult");
  const std::vector<std::pair<FailurePoint, std::uint32_t>> faultCounts{
      {FailurePoint::databaseOpen, 4},
      {FailurePoint::sqlExecution, 1},
      {FailurePoint::statementPreparation, 16},
      {FailurePoint::textBinding, 3},
      {FailurePoint::integerBinding, 3},
      {FailurePoint::statementExecution, 2},
  };
  for (const auto &[point, count] : faultCounts) {
    for (std::uint32_t occurrence = 0; occurrence < count; ++occurrence) {
      const auto suffix = std::to_string(static_cast<int>(point)) + "-" +
                          std::to_string(occurrence);
      auto destination = bte::results::ResultStore::open(
          root_ / ("ImportStage-" + suffix), root_ / "Data");
      ASSERT_TRUE(destination.ok()) << suffix;
      bte::results::testing::failAfter(point, occurrence);
      const auto imported = destination.value()->importResult(source);
      EXPECT_FALSE(imported.ok()) << suffix;
      bte::results::testing::clearFailure();
    }
  }
  EXPECT_TRUE(std::filesystem::exists(source));
}

TEST_F(ResultStoreFixture,
       recoverySurfacesQuarantinePromotionAndRetentionFaults) {
  const auto prepareInterrupted = [&](const std::filesystem::path &storeRoot,
                                      const bool withRecords) {
    std::string resultId;
    {
      auto store = bte::results::ResultStore::open(storeRoot, root_ / "Data");
      EXPECT_TRUE(store.ok());
      auto writer = store.value()->begin(descriptor());
      EXPECT_TRUE(writer.ok());
      resultId = writer.value()->resultId();
      if (withRecords) {
        EXPECT_TRUE(writer.value()->append(records()).ok());
      }
    }
    return resultId;
  };

  const auto quarantineRoot = root_ / "QuarantineCollision";
  const auto emptyId = prepareInterrupted(quarantineRoot, false);
  const auto collision =
      quarantineRoot / "Quarantine" / (emptyId + ".bteresult");
  std::filesystem::create_directories(collision);
  std::ofstream child{collision / "child"};
  child << "blocks rename";
  child.close();
  EXPECT_FALSE(
      bte::results::ResultStore::open(quarantineRoot, root_ / "Data").ok());

  const auto promotionRoot = root_ / "RecoveryPromotionFault";
  const auto promotionId = prepareInterrupted(promotionRoot, true);
  std::filesystem::remove_all(promotionRoot / "Results");
  std::ofstream blocked{promotionRoot / "Results"};
  blocked << "not a directory";
  blocked.close();
  EXPECT_FALSE(
      bte::results::ResultStore::open(promotionRoot, root_ / "Data").ok());
  EXPECT_TRUE(std::filesystem::exists(promotionRoot / "Staging" /
                                      (promotionId + ".bteresult")));

  for (const auto point : {
           bte::data::testing::RetentionFailurePoint::databaseOpen,
           bte::data::testing::RetentionFailurePoint::boundPreparation,
       }) {
    const auto storeRoot = root_ / ("RecoveryRetention-" +
                                    std::to_string(static_cast<int>(point)));
    const auto resultId = prepareInterrupted(storeRoot, true);
    bte::data::testing::failRetentionAfter(point);
    EXPECT_FALSE(
        bte::results::ResultStore::open(storeRoot, root_ / "Data").ok());
    bte::data::testing::clearRetentionFailure();
    EXPECT_TRUE(std::filesystem::exists(storeRoot / "Results" /
                                        (resultId + ".bteresult")));
  }
}

TEST_F(ResultStoreFixture, appendCatalogAndRetentionFaultsRemainStructured) {
  auto store =
      bte::results::ResultStore::open(root_ / "ResultsStore", root_ / "Data");
  ASSERT_TRUE(store.ok());
  const auto batch = records();
  auto splitWriter = store.value()->begin(descriptor());
  ASSERT_TRUE(splitWriter.ok());
  ASSERT_TRUE(splitWriter.value()->append({batch.front()}).ok());
  ASSERT_TRUE(splitWriter.value()
                  ->append(std::vector<bte::results::CanonicalRecord>{
                      batch.begin() + 1, batch.end()})
                  .ok());

  auto commitWriter = store.value()->begin(descriptor());
  ASSERT_TRUE(commitWriter.ok());
  bte::results::testing::failAfter(
      bte::results::testing::FailurePoint::sqlExecution, 1);
  EXPECT_FALSE(commitWriter.value()->append(records()).ok());
  bte::results::testing::clearFailure();

  bte::results::testing::failNext(
      bte::results::testing::FailurePoint::databaseOpen);
  EXPECT_FALSE(store.value()->begin(descriptor()).ok());
  bte::results::testing::clearFailure();
  EXPECT_FALSE(store.value()->restore(std::string(32, 'e')).ok());

  for (const auto point : {
           bte::results::testing::FailurePoint::databaseOpen,
           bte::results::testing::FailurePoint::statementPreparation,
           bte::results::testing::FailurePoint::textBinding,
           bte::results::testing::FailurePoint::statementExecution,
       }) {
    auto writer = store.value()->begin(descriptor());
    ASSERT_TRUE(writer.ok());
    ASSERT_TRUE(writer.value()->append(records()).ok());
    auto finalized = writer.value()->finalizeAndPromote(
        bte::results::RunStatus::completed,
        {.finalEquityMicrodollars = 100'009'900'000, .pnlMicrodollars = 9'900});
    ASSERT_TRUE(finalized.ok());
    bte::results::testing::failNext(point);
    EXPECT_FALSE(store.value()->moveToTrash(finalized.value().resultId).ok());
    bte::results::testing::clearFailure();
  }

  for (const auto point : {
           bte::data::testing::RetentionFailurePoint::databaseOpen,
           bte::data::testing::RetentionFailurePoint::boundPreparation,
       }) {
    auto writer = store.value()->begin(descriptor());
    ASSERT_TRUE(writer.ok());
    ASSERT_TRUE(writer.value()->append(records()).ok());
    bte::data::testing::failRetentionAfter(point);
    EXPECT_FALSE(
        writer.value()
            ->finalizeAndPromote(bte::results::RunStatus::completed,
                                 {.finalEquityMicrodollars = 100'009'900'000,
                                  .pnlMicrodollars = 9'900})
            .ok());
    bte::data::testing::clearRetentionFailure();
  }
}

TEST_F(ResultStoreFixture, importAndPurgeSurfaceFilesystemAndRetentionFaults) {
  auto sourceStore =
      bte::results::ResultStore::open(root_ / "SourceStore", root_ / "Data");
  ASSERT_TRUE(sourceStore.ok());
  auto writer = sourceStore.value()->begin(descriptor());
  ASSERT_TRUE(writer.ok());
  ASSERT_TRUE(writer.value()->append(records()).ok());
  auto finalized = writer.value()->finalizeAndPromote(
      bte::results::RunStatus::completed,
      {.finalEquityMicrodollars = 100'009'900'000, .pnlMicrodollars = 9'900});
  ASSERT_TRUE(finalized.ok());
  const auto source = root_ / "SourceStore" / "Results" /
                      (finalized.value().resultId + ".bteresult");

  for (const auto leaf : {"Staging", "Results"}) {
    const auto storeRoot = root_ / (std::string{"Import"} + leaf + "Fault");
    auto destination =
        bte::results::ResultStore::open(storeRoot, root_ / "Data");
    ASSERT_TRUE(destination.ok());
    std::filesystem::remove_all(storeRoot / leaf);
    std::ofstream blocked{storeRoot / leaf};
    blocked << "not a directory";
    blocked.close();
    EXPECT_FALSE(destination.value()->importResult(source).ok());
  }
  for (const auto point : {
           bte::data::testing::RetentionFailurePoint::databaseOpen,
           bte::data::testing::RetentionFailurePoint::boundPreparation,
       }) {
    auto destination = bte::results::ResultStore::open(
        root_ / ("ImportRetention-" + std::to_string(static_cast<int>(point))),
        root_ / "Data");
    ASSERT_TRUE(destination.ok());
    bte::data::testing::failRetentionAfter(point);
    EXPECT_FALSE(destination.value()->importResult(source).ok());
    bte::data::testing::clearRetentionFailure();
  }

  ASSERT_TRUE(
      sourceStore.value()->moveToTrash(finalized.value().resultId).ok());
  bte::data::testing::failRetentionAfter(
      bte::data::testing::RetentionFailurePoint::databaseOpen);
  EXPECT_FALSE(sourceStore.value()->purge(finalized.value().resultId).ok());
  bte::data::testing::clearRetentionFailure();
}

} // namespace
