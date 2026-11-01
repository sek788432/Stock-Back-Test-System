#include "Bte/Results/ReproducibilityCoordinator.h"

#include <atomic>
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
    // Keep advisory callbacks isolated while retaining a diagnostic count.
    static std::atomic_size_t observerFailureCount = 0;
    observerFailureCount.fetch_add(1, std::memory_order_relaxed);
  }
}

} // namespace

ReproducibilityCoordinator::ReproducibilityCoordinator(
    VerificationStore &store) noexcept
    : store_{&store} {}

namespace {

core::Result<VerificationAttemptOutput>
invokeAttempt(const RunDescriptor &frozenDescriptor,
              const VerificationAttemptRunner &runAttempt,
              const core::CancellationToken &cancellation,
              const VerificationResultIdPublisher &publishResultId,
              std::optional<core::Error> &publicationError) {
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
  return output;
}

core::Result<void>
failVerificationAttempt(VerificationStore &store,
                        VerificationEvidence &evidence,
                        const core::Error &error, const bool canceled,
                        const std::optional<std::string> &resultId,
                        const VerificationProgressObserver &observeProgress) {
  auto terminal = canceled ? store.cancelAttempt(evidence.verificationId,
                                                 error.message, resultId)
                           : store.failAttempt(evidence.verificationId,
                                               "attemptExecutionFailed",
                                               error.message, resultId);
  if (!terminal.ok()) {
    return terminal.error();
  }
  evidence = std::move(terminal).value();
  observe(observeProgress, evidence);
  return {};
}

core::Result<void>
runVerificationAttempt(VerificationStore &store, VerificationEvidence &evidence,
                       const RunDescriptor &frozenDescriptor,
                       const VerificationAttemptRunner &runAttempt,
                       const core::CancellationToken &cancellation,
                       const VerificationProgressObserver &observeProgress) {
  auto started = store.beginAttempt(evidence.verificationId);
  if (!started.ok()) {
    return started.error();
  }
  evidence = std::move(started).value();
  observe(observeProgress, evidence);

  std::optional<std::string> resultId;
  std::optional<core::Error> publicationError;
  const VerificationResultIdPublisher publishResultId =
      [&store, &evidence, &resultId, &publicationError, &observeProgress](
          const std::string &publishedResultId) -> core::Result<void> {
    auto attached =
        store.attachResultId(evidence.verificationId, publishedResultId);
    if (!attached.ok()) {
      publicationError = attached.error();
      return attached.error();
    }
    evidence = std::move(attached).value();
    resultId = publishedResultId;
    observe(observeProgress, evidence);
    return {};
  };
  auto output = invokeAttempt(frozenDescriptor, runAttempt, cancellation,
                              publishResultId, publicationError);
  if (!output.ok()) {
    const auto canceled = output.error().code == core::ErrorCode::cancelled ||
                          cancellation.isCancellationRequested();
    return failVerificationAttempt(store, evidence, output.error(), canceled,
                                   resultId, observeProgress);
  }
  auto completed =
      store.completeAttempt(evidence.verificationId, output.value().descriptor,
                            output.value().result, output.value().status);
  if (!completed.ok()) {
    return completed.error();
  }
  evidence = std::move(completed).value();
  observe(observeProgress, evidence);
  return {};
}

} // namespace

core::Result<VerificationEvidence> ReproducibilityCoordinator::verify(
    const RunDescriptor &frozenDescriptor,
    const VerificationAttemptRunner &runAttempt,
    const core::CancellationToken &cancellation,
    const VerificationProgressObserver &observeProgress) const {
  if (!runAttempt) {
    return core::makeError(core::ErrorCode::invalidArgument,
                           "A verification attempt runner is required");
  }
  const std::unique_lock executionLock{verificationExecutionMutex(),
                                       std::try_to_lock};
  if (!executionLock.owns_lock()) {
    return core::makeError(core::ErrorCode::resourceBusy,
                           "Another reproducibility verification is active");
  }
  auto created = store_->create(frozenDescriptor);
  if (!created.ok()) {
    return created.error();
  }
  auto evidence = std::move(created).value();
  observe(observeProgress, evidence);
  while (evidence.status == VerificationStatus::running) {
    if (cancellation.isCancellationRequested()) {
      auto canceled = store_->cancel(evidence.verificationId);
      if (!canceled.ok()) {
        return canceled.error();
      }
      evidence = std::move(canceled).value();
      observe(observeProgress, evidence);
      break;
    }
    auto attempt =
        runVerificationAttempt(*store_, evidence, frozenDescriptor, runAttempt,
                               cancellation, observeProgress);
    if (!attempt.ok()) {
      return attempt.error();
    }
    if (evidence.status != VerificationStatus::running) {
      break;
    }
  }
  return evidence;
}

} // namespace bte::results
