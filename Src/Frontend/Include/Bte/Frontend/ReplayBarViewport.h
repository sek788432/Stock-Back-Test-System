#pragma once

#include <cstddef>

namespace bte::frontend {

struct ReplayVisibleSlots {
  std::size_t first = 0;
  std::size_t end = 0;
};

class ReplayBarViewport final {
public:
  void setReachedCount(std::size_t reachedCount);
  void setPlotWidth(double plotWidth);
  void zoom(double factor, double anchorFraction);
  void pan(double slotDelta);
  void followLatest();
  void reset();
  [[nodiscard]] ReplayVisibleSlots visibleSlots() const;
  [[nodiscard]] double slotWidth() const;
  [[nodiscard]] double firstSlot() const;
  [[nodiscard]] double slotCapacity() const;
  [[nodiscard]] bool isFollowingLatest() const;
  [[nodiscard]] bool canFitReachedBars() const;
  [[nodiscard]] bool fitReachedBars();

private:
  void followToTail();
  void clampReviewedPosition();

  std::size_t reachedCount_ = 0;
  double plotWidth_ = 900.0;
  double slotWidth_ = 24.0;
  double firstSlot_ = 0.0;
  bool followingLatest_ = true;
};

} // namespace bte::frontend
