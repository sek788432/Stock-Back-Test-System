#include "Bte/Indicators/StreamingIndicator.h"

#include "Bte/Core/Bar.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <limits>
#include <optional>
#include <vector>

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

TEST(StreamingIndicatorTest, smaWarmsAtConfiguredPeriodAndUsesActualBars) {
  auto created = bte::indicators::StreamingIndicator::create({
      .kind = bte::indicators::IndicatorKind::sma,
      .period = 3,
  });
  ASSERT_TRUE(created.ok());
  auto indicator = std::move(created).value();

  EXPECT_FALSE(indicator.update(makeBar(2, 10.0)).value().has_value());
  EXPECT_FALSE(indicator.update(makeBar(3, 20.0)).value().has_value());
  const auto third = indicator.update(makeBar(4, 30.0));

  ASSERT_TRUE(third.ok());
  ASSERT_TRUE(third.value().has_value());
  EXPECT_DOUBLE_EQ(third.value()->value, 20.0);
  EXPECT_EQ(third.value()->domain, bte::indicators::NumericDomain::price);
}

TEST(StreamingIndicatorTest,
     macdExposesSignalAndHistogramAfterIndependentWarmup) {
  auto created = bte::indicators::StreamingIndicator::create({
      .kind = bte::indicators::IndicatorKind::macd,
      .period = 2,
      .secondaryPeriod = 3,
      .signalPeriod = 2,
      .output = bte::indicators::IndicatorOutput::histogram,
  });
  ASSERT_TRUE(created.ok());
  auto indicator = std::move(created).value();

  std::optional<bte::indicators::IndicatorValue> latest;
  for (const auto close : {10.0, 11.0, 13.0, 16.0}) {
    const auto updated =
        indicator.update(makeBar(static_cast<int>(close), close));
    ASSERT_TRUE(updated.ok());
    latest = updated.value();
  }

  ASSERT_TRUE(latest.has_value());
  EXPECT_NEAR(latest->value, 0.1111111111, 0.000001);
  EXPECT_EQ(latest->domain, bte::indicators::NumericDomain::price);
}

TEST(StreamingIndicatorTest, bollingerAndDonchianExposeRequestedOutputs) {
  auto bands = bte::indicators::StreamingIndicator::create({
      .kind = bte::indicators::IndicatorKind::bollingerBands,
      .period = 3,
      .output = bte::indicators::IndicatorOutput::upper,
  });
  auto channel = bte::indicators::StreamingIndicator::create({
      .kind = bte::indicators::IndicatorKind::donchian,
      .period = 3,
      .output = bte::indicators::IndicatorOutput::lower,
  });
  ASSERT_TRUE(bands.ok());
  ASSERT_TRUE(channel.ok());
  auto bollinger = std::move(bands).value();
  auto donchian = std::move(channel).value();

  for (const auto close : {10.0, 20.0, 30.0}) {
    ASSERT_TRUE(bollinger.update(makeBar(static_cast<int>(close), close)).ok());
    ASSERT_TRUE(donchian.update(makeBar(static_cast<int>(close), close)).ok());
  }

  ASSERT_TRUE(bollinger.latest().has_value());
  ASSERT_TRUE(donchian.latest().has_value());
  EXPECT_NEAR(bollinger.latest()->value, 36.3299316, 0.000001);
  EXPECT_DOUBLE_EQ(donchian.latest()->value, 9.0);
}

