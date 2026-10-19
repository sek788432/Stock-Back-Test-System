#include "Bte/Frontend/ReplayTab.h"

#include "ResultRecoveryNoticeLabel.h"

// IWYU pragma: no_include "Bte/Results/ResultStore.h"

#include "Bte/Bindings/ReplayDataLoader.h"
#include "Bte/Bindings/ReplaySessionVm.h"
#include "Bte/Bindings/ResultCatalogSnapshot.h"
#include "Bte/Bindings/ResultReplay.h"
#include "Bte/Bindings/ResultReplayRequests.h"
#include "Bte/Core/Result.h"
#include "Bte/Frontend/IChartView.h"
#include "Bte/Frontend/QtChartsCandlestickView.h"
#include "Bte/Frontend/ReplayBarViewport.h"

#include "ReplayTabSections.h"
#include "ReplayTabStyle.h"

#include <QComboBox>
#include <QDateEdit>
#include <QDateTime>
#include <QDoubleSpinBox>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QList>
#include <QLocale>
#include <QObject>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QShortcut>
#include <QSplitter>
#include <QStandardItemModel>
#include <QStandardPaths>
#include <QString>
#include <QTableWidget>
#include <QTimer>
#include <QToolButton>
#include <QVariant>
#include <QtCore/Qt>
#include <QtCore/qabstractitemmodel.h>
#include <QtCore/qtenvironmentvariables.h>
#include <QtCore/qtimezone.h>
#include <QtGui/qkeysequence.h>
#include <QtWidgets/qslider.h>

#include <algorithm> // IWYU pragma: keep
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string> // IWYU pragma: keep
#include <utility>
#include <vector>

// libstdc++ attributes ranges algorithm return types to this header.
#if defined(__GLIBCXX__)
#include <ranges>
#endif

#include "Bte/Core/Bar.h"

namespace bte::frontend {
using bte::bindings::loadReplayBars;
using bte::bindings::ReplaySessionVm;

namespace {

constexpr std::size_t maximumPlaybackBatchSize = 32;

void setLabelText(QLabel *label, const QString &text) {
  label->setText(text);
  label->setAccessibleName(text);
}

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

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): distinct UI text roles
QTableWidgetItem *makeItem(const QString &text,
                           const QString &accessibleDescription = {}) {
  auto item = std::make_unique<QTableWidgetItem>(text);
  if (!accessibleDescription.isEmpty()) {
    item->setData(Qt::AccessibleDescriptionRole, accessibleDescription);
  }
  return item.release();
}

} // namespace

struct ReplayTab::State final {
  State() = default;
  State(const State &) = delete;
  State &operator=(const State &) = delete;
  State(State &&) = delete;
  State &operator=(State &&) = delete;
  ~State() = default;

  std::unique_ptr<bindings::ResultReplayRequests> catalogRequests;
  std::unique_ptr<bindings::ResultReplayRequests> openRequests;
  std::shared_ptr<bindings::ResultReplay> resultReplay;
  const bindings::ResultReplay *chartedResultReplay = nullptr;
  std::size_t chartedFrameCount = 0;
  std::function<void(const QString &, std::optional<core::Timestamp>)>
      requestOpen;
  std::function<void()> reloadCatalog;
  QString requestedResultId;
  bool resultMode = false;
  bool resultRequestMade = false;
};

ReplayTab::ReplayTab(QWidget *parent)
    : ReplayTab(defaultApplicationStore("BTE_RESULT_STORE", "Results"),
                defaultApplicationStore("BTE_DATA_STORE", "Data"), parent) {}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): Qt wiring
