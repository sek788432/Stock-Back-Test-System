#include "Bte/Strategy/SelectableStrategy.h"

#include "Bte/Core/Digest.h"
#include "SelectableStrategyDetail.h"

// IWYU pragma: no_include <math>

#include <algorithm>
#include <bit>
#include <chrono>
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

void appendFrame(std::string &bytes, const std::string &value) {
  bytes += std::to_string(value.size());
  bytes.push_back(':');
  bytes += value;
}

template <typename Value>
void appendInteger(std::string &bytes, const Value value) {
  appendFrame(bytes, std::to_string(static_cast<std::uint64_t>(value)));
}

void appendCondition(std::string &bytes, const Condition &condition) {
  appendInteger(bytes, condition.source);
  appendInteger(bytes, condition.comparison);
  appendInteger(bytes, std::bit_cast<std::uint64_t>(condition.threshold));
  appendInteger(bytes, condition.thresholdDomain);
  appendInteger(bytes, condition.barField);
  appendInteger(bytes, condition.indicator.kind);
  appendInteger(bytes, condition.indicator.period);
  appendInteger(bytes, condition.indicator.secondaryPeriod);
  appendInteger(bytes, condition.indicator.signalPeriod);
  appendInteger(bytes, condition.indicator.output);
  appendInteger(bytes, condition.indicator.field);
}

void appendGroup(std::string &bytes, const ConditionGroup &group) {
  appendInteger(bytes, group.logic);
  appendInteger(bytes, group.conditions.size());
  for (const auto &condition : group.conditions) {
    appendCondition(bytes, condition);
  }
}

[[nodiscard]] bool isLogic(const ConditionLogic logic) noexcept {
  return logic == ConditionLogic::all || logic == ConditionLogic::any;
}

[[nodiscard]] bool isSource(const ConditionSource source) noexcept {
  return source == ConditionSource::barField ||
         source == ConditionSource::closeChangePercent ||
         source == ConditionSource::indicator ||
         source == ConditionSource::previousDataDayCloseChangePercent;
}

