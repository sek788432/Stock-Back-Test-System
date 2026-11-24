#include "Bte/Data/ReleaseSnapshot.h"
#include "Bte/Data/SegmentRetention.h"

#include "../Support/ScopedTemporaryDirectory.h"
#include "ReleaseSnapshotTestHooks.h"
#include "SegmentRetentionTestHooks.h"

#include "Bte/Core/Cancellation.h"
#include "Bte/Core/Digest.h"
#include "Bte/Core/Time.h"

#include <gtest/gtest.h>
#include <sqlite3.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <memory>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#if defined(__APPLE__) || defined(__linux__)
#include <sys/resource.h>
#endif

namespace {

class ScopedSnapshotFixture final {
public:
  explicit ScopedSnapshotFixture(std::string name)
      : temporaryDirectory_("bte-release-snapshot-" + name),
        root_(temporaryDirectory_.path()), source_(root_ / "Source"),
        store_(root_ / "Store") {
    std::filesystem::create_directories(source_);
  }

  ScopedSnapshotFixture(const ScopedSnapshotFixture &) = delete;
  ScopedSnapshotFixture &operator=(const ScopedSnapshotFixture &) = delete;
  ScopedSnapshotFixture(ScopedSnapshotFixture &&) = delete;
  ScopedSnapshotFixture &operator=(ScopedSnapshotFixture &&) = delete;

  void writeSymbol(const std::string &symbol, const std::string &rows) const {
    std::ofstream output{source_ / (symbol + ".csv")};
    output << "symbol,ts,open,high,low,close,volume,schemaName\n" << rows;
    if (!output) {
      throw std::runtime_error{"Unable to write snapshot CSV fixture"};
    }
  }

  void writeExtractedSymbol(const std::string &symbol,
                            const std::string &rows) const {
    std::ofstream output{source_ / (symbol + ".csv")};
    output << "symbol,ts,open,high,low,close,volume\n" << rows;
    if (!output) {
      throw std::runtime_error{"Unable to write extracted CSV fixture"};
    }
  }

  [[nodiscard]] bte::data::SnapshotBuildRequest
  buildRequest(std::vector<std::string> symbols = {"SYN"},
               const std::size_t rowsPerSegment = 2) const {
    return bte::data::SnapshotBuildRequest{
        .sourceDirectory = source_,
        .storeDirectory = store_,
        .symbols = std::move(symbols),
        .rowsPerSegment = rowsPerSegment,
        .calendarHash = std::string(64, 'a'),
        .splitManifestHash = std::string(64, 'b'),
    };
  }

  [[nodiscard]] const std::filesystem::path &store() const noexcept {
    return store_;
  }

  [[nodiscard]] const std::filesystem::path &source() const noexcept {
    return source_;
  }

private:
  bte::test_support::ScopedTemporaryDirectory temporaryDirectory_;
  std::filesystem::path root_;
  std::filesystem::path source_;
  std::filesystem::path store_;
};

constexpr auto threeHourlyRows =
    "SYN,2024-01-01 23:00:00+00:00,100.125,102,99.5,101.25,1200,ohlcv-1h\n"
    "SYN,2024-01-02 00:00:00+00:00,101.25,103.5,100,102.75,2300,ohlcv-1h\n"
    "SYN,2024-01-02 01:00:00+00:00,102.75,104,101.5,103.125,3400,ohlcv-1h\n";

constexpr auto threeExtractedHourlyRows =
    "SYN,2024-01-01 23:00:00+00:00,100.125,102,99.5,101.25,1200\n"
    "SYN,2024-01-02 00:00:00+00:00,101.25,103.5,100,102.75,2300\n"
    "SYN,2024-01-02 01:00:00+00:00,102.75,104,101.5,103.125,3400\n";

bte::core::Timestamp timestamp(const std::string &text) {
  return bte::core::time::parseIso8601(text).value();
}

std::string readFile(const std::filesystem::path &path) {
  std::ifstream input{path, std::ios::binary};
  return {std::istreambuf_iterator<char>{input},
          std::istreambuf_iterator<char>{}};
}

std::optional<std::uint64_t> peakResidentSetBytes() {
#if defined(__APPLE__) || defined(__linux__)
  rusage usage{};
  if (getrusage(RUSAGE_SELF, &usage) != 0) {
    return std::nullopt;
  }
#if defined(__APPLE__)
  return static_cast<std::uint64_t>(usage.ru_maxrss);
#else
  return static_cast<std::uint64_t>(usage.ru_maxrss) * 1'024U;
#endif
#else
  return std::nullopt;
#endif
}

void writeFile(const std::filesystem::path &path, const std::string &content) {
  std::ofstream output{path, std::ios::binary | std::ios::trunc};
  output.write(content.data(), static_cast<std::streamsize>(content.size()));
  ASSERT_TRUE(output) << path;
}

void writeSourceManifest(const std::filesystem::path &sourceDirectory,
                         const std::string &entries) {
  writeFile(sourceDirectory / "SourceManifest.btesource",
            "BTE-SOURCE-MANIFEST-V1\n" + entries);
}

void executeSql(const std::filesystem::path &path, const std::string &sql) {
  sqlite3 *rawDatabase = nullptr;
  ASSERT_EQ(sqlite3_open(path.string().c_str(), &rawDatabase), SQLITE_OK);
  const auto database = std::unique_ptr<sqlite3, decltype(&sqlite3_close)>{
      rawDatabase, &sqlite3_close};
  ASSERT_EQ(
      sqlite3_exec(database.get(), sql.c_str(), nullptr, nullptr, nullptr),
      SQLITE_OK);
}

void replaceFirst(std::string &text, const std::string &oldValue,
                  const std::string &newValue) {
  const auto position = text.find(oldValue);
  ASSERT_NE(position, std::string::npos) << oldValue;
  text.replace(position, oldValue.size(), newValue);
}

std::string publishManifest(const std::filesystem::path &store,
                            const std::string &manifest) {
  const auto snapshotId = bte::core::sha256(manifest);
  const auto directory = store / "Snapshots" / snapshotId;
  std::filesystem::create_directories(directory);
  writeFile(directory / "Manifest.btesnapshot", manifest);
  return snapshotId;
}

struct RetainedSegmentFixture final {
  std::filesystem::path store;
  std::string segmentId;
  std::unique_ptr<bte::data::SegmentRetentionStore> retention;
};

RetainedSegmentFixture
makeRetainedSegmentFixture(const std::filesystem::path &root,
                           const std::string &name) {
  const auto store = root / name;
  constexpr std::string_view bytes = "retention-catalog-segment";
  const auto segmentId = bte::core::sha256(bytes);
  std::filesystem::create_directories(store / "Segments");
  writeFile(store / "Segments" / (segmentId + ".btedata"), std::string{bytes});
  auto opened = bte::data::SegmentRetentionStore::open(store);
  if (!opened.ok()) {
    throw std::runtime_error{opened.error().message};
  }
  auto retention = std::move(opened).value();
  const auto acquired = retention->acquire("result-catalog", {segmentId});
  if (!acquired.ok()) {
    throw std::runtime_error{acquired.error().message};
  }
  return RetainedSegmentFixture{.store = store,
                                .segmentId = segmentId,
                                .retention = std::move(retention)};
}

std::string publishSegmentMutation(const ScopedSnapshotFixture &fixture,
                                   const bte::data::SnapshotBuildResult &built,
                                   std::string bytes) {
  const auto originalId = built.segments.front();
  const auto replacementId = bte::core::sha256(bytes);
  writeFile(fixture.store() / "Segments" / (replacementId + ".btedata"), bytes);
  auto manifest = readFile(fixture.store() / "Snapshots" / built.snapshotId /
                           "Manifest.btesnapshot");
  replaceFirst(manifest, originalId, replacementId);
  return publishManifest(fixture.store(), manifest);
}

} // namespace

TEST(ReleaseSnapshotTest, exactReadIgnoresUnreferencedMissingSegments) {
  ScopedSnapshotFixture fixture{"exact-unrelated-missing"};
  fixture.writeSymbol("SYN", threeHourlyRows);
  const auto built = bte::data::buildReleaseSnapshot(fixture.buildRequest());
  ASSERT_TRUE(built.ok());
  bte::data::testing::resetSnapshotSegmentReadCount();
  auto reader = bte::data::ReleaseSnapshotReader::open(
      fixture.store(), built.value().snapshotId);
  ASSERT_TRUE(reader.ok());
  EXPECT_EQ(bte::data::testing::snapshotSegmentReadCount(),
            built.value().segmentCount);
  const auto selected = reader.value()->select(
      {.symbols = {"SYN"},
       .range = {.start = timestamp("2024-01-02 01:00:00+00:00"),
                 .end = timestamp("2024-01-02 02:00:00+00:00")},
       .timeframe = "ohlcv-1h"});
  ASSERT_TRUE(selected.ok());
  ASSERT_EQ(selected.value().identity.spans.size(), 1U);
  ASSERT_TRUE(
      std::filesystem::remove(fixture.store() / "Segments" /
                              (built.value().segments.front() + ".btedata")));
  EXPECT_FALSE(bte::data::ReleaseSnapshotReader::open(fixture.store(),
                                                      built.value().snapshotId)
                   .ok());
  const auto exact = bte::data::ReleaseSnapshotReader::openExact(
      fixture.store(), selected.value().identity);
  ASSERT_TRUE(exact.ok()) << exact.error().message;
  const auto exactSummary = exact.value()->summary();
  EXPECT_EQ(exactSummary.symbols, (std::vector<std::string>{"SYN"}));
  EXPECT_EQ(exactSummary.barCount, 3U);
  const auto reopened = exact.value()->readExact(selected.value().identity);
  ASSERT_TRUE(reopened.ok());
  EXPECT_EQ(reopened.value().bars, selected.value().bars);
  EXPECT_FALSE(
      exact.value()
          ->select({.symbols = {"SYN"},
                    .range = {.start = timestamp("2024-01-01 00:00:00+00:00"),
                              .end = timestamp("2024-01-03 00:00:00+00:00")},
                    .timeframe = "ohlcv-1h"})
          .ok());
  writeFile(fixture.store() / "Segments" /
                (built.value().segments.back() + ".btedata"),
            "corrupt");
  EXPECT_FALSE(bte::data::ReleaseSnapshotReader::openExact(
                   fixture.store(), selected.value().identity)
                   .ok());
}

TEST(ReleaseSnapshotTest, removingSnapshotKeepsSegmentsPinnedByResult) {
  ScopedSnapshotFixture fixture{"remove-snapshot"};
  fixture.writeSymbol("SYN", threeHourlyRows);
  const auto built = bte::data::buildReleaseSnapshot(fixture.buildRequest());
  ASSERT_TRUE(built.ok());
  const auto fullReader = bte::data::ReleaseSnapshotReader::open(
      fixture.store(), built.value().snapshotId);
  ASSERT_TRUE(fullReader.ok());
  const auto selection = fullReader.value()->select(
      {.symbols = {"SYN"},
       .range = {.start = timestamp("2024-01-01 00:00:00+00:00"),
                 .end = timestamp("2024-01-03 00:00:00+00:00")},
       .timeframe = "ohlcv-1h"});
  ASSERT_TRUE(selection.ok());
  auto retention = bte::data::SegmentRetentionStore::open(fixture.store());
  ASSERT_TRUE(retention.ok());
  ASSERT_TRUE(
      retention.value()->acquire("result-a", built.value().segments).ok());
  EXPECT_FALSE(
      bte::data::removeReleaseSnapshot(fixture.store(), "invalid").ok());
  ASSERT_TRUE(bte::data::removeReleaseSnapshot(fixture.store(),
                                               built.value().snapshotId)
                  .ok());
  const auto archived = bte::data::ReleaseSnapshotReader::openExact(
      fixture.store(), selection.value().identity);
  ASSERT_TRUE(archived.ok()) << archived.error().message;
  EXPECT_EQ(
      archived.value()->readExact(selection.value().identity).value().bars,
      selection.value().bars);
  EXPECT_FALSE(bte::data::ReleaseSnapshotReader::open(fixture.store(),
                                                      built.value().snapshotId)
                   .ok());
  for (const auto &id : built.value().segments) {
    EXPECT_TRUE(std::filesystem::exists(fixture.store() / "Segments" /
                                        (id + ".btedata")));
  }
  EXPECT_FALSE(bte::data::removeReleaseSnapshot(fixture.store(),
                                                built.value().snapshotId)
                   .ok());
  const auto rebuilt = bte::data::buildReleaseSnapshot(fixture.buildRequest());
  ASSERT_TRUE(rebuilt.ok()) << rebuilt.error().message;
  EXPECT_EQ(rebuilt.value().snapshotId, built.value().snapshotId);
  ASSERT_TRUE(bte::data::removeReleaseSnapshot(fixture.store(),
                                               built.value().snapshotId)
                  .ok());
  EXPECT_TRUE(bte::data::ReleaseSnapshotReader::openExact(
                  fixture.store(), selection.value().identity)
                  .ok());
  const auto released = retention.value()->release("result-a");
  ASSERT_TRUE(released.ok());
  EXPECT_EQ(released.value().size(), built.value().segments.size());
}

TEST(ReleaseSnapshotTest, failedPublicationReleasesItsProvisionalSnapshotPins) {
  ScopedSnapshotFixture reference{"snapshot-publication-reference"};
  reference.writeSymbol("SYN", threeHourlyRows);
  const auto expected =
      bte::data::buildReleaseSnapshot(reference.buildRequest());
  ASSERT_TRUE(expected.ok());
  ScopedSnapshotFixture failed{"snapshot-publication-failed"};
  failed.writeSymbol("SYN", threeHourlyRows);
  bte::data::testing::failSnapshotAt(
      bte::data::testing::SnapshotFailurePoint::manifestWrite);
  const auto result = bte::data::buildReleaseSnapshot(failed.buildRequest());
  bte::data::testing::clearSnapshotFailure();
  ASSERT_FALSE(result.ok());
  auto retention = bte::data::SegmentRetentionStore::open(failed.store());
  ASSERT_TRUE(retention.ok());
  const auto pins =
      retention.value()->references("snapshot-" + expected.value().snapshotId);
  ASSERT_TRUE(pins.ok());
  EXPECT_TRUE(pins.value().empty());
  EXPECT_FALSE(std::filesystem::exists(failed.store() / "Snapshots" /
                                       expected.value().snapshotId));
  for (const auto &segmentId : expected.value().segments) {
    EXPECT_FALSE(std::filesystem::exists(failed.store() / "Segments" /
                                         (segmentId + ".btedata")));
    EXPECT_TRUE(std::filesystem::exists(failed.store() / "Trash" / "Segments" /
                                        (segmentId + ".btedata")));
  }
  const auto retry = bte::data::buildReleaseSnapshot(failed.buildRequest());
  ASSERT_TRUE(retry.ok()) << retry.error().message;
  EXPECT_EQ(retry.value().snapshotId, expected.value().snapshotId);
  EXPECT_TRUE(
      bte::data::removeReleaseSnapshot(failed.store(), retry.value().snapshotId)
          .ok());
}

TEST(ReleaseSnapshotTest,
     existingSnapshotDirectoryMustContainTheExpectedManifest) {
  ScopedSnapshotFixture reference{"existing-snapshot-reference"};
  reference.writeSymbol("SYN", threeHourlyRows);
  const auto expected =
      bte::data::buildReleaseSnapshot(reference.buildRequest());
  ASSERT_TRUE(expected.ok()) << expected.error().message;

  ScopedSnapshotFixture collision{"existing-snapshot-collision"};
  collision.writeSymbol("SYN", threeHourlyRows);
  const auto snapshotDirectory =
      collision.store() / "Snapshots" / expected.value().snapshotId;
  std::filesystem::create_directories(snapshotDirectory);

  const auto result = bte::data::buildReleaseSnapshot(collision.buildRequest());

  ASSERT_FALSE(result.ok());
  EXPECT_FALSE(
      std::filesystem::exists(snapshotDirectory / "Manifest.btesnapshot"));
  EXPECT_FALSE(
      std::filesystem::exists(collision.store() / "References.sqlite3"));
}

TEST(ReleaseSnapshotTest,
     rebuildingSnapshotRemovesMatchingTrashedSegmentDuplicates) {
  ScopedSnapshotFixture fixture{"rebuild-removes-duplicate-trash"};
  fixture.writeSymbol("SYN", threeHourlyRows);
  const auto original = bte::data::buildReleaseSnapshot(fixture.buildRequest());
  ASSERT_TRUE(original.ok()) << original.error().message;
  ASSERT_FALSE(original.value().segments.empty());

  const auto segmentId = original.value().segments.front();
  const auto active = fixture.store() / "Segments" / (segmentId + ".btedata");
  const auto trashed =
      fixture.store() / "Trash" / "Segments" / (segmentId + ".btedata");
  ASSERT_TRUE(bte::data::removeReleaseSnapshot(fixture.store(),
                                               original.value().snapshotId)
                  .ok());
  ASSERT_FALSE(std::filesystem::exists(active));
  ASSERT_TRUE(std::filesystem::exists(trashed));

  const auto restored = bte::data::buildReleaseSnapshot(fixture.buildRequest());

  ASSERT_TRUE(restored.ok()) << restored.error().message;
  EXPECT_EQ(restored.value().snapshotId, original.value().snapshotId);
  EXPECT_TRUE(std::filesystem::exists(active));
  EXPECT_FALSE(std::filesystem::exists(trashed));

  ASSERT_TRUE(bte::data::removeReleaseSnapshot(fixture.store(),
                                               restored.value().snapshotId)
                  .ok());
  ASSERT_FALSE(std::filesystem::exists(active));
  ASSERT_TRUE(std::filesystem::exists(trashed));
  std::filesystem::copy_file(trashed, active);

  const auto rebuilt = bte::data::buildReleaseSnapshot(fixture.buildRequest());

  ASSERT_TRUE(rebuilt.ok()) << rebuilt.error().message;
  EXPECT_EQ(rebuilt.value().snapshotId, original.value().snapshotId);
  EXPECT_TRUE(std::filesystem::exists(active));
  EXPECT_FALSE(std::filesystem::exists(trashed));
}

TEST(ReleaseSnapshotTest,
     publicationRejectsCorruptTrashDuplicateAndRetriesAfterRepair) {
  ScopedSnapshotFixture fixture{"publication-corrupt-trash-duplicate"};
  fixture.writeSymbol("SYN", threeHourlyRows);
  const auto original = bte::data::buildReleaseSnapshot(fixture.buildRequest());
  ASSERT_TRUE(original.ok()) << original.error().message;
  auto secondRequest = fixture.buildRequest();
  secondRequest.calendarHash = std::string(64, 'c');
  const auto second = bte::data::buildReleaseSnapshot(secondRequest);
  ASSERT_TRUE(second.ok()) << second.error().message;
  ASSERT_NE(second.value().snapshotId, original.value().snapshotId);
  ASSERT_FALSE(original.value().segments.empty());

  const auto segmentId = original.value().segments.front();
  const auto active = fixture.store() / "Segments" / (segmentId + ".btedata");
  const auto trashed =
      fixture.store() / "Trash" / "Segments" / (segmentId + ".btedata");
  ASSERT_TRUE(std::filesystem::exists(active));
  std::filesystem::create_directories(trashed.parent_path());
  writeFile(trashed, "corrupted duplicate segment");
  auto thirdRequest = fixture.buildRequest();
  thirdRequest.splitManifestHash = std::string(64, 'd');

  const auto failed = bte::data::buildReleaseSnapshot(thirdRequest);

  ASSERT_FALSE(failed.ok());
  EXPECT_EQ(failed.error().code, bte::core::ErrorCode::dataSnapshotUnavailable);
  EXPECT_TRUE(std::filesystem::exists(active));
  EXPECT_EQ(readFile(trashed), "corrupted duplicate segment");

  writeFile(trashed, readFile(active));
  const auto retried = bte::data::buildReleaseSnapshot(thirdRequest);

  ASSERT_TRUE(retried.ok()) << retried.error().message;
  EXPECT_NE(retried.value().snapshotId, original.value().snapshotId);
  EXPECT_NE(retried.value().snapshotId, second.value().snapshotId);
  EXPECT_TRUE(std::filesystem::exists(active));
  EXPECT_FALSE(std::filesystem::exists(trashed));
}

TEST(ReleaseSnapshotTest,
     publicationConvertsTrashedSegmentLookupExceptionsToStructuredErrors) {
  ScopedSnapshotFixture fixture{"publication-trashed-segment-lookup-error"};
  fixture.writeSymbol("SYN", threeHourlyRows);
  const auto original = bte::data::buildReleaseSnapshot(fixture.buildRequest());
  ASSERT_TRUE(original.ok()) << original.error().message;
  ASSERT_FALSE(original.value().segments.empty());
  const auto active = fixture.store() / "Segments" /
                      (original.value().segments.front() + ".btedata");
  const auto trashed = fixture.store() / "Trash" / "Segments" /
                       (original.value().segments.front() + ".btedata");

  std::filesystem::create_directories(trashed.parent_path());
  std::filesystem::create_symlink(trashed, trashed);

  const auto rebuilt = bte::data::buildReleaseSnapshot(fixture.buildRequest());

  ASSERT_FALSE(rebuilt.ok());
  EXPECT_EQ(rebuilt.error().code, bte::core::ErrorCode::permissionDenied);
  EXPECT_NE(rebuilt.error().message.find("Unable to publish Release Snapshot"),
            std::string::npos)
      << rebuilt.error().message;
  EXPECT_TRUE(std::filesystem::exists(fixture.store() / "Snapshots" /
                                      original.value().snapshotId));
  EXPECT_TRUE(std::filesystem::exists(active));
  EXPECT_TRUE(
      std::filesystem::is_symlink(std::filesystem::symlink_status(trashed)));
}

