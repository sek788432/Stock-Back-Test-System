#pragma once

namespace bte::bindings::testing {

void blockNextLibraryDetailsUntilCancelled() noexcept;
[[nodiscard]] bool libraryDetailsWorkerStarted() noexcept;
void blockNextLibraryCancellationBoundaryUntilCancelled() noexcept;
[[nodiscard]] bool libraryCancellationBoundaryWorkerStarted() noexcept;
[[nodiscard]] bool libraryCancellationBoundaryWorkerFinished() noexcept;
void clearResultLibraryRequestHooks() noexcept;

} // namespace bte::bindings::testing
