#include "Bte/Data/ReleaseSnapshot.h"
#include "Bte/Data/SegmentRetention.h"

#include "Bte/Core/Cancellation.h"
#include "Bte/Core/Time.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
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
