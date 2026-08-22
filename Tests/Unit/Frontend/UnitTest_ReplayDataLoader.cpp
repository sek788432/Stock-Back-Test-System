#include "Bte/Bindings/ReplayDataLoader.h"

#include <QDate>
#include <QTemporaryDir>
#include <QTest>

#include <filesystem>

namespace {

class ReplayDataLoaderTest final : public QObject {
  Q_OBJECT

private slots:
  void loadReplayBars_loadsHourlyCsvBarsForRequestedRange();
  void loadReplayBars_aggregatesHourlyBarsToDailyBars();
  void loadReplayBars_returnsEmptyBarsForMissingFile();
  void loadBacktestBars_preservesMissingFileError();
  void loadBacktestBars_returnsSuccessfulEmptyRange();
  void loadBacktestBars_validatesDateRangeBoundaries();
  void loadBacktestBars_findsManagedDataFromAnUnrelatedWorkingDirectory();
  void loadBacktestBars_doesNotThrowWhenCurrentDirectoryDisappears();
  void loadBacktestBars_honorsRequestedCancellation();
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

void ReplayDataLoaderTest::loadReplayBars_returnsEmptyBarsForMissingFile() {
  const auto bars = bte::bindings::loadReplayBars(
      QStringLiteral("NOT_A_TRACKED_SYMBOL"), QStringLiteral("ohlcv-1h"),
      QDate{2018, 5, 1}, QDate{2018, 5, 2});

  QVERIFY(bars.empty());
}

void ReplayDataLoaderTest::loadBacktestBars_preservesMissingFileError() {
  const auto result = bte::bindings::loadBacktestBars(
      "NOT_A_TRACKED_SYMBOL", "ohlcv-1h", QDate{2018, 5, 1}, QDate{2018, 5, 2});

  QVERIFY(!result.ok());
  QCOMPARE(result.error().code, bte::core::ErrorCode::notFound);
  QVERIFY(result.error().message.find("NOT_A_TRACKED_SYMBOL") !=
          std::string::npos);
}

void ReplayDataLoaderTest::loadBacktestBars_returnsSuccessfulEmptyRange() {
  const auto hourly = bte::bindings::loadBacktestBars(
      "AAPL", "ohlcv-1h", QDate{2035, 1, 1}, QDate{2035, 1, 2});
  const auto daily = bte::bindings::loadBacktestBars(
      "AAPL", "ohlcv-1d", QDate{2035, 1, 1}, QDate{2035, 1, 2});

  QVERIFY(hourly.ok());
  QVERIFY(hourly.value().empty());
  QVERIFY(daily.ok());
  QVERIFY(daily.value().empty());
}

void ReplayDataLoaderTest::loadBacktestBars_validatesDateRangeBoundaries() {
  const auto invalid = bte::bindings::loadBacktestBars(
      "AAPL", "ohlcv-1h", QDate{}, QDate{2018, 5, 2});
  const auto reversed = bte::bindings::loadBacktestBars(
      "AAPL", "ohlcv-1h", QDate{2018, 5, 3}, QDate{2018, 5, 2});
  const auto maximum = bte::bindings::loadBacktestBars(
      "AAPL", "ohlcv-1h", QDate{9999, 12, 31}, QDate{9999, 12, 31});

  QVERIFY(!invalid.ok());
  QVERIFY(!reversed.ok());
  QVERIFY(maximum.ok());
  QCOMPARE(invalid.error().code, bte::core::ErrorCode::invalidArgument);
  QCOMPARE(reversed.error().code, bte::core::ErrorCode::invalidArgument);
}

void ReplayDataLoaderTest::
    loadBacktestBars_findsManagedDataFromAnUnrelatedWorkingDirectory() {
  const auto original = std::filesystem::current_path();
  QTemporaryDir temporary;
  QVERIFY(temporary.isValid());
  std::filesystem::current_path(temporary.path().toStdString());

  const auto result = bte::bindings::loadBacktestBars(
      "AAPL", "ohlcv-1h", QDate{2018, 5, 1}, QDate{2018, 5, 1});

  std::filesystem::current_path(original);
  QVERIFY(result.ok());
  QVERIFY(!result.value().empty());
}

void ReplayDataLoaderTest::
    loadBacktestBars_doesNotThrowWhenCurrentDirectoryDisappears() {
  const auto original = std::filesystem::current_path();
  QTemporaryDir temporary;
  QVERIFY(temporary.isValid());
  temporary.setAutoRemove(false);
  std::filesystem::current_path(temporary.path().toStdString());
  QVERIFY(std::filesystem::remove(temporary.path().toStdString()));

  const auto result = bte::bindings::loadBacktestBars(
      "AAPL", "ohlcv-1h", QDate{2018, 5, 1}, QDate{2018, 5, 1});

  std::filesystem::current_path(original);
  QVERIFY(result.ok());
  QVERIFY(!result.value().empty());
}

void ReplayDataLoaderTest::loadBacktestBars_honorsRequestedCancellation() {
  bte::core::CancellationSource cancellation;
  cancellation.requestCancellation();

  const auto result =
      bte::bindings::loadBacktestBars("AAPL", "ohlcv-1h", QDate{2018, 5, 1},
                                      QDate{2018, 5, 1}, cancellation.token());

  QVERIFY(!result.ok());
  QCOMPARE(result.error().code, bte::core::ErrorCode::cancelled);
}

} // namespace

QTEST_MAIN(ReplayDataLoaderTest)

#include "UnitTest_ReplayDataLoader.moc"
