#include "ManagedWorkerProcess.h"

#include <Bte/Core/Digest.h>

#if defined(BTE_ENABLE_TEST_HOOKS)
#include "ManagedWorkerProcessTestHooks.h"
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <limits>
#include <memory>
#include <random>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <spawn.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace bte::bindings::python {
namespace {

#if defined(BTE_ENABLE_TEST_HOOKS)
std::atomic_uint64_t &workerProcessStartCountStorage() {
  static std::atomic_uint64_t value{0};
  return value;
}
#endif

[[nodiscard]] core::Error workerStartError(std::string message) {
  return core::makeError(core::ErrorCode::workerStartFailed,
                         std::move(message));
}

[[nodiscard]] core::Error workerIoError(std::string message) {
  return core::makeError(core::ErrorCode::workerHandshakeFailed,
                         std::move(message));
}

[[nodiscard]] core::Error timeoutError(const core::ErrorCode code) {
  return core::makeError(code,
                         "Managed strategy worker exceeded its deadline.");
}

#if !defined(__unix__) && !defined(__APPLE__)
[[nodiscard]] core::Error unsupportedProcessError() {
  return core::makeError(
      core::ErrorCode::runtimeUnavailable,
      "Managed strategy workers are unavailable on this platform.");
}
#endif

[[nodiscard]] bool hasEmbeddedNull(const std::string &value) {
  return value.find('\0') != std::string::npos;
}

class DiagnosticBuffer final {
public:
  void append(const char *bytes, const std::size_t count) {
    if (count == 0) {
      return;
    }
    const auto addition = static_cast<std::uint64_t>(count);
    bytesSeen_ =
        addition > std::numeric_limits<std::uint64_t>::max() - bytesSeen_
            ? std::numeric_limits<std::uint64_t>::max()
            : bytesSeen_ + addition;
    constexpr std::size_t retainedPerEnd = maximumWorkerDiagnosticBytes / 2;
    const auto firstRoom = retainedPerEnd - first_.size();
    const auto firstCount = std::min(firstRoom, count);
    first_.append(bytes, firstCount);
    const auto remainingCount = count - firstCount;
    if (remainingCount > 0) {
      tail_.append(bytes + firstCount, remainingCount);
      if (tail_.size() > retainedPerEnd) {
        tail_.erase(0, tail_.size() - retainedPerEnd);
      }
    }
  }

  [[nodiscard]] CapturedDiagnosticStream snapshot() const {
    CapturedDiagnosticStream result;
    result.retainedBytes.reserve(first_.size() + tail_.size());
    result.retainedBytes += first_;
    result.retainedBytes += tail_;
    result.bytesSeen = bytesSeen_;
    result.omittedBytes = bytesSeen_ - result.retainedBytes.size();
    return result;
  }

private:
  std::string first_;
  std::string tail_;
  std::uint64_t bytesSeen_ = 0;
};

#if defined(__unix__) || defined(__APPLE__)
class UniqueDescriptor final {
public:
  UniqueDescriptor() noexcept = default;
  explicit UniqueDescriptor(const int descriptor) noexcept
      : descriptor_(descriptor) {}
  ~UniqueDescriptor() { reset(); }
  UniqueDescriptor(UniqueDescriptor &&other) noexcept
      : descriptor_(std::exchange(other.descriptor_, -1)) {}
  UniqueDescriptor &operator=(UniqueDescriptor &&other) noexcept {
    if (this != &other) {
      reset();
      descriptor_ = std::exchange(other.descriptor_, -1);
    }
    return *this;
  }
  UniqueDescriptor(const UniqueDescriptor &) = delete;
  UniqueDescriptor &operator=(const UniqueDescriptor &) = delete;

  [[nodiscard]] int get() const noexcept { return descriptor_; }
  [[nodiscard]] int release() noexcept {
    return std::exchange(descriptor_, -1);
  }
  void reset(const int descriptor = -1) noexcept {
    if (descriptor_ >= 0) {
      static_cast<void>(::close(descriptor_));
    }
    descriptor_ = descriptor;
  }

private:
  int descriptor_ = -1;
};

class SpawnActions final {
public:
  SpawnActions() : status_(::posix_spawn_file_actions_init(&actions_)) {}
  ~SpawnActions() {
    if (status_ == 0) {
      static_cast<void>(::posix_spawn_file_actions_destroy(&actions_));
    }
  }
  SpawnActions(const SpawnActions &) = delete;
  SpawnActions &operator=(const SpawnActions &) = delete;
  SpawnActions(SpawnActions &&) = delete;
  SpawnActions &operator=(SpawnActions &&) = delete;
  [[nodiscard]] bool valid() const noexcept { return status_ == 0; }
  [[nodiscard]] int addDup2(const int source, const int target) {
    return status_ == 0
               ? ::posix_spawn_file_actions_adddup2(&actions_, source, target)
               : status_;
  }
  [[nodiscard]] int addClose(const int descriptor) {
    return status_ == 0
               ? ::posix_spawn_file_actions_addclose(&actions_, descriptor)
               : status_;
  }
  [[nodiscard]] const posix_spawn_file_actions_t *get() const noexcept {
    return &actions_;
  }

private:
  posix_spawn_file_actions_t actions_{};
  int status_;
};

class SpawnAttributes final {
public:
  SpawnAttributes() : status_(::posix_spawnattr_init(&attributes_)) {}
  ~SpawnAttributes() {
    if (status_ == 0) {
      static_cast<void>(::posix_spawnattr_destroy(&attributes_));
    }
  }
  SpawnAttributes(const SpawnAttributes &) = delete;
  SpawnAttributes &operator=(const SpawnAttributes &) = delete;
  SpawnAttributes(SpawnAttributes &&) = delete;
  SpawnAttributes &operator=(SpawnAttributes &&) = delete;
  [[nodiscard]] bool valid() const noexcept { return status_ == 0; }
  [[nodiscard]] int configureProcessGroup() {
    if (status_ != 0) {
      return status_;
    }
    const auto groupStatus = ::posix_spawnattr_setpgroup(&attributes_, 0);
    if (groupStatus != 0) {
      return groupStatus;
    }
    return ::posix_spawnattr_setflags(&attributes_, POSIX_SPAWN_SETPGROUP);
  }
  [[nodiscard]] const posix_spawnattr_t *get() const noexcept {
    return &attributes_;
  }

private:
  posix_spawnattr_t attributes_{};
  int status_;
};

[[nodiscard]] int duplicateCloseOnExec(const int descriptor) {
  return ::fcntl( // NOLINT(cppcoreguidelines-pro-type-vararg): POSIX
                  // F_DUPFD_CLOEXEC takes the minimum target descriptor
                  // variadically.
      descriptor, F_DUPFD_CLOEXEC, 10);
}

[[nodiscard]] int setCloseOnExec(const int descriptor) {
  const auto flags =
      ::fcntl( // NOLINT(cppcoreguidelines-pro-type-vararg): POSIX F_GETFD reads
               // descriptor flags without an optional variadic argument.
          descriptor, F_GETFD);
  if (flags < 0) {
    return -1;
  }
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): POSIX fcntl varargs.
  return ::fcntl(descriptor, F_SETFD, flags | FD_CLOEXEC);
}

