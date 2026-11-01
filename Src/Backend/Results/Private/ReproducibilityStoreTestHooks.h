#pragma once

#include <cstdint>

namespace bte::results::testing {

enum class ReproducibilitySystemCall : std::uint8_t {
  openLock,
  lockExclusive,
  openStagingFile,
  writeStagingFile,
  flushStagingFile,
  closeStagingFile,
  installEvidence,
  replaceEvidence,
};

void failNextReproducibilitySystemCall(
    ReproducibilitySystemCall systemCall,
    std::uint32_t successfulCallsBeforeFailure, int errorNumber) noexcept;
void clearReproducibilitySystemCallFailure() noexcept;

} // namespace bte::results::testing
