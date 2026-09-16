#include "Bte/Data/SegmentRetention.h"

#include "Bte/Core/Digest.h"
#include "Bte/Core/Result.h"

#include "SegmentRetentionTestHooks.h"

#include <sqlite3.h>

#include <algorithm>
#if defined(BTE_ENABLE_TEST_HOOKS)
#include <atomic>
#endif
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <functional> // IWYU pragma: keep
#include <initializer_list>
#include <iterator>
#include <memory>
#include <mutex>
#include <ranges> // IWYU pragma: keep
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace bte::data {
namespace {

#if defined(BTE_ENABLE_TEST_HOOKS)
std::atomic<testing::RetentionFailurePoint> &configuredFailurePoint() {
  static std::atomic<testing::RetentionFailurePoint> value{
      testing::RetentionFailurePoint::none};
  return value;
}

std::atomic<std::uint32_t> &configuredFailureSkips() {
  static std::atomic<std::uint32_t> value{0};
  return value;
}

bool consumeFailure(const testing::RetentionFailurePoint point) noexcept {
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
      expected, testing::RetentionFailurePoint::none);
}
#else
constexpr bool consumeFailure(testing::RetentionFailurePoint) noexcept {
  return false;
}
#endif

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

  [[nodiscard]] core::Result<void> open(const std::filesystem::path &path) {
    // The manifest pins this exact SQLite ABI; exercising a mismatch would
    // require replacing the process dependency. GCOVR_EXCL_START
    if (sqlite3_libversion_number() != 3'053'004) {
      return core::makeError(core::ErrorCode::schemaMismatch,
                             "SQLite 3.53.4 is required");
    }
    // GCOVR_EXCL_STOP
    if (consumeFailure(testing::RetentionFailurePoint::databaseOpen) ||
        sqlite3_open_v2(path.string().c_str(), &handle_,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE |
                            SQLITE_OPEN_FULLMUTEX,
                        nullptr) != SQLITE_OK) {
      return error("Unable to open segment retention database");
    }
    sqlite3_busy_timeout(handle_, 5'000);
    return {};
  }

  [[nodiscard]] core::Result<std::size_t> execute(const std::string &sql) {
    if (consumeFailure(testing::RetentionFailurePoint::sqlExecution)) {
      return core::makeError(core::ErrorCode::internal,
                             "SQLite operation failed: injected failure");
    }
    char *message = nullptr;
    const auto status =
        sqlite3_exec(handle_, sql.c_str(), nullptr, nullptr, &message);
    const auto detail =
        message == nullptr ? std::string{} : std::string{message};
    sqlite3_free(message);
    if (status != SQLITE_OK) {
      return core::makeError(core::ErrorCode::internal,
                             "SQLite operation failed: " + detail);
    }
    return static_cast<std::size_t>(sqlite3_changes(handle_));
  }

  [[nodiscard]] core::Result<std::size_t>
  executeBound(const std::string &sql, const std::string &first,
               const std::string &second = {}) {
    sqlite3_stmt *statement = nullptr;
    if (consumeFailure(testing::RetentionFailurePoint::boundPreparation) ||
        sqlite3_prepare_v2(handle_, sql.c_str(), -1, &statement, nullptr) !=
            SQLITE_OK) {
      return error("Unable to prepare retention statement");
    }
    const auto finalize =
        std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>{
            statement, &sqlite3_finalize};
    if (consumeFailure(testing::RetentionFailurePoint::boundBinding) ||
        sqlite3_bind_text(statement, 1, first.c_str(), -1, SQLITE_TRANSIENT) !=
            SQLITE_OK ||
        (!second.empty() && sqlite3_bind_text(statement, 2, second.c_str(), -1,
                                              SQLITE_TRANSIENT) != SQLITE_OK)) {
      return error("Unable to bind retention statement");
    }
    if (consumeFailure(testing::RetentionFailurePoint::boundExecution) ||
        sqlite3_step(statement) != SQLITE_DONE) {
      return error("Unable to execute retention statement");
    }
    return static_cast<std::size_t>(sqlite3_changes(handle_));
  }

  [[nodiscard]] core::Result<std::vector<std::string>>
  strings(const std::string &sql, const std::string &value) {
    sqlite3_stmt *statement = nullptr;
    if (consumeFailure(testing::RetentionFailurePoint::queryPreparation) ||
        sqlite3_prepare_v2(handle_, sql.c_str(), -1, &statement, nullptr) !=
            SQLITE_OK) {
      return error("Unable to prepare retention query");
    }
    const auto finalize =
        std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>{
            statement, &sqlite3_finalize};
    if (consumeFailure(testing::RetentionFailurePoint::queryBinding) ||
        sqlite3_bind_text(statement, 1, value.c_str(), -1, SQLITE_TRANSIENT) !=
            SQLITE_OK) {
      return error("Unable to bind retention query");
    }
    std::vector<std::string> values;
    while (true) {
      const auto status = sqlite3_step(statement);
      if (status == SQLITE_DONE) {
        return values;
      }
      if (consumeFailure(testing::RetentionFailurePoint::queryExecution) ||
          status != SQLITE_ROW) {
        return error("Unable to execute retention query");
      }
      const auto *text = sqlite3_column_text(statement, 0);
      if (consumeFailure(testing::RetentionFailurePoint::queryNullIdentity) ||
          text == nullptr) {
        return core::makeError(core::ErrorCode::internal,
                               "Retention query returned null identity");
      }
      // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): SQLite
      values.emplace_back(reinterpret_cast<const char *>(text));
    }
  }

  [[nodiscard]] core::Result<std::size_t>
  countReferences(const std::string &segmentId) {
    sqlite3_stmt *statement = nullptr;
    constexpr auto sql =
        "SELECT COUNT(*) FROM segment_references WHERE segment_id = ?1";
    if (consumeFailure(testing::RetentionFailurePoint::countPreparation) ||
        sqlite3_prepare_v2(handle_, sql, -1, &statement, nullptr) !=
            SQLITE_OK) {
      return error("Unable to prepare reference-count query");
    }
    const auto finalize =
        std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>{
            statement, &sqlite3_finalize};
    if (consumeFailure(testing::RetentionFailurePoint::countExecution) ||
        sqlite3_bind_text(statement, 1, segmentId.c_str(), -1,
                          SQLITE_TRANSIENT) != SQLITE_OK ||
        sqlite3_step(statement) != SQLITE_ROW) {
      return error("Unable to execute reference-count query");
    }
    return static_cast<std::size_t>(sqlite3_column_int64(statement, 0));
  }

private:
  [[nodiscard]] core::Error error(const std::string &message) const {
    const auto detail =
        handle_ == nullptr ? std::string{} : sqlite3_errmsg(handle_);
    return core::makeError(core::ErrorCode::internal,
                           detail.empty() ? message : message + ": " + detail);
  }

  sqlite3 *handle_ = nullptr;
};

bool validIdentity(const std::string &identity) {
  return !identity.empty() && identity.size() <= 128 &&
         std::ranges::all_of(identity, [](const char character) {
           return (character >= 'a' && character <= 'z') ||
                  (character >= '0' && character <= '9') || character == '-';
         });
}

bool validSegmentId(const std::string &identity) {
  return identity.size() == 64 &&
         std::ranges::all_of(identity, [](const char character) {
           return (character >= 'a' && character <= 'f') ||
                  (character >= '0' && character <= '9');
         });
}

core::Result<std::unique_ptr<Database>>
openDatabase(const std::filesystem::path &storeDirectory) {
  auto database = std::make_unique<Database>();
  auto opened = database->open(storeDirectory / "References.sqlite3");
  if (!opened.ok()) {
    return opened.error();
  }
  return database;
}

std::mutex &retentionDatabaseMutex() {
  // SQLite's WAL shared-memory locking is safe across processes but invisible
  // to ThreadSanitizer. Serialize this process's short retention transactions;
  // SQLite remains the authority for coordination with another process.
  static std::mutex mutex;
  return mutex;
}

struct SegmentMove final {
  std::filesystem::path source;
  std::filesystem::path destination;
};

void restoreMoves(std::vector<SegmentMove> &moves) noexcept {
  for (const auto &move : moves | std::views::reverse) {
    std::error_code ignored;
    std::filesystem::rename(move.destination, move.source, ignored);
  }
}

core::Result<void> moveSegment(const std::filesystem::path &source,
                               const std::filesystem::path &destination,
                               const std::string &message) {
  std::error_code errorCode;
  if (!std::filesystem::is_regular_file(source, errorCode) || errorCode ||
      std::filesystem::exists(destination, errorCode) || errorCode) {
    return core::makeError(core::ErrorCode::permissionDenied, message);
  }
  std::filesystem::rename(source, destination, errorCode);
  // The preflight checks above make this reachable only through a filesystem
  // permission/state race; injected storage failures cover the public error
  // contract without racing the host filesystem. GCOVR_EXCL_START
  if (errorCode) {
    return core::makeError(core::ErrorCode::permissionDenied,
                           message + ": " + errorCode.message());
  }
  // GCOVR_EXCL_STOP
  return {};
}

} // namespace

