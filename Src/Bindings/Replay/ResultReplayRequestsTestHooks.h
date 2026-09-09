#pragma once

namespace bte::bindings::testing {

void blockNextResultOpenUntilCancelled() noexcept;
[[nodiscard]] bool resultOpenWorkerStarted() noexcept;
void clearResultReplayRequestHooks() noexcept;

} // namespace bte::bindings::testing
