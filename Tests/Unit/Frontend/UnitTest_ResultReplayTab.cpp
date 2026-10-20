#include "Bte/Frontend/ReplayTab.h"

#include "Bte/Bindings/BacktestSessionVm.h"
#include "Bte/Core/Time.h"
#include "Bte/Data/ReleaseSnapshot.h"
#include "Bte/Frontend/QtChartsCandlestickView.h"
#include "Bte/Results/ResultStore.h"
#include "Bte/Strategy/SelectableStrategy.h"

#include "ResultReplayRequestsTestHooks.h"
#include "WaitUntil.h"

#include <QAccessible>
#include <QByteArray>
#include <QChart>
#include <QChartView>
#include <QComboBox>
#include <QDate>
#include <QDateTime>
#include <QElapsedTimer>
#include <QGraphicsItem>
#include <QGraphicsScene>
#include <QLabel>
#include <QLineF>
#include <QMetaObject>
#include <QPointer>
#include <QPushButton>
#include <QScatterSeries>
#include <QSignalSpy>
#include <QSlider>
#include <QTableWidget>
#include <QTest>
#include <QTime>
#include <QTimeZone>
#include <QTimer>
#include <QToolButton>

#include <array>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>

namespace {

class ScopedEnvironmentRestore final {
public:
  explicit ScopedEnvironmentRestore(QByteArray name)
      : name_{std::move(name)},
        previous_{qEnvironmentVariableIsSet(name_.constData())
                      ? std::optional<QByteArray>{qgetenv(name_.constData())}
                      : std::nullopt} {}

  ~ScopedEnvironmentRestore() {
    if (previous_.has_value()) {
      qputenv(name_.constData(), *previous_);
    } else {
      qunsetenv(name_.constData());
    }
  }

