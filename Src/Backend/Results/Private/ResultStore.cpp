#include "Bte/Results/ResultStore.h"

#include "Bte/Core/Digest.h"
#include "Bte/Core/Result.h"
#include "Bte/Core/Time.h"
#include "Bte/Data/ReleaseSnapshot.h"
#include "Bte/Data/SegmentRetention.h"

#include "ResultStoreTestHooks.h"

#include <sqlite3.h>

#include <algorithm>
#if defined(BTE_ENABLE_TEST_HOOKS)
#include <atomic>
#include <mutex>
#endif
#include <chrono>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional> // IWYU pragma: keep
#include <initializer_list>
#include <memory>
#include <optional>
#include <random>
#include <ranges> // IWYU pragma: keep
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace bte::results {
namespace {

constexpr auto schemaVersion = 2;
constexpr std::string_view numericPolicy = "fixed-point-v1";
constexpr std::string_view aggregationPolicy = "utcCalendarDayV1";

enum class DataValidationPolicy : std::uint8_t { deferToCaller, validateExact };
enum class ResultStatusPolicy : std::uint8_t { terminalOnly, allowRunning };

#if defined(BTE_ENABLE_TEST_HOOKS)
std::atomic<testing::FailurePoint> &configuredFailurePoint() {
  static std::atomic<testing::FailurePoint> value{testing::FailurePoint::none};
  return value;
}

std::atomic<std::uint32_t> &configuredFailureSkips() {
  static std::atomic<std::uint32_t> value{0};
  return value;
}

std::atomic<core::CancellationSource *> &
openResultWithDataCancellationSource() {
  static std::atomic<core::CancellationSource *> value{nullptr};
  return value;
}

std::mutex &forcedResultIdMutex() {
  static std::mutex value;
  return value;
}

std::optional<std::string> &forcedResultId() {
  static std::optional<std::string> value;
  return value;
}

std::optional<std::string> consumeForcedResultId() {
  std::scoped_lock lock{forcedResultIdMutex()};
  return std::exchange(forcedResultId(), std::nullopt);
}

bool consumeFailure(const testing::FailurePoint point) noexcept {
  if (configuredFailurePoint().load() != point) {
    return false;
  }
  auto skips = configuredFailureSkips().load();
  while (skips > 0) {
    if (configuredFailureSkips().compare_exchange_weak(skips, skips - 1)) {
      return false;
    }
  }
  auto expected = point;
  return configuredFailurePoint().compare_exchange_strong(
      expected, testing::FailurePoint::none);
}
#else
constexpr bool consumeFailure(testing::FailurePoint) noexcept { return false; }
#endif

core::Error storageError(std::string message) {
  return core::makeError(core::ErrorCode::internal, std::move(message));
}

core::Error injectedFailure(const std::string_view operation) {
  return storageError("Injected Result store failure at " +
                      std::string{operation});
}

core::Result<void> moveNoClobber(const std::filesystem::path &source,
                                 const std::filesystem::path &destination,
                                 const std::string_view operation) {
  std::error_code errorCode;
  std::filesystem::create_hard_link(source, destination, errorCode);
  if (errorCode) {
    return storageError(std::string{operation} +
                        " without clobbering: " + errorCode.message());
  }
  std::filesystem::remove(source, errorCode);
  if (errorCode) {
    std::error_code rollbackError;
    std::filesystem::remove(destination, rollbackError);
    return storageError(std::string{operation} + ": " + errorCode.message());
  }
  return {};
}

bool hasResultArtifact(const std::filesystem::path &root,
                       const std::string_view resultId) {
  for (const auto *directory :
       {"Staging", "Drafts", "Results", "Trash", "Quarantine"}) {
    for (const auto *suffix :
         {".bteresult", ".purging.bteresult", ".discarding.bteresult"}) {
      if (std::filesystem::exists(root / directory /
                                  (std::string{resultId} + suffix))) {
        return true;
      }
    }
  }
  return false;
}

bool hasSiblingResultArtifact(const std::filesystem::path &root,
                              const std::string_view resultId,
                              const std::filesystem::path &currentPath) {
  const auto current = currentPath.lexically_normal();
  for (const auto *directory : {"Staging", "Drafts", "Results", "Trash"}) {
    for (const auto *suffix :
         {".bteresult", ".purging.bteresult", ".discarding.bteresult"}) {
      const auto candidate =
          (root / directory / (std::string{resultId} + suffix))
              .lexically_normal();
      if (candidate != current && std::filesystem::exists(candidate)) {
        return true;
      }
    }
  }
  return false;
}

class Database final {
public:
  Database() = default;
  ~Database() {
    if (handle_ != nullptr) {
      sqlite3_close_v2(handle_);
    }
  }
  Database(const Database &) = delete;
  Database &operator=(const Database &) = delete;
  Database(Database &&) = delete;
  Database &operator=(Database &&) = delete;

  [[nodiscard]] core::Result<void> open(const std::filesystem::path &path,
                                        const int flags) {
    if (consumeFailure(testing::FailurePoint::databaseOpen)) {
      return injectedFailure("database open");
    }
    // The manifest pins this exact SQLite ABI; exercising a mismatch would
    // require replacing the process dependency. GCOVR_EXCL_START
    if (sqlite3_libversion_number() != 3'053'004) {
      return core::makeError(core::ErrorCode::schemaMismatch,
                             "SQLite 3.53.4 is required");
    }
    // GCOVR_EXCL_STOP
    if (sqlite3_open_v2(path.string().c_str(), &handle_, flags, nullptr) !=
        SQLITE_OK) {
      return error("Unable to open Result database");
    }
    sqlite3_busy_timeout(handle_, 5'000);
    return {};
  }

  [[nodiscard]] core::Result<void> execute(const std::string_view sql) {
    if (consumeFailure(testing::FailurePoint::sqlExecution)) {
      return injectedFailure("SQL execution");
    }
    char *message = nullptr;
    const auto status = sqlite3_exec(handle_, std::string{sql}.c_str(), nullptr,
                                     nullptr, &message);
    const auto detail =
        message == nullptr ? std::string{} : std::string{message};
    sqlite3_free(message);
    if (status != SQLITE_OK) {
      return storageError("SQLite operation failed: " + detail);
    }
    return {};
  }

  [[nodiscard]] sqlite3 *handle() const noexcept { return handle_; }

  [[nodiscard]] core::Result<void> close() {
    if (handle_ == nullptr) {
      return {};
    }
    if (sqlite3_close(handle_) != SQLITE_OK) {
      return error("Unable to close Result database");
    }
    handle_ = nullptr;
    return {};
  }

  void observeCancellation(const core::CancellationToken &cancellation) {
    cancellation_ = cancellation;
    sqlite3_progress_handler(
        handle_, 1'000,
        [](void *context) {
          const auto *token =
              static_cast<const core::CancellationToken *>(context);
          return token->isCancellationRequested() ? 1 : 0;
        },
        &cancellation_);
  }

  [[nodiscard]] core::Error error(const std::string &message) const {
    const auto detail =
        handle_ == nullptr ? std::string{} : sqlite3_errmsg(handle_);
    return storageError(detail.empty() ? message : message + ": " + detail);
  }

private:
  sqlite3 *handle_ = nullptr;
  core::CancellationToken cancellation_;
};

class Statement final {
public:
  Statement() = default;
  ~Statement() {
    if (handle_ != nullptr) {
      sqlite3_finalize(handle_);
    }
  }
  Statement(const Statement &) = delete;
  Statement &operator=(const Statement &) = delete;
  Statement(Statement &&) = delete;
  Statement &operator=(Statement &&) = delete;

  [[nodiscard]] core::Result<void> prepare(Database &database,
                                           const std::string_view sql) {
    database_ = &database;
    if (consumeFailure(testing::FailurePoint::statementPreparation)) {
      return injectedFailure("statement preparation");
    }
    if (sqlite3_prepare_v2(database.handle(), std::string{sql}.c_str(), -1,
                           &handle_, nullptr) != SQLITE_OK) {
      return database.error("Unable to prepare Result statement");
    }
    return {};
  }

  [[nodiscard]] core::Result<void> text(const int index,
                                        const std::string_view value) {
    if (consumeFailure(testing::FailurePoint::textBinding) ||
        sqlite3_bind_text(handle_, index, value.data(),
                          static_cast<int>(value.size()),
                          SQLITE_TRANSIENT) != SQLITE_OK) {
      return database_->error("Unable to bind Result text");
    }
    return {};
  }

  [[nodiscard]] core::Result<void> integer(const int index,
                                           const std::int64_t value) {
    if (consumeFailure(testing::FailurePoint::integerBinding) ||
        sqlite3_bind_int64(handle_, index, value) != SQLITE_OK) {
      return database_->error("Unable to bind Result integer");
    }
    return {};
  }

  [[nodiscard]] core::Result<void>
  optionalInteger(const int index, const std::optional<std::int64_t> value) {
    if (consumeFailure(testing::FailurePoint::optionalIntegerBinding)) {
      return database_->error("Unable to bind optional Result integer");
    }
    const auto status = value.has_value()
                            ? sqlite3_bind_int64(handle_, index, *value)
                            : sqlite3_bind_null(handle_, index);
    if (status != SQLITE_OK) {
      return database_->error("Unable to bind optional Result integer");
    }
    return {};
  }

  [[nodiscard]] core::Result<void> done() {
    if (consumeFailure(testing::FailurePoint::statementExecution) ||
        sqlite3_step(handle_) != SQLITE_DONE) {
      return database_->error("Unable to execute Result statement");
    }
    return {};
  }

  [[nodiscard]] int step() { return sqlite3_step(handle_); }
  [[nodiscard]] sqlite3_stmt *handle() const noexcept { return handle_; }

private:
  Database *database_ = nullptr;
  sqlite3_stmt *handle_ = nullptr;
};

core::Result<std::unique_ptr<Database>>
openDatabase(const std::filesystem::path &path, const bool readOnly = false,
             const bool createIfMissing = true) {
  auto database = std::make_unique<Database>();
  const auto flags = readOnly ? SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX
                              : SQLITE_OPEN_READWRITE | SQLITE_OPEN_FULLMUTEX |
                                    (createIfMissing ? SQLITE_OPEN_CREATE : 0);
  auto opened = database->open(path, flags);
  if (!opened.ok()) {
    return opened.error();
  }
  return database;
}

bool validHash(const std::string_view value) {
  return value.size() == 64 &&
         std::ranges::all_of(value, [](const char character) {
           return (character >= '0' && character <= '9') ||
                  (character >= 'a' && character <= 'f');
         });
}

bool validResultId(const std::string_view value) {
  return value.size() == 32 &&
         std::ranges::all_of(value, [](const char character) {
           return (character >= '0' && character <= '9') ||
                  (character >= 'a' && character <= 'f');
         });
}

core::Result<std::filesystem::path>
existingResultPath(const std::filesystem::path &root,
                   const std::string &resultId) {
  if (!validResultId(resultId)) {
    return core::makeError(core::ErrorCode::invalidArgument,
                           "Result ID is invalid");
  }
  const auto path = root / "Results" / (resultId + ".bteresult");
  if (!std::filesystem::exists(path)) {
    return core::makeError(core::ErrorCode::notFound, "Result is unavailable");
  }
  return path;
}

std::size_t utf8SequenceLength(const unsigned char lead) {
  if (lead < 0x80) {
    return 1;
  }
  if (lead >= 0xC2 && lead <= 0xDF) {
    return 2;
  }
  if (lead >= 0xE0 && lead <= 0xEF) {
    return 3;
  }
  if (lead >= 0xF0 && lead <= 0xF4) {
    return 4;
  }
  return 0;
}

bool validUtf8SecondByte(const unsigned char lead, const unsigned char second) {
  if (second < 0x80 || second > 0xBF) {
    return false;
  }
  if (lead == 0xE0 || lead == 0xF0) {
    return second >= (lead == 0xE0 ? 0xA0 : 0x90);
  }
  if (lead == 0xED || lead == 0xF4) {
    return second <= (lead == 0xED ? 0x9F : 0x8F);
  }
  return true;
}

bool validUtf8(const std::string_view value) {
  for (std::size_t index = 0; index < value.size();) {
    const auto lead = static_cast<unsigned char>(value[index]);
    const auto length = utf8SequenceLength(lead);
    if (length == 0 || index + length > value.size()) {
      return false;
    }
    if (length > 1 && !validUtf8SecondByte(
                          lead, static_cast<unsigned char>(value[index + 1]))) {
      return false;
    }
    for (auto offset = 2U; offset < length; ++offset) {
      const auto continuation =
          static_cast<unsigned char>(value[index + offset]);
      if (continuation < 0x80 || continuation > 0xBF) {
        return false;
      }
    }
    index += length;
  }
  return true;
}

bool validDisplayName(const std::string_view value) {
  if (value.empty() || value.size() > 160 || value.front() == ' ' ||
      value.back() == ' ') {
    return false;
  }
  return validUtf8(value) &&
         std::ranges::none_of(value, [](const unsigned char character) {
           return character < 32 || character == 127;
         });
}

core::Result<void> writeDisplayName(const std::filesystem::path &path,
                                    const std::string_view displayName) {
  auto database = openDatabase(path, false, false);
  if (!database.ok()) {
    return database.error();
  }
  for (const auto *const sql : {"BEGIN IMMEDIATE",
                                "CREATE TABLE IF NOT EXISTS "
                                "result_display_name(name TEXT NOT NULL)",
                                "DELETE FROM result_display_name"}) {
    auto executed = database.value()->execute(sql);
    if (!executed.ok()) {
      return executed.error();
    }
  }
  {
    Statement name;
    auto prepared = name.prepare(*database.value(),
                                 "INSERT INTO result_display_name VALUES(?1)");
    if (!prepared.ok()) {
      return prepared.error();
    }
    auto bound = name.text(1, displayName);
    if (!bound.ok()) {
      return bound.error();
    }
    auto inserted = name.done();
    if (!inserted.ok()) {
      return inserted.error();
    }
  }
  auto committed = database.value()->execute("COMMIT");
  if (!committed.ok()) {
    return committed.error();
  }
  Statement checkpoint;
  auto prepared =
      checkpoint.prepare(*database.value(), "PRAGMA wal_checkpoint(TRUNCATE)");
  if (!prepared.ok() || checkpoint.step() != SQLITE_ROW ||
      sqlite3_column_int(checkpoint.handle(), 0) != 0) {
    return storageError("Result display name checkpoint is blocked");
  }
  return {};
}

bool validRunStatus(const RunStatus status) {
  return status >= RunStatus::running && status <= RunStatus::incomplete;
}

core::Result<void> validateCanonicalRecord(const CanonicalRecord &record,
                                           const RunDescriptor &descriptor,
                                           const core::ErrorCode errorCode) {
  const auto invalid = [&](const std::string &message) {
    return core::makeError(errorCode, message);
  };
  if (record.family < RecordFamily::order ||
      record.family > RecordFamily::terminalDiagnostic ||
      record.side < OrderSide::none || record.side > OrderSide::sell) {
    return invalid("Canonical record enum is invalid");
  }
  if (std::ranges::find(descriptor.universe, record.symbol) ==
          descriptor.universe.end() ||
      record.timestamp < descriptor.range.start ||
      record.timestamp >= descriptor.range.end) {
    return invalid("Canonical record identity or timestamp is invalid");
  }
  switch (record.family) {
  case RecordFamily::order:
    if (record.side == OrderSide::none || !record.quantityShares.has_value() ||
        *record.quantityShares <= 0) {
      return invalid("Canonical order payload is invalid");
    }
    break;
  case RecordFamily::fill:
    if (record.side == OrderSide::none || !record.quantityShares.has_value() ||
        *record.quantityShares <= 0 || !record.priceNanodollars.has_value() ||
        *record.priceNanodollars <= 0 ||
        !record.amountMicrodollars.has_value() ||
        *record.amountMicrodollars < 0) {
      return invalid("Canonical fill payload is invalid");
    }
    break;
  case RecordFamily::portfolio:
    if (record.side != OrderSide::none ||
        !record.cashMicrodollars.has_value() ||
        !record.marketValueMicrodollars.has_value() ||
        !record.equityMicrodollars.has_value() ||
        !record.pnlMicrodollars.has_value() ||
        !record.positionShares.has_value()) {
      return invalid("Canonical portfolio payload is invalid");
    }
    break;
  case RecordFamily::cost:
    if (record.side == OrderSide::none || !record.quantityShares.has_value() ||
        *record.quantityShares <= 0 || !record.amountMicrodollars.has_value() ||
        *record.amountMicrodollars < 0) {
      return invalid("Canonical cost payload is invalid");
    }
    break;
  case RecordFamily::warning:
  case RecordFamily::log:
  case RecordFamily::terminalDiagnostic:
    if (record.side != OrderSide::none || record.text.empty()) {
      return invalid("Canonical diagnostic payload is invalid");
    }
    break;
  }
  return {};
}

core::Result<void> validateDescriptor(const RunDescriptor &descriptor) {
  if (descriptor.universe.empty() ||
      descriptor.range.start >= descriptor.range.end ||
      descriptor.initialCapitalMicrodollars <= 0 ||
      descriptor.strategyId.empty() || !validHash(descriptor.strategyHash) ||
      !validHash(descriptor.dataSelection.snapshotId) ||
      descriptor.dataSelection.timeframe != "ohlcv-1h" ||
      descriptor.dataSelection.profile.empty() ||
      descriptor.dataSelection.spans.empty()) {
    return core::makeError(core::ErrorCode::invalidArgument,
                           "Run descriptor is invalid");
  }
  auto universe = descriptor.universe;
  std::ranges::sort(universe);
  if (std::ranges::adjacent_find(universe) != universe.end()) {
    return core::makeError(core::ErrorCode::invalidArgument,
                           "Run universe must contain unique symbols");
  }
  std::optional<std::size_t> previousOrdinal;
  for (const auto &span : descriptor.dataSelection.spans) {
    if ((previousOrdinal.has_value() &&
         *previousOrdinal >= span.segmentOrdinal) ||
        !std::ranges::binary_search(universe, span.symbol) ||
        !validHash(span.segmentId) || span.segmentHash != span.segmentId ||
        span.rowCount == 0 || span.firstTimestamp > span.lastTimestamp) {
      return core::makeError(core::ErrorCode::invalidArgument,
                             "Data Selection span is invalid");
    }
    previousOrdinal = span.segmentOrdinal;
  }
  return {};
}

core::Result<void>
validateDataSelection(const std::filesystem::path &dataRoot,
                      const data::DataSelectionIdentity &identity,
                      const core::CancellationToken &cancellation = {}) {
  auto reader = data::ReleaseSnapshotReader::open(dataRoot, identity.snapshotId,
                                                  cancellation);
  if (!reader.ok()) {
    return reader.error();
  }
  auto exact = reader.value()->readExact(identity, cancellation);
  if (!exact.ok()) {
    return exact.error();
  }
  return {};
}

std::string joinUniverse(const std::vector<std::string> &universe) {
  std::string joined;
  for (const auto &symbol : universe) {
    if (!joined.empty()) {
      joined.push_back('\n');
    }
    joined.append(symbol);
  }
  return joined;
}

std::vector<std::string> splitUniverse(const std::string_view joined) {
  std::vector<std::string> universe;
  std::size_t start = 0;
  while (start <= joined.size()) {
    const auto end = joined.find('\n', start);
    universe.emplace_back(joined.substr(start, end - start));
    if (end == std::string_view::npos) {
      break;
    }
    start = end + 1;
  }
  return universe;
}

std::string textColumn(sqlite3_stmt *statement, const int column) {
  const auto *value = sqlite3_column_text(statement, column);
  if (value == nullptr) {
    return {};
  }
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): SQLite
  return std::string{reinterpret_cast<const char *>(value)};
}

std::optional<std::int64_t> optionalIntegerColumn(sqlite3_stmt *statement,
                                                  const int column) {
  if (sqlite3_column_type(statement, column) == SQLITE_NULL) {
    return std::nullopt;
  }
  return sqlite3_column_int64(statement, column);
}

void appendUnsigned(std::string &bytes, const std::uint64_t value) {
  for (int shift = 56; shift >= 0; shift -= 8) {
    bytes.push_back(static_cast<char>((value >> shift) & 0xffU));
  }
}

void appendFrame(std::string &bytes, const char tag,
                 const std::string_view value) {
  bytes.push_back(tag);
  appendUnsigned(bytes, value.size());
  bytes.append(value);
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): typed canonical frame
void appendInteger(std::string &bytes, const char tag,
                   const std::int64_t value) {
  // Integer frames always carry eight bytes. Write directly into the canonical
  // stream instead of constructing a temporary string for every record field.
  bytes.push_back(tag);
  appendUnsigned(bytes, 8);
  appendUnsigned(bytes, static_cast<std::uint64_t>(value));
}

void appendOptionalInteger(std::string &bytes, const char tag,
                           const std::optional<std::int64_t> value) {
  if (!value.has_value()) {
    appendFrame(bytes, tag, std::string_view{});
    return;
  }
  appendInteger(bytes, tag, *value);
}

std::string
canonicalHash(const RunDescriptor &descriptor,
              const std::vector<CanonicalRecord> &records,
              const RunStatus status, const std::string_view terminalReason,
              const FinalSummary &summary,
              const std::span<const CanonicalRecord> terminalTail = {}) {
  std::string bytes{"BTE-CANONICAL-RESULT"};
  bytes.push_back('\0');
  appendInteger(bytes, 'v', schemaVersion);
  for (const auto &symbol : descriptor.universe) {
    appendFrame(bytes, 'u', symbol);
  }
  appendInteger(bytes, 's', core::time::toUnixMillis(descriptor.range.start));
  appendInteger(bytes, 'e', core::time::toUnixMillis(descriptor.range.end));
  appendInteger(bytes, 'c', descriptor.initialCapitalMicrodollars);
  appendFrame(bytes, 'i', descriptor.strategyId);
  appendFrame(bytes, 'h', descriptor.strategyHash);
  appendFrame(bytes, 'n', std::string{numericPolicy});
  appendFrame(bytes, 'a', std::string{aggregationPolicy});
  appendFrame(bytes, 'd', descriptor.dataSelection.snapshotId);
  appendFrame(bytes, 'd', descriptor.dataSelection.calendarHash);
  appendFrame(bytes, 'd', descriptor.dataSelection.splitManifestHash);
  appendFrame(bytes, 'd', descriptor.dataSelection.timeframe);
  appendFrame(bytes, 'd', descriptor.dataSelection.profile);
  for (const auto &span : descriptor.dataSelection.spans) {
    appendInteger(bytes, 'o', static_cast<std::int64_t>(span.segmentOrdinal));
    appendFrame(bytes, 'y', span.symbol);
    appendFrame(bytes, 'g', span.segmentId);
    appendFrame(bytes, 'g', span.segmentHash);
    appendInteger(bytes, 'r', static_cast<std::int64_t>(span.firstRow));
    appendInteger(bytes, 'r', static_cast<std::int64_t>(span.rowCount));
    appendInteger(bytes, 't', core::time::toUnixMillis(span.firstTimestamp));
    appendInteger(bytes, 't', core::time::toUnixMillis(span.lastTimestamp));
  }
  for (const auto part :
       {std::span<const CanonicalRecord>{records}, terminalTail}) {
    for (const auto &record : part) {
      appendInteger(bytes, 'q', static_cast<std::int64_t>(record.sequence));
      appendInteger(bytes, 't', core::time::toUnixMillis(record.timestamp));
      appendFrame(bytes, 'y', record.symbol);
      appendInteger(bytes, 'f', static_cast<std::int64_t>(record.family));
      appendInteger(bytes, 'x', static_cast<std::int64_t>(record.side));
      appendOptionalInteger(bytes, '1', record.quantityShares);
      appendOptionalInteger(bytes, '2', record.priceNanodollars);
      appendOptionalInteger(bytes, '3', record.amountMicrodollars);
      appendOptionalInteger(bytes, '4', record.cashMicrodollars);
      appendOptionalInteger(bytes, '5', record.marketValueMicrodollars);
      appendOptionalInteger(bytes, '6', record.equityMicrodollars);
      appendOptionalInteger(bytes, 'p', record.pnlMicrodollars);
      appendOptionalInteger(bytes, '7', record.positionShares);
      appendFrame(bytes, 'm', record.text);
    }
  }
  appendInteger(bytes, 'z', static_cast<std::int64_t>(status));
  appendFrame(bytes, 'm', terminalReason);
  appendOptionalInteger(bytes, '8', summary.finalEquityMicrodollars);
  appendOptionalInteger(bytes, '9', summary.pnlMicrodollars);
  return core::sha256(bytes);
}

std::string allocateResultId() {
#if defined(BTE_ENABLE_TEST_HOOKS)
  if (auto forced = consumeForcedResultId(); forced.has_value()) {
    return std::move(*forced);
  }
#endif
  std::random_device source;
  constexpr auto digits = std::string_view{"0123456789abcdef"};
  std::string result(32, '0');
  for (auto &character : result) {
    character = digits[source() & 0xfU];
  }
  return result;
}

std::int64_t nowUtcMillis() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

// SQLite is the cross-process authority, including for aliased store paths.
// Each raw pointer below is an external C ABI output immediately owned by RAII.
class ResultLease final {
public:
  enum class Creation : std::uint8_t { existing, fresh, terminal };

  [[nodiscard]] static core::Result<std::unique_ptr<ResultLease>>
  acquire(const std::filesystem::path &root, const std::string &id,
          const Creation creation) {
    if (!validResultId(id)) {
      return core::makeError(core::ErrorCode::invalidArgument,
                             "Result lease ID is invalid");
    }
    const auto path = root / "Leases" / (id + ".sqlite3");
    if (creation != Creation::existing && !std::filesystem::exists(path)) {
      auto installed = install(root, id, path);
      if (!installed.ok()) {
        return installed.error();
      }
    } else if (creation == Creation::fresh) {
      return core::makeError(core::ErrorCode::resourceBusy,
                             "Result lease identity already exists");
    }
    auto lease = std::make_unique<ResultLease>();
    auto opened = lease->open(path);
    if (!opened.ok()) {
      return opened.error();
    }
    auto locked = lease->execute("BEGIN EXCLUSIVE");
    if (!locked.ok()) {
      return locked.error();
    }
    // Inspection never changes an existing database's journal configuration.
    auto mode = lease->query("PRAGMA journal_mode");
    if (!mode.ok()) {
      return mode.error();
    }
    if (mode.value().first != "delete") {
      return core::makeError(core::ErrorCode::schemaMismatch,
                             "Result lease requires DELETE journal mode");
    }
    auto identity =
        lease->query("SELECT result_id,protocol_version FROM lease_identity");
    if (!identity.ok()) {
      return identity.error();
    }
    if (identity.value().first != id || identity.value().second != 1) {
      return core::makeError(core::ErrorCode::schemaMismatch,
                             "Result lease identity or protocol is invalid");
    }
    return lease;
  }

private:
  struct SeedDirectory final {
    std::filesystem::path path;
    ~SeedDirectory() {
      std::error_code ignored;
      std::filesystem::remove_all(path, ignored);
    }
  };

  [[nodiscard]] static core::Result<void>
  install(const std::filesystem::path &root, const std::string &id,
          const std::filesystem::path &destination) {
    // The directory is allocated exclusively. Publish a fully initialized,
    // closed database using a no-clobber hard link; never replace a lease
    // inode.
    const auto directory = root / "Leases" / ("Seed-" + allocateResultId());
    std::error_code errorCode;
    if (!std::filesystem::create_directory(directory, errorCode)) {
      return storageError("Unable to allocate Result lease initialization: " +
                          errorCode.message());
    }
    const SeedDirectory owner{directory};
    const auto source = directory / "Lease.sqlite3";
    {
      ResultLease seed;
      auto opened = seed.open(source, true);
      if (!opened.ok()) {
        return opened.error();
      }
      for (const auto &sql :
           {std::string{"PRAGMA journal_mode=DELETE"},
            std::string{"PRAGMA synchronous=EXTRA"},
            std::string{"CREATE TABLE lease_identity(result_id TEXT PRIMARY "
                        "KEY,protocol_version INTEGER NOT NULL)"},
            "INSERT INTO lease_identity VALUES('" + id + "',1)"}) {
        auto executed = seed.execute(sql);
        if (!executed.ok()) {
          return executed.error();
        }
      }
    }
    std::filesystem::create_hard_link(source, destination, errorCode);
    if (errorCode) {
      return core::makeError(
          errorCode == std::errc::file_exists ? core::ErrorCode::resourceBusy
                                              : core::ErrorCode::internal,
          "Unable to install Result lease: " + errorCode.message());
    }
    return {};
  }

  [[nodiscard]] core::Error error(const std::string &operation) const {
    const auto code = sqlite3_errcode(database_.get());
    return core::makeError(code == SQLITE_BUSY || code == SQLITE_LOCKED
                               ? core::ErrorCode::resourceBusy
                               : core::ErrorCode::schemaMismatch,
                           operation + ": " + sqlite3_errmsg(database_.get()));
  }

  [[nodiscard]] core::Result<void> open(const std::filesystem::path &path,
                                        const bool create = false) {
    sqlite3 *raw = nullptr;
    const auto flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_FULLMUTEX |
                       (create ? SQLITE_OPEN_CREATE : 0);
    const auto status =
        sqlite3_open_v2(path.string().c_str(), &raw, flags, nullptr);
    database_.reset(raw);
    if (status != SQLITE_OK) {
      return error("Unable to open Result lease");
    }
    sqlite3_busy_timeout(database_.get(), 0);
    return execute("PRAGMA synchronous=EXTRA");
  }

  [[nodiscard]] core::Result<void> execute(const std::string &sql) {
    if (sqlite3_exec(database_.get(), sql.c_str(), nullptr, nullptr, nullptr) !=
        SQLITE_OK) {
      return error("Unable to acquire or initialize Result lease");
    }
    return {};
  }

  [[nodiscard]] core::Result<std::pair<std::string, int>>
  query(const std::string &sql) {
    sqlite3_stmt *raw = nullptr;
    const auto prepared =
        sqlite3_prepare_v2(database_.get(), sql.c_str(), -1, &raw, nullptr);
    const auto statement =
        std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>{
            raw, &sqlite3_finalize};
    if (prepared != SQLITE_OK || sqlite3_step(statement.get()) != SQLITE_ROW) {
      return error("Unable to validate Result lease");
    }
    auto value = std::pair{textColumn(statement.get(), 0),
                           (sqlite3_column_count(statement.get()) > 1
                                ? sqlite3_column_int(statement.get(), 1)
                                : 0)};
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
      return core::makeError(core::ErrorCode::schemaMismatch,
                             "Result lease metadata has duplicate identities");
    }
    return value;
  }

  std::unique_ptr<sqlite3, decltype(&sqlite3_close)> database_{nullptr,
                                                               &sqlite3_close};
};

std::vector<std::string> distinctSegments(const RunDescriptor &descriptor) {
  std::vector<std::string> ids;
  ids.reserve(descriptor.dataSelection.spans.size());
  for (const auto &span : descriptor.dataSelection.spans) {
    ids.push_back(span.segmentId);
  }
  std::ranges::sort(ids);
  const auto duplicates = std::ranges::unique(ids);
  ids.erase(duplicates.begin(), duplicates.end());
  return ids;
}

core::Result<void> createResultSchema(Database &database,
                                      const RunDescriptor &descriptor,
                                      const std::string &resultId) {
  for (
      const auto *const sql : {
          "PRAGMA journal_mode=WAL",
          "PRAGMA synchronous=FULL",
          "PRAGMA foreign_keys=ON",
          "CREATE TABLE result_meta(schema_version INTEGER NOT NULL, "
          "result_id TEXT PRIMARY KEY, status INTEGER NOT NULL, "
          "canonical_hash TEXT NOT NULL, terminal_reason TEXT NOT NULL, "
          "numeric_policy TEXT NOT NULL, aggregation_policy TEXT NOT NULL, "
          "created_utc_ms INTEGER NOT NULL, saved_utc_ms INTEGER NOT NULL)",
          "CREATE TABLE run_configuration(universe TEXT NOT NULL, "
          "range_start_ms INTEGER NOT NULL, range_end_ms INTEGER NOT NULL, "
          "initial_capital INTEGER NOT NULL, strategy_id TEXT NOT NULL, "
          "strategy_hash TEXT NOT NULL)",
          "CREATE TABLE data_selection(snapshot_id TEXT NOT NULL, "
          "calendar_hash TEXT NOT NULL, split_manifest_hash TEXT NOT NULL, "
          "timeframe TEXT NOT NULL, profile TEXT NOT NULL)",
          "CREATE TABLE result_capabilities(orders INTEGER NOT NULL, fills "
          "INTEGER NOT NULL, post_slice_portfolio INTEGER NOT NULL, "
          "slippage_costs INTEGER NOT NULL, warnings INTEGER NOT NULL, "
          "terminal_diagnostics INTEGER NOT NULL, trades INTEGER NOT NULL, "
          "metrics INTEGER NOT NULL, strategy_logs INTEGER NOT NULL, "
          "indicator_snapshots INTEGER NOT NULL, corporate_actions INTEGER "
          "NOT NULL, margin INTEGER NOT NULL)",
          "CREATE TABLE data_spans(ordinal INTEGER PRIMARY KEY, symbol TEXT "
          "NOT NULL, segment_id TEXT NOT NULL, segment_hash TEXT NOT NULL, "
          "first_row INTEGER NOT NULL, row_count INTEGER NOT NULL, "
          "first_timestamp_ms INTEGER NOT NULL, last_timestamp_ms INTEGER NOT "
          "NULL)",
          "CREATE TABLE canonical_records(sequence INTEGER PRIMARY KEY, "
          "timestamp_ms INTEGER NOT NULL, symbol TEXT NOT NULL, family INTEGER "
          "NOT NULL, side INTEGER NOT NULL, quantity INTEGER, price INTEGER, "
          "amount INTEGER, cash INTEGER, market_value INTEGER, equity INTEGER, "
          "pnl INTEGER, position INTEGER, text TEXT NOT NULL)",
          "CREATE TABLE summary(final_equity INTEGER, pnl INTEGER)",
      }) {
    auto executed = database.execute(sql);
    if (!executed.ok()) {
      return executed.error();
    }
  }

  Statement meta;
  auto prepared = meta.prepare(
      database, "INSERT INTO result_meta VALUES(?1,?2,?3,'','',?4,?5,?6,0)");
  if (!prepared.ok() || !meta.integer(1, schemaVersion).ok() ||
      !meta.text(2, resultId).ok() ||
      !meta.integer(3, static_cast<std::int64_t>(RunStatus::running)).ok() ||
      !meta.text(4, numericPolicy).ok() ||
      !meta.text(5, aggregationPolicy).ok() ||
      !meta.integer(6, nowUtcMillis()).ok()) {
    return database.error("Unable to bind Result metadata");
  }
  auto inserted = meta.done();
  if (!inserted.ok()) {
    return inserted.error();
  }

  Statement run;
  prepared = run.prepare(database, "INSERT INTO run_configuration VALUES("
                                   "?1,?2,?3,?4,?5,?6)");
  if (!prepared.ok() || !run.text(1, joinUniverse(descriptor.universe)).ok() ||
      !run.integer(2, core::time::toUnixMillis(descriptor.range.start)).ok() ||
      !run.integer(3, core::time::toUnixMillis(descriptor.range.end)).ok() ||
      !run.integer(4, descriptor.initialCapitalMicrodollars).ok() ||
      !run.text(5, descriptor.strategyId).ok() ||
      !run.text(6, descriptor.strategyHash).ok()) {
    return database.error("Unable to bind Run Configuration");
  }
  inserted = run.done();
  if (!inserted.ok()) {
    return inserted.error();
  }

  Statement selection;
  prepared = selection.prepare(database, "INSERT INTO data_selection VALUES("
                                         "?1,?2,?3,?4,?5)");
  const auto &identity = descriptor.dataSelection;
  if (!prepared.ok() || !selection.text(1, identity.snapshotId).ok() ||
      !selection.text(2, identity.calendarHash).ok() ||
      !selection.text(3, identity.splitManifestHash).ok() ||
      !selection.text(4, identity.timeframe).ok() ||
      !selection.text(5, identity.profile).ok()) {
    return database.error("Unable to bind Data Selection");
  }
  inserted = selection.done();
  if (!inserted.ok()) {
    return inserted.error();
  }

  Statement capabilities;
  prepared =
      capabilities.prepare(database, "INSERT INTO result_capabilities VALUES("
                                     "?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12)");
  const auto declared = ResultCapabilities{};
  const auto flag = [](const bool enabled) -> std::int64_t {
    return enabled ? 1 : 0;
  };
  if (!prepared.ok() || !capabilities.integer(1, flag(declared.orders)).ok() ||
      !capabilities.integer(2, flag(declared.fills)).ok() ||
      !capabilities.integer(3, flag(declared.postSlicePortfolio)).ok() ||
      !capabilities.integer(4, flag(declared.slippageCosts)).ok() ||
      !capabilities.integer(5, flag(declared.warnings)).ok() ||
      !capabilities.integer(6, flag(declared.terminalDiagnostics)).ok() ||
      !capabilities.integer(7, flag(declared.trades)).ok() ||
      !capabilities.integer(8, flag(declared.metrics)).ok() ||
      !capabilities.integer(9, flag(declared.strategyLogs)).ok() ||
      !capabilities.integer(10, flag(declared.indicatorSnapshots)).ok() ||
      !capabilities.integer(11, flag(declared.corporateActions)).ok() ||
      !capabilities.integer(12, flag(declared.margin)).ok()) {
    return database.error("Unable to bind Result capabilities");
  }
  inserted = capabilities.done();
  if (!inserted.ok()) {
    return inserted.error();
  }

  for (const auto &span : identity.spans) {
    Statement statement;
    prepared = statement.prepare(database, "INSERT INTO data_spans VALUES("
                                           "?1,?2,?3,?4,?5,?6,?7,?8)");
    if (!prepared.ok() ||
        !statement.integer(1, static_cast<std::int64_t>(span.segmentOrdinal))
             .ok() ||
        !statement.text(2, span.symbol).ok() ||
        !statement.text(3, span.segmentId).ok() ||
        !statement.text(4, span.segmentHash).ok() ||
        !statement.integer(5, static_cast<std::int64_t>(span.firstRow)).ok() ||
        !statement.integer(6, static_cast<std::int64_t>(span.rowCount)).ok() ||
        !statement.integer(7, core::time::toUnixMillis(span.firstTimestamp))
             .ok() ||
        !statement.integer(8, core::time::toUnixMillis(span.lastTimestamp))
             .ok()) {
      return database.error("Unable to bind Data Span");
    }
    inserted = statement.done();
    if (!inserted.ok()) {
      return inserted.error();
    }
  }
  return {};
}

// clang-format off
// NOLINTNEXTLINE(readability-function-cognitive-complexity): parser
core::Result<OpenedResult> readResultFile(
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): store paths
    const std::filesystem::path &path, const std::filesystem::path &dataRoot,
    const DataValidationPolicy dataValidation,
    const ResultStatusPolicy resultStatus = ResultStatusPolicy::terminalOnly,
    const core::CancellationToken &cancellation = {},
    const bool readOnly = false) {
  // clang-format on
  if (cancellation.isCancellationRequested()) {
    return core::makeError(core::ErrorCode::cancelled,
                           "Result read was cancelled");
  }
  auto database = openDatabase(path, readOnly, false);
  if (!database.ok()) {
    return core::makeError(core::ErrorCode::schemaMismatch,
                           "Result database cannot be opened");
  }
  database.value()->observeCancellation(cancellation);
  Statement integrity;
  auto prepared =
      integrity.prepare(*database.value(), "PRAGMA integrity_check");
  if (!prepared.ok() || integrity.step() != SQLITE_ROW ||
      textColumn(integrity.handle(), 0) != "ok") {
    if (cancellation.isCancellationRequested()) {
      return core::makeError(core::ErrorCode::cancelled,
                             "Result read was cancelled");
    }
    return core::makeError(core::ErrorCode::schemaMismatch,
                           "Result database integrity check failed");
  }

  OpenedResult result;
  Statement meta;
  prepared = meta.prepare(*database.value(),
                          "SELECT schema_version,result_id,status,"
                          "canonical_hash,terminal_reason,numeric_policy,"
                          "aggregation_policy,saved_utc_ms FROM result_meta");
  if (!prepared.ok() || meta.step() != SQLITE_ROW ||
      sqlite3_column_int(meta.handle(), 0) != schemaVersion ||
      textColumn(meta.handle(), 5) != numericPolicy ||
      textColumn(meta.handle(), 6) != aggregationPolicy) {
    if (cancellation.isCancellationRequested()) {
      return core::makeError(core::ErrorCode::cancelled,
                             "Result read was cancelled");
    }
    return core::makeError(core::ErrorCode::schemaMismatch,
                           "Result schema or policy is incompatible");
  }
  result.resultId = textColumn(meta.handle(), 1);
  result.status = static_cast<RunStatus>(sqlite3_column_int(meta.handle(), 2));
  result.canonicalResultHash = textColumn(meta.handle(), 3);
  result.terminalReason = textColumn(meta.handle(), 4);
  result.savedUtcMillis = sqlite3_column_int64(meta.handle(), 7);
  if (!validResultId(result.resultId) || !validRunStatus(result.status) ||
      (result.status == RunStatus::running &&
       resultStatus != ResultStatusPolicy::allowRunning) ||
      (result.status != RunStatus::running &&
       !validHash(result.canonicalResultHash))) {
    return core::makeError(core::ErrorCode::schemaMismatch,
                           "Result metadata is incomplete");
  }

  Statement nameTable;
  prepared = nameTable.prepare(
      *database.value(),
      "SELECT count(*) FROM sqlite_master WHERE type='table' AND "
      "name='result_display_name'");
  if (!prepared.ok() || nameTable.step() != SQLITE_ROW) {
    return core::makeError(core::ErrorCode::schemaMismatch,
                           "Result display metadata cannot be inspected");
  }
  if (sqlite3_column_int(nameTable.handle(), 0) != 0) {
    Statement name;
    prepared =
        name.prepare(*database.value(), "SELECT name FROM result_display_name");
    if (!prepared.ok() || name.step() != SQLITE_ROW) {
      return core::makeError(core::ErrorCode::schemaMismatch,
                             "Result display name is invalid");
    }
    result.displayName = textColumn(name.handle(), 0);
    if (!validDisplayName(result.displayName) || name.step() != SQLITE_DONE) {
      return core::makeError(core::ErrorCode::schemaMismatch,
                             "Result display name is invalid");
    }
  }

  Statement run;
  prepared = run.prepare(*database.value(),
                         "SELECT universe,range_start_ms,range_end_ms,"
                         "initial_capital,strategy_id,strategy_hash FROM "
                         "run_configuration");
  if (!prepared.ok() || run.step() != SQLITE_ROW) {
    if (cancellation.isCancellationRequested()) {
      return core::makeError(core::ErrorCode::cancelled,
                             "Result read was cancelled");
    }
    return core::makeError(core::ErrorCode::schemaMismatch,
                           "Run Configuration is unavailable");
  }
  result.descriptor.universe = splitUniverse(textColumn(run.handle(), 0));
  result.descriptor.range = {
      .start =
          core::time::fromUnixMillis(sqlite3_column_int64(run.handle(), 1)),
      .end = core::time::fromUnixMillis(sqlite3_column_int64(run.handle(), 2)),
  };
  result.descriptor.initialCapitalMicrodollars =
      sqlite3_column_int64(run.handle(), 3);
  result.descriptor.strategyId = textColumn(run.handle(), 4);
  result.descriptor.strategyHash = textColumn(run.handle(), 5);

  Statement selection;
  prepared = selection.prepare(*database.value(),
                               "SELECT snapshot_id,calendar_hash,"
                               "split_manifest_hash,timeframe,profile FROM "
                               "data_selection");
  if (!prepared.ok() || selection.step() != SQLITE_ROW) {
    if (cancellation.isCancellationRequested()) {
      return core::makeError(core::ErrorCode::cancelled,
                             "Result read was cancelled");
    }
    return core::makeError(core::ErrorCode::schemaMismatch,
                           "Data Selection is unavailable");
  }
  auto &identity = result.descriptor.dataSelection;
  identity.snapshotId = textColumn(selection.handle(), 0);
  identity.calendarHash = textColumn(selection.handle(), 1);
  identity.splitManifestHash = textColumn(selection.handle(), 2);
  identity.timeframe = textColumn(selection.handle(), 3);
  identity.profile = textColumn(selection.handle(), 4);

  Statement capabilities;
  prepared = capabilities.prepare(
      *database.value(),
      "SELECT orders,fills,post_slice_portfolio,slippage_costs,warnings,"
      "terminal_diagnostics,trades,metrics,strategy_logs,indicator_snapshots,"
      "corporate_actions,margin FROM result_capabilities");
  if (!prepared.ok() || capabilities.step() != SQLITE_ROW) {
    if (cancellation.isCancellationRequested()) {
      return core::makeError(core::ErrorCode::cancelled,
                             "Result read was cancelled");
    }
    return core::makeError(core::ErrorCode::schemaMismatch,
                           "Result capability declaration is unavailable");
  }
  for (auto column = 0; column < 12; ++column) {
    const auto value = sqlite3_column_int(capabilities.handle(), column);
    if (value != 0 && value != 1) {
      return core::makeError(core::ErrorCode::schemaMismatch,
                             "Result capability declaration is invalid");
    }
  }
  result.capabilities = {
      .orders = sqlite3_column_int(capabilities.handle(), 0) != 0,
      .fills = sqlite3_column_int(capabilities.handle(), 1) != 0,
      .postSlicePortfolio = sqlite3_column_int(capabilities.handle(), 2) != 0,
      .slippageCosts = sqlite3_column_int(capabilities.handle(), 3) != 0,
      .warnings = sqlite3_column_int(capabilities.handle(), 4) != 0,
      .terminalDiagnostics = sqlite3_column_int(capabilities.handle(), 5) != 0,
      .trades = sqlite3_column_int(capabilities.handle(), 6) != 0,
      .metrics = sqlite3_column_int(capabilities.handle(), 7) != 0,
      .strategyLogs = sqlite3_column_int(capabilities.handle(), 8) != 0,
      .indicatorSnapshots = sqlite3_column_int(capabilities.handle(), 9) != 0,
      .corporateActions = sqlite3_column_int(capabilities.handle(), 10) != 0,
      .margin = sqlite3_column_int(capabilities.handle(), 11) != 0,
  };
  if (result.capabilities != ResultCapabilities{} ||
      capabilities.step() != SQLITE_DONE) {
    if (cancellation.isCancellationRequested()) {
      return core::makeError(core::ErrorCode::cancelled,
                             "Result read was cancelled");
    }
    return core::makeError(core::ErrorCode::schemaMismatch,
                           "Result capability declaration is invalid");
  }

  Statement spans;
  prepared = spans.prepare(
      *database.value(), "SELECT ordinal,symbol,segment_id,segment_hash,"
                         "first_row,row_count,first_timestamp_ms,"
                         "last_timestamp_ms FROM data_spans ORDER BY ordinal");
  if (!prepared.ok()) {
    return core::makeError(core::ErrorCode::schemaMismatch,
                           "Data Span schema is unavailable");
  }
  while (spans.step() == SQLITE_ROW) {
    if (cancellation.isCancellationRequested()) {
      return core::makeError(core::ErrorCode::cancelled,
                             "Result read was cancelled");
    }
    identity.spans.push_back({
        .segmentOrdinal =
            static_cast<std::size_t>(sqlite3_column_int64(spans.handle(), 0)),
        .symbol = textColumn(spans.handle(), 1),
        .segmentId = textColumn(spans.handle(), 2),
        .segmentHash = textColumn(spans.handle(), 3),
        .firstRow =
            static_cast<std::size_t>(sqlite3_column_int64(spans.handle(), 4)),
        .rowCount =
            static_cast<std::size_t>(sqlite3_column_int64(spans.handle(), 5)),
        .firstTimestamp =
            core::time::fromUnixMillis(sqlite3_column_int64(spans.handle(), 6)),
        .lastTimestamp =
            core::time::fromUnixMillis(sqlite3_column_int64(spans.handle(), 7)),
    });
  }
  if (cancellation.isCancellationRequested()) {
    return core::makeError(core::ErrorCode::cancelled,
                           "Result read was cancelled");
  }
  auto validated = validateDescriptor(result.descriptor);
  if (!validated.ok()) {
    return core::makeError(core::ErrorCode::schemaMismatch,
                           "Persisted Run descriptor is invalid");
  }

  Statement records;
  prepared = records.prepare(
      *database.value(),
      "SELECT sequence,timestamp_ms,symbol,family,side,quantity,price,amount,"
      "cash,market_value,equity,pnl,position,text FROM canonical_records ORDER "
      "BY "
      "sequence");
  if (!prepared.ok()) {
    return core::makeError(core::ErrorCode::schemaMismatch,
                           "Canonical record schema is unavailable");
  }
  while (records.step() == SQLITE_ROW) {
    if (cancellation.isCancellationRequested()) {
      return core::makeError(core::ErrorCode::cancelled,
                             "Result read was cancelled");
    }
    result.records.push_back({
        .sequence = static_cast<std::uint64_t>(
            sqlite3_column_int64(records.handle(), 0)),
        .timestamp = core::time::fromUnixMillis(
            sqlite3_column_int64(records.handle(), 1)),
        .symbol = textColumn(records.handle(), 2),
        .family =
            static_cast<RecordFamily>(sqlite3_column_int(records.handle(), 3)),
        .side = static_cast<OrderSide>(sqlite3_column_int(records.handle(), 4)),
        .quantityShares = optionalIntegerColumn(records.handle(), 5),
        .priceNanodollars = optionalIntegerColumn(records.handle(), 6),
        .amountMicrodollars = optionalIntegerColumn(records.handle(), 7),
        .cashMicrodollars = optionalIntegerColumn(records.handle(), 8),
        .marketValueMicrodollars = optionalIntegerColumn(records.handle(), 9),
        .equityMicrodollars = optionalIntegerColumn(records.handle(), 10),
        .pnlMicrodollars = optionalIntegerColumn(records.handle(), 11),
        .positionShares = optionalIntegerColumn(records.handle(), 12),
        .text = textColumn(records.handle(), 13),
    });
  }
  if (cancellation.isCancellationRequested()) {
    return core::makeError(core::ErrorCode::cancelled,
                           "Result read was cancelled");
  }
  for (std::size_t index = 0; index < result.records.size(); ++index) {
    if (cancellation.isCancellationRequested()) {
      return core::makeError(core::ErrorCode::cancelled,
                             "Result read was cancelled");
    }
    if (result.records[index].sequence != index ||
        (index > 0 && result.records[index - 1].timestamp >
                          result.records[index].timestamp)) {
      return core::makeError(core::ErrorCode::schemaMismatch,
                             "Canonical record order is invalid");
    }
    auto recordValidation =
        validateCanonicalRecord(result.records[index], result.descriptor,
                                core::ErrorCode::schemaMismatch);
    if (!recordValidation.ok()) {
      return recordValidation.error();
    }
  }

  Statement summary;
  prepared = summary.prepare(*database.value(),
                             "SELECT final_equity,pnl FROM summary");
  if (!prepared.ok()) {
    return core::makeError(core::ErrorCode::schemaMismatch,
                           "Result summary schema is unavailable");
  }
  if (summary.step() == SQLITE_ROW) {
    result.summary.finalEquityMicrodollars =
        optionalIntegerColumn(summary.handle(), 0);
    result.summary.pnlMicrodollars = optionalIntegerColumn(summary.handle(), 1);
  }
  if (cancellation.isCancellationRequested()) {
    return core::makeError(core::ErrorCode::cancelled,
                           "Result read was cancelled");
  }
  if (result.status != RunStatus::running &&
      canonicalHash(result.descriptor, result.records, result.status,
                    result.terminalReason,
                    result.summary) != result.canonicalResultHash) {
    return core::makeError(core::ErrorCode::schemaMismatch,
                           "Canonical Result hash is invalid");
  }
  if (dataValidation == DataValidationPolicy::validateExact) {
    auto exact = validateDataSelection(dataRoot, identity, cancellation);
    if (!exact.ok()) {
      return exact.error();
    }
  }
  return result;
}

