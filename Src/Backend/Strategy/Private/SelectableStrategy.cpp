#include "Bte/Strategy/SelectableStrategy.h"

#include <algorithm>
#include <cmath>
#include <exception>
#include <math.h>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace bte::strategy {
namespace {

[[nodiscard]] bool isLogic(const ConditionLogic logic) noexcept {
  return logic == ConditionLogic::all || logic == ConditionLogic::any;
}

[[nodiscard]] bool isSource(const ConditionSource source) noexcept {
  return source >= ConditionSource::barField &&
         source <= ConditionSource::indicator;
}

[[nodiscard]] bool isComparison(const Comparison comparison) noexcept {
  return comparison >= Comparison::greaterThan &&
         comparison <= Comparison::notEqual;
}

[[nodiscard]] bool
isNumericDomain(const indicators::NumericDomain domain) noexcept {
  return domain >= indicators::NumericDomain::price &&
         domain <= indicators::NumericDomain::scalar;
}

[[nodiscard]] bool matches(const indicators::IndicatorValue &value,
                           const Comparison comparison,
                           const Condition &condition) noexcept {
  if (value.domain != condition.thresholdDomain) {
    return false;
  }
  switch (comparison) {
  case Comparison::greaterThan:
    return value.value > condition.threshold;
  case Comparison::greaterThanOrEqual:
    return value.value >= condition.threshold;
  case Comparison::lessThan:
    return value.value < condition.threshold;
  case Comparison::lessThanOrEqual:
    return value.value <= condition.threshold;
  case Comparison::equal:
    return value.value == condition.threshold;
  case Comparison::notEqual:
    return value.value != condition.threshold;
  }
  return false;
}

[[nodiscard]] core::Error compileError(std::string message) {
  return core::makeError(core::ErrorCode::strategyCompileFailed,
                         std::move(message));
}

[[nodiscard]] core::Result<Condition>
validateCondition(const Condition &condition) {
  if (!isSource(condition.source) || !isComparison(condition.comparison) ||
      !indicators::isValidBarField(condition.barField) ||
      !isNumericDomain(condition.thresholdDomain) ||
      !std::isfinite(condition.threshold)) {
    return compileError(
        "condition has an invalid source, comparison, field, threshold, or "
        "threshold domain");
  }
  if (condition.source == ConditionSource::barField) {
    const auto value =
        indicators::barFieldValue(core::Bar{}, condition.barField);
    if (value.domain != condition.thresholdDomain) {
      return compileError(
          "condition threshold domain does not match bar field");
    }
    return condition;
  }
  if (condition.source == ConditionSource::closeChangePercent) {
    if (condition.thresholdDomain != indicators::NumericDomain::percent) {
      return compileError(
          "condition threshold domain does not match close-change percentage");
    }
    return condition;
  }
  const auto indicator =
      indicators::StreamingIndicator::create(condition.indicator);
  if (!indicator.ok()) {
    return compileError("condition has an invalid indicator: " +
                        indicator.error().message);
  }
  if (indicators::indicatorOutputDomain(condition.indicator) !=
      condition.thresholdDomain) {
    return compileError("condition threshold domain does not match indicator");
  }
  return condition;
}

[[nodiscard]] core::Result<ConditionGroup>
validateGroup(const ConditionGroup &group, const bool required) {
  if (!isLogic(group.logic)) {
    return compileError("condition group has an invalid logic value");
  }
  if (required && group.conditions.empty()) {
    return compileError(
        "a selectable strategy requires at least one buy condition");
  }
  if (group.conditions.size() > maximumConditionsPerGroup) {
    return compileError("a selectable condition group supports at most two "
                        "conditions");
  }
  for (const auto &condition : group.conditions) {
    const auto validated = validateCondition(condition);
    if (!validated.ok()) {
      return validated.error();
    }
  }
  return group;
}

} // namespace

struct SelectableStrategy::Impl final {
  struct RuntimeCondition final {
    Condition condition;
    std::unique_ptr<indicators::StreamingIndicator> indicator;
  };

  struct RuntimeGroup final {
    ConditionLogic logic = ConditionLogic::all;
    std::vector<RuntimeCondition> conditions;
  };

  explicit Impl(SelectableStrategyPlan configuredPlan)
      : plan(std::move(configuredPlan)) {}

  [[nodiscard]] core::Result<bool> initialize() {
    auto buyGroup = makeGroup(plan.buy);
    if (!buyGroup.ok()) {
      return buyGroup.error();
    }
    auto sellGroup = makeGroup(plan.sell);
    if (!sellGroup.ok()) {
      return sellGroup.error();
    }
    buy = std::move(buyGroup).value();
    sell = std::move(sellGroup).value();
    return true;
  }

