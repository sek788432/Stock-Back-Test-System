#pragma once

#include "ManagedWorkerProcess.h"
#include "RuntimeProfile.h"

#include <Bte/Core/Bar.h>
#include <Bte/Core/Cancellation.h>
#include <Bte/Core/Result.h>
#include <Bte/Strategy/Strategy.h>

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <nlohmann/json_fwd.hpp>
#include <span>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace bte::bindings::python {

struct PythonStrategyDiagnostics {
  WorkerDiagnostics streams;
  std::vector<nlohmann::json> structuredLogs;
  std::vector<nlohmann::json> structuredErrors;
  std::uint64_t omittedLogEntries = 0;
  std::uint64_t omittedLogBytes = 0;
  bool complete = true;
};

class PythonWorkerStrategy final : public strategy::Strategy {
private:
  struct Impl;

public:
  ~PythonWorkerStrategy() override;
  PythonWorkerStrategy(PythonWorkerStrategy &&) = delete;
  PythonWorkerStrategy &operator=(PythonWorkerStrategy &&) = delete;
  PythonWorkerStrategy(const PythonWorkerStrategy &) = delete;
  PythonWorkerStrategy &operator=(const PythonWorkerStrategy &) = delete;

  [[nodiscard]] static core::Result<std::unique_ptr<strategy::Strategy>>
  create(const RuntimeProfile &verifiedRuntime,
         const std::filesystem::path &workerBootstrapPath, std::string source,
         nlohmann::json parameters, bool sourceConsentGranted,
         const core::CancellationToken &cancellation = {},
         std::span<const core::Bar> warmupHistory = {});

#if defined(BTE_ENABLE_TEST_HOOKS)
  [[nodiscard]] static core::Result<std::unique_ptr<strategy::Strategy>>
  createForTesting(const std::filesystem::path &pythonExecutable,
                   const std::filesystem::path &workerBootstrapPath,
                   std::string source, nlohmann::json parameters,
                   bool sourceConsentGranted, bool skipNumpySeedForTest = true,
                   const core::CancellationToken &cancellation = {},
                   std::span<const core::Bar> warmupHistory = {},
                   std::filesystem::path timezoneDatabasePath = {});
#endif

  [[nodiscard]] core::Result<void>
  onStart(const strategy::StrategyContext &context,
          const core::CancellationToken &cancellation) override;

  [[nodiscard]] const strategy::StrategyRequirements &
  requirements() const noexcept override;

  [[nodiscard]] core::Result<strategy::StrategyCommandBatch>
  onMarket(const strategy::StrategyContext &context,
           const core::CancellationToken &cancellation) override;

  [[nodiscard]] core::Result<void>
  onFill(const strategy::StrategyFill &fill,
         const strategy::StrategyContext &context,
         const core::CancellationToken &cancellation) override;

  [[nodiscard]] core::Result<void>
  onFinish(const strategy::StrategyContext &context,
           const core::CancellationToken &cancellation) override;
  void onCancel() noexcept override;
  [[nodiscard]] strategy::StrategyDiagnosticBatch takeDiagnostics() override;

  [[nodiscard]] PythonStrategyDiagnostics diagnostics() const;

  /// Internal construction seam; the implementation type is intentionally
  /// private so application callers use `create` instead.
  explicit PythonWorkerStrategy(std::unique_ptr<Impl> impl) noexcept;

private:
  [[nodiscard]] static core::Result<std::unique_ptr<strategy::Strategy>>
  createInternal(const std::filesystem::path &pythonExecutable,
                 const std::filesystem::path &workerBootstrapPath,
                 std::string source, nlohmann::json parameters,
                 bool sourceConsentGranted, bool verifyRuntime,
                 bool skipNumpySeedForTest,
                 const core::CancellationToken &cancellation,
                 std::span<const core::Bar> warmupHistory,
                 std::filesystem::path timezoneDatabasePath);

  std::unique_ptr<Impl> impl_;
};

} // namespace bte::bindings::python
