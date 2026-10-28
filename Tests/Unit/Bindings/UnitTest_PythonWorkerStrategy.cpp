#include "ManagedWorkerProcessTestHooks.h"
#include "PythonWorkerStrategy.h"
#include "RuntimeProfile.h"

#include <Bte/Core/CanonicalJson.h>
#include <Bte/Core/Digest.h>
#include <Bte/Core/Time.h>
#include <Bte/Data/ReleaseSnapshot.h>
#include <Bte/Engine/Backtest.h>
#include <Bte/Results/ResultStore.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <ranges>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#if defined(BTE_TEST_PYTHON_RESULT_REPLAY)
#include <Bte/Bindings/ResultReplay.h>
#include <QTemporaryDir>
#endif

namespace {

using bte::bindings::python::PythonWorkerStrategy;
using bte::strategy::StrategyCommandKind;

const std::string validStrategySource = R"python(
STOCKBT_API_VERSION = "1"
STOCKBT_TEMPLATE_VERSION = "1.0.0"

class Example:
    max_history_bars = 1
    indicators = {}

    def on_start(self, ctx):
        ctx.log.info("started", symbol=ctx.symbol)

    def on_market(self, ctx):
        ctx.log.info("market", close=ctx.bar.close)
        if ctx.timestamp_utc_ms == 1700000000000:
            ctx.orders.buy_market(ctx.symbol, quantity=2)

    def on_fill(self, fill, ctx):
        ctx.log.info("fill", side=fill["side"], quantity=fill["quantity_shares"])

    def on_finish(self, ctx):
        ctx.log.info("finished", position=ctx.portfolio.positions.get(ctx.symbol) is not None)

def create_strategy(params):
    assert params.get("fast_period") == 2
    assert params.get("slow_period") == 3
    assert params.get("quantity") == 2
    return Example()
)python";

const std::string parameterBoundaryStrategySource = R"python(
STOCKBT_API_VERSION = "1"
STOCKBT_TEMPLATE_VERSION = "1.0.0"

class Example:
    max_history_bars = 1
    indicators = {}

    def on_market(self, ctx):
        return None

def create_strategy(params):
    return Example()
)python";

bte::strategy::StrategyContext context(const std::int64_t timestamp,
                                       const std::int64_t position = 0) {
  using bte::core::Bar;
  using bte::core::Timestamp;
  return {.symbol = "AAPL",
          .bar = Bar{.ts = Timestamp{std::chrono::milliseconds{timestamp}},
                     .open = 10.0,
                     .high = 11.0,
                     .low = 9.0,
                     .close = 10.5,
                     .volume = 123.0},
          .cashMicrodollars = 12'345'678,
          .equityMicrodollars = 13'456'789,
          .positionShares = position,
          .markPriceNanodollars = 10'500'000'000};
}

nlohmann::json parameters() {
  return nlohmann::json{
      {"fast_period", 2}, {"slow_period", 3}, {"quantity", 2}};
}

#if defined(BTE_TEST_MANAGED_RUNTIME_PROFILE_PATH)
TEST(PythonWorkerStrategyTest,
     loadsTheProjectOwnedStockbtFromTheManagedProfile) {
  const auto runtime = bte::bindings::python::verifyRuntimeProfile(
      BTE_TEST_MANAGED_RUNTIME_PROFILE_PATH);
  ASSERT_TRUE(runtime.ok()) << runtime.error().message;
  const std::string source = R"python(
import stockbt
STOCKBT_API_VERSION = "1"
STOCKBT_TEMPLATE_VERSION = "1.0.0"
class Example:
    max_history_bars = 1
    indicators = {"fast": stockbt.SMA("fast", period=1)}
    def on_market(self, ctx):
        return

def create_strategy(params):
    return Example()
)python";

  const auto strategy = PythonWorkerStrategy::create(
      runtime.value(), BTE_PYTHON_WORKER_BOOTSTRAP_PATH, source, parameters(),
      true);

  ASSERT_TRUE(strategy.ok()) << strategy.error().message;
  EXPECT_NE(strategy.value(), nullptr);
}
#endif

#if defined(__unix__) || defined(__APPLE__)
TEST(PythonWorkerStrategyTest, runsLifecycleThroughTypedStrategySeam) {
  auto strategyResult = PythonWorkerStrategy::createForTesting(
      BTE_TEST_PYTHON_EXECUTABLE, BTE_PYTHON_WORKER_BOOTSTRAP_PATH,
      validStrategySource, parameters(), true);
  ASSERT_TRUE(strategyResult.ok()) << strategyResult.error().message;
  auto strategy = std::move(strategyResult).value();

  const bte::strategy::StrategyContext startContext{
      .symbol = "AAPL",
      .bar = std::nullopt,
      .cashMicrodollars = 100'000'000,
      .equityMicrodollars = 100'000'000,
      .positionShares = 0,
      .markPriceNanodollars = std::nullopt};
  const auto started = strategy->onStart(startContext);
  ASSERT_TRUE(started.ok()) << started.error().message;

  const auto market = strategy->onMarket(context(1'700'000'000'000LL));
  ASSERT_TRUE(market.ok()) << market.error().message;
  ASSERT_EQ(market.value().size(), 1U);
  EXPECT_EQ(market.value().front().kind, StrategyCommandKind::buyMarket);
  EXPECT_EQ(market.value().front().symbol, "AAPL");
  EXPECT_EQ(market.value().front().quantityShares, 2);

  const bte::strategy::StrategyFill fill{
      .timestamp =
          std::chrono::sys_time<std::chrono::milliseconds>{
              std::chrono::milliseconds{1'700'000'000'000LL}},
      .side = bte::strategy::StrategyFillSide::buy,
      .quantityShares = 2,
      .priceNanodollars = 10'500'000'000,
      .amountMicrodollars = 21'000'000};
  ASSERT_TRUE(strategy->onFill(fill, context(1'700'000'000'000LL, 2)).ok());
  ASSERT_TRUE(strategy->onFinish(context(1'700'000'003'600'000LL, 2)).ok());

  const auto *pythonStrategy =
      dynamic_cast<PythonWorkerStrategy *>(strategy.get());
  ASSERT_NE(pythonStrategy, nullptr);
  const auto diagnostics = pythonStrategy->diagnostics();
  EXPECT_GE(diagnostics.structuredLogs.size(), 4U);
  EXPECT_EQ(diagnostics.structuredLogs.front().at("message"), "started");
  EXPECT_EQ(diagnostics.streams.standardError.bytesSeen, 0U);
}

TEST(PythonWorkerStrategyTest, cancellationInterruptsModuleStartup) {
  bte::core::CancellationSource cancellation;
  auto creation = std::async(std::launch::async, [&cancellation] {
    return PythonWorkerStrategy::createForTesting(
        BTE_TEST_PYTHON_EXECUTABLE, BTE_PYTHON_WORKER_BOOTSTRAP_PATH,
        "while True:\n    pass\n", parameters(), true, true,
        cancellation.token());
  });
  EXPECT_EQ(creation.wait_for(std::chrono::milliseconds{500}),
            std::future_status::timeout);
  cancellation.requestCancellation();
  ASSERT_EQ(creation.wait_for(std::chrono::seconds{3}),
            std::future_status::ready)
      << "canceling module startup must terminate the worker promptly";
  auto result = creation.get();
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().code, bte::core::ErrorCode::cancelled);
}

TEST(PythonWorkerStrategyTest, cancellationInterruptsStrategyFactoryStartup) {
  bte::core::CancellationSource cancellation;
  const std::string source = R"python(
STOCKBT_API_VERSION = "1"
STOCKBT_TEMPLATE_VERSION = "1.0.0"

def create_strategy(params):
    while True:
        pass
)python";
  auto creation = std::async(std::launch::async, [&cancellation, &source] {
    return PythonWorkerStrategy::createForTesting(
        BTE_TEST_PYTHON_EXECUTABLE, BTE_PYTHON_WORKER_BOOTSTRAP_PATH, source,
        parameters(), true, true, cancellation.token());
  });
  EXPECT_EQ(creation.wait_for(std::chrono::milliseconds{500}),
            std::future_status::timeout);
  cancellation.requestCancellation();
  ASSERT_EQ(creation.wait_for(std::chrono::seconds{3}),
            std::future_status::ready)
      << "canceling strategy factory startup must terminate the worker "
         "promptly";
  auto result = creation.get();
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().code, bte::core::ErrorCode::cancelled);
}

TEST(PythonWorkerStrategyTest, usesOnlyTheConfiguredTimezoneDatabasePath) {
  const auto timezonePath =
      std::filesystem::temp_directory_path() /
      ("bte-python-timezone-test-" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));
  std::filesystem::create_directories(timezonePath);
  struct DirectoryCleanup final {
    std::filesystem::path path;
    ~DirectoryCleanup() {
      std::error_code ignored;
      std::filesystem::remove_all(path, ignored);
    }
  } cleanup{timezonePath};

  const std::string source = R"python(
import zoneinfo
STOCKBT_API_VERSION = "1"
STOCKBT_TEMPLATE_VERSION = "1.0.0"
class TimezoneProbe:
    max_history_bars = 1
    indicators = {}
    def on_market(self, ctx):
        ctx.log.info("timezone-path", paths=list(zoneinfo.TZPATH))
def create_strategy(params):
    return TimezoneProbe()
)python";
  auto strategyResult = PythonWorkerStrategy::createForTesting(
      BTE_TEST_PYTHON_EXECUTABLE, BTE_PYTHON_WORKER_BOOTSTRAP_PATH, source,
      parameters(), true, true, {}, {}, timezonePath);
  ASSERT_TRUE(strategyResult.ok()) << strategyResult.error().message;
  auto strategy = std::move(strategyResult).value();
  const auto result = strategy->onMarket(context(1'700'000'000'000LL));
  ASSERT_TRUE(result.ok()) << result.error().message;
  const auto *pythonStrategy =
      dynamic_cast<PythonWorkerStrategy *>(strategy.get());
  ASSERT_NE(pythonStrategy, nullptr);
  const auto diagnostics = pythonStrategy->diagnostics();
  ASSERT_EQ(diagnostics.structuredLogs.size(), 1U);
  EXPECT_EQ(diagnostics.structuredLogs.front()
                .at("fields")
                .at("paths")
                .get<std::vector<std::string>>(),
            std::vector<std::string>{timezonePath.string()});
}

