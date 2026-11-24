#pragma once

#include <cstddef>
#include <cstdint>

namespace bte::data::testing {

enum class SnapshotFailurePoint : std::uint8_t {
  none,
  buildCancellation,
  artifactWrite,
  artifactInspection,
  artifactRead,
  manifestWrite,
  snapshotRemovalRollback,
  openCancellation,
  exactReadCancellation,
  historyLookupCancellation,
  historyDataDayScanCancellation,
};

void failSnapshotAt(SnapshotFailurePoint point) noexcept;
void clearSnapshotFailure() noexcept;
void resetSnapshotSegmentReadCount() noexcept;
[[nodiscard]] std::size_t snapshotSegmentReadCount() noexcept;

} // namespace bte::data::testing
