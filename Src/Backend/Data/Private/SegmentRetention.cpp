#include "Bte/Data/SegmentRetention.h"

#include "Bte/Core/Digest.h"
#include "Bte/Core/Result.h"

#include "SegmentRetentionTestHooks.h"

#include <sqlite3.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace bte::data {
namespace {

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
    if (sqlite3_libversion_number() != 3'053'004) {
      return core::makeError(core::ErrorCode::schemaMismatch,
                             "SQLite 3.53.4 is required");
    }
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
    char *message = nullptr;
    const auto status =
        sqlite3_exec(handle_, sql.c_str(), nullptr, nullptr, &message);
    const auto detail =
        message == nullptr ? std::string{} : std::string{message};
    sqlite3_free(message);
    if (consumeFailure(testing::RetentionFailurePoint::sqlExecution) ||
        status != SQLITE_OK) {
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

} // namespace

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
  } catch (const std::filesystem::filesystem_error &error) {
    return core::makeError(core::ErrorCode::permissionDenied,
                           "Unable to create retention store: " +
                               std::string{error.what()});
  }
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
  for (const auto &segmentId : uniqueIds) {
    if (!validSegmentId(segmentId)) {
      return core::makeError(core::ErrorCode::invalidArgument,
                             "Data Segment identity is invalid");
    }
    const auto path = storeDirectory_ / "Segments" / (segmentId + ".btedata");
    std::ifstream input{path, std::ios::binary};
    if (!input) {
      return core::makeError(core::ErrorCode::dataSnapshotUnavailable,
                             "Referenced Data Segment is unavailable");
    }
    const std::string bytes{std::istreambuf_iterator<char>{input},
                            std::istreambuf_iterator<char>{}};
    if (core::sha256(bytes) != segmentId) {
      return core::makeError(core::ErrorCode::dataSnapshotUnavailable,
                             "Referenced Data Segment hash is invalid");
    }
  }

  auto database = openDatabase(storeDirectory_);
  if (!database.ok()) {
    return database.error();
  }
  auto begun = database.value()->execute("BEGIN IMMEDIATE");
  if (!begun.ok()) {
    return begun.error();
  }
  std::size_t acquired = 0;
  for (const auto &segmentId : uniqueIds) {
    auto inserted = database.value()->executeBound(
        "INSERT OR IGNORE INTO segment_references(result_id, segment_id) "
        "VALUES(?1, ?2)",
        resultId, segmentId);
    if (!inserted.ok()) {
      (void)database.value()->execute("ROLLBACK");
      return inserted.error();
    }
    acquired += inserted.value();
  }
  auto committed = database.value()->execute("COMMIT");
  if (!committed.ok()) {
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
  for (const auto &segmentId : segmentIds.value()) {
    auto count = database.value()->countReferences(segmentId);
    if (!count.ok()) {
      (void)database.value()->execute("ROLLBACK");
      return count.error();
    }
    if (count.value() == 0) {
      try {
        if (std::filesystem::remove(storeDirectory_ / "Segments" /
                                    (segmentId + ".btedata"))) {
          purged.push_back(segmentId);
        }
      } catch (const std::filesystem::filesystem_error &error) {
        (void)database.value()->execute("ROLLBACK");
        return core::makeError(core::ErrorCode::permissionDenied,
                               "Unable to purge Data Segment: " +
                                   std::string{error.what()});
      }
    }
  }
  auto committed = database.value()->execute("COMMIT");
  if (!committed.ok()) {
    return committed.error();
  }
  return purged;
}

} // namespace bte::data
