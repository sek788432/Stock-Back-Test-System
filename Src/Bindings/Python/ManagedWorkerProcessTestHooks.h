#pragma once

#include <cstdint>

namespace bte::bindings::python::testing {

enum class ManagedWorkerSystemCall : std::uint8_t {
  sharedMemoryCreate,
  sharedMemoryTruncate,
  sharedMemoryMap,
  sharedMemoryReadOnlyOpen,
  sharedMemoryStat,
  pipeCreate,
  closeOnExec,
  duplicateDescriptor,
  poll,
  protocolRead,
  protocolWrite,
  terminateSignal,
};

[[nodiscard]] std::uint64_t managedWorkerProcessStartCount() noexcept;
void failNextManagedWorkerSystemCall(ManagedWorkerSystemCall systemCall,
                                     std::uint32_t successfulCallsBeforeFailure,
                                     int errorNumber) noexcept;
void clearManagedWorkerSystemCallFailure() noexcept;

} // namespace bte::bindings::python::testing
