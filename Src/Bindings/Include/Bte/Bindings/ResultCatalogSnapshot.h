#pragma once

#include "Bte/Results/ResultStore.h"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace bte::bindings {

enum class ResultRunStatus : std::uint8_t {
  running,
  completed,
  failed,
  canceled,
  interrupted,
  incomplete,
};

struct ResultCatalogEntry final {
  std::string resultId;
  std::string displayName;
  std::string canonicalResultHash;
  ResultRunStatus status = ResultRunStatus::incomplete;
  std::int64_t savedUtcMillis = 0;
  bool available = false;
  std::string unavailableReason;
};

enum class ResultRecoveryReason : std::uint8_t {
  legacyUnleased,
  invalidLease,
  invalidPrefix,
  missingReferences,
  orphanedInitialization,
};

struct ResultRecoveryNotice final {
  std::string resultId;
  ResultRecoveryReason reason = ResultRecoveryReason::invalidPrefix;
  std::string diagnostic;

  bool operator==(const ResultRecoveryNotice &) const = default;
};

/// Value-owned entries and recovery notices from one opened Results store.
struct ResultCatalogSnapshot final {
  std::vector<ResultCatalogEntry> entries;
  std::vector<ResultRecoveryNotice> notices;
};

/// Translate Results-owned catalog and recovery values for queued UI delivery.
[[nodiscard]] ResultCatalogSnapshot makeResultCatalogSnapshot(
    std::vector<results::ResultSummary> entries,
    std::span<const results::ResultRecoveryNotice> notices);

[[nodiscard]] ResultRunStatus toResultRunStatus(results::RunStatus status);

} // namespace bte::bindings
