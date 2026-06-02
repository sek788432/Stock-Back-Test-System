#pragma once

#include <QWidget>

class QProgressBar;
class QToolButton;

namespace bte::frontend {

class QtChartsCandlestickView;
class ReplaySessionVm;

class ReplayTab final : public QWidget {
  public:
    explicit ReplayTab(QWidget* parent = nullptr);

  private:
    void configureSpeed(int speedIndex);
    void updatePlaybackIcon(bool playing);

    ReplaySessionVm* replayVm_ = nullptr;
    QtChartsCandlestickView* chartView_ = nullptr;
    QProgressBar* progress_ = nullptr;
    QToolButton* playPauseButton_ = nullptr;
};

} // namespace bte::frontend
