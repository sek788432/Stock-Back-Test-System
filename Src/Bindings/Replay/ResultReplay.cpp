#include "Bte/Bindings/ResultReplay.h"

#include "Bte/Core/Result.h"
#include "Bte/Core/Time.h"
#include "Bte/Data/ReleaseSnapshot.h"
#include "Bte/Results/ResultStore.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace bte::bindings {
namespace {

constexpr auto nanodollarsPerDollar = 1'000'000'000.0;
constexpr auto microdollarsPerDollar = 1'000'000.0;
constexpr auto microsharesPerShare = 1'000'000.0;
constexpr std::size_t maximumVisibleFrames = 500;

double price(const std::int64_t nanodollars) {
  return static_cast<double>(nanodollars) / nanodollarsPerDollar;
}

double money(const std::int64_t microdollars) {
  return static_cast<double>(microdollars) / microdollarsPerDollar;
}

core::Bar toBar(const data::SnapshotBar &bar) {
  return {.ts = bar.timestamp,
          .open = price(bar.openNanodollars),
          .high = price(bar.highNanodollars),
          .low = price(bar.lowNanodollars),
          .close = price(bar.closeNanodollars),
          .volume =
              static_cast<double>(bar.volumeMicroshares) / microsharesPerShare};
}

class ReplayRecordIndex final {
public:
  explicit ReplayRecordIndex(const results::OpenedResult &result) {
    for (const auto &record : result.records) {
      if (record.family == results::RecordFamily::portfolio &&
          record.cashMicrodollars.has_value() &&
          record.marketValueMicrodollars.has_value() &&
          record.equityMicrodollars.has_value() &&
          record.positionShares.has_value()) {
        portfolios_.push_back({
            .timestamp = record.timestamp,
            .portfolio = {.cash = money(*record.cashMicrodollars),
                          .positionShares = *record.positionShares,
                          .marketValue = money(*record.marketValueMicrodollars),
                          .equity = money(*record.equityMicrodollars),
                          .pnl = money(
                              *record.equityMicrodollars -
                              result.descriptor.initialCapitalMicrodollars)},
        });
      } else if (record.family == results::RecordFamily::fill &&
                 record.quantityShares.has_value() &&
                 record.priceNanodollars.has_value() &&
                 record.amountMicrodollars.has_value()) {
        fills_.push_back({
            .timestamp = record.timestamp,
            .isBuy = record.side == results::OrderSide::buy,
            .quantityShares = *record.quantityShares,
            .price = price(*record.priceNanodollars),
            .amount = money(*record.amountMicrodollars),
        });
      }
    }
  }

  [[nodiscard]] std::optional<ResultReplayPortfolio>
  portfolioAt(const core::Timestamp timestamp) const {
    const auto end = std::ranges::upper_bound(portfolios_, timestamp, {},
                                              &TimedPortfolio::timestamp);
    if (end == portfolios_.begin()) {
      return std::nullopt;
    }
    return std::prev(end)->portfolio;
  }

  [[nodiscard]] std::vector<ResultReplayFill>
  fillsAt(const core::Timestamp start, const core::Timestamp end) const {
    const auto first = std::ranges::lower_bound(fills_, start, {},
                                                &ResultReplayFill::timestamp);
    const auto last = std::ranges::lower_bound(first, fills_.end(), end, {},
                                               &ResultReplayFill::timestamp);
    return {first, last};
  }

private:
  struct TimedPortfolio {
    core::Timestamp timestamp;
    ResultReplayPortfolio portfolio;
  };

  std::vector<TimedPortfolio> portfolios_;
  std::vector<ResultReplayFill> fills_;
};

core::Result<std::vector<ResultReplayFrame>>
makeHourlyFrames(const results::OpenedResult &result,
                 const std::vector<data::SnapshotBar> &bars,
                 const core::CancellationToken &cancellation) {
  const ReplayRecordIndex records{result};
  std::vector<ResultReplayFrame> frames;
  frames.reserve(bars.size());
  for (const auto &bar : bars) {
    if (cancellation.isCancellationRequested()) {
      return core::makeError(core::ErrorCode::cancelled,
                             "Result Replay open was cancelled");
    }
    const auto portfolio = records.portfolioAt(bar.timestamp);
    if (!portfolio.has_value()) {
      continue;
    }
    frames.push_back({
        .candle = toBar(bar),
        .fills = records.fillsAt(bar.timestamp,
                                 bar.timestamp + std::chrono::milliseconds{1}),
        .portfolio = *portfolio,
        .partialUtcDay = false,
    });
  }
  return frames;
}

core::Result<std::vector<ResultReplayFrame>>
makeDailyFrames(const results::OpenedResult &result,
                const std::vector<data::SnapshotBar> &bars,
                const core::CancellationToken &cancellation) {
  using namespace std::chrono;
  const ReplayRecordIndex records{result};
  std::vector<ResultReplayFrame> frames;
  std::size_t begin = 0;
  while (begin < bars.size()) {
    if (cancellation.isCancellationRequested()) {
      return core::makeError(core::ErrorCode::cancelled,
                             "Result Replay open was cancelled");
    }
    const auto day = floor<days>(bars[begin].timestamp);
    auto end = begin + 1;
    auto high = bars[begin].highNanodollars;
    auto low = bars[begin].lowNanodollars;
    auto volume = bars[begin].volumeMicroshares;
    while (end < bars.size() && floor<days>(bars[end].timestamp) == day) {
      high = std::max(high, bars[end].highNanodollars);
      low = std::min(low, bars[end].lowNanodollars);
      if (bars[end].volumeMicroshares >
          std::numeric_limits<std::int64_t>::max() - volume) {
        return core::makeError(core::ErrorCode::invalidArgument,
                               "Daily Replay volume exceeds supported range");
      }
      volume += bars[end].volumeMicroshares;
      ++end;
    }
    const auto bucketEnd = core::Timestamp{day + days{1}};
    const auto portfolio = records.portfolioAt(bars[end - 1].timestamp);
    if (portfolio.has_value()) {
      frames.push_back({
          .candle = {.ts = core::Timestamp{day},
                     .open = price(bars[begin].openNanodollars),
                     .high = price(high),
                     .low = price(low),
                     .close = price(bars[end - 1].closeNanodollars),
                     .volume =
                         static_cast<double>(volume) / microsharesPerShare},
          .fills = records.fillsAt(core::Timestamp{day}, bucketEnd),
          .portfolio = *portfolio,
          .partialUtcDay = end - begin != 24,
      });
    }
    begin = end;
  }
  return frames;
}

} // namespace

