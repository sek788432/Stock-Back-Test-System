#include "Bte/Results/ReproducibilityCoordinator.h"
#include "Bte/Results/ResultStore.h"

#include "Bte/Core/Cancellation.h"
#include "Bte/Core/CanonicalJson.h"
#include "Bte/Core/Digest.h"
#include "Bte/Core/Time.h"
#include "Bte/Data/ReleaseSnapshot.h"
#include "Bte/Data/SegmentRetention.h"
#include "Bte/Results/ReproducibilityStore.h"

#include "ResultStoreTestHooks.h"
#include "SegmentRetentionTestHooks.h"

#include <gtest/gtest.h>
#include <sqlite3.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <nlohmann/json.hpp>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
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

[[nodiscard]] std::string queryText(const std::filesystem::path &path,
                                    const std::string &sql) {
  sqlite3 *rawDatabase = nullptr;
  if (sqlite3_open_v2(path.string().c_str(), &rawDatabase, SQLITE_OPEN_READONLY,
                      nullptr) != SQLITE_OK) {
    return {};
  }
  const auto database = std::unique_ptr<sqlite3, decltype(&sqlite3_close)>{
      rawDatabase, &sqlite3_close};
  sqlite3_stmt *rawStatement = nullptr;
  if (sqlite3_prepare_v2(database.get(), sql.c_str(), -1, &rawStatement,
                         nullptr) != SQLITE_OK) {
    return {};
  }
  const auto statement =
      std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>{
          rawStatement, &sqlite3_finalize};
  if (sqlite3_step(statement.get()) != SQLITE_ROW) {
    return {};
  }
  const auto *const value = sqlite3_column_text(statement.get(), 0);
  const auto size = sqlite3_column_bytes(statement.get(), 0);
  if (value == nullptr || size < 0) {
    return {};
  }
  std::string result;
  result.reserve(static_cast<std::size_t>(size));
  for (auto index = 0; index < size; ++index) {
    result.push_back(static_cast<char>(value[index]));
  }
  return result;
}

void executeSqlText(const std::filesystem::path &path, const std::string &sql,
                    const std::string &value) {
  sqlite3 *rawDatabase = nullptr;
  ASSERT_EQ(sqlite3_open(path.string().c_str(), &rawDatabase), SQLITE_OK);
  const auto database = std::unique_ptr<sqlite3, decltype(&sqlite3_close)>{
      rawDatabase, &sqlite3_close};
  sqlite3_stmt *rawStatement = nullptr;
  ASSERT_EQ(sqlite3_prepare_v2(database.get(), sql.c_str(), -1, &rawStatement,
                               nullptr),
            SQLITE_OK);
  const auto statement =
      std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>{
          rawStatement, &sqlite3_finalize};
  ASSERT_EQ(sqlite3_bind_text(statement.get(), 1, value.data(),
                              static_cast<int>(value.size()), SQLITE_TRANSIENT),
            SQLITE_OK);
  ASSERT_EQ(sqlite3_step(statement.get()), SQLITE_DONE);
}

