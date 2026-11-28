#include "ReplayTabSections.h"

#include "Bte/Frontend/ReplayTab.h"

#include <QAbstractItemView>
#include <QDateTime>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QList>
#include <QSizePolicy>
#include <QString>
#include <QStyle>
#include <QtCore/Qt>
#include <QtCore/qsize.h>
// IWYU pragma: no_include <QtWidgets/qslider.h>
#include <qslider.h> // IWYU pragma: keep

#include <memory>

namespace bte::frontend {
namespace {

QLabel *makeValueLabel(const QString &text, const char *objectName) {
  auto label = std::make_unique<QLabel>(text);
  label->setObjectName(QString::fromLatin1(objectName));
  label->setAccessibleName(label->text());
  label->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
  label->setMinimumWidth(150);
  return label.release();
}

QLabel *makeFormLabel(const QString &text, const char *objectName,
                      QWidget *parent) {
  auto label = std::make_unique<QLabel>(text, parent);
  label->setObjectName(QString::fromLatin1(objectName));
  label->setAccessibleName(label->text());
  return label.release();
}

class ReplayChartPanel final : public QFrame {
public:
  explicit ReplayChartPanel(QWidget *parent) : QFrame(parent) {}

  [[nodiscard]] QSize sizeHint() const override { return {900, 500}; }
  [[nodiscard]] QSize minimumSizeHint() const override { return {320, 500}; }
};

QToolButton *makeToolButton(QWidget *owner, const QStyle::StandardPixmap icon,
                            const char *name, const QString &tooltip) {
  auto button = std::make_unique<QToolButton>(owner);
  button->setObjectName(QString::fromLatin1(name));
  button->setAccessibleName(button->objectName());
  button->setToolTip(tooltip);
  button->setIcon(owner->style()->standardIcon(icon));
  return button.release();
}

} // namespace

ReplaySetupControls makeReplaySetupControls(QWidget *owner) {
  ReplaySetupControls controls;
  controls.box =
      std::make_unique<QGroupBox>(ReplayTab::tr("Replay setup"), owner)
          .release();
  controls.box->setObjectName("replaySetupBox");
  controls.box->setAccessibleName("Replay setup");
  controls.box->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);

  auto *setupLayout = std::make_unique<QVBoxLayout>(controls.box).release();
  setupLayout->setContentsMargins(18, 22, 18, 16);
  setupLayout->setSpacing(12);
  const auto addSectionHeading = [controls](const QString &text,
                                            const char *objectName) {
    auto label = std::make_unique<QLabel>(text, controls.box);
    label->setObjectName(QString::fromLatin1(objectName));
    label->setAccessibleName(text);
    return label.release();
  };

  controls.resultCombo = std::make_unique<QComboBox>(controls.box).release();
  controls.resultCombo->setObjectName("replayResultCombo");
  controls.resultCombo->setAccessibleName("Saved Backtest result");
  controls.resultCombo->setEditable(true);
  controls.resultCombo->setPlaceholderText(ReplayTab::tr("No saved results"));
  controls.resultCombo->setMinimumWidth(180);
  controls.resultCombo->setMaximumWidth(720);
  controls.resultCombo->setSizePolicy(QSizePolicy::Expanding,
                                      QSizePolicy::Fixed);

  controls.resultTimeframeCombo =
      std::make_unique<QComboBox>(controls.box).release();
  controls.resultTimeframeCombo->setObjectName("replayResultTimeframeCombo");
  controls.resultTimeframeCombo->setAccessibleName("Result replay timeframe");
  controls.resultTimeframeCombo->addItems(
      {ReplayTab::tr("Hourly"), ReplayTab::tr("Daily (UTC)")});
  controls.resultTimeframeCombo->setMinimumWidth(130);
  controls.resultTimeframeCombo->setMaximumWidth(240);
  controls.resultTimeframeCombo->setSizePolicy(QSizePolicy::Expanding,
                                               QSizePolicy::Fixed);

  controls.openResultButton =
      std::make_unique<QPushButton>(ReplayTab::tr("Open Result"), controls.box)
          .release();
  controls.openResultButton->setObjectName("replayOpenResultButton");
  controls.openResultButton->setAccessibleName("Open selected Backtest result");
  controls.openResultButton->setMinimumWidth(120);

  controls.resultStatusLabel =
      makeFormLabel(ReplayTab::tr("Saved results are loading…"),
                    "replayResultStatusLabel", owner);
  controls.resultStatusLabel->setWordWrap(true);
  controls.partialLabel = makeFormLabel(ReplayTab::tr("Partial UTC day"),
                                        "replayPartialLabel", owner);
  controls.partialLabel->setVisible(false);
  auto *barOnlyPreviewLabel = makeFormLabel(
      ReplayTab::tr("Bar-only preview (no fills or portfolio accounting)"),
      "replayBarOnlyPreviewLabel", controls.box);
  barOnlyPreviewLabel->setWordWrap(true);

