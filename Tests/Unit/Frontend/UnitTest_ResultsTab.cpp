#include "Bte/Frontend/ResultsTab.h"

#include "Bte/Bindings/BacktestSessionVm.h"
#include "Bte/Data/ReleaseSnapshot.h"
#include "Bte/Results/ResultStore.h"

#include <QAbstractButton>
#include <QApplication>
#include <QByteArray>
#include <QComboBox>
#include <QInputDialog>
#include <QLabel>
#include <QMessageBox>
#include <QPoint>
#include <QPushButton>
#include <QSignalSpy>
#include <QSize>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QWidget>
#include <QtCore/qtenvironmentvariables.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <utility>

namespace {

class ScopedEnvironment final {
public:
  ScopedEnvironment(QByteArray name, const QByteArray &value)
      : name_(std::move(name)),
        previous_(qEnvironmentVariableIsSet(name_.constData())
                      ? std::optional<QByteArray>{qgetenv(name_.constData())}
                      : std::nullopt) {
    qputenv(name_.constData(), value);
  }

  ~ScopedEnvironment() {
    if (previous_.has_value()) {
      qputenv(name_.constData(), *previous_);
    } else {
      qunsetenv(name_.constData());
    }
  }

  ScopedEnvironment(const ScopedEnvironment &) = delete;
  ScopedEnvironment &operator=(const ScopedEnvironment &) = delete;
  ScopedEnvironment(ScopedEnvironment &&) = delete;
  ScopedEnvironment &operator=(ScopedEnvironment &&) = delete;

private:
  QByteArray name_;
  std::optional<QByteArray> previous_;
};

class ResultsTabTest final : public QObject {
  Q_OBJECT

private slots:
  void emptyLibraryHasNoEnabledResultActions();
  void configuredDefaultStoreUsesEnvironmentOverride();
  void corruptResultCannotOpenReplayAndKeepsReadableReason();
  void savedResultDetailsReplayTrashAndRestoreKeepExactId();
  void controlsRemainReachableAtThreeWindowSizes();
};

void ResultsTabTest::controlsRemainReachableAtThreeWindowSizes() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const auto root = std::filesystem::path{directory.path().toStdString()};
  bte::frontend::ResultsTab tab{root / "Results", root / "Data"};
  const auto controls = std::array{
      "resultsLocationCombo",     "resultsRefreshButton",
      "resultsViewDetailsButton", "resultsOpenReplayButton",
      "resultsRenameButton",      "resultsMoveToTrashButton",
      "resultsRestoreButton",
  };
  for (const auto &size :
       {QSize{1024, 768}, QSize{1440, 900}, QSize{1920, 1080}}) {
    tab.resize(size);
    tab.show();
    QApplication::processEvents();
    for (const auto *name : controls) {
      auto *control = tab.findChild<QWidget *>(name);
      QVERIFY(control != nullptr);
      const auto topLeft = control->mapTo(&tab, QPoint{});
      QVERIFY2(topLeft.x() >= 0 && topLeft.y() >= 0, name);
      QVERIFY2(topLeft.x() + control->width() <= tab.width(), name);
      QVERIFY2(topLeft.y() + control->height() <= tab.height(), name);
    }
  }
}

void ResultsTabTest::emptyLibraryHasNoEnabledResultActions() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const auto root = std::filesystem::path{directory.path().toStdString()};
  bte::frontend::ResultsTab tab{root / "Results", root / "Data"};

  auto *table = tab.findChild<QTableWidget *>("resultsTable");
  auto *view = tab.findChild<QPushButton *>("resultsViewDetailsButton");
  auto *open = tab.findChild<QPushButton *>("resultsOpenReplayButton");
  auto *remove = tab.findChild<QPushButton *>("resultsMoveToTrashButton");
  auto *rename = tab.findChild<QPushButton *>("resultsRenameButton");
  QVERIFY(table != nullptr);
  QVERIFY(view != nullptr);
  QVERIFY(open != nullptr);
  QVERIFY(remove != nullptr);
  QVERIFY(rename != nullptr);
  QTRY_COMPARE(table->rowCount(), 0);
  QVERIFY(!view->isEnabled());
  QVERIFY(!open->isEnabled());
  QVERIFY(!remove->isEnabled());
  QVERIFY(!rename->isEnabled());
  emit view->clicked();
  emit remove->clicked();
  auto *restore = tab.findChild<QPushButton *>("resultsRestoreButton");
  QVERIFY(restore != nullptr);
  emit restore->clicked();
  QCOMPARE(table->rowCount(), 0);
}

