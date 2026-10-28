#include "ManagedWorkerProcess.h"
#include "ManagedWorkerProcessTestHooks.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace {

using namespace std::chrono_literals;
using bte::bindings::python::ManagedWorkerProcess;
using bte::bindings::python::ProtocolMessage;
using bte::bindings::python::ReadOnlySharedMemory;
using bte::bindings::python::WorkerProcessLaunch;

#if defined(__unix__) || defined(__APPLE__)
[[nodiscard]] std::filesystem::path fixturePath() {
  return std::filesystem::path{BTE_WORKER_FIXTURE_PATH};
}

[[nodiscard]] ProtocolMessage request() {
  return ProtocolMessage{.protocolVersion = 1,
                         .runId = "fixture-run",
                         .sequence = 0,
                         .messageType = "start",
                         .payload = nlohmann::json::object()};
}

class EnvironmentVariable final {
public:
  explicit EnvironmentVariable(const char *key) : key_(key) {
    const char *value = std::getenv(key_.c_str());
    if (value != nullptr) {
      original_ = value;
    }
  }
  ~EnvironmentVariable() {
    if (original_.has_value()) {
      static_cast<void>(::setenv(key_.c_str(), original_->c_str(), 1));
    } else {
      static_cast<void>(::unsetenv(key_.c_str()));
    }
  }

private:
  std::string key_;
  std::optional<std::string> original_;
};

class TemporaryDirectory final {
public:
  TemporaryDirectory() {
    const auto stamp =
        std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = std::filesystem::temp_directory_path() /
            ("bte-managed-worker-test-" + std::to_string(stamp));
    std::filesystem::create_directories(path_);
  }

  ~TemporaryDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }

  [[nodiscard]] const std::filesystem::path &path() const noexcept {
    return path_;
  }

private:
  std::filesystem::path path_;
};
#endif

TEST(ManagedWorkerProcessTest,
     directlySpawnsWithSanitizedEnvironmentAndBoundsDiagnostics) {
#if defined(__unix__) || defined(__APPLE__)
  EnvironmentVariable secret("BTE_TEST_PRIVATE_SECRET");
  ASSERT_EQ(::setenv("BTE_TEST_PRIVATE_SECRET", "must-not-inherit", 1), 0);
  auto started = ManagedWorkerProcess::start(WorkerProcessLaunch{
      .executable = fixturePath(), .arguments = {"echo", "300000"}});
  ASSERT_TRUE(started.ok()) << started.error().message;
  auto worker = std::move(started).value();

  ASSERT_TRUE(worker
                  .sendMessage(request(), 2s,
                               bte::core::ErrorCode::workerHandshakeTimeout)
                  .ok());
  const auto response = worker.receiveMessage(
      "fixture-run", 0, 2s, bte::core::ErrorCode::workerHandshakeTimeout);
  ASSERT_TRUE(response.ok())
      << response.error().message << "\n"
      << worker.diagnostics().standardError.retainedBytes;
  EXPECT_EQ(response.value().messageType, "ready");
  EXPECT_TRUE(response.value().payload.at("secretAbsent").get<bool>());

  const auto diagnostics = worker.diagnostics();
  EXPECT_GT(diagnostics.standardOutput.bytesSeen,
            bte::bindings::python::maximumWorkerDiagnosticBytes);
  EXPECT_EQ(diagnostics.standardOutput.retainedBytes.size(),
            bte::bindings::python::maximumWorkerDiagnosticBytes);
  EXPECT_EQ(diagnostics.standardOutput.omittedBytes,
            diagnostics.standardOutput.bytesSeen -
                diagnostics.standardOutput.retainedBytes.size());
  EXPECT_EQ(diagnostics.standardError.retainedBytes.size(),
            bte::bindings::python::maximumWorkerDiagnosticBytes);
  EXPECT_TRUE(
      diagnostics.standardOutput.retainedBytes.starts_with("stdout-first\n"));
  EXPECT_NE(diagnostics.standardOutput.retainedBytes.rfind("stdout-last\n"),
            std::string::npos);
  EXPECT_TRUE(
      diagnostics.standardError.retainedBytes.starts_with("stderr-first\n"));
  EXPECT_NE(diagnostics.standardError.retainedBytes.rfind("stderr-last\n"),
            std::string::npos);
  EXPECT_TRUE(worker.waitForExit(2s).ok());
#else
  GTEST_SKIP()
      << "POSIX worker process fixture is unavailable on this platform.";
#endif
}