TEST(StreamingIndicatorTest, invalidDefinitionsAndBarsReturnStructuredErrors) {
  const auto noPeriod = bte::indicators::StreamingIndicator::create({
      .kind = bte::indicators::IndicatorKind::rsi,
      .period = 0,
  });
  const auto invalidOutput = bte::indicators::StreamingIndicator::create({
      .kind = bte::indicators::IndicatorKind::sma,
      .period = 2,
      .output = bte::indicators::IndicatorOutput::upper,
  });
  auto created = bte::indicators::StreamingIndicator::create({
      .kind = bte::indicators::IndicatorKind::ema,
      .period = 2,
  });
  ASSERT_TRUE(created.ok());
  auto indicator = std::move(created).value();
  auto invalidBar = makeBar(2, 10.0);
  invalidBar.low = 0.0;
  const auto update = indicator.update(invalidBar);

  EXPECT_FALSE(noPeriod.ok());
  EXPECT_FALSE(invalidOutput.ok());
  EXPECT_FALSE(update.ok());
  EXPECT_EQ(noPeriod.error().code, bte::core::ErrorCode::invalidArgument);
  EXPECT_EQ(invalidOutput.error().code, bte::core::ErrorCode::invalidArgument);
  EXPECT_EQ(update.error().code, bte::core::ErrorCode::invalidArgument);
}

TEST(StreamingIndicatorTest, nonFiniteComputedOutputReturnsStructuredError) {
  auto created = bte::indicators::StreamingIndicator::create({
      .kind = bte::indicators::IndicatorKind::vwap,
      .period = 1,
  });
  ASSERT_TRUE(created.ok());
  auto indicator = std::move(created).value();
  auto overflowBar = makeBar(2, std::numeric_limits<double>::max());
  overflowBar.open = std::numeric_limits<double>::max();
  overflowBar.high = std::numeric_limits<double>::max();
  overflowBar.low = std::numeric_limits<double>::max();
  overflowBar.volume = std::numeric_limits<double>::max();

  const auto updated = indicator.update(overflowBar);

  EXPECT_FALSE(updated.ok());
  EXPECT_EQ(updated.error().code, bte::core::ErrorCode::invalidArgument);
  EXPECT_FALSE(indicator.latest().has_value());
}

TEST(StreamingIndicatorTest, rejectsInvalidEnumAndMultiPeriodDefinitions) {
  using bte::indicators::BarField;
  using bte::indicators::IndicatorKind;

  const auto unknownKind = bte::indicators::StreamingIndicator::create({
      .kind = static_cast<IndicatorKind>(99),
  });
  const auto unknownField = bte::indicators::StreamingIndicator::create({
      .kind = IndicatorKind::barField,
      .field = static_cast<BarField>(99),
  });
  const auto invalidMacd = bte::indicators::StreamingIndicator::create({
      .kind = IndicatorKind::macd,
      .period = 3,
      .secondaryPeriod = 2,
      .signalPeriod = 1,
  });
  const auto invalidStochastic = bte::indicators::StreamingIndicator::create({
      .kind = IndicatorKind::stochastic,
      .period = 2,
      .signalPeriod = bte::indicators::maximumIndicatorPeriod + 1,
  });

  EXPECT_FALSE(unknownKind.ok());
  EXPECT_FALSE(unknownField.ok());
  EXPECT_FALSE(invalidMacd.ok());
  EXPECT_FALSE(invalidStochastic.ok());
  EXPECT_EQ(unknownKind.error().code, bte::core::ErrorCode::invalidArgument);
  EXPECT_EQ(unknownField.error().code, bte::core::ErrorCode::invalidArgument);
  EXPECT_EQ(invalidMacd.error().code, bte::core::ErrorCode::invalidArgument);
  EXPECT_EQ(invalidStochastic.error().code,
            bte::core::ErrorCode::invalidArgument);
}

