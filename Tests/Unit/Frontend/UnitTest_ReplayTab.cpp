#include "Bte/Frontend/ReplayTab.h"

#include "Bte/Bindings/ReplayDataLoader.h"
#include "Bte/Core/Digest.h"
#include "Bte/Frontend/QtChartsCandlestickView.h"

#include "WaitUntil.h"

#include <QApplication>
#include <QChart>
#include <QChartView>
#include <QComboBox>
#include <QDateEdit>
#include <QDateTimeAxis>
#include <QDoubleSpinBox>
#include <QGroupBox>
#include <QImage>
#include <QLabel>
#include <QMouseEvent>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSlider>
#include <QSplitter>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QToolButton>
#include <QtGlobal>

#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

namespace {

class ScopedCurrentDirectory final {
public:
  explicit ScopedCurrentDirectory(const std::filesystem::path &path)
      : previous_{std::filesystem::current_path()} {
    std::filesystem::current_path(path);
  }

  ~ScopedCurrentDirectory() {
    std::error_code ignored;
    std::filesystem::current_path(previous_, ignored);
  }

  ScopedCurrentDirectory(const ScopedCurrentDirectory &) = delete;
  ScopedCurrentDirectory &operator=(const ScopedCurrentDirectory &) = delete;
  ScopedCurrentDirectory(ScopedCurrentDirectory &&) = delete;
  ScopedCurrentDirectory &operator=(ScopedCurrentDirectory &&) = delete;

private:
  std::filesystem::path previous_;
};

[[nodiscard]] bool writeHourlyReplaySource(const std::filesystem::path &root) {
  const auto sourceDirectory = root / "StockData" / "Extracted";
  std::filesystem::create_directories(sourceDirectory);
  const std::string csv = "symbol,ts,open,high,low,close,volume\n"
                          "AAPL,2024-01-01T10:00:00Z,100,102,99,101,100\n";
  std::ofstream source{sourceDirectory / "AAPL.csv", std::ios::binary};
  source << csv;
  std::ofstream symbols{sourceDirectory.parent_path() / "Symbols.txt",
                        std::ios::binary};
  symbols << "AAPL\n";
  std::ofstream manifest{sourceDirectory / "SourceManifest.btesource",
                         std::ios::binary};
  manifest << "BTE-SOURCE-MANIFEST-V1\n"
           << "file=AAPL.csv|ohlcv-1h|" << bte::core::sha256(csv) << '\n';
  return source.good() && symbols.good() && manifest.good();
}

class ReplayTabTest final : public QObject {
  Q_OBJECT

private slots:
  void exposesReplaySetupControls();
  void unsupportedSourcesDisableReplaySetup();
  void unavailableSourceCatalogDisablesReplaySetup();
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
  void expandedSetupFitsAndCanBeReachedAtSupportedWindowSizes();
  void collapsedSetupKeepsModeSourceAndResultStatusVisible();
  void chartAndDetailsCanBeResizedByDraggingSplitterHandle();
  void rendersNonBlankReplaySnapshot();
  void compactViewportCanScrollToTradeLog();
};

void ReplayTabTest::exposesReplaySetupControls() {
  QTemporaryDir temporary;
  QVERIFY(temporary.isValid());
  const auto sourceDirectory =
      std::filesystem::path{temporary.path().toStdString()} / "StockData" /
      "Extracted";
  std::filesystem::create_directories(sourceDirectory);
  const std::string hourlyCsv =
      "symbol,ts,open,high,low,close,volume\n"
      "AAPL,2024-01-01T10:00:00Z,100,102,99,101,100\n";
  const std::string dailyCsv =
      "symbol,ts,open,high,low,close,volume\n"
      "MSFT,2024-01-01T00:00:00Z,200,203,199,202,500\n";
  const std::string unsupportedCsv =
      "symbol,ts,open,high,low,close,volume\n"
      "SPY,2024-01-01T10:00:00Z,300,303,299,302,1000\n";
  {
    std::ofstream output{sourceDirectory / "AAPL.csv", std::ios::binary};
    output << hourlyCsv;
  }
  {
    std::ofstream output{sourceDirectory / "MSFT.csv", std::ios::binary};
    output << dailyCsv;
  }
  {
    std::ofstream output{sourceDirectory / "SPY.csv", std::ios::binary};
    output << unsupportedCsv;
  }
  {
    std::ofstream symbols{sourceDirectory.parent_path() / "Symbols.txt",
                          std::ios::binary};
    symbols << "AAPL\nMSFT\nSPY\n";
  }
  {
    std::ofstream manifest{sourceDirectory / "SourceManifest.btesource",
                           std::ios::binary};
    manifest << "BTE-SOURCE-MANIFEST-V1\n"
             << "file=AAPL.csv|ohlcv-1h|" << bte::core::sha256(hourlyCsv)
             << '\n'
             << "file=MSFT.csv|ohlcv-1d|" << bte::core::sha256(dailyCsv) << '\n'
             << "file=SPY.csv|ohlcv-1m|" << bte::core::sha256(unsupportedCsv)
             << '\n';
  }
  const ScopedCurrentDirectory currentDirectory{temporary.path().toStdString()};
  const bte::frontend::ReplayTab tab;

  QVERIFY(tab.findChild<QComboBox *>("replaySymbolCombo") != nullptr);
  auto *symbolCombo = tab.findChild<QComboBox *>("replaySymbolCombo");
  QVERIFY(symbolCombo != nullptr);
  QCOMPARE(symbolCombo->count(), 2);
  QCOMPARE(symbolCombo->itemText(0), QString{"AAPL"});
  QCOMPARE(symbolCombo->itemText(1), QString{"MSFT"});
  const auto *schemaCombo = tab.findChild<QComboBox *>("replaySchemaCombo");
  QVERIFY(schemaCombo != nullptr);
  QCOMPARE(symbolCombo->currentData(Qt::UserRole).toString(),
           QString{"ohlcv-1h"});
  QCOMPARE(schemaCombo->count(), 2);
  QCOMPARE(schemaCombo->itemText(0), QString{"ohlcv-1d"});
  QCOMPARE(schemaCombo->itemText(1), QString{"ohlcv-1h"});
  symbolCombo->setCurrentIndex(1);
  QCOMPARE(symbolCombo->currentData(Qt::UserRole).toString(),
           QString{"ohlcv-1d"});
  QCOMPARE(schemaCombo->count(), 1);
  QCOMPARE(schemaCombo->itemText(0), QString{"ohlcv-1d"});
  symbolCombo->setCurrentIndex(0);
  QCOMPARE(schemaCombo->count(), 2);
  QCOMPARE(schemaCombo->itemText(1), QString{"ohlcv-1h"});
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

void ReplayTabTest::unsupportedSourcesDisableReplaySetup() {
  QTemporaryDir temporary;
  QVERIFY(temporary.isValid());
  const auto sourceDirectory =
      std::filesystem::path{temporary.path().toStdString()} / "StockData" /
      "Extracted";
  std::filesystem::create_directories(sourceDirectory);
  {
    std::ofstream csv{sourceDirectory / "SPY.csv", std::ios::binary};
    csv << "symbol,ts,open,high,low,close,volume,schemaName\n"
           "SPY,2024-01-01T10:00:00Z,300,303,299,302,1000,ohlcv-1m\n";
  }
  {
    std::ofstream symbols{sourceDirectory.parent_path() / "Symbols.txt",
                          std::ios::binary};
    symbols << "SPY\n";
  }
  const ScopedCurrentDirectory currentDirectory{temporary.path().toStdString()};
  const bte::frontend::ReplayTab tab;
  const auto *symbolCombo = tab.findChild<QComboBox *>("replaySymbolCombo");
  const auto *schemaCombo = tab.findChild<QComboBox *>("replaySchemaCombo");
  const auto *loadButton = tab.findChild<QPushButton *>("replayLoadButton");

  QVERIFY(symbolCombo != nullptr);
  QVERIFY(schemaCombo != nullptr);
  QVERIFY(loadButton != nullptr);
  QCOMPARE(symbolCombo->currentText(), QString{"No supported CSV sources"});
  QVERIFY(!symbolCombo->isEnabled());
  QVERIFY(!schemaCombo->isEnabled());
  QVERIFY(!loadButton->isEnabled());
}

void ReplayTabTest::unavailableSourceCatalogDisablesReplaySetup() {
  QTemporaryDir temporary;
  QVERIFY(temporary.isValid());
  const auto sourceDirectory =
      std::filesystem::path{temporary.path().toStdString()} / "StockData" /
      "Extracted";
  std::filesystem::create_directories(sourceDirectory);
  {
    std::ofstream csv{sourceDirectory / "BROKEN.csv", std::ios::binary};
    csv << "invalid,csv,header\n";
  }
  {
    std::ofstream symbols{sourceDirectory.parent_path() / "Symbols.txt",
                          std::ios::binary};
    symbols << "BROKEN\n";
  }
  const ScopedCurrentDirectory currentDirectory{temporary.path().toStdString()};
  const bte::frontend::ReplayTab tab;
  const auto *symbolCombo = tab.findChild<QComboBox *>("replaySymbolCombo");
  const auto *schemaCombo = tab.findChild<QComboBox *>("replaySchemaCombo");
  const auto *loadButton = tab.findChild<QPushButton *>("replayLoadButton");

  QVERIFY(symbolCombo != nullptr);
  QVERIFY(schemaCombo != nullptr);
  QVERIFY(loadButton != nullptr);
  QCOMPARE(symbolCombo->currentText(), QString{"Source catalog unavailable"});
  QVERIFY(!symbolCombo->isEnabled());
  QVERIFY(!schemaCombo->isEnabled());
  QVERIFY(!loadButton->isEnabled());
}

void ReplayTabTest::exactResultIdStartsOwnedAsynchronousOpen() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const auto root = std::filesystem::path{directory.path().toStdString()};
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
  const auto zoomedSlotWidth =
      priceChart->plotArea().width() * 3'600'000.0 /
      static_cast<double>(timeAxis->min().msecsTo(timeAxis->max()));
  QTest::mouseClick(follow, Qt::LeftButton);
  for (auto pass = 0; pass < 20; ++pass) {
    QCoreApplication::processEvents();
  }
  const auto followedSlots = chartView->visibleSlots();
  QCOMPARE(followedSlots.end, bars.size());
  const auto followedSlotWidth =
      priceChart->plotArea().width() * 3'600'000.0 /
      static_cast<double>(timeAxis->min().msecsTo(timeAxis->max()));
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
  QVERIFY(tab.findChild<QLabel *>("replayRealizedPnlLabel") != nullptr);
  QVERIFY(tab.findChild<QLabel *>("replayUnrealizedPnlLabel") != nullptr);
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
  auto *realizedPnlLabel = tab.findChild<QLabel *>("replayRealizedPnlLabel");
  auto *unrealizedPnlLabel =
      tab.findChild<QLabel *>("replayUnrealizedPnlLabel");
  auto *lastPriceLabel = tab.findChild<QLabel *>("replayLastPriceLabel");
  auto *barIndexLabel = tab.findChild<QLabel *>("replayBarIndexLabel");
  QVERIFY(stepForwardButton != nullptr);
  QVERIFY(cashLabel != nullptr);
  QVERIFY(positionLabel != nullptr);
  QVERIFY(marketValueLabel != nullptr);
  QVERIFY(equityLabel != nullptr);
  QVERIFY(pnlLabel != nullptr);
  QVERIFY(realizedPnlLabel != nullptr);
  QVERIFY(unrealizedPnlLabel != nullptr);
  QVERIFY(lastPriceLabel != nullptr);
  QVERIFY(barIndexLabel != nullptr);

  QVERIFY(!cashLabel->text().contains("--"));
  QCOMPARE(positionLabel->text(), QString{"Position: 0"});
  QCOMPARE(marketValueLabel->text(), QString{"Market: $0.00"});
  QVERIFY(equityLabel->text().contains("$100,000.00"));
  QCOMPARE(pnlLabel->text(), QString{"PnL: $0.00"});
  QCOMPARE(realizedPnlLabel->text(), QString{"Realized P/L: $0.00"});
  QCOMPARE(unrealizedPnlLabel->text(), QString{"Unrealized P/L: $0.00"});
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

void ReplayTabTest::expandedSetupFitsAndCanBeReachedAtSupportedWindowSizes() {
  QTemporaryDir temporary;
  QVERIFY(temporary.isValid());
  const auto root = std::filesystem::path{temporary.path().toStdString()};
  QVERIFY(writeHourlyReplaySource(root));
  const ScopedCurrentDirectory currentDirectory{root};
  bte::frontend::ReplayTab tab{root / "Results", root / "Data"};
  auto *setup = tab.findChild<QGroupBox *>("replaySetupBox");
  auto *setupToggle = tab.findChild<QToolButton *>("replaySetupToggleButton");
  auto *scrollArea = tab.findChild<QScrollArea *>("replayScrollArea");
  QVERIFY(setup != nullptr);
  QVERIFY(setupToggle != nullptr);
  QVERIFY(scrollArea != nullptr);

  tab.resize(1024, 768);
  tab.show();
  QVERIFY(QTest::qWaitForWindowExposed(&tab));
  QTest::mouseClick(setupToggle, Qt::LeftButton);
  QVERIFY(setup->isVisible());

  const std::array<QWidget *, 9> controls{
      tab.findChild<QComboBox *>("replayResultCombo"),
      tab.findChild<QComboBox *>("replayResultTimeframeCombo"),
      tab.findChild<QPushButton *>("replayOpenResultButton"),
      tab.findChild<QComboBox *>("replaySymbolCombo"),
      tab.findChild<QComboBox *>("replaySchemaCombo"),
      tab.findChild<QDateEdit *>("replayStartDateEdit"),
      tab.findChild<QDateEdit *>("replayEndDateEdit"),
      tab.findChild<QDoubleSpinBox *>("replayInitialCapitalSpinBox"),
      tab.findChild<QPushButton *>("replayLoadButton")};
  for (auto *control : controls) {
    QVERIFY(control != nullptr);
    QVERIFY(control->isEnabled());
  }

  const std::array<QSize, 3> windowSizes{QSize{1024, 768}, QSize{1200, 800},
                                         QSize{1440, 900}};
  for (const auto &windowSize : windowSizes) {
    tab.resize(windowSize);
    QCoreApplication::processEvents();
    QCOMPARE(scrollArea->horizontalScrollBar()->maximum(), 0);

    for (auto *control : controls) {
      scrollArea->ensureWidgetVisible(control, 0, 8);
      QCoreApplication::processEvents();
      const auto topLeft = control->mapTo(scrollArea->viewport(), QPoint{0, 0});
      QVERIFY2(topLeft.x() >= 0 && topLeft.x() + control->width() <=
                                       scrollArea->viewport()->width(),
               qPrintable(QString{"%1 exceeds the %2px viewport"}
                              .arg(control->objectName())
                              .arg(windowSize.width())));
      QVERIFY(control->focusPolicy() != Qt::NoFocus);
      QVERIFY(scrollArea->viewport()->rect().intersects(
          QRect{topLeft, control->size()}));
    }
  }

  const std::array<QWidget *, 8> keyboardOrder{
      controls[1], controls[2], controls[3], controls[4],
      controls[5], controls[6], controls[7], controls[8]};
  controls[0]->setFocus();
  QCOMPARE(QApplication::focusWidget(), controls[0]);
  QTest::keyClick(controls[0], Qt::Key_Tab);
  QCOMPARE(QApplication::focusWidget(), keyboardOrder.front());
  for (std::size_t index = 1; index < keyboardOrder.size(); ++index) {
    for (auto attempt = 0;
         attempt < 4 && QApplication::focusWidget() != keyboardOrder[index];
         ++attempt) {
      auto *focusedWidget = QApplication::focusWidget();
      QVERIFY(focusedWidget != nullptr);
      QTest::keyClick(focusedWidget, Qt::Key_Tab);
    }
    QCOMPARE(QApplication::focusWidget(), keyboardOrder[index]);
    const auto topLeft =
        keyboardOrder[index]->mapTo(scrollArea->viewport(), QPoint{0, 0});
    QVERIFY(scrollArea->viewport()->rect().intersects(
        QRect{topLeft, keyboardOrder[index]->size()}));
  }
}

void ReplayTabTest::collapsedSetupKeepsModeSourceAndResultStatusVisible() {
  QTemporaryDir temporary;
  QVERIFY(temporary.isValid());
  const auto root = std::filesystem::path{temporary.path().toStdString()};
  QVERIFY(writeHourlyReplaySource(root));
  const ScopedCurrentDirectory currentDirectory{root};
  const auto blockedCatalog = root / "BlockedCatalog";
  {
    std::ofstream file{blockedCatalog};
    file << "not a directory";
  }
  bte::frontend::ReplayTab tab{blockedCatalog, root / "Data"};
  tab.resize(1024, 768);
  tab.show();
  QVERIFY(QTest::qWaitForWindowExposed(&tab));

  auto *setup = tab.findChild<QGroupBox *>("replaySetupBox");
  auto *setupToggle = tab.findChild<QToolButton *>("replaySetupToggleButton");
  auto *loadButton = tab.findChild<QPushButton *>("replayLoadButton");
  auto *context = tab.findChild<QLabel *>("replayModeSourceLabel");
  auto *status = tab.findChild<QLabel *>("replayResultStatusLabel");
  auto *timeframe = tab.findChild<QComboBox *>("replaySchemaCombo");
  QVERIFY(setup != nullptr);
  QVERIFY(setupToggle != nullptr);
  QVERIFY(loadButton != nullptr);
  QVERIFY(context != nullptr);
  QVERIFY(status != nullptr);
  QVERIFY(timeframe != nullptr);
  QVERIFY(setup->isHidden());
  QVERIFY(context->isVisible());
  QVERIFY(context->text().contains("Bar-only preview"));
  QVERIFY(context->text().contains("AAPL"));
  QVERIFY(bte::test::waitUntil([status] {
    return status->text().startsWith("Cannot load saved results");
  }));
  QVERIFY(status->isVisible());
  const auto hourlyIndex = timeframe->findText("ohlcv-1h");
  QVERIFY(hourlyIndex >= 0);
  timeframe->setCurrentIndex(hourlyIndex);
  QVERIFY(context->text().contains("ohlcv-1h"));
  QVERIFY(status->isVisible());
  const auto catalogError = status->text();
  setupToggle->click();
  QVERIFY(setup->isVisible());
  loadButton->click();
  QCOMPARE(status->text(), catalogError);
  QVERIFY(status->isVisible());
  setupToggle->click();
  QVERIFY(setup->isHidden());
  QVERIFY(status->isVisible());

  std::filesystem::remove(blockedCatalog);
  std::filesystem::create_directories(blockedCatalog);
  tab.hide();
  tab.show();
  QVERIFY(bte::test::waitUntil(
      [status] { return status->text() == "No saved Backtest results"; }));
  QVERIFY(!status->isVisible());

  tab.openResult("missing-result");
  QVERIFY(bte::test::waitUntil(
      [status] { return status->text().contains("Cannot open result"); }));
  QVERIFY(setup->isHidden());
  QVERIFY(context->isVisible());
  QVERIFY(status->isVisible());
  QVERIFY(context->text().contains("Result replay"));
  QVERIFY(context->text().contains("missing-result"));

  bte::frontend::ReplayTab neutral{root / "EmptyCatalog", root / "Data"};
  neutral.resize(1024, 768);
  neutral.show();
  QVERIFY(QTest::qWaitForWindowExposed(&neutral));
  auto *neutralContext = neutral.findChild<QLabel *>("replayModeSourceLabel");
  auto *neutralStatus = neutral.findChild<QLabel *>("replayResultStatusLabel");
  QVERIFY(neutralContext != nullptr);
  QVERIFY(neutralStatus != nullptr);
  QVERIFY(bte::test::waitUntil([neutralStatus] {
    return neutralStatus->text() == "No saved Backtest results";
  }));
  QVERIFY(neutralContext->isVisible());
  QVERIFY(!neutralStatus->isVisible());

  neutral.openResult(QString{});
  QVERIFY(bte::test::waitUntil([neutralStatus] {
    return neutralStatus->text() == "Select a saved result";
  }));
  QVERIFY(neutralStatus->isVisible());
  QCOMPARE(neutralContext->text(), QString{"Result replay · Hourly"});

  QTemporaryDir noSourceTemporary;
  QVERIFY(noSourceTemporary.isValid());
  const auto noSourceRoot =
      std::filesystem::path{noSourceTemporary.path().toStdString()};
  std::filesystem::create_directories(noSourceRoot / "StockData" / "Extracted");
  {
    const ScopedCurrentDirectory emptyDirectory{noSourceRoot};
    bte::frontend::ReplayTab noSource{noSourceRoot / "Results",
                                      noSourceRoot / "Data"};
    noSource.resize(1024, 768);
    noSource.show();
    QVERIFY(QTest::qWaitForWindowExposed(&noSource));
    auto *noSourceContext =
        noSource.findChild<QLabel *>("replayModeSourceLabel");
    auto *noSourceSchema = noSource.findChild<QComboBox *>("replaySchemaCombo");
    QVERIFY(noSourceContext != nullptr);
    QVERIFY(noSourceSchema != nullptr);
    QCOMPARE(noSourceContext->text(),
             QString{"Bar-only preview · No supported CSV sources"});
    QVERIFY(noSourceSchema->currentText().isEmpty());
  }
}

void ReplayTabTest::chartAndDetailsCanBeResizedByDraggingSplitterHandle() {
  bte::frontend::ReplayTab tab;
  auto *splitter = tab.findChild<QSplitter *>("replayChartTradeSplitter");
  QVERIFY(splitter != nullptr);
  tab.resize(1440, 1300);
  tab.show();
  QVERIFY(QTest::qWaitForWindowExposed(&tab));
  QCoreApplication::processEvents();

  auto *handle = splitter->handle(1);
  QVERIFY(handle != nullptr);
  const auto sizesBefore = splitter->sizes();
  QCOMPARE(sizesBefore.size(), 2);
  const auto dragHandle = [handle](const QPoint &start, const QPoint &end) {
    const auto globalStart = handle->mapToGlobal(start);
    const auto globalEnd = handle->mapToGlobal(end);
    QMouseEvent press{QEvent::MouseButtonPress, QPointF{start},
                      QPointF{globalStart},     Qt::LeftButton,
                      Qt::LeftButton,           Qt::NoModifier};
    QApplication::sendEvent(handle, &press);
    QMouseEvent move{QEvent::MouseMove, QPointF{end},   QPointF{globalEnd},
                     Qt::NoButton,      Qt::LeftButton, Qt::NoModifier};
    QApplication::sendEvent(handle, &move);
    QMouseEvent release{QEvent::MouseButtonRelease,
                        QPointF{end},
                        QPointF{globalEnd},
                        Qt::LeftButton,
                        Qt::NoButton,
                        Qt::NoModifier};
    QApplication::sendEvent(handle, &release);
  };

  auto start = handle->rect().center();
  dragHandle(start, start + QPoint{0, 80});
  QCoreApplication::processEvents();

  const auto sizesAfterExpand = splitter->sizes();
  QVERIFY2(sizesAfterExpand[0] > sizesBefore[0],
           qPrintable(QString{"chart size did not grow: before=%1 after=%2"}
                          .arg(sizesBefore[0])
                          .arg(sizesAfterExpand[0])));
  QVERIFY(sizesAfterExpand[1] < sizesBefore[1]);

  start = handle->rect().center();
  dragHandle(start, start + QPoint{0, -40});
  QCoreApplication::processEvents();
  const auto sizesAfterShrink = splitter->sizes();
  QVERIFY(sizesAfterShrink[0] < sizesAfterExpand[0]);
  QVERIFY(sizesAfterShrink[1] > sizesAfterExpand[1]);
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
