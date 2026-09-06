#include "Bte/Frontend/ReplayTab.h"

// IWYU pragma: no_include "Bte/Results/ResultStore.h"

#include "Bte/Bindings/ReplayDataLoader.h"
#include "Bte/Bindings/ReplaySessionVm.h"
#include "Bte/Bindings/ResultReplay.h"
#include "Bte/Core/Cancellation.h"
#include "Bte/Core/Result.h"
#include "Bte/Frontend/IChartView.h"
#include "Bte/Frontend/QtChartsCandlestickView.h"

#include "ReplayTabSections.h"
#include "ReplayTabStyle.h"

#include <QComboBox>
#include <QDateEdit>
#include <QDateTime>
#include <QDoubleSpinBox>
#include <QFrame>
#include <QFuture>
#include <QFutureWatcher>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLocale>
#include <QObject>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QShortcut>
#include <QStandardItemModel>
#include <QStandardPaths>
#include <QString>
#include <QTableWidget>
#include <QTimer>
#include <QToolButton>
#include <QVariant>
#include <QtConcurrentRun>
#include <QtCore/Qt>
#include <QtCore/qabstractitemmodel.h>
#include <QtCore/qtenvironmentvariables.h>
#include <QtCore/qtimezone.h>
#include <QtGui/qkeysequence.h>
#include <QtWidgets/qslider.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "Bte/Core/Bar.h"

namespace bte::frontend {
using bte::bindings::loadReplayBars;
using bte::bindings::ReplaySessionVm;

namespace {

int timerIntervalForSpeed(const QString &speed) {
  if (speed == "5x") {
    return 200;
  }
  if (speed == "10x") {
    return 100;
  }
  if (speed == "max") {
    return 0;
  }
  return 1000;
}

QString formatMoney(const double value) {
  return QLocale{QLocale::English, QLocale::UnitedStates}.toCurrencyString(
      value, "$");
}

QString formatPrice(const double value) {
  return QString{"$%1"}.arg(value, 0, 'f', 2);
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): distinct inputs
std::filesystem::path defaultApplicationStore(const char *environmentName,
                                              const char *leaf) {
  const auto configured = qEnvironmentVariable(environmentName);
  if (!configured.isEmpty()) {
    return std::filesystem::path{configured.toStdString()};
  }
  return std::filesystem::path{QStandardPaths::writableLocation(
                                   QStandardPaths::AppLocalDataLocation)
                                   .toStdString()} /
         leaf;
}

QTableWidgetItem *makeItem(const QString &text) {
  return std::make_unique<QTableWidgetItem>(text).release();
}

} // namespace

struct ReplayTab::State final {
  using OpenResult = core::Result<std::shared_ptr<bindings::ResultReplay>>;
  using CatalogResult = core::Result<std::vector<results::ResultSummary>>;

  ~State() {
    cancellation.requestCancellation();
    for (auto &future : openFutures) {
      future.waitForFinished();
    }
    catalogFuture.waitForFinished();
  }
  State() = default;
  State(const State &) = delete;
  State &operator=(const State &) = delete;
  State(State &&) = delete;
  State &operator=(State &&) = delete;

  std::filesystem::path resultStore;
  std::filesystem::path dataStore;
  core::CancellationSource cancellation;
  std::uint64_t generation = 0;
  std::shared_ptr<bindings::ResultReplay> resultReplay;
  std::vector<QFuture<OpenResult>> openFutures;
  QFuture<CatalogResult> catalogFuture;
  std::function<void(const QString &)> requestOpen;
  bool resultMode = false;
  bool resultLoadPending = false;
  bool resultRequestMade = false;
};

ReplayTab::ReplayTab(QWidget *parent)
    : ReplayTab(defaultApplicationStore("BTE_RESULT_STORE", "Results"),
                defaultApplicationStore("BTE_DATA_STORE", "Data"), parent) {}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): Qt wiring