  ScopedEnvironmentRestore(const ScopedEnvironmentRestore &) = delete;
  ScopedEnvironmentRestore &
  operator=(const ScopedEnvironmentRestore &) = delete;
  ScopedEnvironmentRestore(ScopedEnvironmentRestore &&) = delete;
  ScopedEnvironmentRestore &operator=(ScopedEnvironmentRestore &&) = delete;

private:
  QByteArray name_;
  std::optional<QByteArray> previous_;
};

class ResultReplayTabTest final : public QObject {
  Q_OBJECT

private slots:
  void initTestCase();
  void cleanupTestCase();
  void rapidSwitchRejectsStaleCompletionAndPresentsExactFirstFrame();
  void resultControlsAndPersistedFillDetailsAreAccessible();
  void dailyMarkersAnchorToTheirCandleAndKeepActualFillTime();
  void externalOpenDefersCatalogRefreshUntilOpenCompletes();
  void resultKeyboardShortcutsRestartAndSpeedMapping();
  void selectionWhilePlayingStopsPriorPlayback();
  void timeframeChangeDuringOpenReplacesThePendingRequest();
  void timeframeSwitchKeepsTheProgressivePrefixAndCurrentDate();
  void resultControlsCoverEmptyTerminalUnavailableAndNavigationBoundaries();
  void catalogEmptyAndFailureStatesArePresented();
  void configuredDefaultStoreUsesEnvironmentOverride();
  void tabOwnsRequestWorkerAndDestroysItWithPendingOpen();
  void maximumPlaybackBatchesFramesWithoutTruncatingReachedCandles();
  void multiDaySelectableFillsAppearOnlyWhenPlaybackReachesTheirBars();
  void longRangeSeekPresentsEveryReachedCandleAndFill();
  void maximumPlaybackStopsAtAndAfterTheFinalFrame();
  void trashedActiveResultStopsReplayAndClearsSelection();
  void renamedResultRefreshesSelectorWithoutChangingOpenId();
  void thirtyTwoCharacterNamedOptionOpensItsStoredResultId();
  void preservedLegacyArtifactsWarnBesideUsableSavedResults();

private:
  std::filesystem::path root_;
  std::string snapshotId_;
  std::string firstResultId_;
  std::string secondResultId_;
  std::string emptyResultId_;
  std::string terminalResultId_;
  std::string largeResultId_;
  std::string multiDayMultiFillResultId_;
  std::string unavailableResultId_ = std::string(32, 'd');
};

void ResultReplayTabTest::
    preservedLegacyArtifactsWarnBesideUsableSavedResults() {
  const auto legacyId = std::string(32, 'b');
  const auto legacyPath =
      root_ / "Store" / "Staging" / (legacyId + ".bteresult");
  const std::string legacyBytes =
      "legacy staging must not be removed by Replay";
  {
    std::ofstream legacy{legacyPath};
    legacy << legacyBytes;
  }
  bte::frontend::ReplayTab tab{root_ / "Store", root_ / "Data"};
  auto *notices = tab.findChild<QLabel *>("replayRecoveryNoticesLabel");
  auto *selector = tab.findChild<QComboBox *>("replayResultCombo");
  auto *status = tab.findChild<QLabel *>("replayResultStatusLabel");
  auto *toggle = tab.findChild<QToolButton *>("replaySetupToggleButton");
  QVERIFY(notices != nullptr);
  QVERIFY(selector != nullptr);
  QVERIFY(status != nullptr);
  QVERIFY(toggle != nullptr);
  QVERIFY(!toggle->isChecked());
  QVERIFY(bte::test::waitUntil([&] {
    return notices->text().contains(QString::fromStdString(legacyId)) &&
           selector->findData(QString::fromStdString(firstResultId_)) >= 0;
  }));
  QVERIFY(!notices->isHidden());
  QVERIFY(notices->text().contains("preserved", Qt::CaseInsensitive));
  tab.openResult(QString::fromStdString(firstResultId_));
  QVERIFY(bte::test::waitUntil(
      [&] { return status->text().startsWith("Result "); }));
  QVERIFY(!notices->isHidden());
  QVERIFY(std::filesystem::is_regular_file(legacyPath));
  std::ifstream preserved{legacyPath};
  const std::string preservedBytes{std::istreambuf_iterator<char>{preserved},
                                   std::istreambuf_iterator<char>{}};
  QCOMPARE(preservedBytes, legacyBytes);
}

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
  std::ofstream largeSource{root_ / "Source" / "BIG.csv"};
  largeSource << "symbol,ts,open,high,low,close,volume,schemaName\n";
  auto timestamp = QDateTime{QDate{2024, 1, 1}, QTime{0, 0}, QTimeZone::UTC};
  for (auto index = 0; index < 520; ++index) {
    const auto open = 100 + (index % 7);
    largeSource << "BIG,"
                << timestamp.toString("yyyy-MM-dd HH:mm:ss+00:00").toStdString()
                << ',' << open << ',' << open + 2 << ',' << open - 1 << ','
                << open + 1 << ',' << 1'000 + index << ",ohlcv-1h\n";
    timestamp = timestamp.addSecs(3'600);
  }
  largeSource.close();
  auto built = bte::data::buildReleaseSnapshot({
      .sourceDirectory = root_ / "Source",
      .storeDirectory = root_ / "Data",
      .symbols = {"BIG", "SYN"},
      .rowsPerSegment = 100,
      .calendarHash = std::string(64, 'a'),
      .splitManifestHash = std::string(64, 'b'),
  });
  QVERIFY2(built.ok(), built.error().message.c_str());
  snapshotId_ = built.value().snapshotId;
  const auto run = [this, &built](const std::int64_t quantity) {
    return bte::bindings::runPersistedBacktestConfiguration(
        {.symbol = "SYN",
         .schema = "ohlcv-1h",
         .startDate = QDate{2024, 1, 1},
         .endDate = QDate{2024, 1, 2},
         .initialCapital = 2'000,
         .quantityShares = quantity,
         .selectableStrategy = {}},
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
  auto large = bte::bindings::runPersistedBacktestConfiguration(
      {.symbol = "BIG",
       .schema = "ohlcv-1h",
       .startDate = QDate{2024, 1, 1},
       .endDate = QDate{2024, 1, 22},
       .initialCapital = 1'000'000,
       .quantityShares = 1,
       .selectableStrategy = {}},
      {.resultStore = root_ / "Store",
       .dataStore = root_ / "Data",
       .snapshotId = built.value().snapshotId,
       .strategyHash = std::string(64, 'c')});
  QVERIFY2(large.ok(), large.error().message.c_str());
  largeResultId_ = large.value().resultId;
  const auto multiFillPlan = bte::strategy::SelectableStrategyPlan{
      .buy = {.logic = bte::strategy::ConditionLogic::all,
              .conditions = {{.source =
                                  bte::strategy::ConditionSource::barField,
                              .comparison = bte::strategy::Comparison::lessThan,
                              .threshold = 103.0}}},
      .sell = {
          .logic = bte::strategy::ConditionLogic::all,
          .conditions = {{.source = bte::strategy::ConditionSource::barField,
                          .comparison = bte::strategy::Comparison::greaterThan,
                          .threshold = 104.0}}}};
  auto multiDayMultiFill = bte::bindings::runPersistedBacktestConfiguration(
      {.symbol = "BIG",
       .schema = "ohlcv-1h",
       .startDate = QDate{2024, 1, 1},
       .endDate = QDate{2024, 1, 22},
       .initialCapital = 1'000'000,
       .quantityShares = 1,
       .selectableStrategy = multiFillPlan},
      {.resultStore = root_ / "Store",
       .dataStore = root_ / "Data",
       .snapshotId = built.value().snapshotId,
       .strategyHash = std::string(64, 'c')});
  QVERIFY2(multiDayMultiFill.ok(), multiDayMultiFill.error().message.c_str());
  multiDayMultiFillResultId_ = multiDayMultiFill.value().resultId;

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
  for (const auto &resultId : {firstResultId_, secondResultId_, largeResultId_,
                               multiDayMultiFillResultId_}) {
    const auto published = store.value()->publishDraft(resultId);
    QVERIFY2(published.ok(), published.error().message.c_str());
  }
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
                         .pnlMicrodollars = 0,
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

void ResultReplayTabTest::externalOpenDefersCatalogRefreshUntilOpenCompletes() {
  const auto resultRoot = root_ / "DeferredStore";
  bte::frontend::ReplayTab tab{resultRoot, root_ / "Data"};
  auto *selector = tab.findChild<QComboBox *>("replayResultCombo");
  auto *status = tab.findChild<QLabel *>("replayResultStatusLabel");
  QVERIFY(selector != nullptr);
  QVERIFY(status != nullptr);
  QVERIFY(bte::test::waitUntil(
      [&] { return status->text() == "No saved Backtest results"; }));

  const auto run = bte::bindings::runPersistedBacktestConfiguration(
      {.symbol = "SYN",
       .schema = "ohlcv-1h",
       .startDate = QDate{2024, 1, 1},
       .endDate = QDate{2024, 1, 2},
       .initialCapital = 2'000,
       .quantityShares = 1,
       .selectableStrategy = {}},
      {.resultStore = resultRoot,
       .dataStore = root_ / "Data",
       .snapshotId = snapshotId_,
       .strategyHash = std::string(64, 'c')});
  QVERIFY2(run.ok(), run.error().message.c_str());
  auto store = bte::results::ResultStore::open(resultRoot, root_ / "Data");
  QVERIFY2(store.ok(), store.error().message.c_str());
  const auto published = store.value()->publishDraft(run.value().resultId);
  QVERIFY2(published.ok(), published.error().message.c_str());

  const auto catalogRequestsBeforeOpen =
      bte::bindings::testing::resultCatalogRequestCount();
  bte::bindings::testing::blockNextResultOpenUntilCancelled();
  tab.openResult(QString::fromStdString(run.value().resultId));
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds{5};
  while (!bte::bindings::testing::resultOpenWorkerStarted() &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::yield();
  }
  QVERIFY(bte::bindings::testing::resultOpenWorkerStarted());
  QCOMPARE(bte::bindings::testing::resultCatalogRequestCount(),
           catalogRequestsBeforeOpen);
  QCOMPARE(selector->findData(QString::fromStdString(run.value().resultId)),
           -1);

  bte::bindings::testing::clearResultReplayRequestHooks();
  tab.openResult(QString::fromStdString(run.value().resultId));
  QVERIFY(bte::test::waitUntil([&] {
    return status->text().startsWith("Result ") &&
           selector->findData(QString::fromStdString(run.value().resultId)) >=
               0;
  }));
}

void ResultReplayTabTest::trashedActiveResultStopsReplayAndClearsSelection() {
  bte::frontend::ReplayTab tab{root_ / "Store", root_ / "Data"};
  auto *selector = tab.findChild<QComboBox *>("replayResultCombo");
  auto *status = tab.findChild<QLabel *>("replayResultStatusLabel");
  QVERIFY(selector != nullptr);
  QVERIFY(status != nullptr);
  const auto resultId = QString::fromStdString(firstResultId_);
  QVERIFY(bte::test::waitUntil(
      [&] { return (selector->findText(resultId) >= 0); }));
  tab.openResult(resultId);
  QVERIFY(bte::test::waitUntil(
      [&] { return (status->text().startsWith("Result ")); }));
  tab.closeResultIfId("other-result");
  QCOMPARE(selector->currentText(), resultId);
  tab.closeResultIfId(resultId);
  QCOMPARE(status->text(), QString{"Select a saved result"});
  QCOMPARE(selector->currentIndex(), -1);
  QVERIFY(selector->findText(resultId) < 0);
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
  auto *chart = tab.findChild<bte::frontend::QtChartsCandlestickView *>(
      "replayCandlestickChartView");
  auto *cash = tab.findChild<QLabel *>("replayCashLabel");
  auto *trades = tab.findChild<QTableWidget *>("replayTradeLogTable");
  QVERIFY(status != nullptr);
  QVERIFY(play != nullptr);
  QVERIFY(timer != nullptr);
  QVERIFY(chart != nullptr);
  QVERIFY(cash != nullptr);
  QVERIFY(trades != nullptr);
  tab.openResult(QString::fromStdString(firstResultId_));
  QVERIFY(bte::test::waitUntil(
      [&] { return status->text().startsWith("Result "); }));

  QTest::mouseClick(play, Qt::LeftButton);
  QVERIFY(timer->isActive());
  QVERIFY(bte::test::waitUntil([&] {
    return tab.findChild<QComboBox *>("replayResultCombo")
               ->findText(QString::fromStdString(unavailableResultId_)) >= 0;
  }));
  tab.openResult(QString::fromStdString(unavailableResultId_));

  QVERIFY(!timer->isActive());
  QCOMPARE(play->text(), QString{"Play"});
  QCOMPARE(chart->candleCount(), 0U);
  QCOMPARE(trades->rowCount(), 0);
  QCOMPARE(cash->text(), QString{"Cash: --"});

  tab.openResult(QString::fromStdString(secondResultId_));

  QCOMPARE(chart->candleCount(), 0U);
  QCOMPARE(trades->rowCount(), 0);
  QCOMPARE(cash->text(), QString{"Cash: --"});
  QVERIFY(bte::test::waitUntil([&] {
    return status->text().startsWith(
        QString{"Result %1"}.arg(QString::fromStdString(secondResultId_)));
  }));
}

void ResultReplayTabTest::timeframeChangeDuringOpenReplacesThePendingRequest() {
  bte::frontend::ReplayTab tab{root_ / "Store", root_ / "Data"};
  auto *status = tab.findChild<QLabel *>("replayResultStatusLabel");
  auto *timeframe = tab.findChild<QComboBox *>("replayResultTimeframeCombo");
  auto *seek = tab.findChild<QSlider *>("replaySeekSlider");
  QVERIFY(status != nullptr);
  QVERIFY(timeframe != nullptr);
  QVERIFY(seek != nullptr);

  bte::bindings::testing::blockNextResultOpenUntilCancelled();
  tab.openResult(QString::fromStdString(firstResultId_));
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds{5};
  while (!bte::bindings::testing::resultOpenWorkerStarted() &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::yield();
  }
  QVERIFY(bte::bindings::testing::resultOpenWorkerStarted());

  timeframe->setCurrentIndex(1);
  QVERIFY(bte::test::waitUntil([&] {
    return status->text().startsWith("Result ") && seek->maximum() == 1;
  }));
  bte::bindings::testing::clearResultReplayRequestHooks();
}

void ResultReplayTabTest::
    timeframeSwitchKeepsTheProgressivePrefixAndCurrentDate() {
  bte::frontend::ReplayTab tab{root_ / "Store", root_ / "Data"};
  auto *status = tab.findChild<QLabel *>("replayResultStatusLabel");
  auto *timeframe = tab.findChild<QComboBox *>("replayResultTimeframeCombo");
  auto *seek = tab.findChild<QSlider *>("replaySeekSlider");
  auto *chart = tab.findChild<bte::frontend::QtChartsCandlestickView *>(
      "replayCandlestickChartView");
  QVERIFY(status != nullptr);
  QVERIFY(timeframe != nullptr);
  QVERIFY(seek != nullptr);
  QVERIFY(chart != nullptr);

  tab.openResult(QString::fromStdString(largeResultId_));
  QVERIFY(bte::test::waitUntil([&] {
    return status->text().startsWith("Result ") && seek->maximum() == 519;
  }));
  seek->setValue(48);
  QCOMPARE(chart->candleCount(), 49U);
  QCOMPARE(chart->markerCount(), 1U);

  timeframe->setCurrentIndex(1);
  QVERIFY(bte::test::waitUntil([&] {
    return status->text().startsWith("Result ") && seek->maximum() == 21;
  }));
  QCOMPARE(seek->value(), 2);
  QCOMPARE(chart->candleCount(), 3U);
  QCOMPARE(chart->markerCount(), 1U);

  timeframe->setCurrentIndex(0);
  QVERIFY(bte::test::waitUntil([&] {
    return status->text().startsWith("Result ") && seek->maximum() == 519;
  }));
  QCOMPARE(seek->value(), 48);
  QCOMPARE(chart->candleCount(), 49U);
  QCOMPARE(chart->markerCount(), 1U);
}

void ResultReplayTabTest::resultControlsAndPersistedFillDetailsAreAccessible() {
  bte::frontend::ReplayTab tab{root_ / "Store", root_ / "Data"};
  const auto widgets = tab.findChildren<QWidget *>();
  for (const auto *widget : widgets) {
    if (widget->objectName().startsWith("replay")) {
      QVERIFY2(!widget->accessibleName().isEmpty(),
               qPrintable(widget->objectName()));
    }
  }

  const auto verifyAccessibleLabels = [&tab]() {
    for (const auto *name :
         {"replayResultStatusLabel", "replayCashLabel", "replayPositionLabel",
          "replayMarketValueLabel", "replayEquityLabel", "replayPnlLabel",
          "replayLastPriceLabel", "replayBarIndexLabel"}) {
      auto *label = tab.findChild<QLabel *>(name);
      QVERIFY(label != nullptr);
      auto *accessible = QAccessible::queryAccessibleInterface(label);
      QVERIFY(accessible != nullptr);
      QCOMPARE(accessible->text(QAccessible::Name), label->text());
      QCOMPARE(label->accessibleName(), label->text());
    }
  };
  verifyAccessibleLabels();

  auto *status = tab.findChild<QLabel *>("replayResultStatusLabel");
  auto *step = tab.findChild<QToolButton *>("replayStepForwardButton");
  auto *seek = tab.findChild<QSlider *>("replaySeekSlider");
  auto *trades = tab.findChild<QTableWidget *>("replayTradeLogTable");
  QVERIFY(status != nullptr);
  QVERIFY(step != nullptr);
  QVERIFY(trades != nullptr);
  QVERIFY(seek != nullptr);
  const auto resultId = QString::fromStdString(secondResultId_);
  tab.openResult(resultId);
  verifyAccessibleLabels();
  QVERIFY(bte::test::waitUntil(
      [&] { return status->text().startsWith("Result "); }));
  verifyAccessibleLabels();
  QTest::mouseClick(step, Qt::LeftButton);
  verifyAccessibleLabels();
  QCOMPARE(trades->rowCount(), 1);
  const auto fillDescription =
      trades->item(0, 0)->data(Qt::AccessibleDescriptionRole).toString();
  QVERIFY(fillDescription.contains("Buy fill"));
  QVERIFY(fillDescription.contains("5 shares"));
  QVERIFY(fillDescription.contains(trades->item(0, 2)->text()));
  seek->setValue(seek->maximum());
  verifyAccessibleLabels();
  seek->setValue(seek->minimum());
  verifyAccessibleLabels();
  tab.closeResultIfId(resultId);
  QCOMPARE(status->text(), QString{"Select a saved result"});
  QCOMPARE(tab.findChild<QLabel *>("replayCashLabel")->text(),
           QString{"Cash: --"});
  verifyAccessibleLabels();
  tab.openResult("missing-accessibility-result");
  verifyAccessibleLabels();
  QVERIFY(bte::test::waitUntil(
      [&] { return status->text().startsWith("Cannot open result"); }));
  verifyAccessibleLabels();
}

void ResultReplayTabTest::
    dailyMarkersAnchorToTheirCandleAndKeepActualFillTime() {
  bte::frontend::ReplayTab tab{root_ / "Store", root_ / "Data"};
  tab.resize(1440, 900);
  tab.show();
  QVERIFY(QTest::qWaitForWindowExposed(&tab));
  auto *timeframe = tab.findChild<QComboBox *>("replayResultTimeframeCombo");
  auto *status = tab.findChild<QLabel *>("replayResultStatusLabel");
  auto *chart = tab.findChild<bte::frontend::QtChartsCandlestickView *>(
      "replayCandlestickChartView");
  auto *trades = tab.findChild<QTableWidget *>("replayTradeLogTable");
  QVERIFY(timeframe != nullptr);
  QVERIFY(status != nullptr);
  QVERIFY(chart != nullptr);
  QVERIFY(trades != nullptr);
  timeframe->setCurrentIndex(1);
  tab.openResult(QString::fromStdString(largeResultId_));
  QVERIFY(bte::test::waitUntil(
      [&] { return status->text().startsWith("Result "); }));
  QCOMPARE(chart->candleCount(), 1U);
  QCOMPARE(chart->markerCount(), 1U);
  QCOMPARE(trades->rowCount(), 1);
  QCOMPARE(trades->item(0, 0)->text(), QString("2024-01-01T01:00:00Z"));
  const auto *viewport = chart->findChild<QChartView *>("replayChartView");
  QVERIFY(viewport != nullptr);
  auto *buy =
      viewport->chart()->findChild<QScatterSeries *>("replayBuyMarkers");
  QVERIFY(buy != nullptr);
  QCOMPARE(buy->points().front().x(), 0.0);
  auto glyphCount = 0;
  auto leaderCount = 0;
  const auto anchor = viewport->chart()->mapToScene(
      viewport->chart()->mapToPosition(QPointF(0.0, 99.0), buy));
  QPointF glyphCenter;
  for (const auto *item : viewport->scene()->items()) {
    if (item->data(0).toString() == "replayTradeGlyph") {
      ++glyphCount;
      QVERIFY(item->toolTip().contains("2024-01-01T01:00:00Z"));
      QVERIFY(item->isVisible());
      glyphCenter = item->sceneBoundingRect().center();
    } else if (item->data(0).toString() == "replayTradeLeader") {
      const auto *leader = qgraphicsitem_cast<const QGraphicsLineItem *>(item);
      QVERIFY(leader != nullptr);
      ++leaderCount;
      const auto leaderAnchor =
          viewport->chart()->mapToScene(leader->line().p1());
      const QLineF anchorDelta{leaderAnchor, anchor};
      QVERIFY(anchorDelta.length() < 0.01);
    }
  }
  QCOMPARE(glyphCount, 1);
  QCOMPARE(leaderCount, 1);
  QVERIFY(std::abs(glyphCenter.x() - anchor.x()) <= 1.0);
  QVERIFY(glyphCenter.y() >= anchor.y() + 8.0);
}

void ResultReplayTabTest::resultKeyboardShortcutsRestartAndSpeedMapping() {
  bte::frontend::ReplayTab tab{root_ / "Store", root_ / "Data"};
  auto *status = tab.findChild<QLabel *>("replayResultStatusLabel");
  auto *seek = tab.findChild<QSlider *>("replaySeekSlider");
  auto *speed = tab.findChild<QComboBox *>("replaySpeedCombo");
  auto *timer = tab.findChild<QTimer *>("replayPlaybackTimer");
  auto *play = tab.findChild<QToolButton *>("replayPlayPauseButton");
  QVERIFY(status != nullptr);
  QVERIFY(seek != nullptr);
  QVERIFY(speed != nullptr);
  QVERIFY(timer != nullptr);
  QVERIFY(play != nullptr);
  tab.show();
  tab.activateWindow();
  tab.setFocus();
  tab.openResult(QString::fromStdString(firstResultId_));
  QVERIFY(bte::test::waitUntil([&] {
    return status->text().startsWith("Result ") && seek->maximum() == 2;
  }));

  QTest::keyClick(&tab, Qt::Key_Right);
  QCOMPARE(seek->value(), 1);
  QTest::keyClick(&tab, Qt::Key_Left);
  QCOMPARE(seek->value(), 0);

  speed->setCurrentText("5x");
  QCOMPARE(timer->interval(), 200);
  speed->setCurrentText("10x");
  QCOMPARE(timer->interval(), 100);
  speed->setCurrentText("max");
  QCOMPARE(timer->interval(), 0);
  speed->setCurrentText("1x");
  QCOMPARE(timer->interval(), 1000);

  QTest::keyClick(&tab, Qt::Key_Space);
  QCOMPARE(seek->value(), 1);
  QVERIFY(timer->isActive());
  QTest::keyClick(&tab, Qt::Key_Space);
  QVERIFY(!timer->isActive());
  seek->setValue(2);
  QTest::keyClick(&tab, Qt::Key_Space);
  QCOMPARE(seek->value(), 1);
  QVERIFY(timer->isActive());
  QTest::keyClick(&tab, Qt::Key_Space);
  QCOMPARE(play->text(), QString{"Play"});
  QVERIFY(!timer->isActive());
}

void ResultReplayTabTest::
    resultControlsCoverEmptyTerminalUnavailableAndNavigationBoundaries() {
  bte::frontend::ReplayTab tab{root_ / "Store", root_ / "Data"};
  auto *selector = tab.findChild<QComboBox *>("replayResultCombo");
  auto *timeframe = tab.findChild<QComboBox *>("replayResultTimeframeCombo");
  auto *open = tab.findChild<QPushButton *>("replayOpenResultButton");
  auto *status = tab.findChild<QLabel *>("replayResultStatusLabel");
  auto *partial = tab.findChild<QLabel *>("replayPartialLabel");
  auto *seek = tab.findChild<QSlider *>("replaySeekSlider");
  auto *back = tab.findChild<QToolButton *>("replayStepBackButton");
  auto *step = tab.findChild<QToolButton *>("replayStepForwardButton");
  auto *play = tab.findChild<QToolButton *>("replayPlayPauseButton");
  QVERIFY(selector != nullptr);
  QVERIFY(timeframe != nullptr);
  QVERIFY(open != nullptr);
  QVERIFY(status != nullptr);
  QVERIFY(partial != nullptr);
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
  QCOMPARE(partial->text(), QString{"Partial UTC day"});
  QVERIFY(partial->isHidden());
  const auto secondIndex =
      selector->findData(QString::fromStdString(secondResultId_));
  QVERIFY(secondIndex >= 0);
  selector->setCurrentIndex(secondIndex);
  QVERIFY(QMetaObject::invokeMethod(selector, "activated",
                                    Q_ARG(int, selector->currentIndex())));
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
  const ScopedEnvironmentRestore restore{"BTE_RESULT_STORE"};
  QVERIFY(qputenv("BTE_RESULT_STORE", configured.string().c_str()));
  {
    bte::frontend::ReplayTab tab;
    auto *status = tab.findChild<QLabel *>("replayResultStatusLabel");
    QVERIFY(status != nullptr);
    QVERIFY(bte::test::waitUntil(
        [&] { return status->text() == "No saved Backtest results"; }));
  }
}

void ResultReplayTabTest::tabOwnsRequestWorkerAndDestroysItWithPendingOpen() {
  auto tab = std::make_unique<bte::frontend::ReplayTab>(root_ / "Store",
                                                        root_ / "Data");
  auto *requestWorker = tab->findChild<QObject *>("resultReplayOpenRequests");
  QVERIFY(requestWorker != nullptr);
  const QPointer<QObject> requestWorkerGuard{requestWorker};

  tab->openResult(QString::fromStdString(firstResultId_));
  tab->openResult(QString::fromStdString(secondResultId_));
  tab.reset();

  QVERIFY(requestWorkerGuard.isNull());
}

void ResultReplayTabTest::
    maximumPlaybackBatchesFramesWithoutTruncatingReachedCandles() {
  bte::frontend::ReplayTab tab{root_ / "Store", root_ / "Data"};
  auto *status = tab.findChild<QLabel *>("replayResultStatusLabel");
  auto *seek = tab.findChild<QSlider *>("replaySeekSlider");
  auto *speed = tab.findChild<QComboBox *>("replaySpeedCombo");
  auto *play = tab.findChild<QToolButton *>("replayPlayPauseButton");
  auto *timer = tab.findChild<QTimer *>("replayPlaybackTimer");
  auto *chart = tab.findChild<bte::frontend::QtChartsCandlestickView *>(
      "replayCandlestickChartView");
  QVERIFY(status != nullptr);
  QVERIFY(seek != nullptr);
  QVERIFY(speed != nullptr);
  QVERIFY(play != nullptr);
  QVERIFY(timer != nullptr);
  QVERIFY(chart != nullptr);

  tab.openResult(QString::fromStdString(largeResultId_));
  QVERIFY(bte::test::waitUntil([&] {
    return status->text().startsWith("Result ") && seek->maximum() == 519;
  }));

  QCOMPARE(chart->candleCount(), 1U);
  QCOMPARE(chart->volumePointCount(), 1U);
  QCOMPARE(chart->markerCount(), 0U);
  seek->setValue(1);
  QCOMPARE(chart->candleCount(), 2U);
  QCOMPARE(chart->markerCount(), 1U);
  seek->setValue(0);
  QCOMPARE(chart->candleCount(), 1U);
  QCOMPARE(chart->markerCount(), 0U);

  seek->setValue(499);
  QCOMPARE(chart->candleCount(), 500U);
  QCOMPARE(chart->volumePointCount(), 500U);
  seek->setValue(500);
  QCOMPARE(chart->candleCount(), 501U);
  QCOMPARE(chart->volumePointCount(), 501U);
  seek->setValue(519);
  QCOMPARE(chart->candleCount(), 520U);
  QCOMPARE(chart->volumePointCount(), 520U);

  seek->setValue(0);
  speed->setCurrentText("max");
  QTest::mouseClick(play, Qt::LeftButton);
  timer->stop();
  const auto beforeBatch = seek->value();
  QVERIFY(QMetaObject::invokeMethod(timer, "timeout", Qt::DirectConnection));

  QCOMPARE(seek->value(), beforeBatch + 32);
  QCOMPARE(chart->candleCount(), 34U);
  QCOMPARE(chart->candleCount(), chart->volumePointCount());
  bool eventLoopResponsive = false;
  QTimer::singleShot(0, &tab, [&] { eventLoopResponsive = true; });
  QCoreApplication::processEvents();
  QVERIFY(eventLoopResponsive);
}

void ResultReplayTabTest::
    multiDaySelectableFillsAppearOnlyWhenPlaybackReachesTheirBars() {
  bte::frontend::ReplayTab tab{root_ / "Store", root_ / "Data"};
  auto *status = tab.findChild<QLabel *>("replayResultStatusLabel");
  auto *seek = tab.findChild<QSlider *>("replaySeekSlider");
  auto *speed = tab.findChild<QComboBox *>("replaySpeedCombo");
  auto *play = tab.findChild<QToolButton *>("replayPlayPauseButton");
  auto *timer = tab.findChild<QTimer *>("replayPlaybackTimer");
  auto *chart = tab.findChild<bte::frontend::QtChartsCandlestickView *>(
      "replayCandlestickChartView");
  auto *trades = tab.findChild<QTableWidget *>("replayTradeLogTable");
  QVERIFY(status != nullptr);
  QVERIFY(seek != nullptr);
  QVERIFY(speed != nullptr);
  QVERIFY(play != nullptr);
  QVERIFY(timer != nullptr);
  QVERIFY(chart != nullptr);
  QVERIFY(trades != nullptr);

  tab.openResult(QString::fromStdString(multiDayMultiFillResultId_));
  QVERIFY(bte::test::waitUntil([&] {
    return status->text().startsWith("Result ") && seek->maximum() == 519;
  }));
  QCOMPARE(chart->candleCount(), 1U);
  QCOMPARE(chart->markerCount(), 0U);
  QCOMPARE(trades->rowCount(), 0);

  seek->setValue(1);
  QCOMPARE(chart->candleCount(), 2U);
  QCOMPARE(chart->markerCount(), 1U);
  QCOMPARE(trades->rowCount(), 1);
  QCOMPARE(trades->item(0, 0)->text(), QString{"2024-01-01T01:00:00Z"});

  QSignalSpy insertedRows{trades->model(), &QAbstractItemModel::rowsInserted};
  QSignalSpy changedCells{trades->model(), &QAbstractItemModel::dataChanged};
  seek->setValue(12);
  QCOMPARE(insertedRows.size(), 1);
  QCOMPARE(changedCells.size(), 1);
  QCOMPARE(insertedRows.front().at(1).toInt(), 1);
  QCOMPARE(insertedRows.front().at(2).toInt(), 3);
  const auto changedFirst =
      qvariant_cast<QModelIndex>(changedCells.front().at(0));
  const auto changedLast =
      qvariant_cast<QModelIndex>(changedCells.front().at(1));
  QCOMPARE(changedFirst, trades->model()->index(1, 0));
  QCOMPARE(changedLast, trades->model()->index(3, 5));
  QCOMPARE(chart->candleCount(), 13U);
  QCOMPARE(chart->markerCount(), 4U);
  QCOMPARE(trades->rowCount(), 4);
  QCOMPARE(trades->item(3, 0)->text(), QString{"2024-01-01T12:00:00Z"});
  QVERIFY(!trades->item(3, 0)
               ->data(Qt::AccessibleDescriptionRole)
               .toString()
               .isEmpty());
  const auto *viewport = chart->findChild<QChartView *>("replayChartView");
  QVERIFY(viewport != nullptr);
  const auto *buy =
      viewport->chart()->findChild<QScatterSeries *>("replayBuyMarkers");
  const auto *sell =
      viewport->chart()->findChild<QScatterSeries *>("replaySellMarkers");
  QVERIFY(buy != nullptr);
  QVERIFY(sell != nullptr);
  QCOMPARE(buy->points().size(), 2);
  QCOMPARE(sell->points().size(), 2);
  QCOMPARE(buy->points().front().x(), 3'600'000.0);
  QCOMPARE(sell->points().front().x(), 18'000'000.0);

  seek->setValue(0);
  QCOMPARE(chart->candleCount(), 1U);
  QCOMPARE(chart->markerCount(), 0U);
  QCOMPARE(trades->rowCount(), 0);

  speed->setCurrentText("max");
  QTest::mouseClick(play, Qt::LeftButton);
  timer->stop();
  QVERIFY(QMetaObject::invokeMethod(timer, "timeout", Qt::DirectConnection));
  QCOMPARE(seek->value(), 33);
  QCOMPARE(chart->candleCount(), 34U);
  QCOMPARE(chart->markerCount(), 10U);
  QCOMPARE(trades->rowCount(), 10);

  auto *timeframe = tab.findChild<QComboBox *>("replayResultTimeframeCombo");
  QVERIFY(timeframe != nullptr);
  timeframe->setCurrentIndex(1);
  QVERIFY(bte::test::waitUntil([&] {
    return status->text().startsWith("Result ") && seek->maximum() == 21;
  }));
  seek->setValue(0);
  QCOMPARE(chart->candleCount(), 1U);
  QCOMPARE(chart->markerCount(), 7U);
  QCOMPARE(trades->rowCount(), 7);
  const std::array expectedTimes{
      QString{"2024-01-01T01:00:00Z"}, QString{"2024-01-01T05:00:00Z"},
      QString{"2024-01-01T08:00:00Z"}, QString{"2024-01-01T12:00:00Z"},
      QString{"2024-01-01T15:00:00Z"}, QString{"2024-01-01T19:00:00Z"},
      QString{"2024-01-01T22:00:00Z"}};
  for (auto row = 0; row < 7; ++row) {
    QCOMPARE(trades->item(row, 0)->text(),
             expectedTimes.at(static_cast<std::size_t>(row)));
    QCOMPARE(trades->item(row, 1)->text(),
             row % 2 == 0 ? QString{"Buy"} : QString{"Sell"});
    QCOMPARE(trades->item(row, 3)->text(), QString{"1"});
  }
  const auto *dailyViewport = chart->findChild<QChartView *>("replayChartView");
  QVERIFY(dailyViewport != nullptr);
  const auto *dailyBuy =
      dailyViewport->chart()->findChild<QScatterSeries *>("replayBuyMarkers");
  const auto *dailySell =
      dailyViewport->chart()->findChild<QScatterSeries *>("replaySellMarkers");
  QVERIFY(dailyBuy != nullptr);
  QVERIFY(dailySell != nullptr);
  QCOMPARE(dailyBuy->count(), 4);
  QCOMPARE(dailySell->count(), 3);
  for (const auto &point : dailyBuy->points()) {
    QCOMPARE(point, QPointF(0.0, 101.0101));
  }
  auto buyGlyphs = 0;
  for (const auto *item : dailyViewport->scene()->items()) {
    if (item->data(0).toString() == "replayTradeGlyph" &&
        item->toolTip().startsWith("Buy")) {
      ++buyGlyphs;
      QVERIFY(item->toolTip().contains("4 reached Buy fills"));
      QVERIFY(item->toolTip().contains("2024-01-01T01:00:00Z"));
    }
  }
  QCOMPARE(buyGlyphs, 1);
}

void ResultReplayTabTest::longRangeSeekPresentsEveryReachedCandleAndFill() {
  const auto sourceRoot = root_ / "LongSource";
  const auto dataRoot = root_ / "LongData";
  const auto resultRoot = root_ / "LongResults";
  std::filesystem::create_directories(sourceRoot);
  std::ofstream source{sourceRoot / "LONG.csv"};
  source << "symbol,ts,open,high,low,close,volume,schemaName\n";
  const auto start = QDateTime{QDate{2024, 1, 1}, QTime{0, 0}, QTimeZone::UTC};
  constexpr auto barCount = 23'777;
  for (auto index = 0; index < barCount; ++index) {
    const auto open = 100 + index % 7;
    source << "LONG,"
           << start.addSecs(index * 3'600)
                  .toString("yyyy-MM-dd HH:mm:ss+00:00")
                  .toStdString()
           << ',' << open << ',' << open + 2 << ',' << open - 1 << ','
           << open + 1 << ',' << 1'000 + index << ",ohlcv-1h\n";
  }
  source.close();
  const auto snapshot = bte::data::buildReleaseSnapshot({
      .sourceDirectory = sourceRoot,
      .storeDirectory = dataRoot,
      .symbols = {"LONG"},
      .rowsPerSegment = 1'000,
      .calendarHash = std::string(64, 'a'),
      .splitManifestHash = std::string(64, 'b'),
  });
  QVERIFY2(snapshot.ok(), snapshot.error().message.c_str());
  const auto result = bte::bindings::runPersistedBacktestConfiguration(
      {.symbol = "LONG",
       .schema = "ohlcv-1h",
       .startDate = start.date(),
       .endDate = start.addSecs((barCount - 1) * 3'600).date(),
       .initialCapital = 1'000'000,
       .quantityShares = 1,
       .selectableStrategy =
           bte::strategy::SelectableStrategyPlan{
               .buy = {.conditions = {{.comparison =
                                           bte::strategy::Comparison::lessThan,
                                       .threshold = 103.0}}},
               .sell =
                   {.conditions = {{.comparison =
                                        bte::strategy::Comparison::greaterThan,
                                    .threshold = 104.0}}}}},
      {.resultStore = resultRoot,
       .dataStore = dataRoot,
       .snapshotId = snapshot.value().snapshotId,
       .strategyHash = std::string(64, 'c')});
  QVERIFY2(result.ok(), result.error().message.c_str());
  auto store = bte::results::ResultStore::open(resultRoot, dataRoot);
  QVERIFY2(store.ok(), store.error().message.c_str());
  const auto published = store.value()->publishDraft(result.value().resultId);
  QVERIFY2(published.ok(), published.error().message.c_str());
  bte::frontend::ReplayTab tab{resultRoot, dataRoot};
  tab.resize(1200, 800);
  tab.show();
  QVERIFY(QTest::qWaitForWindowExposed(&tab));
  auto *seek = tab.findChild<QSlider *>("replaySeekSlider");
  auto *trades = tab.findChild<QTableWidget *>("replayTradeLogTable");
  auto *zoomIn = tab.findChild<QToolButton *>("replayZoomInButton");
  auto *follow = tab.findChild<QToolButton *>("replayFollowLatestButton");
  auto *status = tab.findChild<QLabel *>("replayResultStatusLabel");
  auto *selector = tab.findChild<QComboBox *>("replayResultCombo");
  auto *chart = tab.findChild<bte::frontend::QtChartsCandlestickView *>(
      "replayCandlestickChartView");
  QVERIFY(seek != nullptr);
  QVERIFY(trades != nullptr);
  QVERIFY(zoomIn != nullptr);
  QVERIFY(follow != nullptr);
  QVERIFY(status != nullptr);
  QVERIFY(selector != nullptr);
  QVERIFY(chart != nullptr);
  QElapsedTimer loadTimer;
  loadTimer.start();
  tab.openResult(QString::fromStdString(result.value().resultId));
  const auto loaded =
      bte::test::waitUntil([&] { return seek->maximum() == barCount - 1; });
  qInfo("23,777-bar / 6,793-fill Replay load: %lld ms", loadTimer.elapsed());
  QVERIFY2(loaded, qPrintable(status->text()));
  QVERIFY(bte::test::waitUntil([&] {
    return selector->findData(
               QString::fromStdString(result.value().resultId)) >= 0;
  }));
  QCOMPARE(chart->candleCount(), 1U);
  QElapsedTimer timer;
  timer.start();
  seek->setValue(barCount - 1);
  QCoreApplication::processEvents();
  qInfo("23,777-bar / 6,793-fill Replay seek: %lld ms", timer.elapsed());
  QCOMPARE(chart->candleCount(), static_cast<std::size_t>(barCount));
  QCOMPARE(chart->volumePointCount(), chart->candleCount());
  QCOMPARE(chart->markerCount(), 6'793U);
  QCOMPARE(trades->rowCount(), 6'793);
  QCOMPARE(trades->item(0, 0)->text(), QString{"2024-01-01T01:00:00Z"});
  QCOMPARE(trades->item(6'792, 0)->text(),
           start.addSecs(23'773 * 3'600).toString(Qt::ISODate));
  const auto cursorBeforeFollow = seek->value();
  const auto resultBeforeFollow = status->text();
  QVERIFY(resultBeforeFollow.contains(
      QString::fromStdString(result.value().resultId)));
  QTest::mouseClick(zoomIn, Qt::LeftButton);
  const auto zoomedSlots = chart->visibleSlots();
  QVERIFY(zoomedSlots.end < static_cast<std::size_t>(barCount));
  QVERIFY(follow->isEnabled());
  QTest::mouseClick(follow, Qt::LeftButton);
  const auto followedSlots = chart->visibleSlots();
  QCOMPARE(followedSlots.end, static_cast<std::size_t>(barCount));
  QCOMPARE(followedSlots.end - followedSlots.first,
           zoomedSlots.end - zoomedSlots.first);
  QCOMPARE(seek->value(), cursorBeforeFollow);
  QCOMPARE(status->text(), resultBeforeFollow);
  QVERIFY(!follow->isEnabled());
  seek->setValue(0);
  QCOMPARE(chart->candleCount(), 1U);
  QCOMPARE(chart->markerCount(), 0U);
  QCOMPARE(trades->rowCount(), 0);
}

void ResultReplayTabTest::maximumPlaybackStopsAtAndAfterTheFinalFrame() {
  bte::frontend::ReplayTab tab{root_ / "Store", root_ / "Data"};
  auto *status = tab.findChild<QLabel *>("replayResultStatusLabel");
  auto *seek = tab.findChild<QSlider *>("replaySeekSlider");
  auto *speed = tab.findChild<QComboBox *>("replaySpeedCombo");
  auto *play = tab.findChild<QToolButton *>("replayPlayPauseButton");
  auto *timer = tab.findChild<QTimer *>("replayPlaybackTimer");
  QVERIFY(status != nullptr);
  QVERIFY(seek != nullptr);
  QVERIFY(speed != nullptr);
  QVERIFY(play != nullptr);
  QVERIFY(timer != nullptr);
  tab.openResult(QString::fromStdString(firstResultId_));
  QVERIFY(bte::test::waitUntil([&] {
    return status->text().startsWith("Result ") && seek->maximum() == 2;
  }));
  speed->setCurrentText("max");
  seek->setValue(1);

  QVERIFY(QMetaObject::invokeMethod(timer, "timeout", Qt::DirectConnection));

  QCOMPARE(seek->value(), 2);
  QCOMPARE(play->text(), QString{"Play"});
  QVERIFY(!timer->isActive());

  play->setText("Pause");
  QVERIFY(QMetaObject::invokeMethod(timer, "timeout", Qt::DirectConnection));

  QCOMPARE(seek->value(), 2);
  QCOMPARE(play->text(), QString{"Play"});
  QVERIFY(!timer->isActive());
}

void ResultReplayTabTest::
    renamedResultRefreshesSelectorWithoutChangingOpenId() {
  bte::frontend::ReplayTab tab{root_ / "Store", root_ / "Data"};
  auto *selector = tab.findChild<QComboBox *>("replayResultCombo");
  auto *status = tab.findChild<QLabel *>("replayResultStatusLabel");
  QVERIFY(selector != nullptr);
  QVERIFY(status != nullptr);
  auto store = bte::results::ResultStore::open(root_ / "Store", root_ / "Data");
  QVERIFY(store.ok());
  QVERIFY(store.value()->renameResult(secondResultId_, "Renamed fixture").ok());

  tab.openResult(QString::fromStdString(secondResultId_));
  QVERIFY(bte::test::waitUntil([&] {
    return (selector->currentText().startsWith("Renamed fixture — "));
  }));
  QCOMPARE(selector->currentData().toString(),
           QString::fromStdString(secondResultId_));
  QVERIFY(bte::test::waitUntil([&] {
    return (status->text().startsWith(
        QString{"Result %1"}.arg(QString::fromStdString(secondResultId_))));
  }));
}

void ResultReplayTabTest::
    thirtyTwoCharacterNamedOptionOpensItsStoredResultId() {
  const auto name = std::string(21, 'N');
  auto store = bte::results::ResultStore::open(root_ / "Store", root_ / "Data");
  QVERIFY(store.ok());
  QVERIFY(store.value()->renameResult(secondResultId_, name).ok());

  bte::frontend::ReplayTab tab{root_ / "Store", root_ / "Data"};
  auto *selector = tab.findChild<QComboBox *>("replayResultCombo");
  auto *open = tab.findChild<QPushButton *>("replayOpenResultButton");
  auto *status = tab.findChild<QLabel *>("replayResultStatusLabel");
  QVERIFY(selector != nullptr);
  QVERIFY(open != nullptr);
  QVERIFY(status != nullptr);
  QVERIFY(bte::test::waitUntil([&] {
    return (selector->findData(QString::fromStdString(secondResultId_)) >= 0);
  }));
  selector->setCurrentIndex(
      selector->findData(QString::fromStdString(secondResultId_)));
  QCOMPARE(selector->currentText().size(), 32);
  QTest::mouseClick(open, Qt::LeftButton);
  QVERIFY(bte::test::waitUntil([&] {
    return (status->text().startsWith(
        QString{"Result %1"}.arg(QString::fromStdString(secondResultId_))));
  }));
}

} // namespace

QTEST_MAIN(ResultReplayTabTest)

#include "UnitTest_ResultReplayTab.moc"
