#include "Bte/Bindings/ResultCatalogSnapshot.h"

#include <algorithm>
#include <utility>
#include <vector>

namespace bte::bindings {
namespace {

ResultRunStatus translateStatus(const results::RunStatus status) {
  switch (status) {
  case results::RunStatus::running:
    return ResultRunStatus::running;
  case results::RunStatus::completed:
    return ResultRunStatus::completed;
  case results::RunStatus::failed:
    return ResultRunStatus::failed;
  case results::RunStatus::canceled:
    return ResultRunStatus::canceled;
  case results::RunStatus::interrupted:
    return ResultRunStatus::interrupted;
  case results::RunStatus::incomplete:
    return ResultRunStatus::incomplete;
  }
  return ResultRunStatus::incomplete;
}

ResultRecoveryReason
translateReason(const results::ResultRecoveryReason reason) {
  switch (reason) {
  case results::ResultRecoveryReason::legacyUnleased:
    return ResultRecoveryReason::legacyUnleased;
  case results::ResultRecoveryReason::invalidLease:
    return ResultRecoveryReason::invalidLease;
  case results::ResultRecoveryReason::invalidPrefix:
    return ResultRecoveryReason::invalidPrefix;
  case results::ResultRecoveryReason::missingReferences:
    return ResultRecoveryReason::missingReferences;
  case results::ResultRecoveryReason::orphanedInitialization:
    return ResultRecoveryReason::orphanedInitialization;
  }
  return ResultRecoveryReason::invalidPrefix;
}

} // namespace

ResultRunStatus toResultRunStatus(const results::RunStatus status) {
  return translateStatus(status);
}

ResultCatalogSnapshot makeResultCatalogSnapshot(
    std::vector<results::ResultSummary> entries,
    const std::span<const results::ResultRecoveryNotice> notices) {
  std::vector<ResultCatalogEntry> translatedEntries;
  translatedEntries.reserve(entries.size());
  for (auto &entry : entries) {
    translatedEntries.push_back(ResultCatalogEntry{
        .resultId = std::move(entry.resultId),
        .displayName = std::move(entry.displayName),
        .canonicalResultHash = std::move(entry.canonicalResultHash),
        .status = translateStatus(entry.status),
        .savedUtcMillis = entry.savedUtcMillis,
        .available = entry.available,
        .unavailableReason = std::move(entry.unavailableReason),
    });
  }

  std::vector<ResultRecoveryNotice> translatedNotices;
  translatedNotices.reserve(notices.size());
  for (const auto &notice : notices) {
    translatedNotices.push_back(ResultRecoveryNotice{
        .resultId = notice.resultId,
        .reason = translateReason(notice.reason),
        .diagnostic = notice.diagnostic,
    });
  }
  return ResultCatalogSnapshot{
      .entries = std::move(translatedEntries),
      .notices = std::move(translatedNotices),
  };
}

} // namespace bte::bindings
