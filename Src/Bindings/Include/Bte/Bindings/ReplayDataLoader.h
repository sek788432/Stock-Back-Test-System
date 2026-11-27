#pragma once

#include "Bte/Core/Bar.h"
#include "Bte/Core/Cancellation.h"
#include "Bte/Core/Result.h"

#include <QDate>
#include <QString>

#include <filesystem>
#include <string>
#include <vector>

namespace bte::bindings {

struct ReplayDataSource final {
  std::string symbol;
  std::string timeframe;
};

[[nodiscard]] std::filesystem::path replayCsvDataDirectory();

[[nodiscard]] bte::core::Result<std::vector<ReplayDataSource>>
availableReplaySources(const std::filesystem::path &csvDirectory);

[[nodiscard]] bte::core::Result<std::vector<bte::core::Bar>>
loadBacktestBars(const QString &symbol, const QString &schemaName, QDate start,
                 QDate end, const core::CancellationToken &cancellation = {});

[[nodiscard]] bte::core::Result<std::vector<bte::core::Bar>>
loadBacktestBarsFrom(const QString &symbol, const QString &schemaName,
                     QDate start, QDate end,
                     const std::filesystem::path &csvDirectory,
                     const core::CancellationToken &cancellation = {});

std::vector<bte::core::Bar> loadReplayBars(const QString &symbol,
                                           const QString &schemaName,
                                           QDate start, QDate end);

} // namespace bte::bindings