TEST(ManagedWorkerProcessTest,
     rejectsAnIndividualArgumentAbove64KiBBeforeStartingAWorker) {
#if defined(__unix__) || defined(__APPLE__)
  const auto startCountBefore =
      bte::bindings::python::testing::managedWorkerProcessStartCount();
  const auto oversizedArgument = std::string(64U * 1024U + 1U, 'x');
  const auto started = ManagedWorkerProcess::start(WorkerProcessLaunch{
      .executable = fixturePath(), .arguments = {oversizedArgument}});

  ASSERT_FALSE(started.ok());
  EXPECT_EQ(started.error().message,
            "Managed worker arguments are malformed or too large.");
  EXPECT_EQ(bte::bindings::python::testing::managedWorkerProcessStartCount(),
            startCountBefore);
#else
  GTEST_SKIP()
      << "POSIX worker process fixture is unavailable on this platform.";
#endif
}

TEST(ManagedWorkerProcessTest,
     rejectsInvalidExecutablesArgumentsAndSharedDataMetadata) {
#if defined(__unix__) || defined(__APPLE__)
  const auto emptyPath = ManagedWorkerProcess::start(WorkerProcessLaunch{});
  ASSERT_FALSE(emptyPath.ok());
  EXPECT_EQ(emptyPath.error().code, bte::core::ErrorCode::workerStartFailed);

  const auto missing = ManagedWorkerProcess::start(WorkerProcessLaunch{
      .executable = fixturePath().parent_path() / "missing-worker"});
  ASSERT_FALSE(missing.ok());
  EXPECT_EQ(missing.error().code, bte::core::ErrorCode::workerStartFailed);

  TemporaryDirectory directory;
  const auto nonExecutablePath = directory.path() / "worker.txt";
  {
    std::ofstream file{nonExecutablePath};
    file << "not executable";
  }
  const auto nonExecutable = ManagedWorkerProcess::start(
      WorkerProcessLaunch{.executable = nonExecutablePath});
  ASSERT_FALSE(nonExecutable.ok());
  EXPECT_NE(nonExecutable.error().message.find("not executable"),
            std::string::npos);

  const auto embeddedNull = ManagedWorkerProcess::start(WorkerProcessLaunch{
      .executable = fixturePath(),
      .arguments = {std::string{"bad\0argument", 12}},
  });
  ASSERT_FALSE(embeddedNull.ok());
  EXPECT_EQ(embeddedNull.error().message,
            "Managed worker arguments are malformed or too large.");

  const auto oversizedTotal = ManagedWorkerProcess::start(WorkerProcessLaunch{
      .executable = fixturePath(),
      .arguments = {std::string(32U * 1024U, 'a'),
                    std::string(32U * 1024U, 'b')},
  });
  ASSERT_FALSE(oversizedTotal.ok());
  EXPECT_EQ(oversizedTotal.error().message,
            "Managed worker arguments are malformed or too large.");

  const auto dataMetadataWithoutDescriptor =
      ManagedWorkerProcess::start(WorkerProcessLaunch{
          .executable = fixturePath(), .readOnlyDataLength = 1});
  ASSERT_FALSE(dataMetadataWithoutDescriptor.ok());
  EXPECT_NE(dataMetadataWithoutDescriptor.error().message.find(
                "metadata has no descriptor"),
            std::string::npos);

  const auto historyMetadataWithoutDescriptor =
      ManagedWorkerProcess::start(WorkerProcessLaunch{
          .executable = fixturePath(),
          .readOnlyHistorySha256 = std::string(64, 'a'),
      });
  ASSERT_FALSE(historyMetadataWithoutDescriptor.ok());
  EXPECT_NE(historyMetadataWithoutDescriptor.error().message.find(
                "metadata has no descriptor"),
            std::string::npos);

  auto memory = ReadOnlySharedMemory::create("shared-data");
  ASSERT_TRUE(memory.ok()) << memory.error().message;
  const auto invalidDataLength =
      ManagedWorkerProcess::start(WorkerProcessLaunch{
          .executable = fixturePath(),
          .readOnlyDataDescriptor = memory.value().descriptor(),
          .readOnlyDataLength = bte::bindings::python::maximumWorkerSourceBytes,
          .readOnlyDataSha256 = memory.value().sha256Digest(),
      });
  ASSERT_FALSE(invalidDataLength.ok());
  EXPECT_NE(invalidDataLength.error().message.find("size does not match"),
            std::string::npos);

  const auto invalidHistoryDigest =
      ManagedWorkerProcess::start(WorkerProcessLaunch{
          .executable = fixturePath(),
          .readOnlyHistoryDescriptor = memory.value().descriptor(),
          .readOnlyHistoryLength = memory.value().size(),
          .readOnlyHistorySha256 = "short",
      });
  ASSERT_FALSE(invalidHistoryDigest.ok());
  EXPECT_NE(invalidHistoryDigest.error().message.find("invalid or writable"),
            std::string::npos);
#else
  GTEST_SKIP()
      << "POSIX worker process fixture is unavailable on this platform.";
#endif
}

