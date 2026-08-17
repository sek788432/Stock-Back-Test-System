#pragma once

#include "Bte/Core/Result.h"
#include "Bte/Indicators/StreamingIndicator.h"
#include "Bte/Strategy/SelectableStrategy.h"

#include <memory>

namespace bte::strategy::detail {

using IndicatorFactory = core::Result<indicators::StreamingIndicator> (*)(
    const indicators::IndicatorDefinition &definition);

struct SelectableStrategyTestAccess final {
  [[nodiscard]] static core::Result<std::unique_ptr<SelectableStrategy>>
  create(SelectableStrategyPlan plan, IndicatorFactory indicatorFactory);
};

} // namespace bte::strategy::detail
