#pragma once

#include "Bte/Core/Result.h"

#include <filesystem>
#include <string>
#include <unordered_set>

namespace bte::data {

[[nodiscard]] core::Result<std::unordered_set<std::string>>
publishedSnapshotSegments(const std::filesystem::path &storeDirectory);

} // namespace bte::data