[[nodiscard]] int setNonBlocking(const int descriptor) {
  const auto flags =
      ::fcntl( // NOLINT(cppcoreguidelines-pro-type-vararg): POSIX F_GETFL reads
               // status flags before setting O_NONBLOCK.
          descriptor, F_GETFL);
  if (flags < 0) {
    return -1;
  }
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): POSIX fcntl varargs.
  return ::fcntl(descriptor, F_SETFL, flags | O_NONBLOCK);
}

[[nodiscard]] bool isWritableDescriptor(const int descriptor) {
  const auto flags =
      ::fcntl( // NOLINT(cppcoreguidelines-pro-type-vararg): POSIX F_GETFL reads
               // O_ACCMODE for the read-only check.
          descriptor, F_GETFL);
  return flags >= 0 && (flags & O_ACCMODE) == O_RDONLY;
}

[[nodiscard]] ssize_t writeWithoutSigpipe(const int descriptor,
                                          const void *data,
                                          const std::size_t size) {
  sigset_t blockedSignals{};
  sigset_t oldSignals{};
  sigemptyset(&blockedSignals);
  sigaddset(&blockedSignals, SIGPIPE);
  if (::pthread_sigmask(SIG_BLOCK, &blockedSignals, &oldSignals) != 0) {
    return -1;
  }
  sigset_t pendingSignals{};
  const auto hasPendingResult = ::sigpending(&pendingSignals);
  const bool wasAlreadyPending =
      hasPendingResult == 0 && sigismember(&pendingSignals, SIGPIPE) == 1;
  const auto result = ::write(descriptor, data, size);
  const auto writeError = errno;
  if (result < 0 && writeError == EPIPE && !wasAlreadyPending) {
    int receivedSignal = 0;
    static_cast<void>(::sigwait(&blockedSignals, &receivedSignal));
  }
  static_cast<void>(::pthread_sigmask(SIG_SETMASK, &oldSignals, nullptr));
  errno = writeError;
  return result;
}

[[nodiscard]] bool processGroupExists(const pid_t processGroup) {
  if (processGroup <= 0) {
    return false;
  }
  if (::kill(-processGroup, 0) == 0) {
    return true;
  }
  return errno == EPERM;
}

#endif

struct DiagnosticEvents final {
  short standardOutput = 0;
  short standardError = 0;
};

} // namespace

#if defined(BTE_ENABLE_TEST_HOOKS)
std::uint64_t testing::managedWorkerProcessStartCount() noexcept {
  return workerProcessStartCountStorage().load(std::memory_order_relaxed);
}
#endif

ReadOnlySharedMemory::ReadOnlySharedMemory() noexcept = default;

ReadOnlySharedMemory::ReadOnlySharedMemory(const int descriptor,
                                           std::string digest,
                                           const std::uint64_t size) noexcept
    : descriptor_(descriptor), size_(size), digest_(std::move(digest)) {}

ReadOnlySharedMemory::~ReadOnlySharedMemory() { reset(); }

ReadOnlySharedMemory::ReadOnlySharedMemory(
    ReadOnlySharedMemory &&other) noexcept
    : descriptor_(std::exchange(other.descriptor_, -1)),
      size_(std::exchange(other.size_, 0)), digest_(std::move(other.digest_)) {}

ReadOnlySharedMemory &
ReadOnlySharedMemory::operator=(ReadOnlySharedMemory &&other) noexcept {
  if (this != &other) {
    reset();
    descriptor_ = std::exchange(other.descriptor_, -1);
    size_ = std::exchange(other.size_, 0);
    digest_ = std::move(other.digest_);
  }
  return *this;
}