TEST(ManagedWorkerProcessTest, rejectsInvalidSharedMemorySizeBoundaries) {
  EXPECT_FALSE(ReadOnlySharedMemory::create("").ok());
  const auto oversized =
      std::string(bte::bindings::python::maximumWorkerSourceBytes + 1, 'x');
  const auto rejected = ReadOnlySharedMemory::create(oversized);
  ASSERT_FALSE(rejected.ok());
  EXPECT_EQ(rejected.error().code, bte::core::ErrorCode::invalidArgument);
}

TEST(ManagedWorkerProcessTest, emptyProcessReportsClosedStateAndNoDiagnostics) {
  ManagedWorkerProcess process;
  const ProtocolMessage message{.protocolVersion = 1,
                                .runId = "fixture-run",
                                .sequence = 0,
                                .messageType = "start",
                                .payload = nlohmann::json::object()};
  EXPECT_FALSE(process.isRunning());
  EXPECT_EQ(process.processId(), -1);
#if defined(__unix__) || defined(__APPLE__)
  EXPECT_TRUE(process.waitForExit(0ms).ok());
  EXPECT_TRUE(process.terminateProcessTree(0ms).ok());

  const auto sent = process.sendMessage(
      message, 0ms, bte::core::ErrorCode::workerHandshakeTimeout);
  ASSERT_FALSE(sent.ok());
  EXPECT_EQ(sent.error().code, bte::core::ErrorCode::workerHandshakeFailed);
  const auto received = process.receiveMessage(
      "fixture-run", 0, 0ms, bte::core::ErrorCode::workerHandshakeTimeout);
  ASSERT_FALSE(received.ok());
  EXPECT_EQ(received.error().code, bte::core::ErrorCode::workerHandshakeFailed);
  EXPECT_TRUE(process.diagnostics().standardOutput.retainedBytes.empty());
#else
  EXPECT_EQ(process.waitForExit(0ms).error().code,
            bte::core::ErrorCode::runtimeUnavailable);
  EXPECT_EQ(process.terminateProcessTree(0ms).error().code,
            bte::core::ErrorCode::runtimeUnavailable);
  const auto sent = process.sendMessage(
      message, 0ms, bte::core::ErrorCode::workerHandshakeTimeout);
  ASSERT_FALSE(sent.ok());
  EXPECT_EQ(sent.error().code, bte::core::ErrorCode::runtimeUnavailable);
  const auto received = process.receiveMessage(
      "fixture-run", 0, 0ms, bte::core::ErrorCode::workerHandshakeTimeout);
  ASSERT_FALSE(received.ok());
  EXPECT_EQ(received.error().code, bte::core::ErrorCode::runtimeUnavailable);
#endif
}

