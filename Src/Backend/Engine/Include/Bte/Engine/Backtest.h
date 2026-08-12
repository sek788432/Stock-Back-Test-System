#pragma once

#include "Bte/Core/Bar.h"
#include "Bte/Core/Cancellation.h"
#include "Bte/Core/Result.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace bte::engine {

inline constexpr std::int64_t maximumStarterQuantityShares = 1'000'000'000;

enum class StarterOrderStatus {
  filled,
  rejectedInsufficientCash,
  cancelledNoFutureMarketData,
};

struct BacktestRequest {
  std::vector<core::Bar> bars;
  std::int64_t initialCapitalMicrodollars = 0;
  std::int64_t quantityShares = 0;
};

struct BacktestFill {
  core::Timestamp timestamp{};
  std::int64_t quantityShares = 0;
  std::int64_t priceNanodollars = 0;
  std::int64_t amountMicrodollars = 0;
};

struct BacktestResult {
  StarterOrderStatus orderStatus =
      StarterOrderStatus::cancelledNoFutureMarketData;
  std::optional<BacktestFill> fill;
  std::int64_t initialCapitalMicrodollars = 0;
  std::int64_t cashMicrodollars = 0;
  std::int64_t marketValueMicrodollars = 0;
  std::int64_t equityMicrodollars = 0;
  std::int64_t pnlMicrodollars = 0;
  std::int64_t finalPriceNanodollars = 0;
  std::size_t barsProcessed = 0;
};

/// Runs the first vertical engine slice: submit one market buy on the first
/// bar, make it eligible on the next actual bar, and retain the final open
/// position.
[[nodiscard]] core::Result<BacktestResult>
runBacktest(const BacktestRequest &request,
            core::CancellationToken cancellation = {});

} // namespace bte::engine