core::Result<ReadOnlySharedMemory>
ReadOnlySharedMemory::create(const std::string_view bytes) {
  if (bytes.empty() || bytes.size() > maximumWorkerSourceBytes) {
    return core::makeError(core::ErrorCode::invalidArgument,
                           "Shared strategy source size is outside its limit.");
  }
#if defined(__unix__) || defined(__APPLE__)
  std::string sharedName;
  UniqueDescriptor writableDescriptor;
  try {
    std::random_device entropy;
    for (int attempt = 0; attempt < 8 && writableDescriptor.get() < 0;
         ++attempt) {
      const auto nonce =
          (static_cast<std::uint64_t>(entropy()) << 32U) | entropy();
      std::array<char, 16> nonceBuffer{};
      const auto encodedNonce =
          std::to_chars(nonceBuffer.data(),
                        nonceBuffer.data() + nonceBuffer.size(), nonce, 16);
      if (encodedNonce.ec != std::errc{}) {
        break;
      }
      sharedName =
          "/bte-" + std::string{nonceBuffer.data(),
                                static_cast<std::size_t>(encodedNonce.ptr -
                                                         nonceBuffer.data())};
      writableDescriptor.reset(
          ::shm_open( // NOLINT(cppcoreguidelines-pro-type-vararg): POSIX
                      // shm_open requires this mode because O_CREAT is set.
              sharedName.c_str(), O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC,
              S_IRUSR | S_IWUSR));
      if (writableDescriptor.get() < 0 && errno != EEXIST) {
        break;
      }
    }
  } catch (const std::exception &) {
    return workerStartError(
        "Could not create an unpredictable shared-memory name.");
  }
  if (writableDescriptor.get() < 0) {
    return workerStartError(
        "Could not create a shared-memory object for strategy source: " +
        std::error_code{errno, std::generic_category()}.message());
  }
  const auto removeName = [&sharedName]() {
    if (!sharedName.empty()) {
      static_cast<void>(::shm_unlink(sharedName.c_str()));
    }
  };
  if (::ftruncate(writableDescriptor.get(), static_cast<off_t>(bytes.size())) !=
      0) {
    removeName();
    return workerStartError(
        "Could not size shared memory for strategy source.");
  }
  auto *writableMapping = ::mmap(nullptr, bytes.size(), PROT_READ | PROT_WRITE,
                                 MAP_SHARED, writableDescriptor.get(), 0);
  if (writableMapping == MAP_FAILED) {
    removeName();
    return workerStartError("Could not map shared memory for strategy source.");
  }
  std::memcpy(writableMapping, bytes.data(), bytes.size());
  const auto unmapResult = ::munmap(writableMapping, bytes.size());
  if (unmapResult != 0) {
    removeName();
    return workerStartError(
        "Could not seal the shared strategy source mapping.");
  }

  UniqueDescriptor readOnlyDescriptor(
      ::shm_open(sharedName.c_str(), O_RDONLY | O_CLOEXEC, 0));
  removeName();
  if (readOnlyDescriptor.get() < 0) {
    return workerStartError(
        "Could not reopen strategy source as read-only memory.");
  }
  struct stat status {};
  if (::fstat(readOnlyDescriptor.get(), &status) != 0 || status.st_size < 0 ||
      static_cast<std::uint64_t>(status.st_size) < bytes.size()) {
    return workerStartError(
        "Read-only strategy memory has an unexpected size (actual " +
        std::to_string(status.st_size) + ", expected " +
        std::to_string(bytes.size()) + ").");
  }
  return ReadOnlySharedMemory(readOnlyDescriptor.release(), core::sha256(bytes),
                              bytes.size());
#else
  static_cast<void>(bytes);
  return unsupportedProcessError();
#endif
}

int ReadOnlySharedMemory::descriptor() const noexcept { return descriptor_; }

std::uint64_t ReadOnlySharedMemory::size() const noexcept { return size_; }

const std::string &ReadOnlySharedMemory::sha256Digest() const noexcept {
  return digest_;
}

void ReadOnlySharedMemory::reset() noexcept {
#if defined(__unix__) || defined(__APPLE__)
  if (descriptor_ >= 0) {
    static_cast<void>(::close(descriptor_));
  }
#endif
  descriptor_ = -1;
  size_ = 0;
  digest_.clear();
}

struct ManagedWorkerProcess::Impl final {
#if defined(__unix__) || defined(__APPLE__)
  pid_t process = -1;
  pid_t processGroup = -1;
  UniqueDescriptor protocolInput;
  UniqueDescriptor standardOutput;
  UniqueDescriptor standardError;
  UniqueDescriptor protocolOutput;
  bool standardOutputOpen = true;
  bool standardErrorOpen = true;
  bool reaped = false;
  int exitStatus = 0;
  DiagnosticBuffer capturedStandardOutput;
  DiagnosticBuffer capturedStandardError;

  ~Impl() noexcept {
    try {
      static_cast<void>(terminate(std::chrono::milliseconds{100}));
    } catch (...) {
      if (processGroup > 0) {
        static_cast<void>(::kill(-processGroup, SIGKILL));
      }
    }
  }

  Impl() = default;
  Impl(const Impl &) = delete;
  Impl &operator=(const Impl &) = delete;
  Impl(Impl &&) = delete;
  Impl &operator=(Impl &&) = delete;

  [[nodiscard]] bool refreshChildStatus() {
    if (process <= 0 || reaped) {
      return reaped;
    }
    const auto result = ::waitpid(process, &exitStatus, WNOHANG);
    if (result == process) {
      reaped = true;
      process = -1;
    }
    return reaped;
  }

  void closeStandardOutput() {
    standardOutput.reset();
    standardOutputOpen = false;
  }

  void closeStandardError() {
    standardError.reset();
    standardErrorOpen = false;
  }