[[nodiscard]] bool rewriteVerificationEvidence(
    const std::filesystem::path &path,
    const std::function<void(nlohmann::json &)> &mutate) {
  std::ifstream input{path, std::ios::binary};
  if (!input) {
    return false;
  }
  const std::string bytes{std::istreambuf_iterator<char>{input},
                          std::istreambuf_iterator<char>{}};
  auto parsed = bte::core::parseCanonicalJson(bytes);
  if (!parsed.ok()) {
    return false;
  }
  auto document = std::move(parsed).value();
  mutate(document);
  document.erase("integrityHash");
  auto integrityBytes = bte::core::serializeCanonicalJson(document);
  if (!integrityBytes.ok()) {
    return false;
  }
  document["integrityHash"] = bte::core::sha256(integrityBytes.value());
  auto serialized = bte::core::serializeCanonicalJson(document);
  if (!serialized.ok()) {
    return false;
  }
  std::ofstream output{path, std::ios::binary | std::ios::trunc};
  if (!output) {
    return false;
  }
  output.write(serialized.value().data(),
               static_cast<std::streamsize>(serialized.value().size()));
  return output.good();
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
        .provenance = {},
    };
  }

  [[nodiscard]] bte::results::RunDescriptor schemaThreeDescriptor() const {
    using namespace bte;
    auto value = descriptor();
    value.schemaVersion = 3;
    auto &provenance = value.provenance;
    provenance.engineBuild = "fixture-build-1";
    provenance.timeframe = selection_.timeframe;
    provenance.costProfile = "retailUsEquityV1";
    provenance.commissionRatePartsPerBillion = 0;
    provenance.slippageRatePartsPerBillion = 10'000'000;
    provenance.seed = 0;
    provenance.strategyMode = results::StrategyMode::python;
    provenance.apiVersion = "1";
    provenance.templateVersion = "sma-cross-v1";
    provenance.canonicalParametersJson =
        R"({"fast_period":2,"quantity":1,"slow_period":3})";
    provenance.sourceUtf8 =
        "def create_strategy(params):\n    return Strategy()\n";
    provenance.sourceHash = core::sha256(provenance.sourceUtf8);
    value.strategyHash = provenance.sourceHash;

    const auto numpyHash = std::string(64, 'd');
    const auto stockbtHash = std::string(64, 'e');
    const auto installedHash = std::string(64, 'f');
    const auto components = nlohmann::json::array({
        {{"artifactSha256", numpyHash},
         {"name", "numpy"},
         {"licenses", nlohmann::json::array({"BSD-3-Clause"})},
         {"upstreamUrl", "https://example.invalid/numpy"},
         {"version", "2.5.4"}},
        {{"artifactSha256", stockbtHash},
         {"name", "stockbt"},
         {"licenses", nlohmann::json::array({"Apache-2.0"})},
         {"upstreamUrl", "project-owned"},
         {"version", "1"}},
    });
    const auto manifest = nlohmann::json{
        {"architecture", "arm64"},
        {"components", components},
        {"installedFiles",
         nlohmann::json::array(
             {{{"path", "python/bin/python3"}, {"sha256", installedHash}}})},
        {"platform", "macos"},
        {"profileBuilderVersion", "fixture-builder-1"},
        {"profileId", "fixture-profile"},
        {"pythonAbi", "cp314"},
        {"pythonExecutable", "python/bin/python3"},
        {"pythonVersion", "3.14.9"},
        {"schemaVersion", std::uint64_t{1}},
        {"stockbtApiVersion", "1"},
        {"stockbtSourceSha256", stockbtHash},
    };
    const auto canonicalManifest = core::serializeCanonicalJson(manifest);
    EXPECT_TRUE(canonicalManifest.ok());
    provenance.runtime = results::RuntimeProfileProvenance{
        .profileId = "fixture-profile",
        .canonicalManifestJson = canonicalManifest.value(),
        .manifestHash = core::sha256(canonicalManifest.value()),
        .pythonVersion = "3.14.9",
        .pythonAbi = "cp314",
        .packages =
            {{.name = "numpy", .version = "2.5.4", .artifactHash = numpyHash},
             {.name = "stockbt", .version = "1", .artifactHash = stockbtHash}},
        .stockbtSourceHash = stockbtHash,
    };
    for (const auto *key :
         {"aggregationPolicy", "costs", "dataSelection", "engineBuild",
          "numericPolicy", "runtimeProfile", "runtimePackages", "seed",
          "strategyApi", "strategyArtifact", "strategyMode", "strategyTemplate",
          "timeframe"}) {
      provenance.provenanceAvailability.emplace(
          key, results::ProvenanceAvailabilityState::recorded);
    }
    return value;
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

  [[nodiscard]] bte::core::Result<bte::results::OpenedResult>
  createSchemaThreeResult(bte::results::ResultStore &store,
                          const bte::results::RunDescriptor &value,
                          const bte::results::RunStatus status,
                          const std::string &terminalReason) const {
    auto writer = store.begin(value);
    if (!writer.ok()) {
      return writer.error();
    }
    auto appended = writer.value()->append(records());
    if (!appended.ok()) {
      return appended.error();
    }
    const auto summary =
        status == bte::results::RunStatus::completed
            ? bte::results::FinalSummary{.finalEquityMicrodollars =
                                             100'009'900'000,
                                         .pnlMicrodollars = 9'900}
            : bte::results::FinalSummary{};
    auto finalized =
        writer.value()->finalizeAndPromote(status, summary, terminalReason);
    if (!finalized.ok()) {
      return finalized.error();
    }
    return store.openResult(finalized.value().resultId);
  }

  [[nodiscard]] bte::core::Result<bte::results::FinalizedResult>
  createSchemaThreeDraft(bte::results::ResultStore &store,
                         const bte::results::RunDescriptor &value) const {
    auto writer = store.begin(value);
    if (!writer.ok()) {
      return writer.error();
    }
    auto appended = writer.value()->append(records());
    if (!appended.ok()) {
      return appended.error();
    }
    return writer.value()->finalizeDraft(
        bte::results::RunStatus::completed,
        {.finalEquityMicrodollars = 100'009'900'000, .pnlMicrodollars = 9'900});
  }

  [[nodiscard]] static bte::results::FinalizedResult
  asFinalized(const bte::results::OpenedResult &result) {
    return {.resultId = result.resultId,
            .canonicalResultHash = result.canonicalResultHash};
  }

  std::filesystem::path root_;
  bte::data::DataSelectionIdentity selection_;
};

TEST_F(ResultStoreFixture, schemaTwoHashMatchesIndependentLiteralOracle) {
  // Python struct.pack/hashlib independently encode the normative frames;
  // the framing contract is specified in Docs/Specs/EngineReplayPnL.md §8.1.
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

TEST_F(ResultStoreFixture,
       schemaTwoMigrationCreatesVerifiedLegacyCopyAndPreservesSource) {
  auto store =
      bte::results::ResultStore::open(root_ / "Migration", root_ / "Data");
  ASSERT_TRUE(store.ok()) << store.error().message;
  auto writer = store.value()->begin(descriptor());
  ASSERT_TRUE(writer.ok()) << writer.error().message;
  ASSERT_TRUE(writer.value()->append(records()).ok());
  const auto source = writer.value()->finalizeAndPromote(
      bte::results::RunStatus::failed, {}, "legacy failure");
  ASSERT_TRUE(source.ok()) << source.error().message;
  ASSERT_TRUE(store.value()
                  ->renameResult(source.value().resultId, "Legacy result")
                  .ok());
  const auto sourcePath = root_ / "Migration" / "Results" /
                          (source.value().resultId + ".bteresult");
  const auto readBytes = [](const std::filesystem::path &path) {
    std::ifstream input{path, std::ios::binary};
    return std::string{std::istreambuf_iterator<char>{input},
                       std::istreambuf_iterator<char>{}};
  };
  const auto sourceBytes = readBytes(sourcePath);
  const auto original = store.value()->openResult(source.value().resultId);
  ASSERT_TRUE(original.ok()) << original.error().message;
  // Independently computed from the schema-2 framing using Python struct and
  // hashlib, then from the schema-3 JCS frames using Python json and hashlib.
  EXPECT_EQ(source.value().canonicalResultHash,
            "6cddbbf4779472999e8bd30acecaae76d89268f87c5f07541eb0034007e2f62a");

  const auto migrated =
      store.value()->migrateSchema2Copy(source.value().resultId);

  ASSERT_TRUE(migrated.ok()) << migrated.error().message;
  EXPECT_EQ(migrated.value().canonicalResultHash,
            "0ebf2a63f7e10b3591e77d377aec9e227f503525938f31d1a30a07740de1bf95");
  EXPECT_NE(migrated.value().resultId, source.value().resultId);
  EXPECT_NE(migrated.value().canonicalResultHash,
            source.value().canonicalResultHash);
  const auto copy = store.value()->openResult(migrated.value().resultId);
  ASSERT_TRUE(copy.ok()) << copy.error().message;
  EXPECT_EQ(copy.value().schemaVersion, 3);
  EXPECT_EQ(copy.value().status, original.value().status);
  EXPECT_EQ(copy.value().terminalReason, original.value().terminalReason);
  EXPECT_EQ(copy.value().descriptor.universe,
            original.value().descriptor.universe);
  EXPECT_EQ(copy.value().descriptor.range, original.value().descriptor.range);
  EXPECT_EQ(copy.value().descriptor.initialCapitalMicrodollars,
            original.value().descriptor.initialCapitalMicrodollars);
  EXPECT_EQ(copy.value().descriptor.dataSelection,
            original.value().descriptor.dataSelection);
  EXPECT_EQ(copy.value().records, original.value().records);
  EXPECT_EQ(copy.value().capabilities, original.value().capabilities);
  EXPECT_EQ(copy.value().displayName, "Legacy result");
  EXPECT_FALSE(copy.value().summary.diagnosticsComplete);
  EXPECT_EQ(copy.value().descriptor.provenance.strategyMode,
            bte::results::StrategyMode::legacyUnrecorded);
  EXPECT_EQ(copy.value().descriptor.provenance.canonicalParametersJson, "null");
  ASSERT_TRUE(copy.value().migratedFrom.has_value());
  EXPECT_EQ(copy.value().migratedFrom->resultId, source.value().resultId);
  EXPECT_EQ(copy.value().migratedFrom->canonicalResultHash,
            source.value().canonicalResultHash);
  const auto expectedMigrationSource = bte::results::MigrationSourceIdentity{
      .resultId = source.value().resultId,
      .canonicalResultHash = source.value().canonicalResultHash};
  EXPECT_EQ(*copy.value().migratedFrom, expectedMigrationSource);
  EXPECT_EQ(readBytes(sourcePath), sourceBytes);
  auto retention = bte::data::SegmentRetentionStore::open(root_ / "Data");
  ASSERT_TRUE(retention.ok()) << retention.error().message;
  const auto sourceReferences =
      retention.value()->references(source.value().resultId);
  const auto copyReferences =
      retention.value()->references(migrated.value().resultId);
  ASSERT_TRUE(sourceReferences.ok()) << sourceReferences.error().message;
  ASSERT_TRUE(copyReferences.ok()) << copyReferences.error().message;
  EXPECT_EQ(copyReferences.value(), sourceReferences.value());

  const auto reopenedSource =
      store.value()->openResult(source.value().resultId);
  ASSERT_TRUE(reopenedSource.ok()) << reopenedSource.error().message;
  EXPECT_EQ(reopenedSource.value().canonicalResultHash,
            source.value().canonicalResultHash);
  EXPECT_EQ(reopenedSource.value().records, records());
}

TEST_F(ResultStoreFixture,
       schemaTwoMigrationFailuresPreserveSourceAndRemoveStagingCopy) {
  auto store = bte::results::ResultStore::open(root_ / "MigrationFailures",
                                               root_ / "Data");
  ASSERT_TRUE(store.ok()) << store.error().message;
  auto writer = store.value()->begin(descriptor());
  ASSERT_TRUE(writer.ok()) << writer.error().message;
  ASSERT_TRUE(writer.value()->append(records()).ok());
  const auto source = writer.value()->finalizeAndPromote(
      bte::results::RunStatus::failed, {}, "legacy failure");
  ASSERT_TRUE(source.ok()) << source.error().message;
  const auto sourcePath = root_ / "MigrationFailures" / "Results" /
                          (source.value().resultId + ".bteresult");
  const auto readBytes = [](const std::filesystem::path &path) {
    std::ifstream input{path, std::ios::binary};
    return std::string{std::istreambuf_iterator<char>{input},
                       std::istreambuf_iterator<char>{}};
  };
  const auto sourceBytes = readBytes(sourcePath);
  auto retention = bte::data::SegmentRetentionStore::open(root_ / "Data");
  ASSERT_TRUE(retention.ok()) << retention.error().message;
  const std::vector<std::pair<bte::results::testing::FailurePoint, char>>
      failures{{bte::results::testing::FailurePoint::migrationCopy, '1'},
               {bte::results::testing::FailurePoint::migrationWrite, '2'},
               {bte::results::testing::FailurePoint::migrationValidation, '3'},
               {bte::results::testing::FailurePoint::catalogVisibility, '4'}};

  for (const auto &[failure, identity] : failures) {
    const auto candidateId = std::string(32, identity);
    bte::results::testing::forceNextResultId(candidateId);
    bte::results::testing::failNext(failure);

    const auto migrated =
        store.value()->migrateSchema2Copy(source.value().resultId);

    EXPECT_FALSE(migrated.ok());
    EXPECT_EQ(readBytes(sourcePath), sourceBytes);
    EXPECT_TRUE(store.value()->openResult(source.value().resultId).ok());
    EXPECT_TRUE(
        std::filesystem::is_empty(root_ / "MigrationFailures" / "Staging"));
    const auto references = retention.value()->references(candidateId);
    ASSERT_TRUE(references.ok()) << references.error().message;
    EXPECT_TRUE(references.value().empty());
    std::size_t resultArtifacts = 0;
    for (const auto &entry : std::filesystem::directory_iterator(
             root_ / "MigrationFailures" / "Results")) {
      if (entry.path().extension() == ".bteresult") {
        ++resultArtifacts;
      }
    }
    EXPECT_EQ(resultArtifacts, 1U);
  }
}

TEST_F(ResultStoreFixture,
       schemaTwoMigrationSkipsOccupiedDestinationIdWithoutClobbering) {
  auto store = bte::results::ResultStore::open(root_ / "MigrationCollision",
                                               root_ / "Data");
  ASSERT_TRUE(store.ok()) << store.error().message;
  const auto createLegacyResult = [&]() {
    auto writer = store.value()->begin(descriptor());
    if (!writer.ok()) {
      return bte::core::Result<bte::results::FinalizedResult>{writer.error()};
    }
    auto appended = writer.value()->append(records());
    if (!appended.ok()) {
      return bte::core::Result<bte::results::FinalizedResult>{appended.error()};
    }
    return writer.value()->finalizeAndPromote(bte::results::RunStatus::failed,
                                              {}, "legacy failure");
  };
  const auto source = createLegacyResult();
  ASSERT_TRUE(source.ok()) << source.error().message;
  const auto occupied = createLegacyResult();
  ASSERT_TRUE(occupied.ok()) << occupied.error().message;
  const auto occupiedPath = root_ / "MigrationCollision" / "Results" /
                            (occupied.value().resultId + ".bteresult");
  const auto readBytes = [](const std::filesystem::path &path) {
    std::ifstream input{path, std::ios::binary};
    return std::string{std::istreambuf_iterator<char>{input},
                       std::istreambuf_iterator<char>{}};
  };
  const auto occupiedBytes = readBytes(occupiedPath);
  bte::results::testing::forceNextResultId(occupied.value().resultId);

  const auto migrated =
      store.value()->migrateSchema2Copy(source.value().resultId);

  ASSERT_TRUE(migrated.ok()) << migrated.error().message;
  EXPECT_NE(migrated.value().resultId, occupied.value().resultId);
  EXPECT_EQ(readBytes(occupiedPath), occupiedBytes);
  const auto preserved = store.value()->openResult(occupied.value().resultId);
  ASSERT_TRUE(preserved.ok()) << preserved.error().message;
  EXPECT_EQ(preserved.value().canonicalResultHash,
            occupied.value().canonicalResultHash);
}

TEST_F(ResultStoreFixture,
       schemaTwoMigrationRejectsCorruptSourceWithoutCreatingCopy) {
  auto store = bte::results::ResultStore::open(root_ / "MigrationCorrupt",
                                               root_ / "Data");
  ASSERT_TRUE(store.ok()) << store.error().message;
  auto writer = store.value()->begin(descriptor());
  ASSERT_TRUE(writer.ok()) << writer.error().message;
  ASSERT_TRUE(writer.value()->append(records()).ok());
  const auto source = writer.value()->finalizeAndPromote(
      bte::results::RunStatus::failed, {}, "legacy failure");
  ASSERT_TRUE(source.ok()) << source.error().message;
  const auto sourcePath = root_ / "MigrationCorrupt" / "Results" /
                          (source.value().resultId + ".bteresult");
  executeSql(sourcePath, "UPDATE result_meta SET canonical_hash='" +
                             std::string(64, '0') + "'");
  const auto readBytes = [](const std::filesystem::path &path) {
    std::ifstream input{path, std::ios::binary};
    return std::string{std::istreambuf_iterator<char>{input},
                       std::istreambuf_iterator<char>{}};
  };
  const auto sourceBytes = readBytes(sourcePath);

  const auto migrated =
      store.value()->migrateSchema2Copy(source.value().resultId);

  ASSERT_FALSE(migrated.ok());
  EXPECT_EQ(migrated.error().code, bte::core::ErrorCode::schemaMismatch);
  EXPECT_EQ(readBytes(sourcePath), sourceBytes);
  EXPECT_TRUE(
      std::filesystem::is_empty(root_ / "MigrationCorrupt" / "Staging"));
  EXPECT_TRUE(std::filesystem::is_empty(root_ / "MigrationCorrupt" / "Drafts"));
}

TEST_F(ResultStoreFixture,
       schemaTwoMigrationRejectsUnavailableDataReferenceWithoutChangingSource) {
  auto store = bte::results::ResultStore::open(root_ / "MigrationMissingData",
                                               root_ / "Data");
  ASSERT_TRUE(store.ok()) << store.error().message;
  auto writer = store.value()->begin(descriptor());
  ASSERT_TRUE(writer.ok()) << writer.error().message;
  ASSERT_TRUE(writer.value()->append(records()).ok());
  const auto source = writer.value()->finalizeAndPromote(
      bte::results::RunStatus::failed, {}, "legacy failure");
  ASSERT_TRUE(source.ok()) << source.error().message;
  const auto opened = store.value()->openResult(source.value().resultId);
  ASSERT_TRUE(opened.ok()) << opened.error().message;
  ASSERT_FALSE(opened.value().descriptor.dataSelection.spans.empty());
  const auto sourcePath = root_ / "MigrationMissingData" / "Results" /
                          (source.value().resultId + ".bteresult");
  const auto segmentPath =
      root_ / "Data" / "Segments" /
      (opened.value().descriptor.dataSelection.spans.front().segmentId +
       ".btedata");
  const auto readBytes = [](const std::filesystem::path &path) {
    std::ifstream input{path, std::ios::binary};
    return std::string{std::istreambuf_iterator<char>{input},
                       std::istreambuf_iterator<char>{}};
  };
  const auto sourceBytes = readBytes(sourcePath);
  ASSERT_TRUE(std::filesystem::remove(segmentPath));

  const auto migrated =
      store.value()->migrateSchema2Copy(source.value().resultId);

  ASSERT_FALSE(migrated.ok());
  EXPECT_EQ(migrated.error().code,
            bte::core::ErrorCode::dataSnapshotUnavailable);
  EXPECT_EQ(readBytes(sourcePath), sourceBytes);
  EXPECT_TRUE(
      std::filesystem::is_empty(root_ / "MigrationMissingData" / "Staging"));
}

TEST_F(ResultStoreFixture,
       schemaTwoMigrationRejectsSchemaThreeAndInvalidResultIdentities) {
  const auto directory = root_ / "MigrationWrongSchema";
  auto store = bte::results::ResultStore::open(directory, root_ / "Data");
  ASSERT_TRUE(store.ok()) << store.error().message;
  const auto schemaThree = createSchemaThreeResult(
      *store.value(), schemaThreeDescriptor(),
      bte::results::RunStatus::completed, "already schema three");
  ASSERT_TRUE(schemaThree.ok()) << schemaThree.error().message;

  const auto wrongSchema =
      store.value()->migrateSchema2Copy(schemaThree.value().resultId);
  ASSERT_FALSE(wrongSchema.ok());
  EXPECT_EQ(wrongSchema.error().code, bte::core::ErrorCode::schemaMismatch);
  const auto invalidId = store.value()->migrateSchema2Copy("invalid");
  ASSERT_FALSE(invalidId.ok());
  EXPECT_EQ(invalidId.error().code, bte::core::ErrorCode::invalidArgument);
  EXPECT_TRUE(std::filesystem::is_empty(directory / "Staging"));
}

TEST_F(
    ResultStoreFixture,
    reproducibilityEvidencePersistsCanonicalIdentityWithoutPublishingResults) {
  const auto directory = root_ / "VerificationStore";
  const auto descriptor = schemaThreeDescriptor();
  auto store = bte::results::VerificationStore::open(directory);
  ASSERT_TRUE(store.ok()) << store.error().message;

  const auto created = store.value()->create(descriptor);

  ASSERT_TRUE(created.ok()) << created.error().message;
  EXPECT_EQ(created.value().status, bte::results::VerificationStatus::running);
  EXPECT_EQ(created.value().progressOrdinal, 0U);
  EXPECT_TRUE(created.value().attempts.empty());
  EXPECT_EQ(created.value().identity.sha256.size(), 64U);
  EXPECT_EQ(created.value().integrityHash.size(), 64U);
  const auto reopened = store.value()->load(created.value().verificationId);
  ASSERT_TRUE(reopened.ok()) << reopened.error().message;
  EXPECT_EQ(reopened.value(), created.value());
  EXPECT_TRUE(std::filesystem::is_regular_file(
      directory / "Verifications" /
      (created.value().verificationId + ".bteverify")));
  EXPECT_FALSE(std::filesystem::exists(directory / "Results"));
  const auto verified = store.value()->hasVerifiedEvidence(descriptor);
  ASSERT_TRUE(verified.ok()) << verified.error().message;
  EXPECT_FALSE(verified.value());

  auto oversized = descriptor;
  const nlohmann::json largeParameters{{"payload", std::string(63'000, 'x')}};
  auto canonicalParameters = bte::core::serializeCanonicalJson(largeParameters);
  ASSERT_TRUE(canonicalParameters.ok()) << canonicalParameters.error().message;
  ASSERT_LT(canonicalParameters.value().size(), 64U * 1024U);
  oversized.provenance.canonicalParametersJson = canonicalParameters.value();
  const auto tooLarge = store.value()->create(oversized);
  ASSERT_FALSE(tooLarge.ok());
  EXPECT_EQ(tooLarge.error().code, bte::core::ErrorCode::invalidArgument);
  const auto listed = store.value()->list();
  ASSERT_TRUE(listed.ok()) << listed.error().message;
  EXPECT_EQ(listed.value().size(), 1U);
}

TEST_F(ResultStoreFixture,
       reproducibilityStoreRejectsIncompleteFunctionalInputsBeforeWriting) {
  using bte::results::ProvenanceAvailabilityState;
  using bte::results::RunDescriptor;
  using DescriptorMutation = std::function<void(RunDescriptor &)>;

  auto store = bte::results::VerificationStore::open(root_ / "InvalidInputs");
  ASSERT_TRUE(store.ok()) << store.error().message;
  const auto valid = schemaThreeDescriptor();
  const std::vector<DescriptorMutation> invalidMutations{
      [](auto &value) { value.schemaVersion = 2; },
      [](auto &value) { value.universe.clear(); },
      [](auto &value) { value.range.end = value.range.start; },
      [](auto &value) { value.initialCapitalMicrodollars = 0; },
      [](auto &value) { value.strategyId.clear(); },
      [](auto &value) {
        value.provenance.strategyMode =
            bte::results::StrategyMode::selectableConditions;
      },
      [](auto &value) { value.universe.push_back("SYN"); },
      [](auto &value) { value.dataSelection.snapshotId = "invalid"; },
      [](auto &value) { value.dataSelection.calendarHash = "invalid"; },
      [](auto &value) { value.dataSelection.splitManifestHash = "invalid"; },
      [](auto &value) { value.dataSelection.timeframe.clear(); },
      [](auto &value) { value.dataSelection.profile.clear(); },
      [](auto &value) { value.provenance.engineBuild.reset(); },
      [](auto &value) { value.provenance.engineBuild = ""; },
      [](auto &value) { value.provenance.timeframe = "ohlcv-1d"; },
      [](auto &value) { value.provenance.costProfile.reset(); },
      [](auto &value) { value.provenance.costProfile = ""; },
      [](auto &value) {
        value.provenance.commissionRatePartsPerBillion.reset();
      },
      [](auto &value) { value.provenance.commissionRatePartsPerBillion = -1; },
      [](auto &value) { value.provenance.slippageRatePartsPerBillion.reset(); },
      [](auto &value) { value.provenance.slippageRatePartsPerBillion = -1; },
      [](auto &value) { value.provenance.seed.reset(); },
      [](auto &value) { value.provenance.seed = -1; },
      [](auto &value) { value.provenance.apiVersion.clear(); },
      [](auto &value) { value.provenance.templateVersion.clear(); },
      [](auto &value) { value.provenance.runtime.reset(); },
      [](auto &value) {
        value.provenance.runtime->canonicalManifestJson = "{";
        value.provenance.runtime->manifestHash =
            bte::core::sha256(value.provenance.runtime->canonicalManifestJson);
      },
      [](auto &value) {
        value.provenance.runtime->canonicalManifestJson = "[]";
        value.provenance.runtime->manifestHash =
            bte::core::sha256(value.provenance.runtime->canonicalManifestJson);
      },
      [](auto &value) {
        value.provenance.runtime->canonicalManifestJson += " ";
        value.provenance.runtime->manifestHash =
            bte::core::sha256(value.provenance.runtime->canonicalManifestJson);
      },
      [](auto &value) { value.provenance.runtime->profileId.clear(); },
      [](auto &value) { value.provenance.runtime->pythonVersion.clear(); },
      [](auto &value) { value.provenance.runtime->pythonAbi.clear(); },
      [](auto &value) {
        value.provenance.runtime->stockbtSourceHash = "invalid";
      },
      [](auto &value) { value.provenance.sourceUtf8.clear(); },
      [](auto &value) { value.provenance.sourceUtf8 = std::string(1, '\0'); },
      [](auto &value) { value.provenance.sourceUtf8 = "bad\rsource"; },
      [](auto &value) { value.provenance.sourceUtf8 = "\xef\xbb\xbfsource"; },
      [](auto &value) {
        value.provenance.sourceUtf8 = std::string(1024U * 1024U + 1U, 'x');
      },
      [](auto &value) { value.provenance.sourceHash = std::string(64, '0'); },
      [](auto &value) { value.strategyHash = std::string(64, '0'); },
      [](auto &value) { value.provenance.provenanceAvailability.clear(); },
      [](auto &value) {
        value.provenance.provenanceAvailability.begin()->second =
            static_cast<ProvenanceAvailabilityState>(99);
      },
      [](auto &value) { value.provenance.canonicalParametersJson = "{"; },
      [](auto &value) { value.provenance.canonicalParametersJson = "null"; },
      [](auto &value) {
        value.provenance.canonicalParametersJson =
            R"({"quantity":1,"fast_period":2,"slow_period":3})";
      },
      [](auto &value) {
        value.provenance.runtime->manifestHash = std::string(64, '0');
      },
      [](auto &value) {
        value.provenance.runtime->packages.front().version = "mismatched";
      },
      [](auto &value) { value.provenance.runtime->packages.clear(); },
      [](auto &value) {
        value.provenance.runtime->packages.front().name.clear();
      },
      [](auto &value) {
        value.provenance.runtime->packages.front().version.clear();
      },
      [](auto &value) {
        value.provenance.runtime->packages.front().artifactHash = "invalid";
      },
      [](auto &value) {
        value.provenance.runtime->packages.front().name =
            value.provenance.runtime->packages.back().name;
      },
      [](auto &value) { value.dataSelection.spans.clear(); },
      [](auto &value) {
        value.dataSelection.spans[1].segmentOrdinal =
            value.dataSelection.spans[0].segmentOrdinal;
      },
      [](auto &value) { value.dataSelection.spans[0].symbol = "OTHER"; },
      [](auto &value) { value.dataSelection.spans[0].segmentId = "invalid"; },
      [](auto &value) { value.dataSelection.spans[0].segmentHash = "invalid"; },
      [](auto &value) { value.dataSelection.spans[0].rowCount = 0; },
      [](auto &value) {
        value.dataSelection.spans[0].firstTimestamp =
            bte::core::time::parseIso8601("2024-01-02 00:00:00+00:00").value();
        value.dataSelection.spans[0].lastTimestamp =
            bte::core::time::parseIso8601("2024-01-01 23:00:00+00:00").value();
      },
  };

  for (const auto &mutate : invalidMutations) {
    auto invalid = valid;
    mutate(invalid);
    const auto rejected = store.value()->create(invalid);
    ASSERT_FALSE(rejected.ok());
    EXPECT_EQ(rejected.error().code, bte::core::ErrorCode::invalidArgument);
  }

  using ManifestMutation = std::function<void(nlohmann::json &)>;
  const std::vector<ManifestMutation> invalidManifestMutations{
      [](auto &manifest) { manifest["profileId"] = "mismatched-profile"; },
      [](auto &manifest) { manifest["pythonVersion"] = "3.12"; },
      [](auto &manifest) { manifest["pythonAbi"] = "cp312"; },
      [](auto &manifest) {
        manifest["stockbtSourceSha256"] = std::string(64, '0');
      },
      [](auto &manifest) { manifest["components"] = "invalid"; },
      [](auto &manifest) { manifest["components"][0] = "not-an-object"; },
      [](auto &manifest) { manifest["components"][0]["name"] = 1; },
      [](auto &manifest) { manifest["components"][0]["version"] = 1; },
      [](auto &manifest) { manifest["components"][0]["artifactSha256"] = 1; },
      [](auto &manifest) {
        manifest["components"].push_back(manifest["components"][0]);
      },
  };
  for (const auto &mutate : invalidManifestMutations) {
    auto invalid = valid;
    auto &runtime = invalid.provenance.runtime.value();
    auto parsed = bte::core::parseCanonicalJson(runtime.canonicalManifestJson);
    ASSERT_TRUE(parsed.ok()) << parsed.error().message;
    auto manifest = std::move(parsed).value();
    mutate(manifest);
    const auto canonical = bte::core::serializeCanonicalJson(manifest);
    ASSERT_TRUE(canonical.ok()) << canonical.error().message;
    runtime.canonicalManifestJson = canonical.value();
    runtime.manifestHash = bte::core::sha256(canonical.value());
    const auto rejected = store.value()->create(invalid);
    ASSERT_FALSE(rejected.ok());
    EXPECT_EQ(rejected.error().code, bte::core::ErrorCode::invalidArgument);
  }
  const auto entries = store.value()->list();
  ASSERT_TRUE(entries.ok()) << entries.error().message;
  EXPECT_TRUE(entries.value().empty());
}

TEST_F(ResultStoreFixture,
       reproducibilityCoordinatorRunsThreeFreshAttemptsSequentially) {
  using namespace bte;
  const auto directory = root_ / "VerificationCoordinator";
  const auto descriptor = schemaThreeDescriptor();
  auto resultStore = results::ResultStore::open(directory, root_ / "Data");
  ASSERT_TRUE(resultStore.ok()) << resultStore.error().message;
  auto evidenceStore = results::VerificationStore::open(directory);
  ASSERT_TRUE(evidenceStore.ok()) << evidenceStore.error().message;
  results::ReproducibilityCoordinator coordinator{*evidenceStore.value()};

  auto activeAttempts = 0;
  auto maximumActiveAttempts = 0;
  auto launches = 0;
  const auto evidence = coordinator.verify(
      descriptor,
      [this, &resultStore, &activeAttempts, &maximumActiveAttempts,
       &launches](const results::RunDescriptor &attemptDescriptor,
                  const core::CancellationToken &,
                  const results::VerificationResultIdPublisher &publishId)
          -> core::Result<results::VerificationAttemptOutput> {
        ++activeAttempts;
        maximumActiveAttempts = std::max(maximumActiveAttempts, activeAttempts);
        const auto ordinal = ++launches;
        const auto attempt =
            createSchemaThreeDraft(*resultStore.value(), attemptDescriptor);
        if (!attempt.ok()) {
          --activeAttempts;
          return attempt.error();
        }
        const auto published = publishId(attempt.value().resultId);
        if (!published.ok()) {
          --activeAttempts;
          return published.error();
        }
        --activeAttempts;
        EXPECT_EQ(ordinal, launches);
        return results::VerificationAttemptOutput{
            .descriptor = attemptDescriptor,
            .result = attempt.value(),
            .status = results::RunStatus::completed,
        };
      });

  ASSERT_TRUE(evidence.ok()) << evidence.error().message;
  EXPECT_EQ(evidence.value().status, results::VerificationStatus::verified);
  EXPECT_EQ(evidence.value().attempts.size(), 3U);
  EXPECT_EQ(launches, 3);
  EXPECT_EQ(maximumActiveAttempts, 1);
  for (const auto &attempt : evidence.value().attempts) {
    EXPECT_TRUE(attempt.resultId.has_value());
    EXPECT_EQ(attempt.terminalStatus, results::RunStatus::completed);
    EXPECT_TRUE(attempt.canonicalResultHash.has_value());
  }
  EXPECT_TRUE(resultStore.value()->list().value().empty());
  EXPECT_EQ(resultStore.value()->listDrafts().value().size(), 3U);
}

TEST_F(ResultStoreFixture,
       reproducibilityCoordinatorStopsAfterFailureAndCancellation) {
  using namespace bte;
  const auto directory = root_ / "VerificationCoordinatorOutcomes";
  const auto descriptor = schemaThreeDescriptor();
  auto resultStore = results::ResultStore::open(directory, root_ / "Data");
  ASSERT_TRUE(resultStore.ok()) << resultStore.error().message;
  auto evidenceStore = results::VerificationStore::open(directory);
  ASSERT_TRUE(evidenceStore.ok()) << evidenceStore.error().message;
  results::ReproducibilityCoordinator coordinator{*evidenceStore.value()};

  auto launches = 0;
  const auto failed = coordinator.verify(
      descriptor,
      [this, &resultStore,
       &launches](const results::RunDescriptor &attemptDescriptor,
                  const core::CancellationToken &,
                  const results::VerificationResultIdPublisher &publishId)
          -> core::Result<results::VerificationAttemptOutput> {
        ++launches;
        if (launches == 2) {
          return core::makeError(core::ErrorCode::workerStartFailed,
                                 "second worker failed to start");
        }
        const auto attempt =
            createSchemaThreeDraft(*resultStore.value(), attemptDescriptor);
        if (!attempt.ok()) {
          return attempt.error();
        }
        const auto published = publishId(attempt.value().resultId);
        if (!published.ok()) {
          return published.error();
        }
        return results::VerificationAttemptOutput{
            .descriptor = attemptDescriptor,
            .result = attempt.value(),
            .status = results::RunStatus::completed,
        };
      });
  ASSERT_TRUE(failed.ok()) << failed.error().message;
  EXPECT_EQ(failed.value().status, results::VerificationStatus::failed);
  EXPECT_EQ(failed.value().attempts.size(), 2U);
  EXPECT_EQ(launches, 2);
  EXPECT_EQ(failed.value().attempts.back().error->code,
            "attemptExecutionFailed");

  core::CancellationSource cancellation;
  launches = 0;
  const auto canceledBetween = coordinator.verify(
      descriptor,
      [this, &resultStore, &launches,
       &cancellation](const results::RunDescriptor &attemptDescriptor,
                      const core::CancellationToken &,
                      const results::VerificationResultIdPublisher &publishId)
          -> core::Result<results::VerificationAttemptOutput> {
        ++launches;
        const auto attempt =
            createSchemaThreeDraft(*resultStore.value(), attemptDescriptor);
        if (!attempt.ok()) {
          return attempt.error();
        }
        const auto published = publishId(attempt.value().resultId);
        if (!published.ok()) {
          return published.error();
        }
        cancellation.requestCancellation();
        return results::VerificationAttemptOutput{
            .descriptor = attemptDescriptor,
            .result = attempt.value(),
            .status = results::RunStatus::completed,
        };
      },
      cancellation.token());
  ASSERT_TRUE(canceledBetween.ok()) << canceledBetween.error().message;
  EXPECT_EQ(canceledBetween.value().status,
            results::VerificationStatus::canceled);
  EXPECT_EQ(canceledBetween.value().attempts.size(), 1U);
  EXPECT_EQ(launches, 1);

  core::CancellationSource duringAttempt;
  launches = 0;
  const auto canceledDuring = coordinator.verify(
      descriptor,
      [&launches, &duringAttempt](
          const results::RunDescriptor &, const core::CancellationToken &,
          const results::VerificationResultIdPublisher &)
          -> core::Result<results::VerificationAttemptOutput> {
        ++launches;
        duringAttempt.requestCancellation();
        return core::makeError(core::ErrorCode::cancelled,
                               "worker canceled before creating a Result");
      },
      duringAttempt.token());
  ASSERT_TRUE(canceledDuring.ok()) << canceledDuring.error().message;
  EXPECT_EQ(canceledDuring.value().status,
            results::VerificationStatus::canceled);
  EXPECT_EQ(canceledDuring.value().attempts.size(), 1U);
  EXPECT_EQ(canceledDuring.value().attempts.front().error->code, "canceled");
  EXPECT_EQ(launches, 1);
}

TEST_F(ResultStoreFixture,
       reproducibilityCoordinatorContainsRunnerAndObserverFailures) {
  using namespace bte;
  const auto directory = root_ / "VerificationCoordinatorFailures";
  const auto descriptor = schemaThreeDescriptor();
  auto evidenceStore = results::VerificationStore::open(directory);
  ASSERT_TRUE(evidenceStore.ok()) << evidenceStore.error().message;
  results::ReproducibilityCoordinator coordinator{*evidenceStore.value()};

  const auto missingRunner = coordinator.verify(descriptor, {});
  ASSERT_FALSE(missingRunner.ok());
  EXPECT_EQ(missingRunner.error().code, core::ErrorCode::invalidArgument);

  auto nestedError = std::optional<core::ErrorCode>{};
  const auto nested = coordinator.verify(
      descriptor,
      [&coordinator,
       &nestedError](const results::RunDescriptor &attemptDescriptor,
                     const core::CancellationToken &,
                     const results::VerificationResultIdPublisher &)
          -> core::Result<results::VerificationAttemptOutput> {
        auto competingVerification =
            std::async(std::launch::async, [&coordinator, &attemptDescriptor,
                                            &nestedError] {
              const auto inner = coordinator.verify(
                  attemptDescriptor,
                  [](const results::RunDescriptor &,
                     const core::CancellationToken &,
                     const results::VerificationResultIdPublisher &)
                      -> core::Result<results::VerificationAttemptOutput> {
                    return core::makeError(
                        core::ErrorCode::internal,
                        "The competing runner must not execute");
                  });
              if (!inner.ok()) {
                nestedError = inner.error().code;
              }
            });
        competingVerification.get();
        return core::makeError(core::ErrorCode::workerStartFailed,
                               "The outer verification stopped");
      });
  ASSERT_TRUE(nested.ok()) << nested.error().message;
  EXPECT_EQ(nested.value().status, results::VerificationStatus::failed);
  ASSERT_TRUE(nestedError.has_value());
  EXPECT_EQ(nestedError.value(), core::ErrorCode::resourceBusy);

  const auto standardException = coordinator.verify(
      descriptor,
      [](const results::RunDescriptor &, const core::CancellationToken &,
         const results::VerificationResultIdPublisher &)
          -> core::Result<results::VerificationAttemptOutput> {
        throw std::runtime_error("runner exception");
      },
      {}, [](const results::VerificationEvidence &) { throw 1; });
  ASSERT_TRUE(standardException.ok()) << standardException.error().message;
  EXPECT_EQ(standardException.value().status,
            results::VerificationStatus::failed);
  ASSERT_EQ(standardException.value().attempts.size(), 1U);
  EXPECT_NE(standardException.value().attempts.front().error->message.find(
                "runner exception"),
            std::string::npos);

  const auto unknownException = coordinator.verify(
      descriptor,
      [](const results::RunDescriptor &, const core::CancellationToken &,
         const results::VerificationResultIdPublisher &)
          -> core::Result<results::VerificationAttemptOutput> { throw 7; });
  ASSERT_TRUE(unknownException.ok()) << unknownException.error().message;
  EXPECT_EQ(unknownException.value().status,
            results::VerificationStatus::failed);
  ASSERT_EQ(unknownException.value().attempts.size(), 1U);
  EXPECT_EQ(unknownException.value().attempts.front().error->message,
            "Verification attempt raised an unknown error");

  const auto publicationFailure = coordinator.verify(
      descriptor,
      [](const results::RunDescriptor &attemptDescriptor,
         const core::CancellationToken &,
         const results::VerificationResultIdPublisher &publishId)
          -> core::Result<results::VerificationAttemptOutput> {
        const auto invalidPublication = publishId(std::string(32, 'z'));
        EXPECT_FALSE(invalidPublication.ok());
        return results::VerificationAttemptOutput{
            .descriptor = attemptDescriptor,
            .result = {.resultId = std::string(32, '1'),
                       .canonicalResultHash = std::string(64, 'a')},
            .status = results::RunStatus::completed,
        };
      });
  ASSERT_TRUE(publicationFailure.ok()) << publicationFailure.error().message;
  EXPECT_EQ(publicationFailure.value().status,
            results::VerificationStatus::failed);
  ASSERT_EQ(publicationFailure.value().attempts.size(), 1U);
  EXPECT_EQ(publicationFailure.value().attempts.front().error->message,
            "Attempt Result ID is invalid");

  core::CancellationSource cancellation;
  cancellation.requestCancellation();
  auto launches = 0;
  const auto canceledBeforeStart = coordinator.verify(
      descriptor,
      [&launches](const results::RunDescriptor &,
                  const core::CancellationToken &,
                  const results::VerificationResultIdPublisher &)
          -> core::Result<results::VerificationAttemptOutput> {
        ++launches;
        return core::makeError(core::ErrorCode::internal,
                               "A canceled runner must not execute");
      },
      cancellation.token());
  ASSERT_TRUE(canceledBeforeStart.ok()) << canceledBeforeStart.error().message;
  EXPECT_EQ(canceledBeforeStart.value().status,
            results::VerificationStatus::canceled);
  EXPECT_TRUE(canceledBeforeStart.value().attempts.empty());
  EXPECT_EQ(launches, 0);

  const auto oversizedFailure = coordinator.verify(
      descriptor,
      [](const results::RunDescriptor &, const core::CancellationToken &,
         const results::VerificationResultIdPublisher &)
          -> core::Result<results::VerificationAttemptOutput> {
        return core::makeError(core::ErrorCode::internal,
                               std::string(2049, 'x'));
      });
  ASSERT_FALSE(oversizedFailure.ok());
  EXPECT_EQ(oversizedFailure.error().code, core::ErrorCode::invalidArgument);
}

TEST_F(ResultStoreFixture,
       reproducibilityCoordinatorPropagatesVerificationStoreFailures) {
  using namespace bte;

  const auto createDirectory = root_ / "VerificationCreateFailure";
  auto createStore = results::VerificationStore::open(createDirectory);
  ASSERT_TRUE(createStore.ok()) << createStore.error().message;
  results::ReproducibilityCoordinator createCoordinator{*createStore.value()};
  auto invalidDescriptor = schemaThreeDescriptor();
  invalidDescriptor.provenance.sourceHash = std::string(64, '0');
  auto createRunnerCalls = 0;
  const auto createFailure = createCoordinator.verify(
      invalidDescriptor,
      [&createRunnerCalls](const results::RunDescriptor &,
                           const core::CancellationToken &,
                           const results::VerificationResultIdPublisher &)
          -> core::Result<results::VerificationAttemptOutput> {
        ++createRunnerCalls;
        return core::makeError(core::ErrorCode::internal,
                               "The invalid descriptor must not run");
      });
  ASSERT_FALSE(createFailure.ok());
  EXPECT_EQ(createFailure.error().code, core::ErrorCode::invalidArgument);
  EXPECT_EQ(createRunnerCalls, 0);

  const auto beginDirectory = root_ / "VerificationBeginFailure";
  auto beginStore = results::VerificationStore::open(beginDirectory);
  ASSERT_TRUE(beginStore.ok()) << beginStore.error().message;
  results::ReproducibilityCoordinator beginCoordinator{*beginStore.value()};
  auto beginEvidenceRemoved = false;
  auto beginRunnerCalls = 0;
  const auto beginFailure = beginCoordinator.verify(
      schemaThreeDescriptor(),
      [&beginRunnerCalls](const results::RunDescriptor &,
                          const core::CancellationToken &,
                          const results::VerificationResultIdPublisher &)
          -> core::Result<results::VerificationAttemptOutput> {
        ++beginRunnerCalls;
        return core::makeError(core::ErrorCode::internal,
                               "The removed evidence must not run");
      },
      {},
      [&beginDirectory,
       &beginEvidenceRemoved](const results::VerificationEvidence &evidence) {
        if (evidence.attempts.empty()) {
          beginEvidenceRemoved =
              std::filesystem::remove(beginDirectory / "Verifications" /
                                      (evidence.verificationId + ".bteverify"));
        }
      });
  ASSERT_FALSE(beginFailure.ok());
  EXPECT_EQ(beginFailure.error().code, core::ErrorCode::notFound);
  EXPECT_TRUE(beginEvidenceRemoved);
  EXPECT_EQ(beginRunnerCalls, 0);

  const auto completeDirectory = root_ / "VerificationCompleteFailure";
  auto completeStore = results::VerificationStore::open(completeDirectory);
  ASSERT_TRUE(completeStore.ok()) << completeStore.error().message;
  results::ReproducibilityCoordinator completeCoordinator{
      *completeStore.value()};
  std::string completeVerificationId;
  auto completeEvidenceCorrupted = false;
  const auto completeFailure = completeCoordinator.verify(
      schemaThreeDescriptor(),
      [&completeDirectory, &completeVerificationId, &completeEvidenceCorrupted](
          const results::RunDescriptor &attemptDescriptor,
          const core::CancellationToken &,
          const results::VerificationResultIdPublisher &)
          -> core::Result<results::VerificationAttemptOutput> {
        const auto evidencePath = completeDirectory / "Verifications" /
                                  (completeVerificationId + ".bteverify");
        std::ofstream output{evidencePath, std::ios::binary | std::ios::trunc};
        output << "{";
        output.close();
        completeEvidenceCorrupted = output.good();
        return results::VerificationAttemptOutput{
            .descriptor = attemptDescriptor,
            .result = {.resultId = std::string(32, '1'),
                       .canonicalResultHash = std::string(64, 'a')},
            .status = results::RunStatus::completed,
        };
      },
      {},
      [&completeVerificationId](const results::VerificationEvidence &evidence) {
        if (evidence.attempts.empty()) {
          completeVerificationId = evidence.verificationId;
        }
      });
  ASSERT_FALSE(completeFailure.ok());
  EXPECT_TRUE(completeEvidenceCorrupted);
  EXPECT_EQ(completeFailure.error().code, core::ErrorCode::schemaMismatch);

  const auto cancelDirectory = root_ / "VerificationCancelFailure";
  auto cancelStore = results::VerificationStore::open(cancelDirectory);
  ASSERT_TRUE(cancelStore.ok()) << cancelStore.error().message;
  results::ReproducibilityCoordinator cancelCoordinator{*cancelStore.value()};
  core::CancellationSource cancellation;
  auto cancelEvidenceRemoved = false;
  auto cancelRunnerCalls = 0;
  const auto cancelFailure = cancelCoordinator.verify(
      schemaThreeDescriptor(),
      [&cancelRunnerCalls](const results::RunDescriptor &,
                           const core::CancellationToken &,
                           const results::VerificationResultIdPublisher &)
          -> core::Result<results::VerificationAttemptOutput> {
        ++cancelRunnerCalls;
        return core::makeError(core::ErrorCode::internal,
                               "A canceled run must not start");
      },
      cancellation.token(),
      [&cancelDirectory, &cancelEvidenceRemoved,
       &cancellation](const results::VerificationEvidence &evidence) {
        if (evidence.attempts.empty()) {
          cancelEvidenceRemoved =
              std::filesystem::remove(cancelDirectory / "Verifications" /
                                      (evidence.verificationId + ".bteverify"));
          cancellation.requestCancellation();
        }
      });
  ASSERT_FALSE(cancelFailure.ok());
  EXPECT_EQ(cancelFailure.error().code, core::ErrorCode::notFound);
  EXPECT_TRUE(cancelEvidenceRemoved);
  EXPECT_EQ(cancelRunnerCalls, 0);
}

TEST_F(ResultStoreFixture,
       reproducibilityVerificationRequiresThreeIdenticalCompletedResults) {
  const auto directory = root_ / "VerificationMatching";
  const auto value = schemaThreeDescriptor();
  auto results = bte::results::ResultStore::open(directory, root_ / "Data");
  ASSERT_TRUE(results.ok()) << results.error().message;
  auto verifications = bte::results::VerificationStore::open(directory);
  ASSERT_TRUE(verifications.ok()) << verifications.error().message;
  auto created = verifications.value()->create(value);
  ASSERT_TRUE(created.ok()) << created.error().message;

  for (std::size_t index = 0; index < 3; ++index) {
    const auto attemptResult = createSchemaThreeResult(
        *results.value(), value, bte::results::RunStatus::completed,
        "same functional result");
    ASSERT_TRUE(attemptResult.ok()) << attemptResult.error().message;
    auto started =
        verifications.value()->beginAttempt(created.value().verificationId);
    ASSERT_TRUE(started.ok()) << started.error().message;
    EXPECT_EQ(started.value().progressOrdinal, index + 1);
    auto attached = verifications.value()->attachResultId(
        created.value().verificationId, attemptResult.value().resultId);
    ASSERT_TRUE(attached.ok()) << attached.error().message;
    auto completed = verifications.value()->completeAttempt(
        created.value().verificationId, attemptResult.value().descriptor,
        asFinalized(attemptResult.value()), attemptResult.value().status);
    ASSERT_TRUE(completed.ok()) << completed.error().message;
    created = std::move(completed);
    if (index < 2) {
      EXPECT_EQ(created.value().status,
                bte::results::VerificationStatus::running);
    }
  }
  EXPECT_EQ(created.value().status, bte::results::VerificationStatus::verified);
  EXPECT_EQ(created.value().attempts.size(), 3U);
  const auto matches = verifications.value()->hasVerifiedEvidence(value);
  ASSERT_TRUE(matches.ok()) << matches.error().message;
  EXPECT_TRUE(matches.value());

  const auto verifiedResult = bte::results::FinalizedResult{
      .resultId = created.value().attempts.front().resultId.value(),
      .canonicalResultHash =
          created.value().attempts.front().canonicalResultHash.value(),
  };
  const auto resultMatches =
      verifications.value()->hasVerifiedResult(value, verifiedResult);
  ASSERT_TRUE(resultMatches.ok()) << resultMatches.error().message;
  EXPECT_TRUE(resultMatches.value());

  auto differentResult = verifiedResult;
  differentResult.canonicalResultHash = std::string(64, '0');
  const auto differentHash =
      verifications.value()->hasVerifiedResult(value, differentResult);
  ASSERT_TRUE(differentHash.ok()) << differentHash.error().message;
  EXPECT_FALSE(differentHash.value());
  differentResult.resultId = std::string(32, 'z');
  const auto invalidResultId =
      verifications.value()->hasVerifiedResult(value, differentResult);
  ASSERT_FALSE(invalidResultId.ok());
  EXPECT_EQ(invalidResultId.error().code,
            bte::core::ErrorCode::invalidArgument);

  auto changedSource = value;
  changedSource.provenance.sourceUtf8 += "# changed input\n";
  changedSource.provenance.sourceHash =
      bte::core::sha256(changedSource.provenance.sourceUtf8);
  changedSource.strategyHash = changedSource.provenance.sourceHash;
  auto sourceMismatch =
      verifications.value()->hasVerifiedEvidence(changedSource);
  ASSERT_TRUE(sourceMismatch.ok()) << sourceMismatch.error().message;
  EXPECT_FALSE(sourceMismatch.value());

  auto changedParameters = value;
  changedParameters.provenance.canonicalParametersJson =
      R"({"fast_period":2,"quantity":2,"slow_period":3})";
  auto parameterMismatch =
      verifications.value()->hasVerifiedEvidence(changedParameters);
  ASSERT_TRUE(parameterMismatch.ok()) << parameterMismatch.error().message;
  EXPECT_FALSE(parameterMismatch.value());

  auto changedSeed = value;
  changedSeed.provenance.seed = 1;
  auto seedMismatch = verifications.value()->hasVerifiedEvidence(changedSeed);
  ASSERT_TRUE(seedMismatch.ok()) << seedMismatch.error().message;
  EXPECT_FALSE(seedMismatch.value());

  auto changedRuntime = value;
  auto manifest = bte::core::parseCanonicalJson(
      changedRuntime.provenance.runtime->canonicalManifestJson);
  ASSERT_TRUE(manifest.ok()) << manifest.error().message;
  auto manifestValue = std::move(manifest).value();
  manifestValue["profileId"] = "fixture-profile-v2";
  auto canonicalManifest = bte::core::serializeCanonicalJson(manifestValue);
  ASSERT_TRUE(canonicalManifest.ok()) << canonicalManifest.error().message;
  changedRuntime.provenance.runtime->profileId = "fixture-profile-v2";
  changedRuntime.provenance.runtime->canonicalManifestJson =
      canonicalManifest.value();
  changedRuntime.provenance.runtime->manifestHash =
      bte::core::sha256(canonicalManifest.value());
  auto runtimeMismatch =
      verifications.value()->hasVerifiedEvidence(changedRuntime);
  ASSERT_TRUE(runtimeMismatch.ok()) << runtimeMismatch.error().message;
  EXPECT_FALSE(runtimeMismatch.value());
}

TEST_F(ResultStoreFixture,
       reproducibilityVerificationStopsAtMismatchFailureAndCancellation) {
  const auto directory = root_ / "VerificationOutcomes";
  const auto value = schemaThreeDescriptor();
  auto results = bte::results::ResultStore::open(directory, root_ / "Data");
  ASSERT_TRUE(results.ok()) << results.error().message;
  auto verifications = bte::results::VerificationStore::open(directory);
  ASSERT_TRUE(verifications.ok()) << verifications.error().message;

  auto mismatch = verifications.value()->create(value);
  ASSERT_TRUE(mismatch.ok()) << mismatch.error().message;
  const auto firstResult = createSchemaThreeResult(
      *results.value(), value, bte::results::RunStatus::completed, "first");
  ASSERT_TRUE(firstResult.ok()) << firstResult.error().message;
  ASSERT_TRUE(verifications.value()
                  ->beginAttempt(mismatch.value().verificationId)
                  .ok());
  ASSERT_TRUE(verifications.value()
                  ->completeAttempt(mismatch.value().verificationId,
                                    firstResult.value().descriptor,
                                    asFinalized(firstResult.value()),
                                    firstResult.value().status)
                  .ok());
  const auto differentResult = createSchemaThreeResult(
      *results.value(), value, bte::results::RunStatus::completed, "second");
  ASSERT_TRUE(differentResult.ok()) << differentResult.error().message;
  ASSERT_NE(firstResult.value().canonicalResultHash,
            differentResult.value().canonicalResultHash);
  ASSERT_TRUE(verifications.value()
                  ->beginAttempt(mismatch.value().verificationId)
                  .ok());
  const auto mismatched = verifications.value()->completeAttempt(
      mismatch.value().verificationId, differentResult.value().descriptor,
      asFinalized(differentResult.value()), differentResult.value().status);
  ASSERT_TRUE(mismatched.ok()) << mismatched.error().message;
  EXPECT_EQ(mismatched.value().status,
            bte::results::VerificationStatus::mismatch);
  EXPECT_FALSE(verifications.value()
                   ->beginAttempt(mismatch.value().verificationId)
                   .ok());

  auto failed = verifications.value()->create(value);
  ASSERT_TRUE(failed.ok()) << failed.error().message;
  ASSERT_TRUE(
      verifications.value()->beginAttempt(failed.value().verificationId).ok());
  const auto overlongError = verifications.value()->failAttempt(
      failed.value().verificationId, std::string(129, 'e'), "too long");
  ASSERT_FALSE(overlongError.ok());
  EXPECT_EQ(overlongError.error().code, bte::core::ErrorCode::invalidArgument);
  const auto stillRunning =
      verifications.value()->load(failed.value().verificationId);
  ASSERT_TRUE(stillRunning.ok()) << stillRunning.error().message;
  EXPECT_EQ(stillRunning.value().status,
            bte::results::VerificationStatus::running);
  ASSERT_EQ(stillRunning.value().attempts.size(), 1U);
  EXPECT_FALSE(stillRunning.value().attempts.front().error.has_value());
  const auto failedAttempt = verifications.value()->failAttempt(
      failed.value().verificationId, "workerStartFailed",
      "Managed Python worker could not start");
  ASSERT_TRUE(failedAttempt.ok()) << failedAttempt.error().message;
  EXPECT_EQ(failedAttempt.value().status,
            bte::results::VerificationStatus::failed);
  ASSERT_EQ(failedAttempt.value().attempts.size(), 1U);
  EXPECT_EQ(failedAttempt.value().attempts.front().error->code,
            "workerStartFailed");

  auto failedSecond = verifications.value()->create(value);
  ASSERT_TRUE(failedSecond.ok()) << failedSecond.error().message;
  ASSERT_TRUE(verifications.value()
                  ->beginAttempt(failedSecond.value().verificationId)
                  .ok());
  ASSERT_TRUE(verifications.value()
                  ->completeAttempt(failedSecond.value().verificationId,
                                    firstResult.value().descriptor,
                                    asFinalized(firstResult.value()),
                                    firstResult.value().status)
                  .ok());
  ASSERT_TRUE(verifications.value()
                  ->beginAttempt(failedSecond.value().verificationId)
                  .ok());
  const auto secondFailure = verifications.value()->failAttempt(
      failedSecond.value().verificationId, "strategyHookTimeout",
      "The second Strategy callback exceeded its deadline");
  ASSERT_TRUE(secondFailure.ok()) << secondFailure.error().message;
  EXPECT_EQ(secondFailure.value().status,
            bte::results::VerificationStatus::failed);
  EXPECT_EQ(secondFailure.value().attempts.size(), 2U);
  EXPECT_FALSE(verifications.value()
                   ->beginAttempt(failedSecond.value().verificationId)
                   .ok());

  auto canceledBetween = verifications.value()->create(value);
  ASSERT_TRUE(canceledBetween.ok()) << canceledBetween.error().message;
  ASSERT_TRUE(verifications.value()
                  ->beginAttempt(canceledBetween.value().verificationId)
                  .ok());
  ASSERT_TRUE(verifications.value()
                  ->completeAttempt(canceledBetween.value().verificationId,
                                    firstResult.value().descriptor,
                                    asFinalized(firstResult.value()),
                                    firstResult.value().status)
                  .ok());
  const auto canceled =
      verifications.value()->cancel(canceledBetween.value().verificationId);
  ASSERT_TRUE(canceled.ok()) << canceled.error().message;
  EXPECT_EQ(canceled.value().status,
            bte::results::VerificationStatus::canceled);
  EXPECT_FALSE(verifications.value()
                   ->beginAttempt(canceledBetween.value().verificationId)
                   .ok());

  auto canceledDuring = verifications.value()->create(value);
  ASSERT_TRUE(canceledDuring.ok()) << canceledDuring.error().message;
  ASSERT_TRUE(verifications.value()
                  ->beginAttempt(canceledDuring.value().verificationId)
                  .ok());
  const auto canceledResult = createSchemaThreeResult(
      *results.value(), value, bte::results::RunStatus::canceled,
      "cancel requested");
  ASSERT_TRUE(canceledResult.ok()) << canceledResult.error().message;
  const auto canceledAttempt = verifications.value()->completeAttempt(
      canceledDuring.value().verificationId, canceledResult.value().descriptor,
      asFinalized(canceledResult.value()), canceledResult.value().status);
  ASSERT_TRUE(canceledAttempt.ok()) << canceledAttempt.error().message;
  EXPECT_EQ(canceledAttempt.value().status,
            bte::results::VerificationStatus::canceled);
  const auto reopenedCanceled =
      verifications.value()->load(canceledDuring.value().verificationId);
  ASSERT_TRUE(reopenedCanceled.ok()) << reopenedCanceled.error().message;
  EXPECT_EQ(reopenedCanceled.value().status,
            bte::results::VerificationStatus::canceled);
  ASSERT_EQ(reopenedCanceled.value().attempts.size(), 1U);
  EXPECT_EQ(reopenedCanceled.value().attempts.front().terminalStatus,
            bte::results::RunStatus::canceled);
}

TEST_F(ResultStoreFixture, verificationStoreRejectsSymlinkedLockFile) {
#if defined(__APPLE__) || defined(__linux__)
  const auto resultStoreRoot = root_ / "VerificationSymlinkLock";
  const auto directory = resultStoreRoot / "Verifications";
  std::filesystem::create_directories(directory);
  const auto target = root_ / "VerificationLockTarget";
  {
    std::ofstream contents{target};
    ASSERT_TRUE(contents.is_open());
    contents << "preserve target";
  }
  const auto lockPath = directory / ".verification-store.lock";
  std::filesystem::create_symlink(target, lockPath);

  const auto store = bte::results::VerificationStore::open(resultStoreRoot);
  ASSERT_TRUE(store.ok()) << store.error().message;
  const auto created = store.value()->create(schemaThreeDescriptor());

  ASSERT_FALSE(created.ok());
  EXPECT_EQ(created.error().code, bte::core::ErrorCode::permissionDenied);
  EXPECT_TRUE(std::filesystem::is_symlink(lockPath));
  std::ifstream preserved{target};
  ASSERT_TRUE(preserved.is_open());
  const auto preservedContents =
      std::string{std::istreambuf_iterator<char>{preserved},
                  std::istreambuf_iterator<char>{}};
  EXPECT_EQ(preservedContents, "preserve target");
#else
  GTEST_SKIP() << "The verification lock uses O_NOFOLLOW on POSIX.";
#endif
}

TEST_F(ResultStoreFixture,
       reproducibilityAttemptResultIdAssignmentsAreStableAndValidated) {
  const auto directory = root_ / "VerificationResultIdAssignment";
  auto store = bte::results::VerificationStore::open(directory);
  ASSERT_TRUE(store.ok()) << store.error().message;
  auto evidence = store.value()->create(schemaThreeDescriptor());
  ASSERT_TRUE(evidence.ok()) << evidence.error().message;
  ASSERT_TRUE(
      store.value()->beginAttempt(evidence.value().verificationId).ok());

  const auto invalidId =
      store.value()->attachResultId(evidence.value().verificationId, "invalid");
  ASSERT_FALSE(invalidId.ok());
  EXPECT_EQ(invalidId.error().code, bte::core::ErrorCode::invalidArgument);

  const auto assignedId = std::string(32, '1');
  const auto attached = store.value()->attachResultId(
      evidence.value().verificationId, assignedId);
  ASSERT_TRUE(attached.ok()) << attached.error().message;
  const auto sameId = store.value()->attachResultId(
      evidence.value().verificationId, assignedId);
  ASSERT_TRUE(sameId.ok()) << sameId.error().message;
  const auto conflictingId = store.value()->attachResultId(
      evidence.value().verificationId, std::string(32, '2'));
  ASSERT_FALSE(conflictingId.ok());
  EXPECT_EQ(conflictingId.error().code, bte::core::ErrorCode::invalidArgument);

  const auto persisted = store.value()->load(evidence.value().verificationId);
  ASSERT_TRUE(persisted.ok()) << persisted.error().message;
  ASSERT_EQ(persisted.value().attempts.size(), 1U);
  EXPECT_EQ(persisted.value().attempts.front().resultId, assignedId);
  EXPECT_FALSE(persisted.value().attempts.front().terminalStatus.has_value());
}

TEST_F(ResultStoreFixture,
       reproducibilityCompletionPersistsMismatchedAttemptResultIdentity) {
  const auto directory = root_ / "VerificationResultIdMismatch";
  const auto value = schemaThreeDescriptor();
  auto store = bte::results::VerificationStore::open(directory);
  ASSERT_TRUE(store.ok()) << store.error().message;
  auto evidence = store.value()->create(value);
  ASSERT_TRUE(evidence.ok()) << evidence.error().message;
  ASSERT_TRUE(
      store.value()->beginAttempt(evidence.value().verificationId).ok());
  const auto assignedId = std::string(32, '1');
  ASSERT_TRUE(store.value()
                  ->attachResultId(evidence.value().verificationId, assignedId)
                  .ok());

  const auto mismatched = store.value()->completeAttempt(
      evidence.value().verificationId, value,
      {.resultId = std::string(32, '2'),
       .canonicalResultHash = std::string(64, 'a')},
      bte::results::RunStatus::completed);
  ASSERT_TRUE(mismatched.ok()) << mismatched.error().message;
  EXPECT_EQ(mismatched.value().status,
            bte::results::VerificationStatus::mismatch);
  ASSERT_EQ(mismatched.value().attempts.size(), 1U);
  EXPECT_EQ(mismatched.value().attempts.front().resultId, assignedId);
  EXPECT_EQ(mismatched.value().attempts.front().terminalStatus,
            bte::results::RunStatus::completed);
  ASSERT_TRUE(mismatched.value().attempts.front().error.has_value());
  EXPECT_EQ(mismatched.value().attempts.front().error->code,
            "attemptResultIdMismatch");

  const auto reopened = store.value()->load(evidence.value().verificationId);
  ASSERT_TRUE(reopened.ok()) << reopened.error().message;
  EXPECT_EQ(reopened.value().status,
            bte::results::VerificationStatus::mismatch);
  EXPECT_EQ(reopened.value().attempts.front().resultId, assignedId);
  ASSERT_TRUE(reopened.value().attempts.front().error.has_value());
  EXPECT_EQ(reopened.value().attempts.front().error->code,
            "attemptResultIdMismatch");
}

TEST_F(ResultStoreFixture,
       reproducibilityPersistsMaximumFailureDetailsAndIncompleteRunStatus) {
  const auto directory = root_ / "VerificationTerminalBoundaries";
  const auto value = schemaThreeDescriptor();
  auto store = bte::results::VerificationStore::open(directory);
  ASSERT_TRUE(store.ok()) << store.error().message;

  auto failed = store.value()->create(value);
  ASSERT_TRUE(failed.ok()) << failed.error().message;
  ASSERT_TRUE(store.value()->beginAttempt(failed.value().verificationId).ok());
  const auto maxDetails =
      store.value()->failAttempt(failed.value().verificationId,
                                 std::string(128, 'e'), std::string(2048, 'x'));
  ASSERT_TRUE(maxDetails.ok()) << maxDetails.error().message;
  const auto persistedFailure =
      store.value()->load(failed.value().verificationId);
  ASSERT_TRUE(persistedFailure.ok()) << persistedFailure.error().message;
  ASSERT_TRUE(persistedFailure.value().attempts.front().error.has_value());
  EXPECT_EQ(persistedFailure.value().attempts.front().error->code.size(), 128U);
  EXPECT_EQ(persistedFailure.value().attempts.front().error->message.size(),
            2048U);

  auto incomplete = store.value()->create(value);
  ASSERT_TRUE(incomplete.ok()) << incomplete.error().message;
  ASSERT_TRUE(
      store.value()->beginAttempt(incomplete.value().verificationId).ok());
  const auto incompleteResult = store.value()->completeAttempt(
      incomplete.value().verificationId, value,
      {.resultId = std::string(32, '3'),
       .canonicalResultHash = std::string(64, 'b')},
      bte::results::RunStatus::incomplete);
  ASSERT_TRUE(incompleteResult.ok()) << incompleteResult.error().message;
  EXPECT_EQ(incompleteResult.value().status,
            bte::results::VerificationStatus::failed);
  ASSERT_EQ(incompleteResult.value().attempts.size(), 1U);
  EXPECT_EQ(incompleteResult.value().attempts.front().terminalStatus,
            bte::results::RunStatus::incomplete);
  const auto reopenedIncomplete =
      store.value()->load(incomplete.value().verificationId);
  ASSERT_TRUE(reopenedIncomplete.ok()) << reopenedIncomplete.error().message;
  ASSERT_EQ(reopenedIncomplete.value().attempts.size(), 1U);
  EXPECT_EQ(reopenedIncomplete.value().attempts.front().terminalStatus,
            bte::results::RunStatus::incomplete);
}

TEST_F(
    ResultStoreFixture,
    reproducibilityVerificationRecoversInterruptedAndPreservesCorruptEvidence) {
  const auto directory = root_ / "VerificationRecovery";
  const auto value = schemaThreeDescriptor();
  auto results = bte::results::ResultStore::open(directory, root_ / "Data");
  ASSERT_TRUE(results.ok()) << results.error().message;
  auto verifications = bte::results::VerificationStore::open(directory);
  ASSERT_TRUE(verifications.ok()) << verifications.error().message;
  auto evidence = verifications.value()->create(value);
  ASSERT_TRUE(evidence.ok()) << evidence.error().message;
  const auto attemptResult = createSchemaThreeResult(
      *results.value(), value, bte::results::RunStatus::completed, "attempt");
  ASSERT_TRUE(attemptResult.ok()) << attemptResult.error().message;
  ASSERT_TRUE(verifications.value()
                  ->beginAttempt(evidence.value().verificationId)
                  .ok());
  EXPECT_FALSE(verifications.value()
                   ->beginAttempt(evidence.value().verificationId)
                   .ok());
  ASSERT_TRUE(verifications.value()
                  ->attachResultId(evidence.value().verificationId,
                                   attemptResult.value().resultId)
                  .ok());

  auto reopened = bte::results::VerificationStore::open(directory);
  ASSERT_TRUE(reopened.ok()) << reopened.error().message;
  const auto recovered = reopened.value()->recoverInterrupted();
  ASSERT_TRUE(recovered.ok()) << recovered.error().message;
  ASSERT_EQ(recovered.value().size(), 1U);
  EXPECT_EQ(recovered.value().front().status,
            bte::results::VerificationStatus::interrupted);
  ASSERT_EQ(recovered.value().front().attempts.size(), 1U);
  EXPECT_EQ(recovered.value().front().attempts.front().resultId,
            attemptResult.value().resultId);
  EXPECT_EQ(recovered.value().front().attempts.front().terminalStatus,
            bte::results::RunStatus::interrupted);
  EXPECT_FALSE(
      reopened.value()->beginAttempt(evidence.value().verificationId).ok());
  EXPECT_TRUE(reopened.value()->recoverInterrupted().value().empty());

  const auto evidencePath = directory / "Verifications" /
                            (evidence.value().verificationId + ".bteverify");
  std::ifstream input{evidencePath, std::ios::binary};
  const std::string originalBytes{std::istreambuf_iterator<char>{input},
                                  std::istreambuf_iterator<char>{}};
  input.close();
  auto corruptedBytes = originalBytes;
  ASSERT_FALSE(corruptedBytes.empty());
  corruptedBytes.back() = 'x';
  std::ofstream output{evidencePath, std::ios::binary | std::ios::trunc};
  output.write(corruptedBytes.data(),
               static_cast<std::streamsize>(corruptedBytes.size()));
  output.close();

  const auto corruptRead =
      reopened.value()->load(evidence.value().verificationId);
  ASSERT_FALSE(corruptRead.ok());
  EXPECT_EQ(corruptRead.error().code, bte::core::ErrorCode::schemaMismatch);
  std::ifstream preservedFile{evidencePath, std::ios::binary};
  const std::string preservedBytes{
      std::istreambuf_iterator<char>{preservedFile},
      std::istreambuf_iterator<char>{}};
  EXPECT_EQ(preservedBytes, corruptedBytes);
  const auto entries = reopened.value()->list();
  ASSERT_TRUE(entries.ok()) << entries.error().message;
  ASSERT_EQ(entries.value().size(), 1U);
  EXPECT_FALSE(entries.value().front().evidence.has_value());
  EXPECT_FALSE(entries.value().front().diagnostic.empty());
}

TEST_F(ResultStoreFixture,
       reproducibilityRecoveryPreservesAnAttemptWithoutPublishedResultId) {
  const auto directory = root_ / "VerificationRecoveryWithoutResult";
  auto store = bte::results::VerificationStore::open(directory);
  ASSERT_TRUE(store.ok()) << store.error().message;
  auto evidence = store.value()->create(schemaThreeDescriptor());
  ASSERT_TRUE(evidence.ok()) << evidence.error().message;
  const auto verificationId = evidence.value().verificationId;
  const auto started = store.value()->beginAttempt(verificationId);
  ASSERT_TRUE(started.ok()) << started.error().message;
  ASSERT_FALSE(started.value().attempts.back().resultId.has_value());

  auto reopened = bte::results::VerificationStore::open(directory);
  ASSERT_TRUE(reopened.ok()) << reopened.error().message;
  const auto recovered = reopened.value()->recoverInterrupted();

  ASSERT_TRUE(recovered.ok()) << recovered.error().message;
  ASSERT_EQ(recovered.value().size(), 1U);
  EXPECT_EQ(recovered.value().front().status,
            bte::results::VerificationStatus::interrupted);
  ASSERT_EQ(recovered.value().front().attempts.size(), 1U);
  const auto &attempt = recovered.value().front().attempts.front();
  EXPECT_FALSE(attempt.resultId.has_value());
  EXPECT_FALSE(attempt.terminalStatus.has_value());
  ASSERT_TRUE(attempt.error.has_value());
  EXPECT_EQ(attempt.error->code, "applicationInterrupted");

  const auto persisted = reopened.value()->load(verificationId);
  ASSERT_TRUE(persisted.ok()) << persisted.error().message;
  ASSERT_EQ(persisted.value().attempts.size(), 1U);
  EXPECT_FALSE(persisted.value().attempts.front().resultId.has_value());
  EXPECT_FALSE(persisted.value().attempts.front().terminalStatus.has_value());
  ASSERT_TRUE(persisted.value().attempts.front().error.has_value());
  EXPECT_EQ(persisted.value().attempts.front().error->code,
            "applicationInterrupted");
}

TEST_F(ResultStoreFixture,
       reproducibilityParserRejectsRehashedInvalidEvidenceStates) {
  using EvidenceMutation = std::function<void(nlohmann::json &)>;
  const auto directory = root_ / "VerificationInvalidEvidence";
  auto verifications = bte::results::VerificationStore::open(directory);
  ASSERT_TRUE(verifications.ok()) << verifications.error().message;
  const auto descriptor = schemaThreeDescriptor();
  const std::vector<EvidenceMutation> invalidMutations{
      [](auto &document) { document["schemaVersion"] = 2; },
      [](auto &document) { document["state"] = "unknown"; },
      [](auto &document) { document["state"] = 1; },
      [](auto &document) { document["progressOrdinal"] = 0; },
      [](auto &document) { document["progressOrdinal"] = 4; },
      [](auto &document) { document["verificationId"] = std::string(32, '1'); },
      [](auto &document) { document["identity"] = nlohmann::json::array(); },
      [](auto &document) { document["identityHash"] = std::string(64, '0'); },
      [](auto &document) { document["state"] = "verified"; },
      [](auto &document) { document["attempts"][0]["attemptOrdinal"] = 2; },
      [](auto &document) {
        document["attempts"][0]["resultId"] = std::string(33, '1');
      },
      [](auto &document) {
        document["attempts"][0]["resultId"] = std::string(32, 'z');
      },
      [](auto &document) { document["attempts"][0]["resultId"] = 1; },
      [](auto &document) {
        document["attempts"][0]["canonicalResultHash"] = "invalid";
      },
      [](auto &document) {
        document["attempts"][0]["canonicalResultHash"] = std::string(65, 'a');
      },
      [](auto &document) {
        document["attempts"][0]["terminalStatus"] = "running";
      },
      [](auto &document) {
        document["attempts"][0]["terminalStatus"] = std::string(33, 'x');
      },
      [](auto &document) {
        document["attempts"][0]["error"] = {{"code", 1},
                                            {"message", "failure"}};
      },
      [](auto &document) {
        document["attempts"][0]["error"] = {{"code", "only one field"}};
      },
      [](auto &document) {
        document["attempts"][0]["resultId"] = std::string(32, '1');
        document["attempts"][0]["terminalStatus"] = "completed";
      },
      [](auto &document) {
        document["attempts"][0]["terminalStatus"] = "failed";
      },
      [](auto &document) {
        document["attempts"][0]["error"] = {{"code", ""},
                                            {"message", "failure"}};
      },
      [](auto &document) {
        document["attempts"][0]["error"] = {
            {"code", "workerFailed"}, {"message", std::string(2049, 'x')}};
      },
      [](auto &document) {
        document["attempts"][0]["error"] = {
            {"code", "stillRunning"}, {"message", "attempt has not ended"}};
      },
      [](auto &document) {
        document["attempts"][0]["resultId"] = std::string(32, '1');
        document["attempts"][0]["terminalStatus"] = "failed";
      },
      [](auto &document) {
        document["attempts"].push_back(document["attempts"][0]);
        document["attempts"][0]["attemptOrdinal"] = 1;
        document["attempts"][1]["attemptOrdinal"] = 2;
        document["progressOrdinal"] = 2;
      },
  };

  for (const auto &mutate : invalidMutations) {
    const auto created = verifications.value()->create(descriptor);
    ASSERT_TRUE(created.ok()) << created.error().message;
    const auto started =
        verifications.value()->beginAttempt(created.value().verificationId);
    ASSERT_TRUE(started.ok()) << started.error().message;
    const auto evidencePath = directory / "Verifications" /
                              (created.value().verificationId + ".bteverify");
    ASSERT_TRUE(rewriteVerificationEvidence(evidencePath, mutate));

    const auto rejected =
        verifications.value()->load(created.value().verificationId);
    ASSERT_FALSE(rejected.ok());
    EXPECT_EQ(rejected.error().code, bte::core::ErrorCode::schemaMismatch);
  }
  const auto listed = verifications.value()->list();
  ASSERT_TRUE(listed.ok()) << listed.error().message;
  EXPECT_EQ(listed.value().size(), invalidMutations.size());
  EXPECT_TRUE(std::ranges::all_of(listed.value(), [](const auto &entry) {
    return !entry.evidence.has_value() && !entry.diagnostic.empty();
  }));
}

TEST_F(ResultStoreFixture,
       schemaThreePersistsTypedPythonProvenanceAndMatchesIndependentHash) {
  const auto value = schemaThreeDescriptor();
  auto store =
      bte::results::ResultStore::open(root_ / "SchemaThree", root_ / "Data");
  ASSERT_TRUE(store.ok()) << store.error().message;
  auto writer = store.value()->begin(value);
  ASSERT_TRUE(writer.ok()) << writer.error().message;
  ASSERT_TRUE(writer.value()->append(records()).ok());
  const auto summary = bte::results::FinalSummary{
      .finalEquityMicrodollars = 100'009'900'000,
      .pnlMicrodollars = 9'900,
  };

  const auto finalized = writer.value()->finalizeAndPromote(
      bte::results::RunStatus::completed, summary, "oracle-v3");

  ASSERT_TRUE(finalized.ok()) << finalized.error().message;
  EXPECT_EQ(finalized.value().canonicalResultHash,
            "75cb1b1a021d1c7036f30dcc697e1fbfb19f4adce361656cf409432bb5784363");
  const auto opened = store.value()->openResult(finalized.value().resultId);
  ASSERT_TRUE(opened.ok()) << opened.error().message;
  EXPECT_EQ(opened.value().schemaVersion, 3);
  EXPECT_EQ(opened.value().descriptor, value);
  EXPECT_EQ(opened.value().descriptor.provenance.runtime,
            value.provenance.runtime);
  EXPECT_EQ(opened.value().records, records());
  EXPECT_EQ(opened.value().summary, summary);
  EXPECT_TRUE(opened.value().capabilities.strategyLogs);
}

TEST_F(ResultStoreFixture,
       schemaThreeReopensPythonDiagnosticsAndTheirCompletenessState) {
  auto value = schemaThreeDescriptor();
  auto store =
      bte::results::ResultStore::open(root_ / "Diagnostics", root_ / "Data");
  ASSERT_TRUE(store.ok()) << store.error().message;
  auto writer = store.value()->begin(value);
  ASSERT_TRUE(writer.ok()) << writer.error().message;
  const std::vector<bte::results::CanonicalRecord> diagnostics{
      {.sequence = 0,
       .timestamp = timestamp("2024-01-01 23:00:00+00:00"),
       .symbol = "SYN",
       .family = bte::results::RecordFamily::log,
       .text =
           R"({"fields":{"phase":"decision"},"hook":"on_market","level":"warning","message":"before failure"})"},
      {.sequence = 1,
       .timestamp = timestamp("2024-01-01 23:00:00+00:00"),
       .symbol = "SYN",
       .family = bte::results::RecordFamily::terminalDiagnostic,
       .text =
           R"({"errorCode":"strategyRuntimeError","hook":"on_market","message":"deliberate failure","omittedBytes":0,"omittedEntries":0,"sourceColumn":null,"sourceLine":12,"traceback":"Traceback (most recent call last):\n  File \"strategy.py\", line 12\nValueError: deliberate failure"})"},
  };
  ASSERT_TRUE(writer.value()->append(diagnostics).ok());

  const auto finalized = writer.value()->finalizeAndPromote(
      bte::results::RunStatus::failed,
      bte::results::FinalSummary{.finalEquityMicrodollars = std::nullopt,
                                 .diagnosticsComplete = false},
      "deliberate failure");

  ASSERT_TRUE(finalized.ok()) << finalized.error().message;
  const auto reopened = store.value()->openResult(finalized.value().resultId);
  ASSERT_TRUE(reopened.ok()) << reopened.error().message;
  EXPECT_EQ(reopened.value().records, diagnostics);
  EXPECT_EQ(reopened.value().summary.diagnosticsComplete, false);
  EXPECT_TRUE(reopened.value().capabilities.strategyLogs);
  EXPECT_EQ(reopened.value().canonicalResultHash,
            finalized.value().canonicalResultHash);
}

TEST_F(ResultStoreFixture, schemaThreeRejectsMalformedProvenanceBeforeStaging) {
  const auto store = bte::results::ResultStore::open(
      root_ / "SchemaThreeInvalid", root_ / "Data");
  ASSERT_TRUE(store.ok()) << store.error().message;
  auto invalidSourceHash = schemaThreeDescriptor();
  invalidSourceHash.provenance.sourceHash = std::string(64, '0');
  EXPECT_FALSE(store.value()->begin(invalidSourceHash).ok());
  EXPECT_TRUE(
      std::filesystem::is_empty(root_ / "SchemaThreeInvalid" / "Staging"));

  auto invalidManifest = schemaThreeDescriptor();
  invalidManifest.provenance.runtime->manifestHash = std::string(64, '0');
  EXPECT_FALSE(store.value()->begin(invalidManifest).ok());

  auto invalidParameters = schemaThreeDescriptor();
  invalidParameters.provenance.canonicalParametersJson =
      R"({"quantity":1,"fast_period":2,"slow_period":3})";
  EXPECT_FALSE(store.value()->begin(invalidParameters).ok());

  auto invalidPackage = schemaThreeDescriptor();
  invalidPackage.provenance.runtime->packages.front().artifactHash =
      std::string(64, '0');
  EXPECT_FALSE(store.value()->begin(invalidPackage).ok());
}

TEST_F(ResultStoreFixture,
       schemaThreeRejectsInvalidRuntimeAndStrategyContractsBeforeStaging) {
  using bte::results::ProvenanceAvailabilityState;
  using bte::results::RunDescriptor;
  using DescriptorMutation = std::function<void(RunDescriptor &)>;
  using ManifestMutation = std::function<void(nlohmann::json &)>;

  auto store = bte::results::ResultStore::open(
      root_ / "SchemaThreeRuntimeInvalid", root_ / "Data");
  ASSERT_TRUE(store.ok()) << store.error().message;
  const auto expectRejected = [&store](const RunDescriptor &invalid) {
    const auto rejected = store.value()->begin(invalid);
    EXPECT_FALSE(rejected.ok());
    if (!rejected.ok()) {
      EXPECT_EQ(rejected.error().code, bte::core::ErrorCode::invalidArgument);
    }
  };
  const std::vector<DescriptorMutation> invalidDescriptors{
      [](auto &value) { value.provenance.timeframe = "ohlcv-1d"; },
      [](auto &value) { value.provenance.engineBuild.reset(); },
      [](auto &value) { value.provenance.engineBuild = ""; },
      [](auto &value) { value.provenance.seed.reset(); },
      [](auto &value) { value.provenance.costProfile = std::string{}; },
      [](auto &value) { value.provenance.commissionRatePartsPerBillion = -1; },
      [](auto &value) { value.provenance.slippageRatePartsPerBillion = -1; },
      [](auto &value) {
        value.provenance.strategyMode =
            static_cast<bte::results::StrategyMode>(99);
      },
      [](auto &value) {
        value.provenance.provenanceAvailability["costs"] =
            ProvenanceAvailabilityState::legacyUnrecorded;
      },
      [](auto &value) {
        value.provenance.provenanceAvailability["strategyApi"] =
            ProvenanceAvailabilityState::legacyUnrecorded;
      },
      [](auto &value) {
        value.provenance.provenanceAvailability["unknown"] =
            ProvenanceAvailabilityState::recorded;
      },
      [](auto &value) {
        value.provenance.provenanceAvailability["aggregationPolicy"] =
            static_cast<ProvenanceAvailabilityState>(99);
      },
      [](auto &value) { value.provenance.canonicalParametersJson = "null"; },
      [](auto &value) {
        value.provenance.canonicalParametersJson =
            std::string(64U * 1024U + 1U, 'x');
      },
      [](auto &value) { value.provenance.sourceUtf8 = "source\rwith-cr"; },
      [](auto &value) { value.provenance.sourceUtf8 = "\xef\xbb\xbfsource"; },
      [](auto &value) { value.provenance.sourceUtf8 = std::string(1, '\0'); },
      [](auto &value) {
        value.provenance.sourceUtf8 = std::string(1024U * 1024U + 1U, 'x');
      },
      [](auto &value) {
        value.provenance.sourceUtf8 = std::string(1, static_cast<char>(0xff));
        value.provenance.sourceHash =
            bte::core::sha256(value.provenance.sourceUtf8);
        value.strategyHash = value.provenance.sourceHash;
      },
      [](auto &value) { value.provenance.sourceHash = std::string(64, '0'); },
      [](auto &value) { value.provenance.canonicalPlanJson = "{}"; },
      [](auto &value) { value.provenance.planHash = "invalid"; },
      [](auto &value) { value.provenance.runtime.reset(); },
      [](auto &value) {
        value.provenance.runtime->canonicalManifestJson = "[]";
        value.provenance.runtime->manifestHash = bte::core::sha256("[]");
      },
      [](auto &value) {
        value.provenance.runtime->canonicalManifestJson =
            std::string(1024U * 1024U + 1U, 'x');
      },
      [](auto &value) {
        value.provenance.strategyMode = bte::results::StrategyMode::starter;
      },
      [](auto &value) { value.provenance.runtime->profileId.clear(); },
      [](auto &value) { value.provenance.runtime->pythonVersion.clear(); },
      [](auto &value) { value.provenance.runtime->pythonAbi.clear(); },
      [](auto &value) {
        value.provenance.runtime->stockbtSourceHash = std::string(64, '0');
      },
      [](auto &value) {
        value.provenance.runtime->packages.front().name = "stockbt";
      },
      [](auto &value) {
        value.provenance.runtime->packages.front().artifactHash = "invalid";
      },
      [](auto &value) {
        value.provenance.runtime->packages.erase(std::ranges::find(
            value.provenance.runtime->packages, std::string{"stockbt"},
            &bte::results::RuntimePackageIdentity::name));
      },
      [](auto &value) {
        value.provenance.runtime->packages.front().name =
            value.provenance.runtime->packages.back().name;
      },
  };

  for (const auto &mutate : invalidDescriptors) {
    auto invalid = schemaThreeDescriptor();
    mutate(invalid);
    expectRejected(invalid);
  }

  auto selectable = schemaThreeDescriptor();
  selectable.provenance.strategyMode =
      bte::results::StrategyMode::selectableConditions;
  selectable.provenance.sourceUtf8.clear();
  selectable.provenance.sourceHash.clear();
  selectable.provenance.canonicalPlanJson = R"({"version":1})";
  selectable.provenance.planHash =
      bte::core::sha256(selectable.provenance.canonicalPlanJson);
  selectable.provenance.runtime.reset();
  expectRejected([&] {
    auto invalid = selectable;
    invalid.provenance.canonicalPlanJson = "{";
    return invalid;
  }());
  expectRejected([&] {
    auto invalid = selectable;
    invalid.provenance.canonicalPlanJson = "null";
    invalid.provenance.planHash =
        bte::core::sha256(invalid.provenance.canonicalPlanJson);
    return invalid;
  }());
  expectRejected([&] {
    auto invalid = selectable;
    invalid.provenance.canonicalPlanJson = R"({"version":1,"condition":"buy"})";
    invalid.provenance.planHash =
        bte::core::sha256(invalid.provenance.canonicalPlanJson);
    return invalid;
  }());
  auto validSelectable = store.value()->begin(selectable);
  ASSERT_TRUE(validSelectable.ok()) << validSelectable.error().message;
  ASSERT_TRUE(validSelectable.value()->append(records()).ok());
  const auto selectableResult = validSelectable.value()->finalizeAndPromote(
      bte::results::RunStatus::completed,
      {.finalEquityMicrodollars = 100'009'900'000, .pnlMicrodollars = 9'900});
  ASSERT_TRUE(selectableResult.ok()) << selectableResult.error().message;
  const auto reopenedSelectable =
      store.value()->openResult(selectableResult.value().resultId);
  ASSERT_TRUE(reopenedSelectable.ok()) << reopenedSelectable.error().message;
  EXPECT_EQ(reopenedSelectable.value().descriptor.provenance.strategyMode,
            bte::results::StrategyMode::selectableConditions);
  EXPECT_EQ(reopenedSelectable.value().descriptor.provenance.planHash,
            selectable.provenance.planHash);
  expectRejected([&] {
    auto invalid = selectable;
    invalid.provenance.apiVersion.clear();
    return invalid;
  }());

  auto legacy = schemaThreeDescriptor();
  legacy.provenance.strategyMode = bte::results::StrategyMode::legacyUnrecorded;
  legacy.provenance.canonicalParametersJson = "null";
  legacy.provenance.runtime.reset();
  expectRejected(legacy);

  const std::vector<ManifestMutation> invalidManifests{
      [](auto &manifest) { manifest["schemaVersion"] = 2; },
      [](auto &manifest) { manifest["platform"] = "linux"; },
      [](auto &manifest) { manifest["architecture"] = "x86_64"; },
      [](auto &manifest) { manifest["stockbtApiVersion"] = "2"; },
      [](auto &manifest) { manifest["profileBuilderVersion"] = ""; },
      [](auto &manifest) { manifest["pythonExecutable"] = ""; },
      [](auto &manifest) {
        manifest["installedFiles"] = nlohmann::json::array();
      },
      [](auto &manifest) { manifest["profileId"] = "wrong-profile"; },
      [](auto &manifest) { manifest["pythonVersion"] = "3.12"; },
      [](auto &manifest) { manifest["pythonAbi"] = "cp312"; },
      [](auto &manifest) {
        manifest["stockbtSourceSha256"] = std::string(64, '0');
      },
      [](auto &manifest) { manifest["installedFiles"][0] = "not-an-object"; },
      [](auto &manifest) { manifest["installedFiles"][0].erase("path"); },
      [](auto &manifest) { manifest["installedFiles"][0]["path"] = ""; },
      [](auto &manifest) {
        manifest["installedFiles"][0]["sha256"] = "invalid";
      },
      [](auto &manifest) {
        manifest["installedFiles"].push_back(manifest["installedFiles"][0]);
      },
      [](auto &manifest) { manifest["components"][0] = "not-an-object"; },
      [](auto &manifest) {
        manifest["components"][0]["licenses"] = nlohmann::json::array();
      },
      [](auto &manifest) { manifest["components"][0].erase("upstreamUrl"); },
      [](auto &manifest) { manifest["components"] = nlohmann::json::array(); },
  };
  for (const auto &mutate : invalidManifests) {
    auto invalid = schemaThreeDescriptor();
    auto parsed = bte::core::parseCanonicalJson(
        invalid.provenance.runtime->canonicalManifestJson);
    ASSERT_TRUE(parsed.ok()) << parsed.error().message;
    auto manifest = std::move(parsed).value();
    mutate(manifest);
    const auto canonical = bte::core::serializeCanonicalJson(manifest);
    ASSERT_TRUE(canonical.ok()) << canonical.error().message;
    invalid.provenance.runtime->canonicalManifestJson = canonical.value();
    invalid.provenance.runtime->manifestHash =
        bte::core::sha256(canonical.value());
    expectRejected(invalid);
  }

  EXPECT_TRUE(std::filesystem::is_empty(root_ / "SchemaThreeRuntimeInvalid" /
                                        "Staging"));
}

TEST_F(ResultStoreFixture,
       schemaThreeRequiresRuntimeStockbtHashToMatchThePackageIdentity) {
  const auto directory = root_ / "SchemaThreeStockbtHashMismatch";
  auto store = bte::results::ResultStore::open(directory, root_ / "Data");
  ASSERT_TRUE(store.ok()) << store.error().message;
  auto invalid = schemaThreeDescriptor();
  auto parsed = bte::core::parseCanonicalJson(
      invalid.provenance.runtime->canonicalManifestJson);
  ASSERT_TRUE(parsed.ok()) << parsed.error().message;
  auto manifest = std::move(parsed).value();
  const auto mismatchedHash = std::string(64, '0');
  manifest["stockbtSourceSha256"] = mismatchedHash;
  const auto canonical = bte::core::serializeCanonicalJson(manifest);
  ASSERT_TRUE(canonical.ok()) << canonical.error().message;
  invalid.provenance.runtime->stockbtSourceHash = mismatchedHash;
  invalid.provenance.runtime->canonicalManifestJson = canonical.value();
  invalid.provenance.runtime->manifestHash =
      bte::core::sha256(canonical.value());

  const auto rejected = store.value()->begin(invalid);

  ASSERT_FALSE(rejected.ok());
  EXPECT_EQ(rejected.error().code, bte::core::ErrorCode::invalidArgument);
  EXPECT_TRUE(std::filesystem::is_empty(directory / "Staging"));
}

TEST_F(ResultStoreFixture,
       schemaThreeRejectsTamperedProvenanceAndUnknownMajorVersion) {
  auto store = bte::results::ResultStore::open(root_ / "SchemaThreeTamper",
                                               root_ / "Data");
  ASSERT_TRUE(store.ok()) << store.error().message;
  auto create = [&]() {
    auto writer = store.value()->begin(schemaThreeDescriptor());
    EXPECT_TRUE(writer.ok()) << writer.error().message;
    EXPECT_TRUE(writer.value()->append(records()).ok());
    auto finalized = writer.value()->finalizeAndPromote(
        bte::results::RunStatus::completed,
        {.finalEquityMicrodollars = 100'009'900'000, .pnlMicrodollars = 9'900});
    EXPECT_TRUE(finalized.ok()) << finalized.error().message;
    return finalized.value().resultId;
  };
  const auto tamperedProvenanceId = create();
  executeSql(root_ / "SchemaThreeTamper" / "Results" /
                 (tamperedProvenanceId + ".bteresult"),
             "UPDATE schema3_provenance SET canonical_json="
             "replace(canonical_json,'fixture-build-1','fixture-build-2')");
  const auto tamperedProvenance =
      store.value()->openResult(tamperedProvenanceId);
  ASSERT_FALSE(tamperedProvenance.ok());
  EXPECT_EQ(tamperedProvenance.error().code,
            bte::core::ErrorCode::schemaMismatch);

  const auto unknownMajorId = create();
  executeSql(root_ / "SchemaThreeTamper" / "Results" /
                 (unknownMajorId + ".bteresult"),
             "UPDATE result_meta SET schema_version=4");
  const auto unknownMajor = store.value()->openResult(unknownMajorId);
  ASSERT_FALSE(unknownMajor.ok());
  EXPECT_EQ(unknownMajor.error().code, bte::core::ErrorCode::schemaMismatch);
}

TEST_F(ResultStoreFixture,
       schemaThreeRejectsMalformedPersistedProvenanceFields) {
  using ProvenanceMutation = std::function<void(nlohmann::json &)>;
  const auto directory = root_ / "SchemaThreeMalformedStoredProvenance";
  auto store = bte::results::ResultStore::open(directory, root_ / "Data");
  ASSERT_TRUE(store.ok()) << store.error().message;
  const std::vector<ProvenanceMutation> invalidMutations{
      [](auto &document) { document["strategyMode"] = "unknown"; },
      [](auto &document) { document["apiVersion"] = 1; },
      [](auto &document) { document.erase("engineBuild"); },
      [](auto &document) { document["costProfile"] = nlohmann::json::array(); },
      [](auto &document) { document["commissionRatePartsPerBillion"] = "01"; },
      [](auto &document) {
        document["provenanceAvailability"]["costs"] = false;
      },
      [](auto &document) {
        document["provenanceAvailability"]["costs"] = "unknown";
      },
      [](auto &document) { document["runtime"].erase("manifest"); },
      [](auto &document) {
        document["runtime"]["packages"][0]["extra"] = "field";
      },
      [](auto &document) {
        document["runtime"]["packages"][0]["artifactHash"] = 1;
      },
      [](auto &document) { document["runtime"]["packages"] = "invalid"; },
      [](auto &document) { document["runtime"] = false; },
      [](auto &document) { document["unexpected"] = true; },
  };

  for (const auto &mutate : invalidMutations) {
    const auto result = createSchemaThreeResult(
        *store.value(), schemaThreeDescriptor(),
        bte::results::RunStatus::completed, "stored provenance validation");
    ASSERT_TRUE(result.ok()) << result.error().message;
    const auto resultPath =
        directory / "Results" / (result.value().resultId + ".bteresult");
    const auto provenanceBytes =
        queryText(resultPath, "SELECT canonical_json FROM schema3_provenance");
    ASSERT_FALSE(provenanceBytes.empty());
    auto provenance = bte::core::parseCanonicalJson(provenanceBytes);
    ASSERT_TRUE(provenance.ok()) << provenance.error().message;
    auto document = std::move(provenance).value();
    mutate(document);
    const auto serialized = bte::core::serializeCanonicalJson(document);
    ASSERT_TRUE(serialized.ok()) << serialized.error().message;
    executeSqlText(resultPath,
                   "UPDATE schema3_provenance SET canonical_json=?1",
                   serialized.value());

    const auto rejected = store.value()->openResult(result.value().resultId);
    ASSERT_FALSE(rejected.ok());
    EXPECT_EQ(rejected.error().code, bte::core::ErrorCode::schemaMismatch);
  }
}

TEST_F(ResultStoreFixture,
       schemaThreeBindsAndReopensDiagnosticCompletenessState) {
  auto store = bte::results::ResultStore::open(root_ / "SchemaThreeDiagnostics",
                                               root_ / "Data");
  ASSERT_TRUE(store.ok()) << store.error().message;
  auto writer = store.value()->begin(schemaThreeDescriptor());
  ASSERT_TRUE(writer.ok()) << writer.error().message;
  ASSERT_TRUE(writer.value()->append(records()).ok());
  const auto summary = bte::results::FinalSummary{
      .finalEquityMicrodollars = std::nullopt,
      .diagnosticsComplete = false,
  };
  const auto finalized = writer.value()->finalizeAndPromote(
      bte::results::RunStatus::canceled, summary, "diagnostics-truncated");
  ASSERT_TRUE(finalized.ok()) << finalized.error().message;
  const auto opened = store.value()->openResult(finalized.value().resultId);
  ASSERT_TRUE(opened.ok()) << opened.error().message;
  EXPECT_FALSE(opened.value().summary.diagnosticsComplete);

  executeSql(root_ / "SchemaThreeDiagnostics" / "Results" /
                 (finalized.value().resultId + ".bteresult"),
             "UPDATE schema3_terminal_state SET diagnostics_complete=1");
  const auto tampered = store.value()->openResult(finalized.value().resultId);
  ASSERT_FALSE(tampered.ok());
  EXPECT_EQ(tampered.error().code, bte::core::ErrorCode::schemaMismatch);
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
       cancellationAfterSnapshotReaderOpenStopsExactReadWithoutMutation) {
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
  bte::results::testing::cancelNextOpenResultWithDataAfterReaderOpen(
      &cancellation);
  const auto cancelled = store.value()->openResultWithData(
      finalized.value().resultId, cancellation.token());

  ASSERT_FALSE(cancelled.ok());
  EXPECT_EQ(cancelled.error().code, bte::core::ErrorCode::cancelled);
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

TEST_F(ResultStoreFixture, allocatorSkipsQuarantinedIdentityForBeginAndImport) {
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
  const auto quarantinedPath =
      destinationRoot / "Quarantine" / (std::string{forcedId} + ".bteresult");
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
  const auto lease =
      root_ / "Store" / "Leases" / (writer.value()->resultId() + ".sqlite3");
  EXPECT_TRUE(std::filesystem::is_regular_file(lease));
  auto observer =
      bte::results::ResultStore::open(root_ / "Store", root_ / "Data");
  ASSERT_TRUE(observer.ok());
  EXPECT_TRUE(observer.value()->recoveryNotices().empty());
  EXPECT_TRUE(observer.value()->listDrafts().value().empty());
}

TEST_F(ResultStoreFixture, initializationAndImportPinFailuresExposeNoArtifact) {
  auto source =
      bte::results::ResultStore::open(root_ / "SourceStore", root_ / "Data");
  ASSERT_TRUE(source.ok());
  auto writer = source.value()->begin(descriptor());
  ASSERT_TRUE(writer.ok());
  ASSERT_TRUE(writer.value()->append(records()).ok());
  auto finalized = writer.value()->finalizeAndPromote(
      bte::results::RunStatus::completed,
      {.finalEquityMicrodollars = 100'009'900'000, .pnlMicrodollars = 9'900});
  ASSERT_TRUE(finalized.ok());
  const auto sourceFile = root_ / "SourceStore" / "Results" /
                          (finalized.value().resultId + ".bteresult");
  for (const auto point :
       {bte::data::testing::RetentionFailurePoint::databaseOpen,
        bte::data::testing::RetentionFailurePoint::boundPreparation}) {
    for (const auto importing : {false, true}) {
      const auto directory =
          root_ / ("PinFailure-" + std::to_string(static_cast<int>(point)) +
                   (importing ? "Import" : "Begin"));
      auto destination =
          bte::results::ResultStore::open(directory, root_ / "Data");
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

TEST_F(ResultStoreFixture,
       legacyUnleasedArtifactsRemainByteIdenticalBesideSavedResults) {
  const auto directory = root_ / "Store";
  auto store = bte::results::ResultStore::open(directory, root_ / "Data");
  ASSERT_TRUE(store.ok());
  auto writer = store.value()->begin(descriptor());
  ASSERT_TRUE(writer.ok());
  ASSERT_TRUE(writer.value()->append(records()).ok());
  ASSERT_TRUE(
      writer.value()
          ->finalizeAndPromote(bte::results::RunStatus::completed,
                               {.finalEquityMicrodollars = 100'009'900'000,
                                .pnlMicrodollars = 9'900})
          .ok());
  for (const auto &[id, suffix] :
       std::vector<std::pair<std::string, std::string>>{
           {std::string(32, 'a'), ".bteresult"},
           {std::string(32, 'b'), ".purging.bteresult"},
           {std::string(32, 'c'), ".discarding.bteresult"}}) {
    std::ofstream artifact{directory / "Staging" / (id + suffix)};
    artifact << "legacy partial payload";
  }
  for (int attempt = 0; attempt < 2; ++attempt) {
    auto reopened = bte::results::ResultStore::open(directory, root_ / "Data");
    ASSERT_TRUE(reopened.ok());
    ASSERT_EQ(reopened.value()->recoveryNotices().size(), 3U);
    for (const auto &notice : reopened.value()->recoveryNotices()) {
      EXPECT_EQ(notice.reason,
                bte::results::ResultRecoveryReason::legacyUnleased);
      EXPECT_FALSE(notice.diagnostic.empty());
      EXPECT_FALSE(std::filesystem::exists(directory / "Leases" /
                                           (notice.resultId + ".sqlite3")));
    }
    EXPECT_EQ(reopened.value()->list().value().size(), 1U);
    EXPECT_TRUE(reopened.value()->listDrafts().value().empty());
    for (const auto &entry :
         std::filesystem::directory_iterator{directory / "Staging"}) {
      std::ifstream input{entry.path()};
      std::string content;
      std::getline(input, content);
      EXPECT_EQ(content, "legacy partial payload") << entry.path();
    }
  }
}

TEST_F(ResultStoreFixture,
       recoveryPreservesMissingPinsAndRejectsIncompleteOrSkippedBars) {
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
    EXPECT_TRUE(
        std::filesystem::exists(directory / "Staging" / (id + ".bteresult")));
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

TEST_F(ResultStoreFixture,
       orphanInitializationPinsAreReportedAndTombstonesRemainQuiet) {
  const auto directory = root_ / "Store";
  std::string id;
  {
    auto store = bte::results::ResultStore::open(directory, root_ / "Data");
    ASSERT_TRUE(store.ok());
    auto writer = store.value()->begin(descriptor());
    ASSERT_TRUE(writer.ok());
    id = writer.value()->resultId();
  }
  ASSERT_TRUE(
      std::filesystem::remove(directory / "Staging" / (id + ".bteresult")));
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
  EXPECT_TRUE(std::filesystem::is_regular_file(directory / "Leases" /
                                               (id + ".sqlite3")));
}

TEST_F(ResultStoreFixture,
       invalidLeaseIdentityVersionModeAndSchemaPreserveStaging) {
  for (const auto &[name, sql] :
       std::vector<std::pair<std::string, std::string>>{
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
    EXPECT_TRUE(std::filesystem::is_regular_file(directory / "Staging" /
                                                 (id + ".bteresult")));
    EXPECT_TRUE(reopened.value()->listDrafts().value().empty());
  }
}

TEST_F(ResultStoreFixture,
       terminalTailAndMetadataCommitAtomicallyAcrossFailures) {
  for (const auto point :
       {bte::results::testing::FailurePoint::statementPreparation,
        bte::results::testing::FailurePoint::statementExecution,
        bte::results::testing::FailurePoint::sqlExecution}) {
    const auto directory =
        root_ / ("AtomicTail-" + std::to_string(static_cast<int>(point)));
    auto store = bte::results::ResultStore::open(directory, root_ / "Data");
    ASSERT_TRUE(store.ok());
    auto writer = store.value()->begin(descriptor());
    ASSERT_TRUE(writer.ok());
    const auto canonical = records();
    ASSERT_TRUE(writer.value()->append({canonical.front()}).ok());
    const auto path =
        directory / "Staging" / (writer.value()->resultId() + ".bteresult");
    bte::results::testing::failAfter(
        point,
        point == bte::results::testing::FailurePoint::sqlExecution ? 1U : 0U);
    const auto tail =
        std::span<const bte::results::CanonicalRecord>{canonical}.subspan(1);
    const bte::results::FinalSummary summary{
        .finalEquityMicrodollars = 100'009'900'000, .pnlMicrodollars = 9'900};
    EXPECT_FALSE(writer.value()
                     ->finalizeDraft(bte::results::RunStatus::completed,
                                     summary, "", tail)
                     .ok());
    bte::results::testing::clearFailure();
    EXPECT_EQ(queryInteger(path, "SELECT count(*) FROM canonical_records"), 1);
    EXPECT_EQ(queryInteger(path, "SELECT count(*) FROM summary"), 0);
    EXPECT_EQ(queryInteger(path, "SELECT status FROM result_meta"), 0);
    auto invalid = canonical.back();
    invalid.sequence = 0;
    EXPECT_FALSE(writer.value()
                     ->finalizeDraft(bte::results::RunStatus::completed,
                                     summary, "", {&invalid, 1})
                     .ok());
    ASSERT_TRUE(writer.value()
                    ->finalizeDraft(bte::results::RunStatus::completed, summary,
                                    "", tail)
                    .ok());
    ASSERT_TRUE(store.value()->publishDraft(writer.value()->resultId()).ok());
    const auto opened = store.value()->openResult(writer.value()->resultId());
    ASSERT_TRUE(opened.ok());
    EXPECT_EQ(opened.value().records, canonical);
    EXPECT_EQ(opened.value().summary, summary);
  }
}

TEST_F(ResultStoreFixture,
       everyLifecycleMutationReturnsBusyWithoutChangingArtifacts) {
  const auto directory = root_ / "Store";
  auto store = bte::results::ResultStore::open(directory, root_ / "Data");
  ASSERT_TRUE(store.ok());
  auto writer = store.value()->begin(descriptor());
  ASSERT_TRUE(writer.ok());
  ASSERT_TRUE(writer.value()->append(records()).ok());
  ASSERT_TRUE(writer.value()
                  ->finalizeDraft(bte::results::RunStatus::completed,
                                  {.finalEquityMicrodollars = 100'009'900'000,
                                   .pnlMicrodollars = 9'900})
                  .ok());
  const auto id = writer.value()->resultId();
  sqlite3 *raw = nullptr;
  ASSERT_EQ(
      sqlite3_open((directory / "Leases" / (id + ".sqlite3")).string().c_str(),
                   &raw),
      SQLITE_OK);
  const auto database =
      std::unique_ptr<sqlite3, decltype(&sqlite3_close)>{raw, &sqlite3_close};
  ASSERT_EQ(sqlite3_exec(database.get(), "BEGIN EXCLUSIVE", nullptr, nullptr,
                         nullptr),
            SQLITE_OK);
  for (const auto &operation :
       std::vector<std::function<bte::core::Result<void>()>>{
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
  EXPECT_TRUE(std::filesystem::is_regular_file(directory / "Drafts" /
                                               (id + ".bteresult")));
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

TEST_F(ResultStoreFixture,
       legacyTerminalIdentityIsValidatedBeforeLeaseCreation) {
  const auto directory = root_ / "LegacyTerminal";
  auto store = bte::results::ResultStore::open(directory, root_ / "Data");
  ASSERT_TRUE(store.ok()) << store.error().message;
  auto writer = store.value()->begin(descriptor());
  ASSERT_TRUE(writer.ok()) << writer.error().message;
  ASSERT_TRUE(writer.value()->append(records()).ok());
  const auto finalized = writer.value()->finalizeAndPromote(
      bte::results::RunStatus::completed,
      {.finalEquityMicrodollars = 100'009'900'000, .pnlMicrodollars = 9'900});
  ASSERT_TRUE(finalized.ok()) << finalized.error().message;

  const auto resultId = finalized.value().resultId;
  const auto resultPath = directory / "Results" / (resultId + ".bteresult");
  const auto leasePath = directory / "Leases" / (resultId + ".sqlite3");
  ASSERT_TRUE(std::filesystem::remove(leasePath));
  ASSERT_TRUE(store.value()->renameResult(resultId, "Legacy terminal").ok());
  EXPECT_TRUE(std::filesystem::is_regular_file(leasePath));
  const auto renamed = store.value()->openResult(resultId);
  ASSERT_TRUE(renamed.ok()) << renamed.error().message;
  EXPECT_EQ(renamed.value().displayName, "Legacy terminal");

  const auto mismatchedId = std::string(32, 'e');
  const auto mismatchedPath =
      directory / "Results" / (mismatchedId + ".bteresult");
  ASSERT_TRUE(std::filesystem::copy_file(resultPath, mismatchedPath));
  const auto mismatch = store.value()->moveToTrash(mismatchedId);
  ASSERT_FALSE(mismatch.ok());
  EXPECT_EQ(mismatch.error().code, bte::core::ErrorCode::schemaMismatch);
  EXPECT_TRUE(std::filesystem::is_regular_file(mismatchedPath));
  EXPECT_FALSE(std::filesystem::exists(directory / "Leases" /
                                       (mismatchedId + ".sqlite3")));
}

TEST_F(ResultStoreFixture,
       invalidLeaseNamesAndUnreadableLeaseFilesRemainRecoveryNotices) {
  const auto directory = root_ / "InvalidLeaseArtifacts";
  auto store = bte::results::ResultStore::open(directory, root_ / "Data");
  ASSERT_TRUE(store.ok()) << store.error().message;

  const auto invalidId = std::string{"INVALID-ID"};
  {
    std::ofstream staging{directory / "Staging" / (invalidId + ".bteresult")};
    staging << "legacy invalid identity";
    ASSERT_TRUE(staging.good());
    std::ofstream lease{directory / "Leases" / (invalidId + ".sqlite3")};
    lease << "invalid lease";
    ASSERT_TRUE(lease.good());
  }

  const auto unreadableId = std::string(32, 'f');
  const auto unreadableLease =
      directory / "Leases" / (unreadableId + ".sqlite3");
  ASSERT_TRUE(std::filesystem::create_directory(unreadableLease));
  {
    std::ofstream staging{directory / "Staging" /
                          (unreadableId + ".bteresult")};
    staging << "staging with unreadable lease";
    ASSERT_TRUE(staging.good());
  }

  const auto reopened =
      bte::results::ResultStore::open(directory, root_ / "Data");
  ASSERT_TRUE(reopened.ok()) << reopened.error().message;
  ASSERT_EQ(reopened.value()->recoveryNotices().size(), 2U);
  for (const auto &notice : reopened.value()->recoveryNotices()) {
    EXPECT_EQ(notice.reason, bte::results::ResultRecoveryReason::invalidLease);
  }
  EXPECT_TRUE(std::filesystem::is_regular_file(directory / "Staging" /
                                               (invalidId + ".bteresult")));
  EXPECT_TRUE(std::filesystem::is_regular_file(directory / "Staging" /
                                               (unreadableId + ".bteresult")));
}

TEST_F(ResultStoreFixture, existingLeaseIdentityIsSkippedDuringBegin) {
  const auto directory = root_ / "ExistingLease";
  auto store = bte::results::ResultStore::open(directory, root_ / "Data");
  ASSERT_TRUE(store.ok()) << store.error().message;

  const auto occupiedId = std::string(32, 'a');
  std::ofstream occupiedLease{directory / "Leases" / (occupiedId + ".sqlite3")};
  ASSERT_TRUE(occupiedLease.good());
  occupiedLease << "lease identity is already owned";
  occupiedLease.close();

  bte::results::testing::forceNextResultId(occupiedId);
  const auto writer = store.value()->begin(descriptor());
  ASSERT_TRUE(writer.ok()) << writer.error().message;
  EXPECT_NE(writer.value()->resultId(), occupiedId);
}

TEST_F(ResultStoreFixture, leaseInitializationFailureDoesNotPinOrStageResult) {
  const auto directory = root_ / "BlockedLeaseInitialization";
  auto store = bte::results::ResultStore::open(directory, root_ / "Data");
  ASSERT_TRUE(store.ok()) << store.error().message;
  std::filesystem::remove_all(directory / "Leases");
  std::ofstream blockedLeases{directory / "Leases"};
  ASSERT_TRUE(blockedLeases.good());
  blockedLeases << "not a directory";
  blockedLeases.close();

  const auto resultId = std::string(32, 'b');
  bte::results::testing::forceNextResultId(resultId);
  const auto writer = store.value()->begin(descriptor());
  ASSERT_FALSE(writer.ok());
  EXPECT_FALSE(std::filesystem::exists(directory / "Staging" /
                                       (resultId + ".bteresult")));

  auto retention = bte::data::SegmentRetentionStore::open(root_ / "Data");
  ASSERT_TRUE(retention.ok()) << retention.error().message;
  const auto references = retention.value()->references(resultId);
  ASSERT_TRUE(references.ok()) << references.error().message;
  EXPECT_TRUE(references.value().empty());
}

TEST_F(ResultStoreFixture,
       metadataWriteFailuresLeavePublishedResultNameUnchanged) {
  const auto directory = root_ / "MetadataWriteFailures";
  auto store = bte::results::ResultStore::open(directory, root_ / "Data");
  ASSERT_TRUE(store.ok()) << store.error().message;
  auto writer = store.value()->begin(descriptor());
  ASSERT_TRUE(writer.ok()) << writer.error().message;
  ASSERT_TRUE(writer.value()->append(records()).ok());
  const auto finalized = writer.value()->finalizeAndPromote(
      bte::results::RunStatus::completed,
      {.finalEquityMicrodollars = 100'009'900'000, .pnlMicrodollars = 9'900});
  ASSERT_TRUE(finalized.ok()) << finalized.error().message;
  const auto resultId = finalized.value().resultId;

  const auto expectFailedRename =
      [&](const bte::results::testing::FailurePoint point,
          const std::uint32_t skipped, const std::string &name) {
        bte::results::testing::failAfter(point, skipped);
        const auto renamed = store.value()->renameResult(resultId, name);
        const auto injected = !bte::results::testing::failurePending();
        bte::results::testing::clearFailure();
        EXPECT_TRUE(injected) << name;
        EXPECT_FALSE(renamed.ok()) << name;
        const auto opened = store.value()->openResult(resultId);
        ASSERT_TRUE(opened.ok()) << opened.error().message;
        EXPECT_NE(opened.value().displayName, name);
      };

  expectFailedRename(bte::results::testing::FailurePoint::databaseOpen, 1,
                     "database open failure");
  expectFailedRename(bte::results::testing::FailurePoint::sqlExecution, 0,
                     "transaction begin failure");
  expectFailedRename(bte::results::testing::FailurePoint::sqlExecution, 3,
                     "transaction commit failure");
  expectFailedRename(bte::results::testing::FailurePoint::textBinding, 0,
                     "display name binding failure");
  expectFailedRename(bte::results::testing::FailurePoint::statementExecution, 0,
                     "display name insert failure");

  auto preparationReachedEnd = false;
  for (std::uint32_t skipped = 0; skipped < 32; ++skipped) {
    bte::results::testing::failAfter(
        bte::results::testing::FailurePoint::statementPreparation, skipped);
    const auto renamed = store.value()->renameResult(
        resultId, "preparation failure " + std::to_string(skipped));
    const auto injected = !bte::results::testing::failurePending();
    bte::results::testing::clearFailure();
    if (!injected) {
      ASSERT_TRUE(renamed.ok()) << renamed.error().message;
      preparationReachedEnd = true;
      break;
    }
    EXPECT_FALSE(renamed.ok()) << skipped;
  }
  EXPECT_TRUE(preparationReachedEnd);
}

TEST_F(ResultStoreFixture,
       verificationStoreRejectsMissingSymlinkedAndOversizedEvidence) {
  using bte::results::VerificationListEntry;
  using bte::results::VerificationStatus;

  const auto emptyRoot = bte::results::VerificationStore::open({});
  ASSERT_FALSE(emptyRoot.ok());
  EXPECT_EQ(emptyRoot.error().code, bte::core::ErrorCode::invalidArgument);

  const auto directory = root_ / "VerificationEvidenceReadFailures";
  auto store = bte::results::VerificationStore::open(directory);
  ASSERT_TRUE(store.ok()) << store.error().message;

  const auto invalidId = store.value()->load("not-a-verification-id");
  ASSERT_FALSE(invalidId.ok());
  EXPECT_EQ(invalidId.error().code, bte::core::ErrorCode::invalidArgument);

  const auto missingId = std::string(32, 'f');
  const auto missing = store.value()->load(missingId);
  ASSERT_FALSE(missing.ok());
  EXPECT_EQ(missing.error().code, bte::core::ErrorCode::notFound);

  const auto valid = store.value()->create(schemaThreeDescriptor());
  ASSERT_TRUE(valid.ok()) << valid.error().message;

#if defined(__APPLE__) || defined(__linux__)
  const auto symlinkId = std::string(32, 'b');
  const auto target = root_ / "VerificationEvidenceSymlinkTarget";
  {
    std::ofstream contents{target, std::ios::binary};
    ASSERT_TRUE(contents.is_open());
    contents << "preserve evidence target";
  }
  const auto symlinkPath =
      directory / "Verifications" / (symlinkId + ".bteverify");
  std::filesystem::create_symlink(target, symlinkPath);

  const auto symlinkRead = store.value()->load(symlinkId);
  ASSERT_FALSE(symlinkRead.ok());
  EXPECT_EQ(symlinkRead.error().code, bte::core::ErrorCode::schemaMismatch);
  EXPECT_TRUE(std::filesystem::is_symlink(symlinkPath));
  std::ifstream preservedTarget{target, std::ios::binary};
  ASSERT_TRUE(preservedTarget.is_open());
  const std::string preservedContents{
      std::istreambuf_iterator<char>{preservedTarget},
      std::istreambuf_iterator<char>{}};
  EXPECT_EQ(preservedContents, "preserve evidence target");
#endif

  const auto oversizedId = std::string(32, 'c');
  const auto oversizedPath =
      directory / "Verifications" / (oversizedId + ".bteverify");
  {
    std::ofstream oversized{oversizedPath, std::ios::binary};
    ASSERT_TRUE(oversized.is_open());
    const std::string bytes(64U * 1024U + 1U, 'x');
    oversized.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    ASSERT_TRUE(oversized.good());
  }

  const auto oversized = store.value()->load(oversizedId);
  ASSERT_FALSE(oversized.ok());
  EXPECT_EQ(oversized.error().code, bte::core::ErrorCode::schemaMismatch);

  const auto entries = store.value()->list();
  ASSERT_TRUE(entries.ok()) << entries.error().message;
  const auto validEntry =
      std::ranges::find(entries.value(), valid.value().verificationId,
                        &VerificationListEntry::verificationId);
  ASSERT_NE(validEntry, entries.value().end());
  ASSERT_TRUE(validEntry->evidence.has_value());
  EXPECT_EQ(validEntry->evidence->status, VerificationStatus::running);
  const auto oversizedEntry = std::ranges::find(
      entries.value(), oversizedId, &VerificationListEntry::verificationId);
  ASSERT_NE(oversizedEntry, entries.value().end());
  EXPECT_FALSE(oversizedEntry->evidence.has_value());
  EXPECT_FALSE(oversizedEntry->diagnostic.empty());
  EXPECT_TRUE(std::filesystem::is_regular_file(oversizedPath));
}

TEST_F(ResultStoreFixture,
       verificationAttemptFailureAndCancellationPreserveAssignedResultId) {
  using bte::results::FinalizedResult;
  using bte::results::RunStatus;
  using bte::results::VerificationAttempt;
  using bte::results::VerificationError;
  using bte::results::VerificationStatus;

  const auto directory = root_ / "VerificationAttemptTransitions";
  auto store = bte::results::VerificationStore::open(directory);
  ASSERT_TRUE(store.ok()) << store.error().message;

  const auto canceledBeforeAttempt =
      store.value()->create(schemaThreeDescriptor());
  ASSERT_TRUE(canceledBeforeAttempt.ok())
      << canceledBeforeAttempt.error().message;
  const auto canceledWithoutAttempt =
      store.value()->cancel(canceledBeforeAttempt.value().verificationId);
  ASSERT_TRUE(canceledWithoutAttempt.ok())
      << canceledWithoutAttempt.error().message;
  EXPECT_EQ(canceledWithoutAttempt.value().status,
            VerificationStatus::canceled);
  EXPECT_TRUE(canceledWithoutAttempt.value().attempts.empty());

  const auto descriptor = schemaThreeDescriptor();
  const auto evidence = store.value()->create(descriptor);
  ASSERT_TRUE(evidence.ok()) << evidence.error().message;
  const auto verificationId = evidence.value().verificationId;
  const auto validResult = FinalizedResult{
      .resultId = std::string(32, '1'),
      .canonicalResultHash = std::string(64, 'a'),
  };

  const auto invalidStatus = store.value()->completeAttempt(
      verificationId, descriptor, validResult, static_cast<RunStatus>(99));
  ASSERT_FALSE(invalidStatus.ok());
  EXPECT_EQ(invalidStatus.error().code, bte::core::ErrorCode::invalidArgument);

  const auto started = store.value()->beginAttempt(verificationId);
  ASSERT_TRUE(started.ok()) << started.error().message;
  const auto cancelWhileActive = store.value()->cancel(verificationId);
  ASSERT_FALSE(cancelWhileActive.ok());
  EXPECT_EQ(cancelWhileActive.error().code,
            bte::core::ErrorCode::invalidArgument);

  const auto assignedId = std::string(32, '2');
  const auto attached =
      store.value()->attachResultId(verificationId, assignedId);
  ASSERT_TRUE(attached.ok()) << attached.error().message;
  const auto conflictingFailure = store.value()->failAttempt(
      verificationId, "workerFailed", "worker stopped", std::string(32, '3'));
  ASSERT_FALSE(conflictingFailure.ok());
  EXPECT_EQ(conflictingFailure.error().code,
            bte::core::ErrorCode::invalidArgument);
  const auto conflictingCancellation = store.value()->cancelAttempt(
      verificationId, "cancel requested", std::string(32, '3'));
  ASSERT_FALSE(conflictingCancellation.ok());
  EXPECT_EQ(conflictingCancellation.error().code,
            bte::core::ErrorCode::invalidArgument);

  const auto stillActive = store.value()->load(verificationId);
  ASSERT_TRUE(stillActive.ok()) << stillActive.error().message;
  EXPECT_EQ(stillActive.value().status, VerificationStatus::running);
  ASSERT_EQ(stillActive.value().attempts.size(), 1U);
  EXPECT_EQ(stillActive.value().attempts.front().resultId, assignedId);
  EXPECT_FALSE(stillActive.value().attempts.front().error.has_value());

  const auto canceled = store.value()->cancelAttempt(verificationId, "");
  ASSERT_TRUE(canceled.ok()) << canceled.error().message;
  EXPECT_EQ(canceled.value().status, VerificationStatus::canceled);
  const VerificationAttempt expectedAttempt{
      .ordinal = 1,
      .resultId = assignedId,
      .terminalStatus = std::nullopt,
      .canonicalResultHash = std::nullopt,
      .error =
          VerificationError{
              .code = "canceled",
              .message = "Verification attempt was canceled",
          },
  };
  ASSERT_EQ(canceled.value().attempts.size(), 1U);
  EXPECT_EQ(canceled.value().attempts.front(), expectedAttempt);

  const auto rejectedRepeat =
      store.value()->failAttempt(verificationId, "workerFailed", "too late");
  ASSERT_FALSE(rejectedRepeat.ok());
  EXPECT_EQ(rejectedRepeat.error().code, bte::core::ErrorCode::invalidArgument);
  const auto persisted = store.value()->load(verificationId);
  ASSERT_TRUE(persisted.ok()) << persisted.error().message;
  ASSERT_EQ(persisted.value().attempts.size(), 1U);
  EXPECT_EQ(persisted.value().attempts.front(), expectedAttempt);
}

} // namespace