TEST(ReleaseSnapshotTest,
     nextPublicationReclaimsPinsLeftByAnInterruptedSnapshotBuild) {
  ScopedSnapshotFixture fixture{"interrupted-snapshot-recovery"};
  fixture.writeSymbol("SYN", threeHourlyRows);
  const auto original = bte::data::buildReleaseSnapshot(fixture.buildRequest());
  ASSERT_TRUE(original.ok()) << original.error().message;
  ASSERT_TRUE(bte::data::removeReleaseSnapshot(fixture.store(),
                                               original.value().snapshotId)
                  .ok());

  auto retention = bte::data::SegmentRetentionStore::open(fixture.store());
  ASSERT_TRUE(retention.ok()) << retention.error().message;
  const auto interruptedSnapshotId = std::string(64, 'f');
  ASSERT_TRUE(retention.value()
                  ->acquire("snapshot-" + interruptedSnapshotId,
                            original.value().segments)
                  .ok());
  for (const auto &segmentId : original.value().segments) {
    EXPECT_TRUE(std::filesystem::exists(fixture.store() / "Segments" /
                                        (segmentId + ".btedata")));
  }

  fixture.writeSymbol(
      "SYN",
      "SYN,2024-01-01 23:00:00+00:00,110,112,109,111,1200,ohlcv-1h\n"
      "SYN,2024-01-02 00:00:00+00:00,111,113.5,110,112.75,2300,ohlcv-1h\n"
      "SYN,2024-01-02 01:00:00+00:00,112.75,114,111.5,113.125,3400,ohlcv-1h\n");
  const auto next = bte::data::buildReleaseSnapshot(fixture.buildRequest());

  ASSERT_TRUE(next.ok()) << next.error().message;
  const auto stalePins =
      retention.value()->references("snapshot-" + interruptedSnapshotId);
  ASSERT_TRUE(stalePins.ok());
  EXPECT_TRUE(stalePins.value().empty());
  for (const auto &segmentId : original.value().segments) {
    EXPECT_FALSE(std::filesystem::exists(fixture.store() / "Segments" /
                                         (segmentId + ".btedata")));
    EXPECT_TRUE(std::filesystem::exists(fixture.store() / "Trash" / "Segments" /
                                        (segmentId + ".btedata")));
  }
}

TEST(ReleaseSnapshotTest,
     nextPublicationTrashesUnpinnedSegmentsLeftByAnInterruptedSnapshotBuild) {
  ScopedSnapshotFixture fixture{"interrupted-unpinned-segment-recovery"};
  fixture.writeSymbol("SYN", threeHourlyRows);
  const auto original = bte::data::buildReleaseSnapshot(fixture.buildRequest());
  ASSERT_TRUE(original.ok()) << original.error().message;
  ASSERT_TRUE(bte::data::removeReleaseSnapshot(fixture.store(),
                                               original.value().snapshotId)
                  .ok());

  std::filesystem::create_directories(fixture.store() / "Segments");
  for (const auto &segmentId : original.value().segments) {
    const auto trashed =
        fixture.store() / "Trash" / "Segments" / (segmentId + ".btedata");
    const auto active = fixture.store() / "Segments" / (segmentId + ".btedata");
    ASSERT_TRUE(std::filesystem::exists(trashed));
    std::filesystem::copy_file(trashed, active);
  }
  fixture.writeSymbol(
      "SYN",
      "SYN,2024-01-01 23:00:00+00:00,110,112,109,111,1200,ohlcv-1h\n"
      "SYN,2024-01-02 00:00:00+00:00,111,113.5,110,112.75,2300,ohlcv-1h\n"
      "SYN,2024-01-02 01:00:00+00:00,112.75,114,111.5,113.125,3400,ohlcv-1h\n");

  const auto next = bte::data::buildReleaseSnapshot(fixture.buildRequest());

  ASSERT_TRUE(next.ok()) << next.error().message;
  EXPECT_NE(next.value().snapshotId, original.value().snapshotId);
  for (const auto &segmentId : original.value().segments) {
    EXPECT_FALSE(std::filesystem::exists(fixture.store() / "Segments" /
                                         (segmentId + ".btedata")));
    EXPECT_TRUE(std::filesystem::exists(fixture.store() / "Trash" / "Segments" /
                                        (segmentId + ".btedata")));
  }
}

TEST(SegmentRetentionTest,
     abandonUnpublishedSnapshotValidatesAndRollsBackStorageFailures) {
  using bte::data::testing::RetentionFailurePoint;
  bte::test_support::ScopedTemporaryDirectory temporary{
      "abandon-unpublished-snapshot-failures"};
  const auto storeDirectory = temporary.path() / "Store";
  auto retention = bte::data::SegmentRetentionStore::open(storeDirectory);
  ASSERT_TRUE(retention.ok()) << retention.error().message;

  const auto invalidIdentity =
      retention.value()->abandonUnpublishedSnapshot("../invalid");
  ASSERT_FALSE(invalidIdentity.ok());
  EXPECT_EQ(invalidIdentity.error().code,
            bte::core::ErrorCode::invalidArgument);

  bte::data::testing::failRetentionAfter(RetentionFailurePoint::databaseOpen);
  const auto openFailure =
      retention.value()->abandonUnpublishedSnapshot(std::string(64, 'f'));
  bte::data::testing::clearRetentionFailure();
  ASSERT_FALSE(openFailure.ok());

  bte::data::testing::failRetentionAfter(RetentionFailurePoint::sqlExecution);
  const auto beginFailure =
      retention.value()->abandonUnpublishedSnapshot(std::string(64, 'e'));
  bte::data::testing::clearRetentionFailure();
  ASSERT_FALSE(beginFailure.ok());

  const auto absent =
      retention.value()->abandonUnpublishedSnapshot(std::string(64, 'a'));
  ASSERT_TRUE(absent.ok()) << absent.error().message;

  const auto publishedId = std::string(64, 'b');
  std::filesystem::create_directories(storeDirectory / "Snapshots" /
                                      publishedId);
  const auto published =
      retention.value()->abandonUnpublishedSnapshot(publishedId);
  ASSERT_FALSE(published.ok());
  EXPECT_EQ(published.error().code, bte::core::ErrorCode::invalidArgument);

  bte::data::testing::failRetentionAfter(RetentionFailurePoint::boundExecution);
  const auto deleteFailure =
      retention.value()->abandonUnpublishedSnapshot(std::string(64, 'c'));
  bte::data::testing::clearRetentionFailure();
  ASSERT_FALSE(deleteFailure.ok());

  bte::data::testing::failRetentionAfter(RetentionFailurePoint::sqlExecution,
                                         1);
  const auto commitFailure =
      retention.value()->abandonUnpublishedSnapshot(std::string(64, 'd'));
  bte::data::testing::clearRetentionFailure();
  ASSERT_FALSE(commitFailure.ok());
}

constexpr std::uint32_t databaseOpensBeforeSnapshotPinRelease = 5;

TEST(ReleaseSnapshotTest, failedRetentionReleaseRestoresSnapshotForRetry) {
  ScopedSnapshotFixture fixture{"remove-snapshot-release-failure"};
  fixture.writeSymbol("SYN", threeHourlyRows);
  const auto built = bte::data::buildReleaseSnapshot(fixture.buildRequest());
  ASSERT_TRUE(built.ok()) << built.error().message;

  // Skip three recovery opens, the retention open, and pin acquisition to fail
  // the release operation itself.
  bte::data::testing::failRetentionAfter(
      bte::data::testing::RetentionFailurePoint::databaseOpen,
      databaseOpensBeforeSnapshotPinRelease);
  const auto removed = bte::data::removeReleaseSnapshot(
      fixture.store(), built.value().snapshotId);
  bte::data::testing::clearRetentionFailure();

  ASSERT_FALSE(removed.ok());
  EXPECT_EQ(removed.error().code, bte::core::ErrorCode::internal);
  EXPECT_TRUE(std::filesystem::is_directory(fixture.store() / "Snapshots" /
                                            built.value().snapshotId));
  EXPECT_FALSE(std::filesystem::exists(fixture.store() / "ArchivedSnapshots" /
                                       built.value().snapshotId));
  EXPECT_TRUE(bte::data::ReleaseSnapshotReader::open(fixture.store(),
                                                     built.value().snapshotId)
                  .ok());

  const auto retried = bte::data::removeReleaseSnapshot(
      fixture.store(), built.value().snapshotId);
  ASSERT_TRUE(retried.ok()) << retried.error().message;
  EXPECT_FALSE(std::filesystem::exists(fixture.store() / "Snapshots" /
                                       built.value().snapshotId));
  EXPECT_TRUE(std::filesystem::is_directory(
      fixture.store() / "ArchivedSnapshots" / built.value().snapshotId));
}

TEST(ReleaseSnapshotTest,
     failedSnapshotRollbackLeavesTheArchivedSnapshotRecoverable) {
  ScopedSnapshotFixture fixture{"remove-snapshot-rollback-failure"};
  fixture.writeSymbol("SYN", threeHourlyRows);
  const auto built = bte::data::buildReleaseSnapshot(fixture.buildRequest());
  ASSERT_TRUE(built.ok()) << built.error().message;

  // Skip three recovery opens, the retention open, and pin acquisition to fail
  // the release operation itself.
  bte::data::testing::failRetentionAfter(
      bte::data::testing::RetentionFailurePoint::databaseOpen,
      databaseOpensBeforeSnapshotPinRelease);
  bte::data::testing::failSnapshotAt(
      bte::data::testing::SnapshotFailurePoint::snapshotRemovalRollback);
  const auto removed = bte::data::removeReleaseSnapshot(
      fixture.store(), built.value().snapshotId);
  bte::data::testing::clearRetentionFailure();
  bte::data::testing::clearSnapshotFailure();

  ASSERT_FALSE(removed.ok());
  EXPECT_EQ(removed.error().code, bte::core::ErrorCode::internal);
  EXPECT_NE(removed.error().message.find("Unable to restore snapshot"),
            std::string::npos);
  EXPECT_FALSE(std::filesystem::exists(fixture.store() / "Snapshots" /
                                       built.value().snapshotId));
  EXPECT_TRUE(std::filesystem::is_directory(
      fixture.store() / "ArchivedSnapshots" / built.value().snapshotId));
}

TEST(ReleaseSnapshotTest, removesSnapshotWithMatchingExistingArchive) {
  ScopedSnapshotFixture fixture{"matching-existing-snapshot-archive"};
  fixture.writeSymbol("SYN", threeHourlyRows);
  const auto built = bte::data::buildReleaseSnapshot(fixture.buildRequest());
  ASSERT_TRUE(built.ok()) << built.error().message;
  const auto manifest =
      readFile(fixture.store() / "Snapshots" / built.value().snapshotId /
               "Manifest.btesnapshot");
  const auto archivedDirectory =
      fixture.store() / "ArchivedSnapshots" / built.value().snapshotId;
  std::filesystem::create_directories(archivedDirectory);
  writeFile(archivedDirectory / "Manifest.btesnapshot", manifest);

  const auto removed = bte::data::removeReleaseSnapshot(
      fixture.store(), built.value().snapshotId);

  ASSERT_TRUE(removed.ok()) << removed.error().message;
  EXPECT_FALSE(std::filesystem::exists(fixture.store() / "Snapshots" /
                                       built.value().snapshotId));
  EXPECT_TRUE(std::filesystem::is_directory(archivedDirectory));
  EXPECT_EQ(readFile(archivedDirectory / "Manifest.btesnapshot"), manifest);
}

TEST(ReleaseSnapshotTest, removeReportsAnArchiveRootThatIsAFile) {
  ScopedSnapshotFixture fixture{"remove-archive-root-is-a-file"};
  fixture.writeSymbol("SYN", threeHourlyRows);
  const auto built = bte::data::buildReleaseSnapshot(fixture.buildRequest());
  ASSERT_TRUE(built.ok()) << built.error().message;
  writeFile(fixture.store() / "ArchivedSnapshots", "not a directory");

  const auto removed = bte::data::removeReleaseSnapshot(
      fixture.store(), built.value().snapshotId);

  ASSERT_FALSE(removed.ok());
  EXPECT_EQ(removed.error().code, bte::core::ErrorCode::permissionDenied);
  EXPECT_TRUE(std::filesystem::is_directory(fixture.store() / "Snapshots" /
                                            built.value().snapshotId));
}

TEST(ReleaseSnapshotTest, removeReportsAnExistingStagingDirectory) {
  ScopedSnapshotFixture fixture{"remove-existing-staging-directory"};
  fixture.writeSymbol("SYN", threeHourlyRows);
  const auto built = bte::data::buildReleaseSnapshot(fixture.buildRequest());
  ASSERT_TRUE(built.ok()) << built.error().message;
  const auto manifest =
      readFile(fixture.store() / "Snapshots" / built.value().snapshotId /
               "Manifest.btesnapshot");
  const auto archivedDirectory =
      fixture.store() / "ArchivedSnapshots" / built.value().snapshotId;
  std::filesystem::create_directories(archivedDirectory);
  writeFile(archivedDirectory / "Manifest.btesnapshot", manifest);
  std::filesystem::create_directories(
      fixture.store() / "Snapshots" /
      ("." + built.value().snapshotId + ".removing"));

  const auto removed = bte::data::removeReleaseSnapshot(
      fixture.store(), built.value().snapshotId);

  ASSERT_FALSE(removed.ok());
  EXPECT_EQ(removed.error().code, bte::core::ErrorCode::resourceBusy);
  EXPECT_TRUE(std::filesystem::is_directory(fixture.store() / "Snapshots" /
                                            built.value().snapshotId));
}

TEST(ReleaseSnapshotTest,
     removesSnapshotAfterRecoveringItsStaleRemovalStagingDirectory) {
  ScopedSnapshotFixture fixture{"remove-stale-removal-staging-directory"};
  fixture.writeSymbol("SYN", threeHourlyRows);
  const auto built = bte::data::buildReleaseSnapshot(fixture.buildRequest());
  ASSERT_TRUE(built.ok()) << built.error().message;
  const auto manifest =
      readFile(fixture.store() / "Snapshots" / built.value().snapshotId /
               "Manifest.btesnapshot");
  const auto archivedDirectory =
      fixture.store() / "ArchivedSnapshots" / built.value().snapshotId;
  std::filesystem::create_directories(archivedDirectory);
  writeFile(archivedDirectory / "Manifest.btesnapshot", manifest);
  const auto staleStagingDirectory =
      fixture.store() / "Snapshots" /
      ("." + built.value().snapshotId + ".removing");
  std::filesystem::create_directories(staleStagingDirectory);
  writeFile(staleStagingDirectory / "Manifest.btesnapshot", manifest);

  const auto removed = bte::data::removeReleaseSnapshot(
      fixture.store(), built.value().snapshotId);

  ASSERT_TRUE(removed.ok()) << removed.error().message;
  EXPECT_FALSE(std::filesystem::exists(fixture.store() / "Snapshots" /
                                       built.value().snapshotId));
  EXPECT_FALSE(std::filesystem::exists(staleStagingDirectory));
  EXPECT_EQ(readFile(archivedDirectory / "Manifest.btesnapshot"), manifest);
}

TEST(ReleaseSnapshotTest, removeReportsAnUnavailablePublicationLock) {
  bte::test_support::ScopedTemporaryDirectory temporary{
      "remove-publication-lock-unavailable"};
  const auto store = temporary.path() / "Store";
  const auto manifest =
      std::string{"BTE-SNAPSHOT-MANIFEST-V1\n"} +
      "profile=vendorExtendedHours\ntimeframe=ohlcv-1h\ncalendarHash=" +
      std::string(64, 'a') + "\nsplitManifestHash=" + std::string(64, 'b') +
      "\n";
  const auto snapshotId = publishManifest(store, manifest);
  std::filesystem::create_directories(store / "SnapshotOperations.sqlite3");

  const auto removed = bte::data::removeReleaseSnapshot(store, snapshotId);

  ASSERT_FALSE(removed.ok());
  EXPECT_EQ(removed.error().code, bte::core::ErrorCode::internal);
  EXPECT_TRUE(std::filesystem::is_directory(store / "Snapshots" / snapshotId));
}

TEST(ReleaseSnapshotTest, removeReportsRetentionStoreOpenFailure) {
  ScopedSnapshotFixture fixture{"remove-retention-open-failure"};
  fixture.writeSymbol("SYN", threeHourlyRows);
  const auto built = bte::data::buildReleaseSnapshot(fixture.buildRequest());
  ASSERT_TRUE(built.ok()) << built.error().message;
  bte::data::testing::failRetentionAfter(
      bte::data::testing::RetentionFailurePoint::databaseOpen);

  const auto removed = bte::data::removeReleaseSnapshot(
      fixture.store(), built.value().snapshotId);
  bte::data::testing::clearRetentionFailure();

  ASSERT_FALSE(removed.ok());
  EXPECT_EQ(removed.error().code, bte::core::ErrorCode::internal);
  EXPECT_TRUE(std::filesystem::is_directory(fixture.store() / "Snapshots" /
                                            built.value().snapshotId));
}

TEST(ReleaseSnapshotTest, removeReportsAContendedPublicationLock) {
  ScopedSnapshotFixture fixture{"remove-publication-lock-busy"};
  fixture.writeSymbol("SYN", threeHourlyRows);
  const auto built = bte::data::buildReleaseSnapshot(fixture.buildRequest());
  ASSERT_TRUE(built.ok()) << built.error().message;
  sqlite3 *lockDatabase = nullptr;
  const auto lockPath =
      (fixture.store() / "SnapshotOperations.sqlite3").string();
  ASSERT_EQ(sqlite3_open_v2(lockPath.c_str(), &lockDatabase,
                            SQLITE_OPEN_READWRITE | SQLITE_OPEN_FULLMUTEX,
                            nullptr),
            SQLITE_OK);
  ASSERT_EQ(
      sqlite3_exec(lockDatabase, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr),
      SQLITE_OK);

  const auto removed = bte::data::removeReleaseSnapshot(
      fixture.store(), built.value().snapshotId);

  EXPECT_EQ(sqlite3_exec(lockDatabase, "ROLLBACK", nullptr, nullptr, nullptr),
            SQLITE_OK);
  EXPECT_EQ(sqlite3_close(lockDatabase), SQLITE_OK);
  ASSERT_FALSE(removed.ok());
  EXPECT_EQ(removed.error().code, bte::core::ErrorCode::resourceBusy);
}

TEST(ReleaseSnapshotTest, removeReportsRetentionPinFailure) {
  ScopedSnapshotFixture fixture{"remove-retention-pin-failure"};
  fixture.writeSymbol("SYN", threeHourlyRows);
  const auto built = bte::data::buildReleaseSnapshot(fixture.buildRequest());
  ASSERT_TRUE(built.ok()) << built.error().message;
  bte::data::testing::failRetentionAfter(
      bte::data::testing::RetentionFailurePoint::boundExecution);

  const auto removed = bte::data::removeReleaseSnapshot(
      fixture.store(), built.value().snapshotId);
  bte::data::testing::clearRetentionFailure();

  ASSERT_FALSE(removed.ok());
  EXPECT_EQ(removed.error().code, bte::core::ErrorCode::internal);
  EXPECT_TRUE(std::filesystem::is_directory(fixture.store() / "Snapshots" /
                                            built.value().snapshotId));
}

TEST(ReleaseSnapshotTest, exactReadCanOpenSnapshotAfterItHasBeenArchived) {
  ScopedSnapshotFixture fixture{"exact-read-archived-snapshot"};
  fixture.writeSymbol("SYN", threeHourlyRows);
  const auto built = bte::data::buildReleaseSnapshot(fixture.buildRequest());
  ASSERT_TRUE(built.ok()) << built.error().message;
  auto reader = bte::data::ReleaseSnapshotReader::open(
      fixture.store(), built.value().snapshotId);
  ASSERT_TRUE(reader.ok()) << reader.error().message;
  const auto selected = reader.value()->select(
      {.symbols = {"SYN"},
       .range = {.start = timestamp("2024-01-01 00:00:00+00:00"),
                 .end = timestamp("2024-01-03 00:00:00+00:00")},
       .timeframe = "ohlcv-1h"});
  ASSERT_TRUE(selected.ok()) << selected.error().message;
  auto retention = bte::data::SegmentRetentionStore::open(fixture.store());
  ASSERT_TRUE(retention.ok()) << retention.error().message;
  const auto retained = retention.value()->acquire(
      "exact-read-archive-reference", built.value().segments);
  ASSERT_TRUE(retained.ok()) << retained.error().message;
  const auto removed = bte::data::removeReleaseSnapshot(
      fixture.store(), built.value().snapshotId);
  ASSERT_TRUE(removed.ok()) << removed.error().message;

  auto exact = bte::data::ReleaseSnapshotReader::openExact(
      fixture.store(), selected.value().identity);

  ASSERT_TRUE(exact.ok()) << exact.error().message;
  const auto exactSelection =
      exact.value()->readExact(selected.value().identity);
  ASSERT_TRUE(exactSelection.ok()) << exactSelection.error().message;
  EXPECT_EQ(exactSelection.value().bars, selected.value().bars);
}

