#include "MainWindow.h"

#include "Bte/Frontend/BacktestTab.h"
#include "Bte/Frontend/ReplayTab.h"
#include "Bte/Frontend/ResultsTab.h"

#include <QAction>
#include <QByteArray>
#include <QComboBox>
#include <QLabel>
#include <QMessageBox>
#include <QStandardPaths>
#include <QTabWidget>
#include <QTest>

#include <array>

namespace {

class MainWindowTest final : public QObject {
  Q_OBJECT

private slots:
  void initTestCase();
  void mountsBacktestAsTheSelectedApplicationPage();
  void exposesOnlyApprovedApplicationPages();
  void libraryPagesExplainCurrentDeliveryStatus();
  void resultNavigationSelectsReplayAndPassesExactId();
  void buildInformationShowsLauncherIdentity();
};

void MainWindowTest::initTestCase() {
  QStandardPaths::setTestModeEnabled(true);
}

void MainWindowTest::mountsBacktestAsTheSelectedApplicationPage() {
  const bte::app::MainWindow window;

  const auto *tabs = window.findChild<QTabWidget *>("mainTabWidget");
  QVERIFY(tabs != nullptr);
  const auto backtestIndex = [&]() {
    for (auto index = 0; index < tabs->count(); ++index) {
      if (tabs->tabText(index) == "Backtest") {
        return index;
      }
    }
    return -1;
  }();

  QVERIFY(backtestIndex >= 0);
  QVERIFY(dynamic_cast<bte::frontend::BacktestTab *>(
              tabs->widget(backtestIndex)) != nullptr);
  QCOMPARE(tabs->currentIndex(), backtestIndex);
}

void MainWindowTest::exposesOnlyApprovedApplicationPages() {
  const bte::app::MainWindow window;

  const auto *tabs = window.findChild<QTabWidget *>("mainTabWidget");
  QVERIFY(tabs != nullptr);
  const auto expectedTabs =
      std::array{QString{"Strategies"}, QString{"Backtest"}, QString{"Results"},
                 QString{"Replay"}};

  QCOMPARE(tabs->count(), static_cast<int>(expectedTabs.size()));
  for (auto index = 0; index < tabs->count(); ++index) {
    QCOMPARE(tabs->tabText(index),
             expectedTabs.at(static_cast<std::size_t>(index)));
  }
}

void MainWindowTest::libraryPagesExplainCurrentDeliveryStatus() {
  const bte::app::MainWindow window;

  const auto *tabs = window.findChild<QTabWidget *>("mainTabWidget");
  QVERIFY(tabs != nullptr);

  const auto *strategiesMessage = qobject_cast<QLabel *>(tabs->widget(0));
  const auto *results =
      qobject_cast<bte::frontend::ResultsTab *>(tabs->widget(2));

  QVERIFY(strategiesMessage != nullptr);
  QCOMPARE(strategiesMessage->text(),
           QString{"Saved Strategy persistence is planned."});
  QVERIFY(results != nullptr);
}

void MainWindowTest::resultNavigationSelectsReplayAndPassesExactId() {
  bte::app::MainWindow window;
  auto *tabs = window.findChild<QTabWidget *>("mainTabWidget");
  auto *backtest = window.findChild<bte::frontend::BacktestTab *>();
  auto *replay = window.findChild<bte::frontend::ReplayTab *>();
  auto *results = window.findChild<bte::frontend::ResultsTab *>();
  QVERIFY(tabs != nullptr);
  QVERIFY(backtest != nullptr);
  QVERIFY(replay != nullptr);
  QVERIFY(results != nullptr);

  const QString resultId{"fedcba9876543210fedcba9876543210"};
  emit backtest->openResultInReplay(resultId);

  QCOMPARE(tabs->currentWidget(), replay);
  auto *selector = replay->findChild<QComboBox *>("replayResultCombo");
  QVERIFY(selector != nullptr);
  QCOMPARE(selector->currentText(), resultId);

  tabs->setCurrentWidget(results);
  emit results->openResultInReplay(resultId);
  QCOMPARE(tabs->currentWidget(), replay);
  QCOMPARE(selector->currentText(), resultId);
}

void MainWindowTest::buildInformationShowsLauncherIdentity() {
  qputenv("BTE_LAUNCH_CHECKOUT", "/tmp/stock checkout");
  qputenv("BTE_LAUNCH_BRANCH", "codex/identity-test");
  qputenv("BTE_LAUNCH_COMMIT", "1234567890abcdef");
  qputenv("BTE_LAUNCH_DIRTY", "yes");
  qputenv("BTE_LAUNCH_APP_SHA256", QByteArray(64, 'a'));
  qputenv("BTE_DATA_SNAPSHOT_ID", QByteArray(64, 'b'));

  bte::app::MainWindow window;
  auto *action = window.findChild<QAction *>("buildInformationAction");
  QVERIFY(action != nullptr);
  action->trigger();
  const auto *dialog =
      window.findChild<QMessageBox *>("buildInformationDialog");
  QVERIFY(dialog != nullptr);
  QVERIFY(dialog->isVisible());
  const auto information = dialog->text();
  QVERIFY(information.contains("/tmp/stock checkout"));
  QVERIFY(information.contains("codex/identity-test"));
  QVERIFY(information.contains("1234567890abcdef"));
  QVERIFY(information.contains("Dirty: yes"));
  QVERIFY(information.contains(QByteArray(64, 'a')));
  QVERIFY(information.contains(QByteArray(64, 'b')));

  qunsetenv("BTE_LAUNCH_CHECKOUT");
  qunsetenv("BTE_LAUNCH_BRANCH");
  qunsetenv("BTE_LAUNCH_COMMIT");
  qunsetenv("BTE_LAUNCH_DIRTY");
  qunsetenv("BTE_LAUNCH_APP_SHA256");
  qunsetenv("BTE_DATA_SNAPSHOT_ID");
}

} // namespace

QTEST_MAIN(MainWindowTest)

#include "UnitTest_MainWindow.moc"
