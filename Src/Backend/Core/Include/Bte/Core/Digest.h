#pragma once

#include <cstddef>
#include <span>
#include <string>
#include <string_view>

namespace bte::core {

[[nodiscard]] std::string sha256(std::span<const std::byte> bytes);
[[nodiscard]] std::string sha256(std::string_view text);

} // namespace bte::core
