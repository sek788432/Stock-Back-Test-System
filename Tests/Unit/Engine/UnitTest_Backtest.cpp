#include "Bte/Engine/Backtest.h"

#include "Bte/Core/Bar.h"
#include "Bte/Core/Cancellation.h"
#include "Bte/Results/ResultStore.h"
#include "Bte/Strategy/SelectableStrategy.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

bte::core::Bar makeBar(const int day, const double open, const double close) {
  return bte::core::Bar{
      .ts = bte::core::Timestamp{std::chrono::sys_days{std::chrono::year{2024} /
                                                       1 / day}},
      .open = open,
      .high = std::max(open, close) + 1.0,
      .low = std::min(open, close) - 1.0,
      .close = close,
      .volume = 1'000'000.0,
  };
}

bte::core::Bar makeFlatBar(const int day, const double price) {
  return bte::core::Bar{
      .ts = bte::core::Timestamp{std::chrono::sys_days{std::chrono::year{2024} /
                                                       1 / day}},
      .open = price,
      .high = price,
      .low = price,
      .close = price,
      .volume = 1'000'000.0,
  };
}

void expectRequestedBarsWereProcessed(
    const bte::engine::BacktestResult &result,
    const bte::engine::BacktestRequest &request) {
  EXPECT_EQ(result.barsProcessed, request.bars.size());
  EXPECT_EQ(std::ranges::count_if(
                result.canonicalRecords,
                [](const auto &record) {
                  return record.family == bte::results::RecordFamily::portfolio;
                }),
            request.bars.size());
}

