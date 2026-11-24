#include "Bte/Data/ReleaseSnapshot.h"

#include "Bte/Core/Digest.h"

#include "Bte/Data/SegmentRetention.h"

#include "PublishedSnapshotSegments.h"
#include "ReleaseSnapshotTestHooks.h"
#include "SourceCsvManifest.h"
#include <sqlite3.h>

#include <algorithm>
#include <array>
#if defined(BTE_ENABLE_TEST_HOOKS)
#include <atomic>
#endif
#include <charconv>
#include <chrono>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges> // IWYU pragma: keep
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <utility>
#include <vector>

namespace bte::data {
namespace {

struct SnapshotPublicationLock final {
  explicit SnapshotPublicationLock(std::mutex &mutex)
      : localLock{mutex}, database{nullptr, sqlite3_close} {}
  std::unique_lock<std::mutex> localLock;
  std::unique_ptr<sqlite3, decltype(&sqlite3_close)> database;
};

core::Result<std::unique_ptr<SnapshotPublicationLock>>
lockSnapshotPublication(const std::filesystem::path &storeDirectory) {
  static std::mutex publicationMutex;
  auto lock = std::make_unique<SnapshotPublicationLock>(publicationMutex);
  std::error_code error;
  std::filesystem::create_directories(storeDirectory, error);
  if (error) {
    return core::makeError(core::ErrorCode::permissionDenied,
                           "Unable to prepare snapshot store: " +
                               error.message());
  }
  sqlite3 *raw = nullptr;
  const auto path = storeDirectory / "SnapshotOperations.sqlite3";
  const auto opened = sqlite3_open_v2(
      path.string().c_str(), &raw,
      SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
      nullptr);
  lock->database.reset(raw);
  if (opened != SQLITE_OK || raw == nullptr) {
    return core::makeError(core::ErrorCode::internal,
                           "Unable to open snapshot publication lock");
  }
  sqlite3_busy_timeout(raw, 5'000);
  if (sqlite3_exec(raw, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr) !=
      SQLITE_OK) {
    return core::makeError(core::ErrorCode::resourceBusy,
                           "Unable to acquire snapshot publication lock");
  }
  return lock;
}

core::Result<void>
recoverUnpublishedSnapshotPins(const std::filesystem::path &storeDirectory) {
  std::error_code error;
  const auto referencesExist =
      std::filesystem::exists(storeDirectory / "References.sqlite3", error);
  if (error) {
    return core::makeError(core::ErrorCode::permissionDenied,
                           "Unable to inspect segment retention state: " +
                               error.message());
  }
  const auto segmentDirectory = storeDirectory / "Segments";
  const auto segmentDirectoryExists =
      std::filesystem::exists(segmentDirectory, error);
  if (error) {
    return core::makeError(core::ErrorCode::permissionDenied,
                           "Unable to inspect snapshot segments: " +
                               error.message());
  }
  const auto hasSegmentArtifacts =
      segmentDirectoryExists &&
      !std::filesystem::is_empty(segmentDirectory, error);
  if (error) {
    return core::makeError(core::ErrorCode::permissionDenied,
                           "Unable to inspect snapshot segments: " +
                               error.message());
  }
  if (!referencesExist && !hasSegmentArtifacts) {
    return {};
  }
  auto retention = SegmentRetentionStore::open(storeDirectory);
  if (!retention.ok()) {
    return retention.error();
  }
  return retention.value()->recoverUnpublishedSnapshotPins();
}

#if defined(BTE_ENABLE_TEST_HOOKS)
std::atomic<testing::SnapshotFailurePoint> &snapshotFailurePoint() {
  static std::atomic<testing::SnapshotFailurePoint> value{
      testing::SnapshotFailurePoint::none};
  return value;
}

std::atomic_size_t &snapshotSegmentReads() {
  static std::atomic_size_t value{0};
  return value;
}

bool consumeFailure(const testing::SnapshotFailurePoint point) noexcept {
  auto expected = point;
  return snapshotFailurePoint().compare_exchange_strong(
      expected, testing::SnapshotFailurePoint::none);
}
#else
constexpr bool consumeFailure(testing::SnapshotFailurePoint) noexcept {
  return false;
}
#endif

bool cancellationRequested(
    const core::CancellationToken &cancellation,
    const testing::SnapshotFailurePoint failurePoint) noexcept {
#if defined(BTE_ENABLE_TEST_HOOKS)
  return consumeFailure(failurePoint) || cancellation.isCancellationRequested();
#else
  static_cast<void>(failurePoint);
  return cancellation.isCancellationRequested();
#endif
}

constexpr std::string_view segmentMagic = "BTEDATA1";
constexpr std::string_view manifestMagic = "BTE-SNAPSHOT-MANIFEST-V1";
constexpr std::string_view hourlyTimeframe = "ohlcv-1h";
constexpr std::string_view snapshotProfile = "vendorExtendedHours";

struct SegmentMetadata {
  std::size_t ordinal = 0;
  std::string symbol;
  std::string id;
  std::size_t rowCount = 0;
  core::Timestamp firstTimestamp;
  core::Timestamp lastTimestamp;
};

struct LoadedSegment {
  SegmentMetadata metadata;
  std::vector<SnapshotBar> bars;
};

std::string trim(std::string_view value) {
  while (!value.empty() && (value.front() == ' ' || value.front() == '\t' ||
                            value.front() == '\r' || value.front() == '\n')) {
    value.remove_prefix(1);
  }
  while (!value.empty() && (value.back() == ' ' || value.back() == '\t' ||
                            value.back() == '\r' || value.back() == '\n')) {
    value.remove_suffix(1);
  }
  return std::string{value};
}

std::vector<std::string> split(std::string_view text, const char delimiter) {
  std::vector<std::string> fields;
  while (true) {
    const auto position = text.find(delimiter);
    if (position == std::string_view::npos) {
      fields.push_back(trim(text));
      break;
    }
    fields.push_back(trim(text.substr(0, position)));
    text.remove_prefix(position + 1);
  }
  return fields;
}

core::Result<std::int64_t> parseInteger(std::string_view text,
                                        const std::string_view field) {
  std::int64_t value = 0;
  const auto conversion =
      std::from_chars(text.data(), text.data() + text.size(), value);
  if (conversion.ec != std::errc{} ||
      conversion.ptr != text.data() + text.size()) {
    return core::makeError(core::ErrorCode::schemaMismatch,
                           std::string{field} + " is not an integer");
  }
  return value;
}

core::Result<std::int64_t> parseScaled(std::string_view text,
                                       const int fractionalDigits,
                                       const std::string_view field) {
  const auto cleaned = trim(text);
  if (cleaned.empty() || cleaned.front() == '-') {
    return core::makeError(core::ErrorCode::invalidArgument,
                           std::string{field} + " must be non-negative");
  }

  const auto dot = cleaned.find('.');
  const auto integerText = cleaned.substr(0, dot);
  auto fractionalText =
      dot == std::string::npos ? std::string{} : cleaned.substr(dot + 1);
  if (integerText.empty() ||
      (dot != std::string::npos && fractionalText.empty()) ||
      fractionalText.size() > static_cast<std::size_t>(fractionalDigits)) {
    return core::makeError(core::ErrorCode::invalidArgument,
                           std::string{field} + " has unsupported precision");
  }

  if (!std::ranges::all_of(fractionalText, [](const char digit) {
        return digit >= '0' && digit <= '9';
      })) {
    return core::makeError(core::ErrorCode::invalidArgument,
                           std::string{field} + " is invalid");
  }

  auto integer = parseInteger(integerText, field);
  if (!integer.ok() || integer.value() < 0) {
    return core::makeError(core::ErrorCode::invalidArgument,
                           std::string{field} + " is invalid");
  }
  fractionalText.append(
      static_cast<std::size_t>(fractionalDigits) - fractionalText.size(), '0');
  // Price and volume policies always retain at least one fractional digit.
  // GCOVR_EXCL_START
  auto fractional = fractionalText.empty()
                        ? core::Result<std::int64_t>{std::int64_t{0}}
                        : parseInteger(fractionalText, field);
  // GCOVR_EXCL_STOP
  if (!fractional.ok()) {
    return fractional.error();
  }

  std::int64_t scale = 1;
  for (int digit = 0; digit < fractionalDigits; ++digit) {
    scale *= 10;
  }
  if (integer.value() >
      (std::numeric_limits<std::int64_t>::max() - fractional.value()) / scale) {
    return core::makeError(core::ErrorCode::invalidArgument,
                           std::string{field} + " exceeds fixed-point range");
  }
  return integer.value() * scale + fractional.value();
}

struct SourceLayout final {
  std::size_t columnCount = 0;
  std::size_t schemaColumnIndex = 0;
  bool hasSchemaColumn = false;
};

using detail::readSourceManifest;
using detail::SourceFileDeclaration;
using detail::SourceManifest;

[[nodiscard]] bool validHash(const std::string &hash);

[[nodiscard]] core::Result<SourceLayout>
parseSourceLayout(const std::span<const std::string> header) {
  constexpr std::array extractedColumns{"symbol", "ts",    "open",  "high",
                                        "low",    "close", "volume"};
  const auto extractedHeader =
      header.size() >= extractedColumns.size() &&
      std::ranges::equal(extractedColumns,
                         header.first(extractedColumns.size()));
  if (extractedHeader && header.size() == extractedColumns.size()) {
    return SourceLayout{.columnCount = header.size(),
                        .schemaColumnIndex = 0,
                        .hasSchemaColumn = false};
  }
  if (extractedHeader && header.size() > extractedColumns.size()) {
    const auto trailingColumns = header.subspan(extractedColumns.size());
    const auto schemaColumn = std::ranges::find(trailingColumns, "schemaName");
    if (schemaColumn != trailingColumns.end() &&
        std::ranges::count(trailingColumns, "schemaName") == 1) {
      return SourceLayout{
          .columnCount = header.size(),
          .schemaColumnIndex = static_cast<std::size_t>(
              std::distance(header.begin(), schemaColumn)),
          .hasSchemaColumn = true,
      };
    }
  }
  return core::makeError(core::ErrorCode::schemaMismatch,
                         "Snapshot source has unsupported columns");
}

core::Result<std::string>
readSourceBytes(const std::filesystem::path &path,
                const core::CancellationToken &cancellation) {
  std::ifstream file{path, std::ios::binary};
  if (!file) {
    return core::makeError(core::ErrorCode::notFound,
                           "Snapshot source not found: " + path.string());
  }
  auto sourceBytes = std::string{};
  constexpr std::size_t readBufferSize = 65'536;
  auto readBuffer = std::array<char, readBufferSize>{};
  while (true) {
    if (cancellationRequested(
            cancellation, testing::SnapshotFailurePoint::buildCancellation)) {
      return core::makeError(core::ErrorCode::cancelled,
                             "Snapshot build was cancelled");
    }
    file.read(readBuffer.data(),
              static_cast<std::streamsize>(readBuffer.size()));
    const auto readCount = file.gcount();
    if (readCount > 0) {
      sourceBytes.append(readBuffer.data(),
                         static_cast<std::size_t>(readCount));
    }
    if (file.bad()) {
      return core::makeError(core::ErrorCode::schemaMismatch,
                             "Unable to read snapshot source: " +
                                 path.string());
    }
    if (file.eof()) {
      break;
    }
  }
  return sourceBytes;
}

core::Result<std::optional<SourceFileDeclaration>>
sourceDeclarationFor(const std::filesystem::path &path,
                     const std::string &sourceBytes,
                     const SourceManifest &sourceManifest) {
  const auto sourceFileName = path.filename().string();
  const auto declaration = sourceManifest.files.find(sourceFileName);
  if (sourceManifest.present && declaration == sourceManifest.files.end()) {
    return core::makeError(core::ErrorCode::schemaMismatch,
                           "Snapshot source is missing from source manifest: " +
                               sourceFileName);
  }
  if (declaration == sourceManifest.files.end()) {
    return std::optional<SourceFileDeclaration>{};
  }
  if (declaration->second.sha256 != core::sha256(sourceBytes)) {
    return core::makeError(
        core::ErrorCode::schemaMismatch,
        "Snapshot source hash does not match source manifest: " +
            sourceFileName);
  }
  return std::optional<SourceFileDeclaration>{declaration->second};
}

core::Result<std::string>
sourceTimeframeFor(const SourceLayout &layout,
                   const std::optional<SourceFileDeclaration> &declaration) {
  if (!layout.hasSchemaColumn && !declaration.has_value()) {
    return core::makeError(core::ErrorCode::schemaMismatch,
                           "Seven-column snapshot sources require a hash-bound "
                           "source timeframe manifest");
  }
  const auto timeframe = declaration.has_value() ? declaration->timeframe
                                                 : std::string{hourlyTimeframe};
  if (timeframe != hourlyTimeframe) {
    return core::makeError(core::ErrorCode::schemaMismatch,
                           "Snapshot source timeframe is not supported: " +
                               timeframe);
  }
  return timeframe;
}

bool validOhlcv(const SnapshotBar &bar) {
  return bar.openNanodollars > 0 && bar.lowNanodollars > 0 &&
         bar.volumeMicroshares >= 0 &&
         bar.highNanodollars >=
             std::max({bar.openNanodollars, bar.closeNanodollars,
                       bar.lowNanodollars}) &&
         bar.lowNanodollars <=
             std::min({bar.openNanodollars, bar.closeNanodollars,
                       bar.highNanodollars});
}

core::Result<SnapshotBar>
parseSourceRow(const std::span<const std::string> fields,
               const std::string &symbol, const SourceLayout &layout,
               const std::string &timeframe) {
  if (fields.size() != layout.columnCount || fields[0] != symbol) {
    return core::makeError(core::ErrorCode::schemaMismatch,
                           "Snapshot row identity does not match request");
  }
  if (layout.hasSchemaColumn && fields[layout.schemaColumnIndex] != timeframe) {
    return core::makeError(core::ErrorCode::schemaMismatch,
                           "Snapshot row identity does not match request");
  }

  auto parsedTimestamp = core::time::parseIso8601(fields[1]);
  auto open = parseScaled(fields[2], 9, "open");
  auto high = parseScaled(fields[3], 9, "high");
  auto low = parseScaled(fields[4], 9, "low");
  auto close = parseScaled(fields[5], 9, "close");
  auto volume = parseScaled(fields[6], 6, "volume");
  if (!parsedTimestamp.ok()) {
    return parsedTimestamp.error();
  }
  for (const auto *value : {&open, &high, &low, &close, &volume}) {
    if (!value->ok()) {
      return value->error();
    }
  }

  const SnapshotBar bar{
      .timestamp = parsedTimestamp.value(),
      .openNanodollars = open.value(),
      .highNanodollars = high.value(),
      .lowNanodollars = low.value(),
      .closeNanodollars = close.value(),
      .volumeMicroshares = volume.value(),
  };
  if (!validOhlcv(bar)) {
    return core::makeError(core::ErrorCode::invalidArgument,
                           "Snapshot row contains invalid OHLCV values");
  }
  return bar;
}

core::Result<std::vector<SnapshotBar>>
parseSourceRows(std::istream &input, const std::string &symbol,
                const SourceLayout &layout, const std::string &timeframe,
                const core::CancellationToken &cancellation) {
  std::vector<SnapshotBar> bars;
  std::string line;
  while (std::getline(input, line)) {
    if (cancellationRequested(
            cancellation, testing::SnapshotFailurePoint::buildCancellation)) {
      return core::makeError(core::ErrorCode::cancelled,
                             "Snapshot build was cancelled");
    }
    if (trim(line).empty()) {
      continue;
    }
    const auto fields = split(line, ',');
    auto bar = parseSourceRow(fields, symbol, layout, timeframe);
    if (!bar.ok()) {
      return bar.error();
    }
    if (!bars.empty() && bars.back().timestamp >= bar.value().timestamp) {
      return core::makeError(core::ErrorCode::invalidArgument,
                             "Snapshot source timestamps must be increasing");
    }
    bars.push_back(std::move(bar).value());
  }
  return bars;
}

core::Result<std::vector<SnapshotBar>>
readSourceBars(const std::filesystem::path &path, const std::string &symbol,
               const SourceManifest &sourceManifest,
               const core::CancellationToken &cancellation) {
  auto sourceBytes = readSourceBytes(path, cancellation);
  if (!sourceBytes.ok()) {
    return sourceBytes.error();
  }
  auto declaration =
      sourceDeclarationFor(path, sourceBytes.value(), sourceManifest);
  if (!declaration.ok()) {
    return declaration.error();
  }

  std::istringstream input{sourceBytes.value()};
  std::string line;
  if (!std::getline(input, line)) {
    return core::makeError(core::ErrorCode::schemaMismatch,
                           "Snapshot source is empty: " + path.string());
  }
  const auto header = split(line, ',');
  const auto layout = parseSourceLayout(header);
  if (!layout.ok()) {
    return layout.error();
  }
  auto timeframe = sourceTimeframeFor(layout.value(), declaration.value());
  if (!timeframe.ok()) {
    return timeframe.error();
  }
  return parseSourceRows(input, symbol, layout.value(), timeframe.value(),
                         cancellation);
}

void appendU64(std::vector<std::byte> &bytes, const std::uint64_t value) {
  for (int shift = 56; shift >= 0; shift -= 8) {
    bytes.push_back(static_cast<std::byte>((value >> shift) & 0xffU));
  }
}

void appendI64(std::vector<std::byte> &bytes, const std::int64_t value) {
  appendU64(bytes, static_cast<std::uint64_t>(value));
}

void appendText(std::vector<std::byte> &bytes, const std::string_view text) {
  appendU64(bytes, text.size());
  const auto raw = std::as_bytes(std::span{text.data(), text.size()});
  bytes.insert(bytes.end(), raw.begin(), raw.end());
}

std::vector<std::byte> encodeSegment(const std::string &symbol,
                                     const std::span<const SnapshotBar> bars) {
  std::vector<std::byte> bytes;
  bytes.reserve(segmentMagic.size() + 32 + symbol.size() + bars.size() * 48);
  const auto magic =
      std::as_bytes(std::span{segmentMagic.data(), segmentMagic.size()});
  bytes.insert(bytes.end(), magic.begin(), magic.end());
  appendU64(bytes, 1);
  appendText(bytes, symbol);
  appendU64(bytes, bars.size());
  for (const auto &bar : bars) {
    appendI64(bytes, core::time::toUnixMillis(bar.timestamp));
    appendI64(bytes, bar.openNanodollars);
    appendI64(bytes, bar.highNanodollars);
    appendI64(bytes, bar.lowNanodollars);
    appendI64(bytes, bar.closeNanodollars);
    appendI64(bytes, bar.volumeMicroshares);
  }
  return bytes;
}

core::Result<void> writeBytes(const std::filesystem::path &path,
                              const std::span<const std::byte> bytes) {
  auto stagingPath = path;
  stagingPath += ".staging";
  std::error_code error;
  std::filesystem::remove(stagingPath, error);
  if (error) {
    return core::makeError(core::ErrorCode::permissionDenied,
                           "Unable to clear snapshot artifact staging: " +
                               error.message());
  }
  std::ofstream output{stagingPath, std::ios::binary | std::ios::trunc};
  if (!output) {
    return core::makeError(core::ErrorCode::permissionDenied,
                           "Unable to create snapshot artifact: " +
                               stagingPath.string());
  }
  const auto injectedFailure =
      consumeFailure(testing::SnapshotFailurePoint::artifactWrite);
  const auto bytesToWrite = injectedFailure ? bytes.size() / 2U : bytes.size();
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): bytes
  output.write(reinterpret_cast<const char *>(bytes.data()),
               static_cast<std::streamsize>(bytesToWrite));
  output.close();
  if (injectedFailure || !output) {
    std::filesystem::remove(stagingPath, error);
    return core::makeError(core::ErrorCode::permissionDenied,
                           "Unable to write snapshot artifact: " +
                               path.string());
  }
  std::filesystem::rename(stagingPath, path, error);
  if (error) {
    const auto publishError = error;
    std::error_code cleanupError;
    std::filesystem::remove(stagingPath, cleanupError);
    return core::makeError(core::ErrorCode::permissionDenied,
                           "Unable to publish snapshot artifact: " +
                               publishError.message());
  }
  return {};
}

core::Result<std::vector<std::byte>>
readBytes(const std::filesystem::path &path) {
  std::ifstream input{path, std::ios::binary};
  if (!input) {
    return core::makeError(core::ErrorCode::dataSnapshotUnavailable,
                           "Data Segment is unavailable: " + path.string());
  }
  input.seekg(0, std::ios::end);
  const auto size = input.tellg();
  if (consumeFailure(testing::SnapshotFailurePoint::artifactInspection) ||
      size < 0) {
    return core::makeError(core::ErrorCode::dataSnapshotUnavailable,
                           "Unable to inspect Data Segment");
  }
  input.seekg(0, std::ios::beg);
  std::vector<std::byte> bytes(static_cast<std::size_t>(size));
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): bytes
  input.read(reinterpret_cast<char *>(bytes.data()), size);
  if (consumeFailure(testing::SnapshotFailurePoint::artifactRead) || !input) {
    return core::makeError(core::ErrorCode::dataSnapshotUnavailable,
                           "Unable to read Data Segment");
  }
  return bytes;
}

core::Result<std::uint64_t> readU64(const std::span<const std::byte> bytes,
                                    std::size_t &cursor) {
  if (cursor + 8 > bytes.size()) {
    return core::makeError(core::ErrorCode::dataSnapshotUnavailable,
                           "Data Segment is truncated");
  }
  std::uint64_t value = 0;
  for (int count = 0; count < 8; ++count) {
    value = (value << 8U) | std::to_integer<std::uint64_t>(bytes[cursor++]);
  }
  return value;
}

core::Result<std::string> readText(const std::span<const std::byte> bytes,
                                   std::size_t &cursor) {
  auto length = readU64(bytes, cursor);
  if (!length.ok() || length.value() > bytes.size() - cursor) {
    return core::makeError(core::ErrorCode::dataSnapshotUnavailable,
                           "Data Segment text is truncated");
  }
  const auto text =
      // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): bytes
      std::string{reinterpret_cast<const char *>(bytes.data() + cursor),
                  static_cast<std::size_t>(length.value())};
  cursor += static_cast<std::size_t>(length.value());
  return text;
}

core::Result<std::vector<SnapshotBar>>
decodeSegment(const std::span<const std::byte> bytes,
              const std::string &expectedSymbol,
              const std::size_t expectedRows) {
  if (bytes.size() < segmentMagic.size() ||
      !std::ranges::equal(
          std::as_bytes(std::span{segmentMagic.data(), segmentMagic.size()}),
          bytes.first(segmentMagic.size()))) {
    return core::makeError(core::ErrorCode::dataSnapshotUnavailable,
                           "Data Segment magic is invalid");
  }
  std::size_t cursor = segmentMagic.size();
  auto version = readU64(bytes, cursor);
  auto symbol = readText(bytes, cursor);
  auto count = readU64(bytes, cursor);
  if (!version.ok() || version.value() != 1 || !symbol.ok() ||
      symbol.value() != expectedSymbol || !count.ok() ||
      count.value() != expectedRows) {
    return core::makeError(core::ErrorCode::dataSnapshotUnavailable,
                           "Data Segment metadata is invalid");
  }

  constexpr std::size_t rowBytes = 6 * sizeof(std::int64_t);
  const auto remaining = bytes.size() - cursor;
  if (remaining % rowBytes != 0 || remaining / rowBytes != expectedRows) {
    return core::makeError(core::ErrorCode::dataSnapshotUnavailable,
                           "Data Segment row count or length is invalid");
  }

  std::vector<SnapshotBar> bars;
  bars.reserve(expectedRows);
  for (std::size_t index = 0; index < expectedRows; ++index) {
    std::array<std::int64_t, 6> values{};
    for (auto &value : values) {
      auto encoded = readU64(bytes, cursor);
      if (!encoded.ok()) {
        return encoded.error();
      }
      value = static_cast<std::int64_t>(encoded.value());
    }
    const SnapshotBar bar{
        .timestamp = core::time::fromUnixMillis(values[0]),
        .openNanodollars = values[1],
        .highNanodollars = values[2],
        .lowNanodollars = values[3],
        .closeNanodollars = values[4],
        .volumeMicroshares = values[5],
    };
    if (!validOhlcv(bar)) {
      return core::makeError(core::ErrorCode::dataSnapshotUnavailable,
                             "Data Segment contains invalid OHLCV values");
    }
    bars.push_back(bar);
  }
  if (cursor != bytes.size() ||
      !std::ranges::is_sorted(bars, {}, &SnapshotBar::timestamp) ||
      std::ranges::adjacent_find(bars, {}, &SnapshotBar::timestamp) !=
          bars.end()) {
    return core::makeError(core::ErrorCode::dataSnapshotUnavailable,
                           "Data Segment row order is invalid");
  }
  return bars;
}

std::string makeManifest(const SnapshotBuildRequest &request,
                         const std::vector<SegmentMetadata> &segments) {
  std::ostringstream manifest;
  manifest << manifestMagic << '\n';
  manifest << "profile=" << snapshotProfile << '\n';
  manifest << "timeframe=" << hourlyTimeframe << '\n';
  manifest << "calendarHash=" << request.calendarHash << '\n';
  manifest << "splitManifestHash=" << request.splitManifestHash << '\n';
  for (const auto &segment : segments) {
    manifest << "segment=" << segment.ordinal << '|' << segment.symbol << '|'
             << segment.id << '|' << segment.rowCount << '|'
             << core::time::toUnixMillis(segment.firstTimestamp) << '|'
             << core::time::toUnixMillis(segment.lastTimestamp) << '\n';
  }
  return manifest.str();
}

// clang-format off
core::Result<std::vector<SegmentMetadata>> parseManifest(
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): manifest fields
    const std::string &manifest, std::string &calendarHash,
    std::string &splitManifestHash) {
  // clang-format on
  std::istringstream input{manifest};
  std::string line;
  if (!std::getline(input, line) || line != manifestMagic) {
    return core::makeError(core::ErrorCode::dataSnapshotUnavailable,
                           "Snapshot manifest magic is invalid");
  }
  std::array<std::string, 4> fixed{};
  for (auto &entry : fixed) {
    if (!std::getline(input, entry)) {
      return core::makeError(core::ErrorCode::dataSnapshotUnavailable,
                             "Snapshot manifest is truncated");
    }
  }
  if (fixed[0] != "profile=vendorExtendedHours" ||
      fixed[1] != "timeframe=ohlcv-1h" ||
      !fixed[2].starts_with("calendarHash=") ||
      !fixed[3].starts_with("splitManifestHash=")) {
    return core::makeError(core::ErrorCode::dataSnapshotUnavailable,
                           "Snapshot manifest profile is invalid");
  }
  calendarHash = fixed[2].substr(std::string{"calendarHash="}.size());
  splitManifestHash = fixed[3].substr(std::string{"splitManifestHash="}.size());

  std::vector<SegmentMetadata> segments;
  while (std::getline(input, line)) {
    if (!line.starts_with("segment=")) {
      return core::makeError(core::ErrorCode::dataSnapshotUnavailable,
                             "Snapshot manifest record is invalid");
    }
    const auto fields = split(line.substr(std::string{"segment="}.size()), '|');
    if (fields.size() != 6) {
      return core::makeError(core::ErrorCode::dataSnapshotUnavailable,
                             "Snapshot segment metadata is invalid");
    }
    auto ordinal = parseInteger(fields[0], "segment ordinal");
    auto rowCount = parseInteger(fields[3], "segment row count");
    auto first = parseInteger(fields[4], "segment first timestamp");
    auto last = parseInteger(fields[5], "segment last timestamp");
    if (!ordinal.ok() || !rowCount.ok() || !first.ok() || !last.ok() ||
        ordinal.value() != static_cast<std::int64_t>(segments.size()) ||
        rowCount.value() <= 0 || fields[1].empty() || fields[2].size() != 64) {
      return core::makeError(core::ErrorCode::dataSnapshotUnavailable,
                             "Snapshot segment metadata is invalid");
    }
    segments.push_back(SegmentMetadata{
        .ordinal = static_cast<std::size_t>(ordinal.value()),
        .symbol = fields[1],
        .id = fields[2],
        .rowCount = static_cast<std::size_t>(rowCount.value()),
        .firstTimestamp = core::time::fromUnixMillis(first.value()),
        .lastTimestamp = core::time::fromUnixMillis(last.value()),
    });
  }
  return segments;
}

