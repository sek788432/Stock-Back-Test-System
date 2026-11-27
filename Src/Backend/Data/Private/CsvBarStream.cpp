#include "Bte/Data/BarStream.h"

#include "Bte/Core/Bar.h"
#include "Bte/Core/Cancellation.h"
#include "Bte/Core/Digest.h"
#include "Bte/Core/Result.h"
#include "Bte/Core/Time.h"

#include "SourceCsvManifest.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <locale>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

namespace bte::data {
namespace {

struct CsvColumns {
  int symbol = -1;
  int timestamp = -1;
  int open = -1;
  int high = -1;
  int low = -1;
  int close = -1;
  int volume = -1;
  int schemaName = -1;
};

[[nodiscard]] std::vector<std::string> splitCsvLine(const std::string &line) {
  std::vector<std::string> fields;
  std::string field;
  std::istringstream input{line};
  while (std::getline(input, field, ',')) {
    fields.push_back(std::move(field));
  }
  if (!line.empty() && line.back() == ',') {
    fields.emplace_back();
  }
  return fields;
}

[[nodiscard]] std::string trim(std::string_view value) {
  while (!value.empty() &&
         std::isspace(static_cast<unsigned char>(value.front())) != 0) {
    value.remove_prefix(1);
  }
  while (!value.empty() &&
         std::isspace(static_cast<unsigned char>(value.back())) != 0) {
    value.remove_suffix(1);
  }
  return std::string{value};
}

[[nodiscard]] std::unordered_map<std::string, int>
headerIndex(const std::vector<std::string> &header) {
  std::unordered_map<std::string, int> index;
  for (int i = 0; i < static_cast<int>(header.size()); ++i) {
    index.emplace(trim(header[static_cast<std::size_t>(i)]), i);
  }
  return index;
}

[[nodiscard]] core::Result<CsvColumns>
resolveColumns(const std::vector<std::string> &header) {
  const auto index = headerIndex(header);
  auto require = [&](const std::string &name) -> core::Result<int> {
    const auto found = index.find(name);
    if (found == index.end()) {
      return core::makeError(core::ErrorCode::schemaMismatch,
                             "CSV header is missing required column '" + name +
                                 "'");
    }
    return found->second;
  };

  CsvColumns columns;
  auto symbol = require("symbol");
  if (!symbol.ok()) {
    return symbol.error();
  }
  columns.symbol = symbol.value();

  const auto timestampColumn = require("ts");
  if (!timestampColumn.ok()) {
    return timestampColumn.error();
  }
  columns.timestamp = timestampColumn.value();

  const auto open = require("open");
  if (!open.ok()) {
    return open.error();
  }
  columns.open = open.value();

  const auto high = require("high");
  if (!high.ok()) {
    return high.error();
  }
  columns.high = high.value();

  const auto low = require("low");
  if (!low.ok()) {
    return low.error();
  }
  columns.low = low.value();

  const auto close = require("close");
  if (!close.ok()) {
    return close.error();
  }
  columns.close = close.value();

  const auto volume = require("volume");
  if (!volume.ok()) {
    return volume.error();
  }
  columns.volume = volume.value();

  if (const auto schemaName = index.find("schemaName");
      schemaName != index.end()) {
    columns.schemaName = schemaName->second;
  }

  return columns;
}

[[nodiscard]] bool hasColumn(const std::vector<std::string> &fields,
                             const int column) {
  return column >= 0 && static_cast<std::size_t>(column) < fields.size();
}

[[nodiscard]] core::Result<double>
parseDouble(const std::vector<std::string> &fields, const int column,
            const char *name) {
  if (!hasColumn(fields, column)) {
    return core::makeError(core::ErrorCode::schemaMismatch,
                           std::string{"CSV row is missing column '"} + name +
                               "'");
  }

  const auto text = trim(fields[static_cast<std::size_t>(column)]);
  std::istringstream input{text};
  input.imbue(std::locale::classic());

  double value = 0.0;
  input >> std::noskipws >> value;
  if (!input || input.peek() != std::char_traits<char>::eof()) {
    return core::makeError(core::ErrorCode::invalidArgument,
                           std::string{"CSV column '"} + name +
                               "' is not numeric");
  }
  return value;
}

[[nodiscard]] bool rangeContains(const core::DateRange range,
                                 const core::Timestamp timestamp) {
  if (range.start != core::Timestamp{} && timestamp < range.start) {
    return false;
  }
  if (range.end != core::Timestamp{} && timestamp >= range.end) {
    return false;
  }
  return true;
}

void hashLine(core::Sha256Accumulator &hash, const std::string &line,
              const bool hasTrailingNewline) {
  hash.update(std::as_bytes(std::span{line.data(), line.size()}));
  if (hasTrailingNewline) {
    constexpr std::array newline{std::byte{'\n'}};
    hash.update(newline);
  }
}

[[nodiscard]] core::Result<core::Bar>
parseBar(const std::vector<std::string> &fields, const CsvColumns &columns,
         const std::string &expectedSymbol,
         const std::string &expectedSchemaName, const int lineNumber) {
  if (!hasColumn(fields, columns.symbol) ||
      trim(fields[static_cast<std::size_t>(columns.symbol)]) !=
          expectedSymbol) {
    return core::makeError(core::ErrorCode::invalidArgument,
                           "CSV line " + std::to_string(lineNumber) +
                               " does not match requested symbol '" +
                               expectedSymbol + "'");
  }

  if (columns.schemaName >= 0 && hasColumn(fields, columns.schemaName) &&
      !expectedSchemaName.empty() &&
      trim(fields[static_cast<std::size_t>(columns.schemaName)]) !=
          expectedSchemaName) {
    return core::makeError(core::ErrorCode::invalidArgument,
                           "CSV line " + std::to_string(lineNumber) +
                               " does not match requested schemaName '" +
                               expectedSchemaName + "'");
  }

  if (!hasColumn(fields, columns.timestamp)) {
    return core::makeError(core::ErrorCode::schemaMismatch,
                           "CSV line " + std::to_string(lineNumber) +
                               " is missing ts");
  }

  auto timestamp = core::time::parseIso8601(
      trim(fields[static_cast<std::size_t>(columns.timestamp)]));
  if (!timestamp.ok()) {
    return core::makeError(
        core::ErrorCode::invalidArgument,
        "CSV line " + std::to_string(lineNumber) +
            " has invalid timestamp: " + timestamp.error().message);
  }

  auto open = parseDouble(fields, columns.open, "open");
  auto high = parseDouble(fields, columns.high, "high");
  auto low = parseDouble(fields, columns.low, "low");
  auto close = parseDouble(fields, columns.close, "close");
  auto volume = parseDouble(fields, columns.volume, "volume");
  if (!open.ok()) {
    return open.error();
  }
  if (!high.ok()) {
    return high.error();
  }
  if (!low.ok()) {
    return low.error();
  }
  if (!close.ok()) {
    return close.error();
  }
  if (!volume.ok()) {
    return volume.error();
  }

  core::Bar bar{
      .ts = timestamp.value(),
      .open = open.value(),
      .high = high.value(),
      .low = low.value(),
      .close = close.value(),
      .volume = volume.value(),
  };

  if (!bar.isValid()) {
    return core::makeError(core::ErrorCode::invalidArgument,
                           "CSV line " + std::to_string(lineNumber) +
                               " contains invalid OHLCV values");
  }

  return bar;
}

[[nodiscard]] core::Result<std::string>
readLooseCsvTimeframe(const std::filesystem::path &path) {
  std::ifstream input{path, std::ios::binary};
  std::string line;
  if (!input || !std::getline(input, line)) {
    std::error_code fileError;
    if (std::filesystem::is_regular_file(path, fileError)) {
      return core::makeError(core::ErrorCode::schemaMismatch,
                             "CSV file is empty: " + path.string());
    }
    return core::makeError(core::ErrorCode::notFound,
                           "CSV file not found: " + path.string());
  }
  auto columns = resolveColumns(splitCsvLine(line));
  if (!columns.ok()) {
    return columns.error();
  }

  std::string timeframe;
  if (columns.value().schemaName >= 0) {
    while (std::getline(input, line)) {
      if (trim(line).empty()) {
        continue;
      }
      const auto fields = splitCsvLine(line);
      if (hasColumn(fields, columns.value().schemaName)) {
        timeframe =
            trim(fields[static_cast<std::size_t>(columns.value().schemaName)]);
      }
      break;
    }
  }
  if (input.bad()) {
    return core::makeError(core::ErrorCode::schemaMismatch,
                           "Unable to read CSV source: " + path.string());
  }
  return timeframe;
}

[[nodiscard]] core::Result<CsvSourceInfo>
inspectCsvSource(const std::filesystem::path &path, const std::string &symbol,
                 const detail::SourceManifest &manifest) {
  const auto declaration = manifest.files.find(path.filename().string());
  if (manifest.present && declaration == manifest.files.end()) {
    return core::makeError(core::ErrorCode::notFound,
                           "CSV source is not declared for symbol '" + symbol +
                               "'");
  }

  auto timeframe = std::string{};
  auto sha256 = std::string{};
  const auto manifestBound = declaration != manifest.files.end();
  if (manifestBound) {
    timeframe = declaration->second.timeframe;
    sha256 = declaration->second.sha256;
  } else {
    auto detectedTimeframe = readLooseCsvTimeframe(path);
    if (!detectedTimeframe.ok()) {
      return detectedTimeframe.error();
    }
    timeframe = std::move(detectedTimeframe).value();
  }
  return CsvSourceInfo{.path = path,
                       .timeframe = std::move(timeframe),
                       .sha256 = std::move(sha256),
                       .manifestBound = manifestBound};
}

[[nodiscard]] core::Result<void>
appendSourceCatalogEntry(const std::string &symbol,
                         const std::filesystem::path &path,
                         const detail::SourceManifest &manifest,
                         std::vector<CsvSourceCatalogEntry> &sources) {
  auto source = inspectCsvSource(path, symbol, manifest);
  if (!source.ok()) {
    return source.error();
  }
  sources.push_back(CsvSourceCatalogEntry{.symbol = symbol,
                                          .source = std::move(source).value()});
  return {};
}

[[nodiscard]] core::Result<void>
appendSymbolCatalog(std::istream &input,
                    const std::filesystem::path &symbolsPath,
                    const detail::SourceManifest &manifest,
                    const std::filesystem::path &csvDirectory,
                    std::vector<CsvSourceCatalogEntry> &sources) {
  std::string symbol;
  while (std::getline(input, symbol)) {
    symbol = trim(symbol);
    if (symbol.empty()) {
      continue;
    }
    const auto source =
        detail::sourceCsvPathForSymbol(csvDirectory, symbol, manifest);
    if (!source.ok()) {
      if (source.error().code == core::ErrorCode::notFound) {
        continue;
      }
      return source.error();
    }
    auto filesystemError = std::error_code{};
    if (std::filesystem::is_regular_file(source.value(), filesystemError)) {
      const auto appended =
          appendSourceCatalogEntry(symbol, source.value(), manifest, sources);
      if (!appended.ok()) {
        return appended.error();
      }
    } else if (filesystemError) {
      return core::makeError(core::ErrorCode::schemaMismatch,
                             "Unable to inspect CSV source: " +
                                 source.value().string());
    }
  }
  if (input.bad()) {
    return core::makeError(core::ErrorCode::schemaMismatch,
                           "Unable to read symbol catalog: " +
                               symbolsPath.string());
  }
  return {};
}

[[nodiscard]] core::Result<void>
appendDirectoryCatalog(const std::filesystem::path &csvDirectory,
                       const detail::SourceManifest &manifest,
                       std::vector<CsvSourceCatalogEntry> &sources) {
  std::error_code error;
  for (std::filesystem::directory_iterator entry{csvDirectory, error}, end;
       entry != end; entry.increment(error)) {
    if (!entry->is_regular_file(error)) {
      if (error) {
        return core::makeError(core::ErrorCode::schemaMismatch,
                               "Unable to inspect CSV source: " +
                                   error.message());
      }
      continue;
    }
    const auto filename = entry->path().filename().string();
    if (!filename.ends_with(".csv")) {
      continue;
    }
    const auto symbol = entry->path().stem().string();
    const auto appended =
        appendSourceCatalogEntry(symbol, entry->path(), manifest, sources);
    if (!appended.ok()) {
      return appended.error();
    }
  }
  if (error) {
    return core::makeError(core::ErrorCode::schemaMismatch,
                           "Unable to enumerate CSV sources: " +
                               error.message());
  }
  return {};
}

} // namespace

CsvBarStream::CsvBarStream(ConstructionKey, std::vector<core::Bar> bars)
    : bars_(std::move(bars)) {}

core::Result<std::unique_ptr<CsvBarStream>>
CsvBarStream::open(const StreamRequest &request,
                   const core::CancellationToken &cancellation) {
  if (cancellation.isCancellationRequested()) {
    return core::makeError(core::ErrorCode::cancelled,
                           "CSV loading was cancelled");
  }
  if (request.symbol.empty()) {
    return core::makeError(core::ErrorCode::invalidArgument,
                           "StreamRequest.symbol is required");
  }
  if (request.schemaName.empty()) {
    return core::makeError(core::ErrorCode::invalidArgument,
                           "StreamRequest.schemaName is required");
  }

  const auto source = inspectSource(request.csvDir, request.symbol);
  if (!source.ok()) {
    return source.error();
  }
  if (source.value().manifestBound &&
      source.value().timeframe != request.schemaName) {
    return core::makeError(core::ErrorCode::schemaMismatch,
                           "CSV source timeframe '" + source.value().timeframe +
                               "' does not match requested source schema '" +
                               request.schemaName + "'");
  }

  std::ifstream input{source.value().path, std::ios::binary};
  if (!input) {
    return core::makeError(core::ErrorCode::notFound,
                           "CSV file not found: " +
                               source.value().path.string());
  }

  core::Sha256Accumulator sourceHash;
  std::string line;
  if (!std::getline(input, line)) {
    return core::makeError(core::ErrorCode::schemaMismatch,
                           "CSV file is empty: " +
                               source.value().path.string());
  }
  hashLine(sourceHash, line, !input.eof());

  auto columns = resolveColumns(splitCsvLine(line));
  if (!columns.ok()) {
    return columns.error();
  }

  std::vector<core::Bar> bars;
  int lineNumber = 1;
  while (std::getline(input, line)) {
    hashLine(sourceHash, line, !input.eof());
    if (cancellation.isCancellationRequested()) {
      return core::makeError(core::ErrorCode::cancelled,
                             "CSV loading was cancelled");
    }
    ++lineNumber;
    if (trim(line).empty()) {
      continue;
    }

    auto bar = parseBar(splitCsvLine(line), columns.value(), request.symbol,
                        request.schemaName, lineNumber);
    if (!bar.ok()) {
      return bar.error();
    }
    if (rangeContains(request.range, bar.value().ts)) {
      bars.push_back(bar.value());
    }
  }

  if (input.bad()) {
    return core::makeError(core::ErrorCode::schemaMismatch,
                           "Unable to read CSV file: " +
                               source.value().path.string());
  }
  if (source.value().manifestBound &&
      source.value().sha256 != sourceHash.finalize()) {
    return core::makeError(
        core::ErrorCode::dataSnapshotUnavailable,
        "CSV source SHA-256 does not match source manifest: " +
            source.value().path.filename().string());
  }

  std::ranges::sort(bars, {}, &core::Bar::ts);

  return std::make_unique<CsvBarStream>(ConstructionKey{}, std::move(bars));
}

core::Result<CsvSourceInfo>
CsvBarStream::inspectSource(const std::filesystem::path &csvDirectory,
                            const std::string &symbol) {
  if (symbol.empty()) {
    return core::makeError(core::ErrorCode::invalidArgument,
                           "CSV source symbol is required");
  }
  auto manifest = detail::readSourceManifest(csvDirectory);
  if (!manifest.ok()) {
    return manifest.error();
  }
  auto path =
      detail::sourceCsvPathForSymbol(csvDirectory, symbol, manifest.value());
  if (!path.ok()) {
    return path.error();
  }
  return inspectCsvSource(path.value(), symbol, manifest.value());
}

core::Result<std::vector<std::string>>
CsvBarStream::availableSymbols(const std::filesystem::path &csvDirectory) {
  const auto sources = availableSources(csvDirectory);
  if (!sources.ok()) {
    return sources.error();
  }
  std::vector<std::string> symbols;
  symbols.reserve(sources.value().size());
  for (const auto &source : sources.value()) {
    symbols.push_back(source.symbol);
  }
  return symbols;
}

core::Result<std::vector<CsvSourceCatalogEntry>>
CsvBarStream::availableSources(const std::filesystem::path &csvDirectory) {
  auto manifest = detail::readSourceManifest(csvDirectory);
  if (!manifest.ok()) {
    return manifest.error();
  }

  std::vector<CsvSourceCatalogEntry> sources;
  const auto symbolsPath = csvDirectory.parent_path() / "Symbols.txt";
  std::ifstream input{symbolsPath};
  core::Result<void> catalogResult;
  if (input) {
    catalogResult = appendSymbolCatalog(input, symbolsPath, manifest.value(),
                                        csvDirectory, sources);
  } else if (manifest.value().present) {
    return core::makeError(core::ErrorCode::schemaMismatch,
                           "Source symbol catalog is missing: " +
                               symbolsPath.string());
  } else {
    catalogResult =
        appendDirectoryCatalog(csvDirectory, manifest.value(), sources);
  }
  if (!catalogResult.ok()) {
    return catalogResult.error();
  }
  std::ranges::sort(sources, {}, &CsvSourceCatalogEntry::symbol);
  sources.erase(std::unique(sources.begin(), sources.end(),
                            [](const auto &left, const auto &right) {
                              return left.symbol == right.symbol;
                            }),
                sources.end());
  return sources;
}

std::optional<core::Bar> CsvBarStream::next() {
  if (consumed_ >= totalBars()) {
    return std::nullopt;
  }
  return bars_[static_cast<std::size_t>(consumed_++)];
}

std::int64_t CsvBarStream::totalBars() const noexcept {
  return static_cast<std::int64_t>(bars_.size());
}

std::int64_t CsvBarStream::consumed() const noexcept { return consumed_; }

void CsvBarStream::reset() noexcept { consumed_ = 0; }

} // namespace bte::data
