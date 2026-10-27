#pragma once

#include "Bte/Core/Result.h"

#include <nlohmann/json.hpp>

#include <string>
#include <string_view>

namespace bte::core {

[[nodiscard]] Result<nlohmann::json> parseCanonicalJson(std::string_view bytes);

[[nodiscard]] Result<std::string>
serializeCanonicalJson(const nlohmann::json &value);

} // namespace bte::core