TEST(PythonWorkerStrategyTest, requiresConsentBeforeWorkerCreation) {
  auto strategyResult = PythonWorkerStrategy::createForTesting(
      BTE_TEST_PYTHON_EXECUTABLE, BTE_PYTHON_WORKER_BOOTSTRAP_PATH,
      validStrategySource, parameters(), false);

  ASSERT_FALSE(strategyResult.ok());
  EXPECT_EQ(strategyResult.error().code,
            bte::core::ErrorCode::permissionDenied);
}

TEST(PythonWorkerStrategyTest, rejectsMalformedSourceBeforeStartingAWorker) {
  const auto startCountBefore =
      bte::bindings::python::testing::managedWorkerProcessStartCount();
  const std::array invalidSources{
      std::string{},
      std::string{"source\0text", 11},
      std::string{"\xef\xbb\xbf", 3},
      std::string{"\xff", 1},
      std::string(bte::bindings::python::maximumWorkerSourceBytes + 1, 'x'),
  };

  for (const auto &source : invalidSources) {
    const auto rejected = PythonWorkerStrategy::createForTesting(
        BTE_TEST_PYTHON_EXECUTABLE, BTE_PYTHON_WORKER_BOOTSTRAP_PATH, source,
        parameters(), true);
    ASSERT_FALSE(rejected.ok());
    EXPECT_EQ(rejected.error().code, bte::core::ErrorCode::strategyLoadFailed);
    EXPECT_EQ(bte::bindings::python::testing::managedWorkerProcessStartCount(),
              startCountBefore);
  }
}

TEST(PythonWorkerStrategyTest,
     normalizesBomAndBothCrLineEndingFormsBeforeModuleLoad) {
  std::string source{"\xef\xbb\xbf", 3};
  bool usedLoneCarriageReturn = false;
  for (const auto character : validStrategySource) {
    if (character != '\n') {
      source.push_back(character);
    } else if (!usedLoneCarriageReturn) {
      source.push_back('\r');
      usedLoneCarriageReturn = true;
    } else {
      source += "\r\n";
    }
  }
  const auto strategyResult = PythonWorkerStrategy::createForTesting(
      BTE_TEST_PYTHON_EXECUTABLE, BTE_PYTHON_WORKER_BOOTSTRAP_PATH, source,
      parameters(), true);
  ASSERT_TRUE(strategyResult.ok()) << strategyResult.error().message;

  const auto market =
      strategyResult.value()->onMarket(context(1'700'000'000'000LL));

  ASSERT_TRUE(market.ok()) << market.error().message;
  ASSERT_EQ(market.value().size(), 1U);
  EXPECT_EQ(market.value().front().kind, StrategyCommandKind::buyMarket);
}

TEST(PythonWorkerStrategyTest,
     rejectsMissingMarketBarsAndInvalidContextValues) {
  auto strategyResult = PythonWorkerStrategy::createForTesting(
      BTE_TEST_PYTHON_EXECUTABLE, BTE_PYTHON_WORKER_BOOTSTRAP_PATH,
      parameterBoundaryStrategySource, parameters(), true);
  ASSERT_TRUE(strategyResult.ok()) << strategyResult.error().message;
  auto missingBar = context(1'700'000'000'000LL);
  missingBar.bar.reset();
  const auto missingBarResult = strategyResult.value()->onMarket(missingBar);
  ASSERT_FALSE(missingBarResult.ok());
  EXPECT_EQ(missingBarResult.error().code,
            bte::core::ErrorCode::strategyContractViolation);

  auto emptySymbol = context(1'700'000'000'000LL);
  emptySymbol.symbol = {};
  auto invalidUtf8Symbol = context(1'700'000'000'000LL);
  invalidUtf8Symbol.symbol = std::string{"\xff", 1};
  const auto invalidContexts = std::array{
      context(1'700'000'000'000LL, -1),
      context(1'700'000'000'000LL,
              bte::strategy::maximumStrategyQuantityShares + 1),
      emptySymbol,
      invalidUtf8Symbol,
  };
  auto invalidMarkPrice = context(1'700'000'000'000LL);
  invalidMarkPrice.markPriceNanodollars = 0;
  for (auto invalidContext : invalidContexts) {
    const auto freshStrategy = PythonWorkerStrategy::createForTesting(
        BTE_TEST_PYTHON_EXECUTABLE, BTE_PYTHON_WORKER_BOOTSTRAP_PATH,
        parameterBoundaryStrategySource, parameters(), true);
    ASSERT_TRUE(freshStrategy.ok()) << freshStrategy.error().message;
    const auto result = freshStrategy.value()->onMarket(invalidContext);
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.error().code,
              bte::core::ErrorCode::strategyContractViolation);
  }
  const auto freshStrategy = PythonWorkerStrategy::createForTesting(
      BTE_TEST_PYTHON_EXECUTABLE, BTE_PYTHON_WORKER_BOOTSTRAP_PATH,
      parameterBoundaryStrategySource, parameters(), true);
  ASSERT_TRUE(freshStrategy.ok()) << freshStrategy.error().message;
  const auto invalidMark = freshStrategy.value()->onMarket(invalidMarkPrice);
  ASSERT_FALSE(invalidMark.ok());
  EXPECT_EQ(invalidMark.error().code,
            bte::core::ErrorCode::strategyContractViolation);
}

TEST(PythonWorkerStrategyTest, rejectsCommandsDuringStrategyStartup) {
  const std::string source = R"python(
STOCKBT_API_VERSION = "1"
STOCKBT_TEMPLATE_VERSION = "1.0.0"
class Example:
    max_history_bars = 1
    indicators = {}
    def on_start(self, ctx):
        ctx.orders.buy_market(ctx.symbol, quantity=1)
    def on_market(self, ctx):
        return None
def create_strategy(params):
    return Example()
)python";
  const auto strategyResult = PythonWorkerStrategy::createForTesting(
      BTE_TEST_PYTHON_EXECUTABLE, BTE_PYTHON_WORKER_BOOTSTRAP_PATH, source,
      parameters(), true);
  ASSERT_TRUE(strategyResult.ok()) << strategyResult.error().message;
  const auto started =
      strategyResult.value()->onStart({.symbol = "AAPL",
                                       .bar = std::nullopt,
                                       .cashMicrodollars = 100'000'000,
                                       .equityMicrodollars = 100'000'000,
                                       .positionShares = 0,
                                       .markPriceNanodollars = std::nullopt});

  ASSERT_FALSE(started.ok());
  EXPECT_EQ(started.error().code,
            bte::core::ErrorCode::strategyCommandRejected);
}

TEST(PythonWorkerStrategyTest, preservesHookLogsWhenPythonHookRaises) {
  const std::string source = R"python(
STOCKBT_API_VERSION = "1"
STOCKBT_TEMPLATE_VERSION = "1.0.0"
class Example:
    max_history_bars = 1
    indicators = {}
    def on_market(self, ctx):
        ctx.log.warning("before failure", phase="decision")
        raise ValueError("deliberate failure")
def create_strategy(params):
    return Example()
)python";
  const auto processStartsBeforeRun =
      bte::bindings::python::testing::managedWorkerProcessStartCount();
  auto strategyResult = PythonWorkerStrategy::createForTesting(
      BTE_TEST_PYTHON_EXECUTABLE, BTE_PYTHON_WORKER_BOOTSTRAP_PATH, source,
      parameters(), true);
  ASSERT_TRUE(strategyResult.ok()) << strategyResult.error().message;
  EXPECT_EQ(bte::bindings::python::testing::managedWorkerProcessStartCount(),
            processStartsBeforeRun + 1);
  auto *pythonStrategy =
      dynamic_cast<PythonWorkerStrategy *>(strategyResult.value().get());
  ASSERT_NE(pythonStrategy, nullptr);

  const auto failed =
      strategyResult.value()->onMarket(context(1'700'000'000'000LL), {});
  ASSERT_FALSE(failed.ok());
  EXPECT_EQ(failed.error().code, bte::core::ErrorCode::strategyRuntimeError);
  const auto diagnostics = pythonStrategy->diagnostics();
  ASSERT_EQ(diagnostics.structuredLogs.size(), 1U);
  EXPECT_EQ(diagnostics.structuredLogs.front().at("message"), "before failure");
  ASSERT_EQ(diagnostics.structuredErrors.size(), 1U);
  EXPECT_EQ(diagnostics.structuredErrors.front().at("errorCode"),
            "strategyRuntimeError");
  EXPECT_EQ(diagnostics.structuredErrors.front().at("hook"), "on_market");
  EXPECT_EQ(diagnostics.structuredErrors.front().at("sourceLine"), 9);
  EXPECT_NE(diagnostics.structuredErrors.front()
                .at("traceback")
                .get<std::string>()
                .find("File \"strategy.py\", line 9"),
            std::string::npos);
}

TEST(PythonWorkerStrategyTest,
     marksCallbackDiagnosticsIncompleteWhenTheParentCancels) {
  const std::string source = R"python(
STOCKBT_API_VERSION = "1"
STOCKBT_TEMPLATE_VERSION = "1.0.0"
class Example:
    max_history_bars = 1
    indicators = {}
    def on_market(self, ctx):
        ctx.log.info("before cancellation")
def create_strategy(params):
    return Example()
)python";
  auto strategyResult = PythonWorkerStrategy::createForTesting(
      BTE_TEST_PYTHON_EXECUTABLE, BTE_PYTHON_WORKER_BOOTSTRAP_PATH, source,
      parameters(), true);
  ASSERT_TRUE(strategyResult.ok()) << strategyResult.error().message;
  auto *pythonStrategy =
      dynamic_cast<PythonWorkerStrategy *>(strategyResult.value().get());
  ASSERT_NE(pythonStrategy, nullptr);

  bte::core::CancellationSource cancellation;
  cancellation.requestCancellation();
  const auto canceled = strategyResult.value()->onMarket(
      context(1'700'000'000'000LL), cancellation.token());

  ASSERT_FALSE(canceled.ok());
  EXPECT_EQ(canceled.error().code, bte::core::ErrorCode::cancelled);
  EXPECT_FALSE(pythonStrategy->diagnostics().complete);
}

