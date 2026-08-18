#include "MainWindow.h"

#include <QApplication>
#include <QList>
#include <QObject> // IWYU pragma: keep
#include <QString>
#include <QTimer>
#include <QtCore/qcoreapplication.h>

int main(int argc, char *argv[]) {
  QApplication app(argc, argv);

  bte::app::MainWindow window;
  window.showNormal();
  window.raise();
  window.activateWindow();

  if (QCoreApplication::arguments().contains("--smoke-test")) {
    QTimer::singleShot(0, &app, &QCoreApplication::quit);
  }

  return QApplication::exec();
}