void ResultsTabTest::configuredDefaultStoreUsesEnvironmentOverride() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const auto root = std::filesystem::path{directory.path().toStdString()};
  ScopedEnvironment resultsRoot{
      "BTE_RESULT_STORE", QByteArray::fromStdString((root / "Store").string())};
  ScopedEnvironment dataRoot{
      "BTE_DATA_STORE", QByteArray::fromStdString((root / "Data").string())};
  bte::frontend::ResultsTab tab;
  auto *table = tab.findChild<QTableWidget *>("resultsTable");
  auto *status = tab.findChild<QLabel *>("resultsStatusLabel");
  QVERIFY(table != nullptr);
  QVERIFY(status != nullptr);
  QTRY_COMPARE(status->text(), QString{"No results in this location."});
  QVERIFY(std::filesystem::is_directory(root / "Store" / "Results"));
}

void ResultsTabTest::corruptResultCannotOpenReplayAndKeepsReadableReason() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const auto root = std::filesystem::path{directory.path().toStdString()};
  auto store = bte::results::ResultStore::open(root / "Store", root / "Data");
  QVERIFY2(store.ok(), store.error().message.c_str());
  const auto corruptId = std::string(32, 'd');
  std::ofstream corrupt{root / "Store" / "Results" /
                        (corruptId + ".bteresult")};
  corrupt << "not a Result database";
  corrupt.close();

  bte::frontend::ResultsTab tab{root / "Store", root / "Data"};
  auto *table = tab.findChild<QTableWidget *>("resultsTable");
  auto *view = tab.findChild<QPushButton *>("resultsViewDetailsButton");
  auto *open = tab.findChild<QPushButton *>("resultsOpenReplayButton");
  QVERIFY(table != nullptr);
  QVERIFY(view != nullptr);
  QVERIFY(open != nullptr);
  QTRY_COMPARE(table->rowCount(), 1);
  table->selectRow(0);
  QVERIFY(!view->isEnabled());
  QVERIFY(!open->isEnabled());
  QVERIFY(table->item(0, 4)->text().startsWith("Unavailable"));
  QVERIFY(table->item(0, 4)->text().size() > 15);
}