core::Result<std::vector<std::pair<std::string, std::filesystem::path>>>
resolveSourceCsvFiles(const std::filesystem::path &sourceDirectory,
                      const std::vector<std::string> &symbols,
                      const SourceManifest &sourceManifest) {
  auto sourceFiles =
      std::vector<std::pair<std::string, std::filesystem::path>>{};
  sourceFiles.reserve(symbols.size());
  auto sourceFilenames = std::unordered_set<std::string>{};
  for (const auto &symbol : symbols) {
    auto sourcePath =
        detail::sourceCsvPathForSymbol(sourceDirectory, symbol, sourceManifest);
    if (!sourcePath.ok()) {
      if (sourceManifest.present &&
          sourcePath.error().code == core::ErrorCode::notFound) {
        return core::makeError(
            core::ErrorCode::schemaMismatch,
            "Snapshot source is missing from source manifest: " + symbol);
      }
      return sourcePath.error();
    }
    if (!sourceFilenames.emplace(sourcePath.value().filename().string())
             .second) {
      return core::makeError(
          core::ErrorCode::invalidArgument,
          "Snapshot symbols map to the same extracted CSV filename");
    }
    sourceFiles.emplace_back(symbol, std::move(sourcePath).value());
  }
  return sourceFiles;
}

bool validHash(const std::string &hash) {
  return hash.size() == 64 &&
         std::ranges::all_of(hash, [](const char character) {
           return (character >= '0' && character <= '9') ||
                  (character >= 'a' && character <= 'f');
         });
}