  static void drainPipe(UniqueDescriptor &descriptor, bool &open,
                        DiagnosticBuffer &capture) {
    if (!open || descriptor.get() < 0) {
      return;
    }
    std::array<char, 8192> bytes{};
    for (int iteration = 0; iteration < 16; ++iteration) {
      const auto count = ::read(descriptor.get(), bytes.data(), bytes.size());
      if (count > 0) {
        capture.append(bytes.data(), static_cast<std::size_t>(count));
        continue;
      }
      if (count == 0) {
        descriptor.reset();
        open = false;
        return;
      }
      if (errno == EINTR) {
        --iteration;
        continue;
      }
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        return;
      }
      descriptor.reset();
      open = false;
      return;
    }
  }

  [[nodiscard]] core::Result<void>
  drainReadyDiagnostics(const DiagnosticEvents events) {
    if ((events.standardOutput & (POLLIN | POLLHUP | POLLERR | POLLNVAL)) !=
        0) {
      drainPipe(standardOutput, standardOutputOpen, capturedStandardOutput);
    }
    if ((events.standardError & (POLLIN | POLLHUP | POLLERR | POLLNVAL)) != 0) {
      drainPipe(standardError, standardErrorOpen, capturedStandardError);
    }
    return {};
  }

  [[nodiscard]] core::Result<void> drainUnexpectedProtocolOutput() {
    if (protocolOutput.get() < 0) {
      return {};
    }
    std::array<std::byte, 8192> discarded{};
    bool unexpectedBytes = false;
    while (true) {
      const auto count =
          ::read(protocolOutput.get(), discarded.data(), discarded.size());
      if (count > 0) {
        unexpectedBytes = true;
        continue;
      }
      if (count < 0 && errno == EINTR) {
        continue;
      }
      if (count == 0) {
        protocolOutput.reset();
      }
      break;
    }
    if (unexpectedBytes) {
      return core::makeError(
          core::ErrorCode::strategyProtocolViolation,
          "Worker wrote an unexpected protocol message while exiting.");
    }
    return {};
  }

  [[nodiscard]] core::Result<void> drainAfterExit() {
    drainPipe(standardOutput, standardOutputOpen, capturedStandardOutput);
    drainPipe(standardError, standardErrorOpen, capturedStandardError);
    return drainUnexpectedProtocolOutput();
  }

  [[nodiscard]] core::Result<short>
  waitForDescriptor(const int descriptor, const short requestedEvents,
                    const std::chrono::steady_clock::time_point deadline,
                    const core::ErrorCode deadlineCode,
                    const core::CancellationToken &cancellation) {
    while (true) {
      if (cancellation.isCancellationRequested()) {
        return core::makeError(
            core::ErrorCode::cancelled,
            "Managed strategy worker operation was cancelled.");
      }
      const auto now = std::chrono::steady_clock::now();
      if (now >= deadline) {
        return timeoutError(deadlineCode);
      }
      auto remaining =
          std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
      if (remaining.count() == 0) {
        remaining = std::chrono::milliseconds{1};
      }
      const auto boundedWait =
          static_cast<int>(std::min<std::int64_t>(remaining.count(), 20));
      std::array<pollfd, 3> descriptors{{
          pollfd{.fd = descriptor, .events = requestedEvents, .revents = 0},
          pollfd{.fd = standardOutputOpen ? standardOutput.get() : -1,
                 .events = POLLIN,
                 .revents = 0},
          pollfd{.fd = standardErrorOpen ? standardError.get() : -1,
                 .events = POLLIN,
                 .revents = 0},
      }};
      const auto pollResult =
          ::poll(descriptors.data(), descriptors.size(), boundedWait);
      if (pollResult < 0) {
        if (errno == EINTR) {
          continue;
        }
        return workerIoError("Could not poll managed worker pipes.");
      }
      const auto diagnostics = drainReadyDiagnostics(DiagnosticEvents{
          .standardOutput = descriptors[1].revents,
          .standardError = descriptors[2].revents,
      });
      if (!diagnostics.ok()) {
        return diagnostics.error();
      }
      if (descriptors[0].revents != 0) {
        return descriptors[0].revents;
      }
      static_cast<void>(refreshChildStatus());
    }
  }

  [[nodiscard]] core::Result<std::size_t>
  readSome(const std::span<std::byte> destination,
           const std::chrono::steady_clock::time_point deadline,
           const core::ErrorCode deadlineCode,
           const core::CancellationToken &cancellation) {
    while (true) {
      const auto events = waitForDescriptor(
          protocolOutput.get(), POLLIN, deadline, deadlineCode, cancellation);
      if (!events.ok()) {
        return events.error();
      }
      const auto count =
          ::read(protocolOutput.get(), destination.data(), destination.size());
      if (count > 0) {
        return static_cast<std::size_t>(count);
      }
      if (count == 0) {
        return std::size_t{0};
      }
      if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
        continue;
      }
      return workerIoError("Could not read a managed worker protocol frame.");
    }
  }

  [[nodiscard]] core::Result<std::size_t>
  writeSome(const std::span<const std::byte> source,
            const std::chrono::steady_clock::time_point deadline,
            const core::ErrorCode deadlineCode,
            const core::CancellationToken &cancellation) {
    while (true) {
      const auto events = waitForDescriptor(
          protocolInput.get(), POLLOUT, deadline, deadlineCode, cancellation);
      if (!events.ok()) {
        return events.error();
      }
      const auto count = writeWithoutSigpipe(protocolInput.get(), source.data(),
                                             source.size());
      if (count > 0) {
        return static_cast<std::size_t>(count);
      }
      if (count < 0 &&
          (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) {
        continue;
      }
      return workerIoError("Could not write a managed worker protocol frame.");
    }
  }

  [[nodiscard]] core::Result<void>
  terminate(const std::chrono::milliseconds gracePeriod) {
    if (processGroup <= 0) {
      return {};
    }
    if (::kill(-processGroup, SIGTERM) != 0 && errno != ESRCH) {
      return workerIoError(
          "Could not request managed worker process-tree shutdown.");
    }
    const auto deadline = std::chrono::steady_clock::now() +
                          std::max(gracePeriod, std::chrono::milliseconds{0});
    while (std::chrono::steady_clock::now() < deadline &&
           processGroupExists(processGroup)) {
      static_cast<void>(refreshChildStatus());
      std::array<pollfd, 2> diagnostics{{
          pollfd{.fd = standardOutputOpen ? standardOutput.get() : -1,
                 .events = POLLIN,
                 .revents = 0},
          pollfd{.fd = standardErrorOpen ? standardError.get() : -1,
                 .events = POLLIN,
                 .revents = 0},
      }};
      static_cast<void>(::poll(diagnostics.data(), diagnostics.size(), 10));
      static_cast<void>(drainReadyDiagnostics(DiagnosticEvents{
          .standardOutput = diagnostics[0].revents,
          .standardError = diagnostics[1].revents,
      }));
    }
    if (processGroupExists(processGroup)) {
      if (::kill(-processGroup, SIGKILL) != 0 && errno != ESRCH) {
        return workerIoError(
            "Could not terminate the managed worker process tree.");
      }
    }
    if (process > 0 && !reaped) {
      int status = 0;
      while (::waitpid(process, &status, 0) < 0 && errno == EINTR) {
      }
      process = -1;
      reaped = true;
      exitStatus = status;
    }
    processGroup = -1;
    drainPipe(standardOutput, standardOutputOpen, capturedStandardOutput);
    drainPipe(standardError, standardErrorOpen, capturedStandardError);
    return {};
  }
#else
  [[nodiscard]] core::Result<void> terminate(std::chrono::milliseconds) {
    return unsupportedProcessError();
  }
#endif
};

