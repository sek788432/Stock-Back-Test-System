#include "Bte/Results/ResultStore.h"

#include "Bte/Core/Cancellation.h"
#include "Bte/Core/Time.h"
#include "Bte/Data/ReleaseSnapshot.h"
#include "Bte/Data/SegmentRetention.h"

#include "ResultStoreTestHooks.h"
#include "SegmentRetentionTestHooks.h"

#include <gtest/gtest.h>
#include <sqlite3.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <span>
#include <limits>
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

std::int64_t queryInteger(const std::filesystem::path &path,
                          const std::string &sql) {
  sqlite3 *rawDatabase = nullptr;
  EXPECT_EQ(sqlite3_open_v2(path.string().c_str(), &rawDatabase,
                            SQLITE_OPEN_READONLY, nullptr),
            SQLITE_OK);
  const auto database = std::unique_ptr<sqlite3, decltype(&sqlite3_close)>{
      rawDatabase, &sqlite3_close};
  sqlite3_stmt *rawStatement = nullptr;
  EXPECT_EQ(sqlite3_prepare_v2(database.get(), sql.c_str(), -1, &rawStatement,
                               nullptr),
            SQLITE_OK);
  const auto statement =
      std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>{
          rawStatement, &sqlite3_finalize};
  EXPECT_EQ(sqlite3_step(statement.get()), SQLITE_ROW);
  return sqlite3_column_int64(statement.get(), 0);
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
         .marketValueMicrodollars = 0,
         .equityMicrodollars = 100'000'000,
         .pnlMicrodollars = 0,
         .positionShares = 0},
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
         .pnlMicrodollars = 9'900,
         .positionShares = 10},
    };
  }

  std::filesystem::path root_;
  bte::data::DataSelectionIdentity selection_;
};

TEST_F(ResultStoreFixture, schemaTwoHashMatchesIndependentLiteralOracle) {
  // Python struct.pack/hashlib independently encode the normative frames;
  // the complete worked input is recorded in Issue10CanonicalHashEvidence.md.
  ASSERT_EQ(selection_.snapshotId,
            "bf5e2533584d2e65312e8d15c03c11c10a4118bfca341ef806a04b38e9596ec6");
  auto canonical = records();
  canonical.push_back({
      .sequence = 3,
      .timestamp = timestamp("2024-01-02 00:00:00+00:00"),
      .symbol = "SYN",
      .family = bte::results::RecordFamily::portfolio,
      .cashMicrodollars = std::numeric_limits<std::int64_t>::min(),
      .marketValueMicrodollars = std::numeric_limits<std::int64_t>::max(),
      .equityMicrodollars = -1,
      .pnlMicrodollars = -256,
      .positionShares = 0,
      .text = "boundary cafe\u0301",
  });
  auto store =
      bte::results::ResultStore::open(root_ / "Oracle", root_ / "Data");
  ASSERT_TRUE(store.ok()) << store.error().message;
  for (const auto status : {bte::results::RunStatus::completed,
                            bte::results::RunStatus::interrupted}) {
    auto writer = store.value()->begin(descriptor());
    ASSERT_TRUE(writer.ok()) << writer.error().message;
    ASSERT_TRUE(writer.value()->append(canonical).ok());
    const auto summary =
        status == bte::results::RunStatus::completed
            ? bte::results::FinalSummary{.finalEquityMicrodollars =
                                             100'009'900'000,
                                         .pnlMicrodollars = -256}
            : bte::results::FinalSummary{};
    const auto expected =
        status == bte::results::RunStatus::completed
            ? "b8b436a6dd7fc99a86e40fa2ea04ebe0a3633bf303927147d8cd98f6f0b1e3d7"
            : "5d16a7ba640f51cf7b6dbcc22a1e9299ca230ce66b0c83cf4ced1d101419297"
              "8";
    const auto finalized =
        writer.value()->finalizeDraft(status, summary, "oracle");
    ASSERT_TRUE(finalized.ok()) << finalized.error().message;
    EXPECT_EQ(finalized.value().canonicalResultHash, expected);
    ASSERT_TRUE(store.value()->publishDraft(finalized.value().resultId).ok());
    const auto reopened = store.value()->openResult(finalized.value().resultId);
    ASSERT_TRUE(reopened.ok()) << reopened.error().message;
    EXPECT_EQ(reopened.value().canonicalResultHash, expected);
    EXPECT_EQ(reopened.value().records, canonical);
    EXPECT_EQ(reopened.value().summary, summary);
  }
}

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
    EXPECT_FALSE(store.value()->openResultWithData(invalidId).ok());
    EXPECT_FALSE(store.value()->moveToTrash(invalidId).ok());
    EXPECT_FALSE(store.value()->restore(invalidId).ok());
    EXPECT_FALSE(store.value()->purge(invalidId).ok());
  }
  EXPECT_FALSE(store.value()->openResult(std::string(32, '0')).ok());
  EXPECT_FALSE(store.value()->openResultWithData(std::string(32, '0')).ok());
  EXPECT_FALSE(store.value()->purge(std::string(32, '0')).ok());
  EXPECT_FALSE(store.value()->importResult({}).ok());
  EXPECT_FALSE(store.value()->importResult(root_ / "missing.bteresult").ok());
}

TEST_F(ResultStoreFixture,
       savedDisplayNameCanChangeWithoutChangingIdentityOrCanonicalHash) {
  auto store =
      bte::results::ResultStore::open(root_ / "ResultsStore", root_ / "Data");
  ASSERT_TRUE(store.ok()) << store.error().message;
  auto writer = store.value()->begin(descriptor());
  ASSERT_TRUE(writer.ok()) << writer.error().message;
  auto writerOwner = std::move(writer).value();
  ASSERT_TRUE(writerOwner->append(records()).ok());
  const auto finalized = writerOwner->finalizeDraft(
      bte::results::RunStatus::completed,
      {.finalEquityMicrodollars = 100'009'900'000, .pnlMicrodollars = 9'900});
  ASSERT_TRUE(finalized.ok()) << finalized.error().message;
  writerOwner.reset();
  const auto published = store.value()->publishDraft(finalized.value().resultId,
                                                     "AAPL May 2018 test");
  ASSERT_TRUE(published.ok()) << published.error().message;
  auto opened = store.value()->openResult(published.value().resultId);
  ASSERT_TRUE(opened.ok()) << opened.error().message;
  EXPECT_EQ(opened.value().displayName, "AAPL May 2018 test");
  EXPECT_EQ(opened.value().canonicalResultHash,
            finalized.value().canonicalResultHash);

  auto catalog = store.value()->list();
  ASSERT_TRUE(catalog.ok()) << catalog.error().message;
  ASSERT_EQ(catalog.value().size(), 1U);
  EXPECT_EQ(catalog.value().front().displayName, "AAPL May 2018 test");
  ASSERT_TRUE(store.value()
                  ->renameResult(published.value().resultId, "Updated name")
                  .ok());
  opened = store.value()->openResult(published.value().resultId);
  ASSERT_TRUE(opened.ok()) << opened.error().message;
  EXPECT_EQ(opened.value().displayName, "Updated name");
  EXPECT_EQ(opened.value().canonicalResultHash,
            finalized.value().canonicalResultHash);
  const auto repeatedSave = store.value()->publishDraft(
      published.value().resultId, "A different save-time name");
  ASSERT_TRUE(repeatedSave.ok()) << repeatedSave.error().message;
  opened = store.value()->openResult(published.value().resultId);
  ASSERT_TRUE(opened.ok()) << opened.error().message;
  EXPECT_EQ(opened.value().displayName, "Updated name");
  EXPECT_EQ(opened.value().canonicalResultHash,
            finalized.value().canonicalResultHash);
  ASSERT_TRUE(store.value()->moveToTrash(published.value().resultId).ok());
  auto trash = store.value()->listTrash();
  ASSERT_TRUE(trash.ok()) << trash.error().message;
  ASSERT_EQ(trash.value().size(), 1U);
  EXPECT_EQ(trash.value().front().displayName, "Updated name");
  ASSERT_TRUE(store.value()->restore(published.value().resultId).ok());
  opened = store.value()->openResult(published.value().resultId);
  ASSERT_TRUE(opened.ok()) << opened.error().message;
  EXPECT_EQ(opened.value().displayName, "Updated name");
  const auto maxLengthName = std::string(160, 'x');
  ASSERT_TRUE(store.value()
                  ->renameResult(published.value().resultId, maxLengthName)
                  .ok());
  EXPECT_FALSE(
      store.value()->renameResult(published.value().resultId, " ").ok());
  EXPECT_FALSE(
      store.value()
          ->renameResult(published.value().resultId, std::string(161, 'x'))
          .ok());
  EXPECT_FALSE(store.value()
                   ->renameResult(published.value().resultId, "bad\nname")
                   .ok());
  EXPECT_FALSE(
      store.value()
          ->renameResult(published.value().resultId, std::string{"\xC0\xAF", 2})
          .ok());
  for (const auto &validName : std::vector<std::string>{
           std::string{"\xC3\xA9", 2}, std::string{"\xE2\x82\xAC", 3},
           std::string{"\xF0\x9F\x98\x80", 4}}) {
    ASSERT_TRUE(store.value()
                    ->renameResult(published.value().resultId, validName)
                    .ok());
    opened = store.value()->openResult(published.value().resultId);
    ASSERT_TRUE(opened.ok()) << opened.error().message;
    EXPECT_EQ(opened.value().displayName, validName);
  }
  for (const auto &invalidName : std::vector<std::string>{
           std::string{"\x80", 1}, std::string{"\xC3", 1},
           std::string{"\xC3x", 2}, std::string{"\xE2\x82", 2},
           std::string{"\xE2\x82x", 3}, std::string{"\xE0\x80\x80", 3},
           std::string{"\xED\xA0\x80", 3}, std::string{"\xF0\x9F\x98", 3},
           std::string{"\xF0\x9F\x98x", 4}, std::string{"\xF0\x80\x80\x80", 4},
           std::string{"\xF4\x90\x80\x80", 4},
           std::string{"\xF5\x80\x80\x80", 4}, std::string{" Leading space"},
           std::string{"Trailing space "}, std::string{"bad\x7Fname", 8}}) {
    EXPECT_FALSE(store.value()
                     ->renameResult(published.value().resultId, invalidName)
                     .ok());
  }
  ASSERT_TRUE(store.value()
                  ->renameResult(published.value().resultId, maxLengthName)
                  .ok());
  opened = store.value()->openResult(published.value().resultId);
  ASSERT_TRUE(opened.ok()) << opened.error().message;
  EXPECT_EQ(opened.value().displayName, maxLengthName);
  EXPECT_EQ(opened.value().canonicalResultHash,
            finalized.value().canonicalResultHash);
}