ReplayTab::ReplayTab(std::filesystem::path resultStore,
                     std::filesystem::path dataStore, QWidget *parent)
    : QWidget(parent), state_(std::make_unique<State>()) {
  state_->resultStore = std::move(resultStore);
  state_->dataStore = std::move(dataStore);
  setObjectName("replayTab");
  setAccessibleName("K-line replay tab");
  setStyleSheet(replayTabStyleSheet());

  auto *outer = std::make_unique<QVBoxLayout>(this).release();
  outer->setContentsMargins(0, 0, 0, 0);
  outer->setSpacing(0);

  auto *scrollArea = std::make_unique<QScrollArea>(this).release();
  scrollArea->setObjectName("replayScrollArea");
  scrollArea->setAccessibleName("Replay scroll area");
  scrollArea->setWidgetResizable(true);
  scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  scrollArea->setFrameShape(QFrame::NoFrame);

  auto *content = std::make_unique<QWidget>(scrollArea).release();
  content->setObjectName("replayScrollContent");
  content->setAccessibleName("Replay scroll content");
  scrollArea->setWidget(content);
  outer->addWidget(scrollArea);

  auto *root = std::make_unique<QVBoxLayout>(content).release();
  root->setContentsMargins(14, 12, 14, 12);
  root->setSpacing(8);

  auto headerRowOwner = std::make_unique<QHBoxLayout>();
  auto *headerRow = headerRowOwner.get();
  auto headerTextOwner = std::make_unique<QVBoxLayout>();
  auto *headerText = headerTextOwner.get();
  headerText->setSpacing(2);
  auto *title = std::make_unique<QLabel>(tr("K-line Replay"), this).release();
  title->setObjectName("replayTitleLabel");
  title->setAccessibleName("K-line Replay");
  headerText->addWidget(title);
  headerRow->addLayout(headerTextOwner.release());
  headerRow->addStretch(1);
  root->addLayout(headerRowOwner.release());

  const auto setup = makeReplaySetupControls(this);
  root->addWidget(setup.box);

  const auto playback = makeReplayPlaybackControls(this);
  root->addWidget(playback.bar);

  const auto chart = makeReplayChartSection(this);
  root->addWidget(chart.panel, 1);

  const auto portfolio = makeReplayPortfolioSection(this);
  root->addWidget(portfolio.box);

  const auto tradeLog = makeReplayTradeLogSection(this);
  root->addWidget(tradeLog.panel);

  auto replaySession = std::make_shared<ReplaySessionVm>();
  auto *replayTimer = std::make_unique<QTimer>(this).release();
  replayTimer->setObjectName("replayPlaybackTimer");
  replayTimer->setTimerType(Qt::PreciseTimer);
  replayTimer->setInterval(
      timerIntervalForSpeed(playback.speedCombo->currentText()));

  const auto updateLegacyPortfolio = [=]() {
    const auto snapshot = replaySession->portfolioSnapshot();
    portfolio.cashLabel->setText(
        tr("Cash: %1").arg(formatMoney(snapshot.cash)));
    portfolio.positionLabel->setText(
        tr("Position: %1").arg(snapshot.position, 0, 'f', 0));
    portfolio.marketValueLabel->setText(
        tr("Market: %1").arg(formatMoney(snapshot.marketValue)));
    portfolio.equityLabel->setText(
        tr("Equity: %1").arg(formatMoney(snapshot.equity)));
    portfolio.pnlLabel->setText(tr("PnL: %1").arg(formatMoney(snapshot.pnl)));
    portfolio.lastPriceLabel->setText(
        snapshot.hasLastPrice
            ? tr("Last: %1").arg(formatPrice(snapshot.lastPrice))
            : tr("Last: --"));
    portfolio.barIndexLabel->setText(tr("Bar: %1/%2")
                                         .arg(replaySession->currentIndex())
                                         .arg(replaySession->totalBars()));
  };

  const auto updateLegacyProgress = [=]() {
    playback.progress->setValue(replaySession->progressPercent());
  };

  const auto setPlaying = [=](const bool isPlaying) {
    playback.playPauseButton->setText(isPlaying ? tr("Pause") : tr("Play"));
  };

  const auto resetVisibleReplay = [=]() {
    chart.chartView->setBarWindow(replaySession->visibleBars());
    chart.chartView->clearMarkers();
    playback.seekSlider->setRange(0, 0);
    updateLegacyProgress();
    updateLegacyPortfolio();
  };

  const auto stopPlayback = [=]() {
    replayTimer->stop();
    setPlaying(false);
  };

  const auto renderResult = [this, chart, playback, tradeLog, setup,
                             portfolio]() {
    if (state_->resultReplay == nullptr) {
      chart.chartView->setBarWindow({});
      chart.chartView->clearMarkers();
      playback.progress->setValue(0);
      playback.seekSlider->setRange(0, 0);
      tradeLog.table->setRowCount(0);
      return;
    }
    const auto frames = state_->resultReplay->visibleWindow();
    std::vector<core::Bar> bars;
    std::vector<ChartMarker> markers;
    bars.reserve(frames.size());
    for (const auto &frame : frames) {
      bars.push_back(frame.candle);
      for (const auto &fill : frame.fills) {
        markers.push_back({.timestamp = fill.timestamp,
                           .price = fill.price,
                           .isBuy = fill.isBuy});
      }
    }
    chart.chartView->setBarWindow(bars);
    chart.chartView->setMarkers(markers);
    playback.progress->setValue(state_->resultReplay->progressPercent());
    const auto *frame = state_->resultReplay->current();
    if (frame == nullptr) {
      playback.seekSlider->setRange(0, 0);
      setup.resultStatusLabel->setText(tr("Result is valid but empty"));
      return;
    }
    playback.seekSlider->setRange(
        0, static_cast<int>(state_->resultReplay->totalFrames() - 1));
    playback.seekSlider->setValue(
        static_cast<int>(state_->resultReplay->currentIndex()));
    portfolio.cashLabel->setText(
        tr("Cash: %1").arg(formatMoney(frame->portfolio.cash)));
    portfolio.positionLabel->setText(
        tr("Position: %1").arg(frame->portfolio.positionShares));
    portfolio.marketValueLabel->setText(
        tr("Market: %1").arg(formatMoney(frame->portfolio.marketValue)));
    portfolio.equityLabel->setText(
        tr("Equity: %1").arg(formatMoney(frame->portfolio.equity)));
    portfolio.pnlLabel->setText(
        tr("PnL: %1").arg(formatMoney(frame->portfolio.pnl)));
    portfolio.lastPriceLabel->setText(
        tr("Last: %1").arg(formatPrice(frame->candle.close)));
    portfolio.barIndexLabel->setText(
        tr("Bar: %1/%2")
            .arg(state_->resultReplay->currentIndex() + 1)
            .arg(state_->resultReplay->totalFrames()));
    setup.partialLabel->setVisible(frame->partialUtcDay);

    tradeLog.table->setRowCount(0);
    for (const auto &visibleFrame : frames) {
      for (const auto &fill : visibleFrame.fills) {
        const auto milliseconds =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                fill.timestamp.time_since_epoch())
                .count();
        const auto row = tradeLog.table->rowCount();
        tradeLog.table->insertRow(row);
        tradeLog.table->setItem(row, 0,
                                makeItem(QDateTime::fromMSecsSinceEpoch(
                                             milliseconds, QTimeZone::UTC)
                                             .toString(Qt::ISODate)));
        tradeLog.table->setItem(row, 1,
                                makeItem(fill.isBuy ? tr("Buy") : tr("Sell")));
        tradeLog.table->setItem(row, 2, makeItem(formatPrice(fill.price)));
        tradeLog.table->setItem(row, 3,
                                makeItem(QString::number(fill.quantityShares)));
        tradeLog.table->setItem(row, 4, makeItem(formatMoney(fill.amount)));
        tradeLog.table->setItem(row, 5, makeItem(QString{}));
      }
    }
  };

  const auto advanceOneBar = [this, stopPlayback, renderResult, replaySession,
                              chart, updateLegacyProgress,
                              updateLegacyPortfolio]() {
    if (state_->resultMode) {
      if (state_->resultReplay == nullptr ||
          !state_->resultReplay->stepForward()) {
        stopPlayback();
        return;
      }
      renderResult();
      if (state_->resultReplay->currentIndex() + 1 >=
          state_->resultReplay->totalFrames()) {
        stopPlayback();
      }
      return;
    }
    if (!replaySession->stepForward()) {
      stopPlayback();
      return;
    }
    chart.chartView->appendBar(replaySession->visibleBars().back());
    updateLegacyProgress();
    updateLegacyPortfolio();
    if (replaySession->currentIndex() >= replaySession->totalBars()) {
      stopPlayback();
    }
  };

  const auto reloadBars = [this, stopPlayback, setup, tradeLog, replaySession,
                           resetVisibleReplay]() {
    stopPlayback();
    state_->cancellation.requestCancellation();
    ++state_->generation;
    state_->resultMode = false;
    state_->resultLoadPending = false;
    state_->resultRequestMade = false;
    state_->resultReplay.reset();
    setup.partialLabel->setVisible(false);
    setup.resultStatusLabel->setText(tr("Bar-only preview"));
    tradeLog.table->setRowCount(0);
    replaySession->setInitialCapital(setup.initialCapital->value());
    replaySession->reset(loadReplayBars(
        setup.symbolCombo->currentText(), setup.schemaCombo->currentText(),
        setup.startDate->date(), setup.endDate->date()));
    resetVisibleReplay();
  };

  QObject::connect(playback.stepForwardButton, &QToolButton::clicked, this,
                   advanceOneBar);
  QObject::connect(playback.stepBackButton, &QToolButton::clicked, this,
                   [this, stopPlayback, renderResult, replaySession, chart,
                    updateLegacyProgress, updateLegacyPortfolio]() {
                     stopPlayback();
                     if (state_->resultMode) {
                       if (state_->resultReplay != nullptr &&
                           state_->resultReplay->stepBack()) {
                         renderResult();
                       }
                       return;
                     }
                     if (!replaySession->stepBack()) {
                       return;
                     }
                     chart.chartView->setBarWindow(
                         replaySession->visibleBars());
                     updateLegacyProgress();
                     updateLegacyPortfolio();
                   });
  QObject::connect(
      playback.playPauseButton, &QToolButton::clicked, this,
      [this, replayTimer, stopPlayback, renderResult, setPlaying, advanceOneBar,
       playback, replaySession, setup, resetVisibleReplay]() {
        if (replayTimer->isActive()) {
          stopPlayback();
          return;
        }
        if (state_->resultMode) {
          if (state_->resultReplay == nullptr ||
              state_->resultReplay->totalFrames() == 0U) {
            return;
          }
          if (state_->resultReplay->currentIndex() + 1 >=
              state_->resultReplay->totalFrames()) {
            (void)state_->resultReplay->seek(0);
            renderResult();
          }
          setPlaying(true);
          advanceOneBar();
          if (state_->resultReplay->currentIndex() + 1 <
              state_->resultReplay->totalFrames()) {
            replayTimer->start(
                timerIntervalForSpeed(playback.speedCombo->currentText()));
          }
          return;
        }
        if (replaySession->totalBars() == 0U) {
          return;
        }
        if (replaySession->currentIndex() >= replaySession->totalBars()) {
          replaySession->reset(loadReplayBars(setup.symbolCombo->currentText(),
                                              setup.schemaCombo->currentText(),
                                              setup.startDate->date(),
                                              setup.endDate->date()));
          resetVisibleReplay();
        }
        setPlaying(true);
        advanceOneBar();
        if (replaySession->currentIndex() < replaySession->totalBars()) {
          replayTimer->start(
              timerIntervalForSpeed(playback.speedCombo->currentText()));
        }
      });
  QObject::connect(replayTimer, &QTimer::timeout, this, advanceOneBar);
  const auto addShortcut = [this](const QKeySequence &sequence,
                                  QToolButton *button) {
    auto *shortcut = std::make_unique<QShortcut>(sequence, this).release();
    shortcut->setContext(Qt::WidgetWithChildrenShortcut);
    QObject::connect(shortcut, &QShortcut::activated, button,
                     &QToolButton::click);
  };
  addShortcut(QKeySequence{Qt::Key_Left}, playback.stepBackButton);
  addShortcut(QKeySequence{Qt::Key_Space}, playback.playPauseButton);
  addShortcut(QKeySequence{Qt::Key_Right}, playback.stepForwardButton);
  QObject::connect(playback.speedCombo, &QComboBox::currentTextChanged, this,
                   [replayTimer](const QString &speed) {
                     replayTimer->setInterval(timerIntervalForSpeed(speed));
                   });
  QObject::connect(setup.loadButton, &QPushButton::clicked, this, reloadBars);
  QObject::connect(setup.symbolCombo, &QComboBox::currentTextChanged, this,
                   reloadBars);
  QObject::connect(setup.schemaCombo, &QComboBox::currentTextChanged, this,
                   reloadBars);
  QObject::connect(setup.startDate, &QDateEdit::dateChanged, this, reloadBars);
  QObject::connect(setup.endDate, &QDateEdit::dateChanged, this, reloadBars);
  QObject::connect(
      setup.initialCapital, qOverload<double>(&QDoubleSpinBox::valueChanged),
      this, [this, replaySession, updateLegacyPortfolio](const double capital) {
        replaySession->setInitialCapital(capital);
        if (!state_->resultMode) {
          updateLegacyPortfolio();
        }
      });
  QObject::connect(playback.zoomOutButton, &QToolButton::clicked,
                   chart.chartView, &QtChartsCandlestickView::zoomOut);
  QObject::connect(playback.zoomInButton, &QToolButton::clicked,
                   chart.chartView, &QtChartsCandlestickView::zoomIn);
  QObject::connect(playback.zoomResetButton, &QToolButton::clicked,
                   chart.chartView, &QtChartsCandlestickView::resetZoom);

  state_->requestOpen = [this, setup, stopPlayback,
                         renderResult](const QString &requestedId) {
    const auto resultId = requestedId.trimmed();
    if (resultId.isEmpty()) {
      setup.resultStatusLabel->setText(tr("Select a saved result"));
      return;
    }
    const auto catalogIndex = setup.resultCombo->findText(resultId);
    if (catalogIndex >= 0 &&
        setup.resultCombo->itemData(catalogIndex, Qt::UserRole).isValid() &&
        !setup.resultCombo->itemData(catalogIndex, Qt::UserRole).toBool()) {
      setup.resultStatusLabel->setText(
          tr("Result unavailable — %1")
              .arg(setup.resultCombo->itemData(catalogIndex, Qt::ToolTipRole)
                       .toString()));
      return;
    }
    stopPlayback();
    state_->cancellation.requestCancellation();
    state_->cancellation = core::CancellationSource{};
    const auto token = state_->cancellation.token();
    const auto generation = ++state_->generation;
    state_->resultMode = false;
    state_->resultLoadPending = true;
    state_->resultRequestMade = true;
    state_->resultReplay.reset();
    setup.resultCombo->setCurrentText(resultId);
    setup.resultStatusLabel->setText(tr("Loading result %1…").arg(resultId));
    setup.partialLabel->setVisible(false);
    const auto timeframe = setup.resultTimeframeCombo->currentIndex() == 0
                               ? bindings::ResultReplayTimeframe::hourly
                               : bindings::ResultReplayTimeframe::dailyUtc;
    auto *watcher =
        std::make_unique<QFutureWatcher<State::OpenResult>>(this).release();
    QObject::connect(
        watcher, &QFutureWatcher<State::OpenResult>::finished, this,
        [this, watcher, generation, setup, resultId, renderResult]() {
          const auto opened = watcher->result();
          watcher->deleteLater();
          if (generation != state_->generation) {
            return;
          }
          state_->resultLoadPending = false;
          if (!opened.ok()) {
            setup.resultStatusLabel->setText(
                tr("Cannot open result — %1")
                    .arg(QString::fromStdString(opened.error().message)));
            return;
          }
          state_->resultReplay = opened.value();
          state_->resultMode = true;
          setup.resultStatusLabel->setText(
              state_->resultReplay->terminalReason().empty()
                  ? tr("Result %1").arg(resultId)
                  : tr("Result %1 — %2")
                        .arg(resultId,
                             QString::fromStdString(
                                 state_->resultReplay->terminalReason())));
          renderResult();
        });
    auto future = QtConcurrent::run([resultStore = state_->resultStore,
                                     dataStore = state_->dataStore,
                                     resultId = resultId.toStdString(),
                                     timeframe, token]() -> State::OpenResult {
      auto opened = bindings::ResultReplay::open(resultStore, dataStore,
                                                 resultId, timeframe, token);
      if (!opened.ok()) {
        return opened.error();
      }
      return std::shared_ptr<bindings::ResultReplay>{std::move(opened).value()};
    });
    state_->openFutures.push_back(future);
    watcher->setFuture(future);
  };

  QObject::connect(setup.openResultButton, &QPushButton::clicked, this,
                   [this, setup]() {
                     state_->requestOpen(setup.resultCombo->currentText());
                   });
  QObject::connect(
      setup.resultCombo, &QComboBox::textActivated, this,
      [this](const QString &resultId) { state_->requestOpen(resultId); });
  QObject::connect(
      setup.resultTimeframeCombo, &QComboBox::currentIndexChanged, this,
      [this](const int) {
        if (state_->resultMode && state_->resultReplay != nullptr) {
          state_->requestOpen(
              QString::fromStdString(state_->resultReplay->resultId()));
        }
      });

  QObject::connect(
      playback.seekSlider, &QSlider::valueChanged, this,
      [this, stopPlayback, renderResult](const int index) {
        if (!state_->resultMode || state_->resultReplay == nullptr ||
            index < 0 ||
            static_cast<std::size_t>(index) ==
                state_->resultReplay->currentIndex()) {
          return;
        }
        stopPlayback();
        if (state_->resultReplay->seek(static_cast<std::size_t>(index))) {
          renderResult();
        }
      });

  reloadBars();

  auto *catalogWatcher =
      std::make_unique<QFutureWatcher<State::CatalogResult>>(this).release();
  QObject::connect(
      catalogWatcher, &QFutureWatcher<State::CatalogResult>::finished, this,
      [this, catalogWatcher, setup]() {
        const auto catalog = catalogWatcher->result();
        catalogWatcher->deleteLater();
        if (!catalog.ok()) {
          if (!state_->resultRequestMade) {
            setup.resultStatusLabel->setText(
                tr("Cannot load saved results — %1")
                    .arg(QString::fromStdString(catalog.error().message)));
          }
          return;
        }
        const auto activeResultId = setup.resultCombo->currentText();
        setup.resultCombo->clear();
        for (const auto &summary : catalog.value()) {
          setup.resultCombo->addItem(QString::fromStdString(summary.resultId));
          const auto index = setup.resultCombo->count() - 1;
          setup.resultCombo->setItemData(index, summary.available,
                                         Qt::UserRole);
          setup.resultCombo->setItemData(
              index, QString::fromStdString(summary.unavailableReason),
              Qt::ToolTipRole);
          if (auto *model = qobject_cast<QStandardItemModel *>(
                  setup.resultCombo->model());
              model != nullptr && model->item(index) != nullptr) {
            model->item(index)->setEnabled(summary.available);
          }
        }
        if (!state_->resultRequestMade) {
          setup.resultStatusLabel->setText(
              catalog.value().empty() ? tr("No saved Backtest results")
                                      : tr("Select a saved Backtest result"));
        } else {
          setup.resultCombo->setCurrentText(activeResultId);
        }
      });
  state_->catalogFuture = QtConcurrent::run(
      [resultStore = state_->resultStore, dataStore = state_->dataStore]() {
        return bindings::ResultReplay::list(resultStore, dataStore);
      });
  catalogWatcher->setFuture(state_->catalogFuture);
}

ReplayTab::~ReplayTab() = default;

void ReplayTab::openResult(const QString &resultId) {
  if (state_->requestOpen) {
    state_->requestOpen(resultId);
  }
}

} // namespace bte::frontend
