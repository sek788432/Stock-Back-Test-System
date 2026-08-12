#pragma once

#include "Bte/Core/Bar.h"
#include "Bte/Core/Cancellation.h"
#include "Bte/Core/Result.h"

#include <QDate>
#include <QString>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace bte::bindings {

enum class BacktestOutcome {
  filled,
  rejectedInsufficientCash,
  cancelledNoFutureMarketData,
};

struct BacktestFillSnapshot {
  core::Timestamp timestamp{};
  std::int64_t quantityShares = 0;
  double price = 0.0;
  double amount = 0.0;
};

struct BacktestSnapshot {
  BacktestOutcome outcome = BacktestOutcome::cancelledNoFutureMarketData;
  std::optional<BacktestFillSnapshot> fill;
  double initialCapital = 0.0;
  double cash = 0.0;
  double marketValue = 0.0;
  double equity = 0.0;
  double pnl = 0.0;
  double finalPrice = 0.0;
  std::int64_t positionShares = 0;
  std::size_t barsProcessed = 0;
};

struct BacktestConfiguration {
  QString symbol;
  QString schema;
  QDate startDate;
  QDate endDate;
  double initialCapital = 0.0;
  std::int64_t quantityShares = 0;
};

[[nodiscard]] core::Result<BacktestSnapshot>
runBacktestSession(std::vector<core::Bar> bars, double initialCapital,
                   std::int64_t quantityShares,
                   core::CancellationToken cancellation = {});

[[nodiscard]] core::Result<BacktestSnapshot>
runBacktestConfiguration(BacktestConfiguration configuration,
                         core::CancellationToken cancellation = {});

} // namespace bte::bindings
