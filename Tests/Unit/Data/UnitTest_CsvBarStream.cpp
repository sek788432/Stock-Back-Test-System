#include "Bte/Data/BarStream.h"

#include "Bte/Core/Cancellation.h"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace {

std::filesystem::path fixtureCsvDir() {
  return std::filesystem::path{BTE_TEST_FIXTURE_DIR} / "Data" / "Extracted";
}

bte::data::StreamRequest makeRequest(std::string symbol = "AAPL") {
  return bte::data::StreamRequest{
      .symbol = std::move(symbol),
      .schemaName = "ohlcv-1d",
      .csvDir = fixtureCsvDir(),
  };
}

class ScopedCsvFixture final {
public:
  ScopedCsvFixture(std::string symbol, const std::string_view contents)
      : symbol_(std::move(symbol)),
        directory_(std::filesystem::temp_directory_path() /
                   "bte-csv-bar-stream-tests"),
        path_(directory_ / (symbol_ + ".csv")) {
    std::filesystem::create_directories(directory_);
    std::ofstream output{path_};
    output << contents;
    if (!output) {
      throw std::runtime_error{"Unable to write temporary CSV fixture"};
    }
  }

  ~ScopedCsvFixture() { std::filesystem::remove(path_); }

  ScopedCsvFixture(const ScopedCsvFixture &) = delete;
  ScopedCsvFixture &operator=(const ScopedCsvFixture &) = delete;
  ScopedCsvFixture(ScopedCsvFixture &&) = delete;
  ScopedCsvFixture &operator=(ScopedCsvFixture &&) = delete;

  [[nodiscard]] bte::data::StreamRequest request() const {
    auto result = makeRequest(symbol_);
    result.csvDir = directory_;
    return result;
  }

private:
  std::string symbol_;
  std::filesystem::path directory_;
  std::filesystem::path path_;
};

std::int64_t unixMillis(const bte::core::Bar &bar) {
  return bte::core::time::toUnixMillis(bar.ts);
}

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

TEST(CsvBarStreamTest, CsvBarStream_reset_rewindsSequentialConsumption) {
  auto stream = bte::data::CsvBarStream::open(makeRequest());
  ASSERT_TRUE(stream.ok()) << stream.error().message;
  ASSERT_TRUE(stream.value()->next().has_value());
  ASSERT_TRUE(stream.value()->next().has_value());
  EXPECT_EQ(stream.value()->consumed(), 2);

  stream.value()->reset();

  EXPECT_EQ(stream.value()->consumed(), 0);
  const auto first = stream.value()->next();
  ASSERT_TRUE(first.has_value());
  EXPECT_EQ(first->open, 100.0);
}

TEST(CsvBarStreamTest, CsvBarStream_totalBars_matchesFixtureRowCount) {
  auto stream = bte::data::CsvBarStream::open(makeRequest());
  ASSERT_TRUE(stream.ok()) << stream.error().message;

  EXPECT_EQ(stream.value()->totalBars(), 3);
}

