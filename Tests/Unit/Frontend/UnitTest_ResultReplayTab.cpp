#include "Bte/Frontend/ReplayTab.h"

#include "Bte/Bindings/BacktestSessionVm.h"
#include "Bte/Data/ReleaseSnapshot.h"
#include "Bte/Frontend/QtChartsCandlestickView.h"

#include "WaitUntil.h"

#include <QComboBox>
#include <QDate>
#include <QLabel>
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
  void destructionCancelsAndJoinsOwnedOpenWork();

private:
  std::filesystem::path root_;
  std::string firstResultId_;
  std::string secondResultId_;
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