TEST(PythonWorkerStrategyTest, capturesWorkerStdoutAsAnEngineDiagnostic) {
  const std::string source = R"python(
STOCKBT_API_VERSION = "1"
STOCKBT_TEMPLATE_VERSION = "1.0.0"
class Example:
    max_history_bars = 1
    indicators = {}
    def on_market(self, ctx):
        print("python-output")
def create_strategy(params):
    return Example()
)python";
  auto strategyResult = PythonWorkerStrategy::createForTesting(
      BTE_TEST_PYTHON_EXECUTABLE, BTE_PYTHON_WORKER_BOOTSTRAP_PATH, source,
      parameters(), true);
  ASSERT_TRUE(strategyResult.ok()) << strategyResult.error().message;
  auto *pythonStrategy =
      dynamic_cast<PythonWorkerStrategy *>(strategyResult.value().get());
  ASSERT_NE(pythonStrategy, nullptr);
  auto execution = bte::engine::BacktestExecution::start(
      bte::engine::BacktestRequest{
          .bars = {context(1'700'000'000'000LL).bar.value()},
          .symbol = "AAPL",
          .initialCapitalMicrodollars = 100'000'000,
          .quantityShares = 1},
      std::move(strategyResult).value());
  ASSERT_TRUE(execution.ok()) << execution.error().message;

  const auto completed = execution.value()->runToCompletion();

  ASSERT_TRUE(completed.ok()) << completed.error().message;
  const auto pythonDiagnostics = pythonStrategy->diagnostics();
  ASSERT_NE(pythonDiagnostics.streams.standardOutput.retainedBytes.find(
                "python-output"),
            std::string::npos)
      << pythonDiagnostics.streams.standardOutput.bytesSeen;
  const auto &records = execution.value()->result().canonicalRecords;
  const auto output = std::ranges::find_if(records, [](const auto &record) {
    return record.family == bte::results::RecordFamily::log &&
           record.text.find("cHl0aG9uLW91dHB1dAo=") != std::string::npos;
  });
  ASSERT_NE(output, records.end());
  EXPECT_NE(output->text.find(R"("hook":"worker")"), std::string::npos);
  EXPECT_NE(output->text.find("cHl0aG9uLW91dHB1dAo="), std::string::npos);
}

TEST(PythonWorkerStrategyTest, marksResultIncompleteAfterBoundedLogTruncation) {
  const std::string source = R"python(
STOCKBT_API_VERSION = "1"
STOCKBT_TEMPLATE_VERSION = "1.0.0"
class Example:
    max_history_bars = 1
    indicators = {}
    def on_market(self, ctx):
        message = "x" * 8192
        for _ in range(100):
            ctx.log.info(message)
def create_strategy(params):
    return Example()
)python";
  auto strategyResult = PythonWorkerStrategy::createForTesting(
      BTE_TEST_PYTHON_EXECUTABLE, BTE_PYTHON_WORKER_BOOTSTRAP_PATH, source,
      parameters(), true);
  ASSERT_TRUE(strategyResult.ok()) << strategyResult.error().message;
  auto execution = bte::engine::BacktestExecution::start(
      bte::engine::BacktestRequest{
          .bars = {context(1'700'000'000'000LL).bar.value()},
          .symbol = "AAPL",
          .initialCapitalMicrodollars = 100'000'000,
          .quantityShares = 1},
      std::move(strategyResult).value());
  ASSERT_TRUE(execution.ok()) << execution.error().message;

  const auto completed = execution.value()->runToCompletion();

  ASSERT_TRUE(completed.ok()) << completed.error().message;
  const auto &result = execution.value()->result();
  EXPECT_FALSE(result.diagnosticsComplete);
  EXPECT_EQ(std::ranges::count_if(
                result.canonicalRecords,
                [](const auto &record) {
                  return record.family == bte::results::RecordFamily::log &&
                         record.text.find("DiagnosticsTruncated") !=
                             std::string::npos;
                }),
            1);
  EXPECT_TRUE(
      std::ranges::any_of(result.canonicalRecords, [](const auto &record) {
        return record.family == bte::results::RecordFamily::log &&
               record.text.find(R"("hook":"on_market")") != std::string::npos;
      }));
}

TEST(PythonWorkerStrategyTest, validatesFrozenParametersBeforeLaunchingWorker) {
  auto invalidParameters = parameters();
  invalidParameters["fast_period"] = 3;
  invalidParameters["slow_period"] = 3;

  auto strategyResult = PythonWorkerStrategy::createForTesting(
      BTE_TEST_PYTHON_EXECUTABLE, BTE_PYTHON_WORKER_BOOTSTRAP_PATH,
      validStrategySource, invalidParameters, true);

  ASSERT_FALSE(strategyResult.ok());
  EXPECT_EQ(strategyResult.error().code, bte::core::ErrorCode::invalidArgument);
}