  controls.symbolCombo = std::make_unique<QComboBox>(controls.box).release();
  controls.symbolCombo->setObjectName("replaySymbolCombo");
  controls.symbolCombo->setAccessibleName("Replay symbol");
  controls.symbolCombo->setMinimumWidth(140);
  controls.symbolCombo->setMaximumWidth(480);
  controls.symbolCombo->setSizePolicy(QSizePolicy::Expanding,
                                      QSizePolicy::Fixed);

  controls.schemaCombo = std::make_unique<QComboBox>(controls.box).release();
  controls.schemaCombo->setObjectName("replaySchemaCombo");
  controls.schemaCombo->setAccessibleName("Replay timeframe schema");
  controls.schemaCombo->setMinimumWidth(140);
  controls.schemaCombo->setMaximumWidth(480);
  controls.schemaCombo->setSizePolicy(QSizePolicy::Expanding,
                                      QSizePolicy::Fixed);

  controls.startDate =
      std::make_unique<QDateEdit>(QDate{2024, 1, 1}, controls.box).release();
  controls.startDate->setObjectName("replayStartDateEdit");
  controls.startDate->setAccessibleName("Replay start date");
  controls.startDate->setCalendarPopup(true);
  controls.startDate->setMinimumWidth(140);
  controls.startDate->setMaximumWidth(480);
  controls.startDate->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

  controls.endDate =
      std::make_unique<QDateEdit>(QDate{2024, 6, 30}, controls.box).release();
  controls.endDate->setObjectName("replayEndDateEdit");
  controls.endDate->setAccessibleName("Replay end date");
  controls.endDate->setCalendarPopup(true);
  controls.endDate->setMinimumWidth(140);
  controls.endDate->setMaximumWidth(480);
  controls.endDate->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