core::Result<void> initializeCatalog(const std::filesystem::path &root) {
  auto database = openDatabase(root / "Catalog.sqlite3");
  if (!database.ok()) {
    return database.error();
  }
  for (const auto *const sql : {
           "PRAGMA journal_mode=WAL",
           "PRAGMA synchronous=FULL",
           "CREATE TABLE IF NOT EXISTS results(result_id TEXT PRIMARY KEY, "
           "canonical_hash TEXT NOT NULL, status INTEGER NOT NULL, "
           "saved_utc_ms INTEGER NOT NULL)",
       }) {
    auto executed = database.value()->execute(sql);
    if (!executed.ok()) {
      return executed.error();
    }
  }
  return {};
}

core::Result<void> upsertCatalog(const std::filesystem::path &root,
                                 const ResultSummary &summary) {
  auto database = openDatabase(root / "Catalog.sqlite3");
  if (!database.ok()) {
    return database.error();
  }
  Statement statement;
  auto prepared = statement.prepare(
      *database.value(), "INSERT OR REPLACE INTO results VALUES(?1,?2,?3,?4)");
  if (!prepared.ok() || !statement.text(1, summary.resultId).ok() ||
      !statement.text(2, summary.canonicalResultHash).ok() ||
      !statement.integer(3, static_cast<std::int64_t>(summary.status)).ok() ||
      !statement.integer(4, summary.savedUtcMillis).ok()) {
    return database.value()->error("Unable to bind catalog entry");
  }
  return statement.done();
}