  [[nodiscard]] core::Result<SelectableStrategySignal>
  onBar(const core::Bar &bar) {
    if (!bar.isValid() || !std::isfinite(bar.volume)) {
      return core::makeError(core::ErrorCode::invalidArgument,
                             "strategy input must satisfy OHLCV invariants");
    }
    const auto buyUpdate = updateIndicators(buy, bar);
    if (!buyUpdate.ok()) {
      return buyUpdate.error();
    }
    const auto sellUpdate = updateIndicators(sell, bar);
    if (!sellUpdate.ok()) {
      return sellUpdate.error();
    }
    const auto signal = SelectableStrategySignal{
        .buy = evaluate(buy, bar),
        .sell = evaluate(sell, bar),
    };
    previousClose = bar.close;
    return signal;
  }

  [[nodiscard]] static core::Result<RuntimeGroup>
  makeGroup(const ConditionGroup &group) {
    auto runtime = RuntimeGroup{.logic = group.logic, .conditions = {}};
    runtime.conditions.reserve(group.conditions.size());
    for (const auto &condition : group.conditions) {
      auto runtimeCondition = RuntimeCondition{
          .condition = condition,
          .indicator = {},
      };
      if (condition.source == ConditionSource::indicator) {
        auto indicator =
            indicators::StreamingIndicator::create(condition.indicator);
        if (!indicator.ok()) {
          return compileError("could not construct condition indicator: " +
                              indicator.error().message);
        }
        runtimeCondition.indicator =
            std::make_unique<indicators::StreamingIndicator>(
                std::move(indicator).value());
      }
      runtime.conditions.push_back(std::move(runtimeCondition));
    }
    return runtime;
  }

  [[nodiscard]] static core::Result<bool>
  updateIndicators(RuntimeGroup &group, const core::Bar &bar) {
    for (auto &condition : group.conditions) {
      if (condition.indicator == nullptr) {
        continue;
      }
      const auto updated = condition.indicator->update(bar);
      if (!updated.ok()) {
        return updated.error();
      }
    }
    return true;
  }

  [[nodiscard]] bool evaluate(const RuntimeGroup &group,
                              const core::Bar &bar) const noexcept {
    if (group.conditions.empty()) {
      return false;
    }
    const auto evaluateCondition = [this,
                                    &bar](const RuntimeCondition &condition) {
      const auto value = valueFor(condition, bar);
      return value.has_value() &&
             matches(*value, condition.condition.comparison,
                     condition.condition);
    };
    if (group.logic == ConditionLogic::all) {
      return std::ranges::all_of(group.conditions, evaluateCondition);
    }
    return std::ranges::any_of(group.conditions, evaluateCondition);
  }

  [[nodiscard]] std::optional<indicators::IndicatorValue>
  valueFor(const RuntimeCondition &condition,
           const core::Bar &bar) const noexcept {
    switch (condition.condition.source) {
    case ConditionSource::barField:
      return indicators::barFieldValue(bar, condition.condition.barField);
    case ConditionSource::closeChangePercent:
      if (!previousClose.has_value() || *previousClose == 0.0) {
        return std::nullopt;
      }
      return indicators::IndicatorValue{
          .value = 100.0 * (bar.close - *previousClose) / *previousClose,
          .domain = indicators::NumericDomain::percent,
      };
    case ConditionSource::indicator:
      return condition.indicator->latest();
    }
    return std::nullopt;
  }

  SelectableStrategyPlan plan;
  RuntimeGroup buy;
  RuntimeGroup sell;
  std::optional<double> previousClose;
};

SelectableStrategy::SelectableStrategy(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}

SelectableStrategy::~SelectableStrategy() = default;
SelectableStrategy::SelectableStrategy(SelectableStrategy &&) noexcept =
    default;
SelectableStrategy &
SelectableStrategy::operator=(SelectableStrategy &&) noexcept = default;

core::Result<std::unique_ptr<SelectableStrategy>>
SelectableStrategy::create(SelectableStrategyPlan plan) {
  const auto buyValidation = validateGroup(plan.buy, true);
  if (!buyValidation.ok()) {
    return buyValidation.error();
  }
  const auto sellValidation = validateGroup(plan.sell, false);
  if (!sellValidation.ok()) {
    return sellValidation.error();
  }
  try {
    auto implementation = std::make_unique<Impl>(std::move(plan));
    const auto initialized = implementation->initialize();
    if (!initialized.ok()) {
      return initialized.error();
    }
    return std::make_unique<SelectableStrategy>(std::move(implementation));
  } catch (const std::exception &error) {
    return core::makeError(core::ErrorCode::internal,
                           std::string{"could not allocate strategy state: "} +
                               error.what());
  }
}

core::Result<SelectableStrategySignal>
SelectableStrategy::onBar(const core::Bar &bar) {
  return impl_->onBar(bar);
}

} // namespace bte::strategy
