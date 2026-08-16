#include "Bte/Core/Bar.h"

#include <chrono>
#include <gtest/gtest.h>
#include <limits>

namespace {

using bte::core::Bar;
using bte::core::medianPrice;
using bte::core::SymbolBar;
using bte::core::Timestamp;
using bte::core::trueRange;
using bte::core::typicalPrice;

Timestamp makeTs(int64_t msSinceEpoch) {
  return Timestamp{std::chrono::milliseconds{msSinceEpoch}};
}

} // namespace

TEST(BarTest, isValid_returnsTrueForWellFormedPositiveBar) {
  const Bar bar{.ts = makeTs(1),
                .open = 10.0,
                .high = 12.0,
                .low = 9.0,
                .close = 11.0,
                .volume = 1'000.0};
  EXPECT_TRUE(bar.isValid());
}

TEST(BarTest, isValid_returnsFalseWhenHighLessThanLow) {
  const Bar bar{.ts = makeTs(1),
                .open = 10.0,
                .high = 9.0,
                .low = 10.0,
                .close = 10.0,
                .volume = 1.0};
  EXPECT_FALSE(bar.isValid());
}

TEST(BarTest, isValid_returnsFalseWhenOpenNotPositive) {
  const Bar bar{.ts = makeTs(1),
                .open = 0.0,
                .high = 1.0,
                .low = 0.5,
                .close = 0.8,
                .volume = 1.0};
  EXPECT_FALSE(bar.isValid());
}

TEST(BarTest, isValid_returnsFalseWhenVolumeNegative) {
  const Bar bar{.ts = makeTs(1),
                .open = 1.0,
                .high = 2.0,
                .low = 0.5,
                .close = 1.5,
                .volume = -1.0};
  EXPECT_FALSE(bar.isValid());
}

TEST(BarTest, isValid_returnsFalseWhenAnyOhlcvValueIsNotFinite) {
  auto bar = Bar{.ts = makeTs(1),
                 .open = 1.0,
                 .high = 2.0,
                 .low = 0.5,
                 .close = 1.5,
                 .volume = 1.0};
  const auto infinity = std::numeric_limits<double>::infinity();

  for (auto *value :
       {&bar.open, &bar.high, &bar.low, &bar.close, &bar.volume}) {
    const auto original = *value;
    *value = infinity;
    EXPECT_FALSE(bar.isValid());
    *value = original;
  }
}

TEST(BarTest, typicalPrice_returnsExpectedValueForValidBar) {
  const Bar bar{.ts = makeTs(1),
                .open = 10.0,
                .high = 14.0,
                .low = 8.0,
                .close = 12.0,
                .volume = 100.0};
  const auto got = typicalPrice(bar);
  ASSERT_TRUE(got.has_value());
  EXPECT_DOUBLE_EQ(*got, (14.0 + 8.0 + 12.0) / 3.0);
}

TEST(BarTest, typicalPrice_returnsNulloptForInvalidBar) {
  const Bar bar{.ts = makeTs(1),
                .open = 10.0,
                .high = 9.0,
                .low = 10.0,
                .close = 10.0,
                .volume = 1.0};
  EXPECT_FALSE(typicalPrice(bar).has_value());
}

TEST(BarTest, symbolBar_holdsSymbolAndBar) {
  const Bar bar{.ts = makeTs(1),
                .open = 1.0,
                .high = 2.0,
                .low = 0.9,
                .close = 1.5,
                .volume = 100.0};
  const SymbolBar sym{.symbol = "AAPL", .bar = bar};
  EXPECT_EQ(sym.symbol, "AAPL");
  EXPECT_DOUBLE_EQ(sym.bar.close, 1.5);
  EXPECT_TRUE(sym.bar.isValid());
}

TEST(BarTest, medianPrice_averagesHighAndLow) {
  const Bar bar{.ts = makeTs(2),
                .open = 12.0,
                .high = 20.0,
                .low = 10.0,
                .close = 15.0,
                .volume = 50.0};
  const auto got = medianPrice(bar);
  ASSERT_TRUE(got.has_value());
  EXPECT_DOUBLE_EQ(*got, 15.0);
}

TEST(BarTest, medianPrice_returnsNulloptForInvalidBar) {
  const Bar bar{.ts = makeTs(1),
                .open = 10.0,
                .high = 9.0,
                .low = 10.0,
                .close = 10.0,
                .volume = 1.0};
  EXPECT_FALSE(medianPrice(bar).has_value());
}

TEST(BarTest, trueRange_isHighMinusLow) {
  const Bar bar{.ts = makeTs(3),
                .open = 5.0,
                .high = 11.0,
                .low = 4.0,
                .close = 9.0,
                .volume = 10.0};
  const auto got = trueRange(bar);
  ASSERT_TRUE(got.has_value());
  EXPECT_DOUBLE_EQ(*got, 7.0);
  const auto gotExplicit = trueRange(bar, std::nullopt);
  ASSERT_TRUE(gotExplicit.has_value());
  EXPECT_DOUBLE_EQ(*gotExplicit, 7.0);
}

TEST(BarTest, trueRange_withPreviousCloseReturnsNulloptForInvalidBar) {
  const Bar bar{.ts = makeTs(3),
                .open = 10.0,
                .high = 9.0,
                .low = 10.0,
                .close = 10.0,
                .volume = 1.0};

  EXPECT_FALSE(trueRange(bar, 10.0).has_value());
}

TEST(
    BarTest,
    trueRange_withPrevClose_prefersDistanceThroughPrevOverIntrabarRange_gapDown) {
  const Bar bar{.ts = makeTs(4),
                .open = 98.0,
                .high = 98.0,
                .low = 90.0,
                .close = 92.0,
                .volume = 1.0};
  const auto got = trueRange(bar, 100.0);
  ASSERT_TRUE(got.has_value());
  EXPECT_DOUBLE_EQ(*got, 10.0);
}

TEST(
    BarTest,
    trueRange_withPrevClose_prefersDistanceThroughPrevOverIntrabarRange_gapUp) {
  const Bar bar{.ts = makeTs(5),
                .open = 108.0,
                .high = 115.0,
                .low = 105.0,
                .close = 110.0,
                .volume = 1.0};
  const auto got = trueRange(bar, 100.0);
  ASSERT_TRUE(got.has_value());
  EXPECT_DOUBLE_EQ(*got, 15.0);
}
