#pragma once

#include "Bte/Core/Cancellation.h"

#include <cstdint>

namespace bte::bindings::testing {

enum class PortfolioCompletionCancellationPoint : std::uint8_t {
  trackerCreation,
  frame,
  fillAdvance,
  frameFill,
};

void cancelNextResultReplayPortfolioCompletion(
    core::CancellationSource *source,
    PortfolioCompletionCancellationPoint point) noexcept;
void clearResultReplayTestHooks() noexcept;

} // namespace bte::bindings::testing
