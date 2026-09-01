#include "Bte/Data/ReleaseSnapshot.h"

#include "Bte/Core/Digest.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace bte::data {
namespace {

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
      fractionalText.size() > static_cast<std::size_t>(fractionalDigits)) {
    return core::makeError(core::ErrorCode::invalidArgument,
                           std::string{field} + " has unsupported precision");
  }

  auto integer = parseInteger(integerText, field);
  if (!integer.ok() || integer.value() < 0) {
    return core::makeError(core::ErrorCode::invalidArgument,
                           std::string{field} + " is invalid");
  }
  fractionalText.append(
      static_cast<std::size_t>(fractionalDigits) - fractionalText.size(), '0');
  auto fractional = fractionalText.empty()
                        ? core::Result<std::int64_t>{std::int64_t{0}}
                        : parseInteger(fractionalText, field);
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

core::Result<std::vector<SnapshotBar>>
readSourceBars(const std::filesystem::path &path, const std::string &symbol,
               const core::CancellationToken &cancellation) {
  std::ifstream input{path};
  if (!input) {
    return core::makeError(core::ErrorCode::notFound,
                           "Snapshot source not found: " + path.string());
  }

  std::string line;
  if (!std::getline(input, line)) {
    return core::makeError(core::ErrorCode::schemaMismatch,
                           "Snapshot source is empty: " + path.string());
  }
  const auto header = split(line, ',');
  constexpr std::array required{"symbol", "ts",    "open",   "high",
                                "low",    "close", "volume", "schemaName"};
  if (header.size() < required.size() ||
      !std::ranges::equal(required, std::span{header}.first(required.size()))) {
    return core::makeError(core::ErrorCode::schemaMismatch,
                           "Snapshot source has unsupported columns");
  }

  std::vector<SnapshotBar> bars;
  while (std::getline(input, line)) {
    if (cancellation.isCancellationRequested()) {
      return core::makeError(core::ErrorCode::cancelled,
                             "Snapshot build was cancelled");
    }
    if (trim(line).empty()) {
      continue;
    }
    const auto fields = split(line, ',');
    if (fields.size() < required.size() || fields[0] != symbol ||
        fields[7] != hourlyTimeframe) {
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

    SnapshotBar bar{
        .timestamp = parsedTimestamp.value(),
        .openNanodollars = open.value(),
        .highNanodollars = high.value(),
        .lowNanodollars = low.value(),
        .closeNanodollars = close.value(),
        .volumeMicroshares = volume.value(),
    };
    if (bar.openNanodollars <= 0 || bar.lowNanodollars <= 0 ||
        bar.highNanodollars <
            std::max({bar.openNanodollars, bar.closeNanodollars,
                      bar.lowNanodollars}) ||
        bar.lowNanodollars >
            std::min({bar.openNanodollars, bar.closeNanodollars,
                      bar.highNanodollars})) {
      return core::makeError(core::ErrorCode::invalidArgument,
                             "Snapshot row contains invalid OHLCV values");
    }
    if (!bars.empty() && bars.back().timestamp >= bar.timestamp) {
      return core::makeError(core::ErrorCode::invalidArgument,
                             "Snapshot source timestamps must be increasing");
    }
    bars.push_back(bar);
  }
  return bars;
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
  std::ofstream output{path, std::ios::binary | std::ios::trunc};
  output.write(reinterpret_cast<const char *>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
  if (!output) {
    return core::makeError(core::ErrorCode::permissionDenied,
                           "Unable to write snapshot artifact: " +
                               path.string());
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
  if (size < 0) {
    return core::makeError(core::ErrorCode::dataSnapshotUnavailable,
                           "Unable to inspect Data Segment");
  }
  input.seekg(0, std::ios::beg);
  std::vector<std::byte> bytes(static_cast<std::size_t>(size));
  input.read(reinterpret_cast<char *>(bytes.data()), size);
  if (!input) {
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
    bars.push_back(SnapshotBar{
        .timestamp = core::time::fromUnixMillis(values[0]),
        .openNanodollars = values[1],
        .highNanodollars = values[2],
        .lowNanodollars = values[3],
        .closeNanodollars = values[4],
        .volumeMicroshares = values[5],
    });
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

core::Result<std::vector<SegmentMetadata>>
parseManifest(const std::string &manifest, std::string &calendarHash,
              std::string &splitManifestHash) {
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

bool validHash(const std::string &hash) {
  return hash.size() == 64 &&
         std::ranges::all_of(hash, [](const char character) {
           return (character >= '0' && character <= '9') ||
                  (character >= 'a' && character <= 'f');
         });
}

} // namespace

struct ReleaseSnapshotReader::Impl {
  std::string snapshotId;
  std::string calendarHash;
  std::string splitManifestHash;
  std::vector<LoadedSegment> segments;
};

core::Result<SnapshotBuildResult>
buildReleaseSnapshot(const SnapshotBuildRequest &request,
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

  struct EncodedSegment {
    SegmentMetadata metadata;
    std::vector<std::byte> bytes;
  };
  std::vector<EncodedSegment> encodedSegments;
  std::size_t totalBars = 0;
  for (const auto &symbol : symbols) {
    auto bars = readSourceBars(request.sourceDirectory / (symbol + ".csv"),
                               symbol, cancellation);
    if (!bars.ok()) {
      return bars.error();
    }
    totalBars += bars.value().size();
    for (std::size_t offset = 0; offset < bars.value().size();
         offset += request.rowsPerSegment) {
      const auto count =
          std::min(request.rowsPerSegment, bars.value().size() - offset);
      const auto window =
          std::span<const SnapshotBar>{bars.value()}.subspan(offset, count);
      auto bytes = encodeSegment(symbol, window);
      const auto id = core::sha256(bytes);
      encodedSegments.push_back(EncodedSegment{
          .metadata = {.ordinal = encodedSegments.size(),
                       .symbol = symbol,
                       .id = id,
                       .rowCount = count,
                       .firstTimestamp = window.front().timestamp,
                       .lastTimestamp = window.back().timestamp},
          .bytes = std::move(bytes),
      });
    }
  }

  std::vector<SegmentMetadata> metadata;
  metadata.reserve(encodedSegments.size());
  for (const auto &segment : encodedSegments) {
    metadata.push_back(segment.metadata);
  }
  const auto manifest = makeManifest(request, metadata);
  const auto snapshotId = core::sha256(manifest);

  try {
    const auto segmentDirectory = request.storeDirectory / "Segments";
    const auto snapshotDirectory = request.storeDirectory / "Snapshots";
    std::filesystem::create_directories(segmentDirectory);
    std::filesystem::create_directories(snapshotDirectory);

    for (const auto &segment : encodedSegments) {
      const auto path = segmentDirectory / (segment.metadata.id + ".btedata");
      if (std::filesystem::exists(path)) {
        auto existing = readBytes(path);
        if (!existing.ok() ||
            core::sha256(existing.value()) != segment.metadata.id) {
          return core::makeError(core::ErrorCode::dataSnapshotUnavailable,
                                 "Existing Data Segment is corrupt");
        }
        continue;
      }
      auto written = writeBytes(path, segment.bytes);
      if (!written.ok()) {
        return written.error();
      }
    }

    const auto finalDirectory = snapshotDirectory / snapshotId;
    if (!std::filesystem::exists(finalDirectory)) {
      const auto stagingDirectory =
          snapshotDirectory / ("." + snapshotId + ".staging");
      std::filesystem::remove_all(stagingDirectory);
      std::filesystem::create_directories(stagingDirectory);
      std::ofstream output{stagingDirectory / "Manifest.btesnapshot",
                           std::ios::binary | std::ios::trunc};
      output << manifest;
      output.close();
      if (!output) {
        std::filesystem::remove_all(stagingDirectory);
        return core::makeError(core::ErrorCode::permissionDenied,
                               "Unable to write snapshot manifest");
      }
      std::filesystem::rename(stagingDirectory, finalDirectory);
    }
  } catch (const std::filesystem::filesystem_error &error) {
    return core::makeError(core::ErrorCode::permissionDenied,
                           "Unable to publish Release Snapshot: " +
                               std::string{error.what()});
  }

  SnapshotBuildResult result{
      .snapshotId = snapshotId,
      .segmentCount = encodedSegments.size(),
      .barCount = totalBars,
  };
  result.segments.reserve(encodedSegments.size());
  for (const auto &segment : encodedSegments) {
    result.segments.push_back(segment.metadata.id);
  }
  return result;
}

ReleaseSnapshotReader::ReleaseSnapshotReader(ConstructionKey,
                                             std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}

ReleaseSnapshotReader::~ReleaseSnapshotReader() = default;

core::Result<std::unique_ptr<ReleaseSnapshotReader>>
ReleaseSnapshotReader::open(const std::filesystem::path &storeDirectory,
                            const std::string &snapshotId,
                            const core::CancellationToken &cancellation) {
  if (cancellation.isCancellationRequested()) {
    return core::makeError(core::ErrorCode::cancelled,
                           "Snapshot open was cancelled");
  }
  if (storeDirectory.empty() || !validHash(snapshotId)) {
    return core::makeError(core::ErrorCode::invalidArgument,
                           "Snapshot open request is invalid");
  }

  const auto manifestPath =
      storeDirectory / "Snapshots" / snapshotId / "Manifest.btesnapshot";
  std::ifstream input{manifestPath, std::ios::binary};
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
  auto metadata =
      parseManifest(manifest, impl->calendarHash, impl->splitManifestHash);
  if (!metadata.ok()) {
    return metadata.error();
  }

  std::optional<std::pair<std::string, core::Timestamp>> previous;
  for (const auto &segment : metadata.value()) {
    if (cancellation.isCancellationRequested()) {
      return core::makeError(core::ErrorCode::cancelled,
                             "Snapshot open was cancelled");
    }
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
      .identity = {.snapshotId = impl_->snapshotId,
                   .calendarHash = impl_->calendarHash,
                   .splitManifestHash = impl_->splitManifestHash,
                   .timeframe = std::string{hourlyTimeframe},
                   .profile = std::string{snapshotProfile}},
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

} // namespace bte::data
