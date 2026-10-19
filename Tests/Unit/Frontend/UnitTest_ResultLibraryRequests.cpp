#include "Bte/Bindings/ResultLibraryRequests.h"

#include "ResultLibraryRequestsTestHooks.h"
#include "WaitUntil.h"

#include <QTemporaryDir>
#include <QTest>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <utility>

namespace {

class ResultLibraryRequestsTest final : public QObject {
  Q_OBJECT

private slots:
  void emptyCatalogAndTrashAreDeliveredAsynchronously();
  void legacyStagingNoticesAreQueuedWithoutHidingCatalogSuccess();
  void invalidDetailsAndTrashActionReturnStructuredErrors();
  void newerDetailsRequestSuppressesStaleCompletion();
  void inaccessibleStoreReturnsErrorsForEveryOperation();
  void cancellationAfterOpenStopsCatalogAndActionWorkers();
};

void ResultLibraryRequestsTest::
    legacyStagingNoticesAreQueuedWithoutHidingCatalogSuccess() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const auto root = std::filesystem::path{directory.path().toStdString()};
  const auto id = std::string(32, 'a');
  const auto staging = root / "Results" / "Staging" / (id + ".bteresult");
  std::filesystem::create_directories(staging.parent_path());
  const std::string bytes = "legacy result payload";
  {
    std::ofstream legacy{staging};
    legacy << bytes;
  }
  bte::bindings::ResultLibraryRequests requests{root / "Results",
                                                root / "Data"};
  for (const auto trash : {false, true}) {
    std::optional<bte::bindings::ResultLibraryRequests::CatalogResult>
        completed;
    requests.requestCatalog(trash, [&completed](auto result) {
      completed.emplace(std::move(result));
    });
    QVERIFY(bte::test::waitUntil([&] { return completed.has_value(); }));
    QVERIFY2(completed->ok(), completed->error().message.c_str());
    QVERIFY(completed->value().entries.empty());
    QCOMPARE(completed->value().notices.size(), std::size_t{1});
    const auto &notice = completed->value().notices.front();
    QCOMPARE(notice.resultId, id);
    QCOMPARE(notice.reason,
             bte::bindings::ResultRecoveryReason::legacyUnleased);
    QVERIFY(!notice.diagnostic.empty());
    QVERIFY(std::filesystem::is_regular_file(staging));
    std::ifstream preserved{staging};
    const std::string actual{std::istreambuf_iterator<char>{preserved},
                             std::istreambuf_iterator<char>{}};
    QCOMPARE(actual, bytes);
  }
}

void ResultLibraryRequestsTest::
    emptyCatalogAndTrashAreDeliveredAsynchronously() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const auto root = std::filesystem::path{directory.path().toStdString()};
  bte::bindings::ResultLibraryRequests requests{root / "Results",
                                                root / "Data"};

  std::optional<bte::bindings::ResultLibraryRequests::CatalogResult> active;
  requests.requestCatalog(
      false, [&active](auto result) { active.emplace(std::move(result)); });
  QVERIFY(bte::test::waitUntil([&] { return active.has_value(); }));
  QVERIFY(active->ok());
  QVERIFY(active->value().entries.empty());
  QVERIFY(active->value().notices.empty());

  std::optional<bte::bindings::ResultLibraryRequests::CatalogResult> trash;
  requests.requestCatalog(
      true, [&trash](auto result) { trash.emplace(std::move(result)); });
  QVERIFY(bte::test::waitUntil([&] { return trash.has_value(); }));
  QVERIFY(trash->ok());
  QVERIFY(trash->value().entries.empty());
  QVERIFY(trash->value().notices.empty());
}

void ResultLibraryRequestsTest::
    inaccessibleStoreReturnsErrorsForEveryOperation() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const auto root = std::filesystem::path{directory.path().toStdString()};
  std::ofstream blocked{root / "Blocked"};
  blocked << "not a directory";
  blocked.close();
  bte::bindings::ResultLibraryRequests requests{root / "Blocked",
                                                root / "Data"};

  std::optional<bte::bindings::ResultLibraryRequests::CatalogResult> catalog;
  requests.requestCatalog(
      false, [&catalog](auto result) { catalog.emplace(std::move(result)); });
  QVERIFY(bte::test::waitUntil([&] { return catalog.has_value(); }));
  QVERIFY(!catalog->ok());

  std::optional<bte::bindings::ResultLibraryRequests::DetailsResult> details;
  requests.requestDetails(std::string(32, 'a'), [&details](auto result) {
    details.emplace(std::move(result));
  });
  QVERIFY(bte::test::waitUntil([&] { return details.has_value(); }));
  QVERIFY(!details->ok());

  std::optional<bte::bindings::ResultLibraryRequests::ActionResult> moved;
  requests.requestMoveToTrash(std::string(32, 'a'), [&moved](auto result) {
    moved.emplace(std::move(result));
  });
  QVERIFY(bte::test::waitUntil([&] { return moved.has_value(); }));
  QVERIFY(!moved->ok());

  std::optional<bte::bindings::ResultLibraryRequests::ActionResult> restored;
  requests.requestRestore(std::string(32, 'a'), [&restored](auto result) {
    restored.emplace(std::move(result));
  });
  QVERIFY(bte::test::waitUntil([&] { return restored.has_value(); }));
  QVERIFY(!restored->ok());

  std::optional<bte::bindings::ResultLibraryRequests::ActionResult> renamed;
  requests.requestRename(
      std::string(32, 'a'), "Valid name",
      [&renamed](auto result) { renamed.emplace(std::move(result)); });
  QVERIFY(bte::test::waitUntil([&] { return renamed.has_value(); }));
  QVERIFY(!renamed->ok());
}

