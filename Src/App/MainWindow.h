#pragma once

#include <QMainWindow>
#include <QTabWidget>
#include <QWidget>

namespace bte::app {

class MainWindow final : public QMainWindow {
  public:
    explicit MainWindow(QWidget* parent = nullptr);

  private:
    QTabWidget* tabs_ = nullptr;
};

} // namespace bte::app
