#pragma once

#include "Bte/Results/ResultStore.h"

#include <optional>
#include <string>
#include <string_view>

namespace bte::results::detail {

[[nodiscard]] std::string_view strategyModeName(StrategyMode mode);
[[nodiscard]] std::optional<std::string_view>
availabilityName(ProvenanceAvailabilityState state);

[[nodiscard]] core::Result<std::string>
serializeStoredProvenance(const ResultProvenance &provenance);
[[nodiscard]] core::Result<ResultProvenance>
parseStoredProvenance(std::string_view bytes);

} // namespace bte::results::detail
