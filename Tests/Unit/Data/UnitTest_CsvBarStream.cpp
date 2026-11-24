#include "Bte/Data/BarStream.h"

#include "../Support/ScopedTemporaryDirectory.h"
#include "Bte/Core/Cancellation.h"
#include "Bte/Core/Digest.h"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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
  ScopedCsvFixture(std::string symbol, const std::string_view contents,
                   std::string filenameStem = {})
      : symbol_(std::move(symbol)), directory_(makeUniqueDirectory()),
        filename_(filenameStem.empty() ? symbol_ + ".csv"
                                       : std::move(filenameStem) + ".csv"),
        path_(directory_ / filename_) {
    std::filesystem::create_directories(directory_);
    std::ofstream output{path_};
    output << contents;
    if (!output) {
      throw std::runtime_error{"Unable to write temporary CSV fixture"};
    }
  }

  ~ScopedCsvFixture() { std::filesystem::remove_all(directory_); }

  ScopedCsvFixture(const ScopedCsvFixture &) = delete;
  ScopedCsvFixture &operator=(const ScopedCsvFixture &) = delete;
  ScopedCsvFixture(ScopedCsvFixture &&) = delete;
  ScopedCsvFixture &operator=(ScopedCsvFixture &&) = delete;

  [[nodiscard]] bte::data::StreamRequest request() const {
    auto result = makeRequest(symbol_);
    result.csvDir = directory_;
    return result;
  }

  void writeManifest(const std::string_view timeframe,
                     const std::string_view digest = {}) const {
    const auto contentDigest = digest.empty()
                                   ? bte::core::sha256(readContents())
                                   : std::string{digest};
    std::ofstream output{directory_ / "SourceManifest.btesource"};
    output << "BTE-SOURCE-MANIFEST-V1\nfile=" << filename_ << '|' << timeframe
           << '|' << contentDigest << '\n';
  }

private:
  static std::filesystem::path makeUniqueDirectory() {
    static std::atomic<std::uint64_t> nextId{0};
    std::random_device random;
    return std::filesystem::temp_directory_path() /
           ("bte-csv-bar-stream-tests-" + std::to_string(random()) + "-" +
            std::to_string(nextId.fetch_add(1)));
  }

  [[nodiscard]] std::string readContents() const {
    std::ifstream input{path_, std::ios::binary};
    return {std::istreambuf_iterator<char>{input},
            std::istreambuf_iterator<char>{}};
  }

  std::string symbol_;
  std::filesystem::path directory_;
  std::string filename_;
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
  ASSERT_EQ(stream.value()->totalBars(), 2U);
  const auto first = stream.value()->next();
  ASSERT_TRUE(first.has_value());
  EXPECT_EQ(first->open, 100.0);
  const auto second = stream.value()->next();
  ASSERT_TRUE(second.has_value());
  EXPECT_EQ(second->open, 104.0);
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

TEST(CsvBarStreamTest,
     CsvBarStream_resolvesAndVerifiesDottedSymbolSourceDeclaration) {
  constexpr std::string_view contents =
      "symbol,ts,open,high,low,close,volume\n"
      "BRK.B,2024-01-01 00:00:00+00:00,100,105,99,104,1000\n";
  const ScopedCsvFixture fixture{"BRK.B", contents, "BRK_B"};
  fixture.writeManifest("ohlcv-1h");
  auto request = fixture.request();
  request.schemaName = "ohlcv-1h";

  const auto stream = bte::data::CsvBarStream::open(request);

  ASSERT_TRUE(stream.ok()) << stream.error().message;
  ASSERT_EQ(stream.value()->totalBars(), 1U);
  const auto next = stream.value()->next();
  ASSERT_TRUE(next.has_value());
  EXPECT_DOUBLE_EQ(next->close, 104.0);
}

