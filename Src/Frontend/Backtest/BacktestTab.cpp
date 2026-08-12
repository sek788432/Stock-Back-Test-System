#include "Bte/Frontend/BacktestTab.h"

#include "Bte/Bindings/BacktestSessionVm.h"
#include "Bte/Indicators/StreamingIndicator.h"
#include "Bte/Strategy/SelectableStrategy.h"

#include <QAbstractItemView>
#include <QCalendarWidget>
#include <QCheckBox>
#include <QComboBox>
#include <QDateEdit>
#include <QDateTime>
#include <QDoubleSpinBox>
#include <QFont>
#include <QFutureWatcher>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLayout>
#include <QList>
#include <QPushButton>
#include <QString>
#include <QTableWidget>
#include <QToolButton>
#include <QVariant>
#include <QtConcurrentRun>
#include <QtCore/Qt>
#include <QtCore/qlocale.h>
#include <QtCore/qobject.h>
#include <QtCore/qoverload.h>
#include <QtCore/qtimezone.h>
#include <QtGui/qkeysequence.h>
#include <algorithm>

#include <chrono>
#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace bte::frontend {
namespace {

constexpr auto yearSelectorMinimumWidth = 96;

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
QLabel *makeLabel(const QString &text, const QString &objectName,
                  QWidget *parent) {
  auto label = std::make_unique<QLabel>(text, parent);
  label->setObjectName(objectName);
  label->setAccessibleName(text);
  return label.release();
}

void setLabelText(QLabel *label, const QString &text) {
  label->setText(text);
  label->setAccessibleName(text);
}

QComboBox *addYearSelector(QDateEdit *dateEdit, const QString &objectName) {
  auto *calendar = dateEdit->calendarWidget();
  auto *yearButton =
      calendar->findChild<QToolButton *>("qt_calendar_yearbutton");
  if (yearButton == nullptr) {
    return nullptr;
  }
  auto *navigationBar = yearButton->parentWidget();
  auto *navigationLayout = qobject_cast<QHBoxLayout *>(navigationBar->layout());
  if (navigationLayout == nullptr) {
    return nullptr;
  }

  auto yearSelector = std::make_unique<QComboBox>(navigationBar);
  yearSelector->setObjectName(objectName);
  yearSelector->setAccessibleName(BacktestTab::tr("Calendar year"));
  yearSelector->setMinimumWidth(yearSelectorMinimumWidth);
  yearSelector->setSizeAdjustPolicy(QComboBox::AdjustToContents);
  const auto minimumYear = dateEdit->minimumDate().year();
  const auto maximumYear = dateEdit->maximumDate().year();
  for (auto year = minimumYear; year <= maximumYear; ++year) {
    yearSelector->addItem(QString::number(year), year);
  }

  auto *selector = yearSelector.get();
  const auto yearButtonIndex = navigationLayout->indexOf(yearButton);
  navigationLayout->insertWidget(yearButtonIndex, yearSelector.release());
  yearButton->hide();

  QObject::connect(selector, &QComboBox::currentIndexChanged, calendar,
                   [calendar, selector](const int index) {
                     if (index >= 0) {
                       calendar->setCurrentPage(
                           selector->itemData(index).toInt(),
                           calendar->monthShown());
                     }
                   });
  QObject::connect(calendar, &QCalendarWidget::currentPageChanged, selector,
                   [selector, minimumYear](const int year, const int) {
                     const QSignalBlocker blocker{selector};
                     selector->setCurrentIndex(year - minimumYear);
                   });

  const QSignalBlocker blocker{selector};
  selector->setCurrentIndex(calendar->yearShown() - minimumYear);
  return selector;
}

QString formatMoney(const double value) {
  return QLocale{QLocale::English, QLocale::UnitedStates}.toCurrencyString(
      value, "$", 2);
}

QTableWidgetItem *makeItem(const QString &text) {
  return std::make_unique<QTableWidgetItem>(text).release();
}

QString statusText(const bindings::BacktestOutcome outcome,
                   const bool selectableConditions) {
  const auto strategyName = selectableConditions
                                ? BacktestTab::tr("selectable strategy")
                                : BacktestTab::tr("starter order");
  switch (outcome) {
  case bindings::BacktestOutcome::filled:
    return BacktestTab::tr("Completed — %1 filled").arg(strategyName);
  case bindings::BacktestOutcome::rejectedInsufficientCash:
    return BacktestTab::tr("Completed — %1 rejected: insufficient cash")
        .arg(strategyName);
  case bindings::BacktestOutcome::cancelledNoFutureMarketData:
    return BacktestTab::tr("Completed — %1 cancelled: no future market data")
        .arg(strategyName);
  case bindings::BacktestOutcome::completedNoSignal:
    return BacktestTab::tr("Completed — no selectable condition matched");
  }
  return BacktestTab::tr("Completed");
}

enum class ConditionMetric : std::uint8_t {
  closeChangePercent,
  close,
  sma,
  ema,
  wma,
  rsi,
  macdHistogram,
  bollingerUpper,
  atr,
  adx,
  stochasticPercentK,
  donchianUpper,
  vwap,
  obv,
  roc,
  momentum,
  trueRange,
  volume,
};

void addConditionMetricItems(QComboBox *combo) {
  const auto add = [combo](const QString &text, const ConditionMetric metric) {
    combo->addItem(text, static_cast<int>(metric));
  };
  add(BacktestTab::tr("Close change %"), ConditionMetric::closeChangePercent);
  add(BacktestTab::tr("Close"), ConditionMetric::close);
  add(BacktestTab::tr("SMA"), ConditionMetric::sma);
  add(BacktestTab::tr("EMA"), ConditionMetric::ema);
  add(BacktestTab::tr("WMA"), ConditionMetric::wma);
  add(BacktestTab::tr("RSI"), ConditionMetric::rsi);
  add(BacktestTab::tr("MACD histogram"), ConditionMetric::macdHistogram);
  add(BacktestTab::tr("Bollinger upper"), ConditionMetric::bollingerUpper);
  add(BacktestTab::tr("ATR"), ConditionMetric::atr);
  add(BacktestTab::tr("ADX"), ConditionMetric::adx);
  add(BacktestTab::tr("Stochastic %K"), ConditionMetric::stochasticPercentK);
  add(BacktestTab::tr("Donchian upper"), ConditionMetric::donchianUpper);
  add(BacktestTab::tr("VWAP"), ConditionMetric::vwap);
  add(BacktestTab::tr("OBV"), ConditionMetric::obv);
  add(BacktestTab::tr("ROC"), ConditionMetric::roc);
  add(BacktestTab::tr("Momentum"), ConditionMetric::momentum);
  add(BacktestTab::tr("True range"), ConditionMetric::trueRange);
  add(BacktestTab::tr("Volume"), ConditionMetric::volume);
}

void addComparisonItems(QComboBox *combo) {
  const auto add = [combo](const QString &text,
                           const strategy::Comparison comparison) {
    combo->addItem(text, static_cast<int>(comparison));
  };
  add(BacktestTab::tr("is greater than"), strategy::Comparison::greaterThan);
  add(BacktestTab::tr("is at least"), strategy::Comparison::greaterThanOrEqual);
  add(BacktestTab::tr("is less than"), strategy::Comparison::lessThan);
  add(BacktestTab::tr("is at most"), strategy::Comparison::lessThanOrEqual);
  add(BacktestTab::tr("equals"), strategy::Comparison::equal);
  add(BacktestTab::tr("does not equal"), strategy::Comparison::notEqual);
}

strategy::Condition makeCondition(const ConditionMetric metric,
                                  const strategy::Comparison comparison,
                                  const double threshold, const int period) {
  auto condition = strategy::Condition{
      .source = strategy::ConditionSource::indicator,
      .comparison = comparison,
      .threshold = threshold,
      .indicator =
          {
              .period = period,
          },
  };
  switch (metric) {
  case ConditionMetric::closeChangePercent:
    condition.source = strategy::ConditionSource::closeChangePercent;
    condition.thresholdDomain = indicators::NumericDomain::percent;
    break;
  case ConditionMetric::close:
    condition.source = strategy::ConditionSource::barField;
    condition.barField = indicators::BarField::close;
    break;
  case ConditionMetric::sma:
    condition.indicator.kind = indicators::IndicatorKind::sma;
    break;
  case ConditionMetric::ema:
    condition.indicator.kind = indicators::IndicatorKind::ema;
    break;
  case ConditionMetric::wma:
    condition.indicator.kind = indicators::IndicatorKind::wma;
    break;
  case ConditionMetric::rsi:
    condition.indicator.kind = indicators::IndicatorKind::rsi;
    break;
  case ConditionMetric::macdHistogram: {
    const auto slowPeriod = std::max(period, 2);
    condition.indicator = {
        .kind = indicators::IndicatorKind::macd,
        .period = std::max(1, slowPeriod / 2),
        .secondaryPeriod = slowPeriod,
        .signalPeriod = std::max(1, slowPeriod / 3),
        .output = indicators::IndicatorOutput::histogram,
    };
    break;
  }
  case ConditionMetric::bollingerUpper:
    condition.indicator.kind = indicators::IndicatorKind::bollingerBands;
    condition.indicator.output = indicators::IndicatorOutput::upper;
    break;
  case ConditionMetric::atr:
    condition.indicator.kind = indicators::IndicatorKind::atr;
    break;
  case ConditionMetric::adx:
    condition.indicator.kind = indicators::IndicatorKind::adx;
    break;
  case ConditionMetric::stochasticPercentK:
    condition.indicator.kind = indicators::IndicatorKind::stochastic;
    condition.indicator.output = indicators::IndicatorOutput::percentK;
    break;
  case ConditionMetric::donchianUpper:
    condition.indicator.kind = indicators::IndicatorKind::donchian;
    condition.indicator.output = indicators::IndicatorOutput::upper;
    break;
  case ConditionMetric::vwap:
    condition.indicator.kind = indicators::IndicatorKind::vwap;
    break;
  case ConditionMetric::obv:
    condition.indicator.kind = indicators::IndicatorKind::obv;
    break;
  case ConditionMetric::roc:
    condition.indicator.kind = indicators::IndicatorKind::roc;
    break;
  case ConditionMetric::momentum:
    condition.indicator.kind = indicators::IndicatorKind::momentum;
    break;
  case ConditionMetric::trueRange:
    condition.indicator.kind = indicators::IndicatorKind::trueRange;
    break;
  case ConditionMetric::volume:
    condition.source = strategy::ConditionSource::barField;
    condition.barField = indicators::BarField::volume;
    break;
  }
  if (condition.source == strategy::ConditionSource::indicator) {
    condition.thresholdDomain =
        indicators::indicatorOutputDomain(condition.indicator);
  } else if (condition.source == strategy::ConditionSource::barField) {
    condition.thresholdDomain = indicators::indicatorOutputDomain({
        .kind = indicators::IndicatorKind::barField,
        .field = condition.barField,
    });
  }
  return condition;
}

strategy::ConditionLogic logicFrom(const QComboBox *combo) {
  return combo->currentIndex() == 0 ? strategy::ConditionLogic::all
                                    : strategy::ConditionLogic::any;
}

strategy::Comparison comparisonFrom(const QComboBox *combo) {
  return static_cast<strategy::Comparison>(combo->currentData().toInt());
}

ConditionMetric metricFrom(const QComboBox *combo) {
  return static_cast<ConditionMetric>(combo->currentData().toInt());
}

} // namespace

