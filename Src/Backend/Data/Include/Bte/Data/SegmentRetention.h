#pragma once

#include "Bte/Core/Result.h"

#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace bte::data {

class SegmentRetentionStore final {
private:
  struct ConstructionKey final {};

public:
  explicit SegmentRetentionStore(ConstructionKey,
                                 std::filesystem::path storeDirectory);
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
  release(const std::string &resultId) const;

private:
  std::filesystem::path storeDirectory_;
};

} // namespace bte::data
