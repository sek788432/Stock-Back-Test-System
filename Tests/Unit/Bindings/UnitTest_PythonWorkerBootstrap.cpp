#include "ManagedWorkerProcess.h"

#include <Bte/Core/Digest.h>

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>

namespace {

using namespace std::chrono_literals;
using bte::bindings::python::ManagedWorkerProcess;
using bte::bindings::python::ProtocolMessage;
using bte::bindings::python::ReadOnlySharedMemory;
using bte::bindings::python::WorkerProcessLaunch;

#if defined(__unix__) || defined(__APPLE__)
class WorkerHarness final {
public:
  [[nodiscard]] static bte::core::Result<WorkerHarness>
  start(const std::string_view source) {
    auto sharedResult = ReadOnlySharedMemory::create(source);
    if (!sharedResult.ok()) {
      return sharedResult.error();
    }
    auto shared = std::move(sharedResult).value();
    auto processResult = ManagedWorkerProcess::start(WorkerProcessLaunch{
        .executable = BTE_TEST_PYTHON_EXECUTABLE,
        .arguments = {"-I", "-B", BTE_PYTHON_WORKER_BOOTSTRAP_PATH,
                      "--run-id=python-bootstrap-test",
                      "--test-only-skip-numpy-seed"},
        .readOnlyDataDescriptor = shared.descriptor(),
        .readOnlyDataLength = shared.size(),
        .readOnlyDataSha256 = shared.sha256Digest(),
    });
    if (!processResult.ok()) {
      return processResult.error();
    }
    return WorkerHarness{std::move(processResult).value(), std::move(shared)};
  }

  WorkerHarness(ManagedWorkerProcess process, ReadOnlySharedMemory source)
      : process_(std::move(process)), source_(std::move(source)) {}

  [[nodiscard]] bte::core::Result<void> handshake() {
    const auto ready =
        process_.receiveMessage(runId_, outputSequence_, 10s,
                                bte::core::ErrorCode::workerBootstrapTimeout);
    if (!ready.ok()) {
      return ready.error();
    }
    if (ready.value().messageType != "bootstrap-ready") {
      return bte::core::makeError(bte::core::ErrorCode::workerStartFailed,
                                  "worker did not report bootstrap-ready");
    }
    ++outputSequence_;
    const auto sent =
        send("handshake", nlohmann::json{{"stockbtApiVersion", "1"},
                                         {"protocolVersion", 1}});
    if (!sent.ok()) {
      return sent.error();
    }
    const auto response =
        process_.receiveMessage(runId_, outputSequence_, 5s,
                                bte::core::ErrorCode::workerHandshakeTimeout);
    if (!response.ok()) {
      return response.error();
    }
    if (response.value().messageType != "handshake-complete") {
      return bte::core::makeError(bte::core::ErrorCode::workerHandshakeFailed,
                                  "worker handshake response is invalid");
    }
    ++outputSequence_;
    return {};
  }

  [[nodiscard]] bte::core::Result<void>
  loadModule(const std::string_view sourceHash) {
    return loadModule(sourceHash, 10s,
                      bte::core::ErrorCode::strategyLoadTimeout);
  }

  [[nodiscard]] bte::core::Result<void>
  loadModule(const std::string_view sourceHash,
             const std::chrono::milliseconds timeout,
             const bte::core::ErrorCode timeoutCode) {
    const auto sent =
        send("load-module", nlohmann::json{{"sourceSha256", sourceHash},
                                           {"stockbtApiVersion", "1"}});
    if (!sent.ok()) {
      return sent.error();
    }
    const auto response =
        process_.receiveMessage(runId_, outputSequence_, timeout, timeoutCode);
    if (!response.ok()) {
      return response.error();
    }
    if (response.value().messageType == "worker-error") {
      return bte::core::makeError(
          bte::core::ErrorCode::strategyLoadFailed,
          response.value().payload.at("message").get<std::string>());
    }
    if (response.value().messageType != "module-loaded") {
      return bte::core::makeError(bte::core::ErrorCode::strategyLoadFailed,
                                  "worker module response is invalid");
    }
    ++outputSequence_;
    return {};
  }