struct EncodedSegment final {
  SegmentMetadata metadata;
  std::vector<std::byte> bytes;
};

struct EncodedSnapshot final {
  std::vector<EncodedSegment> segments;
  std::size_t barCount = 0;
};

core::Result<EncodedSnapshot>
encodeSnapshotSegments(const SnapshotBuildRequest &request,
                       const std::vector<std::string> &symbols,
                       const detail::SourceManifest &sourceManifest,
                       const core::CancellationToken &cancellation) {
  auto sourceFiles =
      resolveSourceCsvFiles(request.sourceDirectory, symbols, sourceManifest);
  if (!sourceFiles.ok()) {
    return sourceFiles.error();
  }

  EncodedSnapshot snapshot;
  for (const auto &[symbol, sourcePath] : sourceFiles.value()) {
    auto bars =
        readSourceBars(sourcePath, symbol, sourceManifest, cancellation);
    if (!bars.ok()) {
      return bars.error();
    }
    snapshot.barCount += bars.value().size();
    for (std::size_t offset = 0; offset < bars.value().size();
         offset += request.rowsPerSegment) {
      const auto count =
          std::min(request.rowsPerSegment, bars.value().size() - offset);
      const auto window =
          std::span<const SnapshotBar>{bars.value()}.subspan(offset, count);
      auto bytes = encodeSegment(symbol, window);
      const auto segmentId = core::sha256(bytes);
      snapshot.segments.push_back(EncodedSegment{
          .metadata = {.ordinal = snapshot.segments.size(),
                       .symbol = symbol,
                       .id = segmentId,
                       .rowCount = count,
                       .firstTimestamp = window.front().timestamp,
                       .lastTimestamp = window.back().timestamp},
          .bytes = std::move(bytes),
      });
    }
  }
  return snapshot;
}

