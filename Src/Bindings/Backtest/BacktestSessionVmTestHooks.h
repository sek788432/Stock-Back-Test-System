#pragma once

#include "Bte/Core/Bar.h"
#include "Bte/Core/Cancellation.h"
#include "Bte/Core/Result.h"

#include <QDate>

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace bte::bindings::testing {

void cancelAfterNextDraftStoreOpen(core::CancellationSource &source) noexcept;
void clearDraftStoreOpenHook() noexcept;

[[nodiscard]] core::Result<std::vector<core::Bar>> selectPythonWarmupBars(
    const std::filesystem::path &dataStore, const std::string &snapshotId,
    const std::string &symbol, QDate startDate, std::size_t maximumHistory,
    const core::CancellationToken &cancellation = {});

} // namespace bte::bindings::testing
