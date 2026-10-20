#include "Bte/Strategy/SelectableStrategy.h"

#include "Bte/Core/Bar.h"
#include "SelectableStrategyDetail.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>

namespace {

bte::core::Bar makeBarAt(bte::core::Timestamp timestamp, double close);

bte::core::Bar makeBar(const int day, const double close) {
  return makeBarAt(bte::core::Timestamp{std::chrono::sys_days{
                       std::chrono::year{2024} / 1 / day}},
                   close);
}

bte::core::Bar makeBarAt(const bte::core::Timestamp timestamp,
                         const double close) {
  return bte::core::Bar{
      .ts = timestamp,
      .open = close,
      .high = close + 1.0,
      .low = std::max(0.01, close - 1.0),
      .close = close,
      .volume = 100.0,
  };
}

bte::strategy::Condition
previousDataDayCloseCondition(const bte::strategy::Comparison comparison,
                              const double threshold) {
  return bte::strategy::Condition{
      .source =
          bte::strategy::ConditionSource::previousDataDayCloseChangePercent,
      .comparison = comparison,
      .threshold = threshold,
      .thresholdDomain = bte::indicators::NumericDomain::percent,
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

bte::strategy::Condition
historicalDataDayCondition(const bte::strategy::ConditionSource source,
                           const std::int32_t period,
                           const double threshold = 10.0) {
  const bool breakout =
      source == bte::strategy::ConditionSource::breakPeriodHigh ||
      source == bte::strategy::ConditionSource::breakPeriodLow;
  return bte::strategy::Condition{
      .source = source,
      .comparison = breakout ? bte::strategy::Comparison::greaterThan
                             : bte::strategy::Comparison::greaterThanOrEqual,
      .threshold = breakout ? 0.0 : threshold,
      .thresholdDomain = breakout ? bte::indicators::NumericDomain::scalar
                                  : bte::indicators::NumericDomain::percent,
      .lookbackPeriod = period,
  };
}

bte::strategy::PreviousDataDaySummary
previousDataDaySummary(const std::chrono::year_month_day date,
                       const double high, const double low,
                       const double close) {
  return bte::strategy::PreviousDataDaySummary{
      .utcDateStart = bte::core::Timestamp{std::chrono::sys_days{date}},
      .high = high,
      .low = low,
      .close = close,
  };
}

bte::core::Result<bte::indicators::StreamingIndicator>
failIndicatorConstruction(const bte::indicators::IndicatorDefinition &) {
  return bte::core::makeError(bte::core::ErrorCode::internal,
                              "simulated indicator allocation failure");
}

bte::core::Result<bte::indicators::StreamingIndicator>
throwDuringIndicatorConstruction(const bte::indicators::IndicatorDefinition &) {
  throw std::runtime_error{"simulated indicator factory exception"};
}

bte::core::Result<bte::indicators::StreamingIndicator>
throwUnknownDuringIndicatorConstruction(
    const bte::indicators::IndicatorDefinition &) {
  throw 7;
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
      .sell = {},
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

TEST(SelectableStrategyTest, legacySelectableHashRemainsV1) {
  auto plan = bte::strategy::SelectableStrategyPlan{
      .buy = {.logic = bte::strategy::ConditionLogic::all,
              .conditions = {priceChangeCondition(
                  bte::strategy::Comparison::greaterThan, 5.0)}},
      .sell = {.logic = bte::strategy::ConditionLogic::any,
               .conditions = {bte::strategy::Condition{
                   .source = bte::strategy::ConditionSource::indicator,
                   .comparison = bte::strategy::Comparison::lessThan,
                   .threshold = 20.0,
                   .thresholdDomain = bte::indicators::NumericDomain::percent,
                   .indicator = {.kind = bte::indicators::IndicatorKind::rsi,
                                 .period = 14}}}},
  };
  const auto original = bte::strategy::canonicalStrategyHash(plan);
  EXPECT_EQ(original,
            "d8722c8bb0625c68107e91460de923d06ef0847c10ae4e3294ec76c028b6aa85");
  EXPECT_EQ(original, bte::strategy::canonicalStrategyHash(plan));
  EXPECT_EQ(original.size(), 64U);
  EXPECT_NE(original, bte::strategy::canonicalStrategyHash(std::nullopt));

  auto changedThreshold = plan;
  changedThreshold.buy.conditions.front().threshold = 5.5;
  EXPECT_NE(original, bte::strategy::canonicalStrategyHash(changedThreshold));
  auto changedIndicator = plan;
  changedIndicator.sell.conditions.front().indicator.period = 15;
  EXPECT_NE(original, bte::strategy::canonicalStrategyHash(changedIndicator));
}

TEST(SelectableStrategyTest, historicalDataDayPlanUsesV3Hash) {
  using namespace bte::strategy;
  auto plan = SelectableStrategyPlan{
      .buy = {.conditions = {historicalDataDayCondition(
                  ConditionSource::dataDayGainPercent, 2)}},
      .sell = {},
  };
  const auto original = canonicalStrategyHash(plan);
  EXPECT_EQ(original,
            "cf6bd372ef43d4553e0467b7c046208a2e48fe9e686570a2e353534cb24a0a85");
  EXPECT_EQ(original.size(), 64U);

  auto changedLookback = plan;
  changedLookback.buy.conditions.front().lookbackPeriod = 3;
  EXPECT_NE(original, canonicalStrategyHash(changedLookback));
}

TEST(SelectableStrategyTest,
     dataDayCloseConditionUsesInclusivePositiveMagnitude) {
  using namespace std::chrono;
  const auto plan = bte::strategy::SelectableStrategyPlan{
      .buy = {.conditions = {previousDataDayCloseCondition(
                  bte::strategy::Comparison::lessThanOrEqual, -5.0)}},
      .sell = {.conditions = {previousDataDayCloseCondition(
                   bte::strategy::Comparison::greaterThanOrEqual, 5.0)}},
  };
  const auto previousClose = bte::strategy::PreviousDataDayClose{
      .timestamp =
          bte::core::Timestamp{sys_days{year{2024} / January / 1} + hours{22}},
      .close = 100.0,
  };
  auto created = bte::strategy::SelectableStrategy::create(plan, previousClose);
  ASSERT_TRUE(created.ok());
  auto strategy = std::move(created).value();
  const auto testDate = sys_days{year{2024} / January / 2};

  const auto exactDown = strategy->onBar(
      makeBarAt(bte::core::Timestamp{testDate + hours{1}}, 95.0));
  const auto belowDown = strategy->onBar(
      makeBarAt(bte::core::Timestamp{testDate + hours{2}}, 94.99));
  const auto aboveDown = strategy->onBar(
      makeBarAt(bte::core::Timestamp{testDate + hours{3}}, 95.01));
  const auto exactUp = strategy->onBar(
      makeBarAt(bte::core::Timestamp{testDate + hours{4}}, 105.0));
  const auto belowUp = strategy->onBar(
      makeBarAt(bte::core::Timestamp{testDate + hours{5}}, 104.99));
  const auto aboveUp = strategy->onBar(
      makeBarAt(bte::core::Timestamp{testDate + hours{6}}, 105.01));

  ASSERT_TRUE(exactDown.ok());
  ASSERT_TRUE(belowDown.ok());
  ASSERT_TRUE(aboveDown.ok());
  ASSERT_TRUE(exactUp.ok());
  ASSERT_TRUE(belowUp.ok());
  ASSERT_TRUE(aboveUp.ok());
  EXPECT_TRUE(exactDown.value().buy);
  EXPECT_TRUE(belowDown.value().buy);
  EXPECT_FALSE(aboveDown.value().buy);
  EXPECT_FALSE(exactDown.value().sell);
  EXPECT_TRUE(exactUp.value().sell);
  EXPECT_FALSE(belowUp.value().sell);
  EXPECT_TRUE(aboveUp.value().sell);
}

TEST(SelectableStrategyTest,
     historicalGainUsesTheCloseExactlyNAvailableDataDaysBeforeCurrentDay) {
  using namespace std::chrono;
  using namespace bte::strategy;
  auto condition = Condition{
      .source = ConditionSource::dataDayGainPercent,
      .comparison = Comparison::greaterThanOrEqual,
      .threshold = 10.0,
      .thresholdDomain = bte::indicators::NumericDomain::percent,
      .lookbackPeriod = 2,
  };
  const auto plan = SelectableStrategyPlan{
      .buy = {.conditions = {condition}},
      .sell = {},
  };
  const auto history = std::vector<PreviousDataDaySummary>{
      {.utcDateStart = bte::core::Timestamp{sys_days{year{2024} / January / 1}},
       .high = 105.0,
       .low = 95.0,
       .close = 100.0},
      {.utcDateStart = bte::core::Timestamp{sys_days{year{2024} / January / 2}},
       .high = 112.0,
       .low = 108.0,
       .close = 110.0},
  };
  auto created = SelectableStrategy::create(plan, std::nullopt, history);
  ASSERT_TRUE(created.ok()) << created.error().message;
  auto strategy = std::move(created).value();
  const auto currentDay = sys_days{year{2024} / January / 4};

  const auto exactGain = strategy->onBar(
      makeBarAt(bte::core::Timestamp{currentDay + hours{10}}, 110.0));
  const auto belowGain = strategy->onBar(
      makeBarAt(bte::core::Timestamp{currentDay + hours{11}}, 109.99));

  ASSERT_TRUE(exactGain.ok());
  ASSERT_TRUE(belowGain.ok());
  EXPECT_TRUE(exactGain.value().buy);
  EXPECT_FALSE(belowGain.value().buy);
}

TEST(SelectableStrategyTest,
     historicalGainUsesThirtyAvailableDatesAndTheFinalExtendedHoursClose) {
  using namespace std::chrono;
  using namespace bte::strategy;
  const auto currentDate = sys_days{year{2024} / April / 1};
  auto reverseHistory = std::vector<PreviousDataDaySummary>{};
  for (auto date = currentDate - days{1}; reverseHistory.size() < 30U;
       date -= days{1}) {
    const auto dayOfWeek = weekday{date}.iso_encoding();
    if (dayOfWeek > 5U) {
      continue;
    }
    reverseHistory.push_back(PreviousDataDaySummary{
        .utcDateStart = bte::core::Timestamp{date},
        .high = 120.0,
        .low = 80.0,
        .close = reverseHistory.size() == 29U ? 110.0 : 100.0,
    });
  }
  std::ranges::reverse(reverseHistory);

  const auto plan = SelectableStrategyPlan{
      .buy = {.conditions = {Condition{
                  .source = ConditionSource::dataDayGainPercent,
                  .comparison = Comparison::greaterThanOrEqual,
                  .threshold = 10.0,
                  .thresholdDomain = bte::indicators::NumericDomain::percent,
                  .lookbackPeriod = 30,
              }}},
      .sell = {},
  };
  auto created = SelectableStrategy::create(plan, std::nullopt, reverseHistory);
  ASSERT_TRUE(created.ok()) << created.error().message;
  auto strategy = std::move(created).value();

  const auto exactGain = strategy->onBar(
      makeBarAt(bte::core::Timestamp{currentDate + hours{9}}, 121.0));
  const auto belowGain = strategy->onBar(
      makeBarAt(bte::core::Timestamp{currentDate + hours{10}}, 120.999));

  ASSERT_TRUE(exactGain.ok());
  ASSERT_TRUE(belowGain.ok());
  EXPECT_TRUE(exactGain.value().buy);
  EXPECT_FALSE(belowGain.value().buy);
}

TEST(SelectableStrategyTest,
     historicalOneMonthPresetUsesExactlyTwentyOneAvailableDataDays) {
  using namespace std::chrono;
  using namespace bte::strategy;
  const auto currentDate = sys_days{year{2024} / April / 1};
  auto reverseHistory = std::vector<PreviousDataDaySummary>{};
  for (auto date = currentDate - days{1}; reverseHistory.size() < 21U;
       date -= days{1}) {
    if (weekday{date}.iso_encoding() > 5U) {
      continue;
    }
    const auto isOldestDate = reverseHistory.size() == 20U;
    reverseHistory.push_back(PreviousDataDaySummary{
        .utcDateStart = bte::core::Timestamp{date},
        .high = isOldestDate ? 120.0 : 115.0,
        .low = isOldestDate ? 80.0 : 85.0,
        .close = isOldestDate ? 100.0 : 110.0,
    });
  }
  std::ranges::reverse(reverseHistory);

  const auto runSignal = [&reverseHistory, &currentDate](
                             const ConditionSource source,
                             const std::int32_t period, const double close) {
    const auto plan = SelectableStrategyPlan{
        .buy = {.conditions = {historicalDataDayCondition(source, period)}},
        .sell = {},
    };
    auto created =
        SelectableStrategy::create(plan, std::nullopt, reverseHistory);
    if (!created.ok()) {
      return false;
    }
    auto strategy = std::move(created).value();
    const auto result = strategy->onBar(
        makeBarAt(bte::core::Timestamp{currentDate + hours{9}}, close));
    return result.ok() && result.value().buy;
  };

  EXPECT_TRUE(runSignal(ConditionSource::dataDayGainPercent, 21, 110.0));
  EXPECT_FALSE(runSignal(ConditionSource::dataDayGainPercent, 20, 110.0));
  EXPECT_TRUE(runSignal(ConditionSource::periodHighDrawdownPercent, 21, 108.0));
  EXPECT_FALSE(
      runSignal(ConditionSource::periodHighDrawdownPercent, 20, 108.0));
  EXPECT_TRUE(runSignal(ConditionSource::periodLowReboundPercent, 21, 88.0));
  EXPECT_FALSE(runSignal(ConditionSource::periodLowReboundPercent, 20, 88.0));
}

TEST(SelectableStrategyTest,
     historicalExtremaPercentagesUsePriorDataDayHighAndLow) {
  using namespace std::chrono;
  using namespace bte::strategy;
  const auto history = std::vector<PreviousDataDaySummary>{
      previousDataDaySummary(year{2024} / January / 1, 120.0, 100.0, 110.0),
      previousDataDaySummary(year{2024} / January / 2, 115.0, 80.0, 90.0),
  };
  const auto currentDay = sys_days{year{2024} / January / 4};
  const auto runSignal = [&history, &currentDay](const ConditionSource source,
                                                 const double close) {
    const auto plan = SelectableStrategyPlan{
        .buy = {.conditions = {historicalDataDayCondition(source, 2)}},
        .sell = {},
    };
    auto created = SelectableStrategy::create(plan, std::nullopt, history);
    EXPECT_TRUE(created.ok()) << created.error().message;
    if (!created.ok()) {
      return false;
    }
    auto strategy = std::move(created).value();
    const auto result = strategy->onBar(
        makeBarAt(bte::core::Timestamp{currentDay + hours{10}}, close));
    EXPECT_TRUE(result.ok()) << result.error().message;
    return result.ok() && result.value().buy;
  };

  EXPECT_TRUE(runSignal(ConditionSource::periodHighDrawdownPercent, 108.0));
  EXPECT_FALSE(runSignal(ConditionSource::periodHighDrawdownPercent, 108.01));
  EXPECT_TRUE(runSignal(ConditionSource::periodLowReboundPercent, 88.0));
  EXPECT_FALSE(runSignal(ConditionSource::periodLowReboundPercent, 87.99));
}

TEST(SelectableStrategyTest,
     historicalBreakoutsAreStrictPersistentAndExcludeCurrentDateExtremes) {
  using namespace std::chrono;
  using namespace bte::strategy;
  const auto history = std::vector<PreviousDataDaySummary>{
      previousDataDaySummary(year{2024} / January / 1, 100.0, 80.0, 90.0),
      previousDataDaySummary(year{2024} / January / 2, 95.0, 85.0, 90.0),
  };
  const auto firstCurrentDay = sys_days{year{2024} / January / 4};
  const auto nextDataDay = sys_days{year{2024} / January / 5};
  const auto createStrategy = [&history](const ConditionSource source) {
    const auto plan = SelectableStrategyPlan{
        .buy = {.conditions = {historicalDataDayCondition(source, 2)}},
        .sell = {},
    };
    return SelectableStrategy::create(plan, std::nullopt, history);
  };
  const auto makeExtremeBar = [](const bte::core::Timestamp timestamp,
                                 const double high, const double low,
                                 const double close) {
    return bte::core::Bar{.ts = timestamp,
                          .open = close,
                          .high = high,
                          .low = low,
                          .close = close,
                          .volume = 100.0};
  };

  auto highStrategyResult = createStrategy(ConditionSource::breakPeriodHigh);
  ASSERT_TRUE(highStrategyResult.ok()) << highStrategyResult.error().message;
  auto highStrategy = std::move(highStrategyResult).value();
  const auto highEqualsPrior = highStrategy->onBar(makeExtremeBar(
      bte::core::Timestamp{firstCurrentDay + hours{9}}, 150.0, 99.0, 100.0));
  const auto highBreaksPrior = highStrategy->onBar(
      makeBarAt(bte::core::Timestamp{firstCurrentDay + hours{10}}, 101.0));
  const auto highBreakPersists = highStrategy->onBar(
      makeBarAt(bte::core::Timestamp{firstCurrentDay + hours{11}}, 101.0));
  const auto highEqualsCurrentDateExtreme = highStrategy->onBar(
      makeBarAt(bte::core::Timestamp{nextDataDay + hours{9}}, 150.0));
  const auto highExceedsCompletedExtreme = highStrategy->onBar(
      makeBarAt(bte::core::Timestamp{nextDataDay + hours{10}}, 150.01));

  ASSERT_TRUE(highEqualsPrior.ok());
  ASSERT_TRUE(highBreaksPrior.ok());
  ASSERT_TRUE(highBreakPersists.ok());
  ASSERT_TRUE(highEqualsCurrentDateExtreme.ok());
  ASSERT_TRUE(highExceedsCompletedExtreme.ok());
  EXPECT_FALSE(highEqualsPrior.value().buy);
  EXPECT_TRUE(highBreaksPrior.value().buy);
  EXPECT_TRUE(highBreakPersists.value().buy);
  EXPECT_FALSE(highEqualsCurrentDateExtreme.value().buy);
  EXPECT_TRUE(highExceedsCompletedExtreme.value().buy);

  auto lowStrategyResult = createStrategy(ConditionSource::breakPeriodLow);
  ASSERT_TRUE(lowStrategyResult.ok()) << lowStrategyResult.error().message;
  auto lowStrategy = std::move(lowStrategyResult).value();
  const auto lowEqualsPrior = lowStrategy->onBar(makeExtremeBar(
      bte::core::Timestamp{firstCurrentDay + hours{9}}, 81.0, 1.0, 80.0));
  const auto lowBreaksPrior = lowStrategy->onBar(
      makeBarAt(bte::core::Timestamp{firstCurrentDay + hours{10}}, 79.0));
  const auto lowBreakPersists = lowStrategy->onBar(
      makeBarAt(bte::core::Timestamp{firstCurrentDay + hours{11}}, 79.0));
  const auto lowEqualsCurrentDateExtreme = lowStrategy->onBar(
      makeBarAt(bte::core::Timestamp{nextDataDay + hours{9}}, 1.0));
  const auto lowExceedsCompletedExtreme = lowStrategy->onBar(
      makeBarAt(bte::core::Timestamp{nextDataDay + hours{10}}, 0.99));

  ASSERT_TRUE(lowEqualsPrior.ok());
  ASSERT_TRUE(lowBreaksPrior.ok());
  ASSERT_TRUE(lowBreakPersists.ok());
  ASSERT_TRUE(lowEqualsCurrentDateExtreme.ok());
  ASSERT_TRUE(lowExceedsCompletedExtreme.ok());
  EXPECT_FALSE(lowEqualsPrior.value().buy);
  EXPECT_TRUE(lowBreaksPrior.value().buy);
  EXPECT_TRUE(lowBreakPersists.value().buy);
  EXPECT_FALSE(lowEqualsCurrentDateExtreme.value().buy);
  EXPECT_TRUE(lowExceedsCompletedExtreme.value().buy);
}

TEST(SelectableStrategyTest,
     historicalConditionsDoNotMatchWithoutACompleteWindowAndValidatePeriods) {
  using namespace std::chrono;
  using namespace bte::strategy;
  const auto history = std::vector<PreviousDataDaySummary>{
      previousDataDaySummary(year{2024} / January / 1, 120.0, 80.0, 100.0),
  };
  const auto currentDay = sys_days{year{2024} / January / 2};
  const auto runWithPeriod = [&history](const std::int32_t period) {
    const auto plan = SelectableStrategyPlan{
        .buy = {.conditions = {historicalDataDayCondition(
                    ConditionSource::periodHighDrawdownPercent, period)}},
        .sell = {},
    };
    return SelectableStrategy::create(plan, std::nullopt, history);
  };

  auto insufficient = runWithPeriod(2);
  ASSERT_TRUE(insufficient.ok()) << insufficient.error().message;
  auto insufficientStrategy = std::move(insufficient).value();
  const auto noWindow = insufficientStrategy->onBar(
      makeBarAt(bte::core::Timestamp{currentDay + hours{9}}, 108.0));
  ASSERT_TRUE(noWindow.ok());
  EXPECT_FALSE(noWindow.value().buy);

  EXPECT_TRUE(runWithPeriod(1).ok());
  EXPECT_TRUE(runWithPeriod(bte::indicators::maximumIndicatorPeriod).ok());
  EXPECT_FALSE(runWithPeriod(0).ok());
  EXPECT_FALSE(runWithPeriod(bte::indicators::maximumIndicatorPeriod + 1).ok());

  const auto invalidHistory = std::vector<PreviousDataDaySummary>{
      previousDataDaySummary(year{2024} / January / 1, 120.0, 80.0, 0.0),
  };
  const auto invalidHistoryPlan = SelectableStrategyPlan{
      .buy = {.conditions = {historicalDataDayCondition(
                  ConditionSource::periodHighDrawdownPercent, 1)}},
      .sell = {},
  };
  auto invalidSeedStrategy = SelectableStrategy::create(
      invalidHistoryPlan, std::nullopt, invalidHistory);
  ASSERT_TRUE(invalidSeedStrategy.ok()) << invalidSeedStrategy.error().message;
  auto strategy = std::move(invalidSeedStrategy).value();
  const auto invalidSeedSignal = strategy->onBar(
      makeBarAt(bte::core::Timestamp{currentDay + hours{9}}, 108.0));
  ASSERT_TRUE(invalidSeedSignal.ok());
  EXPECT_FALSE(invalidSeedSignal.value().buy);
}

TEST(SelectableStrategyTest,
     historicalPredicatesRejectUnsupportedConditionParameters) {
  using namespace bte::strategy;
  const auto rejects = [](const Condition condition) {
    return !SelectableStrategy::create(
                {.buy = {.conditions = {condition}}, .sell = {}})
                .ok();
  };

  auto invalidBreakout =
      historicalDataDayCondition(ConditionSource::breakPeriodHigh, 1);
  invalidBreakout.comparison = Comparison::lessThan;
  EXPECT_TRUE(rejects(invalidBreakout));

  auto invalidPercentage =
      historicalDataDayCondition(ConditionSource::dataDayGainPercent, 1, 0.0);
  EXPECT_TRUE(rejects(invalidPercentage));

  auto barFieldWithPeriod = Condition{
      .source = ConditionSource::barField,
      .comparison = Comparison::greaterThan,
      .threshold = 1.0,
      .lookbackPeriod = 1,
  };
  EXPECT_TRUE(rejects(barFieldWithPeriod));

  auto mismatchedBarFieldDomain = Condition{
      .source = ConditionSource::barField,
      .comparison = Comparison::greaterThan,
      .threshold = 1.0,
      .thresholdDomain = bte::indicators::NumericDomain::volume,
  };
  EXPECT_TRUE(rejects(mismatchedBarFieldDomain));

  auto indicatorWithPeriod = Condition{
      .source = ConditionSource::indicator,
      .comparison = Comparison::greaterThan,
      .threshold = 1.0,
      .thresholdDomain = bte::indicators::NumericDomain::percent,
      .indicator = {.kind = bte::indicators::IndicatorKind::rsi, .period = 2},
      .lookbackPeriod = 1,
  };
  EXPECT_TRUE(rejects(indicatorWithPeriod));
}

TEST(SelectableStrategyTest, historicalDataDaySeedEnforcesItsSupportedLimit) {
  using namespace std::chrono;
  using namespace bte::strategy;
  auto history = std::vector<PreviousDataDaySummary>{};
  history.reserve(
      static_cast<std::size_t>(bte::indicators::maximumIndicatorPeriod) + 1U);
  const auto firstDate = sys_days{year{2020} / January / 1};
  for (std::size_t index = 0;
       index <=
       static_cast<std::size_t>(bte::indicators::maximumIndicatorPeriod);
       ++index) {
    history.push_back(PreviousDataDaySummary{
        .utcDateStart =
            bte::core::Timestamp{firstDate +
                                 days{static_cast<days::rep>(index)}},
        .high = 3.0,
        .low = 1.0,
        .close = 2.0,
    });
  }
  const auto plan = SelectableStrategyPlan{
      .buy = {.conditions = {historicalDataDayCondition(
                  ConditionSource::dataDayGainPercent, 1)}},
      .sell = {},
  };

  const auto result = SelectableStrategy::create(plan, std::nullopt, history);

  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().code, bte::core::ErrorCode::strategyCompileFailed);
  EXPECT_NE(result.error().message.find("seed exceeds its supported limit"),
            std::string::npos);
}

TEST(SelectableStrategyTest,
     futureHistoricalSeedsAreIgnoredAndMixedConditionsInitialize) {
  using namespace std::chrono;
  using namespace bte::strategy;
  const auto history = std::vector<PreviousDataDaySummary>{
      previousDataDaySummary(year{2024} / January / 1, 120.0, 80.0, 100.0),
      previousDataDaySummary(year{2024} / January / 2, 130.0, 70.0, 110.0),
  };
  auto plan = SelectableStrategyPlan{
      .buy = {.conditions =
                  {
                      historicalDataDayCondition(
                          ConditionSource::dataDayGainPercent, 2),
                      Condition{.source = ConditionSource::barField,
                                .comparison = Comparison::greaterThan,
                                .threshold = 1.0},
                  }},
      .sell = {.conditions = {Condition{
                   .source = ConditionSource::barField,
                   .comparison = Comparison::greaterThan,
                   .threshold = 1.0,
               }}},
  };
  auto created = SelectableStrategy::create(plan, std::nullopt, history);
  ASSERT_TRUE(created.ok()) << created.error().message;
  auto strategy = std::move(created).value();
  const auto currentDay = sys_days{year{2024} / January / 2};

  const auto signal = strategy->onBar(
      makeBarAt(bte::core::Timestamp{currentDay + hours{9}}, 120.0));

  ASSERT_TRUE(signal.ok()) << signal.error().message;
  EXPECT_FALSE(signal.value().buy);
}

TEST(SelectableStrategyTest,
     outOfOrderDataDayBarCannotEvaluateHistoricalPredicates) {
  using namespace std::chrono;
  using namespace bte::strategy;
  const auto history = std::vector<PreviousDataDaySummary>{
      previousDataDaySummary(year{2024} / January / 1, 100.0, 80.0, 90.0),
  };
  const auto plan = SelectableStrategyPlan{
      .buy = {.conditions = {historicalDataDayCondition(
                  ConditionSource::dataDayGainPercent, 1, 0.5)}},
      .sell = {},
  };
  auto created = SelectableStrategy::create(plan, std::nullopt, history);
  ASSERT_TRUE(created.ok()) << created.error().message;
  auto strategy = std::move(created).value();
  const auto jan2 = sys_days{year{2024} / January / 2};
  const auto jan3 = sys_days{year{2024} / January / 3};

  const auto currentDay =
      strategy->onBar(makeBarAt(bte::core::Timestamp{jan2}, 110.0));
  const auto laterDay =
      strategy->onBar(makeBarAt(bte::core::Timestamp{jan3}, 111.0));
  const auto outOfOrder =
      strategy->onBar(makeBarAt(bte::core::Timestamp{jan2 + hours{1}}, 200.0));

  ASSERT_TRUE(currentDay.ok());
  ASSERT_TRUE(laterDay.ok());
  ASSERT_TRUE(outOfOrder.ok());
  EXPECT_TRUE(currentDay.value().buy);
  EXPECT_TRUE(laterDay.value().buy);
  EXPECT_FALSE(outOfOrder.value().buy);
}

TEST(SelectableStrategyTest,
     dataDayCloseConditionKeepsReferenceAcrossIntradayBars) {
  using namespace std::chrono;
  const auto plan = bte::strategy::SelectableStrategyPlan{
      .buy = {.conditions = {previousDataDayCloseCondition(
                  bte::strategy::Comparison::lessThanOrEqual, -5.0)}},
      .sell = {},
  };
  const auto previousClose = bte::strategy::PreviousDataDayClose{
      .timestamp =
          bte::core::Timestamp{sys_days{year{2024} / January / 1} + hours{22}},
      .close = 110.0,
  };
  auto created = bte::strategy::SelectableStrategy::create(plan, previousClose);
  ASSERT_TRUE(created.ok());
  auto strategy = std::move(created).value();
  const auto date = sys_days{year{2024} / January / 2};

  const auto down =
      strategy->onBar(makeBarAt(bte::core::Timestamp{date + hours{9}}, 104.5));
  const auto intradayRise =
      strategy->onBar(makeBarAt(bte::core::Timestamp{date + hours{15}}, 121.0));
  const auto downAgain =
      strategy->onBar(makeBarAt(bte::core::Timestamp{date + hours{23}}, 104.5));

  ASSERT_TRUE(down.ok());
  ASSERT_TRUE(intradayRise.ok());
  ASSERT_TRUE(downAgain.ok());
  EXPECT_TRUE(down.value().buy);
  EXPECT_FALSE(intradayRise.value().buy);
  EXPECT_TRUE(downAgain.value().buy);
}

TEST(SelectableStrategyTest,
     dataDayCloseConditionRollsAtUtcDateBoundaryAfterFridayExtendedClose) {
  using namespace std::chrono;
  const auto plan = bte::strategy::SelectableStrategyPlan{
      .buy = {.conditions = {previousDataDayCloseCondition(
                  bte::strategy::Comparison::lessThanOrEqual, -5.0)}},
      .sell = {},
  };
  const auto previousClose = bte::strategy::PreviousDataDayClose{
      .timestamp =
          bte::core::Timestamp{sys_days{year{2024} / March / 7} + hours{22}},
      .close = 100.0,
  };
  auto created = bte::strategy::SelectableStrategy::create(plan, previousClose);
  ASSERT_TRUE(created.ok());
  auto strategy = std::move(created).value();
  const auto friday = sys_days{year{2024} / March / 8};
  const auto monday = sys_days{year{2024} / March / 11};

  const auto regularClose = strategy->onBar(
      makeBarAt(bte::core::Timestamp{friday + hours{16}}, 100.0));
  const auto extendedClose = strategy->onBar(
      makeBarAt(bte::core::Timestamp{friday + hours{22}}, 110.0));
  const auto mondayOpen =
      strategy->onBar(makeBarAt(bte::core::Timestamp{monday}, 104.5));

  ASSERT_TRUE(regularClose.ok());
  ASSERT_TRUE(extendedClose.ok());
  ASSERT_TRUE(mondayOpen.ok());
  EXPECT_FALSE(regularClose.value().buy);
  EXPECT_FALSE(extendedClose.value().buy);
  EXPECT_TRUE(mondayOpen.value().buy);
}

TEST(SelectableStrategyTest,
     dataDayCloseConditionUsesUtcDatesAcrossYearLeapAndExchangeDst) {
  using namespace std::chrono;
  const auto plan = bte::strategy::SelectableStrategyPlan{
      .buy = {.conditions = {previousDataDayCloseCondition(
                  bte::strategy::Comparison::lessThanOrEqual, -5.0)}},
      .sell = {.conditions = {previousDataDayCloseCondition(
                   bte::strategy::Comparison::greaterThanOrEqual, 5.0)}},
  };
  const auto previousClose = bte::strategy::PreviousDataDayClose{
      .timestamp = bte::core::Timestamp{sys_days{year{2023} / December / 30} +
                                        hours{22}},
      .close = 100.0,
  };
  auto created = bte::strategy::SelectableStrategy::create(plan, previousClose);
  ASSERT_TRUE(created.ok());
  auto strategy = std::move(created).value();
  const auto dec31 = sys_days{year{2023} / December / 31};
  const auto jan1 = sys_days{year{2024} / January / 1};
  const auto feb28 = sys_days{year{2024} / February / 28};
  const auto feb29 = sys_days{year{2024} / February / 29};
  const auto mar10 = sys_days{year{2024} / March / 10};

  const auto lastYear = strategy->onBar(
      makeBarAt(bte::core::Timestamp{dec31 + hours{23}}, 100.0));
  const auto newYearDown =
      strategy->onBar(makeBarAt(bte::core::Timestamp{jan1}, 95.0));
  const auto newYearUp =
      strategy->onBar(makeBarAt(bte::core::Timestamp{jan1 + hours{23}}, 110.0));
  const auto afterLongGap = strategy->onBar(
      makeBarAt(bte::core::Timestamp{feb28 + hours{23}}, 104.5));
  const auto leapDayDown =
      strategy->onBar(makeBarAt(bte::core::Timestamp{feb29}, 99.0));
  const auto leapDayIntraday = strategy->onBar(
      makeBarAt(bte::core::Timestamp{feb29 + hours{23}}, 104.5));
  const auto afterDstDate =
      strategy->onBar(makeBarAt(bte::core::Timestamp{mar10 + hours{8}}, 99.0));

  ASSERT_TRUE(lastYear.ok());
  ASSERT_TRUE(newYearDown.ok());
  ASSERT_TRUE(newYearUp.ok());
  ASSERT_TRUE(afterLongGap.ok());
  ASSERT_TRUE(leapDayDown.ok());
  ASSERT_TRUE(leapDayIntraday.ok());
  ASSERT_TRUE(afterDstDate.ok());
  EXPECT_FALSE(lastYear.value().buy);
  EXPECT_TRUE(newYearDown.value().buy);
  EXPECT_TRUE(newYearUp.value().sell);
  EXPECT_TRUE(afterLongGap.value().buy);
  EXPECT_TRUE(leapDayDown.value().buy);
  EXPECT_FALSE(leapDayIntraday.value().sell);
  EXPECT_TRUE(afterDstDate.value().buy);
}

TEST(SelectableStrategyTest,
     dataDayCloseConditionIgnoresAbsentInvalidAndFutureHistory) {
  using namespace std::chrono;
  const auto plan = bte::strategy::SelectableStrategyPlan{
      .buy = {.conditions = {previousDataDayCloseCondition(
                  bte::strategy::Comparison::lessThanOrEqual, -5.0)}},
      .sell = {},
  };
  const auto priorDate = sys_days{year{2024} / January / 1};
  const auto runWithoutSeed =
      [&](const std::optional<bte::strategy::PreviousDataDayClose> seed) {
        auto created = bte::strategy::SelectableStrategy::create(plan, seed);
        ASSERT_TRUE(created.ok());
        auto strategy = std::move(created).value();
        const auto signal = strategy->onBar(makeBar(2, 94.99));
        ASSERT_TRUE(signal.ok());
        EXPECT_FALSE(signal.value().buy);
      };

  runWithoutSeed(std::nullopt);
  runWithoutSeed(bte::strategy::PreviousDataDayClose{
      .timestamp = bte::core::Timestamp{priorDate}, .close = 0.0});
  runWithoutSeed(bte::strategy::PreviousDataDayClose{
      .timestamp = bte::core::Timestamp{priorDate}, .close = -100.0});
  runWithoutSeed(bte::strategy::PreviousDataDayClose{
      .timestamp = bte::core::Timestamp{priorDate},
      .close = std::numeric_limits<double>::quiet_NaN()});
  runWithoutSeed(bte::strategy::PreviousDataDayClose{
      .timestamp = bte::core::Timestamp{priorDate},
      .close = std::numeric_limits<double>::infinity()});
  runWithoutSeed(bte::strategy::PreviousDataDayClose{
      .timestamp = bte::core::Timestamp{sys_days{year{2024} / January / 2}},
      .close = 100.0});
  runWithoutSeed(bte::strategy::PreviousDataDayClose{
      .timestamp = bte::core::Timestamp{sys_days{year{2024} / January / 3}},
      .close = 100.0});
}

TEST(SelectableStrategyTest,
     dataDayCloseConditionIgnoresNonFiniteCalculatedChange) {
  using namespace std::chrono;
  const auto plan = bte::strategy::SelectableStrategyPlan{
      .buy = {.conditions = {previousDataDayCloseCondition(
                  bte::strategy::Comparison::lessThanOrEqual, -5.0)}},
      .sell = {},
  };
  auto created = bte::strategy::SelectableStrategy::create(
      plan,
      bte::strategy::PreviousDataDayClose{
          .timestamp = bte::core::Timestamp{sys_days{year{2024} / January / 1}},
          .close = std::numeric_limits<double>::min(),
      });
  ASSERT_TRUE(created.ok());
  auto strategy = std::move(created).value();
  const auto maximum = std::numeric_limits<double>::max();
  const auto bar = bte::core::Bar{
      .ts = bte::core::Timestamp{sys_days{year{2024} / January / 2}},
      .open = maximum,
      .high = maximum,
      .low = maximum,
      .close = maximum,
      .volume = 100.0,
  };

  const auto signal = strategy->onBar(bar);

  ASSERT_TRUE(signal.ok());
  EXPECT_FALSE(signal.value().buy);
}

TEST(SelectableStrategyTest,
     outOfOrderDataDayBarDoesNotChangeChronologicalCloseState) {
  using namespace std::chrono;
  const auto plan = bte::strategy::SelectableStrategyPlan{
      .buy = {.conditions = {previousDataDayCloseCondition(
                  bte::strategy::Comparison::lessThanOrEqual, -5.0)}},
      .sell = {},
  };
  auto created = bte::strategy::SelectableStrategy::create(
      plan,
      bte::strategy::PreviousDataDayClose{
          .timestamp = bte::core::Timestamp{sys_days{year{2024} / January / 1}},
          .close = 100.0,
      });
  ASSERT_TRUE(created.ok());
  auto strategy = std::move(created).value();
  const auto jan2 = sys_days{year{2024} / January / 2};
  const auto jan3 = sys_days{year{2024} / January / 3};
  const auto jan4 = sys_days{year{2024} / January / 4};

  const auto firstDay =
      strategy->onBar(makeBarAt(bte::core::Timestamp{jan2}, 100.0));
  const auto secondDay =
      strategy->onBar(makeBarAt(bte::core::Timestamp{jan3}, 90.0));
  const auto outOfOrder =
      strategy->onBar(makeBarAt(bte::core::Timestamp{jan2 + hours{1}}, 70.0));
  const auto nextDay =
      strategy->onBar(makeBarAt(bte::core::Timestamp{jan4}, 80.0));

  ASSERT_TRUE(firstDay.ok());
  ASSERT_TRUE(secondDay.ok());
  ASSERT_TRUE(outOfOrder.ok());
  ASSERT_TRUE(nextDay.ok());
  EXPECT_FALSE(firstDay.value().buy);
  EXPECT_TRUE(secondDay.value().buy);
  EXPECT_FALSE(outOfOrder.value().buy);
  EXPECT_TRUE(nextDay.value().buy);
}

TEST(SelectableStrategyTest,
     dataDayCloseConditionRejectsInvalidDirectionAndDomain) {
  using namespace bte::strategy;
  const auto makePlan = [](const Condition condition) {
    return SelectableStrategyPlan{.buy = {.conditions = {condition}},
                                  .sell = {}};
  };
  const auto wrongDirection = SelectableStrategy::create(makePlan(
      previousDataDayCloseCondition(Comparison::lessThanOrEqual, 5.0)));
  const auto wrongComparison = SelectableStrategy::create(
      makePlan(previousDataDayCloseCondition(Comparison::lessThan, -5.0)));
  const auto zeroThreshold = SelectableStrategy::create(makePlan(
      previousDataDayCloseCondition(Comparison::greaterThanOrEqual, 0.0)));
  const auto oppositeSignedThreshold = SelectableStrategy::create(makePlan(
      previousDataDayCloseCondition(Comparison::greaterThanOrEqual, -5.0)));
  auto wrongDomainCondition =
      previousDataDayCloseCondition(Comparison::lessThanOrEqual, -5.0);
  wrongDomainCondition.thresholdDomain = bte::indicators::NumericDomain::price;
  const auto wrongDomain =
      SelectableStrategy::create(makePlan(wrongDomainCondition));

  EXPECT_FALSE(wrongDirection.ok());
  EXPECT_FALSE(wrongComparison.ok());
  EXPECT_FALSE(zeroThreshold.ok());
  EXPECT_FALSE(oppositeSignedThreshold.ok());
  EXPECT_FALSE(wrongDomain.ok());
}

TEST(SelectableStrategyTest, dataDayClosePlanUsesV2Hash) {
  using namespace bte::strategy;
  auto plan = SelectableStrategyPlan{
      .buy = {.conditions = {previousDataDayCloseCondition(
                  Comparison::lessThanOrEqual, -5.0)}},
      .sell = {},
  };
  const auto original = canonicalStrategyHash(plan);
  EXPECT_EQ(original,
            "87d4d78867b5cc107f74913b4c917f695a70fc43562b9715e1eb13c94dbeabbb");

  auto changedSource = plan;
  changedSource.buy.conditions.front().source =
      ConditionSource::closeChangePercent;
  EXPECT_NE(original, canonicalStrategyHash(changedSource));
  auto changedComparison = plan;
  changedComparison.buy.conditions.front().comparison =
      Comparison::greaterThanOrEqual;
  EXPECT_NE(original, canonicalStrategyHash(changedComparison));
  auto changedDirection = plan;
  changedDirection.buy.conditions.front().threshold = 5.0;
  EXPECT_NE(original, canonicalStrategyHash(changedDirection));
  auto changedThreshold = plan;
  changedThreshold.buy.conditions.front().threshold = -5.5;
  EXPECT_NE(original, canonicalStrategyHash(changedThreshold));
  auto changedLogic = plan;
  changedLogic.buy.logic = ConditionLogic::any;
  EXPECT_NE(original, canonicalStrategyHash(changedLogic));

  auto changedOrder = plan;
  changedOrder.buy.conditions.push_back(Condition{
      .source = ConditionSource::barField,
      .comparison = Comparison::greaterThan,
      .threshold = 10.0,
  });
  const auto changedOrderHash = canonicalStrategyHash(changedOrder);
  std::ranges::reverse(changedOrder.buy.conditions);
  EXPECT_NE(changedOrderHash, canonicalStrategyHash(changedOrder));
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
      .sell = {},
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
      .sell = {},
  });

  EXPECT_FALSE(noBuy.ok());
  EXPECT_FALSE(notFinite.ok());
  EXPECT_FALSE(invalidIndicator.ok());
  EXPECT_EQ(noBuy.error().code, bte::core::ErrorCode::strategyCompileFailed);
  EXPECT_EQ(notFinite.error().code,
            bte::core::ErrorCode::strategyCompileFailed);
  EXPECT_EQ(invalidIndicator.error().code,
            bte::core::ErrorCode::strategyCompileFailed);
  EXPECT_NE(invalidIndicator.error().message.find("buy condition group"),
            std::string::npos);
  EXPECT_NE(invalidIndicator.error().message.find(
                "indicator period must be between 1 and 10000"),
            std::string::npos);
  ASSERT_EQ(invalidIndicator.error().causes.size(), 1U);
  EXPECT_EQ(invalidIndicator.error().causes.front().code,
            bte::core::ErrorCode::strategyCompileFailed);
  EXPECT_NE(invalidIndicator.error().causes.front().message.find(
                "condition has an invalid indicator"),
            std::string::npos);
  ASSERT_EQ(invalidIndicator.error().causes.front().causes.size(), 1U);
  EXPECT_EQ(invalidIndicator.error().causes.front().causes.front().code,
            bte::core::ErrorCode::invalidArgument);
  EXPECT_NE(invalidIndicator.error().causes.front().causes.front().message.find(
                "indicator period must be between 1 and 10000"),
            std::string::npos);
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
      .sell = {},
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
      .sell = {},
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
      .sell = {},
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
        .sell = {},
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
      .sell = {},
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
      .sell = {},
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
      .sell = {},
  });
  const auto invalidSource = bte::strategy::SelectableStrategy::create({
      .buy = {.conditions = {bte::strategy::Condition{
                  .source = static_cast<bte::strategy::ConditionSource>(99),
              }}},
      .sell = {},
  });
  const auto invalidComparison = bte::strategy::SelectableStrategy::create({
      .buy = {.conditions = {bte::strategy::Condition{
                  .comparison = static_cast<bte::strategy::Comparison>(99),
              }}},
      .sell = {},
  });
  const auto invalidBarField = bte::strategy::SelectableStrategy::create({
      .buy = {.conditions = {bte::strategy::Condition{
                  .barField = static_cast<bte::indicators::BarField>(99),
              }}},
      .sell = {},
  });
  const auto invalidDomain = bte::strategy::SelectableStrategy::create({
      .buy = {.conditions = {bte::strategy::Condition{
                  .thresholdDomain =
                      static_cast<bte::indicators::NumericDomain>(99),
              }}},
      .sell = {},
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
  EXPECT_NE(invalidSell.error().message.find("sell condition group"),
            std::string::npos);
  EXPECT_NE(invalidSell.error().message.find(
                "indicator period must be between 1 and 10000"),
            std::string::npos);
  ASSERT_EQ(invalidSell.error().causes.size(), 1U);
  EXPECT_EQ(invalidSell.error().causes.front().code,
            bte::core::ErrorCode::strategyCompileFailed);
  EXPECT_NE(invalidSell.error().causes.front().message.find(
                "condition has an invalid indicator"),
            std::string::npos);
}

TEST(SelectableStrategyTest,
     indicatorPeriodAcceptsMaximumAndRejectsNextValueDuringConstruction) {
  const auto maximum = bte::strategy::SelectableStrategy::create({
      .buy = {.conditions = {bte::strategy::Condition{
                  .source = bte::strategy::ConditionSource::indicator,
                  .indicator =
                      {
                          .kind = bte::indicators::IndicatorKind::sma,
                          .period = bte::indicators::maximumIndicatorPeriod,
                      },
              }}},
      .sell = {},
  });
  const auto tooLarge = bte::strategy::SelectableStrategy::create({
      .buy = {.conditions = {priceChangeCondition(
                  bte::strategy::Comparison::greaterThan, 1.0)}},
      .sell = {.conditions = {bte::strategy::Condition{
                   .source = bte::strategy::ConditionSource::indicator,
                   .indicator =
                       {
                           .kind = bte::indicators::IndicatorKind::sma,
                           .period =
                               bte::indicators::maximumIndicatorPeriod + 1,
                       },
               }}},
  });

  EXPECT_TRUE(maximum.ok());
  EXPECT_FALSE(tooLarge.ok());
  EXPECT_EQ(tooLarge.error().code, bte::core::ErrorCode::strategyCompileFailed);
  EXPECT_NE(tooLarge.error().message.find("sell condition group"),
            std::string::npos);
}

TEST(SelectableStrategyTest,
     indicatorConstructionFailurePreservesInternalErrorAndCauseChain) {
  const auto failed =
      bte::strategy::detail::SelectableStrategyTestAccess::create(
          {
              .buy = {.conditions = {bte::strategy::Condition{
                          .source = bte::strategy::ConditionSource::indicator,
                          .indicator =
                              {
                                  .kind = bte::indicators::IndicatorKind::sma,
                                  .period = 2,
                              },
                      }}},
              .sell = {},
          },
          &failIndicatorConstruction);

  ASSERT_FALSE(failed.ok());
  EXPECT_EQ(failed.error().code, bte::core::ErrorCode::internal);
  EXPECT_NE(failed.error().message.find("buy condition group"),
            std::string::npos);
  ASSERT_EQ(failed.error().causes.size(), 1U);
  EXPECT_NE(failed.error().causes.front().message.find(
                "could not construct condition indicator"),
            std::string::npos);
  ASSERT_EQ(failed.error().causes.front().causes.size(), 1U);
  EXPECT_EQ(failed.error().causes.front().causes.front().code,
            bte::core::ErrorCode::internal);
  EXPECT_EQ(failed.error().causes.front().causes.front().message,
            "simulated indicator allocation failure");
}

TEST(SelectableStrategyTest,
     indicatorFactoryExceptionIsContainedAtTheStrategyBoundary) {
  const auto failed =
      bte::strategy::detail::SelectableStrategyTestAccess::create(
          {
              .buy = {.conditions = {bte::strategy::Condition{
                          .source = bte::strategy::ConditionSource::indicator,
                          .indicator =
                              {
                                  .kind = bte::indicators::IndicatorKind::sma,
                                  .period = 2,
                              },
                      }}},
              .sell = {},
          },
          &throwDuringIndicatorConstruction);

  ASSERT_FALSE(failed.ok());
  EXPECT_EQ(failed.error().code, bte::core::ErrorCode::internal);
  EXPECT_EQ(failed.error().message, "simulated indicator factory exception");
}

TEST(SelectableStrategyTest,
     unknownIndicatorFactoryExceptionIsContainedAtTheStrategyBoundary) {
  bte::core::Result<std::unique_ptr<bte::strategy::SelectableStrategy>> failed{
      bte::core::makeError(bte::core::ErrorCode::internal, "not called")};

  EXPECT_NO_THROW(
      failed = bte::strategy::detail::SelectableStrategyTestAccess::create(
          {
              .buy = {.conditions = {bte::strategy::Condition{
                          .source = bte::strategy::ConditionSource::indicator,
                          .indicator =
                              {
                                  .kind = bte::indicators::IndicatorKind::sma,
                                  .period = 2,
                              },
                      }}},
              .sell = {},
          },
          &throwUnknownDuringIndicatorConstruction));

  ASSERT_FALSE(failed.ok());
  EXPECT_EQ(failed.error().code, bte::core::ErrorCode::internal);
  EXPECT_EQ(failed.error().message, "indicator factory failed");
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
      .sell = {},
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
      .sell = {},
  });
  ASSERT_TRUE(created.ok());
  auto strategy = std::move(created).value();

  auto invalidBar = makeBar(2, 10.0);
  invalidBar.volume = std::numeric_limits<double>::infinity();
  const auto result = strategy->onBar(invalidBar);

  EXPECT_FALSE(result.ok());
  EXPECT_EQ(result.error().code, bte::core::ErrorCode::invalidArgument);
}