core::Result<void> deleteCatalog(const std::filesystem::path &root,
                                 const std::string &resultId) {
  auto database = openDatabase(root / "Catalog.sqlite3");
  if (!database.ok()) {
    return database.error();
  }
  Statement statement;
  auto prepared = statement.prepare(*database.value(),
                                    "DELETE FROM results WHERE result_id=?1");
  if (!prepared.ok() || !statement.text(1, resultId).ok()) {
    return database.value()->error("Unable to bind catalog deletion");
  }
  return statement.done();
}

core::Result<std::unique_ptr<ResultLease>>
mutationLease(const std::filesystem::path &root,
              const std::filesystem::path &dataStore,
              const std::string &resultId) {
  const auto path = root / "Leases" / (resultId + ".sqlite3");
  if (std::filesystem::exists(path)) {
    return ResultLease::acquire(root, resultId,
                                ResultLease::Creation::existing);
  }
  // Legacy terminal artifacts may acquire operational metadata only after
  // validation. Legacy Staging never enters this compatibility path.
  for (const auto *directory : {"Drafts", "Results", "Trash"}) {
    const auto artifact = root / directory / (resultId + ".bteresult");
    if (!std::filesystem::exists(artifact)) {
      continue;
    }
    if (!std::filesystem::is_regular_file(artifact)) {
      return storageError("Result artifact is not a regular file");
    }
    auto valid = readResultFile(artifact, dataStore,
                                DataValidationPolicy::deferToCaller);
    if (!valid.ok()) {
      return valid.error();
    }
    if (valid.value().resultId != resultId) {
      return core::makeError(core::ErrorCode::schemaMismatch,
                             "Result identity does not match its filename");
    }
    return ResultLease::acquire(root, resultId,
                                ResultLease::Creation::terminal);
  }
  return core::makeError(core::ErrorCode::notFound, "Result is unavailable");
}

