#include "Bte/Data/SegmentRetention.h"

#include "Bte/Core/Digest.h"
#include "Bte/Core/Result.h"

#include "PublishedSnapshotSegments.h"
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
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges> // IWYU pragma: keep
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_set>
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

std::shared_ptr<std::mutex>
storeMutexFor(const std::filesystem::path &canonicalDirectory) {
  static std::mutex registryMutex;
  static std::map<std::filesystem::path, std::shared_ptr<std::mutex>> registry;
  const std::scoped_lock lock{registryMutex};
  // Reclaim paths held only by the registry; active stores keep a second owner.
  std::erase_if(registry, [](const auto &entry) {
    return entry.second.use_count() == 1;
  });
  if (const auto position = registry.find(canonicalDirectory);
      position != registry.end()) {
    return position->second;
  }
  auto created = std::make_shared<std::mutex>();
  registry.insert_or_assign(canonicalDirectory, created);
  return created;
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

struct SegmentMove final {
  std::filesystem::path source;
  std::filesystem::path destination;
};

struct SegmentRecoveryAction final {
  std::filesystem::path activePath;
  std::filesystem::path trashedPath;
  bool alreadyTrashed = false;
};

struct SegmentRecoveryPlan final {
  std::vector<std::filesystem::path> duplicateActiveFiles;
  std::vector<SegmentMove> activeFilesToTrash;
};

struct SegmentRecoveryDirectories final {
  std::filesystem::path active;
  std::filesystem::path store;
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

core::Result<std::optional<SegmentRecoveryAction>>
recoveryActionForSegment(const std::filesystem::directory_entry &entry,
                         const std::filesystem::path &storeDirectory,
                         const std::unordered_set<std::string> &referenced,
                         const std::unordered_set<std::string> &published) {
  constexpr std::string_view segmentSuffix{".btedata"};
  const auto fileName = entry.path().filename().string();
  if (!fileName.ends_with(segmentSuffix)) {
    return std::optional<SegmentRecoveryAction>{};
  }
  const auto segmentId =
      fileName.substr(0, fileName.size() - segmentSuffix.size());
  if (!validSegmentId(segmentId) || referenced.contains(segmentId) ||
      published.contains(segmentId)) {
    return std::optional<SegmentRecoveryAction>{};
  }

  std::error_code error;
  const auto status = entry.symlink_status(error);
  if (error) {
    return core::makeError(core::ErrorCode::permissionDenied,
                           "Unable to inspect snapshot segments: " +
                               error.message());
  }
  if (!std::filesystem::is_regular_file(status)) {
    return std::optional<SegmentRecoveryAction>{};
  }
  const auto trashedPath = storeDirectory / "Trash" / "Segments" / fileName;
  const auto alreadyTrashed = std::filesystem::exists(trashedPath, error);
  if (error) {
    return core::makeError(core::ErrorCode::permissionDenied,
                           "Unable to inspect trashed snapshot segments: " +
                               error.message());
  }
  if (alreadyTrashed) {
    const auto trashedStatus =
        std::filesystem::symlink_status(trashedPath, error);
    if (error) {
      return core::makeError(core::ErrorCode::permissionDenied,
                             "Unable to inspect trashed snapshot segment: " +
                                 error.message());
    }
    if (!std::filesystem::is_regular_file(trashedStatus)) {
      return core::makeError(core::ErrorCode::dataSnapshotUnavailable,
                             "Trashed Data Segment is unavailable");
    }
    std::ifstream input{trashedPath, std::ios::binary};
    if (!input) {
      return core::makeError(core::ErrorCode::dataSnapshotUnavailable,
                             "Trashed Data Segment is unavailable");
    }
    const std::string bytes{std::istreambuf_iterator<char>{input},
                            std::istreambuf_iterator<char>{}};
    if (input.bad() || core::sha256(bytes) != segmentId) {
      return core::makeError(core::ErrorCode::dataSnapshotUnavailable,
                             "Trashed Data Segment hash is invalid");
    }
  }
  return std::optional<SegmentRecoveryAction>{SegmentRecoveryAction{
      .activePath = entry.path(),
      .trashedPath = trashedPath,
      .alreadyTrashed = alreadyTrashed,
  }};
}

core::Result<SegmentRecoveryPlan> collectUnpublishedSegmentRecoveryPlan(
    const SegmentRecoveryDirectories &directories,
    const std::unordered_set<std::string> &referenced,
    const std::unordered_set<std::string> &published) {
  std::error_code error;
  auto entries = std::filesystem::directory_iterator{directories.active, error};
  if (error) {
    return core::makeError(core::ErrorCode::permissionDenied,
                           "Unable to inspect snapshot segments: " +
                               error.message());
  }
  SegmentRecoveryPlan plan;
  for (; entries != std::filesystem::directory_iterator{};
       entries.increment(error)) {
    if (error) {
      return core::makeError(core::ErrorCode::permissionDenied,
                             "Unable to inspect snapshot segments: " +
                                 error.message());
    }
    auto action = recoveryActionForSegment(*entries, directories.store,
                                           referenced, published);
    if (!action.ok()) {
      return action.error();
    }
    auto recoveredAction = std::move(action).value();
    if (!recoveredAction.has_value()) {
      continue;
    }
    const auto &actionValue = recoveredAction.value();
    if (actionValue.alreadyTrashed) {
      plan.duplicateActiveFiles.push_back(actionValue.activePath);
      continue;
    }
    plan.activeFilesToTrash.push_back({.source = actionValue.activePath,
                                       .destination = actionValue.trashedPath});
  }
  if (error) {
    return core::makeError(core::ErrorCode::permissionDenied,
                           "Unable to inspect snapshot segments: " +
                               error.message());
  }
  return plan;
}

core::Result<std::vector<SegmentMove>>
applyUnpublishedSegmentRecoveryPlan(const SegmentRecoveryPlan &plan) {
  std::vector<SegmentMove> movedToTrash;
  for (const auto &duplicate : plan.duplicateActiveFiles) {
    std::error_code error;
    std::filesystem::remove(duplicate, error);
    if (error) {
      restoreMoves(movedToTrash);
      return core::makeError(
          core::ErrorCode::permissionDenied,
          "Unable to clear duplicate abandoned Data Segment: " +
              error.message());
    }
  }
  for (const auto &move : plan.activeFilesToTrash) {
    auto moved = moveSegment(move.source, move.destination,
                             "Unable to trash abandoned Data Segment");
    if (!moved.ok()) {
      restoreMoves(movedToTrash);
      return moved.error();
    }
    movedToTrash.push_back(move);
  }
  return movedToTrash;
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
    ConstructionKey, std::filesystem::path storeDirectory,
    std::shared_ptr<std::mutex> storeMutex)
    : storeDirectory_(std::move(storeDirectory)),
      storeMutex_(std::move(storeMutex)) {}

core::Result<std::unique_ptr<SegmentRetentionStore>>
SegmentRetentionStore::open(const std::filesystem::path &storeDirectory) {
  if (storeDirectory.empty()) {
    return core::makeError(core::ErrorCode::invalidArgument,
                           "Segment retention store directory is required");
  }
  std::filesystem::path canonicalDirectory;
  try {
    std::filesystem::create_directories(storeDirectory);
    std::filesystem::create_directories(storeDirectory / "Trash" / "Segments");
    canonicalDirectory = std::filesystem::canonical(storeDirectory);
  } catch (const std::filesystem::filesystem_error &error) {
    return core::makeError(core::ErrorCode::permissionDenied,
                           "Unable to prepare retention store: " +
                               std::string{error.what()});
  }
  auto storeMutex = storeMutexFor(canonicalDirectory);
  {
    const std::scoped_lock lock{*storeMutex};
    auto database = openDatabase(canonicalDirectory);
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
  }
  return std::make_unique<SegmentRetentionStore>(
      ConstructionKey{}, std::move(canonicalDirectory), std::move(storeMutex));
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
  const std::scoped_lock lock{*storeMutex_};
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
    restoreMoves(restored);
    (void)database.value()->execute("ROLLBACK");
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
SegmentRetentionStore::references(const std::string &resultId) const {
  if (!validIdentity(resultId)) {
    return core::makeError(core::ErrorCode::invalidArgument,
                           "Result ID is invalid");
  }
  const std::scoped_lock lock{*storeMutex_};
  auto database = openDatabase(storeDirectory_);
  if (!database.ok()) {
    return database.error();
  }
  return database.value()->strings("SELECT segment_id FROM segment_references "
                                   "WHERE result_id = ?1 ORDER BY segment_id",
                                   resultId);
}

core::Result<void> SegmentRetentionStore::abandonUnpublishedSnapshot(
    const std::string &snapshotId) const {
  if (!validSegmentId(snapshotId)) {
    return core::makeError(core::ErrorCode::invalidArgument,
                           "Snapshot identity is invalid");
  }
  std::error_code error;
  const auto exists = std::filesystem::exists(
      storeDirectory_ / "Snapshots" / snapshotId, error);
  if (error || exists) {
    return core::makeError(core::ErrorCode::invalidArgument,
                           "Snapshot is published or cannot be inspected");
  }
  auto released = release("snapshot-" + snapshotId);
  if (!released.ok()) {
    return released.error();
  }
  return {};
}

core::Result<void>
SegmentRetentionStore::recoverUnpublishedSnapshotPins() const {
  auto abandonedPinsRecovered = recoverAbandonedSnapshotPins();
  if (!abandonedPinsRecovered.ok()) {
    return abandonedPinsRecovered.error();
  }
  auto stagingCleared = clearStagedSnapshotArtifacts();
  if (!stagingCleared.ok()) {
    return stagingCleared.error();
  }
  return trashUnpublishedSnapshotSegments();
}

core::Result<void> SegmentRetentionStore::recoverAbandonedSnapshotPins() const {
  std::vector<std::string> resultIds;
  {
    const std::scoped_lock lock{*storeMutex_};
    auto database = openDatabase(storeDirectory_);
    if (!database.ok()) {
      return database.error();
    }
    auto queried = database.value()->strings(
        "SELECT DISTINCT result_id FROM segment_references "
        "WHERE result_id LIKE ?1 ORDER BY result_id",
        "snapshot-%");
    if (!queried.ok()) {
      return queried.error();
    }
    resultIds = std::move(queried).value();
  } // Release the lock before filesystem-based snapshot recovery.
  constexpr std::string_view pinPrefix{"snapshot-"};
  for (const auto &resultId : resultIds) {
    if (!resultId.starts_with(pinPrefix)) {
      continue;
    }
    const auto snapshotId = resultId.substr(pinPrefix.size());
    if (!validSegmentId(snapshotId)) {
      continue;
    }
    std::error_code error;
    const auto published = std::filesystem::exists(
        storeDirectory_ / "Snapshots" / snapshotId, error);
    if (error) {
      return core::makeError(core::ErrorCode::permissionDenied,
                             "Unable to inspect snapshot recovery state: " +
                                 error.message());
    }
    if (published) {
      continue;
    }
    auto abandoned = abandonUnpublishedSnapshot(snapshotId);
    if (!abandoned.ok()) {
      return abandoned.error();
    }
  }
  return {};
}

core::Result<void> SegmentRetentionStore::clearStagedSnapshotArtifacts() const {
  const auto segmentDirectory = storeDirectory_ / "Segments";
  std::error_code error;
  const auto segmentDirectoryExists =
      std::filesystem::exists(segmentDirectory, error);
  if (error) {
    return core::makeError(core::ErrorCode::permissionDenied,
                           "Unable to inspect snapshot segments: " +
                               error.message());
  }
  if (!segmentDirectoryExists) {
    return {};
  }

  auto stagedEntries =
      std::filesystem::directory_iterator{segmentDirectory, error};
  if (error) {
    return core::makeError(core::ErrorCode::permissionDenied,
                           "Unable to inspect snapshot artifact staging: " +
                               error.message());
  }
  for (; stagedEntries != std::filesystem::directory_iterator{};
       stagedEntries.increment(error)) {
    if (error) {
      return core::makeError(core::ErrorCode::permissionDenied,
                             "Unable to inspect snapshot artifact staging: " +
                                 error.message());
    }
    if (!stagedEntries->path().filename().string().ends_with(
            ".btedata.staging")) {
      continue;
    }
    const auto status = stagedEntries->symlink_status(error);
    if (error) {
      return core::makeError(core::ErrorCode::permissionDenied,
                             "Unable to inspect snapshot artifact staging: " +
                                 error.message());
    } // GCOVR_EXCL_LINE: the compiler emits no executable instruction here.
    if (!std::filesystem::is_regular_file(status) &&
        !std::filesystem::is_symlink(status)) {
      continue;
    } // GCOVR_EXCL_LINE: the compiler emits no executable instruction here.
    std::filesystem::remove(stagedEntries->path(), error);
    if (error) {
      return core::makeError(core::ErrorCode::permissionDenied,
                             "Unable to clear snapshot artifact staging: " +
                                 error.message());
    } // GCOVR_EXCL_LINE: the compiler emits no executable instruction here.
  }
  if (error) {
    return core::makeError(core::ErrorCode::permissionDenied,
                           "Unable to inspect snapshot artifact staging: " +
                               error.message());
  }
  return {};
}

core::Result<void>
SegmentRetentionStore::trashUnpublishedSnapshotSegments() const {
  const auto segmentDirectory = storeDirectory_ / "Segments";
  std::error_code filesystemError;
  const auto segmentDirectoryExists =
      std::filesystem::exists(segmentDirectory, filesystemError);
  if (filesystemError) {
    return core::makeError(core::ErrorCode::permissionDenied,
                           "Unable to inspect snapshot segments: " +
                               filesystemError.message());
  }
  if (!segmentDirectoryExists) {
    return {};
  }
  const std::scoped_lock lock{*storeMutex_};
  auto database = openDatabase(storeDirectory_);
  if (!database.ok()) {
    return database.error();
  }
  auto begun = database.value()->execute("BEGIN IMMEDIATE");
  if (!begun.ok()) {
    return begun.error();
  }
  const auto rollback = [&] { (void)database.value()->execute("ROLLBACK"); };
  auto referencedResult = database.value()->strings(
      "SELECT DISTINCT segment_id FROM segment_references "
      "WHERE segment_id LIKE ?1 ORDER BY segment_id",
      "%");
  if (!referencedResult.ok()) {
    rollback();
    return referencedResult.error();
  }
  const std::unordered_set<std::string> referenced{
      referencedResult.value().begin(), referencedResult.value().end()};
  auto published = publishedSnapshotSegments(storeDirectory_);
  if (!published.ok()) {
    rollback();
    return published.error();
  }
  auto plan = collectUnpublishedSegmentRecoveryPlan(
      {.active = segmentDirectory, .store = storeDirectory_}, referenced,
      published.value());
  if (!plan.ok()) {
    rollback();
    return plan.error();
  }
  auto movedToTrash = applyUnpublishedSegmentRecoveryPlan(plan.value());
  if (!movedToTrash.ok()) {
    rollback();
    return movedToTrash.error();
  }
  auto movesToRestore = std::move(movedToTrash).value();
  auto committed = database.value()->execute("COMMIT");
  if (!committed.ok()) {
    rollback();
    restoreMoves(movesToRestore);
    return committed.error();
  }
  return {};
}

core::Result<std::vector<std::string>>
SegmentRetentionStore::release(const std::string &resultId) const {
  if (!validIdentity(resultId)) {
    return core::makeError(core::ErrorCode::invalidArgument,
                           "Result ID is invalid");
  }
  const std::scoped_lock lock{*storeMutex_};
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
  auto published = publishedSnapshotSegments(storeDirectory_);
  if (!published.ok()) {
    (void)database.value()->execute("ROLLBACK");
    return published.error();
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
    restoreMoves(movedToTrash);
    (void)database.value()->execute("ROLLBACK");
  };
  for (const auto &segmentId : segmentIds.value()) {
    auto count = database.value()->countReferences(segmentId);
    if (!count.ok()) {
      rollback();
      return count.error();
    }
    if (count.value() == 0 && !published.value().contains(segmentId)) {
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
