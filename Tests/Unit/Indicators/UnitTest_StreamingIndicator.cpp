#include "Bte/Indicators/StreamingIndicator.h"

#include "Bte/Core/Bar.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

std::vector<std::string> csvCells(const std::string &row) {
  std::istringstream stream{row};
  std::vector<std::string> result;
  for (std::string cell; std::getline(stream, cell, ',');) {
    result.push_back(std::move(cell));
  }
  return result;
}

template <typename Enum, std::size_t Size>
std::optional<Enum>
namedEnum(const std::string_view name,
          const std::array<std::pair<std::string_view, Enum>, Size> &entries) {
  const auto found = std::ranges::find_if(
      entries, [name](const auto &entry) { return entry.first == name; });
  if (found == entries.end()) {
    return std::nullopt;
  }
  return found->second;
}

TEST(StreamingIndicatorTest, catalogMatchesIndependentDecimalBatchReference) {
  using Kind = bte::indicators::IndicatorKind;
  using Output = bte::indicators::IndicatorOutput;
  using Field = bte::indicators::BarField;
  using Name = std::string_view;
  const std::array kinds{
      std::pair{Name{"sma"}, Kind::sma},
      std::pair{Name{"ema"}, Kind::ema},
      std::pair{Name{"wma"}, Kind::wma},
      std::pair{Name{"rsi"}, Kind::rsi},
      std::pair{Name{"macd"}, Kind::macd},
      std::pair{Name{"bollingerBands"}, Kind::bollingerBands},
      std::pair{Name{"atr"}, Kind::atr},
      std::pair{Name{"adx"}, Kind::adx},
      std::pair{Name{"stochastic"}, Kind::stochastic},
      std::pair{Name{"donchian"}, Kind::donchian},
      std::pair{Name{"vwap"}, Kind::vwap},
      std::pair{Name{"obv"}, Kind::obv},
      std::pair{Name{"roc"}, Kind::roc},
      std::pair{Name{"momentum"}, Kind::momentum},
      std::pair{Name{"trueRange"}, Kind::trueRange},
      std::pair{Name{"barField"}, Kind::barField},
  };
  const std::array outputs{
      std::pair{Name{"value"}, Output::value},
      std::pair{Name{"signal"}, Output::signal},
      std::pair{Name{"histogram"}, Output::histogram},
      std::pair{Name{"upper"}, Output::upper},
      std::pair{Name{"middle"}, Output::middle},
      std::pair{Name{"lower"}, Output::lower},
      std::pair{Name{"width"}, Output::width},
      std::pair{Name{"percentB"}, Output::percentB},
      std::pair{Name{"positiveDirectionalIndex"},
                Output::positiveDirectionalIndex},
      std::pair{Name{"negativeDirectionalIndex"},
                Output::negativeDirectionalIndex},
      std::pair{Name{"percentK"}, Output::percentK},
      std::pair{Name{"percentD"}, Output::percentD},
  };
  const std::array fields{
      std::pair{Name{"open"}, Field::open},
      std::pair{Name{"high"}, Field::high},
      std::pair{Name{"low"}, Field::low},
      std::pair{Name{"close"}, Field::close},
      std::pair{Name{"volume"}, Field::volume},
  };
  const auto directory = std::filesystem::path{BTE_INDICATOR_FIXTURE_DIR};
  std::ifstream barFile{directory / "Bars.csv"};
  ASSERT_TRUE(barFile.is_open());
  std::string row;
  ASSERT_TRUE(static_cast<bool>(std::getline(barFile, row)));
  std::vector<bte::core::Bar> bars;
  while (std::getline(barFile, row)) {
    const auto cells = csvCells(row);
    ASSERT_EQ(cells.size(), 6U);
    bars.push_back({
        .ts = bte::core::Timestamp{std::chrono::milliseconds{
            std::stoll(cells[0])}},
        .open = std::stod(cells[1]),
        .high = std::stod(cells[2]),
        .low = std::stod(cells[3]),
        .close = std::stod(cells[4]),
        .volume = std::stod(cells[5]),
    });
  }
  ASSERT_EQ(bars.size(), 140U);
  std::ifstream expectedFile{directory / "Expected.csv"};
  ASSERT_TRUE(expectedFile.is_open());
  ASSERT_TRUE(static_cast<bool>(std::getline(expectedFile, row)));
  std::size_t configurations = 0;
  while (std::getline(expectedFile, row)) {
    const auto cells = csvCells(row);
    ASSERT_EQ(cells.size(), 6U + bars.size());
    SCOPED_TRACE(cells[0] + ":" + cells[1] + ":" + cells[4] + ":" + cells[5]);
    const auto kind = namedEnum(cells[0], kinds);
    const auto output = namedEnum(cells[4], outputs);
    const auto field = namedEnum(cells[5], fields);
    ASSERT_TRUE(kind.has_value());
    ASSERT_TRUE(output.has_value());
    ASSERT_TRUE(field.has_value());
    auto created = bte::indicators::StreamingIndicator::create({
        .kind = *kind,
        .period = std::stoi(cells[1]),
        .secondaryPeriod = std::stoi(cells[2]),
        .signalPeriod = std::stoi(cells[3]),
        .output = *output,
        .field = *field,
    });
    ASSERT_TRUE(created.ok());
    auto indicator = std::move(created).value();
    for (std::size_t index = 0; index < bars.size(); ++index) {
      SCOPED_TRACE(index);
      const auto updated = indicator.update(bars[index]);
      ASSERT_TRUE(updated.ok());
      const auto &expected = cells[index + 6U];
      if (expected == "none") {
        EXPECT_FALSE(updated.value().has_value());
      } else {
        ASSERT_TRUE(updated.value().has_value());
        const auto reference = std::stod(expected);
        EXPECT_NEAR(updated.value()->value, reference,
                    std::max(1e-8, std::abs(reference) * 1e-10));
      }
    }
    ++configurations;
  }
  EXPECT_EQ(configurations, 124U);
}

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

