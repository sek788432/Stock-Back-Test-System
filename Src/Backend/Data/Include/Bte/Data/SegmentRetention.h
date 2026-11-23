#pragma once

#include "Bte/Core/Result.h"

#include <cstddef>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace bte::data {

class SegmentRetentionStore final {
private:
  struct ConstructionKey final {};

public:
  explicit SegmentRetentionStore(ConstructionKey,
                                 std::filesystem::path storeDirectory,
                                 std::shared_ptr<std::mutex> storeMutex);
  ~SegmentRetentionStore() = default;
  SegmentRetentionStore(const SegmentRetentionStore &) = delete;
  SegmentRetentionStore &operator=(const SegmentRetentionStore &) = delete;
  SegmentRetentionStore(SegmentRetentionStore &&) = delete;
  SegmentRetentionStore &operator=(SegmentRetentionStore &&) = delete;

  [[nodiscard]] static core::Result<std::unique_ptr<SegmentRetentionStore>>
  open(const std::filesystem::path &storeDirectory);

  [[nodiscard]] core::Result<std::size_t>
  acquire(const std::string &resultId,
          const std::vector<std::string> &segmentIds) const;

  [[nodiscard]] core::Result<std::vector<std::string>>
  references(const std::string &resultId) const;

  [[nodiscard]] core::Result<std::vector<std::string>>
  release(const std::string &resultId) const;

  /// Discards a provisional pin while the caller holds the snapshot
  /// publication lock and the corresponding snapshot has not been published.
  [[nodiscard]] core::Result<void>
  abandonUnpublishedSnapshot(const std::string &snapshotId) const;

  /// Recovers interrupted snapshot pins and unpublished segment artifacts
  /// while the caller holds the snapshot publication lock, so active
  /// publishers and removals cannot be mistaken for interrupted operations.
  [[nodiscard]] core::Result<void> recoverUnpublishedSnapshotPins() const;

private:
  [[nodiscard]] core::Result<void> recoverAbandonedSnapshotPins() const;
  [[nodiscard]] core::Result<void> clearStagedSnapshotArtifacts() const;
  [[nodiscard]] core::Result<void> trashUnpublishedSnapshotSegments() const;

  std::filesystem::path storeDirectory_;
  // All instances for this canonical store share one lock. Retention
  // transactions include segment file moves, and TSan cannot observe the OS
  // locks SQLite uses for WAL coordination. SQLite BEGIN IMMEDIATE remains
  // the cross-process authority for the same transaction protocol.
  std::shared_ptr<std::mutex> storeMutex_;
};

} // namespace bte::data
