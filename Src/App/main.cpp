#include "MainWindow.h"

#include <QApplication>

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);

    bte::app::MainWindow window;
    window.show();

    return QApplication::exec();
}