void ResultsTabTest::savedResultDetailsReplayTrashAndRestoreKeepExactId() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const auto root = std::filesystem::path{directory.path().toStdString()};
  std::filesystem::create_directories(root / "Source");
  std::ofstream source{root / "Source" / "SYN.csv"};
  source << "symbol,ts,open,high,low,close,volume,schemaName\n"
            "SYN,2024-01-01 09:00:00+00:00,100,102,99,101,1200,ohlcv-1h\n"
            "SYN,2024-01-01 10:00:00+00:00,101,103,100,102,1300,ohlcv-1h\n"
            "SYN,2024-01-01 11:00:00+00:00,102,104,101,103,1400,ohlcv-1h\n";
  source.close();
  const auto built = bte::data::buildReleaseSnapshot({
      .sourceDirectory = root / "Source",
      .storeDirectory = root / "Data",
      .symbols = {"SYN"},
      .rowsPerSegment = 2,
      .calendarHash = std::string(64, 'a'),
      .splitManifestHash = std::string(64, 'b'),
  });
  QVERIFY2(built.ok(), built.error().message.c_str());
  const auto run = bte::bindings::runPersistedBacktestConfiguration(
      {.symbol = "SYN",
       .schema = "ohlcv-1h",
       .startDate = QDate{2024, 1, 1},
       .endDate = QDate{2024, 1, 2},
       .initialCapital = 2'000,
       .quantityShares = 1,
       .selectableStrategy = {}},
      {.resultStore = root / "Store",
       .dataStore = root / "Data",
       .snapshotId = built.value().snapshotId,
       .strategyHash = std::string(64, 'c')});
  QVERIFY2(run.ok(), run.error().message.c_str());
  auto store = bte::results::ResultStore::open(root / "Store", root / "Data");
  QVERIFY2(store.ok(), store.error().message.c_str());
  const auto published = store.value()->publishDraft(run.value().resultId);
  QVERIFY2(published.ok(), published.error().message.c_str());

  bte::frontend::ResultsTab tab{root / "Store", root / "Data"};
  auto *table = tab.findChild<QTableWidget *>("resultsTable");
  auto *view = tab.findChild<QPushButton *>("resultsViewDetailsButton");
  auto *open = tab.findChild<QPushButton *>("resultsOpenReplayButton");
  auto *remove = tab.findChild<QPushButton *>("resultsMoveToTrashButton");
  auto *rename = tab.findChild<QPushButton *>("resultsRenameButton");
  auto *restore = tab.findChild<QPushButton *>("resultsRestoreButton");
  auto *location = tab.findChild<QComboBox *>("resultsLocationCombo");
  auto *details = tab.findChild<QLabel *>("resultsDetailsLabel");
  auto *status = tab.findChild<QLabel *>("resultsStatusLabel");
  QVERIFY(table != nullptr);
  QVERIFY(view != nullptr);
  QVERIFY(open != nullptr);
  QVERIFY(remove != nullptr);
  QVERIFY(rename != nullptr);
  QVERIFY(restore != nullptr);
  QVERIFY(location != nullptr);
  QVERIFY(details != nullptr);
  QVERIFY(status != nullptr);
  QTRY_COMPARE(table->rowCount(), 1);
  QCOMPARE(table->item(0, 0)->text(),
           QString::fromStdString(run.value().resultId));
  table->selectRow(0);
  QVERIFY(view->isEnabled());
  QVERIFY(open->isEnabled());
  view->click();
  QTRY_VERIFY(details->text().contains("Symbol: SYN"));
  QVERIFY(details->text().contains(
      QString::fromStdString(published.value().canonicalResultHash)));
  QSignalSpy replayRequested{&tab,
                             &bte::frontend::ResultsTab::openResultInReplay};
  open->click();
  QCOMPARE(replayRequested.size(), 1);
  QCOMPARE(replayRequested.first().first().toString(),
           QString::fromStdString(run.value().resultId));

  QTimer::singleShot(0, []() {
    for (auto *widget : QApplication::topLevelWidgets()) {
      if (auto *dialog = qobject_cast<QInputDialog *>(widget);
          dialog != nullptr) {
        dialog->reject();
      }
    }
  });
  rename->click();
  QCOMPARE(table->item(0, 0)->text(),
           QString::fromStdString(run.value().resultId));
  QTimer::singleShot(0, []() {
    for (auto *widget : QApplication::topLevelWidgets()) {
      if (auto *dialog = qobject_cast<QInputDialog *>(widget);
          dialog != nullptr) {
        dialog->setTextValue(QString(161, 'x'));
        dialog->accept();
      }
    }
  });
  rename->click();
  QVERIFY(status->text().startsWith("Cannot rename"));
  QCOMPARE(table->item(0, 0)->text(),
           QString::fromStdString(run.value().resultId));

  QTimer::singleShot(0, []() {
    for (auto *widget : QApplication::topLevelWidgets()) {
      if (auto *dialog = qobject_cast<QInputDialog *>(widget);
          dialog != nullptr) {
        dialog->setTextValue("Named SYN run");
        dialog->accept();
      }
    }
  });
  rename->click();
  QTRY_COMPARE(table->item(0, 0)->text(), QString{"Named SYN run"});
  table->selectRow(0);
  view->click();
  QTRY_VERIFY(details->text().contains("Name: Named SYN run"));
  const auto namedResult = store.value()->openResult(run.value().resultId);
  QVERIFY(namedResult.ok());
  QCOMPARE(namedResult.value().canonicalResultHash,
           published.value().canonicalResultHash);

  QTimer::singleShot(0, []() {
    for (auto *widget : QApplication::topLevelWidgets()) {
      if (auto *dialog = qobject_cast<QMessageBox *>(widget);
          dialog != nullptr) {
        dialog->button(QMessageBox::Cancel)->click();
      }
    }
  });
  remove->click();
  QCOMPARE(table->rowCount(), 1);
  QSignalSpy moved{&tab, &bte::frontend::ResultsTab::resultMovedToTrash};
  QTimer::singleShot(0, []() {
    for (auto *widget : QApplication::topLevelWidgets()) {
      if (auto *dialog = qobject_cast<QMessageBox *>(widget);
          dialog != nullptr) {
        dialog->button(QMessageBox::Yes)->click();
      }
    }
  });
  remove->click();
  tab.refresh();
  QCOMPARE(table->rowCount(), 1);
  QTRY_VERIFY(moved.size() == 1 || status->text().startsWith("Cannot"));
  QVERIFY2(moved.size() == 1, status->text().toUtf8().constData());
  QTRY_COMPARE(table->rowCount(), 0);
  QCOMPARE(moved.first().first().toString(),
           QString::fromStdString(run.value().resultId));

  location->setCurrentIndex(1);
  QTRY_COMPARE(table->rowCount(), 1);
  table->selectRow(0);
  QVERIFY(restore->isEnabled());
  restore->click();
  QTRY_COMPARE(table->rowCount(), 0);
  location->setCurrentIndex(0);
  QTRY_COMPARE(table->rowCount(), 1);
  QCOMPARE(table->item(0, 0)->text(), QString{"Named SYN run"});
  const auto reopened = store.value()->openResult(run.value().resultId);
  QVERIFY2(reopened.ok(), reopened.error().message.c_str());
  QCOMPARE(reopened.value().displayName, "Named SYN run");
  QCOMPARE(reopened.value().canonicalResultHash,
           published.value().canonicalResultHash);

  table->selectRow(0);
  QVERIFY(store.value()->moveToTrash(run.value().resultId).ok());
  view->click();
  QTRY_VERIFY(details->text().startsWith("Cannot view result"));
  QTimer::singleShot(0, []() {
    for (auto *widget : QApplication::topLevelWidgets()) {
      if (auto *dialog = qobject_cast<QMessageBox *>(widget);
          dialog != nullptr) {
        dialog->button(QMessageBox::Yes)->click();
      }
    }
  });
  remove->click();
  QTRY_VERIFY(status->text().startsWith("Cannot move result to Trash"));

  location->setCurrentIndex(1);
  QTRY_COMPARE(table->rowCount(), 1);
  table->selectRow(0);
  QVERIFY(store.value()->restore(run.value().resultId).ok());
  restore->click();
  QTRY_VERIFY(status->text().startsWith("Cannot restore result"));

  const auto descriptor = reopened.value().descriptor;
  for (const auto terminalStatus : {
           bte::results::RunStatus::failed,
           bte::results::RunStatus::canceled,
           bte::results::RunStatus::interrupted,
           bte::results::RunStatus::incomplete,
       }) {
    auto writer = store.value()->begin(descriptor);
    QVERIFY2(writer.ok(), writer.error().message.c_str());
    const auto finalized = writer.value()->finalizeAndPromote(
        terminalStatus, {}, "diagnostic display fixture");
    QVERIFY2(finalized.ok(), finalized.error().message.c_str());
  }
  location->setCurrentIndex(0);
  QTRY_COMPARE(table->rowCount(), 5);
  auto failedRow = -1;
  for (const auto &label :
       {"Failed", "Canceled", "Interrupted", "Incomplete"}) {
    auto found = false;
    for (auto row = 0; row < table->rowCount(); ++row) {
      if (table->item(row, 3)->text() == label) {
        found = true;
        if (QString{label} == "Failed") {
          failedRow = row;
        }
      }
    }
    QVERIFY(found);
  }
  QVERIFY(failedRow >= 0);
  table->selectRow(failedRow);
  view->click();
  QTRY_VERIFY(details->text().contains("Final equity: Unavailable"));

  const auto corruptId = std::string(32, 'e');
  std::ofstream corrupt{root / "Store" / "Results" /
                        (corruptId + ".bteresult")};
  corrupt << "not a Result database";
  corrupt.close();
  tab.refresh();
  QTRY_COMPARE(table->rowCount(), 6);
  const auto listed = store.value()->list();
  QVERIFY(listed.ok());
  QCOMPARE(listed.value().size(), std::size_t{6});
  for (auto row = 0; row < table->rowCount(); ++row) {
    const auto &expected = listed.value().at(static_cast<std::size_t>(row));
    QCOMPARE(table->item(row, 2)->text(),
             QString::fromStdString(expected.resultId));
    QCOMPARE(table->item(row, 4)->text().startsWith("Available"),
             expected.available);
    if (expected.resultId == corruptId) {
      table->selectRow(row);
      QVERIFY(!open->isEnabled());
      QVERIFY(!view->isEnabled());
    }
  }
  auto validRow = -1;
  for (auto row = 0; row < table->rowCount(); ++row) {
    if (table->item(row, 2)->text() ==
        QString::fromStdString(run.value().resultId)) {
      validRow = row;
    }
  }
  QVERIFY(validRow >= 0);
  table->selectRow(validRow);
  QVERIFY(open->isEnabled());
  open->click();
  QCOMPARE(replayRequested.size(), 2);
  QCOMPARE(replayRequested.last().first().toString(),
           QString::fromStdString(run.value().resultId));

  QVERIFY(store.value()->moveToTrash(run.value().resultId).ok());
  QTimer::singleShot(0, []() {
    for (auto *widget : QApplication::topLevelWidgets()) {
      if (auto *dialog = qobject_cast<QInputDialog *>(widget);
          dialog != nullptr) {
        dialog->setTextValue("Stale rename");
        dialog->accept();
      }
    }
  });
  rename->click();
  QTRY_VERIFY(status->text().startsWith("Cannot rename result"));
}

} // namespace

QTEST_MAIN(ResultsTabTest)

#include "UnitTest_ResultsTab.moc"
