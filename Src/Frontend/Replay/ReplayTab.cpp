#include "Bte/Frontend/ReplayTab.h"

#include "Bte/Bindings/ReplayDataLoader.h"
#include "Bte/Bindings/ReplaySessionVm.h"
#include "Bte/Frontend/QtChartsCandlestickView.h"

#include "ReplayTabSections.h"
#include "ReplayTabStyle.h"

#include <QComboBox>
#include <QDateEdit>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLocale>
#include <QPushButton>
#include <QScrollArea>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <memory>

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

} // namespace

ReplayTab::ReplayTab(QWidget *parent) : QWidget(parent) {
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

  const auto updatePortfolioSnapshot = [=]() {
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

  const auto updateProgress = [=]() {
    playback.progress->setValue(replaySession->progressPercent());
  };

  const auto setPlaying = [=](const bool isPlaying) {
    playback.playPauseButton->setText(isPlaying ? tr("Pause") : tr("Play"));
  };

  const auto resetVisibleReplay = [=]() {
    chart.chartView->setBarWindow(replaySession->visibleBars());
    updateProgress();
    updatePortfolioSnapshot();
  };

  const auto stopPlayback = [=]() {
    replayTimer->stop();
    setPlaying(false);
  };

  const auto advanceOneBar = [=]() {
    if (!replaySession->stepForward()) {
      stopPlayback();
      return;
    }

    chart.chartView->appendBar(replaySession->visibleBars().back());
    updateProgress();
    updatePortfolioSnapshot();

    if (replaySession->currentIndex() >= replaySession->totalBars()) {
      stopPlayback();
    }
  };

  const auto reloadBars = [=]() {
    stopPlayback();
    replaySession->setInitialCapital(setup.initialCapital->value());
    replaySession->reset(loadReplayBars(
        setup.symbolCombo->currentText(), setup.schemaCombo->currentText(),
        setup.startDate->date(), setup.endDate->date()));
    resetVisibleReplay();
  };

  QObject::connect(playback.stepForwardButton, &QToolButton::clicked, this,
                   advanceOneBar);
  QObject::connect(playback.stepBackButton, &QToolButton::clicked, this, [=]() {
    stopPlayback();
    if (!replaySession->stepBack()) {
      return;
    }
    chart.chartView->setBarWindow(replaySession->visibleBars());
    updateProgress();
    updatePortfolioSnapshot();
  });
  QObject::connect(
      playback.playPauseButton, &QToolButton::clicked, this, [=]() {
        if (replayTimer->isActive()) {
          stopPlayback();
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
  QObject::connect(playback.speedCombo, &QComboBox::currentTextChanged, this,
                   [=](const QString &speed) {
                     replayTimer->setInterval(timerIntervalForSpeed(speed));
                   });
  QObject::connect(setup.loadButton, &QPushButton::clicked, this, reloadBars);
  QObject::connect(setup.symbolCombo, &QComboBox::currentTextChanged, this,
                   reloadBars);
  QObject::connect(setup.schemaCombo, &QComboBox::currentTextChanged, this,
                   reloadBars);
  QObject::connect(setup.startDate, &QDateEdit::dateChanged, this, reloadBars);
  QObject::connect(setup.endDate, &QDateEdit::dateChanged, this, reloadBars);
  QObject::connect(setup.initialCapital,
                   qOverload<double>(&QDoubleSpinBox::valueChanged), this,
                   [=](const double capital) {
                     replaySession->setInitialCapital(capital);
                     updatePortfolioSnapshot();
                   });
  QObject::connect(playback.zoomOutButton, &QToolButton::clicked,
                   chart.chartView, &QtChartsCandlestickView::zoomOut);
  QObject::connect(playback.zoomInButton, &QToolButton::clicked,
                   chart.chartView, &QtChartsCandlestickView::zoomIn);
  QObject::connect(playback.zoomResetButton, &QToolButton::clicked,
                   chart.chartView, &QtChartsCandlestickView::resetZoom);
  reloadBars();
}

} // namespace bte::frontend