void expectTask17RoundTripFillsUseNextActualOpens(
    const bte::engine::BacktestResult &result,
    const bte::engine::BacktestRequest &request) {
  ASSERT_EQ(result.fills.size(), 2U);
  EXPECT_EQ(result.fills.front().timestamp, request.bars[1].ts);
  EXPECT_EQ(result.fills.front().priceNanodollars, 97'009'700'000);
  EXPECT_EQ(result.fills.back().timestamp, request.bars[2].ts);
  EXPECT_EQ(result.fills.back().priceNanodollars, 99'990'000'000);
}

void expectTask17OrdersFollowActualSignals(
    const bte::engine::BacktestResult &result,
    const bte::engine::BacktestRequest &request) {
  const auto &records = result.canonicalRecords;
  EXPECT_EQ(std::ranges::count_if(records,
                                  [](const auto &record) {
                                    return record.family ==
                                           bte::results::RecordFamily::order;
                                  }),
            2);
  EXPECT_EQ(std::ranges::count_if(records,
                                  [](const auto &record) {
                                    return record.family ==
                                           bte::results::RecordFamily::fill;
                                  }),
            2);
  const auto firstOrder = std::ranges::find_if(records, [](const auto &record) {
    return record.family == bte::results::RecordFamily::order;
  });
  ASSERT_NE(firstOrder, records.end());
  EXPECT_EQ(firstOrder->timestamp, request.bars[0].ts);
  const auto secondOrder = std::ranges::find_if(
      std::next(firstOrder), records.end(), [](const auto &record) {
        return record.family == bte::results::RecordFamily::order;
      });
  ASSERT_NE(secondOrder, records.end());
  EXPECT_EQ(secondOrder->timestamp, request.bars[1].ts);
}

void expectNoPreRangeRecords(const bte::engine::BacktestResult &result,
                             const bte::engine::BacktestRequest &request) {
  for (const auto &record : result.canonicalRecords) {
    EXPECT_GE(record.timestamp, request.bars.front().ts);
  }
}

class RequirementsProbeStrategy final : public bte::strategy::Strategy {
public:
  explicit RequirementsProbeStrategy(
      bte::strategy::StrategyRequirements requirements)
      : requirements_(std::move(requirements)) {}

  [[nodiscard]] const bte::strategy::StrategyRequirements &
  requirements() const noexcept override {
    return requirements_;
  }

  [[nodiscard]] bte::core::Result<bte::strategy::StrategyCommandBatch>
  onMarket(const bte::strategy::StrategyContext &context,
           const bte::core::CancellationToken &) override {
    observedHistorySizes.push_back(context.warmupHistory.size());
    observedBars.push_back(static_cast<std::size_t>(context.bar.has_value()));
    if (context.indicatorValues.empty() ||
        !context.indicatorValues.front().value.has_value()) {
      observedIndicatorValues.push_back(std::nullopt);
    } else {
      observedIndicatorValues.push_back(
          context.indicatorValues.front().value->value);
    }
    return bte::strategy::StrategyCommandBatch{};
  }

  std::vector<std::size_t> observedHistorySizes;
  std::vector<std::size_t> observedBars;
  std::vector<std::optional<double>> observedIndicatorValues;

private:
  bte::strategy::StrategyRequirements requirements_;
};

class DiagnosticProbeStrategy final : public bte::strategy::Strategy {
public:
  explicit DiagnosticProbeStrategy(bte::strategy::StrategyDiagnosticBatch batch)
      : batch_(std::move(batch)) {}

  [[nodiscard]] bte::core::Result<bte::strategy::StrategyCommandBatch>
  onMarket(const bte::strategy::StrategyContext &,
           const bte::core::CancellationToken &) override {
    return bte::strategy::StrategyCommandBatch{};
  }

  [[nodiscard]] bte::strategy::StrategyDiagnosticBatch
  takeDiagnostics() override {
    auto result = std::move(batch_);
    batch_ = {};
    return result;
  }

private:
  bte::strategy::StrategyDiagnosticBatch batch_;
};

enum class CancellationPoint { none, onStart, onMarket, onFill, onFinish };

class LifecycleProbeStrategy final : public bte::strategy::Strategy {
public:
  LifecycleProbeStrategy(bte::core::CancellationSource &cancellation,
                         const CancellationPoint cancellationPoint,
                         const bool submitBuy = false,
                         const bool failFinish = false)
      : cancellation_(cancellation), cancellationPoint_(cancellationPoint),
        submitBuy_(submitBuy), failFinish_(failFinish) {}

  [[nodiscard]] bte::core::Result<void>
  onStart(const bte::strategy::StrategyContext &,
          const bte::core::CancellationToken &) override {
    cancelIf(CancellationPoint::onStart);
    return {};
  }

  [[nodiscard]] bte::core::Result<bte::strategy::StrategyCommandBatch>
  onMarket(const bte::strategy::StrategyContext &context,
           const bte::core::CancellationToken &) override {
    cancelIf(CancellationPoint::onMarket);
    if (!submitBuy_ || submitted_) {
      return bte::strategy::StrategyCommandBatch{};
    }
    submitted_ = true;
    return bte::strategy::StrategyCommandBatch{{
        .kind = bte::strategy::StrategyCommandKind::buyMarket,
        .symbol = std::string{context.symbol},
        .quantityShares = 1,
    }};
  }

  [[nodiscard]] bte::core::Result<void>
  onFill(const bte::strategy::StrategyFill &,
         const bte::strategy::StrategyContext &,
         const bte::core::CancellationToken &) override {
    cancelIf(CancellationPoint::onFill);
    return {};
  }

  [[nodiscard]] bte::core::Result<void>
  onFinish(const bte::strategy::StrategyContext &,
           const bte::core::CancellationToken &) override {
    if (failFinish_) {
      return bte::core::makeError(bte::core::ErrorCode::internal,
                                  "simulated finish failure");
    }
    cancelIf(CancellationPoint::onFinish);
    return {};
  }

private:
  void cancelIf(const CancellationPoint point) {
    if (cancellationPoint_ == point) {
      cancellation_.requestCancellation();
    }
  }

  bte::core::CancellationSource &cancellation_;
  CancellationPoint cancellationPoint_ = CancellationPoint::none;
  bool submitBuy_ = false;
  bool failFinish_ = false;
  bool submitted_ = false;
};

} // namespace

TEST(BacktestTest, validatesStrategyHistoryBudgetsAndIndicatorRegistrations) {
  using Registration = bte::strategy::StrategyIndicatorRegistration;
  using Definition = bte::indicators::IndicatorDefinition;
  const auto registration = [](std::string name, Definition definition) {
    return Registration{.name = std::move(name), .definition = definition};
  };
  const Definition validDefinition{};
  const std::vector<bte::strategy::StrategyRequirements> invalidRequirements{
      {.maxHistoryBars = 10'001},
      {.maxHistoryBars = 2, .indicators = {registration("", validDefinition)}},
      {.maxHistoryBars = 2,
       .indicators = {registration("1sma", validDefinition)}},
      {.maxHistoryBars = 2,
       .indicators = {registration("sma-name", validDefinition)}},
      {.maxHistoryBars = 2,
       .indicators = {registration(std::string(65, 's'), validDefinition)}},
      {.maxHistoryBars = 2,
       .indicators = {registration("sma", validDefinition),
                      registration("sma", validDefinition)}},
      {.maxHistoryBars = 14,
       .indicators = {registration(
           "ema", Definition{.kind = bte::indicators::IndicatorKind::ema})}},
      {.maxHistoryBars = 14,
       .indicators = {registration(
           "signal",
           Definition{.output = bte::indicators::IndicatorOutput::signal})}},
      {.maxHistoryBars = 14,
       .indicators = {registration(
           "open", Definition{.field = bte::indicators::BarField::open})}},
      {.maxHistoryBars = 14,
       .indicators = {registration("zero", Definition{.period = 0})}},
      {.maxHistoryBars = 10'000,
       .indicators = {registration("long", Definition{.period = 10'001})}},
      {.maxHistoryBars = 13,
       .indicators = {registration("short", Definition{.period = 14})}},
  };
  const auto bars = std::vector{makeFlatBar(2, 10.0), makeFlatBar(3, 11.0)};

  for (const auto &requirements : invalidRequirements) {
    auto execution = bte::engine::BacktestExecution::start(
        {.bars = bars,
         .initialCapitalMicrodollars = 1'000'000'000,
         .quantityShares = 1,
         .previousDataDays = {}},
        std::make_unique<RequirementsProbeStrategy>(requirements));
    ASSERT_FALSE(execution.ok());
    EXPECT_EQ(execution.error().code, bte::core::ErrorCode::invalidArgument);
  }

  auto insufficientHistory = bte::engine::BacktestExecution::start(
      {.bars = bars,
       .historyBars = {makeFlatBar(1, 9.0)},
       .initialCapitalMicrodollars = 1'000'000'000,
       .quantityShares = 1,
       .previousDataDays = {}},
      std::make_unique<RequirementsProbeStrategy>(
          bte::strategy::StrategyRequirements{.maxHistoryBars = 0}));
  ASSERT_FALSE(insufficientHistory.ok());
  EXPECT_EQ(insufficientHistory.error().code,
            bte::core::ErrorCode::invalidArgument);
}

TEST(BacktestTest, rejectsInvalidWarmupHistoryBeforeStartingStrategy) {
  const auto actualBars = std::vector{makeFlatBar(20, 10.0)};
  const auto validHistory =
      std::vector{makeFlatBar(18, 9.0), makeFlatBar(19, 9.5)};
  const auto startWithHistory = [&](std::vector<bte::core::Bar> history) {
    return bte::engine::BacktestExecution::start(
        {.bars = actualBars,
         .historyBars = std::move(history),
         .initialCapitalMicrodollars = 1'000'000'000,
         .quantityShares = 1,
         .previousDataDays = {}},
        std::make_unique<RequirementsProbeStrategy>(
            bte::strategy::StrategyRequirements{.maxHistoryBars = 10'000}));
  };

  auto tooManyBars = std::vector<bte::core::Bar>{};
  tooManyBars.reserve(10'001);
  for (int day = 1; day <= 10'001; ++day) {
    tooManyBars.push_back(makeFlatBar(day, 9.0));
  }
  const auto excessive = startWithHistory(std::move(tooManyBars));

  auto invalidOhlcv = validHistory;
  invalidOhlcv.front().volume = std::numeric_limits<double>::infinity();
  const auto invalidVolume = startWithHistory(std::move(invalidOhlcv));

  auto unrepresentablePrice = validHistory;
  unrepresentablePrice.back() = makeFlatBar(19, 10'000'000'000.0);
  const auto invalidPrice = startWithHistory(std::move(unrepresentablePrice));

  auto runDateHistory = validHistory;
  runDateHistory.back() = makeFlatBar(20, 9.5);
  const auto historyAtRunStart = startWithHistory(std::move(runDateHistory));

  auto decreasingHistory = validHistory;
  std::ranges::reverse(decreasingHistory);
  const auto outOfOrder = startWithHistory(std::move(decreasingHistory));

  for (const auto *result : {&excessive, &invalidVolume, &invalidPrice,
                             &historyAtRunStart, &outOfOrder}) {
    ASSERT_FALSE(result->ok());
    EXPECT_EQ(result->error().code, bte::core::ErrorCode::invalidArgument);
  }
}

TEST(BacktestTest, cancellationDuringStrategyCallbacksRollsBackCurrentSlice) {
  const auto oneBar = std::vector{makeFlatBar(2, 10.0)};
  const auto twoBars = std::vector{makeFlatBar(2, 10.0), makeFlatBar(3, 11.0)};
  const auto makeRequest = [](const std::vector<bte::core::Bar> &bars) {
    return bte::engine::BacktestRequest{
        .bars = bars,
        .symbol = "SYN",
        .initialCapitalMicrodollars = 1'000'000'000,
        .quantityShares = 1,
        .previousDataDays = {},
    };
  };

  bte::core::CancellationSource onStartCancellation;
  auto onStart = bte::engine::BacktestExecution::start(
      makeRequest(oneBar),
      std::make_unique<LifecycleProbeStrategy>(onStartCancellation,
                                               CancellationPoint::onStart));
  ASSERT_TRUE(onStart.ok()) << onStart.error().message;
  const auto canceledAtStart =
      onStart.value()->advance(onStartCancellation.token());
  EXPECT_FALSE(canceledAtStart.ok());
  EXPECT_EQ(onStart.value()->status(), bte::results::RunStatus::canceled);
  EXPECT_EQ(onStart.value()->result().barsProcessed, 0U);

  bte::core::CancellationSource onMarketCancellation;
  auto onMarket = bte::engine::BacktestExecution::start(
      makeRequest(oneBar),
      std::make_unique<LifecycleProbeStrategy>(onMarketCancellation,
                                               CancellationPoint::onMarket));
  ASSERT_TRUE(onMarket.ok()) << onMarket.error().message;
  const auto canceledAtMarket =
      onMarket.value()->advance(onMarketCancellation.token());
  EXPECT_FALSE(canceledAtMarket.ok());
  EXPECT_EQ(onMarket.value()->status(), bte::results::RunStatus::canceled);
  EXPECT_EQ(onMarket.value()->result().barsProcessed, 0U);
  EXPECT_EQ(std::ranges::count_if(
                onMarket.value()->result().canonicalRecords,
                [](const auto &record) {
                  return record.family == bte::results::RecordFamily::portfolio;
                }),
            0);

  bte::core::CancellationSource onFillCancellation;
  auto onFill = bte::engine::BacktestExecution::start(
      makeRequest(twoBars),
      std::make_unique<LifecycleProbeStrategy>(
          onFillCancellation, CancellationPoint::onFill, true));
  ASSERT_TRUE(onFill.ok()) << onFill.error().message;
  ASSERT_TRUE(onFill.value()->advance().ok());
  const auto canceledAtFill =
      onFill.value()->advance(onFillCancellation.token());
  EXPECT_FALSE(canceledAtFill.ok());
  EXPECT_EQ(onFill.value()->status(), bte::results::RunStatus::canceled);
  EXPECT_EQ(onFill.value()->result().barsProcessed, 1U);
  EXPECT_TRUE(onFill.value()->result().fills.empty());

  bte::core::CancellationSource onFinishCancellation;
  auto onFinish = bte::engine::BacktestExecution::start(
      makeRequest(oneBar),
      std::make_unique<LifecycleProbeStrategy>(onFinishCancellation,
                                               CancellationPoint::onFinish));
  ASSERT_TRUE(onFinish.ok()) << onFinish.error().message;
  const auto canceledAtFinish =
      onFinish.value()->advance(onFinishCancellation.token());
  EXPECT_FALSE(canceledAtFinish.ok());
  EXPECT_EQ(onFinish.value()->status(), bte::results::RunStatus::canceled);
  EXPECT_EQ(onFinish.value()->result().barsProcessed, 1U);
}

TEST(BacktestTest, finishFailureProducesFailedTerminalResult) {
  bte::core::CancellationSource cancellation;
  auto execution = bte::engine::BacktestExecution::start(
      {.bars = {makeFlatBar(2, 10.0)},
       .initialCapitalMicrodollars = 1'000'000'000,
       .quantityShares = 1,
       .previousDataDays = {}},
      std::make_unique<LifecycleProbeStrategy>(
          cancellation, CancellationPoint::none, false, true));
  ASSERT_TRUE(execution.ok()) << execution.error().message;

  const auto finished = execution.value()->advance();

  ASSERT_FALSE(finished.ok());
  EXPECT_EQ(finished.error().message, "simulated finish failure");
  EXPECT_EQ(execution.value()->status(), bte::results::RunStatus::failed);
  ASSERT_TRUE(execution.value()->terminalError().has_value());
  EXPECT_EQ(execution.value()->terminalError()->code,
            bte::core::ErrorCode::internal);
}

TEST(BacktestTest, boundedHistorySeedsDeclaredStreamingIndicators) {
  auto requirements = bte::strategy::StrategyRequirements{
      .maxHistoryBars = 2,
      .indicators = {{.name = "close_sma",
                      .definition = bte::indicators::IndicatorDefinition{
                          .kind = bte::indicators::IndicatorKind::sma,
                          .period = 2,
                          .output = bte::indicators::IndicatorOutput::value,
                          .field = bte::indicators::BarField::close}}}};
  auto strategy =
      std::make_unique<RequirementsProbeStrategy>(std::move(requirements));
  auto *observer = strategy.get();
  const auto request = bte::engine::BacktestRequest{
      .bars = {makeBar(3, 30.0, 30.0), makeBar(4, 40.0, 40.0)},
      .historyBars = {makeBar(1, 10.0, 10.0), makeBar(2, 20.0, 20.0)},
      .symbol = "SYN",
      .initialCapitalMicrodollars = 1'000'000'000,
      .quantityShares = 1,
      .previousDataDays = {},
  };

  auto execution =
      bte::engine::BacktestExecution::start(request, std::move(strategy));
  ASSERT_TRUE(execution.ok()) << execution.error().message;
  ASSERT_TRUE(execution.value()->runToCompletion().ok());
  ASSERT_EQ(observer->observedIndicatorValues.size(), 2U);
  EXPECT_EQ(observer->observedHistorySizes, (std::vector<std::size_t>{2, 2}));
  EXPECT_EQ(observer->observedBars, (std::vector<std::size_t>{1, 1}));
  EXPECT_DOUBLE_EQ(observer->observedIndicatorValues[0].value(), 25.0);
  EXPECT_DOUBLE_EQ(observer->observedIndicatorValues[1].value(), 35.0);
  EXPECT_EQ(execution.value()->result().barsProcessed, request.bars.size());
  EXPECT_EQ(std::ranges::count_if(
                execution.value()->result().canonicalRecords,
                [](const auto &record) {
                  return record.family == bte::results::RecordFamily::portfolio;
                }),
            request.bars.size());
}

TEST(BacktestTest, preservesStructuredAndTerminalStrategyDiagnostics) {
  auto diagnostics = bte::strategy::StrategyDiagnosticBatch{
      .records = {{.kind = bte::strategy::StrategyDiagnosticKind::log,
                   .hook = "on_market",
                   .level = "warning",
                   .message = "risk threshold reached",
                   .canonicalFieldsJson = R"({"drawdown":4.5})"},
                  {.kind = bte::strategy::StrategyDiagnosticKind::terminal,
                   .hook = "on_market",
                   .level = "error",
                   .errorCode = "strategyRuntimeError",
                   .message = "script stopped",
                   .sourceLine = 42,
                   .sourceColumn = 7,
                   .traceback = "ValueError: script stopped",
                   .omittedBytes = 7,
                   .omittedEntries = 2}},
      .omittedLogBytes = 23,
      .omittedLogEntries = 1,
      .complete = false};
  auto execution = bte::engine::BacktestExecution::start(
      {.bars = {makeFlatBar(2, 10.0)},
       .initialCapitalMicrodollars = 1'000'000'000,
       .quantityShares = 1,
       .previousDataDays = {}},
      std::make_unique<DiagnosticProbeStrategy>(std::move(diagnostics)));

  ASSERT_TRUE(execution.ok()) << execution.error().message;
  ASSERT_TRUE(execution.value()->runToCompletion().ok());
  const auto &result = execution.value()->result();
  EXPECT_EQ(result.status, bte::results::RunStatus::completed);
  EXPECT_FALSE(result.diagnosticsComplete);
  const auto log =
      std::ranges::find_if(result.canonicalRecords, [](const auto &record) {
        return record.family == bte::results::RecordFamily::log &&
               record.text.find("risk threshold reached") != std::string::npos;
      });
  ASSERT_NE(log, result.canonicalRecords.end());
  EXPECT_NE(log->text.find(R"("drawdown":4.5)"), std::string::npos);
  const auto truncation =
      std::ranges::find_if(result.canonicalRecords, [](const auto &record) {
        return record.family == bte::results::RecordFamily::log &&
               record.text.find("DiagnosticsTruncated") != std::string::npos;
      });
  ASSERT_NE(truncation, result.canonicalRecords.end());
  EXPECT_NE(truncation->text.find(R"("omitted_bytes":"30")"),
            std::string::npos);
  EXPECT_NE(truncation->text.find(R"("omitted_entries":"1")"),
            std::string::npos);
  const auto terminal =
      std::ranges::find_if(result.canonicalRecords, [](const auto &record) {
        return record.family == bte::results::RecordFamily::terminalDiagnostic;
      });
  ASSERT_NE(terminal, result.canonicalRecords.end());
  EXPECT_NE(terminal->text.find(R"("sourceColumn":7)"), std::string::npos);
  EXPECT_NE(terminal->text.find(R"("sourceLine":42)"), std::string::npos);
  EXPECT_NE(terminal->text.find(R"("omittedEntries":2)"), std::string::npos);
  EXPECT_NE(terminal->text.find("ValueError: script stopped"),
            std::string::npos);
}

TEST(BacktestTest, malformedStructuredStrategyDiagnosticFailsTheRun) {
  auto execution = bte::engine::BacktestExecution::start(
      {.bars = {makeFlatBar(2, 10.0)},
       .initialCapitalMicrodollars = 1'000'000'000,
       .quantityShares = 1,
       .previousDataDays = {}},
      std::make_unique<DiagnosticProbeStrategy>(
          bte::strategy::StrategyDiagnosticBatch{
              .records = {{.kind = bte::strategy::StrategyDiagnosticKind::log,
                           .hook = "on_market",
                           .level = "error",
                           .message = "malformed fields",
                           .canonicalFieldsJson = "[]"}}}));

  ASSERT_TRUE(execution.ok()) << execution.error().message;
  const auto advanced = execution.value()->advance();
  ASSERT_FALSE(advanced.ok());
  EXPECT_EQ(advanced.error().code,
            bte::core::ErrorCode::strategyProtocolViolation);
  EXPECT_EQ(execution.value()->status(), bte::results::RunStatus::failed);
  EXPECT_EQ(execution.value()->result().barsProcessed, 0U);
  EXPECT_FALSE(execution.value()->result().diagnosticsComplete);
}

TEST(BacktestTest, starterMarketBuyFillsAtNextBarOpenAndMarksFinalEquity) {
  const bte::engine::BacktestRequest request{
      .bars = {makeBar(2, 100.0, 100.0), makeBar(3, 110.0, 120.0)},
      .initialCapitalMicrodollars = 2'000'000'000,
      .quantityShares = 10,
      .selectableStrategy = {},
      .previousDataDays = {},
  };

  const auto result = bte::engine::runBacktest(request);

  ASSERT_TRUE(result.ok());
  EXPECT_EQ(result.value().orderStatus,
            bte::engine::StarterOrderStatus::filled);
  ASSERT_TRUE(result.value().fill.has_value());
  EXPECT_EQ(result.value().fill->timestamp, request.bars[1].ts);
  EXPECT_EQ(result.value().fill->priceNanodollars, 110'011'000'000);
  EXPECT_EQ(result.value().fill->amountMicrodollars, 1'100'110'000);
  EXPECT_EQ(result.value().cashMicrodollars, 899'890'000);
  EXPECT_EQ(result.value().marketValueMicrodollars, 1'200'000'000);
  EXPECT_EQ(result.value().equityMicrodollars, 2'099'890'000);
  EXPECT_EQ(result.value().pnlMicrodollars, 99'890'000);
  EXPECT_EQ(result.value().barsProcessed, 2U);
}

TEST(BacktestTest, selectableStrategyHistoryDoesNotAddExecutedBarsOrRecords) {
  using namespace std::chrono;
  const auto request =
      bte::engine::BacktestRequest{
          .bars = {makeBar(2, 95.0, 95.0), makeBar(3, 97.0, 98.0),
                   makeBar(4, 100.0, 100.0)},
          .symbol = "SYN",
          .initialCapitalMicrodollars = 2'000'000'000,
          .quantityShares = 10,
          .selectableStrategy =
              bte::strategy::SelectableStrategyPlan{
                  .buy = {.conditions = {bte::strategy::Condition{
                              .source = bte::strategy::ConditionSource::
                                  previousDataDayCloseChangePercent,
                              .comparison =
                                  bte::strategy::Comparison::lessThanOrEqual,
                              .threshold = -5.0,
                              .thresholdDomain =
                                  bte::indicators::NumericDomain::percent,
                          }}},
                  .sell = {.conditions = {bte::strategy::Condition{
                               .source = bte::strategy::ConditionSource::
                                   previousDataDayCloseChangePercent,
                               .comparison = bte::strategy::Comparison::
                                   greaterThanOrEqual,
                               .threshold = 3.0,
                               .thresholdDomain =
                                   bte::indicators::NumericDomain::percent,
                           }}},
              },
          .previousDataDayClose =
              bte::strategy::PreviousDataDayClose{
                  .timestamp =
                      bte::core::Timestamp{sys_days{year{2024} / January / 1} +
                                           hours{22}},
                  .close = 100.0,
              },
          .previousDataDays = {},
      };

  const auto result = bte::engine::runBacktest(request);

  ASSERT_TRUE(result.ok()) << result.error().message;
  expectRequestedBarsWereProcessed(result.value(), request);
  expectTask17RoundTripFillsUseNextActualOpens(result.value(), request);
  expectTask17OrdersFollowActualSignals(result.value(), request);
  expectNoPreRangeRecords(result.value(), request);
}

TEST(BacktestTest,
     historicalDataDayWarmupSignalsWithoutAddingExecutedBarsOrRecords) {
  using namespace std::chrono;
  const auto bars = std::vector<bte::core::Bar>{makeBar(2, 103.0, 103.0),
                                                makeBar(3, 104.0, 104.0)};
  const auto request = bte::engine::BacktestRequest{
      .bars = bars,
      .symbol = "SYN",
      .initialCapitalMicrodollars = 2'000'000'000,
      .quantityShares = 10,
      .selectableStrategy =
          bte::strategy::SelectableStrategyPlan{
              .buy =
                  {.conditions = {bte::strategy::Condition{
                       .source =
                           bte::strategy::ConditionSource::dataDayGainPercent,
                       .comparison =
                           bte::strategy::Comparison::greaterThanOrEqual,
                       .threshold = 1.0,
                       .thresholdDomain =
                           bte::indicators::NumericDomain::percent,
                       .lookbackPeriod = 1}}},
              .sell = {},
          },
      .previousDataDays = {bte::strategy::PreviousDataDaySummary{
          .utcDateStart =
              bte::core::Timestamp{sys_days{year{2024} / January / 1}},
          .high = 102.0,
          .low = 99.0,
          .close = 101.0,
      }},
  };

  const auto result = bte::engine::runBacktest(request);

  ASSERT_TRUE(result.ok()) << result.error().message;
  EXPECT_EQ(result.value().barsProcessed, bars.size());
  ASSERT_TRUE(result.value().fill.has_value());
  EXPECT_EQ(result.value().fill->timestamp, bars[1].ts);
  expectNoPreRangeRecords(result.value(), request);
}

TEST(BacktestTest, historicalDataDayBuyAndSellSignalsFillAtNextSelectedOpens) {
  using namespace std::chrono;
  const auto makeHourlyBar = [](const sys_days date, const hours hour,
                                const double open, const double high,
                                const double low, const double close) {
    return bte::core::Bar{.ts = bte::core::Timestamp{date + hour},
                          .open = open,
                          .high = high,
                          .low = low,
                          .close = close,
                          .volume = 1'000'000.0};
  };
  const auto request =
      bte::engine::BacktestRequest{
          .bars =
              {
                  makeHourlyBar(sys_days{year{2024} / January / 2}, hours{9},
                                102.0, 103.0, 102.0, 103.0),
                  makeHourlyBar(sys_days{year{2024} / January / 2}, hours{10},
                                104.0, 105.0, 104.0, 105.0),
                  makeHourlyBar(sys_days{year{2024} / January / 3}, hours{9},
                                94.0, 94.0, 94.0, 94.0),
                  makeHourlyBar(sys_days{year{2024} / January / 3}, hours{10},
                                93.0, 93.0, 93.0, 93.0),
              },
          .symbol = "SYN",
          .initialCapitalMicrodollars = 2'000'000'000,
          .quantityShares = 10,
          .selectableStrategy =
              bte::strategy::SelectableStrategyPlan{
                  .buy = {.conditions = {bte::strategy::Condition{
                              .source = bte::strategy::ConditionSource::
                                  dataDayGainPercent,
                              .comparison =
                                  bte::strategy::Comparison::greaterThanOrEqual,
                              .threshold = 1.0,
                              .thresholdDomain =
                                  bte::indicators::NumericDomain::percent,
                              .lookbackPeriod = 1}}},
                  .sell = {.conditions =
                               {
                                   bte::strategy::Condition{.source =
                                                                bte::strategy::ConditionSource::periodHighDrawdownPercent,
                                                            .comparison = bte::strategy::Comparison::greaterThanOrEqual,
                                                            .threshold = 10.0,
                                                            .thresholdDomain =
                                                                bte::indicators::NumericDomain::percent,
                                                            .lookbackPeriod =
                                                                1}}},
              },
          .previousDataDays = {bte::strategy::PreviousDataDaySummary{
              .utcDateStart =
                  bte::core::Timestamp{sys_days{year{2024} / January / 1}},
              .high = 102.0,
              .low = 99.0,
              .close = 101.0,
          }},
      };

  const auto result = bte::engine::runBacktest(request);

  ASSERT_TRUE(result.ok()) << result.error().message;
  expectRequestedBarsWereProcessed(result.value(), request);
  ASSERT_EQ(result.value().fills.size(), 2U);
  EXPECT_EQ(result.value().fills[0].side, bte::engine::BacktestOrderSide::buy);
  EXPECT_EQ(result.value().fills[0].timestamp, request.bars[1].ts);
  EXPECT_EQ(result.value().fills[1].side, bte::engine::BacktestOrderSide::sell);
  EXPECT_EQ(result.value().fills[1].timestamp, request.bars[3].ts);
  EXPECT_EQ(result.value().positionShares, 0);
  expectNoPreRangeRecords(result.value(), request);
}

TEST(BacktestTest,
     canonicalTimelineOrdersSameTimestampFillBeforePostSlicePortfolio) {
  const bte::engine::BacktestRequest request{
      .bars = {makeBar(2, 100.0, 100.0), makeBar(3, 110.0, 120.0)},
      .symbol = "SYN",
      .initialCapitalMicrodollars = 2'000'000'000,
      .quantityShares = 10,
      .previousDataDays = {},
  };

  const auto result = bte::engine::runBacktest(request);

  ASSERT_TRUE(result.ok()) << result.error().message;
  const auto &records = result.value().canonicalRecords;
  ASSERT_EQ(records.size(), 6);
  EXPECT_EQ(records[0].family, bte::results::RecordFamily::warning);
  EXPECT_EQ(records[1].family, bte::results::RecordFamily::order);
  EXPECT_EQ(records[2].family, bte::results::RecordFamily::portfolio);
  EXPECT_EQ(records[3].family, bte::results::RecordFamily::fill);
  EXPECT_EQ(records[4].family, bte::results::RecordFamily::cost);
  EXPECT_EQ(records[5].family, bte::results::RecordFamily::portfolio);
  EXPECT_EQ(records[3].timestamp, records[4].timestamp);
  EXPECT_EQ(records[4].timestamp, records[5].timestamp);
  EXPECT_EQ(records[4].amountMicrodollars, 110'000);
  EXPECT_EQ(records[4].text, "slippage");
  EXPECT_EQ(records[5].cashMicrodollars, 899'890'000);
  EXPECT_EQ(records[5].marketValueMicrodollars, 1'200'000'000);
  EXPECT_EQ(records[5].equityMicrodollars, 2'099'890'000);
  EXPECT_EQ(records[5].positionShares, 10);
  for (std::size_t index = 0; index < records.size(); ++index) {
    EXPECT_EQ(records[index].sequence, index);
    EXPECT_EQ(records[index].symbol, "SYN");
  }
}

TEST(BacktestTest, canonicalTimelineHasOnePostSliceCheckpointForEveryBar) {
  const auto request = bte::engine::BacktestRequest{
      .bars = {makeFlatBar(2, 10.0), makeFlatBar(3, 10.0),
               makeFlatBar(4, 10.0)},
      .symbol = "SYN",
      .initialCapitalMicrodollars = 1'000'000'000,
      .quantityShares = 1,
      .selectableStrategy =
          bte::strategy::SelectableStrategyPlan{
              .buy = {.conditions = {bte::strategy::Condition{
                          .source = bte::strategy::ConditionSource::barField,
                          .comparison = bte::strategy::Comparison::greaterThan,
                          .threshold = 20.0,
                      }}},
              .sell = {},
          },
      .previousDataDays = {},
  };

  const auto first = bte::engine::runBacktest(request);
  const auto second = bte::engine::runBacktest(request);

  ASSERT_TRUE(first.ok()) << first.error().message;
  ASSERT_TRUE(second.ok()) << second.error().message;
  EXPECT_EQ(first.value().canonicalRecords, second.value().canonicalRecords);
  EXPECT_EQ(std::ranges::count_if(
                first.value().canonicalRecords,
                [](const auto &record) {
                  return record.family == bte::results::RecordFamily::portfolio;
                }),
            request.bars.size());
}

TEST(BacktestTest, unaffordableNextOpenRejectsOrderWithoutChangingCash) {
  const bte::engine::BacktestRequest request{
      .bars = {makeBar(2, 50.0, 50.0), makeBar(3, 150.0, 160.0)},
      .initialCapitalMicrodollars = 1'000'000'000,
      .quantityShares = 10,
      .selectableStrategy = {},
      .previousDataDays = {},
  };

  const auto result = bte::engine::runBacktest(request);

  ASSERT_TRUE(result.ok());
  EXPECT_EQ(result.value().orderStatus,
            bte::engine::StarterOrderStatus::rejectedInsufficientCash);
  EXPECT_FALSE(result.value().fill.has_value());
  EXPECT_EQ(result.value().cashMicrodollars, 1'000'000'000);
  EXPECT_EQ(result.value().equityMicrodollars, 1'000'000'000);
  EXPECT_EQ(result.value().pnlMicrodollars, 0);
}

TEST(BacktestTest, oneBarCancelsOrderBecauseNoFutureMarketDataExists) {
  const bte::engine::BacktestRequest request{
      .bars = {makeBar(2, 100.0, 105.0)},
      .initialCapitalMicrodollars = 1'000'000'000,
      .quantityShares = 1,
      .selectableStrategy = {},
      .previousDataDays = {},
  };

  const auto result = bte::engine::runBacktest(request);

  ASSERT_TRUE(result.ok());
  EXPECT_EQ(result.value().orderStatus,
            bte::engine::StarterOrderStatus::cancelledNoFutureMarketData);
  EXPECT_FALSE(result.value().fill.has_value());
  EXPECT_EQ(result.value().barsProcessed, 1U);
  EXPECT_EQ(result.value().equityMicrodollars, 1'000'000'000);
}

TEST(BacktestTest, selectableConditionsExecuteBuyAndSellOnlyAtNextActualOpen) {
  const auto
      request =
          bte::engine::BacktestRequest{
              .bars =
                  {
                      makeFlatBar(2, 100.0),
                      makeFlatBar(3, 110.0),
                      makeFlatBar(4, 100.0),
                      makeFlatBar(5, 90.0),
                  },
              .initialCapitalMicrodollars = 2'000'000'000,
              .quantityShares = 10,
              .selectableStrategy =
                  bte::strategy::SelectableStrategyPlan{
                      .buy =
                          {
                              .logic = bte::strategy::ConditionLogic::all,
                              .conditions = {bte::strategy::Condition{
                                  .source = bte::strategy::ConditionSource::
                                      closeChangePercent,
                                  .comparison =
                                      bte::strategy::Comparison::greaterThan,
                                  .threshold = 5.0,
                                  .thresholdDomain =
                                      bte::indicators::NumericDomain::percent,
                              }},
                          },
                      .sell =
                          {
                              .logic = bte::strategy::ConditionLogic::all,
                              .conditions =
                                  {
                                      bte::strategy::Condition{
                                          .source =
                                              bte::strategy::ConditionSource::
                                                  closeChangePercent,
                                          .comparison =
                                              bte::strategy::Comparison::lessThan,
                                          .threshold = -5.0,
                                          .thresholdDomain = bte::indicators::NumericDomain::percent,
                                      }},
                          },
                  },
              .previousDataDays = {},
          };

  const auto result = bte::engine::runBacktest(request);

  ASSERT_TRUE(result.ok());
  ASSERT_EQ(result.value().fills.size(), 2U);
  EXPECT_EQ(result.value().fills[0].side, bte::engine::BacktestOrderSide::buy);
  EXPECT_EQ(result.value().fills[0].timestamp, request.bars[2].ts);
  EXPECT_EQ(result.value().fills[1].side, bte::engine::BacktestOrderSide::sell);
  EXPECT_EQ(result.value().fills[1].timestamp, request.bars[3].ts);
  EXPECT_EQ(result.value().positionShares, 0);
  EXPECT_EQ(result.value().cashMicrodollars, 1'899'810'000);
  EXPECT_EQ(result.value().equityMicrodollars, 1'899'810'000);
}

TEST(BacktestTest,
     selectableConditionsReportNoSignalPendingAndRejectedOutcomes) {
  const auto noSignal = bte::engine::runBacktest({
      .bars = {makeFlatBar(2, 10.0), makeFlatBar(3, 10.0)},
      .initialCapitalMicrodollars = 1'000'000'000,
      .quantityShares = 1,
      .selectableStrategy =
          bte::strategy::SelectableStrategyPlan{
              .buy = {.conditions = {bte::strategy::Condition{
                          .source = bte::strategy::ConditionSource::barField,
                          .comparison = bte::strategy::Comparison::greaterThan,
                          .threshold = 20.0,
                      }}},
              .sell = {},
          },
      .previousDataDays = {},
  });
  const auto pending = bte::engine::runBacktest({
      .bars = {makeFlatBar(2, 5.0), makeFlatBar(3, 10.0)},
      .initialCapitalMicrodollars = 1'000'000'000,
      .quantityShares = 1,
      .selectableStrategy =
          bte::strategy::SelectableStrategyPlan{
              .buy = {.conditions = {bte::strategy::Condition{
                          .source = bte::strategy::ConditionSource::barField,
                          .comparison = bte::strategy::Comparison::greaterThan,
                          .threshold = 7.0,
                      }}},
              .sell = {},
          },
      .previousDataDays = {},
  });
  const auto rejected = bte::engine::runBacktest({
      .bars = {makeFlatBar(2, 10.0), makeFlatBar(3, 10.0)},
      .initialCapitalMicrodollars = 1,
      .quantityShares = 1,
      .selectableStrategy =
          bte::strategy::SelectableStrategyPlan{
              .buy = {.conditions = {bte::strategy::Condition{
                          .source = bte::strategy::ConditionSource::barField,
                          .comparison = bte::strategy::Comparison::greaterThan,
                          .threshold = 0.0,
                      }}},
              .sell = {},
          },
      .previousDataDays = {},
  });

  ASSERT_TRUE(noSignal.ok());
  ASSERT_TRUE(pending.ok());
  ASSERT_TRUE(rejected.ok());
  EXPECT_EQ(noSignal.value().orderStatus,
            bte::engine::StarterOrderStatus::completedNoSignal);
  EXPECT_EQ(pending.value().orderStatus,
            bte::engine::StarterOrderStatus::cancelledNoFutureMarketData);
  EXPECT_EQ(rejected.value().orderStatus,
            bte::engine::StarterOrderStatus::rejectedInsufficientCash);
  const auto expectNoFill = [](const bte::engine::BacktestResult &result,
                               const std::int64_t initialCapital) {
    EXPECT_FALSE(result.fill.has_value());
    EXPECT_TRUE(result.fills.empty());
    EXPECT_EQ(result.positionShares, 0);
    EXPECT_EQ(result.cashMicrodollars, initialCapital);
    EXPECT_EQ(result.marketValueMicrodollars, 0);
    EXPECT_EQ(result.equityMicrodollars, initialCapital);
    EXPECT_EQ(result.pnlMicrodollars, 0);
  };
  expectNoFill(noSignal.value(), 1'000'000'000);
  expectNoFill(pending.value(), 1'000'000'000);
  expectNoFill(rejected.value(), 1);
}

TEST(BacktestTest, selectableConditionsPropagateExecutionAndPlanErrors) {
  constexpr auto exclusivePriceUpperBound =
      9'223'372'036'854'775'808.0 / 1'000'000'000.0;
  const auto nearMaximumPrice = std::nextafter(exclusivePriceUpperBound, 0.0);
  const auto alwaysBuy = bte::strategy::Condition{
      .source = bte::strategy::ConditionSource::barField,
      .comparison = bte::strategy::Comparison::greaterThan,
      .threshold = 0.0,
  };
  const auto buyOverflow = bte::engine::runBacktest({
      .bars = {makeFlatBar(2, 1.0), makeFlatBar(3, nearMaximumPrice)},
      .initialCapitalMicrodollars = std::numeric_limits<std::int64_t>::max(),
      .quantityShares = 1,
      .selectableStrategy =
          bte::strategy::SelectableStrategyPlan{
              .buy = {.conditions = {alwaysBuy}},
              .sell = {},
          },
      .previousDataDays = {},
  });
  const auto sellUnderflow = bte::engine::runBacktest({
      .bars = {makeFlatBar(2, 0.000000001), makeFlatBar(3, 0.000000001),
               makeFlatBar(4, 0.000000001)},
      .initialCapitalMicrodollars = 1,
      .quantityShares = 1,
      .selectableStrategy =
          bte::strategy::SelectableStrategyPlan{
              .buy = {.conditions = {alwaysBuy}},
              .sell = {.conditions = {alwaysBuy}},
          },
      .previousDataDays = {},
  });
  const auto invalidPlan = bte::engine::runBacktest({
      .bars = {makeFlatBar(2, 1.0), makeFlatBar(3, 1.0)},
      .initialCapitalMicrodollars = 1'000'000,
      .quantityShares = 1,
      .selectableStrategy = bte::strategy::SelectableStrategyPlan{},
      .previousDataDays = {},
  });

  EXPECT_FALSE(buyOverflow.ok());
  EXPECT_FALSE(sellUnderflow.ok());
  EXPECT_FALSE(invalidPlan.ok());
  EXPECT_EQ(buyOverflow.error().code, bte::core::ErrorCode::invalidArgument);
  EXPECT_EQ(sellUnderflow.error().code, bte::core::ErrorCode::invalidArgument);
  EXPECT_EQ(invalidPlan.error().code,
            bte::core::ErrorCode::strategyCompileFailed);
}

TEST(BacktestTest, selectableConditionsPropagateNonFiniteIndicatorOutput) {
  auto overflowBar = makeFlatBar(2, 1'000'000'000.0);
  overflowBar.volume = std::numeric_limits<double>::max();
  const auto result =
      bte::engine::runBacktest(
          {
              .bars = {overflowBar},
              .initialCapitalMicrodollars = 1'000'000,
              .quantityShares = 1,
              .selectableStrategy =
                  bte::strategy::SelectableStrategyPlan{
                      .buy = {.conditions = {bte::strategy::Condition{
                                  .source =
                                      bte::strategy::ConditionSource::indicator,
                                  .comparison =
                                      bte::strategy::Comparison::greaterThan,
                                  .threshold = 0.0,
                                  .indicator =
                                      {
                                          .kind = bte::indicators::IndicatorKind::vwap,
                                          .period = 1,
                                      },
                              }}},
                      .sell = {},
                  },
              .previousDataDays = {},
          });

  EXPECT_FALSE(result.ok());
  EXPECT_EQ(result.error().code, bte::core::ErrorCode::invalidArgument);
}

TEST(BacktestTest, selectableConditionsPropagateEveryReachableAccountingError) {
  constexpr auto highPrice = 9'000'000'000.0;
  const auto alwaysBuy = bte::strategy::Condition{
      .source = bte::strategy::ConditionSource::barField,
      .comparison = bte::strategy::Comparison::greaterThan,
      .threshold = 0.0,
  };
  const auto buyAmountOverflow = bte::engine::runBacktest({
      .bars = {makeFlatBar(2, 0.000000001), makeFlatBar(3, highPrice)},
      .initialCapitalMicrodollars = std::numeric_limits<std::int64_t>::max(),
      .quantityShares = 1'000'000'000,
      .selectableStrategy =
          bte::strategy::SelectableStrategyPlan{
              .buy = {.conditions = {alwaysBuy}},
              .sell = {},
          },
      .previousDataDays = {},
  });
  const auto sellAmountOverflow = bte::engine::runBacktest({
      .bars = {makeFlatBar(2, 0.000000001), makeFlatBar(3, 0.000000001),
               makeFlatBar(4, highPrice)},
      .initialCapitalMicrodollars = 3'000'000,
      .quantityShares = 1'000'000'000,
      .selectableStrategy =
          bte::strategy::SelectableStrategyPlan{
              .buy = {.conditions = {alwaysBuy}},
              .sell = {.conditions = {alwaysBuy}},
          },
      .previousDataDays = {},
  });
  const auto sellCashOverflow = bte::engine::runBacktest({
      .bars = {makeFlatBar(2, 0.000000001), makeFlatBar(3, 0.000000001),
               makeFlatBar(4, 10.0)},
      .initialCapitalMicrodollars = std::numeric_limits<std::int64_t>::max(),
      .quantityShares = 1,
      .selectableStrategy =
          bte::strategy::SelectableStrategyPlan{
              .buy = {.conditions = {alwaysBuy}},
              .sell = {.conditions = {alwaysBuy}},
          },
      .previousDataDays = {},
  });
  const auto marketValueOverflow = bte::engine::runBacktest({
      .bars = {makeFlatBar(2, 0.000000001), makeFlatBar(3, 0.000000001),
               makeFlatBar(4, highPrice)},
      .initialCapitalMicrodollars = 3'000'000,
      .quantityShares = 1'000'000'000,
      .selectableStrategy =
          bte::strategy::SelectableStrategyPlan{
              .buy = {.conditions = {alwaysBuy}},
              .sell = {},
          },
      .previousDataDays = {},
  });
  const auto equityOverflow = bte::engine::runBacktest({
      .bars = {makeFlatBar(2, 0.000000001), makeFlatBar(3, 0.000000001),
               makeFlatBar(4, highPrice)},
      .initialCapitalMicrodollars = std::numeric_limits<std::int64_t>::max(),
      .quantityShares = 1'000,
      .selectableStrategy =
          bte::strategy::SelectableStrategyPlan{
              .buy = {.conditions = {alwaysBuy}},
              .sell = {},
          },
      .previousDataDays = {},
  });

  EXPECT_FALSE(buyAmountOverflow.ok());
  EXPECT_FALSE(sellAmountOverflow.ok());
  EXPECT_FALSE(sellCashOverflow.ok());
  EXPECT_FALSE(marketValueOverflow.ok());
  EXPECT_FALSE(equityOverflow.ok());
  EXPECT_EQ(buyAmountOverflow.error().code,
            bte::core::ErrorCode::invalidArgument);
  EXPECT_EQ(sellAmountOverflow.error().code,
            bte::core::ErrorCode::invalidArgument);
  EXPECT_EQ(sellCashOverflow.error().code,
            bte::core::ErrorCode::invalidArgument);
  EXPECT_EQ(marketValueOverflow.error().code,
            bte::core::ErrorCode::invalidArgument);
  EXPECT_EQ(equityOverflow.error().code, bte::core::ErrorCode::invalidArgument);
}

TEST(BacktestTest, invalidConfigurationAndBarsReturnInvalidArgument) {
  auto validBars =
      std::vector{makeBar(2, 100.0, 100.0), makeBar(3, 101.0, 102.0)};

  const auto noCapital = bte::engine::runBacktest({
      .bars = validBars,
      .initialCapitalMicrodollars = 0,
      .quantityShares = 1,
      .selectableStrategy = {},
      .previousDataDays = {},
  });
  const auto noQuantity = bte::engine::runBacktest({
      .bars = validBars,
      .initialCapitalMicrodollars = 1'000'000,
      .quantityShares = 0,
      .selectableStrategy = {},
      .previousDataDays = {},
  });
  const auto noBars = bte::engine::runBacktest({
      .bars = {},
      .initialCapitalMicrodollars = 1'000'000,
      .quantityShares = 1,
      .selectableStrategy = {},
      .previousDataDays = {},
  });
  auto invalidBars = validBars;
  invalidBars[1].high = 1.0;
  const auto invalidBar = bte::engine::runBacktest({
      .bars = invalidBars,
      .initialCapitalMicrodollars = 1'000'000,
      .quantityShares = 1,
      .selectableStrategy = {},
      .previousDataDays = {},
  });

  EXPECT_FALSE(noCapital.ok());
  EXPECT_FALSE(noQuantity.ok());
  EXPECT_FALSE(noBars.ok());
  EXPECT_FALSE(invalidBar.ok());
  EXPECT_EQ(noCapital.error().code, bte::core::ErrorCode::invalidArgument);
  EXPECT_EQ(noQuantity.error().code, bte::core::ErrorCode::invalidArgument);
  EXPECT_EQ(noBars.error().code, bte::core::ErrorCode::invalidArgument);
  EXPECT_EQ(invalidBar.error().code, bte::core::ErrorCode::invalidArgument);
}

TEST(BacktestTest, quantityAboveStarterLimitIsRejectedAtBoundary) {
  const auto result = bte::engine::runBacktest({
      .bars = {makeBar(2, 2.0, 2.0), makeBar(3, 2.0, 2.0)},
      .initialCapitalMicrodollars = 5'000'000'000'000'000,
      .quantityShares = 1'000'000'001,
      .selectableStrategy = {},
      .previousDataDays = {},
  });

  EXPECT_FALSE(result.ok());
  EXPECT_EQ(result.error().code, bte::core::ErrorCode::invalidArgument);
}

TEST(BacktestTest, maximumStarterQuantityIsAcceptedAtBoundary) {
  const auto result = bte::engine::runBacktest({
      .bars = {makeBar(2, 2.0, 2.0), makeBar(3, 2.0, 2.0)},
      .initialCapitalMicrodollars = 5'000'000'000'000'000,
      .quantityShares = 1'000'000'000,
      .selectableStrategy = {},
      .previousDataDays = {},
  });

  ASSERT_TRUE(result.ok());
  EXPECT_EQ(result.value().orderStatus,
            bte::engine::StarterOrderStatus::filled);
  ASSERT_TRUE(result.value().fill.has_value());
  EXPECT_EQ(result.value().fill->quantityShares, 1'000'000'000);
}

TEST(BacktestTest, exactAffordabilityFillsAndOneMicrodollarLessRejects) {
  const auto bars =
      std::vector{makeBar(2, 100.0, 100.0), makeBar(3, 100.0, 100.0)};

  const auto exact = bte::engine::runBacktest({
      .bars = bars,
      .initialCapitalMicrodollars = 1'000'100'000,
      .quantityShares = 10,
      .selectableStrategy = {},
      .previousDataDays = {},
  });
  const auto below = bte::engine::runBacktest({
      .bars = bars,
      .initialCapitalMicrodollars = 1'000'099'999,
      .quantityShares = 10,
      .selectableStrategy = {},
      .previousDataDays = {},
  });
  const auto above = bte::engine::runBacktest({
      .bars = bars,
      .initialCapitalMicrodollars = 1'000'100'001,
      .quantityShares = 10,
      .selectableStrategy = {},
      .previousDataDays = {},
  });

  ASSERT_TRUE(exact.ok());
  EXPECT_EQ(exact.value().orderStatus, bte::engine::StarterOrderStatus::filled);
  EXPECT_EQ(exact.value().cashMicrodollars, 0);
  ASSERT_TRUE(below.ok());
  EXPECT_EQ(below.value().orderStatus,
            bte::engine::StarterOrderStatus::rejectedInsufficientCash);
  EXPECT_EQ(below.value().cashMicrodollars, 1'000'099'999);
  EXPECT_FALSE(below.value().fill.has_value());
  ASSERT_TRUE(above.ok());
  EXPECT_EQ(above.value().orderStatus, bte::engine::StarterOrderStatus::filled);
  EXPECT_EQ(above.value().cashMicrodollars, 1);
}

TEST(BacktestTest, minimumCapitalIsAcceptedAtBoundary) {
  const auto result = bte::engine::runBacktest({
      .bars = {makeBar(2, 2.0, 2.0), makeBar(3, 2.0, 2.0)},
      .initialCapitalMicrodollars = 1,
      .quantityShares = 1,
      .selectableStrategy = {},
      .previousDataDays = {},
  });

  ASSERT_TRUE(result.ok());
  EXPECT_EQ(result.value().orderStatus,
            bte::engine::StarterOrderStatus::rejectedInsufficientCash);
  EXPECT_EQ(result.value().cashMicrodollars, 1);
}

TEST(BacktestTest, duplicateAndDecreasingTimestampsAreRejected) {
  auto duplicate =
      std::vector{makeBar(2, 100.0, 100.0), makeBar(2, 101.0, 101.0)};
  auto decreasing =
      std::vector{makeBar(3, 100.0, 100.0), makeBar(2, 101.0, 101.0)};

  const auto duplicateResult = bte::engine::runBacktest({
      .bars = duplicate,
      .initialCapitalMicrodollars = 1'000'000'000,
      .quantityShares = 1,
      .selectableStrategy = {},
      .previousDataDays = {},
  });
  const auto decreasingResult = bte::engine::runBacktest({
      .bars = decreasing,
      .initialCapitalMicrodollars = 1'000'000'000,
      .quantityShares = 1,
      .selectableStrategy = {},
      .previousDataDays = {},
  });

  EXPECT_FALSE(duplicateResult.ok());
  EXPECT_FALSE(decreasingResult.ok());
  EXPECT_EQ(duplicateResult.error().code,
            bte::core::ErrorCode::invalidArgument);
  EXPECT_EQ(decreasingResult.error().code,
            bte::core::ErrorCode::invalidArgument);
}

TEST(BacktestTest, priceThatNormalizesToZeroIsRejected) {
  const auto tinyBar = bte::core::Bar{
      .ts = bte::core::Timestamp{std::chrono::sys_days{std::chrono::year{2024} /
                                                       1 / 2}},
      .open = 0.0000000004,
      .high = 0.0000000004,
      .low = 0.0000000004,
      .close = 0.0000000004,
      .volume = 1.0,
  };
  auto nextTinyBar = tinyBar;
  nextTinyBar.ts = bte::core::Timestamp{
      std::chrono::sys_days{std::chrono::year{2024} / 1 / 3}};
  const auto result = bte::engine::runBacktest({
      .bars = {tinyBar, nextTinyBar},
      .initialCapitalMicrodollars = 1,
      .quantityShares = 1,
      .selectableStrategy = {},
      .previousDataDays = {},
  });

  EXPECT_FALSE(result.ok());
  EXPECT_EQ(result.error().code, bte::core::ErrorCode::invalidArgument);
}

TEST(BacktestTest, priceConversionRejectsUnsafeBoundaryAndAcceptsBelowIt) {
  constexpr auto exclusivePriceUpperBound =
      9'223'372'036'854'775'808.0 / 1'000'000'000.0;
  const auto safePrice = std::nextafter(exclusivePriceUpperBound, 0.0);

  const auto unsafe = bte::engine::runBacktest({
      .bars = {makeFlatBar(2, exclusivePriceUpperBound)},
      .initialCapitalMicrodollars = std::numeric_limits<std::int64_t>::max(),
      .quantityShares = 1,
      .selectableStrategy = {},
      .previousDataDays = {},
  });
  const auto safe = bte::engine::runBacktest({
      .bars = {makeFlatBar(2, safePrice)},
      .initialCapitalMicrodollars = std::numeric_limits<std::int64_t>::max(),
      .quantityShares = 1,
      .selectableStrategy = {},
      .previousDataDays = {},
  });

  EXPECT_FALSE(unsafe.ok());
  EXPECT_EQ(unsafe.error().code, bte::core::ErrorCode::invalidArgument);
  EXPECT_TRUE(safe.ok());
}

TEST(BacktestTest, everyBarMustBeFiniteAndRepresentableAtTheEngineSeam) {
  auto bars = std::vector{makeBar(2, 100.0, 100.0), makeBar(3, 110.0, 110.0),
                          makeBar(4, 120.0, 120.0), makeBar(5, 130.0, 130.0)};
  bars[2].open = std::numeric_limits<double>::infinity();
  bars[2].high = std::numeric_limits<double>::infinity();
  bars[2].low = std::numeric_limits<double>::infinity();
  bars[2].close = std::numeric_limits<double>::infinity();

  const auto result = bte::engine::runBacktest({
      .bars = bars,
      .initialCapitalMicrodollars = 1'000'000'000,
      .quantityShares = 1,
      .selectableStrategy = {},
      .previousDataDays = {},
  });
  auto infiniteVolumeBars =
      std::vector{makeBar(2, 100.0, 100.0), makeBar(3, 110.0, 110.0)};
  infiniteVolumeBars.front().volume = std::numeric_limits<double>::infinity();
  const auto infiniteVolume = bte::engine::runBacktest({
      .bars = infiniteVolumeBars,
      .initialCapitalMicrodollars = 1'000'000'000,
      .quantityShares = 1,
      .selectableStrategy = {},
      .previousDataDays = {},
  });

  EXPECT_FALSE(result.ok());
  EXPECT_FALSE(infiniteVolume.ok());
  EXPECT_EQ(result.error().code, bte::core::ErrorCode::invalidArgument);
  EXPECT_EQ(infiniteVolume.error().code, bte::core::ErrorCode::invalidArgument);
}

TEST(BacktestTest, checkedArithmeticRejectsSlippageAndMarketValueOverflow) {
  constexpr auto exclusivePriceUpperBound =
      9'223'372'036'854'775'808.0 / 1'000'000'000.0;
  const auto nearMaximumPrice = std::nextafter(exclusivePriceUpperBound, 0.0);
  const auto slippageOverflow = bte::engine::runBacktest({
      .bars = {makeFlatBar(2, 1.0), makeFlatBar(3, nearMaximumPrice)},
      .initialCapitalMicrodollars = std::numeric_limits<std::int64_t>::max(),
      .quantityShares = 1,
      .selectableStrategy = {},
      .previousDataDays = {},
  });
  const auto marketValueOverflow = bte::engine::runBacktest({
      .bars = {makeFlatBar(2, 1.0), makeFlatBar(3, 1.0),
               makeFlatBar(4, 9'000'000'000.0)},
      .initialCapitalMicrodollars = std::numeric_limits<std::int64_t>::max(),
      .quantityShares = 1'000'000'000,
      .selectableStrategy = {},
      .previousDataDays = {},
  });
  const auto fillAmountOverflow = bte::engine::runBacktest({
      .bars = {makeFlatBar(2, 1.0), makeFlatBar(3, 9'000'000'000.0)},
      .initialCapitalMicrodollars = std::numeric_limits<std::int64_t>::max(),
      .quantityShares = 1'000'000'000,
      .selectableStrategy = {},
      .previousDataDays = {},
  });
  const auto finalEquityOverflow = bte::engine::runBacktest({
      .bars = {makeFlatBar(2, 1.0), makeFlatBar(3, 1.0),
               makeFlatBar(4, 9'000'000'000.0)},
      .initialCapitalMicrodollars = std::numeric_limits<std::int64_t>::max(),
      .quantityShares = 1,
      .selectableStrategy = {},
      .previousDataDays = {},
  });

  EXPECT_FALSE(slippageOverflow.ok());
  EXPECT_FALSE(marketValueOverflow.ok());
  EXPECT_FALSE(fillAmountOverflow.ok());
  EXPECT_FALSE(finalEquityOverflow.ok());
  EXPECT_EQ(slippageOverflow.error().code,
            bte::core::ErrorCode::invalidArgument);
  EXPECT_EQ(marketValueOverflow.error().code,
            bte::core::ErrorCode::invalidArgument);
  EXPECT_EQ(fillAmountOverflow.error().code,
            bte::core::ErrorCode::invalidArgument);
  EXPECT_EQ(finalEquityOverflow.error().code,
            bte::core::ErrorCode::invalidArgument);
  EXPECT_NE(slippageOverflow.error().message.find("accounting value"),
            std::string::npos);
  EXPECT_NE(marketValueOverflow.error().message.find("accounting value"),
            std::string::npos);
  EXPECT_NE(fillAmountOverflow.error().message.find("accounting value"),
            std::string::npos);
  EXPECT_NE(finalEquityOverflow.error().message.find("accounting value"),
            std::string::npos);
}

TEST(BacktestTest, subMicrodollarFillRoundsToNearestMicrodollar) {
  const auto result = bte::engine::runBacktest({
      .bars = {makeFlatBar(2, 0.0000005), makeFlatBar(3, 0.0000005)},
      .initialCapitalMicrodollars = 1,
      .quantityShares = 1,
      .selectableStrategy = {},
      .previousDataDays = {},
  });

  ASSERT_TRUE(result.ok());
  EXPECT_EQ(result.value().orderStatus,
            bte::engine::StarterOrderStatus::filled);
  ASSERT_TRUE(result.value().fill.has_value());
  EXPECT_EQ(result.value().fill->priceNanodollars, 501);
  EXPECT_EQ(result.value().fill->amountMicrodollars, 1);
}

TEST(BacktestTest, priceNormalizationUsesHalfEvenTies) {
  const auto tieToEven = bte::engine::runBacktest({
      .bars = {makeFlatBar(2, 0.0000000025)},
      .initialCapitalMicrodollars = 1,
      .quantityShares = 1,
      .selectableStrategy = {},
      .previousDataDays = {},
  });
  const auto tieToOdd = bte::engine::runBacktest({
      .bars = {makeFlatBar(2, 0.0000000035)},
      .initialCapitalMicrodollars = 1,
      .quantityShares = 1,
      .selectableStrategy = {},
      .previousDataDays = {},
  });

  ASSERT_TRUE(tieToEven.ok());
  ASSERT_TRUE(tieToOdd.ok());
  EXPECT_EQ(tieToEven.value().finalPriceNanodollars, 2);
  EXPECT_EQ(tieToOdd.value().finalPriceNanodollars, 4);
}

TEST(BacktestTest, monetaryHalfEvenUsesTheEntireIntegerAmount) {
  struct Example {
    std::int64_t shares;
    double finalPrice;
    std::int64_t marketValue;
    std::int64_t cash;
    std::int64_t equity;
    std::int64_t pnl;
  };
  const std::array examples{
      Example{1, 1.0000015, 1'000'002, 998'999'900, 999'999'902, -98},
      Example{3, 1.0000015, 3'000'004, 996'999'700, 999'999'704, -296},
      Example{5, 1.0000015, 5'000'008, 994'999'500, 999'999'508, -492},
      Example{2, 1.0000015, 2'000'003, 997'999'800, 999'999'803, -197},
      Example{1, 1.000001499, 1'000'001, 998'999'900, 999'999'901, -99},
      Example{1, 1.000001501, 1'000'002, 998'999'900, 999'999'902, -98},
      Example{1, 1.0000005, 1'000'000, 998'999'900, 999'999'900, -100},
      Example{3, 1.0000005, 3'000'002, 996'999'700, 999'999'702, -298},
      Example{5, 1.0000005, 5'000'002, 994'999'500, 999'999'502, -498},
      Example{1, 1.000000499, 1'000'000, 998'999'900, 999'999'900, -100},
      Example{1, 1.000000501, 1'000'001, 998'999'900, 999'999'901, -99},
  };
  for (const auto &example : examples) {
    SCOPED_TRACE(example.shares);
    SCOPED_TRACE(example.finalPrice);
    const auto result = bte::engine::runBacktest({
        .bars = {makeFlatBar(2, 1.0), makeFlatBar(3, 1.0),
                 makeFlatBar(4, example.finalPrice)},
        .initialCapitalMicrodollars = 1'000'000'000,
        .quantityShares = example.shares,
        .previousDataDays = {},
    });
    ASSERT_TRUE(result.ok()) << result.error().message;
    EXPECT_EQ(result.value().marketValueMicrodollars, example.marketValue);
    EXPECT_EQ(result.value().cashMicrodollars, example.cash);
    EXPECT_EQ(result.value().equityMicrodollars, example.equity);
    EXPECT_EQ(result.value().pnlMicrodollars, example.pnl);
    ASSERT_FALSE(result.value().canonicalRecords.empty());
    const auto &portfolio = result.value().canonicalRecords.back();
    EXPECT_EQ(portfolio.family, bte::results::RecordFamily::portfolio);
    EXPECT_EQ(portfolio.marketValueMicrodollars, example.marketValue);
    EXPECT_EQ(portfolio.cashMicrodollars, example.cash);
    EXPECT_EQ(portfolio.equityMicrodollars, example.equity);
    EXPECT_EQ(portfolio.pnlMicrodollars, example.pnl);
  }
}

TEST(BacktestTest, monetaryConversionChecksTheFullSumAtTheInt64Boundary) {
  const auto makeRequest = [](const double finalPrice) {
    return bte::engine::BacktestRequest{
        .bars = {makeFlatBar(2, 0.000000001), makeFlatBar(3, 0.000000001),
                 makeFlatBar(4, finalPrice)},
        .initialCapitalMicrodollars = 2'000'000,
        .quantityShares = 1'000'000'000,
        .previousDataDays = {},
    };
  };
  const auto inside = bte::engine::runBacktest(makeRequest(9'223.372036854));
  const auto outside = bte::engine::runBacktest(makeRequest(9'223.372036855));

  ASSERT_TRUE(inside.ok()) << inside.error().message;
  EXPECT_EQ(inside.value().cashMicrodollars, 0);
  EXPECT_EQ(inside.value().marketValueMicrodollars, 9'223'372'036'854'000'000);
  EXPECT_EQ(inside.value().equityMicrodollars, 9'223'372'036'854'000'000);
  EXPECT_EQ(inside.value().pnlMicrodollars, 9'223'372'036'852'000'000);
  ASSERT_FALSE(outside.ok());
  EXPECT_EQ(outside.error().code, bte::core::ErrorCode::invalidArgument);
  EXPECT_EQ(outside.error().message,
            "accounting value exceeds the supported range");
}

TEST(BacktestTest, requestedStopCancelsBeforeExecution) {
  bte::core::CancellationSource cancellation;
  cancellation.requestCancellation();

  const auto result = bte::engine::runBacktest(
      {
          .bars = {makeBar(2, 100.0, 100.0), makeBar(3, 110.0, 120.0)},
          .initialCapitalMicrodollars = 2'000'000'000,
          .quantityShares = 10,
          .selectableStrategy = {},
          .previousDataDays = {},
      },
      cancellation.token());

  EXPECT_FALSE(result.ok());
  EXPECT_EQ(result.error().code, bte::core::ErrorCode::cancelled);
}

TEST(BacktestTest, requiredFinalMarkCannotPrecedeLastActualBar) {
  const auto bars = std::vector{makeFlatBar(2, 100.0), makeFlatBar(3, 101.0)};

  const auto execution = bte::engine::BacktestExecution::start({
      .bars = bars,
      .initialCapitalMicrodollars = 1'000'000'000,
      .quantityShares = 1,
      .previousDataDays = {},
      .requiredFinalMarkTimestamp = bars.front().ts,
  });

  ASSERT_FALSE(execution.ok());
  EXPECT_EQ(execution.error().code, bte::core::ErrorCode::invalidArgument);
  EXPECT_EQ(execution.error().message,
            "required final mark cannot precede the last actual bar");
}

TEST(BacktestTest, canceledPacedExecutionExposesErrorAndRejectsFurtherAdvance) {
  auto execution = bte::engine::BacktestExecution::start({
      .bars = {makeFlatBar(2, 100.0), makeFlatBar(3, 101.0)},
      .initialCapitalMicrodollars = 1'000'000'000,
      .quantityShares = 1,
      .previousDataDays = {},
  });
  ASSERT_TRUE(execution.ok()) << execution.error().message;
  bte::core::CancellationSource cancellation;
  cancellation.requestCancellation();

  const auto canceled = execution.value()->advance(cancellation.token());

  ASSERT_FALSE(canceled.ok());
  EXPECT_EQ(execution.value()->status(), bte::results::RunStatus::canceled);
  ASSERT_TRUE(execution.value()->terminalError().has_value());
  EXPECT_EQ(execution.value()->terminalError()->code,
            bte::core::ErrorCode::cancelled);
  const auto repeated = execution.value()->advance();
  ASSERT_FALSE(repeated.ok());
  EXPECT_EQ(repeated.error().code, bte::core::ErrorCode::invalidArgument);
  EXPECT_EQ(repeated.error().message, "backtest execution is already terminal");
  EXPECT_EQ(execution.value()->result().barsProcessed, 0U);
}
