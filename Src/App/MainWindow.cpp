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

QWidget* makePlaceholderTab(QString title) {
    auto* label = new QLabel(std::move(title));
    label->setAlignment(Qt::AlignCenter);
    label->setObjectName("placeholderTab");
    label->setAccessibleName(label->text());
    return label;
}

}  // namespace

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent) {
    setObjectName("mainWindow");
    setAccessibleName("Stock Backtester main window");
    setWindowTitle(tr("Stock Backtester"));
    resize(1280, 800);

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

}  // namespace bte::app