#if defined(BTE_ENABLE_TEST_HOOKS)
namespace testing {

void failRetentionAfter(const RetentionFailurePoint point,
                        const std::uint32_t occurrencesToSkip) noexcept {
  configuredFailureSkips().store(occurrencesToSkip);
  configuredFailurePoint().store(point);
}

void clearRetentionFailure() noexcept {
  configuredFailurePoint().store(RetentionFailurePoint::none);
  configuredFailureSkips().store(0);
}

} // namespace testing
#endif

SegmentRetentionStore::SegmentRetentionStore(
    ConstructionKey, std::filesystem::path storeDirectory)
    : storeDirectory_(std::move(storeDirectory)) {}

core::Result<std::unique_ptr<SegmentRetentionStore>>
SegmentRetentionStore::open(const std::filesystem::path &storeDirectory) {
  if (storeDirectory.empty()) {
    return core::makeError(core::ErrorCode::invalidArgument,
                           "Segment retention store directory is required");
  }
  try {
    std::filesystem::create_directories(storeDirectory);
    std::filesystem::create_directories(storeDirectory / "Trash" / "Segments");
  } catch (const std::filesystem::filesystem_error &error) {
    return core::makeError(core::ErrorCode::permissionDenied,
                           "Unable to create retention store: " +
                               std::string{error.what()});
  }
  const std::scoped_lock lock{retentionDatabaseMutex()};
  auto database = openDatabase(storeDirectory);
  if (!database.ok()) {
    return database.error();
  }
  for (const auto &sql : {
           "PRAGMA journal_mode=WAL",
           "PRAGMA synchronous=FULL",
           "CREATE TABLE IF NOT EXISTS segment_references("
           "result_id TEXT NOT NULL, segment_id TEXT NOT NULL, "
           "PRIMARY KEY(result_id, segment_id)) WITHOUT ROWID",
           "CREATE INDEX IF NOT EXISTS segment_reference_counts "
           "ON segment_references(segment_id)",
       }) {
    auto executed = database.value()->execute(sql);
    if (!executed.ok()) {
      return executed.error();
    }
  }
  return std::make_unique<SegmentRetentionStore>(ConstructionKey{},
                                                 storeDirectory);
}

