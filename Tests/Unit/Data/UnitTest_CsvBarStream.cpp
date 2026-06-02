#include "Bte/Data/BarStream.h"

#include <gtest/gtest.h>

#include <filesystem>

namespace {

std::filesystem::path fixtureCsvDir() { return std::filesystem::path{BTE_TEST_FIXTURE_DIR} / "Data" / "Extracted"; }

bte::data::StreamRequest makeRequest(std::string symbol = "AAPL") {
    return bte::data::StreamRequest{
        .symbol = std::move(symbol),
        .schemaName = "ohlcv-1d",
        .csvDir = fixtureCsvDir(),
        .source = bte::data::StreamRequest::Source::csv,
    };
}

std::int64_t unixMillis(const bte::core::Bar& bar) { return bte::core::time::toUnixMillis(bar.ts); }

} // namespace

TEST(CsvBarStreamTest, CsvBarStream_next_returnsBarsInOrder) {
    auto stream = bte::data::CsvBarStream::open(makeRequest());
    ASSERT_TRUE(stream.ok()) << stream.error().message;

    const auto first = stream.value()->next();
    const auto second = stream.value()->next();
    const auto third = stream.value()->next();
    const auto end = stream.value()->next();

    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(second.has_value());
    ASSERT_TRUE(third.has_value());
    EXPECT_LT(unixMillis(*first), unixMillis(*second));
    EXPECT_LT(unixMillis(*second), unixMillis(*third));
    EXPECT_EQ(first->open, 100.0);
    EXPECT_EQ(second->open, 104.0);
    EXPECT_EQ(third->open, 102.0);
    EXPECT_FALSE(end.has_value());
}

TEST(CsvBarStreamTest, CsvBarStream_seek_movesToRequestedIndex) {
    auto stream = bte::data::CsvBarStream::open(makeRequest());
    ASSERT_TRUE(stream.ok()) << stream.error().message;

    ASSERT_TRUE(stream.value()->seek(2));
    EXPECT_EQ(stream.value()->consumed(), 2);

    const auto bar = stream.value()->next();

    ASSERT_TRUE(bar.has_value());
    EXPECT_EQ(bar->open, 102.0);
    EXPECT_EQ(stream.value()->consumed(), 3);
}

TEST(CsvBarStreamTest, CsvBarStream_at_returnsExpectedBarWithoutChangingConsumedPosition) {
    auto stream = bte::data::CsvBarStream::open(makeRequest());
    ASSERT_TRUE(stream.ok()) << stream.error().message;

    ASSERT_TRUE(stream.value()->next().has_value());
    EXPECT_EQ(stream.value()->consumed(), 1);

    const auto bar = stream.value()->at(2);

    ASSERT_TRUE(bar.has_value());
    EXPECT_EQ(bar->open, 102.0);
    EXPECT_EQ(stream.value()->consumed(), 1);
}

TEST(CsvBarStreamTest, CsvBarStream_totalBars_matchesFixtureRowCount) {
    auto stream = bte::data::CsvBarStream::open(makeRequest());
    ASSERT_TRUE(stream.ok()) << stream.error().message;

    EXPECT_EQ(stream.value()->totalBars(), 3);
    EXPECT_EQ(stream.value()->symbol(), "AAPL");
    EXPECT_EQ(stream.value()->schemaName(), "ohlcv-1d");
}

TEST(CsvBarStreamTest, CsvBarStream_range_filtersHalfOpenDateRange) {
    auto request = makeRequest();
    request.range = bte::core::DateRange{
        .start = bte::core::time::parseIso8601("2024-01-02 00:00:00+00:00").value(),
        .end = bte::core::time::parseIso8601("2024-01-03 00:00:00+00:00").value(),
    };

    auto stream = bte::data::CsvBarStream::open(request);
    ASSERT_TRUE(stream.ok()) << stream.error().message;

    EXPECT_EQ(stream.value()->totalBars(), 1);
    EXPECT_EQ(bte::core::time::toUnixMillis(stream.value()->range().start),
              bte::core::time::toUnixMillis(request.range.start));
    EXPECT_EQ(bte::core::time::toUnixMillis(stream.value()->range().end),
              bte::core::time::toUnixMillis(request.range.end));

    const auto bar = stream.value()->next();
    ASSERT_TRUE(bar.has_value());
    EXPECT_EQ(bar->open, 104.0);
    EXPECT_FALSE(stream.value()->next().has_value());
}

TEST(CsvBarStreamTest, CsvBarStream_invalidOhlc_returnsError) {
    auto stream = bte::data::CsvBarStream::open(makeRequest("BAD"));

    ASSERT_FALSE(stream.ok());
    EXPECT_EQ(stream.error().code, bte::core::ErrorCode::invalidArgument);
    EXPECT_NE(stream.error().message.find("invalid OHLCV"), std::string::npos);
}

TEST(CsvBarStreamTest, CsvBarStream_missingFile_returnsNotFound) {
    auto stream = bte::data::CsvBarStream::open(makeRequest("MISSING"));

    ASSERT_FALSE(stream.ok());
    EXPECT_EQ(stream.error().code, bte::core::ErrorCode::notFound);
    EXPECT_NE(stream.error().message.find("CSV file not found"), std::string::npos);
}
