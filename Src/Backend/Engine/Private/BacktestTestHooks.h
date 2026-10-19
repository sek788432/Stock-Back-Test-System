#pragma once

#include <cstdint>

namespace bte::engine::testing {

[[nodiscard]] std::uint64_t backtestStartCount() noexcept;
void recordBacktestStart() noexcept;

} // namespace bte::engine::testing
