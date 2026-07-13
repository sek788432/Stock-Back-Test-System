#include "ReplayTabSections.h"

#include "Bte/Frontend/ReplayTab.h"

#include <QAbstractItemView>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QSizePolicy>
#include <QStyle>
#include <QVBoxLayout>

#include <utility>

namespace bte::frontend {
namespace {

QLabel *makeValueLabel(QString text, QString objectName) {
  auto *label = new QLabel(std::move(text));
  label->setObjectName(std::move(objectName));
  label->setAccessibleName(label->objectName());
  label->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
  label->setMinimumWidth(150);
  return label;
}

QLabel *makeFormLabel(QString text, QString objectName, QWidget *parent) {
  auto *label = new QLabel(std::move(text), parent);
  label->setObjectName(std::move(objectName));
  label->setAccessibleName(label->text());
  return label;
}

QFrame *makePanel(QString objectName, QWidget *parent) {
  auto *frame = new QFrame(parent);
  frame->setObjectName(std::move(objectName));
  frame->setAccessibleName(frame->objectName());
  frame->setFrameShape(QFrame::StyledPanel);
  frame->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
  return frame;
}

QToolButton *makeToolButton(QWidget *owner, QStyle::StandardPixmap icon,
                            QString name, QString tooltip) {
  auto *button = new QToolButton(owner);
  button->setObjectName(std::move(name));
  button->setAccessibleName(button->objectName());
  button->setToolTip(std::move(tooltip));
  button->setIcon(owner->style()->standardIcon(icon));
  return button;
}

} // namespace

ReplaySetupControls makeReplaySetupControls(QWidget *owner) {
  ReplaySetupControls controls;
  controls.box = new QGroupBox(ReplayTab::tr("Replay setup"), owner);
  controls.box->setObjectName("replaySetupBox");
  controls.box->setAccessibleName("Replay setup");
  controls.box->setMinimumHeight(142);
  controls.box->setMaximumHeight(150);

  auto *setupLayout = new QGridLayout(controls.box);
  setupLayout->setContentsMargins(18, 22, 18, 16);
  setupLayout->setHorizontalSpacing(16);
  setupLayout->setVerticalSpacing(12);

  controls.symbolCombo = new QComboBox(controls.box);
  controls.symbolCombo->setObjectName("replaySymbolCombo");
  controls.symbolCombo->setAccessibleName("Replay symbol");
  controls.symbolCombo->addItems({"AAPL", "MSFT", "NVDA"});
  controls.symbolCombo->setFixedWidth(240);

  controls.schemaCombo = new QComboBox(controls.box);
  controls.schemaCombo->setObjectName("replaySchemaCombo");
  controls.schemaCombo->setAccessibleName("Replay timeframe schema");
  controls.schemaCombo->addItems({"ohlcv-1d", "ohlcv-1h", "ohlcv-1m"});
  controls.schemaCombo->setFixedWidth(240);

  controls.startDate = new QDateEdit(QDate{2024, 1, 1}, controls.box);
  controls.startDate->setObjectName("replayStartDateEdit");
  controls.startDate->setAccessibleName("Replay start date");
  controls.startDate->setCalendarPopup(true);
  controls.startDate->setFixedWidth(240);

  controls.endDate = new QDateEdit(QDate{2024, 6, 30}, controls.box);
  controls.endDate->setObjectName("replayEndDateEdit");
  controls.endDate->setAccessibleName("Replay end date");
  controls.endDate->setCalendarPopup(true);
  controls.endDate->setFixedWidth(240);

  controls.initialCapital = new QDoubleSpinBox(controls.box);
  controls.initialCapital->setObjectName("replayInitialCapitalSpinBox");
  controls.initialCapital->setAccessibleName("Replay initial capital");
  controls.initialCapital->setRange(1.0, 1'000'000'000.0);
  controls.initialCapital->setDecimals(2);
  controls.initialCapital->setPrefix("$");
  controls.initialCapital->setValue(100'000.0);
  controls.initialCapital->setFixedWidth(220);

  controls.loadButton = new QPushButton(ReplayTab::tr("Load"), controls.box);
  controls.loadButton->setObjectName("replayLoadButton");
  controls.loadButton->setAccessibleName("Load replay data");
  controls.loadButton->setFixedSize(132, 38);

  setupLayout->addWidget(
      makeFormLabel(ReplayTab::tr("Symbol"), "replaySymbolLabel", controls.box),
      0, 0);
  setupLayout->addWidget(controls.symbolCombo, 0, 1);
  setupLayout->addWidget(makeFormLabel(ReplayTab::tr("Timeframe"),
                                       "replaySchemaLabel", controls.box),
                         0, 2);
  setupLayout->addWidget(controls.schemaCombo, 0, 3);
  setupLayout->addWidget(makeFormLabel(ReplayTab::tr("Capital"),
                                       "replayInitialCapitalLabel",
                                       controls.box),
                         0, 4);
  setupLayout->addWidget(controls.initialCapital, 0, 5);
  setupLayout->addWidget(controls.loadButton, 0, 6, Qt::AlignTop);
  setupLayout->addWidget(makeFormLabel(ReplayTab::tr("Start"),
                                       "replayStartDateLabel", controls.box),
                         1, 0);
  setupLayout->addWidget(controls.startDate, 1, 1);
  setupLayout->addWidget(
      makeFormLabel(ReplayTab::tr("End"), "replayEndDateLabel", controls.box),
      1, 2);
  setupLayout->addWidget(controls.endDate, 1, 3);
  setupLayout->setColumnMinimumWidth(0, 74);
  setupLayout->setColumnMinimumWidth(2, 92);
  setupLayout->setColumnMinimumWidth(4, 70);
  setupLayout->setColumnStretch(7, 1);

  return controls;
}

ReplayPlaybackControls makeReplayPlaybackControls(QWidget *owner) {
  ReplayPlaybackControls controls;
  controls.stepBackButton =
      makeToolButton(owner, QStyle::SP_MediaSkipBackward,
                     "replayStepBackButton", ReplayTab::tr("Step back"));
  controls.playPauseButton =
      makeToolButton(owner, QStyle::SP_MediaPlay, "replayPlayPauseButton",
                     ReplayTab::tr("Play or pause"));
  controls.stepForwardButton =
      makeToolButton(owner, QStyle::SP_MediaSkipForward,
                     "replayStepForwardButton", ReplayTab::tr("Step forward"));

  controls.stepBackButton->setIcon(QIcon{});
  controls.stepBackButton->setText(ReplayTab::tr("Back"));
  controls.stepBackButton->setToolButtonStyle(Qt::ToolButtonTextOnly);
  controls.playPauseButton->setIcon(QIcon{});
  controls.playPauseButton->setText(ReplayTab::tr("Play"));
  controls.playPauseButton->setToolButtonStyle(Qt::ToolButtonTextOnly);
  controls.playPauseButton->setMinimumWidth(82);
  controls.stepForwardButton->setIcon(QIcon{});
  controls.stepForwardButton->setText(ReplayTab::tr("Step"));
  controls.stepForwardButton->setToolButtonStyle(Qt::ToolButtonTextOnly);

  controls.zoomOutButton = new QToolButton(owner);
  controls.zoomOutButton->setObjectName("replayZoomOutButton");
  controls.zoomOutButton->setAccessibleName("Zoom out");
  controls.zoomOutButton->setToolTip(ReplayTab::tr("Zoom out"));
  controls.zoomOutButton->setText(ReplayTab::tr("Out"));
  controls.zoomOutButton->setMinimumWidth(64);

  controls.zoomInButton = new QToolButton(owner);
  controls.zoomInButton->setObjectName("replayZoomInButton");
  controls.zoomInButton->setAccessibleName("Zoom in");
  controls.zoomInButton->setToolTip(ReplayTab::tr("Zoom in"));
  controls.zoomInButton->setText(ReplayTab::tr("In"));
  controls.zoomInButton->setMinimumWidth(64);

  controls.zoomResetButton = new QToolButton(owner);
  controls.zoomResetButton->setObjectName("replayZoomResetButton");
  controls.zoomResetButton->setAccessibleName("Reset zoom");
  controls.zoomResetButton->setToolTip(ReplayTab::tr("Reset zoom"));
  controls.zoomResetButton->setText(ReplayTab::tr("Reset"));
  controls.zoomResetButton->setMinimumWidth(72);

  controls.speedCombo = new QComboBox(owner);
  controls.speedCombo->setObjectName("replaySpeedCombo");
  controls.speedCombo->setAccessibleName("Replay speed");
  controls.speedCombo->addItems({"1x", "5x", "10x", "max"});

  controls.progress = new QProgressBar(owner);
  controls.progress->setObjectName("replayProgressBar");
  controls.progress->setAccessibleName("Replay progress");
  controls.progress->setRange(0, 100);
  controls.progress->setValue(0);
  controls.progress->setFixedWidth(260);

  controls.bar = new QFrame(owner);
  controls.bar->setObjectName("replayPlaybackBar");
  controls.bar->setAccessibleName("Replay playback controls");
  controls.bar->setFrameShape(QFrame::StyledPanel);
  auto *playbackRow = new QHBoxLayout(controls.bar);
  playbackRow->setContentsMargins(10, 8, 10, 8);
  playbackRow->setSpacing(8);
  playbackRow->addWidget(controls.stepBackButton);
  playbackRow->addWidget(controls.playPauseButton);
  playbackRow->addWidget(controls.stepForwardButton);
  playbackRow->addSpacing(12);
  playbackRow->addWidget(
      makeFormLabel(ReplayTab::tr("Speed"), "replaySpeedLabel", owner));
  playbackRow->addWidget(controls.speedCombo);
  playbackRow->addWidget(controls.progress);
  playbackRow->addSpacing(10);
  playbackRow->addWidget(controls.zoomOutButton);
  playbackRow->addWidget(controls.zoomInButton);
  playbackRow->addWidget(controls.zoomResetButton);
  playbackRow->addStretch(1);

  return controls;
}

ReplayChartSection makeReplayChartSection(QWidget *owner) {
  ReplayChartSection section;
  section.panel = makePanel("replayChartPanel", owner);
  auto *chartLayout = new QVBoxLayout(section.panel);
  chartLayout->setContentsMargins(10, 10, 10, 10);
  chartLayout->setSpacing(8);

  section.chartView = new QtChartsCandlestickView(section.panel);
  section.chartView->setMinimumHeight(300);

  chartLayout->addWidget(section.chartView, 1);
  return section;
}

ReplayPortfolioSection makeReplayPortfolioSection(QWidget *owner) {
  ReplayPortfolioSection section;
  section.box = new QGroupBox(ReplayTab::tr("Portfolio status"), owner);
  section.box->setObjectName("replayPortfolioBox");
  section.box->setAccessibleName("Portfolio status");

  auto *portfolioLayout = new QGridLayout(section.box);
  portfolioLayout->setContentsMargins(16, 20, 16, 14);
  portfolioLayout->setHorizontalSpacing(10);
  portfolioLayout->setVerticalSpacing(8);

  section.cashLabel =
      makeValueLabel(ReplayTab::tr("Cash: --"), "replayCashLabel");
  section.positionLabel =
      makeValueLabel(ReplayTab::tr("Position: --"), "replayPositionLabel");
  section.marketValueLabel =
      makeValueLabel(ReplayTab::tr("Market: --"), "replayMarketValueLabel");
  section.equityLabel =
      makeValueLabel(ReplayTab::tr("Equity: --"), "replayEquityLabel");
  section.pnlLabel = makeValueLabel(ReplayTab::tr("PnL: --"), "replayPnlLabel");
  section.lastPriceLabel =
      makeValueLabel(ReplayTab::tr("Last: --"), "replayLastPriceLabel");
  section.barIndexLabel =
      makeValueLabel(ReplayTab::tr("Bar: 0/0"), "replayBarIndexLabel");

  portfolioLayout->addWidget(section.cashLabel, 0, 0);
  portfolioLayout->addWidget(section.positionLabel, 0, 1);
  portfolioLayout->addWidget(section.marketValueLabel, 0, 2);
  portfolioLayout->addWidget(section.equityLabel, 0, 3);
  portfolioLayout->addWidget(section.pnlLabel, 1, 0);
  portfolioLayout->addWidget(section.lastPriceLabel, 1, 1);
  portfolioLayout->addWidget(section.barIndexLabel, 1, 2);
  portfolioLayout->setColumnStretch(3, 1);
  return section;
}

ReplayTradeLogSection makeReplayTradeLogSection(QWidget *owner) {
  ReplayTradeLogSection section;
  section.panel = new QFrame(owner);
  section.panel->setObjectName("replayTradeLogPanel");
  section.panel->setAccessibleName("Trade log panel");
  section.panel->setFrameShape(QFrame::StyledPanel);

  auto *tradeLogLayout = new QVBoxLayout(section.panel);
  tradeLogLayout->setContentsMargins(10, 10, 10, 10);
  tradeLogLayout->setSpacing(8);

  auto *tradeLogTitle = new QLabel(ReplayTab::tr("Trade log"), section.panel);
  tradeLogTitle->setObjectName("replayTradeLogTitle");
  tradeLogTitle->setAccessibleName("Trade log title");
  tradeLogLayout->addWidget(tradeLogTitle);

  section.table = new QTableWidget(0, 6, owner);
  section.table->setObjectName("replayTradeLogTable");
  section.table->setAccessibleName("Trade log");
  section.table->setHorizontalHeaderLabels(
      {ReplayTab::tr("Time"), ReplayTab::tr("Side"), ReplayTab::tr("Price"),
       ReplayTab::tr("Qty"), ReplayTab::tr("Amount"), ReplayTab::tr("PnL")});
  section.table->setAlternatingRowColors(true);
  section.table->setMinimumHeight(110);
  section.table->verticalHeader()->hide();
  section.table->horizontalHeader()->setStretchLastSection(true);
  section.table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
  section.table->setEditTriggers(QAbstractItemView::NoEditTriggers);
  section.table->setSelectionMode(QAbstractItemView::NoSelection);

  tradeLogLayout->addWidget(section.table);
  return section;
}

} // namespace bte::frontend