core::Result<void>
validateCommittedPrefix(const OpenedResult &staged,
                        const std::filesystem::path &dataStore,
                        const core::CancellationToken &cancellation) {
  auto reader = data::ReleaseSnapshotReader::open(
      dataStore, staged.descriptor.dataSelection.snapshotId, cancellation);
  if (!reader.ok()) {
    return reader.error();
  }
  auto exact =
      reader.value()->readExact(staged.descriptor.dataSelection, cancellation);
  if (!exact.ok()) {
    return exact.error();
  }
  std::size_t completedBars = 0;
  for (const auto &record : staged.records) {
    if (cancellation.isCancellationRequested()) {
      return core::makeError(core::ErrorCode::cancelled,
                             "Result recovery was cancelled");
    }
    if (completedBars >= exact.value().bars.size() ||
        record.timestamp != exact.value().bars[completedBars].timestamp ||
        record.family == RecordFamily::terminalDiagnostic) {
      return core::makeError(core::ErrorCode::schemaMismatch,
                             "Running Result does not contain a chronological "
                             "complete-bar prefix");
    }
    if (record.family == RecordFamily::portfolio) {
      ++completedBars;
    }
  }
  if (!staged.records.empty() &&
      staged.records.back().family != RecordFamily::portfolio) {
    return core::makeError(core::ErrorCode::schemaMismatch,
                           "Running Result has an incomplete trailing bar");
  }
  return {};
}