TEST(CsvBarStreamTest,
     CsvBarStream_rejectsSourceTimeframeDifferentFromDeclaredTimeframe) {
  constexpr std::string_view contents =
      "symbol,ts,open,high,low,close,volume\n"
      "AAPL,2024-01-01 00:00:00+00:00,100,105,99,104,1000\n";
  const ScopedCsvFixture fixture{"AAPL", contents};
  fixture.writeManifest("ohlcv-1d");
  auto request = fixture.request();
  request.schemaName = "ohlcv-1h";

  const auto stream = bte::data::CsvBarStream::open(request);

  ASSERT_FALSE(stream.ok());
  EXPECT_EQ(stream.error().code, bte::core::ErrorCode::schemaMismatch);
}

TEST(CsvBarStreamTest, CsvBarStream_rejectsSourceManifestHashMismatch) {
  constexpr std::string_view contents =
      "symbol,ts,open,high,low,close,volume\n"
      "AAPL,2024-01-01 00:00:00+00:00,100,105,99,104,1000\n";
  const ScopedCsvFixture fixture{"AAPL", contents};
  fixture.writeManifest("ohlcv-1h", std::string(64, '0'));
  auto request = fixture.request();
  request.schemaName = "ohlcv-1h";

  const auto stream = bte::data::CsvBarStream::open(request);

  ASSERT_FALSE(stream.ok());
  EXPECT_EQ(stream.error().code, bte::core::ErrorCode::dataSnapshotUnavailable);
  EXPECT_NE(stream.error().message.find("SHA-256"), std::string::npos);
}

TEST(CsvBarStreamTest, CsvBarStream_rejectsUnsafeSourceSymbol) {
  bte::test_support::ScopedTemporaryDirectory temporary{"csv-path-traversal"};
  const auto csvDirectory = temporary.path() / "Extracted";
  std::filesystem::create_directories(csvDirectory);
  std::ofstream outside{temporary.path() / "outside.csv"};
  outside << "symbol,ts,open,high,low,close,volume\n"
             "../outside,2024-01-01 00:00:00+00:00,100,105,99,104,1000\n";
  outside.close();

  const auto source =
      bte::data::CsvBarStream::inspectSource(csvDirectory, "../outside");

  ASSERT_FALSE(source.ok());
  EXPECT_EQ(source.error().code, bte::core::ErrorCode::invalidArgument);
}

TEST(CsvBarStreamTest, CsvBarStream_acceptsSourceWithoutTrailingNewline) {
  constexpr std::string_view contents =
      "symbol,ts,open,high,low,close,volume,schemaName\n"
      "AAPL,2024-01-01 00:00:00+00:00,100,105,99,104,1000,ohlcv-1d";
  const ScopedCsvFixture fixture{"AAPL", contents};

  const auto stream = bte::data::CsvBarStream::open(fixture.request());

  ASSERT_TRUE(stream.ok()) << stream.error().message;
  ASSERT_EQ(stream.value()->totalBars(), 1U);
  const auto next = stream.value()->next();
  ASSERT_TRUE(next.has_value());
  EXPECT_EQ(next->close, 104.0);
}

TEST(CsvBarStreamTest, inspectSourceRejectsDottedSymbolUndeclaredByManifest) {
  constexpr std::string_view contents =
      "symbol,ts,open,high,low,close,volume,schemaName\n"
      "AAPL,2024-01-01 00:00:00+00:00,100,105,99,104,1000,ohlcv-1h\n";
  const ScopedCsvFixture fixture{"AAPL", contents};
  fixture.writeManifest("ohlcv-1h");

  const auto source =
      bte::data::CsvBarStream::inspectSource(fixture.request().csvDir, "BRK.B");

  ASSERT_FALSE(source.ok());
  EXPECT_EQ(source.error().code, bte::core::ErrorCode::notFound);
}