TEST(ManagedWorkerProcessTest,
     receivesBufferedProtocolResponseAfterWorkerExits) {
#if defined(__unix__) || defined(__APPLE__)
  auto started = ManagedWorkerProcess::start(WorkerProcessLaunch{
      .executable = fixturePath(), .arguments = {"exit-ready"}});
  ASSERT_TRUE(started.ok()) << started.error().message;
  auto worker = std::move(started).value();
  ASSERT_TRUE(worker
                  .sendMessage(request(), 2s,
                               bte::core::ErrorCode::workerHandshakeTimeout)
                  .ok());

  std::this_thread::sleep_for(100ms);

  const auto response = worker.receiveMessage(
      "fixture-run", 0, 2s, bte::core::ErrorCode::workerHandshakeTimeout);

  ASSERT_TRUE(response.ok()) << response.error().message;
  EXPECT_EQ(response.value().messageType, "ready");
#else
  GTEST_SKIP()
      << "POSIX worker process fixture is unavailable on this platform.";
#endif
}

TEST(ManagedWorkerProcessTest, drainsDiagnosticBytesWrittenAtProcessExit) {
#if defined(__unix__) || defined(__APPLE__)
  auto started = ManagedWorkerProcess::start(WorkerProcessLaunch{
      .executable = fixturePath(), .arguments = {"exit-output"}});
  ASSERT_TRUE(started.ok()) << started.error().message;
  auto worker = std::move(started).value();
  ASSERT_TRUE(worker
                  .sendMessage(request(), 2s,
                               bte::core::ErrorCode::workerHandshakeTimeout)
                  .ok());
  const auto response = worker.receiveMessage(
      "fixture-run", 0, 2s, bte::core::ErrorCode::workerHandshakeTimeout);
  ASSERT_TRUE(response.ok()) << response.error().message;
  ASSERT_TRUE(
      worker
          .sendMessage(ProtocolMessage{.protocolVersion = 1,
                                       .runId = "fixture-run",
                                       .sequence = 1,
                                       .messageType = "shutdown",
                                       .payload = nlohmann::json::object()},
                       2s, bte::core::ErrorCode::workerHandshakeTimeout)
          .ok());
  std::this_thread::sleep_for(100ms);
  ASSERT_TRUE(worker.waitForExit(2s).ok());

  const auto diagnostics = worker.diagnostics();
  EXPECT_EQ(diagnostics.standardOutput.retainedBytes, "exit-output\n");
  EXPECT_EQ(diagnostics.standardOutput.bytesSeen, 12U);
#else
  GTEST_SKIP()
      << "POSIX worker process fixture is unavailable on this platform.";
#endif
}

