#include "Bte/Frontend/ReplayTab.h"

#include "Bte/Frontend/QtChartsCandlestickView.h"

#include "WaitUntil.h"

#include <QChart>
#include <QChartView>
#include <QComboBox>
#include <QDateEdit>
#include <QDateTimeAxis>
#include <QDoubleSpinBox>
#include <QGroupBox>
#include <QImage>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSlider>
#include <QSplitter>
#include <QTableWidget>
#include <QTest>
#include <QTimer>
#include <QToolButton>
#include <QtGlobal>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <vector>

namespace {

class ReplayTabTest final : public QObject {
  Q_OBJECT

private slots:
  void exposesReplaySetupControls();
  void exactResultIdStartsOwnedAsynchronousOpen();
  void exposesPlaybackControls();
  void zoomAwayFromNewestEnablesFollowLatestButton();
  void exposesChartAndPortfolioPlaceholders();
  void stepForwardAppendsOneCandle();
  void stepForwardUpdatesPortfolioSnapshot();
  void stepBackRewindsChartAndPortfolioSnapshot();
  void loadReplayResetsPlaybackAndPortfolioSnapshot();
  void initialCapitalChangeUpdatesPortfolioSnapshot();
  void playAdvancesReplay();
  void pauseStopsReplayAdvance();
  void speedSelectionsUpdatePlaybackInterval();
  void keyboardControlsStepAndTogglePlayback();
  void emptyAndCompletedPlaybackBoundariesRemainStable();
  void chartFirstLayoutIsCollapsibleAdjustableAndUsesMostOfTheViewport();
  void rendersNonBlankReplaySnapshot();
  void compactViewportCanScrollToTradeLog();
};

void ReplayTabTest::exposesReplaySetupControls() {
  const bte::frontend::ReplayTab tab;

  QVERIFY(tab.findChild<QComboBox *>("replaySymbolCombo") != nullptr);
  const auto *schemaCombo = tab.findChild<QComboBox *>("replaySchemaCombo");
  QVERIFY(schemaCombo != nullptr);
  QCOMPARE(schemaCombo->count(), 2);
  QCOMPARE(schemaCombo->itemText(0), QString{"ohlcv-1d"});
  QCOMPARE(schemaCombo->itemText(1), QString{"ohlcv-1h"});
  QCOMPARE(schemaCombo->findText("ohlcv-1m"), -1);
  QVERIFY(tab.findChild<QDateEdit *>("replayStartDateEdit") != nullptr);
  QVERIFY(tab.findChild<QDateEdit *>("replayEndDateEdit") != nullptr);
  QVERIFY(tab.findChild<QDoubleSpinBox *>("replayInitialCapitalSpinBox") !=
          nullptr);
  QVERIFY(tab.findChild<QPushButton *>("replayLoadButton") != nullptr);
  QVERIFY(tab.findChild<QComboBox *>("replayResultCombo") != nullptr);
  QVERIFY(tab.findChild<QComboBox *>("replayResultTimeframeCombo") != nullptr);
  QVERIFY(tab.findChild<QPushButton *>("replayOpenResultButton") != nullptr);
  QVERIFY(tab.findChild<QLabel *>("replayResultStatusLabel") != nullptr);
  const auto *previewLabel =
      tab.findChild<QLabel *>("replayBarOnlyPreviewLabel");
  QVERIFY(previewLabel != nullptr);
  QVERIFY(previewLabel->text().contains("no fills or portfolio accounting"));
}

void ReplayTabTest::exactResultIdStartsOwnedAsynchronousOpen() {
  const auto root =
      std::filesystem::temp_directory_path() / "bte-replay-tab-missing-result";
  std::filesystem::remove_all(root);
  {
    bte::frontend::ReplayTab tab{root / "Results", root / "Data"};
    auto *selector = tab.findChild<QComboBox *>("replayResultCombo");
    auto *status = tab.findChild<QLabel *>("replayResultStatusLabel");
    QVERIFY(selector != nullptr);
    QVERIFY(status != nullptr);

    const QString resultId{"0123456789abcdef0123456789abcdef"};
    tab.openResult(resultId);
    QCOMPARE(selector->currentText(), resultId);
    QVERIFY(bte::test::waitUntil(
        [status] { return status->text().contains("Cannot open result"); }));
  }
  std::filesystem::remove_all(root);
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
  QVERIFY(tab.findChild<QSlider *>("replaySeekSlider") != nullptr);
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
  const auto *follow = tab.findChild<QToolButton *>("replayFollowLatestButton");
  const auto *fit = tab.findChild<QToolButton *>("replayFitReachedBarsButton");
  QVERIFY(follow != nullptr);
  QVERIFY(fit != nullptr);
  QCOMPARE(follow->text(), QString{"Follow latest"});
  QCOMPARE(fit->text(), QString{"Fit reached"});
  QVERIFY(!follow->accessibleName().isEmpty());
  QVERIFY(!fit->accessibleName().isEmpty());
}

void ReplayTabTest::zoomAwayFromNewestEnablesFollowLatestButton() {
  bte::frontend::ReplayTab tab;
  auto *chartView = tab.findChild<bte::frontend::QtChartsCandlestickView *>(
      "replayCandlestickChartView");
  auto *zoomIn = tab.findChild<QToolButton *>("replayZoomInButton");
  auto *follow = tab.findChild<QToolButton *>("replayFollowLatestButton");
  QVERIFY(chartView != nullptr);
  QVERIFY(zoomIn != nullptr);
  QVERIFY(follow != nullptr);
  QVERIFY(!follow->isEnabled());

  std::vector<bte::core::Bar> bars;
  bars.reserve(200);
  const auto start = bte::core::Timestamp{
      std::chrono::sys_days{std::chrono::year{2024} / 1 / 1}};
  for (auto index = 0; index < 200; ++index) {
    bars.push_back({.ts = start + std::chrono::hours{index},
                    .open = 100.0,
                    .high = 104.0,
                    .low = 98.0,
                    .close = 102.0,
                    .volume = 1'000.0});
  }
  chartView->setBarWindow(bars);
  tab.resize(1200, 800);
  tab.show();
  QVERIFY(QTest::qWaitForWindowExposed(&tab));
  for (auto pass = 0; pass < 20; ++pass) {
    QCoreApplication::processEvents();
  }
  QVERIFY(!follow->isEnabled());

  QTest::mouseClick(zoomIn, Qt::LeftButton);
  const auto zoomedSlots = chartView->visibleSlots();
  QVERIFY(zoomedSlots.end < bars.size());
  QVERIFY2(follow->isEnabled(),
           "Follow latest must be clickable when zoom hides the newest bar");
  auto *priceView = chartView->findChild<QChartView *>("replayChartView");
  QVERIFY(priceView != nullptr);
  auto *priceChart = priceView->chart();
  auto *timeAxis =
      qobject_cast<QDateTimeAxis *>(priceChart->axes(Qt::Horizontal).first());
  QVERIFY(timeAxis != nullptr);
  const auto zoomedSlotWidth = priceChart->plotArea().width() * 3'600'000.0 /
                               timeAxis->min().msecsTo(timeAxis->max());
  QTest::mouseClick(follow, Qt::LeftButton);
  for (auto pass = 0; pass < 20; ++pass) {
    QCoreApplication::processEvents();
  }
  const auto followedSlots = chartView->visibleSlots();
  QCOMPARE(followedSlots.end, bars.size());
  const auto followedSlotWidth = priceChart->plotArea().width() * 3'600'000.0 /
                                 timeAxis->min().msecsTo(timeAxis->max());
  QVERIFY2(std::abs(followedSlotWidth - zoomedSlotWidth) < 0.05,
           "Follow latest must preserve the zoomed candle density");
  QVERIFY(!follow->isEnabled());
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
  auto *fit = tab.findChild<QToolButton *>("replayFitReachedBarsButton");
  QVERIFY(stepForwardButton != nullptr);
  QVERIFY(chartView != nullptr);
  QVERIFY(progress != nullptr);
  QVERIFY(fit != nullptr);

  QCOMPARE(chartView->candleCount(), 0U);
  QCOMPARE(progress->value(), 0);

  QTest::mouseClick(stepForwardButton, Qt::LeftButton);

  QCOMPARE(chartView->candleCount(), 1U);
  QVERIFY(progress->value() > 0);
  QVERIFY(fit->isEnabled());
  QTest::mouseClick(fit, Qt::LeftButton);
  QCOMPARE(chartView->candleCount(), 1U);
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

  QVERIFY(bte::test::waitUntil(
      [&chartView] { return chartView->candleCount() > 1U; },
      std::chrono::milliseconds{1'000}));
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
  QVERIFY(bte::test::waitUntil(
      [&chartView] { return chartView->candleCount() > 2U; },
      std::chrono::milliseconds{1'000}));

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

void ReplayTabTest::keyboardControlsStepAndTogglePlayback() {
  bte::frontend::ReplayTab tab;
  tab.show();
  tab.activateWindow();
  tab.setFocus();
  QCoreApplication::processEvents();
  auto *chartView = tab.findChild<bte::frontend::QtChartsCandlestickView *>(
      "replayCandlestickChartView");
  auto *timer = tab.findChild<QTimer *>("replayPlaybackTimer");
  QVERIFY(chartView != nullptr);
  QVERIFY(timer != nullptr);

  QTest::keyClick(&tab, Qt::Key_Right);
  QCOMPARE(chartView->candleCount(), 1U);
  QTest::keyClick(&tab, Qt::Key_Left);
  QCOMPARE(chartView->candleCount(), 0U);
  QTest::keyClick(&tab, Qt::Key_Space);
  QVERIFY(timer->isActive());
  QTest::keyClick(&tab, Qt::Key_Space);
  QVERIFY(!timer->isActive());
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

void ReplayTabTest::
    chartFirstLayoutIsCollapsibleAdjustableAndUsesMostOfTheViewport() {
  bte::frontend::ReplayTab tab;
  auto *setup = tab.findChild<QGroupBox *>("replaySetupBox");
  auto *setupToggle = tab.findChild<QToolButton *>("replaySetupToggleButton");
  auto *splitter = tab.findChild<QSplitter *>("replayChartTradeSplitter");
  auto *chartPanel = tab.findChild<QWidget *>("replayChartPanel");
  auto *tradeLogPanel = tab.findChild<QWidget *>("replayTradeLogPanel");
  auto *scrollArea = tab.findChild<QScrollArea *>("replayScrollArea");
  QVERIFY(setup != nullptr);
  QVERIFY(setupToggle != nullptr);
  QVERIFY(splitter != nullptr);
  QVERIFY(chartPanel != nullptr);
  QVERIFY(tradeLogPanel != nullptr);
  QVERIFY(scrollArea != nullptr);
  QVERIFY(setup->isHidden());
  QVERIFY(setupToggle->isCheckable());
  QCOMPARE(setupToggle->text(), QString{"Show replay setup"});
  QVERIFY(setupToggle->focusPolicy() != Qt::NoFocus);
  QCOMPARE(splitter->orientation(), Qt::Vertical);
  QVERIFY(!splitter->childrenCollapsible());

  const auto verifyChartShare = [&] {
    QCoreApplication::processEvents();
    QVERIFY2(chartPanel->height() * 100 >=
                 scrollArea->viewport()->height() * 55,
             qPrintable(QString{"chart=%1 viewport=%2"}
                            .arg(chartPanel->height())
                            .arg(scrollArea->viewport()->height())));
  };
  tab.resize(1200, 800);
  tab.show();
  QVERIFY(QTest::qWaitForWindowExposed(&tab));
  verifyChartShare();
  tab.resize(1440, 900);
  verifyChartShare();

  splitter->setSizes({350, 240});
  QCoreApplication::processEvents();
  QVERIFY(chartPanel->height() >= chartPanel->minimumHeight());
  QVERIFY(tradeLogPanel->height() >= tradeLogPanel->minimumHeight());

  QTest::mouseClick(setupToggle, Qt::LeftButton);
  QVERIFY(setup->isVisible());
  QCOMPARE(setupToggle->text(), QString{"Hide replay setup"});
  QTest::mouseClick(setupToggle, Qt::LeftButton);
  QVERIFY(setup->isHidden());

  tab.resize(1024, 768);
  QCoreApplication::processEvents();
  QCOMPARE(scrollArea->horizontalScrollBar()->maximum(), 0);
  verifyChartShare();
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
