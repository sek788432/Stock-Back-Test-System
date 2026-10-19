#pragma once

#include <cstdint>

namespace bte::bindings::testing {

void blockNextResultOpenUntilCancelled() noexcept;
[[nodiscard]] bool resultOpenWorkerStarted() noexcept;
[[nodiscard]] std::uint64_t resultCatalogRequestCount() noexcept;
void clearResultReplayRequestHooks() noexcept;

} // namespace bte::bindings::testing
