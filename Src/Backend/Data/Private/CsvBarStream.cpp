#include "Bte/Data/BarStream.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <fstream>
#include <locale>
#include <ranges>
#include <sstream>
#include <string_view>
#include <unordered_map>

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

[[nodiscard]] std::vector<std::string> splitCsvLine(const std::string& line) {
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
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())) != 0) {
        value.remove_prefix(1);
    }
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())) != 0) {
        value.remove_suffix(1);
    }
    return std::string{value};
}

[[nodiscard]] std::unordered_map<std::string, int> headerIndex(const std::vector<std::string>& header) {
    std::unordered_map<std::string, int> index;
    for (int i = 0; i < static_cast<int>(header.size()); ++i) {
        index.emplace(trim(header[static_cast<std::size_t>(i)]), i);
    }
    return index;
}

[[nodiscard]] core::Result<CsvColumns> resolveColumns(const std::vector<std::string>& header) {
    const auto index = headerIndex(header);
    auto require = [&](const std::string& name) -> core::Result<int> {
        const auto found = index.find(name);
        if (found == index.end()) {
            return core::makeError(core::ErrorCode::schemaMismatch,
                                   "CSV header is missing required column '" + name + "'");
        }
        return found->second;
    };

    CsvColumns columns;
    auto symbol = require("symbol");
    if (!symbol.ok()) {
        return symbol.error();
    }
    columns.symbol = symbol.value();

    const auto ts = require("ts");
    if (!ts.ok()) {
        return ts.error();
    }
    columns.timestamp = ts.value();

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

    if (const auto schemaName = index.find("schemaName"); schemaName != index.end()) {
        columns.schemaName = schemaName->second;
    }

    return columns;
}

[[nodiscard]] bool hasColumn(const std::vector<std::string>& fields, const int column) {
    return column >= 0 && static_cast<std::size_t>(column) < fields.size();
}

[[nodiscard]] core::Result<double> parseDouble(const std::vector<std::string>& fields, const int column,
                                               const char* name) {
    if (!hasColumn(fields, column)) {
        return core::makeError(core::ErrorCode::schemaMismatch,
                               std::string{"CSV row is missing column '"} + name + "'");
    }

    const auto text = trim(fields[static_cast<std::size_t>(column)]);
    std::istringstream input{text};
    input.imbue(std::locale::classic());

    double value = 0.0;
    input >> std::noskipws >> value;
    if (!input || input.peek() != std::char_traits<char>::eof()) {
        return core::makeError(core::ErrorCode::invalidArgument,
                               std::string{"CSV column '"} + name + "' is not numeric");
    }
    return value;
}

[[nodiscard]] bool rangeContains(const core::DateRange range, const core::Timestamp timestamp) {
    if (range.start != core::Timestamp{} && timestamp < range.start) {
        return false;
    }
    if (range.end != core::Timestamp{} && timestamp >= range.end) {
        return false;
    }
    return true;
}

[[nodiscard]] core::Result<core::Bar> parseBar(const std::vector<std::string>& fields, const CsvColumns columns,
                                               const std::string& expectedSymbol, const std::string& expectedSchemaName,
                                               const int lineNumber) {
    if (!hasColumn(fields, columns.symbol) ||
        trim(fields[static_cast<std::size_t>(columns.symbol)]) != expectedSymbol) {
        return core::makeError(core::ErrorCode::invalidArgument, "CSV line " + std::to_string(lineNumber) +
                                                                     " does not match requested symbol '" +
                                                                     expectedSymbol + "'");
    }

    if (columns.schemaName >= 0 && hasColumn(fields, columns.schemaName) && !expectedSchemaName.empty() &&
        trim(fields[static_cast<std::size_t>(columns.schemaName)]) != expectedSchemaName) {
        return core::makeError(core::ErrorCode::invalidArgument, "CSV line " + std::to_string(lineNumber) +
                                                                     " does not match requested schemaName '" +
                                                                     expectedSchemaName + "'");
    }

    if (!hasColumn(fields, columns.timestamp)) {
        return core::makeError(core::ErrorCode::schemaMismatch,
                               "CSV line " + std::to_string(lineNumber) + " is missing ts");
    }

    auto timestamp = core::time::parseIso8601(trim(fields[static_cast<std::size_t>(columns.timestamp)]));
    if (!timestamp.ok()) {
        return core::makeError(core::ErrorCode::invalidArgument,
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
                               "CSV line " + std::to_string(lineNumber) + " contains invalid OHLCV values");
    }

    return bar;
}

} // namespace

