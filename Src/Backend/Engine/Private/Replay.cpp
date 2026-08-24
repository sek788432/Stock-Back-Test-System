#include "Bte/Engine/Replay.h"

#include <utility>

namespace bte::engine {

Replay::Replay(std::unique_ptr<data::BarStream> stream)
    : stream_(std::move(stream)) {}

std::optional<ReplayProgressSnapshot> Replay::step() {
  if (!stream_) {
    return std::nullopt;
  }

  auto bar = stream_->next();
  if (!bar.has_value()) {
    currentBarIndex_ = totalBars();
    return std::nullopt;
  }

  currentBarIndex_ = static_cast<int>(stream_->consumed());
  return ReplayProgressSnapshot{
      .bar = *bar,
      .barIndex = currentBarIndex_,
      .totalBars = totalBars(),
      .done = currentBarIndex_ >= totalBars(),
  };
}

void Replay::reset() {
  if (!stream_) {
    currentBarIndex_ = 0;
    return;
  }
  stream_->reset();
  currentBarIndex_ = 0;
}

int Replay::currentBarIndex() const noexcept { return currentBarIndex_; }

int Replay::totalBars() const noexcept {
  return stream_ == nullptr ? 0 : static_cast<int>(stream_->totalBars());
}

bool Replay::done() const noexcept { return currentBarIndex_ >= totalBars(); }

} // namespace bte::engine