std::vector<SegmentMetadata>
snapshotSegmentMetadata(const std::vector<EncodedSegment> &segments) {
  std::vector<SegmentMetadata> metadata;
  metadata.reserve(segments.size());
  for (const auto &segment : segments) {
    metadata.push_back(segment.metadata);
  }
  return metadata;
}

core::Result<void>
writeSnapshotSegments(const SnapshotBuildRequest &request,
                      const std::vector<EncodedSegment> &segments) {
  const auto segmentDirectory = request.storeDirectory / "Segments";
  const auto snapshotDirectory = request.storeDirectory / "Snapshots";
  std::filesystem::create_directories(segmentDirectory);
  std::filesystem::create_directories(snapshotDirectory);
  for (const auto &segment : segments) {
    const auto path = segmentDirectory / (segment.metadata.id + ".btedata");
    const auto trashedPath = request.storeDirectory / "Trash" / "Segments" /
                             (segment.metadata.id + ".btedata");
    if (std::filesystem::exists(path)) {
      auto existing = readBytes(path);
      if (!existing.ok() ||
          core::sha256(existing.value()) != segment.metadata.id) {
        return core::makeError(core::ErrorCode::dataSnapshotUnavailable,
                               "Existing Data Segment is corrupt");
      }
      if (std::filesystem::exists(trashedPath)) {
        auto trashed = readBytes(trashedPath);
        if (!trashed.ok() ||
            core::sha256(trashed.value()) != segment.metadata.id) {
          return core::makeError(core::ErrorCode::dataSnapshotUnavailable,
                                 "Trashed Data Segment is corrupt");
        }
        std::filesystem::remove(trashedPath);
      }
      continue;
    }
    if (std::filesystem::exists(trashedPath)) {
      auto trashed = readBytes(trashedPath);
      if (!trashed.ok() ||
          core::sha256(trashed.value()) != segment.metadata.id) {
        return core::makeError(core::ErrorCode::dataSnapshotUnavailable,
                               "Trashed Data Segment is corrupt");
      }
      std::filesystem::rename(trashedPath, path);
      continue;
    }
    auto written = writeBytes(path, segment.bytes);
    if (!written.ok()) {
      return written.error();
    }
  }
  return {};
}

core::Result<std::unique_ptr<SegmentRetentionStore>>
pinSnapshotSegments(const std::filesystem::path &storeDirectory,
                    const std::string &snapshotId,
                    const std::vector<EncodedSegment> &segments) {
  auto retention = std::unique_ptr<SegmentRetentionStore>{};
  if (segments.empty()) {
    return retention;
  }
  std::vector<std::string> segmentIds;
  segmentIds.reserve(segments.size());
  for (const auto &segment : segments) {
    segmentIds.push_back(segment.metadata.id);
  }
  std::ranges::sort(segmentIds);
  const auto uniqueSegmentIds = std::ranges::unique(segmentIds);
  segmentIds.erase(uniqueSegmentIds.begin(), uniqueSegmentIds.end());
  auto opened = SegmentRetentionStore::open(storeDirectory);
  if (!opened.ok()) {
    return opened.error();
  }
  retention = std::move(opened).value();
  const auto pinned = retention->acquire("snapshot-" + snapshotId, segmentIds);
  if (!pinned.ok()) {
    return pinned.error();
  }
  return retention;
}

