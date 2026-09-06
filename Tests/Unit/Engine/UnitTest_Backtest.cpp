#include "Bte/Engine/Backtest.h"

#include "Bte/Core/Bar.h"
#include "Bte/Core/Cancellation.h"
#include "Bte/Results/ResultStore.h"
#include "Bte/Strategy/SelectableStrategy.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
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

} // namespace

TEST(BacktestTest, starterMarketBuyFillsAtNextBarOpenAndMarksFinalEquity) {
  const bte::engine::BacktestRequest request{
      .bars = {makeBar(2, 100.0, 100.0), makeBar(3, 110.0, 120.0)},
      .initialCapitalMicrodollars = 2'000'000'000,
      .quantityShares = 10,
      .selectableStrategy = {},
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

TEST(BacktestTest,
     canonicalTimelineOrdersSameTimestampFillBeforePostSlicePortfolio) {
  const bte::engine::BacktestRequest request{
      .bars = {makeBar(2, 100.0, 100.0), makeBar(3, 110.0, 120.0)},
      .symbol = "SYN",
      .initialCapitalMicrodollars = 2'000'000'000,
      .quantityShares = 10,
  };

  const auto result = bte::engine::runBacktest(request);

  ASSERT_TRUE(result.ok()) << result.error().message;
  const auto &records = result.value().canonicalRecords;
  ASSERT_EQ(records.size(), 4);
  EXPECT_EQ(records[0].family, bte::results::RecordFamily::order);
  EXPECT_EQ(records[1].family, bte::results::RecordFamily::portfolio);
  EXPECT_EQ(records[2].family, bte::results::RecordFamily::fill);
  EXPECT_EQ(records[3].family, bte::results::RecordFamily::portfolio);
  EXPECT_EQ(records[2].timestamp, records[3].timestamp);
  EXPECT_EQ(records[3].cashMicrodollars, 899'890'000);
  EXPECT_EQ(records[3].marketValueMicrodollars, 1'200'000'000);
  EXPECT_EQ(records[3].equityMicrodollars, 2'099'890'000);
  EXPECT_EQ(records[3].positionShares, 10);
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
  });
  const auto invalidPlan = bte::engine::runBacktest({
      .bars = {makeFlatBar(2, 1.0), makeFlatBar(3, 1.0)},
      .initialCapitalMicrodollars = 1'000'000,
      .quantityShares = 1,
      .selectableStrategy = bte::strategy::SelectableStrategyPlan{},
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
  });
  const auto noQuantity = bte::engine::runBacktest({
      .bars = validBars,
      .initialCapitalMicrodollars = 1'000'000,
      .quantityShares = 0,
      .selectableStrategy = {},
  });
  const auto noBars = bte::engine::runBacktest({
      .bars = {},
      .initialCapitalMicrodollars = 1'000'000,
      .quantityShares = 1,
      .selectableStrategy = {},
  });
  auto invalidBars = validBars;
  invalidBars[1].high = 1.0;
  const auto invalidBar = bte::engine::runBacktest({
      .bars = invalidBars,
      .initialCapitalMicrodollars = 1'000'000,
      .quantityShares = 1,
      .selectableStrategy = {},
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
  });
  const auto below = bte::engine::runBacktest({
      .bars = bars,
      .initialCapitalMicrodollars = 1'000'099'999,
      .quantityShares = 10,
      .selectableStrategy = {},
  });
  const auto above = bte::engine::runBacktest({
      .bars = bars,
      .initialCapitalMicrodollars = 1'000'100'001,
      .quantityShares = 10,
      .selectableStrategy = {},
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
  });
  const auto decreasingResult = bte::engine::runBacktest({
      .bars = decreasing,
      .initialCapitalMicrodollars = 1'000'000'000,
      .quantityShares = 1,
      .selectableStrategy = {},
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
  });
  const auto safe = bte::engine::runBacktest({
      .bars = {makeFlatBar(2, safePrice)},
      .initialCapitalMicrodollars = std::numeric_limits<std::int64_t>::max(),
      .quantityShares = 1,
      .selectableStrategy = {},
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
  });
  auto infiniteVolumeBars =
      std::vector{makeBar(2, 100.0, 100.0), makeBar(3, 110.0, 110.0)};
  infiniteVolumeBars.front().volume = std::numeric_limits<double>::infinity();
  const auto infiniteVolume = bte::engine::runBacktest({
      .bars = infiniteVolumeBars,
      .initialCapitalMicrodollars = 1'000'000'000,
      .quantityShares = 1,
      .selectableStrategy = {},
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
  });
  const auto marketValueOverflow = bte::engine::runBacktest({
      .bars = {makeFlatBar(2, 1.0), makeFlatBar(3, 1.0),
               makeFlatBar(4, 9'000'000'000.0)},
      .initialCapitalMicrodollars = std::numeric_limits<std::int64_t>::max(),
      .quantityShares = 1'000'000'000,
      .selectableStrategy = {},
  });
  const auto fillAmountOverflow = bte::engine::runBacktest({
      .bars = {makeFlatBar(2, 1.0), makeFlatBar(3, 9'000'000'000.0)},
      .initialCapitalMicrodollars = std::numeric_limits<std::int64_t>::max(),
      .quantityShares = 1'000'000'000,
      .selectableStrategy = {},
  });
  const auto finalEquityOverflow = bte::engine::runBacktest({
      .bars = {makeFlatBar(2, 1.0), makeFlatBar(3, 1.0),
               makeFlatBar(4, 9'000'000'000.0)},
      .initialCapitalMicrodollars = std::numeric_limits<std::int64_t>::max(),
      .quantityShares = 1,
      .selectableStrategy = {},
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
  });
  const auto tieToOdd = bte::engine::runBacktest({
      .bars = {makeFlatBar(2, 0.0000000035)},
      .initialCapitalMicrodollars = 1,
      .quantityShares = 1,
      .selectableStrategy = {},
  });

  ASSERT_TRUE(tieToEven.ok());
  ASSERT_TRUE(tieToOdd.ok());
  EXPECT_EQ(tieToEven.value().finalPriceNanodollars, 2);
  EXPECT_EQ(tieToOdd.value().finalPriceNanodollars, 4);
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
      },
      cancellation.token());

  EXPECT_FALSE(result.ok());
  EXPECT_EQ(result.error().code, bte::core::ErrorCode::cancelled);
}