TEST(StreamingIndicatorTest, bollingerPreservesSmallSpreadsAtNonzeroPrice) {
  auto created = bte::indicators::StreamingIndicator::create({
      .kind = bte::indicators::IndicatorKind::bollingerBands,
      .period = 2,
      .output = bte::indicators::IndicatorOutput::upper,
  });
  ASSERT_TRUE(created.ok());
  auto indicator = std::move(created).value();
  EXPECT_FALSE(indicator.update(makeBar(1, 100.0)).value().has_value());
  const std::array closes{100.000001, 100.000002, 100.000003, 100.000003,
                          100.000003};
  // A two-value population has sigma = abs(b-a)/2, so upper is
  // (a+b)/2 + abs(b-a). Equal values have exactly zero deviation.
  const std::array expected{100.0000015, 100.0000025, 100.0000035, 100.000003,
                            100.000003};
  for (std::size_t index = 0; index < closes.size(); ++index) {
    const auto updated =
        indicator.update(makeBar(static_cast<int>(index) + 2, closes[index]));
    ASSERT_TRUE(updated.ok());
    ASSERT_TRUE(updated.value().has_value());
    EXPECT_NEAR(updated.value()->value, expected[index], 1e-12);
  }
}

TEST(StreamingIndicatorTest, expiredLargeValuesCannotCorruptRemainingWindow) {
  for (const auto kind : {bte::indicators::IndicatorKind::sma,
                          bte::indicators::IndicatorKind::wma,
                          bte::indicators::IndicatorKind::vwap}) {
    SCOPED_TRACE(static_cast<int>(kind));
    auto created = bte::indicators::StreamingIndicator::create({
        .kind = kind,
        .period = 2,
    });
    ASSERT_TRUE(created.ok());
    auto indicator = std::move(created).value();
    ASSERT_TRUE(indicator.update(makeBar(1, 1e16)).ok());
    ASSERT_TRUE(indicator.update(makeBar(2, 1.0)).ok());
    for (int day = 3; day <= 12; ++day) {
      const auto updated = indicator.update(makeBar(day, 1.0));
      ASSERT_TRUE(updated.ok());
      ASSERT_TRUE(updated.value().has_value());
      EXPECT_DOUBLE_EQ(updated.value()->value, 1.0);
    }
  }
}