TEST(ReleaseSnapshotTest, removeAcceptsAnEmptyPublishedSnapshot) {
  ScopedSnapshotFixture fixture{"remove-empty-snapshot"};
  fixture.writeSymbol("SYN", "");
  const auto built = bte::data::buildReleaseSnapshot(fixture.buildRequest());
  ASSERT_TRUE(built.ok()) << built.error().message;
  EXPECT_EQ(built.value().segmentCount, 0);

  const auto removed = bte::data::removeReleaseSnapshot(
      fixture.store(), built.value().snapshotId);

  ASSERT_TRUE(removed.ok()) << removed.error().message;
  EXPECT_FALSE(std::filesystem::exists(fixture.store() / "Snapshots" /
                                       built.value().snapshotId));
}

TEST(ReleaseSnapshotTest, snapshotPublicationReportsRetentionPinFailures) {
  using bte::data::testing::RetentionFailurePoint;

  ScopedSnapshotFixture reference{"publication-retention-reference"};
  reference.writeSymbol("SYN", threeHourlyRows);
  const auto expected =
      bte::data::buildReleaseSnapshot(reference.buildRequest());
  ASSERT_TRUE(expected.ok()) << expected.error().message;

  const auto expectAbandonedSegmentsInTrash =
      [&expected](const ScopedSnapshotFixture &fixture) {
        auto retention =
            bte::data::SegmentRetentionStore::open(fixture.store());
        ASSERT_TRUE(retention.ok()) << retention.error().message;
        const auto pins = retention.value()->references(
            "snapshot-" + expected.value().snapshotId);
        ASSERT_TRUE(pins.ok()) << pins.error().message;
        EXPECT_TRUE(pins.value().empty());
        EXPECT_FALSE(std::filesystem::exists(fixture.store() / "Snapshots" /
                                             expected.value().snapshotId));
        for (const auto &segmentId : expected.value().segments) {
          EXPECT_FALSE(std::filesystem::exists(fixture.store() / "Segments" /
                                               (segmentId + ".btedata")));
          EXPECT_TRUE(
              std::filesystem::exists(fixture.store() / "Trash" / "Segments" /
                                      (segmentId + ".btedata")));
        }
      };

  ScopedSnapshotFixture openFailure{"publication-retention-open-failure"};
  openFailure.writeSymbol("SYN", threeHourlyRows);
  bte::data::testing::failRetentionAfter(RetentionFailurePoint::databaseOpen);
  const auto failedOpen =
      bte::data::buildReleaseSnapshot(openFailure.buildRequest());
  bte::data::testing::clearRetentionFailure();
  ASSERT_FALSE(failedOpen.ok());
  EXPECT_EQ(failedOpen.error().code, bte::core::ErrorCode::internal);
  expectAbandonedSegmentsInTrash(openFailure);

  ScopedSnapshotFixture acquireFailure{"publication-retention-pin-failure"};
  acquireFailure.writeSymbol("SYN", threeHourlyRows);
  bte::data::testing::failRetentionAfter(RetentionFailurePoint::sqlExecution,
                                         4);
  const auto failedPin =
      bte::data::buildReleaseSnapshot(acquireFailure.buildRequest());
  bte::data::testing::clearRetentionFailure();
  ASSERT_FALSE(failedPin.ok());
  EXPECT_EQ(failedPin.error().code, bte::core::ErrorCode::internal);
  expectAbandonedSegmentsInTrash(acquireFailure);
}

TEST(ReleaseSnapshotTest,
     failedSegmentWriteLeavesNoPartialArtifactAndCanBeRetried) {
  ScopedSnapshotFixture reference{"partial-segment-write-reference"};
  reference.writeSymbol("SYN", threeHourlyRows);
  const auto expected =
      bte::data::buildReleaseSnapshot(reference.buildRequest());
  ASSERT_TRUE(expected.ok()) << expected.error().message;

  ScopedSnapshotFixture fixture{"partial-segment-write-retry"};
  fixture.writeSymbol("SYN", threeHourlyRows);
  bte::data::testing::failSnapshotAt(
      bte::data::testing::SnapshotFailurePoint::artifactWrite);
  const auto failed = bte::data::buildReleaseSnapshot(fixture.buildRequest());
  bte::data::testing::clearSnapshotFailure();

  ASSERT_FALSE(failed.ok());
  EXPECT_FALSE(std::filesystem::exists(fixture.store() / "Snapshots" /
                                       expected.value().snapshotId));
  for (const auto &segmentId : expected.value().segments) {
    EXPECT_FALSE(std::filesystem::exists(fixture.store() / "Segments" /
                                         (segmentId + ".btedata")));
    EXPECT_FALSE(std::filesystem::exists(fixture.store() / "Segments" /
                                         (segmentId + ".btedata.staging")));
  }

  const auto retried = bte::data::buildReleaseSnapshot(fixture.buildRequest());

  ASSERT_TRUE(retried.ok()) << retried.error().message;
  EXPECT_EQ(retried.value().snapshotId, expected.value().snapshotId);
}

TEST(ReleaseSnapshotTest, snapshotBuildReportsNonEmptySegmentStagingPath) {
  ScopedSnapshotFixture fixture{"non-empty-segment-staging-path"};
  fixture.writeSymbol("SYN", threeHourlyRows);
  const auto built = bte::data::buildReleaseSnapshot(fixture.buildRequest());
  ASSERT_TRUE(built.ok()) << built.error().message;
  ASSERT_FALSE(built.value().segments.empty());
  const auto segmentId = built.value().segments.front();
  const auto active = fixture.store() / "Segments" / (segmentId + ".btedata");
  const auto trashed =
      fixture.store() / "Trash" / "Segments" / (segmentId + ".btedata");
  ASSERT_TRUE(bte::data::removeReleaseSnapshot(fixture.store(),
                                               built.value().snapshotId)
                  .ok());
  ASSERT_TRUE(std::filesystem::exists(trashed));
  std::filesystem::remove(trashed);
  auto staging = active;
  staging += ".staging";
  std::filesystem::create_directories(staging);
  writeFile(staging / "preserve.txt", "staging cleanup must not erase this");

  const auto rebuilt = bte::data::buildReleaseSnapshot(fixture.buildRequest());

  ASSERT_FALSE(rebuilt.ok());
  EXPECT_EQ(rebuilt.error().code, bte::core::ErrorCode::permissionDenied);
  EXPECT_NE(rebuilt.error().message.find("clear snapshot artifact staging"),
            std::string::npos);
  EXPECT_FALSE(std::filesystem::exists(active));
  EXPECT_TRUE(std::filesystem::exists(staging / "preserve.txt"));
}

TEST(ReleaseSnapshotTest,
     failedEmptySnapshotPublicationDoesNotRequireRetentionStore) {
  ScopedSnapshotFixture fixture{"empty-snapshot-publication-failure"};
  fixture.writeSymbol("SYN", "");
  bte::data::testing::failSnapshotAt(
      bte::data::testing::SnapshotFailurePoint::manifestWrite);

  const auto failed = bte::data::buildReleaseSnapshot(fixture.buildRequest());

  bte::data::testing::clearSnapshotFailure();
  ASSERT_FALSE(failed.ok());
  EXPECT_EQ(failed.error().code, bte::core::ErrorCode::permissionDenied);
}

TEST(ReleaseSnapshotTest,
     failedUnpublishedSnapshotCleanupReportsRetentionFailure) {
  ScopedSnapshotFixture fixture{"publication-cleanup-retention-failure"};
  fixture.writeSymbol("SYN", threeHourlyRows);
  bte::data::testing::failRetentionAfter(
      bte::data::testing::RetentionFailurePoint::databaseOpen, 2);
  bte::data::testing::failSnapshotAt(
      bte::data::testing::SnapshotFailurePoint::manifestWrite);

  const auto failed = bte::data::buildReleaseSnapshot(fixture.buildRequest());

  bte::data::testing::clearRetentionFailure();
  bte::data::testing::clearSnapshotFailure();
  ASSERT_FALSE(failed.ok());
  EXPECT_EQ(failed.error().code, bte::core::ErrorCode::internal);
  EXPECT_NE(
      failed.error().message.find("Unable to open segment retention database"),
      std::string::npos);
  const auto snapshotsDirectory = fixture.store() / "Snapshots";
  EXPECT_TRUE(!std::filesystem::exists(snapshotsDirectory) ||
              std::filesystem::is_empty(snapshotsDirectory));
}

TEST(ReleaseSnapshotTest,
     publishedSnapshotSegmentCatalogRejectsMalformedEntries) {
  bte::test_support::ScopedTemporaryDirectory temporary{
      "published-snapshot-catalog-errors"};
  const auto missing =
      makeRetainedSegmentFixture(temporary.path(), "MissingSnapshots");
  const auto releasedMissing = missing.retention->release("result-catalog");
  ASSERT_TRUE(releasedMissing.ok()) << releasedMissing.error().message;
  EXPECT_EQ(releasedMissing.value().size(), 1U);

  const auto invalidName =
      makeRetainedSegmentFixture(temporary.path(), "InvalidName");
  std::filesystem::create_directories(invalidName.store / "Snapshots" /
                                      "not-a-hash");
  const auto releasedInvalidName =
      invalidName.retention->release("result-catalog");
  ASSERT_TRUE(releasedInvalidName.ok()) << releasedInvalidName.error().message;

  const auto notDirectory =
      makeRetainedSegmentFixture(temporary.path(), "SnapshotsIsAFile");
  writeFile(notDirectory.store / "Snapshots", "not a directory");
  const auto iteratorError = notDirectory.retention->release("result-catalog");
  ASSERT_FALSE(iteratorError.ok());

  const auto hashNamedFile =
      makeRetainedSegmentFixture(temporary.path(), "HashNamedFile");
  const auto hashFile =
      hashNamedFile.store / "Snapshots" / std::string(64, 'a');
  std::filesystem::create_directories(hashFile.parent_path());
  writeFile(hashFile, "not a snapshot directory");
  const auto notDirectoryError =
      hashNamedFile.retention->release("result-catalog");
  ASSERT_FALSE(notDirectoryError.ok());

  const auto missingManifest =
      makeRetainedSegmentFixture(temporary.path(), "MissingManifest");
  std::filesystem::create_directories(missingManifest.store / "Snapshots" /
                                      std::string(64, 'b'));
  const auto manifestError =
      missingManifest.retention->release("result-catalog");
  ASSERT_FALSE(manifestError.ok());

  const auto mismatched =
      makeRetainedSegmentFixture(temporary.path(), "MismatchedManifest");
  const auto mismatchedDirectory =
      mismatched.store / "Snapshots" / std::string(64, 'c');
  std::filesystem::create_directories(mismatchedDirectory);
  writeFile(mismatchedDirectory / "Manifest.btesnapshot", "mismatched");
  const auto mismatchedError = mismatched.retention->release("result-catalog");
  ASSERT_FALSE(mismatchedError.ok());

  const auto malformed =
      makeRetainedSegmentFixture(temporary.path(), "MalformedManifest");
  publishManifest(malformed.store, "not-a-manifest\n");
  const auto parseError = malformed.retention->release("result-catalog");
  ASSERT_FALSE(parseError.ok());

  const auto invalidSegment =
      makeRetainedSegmentFixture(temporary.path(), "InvalidSegmentIdentity");
  const auto invalidManifest = std::string{"BTE-SNAPSHOT-MANIFEST-V1\n"} +
                               "profile=vendorExtendedHours\n"
                               "timeframe=ohlcv-1h\n"
                               "calendarHash=" +
                               std::string(64, 'a') +
                               "\n"
                               "splitManifestHash=" +
                               std::string(64, 'b') +
                               "\n"
                               "segment=0|SYN|" +
                               std::string(64, 'z') +
                               "|1|1704150000000|1704150000000\n";
  publishManifest(invalidSegment.store, invalidManifest);
  const auto segmentIdentityError =
      invalidSegment.retention->release("result-catalog");
  ASSERT_FALSE(segmentIdentityError.ok());
  EXPECT_NE(
      segmentIdentityError.error().message.find("segment identity is invalid"),
      std::string::npos);
}

TEST(ReleaseSnapshotTest, removalRejectsManifestThatCannotBeParsed) {
  bte::test_support::ScopedTemporaryDirectory temporary{
      "remove-unparseable-manifest"};
  const auto store = temporary.path() / "Store";
  const auto snapshotId = publishManifest(store, "not-a-manifest\n");

  const auto removed = bte::data::removeReleaseSnapshot(store, snapshotId);

  ASSERT_FALSE(removed.ok());
  EXPECT_EQ(removed.error().code,
            bte::core::ErrorCode::dataSnapshotUnavailable);
}

TEST(ReleaseSnapshotTest, exactReaderRejectsRangeAndHistoryOperations) {
  ScopedSnapshotFixture fixture{"exact-reader-operation-contract"};
  fixture.writeSymbol("SYN", threeHourlyRows);
  const auto built = bte::data::buildReleaseSnapshot(fixture.buildRequest());
  ASSERT_TRUE(built.ok()) << built.error().message;
  auto reader = bte::data::ReleaseSnapshotReader::open(
      fixture.store(), built.value().snapshotId);
  ASSERT_TRUE(reader.ok()) << reader.error().message;
  const auto selected = reader.value()->select(
      {.symbols = {"SYN"},
       .range = {.start = timestamp("2024-01-01 00:00:00+00:00"),
                 .end = timestamp("2024-01-03 00:00:00+00:00")},
       .timeframe = "ohlcv-1h"});
  ASSERT_TRUE(selected.ok()) << selected.error().message;
  auto exact = bte::data::ReleaseSnapshotReader::openExact(
      fixture.store(), selected.value().identity);
  ASSERT_TRUE(exact.ok()) << exact.error().message;

  const auto lastClose = exact.value()->lastCloseBeforeUtcDate(
      "SYN", timestamp("2024-01-02 00:00:00+00:00"));
  const auto lastDays = exact.value()->lastDataDaysBeforeUtcDate(
      "SYN", timestamp("2024-01-02 00:00:00+00:00"), 1);

  ASSERT_FALSE(lastClose.ok());
  EXPECT_EQ(lastClose.error().code, bte::core::ErrorCode::invalidArgument);
  ASSERT_FALSE(lastDays.ok());
  EXPECT_EQ(lastDays.error().code, bte::core::ErrorCode::invalidArgument);
}

