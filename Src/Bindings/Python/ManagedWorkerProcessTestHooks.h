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
  spawnActionsInitialize,
  spawnAttributesInitialize,
  spawnActionsAddDup2,
  spawnActionsAddClose,
  spawnAttributesSetProcessGroup,
  spawnAttributesSetFlags,
  processSpawn,
  nonBlocking,
  poll,
  diagnosticRead,
  unexpectedProtocolRead,
  protocolRead,
  protocolWrite,
  terminateSignal,
  terminateProcessGroupSignal,
  processGroupProbe,
  terminateChildSignal,
  waitForChild,
};

[[nodiscard]] std::uint64_t managedWorkerProcessStartCount() noexcept;
[[nodiscard]] bool managedWorkerSystemCallFailurePending() noexcept;
void failNextManagedWorkerSystemCall(ManagedWorkerSystemCall systemCall,
                                     std::uint32_t successfulCallsBeforeFailure,
                                     int errorNumber) noexcept;
void clearManagedWorkerSystemCallFailure() noexcept;

} // namespace bte::bindings::python::testing