ManagedWorkerProcess::ManagedWorkerProcess() noexcept = default;

ManagedWorkerProcess::ManagedWorkerProcess(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}

ManagedWorkerProcess::~ManagedWorkerProcess() = default;

ManagedWorkerProcess::ManagedWorkerProcess(
    ManagedWorkerProcess &&other) noexcept = default;

ManagedWorkerProcess &ManagedWorkerProcess::operator=(
    ManagedWorkerProcess &&other) noexcept = default;

namespace {

[[nodiscard]] core::Result<std::filesystem::path>
validateWorkerExecutable(const std::filesystem::path &path) {
  if (path.empty()) {
    return workerStartError("Managed worker executable path is empty.");
  }
  std::error_code filesystemError;
  const auto executable = std::filesystem::canonical(path, filesystemError);
  if (filesystemError ||
      !std::filesystem::is_regular_file(executable, filesystemError)) {
    return workerStartError("Managed worker executable is missing or invalid.");
  }
  const auto executablePermissions =
      std::filesystem::status(executable, filesystemError).permissions();
  if (filesystemError ||
      (executablePermissions & (std::filesystem::perms::owner_exec |
                                std::filesystem::perms::group_exec |
                                std::filesystem::perms::others_exec)) ==
          std::filesystem::perms::none) {
    return workerStartError("Managed worker executable is not executable.");
  }
  return executable;
}

[[nodiscard]] core::Result<void>
validateWorkerArguments(const WorkerProcessLaunch &launch,
                        const std::filesystem::path &executable) {
  constexpr auto maximumArgumentBytes = std::size_t{64} * std::size_t{1024};
  std::size_t argumentBytes = executable.string().size();
  for (const auto &argument : launch.arguments) {
    if (hasEmbeddedNull(argument) || argument.size() > maximumArgumentBytes ||
        argumentBytes > maximumArgumentBytes - argument.size()) {
      return workerStartError(
          "Managed worker arguments are malformed or too large.");
    }
    argumentBytes += argument.size();
  }
  return {};
}

[[nodiscard]] core::Result<void>
validateWorkerDataDescriptor(const WorkerProcessLaunch &launch) {
  if (launch.readOnlyDataDescriptor >= 0) {
    if (!isWritableDescriptor(launch.readOnlyDataDescriptor) ||
        launch.readOnlyDataLength == 0 ||
        launch.readOnlyDataLength > maximumWorkerSourceBytes ||
        launch.readOnlyDataSha256.size() != 64) {
      return workerStartError(
          "Worker shared-data descriptor is invalid or writable.");
    }
    struct stat sharedStatus {};
    if (::fstat(launch.readOnlyDataDescriptor, &sharedStatus) != 0 ||
        sharedStatus.st_size < 0 ||
        static_cast<std::uint64_t>(sharedStatus.st_size) <
            launch.readOnlyDataLength) {
      return workerStartError(
          "Worker shared-data descriptor size does not match.");
    }
  } else if (launch.readOnlyDataLength != 0 ||
             !launch.readOnlyDataSha256.empty()) {
    return workerStartError("Worker shared-data metadata has no descriptor.");
  }
  return {};
}

[[nodiscard]] core::Result<void>
validateWorkerHistoryDescriptor(const WorkerProcessLaunch &launch) {
  if (launch.readOnlyHistoryDescriptor >= 0) {
    if (!isWritableDescriptor(launch.readOnlyHistoryDescriptor) ||
        launch.readOnlyHistoryLength == 0 ||
        launch.readOnlyHistoryLength > maximumWorkerSourceBytes ||
        launch.readOnlyHistorySha256.size() != 64) {
      return workerStartError(
          "Worker shared-history descriptor is invalid or writable.");
    }
    struct stat historyStatus {};
    if (::fstat(launch.readOnlyHistoryDescriptor, &historyStatus) != 0 ||
        historyStatus.st_size < 0 ||
        static_cast<std::uint64_t>(historyStatus.st_size) <
            launch.readOnlyHistoryLength) {
      return workerStartError(
          "Worker shared-history descriptor size does not match.");
    }
  } else if (launch.readOnlyHistoryLength != 0 ||
             !launch.readOnlyHistorySha256.empty()) {
    return workerStartError(
        "Worker shared-history metadata has no descriptor.");
  }
  return {};
}

struct WorkerArguments {
  std::vector<std::string> argumentStorage;
  std::vector<char *> argumentVector;
  std::array<std::string, 6> environmentStorage{"PATH=/usr/bin:/bin",
                                                "LANG=C",
                                                "TZ=UTC",
                                                "PYTHONNOUSERSITE=1",
                                                "PYTHONDONTWRITEBYTECODE=1",
                                                "PYTHONSAFEPATH=1"};
  std::array<char *, 7> environmentVector{};

