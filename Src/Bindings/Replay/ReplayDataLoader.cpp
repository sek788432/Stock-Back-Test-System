#include "Bte/Bindings/ReplayDataLoader.h"

#include "Bte/Core/Time.h"
#include "Bte/Data/BarStream.h"

#include <QCoreApplication>
#include <QDate>
#include <QDebug>

#include <algorithm>
#include <chrono>
#include <compare> // IWYU pragma: keep
#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <string> // IWYU pragma: keep
#include <system_error>
#include <utility>
#include <vector>

namespace bte::bindings {
namespace {

constexpr auto hourlyTimeframe = "ohlcv-1h";
constexpr auto dailyTimeframe = "ohlcv-1d";

std::optional<std::filesystem::path>
findCsvDataDirFrom(std::filesystem::path cursor) {
  std::error_code errorCode;
  cursor = std::filesystem::absolute(cursor);

  while (!cursor.empty()) {
    auto candidate = cursor / "StockData" / "Extracted";
    if (std::filesystem::exists(candidate, errorCode) &&
        std::filesystem::is_directory(candidate, errorCode)) {
      return candidate;
    }
    const auto parent = cursor.parent_path();
    if (parent == cursor) {
      break;
    }
    cursor = parent;
  }

  return std::nullopt;
}

std::filesystem::path findCsvDataDir() {
  std::error_code errorCode;
  const auto currentDirectory = std::filesystem::current_path(errorCode);
  std::vector<std::filesystem::path> searchRoots;
  searchRoots.reserve(4);
  if (!errorCode) {
    searchRoots.push_back(currentDirectory);
  }
  searchRoots.emplace_back(
      QCoreApplication::applicationDirPath().toStdString());
#ifdef BTE_SOURCE_DIR
  searchRoots.emplace_back(BTE_SOURCE_DIR);
#endif
  searchRoots.emplace_back(__FILE__);
  auto dataDirectory = std::filesystem::path{"StockData"} / "Extracted";
  for (const auto &root : searchRoots) {
    if (auto directory = findCsvDataDirFrom(root)) {
      dataDirectory = *directory;
      break;
    }
  }
  return dataDirectory;
}

bte::core::Timestamp timestampFromDate(const QDate date) {
  using namespace std::chrono;
  return bte::core::Timestamp{
      sys_days{year{date.year()} / month{static_cast<unsigned>(date.month())} /
               day{static_cast<unsigned>(date.day())}}};
}

std::vector<bte::core::Bar>
aggregateDailyBars(const std::vector<bte::core::Bar> &bars) {
  if (bars.empty()) {
    return {};
  }

  using namespace std::chrono;
  std::vector<bte::core::Bar> daily;
  auto currentDay = floor<days>(bars.front().ts);
  bte::core::Bar current{
      .ts = bte::core::Timestamp{currentDay},
      .open = bars.front().open,
      .high = bars.front().high,
      .low = bars.front().low,
      .close = bars.front().close,
      .volume = bars.front().volume,
  };

  for (std::size_t index = 1; index < bars.size(); ++index) {
    const auto &bar = bars[index];
    const auto barDay = floor<days>(bar.ts);
    if (barDay != currentDay) {
      daily.push_back(current);
      currentDay = barDay;
      current = bte::core::Bar{
          .ts = bte::core::Timestamp{barDay},
          .open = bar.open,
          .high = bar.high,
          .low = bar.low,
          .close = bar.close,
          .volume = bar.volume,
      };
      continue;
    }

    current.high = std::max(current.high, bar.high);
    current.low = std::min(current.low, bar.low);
    current.close = bar.close;
    current.volume += bar.volume;
  }

  daily.push_back(current);
  return daily;
}

std::vector<bte::core::Bar> barsForSchema(std::vector<bte::core::Bar> &&bars,
                                          const std::string &requestedTimeframe,
                                          const std::string &sourceTimeframe) {
  if (requestedTimeframe == dailyTimeframe &&
      sourceTimeframe == hourlyTimeframe) {
    return aggregateDailyBars(bars);
  }
  return std::move(bars);
}

} // namespace

std::filesystem::path replayCsvDataDirectory() { return findCsvDataDir(); }

core::Result<std::vector<ReplayDataSource>>
availableReplaySources(const std::filesystem::path &csvDirectory) {
  auto sources = data::CsvBarStream::availableSources(csvDirectory);
  if (!sources.ok()) {
    return sources.error();
  }
  std::vector<ReplayDataSource> replaySources;
  replaySources.reserve(sources.value().size());
  for (const auto &source : sources.value()) {
    replaySources.push_back(
        {.symbol = source.symbol, .timeframe = source.source.timeframe});
  }
  return replaySources;
}

core::Result<std::vector<core::Bar>>
// Symbol and schema remain separate because they map directly to StreamRequest.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): distinct request fields
loadBacktestBars(const QString &symbol, const QString &schemaName,
                 const QDate start, const QDate end,
                 const core::CancellationToken &cancellation) {
  try {
    return loadBacktestBarsFrom(symbol, schemaName, start, end,
                                replayCsvDataDirectory(), cancellation);
  } catch (...) {
    return core::makeError(core::ErrorCode::internal,
                           "backtest data loading failed");
  }
}

core::Result<std::vector<core::Bar>>
loadBacktestBarsFrom(const QString &symbol, const QString &schemaName,
                     const QDate start, const QDate end,
                     const std::filesystem::path &csvDirectory,
                     const core::CancellationToken &cancellation) {
  try {
    const auto exclusiveEnd = end.addDays(1);
    if (!start.isValid() || !end.isValid() || start > end ||
        !exclusiveEnd.isValid()) {
      return core::makeError(core::ErrorCode::invalidArgument,
                             "backtest date range is invalid");
    }
    if (cancellation.isCancellationRequested()) {
      return core::makeError(core::ErrorCode::cancelled,
                             "backtest data loading was cancelled");
    }
    const auto requestedTimeframe = schemaName.toStdString();
    if (requestedTimeframe != hourlyTimeframe &&
        requestedTimeframe != dailyTimeframe) {
      return core::makeError(core::ErrorCode::schemaMismatch,
                             "Replay timeframe is unsupported: " +
                                 requestedTimeframe);
    }
    auto source =
        data::CsvBarStream::inspectSource(csvDirectory, symbol.toStdString());
    if (!source.ok()) {
      return source.error();
    }
    const auto &sourceTimeframe = source.value().timeframe;
    if (sourceTimeframe != hourlyTimeframe &&
        sourceTimeframe != dailyTimeframe) {
      return core::makeError(core::ErrorCode::schemaMismatch,
                             "Replay source timeframe is unsupported: " +
                                 sourceTimeframe);
    }
    if (sourceTimeframe == dailyTimeframe &&
        requestedTimeframe == hourlyTimeframe) {
      return core::makeError(core::ErrorCode::schemaMismatch,
                             "Daily Replay sources cannot provide hourly bars");
    }
    const bte::data::StreamRequest request{
        .symbol = symbol.toStdString(),
        .schemaName = sourceTimeframe,
        .range =
            bte::core::DateRange{
                .start = timestampFromDate(start),
                .end = timestampFromDate(exclusiveEnd),
            },
        .csvDir = csvDirectory,
    };

    auto stream = bte::data::CsvBarStream::open(request, cancellation);
    if (!stream.ok()) {
      return stream.error();
    }

    std::vector<bte::core::Bar> bars;
    while (auto bar = stream.value()->next()) {
      if (cancellation.isCancellationRequested()) {
        return core::makeError(core::ErrorCode::cancelled,
                               "backtest data loading was cancelled");
      }
      bars.push_back(*bar);
    }
    return barsForSchema(std::move(bars), requestedTimeframe, sourceTimeframe);
  } catch (...) {
    return core::makeError(core::ErrorCode::internal,
                           "backtest data loading failed");
  }
}

std::vector<bte::core::Bar> loadReplayBars(const QString &symbol,
                                           const QString &schemaName,
                                           const QDate start, const QDate end) {
  auto result = loadBacktestBars(symbol, schemaName, start, end);
  if (!result.ok()) {
    qWarning() << "Failed to load replay CSV:"
               << QString::fromStdString(result.error().message);
    return {};
  }
  return std::move(result).value();
}

} // namespace bte::bindings
