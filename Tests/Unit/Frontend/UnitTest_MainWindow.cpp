#include "MainWindow.h"

#include "Bte/Frontend/BacktestTab.h"
#include "Bte/Frontend/ReplayTab.h"

#include <QComboBox>
#include <QLabel>
#include <QTabWidget>
#include <QTest>

#include <array>

namespace {

class MainWindowTest final : public QObject {
  Q_OBJECT

private slots:
  void mountsBacktestAsTheSelectedApplicationPage();
  void exposesOnlyApprovedApplicationPages();
  void libraryPagesExplainCurrentDeliveryStatus();
  void resultNavigationSelectsReplayAndPassesExactId();
};

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
  const auto *resultsMessage = qobject_cast<QLabel *>(tabs->widget(2));

  QVERIFY(strategiesMessage != nullptr);
  QCOMPARE(strategiesMessage->text(),
           QString{"Saved Strategy persistence is planned."});
  QVERIFY(resultsMessage != nullptr);
  QCOMPARE(
      resultsMessage->text(),
      QString{"Result library management is planned; stored results can be "
              "opened in Replay."});
}

void MainWindowTest::resultNavigationSelectsReplayAndPassesExactId() {
  bte::app::MainWindow window;
  auto *tabs = window.findChild<QTabWidget *>("mainTabWidget");
  auto *backtest = window.findChild<bte::frontend::BacktestTab *>();
  auto *replay = window.findChild<bte::frontend::ReplayTab *>();
  QVERIFY(tabs != nullptr);
  QVERIFY(backtest != nullptr);
  QVERIFY(replay != nullptr);

  const QString resultId{"fedcba9876543210fedcba9876543210"};
  emit backtest->openResultInReplay(resultId);

  QCOMPARE(tabs->currentWidget(), replay);
  auto *selector = replay->findChild<QComboBox *>("replayResultCombo");
  QVERIFY(selector != nullptr);
  QCOMPARE(selector->currentText(), resultId);
}

} // namespace

QTEST_MAIN(MainWindowTest)

#include "UnitTest_MainWindow.moc"