  [[nodiscard]] bte::core::Result<void>
  createStrategy(const nlohmann::json &parameters) {
    const auto sent =
        send("create-strategy", nlohmann::json{{"parameters", parameters}});
    if (!sent.ok()) {
      return sent.error();
    }
    const auto response =
        process_.receiveMessage(runId_, outputSequence_, 10s,
                                bte::core::ErrorCode::strategyFactoryTimeout);
    if (!response.ok()) {
      return response.error();
    }
    if (response.value().messageType == "worker-error") {
      return bte::core::makeError(
          response.value().payload.at("errorCode").get<std::string>() ==
                  "StrategyFactoryFailed"
              ? bte::core::ErrorCode::strategyFactoryFailed
              : bte::core::ErrorCode::strategyContractViolation,
          response.value().payload.at("message").get<std::string>());
    }
    if (response.value().messageType != "strategy-loaded") {
      return bte::core::makeError(bte::core::ErrorCode::strategyFactoryFailed,
                                  "worker factory response is invalid");
    }
    ++outputSequence_;
    return {};
  }

  [[nodiscard]] bte::core::Result<ProtocolMessage>
  callback(const std::string_view hook, const nlohmann::json &context,
           const std::chrono::milliseconds timeout = 30s,
           const bte::core::ErrorCode timeoutCode =
               bte::core::ErrorCode::strategyHookTimeout,
           const nlohmann::json &additional = nlohmann::json::object()) {
    auto payload = additional;
    payload["context"] = context;
    payload["hook"] = hook;
    const auto sent = send("callback", payload);
    if (!sent.ok()) {
      return sent.error();
    }
    auto response =
        process_.receiveMessage(runId_, outputSequence_, timeout, timeoutCode);
    if (response.ok()) {
      ++outputSequence_;
    }
    return response;
  }

  [[nodiscard]] bte::core::Result<void> shutdown() {
    const auto sent = send("shutdown", nlohmann::json::object());
    if (!sent.ok()) {
      return sent.error();
    }
    const auto response =
        process_.receiveMessage(runId_, outputSequence_, 2s,
                                bte::core::ErrorCode::workerHandshakeTimeout);
    if (!response.ok()) {
      return response.error();
    }
    if (response.value().messageType != "shutdown-complete") {
      return bte::core::makeError(
          bte::core::ErrorCode::strategyProtocolViolation,
          "worker shutdown response is invalid");
    }
    ++outputSequence_;
    return process_.waitForExit(2s);
  }

  [[nodiscard]] ManagedWorkerProcess &process() noexcept { return process_; }

private:
  [[nodiscard]] bte::core::Result<void> send(std::string_view type,
                                             nlohmann::json payload) {
    const auto result =
        process_.sendMessage(ProtocolMessage{.protocolVersion = 1,
                                             .runId = std::string{runId_},
                                             .sequence = inputSequence_,
                                             .messageType = std::string{type},
                                             .payload = std::move(payload)},
                             5s, bte::core::ErrorCode::workerHandshakeTimeout);
    if (result.ok()) {
      ++inputSequence_;
    }
    return result;
  }

  static constexpr std::string_view runId_ = "python-bootstrap-test";
  ManagedWorkerProcess process_;
  ReadOnlySharedMemory source_;
  std::uint64_t inputSequence_ = 0;
  std::uint64_t outputSequence_ = 0;
};

[[nodiscard]] std::string sourceDigest(const std::string_view source) {
  return bte::core::sha256(source);
}

[[nodiscard]] nlohmann::json contextValue() {
  return nlohmann::json{
      {"bar", nlohmann::json{{"timestamp_utc_ms", 1'700'000'000'000LL},
                             {"open", 100.0},
                             {"high", 102.0},
                             {"low", 99.0},
                             {"close", 101.0},
                             {"volume", 1234.0}}},
      {"bars", nlohmann::json::object()},
      {"indicators", nlohmann::json::object()},
      {"portfolio", nlohmann::json{{"cash", 10000.0},
                                   {"equity", 10000.0},
                                   {"positions", nlohmann::json::object()}}},
      {"symbol", "AAPL"},
      {"timestamp_utc_ms", 1'700'000'000'000LL},
  };
}

const std::string validStrategySource = R"python(
import math

STOCKBT_API_VERSION = "1"
STOCKBT_TEMPLATE_VERSION = "1.0.0"

class Example:
    max_history_bars = 1
    indicators = {}