TEST(SelectableStrategyTest, nonFiniteBuyAndSellIndicatorOutputsArePropagated) {
  const auto overflowCondition = bte::strategy::Condition{
      .source = bte::strategy::ConditionSource::indicator,
      .comparison = bte::strategy::Comparison::greaterThan,
      .threshold = 0.0,
      .indicator =
          {
              .kind = bte::indicators::IndicatorKind::vwap,
              .period = 1,
          },
  };
  const auto harmlessCondition = bte::strategy::Condition{
      .source = bte::strategy::ConditionSource::barField,
      .comparison = bte::strategy::Comparison::greaterThan,
      .threshold = 0.0,
  };
  auto buyResult = bte::strategy::SelectableStrategy::create({
      .buy = {.conditions = {overflowCondition}},
      .sell = {},
  });
  auto sellResult = bte::strategy::SelectableStrategy::create({
      .buy = {.conditions = {harmlessCondition}},
      .sell = {.conditions = {overflowCondition}},
  });
  ASSERT_TRUE(buyResult.ok());
  ASSERT_TRUE(sellResult.ok());
  auto buyStrategy = std::move(buyResult).value();
  auto sellStrategy = std::move(sellResult).value();
  auto overflowBar = makeBar(2, std::numeric_limits<double>::max());
  overflowBar.open = std::numeric_limits<double>::max();
  overflowBar.high = std::numeric_limits<double>::max();
  overflowBar.low = std::numeric_limits<double>::max();
  overflowBar.volume = std::numeric_limits<double>::max();

  const auto buyUpdate = buyStrategy->onBar(overflowBar);
  const auto sellUpdate = sellStrategy->onBar(overflowBar);

  EXPECT_FALSE(buyUpdate.ok());
  EXPECT_FALSE(sellUpdate.ok());
  EXPECT_EQ(buyUpdate.error().code, bte::core::ErrorCode::invalidArgument);
  EXPECT_EQ(sellUpdate.error().code, bte::core::ErrorCode::invalidArgument);
}

} // namespace
