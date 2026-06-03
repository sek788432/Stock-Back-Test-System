#include "MainWindow.h"

#include <QApplication>

int main(int argc, char *argv[]) {
  QApplication app(argc, argv);

  bte::app::MainWindow window;
  window.showNormal();
  window.raise();
  window.activateWindow();

  return QApplication::exec();
}
