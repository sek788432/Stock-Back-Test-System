#include "ManagedWorkerProcess.h"
#include "ManagedWorkerProcessTestHooks.h"

#include <Bte/Core/Digest.h>

#include <gtest/gtest.h>

#include <array>
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
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

using namespace std::chrono_literals;
using bte::bindings::python::ManagedWorkerProcess;
using bte::bindings::python::ProtocolMessage;
using bte::bindings::python::ReadOnlySharedMemory;
using bte::bindings::python::WorkerProcessLaunch;
using bte::bindings::python::testing::ManagedWorkerSystemCall;

#if defined(__unix__) || defined(__APPLE__)
[[nodiscard]] std::filesystem::path fixturePath() {
  return std::filesystem::path{BTE_WORKER_FIXTURE_PATH};
}

[[nodiscard]] std::vector<int> openDescriptorSnapshot() {
  std::vector<int> descriptors;
  for (int descriptor = 0; descriptor < 256; ++descriptor) {
    const auto duplicate = ::dup(descriptor);
    if (duplicate >= 0) {
      descriptors.emplace_back(descriptor);
      static_cast<void>(::close(duplicate));
    }
  }
  return descriptors;
}

class ScopedReservedDescriptors final {
public:
  ~ScopedReservedDescriptors() {
    for (const auto descriptor : descriptors_) {
      static_cast<void>(::close(descriptor));
    }
  }

  [[nodiscard]] bool reserve(const std::size_t count) {
    while (descriptors_.size() < count) {
      const auto descriptor = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
      if (descriptor < 0) {
        return false;
      }
      descriptors_.push_back(descriptor);
    }
    return true;
  }

private:
  std::vector<int> descriptors_;
};

class ManagedWorkerSystemCallFailure final {
public:
  ManagedWorkerSystemCallFailure(const ManagedWorkerSystemCall systemCall,
                                 const std::uint32_t skippedCalls,
                                 const int errorNumber) {
    bte::bindings::python::testing::failNextManagedWorkerSystemCall(
        systemCall, skippedCalls, errorNumber);
  }

  ~ManagedWorkerSystemCallFailure() {
    bte::bindings::python::testing::clearManagedWorkerSystemCallFailure();
  }

  ManagedWorkerSystemCallFailure(const ManagedWorkerSystemCallFailure &) =
      delete;
  ManagedWorkerSystemCallFailure &
  operator=(const ManagedWorkerSystemCallFailure &) = delete;
};

[[nodiscard]] bool
waitForProcessExitWithoutReaping(const ManagedWorkerProcess &worker,
                                 const std::chrono::milliseconds timeout) {
  const auto process = worker.processId();
  if (process <= 0) {
    return false;
  }
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    siginfo_t status{};
    if (::waitid(P_PID, static_cast<id_t>(process), &status,
                 WEXITED | WNOHANG | WNOWAIT) != 0) {
      if (errno == EINTR) {
        continue;
      }
      return false;
    }
    if (status.si_pid == process) {
      return true;
    }
    std::this_thread::sleep_for(1ms);
  }
  return false;
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

  const auto invalidFormatPath = directory.path() / "invalid-worker";
  {
    std::ofstream file{invalidFormatPath};
    file << "not a valid executable";
  }
  std::filesystem::permissions(invalidFormatPath,
                               std::filesystem::perms::owner_read |
                                   std::filesystem::perms::owner_write |
                                   std::filesystem::perms::owner_exec,
                               std::filesystem::perm_options::replace);
  const auto invalidFormat = ManagedWorkerProcess::start(
      WorkerProcessLaunch{.executable = invalidFormatPath});
  ASSERT_FALSE(invalidFormat.ok());
  EXPECT_EQ(invalidFormat.error().code,
            bte::core::ErrorCode::workerStartFailed);
  EXPECT_EQ(invalidFormat.error().message,
            "Could not launch the managed worker executable.");

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

  const auto writableDataPath = directory.path() / "writable-source.txt";
  {
    std::ofstream file{writableDataPath, std::ios::binary};
    file << "shared-data";
  }
  const int writableDataDescriptor =
      ::open(writableDataPath.c_str(), O_RDWR | O_CLOEXEC);
  ASSERT_GE(writableDataDescriptor, 0);
  const auto writableData = ManagedWorkerProcess::start(WorkerProcessLaunch{
      .executable = fixturePath(),
      .readOnlyDataDescriptor = writableDataDescriptor,
      .readOnlyDataLength = std::string_view{"shared-data"}.size(),
      .readOnlyDataSha256 = bte::core::sha256("shared-data"),
  });
  static_cast<void>(::close(writableDataDescriptor));
  ASSERT_FALSE(writableData.ok());
  EXPECT_NE(writableData.error().message.find("invalid or writable"),
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

  const int readOnlyHistoryDescriptor =
      ::open(writableDataPath.c_str(), O_RDONLY | O_CLOEXEC);
  ASSERT_GE(readOnlyHistoryDescriptor, 0);
  const auto invalidHistoryLength =
      ManagedWorkerProcess::start(WorkerProcessLaunch{
          .executable = fixturePath(),
          .readOnlyHistoryDescriptor = readOnlyHistoryDescriptor,
          .readOnlyHistoryLength = std::string_view{"shared-data"}.size() + 1,
          .readOnlyHistorySha256 = memory.value().sha256Digest(),
      });
  static_cast<void>(::close(readOnlyHistoryDescriptor));
  ASSERT_FALSE(invalidHistoryLength.ok());
  EXPECT_NE(invalidHistoryLength.error().message.find("size does not match"),
            std::string::npos);
#else
  GTEST_SKIP()
      << "POSIX worker process fixture is unavailable on this platform.";
#endif
}

