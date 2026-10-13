#pragma once

#include "Bte/Core/Cancellation.h"

namespace bte::bindings::testing {

void cancelAfterNextDraftStoreOpen(core::CancellationSource &source) noexcept;
void clearDraftStoreOpenHook() noexcept;

} // namespace bte::bindings::testing