core::Result<void> interruptArtifact(const std::filesystem::path &path,
                                     const OpenedResult &staged) {
  constexpr auto reason = "Run interrupted before terminal finalization";
  const FinalSummary summary;
  const auto hash = canonicalHash(staged.descriptor, staged.records,
                                  RunStatus::interrupted, reason, summary);
  auto database = openDatabase(path, false, false);
  if (!database.ok()) {
    return database.error();
  }
  auto begun = database.value()->execute("BEGIN IMMEDIATE");
  if (!begun.ok()) {
    return begun.error();
  }
  auto inserted =
      database.value()->execute("INSERT INTO summary VALUES(NULL,NULL)");
  if (!inserted.ok()) {
    return inserted.error();
  }
  Statement meta;
  auto prepared = meta.prepare(
      *database.value(),
      "UPDATE result_meta SET "
      "status=?1,canonical_hash=?2,terminal_reason=?3,saved_utc_ms=?4");
  if (!prepared.ok() ||
      !meta.integer(1, static_cast<std::int64_t>(RunStatus::interrupted))
           .ok() ||
      !meta.text(2, hash).ok() || !meta.text(3, reason).ok() ||
      !meta.integer(4, nowUtcMillis()).ok()) {
    return database.value()->error("Unable to bind recovered Result");
  }
  auto updated = meta.done();
  if (!updated.ok()) {
    return updated.error();
  }
  auto committed = database.value()->execute("COMMIT");
  if (!committed.ok()) {
    return committed.error();
  }
  return database.value()->execute("PRAGMA wal_checkpoint(TRUNCATE)");
}

using RecoveryOutcome = core::Result<std::optional<ResultRecoveryNotice>>;

RecoveryOutcome recoverArtifact(const std::filesystem::path &path,
                                const std::filesystem::path &root,
                                const std::filesystem::path &dataStore,
                                const core::CancellationToken &cancellation) {
  const auto filename = path.filename().string();
  const auto removal = filename.ends_with(".purging.bteresult") ||
                       filename.ends_with(".discarding.bteresult");
  const auto suffix =
      filename.ends_with(".purging.bteresult")      ? ".purging.bteresult"
      : filename.ends_with(".discarding.bteresult") ? ".discarding.bteresult"
                                                    : ".bteresult";
  const auto id =
      filename.substr(0, filename.size() - std::string_view{suffix}.size());
  const auto notice = [&id](const ResultRecoveryReason reason,
                            const std::string &diagnostic) -> RecoveryOutcome {
    return std::optional<ResultRecoveryNotice>{{id, reason, diagnostic}};
  };
  if (!std::filesystem::exists(root / "Leases" / (id + ".sqlite3"))) {
    return notice(ResultRecoveryReason::legacyUnleased,
                  "Preserved legacy Result staging without an ownership lease");
  }
  auto lease = ResultLease::acquire(root, id, ResultLease::Creation::existing);
  if (!lease.ok()) {
    if (lease.error().code == core::ErrorCode::resourceBusy) {
      return std::optional<ResultRecoveryNotice>{};
    }
    return notice(ResultRecoveryReason::invalidLease, lease.error().message);
  }
  auto staged =
      readResultFile(path, dataStore, DataValidationPolicy::deferToCaller,
                     ResultStatusPolicy::allowRunning, cancellation);
  if (!staged.ok()) {
    if (staged.error().code == core::ErrorCode::cancelled) {
      return staged.error();
    }
    return notice(ResultRecoveryReason::invalidPrefix, staged.error().message);
  }
  if (staged.value().resultId != id) {
    return notice(ResultRecoveryReason::invalidPrefix,
                  "Result identity does not match its staging filename");
  }
  if (hasSiblingResultArtifact(root, id, path)) {
    return notice(ResultRecoveryReason::invalidPrefix,
                  "Preserved Result staging because another artifact with the "
                  "same identity already exists");
  }
  auto retention = data::SegmentRetentionStore::open(dataStore);
  if (!retention.ok()) {
    return notice(ResultRecoveryReason::missingReferences,
                  retention.error().message);
  }
  const auto expected = distinctSegments(staged.value().descriptor);
  auto references = retention.value()->references(id);
  if (!references.ok()) {
    return notice(ResultRecoveryReason::missingReferences,
                  references.error().message);
  }
  if ((!removal && references.value() != expected) ||
      (removal && !references.value().empty() &&
       references.value() != expected)) {
    return notice(ResultRecoveryReason::missingReferences,
                  "Preserved Result staging with missing or inconsistent data "
                  "references");
  }
  auto pinned = retention.value()->acquire(id, expected);
  if (!pinned.ok()) {
    return notice(ResultRecoveryReason::missingReferences,
                  pinned.error().message);
  }
  auto exact = validateDataSelection(
      dataStore, staged.value().descriptor.dataSelection, cancellation);
  if (!exact.ok()) {
    if (exact.error().code == core::ErrorCode::cancelled) {
      return exact.error();
    }
    return notice(ResultRecoveryReason::missingReferences,
                  exact.error().message);
  }
  if (!removal && staged.value().status == RunStatus::running) {
    auto prefix =
        validateCommittedPrefix(staged.value(), dataStore, cancellation);
    if (!prefix.ok()) {
      if (prefix.error().code == core::ErrorCode::cancelled) {
        return prefix.error();
      }
      return notice(ResultRecoveryReason::invalidPrefix,
                    prefix.error().message);
    }
    auto interrupted = interruptArtifact(path, staged.value());
    if (!interrupted.ok()) {
      return notice(ResultRecoveryReason::invalidPrefix,
                    interrupted.error().message);
    }
  }
  auto validated =
      readResultFile(path, dataStore, DataValidationPolicy::validateExact,
                     ResultStatusPolicy::terminalOnly, cancellation);
  if (!validated.ok()) {
    if (validated.error().code == core::ErrorCode::cancelled) {
      return validated.error();
    }
    return notice(ResultRecoveryReason::invalidPrefix,
                  validated.error().message);
  }
  const auto destinationDirectory =
      filename.ends_with(".purging.bteresult") ? "Trash" : "Drafts";
  auto moved =
      moveNoClobber(path, root / destinationDirectory / (id + ".bteresult"),
                    "Unable to recover owned Result artifact");
  if (!moved.ok()) {
    return notice(ResultRecoveryReason::invalidPrefix, moved.error().message);
  }
  return std::optional<ResultRecoveryNotice>{};
}

RecoveryOutcome inspectOrphan(const std::filesystem::path &path,
                              const std::filesystem::path &root,
                              const std::filesystem::path &dataStore) {
  const auto id = path.stem().string();
  for (const auto *directory : {"Staging", "Drafts", "Results", "Trash"}) {
    for (const auto *suffix :
         {".bteresult", ".purging.bteresult", ".discarding.bteresult"}) {
      if (std::filesystem::exists(root / directory / (id + suffix))) {
        return std::optional<ResultRecoveryNotice>{};
      }
    }
  }
  auto lease = ResultLease::acquire(root, id, ResultLease::Creation::existing);
  if (!lease.ok()) {
    if (lease.error().code == core::ErrorCode::resourceBusy) {
      return std::optional<ResultRecoveryNotice>{};
    }
    return std::optional<ResultRecoveryNotice>{
        {id, ResultRecoveryReason::invalidLease, lease.error().message}};
  }
  auto retention = data::SegmentRetentionStore::open(dataStore);
  if (!retention.ok()) {
    return std::optional<ResultRecoveryNotice>{
        {id, ResultRecoveryReason::missingReferences,
         retention.error().message}};
  }
  auto references = retention.value()->references(id);
  if (!references.ok()) {
    return std::optional<ResultRecoveryNotice>{
        {id, ResultRecoveryReason::missingReferences,
         references.error().message}};
  }
  if (!references.value().empty()) {
    return std::optional<ResultRecoveryNotice>{
        {id, ResultRecoveryReason::orphanedInitialization,
         "Preserved Result initialization references without a recoverable "
         "artifact"}};
  }
  return std::optional<ResultRecoveryNotice>{};
}