TEST(StreamingIndicatorTest, derivedVolumeValuesRetainTheirNumericDomain) {
  using Kind = bte::indicators::IndicatorKind;
  for (const auto kind : {Kind::sma, Kind::ema, Kind::wma, Kind::macd,
                          Kind::bollingerBands, Kind::vwap, Kind::momentum}) {
    SCOPED_TRACE(static_cast<int>(kind));
    auto created = bte::indicators::StreamingIndicator::create({
        .kind = kind,
        .period = 2,
        .secondaryPeriod = 3,
        .signalPeriod = 2,
        .output = kind == Kind::bollingerBands
                      ? bte::indicators::IndicatorOutput::upper
                      : bte::indicators::IndicatorOutput::value,
        .field = bte::indicators::BarField::volume,
    });
    ASSERT_TRUE(created.ok());
    auto indicator = std::move(created).value();
    for (int day = 1; day <= 6; ++day) {
      auto bar = makeBar(day, 100.0);
      bar.volume = 10.0 * static_cast<double>(day);
      ASSERT_TRUE(indicator.update(bar).ok());
    }
    ASSERT_TRUE(indicator.latest().has_value());
    EXPECT_EQ(indicator.latest()->domain,
              bte::indicators::NumericDomain::volume);
  }
}

TEST(StreamingIndicatorTest, finiteSmoothingMustNotOverflowAnIntermediateSum) {
  using Kind = bte::indicators::IndicatorKind;
  const auto maximum = std::numeric_limits<double>::max();
  for (const auto kind : {Kind::ema, Kind::atr}) {
    SCOPED_TRACE(static_cast<int>(kind));
    auto created = bte::indicators::StreamingIndicator::create({
        .kind = kind,
        .period = 2,
    });
    ASSERT_TRUE(created.ok());
    auto indicator = std::move(created).value();
    for (int day = 1; day <= 5; ++day) {
      auto bar = makeBar(day, 100.0);
      bar.open = bar.high = bar.close = maximum;
      bar.low = kind == Kind::atr ? 1.0 : maximum;
      const auto updated = indicator.update(bar);
      ASSERT_TRUE(updated.ok());
      if (day >= 2) {
        ASSERT_TRUE(updated.value().has_value());
        EXPECT_DOUBLE_EQ(updated.value()->value, maximum);
      }
    }
  }
}

TEST(StreamingIndicatorTest, ratiosAndMidpointsAvoidPrematureOverflow) {
  using Kind = bte::indicators::IndicatorKind;
  using Output = bte::indicators::IndicatorOutput;
  const auto maximum = std::numeric_limits<double>::max();
  const std::array definitions{
      bte::indicators::IndicatorDefinition{
          .kind = Kind::stochastic, .period = 1, .output = Output::percentK},
      bte::indicators::IndicatorDefinition{
          .kind = Kind::donchian, .period = 1, .output = Output::middle},
      bte::indicators::IndicatorDefinition{.kind = Kind::roc, .period = 1},
      bte::indicators::IndicatorDefinition{
          .kind = Kind::adx,
          .period = 1,
          .output = Output::positiveDirectionalIndex},
  };
  const std::array expected{100.0, maximum * 0.75, -50.0, 100.0};
  for (std::size_t index = 0; index < definitions.size(); ++index) {
    SCOPED_TRACE(index);
    auto created =
        bte::indicators::StreamingIndicator::create(definitions[index]);
    ASSERT_TRUE(created.ok());
    auto indicator = std::move(created).value();
    auto first =
        makeBar(1, definitions[index].kind == Kind::roc ? maximum : 1.0);
    first.open = first.high = first.low = first.close;
    ASSERT_TRUE(indicator.update(first).ok());
    auto last = makeBar(2, maximum);
    last.open = last.close = maximum;
    last.high = maximum;
    last.low = definitions[index].kind == Kind::donchian ? maximum / 2.0 : 1.0;
    if (definitions[index].kind == Kind::roc) {
      last.open = last.close = last.high = last.low = maximum / 2.0;
    }
    const auto updated = indicator.update(last);
    ASSERT_TRUE(updated.ok());
    ASSERT_TRUE(updated.value().has_value());
    EXPECT_DOUBLE_EQ(updated.value()->value, expected[index]);
  }
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
      .kind = bte::indicators::IndicatorKind::bollingerBands,
      .period = 2,
      .output = bte::indicators::IndicatorOutput::upper,
  });
  ASSERT_TRUE(created.ok());
  auto indicator = std::move(created).value();
  ASSERT_TRUE(indicator.update(makeBar(1, 1.0)).ok());
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

