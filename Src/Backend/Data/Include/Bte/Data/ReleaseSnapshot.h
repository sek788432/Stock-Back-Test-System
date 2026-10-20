#pragma once

#include "Bte/Core/Bar.h"
#include "Bte/Core/Cancellation.h"
#include "Bte/Core/Result.h"
#include "Bte/Core/Time.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace bte::data {

struct SnapshotBar {
  core::Timestamp timestamp;
  std::int64_t openNanodollars = 0;
  std::int64_t highNanodollars = 0;
  std::int64_t lowNanodollars = 0;
  std::int64_t closeNanodollars = 0;
  std::int64_t volumeMicroshares = 0;

  bool operator==(const SnapshotBar &) const = default;
};

struct SnapshotHistoryClose {
  core::Timestamp timestamp;
  std::int64_t closeNanodollars = 0;

  bool operator==(const SnapshotHistoryClose &) const = default;
};

struct SnapshotDataDaySummary {
  core::Timestamp utcDateStart;
  std::int64_t highNanodollars = 0;
  std::int64_t lowNanodollars = 0;
  std::int64_t closeNanodollars = 0;

  bool operator==(const SnapshotDataDaySummary &) const = default;
};

inline constexpr std::size_t maximumSnapshotHistoryDataDays = 10'000;

// Data-only aggregate initializers do not produce reliably attributable gcov
// branches across supported compilers. GCOVR_EXCL_START
struct DataSegmentSpan {
  std::size_t segmentOrdinal = 0;
  std::string symbol;
  std::string segmentId;
  std::string segmentHash;
  std::size_t firstRow = 0;
  std::size_t rowCount = 0;
  core::Timestamp firstTimestamp;
  core::Timestamp lastTimestamp;

  bool operator==(const DataSegmentSpan &) const = default;
};
// GCOVR_EXCL_STOP

struct DataSelectionIdentity {
  std::string snapshotId;
  std::string calendarHash;
  std::string splitManifestHash;
  std::string timeframe;
  std::string profile;
  std::vector<DataSegmentSpan> spans;

  bool operator==(const DataSelectionIdentity &) const = default;
};

struct SnapshotSelection {
  std::vector<SnapshotBar> bars;
  DataSelectionIdentity identity;
  std::optional<SnapshotHistoryClose> preRangeDataDayClose;
  std::vector<SnapshotDataDaySummary> preRangeDataDays;
};

struct SnapshotBuildRequest {
  std::filesystem::path sourceDirectory;
  std::filesystem::path storeDirectory;
  std::vector<std::string> symbols;
  std::size_t rowsPerSegment = 512;
  std::string calendarHash;
  std::string splitManifestHash;
};

struct SnapshotBuildResult {
  std::string snapshotId;
  std::vector<std::string> segments;
  std::size_t segmentCount = 0;
  std::size_t barCount = 0;
};

struct SnapshotSelectRequest {
  std::vector<std::string> symbols;
  core::DateRange range;
  std::string timeframe;
};

[[nodiscard]] core::Result<SnapshotBuildResult>
buildReleaseSnapshot(const SnapshotBuildRequest &request,
                     const core::CancellationToken &cancellation = {});

class ReleaseSnapshotReader final {
private:
  struct ConstructionKey final {};
  struct Impl;

public:
  explicit ReleaseSnapshotReader(ConstructionKey, std::unique_ptr<Impl> impl);
  ~ReleaseSnapshotReader();
  ReleaseSnapshotReader(const ReleaseSnapshotReader &) = delete;
  ReleaseSnapshotReader &operator=(const ReleaseSnapshotReader &) = delete;
  ReleaseSnapshotReader(ReleaseSnapshotReader &&) = delete;
  ReleaseSnapshotReader &operator=(ReleaseSnapshotReader &&) = delete;

  [[nodiscard]] static core::Result<std::unique_ptr<ReleaseSnapshotReader>>
  open(const std::filesystem::path &storeDirectory,
       const std::string &snapshotId,
       const core::CancellationToken &cancellation = {});

  [[nodiscard]] core::Result<SnapshotSelection>
  select(const SnapshotSelectRequest &request,
         const core::CancellationToken &cancellation = {}) const;

  /// Returns the latest source close before a UTC date boundary. The boundary
  /// must be midnight UTC; no earlier row is represented by an empty value.
  [[nodiscard]] core::Result<std::optional<SnapshotHistoryClose>>
  lastCloseBeforeUtcDate(
      std::string_view symbol, core::Timestamp utcDateStart,
      const core::CancellationToken &cancellation = {}) const;

  /// Returns up to the latest `count` UTC dates containing source rows strictly
  /// before the UTC date boundary, in chronological order. High, low, and
  /// close include every source row for each date, including extended hours.
  /// `count` must be between one and maximumSnapshotHistoryDataDays.
  [[nodiscard]] core::Result<std::vector<SnapshotDataDaySummary>>
  lastDataDaysBeforeUtcDate(
      std::string_view symbol, core::Timestamp utcDateStart, std::size_t count,
      const core::CancellationToken &cancellation = {}) const;

  /// Reopens the exact immutable rows previously selected. Every identity and
  /// span field is verified against the loaded snapshot; range reselection is
  /// intentionally not permitted at this seam.
  [[nodiscard]] core::Result<SnapshotSelection>
  readExact(const DataSelectionIdentity &identity,
            const core::CancellationToken &cancellation = {}) const;

private:
  std::unique_ptr<Impl> impl_;
};

} // namespace bte::data
