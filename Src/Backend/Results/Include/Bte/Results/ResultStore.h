#pragma once

#include "Bte/Core/Result.h"
#include "Bte/Core/Time.h"
#include "Bte/Data/ReleaseSnapshot.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
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

struct RunDescriptor {
  std::vector<std::string> universe;
  core::DateRange range;
  std::int64_t initialCapitalMicrodollars = 0;
  std::string strategyId;
  std::string strategyHash;
  data::DataSelectionIdentity dataSelection;

  bool operator==(const RunDescriptor &) const = default;
};

struct CanonicalRecord {
  std::uint64_t sequence = 0;
  core::Timestamp timestamp;
  std::string symbol;
  RecordFamily family = RecordFamily::log;
  OrderSide side = OrderSide::none;
  std::optional<std::int64_t> quantityShares;
  std::optional<std::int64_t> priceNanodollars;
  std::optional<std::int64_t> amountMicrodollars;
  std::optional<std::int64_t> cashMicrodollars;
  std::optional<std::int64_t> marketValueMicrodollars;
  std::optional<std::int64_t> equityMicrodollars;
  std::optional<std::int64_t> positionShares;
  std::string text;

  bool operator==(const CanonicalRecord &) const = default;
};

struct FinalSummary {
  std::optional<std::int64_t> finalEquityMicrodollars;
  std::optional<std::int64_t> pnlMicrodollars;

  bool operator==(const FinalSummary &) const = default;
};

struct FinalizedResult {
  std::string resultId;
  std::string canonicalResultHash;
};

struct ResultSummary {
  std::string resultId;
  std::string canonicalResultHash;
  RunStatus status = RunStatus::incomplete;
  std::int64_t savedUtcMillis = 0;
  bool available = false;
  std::string unavailableReason;
};

struct OpenedResult {
  std::string resultId;
  std::string canonicalResultHash;
  RunStatus status = RunStatus::incomplete;
  std::string terminalReason;
  RunDescriptor descriptor;
  std::vector<CanonicalRecord> records;
  FinalSummary summary;
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
  finalizeAndPromote(RunStatus status, const FinalSummary &summary,
                     std::string terminalReason = {});

private:
  friend class ResultStore;
  std::unique_ptr<Impl> impl_;
};

class ResultStore final {
private:
  struct ConstructionKey final {};

public:
  explicit ResultStore(ConstructionKey, std::filesystem::path root,
                       std::filesystem::path dataStore);
  ~ResultStore() = default;
  ResultStore(const ResultStore &) = delete;
  ResultStore &operator=(const ResultStore &) = delete;
  ResultStore(ResultStore &&) = delete;
  ResultStore &operator=(ResultStore &&) = delete;

  [[nodiscard]] static core::Result<std::unique_ptr<ResultStore>>
  open(const std::filesystem::path &root,
       const std::filesystem::path &dataStore);

  [[nodiscard]] core::Result<std::unique_ptr<ResultWriter>>
  begin(const RunDescriptor &descriptor) const;
  [[nodiscard]] core::Result<std::vector<ResultSummary>> list() const;
  [[nodiscard]] core::Result<OpenedResult>
  openResult(const std::string &resultId) const;
  [[nodiscard]] core::Result<void>
  moveToTrash(const std::string &resultId) const;
  [[nodiscard]] core::Result<void> restore(const std::string &resultId) const;
  [[nodiscard]] core::Result<void> purge(const std::string &resultId) const;

private:
  std::filesystem::path root_;
  std::filesystem::path dataStore_;
};

} // namespace bte::results
