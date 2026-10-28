#pragma once

#include "Bte/Core/Bar.h"
#include "Bte/Core/Cancellation.h"
#include "Bte/Core/Result.h"
#include "Bte/Indicators/StreamingIndicator.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bte::strategy {

inline constexpr std::int64_t maximumStrategyQuantityShares = 1'000'000'000;
inline constexpr std::size_t maximumCommandsPerMarketCallback = 1;

enum class StrategyCommandKind : std::uint8_t { buyMarket, closePosition };

struct StrategyCommand final {
  StrategyCommandKind kind = StrategyCommandKind::buyMarket;
  std::string symbol;
  /// Used by `buyMarket`; `closePosition` must carry zero.
  std::int64_t quantityShares = 0;
};

using StrategyCommandBatch = std::vector<StrategyCommand>;

enum class StrategyFillSide : std::uint8_t { buy, sell };

struct StrategyFill final {
  core::Timestamp timestamp;
  StrategyFillSide side = StrategyFillSide::buy;
  std::int64_t quantityShares = 0;
  std::int64_t priceNanodollars = 0;
  std::int64_t amountMicrodollars = 0;
};

struct StrategyIndicatorRegistration final {
  std::string name;
  indicators::IndicatorDefinition definition;
};

struct StrategyRequirements final {
  std::size_t maxHistoryBars = 0;
  std::vector<StrategyIndicatorRegistration> indicators{};
};

struct StrategyIndicatorValue final {
  std::string_view name;
  std::optional<indicators::IndicatorValue> value;
};

struct StrategyContext final {
  std::string_view symbol{};
  std::optional<core::Bar> bar = std::nullopt;
  std::int64_t cashMicrodollars = 0;
  std::int64_t equityMicrodollars = 0;
  std::int64_t positionShares = 0;
  std::optional<std::int64_t> markPriceNanodollars = std::nullopt;
  /// Pre-range actual bars are available to the adapter only for initial seed.
  std::span<const core::Bar> warmupHistory{};
  std::span<const StrategyIndicatorValue> indicatorValues{};
};

enum class StrategyDiagnosticKind : std::uint8_t { log, terminal };

/// A bounded, structured diagnostic emitted by a Strategy callback. Fields
/// remain canonical JSON text so the Strategy module does not own a JSON ABI.
struct StrategyDiagnostic final {
  StrategyDiagnosticKind kind = StrategyDiagnosticKind::log;
  std::string hook{};
  std::string level{};
  std::string errorCode{};
  std::string message{};
  std::string canonicalFieldsJson = "{}";
  std::optional<std::uint32_t> sourceLine = std::nullopt;
  std::optional<std::uint32_t> sourceColumn = std::nullopt;
  std::string traceback{};
  std::uint64_t omittedBytes = 0;
  std::uint64_t omittedEntries = 0;
};

struct StrategyDiagnosticBatch final {
  std::vector<StrategyDiagnostic> records;
  std::uint64_t omittedLogBytes = 0;
  std::uint64_t omittedLogEntries = 0;
  bool complete = true;
};

/// Receives immutable Engine observations and returns a complete transactional
/// command batch. The Engine remains the sole order and accounting authority.
class Strategy {
public:
  Strategy() = default;
  Strategy(const Strategy &) = delete;
  Strategy &operator=(const Strategy &) = delete;
  Strategy(Strategy &&) = delete;
  Strategy &operator=(Strategy &&) = delete;
  virtual ~Strategy() = default;

  [[nodiscard]] virtual const StrategyRequirements &
  requirements() const noexcept {
    static const StrategyRequirements none{};
    return none;
  }

  [[nodiscard]] virtual core::Result<void>
  onStart(const StrategyContext &,
          const core::CancellationToken &cancellation = {});
  [[nodiscard]] virtual core::Result<StrategyCommandBatch>
  onMarket(const StrategyContext &context,
           const core::CancellationToken &cancellation = {}) = 0;
  [[nodiscard]] virtual core::Result<void>
  onFill(const StrategyFill &, const StrategyContext &,
         const core::CancellationToken &cancellation = {});
  [[nodiscard]] virtual core::Result<void>
  onFinish(const StrategyContext &,
           const core::CancellationToken &cancellation = {});
  virtual void onCancel() noexcept;
  /// Transfers newly captured diagnostics to the Engine. Repeated calls do
  /// not return the same records.
  [[nodiscard]] virtual StrategyDiagnosticBatch takeDiagnostics();
};

inline core::Result<void> Strategy::onStart(const StrategyContext &,
                                            const core::CancellationToken &) {
  return {};
}

inline core::Result<void> Strategy::onFill(const StrategyFill &,
                                           const StrategyContext &,
                                           const core::CancellationToken &) {
  return {};
}

inline core::Result<void> Strategy::onFinish(const StrategyContext &,
                                             const core::CancellationToken &) {
  return {};
}

inline void Strategy::onCancel() noexcept {}

inline StrategyDiagnosticBatch Strategy::takeDiagnostics() { return {}; }

} // namespace bte::strategy