ReplayTab::ReplayTab(std::filesystem::path resultStore,
                     std::filesystem::path dataStore, QWidget *parent)
    : QWidget(parent), state_(std::make_unique<State>()) {
  state_->catalogRequests = std::make_unique<bindings::ResultReplayRequests>(
      resultStore, dataStore, this);
  state_->catalogRequests->setObjectName("resultReplayCatalogRequests");
  state_->openRequests = std::make_unique<bindings::ResultReplayRequests>(
      std::move(resultStore), std::move(dataStore), this);
  state_->openRequests->setObjectName("resultReplayOpenRequests");
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

  auto *recoveryNotices = std::make_unique<ResultRecoveryNoticeLabel>(
                              "replayRecoveryNoticesLabel", content)
                              .release();
  root->addWidget(recoveryNotices);

  const auto setup = makeReplaySetupControls(this);
  auto *setupToggle = std::make_unique<QToolButton>(this).release();
  setupToggle->setObjectName("replaySetupToggleButton");
  setupToggle->setAccessibleName("Show or hide replay setup");
  setupToggle->setText(tr("Show replay setup"));
  setupToggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
  setupToggle->setArrowType(Qt::RightArrow);
  setupToggle->setCheckable(true);
  setupToggle->setFocusPolicy(Qt::StrongFocus);
  setup.box->setVisible(false);
  root->addWidget(setupToggle, 0, Qt::AlignLeft);
  root->addWidget(setup.box);

  const auto playback = makeReplayPlaybackControls(this);
  root->addWidget(playback.bar);

  const auto chart = makeReplayChartSection(this);
  const auto portfolio = makeReplayPortfolioSection(this);
  const auto tradeLog = makeReplayTradeLogSection(this);

  auto *detailsPanel = std::make_unique<QWidget>(this).release();
  detailsPanel->setObjectName("replayDetailsPanel");
  detailsPanel->setAccessibleName("Replay portfolio and trade details");
  auto *detailsLayout = std::make_unique<QVBoxLayout>(detailsPanel).release();
  detailsLayout->setContentsMargins(0, 0, 0, 0);
  detailsLayout->setSpacing(8);
  detailsLayout->addWidget(portfolio.box);
  detailsLayout->addWidget(tradeLog.panel, 1);

  auto *chartTradeSplitter =
      std::make_unique<QSplitter>(Qt::Vertical, this).release();
  chartTradeSplitter->setObjectName("replayChartTradeSplitter");
  chartTradeSplitter->setAccessibleName(
      "Adjust chart and replay details height");
  chartTradeSplitter->setChildrenCollapsible(false);
  chartTradeSplitter->addWidget(chart.panel);
  chartTradeSplitter->addWidget(detailsPanel);
  chartTradeSplitter->setStretchFactor(0, 5);
  chartTradeSplitter->setStretchFactor(1, 2);
  chartTradeSplitter->setSizes({600, 220});
  root->addWidget(chartTradeSplitter, 1);

  connect(setupToggle, &QToolButton::toggled, this,
          [setupToggle, setup](const bool expanded) {
            setup.box->setVisible(expanded);
            setupToggle->setText(expanded ? ReplayTab::tr("Hide replay setup")
                                          : ReplayTab::tr("Show replay setup"));
            setupToggle->setArrowType(expanded ? Qt::DownArrow
                                               : Qt::RightArrow);
          });

  auto replaySession = std::make_shared<ReplaySessionVm>();
  auto *replayTimer = std::make_unique<QTimer>(this).release();
  replayTimer->setObjectName("replayPlaybackTimer");
  replayTimer->setTimerType(Qt::PreciseTimer);
  replayTimer->setInterval(
      timerIntervalForSpeed(playback.speedCombo->currentText()));

  const auto updateLegacyPortfolio = [=]() {
    const auto snapshot = replaySession->portfolioSnapshot();
    setLabelText(portfolio.cashLabel,
                 tr("Cash: %1").arg(formatMoney(snapshot.cash)));
    setLabelText(portfolio.positionLabel,
                 tr("Position: %1").arg(snapshot.position, 0, 'f', 0));
    setLabelText(portfolio.marketValueLabel,
                 tr("Market: %1").arg(formatMoney(snapshot.marketValue)));
    setLabelText(portfolio.equityLabel,
                 tr("Equity: %1").arg(formatMoney(snapshot.equity)));
    setLabelText(portfolio.pnlLabel,
                 tr("PnL: %1").arg(formatMoney(snapshot.pnl)));
    setLabelText(portfolio.lastPriceLabel,
                 snapshot.hasLastPrice
                     ? tr("Last: %1").arg(formatPrice(snapshot.lastPrice))
                     : tr("Last: --"));
    setLabelText(portfolio.barIndexLabel,
                 tr("Bar: %1/%2")
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
    chart.chartView->setDailyTimeframe(
        setup.schemaCombo->currentText().contains("1d"));
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
    chart.chartView->setDailyTimeframe(
        setup.resultTimeframeCombo->currentIndex() != 0);
    if (state_->resultReplay == nullptr) {
      state_->chartedResultReplay = nullptr;
      state_->chartedFrameCount = 0;
      chart.chartView->setBarWindow({});
      chart.chartView->clearMarkers();
      playback.progress->setValue(0);
      playback.seekSlider->setRange(0, 0);
      tradeLog.table->setRowCount(0);
      setLabelText(portfolio.cashLabel, tr("Cash: --"));
      setLabelText(portfolio.positionLabel, tr("Position: --"));
      setLabelText(portfolio.marketValueLabel, tr("Market: --"));
      setLabelText(portfolio.equityLabel, tr("Equity: --"));
      setLabelText(portfolio.pnlLabel, tr("PnL: --"));
      setLabelText(portfolio.lastPriceLabel, tr("Last: --"));
      setLabelText(portfolio.barIndexLabel, tr("Bar: 0/0"));
      setup.partialLabel->setVisible(false);
      return;
    }
    const auto frames = state_->resultReplay->timeline();
    const auto presentedFrameCount =
        std::min(frames.size(), state_->resultReplay->currentIndex() + 1);
    const auto mustReplaceChartTimeline =
        state_->chartedResultReplay != state_->resultReplay.get() ||
        presentedFrameCount < state_->chartedFrameCount;
    const auto firstChangedFrame =
        mustReplaceChartTimeline ? std::size_t{0} : state_->chartedFrameCount;
    std::vector<ChartMarker> markers;
    std::vector<core::Bar> bars;
    bars.reserve(presentedFrameCount - firstChangedFrame);
    for (auto index = firstChangedFrame; index < presentedFrameCount; ++index) {
      const auto &frame = frames[index];
      bars.push_back(frame.candle);
      for (const auto &fill : frame.fills) {
        markers.push_back({.timestamp = fill.timestamp,
                           .price = fill.price,
                           .isBuy = fill.isBuy,
                           .candleTimestamp = frame.candle.ts});
      }
    }
    if (mustReplaceChartTimeline) {
      chart.chartView->setBarWindow(bars);
      chart.chartView->setMarkers(markers);
    } else if (!bars.empty()) {
      chart.chartView->appendBars(bars);
      chart.chartView->appendMarkers(markers);
    }
    state_->chartedResultReplay = state_->resultReplay.get();
    state_->chartedFrameCount = presentedFrameCount;
    playback.progress->setValue(state_->resultReplay->progressPercent());
    const auto *frame = state_->resultReplay->current();
    if (frame == nullptr) {
      playback.seekSlider->setRange(0, 0);
      setLabelText(setup.resultStatusLabel, tr("Result is valid but empty"));
      tradeLog.table->setRowCount(0);
      return;
    }
    playback.seekSlider->setRange(
        0, static_cast<int>(state_->resultReplay->totalFrames() - 1));
    playback.seekSlider->setValue(
        static_cast<int>(state_->resultReplay->currentIndex()));
    setLabelText(portfolio.cashLabel,
                 tr("Cash: %1").arg(formatMoney(frame->portfolio.cash)));
    setLabelText(portfolio.positionLabel,
                 tr("Position: %1").arg(frame->portfolio.positionShares));
    setLabelText(
        portfolio.marketValueLabel,
        tr("Market: %1").arg(formatMoney(frame->portfolio.marketValue)));
    setLabelText(portfolio.equityLabel,
                 tr("Equity: %1").arg(formatMoney(frame->portfolio.equity)));
    setLabelText(portfolio.pnlLabel,
                 tr("PnL: %1").arg(formatMoney(frame->portfolio.pnl)));
    setLabelText(portfolio.lastPriceLabel,
                 tr("Last: %1").arg(formatPrice(frame->candle.close)));
    setLabelText(portfolio.barIndexLabel,
                 tr("Bar: %1/%2")
                     .arg(state_->resultReplay->currentIndex() + 1)
                     .arg(state_->resultReplay->totalFrames()));
    setup.partialLabel->setVisible(frame->partialUtcDay);

    if (mustReplaceChartTimeline) {
      tradeLog.table->setRowCount(0);
    }
    auto nextTradeRow = tradeLog.table->rowCount();
    auto addedTradeRows = 0;
    for (auto index = firstChangedFrame; index < presentedFrameCount; ++index) {
      addedTradeRows += static_cast<int>(frames[index].fills.size());
    }
    if (addedTradeRows != 0) {
      tradeLog.table->setRowCount(nextTradeRow + addedTradeRows);
    }
    QSignalBlocker cellUpdates{tradeLog.table->model()};
    for (auto index = firstChangedFrame; index < presentedFrameCount; ++index) {
      for (const auto &fill : frames[index].fills) {
        const auto milliseconds =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                fill.timestamp.time_since_epoch())
                .count();
        const auto row = nextTradeRow++;
        const auto timeText =
            QDateTime::fromMSecsSinceEpoch(milliseconds, QTimeZone::UTC)
                .toString(Qt::ISODate);
        const auto sideText = fill.isBuy ? tr("Buy") : tr("Sell");
        const auto priceText = formatPrice(fill.price);
        const auto quantityText = QString::number(fill.quantityShares);
        const auto amountText = formatMoney(fill.amount);
        const auto fillDescription =
            tr("%1 fill: %2 shares at %3, amount %4, %5")
                .arg(sideText, quantityText, priceText, amountText, timeText);
        tradeLog.table->setItem(row, 0, makeItem(timeText, fillDescription));
        tradeLog.table->setItem(row, 1, makeItem(sideText, fillDescription));
        tradeLog.table->setItem(row, 2, makeItem(priceText, fillDescription));
        tradeLog.table->setItem(row, 3,
                                makeItem(quantityText, fillDescription));
        tradeLog.table->setItem(row, 4, makeItem(amountText, fillDescription));
        tradeLog.table->setItem(row, 5, makeItem(QString{}, fillDescription));
      }
    }
    cellUpdates.unblock();
    if (addedTradeRows != 0) {
      auto *model = tradeLog.table->model();
      emit model->dataChanged(model->index(nextTradeRow - addedTradeRows, 0),
                              model->index(nextTradeRow - 1, 5));
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
    state_->openRequests->cancel();
    state_->resultMode = false;
    state_->resultRequestMade = false;
    state_->requestedResultId.clear();
    state_->resultReplay.reset();
    setup.partialLabel->setVisible(false);
    setLabelText(setup.resultStatusLabel, tr("Bar-only preview"));
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
  QObject::connect(
      replayTimer, &QTimer::timeout, this,
      [this, playback, advanceOneBar, renderResult, stopPlayback]() {
        if (!state_->resultMode ||
            playback.speedCombo->currentText() != "max") {
          advanceOneBar();
          return;
        }
        // Result mode is entered only after publishing a non-null Replay.
        // GCOVR_EXCL_START
        if (state_->resultReplay == nullptr) {
          stopPlayback();
          return;
        }
        // GCOVR_EXCL_STOP
        std::size_t advanced = 0;
        while (advanced < maximumPlaybackBatchSize &&
               state_->resultReplay->stepForward()) {
          ++advanced;
        }
        if (advanced == 0U) {
          stopPlayback();
          return;
        }
        renderResult();
        if (state_->resultReplay->currentIndex() + 1 >=
            state_->resultReplay->totalFrames()) {
          stopPlayback();
        }
      });
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
  QObject::connect(playback.followLatestButton, &QToolButton::clicked,
                   chart.chartView, &QtChartsCandlestickView::followLatest);
  QObject::connect(playback.fitReachedBarsButton, &QToolButton::clicked,
                   chart.chartView, [chartView = chart.chartView] {
                     [[maybe_unused]] const auto fitted =
                         chartView->fitReachedBars();
                   });
  QObject::connect(
      chart.chartView, &QtChartsCandlestickView::viewportModeChanged, this,
      [playback, chartView = chart.chartView](const bool followingLatest) {
        playback.followLatestButton->setEnabled(
            chartView->candleCount() > 0 &&
            (!followingLatest ||
             chartView->visibleSlots().end < chartView->candleCount()));
        playback.fitReachedBarsButton->setEnabled(
            chartView->canFitReachedBars());
      });
  playback.followLatestButton->setEnabled(false);
  playback.fitReachedBarsButton->setEnabled(false);

  state_->requestOpen = [this, setup, stopPlayback, renderResult](
                            const QString &requestedId,
                            const std::optional<core::Timestamp> cursorAnchor) {
    const auto resultId = requestedId.trimmed();
    stopPlayback();
    state_->openRequests->cancel();
    state_->resultMode = false;
    state_->resultReplay.reset();
    state_->requestedResultId = resultId;
    renderResult();
    if (resultId.isEmpty()) {
      setLabelText(setup.resultStatusLabel, tr("Select a saved result"));
      return;
    }
    const auto catalogIndex =
        setup.resultCombo->findData(resultId, Qt::UserRole);
    if (catalogIndex >= 0 &&
        !setup.resultCombo->itemData(catalogIndex, Qt::UserRole + 1).toBool()) {
      setLabelText(
          setup.resultStatusLabel,
          tr("Result unavailable — %1")
              .arg(setup.resultCombo->itemData(catalogIndex, Qt::ToolTipRole)
                       .toString()));
      return;
    }
    state_->resultRequestMade = true;
    if (catalogIndex >= 0) {
      setup.resultCombo->setCurrentIndex(catalogIndex);
    } else {
      setup.resultCombo->setCurrentText(resultId);
    }
    setLabelText(setup.resultStatusLabel,
                 tr("Loading result %1…").arg(resultId));
    setup.partialLabel->setVisible(false);
    const auto timeframe = setup.resultTimeframeCombo->currentIndex() == 0
                               ? bindings::ResultReplayTimeframe::hourly
                               : bindings::ResultReplayTimeframe::dailyUtc;
    state_->openRequests->requestOpen(
        resultId.toStdString(), timeframe,
        [this, setup, resultId, renderResult, cursorAnchor](
            const bindings::ResultReplayRequests::OpenResult &opened) {
          if (!opened.ok()) {
            setLabelText(
                setup.resultStatusLabel,
                tr("Cannot open result — %1")
                    .arg(QString::fromStdString(opened.error().message)));
            return;
          }
          state_->resultReplay = opened.value();
          state_->resultMode = true;
          if (cursorAnchor.has_value()) {
            const auto timeline = state_->resultReplay->timeline();
            const auto position = std::ranges::upper_bound(
                timeline, *cursorAnchor, {},
                [](const auto &frame) { return frame.candle.ts; });
            if (position != timeline.begin()) {
              const auto index =
                  static_cast<std::size_t>(position - timeline.begin() - 1);
              (void)state_->resultReplay->seek(index);
            }
          }
          setLabelText(
              setup.resultStatusLabel,
              state_->resultReplay->terminalReason().empty()
                  ? tr("Result %1").arg(resultId)
                  : tr("Result %1 — %2")
                        .arg(resultId,
                             QString::fromStdString(
                                 state_->resultReplay->terminalReason())));
          renderResult();
        });
  };

  QObject::connect(
      setup.openResultButton, &QPushButton::clicked, this, [this, setup]() {
        const auto text = setup.resultCombo->currentText();
        const auto index = setup.resultCombo->currentIndex();
        const auto selectedId =
            index >= 0 && text == setup.resultCombo->itemText(index)
                ? setup.resultCombo->itemData(index, Qt::UserRole).toString()
                : text.trimmed();
        state_->requestOpen(selectedId, std::nullopt);
      });
  QObject::connect(
      setup.resultCombo, &QComboBox::activated, this,
      [this, setup](const int index) {
        state_->requestOpen(
            setup.resultCombo->itemData(index, Qt::UserRole).toString(),
            std::nullopt);
      });
  QObject::connect(
      setup.resultTimeframeCombo, &QComboBox::currentIndexChanged, this,
      [this](const int) {
        if (!state_->requestedResultId.isEmpty()) {
          const auto *frame = state_->resultReplay == nullptr
                                  ? nullptr
                                  : state_->resultReplay->current();
          state_->requestOpen(state_->requestedResultId,
                              frame == nullptr ? std::nullopt
                                               : std::optional<core::Timestamp>{
                                                     frame->candle.ts});
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

  state_->reloadCatalog = [this, setup, recoveryNotices] {
    state_->catalogRequests->requestCatalog(
        [this, setup, recoveryNotices](
            const bindings::ResultReplayRequests::CatalogResult &catalog) {
          if (!catalog.ok()) {
            if (!state_->resultRequestMade) {
              setLabelText(
                  setup.resultStatusLabel,
                  tr("Cannot load saved results — %1")
                      .arg(QString::fromStdString(catalog.error().message)));
            }
            return;
          }
          recoveryNotices->present(catalog.value().notices);
          const auto activeResultId =
              state_->requestedResultId.isEmpty()
                  ? setup.resultCombo->currentData(Qt::UserRole).toString()
                  : state_->requestedResultId;
          setup.resultCombo->clear();
          for (const auto &summary : catalog.value().entries) {
            const auto resultId = QString::fromStdString(summary.resultId);
            const auto displayName =
                QString::fromStdString(summary.displayName);
            setup.resultCombo->addItem(
                displayName.isEmpty()
                    ? resultId
                    : tr("%1 — %2").arg(displayName, resultId.left(8)),
                resultId);
            const auto index = setup.resultCombo->count() - 1;
            setup.resultCombo->setItemData(index, summary.available,
                                           Qt::UserRole + 1);
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
            setLabelText(setup.resultStatusLabel,
                         catalog.value().entries.empty()
                             ? tr("No saved Backtest results")
                             : tr("Select a saved Backtest result"));
          } else {
            const auto index =
                setup.resultCombo->findData(activeResultId, Qt::UserRole);
            if (index >= 0) {
              setup.resultCombo->setCurrentIndex(index);
            }
          }
        });
  };
  state_->reloadCatalog();
}

ReplayTab::~ReplayTab() = default;

void ReplayTab::openResult(const QString &resultId) {
  if (state_->requestOpen) {
    state_->requestOpen(resultId, std::nullopt);
    state_->reloadCatalog();
  }
}

void ReplayTab::closeResultIfId(const QString &resultId) {
  if (state_->requestedResultId != resultId) {
    return;
  }
  state_->catalogRequests->cancel();
  openResult({});
  if (auto *selector = findChild<QComboBox *>("replayResultCombo");
      selector != nullptr) {
    const auto index = selector->findData(resultId, Qt::UserRole);
    if (index >= 0) {
      selector->removeItem(index);
    }
    selector->setCurrentIndex(-1);
  }
}

} // namespace bte::frontend