TEST(StreamingIndicatorTest, barFieldHelpersPreserveValuesAndDomains) {
  const auto bar = bte::core::Bar{
      .ts = bte::core::Timestamp{std::chrono::sys_days{std::chrono::year{2024} /
                                                       1 / 2}},
      .open = 10.0,
      .high = 13.0,
      .low = 8.0,
      .close = 11.0,
      .volume = 250.0,
  };

  const auto open =
      bte::indicators::barFieldValue(bar, bte::indicators::BarField::open);
  const auto high =
      bte::indicators::barFieldValue(bar, bte::indicators::BarField::high);
  const auto low =
      bte::indicators::barFieldValue(bar, bte::indicators::BarField::low);
  const auto volume =
      bte::indicators::barFieldValue(bar, bte::indicators::BarField::volume);
  const auto unknown = bte::indicators::barFieldValue(
      bar, static_cast<bte::indicators::BarField>(99));

  EXPECT_TRUE(
      bte::indicators::isValidBarField(bte::indicators::BarField::close));
  EXPECT_FALSE(bte::indicators::isValidBarField(
      static_cast<bte::indicators::BarField>(99)));
  EXPECT_DOUBLE_EQ(open.value, 10.0);
  EXPECT_DOUBLE_EQ(high.value, 13.0);
  EXPECT_DOUBLE_EQ(low.value, 8.0);
  EXPECT_DOUBLE_EQ(volume.value, 250.0);
  EXPECT_EQ(open.domain, bte::indicators::NumericDomain::price);
  EXPECT_EQ(volume.domain, bte::indicators::NumericDomain::volume);
  EXPECT_EQ(unknown.domain, bte::indicators::NumericDomain::scalar);
  EXPECT_EQ(bte::indicators::indicatorOutputDomain(
                {.kind = static_cast<bte::indicators::IndicatorKind>(99)}),
            bte::indicators::NumericDomain::scalar);
}

TEST(StreamingIndicatorTest, handlesNeutralRsiAndZeroVolumeRocBoundaries) {
  auto rsi = bte::indicators::StreamingIndicator::create({
      .kind = bte::indicators::IndicatorKind::rsi,
      .period = 1,
  });
  auto roc = bte::indicators::StreamingIndicator::create({
      .kind = bte::indicators::IndicatorKind::roc,
      .period = 2,
      .field = bte::indicators::BarField::volume,
  });
  ASSERT_TRUE(rsi.ok());
  ASSERT_TRUE(roc.ok());
  auto neutralRsi = std::move(rsi).value();
  auto zeroVolumeRoc = std::move(roc).value();
  auto zeroVolumeBar = makeBar(2, 10.0);
  zeroVolumeBar.volume = 0.0;

  EXPECT_FALSE(neutralRsi.latest().has_value());
  EXPECT_FALSE(neutralRsi.update(makeBar(2, 10.0)).value().has_value());
  const auto neutral = neutralRsi.update(makeBar(3, 10.0));
  ASSERT_TRUE(neutral.ok());
  ASSERT_TRUE(neutral.value().has_value());
  EXPECT_DOUBLE_EQ(neutral.value()->value, 50.0);
  EXPECT_EQ(neutralRsi.definition().kind, bte::indicators::IndicatorKind::rsi);

  EXPECT_FALSE(zeroVolumeRoc.update(zeroVolumeBar).value().has_value());
  ASSERT_TRUE(zeroVolumeRoc.update(zeroVolumeBar).ok());
  const auto undefinedRoc = zeroVolumeRoc.update(zeroVolumeBar);
  ASSERT_TRUE(undefinedRoc.ok());
  EXPECT_FALSE(undefinedRoc.value().has_value());
}

TEST(StreamingIndicatorTest, constantBollingerBandUsesNeutralPercentB) {
  auto created = bte::indicators::StreamingIndicator::create({
      .kind = bte::indicators::IndicatorKind::bollingerBands,
      .period = 2,
      .output = bte::indicators::IndicatorOutput::percentB,
  });
  ASSERT_TRUE(created.ok());
  auto indicator = std::move(created).value();

  ASSERT_TRUE(indicator.update(makeBar(2, 10.0)).ok());
  const auto second = indicator.update(makeBar(3, 10.0));

  ASSERT_TRUE(second.ok());
  ASSERT_TRUE(second.value().has_value());
  EXPECT_DOUBLE_EQ(second.value()->value, 0.5);
}

