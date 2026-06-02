#pragma once

#include "Bte/Core/Bar.h"
#include "Bte/Core/Result.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace bte::core {

struct DateRange {
    Timestamp start{};
    Timestamp end{};
};

namespace time {

[[nodiscard]] Result<Timestamp> parseIso8601(std::string_view text);
[[nodiscard]] std::string toIso8601(Timestamp timestamp);
[[nodiscard]] Timestamp fromUnixMillis(std::int64_t milliseconds);
[[nodiscard]] std::int64_t toUnixMillis(Timestamp timestamp);

} // namespace time

} // namespace bte::core