core::Result<void>
releaseUnpublishedSnapshotPin(SegmentRetentionStore *retention,
                              const std::filesystem::path &finalDirectory,
                              const std::string &snapshotId) {
  if (retention == nullptr) {
    return {};
  }
  std::error_code error;
  const auto published = std::filesystem::exists(finalDirectory, error);
  if (error) {
    return core::makeError(core::ErrorCode::permissionDenied,
                           "Unable to inspect snapshot publication: " +
                               error.message());
  }
  if (published) {
    return {};
  }
  return retention->abandonUnpublishedSnapshot(snapshotId);
}

struct SnapshotManifestPublication final {
  std::filesystem::path snapshotDirectory;
  std::filesystem::path finalDirectory;
  std::string_view snapshotId;
  std::string_view manifest;
};

[[nodiscard]] core::Result<std::string>
readValidatedSnapshotManifest(const std::filesystem::path &directory,
                              const std::string &snapshotId);

core::Result<void>
validateExistingSnapshotManifest(const std::filesystem::path &directory,
                                 const std::string &snapshotId,
                                 const std::string_view expectedManifest) {
  auto existing = readValidatedSnapshotManifest(directory, snapshotId);
  if (!existing.ok()) {
    return existing.error();
  }
  if (existing.value() != expectedManifest) {
    return core::makeError(core::ErrorCode::dataSnapshotUnavailable,
                           "Existing Release Snapshot manifest is invalid");
  }
  return {};
}

core::Result<void>
publishSnapshotManifest(const SnapshotManifestPublication &publication) {
  std::error_code error;
  if (std::filesystem::exists(publication.finalDirectory, error)) {
    return validateExistingSnapshotManifest(publication.finalDirectory,
                                            std::string{publication.snapshotId},
                                            publication.manifest);
  }
  if (error) {
    return core::makeError(core::ErrorCode::permissionDenied,
                           "Unable to inspect snapshot publication: " +
                               error.message());
  }
  const auto stagingDirectory =
      publication.snapshotDirectory /
      ("." + std::string{publication.snapshotId} + ".staging");
  std::filesystem::remove_all(stagingDirectory, error);
  if (error) {
    return core::makeError(core::ErrorCode::permissionDenied,
                           "Unable to clear snapshot staging directory: " +
                               error.message());
  }
  std::filesystem::create_directories(stagingDirectory, error);
  if (error) {
    return core::makeError(core::ErrorCode::permissionDenied,
                           "Unable to create snapshot staging directory: " +
                               error.message());
  }
  std::ofstream output{stagingDirectory / "Manifest.btesnapshot",
                       std::ios::binary | std::ios::trunc};
  output << publication.manifest;
  output.close();
  if (consumeFailure(testing::SnapshotFailurePoint::manifestWrite) || !output) {
    std::filesystem::remove_all(stagingDirectory, error);
    return core::makeError(core::ErrorCode::permissionDenied,
                           "Unable to write snapshot manifest");
  }
  std::filesystem::rename(stagingDirectory, publication.finalDirectory, error);
  if (error) {
    return core::makeError(core::ErrorCode::permissionDenied,
                           "Unable to publish snapshot manifest: " +
                               error.message());
  }
  return {};
}

core::Result<void>
resolveUnpublishedSnapshotFailure(SegmentRetentionStore *retention,
                                  const std::filesystem::path &finalDirectory,
                                  const std::string &snapshotId,
                                  core::Error failure) {
  auto released =
      releaseUnpublishedSnapshotPin(retention, finalDirectory, snapshotId);
  if (!released.ok()) {
    return released.error();
  }
  return core::Result<void>{std::move(failure)};
}

core::Result<void>
publishSnapshot(const SnapshotBuildRequest &request,
                const std::string &snapshotId, const std::string &manifest,
                const std::vector<EncodedSegment> &segments) {
  auto publication = lockSnapshotPublication(request.storeDirectory);
  if (!publication.ok()) {
    return publication.error();
  }
  auto recovered = recoverUnpublishedSnapshotPins(request.storeDirectory);
  if (!recovered.ok()) {
    return recovered.error();
  }
  const auto snapshotDirectory = request.storeDirectory / "Snapshots";
  const auto finalDirectory = snapshotDirectory / snapshotId;
  std::error_code filesystemError;
  if (std::filesystem::exists(finalDirectory, filesystemError)) {
    auto validated =
        validateExistingSnapshotManifest(finalDirectory, snapshotId, manifest);
    if (!validated.ok()) {
      return validated.error();
    }
  }
  if (filesystemError) {
    return core::makeError(core::ErrorCode::permissionDenied,
                           "Unable to inspect snapshot publication: " +
                               filesystemError.message());
  }
  auto retentionStore = std::unique_ptr<SegmentRetentionStore>{};
  try {
    auto written = writeSnapshotSegments(request, segments);
    if (!written.ok()) {
      auto artifactCleanup =
          recoverUnpublishedSnapshotPins(request.storeDirectory);
      if (!artifactCleanup.ok()) {
        return artifactCleanup.error();
      }
      return written.error();
    }
    auto retention =
        pinSnapshotSegments(request.storeDirectory, snapshotId, segments);
    if (!retention.ok()) {
      auto pinFailureCleanup =
          recoverUnpublishedSnapshotPins(request.storeDirectory);
      if (!pinFailureCleanup.ok()) {
        return pinFailureCleanup.error();
      }
      return retention.error();
    }
    retentionStore = std::move(retention).value();
    auto published =
        publishSnapshotManifest({.snapshotDirectory = snapshotDirectory,
                                 .finalDirectory = finalDirectory,
                                 .snapshotId = snapshotId,
                                 .manifest = manifest});
    if (!published.ok()) {
      return resolveUnpublishedSnapshotFailure(
          retentionStore.get(), finalDirectory, snapshotId, published.error());
    }
  } catch (const std::filesystem::filesystem_error &error) {
    return resolveUnpublishedSnapshotFailure(
        retentionStore.get(), finalDirectory, snapshotId,
        core::makeError(core::ErrorCode::permissionDenied,
                        "Unable to publish Release Snapshot: " +
                            std::string{error.what()}));
  }
  return {};
}

core::Result<std::string>
readValidatedSnapshotManifest(const std::filesystem::path &directory,
                              const std::string &snapshotId) {
  std::ifstream input{directory / "Manifest.btesnapshot", std::ios::binary};
  if (!input) {
    return core::makeError(core::ErrorCode::notFound,
                           "Release Snapshot manifest is unavailable");
  }
  std::string manifest{std::istreambuf_iterator<char>{input},
                       std::istreambuf_iterator<char>{}};
  if (input.bad() || core::sha256(manifest) != snapshotId) {
    return core::makeError(core::ErrorCode::dataSnapshotUnavailable,
                           "Release Snapshot manifest hash is invalid");
  }
  return manifest;
}

core::Result<std::vector<std::string>>
snapshotSegmentIds(const std::string &manifest) {
  std::string calendarHash;
  std::string splitHash;
  auto metadata = parseManifest(manifest, calendarHash, splitHash);
  if (!metadata.ok()) {
    return metadata.error();
  }
  std::vector<std::string> segmentIds;
  for (const auto &segment : metadata.value()) {
    segmentIds.push_back(segment.id);
  }
  std::ranges::sort(segmentIds);
  const auto uniqueSegmentIds = std::ranges::unique(segmentIds);
  segmentIds.erase(uniqueSegmentIds.begin(), uniqueSegmentIds.end());
  return segmentIds;
}

struct SnapshotRemovalStage final {
  std::filesystem::path stagedDirectory;
  bool alreadyArchived = false;
};

struct SnapshotRemovalRequest final {
  std::filesystem::path directory;
  std::filesystem::path archiveRoot;
  std::string_view snapshotId;
  std::string_view manifest;
};