TEST_F(ResultStoreFixture,
       openResultWithDataReturnsThePersistedExactBarsAndIdentity) {
  auto store =
      bte::results::ResultStore::open(root_ / "ResultsStore", root_ / "Data");
  ASSERT_TRUE(store.ok()) << store.error().message;
  auto writer = store.value()->begin(descriptor());
  ASSERT_TRUE(writer.ok()) << writer.error().message;
  ASSERT_TRUE(writer.value()->append(records()).ok());
  const auto finalized = writer.value()->finalizeAndPromote(
      bte::results::RunStatus::completed,
      {.finalEquityMicrodollars = 100'009'900'000, .pnlMicrodollars = 9'900});
  ASSERT_TRUE(finalized.ok()) << finalized.error().message;

  const auto opened =
      store.value()->openResultWithData(finalized.value().resultId);
  ASSERT_TRUE(opened.ok()) << opened.error().message;
  EXPECT_EQ(opened.value().result.resultId, finalized.value().resultId);
  EXPECT_EQ(opened.value().result.canonicalResultHash,
            finalized.value().canonicalResultHash);
  EXPECT_EQ(opened.value().selection.identity, selection_);
  ASSERT_EQ(opened.value().selection.bars.size(), 2U);
  EXPECT_EQ(opened.value().selection.bars[0],
            (bte::data::SnapshotBar{.timestamp =
                                        timestamp("2024-01-01 23:00:00+00:00"),
                                    .openNanodollars = 100'000'000'000,
                                    .highNanodollars = 102'000'000'000,
                                    .lowNanodollars = 99'000'000'000,
                                    .closeNanodollars = 101'000'000'000,
                                    .volumeMicroshares = 1'200'000'000}));
  EXPECT_EQ(opened.value().selection.bars[1],
            (bte::data::SnapshotBar{.timestamp =
                                        timestamp("2024-01-02 00:00:00+00:00"),
                                    .openNanodollars = 101'000'000'000,
                                    .highNanodollars = 103'000'000'000,
                                    .lowNanodollars = 100'000'000'000,
                                    .closeNanodollars = 102'000'000'000,
                                    .volumeMicroshares = 2'300'000'000}));
}

TEST_F(ResultStoreFixture,
       openResultWithDataRejectsMissingSegmentWithoutMutatingTheResult) {
  auto store =
      bte::results::ResultStore::open(root_ / "ResultsStore", root_ / "Data");
  ASSERT_TRUE(store.ok()) << store.error().message;
  auto writer = store.value()->begin(descriptor());
  ASSERT_TRUE(writer.ok()) << writer.error().message;
  ASSERT_TRUE(writer.value()->append(records()).ok());
  const auto finalized = writer.value()->finalizeAndPromote(
      bte::results::RunStatus::completed,
      {.finalEquityMicrodollars = 100'009'900'000, .pnlMicrodollars = 9'900});
  ASSERT_TRUE(finalized.ok()) << finalized.error().message;
  const auto segmentPath = root_ / "Data" / "Segments" /
                           (selection_.spans.front().segmentId + ".btedata");
  ASSERT_TRUE(std::filesystem::exists(segmentPath));
  std::filesystem::remove(segmentPath);

  const auto opened =
      store.value()->openResultWithData(finalized.value().resultId);
  ASSERT_FALSE(opened.ok());
  EXPECT_EQ(opened.error().code, bte::core::ErrorCode::dataSnapshotUnavailable);
  EXPECT_TRUE(
      std::filesystem::exists(root_ / "ResultsStore" / "Results" /
                              (finalized.value().resultId + ".bteresult")));
}

TEST_F(ResultStoreFixture,
       publishingNamedDraftWaitsForItsMainDatabaseToContainTheName) {
  auto store =
      bte::results::ResultStore::open(root_ / "ResultsStore", root_ / "Data");
  ASSERT_TRUE(store.ok()) << store.error().message;
  auto writer = store.value()->begin(descriptor());
  ASSERT_TRUE(writer.ok()) << writer.error().message;
  auto writerOwner = std::move(writer).value();
  ASSERT_TRUE(writerOwner->append(records()).ok());
  const auto finalized = writerOwner->finalizeDraft(
      bte::results::RunStatus::completed,
      {.finalEquityMicrodollars = 100'009'900'000, .pnlMicrodollars = 9'900});
  ASSERT_TRUE(finalized.ok()) << finalized.error().message;
  writerOwner.reset();

  const auto draftPath = root_ / "ResultsStore" / "Drafts" /
                         (finalized.value().resultId + ".bteresult");
  sqlite3 *rawReader = nullptr;
  ASSERT_EQ(sqlite3_open_v2(draftPath.string().c_str(), &rawReader,
                            SQLITE_OPEN_READONLY, nullptr),
            SQLITE_OK);
  auto reader = std::unique_ptr<sqlite3, decltype(&sqlite3_close)>{
      rawReader, &sqlite3_close};
  ASSERT_EQ(sqlite3_exec(reader.get(),
                         "BEGIN; SELECT count(*) FROM sqlite_master", nullptr,
                         nullptr, nullptr),
            SQLITE_OK);

  const auto blocked = store.value()->publishDraft(finalized.value().resultId,
                                                   "Concurrent reader name");
  EXPECT_FALSE(blocked.ok());
  const auto savedPath = root_ / "ResultsStore" / "Results" /
                         (finalized.value().resultId + ".bteresult");
  EXPECT_FALSE(std::filesystem::exists(savedPath));
  ASSERT_EQ(sqlite3_exec(reader.get(), "ROLLBACK", nullptr, nullptr, nullptr),
            SQLITE_OK);
  reader.reset();

  const auto published = store.value()->publishDraft(finalized.value().resultId,
                                                     "Concurrent reader name");
  ASSERT_TRUE(published.ok()) << published.error().message;
  const auto opened = store.value()->openResult(published.value().resultId);
  ASSERT_TRUE(opened.ok()) << opened.error().message;
  EXPECT_EQ(opened.value().displayName, "Concurrent reader name");
}

TEST_F(ResultStoreFixture,
       malformedDisplayMetadataIsUnavailableAndCannotBeRenamed) {
  auto store =
      bte::results::ResultStore::open(root_ / "ResultsStore", root_ / "Data");
  ASSERT_TRUE(store.ok()) << store.error().message;
  auto writer = store.value()->begin(descriptor());
  ASSERT_TRUE(writer.ok()) << writer.error().message;
  auto writerOwner = std::move(writer).value();
  ASSERT_TRUE(writerOwner->append(records()).ok());
  const auto finalized = writerOwner->finalizeDraft(
      bte::results::RunStatus::completed,
      {.finalEquityMicrodollars = 100'009'900'000, .pnlMicrodollars = 9'900});
  ASSERT_TRUE(finalized.ok()) << finalized.error().message;
  writerOwner.reset();

  const auto &id = finalized.value().resultId;
  const auto invalidName = store.value()->publishDraft(id, " Bad name");
  ASSERT_FALSE(invalidName.ok());
  EXPECT_EQ(invalidName.error().code, bte::core::ErrorCode::invalidArgument);
  const auto published = store.value()->publishDraft(id, "Valid name");
  ASSERT_TRUE(published.ok()) << published.error().message;
  const auto path = root_ / "ResultsStore" / "Results" / (id + ".bteresult");

  executeSql(path, "DELETE FROM result_display_name");
  auto opened = store.value()->openResult(id);
  ASSERT_FALSE(opened.ok());
  EXPECT_EQ(opened.error().code, bte::core::ErrorCode::schemaMismatch);
  auto catalog = store.value()->list();
  ASSERT_TRUE(catalog.ok()) << catalog.error().message;
  ASSERT_EQ(catalog.value().size(), 1U);
  EXPECT_FALSE(catalog.value().front().available);
  EXPECT_FALSE(store.value()->renameResult(id, "Replacement").ok());

  executeSql(path, "INSERT INTO result_display_name VALUES(' Bad name')");
  opened = store.value()->openResult(id);
  ASSERT_FALSE(opened.ok());
  EXPECT_EQ(opened.error().code, bte::core::ErrorCode::schemaMismatch);

  executeSql(path, "UPDATE result_display_name SET name='Valid name'; "
                   "INSERT INTO result_display_name VALUES('Extra row')");
  opened = store.value()->openResult(id);
  ASSERT_FALSE(opened.ok());
  EXPECT_EQ(opened.error().code, bte::core::ErrorCode::schemaMismatch);

  executeSql(path, "DROP TABLE result_display_name; "
                   "CREATE VIEW result_display_name AS SELECT 'Valid name' AS "
                   "name");
  opened = store.value()->openResult(id);
  ASSERT_TRUE(opened.ok()) << opened.error().message;
  EXPECT_TRUE(opened.value().displayName.empty());
  const auto renamed = store.value()->renameResult(id, "Replacement");
  EXPECT_FALSE(renamed.ok());
}

TEST_F(ResultStoreFixture, duplicateDisplayNamesNeverAliasResultIdentity) {
  auto store =
      bte::results::ResultStore::open(root_ / "ResultsStore", root_ / "Data");
  ASSERT_TRUE(store.ok()) << store.error().message;
  std::vector<std::string> ids;
  for (auto index = 0; index < 2; ++index) {
    auto writer = store.value()->begin(descriptor());
    ASSERT_TRUE(writer.ok()) << writer.error().message;
    auto writerOwner = std::move(writer).value();
    ASSERT_TRUE(writerOwner->append(records()).ok());
    const auto finalized = writerOwner->finalizeDraft(
        bte::results::RunStatus::completed,
        {.finalEquityMicrodollars = 100'009'900'000, .pnlMicrodollars = 9'900});
    ASSERT_TRUE(finalized.ok()) << finalized.error().message;
    writerOwner.reset();
    const auto published =
        store.value()->publishDraft(finalized.value().resultId, "Shared name");
    ASSERT_TRUE(published.ok()) << published.error().message;
    ids.push_back(published.value().resultId);
  }
  ASSERT_NE(ids[0], ids[1]);
  const auto catalog = store.value()->list();
  ASSERT_TRUE(catalog.ok()) << catalog.error().message;
  ASSERT_EQ(catalog.value().size(), 2U);
  for (const auto &summary : catalog.value()) {
    EXPECT_EQ(summary.displayName, "Shared name");
    EXPECT_TRUE(summary.resultId == ids[0] || summary.resultId == ids[1]);
    const auto opened = store.value()->openResult(summary.resultId);
    ASSERT_TRUE(opened.ok()) << opened.error().message;
    EXPECT_EQ(opened.value().displayName, "Shared name");
  }
}

TEST_F(ResultStoreFixture,
       preRequestedCancellationRejectsOpenListAndResultReadWithoutMutation) {
  auto store =
      bte::results::ResultStore::open(root_ / "ResultsStore", root_ / "Data");
  ASSERT_TRUE(store.ok()) << store.error().message;
  auto writer = store.value()->begin(descriptor());
  ASSERT_TRUE(writer.ok()) << writer.error().message;
  ASSERT_TRUE(writer.value()->append(records()).ok());
  const auto finalized = writer.value()->finalizeAndPromote(
      bte::results::RunStatus::completed,
      {.finalEquityMicrodollars = 100'009'900'000, .pnlMicrodollars = 9'900});
  ASSERT_TRUE(finalized.ok()) << finalized.error().message;
  const auto artifact = root_ / "ResultsStore" / "Results" /
                        (finalized.value().resultId + ".bteresult");

  bte::core::CancellationSource cancellation;
  cancellation.requestCancellation();
  const auto cancelledRoot = root_ / "CancelledStore";
  const auto cancelledOpen = bte::results::ResultStore::open(
      cancelledRoot, root_ / "Data", cancellation.token());
  ASSERT_FALSE(cancelledOpen.ok());
  EXPECT_EQ(cancelledOpen.error().code, bte::core::ErrorCode::cancelled);
  EXPECT_FALSE(std::filesystem::exists(cancelledRoot));

  const auto cancelledList = store.value()->list(cancellation.token());
  ASSERT_FALSE(cancelledList.ok());
  EXPECT_EQ(cancelledList.error().code, bte::core::ErrorCode::cancelled);
  const auto cancelledResult = store.value()->openResult(
      finalized.value().resultId, cancellation.token());
  ASSERT_FALSE(cancelledResult.ok());
  EXPECT_EQ(cancelledResult.error().code, bte::core::ErrorCode::cancelled);
  const auto cancelledResultWithData = store.value()->openResultWithData(
      finalized.value().resultId, cancellation.token());
  ASSERT_FALSE(cancelledResultWithData.ok());
  EXPECT_EQ(cancelledResultWithData.error().code,
            bte::core::ErrorCode::cancelled);
  EXPECT_TRUE(std::filesystem::exists(artifact));
}

TEST_F(ResultStoreFixture,
       defaultLogRecordRoundTripsItsCanonicalSequenceFamilyAndSide) {
  auto store =
      bte::results::ResultStore::open(root_ / "ResultsStore", root_ / "Data");
  ASSERT_TRUE(store.ok()) << store.error().message;
  auto writer = store.value()->begin(descriptor());
  ASSERT_TRUE(writer.ok()) << writer.error().message;

  bte::results::CanonicalRecord record;
  record.timestamp = timestamp("2024-01-01 23:00:00+00:00");
  record.symbol = "SYN";
  record.text = "strategy diagnostic";
  ASSERT_TRUE(writer.value()->append({record}).ok());
  const auto finalized = writer.value()->finalizeAndPromote(
      bte::results::RunStatus::failed, {}, "strategy failed");
  ASSERT_TRUE(finalized.ok()) << finalized.error().message;

  const auto opened = store.value()->openResult(finalized.value().resultId);
  ASSERT_TRUE(opened.ok()) << opened.error().message;
  ASSERT_EQ(opened.value().records.size(), 1U);
  EXPECT_EQ(opened.value().records.front().sequence, 0U);
  EXPECT_EQ(opened.value().records.front().family,
            bte::results::RecordFamily::log);
  EXPECT_EQ(opened.value().records.front().side, bte::results::OrderSide::none);
  EXPECT_EQ(opened.value().records.front().text, "strategy diagnostic");
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

TEST_F(ResultStoreFixture,
       descriptorAcceptsSelectionBeginningAtLaterManifestSegment) {
  auto store =
      bte::results::ResultStore::open(root_ / "LaterStore", root_ / "Data");
  ASSERT_TRUE(store.ok()) << store.error().message;
  auto later = descriptor();
  ASSERT_GT(later.dataSelection.spans.size(), 1U);
  later.dataSelection.spans.erase(later.dataSelection.spans.begin());
  ASSERT_EQ(later.dataSelection.spans.front().segmentOrdinal, 1U);

  auto writer = store.value()->begin(later);
  ASSERT_TRUE(writer.ok()) << writer.error().message;
}

TEST_F(ResultStoreFixture,
       beginRejectsStructurallyValidSelectionThatDoesNotMatchSnapshotRows) {
  auto store =
      bte::results::ResultStore::open(root_ / "ExactStore", root_ / "Data");
  ASSERT_TRUE(store.ok()) << store.error().message;
  auto mismatched = descriptor();
  ++mismatched.dataSelection.spans.front().firstRow;

  const auto writer = store.value()->begin(mismatched);

  ASSERT_FALSE(writer.ok());
  EXPECT_EQ(writer.error().code, bte::core::ErrorCode::dataSnapshotUnavailable);
  EXPECT_TRUE(std::filesystem::is_empty(root_ / "ExactStore" / "Staging"));
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

TEST_F(ResultStoreFixture,
       writerRejectsInvalidCanonicalEnumsSymbolsRangesAndPayloads) {
  auto store =
      bte::results::ResultStore::open(root_ / "ValidatedStore", root_ / "Data");
  ASSERT_TRUE(store.ok());
  auto writer = store.value()->begin(descriptor());
  ASSERT_TRUE(writer.ok());
  const auto valid = bte::results::CanonicalRecord{
      .timestamp = timestamp("2024-01-01 23:00:00+00:00"),
      .symbol = "SYN",
      .family = bte::results::RecordFamily::log,
      .text = "valid diagnostic",
  };
  std::vector<bte::results::CanonicalRecord> invalid;
  auto add = [&](const auto &mutate) {
    auto candidate = valid;
    mutate(candidate);
    invalid.push_back(std::move(candidate));
  };
  add([](auto &record) {
    record.family = static_cast<bte::results::RecordFamily>(99);
  });
  add([](auto &record) {
    record.side = static_cast<bte::results::OrderSide>(99);
  });
  add([](auto &record) { record.symbol = "OTHER"; });
  add([&](auto &record) {
    record.timestamp = timestamp("2024-01-02 01:00:00+00:00");
  });
  add([](auto &record) {
    record.family = bte::results::RecordFamily::fill;
    record.text.clear();
  });
  add([](auto &record) {
    record.family = bte::results::RecordFamily::portfolio;
    record.text.clear();
  });
  add([](auto &record) {
    record.family = bte::results::RecordFamily::order;
    record.text.clear();
  });
  add([](auto &record) {
    record.family = bte::results::RecordFamily::cost;
    record.text.clear();
  });
  add([](auto &record) { record.side = bte::results::OrderSide::buy; });
  for (const auto &record : invalid) {
    const auto appended = writer.value()->append({record});
    ASSERT_FALSE(appended.ok());
    EXPECT_EQ(appended.error().code, bte::core::ErrorCode::invalidArgument);
  }
  ASSERT_TRUE(writer.value()->append({valid}).ok());

  const auto invalidStatus = writer.value()->finalizeAndPromote(
      static_cast<bte::results::RunStatus>(99), {});
  ASSERT_FALSE(invalidStatus.ok());
  EXPECT_EQ(invalidStatus.error().code, bte::core::ErrorCode::invalidArgument);
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
      "UPDATE result_capabilities SET orders=2",
      "UPDATE result_capabilities SET orders=0",
      "INSERT INTO result_capabilities SELECT * FROM result_capabilities",
      "DELETE FROM run_configuration",
      "DELETE FROM data_selection",
      "UPDATE data_spans SET row_count=0",
      "UPDATE canonical_records SET sequence=7 WHERE sequence=1",
      "UPDATE canonical_records SET timestamp_ms=0 WHERE sequence=1",
      "UPDATE canonical_records SET family=0,side=0,quantity=NULL WHERE "
      "sequence=0",
      "UPDATE canonical_records SET pnl=123 WHERE sequence=2",
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
      "DROP TABLE result_capabilities",
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
       terminalRecordingRemainsDraftUntilExplicitPublication) {
  const auto storeRoot = root_ / "ExplicitPublication";
  auto store = bte::results::ResultStore::open(storeRoot, root_ / "Data");
  ASSERT_TRUE(store.ok());
  auto writer = store.value()->begin(descriptor());
  ASSERT_TRUE(writer.ok());
  const auto resultId = writer.value()->resultId();
  ASSERT_TRUE(writer.value()->append(records()).ok());

  const auto finalized = writer.value()->finalizeDraft(
      bte::results::RunStatus::completed,
      {.finalEquityMicrodollars = 11'000'000, .pnlMicrodollars = 1'000'000});

  ASSERT_TRUE(finalized.ok());
  const auto listed = store.value()->list();
  ASSERT_TRUE(listed.ok());
  EXPECT_TRUE(listed.value().empty());
  EXPECT_TRUE(std::filesystem::exists(storeRoot / "Drafts" /
                                      (resultId + ".bteresult")));
  EXPECT_FALSE(std::filesystem::exists(storeRoot / "Results" /
                                       (resultId + ".bteresult")));

  const auto drafts = store.value()->listDrafts();
  ASSERT_TRUE(drafts.ok());
  ASSERT_EQ(drafts.value().size(), 1U);
  EXPECT_EQ(drafts.value().front().resultId, resultId);
  EXPECT_EQ(drafts.value().front().status, bte::results::RunStatus::completed);
  EXPECT_TRUE(drafts.value().front().available);
}

TEST_F(ResultStoreFixture, openingStoreDoesNotRecoverAnActiveWriter) {
  const auto storeRoot = root_ / "ActiveWriter";
  auto store = bte::results::ResultStore::open(storeRoot, root_ / "Data");
  ASSERT_TRUE(store.ok());
  auto writer = store.value()->begin(descriptor());
  ASSERT_TRUE(writer.ok());
  const auto resultId = writer.value()->resultId();
  ASSERT_TRUE(writer.value()->append(records()).ok());

  auto observer = bte::results::ResultStore::open(storeRoot, root_ / "Data");
  ASSERT_TRUE(observer.ok()) << observer.error().message;
  auto drafts = observer.value()->listDrafts();
  ASSERT_TRUE(drafts.ok()) << drafts.error().message;
  EXPECT_TRUE(drafts.value().empty());
  EXPECT_TRUE(std::filesystem::exists(storeRoot / "Staging" /
                                      (resultId + ".bteresult")));

  auto finalized = writer.value()->finalizeDraft(
      bte::results::RunStatus::completed,
      {.finalEquityMicrodollars = 11'000'000, .pnlMicrodollars = 1'000'000});
  ASSERT_TRUE(finalized.ok()) << finalized.error().message;
  drafts = observer.value()->listDrafts();
  ASSERT_TRUE(drafts.ok());
  ASSERT_EQ(drafts.value().size(), 1U);
  EXPECT_EQ(drafts.value().front().resultId, resultId);
}

TEST_F(ResultStoreFixture,
       explicitDraftDiscardRemovesOnlyDraftAndReleasesRetention) {
  const auto storeRoot = root_ / "ExplicitDiscard";
  auto store = bte::results::ResultStore::open(storeRoot, root_ / "Data");
  ASSERT_TRUE(store.ok());
  auto writer = store.value()->begin(descriptor());
  ASSERT_TRUE(writer.ok());
  const auto resultId = writer.value()->resultId();
  ASSERT_TRUE(writer.value()->append(records()).ok());
  ASSERT_TRUE(writer.value()
                  ->finalizeDraft(bte::results::RunStatus::completed,
                                  {.finalEquityMicrodollars = 11'000'000,
                                   .pnlMicrodollars = 1'000'000})
                  .ok());

  const auto discarded = store.value()->discardDraft(resultId);

  ASSERT_TRUE(discarded.ok()) << discarded.error().message;
  EXPECT_FALSE(std::filesystem::exists(storeRoot / "Drafts" /
                                       (resultId + ".bteresult")));
  const auto drafts = store.value()->listDrafts();
  ASSERT_TRUE(drafts.ok());
  EXPECT_TRUE(drafts.value().empty());
  const auto published = store.value()->publishDraft(resultId);
  ASSERT_FALSE(published.ok());
  EXPECT_EQ(published.error().code, bte::core::ErrorCode::notFound);

  for (const auto &span : selection_.spans) {
    EXPECT_EQ(queryInteger(root_ / "Data" / "References.sqlite3",
                           "SELECT COUNT(*) FROM segment_references WHERE "
                           "segment_id = '" +
                               span.segmentId + "'"),
              0);
  }
}

TEST_F(ResultStoreFixture,
       explicitPublicationIsIdempotentAndPreservesDraftIdentity) {
  const auto storeRoot = root_ / "IdempotentPublication";
  auto store = bte::results::ResultStore::open(storeRoot, root_ / "Data");
  ASSERT_TRUE(store.ok());
  auto writer = store.value()->begin(descriptor());
  ASSERT_TRUE(writer.ok());
  const auto resultId = writer.value()->resultId();
  ASSERT_TRUE(writer.value()->append(records()).ok());
  const auto draft = writer.value()->finalizeDraft(
      bte::results::RunStatus::completed,
      {.finalEquityMicrodollars = 11'000'000, .pnlMicrodollars = 1'000'000});
  ASSERT_TRUE(draft.ok());

  const auto first = store.value()->publishDraft(resultId);
  const auto second = store.value()->publishDraft(resultId);

  ASSERT_TRUE(first.ok());
  ASSERT_TRUE(second.ok());
  EXPECT_EQ(first.value().resultId, resultId);
  EXPECT_EQ(first.value().canonicalResultHash,
            draft.value().canonicalResultHash);
  EXPECT_EQ(second.value().resultId, first.value().resultId);
  EXPECT_EQ(second.value().canonicalResultHash,
            first.value().canonicalResultHash);
  EXPECT_FALSE(std::filesystem::exists(storeRoot / "Drafts" /
                                       (resultId + ".bteresult")));
  EXPECT_TRUE(std::filesystem::exists(storeRoot / "Results" /
                                      (resultId + ".bteresult")));
  const auto listed = store.value()->list();
  ASSERT_TRUE(listed.ok());
  ASSERT_EQ(listed.value().size(), 1U);
  EXPECT_EQ(listed.value().front().resultId, resultId);
}

TEST_F(ResultStoreFixture,
       failedExplicitPublicationKeepsDraftRetryableAndCatalogHidden) {
  for (const auto point : {
           bte::results::testing::FailurePoint::catalogVisibility,
           bte::results::testing::FailurePoint::promotion,
       }) {
    const auto storeRoot = root_ / ("RetryablePublication-" +
                                    std::to_string(static_cast<int>(point)));
    auto store = bte::results::ResultStore::open(storeRoot, root_ / "Data");
    ASSERT_TRUE(store.ok());
    auto writer = store.value()->begin(descriptor());
    ASSERT_TRUE(writer.ok());
    const auto resultId = writer.value()->resultId();
    ASSERT_TRUE(writer.value()->append(records()).ok());
    ASSERT_TRUE(writer.value()
                    ->finalizeDraft(bte::results::RunStatus::completed,
                                    {.finalEquityMicrodollars = 11'000'000,
                                     .pnlMicrodollars = 1'000'000})
                    .ok());

    bte::results::testing::failNext(point);
    const auto failed = store.value()->publishDraft(resultId);
    bte::results::testing::clearFailure();

    ASSERT_FALSE(failed.ok());
    EXPECT_TRUE(std::filesystem::exists(storeRoot / "Drafts" /
                                        (resultId + ".bteresult")));
    const auto hidden = store.value()->list();
    ASSERT_TRUE(hidden.ok());
    EXPECT_TRUE(hidden.value().empty());
    const auto retried = store.value()->publishDraft(resultId);
    ASSERT_TRUE(retried.ok()) << retried.error().message;
    EXPECT_EQ(retried.value().resultId, resultId);
  }
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
       canonicalHashIgnoresSaveMetadataPathPageLayoutAndCatalogOrder) {
  auto store =
      bte::results::ResultStore::open(root_ / "ResultsStore", root_ / "Data");
  ASSERT_TRUE(store.ok()) << store.error().message;
  auto writer = store.value()->begin(descriptor());
  ASSERT_TRUE(writer.ok()) << writer.error().message;
  ASSERT_TRUE(writer.value()->append(records()).ok());
  const auto original = writer.value()->finalizeAndPromote(
      bte::results::RunStatus::completed,
      {.finalEquityMicrodollars = 100'009'900'000, .pnlMicrodollars = 9'900});
  ASSERT_TRUE(original.ok()) << original.error().message;
  const auto originalPath = root_ / "ResultsStore" / "Results" /
                            (original.value().resultId + ".bteresult");
  executeSql(originalPath, "UPDATE result_meta SET saved_utc_ms=10");

  auto unrelatedDescriptor = descriptor();
  unrelatedDescriptor.strategyId = "unrelated";
  auto unrelatedWriter = store.value()->begin(unrelatedDescriptor);
  ASSERT_TRUE(unrelatedWriter.ok()) << unrelatedWriter.error().message;
  ASSERT_TRUE(unrelatedWriter.value()->append(records()).ok());
  const auto unrelated = unrelatedWriter.value()->finalizeAndPromote(
      bte::results::RunStatus::completed,
      {.finalEquityMicrodollars = 100'009'900'000, .pnlMicrodollars = 9'900});
  ASSERT_TRUE(unrelated.ok()) << unrelated.error().message;
  ASSERT_NE(unrelated.value().canonicalResultHash,
            original.value().canonicalResultHash);
  const auto unrelatedPath = root_ / "ResultsStore" / "Results" /
                             (unrelated.value().resultId + ".bteresult");
  executeSql(unrelatedPath, "UPDATE result_meta SET saved_utc_ms=20");

  const auto relocatedDirectory = root_ / "Elsewhere" / "Nested";
  std::filesystem::create_directories(relocatedDirectory);
  const auto relocated = relocatedDirectory / "renamed-result.sqlite";
  ASSERT_NE(queryInteger(originalPath, "PRAGMA page_size"), 8'192);
  ASSERT_TRUE(std::filesystem::copy_file(originalPath, relocated));
  executeSql(relocated,
             "PRAGMA journal_mode=DELETE; PRAGMA page_size=8192; VACUUM; "
             "UPDATE result_meta SET created_utc_ms=17,saved_utc_ms=23");
  ASSERT_EQ(queryInteger(relocated, "PRAGMA page_size"), 8'192);
  ASSERT_EQ(
      queryInteger(relocated, "SELECT saved_utc_ms FROM result_meta LIMIT 1"),
      23);

  const auto imported = store.value()->importResult(relocated);
  ASSERT_TRUE(imported.ok()) << imported.error().message;
  EXPECT_EQ(imported.value().canonicalResultHash,
            original.value().canonicalResultHash);
  const auto importedPath = root_ / "ResultsStore" / "Results" /
                            (imported.value().resultId + ".bteresult");
  executeSql(importedPath, "UPDATE result_meta SET saved_utc_ms=30");
  auto opened = store.value()->openResult(imported.value().resultId);
  ASSERT_TRUE(opened.ok()) << opened.error().message;
  EXPECT_EQ(opened.value().descriptor, descriptor());
  EXPECT_EQ(opened.value().records, records());

  const auto catalog = store.value()->list();
  ASSERT_TRUE(catalog.ok()) << catalog.error().message;
  ASSERT_EQ(catalog.value().size(), 3);
  EXPECT_EQ(catalog.value()[0].resultId, imported.value().resultId);
  EXPECT_EQ(catalog.value()[0].savedUtcMillis, 30);
  EXPECT_EQ(catalog.value()[1].resultId, unrelated.value().resultId);
  EXPECT_EQ(catalog.value()[1].savedUtcMillis, 20);
  EXPECT_EQ(catalog.value()[2].resultId, original.value().resultId);
  EXPECT_EQ(catalog.value()[2].savedUtcMillis, 10);
  const auto matchingHashes =
      std::ranges::count_if(catalog.value(), [&](const auto &summary) {
        return summary.canonicalResultHash ==
               original.value().canonicalResultHash;
      });
  EXPECT_EQ(matchingHashes, 2);
  EXPECT_EQ(catalog.value()[0].canonicalResultHash,
            catalog.value()[2].canonicalResultHash);
}

TEST_F(ResultStoreFixture,
       allocatorCollisionsRetryWithoutOverwritingBeginOrImportArtifacts) {
  constexpr auto forcedId =
      std::string_view{"11111111111111111111111111111111"};
  bte::results::testing::forceNextResultId(std::string{forcedId});
  auto store =
      bte::results::ResultStore::open(root_ / "ResultsStore", root_ / "Data");
  ASSERT_TRUE(store.ok()) << store.error().message;
  auto originalWriter = store.value()->begin(descriptor());
  ASSERT_TRUE(originalWriter.ok()) << originalWriter.error().message;
  ASSERT_EQ(originalWriter.value()->resultId(), forcedId);
  ASSERT_TRUE(originalWriter.value()->append(records()).ok());
  const auto original = originalWriter.value()->finalizeAndPromote(
      bte::results::RunStatus::completed,
      {.finalEquityMicrodollars = 100'009'900'000, .pnlMicrodollars = 9'900});
  ASSERT_TRUE(original.ok()) << original.error().message;
  const auto originalPath = root_ / "ResultsStore" / "Results" /
                            (original.value().resultId + ".bteresult");

  bte::results::testing::forceNextResultId(std::string{forcedId});
  auto retryWriter = store.value()->begin(descriptor());
  ASSERT_TRUE(retryWriter.ok()) << retryWriter.error().message;
  EXPECT_NE(retryWriter.value()->resultId(), forcedId);
  ASSERT_TRUE(retryWriter.value()->append(records()).ok());
  const auto retry = retryWriter.value()->finalizeAndPromote(
      bte::results::RunStatus::completed,
      {.finalEquityMicrodollars = 100'009'900'000, .pnlMicrodollars = 9'900});
  ASSERT_TRUE(retry.ok()) << retry.error().message;

  bte::results::testing::forceNextResultId(std::string{forcedId});
  const auto imported = store.value()->importResult(originalPath);
  ASSERT_TRUE(imported.ok()) << imported.error().message;
  EXPECT_NE(imported.value().resultId, forcedId);
  EXPECT_EQ(imported.value().canonicalResultHash,
            original.value().canonicalResultHash);

  const auto reopenedOriginal =
      store.value()->openResult(std::string{forcedId});
  ASSERT_TRUE(reopenedOriginal.ok()) << reopenedOriginal.error().message;
  EXPECT_EQ(reopenedOriginal.value().records, records());
  EXPECT_EQ(reopenedOriginal.value().canonicalResultHash,
            original.value().canonicalResultHash);
}

TEST_F(ResultStoreFixture,
       allocatorSkipsQuarantinedIdentityForBeginAndImport) {
  constexpr auto forcedId =
      std::string_view{"22222222222222222222222222222222"};
  auto sourceStore =
      bte::results::ResultStore::open(root_ / "SourceStore", root_ / "Data");
  ASSERT_TRUE(sourceStore.ok());
  auto sourceWriter = sourceStore.value()->begin(descriptor());
  ASSERT_TRUE(sourceWriter.ok());
  ASSERT_TRUE(sourceWriter.value()->append(records()).ok());
  auto source = sourceWriter.value()->finalizeAndPromote(
      bte::results::RunStatus::completed,
      {.finalEquityMicrodollars = 100'009'900'000, .pnlMicrodollars = 9'900});
  ASSERT_TRUE(source.ok());
  const auto sourcePath = root_ / "SourceStore" / "Results" /
                          (source.value().resultId + ".bteresult");

  const auto destinationRoot = root_ / "DestinationStore";
  const auto quarantinedPath = destinationRoot / "Quarantine" /
                               (std::string{forcedId} + ".bteresult");
  std::filesystem::create_directories(quarantinedPath.parent_path());
  {
    std::ofstream quarantined{quarantinedPath};
    quarantined << "quarantined artifact";
  }

  auto destination =
      bte::results::ResultStore::open(destinationRoot, root_ / "Data");
  ASSERT_TRUE(destination.ok());
  bte::results::testing::forceNextResultId(std::string{forcedId});
  auto writer = destination.value()->begin(descriptor());
  ASSERT_TRUE(writer.ok()) << writer.error().message;
  EXPECT_NE(writer.value()->resultId(), forcedId);

  bte::results::testing::forceNextResultId(std::string{forcedId});
  auto imported = destination.value()->importResult(sourcePath);
  ASSERT_TRUE(imported.ok()) << imported.error().message;
  EXPECT_NE(imported.value().resultId, forcedId);
  std::ifstream quarantined{quarantinedPath};
  std::string content;
  std::getline(quarantined, content);
  EXPECT_EQ(content, "quarantined artifact");
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
       restoringMissingTrashArtifactDoesNotCreateAnEmptyDatabase) {
  auto store =
      bte::results::ResultStore::open(root_ / "ResultsStore", root_ / "Data");
  ASSERT_TRUE(store.ok());
  auto writer = store.value()->begin(descriptor());
  ASSERT_TRUE(writer.ok());
  ASSERT_TRUE(writer.value()->append(records()).ok());
  auto finalized = writer.value()->finalizeAndPromote(
      bte::results::RunStatus::completed,
      {.finalEquityMicrodollars = 100'009'900'000, .pnlMicrodollars = 9'900});
  ASSERT_TRUE(finalized.ok());
  const auto trashPath = root_ / "ResultsStore" / "Trash" /
                         (finalized.value().resultId + ".bteresult");

  const auto restored = store.value()->restore(finalized.value().resultId);
  EXPECT_FALSE(restored.ok());
  EXPECT_FALSE(std::filesystem::exists(trashPath));
  EXPECT_TRUE(store.value()->openResult(finalized.value().resultId).ok());
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
       restartRecoversCompleteAndZeroBarPrefixesAsInterruptedDrafts) {
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
  const auto active = recovered.value()->list();
  ASSERT_TRUE(active.ok());
  EXPECT_TRUE(active.value().empty());
  EXPECT_TRUE(std::filesystem::exists(root_ / "ResultsStore" / "Drafts" /
                                      (recoverableId + ".bteresult")));
  ASSERT_TRUE(recovered.value()->publishDraft(recoverableId).ok());
  auto opened = recovered.value()->openResult(recoverableId);
  ASSERT_TRUE(opened.ok()) << opened.error().message;
  EXPECT_EQ(opened.value().status, bte::results::RunStatus::interrupted);
  EXPECT_EQ(opened.value().records, records());
  EXPECT_FALSE(opened.value().summary.finalEquityMicrodollars.has_value());
  EXPECT_TRUE(std::filesystem::exists(root_ / "ResultsStore" / "Drafts" /
                                      (emptyId + ".bteresult")));
  ASSERT_TRUE(recovered.value()->publishDraft(emptyId).ok());
  auto empty = recovered.value()->openResult(emptyId);
  ASSERT_TRUE(empty.ok());
  EXPECT_EQ(empty.value().status, bte::results::RunStatus::interrupted);
  EXPECT_TRUE(empty.value().records.empty());
  EXPECT_FALSE(empty.value().summary.finalEquityMicrodollars.has_value());
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
      const auto visibleBeforeRecovery = store.value()->list();
      ASSERT_TRUE(visibleBeforeRecovery.ok());
      EXPECT_TRUE(visibleBeforeRecovery.value().empty())
          << "failed finalization must not publish at point "
          << static_cast<int>(point);
    }
    bte::results::testing::clearFailure();

    auto recovered = bte::results::ResultStore::open(storeRoot, root_ / "Data");
    ASSERT_TRUE(recovered.ok()) << recovered.error().message;
    ASSERT_TRUE(recovered.value()->publishDraft(resultId).ok());
    auto opened = recovered.value()->openResult(resultId);
    ASSERT_TRUE(opened.ok()) << opened.error().message;
    EXPECT_EQ(opened.value().status, bte::results::RunStatus::completed);
    EXPECT_EQ(opened.value().records, records());
  }
}

TEST_F(ResultStoreFixture,
       injectedSchemaCreationIsPreservedWithRecoveryNotice) {
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
  EXPECT_FALSE(std::filesystem::is_empty(root_ / "ResultsStore" / "Staging"));
  EXPECT_TRUE(std::filesystem::is_empty(root_ / "ResultsStore" / "Quarantine"));
  ASSERT_EQ(store.value()->recoveryNotices().size(), 1U);
  EXPECT_EQ(store.value()->recoveryNotices().front().reason,
            bte::results::ResultRecoveryReason::invalidPrefix);
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
      {FailurePoint::databaseOpen, 4},
      {FailurePoint::sqlExecution, 6},
      {FailurePoint::statementPreparation, 17},
      {FailurePoint::textBinding, 2},
      {FailurePoint::integerBinding, 2},
      {FailurePoint::statementExecution, 1},
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
      auto recovered =
          bte::results::ResultStore::open(storeRoot, root_ / "Data");
      EXPECT_FALSE(bte::results::testing::failurePending()) << suffix;
      if (!recovered.ok()) {
        EXPECT_TRUE(recovered.error().code == bte::core::ErrorCode::internal ||
                    recovered.error().code ==
                        bte::core::ErrorCode::schemaMismatch)
            << suffix;
        recovered = bte::results::ResultStore::open(storeRoot, root_ / "Data");
        ASSERT_TRUE(recovered.ok()) << suffix;
      }
      if (!recovered.value()->recoveryNotices().empty()) {
        EXPECT_TRUE(std::filesystem::exists(storeRoot / "Staging"));
        recovered = bte::results::ResultStore::open(storeRoot, root_ / "Data");
        ASSERT_TRUE(recovered.ok()) << suffix;
        EXPECT_TRUE(recovered.value()->recoveryNotices().empty()) << suffix;
      }
      const auto resultArtifactCount = [](const std::filesystem::path &path) {
        return std::ranges::count_if(
            std::filesystem::directory_iterator{path}, [](const auto &entry) {
              return entry.path().extension() == ".bteresult";
            });
      };
      EXPECT_EQ(resultArtifactCount(storeRoot / "Staging"), 0) << suffix;
      EXPECT_EQ(resultArtifactCount(storeRoot / "Drafts") +
                    resultArtifactCount(storeRoot / "Quarantine"),
                1)
          << suffix;
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
       recoveryPreservesArtifactsAndReportsRetentionFaults) {
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
  auto zeroRecovered =
      bte::results::ResultStore::open(quarantineRoot, root_ / "Data");
  ASSERT_TRUE(zeroRecovered.ok());
  EXPECT_TRUE(std::filesystem::exists(quarantineRoot / "Drafts" /
                                      (emptyId + ".bteresult")));
  EXPECT_TRUE(std::filesystem::exists(collision / "child"));

  const auto promotionRoot = root_ / "RecoveryPromotionFault";
  const auto promotionId = prepareInterrupted(promotionRoot, true);
  std::filesystem::remove_all(promotionRoot / "Drafts");
  std::ofstream blocked{promotionRoot / "Drafts"};
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
    auto preserved = bte::results::ResultStore::open(storeRoot, root_ / "Data");
    ASSERT_TRUE(preserved.ok());
    bte::data::testing::clearRetentionFailure();
    ASSERT_EQ(preserved.value()->recoveryNotices().size(), 1U);
    EXPECT_EQ(preserved.value()->recoveryNotices().front().reason,
              bte::results::ResultRecoveryReason::missingReferences);
    EXPECT_TRUE(std::filesystem::exists(storeRoot / "Staging" /
                                        (resultId + ".bteresult")));
    EXPECT_FALSE(std::filesystem::exists(storeRoot / "Drafts" /
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
  const auto failedPurge =
      sourceStore.value()->purge(finalized.value().resultId);
  EXPECT_FALSE(failedPurge.ok());
  bte::data::testing::clearRetentionFailure();
  EXPECT_TRUE(
      std::filesystem::exists(root_ / "SourceStore" / "Trash" /
                              (finalized.value().resultId + ".bteresult")));
  const auto stillPinned =
      bte::data::SegmentRetentionStore::open(root_ / "Data");
  ASSERT_TRUE(stillPinned.ok()) << stillPinned.error().message;
  std::vector<std::string> segmentIds;
  for (const auto &span : descriptor().dataSelection.spans) {
    segmentIds.push_back(span.segmentId);
  }
  const auto reacquired =
      stillPinned.value()->acquire(finalized.value().resultId, segmentIds);
  ASSERT_TRUE(reacquired.ok()) << reacquired.error().message;
  EXPECT_EQ(reacquired.value(), 0U);
}

TEST_F(ResultStoreFixture, openRollsBackAnInterruptedPurge) {
  const auto storeRoot = root_ / "InterruptedPurge";
  auto store = bte::results::ResultStore::open(storeRoot, root_ / "Data");
  ASSERT_TRUE(store.ok());
  auto writer = store.value()->begin(descriptor());
  ASSERT_TRUE(writer.ok());
  ASSERT_TRUE(writer.value()->append(records()).ok());
  auto finalized = writer.value()->finalizeAndPromote(
      bte::results::RunStatus::completed,
      {.finalEquityMicrodollars = 100'009'900'000, .pnlMicrodollars = 9'900});
  ASSERT_TRUE(finalized.ok());
  ASSERT_TRUE(store.value()->moveToTrash(finalized.value().resultId).ok());

  const auto trashPath =
      storeRoot / "Trash" / (finalized.value().resultId + ".bteresult");
  const auto purgingPath = storeRoot / "Staging" /
                           (finalized.value().resultId + ".purging.bteresult");
  std::filesystem::rename(trashPath, purgingPath);
  auto retention = bte::data::SegmentRetentionStore::open(root_ / "Data");
  ASSERT_TRUE(retention.ok());
  ASSERT_TRUE(retention.value()->release(finalized.value().resultId).ok());

  auto recovered = bte::results::ResultStore::open(storeRoot, root_ / "Data");
  ASSERT_TRUE(recovered.ok()) << recovered.error().message;
  EXPECT_TRUE(std::filesystem::exists(trashPath));
  EXPECT_FALSE(std::filesystem::exists(purgingPath));
  EXPECT_FALSE(std::filesystem::exists(
      storeRoot / "Results" / (finalized.value().resultId + ".bteresult")));

  std::vector<std::string> segmentIds;
  for (const auto &span : descriptor().dataSelection.spans) {
    segmentIds.push_back(span.segmentId);
    EXPECT_TRUE(std::filesystem::exists(root_ / "Data" / "Segments" /
                                        (span.segmentId + ".btedata")));
  }
  auto activeRetention = bte::data::SegmentRetentionStore::open(root_ / "Data");
  ASSERT_TRUE(activeRetention.ok());
  const auto reacquired =
      activeRetention.value()->acquire(finalized.value().resultId, segmentIds);
  ASSERT_TRUE(reacquired.ok());
  EXPECT_EQ(reacquired.value(), 0U);
}

TEST_F(ResultStoreFixture, openRollsBackAnInterruptedDraftDiscard) {
  const auto storeRoot = root_ / "InterruptedDraftDiscard";
  auto store = bte::results::ResultStore::open(storeRoot, root_ / "Data");
  ASSERT_TRUE(store.ok());
  auto writer = store.value()->begin(descriptor());
  ASSERT_TRUE(writer.ok());
  ASSERT_TRUE(writer.value()->append(records()).ok());
  auto finalized = writer.value()->finalizeDraft(
      bte::results::RunStatus::completed,
      {.finalEquityMicrodollars = 100'009'900'000, .pnlMicrodollars = 9'900});
  ASSERT_TRUE(finalized.ok());

  const auto draftPath =
      storeRoot / "Drafts" / (finalized.value().resultId + ".bteresult");
  const auto discardingPath =
      storeRoot / "Staging" /
      (finalized.value().resultId + ".discarding.bteresult");
  std::filesystem::rename(draftPath, discardingPath);
  auto retention = bte::data::SegmentRetentionStore::open(root_ / "Data");
  ASSERT_TRUE(retention.ok());
  ASSERT_TRUE(retention.value()->release(finalized.value().resultId).ok());

  auto recovered = bte::results::ResultStore::open(storeRoot, root_ / "Data");
  ASSERT_TRUE(recovered.ok()) << recovered.error().message;
  EXPECT_TRUE(std::filesystem::exists(draftPath));
  EXPECT_FALSE(std::filesystem::exists(discardingPath));
  const auto drafts = recovered.value()->listDrafts();
  ASSERT_TRUE(drafts.ok());
  ASSERT_EQ(drafts.value().size(), 1U);
  EXPECT_EQ(drafts.value().front().resultId, finalized.value().resultId);

  std::vector<std::string> segmentIds;
  for (const auto &span : descriptor().dataSelection.spans) {
    segmentIds.push_back(span.segmentId);
  }
  auto activeRetention = bte::data::SegmentRetentionStore::open(root_ / "Data");
  ASSERT_TRUE(activeRetention.ok());
  const auto reacquired =
      activeRetention.value()->acquire(finalized.value().resultId, segmentIds);
  ASSERT_TRUE(reacquired.ok());
  EXPECT_EQ(reacquired.value(), 0U);
}

TEST_F(ResultStoreFixture, draftListingAndActionsKeepCorruptArtifactsIsolated) {
  const auto storeRoot = root_ / "CorruptDrafts";
  auto store = bte::results::ResultStore::open(storeRoot, root_ / "Data");
  ASSERT_TRUE(store.ok());
  const auto corruptId = std::string(32, 'a');
  const auto corruptPath = storeRoot / "Drafts" / (corruptId + ".bteresult");
  std::ofstream corrupt{corruptPath};
  corrupt << "not a Result database";
  corrupt.close();
  std::ofstream ignored{storeRoot / "Drafts" / "ignore.txt"};
  ignored << "not a Result artifact";
  ignored.close();

  const auto listed = store.value()->listDrafts();
  ASSERT_TRUE(listed.ok()) << listed.error().message;
  ASSERT_EQ(listed.value().size(), 1U);
  EXPECT_EQ(listed.value().front().resultId, corruptId);
  EXPECT_FALSE(listed.value().front().available);
  EXPECT_FALSE(listed.value().front().unavailableReason.empty());
  EXPECT_FALSE(store.value()->publishDraft(corruptId).ok());
  EXPECT_FALSE(store.value()->discardDraft(corruptId).ok());
  EXPECT_TRUE(std::filesystem::exists(corruptPath));

  EXPECT_EQ(store.value()->publishDraft("invalid").error().code,
            bte::core::ErrorCode::invalidArgument);
  EXPECT_EQ(store.value()->discardDraft("invalid").error().code,
            bte::core::ErrorCode::invalidArgument);
  EXPECT_EQ(store.value()->publishDraft(std::string(32, 'b')).error().code,
            bte::core::ErrorCode::notFound);
  EXPECT_EQ(store.value()->discardDraft(std::string(32, 'b')).error().code,
            bte::core::ErrorCode::notFound);
}

TEST_F(ResultStoreFixture, trashListingSupportsRecoverableResults) {
  const auto storeRoot = root_ / "TrashListing";
  auto store = bte::results::ResultStore::open(storeRoot, root_ / "Data");
  ASSERT_TRUE(store.ok());
  auto writer = store.value()->begin(descriptor());
  ASSERT_TRUE(writer.ok());
  ASSERT_TRUE(writer.value()->append(records()).ok());
  auto finalized = writer.value()->finalizeAndPromote(
      bte::results::RunStatus::completed,
      {.finalEquityMicrodollars = 100'009'900'000, .pnlMicrodollars = 9'900});
  ASSERT_TRUE(finalized.ok());
  ASSERT_TRUE(store.value()->moveToTrash(finalized.value().resultId).ok());

  const auto active = store.value()->list();
  const auto trash = store.value()->listTrash();
  ASSERT_TRUE(active.ok());
  ASSERT_TRUE(trash.ok());
  EXPECT_TRUE(active.value().empty());
  ASSERT_EQ(trash.value().size(), 1U);
  EXPECT_EQ(trash.value().front().resultId, finalized.value().resultId);
  EXPECT_EQ(trash.value().front().canonicalResultHash,
            finalized.value().canonicalResultHash);
  EXPECT_TRUE(trash.value().front().available);

  ASSERT_TRUE(store.value()->restore(finalized.value().resultId).ok());
  const auto restoredTrash = store.value()->listTrash();
  ASSERT_TRUE(restoredTrash.ok());
  EXPECT_TRUE(restoredTrash.value().empty());
}

TEST_F(ResultStoreFixture, trashListingIsolatesCorruptFilesAndCancellation) {
  const auto storeRoot = root_ / "UnreadableTrash";
  auto store = bte::results::ResultStore::open(storeRoot, root_ / "Data");
  ASSERT_TRUE(store.ok());
  const auto corruptId = std::string(32, 'b');
  const auto corruptPath = storeRoot / "Trash" / (corruptId + ".bteresult");
  std::ofstream corrupt{corruptPath};
  corrupt << "not a Result database";
  corrupt.close();

  const auto listed = store.value()->listTrash();
  ASSERT_TRUE(listed.ok());
  ASSERT_EQ(listed.value().size(), 1U);
  EXPECT_EQ(listed.value().front().resultId, corruptId);
  EXPECT_FALSE(listed.value().front().available);
  EXPECT_FALSE(listed.value().front().unavailableReason.empty());

  bte::core::CancellationSource cancelled;
  cancelled.requestCancellation();
  const auto interrupted = store.value()->listTrash(cancelled.token());
  ASSERT_FALSE(interrupted.ok());
  EXPECT_EQ(interrupted.error().code, bte::core::ErrorCode::cancelled);
}

TEST_F(ResultStoreFixture, draftListingReportsDirectoryFailures) {
  const auto storeRoot = root_ / "UnreadableDrafts";
  auto store = bte::results::ResultStore::open(storeRoot, root_ / "Data");
  ASSERT_TRUE(store.ok());
  std::filesystem::remove(storeRoot / "Drafts");
  std::ofstream blocked{storeRoot / "Drafts"};
  blocked << "not a directory";
  blocked.close();
  const auto listed = store.value()->listDrafts();
  ASSERT_FALSE(listed.ok());
  EXPECT_EQ(listed.error().code, bte::core::ErrorCode::internal);
}

TEST_F(ResultStoreFixture,
       draftDiscardRetentionFailureRestoresArtifactAndAllowsRetry) {
  const auto storeRoot = root_ / "RetryableDiscard";
  auto store = bte::results::ResultStore::open(storeRoot, root_ / "Data");
  ASSERT_TRUE(store.ok());
  auto writer = store.value()->begin(descriptor());
  ASSERT_TRUE(writer.ok());
  const auto resultId = writer.value()->resultId();
  ASSERT_TRUE(writer.value()->append(records()).ok());
  ASSERT_TRUE(writer.value()
                  ->finalizeDraft(bte::results::RunStatus::completed,
                                  {.finalEquityMicrodollars = 100'009'900'000,
                                   .pnlMicrodollars = 9'900})
                  .ok());
  const auto draftPath = storeRoot / "Drafts" / (resultId + ".bteresult");
  for (const auto point : {
           bte::data::testing::RetentionFailurePoint::databaseOpen,
           bte::data::testing::RetentionFailurePoint::boundPreparation,
       }) {
    bte::data::testing::failRetentionAfter(point);
    const auto failed = store.value()->discardDraft(resultId);
    bte::data::testing::clearRetentionFailure();
    EXPECT_FALSE(failed.ok());
    EXPECT_TRUE(std::filesystem::exists(draftPath));
    EXPECT_FALSE(std::filesystem::exists(storeRoot / "Staging" /
                                         (resultId + ".discarding.bteresult")));
  }
  bte::results::testing::failNext(
      bte::results::testing::FailurePoint::draftDiscardRemoval);
  const auto failedRemoval = store.value()->discardDraft(resultId);
  bte::results::testing::clearFailure();
  EXPECT_FALSE(failedRemoval.ok());
  EXPECT_TRUE(std::filesystem::exists(draftPath));
  EXPECT_FALSE(std::filesystem::exists(storeRoot / "Staging" /
                                       (resultId + ".discarding.bteresult")));
  auto retention = bte::data::SegmentRetentionStore::open(root_ / "Data");
  ASSERT_TRUE(retention.ok());
  std::vector<std::string> segmentIds;
  for (const auto &span : descriptor().dataSelection.spans) {
    segmentIds.push_back(span.segmentId);
  }
  const auto alreadyRetained = retention.value()->acquire(resultId, segmentIds);
  ASSERT_TRUE(alreadyRetained.ok());
  EXPECT_EQ(alreadyRetained.value(), 0U);
  ASSERT_TRUE(store.value()->discardDraft(resultId).ok());
  EXPECT_FALSE(std::filesystem::exists(draftPath));
}

TEST_F(ResultStoreFixture,
       interruptedDraftDiscardRejectsMismatchedFilenameIdentity) {
  const auto storeRoot = root_ / "MismatchedDiscard";
  auto store = bte::results::ResultStore::open(storeRoot, root_ / "Data");
  ASSERT_TRUE(store.ok());
  auto writer = store.value()->begin(descriptor());
  ASSERT_TRUE(writer.ok());
  ASSERT_TRUE(writer.value()->append(records()).ok());
  const auto finalized = writer.value()->finalizeDraft(
      bte::results::RunStatus::completed,
      {.finalEquityMicrodollars = 100'009'900'000, .pnlMicrodollars = 9'900});
  ASSERT_TRUE(finalized.ok());
  const auto incorrectId = std::string(32, 'f');
  const auto staged =
      storeRoot / "Staging" / (incorrectId + ".discarding.bteresult");
  std::filesystem::rename(storeRoot / "Drafts" /
                              (finalized.value().resultId + ".bteresult"),
                          staged);
  const auto recovered =
      bte::results::ResultStore::open(storeRoot, root_ / "Data");
  ASSERT_TRUE(recovered.ok());
  EXPECT_FALSE(recovered.value()->recoveryNotices().empty());
  EXPECT_TRUE(std::ranges::any_of(
      recovered.value()->recoveryNotices(), [&](const auto &notice) {
        return notice.resultId == incorrectId &&
               notice.reason ==
                   bte::results::ResultRecoveryReason::legacyUnleased;
      }));
  EXPECT_TRUE(std::filesystem::exists(staged));
}

TEST_F(ResultStoreFixture,
       publicationAndDiscardFailClosedOnFilesystemAndCatalogFaults) {
  const auto storeRoot = root_ / "DraftActionFaults";
  auto store = bte::results::ResultStore::open(storeRoot, root_ / "Data");
  ASSERT_TRUE(store.ok());
  auto writer = store.value()->begin(descriptor());
  ASSERT_TRUE(writer.ok());
  ASSERT_TRUE(writer.value()->append(records()).ok());
  const auto finalized = writer.value()->finalizeDraft(
      bte::results::RunStatus::completed,
      {.finalEquityMicrodollars = 100'009'900'000, .pnlMicrodollars = 9'900});
  ASSERT_TRUE(finalized.ok());
  const auto resultId = finalized.value().resultId;
  const auto draftPath = storeRoot / "Drafts" / (resultId + ".bteresult");

  const auto blockedStage =
      storeRoot / "Staging" / (resultId + ".discarding.bteresult");
  std::filesystem::create_directory(blockedStage);
  EXPECT_FALSE(store.value()->discardDraft(resultId).ok());
  EXPECT_TRUE(std::filesystem::exists(draftPath));
  std::filesystem::remove(blockedStage);

  bte::results::testing::failNext(
      bte::results::testing::FailurePoint::draftPromotionMove);
  const auto failedMove = store.value()->publishDraft(resultId);
  bte::results::testing::clearFailure();
  EXPECT_FALSE(failedMove.ok());
  EXPECT_TRUE(std::filesystem::exists(draftPath));
  const auto catalogAfterMoveFailure = store.value()->list();
  ASSERT_TRUE(catalogAfterMoveFailure.ok());
  EXPECT_TRUE(catalogAfterMoveFailure.value().empty());

  bte::results::testing::failAfter(
      bte::results::testing::FailurePoint::databaseOpen, 1);
  const auto failed = store.value()->publishDraft(resultId);
  bte::results::testing::clearFailure();
  EXPECT_FALSE(failed.ok());
  EXPECT_TRUE(std::filesystem::exists(draftPath));

  ASSERT_TRUE(store.value()->publishDraft(resultId).ok());
  const auto resultPath = storeRoot / "Results" / (resultId + ".bteresult");
  std::ofstream corrupt{resultPath, std::ios::trunc};
  corrupt << "damaged published result";
  corrupt.close();
  EXPECT_FALSE(store.value()->publishDraft(resultId).ok());
}

TEST_F(ResultStoreFixture, beginPinsSourcesBeforeAnyWriterCanBeRecovered) {
  auto store = bte::results::ResultStore::open(root_ / "Store", root_ / "Data");
  ASSERT_TRUE(store.ok());
  auto writer = store.value()->begin(descriptor());
  ASSERT_TRUE(writer.ok());
  auto retention = bte::data::SegmentRetentionStore::open(root_ / "Data");
  ASSERT_TRUE(retention.ok());
  auto pinned = retention.value()->references(writer.value()->resultId());
  ASSERT_TRUE(pinned.ok());
  std::vector<std::string> expected;
  for (const auto &span : descriptor().dataSelection.spans) {
    expected.push_back(span.segmentId);
  }
  std::ranges::sort(expected);
  EXPECT_EQ(pinned.value(), expected);
  const auto lease = root_ / "Store" / "Leases" / (writer.value()->resultId() + ".sqlite3");
  EXPECT_TRUE(std::filesystem::is_regular_file(lease));
  auto observer = bte::results::ResultStore::open(root_ / "Store", root_ / "Data");
  ASSERT_TRUE(observer.ok());
  EXPECT_TRUE(observer.value()->recoveryNotices().empty());
  EXPECT_TRUE(observer.value()->listDrafts().value().empty());
}

TEST_F(ResultStoreFixture, initializationAndImportPinFailuresExposeNoArtifact) {
  auto source = bte::results::ResultStore::open(root_ / "SourceStore", root_ / "Data");
  ASSERT_TRUE(source.ok());
  auto writer = source.value()->begin(descriptor());
  ASSERT_TRUE(writer.ok());
  ASSERT_TRUE(writer.value()->append(records()).ok());
  auto finalized = writer.value()->finalizeAndPromote(bte::results::RunStatus::completed,
      {.finalEquityMicrodollars = 100'009'900'000, .pnlMicrodollars = 9'900});
  ASSERT_TRUE(finalized.ok());
  const auto sourceFile = root_ / "SourceStore" / "Results" / (finalized.value().resultId + ".bteresult");
  for (const auto point : {bte::data::testing::RetentionFailurePoint::databaseOpen,
                           bte::data::testing::RetentionFailurePoint::boundPreparation}) {
    for (const auto importing : {false, true}) {
      const auto directory = root_ / ("PinFailure-" + std::to_string(static_cast<int>(point)) + (importing ? "Import" : "Begin"));
      auto destination = bte::results::ResultStore::open(directory, root_ / "Data");
      ASSERT_TRUE(destination.ok());
      bte::data::testing::failRetentionAfter(point);
      if (importing) {
        EXPECT_FALSE(destination.value()->importResult(sourceFile).ok());
      } else {
        EXPECT_FALSE(destination.value()->begin(descriptor()).ok());
      }
      bte::data::testing::clearRetentionFailure();
      for (const auto *leaf : {"Staging", "Drafts", "Results", "Trash"}) {
        EXPECT_TRUE(std::filesystem::is_empty(directory / leaf));
      }
    }
  }
  EXPECT_TRUE(source.value()->openResult(finalized.value().resultId).ok());
}

TEST_F(ResultStoreFixture, legacyUnleasedArtifactsRemainByteIdenticalBesideSavedResults) {
  const auto directory = root_ / "Store";
  auto store = bte::results::ResultStore::open(directory, root_ / "Data");
  ASSERT_TRUE(store.ok());
  auto writer = store.value()->begin(descriptor());
  ASSERT_TRUE(writer.ok());
  ASSERT_TRUE(writer.value()->append(records()).ok());
  ASSERT_TRUE(writer.value()->finalizeAndPromote(bte::results::RunStatus::completed,
      {.finalEquityMicrodollars = 100'009'900'000, .pnlMicrodollars = 9'900}).ok());
  for (const auto &[id, suffix] : std::vector<std::pair<std::string, std::string>>{
           {std::string(32, 'a'), ".bteresult"}, {std::string(32, 'b'), ".purging.bteresult"},
           {std::string(32, 'c'), ".discarding.bteresult"}}) {
    std::ofstream artifact{directory / "Staging" / (id + suffix)};
    artifact << "legacy partial payload";
  }
  for (int attempt = 0; attempt < 2; ++attempt) {
    auto reopened = bte::results::ResultStore::open(directory, root_ / "Data");
    ASSERT_TRUE(reopened.ok());
    ASSERT_EQ(reopened.value()->recoveryNotices().size(), 3U);
    for (const auto &notice : reopened.value()->recoveryNotices()) {
      EXPECT_EQ(notice.reason, bte::results::ResultRecoveryReason::legacyUnleased);
      EXPECT_FALSE(notice.diagnostic.empty());
      EXPECT_FALSE(std::filesystem::exists(directory / "Leases" / (notice.resultId + ".sqlite3")));
    }
    EXPECT_EQ(reopened.value()->list().value().size(), 1U);
    EXPECT_TRUE(reopened.value()->listDrafts().value().empty());
    for (const auto &entry : std::filesystem::directory_iterator{directory / "Staging"}) {
      std::ifstream input{entry.path()};
      std::string content;
      std::getline(input, content);
      EXPECT_EQ(content, "legacy partial payload") << entry.path();
    }
  }
}

TEST_F(ResultStoreFixture, recoveryPreservesMissingPinsAndRejectsIncompleteOrSkippedBars) {
  for (const auto mode : {0, 1, 2, 3}) {
    const auto directory = root_ / ("InvalidPrefix-" + std::to_string(mode));
    std::string id;
    {
      auto store = bte::results::ResultStore::open(directory, root_ / "Data");
      ASSERT_TRUE(store.ok());
      auto writer = store.value()->begin(descriptor());
      ASSERT_TRUE(writer.ok());
      id = writer.value()->resultId();
      auto prefix = records();
      if (mode == 1) {
        prefix.pop_back();
      } else if (mode == 2) {
        prefix = {prefix.front()};
        prefix.front().timestamp = timestamp("2024-01-02 00:00:00+00:00");
      } else if (mode == 3) {
        prefix = {prefix.front(), prefix.front()};
        prefix.back().sequence = 1;
      }
      ASSERT_TRUE(writer.value()->append(prefix).ok());
    }
    if (mode == 0) {
      executeSql(root_ / "Data" / "References.sqlite3",
                 "DELETE FROM segment_references WHERE result_id='" + id + "'");
    }
    auto reopened = bte::results::ResultStore::open(directory, root_ / "Data");
    ASSERT_TRUE(reopened.ok());
    ASSERT_EQ(reopened.value()->recoveryNotices().size(), 1U);
    EXPECT_EQ(reopened.value()->recoveryNotices().front().reason,
        mode == 0 ? bte::results::ResultRecoveryReason::missingReferences
                  : bte::results::ResultRecoveryReason::invalidPrefix);
    EXPECT_TRUE(std::filesystem::exists(directory / "Staging" / (id + ".bteresult")));
    EXPECT_TRUE(reopened.value()->listDrafts().value().empty());
  }
}

TEST_F(ResultStoreFixture, recoveryPreservesDuplicateIdentityArtifacts) {
  const auto directory = root_ / "DuplicateRecovery";
  std::string id;
  std::filesystem::path stagingPath;
  std::filesystem::path draftPath;
  {
    auto store = bte::results::ResultStore::open(directory, root_ / "Data");
    ASSERT_TRUE(store.ok());
    auto writer = store.value()->begin(descriptor());
    ASSERT_TRUE(writer.ok());
    id = writer.value()->resultId();
    ASSERT_TRUE(writer.value()->append(records()).ok());
    stagingPath = directory / "Staging" / (id + ".bteresult");
    draftPath = directory / "Drafts" / (id + ".bteresult");
  }
  const auto stagingBytes = [&] {
    std::ifstream input{stagingPath, std::ios::binary};
    return std::string{std::istreambuf_iterator<char>{input},
                       std::istreambuf_iterator<char>{}};
  }();
  {
    std::ofstream duplicate{draftPath, std::ios::binary};
    duplicate << "unrelated duplicate identity artifact";
  }
  const auto draftBytes = [&] {
    std::ifstream input{draftPath, std::ios::binary};
    return std::string{std::istreambuf_iterator<char>{input},
                       std::istreambuf_iterator<char>{}};
  }();

  auto reopened = bte::results::ResultStore::open(directory, root_ / "Data");
  ASSERT_TRUE(reopened.ok());
  ASSERT_EQ(reopened.value()->recoveryNotices().size(), 1U);
  EXPECT_EQ(reopened.value()->recoveryNotices().front().resultId, id);
  EXPECT_EQ(reopened.value()->recoveryNotices().front().reason,
            bte::results::ResultRecoveryReason::invalidPrefix);
  const auto stagingAfter = [&] {
    std::ifstream input{stagingPath, std::ios::binary};
    return std::string{std::istreambuf_iterator<char>{input},
                       std::istreambuf_iterator<char>{}};
  }();
  const auto draftAfter = [&] {
    std::ifstream input{draftPath, std::ios::binary};
    return std::string{std::istreambuf_iterator<char>{input},
                       std::istreambuf_iterator<char>{}};
  }();
  EXPECT_EQ(stagingBytes, stagingAfter);
  EXPECT_EQ(draftBytes, draftAfter);
}

TEST_F(ResultStoreFixture, orphanInitializationPinsAreReportedAndTombstonesRemainQuiet) {
  const auto directory = root_ / "Store";
  std::string id;
  {
    auto store = bte::results::ResultStore::open(directory, root_ / "Data");
    ASSERT_TRUE(store.ok());
    auto writer = store.value()->begin(descriptor());
    ASSERT_TRUE(writer.ok());
    id = writer.value()->resultId();
  }
  ASSERT_TRUE(std::filesystem::remove(directory / "Staging" / (id + ".bteresult")));
  auto reopened = bte::results::ResultStore::open(directory, root_ / "Data");
  ASSERT_TRUE(reopened.ok());
  ASSERT_EQ(reopened.value()->recoveryNotices().size(), 1U);
  EXPECT_EQ(reopened.value()->recoveryNotices().front().reason,
            bte::results::ResultRecoveryReason::orphanedInitialization);
  auto retention = bte::data::SegmentRetentionStore::open(root_ / "Data");
  ASSERT_TRUE(retention.ok());
  ASSERT_EQ(retention.value()->references(id).value().size(), 2U);
  ASSERT_TRUE(retention.value()->release(id).ok());
  reopened = bte::results::ResultStore::open(directory, root_ / "Data");
  ASSERT_TRUE(reopened.ok());
  EXPECT_TRUE(reopened.value()->recoveryNotices().empty());
  EXPECT_TRUE(std::filesystem::is_regular_file(directory / "Leases" / (id + ".sqlite3")));
}

TEST_F(ResultStoreFixture, invalidLeaseIdentityVersionModeAndSchemaPreserveStaging) {
  for (const auto &[name, sql] : std::vector<std::pair<std::string, std::string>>{
           {"Identity", "UPDATE lease_identity SET result_id='wrong'"},
           {"Version", "UPDATE lease_identity SET protocol_version=2"},
           {"Mode", "PRAGMA journal_mode=WAL"},
           {"Schema", "DROP TABLE lease_identity"},
           {"Duplicate", "INSERT INTO lease_identity VALUES('extra',1)"}}) {
    const auto directory = root_ / name;
    std::string id;
    {
      auto store = bte::results::ResultStore::open(directory, root_ / "Data");
      ASSERT_TRUE(store.ok());
      auto writer = store.value()->begin(descriptor());
      ASSERT_TRUE(writer.ok());
      id = writer.value()->resultId();
    }
    executeSql(directory / "Leases" / (id + ".sqlite3"), sql);
    auto reopened = bte::results::ResultStore::open(directory, root_ / "Data");
    ASSERT_TRUE(reopened.ok());
    ASSERT_EQ(reopened.value()->recoveryNotices().size(), 1U);
    EXPECT_EQ(reopened.value()->recoveryNotices().front().reason,
              bte::results::ResultRecoveryReason::invalidLease);
    EXPECT_TRUE(std::filesystem::is_regular_file(directory / "Staging" / (id + ".bteresult")));
    EXPECT_TRUE(reopened.value()->listDrafts().value().empty());
  }
}

TEST_F(ResultStoreFixture, terminalTailAndMetadataCommitAtomicallyAcrossFailures) {
  for (const auto point : {bte::results::testing::FailurePoint::statementPreparation,
                           bte::results::testing::FailurePoint::statementExecution,
                           bte::results::testing::FailurePoint::sqlExecution}) {
    const auto directory = root_ / ("AtomicTail-" + std::to_string(static_cast<int>(point)));
    auto store = bte::results::ResultStore::open(directory, root_ / "Data");
    ASSERT_TRUE(store.ok());
    auto writer = store.value()->begin(descriptor());
    ASSERT_TRUE(writer.ok());
    const auto canonical = records();
    ASSERT_TRUE(writer.value()->append({canonical.front()}).ok());
    const auto path = directory / "Staging" / (writer.value()->resultId() + ".bteresult");
    bte::results::testing::failAfter(point,
        point == bte::results::testing::FailurePoint::sqlExecution ? 1U : 0U);
    const auto tail = std::span<const bte::results::CanonicalRecord>{canonical}.subspan(1);
    const bte::results::FinalSummary summary{.finalEquityMicrodollars = 100'009'900'000, .pnlMicrodollars = 9'900};
    EXPECT_FALSE(writer.value()->finalizeDraft(bte::results::RunStatus::completed, summary, "", tail).ok());
    bte::results::testing::clearFailure();
    EXPECT_EQ(queryInteger(path, "SELECT count(*) FROM canonical_records"), 1);
    EXPECT_EQ(queryInteger(path, "SELECT count(*) FROM summary"), 0);
    EXPECT_EQ(queryInteger(path, "SELECT status FROM result_meta"), 0);
    auto invalid = canonical.back();
    invalid.sequence = 0;
    EXPECT_FALSE(writer.value()->finalizeDraft(bte::results::RunStatus::completed, summary, "", {&invalid, 1}).ok());
    ASSERT_TRUE(writer.value()->finalizeDraft(bte::results::RunStatus::completed, summary, "", tail).ok());
    ASSERT_TRUE(store.value()->publishDraft(writer.value()->resultId()).ok());
    const auto opened = store.value()->openResult(writer.value()->resultId());
    ASSERT_TRUE(opened.ok());
    EXPECT_EQ(opened.value().records, canonical);
    EXPECT_EQ(opened.value().summary, summary);
  }
}

TEST_F(ResultStoreFixture, everyLifecycleMutationReturnsBusyWithoutChangingArtifacts) {
  const auto directory = root_ / "Store";
  auto store = bte::results::ResultStore::open(directory, root_ / "Data");
  ASSERT_TRUE(store.ok());
  auto writer = store.value()->begin(descriptor());
  ASSERT_TRUE(writer.ok());
  ASSERT_TRUE(writer.value()->append(records()).ok());
  ASSERT_TRUE(writer.value()->finalizeDraft(bte::results::RunStatus::completed,
      {.finalEquityMicrodollars = 100'009'900'000, .pnlMicrodollars = 9'900}).ok());
  const auto id = writer.value()->resultId();
  sqlite3 *raw = nullptr;
  ASSERT_EQ(sqlite3_open((directory / "Leases" / (id + ".sqlite3")).string().c_str(), &raw), SQLITE_OK);
  const auto database = std::unique_ptr<sqlite3, decltype(&sqlite3_close)>{raw, &sqlite3_close};
  ASSERT_EQ(sqlite3_exec(database.get(), "BEGIN EXCLUSIVE", nullptr, nullptr, nullptr), SQLITE_OK);
  for (const auto &operation : std::vector<std::function<bte::core::Result<void>()>>{
           [&] { return store.value()->discardDraft(id); },
           [&] { return store.value()->renameResult(id, "Busy"); },
           [&] { return store.value()->moveToTrash(id); },
           [&] { return store.value()->restore(id); },
           [&] { return store.value()->purge(id); }}) {
    const auto result = operation();
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.error().code, bte::core::ErrorCode::resourceBusy);
  }
  const auto published = store.value()->publishDraft(id);
  ASSERT_FALSE(published.ok());
  EXPECT_EQ(published.error().code, bte::core::ErrorCode::resourceBusy);
  EXPECT_TRUE(std::filesystem::is_regular_file(directory / "Drafts" / (id + ".bteresult")));
  std::string unexpectedStagingEntries;
  for (const auto &entry :
       std::filesystem::directory_iterator{directory / "Staging"}) {
    unexpectedStagingEntries += entry.path().filename().string() + " ";
  }
  EXPECT_TRUE(std::filesystem::is_empty(directory / "Staging"))
      << "unexpected Staging entries: " << unexpectedStagingEntries;
  EXPECT_TRUE(std::filesystem::is_empty(directory / "Results"));
  EXPECT_TRUE(std::filesystem::is_empty(directory / "Trash"));
}

} // namespace
