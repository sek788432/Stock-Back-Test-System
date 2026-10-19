#pragma once

#include "Bte/Results/ResultStore.h"

#include <vector>

namespace bte::bindings {

/// Value-owned entries and recovery notices from one opened Results store.
struct ResultCatalogSnapshot final {
  std::vector<results::ResultSummary> entries;
  std::vector<results::ResultRecoveryNotice> notices;
};

} // namespace bte::bindings