TEST(PythonWorkerStrategyTest, acceptsRunParameterLimitBoundaries) {
  const auto boundaryParameters = std::array{
      nlohmann::json{{"fast_period", 1}, {"slow_period", 2}, {"quantity", 1}},
      nlohmann::json{{"fast_period", 9999},
                     {"slow_period", 10000},
                     {"quantity", 1'000'000'000}},
  };

  for (const auto &configuration : boundaryParameters) {
    auto strategyResult = PythonWorkerStrategy::createForTesting(
        BTE_TEST_PYTHON_EXECUTABLE, BTE_PYTHON_WORKER_BOOTSTRAP_PATH,
        parameterBoundaryStrategySource, configuration, true);
    ASSERT_TRUE(strategyResult.ok()) << strategyResult.error().message;
  }
}

TEST(PythonWorkerStrategyTest,
     rejectsOutOfRangeRunParametersBeforeLaunchingWorker) {
  const auto invalidParameters = std::array{
      nlohmann::json{{"fast_period", 0}, {"slow_period", 2}, {"quantity", 1}},
      nlohmann::json{
          {"fast_period", 10001}, {"slow_period", 10000}, {"quantity", 1}},
      nlohmann::json{{"fast_period", 1}, {"slow_period", 0}, {"quantity", 1}},
      nlohmann::json{
          {"fast_period", 1}, {"slow_period", 10001}, {"quantity", 1}},
      nlohmann::json{{"fast_period", 1}, {"slow_period", 2}, {"quantity", 0}},
      nlohmann::json{
          {"fast_period", 1}, {"slow_period", 2}, {"quantity", 1'000'000'001}},
      nlohmann::json{{"fast_period", 1}, {"slow_period", 2}, {"quantity", 1.5}},
      nlohmann::json{{"fast_period", 1.5}, {"slow_period", 2}, {"quantity", 1}},
  };

  for (const auto &configuration : invalidParameters) {
    const auto startCountBefore =
        bte::bindings::python::testing::managedWorkerProcessStartCount();
    const auto strategyResult = PythonWorkerStrategy::createForTesting(
        BTE_TEST_PYTHON_EXECUTABLE, BTE_PYTHON_WORKER_BOOTSTRAP_PATH,
        parameterBoundaryStrategySource, configuration, true);

    ASSERT_FALSE(strategyResult.ok());
    EXPECT_EQ(strategyResult.error().code,
              bte::core::ErrorCode::invalidArgument);
    EXPECT_EQ(bte::bindings::python::testing::managedWorkerProcessStartCount(),
              startCountBefore);
  }
}

TEST(PythonWorkerStrategyTest, exposesOnlyValidatedSmaRequirements) {
  const std::string source = R"python(
import stockbt
STOCKBT_API_VERSION = "1"
STOCKBT_TEMPLATE_VERSION = "1.0.0"
class Example:
    max_history_bars = 3
    indicators = {
        "fast": stockbt.SMA("fast", period=2),
        "slow": stockbt.SMA("slow", period=3),
    }
    def on_market(self, ctx):
        return None
def create_strategy(params):
    return Example()
)python";
  auto strategyResult = PythonWorkerStrategy::createForTesting(
      BTE_TEST_PYTHON_EXECUTABLE, BTE_PYTHON_WORKER_BOOTSTRAP_PATH, source,
      parameters(), true);
  ASSERT_TRUE(strategyResult.ok()) << strategyResult.error().message;

  const auto &requirements = strategyResult.value()->requirements();
  EXPECT_EQ(requirements.maxHistoryBars, 3U);
  ASSERT_EQ(requirements.indicators.size(), 2U);
  EXPECT_EQ(requirements.indicators[0].name, "fast");
  EXPECT_EQ(requirements.indicators[0].definition.kind,
            bte::indicators::IndicatorKind::sma);
  EXPECT_EQ(requirements.indicators[0].definition.period, 2);
  EXPECT_EQ(requirements.indicators[1].name, "slow");
  EXPECT_EQ(requirements.indicators[1].definition.period, 3);
}

TEST(PythonWorkerStrategyTest,
     rejectsIndicatorPeriodAboveDeclaredHistoryBudget) {
  const std::string source = R"python(
import stockbt
STOCKBT_API_VERSION = "1"
STOCKBT_TEMPLATE_VERSION = "1.0.0"
class Example:
    max_history_bars = 2
    indicators = {"slow": stockbt.SMA("slow", period=3)}
    def on_market(self, ctx):
        return None
def create_strategy(params):
    return Example()
)python";
  auto strategyResult = PythonWorkerStrategy::createForTesting(
      BTE_TEST_PYTHON_EXECUTABLE, BTE_PYTHON_WORKER_BOOTSTRAP_PATH, source,
      parameters(), true);

  ASSERT_FALSE(strategyResult.ok());
  EXPECT_EQ(strategyResult.error().code,
            bte::core::ErrorCode::strategyContractViolation);
  EXPECT_NE(strategyResult.error().message.find("history budget"),
            std::string::npos);
}

TEST(PythonWorkerStrategyTest, rejectsUnregisteredIndicatorDefinitions) {
  const std::string source = R"python(
STOCKBT_API_VERSION = "1"
STOCKBT_TEMPLATE_VERSION = "1.0.0"
class Example:
    max_history_bars = 3
    indicators = {"fast": {"kind": "EMA", "name": "fast", "period": 2}}
    def on_market(self, ctx):
        return None
def create_strategy(params):
    return Example()
)python";
  auto strategyResult = PythonWorkerStrategy::createForTesting(
      BTE_TEST_PYTHON_EXECUTABLE, BTE_PYTHON_WORKER_BOOTSTRAP_PATH, source,
      parameters(), true);

  ASSERT_FALSE(strategyResult.ok());
  EXPECT_EQ(strategyResult.error().code,
            bte::core::ErrorCode::strategyContractViolation);
  EXPECT_NE(strategyResult.error().message.find("SMA"), std::string::npos);
}

TEST(PythonWorkerStrategyTest, sampleSmaCrossMatchesIndependentEngineLedger) {
  const std::string source = R"python(
import stockbt
STOCKBT_API_VERSION = "1"
STOCKBT_TEMPLATE_VERSION = "1.0.0"
class SmaCross:
    max_history_bars = 3
    indicators = {
        "fast": stockbt.SMA("fast", period=2),
        "slow": stockbt.SMA("slow", period=3),
    }
    def __init__(self):
        self.previous_fast = None
        self.previous_slow = None
    def on_market(self, ctx):
        fast = ctx.indicators.get("fast")
        slow = ctx.indicators.get("slow")
        ctx.log.info("sma", timestamp=ctx.timestamp_utc_ms, fast=fast, slow=slow)
        if fast is not None and slow is not None:
            crossed_up = (
                self.previous_fast is not None
                and self.previous_slow is not None
                and self.previous_fast <= self.previous_slow
                and fast > slow
            )
            crossed_down = (
                self.previous_fast is not None
                and self.previous_slow is not None
                and self.previous_fast >= self.previous_slow
                and fast < slow
            )
            position = ctx.portfolio.positions.get(ctx.symbol)
            is_long = position is not None and position.quantity_shares > 0
            if not is_long and crossed_up:
                ctx.orders.buy_market(ctx.symbol, quantity=1)
            elif is_long and crossed_down:
                ctx.orders.close_position(ctx.symbol)
        self.previous_fast = fast
        self.previous_slow = slow
def create_strategy(params):
    return SmaCross()
)python";
  auto strategyResult = PythonWorkerStrategy::createForTesting(
      BTE_TEST_PYTHON_EXECUTABLE, BTE_PYTHON_WORKER_BOOTSTRAP_PATH, source,
      parameters(), true);
  ASSERT_TRUE(strategyResult.ok()) << strategyResult.error().message;
  auto *pythonStrategy =
      dynamic_cast<PythonWorkerStrategy *>(strategyResult.value().get());
  ASSERT_NE(pythonStrategy, nullptr);

  using bte::core::Bar;
  using bte::core::Timestamp;
  const auto start = std::int64_t{1'700'000'000'000};
  const std::array closes{3.0, 2.0, 1.0, 2.0, 4.0, 1.0, 1.0, 1.0};
  const std::array opens{3.0, 2.0, 1.0, 2.0, 4.0, 5.0, 1.0, 2.0};
  std::vector<Bar> bars;
  bars.reserve(closes.size());
  for (std::size_t index = 0; index < closes.size(); ++index) {
    const auto hour = static_cast<std::int64_t>(index + (index >= 6 ? 1 : 0));
    const auto low = std::min({opens[index], closes[index], 1.0});
    const auto high = std::max({opens[index], closes[index], 5.0});
    bars.push_back(Bar{
        .ts = Timestamp{std::chrono::milliseconds{start + hour * 3'600'000}},
        .open = opens[index],
        .high = high,
        .low = low,
        .close = closes[index],
        .volume = 100.0,
    });
  }
  auto request = bte::engine::BacktestRequest{
      .bars = std::move(bars),
      .symbol = "AAPL",
      .initialCapitalMicrodollars = 100'000'000,
      .quantityShares = 1,
  };
  auto execution = bte::engine::BacktestExecution::start(
      std::move(request), std::move(strategyResult).value());
  ASSERT_TRUE(execution.ok()) << execution.error().message;

  const auto completed = execution.value()->runToCompletion();

  ASSERT_TRUE(completed.ok()) << completed.error().message;
  const auto &result = execution.value()->result();
  EXPECT_EQ(result.status, bte::results::RunStatus::completed);
  const auto diagnostics = pythonStrategy->diagnostics();
  ASSERT_EQ(diagnostics.structuredLogs.size(), 8U);
  const std::array<std::optional<double>, 8> expectedFast{
      std::nullopt, 2.5, 1.5, 1.5, 3.0, 2.5, 1.0, 1.0};
  const std::array<std::optional<double>, 8> expectedSlow{
      std::nullopt, std::nullopt, 2.0, 5.0 / 3.0,
      7.0 / 3.0,    7.0 / 3.0,    2.0, 1.0};
  for (std::size_t index = 0; index < diagnostics.structuredLogs.size();
       ++index) {
    const auto &log = diagnostics.structuredLogs[index];
    ASSERT_EQ(log.at("message"), "sma");
    const auto &fields = log.at("fields");
    EXPECT_EQ(fields.at("timestamp"),
              start + static_cast<std::int64_t>(index + (index >= 6 ? 1 : 0)) *
                          3'600'000);
    if (expectedFast[index].has_value()) {
      EXPECT_DOUBLE_EQ(fields.at("fast").get<double>(),
                       expectedFast[index].value());
    } else {
      EXPECT_TRUE(fields.at("fast").is_null());
    }
    if (expectedSlow[index].has_value()) {
      EXPECT_DOUBLE_EQ(fields.at("slow").get<double>(),
                       expectedSlow[index].value());
    } else {
      EXPECT_TRUE(fields.at("slow").is_null());
    }
  }
  ASSERT_EQ(result.fills.size(), 2U)
      << nlohmann::json{diagnostics.structuredLogs}.dump();
  EXPECT_EQ(result.fills[0].priceNanodollars, 5'000'500'000);
  EXPECT_EQ(result.fills[0].timestamp,
            Timestamp{std::chrono::milliseconds{start + 5 * 3'600'000}});
  EXPECT_EQ(result.fills[1].priceNanodollars, 1'999'800'000);
  EXPECT_EQ(result.fills[1].timestamp,
            Timestamp{std::chrono::milliseconds{start + 8 * 3'600'000}});
  EXPECT_EQ(result.cashMicrodollars, 96'999'300);
  EXPECT_EQ(result.equityMicrodollars, 96'999'300);
  EXPECT_EQ(result.pnlMicrodollars, -3'000'700);
  EXPECT_EQ(result.positionShares, 0);
  EXPECT_EQ(diagnostics.streams.standardError.bytesSeen, 0U);
}

TEST(PythonWorkerStrategyTest, historyReturnsImmutableInclusiveActualBars) {
  const std::string source = R"python(
import stockbt
STOCKBT_API_VERSION = "1"
STOCKBT_TEMPLATE_VERSION = "1.0.0"
class HistoryProbe:
    max_history_bars = 3
    indicators = {
        "fast": stockbt.SMA("fast", period=2),
        "slow": stockbt.SMA("slow", period=3),
    }
    def on_market(self, ctx):
        bars = ctx.history(ctx.symbol, count=3)
        prior = ctx.history(
            ctx.symbol, count=3, end_utc_ms=ctx.timestamp_utc_ms - 1
        )
        immutable = isinstance(bars, tuple)
        try:
            bars[-1].close = -1.0
            immutable = False
        except AttributeError:
            pass
        ctx.log.info(
            "history",
            timestamps=[bar.timestamp_utc_ms for bar in bars],
            closes=[bar.close for bar in bars],
            prior_timestamps=[bar.timestamp_utc_ms for bar in prior],
            fast=ctx.indicators["fast"],
            slow=ctx.indicators["slow"],
            immutable=immutable,
        )
def create_strategy(params):
    return HistoryProbe()
)python";
  using bte::core::Bar;
  using bte::core::Timestamp;
  const auto start = std::int64_t{1'700'100'000'000};
  const auto makeBar = [](const std::int64_t hour, const double close) {
    return Bar{
        .ts = Timestamp{std::chrono::milliseconds{start + hour * 3'600'000}},
        .open = close,
        .high = close,
        .low = close,
        .close = close,
        .volume = 10.0,
    };
  };
  std::vector<Bar> history{makeBar(-2, 10.0), makeBar(-1, 11.0)};
  auto strategyResult = PythonWorkerStrategy::createForTesting(
      BTE_TEST_PYTHON_EXECUTABLE, BTE_PYTHON_WORKER_BOOTSTRAP_PATH, source,
      parameters(), true, true, {}, history);
  ASSERT_TRUE(strategyResult.ok()) << strategyResult.error().message;
  auto *pythonStrategy =
      dynamic_cast<PythonWorkerStrategy *>(strategyResult.value().get());
  ASSERT_NE(pythonStrategy, nullptr);

  auto request = bte::engine::BacktestRequest{
      .bars = {makeBar(0, 12.0), makeBar(2, 13.0), makeBar(3, 14.0)},
      .historyBars = std::move(history),
      .symbol = "AAPL",
      .initialCapitalMicrodollars = 100'000'000,
      .quantityShares = 1,
  };
  auto execution = bte::engine::BacktestExecution::start(
      std::move(request), std::move(strategyResult).value());
  ASSERT_TRUE(execution.ok()) << execution.error().message;

  const auto completed = execution.value()->runToCompletion();

  ASSERT_TRUE(completed.ok()) << completed.error().message;
  const auto diagnostics = pythonStrategy->diagnostics();
  ASSERT_EQ(diagnostics.structuredLogs.size(), 3U);
  const std::array<std::vector<std::int64_t>, 3> expectedTimestamps{
      std::vector<std::int64_t>{start - 2 * 3'600'000, start - 3'600'000,
                                start},
      std::vector<std::int64_t>{start - 3'600'000, start,
                                start + 2 * 3'600'000},
      std::vector<std::int64_t>{start, start + 2 * 3'600'000,
                                start + 3 * 3'600'000}};
  const std::array<std::vector<double>, 3> expectedCloses{
      std::vector<double>{10.0, 11.0, 12.0},
      std::vector<double>{11.0, 12.0, 13.0},
      std::vector<double>{12.0, 13.0, 14.0}};
  const std::array expectedFast{11.5, 12.5, 13.5};
  const std::array expectedSlow{11.0, 12.0, 13.0};
  const std::array<std::vector<std::int64_t>, 3> expectedPriorTimestamps{
      std::vector<std::int64_t>{start - 2 * 3'600'000, start - 3'600'000},
      std::vector<std::int64_t>{start - 3'600'000, start},
      std::vector<std::int64_t>{start, start + 2 * 3'600'000}};
  for (std::size_t index = 0; index < diagnostics.structuredLogs.size();
       ++index) {
    const auto &log = diagnostics.structuredLogs[index];
    ASSERT_EQ(log.at("message"), "history");
    const auto &fields = log.at("fields");
    EXPECT_TRUE(fields.at("immutable").get<bool>());
    EXPECT_EQ(fields.at("timestamps").get<std::vector<std::int64_t>>(),
              expectedTimestamps[index]);
    EXPECT_EQ(fields.at("closes").get<std::vector<double>>(),
              expectedCloses[index]);
    EXPECT_DOUBLE_EQ(fields.at("fast").get<double>(), expectedFast[index]);
    EXPECT_DOUBLE_EQ(fields.at("slow").get<double>(), expectedSlow[index]);
    EXPECT_EQ(fields.at("prior_timestamps").get<std::vector<std::int64_t>>(),
              expectedPriorTimestamps[index]);
  }
}

TEST(PythonWorkerStrategyTest, historySeedAcceptsTheExactTenThousandBarLimit) {
  const std::string source = R"python(
STOCKBT_API_VERSION = "1"
STOCKBT_TEMPLATE_VERSION = "1.0.0"
class HistoryBoundary:
    max_history_bars = 10000
    indicators = {}
    def on_market(self, ctx):
        bars = ctx.history(ctx.symbol, count=10000)
        ctx.log.info(
            "boundary",
            count=len(bars),
            first=bars[0].timestamp_utc_ms,
            last=bars[-1].timestamp_utc_ms,
        )
def create_strategy(params):
    return HistoryBoundary()
)python";
  using bte::core::Bar;
  using bte::core::Timestamp;
  const auto start = std::int64_t{1'700'500'000'000};
  std::vector<Bar> history;
  history.reserve(10'000);
  for (std::int64_t index = 0; index < 10'000; ++index) {
    const auto timestamp = start + index * 3'600'000;
    history.push_back(Bar{
        .ts = Timestamp{std::chrono::milliseconds{timestamp}},
        .open = 10.0,
        .high = 10.0,
        .low = 10.0,
        .close = 10.0,
        .volume = 100.0,
    });
  }
  auto strategyResult = PythonWorkerStrategy::createForTesting(
      BTE_TEST_PYTHON_EXECUTABLE, BTE_PYTHON_WORKER_BOOTSTRAP_PATH, source,
      parameters(), true, true, {}, history);
  ASSERT_TRUE(strategyResult.ok()) << strategyResult.error().message;
  auto *pythonStrategy =
      dynamic_cast<PythonWorkerStrategy *>(strategyResult.value().get());
  ASSERT_NE(pythonStrategy, nullptr);

  auto request = bte::engine::BacktestRequest{
      .bars = {Bar{.ts = Timestamp{std::chrono::milliseconds{
                       start + std::int64_t{10'000} * 3'600'000}},
                   .open = 10.0,
                   .high = 10.0,
                   .low = 10.0,
                   .close = 10.0,
                   .volume = 100.0}},
      .historyBars = std::move(history),
      .symbol = "AAPL",
      .initialCapitalMicrodollars = 100'000'000,
      .quantityShares = 1,
  };
  auto execution = bte::engine::BacktestExecution::start(
      std::move(request), std::move(strategyResult).value());
  ASSERT_TRUE(execution.ok()) << execution.error().message;

  const auto completed = execution.value()->runToCompletion();

  ASSERT_TRUE(completed.ok()) << completed.error().message;
  const auto diagnostics = pythonStrategy->diagnostics();
  ASSERT_EQ(diagnostics.structuredLogs.size(), 1U);
  const auto &fields = diagnostics.structuredLogs.front().at("fields");
  EXPECT_EQ(fields.at("count"), 10'000);
  EXPECT_EQ(fields.at("first"), start + 3'600'000);
  EXPECT_EQ(fields.at("last"), start + std::int64_t{10'000} * 3'600'000);
}

TEST(PythonWorkerStrategyTest,
     simplePythonThresholdMatchesSelectableEngineEvents) {
  const std::string source = R"python(
STOCKBT_API_VERSION = "1"
STOCKBT_TEMPLATE_VERSION = "1.0.0"
class Threshold:
    max_history_bars = 1
    indicators = {}
    def on_market(self, ctx):
        position = ctx.portfolio.positions.get(ctx.symbol)
        if ctx.bar.close > 1.5 and position is None:
            ctx.orders.buy_market(ctx.symbol, quantity=1)
def create_strategy(params):
    return Threshold()
)python";
  auto strategyResult = PythonWorkerStrategy::createForTesting(
      BTE_TEST_PYTHON_EXECUTABLE, BTE_PYTHON_WORKER_BOOTSTRAP_PATH, source,
      parameters(), true);
  ASSERT_TRUE(strategyResult.ok()) << strategyResult.error().message;

  using bte::core::Bar;
  using bte::core::Timestamp;
  const auto start = std::int64_t{1'700'400'000'000};
  const std::vector<Bar> bars{
      {.ts = Timestamp{std::chrono::milliseconds{start}},
       .open = 1.0,
       .high = 1.0,
       .low = 1.0,
       .close = 1.0,
       .volume = 10.0},
      {.ts = Timestamp{std::chrono::milliseconds{start + 3'600'000}},
       .open = 2.0,
       .high = 2.0,
       .low = 2.0,
       .close = 2.0,
       .volume = 10.0},
      {.ts = Timestamp{std::chrono::milliseconds{start + 7'200'000}},
       .open = 1.0,
       .high = 1.0,
       .low = 1.0,
       .close = 1.0,
       .volume = 10.0},
  };
  const auto makeRequest =
      [&bars](std::optional<bte::strategy::SelectableStrategyPlan> selectable =
                  std::nullopt) {
        return bte::engine::BacktestRequest{
            .bars = bars,
            .symbol = "AAPL",
            .initialCapitalMicrodollars = 100'000'000,
            .quantityShares = 1,
            .selectableStrategy = std::move(selectable),
        };
      };
  bte::strategy::SelectableStrategyPlan plan{
      .buy = {.logic = bte::strategy::ConditionLogic::all,
              .conditions = {bte::strategy::Condition{
                  .source = bte::strategy::ConditionSource::barField,
                  .comparison = bte::strategy::Comparison::greaterThan,
                  .threshold = 1.5,
                  .thresholdDomain = bte::indicators::NumericDomain::price,
                  .barField = bte::indicators::BarField::close}}},
      .sell = {},
  };
  auto selectable = bte::engine::BacktestExecution::start(makeRequest(plan));
  ASSERT_TRUE(selectable.ok()) << selectable.error().message;
  auto python = bte::engine::BacktestExecution::start(
      makeRequest(), std::move(strategyResult).value());
  ASSERT_TRUE(python.ok()) << python.error().message;

  const auto selectableCompleted = selectable.value()->runToCompletion();
  const auto pythonCompleted = python.value()->runToCompletion();

  ASSERT_TRUE(selectableCompleted.ok()) << selectableCompleted.error().message;
  ASSERT_TRUE(pythonCompleted.ok()) << pythonCompleted.error().message;
  EXPECT_EQ(selectable.value()->result().canonicalRecords,
            python.value()->result().canonicalRecords);
  ASSERT_EQ(python.value()->result().fills.size(), 1U);
  EXPECT_EQ(python.value()->result().fills.front().timestamp,
            Timestamp{std::chrono::milliseconds{start + 7'200'000}});
  EXPECT_EQ(python.value()->result().fills.front().priceNanodollars,
            1'000'100'000);
}

TEST(PythonWorkerStrategyTest,
     historyRejectsInvalidFutureAndOverBudgetRequests) {
  const auto runFailure = [](const std::string &source,
                             const bte::core::ErrorCode expectedError) {
    auto strategyResult = PythonWorkerStrategy::createForTesting(
        BTE_TEST_PYTHON_EXECUTABLE, BTE_PYTHON_WORKER_BOOTSTRAP_PATH, source,
        parameters(), true);
    EXPECT_TRUE(strategyResult.ok()) << strategyResult.error().message;
    if (!strategyResult.ok()) {
      return;
    }
    using bte::core::Bar;
    using bte::core::Timestamp;
    auto request = bte::engine::BacktestRequest{
        .bars = {Bar{
            .ts = Timestamp{std::chrono::milliseconds{1'700'200'000'000}},
            .open = 10.0,
            .high = 10.0,
            .low = 10.0,
            .close = 10.0,
            .volume = 10.0}},
        .symbol = "AAPL",
        .initialCapitalMicrodollars = 100'000'000,
        .quantityShares = 1,
    };
    auto execution = bte::engine::BacktestExecution::start(
        std::move(request), std::move(strategyResult).value());
    ASSERT_TRUE(execution.ok()) << execution.error().message;

    const auto advanced = execution.value()->advance();

    ASSERT_FALSE(advanced.ok());
    EXPECT_EQ(advanced.error().code, expectedError);
    EXPECT_EQ(execution.value()->status(), bte::results::RunStatus::failed);
    EXPECT_EQ(execution.value()->result().barsProcessed, 0U);
    EXPECT_TRUE(std::ranges::none_of(
        execution.value()->result().canonicalRecords, [](const auto &record) {
          return record.family == bte::results::RecordFamily::order ||
                 record.family == bte::results::RecordFamily::fill ||
                 record.family == bte::results::RecordFamily::portfolio;
        }));
  };

  runFailure(R"python(
STOCKBT_API_VERSION = "1"
STOCKBT_TEMPLATE_VERSION = "1.0.0"
class Probe:
    max_history_bars = 1
    indicators = {}
    def on_market(self, ctx):
        ctx.history(ctx.symbol, count=1, end_utc_ms=ctx.timestamp_utc_ms + 1)
def create_strategy(params):
    return Probe()
)python",
             bte::core::ErrorCode::futureDataRequest);
  runFailure(R"python(
STOCKBT_API_VERSION = "1"
STOCKBT_TEMPLATE_VERSION = "1.0.0"
class Probe:
    max_history_bars = 1
    indicators = {}
    def on_market(self, ctx):
        ctx.history(ctx.symbol, count=2)
def create_strategy(params):
    return Probe()
)python",
             bte::core::ErrorCode::historyBudgetExceeded);
  runFailure(R"python(
STOCKBT_API_VERSION = "1"
STOCKBT_TEMPLATE_VERSION = "1.0.0"
class Probe:
    max_history_bars = 1
    indicators = {}
    def on_market(self, ctx):
        ctx.history(ctx.symbol, count=True)
def create_strategy(params):
    return Probe()
)python",
             bte::core::ErrorCode::invalidHistoryRequest);
  runFailure(R"python(
STOCKBT_API_VERSION = "1"
STOCKBT_TEMPLATE_VERSION = "1.0.0"
class Probe:
    max_history_bars = 1
    indicators = {}
    def on_market(self, ctx):
        ctx.history("MSFT", count=1)
def create_strategy(params):
    return Probe()
)python",
             bte::core::ErrorCode::invalidHistoryRequest);
  runFailure(R"python(
STOCKBT_API_VERSION = "1"
STOCKBT_TEMPLATE_VERSION = "1.0.0"
class Probe:
    max_history_bars = 1
    indicators = {}
    def on_market(self, ctx):
        ctx.history(ctx.symbol, count=1, end_utc_ms=True)
def create_strategy(params):
    return Probe()
)python",
             bte::core::ErrorCode::invalidHistoryRequest);
}

TEST(PythonWorkerStrategyTest, historyIsUnavailableBeforeAStartTimestamp) {
  const std::string source = R"python(
STOCKBT_API_VERSION = "1"
STOCKBT_TEMPLATE_VERSION = "1.0.0"
class Probe:
    max_history_bars = 1
    indicators = {}
    def on_start(self, ctx):
        ctx.history(ctx.symbol, count=1)
    def on_market(self, ctx):
        return None
def create_strategy(params):
    return Probe()
)python";
  auto strategyResult = PythonWorkerStrategy::createForTesting(
      BTE_TEST_PYTHON_EXECUTABLE, BTE_PYTHON_WORKER_BOOTSTRAP_PATH, source,
      parameters(), true);
  ASSERT_TRUE(strategyResult.ok()) << strategyResult.error().message;
  auto execution = bte::engine::BacktestExecution::start(
      bte::engine::BacktestRequest{
          .bars = {context(1'700'300'000'000LL).bar.value()},
          .symbol = "AAPL",
          .initialCapitalMicrodollars = 100'000'000,
          .quantityShares = 1},
      std::move(strategyResult).value());
  ASSERT_TRUE(execution.ok()) << execution.error().message;

  const auto advanced = execution.value()->advance();

  ASSERT_FALSE(advanced.ok());
  EXPECT_EQ(advanced.error().code, bte::core::ErrorCode::historyUnavailable);
  EXPECT_EQ(execution.value()->result().barsProcessed, 0U);
}

TEST(PythonWorkerStrategyTest, rejectsWorkerCommandAndReturnsNoPartialBatch) {
  const std::string invalidStrategy = R"python(
STOCKBT_API_VERSION = "1"
STOCKBT_TEMPLATE_VERSION = "1.0.0"
class Example:
    max_history_bars = 1
    indicators = {}
    def on_market(self, ctx):
        ctx.orders.buy_market(ctx.symbol, quantity=2)
        ctx.orders.close_position(ctx.symbol)
def create_strategy(params):
    return Example()
)python";
  auto strategyResult = PythonWorkerStrategy::createForTesting(
      BTE_TEST_PYTHON_EXECUTABLE, BTE_PYTHON_WORKER_BOOTSTRAP_PATH,
      invalidStrategy, parameters(), true);
  ASSERT_TRUE(strategyResult.ok()) << strategyResult.error().message;

  const auto market =
      strategyResult.value()->onMarket(context(1'700'000'000'000LL));

  ASSERT_FALSE(market.ok());
  EXPECT_EQ(market.error().code, bte::core::ErrorCode::strategyCommandRejected)
      << market.error().message;
}