    def __init__(self):
        self.started = False

    def on_start(self, ctx):
        self.started = ctx.timestamp_utc_ms is None

    def on_market(self, ctx):
        assert self.started
        assert math.isclose(ctx.bar.close, 101.0)
        assert ctx.portfolio.positions.get(ctx.symbol) is None
        ctx.orders.buy_market(ctx.symbol, quantity=2)
        ctx.log.info("entry signal", timestamp=ctx.timestamp_utc_ms)

    def on_fill(self, fill, ctx):
        ctx.log.info("fill observed", order_id=fill["order_id"])

    def on_finish(self, ctx):
        assert ctx.timestamp_utc_ms is not None

def create_strategy(params):
    assert params.get("quantity") == 2
    return Example()
)python";

[[nodiscard]] bte::core::Result<WorkerHarness>
startLoadedWorker(const std::string_view source) {
  auto workerResult = WorkerHarness::start(source);
  if (!workerResult.ok()) {
    return workerResult.error();
  }
  auto worker = std::move(workerResult).value();
  const auto handshake = worker.handshake();
  if (!handshake.ok()) {
    return handshake.error();
  }
  const auto module = worker.loadModule(sourceDigest(source));
  if (!module.ok()) {
    return module.error();
  }
  const auto factory = worker.createStrategy(nlohmann::json{{"quantity", 2}});
  if (!factory.ok()) {
    return factory.error();
  }
  return std::move(worker);
}

TEST(PythonWorkerBootstrapTest,
     loadsTrustedModuleAndRunsTypedLifecycleCommands) {
  auto workerResult = startLoadedWorker(validStrategySource);
  ASSERT_TRUE(workerResult.ok()) << workerResult.error().message;
  auto worker = std::move(workerResult).value();

  auto onStart = worker.callback(
      "on_start", nlohmann::json{{"bar", nullptr},
                                 {"bars", nlohmann::json::object()},
                                 {"indicators", nlohmann::json::object()},
                                 {"portfolio", contextValue().at("portfolio")},
                                 {"symbol", "AAPL"},
                                 {"timestamp_utc_ms", nullptr}});
  ASSERT_TRUE(onStart.ok()) << onStart.error().message;
  ASSERT_EQ(onStart.value().messageType, "callback-result");
  EXPECT_TRUE(onStart.value().payload.at("commands").empty());

  const auto onMarket = worker.callback("on_market", contextValue());
  ASSERT_TRUE(onMarket.ok()) << onMarket.error().message;
  ASSERT_EQ(onMarket.value().messageType, "callback-result");
  ASSERT_EQ(onMarket.value().payload.at("commands").size(), 1U);
  const auto command = onMarket.value().payload.at("commands").at(0);
  EXPECT_EQ(command.at("kind"), "buy_market");
  EXPECT_EQ(command.at("symbol"), "AAPL");
  EXPECT_EQ(command.at("quantity_shares"), 2);
  ASSERT_EQ(onMarket.value().payload.at("logs").size(), 1U);
  EXPECT_EQ(onMarket.value().payload.at("logs").at(0).at("message"),
            "entry signal");

  auto fillContext = contextValue();
  fillContext["timestamp_utc_ms"] = 1'700'000'000'000LL;
  const auto onFill = worker.callback(
      "on_fill", fillContext, 30s, bte::core::ErrorCode::strategyHookTimeout,
      nlohmann::json{{"fill", nlohmann::json{{"order_id", "fill-1"}}}});
  ASSERT_TRUE(onFill.ok()) << onFill.error().message;
  ASSERT_EQ(onFill.value().messageType, "callback-result");
  EXPECT_TRUE(onFill.value().payload.at("commands").empty());

  const auto onFinish = worker.callback("on_finish", fillContext);
  ASSERT_TRUE(onFinish.ok()) << onFinish.error().message;
  EXPECT_EQ(onFinish.value().messageType, "callback-result");
  EXPECT_TRUE(worker.shutdown().ok());
}

