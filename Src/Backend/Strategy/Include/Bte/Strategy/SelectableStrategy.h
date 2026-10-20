#pragma once

#include "Bte/Core/Bar.h"
#include "Bte/Core/Result.h"
#include "Bte/Indicators/StreamingIndicator.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace bte::strategy {

namespace detail {
struct SelectableStrategyTestAccess;
}

inline constexpr std::size_t maximumConditionsPerGroup = 2;

enum class ConditionLogic : std::uint8_t { all, any };

enum class ConditionSource : std::uint8_t {
  barField,
  closeChangePercent,
  indicator,
  previousDataDayCloseChangePercent,
};

enum class Comparison : std::uint8_t {
  greaterThan,
  greaterThanOrEqual,
  lessThan,
  lessThanOrEqual,
  equal,
  notEqual,
};

struct PreviousDataDayClose {
  /// Timestamp of the source row that supplied this close.
  core::Timestamp timestamp;
  /// Close in dollars from the immutable data snapshot.
  double close = 0.0;
};

struct Condition {
  ConditionSource source = ConditionSource::barField;
  Comparison comparison = Comparison::greaterThan;
  double threshold = 0.0;
  indicators::NumericDomain thresholdDomain = indicators::NumericDomain::price;
  indicators::BarField barField = indicators::BarField::close;
  indicators::IndicatorDefinition indicator{};
};

struct ConditionGroup {
  ConditionLogic logic = ConditionLogic::all;
  std::vector<Condition> conditions;
};

struct SelectableStrategyPlan {
  ConditionGroup buy;
  ConditionGroup sell;
};

struct SelectableStrategySignal {
  bool buy = false;
  bool sell = false;
};

/// Hashes the complete ordered Strategy configuration with versioned framing.
/// A missing plan identifies the built-in starter Strategy.
[[nodiscard]] std::string
canonicalStrategyHash(const std::optional<SelectableStrategyPlan> &plan);

/// Evaluates a flat Selectable Conditions plan against actual chronological
/// bars. It has no access to orders, fills, or portfolio state.
class SelectableStrategy final {
public:
  /// Seeds the optional prior-data-day predicate from the same immutable
  /// snapshot as the actual bars. Seeds on or after the first UTC data day are
  /// ignored; invalid closes are treated as absent history.
  [[nodiscard]] static core::Result<std::unique_ptr<SelectableStrategy>> create(
      SelectableStrategyPlan plan,
      std::optional<PreviousDataDayClose> previousDataDayClose = std::nullopt);

  ~SelectableStrategy();
  SelectableStrategy(SelectableStrategy &&) noexcept;
  SelectableStrategy &operator=(SelectableStrategy &&) noexcept;
  SelectableStrategy(const SelectableStrategy &) = delete;
  SelectableStrategy &operator=(const SelectableStrategy &) = delete;

  [[nodiscard]] core::Result<SelectableStrategySignal>
  onBar(const core::Bar &bar);

private:
  struct Impl;
  using IndicatorFactory = core::Result<indicators::StreamingIndicator> (*)(
      const indicators::IndicatorDefinition &definition);

  [[nodiscard]] static core::Result<std::unique_ptr<SelectableStrategy>>
  createWithIndicatorFactory(
      SelectableStrategyPlan plan, IndicatorFactory indicatorFactory,
      std::optional<PreviousDataDayClose> previousDataDayClose = std::nullopt);

  friend struct detail::SelectableStrategyTestAccess;

public:
  explicit SelectableStrategy(std::unique_ptr<Impl> impl) noexcept;

private:
  std::unique_ptr<Impl> impl_;
};

} // namespace bte::strategy
