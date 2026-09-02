#include "MainWindow.h"

#include "Bte/Frontend/BacktestTab.h"
#include "Bte/Frontend/ReplayTab.h"

#include <QLabel>
#include <QMenuBar>
#include <QObject>
#include <QStatusBar>
#include <QString>
#include <QtCore/Qt>

#include <memory>

namespace bte::app {
namespace {

QWidget *makePlaceholderTab(const QString &message) {
  auto label = std::make_unique<QLabel>(message);
  label->setAlignment(Qt::AlignCenter);
  label->setObjectName("placeholderTab");
  label->setAccessibleName(label->text());
  return label.release();
}

} // namespace

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent) {
  setObjectName("mainWindow");
  setAccessibleName("Stock Backtester main window");
  setWindowTitle(tr("Stock Backtester"));
  resize(1440, 900);
  setStyleSheet(R"(
        QMainWindow {
            background: #07111b;
        }
        QMenuBar {
            background: #07111b;
            border-bottom: 1px solid #22384f;
            color: #dce9f5;
            spacing: 4px;
        }
        QMenuBar::item {
            background: transparent;
            padding: 7px 13px;
        }
        QMenuBar::item:selected {
            background: #112235;
            border-radius: 4px;
        }
        QMenu {
            background: #0b1724;
            border: 1px solid #2b4663;
            color: #dce9f5;
        }
        QTabWidget::pane {
            border: 0;
            top: -1px;
        }
        QTabBar::tab {
            background: #0a1521;
            border: 1px solid #22384f;
            border-bottom: 0;
            color: #b7cbdd;
            min-width: 96px;
            padding: 9px 18px;
        }
        QTabBar::tab:selected {
            background: #07111b;
            color: #ffffff;
            border-top: 2px solid #27b6c7;
        }
        QTabBar::tab:hover:!selected {
            background: #0f2030;
            color: #dce9f5;
        }
        QStatusBar {
            background: #07111b;
            border-top: 1px solid #22384f;
            color: #89a6bd;
        }
    )");

  menuBar()->addMenu(tr("&File"));
  menuBar()->addMenu(tr("&Strategy"));
  menuBar()->addMenu(tr("&Data"));
  menuBar()->addMenu(tr("&View"));
  menuBar()->addMenu(tr("&Help"));

  tabs_ = std::make_unique<QTabWidget>(this).release();
  tabs_->setObjectName("mainTabWidget");
  tabs_->setAccessibleName("Main tabs");
  tabs_->addTab(
      makePlaceholderTab(tr("Saved Strategy persistence is planned.")),
      tr("Strategies"));
  auto backtestOwner =
      frontend::BacktestTab::createApplicationConfigured(tabs_);
  auto *backtest = backtestOwner.release();
  tabs_->addTab(backtest, tr("Backtest"));
  tabs_->addTab(
      makePlaceholderTab(tr("Result library management is planned; stored "
                            "results can be opened in Replay.")),
      tr("Results"));
  auto *replay = std::make_unique<frontend::ReplayTab>(tabs_).release();
  const auto replayIndex = tabs_->addTab(replay, tr("Replay"));
  QObject::connect(backtest, &frontend::BacktestTab::openResultInReplay, this,
                   [this, replay, replayIndex](const QString &resultId) {
                     replay->openResult(resultId);
                     tabs_->setCurrentIndex(replayIndex);
                   });
  tabs_->setCurrentIndex(1);
  setCentralWidget(tabs_);

  statusBar()->showMessage(tr("Ready"));
}

} // namespace bte::app