TEST(PythonWorkerBootstrapTest,
     rejectsForbiddenImportsBeforeExecutingTheModule) {
  const auto marker = std::filesystem::temp_directory_path() /
                      "bte-python-forbidden-import-marker";
  std::error_code ignored;
  std::filesystem::remove(marker, ignored);
  const std::string source = "open(" + nlohmann::json(marker.string()).dump() +
                             ", 'w').write('executed')\n"
                             "import os\n"
                             "STOCKBT_API_VERSION = '1'\n"
                             "STOCKBT_TEMPLATE_VERSION = '1.0.0'\n";
  auto workerResult = WorkerHarness::start(source);
  ASSERT_TRUE(workerResult.ok()) << workerResult.error().message;
  auto worker = std::move(workerResult).value();
  ASSERT_TRUE(worker.handshake().ok());

  const auto sent = worker.process().sendMessage(
      ProtocolMessage{.protocolVersion = 1,
                      .runId = "python-bootstrap-test",
                      .sequence = 1,
                      .messageType = "load-module",
                      .payload =
                          nlohmann::json{{"sourceSha256", sourceDigest(source)},
                                         {"stockbtApiVersion", "1"}}},
      5s, bte::core::ErrorCode::strategyLoadTimeout);
  ASSERT_TRUE(sent.ok()) << sent.error().message;
  const auto rejected = worker.process().receiveMessage(
      "python-bootstrap-test", 2, 5s,
      bte::core::ErrorCode::strategyLoadTimeout);
  ASSERT_TRUE(rejected.ok()) << rejected.error().message;
  EXPECT_EQ(rejected.value().messageType, "worker-error");
  EXPECT_EQ(rejected.value().payload.at("errorCode"),
            "StrategyImportNotAllowed");
  EXPECT_FALSE(std::filesystem::exists(marker));
  std::filesystem::remove(marker, ignored);
}

TEST(PythonWorkerBootstrapTest,
     rejectsReflectiveDynamicImportsBeforeExecutingTheModule) {
  const std::array<std::string, 3> dynamicImports{
      "getattr(__builtins__, '__import__')('os')",
      "vars(__builtins__)['__import__']('os')",
      "__builtins__.__import__('os')",
  };
  for (std::size_t index = 0; index < dynamicImports.size(); ++index) {
    const auto marker =
        std::filesystem::temp_directory_path() /
        ("bte-python-reflective-import-marker-" + std::to_string(index));
    std::error_code ignored;
    std::filesystem::remove(marker, ignored);
    const auto source = "open(" + nlohmann::json(marker.string()).dump() +
                        ", 'w').write('executed')\n" + dynamicImports[index] +
                        "\nSTOCKBT_API_VERSION = '1'\n"
                        "STOCKBT_TEMPLATE_VERSION = '1.0.0'\n";
    auto workerResult = WorkerHarness::start(source);
    ASSERT_TRUE(workerResult.ok()) << workerResult.error().message;
    auto worker = std::move(workerResult).value();
    ASSERT_TRUE(worker.handshake().ok());

    const auto sent = worker.process().sendMessage(
        ProtocolMessage{
            .protocolVersion = 1,
            .runId = "python-bootstrap-test",
            .sequence = 1,
            .messageType = "load-module",
            .payload = nlohmann::json{{"sourceSha256", sourceDigest(source)},
                                      {"stockbtApiVersion", "1"}}},
        5s, bte::core::ErrorCode::strategyLoadTimeout);
    ASSERT_TRUE(sent.ok()) << sent.error().message;
    const auto rejected = worker.process().receiveMessage(
        "python-bootstrap-test", 2, 5s,
        bte::core::ErrorCode::strategyLoadTimeout);
    ASSERT_TRUE(rejected.ok()) << rejected.error().message;
    EXPECT_EQ(rejected.value().messageType, "worker-error");
    EXPECT_EQ(rejected.value().payload.at("errorCode"),
              "StrategyImportNotAllowed");
    EXPECT_FALSE(std::filesystem::exists(marker));
    std::filesystem::remove(marker, ignored);
  }
}