core::Result<SnapshotRemovalStage>
stageSnapshotRemoval(const SnapshotRemovalRequest &request) {
  std::error_code error;
  std::filesystem::create_directories(request.archiveRoot, error);
  if (error) {
    return core::makeError(core::ErrorCode::permissionDenied,
                           "Unable to prepare snapshot archive: " +
                               error.message());
  }
  const auto archived = request.archiveRoot / request.snapshotId;
  const auto alreadyArchived = std::filesystem::exists(archived, error);
  if (error) {
    return core::makeError(core::ErrorCode::permissionDenied,
                           "Unable to inspect snapshot archive: " +
                               error.message());
  }
  if (alreadyArchived) {
    std::ifstream previous{archived / "Manifest.btesnapshot", std::ios::binary};
    const std::string archivedManifest{std::istreambuf_iterator<char>{previous},
                                       std::istreambuf_iterator<char>{}};
    if (!previous || archivedManifest != request.manifest) {
      return core::makeError(core::ErrorCode::dataSnapshotUnavailable,
                             "Existing snapshot archive is invalid");
    }
  }
  const auto stagedDirectory =
      alreadyArchived
          ? request.directory.parent_path() /
                ("." + std::string{request.snapshotId} + ".removing")
          : archived;
  const auto staleStageExists = std::filesystem::exists(stagedDirectory, error);
  if (error) {
    return core::makeError(core::ErrorCode::permissionDenied,
                           "Unable to inspect snapshot removal staging: " +
                               error.message());
  }
  if (staleStageExists) {
    if (!alreadyArchived) {
      return core::makeError(core::ErrorCode::resourceBusy,
                             "Snapshot removal is already in progress");
    }
    auto staleManifest = readValidatedSnapshotManifest(
        stagedDirectory, std::string{request.snapshotId});
    if (!staleManifest.ok() || staleManifest.value() != request.manifest) {
      return core::makeError(core::ErrorCode::resourceBusy,
                             "Snapshot removal is already in progress");
    }
    std::filesystem::remove_all(stagedDirectory, error);
    if (error) {
      return core::makeError(
          core::ErrorCode::permissionDenied,
          "Unable to clear stale snapshot removal staging: " + error.message());
    }
  }
  std::filesystem::rename(request.directory, stagedDirectory, error);
  if (error) {
    return core::makeError(core::ErrorCode::permissionDenied,
                           "Unable to stage snapshot removal: " +
                               error.message());
  }
  return SnapshotRemovalStage{.stagedDirectory = stagedDirectory,
                              .alreadyArchived = alreadyArchived};
}

core::Result<void> completeSnapshotRemoval(
    SegmentRetentionStore &retention, const std::string &pinId,
    const std::filesystem::path &directory, const SnapshotRemovalStage &stage) {
  const auto released = retention.release(pinId);
  if (!released.ok()) {
    std::error_code rollbackError;
    if (consumeFailure(
            testing::SnapshotFailurePoint::snapshotRemovalRollback)) {
      rollbackError = std::make_error_code(std::errc::permission_denied);
    } else {
      std::filesystem::rename(stage.stagedDirectory, directory, rollbackError);
    }
    if (rollbackError) {
      return core::makeError(
          core::ErrorCode::internal,
          "Unable to restore snapshot after removal failure: " +
              rollbackError.message());
    }
    return released.error();
  }
  if (stage.alreadyArchived) {
    std::error_code error;
    std::filesystem::remove_all(stage.stagedDirectory, error);
    if (error) {
      return core::makeError(core::ErrorCode::permissionDenied,
                             "Unable to clean up removed snapshot: " +
                                 error.message());
    }
  }
  return {};
}

} // namespace

core::Result<std::unordered_set<std::string>>
publishedSnapshotSegments(const std::filesystem::path &storeDirectory) {
  std::unordered_set<std::string> segments;
  const auto directory = storeDirectory / "Snapshots";
  std::error_code error;
  if (!std::filesystem::exists(directory, error) && !error) {
    return segments;
  }
  auto entry = std::filesystem::directory_iterator{directory, error};
  if (error) {
    return core::makeError(core::ErrorCode::dataSnapshotUnavailable,
                           "Unable to inspect published snapshots: " +
                               error.message());
  }
  for (; entry != std::filesystem::directory_iterator{};
       entry.increment(error)) {
    if (error) {
      return core::makeError(core::ErrorCode::dataSnapshotUnavailable,
                             "Unable to inspect published snapshots: " +
                                 error.message());
    }
    const auto snapshotId = entry->path().filename().string();
    if (!validHash(snapshotId)) {
      continue;
    }
    const auto status = entry->symlink_status(error);
    if (error || !std::filesystem::is_directory(status)) {
      return core::makeError(core::ErrorCode::dataSnapshotUnavailable,
                             "Published snapshot directory is unavailable");
    }
    std::ifstream input{entry->path() / "Manifest.btesnapshot",
                        std::ios::binary};
    if (!input) {
      return core::makeError(core::ErrorCode::dataSnapshotUnavailable,
                             "Published snapshot manifest is unavailable");
    }
    const std::string manifest{std::istreambuf_iterator<char>{input},
                               std::istreambuf_iterator<char>{}};
    if (input.bad() || core::sha256(manifest) != snapshotId) {
      return core::makeError(core::ErrorCode::dataSnapshotUnavailable,
                             "Published snapshot manifest hash is invalid");
    }
    std::string calendarHash;
    std::string splitHash;
    const auto parsed = parseManifest(manifest, calendarHash, splitHash);
    if (!parsed.ok()) {
      return parsed.error();
    }
    for (const auto &segment : parsed.value()) {
      if (!validHash(segment.id)) {
        return core::makeError(
            core::ErrorCode::dataSnapshotUnavailable,
            "Published snapshot segment identity is invalid");
      }
      segments.insert(segment.id);
    }
  }
  if (error) {
    return core::makeError(core::ErrorCode::dataSnapshotUnavailable,
                           "Unable to inspect published snapshots: " +
                               error.message());
  }
  return segments;
}

#if defined(BTE_ENABLE_TEST_HOOKS)
namespace testing {

void failSnapshotAt(const SnapshotFailurePoint point) noexcept {
  snapshotFailurePoint().store(point);
}

void clearSnapshotFailure() noexcept {
  snapshotFailurePoint().store(SnapshotFailurePoint::none);
}

void resetSnapshotSegmentReadCount() noexcept {
  snapshotSegmentReads().store(0);
}

std::size_t snapshotSegmentReadCount() noexcept {
  return snapshotSegmentReads().load();
}

} // namespace testing
#endif

struct ReleaseSnapshotReader::Impl {
  std::string snapshotId;
  std::string calendarHash;
  std::string splitManifestHash;
  std::vector<LoadedSegment> segments;
  bool exactOnly = false;
};

core::Result<std::vector<std::string>>
validatedSnapshotSymbols(const SnapshotBuildRequest &request,
                         const core::CancellationToken &cancellation) {
  if (cancellation.isCancellationRequested()) {
    return core::makeError(core::ErrorCode::cancelled,
                           "Snapshot build was cancelled");
  }
  if (request.sourceDirectory.empty() || request.storeDirectory.empty() ||
      request.symbols.empty() || request.rowsPerSegment == 0 ||
      !validHash(request.calendarHash) ||
      !validHash(request.splitManifestHash)) {
    return core::makeError(core::ErrorCode::invalidArgument,
                           "Snapshot build request is invalid");
  }

  auto symbols = request.symbols;
  std::ranges::sort(symbols);
  if (std::ranges::adjacent_find(symbols) != symbols.end()) {
    return core::makeError(core::ErrorCode::invalidArgument,
                           "Snapshot symbols must be unique");
  }
  return symbols;
}

core::Result<SnapshotBuildResult>
buildReleaseSnapshot(const SnapshotBuildRequest &request,
                     const core::CancellationToken &cancellation) {
  auto symbols = validatedSnapshotSymbols(request, cancellation);
  if (!symbols.ok()) {
    return symbols.error();
  }
  auto sourceManifest = readSourceManifest(request.sourceDirectory);
  if (!sourceManifest.ok()) {
    return sourceManifest.error();
  }

  auto encoded = encodeSnapshotSegments(request, symbols.value(),
                                        sourceManifest.value(), cancellation);
  if (!encoded.ok()) {
    return encoded.error();
  }
  const auto metadata = snapshotSegmentMetadata(encoded.value().segments);
  const auto manifest = makeManifest(request, metadata);
  const auto snapshotId = core::sha256(manifest);
  auto published =
      publishSnapshot(request, snapshotId, manifest, encoded.value().segments);
  if (!published.ok()) {
    return published.error();
  }

  SnapshotBuildResult result{
      .snapshotId = snapshotId,
      .segments = {},
      .segmentCount = encoded.value().segments.size(),
      .barCount = encoded.value().barCount,
  };
  result.segments.reserve(encoded.value().segments.size());
  for (const auto &segment : encoded.value().segments) {
    result.segments.push_back(segment.metadata.id);
  }
  return result;
}

core::Result<void>
removeReleaseSnapshot(const std::filesystem::path &storeDirectory,
                      const std::string &snapshotId) {
  if (storeDirectory.empty() || !validHash(snapshotId)) {
    return core::makeError(core::ErrorCode::invalidArgument,
                           "Snapshot removal request is invalid");
  }
  auto publication = lockSnapshotPublication(storeDirectory);
  if (!publication.ok()) {
    return publication.error();
  }
  auto recovered = recoverUnpublishedSnapshotPins(storeDirectory);
  if (!recovered.ok()) {
    return recovered.error();
  }
  const auto directory = storeDirectory / "Snapshots" / snapshotId;
  auto manifest = readValidatedSnapshotManifest(directory, snapshotId);
  if (!manifest.ok()) {
    return manifest.error();
  }
  auto segmentIds = snapshotSegmentIds(manifest.value());
  if (!segmentIds.ok()) {
    return segmentIds.error();
  }
  auto retention = SegmentRetentionStore::open(storeDirectory);
  if (!retention.ok()) {
    return retention.error();
  }
  const auto pinId = "snapshot-" + snapshotId;
  if (!segmentIds.value().empty()) {
    const auto pinned = retention.value()->acquire(pinId, segmentIds.value());
    if (!pinned.ok()) {
      return pinned.error();
    }
  }
  const auto archiveRoot = storeDirectory / "ArchivedSnapshots";
  auto staged = stageSnapshotRemoval({.directory = directory,
                                      .archiveRoot = archiveRoot,
                                      .snapshotId = snapshotId,
                                      .manifest = manifest.value()});
  if (!staged.ok()) {
    return staged.error();
  }
  return completeSnapshotRemoval(*retention.value(), pinId, directory,
                                 staged.value());
}

