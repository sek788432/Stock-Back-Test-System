#include "Bte/Results/ResultStore.h"

#include "Bte/Core/Time.h"
#include "Bte/Data/ReleaseSnapshot.h"

#include "ResultStoreTestHooks.h"

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
  const auto database =
      std::unique_ptr<sqlite3, decltype(&sqlite3_close)>{rawDatabase,
                                                        &sqlite3_close};
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
  const auto emptyData =
      bte::results::ResultStore::open(root_ / "Store", {});
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
  ASSERT_TRUE(writer.value()
                  ->finalizeAndPromote(
                      bte::results::RunStatus::completed,
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
      "UPDATE result_meta SET result_id='invalid'",
      "UPDATE result_meta SET canonical_hash='invalid'",
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
    const auto resultPath =
        storeRoot / "Results" / (resultId + ".bteresult");
    executeSql(resultPath, mutations[index]);
    const auto opened = store->openResult(resultId);
    EXPECT_FALSE(opened.ok()) << mutations[index];
    EXPECT_EQ(opened.error().code, bte::core::ErrorCode::schemaMismatch)
        << mutations[index];
  }
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

} // namespace
