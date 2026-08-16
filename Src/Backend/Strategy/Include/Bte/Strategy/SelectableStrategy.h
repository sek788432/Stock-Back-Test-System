#pragma once

#include "Bte/Core/Bar.h"
#include "Bte/Core/Result.h"
#include "Bte/Indicators/StreamingIndicator.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace bte::strategy {

inline constexpr std::size_t maximumConditionsPerGroup = 2;

enum class ConditionLogic : std::uint8_t { all, any };

enum class ConditionSource : std::uint8_t {
  barField,
  closeChangePercent,
  indicator,
};

enum class Comparison : std::uint8_t {
  greaterThan,
  greaterThanOrEqual,
  lessThan,
  lessThanOrEqual,
  equal,
  notEqual,
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

/// Evaluates the version-one flat Selectable Conditions plan against actual
/// chronological bars. It has no access to orders, fills, or portfolio state.
class SelectableStrategy final {
public:
  [[nodiscard]] static core::Result<std::unique_ptr<SelectableStrategy>>
  create(SelectableStrategyPlan plan);

  ~SelectableStrategy();
  SelectableStrategy(SelectableStrategy &&) noexcept;
  SelectableStrategy &operator=(SelectableStrategy &&) noexcept;
  SelectableStrategy(const SelectableStrategy &) = delete;
  SelectableStrategy &operator=(const SelectableStrategy &) = delete;

  [[nodiscard]] core::Result<SelectableStrategySignal>
  onBar(const core::Bar &bar);

private:
  struct Impl;

public:
  explicit SelectableStrategy(std::unique_ptr<Impl> impl) noexcept;

private:
  std::unique_ptr<Impl> impl_;
};

} // namespace bte::strategy
