#include "Bte/Bindings/ReplayDataLoader.h"

#include "Bte/Core/Digest.h"

#include <QDate>
#include <QTemporaryDir>
#include <QTest>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

std::filesystem::path writeDeclaredCsv(const std::filesystem::path &root,
                                       const std::string &timeframe,
                                       const std::string &csvBytes) {
  const auto directory = root / "StockData" / "Extracted";
  std::filesystem::create_directories(directory);
  const auto csvPath = directory / "SYN.csv";
  {
    std::ofstream csv{csvPath, std::ios::binary};
    csv << csvBytes;
  }
  {
    std::ofstream manifest{directory / "SourceManifest.btesource",
                           std::ios::binary};
    manifest << "BTE-SOURCE-MANIFEST-V1\nfile=SYN.csv|" << timeframe << '|'
             << bte::core::sha256(csvBytes) << '\n';
  }
  return directory;
}

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
  void loadBacktestBarsFrom_obeysTheDeclaredSourceTimeframe();
  void loadBacktestBarsFrom_rejectsUnsupportedTimeframes();
  void availableReplaySourcesUsesTheManifestBoundSymbolCatalog();
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

void ReplayDataLoaderTest::
    loadBacktestBarsFrom_obeysTheDeclaredSourceTimeframe() {
  const auto csvBytes = "symbol,ts,open,high,low,close,volume\n"
                        "SYN,2024-01-01T10:00:00Z,100,102,99,101,100\n"
                        "SYN,2024-01-01T11:00:00Z,101,103,100,102,200\n"
                        "SYN,2024-01-02T10:00:00Z,102,104,101,103,300\n";
  QTemporaryDir temporary;
  QVERIFY(temporary.isValid());
  const auto csvDirectory =
      writeDeclaredCsv(temporary.path().toStdString(), "ohlcv-1h", csvBytes);

  const auto hourly = bte::bindings::loadBacktestBarsFrom(
      "SYN", "ohlcv-1h", QDate{2024, 1, 1}, QDate{2024, 1, 2}, csvDirectory);
  const auto dailyFromHourly = bte::bindings::loadBacktestBarsFrom(
      "SYN", "ohlcv-1d", QDate{2024, 1, 1}, QDate{2024, 1, 2}, csvDirectory);
  QVERIFY2(hourly.ok(), hourly.error().message.c_str());
  QVERIFY2(dailyFromHourly.ok(), dailyFromHourly.error().message.c_str());
  QCOMPARE(hourly.value().size(), 3U);
  QCOMPARE(dailyFromHourly.value().size(), 2U);
  const auto &firstDailyBar = dailyFromHourly.value().front();
  QVERIFY(firstDailyBar.ts == bte::core::Timestamp{std::chrono::sys_days{
                                  std::chrono::year{2024} / 1 / 1}});
  QCOMPARE(firstDailyBar.open, 100.0);
  QCOMPARE(firstDailyBar.high, 103.0);
  QCOMPARE(firstDailyBar.low, 99.0);
  QCOMPARE(firstDailyBar.close, 102.0);
  QCOMPARE(firstDailyBar.volume, 300.0);
  const auto &secondDailyBar = dailyFromHourly.value().back();
  QVERIFY(secondDailyBar.ts == bte::core::Timestamp{std::chrono::sys_days{
                                   std::chrono::year{2024} / 1 / 2}});
  QCOMPARE(secondDailyBar.open, 102.0);
  QCOMPARE(secondDailyBar.high, 104.0);
  QCOMPARE(secondDailyBar.low, 101.0);
  QCOMPARE(secondDailyBar.close, 103.0);
  QCOMPARE(secondDailyBar.volume, 300.0);

  writeDeclaredCsv(temporary.path().toStdString(), "ohlcv-1d", csvBytes);
  const auto dailySource = bte::bindings::loadBacktestBarsFrom(
      "SYN", "ohlcv-1d", QDate{2024, 1, 1}, QDate{2024, 1, 2}, csvDirectory);
  const auto hourlyFromDaily = bte::bindings::loadBacktestBarsFrom(
      "SYN", "ohlcv-1h", QDate{2024, 1, 1}, QDate{2024, 1, 2}, csvDirectory);

  QVERIFY2(dailySource.ok(), dailySource.error().message.c_str());
  QCOMPARE(dailySource.value().size(), 3U);
  QVERIFY(!hourlyFromDaily.ok());
  QCOMPARE(hourlyFromDaily.error().code, bte::core::ErrorCode::schemaMismatch);
}

void ReplayDataLoaderTest::loadBacktestBarsFrom_rejectsUnsupportedTimeframes() {
  QTemporaryDir temporary;
  QVERIFY(temporary.isValid());
  const auto csvDirectory =
      writeDeclaredCsv(temporary.path().toStdString(), "ohlcv-1m",
                       "symbol,ts,open,high,low,close,volume\n"
                       "SYN,2024-01-01T10:00:00Z,100,102,99,101,100\n");

  const auto unsupportedSource = bte::bindings::loadBacktestBarsFrom(
      "SYN", "ohlcv-1d", QDate{2024, 1, 1}, QDate{2024, 1, 1}, csvDirectory);
  const auto unsupportedRequest = bte::bindings::loadBacktestBarsFrom(
      "SYN", "ohlcv-5m", QDate{2024, 1, 1}, QDate{2024, 1, 1}, csvDirectory);

  QVERIFY(!unsupportedSource.ok());
  QVERIFY(!unsupportedRequest.ok());
  QCOMPARE(unsupportedSource.error().code,
           bte::core::ErrorCode::schemaMismatch);
  QCOMPARE(unsupportedRequest.error().code,
           bte::core::ErrorCode::schemaMismatch);
}

void ReplayDataLoaderTest::
    availableReplaySourcesUsesTheManifestBoundSymbolCatalog() {
  QTemporaryDir temporary;
  QVERIFY(temporary.isValid());
  const auto csvDirectory =
      writeDeclaredCsv(temporary.path().toStdString(), "ohlcv-1d",
                       "symbol,ts,open,high,low,close,volume\n"
                       "SYN,2024-01-01T00:00:00Z,100,102,99,101,100\n");
  {
    std::ofstream symbols{csvDirectory.parent_path() / "Symbols.txt"};
    symbols << "SYN\nMISSING\n";
  }

  const auto sources = bte::bindings::availableReplaySources(csvDirectory);

  QVERIFY2(sources.ok(), sources.error().message.c_str());
  QCOMPARE(sources.value().size(), 1U);
  QCOMPARE(sources.value().front().symbol, std::string{"SYN"});
  QCOMPARE(sources.value().front().timeframe, std::string{"ohlcv-1d"});
}

} // namespace

QTEST_MAIN(ReplayDataLoaderTest)

#include "UnitTest_ReplayDataLoader.moc"
