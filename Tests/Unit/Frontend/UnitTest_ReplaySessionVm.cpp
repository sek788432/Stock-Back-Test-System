#include "Bte/Bindings/ReplaySessionVm.h"

#include "Bte/Core/Bar.h"

#include <QTest>

#include <chrono>
#include <vector>

namespace {

bte::core::Bar makeBar(const int day, const double close) {
  using namespace std::chrono;
  return bte::core::Bar{
      .ts = bte::core::Timestamp{sys_days{year{2024} / 1 / day}},
      .open = close - 1.0,
      .high = close + 2.0,
      .low = close - 3.0,
      .close = close,
      .volume = 1'000'000.0,
  };
}

class ReplaySessionVmTest final : public QObject {
  Q_OBJECT

private slots:
  void resetLoadsBarsAndInitialPortfolio();
  void stepForwardAdvancesWindowAndSnapshot();
  void stepBackRewindsWindowAndSnapshot();
  void initialCapitalChangeRefreshesCashAndEquity();
};

void ReplaySessionVmTest::resetLoadsBarsAndInitialPortfolio() {
  bte::bindings::ReplaySessionVm vm;

  vm.setInitialCapital(250'000.0);
  vm.reset({makeBar(2, 185.0), makeBar(3, 190.0)});

  QCOMPARE(vm.visibleBars().size(), 0U);
  QCOMPARE(vm.currentIndex(), 0U);
  QCOMPARE(vm.totalBars(), 2U);
  QCOMPARE(vm.progressPercent(), 0);
  const auto snapshot = vm.portfolioSnapshot();
  QCOMPARE(snapshot.cash, 250'000.0);
  QCOMPARE(snapshot.position, 0.0);
  QCOMPARE(snapshot.marketValue, 0.0);
  QCOMPARE(snapshot.equity, 250'000.0);
  QCOMPARE(snapshot.pnl, 0.0);
  QCOMPARE(snapshot.hasLastPrice, false);
}

void ReplaySessionVmTest::stepForwardAdvancesWindowAndSnapshot() {
  bte::bindings::ReplaySessionVm vm;
  vm.reset({makeBar(2, 185.0), makeBar(3, 190.0)});

  QVERIFY(vm.stepForward());

  QCOMPARE(vm.visibleBars().size(), 1U);
  QCOMPARE(vm.currentIndex(), 1U);
  QVERIFY(vm.progressPercent() > 0);
  const auto snapshot = vm.portfolioSnapshot();
  QCOMPARE(snapshot.hasLastPrice, true);
  QCOMPARE(snapshot.lastPrice, 185.0);
}

void ReplaySessionVmTest::stepBackRewindsWindowAndSnapshot() {
  bte::bindings::ReplaySessionVm vm;
  vm.reset({makeBar(2, 185.0), makeBar(3, 190.0)});
  QVERIFY(vm.stepForward());
  QVERIFY(vm.stepForward());

  QVERIFY(vm.stepBack());

  QCOMPARE(vm.visibleBars().size(), 1U);
  QCOMPARE(vm.currentIndex(), 1U);
  const auto snapshot = vm.portfolioSnapshot();
  QCOMPARE(snapshot.hasLastPrice, true);
  QCOMPARE(snapshot.lastPrice, 185.0);

  QVERIFY(vm.stepBack());
  QCOMPARE(vm.visibleBars().size(), 0U);
  QCOMPARE(vm.portfolioSnapshot().hasLastPrice, false);
  QVERIFY(!vm.stepBack());
}

void ReplaySessionVmTest::initialCapitalChangeRefreshesCashAndEquity() {
  bte::bindings::ReplaySessionVm vm;
  vm.reset({makeBar(2, 185.0)});
  QVERIFY(vm.stepForward());

  vm.setInitialCapital(500'000.0);

  const auto snapshot = vm.portfolioSnapshot();
  QCOMPARE(snapshot.cash, 500'000.0);
  QCOMPARE(snapshot.equity, 500'000.0);
  QCOMPARE(snapshot.pnl, 0.0);
  QCOMPARE(snapshot.hasLastPrice, true);
  QCOMPARE(snapshot.lastPrice, 185.0);
}

} // namespace

QTEST_MAIN(ReplaySessionVmTest)

#include "UnitTest_ReplaySessionVm.moc"