TEST(CsvBarStreamTest, inspectAndListRejectMalformedSourceManifest) {
  constexpr std::string_view contents =
      "symbol,ts,open,high,low,close,volume,schemaName\n"
      "AAPL,2024-01-01 00:00:00+00:00,100,105,99,104,1000,ohlcv-1h\n";
  const ScopedCsvFixture fixture{"AAPL", contents};
  std::ofstream manifest{fixture.request().csvDir / "SourceManifest.btesource"};
  manifest << "invalid manifest header\n";
  manifest.close();

  const auto source =
      bte::data::CsvBarStream::inspectSource(fixture.request().csvDir, "AAPL");
  const auto sources =
      bte::data::CsvBarStream::availableSources(fixture.request().csvDir);

  ASSERT_FALSE(source.ok());
  EXPECT_EQ(source.error().code, bte::core::ErrorCode::schemaMismatch);
  ASSERT_FALSE(sources.ok());
  EXPECT_EQ(sources.error().code, bte::core::ErrorCode::schemaMismatch);
}

TEST(CsvBarStreamTest, inspectSourceRejectsAnEmptySymbol) {
  bte::test_support::ScopedTemporaryDirectory temporary{"csv-empty-symbol"};

  const auto source =
      bte::data::CsvBarStream::inspectSource(temporary.path(), "");

  ASSERT_FALSE(source.ok());
  EXPECT_EQ(source.error().code, bte::core::ErrorCode::invalidArgument);
}

TEST(CsvBarStreamTest, inspectSourceReportsManifestDirectoryLookupFailure) {
  bte::test_support::ScopedTemporaryDirectory temporary{
      "csv-manifest-directory-lookup-failure"};
  const auto sourceDirectory = temporary.path() / "not-a-directory";
  std::error_code linkError;
  std::filesystem::create_symlink(sourceDirectory.filename(), sourceDirectory,
                                  linkError);
  ASSERT_FALSE(linkError) << linkError.message();

  const auto source =
      bte::data::CsvBarStream::inspectSource(sourceDirectory, "AAPL");

  ASSERT_FALSE(source.ok());
  EXPECT_EQ(source.error().code, bte::core::ErrorCode::schemaMismatch);
  EXPECT_NE(source.error().message.find("Unable to inspect source manifest"),
            std::string::npos);
}

TEST(CsvBarStreamTest, inspectAndListRejectSourceManifestWithEmptyFilename) {
  constexpr std::string_view contents =
      "symbol,ts,open,high,low,close,volume,schemaName\n"
      "AAPL,2024-01-01 00:00:00+00:00,100,105,99,104,1000,ohlcv-1h\n";
  const ScopedCsvFixture fixture{"AAPL", contents};
  std::ofstream manifest{fixture.request().csvDir / "SourceManifest.btesource"};
  manifest << "BTE-SOURCE-MANIFEST-V1\nfile=|ohlcv-1h|" << std::string(64, 'a')
           << '\n';
  manifest.close();

  const auto source =
      bte::data::CsvBarStream::inspectSource(fixture.request().csvDir, "AAPL");
  const auto sources =
      bte::data::CsvBarStream::availableSources(fixture.request().csvDir);

  ASSERT_FALSE(source.ok());
  EXPECT_EQ(source.error().code, bte::core::ErrorCode::schemaMismatch);
  ASSERT_FALSE(sources.ok());
  EXPECT_EQ(sources.error().code, bte::core::ErrorCode::schemaMismatch);
}

TEST(CsvBarStreamTest, inspectSourceSupportsLegacyHeaderWithoutSchemaName) {
  constexpr std::string_view contents =
      "symbol,ts,open,high,low,close,volume\n"
      "AAPL,2024-01-01 00:00:00+00:00,100,105,99,104,1000\n";
  const ScopedCsvFixture fixture{"AAPL", contents};

  const auto source =
      bte::data::CsvBarStream::inspectSource(fixture.request().csvDir, "AAPL");

  ASSERT_TRUE(source.ok()) << source.error().message;
  EXPECT_TRUE(source.value().timeframe.empty());
}

TEST(CsvBarStreamTest,
     inspectSourceReturnsUnknownTimeframeForSchemaHeaderWithoutRows) {
  constexpr std::string_view contents =
      "symbol,ts,open,high,low,close,volume,schemaName\n";
  const ScopedCsvFixture fixture{"AAPL", contents};

  const auto source =
      bte::data::CsvBarStream::inspectSource(fixture.request().csvDir, "AAPL");

  ASSERT_TRUE(source.ok()) << source.error().message;
  EXPECT_TRUE(source.value().timeframe.empty());
}

