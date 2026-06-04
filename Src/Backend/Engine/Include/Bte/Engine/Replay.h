#pragma once

#include "Bte/Core/Bar.h"
#include "Bte/Data/BarStream.h"

#include <chrono>
#include <memory>
#include <optional>

namespace bte::engine {

struct ReplayProgressSnapshot {
  core::Bar bar{};
  int barIndex = 0;
  int totalBars = 0;
  bool done = false;
};

class ReplayClock {
public:
  void setSpeedMultiplier(double multiplier) noexcept;
  [[nodiscard]] double speedMultiplier() const noexcept;
  [[nodiscard]] std::chrono::milliseconds waitInterval() const noexcept;

private:
  double speedMultiplier_ = 1.0;
  std::chrono::milliseconds intervalAtOneX_{1000};
};

class Replay {
public:
  explicit Replay(std::unique_ptr<data::BarStream> stream);

  [[nodiscard]] std::optional<ReplayProgressSnapshot> step();
  void reset();
  [[nodiscard]] int currentBarIndex() const noexcept;
  [[nodiscard]] int totalBars() const noexcept;
  [[nodiscard]] bool done() const noexcept;

private:
  std::unique_ptr<data::BarStream> stream_;
  int currentBarIndex_ = 0;
};

} // namespace bte::engine