TEST(StreamingIndicatorTest,
     maximumSupportedPeriodIsAcceptedAndNextValueIsRejected) {
  const auto maximum = bte::indicators::StreamingIndicator::create({
      .kind = bte::indicators::IndicatorKind::sma,
      .period = bte::indicators::maximumIndicatorPeriod,
  });
  const auto tooLarge = bte::indicators::StreamingIndicator::create({
      .kind = bte::indicators::IndicatorKind::sma,
      .period = bte::indicators::maximumIndicatorPeriod + 1,
  });

  EXPECT_TRUE(maximum.ok());
  EXPECT_FALSE(tooLarge.ok());
  EXPECT_EQ(tooLarge.error().code, bte::core::ErrorCode::invalidArgument);
}

TEST(StreamingIndicatorTest, catalogOutputsMatchFixedOracles) {
  using bte::indicators::IndicatorDefinition;
  using bte::indicators::IndicatorKind;
  using bte::indicators::IndicatorOutput;
  using bte::indicators::NumericDomain;
  const auto bars = std::vector{
      bte::core::Bar{.ts = bte::core::Timestamp{std::chrono::sys_days{
                         std::chrono::year{2024} / 1 / 2}},
                     .open = 10.0,
                     .high = 12.0,
                     .low = 9.0,
                     .close = 10.0,
                     .volume = 100.0},
      bte::core::Bar{.ts = bte::core::Timestamp{std::chrono::sys_days{
                         std::chrono::year{2024} / 1 / 3}},
                     .open = 11.0,
                     .high = 14.0,
                     .low = 10.0,
                     .close = 12.0,
                     .volume = 200.0},
      bte::core::Bar{.ts = bte::core::Timestamp{std::chrono::sys_days{
                         std::chrono::year{2024} / 1 / 4}},
                     .open = 12.0,
                     .high = 13.0,
                     .low = 8.0,
                     .close = 9.0,
                     .volume = 150.0},
      bte::core::Bar{.ts = bte::core::Timestamp{std::chrono::sys_days{
                         std::chrono::year{2024} / 1 / 5}},
                     .open = 10.0,
                     .high = 16.0,
                     .low = 9.0,
                     .close = 15.0,
                     .volume = 250.0},
      bte::core::Bar{.ts = bte::core::Timestamp{std::chrono::sys_days{
                         std::chrono::year{2024} / 1 / 6}},
                     .open = 15.0,
                     .high = 18.0,
                     .low = 14.0,
                     .close = 16.0,
                     .volume = 300.0},
      bte::core::Bar{.ts = bte::core::Timestamp{std::chrono::sys_days{
                         std::chrono::year{2024} / 1 / 7}},
                     .open = 16.0,
                     .high = 17.0,
                     .low = 11.0,
                     .close = 12.0,
                     .volume = 175.0},
  };
  struct OutputCase {
    const char *name;
    IndicatorDefinition definition;
    double expected;
    NumericDomain domain;
  };
  auto cases = std::vector<OutputCase>{
      {"SMA",
       {.kind = IndicatorKind::sma, .period = 3},
       0.0,
       NumericDomain::price},
      {"EMA",
       {.kind = IndicatorKind::ema, .period = 3},
       0.0,
       NumericDomain::price},
      {"WMA",
       {.kind = IndicatorKind::wma, .period = 3},
       0.0,
       NumericDomain::price},
      {"RSI",
       {.kind = IndicatorKind::rsi, .period = 3},
       0.0,
       NumericDomain::percent},
      {"MACD line",
       {.kind = IndicatorKind::macd,
        .period = 2,
        .secondaryPeriod = 3,
        .signalPeriod = 2,
        .output = IndicatorOutput::value},
       0.0,
       NumericDomain::price},
      {"MACD signal",
       {.kind = IndicatorKind::macd,
        .period = 2,
        .secondaryPeriod = 3,
        .signalPeriod = 2,
        .output = IndicatorOutput::signal},
       0.0,
       NumericDomain::price},
      {"MACD histogram",
       {.kind = IndicatorKind::macd,
        .period = 2,
        .secondaryPeriod = 3,
        .signalPeriod = 2,
        .output = IndicatorOutput::histogram},
       0.0,
       NumericDomain::price},
      {"Bollinger upper",
       {.kind = IndicatorKind::bollingerBands,
        .period = 3,
        .output = IndicatorOutput::upper},
       0.0,
       NumericDomain::price},
      {"Bollinger middle",
       {.kind = IndicatorKind::bollingerBands,
        .period = 3,
        .output = IndicatorOutput::middle},
       0.0,
       NumericDomain::price},
      {"Bollinger lower",
       {.kind = IndicatorKind::bollingerBands,
        .period = 3,
        .output = IndicatorOutput::lower},
       0.0,
       NumericDomain::price},
      {"Bollinger width",
       {.kind = IndicatorKind::bollingerBands,
        .period = 3,
        .output = IndicatorOutput::width},
       0.0,
       NumericDomain::scalar},
      {"Bollinger percent B",
       {.kind = IndicatorKind::bollingerBands,
        .period = 3,
        .output = IndicatorOutput::percentB},
       0.0,
       NumericDomain::scalar},
      {"ATR",
       {.kind = IndicatorKind::atr, .period = 3},
       0.0,
       NumericDomain::price},
      {"ADX",
       {.kind = IndicatorKind::adx,
        .period = 2,
        .output = IndicatorOutput::value},
       0.0,
       NumericDomain::percent},
      {"ADX positive DI",
       {.kind = IndicatorKind::adx,
        .period = 2,
        .output = IndicatorOutput::positiveDirectionalIndex},
       0.0,
       NumericDomain::percent},
      {"ADX negative DI",
       {.kind = IndicatorKind::adx,
        .period = 2,
        .output = IndicatorOutput::negativeDirectionalIndex},
       0.0,
       NumericDomain::percent},
      {"Stochastic percent K",
       {.kind = IndicatorKind::stochastic,
        .period = 3,
        .output = IndicatorOutput::percentK},
       0.0,
       NumericDomain::percent},
      {"Stochastic percent D",
       {.kind = IndicatorKind::stochastic,
        .period = 3,
        .signalPeriod = 2,
        .output = IndicatorOutput::percentD},
       0.0,
       NumericDomain::percent},
      {"Donchian upper",
       {.kind = IndicatorKind::donchian,
        .period = 3,
        .output = IndicatorOutput::upper},
       0.0,
       NumericDomain::price},
      {"Donchian middle",
       {.kind = IndicatorKind::donchian,
        .period = 3,
        .output = IndicatorOutput::middle},
       0.0,
       NumericDomain::price},
      {"Donchian lower",
       {.kind = IndicatorKind::donchian,
        .period = 3,
        .output = IndicatorOutput::lower},
       0.0,
       NumericDomain::price},
      {"VWAP",
       {.kind = IndicatorKind::vwap, .period = 3},
       0.0,
       NumericDomain::price},
      {"OBV", {.kind = IndicatorKind::obv}, 0.0, NumericDomain::volume},
      {"ROC",
       {.kind = IndicatorKind::roc, .period = 3},
       0.0,
       NumericDomain::percent},
      {"Momentum",
       {.kind = IndicatorKind::momentum, .period = 3},
       0.0,
       NumericDomain::price},
      {"True range",
       {.kind = IndicatorKind::trueRange},
       0.0,
       NumericDomain::price},
      {"Volume field",
       {.kind = IndicatorKind::barField,
        .field = bte::indicators::BarField::volume},
       0.0,
       NumericDomain::volume},
  };

  // Independently calculated from the six fixed OHLCV rows above. Values are
  // deliberately literal fixtures rather than an in-test implementation.
  const auto expectedValues = std::array<double, 27>{
      14.3333333333, 13.1666666667, 13.8333333333, 44.1860465116, -0.1419753086,
      0.0637860082,  -0.2057613169, 17.7326796757, 14.3333333333, 10.9339869909,
      0.4743273966,  0.1567967635,  5.1111111111,  38.8492063492, 18.3908045977,
      29.8850574713, 33.3333333333, 56.6666666667, 18.0,          13.5,
      9.0,           14.6896551724, 425.0,         33.3333333333, 3.0,
      6.0,           175.0,
  };
  const auto firstReadyAt = std::array<std::size_t, 27>{
      3, 3, 3, 4, 3, 4, 4, 3, 3, 3, 3, 3, 3, 4,
      3, 3, 3, 4, 3, 3, 3, 3, 1, 4, 4, 1, 1,
  };
  ASSERT_EQ(cases.size(), expectedValues.size());
  ASSERT_EQ(cases.size(), firstReadyAt.size());

  for (std::size_t caseIndex = 0; caseIndex < cases.size(); ++caseIndex) {
    auto &outputCase = cases[caseIndex];
    outputCase.expected = expectedValues[caseIndex];
    SCOPED_TRACE(outputCase.name);
    auto created =
        bte::indicators::StreamingIndicator::create(outputCase.definition);
    ASSERT_TRUE(created.ok());
    auto indicator = std::move(created).value();
    std::optional<bte::indicators::IndicatorValue> latest;
    for (std::size_t barIndex = 0; barIndex < bars.size(); ++barIndex) {
      const auto &bar = bars[barIndex];
      const auto updated = indicator.update(bar);
      ASSERT_TRUE(updated.ok());
      latest = updated.value();
      if (barIndex + 1U < firstReadyAt[caseIndex]) {
        EXPECT_FALSE(latest.has_value());
      }
    }
    ASSERT_TRUE(latest.has_value());
    EXPECT_NEAR(latest->value, outputCase.expected, 0.000001);
    EXPECT_EQ(latest->domain, outputCase.domain);
  }
}

