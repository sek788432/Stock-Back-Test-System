#pragma once

#include "StrategyProtocol.h"

#include <Bte/Core/Cancellation.h>
#include <Bte/Core/Result.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace bte::bindings::python {

inline constexpr std::size_t maximumWorkerDiagnosticBytes =
    std::size_t{256} * 1024;
inline constexpr std::size_t maximumWorkerSourceBytes =
    std::size_t{1024} * 1024;

struct CapturedDiagnosticStream {
  std::string retainedBytes;
  std::uint64_t bytesSeen = 0;
  std::uint64_t omittedBytes = 0;
};

struct WorkerDiagnostics {
  CapturedDiagnosticStream standardOutput;
  CapturedDiagnosticStream standardError;
};

class ReadOnlySharedMemory final {
public:
  ReadOnlySharedMemory() noexcept;
  ~ReadOnlySharedMemory();
  ReadOnlySharedMemory(ReadOnlySharedMemory &&other) noexcept;
  ReadOnlySharedMemory &operator=(ReadOnlySharedMemory &&other) noexcept;
  ReadOnlySharedMemory(const ReadOnlySharedMemory &) = delete;
  ReadOnlySharedMemory &operator=(const ReadOnlySharedMemory &) = delete;

  [[nodiscard]] static core::Result<ReadOnlySharedMemory>
  create(std::string_view bytes);

  [[nodiscard]] int descriptor() const noexcept;
  [[nodiscard]] std::uint64_t size() const noexcept;
  [[nodiscard]] const std::string &sha256Digest() const noexcept;

private:
  ReadOnlySharedMemory(int descriptor, std::string digest,
                       std::uint64_t size) noexcept;
  void reset() noexcept;

  int descriptor_ = -1;
  std::uint64_t size_ = 0;
  std::string digest_;
};

struct WorkerProcessLaunch {
  std::filesystem::path executable;
  std::vector<std::string> arguments;
  std::filesystem::path timezoneDatabasePath;
  int readOnlyDataDescriptor = -1;
  std::uint64_t readOnlyDataLength = 0;
  std::string readOnlyDataSha256;
  int readOnlyHistoryDescriptor = -1;
  std::uint64_t readOnlyHistoryLength = 0;
  std::string readOnlyHistorySha256;
};

class ManagedWorkerProcess final {
public:
  ManagedWorkerProcess() noexcept;
  ~ManagedWorkerProcess();
  ManagedWorkerProcess(ManagedWorkerProcess &&other) noexcept;
  ManagedWorkerProcess &operator=(ManagedWorkerProcess &&other) noexcept;
  ManagedWorkerProcess(const ManagedWorkerProcess &) = delete;
  ManagedWorkerProcess &operator=(const ManagedWorkerProcess &) = delete;

  [[nodiscard]] static core::Result<ManagedWorkerProcess>
  start(const WorkerProcessLaunch &launch);

  [[nodiscard]] core::Result<void>
  sendMessage(const ProtocolMessage &message, std::chrono::milliseconds timeout,
              core::ErrorCode timeoutCode,
              const core::CancellationToken &cancellation = {});

  [[nodiscard]] core::Result<ProtocolMessage>
  receiveMessage(std::string_view expectedRunId, std::uint64_t expectedSequence,
                 std::chrono::milliseconds timeout, core::ErrorCode timeoutCode,
                 const core::CancellationToken &cancellation = {});

  [[nodiscard]] core::Result<void>
  waitForExit(std::chrono::milliseconds timeout);
  [[nodiscard]] core::Result<void> terminateProcessTree(
      std::chrono::milliseconds gracePeriod = std::chrono::seconds{2});

  [[nodiscard]] bool isRunning() const noexcept;
  [[nodiscard]] int processId() const noexcept;
  [[nodiscard]] WorkerDiagnostics diagnostics() const;

private:
  struct Impl;
  explicit ManagedWorkerProcess(std::unique_ptr<Impl> impl) noexcept;

  std::unique_ptr<Impl> impl_;
};

} // namespace bte::bindings::python
