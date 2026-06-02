#include "Bte/Frontend/ReplaySessionVm.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace bte::frontend {

ReplaySessionVm::ReplaySessionVm(QObject* parent) : QObject(parent) {
    timer_.setTimerType(Qt::PreciseTimer);
    timer_.setSingleShot(false);
    connect(&timer_, &QTimer::timeout, this, &ReplaySessionVm::step);
}

void ReplaySessionVm::setBars(std::vector<core::Bar> bars) {
    stopPlayback(true);
    bars_ = std::move(bars);
    currentIndex_ = 0;
    emit progressChanged(currentIndex_, totalBars());
}

void ReplaySessionVm::setSpeedMultiplier(const double speedMultiplier) {
    speedMultiplier_ = speedMultiplier > 0.0 ? speedMultiplier : 1.0;
    if (!playing_) {
        return;
    }

    if (std::isfinite(speedMultiplier_)) {
        timer_.start(timerIntervalMs());
    } else {
        timer_.stop();
        QTimer::singleShot(0, this, &ReplaySessionVm::drainRemainingBars);
    }
}

int ReplaySessionVm::currentIndex() const noexcept { return currentIndex_; }

int ReplaySessionVm::totalBars() const noexcept { return static_cast<int>(bars_.size()); }

bool ReplaySessionVm::isPlaying() const noexcept { return playing_; }

bool ReplaySessionVm::isCompleted() const noexcept { return currentIndex_ >= totalBars(); }

double ReplaySessionVm::speedMultiplier() const noexcept { return speedMultiplier_; }

void ReplaySessionVm::step() {
    if (isCompleted()) {
        stopPlayback(true);
        return;
    }

    emit barAppended(bars_[static_cast<std::size_t>(currentIndex_)]);
    ++currentIndex_;
    emit progressChanged(currentIndex_, totalBars());

    if (isCompleted()) {
        stopPlayback(true);
        emit completed();
    }
}

void ReplaySessionVm::play() {
    if (playing_ || isCompleted()) {
        return;
    }

    playing_ = true;
    emit playbackStateChanged(true);

    if (!std::isfinite(speedMultiplier_)) {
        QTimer::singleShot(0, this, &ReplaySessionVm::drainRemainingBars);
        return;
    }

    timer_.start(timerIntervalMs());
}

void ReplaySessionVm::pause() { stopPlayback(true); }

void ReplaySessionVm::drainRemainingBars() {
    while (playing_ && !isCompleted()) {
        step();
    }
}

int ReplaySessionVm::timerIntervalMs() const noexcept {
    constexpr auto baseIntervalMs = 1000.0;
    return std::max(1, static_cast<int>(baseIntervalMs / speedMultiplier_));
}

void ReplaySessionVm::stopPlayback(const bool emitStateChange) {
    timer_.stop();
    if (!playing_) {
        return;
    }

    playing_ = false;
    if (emitStateChange) {
        emit playbackStateChanged(false);
    }
}

} // namespace bte::frontend