  void buildVectors() {
    argumentVector.reserve(argumentStorage.size() + 1);
    for (auto &argument : argumentStorage) {
      argumentVector.emplace_back(argument.data());
    }
    argumentVector.emplace_back(nullptr);
    for (std::size_t index = 0; index < environmentStorage.size(); ++index) {
      environmentVector[index] = environmentStorage[index].data();
    }
  }
};

struct WorkerPipeSet {
  std::array<std::array<int, 2>, 4> descriptors{};
  std::array<UniqueDescriptor, 8> owners;
};

void closeWorkerPipes(const std::array<std::array<int, 2>, 4> &pipes) {
  for (const auto &created : pipes) {
    if (created[0] >= 0) {
      static_cast<void>(::close(created[0]));
    }
    if (created[1] >= 0) {
      static_cast<void>(::close(created[1]));
    }
  }
}

[[nodiscard]] core::Result<WorkerPipeSet> createWorkerPipes() {
  WorkerPipeSet workerPipes;
  for (auto &pipe : workerPipes.descriptors) {
    pipe = {-1, -1};
  }
  for (auto &pipe : workerPipes.descriptors) {
    if (::pipe(pipe.data()) != 0) {
      closeWorkerPipes(workerPipes.descriptors);
      return workerStartError("Could not create managed worker pipes.");
    }
    if (setCloseOnExec(pipe[0]) != 0 || setCloseOnExec(pipe[1]) != 0) {
      closeWorkerPipes(workerPipes.descriptors);
      return workerStartError(
          "Could not isolate managed worker pipe descriptors.");
    }
  }
  auto *owner = workerPipes.owners.data();
  for (const auto &pipe : workerPipes.descriptors) {
    owner->reset(pipe[0]);
    ++owner;
    owner->reset(pipe[1]);
    ++owner;
  }
  return workerPipes;
}

struct WorkerDuplicates {
  std::array<UniqueDescriptor, 4> children;
  UniqueDescriptor sharedData;
  UniqueDescriptor sharedHistory;
};

[[nodiscard]] core::Result<WorkerDuplicates>
duplicateWorkerDescriptors(const WorkerProcessLaunch &launch,
                           const std::array<int, 4> &childDescriptors) {
  WorkerDuplicates duplicates;
  auto *childDuplicate = duplicates.children.data();
  for (const auto descriptor : childDescriptors) {
    const auto duplicate = duplicateCloseOnExec(descriptor);
    if (duplicate < 0) {
      return workerStartError("Could not prepare worker protocol descriptors.");
    }
    childDuplicate->reset(duplicate);
    ++childDuplicate;
  }
  if (launch.readOnlyDataDescriptor >= 0) {
    const auto duplicate = duplicateCloseOnExec(launch.readOnlyDataDescriptor);
    if (duplicate < 0) {
      return workerStartError(
          "Could not prepare the read-only shared-data descriptor.");
    }
    duplicates.sharedData.reset(duplicate);
  }
  if (launch.readOnlyHistoryDescriptor >= 0) {
    const auto duplicate =
        duplicateCloseOnExec(launch.readOnlyHistoryDescriptor);
    if (duplicate < 0) {
      return workerStartError(
          "Could not prepare the read-only shared-history descriptor.");
    }
    duplicates.sharedHistory.reset(duplicate);
  }
  return duplicates;
}

[[nodiscard]] core::Result<void> addInheritedWorkerDescriptorClosures(
    const WorkerProcessLaunch &launch, const WorkerPipeSet &workerPipes,
    const WorkerDuplicates &duplicates, SpawnActions &actions) {
  for (const auto &pipe : workerPipes.descriptors) {
    for (const auto descriptor : pipe) {
      if (descriptor > 4 &&
          (duplicates.sharedHistory.get() < 0 || descriptor != 5) &&
          actions.addClose(descriptor) != 0) {
        return workerStartError(
            "Could not close inherited parent pipe descriptors.");
      }
    }
  }
  if (launch.readOnlyDataDescriptor > 4 &&
      (duplicates.sharedHistory.get() < 0 ||
       launch.readOnlyDataDescriptor != 5) &&
      actions.addClose(launch.readOnlyDataDescriptor) != 0) {
    return workerStartError("Could not close the inherited source descriptor.");
  }
  if (launch.readOnlyHistoryDescriptor > 5 &&
      actions.addClose(launch.readOnlyHistoryDescriptor) != 0) {
    return workerStartError(
        "Could not close the inherited history descriptor.");
  }
  return {};
}

[[nodiscard]] core::Result<void> configureWorkerSpawnActions(
    const WorkerProcessLaunch &launch, const WorkerPipeSet &workerPipes,
    const WorkerDuplicates &duplicates, SpawnActions &actions,
    SpawnAttributes &attributes) {
  if (!actions.valid() || !attributes.valid()) {
    return workerStartError(
        "Could not initialize managed worker spawn actions.");
  }
  int targetDescriptor = 0;
  for (const auto &duplicate : duplicates.children) {
    if (actions.addDup2(duplicate.get(), targetDescriptor) != 0) {
      return workerStartError("Could not route managed worker protocol pipes.");
    }
    ++targetDescriptor;
  }
  if (duplicates.sharedData.get() >= 0 &&
      actions.addDup2(duplicates.sharedData.get(), 4) != 0) {
    return workerStartError(
        "Could not route the read-only shared-data descriptor.");
  }
  if (duplicates.sharedHistory.get() >= 0 &&
      actions.addDup2(duplicates.sharedHistory.get(), 5) != 0) {
    return workerStartError(
        "Could not route the read-only shared-history descriptor.");
  }
  for (const auto &duplicate : duplicates.children) {
    if (actions.addClose(duplicate.get()) != 0) {
      return workerStartError(
          "Could not close temporary worker descriptors after spawn.");
    }
  }
  if (duplicates.sharedData.get() >= 0 &&
      actions.addClose(duplicates.sharedData.get()) != 0) {
    return workerStartError(
        "Could not close the temporary shared-data descriptor.");
  }
  if (duplicates.sharedHistory.get() >= 0 &&
      actions.addClose(duplicates.sharedHistory.get()) != 0) {
    return workerStartError(
        "Could not close the temporary shared-history descriptor.");
  }
  const auto inheritedClosuresResult = addInheritedWorkerDescriptorClosures(
      launch, workerPipes, duplicates, actions);
  if (!inheritedClosuresResult.ok()) {
    return inheritedClosuresResult.error();
  }
  if (attributes.configureProcessGroup() != 0) {
    return workerStartError(
        "Could not configure managed worker process ownership.");
  }
  return {};
}

[[nodiscard]] core::Result<WorkerArguments>
buildWorkerArguments(const WorkerProcessLaunch &launch,
                     const std::filesystem::path &executable) {
  WorkerArguments arguments;
  arguments.argumentStorage.reserve(launch.arguments.size() + 8);
  arguments.argumentStorage.emplace_back(executable.string());
  arguments.argumentStorage.insert(arguments.argumentStorage.end(),
                                   launch.arguments.begin(),
                                   launch.arguments.end());
  if (launch.readOnlyDataDescriptor >= 0) {
    arguments.argumentStorage.emplace_back("--shared-data-fd=4");
    arguments.argumentStorage.emplace_back(
        "--shared-data-length=" + std::to_string(launch.readOnlyDataLength));
    arguments.argumentStorage.emplace_back("--shared-data-sha256=" +
                                           launch.readOnlyDataSha256);
  }
  if (launch.readOnlyHistoryDescriptor >= 0) {
    arguments.argumentStorage.emplace_back("--history-data-fd=5");
    arguments.argumentStorage.emplace_back(
        "--history-data-length=" +
        std::to_string(launch.readOnlyHistoryLength));
    arguments.argumentStorage.emplace_back("--history-data-sha256=" +
                                           launch.readOnlyHistorySha256);
  }
  if (!launch.timezoneDatabasePath.empty()) {
    const auto timezonePath = launch.timezoneDatabasePath.string();
    if (hasEmbeddedNull(timezonePath)) {
      return workerStartError("Worker timezone database path is malformed.");
    }
    arguments.argumentStorage.emplace_back("--timezone-data-path=" +
                                           timezonePath);
  }
  return arguments;
}

} // namespace

