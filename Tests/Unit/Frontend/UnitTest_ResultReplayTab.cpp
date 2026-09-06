#include "Bte/Frontend/ReplayTab.h"

#include "Bte/Bindings/BacktestSessionVm.h"
#include "Bte/Core/Time.h"
#include "Bte/Data/ReleaseSnapshot.h"
#include "Bte/Frontend/QtChartsCandlestickView.h"
#include "Bte/Results/ResultStore.h"

#include "WaitUntil.h"

#include <QComboBox>
#include <QDate>
#include <QLabel>
#include <QMetaObject>
#include <QPushButton>
#include <QSlider>
#include <QTableWidget>
#include <QTest>
#include <QTimer>
#include <QToolButton>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

namespace {

class ResultReplayTabTest final : public QObject {
  Q_OBJECT

private slots:
  void initTestCase();
  void cleanupTestCase();
  void rapidSwitchRejectsStaleCompletionAndPresentsExactFirstFrame();
  void selectionWhilePlayingStopsPriorPlayback();
  void resultControlsCoverEmptyTerminalUnavailableAndNavigationBoundaries();
  void catalogEmptyAndFailureStatesArePresented();
  void configuredDefaultStoreUsesEnvironmentOverride();
  void destructionCancelsAndJoinsOwnedOpenWork();

private:
  std::filesystem::path root_;
  std::string firstResultId_;
  std::string secondResultId_;
  std::string emptyResultId_;
  std::string terminalResultId_;
  std::string unavailableResultId_ = std::string(32, 'd');
};

void ResultReplayTabTest::initTestCase() {
  root_ = std::filesystem::temp_directory_path() / "bte-result-replay-tab";
  std::filesystem::remove_all(root_);
  std::filesystem::create_directories(root_ / "Source");
  std::ofstream source{root_ / "Source" / "SYN.csv"};
  source << "symbol,ts,open,high,low,close,volume,schemaName\n"
            "SYN,2024-01-01 23:00:00+00:00,100,102,99,101,1200,ohlcv-1h\n"
            "SYN,2024-01-02 00:00:00+00:00,101,104,100,103,2300,ohlcv-1h\n"
            "SYN,2024-01-02 01:00:00+00:00,103,105,102,104,3400,ohlcv-1h\n";
  source.close();
  auto built = bte::data::buildReleaseSnapshot({
      .sourceDirectory = root_ / "Source",
      .storeDirectory = root_ / "Data",
      .symbols = {"SYN"},
      .rowsPerSegment = 2,
      .calendarHash = std::string(64, 'a'),
      .splitManifestHash = std::string(64, 'b'),
  });
  QVERIFY2(built.ok(), built.error().message.c_str());
  const auto run = [this, &built](const std::int64_t quantity) {
    return bte::bindings::runPersistedBacktestConfiguration(
        {.symbol = "SYN",
         .schema = "ohlcv-1h",
         .startDate = QDate{2024, 1, 1},
         .endDate = QDate{2024, 1, 2},
         .initialCapital = 2'000,
         .quantityShares = quantity},
        {.resultStore = root_ / "Store",
         .dataStore = root_ / "Data",
         .snapshotId = built.value().snapshotId,
         .strategyHash = std::string(64, 'c')});
  };
  auto first = run(10);
  QVERIFY2(first.ok(), first.error().message.c_str());
  auto second = run(5);
  QVERIFY2(second.ok(), second.error().message.c_str());
  firstResultId_ = first.value().resultId;
  secondResultId_ = second.value().resultId;

  auto reader = bte::data::ReleaseSnapshotReader::open(
      root_ / "Data", built.value().snapshotId);
  QVERIFY2(reader.ok(), reader.error().message.c_str());
  auto selected = reader.value()->select({
      .symbols = {"SYN"},
      .range = {.start =
                    bte::core::time::parseIso8601("2024-01-01 23:00:00+00:00")
                        .value(),
                .end =
                    bte::core::time::parseIso8601("2024-01-02 02:00:00+00:00")
                        .value()},
      .timeframe = "ohlcv-1h",
  });
  QVERIFY2(selected.ok(), selected.error().message.c_str());
  auto store = bte::results::ResultStore::open(root_ / "Store", root_ / "Data");
  QVERIFY2(store.ok(), store.error().message.c_str());
  const auto descriptor = bte::results::RunDescriptor{
      .universe = {"SYN"},
      .range = {.start =
                    bte::core::time::parseIso8601("2024-01-01 23:00:00+00:00")
                        .value(),
                .end =
                    bte::core::time::parseIso8601("2024-01-02 02:00:00+00:00")
                        .value()},
      .initialCapitalMicrodollars = 2'000'000'000,
      .strategyId = "ui-state-fixture",
      .strategyHash = std::string(64, 'c'),
      .dataSelection = selected.value().identity,
  };
  auto emptyWriter = store.value()->begin(descriptor);
  QVERIFY2(emptyWriter.ok(), emptyWriter.error().message.c_str());
  auto empty = emptyWriter.value()->finalizeAndPromote(
      bte::results::RunStatus::completed,
      {.finalEquityMicrodollars = 2'000'000'000, .pnlMicrodollars = 0});
  QVERIFY2(empty.ok(), empty.error().message.c_str());
  emptyResultId_ = empty.value().resultId;

  auto terminalWriter = store.value()->begin(descriptor);
  QVERIFY2(terminalWriter.ok(), terminalWriter.error().message.c_str());
  QVERIFY(terminalWriter.value()
              ->append({{.sequence = 0,
                         .timestamp = descriptor.range.start,
                         .symbol = "SYN",
                         .family = bte::results::RecordFamily::portfolio,
                         .cashMicrodollars = 2'000'000'000,
                         .marketValueMicrodollars = 0,
                         .equityMicrodollars = 2'000'000'000,
                         .positionShares = 0}})
              .ok());
  auto terminal = terminalWriter.value()->finalizeAndPromote(
      bte::results::RunStatus::failed, {}, "intentional UI fixture stop");
  QVERIFY2(terminal.ok(), terminal.error().message.c_str());
  terminalResultId_ = terminal.value().resultId;

  std::ofstream unavailable{root_ / "Store" / "Results" /
                            (unavailableResultId_ + ".bteresult")};
  unavailable << "corrupt";
}

void ResultReplayTabTest::cleanupTestCase() {
  std::filesystem::remove_all(root_);
}

void ResultReplayTabTest::
    rapidSwitchRejectsStaleCompletionAndPresentsExactFirstFrame() {
  bte::frontend::ReplayTab tab{root_ / "Store", root_ / "Data"};
  auto *selector = tab.findChild<QComboBox *>("replayResultCombo");
  auto *status = tab.findChild<QLabel *>("replayResultStatusLabel");
  auto *chart = tab.findChild<bte::frontend::QtChartsCandlestickView *>(
      "replayCandlestickChartView");
  auto *cash = tab.findChild<QLabel *>("replayCashLabel");
  auto *position = tab.findChild<QLabel *>("replayPositionLabel");
  auto *seek = tab.findChild<QSlider *>("replaySeekSlider");
  auto *step = tab.findChild<QToolButton *>("replayStepForwardButton");
  auto *trades = tab.findChild<QTableWidget *>("replayTradeLogTable");
  QVERIFY(selector != nullptr);
  QVERIFY(status != nullptr);
  QVERIFY(chart != nullptr);
  QVERIFY(cash != nullptr);
  QVERIFY(position != nullptr);
  QVERIFY(seek != nullptr);
  QVERIFY(step != nullptr);
  QVERIFY(trades != nullptr);

  tab.openResult(QString::fromStdString(firstResultId_));
  tab.openResult(QString::fromStdString(secondResultId_));

  QVERIFY(bte::test::waitUntil([&] {
    return status->text().startsWith(
        QString{"Result %1"}.arg(QString::fromStdString(secondResultId_)));
  }));
  QCOMPARE(selector->currentText(), QString::fromStdString(secondResultId_));
  QCOMPARE(chart->candleCount(), 1U);
  QCOMPARE(chart->volumePointCount(), 1U);
  QCOMPARE(chart->markerCount(), 0U);
  QVERIFY(!cash->text().contains("--"));
  QCOMPARE(position->text(), QString{"Position: 0"});
  QCOMPARE(seek->minimum(), 0);
  QCOMPARE(seek->maximum(), 2);

  QTest::mouseClick(step, Qt::LeftButton);
  QCOMPARE(chart->candleCount(), 2U);
  QCOMPARE(chart->volumePointCount(), 2U);
  QCOMPARE(chart->markerCount(), 1U);
  QCOMPARE(trades->rowCount(), 1);
  QCOMPARE(position->text(), QString{"Position: 5"});
}

void ResultReplayTabTest::selectionWhilePlayingStopsPriorPlayback() {
  bte::frontend::ReplayTab tab{root_ / "Store", root_ / "Data"};
  auto *status = tab.findChild<QLabel *>("replayResultStatusLabel");
  auto *play = tab.findChild<QToolButton *>("replayPlayPauseButton");
  auto *timer = tab.findChild<QTimer *>("replayPlaybackTimer");
  QVERIFY(status != nullptr);
  QVERIFY(play != nullptr);
  QVERIFY(timer != nullptr);
  tab.openResult(QString::fromStdString(firstResultId_));
  QVERIFY(bte::test::waitUntil(
      [&] { return status->text().startsWith("Result "); }));

  QTest::mouseClick(play, Qt::LeftButton);
  QVERIFY(timer->isActive());
  tab.openResult(QString::fromStdString(secondResultId_));

  QVERIFY(!timer->isActive());
  QCOMPARE(play->text(), QString{"Play"});
  QVERIFY(bte::test::waitUntil([&] {
    return status->text().startsWith(
        QString{"Result %1"}.arg(QString::fromStdString(secondResultId_)));
  }));
}

void ResultReplayTabTest::
    resultControlsCoverEmptyTerminalUnavailableAndNavigationBoundaries() {
  bte::frontend::ReplayTab tab{root_ / "Store", root_ / "Data"};
  auto *selector = tab.findChild<QComboBox *>("replayResultCombo");
  auto *timeframe = tab.findChild<QComboBox *>("replayResultTimeframeCombo");
  auto *open = tab.findChild<QPushButton *>("replayOpenResultButton");
  auto *status = tab.findChild<QLabel *>("replayResultStatusLabel");
  auto *seek = tab.findChild<QSlider *>("replaySeekSlider");
  auto *back = tab.findChild<QToolButton *>("replayStepBackButton");
  auto *step = tab.findChild<QToolButton *>("replayStepForwardButton");
  auto *play = tab.findChild<QToolButton *>("replayPlayPauseButton");
  QVERIFY(selector != nullptr);
  QVERIFY(timeframe != nullptr);
  QVERIFY(open != nullptr);
  QVERIFY(status != nullptr);
  QVERIFY(seek != nullptr);
  QVERIFY(back != nullptr);
  QVERIFY(step != nullptr);
  QVERIFY(play != nullptr);
  QVERIFY(bte::test::waitUntil([&] {
    return selector->findText(QString::fromStdString(unavailableResultId_)) >=
           0;
  }));

  tab.openResult("   ");
  QCOMPARE(status->text(), QString{"Select a saved result"});
  selector->setCurrentText(QString::fromStdString(unavailableResultId_));
  QTest::mouseClick(open, Qt::LeftButton);
  QVERIFY(status->text().startsWith("Result unavailable"));

  tab.openResult(QString::fromStdString(emptyResultId_));
  QVERIFY(bte::test::waitUntil(
      [&] { return status->text() == QString{"Result is valid but empty"}; }));
  QCOMPARE(seek->maximum(), 0);
  QTest::mouseClick(play, Qt::LeftButton);
  QCOMPARE(play->text(), QString{"Play"});

  tab.openResult(QString::fromStdString(terminalResultId_));
  QVERIFY(bte::test::waitUntil(
      [&] { return status->text().contains("intentional UI fixture stop"); }));

  tab.openResult(QString::fromStdString(firstResultId_));
  QVERIFY(bte::test::waitUntil([&] {
    return status->text().startsWith("Result ") && seek->maximum() == 2;
  }));
  QTest::mouseClick(step, Qt::LeftButton);
  QTest::mouseClick(step, Qt::LeftButton);
  QTest::mouseClick(step, Qt::LeftButton);
  QTest::mouseClick(step, Qt::LeftButton);
  QTest::mouseClick(back, Qt::LeftButton);
  QCOMPARE(seek->value(), 1);
  QTest::mouseClick(back, Qt::LeftButton);
  QCOMPARE(seek->value(), 0);
  seek->setValue(2);
  QCOMPARE(seek->value(), 2);
  QTest::mouseClick(play, Qt::LeftButton);
  QVERIFY(seek->value() >= 1);

  timeframe->setCurrentIndex(1);
  QVERIFY(bte::test::waitUntil([&] {
    return status->text().startsWith("Result ") && seek->maximum() == 1;
  }));
  selector->setCurrentText(QString::fromStdString(secondResultId_));
  QVERIFY(QMetaObject::invokeMethod(selector, "textActivated",
                                    Q_ARG(QString, selector->currentText())));
  QVERIFY(bte::test::waitUntil([&] {
    return status->text().startsWith(
        QString{"Result %1"}.arg(QString::fromStdString(secondResultId_)));
  }));
}

void ResultReplayTabTest::catalogEmptyAndFailureStatesArePresented() {
  const auto emptyRoot = root_ / "EmptyCatalog";
  bte::frontend::ReplayTab empty{emptyRoot / "Store", root_ / "Data"};
  auto *emptyStatus = empty.findChild<QLabel *>("replayResultStatusLabel");
  QVERIFY(emptyStatus != nullptr);
  QVERIFY(bte::test::waitUntil(
      [&] { return emptyStatus->text() == "No saved Backtest results"; }));

  const auto blocked = root_ / "BlockedCatalog";
  std::ofstream file{blocked};
  file << "not a directory";
  file.close();
  bte::frontend::ReplayTab failed{blocked, root_ / "Data"};
  auto *failedStatus = failed.findChild<QLabel *>("replayResultStatusLabel");
  QVERIFY(failedStatus != nullptr);
  QVERIFY(bte::test::waitUntil([&] {
    return failedStatus->text().startsWith("Cannot load saved results");
  }));
}

void ResultReplayTabTest::configuredDefaultStoreUsesEnvironmentOverride() {
  const auto configured = root_ / "ConfiguredStore";
  qputenv("BTE_RESULT_STORE", configured.string().c_str());
  {
    bte::frontend::ReplayTab tab;
    auto *status = tab.findChild<QLabel *>("replayResultStatusLabel");
    QVERIFY(status != nullptr);
    QVERIFY(bte::test::waitUntil(
        [&] { return status->text() == "No saved Backtest results"; }));
  }
  qunsetenv("BTE_RESULT_STORE");
}

void ResultReplayTabTest::destructionCancelsAndJoinsOwnedOpenWork() {
  auto tab = std::make_unique<bte::frontend::ReplayTab>(root_ / "Store",
                                                        root_ / "Data");
  tab->openResult(QString::fromStdString(firstResultId_));
  tab->openResult(QString::fromStdString(secondResultId_));
  tab.reset();
  QVERIFY(true);
}

} // namespace

QTEST_MAIN(ResultReplayTabTest)

#include "UnitTest_ResultReplayTab.moc"