ReleaseSnapshotReader::ReleaseSnapshotReader(ConstructionKey,
                                             std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}

ReleaseSnapshotReader::~ReleaseSnapshotReader() = default;

core::Result<std::unique_ptr<ReleaseSnapshotReader>>
ReleaseSnapshotReader::open(const std::filesystem::path &storeDirectory,
                            const std::string &snapshotId,
                            const core::CancellationToken &cancellation) {
  return openImpl(storeDirectory, snapshotId, nullptr, cancellation);
}

core::Result<std::unique_ptr<ReleaseSnapshotReader>>
ReleaseSnapshotReader::openExact(const std::filesystem::path &storeDirectory,
                                 const DataSelectionIdentity &identity,
                                 const core::CancellationToken &cancellation) {
  auto reader =
      openImpl(storeDirectory, identity.snapshotId, &identity, cancellation);
  if (!reader.ok()) {
    return reader.error();
  }
  const auto checked = reader.value()->validateExact(identity, cancellation);
  if (!checked.ok()) {
    return checked.error();
  }
  return std::move(reader).value();
}

core::Result<std::unique_ptr<ReleaseSnapshotReader>>
ReleaseSnapshotReader::openImpl(const std::filesystem::path &storeDirectory,
                                const std::string &snapshotId,
                                const DataSelectionIdentity *identity,
                                const core::CancellationToken &cancellation) {
  if (cancellation.isCancellationRequested()) {
    return core::makeError(core::ErrorCode::cancelled,
                           "Snapshot open was cancelled");
  }
  if (storeDirectory.empty() || !validHash(snapshotId)) {
    return core::makeError(core::ErrorCode::invalidArgument,
                           "Snapshot open request is invalid");
  }

  auto manifestPath =
      storeDirectory / "Snapshots" / snapshotId / "Manifest.btesnapshot";
  std::ifstream input{manifestPath, std::ios::binary};
  if (!input && identity != nullptr) {
    manifestPath = storeDirectory / "ArchivedSnapshots" / snapshotId /
                   "Manifest.btesnapshot";
    input = std::ifstream{manifestPath, std::ios::binary};
  }
  if (!input) {
    return core::makeError(core::ErrorCode::dataSnapshotUnavailable,
                           "Release Snapshot manifest is unavailable");
  }
  const std::string manifest{std::istreambuf_iterator<char>{input},
                             std::istreambuf_iterator<char>{}};
  if (core::sha256(manifest) != snapshotId) {
    return core::makeError(core::ErrorCode::dataSnapshotUnavailable,
                           "Release Snapshot manifest hash is invalid");
  }

  auto impl = std::make_unique<Impl>();
  impl->snapshotId = snapshotId;
  impl->exactOnly = identity != nullptr;
  auto metadata =
      parseManifest(manifest, impl->calendarHash, impl->splitManifestHash);
  if (!metadata.ok()) {
    return metadata.error();
  }

  std::optional<std::pair<std::string, core::Timestamp>> previous;
  for (const auto &segment : metadata.value()) {
    if (cancellationRequested(
            cancellation, testing::SnapshotFailurePoint::openCancellation)) {
      return core::makeError(core::ErrorCode::cancelled,
                             "Snapshot open was cancelled");
    }
    if (identity != nullptr &&
        std::ranges::none_of(identity->spans, [&segment](const auto &span) {
          return span.segmentOrdinal == segment.ordinal;
        })) {
      impl->segments.push_back(LoadedSegment{.metadata = segment, .bars = {}});
      continue;
    }
#if defined(BTE_ENABLE_TEST_HOOKS)
    snapshotSegmentReads().fetch_add(1);
#endif
    auto bytes =
        readBytes(storeDirectory / "Segments" / (segment.id + ".btedata"));
    if (!bytes.ok() || core::sha256(bytes.value()) != segment.id) {
      return core::makeError(core::ErrorCode::dataSnapshotUnavailable,
                             "Data Segment hash is invalid");
    }
    auto bars = decodeSegment(bytes.value(), segment.symbol, segment.rowCount);
    if (!bars.ok() ||
        bars.value().front().timestamp != segment.firstTimestamp ||
        bars.value().back().timestamp != segment.lastTimestamp) {
      return core::makeError(core::ErrorCode::dataSnapshotUnavailable,
                             "Data Segment rows do not match manifest");
    }
    if (previous.has_value() && previous->first == segment.symbol &&
        previous->second >= bars.value().front().timestamp) {
      return core::makeError(core::ErrorCode::dataSnapshotUnavailable,
                             "Data Segments are out of order");
    }
    previous = std::pair{segment.symbol, bars.value().back().timestamp};
    impl->segments.push_back(
        LoadedSegment{.metadata = segment, .bars = std::move(bars).value()});
  }
  return std::make_unique<ReleaseSnapshotReader>(ConstructionKey{},
                                                 std::move(impl));
}

core::Result<SnapshotSelection> ReleaseSnapshotReader::select(
    const SnapshotSelectRequest &request,
    const core::CancellationToken &cancellation) const {
  if (impl_->exactOnly) {
    return core::makeError(
        core::ErrorCode::invalidArgument,
        "Exact snapshot reader does not support range selection");
  }
  if (cancellation.isCancellationRequested()) {
    return core::makeError(core::ErrorCode::cancelled,
                           "Snapshot selection was cancelled");
  }
  if (request.symbols.empty() || request.range.start >= request.range.end) {
    return core::makeError(core::ErrorCode::invalidArgument,
                           "Snapshot selection request is invalid");
  }
  if (request.timeframe != hourlyTimeframe) {
    return core::makeError(core::ErrorCode::schemaMismatch,
                           "Only ohlcv-1h snapshot selection is supported");
  }
  const std::unordered_set<std::string> symbols{request.symbols.begin(),
                                                request.symbols.end()};
  if (symbols.size() != request.symbols.size()) {
    return core::makeError(core::ErrorCode::invalidArgument,
                           "Snapshot selection symbols must be unique");
  }

  SnapshotSelection selection{
      .bars = {},
      .identity = {.snapshotId = impl_->snapshotId,
                   .calendarHash = impl_->calendarHash,
                   .splitManifestHash = impl_->splitManifestHash,
                   .timeframe = std::string{hourlyTimeframe},
                   .profile = std::string{snapshotProfile},
                   .spans = {}},
      .preRangeDataDayClose = std::nullopt,
      .preRangeDataDays = {},
  };
  for (const auto &segment : impl_->segments) {
    if (!symbols.contains(segment.metadata.symbol)) {
      continue;
    }
    const auto first = std::ranges::lower_bound(
        segment.bars, request.range.start, {}, &SnapshotBar::timestamp);
    const auto last = std::ranges::lower_bound(segment.bars, request.range.end,
                                               {}, &SnapshotBar::timestamp);
    if (first == last) {
      continue;
    }
    const auto firstRow =
        static_cast<std::size_t>(std::distance(segment.bars.begin(), first));
    const auto rowCount = static_cast<std::size_t>(std::distance(first, last));
    selection.bars.insert(selection.bars.end(), first, last);
    selection.identity.spans.push_back(DataSegmentSpan{
        .segmentOrdinal = segment.metadata.ordinal,
        .symbol = segment.metadata.symbol,
        .segmentId = segment.metadata.id,
        .segmentHash = segment.metadata.id,
        .firstRow = firstRow,
        .rowCount = rowCount,
        .firstTimestamp = first->timestamp,
        .lastTimestamp = std::prev(last)->timestamp,
    });
  }
  std::ranges::sort(selection.bars,
                    [](const SnapshotBar &left, const SnapshotBar &right) {
                      return left.timestamp < right.timestamp;
                    });
  return selection;
}