TEST(ManagedWorkerProcessTest,
     passesUnlinkedSharedMemoryThroughReadOnlyDescriptor) {
#if defined(__unix__) || defined(__APPLE__)
  constexpr std::string_view source =
      "def create_strategy(params): return None\n";
  auto sharedMemoryResult = ReadOnlySharedMemory::create(source);
  ASSERT_TRUE(sharedMemoryResult.ok()) << sharedMemoryResult.error().message;
  auto sharedMemory = std::move(sharedMemoryResult).value();
  constexpr std::string_view historyBytes = "binary-history-region";
  auto historyMemoryResult = ReadOnlySharedMemory::create(historyBytes);
  ASSERT_TRUE(historyMemoryResult.ok()) << historyMemoryResult.error().message;
  auto historyMemory = std::move(historyMemoryResult).value();
  const auto flags = ::fcntl(sharedMemory.descriptor(), F_GETFL);
  ASSERT_GE(flags, 0);
  EXPECT_EQ(flags & O_ACCMODE, O_RDONLY);
  EXPECT_EQ(::write(sharedMemory.descriptor(), "x", 1), -1);
  EXPECT_EQ(errno, EBADF);
  std::string sourceRead(source.size(), '\0');
  const auto *mapping = ::mmap(nullptr, sourceRead.size(), PROT_READ,
                               MAP_SHARED, sharedMemory.descriptor(), 0);
  ASSERT_NE(mapping, MAP_FAILED);
  std::memcpy(sourceRead.data(), mapping, sourceRead.size());
  EXPECT_EQ(::munmap(const_cast<void *>(mapping), sourceRead.size()), 0);
  EXPECT_EQ(sourceRead, std::string{source});

  auto started = ManagedWorkerProcess::start(WorkerProcessLaunch{
      .executable = fixturePath(),
      .arguments = {"read-history"},
      .readOnlyDataDescriptor = sharedMemory.descriptor(),
      .readOnlyDataLength = sharedMemory.size(),
      .readOnlyDataSha256 = sharedMemory.sha256Digest(),
      .readOnlyHistoryDescriptor = historyMemory.descriptor(),
      .readOnlyHistoryLength = historyMemory.size(),
      .readOnlyHistorySha256 = historyMemory.sha256Digest(),
  });
  ASSERT_TRUE(started.ok()) << started.error().message;
  auto worker = std::move(started).value();
  sharedMemory = ReadOnlySharedMemory{};
  historyMemory = ReadOnlySharedMemory{};

  ASSERT_TRUE(worker
                  .sendMessage(request(), 2s,
                               bte::core::ErrorCode::workerHandshakeTimeout)
                  .ok());
  const auto response = worker.receiveMessage(
      "fixture-run", 0, 2s, bte::core::ErrorCode::workerHandshakeTimeout);
  ASSERT_TRUE(response.ok())
      << response.error().message << "\n"
      << worker.diagnostics().standardError.retainedBytes;
  EXPECT_EQ(response.value().payload.at("source").get<std::string>(), source);
  EXPECT_TRUE(response.value().payload.at("sourceSha256Matches").get<bool>());
  EXPECT_TRUE(
      response.value().payload.at("sharedSourceIsReadOnly").get<bool>());
  EXPECT_EQ(response.value().payload.at("history").get<std::string>(),
            historyBytes);
  EXPECT_TRUE(response.value().payload.at("historySha256Matches").get<bool>());
  EXPECT_TRUE(
      response.value().payload.at("sharedHistoryIsReadOnly").get<bool>());
#else
  GTEST_SKIP() << "POSIX shared memory is unavailable on this platform.";
#endif
}

