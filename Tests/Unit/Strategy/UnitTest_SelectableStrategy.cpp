#include "Bte/Strategy/SelectableStrategy.h"

#include "Bte/Core/Bar.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <limits>

namespace {

bte::core::Bar makeBar(const int day, const double close) {
  return bte::core::Bar{
      .ts = bte::core::Timestamp{std::chrono::sys_days{std::chrono::year{2024} /
                                                       1 / day}},
      .open = close,
      .high = close + 1.0,
      .low = std::max(0.01, close - 1.0),
      .close = close,
      .volume = 100.0,
  };
}

bte::strategy::Condition
priceChangeCondition(const bte::strategy::Comparison comparison,
                     const double threshold) {
  return bte::strategy::Condition{
      .source = bte::strategy::ConditionSource::closeChangePercent,
      .comparison = comparison,
      .threshold = threshold,
      .thresholdDomain = bte::indicators::NumericDomain::percent,
  };
}

TEST(SelectableStrategyTest, allRequiresEveryWarmedConditionBeforeBuying) {
  const auto plan = bte::strategy::SelectableStrategyPlan{
      .buy =
          {
              .logic = bte::strategy::ConditionLogic::all,
              .conditions =
                  {
                      priceChangeCondition(
                          bte::strategy::Comparison::greaterThan, 5.0),
                      bte::strategy::Condition{
                          .source = bte::strategy::ConditionSource::indicator,
                          .comparison = bte::strategy::Comparison::greaterThan,
                          .threshold = 11.0,
                          .indicator =
                              {
                                  .kind = bte::indicators::IndicatorKind::sma,
                                  .period = 2,
                              },
                      },
                  },
          },
  };
  auto created = bte::strategy::SelectableStrategy::create(plan);
  ASSERT_TRUE(created.ok());
  auto strategy = std::move(created).value();

  const auto first = strategy->onBar(makeBar(2, 10.0));
  const auto second = strategy->onBar(makeBar(3, 11.0));
  const auto third = strategy->onBar(makeBar(4, 13.0));

  ASSERT_TRUE(first.ok());
  ASSERT_TRUE(second.ok());
  ASSERT_TRUE(third.ok());
  EXPECT_FALSE(first.value().buy);
  EXPECT_FALSE(second.value().buy);
  EXPECT_TRUE(third.value().buy);
}

TEST(SelectableStrategyTest, anyUsesPriceChangeOrAnIndicatorAndSeparatesSell) {
  const auto plan = bte::strategy::SelectableStrategyPlan{
      .buy =
          {
              .logic = bte::strategy::ConditionLogic::any,
              .conditions =
                  {
                      priceChangeCondition(
                          bte::strategy::Comparison::greaterThan, 9.0),
                      bte::strategy::Condition{
                          .source = bte::strategy::ConditionSource::barField,
                          .comparison = bte::strategy::Comparison::lessThan,
                          .threshold = 5.0,
                      },
                  },
          },
      .sell =
          {
              .logic = bte::strategy::ConditionLogic::all,
              .conditions =
                  {
                      priceChangeCondition(bte::strategy::Comparison::lessThan,
                                           -4.0),
                  },
          },
  };
  auto created = bte::strategy::SelectableStrategy::create(plan);
  ASSERT_TRUE(created.ok());
  auto strategy = std::move(created).value();

  ASSERT_TRUE(strategy->onBar(makeBar(2, 10.0)).ok());
  const auto rising = strategy->onBar(makeBar(3, 11.0));
  const auto falling = strategy->onBar(makeBar(4, 10.0));

  ASSERT_TRUE(rising.ok());
  ASSERT_TRUE(falling.ok());
  EXPECT_TRUE(rising.value().buy);
  EXPECT_FALSE(rising.value().sell);
  EXPECT_FALSE(falling.value().buy);
  EXPECT_TRUE(falling.value().sell);
}

TEST(SelectableStrategyTest, invalidPlansDoNotCreateExecutableStrategies) {
  const auto noBuy = bte::strategy::SelectableStrategy::create({});
  const auto notFinite = bte::strategy::SelectableStrategy::create({
      .buy =
          {
              .conditions = {priceChangeCondition(
                  bte::strategy::Comparison::greaterThan,
                  std::numeric_limits<double>::infinity())},
          },
  });
  const auto invalidIndicator = bte::strategy::SelectableStrategy::create({
      .buy =
          {
              .conditions = {bte::strategy::Condition{
                  .source = bte::strategy::ConditionSource::indicator,
                  .indicator =
                      {
                          .kind = bte::indicators::IndicatorKind::sma,
                          .period = 0,
                      },
              }},
          },
  });

  EXPECT_FALSE(noBuy.ok());
  EXPECT_FALSE(notFinite.ok());
  EXPECT_FALSE(invalidIndicator.ok());
  EXPECT_EQ(noBuy.error().code, bte::core::ErrorCode::strategyCompileFailed);
  EXPECT_EQ(notFinite.error().code,
            bte::core::ErrorCode::strategyCompileFailed);
  EXPECT_EQ(invalidIndicator.error().code,
            bte::core::ErrorCode::strategyCompileFailed);
}

TEST(SelectableStrategyTest, groupsAllowTwoConditionsAndRejectThree) {
  const auto twoConditions = bte::strategy::SelectableStrategy::create({
      .buy =
          {
              .conditions =
                  {
                      priceChangeCondition(
                          bte::strategy::Comparison::greaterThan, 1.0),
                      priceChangeCondition(bte::strategy::Comparison::lessThan,
                                           10.0),
                  },
          },
  });
  const auto threeConditions = bte::strategy::SelectableStrategy::create({
      .buy =
          {
              .conditions =
                  {
                      priceChangeCondition(
                          bte::strategy::Comparison::greaterThan, 1.0),
                      priceChangeCondition(bte::strategy::Comparison::lessThan,
                                           10.0),
                      priceChangeCondition(bte::strategy::Comparison::notEqual,
                                           0.0),
                  },
          },
  });

  EXPECT_TRUE(twoConditions.ok());
  EXPECT_FALSE(threeConditions.ok());
  EXPECT_EQ(threeConditions.error().code,
            bte::core::ErrorCode::strategyCompileFailed);
}

TEST(SelectableStrategyTest, incompatibleThresholdDomainIsRejected) {
  const auto invalid = bte::strategy::SelectableStrategy::create({
      .buy =
          {
              .conditions =
                  {
                      bte::strategy::Condition{
                          .source = bte::strategy::ConditionSource::barField,
                          .comparison = bte::strategy::Comparison::greaterThan,
                          .threshold = 100.0,
                          .thresholdDomain =
                              bte::indicators::NumericDomain::volume,
                          .barField = bte::indicators::BarField::close,
                      },
                  },
          },
  });

  EXPECT_FALSE(invalid.ok());
  EXPECT_EQ(invalid.error().code, bte::core::ErrorCode::strategyCompileFailed);
}

TEST(SelectableStrategyTest, comparisonOperatorsUseTypedBarFieldValues) {
  struct ComparisonCase final {
    bte::strategy::Comparison comparison;
    double close;
    bool expected;
  };
  const auto comparisons = std::array{
      ComparisonCase{bte::strategy::Comparison::greaterThan, 11.0, true},
      ComparisonCase{bte::strategy::Comparison::greaterThan, 9.0, false},
      ComparisonCase{bte::strategy::Comparison::greaterThanOrEqual, 10.0, true},
      ComparisonCase{bte::strategy::Comparison::greaterThanOrEqual, 9.0, false},
      ComparisonCase{bte::strategy::Comparison::lessThan, 9.0, true},
      ComparisonCase{bte::strategy::Comparison::lessThan, 11.0, false},
      ComparisonCase{bte::strategy::Comparison::lessThanOrEqual, 10.0, true},
      ComparisonCase{bte::strategy::Comparison::lessThanOrEqual, 11.0, false},
      ComparisonCase{bte::strategy::Comparison::equal, 10.0, true},
      ComparisonCase{bte::strategy::Comparison::equal, 11.0, false},
      ComparisonCase{bte::strategy::Comparison::notEqual, 10.0, false},
      ComparisonCase{bte::strategy::Comparison::notEqual, 11.0, true},
  };

  for (const auto &comparisonCase : comparisons) {
    auto created = bte::strategy::SelectableStrategy::create({
        .buy =
            {
                .conditions = {bte::strategy::Condition{
                    .source = bte::strategy::ConditionSource::barField,
                    .comparison = comparisonCase.comparison,
                    .threshold = 10.0,
                    .barField = bte::indicators::BarField::close,
                }},
            },
    });
    ASSERT_TRUE(created.ok());
    auto strategy = std::move(created).value();

    const auto signal = strategy->onBar(makeBar(2, comparisonCase.close));
    ASSERT_TRUE(signal.ok());
    EXPECT_EQ(signal.value().buy, comparisonCase.expected);
  }
}

TEST(SelectableStrategyTest, rejectsInvalidLogicAndTypedConditionPlans) {
  const auto invalidLogic = bte::strategy::SelectableStrategy::create({
      .buy =
          {
              .logic = static_cast<bte::strategy::ConditionLogic>(99),
              .conditions = {priceChangeCondition(
                  bte::strategy::Comparison::greaterThan, 1.0)},
          },
  });
  const auto wrongPercentDomain = bte::strategy::SelectableStrategy::create({
      .buy =
          {
              .conditions = {bte::strategy::Condition{
                  .source = bte::strategy::ConditionSource::closeChangePercent,
                  .comparison = bte::strategy::Comparison::greaterThan,
                  .threshold = 1.0,
                  .thresholdDomain = bte::indicators::NumericDomain::price,
              }},
          },
  });
  const auto wrongIndicatorDomain = bte::strategy::SelectableStrategy::create({
      .buy =
          {
              .conditions = {bte::strategy::Condition{
                  .source = bte::strategy::ConditionSource::indicator,
                  .comparison = bte::strategy::Comparison::greaterThan,
                  .threshold = 50.0,
                  .thresholdDomain = bte::indicators::NumericDomain::price,
                  .indicator =
                      {
                          .kind = bte::indicators::IndicatorKind::rsi,
                          .period = 2,
                      },
              }},
          },
  });
  const auto invalidSource = bte::strategy::SelectableStrategy::create({
      .buy = {.conditions = {bte::strategy::Condition{
                  .source = static_cast<bte::strategy::ConditionSource>(99),
              }}},
  });
  const auto invalidComparison = bte::strategy::SelectableStrategy::create({
      .buy = {.conditions = {bte::strategy::Condition{
                  .comparison = static_cast<bte::strategy::Comparison>(99),
              }}},
  });
  const auto invalidBarField = bte::strategy::SelectableStrategy::create({
      .buy = {.conditions = {bte::strategy::Condition{
                  .barField = static_cast<bte::indicators::BarField>(99),
              }}},
  });
  const auto invalidDomain = bte::strategy::SelectableStrategy::create({
      .buy = {.conditions = {bte::strategy::Condition{
                  .thresholdDomain =
                      static_cast<bte::indicators::NumericDomain>(99),
              }}},
  });

  EXPECT_FALSE(invalidLogic.ok());
  EXPECT_FALSE(wrongPercentDomain.ok());
  EXPECT_FALSE(wrongIndicatorDomain.ok());
  EXPECT_FALSE(invalidSource.ok());
  EXPECT_FALSE(invalidComparison.ok());
  EXPECT_FALSE(invalidBarField.ok());
  EXPECT_FALSE(invalidDomain.ok());
  EXPECT_EQ(invalidLogic.error().code,
            bte::core::ErrorCode::strategyCompileFailed);
  EXPECT_EQ(wrongPercentDomain.error().code,
            bte::core::ErrorCode::strategyCompileFailed);
  EXPECT_EQ(wrongIndicatorDomain.error().code,
            bte::core::ErrorCode::strategyCompileFailed);
  EXPECT_EQ(invalidSource.error().code,
            bte::core::ErrorCode::strategyCompileFailed);
  EXPECT_EQ(invalidComparison.error().code,
            bte::core::ErrorCode::strategyCompileFailed);
  EXPECT_EQ(invalidBarField.error().code,
            bte::core::ErrorCode::strategyCompileFailed);
  EXPECT_EQ(invalidDomain.error().code,
            bte::core::ErrorCode::strategyCompileFailed);
}

TEST(SelectableStrategyTest, invalidSellPlanIsRejectedAfterValidBuyPlan) {
  const auto invalidSell = bte::strategy::SelectableStrategy::create({
      .buy =
          {
              .conditions = {priceChangeCondition(
                  bte::strategy::Comparison::greaterThan, 1.0)},
          },
      .sell =
          {
              .conditions = {bte::strategy::Condition{
                  .source = bte::strategy::ConditionSource::indicator,
                  .indicator =
                      {
                          .kind = bte::indicators::IndicatorKind::sma,
                          .period = 0,
                      },
              }},
          },
  });

  EXPECT_FALSE(invalidSell.ok());
  EXPECT_EQ(invalidSell.error().code,
            bte::core::ErrorCode::strategyCompileFailed);
}

TEST(SelectableStrategyTest, moveOperationsPreserveExecutableStrategyState) {
  const auto plan = bte::strategy::SelectableStrategyPlan{
      .buy =
          {
              .conditions = {bte::strategy::Condition{
                  .source = bte::strategy::ConditionSource::barField,
                  .comparison = bte::strategy::Comparison::greaterThan,
                  .threshold = 5.0,
              }},
          },
  };
  auto initialResult = bte::strategy::SelectableStrategy::create(plan);
  auto replacementResult = bte::strategy::SelectableStrategy::create(plan);
  ASSERT_TRUE(initialResult.ok());
  ASSERT_TRUE(replacementResult.ok());
  auto initial = std::move(initialResult).value();
  auto replacement = std::move(replacementResult).value();

  auto moved = std::move(*initial);
  *replacement = std::move(moved);
  const auto signal = replacement->onBar(makeBar(2, 10.0));

  ASSERT_TRUE(signal.ok());
  EXPECT_TRUE(signal.value().buy);
}

TEST(SelectableStrategyTest,
     invalidInputBarIsRejectedBeforeConditionEvaluation) {
  auto created = bte::strategy::SelectableStrategy::create({
      .buy =
          {
              .conditions = {priceChangeCondition(
                  bte::strategy::Comparison::greaterThan, 1.0)},
          },
  });
  ASSERT_TRUE(created.ok());
  auto strategy = std::move(created).value();

  auto invalidBar = makeBar(2, 10.0);
  invalidBar.volume = std::numeric_limits<double>::infinity();
  const auto result = strategy->onBar(invalidBar);

  EXPECT_FALSE(result.ok());
  EXPECT_EQ(result.error().code, bte::core::ErrorCode::invalidArgument);
}

} // namespace
