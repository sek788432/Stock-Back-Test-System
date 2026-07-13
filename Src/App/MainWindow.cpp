#include "MainWindow.h"

#include "Bte/Frontend/ReplayTab.h"

#include <QLabel>
#include <QMenuBar>
#include <QStatusBar>
#include <QTabWidget>
#include <QWidget>

#include <utility>

namespace bte::app {
namespace {

QWidget *makePlaceholderTab(QString title) {
  auto *label = new QLabel(std::move(title));
  label->setAlignment(Qt::AlignCenter);
  label->setObjectName("placeholderTab");
  label->setAccessibleName(label->text());
  return label;
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

  tabs_ = new QTabWidget(this);
  tabs_->setObjectName("mainTabWidget");
  tabs_->setAccessibleName("Main tabs");
  tabs_->addTab(makePlaceholderTab(tr("Strategies")), tr("Strategies"));
  tabs_->addTab(makePlaceholderTab(tr("Backtest")), tr("Backtest"));
  tabs_->addTab(new frontend::ReplayTab(tabs_), tr("Replay"));
  tabs_->addTab(makePlaceholderTab(tr("Screener")), tr("Screener"));
  tabs_->addTab(makePlaceholderTab(tr("Plugins")), tr("Plugins"));
  tabs_->addTab(makePlaceholderTab(tr("Logs")), tr("Logs"));
  tabs_->setCurrentIndex(2);
  setCentralWidget(tabs_);

  statusBar()->showMessage(tr("Ready"));
}

} // namespace bte::app