void ResultLibraryRequestsTest::newerDetailsRequestSuppressesStaleCompletion() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const auto root = std::filesystem::path{directory.path().toStdString()};
  bte::bindings::ResultLibraryRequests requests{root / "Results",
                                                root / "Data"};
  bool staleCalled = false;
  bte::bindings::testing::blockNextLibraryDetailsUntilCancelled();
  requests.requestDetails(std::string(32, 'a'),
                          [&staleCalled](auto) { staleCalled = true; });
  QVERIFY(bte::test::waitUntil(
      [] { return bte::bindings::testing::libraryDetailsWorkerStarted(); }));
  std::optional<bte::bindings::ResultLibraryRequests::DetailsResult> newest;
  requests.requestDetails(
      "invalid", [&newest](auto result) { newest.emplace(std::move(result)); });
  QVERIFY(bte::test::waitUntil([&] { return newest.has_value(); }));
  QCOMPARE(newest->error().code, bte::core::ErrorCode::invalidArgument);
  QVERIFY(!staleCalled);
  bte::bindings::testing::clearResultLibraryRequestHooks();
}

void ResultLibraryRequestsTest::
    cancellationAfterOpenStopsCatalogAndActionWorkers() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const auto root = std::filesystem::path{directory.path().toStdString()};
  bte::bindings::ResultLibraryRequests requests{root / "Results",
                                                root / "Data"};

  const auto cancelAtBoundary = [&](auto submitRequest) {
    std::atomic_bool callbackCalled = false;
    bte::bindings::testing::
        blockNextLibraryCancellationBoundaryUntilCancelled();
    submitRequest([&callbackCalled](auto) { callbackCalled.store(true); });
    const auto started = bte::test::waitUntil([] {
      return bte::bindings::testing::libraryCancellationBoundaryWorkerStarted();
    });
    if (!started) {
      bte::bindings::testing::clearResultLibraryRequestHooks();
      return false;
    }
    requests.cancel();
    const auto finished = bte::test::waitUntil([] {
      return bte::bindings::testing::
          libraryCancellationBoundaryWorkerFinished();
    });
    const auto callbackWasSuppressed = !callbackCalled.load();
    bte::bindings::testing::clearResultLibraryRequestHooks();
    return finished && callbackWasSuppressed;
  };

  QVERIFY(cancelAtBoundary([&](auto callback) {
    requests.requestCatalog(false, std::move(callback));
  }));
  QVERIFY(cancelAtBoundary([&](auto callback) {
    requests.requestMoveToTrash(std::string(32, 'a'), std::move(callback));
  }));
  QVERIFY(cancelAtBoundary([&](auto callback) {
    requests.requestRestore(std::string(32, 'a'), std::move(callback));
  }));
  QVERIFY(cancelAtBoundary([&](auto callback) {
    requests.requestRename(std::string(32, 'a'), "Name", std::move(callback));
  }));
}

void ResultLibraryRequestsTest::
    invalidDetailsAndTrashActionReturnStructuredErrors() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const auto root = std::filesystem::path{directory.path().toStdString()};
  bte::bindings::ResultLibraryRequests requests{root / "Results",
                                                root / "Data"};

  std::optional<bte::bindings::ResultLibraryRequests::DetailsResult> details;
  requests.requestDetails("invalid", [&details](auto result) {
    details.emplace(std::move(result));
  });
  QVERIFY(bte::test::waitUntil([&] { return details.has_value(); }));
  QVERIFY(!details->ok());
  QCOMPARE(details->error().code, bte::core::ErrorCode::invalidArgument);

  std::optional<bte::bindings::ResultLibraryRequests::ActionResult> moved;
  requests.requestMoveToTrash(
      "invalid", [&moved](auto result) { moved.emplace(std::move(result)); });
  QVERIFY(bte::test::waitUntil([&] { return moved.has_value(); }));
  QVERIFY(!moved->ok());
  QCOMPARE(moved->error().code, bte::core::ErrorCode::invalidArgument);

  std::optional<bte::bindings::ResultLibraryRequests::ActionResult> restored;
  requests.requestRestore("invalid", [&restored](auto result) {
    restored.emplace(std::move(result));
  });
  QVERIFY(bte::test::waitUntil([&] { return restored.has_value(); }));
  QVERIFY(!restored->ok());
  QCOMPARE(restored->error().code, bte::core::ErrorCode::invalidArgument);

  std::optional<bte::bindings::ResultLibraryRequests::ActionResult> renamed;
  requests.requestRename("invalid", "Valid name", [&renamed](auto result) {
    renamed.emplace(std::move(result));
  });
  QVERIFY(bte::test::waitUntil([&] { return renamed.has_value(); }));
  QVERIFY(!renamed->ok());
  QCOMPARE(renamed->error().code, bte::core::ErrorCode::invalidArgument);
}

} // namespace

QTEST_MAIN(ResultLibraryRequestsTest)

#include "UnitTest_ResultLibraryRequests.moc"
