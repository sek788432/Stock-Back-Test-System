#pragma once

#include "Bte/Core/Bar.h"
#include "Bte/Core/Cancellation.h"
#include "Bte/Core/Result.h"

#include <QDate>
#include <QString>

#include <vector>

namespace bte::bindings {

[[nodiscard]] bte::core::Result<std::vector<bte::core::Bar>>
loadBacktestBars(const QString &symbol, const QString &schemaName, QDate start,
                 QDate end, const core::CancellationToken &cancellation = {});

std::vector<bte::core::Bar> loadReplayBars(const QString &symbol,
                                           const QString &schemaName,
                                           QDate start, QDate end);

} // namespace bte::bindings
