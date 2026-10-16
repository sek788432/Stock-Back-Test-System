#pragma once

namespace bte::bindings::testing {

void blockNextLibraryDetailsUntilCancelled() noexcept;
[[nodiscard]] bool libraryDetailsWorkerStarted() noexcept;
void clearResultLibraryRequestHooks() noexcept;

} // namespace bte::bindings::testing