core::Result<std::optional<SnapshotHistoryClose>>
ReleaseSnapshotReader::lastCloseBeforeUtcDate(
    const std::string_view symbol, const core::Timestamp utcDateStart,
    const core::CancellationToken &cancellation) const {
  if (impl_->exactOnly) {
    return core::makeError(core::ErrorCode::invalidArgument,
                           "Exact snapshot reader does not support history");
  }
  if (cancellation.isCancellationRequested()) {
    return core::makeError(core::ErrorCode::cancelled,
                           "Snapshot history lookup was cancelled");
  }
  if (std::chrono::floor<std::chrono::days>(utcDateStart) != utcDateStart) {
    return core::makeError(core::ErrorCode::invalidArgument,
                           "Snapshot history boundary must be midnight UTC");
  }

  std::optional<SnapshotHistoryClose> result;
  for (const auto &segment : impl_->segments) {
    if (cancellationRequested(
            cancellation,
            testing::SnapshotFailurePoint::historyLookupCancellation)) {
      return core::makeError(core::ErrorCode::cancelled,
                             "Snapshot history lookup was cancelled");
    }
    if (segment.metadata.symbol != symbol) {
      continue;
    }

    const auto firstAtOrAfterBoundary = std::ranges::lower_bound(
        segment.bars, utcDateStart, {}, &SnapshotBar::timestamp);
    if (firstAtOrAfterBoundary == segment.bars.begin()) {
      continue;
    }

    const auto &bar = *std::prev(firstAtOrAfterBoundary);
    if (!result.has_value() || result->timestamp < bar.timestamp) {
      result = SnapshotHistoryClose{
          .timestamp = bar.timestamp,
          .closeNanodollars = bar.closeNanodollars,
      };
    }
  }
  return result;
}

core::Result<std::vector<SnapshotDataDaySummary>>
ReleaseSnapshotReader::lastDataDaysBeforeUtcDate(
    const std::string_view symbol, const core::Timestamp utcDateStart,
    const std::size_t count,
    const core::CancellationToken &cancellation) const {
  if (impl_->exactOnly) {
    return core::makeError(core::ErrorCode::invalidArgument,
                           "Exact snapshot reader does not support history");
  }
  if (cancellation.isCancellationRequested()) {
    return core::makeError(core::ErrorCode::cancelled,
                           "Snapshot history lookup was cancelled");
  }
  if (count == 0 || count > maximumSnapshotHistoryDataDays) {
    return core::makeError(core::ErrorCode::invalidArgument,
                           "Snapshot history day count is outside its limit");
  }
  if (std::chrono::floor<std::chrono::days>(utcDateStart) != utcDateStart) {
    return core::makeError(core::ErrorCode::invalidArgument,
                           "Snapshot history boundary must be midnight UTC");
  }

  std::vector<SnapshotDataDaySummary> summaries;
  bool reachedOlderDate = false;
  for (auto segment = impl_->segments.rbegin();
       segment != impl_->segments.rend() && !reachedOlderDate; ++segment) {
    if (cancellationRequested(
            cancellation,
            testing::SnapshotFailurePoint::historyLookupCancellation)) {
      return core::makeError(core::ErrorCode::cancelled,
                             "Snapshot history lookup was cancelled");
    }
    if (segment->metadata.symbol != symbol) {
      continue;
    }

    const auto firstAtOrAfterBoundary = std::ranges::lower_bound(
        segment->bars, utcDateStart, {}, &SnapshotBar::timestamp);
    auto bar = firstAtOrAfterBoundary;
    while (bar != segment->bars.begin()) {
      if (cancellationRequested(
              cancellation,
              testing::SnapshotFailurePoint::historyDataDayScanCancellation)) {
        return core::makeError(core::ErrorCode::cancelled,
                               "Snapshot history lookup was cancelled");
      }
      --bar;
      const auto utcDateStartForBar =
          std::chrono::floor<std::chrono::days>(bar->timestamp);
      if (summaries.empty() ||
          summaries.back().utcDateStart != utcDateStartForBar) {
        if (summaries.size() == count) {
          reachedOlderDate = true;
          break;
        }
        summaries.push_back(SnapshotDataDaySummary{
            .utcDateStart = utcDateStartForBar,
            .highNanodollars = bar->highNanodollars,
            .lowNanodollars = bar->lowNanodollars,
            .closeNanodollars = bar->closeNanodollars,
        });
        continue;
      }
      auto &summary = summaries.back();
      summary.highNanodollars =
          std::max(summary.highNanodollars, bar->highNanodollars);
      summary.lowNanodollars =
          std::min(summary.lowNanodollars, bar->lowNanodollars);
    }
  }
  std::ranges::reverse(summaries);
  return summaries;
}

core::Result<SnapshotSelection> ReleaseSnapshotReader::readExact(
    const DataSelectionIdentity &identity,
    const core::CancellationToken &cancellation) const {
  const auto validated = validateExact(identity, cancellation);
  if (!validated.ok()) {
    return validated.error();
  }
  SnapshotSelection selection{
      .bars = {},
      .identity = identity,
      .preRangeDataDayClose = std::nullopt,
      .preRangeDataDays = {},
  };
  std::size_t selectedRowCount = 0;
  for (const auto &span : identity.spans) {
    if (span.rowCount >
        std::numeric_limits<std::size_t>::max() - selectedRowCount) {
      return core::makeError(core::ErrorCode::dataSnapshotUnavailable,
                             "Persisted Data Selection row count overflows");
    }
    selectedRowCount += span.rowCount;
  }
  selection.bars.reserve(selectedRowCount);
  for (const auto &span : identity.spans) {
    if (cancellationRequested(
            cancellation,
            testing::SnapshotFailurePoint::exactReadCancellation)) {
      return core::makeError(core::ErrorCode::cancelled,
                             "Exact snapshot read was cancelled");
    }
    const auto &segment = impl_->segments[span.segmentOrdinal];
    const auto first =
        segment.bars.begin() + static_cast<std::ptrdiff_t>(span.firstRow);
    const auto last = first + static_cast<std::ptrdiff_t>(span.rowCount);
    selection.bars.insert(selection.bars.end(), first, last);
  }
  std::ranges::sort(selection.bars,
                    [](const SnapshotBar &left, const SnapshotBar &right) {
                      return left.timestamp < right.timestamp;
                    });
  return selection;
}

core::Result<void> ReleaseSnapshotReader::validateExact(
    const DataSelectionIdentity &identity,
    const core::CancellationToken &cancellation) const {
  if (cancellation.isCancellationRequested()) {
    return core::makeError(core::ErrorCode::cancelled,
                           "Exact snapshot read was cancelled");
  }
  if (identity.snapshotId != impl_->snapshotId ||
      identity.calendarHash != impl_->calendarHash ||
      identity.splitManifestHash != impl_->splitManifestHash ||
      identity.timeframe != hourlyTimeframe ||
      identity.profile != snapshotProfile || identity.spans.empty()) {
    return core::makeError(core::ErrorCode::dataSnapshotUnavailable,
                           "Persisted Data Selection identity is unavailable");
  }

  std::optional<std::size_t> previousOrdinal;
  for (const auto &span : identity.spans) {
    if (cancellation.isCancellationRequested()) {
      return core::makeError(core::ErrorCode::cancelled,
                             "Exact snapshot read was cancelled");
    }
    if (span.segmentOrdinal >= impl_->segments.size() ||
        (previousOrdinal.has_value() &&
         *previousOrdinal >= span.segmentOrdinal)) {
      return core::makeError(core::ErrorCode::dataSnapshotUnavailable,
                             "Persisted Data Segment span order is invalid");
    }
    const auto &segment = impl_->segments[span.segmentOrdinal];
    if (span.symbol != segment.metadata.symbol ||
        span.segmentId != segment.metadata.id ||
        span.segmentHash != segment.metadata.id || span.rowCount == 0 ||
        span.firstRow > segment.bars.size() ||
        span.rowCount > segment.bars.size() - span.firstRow) {
      return core::makeError(core::ErrorCode::dataSnapshotUnavailable,
                             "Persisted Data Segment span is unavailable");
    }
    const auto first =
        segment.bars.begin() + static_cast<std::ptrdiff_t>(span.firstRow);
    const auto last = first + static_cast<std::ptrdiff_t>(span.rowCount);
    if (first->timestamp != span.firstTimestamp ||
        std::prev(last)->timestamp != span.lastTimestamp) {
      return core::makeError(core::ErrorCode::dataSnapshotUnavailable,
                             "Persisted Data Segment row identity is invalid");
    }
    previousOrdinal = span.segmentOrdinal;
  }
  return {};
}

ReleaseSnapshotSummary ReleaseSnapshotReader::summary() const {
  ReleaseSnapshotSummary result{
      .snapshotId = impl_->snapshotId,
      .profile = std::string{snapshotProfile},
      .timeframe = std::string{hourlyTimeframe},
      .symbols = {},
  };
  for (const auto &segment : impl_->segments) {
    result.symbols.push_back(segment.metadata.symbol);
    result.barCount += segment.metadata.rowCount;
  }
  std::ranges::sort(result.symbols);
  result.symbols.erase(
      std::unique(result.symbols.begin(), result.symbols.end()),
      result.symbols.end());
  return result;
}

} // namespace bte::data
