#pragma once

#include "Bte/Core/Result.h"
#include "Bte/Results/ResultStore.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace bte::results {

enum class VerificationStatus : std::uint8_t {
  running,
  verified,
  mismatch,
  failed,
  canceled,
  interrupted,
};

struct VerificationIdentity {
  std::string canonicalJson;
  std::string sha256;

  bool operator==(const VerificationIdentity &) const = default;
};

struct VerificationError {
  std::string code;
  std::string message;

  bool operator==(const VerificationError &) const = default;
};

struct VerificationAttempt {
  std::size_t ordinal = 0;
  std::optional<std::string> resultId;
  std::optional<RunStatus> terminalStatus;
  std::optional<std::string> canonicalResultHash;
  std::optional<VerificationError> error;

  bool operator==(const VerificationAttempt &) const = default;
};

struct VerificationEvidence {
  std::string verificationId;
  VerificationIdentity identity;
  VerificationStatus status = VerificationStatus::running;
  std::size_t progressOrdinal = 0;
  std::vector<VerificationAttempt> attempts;
  std::string integrityHash;

  bool operator==(const VerificationEvidence &) const = default;
};

struct VerificationListEntry {
  std::string verificationId;
  std::optional<VerificationEvidence> evidence;
  std::string diagnostic;
};

class VerificationStore final {
private:
  struct Impl;

public:
  explicit VerificationStore(std::unique_ptr<Impl> impl);
  ~VerificationStore();
  VerificationStore(const VerificationStore &) = delete;
  VerificationStore &operator=(const VerificationStore &) = delete;
  VerificationStore(VerificationStore &&) = delete;
  VerificationStore &operator=(VerificationStore &&) = delete;

  [[nodiscard]] static core::Result<std::unique_ptr<VerificationStore>>
  open(const std::filesystem::path &resultStoreRoot);

  [[nodiscard]] core::Result<VerificationEvidence>
  create(const RunDescriptor &descriptor);
  [[nodiscard]] core::Result<VerificationEvidence>
  load(const std::string &verificationId) const;
  [[nodiscard]] core::Result<std::vector<VerificationListEntry>> list() const;
  [[nodiscard]] core::Result<VerificationEvidence>
  beginAttempt(const std::string &verificationId);
  [[nodiscard]] core::Result<VerificationEvidence>
  attachResultId(const std::string &verificationId,
                 const std::string &resultId);
  [[nodiscard]] core::Result<VerificationEvidence>
  completeAttempt(const std::string &verificationId,
                  const RunDescriptor &attemptDescriptor,
                  const FinalizedResult &result, RunStatus terminalStatus);
  [[nodiscard]] core::Result<VerificationEvidence>
  failAttempt(std::string_view verificationId, std::string errorCode,
              std::string errorMessage,
              std::optional<std::string> resultId = std::nullopt);
  [[nodiscard]] core::Result<VerificationEvidence>
  cancelAttempt(std::string_view verificationId, std::string errorMessage,
                std::optional<std::string> resultId = std::nullopt);
  [[nodiscard]] core::Result<VerificationEvidence>
  cancel(const std::string &verificationId);
  [[nodiscard]] core::Result<std::vector<VerificationEvidence>>
  recoverInterrupted();
  [[nodiscard]] core::Result<bool>
  hasVerifiedEvidence(const RunDescriptor &descriptor) const;
  [[nodiscard]] core::Result<bool>
  hasVerifiedResult(const RunDescriptor &descriptor,
                    const FinalizedResult &result) const;

private:
  std::unique_ptr<Impl> impl_;
};

} // namespace bte::results
