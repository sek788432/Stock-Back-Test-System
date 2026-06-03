#include "Bte/Engine/Replay.h"

#include "Bte/Core/Bar.h"
#include "Bte/Core/Time.h"
#include "Bte/Data/BarStream.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

bte::core::Bar makeBar(const int day, const double close) {
  using namespace std::chrono;
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
    return bars_[consumed_++];
  }

  std::int64_t totalBars() const noexcept override {
    return static_cast<std::int64_t>(bars_.size());
  }
  std::int64_t consumed() const noexcept override { return consumed_; }
  bte::core::DateRange range() const noexcept override { return {}; }
  std::string symbol() const override { return "AAPL"; }
  std::string schemaName() const override { return "ohlcv-1d"; }

  std::optional<bte::core::Bar> at(const std::int64_t barIndex) const override {
    if (barIndex < 0 || barIndex >= static_cast<std::int64_t>(bars_.size())) {
      return std::nullopt;
    }
    return bars_[barIndex];
  }

  bool seek(const std::int64_t barIndex) noexcept override {
    if (barIndex < 0 || barIndex > static_cast<std::int64_t>(bars_.size())) {
      return false;
    }
    consumed_ = barIndex;
    return true;
  }

private:
  std::vector<bte::core::Bar> bars_;
  std::int64_t consumed_ = 0;
};

std::unique_ptr<bte::data::BarStream> makeStream() {
  return std::make_unique<MemoryBarStream>(
      std::vector{makeBar(2, 101.0), makeBar(3, 102.0), makeBar(4, 103.0)});
}

} // namespace

TEST(ReplayClockTest, intervalForSpeedMultiplier_matchesPlaybackContract) {
  bte::engine::ReplayClock clock{};

  clock.setSpeedMultiplier(1.0);
  EXPECT_EQ(clock.waitInterval(), std::chrono::milliseconds{1000});

  clock.setSpeedMultiplier(5.0);
  EXPECT_EQ(clock.waitInterval(), std::chrono::milliseconds{200});

  clock.setSpeedMultiplier(10.0);
  EXPECT_EQ(clock.waitInterval(), std::chrono::milliseconds{100});

  clock.setSpeedMultiplier(0.0);
  EXPECT_EQ(clock.waitInterval(), std::chrono::milliseconds{0});
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
