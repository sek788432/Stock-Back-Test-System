#include "Bte/Frontend/BacktestTab.h"

#include <QCalendarWidget>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDateEdit>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QPushButton>
#include <QSpinBox>
#include <QTableWidget>
#include <QTest>
#include <QThread>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <vector>

namespace {

bte::bindings::BacktestSnapshot filledSnapshot() {
  return bte::bindings::BacktestSnapshot{
      .outcome = bte::bindings::BacktestOutcome::filled,
      .fill =
          bte::bindings::BacktestFillSnapshot{
              .timestamp = bte::core::Timestamp{std::chrono::sys_days{
                  std::chrono::year{2024} / 1 / 3}},
              .quantityShares = 10,
              .price = 110.011,
              .amount = 1'100.11,
          },
      .fills = {bte::bindings::BacktestFillSnapshot{
          .timestamp = bte::core::Timestamp{std::chrono::sys_days{
              std::chrono::year{2024} / 1 / 3}},
          .quantityShares = 10,
          .price = 110.011,
          .amount = 1'100.11,
      }},
      .initialCapital = 2'000.0,
      .cash = 899.89,
      .marketValue = 1'200.0,
      .equity = 2'099.89,
      .pnl = 99.89,
      .finalPrice = 120.0,
      .positionShares = 10,
      .barsProcessed = 2,
  };
}

bte::bindings::BacktestSnapshot
noFillSnapshot(const bte::bindings::BacktestOutcome outcome) {
  return bte::bindings::BacktestSnapshot{
      .outcome = outcome,
      .fill = {},
      .initialCapital = 100.0,
      .cash = 100.0,
      .marketValue = 0.0,
      .equity = 100.0,
      .pnl = 0.0,
      .finalPrice = 200.0,
      .positionShares = 0,
      .barsProcessed = 2,
  };
}

class BacktestTabTest final : public QObject {
  Q_OBJECT

private slots:
  void exposesAccessibleRunConfiguration();
  void calendarYearsAreDirectlySelectable();
  void runExecutesOffTheUiThreadAndDisplaysFill();
  void failedRunClearsPriorResultAndPreservesError();
  void presentsRejectedAndCancelledOutcomes();
  void runSubmitsEveryVisibleConfigurationField();
  void destructionCancelsAndJoinsActiveRun();
  void workerExceptionIsPresentedAsAnError();
  void selectableConditionsSubmitTypedPlanToBackend();
  void selectableMetricsSubmitTypedPlansToBackend();
  void selectableConditionStatusNamesItsSelectedStrategy();
  void selectableControlsPresentNoSignalAndSellFill();
};

void BacktestTabTest::exposesAccessibleRunConfiguration() {
  const bte::frontend::BacktestTab tab;

  const auto *strategy = tab.findChild<QComboBox *>("backtestStrategyCombo");
  const auto *run = tab.findChild<QPushButton *>("backtestRunButton");
  QVERIFY(strategy != nullptr);
  QVERIFY(run != nullptr);
  QCOMPARE(strategy->currentText(), QString{"Starter market buy"});
  QVERIFY(!run->shortcut().isEmpty());
  QVERIFY(!tab.accessibleName().isEmpty());

  const auto widgets = tab.findChildren<QWidget *>();
  for (const auto *widget : widgets) {
    if (widget->objectName().startsWith("backtest")) {
      QVERIFY2(!widget->accessibleName().isEmpty(),
               qPrintable(widget->objectName()));
    }
  }
}

void BacktestTabTest::calendarYearsAreDirectlySelectable() {
  bte::frontend::BacktestTab tab;

  auto *start = tab.findChild<QDateEdit *>("backtestStartDateEdit");
  auto *startYear = tab.findChild<QComboBox *>("backtestStartYearCombo");
  auto *endYear = tab.findChild<QComboBox *>("backtestEndYearCombo");
  QVERIFY(start != nullptr);
  QVERIFY(startYear != nullptr);
  QVERIFY(endYear != nullptr);
  QVERIFY(start->calendarWidget()->isAncestorOf(startYear));
  const auto *end = tab.findChild<QDateEdit *>("backtestEndDateEdit");
  QVERIFY(end != nullptr);
  QVERIFY(end->calendarWidget()->isAncestorOf(endYear));
  QVERIFY(startYear->count() > 100);
  QCOMPARE(startYear->currentText(), QString{"2018"});
  QVERIFY(startYear->minimumWidth() >= 88);
  QVERIFY(endYear->minimumWidth() >= 88);

  startYear->setCurrentText("2035");
  QCOMPARE(start->calendarWidget()->yearShown(), 2035);

  start->setDate(QDate{2024, 2, 29});
  QCOMPARE(startYear->currentText(), QString{"2024"});

  startYear->setCurrentIndex(0);
  QCOMPARE(start->calendarWidget()->yearShown(), start->minimumDate().year());
  startYear->setCurrentIndex(startYear->count() - 1);
  QCOMPARE(start->calendarWidget()->yearShown(), start->maximumDate().year());
  startYear->setCurrentText("10000");
  QCOMPARE(start->calendarWidget()->yearShown(), start->maximumDate().year());
}

void BacktestTabTest::runExecutesOffTheUiThreadAndDisplaysFill() {
  std::atomic_bool ranOffUiThread = false;
  bte::frontend::BacktestTab tab{[&ranOffUiThread](
                                     bte::bindings::BacktestConfiguration,
                                     bte::core::CancellationToken) {
    ranOffUiThread =
        QThread::currentThread() != QCoreApplication::instance()->thread();
    return bte::core::Result<bte::bindings::BacktestSnapshot>{filledSnapshot()};
  }};

  auto *run = tab.findChild<QPushButton *>("backtestRunButton");
  auto *status = tab.findChild<QLabel *>("backtestStatusLabel");
  auto *equity = tab.findChild<QLabel *>("backtestEquityLabel");
  auto *pnl = tab.findChild<QLabel *>("backtestPnlLabel");
  auto *trades = tab.findChild<QTableWidget *>("backtestTradeLogTable");
  QVERIFY(run != nullptr);
  QVERIFY(status != nullptr);
  QVERIFY(equity != nullptr);
  QVERIFY(pnl != nullptr);
  QVERIFY(trades != nullptr);

  QTest::mouseClick(run, Qt::LeftButton);

  QTRY_VERIFY_WITH_TIMEOUT(status->text().contains("Completed"), 5'000);
  QVERIFY(ranOffUiThread);
  QVERIFY(run->isEnabled());
  QVERIFY(!equity->text().contains("--"));
  QVERIFY(!pnl->text().contains("--"));
  QCOMPARE(equity->accessibleName(), equity->text());
  QCOMPARE(status->accessibleName(), status->text());
  QCOMPARE(trades->rowCount(), 1);
}

void BacktestTabTest::failedRunClearsPriorResultAndPreservesError() {
  std::atomic_int invocation = 0;
  bte::frontend::BacktestTab tab{
      [&invocation](bte::bindings::BacktestConfiguration,
                    bte::core::CancellationToken) {
        if (invocation.fetch_add(1) == 0) {
          return bte::core::Result<bte::bindings::BacktestSnapshot>{
              filledSnapshot()};
        }
        return bte::core::Result<bte::bindings::BacktestSnapshot>{
            bte::core::makeError(bte::core::ErrorCode::notFound,
                                 "source file is missing")};
      }};

  auto *run = tab.findChild<QPushButton *>("backtestRunButton");
  auto *status = tab.findChild<QLabel *>("backtestStatusLabel");
  auto *cash = tab.findChild<QLabel *>("backtestCashLabel");
  auto *equity = tab.findChild<QLabel *>("backtestEquityLabel");
  auto *pnl = tab.findChild<QLabel *>("backtestPnlLabel");
  auto *bars = tab.findChild<QLabel *>("backtestBarsLabel");
  auto *trades = tab.findChild<QTableWidget *>("backtestTradeLogTable");
  QVERIFY(run != nullptr);
  QVERIFY(status != nullptr);
  QVERIFY(cash != nullptr);
  QVERIFY(equity != nullptr);
  QVERIFY(pnl != nullptr);
  QVERIFY(bars != nullptr);
  QVERIFY(trades != nullptr);

  QTest::mouseClick(run, Qt::LeftButton);
  QTRY_COMPARE_WITH_TIMEOUT(trades->rowCount(), 1, 5'000);
  QTest::mouseClick(run, Qt::LeftButton);

  QTRY_VERIFY_WITH_TIMEOUT(status->text().contains("source file is missing"),
                           5'000);
  QCOMPARE(trades->rowCount(), 0);
  QVERIFY(cash->text().contains("--"));
  QVERIFY(equity->text().contains("--"));
  QVERIFY(pnl->text().contains("--"));
  QVERIFY(bars->text().contains("--"));
}

void BacktestTabTest::presentsRejectedAndCancelledOutcomes() {
  const auto runOutcome = [](const bte::bindings::BacktestOutcome outcome,
                             const QString &expectedText) {
    bte::frontend::BacktestTab tab{
        [outcome](bte::bindings::BacktestConfiguration,
                  bte::core::CancellationToken) {
          return bte::core::Result<bte::bindings::BacktestSnapshot>{
              noFillSnapshot(outcome)};
        }};
    auto *run = tab.findChild<QPushButton *>("backtestRunButton");
    auto *status = tab.findChild<QLabel *>("backtestStatusLabel");
    auto *trades = tab.findChild<QTableWidget *>("backtestTradeLogTable");
    QVERIFY(run != nullptr);
    QVERIFY(status != nullptr);
    QVERIFY(trades != nullptr);

    QTest::mouseClick(run, Qt::LeftButton);
    QTRY_VERIFY_WITH_TIMEOUT(status->text().contains(expectedText), 5'000);
    QCOMPARE(trades->rowCount(), 0);
  };

  runOutcome(bte::bindings::BacktestOutcome::rejectedInsufficientCash,
             "insufficient cash");
  runOutcome(bte::bindings::BacktestOutcome::cancelledNoFutureMarketData,
             "no future market data");
}

void BacktestTabTest::runSubmitsEveryVisibleConfigurationField() {
  std::mutex captureMutex;
  std::optional<bte::bindings::BacktestConfiguration> captured;
  bte::frontend::BacktestTab tab{
      [&captureMutex, &captured](bte::bindings::BacktestConfiguration config,
                                 bte::core::CancellationToken) {
        const std::scoped_lock lock{captureMutex};
        captured = std::move(config);
        return bte::core::Result<bte::bindings::BacktestSnapshot>{
            filledSnapshot()};
      }};
  auto *symbol = tab.findChild<QComboBox *>("backtestSymbolCombo");
  auto *start = tab.findChild<QDateEdit *>("backtestStartDateEdit");
  auto *end = tab.findChild<QDateEdit *>("backtestEndDateEdit");
  auto *capital =
      tab.findChild<QDoubleSpinBox *>("backtestInitialCapitalSpinBox");
  auto *quantity = tab.findChild<QSpinBox *>("backtestQuantitySpinBox");
  auto *run = tab.findChild<QPushButton *>("backtestRunButton");
  auto *status = tab.findChild<QLabel *>("backtestStatusLabel");
  QVERIFY(symbol != nullptr);
  QVERIFY(start != nullptr);
  QVERIFY(end != nullptr);
  QVERIFY(capital != nullptr);
  QVERIFY(quantity != nullptr);
  QVERIFY(run != nullptr);
  QVERIFY(status != nullptr);

  symbol->setCurrentText("MSFT");
  start->setDate(QDate{2020, 2, 3});
  end->setDate(QDate{2021, 4, 5});
  capital->setValue(12'345.67);
  quantity->setValue(321);
  QTest::mouseClick(run, Qt::LeftButton);
  QTRY_VERIFY_WITH_TIMEOUT(status->text().contains("Completed"), 5'000);

  const std::scoped_lock lock{captureMutex};
  QVERIFY(captured.has_value());
  QCOMPARE(captured->symbol, QString{"MSFT"});
  QCOMPARE(captured->schema, QString{"ohlcv-1h"});
  QCOMPARE(captured->startDate, QDate(2020, 2, 3));
  QCOMPARE(captured->endDate, QDate(2021, 4, 5));
  QCOMPARE(captured->initialCapital, 12'345.67);
  QCOMPARE(captured->quantityShares, 321);
}

void BacktestTabTest::destructionCancelsAndJoinsActiveRun() {
  std::atomic_bool started = false;
  std::atomic_bool observedCancellation = false;
  auto tab = std::make_unique<bte::frontend::BacktestTab>(
      [&started, &observedCancellation](bte::bindings::BacktestConfiguration,
                                        bte::core::CancellationToken token) {
        started.store(true);
        while (!token.isCancellationRequested()) {
          QThread::msleep(1);
        }
        observedCancellation.store(true);
        return bte::core::Result<bte::bindings::BacktestSnapshot>{
            bte::core::makeError(bte::core::ErrorCode::cancelled,
                                 "backtest was cancelled")};
      });
  auto *run = tab->findChild<QPushButton *>("backtestRunButton");
  QVERIFY(run != nullptr);
  QTest::mouseClick(run, Qt::LeftButton);
  QTRY_VERIFY_WITH_TIMEOUT(started.load(), 5'000);

  tab.reset();

  QVERIFY(observedCancellation.load());
}

void BacktestTabTest::workerExceptionIsPresentedAsAnError() {
  bte::frontend::BacktestTab tab{
      [](bte::bindings::BacktestConfiguration, bte::core::CancellationToken)
          -> bte::core::Result<bte::bindings::BacktestSnapshot> {
        throw std::runtime_error{"unexpected runner failure"};
      }};
  auto *run = tab.findChild<QPushButton *>("backtestRunButton");
  auto *status = tab.findChild<QLabel *>("backtestStatusLabel");
  QVERIFY(run != nullptr);
  QVERIFY(status != nullptr);

  QTest::mouseClick(run, Qt::LeftButton);

  QTRY_VERIFY_WITH_TIMEOUT(status->text().contains("unexpected runner failure"),
                           5'000);
  QVERIFY(run->isEnabled());
}

void BacktestTabTest::selectableConditionsSubmitTypedPlanToBackend() {
  std::mutex captureMutex;
  std::optional<bte::bindings::BacktestConfiguration> captured;
  bte::frontend::BacktestTab tab{
      [&captureMutex,
       &captured](bte::bindings::BacktestConfiguration configuration,
                  bte::core::CancellationToken) {
        const std::scoped_lock lock{captureMutex};
        captured = std::move(configuration);
        return bte::core::Result<bte::bindings::BacktestSnapshot>{
            filledSnapshot()};
      }};
  auto *strategy = tab.findChild<QComboBox *>("backtestStrategyCombo");
  auto *buyLogic = tab.findChild<QComboBox *>("backtestBuyLogicCombo");
  auto *buyMetric = tab.findChild<QComboBox *>("backtestBuyMetricCombo");
  auto *buyPeriod = tab.findChild<QSpinBox *>("backtestBuyPeriodSpinBox");
  auto *buyThreshold =
      tab.findChild<QDoubleSpinBox *>("backtestBuyThresholdSpinBox");
  auto *buySecond =
      tab.findChild<QCheckBox *>("backtestBuySecondEnabledCheckBox");
  auto *buySecondMetric =
      tab.findChild<QComboBox *>("backtestBuySecondMetricCombo");
  auto *run = tab.findChild<QPushButton *>("backtestRunButton");
  auto *status = tab.findChild<QLabel *>("backtestStatusLabel");
  QVERIFY(strategy != nullptr);
  QVERIFY(buyLogic != nullptr);
  QVERIFY(buyMetric != nullptr);
  QVERIFY(buyPeriod != nullptr);
  QVERIFY(buyThreshold != nullptr);
  QVERIFY(buySecond != nullptr);
  QVERIFY(buySecondMetric != nullptr);
  QVERIFY(run != nullptr);
  QVERIFY(status != nullptr);

  strategy->setCurrentText("Selectable conditions");
  buyLogic->setCurrentIndex(1);
  buyMetric->setCurrentText("MACD histogram");
  buyPeriod->setValue(16);
  buyThreshold->setValue(60.0);
  buySecond->setChecked(true);
  buySecondMetric->setCurrentText("Close change %");
  QTest::mouseClick(run, Qt::LeftButton);
  QTRY_VERIFY_WITH_TIMEOUT(status->text().contains("Completed"), 5'000);

  const std::scoped_lock lock{captureMutex};
  QVERIFY(captured.has_value());
  QVERIFY(captured->selectableStrategy.has_value());
  const auto &buy = captured->selectableStrategy->buy;
  QCOMPARE(buy.logic, bte::strategy::ConditionLogic::any);
  QCOMPARE(buy.conditions.size(), 2U);
  QCOMPARE(buy.conditions[0].source, bte::strategy::ConditionSource::indicator);
  QCOMPARE(buy.conditions[0].indicator.kind,
           bte::indicators::IndicatorKind::macd);
  QCOMPARE(buy.conditions[0].indicator.period, 8);
  QCOMPARE(buy.conditions[0].indicator.secondaryPeriod, 16);
  QCOMPARE(buy.conditions[0].indicator.signalPeriod, 5);
  QCOMPARE(buy.conditions[0].threshold, 60.0);
  QCOMPARE(buy.conditions[0].thresholdDomain,
           bte::indicators::NumericDomain::price);
  QCOMPARE(buy.conditions[1].source,
           bte::strategy::ConditionSource::closeChangePercent);
  QCOMPARE(buy.conditions[1].thresholdDomain,
           bte::indicators::NumericDomain::percent);
}

void BacktestTabTest::selectableMetricsSubmitTypedPlansToBackend() {
  struct MetricCase final {
    QString label;
    bte::strategy::ConditionSource source;
    bte::indicators::NumericDomain domain;
    std::optional<bte::indicators::IndicatorKind> indicatorKind;
    bte::indicators::IndicatorOutput indicatorOutput =
        bte::indicators::IndicatorOutput::value;
  };
  const auto metricCases = std::vector<MetricCase>{
      {"Close change %",
       bte::strategy::ConditionSource::closeChangePercent,
       bte::indicators::NumericDomain::percent,
       {}},
      {"Close",
       bte::strategy::ConditionSource::barField,
       bte::indicators::NumericDomain::price,
       {}},
      {"SMA", bte::strategy::ConditionSource::indicator,
       bte::indicators::NumericDomain::price,
       bte::indicators::IndicatorKind::sma},
      {"EMA", bte::strategy::ConditionSource::indicator,
       bte::indicators::NumericDomain::price,
       bte::indicators::IndicatorKind::ema},
      {"WMA", bte::strategy::ConditionSource::indicator,
       bte::indicators::NumericDomain::price,
       bte::indicators::IndicatorKind::wma},
      {"RSI", bte::strategy::ConditionSource::indicator,
       bte::indicators::NumericDomain::percent,
       bte::indicators::IndicatorKind::rsi},
      {"MACD histogram", bte::strategy::ConditionSource::indicator,
       bte::indicators::NumericDomain::price,
       bte::indicators::IndicatorKind::macd,
       bte::indicators::IndicatorOutput::histogram},
      {"Bollinger upper", bte::strategy::ConditionSource::indicator,
       bte::indicators::NumericDomain::price,
       bte::indicators::IndicatorKind::bollingerBands,
       bte::indicators::IndicatorOutput::upper},
      {"ATR", bte::strategy::ConditionSource::indicator,
       bte::indicators::NumericDomain::price,
       bte::indicators::IndicatorKind::atr},
      {"ADX", bte::strategy::ConditionSource::indicator,
       bte::indicators::NumericDomain::percent,
       bte::indicators::IndicatorKind::adx},
      {"Stochastic %K", bte::strategy::ConditionSource::indicator,
       bte::indicators::NumericDomain::percent,
       bte::indicators::IndicatorKind::stochastic,
       bte::indicators::IndicatorOutput::percentK},
      {"Donchian upper", bte::strategy::ConditionSource::indicator,
       bte::indicators::NumericDomain::price,
       bte::indicators::IndicatorKind::donchian,
       bte::indicators::IndicatorOutput::upper},
      {"VWAP", bte::strategy::ConditionSource::indicator,
       bte::indicators::NumericDomain::price,
       bte::indicators::IndicatorKind::vwap},
      {"OBV", bte::strategy::ConditionSource::indicator,
       bte::indicators::NumericDomain::volume,
       bte::indicators::IndicatorKind::obv},
      {"ROC", bte::strategy::ConditionSource::indicator,
       bte::indicators::NumericDomain::percent,
       bte::indicators::IndicatorKind::roc},
      {"Momentum", bte::strategy::ConditionSource::indicator,
       bte::indicators::NumericDomain::price,
       bte::indicators::IndicatorKind::momentum},
      {"True range", bte::strategy::ConditionSource::indicator,
       bte::indicators::NumericDomain::price,
       bte::indicators::IndicatorKind::trueRange},
      {"Volume",
       bte::strategy::ConditionSource::barField,
       bte::indicators::NumericDomain::volume,
       {}},
  };

  std::mutex captureMutex;
  std::optional<bte::bindings::BacktestConfiguration> captured;
  std::atomic_int runCount = 0;
  bte::frontend::BacktestTab tab{
      [&captureMutex, &captured,
       &runCount](bte::bindings::BacktestConfiguration configuration,
                  bte::core::CancellationToken) {
        const std::scoped_lock lock{captureMutex};
        captured = std::move(configuration);
        runCount.fetch_add(1);
        return bte::core::Result<bte::bindings::BacktestSnapshot>{
            filledSnapshot()};
      }};
  auto *strategy = tab.findChild<QComboBox *>("backtestStrategyCombo");
  auto *metric = tab.findChild<QComboBox *>("backtestBuyMetricCombo");
  auto *period = tab.findChild<QSpinBox *>("backtestBuyPeriodSpinBox");
  auto *run = tab.findChild<QPushButton *>("backtestRunButton");
  QVERIFY(strategy != nullptr);
  QVERIFY(metric != nullptr);
  QVERIFY(period != nullptr);
  QVERIFY(run != nullptr);

  strategy->setCurrentText("Selectable conditions");
  period->setValue(18);
  for (std::size_t index = 0; index < metricCases.size(); ++index) {
    const auto &metricCase = metricCases[index];
    metric->setCurrentText(metricCase.label);
    QTest::mouseClick(run, Qt::LeftButton);
    const auto expectedRuns = static_cast<int>(index + 1U);
    QTRY_VERIFY_WITH_TIMEOUT(
        runCount.load() == expectedRuns && run->isEnabled(), 5'000);

    const std::scoped_lock lock{captureMutex};
    QVERIFY(captured.has_value());
    QVERIFY(captured->selectableStrategy.has_value());
    const auto &condition = captured->selectableStrategy->buy.conditions[0];
    QCOMPARE(condition.source, metricCase.source);
    QCOMPARE(condition.thresholdDomain, metricCase.domain);
    if (metricCase.indicatorKind.has_value()) {
      QCOMPARE(condition.indicator.kind, metricCase.indicatorKind.value());
      QCOMPARE(condition.indicator.output, metricCase.indicatorOutput);
    }
  }
}

void BacktestTabTest::selectableConditionStatusNamesItsSelectedStrategy() {
  bte::frontend::BacktestTab tab{[](bte::bindings::BacktestConfiguration,
                                    bte::core::CancellationToken) {
    return bte::core::Result<bte::bindings::BacktestSnapshot>{filledSnapshot()};
  }};
  auto *strategy = tab.findChild<QComboBox *>("backtestStrategyCombo");
  auto *run = tab.findChild<QPushButton *>("backtestRunButton");
  auto *status = tab.findChild<QLabel *>("backtestStatusLabel");
  QVERIFY(strategy != nullptr);
  QVERIFY(run != nullptr);
  QVERIFY(status != nullptr);

  strategy->setCurrentText("Selectable conditions");
  QTest::mouseClick(run, Qt::LeftButton);

  QTRY_VERIFY_WITH_TIMEOUT(status->text().contains("selectable strategy"),
                           5'000);
  QVERIFY(!status->text().contains("starter order"));
}

void BacktestTabTest::selectableControlsPresentNoSignalAndSellFill() {
  std::atomic_int invocation = 0;
  std::mutex captureMutex;
  std::optional<bte::bindings::BacktestConfiguration> captured;
  bte::frontend::BacktestTab tab{
      [&captureMutex, &captured,
       &invocation](bte::bindings::BacktestConfiguration configuration,
                    bte::core::CancellationToken) {
        {
          const std::scoped_lock lock{captureMutex};
          captured = std::move(configuration);
        }
        if (invocation.fetch_add(1) == 0) {
          return bte::core::Result<bte::bindings::BacktestSnapshot>{
              noFillSnapshot(
                  bte::bindings::BacktestOutcome::completedNoSignal)};
        }
        auto snapshot = filledSnapshot();
        snapshot.fills.front().side = bte::bindings::BacktestFillSide::sell;
        return bte::core::Result<bte::bindings::BacktestSnapshot>{snapshot};
      }};
  auto *strategy = tab.findChild<QComboBox *>("backtestStrategyCombo");
  auto *sellSecond =
      tab.findChild<QCheckBox *>("backtestSellSecondEnabledCheckBox");
  auto *sellLogic = tab.findChild<QComboBox *>("backtestSellLogicCombo");
  auto *sellMetric = tab.findChild<QComboBox *>("backtestSellMetricCombo");
  auto *sellThreshold =
      tab.findChild<QDoubleSpinBox *>("backtestSellThresholdSpinBox");
  auto *sellSecondMetric =
      tab.findChild<QComboBox *>("backtestSellSecondMetricCombo");
  auto *sellSecondThreshold =
      tab.findChild<QDoubleSpinBox *>("backtestSellSecondThresholdSpinBox");
  auto *run = tab.findChild<QPushButton *>("backtestRunButton");
  auto *status = tab.findChild<QLabel *>("backtestStatusLabel");
  auto *trades = tab.findChild<QTableWidget *>("backtestTradeLogTable");
  QVERIFY(strategy != nullptr);
  QVERIFY(sellSecond != nullptr);
  QVERIFY(sellLogic != nullptr);
  QVERIFY(sellMetric != nullptr);
  QVERIFY(sellThreshold != nullptr);
  QVERIFY(sellSecondMetric != nullptr);
  QVERIFY(sellSecondThreshold != nullptr);
  QVERIFY(run != nullptr);
  QVERIFY(status != nullptr);
  QVERIFY(trades != nullptr);

  strategy->setCurrentText("Selectable conditions");
  sellLogic->setCurrentText("ANY (OR)");
  sellMetric->setCurrentText("ROC");
  sellThreshold->setValue(-2.0);
  QVERIFY(!sellSecondMetric->isEnabled());
  QTest::mouseClick(sellSecond, Qt::LeftButton);
  sellSecondMetric->setCurrentText("Stochastic %K");
  sellSecondThreshold->setValue(20.0);
  QVERIFY(sellSecondMetric->isEnabled());
  QTest::mouseClick(run, Qt::LeftButton);
  QTRY_VERIFY_WITH_TIMEOUT(status->text().contains("no selectable condition"),
                           5'000);
  {
    const std::scoped_lock lock{captureMutex};
    QVERIFY(captured.has_value());
    QVERIFY(captured->selectableStrategy.has_value());
    const auto &sell = captured->selectableStrategy->sell;
    QCOMPARE(sell.logic, bte::strategy::ConditionLogic::any);
    QCOMPARE(sell.conditions.size(), 2U);
    QCOMPARE(sell.conditions[0].indicator.kind,
             bte::indicators::IndicatorKind::roc);
    QCOMPARE(sell.conditions[0].threshold, -2.0);
    QCOMPARE(sell.conditions[1].indicator.kind,
             bte::indicators::IndicatorKind::stochastic);
    QCOMPARE(sell.conditions[1].indicator.output,
             bte::indicators::IndicatorOutput::percentK);
    QCOMPARE(sell.conditions[1].threshold, 20.0);
  }

  QTest::mouseClick(run, Qt::LeftButton);
  QTRY_COMPARE_WITH_TIMEOUT(trades->rowCount(), 1, 5'000);
  QCOMPARE(trades->item(0, 1)->text(), QString{"Sell"});
}

} // namespace

QTEST_MAIN(BacktestTabTest)

#include "UnitTest_BacktestTab.moc"
