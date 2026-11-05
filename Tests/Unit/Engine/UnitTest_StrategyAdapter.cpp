#include "Bte/Engine/Backtest.h"

#include "Bte/Indicators/StreamingIndicator.h"
#include "Bte/Strategy/Strategy.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <utility>
#include <vector>

namespace {

bte::core::Bar makeBar(const int day, const double price) {
  using namespace std::chrono;
  return {.ts = bte::core::Timestamp{sys_days{year{2024} / January / day} +
                                     hours{9}},
          .open = price,
          .high = price,
          .low = price,
          .close = price,
          .volume = 100.0};
}

bte::core::Bar makeBarAt(const bte::core::Timestamp timestamp,
                         const double open, const double high, const double low,
                         const double close) {
  return {.ts = timestamp,
          .open = open,
          .high = high,
          .low = low,
          .close = close,
          .volume = 100.0};
}

bte::engine::BacktestRequest makeRequest(std::vector<bte::core::Bar> bars) {
  return {.bars = std::move(bars),
          .symbol = "SYN",
          .initialCapitalMicrodollars = 10'000'000'000,
          .quantityShares = 1,
          .selectableStrategy = std::nullopt};
}

class ScriptedStrategy final : public bte::strategy::Strategy {
public:
  explicit ScriptedStrategy(
      std::vector<bte::strategy::StrategyCommandBatch> decisions)
      : decisions_(std::move(decisions)) {}

  bte::core::Result<void>
  onStart(const bte::strategy::StrategyContext &context,
          const bte::core::CancellationToken &) override {
    calls.push_back("start");
    sawStartWithoutBar = !context.bar.has_value();
    return {};
  }

  bte::core::Result<bte::strategy::StrategyCommandBatch>
  onMarket(const bte::strategy::StrategyContext &context,
           const bte::core::CancellationToken &) override {
    calls.push_back("market");
    marketPositions.push_back(context.positionShares);
    if (nextDecision_ >= decisions_.size()) {
      return bte::strategy::StrategyCommandBatch{};
    }
    return decisions_[nextDecision_++];
  }

  bte::core::Result<void>
  onFill(const bte::strategy::StrategyFill &fill,
         const bte::strategy::StrategyContext &context,
         const bte::core::CancellationToken &) override {
    calls.push_back(fill.side == bte::strategy::StrategyFillSide::buy
                        ? "fill-buy"
                        : "fill-sell");
    fillPositions.push_back(context.positionShares);
    return {};
  }

  bte::core::Result<void>
  onFinish(const bte::strategy::StrategyContext &context,
           const bte::core::CancellationToken &) override {
    calls.push_back("finish");
    sawFinishWithBar = context.bar.has_value();
    return {};
  }

  std::vector<std::string> calls;
  std::vector<std::int64_t> marketPositions;
  std::vector<std::int64_t> fillPositions;
  bool sawStartWithoutBar = false;
  bool sawFinishWithBar = false;

private:
  std::vector<bte::strategy::StrategyCommandBatch> decisions_;
  std::size_t nextDecision_ = 0;
};

class DiagnosticThenFailureStrategy final : public bte::strategy::Strategy {
public:
  bte::core::Result<void>
  onStart(const bte::strategy::StrategyContext &,
          const bte::core::CancellationToken &) override {
    pending_.records.push_back({
        .kind = bte::strategy::StrategyDiagnosticKind::log,
        .hook = "on_start",
        .level = "info",
        .message = "started",
        .canonicalFieldsJson = R"({"stage":"setup"})",
    });
    return {};
  }

