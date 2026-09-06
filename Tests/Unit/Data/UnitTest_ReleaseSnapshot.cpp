#include "Bte/Data/ReleaseSnapshot.h"
#include "Bte/Data/SegmentRetention.h"

#include "ReleaseSnapshotTestHooks.h"
#include "SegmentRetentionTestHooks.h"

#include "Bte/Core/Cancellation.h"
#include "Bte/Core/Digest.h"
#include "Bte/Core/Time.h"

#include <gtest/gtest.h>
#include <sqlite3.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

class ScopedSnapshotFixture final {
public:
  explicit ScopedSnapshotFixture(std::string name)
      : root_(std::filesystem::temp_directory_path() /
              ("bte-release-snapshot-" + std::move(name))),
        source_(root_ / "Source"), store_(root_ / "Store") {
    std::filesystem::remove_all(root_);
    std::filesystem::create_directories(source_);
  }

  ~ScopedSnapshotFixture() { std::filesystem::remove_all(root_); }

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

  [[nodiscard]] bte::data::SnapshotBuildRequest
  buildRequest(std::vector<std::string> symbols = {"SYN"}) const {
    return bte::data::SnapshotBuildRequest{
        .sourceDirectory = source_,
        .storeDirectory = store_,
        .symbols = std::move(symbols),
        .rowsPerSegment = 2,
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
  std::filesystem::path root_;
  std::filesystem::path source_;
  std::filesystem::path store_;
};

constexpr auto threeHourlyRows =
    "SYN,2024-01-01 23:00:00+00:00,100.125,102,99.5,101.25,1200,ohlcv-1h\n"
    "SYN,2024-01-02 00:00:00+00:00,101.25,103.5,100,102.75,2300,ohlcv-1h\n"
    "SYN,2024-01-02 01:00:00+00:00,102.75,104,101.5,103.125,3400,ohlcv-1h\n";

bte::core::Timestamp timestamp(const std::string &text) {
  return bte::core::time::parseIso8601(text).value();
}

std::string readFile(const std::filesystem::path &path) {
  std::ifstream input{path, std::ios::binary};
  return {std::istreambuf_iterator<char>{input},
          std::istreambuf_iterator<char>{}};
}

void writeFile(const std::filesystem::path &path, const std::string &content) {
  std::ofstream output{path, std::ios::binary | std::ios::trunc};
  output.write(content.data(), static_cast<std::streamsize>(content.size()));
  ASSERT_TRUE(output) << path;
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
      "SYN", "SYN,2024-01-01 00:00:00+00:00,100.,102,99,101,1,ohlcv-1h   \n");
  EXPECT_TRUE(bte::data::buildReleaseSnapshot(trimmed.buildRequest()).ok());
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

  auto retention = bte::data::SegmentRetentionStore::open(fixture.store());
  ASSERT_TRUE(retention.ok()) << retention.error().message;
  ASSERT_TRUE(
      retention.value()->acquire("result-a", built.value().segments).ok());
  ASSERT_TRUE(
      retention.value()->acquire("result-b", built.value().segments).ok());

  auto releaseFirst = std::async(std::launch::async, [&] {
    return retention.value()->release("result-a");
  });
  auto releaseSecond = std::async(std::launch::async, [&] {
    return retention.value()->release("result-b");
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
  const auto openFaultRoot = std::filesystem::temp_directory_path() /
                             "bte-retention-injected-open-faults";
  std::filesystem::remove_all(openFaultRoot);
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
  std::filesystem::remove_all(openFaultRoot);

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
  }
  const auto commitId = std::string{"release-commit"};
  ASSERT_TRUE(
      retention.value()->acquire(commitId, built.value().segments).ok());
  bte::data::testing::failRetentionAfter(RetentionFailurePoint::sqlExecution,
                                         1);
  EXPECT_FALSE(retention.value()->release(commitId).ok());
  bte::data::testing::clearRetentionFailure();
}