TEST(ManagedWorkerProcessTest,
     enforcesParentDeadlinesAndRejectsInvalidWorkerFrames) {
#if defined(__unix__) || defined(__APPLE__)
  auto hangingStart = ManagedWorkerProcess::start(
      WorkerProcessLaunch{.executable = fixturePath(), .arguments = {"hang"}});
  ASSERT_TRUE(hangingStart.ok()) << hangingStart.error().message;
  auto hangingWorker = std::move(hangingStart).value();
  const auto timedOut = hangingWorker.receiveMessage(
      "fixture-run", 0, 50ms, bte::core::ErrorCode::workerHandshakeTimeout);
  ASSERT_FALSE(timedOut.ok());
  EXPECT_EQ(timedOut.error().code,
            bte::core::ErrorCode::workerHandshakeTimeout);
  const auto exitTimedOut = hangingWorker.waitForExit(20ms);
  ASSERT_FALSE(exitTimedOut.ok());
  EXPECT_EQ(exitTimedOut.error().code, bte::core::ErrorCode::timeout);
  ASSERT_TRUE(hangingWorker.terminateProcessTree(50ms).ok());
  EXPECT_FALSE(hangingWorker.isRunning());

  auto invalidStart = ManagedWorkerProcess::start(WorkerProcessLaunch{
      .executable = fixturePath(), .arguments = {"invalid-frame"}});
  ASSERT_TRUE(invalidStart.ok()) << invalidStart.error().message;
  auto invalidWorker = std::move(invalidStart).value();
  const auto invalid = invalidWorker.receiveMessage(
      "fixture-run", 0, 2s, bte::core::ErrorCode::workerHandshakeTimeout);
  ASSERT_FALSE(invalid.ok());
  EXPECT_EQ(invalid.error().code,
            bte::core::ErrorCode::strategyProtocolViolation);
#else
  GTEST_SKIP()
      << "POSIX worker process fixture is unavailable on this platform.";
#endif
}

TEST(ManagedWorkerProcessTest, cancellationInterruptsAWorkerPipeWait) {
#if defined(__unix__) || defined(__APPLE__)
  auto started = ManagedWorkerProcess::start(
      WorkerProcessLaunch{.executable = fixturePath(), .arguments = {"hang"}});
  ASSERT_TRUE(started.ok()) << started.error().message;
  auto worker = std::move(started).value();
  bte::core::CancellationSource cancellation;
  cancellation.requestCancellation();

  const auto received = worker.receiveMessage(
      "fixture-run", 0, 2s, bte::core::ErrorCode::workerHandshakeTimeout,
      cancellation.token());

  ASSERT_FALSE(received.ok());
  EXPECT_EQ(received.error().code, bte::core::ErrorCode::cancelled);
  ASSERT_TRUE(worker.terminateProcessTree(50ms).ok());
#else
  GTEST_SKIP()
      << "POSIX worker process fixture is unavailable on this platform.";
#endif
}

TEST(ManagedWorkerProcessTest, rejectsUnexpectedProtocolBytesWhileWorkerExits) {
#if defined(__unix__) || defined(__APPLE__)
  auto started = ManagedWorkerProcess::start(WorkerProcessLaunch{
      .executable = fixturePath(), .arguments = {"invalid-frame"}});
  ASSERT_TRUE(started.ok()) << started.error().message;
  auto worker = std::move(started).value();

  const auto exited = worker.waitForExit(2s);

  ASSERT_FALSE(exited.ok());
  EXPECT_EQ(exited.error().code,
            bte::core::ErrorCode::strategyProtocolViolation);
#else
  GTEST_SKIP()
      << "POSIX worker process fixture is unavailable on this platform.";
#endif
}

TEST(ManagedWorkerProcessTest, terminatesTheOwnedProcessGroupAndDescendants) {
#if defined(__unix__) || defined(__APPLE__)
  auto started = ManagedWorkerProcess::start(WorkerProcessLaunch{
      .executable = fixturePath(), .arguments = {"descendant"}});
  ASSERT_TRUE(started.ok()) << started.error().message;
  auto worker = std::move(started).value();
  const auto group = worker.processId();
  ASSERT_GT(group, 0);
  std::this_thread::sleep_for(20ms);

  ASSERT_TRUE(worker.terminateProcessTree(50ms).ok());
  EXPECT_FALSE(worker.isRunning());
  const auto groupGoneDeadline = std::chrono::steady_clock::now() + 1s;
  while (::kill(-group, 0) == 0 &&
         std::chrono::steady_clock::now() < groupGoneDeadline) {
    std::this_thread::sleep_for(5ms);
  }
  EXPECT_EQ(::kill(-group, 0), -1);
  EXPECT_EQ(errno, ESRCH);
#else
  GTEST_SKIP()
      << "POSIX process-group support is unavailable on this platform.";
#endif
}

} // namespace