TEST(ReleaseSnapshotTest,
     buildAndSelect_preservesFixedPointBarsAndExactHalfOpenRowSpans) {
  ScopedSnapshotFixture fixture{"selection"};
  fixture.writeSymbol("SYN", threeHourlyRows);

  const auto built = bte::data::buildReleaseSnapshot(fixture.buildRequest());
  ASSERT_TRUE(built.ok()) << built.error().message;
  EXPECT_EQ(built.value().segmentCount, 2);
  EXPECT_EQ(built.value().barCount, 3);
  EXPECT_EQ(built.value().snapshotId.size(), 64);

  auto reader = bte::data::ReleaseSnapshotReader::open(
      fixture.store(), built.value().snapshotId);
  ASSERT_TRUE(reader.ok()) << reader.error().message;
  const auto summary = reader.value()->summary();
  EXPECT_EQ(summary.snapshotId, built.value().snapshotId);
  EXPECT_EQ(summary.profile, "vendorExtendedHours");
  EXPECT_EQ(summary.timeframe, "ohlcv-1h");
  EXPECT_EQ(summary.symbols, (std::vector<std::string>{"SYN"}));
  EXPECT_EQ(summary.barCount, 3U);

  const auto selected = reader.value()->select(bte::data::SnapshotSelectRequest{
      .symbols = {"SYN"},
      .range = {.start = timestamp("2024-01-02 00:00:00+00:00"),
                .end = timestamp("2024-01-02 02:00:00+00:00")},
      .timeframe = "ohlcv-1h",
  });

  ASSERT_TRUE(selected.ok()) << selected.error().message;
  ASSERT_EQ(selected.value().bars.size(), 2);
  EXPECT_EQ(selected.value().bars[0].timestamp,
            timestamp("2024-01-02 00:00:00+00:00"));
  EXPECT_EQ(selected.value().bars[0].openNanodollars, 101'250'000'000);
  EXPECT_EQ(selected.value().bars[0].highNanodollars, 103'500'000'000);
  EXPECT_EQ(selected.value().bars[0].lowNanodollars, 100'000'000'000);
  EXPECT_EQ(selected.value().bars[0].closeNanodollars, 102'750'000'000);
  EXPECT_EQ(selected.value().bars[0].volumeMicroshares, 2'300'000'000);
  EXPECT_EQ(selected.value().bars[1].closeNanodollars, 103'125'000'000);

  ASSERT_EQ(selected.value().identity.spans.size(), 2);
  EXPECT_EQ(selected.value().identity.spans[0].segmentOrdinal, 0);
  EXPECT_EQ(selected.value().identity.spans[0].firstRow, 1);
  EXPECT_EQ(selected.value().identity.spans[0].rowCount, 1);
  EXPECT_EQ(selected.value().identity.spans[1].segmentOrdinal, 1);
  EXPECT_EQ(selected.value().identity.spans[1].firstRow, 0);
  EXPECT_EQ(selected.value().identity.spans[1].rowCount, 1);
  EXPECT_EQ(selected.value().identity.snapshotId, built.value().snapshotId);
}

TEST(ReleaseSnapshotTest,
     releaseSnapshotSelectionReportsOpenSelectHistoryAndSpanMetrics) {
#if !defined(NDEBUG)
  GTEST_SKIP() << "P04 performance sampling runs in the Release profile";
#endif
  ScopedSnapshotFixture fixture{"selection-stage-measurement"};
  constexpr auto barsPerSymbol = 10'000;
  const auto firstTimestamp = timestamp("2020-01-01 00:00:00+00:00");
  for (const auto &symbol : {std::string{"BTE000"}, std::string{"BTE001"}}) {
    std::ofstream output{fixture.source() / (symbol + ".csv")};
    output << "symbol,ts,open,high,low,close,volume,schemaName\n";
    for (auto index = 0; index < barsPerSymbol; ++index) {
      const auto barTimestamp =
          firstTimestamp + std::chrono::hours{static_cast<std::int64_t>(index)};
      output << symbol << ',' << bte::core::time::toIso8601(barTimestamp)
             << ",100,102,99,101,1000,ohlcv-1h\n";
    }
    ASSERT_TRUE(output.good()) << symbol;
  }
  const auto built = bte::data::buildReleaseSnapshot(
      fixture.buildRequest({"BTE000", "BTE001"}, 1'000));
  ASSERT_TRUE(built.ok()) << built.error().message;
  ASSERT_EQ(built.value().barCount, barsPerSymbol * 2U);
  ASSERT_EQ(built.value().segmentCount, 20U);

  const auto rangeStart = firstTimestamp + std::chrono::hours{8'904};
  const auto rangeEnd = rangeStart + std::chrono::hours{200};
  const auto historyStart = rangeStart - std::chrono::hours{24 * 10};
  const bte::data::SnapshotSelectRequest request{
      .symbols = {"BTE000"},
      .range = {.start = rangeStart, .end = rangeEnd},
      .timeframe = "ohlcv-1h",
  };
  constexpr auto warmupCount = 2;
  constexpr auto measuredCount = 20;
  std::vector<std::int64_t> openMicroseconds;
  std::vector<std::int64_t> selectMicroseconds;
  std::vector<std::int64_t> lastCloseMicroseconds;
  std::vector<std::int64_t> historyDaysMicroseconds;
  openMicroseconds.reserve(measuredCount);
  selectMicroseconds.reserve(measuredCount);
  lastCloseMicroseconds.reserve(measuredCount);
  historyDaysMicroseconds.reserve(measuredCount);
  const auto peakRssBefore = peakResidentSetBytes();
  bte::data::testing::resetSnapshotSegmentReadCount();
  std::vector<std::size_t> lastSpanWidths;

  for (auto sample = 0; sample < warmupCount + measuredCount; ++sample) {
    auto started = std::chrono::steady_clock::now();
    auto reader = bte::data::ReleaseSnapshotReader::open(
        fixture.store(), built.value().snapshotId);
    const auto openedAt = std::chrono::steady_clock::now();
    ASSERT_TRUE(reader.ok()) << reader.error().message;

    const auto selected = reader.value()->select(request);
    const auto selectedAt = std::chrono::steady_clock::now();
    ASSERT_TRUE(selected.ok()) << selected.error().message;
    ASSERT_EQ(selected.value().bars.size(), 200U)
        << bte::core::time::toIso8601(rangeStart) << ".."
        << bte::core::time::toIso8601(rangeEnd)
        << " spans=" << selected.value().identity.spans.size();
    ASSERT_EQ(selected.value().identity.spans.size(), 2U);
    ASSERT_EQ(selected.value().identity.spans[0].rowCount, 96U);
    ASSERT_EQ(selected.value().identity.spans[1].rowCount, 104U);

    const auto priorClose =
        reader.value()->lastCloseBeforeUtcDate("BTE000", rangeStart);
    const auto priorCloseAt = std::chrono::steady_clock::now();
    ASSERT_TRUE(priorClose.ok()) << priorClose.error().message;
    ASSERT_TRUE(priorClose.value().has_value());
    EXPECT_EQ(priorClose.value()->timestamp,
              firstTimestamp + std::chrono::hours{8'903});

    const auto history =
        reader.value()->lastDataDaysBeforeUtcDate("BTE000", rangeStart, 10);
    const auto historyAt = std::chrono::steady_clock::now();
    ASSERT_TRUE(history.ok()) << history.error().message;
    ASSERT_EQ(history.value().size(), 10U);
    EXPECT_EQ(history.value().front().utcDateStart,
              std::chrono::floor<std::chrono::days>(historyStart));

    if (sample == warmupCount + measuredCount - 1) {
      lastSpanWidths = {selected.value().identity.spans[0].rowCount,
                        selected.value().identity.spans[1].rowCount};
    }
    if (sample >= warmupCount) {
      openMicroseconds.push_back(
          std::chrono::duration_cast<std::chrono::microseconds>(openedAt -
                                                                started)
              .count());
      selectMicroseconds.push_back(
          std::chrono::duration_cast<std::chrono::microseconds>(selectedAt -
                                                                openedAt)
              .count());
      lastCloseMicroseconds.push_back(
          std::chrono::duration_cast<std::chrono::microseconds>(priorCloseAt -
                                                                selectedAt)
              .count());
      historyDaysMicroseconds.push_back(
          std::chrono::duration_cast<std::chrono::microseconds>(historyAt -
                                                                priorCloseAt)
              .count());
    }
  }

  const auto nearestRank = [](std::vector<std::int64_t> samples,
                              const std::size_t rank) {
    std::ranges::sort(samples);
    return samples.at(rank - 1U);
  };
  const auto rawSamples = [](const std::vector<std::int64_t> &samples) {
    auto raw = std::string{};
    for (const auto sample : samples) {
      if (!raw.empty()) {
        raw.append(",");
      }
      raw.append(std::to_string(sample));
    }
    return raw;
  };
  const auto recordStage =
      [&nearestRank, &rawSamples](const std::string &stage,
                                  const std::vector<std::int64_t> &samples) {
        RecordProperty(stage + "P50Microseconds", nearestRank(samples, 10U));
        RecordProperty(stage + "P95Microseconds", nearestRank(samples, 19U));
        RecordProperty(stage + "RawMicroseconds", rawSamples(samples));
      };
  RecordProperty("fixtureBars", barsPerSymbol * 2U);
  RecordProperty("fixtureSymbols", 2);
  RecordProperty("fixtureSegments", built.value().segmentCount);
  RecordProperty("warmupCount", warmupCount);
  RecordProperty("measuredSampleCount", measuredCount);
  RecordProperty("selectionBars", 200);
  RecordProperty("selectionSpanCount", lastSpanWidths.size());
  RecordProperty("selectionSpanWidths",
                 rawSamples({static_cast<std::int64_t>(lastSpanWidths[0]),
                             static_cast<std::int64_t>(lastSpanWidths[1])}));
  RecordProperty("requiredPriorDataDays", 10);
  RecordProperty("requiredPriorHourlyRows", 240);
  recordStage("snapshotOpen", openMicroseconds);
  recordStage("rangeSelect", selectMicroseconds);
  recordStage("lastClose", lastCloseMicroseconds);
  recordStage("historyDays", historyDaysMicroseconds);
  EXPECT_EQ(bte::data::testing::snapshotSegmentReadCount(), 440U);
  RecordProperty("segmentReadsAcrossAllSamples",
                 bte::data::testing::snapshotSegmentReadCount());
  const auto peakRssAfter = peakResidentSetBytes();
  if (peakRssBefore.has_value() && peakRssAfter.has_value()) {
    RecordProperty("peakRssBeforeSamplingBytes", *peakRssBefore);
    RecordProperty("peakRssAfterSamplingBytes", *peakRssAfter);
  }
}

TEST(ReleaseSnapshotTest, lastCloseBeforeUtcDateExcludesRowsFromBoundaryDate) {
  const ScopedSnapshotFixture fixture{"prior-data-day-boundary"};
  fixture.writeSymbol(
      "SYN", "SYN,2024-01-05 20:00:00+00:00,99,101,98,100,100,ohlcv-1h\n"
             "SYN,2024-01-05 22:00:00+00:00,100,111,99,110,100,ohlcv-1h\n"
             "SYN,2024-01-08 00:00:00+00:00,110,121,109,120,100,ohlcv-1h\n"
             "SYN,2024-01-08 01:00:00+00:00,120,122,119,121,100,ohlcv-1h\n");
  const auto built = bte::data::buildReleaseSnapshot(fixture.buildRequest());
  ASSERT_TRUE(built.ok()) << built.error().message;
  auto reader = bte::data::ReleaseSnapshotReader::open(
      fixture.store(), built.value().snapshotId);
  ASSERT_TRUE(reader.ok()) << reader.error().message;

  const auto boundary = timestamp("2024-01-08 00:00:00+00:00");
  const auto history = reader.value()->lastCloseBeforeUtcDate("SYN", boundary);
  ASSERT_TRUE(history.ok()) << history.error().message;
  const auto expected = std::optional{bte::data::SnapshotHistoryClose{
      .timestamp = timestamp("2024-01-05 22:00:00+00:00"),
      .closeNanodollars = 110'000'000'000,
  }};
  EXPECT_EQ(history.value(), expected);

  const auto selection = reader.value()->select({
      .symbols = {"SYN"},
      .range = {.start = boundary,
                .end = timestamp("2024-01-08 02:00:00+00:00")},
      .timeframe = "ohlcv-1h",
  });
  ASSERT_TRUE(selection.ok()) << selection.error().message;
  ASSERT_EQ(selection.value().bars.size(), 2U);
  EXPECT_EQ(selection.value().bars.front().timestamp, boundary);
  EXPECT_FALSE(selection.value().preRangeDataDayClose.has_value());
}

TEST(ReleaseSnapshotTest,
     lastCloseBeforeUtcDateReturnsLastAvailableRowAcrossLongGap) {
  const ScopedSnapshotFixture fixture{"prior-data-day-long-gap"};
  fixture.writeSymbol(
      "SYN", "SYN,2024-01-02 20:00:00+00:00,99,101,98,100,100,ohlcv-1h\n"
             "SYN,2024-01-19 22:00:00+00:00,100,111,99,110,100,ohlcv-1h\n"
             "SYN,2024-03-15 22:00:00+00:00,110,121,109,120,100,ohlcv-1h\n");
  auto request = fixture.buildRequest();
  request.rowsPerSegment = 1;
  const auto built = bte::data::buildReleaseSnapshot(request);
  ASSERT_TRUE(built.ok()) << built.error().message;
  ASSERT_EQ(built.value().segmentCount, 3U);
  auto reader = bte::data::ReleaseSnapshotReader::open(
      fixture.store(), built.value().snapshotId);
  ASSERT_TRUE(reader.ok()) << reader.error().message;

  const auto history = reader.value()->lastCloseBeforeUtcDate(
      "SYN", timestamp("2024-04-30 00:00:00+00:00"));

  ASSERT_TRUE(history.ok()) << history.error().message;
  const auto expected = std::optional{bte::data::SnapshotHistoryClose{
      .timestamp = timestamp("2024-03-15 22:00:00+00:00"),
      .closeNanodollars = 120'000'000'000,
  }};
  EXPECT_EQ(history.value(), expected);
}

TEST(ReleaseSnapshotTest, lastCloseBeforeUtcDateReturnsEmptyWithoutPriorRows) {
  const ScopedSnapshotFixture fixture{"prior-data-day-empty"};
  fixture.writeSymbol(
      "SYN", "SYN,2024-01-08 00:00:00+00:00,110,121,109,120,100,ohlcv-1h\n"
             "SYN,2024-01-08 01:00:00+00:00,120,122,119,121,100,ohlcv-1h\n");
  const auto built = bte::data::buildReleaseSnapshot(fixture.buildRequest());
  ASSERT_TRUE(built.ok()) << built.error().message;
  auto reader = bte::data::ReleaseSnapshotReader::open(
      fixture.store(), built.value().snapshotId);
  ASSERT_TRUE(reader.ok()) << reader.error().message;

  const auto boundary = timestamp("2024-01-08 00:00:00+00:00");
  const auto history = reader.value()->lastCloseBeforeUtcDate("SYN", boundary);
  ASSERT_TRUE(history.ok()) << history.error().message;
  EXPECT_FALSE(history.value().has_value());

  const auto absentSymbol =
      reader.value()->lastCloseBeforeUtcDate("MISSING", boundary);
  ASSERT_TRUE(absentSymbol.ok()) << absentSymbol.error().message;
  EXPECT_FALSE(absentSymbol.value().has_value());
}

TEST(ReleaseSnapshotTest,
     lastDataDaysBeforeUtcDateAggregatesAvailableDatesAndExcludesBoundary) {
  const ScopedSnapshotFixture fixture{"historical-data-day-summaries"};
  fixture.writeSymbol(
      "SYN", "SYN,2024-01-01 20:00:00+00:00,99,105,95,100,100,ohlcv-1h\n"
             "SYN,2024-01-01 22:00:00+00:00,100,120,80,110,100,ohlcv-1h\n"
             "SYN,2024-01-02 20:00:00+00:00,110,115,108,112,100,ohlcv-1h\n"
             "SYN,2024-01-04 20:00:00+00:00,112,118,109,116,100,ohlcv-1h\n"
             "SYN,2024-01-05 00:00:00+00:00,116,999,1,117,100,ohlcv-1h\n");
  auto request = fixture.buildRequest();
  request.rowsPerSegment = 2;
  const auto built = bte::data::buildReleaseSnapshot(request);
  ASSERT_TRUE(built.ok()) << built.error().message;
  auto reader = bte::data::ReleaseSnapshotReader::open(
      fixture.store(), built.value().snapshotId);
  ASSERT_TRUE(reader.ok()) << reader.error().message;

  const auto summaries = reader.value()->lastDataDaysBeforeUtcDate(
      "SYN", timestamp("2024-01-05 00:00:00+00:00"), 3);

  ASSERT_TRUE(summaries.ok()) << summaries.error().message;
  const auto expected = std::vector<bte::data::SnapshotDataDaySummary>{
      {.utcDateStart = timestamp("2024-01-01 00:00:00+00:00"),
       .highNanodollars = 120'000'000'000,
       .lowNanodollars = 80'000'000'000,
       .closeNanodollars = 110'000'000'000},
      {.utcDateStart = timestamp("2024-01-02 00:00:00+00:00"),
       .highNanodollars = 115'000'000'000,
       .lowNanodollars = 108'000'000'000,
       .closeNanodollars = 112'000'000'000},
      {.utcDateStart = timestamp("2024-01-04 00:00:00+00:00"),
       .highNanodollars = 118'000'000'000,
       .lowNanodollars = 109'000'000'000,
       .closeNanodollars = 116'000'000'000},
  };
  EXPECT_EQ(summaries.value(), expected);
}

TEST(ReleaseSnapshotTest,
     historicalDataDaySummariesRemainStableAfterSourceMutation) {
  const ScopedSnapshotFixture fixture{"historical-data-day-source-mutation"};
  fixture.writeSymbol("SYN", threeHourlyRows);
  const auto built = bte::data::buildReleaseSnapshot(fixture.buildRequest());
  ASSERT_TRUE(built.ok()) << built.error().message;
  auto reader = bte::data::ReleaseSnapshotReader::open(
      fixture.store(), built.value().snapshotId);
  ASSERT_TRUE(reader.ok()) << reader.error().message;
  fixture.writeSymbol(
      "SYN", "SYN,2024-01-01 23:00:00+00:00,100,1000,99,999,1200,ohlcv-1h\n"
             "SYN,2024-01-02 00:00:00+00:00,999,999,999,999,2300,ohlcv-1h\n");

  const auto summaries = reader.value()->lastDataDaysBeforeUtcDate(
      "SYN", timestamp("2024-01-03 00:00:00+00:00"), 2);

  ASSERT_TRUE(summaries.ok()) << summaries.error().message;
  const auto expected = std::vector<bte::data::SnapshotDataDaySummary>{
      {.utcDateStart = timestamp("2024-01-01 00:00:00+00:00"),
       .highNanodollars = 102'000'000'000,
       .lowNanodollars = 99'500'000'000,
       .closeNanodollars = 101'250'000'000},
      {.utcDateStart = timestamp("2024-01-02 00:00:00+00:00"),
       .highNanodollars = 104'000'000'000,
       .lowNanodollars = 100'000'000'000,
       .closeNanodollars = 103'125'000'000},
  };
  EXPECT_EQ(summaries.value(), expected);
}

TEST(ReleaseSnapshotTest,
     lastDataDaysBeforeUtcDateReturnsAvailableHistoryAndEnforcesCountLimit) {
  const ScopedSnapshotFixture fixture{"historical-data-day-count-boundary"};
  fixture.writeSymbol(
      "SYN", "SYN,2024-01-05 20:00:00+00:00,99,101,98,100,100,ohlcv-1h\n");
  const auto built = bte::data::buildReleaseSnapshot(fixture.buildRequest());
  ASSERT_TRUE(built.ok()) << built.error().message;
  auto reader = bte::data::ReleaseSnapshotReader::open(
      fixture.store(), built.value().snapshotId);
  ASSERT_TRUE(reader.ok()) << reader.error().message;
  const auto boundary = timestamp("2024-01-05 00:00:00+00:00");

  const auto noPriorDate =
      reader.value()->lastDataDaysBeforeUtcDate("SYN", boundary, 1);
  ASSERT_TRUE(noPriorDate.ok()) << noPriorDate.error().message;
  EXPECT_TRUE(noPriorDate.value().empty());

  const auto maximumCount = reader.value()->lastDataDaysBeforeUtcDate(
      "SYN", boundary, bte::data::maximumSnapshotHistoryDataDays);
  ASSERT_TRUE(maximumCount.ok()) << maximumCount.error().message;
  EXPECT_TRUE(maximumCount.value().empty());

  const auto zeroCount =
      reader.value()->lastDataDaysBeforeUtcDate("SYN", boundary, 0);
  ASSERT_FALSE(zeroCount.ok());
  EXPECT_EQ(zeroCount.error().code, bte::core::ErrorCode::invalidArgument);

  const auto excessiveCount = reader.value()->lastDataDaysBeforeUtcDate(
      "SYN", boundary, bte::data::maximumSnapshotHistoryDataDays + 1);
  ASSERT_FALSE(excessiveCount.ok());
  EXPECT_EQ(excessiveCount.error().code, bte::core::ErrorCode::invalidArgument);

  const auto invalidBoundary = reader.value()->lastDataDaysBeforeUtcDate(
      "SYN", timestamp("2024-01-05 00:01:00+00:00"), 1);
  ASSERT_FALSE(invalidBoundary.ok());
  EXPECT_EQ(invalidBoundary.error().code,
            bte::core::ErrorCode::invalidArgument);
}

TEST(ReleaseSnapshotTest, lastDataDaysBeforeUtcDateHonorsCancellation) {
  const ScopedSnapshotFixture fixture{"historical-data-day-cancelled"};
  fixture.writeSymbol("SYN", threeHourlyRows);
  auto request = fixture.buildRequest();
  request.rowsPerSegment = 1;
  const auto built = bte::data::buildReleaseSnapshot(request);
  ASSERT_TRUE(built.ok()) << built.error().message;
  auto reader = bte::data::ReleaseSnapshotReader::open(
      fixture.store(), built.value().snapshotId);
  ASSERT_TRUE(reader.ok()) << reader.error().message;

  bte::core::CancellationSource alreadyCancelled;
  alreadyCancelled.requestCancellation();
  const auto cancelledBeforeLookup = reader.value()->lastDataDaysBeforeUtcDate(
      "SYN", timestamp("2024-01-03 00:00:00+00:00"), 1,
      alreadyCancelled.token());
  ASSERT_FALSE(cancelledBeforeLookup.ok());
  EXPECT_EQ(cancelledBeforeLookup.error().code,
            bte::core::ErrorCode::cancelled);

  const auto absentSymbol = reader.value()->lastDataDaysBeforeUtcDate(
      "ABSENT", timestamp("2024-01-03 00:00:00+00:00"), 1);
  ASSERT_TRUE(absentSymbol.ok()) << absentSymbol.error().message;
  EXPECT_TRUE(absentSymbol.value().empty());

  const auto limitedHistory = reader.value()->lastDataDaysBeforeUtcDate(
      "SYN", timestamp("2024-01-03 00:00:00+00:00"), 1);
  ASSERT_TRUE(limitedHistory.ok()) << limitedHistory.error().message;
  ASSERT_EQ(limitedHistory.value().size(), 1U);
  EXPECT_EQ(limitedHistory.value().front().utcDateStart,
            timestamp("2024-01-02 00:00:00+00:00"));

  bte::data::testing::failSnapshotAt(
      bte::data::testing::SnapshotFailurePoint::historyLookupCancellation);
  const auto result = reader.value()->lastDataDaysBeforeUtcDate(
      "SYN", timestamp("2024-01-03 00:00:00+00:00"), 1);
  bte::data::testing::clearSnapshotFailure();

  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().code, bte::core::ErrorCode::cancelled);
}

TEST(ReleaseSnapshotTest,
     lastDataDaysBeforeUtcDateHonorsCancellationDuringBarScan) {
  const ScopedSnapshotFixture fixture{"historical-data-day-bar-scan-cancelled"};
  fixture.writeSymbol("SYN", threeHourlyRows);
  auto request = fixture.buildRequest();
  request.rowsPerSegment = 10;
  const auto built = bte::data::buildReleaseSnapshot(request);
  ASSERT_TRUE(built.ok()) << built.error().message;
  auto reader = bte::data::ReleaseSnapshotReader::open(
      fixture.store(), built.value().snapshotId);
  ASSERT_TRUE(reader.ok()) << reader.error().message;

  bte::data::testing::failSnapshotAt(
      bte::data::testing::SnapshotFailurePoint::historyDataDayScanCancellation);
  const auto result = reader.value()->lastDataDaysBeforeUtcDate(
      "SYN", timestamp("2024-01-03 00:00:00+00:00"), 1);
  bte::data::testing::clearSnapshotFailure();

  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().code, bte::core::ErrorCode::cancelled);
}

TEST(ReleaseSnapshotTest, lastCloseBeforeUtcDateHonorsCancellation) {
  const ScopedSnapshotFixture fixture{"prior-data-day-cancelled"};
  fixture.writeSymbol("SYN", threeHourlyRows);
  const auto built = bte::data::buildReleaseSnapshot(fixture.buildRequest());
  ASSERT_TRUE(built.ok()) << built.error().message;
  auto reader = bte::data::ReleaseSnapshotReader::open(
      fixture.store(), built.value().snapshotId);
  ASSERT_TRUE(reader.ok()) << reader.error().message;
  bte::core::CancellationSource cancellation;
  cancellation.requestCancellation();

  const auto result = reader.value()->lastCloseBeforeUtcDate(
      "SYN", timestamp("2024-01-02 00:00:00+00:00"), cancellation.token());

  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().code, bte::core::ErrorCode::cancelled);
}

TEST(ReleaseSnapshotTest,
     lastCloseBeforeUtcDateHonorsCancellationDuringSegmentScan) {
  const ScopedSnapshotFixture fixture{"prior-data-day-scan-cancelled"};
  fixture.writeSymbol("SYN", threeHourlyRows);
  auto request = fixture.buildRequest();
  request.rowsPerSegment = 1;
  const auto built = bte::data::buildReleaseSnapshot(request);
  ASSERT_TRUE(built.ok()) << built.error().message;
  auto reader = bte::data::ReleaseSnapshotReader::open(
      fixture.store(), built.value().snapshotId);
  ASSERT_TRUE(reader.ok()) << reader.error().message;

  bte::data::testing::failSnapshotAt(
      bte::data::testing::SnapshotFailurePoint::historyLookupCancellation);
  const auto result = reader.value()->lastCloseBeforeUtcDate(
      "SYN", timestamp("2024-01-02 00:00:00+00:00"));
  bte::data::testing::clearSnapshotFailure();

  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().code, bte::core::ErrorCode::cancelled);
}

TEST(ReleaseSnapshotTest, lastCloseBeforeUtcDateRejectsNonMidnightBoundary) {
  const ScopedSnapshotFixture fixture{"prior-data-day-invalid-boundary"};
  fixture.writeSymbol("SYN", threeHourlyRows);
  const auto built = bte::data::buildReleaseSnapshot(fixture.buildRequest());
  ASSERT_TRUE(built.ok()) << built.error().message;
  auto reader = bte::data::ReleaseSnapshotReader::open(
      fixture.store(), built.value().snapshotId);
  ASSERT_TRUE(reader.ok()) << reader.error().message;

  const auto result = reader.value()->lastCloseBeforeUtcDate(
      "SYN", timestamp("2024-01-02 12:00:00+00:00"));

  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().code, bte::core::ErrorCode::invalidArgument);
}

TEST(ReleaseSnapshotTest, preRangeDataDayCloseIsStableAfterSourceMutation) {
  const ScopedSnapshotFixture fixture{"prior-data-day-source-mutation"};
  fixture.writeSymbol(
      "SYN", "SYN,2024-01-05 20:00:00+00:00,99,101,98,100,100,ohlcv-1h\n"
             "SYN,2024-01-05 22:00:00+00:00,100,111,99,110,100,ohlcv-1h\n"
             "SYN,2024-01-08 00:00:00+00:00,110,121,109,120,100,ohlcv-1h\n");
  const auto built = bte::data::buildReleaseSnapshot(fixture.buildRequest());
  ASSERT_TRUE(built.ok()) << built.error().message;
  auto reader = bte::data::ReleaseSnapshotReader::open(
      fixture.store(), built.value().snapshotId);
  ASSERT_TRUE(reader.ok()) << reader.error().message;
  fixture.writeSymbol(
      "SYN", "SYN,2024-01-05 20:00:00+00:00,99,101,98,999,100,ohlcv-1h\n"
             "SYN,2024-01-08 00:00:00+00:00,110,121,109,120,100,ohlcv-1h\n");

  const auto history = reader.value()->lastCloseBeforeUtcDate(
      "SYN", timestamp("2024-01-08 00:00:00+00:00"));

  ASSERT_TRUE(history.ok()) << history.error().message;
  const auto expected = std::optional{bte::data::SnapshotHistoryClose{
      .timestamp = timestamp("2024-01-05 22:00:00+00:00"),
      .closeNanodollars = 110'000'000'000,
  }};
  EXPECT_EQ(history.value(), expected);
}

TEST(ReleaseSnapshotTest, buildAcceptsCanonicalSevenColumnExtractedHourlyCsv) {
  ScopedSnapshotFixture fixture{"canonical-extracted-input"};
  fixture.writeExtractedSymbol("SYN", threeExtractedHourlyRows);
  const auto csvBytes = readFile(fixture.source() / "SYN.csv");
  writeSourceManifest(fixture.source(), "file=SYN.csv|ohlcv-1h|" +
                                            bte::core::sha256(csvBytes) + "\n");

  const auto built = bte::data::buildReleaseSnapshot(fixture.buildRequest());
  ASSERT_TRUE(built.ok()) << built.error().message;
  EXPECT_EQ(built.value().barCount, 3U);

  auto reader = bte::data::ReleaseSnapshotReader::open(
      fixture.store(), built.value().snapshotId);
  ASSERT_TRUE(reader.ok()) << reader.error().message;
  const auto selected = reader.value()->select({
      .symbols = {"SYN"},
      .range = {.start = timestamp("2024-01-01 23:00:00+00:00"),
                .end = timestamp("2024-01-02 02:00:00+00:00")},
      .timeframe = "ohlcv-1h",
  });
  ASSERT_TRUE(selected.ok()) << selected.error().message;
  ASSERT_EQ(selected.value().bars.size(), 3U);
  EXPECT_EQ(selected.value().bars[1].openNanodollars, 101'250'000'000);
  EXPECT_EQ(selected.value().bars[2].closeNanodollars, 103'125'000'000);
}

TEST(ReleaseSnapshotTest,
     buildResolvesDottedSymbolsToExtractorSanitizedCsvFilenames) {
  ScopedSnapshotFixture fixture{"dotted-symbol-extracted-input"};
  constexpr auto symbol = "BRK.B";
  constexpr auto rows =
      "BRK.B,2024-01-01 23:00:00+00:00,100,102,99,101,1200\n"
      "BRK.B,2024-01-02 00:00:00+00:00,101,103,100,102,2300\n"
      "BRK.B,2024-01-02 01:00:00+00:00,102,104,101,103,3400\n";
  const auto sourcePath = fixture.source() / "BRK_B.csv";
  writeFile(sourcePath,
            "symbol,ts,open,high,low,close,volume\n" + std::string{rows});
  const auto csvBytes = readFile(sourcePath);
  writeSourceManifest(fixture.source(), "file=BRK_B.csv|ohlcv-1h|" +
                                            bte::core::sha256(csvBytes) + "\n");

  const auto built = bte::data::buildReleaseSnapshot(
      fixture.buildRequest({std::string{symbol}}));

  ASSERT_TRUE(built.ok()) << built.error().message;
  EXPECT_EQ(built.value().barCount, 3U);
  auto reader = bte::data::ReleaseSnapshotReader::open(
      fixture.store(), built.value().snapshotId);
  ASSERT_TRUE(reader.ok()) << reader.error().message;
  const auto selected = reader.value()->select({
      .symbols = {symbol},
      .range = {.start = timestamp("2024-01-01 23:00:00+00:00"),
                .end = timestamp("2024-01-02 02:00:00+00:00")},
      .timeframe = "ohlcv-1h",
  });
  ASSERT_TRUE(selected.ok()) << selected.error().message;
  ASSERT_EQ(selected.value().bars.size(), 3U);
  EXPECT_EQ(selected.value().bars.front().closeNanodollars, 101'000'000'000);
}

TEST(ReleaseSnapshotTest,
     buildRejectsSymbolsThatMapToTheSameExtractedCsvFilename) {
  ScopedSnapshotFixture fixture{"colliding-extracted-symbol-filenames"};
  const auto sourcePath = fixture.source() / "BRK_B.csv";
  const auto csv = std::string{"symbol,ts,open,high,low,close,volume\n"} +
                   "BRK.B,2024-01-01 23:00:00+00:00,100,102,99,101,1200\n";
  writeFile(sourcePath, csv);
  writeSourceManifest(fixture.source(), "file=BRK_B.csv|ohlcv-1h|" +
                                            bte::core::sha256(csv) + "\n");

  const auto built =
      bte::data::buildReleaseSnapshot(fixture.buildRequest({"BRK.B", "BRK_B"}));

  ASSERT_FALSE(built.ok());
  EXPECT_EQ(built.error().code, bte::core::ErrorCode::invalidArgument);
  EXPECT_EQ(built.error().message,
            "Snapshot symbols map to the same extracted CSV filename");
}

TEST(ReleaseSnapshotTest, buildKeepsExplicitDottedSymbolCsvFilenameCompatible) {
  ScopedSnapshotFixture fixture{"explicit-dotted-symbol-filename"};
  constexpr auto rows =
      "BRK.B,2024-01-01 23:00:00+00:00,100,102,99,101,1200,ohlcv-1h\n"
      "BRK.B,2024-01-02 00:00:00+00:00,101,103,100,102,2300,ohlcv-1h\n"
      "BRK.B,2024-01-02 01:00:00+00:00,102,104,101,103,3400,ohlcv-1h\n";
  writeFile(fixture.source() / "BRK.B.csv",
            "symbol,ts,open,high,low,close,volume,schemaName\n" +
                std::string{rows});

  const auto built =
      bte::data::buildReleaseSnapshot(fixture.buildRequest({"BRK.B"}));

  ASSERT_TRUE(built.ok()) << built.error().message;
  EXPECT_EQ(built.value().barCount, 3U);
}

TEST(ReleaseSnapshotTest,
     buildUsesManifestDeclaredCanonicalDottedSymbolFilename) {
  ScopedSnapshotFixture fixture{"manifest-declared-dotted-symbol-filename"};
  constexpr auto csv = "symbol,ts,open,high,low,close,volume\n"
                       "BRK.B,2024-01-01 23:00:00+00:00,100,102,99,101,1200\n";
  writeFile(fixture.source() / "BRK.B.csv", csv);
  writeSourceManifest(fixture.source(), "file=BRK.B.csv|ohlcv-1h|" +
                                            bte::core::sha256(csv) + "\n");

  const auto built =
      bte::data::buildReleaseSnapshot(fixture.buildRequest({"BRK.B"}));

  ASSERT_TRUE(built.ok()) << built.error().message;
  EXPECT_EQ(built.value().barCount, 1U);
}

TEST(ReleaseSnapshotTest,
     buildUsesExtractorFilenameFallbackForExplicitSchemaWithoutManifest) {
  ScopedSnapshotFixture fixture{"sanitized-dotted-symbol-without-manifest"};
  constexpr auto csv =
      "symbol,ts,open,high,low,close,volume,schemaName\n"
      "BRK.B,2024-01-01 23:00:00+00:00,100,102,99,101,1200,ohlcv-1h\n";
  writeFile(fixture.source() / "BRK_B.csv", csv);

  const auto built =
      bte::data::buildReleaseSnapshot(fixture.buildRequest({"BRK.B"}));

  ASSERT_TRUE(built.ok()) << built.error().message;
  EXPECT_EQ(built.value().barCount, 1U);
}

TEST(ReleaseSnapshotTest,
     buildPrefersManifestDeclaredCanonicalDottedSymbolFilename) {
  ScopedSnapshotFixture fixture{"both-dotted-symbol-filenames-declared"};
  constexpr auto exactCsv =
      "symbol,ts,open,high,low,close,volume\n"
      "BRK.B,2024-01-01 23:00:00+00:00,100,102,99,101,1200\n";
  constexpr auto extractedCsv =
      "symbol,ts,open,high,low,close,volume\n"
      "BRK.B,2024-01-01 23:00:00+00:00,100,102,99,101,1200\n"
      "BRK.B,2024-01-02 00:00:00+00:00,101,103,100,102,2300\n";
  writeFile(fixture.source() / "BRK.B.csv", exactCsv);
  writeFile(fixture.source() / "BRK_B.csv", extractedCsv);
  writeSourceManifest(fixture.source(),
                      "file=BRK.B.csv|ohlcv-1h|" + bte::core::sha256(exactCsv) +
                          "\n" + "file=BRK_B.csv|ohlcv-1h|" +
                          bte::core::sha256(extractedCsv) + "\n");

  const auto built =
      bte::data::buildReleaseSnapshot(fixture.buildRequest({"BRK.B"}));

  ASSERT_TRUE(built.ok()) << built.error().message;
  EXPECT_EQ(built.value().barCount, 1U);
}

TEST(ReleaseSnapshotTest,
     buildReportsInspectionFailureForAnUnrepresentableExactFilename) {
  ScopedSnapshotFixture fixture{"unrepresentable-dotted-symbol-filename"};
  const auto symbol = std::string(260, 'A') + ".B";

  const auto built =
      bte::data::buildReleaseSnapshot(fixture.buildRequest({symbol}));

  ASSERT_FALSE(built.ok());
  EXPECT_EQ(built.error().code, bte::core::ErrorCode::schemaMismatch);
  EXPECT_EQ(built.error().message,
            "Unable to inspect snapshot source: " +
                (fixture.source() / (symbol + ".csv")).string());
}

TEST(ReleaseSnapshotTest,
     buildReportsInspectionFailureForAnUninspectableExtractedFilename) {
  ScopedSnapshotFixture fixture{"uninspectable-extracted-symbol-filename"};
  const auto extractedPath = fixture.source() / "BRK_B.csv";
  auto filesystemError = std::error_code{};
  std::filesystem::create_symlink(extractedPath.filename(), extractedPath,
                                  filesystemError);
  ASSERT_FALSE(filesystemError) << filesystemError.message();

  const auto built =
      bte::data::buildReleaseSnapshot(fixture.buildRequest({"BRK.B"}));

  ASSERT_FALSE(built.ok());
  EXPECT_EQ(built.error().code, bte::core::ErrorCode::schemaMismatch);
  EXPECT_EQ(built.error().message,
            "Unable to inspect snapshot source: " + extractedPath.string());
}

TEST(ReleaseSnapshotTest,
     buildKeepsExactDottedFilenameAsMissingSourceFallback) {
  ScopedSnapshotFixture fixture{"missing-dotted-symbol-source"};

  const auto built =
      bte::data::buildReleaseSnapshot(fixture.buildRequest({"BRK.B"}));

  ASSERT_FALSE(built.ok());
  EXPECT_EQ(built.error().code, bte::core::ErrorCode::notFound);
  EXPECT_EQ(built.error().message,
            "Snapshot source not found: " +
                (fixture.source() / "BRK.B.csv").string());
}

TEST(ReleaseSnapshotTest,
     buildRejectsSevenColumnExtractWithoutSourceTimeframeManifest) {
  ScopedSnapshotFixture fixture{"unmanifested-extracted-input"};
  fixture.writeExtractedSymbol("SYN", threeExtractedHourlyRows);

  const auto built = bte::data::buildReleaseSnapshot(fixture.buildRequest());

  ASSERT_FALSE(built.ok());
  EXPECT_EQ(built.error().code, bte::core::ErrorCode::schemaMismatch);
}

TEST(ReleaseSnapshotTest,
     buildRejectsSevenColumnExtractWhenManifestHashDoesNotMatch) {
  ScopedSnapshotFixture fixture{"mismatched-source-hash"};
  fixture.writeExtractedSymbol("SYN", threeExtractedHourlyRows);
  writeSourceManifest(fixture.source(),
                      "file=SYN.csv|ohlcv-1h|" + std::string(64, 'a') + "\n");

  const auto built = bte::data::buildReleaseSnapshot(fixture.buildRequest());

  ASSERT_FALSE(built.ok());
  EXPECT_EQ(built.error().code, bte::core::ErrorCode::schemaMismatch);
}

TEST(ReleaseSnapshotTest,
     buildRejectsSevenColumnExtractWhenManifestDeclaresDailyTimeframe) {
  ScopedSnapshotFixture fixture{"wrong-source-timeframe"};
  fixture.writeExtractedSymbol("SYN", threeExtractedHourlyRows);
  const auto csvBytes = readFile(fixture.source() / "SYN.csv");
  writeSourceManifest(fixture.source(), "file=SYN.csv|ohlcv-1d|" +
                                            bte::core::sha256(csvBytes) + "\n");

  const auto built = bte::data::buildReleaseSnapshot(fixture.buildRequest());

  ASSERT_FALSE(built.ok());
  EXPECT_EQ(built.error().code, bte::core::ErrorCode::schemaMismatch);
}

TEST(ReleaseSnapshotTest,
     buildKeepsExplicitSchemaColumnCompatibleWithoutSourceManifest) {
  ScopedSnapshotFixture fixture{"explicit-schema-source-compatibility"};
  writeFile(fixture.source() / "SYN.csv",
            "symbol,ts,open,high,low,close,volume,schemaName,legacyNote\n"
            "SYN,2024-01-01 23:00:00+00:00,100.125,102,99.5,101.25,1200,"
            "ohlcv-1h,legacy\n"
            "SYN,2024-01-02 00:00:00+00:00,101.25,103.5,100,102.75,2300,"
            "ohlcv-1h,legacy\n"
            "SYN,2024-01-02 01:00:00+00:00,102.75,104,101.5,103.125,3400,"
            "ohlcv-1h,legacy\n");

  const auto built = bte::data::buildReleaseSnapshot(fixture.buildRequest());

  ASSERT_TRUE(built.ok()) << built.error().message;
  EXPECT_EQ(built.value().barCount, 3U);
}

TEST(ReleaseSnapshotTest,
     buildAcceptsExtractorProvenanceColumnsWithHashBoundManifest) {
  ScopedSnapshotFixture fixture{"extractor-provenance-column-order"};
  writeFile(
      fixture.source() / "SYN.csv",
      "symbol,ts,open,high,low,close,volume,source,schemaName,ingestedAt\n"
      "SYN,2024-01-01 23:00:00+00:00,100.125,102,99.5,101.25,1200,"
      "databento,ohlcv-1h,2024-01-02 00:00:00+00:00\n"
      "SYN,2024-01-02 00:00:00+00:00,101.25,103.5,100,102.75,2300,"
      "databento,ohlcv-1h,2024-01-02 00:00:00+00:00\n"
      "SYN,2024-01-02 01:00:00+00:00,102.75,104,101.5,103.125,3400,"
      "databento,ohlcv-1h,2024-01-02 00:00:00+00:00\n");
  const auto csvBytes = readFile(fixture.source() / "SYN.csv");
  writeSourceManifest(fixture.source(), "file=SYN.csv|ohlcv-1h|" +
                                            bte::core::sha256(csvBytes) + "\n");

  const auto built = bte::data::buildReleaseSnapshot(fixture.buildRequest());

  ASSERT_TRUE(built.ok()) << built.error().message;
  EXPECT_EQ(built.value().barCount, 3U);
}

TEST(ReleaseSnapshotTest,
     buildRejectsManifestThatDoesNotDeclareEverySelectedCsv) {
  ScopedSnapshotFixture fixture{"incomplete-source-manifest"};
  fixture.writeSymbol("SYN", threeHourlyRows);
  writeSourceManifest(fixture.source(),
                      "file=OTHER.csv|ohlcv-1h|" + std::string(64, 'a') + "\n");

  const auto built = bte::data::buildReleaseSnapshot(fixture.buildRequest());

  ASSERT_FALSE(built.ok());
  EXPECT_EQ(built.error().code, bte::core::ErrorCode::schemaMismatch);
}

TEST(ReleaseSnapshotTest, buildRejectsDuplicateSourceManifestFilenames) {
  ScopedSnapshotFixture fixture{"duplicate-source-manifest-entry"};
  fixture.writeExtractedSymbol("SYN", threeExtractedHourlyRows);
  const auto csvBytes = readFile(fixture.source() / "SYN.csv");
  const auto entry =
      "file=SYN.csv|ohlcv-1h|" + bte::core::sha256(csvBytes) + "\n";
  writeSourceManifest(fixture.source(), entry + entry);

  const auto built = bte::data::buildReleaseSnapshot(fixture.buildRequest());

  ASSERT_FALSE(built.ok());
  EXPECT_EQ(built.error().code, bte::core::ErrorCode::schemaMismatch);
}

TEST(ReleaseSnapshotTest,
     buildRejectsMalformedSourceManifestHeadersAndEntries) {
  const auto validHash = std::string(64, 'a');
  const std::vector<std::pair<std::string, std::string>> invalidManifests{
      {"bad-header", "BAD-MAGIC\n"},
      {"bad-entry-prefix", "BTE-SOURCE-MANIFEST-V1\nunexpected\n"},
      {"bad-entry-field-count",
       "BTE-SOURCE-MANIFEST-V1\nfile=SYN.csv|ohlcv-1h\n"},
      {"bad-entry-filename",
       "BTE-SOURCE-MANIFEST-V1\nfile=../SYN.csv|ohlcv-1h|" + validHash + "\n"},
      {"bad-entry-extension",
       "BTE-SOURCE-MANIFEST-V1\nfile=.csv|ohlcv-1h|" + validHash + "\n"},
      {"bad-entry-timeframe",
       "BTE-SOURCE-MANIFEST-V1\nfile=SYN.csv|ohlcv-15m|" + validHash + "\n"},
      {"bad-entry-hash",
       "BTE-SOURCE-MANIFEST-V1\nfile=SYN.csv|ohlcv-1h|not-a-hash\n"},
  };

  for (const auto &[name, manifest] : invalidManifests) {
    ScopedSnapshotFixture fixture{"malformed-source-manifest-" + name};
    fixture.writeExtractedSymbol("SYN", threeExtractedHourlyRows);
    writeFile(fixture.source() / "SourceManifest.btesource", manifest);

    const auto built = bte::data::buildReleaseSnapshot(fixture.buildRequest());

    ASSERT_FALSE(built.ok()) << name;
    EXPECT_EQ(built.error().code, bte::core::ErrorCode::schemaMismatch) << name;
  }
}

TEST(ReleaseSnapshotTest,
     selectionIdentityIsStableForTheSameRangeAndChangesForANarrowerRange) {
  ScopedSnapshotFixture fixture{"selection-identity"};
  fixture.writeSymbol("SYN", threeHourlyRows);
  const auto built = bte::data::buildReleaseSnapshot(fixture.buildRequest());
  ASSERT_TRUE(built.ok()) << built.error().message;
  auto reader = bte::data::ReleaseSnapshotReader::open(
      fixture.store(), built.value().snapshotId);
  ASSERT_TRUE(reader.ok()) << reader.error().message;

  const bte::data::SnapshotSelectRequest fullRange{
      .symbols = {"SYN"},
      .range = {.start = timestamp("2024-01-01 23:00:00+00:00"),
                .end = timestamp("2024-01-02 02:00:00+00:00")},
      .timeframe = "ohlcv-1h",
  };
  const auto first = reader.value()->select(fullRange);
  const auto repeated = reader.value()->select(fullRange);
  ASSERT_TRUE(first.ok()) << first.error().message;
  ASSERT_TRUE(repeated.ok()) << repeated.error().message;
  EXPECT_EQ(first.value().identity, repeated.value().identity);

  auto narrowerRange = fullRange;
  narrowerRange.range.start = timestamp("2024-01-02 01:00:00+00:00");
  const auto narrowed = reader.value()->select(narrowerRange);
  ASSERT_TRUE(narrowed.ok()) << narrowed.error().message;
  ASSERT_EQ(narrowed.value().bars.size(), 1);
  EXPECT_NE(first.value().identity, narrowed.value().identity);
}

TEST(ReleaseSnapshotTest,
     exactReadReturnsOnlyPersistedRowsAndRejectsAnySpanMutation) {
  ScopedSnapshotFixture fixture{"exact-selection"};
  fixture.writeSymbol("SYN", threeHourlyRows);
  const auto built = bte::data::buildReleaseSnapshot(fixture.buildRequest());
  ASSERT_TRUE(built.ok()) << built.error().message;
  auto reader = bte::data::ReleaseSnapshotReader::open(
      fixture.store(), built.value().snapshotId);
  ASSERT_TRUE(reader.ok()) << reader.error().message;
  auto selected = reader.value()->select({
      .symbols = {"SYN"},
      .range = {.start = timestamp("2024-01-02 00:00:00+00:00"),
                .end = timestamp("2024-01-02 02:00:00+00:00")},
      .timeframe = "ohlcv-1h",
  });
  ASSERT_TRUE(selected.ok()) << selected.error().message;

  const auto exact = reader.value()->readExact(selected.value().identity);
  ASSERT_TRUE(exact.ok()) << exact.error().message;
  EXPECT_EQ(exact.value().bars, selected.value().bars);
  EXPECT_EQ(exact.value().identity, selected.value().identity);
  EXPECT_TRUE(reader.value()->validateExact(selected.value().identity).ok());

  const auto expectRejected = [&](bte::data::DataSelectionIdentity identity) {
    const auto rejected = reader.value()->readExact(identity);
    ASSERT_FALSE(rejected.ok());
    EXPECT_EQ(rejected.error().code,
              bte::core::ErrorCode::dataSnapshotUnavailable);
    const auto validation = reader.value()->validateExact(identity);
    ASSERT_FALSE(validation.ok());
    EXPECT_EQ(validation.error().code,
              bte::core::ErrorCode::dataSnapshotUnavailable);
  };
  auto mutate = selected.value().identity;
  mutate.snapshotId = std::string(64, 'f');
  expectRejected(mutate);
  mutate = selected.value().identity;
  mutate.calendarHash = std::string(64, 'f');
  expectRejected(mutate);
  mutate = selected.value().identity;
  mutate.splitManifestHash = std::string(64, 'f');
  expectRejected(mutate);
  mutate = selected.value().identity;
  mutate.timeframe = "ohlcv-1d";
  expectRejected(mutate);
  mutate = selected.value().identity;
  mutate.profile = "other";
  expectRejected(mutate);
  mutate = selected.value().identity;
  mutate.spans.front().segmentOrdinal = 9;
  expectRejected(mutate);
  mutate = selected.value().identity;
  mutate.spans.front().symbol = "OTHER";
  expectRejected(mutate);
  mutate = selected.value().identity;
  mutate.spans.front().segmentId = std::string(64, 'f');
  expectRejected(mutate);
  mutate = selected.value().identity;
  mutate.spans.front().segmentHash = std::string(64, 'f');
  expectRejected(mutate);
  mutate = selected.value().identity;
  mutate.spans.front().firstRow = 0;
  expectRejected(mutate);
  mutate = selected.value().identity;
  mutate.spans.front().rowCount = 2;
  expectRejected(mutate);
  mutate = selected.value().identity;
  mutate.spans.front().firstTimestamp = timestamp("2024-01-01 23:00:00+00:00");
  expectRejected(mutate);
  mutate = selected.value().identity;
  mutate.spans.front().lastTimestamp = timestamp("2024-01-01 23:00:00+00:00");
  expectRejected(mutate);
  mutate = selected.value().identity;
  std::ranges::reverse(mutate.spans);
  expectRejected(mutate);
}

TEST(ReleaseSnapshotTest,
     exactReadRejectsAnEmptyDefaultSpanAndHonorsPreRequestedCancellation) {
  ScopedSnapshotFixture fixture{"exact-empty-and-cancelled"};
  fixture.writeSymbol("SYN", threeHourlyRows);
  const auto built = bte::data::buildReleaseSnapshot(fixture.buildRequest());
  ASSERT_TRUE(built.ok()) << built.error().message;
  auto reader = bte::data::ReleaseSnapshotReader::open(
      fixture.store(), built.value().snapshotId);
  ASSERT_TRUE(reader.ok()) << reader.error().message;

  auto selected = reader.value()->select({
      .symbols = {"SYN"},
      .range = {.start = timestamp("2024-01-01 23:00:00+00:00"),
                .end = timestamp("2024-01-02 02:00:00+00:00")},
      .timeframe = "ohlcv-1h",
  });
  ASSERT_TRUE(selected.ok()) << selected.error().message;

  auto emptySpanIdentity = selected.value().identity;
  emptySpanIdentity.spans = {bte::data::DataSegmentSpan{}};
  const auto emptySpan = reader.value()->readExact(emptySpanIdentity);
  ASSERT_FALSE(emptySpan.ok());
  EXPECT_EQ(emptySpan.error().code,
            bte::core::ErrorCode::dataSnapshotUnavailable);
  const auto invalidEmptySpan =
      reader.value()->validateExact(emptySpanIdentity);
  ASSERT_FALSE(invalidEmptySpan.ok());
  EXPECT_EQ(invalidEmptySpan.error().code,
            bte::core::ErrorCode::dataSnapshotUnavailable);

  bte::core::CancellationSource cancellation;
  cancellation.requestCancellation();
  const auto cancelled = reader.value()->readExact(selected.value().identity,
                                                   cancellation.token());
  ASSERT_FALSE(cancelled.ok());
  EXPECT_EQ(cancelled.error().code, bte::core::ErrorCode::cancelled);
  const auto cancelledValidation = reader.value()->validateExact(
      selected.value().identity, cancellation.token());
  ASSERT_FALSE(cancelledValidation.ok());
  EXPECT_EQ(cancelledValidation.error().code, bte::core::ErrorCode::cancelled);
}

TEST(ReleaseSnapshotTest,
     exactReadResolvesLaterManifestSegmentInMultiSymbolSnapshot) {
  ScopedSnapshotFixture fixture{"exact-later-segment"};
  fixture.writeSymbol(
      "AAA", "AAA,2024-01-01 00:00:00+00:00,10,11,9,10.5,100,ohlcv-1h\n");
  fixture.writeSymbol(
      "ZZZ", "ZZZ,2024-01-01 01:00:00+00:00,20,21,19,20.5,200,ohlcv-1h\n");
  auto request = fixture.buildRequest({"AAA", "ZZZ"});
  request.rowsPerSegment = 1;
  const auto built = bte::data::buildReleaseSnapshot(request);
  ASSERT_TRUE(built.ok()) << built.error().message;
  auto reader = bte::data::ReleaseSnapshotReader::open(
      fixture.store(), built.value().snapshotId);
  ASSERT_TRUE(reader.ok()) << reader.error().message;

  auto selected = reader.value()->select(
      {.symbols = {"ZZZ"},
       .range = {.start = timestamp("2024-01-01 01:00:00+00:00"),
                 .end = timestamp("2024-01-01 02:00:00+00:00")},
       .timeframe = "ohlcv-1h"});
  ASSERT_TRUE(selected.ok()) << selected.error().message;
  ASSERT_EQ(selected.value().identity.spans.size(), 1U);
  EXPECT_EQ(selected.value().identity.spans.front().segmentOrdinal, 1U);

  auto exact = reader.value()->readExact(selected.value().identity);
  ASSERT_TRUE(exact.ok()) << exact.error().message;
  EXPECT_EQ(exact.value().bars, selected.value().bars);
  EXPECT_EQ(exact.value().identity, selected.value().identity);
}

TEST(ReleaseSnapshotTest,
     identicalInputs_produceIdenticalSnapshotAndSegmentIdentities) {
  ScopedSnapshotFixture first{"determinism-first"};
  ScopedSnapshotFixture second{"determinism-second"};
  first.writeSymbol("SYN", threeHourlyRows);
  second.writeSymbol("SYN", threeHourlyRows);

  const auto firstBuild = bte::data::buildReleaseSnapshot(first.buildRequest());
  const auto secondBuild =
      bte::data::buildReleaseSnapshot(second.buildRequest());

  ASSERT_TRUE(firstBuild.ok()) << firstBuild.error().message;
  ASSERT_TRUE(secondBuild.ok()) << secondBuild.error().message;
  EXPECT_EQ(firstBuild.value().snapshotId, secondBuild.value().snapshotId);
  EXPECT_EQ(firstBuild.value().segments, secondBuild.value().segments);
}

TEST(ReleaseSnapshotTest,
     cancellationAndInvalidRequestsFailWithoutPublishingSnapshot) {
  ScopedSnapshotFixture fixture{"cancelled"};
  fixture.writeSymbol("SYN", threeHourlyRows);
  bte::core::CancellationSource cancellation;
  cancellation.requestCancellation();

  const auto cancelled = bte::data::buildReleaseSnapshot(fixture.buildRequest(),
                                                         cancellation.token());
  ASSERT_FALSE(cancelled.ok());
  EXPECT_EQ(cancelled.error().code, bte::core::ErrorCode::cancelled);
  EXPECT_FALSE(std::filesystem::exists(fixture.store() / "Snapshots"));

  auto invalid = fixture.buildRequest({});
  invalid.rowsPerSegment = 0;
  const auto rejected = bte::data::buildReleaseSnapshot(invalid);
  ASSERT_FALSE(rejected.ok());
  EXPECT_EQ(rejected.error().code, bte::core::ErrorCode::invalidArgument);
  EXPECT_FALSE(std::filesystem::exists(fixture.store() / "Snapshots"));
}

TEST(ReleaseSnapshotTest,
     selectionRejectsUnsupportedTimeframeAndReversedRange) {
  ScopedSnapshotFixture fixture{"invalid-selection"};
  fixture.writeSymbol("SYN", threeHourlyRows);
  const auto built = bte::data::buildReleaseSnapshot(fixture.buildRequest());
  ASSERT_TRUE(built.ok()) << built.error().message;
  auto reader = bte::data::ReleaseSnapshotReader::open(
      fixture.store(), built.value().snapshotId);
  ASSERT_TRUE(reader.ok()) << reader.error().message;

  auto request = bte::data::SnapshotSelectRequest{
      .symbols = {"SYN"},
      .range = {.start = timestamp("2024-01-01 00:00:00+00:00"),
                .end = timestamp("2024-01-03 00:00:00+00:00")},
      .timeframe = "ohlcv-1d",
  };
  const auto unsupported = reader.value()->select(request);
  ASSERT_FALSE(unsupported.ok());
  EXPECT_EQ(unsupported.error().code, bte::core::ErrorCode::schemaMismatch);

  request.timeframe = "ohlcv-1h";
  request.range.start = request.range.end;
  const auto reversed = reader.value()->select(request);
  ASSERT_FALSE(reversed.ok());
  EXPECT_EQ(reversed.error().code, bte::core::ErrorCode::invalidArgument);
}

TEST(ReleaseSnapshotTest, corruptedSegmentFailsClosedWithoutCsvFallback) {
  ScopedSnapshotFixture fixture{"corrupt-segment"};
  fixture.writeSymbol("SYN", threeHourlyRows);
  const auto built = bte::data::buildReleaseSnapshot(fixture.buildRequest());
  ASSERT_TRUE(built.ok()) << built.error().message;
  ASSERT_FALSE(built.value().segments.empty());

  const auto segmentPath = fixture.store() / "Segments" /
                           (built.value().segments.front() + ".btedata");
  std::ofstream corrupt{segmentPath, std::ios::binary | std::ios::trunc};
  corrupt << "corrupt";
  corrupt.close();

  auto reader = bte::data::ReleaseSnapshotReader::open(
      fixture.store(), built.value().snapshotId);
  ASSERT_FALSE(reader.ok());
  EXPECT_EQ(reader.error().code, bte::core::ErrorCode::dataSnapshotUnavailable);
}

TEST(ReleaseSnapshotTest, corruptedManifestFailsAsDataSnapshotUnavailable) {
  ScopedSnapshotFixture fixture{"corrupt-manifest"};
  fixture.writeSymbol("SYN", threeHourlyRows);
  const auto built = bte::data::buildReleaseSnapshot(fixture.buildRequest());
  ASSERT_TRUE(built.ok()) << built.error().message;

  const auto manifestPath = fixture.store() / "Snapshots" /
                            built.value().snapshotId / "Manifest.btesnapshot";
  std::ofstream corrupt{manifestPath, std::ios::binary | std::ios::app};
  corrupt << "mutation";
  corrupt.close();

  const auto reader = bte::data::ReleaseSnapshotReader::open(
      fixture.store(), built.value().snapshotId);
  ASSERT_FALSE(reader.ok());
  EXPECT_EQ(reader.error().code, bte::core::ErrorCode::dataSnapshotUnavailable);
}

TEST(ReleaseSnapshotTest,
     missingSegmentOutOfOrderSourceAndSelectionCancellationFailClosed) {
  ScopedSnapshotFixture missing{"missing-segment"};
  missing.writeSymbol("SYN", threeHourlyRows);
  const auto built = bte::data::buildReleaseSnapshot(missing.buildRequest());
  ASSERT_TRUE(built.ok()) << built.error().message;
  ASSERT_GE(built.value().segments.size(), 2);
  std::filesystem::remove(missing.store() / "Segments" /
                          (built.value().segments[1] + ".btedata"));

  const auto unavailable = bte::data::ReleaseSnapshotReader::open(
      missing.store(), built.value().snapshotId);
  ASSERT_FALSE(unavailable.ok());
  EXPECT_EQ(unavailable.error().code,
            bte::core::ErrorCode::dataSnapshotUnavailable);

  ScopedSnapshotFixture outOfOrder{"out-of-order-source"};
  outOfOrder.writeSymbol(
      "SYN", "SYN,2024-01-02 00:00:00+00:00,101,103,100,102,100,ohlcv-1h\n"
             "SYN,2024-01-01 23:00:00+00:00,100,102,99,101,100,ohlcv-1h\n");
  const auto rejected =
      bte::data::buildReleaseSnapshot(outOfOrder.buildRequest());
  ASSERT_FALSE(rejected.ok());
  EXPECT_EQ(rejected.error().code, bte::core::ErrorCode::invalidArgument);

  ScopedSnapshotFixture cancelled{"selection-cancelled"};
  cancelled.writeSymbol("SYN", threeHourlyRows);
  const auto cancellableBuild =
      bte::data::buildReleaseSnapshot(cancelled.buildRequest());
  ASSERT_TRUE(cancellableBuild.ok()) << cancellableBuild.error().message;
  auto reader = bte::data::ReleaseSnapshotReader::open(
      cancelled.store(), cancellableBuild.value().snapshotId);
  ASSERT_TRUE(reader.ok()) << reader.error().message;
  bte::core::CancellationSource cancellation;
  cancellation.requestCancellation();
  const auto selection = reader.value()->select(
      {.symbols = {"SYN"},
       .range = {.start = timestamp("2024-01-01 00:00:00+00:00"),
                 .end = timestamp("2024-01-03 00:00:00+00:00")},
       .timeframe = "ohlcv-1h"},
      cancellation.token());
  ASSERT_FALSE(selection.ok());
  EXPECT_EQ(selection.error().code, bte::core::ErrorCode::cancelled);
}

TEST(ReleaseSnapshotTest,
     malformedSourcesRejectMissingIdentityNumericPrecisionAndOhlcvFaults) {
  const std::vector<std::string> rows{
      "SYN,2024-01-01 00:00:00+00:00,,2,1,1,1,ohlcv-1h\n",
      "SYN,2024-01-01 00:00:00+00:00,-1,2,1,1,1,ohlcv-1h\n",
      "SYN,2024-01-01 00:00:00+00:00,.1,2,1,1,1,ohlcv-1h\n",
      "SYN,2024-01-01 00:00:00+00:00,1.0000000001,2,1,1,1,ohlcv-1h\n",
      "SYN,2024-01-01 00:00:00+00:00,not-a-price,2,1,1,1,ohlcv-1h\n",
      "SYN,2024-01-01 00:00:00+00:00,1.bad,2,1,1,1,ohlcv-1h\n",
      "SYN,2024-01-01 00:00:00+00:00,9223372036854775807,"
      "9223372036854775807,1,1,1,ohlcv-1h\n",
      "SYN,invalid,1,2,1,1,1,ohlcv-1h\n",
      "SYN,2024-01-01 00:00:00+00:00,0,2,1,1,1,ohlcv-1h\n",
      "SYN,2024-01-01 00:00:00+00:00,2,1,1,1,1,ohlcv-1h\n",
      "OTHER,2024-01-01 00:00:00+00:00,1,2,1,1,1,ohlcv-1h\n",
      "SYN,2024-01-01 00:00:00+00:00,1,2,1,1,1,ohlcv-1d\n",
  };
  for (std::size_t index = 0; index < rows.size(); ++index) {
    ScopedSnapshotFixture fixture{"malformed-" + std::to_string(index)};
    fixture.writeSymbol("SYN", rows[index]);
    const auto built = bte::data::buildReleaseSnapshot(fixture.buildRequest());
    EXPECT_FALSE(built.ok()) << index;
  }

  ScopedSnapshotFixture missing{"missing-source"};
  const auto notFound = bte::data::buildReleaseSnapshot(missing.buildRequest());
  ASSERT_FALSE(notFound.ok());
  EXPECT_EQ(notFound.error().code, bte::core::ErrorCode::notFound);

  ScopedSnapshotFixture empty{"empty-source"};
  writeFile(empty.source() / "SYN.csv", {});
  EXPECT_FALSE(bte::data::buildReleaseSnapshot(empty.buildRequest()).ok());

  ScopedSnapshotFixture header{"bad-header"};
  writeFile(header.source() / "SYN.csv", "symbol,ts,open\n");
  EXPECT_FALSE(bte::data::buildReleaseSnapshot(header.buildRequest()).ok());

  ScopedSnapshotFixture blank{"blank-row"};
  blank.writeSymbol(
      "SYN", " \t\r\nSYN,2024-01-01 00:00:00+00:00,1,2,1,1,1,ohlcv-1h\n");
  EXPECT_TRUE(bte::data::buildReleaseSnapshot(blank.buildRequest()).ok());

  ScopedSnapshotFixture trimmed{"trimmed-row"};
  trimmed.writeSymbol(
      "SYN", "SYN,2024-01-01 00:00:00+00:00,100.0,102,99,101,1,ohlcv-1h   \n");
  EXPECT_TRUE(bte::data::buildReleaseSnapshot(trimmed.buildRequest()).ok());
}

TEST(ReleaseSnapshotTest, rejectsMalformedDecimalsWithoutPublishingSnapshot) {
  const std::vector<std::string> values{"100.-1", "100.+1", "100.1-1",
                                        "100.",   ".5",     "100..1"};
  for (std::size_t index = 0; index < values.size(); ++index) {
    ScopedSnapshotFixture fixture{"fraction-sign-" + std::to_string(index)};
    fixture.writeSymbol("SYN", "SYN,2024-01-01 00:00:00+00:00," +
                                   values[index] + ",102,99,101,1,ohlcv-1h\n");
    const auto built = bte::data::buildReleaseSnapshot(fixture.buildRequest());
    ASSERT_FALSE(built.ok()) << values[index];
    EXPECT_EQ(built.error().code, bte::core::ErrorCode::invalidArgument);
    EXPECT_FALSE(std::filesystem::exists(fixture.store() / "Snapshots"));
  }
}

TEST(ReleaseSnapshotTest,
     rejectsUnsupportedTimestampWithoutPublishingSnapshot) {
  ScopedSnapshotFixture fixture{"invalid-timestamp-no-publication"};
  fixture.writeSymbol("SYN",
                      "SYN,2024-01-01T00:00:00+08:00,1,2,1,1,1,ohlcv-1h\n");
  const auto built = bte::data::buildReleaseSnapshot(fixture.buildRequest());
  ASSERT_FALSE(built.ok());
  EXPECT_EQ(built.error().code, bte::core::ErrorCode::invalidArgument);
  EXPECT_FALSE(std::filesystem::exists(fixture.store() / "Snapshots"));
}

TEST(ReleaseSnapshotTest,
     requestAndSelectionBoundariesRejectDuplicatesAndMissingArtifacts) {
  ScopedSnapshotFixture fixture{"request-boundaries"};
  fixture.writeSymbol("SYN", threeHourlyRows);

  auto duplicate = fixture.buildRequest({"SYN", "SYN"});
  const auto duplicateBuild = bte::data::buildReleaseSnapshot(duplicate);
  ASSERT_FALSE(duplicateBuild.ok());
  EXPECT_EQ(duplicateBuild.error().code, bte::core::ErrorCode::invalidArgument);

  auto invalidHash = fixture.buildRequest();
  invalidHash.calendarHash = "A" + std::string(63, 'a');
  EXPECT_FALSE(bte::data::buildReleaseSnapshot(invalidHash).ok());

  bte::core::CancellationSource cancelled;
  cancelled.requestCancellation();
  EXPECT_EQ(bte::data::ReleaseSnapshotReader::open(
                fixture.store(), std::string(64, 'a'), cancelled.token())
                .error()
                .code,
            bte::core::ErrorCode::cancelled);
  EXPECT_FALSE(
      bte::data::ReleaseSnapshotReader::open({}, std::string(64, 'a')).ok());
  EXPECT_FALSE(
      bte::data::ReleaseSnapshotReader::open(fixture.store(), "bad-id").ok());
  const auto missing = bte::data::ReleaseSnapshotReader::open(
      fixture.store(), std::string(64, 'a'));
  ASSERT_FALSE(missing.ok());
  EXPECT_EQ(missing.error().code,
            bte::core::ErrorCode::dataSnapshotUnavailable);

  const auto built = bte::data::buildReleaseSnapshot(fixture.buildRequest());
  ASSERT_TRUE(built.ok());
  auto reader = bte::data::ReleaseSnapshotReader::open(
      fixture.store(), built.value().snapshotId);
  ASSERT_TRUE(reader.ok());
  const auto range =
      bte::core::DateRange{.start = timestamp("2024-01-01 00:00:00+00:00"),
                           .end = timestamp("2024-01-03 00:00:00+00:00")};
  EXPECT_FALSE(reader.value()
                   ->select({.symbols = {"SYN", "SYN"},
                             .range = range,
                             .timeframe = "ohlcv-1h"})
                   .ok());
  const auto absent = reader.value()->select(
      {.symbols = {"ABSENT"}, .range = range, .timeframe = "ohlcv-1h"});
  ASSERT_TRUE(absent.ok());
  EXPECT_TRUE(absent.value().bars.empty());
  const auto outside = reader.value()->select(
      {.symbols = {"SYN"},
       .range = {.start = timestamp("2025-01-01 00:00:00+00:00"),
                 .end = timestamp("2025-01-02 00:00:00+00:00")},
       .timeframe = "ohlcv-1h"});
  ASSERT_TRUE(outside.ok());
  EXPECT_TRUE(outside.value().bars.empty());
}

TEST(ReleaseSnapshotTest,
     malformedManifestRecordsAndSegmentEncodingsFailClosed) {
  ScopedSnapshotFixture fixture{"encoded-corruption"};
  fixture.writeSymbol("SYN", threeHourlyRows);
  const auto built = bte::data::buildReleaseSnapshot(fixture.buildRequest());
  ASSERT_TRUE(built.ok());
  const auto manifest =
      readFile(fixture.store() / "Snapshots" / built.value().snapshotId /
               "Manifest.btesnapshot");

  const std::vector<std::string> invalidManifests{
      "BAD-MAGIC\n",
      "BTE-SNAPSHOT-MANIFEST-V1\nprofile=vendorExtendedHours\n",
      "BTE-SNAPSHOT-MANIFEST-V1\nprofile=wrong\ntimeframe=ohlcv-1h\n"
      "calendarHash=" +
          std::string(64, 'a') + "\nsplitManifestHash=" + std::string(64, 'b') +
          "\n",
      manifest + "unexpected\n",
      manifest.substr(0, manifest.find("segment=")) + "segment=0|SYN\n",
      manifest.substr(0, manifest.find("segment=")) +
          "segment=not-an-integer|SYN|" + std::string(64, 'a') + "|1|0|1\n",
  };
  for (std::size_t index = 0; index < invalidManifests.size(); ++index) {
    const auto snapshotId =
        publishManifest(fixture.store(), invalidManifests[index]);
    const auto opened =
        bte::data::ReleaseSnapshotReader::open(fixture.store(), snapshotId);
    EXPECT_FALSE(opened.ok()) << index;
    EXPECT_EQ(opened.error().code,
              bte::core::ErrorCode::dataSnapshotUnavailable)
        << index;
  }

  const auto segmentPath = fixture.store() / "Segments" /
                           (built.value().segments.front() + ".btedata");
  const auto originalBytes = readFile(segmentPath);
  std::vector<std::string> corruptions;
  corruptions.push_back("short");
  auto invalidVersion = originalBytes;
  invalidVersion[15] = static_cast<char>(2);
  corruptions.push_back(invalidVersion);
  auto truncatedText = originalBytes;
  truncatedText[23] = static_cast<char>(0x7f);
  corruptions.push_back(truncatedText);
  corruptions.push_back(originalBytes.substr(0, originalBytes.size() - 1));
  corruptions.push_back(originalBytes + "trailing");
  auto duplicateTimestamp = originalBytes;
  constexpr std::size_t headerBytes = 8 + 8 + 8 + 3 + 8;
  duplicateTimestamp.replace(headerBytes + 48, 8,
                             duplicateTimestamp.substr(headerBytes, 8));
  corruptions.push_back(duplicateTimestamp);
  auto negativeOpen = originalBytes;
  negativeOpen[headerBytes + 8] = static_cast<char>(0xff);
  corruptions.push_back(negativeOpen);
  auto negativeVolume = originalBytes;
  negativeVolume[headerBytes + 40] = static_cast<char>(0xff);
  corruptions.push_back(negativeVolume);

  for (std::size_t index = 0; index < corruptions.size(); ++index) {
    const auto snapshotId =
        publishSegmentMutation(fixture, built.value(), corruptions[index]);
    const auto opened =
        bte::data::ReleaseSnapshotReader::open(fixture.store(), snapshotId);
    EXPECT_FALSE(opened.ok()) << index;
    EXPECT_EQ(opened.error().code,
              bte::core::ErrorCode::dataSnapshotUnavailable)
        << index;
  }

  auto oversizedCount = originalBytes;
  constexpr std::size_t countOffset = 8 + 8 + 8 + 3;
  oversizedCount.replace(countOffset, 8, "\x7f\xff\xff\xff\xff\xff\xff\xff", 8);
  const auto oversizedId = bte::core::sha256(oversizedCount);
  writeFile(fixture.store() / "Segments" / (oversizedId + ".btedata"),
            oversizedCount);
  auto oversizedManifest = manifest;
  replaceFirst(oversizedManifest, built.value().segments.front(), oversizedId);
  replaceFirst(oversizedManifest, "|2|", "|9223372036854775807|");
  const auto oversizedSnapshot =
      publishManifest(fixture.store(), oversizedManifest);
  const auto oversizedOpen = bte::data::ReleaseSnapshotReader::open(
      fixture.store(), oversizedSnapshot);
  ASSERT_FALSE(oversizedOpen.ok());
  EXPECT_EQ(oversizedOpen.error().code,
            bte::core::ErrorCode::dataSnapshotUnavailable);

  const auto firstStart = manifest.find("segment=");
  const auto firstEnd = manifest.find('\n', firstStart);
  const auto secondStart = firstEnd + 1;
  const auto secondEnd = manifest.find('\n', secondStart);
  auto firstLine = manifest.substr(firstStart, firstEnd - firstStart + 1);
  auto secondLine = manifest.substr(secondStart, secondEnd - secondStart + 1);
  replaceFirst(firstLine, "segment=0", "segment=1");
  replaceFirst(secondLine, "segment=1", "segment=0");
  const auto reordered = manifest.substr(0, firstStart) + secondLine +
                         firstLine + manifest.substr(secondEnd + 1);
  const auto reorderedId = publishManifest(fixture.store(), reordered);
  const auto reorderedOpen =
      bte::data::ReleaseSnapshotReader::open(fixture.store(), reorderedId);
  ASSERT_FALSE(reorderedOpen.ok());
  EXPECT_EQ(reorderedOpen.error().code,
            bte::core::ErrorCode::dataSnapshotUnavailable);
}

TEST(ReleaseSnapshotTest,
     injectedBuildAndReadFaultsPreserveStructuredFailureBoundaries) {
  using bte::data::testing::SnapshotFailurePoint;
  for (const auto point : {SnapshotFailurePoint::buildCancellation,
                           SnapshotFailurePoint::artifactWrite,
                           SnapshotFailurePoint::manifestWrite}) {
    ScopedSnapshotFixture fixture{"snapshot-write-fault-" +
                                  std::to_string(static_cast<int>(point))};
    fixture.writeSymbol("SYN", threeHourlyRows);
    bte::data::testing::failSnapshotAt(point);
    const auto built = bte::data::buildReleaseSnapshot(fixture.buildRequest());
    EXPECT_FALSE(built.ok()) << static_cast<int>(point);
    bte::data::testing::clearSnapshotFailure();
  }

  ScopedSnapshotFixture fixture{"snapshot-read-faults"};
  fixture.writeSymbol("SYN", threeHourlyRows);
  const auto built = bte::data::buildReleaseSnapshot(fixture.buildRequest());
  ASSERT_TRUE(built.ok()) << built.error().message;
  for (const auto point : {SnapshotFailurePoint::artifactInspection,
                           SnapshotFailurePoint::artifactRead,
                           SnapshotFailurePoint::openCancellation}) {
    bte::data::testing::failSnapshotAt(point);
    const auto opened = bte::data::ReleaseSnapshotReader::open(
        fixture.store(), built.value().snapshotId);
    EXPECT_FALSE(opened.ok()) << static_cast<int>(point);
    bte::data::testing::clearSnapshotFailure();
  }
}

TEST(ReleaseSnapshotTest,
     rebuildingRejectsCorruptExistingSegmentAndPublishingIntoFilePath) {
  ScopedSnapshotFixture existing{"existing-corrupt"};
  existing.writeSymbol("SYN", threeHourlyRows);
  const auto built = bte::data::buildReleaseSnapshot(existing.buildRequest());
  ASSERT_TRUE(built.ok());
  writeFile(existing.store() / "Segments" /
                (built.value().segments.front() + ".btedata"),
            "corrupt");
  const auto rebuilt = bte::data::buildReleaseSnapshot(existing.buildRequest());
  ASSERT_FALSE(rebuilt.ok());
  EXPECT_EQ(rebuilt.error().code,
            bte::core::ErrorCode::dataSnapshotUnavailable);

  ScopedSnapshotFixture blocked{"blocked-store"};
  blocked.writeSymbol("SYN", threeHourlyRows);
  writeFile(blocked.store(), "not-a-directory");
  const auto denied = bte::data::buildReleaseSnapshot(blocked.buildRequest());
  ASSERT_FALSE(denied.ok());
  EXPECT_EQ(denied.error().code, bte::core::ErrorCode::permissionDenied);
}

TEST(SegmentRetentionTest,
     acquireAndReleaseRetainsSharedSegmentsUntilFinalReferencePurges) {
  ScopedSnapshotFixture fixture{"retention"};
  fixture.writeSymbol("SYN", threeHourlyRows);
  const auto built = bte::data::buildReleaseSnapshot(fixture.buildRequest());
  ASSERT_TRUE(built.ok()) << built.error().message;
  ASSERT_EQ(built.value().segments.size(), 2);

  auto retention = bte::data::SegmentRetentionStore::open(fixture.store());
  ASSERT_TRUE(retention.ok()) << retention.error().message;
  auto firstAcquire =
      retention.value()->acquire("result-a", built.value().segments);
  ASSERT_TRUE(firstAcquire.ok()) << firstAcquire.error().message;
  EXPECT_EQ(firstAcquire.value(), 2);
  auto secondAcquire =
      retention.value()->acquire("result-b", {built.value().segments[0]});
  ASSERT_TRUE(secondAcquire.ok()) << secondAcquire.error().message;
  EXPECT_EQ(secondAcquire.value(), 1);
  ASSERT_TRUE(bte::data::removeReleaseSnapshot(fixture.store(),
                                               built.value().snapshotId)
                  .ok());

  const auto firstRelease = retention.value()->release("result-a");
  ASSERT_TRUE(firstRelease.ok()) << firstRelease.error().message;
  ASSERT_EQ(firstRelease.value().size(), 1);
  EXPECT_EQ(firstRelease.value()[0], built.value().segments[1]);
  EXPECT_TRUE(std::filesystem::exists(
      fixture.store() / "Segments" / (built.value().segments[0] + ".btedata")));
  EXPECT_FALSE(std::filesystem::exists(
      fixture.store() / "Segments" / (built.value().segments[1] + ".btedata")));

  const auto finalRelease = retention.value()->release("result-b");
  ASSERT_TRUE(finalRelease.ok()) << finalRelease.error().message;
  EXPECT_EQ(finalRelease.value(),
            std::vector<std::string>{built.value().segments[0]});
  EXPECT_FALSE(std::filesystem::exists(
      fixture.store() / "Segments" / (built.value().segments[0] + ".btedata")));
}

TEST(SegmentRetentionTest,
     referencesRejectInvalidIdentityAndReportDatabaseOpenFailure) {
  ScopedSnapshotFixture fixture{"retention-reference-errors"};
  auto retention = bte::data::SegmentRetentionStore::open(fixture.store());
  ASSERT_TRUE(retention.ok()) << retention.error().message;

  const auto invalid = retention.value()->references("Invalid");
  ASSERT_FALSE(invalid.ok());
  EXPECT_EQ(invalid.error().code, bte::core::ErrorCode::invalidArgument);

  bte::data::testing::failRetentionAfter(
      bte::data::testing::RetentionFailurePoint::databaseOpen);
  const auto unavailable = retention.value()->references("valid-result-id");
  bte::data::testing::clearRetentionFailure();
  ASSERT_FALSE(unavailable.ok());
  EXPECT_EQ(unavailable.error().code, bte::core::ErrorCode::internal);
}

TEST(SegmentRetentionTest,
     recoverySucceedsBeforeTheFirstSnapshotCreatesAnActiveSegmentDirectory) {
  bte::test_support::ScopedTemporaryDirectory temporary{
      "retention-empty-recovery"};
  auto retention = bte::data::SegmentRetentionStore::open(temporary.path());
  ASSERT_TRUE(retention.ok()) << retention.error().message;

  const auto recovered = retention.value()->recoverUnpublishedSnapshotPins();

  ASSERT_TRUE(recovered.ok()) << recovered.error().message;
  EXPECT_FALSE(std::filesystem::exists(temporary.path() / "Segments"));
}

TEST(SegmentRetentionTest,
     recoverySkipsInvalidPinsAndPreservesPinsForPublishedSnapshots) {
  ScopedSnapshotFixture fixture{"retention-invalid-and-published-pins"};
  fixture.writeSymbol("SYN", threeHourlyRows);
  const auto built = bte::data::buildReleaseSnapshot(fixture.buildRequest());
  ASSERT_TRUE(built.ok()) << built.error().message;
  auto retention = bte::data::SegmentRetentionStore::open(fixture.store());
  ASSERT_TRUE(retention.ok()) << retention.error().message;
  ASSERT_TRUE(retention.value()
                  ->acquire("snapshot-invalid", built.value().segments)
                  .ok());
  ASSERT_TRUE(retention.value()
                  ->acquire("snapshot-" + built.value().snapshotId,
                            built.value().segments)
                  .ok());
  executeSql(fixture.store() / "References.sqlite3",
             "INSERT INTO segment_references(result_id, segment_id) VALUES('" +
                 std::string{"Snapshot-"} + built.value().snapshotId + "','" +
                 built.value().segments.front() + "')");

  const auto recovered = retention.value()->recoverUnpublishedSnapshotPins();

  ASSERT_TRUE(recovered.ok()) << recovered.error().message;
  const auto invalidReferences =
      retention.value()->references("snapshot-invalid");
  ASSERT_TRUE(invalidReferences.ok()) << invalidReferences.error().message;
  EXPECT_EQ(invalidReferences.value(), built.value().segments);
  const auto publishedReferences =
      retention.value()->references("snapshot-" + built.value().snapshotId);
  ASSERT_TRUE(publishedReferences.ok()) << publishedReferences.error().message;
  EXPECT_EQ(publishedReferences.value(), built.value().segments);
  sqlite3 *rawDatabase = nullptr;
  ASSERT_EQ(
      sqlite3_open((fixture.store() / "References.sqlite3").string().c_str(),
                   &rawDatabase),
      SQLITE_OK);
  const auto database = std::unique_ptr<sqlite3, decltype(&sqlite3_close)>{
      rawDatabase, &sqlite3_close};
  sqlite3_stmt *rawStatement = nullptr;
  ASSERT_EQ(sqlite3_prepare_v2(
                database.get(),
                "SELECT COUNT(*) FROM segment_references WHERE result_id = "
                "'Snapshot-' || ?1",
                -1, &rawStatement, nullptr),
            SQLITE_OK);
  const auto statement =
      std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>{
          rawStatement, &sqlite3_finalize};
  ASSERT_EQ(sqlite3_bind_text(rawStatement, 1, built.value().snapshotId.c_str(),
                              -1, SQLITE_TRANSIENT),
            SQLITE_OK);
  ASSERT_EQ(sqlite3_step(rawStatement), SQLITE_ROW);
  EXPECT_EQ(sqlite3_column_int64(rawStatement, 0), 1);
  for (const auto &segmentId : built.value().segments) {
    EXPECT_TRUE(std::filesystem::exists(fixture.store() / "Segments" /
                                        (segmentId + ".btedata")));
  }
}

TEST(SegmentRetentionTest, recoveryPropagatesPinDatabaseAndQueryFailures) {
  using bte::data::testing::RetentionFailurePoint;
  for (const auto point : {RetentionFailurePoint::databaseOpen,
                           RetentionFailurePoint::queryPreparation,
                           RetentionFailurePoint::queryBinding}) {
    SCOPED_TRACE(static_cast<int>(point));
    bte::test_support::ScopedTemporaryDirectory temporary{
        "retention-pin-recovery-error"};
    auto retention = bte::data::SegmentRetentionStore::open(temporary.path());
    ASSERT_TRUE(retention.ok()) << retention.error().message;
    bte::data::testing::failRetentionAfter(point);

    const auto recovered = retention.value()->recoverUnpublishedSnapshotPins();
    bte::data::testing::clearRetentionFailure();

    ASSERT_FALSE(recovered.ok());
    EXPECT_EQ(recovered.error().code, bte::core::ErrorCode::internal);
  }
}

TEST(SegmentRetentionTest,
     recoveryReportsInvalidSegmentDirectoryAndSnapshotCatalog) {
  {
    bte::test_support::ScopedTemporaryDirectory temporary{
        "retention-segment-directory-not-a-directory"};
    auto retention = bte::data::SegmentRetentionStore::open(temporary.path());
    ASSERT_TRUE(retention.ok()) << retention.error().message;
    writeFile(temporary.path() / "Segments", "not a directory");

    const auto recovered = retention.value()->recoverUnpublishedSnapshotPins();

    ASSERT_FALSE(recovered.ok());
    EXPECT_EQ(recovered.error().code, bte::core::ErrorCode::permissionDenied);
  }
  {
    bte::test_support::ScopedTemporaryDirectory temporary{
        "retention-malformed-published-snapshot"};
    auto retention = bte::data::SegmentRetentionStore::open(temporary.path());
    ASSERT_TRUE(retention.ok()) << retention.error().message;
    std::filesystem::create_directories(temporary.path() / "Segments");
    const auto orphanBytes = std::string{"orphan segment before catalog error"};
    const auto orphanId = bte::core::sha256(orphanBytes);
    const auto active = temporary.path() / "Segments" / (orphanId + ".btedata");
    const auto trashed =
        temporary.path() / "Trash" / "Segments" / (orphanId + ".btedata");
    writeFile(active, orphanBytes);
    const auto snapshotId = std::string(64, 'c');
    const auto snapshotDirectory = temporary.path() / "Snapshots" / snapshotId;
    std::filesystem::create_directories(snapshotDirectory);
    writeFile(snapshotDirectory / "Manifest.btesnapshot",
              "invalid snapshot manifest");

    const auto recovered = retention.value()->recoverUnpublishedSnapshotPins();

    ASSERT_FALSE(recovered.ok());
    EXPECT_EQ(recovered.error().code,
              bte::core::ErrorCode::dataSnapshotUnavailable);
    EXPECT_TRUE(std::filesystem::exists(active));
    EXPECT_FALSE(std::filesystem::exists(trashed));
  }
}

TEST(SegmentRetentionTest,
     recoveryPreservesActiveSegmentWhenTrashDuplicateIsCorrupt) {
  bte::test_support::ScopedTemporaryDirectory temporary{
      "retention-recovery-corrupt-trash-duplicate"};
  auto retention = bte::data::SegmentRetentionStore::open(temporary.path());
  ASSERT_TRUE(retention.ok()) << retention.error().message;
  const auto segmentBytes = std::string{"valid active segment bytes"};
  const auto segmentId = bte::core::sha256(segmentBytes);
  const auto active = temporary.path() / "Segments" / (segmentId + ".btedata");
  const auto trashed =
      temporary.path() / "Trash" / "Segments" / (segmentId + ".btedata");
  std::filesystem::create_directories(active.parent_path());
  std::filesystem::create_directories(trashed.parent_path());
  writeFile(active, segmentBytes);
  writeFile(trashed, "corrupted trash segment bytes");

  const auto recovered = retention.value()->recoverUnpublishedSnapshotPins();

  ASSERT_FALSE(recovered.ok());
  EXPECT_EQ(recovered.error().code,
            bte::core::ErrorCode::dataSnapshotUnavailable);
  EXPECT_TRUE(std::filesystem::exists(active));
  EXPECT_TRUE(std::filesystem::exists(trashed));

  writeFile(trashed, segmentBytes);
  const auto retried = retention.value()->recoverUnpublishedSnapshotPins();

  ASSERT_TRUE(retried.ok()) << retried.error().message;
  EXPECT_FALSE(std::filesystem::exists(active));
  EXPECT_TRUE(std::filesystem::exists(trashed));
}

TEST(SegmentRetentionTest,
     recoveryPreservesActiveSegmentWhenTrashDuplicateIsNotRegularFile) {
  bte::test_support::ScopedTemporaryDirectory temporary{
      "retention-recovery-non-regular-trash-duplicate"};
  auto retention = bte::data::SegmentRetentionStore::open(temporary.path());
  ASSERT_TRUE(retention.ok()) << retention.error().message;
  const auto segmentBytes = std::string{"valid active segment bytes"};
  const auto segmentId = bte::core::sha256(segmentBytes);
  const auto active = temporary.path() / "Segments" / (segmentId + ".btedata");
  const auto trashed =
      temporary.path() / "Trash" / "Segments" / (segmentId + ".btedata");
  std::filesystem::create_directories(active.parent_path());
  std::filesystem::create_directories(trashed);
  writeFile(active, segmentBytes);

  const auto recovered = retention.value()->recoverUnpublishedSnapshotPins();

  ASSERT_FALSE(recovered.ok());
  EXPECT_EQ(recovered.error().code,
            bte::core::ErrorCode::dataSnapshotUnavailable);
  EXPECT_TRUE(std::filesystem::exists(active));
  EXPECT_TRUE(std::filesystem::is_directory(trashed));
}

TEST(SegmentRetentionTest, recoveryRestoresAnOrphanSegmentWhenCommitFails) {
  using bte::data::testing::RetentionFailurePoint;
  bte::test_support::ScopedTemporaryDirectory temporary{
      "retention-recovery-commit-failure"};
  auto retention = bte::data::SegmentRetentionStore::open(temporary.path());
  ASSERT_TRUE(retention.ok()) << retention.error().message;
  const auto segmentBytes = std::string{"abandoned segment bytes"};
  const auto segmentId = bte::core::sha256(segmentBytes);
  const auto active = temporary.path() / "Segments" / (segmentId + ".btedata");
  const auto trashed =
      temporary.path() / "Trash" / "Segments" / (segmentId + ".btedata");
  std::filesystem::create_directories(active.parent_path());
  writeFile(active, segmentBytes);
  bte::data::testing::failRetentionAfter(RetentionFailurePoint::sqlExecution,
                                         1);

  const auto recovered = retention.value()->recoverUnpublishedSnapshotPins();
  bte::data::testing::clearRetentionFailure();

  ASSERT_FALSE(recovered.ok());
  EXPECT_EQ(recovered.error().code, bte::core::ErrorCode::internal);
  EXPECT_TRUE(std::filesystem::exists(active));
  EXPECT_FALSE(std::filesystem::exists(trashed));
}

TEST(SegmentRetentionTest,
     recoveryReportsTransactionOpenQueryAndArtifactErrors) {
  using bte::data::testing::RetentionFailurePoint;
  for (const auto &[point, skips] :
       {std::pair{RetentionFailurePoint::databaseOpen, 1U},
        std::pair{RetentionFailurePoint::sqlExecution, 0U},
        std::pair{RetentionFailurePoint::queryPreparation, 1U}}) {
    SCOPED_TRACE(static_cast<int>(point));
    bte::test_support::ScopedTemporaryDirectory temporary{
        "retention-recovery-transaction-error"};
    auto retention = bte::data::SegmentRetentionStore::open(temporary.path());
    ASSERT_TRUE(retention.ok()) << retention.error().message;
    std::filesystem::create_directories(temporary.path() / "Segments");
    bte::data::testing::failRetentionAfter(point, skips);

    const auto recovered = retention.value()->recoverUnpublishedSnapshotPins();
    bte::data::testing::clearRetentionFailure();

    ASSERT_FALSE(recovered.ok());
    EXPECT_EQ(recovered.error().code, bte::core::ErrorCode::internal);
  }
}

TEST(SegmentRetentionTest,
     recoveryRejectsTrashLookupErrorsAndIgnoresNonRegularSegments) {
  {
    bte::test_support::ScopedTemporaryDirectory temporary{
        "retention-recovery-non-regular-segment"};
    auto retention = bte::data::SegmentRetentionStore::open(temporary.path());
    ASSERT_TRUE(retention.ok()) << retention.error().message;
    const auto segmentId = std::string(64, 'd');
    const auto segmentDirectory = temporary.path() / "Segments";
    const auto nonRegular = segmentDirectory / (segmentId + ".btedata");
    std::filesystem::create_directories(nonRegular);

    const auto recovered = retention.value()->recoverUnpublishedSnapshotPins();

    ASSERT_TRUE(recovered.ok()) << recovered.error().message;
    EXPECT_TRUE(std::filesystem::is_directory(nonRegular));
  }
  {
    bte::test_support::ScopedTemporaryDirectory temporary{
        "retention-recovery-trash-lookup-error"};
    auto retention = bte::data::SegmentRetentionStore::open(temporary.path());
    ASSERT_TRUE(retention.ok()) << retention.error().message;
    const auto segmentBytes = std::string{"segment with blocked trash lookup"};
    const auto segmentId = bte::core::sha256(segmentBytes);
    const auto active =
        temporary.path() / "Segments" / (segmentId + ".btedata");
    const auto trashed =
        temporary.path() / "Trash" / "Segments" / (segmentId + ".btedata");
    std::filesystem::create_directories(active.parent_path());
    writeFile(active, segmentBytes);
    std::filesystem::create_symlink(trashed, trashed);

    const auto recovered = retention.value()->recoverUnpublishedSnapshotPins();

    ASSERT_FALSE(recovered.ok());
    EXPECT_EQ(recovered.error().code, bte::core::ErrorCode::permissionDenied);
    EXPECT_TRUE(std::filesystem::exists(active));
    EXPECT_TRUE(
        std::filesystem::is_symlink(std::filesystem::symlink_status(trashed)));
  }
}

TEST(SegmentRetentionTest,
     recoveryReturnsAnErrorWhenAbandoningAnUnpublishedPinFails) {
  using bte::data::testing::RetentionFailurePoint;
  ScopedSnapshotFixture fixture{"retention-abandoned-pin-release-failure"};
  fixture.writeSymbol("SYN", threeHourlyRows);
  const auto built = bte::data::buildReleaseSnapshot(fixture.buildRequest());
  ASSERT_TRUE(built.ok()) << built.error().message;
  auto retention = bte::data::SegmentRetentionStore::open(fixture.store());
  ASSERT_TRUE(retention.ok()) << retention.error().message;
  const auto abandonedSnapshotId = std::string(64, 'f');
  ASSERT_TRUE(
      retention.value()
          ->acquire("snapshot-" + abandonedSnapshotId, built.value().segments)
          .ok());
  bte::data::testing::failRetentionAfter(RetentionFailurePoint::databaseOpen,
                                         1);

  const auto recovered = retention.value()->recoverUnpublishedSnapshotPins();
  bte::data::testing::clearRetentionFailure();

  ASSERT_FALSE(recovered.ok());
  EXPECT_EQ(recovered.error().code, bte::core::ErrorCode::internal);
  const auto references =
      retention.value()->references("snapshot-" + abandonedSnapshotId);
  ASSERT_TRUE(references.ok()) << references.error().message;
  EXPECT_EQ(references.value(), built.value().segments);
}

TEST(SegmentRetentionTest,
     recoveryRemovesStagedFilesAndLinksButPreservesStagingDirectories) {
  ScopedSnapshotFixture fixture{"retention-staged-artifact-recovery"};
  fixture.writeSymbol("SYN", threeHourlyRows);
  const auto built = bte::data::buildReleaseSnapshot(fixture.buildRequest());
  ASSERT_TRUE(built.ok()) << built.error().message;
  auto retention = bte::data::SegmentRetentionStore::open(fixture.store());
  ASSERT_TRUE(retention.ok()) << retention.error().message;

  const auto segmentDirectory = fixture.store() / "Segments";
  const auto stagedFile = segmentDirectory / "abandoned.btedata.staging";
  const auto stagedDirectory = segmentDirectory / "directory.btedata.staging";
  const auto stagedTarget = segmentDirectory / "staged-target";
  const auto stagedLink = segmentDirectory / "linked.btedata.staging";
  writeFile(stagedFile, "incomplete segment bytes");
  std::filesystem::create_directories(stagedDirectory);
  writeFile(stagedTarget, "target remains outside the staging namespace");
  std::filesystem::create_symlink(stagedTarget, stagedLink);

  const auto recovered = retention.value()->recoverUnpublishedSnapshotPins();

  ASSERT_TRUE(recovered.ok()) << recovered.error().message;
  EXPECT_FALSE(std::filesystem::exists(stagedFile));
  EXPECT_TRUE(std::filesystem::is_directory(stagedDirectory));
  EXPECT_FALSE(std::filesystem::exists(stagedLink));
  EXPECT_TRUE(std::filesystem::exists(stagedTarget));
  EXPECT_TRUE(bte::data::ReleaseSnapshotReader::open(fixture.store(),
                                                     built.value().snapshotId)
                  .ok());
}

TEST(SegmentRetentionTest,
     publishedSnapshotManifestProtectsSegmentsWhenItsPinIsReleased) {
  ScopedSnapshotFixture fixture{"retention-published-catalog-protection"};
  fixture.writeSymbol("SYN", threeHourlyRows);
  const auto built = bte::data::buildReleaseSnapshot(fixture.buildRequest());
  ASSERT_TRUE(built.ok()) << built.error().message;
  auto retention = bte::data::SegmentRetentionStore::open(fixture.store());
  ASSERT_TRUE(retention.ok()) << retention.error().message;

  const auto released =
      retention.value()->release("snapshot-" + built.value().snapshotId);

  ASSERT_TRUE(released.ok()) << released.error().message;
  EXPECT_TRUE(released.value().empty());
  for (const auto &segmentId : built.value().segments) {
    EXPECT_TRUE(std::filesystem::exists(fixture.store() / "Segments" /
                                        (segmentId + ".btedata")));
  }
  EXPECT_TRUE(bte::data::ReleaseSnapshotReader::open(fixture.store(),
                                                     built.value().snapshotId)
                  .ok());
}

TEST(SegmentRetentionTest,
     releaseReportsBlockedSegmentPurgeAndRetainsTheReferenceForRetry) {
  ScopedSnapshotFixture fixture{"retention-blocked-purge"};
  fixture.writeSymbol("SYN", threeHourlyRows);
  auto request = fixture.buildRequest();
  request.rowsPerSegment = 3;
  const auto built = bte::data::buildReleaseSnapshot(request);
  ASSERT_TRUE(built.ok()) << built.error().message;
  ASSERT_EQ(built.value().segments.size(), 1);
  const auto segmentId = built.value().segments.front();
  const auto segmentPath =
      fixture.store() / "Segments" / (segmentId + ".btedata");
  const auto originalSegment = readFile(segmentPath);

  auto retention = bte::data::SegmentRetentionStore::open(fixture.store());
  ASSERT_TRUE(retention.ok()) << retention.error().message;
  ASSERT_TRUE(retention.value()->acquire("result-a", {segmentId}).ok());
  ASSERT_TRUE(bte::data::removeReleaseSnapshot(fixture.store(),
                                               built.value().snapshotId)
                  .ok());

  ASSERT_TRUE(std::filesystem::remove(segmentPath));
  std::filesystem::create_directories(segmentPath);
  writeFile(segmentPath / "blocker", "prevents removal");

  const auto blocked = retention.value()->release("result-a");
  ASSERT_FALSE(blocked.ok());
  EXPECT_EQ(blocked.error().code, bte::core::ErrorCode::permissionDenied);
  EXPECT_TRUE(std::filesystem::exists(segmentPath / "blocker"));

  std::filesystem::remove_all(segmentPath);
  writeFile(segmentPath, originalSegment);
  const auto retried = retention.value()->release("result-a");
  ASSERT_TRUE(retried.ok()) << retried.error().message;
  EXPECT_EQ(retried.value(), std::vector<std::string>{segmentId});
  EXPECT_FALSE(std::filesystem::exists(segmentPath));
}

TEST(SegmentRetentionTest,
     concurrentReleasesSerializeAndPurgeOnlyAfterTheLastReference) {
  ScopedSnapshotFixture fixture{"retention-race"};
  fixture.writeSymbol(
      "SYN", "SYN,2024-01-01 23:00:00+00:00,100,102,99,101,1200,ohlcv-1h\n");
  auto request = fixture.buildRequest();
  request.rowsPerSegment = 1;
  const auto built = bte::data::buildReleaseSnapshot(request);
  ASSERT_TRUE(built.ok()) << built.error().message;
  ASSERT_EQ(built.value().segments.size(), 1);

  auto firstRetention = bte::data::SegmentRetentionStore::open(fixture.store());
  auto secondRetention =
      bte::data::SegmentRetentionStore::open(fixture.store());
  ASSERT_TRUE(firstRetention.ok()) << firstRetention.error().message;
  ASSERT_TRUE(secondRetention.ok()) << secondRetention.error().message;
  ASSERT_TRUE(
      firstRetention.value()->acquire("result-a", built.value().segments).ok());
  ASSERT_TRUE(
      firstRetention.value()->acquire("result-b", built.value().segments).ok());
  ASSERT_TRUE(bte::data::removeReleaseSnapshot(fixture.store(),
                                               built.value().snapshotId)
                  .ok());

  auto releaseFirst = std::async(std::launch::async, [&] {
    return firstRetention.value()->release("result-a");
  });
  auto releaseSecond = std::async(std::launch::async, [&] {
    return secondRetention.value()->release("result-b");
  });
  const auto first = releaseFirst.get();
  const auto second = releaseSecond.get();

  ASSERT_TRUE(first.ok()) << first.error().message;
  ASSERT_TRUE(second.ok()) << second.error().message;
  EXPECT_EQ(first.value().size() + second.value().size(), 1);
  EXPECT_FALSE(std::filesystem::exists(
      fixture.store() / "Segments" / (built.value().segments[0] + ".btedata")));
}

TEST(SegmentRetentionTest,
     validationRejectsInvalidIdentitiesMissingSegmentsAndCorruptContent) {
  EXPECT_FALSE(bte::data::SegmentRetentionStore::open({}).ok());

  ScopedSnapshotFixture blocked{"retention-blocked"};
  writeFile(blocked.store(), "not-a-directory");
  EXPECT_FALSE(bte::data::SegmentRetentionStore::open(blocked.store()).ok());

  ScopedSnapshotFixture fixture{"retention-validation"};
  fixture.writeSymbol("SYN", threeHourlyRows);
  const auto built = bte::data::buildReleaseSnapshot(fixture.buildRequest());
  ASSERT_TRUE(built.ok());
  auto retention = bte::data::SegmentRetentionStore::open(fixture.store());
  ASSERT_TRUE(retention.ok());

  EXPECT_FALSE(retention.value()->acquire("", built.value().segments).ok());
  EXPECT_FALSE(retention.value()->acquire("result", {}).ok());
  EXPECT_FALSE(retention.value()
                   ->acquire("result", {built.value().segments.front(),
                                        built.value().segments.front()})
                   .ok());
  EXPECT_FALSE(retention.value()->acquire("result", {"bad"}).ok());
  EXPECT_FALSE(
      retention.value()->acquire("result", {std::string(64, 'a')}).ok());
  writeFile(fixture.store() / "Segments" /
                (built.value().segments.front() + ".btedata"),
            "corrupt");
  EXPECT_FALSE(
      retention.value()->acquire("result", built.value().segments).ok());
  EXPECT_FALSE(retention.value()->release("INVALID").ok());
  const auto absent = retention.value()->release("absent-result");
  ASSERT_TRUE(absent.ok());
  EXPECT_TRUE(absent.value().empty());
}

TEST(SegmentRetentionTest,
     databaseOpenAndSchemaFaultsReturnErrorsWithoutPublishingReferences) {
  ScopedSnapshotFixture blockedDatabase{"retention-open-fault"};
  std::filesystem::create_directories(blockedDatabase.store() /
                                      "References.sqlite3");
  EXPECT_FALSE(
      bte::data::SegmentRetentionStore::open(blockedDatabase.store()).ok());

  ScopedSnapshotFixture fixture{"retention-schema-fault"};
  fixture.writeSymbol("SYN", threeHourlyRows);
  const auto built = bte::data::buildReleaseSnapshot(fixture.buildRequest());
  ASSERT_TRUE(built.ok());
  auto retention = bte::data::SegmentRetentionStore::open(fixture.store());
  ASSERT_TRUE(retention.ok());
  ASSERT_TRUE(
      retention.value()->acquire("result-a", built.value().segments).ok());
  executeSql(fixture.store() / "References.sqlite3",
             "DROP TABLE segment_references");

  const auto acquire =
      retention.value()->acquire("result-b", built.value().segments);
  ASSERT_FALSE(acquire.ok());
  EXPECT_EQ(acquire.error().code, bte::core::ErrorCode::internal);
  const auto release = retention.value()->release("result-a");
  ASSERT_FALSE(release.ok());
  EXPECT_EQ(release.error().code, bte::core::ErrorCode::internal);
}

TEST(SegmentRetentionTest,
     injectedStorageFaultsReachOpenAcquireReleaseAndCommitBoundaries) {
  using bte::data::testing::RetentionFailurePoint;
  bte::test_support::ScopedTemporaryDirectory openFaultDirectory{
      "bte-retention-injected-open-faults"};
  const auto openFaultRoot = openFaultDirectory.path();
  for (std::uint32_t occurrence = 0; occurrence < 4; ++occurrence) {
    bte::data::testing::failRetentionAfter(RetentionFailurePoint::sqlExecution,
                                           occurrence);
    EXPECT_FALSE(bte::data::SegmentRetentionStore::open(
                     openFaultRoot / std::to_string(occurrence))
                     .ok());
    bte::data::testing::clearRetentionFailure();
  }
  bte::data::testing::failRetentionAfter(RetentionFailurePoint::databaseOpen);
  EXPECT_FALSE(
      bte::data::SegmentRetentionStore::open(openFaultRoot / "database").ok());
  bte::data::testing::clearRetentionFailure();
  ScopedSnapshotFixture fixture{"retention-injected-faults"};
  fixture.writeSymbol("SYN", threeHourlyRows);
  const auto built = bte::data::buildReleaseSnapshot(fixture.buildRequest());
  ASSERT_TRUE(built.ok()) << built.error().message;
  auto retention = bte::data::SegmentRetentionStore::open(fixture.store());
  ASSERT_TRUE(retention.ok()) << retention.error().message;

  for (const auto point : {
           RetentionFailurePoint::databaseOpen,
           RetentionFailurePoint::sqlExecution,
           RetentionFailurePoint::boundPreparation,
           RetentionFailurePoint::boundBinding,
           RetentionFailurePoint::boundExecution,
       }) {
    bte::data::testing::failRetentionAfter(point);
    const auto acquired = retention.value()->acquire(
        "acquire-fault-" + std::to_string(static_cast<int>(point)),
        built.value().segments);
    EXPECT_FALSE(acquired.ok()) << static_cast<int>(point);
    bte::data::testing::clearRetentionFailure();
  }
  bte::data::testing::failRetentionAfter(RetentionFailurePoint::sqlExecution,
                                         1);
  EXPECT_FALSE(retention.value()
                   ->acquire("acquire-commit", built.value().segments)
                   .ok());
  bte::data::testing::clearRetentionFailure();

  const std::vector<RetentionFailurePoint> releaseFaults{
      RetentionFailurePoint::databaseOpen,
      RetentionFailurePoint::sqlExecution,
      RetentionFailurePoint::queryPreparation,
      RetentionFailurePoint::queryBinding,
      RetentionFailurePoint::queryExecution,
      RetentionFailurePoint::queryNullIdentity,
      RetentionFailurePoint::boundPreparation,
      RetentionFailurePoint::boundBinding,
      RetentionFailurePoint::boundExecution,
      RetentionFailurePoint::countPreparation,
      RetentionFailurePoint::countExecution,
  };
  for (const auto point : releaseFaults) {
    const auto resultId =
        "release-fault-" + std::to_string(static_cast<int>(point));
    ASSERT_TRUE(
        retention.value()->acquire(resultId, built.value().segments).ok());
    bte::data::testing::failRetentionAfter(point);
    const auto released = retention.value()->release(resultId);
    EXPECT_FALSE(released.ok()) << static_cast<int>(point);
    bte::data::testing::clearRetentionFailure();
    auto references = retention.value()->references(resultId);
    ASSERT_TRUE(references.ok()) << references.error().message;
    auto expectedReferences = built.value().segments;
    std::ranges::sort(expectedReferences);
    EXPECT_EQ(references.value(), expectedReferences)
        << static_cast<int>(point);
    const auto retriedRelease = retention.value()->release(resultId);
    ASSERT_TRUE(retriedRelease.ok()) << retriedRelease.error().message;
    const auto releasedReferences = retention.value()->references(resultId);
    ASSERT_TRUE(releasedReferences.ok()) << releasedReferences.error().message;
    EXPECT_TRUE(releasedReferences.value().empty()) << static_cast<int>(point);
  }
  ScopedSnapshotFixture commitFixture{"retention-commit-rollback"};
  commitFixture.writeSymbol("SYN", threeHourlyRows);
  const auto commitBuilt =
      bte::data::buildReleaseSnapshot(commitFixture.buildRequest());
  ASSERT_TRUE(commitBuilt.ok()) << commitBuilt.error().message;
  auto commitRetention =
      bte::data::SegmentRetentionStore::open(commitFixture.store());
  ASSERT_TRUE(commitRetention.ok()) << commitRetention.error().message;
  const auto commitId = std::string{"release-commit"};
  ASSERT_TRUE(commitRetention.value()
                  ->acquire(commitId, commitBuilt.value().segments)
                  .ok());
  ASSERT_TRUE(bte::data::removeReleaseSnapshot(commitFixture.store(),
                                               commitBuilt.value().snapshotId)
                  .ok());
  bte::data::testing::failRetentionAfter(RetentionFailurePoint::sqlExecution,
                                         1);
  const auto failedCommit = commitRetention.value()->release(commitId);
  EXPECT_FALSE(failedCommit.ok());
  bte::data::testing::clearRetentionFailure();
  for (const auto &segmentId : commitBuilt.value().segments) {
    EXPECT_TRUE(std::filesystem::exists(commitFixture.store() / "Segments" /
                                        (segmentId + ".btedata")));
  }
  const auto retriedRelease = commitRetention.value()->release(commitId);
  ASSERT_TRUE(retriedRelease.ok()) << retriedRelease.error().message;
  EXPECT_EQ(retriedRelease.value().size(), commitBuilt.value().segments.size());
}
