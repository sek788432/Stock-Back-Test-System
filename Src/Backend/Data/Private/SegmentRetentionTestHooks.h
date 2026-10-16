#pragma once

#include <cstdint>

namespace bte::data::testing {

enum class RetentionFailurePoint : std::uint8_t {
  none,
  databaseOpen,
  sqlExecution,
  boundPreparation,
  boundBinding,
  boundExecution,
  queryPreparation,
  queryBinding,
  queryExecution,
  queryNullIdentity,
  countPreparation,
  countExecution,
};

void failRetentionAfter(RetentionFailurePoint point,
                        std::uint32_t occurrencesToSkip = 0) noexcept;
void clearRetentionFailure() noexcept;

} // namespace bte::data::testing
