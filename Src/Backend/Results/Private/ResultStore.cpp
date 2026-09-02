#include "Bte/Results/ResultStore.h"

#include "Bte/Core/Digest.h"
#include "Bte/Core/Result.h"
#include "Bte/Core/Time.h"
#include "Bte/Data/ReleaseSnapshot.h"
#include "Bte/Data/SegmentRetention.h"

#include "ResultStoreTestHooks.h"

#include <sqlite3.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <random>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace bte::results {
namespace {

constexpr auto schemaVersion = 1;
constexpr std::string_view numericPolicy = "fixed-point-v1";
constexpr std::string_view aggregationPolicy = "utcCalendarDayV1";
std::atomic<testing::FailurePoint> failurePoint{testing::FailurePoint::none};

bool consumeFailure(const testing::FailurePoint point) noexcept {
  auto expected = point;
  return failurePoint.compare_exchange_strong(expected,
                                              testing::FailurePoint::none);
}

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

class Database final {
public:
  Database() = default;
  ~Database() {
    if (handle_ != nullptr) {
      sqlite3_close(handle_);
    }
  }
  Database(const Database &) = delete;
  Database &operator=(const Database &) = delete;
  Database(Database &&) = delete;
  Database &operator=(Database &&) = delete;

  [[nodiscard]] core::Result<void> open(const std::filesystem::path &path,
                                        const int flags) {
    if (sqlite3_libversion_number() != 3'053'004) {
      return core::makeError(core::ErrorCode::schemaMismatch,
                             "SQLite 3.53.4 is required");
    }
    if (sqlite3_open_v2(path.string().c_str(), &handle_, flags, nullptr) !=
        SQLITE_OK) {
      return error("Unable to open Result database");
    }
    sqlite3_busy_timeout(handle_, 5'000);
    return {};
  }

  [[nodiscard]] core::Result<void> execute(const std::string_view sql) {
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

  [[nodiscard]] core::Error error(const std::string &message) const {
    const auto detail =
        handle_ == nullptr ? std::string{} : sqlite3_errmsg(handle_);
    return storageError(detail.empty() ? message : message + ": " + detail);
  }

private:
  sqlite3 *handle_ = nullptr;
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
    if (sqlite3_prepare_v2(database.handle(), std::string{sql}.c_str(), -1,
                           &handle_, nullptr) != SQLITE_OK) {
      return database.error("Unable to prepare Result statement");
    }
    return {};
  }

  [[nodiscard]] core::Result<void> text(const int index,
                                        const std::string_view value) {
    if (sqlite3_bind_text(handle_, index, value.data(),
                          static_cast<int>(value.size()),
                          SQLITE_TRANSIENT) != SQLITE_OK) {
      return database_->error("Unable to bind Result text");
    }
    return {};
  }

  [[nodiscard]] core::Result<void> integer(const int index,
                                           const std::int64_t value) {
    if (sqlite3_bind_int64(handle_, index, value) != SQLITE_OK) {
      return database_->error("Unable to bind Result integer");
    }
    return {};
  }

  [[nodiscard]] core::Result<void>
  optionalInteger(const int index, const std::optional<std::int64_t> value) {
    const auto status = value.has_value()
                            ? sqlite3_bind_int64(handle_, index, *value)
                            : sqlite3_bind_null(handle_, index);
    if (status != SQLITE_OK) {
      return database_->error("Unable to bind optional Result integer");
    }
    return {};
  }

  [[nodiscard]] core::Result<void> done() {
    if (sqlite3_step(handle_) != SQLITE_DONE) {
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
openDatabase(const std::filesystem::path &path, const bool readOnly = false) {
  auto database = std::make_unique<Database>();
  const auto flags = readOnly ? SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX
                              : SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE |
                                    SQLITE_OPEN_FULLMUTEX;
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
  for (std::size_t index = 0; index < descriptor.dataSelection.spans.size();
       ++index) {
    const auto &span = descriptor.dataSelection.spans[index];
    if (span.segmentOrdinal != index || span.symbol.empty() ||
        !validHash(span.segmentId) || span.segmentHash != span.segmentId ||
        span.rowCount == 0 || span.firstTimestamp > span.lastTimestamp) {
      return core::makeError(core::ErrorCode::invalidArgument,
                             "Data Selection span is invalid");
    }
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
  return value == nullptr ? std::string{}
                          : std::string{reinterpret_cast<const char *>(value)};
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

void appendInteger(std::string &bytes, const char tag,
                   const std::int64_t value) {
  std::string encoded;
  encoded.reserve(8);
  appendUnsigned(encoded, static_cast<std::uint64_t>(value));
  appendFrame(bytes, tag, encoded);
}

void appendOptionalInteger(std::string &bytes, const char tag,
                           const std::optional<std::int64_t> value) {
  if (!value.has_value()) {
    appendFrame(bytes, tag, std::string_view{});
    return;
  }
  appendInteger(bytes, tag, *value);
}

std::string canonicalHash(const RunDescriptor &descriptor,
                          const std::vector<CanonicalRecord> &records,
                          const RunStatus status,
                          const std::string_view terminalReason,
                          const FinalSummary &summary) {
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
  for (const auto &record : records) {
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
    appendOptionalInteger(bytes, '7', record.positionShares);
    appendFrame(bytes, 'm', record.text);
  }
  appendInteger(bytes, 'z', static_cast<std::int64_t>(status));
  appendFrame(bytes, 'm', terminalReason);
  appendOptionalInteger(bytes, '8', summary.finalEquityMicrodollars);
  appendOptionalInteger(bytes, '9', summary.pnlMicrodollars);
  return core::sha256(bytes);
}

std::string allocateResultId() {
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

core::Result<void> createResultSchema(Database &database,
                                      const RunDescriptor &descriptor,
                                      const std::string &resultId) {
  for (
      const auto sql : {
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
          "CREATE TABLE data_spans(ordinal INTEGER PRIMARY KEY, symbol TEXT "
          "NOT NULL, segment_id TEXT NOT NULL, segment_hash TEXT NOT NULL, "
          "first_row INTEGER NOT NULL, row_count INTEGER NOT NULL, "
          "first_timestamp_ms INTEGER NOT NULL, last_timestamp_ms INTEGER NOT "
          "NULL)",
          "CREATE TABLE canonical_records(sequence INTEGER PRIMARY KEY, "
          "timestamp_ms INTEGER NOT NULL, symbol TEXT NOT NULL, family INTEGER "
          "NOT NULL, side INTEGER NOT NULL, quantity INTEGER, price INTEGER, "
          "amount INTEGER, cash INTEGER, market_value INTEGER, equity INTEGER, "
          "position INTEGER, text TEXT NOT NULL)",
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

core::Result<OpenedResult> readResultFile(const std::filesystem::path &path,
                                          const std::filesystem::path &dataRoot,
                                          const bool validateData,
                                          const bool allowRunning = false) {
  auto database = openDatabase(path, true);
  if (!database.ok()) {
    return core::makeError(core::ErrorCode::schemaMismatch,
                           "Result database cannot be opened");
  }
  Statement integrity;
  auto prepared =
      integrity.prepare(*database.value(), "PRAGMA integrity_check");
  if (!prepared.ok() || integrity.step() != SQLITE_ROW ||
      textColumn(integrity.handle(), 0) != "ok") {
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
    return core::makeError(core::ErrorCode::schemaMismatch,
                           "Result schema or policy is incompatible");
  }
  result.resultId = textColumn(meta.handle(), 1);
  result.status = static_cast<RunStatus>(sqlite3_column_int(meta.handle(), 2));
  result.canonicalResultHash = textColumn(meta.handle(), 3);
  result.terminalReason = textColumn(meta.handle(), 4);
  result.savedUtcMillis = sqlite3_column_int64(meta.handle(), 7);
  if (!validResultId(result.resultId) ||
      (result.status == RunStatus::running && !allowRunning) ||
      (result.status != RunStatus::running &&
       !validHash(result.canonicalResultHash))) {
    return core::makeError(core::ErrorCode::schemaMismatch,
                           "Result metadata is incomplete");
  }

  Statement run;
  prepared = run.prepare(*database.value(),
                         "SELECT universe,range_start_ms,range_end_ms,"
                         "initial_capital,strategy_id,strategy_hash FROM "
                         "run_configuration");
  if (!prepared.ok() || run.step() != SQLITE_ROW) {
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
    return core::makeError(core::ErrorCode::schemaMismatch,
                           "Data Selection is unavailable");
  }
  auto &identity = result.descriptor.dataSelection;
  identity.snapshotId = textColumn(selection.handle(), 0);
  identity.calendarHash = textColumn(selection.handle(), 1);
  identity.splitManifestHash = textColumn(selection.handle(), 2);
  identity.timeframe = textColumn(selection.handle(), 3);
  identity.profile = textColumn(selection.handle(), 4);

  Statement spans;
  prepared = spans.prepare(
      *database.value(), "SELECT ordinal,symbol,segment_id,segment_hash,"
                         "first_row,row_count,first_timestamp_ms,"
                         "last_timestamp_ms FROM data_spans ORDER BY ordinal");
  if (!prepared.ok()) {
    return prepared.error();
  }
  while (spans.step() == SQLITE_ROW) {
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
  auto validated = validateDescriptor(result.descriptor);
  if (!validated.ok()) {
    return core::makeError(core::ErrorCode::schemaMismatch,
                           "Persisted Run descriptor is invalid");
  }

  Statement records;
  prepared = records.prepare(
      *database.value(),
      "SELECT sequence,timestamp_ms,symbol,family,side,quantity,price,amount,"
      "cash,market_value,equity,position,text FROM canonical_records ORDER BY "
      "sequence");
  if (!prepared.ok()) {
    return prepared.error();
  }
  while (records.step() == SQLITE_ROW) {
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
        .positionShares = optionalIntegerColumn(records.handle(), 11),
        .text = textColumn(records.handle(), 12),
    });
  }
  for (std::size_t index = 0; index < result.records.size(); ++index) {
    if (result.records[index].sequence != index ||
        (index > 0 && result.records[index - 1].timestamp >
                          result.records[index].timestamp)) {
      return core::makeError(core::ErrorCode::schemaMismatch,
                             "Canonical record order is invalid");
    }
  }

  Statement summary;
  prepared = summary.prepare(*database.value(),
                             "SELECT final_equity,pnl FROM summary");
  if (!prepared.ok()) {
    return prepared.error();
  }
  if (summary.step() == SQLITE_ROW) {
    result.summary.finalEquityMicrodollars =
        optionalIntegerColumn(summary.handle(), 0);
    result.summary.pnlMicrodollars = optionalIntegerColumn(summary.handle(), 1);
  }
  if (result.status != RunStatus::running &&
      canonicalHash(result.descriptor, result.records, result.status,
                    result.terminalReason,
                    result.summary) != result.canonicalResultHash) {
    return core::makeError(core::ErrorCode::schemaMismatch,
                           "Canonical Result hash is invalid");
  }
  if (validateData) {
    auto reader =
        data::ReleaseSnapshotReader::open(dataRoot, identity.snapshotId);
    if (!reader.ok()) {
      return reader.error();
    }
  }
  return result;
}

core::Result<void> initializeCatalog(const std::filesystem::path &root) {
  auto database = openDatabase(root / "Catalog.sqlite3");
  if (!database.ok()) {
    return database.error();
  }
  for (const auto sql : {
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

core::Result<void> recoverStaging(const std::filesystem::path &root,
                                  const std::filesystem::path &dataStore) {
  std::error_code iterationError;
  for (const auto &entry :
       std::filesystem::directory_iterator(root / "Staging", iterationError)) {
    if (iterationError) {
      return storageError("Unable to inspect staged Results: " +
                          iterationError.message());
    }
    if (!entry.is_regular_file() || entry.path().extension() != ".bteresult") {
      continue;
    }
    auto staged = readResultFile(entry.path(), dataStore, true, true);
    if (!staged.ok() || (staged.value().status == RunStatus::running &&
                         staged.value().records.empty())) {
      std::error_code quarantineError;
      std::filesystem::rename(entry.path(),
                              root / "Quarantine" / entry.path().filename(),
                              quarantineError);
      if (quarantineError) {
        return storageError("Unable to quarantine staged Result: " +
                            quarantineError.message());
      }
      continue;
    }

    if (staged.value().status == RunStatus::running) {
      constexpr auto reason = "Run interrupted before terminal finalization";
      const FinalSummary summary;
      const auto hash =
          canonicalHash(staged.value().descriptor, staged.value().records,
                        RunStatus::interrupted, reason, summary);
      auto database = openDatabase(entry.path());
      if (!database.ok()) {
        return database.error();
      }
      auto databaseOwner = std::move(database).value();
      auto begun = databaseOwner->execute("BEGIN IMMEDIATE");
      if (!begun.ok()) {
        return begun.error();
      }
      const auto saved = nowUtcMillis();
      Statement summaryStatement;
      auto prepared = summaryStatement.prepare(
          *databaseOwner, "INSERT INTO summary VALUES(NULL,NULL)");
      if (!prepared.ok()) {
        return prepared.error();
      }
      auto written = summaryStatement.done();
      if (!written.ok()) {
        return written.error();
      }
      Statement meta;
      prepared = meta.prepare(
          *databaseOwner, "UPDATE result_meta SET status=?1,canonical_hash=?2,"
                          "terminal_reason=?3,saved_utc_ms=?4");
      if (!prepared.ok() ||
          !meta.integer(1, static_cast<std::int64_t>(RunStatus::interrupted))
               .ok() ||
          !meta.text(2, hash).ok() || !meta.text(3, reason).ok() ||
          !meta.integer(4, saved).ok()) {
        return databaseOwner->error("Unable to bind recovered Result");
      }
      written = meta.done();
      if (!written.ok()) {
        return written.error();
      }
      auto committed = databaseOwner->execute("COMMIT");
      if (!committed.ok()) {
        return committed.error();
      }
      auto checkpointed =
          databaseOwner->execute("PRAGMA wal_checkpoint(TRUNCATE)");
      if (!checkpointed.ok()) {
        return checkpointed.error();
      }
      databaseOwner.reset();
    }

    auto validated = readResultFile(entry.path(), dataStore, true);
    if (!validated.ok()) {
      return validated.error();
    }
    const auto destination = root / "Results" / entry.path().filename();
    auto promoted = moveNoClobber(entry.path(), destination,
                                  "Unable to promote recovered Result");
    if (!promoted.ok()) {
      return promoted.error();
    }

    std::vector<std::string> segmentIds;
    for (const auto &span : validated.value().descriptor.dataSelection.spans) {
      segmentIds.push_back(span.segmentId);
    }
    std::ranges::sort(segmentIds);
    const auto uniqueEnd = std::ranges::unique(segmentIds).begin();
    segmentIds.erase(uniqueEnd, segmentIds.end());
    auto retention = data::SegmentRetentionStore::open(dataStore);
    if (!retention.ok()) {
      return retention.error();
    }
    auto acquired =
        retention.value()->acquire(validated.value().resultId, segmentIds);
    if (!acquired.ok()) {
      return acquired.error();
    }

    auto cataloged = upsertCatalog(
        root, {.resultId = validated.value().resultId,
               .canonicalResultHash = validated.value().canonicalResultHash,
               .status = validated.value().status,
               .savedUtcMillis = validated.value().savedUtcMillis,
               .available = true,
               .unavailableReason = {}});
    if (!cataloged.ok()) {
      return cataloged.error();
    }
  }
  return {};
}

} // namespace

namespace testing {

void failNext(const FailurePoint point) noexcept { failurePoint.store(point); }

void clearFailure() noexcept { failurePoint.store(FailurePoint::none); }

} // namespace testing

struct ResultWriter::Impl {
  std::filesystem::path root;
  std::filesystem::path dataStore;
  std::filesystem::path stagingPath;
  std::string id;
  RunDescriptor descriptor;
  std::vector<CanonicalRecord> records;
  std::unique_ptr<Database> database;
  bool finalized = false;
};

ResultWriter::ResultWriter(ConstructionKey, std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}

ResultWriter::~ResultWriter() = default;

const std::string &ResultWriter::resultId() const noexcept { return impl_->id; }

core::Result<void>
ResultWriter::append(const std::vector<CanonicalRecord> &records) {
  if (impl_->finalized || records.empty()) {
    return core::makeError(core::ErrorCode::invalidArgument,
                           "Result record batch is invalid");
  }
  auto expected = impl_->records.size();
  auto previousTimestamp = impl_->records.empty()
                               ? std::optional<core::Timestamp>{}
                               : impl_->records.back().timestamp;
  for (const auto &record : records) {
    if (record.sequence != expected || record.symbol.empty() ||
        (previousTimestamp.has_value() &&
         *previousTimestamp > record.timestamp)) {
      return core::makeError(core::ErrorCode::invalidArgument,
                             "Canonical Result records are out of order");
    }
    ++expected;
    previousTimestamp = record.timestamp;
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
  for (const auto &record : records) {
    Statement statement;
    auto prepared = statement.prepare(
        *impl_->database, "INSERT INTO canonical_records VALUES("
                          "?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13)");
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
        !statement.optionalInteger(12, record.positionShares).ok() ||
        !statement.text(13, record.text).ok()) {
      const auto ignored = impl_->database->execute("ROLLBACK");
      static_cast<void>(ignored);
      return impl_->database->error("Unable to bind canonical record");
    }
    auto inserted = statement.done();
    if (!inserted.ok()) {
      const auto ignored = impl_->database->execute("ROLLBACK");
      static_cast<void>(ignored);
      return inserted.error();
    }
  }
  auto committed = impl_->database->execute("COMMIT");
  if (!committed.ok()) {
    return committed.error();
  }
  impl_->records.insert(impl_->records.end(), records.begin(), records.end());
  return {};
}

core::Result<FinalizedResult>
ResultWriter::finalizeAndPromote(const RunStatus status,
                                 const FinalSummary &summary,
                                 std::string terminalReason) {
  if (impl_->finalized || status == RunStatus::running ||
      (status == RunStatus::completed &&
       (!summary.finalEquityMicrodollars.has_value() ||
        !summary.pnlMicrodollars.has_value())) ||
      (status != RunStatus::completed &&
       (summary.finalEquityMicrodollars.has_value() ||
        summary.pnlMicrodollars.has_value()))) {
    return core::makeError(core::ErrorCode::invalidArgument,
                           "Result terminal status or summary is invalid");
  }
  const auto hash = canonicalHash(impl_->descriptor, impl_->records, status,
                                  terminalReason, summary);
  if (consumeFailure(testing::FailurePoint::hashFinalization)) {
    return injectedFailure("hash finalization");
  }
  auto begun = impl_->database->execute("BEGIN IMMEDIATE");
  if (!begun.ok()) {
    return begun.error();
  }
  Statement summaryStatement;
  auto prepared = summaryStatement.prepare(*impl_->database,
                                           "INSERT INTO summary VALUES(?1,?2)");
  if (!prepared.ok() ||
      !summaryStatement.optionalInteger(1, summary.finalEquityMicrodollars)
           .ok() ||
      !summaryStatement.optionalInteger(2, summary.pnlMicrodollars).ok()) {
    return impl_->database->error("Unable to bind Result summary");
  }
  auto written = summaryStatement.done();
  if (!written.ok()) {
    return written.error();
  }
  Statement meta;
  prepared = meta.prepare(*impl_->database,
                          "UPDATE result_meta SET status=?1,canonical_hash=?2,"
                          "terminal_reason=?3,saved_utc_ms=?4");
  const auto saved = nowUtcMillis();
  if (!prepared.ok() ||
      !meta.integer(1, static_cast<std::int64_t>(status)).ok() ||
      !meta.text(2, hash).ok() || !meta.text(3, terminalReason).ok() ||
      !meta.integer(4, saved).ok()) {
    return impl_->database->error("Unable to bind Result finalization");
  }
  written = meta.done();
  if (!written.ok()) {
    return written.error();
  }
  auto committed = impl_->database->execute("COMMIT");
  if (!committed.ok()) {
    return committed.error();
  }
  auto checkpointed =
      impl_->database->execute("PRAGMA wal_checkpoint(TRUNCATE)");
  if (!checkpointed.ok()) {
    return checkpointed.error();
  }
  impl_->database.reset();
  if (consumeFailure(testing::FailurePoint::close)) {
    return injectedFailure("close");
  }

  auto reopened = readResultFile(impl_->stagingPath, impl_->dataStore, true);
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

  const auto destination = impl_->root / "Results" / (impl_->id + ".bteresult");
  if (consumeFailure(testing::FailurePoint::promotion)) {
    return injectedFailure("promotion");
  }
  std::error_code errorCode;
  std::filesystem::create_hard_link(impl_->stagingPath, destination, errorCode);
  if (errorCode) {
    return storageError("Unable to promote Result without clobbering: " +
                        errorCode.message());
  }
  std::filesystem::remove(impl_->stagingPath, errorCode);
  if (errorCode) {
    return storageError("Unable to remove promoted staging Result: " +
                        errorCode.message());
  }
  if (consumeFailure(testing::FailurePoint::catalogVisibility)) {
    return injectedFailure("catalog visibility");
  }
  auto cataloged = upsertCatalog(impl_->root, {.resultId = impl_->id,
                                               .canonicalResultHash = hash,
                                               .status = status,
                                               .savedUtcMillis = saved,
                                               .available = true,
                                               .unavailableReason = {}});
  if (!cataloged.ok()) {
    return cataloged.error();
  }
  impl_->finalized = true;
  return FinalizedResult{.resultId = impl_->id, .canonicalResultHash = hash};
}

ResultStore::ResultStore(ConstructionKey, std::filesystem::path root,
                         std::filesystem::path dataStore)
    : root_(std::move(root)), dataStore_(std::move(dataStore)) {}

core::Result<std::unique_ptr<ResultStore>>
ResultStore::open(const std::filesystem::path &root,
                  const std::filesystem::path &dataStore) {
  if (root.empty() || dataStore.empty()) {
    return core::makeError(core::ErrorCode::invalidArgument,
                           "Result and Data store paths are required");
  }
  try {
    for (const auto name : {"Staging", "Results", "Trash", "Quarantine"}) {
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
  auto recovered = recoverStaging(root, dataStore);
  if (!recovered.ok()) {
    return recovered.error();
  }
  return std::make_unique<ResultStore>(ConstructionKey{}, root, dataStore);
}

core::Result<std::unique_ptr<ResultWriter>>
ResultStore::begin(const RunDescriptor &descriptor) const {
  auto validated = validateDescriptor(descriptor);
  if (!validated.ok()) {
    return validated.error();
  }
  for (int attempt = 0; attempt < 8; ++attempt) {
    const auto id = allocateResultId();
    const auto stagingPath = root_ / "Staging" / (id + ".bteresult");
    if (std::filesystem::exists(stagingPath) ||
        std::filesystem::exists(root_ / "Results" / (id + ".bteresult")) ||
        std::filesystem::exists(root_ / "Trash" / (id + ".bteresult"))) {
      continue;
    }
    auto database = openDatabase(stagingPath);
    if (!database.ok()) {
      return database.error();
    }
    if (consumeFailure(testing::FailurePoint::schemaCreation)) {
      return injectedFailure("schema creation");
    }
    auto schema = createResultSchema(*database.value(), descriptor, id);
    if (!schema.ok()) {
      return schema.error();
    }
    auto impl = std::make_unique<ResultWriter::Impl>();
    impl->root = root_;
    impl->dataStore = dataStore_;
    impl->stagingPath = stagingPath;
    impl->id = id;
    impl->descriptor = descriptor;
    impl->database = std::move(database).value();
    return std::make_unique<ResultWriter>(ResultWriter::ConstructionKey{},
                                          std::move(impl));
  }
  return storageError("Unable to allocate a unique Result ID");
}

core::Result<std::vector<ResultSummary>> ResultStore::list() const {
  std::vector<ResultSummary> summaries;
  std::error_code errorCode;
  for (const auto &entry :
       std::filesystem::directory_iterator(root_ / "Results", errorCode)) {
    if (errorCode) {
      return storageError("Unable to inspect Result catalog: " +
                          errorCode.message());
    }
    if (!entry.is_regular_file() || entry.path().extension() != ".bteresult") {
      continue;
    }
    const auto id = entry.path().stem().string();
    auto opened = readResultFile(entry.path(), dataStore_, true);
    if (!opened.ok()) {
      summaries.push_back({.resultId = id,
                           .canonicalResultHash = {},
                           .status = RunStatus::incomplete,
                           .savedUtcMillis = 0,
                           .available = false,
                           .unavailableReason = opened.error().message});
      continue;
    }
    summaries.push_back(
        {.resultId = id,
         .canonicalResultHash = opened.value().canonicalResultHash,
         .status = opened.value().status,
         .savedUtcMillis = opened.value().savedUtcMillis,
         .available = true,
         .unavailableReason = {}});
  }
  std::ranges::sort(summaries, [](const auto &left, const auto &right) {
    if (left.savedUtcMillis != right.savedUtcMillis) {
      return left.savedUtcMillis > right.savedUtcMillis;
    }
    return left.resultId < right.resultId;
  });
  return summaries;
}

core::Result<OpenedResult>
ResultStore::openResult(const std::string &resultId) const {
  if (!validResultId(resultId)) {
    return core::makeError(core::ErrorCode::invalidArgument,
                           "Result ID is invalid");
  }
  const auto path = root_ / "Results" / (resultId + ".bteresult");
  if (!std::filesystem::exists(path)) {
    return core::makeError(core::ErrorCode::notFound, "Result is unavailable");
  }
  return readResultFile(path, dataStore_, true);
}

core::Result<void> ResultStore::moveToTrash(const std::string &resultId) const {
  if (!validResultId(resultId)) {
    return core::makeError(core::ErrorCode::invalidArgument,
                           "Result ID is invalid");
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
  const auto source = root_ / "Trash" / (resultId + ".bteresult");
  auto opened = readResultFile(source, dataStore_, true);
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
  const auto path = root_ / "Trash" / (resultId + ".bteresult");
  if (!std::filesystem::exists(path)) {
    return core::makeError(core::ErrorCode::notFound,
                           "Trashed Result is unavailable");
  }
  std::error_code errorCode;
  std::filesystem::remove(path, errorCode);
  if (errorCode) {
    return storageError("Unable to purge Result: " + errorCode.message());
  }
  auto retention = data::SegmentRetentionStore::open(dataStore_);
  if (!retention.ok()) {
    return retention.error();
  }
  auto released = retention.value()->release(resultId);
  if (!released.ok()) {
    return released.error();
  }
  return deleteCatalog(root_, resultId);
}

core::Result<FinalizedResult>
ResultStore::importResult(const std::filesystem::path &source) const {
  if (source.empty() || !std::filesystem::is_regular_file(source)) {
    return core::makeError(core::ErrorCode::notFound,
                           "Imported Result source is unavailable");
  }
  auto sourceResult = readResultFile(source, dataStore_, true);
  if (!sourceResult.ok()) {
    return sourceResult.error();
  }

  std::string importedId;
  std::filesystem::path stagingPath;
  for (int attempt = 0; attempt < 8; ++attempt) {
    const auto candidate = allocateResultId();
    const auto candidateStaging =
        root_ / "Staging" / (candidate + ".bteresult");
    const auto candidateResult = root_ / "Results" / (candidate + ".bteresult");
    const auto candidateTrash = root_ / "Trash" / (candidate + ".bteresult");
    std::error_code copyError;
    if (std::filesystem::exists(candidateResult) ||
        std::filesystem::exists(candidateTrash) ||
        !std::filesystem::copy_file(source, candidateStaging,
                                    std::filesystem::copy_options::none,
                                    copyError)) {
      if (copyError == std::errc::file_exists) {
        continue;
      }
      if (copyError) {
        return storageError("Unable to stage imported Result: " +
                            copyError.message());
      }
      continue;
    }
    importedId = candidate;
    stagingPath = candidateStaging;
    break;
  }
  if (importedId.empty()) {
    return storageError("Unable to allocate a unique imported Result ID");
  }

  auto database = openDatabase(stagingPath);
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

  auto validated = readResultFile(stagingPath, dataStore_, true);
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

  std::vector<std::string> segmentIds;
  for (const auto &span : validated.value().descriptor.dataSelection.spans) {
    segmentIds.push_back(span.segmentId);
  }
  std::ranges::sort(segmentIds);
  const auto uniqueEnd = std::ranges::unique(segmentIds).begin();
  segmentIds.erase(uniqueEnd, segmentIds.end());
  auto retention = data::SegmentRetentionStore::open(dataStore_);
  if (!retention.ok()) {
    return retention.error();
  }
  auto acquired = retention.value()->acquire(importedId, segmentIds);
  if (!acquired.ok()) {
    return acquired.error();
  }
  auto cataloged = upsertCatalog(
      root_, {.resultId = importedId,
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