TEST(StreamingIndicatorTest, resetRestoresWarmupState) {
  auto created = bte::indicators::StreamingIndicator::create({
      .kind = bte::indicators::IndicatorKind::sma,
      .period = 2,
  });
  ASSERT_TRUE(created.ok());
  auto indicator = std::move(created).value();
  ASSERT_TRUE(indicator.update(makeBar(2, 10.0)).ok());
  ASSERT_TRUE(indicator.update(makeBar(3, 20.0)).ok());
  ASSERT_TRUE(indicator.latest().has_value());

  indicator.reset();
  const auto resetUpdate = indicator.update(makeBar(4, 30.0));

  ASSERT_TRUE(resetUpdate.ok());
  EXPECT_EQ(indicator.consumedBars(), 1);
  EXPECT_FALSE(resetUpdate.value().has_value());
}

TEST(StreamingIndicatorTest, moveAssignmentPreservesIndicatorState) {
  auto sourceResult = bte::indicators::StreamingIndicator::create({
      .kind = bte::indicators::IndicatorKind::sma,
      .period = 2,
  });
  auto destinationResult = bte::indicators::StreamingIndicator::create({
      .kind = bte::indicators::IndicatorKind::ema,
      .period = 3,
  });
  ASSERT_TRUE(sourceResult.ok());
  ASSERT_TRUE(destinationResult.ok());
  auto source = std::move(sourceResult).value();
  auto destination = std::move(destinationResult).value();

  ASSERT_TRUE(source.update(makeBar(2, 10.0)).ok());
  destination = std::move(source);
  const auto updated = destination.update(makeBar(3, 14.0));

  ASSERT_TRUE(updated.ok());
  ASSERT_TRUE(updated.value().has_value());
  EXPECT_DOUBLE_EQ(updated.value()->value, 12.0);
  EXPECT_EQ(destination.definition().kind, bte::indicators::IndicatorKind::sma);
}

} // namespace