struct BacktestTab::RunState final {
  using BacktestResult = core::Result<bindings::BacktestSnapshot>;

  ~RunState() {
    cancellation.requestCancellation();
    watcher.waitForFinished();
  }

  RunState() = default;
  RunState(const RunState &) = delete;
  RunState &operator=(const RunState &) = delete;
  RunState(RunState &&) = delete;
  RunState &operator=(RunState &&) = delete;

  core::CancellationSource cancellation;
  QFutureWatcher<BacktestResult> watcher;
  bool selectableConditions = false;
};

BacktestTab::BacktestTab(QWidget *parent)
    : BacktestTab(
          [](const bindings::BacktestConfiguration &configuration,
             const core::CancellationToken &cancellation) {
            return bindings::runBacktestConfiguration(configuration,
                                                      cancellation);
          },
          parent) {}

BacktestTab::BacktestTab(BacktestRunner runner, QWidget *parent)
    : QWidget(parent), runState_(std::make_unique<RunState>()) {
  setObjectName("backtestTab");
  setAccessibleName("Backtest tab");
  setStyleSheet(R"(
    #backtestTab { background: #07111b; color: #dce9f5; }
    QGroupBox { border: 1px solid #28435e; border-radius: 6px; margin-top: 10px; padding-top: 12px; }
    QGroupBox::title { color: #8ed6df; left: 12px; padding: 0 5px; }
    QComboBox, QDateEdit, QDoubleSpinBox, QSpinBox, QTableWidget { background: #0d1d2c; border: 1px solid #34536e; color: #e8f1f8; padding: 5px; }
    QPushButton { background: #1e91a0; border: 0; border-radius: 4px; color: white; font-weight: 600; padding: 8px 18px; }
    QPushButton:hover { background: #27aaba; }
  )");

  auto *root = std::make_unique<QVBoxLayout>(this).release();
  root->setContentsMargins(18, 16, 18, 16);
  root->setSpacing(12);

  auto *title = makeLabel(tr("Backtest"), "backtestTitleLabel", this);
  auto titleFont = title->font();
  titleFont.setPointSize(18);
  titleFont.setBold(true);
  title->setFont(titleFont);
  root->addWidget(title);
  root->addWidget(makeLabel(tr("Starter orders and Selectable Conditions both "
                               "become eligible only at the next actual bar "
                               "open."),
                            "backtestScopeLabel", this));

  auto *configuration =
      std::make_unique<QGroupBox>(tr("Run configuration"), this).release();
  configuration->setObjectName("backtestConfigurationBox");
  configuration->setAccessibleName("Backtest run configuration");
  auto *form = std::make_unique<QGridLayout>(configuration).release();

  auto *symbol = std::make_unique<QComboBox>(configuration).release();
  symbol->setObjectName("backtestSymbolCombo");
  symbol->setAccessibleName("Backtest symbol");
  symbol->addItems({"AAPL", "MSFT", "NVDA"});

  auto *strategyCombo = std::make_unique<QComboBox>(configuration).release();
  strategyCombo->setObjectName("backtestStrategyCombo");
  strategyCombo->setAccessibleName("Backtest strategy");
  strategyCombo->addItem(tr("Starter market buy"));
  strategyCombo->addItem(tr("Selectable conditions"));

  auto *start =
      std::make_unique<QDateEdit>(QDate{2018, 5, 1}, configuration).release();
  start->setObjectName("backtestStartDateEdit");
  start->setAccessibleName("Backtest start date");
  start->setCalendarPopup(true);
  addYearSelector(start, "backtestStartYearCombo");

  auto *end =
      std::make_unique<QDateEdit>(QDate{2018, 5, 3}, configuration).release();
  end->setObjectName("backtestEndDateEdit");
  end->setAccessibleName("Backtest end date");
  end->setCalendarPopup(true);
  addYearSelector(end, "backtestEndYearCombo");

  auto *capital = std::make_unique<QDoubleSpinBox>(configuration).release();
  capital->setObjectName("backtestInitialCapitalSpinBox");
  capital->setAccessibleName("Backtest initial capital");
  capital->setRange(1.0, 1'000'000'000.0);
  capital->setDecimals(2);
  capital->setPrefix("$");
  capital->setValue(100'000.0);

  auto *quantity = std::make_unique<QSpinBox>(configuration).release();
  quantity->setObjectName("backtestQuantitySpinBox");
  quantity->setAccessibleName("Starter order quantity in shares");
  quantity->setRange(1, 1'000'000);
  quantity->setValue(10);

  auto *run = std::make_unique<QPushButton>(tr("Run Backtest"), configuration)
                  .release();
  run->setObjectName("backtestRunButton");
  run->setAccessibleName("Run backtest");
  run->setShortcut(QKeySequence{tr("Alt+R")});

  form->addWidget(makeLabel(tr("Symbol"), "backtestSymbolLabel", configuration),
                  0, 0);
  form->addWidget(symbol, 0, 1);
  form->addWidget(
      makeLabel(tr("Strategy"), "backtestStrategyLabel", configuration), 0, 2);
  form->addWidget(strategyCombo, 0, 3);
  form->addWidget(
      makeLabel(tr("Capital"), "backtestCapitalLabel", configuration), 0, 4);
  form->addWidget(capital, 0, 5);
  form->addWidget(makeLabel(tr("Start"), "backtestStartLabel", configuration),
                  1, 0);
  form->addWidget(start, 1, 1);
  form->addWidget(makeLabel(tr("End"), "backtestEndLabel", configuration), 1,
                  2);
  form->addWidget(end, 1, 3);
  form->addWidget(
      makeLabel(tr("Shares"), "backtestQuantityLabel", configuration), 1, 4);
  form->addWidget(quantity, 1, 5);
  form->addWidget(run, 0, 6, 2, 1);
  root->addWidget(configuration);

  auto *conditionEditor =
      std::make_unique<QGroupBox>(tr("Selectable conditions"), this).release();
  conditionEditor->setObjectName("backtestConditionEditorBox");
  conditionEditor->setAccessibleName("Selectable condition editor");
  auto *conditionLayout =
      std::make_unique<QGridLayout>(conditionEditor).release();

  auto *buyLogic = std::make_unique<QComboBox>(conditionEditor).release();
  buyLogic->setObjectName("backtestBuyLogicCombo");
  buyLogic->setAccessibleName("Buy condition logic");
  buyLogic->addItems({tr("Match ALL (AND)"), tr("Match ANY (OR)")});
  auto *buyMetric = std::make_unique<QComboBox>(conditionEditor).release();
  buyMetric->setObjectName("backtestBuyMetricCombo");
  buyMetric->setAccessibleName("Buy condition metric");
  addConditionMetricItems(buyMetric);
  auto *buyComparison = std::make_unique<QComboBox>(conditionEditor).release();
  buyComparison->setObjectName("backtestBuyComparisonCombo");
  buyComparison->setAccessibleName("Buy condition comparison");
  addComparisonItems(buyComparison);
  auto *buyThreshold =
      std::make_unique<QDoubleSpinBox>(conditionEditor).release();
  buyThreshold->setObjectName("backtestBuyThresholdSpinBox");
  buyThreshold->setAccessibleName("Buy condition threshold");
  buyThreshold->setRange(-1'000'000'000.0, 1'000'000'000.0);
  buyThreshold->setDecimals(4);
  buyThreshold->setValue(5.0);
  auto *buyPeriod = std::make_unique<QSpinBox>(conditionEditor).release();
  buyPeriod->setObjectName("backtestBuyPeriodSpinBox");
  buyPeriod->setAccessibleName("Buy indicator period");
  buyPeriod->setToolTip(
      tr("Indicator period; MACD uses this as its slow period."));
  buyPeriod->setRange(1, indicators::maximumIndicatorPeriod);
  buyPeriod->setValue(14);
  auto *buySecondEnabled =
      std::make_unique<QCheckBox>(tr("Add condition"), conditionEditor)
          .release();
  buySecondEnabled->setObjectName("backtestBuySecondEnabledCheckBox");
  buySecondEnabled->setAccessibleName("Add second buy condition");
  auto *buySecondMetric =
      std::make_unique<QComboBox>(conditionEditor).release();
  buySecondMetric->setObjectName("backtestBuySecondMetricCombo");
  buySecondMetric->setAccessibleName("Second buy condition metric");
  addConditionMetricItems(buySecondMetric);
  buySecondMetric->setCurrentText(tr("SMA"));
  auto *buySecondComparison =
      std::make_unique<QComboBox>(conditionEditor).release();
  buySecondComparison->setObjectName("backtestBuySecondComparisonCombo");
  buySecondComparison->setAccessibleName("Second buy condition comparison");
  addComparisonItems(buySecondComparison);
  auto *buySecondThreshold =
      std::make_unique<QDoubleSpinBox>(conditionEditor).release();
  buySecondThreshold->setObjectName("backtestBuySecondThresholdSpinBox");
  buySecondThreshold->setAccessibleName("Second buy condition threshold");
  buySecondThreshold->setRange(-1'000'000'000.0, 1'000'000'000.0);
  buySecondThreshold->setDecimals(4);
  buySecondThreshold->setValue(0.0);
  auto *buySecondPeriod = std::make_unique<QSpinBox>(conditionEditor).release();
  buySecondPeriod->setObjectName("backtestBuySecondPeriodSpinBox");
  buySecondPeriod->setAccessibleName("Second buy indicator period");
  buySecondPeriod->setToolTip(
      tr("Indicator period; MACD uses this as its slow period."));
  buySecondPeriod->setRange(1, indicators::maximumIndicatorPeriod);
  buySecondPeriod->setValue(14);

  auto *sellLogic = std::make_unique<QComboBox>(conditionEditor).release();
  sellLogic->setObjectName("backtestSellLogicCombo");
  sellLogic->setAccessibleName("Sell condition logic");
  sellLogic->addItems({tr("Match ALL (AND)"), tr("Match ANY (OR)")});
  auto *sellMetric = std::make_unique<QComboBox>(conditionEditor).release();
  sellMetric->setObjectName("backtestSellMetricCombo");
  sellMetric->setAccessibleName("Sell condition metric");
  addConditionMetricItems(sellMetric);
  auto *sellComparison = std::make_unique<QComboBox>(conditionEditor).release();
  sellComparison->setObjectName("backtestSellComparisonCombo");
  sellComparison->setAccessibleName("Sell condition comparison");
  addComparisonItems(sellComparison);
  sellComparison->setCurrentIndex(2);
  auto *sellThreshold =
      std::make_unique<QDoubleSpinBox>(conditionEditor).release();
  sellThreshold->setObjectName("backtestSellThresholdSpinBox");
  sellThreshold->setAccessibleName("Sell condition threshold");
  sellThreshold->setRange(-1'000'000'000.0, 1'000'000'000.0);
  sellThreshold->setDecimals(4);
  sellThreshold->setValue(-5.0);
  auto *sellPeriod = std::make_unique<QSpinBox>(conditionEditor).release();
  sellPeriod->setObjectName("backtestSellPeriodSpinBox");
  sellPeriod->setAccessibleName("Sell indicator period");
  sellPeriod->setToolTip(
      tr("Indicator period; MACD uses this as its slow period."));
  sellPeriod->setRange(1, indicators::maximumIndicatorPeriod);
  sellPeriod->setValue(14);
  auto *sellSecondEnabled =
      std::make_unique<QCheckBox>(tr("Add condition"), conditionEditor)
          .release();
  sellSecondEnabled->setObjectName("backtestSellSecondEnabledCheckBox");
  sellSecondEnabled->setAccessibleName("Add second sell condition");
  auto *sellSecondMetric =
      std::make_unique<QComboBox>(conditionEditor).release();
  sellSecondMetric->setObjectName("backtestSellSecondMetricCombo");
  sellSecondMetric->setAccessibleName("Second sell condition metric");
  addConditionMetricItems(sellSecondMetric);
  sellSecondMetric->setCurrentText(tr("SMA"));
  auto *sellSecondComparison =
      std::make_unique<QComboBox>(conditionEditor).release();
  sellSecondComparison->setObjectName("backtestSellSecondComparisonCombo");
  sellSecondComparison->setAccessibleName("Second sell condition comparison");
  addComparisonItems(sellSecondComparison);
  auto *sellSecondThreshold =
      std::make_unique<QDoubleSpinBox>(conditionEditor).release();
  sellSecondThreshold->setObjectName("backtestSellSecondThresholdSpinBox");
  sellSecondThreshold->setAccessibleName("Second sell condition threshold");
  sellSecondThreshold->setRange(-1'000'000'000.0, 1'000'000'000.0);
  sellSecondThreshold->setDecimals(4);
  sellSecondThreshold->setValue(0.0);
  auto *sellSecondPeriod =
      std::make_unique<QSpinBox>(conditionEditor).release();
  sellSecondPeriod->setObjectName("backtestSellSecondPeriodSpinBox");
  sellSecondPeriod->setAccessibleName("Second sell indicator period");
  sellSecondPeriod->setToolTip(
      tr("Indicator period; MACD uses this as its slow period."));
  sellSecondPeriod->setRange(1, indicators::maximumIndicatorPeriod);
  sellSecondPeriod->setValue(14);

  conditionLayout->addWidget(
      makeLabel(tr("Buy logic"), "backtestBuyLogicLabel", conditionEditor), 0,
      0);
  conditionLayout->addWidget(buyLogic, 0, 1);
  conditionLayout->addWidget(buyMetric, 0, 2);
  conditionLayout->addWidget(buyComparison, 0, 3);
  conditionLayout->addWidget(buyThreshold, 0, 4);
  conditionLayout->addWidget(
      makeLabel(tr("Period"), "backtestBuyPeriodLabel", conditionEditor), 0, 5);
  conditionLayout->addWidget(buyPeriod, 0, 6);
  conditionLayout->addWidget(buySecondEnabled, 1, 1);
  conditionLayout->addWidget(buySecondMetric, 1, 2);
  conditionLayout->addWidget(buySecondComparison, 1, 3);
  conditionLayout->addWidget(buySecondThreshold, 1, 4);
  conditionLayout->addWidget(buySecondPeriod, 1, 6);

  conditionLayout->addWidget(
      makeLabel(tr("Sell logic"), "backtestSellLogicLabel", conditionEditor), 2,
      0);
  conditionLayout->addWidget(sellLogic, 2, 1);
  conditionLayout->addWidget(sellMetric, 2, 2);
  conditionLayout->addWidget(sellComparison, 2, 3);
  conditionLayout->addWidget(sellThreshold, 2, 4);
  conditionLayout->addWidget(
      makeLabel(tr("Period"), "backtestSellPeriodLabel", conditionEditor), 2,
      5);
  conditionLayout->addWidget(sellPeriod, 2, 6);
  conditionLayout->addWidget(sellSecondEnabled, 3, 1);
  conditionLayout->addWidget(sellSecondMetric, 3, 2);
  conditionLayout->addWidget(sellSecondComparison, 3, 3);
  conditionLayout->addWidget(sellSecondThreshold, 3, 4);
  conditionLayout->addWidget(sellSecondPeriod, 3, 6);
  conditionEditor->setVisible(false);
  root->addWidget(conditionEditor);

  const auto setSecondConditionEnabled =
      [](const bool enabled, QComboBox *metric, QComboBox *comparison,
         QDoubleSpinBox *threshold, QSpinBox *period) {
        metric->setEnabled(enabled);
        comparison->setEnabled(enabled);
        threshold->setEnabled(enabled);
        period->setEnabled(enabled);
      };
  setSecondConditionEnabled(false, buySecondMetric, buySecondComparison,
                            buySecondThreshold, buySecondPeriod);
  setSecondConditionEnabled(false, sellSecondMetric, sellSecondComparison,
                            sellSecondThreshold, sellSecondPeriod);
  QObject::connect(
      buySecondEnabled, &QCheckBox::toggled, this, [=](const bool enabled) {
        setSecondConditionEnabled(enabled, buySecondMetric, buySecondComparison,
                                  buySecondThreshold, buySecondPeriod);
      });
  QObject::connect(
      sellSecondEnabled, &QCheckBox::toggled, this, [=](const bool enabled) {
        setSecondConditionEnabled(enabled, sellSecondMetric,
                                  sellSecondComparison, sellSecondThreshold,
                                  sellSecondPeriod);
      });
  QObject::connect(
      strategyCombo, qOverload<int>(&QComboBox::currentIndexChanged), this,
      [=](const int index) { conditionEditor->setVisible(index == 1); });

  auto *status = makeLabel(tr("Ready to run"), "backtestStatusLabel", this);
  root->addWidget(status);

  auto *summary =
      std::make_unique<QGroupBox>(tr("Result summary"), this).release();
  summary->setObjectName("backtestSummaryBox");
  summary->setAccessibleName("Backtest result summary");
  auto *summaryLayout = std::make_unique<QGridLayout>(summary).release();
  auto *cash = makeLabel(tr("Cash: --"), "backtestCashLabel", summary);
  auto *position =
      makeLabel(tr("Position: --"), "backtestPositionLabel", summary);
  auto *market =
      makeLabel(tr("Market value: --"), "backtestMarketValueLabel", summary);
  auto *equity = makeLabel(tr("Equity: --"), "backtestEquityLabel", summary);
  auto *pnl = makeLabel(tr("PnL: --"), "backtestPnlLabel", summary);
  auto *bars = makeLabel(tr("Bars: --"), "backtestBarsLabel", summary);
  summaryLayout->addWidget(cash, 0, 0);
  summaryLayout->addWidget(position, 0, 1);
  summaryLayout->addWidget(market, 0, 2);
  summaryLayout->addWidget(equity, 1, 0);
  summaryLayout->addWidget(pnl, 1, 1);
  summaryLayout->addWidget(bars, 1, 2);
  root->addWidget(summary);

  auto *tradeLog = std::make_unique<QTableWidget>(0, 5, this).release();
  tradeLog->setObjectName("backtestTradeLogTable");
  tradeLog->setAccessibleName("Backtest fill log");
  tradeLog->setHorizontalHeaderLabels(
      {tr("Time"), tr("Side"), tr("Price"), tr("Shares"), tr("Amount")});
  tradeLog->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
  tradeLog->verticalHeader()->hide();
  tradeLog->setEditTriggers(QAbstractItemView::NoEditTriggers);
  tradeLog->setSelectionMode(QAbstractItemView::NoSelection);
  root->addWidget(tradeLog, 1);

  auto *watcher = &runState_->watcher;

  QObject::connect(
      watcher, &QFutureWatcher<RunState::BacktestResult>::finished, this,
      [this, run, watcher, status, cash, position, market, equity, pnl, bars,
       tradeLog]() {
        run->setEnabled(true);
        const auto result = watcher->result();
        if (!result.ok()) {
          setLabelText(
              status, tr("Cannot run — %1")
                          .arg(QString::fromStdString(result.error().message)));
          return;
        }

        const auto &snapshot = result.value();
        setLabelText(status, statusText(snapshot.outcome,
                                        runState_->selectableConditions));
        setLabelText(cash, tr("Cash: %1").arg(formatMoney(snapshot.cash)));
        setLabelText(position,
                     tr("Position: %1 shares").arg(snapshot.positionShares));
        setLabelText(
            market,
            tr("Market value: %1").arg(formatMoney(snapshot.marketValue)));
        setLabelText(equity,
                     tr("Equity: %1").arg(formatMoney(snapshot.equity)));
        setLabelText(pnl, tr("PnL: %1").arg(formatMoney(snapshot.pnl)));
        setLabelText(bars, tr("Bars: %1").arg(snapshot.barsProcessed));

        for (const auto &fill : snapshot.fills) {
          const auto milliseconds =
              std::chrono::duration_cast<std::chrono::milliseconds>(
                  fill.timestamp.time_since_epoch())
                  .count();
          const auto row = tradeLog->rowCount();
          tradeLog->insertRow(row);
          tradeLog->setItem(row, 0,
                            makeItem(QDateTime::fromMSecsSinceEpoch(
                                         milliseconds, QTimeZone::UTC)
                                         .toString(Qt::ISODate)));
          tradeLog->setItem(
              row, 1,
              makeItem(fill.side == bindings::BacktestFillSide::buy
                           ? tr("Buy")
                           : tr("Sell")));
          tradeLog->setItem(
              row, 2, makeItem(QString{"$%1"}.arg(fill.price, 0, 'f', 4)));
          tradeLog->setItem(row, 3,
                            makeItem(QString::number(fill.quantityShares)));
          tradeLog->setItem(row, 4, makeItem(formatMoney(fill.amount)));
        }
      });

  QObject::connect(
      run, &QPushButton::clicked, this,
      [this, tradeLog, status, cash, position, market, equity, pnl, bars, run,
       symbol, strategyCombo, start, end, capital, quantity, watcher, buyLogic,
       buyMetric, buyComparison, buyThreshold, buyPeriod, buySecondEnabled,
       buySecondMetric, buySecondComparison, buySecondThreshold,
       buySecondPeriod, sellLogic, sellMetric, sellComparison, sellThreshold,
       sellPeriod, sellSecondEnabled, sellSecondMetric, sellSecondComparison,
       sellSecondThreshold, sellSecondPeriod, runner = std::move(runner)]() {
        tradeLog->setRowCount(0);
        setLabelText(status, tr("Running…"));
        setLabelText(cash, tr("Cash: --"));
        setLabelText(position, tr("Position: --"));
        setLabelText(market, tr("Market value: --"));
        setLabelText(equity, tr("Equity: --"));
        setLabelText(pnl, tr("PnL: --"));
        setLabelText(bars, tr("Bars: --"));
        run->setEnabled(false);

        auto configuration = bindings::BacktestConfiguration{
            .symbol = symbol->currentText(),
            .schema = "ohlcv-1h",
            .startDate = start->date(),
            .endDate = end->date(),
            .initialCapital = capital->value(),
            .quantityShares = quantity->value(),
            .selectableStrategy = {},
        };
        if (strategyCombo->currentIndex() == 1) {
          const auto makeGroup = [](const QComboBox *logic,
                                    const QComboBox *metric,
                                    const QComboBox *comparison,
                                    const QDoubleSpinBox *threshold,
                                    const QSpinBox *period,
                                    const QCheckBox *secondEnabled,
                                    const QComboBox *secondMetric,
                                    const QComboBox *secondComparison,
                                    const QDoubleSpinBox *secondThreshold,
                                    const QSpinBox *secondPeriod) {
            auto group = strategy::ConditionGroup{
                .logic = logicFrom(logic),
                .conditions = {makeCondition(
                    metricFrom(metric), comparisonFrom(comparison),
                    threshold->value(), period->value())},
            };
            if (secondEnabled->isChecked()) {
              group.conditions.push_back(makeCondition(
                  metricFrom(secondMetric), comparisonFrom(secondComparison),
                  secondThreshold->value(), secondPeriod->value()));
            }
            return group;
          };
          configuration.selectableStrategy = strategy::SelectableStrategyPlan{
              .buy = makeGroup(buyLogic, buyMetric, buyComparison, buyThreshold,
                               buyPeriod, buySecondEnabled, buySecondMetric,
                               buySecondComparison, buySecondThreshold,
                               buySecondPeriod),
              .sell = makeGroup(sellLogic, sellMetric, sellComparison,
                                sellThreshold, sellPeriod, sellSecondEnabled,
                                sellSecondMetric, sellSecondComparison,
                                sellSecondThreshold, sellSecondPeriod),
          };
        }
        runState_->selectableConditions =
            configuration.selectableStrategy.has_value();
        runState_->cancellation = core::CancellationSource{};
        const auto cancellation = runState_->cancellation.token();
        watcher->setFuture(
            QtConcurrent::run([runner, configuration, cancellation]() mutable {
              try {
                return runner(configuration, cancellation);
              } catch (const std::exception &error) {
                return core::Result<bindings::BacktestSnapshot>{core::makeError(
                    core::ErrorCode::internal,
                    std::string{"backtest worker failed: "} + error.what())};
              } catch (...) {
                return core::Result<bindings::BacktestSnapshot>{core::makeError(
                    core::ErrorCode::internal, "backtest worker failed")};
              }
            }));
      });
}

BacktestTab::~BacktestTab() = default;

} // namespace bte::frontend