[[nodiscard]] bool
planHasPreviousDataDayCondition(const SelectableStrategyPlan &plan) {
  const auto groupHasCondition = [](const ConditionGroup &group) {
    return std::ranges::any_of(group.conditions, [](const Condition &item) {
      return item.source == ConditionSource::previousDataDayCloseChangePercent;
    });
  };
  return groupHasCondition(plan.buy) || groupHasCondition(plan.sell);
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
  if (comparison == Comparison::greaterThan) {
    return value.value > condition.threshold;
  }
  if (comparison == Comparison::greaterThanOrEqual) {
    return value.value >= condition.threshold;
  }
  if (comparison == Comparison::lessThan) {
    return value.value < condition.threshold;
  }
  if (comparison == Comparison::lessThanOrEqual) {
    return value.value <= condition.threshold;
  }
  if (comparison == Comparison::equal) {
    return value.value == condition.threshold;
  }
  return value.value != condition.threshold;
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
  if (condition.source == ConditionSource::previousDataDayCloseChangePercent) {
    const bool validDirection =
        (condition.comparison == Comparison::lessThanOrEqual &&
         condition.threshold < 0.0) ||
        (condition.comparison == Comparison::greaterThanOrEqual &&
         condition.threshold > 0.0);
    if (condition.thresholdDomain != indicators::NumericDomain::percent ||
        !validDirection) {
      return compileError(
          "previous data-day close conditions require a signed inclusive "
          "percentage comparison with a nonzero magnitude");
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

std::string
canonicalStrategyHash(const std::optional<SelectableStrategyPlan> &plan) {
  const bool usesVersionTwo =
      plan.has_value() && planHasPreviousDataDayCondition(*plan);
  std::string bytes{usesVersionTwo ? "bte-strategy-v2" : "bte-strategy-v1"};
  if (!plan.has_value()) {
    appendFrame(bytes, "starter-market-buy-v1");
    return core::sha256(bytes);
  }
  appendFrame(bytes, usesVersionTwo ? "selectable-conditions-v2"
                                    : "selectable-conditions-v1");
  appendGroup(bytes, plan->buy);
  appendGroup(bytes, plan->sell);
  return core::sha256(bytes);
}

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
                const detail::IndicatorFactory configuredIndicatorFactory,
                std::optional<PreviousDataDayClose> configuredPreviousClose)
      : plan(std::move(configuredPlan)),
        indicatorFactory(configuredIndicatorFactory),
        usesPreviousDataDayClose(planHasPreviousDataDayCondition(plan)),
        seedPreviousDataDayClose(
            configuredPreviousClose.has_value() &&
                    std::isfinite(configuredPreviousClose->close) &&
                    configuredPreviousClose->close > 0.0
                ? configuredPreviousClose
                : std::nullopt) {}

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
    if (usesPreviousDataDayClose) {
      beginDataDayBar(bar);
    }
    const auto signal = SelectableStrategySignal{
        .buy = evaluate(buy, bar),
        .sell = evaluate(sell, bar),
    };
    previousClose = bar.close;
    if (usesPreviousDataDayClose) {
      finishDataDayBar(bar);
    }
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
    if (condition.condition.source == ConditionSource::barField) {
      return indicators::barFieldValue(bar, condition.condition.barField);
    }
    if (condition.condition.source == ConditionSource::closeChangePercent) {
      if (!previousClose.has_value() || *previousClose == 0.0) {
        return std::nullopt;
      }
      return indicators::IndicatorValue{
          .value = 100.0 * (bar.close - *previousClose) / *previousClose,
          .domain = indicators::NumericDomain::percent,
      };
    }
    if (condition.condition.source ==
        ConditionSource::previousDataDayCloseChangePercent) {
      if (!previousDataDayCloseForCurrentBar.has_value()) {
        return std::nullopt;
      }
      const auto change =
          (bar.close / *previousDataDayCloseForCurrentBar - 1.0) * 100.0;
      if (!std::isfinite(change)) {
        return std::nullopt;
      }
      return indicators::IndicatorValue{
          .value = change,
          .domain = indicators::NumericDomain::percent,
      };
    }
    return condition.indicator->latest();
  }

  void beginDataDayBar(const core::Bar &bar) {
    using namespace std::chrono;
    currentBarCanUpdateDataDayClose = true;
    const auto dataDay = floor<days>(bar.ts);
    if (!currentDataDay.has_value()) {
      currentDataDay = dataDay;
      if (seedPreviousDataDayClose.has_value() &&
          floor<days>(seedPreviousDataDayClose->timestamp) < dataDay) {
        previousDataDayClose = seedPreviousDataDayClose->close;
      }
    } else if (dataDay > *currentDataDay) {
      previousDataDayClose = currentDataDayLastClose;
      currentDataDay = dataDay;
      currentDataDayLastClose.reset();
    } else if (dataDay < *currentDataDay) {
      previousDataDayCloseForCurrentBar.reset();
      currentBarCanUpdateDataDayClose = false;
      return;
    }
    previousDataDayCloseForCurrentBar = previousDataDayClose;
  }

  void finishDataDayBar(const core::Bar &bar) {
    if (!currentDataDay.has_value() || !currentBarCanUpdateDataDayClose) {
      return;
    }
    currentDataDayLastClose = bar.close;
  }

  SelectableStrategyPlan plan;
  detail::IndicatorFactory indicatorFactory;
  RuntimeGroup buy;
  RuntimeGroup sell;
  std::optional<double> previousClose;
  bool usesPreviousDataDayClose = false;
  std::optional<PreviousDataDayClose> seedPreviousDataDayClose;
  std::optional<std::chrono::sys_days> currentDataDay;
  std::optional<double> previousDataDayClose;
  std::optional<double> currentDataDayLastClose;
  std::optional<double> previousDataDayCloseForCurrentBar;
  bool currentBarCanUpdateDataDayClose = true;
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
    SelectableStrategyPlan plan, const IndicatorFactory indicatorFactory,
    std::optional<PreviousDataDayClose> previousDataDayClose) {
  try {
    auto implementation = std::make_unique<SelectableStrategy::Impl>(
        std::move(plan), indicatorFactory, previousDataDayClose);
    const auto initialized = implementation->initialize();
    if (!initialized.ok()) {
      return initialized.error();
    }
    return std::make_unique<SelectableStrategy>(std::move(implementation));
  } catch (const std::exception &error) {
    return core::makeError(core::ErrorCode::internal, error.what());
  } catch (...) {
    return core::makeError(core::ErrorCode::internal,
                           "indicator factory failed");
  }
}

core::Result<std::unique_ptr<SelectableStrategy>> SelectableStrategy::create(
    SelectableStrategyPlan plan,
    std::optional<PreviousDataDayClose> previousDataDayClose) {
  auto strategy = createWithIndicatorFactory(
      std::move(plan), &indicators::StreamingIndicator::create,
      previousDataDayClose);
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
