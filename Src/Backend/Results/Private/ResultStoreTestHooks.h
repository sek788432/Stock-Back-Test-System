#pragma once

#include <cstdint>
#include <string>

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
void clearFailure();
[[nodiscard]] bool failurePending() noexcept;
void forceNextResultId(std::string resultId);

} // namespace bte::results::testing