core::Result<ManagedWorkerProcess>
ManagedWorkerProcess::start(const WorkerProcessLaunch &launch) {
#if defined(__unix__) || defined(__APPLE__)
  auto executableResult = validateWorkerExecutable(launch.executable);
  if (!executableResult.ok()) {
    return executableResult.error();
  }
  const auto executable = std::move(executableResult).value();
  const auto validArguments = validateWorkerArguments(launch, executable);
  if (!validArguments.ok()) {
    return validArguments.error();
  }
  const auto validDataDescriptor = validateWorkerDataDescriptor(launch);
  if (!validDataDescriptor.ok()) {
    return validDataDescriptor.error();
  }
  const auto validHistoryDescriptor = validateWorkerHistoryDescriptor(launch);
  if (!validHistoryDescriptor.ok()) {
    return validHistoryDescriptor.error();
  }

  auto workerPipesResult = createWorkerPipes();
  if (!workerPipesResult.ok()) {
    return workerPipesResult.error();
  }
  auto workerPipes = std::move(workerPipesResult).value();
  const auto &pipes = workerPipes.descriptors;
  auto &pipeOwners = workerPipes.owners;

  const std::array<int, 4> childDescriptors{
      pipes[0][0], // protocol input -> stdin
      pipes[1][1], // stdout diagnostics
      pipes[2][1], // stderr diagnostics
      pipes[3][1], // protocol output -> descriptor 3
  };
  auto duplicatesResult = duplicateWorkerDescriptors(launch, childDescriptors);
  if (!duplicatesResult.ok()) {
    return duplicatesResult.error();
  }
  auto duplicates = std::move(duplicatesResult).value();
  SpawnActions actions;
  SpawnAttributes attributes;
  const auto spawnActionsResult = configureWorkerSpawnActions(
      launch, workerPipes, duplicates, actions, attributes);
  if (!spawnActionsResult.ok()) {
    return spawnActionsResult.error();
  }

  auto workerArgumentsResult = buildWorkerArguments(launch, executable);
  if (!workerArgumentsResult.ok()) {
    return workerArgumentsResult.error();
  }
  auto workerArguments = std::move(workerArgumentsResult).value();
  workerArguments.buildVectors();

  pid_t process = -1;
  const auto spawnStatus =
      ::posix_spawn(&process, executable.c_str(), actions.get(),
                    attributes.get(), workerArguments.argumentVector.data(),
                    workerArguments.environmentVector.data());
  if (spawnStatus != 0) {
    return workerStartError("Could not launch the managed worker executable.");
  }

#if defined(BTE_ENABLE_TEST_HOOKS)
  workerProcessStartCountStorage().fetch_add(1, std::memory_order_relaxed);
#endif

  auto impl = std::make_unique<Impl>();
  impl->process = process;
  impl->processGroup = process;
  impl->protocolInput.reset(pipes[0][1]);
  static_cast<void>(pipeOwners[1].release());
  impl->standardOutput.reset(pipes[1][0]);
  static_cast<void>(pipeOwners[2].release());
  impl->standardError.reset(pipes[2][0]);
  static_cast<void>(pipeOwners[4].release());
  impl->protocolOutput.reset(pipes[3][0]);
  static_cast<void>(pipeOwners[6].release());
  for (const auto descriptor :
       {impl->protocolInput.get(), impl->standardOutput.get(),
        impl->standardError.get(), impl->protocolOutput.get()}) {
    if (setNonBlocking(descriptor) != 0) {
      static_cast<void>(impl->terminate(std::chrono::milliseconds{0}));
      return workerStartError(
          "Could not configure managed worker pipes as nonblocking.");
    }
  }
  return ManagedWorkerProcess{std::move(impl)};
#else
  static_cast<void>(launch);
  return unsupportedProcessError();
#endif
}