TEST(CsvBarStreamTest, CsvBarStream_opensAValidManifestBoundSource) {
  constexpr std::string_view contents =
      "symbol,ts,open,high,low,close,volume,schemaName\n"
      "AAPL,2024-01-01 00:00:00+00:00,100,105,99,104,1000,ohlcv-1h\n";
  const ScopedCsvFixture fixture{"AAPL", contents};
  fixture.writeManifest("ohlcv-1h");
  auto request = fixture.request();
  request.schemaName = "ohlcv-1h";

  const auto stream = bte::data::CsvBarStream::open(request);

  ASSERT_TRUE(stream.ok()) << stream.error().message;
  ASSERT_EQ(stream.value()->totalBars(), 1U);
  const auto next = stream.value()->next();
  ASSERT_TRUE(next.has_value());
  EXPECT_EQ(next->close, 104.0);
}

TEST(CsvBarStreamTest,
     availableSourcesRequiresSymbolsWhenSourceManifestIsPresent) {
  bte::test_support::ScopedTemporaryDirectory temporary{
      "csv-manifest-requires-symbols"};
  const auto csvDirectory = temporary.path() / "Extracted";
  std::filesystem::create_directories(csvDirectory);
  std::ofstream manifest{csvDirectory / "SourceManifest.btesource"};
  manifest << "BTE-SOURCE-MANIFEST-V1\n";
  manifest.close();

  const auto sources = bte::data::CsvBarStream::availableSources(csvDirectory);

  ASSERT_FALSE(sources.ok());
  EXPECT_EQ(sources.error().code, bte::core::ErrorCode::schemaMismatch);
  EXPECT_NE(sources.error().message.find("symbol catalog is missing"),
            std::string::npos);
}

TEST(CsvBarStreamTest,
     availableSourcesReadsSymbolCatalogAndSkipsBlankAndMissingSymbols) {
  bte::test_support::ScopedTemporaryDirectory temporary{
      "csv-symbol-catalog-scan"};
  const auto csvDirectory = temporary.path() / "Extracted";
  std::filesystem::create_directories(csvDirectory);
  std::ofstream csv{csvDirectory / "AAPL.csv"};
  csv << "symbol,ts,open,high,low,close,volume,schemaName\n\n"
         "AAPL,2024-01-01 00:00:00+00:00,100,105,99,104,1000";
  csv.close();
  std::ofstream symbols{temporary.path() / "Symbols.txt"};
  symbols << "\nAAPL\n";
  symbols.close();

  const auto sources = bte::data::CsvBarStream::availableSources(csvDirectory);

  ASSERT_TRUE(sources.ok()) << sources.error().message;
  ASSERT_EQ(sources.value().size(), 1U);
  EXPECT_EQ(sources.value().front().symbol, "AAPL");
  EXPECT_TRUE(sources.value().front().source.timeframe.empty());

  auto request = makeRequest("AAPL");
  request.csvDir = csvDirectory;
  const auto stream = bte::data::CsvBarStream::open(request);
  ASSERT_TRUE(stream.ok()) << stream.error().message;
  EXPECT_EQ(stream.value()->totalBars(), 1U);
}

TEST(CsvBarStreamTest, availableSourcesSkipsSymbolsUndeclaredByManifest) {
  bte::test_support::ScopedTemporaryDirectory temporary{
      "csv-manifest-symbol-catalog"};
  const auto csvDirectory = temporary.path() / "Extracted";
  std::filesystem::create_directories(csvDirectory);
  constexpr std::string_view csvContents =
      "symbol,ts,open,high,low,close,volume,schemaName\n"
      "AAPL,2024-01-01 00:00:00+00:00,100,105,99,104,1000,ohlcv-1h\n";
  std::ofstream csv{csvDirectory / "AAPL.csv"};
  csv << csvContents;
  csv.close();
  std::ofstream manifest{csvDirectory / "SourceManifest.btesource"};
  manifest << "BTE-SOURCE-MANIFEST-V1\nfile=AAPL.csv|ohlcv-1h|"
           << bte::core::sha256(csvContents) << '\n';
  manifest.close();
  std::ofstream symbols{temporary.path() / "Symbols.txt"};
  symbols << "MISSING\nAAPL\n";
  symbols.close();

  const auto sources = bte::data::CsvBarStream::availableSources(csvDirectory);

  ASSERT_TRUE(sources.ok()) << sources.error().message;
  ASSERT_EQ(sources.value().size(), 1U);
  EXPECT_EQ(sources.value().front().symbol, "AAPL");
}

