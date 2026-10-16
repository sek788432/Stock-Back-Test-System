#include "Bte/Bindings/ResultLibraryRequests.h"

#include "ResultLibraryRequestsTestHooks.h"
#include "WaitUntil.h"

#include <QTemporaryDir>
#include <QTest>

#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <utility>

namespace {

class ResultLibraryRequestsTest final : public QObject {
  Q_OBJECT

private slots:
  void emptyCatalogAndTrashAreDeliveredAsynchronously();
  void invalidDetailsAndTrashActionReturnStructuredErrors();
  void newerDetailsRequestSuppressesStaleCompletion();
  void inaccessibleStoreReturnsErrorsForEveryOperation();
};

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
  QVERIFY(active->value().empty());

  std::optional<bte::bindings::ResultLibraryRequests::CatalogResult> trash;
  requests.requestCatalog(
      true, [&trash](auto result) { trash.emplace(std::move(result)); });
  QVERIFY(bte::test::waitUntil([&] { return trash.has_value(); }));
  QVERIFY(trash->ok());
  QVERIFY(trash->value().empty());
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
