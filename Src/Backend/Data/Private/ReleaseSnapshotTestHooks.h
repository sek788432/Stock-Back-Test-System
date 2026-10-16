#pragma once

#include <cstdint>

namespace bte::data::testing {

enum class SnapshotFailurePoint : std::uint8_t {
  none,
  buildCancellation,
  artifactWrite,
  artifactInspection,
  artifactRead,
  manifestWrite,
  openCancellation,
};

void failSnapshotAt(SnapshotFailurePoint point) noexcept;
void clearSnapshotFailure() noexcept;

} // namespace bte::data::testing
