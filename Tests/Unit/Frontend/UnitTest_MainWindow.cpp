#include "MainWindow.h"

#include "Bte/Frontend/BacktestTab.h"

#include <QTabWidget>
#include <QTest>

#include <array>

namespace {

class MainWindowTest final : public QObject {
  Q_OBJECT

private slots:
  void mountsBacktestAsTheSelectedApplicationPage();
  void exposesOnlyApprovedApplicationPages();
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

} // namespace

QTEST_MAIN(MainWindowTest)

#include "UnitTest_MainWindow.moc"
