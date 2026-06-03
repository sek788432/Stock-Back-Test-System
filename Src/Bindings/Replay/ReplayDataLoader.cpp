#include "Bte/Bindings/ReplayDataLoader.h"

#include "Bte/Data/BarStream.h"

#include <QCoreApplication>
#include <QDebug>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <optional>
#include <utility>

namespace bte::bindings {
namespace {

std::optional<std::filesystem::path>
findCsvDataDirFrom(std::filesystem::path cursor) {
  std::error_code ec;
  cursor = std::filesystem::absolute(std::move(cursor), ec);
  if (ec) {
    return std::nullopt;
  }

  while (!cursor.empty()) {
    const auto candidate = cursor / "StockData" / "Extracted";
    if (std::filesystem::exists(candidate, ec) &&
        std::filesystem::is_directory(candidate, ec)) {
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
  if (auto dir = findCsvDataDirFrom(std::filesystem::current_path())) {
    return *dir;
  }
  if (auto dir = findCsvDataDirFrom(
          QCoreApplication::applicationDirPath().toStdString())) {
    return *dir;
  }
#ifdef BTE_SOURCE_DIR
  if (auto dir = findCsvDataDirFrom(BTE_SOURCE_DIR)) {
    return *dir;
  }
#endif
  if (auto dir = findCsvDataDirFrom(__FILE__)) {
    return *dir;
  }
  return std::filesystem::path{"StockData"} / "Extracted";
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

std::vector<bte::core::Bar> barsForSchema(std::vector<bte::core::Bar> bars,
                                          const QString &schemaName) {
  if (schemaName == "ohlcv-1d") {
    return aggregateDailyBars(bars);
  }
  return bars;
}

} // namespace

std::vector<bte::core::Bar> loadReplayBars(const QString &symbol,
                                           const QString &schemaName,
                                           const QDate start, const QDate end) {
  const bte::data::StreamRequest request{
      .symbol = symbol.toStdString(),
      .schemaName = "ohlcv-1h",
      .range =
          bte::core::DateRange{
              .start = timestampFromDate(start),
              .end = timestampFromDate(end.addDays(1)),
          },
      .csvDir = findCsvDataDir(),
      .source = bte::data::StreamRequest::Source::csv,
  };

  auto stream = bte::data::CsvBarStream::open(request);
  if (!stream.ok()) {
    qWarning() << "Failed to load replay CSV:"
               << QString::fromStdString(stream.error().message);
    return {};
  }

  std::vector<bte::core::Bar> bars;
  while (auto bar = stream.value()->next()) {
    bars.push_back(*bar);
  }
  return barsForSchema(std::move(bars), schemaName);
}

} // namespace bte::bindings