core::Result<std::vector<ResultRecoveryNotice>>
recoverStaging(const std::filesystem::path &root,
               const std::filesystem::path &dataStore,
               const core::CancellationToken &cancellation) {
  std::vector<ResultRecoveryNotice> notices;
  for (const auto *directory : {"Staging", "Leases"}) {
    std::error_code errorCode;
    auto iterator =
        std::filesystem::directory_iterator{root / directory, errorCode};
    const auto end = std::filesystem::directory_iterator{};
    while (!errorCode && iterator != end) {
      if (cancellation.isCancellationRequested()) {
        return core::makeError(core::ErrorCode::cancelled,
                               "Result recovery was cancelled");
      }
      const auto path = iterator->path();
      if (iterator->is_regular_file(errorCode) &&
          path.extension() == (std::string_view{directory} == "Staging"
                                   ? ".bteresult"
                                   : ".sqlite3")) {
        auto outcome =
            std::string_view{directory} == "Staging"
                ? recoverArtifact(path, root, dataStore, cancellation)
                : inspectOrphan(path, root, dataStore);
        if (!outcome.ok()) {
          return outcome.error();
        }
        if (outcome.value().has_value()) {
          notices.push_back(*outcome.value());
        }
      }
      iterator.increment(errorCode);
    }
    if (errorCode) {
      return storageError("Unable to inspect Result recovery directory: " +
                          errorCode.message());
    }
  }
  return notices;
}

} // namespace

#if defined(BTE_ENABLE_TEST_HOOKS)
namespace testing {

void failNext(const FailurePoint point) noexcept { failAfter(point, 0); }

void failAfter(const FailurePoint point,
               const std::uint32_t occurrencesToSkip) noexcept {
  configuredFailureSkips().store(occurrencesToSkip);
  configuredFailurePoint().store(point);
}

void clearFailure() {
  configuredFailurePoint().store(FailurePoint::none);
  configuredFailureSkips().store(0);
  openResultWithDataCancellationSource().store(nullptr);
  const std::scoped_lock lock{forcedResultIdMutex()};
  forcedResultId().reset();
}

bool failurePending() noexcept {
  return configuredFailurePoint().load() != FailurePoint::none;
}

void forceNextResultId(std::string resultId) {
  const std::scoped_lock lock{forcedResultIdMutex()};
  forcedResultId() = std::move(resultId);
}

void cancelNextOpenResultWithDataAfterReaderOpen(
    core::CancellationSource *source) noexcept {
  openResultWithDataCancellationSource().store(source);
}

} // namespace testing
#endif

namespace {

core::Result<void>
validateRecordBatch(const RunDescriptor &descriptor,
                    const std::vector<CanonicalRecord> &previous,
                    const std::span<const CanonicalRecord> records) {
  auto expected = previous.size();
  auto previousTimestamp = previous.empty() ? std::optional<core::Timestamp>{}
                                            : previous.back().timestamp;
  for (const auto &record : records) {
    if (record.sequence != expected || record.symbol.empty() ||
        (previousTimestamp.has_value() &&
         *previousTimestamp > record.timestamp)) {
      return core::makeError(core::ErrorCode::invalidArgument,
                             "Canonical Result records are out of order");
    }
    auto recordValidation = validateCanonicalRecord(
        record, descriptor, core::ErrorCode::invalidArgument);
    if (!recordValidation.ok()) {
      return recordValidation.error();
    }
    ++expected;
    previousTimestamp = record.timestamp;
  }
  return {};
}

core::Result<void>
insertRecordBatch(Database &database,
                  const std::span<const CanonicalRecord> records) {
  for (const auto &record : records) {
    Statement statement;
    auto prepared = statement.prepare(
        database, "INSERT INTO canonical_records VALUES("
                  "?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14)");
    if (!prepared.ok() ||
        !statement.integer(1, static_cast<std::int64_t>(record.sequence))
             .ok() ||
        !statement.integer(2, core::time::toUnixMillis(record.timestamp))
             .ok() ||
        !statement.text(3, record.symbol).ok() ||
        !statement.integer(4, static_cast<std::int64_t>(record.family)).ok() ||
        !statement.integer(5, static_cast<std::int64_t>(record.side)).ok() ||
        !statement.optionalInteger(6, record.quantityShares).ok() ||
        !statement.optionalInteger(7, record.priceNanodollars).ok() ||
        !statement.optionalInteger(8, record.amountMicrodollars).ok() ||
        !statement.optionalInteger(9, record.cashMicrodollars).ok() ||
        !statement.optionalInteger(10, record.marketValueMicrodollars).ok() ||
        !statement.optionalInteger(11, record.equityMicrodollars).ok() ||
        !statement.optionalInteger(12, record.pnlMicrodollars).ok() ||
        !statement.optionalInteger(13, record.positionShares).ok() ||
        !statement.text(14, record.text).ok()) {
      return database.error("Unable to bind canonical record");
    }
    auto inserted = statement.done();
    if (!inserted.ok()) {
      return inserted.error();
    }
  }
  return {};
}

} // namespace

struct ResultWriter::Impl {
  std::filesystem::path root;
  std::filesystem::path dataStore;
  std::filesystem::path stagingPath;
  std::string id;
  RunDescriptor descriptor;
  std::vector<CanonicalRecord> records;
  std::unique_ptr<ResultLease> lease;
  std::unique_ptr<Database> database;
  bool finalized = false;
};

ResultWriter::ResultWriter(ConstructionKey, std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}

ResultWriter::~ResultWriter() { impl_->database.reset(); }

const std::string &ResultWriter::resultId() const noexcept { return impl_->id; }

core::Result<void>
ResultWriter::append(const std::vector<CanonicalRecord> &records) {
  if (impl_->finalized || impl_->database == nullptr || records.empty()) {
    return core::makeError(core::ErrorCode::invalidArgument,
                           "Result record batch is invalid");
  }
  auto validated =
      validateRecordBatch(impl_->descriptor, impl_->records, records);
  if (!validated.ok()) {
    return validated.error();
  }
  auto begun = impl_->database->execute("BEGIN IMMEDIATE");
  if (!begun.ok()) {
    return begun.error();
  }
  if (consumeFailure(testing::FailurePoint::recordTransaction)) {
    const auto rolledBack = impl_->database->execute("ROLLBACK");
    static_cast<void>(rolledBack);
    return injectedFailure("record transaction");
  }
  auto inserted = insertRecordBatch(*impl_->database, records);
  if (!inserted.ok()) {
    (void)impl_->database->execute("ROLLBACK");
    return inserted.error();
  }
  auto committed = impl_->database->execute("COMMIT");
  if (!committed.ok()) {
    const auto ignored = impl_->database->execute("ROLLBACK");
    static_cast<void>(ignored);
    return committed.error();
  }
  impl_->records.insert(impl_->records.end(), records.begin(), records.end());
  return {};
}

core::Result<FinalizedResult>
ResultWriter::finalizeDraft(const RunStatus status, const FinalSummary &summary,
                            const std::string &terminalReason) {
  return finalize(status, summary, terminalReason, FinalizationTarget::draft);
}

core::Result<FinalizedResult> ResultWriter::finalizeDraft(
    const RunStatus status, const FinalSummary &summary,
    const std::string &terminalReason,
    const std::span<const CanonicalRecord> terminalTail) {
  return finalize(status, summary, terminalReason, FinalizationTarget::draft,
                  terminalTail);
}

core::Result<FinalizedResult>
ResultWriter::finalizeAndPromote(const RunStatus status,
                                 const FinalSummary &summary,
                                 const std::string &terminalReason) {
  return finalize(status, summary, terminalReason,
                  FinalizationTarget::promoted);
}

core::Result<FinalizedResult>
// NOLINTNEXTLINE(readability-function-cognitive-complexity): transaction
ResultWriter::finalize(const RunStatus status, const FinalSummary &summary,
                       const std::string &terminalReason,
                       const FinalizationTarget target,
                       const std::span<const CanonicalRecord> terminalTail) {
  if (impl_->finalized || impl_->database == nullptr ||
      !validRunStatus(status) || status == RunStatus::running ||
      (status == RunStatus::completed &&
       (!summary.finalEquityMicrodollars.has_value() ||
        !summary.pnlMicrodollars.has_value())) ||
      (status != RunStatus::completed &&
       (summary.finalEquityMicrodollars.has_value() ||
        summary.pnlMicrodollars.has_value()))) {
    return core::makeError(core::ErrorCode::invalidArgument,
                           "Result terminal status or summary is invalid");
  }
  auto tailValidation =
      validateRecordBatch(impl_->descriptor, impl_->records, terminalTail);
  if (!tailValidation.ok()) {
    return tailValidation.error();
  }
  const auto hash = canonicalHash(impl_->descriptor, impl_->records, status,
                                  terminalReason, summary, terminalTail);
  if (consumeFailure(testing::FailurePoint::hashFinalization)) {
    return injectedFailure("hash finalization");
  }
  auto begun = impl_->database->execute("BEGIN IMMEDIATE");
  if (!begun.ok()) {
    return begun.error();
  }
  auto tailInserted = insertRecordBatch(*impl_->database, terminalTail);
  if (!tailInserted.ok()) {
    (void)impl_->database->execute("ROLLBACK");
    return tailInserted.error();
  }
  const auto saved = nowUtcMillis();
  {
    Statement summaryStatement;
    auto prepared = summaryStatement.prepare(
        *impl_->database, "INSERT INTO summary VALUES(?1,?2)");
    if (!prepared.ok() ||
        !summaryStatement.optionalInteger(1, summary.finalEquityMicrodollars)
             .ok() ||
        !summaryStatement.optionalInteger(2, summary.pnlMicrodollars).ok()) {
      const auto ignored = impl_->database->execute("ROLLBACK");
      static_cast<void>(ignored);
      return impl_->database->error("Unable to bind Result summary");
    }
    auto written = summaryStatement.done();
    if (!written.ok()) {
      const auto ignored = impl_->database->execute("ROLLBACK");
      static_cast<void>(ignored);
      return written.error();
    }
    Statement meta;
    prepared = meta.prepare(
        *impl_->database, "UPDATE result_meta SET status=?1,canonical_hash=?2,"
                          "terminal_reason=?3,saved_utc_ms=?4");
    if (!prepared.ok() ||
        !meta.integer(1, static_cast<std::int64_t>(status)).ok() ||
        !meta.text(2, hash).ok() || !meta.text(3, terminalReason).ok() ||
        !meta.integer(4, saved).ok()) {
      const auto ignored = impl_->database->execute("ROLLBACK");
      static_cast<void>(ignored);
      return impl_->database->error("Unable to bind Result finalization");
    }
    written = meta.done();
    if (!written.ok()) {
      const auto ignored = impl_->database->execute("ROLLBACK");
      static_cast<void>(ignored);
      return written.error();
    }
  }
  auto committed = impl_->database->execute("COMMIT");
  if (!committed.ok()) {
    const auto ignored = impl_->database->execute("ROLLBACK");
    static_cast<void>(ignored);
    return committed.error();
  }
  impl_->records.insert(impl_->records.end(), terminalTail.begin(),
                        terminalTail.end());
  auto checkpointed =
      impl_->database->execute("PRAGMA wal_checkpoint(TRUNCATE)");
  if (!checkpointed.ok()) {
    return checkpointed.error();
  }
  auto closed = impl_->database->close();
  if (!closed.ok()) {
    return closed.error();
  }
  impl_->database.reset();
  if (consumeFailure(testing::FailurePoint::close)) {
    return injectedFailure("close");
  }

  auto reopened = readResultFile(impl_->stagingPath, impl_->dataStore,
                                 DataValidationPolicy::validateExact);
  if (!reopened.ok()) {
    return reopened.error();
  }
  std::vector<std::string> segmentIds;
  for (const auto &span : impl_->descriptor.dataSelection.spans) {
    segmentIds.push_back(span.segmentId);
  }
  std::ranges::sort(segmentIds);
  const auto uniqueEnd = std::ranges::unique(segmentIds).begin();
  segmentIds.erase(uniqueEnd, segmentIds.end());
  auto retention = data::SegmentRetentionStore::open(impl_->dataStore);
  if (!retention.ok()) {
    return retention.error();
  }
  auto acquired = retention.value()->acquire(impl_->id, segmentIds);
  if (!acquired.ok()) {
    return acquired.error();
  }

  const auto promoted = target == FinalizationTarget::promoted;
  const auto destination = impl_->root / (promoted ? "Results" : "Drafts") /
                           (impl_->id + ".bteresult");
  const auto rollBackCatalog = [&] {
    return deleteCatalog(impl_->root, impl_->id);
  };
  if (promoted) {
    if (consumeFailure(testing::FailurePoint::catalogVisibility)) {
      return injectedFailure("catalog visibility");
    }
    auto cataloged = upsertCatalog(impl_->root, {.resultId = impl_->id,
                                                 .displayName = {},
                                                 .canonicalResultHash = hash,
                                                 .status = status,
                                                 .savedUtcMillis = saved,
                                                 .available = true,
                                                 .unavailableReason = {}});
    if (!cataloged.ok()) {
      return cataloged.error();
    }
  }
  if (consumeFailure(testing::FailurePoint::promotion)) {
    if (promoted) {
      const auto ignored = rollBackCatalog();
      static_cast<void>(ignored);
    }
    return injectedFailure("promotion");
  }
  std::error_code errorCode;
  std::filesystem::create_hard_link(impl_->stagingPath, destination, errorCode);
  if (errorCode) {
    if (promoted) {
      const auto ignored = rollBackCatalog();
      static_cast<void>(ignored);
    }
    return storageError("Unable to finalize Result without clobbering: " +
                        errorCode.message());
  }
  std::filesystem::remove(impl_->stagingPath, errorCode);
  // The staging file was just read, flushed, closed, and hard-linked. Removal
  // failure is a host filesystem race; promotion injection covers rollback.
  // GCOVR_EXCL_START
  if (errorCode) {
    std::error_code ignoredFilesystemError;
    std::filesystem::remove(destination, ignoredFilesystemError);
    if (promoted) {
      const auto ignoredCatalog = rollBackCatalog();
      static_cast<void>(ignoredCatalog);
    }
    return storageError("Unable to remove finalized staging Result: " +
                        errorCode.message());
  }
  // GCOVR_EXCL_STOP
  impl_->finalized = true;
  impl_->lease.reset();
  return FinalizedResult{.resultId = impl_->id, .canonicalResultHash = hash};
}

