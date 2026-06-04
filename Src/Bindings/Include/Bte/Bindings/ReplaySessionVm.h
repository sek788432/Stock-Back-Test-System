#pragma once

#include "Bte/Core/Bar.h"

#include <cstddef>
#include <memory>
#include <vector>

namespace bte::engine {
class Replay;
} // namespace bte::engine

namespace bte::bindings {

struct ReplayPortfolioSnapshot {
  double cash = 0.0;
  double position = 0.0;
  double marketValue = 0.0;
  double equity = 0.0;
  double pnl = 0.0;
  double lastPrice = 0.0;
  bool hasLastPrice = false;
};

class ReplaySessionVm {
public:
  ReplaySessionVm();
  ~ReplaySessionVm();

  void setInitialCapital(double initialCapital);
  [[nodiscard]] double initialCapital() const noexcept;

  void reset(std::vector<bte::core::Bar> bars);
  [[nodiscard]] bool stepForward();
  [[nodiscard]] bool stepBack();

  [[nodiscard]] const std::vector<bte::core::Bar> &visibleBars() const noexcept;
  [[nodiscard]] std::size_t currentIndex() const noexcept;
  [[nodiscard]] std::size_t totalBars() const noexcept;
  [[nodiscard]] int progressPercent() const noexcept;
  [[nodiscard]] ReplayPortfolioSnapshot portfolioSnapshot() const noexcept;

private:
  [[nodiscard]] ReplayPortfolioSnapshot makePortfolioSnapshot() const noexcept;
  void rebuildReplayAtCurrentIndex();

  std::vector<bte::core::Bar> bars_;
  std::vector<bte::core::Bar> visibleBars_;
  std::unique_ptr<bte::engine::Replay> replay_;
  std::size_t currentIndex_ = 0;
  double initialCapital_ = 100'000.0;
};

} // namespace bte::bindings