ResultReplay::ResultReplay(ConstructionKey, std::string resultId,
                           std::vector<ResultReplayFrame> frames,
                           std::string terminalReason)
    : resultId_(std::move(resultId)), frames_(std::move(frames)),
      terminalReason_(std::move(terminalReason)) {}

core::Result<std::unique_ptr<ResultReplay>>
ResultReplay::open(const std::filesystem::path &resultStore,
                   const std::filesystem::path &dataStore,
                   const std::string &resultId,
                   const ResultReplayTimeframe timeframe,
                   const core::CancellationToken &cancellation) {
  if (cancellation.isCancellationRequested()) {
    return core::makeError(core::ErrorCode::cancelled,
                           "Result Replay open was cancelled");
  }
  auto store = results::ResultStore::open(resultStore, dataStore);
  if (!store.ok()) {
    return store.error();
  }
  auto result = store.value()->openResult(resultId);
  if (!result.ok()) {
    return result.error();
  }
  auto reader = data::ReleaseSnapshotReader::open(
      dataStore, result.value().descriptor.dataSelection.snapshotId,
      cancellation);
  if (!reader.ok()) {
    return reader.error();
  }
  auto selected =
      reader.value()->select({.symbols = result.value().descriptor.universe,
                              .range = result.value().descriptor.range,
                              .timeframe = "ohlcv-1h"},
                             cancellation);
  if (!selected.ok()) {
    return selected.error();
  }
  auto bars = std::move(selected).value().bars;
  if (result.value().status != results::RunStatus::completed &&
      !result.value().records.empty()) {
    const auto lastTimestamp = result.value().records.back().timestamp;
    const auto firstFuture = std::ranges::upper_bound(
        bars, lastTimestamp, {}, &data::SnapshotBar::timestamp);
    bars.erase(firstFuture, bars.end());
  }
  auto frames = timeframe == ResultReplayTimeframe::hourly
                    ? makeHourlyFrames(result.value(), bars, cancellation)
                    : makeDailyFrames(result.value(), bars, cancellation);
  if (!frames.ok()) {
    return frames.error();
  }
  return std::make_unique<ResultReplay>(ConstructionKey{}, resultId,
                                        std::move(frames).value(),
                                        result.value().terminalReason);
}

core::Result<std::vector<results::ResultSummary>>
ResultReplay::list(const std::filesystem::path &resultStore,
                   const std::filesystem::path &dataStore,
                   const core::CancellationToken &cancellation) {
  if (cancellation.isCancellationRequested()) {
    return core::makeError(core::ErrorCode::cancelled,
                           "Result catalog load was cancelled");
  }
  auto store = results::ResultStore::open(resultStore, dataStore);
  if (!store.ok()) {
    return store.error();
  }
  return store.value()->list();
}

const std::string &ResultReplay::resultId() const noexcept { return resultId_; }

const ResultReplayFrame *ResultReplay::current() const noexcept {
  return frames_.empty() ? nullptr : &frames_[currentIndex_];
}

bool ResultReplay::stepForward() noexcept {
  if (frames_.empty() || currentIndex_ + 1 >= frames_.size()) {
    return false;
  }
  ++currentIndex_;
  return true;
}

bool ResultReplay::stepBack() noexcept {
  if (frames_.empty() || currentIndex_ == 0) {
    return false;
  }
  --currentIndex_;
  return true;
}

bool ResultReplay::seek(const std::size_t index) noexcept {
  if (index >= frames_.size()) {
    return false;
  }
  currentIndex_ = index;
  return true;
}

std::size_t ResultReplay::currentIndex() const noexcept {
  return frames_.empty() ? 0 : currentIndex_;
}

std::size_t ResultReplay::totalFrames() const noexcept {
  return frames_.size();
}

int ResultReplay::progressPercent() const noexcept {
  if (frames_.empty()) {
    return 0;
  }
  return static_cast<int>(((currentIndex_ + 1) * 100) / frames_.size());
}

std::vector<ResultReplayFrame> ResultReplay::visibleWindow() const {
  if (frames_.empty()) {
    return {};
  }
  const auto end = currentIndex_ + 1;
  const auto begin =
      end > maximumVisibleFrames ? end - maximumVisibleFrames : 0;
  return {frames_.begin() + static_cast<std::ptrdiff_t>(begin),
          frames_.begin() + static_cast<std::ptrdiff_t>(end)};
}

const std::string &ResultReplay::terminalReason() const noexcept {
  return terminalReason_;
}

} // namespace bte::bindings
