#include "Bte/Results/ReproducibilityCoordinator.h"

#include <exception>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

namespace bte::results {
namespace {

std::mutex &verificationExecutionMutex() {
  static std::mutex mutex;
  return mutex;
}

void observe(const VerificationProgressObserver &observer,
             const VerificationEvidence &evidence) noexcept {
  if (!observer) {
    return;
  }
  try {
    observer(evidence);
  } catch (...) {
    // UI progress is advisory and cannot change the persisted execution.
  }
}

} // namespace

ReproducibilityCoordinator::ReproducibilityCoordinator(
    VerificationStore &store) noexcept
    : store_{store} {}

core::Result<VerificationEvidence> ReproducibilityCoordinator::verify(
    const RunDescriptor &frozenDescriptor,
    const VerificationAttemptRunner &runAttempt,
    const core::CancellationToken &cancellation,
    const VerificationProgressObserver &observeProgress) const {
  if (!runAttempt) {
    return core::makeError(core::ErrorCode::invalidArgument,
                           "A verification attempt runner is required");
  }
  std::unique_lock executionLock{verificationExecutionMutex(),
                                 std::try_to_lock};
  if (!executionLock.owns_lock()) {
    return core::makeError(core::ErrorCode::resourceBusy,
                           "Another reproducibility verification is active");
  }

  auto evidence = store_.create(frozenDescriptor);
  if (!evidence.ok()) {
    return evidence.error();
  }
  observe(observeProgress, evidence.value());

  while (evidence.value().status == VerificationStatus::running) {
    if (cancellation.isCancellationRequested()) {
      auto canceled = store_.cancel(evidence.value().verificationId);
      if (!canceled.ok()) {
        return canceled.error();
      }
      evidence = std::move(canceled);
      observe(observeProgress, evidence.value());
      break;
    }

    auto started = store_.beginAttempt(evidence.value().verificationId);
    if (!started.ok()) {
      return started.error();
    }
    evidence = std::move(started);
    observe(observeProgress, evidence.value());

    std::optional<std::string> resultId;
    std::optional<core::Error> publicationError;
    const VerificationResultIdPublisher publishResultId =
        [this, &evidence, &resultId, &publicationError,
         &observeProgress](const std::string &id) -> core::Result<void> {
      auto attached =
          store_.attachResultId(evidence.value().verificationId, id);
      if (!attached.ok()) {
        publicationError = attached.error();
        return attached.error();
      }
      evidence = std::move(attached);
      resultId = id;
      observe(observeProgress, evidence.value());
      return {};
    };

    core::Result<VerificationAttemptOutput> output = core::makeError(
        core::ErrorCode::internal, "Verification attempt did not run");
    try {
      output = runAttempt(frozenDescriptor, cancellation, publishResultId);
    } catch (const std::exception &error) {
      output = core::makeError(core::ErrorCode::internal, error.what());
    } catch (...) {
      output = core::makeError(core::ErrorCode::internal,
                               "Verification attempt raised an unknown error");
    }
    if (publicationError.has_value()) {
      return *publicationError;
    }

    if (!output.ok()) {
      const auto canceled = output.error().code == core::ErrorCode::cancelled ||
                            cancellation.isCancellationRequested();
      auto terminal =
          canceled ? store_.cancelAttempt(evidence.value().verificationId,
                                          output.error().message, resultId)
                   : store_.failAttempt(evidence.value().verificationId,
                                        "attemptExecutionFailed",
                                        output.error().message, resultId);
      if (!terminal.ok()) {
        return terminal.error();
      }
      evidence = std::move(terminal);
      observe(observeProgress, evidence.value());
      break;
    }

    auto completed = store_.completeAttempt(
        evidence.value().verificationId, output.value().descriptor,
        output.value().result, output.value().status);
    if (!completed.ok()) {
      return completed.error();
    }
    evidence = std::move(completed);
    observe(observeProgress, evidence.value());
  }
  return evidence;
}

} // namespace bte::results