  controls.initialCapital =
      std::make_unique<QDoubleSpinBox>(controls.box).release();
  controls.initialCapital->setObjectName("replayInitialCapitalSpinBox");
  controls.initialCapital->setAccessibleName("Replay initial capital");
  controls.initialCapital->setRange(1.0, 1'000'000'000.0);
  controls.initialCapital->setDecimals(2);
  controls.initialCapital->setPrefix("$");
  controls.initialCapital->setValue(100'000.0);
  controls.initialCapital->setMinimumWidth(140);
  controls.initialCapital->setMaximumWidth(480);
  controls.initialCapital->setSizePolicy(QSizePolicy::Expanding,
                                         QSizePolicy::Fixed);

  controls.loadButton =
      std::make_unique<QPushButton>(ReplayTab::tr("Load"), controls.box)
          .release();
  controls.loadButton->setObjectName("replayLoadButton");
  controls.loadButton->setAccessibleName("Load replay data");
  controls.loadButton->setFixedSize(132, 38);

  auto *resultLayout = std::make_unique<QGridLayout>().release();
  resultLayout->setHorizontalSpacing(12);
  resultLayout->setVerticalSpacing(6);
  resultLayout->addWidget(
      addSectionHeading(ReplayTab::tr("Saved result"), "replayResultLabel"), 0,
      0);
  resultLayout->addWidget(addSectionHeading(ReplayTab::tr("Timeframe"),
                                            "replayResultTimeframeLabel"),
                          0, 1);
  resultLayout->addWidget(controls.resultCombo, 1, 0);
  resultLayout->addWidget(controls.resultTimeframeCombo, 1, 1);
  resultLayout->addWidget(controls.openResultButton, 1, 2, Qt::AlignLeft);
  resultLayout->setColumnStretch(0, 1);
  setupLayout->addLayout(resultLayout);
  setupLayout->addWidget(barOnlyPreviewLabel);

  auto *previewLayout = std::make_unique<QGridLayout>().release();
  previewLayout->setHorizontalSpacing(16);
  previewLayout->setVerticalSpacing(6);
  previewLayout->addWidget(
      makeFormLabel(ReplayTab::tr("Symbol"), "replaySymbolLabel", controls.box),
      0, 0);
  previewLayout->addWidget(makeFormLabel(ReplayTab::tr("Timeframe"),
                                         "replaySchemaLabel", controls.box),
                           0, 1);
  previewLayout->addWidget(controls.symbolCombo, 1, 0);
  previewLayout->addWidget(controls.schemaCombo, 1, 1);
  previewLayout->addWidget(makeFormLabel(ReplayTab::tr("Start"),
                                         "replayStartDateLabel", controls.box),
                           2, 0);
  previewLayout->addWidget(
      makeFormLabel(ReplayTab::tr("End"), "replayEndDateLabel", controls.box),
      2, 1);
  previewLayout->addWidget(controls.startDate, 3, 0);
  previewLayout->addWidget(controls.endDate, 3, 1);
  previewLayout->addWidget(makeFormLabel(ReplayTab::tr("Capital"),
                                         "replayInitialCapitalLabel",
                                         controls.box),
                           4, 0);
  previewLayout->addWidget(controls.initialCapital, 5, 0);
  previewLayout->addWidget(controls.loadButton, 5, 1, Qt::AlignLeft);
  previewLayout->setColumnStretch(0, 1);
  previewLayout->setColumnStretch(1, 1);
  setupLayout->addLayout(previewLayout);

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

  controls.zoomOutButton = std::make_unique<QToolButton>(owner).release();
  controls.zoomOutButton->setObjectName("replayZoomOutButton");
  controls.zoomOutButton->setAccessibleName("Zoom out");
  controls.zoomOutButton->setToolTip(ReplayTab::tr("Zoom out"));
  controls.zoomOutButton->setText(ReplayTab::tr("Out"));
  controls.zoomOutButton->setMinimumWidth(64);

  controls.zoomInButton = std::make_unique<QToolButton>(owner).release();
  controls.zoomInButton->setObjectName("replayZoomInButton");
  controls.zoomInButton->setAccessibleName("Zoom in");
  controls.zoomInButton->setToolTip(ReplayTab::tr("Zoom in"));
  controls.zoomInButton->setText(ReplayTab::tr("In"));
  controls.zoomInButton->setMinimumWidth(64);

  controls.zoomResetButton = std::make_unique<QToolButton>(owner).release();
  controls.zoomResetButton->setObjectName("replayZoomResetButton");
  controls.zoomResetButton->setAccessibleName("Reset zoom");
  controls.zoomResetButton->setToolTip(ReplayTab::tr("Reset zoom"));
  controls.zoomResetButton->setText(ReplayTab::tr("Reset"));
  controls.zoomResetButton->setMinimumWidth(72);

  controls.followLatestButton = std::make_unique<QToolButton>(owner).release();
  controls.followLatestButton->setObjectName("replayFollowLatestButton");
  controls.followLatestButton->setAccessibleName("Follow latest candles");
  controls.followLatestButton->setToolTip(
      ReplayTab::tr("Return to the newest reached candle"));
  controls.followLatestButton->setText(ReplayTab::tr("Follow latest"));
  controls.followLatestButton->setMinimumWidth(100);

  controls.fitReachedBarsButton =
      std::make_unique<QToolButton>(owner).release();
  controls.fitReachedBarsButton->setObjectName("replayFitReachedBarsButton");
  controls.fitReachedBarsButton->setAccessibleName("Fit reached candles");
  controls.fitReachedBarsButton->setToolTip(
      ReplayTab::tr("Fit reached candles when each remains readable"));
  controls.fitReachedBarsButton->setText(ReplayTab::tr("Fit reached"));
  controls.fitReachedBarsButton->setMinimumWidth(95);

  controls.speedCombo = std::make_unique<QComboBox>(owner).release();
  controls.speedCombo->setObjectName("replaySpeedCombo");
  controls.speedCombo->setAccessibleName("Replay speed");
  controls.speedCombo->addItems({"1x", "5x", "10x", "max"});

  controls.progress = std::make_unique<QProgressBar>(owner).release();
  controls.progress->setObjectName("replayProgressBar");
  controls.progress->setAccessibleName("Replay progress");
  controls.progress->setRange(0, 100);
  controls.progress->setValue(0);
  controls.progress->setFixedWidth(180);

  controls.seekSlider =
      std::make_unique<QSlider>(Qt::Horizontal, owner).release();
  controls.seekSlider->setObjectName("replaySeekSlider");
  controls.seekSlider->setAccessibleName("Replay position");
  controls.seekSlider->setRange(0, 0);
  controls.seekSlider->setMinimumWidth(120);

  controls.bar = std::make_unique<QFrame>(owner).release();
  controls.bar->setObjectName("replayPlaybackBar");
  controls.bar->setAccessibleName("Replay playback controls");
  controls.bar->setFrameShape(QFrame::StyledPanel);
  auto *barLayout = std::make_unique<QVBoxLayout>(controls.bar).release();
  barLayout->setContentsMargins(10, 8, 10, 8);
  barLayout->setSpacing(4);
  auto *playbackRow = std::make_unique<QHBoxLayout>().release();
  playbackRow->setSpacing(8);
  barLayout->addLayout(playbackRow);
  playbackRow->addWidget(controls.stepBackButton);
  playbackRow->addWidget(controls.playPauseButton);
  playbackRow->addWidget(controls.stepForwardButton);
  playbackRow->addSpacing(12);
  playbackRow->addWidget(
      makeFormLabel(ReplayTab::tr("Speed"), "replaySpeedLabel", owner));
  playbackRow->addWidget(controls.speedCombo);
  playbackRow->addWidget(controls.progress);
  playbackRow->addWidget(controls.seekSlider);
  playbackRow->addStretch(1);
  auto *navigationRow = std::make_unique<QHBoxLayout>().release();
  navigationRow->setSpacing(8);
  navigationRow->addStretch(1);
  navigationRow->addWidget(controls.zoomOutButton);
  navigationRow->addWidget(controls.zoomInButton);
  navigationRow->addWidget(controls.zoomResetButton);
  navigationRow->addWidget(controls.followLatestButton);
  navigationRow->addWidget(controls.fitReachedBarsButton);
  barLayout->addLayout(navigationRow);

  return controls;
}

ReplayChartSection makeReplayChartSection(QWidget *owner) {
  ReplayChartSection section;
  section.panel = std::make_unique<ReplayChartPanel>(owner).release();
  section.panel->setObjectName("replayChartPanel");
  section.panel->setAccessibleName("Replay chart panel");
  section.panel->setFrameShape(QFrame::StyledPanel);
  section.panel->setMinimumHeight(500);
  section.panel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
  auto *chartLayout = std::make_unique<QVBoxLayout>(section.panel).release();
  chartLayout->setContentsMargins(10, 10, 10, 10);
  chartLayout->setSpacing(8);

  section.chartView =
      std::make_unique<QtChartsCandlestickView>(section.panel).release();
  section.chartView->setMinimumHeight(460);

  chartLayout->addWidget(section.chartView, 1);
  return section;
}

ReplayPortfolioSection makeReplayPortfolioSection(QWidget *owner) {
  ReplayPortfolioSection section;
  section.box =
      std::make_unique<QGroupBox>(ReplayTab::tr("Portfolio status"), owner)
          .release();
  section.box->setObjectName("replayPortfolioBox");
  section.box->setAccessibleName("Portfolio status");

  auto *portfolioLayout = std::make_unique<QGridLayout>(section.box).release();
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
  section.realizedPnlLabel = makeValueLabel(ReplayTab::tr("Realized P/L: --"),
                                            "replayRealizedPnlLabel");
  section.unrealizedPnlLabel = makeValueLabel(
      ReplayTab::tr("Unrealized P/L: --"), "replayUnrealizedPnlLabel");
  section.lastPriceLabel =
      makeValueLabel(ReplayTab::tr("Last: --"), "replayLastPriceLabel");
  section.barIndexLabel =
      makeValueLabel(ReplayTab::tr("Bar: 0/0"), "replayBarIndexLabel");

  portfolioLayout->addWidget(section.cashLabel, 0, 0);
  portfolioLayout->addWidget(section.positionLabel, 0, 1);
  portfolioLayout->addWidget(section.marketValueLabel, 0, 2);
  portfolioLayout->addWidget(section.equityLabel, 0, 3);
  portfolioLayout->addWidget(section.pnlLabel, 1, 0);
  portfolioLayout->addWidget(section.realizedPnlLabel, 1, 1);
  portfolioLayout->addWidget(section.unrealizedPnlLabel, 1, 2);
  portfolioLayout->addWidget(section.lastPriceLabel, 1, 3);
  portfolioLayout->addWidget(section.barIndexLabel, 2, 0);
  portfolioLayout->setColumnStretch(3, 1);
  return section;
}

ReplayTradeLogSection makeReplayTradeLogSection(QWidget *owner) {
  ReplayTradeLogSection section;
  section.panel = std::make_unique<QFrame>(owner).release();
  section.panel->setObjectName("replayTradeLogPanel");
  section.panel->setAccessibleName("Trade log panel");
  section.panel->setFrameShape(QFrame::StyledPanel);

  auto *tradeLogLayout = std::make_unique<QVBoxLayout>(section.panel).release();
  tradeLogLayout->setContentsMargins(10, 10, 10, 10);
  tradeLogLayout->setSpacing(8);

  auto *tradeLogTitle =
      std::make_unique<QLabel>(ReplayTab::tr("Trade log"), section.panel)
          .release();
  tradeLogTitle->setObjectName("replayTradeLogTitle");
  tradeLogTitle->setAccessibleName("Trade log title");
  tradeLogLayout->addWidget(tradeLogTitle);

  section.table = std::make_unique<QTableWidget>(0, 6, owner).release();
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
