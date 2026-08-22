#include "Bte/Bindings/ReplaySessionVm.h"

#include "Bte/Data/BarStream.h"
#include "Bte/Engine/Replay.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>

namespace {

class ReplayVectorBarStream final : public bte::data::BarStream {
public:
  explicit ReplayVectorBarStream(std::vector<bte::core::Bar> bars)
      : bars_(std::move(bars)) {}

  std::optional<bte::core::Bar> next() override {
    if (consumed_ >= static_cast<std::int64_t>(bars_.size())) {
      return std::nullopt;
    }
    return bars_[consumed_++];
  }

  [[nodiscard]] std::int64_t totalBars() const noexcept override {
    return static_cast<std::int64_t>(bars_.size());
  }

  [[nodiscard]] std::int64_t consumed() const noexcept override {
    return consumed_;
  }

  void reset() noexcept override { consumed_ = 0; }

private:
  std::vector<bte::core::Bar> bars_;
  std::int64_t consumed_ = 0;
};

} // namespace

namespace bte::bindings {

ReplaySessionVm::ReplaySessionVm() = default;

ReplaySessionVm::~ReplaySessionVm() = default;

void ReplaySessionVm::setInitialCapital(const double initialCapital) {
  initialCapital_ = std::max(0.0, initialCapital);
}

double ReplaySessionVm::initialCapital() const noexcept {
  return initialCapital_;
}

void ReplaySessionVm::reset(std::vector<bte::core::Bar> bars) {
  bars_ = std::move(bars);
  visibleBars_.clear();
  currentIndex_ = 0;
  replay_ = std::make_unique<bte::engine::Replay>(
      std::make_unique<ReplayVectorBarStream>(bars_));
}

bool ReplaySessionVm::stepForward() {
  if (replay_ == nullptr) {
    return false;
  }

  auto progress = replay_->step();
  if (!progress.has_value()) {
    currentIndex_ = bars_.size();
    return false;
  }

  visibleBars_.push_back(progress->bar);
  currentIndex_ = static_cast<std::size_t>(progress->barIndex);
  return true;
}

bool ReplaySessionVm::stepBack() {
  if (visibleBars_.empty()) {
    return false;
  }

  visibleBars_.pop_back();
  currentIndex_ = visibleBars_.size();
  rewindReplayToCurrentIndex();
  return true;
}

const std::vector<bte::core::Bar> &
ReplaySessionVm::visibleBars() const noexcept {
  return visibleBars_;
}

std::size_t ReplaySessionVm::currentIndex() const noexcept {
  return currentIndex_;
}

std::size_t ReplaySessionVm::totalBars() const noexcept { return bars_.size(); }

int ReplaySessionVm::progressPercent() const noexcept {
  if (bars_.empty()) {
    return 0;
  }

  const auto percent = static_cast<int>((currentIndex_ * 100) / bars_.size());
  return currentIndex_ == 0 ? 0 : std::max(1, percent);
}

ReplayPortfolioSnapshot ReplaySessionVm::portfolioSnapshot() const noexcept {
  return makePortfolioSnapshot();
}

ReplayPortfolioSnapshot
ReplaySessionVm::makePortfolioSnapshot() const noexcept {
  constexpr auto position = 0.0;
  const auto hasLastPrice = !visibleBars_.empty();
  const auto lastPrice = hasLastPrice ? visibleBars_.back().close : 0.0;
  const auto marketValue = position * lastPrice;
  const auto equity = initialCapital_ + marketValue;

  return ReplayPortfolioSnapshot{
      .cash = initialCapital_,
      .position = position,
      .marketValue = marketValue,
      .equity = equity,
      .pnl = equity - initialCapital_,
      .lastPrice = lastPrice,
      .hasLastPrice = hasLastPrice,
  };
}

void ReplaySessionVm::rewindReplayToCurrentIndex() {
  replay_->reset();
  for (std::size_t index = 0; index < currentIndex_; ++index) {
    (void)replay_->step();
  }
}

} // namespace bte::bindings