ResultStore::ResultStore(ConstructionKey, std::filesystem::path root,
                         std::filesystem::path dataStore,
                         std::vector<ResultRecoveryNotice> notices)
    : root_(std::move(root)), dataStore_(std::move(dataStore)),
      recoveryNotices_(std::move(notices)) {}

const std::vector<ResultRecoveryNotice> &
ResultStore::recoveryNotices() const noexcept {
  return recoveryNotices_;
}

core::Result<std::unique_ptr<ResultStore>>
ResultStore::open(const std::filesystem::path &root,
                  const std::filesystem::path &dataStore,
                  const core::CancellationToken &cancellation) {
  if (cancellation.isCancellationRequested()) {
    return core::makeError(core::ErrorCode::cancelled,
                           "Result store open was cancelled");
  }
  if (root.empty() || dataStore.empty()) {
    return core::makeError(core::ErrorCode::invalidArgument,
                           "Result and Data store paths are required");
  }
  try {
    for (const auto *const name :
         {"Staging", "Drafts", "Results", "Trash", "Quarantine", "Leases"}) {
      std::filesystem::create_directories(root / name);
    }
  } catch (const std::filesystem::filesystem_error &error) {
    return core::makeError(core::ErrorCode::permissionDenied,
                           "Unable to create Result store: " +
                               std::string{error.what()});
  }
  auto initialized = initializeCatalog(root);
  if (!initialized.ok()) {
    return initialized.error();
  }
  auto recovered = recoverStaging(root, dataStore, cancellation);
  if (!recovered.ok()) {
    return recovered.error();
  }
  return std::make_unique<ResultStore>(ConstructionKey{}, root, dataStore,
                                       std::move(recovered).value());
}

core::Result<std::unique_ptr<ResultWriter>>
ResultStore::begin(const RunDescriptor &descriptor) const {
  auto validated = validateDescriptor(descriptor);
  if (!validated.ok()) {
    return validated.error();
  }
  auto exact = validateDataSelection(dataStore_, descriptor.dataSelection);
  if (!exact.ok()) {
    return exact.error();
  }
  for (int attempt = 0; attempt < 8; ++attempt) {
    const auto resultId = allocateResultId();
    const auto stagingPath = root_ / "Staging" / (resultId + ".bteresult");
    if (hasResultArtifact(root_, resultId)) {
      continue;
    }
    auto lease =
        ResultLease::acquire(root_, resultId, ResultLease::Creation::fresh);
    if (!lease.ok()) {
      if (lease.error().code == core::ErrorCode::resourceBusy) {
        continue;
      }
      return lease.error();
    }
    auto retention = data::SegmentRetentionStore::open(dataStore_);
    if (!retention.ok()) {
      return retention.error();
    }
    auto pinned =
        retention.value()->acquire(resultId, distinctSegments(descriptor));
    if (!pinned.ok()) {
      return pinned.error();
    }
    auto revalidated =
        validateDataSelection(dataStore_, descriptor.dataSelection);
    if (!revalidated.ok()) {
      return revalidated.error();
    }
    auto database = openDatabase(stagingPath);
    if (!database.ok()) {
      return database.error();
    }
    if (consumeFailure(testing::FailurePoint::schemaCreation)) {
      return injectedFailure("schema creation");
    }
    auto schema = createResultSchema(*database.value(), descriptor, resultId);
    if (!schema.ok()) {
      return schema.error();
    }
    auto impl = std::make_unique<ResultWriter::Impl>();
    impl->root = root_;
    impl->dataStore = dataStore_;
    impl->stagingPath = stagingPath;
    impl->id = resultId;
    impl->descriptor = descriptor;
    impl->lease = std::move(lease).value();
    impl->database = std::move(database).value();
    return std::make_unique<ResultWriter>(ResultWriter::ConstructionKey{},
                                          std::move(impl));
  }
  return storageError("Unable to allocate a unique Result ID");
}

core::Result<void> ResultStore::validateSelectedBars(
    const data::DataSelectionIdentity &identity,
    const std::vector<data::SnapshotBar> &bars,
    const core::CancellationToken &cancellation) const {
  auto reader = data::ReleaseSnapshotReader::open(
      dataStore_, identity.snapshotId, cancellation);
  // Snapshot open/read errors are covered by Data contract tests and begin /
  // Replay integration tests; this validation seam preserves those errors.
  // GCOVR_EXCL_START
  if (!reader.ok()) {
    return reader.error();
  }
  auto exact = reader.value()->readExact(identity, cancellation);
  if (!exact.ok()) {
    return exact.error();
  }
  // GCOVR_EXCL_STOP
  if (exact.value().bars != bars) {
    return core::makeError(
        core::ErrorCode::invalidArgument,
        "Backtest bars do not match the immutable Data Selection");
  }
  return {};
}

struct ArtifactLocations final {
  std::filesystem::path directory;
  std::filesystem::path dataStore;
};

core::Result<std::vector<ResultSummary>>
listArtifacts(const ArtifactLocations &locations,
              const std::string &description,
              const core::CancellationToken &cancellation) {
  if (cancellation.isCancellationRequested()) {
    return core::makeError(core::ErrorCode::cancelled,
                           description + " load was cancelled");
  }
  std::vector<ResultSummary> summaries;
  std::error_code errorCode;
  const auto entries =
      std::filesystem::directory_iterator(locations.directory, errorCode);
  if (errorCode) {
    return storageError("Unable to inspect " + description + ": " +
                        errorCode.message());
  }
  for (const auto &entry : entries) {
    if (cancellation.isCancellationRequested()) {
      return core::makeError(core::ErrorCode::cancelled,
                             description + " load was cancelled");
    }
    if (!entry.is_regular_file() || entry.path().extension() != ".bteresult") {
      continue;
    }
    const auto resultId = entry.path().stem().string();
    auto opened = readResultFile(
        entry.path(), locations.dataStore, DataValidationPolicy::validateExact,
        ResultStatusPolicy::terminalOnly, cancellation);
    if (!opened.ok()) {
      if (cancellation.isCancellationRequested()) {
        return core::makeError(core::ErrorCode::cancelled,
                               description + " load was cancelled");
      }
      summaries.push_back({.resultId = resultId,
                           .displayName = {},
                           .canonicalResultHash = {},
                           .status = RunStatus::incomplete,
                           .savedUtcMillis = 0,
                           .available = false,
                           .unavailableReason = opened.error().message});
      continue;
    }
    summaries.push_back(
        {.resultId = resultId,
         .displayName = opened.value().displayName,
         .canonicalResultHash = opened.value().canonicalResultHash,
         .status = opened.value().status,
         .savedUtcMillis = opened.value().savedUtcMillis,
         .available = true,
         .unavailableReason = {}});
  }
  if (cancellation.isCancellationRequested()) {
    return core::makeError(core::ErrorCode::cancelled,
                           description + " load was cancelled");
  }
  std::ranges::sort(summaries, [](const auto &left, const auto &right) {
    if (left.savedUtcMillis != right.savedUtcMillis) {
      return left.savedUtcMillis > right.savedUtcMillis;
    }
    return left.resultId < right.resultId;
  });
  return summaries;
}

core::Result<std::vector<ResultSummary>>
ResultStore::list(const core::CancellationToken &cancellation) const {
  return listArtifacts(
      {.directory = root_ / "Results", .dataStore = dataStore_},
      "Result catalog", cancellation);
}

core::Result<std::vector<ResultSummary>>
ResultStore::listDrafts(const core::CancellationToken &cancellation) const {
  return listArtifacts({.directory = root_ / "Drafts", .dataStore = dataStore_},
                       "Result drafts", cancellation);
}

core::Result<std::vector<ResultSummary>>
ResultStore::listTrash(const core::CancellationToken &cancellation) const {
  return listArtifacts({.directory = root_ / "Trash", .dataStore = dataStore_},
                       "Result Trash", cancellation);
}

core::Result<OpenedResult>
ResultStore::openResult(const std::string &resultId,
                        const core::CancellationToken &cancellation) const {
  auto path = existingResultPath(root_, resultId);
  if (!path.ok()) {
    return path.error();
  }
  return readResultFile(path.value(), dataStore_,
                        DataValidationPolicy::validateExact,
                        ResultStatusPolicy::terminalOnly, cancellation);
}

core::Result<OpenedResultWithData> ResultStore::openResultWithData(
    const std::string &resultId,
    const core::CancellationToken &cancellation) const {
  auto path = existingResultPath(root_, resultId);
  if (!path.ok()) {
    return path.error();
  }
  // The exact snapshot is read immediately below; defer its validation here
  // to avoid opening and decoding the same immutable segments twice.
  auto result = readResultFile(path.value(), dataStore_,
                               DataValidationPolicy::deferToCaller,
                               ResultStatusPolicy::terminalOnly, cancellation);
  if (!result.ok()) {
    return result.error();
  }
  auto reader = data::ReleaseSnapshotReader::open(
      dataStore_, result.value().descriptor.dataSelection.snapshotId,
      cancellation);
  if (!reader.ok()) {
    return reader.error();
  }
#if defined(BTE_ENABLE_TEST_HOOKS)
  if (const auto source =
          openResultWithDataCancellationSource().exchange(nullptr);
      source != nullptr) {
    source->requestCancellation();
  }
#endif
  auto selection = reader.value()->readExact(
      result.value().descriptor.dataSelection, cancellation);
  if (!selection.ok()) {
    return selection.error();
  }
  return OpenedResultWithData{.result = std::move(result).value(),
                              .selection = std::move(selection).value()};
}

core::Result<FinalizedResult>
ResultStore::publishDraft(const std::string &resultId,
                          const std::string &displayName) const {
  if (!validResultId(resultId)) {
    return core::makeError(core::ErrorCode::invalidArgument,
                           "Result ID is invalid");
  }
  if (!displayName.empty() && !validDisplayName(displayName)) {
    return core::makeError(core::ErrorCode::invalidArgument,
                           "Result name must be 1–160 bytes without leading, "
                           "trailing, or control characters");
  }
  auto lease = mutationLease(root_, dataStore_, resultId);
  if (!lease.ok()) {
    return lease.error();
  }

  const auto filename = resultId + ".bteresult";
  const auto resultPath = root_ / "Results" / filename;
  if (std::filesystem::exists(resultPath)) {
    auto opened = readResultFile(resultPath, dataStore_,
                                 DataValidationPolicy::validateExact);
    if (!opened.ok()) {
      return opened.error();
    }
    return FinalizedResult{
        .resultId = resultId,
        .canonicalResultHash = opened.value().canonicalResultHash,
    };
  }

  const auto draftPath = root_ / "Drafts" / filename;
  if (!std::filesystem::exists(draftPath)) {
    return core::makeError(core::ErrorCode::notFound,
                           "Result draft is unavailable");
  }
  auto opened = readResultFile(draftPath, dataStore_,
                               DataValidationPolicy::validateExact);
  if (!opened.ok()) {
    return opened.error();
  }
  if (!displayName.empty()) {
    auto named = writeDisplayName(draftPath, displayName);
    if (!named.ok()) {
      return named.error();
    }
  }
  const auto summary = ResultSummary{
      .resultId = resultId,
      .displayName =
          displayName.empty() ? opened.value().displayName : displayName,
      .canonicalResultHash = opened.value().canonicalResultHash,
      .status = opened.value().status,
      .savedUtcMillis = opened.value().savedUtcMillis,
      .available = true,
      .unavailableReason = {},
  };
  if (consumeFailure(testing::FailurePoint::catalogVisibility)) {
    return injectedFailure("catalog visibility");
  }
  auto cataloged = upsertCatalog(root_, summary);
  if (!cataloged.ok()) {
    return cataloged.error();
  }
  const auto rollBackCatalog = [&] { return deleteCatalog(root_, resultId); };
  if (consumeFailure(testing::FailurePoint::promotion)) {
    const auto ignored = rollBackCatalog();
    static_cast<void>(ignored);
    return injectedFailure("promotion");
  }
  auto promoted =
      consumeFailure(testing::FailurePoint::draftPromotionMove)
          ? core::Result<void>{injectedFailure("draft promotion move")}
          : moveNoClobber(draftPath, resultPath,
                          "Unable to publish Result draft");
  if (!promoted.ok()) {
    const auto ignored = rollBackCatalog();
    static_cast<void>(ignored);
    return promoted.error();
  }
  return FinalizedResult{
      .resultId = resultId,
      .canonicalResultHash = opened.value().canonicalResultHash,
  };
}