  bte::core::Result<bte::strategy::StrategyCommandBatch>
  onMarket(const bte::strategy::StrategyContext &,
           const bte::core::CancellationToken &) override {
    if (calls_++ == 0) {
      pending_.records.push_back({
          .kind = bte::strategy::StrategyDiagnosticKind::log,
          .hook = "on_market",
          .level = "info",
          .message = "first slice",
      });
      return bte::strategy::StrategyCommandBatch{};
    }
    pending_.records.push_back({
        .kind = bte::strategy::StrategyDiagnosticKind::log,
        .hook = "on_market",
        .level = "warning",
        .message = "failing slice",
    });
    pending_.records.push_back({
        .kind = bte::strategy::StrategyDiagnosticKind::terminal,
        .hook = "on_market",
        .level = "error",
        .errorCode = "strategyRuntimeError",
        .message = "script failed",
        .sourceLine = 12,
        .traceback = "Traceback (most recent call last):\n  File "
                     "\"strategy.py\", line 12\nValueError: script failed",
    });
    return bte::core::makeError(bte::core::ErrorCode::strategyRuntimeError,
                                "script failed");
  }

  bte::strategy::StrategyDiagnosticBatch takeDiagnostics() override {
    auto result = std::move(pending_);
    pending_ = {};
    return result;
  }

private:
  std::size_t calls_ = 0;
  bte::strategy::StrategyDiagnosticBatch pending_;
};

bte::strategy::StrategyCommand buy(const std::string &symbol,
                                   const std::int64_t quantity) {
  return {.kind = bte::strategy::StrategyCommandKind::buyMarket,
          .symbol = symbol,
          .quantityShares = quantity};
}

bte::strategy::StrategyCommand close(const std::string &symbol) {
  return {.kind = bte::strategy::StrategyCommandKind::closePosition,
          .symbol = symbol,
          .quantityShares = 0};
}

TEST(StrategyAdapterTest,
     lifecycleOrdersFillsBeforeMarketAndExecutesCommandsAtNextOpen) {
  const auto createLifecycleStrategy = [] {
    return std::make_unique<ScriptedStrategy>(
        std::vector<bte::strategy::StrategyCommandBatch>{
            {buy("SYN", 2)}, {close("SYN")}, {}});
  };
  auto strategy = createLifecycleStrategy();
  auto *observer = strategy.get();
  const auto request =
      makeRequest({makeBar(2, 10.0), makeBar(3, 11.0), makeBar(4, 9.0)});
  auto execution =
      bte::engine::BacktestExecution::start(request, std::move(strategy));
  auto paced =
      bte::engine::BacktestExecution::start(request, createLifecycleStrategy());

  ASSERT_TRUE(execution.ok()) << execution.error().message;
  ASSERT_TRUE(paced.ok()) << paced.error().message;
  ASSERT_TRUE(execution.value()->runToCompletion().ok());
  while (paced.value()->status() == bte::results::RunStatus::running) {
    ASSERT_TRUE(paced.value()->advance().ok());
  }

  EXPECT_EQ(observer->calls,
            (std::vector<std::string>{"start", "market", "fill-buy", "market",
                                      "fill-sell", "market", "finish"}));
  EXPECT_TRUE(observer->sawStartWithoutBar);
  EXPECT_TRUE(observer->sawFinishWithBar);
  EXPECT_EQ(observer->marketPositions, (std::vector<std::int64_t>{0, 2, 0}));
  EXPECT_EQ(observer->fillPositions, (std::vector<std::int64_t>{2, 0}));

  const auto &result = execution.value()->result();
  ASSERT_EQ(result.fills.size(), 2U);
  EXPECT_EQ(result.fills[0].timestamp,
            bte::core::Timestamp{
                std::chrono::sys_days{std::chrono::year{2024} / 1 / 3} +
                std::chrono::hours{9}});
  EXPECT_EQ(result.fills[1].timestamp,
            bte::core::Timestamp{
                std::chrono::sys_days{std::chrono::year{2024} / 1 / 4} +
                std::chrono::hours{9}});
  EXPECT_EQ(result.fills[0].priceNanodollars, 11'001'100'000);
  EXPECT_EQ(result.fills[0].amountMicrodollars, 22'002'200);
  EXPECT_EQ(result.fills[1].priceNanodollars, 8'999'100'000);
  EXPECT_EQ(result.fills[1].amountMicrodollars, 17'998'200);
  EXPECT_EQ(result.cashMicrodollars, 9'995'996'000);
  EXPECT_EQ(result.equityMicrodollars, 9'995'996'000);
  EXPECT_EQ(result.positionShares, 0);
  EXPECT_EQ(result.status, bte::results::RunStatus::completed);
  EXPECT_EQ(paced.value()->result().canonicalRecords, result.canonicalRecords);
}

TEST(StrategyAdapterTest,
     failedSliceRollsBackTradingRecordsButRetainsStructuredDiagnostics) {
  auto execution = bte::engine::BacktestExecution::start(
      makeRequest({makeBar(2, 10.0), makeBar(3, 11.0)}),
      std::make_unique<DiagnosticThenFailureStrategy>());
  ASSERT_TRUE(execution.ok()) << execution.error().message;
  ASSERT_TRUE(execution.value()->advance().ok());
  const auto committedPrefix = execution.value()->result().canonicalRecords;
  ASSERT_EQ(committedPrefix.size(), 4U);
  EXPECT_EQ(committedPrefix[1].family, bte::results::RecordFamily::log);
  EXPECT_EQ(
      committedPrefix[1].text,
      R"({"fields":{"stage":"setup"},"hook":"on_start","level":"info","message":"started"})");
  EXPECT_EQ(committedPrefix.back().family,
            bte::results::RecordFamily::portfolio);

  const auto failed = execution.value()->advance();

  ASSERT_FALSE(failed.ok());
  EXPECT_EQ(execution.value()->result().barsProcessed, 1U);
  const auto &records = execution.value()->result().canonicalRecords;
  ASSERT_EQ(records.size(), committedPrefix.size() + 2U);
  EXPECT_TRUE(std::equal(committedPrefix.begin(), committedPrefix.end(),
                         records.begin()));
  EXPECT_EQ(records[4].family, bte::results::RecordFamily::log);
  EXPECT_EQ(
      records[4].text,
      R"({"fields":{},"hook":"on_market","level":"warning","message":"failing slice"})");
  EXPECT_EQ(records[5].family, bte::results::RecordFamily::terminalDiagnostic);
  EXPECT_EQ(
      records[5].text,
      R"({"errorCode":"strategyRuntimeError","hook":"on_market","message":"script failed","omittedBytes":0,"omittedEntries":0,"sourceColumn":null,"sourceLine":12,"traceback":"Traceback (most recent call last):\n  File \"strategy.py\", line 12\nValueError: script failed"})");
  EXPECT_EQ(records[5].timestamp, makeBar(3, 11.0).ts);
}

TEST(StrategyAdapterTest,
     invalidCommandBatchFailsBeforeAnyCommandOrBarIsCommitted) {
  auto strategy = std::make_unique<ScriptedStrategy>(
      std::vector<bte::strategy::StrategyCommandBatch>{
          {buy("SYN", 1), close("SYN")}});
  auto execution = bte::engine::BacktestExecution::start(
      makeRequest({makeBar(2, 10.0), makeBar(3, 11.0)}), std::move(strategy));
  ASSERT_TRUE(execution.ok()) << execution.error().message;

  const auto advanced = execution.value()->advance();

  ASSERT_FALSE(advanced.ok());
  EXPECT_EQ(advanced.error().code,
            bte::core::ErrorCode::strategyCommandRejected);
  EXPECT_EQ(execution.value()->result().barsProcessed, 0U);
  EXPECT_TRUE(execution.value()->result().fills.empty());
  EXPECT_EQ(execution.value()->result().positionShares, 0);
  EXPECT_EQ(std::ranges::count_if(
                execution.value()->result().canonicalRecords,
                [](const auto &record) {
                  return record.family == bte::results::RecordFamily::order ||
                         record.family == bte::results::RecordFamily::fill ||
                         record.family == bte::results::RecordFamily::portfolio;
                }),
            0);
}

TEST(StrategyAdapterTest, mismatchedSymbolAndInvalidQuantityAreRejected) {
  const auto expectCommandRejected =
      [](bte::strategy::StrategyCommand command) {
        auto strategy = std::make_unique<ScriptedStrategy>(
            std::vector<bte::strategy::StrategyCommandBatch>{
                {std::move(command)}});
        auto execution = bte::engine::BacktestExecution::start(
            makeRequest({makeBar(2, 10.0)}), std::move(strategy));
        ASSERT_TRUE(execution.ok()) << execution.error().message;
        const auto advanced = execution.value()->advance();
        ASSERT_FALSE(advanced.ok());
        EXPECT_EQ(advanced.error().code,
                  bte::core::ErrorCode::strategyCommandRejected);
        EXPECT_EQ(execution.value()->result().barsProcessed, 0U);
      };

  expectCommandRejected(buy("OTHER", 1));
  expectCommandRejected(buy("SYN", 0));
  expectCommandRejected(buy("SYN", 1'000'000'001));
  expectCommandRejected(close("SYN"));
  auto sizedClose = close("SYN");
  sizedClose.quantityShares = 1;
  expectCommandRejected(std::move(sizedClose));
  auto unknown = buy("SYN", 1);
  unknown.kind = static_cast<bte::strategy::StrategyCommandKind>(255);
  expectCommandRejected(std::move(unknown));
}

TEST(StrategyAdapterTest, acceptsQuantityBoundsAndKeepsExistingStarterMode) {
  auto strategy = std::make_unique<ScriptedStrategy>(
      std::vector<bte::strategy::StrategyCommandBatch>{
          {buy("SYN", bte::engine::maximumStarterQuantityShares)}});
  auto execution = bte::engine::BacktestExecution::start(
      makeRequest({makeBar(2, 10.0)}), std::move(strategy));
  ASSERT_TRUE(execution.ok()) << execution.error().message;
  ASSERT_TRUE(execution.value()->runToCompletion().ok());
  EXPECT_EQ(std::ranges::count_if(execution.value()->result().canonicalRecords,
                                  [](const auto &record) {
                                    return record.family ==
                                           bte::results::RecordFamily::order;
                                  }),
            1);

  const auto legacy = bte::engine::runBacktest(
      makeRequest({makeBar(2, 10.0), makeBar(3, 11.0)}));
  ASSERT_TRUE(legacy.ok()) << legacy.error().message;
  EXPECT_EQ(legacy.value().orderStatus,
            bte::engine::StarterOrderStatus::filled);
  EXPECT_EQ(legacy.value().fills.size(), 1U);
}

TEST(StrategyAdapterTest, commandCannotOpenAnotherPositionWhileAlreadyLong) {
  auto strategy = std::make_unique<ScriptedStrategy>(
      std::vector<bte::strategy::StrategyCommandBatch>{{buy("SYN", 1)},
                                                       {buy("SYN", 1)}});
  auto execution = bte::engine::BacktestExecution::start(
      makeRequest({makeBar(2, 10.0), makeBar(3, 11.0), makeBar(4, 12.0)}),
      std::move(strategy));
  ASSERT_TRUE(execution.ok()) << execution.error().message;
  ASSERT_TRUE(execution.value()->advance().ok());

  const auto rejected = execution.value()->advance();

  ASSERT_FALSE(rejected.ok());
  EXPECT_EQ(rejected.error().code,
            bte::core::ErrorCode::strategyCommandRejected);
  EXPECT_EQ(execution.value()->result().barsProcessed, 1U);
  EXPECT_TRUE(execution.value()->result().fills.empty());
  EXPECT_EQ(execution.value()->result().positionShares, 0);
}

TEST(StrategyAdapterTest, finalBarCommandRemainsUnfilledWithoutFutureData) {
  auto strategy = std::make_unique<ScriptedStrategy>(
      std::vector<bte::strategy::StrategyCommandBatch>{{buy("SYN", 1)}});
  auto execution = bte::engine::BacktestExecution::start(
      makeRequest({makeBar(2, 10.0)}), std::move(strategy));
  ASSERT_TRUE(execution.ok()) << execution.error().message;
  ASSERT_TRUE(execution.value()->runToCompletion().ok());

  const auto &result = execution.value()->result();
  EXPECT_TRUE(result.fills.empty());
  EXPECT_EQ(result.positionShares, 0);
  EXPECT_EQ(result.orderStatus,
            bte::engine::StarterOrderStatus::cancelledNoFutureMarketData);
  ASSERT_EQ(std::ranges::count_if(result.canonicalRecords,
                                  [](const auto &record) {
                                    return record.family ==
                                           bte::results::RecordFamily::order;
                                  }),
            1);
}

TEST(StrategyAdapterTest, unaffordableTypedBuyUsesExistingCashRejection) {
  auto strategy = std::make_unique<ScriptedStrategy>(
      std::vector<bte::strategy::StrategyCommandBatch>{{buy("SYN", 10)}, {}});
  auto request = makeRequest({makeBar(2, 50.0), makeBar(3, 150.0)});
  request.initialCapitalMicrodollars = 1'000'000'000;
  auto execution = bte::engine::BacktestExecution::start(std::move(request),
                                                         std::move(strategy));
  ASSERT_TRUE(execution.ok()) << execution.error().message;
  ASSERT_TRUE(execution.value()->runToCompletion().ok());

  const auto &result = execution.value()->result();
  EXPECT_TRUE(result.fills.empty());
  EXPECT_EQ(result.cashMicrodollars, 1'000'000'000);
  EXPECT_EQ(result.positionShares, 0);
  EXPECT_EQ(result.orderStatus,
            bte::engine::StarterOrderStatus::rejectedInsufficientCash);
}

class FillFailStrategy final : public bte::strategy::Strategy {
public:
  [[nodiscard]] bte::core::Result<bte::strategy::StrategyCommandBatch>
  onMarket(const bte::strategy::StrategyContext &,
           const bte::core::CancellationToken &) override {
    if (!submitted_) {
      submitted_ = true;
      return bte::strategy::StrategyCommandBatch{{
          .kind = bte::strategy::StrategyCommandKind::buyMarket,
          .symbol = "SYN",
          .quantityShares = 1,
      }};
    }
    return bte::strategy::StrategyCommandBatch{};
  }

  [[nodiscard]] bte::core::Result<void>
  onFill(const bte::strategy::StrategyFill &,
         const bte::strategy::StrategyContext &,
         const bte::core::CancellationToken &) override {
    return bte::core::makeError(bte::core::ErrorCode::strategyRuntimeError,
                                "fill hook failed");
  }

private:
  bool submitted_ = false;
};

class SmaCrossCommandStrategy final : public bte::strategy::Strategy {
public:
  SmaCrossCommandStrategy(bte::indicators::StreamingIndicator fast,
                          bte::indicators::StreamingIndicator slow)
      : fast_(std::move(fast)), slow_(std::move(slow)) {}

  [[nodiscard]] static bte::core::Result<
      std::unique_ptr<SmaCrossCommandStrategy>>
  create(const std::vector<bte::core::Bar> &warmupBars) {
    auto fast = bte::indicators::StreamingIndicator::create(
        {.kind = bte::indicators::IndicatorKind::sma,
         .period = 2,
         .field = bte::indicators::BarField::close});
    if (!fast.ok()) {
      return fast.error();
    }
    auto slow = bte::indicators::StreamingIndicator::create(
        {.kind = bte::indicators::IndicatorKind::sma,
         .period = 3,
         .field = bte::indicators::BarField::close});
    if (!slow.ok()) {
      return slow.error();
    }
    auto strategy = std::make_unique<SmaCrossCommandStrategy>(
        std::move(fast).value(), std::move(slow).value());
    for (const auto &bar : warmupBars) {
      const auto values = strategy->update(bar);
      if (!values.ok()) {
        return values.error();
      }
      if (values.value().has_value()) {
        strategy->previousPair_ = values.value();
      }
    }
    return strategy;
  }

  [[nodiscard]] bte::core::Result<bte::strategy::StrategyCommandBatch>
  onMarket(const bte::strategy::StrategyContext &context,
           const bte::core::CancellationToken &) override {
    if (!context.bar.has_value()) {
      return bte::core::makeError(bte::core::ErrorCode::strategyRuntimeError,
                                  "market callback did not receive a bar");
    }
    const auto updated = update(context.bar.value());
    if (!updated.ok()) {
      return updated.error();
    }
    if (!updated.value().has_value()) {
      return bte::strategy::StrategyCommandBatch{};
    }
    const auto current = updated.value().value();
    observedPairs_.push_back(current);
    bte::strategy::StrategyCommandBatch commands;
    if (previousPair_.has_value()) {
      const auto previous = previousPair_.value();
      if (context.positionShares == 0 && previous.first <= previous.second &&
          current.first > current.second) {
        commands.push_back(
            {.kind = bte::strategy::StrategyCommandKind::buyMarket,
             .symbol = std::string{context.symbol},
             .quantityShares = 1});
      } else if (context.positionShares > 0 &&
                 previous.first >= previous.second &&
                 current.first < current.second) {
        commands.push_back(
            {.kind = bte::strategy::StrategyCommandKind::closePosition,
             .symbol = std::string{context.symbol},
             .quantityShares = 0});
      }
    }
    previousPair_ = current;
    return commands;
  }

  [[nodiscard]] const std::vector<std::pair<double, double>> &
  observedPairs() const noexcept {
    return observedPairs_;
  }

private:
  [[nodiscard]] bte::core::Result<std::optional<std::pair<double, double>>>
  update(const bte::core::Bar &bar) {
    const auto fast = fast_.update(bar);
    if (!fast.ok()) {
      return fast.error();
    }
    const auto slow = slow_.update(bar);
    if (!slow.ok()) {
      return slow.error();
    }
    if (!fast.value().has_value() || !slow.value().has_value()) {
      return std::optional<std::pair<double, double>>{};
    }
    return std::optional<std::pair<double, double>>{
        {fast.value()->value, slow.value()->value}};
  }

  bte::indicators::StreamingIndicator fast_;
  bte::indicators::StreamingIndicator slow_;
  std::optional<std::pair<double, double>> previousPair_;
  std::vector<std::pair<double, double>> observedPairs_;
};

TEST(StrategyAdapterTest, fillHookFailureRollsBackTheCurrentSliceOnly) {
  auto execution = bte::engine::BacktestExecution::start(
      makeRequest({makeBar(2, 10.0), makeBar(3, 11.0)}),
      std::make_unique<FillFailStrategy>());
  ASSERT_TRUE(execution.ok()) << execution.error().message;
  ASSERT_TRUE(execution.value()->advance().ok());

  const auto failed = execution.value()->advance();

  ASSERT_FALSE(failed.ok());
  EXPECT_EQ(failed.error().code, bte::core::ErrorCode::strategyRuntimeError);
  EXPECT_EQ(execution.value()->result().barsProcessed, 1U);
  EXPECT_TRUE(execution.value()->result().fills.empty());
  EXPECT_EQ(execution.value()->result().positionShares, 0);
  EXPECT_EQ(std::ranges::count_if(execution.value()->result().canonicalRecords,
                                  [](const auto &record) {
                                    return record.family ==
                                           bte::results::RecordFamily::fill;
                                  }),
            0);
  EXPECT_EQ(execution.value()->result().status,
            bte::results::RunStatus::failed);
}

TEST(StrategyAdapterTest,
     warmedSmaCrossUsesActualBarsAcrossTimeGapsAndMatchesIndependentLedger) {
  using namespace std::chrono;
  const auto jan2 = sys_days{year{2024} / January / 2};
  const auto jan3 = sys_days{year{2024} / January / 3};
  const auto jan4 = sys_days{year{2024} / January / 4};
  const auto jan5 = sys_days{year{2024} / January / 5};
  const std::vector<bte::core::Bar> warmupBars{
      makeBarAt(bte::core::Timestamp{jan2 + hours{9}}, 3.0, 3.0, 3.0, 3.0),
      makeBarAt(bte::core::Timestamp{jan2 + hours{10}}, 2.0, 2.0, 2.0, 2.0),
      makeBarAt(bte::core::Timestamp{jan2 + hours{11}}, 1.0, 1.0, 1.0, 1.0),
  };
  auto request = makeRequest({
      makeBarAt(bte::core::Timestamp{jan3 + hours{9}}, 2.0, 2.0, 2.0, 2.0),
      makeBarAt(bte::core::Timestamp{jan3 + hours{12}}, 4.0, 4.0, 4.0, 4.0),
      makeBarAt(bte::core::Timestamp{jan4 + hours{9}}, 5.0, 5.5, 0.5, 1.0),
      makeBarAt(bte::core::Timestamp{jan4 + hours{15}}, 1.0, 1.0, 1.0, 1.0),
      makeBarAt(bte::core::Timestamp{jan5 + hours{9}}, 2.0, 2.5, 0.5, 1.0),
  });
  request.initialCapitalMicrodollars = 100'000'000;

  auto batchStrategy = SmaCrossCommandStrategy::create(warmupBars);
  auto pacedStrategy = SmaCrossCommandStrategy::create(warmupBars);
  ASSERT_TRUE(batchStrategy.ok()) << batchStrategy.error().message;
  ASSERT_TRUE(pacedStrategy.ok()) << pacedStrategy.error().message;
  auto *observed = batchStrategy.value().get();
  auto batch = bte::engine::BacktestExecution::start(
      request, std::move(batchStrategy).value());
  auto paced = bte::engine::BacktestExecution::start(
      request, std::move(pacedStrategy).value());
  ASSERT_TRUE(batch.ok()) << batch.error().message;
  ASSERT_TRUE(paced.ok()) << paced.error().message;
  ASSERT_TRUE(batch.value()->runToCompletion().ok());
  while (paced.value()->status() == bte::results::RunStatus::running) {
    ASSERT_TRUE(paced.value()->advance().ok());
  }

  const auto &result = batch.value()->result();
  ASSERT_EQ(observed->observedPairs().size(), 5U);
  EXPECT_DOUBLE_EQ(observed->observedPairs()[0].first, 1.5);
  EXPECT_DOUBLE_EQ(observed->observedPairs()[0].second, 5.0 / 3.0);
  EXPECT_DOUBLE_EQ(observed->observedPairs()[1].first, 3.0);
  EXPECT_DOUBLE_EQ(observed->observedPairs()[1].second, 7.0 / 3.0);
  EXPECT_DOUBLE_EQ(observed->observedPairs()[3].first, 1.0);
  EXPECT_DOUBLE_EQ(observed->observedPairs()[3].second, 2.0);
  ASSERT_EQ(result.fills.size(), 2U);
  EXPECT_EQ(result.fills[0].timestamp, request.bars[2].ts);
  EXPECT_EQ(result.fills[0].priceNanodollars, 5'000'500'000);
  EXPECT_EQ(result.fills[0].amountMicrodollars, 5'000'500);
  EXPECT_EQ(result.fills[1].timestamp, request.bars[4].ts);
  EXPECT_EQ(result.fills[1].priceNanodollars, 1'999'800'000);
  EXPECT_EQ(result.fills[1].amountMicrodollars, 1'999'800);
  EXPECT_EQ(result.cashMicrodollars, 96'999'300);
  EXPECT_EQ(result.equityMicrodollars, 96'999'300);
  EXPECT_EQ(result.pnlMicrodollars, -3'000'700);
  EXPECT_EQ(result.positionShares, 0);
  EXPECT_EQ(result.barsProcessed, request.bars.size());
  EXPECT_EQ(paced.value()->result().canonicalRecords, result.canonicalRecords);
  EXPECT_TRUE(std::ranges::all_of(
      result.canonicalRecords, [&request](const auto &record) {
        return record.timestamp >= request.bars.front().ts;
      }));
  EXPECT_EQ(std::ranges::count_if(
                result.canonicalRecords,
                [](const auto &record) {
                  return record.family == bte::results::RecordFamily::portfolio;
                }),
            request.bars.size());
}

} // namespace