TEST(CsvBarStreamTest, availableSourcesReportsSymbolCatalogSymlinkLoop) {
  bte::test_support::ScopedTemporaryDirectory temporary{
      "csv-symbol-catalog-symlink-loop"};
  const auto csvDirectory = temporary.path() / "Extracted";
  std::filesystem::create_directories(csvDirectory);
  const auto cyclicSource = csvDirectory / "BROKEN.csv";
  std::filesystem::create_symlink(cyclicSource.filename(), cyclicSource);
  std::ofstream symbols{temporary.path() / "Symbols.txt"};
  symbols << "BROKEN\n";
  symbols.close();

  const auto sources = bte::data::CsvBarStream::availableSources(csvDirectory);

  ASSERT_FALSE(sources.ok());
  EXPECT_EQ(sources.error().code, bte::core::ErrorCode::schemaMismatch);
  EXPECT_NE(sources.error().message.find("Unable to inspect CSV source"),
            std::string::npos);
}

TEST(CsvBarStreamTest, availableSourcesPropagatesSymbolCatalogErrors) {
  bte::test_support::ScopedTemporaryDirectory temporary{"csv-symbol-errors"};
  const auto csvDirectory = temporary.path() / "Extracted";
  std::filesystem::create_directories(csvDirectory);
  std::ofstream malformed{csvDirectory / "BROKEN.csv"};
  malformed << "not,a,valid,csv,header\n";
  malformed.close();
  std::ofstream symbols{temporary.path() / "Symbols.txt"};
  symbols << "BROKEN\n";
  symbols.close();

  const auto sources = bte::data::CsvBarStream::availableSources(csvDirectory);

  ASSERT_FALSE(sources.ok());
  EXPECT_EQ(sources.error().code, bte::core::ErrorCode::schemaMismatch);
}

TEST(CsvBarStreamTest, availableSourcesRejectsUnsafeSymbolCatalogEntries) {
  bte::test_support::ScopedTemporaryDirectory temporary{"csv-unsafe-symbol"};
  const auto csvDirectory = temporary.path() / "Extracted";
  std::filesystem::create_directories(csvDirectory);
  std::ofstream symbols{temporary.path() / "Symbols.txt"};
  symbols << "../outside\n";
  symbols.close();

  const auto sources = bte::data::CsvBarStream::availableSources(csvDirectory);

  ASSERT_FALSE(sources.ok());
  EXPECT_EQ(sources.error().code, bte::core::ErrorCode::invalidArgument);
}

TEST(CsvBarStreamTest, availableSourcesAndSymbolsScanCsvFilesOnly) {
  constexpr std::string_view contents =
      "symbol,ts,open,high,low,close,volume,schemaName\n"
      "AAPL,2024-01-01 00:00:00+00:00,100,105,99,104,1000,ohlcv-1h\n";
  const ScopedCsvFixture fixture{"AAPL", contents};
  std::ofstream note{fixture.request().csvDir / "readme.txt"};
  note << "not a market data file";
  note.close();
  std::filesystem::create_directory(fixture.request().csvDir / "nested");

  const auto sources =
      bte::data::CsvBarStream::availableSources(fixture.request().csvDir);
  ASSERT_TRUE(sources.ok()) << sources.error().message;
  ASSERT_EQ(sources.value().size(), 1U);
  EXPECT_EQ(sources.value().front().symbol, "AAPL");

  const auto symbols =
      bte::data::CsvBarStream::availableSymbols(fixture.request().csvDir);
  ASSERT_TRUE(symbols.ok()) << symbols.error().message;
  EXPECT_EQ(symbols.value(), std::vector<std::string>{"AAPL"});
}

