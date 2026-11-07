#include "Bte/Bindings/ResultReplay.h"

#include "Bte/Core/Result.h"
#include "Bte/Core/Time.h"
#include "Bte/Data/ReleaseSnapshot.h"
#include "Bte/Results/ResultStore.h"

#if defined(BTE_ENABLE_TEST_HOOKS)
#include "ResultReplayTestHooks.h"
#endif

#include <algorithm>
#include <bit>
#include <chrono>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <ranges> // IWYU pragma: keep
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#if defined(BTE_ENABLE_TEST_HOOKS)
#include <atomic>
#endif

namespace bte::bindings {
namespace {

#if defined(BTE_ENABLE_TEST_HOOKS)
std::atomic<core::CancellationSource *> &
nextPortfolioSnapshotCancellationSource() {
  static std::atomic<core::CancellationSource *> source{nullptr};
  return source;
}

std::atomic<testing::PortfolioCompletionCancellationPoint> &
nextPortfolioSnapshotCancellationPoint() {
  static std::atomic<testing::PortfolioCompletionCancellationPoint> point{
      testing::PortfolioCompletionCancellationPoint::trackerCreation};
  return point;
}

void requestPortfolioSnapshotCancellationAt(
    const testing::PortfolioCompletionCancellationPoint point) {
  auto expected = point;
  if (!nextPortfolioSnapshotCancellationPoint().compare_exchange_strong(
          expected,
          testing::PortfolioCompletionCancellationPoint::trackerCreation)) {
    return;
  }
  if (const auto source =
          nextPortfolioSnapshotCancellationSource().exchange(nullptr);
      source != nullptr) {
    source->requestCancellation();
  }
}
#endif

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

core::Result<void>
validateReplayRecords(const std::vector<results::CanonicalRecord> &records,
                      const std::string_view expectedSymbol) {
  for (const auto &record : records) {
    if (record.symbol != expectedSymbol ||
        record.family < results::RecordFamily::order ||
        record.family > results::RecordFamily::terminalDiagnostic ||
        record.side < results::OrderSide::none ||
        record.side > results::OrderSide::sell) {
      return core::makeError(core::ErrorCode::schemaMismatch,
                             "Canonical Replay record enum is invalid");
    }
    if (record.family == results::RecordFamily::fill &&
        (record.side == results::OrderSide::none ||
         !record.quantityShares.has_value() || *record.quantityShares <= 0 ||
         !record.priceNanodollars.has_value() ||
         !record.amountMicrodollars.has_value() ||
         *record.amountMicrodollars < 0)) {
      return core::makeError(core::ErrorCode::schemaMismatch,
                             "Canonical fill payload is invalid");
    }
    if (record.family == results::RecordFamily::portfolio &&
        (record.side != results::OrderSide::none ||
         !record.cashMicrodollars.has_value() ||
         !record.marketValueMicrodollars.has_value() ||
         !record.equityMicrodollars.has_value() ||
         !record.pnlMicrodollars.has_value() ||
         !record.positionShares.has_value())) {
      return core::makeError(core::ErrorCode::schemaMismatch,
                             "Canonical portfolio payload is invalid");
    }
  }
  return {};
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
          record.pnlMicrodollars.has_value() &&
          record.positionShares.has_value()) {
        portfolios_.push_back({
            .timestamp = record.timestamp,
            .portfolio = {.cash = money(*record.cashMicrodollars),
                          .positionShares = *record.positionShares,
                          .marketValue = money(*record.marketValueMicrodollars),
                          .equity = money(*record.equityMicrodollars),
                          .pnl = money(*record.pnlMicrodollars)},
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
  for (std::size_t barIndex = 0; barIndex < bars.size(); ++barIndex) {
    const auto &bar = bars[barIndex];
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
        .barIndex = barIndex,
        .partialUtcDay = false,
    });
  }
  return frames;
}

struct DailyBarSets final {
  std::span<const data::SnapshotBar> selected;
  std::span<const data::SnapshotBar> fullSource;
};

core::Result<std::vector<ResultReplayFrame>>
makeDailyFrames(const results::OpenedResult &result,
                const DailyBarSets &barSets,
                const core::CancellationToken &cancellation) {
  using namespace std::chrono;
  const auto &bars = barSets.selected;
  const auto &fullSourceBars = barSets.fullSource;
  const ReplayRecordIndex records{result};
  std::vector<ResultReplayFrame> frames;
  std::size_t begin = 0;
  while (begin < bars.size()) {
    if (cancellation.isCancellationRequested()) {
      return core::makeError(core::ErrorCode::cancelled,
                             "Result Replay open was cancelled");
    }
    // Match Hourly's trust boundary before computing any Daily OHLCV.
    if (!records.portfolioAt(bars[begin].timestamp).has_value()) {
      ++begin;
      continue;
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
    const auto sourceBegin =
        std::ranges::lower_bound(fullSourceBars, core::Timestamp{day}, {},
                                 &data::SnapshotBar::timestamp);
    const auto sourceEnd =
        std::ranges::lower_bound(sourceBegin, fullSourceBars.end(), bucketEnd,
                                 {}, &data::SnapshotBar::timestamp);
    const auto completeSourceDay = std::ranges::equal(
        std::ranges::subrange{bars.begin() + static_cast<std::ptrdiff_t>(begin),
                              bars.begin() + static_cast<std::ptrdiff_t>(end)},
        std::ranges::subrange{sourceBegin, sourceEnd});
    const auto rangeCutsUtcDay = result.descriptor.range.start > day ||
                                 result.descriptor.range.end < bucketEnd;
    const auto portfolio = records.portfolioAt(bars[end - 1].timestamp);
    if (portfolio.has_value()) {
      auto fills = records.fillsAt(bars[begin].timestamp,
                                   bars[end - 1].timestamp + milliseconds{1});
      // Saved spans can omit an interior source row. Match Hourly's exact
      // timestamp boundary rather than admitting every fill between endpoints.
      const auto selectedDay = bars.subspan(begin, end - begin);
      std::erase_if(fills, [selectedDay](const ResultReplayFill &fill) {
        return !std::ranges::binary_search(selectedDay, fill.timestamp, {},
                                           &data::SnapshotBar::timestamp);
      });
      frames.push_back({
          .candle = {.ts = core::Timestamp{day},
                     .open = price(bars[begin].openNanodollars),
                     .high = price(high),
                     .low = price(low),
                     .close = price(bars[end - 1].closeNanodollars),
                     .volume =
                         static_cast<double>(volume) / microsharesPerShare},
          .fills = std::move(fills),
          .portfolio = *portfolio,
          .barIndex = end - 1,
          .partialUtcDay = !completeSourceDay || rangeCutsUtcDay,
      });
    }
    begin = end;
  }
  return frames;
}

class ReplayPortfolioPnlTracker final {
public:
  [[nodiscard]] static core::Result<ReplayPortfolioPnlTracker>
  create(const std::span<const data::SnapshotBar> selectedBars,
         const results::OpenedResult &result,
         const core::CancellationToken &cancellation) {
    std::vector<const results::CanonicalRecord *> fills;
    fills.reserve(result.records.size());
    for (const auto &record : result.records) {
      if (cancellation.isCancellationRequested()) {
        return core::makeError(core::ErrorCode::cancelled,
                               "Result Replay open was cancelled");
      }
      if (record.family == results::RecordFamily::fill &&
          record.quantityShares.has_value() &&
          record.amountMicrodollars.has_value()) {
        fills.push_back(&record);
      }
    }
    return ReplayPortfolioPnlTracker{selectedBars, std::move(fills)};
  }

  [[nodiscard]] core::Result<void>
  advanceThrough(const core::Timestamp timestamp,
                 const core::CancellationToken &cancellation) {
    while (nextFill_ < fills_.size() &&
           fills_[nextFill_]->timestamp <= timestamp) {
#if defined(BTE_ENABLE_TEST_HOOKS)
      requestPortfolioSnapshotCancellationAt(
          testing::PortfolioCompletionCancellationPoint::fillAdvance);
#endif
      if (cancellation.isCancellationRequested()) {
        return core::makeError(core::ErrorCode::cancelled,
                               "Result Replay open was cancelled");
      }
      const auto &fill = *fills_[nextFill_++];
      if (!selectedBarIndex(fill.timestamp).has_value()) {
        continue;
      }
      auto applied = applyFill(fill);
      if (!applied.ok()) {
        return applied.error();
      }
    }
    return {};
  }

  [[nodiscard]] std::optional<std::size_t>
  selectedBarIndex(const core::Timestamp timestamp) const {
    const auto sourceBar = std::ranges::lower_bound(
        selectedBars_, timestamp, {}, &data::SnapshotBar::timestamp);
    if (sourceBar == selectedBars_.end() || sourceBar->timestamp != timestamp) {
      return std::nullopt;
    }
    return static_cast<std::size_t>(
        std::distance(selectedBars_.begin(), sourceBar));
  }

  [[nodiscard]] double realizedPnl() const {
    return money(realizedPnlMicrodollars_);
  }

private:
  ReplayPortfolioPnlTracker(
      const std::span<const data::SnapshotBar> selectedBars,
      std::vector<const results::CanonicalRecord *> fills)
      : selectedBars_{selectedBars}, fills_{std::move(fills)} {}

  struct FillAmounts final {
    std::int64_t quantityShares;
    std::int64_t amountMicrodollars;
  };

  struct ProductDivision final {
    std::int64_t quotient;
    std::int64_t remainder;
  };

  struct ProductDivisionInput final {
    std::int64_t numeratorFactor;
    std::int64_t multiplier;
    std::int64_t denominator;
  };

  struct CostBasisAllocationInput final {
    std::int64_t openCostBasis;
    std::int64_t closedShares;
    std::int64_t openShares;
  };

  // Preconditions: numeratorFactor < denominator and multiplier <= denominator.
  // Scan multiplier bits while carrying quotient and remainder so the
  // potentially overflowing product never has to fit in int64.
  [[nodiscard]] static ProductDivision
  multiplyDivide(const ProductDivisionInput &input) {
    auto quotient = std::int64_t{0};
    auto remainder = std::int64_t{0};
    const auto unsignedMultiplier =
        static_cast<std::uint64_t>(input.multiplier);
    for (auto bit = std::bit_floor(unsignedMultiplier); bit != 0; bit >>= 1) {
      auto carry = std::int64_t{0};
      if (remainder >= input.denominator - remainder) {
        remainder -= input.denominator - remainder;
        ++carry;
      } else {
        remainder += remainder;
      }
      if ((unsignedMultiplier & bit) != 0) {
        if (remainder >= input.denominator - input.numeratorFactor) {
          remainder -= input.denominator - input.numeratorFactor;
          ++carry;
        } else {
          remainder += input.numeratorFactor;
        }
      }
      quotient = quotient * 2 + carry;
    }
    return {.quotient = quotient, .remainder = remainder};
  }

  [[nodiscard]] static std::int64_t
  proportionalCostBasis(const CostBasisAllocationInput &input) {
    const auto wholeShares = input.openCostBasis / input.openShares;
    const auto fractionalBasis = input.openCostBasis % input.openShares;
    const auto fractionalAllocation =
        multiplyDivide({.numeratorFactor = fractionalBasis,
                        .multiplier = input.closedShares,
                        .denominator = input.openShares});
    auto allocatedBasis =
        wholeShares * input.closedShares + fractionalAllocation.quotient;
    const auto distanceToRoundUp =
        input.openShares - fractionalAllocation.remainder;
    if (fractionalAllocation.remainder > distanceToRoundUp ||
        (fractionalAllocation.remainder == distanceToRoundUp &&
         allocatedBasis % 2 != 0)) {
      ++allocatedBasis;
    }
    return allocatedBasis;
  }

  [[nodiscard]] core::Result<void>
  applyFill(const results::CanonicalRecord &fill) {
    const auto amounts = FillAmounts{
        .quantityShares = fill.quantityShares.value_or(0),
        .amountMicrodollars = fill.amountMicrodollars.value_or(0),
    };
    // Open replay records have already passed validateReplayRecords. Preserve
    // a defensive check for invalid in-memory payloads. GCOVR_EXCL_START
    if (amounts.quantityShares <= 0 || amounts.amountMicrodollars < 0) {
      return core::makeError(core::ErrorCode::schemaMismatch,
                             "Replay fill payload is invalid");
    }
    // GCOVR_EXCL_STOP
    if (fill.side == results::OrderSide::buy) {
      return addLongPosition(amounts);
    }
    if (fill.side == results::OrderSide::sell) {
      return closeLongPosition(amounts);
    }
    // ResultStore accepts only buy or sell sides for validated fills. Keep a
    // defensive error for corrupted in-memory records. GCOVR_EXCL_START
    return core::makeError(core::ErrorCode::schemaMismatch,
                           "Replay fill side is unsupported");
    // GCOVR_EXCL_STOP
  }

  [[nodiscard]] core::Result<void> addLongPosition(const FillAmounts amounts) {
    if (amounts.quantityShares >
            std::numeric_limits<std::int64_t>::max() - positionShares_ ||
        amounts.amountMicrodollars > std::numeric_limits<std::int64_t>::max() -
                                         openCostBasisMicrodollars_) {
      return core::makeError(core::ErrorCode::schemaMismatch,
                             "Replay position exceeds supported range");
    }
    positionShares_ += amounts.quantityShares;
    openCostBasisMicrodollars_ += amounts.amountMicrodollars;
    return {};
  }

  [[nodiscard]] core::Result<void>
  closeLongPosition(const FillAmounts amounts) {
    if (positionShares_ <= 0 || amounts.quantityShares > positionShares_) {
      return core::makeError(core::ErrorCode::schemaMismatch,
                             "Replay sell fill exceeds the open position");
    }
    const auto closedShares = amounts.quantityShares;
    const auto closedCostBasis =
        closedShares == positionShares_
            ? openCostBasisMicrodollars_
            : proportionalCostBasis(
                  {.openCostBasis = openCostBasisMicrodollars_,
                   .closedShares = closedShares,
                   .openShares = positionShares_});
    const auto realizedDelta = amounts.amountMicrodollars - closedCostBasis;
    auto recorded = addRealizedPnl(realizedDelta);
    if (!recorded.ok()) {
      return recorded.error();
    }
    openCostBasisMicrodollars_ -= closedCostBasis;
    positionShares_ -= closedShares;
    return {};
  }

  [[nodiscard]] core::Result<void> addRealizedPnl(const std::int64_t delta) {
    if ((delta > 0 && realizedPnlMicrodollars_ >
                          std::numeric_limits<std::int64_t>::max() - delta) ||
        (delta < 0 && realizedPnlMicrodollars_ <
                          std::numeric_limits<std::int64_t>::min() - delta)) {
      return core::makeError(core::ErrorCode::schemaMismatch,
                             "Replay realized P&L exceeds supported range");
    }
    realizedPnlMicrodollars_ += delta;
    return {};
  }

  std::span<const data::SnapshotBar> selectedBars_;
  std::vector<const results::CanonicalRecord *> fills_;
  std::int64_t positionShares_ = 0;
  std::int64_t openCostBasisMicrodollars_ = 0;
  std::int64_t realizedPnlMicrodollars_ = 0;
  std::size_t nextFill_ = 0;
};

core::Result<void> completeReplayPortfolioSnapshots(
    std::vector<ResultReplayFrame> &frames,
    const std::span<const data::SnapshotBar> selectedBars,
    const results::OpenedResult &result,
    const core::CancellationToken &cancellation) {
#if defined(BTE_ENABLE_TEST_HOOKS)
  requestPortfolioSnapshotCancellationAt(
      testing::PortfolioCompletionCancellationPoint::trackerCreation);
#endif
  auto tracker =
      ReplayPortfolioPnlTracker::create(selectedBars, result, cancellation);
  if (!tracker.ok()) {
    return tracker.error();
  }
  auto pnlTracker = std::move(tracker).value();
  for (auto &frame : frames) {
#if defined(BTE_ENABLE_TEST_HOOKS)
    requestPortfolioSnapshotCancellationAt(
        testing::PortfolioCompletionCancellationPoint::frame);
#endif
    if (cancellation.isCancellationRequested()) {
      return core::makeError(core::ErrorCode::cancelled,
                             "Result Replay open was cancelled");
    }
    // Replay frames are built directly from the selected bar list. This guard
    // protects against an invalid internal frame index. GCOVR_EXCL_START
    if (frame.barIndex >= selectedBars.size()) {
      continue;
    }
    // GCOVR_EXCL_STOP
    auto advanced = pnlTracker.advanceThrough(
        selectedBars[frame.barIndex].timestamp, cancellation);
    if (!advanced.ok()) {
      return advanced.error();
    }
#if defined(BTE_ENABLE_TEST_HOOKS)
    if (!frame.fills.empty()) {
      requestPortfolioSnapshotCancellationAt(
          testing::PortfolioCompletionCancellationPoint::frameFill);
    }
#endif
    for (auto &fill : frame.fills) {
      if (cancellation.isCancellationRequested()) {
        return core::makeError(core::ErrorCode::cancelled,
                               "Result Replay open was cancelled");
      }
      if (const auto barIndex = pnlTracker.selectedBarIndex(fill.timestamp);
          barIndex.has_value()) {
        fill.barIndex = *barIndex;
      }
    }
    frame.portfolio.realizedPnl = pnlTracker.realizedPnl();
    frame.portfolio.unrealizedPnl =
        frame.portfolio.pnl - frame.portfolio.realizedPnl;
  }
  return {};
}

} // namespace

#if defined(BTE_ENABLE_TEST_HOOKS)
namespace testing {

void cancelNextResultReplayPortfolioCompletion(
    core::CancellationSource *source,
    const PortfolioCompletionCancellationPoint point) noexcept {
  nextPortfolioSnapshotCancellationSource().store(source);
  nextPortfolioSnapshotCancellationPoint().store(point);
}

void clearResultReplayTestHooks() noexcept {
  nextPortfolioSnapshotCancellationSource().store(nullptr);
  nextPortfolioSnapshotCancellationPoint().store(
      PortfolioCompletionCancellationPoint::trackerCreation);
}

} // namespace testing
#endif

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
  auto store = results::ResultStore::open(resultStore, dataStore, cancellation);
  if (!store.ok()) {
    return store.error();
  }
  auto opened = store.value()->openResultWithData(resultId, cancellation);
  if (!opened.ok()) {
    return opened.error();
  }
  auto resultAndData = std::move(opened).value();
  const auto &result = resultAndData.result;
  if (result.descriptor.universe.size() != 1) {
    return core::makeError(core::ErrorCode::schemaMismatch,
                           "Multi-symbol Result Replay is unsupported");
  }
  auto validRecords =
      validateReplayRecords(result.records, result.descriptor.universe.front());
  if (!validRecords.ok()) {
    return validRecords.error();
  }
  auto bars = std::move(resultAndData.selection.bars);
  std::vector<data::SnapshotBar> fullDailySourceBars;
  if (timeframe == ResultReplayTimeframe::dailyUtc && !bars.empty()) {
    using namespace std::chrono;
    const auto firstDay = floor<days>(bars.front().timestamp);
    const auto afterLastDay = floor<days>(bars.back().timestamp) + days{1};
    auto reader = data::ReleaseSnapshotReader::open(
        dataStore, result.descriptor.dataSelection.snapshotId, cancellation);
    if (!reader.ok()) {
      return reader.error();
    }
    auto fullSource = reader.value()->select(
        {.symbols = {result.descriptor.universe.front()},
         .range = {.start = core::Timestamp{firstDay},
                   .end = core::Timestamp{afterLastDay}},
         .timeframe = result.descriptor.dataSelection.timeframe},
        cancellation);
    // The exact saved rows above remain authoritative. This wider read only
    // supplies complete UTC-day source coverage for Daily aggregation.
    // GCOVR_EXCL_START
    if (!fullSource.ok()) {
      return fullSource.error();
    }
    // GCOVR_EXCL_STOP
    fullDailySourceBars = std::move(fullSource).value().bars;
  }
  if (result.status != results::RunStatus::completed) {
    const auto checkpoint = std::ranges::find_if(
        result.records.rbegin(), result.records.rend(),
        [](const results::CanonicalRecord &record) {
          return record.family == results::RecordFamily::portfolio;
        });
    if (checkpoint == result.records.rend()) {
      bars.clear();
    } else {
      const auto firstFuture = std::ranges::upper_bound(
          bars, checkpoint->timestamp, {}, &data::SnapshotBar::timestamp);
      bars.erase(firstFuture, bars.end());
    }
  }
  core::Result<std::vector<ResultReplayFrame>> frames =
      core::makeError(core::ErrorCode::schemaMismatch,
                      "Result Replay timeframe is unsupported");
  switch (timeframe) {
  case ResultReplayTimeframe::hourly:
    frames = makeHourlyFrames(result, bars, cancellation);
    break;
  case ResultReplayTimeframe::dailyUtc:
    frames = makeDailyFrames(
        result, {.selected = bars, .fullSource = fullDailySourceBars},
        cancellation);
    break;
  }
  if (!frames.ok()) {
    return frames.error();
  }
  auto replayFrames = std::move(frames).value();
  auto completed = completeReplayPortfolioSnapshots(replayFrames, bars, result,
                                                    cancellation);
  if (!completed.ok()) {
    return completed.error();
  }
  return std::make_unique<ResultReplay>(ConstructionKey{}, resultId,
                                        std::move(replayFrames),
                                        result.terminalReason);
}

core::Result<std::vector<results::ResultSummary>>
ResultReplay::list(const std::filesystem::path &resultStore,
                   const std::filesystem::path &dataStore,
                   const core::CancellationToken &cancellation) {
  if (cancellation.isCancellationRequested()) {
    return core::makeError(core::ErrorCode::cancelled,
                           "Result catalog load was cancelled");
  }
  auto store = results::ResultStore::open(resultStore, dataStore, cancellation);
  if (!store.ok()) {
    return store.error();
  }
  return store.value()->list(cancellation);
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

std::span<const ResultReplayFrame> ResultReplay::timeline() const noexcept {
  return frames_;
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