core::Result<void>
ResultStore::renameResult(const std::string &resultId,
                          const std::string &displayName) const {
  if (!validResultId(resultId) || !validDisplayName(displayName)) {
    return core::makeError(core::ErrorCode::invalidArgument,
                           "Result ID or name is invalid");
  }
  auto lease = mutationLease(root_, dataStore_, resultId);
  if (!lease.ok()) {
    return lease.error();
  }

  const auto path = root_ / "Results" / (resultId + ".bteresult");
  if (!std::filesystem::exists(path)) {
    return core::makeError(core::ErrorCode::notFound, "Result is unavailable");
  }
  auto opened =
      readResultFile(path, dataStore_, DataValidationPolicy::validateExact);
  if (!opened.ok()) {
    return opened.error();
  }
  return writeDisplayName(path, displayName);
}

core::Result<void>
ResultStore::discardDraft(const std::string &resultId) const {
  if (!validResultId(resultId)) {
    return core::makeError(core::ErrorCode::invalidArgument,
                           "Result ID is invalid");
  }
  auto lease = mutationLease(root_, dataStore_, resultId);
  if (!lease.ok()) {
    return lease.error();
  }

  const auto draftPath = root_ / "Drafts" / (resultId + ".bteresult");
  if (!std::filesystem::exists(draftPath)) {
    return core::makeError(core::ErrorCode::notFound,
                           "Result draft is unavailable");
  }
  auto opened = readResultFile(draftPath, dataStore_,
                               DataValidationPolicy::deferToCaller);
  if (!opened.ok()) {
    return opened.error();
  }
  std::vector<std::string> segmentIds;
  segmentIds.reserve(opened.value().descriptor.dataSelection.spans.size());
  for (const auto &span : opened.value().descriptor.dataSelection.spans) {
    segmentIds.push_back(span.segmentId);
  }
  std::ranges::sort(segmentIds);
  const auto duplicates = std::ranges::unique(segmentIds);
  segmentIds.erase(duplicates.begin(), duplicates.end());

  const auto discardingPath =
      root_ / "Staging" / (resultId + ".discarding.bteresult");
  auto staged = moveNoClobber(draftPath, discardingPath,
                              "Unable to stage Result draft discard");
  if (!staged.ok()) {
    return staged.error();
  }
  const auto restoreDraft = [&] {
    return moveNoClobber(discardingPath, draftPath,
                         "Unable to roll back Result draft discard");
  };
  auto retention = data::SegmentRetentionStore::open(dataStore_);
  if (!retention.ok()) {
    const auto restored = restoreDraft();
    return restored.ok() ? retention.error() : restored.error();
  }
  auto released = retention.value()->release(resultId);
  if (!released.ok()) {
    const auto restored = restoreDraft();
    return restored.ok() ? released.error() : restored.error();
  }
  std::error_code errorCode;
  if (consumeFailure(testing::FailurePoint::draftDiscardRemoval)) {
    errorCode = std::make_error_code(std::errc::io_error);
  } else {
    std::filesystem::remove(discardingPath, errorCode);
  }
  if (errorCode) {
    auto reacquired = retention.value()->acquire(resultId, segmentIds);
    auto restored = restoreDraft();
    if (!reacquired.ok()) {
      return reacquired.error();
    }
    if (!restored.ok()) {
      return restored.error();
    }
    return storageError("Unable to discard Result draft: " +
                        errorCode.message());
  }
  return {};
}

core::Result<void> ResultStore::moveToTrash(const std::string &resultId) const {
  if (!validResultId(resultId)) {
    return core::makeError(core::ErrorCode::invalidArgument,
                           "Result ID is invalid");
  }
  auto lease = mutationLease(root_, dataStore_, resultId);
  if (!lease.ok()) {
    return lease.error();
  }

  auto moved = moveNoClobber(root_ / "Results" / (resultId + ".bteresult"),
                             root_ / "Trash" / (resultId + ".bteresult"),
                             "Unable to move Result to Trash");
  if (!moved.ok()) {
    return moved.error();
  }
  return deleteCatalog(root_, resultId);
}

core::Result<void> ResultStore::restore(const std::string &resultId) const {
  if (!validResultId(resultId)) {
    return core::makeError(core::ErrorCode::invalidArgument,
                           "Result ID is invalid");
  }
  auto lease = mutationLease(root_, dataStore_, resultId);
  if (!lease.ok()) {
    return lease.error();
  }

  const auto source = root_ / "Trash" / (resultId + ".bteresult");
  auto opened =
      readResultFile(source, dataStore_, DataValidationPolicy::validateExact);
  if (!opened.ok()) {
    return opened.error();
  }
  const auto destination = root_ / "Results" / (resultId + ".bteresult");
  auto moved = moveNoClobber(source, destination, "Unable to restore Result");
  if (!moved.ok()) {
    return moved.error();
  }
  return upsertCatalog(
      root_, {.resultId = resultId,
              .displayName = opened.value().displayName,
              .canonicalResultHash = opened.value().canonicalResultHash,
              .status = opened.value().status,
              .savedUtcMillis = nowUtcMillis(),
              .available = true,
              .unavailableReason = {}});
}

core::Result<void> ResultStore::purge(const std::string &resultId) const {
  if (!validResultId(resultId)) {
    return core::makeError(core::ErrorCode::invalidArgument,
                           "Result ID is invalid");
  }
  auto lease = mutationLease(root_, dataStore_, resultId);
  if (!lease.ok()) {
    return lease.error();
  }

  const auto path = root_ / "Trash" / (resultId + ".bteresult");
  if (!std::filesystem::exists(path)) {
    return core::makeError(core::ErrorCode::notFound,
                           "Trashed Result is unavailable");
  }
  if (!std::filesystem::is_regular_file(path)) {
    return storageError("Unable to purge Result: artifact is not a file");
  }
  auto opened =
      readResultFile(path, dataStore_, DataValidationPolicy::deferToCaller);
  // Corrupt Result read failures are covered by open/import contract tests;
  // purge deliberately forwards the same structured error. GCOVR_EXCL_START
  if (!opened.ok()) {
    return opened.error();
  }
  // GCOVR_EXCL_STOP
  std::vector<std::string> segmentIds;
  segmentIds.reserve(opened.value().descriptor.dataSelection.spans.size());
  for (const auto &span : opened.value().descriptor.dataSelection.spans) {
    segmentIds.push_back(span.segmentId);
  }
  std::ranges::sort(segmentIds);
  const auto duplicateSegmentIds = std::ranges::unique(segmentIds);
  segmentIds.erase(duplicateSegmentIds.begin(), duplicateSegmentIds.end());

  const auto purgingPath =
      root_ / "Staging" / (resultId + ".purging.bteresult");
  auto staged =
      moveNoClobber(path, purgingPath, "Unable to stage Result purge");
  // move/collision behavior is covered by lifecycle contract tests; this seam
  // forwards the same no-clobber failure. GCOVR_EXCL_START
  if (!staged.ok()) {
    return staged.error();
  }
  // GCOVR_EXCL_STOP
  const auto restoreResult = [&] {
    return moveNoClobber(purgingPath, path, "Unable to roll back Result purge");
  };
  auto retention = data::SegmentRetentionStore::open(dataStore_);
  if (!retention.ok()) {
    auto restored = restoreResult();
    // restoreResult can fail only after another actor mutates the just-vacated
    // Trash destination. GCOVR_EXCL_START
    if (!restored.ok()) {
      return restored.error();
    }
    // GCOVR_EXCL_STOP
    return retention.error();
  }
  auto released = retention.value()->release(resultId);
  // Release failures are fault-injected in SegmentRetention and the public
  // purge rollback is verified above; preserve the underlying error.
  // GCOVR_EXCL_START
  if (!released.ok()) {
    auto restored = restoreResult();
    // The same external mutation race applies to this rollback path.
    // GCOVR_EXCL_START
    if (!restored.ok()) {
      return restored.error();
    }
    // GCOVR_EXCL_STOP
    return released.error();
  }
  // GCOVR_EXCL_STOP
  std::error_code errorCode;
  std::filesystem::remove(purgingPath, errorCode);
  // purgingPath is a validated regular file owned by this operation. Failure
  // after release requires an external filesystem race; retention reacquire
  // and restore are defensive rollback. GCOVR_EXCL_START
  if (errorCode) {
    auto reacquired = retention.value()->acquire(resultId, segmentIds);
    auto restored = restoreResult();
    if (!reacquired.ok()) {
      return reacquired.error();
    }
    if (!restored.ok()) {
      return restored.error();
    }
    return storageError("Unable to purge Result: " + errorCode.message());
  }
  // GCOVR_EXCL_STOP
  return deleteCatalog(root_, resultId);
}

core::Result<FinalizedResult>
// NOLINTNEXTLINE(readability-function-cognitive-complexity): transaction
ResultStore::importResult(const std::filesystem::path &source) const {
  if (source.empty() || !std::filesystem::is_regular_file(source)) {
    return core::makeError(core::ErrorCode::notFound,
                           "Imported Result source is unavailable");
  }
  auto sourceResult =
      readResultFile(source, dataStore_, DataValidationPolicy::validateExact,
                     ResultStatusPolicy::terminalOnly, {}, true);
  if (!sourceResult.ok()) {
    return sourceResult.error();
  }

  std::string importedId;
  std::filesystem::path stagingPath;
  std::unique_ptr<ResultLease> lease;
  for (int attempt = 0; attempt < 8; ++attempt) {
    const auto candidate = allocateResultId();
    if (hasResultArtifact(root_, candidate)) {
      continue;
    }
    auto acquiredLease =
        ResultLease::acquire(root_, candidate, ResultLease::Creation::fresh);
    if (!acquiredLease.ok()) {
      if (acquiredLease.error().code == core::ErrorCode::resourceBusy) {
        continue;
      }
      return acquiredLease.error();
    }
    lease = std::move(acquiredLease).value();
    importedId = candidate;
    stagingPath = root_ / "Staging" / (candidate + ".bteresult");
    break;
  }
  if (importedId.empty()) {
    return storageError("Unable to allocate a unique imported Result ID");
  }
  auto retention = data::SegmentRetentionStore::open(dataStore_);
  if (!retention.ok()) {
    return retention.error();
  }
  auto pinned = retention.value()->acquire(
      importedId, distinctSegments(sourceResult.value().descriptor));
  if (!pinned.ok()) {
    return pinned.error();
  }
  auto exact = validateDataSelection(
      dataStore_, sourceResult.value().descriptor.dataSelection);
  if (!exact.ok()) {
    return exact.error();
  }
  std::error_code copyError;
  if (!std::filesystem::copy_file(source, stagingPath,
                                  std::filesystem::copy_options::none,
                                  copyError)) {
    return storageError("Unable to stage imported Result: " +
                        copyError.message());
  }

  auto database = openDatabase(stagingPath, false, false);
  if (!database.ok()) {
    return database.error();
  }
  auto databaseOwner = std::move(database).value();
  Statement metadata;
  auto prepared = metadata.prepare(
      *databaseOwner,
      "UPDATE result_meta SET result_id=?1,created_utc_ms=?2,saved_utc_ms=?2");
  const auto saved = nowUtcMillis();
  if (!prepared.ok() || !metadata.text(1, importedId).ok() ||
      !metadata.integer(2, saved).ok()) {
    return databaseOwner->error("Unable to bind imported Result identity");
  }
  auto updated = metadata.done();
  if (!updated.ok()) {
    return updated.error();
  }
  auto checkpointed = databaseOwner->execute("PRAGMA wal_checkpoint(TRUNCATE)");
  if (!checkpointed.ok()) {
    return checkpointed.error();
  }
  databaseOwner.reset();

  auto validated = readResultFile(stagingPath, dataStore_,
                                  DataValidationPolicy::validateExact);
  if (!validated.ok()) {
    return validated.error();
  }
  const auto destination = root_ / "Results" / (importedId + ".bteresult");
  std::error_code promotionError;
  std::filesystem::create_hard_link(stagingPath, destination, promotionError);
  if (promotionError) {
    return storageError(
        "Unable to promote imported Result without clobbering: " +
        promotionError.message());
  }
  std::filesystem::remove(stagingPath, promotionError);
  if (promotionError) {
    return storageError("Unable to remove imported staging Result: " +
                        promotionError.message());
  }

  auto cataloged = upsertCatalog(
      root_, {.resultId = importedId,
              .displayName = validated.value().displayName,
              .canonicalResultHash = validated.value().canonicalResultHash,
              .status = validated.value().status,
              .savedUtcMillis = saved,
              .available = true,
              .unavailableReason = {}});
  if (!cataloged.ok()) {
    return cataloged.error();
  }
  return FinalizedResult{.resultId = importedId,
                         .canonicalResultHash =
                             validated.value().canonicalResultHash};
}

} // namespace bte::results
