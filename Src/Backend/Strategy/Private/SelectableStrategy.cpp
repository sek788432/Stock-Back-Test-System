#include "Bte/Strategy/SelectableStrategy.h"

#include "SelectableStrategyDetail.h"

// IWYU pragma: no_include <math>

#include <algorithm>
#include <cmath>
#include <exception>
#include <functional> // IWYU pragma: keep
#include <memory>
#include <optional>
#include <string>
#include <string_view>
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

[[nodiscard]] core::Error compileError(
    std::string message,
    const core::ErrorCode code = core::ErrorCode::strategyCompileFailed,
    const core::Error *const cause = nullptr) {
  auto error = core::makeError(code, std::move(message));
  if (cause != nullptr) {
    error.causes.push_back(*cause);
  }
  return error;
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

  explicit Impl(SelectableStrategyPlan configuredPlan,
                const detail::IndicatorFactory configuredIndicatorFactory)
      : plan(std::move(configuredPlan)),
        indicatorFactory(configuredIndicatorFactory) {}

  [[nodiscard]] core::Result<bool> initialize() {
    auto buyGroup = makeGroup(plan.buy, true, "buy condition group");
    if (!buyGroup.ok()) {
      return buyGroup.error();
    }
    auto sellGroup = makeGroup(plan.sell, false, "sell condition group");
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

  [[nodiscard]] core::Result<RuntimeGroup>
  makeGroup(const ConditionGroup &group, const bool required,
            const std::string_view context) const {
    const auto validation = validateGroup(group, required);
    if (!validation.ok()) {
      return compileError(std::string{context} + ": " +
                              validation.error().message,
                          validation.error().code, &validation.error());
    }
    auto runtime = RuntimeGroup{.logic = group.logic, .conditions = {}};
    runtime.conditions.reserve(group.conditions.size());
    for (const auto &condition : group.conditions) {
      auto runtimeCondition = RuntimeCondition{
          .condition = condition,
          .indicator = {},
      };
      if (condition.source == ConditionSource::indicator) {
        auto indicator = indicatorFactory(condition.indicator);
        if (!indicator.ok()) {
          const auto constructionError =
              indicator.error().code == core::ErrorCode::invalidArgument
                  ? compileError("condition has an invalid indicator: " +
                                     indicator.error().message,
                                 core::ErrorCode::strategyCompileFailed,
                                 &indicator.error())
                  : compileError("could not construct condition indicator: " +
                                     indicator.error().message,
                                 indicator.error().code, &indicator.error());
          return compileError(std::string{context} + ": " +
                                  constructionError.message,
                              constructionError.code, &constructionError);
        }
        if (indicators::indicatorOutputDomain(condition.indicator) !=
            condition.thresholdDomain) {
          const auto domainError = compileError(
              "condition threshold domain does not match indicator");
          return compileError(std::string{context} + ": " + domainError.message,
                              domainError.code, &domainError);
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
  detail::IndicatorFactory indicatorFactory;
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
SelectableStrategy::createWithIndicatorFactory(
    SelectableStrategyPlan plan, const IndicatorFactory indicatorFactory) {
  try {
    auto implementation = std::make_unique<SelectableStrategy::Impl>(
        std::move(plan), indicatorFactory);
    const auto initialized = implementation->initialize();
    if (!initialized.ok()) {
      return initialized.error();
    }
    return std::make_unique<SelectableStrategy>(std::move(implementation));
  } catch (const std::exception &error) {
    return core::makeError(core::ErrorCode::internal, error.what());
  }
}

core::Result<std::unique_ptr<SelectableStrategy>>
SelectableStrategy::create(SelectableStrategyPlan plan) {
  auto strategy = createWithIndicatorFactory(
      std::move(plan), &indicators::StreamingIndicator::create);
  return strategy;
}

core::Result<std::unique_ptr<SelectableStrategy>>
detail::SelectableStrategyTestAccess::create(
    SelectableStrategyPlan plan, const IndicatorFactory indicatorFactory) {
  auto strategy = SelectableStrategy::createWithIndicatorFactory(
      std::move(plan), indicatorFactory);
  return strategy;
}

core::Result<SelectableStrategySignal>
SelectableStrategy::onBar(const core::Bar &bar) {
  return impl_->onBar(bar);
}

} // namespace bte::strategy
