#include "Bte/Frontend/ReplayBarViewport.h"

// IWYU pragma: no_include <math>

#include <algorithm>
#include <cmath>

namespace bte::frontend {

void ReplayBarViewport::setReachedCount(const std::size_t reachedCount) {
  reachedCount_ = reachedCount;
  if (followingLatest_) {
    followToTail();
  } else {
    clampReviewedPosition();
  }
}

void ReplayBarViewport::setPlotWidth(const double plotWidth) {
  if (std::isfinite(plotWidth) && plotWidth > 0.0) {
    const auto center = firstSlot_ + plotWidth_ / slotWidth_ / 2.0;
    plotWidth_ = plotWidth;
    if (followingLatest_) {
      followToTail();
    } else {
      firstSlot_ = center - plotWidth_ / slotWidth_ / 2.0;
      clampReviewedPosition();
    }
  }
}

void ReplayBarViewport::zoom(const double factor, const double anchorFraction) {
  if (!std::isfinite(factor) || factor <= 0.0 ||
      !std::isfinite(anchorFraction) || reachedCount_ == 0) {
    return;
  }
  const auto anchor = std::clamp(anchorFraction, 0.0, 1.0);
  const auto oldCapacity = plotWidth_ / slotWidth_;
  const auto anchoredSlot = firstSlot_ + anchor * oldCapacity;
  slotWidth_ = std::clamp(slotWidth_ * factor, 6.0, 48.0);
  const auto newCapacity = plotWidth_ / slotWidth_;
  firstSlot_ = anchoredSlot - anchor * newCapacity;
  clampReviewedPosition();
}

void ReplayBarViewport::pan(const double slotDelta) {
  if (!std::isfinite(slotDelta) || reachedCount_ == 0) {
    return;
  }
  followingLatest_ = false;
  firstSlot_ += slotDelta;
  clampReviewedPosition();
}

void ReplayBarViewport::followLatest() {
  followingLatest_ = true;
  followToTail();
}

void ReplayBarViewport::reset() {
  slotWidth_ = 24.0;
  followLatest();
}

ReplayVisibleSlots ReplayBarViewport::visibleSlots() const {
  const auto capacity = std::max(1.0, plotWidth_ / slotWidth_);
  const auto first =
      static_cast<std::size_t>(std::max(0.0, std::floor(firstSlot_)));
  const auto end = static_cast<std::size_t>(std::min(
      static_cast<double>(reachedCount_), std::ceil(firstSlot_ + capacity)));
  return {std::min(first, reachedCount_), end};
}

double ReplayBarViewport::slotWidth() const { return slotWidth_; }

double ReplayBarViewport::firstSlot() const { return firstSlot_; }

double ReplayBarViewport::slotCapacity() const {
  return std::max(1.0, plotWidth_ / slotWidth_);
}

bool ReplayBarViewport::isFollowingLatest() const { return followingLatest_; }

bool ReplayBarViewport::canFitReachedBars() const {
  return reachedCount_ != 0 &&
         plotWidth_ / static_cast<double>(reachedCount_) >= 6.0;
}

bool ReplayBarViewport::fitReachedBars() {
  if (!canFitReachedBars()) {
    return false;
  }
  slotWidth_ =
      std::clamp(plotWidth_ / static_cast<double>(reachedCount_), 6.0, 48.0);
  followLatest();
  return true;
}

void ReplayBarViewport::followToTail() {
  const auto capacity = std::max(1.0, plotWidth_ / slotWidth_);
  const auto reached = static_cast<double>(reachedCount_);
  firstSlot_ =
      reached >= capacity ? reached - capacity : (reached - capacity) / 2.0;
}

void ReplayBarViewport::clampReviewedPosition() {
  const auto capacity = std::max(1.0, plotWidth_ / slotWidth_);
  const auto reached = static_cast<double>(reachedCount_);
  if (reached <= capacity) {
    firstSlot_ = (reached - capacity) / 2.0;
    return;
  }
  firstSlot_ = std::clamp(firstSlot_, 0.0, reached - capacity);
}

} // namespace bte::frontend