TEST(ManagedWorkerProcessTest,
     rejectsInvalidSharedMemoryMetadataBeforeSpawning) {
#if defined(__unix__) || defined(__APPLE__)
  auto memoryResult = ReadOnlySharedMemory::create("shared-data");
  ASSERT_TRUE(memoryResult.ok()) << memoryResult.error().message;
  const auto &memory = memoryResult.value();

  struct InvalidLaunch final {
    WorkerProcessLaunch launch;
    std::string_view expectedError;
  };
  const auto invalidLaunches = std::vector<InvalidLaunch>{
      InvalidLaunch{
          .launch =
              WorkerProcessLaunch{
                  .executable = fixturePath(),
                  .readOnlyDataDescriptor = memory.descriptor(),
                  .readOnlyDataLength = 0,
                  .readOnlyDataSha256 = memory.sha256Digest(),
              },
          .expectedError =
              "Worker shared-data descriptor is invalid or writable."},
      InvalidLaunch{
          .launch =
              WorkerProcessLaunch{
                  .executable = fixturePath(),
                  .readOnlyDataDescriptor = memory.descriptor(),
                  .readOnlyDataLength =
                      bte::bindings::python::maximumWorkerSourceBytes + 1,
                  .readOnlyDataSha256 = memory.sha256Digest(),
              },
          .expectedError =
              "Worker shared-data descriptor is invalid or writable."},
      InvalidLaunch{
          .launch =
              WorkerProcessLaunch{
                  .executable = fixturePath(),
                  .readOnlyDataDescriptor = memory.descriptor(),
                  .readOnlyDataLength = memory.size(),
                  .readOnlyDataSha256 = "short",
              },
          .expectedError =
              "Worker shared-data descriptor is invalid or writable."},
      InvalidLaunch{
          .launch =
              WorkerProcessLaunch{
                  .executable = fixturePath(),
                  .readOnlyHistoryDescriptor = memory.descriptor(),
                  .readOnlyHistoryLength = 0,
                  .readOnlyHistorySha256 = memory.sha256Digest(),
              },
          .expectedError =
              "Worker shared-history descriptor is invalid or writable."},
      InvalidLaunch{
          .launch =
              WorkerProcessLaunch{
                  .executable = fixturePath(),
                  .readOnlyHistoryDescriptor = memory.descriptor(),
                  .readOnlyHistoryLength =
                      bte::bindings::python::maximumWorkerSourceBytes + 1,
                  .readOnlyHistorySha256 = memory.sha256Digest(),
              },
          .expectedError =
              "Worker shared-history descriptor is invalid or writable."},
      InvalidLaunch{
          .launch =
              WorkerProcessLaunch{
                  .executable = fixturePath(),
                  .readOnlyHistoryDescriptor = memory.descriptor(),
                  .readOnlyHistoryLength = memory.size(),
                  .readOnlyHistorySha256 = "short",
              },
          .expectedError =
              "Worker shared-history descriptor is invalid or writable."},
  };

  const auto startCountBefore =
      bte::bindings::python::testing::managedWorkerProcessStartCount();
  for (const auto &invalid : invalidLaunches) {
    const auto started = ManagedWorkerProcess::start(invalid.launch);
    ASSERT_FALSE(started.ok());
    EXPECT_EQ(started.error().message, invalid.expectedError);
  }
  EXPECT_EQ(bte::bindings::python::testing::managedWorkerProcessStartCount(),
            startCountBefore);
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

TEST(ManagedWorkerProcessTest,
     reportsSharedMemorySystemCallFailuresAndReleasesDescriptors) {
#if defined(__unix__) || defined(__APPLE__)
  struct ExpectedFailure final {
    ManagedWorkerSystemCall systemCall;
    std::string_view message;
  };
  const auto failures = std::vector<ExpectedFailure>{
      {ManagedWorkerSystemCall::sharedMemoryCreate,
       "Could not create a shared-memory object for strategy source:"},
      {ManagedWorkerSystemCall::sharedMemoryTruncate,
       "Could not size shared memory for strategy source."},
      {ManagedWorkerSystemCall::sharedMemoryMap,
       "Could not map shared memory for strategy source."},
      {ManagedWorkerSystemCall::sharedMemoryReadOnlyOpen,
       "Could not reopen strategy source as read-only memory."},
      {ManagedWorkerSystemCall::sharedMemoryStat,
       "Read-only strategy memory has an unexpected size"},
  };

  for (const auto &failure : failures) {
    const auto descriptorsBefore = openDescriptorSnapshot();
    ManagedWorkerSystemCallFailure inject{failure.systemCall, 0, EIO};
    const auto memory = ReadOnlySharedMemory::create("source bytes");

    ASSERT_FALSE(memory.ok());
    EXPECT_EQ(memory.error().code, bte::core::ErrorCode::workerStartFailed);
    EXPECT_NE(memory.error().message.find(failure.message), std::string::npos);
    EXPECT_EQ(openDescriptorSnapshot(), descriptorsBefore);
  }
#else
  GTEST_SKIP() << "POSIX shared memory is unavailable on this platform.";
#endif
}

TEST(ManagedWorkerProcessTest,
     closesPartialPipeAndDuplicateSetupWhenSystemCallsFail) {
#if defined(__unix__) || defined(__APPLE__)
  struct ExpectedFailure final {
    ManagedWorkerSystemCall systemCall;
    std::uint32_t skippedCalls;
    std::string_view message;
  };
  const auto failures = std::vector<ExpectedFailure>{
      {ManagedWorkerSystemCall::pipeCreate, 2,
       "Could not create managed worker pipes."},
      {ManagedWorkerSystemCall::closeOnExec, 2,
       "Could not isolate managed worker pipe descriptors."},
      {ManagedWorkerSystemCall::duplicateDescriptor, 2,
       "Could not prepare worker protocol descriptors."},
  };

  const auto startCountBefore =
      bte::bindings::python::testing::managedWorkerProcessStartCount();
  for (const auto &failure : failures) {
    const auto descriptorsBefore = openDescriptorSnapshot();
    ManagedWorkerSystemCallFailure inject{failure.systemCall,
                                          failure.skippedCalls, EMFILE};
    const auto started = ManagedWorkerProcess::start(WorkerProcessLaunch{
        .executable = fixturePath(),
    });

    ASSERT_FALSE(started.ok());
    EXPECT_EQ(started.error().code, bte::core::ErrorCode::workerStartFailed);
    EXPECT_EQ(started.error().message, std::string{failure.message});
    EXPECT_EQ(openDescriptorSnapshot(), descriptorsBefore);
    EXPECT_EQ(bte::bindings::python::testing::managedWorkerProcessStartCount(),
              startCountBefore);
  }
#else
  GTEST_SKIP()
      << "POSIX worker process fixture is unavailable on this platform.";
#endif
}

TEST(ManagedWorkerProcessTest,
     releasesSourceAndHistoryMemoryWhenDescriptorDuplicationFails) {
#if defined(__unix__) || defined(__APPLE__)
  auto sourceResult = ReadOnlySharedMemory::create("strategy source");
  ASSERT_TRUE(sourceResult.ok()) << sourceResult.error().message;
  auto source = std::move(sourceResult).value();
  auto historyResult = ReadOnlySharedMemory::create("warmup history");
  ASSERT_TRUE(historyResult.ok()) << historyResult.error().message;
  auto history = std::move(historyResult).value();

  struct ExpectedFailure final {
    std::string_view name;
    std::uint32_t successfulDuplicates;
    bool includeHistory;
    std::string_view message;
  };
  const std::array failures{
      ExpectedFailure{
          "source", 4, false,
          "Could not prepare the read-only shared-data descriptor."},
      ExpectedFailure{
          "history", 5, true,
          "Could not prepare the read-only shared-history descriptor."},
  };

  const auto startCountBefore =
      bte::bindings::python::testing::managedWorkerProcessStartCount();
  for (const auto &failure : failures) {
    const auto descriptorsBefore = openDescriptorSnapshot();
    ManagedWorkerSystemCallFailure inject{
        ManagedWorkerSystemCall::duplicateDescriptor,
        failure.successfulDuplicates, EMFILE};
    const auto started = ManagedWorkerProcess::start(WorkerProcessLaunch{
        .executable = fixturePath(),
        .readOnlyDataDescriptor = source.descriptor(),
        .readOnlyDataLength = source.size(),
        .readOnlyDataSha256 = source.sha256Digest(),
        .readOnlyHistoryDescriptor =
            failure.includeHistory ? history.descriptor() : -1,
        .readOnlyHistoryLength = failure.includeHistory ? history.size() : 0,
        .readOnlyHistorySha256 =
            failure.includeHistory ? history.sha256Digest() : std::string{},
    });

    ASSERT_FALSE(started.ok()) << failure.name;
    EXPECT_EQ(started.error().code, bte::core::ErrorCode::workerStartFailed)
        << failure.name;
    EXPECT_EQ(started.error().message, failure.message) << failure.name;
    EXPECT_EQ(openDescriptorSnapshot(), descriptorsBefore) << failure.name;
    EXPECT_EQ(bte::bindings::python::testing::managedWorkerProcessStartCount(),
              startCountBefore)
        << failure.name;
  }
#else
  GTEST_SKIP()
      << "POSIX worker process fixture is unavailable on this platform.";
#endif
}

TEST(ManagedWorkerProcessTest,
     reportsSpawnSetupFailuresAndReleasesEveryDescriptor) {
#if defined(__unix__) || defined(__APPLE__)
  ScopedReservedDescriptors reserved;
  ASSERT_TRUE(reserved.reserve(3));
  auto sourceResult = ReadOnlySharedMemory::create("strategy source");
  ASSERT_TRUE(sourceResult.ok()) << sourceResult.error().message;
  auto source = std::move(sourceResult).value();
  auto historyResult = ReadOnlySharedMemory::create("warmup history");
  ASSERT_TRUE(historyResult.ok()) << historyResult.error().message;
  auto history = std::move(historyResult).value();

  struct ExpectedFailure final {
    ManagedWorkerSystemCall systemCall;
    std::uint32_t skippedCalls;
    std::string_view message;
    bool spawned;
  };
  const std::array failures{
      ExpectedFailure{ManagedWorkerSystemCall::spawnActionsInitialize, 0,
                      "Could not initialize managed worker spawn actions.",
                      false},
      ExpectedFailure{ManagedWorkerSystemCall::spawnAttributesInitialize, 0,
                      "Could not initialize managed worker spawn actions.",
                      false},
      ExpectedFailure{ManagedWorkerSystemCall::spawnActionsAddDup2, 0,
                      "Could not route managed worker protocol pipes.", false},
      ExpectedFailure{ManagedWorkerSystemCall::spawnActionsAddDup2, 4,
                      "Could not route the read-only shared-data descriptor.",
                      false},
      ExpectedFailure{
          ManagedWorkerSystemCall::spawnActionsAddDup2, 5,
          "Could not route the read-only shared-history descriptor.", false},
      ExpectedFailure{
          ManagedWorkerSystemCall::spawnActionsAddClose, 0,
          "Could not close temporary worker descriptors after spawn.", false},
      ExpectedFailure{ManagedWorkerSystemCall::spawnActionsAddClose, 4,
                      "Could not close the temporary shared-data descriptor.",
                      false},
      ExpectedFailure{
          ManagedWorkerSystemCall::spawnActionsAddClose, 5,
          "Could not close the temporary shared-history descriptor.", false},
      ExpectedFailure{ManagedWorkerSystemCall::spawnActionsAddClose, 6,
                      "Could not close inherited parent pipe descriptors.",
                      false},
      ExpectedFailure{ManagedWorkerSystemCall::spawnActionsAddClose, 14,
                      "Could not close the inherited source descriptor.",
                      false},
      ExpectedFailure{ManagedWorkerSystemCall::spawnActionsAddClose, 15,
                      "Could not close the inherited history descriptor.",
                      false},
      ExpectedFailure{
          ManagedWorkerSystemCall::spawnAttributesSetProcessGroup, 0,
          "Could not configure managed worker process ownership.", false},
      ExpectedFailure{ManagedWorkerSystemCall::spawnAttributesSetFlags, 0,
                      "Could not configure managed worker process ownership.",
                      false},
      ExpectedFailure{ManagedWorkerSystemCall::processSpawn, 0,
                      "Could not launch the managed worker executable.", false},
      ExpectedFailure{
          ManagedWorkerSystemCall::nonBlocking, 0,
          "Could not configure managed worker pipes as nonblocking.", true},
  };
  const auto startCountBefore =
      bte::bindings::python::testing::managedWorkerProcessStartCount();
  for (const auto &failure : failures) {
    const auto descriptorsBefore = openDescriptorSnapshot();
    ManagedWorkerSystemCallFailure inject{failure.systemCall,
                                          failure.skippedCalls, EIO};
    const auto started = ManagedWorkerProcess::start(WorkerProcessLaunch{
        .executable = fixturePath(),
        .arguments = {"hang"},
        .readOnlyDataDescriptor = source.descriptor(),
        .readOnlyDataLength = source.size(),
        .readOnlyDataSha256 = source.sha256Digest(),
        .readOnlyHistoryDescriptor = history.descriptor(),
        .readOnlyHistoryLength = history.size(),
        .readOnlyHistorySha256 = history.sha256Digest(),
    });

    ASSERT_FALSE(started.ok());
    EXPECT_EQ(started.error().code, bte::core::ErrorCode::workerStartFailed);
    EXPECT_EQ(started.error().message, failure.message);
    EXPECT_EQ(openDescriptorSnapshot(), descriptorsBefore);
    EXPECT_EQ(bte::bindings::python::testing::managedWorkerProcessStartCount(),
              startCountBefore + static_cast<std::uint64_t>(failure.spawned));
  }
#else
  GTEST_SKIP() << "POSIX worker process setup is unavailable on this platform.";
#endif
}

TEST(ManagedWorkerProcessTest, acceptsSharedMemoryAtTheMaximumSourceSize) {
#if defined(__unix__) || defined(__APPLE__)
  const auto source =
      std::string(bte::bindings::python::maximumWorkerSourceBytes, 'x');
  auto memory = ReadOnlySharedMemory::create(source);

  ASSERT_TRUE(memory.ok()) << memory.error().message;
  EXPECT_EQ(memory.value().size(), source.size());
  EXPECT_EQ(memory.value().sha256Digest(), bte::core::sha256(source));
#else
  GTEST_SKIP() << "POSIX shared memory is unavailable on this platform.";
#endif
}

TEST(ManagedWorkerProcessTest, rejectsMalformedTimezonePathBeforeSpawning) {
#if defined(__unix__) || defined(__APPLE__)
  const auto startCountBefore =
      bte::bindings::python::testing::managedWorkerProcessStartCount();
  const std::string malformedPath{"zone\0info", 9};
  const auto started = ManagedWorkerProcess::start(WorkerProcessLaunch{
      .executable = fixturePath(),
      .timezoneDatabasePath = std::filesystem::path{malformedPath},
  });

  ASSERT_FALSE(started.ok());
  EXPECT_EQ(started.error().message,
            "Worker timezone database path is malformed.");
  EXPECT_EQ(bte::bindings::python::testing::managedWorkerProcessStartCount(),
            startCountBefore);
#else
  GTEST_SKIP()
      << "POSIX worker process fixture is unavailable on this platform.";
#endif
}

TEST(ManagedWorkerProcessTest, acceptsArgumentsAtTheAggregateSizeLimit) {
#if defined(__unix__) || defined(__APPLE__)
  constexpr auto maximumArgumentBytes = std::size_t{64} * std::size_t{1024};
  const auto executable = std::filesystem::canonical(fixturePath());
  constexpr std::size_t modeLength = std::string_view{"echo"}.size();
  constexpr std::size_t byteCountLength = 1;
  ASSERT_LT(executable.string().size() + modeLength + byteCountLength,
            maximumArgumentBytes);
  const auto paddingBytes = maximumArgumentBytes - executable.string().size() -
                            modeLength - byteCountLength;
  auto started = ManagedWorkerProcess::start(WorkerProcessLaunch{
      .executable = executable,
      .arguments = {"echo", "0", std::string(paddingBytes, 'x')},
  });
  ASSERT_TRUE(started.ok()) << started.error().message;
  auto worker = std::move(started).value();

  ASSERT_TRUE(worker
                  .sendMessage(request(), 2s,
                               bte::core::ErrorCode::workerHandshakeTimeout)
                  .ok());
  const auto response = worker.receiveMessage(
      "fixture-run", 0, 2s, bte::core::ErrorCode::workerHandshakeTimeout);

  ASSERT_TRUE(response.ok()) << response.error().message;
  EXPECT_EQ(response.value().messageType, "ready");
  EXPECT_TRUE(worker.waitForExit(2s).ok());
#else
  GTEST_SKIP()
      << "POSIX worker process fixture is unavailable on this platform.";
#endif
}

TEST(ManagedWorkerProcessTest,
     passesTimezoneDatabasePathAsOneArgumentToTheWorker) {
#if defined(__unix__) || defined(__APPLE__)
  TemporaryDirectory directory;
  const auto timezonePath = directory.path() / "zone database";
  {
    std::ofstream timezoneFile{timezonePath};
    ASSERT_TRUE(timezoneFile.good());
    timezoneFile << "timezone data";
  }

  const auto expectedArgument = "--timezone-data-path=" + timezonePath.string();
  const auto script =
      "import sys; expected = " + nlohmann::json(expectedArgument).dump() +
      "; assert sys.argv[1:] == [expected], repr(sys.argv); "
      "print('timezone-argument-ok', file=sys.stderr)";
  auto started = ManagedWorkerProcess::start(WorkerProcessLaunch{
      .executable = std::filesystem::path{BTE_TEST_PYTHON_EXECUTABLE},
      .arguments = {"-c", script},
      .timezoneDatabasePath = timezonePath,
  });
  ASSERT_TRUE(started.ok()) << started.error().message;
  auto worker = std::move(started).value();

  ASSERT_TRUE(worker.waitForExit(2s).ok());
  const auto diagnostics = worker.diagnostics();
  EXPECT_EQ(diagnostics.standardError.retainedBytes, "timezone-argument-ok\n");
#else
  GTEST_SKIP()
      << "POSIX worker process fixture is unavailable on this platform.";
#endif
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

  ASSERT_TRUE(waitForProcessExitWithoutReaping(worker, 2s));

  const auto response = worker.receiveMessage(
      "fixture-run", 0, 2s, bte::core::ErrorCode::workerHandshakeTimeout);

  ASSERT_TRUE(response.ok()) << response.error().message;
  EXPECT_EQ(response.value().messageType, "ready");
  const auto followup = worker.sendMessage(
      request(), 2s, bte::core::ErrorCode::workerHandshakeTimeout);
  ASSERT_FALSE(followup.ok());
  EXPECT_EQ(followup.error().message, "Managed worker process already exited.");
#else
  GTEST_SKIP()
      << "POSIX worker process fixture is unavailable on this platform.";
#endif
}

TEST(ManagedWorkerProcessTest,
     rejectsWorkerResponsesWithUnexpectedIdentityOrSequence) {
#if defined(__unix__) || defined(__APPLE__)
  struct ExpectedResponse final {
    std::string_view runId;
    std::uint64_t sequence;
  };
  const auto invalidExpectations = std::vector<ExpectedResponse>{
      ExpectedResponse{.runId = "different-run", .sequence = 0},
      ExpectedResponse{.runId = "fixture-run", .sequence = 1},
  };

  for (const auto &expected : invalidExpectations) {
    auto started = ManagedWorkerProcess::start(WorkerProcessLaunch{
        .executable = fixturePath(), .arguments = {"exit-ready"}});
    ASSERT_TRUE(started.ok()) << started.error().message;
    auto worker = std::move(started).value();
    ASSERT_TRUE(worker
                    .sendMessage(request(), 2s,
                                 bte::core::ErrorCode::workerHandshakeTimeout)
                    .ok());

    const auto response =
        worker.receiveMessage(expected.runId, expected.sequence, 2s,
                              bte::core::ErrorCode::workerHandshakeTimeout);

    ASSERT_FALSE(response.ok());
    EXPECT_EQ(response.error().code,
              bte::core::ErrorCode::strategyProtocolViolation);
    EXPECT_EQ(response.error().message,
              "Protocol message version, identity, sequence, or type is "
              "invalid.");
  }
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
     passesOnlyTheSourceSharedMemoryDescriptorWhenHistoryIsAbsent) {
#if defined(__unix__) || defined(__APPLE__)
  constexpr std::string_view source =
      "def create_strategy(params): return None\n";
  auto sharedMemoryResult = ReadOnlySharedMemory::create(source);
  ASSERT_TRUE(sharedMemoryResult.ok()) << sharedMemoryResult.error().message;
  auto sharedMemory = std::move(sharedMemoryResult).value();

  auto started = ManagedWorkerProcess::start(WorkerProcessLaunch{
      .executable = fixturePath(),
      .arguments = {"read-source"},
      .readOnlyDataDescriptor = sharedMemory.descriptor(),
      .readOnlyDataLength = sharedMemory.size(),
      .readOnlyDataSha256 = sharedMemory.sha256Digest(),
  });
  ASSERT_TRUE(started.ok()) << started.error().message;
  auto worker = std::move(started).value();
  sharedMemory = ReadOnlySharedMemory{};

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
  EXPECT_TRUE(
      response.value().payload.at("history").get<std::string>().empty());
  EXPECT_FALSE(response.value().payload.at("historySha256Matches").get<bool>());
  EXPECT_FALSE(
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

TEST(ManagedWorkerProcessTest,
     handlesWorkerClosingProtocolPipesWithoutRaisingSigpipe) {
#if defined(__unix__) || defined(__APPLE__)
  auto started = ManagedWorkerProcess::start(WorkerProcessLaunch{
      .executable = std::filesystem::path{BTE_TEST_PYTHON_EXECUTABLE},
      .arguments = {"-c", "import os, time; os.close(0); os.close(3); "
                          "os.write(2, b'pipes-closed\\n'); time.sleep(10)"},
  });
  ASSERT_TRUE(started.ok()) << started.error().message;
  auto worker = std::move(started).value();

  const auto received = worker.receiveMessage(
      "fixture-run", 0, 2s, bte::core::ErrorCode::workerHandshakeTimeout);
  ASSERT_FALSE(received.ok());
  EXPECT_EQ(received.error().code,
            bte::core::ErrorCode::strategyProtocolViolation);
  EXPECT_EQ(received.error().message,
            "Protocol stream ended before the frame was complete.");
  EXPECT_NE(
      worker.diagnostics().standardError.retainedBytes.find("pipes-closed\n"),
      std::string::npos);
  ASSERT_TRUE(worker.isRunning());

  const auto sent = worker.sendMessage(
      request(), 2s, bte::core::ErrorCode::workerHandshakeTimeout);
  ASSERT_FALSE(sent.ok());
  EXPECT_EQ(sent.error().code, bte::core::ErrorCode::workerHandshakeFailed);
  EXPECT_EQ(sent.error().message,
            "Could not write a managed worker protocol frame.");
  ASSERT_TRUE(worker.terminateProcessTree(50ms).ok());
#else
  GTEST_SKIP()
      << "POSIX worker process fixture is unavailable on this platform.";
#endif
}

TEST(ManagedWorkerProcessTest,
     classifiesPollAndProtocolReadSystemCallFailures) {
#if defined(__unix__) || defined(__APPLE__)
  auto hangingStart = ManagedWorkerProcess::start(
      WorkerProcessLaunch{.executable = fixturePath(), .arguments = {"hang"}});
  ASSERT_TRUE(hangingStart.ok()) << hangingStart.error().message;
  auto hangingWorker = std::move(hangingStart).value();
  {
    ManagedWorkerSystemCallFailure inject{ManagedWorkerSystemCall::poll, 0,
                                          EIO};
    const auto received = hangingWorker.receiveMessage(
        "fixture-run", 0, 2s, bte::core::ErrorCode::workerHandshakeTimeout);
    ASSERT_FALSE(received.ok());
    EXPECT_EQ(received.error().code,
              bte::core::ErrorCode::workerHandshakeFailed);
    EXPECT_EQ(received.error().message, "Could not poll managed worker pipes.");
  }
  ASSERT_TRUE(hangingWorker.terminateProcessTree(50ms).ok());
  EXPECT_FALSE(hangingWorker.isRunning());

  auto exitingStart = ManagedWorkerProcess::start(WorkerProcessLaunch{
      .executable = fixturePath(), .arguments = {"exit-output"}});
  ASSERT_TRUE(exitingStart.ok()) << exitingStart.error().message;
  auto exitingWorker = std::move(exitingStart).value();
  ASSERT_TRUE(exitingWorker
                  .sendMessage(request(), 2s,
                               bte::core::ErrorCode::workerHandshakeTimeout)
                  .ok());
  {
    ManagedWorkerSystemCallFailure inject{ManagedWorkerSystemCall::protocolRead,
                                          0, EIO};
    const auto received = exitingWorker.receiveMessage(
        "fixture-run", 0, 2s, bte::core::ErrorCode::workerHandshakeTimeout);
    ASSERT_FALSE(received.ok());
    EXPECT_EQ(received.error().code,
              bte::core::ErrorCode::workerHandshakeFailed);
    EXPECT_EQ(received.error().message,
              "Could not read a managed worker protocol frame.");
  }
  const auto terminated = exitingWorker.terminateProcessTree(0ms);
  ASSERT_TRUE(terminated.ok()) << terminated.error().message;
  EXPECT_FALSE(exitingWorker.isRunning());
#else
  GTEST_SKIP()
      << "POSIX worker process fixture is unavailable on this platform.";
#endif
}

TEST(ManagedWorkerProcessTest,
     classifiesProtocolWriteFailureAndLeavesTheWorkerTerminable) {
#if defined(__unix__) || defined(__APPLE__)
  auto started = ManagedWorkerProcess::start(
      WorkerProcessLaunch{.executable = fixturePath(), .arguments = {"hang"}});
  ASSERT_TRUE(started.ok()) << started.error().message;
  auto worker = std::move(started).value();
  {
    ManagedWorkerSystemCallFailure inject{
        ManagedWorkerSystemCall::protocolWrite, 0, EIO};
    const auto sent = worker.sendMessage(
        request(), 2s, bte::core::ErrorCode::workerHandshakeTimeout);
    ASSERT_FALSE(sent.ok());
    EXPECT_EQ(sent.error().code, bte::core::ErrorCode::workerHandshakeFailed);
    EXPECT_EQ(sent.error().message,
              "Could not write a managed worker protocol frame.");
  }
  ASSERT_TRUE(worker.terminateProcessTree(50ms).ok());
  EXPECT_FALSE(worker.isRunning());
#else
  GTEST_SKIP()
      << "POSIX worker process fixture is unavailable on this platform.";
#endif
}

TEST(ManagedWorkerProcessTest,
     retriesInterruptedPollDiagnosticReadAndProtocolIo) {
#if defined(__unix__) || defined(__APPLE__)
  for (const auto systemCall :
       {ManagedWorkerSystemCall::poll, ManagedWorkerSystemCall::diagnosticRead,
        ManagedWorkerSystemCall::protocolWrite,
        ManagedWorkerSystemCall::protocolRead}) {
    auto started = ManagedWorkerProcess::start(WorkerProcessLaunch{
        .executable = fixturePath(), .arguments = {"echo", "0"}});
    ASSERT_TRUE(started.ok()) << started.error().message;
    auto worker = std::move(started).value();

    if (systemCall == ManagedWorkerSystemCall::protocolRead) {
      ASSERT_TRUE(worker
                      .sendMessage(request(), 2s,
                                   bte::core::ErrorCode::workerHandshakeTimeout)
                      .ok());
    }
    {
      ManagedWorkerSystemCallFailure inject{systemCall, 0, EINTR};
      if (systemCall != ManagedWorkerSystemCall::protocolRead) {
        const auto sent = worker.sendMessage(
            request(), 2s, bte::core::ErrorCode::workerHandshakeTimeout);
        ASSERT_TRUE(sent.ok()) << sent.error().message;
      }
      const auto received = worker.receiveMessage(
          "fixture-run", 0, 2s, bte::core::ErrorCode::workerHandshakeTimeout);
      ASSERT_TRUE(received.ok()) << received.error().message;
      EXPECT_EQ(received.value().messageType, "ready");
    }
    ASSERT_TRUE(worker.waitForExit(2s).ok());
    const auto diagnostics = worker.diagnostics();
    EXPECT_TRUE(
        diagnostics.standardOutput.retainedBytes.starts_with("stdout-first\n"));
    EXPECT_TRUE(
        diagnostics.standardError.retainedBytes.starts_with("stderr-first\n"));
  }
#else
  GTEST_SKIP()
      << "POSIX worker process fixture is unavailable on this platform.";
#endif
}

TEST(ManagedWorkerProcessTest,
     continuesProtocolAfterDiagnosticPipeReadFailure) {
#if defined(__unix__) || defined(__APPLE__)
  auto started = ManagedWorkerProcess::start(WorkerProcessLaunch{
      .executable = fixturePath(), .arguments = {"echo", "0"}});
  ASSERT_TRUE(started.ok()) << started.error().message;
  auto worker = std::move(started).value();
  ASSERT_TRUE(worker
                  .sendMessage(request(), 2s,
                               bte::core::ErrorCode::workerHandshakeTimeout)
                  .ok());

  {
    ManagedWorkerSystemCallFailure inject{
        ManagedWorkerSystemCall::diagnosticRead, 0, EIO};
    const auto response = worker.receiveMessage(
        "fixture-run", 0, 2s, bte::core::ErrorCode::workerHandshakeTimeout);
    ASSERT_TRUE(response.ok()) << response.error().message;
    EXPECT_EQ(response.value().messageType, "ready");
  }
  ASSERT_TRUE(worker.waitForExit(2s).ok());
  const auto diagnostics = worker.diagnostics();
  EXPECT_TRUE(diagnostics.standardOutput.retainedBytes.empty());
  EXPECT_TRUE(
      diagnostics.standardError.retainedBytes.starts_with("stderr-first\n"));
#else
  GTEST_SKIP()
      << "POSIX worker process fixture is unavailable on this platform.";
#endif
}

TEST(ManagedWorkerProcessTest,
     reportsProcessTreeSignalFailureWithoutLosingTheWorkerHandle) {
#if defined(__unix__) || defined(__APPLE__)
  auto started = ManagedWorkerProcess::start(
      WorkerProcessLaunch{.executable = fixturePath(), .arguments = {"hang"}});
  ASSERT_TRUE(started.ok()) << started.error().message;
  auto worker = std::move(started).value();
  {
    ManagedWorkerSystemCallFailure inject{
        ManagedWorkerSystemCall::terminateSignal, 0, EPERM};
    const auto terminated = worker.terminateProcessTree(0ms);
    ASSERT_FALSE(terminated.ok());
    EXPECT_EQ(terminated.error().code,
              bte::core::ErrorCode::workerHandshakeFailed);
    EXPECT_EQ(terminated.error().message,
              "Could not request managed worker process-tree shutdown.");
  }
  EXPECT_TRUE(worker.isRunning());
  ASSERT_TRUE(worker.terminateProcessTree(50ms).ok());
  EXPECT_FALSE(worker.isRunning());
#else
  GTEST_SKIP()
      << "POSIX process-group support is unavailable on this platform.";
#endif
}

TEST(ManagedWorkerProcessTest,
     treatsPermissionDeniedProcessGroupProbeAsExisting) {
#if defined(__unix__) || defined(__APPLE__)
  auto started = ManagedWorkerProcess::start(
      WorkerProcessLaunch{.executable = fixturePath(),
                          .arguments = {"leader-exits-with-descendant"}});
  ASSERT_TRUE(started.ok()) << started.error().message;
  auto worker = std::move(started).value();
  ASSERT_TRUE(worker.waitForExit(2s).ok());

  {
    ManagedWorkerSystemCallFailure inject{
        ManagedWorkerSystemCall::processGroupProbe, 0, EPERM};
    EXPECT_TRUE(worker.isRunning());
  }
  ASSERT_TRUE(worker.terminateProcessTree(50ms).ok());
  EXPECT_FALSE(worker.isRunning());
#else
  GTEST_SKIP()
      << "POSIX process-group support is unavailable on this platform.";
#endif
}

TEST(ManagedWorkerProcessTest,
     reportsChildSignalFailureAndKeepsTheWorkerCleanupPathAvailable) {
#if defined(__unix__) || defined(__APPLE__)
  auto started = ManagedWorkerProcess::start(
      WorkerProcessLaunch{.executable = fixturePath(), .arguments = {"hang"}});
  ASSERT_TRUE(started.ok()) << started.error().message;
  auto worker = std::move(started).value();
  {
    ManagedWorkerSystemCallFailure inject{
        ManagedWorkerSystemCall::terminateChildSignal, 0, EPERM};
    const auto terminated = worker.terminateProcessTree(0ms);
    ASSERT_FALSE(terminated.ok());
    EXPECT_EQ(terminated.error().code,
              bte::core::ErrorCode::workerHandshakeFailed);
    EXPECT_NE(terminated.error().message.find(
                  "Could not terminate the managed worker process:"),
              std::string::npos);
  }
  ASSERT_TRUE(worker.terminateProcessTree(50ms).ok());
  EXPECT_FALSE(worker.isRunning());
#else
  GTEST_SKIP() << "POSIX process support is unavailable on this platform.";
#endif
}

TEST(ManagedWorkerProcessTest,
     retriesInterruptedChildWaitAndReportsOtherWaitErrors) {
#if defined(__unix__) || defined(__APPLE__)
  auto interruptedStart = ManagedWorkerProcess::start(
      WorkerProcessLaunch{.executable = fixturePath(), .arguments = {"hang"}});
  ASSERT_TRUE(interruptedStart.ok()) << interruptedStart.error().message;
  auto interruptedWorker = std::move(interruptedStart).value();
  {
    ManagedWorkerSystemCallFailure inject{ManagedWorkerSystemCall::waitForChild,
                                          0, EINTR};
    ASSERT_TRUE(interruptedWorker.terminateProcessTree(0ms).ok());
  }
  EXPECT_FALSE(interruptedWorker.isRunning());

  auto failedStart = ManagedWorkerProcess::start(
      WorkerProcessLaunch{.executable = fixturePath(), .arguments = {"hang"}});
  ASSERT_TRUE(failedStart.ok()) << failedStart.error().message;
  auto failedWorker = std::move(failedStart).value();
  {
    ManagedWorkerSystemCallFailure inject{ManagedWorkerSystemCall::waitForChild,
                                          0, EIO};
    const auto terminated = failedWorker.terminateProcessTree(0ms);
    ASSERT_FALSE(terminated.ok());
    EXPECT_EQ(terminated.error().code,
              bte::core::ErrorCode::workerHandshakeFailed);
    EXPECT_NE(terminated.error().message.find(
                  "Could not wait for the managed worker process:"),
              std::string::npos);
  }
  ASSERT_TRUE(failedWorker.terminateProcessTree(50ms).ok());
  EXPECT_FALSE(failedWorker.isRunning());
#else
  GTEST_SKIP() << "POSIX process support is unavailable on this platform.";
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

  const auto exited = [&] {
    ManagedWorkerSystemCallFailure inject{
        ManagedWorkerSystemCall::unexpectedProtocolRead, 0, EINTR};
    return worker.waitForExit(2s);
  }();

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

TEST(ManagedWorkerProcessTest,
     tracksSurvivingDescendantsAfterTheWorkerLeaderExits) {
#if defined(__unix__) || defined(__APPLE__)
  auto started = ManagedWorkerProcess::start(
      WorkerProcessLaunch{.executable = fixturePath(),
                          .arguments = {"leader-exits-with-descendant"}});
  ASSERT_TRUE(started.ok()) << started.error().message;
  auto worker = std::move(started).value();
  const auto group = worker.processId();
  ASSERT_GT(group, 0);
  ASSERT_TRUE(worker.waitForExit(2s).ok());

  const auto runningAfterLeaderExit = worker.isRunning();
  ASSERT_TRUE(worker.terminateProcessTree(50ms).ok());

  EXPECT_TRUE(runningAfterLeaderExit);
  EXPECT_FALSE(worker.isRunning());
  EXPECT_EQ(::kill(-group, 0), -1);
  EXPECT_EQ(errno, ESRCH);
#else
  GTEST_SKIP()
      << "POSIX process-group support is unavailable on this platform.";
#endif
}

} // namespace
