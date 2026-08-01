#include "Bte/Bindings/ReplayDataLoader.h"

#include <QDate>
#include <QTest>

namespace {

class ReplayDataLoaderTest final : public QObject {
  Q_OBJECT

private slots:
  void loadReplayBars_loadsHourlyCsvBarsForRequestedRange();
  void loadReplayBars_aggregatesHourlyBarsToDailyBars();
};

void ReplayDataLoaderTest::
    loadReplayBars_loadsHourlyCsvBarsForRequestedRange() {
  const auto bars = bte::bindings::loadReplayBars(
      QStringLiteral("AAPL"), QStringLiteral("ohlcv-1h"), QDate{2018, 5, 1},
      QDate{2018, 5, 1});

  QVERIFY(!bars.empty());
  QVERIFY(bars.front().open > 0.0);
  QVERIFY(bars.front().high >= bars.front().low);
}

void ReplayDataLoaderTest::loadReplayBars_aggregatesHourlyBarsToDailyBars() {
  const auto bars = bte::bindings::loadReplayBars(
      QStringLiteral("AAPL"), QStringLiteral("ohlcv-1d"), QDate{2018, 5, 1},
      QDate{2018, 5, 2});

  QCOMPARE(bars.size(), 2U);
  QVERIFY(bars.front().volume > 0.0);
  QVERIFY(bars.front().high >= bars.front().low);
}

} // namespace

QTEST_MAIN(ReplayDataLoaderTest)

#include "UnitTest_ReplayDataLoader.moc"