TEST(PythonWorkerStrategyTest,
     preservesStrategyRuntimeErrorWhenUnicodeTracebackIsTruncated) {
  const std::string source = R"python(
STOCKBT_API_VERSION = "1"
STOCKBT_TEMPLATE_VERSION = "1.0.0"
class Example:
    max_history_bars = 1
    indicators = {}
    def on_market(self, ctx):
        raise ValueError("中" * 20000)
def create_strategy(params):
    return Example()
)python";
  auto strategyResult = PythonWorkerStrategy::createForTesting(
      BTE_TEST_PYTHON_EXECUTABLE, BTE_PYTHON_WORKER_BOOTSTRAP_PATH, source,
      parameters(), true);
  ASSERT_TRUE(strategyResult.ok()) << strategyResult.error().message;

  const auto market =
      strategyResult.value()->onMarket(context(1'700'000'000'000LL));

  ASSERT_FALSE(market.ok());
  EXPECT_EQ(market.error().code, bte::core::ErrorCode::strategyRuntimeError)
      << market.error().message;
  EXPECT_NE(market.error().message.find("中"), std::string::npos);
  EXPECT_LE(market.error().message.size(), 64U * 1024U);
}

TEST(PythonWorkerStrategyTest,
     rejectsUnsupportedOrderMethodAndPreservesCallbackAtomicity) {
  const std::string invalidStrategy = R"python(
STOCKBT_API_VERSION = "1"
STOCKBT_TEMPLATE_VERSION = "1.0.0"
class Example:
    max_history_bars = 1
    indicators = {}
    def on_market(self, ctx):
        ctx.orders.buy_market(ctx.symbol, quantity=2)
        ctx.orders.limit_buy(ctx.symbol, quantity=2, limit_price=10)
def create_strategy(params):
    return Example()
)python";
  auto strategyResult = PythonWorkerStrategy::createForTesting(
      BTE_TEST_PYTHON_EXECUTABLE, BTE_PYTHON_WORKER_BOOTSTRAP_PATH,
      invalidStrategy, parameters(), true);
  ASSERT_TRUE(strategyResult.ok()) << strategyResult.error().message;

  using bte::core::Bar;
  using bte::core::Timestamp;
  auto execution = bte::engine::BacktestExecution::start(
      bte::engine::BacktestRequest{
          .bars = {Bar{
              .ts = Timestamp{std::chrono::milliseconds{1'700'500'000'000}},
              .open = 10.0,
              .high = 10.0,
              .low = 10.0,
              .close = 10.0,
              .volume = 10.0}},
          .symbol = "AAPL",
          .initialCapitalMicrodollars = 100'000'000,
          .quantityShares = 1},
      std::move(strategyResult).value());
  ASSERT_TRUE(execution.ok()) << execution.error().message;

  const auto advanced = execution.value()->advance();

  ASSERT_FALSE(advanced.ok());
  EXPECT_EQ(advanced.error().code,
            bte::core::ErrorCode::strategyCommandRejected)
      << advanced.error().message;
  EXPECT_EQ(execution.value()->status(), bte::results::RunStatus::failed);
  EXPECT_EQ(execution.value()->result().barsProcessed, 0U);
  EXPECT_TRUE(std::ranges::none_of(
      execution.value()->result().canonicalRecords, [](const auto &record) {
        return record.family == bte::results::RecordFamily::order ||
               record.family == bte::results::RecordFamily::fill ||
               record.family == bte::results::RecordFamily::portfolio;
      }));
}

