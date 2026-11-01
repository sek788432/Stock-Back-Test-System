#pragma once

#include "Bte/Core/Bar.h"
#include "Bte/Core/Cancellation.h"
#include "Bte/Core/Result.h"
#include "Bte/Core/Time.h"
#include "Bte/Data/ReleaseSnapshot.h"

#include <algorithm> // IWYU pragma: keep
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace bte::results {

enum class RunStatus : std::uint8_t {
  running,
  completed,
  failed,
  canceled,
  interrupted,
  incomplete,
};

enum class RecordFamily : std::uint8_t {
  order,
  fill,
  portfolio,
  cost,
  warning,
  log,
  terminalDiagnostic,
};

enum class OrderSide : std::uint8_t { none, buy, sell };

enum class StrategyMode : std::uint8_t {
  starter,
  selectableConditions,
  python,
  legacyUnrecorded,
};

enum class ProvenanceAvailabilityState : std::uint8_t {
  recorded,
  legacyUnrecorded,
};

struct RuntimePackageIdentity {
  std::string name;
  std::string version;
  std::string artifactHash;

  bool operator==(const RuntimePackageIdentity &) const = default;
};

struct RuntimeProfileProvenance {
  std::string profileId;
  std::string canonicalManifestJson;
  std::string manifestHash;
  std::string pythonVersion;
  std::string pythonAbi;
  std::vector<RuntimePackageIdentity> packages;
  std::string stockbtSourceHash;

  bool operator==(const RuntimeProfileProvenance &) const = default;
};

struct ResultProvenance {
  std::optional<std::string> engineBuild;
  std::string timeframe;
  std::optional<std::string> costProfile;
  std::optional<std::int64_t> commissionRatePartsPerBillion;
  std::optional<std::int64_t> slippageRatePartsPerBillion;
  std::optional<std::int64_t> seed;
  StrategyMode strategyMode = StrategyMode::starter;
  std::string apiVersion;
  std::string templateVersion;
  std::string canonicalParametersJson = "{}";
  std::string sourceUtf8;
  std::string sourceHash;
  std::string canonicalPlanJson;
  std::string planHash;
  std::optional<RuntimeProfileProvenance> runtime;
  std::map<std::string, ProvenanceAvailabilityState> provenanceAvailability;

  bool operator==(const ResultProvenance &) const = default;
};

struct ResultCapabilities {
  bool orders = true;
  bool fills = true;
  bool postSlicePortfolio = true;
  bool slippageCosts = true;
  bool warnings = true;
  bool terminalDiagnostics = true;
  bool trades = false;
  bool metrics = false;
  bool strategyLogs = false;
  bool indicatorSnapshots = false;
  bool corporateActions = false;
  bool margin = false;

  bool operator==(const ResultCapabilities &) const = default;
};

struct RunDescriptor {
  std::int64_t schemaVersion = 2;
  std::vector<std::string> universe;
  core::DateRange range;
  std::int64_t initialCapitalMicrodollars = 0;
  std::string strategyId;
  std::string strategyHash;
  data::DataSelectionIdentity dataSelection;
  ResultProvenance provenance{};

  bool operator==(const RunDescriptor &) const = default;
};

// Data-only aggregate initializers do not produce reliably attributable gcov
// branches across supported compilers. GCOVR_EXCL_START
struct CanonicalRecord {
  std::uint64_t sequence = 0;
  core::Timestamp timestamp;
  std::string symbol;
  RecordFamily family = RecordFamily::log;
  OrderSide side = OrderSide::none;
  std::optional<std::int64_t> quantityShares = std::nullopt;
  std::optional<std::int64_t> priceNanodollars = std::nullopt;
  std::optional<std::int64_t> amountMicrodollars = std::nullopt;
  std::optional<std::int64_t> cashMicrodollars = std::nullopt;
  std::optional<std::int64_t> marketValueMicrodollars = std::nullopt;
  std::optional<std::int64_t> equityMicrodollars = std::nullopt;
  std::optional<std::int64_t> pnlMicrodollars = std::nullopt;
  std::optional<std::int64_t> positionShares = std::nullopt;
  // NOLINTNEXTLINE(readability-redundant-member-init): GCC requires it.
  std::string text{};

  bool operator==(const CanonicalRecord &) const = default;
};
// GCOVR_EXCL_STOP

struct FinalSummary {
  std::optional<std::int64_t> finalEquityMicrodollars = std::nullopt;
  std::optional<std::int64_t> pnlMicrodollars = std::nullopt;
  bool diagnosticsComplete = true;

  bool operator==(const FinalSummary &) const = default;
};

struct FinalizedResult {
  std::string resultId;
  std::string canonicalResultHash;
};

struct ResultSummary {
  std::string resultId;
  std::string displayName;
  std::string canonicalResultHash;
  RunStatus status = RunStatus::incomplete;
  std::int64_t savedUtcMillis = 0;
  bool available = false;
  std::string unavailableReason;
};

struct MigrationSourceIdentity {
  std::string resultId;
  std::string canonicalResultHash;

  bool operator==(const MigrationSourceIdentity &) const = default;
};

struct OpenedResult {
  std::int64_t schemaVersion = 2;
  std::string resultId;
  std::string displayName;
  std::string canonicalResultHash;
  RunStatus status = RunStatus::incomplete;
  std::int64_t savedUtcMillis = 0;
  std::string terminalReason;
  RunDescriptor descriptor;
  std::vector<CanonicalRecord> records;
  FinalSummary summary;
  ResultCapabilities capabilities;
  std::optional<MigrationSourceIdentity> migratedFrom;
};

struct OpenedResultWithData {
  OpenedResult result;
  data::SnapshotSelection selection;
};

enum class ResultRecoveryReason : std::uint8_t {
  legacyUnleased,
  invalidLease,
  invalidPrefix,
  missingReferences,
  orphanedInitialization,
};

struct ResultRecoveryNotice {
  std::string resultId;
  ResultRecoveryReason reason = ResultRecoveryReason::invalidPrefix;
  std::string diagnostic;

  bool operator==(const ResultRecoveryNotice &) const = default;
};

class ResultWriter final {
private:
  struct ConstructionKey final {};
  struct Impl;

public:
  explicit ResultWriter(ConstructionKey, std::unique_ptr<Impl> impl);
  ~ResultWriter();
  ResultWriter(const ResultWriter &) = delete;
  ResultWriter &operator=(const ResultWriter &) = delete;
  ResultWriter(ResultWriter &&) = delete;
  ResultWriter &operator=(ResultWriter &&) = delete;

  [[nodiscard]] const std::string &resultId() const noexcept;
  [[nodiscard]] core::Result<void>
  append(const std::vector<CanonicalRecord> &records);
  [[nodiscard]] core::Result<FinalizedResult>
  finalizeDraft(RunStatus status, const FinalSummary &summary,
                const std::string &terminalReason = {});
  [[nodiscard]] core::Result<FinalizedResult>
  finalizeDraft(RunStatus status, const FinalSummary &summary,
                const std::string &terminalReason,
                std::span<const CanonicalRecord> terminalTail);
  [[nodiscard]] core::Result<FinalizedResult>
  finalizeAndPromote(RunStatus status, const FinalSummary &summary,
                     const std::string &terminalReason = {});

private:
  enum class FinalizationTarget : std::uint8_t { draft, promoted };

  [[nodiscard]] core::Result<FinalizedResult>
  finalize(RunStatus status, const FinalSummary &summary,
           const std::string &terminalReason, FinalizationTarget target,
           std::span<const CanonicalRecord> terminalTail = {});

  friend class ResultStore;
  std::unique_ptr<Impl> impl_;
};

class ResultStore final {
private:
  struct ConstructionKey final {};

public:
  explicit ResultStore(ConstructionKey, std::filesystem::path root,
                       std::filesystem::path dataStore,
                       std::vector<ResultRecoveryNotice> notices = {});
  ~ResultStore() = default;
  ResultStore(const ResultStore &) = delete;
  ResultStore &operator=(const ResultStore &) = delete;
  ResultStore(ResultStore &&) = delete;
  ResultStore &operator=(ResultStore &&) = delete;

  [[nodiscard]] static core::Result<std::unique_ptr<ResultStore>>
  open(const std::filesystem::path &root,
       const std::filesystem::path &dataStore,
       const core::CancellationToken &cancellation = {});

  [[nodiscard]] const std::vector<ResultRecoveryNotice> &
  recoveryNotices() const noexcept;

  [[nodiscard]] core::Result<std::unique_ptr<ResultWriter>>
  begin(const RunDescriptor &descriptor) const;
  [[nodiscard]] core::Result<void>
  validateSelectedBars(const data::DataSelectionIdentity &identity,
                       const std::vector<data::SnapshotBar> &bars,
                       const core::CancellationToken &cancellation = {}) const;
  [[nodiscard]] core::Result<std::vector<ResultSummary>>
  list(const core::CancellationToken &cancellation = {}) const;
  [[nodiscard]] core::Result<std::vector<ResultSummary>>
  listDrafts(const core::CancellationToken &cancellation = {}) const;
  [[nodiscard]] core::Result<std::vector<ResultSummary>>
  listTrash(const core::CancellationToken &cancellation = {}) const;
  [[nodiscard]] core::Result<FinalizedResult>
  publishDraft(const std::string &resultId,
               const std::string &displayName = {}) const;
  [[nodiscard]] core::Result<void>
  renameResult(const std::string &resultId,
               const std::string &displayName) const;
  [[nodiscard]] core::Result<void>
  discardDraft(const std::string &resultId) const;
  [[nodiscard]] core::Result<OpenedResult>
  openResult(const std::string &resultId,
             const core::CancellationToken &cancellation = {}) const;
  [[nodiscard]] core::Result<OpenedResultWithData>
  openResultWithData(const std::string &resultId,
                     const core::CancellationToken &cancellation = {}) const;
  [[nodiscard]] core::Result<FinalizedResult>
  importResult(const std::filesystem::path &source) const;
  [[nodiscard]] core::Result<FinalizedResult>
  migrateSchema2Copy(const std::string &resultId) const;
  [[nodiscard]] core::Result<void>
  moveToTrash(const std::string &resultId) const;
  [[nodiscard]] core::Result<void> restore(const std::string &resultId) const;
  [[nodiscard]] core::Result<void> purge(const std::string &resultId) const;

private:
  std::filesystem::path root_;
  std::filesystem::path dataStore_;
  std::vector<ResultRecoveryNotice> recoveryNotices_;
};

} // namespace bte::results
