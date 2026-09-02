#pragma once

#include "Bte/Frontend/QtChartsCandlestickView.h"

#include <QComboBox>
#include <QDateEdit>
#include <QDoubleSpinBox>
#include <QFrame>
#include <QGroupBox>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QTableWidget>
#include <QToolButton>
#include <QWidget>
#include <QtWidgets/qslider.h>

namespace bte::frontend {

struct ReplaySetupControls {
  QGroupBox *box{};
  QComboBox *resultCombo{};
  QComboBox *resultTimeframeCombo{};
  QPushButton *openResultButton{};
  QLabel *resultStatusLabel{};
  QLabel *partialLabel{};
  QComboBox *symbolCombo{};
  QComboBox *schemaCombo{};
  QDateEdit *startDate{};
  QDateEdit *endDate{};
  QDoubleSpinBox *initialCapital{};
  QPushButton *loadButton{};
};

struct ReplayPlaybackControls {
  QFrame *bar{};
  QToolButton *stepBackButton{};
  QToolButton *playPauseButton{};
  QToolButton *stepForwardButton{};
  QComboBox *speedCombo{};
  QProgressBar *progress{};
  QSlider *seekSlider{};
  QToolButton *zoomOutButton{};
  QToolButton *zoomInButton{};
  QToolButton *zoomResetButton{};
};

struct ReplayChartSection {
  QFrame *panel{};
  QtChartsCandlestickView *chartView{};
};

struct ReplayPortfolioSection {
  QGroupBox *box{};
  QLabel *cashLabel{};
  QLabel *positionLabel{};
  QLabel *marketValueLabel{};
  QLabel *equityLabel{};
  QLabel *pnlLabel{};
  QLabel *lastPriceLabel{};
  QLabel *barIndexLabel{};
};

struct ReplayTradeLogSection {
  QFrame *panel{};
  QTableWidget *table{};
};

ReplaySetupControls makeReplaySetupControls(QWidget *owner);
ReplayPlaybackControls makeReplayPlaybackControls(QWidget *owner);
ReplayChartSection makeReplayChartSection(QWidget *owner);
ReplayPortfolioSection makeReplayPortfolioSection(QWidget *owner);
ReplayTradeLogSection makeReplayTradeLogSection(QWidget *owner);

} // namespace bte::frontend