TEST(CsvBarStreamTest, CsvBarStream_range_filtersHalfOpenDateRange) {
  auto request = makeRequest();
  request.range = bte::core::DateRange{
      .start =
          bte::core::time::parseIso8601("2024-01-02 00:00:00+00:00").value(),
      .end = bte::core::time::parseIso8601("2024-01-03 00:00:00+00:00").value(),
  };

  auto stream = bte::data::CsvBarStream::open(request);
  ASSERT_TRUE(stream.ok()) << stream.error().message;

  EXPECT_EQ(stream.value()->totalBars(), 1);
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

TEST(CsvBarStreamTest, CsvBarStream_numericValueWithTrailingText_returnsError) {
  auto stream = bte::data::CsvBarStream::open(makeRequest("BADNUMERIC"));

  ASSERT_FALSE(stream.ok());
  EXPECT_EQ(stream.error().code, bte::core::ErrorCode::invalidArgument);
  EXPECT_NE(stream.error().message.find("not numeric"), std::string::npos);
}

TEST(CsvBarStreamTest, CsvBarStream_missingFile_returnsNotFound) {
  auto stream = bte::data::CsvBarStream::open(makeRequest("MISSING"));

  ASSERT_FALSE(stream.ok());
  EXPECT_EQ(stream.error().code, bte::core::ErrorCode::notFound);
  EXPECT_NE(stream.error().message.find("CSV file not found"),
            std::string::npos);
}

TEST(CsvBarStreamTest, CsvBarStream_requestedCancellationStopsBeforeReading) {
  bte::core::CancellationSource cancellation;
  cancellation.requestCancellation();

  const auto stream =
      bte::data::CsvBarStream::open(makeRequest(), cancellation.token());

  ASSERT_FALSE(stream.ok());
  EXPECT_EQ(stream.error().code, bte::core::ErrorCode::cancelled);
}

TEST(CsvBarStreamTest, CsvBarStream_rejectsEmptySymbolSchemaAndFile) {
  auto request = makeRequest();
  request.symbol.clear();
  auto stream = bte::data::CsvBarStream::open(request);
  ASSERT_FALSE(stream.ok());
  EXPECT_EQ(stream.error().code, bte::core::ErrorCode::invalidArgument);
  EXPECT_NE(stream.error().message.find("symbol is required"),
            std::string::npos);

  request = makeRequest();
  request.schemaName.clear();
  stream = bte::data::CsvBarStream::open(request);
  ASSERT_FALSE(stream.ok());
  EXPECT_EQ(stream.error().code, bte::core::ErrorCode::invalidArgument);
  EXPECT_NE(stream.error().message.find("schemaName is required"),
            std::string::npos);

  const ScopedCsvFixture empty{"EMPTY", ""};
  stream = bte::data::CsvBarStream::open(empty.request());
  ASSERT_FALSE(stream.ok());
  EXPECT_EQ(stream.error().code, bte::core::ErrorCode::schemaMismatch);
  EXPECT_NE(stream.error().message.find("CSV file is empty"),
            std::string::npos);
}

TEST(CsvBarStreamTest, CsvBarStream_reportsEveryMissingRequiredHeader) {
  constexpr std::array requiredColumns{"symbol", "ts",    "open",  "high",
                                       "low",    "close", "volume"};
  constexpr std::string_view fullHeader =
      "symbol,ts,open,high,low,close,volume";

  for (const auto *missing : requiredColumns) {
    SCOPED_TRACE(missing);
    std::string header;
    for (const auto *column : requiredColumns) {
      if (std::string_view{column} == missing) {
        continue;
      }
      if (!header.empty()) {
        header += ',';
      }
      header += column;
    }
    ASSERT_NE(header, fullHeader);
    const ScopedCsvFixture fixture{std::string{"HEADER_"} + missing,
                                   header + '\n'};

    const auto stream = bte::data::CsvBarStream::open(fixture.request());

    ASSERT_FALSE(stream.ok());
    EXPECT_EQ(stream.error().code, bte::core::ErrorCode::schemaMismatch);
    EXPECT_NE(stream.error().message.find(
                  std::string{"missing required column '"} + missing + "'"),
              std::string::npos);
  }
}

TEST(CsvBarStreamTest, CsvBarStream_rejectsMismatchedIdentityAndTimestampRows) {
  const std::array cases{
      std::pair{
          "OTHER_SYMBOL",
          "symbol,ts,open,high,low,close,volume,schemaName\n"
          "OTHER,2024-01-01 00:00:00+00:00,100,105,99,104,1000,ohlcv-1d\n"},
      std::pair{"OTHER_SCHEMA",
                "symbol,ts,open,high,low,close,volume,schemaName\n"
                "OTHER_SCHEMA,2024-01-01 "
                "00:00:00+00:00,100,105,99,104,1000,ohlcv-1h\n"},
      std::pair{"MISSING_TS",
                "symbol,ts,open,high,low,close,volume\nMISSING_TS\n"},
      std::pair{"INVALID_TS",
                "symbol,ts,open,high,low,close,volume\n"
                "INVALID_TS,not-a-timestamp,100,105,99,104,1000\n"},
      std::pair{"MISSING_OPEN", "symbol,ts,open,high,low,close,volume\n"
                                "MISSING_OPEN,2024-01-01 00:00:00+00:00\n"},
  };

  for (const auto &[symbol, contents] : cases) {
    SCOPED_TRACE(symbol);
    const ScopedCsvFixture fixture{symbol, contents};
    const auto stream = bte::data::CsvBarStream::open(fixture.request());

    ASSERT_FALSE(stream.ok());
    const auto expectsSchemaError = std::string_view{symbol} == "MISSING_TS" ||
                                    std::string_view{symbol} == "MISSING_OPEN";
    EXPECT_EQ(stream.error().code, expectsSchemaError
                                       ? bte::core::ErrorCode::schemaMismatch
                                       : bte::core::ErrorCode::invalidArgument);
  }
}

TEST(CsvBarStreamTest, CsvBarStream_reportsEachInvalidNumericColumn) {
  constexpr std::array columns{"open", "high", "low", "close", "volume"};

  for (std::size_t invalidIndex = 0; invalidIndex < columns.size();
       ++invalidIndex) {
    SCOPED_TRACE(columns[invalidIndex]);
    std::array<std::string, 5> values{"100", "105", "99", "104", "1000"};
    values[invalidIndex] = "invalid";
    const auto symbol = std::string{"BAD_"} + columns[invalidIndex];
    const auto row = symbol + ",2024-01-01 00:00:00+00:00," + values[0] + ',' +
                     values[1] + ',' + values[2] + ',' + values[3] + ',' +
                     values[4] + '\n';
    const ScopedCsvFixture fixture{
        symbol, "symbol,ts,open,high,low,close,volume\n" + row};

    const auto stream = bte::data::CsvBarStream::open(fixture.request());

    ASSERT_FALSE(stream.ok());
    EXPECT_EQ(stream.error().code, bte::core::ErrorCode::invalidArgument);
    EXPECT_NE(stream.error().message.find(std::string{"column '"} +
                                          columns[invalidIndex] +
                                          "' is not numeric"),
              std::string::npos);
  }
}

TEST(CsvBarStreamTest,
     CsvBarStream_acceptsOptionalSchemaWhitespaceBlankRowsAndTrailingColumn) {
  const ScopedCsvFixture fixture{
      "FLEXIBLE",
      " symbol , ts , open , high , low , close , volume ,\n"
      "\n"
      " FLEXIBLE , 2024-01-02 00:00:00+00:00 , 104 , 106 , 103 , 105 , 2000 ,\n"
      " FLEXIBLE , 2024-01-01 00:00:00+00:00 , 100 , 105 , 99 , 104 , 1000 "
      ",\n"};

  const auto stream = bte::data::CsvBarStream::open(fixture.request());

  ASSERT_TRUE(stream.ok()) << stream.error().message;
  EXPECT_EQ(stream.value()->totalBars(), 2);
  EXPECT_EQ(stream.value()->next()->open, 100.0);
  EXPECT_EQ(stream.value()->next()->open, 104.0);
}

TEST(CsvBarStreamTest, CsvBarStream_emptySelectionHasNoNextBar) {
  auto request = makeRequest();
  request.range = {
      .start =
          bte::core::time::parseIso8601("2025-01-01 00:00:00+00:00").value(),
      .end = bte::core::time::parseIso8601("2025-01-02 00:00:00+00:00").value(),
  };
  auto stream = bte::data::CsvBarStream::open(request);
  ASSERT_TRUE(stream.ok()) << stream.error().message;

  EXPECT_EQ(stream.value()->totalBars(), 0);
  EXPECT_FALSE(stream.value()->next().has_value());
}
