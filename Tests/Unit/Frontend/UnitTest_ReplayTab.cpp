#include "Bte/Frontend/ReplayTab.h"

#include "Bte/Frontend/QtChartsCandlestickView.h"

#include <QComboBox>
#include <QDateEdit>
#include <QDoubleSpinBox>
#include <QImage>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QTableWidget>
#include <QTest>
#include <QTimer>
#include <QToolButton>
#include <QtGlobal>

namespace {

class ReplayTabTest final : public QObject {
  Q_OBJECT

private slots:
  void exposesReplaySetupControls();
  void exposesPlaybackControls();
  void exposesChartAndPortfolioPlaceholders();
  void stepForwardAppendsOneCandle();
  void stepForwardUpdatesPortfolioSnapshot();
  void stepBackRewindsChartAndPortfolioSnapshot();
  void loadReplayResetsPlaybackAndPortfolioSnapshot();
  void initialCapitalChangeUpdatesPortfolioSnapshot();
  void playAdvancesReplay();
  void pauseStopsReplayAdvance();
  void speedSelectionsUpdatePlaybackInterval();
  void emptyAndCompletedPlaybackBoundariesRemainStable();
  void rendersNonBlankReplaySnapshot();
  void compactViewportCanScrollToTradeLog();
};

void ReplayTabTest::exposesReplaySetupControls() {
  const bte::frontend::ReplayTab tab;

  QVERIFY(tab.findChild<QComboBox *>("replaySymbolCombo") != nullptr);
  QVERIFY(tab.findChild<QComboBox *>("replaySchemaCombo") != nullptr);
  QVERIFY(tab.findChild<QDateEdit *>("replayStartDateEdit") != nullptr);
  QVERIFY(tab.findChild<QDateEdit *>("replayEndDateEdit") != nullptr);
  QVERIFY(tab.findChild<QDoubleSpinBox *>("replayInitialCapitalSpinBox") !=
          nullptr);
  QVERIFY(tab.findChild<QPushButton *>("replayLoadButton") != nullptr);
}

void ReplayTabTest::exposesPlaybackControls() {
  const bte::frontend::ReplayTab tab;

  QVERIFY(tab.findChild<QToolButton *>("replayStepBackButton") != nullptr);
  QVERIFY(tab.findChild<QToolButton *>("replayPlayPauseButton") != nullptr);
  QVERIFY(tab.findChild<QToolButton *>("replayStepForwardButton") != nullptr);

  const auto *speedCombo = tab.findChild<QComboBox *>("replaySpeedCombo");
  QVERIFY(speedCombo != nullptr);
  QCOMPARE(speedCombo->count(), 4);
  QCOMPARE(speedCombo->itemText(0), QString{"1x"});
  QCOMPARE(speedCombo->itemText(1), QString{"5x"});
  QCOMPARE(speedCombo->itemText(2), QString{"10x"});
  QCOMPARE(speedCombo->itemText(3), QString{"max"});

  QVERIFY(tab.findChild<QProgressBar *>("replayProgressBar") != nullptr);
  const auto *zoomOutButton =
      tab.findChild<QToolButton *>("replayZoomOutButton");
  const auto *zoomInButton = tab.findChild<QToolButton *>("replayZoomInButton");
  const auto *zoomResetButton =
      tab.findChild<QToolButton *>("replayZoomResetButton");
  QVERIFY(zoomOutButton != nullptr);
  QVERIFY(zoomInButton != nullptr);
  QVERIFY(zoomResetButton != nullptr);
  QCOMPARE(zoomOutButton->text(), QString{"Out"});
  QCOMPARE(zoomInButton->text(), QString{"In"});
  QCOMPARE(zoomResetButton->text(), QString{"Reset"});
}

void ReplayTabTest::exposesChartAndPortfolioPlaceholders() {
  const bte::frontend::ReplayTab tab;

  QVERIFY(tab.findChild<QWidget *>("replayChartPanel") != nullptr);
  QVERIFY(tab.findChild<QScrollArea *>("replayScrollArea") != nullptr);
  const auto *chartView =
      tab.findChild<bte::frontend::QtChartsCandlestickView *>(
          "replayCandlestickChartView");
  QVERIFY(chartView != nullptr);
  QCOMPARE(chartView->candleCount(), 0U);
  QVERIFY(tab.findChild<QLabel *>("replayVolumePlaceholder") == nullptr);
  QVERIFY(tab.findChild<QLabel *>("replayCashLabel") != nullptr);
  QVERIFY(tab.findChild<QLabel *>("replayPositionLabel") != nullptr);
  QVERIFY(tab.findChild<QLabel *>("replayMarketValueLabel") != nullptr);
  QVERIFY(tab.findChild<QLabel *>("replayEquityLabel") != nullptr);
  QVERIFY(tab.findChild<QLabel *>("replayPnlLabel") != nullptr);
  QVERIFY(tab.findChild<QLabel *>("replayLastPriceLabel") != nullptr);
  QVERIFY(tab.findChild<QLabel *>("replayBarIndexLabel") != nullptr);
  QVERIFY(tab.findChild<QLabel *>("replayTradeLogTitle") != nullptr);
  QVERIFY(tab.findChild<QTableWidget *>("replayTradeLogTable") != nullptr);
}

void ReplayTabTest::stepForwardAppendsOneCandle() {
  bte::frontend::ReplayTab tab;

  auto *stepForwardButton =
      tab.findChild<QToolButton *>("replayStepForwardButton");
  auto *chartView = tab.findChild<bte::frontend::QtChartsCandlestickView *>(
      "replayCandlestickChartView");
  auto *progress = tab.findChild<QProgressBar *>("replayProgressBar");
  QVERIFY(stepForwardButton != nullptr);
  QVERIFY(chartView != nullptr);
  QVERIFY(progress != nullptr);

  QCOMPARE(chartView->candleCount(), 0U);
  QCOMPARE(progress->value(), 0);

  QTest::mouseClick(stepForwardButton, Qt::LeftButton);

  QCOMPARE(chartView->candleCount(), 1U);
  QVERIFY(progress->value() > 0);
}

void ReplayTabTest::stepForwardUpdatesPortfolioSnapshot() {
  bte::frontend::ReplayTab tab;

  auto *stepForwardButton =
      tab.findChild<QToolButton *>("replayStepForwardButton");
  auto *cashLabel = tab.findChild<QLabel *>("replayCashLabel");
  auto *positionLabel = tab.findChild<QLabel *>("replayPositionLabel");
  auto *marketValueLabel = tab.findChild<QLabel *>("replayMarketValueLabel");
  auto *equityLabel = tab.findChild<QLabel *>("replayEquityLabel");
  auto *pnlLabel = tab.findChild<QLabel *>("replayPnlLabel");
  auto *lastPriceLabel = tab.findChild<QLabel *>("replayLastPriceLabel");
  auto *barIndexLabel = tab.findChild<QLabel *>("replayBarIndexLabel");
  QVERIFY(stepForwardButton != nullptr);
  QVERIFY(cashLabel != nullptr);
  QVERIFY(positionLabel != nullptr);
  QVERIFY(marketValueLabel != nullptr);
  QVERIFY(equityLabel != nullptr);
  QVERIFY(pnlLabel != nullptr);
  QVERIFY(lastPriceLabel != nullptr);
  QVERIFY(barIndexLabel != nullptr);

  QVERIFY(!cashLabel->text().contains("--"));
  QCOMPARE(positionLabel->text(), QString{"Position: 0"});
  QCOMPARE(marketValueLabel->text(), QString{"Market: $0.00"});
  QVERIFY(equityLabel->text().contains("$100,000.00"));
  QCOMPARE(pnlLabel->text(), QString{"PnL: $0.00"});
  QCOMPARE(lastPriceLabel->text(), QString{"Last: --"});
  QVERIFY(barIndexLabel->text().startsWith("Bar: 0/"));

  QTest::mouseClick(stepForwardButton, Qt::LeftButton);
  const auto firstLastPrice = lastPriceLabel->text();

  QVERIFY(!firstLastPrice.contains("--"));
  QVERIFY(barIndexLabel->text().startsWith("Bar: 1/"));

  QTest::mouseClick(stepForwardButton, Qt::LeftButton);
  QVERIFY(lastPriceLabel->text() != firstLastPrice);
  QVERIFY(barIndexLabel->text().startsWith("Bar: 2/"));
}

void ReplayTabTest::stepBackRewindsChartAndPortfolioSnapshot() {
  bte::frontend::ReplayTab tab;

  auto *stepForwardButton =
      tab.findChild<QToolButton *>("replayStepForwardButton");
  auto *stepBackButton = tab.findChild<QToolButton *>("replayStepBackButton");
  auto *chartView = tab.findChild<bte::frontend::QtChartsCandlestickView *>(
      "replayCandlestickChartView");
  auto *lastPriceLabel = tab.findChild<QLabel *>("replayLastPriceLabel");
  auto *barIndexLabel = tab.findChild<QLabel *>("replayBarIndexLabel");
  QVERIFY(stepForwardButton != nullptr);
  QVERIFY(stepBackButton != nullptr);
  QVERIFY(chartView != nullptr);
  QVERIFY(lastPriceLabel != nullptr);
  QVERIFY(barIndexLabel != nullptr);

  QTest::mouseClick(stepForwardButton, Qt::LeftButton);
  const auto firstLastPrice = lastPriceLabel->text();
  QTest::mouseClick(stepForwardButton, Qt::LeftButton);
  QVERIFY(lastPriceLabel->text() != firstLastPrice);
  QCOMPARE(chartView->candleCount(), 2U);
  QVERIFY(barIndexLabel->text().startsWith("Bar: 2/"));

  QTest::mouseClick(stepBackButton, Qt::LeftButton);

  QCOMPARE(chartView->candleCount(), 1U);
  QCOMPARE(lastPriceLabel->text(), firstLastPrice);
  QVERIFY(barIndexLabel->text().startsWith("Bar: 1/"));

  QTest::mouseClick(stepBackButton, Qt::LeftButton);

  QCOMPARE(chartView->candleCount(), 0U);
  QCOMPARE(lastPriceLabel->text(), QString{"Last: --"});
  QVERIFY(barIndexLabel->text().startsWith("Bar: 0/"));
}

void ReplayTabTest::loadReplayResetsPlaybackAndPortfolioSnapshot() {
  bte::frontend::ReplayTab tab;

  auto *stepForwardButton =
      tab.findChild<QToolButton *>("replayStepForwardButton");
  auto *loadButton = tab.findChild<QPushButton *>("replayLoadButton");
  auto *chartView = tab.findChild<bte::frontend::QtChartsCandlestickView *>(
      "replayCandlestickChartView");
  auto *progress = tab.findChild<QProgressBar *>("replayProgressBar");
  auto *lastPriceLabel = tab.findChild<QLabel *>("replayLastPriceLabel");
  auto *barIndexLabel = tab.findChild<QLabel *>("replayBarIndexLabel");
  auto *tradeLog = tab.findChild<QTableWidget *>("replayTradeLogTable");
  QVERIFY(stepForwardButton != nullptr);
  QVERIFY(loadButton != nullptr);
  QVERIFY(chartView != nullptr);
  QVERIFY(progress != nullptr);
  QVERIFY(lastPriceLabel != nullptr);
  QVERIFY(barIndexLabel != nullptr);
  QVERIFY(tradeLog != nullptr);

  QTest::mouseClick(stepForwardButton, Qt::LeftButton);
  QTest::mouseClick(stepForwardButton, Qt::LeftButton);
  QCOMPARE(chartView->candleCount(), 2U);
  QVERIFY(progress->value() > 0);
  QVERIFY(!lastPriceLabel->text().contains("--"));

  QTest::mouseClick(loadButton, Qt::LeftButton);

  QCOMPARE(chartView->candleCount(), 0U);
  QCOMPARE(progress->value(), 0);
  QCOMPARE(lastPriceLabel->text(), QString{"Last: --"});
  QVERIFY(barIndexLabel->text().startsWith("Bar: 0/"));
  QCOMPARE(tradeLog->rowCount(), 0);
}

void ReplayTabTest::initialCapitalChangeUpdatesPortfolioSnapshot() {
  bte::frontend::ReplayTab tab;

  auto *initialCapital =
      tab.findChild<QDoubleSpinBox *>("replayInitialCapitalSpinBox");
  auto *cashLabel = tab.findChild<QLabel *>("replayCashLabel");
  auto *equityLabel = tab.findChild<QLabel *>("replayEquityLabel");
  auto *pnlLabel = tab.findChild<QLabel *>("replayPnlLabel");
  QVERIFY(initialCapital != nullptr);
  QVERIFY(cashLabel != nullptr);
  QVERIFY(equityLabel != nullptr);
  QVERIFY(pnlLabel != nullptr);

  initialCapital->setValue(250'000.0);

  QCOMPARE(cashLabel->text(), QString{"Cash: $250,000.00"});
  QCOMPARE(equityLabel->text(), QString{"Equity: $250,000.00"});
  QCOMPARE(pnlLabel->text(), QString{"PnL: $0.00"});
}

void ReplayTabTest::playAdvancesReplay() {
  bte::frontend::ReplayTab tab;
  tab.show();

  auto *speedCombo = tab.findChild<QComboBox *>("replaySpeedCombo");
  auto *playPauseButton = tab.findChild<QToolButton *>("replayPlayPauseButton");
  auto *chartView = tab.findChild<bte::frontend::QtChartsCandlestickView *>(
      "replayCandlestickChartView");
  auto *progress = tab.findChild<QProgressBar *>("replayProgressBar");
  QVERIFY(speedCombo != nullptr);
  QVERIFY(playPauseButton != nullptr);
  QVERIFY(chartView != nullptr);
  QVERIFY(progress != nullptr);

  speedCombo->setCurrentText("10x");
  QTest::mouseClick(playPauseButton, Qt::LeftButton);

  QTRY_VERIFY_WITH_TIMEOUT(chartView->candleCount() > 1U, 1000);
  QVERIFY(progress->value() > 0);
}

void ReplayTabTest::pauseStopsReplayAdvance() {
  bte::frontend::ReplayTab tab;
  tab.show();

  auto *speedCombo = tab.findChild<QComboBox *>("replaySpeedCombo");
  auto *playPauseButton = tab.findChild<QToolButton *>("replayPlayPauseButton");
  auto *chartView = tab.findChild<bte::frontend::QtChartsCandlestickView *>(
      "replayCandlestickChartView");
  QVERIFY(speedCombo != nullptr);
  QVERIFY(playPauseButton != nullptr);
  QVERIFY(chartView != nullptr);

  speedCombo->setCurrentText("10x");
  QTest::mouseClick(playPauseButton, Qt::LeftButton);
  QTRY_VERIFY_WITH_TIMEOUT(chartView->candleCount() > 2U, 1000);

  QTest::mouseClick(playPauseButton, Qt::LeftButton);
  const auto pausedCount = chartView->candleCount();
  QTest::qWait(250);

  QCOMPARE(chartView->candleCount(), pausedCount);
}

void ReplayTabTest::speedSelectionsUpdatePlaybackInterval() {
  bte::frontend::ReplayTab tab;
  auto *speedCombo = tab.findChild<QComboBox *>("replaySpeedCombo");
  auto *timer = tab.findChild<QTimer *>("replayPlaybackTimer");
  QVERIFY(speedCombo != nullptr);
  QVERIFY(timer != nullptr);

  speedCombo->setCurrentText("5x");
  QCOMPARE(timer->interval(), 200);
  speedCombo->setCurrentText("10x");
  QCOMPARE(timer->interval(), 100);
  speedCombo->setCurrentText("max");
  QCOMPARE(timer->interval(), 0);
  speedCombo->setCurrentText("1x");
  QCOMPARE(timer->interval(), 1000);
}

void ReplayTabTest::emptyAndCompletedPlaybackBoundariesRemainStable() {
  bte::frontend::ReplayTab tab;
  auto *schemaCombo = tab.findChild<QComboBox *>("replaySchemaCombo");
  auto *startDate = tab.findChild<QDateEdit *>("replayStartDateEdit");
  auto *endDate = tab.findChild<QDateEdit *>("replayEndDateEdit");
  auto *stepBackButton = tab.findChild<QToolButton *>("replayStepBackButton");
  auto *stepForwardButton =
      tab.findChild<QToolButton *>("replayStepForwardButton");
  auto *playPauseButton = tab.findChild<QToolButton *>("replayPlayPauseButton");
  auto *chartView = tab.findChild<bte::frontend::QtChartsCandlestickView *>(
      "replayCandlestickChartView");
  QVERIFY(schemaCombo != nullptr);
  QVERIFY(startDate != nullptr);
  QVERIFY(endDate != nullptr);
  QVERIFY(stepBackButton != nullptr);
  QVERIFY(stepForwardButton != nullptr);
  QVERIFY(playPauseButton != nullptr);
  QVERIFY(chartView != nullptr);

  schemaCombo->setCurrentText("ohlcv-1d");
  startDate->setDate(QDate{2018, 5, 1});
  endDate->setDate(QDate{2018, 5, 1});
  QCOMPARE(chartView->candleCount(), 0U);

  QTest::mouseClick(stepBackButton, Qt::LeftButton);
  QCOMPARE(chartView->candleCount(), 0U);
  QTest::mouseClick(stepForwardButton, Qt::LeftButton);
  QCOMPARE(chartView->candleCount(), 1U);
  QTest::mouseClick(stepForwardButton, Qt::LeftButton);
  QCOMPARE(chartView->candleCount(), 1U);

  QTest::mouseClick(playPauseButton, Qt::LeftButton);
  QCOMPARE(chartView->candleCount(), 1U);
  QCOMPARE(playPauseButton->text(), QString{"Play"});

  startDate->setDate(QDate{2035, 1, 1});
  endDate->setDate(QDate{2035, 1, 1});
  QCOMPARE(chartView->candleCount(), 0U);
  QTest::mouseClick(playPauseButton, Qt::LeftButton);
  QCOMPARE(chartView->candleCount(), 0U);
  QCOMPARE(playPauseButton->text(), QString{"Play"});
}

void ReplayTabTest::rendersNonBlankReplaySnapshot() {
  bte::frontend::ReplayTab tab;
  tab.resize(1200, 760);
  tab.show();
  QTest::qWait(100);

  const QImage snapshot =
      tab.grab().toImage().convertToFormat(QImage::Format_RGB32);
  QVERIFY(!snapshot.isNull());
  QVERIFY(snapshot.width() >= 1100);
  QVERIFY(snapshot.height() >= 700);

  const QRgb firstPixel = snapshot.pixel(0, 0);
  auto differentPixels = 0;
  for (int y = 0; y < snapshot.height(); y += 20) {
    for (int x = 0; x < snapshot.width(); x += 20) {
      if (snapshot.pixel(x, y) != firstPixel) {
        ++differentPixels;
      }
    }
  }
  QVERIFY(differentPixels > 20);

  if (qEnvironmentVariableIsSet("BTE_REPLAY_SNAPSHOT_PATH")) {
    QVERIFY(snapshot.save(qEnvironmentVariable("BTE_REPLAY_SNAPSHOT_PATH")));
  }
}

void ReplayTabTest::compactViewportCanScrollToTradeLog() {
  bte::frontend::ReplayTab tab;
  tab.resize(1000, 560);
  tab.show();
  QTest::qWait(100);

  auto *scrollArea = tab.findChild<QScrollArea *>("replayScrollArea");
  QVERIFY(scrollArea != nullptr);
  QVERIFY(scrollArea->verticalScrollBar() != nullptr);
  QVERIFY(scrollArea->verticalScrollBar()->maximum() > 0);

  auto *tradeLog = tab.findChild<QTableWidget *>("replayTradeLogTable");
  QVERIFY(tradeLog != nullptr);

  scrollArea->verticalScrollBar()->setValue(
      scrollArea->verticalScrollBar()->maximum());
  QTest::qWait(50);

  const QPoint tradeLogTopLeft =
      tradeLog->mapTo(scrollArea->viewport(), QPoint{0, 0});
  const QRect viewportRect = scrollArea->viewport()->rect();
  QVERIFY(viewportRect.intersects(QRect{tradeLogTopLeft, tradeLog->size()}));
}

} // namespace

QTEST_MAIN(ReplayTabTest)

#include "UnitTest_ReplayTab.moc"