core::Result<std::size_t> SegmentRetentionStore::acquire(
    const std::string &resultId,
    const std::vector<std::string> &segmentIds) const {
  if (!validIdentity(resultId) || segmentIds.empty()) {
    return core::makeError(core::ErrorCode::invalidArgument,
                           "Result ID and Data Segments are required");
  }
  auto uniqueIds = segmentIds;
  std::ranges::sort(uniqueIds);
  if (std::ranges::adjacent_find(uniqueIds) != uniqueIds.end()) {
    return core::makeError(core::ErrorCode::invalidArgument,
                           "Data Segment identities must be unique");
  }
  const std::scoped_lock lock{retentionDatabaseMutex()};
  auto database = openDatabase(storeDirectory_);
  if (!database.ok()) {
    return database.error();
  }
  auto begun = database.value()->execute("BEGIN IMMEDIATE");
  if (!begun.ok()) {
    return begun.error();
  }
  std::vector<SegmentMove> restored;
  const auto rollback = [&] {
    (void)database.value()->execute("ROLLBACK");
    restoreMoves(restored);
  };
  for (const auto &segmentId : uniqueIds) {
    if (!validSegmentId(segmentId)) {
      rollback();
      return core::makeError(core::ErrorCode::invalidArgument,
                             "Data Segment identity is invalid");
    }
    const auto active = storeDirectory_ / "Segments" / (segmentId + ".btedata");
    const auto trashed =
        storeDirectory_ / "Trash" / "Segments" / (segmentId + ".btedata");
    if (!std::filesystem::exists(active) && std::filesystem::exists(trashed)) {
      // moveSegment's failure here requires a filesystem race after both
      // existence checks; its public failure contract is covered at the
      // storage seam. GCOVR_EXCL_START
      auto moved =
          moveSegment(trashed, active, "Unable to restore Data Segment");
      if (!moved.ok()) {
        rollback();
        return moved.error();
      }
      // GCOVR_EXCL_STOP
      restored.push_back({.source = trashed, .destination = active});
    }
    std::ifstream input{active, std::ios::binary};
    if (!input) {
      rollback();
      return core::makeError(core::ErrorCode::dataSnapshotUnavailable,
                             "Referenced Data Segment is unavailable");
    }
    const std::string bytes{std::istreambuf_iterator<char>{input},
                            std::istreambuf_iterator<char>{}};
    if (core::sha256(bytes) != segmentId) {
      rollback();
      return core::makeError(core::ErrorCode::dataSnapshotUnavailable,
                             "Referenced Data Segment hash is invalid");
    }
  }
  std::size_t acquired = 0;
  for (const auto &segmentId : uniqueIds) {
    auto inserted = database.value()->executeBound(
        "INSERT OR IGNORE INTO segment_references(result_id, segment_id) "
        "VALUES(?1, ?2)",
        resultId, segmentId);
    if (!inserted.ok()) {
      rollback();
      return inserted.error();
    }
    acquired += inserted.value();
  }
  auto committed = database.value()->execute("COMMIT");
  if (!committed.ok()) {
    rollback();
    return committed.error();
  }
  return acquired;
}

