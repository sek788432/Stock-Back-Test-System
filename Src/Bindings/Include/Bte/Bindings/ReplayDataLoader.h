#pragma once

#include "Bte/Core/Bar.h"

#include <QDate>
#include <QString>

#include <vector>

namespace bte::bindings {

std::vector<bte::core::Bar> loadReplayBars(const QString &symbol,
                                           const QString &schemaName,
                                           QDate start, QDate end);

} // namespace bte::bindings