TEST(StreamingIndicatorTest,
     vwapAvoidsIntermediateOverflowAndRecoversZeroVolume) {
  auto created = bte::indicators::StreamingIndicator::create({
      .kind = bte::indicators::IndicatorKind::vwap,
      .period = 2,
  });
  ASSERT_TRUE(created.ok());
  auto indicator = std::move(created).value();
  auto bar = makeBar(1, 100.0);
  bar.volume = std::numeric_limits<double>::max();
  ASSERT_TRUE(indicator.update(bar).ok());
  bar.ts += std::chrono::hours{1};
  const auto large = indicator.update(bar);
  ASSERT_TRUE(large.ok());
  ASSERT_TRUE(large.value().has_value());
  EXPECT_DOUBLE_EQ(large.value()->value, 100.0);
  bar.volume = 0.0;
  bar.ts += std::chrono::hours{1};
  ASSERT_TRUE(indicator.update(bar).ok());
  bar.ts += std::chrono::hours{1};
  const auto empty = indicator.update(bar);
  ASSERT_TRUE(empty.ok());
  EXPECT_FALSE(empty.value().has_value());
  bar.close = bar.open = 101.0;
  bar.high = 102.0;
  bar.volume = 1.0;
  bar.ts += std::chrono::hours{1};
  const auto recovered = indicator.update(bar);
  ASSERT_TRUE(recovered.ok());
  ASSERT_TRUE(recovered.value().has_value());
  EXPECT_DOUBLE_EQ(recovered.value()->value, 101.0);
  indicator.reset();
  EXPECT_EQ(indicator.consumedBars(), 0);
  EXPECT_FALSE(indicator.latest().has_value());
  EXPECT_FALSE(indicator.update(bar).value().has_value());
}

TEST(StreamingIndicatorTest, vwapPreservesExtremePriceVolumeProducts) {
  struct Case {
    double firstPrice;
    double firstVolume;
    double secondPrice;
    double secondVolume;
    double expected;
  };
  const auto maximum = std::numeric_limits<double>::max();
  for (const auto &example :
       std::array{Case{1e308, 1e-200, 1e-200, 1e200, 1e-92},
                  Case{1e308, 1e-20, 1e-100, 1e308, 1e-20},
                  Case{maximum, maximum, maximum, maximum, maximum},
                  Case{1e-200, 1e-200, 1e-200, 1e-200, 1e-200}}) {
    auto created = bte::indicators::StreamingIndicator::create(
        {.kind = bte::indicators::IndicatorKind::vwap, .period = 2});
    ASSERT_TRUE(created.ok());
    auto indicator = std::move(created).value();
    auto bar = makeBar(1, example.firstPrice);
    bar.open = bar.high = bar.low = bar.close = example.firstPrice;
    bar.volume = example.firstVolume;
    ASSERT_TRUE(indicator.update(bar).ok());
    bar.ts += std::chrono::hours{1};
    bar.open = bar.high = bar.low = bar.close = example.secondPrice;
    bar.volume = example.secondVolume;
    const auto updated = indicator.update(bar);
    ASSERT_TRUE(updated.ok());
    ASSERT_TRUE(updated.value().has_value());
    EXPECT_NEAR(updated.value()->value / example.expected, 1.0, 1e-14);
  }
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

TEST(StreamingIndicatorTest, resetDiscardsBothRollingStacks) {
  using bte::indicators::IndicatorKind;
  for (const auto kind : {IndicatorKind::sma, IndicatorKind::wma,
                          IndicatorKind::bollingerBands, IndicatorKind::vwap}) {
    auto created = bte::indicators::StreamingIndicator::create(
        {.kind = kind,
         .period = 3,
         .output = kind == IndicatorKind::bollingerBands
                       ? bte::indicators::IndicatorOutput::upper
                       : bte::indicators::IndicatorOutput::value});
    ASSERT_TRUE(created.ok());
    auto indicator = std::move(created).value();
    for (int day = 1; day <= 10; ++day) {
      ASSERT_TRUE(indicator.update(makeBar(day, 1e15 + day)).ok());
    }
    indicator.reset();
    EXPECT_EQ(indicator.consumedBars(), 0);
    EXPECT_FALSE(indicator.latest().has_value());
    for (int day = 1; day <= 10; ++day) {
      const auto updated = indicator.update(makeBar(day, 5.0));
      ASSERT_TRUE(updated.ok());
      if (day < 3) {
        EXPECT_FALSE(updated.value().has_value());
      } else {
        ASSERT_TRUE(updated.value().has_value());
        EXPECT_DOUBLE_EQ(updated.value()->value, 5.0);
      }
    }
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
