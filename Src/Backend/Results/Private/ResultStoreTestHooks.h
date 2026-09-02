#pragma once

#include <cstdint>

namespace bte::results::testing {

enum class FailurePoint : std::uint8_t {
  none,
  schemaCreation,
  recordTransaction,
  hashFinalization,
  close,
  promotion,
  catalogVisibility,
};

void failNext(FailurePoint point) noexcept;
void clearFailure() noexcept;

} // namespace bte::results::testing
