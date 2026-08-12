#include "Bte/Frontend/BacktestTab.h"

#include "Bte/Bindings/BacktestSessionVm.h"

#include <QAbstractItemView>
#include <QCalendarWidget>
#include <QComboBox>
#include <QDateEdit>
#include <QDateTime>
#include <QDoubleSpinBox>
#include <QFutureWatcher>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeySequence>
#include <QLabel>
#include <QLocale>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTimeZone>
#include <QToolButton>
#include <QVBoxLayout>
#include <QtConcurrentRun>

#include <chrono>
#include <exception>
#include <memory>
#include <string>
#include <utility>

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

QString statusText(const bindings::BacktestOutcome outcome) {
  switch (outcome) {
  case bindings::BacktestOutcome::filled:
    return BacktestTab::tr("Completed — starter order filled");
  case bindings::BacktestOutcome::rejectedInsufficientCash:
    return BacktestTab::tr(
        "Completed — starter order rejected: insufficient cash");
  case bindings::BacktestOutcome::cancelledNoFutureMarketData:
    return BacktestTab::tr(
        "Completed — starter order cancelled: no future market data");
  }
  return BacktestTab::tr("Completed");
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
  root->addWidget(makeLabel(tr("Starter slice: one market buy submitted on the "
                               "first bar and eligible "
                               "at the next actual bar open."),
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

  auto *strategy = std::make_unique<QComboBox>(configuration).release();
  strategy->setObjectName("backtestStrategyCombo");
  strategy->setAccessibleName("Backtest strategy");
  strategy->addItem(tr("Starter market buy"));

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
  form->addWidget(strategy, 0, 3);
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
      [=]() {
        run->setEnabled(true);
        const auto result = watcher->result();
        if (!result.ok()) {
          setLabelText(
              status, tr("Cannot run — %1")
                          .arg(QString::fromStdString(result.error().message)));
          return;
        }

        const auto &snapshot = result.value();
        setLabelText(status, statusText(snapshot.outcome));
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

        if (!snapshot.fill.has_value()) {
          return;
        }
        const auto milliseconds =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                snapshot.fill->timestamp.time_since_epoch())
                .count();
        tradeLog->insertRow(0);
        tradeLog->setItem(0, 0,
                          makeItem(QDateTime::fromMSecsSinceEpoch(
                                       milliseconds, QTimeZone::UTC)
                                       .toString(Qt::ISODate)));
        tradeLog->setItem(0, 1, makeItem(tr("Buy")));
        tradeLog->setItem(
            0, 2,
            makeItem(QString{"$%1"}.arg(snapshot.fill->price, 0, 'f', 4)));
        tradeLog->setItem(
            0, 3, makeItem(QString::number(snapshot.fill->quantityShares)));
        tradeLog->setItem(0, 4, makeItem(formatMoney(snapshot.fill->amount)));
      });

  QObject::connect(
      run, &QPushButton::clicked, this,
      [this, tradeLog, status, cash, position, market, equity, pnl, bars, run,
       symbol, start, end, capital, quantity, watcher,
       runner = std::move(runner)]() {
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
        };
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
