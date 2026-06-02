#pragma once

#include "Bte/Core/Bar.h"

#include <QObject>
#include <QTimer>

#include <cstddef>
#include <vector>

namespace bte::frontend {

class ReplaySessionVm final : public QObject {
    Q_OBJECT

  public:
    explicit ReplaySessionVm(QObject* parent = nullptr);

    void setBars(std::vector<core::Bar> bars);
    void setSpeedMultiplier(double speedMultiplier);

    [[nodiscard]] int currentIndex() const noexcept;
    [[nodiscard]] int totalBars() const noexcept;
    [[nodiscard]] bool isPlaying() const noexcept;
    [[nodiscard]] bool isCompleted() const noexcept;
    [[nodiscard]] double speedMultiplier() const noexcept;

  public slots:
    void step();
    void play();
    void pause();

  signals:
    void barAppended(const bte::core::Bar& bar);
    void progressChanged(int currentIndex, int totalBars);
    void playbackStateChanged(bool playing);
    void completed();

  private slots:
    void drainRemainingBars();

  private:
    [[nodiscard]] int timerIntervalMs() const noexcept;
    void stopPlayback(bool emitStateChange);

    std::vector<core::Bar> bars_;
    int currentIndex_ = 0;
    double speedMultiplier_ = 1.0;
    bool playing_ = false;
    QTimer timer_;
};

} // namespace bte::frontend
