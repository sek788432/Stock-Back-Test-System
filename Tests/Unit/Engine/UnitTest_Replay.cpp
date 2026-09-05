#include "Bte/Engine/Replay.h"

#include "Bte/Core/Bar.h"
#include "Bte/Core/Time.h"
#include "Bte/Data/BarStream.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace {

bte::core::Bar makeBar(const int day, const double close) {
  using std::chrono::sys_days;
  using std::chrono::year;
  return bte::core::Bar{
      .ts = bte::core::Timestamp{sys_days{year{2024} / 1 / day}},
      .open = close - 1.0,
      .high = close + 1.0,
      .low = close - 2.0,
      .close = close,
      .volume = 1'000'000.0,
  };
}

class MemoryBarStream final : public bte::data::BarStream {
public:
  explicit MemoryBarStream(std::vector<bte::core::Bar> bars)
      : bars_(std::move(bars)) {}

  std::optional<bte::core::Bar> next() override {
    if (consumed_ >= static_cast<std::int64_t>(bars_.size())) {
      return std::nullopt;
    }
    return bars_[static_cast<std::size_t>(consumed_++)];
  }

  std::int64_t totalBars() const noexcept override {
    return static_cast<std::int64_t>(bars_.size());
  }
  std::int64_t consumed() const noexcept override { return consumed_; }

  void reset() noexcept override { consumed_ = 0; }

private:
  std::vector<bte::core::Bar> bars_;
  std::int64_t consumed_ = 0;
};

std::unique_ptr<bte::data::BarStream> makeStream() {
  return std::make_unique<MemoryBarStream>(
      std::vector{makeBar(2, 101.0), makeBar(3, 102.0), makeBar(4, 103.0)});
}

} // namespace

TEST(ReplayTest, nullStreamIsAStableCompletedReplay) {
  bte::engine::Replay replay{nullptr};

  EXPECT_FALSE(replay.step().has_value());
  EXPECT_EQ(replay.currentBarIndex(), 0);
  EXPECT_EQ(replay.totalBars(), 0);
  EXPECT_TRUE(replay.done());

  replay.reset();
  EXPECT_EQ(replay.currentBarIndex(), 0);
  EXPECT_TRUE(replay.done());
}

TEST(ReplayTest, step_returnsOneDeterministicProgressSnapshotPerBar) {
  bte::engine::Replay replay{makeStream()};

  const auto first = replay.step();
  const auto second = replay.step();

  ASSERT_TRUE(first.has_value());
  ASSERT_TRUE(second.has_value());
  EXPECT_EQ(first->barIndex, 1);
  EXPECT_EQ(first->totalBars, 3);
  EXPECT_EQ(first->bar.close, 101.0);
  EXPECT_FALSE(first->done);
  EXPECT_EQ(second->barIndex, 2);
  EXPECT_EQ(second->totalBars, 3);
  EXPECT_EQ(second->bar.close, 102.0);
  EXPECT_FALSE(second->done);
}

TEST(ReplayTest, step_returnsDoneOnLastBarAndNulloptAfterEnd) {
  bte::engine::Replay replay{makeStream()};

  ASSERT_TRUE(replay.step().has_value());
  ASSERT_TRUE(replay.step().has_value());
  const auto third = replay.step();
  const auto end = replay.step();

  ASSERT_TRUE(third.has_value());
  EXPECT_EQ(third->barIndex, 3);
  EXPECT_EQ(third->totalBars, 3);
  EXPECT_TRUE(third->done);
  EXPECT_FALSE(end.has_value());
}

TEST(ReplayTest, reset_rewindsStreamAndState) {
  bte::engine::Replay replay{makeStream()};
  ASSERT_TRUE(replay.step().has_value());
  ASSERT_TRUE(replay.step().has_value());

  replay.reset();
  const auto firstAgain = replay.step();

  ASSERT_TRUE(firstAgain.has_value());
  EXPECT_EQ(firstAgain->barIndex, 1);
  EXPECT_EQ(firstAgain->bar.close, 101.0);
}