TEST(CsvBarStreamTest, availableSourcesReportsAFileThatCannotBeInspected) {
  constexpr std::string_view contents =
      "symbol,ts,open,high,low,close,volume,schemaName\n"
      "AAPL,2024-01-01 00:00:00+00:00,100,105,99,104,1000,ohlcv-1h\n";
  const ScopedCsvFixture fixture{"AAPL", contents};
  std::error_code linkError;
  std::filesystem::create_symlink(
      "Loop.csv", fixture.request().csvDir / "Loop.csv", linkError);
  ASSERT_FALSE(linkError) << linkError.message();

  const auto sources =
      bte::data::CsvBarStream::availableSources(fixture.request().csvDir);

  ASSERT_FALSE(sources.ok());
  EXPECT_EQ(sources.error().code, bte::core::ErrorCode::schemaMismatch);
}

TEST(CsvBarStreamTest,
     availableSourcesSkipsSymbolCatalogEntriesThatAreNotFiles) {
  bte::test_support::ScopedTemporaryDirectory temporary{
      "csv-symbol-catalog-non-file"};
  const auto csvDirectory = temporary.path() / "Extracted";
  std::filesystem::create_directories(csvDirectory / "AAPL.csv");
  std::ofstream symbols{temporary.path() / "Symbols.txt"};
  symbols << "AAPL\n";
  symbols.close();

  const auto sources = bte::data::CsvBarStream::availableSources(csvDirectory);

  ASSERT_TRUE(sources.ok()) << sources.error().message;
  EXPECT_TRUE(sources.value().empty());
}

TEST(CsvBarStreamTest, availableSourcesReportsMalformedCsvAndMissingDirectory) {
  constexpr std::string_view contents =
      "symbol,ts,open,high,low,close,volume,schemaName\n"
      "AAPL,2024-01-01 00:00:00+00:00,100,105,99,104,1000,ohlcv-1h\n";
  const ScopedCsvFixture fixture{"AAPL", contents};
  std::ofstream malformed{fixture.request().csvDir / "BROKEN.csv"};
  malformed << "not,a,valid,market,data,header\n";
  malformed.close();

  const auto malformedSources =
      bte::data::CsvBarStream::availableSources(fixture.request().csvDir);
  ASSERT_FALSE(malformedSources.ok());
  EXPECT_EQ(malformedSources.error().code,
            bte::core::ErrorCode::schemaMismatch);

  bte::test_support::ScopedTemporaryDirectory temporary{"csv-missing-catalog"};
  const auto missingDirectory = temporary.path() / "missing";
  const auto missingSources =
      bte::data::CsvBarStream::availableSources(missingDirectory);
  ASSERT_FALSE(missingSources.ok());
  EXPECT_EQ(missingSources.error().code, bte::core::ErrorCode::schemaMismatch);
  const auto missingSymbols =
      bte::data::CsvBarStream::availableSymbols(missingDirectory);
  ASSERT_FALSE(missingSymbols.ok());
  EXPECT_EQ(missingSymbols.error().code, bte::core::ErrorCode::schemaMismatch);
}

TEST(CsvBarStreamTest, inspectSourceRejectsSymbolsMissingFromManifest) {
  constexpr std::string_view contents =
      "symbol,ts,open,high,low,close,volume,schemaName\n"
      "AAPL,2024-01-01 00:00:00+00:00,100,105,99,104,1000,ohlcv-1h\n";
  const ScopedCsvFixture fixture{"AAPL", contents};
  fixture.writeManifest("ohlcv-1h");

  const auto source =
      bte::data::CsvBarStream::inspectSource(fixture.request().csvDir, "MSFT");

  ASSERT_FALSE(source.ok());
  EXPECT_EQ(source.error().code, bte::core::ErrorCode::notFound);
}
