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
  databaseOpen,
  sqlExecution,
  statementPreparation,
  textBinding,
  integerBinding,
  optionalIntegerBinding,
  statementExecution,
};

void failNext(FailurePoint point) noexcept;
void failAfter(FailurePoint point, std::uint32_t occurrencesToSkip) noexcept;
void clearFailure() noexcept;

} // namespace bte::results::testing
