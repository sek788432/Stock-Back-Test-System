#pragma once

#include <cstdint>

namespace bte::bindings::python::testing {

enum class RuntimeProfileSystemCall : std::uint8_t {
  open,
  stat,
  memoryMap,
};

void failNextRuntimeProfileSystemCall(RuntimeProfileSystemCall systemCall,
                                      int errorNumber) noexcept;
void clearRuntimeProfileSystemCallFailure() noexcept;

} // namespace bte::bindings::python::testing
