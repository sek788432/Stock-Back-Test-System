#include "MainWindow.h"

#include "Bte/Frontend/BacktestTab.h"
#include "Bte/Frontend/ReplayTab.h"
#include "Bte/Frontend/ResultsTab.h"

#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QObject>
#include <QStatusBar>
#include <QString>
#include <QtCore/Qt>
#include <QtCore/qtenvironmentvariables.h>
#include <QtGui/qaction.h>

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

QString launcherValue(const char *name) {
  const auto value = qEnvironmentVariable(name);
  return value.isEmpty() ? QString{"unavailable"} : value;
}

QString buildInformation() {
  return QString{"Checkout: %1\nBranch: %2\nCommit: %3\nDirty: %4\n"
                 "App SHA-256: %5\nSnapshot ID: %6"}
      .arg(launcherValue("BTE_LAUNCH_CHECKOUT"),
           launcherValue("BTE_LAUNCH_BRANCH"),
           launcherValue("BTE_LAUNCH_COMMIT"),
           launcherValue("BTE_LAUNCH_DIRTY"),
           launcherValue("BTE_LAUNCH_APP_SHA256"),
           launcherValue("BTE_DATA_SNAPSHOT_ID"));
} // GCOVR_EXCL_LINE: the compiler emits no executable instruction here.

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
  auto *helpMenu = menuBar()->addMenu(tr("&Help"));
  auto *buildInformationAction = helpMenu->addAction(tr("Build Information"));
  buildInformationAction->setObjectName("buildInformationAction");
  QObject::connect(buildInformationAction, &QAction::triggered, this, [this] {
    auto *dialog = std::make_unique<QMessageBox>(
                       QMessageBox::Information, tr("Build Information"),
                       buildInformation(), QMessageBox::Ok, this)
                       .release();
    dialog->setObjectName("buildInformationDialog");
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->show();
  });

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
  auto *results = std::make_unique<frontend::ResultsTab>(tabs_).release();
  const auto resultsIndex = tabs_->addTab(results, tr("Results"));
  auto *replay = std::make_unique<frontend::ReplayTab>(tabs_).release();
  const auto replayIndex = tabs_->addTab(replay, tr("Replay"));
  QObject::connect(backtest, &frontend::BacktestTab::openResultInReplay, this,
                   [this, replay, replayIndex](const QString &resultId) {
                     replay->openResult(resultId);
                     tabs_->setCurrentIndex(replayIndex);
                   });
  QObject::connect(results, &frontend::ResultsTab::openResultInReplay, this,
                   [this, replay, replayIndex](const QString &resultId) {
                     replay->openResult(resultId);
                     tabs_->setCurrentIndex(replayIndex);
                   });
  QObject::connect(results, &frontend::ResultsTab::resultMovedToTrash, replay,
                   &frontend::ReplayTab::closeResultIfId);
  QObject::connect(tabs_, &QTabWidget::currentChanged, results,
                   [results, resultsIndex](const int index) {
                     if (index == resultsIndex) {
                       results->refresh();
                     }
                   });
  tabs_->setCurrentIndex(1);
  setCentralWidget(tabs_);

  statusBar()->showMessage(tr("Ready"));
}

} // namespace bte::app