CsvBarStream::CsvBarStream(ConstructionKey, std::string symbol, std::string schemaName, core::DateRange range,
                           std::vector<core::Bar> bars)
    : symbol_(std::move(symbol)), schemaName_(std::move(schemaName)), range_(range), bars_(std::move(bars)) {}

core::Result<std::unique_ptr<CsvBarStream>> CsvBarStream::open(const StreamRequest& request) {
    if (request.symbol.empty()) {
        return core::makeError(core::ErrorCode::invalidArgument, "StreamRequest.symbol is required");
    }
    if (request.schemaName.empty()) {
        return core::makeError(core::ErrorCode::invalidArgument, "StreamRequest.schemaName is required");
    }

    const auto csvPath = request.csvDir / (request.symbol + ".csv");
    std::ifstream input{csvPath};
    if (!input) {
        return core::makeError(core::ErrorCode::notFound, "CSV file not found: " + csvPath.string());
    }

    std::string line;
    if (!std::getline(input, line)) {
        return core::makeError(core::ErrorCode::schemaMismatch, "CSV file is empty: " + csvPath.string());
    }

    auto columns = resolveColumns(splitCsvLine(line));
    if (!columns.ok()) {
        return columns.error();
    }

    std::vector<core::Bar> bars;
    int lineNumber = 1;
    while (std::getline(input, line)) {
        ++lineNumber;
        if (trim(line).empty()) {
            continue;
        }

        auto bar = parseBar(splitCsvLine(line), columns.value(), request.symbol, request.schemaName, lineNumber);
        if (!bar.ok()) {
            return bar.error();
        }
        if (rangeContains(request.range, bar.value().ts)) {
            bars.push_back(bar.value());
        }
    }

    std::ranges::sort(bars, {}, &core::Bar::ts);

    core::DateRange resolvedRange = request.range;
    if (!bars.empty()) {
        if (resolvedRange.start == core::Timestamp{}) {
            resolvedRange.start = bars.front().ts;
        }
        if (resolvedRange.end == core::Timestamp{}) {
            resolvedRange.end = bars.back().ts + std::chrono::milliseconds{1};
        }
    }

    return std::make_unique<CsvBarStream>(ConstructionKey{}, request.symbol, request.schemaName, resolvedRange,
                                          std::move(bars));
}

std::optional<core::Bar> CsvBarStream::next() {
    if (consumed_ >= totalBars()) {
        return std::nullopt;
    }
    return bars_[static_cast<std::size_t>(consumed_++)];
}

std::int64_t CsvBarStream::totalBars() const noexcept { return static_cast<std::int64_t>(bars_.size()); }

std::int64_t CsvBarStream::consumed() const noexcept { return consumed_; }

core::DateRange CsvBarStream::range() const noexcept { return range_; }

std::string CsvBarStream::symbol() const { return symbol_; }

std::string CsvBarStream::schemaName() const { return schemaName_; }

std::optional<core::Bar> CsvBarStream::at(const std::int64_t barIndex) const {
    if (barIndex < 0 || barIndex >= totalBars()) {
        return std::nullopt;
    }
    return bars_[static_cast<std::size_t>(barIndex)];
}

bool CsvBarStream::seek(const std::int64_t barIndex) noexcept {
    if (barIndex < 0 || barIndex > totalBars()) {
        return false;
    }
    consumed_ = barIndex;
    return true;
}

} // namespace bte::data