TEST(PythonWorkerStrategyTest,
     validatesPythonOrderQuantityTypesAndPreservesCallbackAtomicity) {
  constexpr std::array<std::string_view, 8> invalidQuantities{
      "True",         "1.0", "1.5", "float('nan')",
      "float('inf')", "0",   "-1",  "1000000001"};
  for (const auto quantity : invalidQuantities) {
    const auto source =
        std::string{R"PY(STOCKBT_API_VERSION = "1"
STOCKBT_TEMPLATE_VERSION = "1.0.0"
class Example:
    max_history_bars = 1
    indicators = {}
    def on_market(self, ctx):
        ctx.orders.buy_market(ctx.symbol, quantity=)PY"} +
        std::string{quantity} +
        ")\ndef create_strategy(params):\n    return Example()\n";
    auto strategyResult = PythonWorkerStrategy::createForTesting(
        BTE_TEST_PYTHON_EXECUTABLE, BTE_PYTHON_WORKER_BOOTSTRAP_PATH, source,
        parameters(), true);
    ASSERT_TRUE(strategyResult.ok()) << strategyResult.error().message;

    using bte::core::Bar;
    using bte::core::Timestamp;
    auto execution = bte::engine::BacktestExecution::start(
        bte::engine::BacktestRequest{
            .bars = {Bar{
                .ts = Timestamp{std::chrono::milliseconds{1'700'500'000'000}},
                .open = 10.0,
                .high = 10.0,
                .low = 10.0,
                .close = 10.0,
                .volume = 10.0}},
            .symbol = "AAPL",
            .initialCapitalMicrodollars = 100'000'000,
            .quantityShares = 1},
        std::move(strategyResult).value());
    ASSERT_TRUE(execution.ok()) << execution.error().message;

    const auto advanced = execution.value()->advance();

    ASSERT_FALSE(advanced.ok()) << quantity;
    EXPECT_EQ(advanced.error().code,
              bte::core::ErrorCode::strategyCommandRejected)
        << quantity << ": " << advanced.error().message;
    EXPECT_EQ(execution.value()->status(), bte::results::RunStatus::failed);
    EXPECT_EQ(execution.value()->result().barsProcessed, 0U);
    EXPECT_TRUE(std::ranges::none_of(
        execution.value()->result().canonicalRecords,
        [](const auto &record) {
          return record.family == bte::results::RecordFamily::order ||
                 record.family == bte::results::RecordFamily::fill ||
                 record.family == bte::results::RecordFamily::portfolio;
        }))
        << quantity;
  }
}

TEST(PythonWorkerStrategyTest,
     acceptsPythonOrderQuantityAtBothSupportedBoundaries) {
  constexpr std::array<std::pair<std::string_view, std::int64_t>, 2> quantities{
      {{"1", 1}, {"1000000000", 1'000'000'000}}};
  for (const auto &[expression, expected] : quantities) {
    const auto source =
        std::string{R"PY(STOCKBT_API_VERSION = "1"
STOCKBT_TEMPLATE_VERSION = "1.0.0"
class Example:
    max_history_bars = 1
    indicators = {}
    def on_market(self, ctx):
        ctx.orders.buy_market(ctx.symbol, quantity=)PY"} +
        std::string{expression} +
        ")\ndef create_strategy(params):\n    return Example()\n";
    auto strategyResult = PythonWorkerStrategy::createForTesting(
        BTE_TEST_PYTHON_EXECUTABLE, BTE_PYTHON_WORKER_BOOTSTRAP_PATH, source,
        parameters(), true);
    ASSERT_TRUE(strategyResult.ok()) << strategyResult.error().message;

    const auto commands =
        strategyResult.value()->onMarket(context(1'700'500'000'000LL));

    ASSERT_TRUE(commands.ok()) << commands.error().message;
    ASSERT_EQ(commands.value().size(), 1U);
    EXPECT_EQ(commands.value().front().kind, StrategyCommandKind::buyMarket);
    EXPECT_EQ(commands.value().front().quantityShares, expected);
  }
}

TEST(PythonWorkerStrategyTest,
     startupFailuresRetainBoundedStrategySourceLocations) {
  const std::array cases{
      std::pair{std::string{"STOCKBT_API_VERSION = \"1\"\n"
                            "STOCKBT_TEMPLATE_VERSION = \"1.0.0\"\n"
                            "def create_strategy(:\n    pass\n"},
                std::pair{bte::core::ErrorCode::strategyLoadFailed,
                          std::string{"strategy.py:3:"}}},
      std::pair{std::string{"STOCKBT_API_VERSION = \"1\"\n"
                            "STOCKBT_TEMPLATE_VERSION = \"1.0.0\"\n"
                            "import subprocess\n"
                            "def create_strategy(params): return None\n"},
                std::pair{bte::core::ErrorCode::strategyLoadFailed,
                          std::string{"strategy.py:3:"}}},
      std::pair{std::string{"STOCKBT_API_VERSION = \"1\"\n"
                            "STOCKBT_TEMPLATE_VERSION = \"1.0.0\"\n"
                            "raise RuntimeError('module boom')\n"
                            "def create_strategy(params): return None\n"},
                std::pair{bte::core::ErrorCode::strategyLoadFailed,
                          std::string{"module boom"}}},
      std::pair{std::string{"STOCKBT_API_VERSION = \"1\"\n"
                            "STOCKBT_TEMPLATE_VERSION = \"1.0.0\"\n"
                            "class Example:\n"
                            "    max_history_bars = 1\n"
                            "    indicators = {}\n"
                            "def create_strategy(params):\n"
                            "    raise ValueError('factory boom')\n"},
                std::pair{bte::core::ErrorCode::strategyFactoryFailed,
                          std::string{"factory boom"}}},
  };
  for (const auto &[source, expected] : cases) {
    auto strategyResult = PythonWorkerStrategy::createForTesting(
        BTE_TEST_PYTHON_EXECUTABLE, BTE_PYTHON_WORKER_BOOTSTRAP_PATH, source,
        parameters(), true);

    ASSERT_FALSE(strategyResult.ok());
    EXPECT_EQ(strategyResult.error().code, expected.first);
    EXPECT_NE(strategyResult.error().message.find(expected.second),
              std::string::npos)
        << strategyResult.error().message;
    EXPECT_LE(strategyResult.error().message.size(), 64U * 1024U);
  }
}

TEST(PythonWorkerStrategyTest, rejectsInvalidSourceWithoutReturningStrategy) {
  const std::string invalidSource = "import os\n";
  auto strategyResult = PythonWorkerStrategy::createForTesting(
      BTE_TEST_PYTHON_EXECUTABLE, BTE_PYTHON_WORKER_BOOTSTRAP_PATH,
      invalidSource, parameters(), true);

  ASSERT_FALSE(strategyResult.ok());
  EXPECT_EQ(strategyResult.error().code,
            bte::core::ErrorCode::strategyLoadFailed);
  EXPECT_TRUE(
      strategyResult.error().message.starts_with("StrategyImportNotAllowed:"));
}

TEST(PythonWorkerStrategyTest, reportsStableSyntaxAndApiVersionErrorCodes) {
  const std::array<std::pair<std::string, std::string>, 2> failures{{
      {"def create_strategy(:\n    pass\n", "StrategySyntaxError:"},
      {"STOCKBT_API_VERSION = \"9\"\n"
       "STOCKBT_TEMPLATE_VERSION = \"1.0.0\"\n"
       "def create_strategy(params): return None\n",
       "UnsupportedStrategyApiVersion:"},
  }};
  for (const auto &[source, expectedCode] : failures) {
    const auto result = PythonWorkerStrategy::createForTesting(
        BTE_TEST_PYTHON_EXECUTABLE, BTE_PYTHON_WORKER_BOOTSTRAP_PATH, source,
        parameters(), true);
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.error().code, bte::core::ErrorCode::strategyLoadFailed);
    EXPECT_TRUE(result.error().message.starts_with(expectedCode))
        << result.error().message;
  }
}

#if defined(BTE_TEST_PYTHON_RESULT_REPLAY)
TEST(PythonWorkerStrategyTest,
     schemaThreePythonRunPersistsAndReplaySupportsIndexedSeek) {
  QTemporaryDir directory;
  ASSERT_TRUE(directory.isValid());
  const auto root = std::filesystem::path{directory.path().toStdString()};
  const auto sourceDirectory = root / "Source";
  std::filesystem::create_directories(sourceDirectory);
  std::ofstream csv{sourceDirectory / "SYN.csv"};
  csv << "symbol,ts,open,high,low,close,volume,schemaName\n"
         "SYN,2024-01-02 00:00:00+00:00,100,101,99,100,1000,ohlcv-1h\n"
         "SYN,2024-01-02 01:00:00+00:00,101,102,100,101,1100,ohlcv-1h\n"
         "SYN,2024-01-02 02:00:00+00:00,102,103,101,102,1200,ohlcv-1h\n";
  csv.close();
  ASSERT_TRUE(csv.good());

  const auto built = bte::data::buildReleaseSnapshot({
      .sourceDirectory = sourceDirectory,
      .storeDirectory = root / "Data",
      .symbols = {"SYN"},
      .rowsPerSegment = 2,
      .calendarHash = std::string(64, 'a'),
      .splitManifestHash = std::string(64, 'b'),
  });
  ASSERT_TRUE(built.ok()) << built.error().message;
  auto reader = bte::data::ReleaseSnapshotReader::open(
      root / "Data", built.value().snapshotId);
  ASSERT_TRUE(reader.ok()) << reader.error().message;
  const auto start = bte::core::time::parseIso8601("2024-01-02 00:00:00+00:00");
  const auto end = bte::core::time::parseIso8601("2024-01-02 03:00:00+00:00");
  ASSERT_TRUE(start.ok());
  ASSERT_TRUE(end.ok());
  auto selection = reader.value()->select({
      .symbols = {"SYN"},
      .range = {.start = start.value(), .end = end.value()},
      .timeframe = "ohlcv-1h",
  });
  ASSERT_TRUE(selection.ok()) << selection.error().message;
  ASSERT_EQ(selection.value().bars.size(), 3U);

  const std::string source = R"python(
STOCKBT_API_VERSION = "1"
STOCKBT_TEMPLATE_VERSION = "1.0.0"
class Example:
    max_history_bars = 1
    indicators = {}
    def __init__(self):
        self.submitted = False
    def on_market(self, ctx):
        if not self.submitted:
            ctx.orders.buy_market(ctx.symbol, quantity=2)
            self.submitted = True
def create_strategy(params):
    return Example()
)python";
  auto strategyResult = PythonWorkerStrategy::createForTesting(
      BTE_TEST_PYTHON_EXECUTABLE, BTE_PYTHON_WORKER_BOOTSTRAP_PATH, source,
      parameters(), true);
  ASSERT_TRUE(strategyResult.ok()) << strategyResult.error().message;

  std::vector<bte::core::Bar> bars;
  bars.reserve(selection.value().bars.size());
  for (const auto &bar : selection.value().bars) {
    bars.push_back({
        .ts = bar.timestamp,
        .open = static_cast<double>(bar.openNanodollars) / 1'000'000'000.0,
        .high = static_cast<double>(bar.highNanodollars) / 1'000'000'000.0,
        .low = static_cast<double>(bar.lowNanodollars) / 1'000'000'000.0,
        .close = static_cast<double>(bar.closeNanodollars) / 1'000'000'000.0,
        .volume = static_cast<double>(bar.volumeMicroshares) / 1'000'000.0,
    });
  }

  auto descriptor = bte::results::RunDescriptor{
      .schemaVersion = 3,
      .universe = {"SYN"},
      .range = {.start = selection.value().bars.front().timestamp,
                .end = selection.value().bars.back().timestamp +
                       std::chrono::hours{1}},
      .initialCapitalMicrodollars = 1'000'000'000,
      .strategyId = "python-integration",
      .strategyHash = bte::core::sha256(source),
      .dataSelection = selection.value().identity,
  };
  auto &provenance = descriptor.provenance;
  provenance.engineBuild = "test-build";
  provenance.timeframe = "ohlcv-1h";
  provenance.costProfile = "retailUsEquityV1";
  provenance.commissionRatePartsPerBillion = 0;
  provenance.slippageRatePartsPerBillion = 100'000;
  provenance.seed = 0;
  provenance.strategyMode = bte::results::StrategyMode::python;
  provenance.apiVersion = "1";
  provenance.templateVersion = "1.0.0";
  provenance.canonicalParametersJson =
      R"({"fast_period":2,"quantity":2,"slow_period":3})";
  provenance.sourceUtf8 = source;
  provenance.sourceHash = bte::core::sha256(source);
  for (const auto *key :
       {"aggregationPolicy", "costs", "dataSelection", "engineBuild",
        "numericPolicy", "runtimeProfile", "runtimePackages", "seed",
        "strategyApi", "strategyArtifact", "strategyMode", "strategyTemplate",
        "timeframe"}) {
    provenance.provenanceAvailability.emplace(
        key, bte::results::ProvenanceAvailabilityState::recorded);
  }

  const auto packageNames = std::array{
      std::pair{"CPython", "3.14.9"},
      std::pair{"numpy", "2.5.4"},
      std::pair{"pandas", "3.0.6"},
      std::pair{"python-dateutil", "2.9.0.post0"},
      std::pair{"six", "1.17.0"},
      std::pair{"stockbt", "1"},
      std::pair{"tzdata", "2026.5"},
  };
  auto components = nlohmann::json::array();
  auto &runtime = provenance.runtime.emplace();
  runtime.profileId = "test-runtime-profile";
  runtime.pythonVersion = "3.14.9";
  runtime.pythonAbi = "cp314";
  runtime.stockbtSourceHash = bte::core::sha256("stockbt-test-source");
  for (const auto &[name, version] : packageNames) {
    const auto hash = std::string{name} == "stockbt"
                          ? runtime.stockbtSourceHash
                          : bte::core::sha256(std::string{name} + version);
    runtime.packages.push_back({
        .name = name,
        .version = version,
        .artifactHash = hash,
    });
    components.push_back({
        {"artifactSha256", hash},
        {"licenses", nlohmann::json::array({"Apache-2.0"})},
        {"name", name},
        {"upstreamUrl", "https://example.invalid/test-artifact"},
        {"version", version},
    });
  }
  const auto runtimeManifest = bte::core::serializeCanonicalJson(nlohmann::json{
      {"architecture", "arm64"},
      {"components", std::move(components)},
      {"installedFiles",
       nlohmann::json::array({{{"path", "python/bin/python3"},
                               {"sha256", bte::core::sha256("python")}}})},
      {"platform", "macos"},
      {"profileBuilderVersion", "test"},
      {"profileId", runtime.profileId},
      {"pythonAbi", runtime.pythonAbi},
      {"pythonExecutable", "python/bin/python3"},
      {"pythonVersion", runtime.pythonVersion},
      {"schemaVersion", 1},
      {"stockbtApiVersion", "1"},
      {"stockbtSourceSha256", runtime.stockbtSourceHash},
  });
  ASSERT_TRUE(runtimeManifest.ok()) << runtimeManifest.error().message;
  runtime.canonicalManifestJson = runtimeManifest.value();
  runtime.manifestHash = bte::core::sha256(runtime.canonicalManifestJson);

  auto store = bte::results::ResultStore::open(root / "Results", root / "Data");
  ASSERT_TRUE(store.ok()) << store.error().message;
  const auto recorded = bte::engine::runBacktestAndRecord(
      {.bars = std::move(bars),
       .symbol = "SYN",
       .initialCapitalMicrodollars = 1'000'000'000,
       .quantityShares = 2},
      std::move(strategyResult).value(), *store.value(), descriptor);
  ASSERT_TRUE(recorded.ok()) << recorded.error().message;
  ASSERT_TRUE(recorded.value().backtest.has_value());
  EXPECT_EQ(recorded.value().backtest->fills.size(), 1U);

  ASSERT_TRUE(
      store.value()->publishDraft(recorded.value().persisted.resultId).ok());
  const auto unavailableRuntime =
      bte::bindings::python::discoverManagedRuntime(root / "MissingRuntime");
  ASSERT_FALSE(unavailableRuntime.ok());
  EXPECT_EQ(unavailableRuntime.error().code,
            bte::core::ErrorCode::runtimeUnavailable);
  const auto processStartsBeforeReplay =
      bte::bindings::python::testing::managedWorkerProcessStartCount();
  auto replay = bte::bindings::ResultReplay::open(
      root / "Results", root / "Data", recorded.value().persisted.resultId,
      bte::bindings::ResultReplayTimeframe::hourly);
  ASSERT_TRUE(replay.ok()) << replay.error().message;
  EXPECT_EQ(bte::bindings::python::testing::managedWorkerProcessStartCount(),
            processStartsBeforeReplay);
  ASSERT_EQ(replay.value()->totalFrames(), 3U);
  EXPECT_TRUE(replay.value()->seek(0));
  EXPECT_TRUE(replay.value()->current()->fills.empty());
  EXPECT_TRUE(replay.value()->seek(1));
  ASSERT_EQ(replay.value()->current()->fills.size(), 1U);
  EXPECT_EQ(replay.value()->current()->fills.front().quantityShares, 2);
  EXPECT_DOUBLE_EQ(replay.value()->current()->fills.front().price, 101.0101);
  EXPECT_TRUE(replay.value()->seek(0));
  EXPECT_EQ(replay.value()->current()->candle.close, 100.0);
}
#endif

TEST(PythonWorkerStrategyTest, cancellationStopsWorkerAndRollsBackActiveBar) {
  const std::string loopingStrategy = R"python(
STOCKBT_API_VERSION = "1"
STOCKBT_TEMPLATE_VERSION = "1.0.0"
class Example:
    max_history_bars = 1
    indicators = {}
    def on_market(self, ctx):
        while True:
            pass
def create_strategy(params):
    return Example()
)python";
  auto strategyResult = PythonWorkerStrategy::createForTesting(
      BTE_TEST_PYTHON_EXECUTABLE, BTE_PYTHON_WORKER_BOOTSTRAP_PATH,
      loopingStrategy, parameters(), true);
  ASSERT_TRUE(strategyResult.ok()) << strategyResult.error().message;
  auto request = bte::engine::BacktestRequest{
      .bars = {context(1'700'000'000'000LL).bar.value()},
      .symbol = "AAPL",
      .initialCapitalMicrodollars = 100'000'000,
      .quantityShares = 1,
      .selectableStrategy = std::nullopt};
  auto execution = bte::engine::BacktestExecution::start(
      std::move(request), std::move(strategyResult).value());
  ASSERT_TRUE(execution.ok()) << execution.error().message;
  bte::core::CancellationSource cancellation;
  auto cancelRequest = std::async(std::launch::async, [&cancellation] {
    std::this_thread::sleep_for(std::chrono::milliseconds{80});
    cancellation.requestCancellation();
  });

  const auto advanced = execution.value()->advance(cancellation.token());
  cancelRequest.get();

  ASSERT_FALSE(advanced.ok());
  EXPECT_EQ(advanced.error().code, bte::core::ErrorCode::cancelled);
  EXPECT_EQ(execution.value()->status(), bte::results::RunStatus::canceled);
  EXPECT_EQ(execution.value()->result().barsProcessed, 0U);
  EXPECT_TRUE(execution.value()->result().fills.empty());
  EXPECT_TRUE(std::ranges::none_of(
      execution.value()->result().canonicalRecords, [](const auto &record) {
        return record.family == bte::results::RecordFamily::order ||
               record.family == bte::results::RecordFamily::fill ||
               record.family == bte::results::RecordFamily::portfolio;
      }));
  const auto &records = execution.value()->result().canonicalRecords;
  const auto terminal = std::ranges::find_if(records, [](const auto &record) {
    return record.family == bte::results::RecordFamily::terminalDiagnostic;
  });
  ASSERT_NE(terminal, records.end());
  EXPECT_NE(terminal->text.find(R"("errorCode":"Canceled")"),
            std::string::npos);
  EXPECT_NE(terminal->text.find(R"("hook":"on_market")"), std::string::npos);
}

TEST(PythonWorkerStrategyTest,
     cancellationDuringOnFillProducesTypedTerminalDiagnostic) {
  auto strategyResult = PythonWorkerStrategy::createForTesting(
      BTE_TEST_PYTHON_EXECUTABLE, BTE_PYTHON_WORKER_BOOTSTRAP_PATH,
      validStrategySource, parameters(), true);
  ASSERT_TRUE(strategyResult.ok()) << strategyResult.error().message;
  bte::core::CancellationSource cancellation;
  cancellation.requestCancellation();

  const auto result = strategyResult.value()->onFill(
      {.timestamp = context(1'700'000'000'000LL).bar->ts,
       .side = bte::strategy::StrategyFillSide::buy,
       .quantityShares = 1,
       .priceNanodollars = 1'000'000'000,
       .amountMicrodollars = 1'000'000},
      context(1'700'000'000'000LL), cancellation.token());

  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().code, bte::core::ErrorCode::cancelled);
  const auto diagnostics = strategyResult.value()->takeDiagnostics();
  const auto terminal =
      std::ranges::find_if(diagnostics.records, [](const auto &record) {
        return record.kind == bte::strategy::StrategyDiagnosticKind::terminal;
      });
  ASSERT_NE(terminal, diagnostics.records.end());
  EXPECT_EQ(terminal->errorCode, "Canceled");
  EXPECT_EQ(terminal->hook, "on_fill");
}

TEST(PythonWorkerStrategyTest,
     cancellationDuringOnFinishProducesTypedTerminalDiagnostic) {
  auto strategyResult = PythonWorkerStrategy::createForTesting(
      BTE_TEST_PYTHON_EXECUTABLE, BTE_PYTHON_WORKER_BOOTSTRAP_PATH,
      validStrategySource, parameters(), true);
  ASSERT_TRUE(strategyResult.ok()) << strategyResult.error().message;
  bte::core::CancellationSource cancellation;
  cancellation.requestCancellation();

  const auto result = strategyResult.value()->onFinish(
      context(1'700'000'000'000LL), cancellation.token());

  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().code, bte::core::ErrorCode::cancelled);
  const auto diagnostics = strategyResult.value()->takeDiagnostics();
  const auto terminal =
      std::ranges::find_if(diagnostics.records, [](const auto &record) {
        return record.kind == bte::strategy::StrategyDiagnosticKind::terminal;
      });
  ASSERT_NE(terminal, diagnostics.records.end());
  EXPECT_EQ(terminal->errorCode, "Canceled");
  EXPECT_EQ(terminal->hook, "on_finish");
}
#else
TEST(PythonWorkerStrategyTest, pythonWorkerRequiresPosixProcessSupport) {
  GTEST_SKIP() << "Python Strategy workers require POSIX descriptor passing.";
}
#endif

} // namespace