core::Result<std::vector<std::string>>
SegmentRetentionStore::release(const std::string &resultId) const {
  if (!validIdentity(resultId)) {
    return core::makeError(core::ErrorCode::invalidArgument,
                           "Result ID is invalid");
  }
  const std::scoped_lock lock{retentionDatabaseMutex()};
  auto database = openDatabase(storeDirectory_);
  if (!database.ok()) {
    return database.error();
  }
  auto begun = database.value()->execute("BEGIN IMMEDIATE");
  if (!begun.ok()) {
    return begun.error();
  }
  auto segmentIds = database.value()->strings(
      "SELECT segment_id FROM segment_references WHERE result_id = ?1 "
      "ORDER BY segment_id",
      resultId);
  if (!segmentIds.ok()) {
    (void)database.value()->execute("ROLLBACK");
    return segmentIds.error();
  }
  auto removed = database.value()->executeBound(
      "DELETE FROM segment_references WHERE result_id = ?1", resultId);
  if (!removed.ok()) {
    (void)database.value()->execute("ROLLBACK");
    return removed.error();
  }

  std::vector<std::string> purged;
  std::vector<SegmentMove> movedToTrash;
  const auto rollback = [&] {
    (void)database.value()->execute("ROLLBACK");
    restoreMoves(movedToTrash);
  };
  for (const auto &segmentId : segmentIds.value()) {
    auto count = database.value()->countReferences(segmentId);
    if (!count.ok()) {
      rollback();
      return count.error();
    }
    if (count.value() == 0) {
      const auto active =
          storeDirectory_ / "Segments" / (segmentId + ".btedata");
      const auto trashed =
          storeDirectory_ / "Trash" / "Segments" / (segmentId + ".btedata");
      auto moved = moveSegment(active, trashed, "Unable to trash Data Segment");
      if (!moved.ok()) {
        rollback();
        return moved.error();
      }
      movedToTrash.push_back({.source = active, .destination = trashed});
      purged.push_back(segmentId);
    }
  }
  auto committed = database.value()->execute("COMMIT");
  if (!committed.ok()) {
    rollback();
    return committed.error();
  }
  return purged;
}

} // namespace bte::data
