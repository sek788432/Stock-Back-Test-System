#include "Bte/Frontend/ReplayTab.h"

#include "Bte/Core/Bar.h"
#include "Bte/Frontend/QtChartsCandlestickView.h"
#include "Bte/Frontend/ReplaySessionVm.h"

#include <QComboBox>
#include <QDate>
#include <QDateEdit>
#include <QDoubleSpinBox>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QSizePolicy>
#include <QStyle>
#include <QToolButton>
#include <QVBoxLayout>

#include <chrono>
#include <limits>
#include <utility>
#include <vector>

namespace bte::frontend {
namespace {

QLabel* makeValueLabel(QString text, QString objectName) {
    auto* label = new QLabel(std::move(text));
    label->setObjectName(std::move(objectName));
    label->setAccessibleName(label->objectName());
    label->setAlignment(Qt::AlignCenter);
    label->setMinimumWidth(110);
    return label;
}

QLabel* makeFormLabel(QString text, QString objectName, QWidget* parent) {
    auto* label = new QLabel(std::move(text), parent);
    label->setObjectName(std::move(objectName));
    label->setAccessibleName(label->text());
    return label;
}

QFrame* makePanel(QString objectName) {
    auto* frame = new QFrame();
    frame->setObjectName(std::move(objectName));
    frame->setAccessibleName(frame->objectName());
    frame->setFrameShape(QFrame::StyledPanel);
    frame->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    return frame;
}

QToolButton* makeToolButton(QWidget* owner, QStyle::StandardPixmap icon, QString name, QString tooltip) {
    auto* button = new QToolButton(owner);
    button->setObjectName(std::move(name));
    button->setAccessibleName(button->objectName());
    button->setToolTip(std::move(tooltip));
    button->setIcon(owner->style()->standardIcon(icon));
    return button;
}

bte::core::Timestamp makeTimestamp(const int day) {
    using namespace std::chrono;
    return bte::core::Timestamp{sys_days{year{2024} / 1 / day}};
}

std::vector<bte::core::Bar> makeFixtureBars() {
    return {
        {.ts = makeTimestamp(2), .open = 184.0, .high = 188.5, .low = 181.5, .close = 187.0, .volume = 42'000'000.0},
        {.ts = makeTimestamp(3), .open = 187.0, .high = 189.2, .low = 183.8, .close = 184.5, .volume = 38'000'000.0},
        {.ts = makeTimestamp(4), .open = 184.5, .high = 191.0, .low = 184.0, .close = 190.2, .volume = 45'000'000.0},
        {.ts = makeTimestamp(5), .open = 190.2, .high = 193.0, .low = 188.6, .close = 189.4, .volume = 41'000'000.0},
        {.ts = makeTimestamp(6), .open = 189.4, .high = 195.3, .low = 188.9, .close = 194.6, .volume = 48'000'000.0},
    };
}

} // namespace

ReplayTab::ReplayTab(QWidget* parent) : QWidget(parent), replayVm_(new ReplaySessionVm(this)) {
    setObjectName("replayTab");
    setAccessibleName("K-line replay tab");

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(12, 12, 12, 12);
    root->setSpacing(10);

    auto* setupBox = new QGroupBox(tr("Replay setup"), this);
    setupBox->setObjectName("replaySetupBox");
    setupBox->setAccessibleName("Replay setup");
    auto* setupLayout = new QGridLayout(setupBox);

    auto* symbolCombo = new QComboBox(setupBox);
    symbolCombo->setObjectName("replaySymbolCombo");
    symbolCombo->setAccessibleName("Replay symbol");
    symbolCombo->addItems({"AAPL", "MSFT", "NVDA"});

    auto* schemaCombo = new QComboBox(setupBox);
    schemaCombo->setObjectName("replaySchemaCombo");
    schemaCombo->setAccessibleName("Replay timeframe schema");
    schemaCombo->addItems({"ohlcv-1d", "ohlcv-1h", "ohlcv-1m"});

    auto* startDate = new QDateEdit(QDate{2024, 1, 1}, setupBox);
    startDate->setObjectName("replayStartDateEdit");
    startDate->setAccessibleName("Replay start date");
    startDate->setCalendarPopup(true);

    auto* endDate = new QDateEdit(QDate{2024, 6, 30}, setupBox);
    endDate->setObjectName("replayEndDateEdit");
    endDate->setAccessibleName("Replay end date");
    endDate->setCalendarPopup(true);

    auto* initialCapital = new QDoubleSpinBox(setupBox);
    initialCapital->setObjectName("replayInitialCapitalSpinBox");
    initialCapital->setAccessibleName("Replay initial capital");
    initialCapital->setRange(1.0, 1'000'000'000.0);
    initialCapital->setDecimals(2);
    initialCapital->setPrefix("$");
    initialCapital->setValue(100'000.0);

    auto* loadButton = new QPushButton(tr("Load"), setupBox);
    loadButton->setObjectName("replayLoadButton");
    loadButton->setAccessibleName("Load replay data");

    setupLayout->addWidget(makeFormLabel(tr("Symbol"), "replaySymbolLabel", setupBox), 0, 0);
    setupLayout->addWidget(symbolCombo, 0, 1);
    setupLayout->addWidget(makeFormLabel(tr("Timeframe"), "replaySchemaLabel", setupBox), 0, 2);
    setupLayout->addWidget(schemaCombo, 0, 3);
    setupLayout->addWidget(makeFormLabel(tr("Start"), "replayStartDateLabel", setupBox), 1, 0);
    setupLayout->addWidget(startDate, 1, 1);
    setupLayout->addWidget(makeFormLabel(tr("End"), "replayEndDateLabel", setupBox), 1, 2);
    setupLayout->addWidget(endDate, 1, 3);
    setupLayout->addWidget(makeFormLabel(tr("Initial capital"), "replayInitialCapitalLabel", setupBox), 1, 4);
    setupLayout->addWidget(initialCapital, 1, 5);
    setupLayout->addWidget(loadButton, 0, 5);
    root->addWidget(setupBox);

    auto* playbackRow = new QHBoxLayout();
    auto* stepBackButton = makeToolButton(this, QStyle::SP_MediaSkipBackward, "replayStepBackButton", tr("Step back"));
    stepBackButton->setEnabled(false);
    playPauseButton_ = makeToolButton(this, QStyle::SP_MediaPlay, "replayPlayPauseButton", tr("Play or pause"));
    auto* stepForwardButton =
        makeToolButton(this, QStyle::SP_MediaSkipForward, "replayStepForwardButton", tr("Step forward"));

    auto* speedCombo = new QComboBox(this);
    speedCombo->setObjectName("replaySpeedCombo");
    speedCombo->setAccessibleName("Replay speed");
    speedCombo->addItems({"1x", "5x", "10x", "max"});

    progress_ = new QProgressBar(this);
    progress_->setObjectName("replayProgressBar");
    progress_->setAccessibleName("Replay progress");
    progress_->setRange(0, 0);
    progress_->setValue(0);

    playbackRow->addWidget(stepBackButton);
    playbackRow->addWidget(playPauseButton_);
    playbackRow->addWidget(stepForwardButton);
    playbackRow->addSpacing(12);
    playbackRow->addWidget(makeFormLabel(tr("Speed"), "replaySpeedLabel", this));
    playbackRow->addWidget(speedCombo);
    playbackRow->addWidget(progress_, 1);
    root->addLayout(playbackRow);

    auto* chartPanel = makePanel("replayChartPanel");
    auto* chartLayout = new QVBoxLayout(chartPanel);
    chartView_ = new QtChartsCandlestickView(chartPanel);
    chartView_->setMinimumHeight(260);
    chartView_->setBarWindow({});

    auto* volumePlaceholder = new QLabel(tr("Volume"), chartPanel);
    volumePlaceholder->setObjectName("replayVolumePlaceholder");
    volumePlaceholder->setAccessibleName("Volume pane placeholder");
    volumePlaceholder->setAlignment(Qt::AlignCenter);
    volumePlaceholder->setMinimumHeight(80);
    chartLayout->addWidget(chartView_, 4);
    chartLayout->addWidget(volumePlaceholder, 1);
    root->addWidget(chartPanel, 1);

    auto* portfolioBox = new QGroupBox(tr("Portfolio status"), this);
    portfolioBox->setObjectName("replayPortfolioBox");
    portfolioBox->setAccessibleName("Portfolio status");
    auto* portfolioLayout = new QHBoxLayout(portfolioBox);
    portfolioLayout->addWidget(makeValueLabel(tr("Cash: --"), "replayCashLabel"));
    portfolioLayout->addWidget(makeValueLabel(tr("Position: --"), "replayPositionLabel"));
    portfolioLayout->addWidget(makeValueLabel(tr("Equity: --"), "replayEquityLabel"));
    portfolioLayout->addWidget(makeValueLabel(tr("PnL: --"), "replayPnlLabel"));
    root->addWidget(portfolioBox);

    connect(loadButton, &QPushButton::clicked, this, [this] {
        chartView_->setBarWindow({});
        replayVm_->setBars(makeFixtureBars());
    });
    connect(stepForwardButton, &QToolButton::clicked, replayVm_, &ReplaySessionVm::step);
    connect(playPauseButton_, &QToolButton::clicked, this, [this] {
        if (replayVm_->isPlaying()) {
            replayVm_->pause();
        } else {
            replayVm_->play();
        }
    });
    connect(speedCombo, &QComboBox::currentIndexChanged, this, &ReplayTab::configureSpeed);
    connect(replayVm_, &ReplaySessionVm::barAppended, chartView_, &QtChartsCandlestickView::appendBar);
    connect(replayVm_, &ReplaySessionVm::progressChanged, this, [this](const int currentIndex, const int totalBars) {
        progress_->setRange(0, totalBars);
        progress_->setValue(currentIndex);
    });
    connect(replayVm_, &ReplaySessionVm::playbackStateChanged, this, &ReplayTab::updatePlaybackIcon);

    configureSpeed(speedCombo->currentIndex());
    replayVm_->setBars(makeFixtureBars());
}

void ReplayTab::configureSpeed(const int speedIndex) {
    switch (speedIndex) {
    case 1:
        replayVm_->setSpeedMultiplier(5.0);
        break;
    case 2:
        replayVm_->setSpeedMultiplier(10.0);
        break;
    case 3:
        replayVm_->setSpeedMultiplier(std::numeric_limits<double>::infinity());
        break;
    default:
        replayVm_->setSpeedMultiplier(1.0);
        break;
    }
}

void ReplayTab::updatePlaybackIcon(const bool playing) {
    playPauseButton_->setIcon(style()->standardIcon(playing ? QStyle::SP_MediaPause : QStyle::SP_MediaPlay));
}

} // namespace bte::frontend