TEST(PythonWorkerBootstrapTest,
     parentDeadlinesStopInfiniteModuleFactoryAndHookCode) {
  const std::string moduleLoop = "while True: pass\n" + validStrategySource;
  auto moduleWorkerResult = WorkerHarness::start(moduleLoop);
  ASSERT_TRUE(moduleWorkerResult.ok()) << moduleWorkerResult.error().message;
  auto moduleWorker = std::move(moduleWorkerResult).value();
  ASSERT_TRUE(moduleWorker.handshake().ok());
  const auto moduleTimeout =
      moduleWorker.loadModule(sourceDigest(moduleLoop), 80ms,
                              bte::core::ErrorCode::strategyLoadTimeout);
  ASSERT_FALSE(moduleTimeout.ok());
  EXPECT_EQ(moduleTimeout.error().code,
            bte::core::ErrorCode::strategyLoadTimeout);
  EXPECT_TRUE(moduleWorker.process().terminateProcessTree(50ms).ok());

  const std::string factoryLoop = R"python(
STOCKBT_API_VERSION = "1"
STOCKBT_TEMPLATE_VERSION = "1.0.0"
def create_strategy(params):
    while True: pass
)python";
  auto factoryWorkerResult = WorkerHarness::start(factoryLoop);
  ASSERT_TRUE(factoryWorkerResult.ok()) << factoryWorkerResult.error().message;
  auto factoryWorker = std::move(factoryWorkerResult).value();
  ASSERT_TRUE(factoryWorker.handshake().ok());
  ASSERT_TRUE(factoryWorker.loadModule(sourceDigest(factoryLoop)).ok());
  const auto factoryTimeout =
      factoryWorker.createStrategy(nlohmann::json::object());
  EXPECT_FALSE(factoryTimeout.ok());
  EXPECT_EQ(factoryTimeout.error().code,
            bte::core::ErrorCode::strategyFactoryTimeout);
  EXPECT_TRUE(factoryWorker.process().terminateProcessTree(50ms).ok());

  const std::string hookLoop = R"python(
STOCKBT_API_VERSION = "1"
STOCKBT_TEMPLATE_VERSION = "1.0.0"
class Example:
    max_history_bars = 1
    indicators = {}
    def on_market(self, ctx):
        while True: pass
def create_strategy(params):
    return Example()
)python";
  auto hookWorkerResult = startLoadedWorker(hookLoop);
  ASSERT_TRUE(hookWorkerResult.ok()) << hookWorkerResult.error().message;
  auto hookWorker = std::move(hookWorkerResult).value();
  const auto hookTimeout =
      hookWorker.callback("on_market", contextValue(), 80ms,
                          bte::core::ErrorCode::strategyHookTimeout);
  EXPECT_FALSE(hookTimeout.ok());
  EXPECT_EQ(hookTimeout.error().code,
            bte::core::ErrorCode::strategyHookTimeout);
  EXPECT_TRUE(hookWorker.process().terminateProcessTree(50ms).ok());
}

#else

TEST(PythonWorkerBootstrapTest, posixWorkerUnavailableOnThisPlatform) {
  GTEST_SKIP() << "Python worker bootstrap requires POSIX descriptor passing.";
}

#endif

} // namespace