core::Result<void>
ManagedWorkerProcess::sendMessage(const ProtocolMessage &message,
                                  const std::chrono::milliseconds timeout,
                                  const core::ErrorCode timeoutCode,
                                  const core::CancellationToken &cancellation) {
#if defined(__unix__) || defined(__APPLE__)
  if (!impl_ || impl_->processGroup <= 0) {
    return workerIoError("Managed worker process is not running.");
  }
  if (impl_->refreshChildStatus()) {
    return workerIoError("Managed worker process already exited.");
  }
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  const ProtocolWriteSome writeSome =
      [this, deadline, timeoutCode,
       &cancellation](const std::span<const std::byte> bytes) {
        return impl_->writeSome(bytes, deadline, timeoutCode, cancellation);
      };
  return writeMessage(writeSome, message);
#else
  static_cast<void>(message);
  static_cast<void>(timeout);
  static_cast<void>(timeoutCode);
  static_cast<void>(cancellation);
  return unsupportedProcessError();
#endif
}

core::Result<ProtocolMessage> ManagedWorkerProcess::receiveMessage(
    const std::string_view expectedRunId, const std::uint64_t expectedSequence,
    const std::chrono::milliseconds timeout, const core::ErrorCode timeoutCode,
    const core::CancellationToken &cancellation) {
#if defined(__unix__) || defined(__APPLE__)
  if (!impl_ || impl_->processGroup <= 0) {
    return workerIoError("Managed worker process is not running.");
  }
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  const ProtocolReadSome readSome =
      [this, deadline, timeoutCode,
       &cancellation](const std::span<std::byte> bytes) {
        return impl_->readSome(bytes, deadline, timeoutCode, cancellation);
      };
  return readMessage(readSome, expectedRunId, expectedSequence);
#else
  static_cast<void>(expectedRunId);
  static_cast<void>(expectedSequence);
  static_cast<void>(timeout);
  static_cast<void>(timeoutCode);
  static_cast<void>(cancellation);
  return unsupportedProcessError();
#endif
}

core::Result<void>
ManagedWorkerProcess::waitForExit(const std::chrono::milliseconds timeout) {
#if defined(__unix__) || defined(__APPLE__)
  if (!impl_) {
    return {};
  }
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (impl_->refreshChildStatus()) {
      return impl_->drainAfterExit();
    }
    std::array<pollfd, 3> descriptors{{
        pollfd{.fd =
                   impl_->standardOutputOpen ? impl_->standardOutput.get() : -1,
               .events = POLLIN,
               .revents = 0},
        pollfd{.fd = impl_->standardErrorOpen ? impl_->standardError.get() : -1,
               .events = POLLIN,
               .revents = 0},
        pollfd{
            .fd = impl_->protocolOutput.get(), .events = POLLIN, .revents = 0},
    }};
    static_cast<void>(::poll(descriptors.data(), descriptors.size(), 10));
    static_cast<void>(impl_->drainReadyDiagnostics(DiagnosticEvents{
        .standardOutput = descriptors[0].revents,
        .standardError = descriptors[1].revents,
    }));
    if ((descriptors[2].revents & (POLLIN | POLLHUP)) != 0) {
      const auto protocolDrain = impl_->drainUnexpectedProtocolOutput();
      if (!protocolDrain.ok()) {
        return protocolDrain.error();
      }
    }
  }
  if (impl_->refreshChildStatus()) {
    return impl_->drainAfterExit();
  }
  return timeoutError(core::ErrorCode::timeout);
#else
  static_cast<void>(timeout);
  return unsupportedProcessError();
#endif
}

core::Result<void> ManagedWorkerProcess::terminateProcessTree(
    const std::chrono::milliseconds gracePeriod) {
#if defined(__unix__) || defined(__APPLE__)
  return impl_ ? impl_->terminate(gracePeriod) : core::Result<void>{};
#else
  static_cast<void>(gracePeriod);
  return unsupportedProcessError();
#endif
}

bool ManagedWorkerProcess::isRunning() const noexcept {
#if defined(__unix__) || defined(__APPLE__)
  return impl_ && impl_->processGroup > 0 && !impl_->reaped;
#else
  return false;
#endif
}

int ManagedWorkerProcess::processId() const noexcept {
#if defined(__unix__) || defined(__APPLE__)
  return impl_ ? static_cast<int>(impl_->processGroup) : -1;
#else
  return -1;
#endif
}

WorkerDiagnostics ManagedWorkerProcess::diagnostics() const {
  WorkerDiagnostics result;
#if defined(__unix__) || defined(__APPLE__)
  if (impl_) {
    result.standardOutput = impl_->capturedStandardOutput.snapshot();
    result.standardError = impl_->capturedStandardError.snapshot();
  }
#endif
  return result;
}

} // namespace bte::bindings::python
