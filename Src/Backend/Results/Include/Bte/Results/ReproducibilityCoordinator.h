#pragma once

#include "Bte/Core/Cancellation.h"
#include "Bte/Core/Result.h"
#include "Bte/Results/ReproducibilityStore.h"

#include <functional>
#include <optional>
#include <string>

namespace bte::results {

struct VerificationAttemptOutput {
  RunDescriptor descriptor;
  FinalizedResult result;
  RunStatus status = RunStatus::incomplete;
};

using VerificationResultIdPublisher =
    std::function<core::Result<void>(const std::string &)>;
using VerificationAttemptRunner =
    std::function<core::Result<VerificationAttemptOutput>(
        const RunDescriptor &, const core::CancellationToken &,
        const VerificationResultIdPublisher &)>;
using VerificationProgressObserver =
    std::function<void(const VerificationEvidence &)>;

/// Runs three fresh Backtests sequentially and persists their evidence.
/// The attempt runner owns one execution and must create a fresh Strategy
/// worker for each invocation. Result drafts remain under explicit user
/// publication; this coordinator never promotes them.
class ReproducibilityCoordinator final {
public:
  explicit ReproducibilityCoordinator(VerificationStore &store) noexcept;

  [[nodiscard]] core::Result<VerificationEvidence>
  verify(const RunDescriptor &frozenDescriptor,
         const VerificationAttemptRunner &runAttempt,
         const core::CancellationToken &cancellation = {},
         const VerificationProgressObserver &observeProgress = {}) const;

private:
  VerificationStore *store_;
};

} // namespace bte::results
