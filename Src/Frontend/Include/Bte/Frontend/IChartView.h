#pragma once

#include "Bte/Core/Bar.h"

#include <span>

namespace bte::frontend {

class IChartView {
  public:
    virtual ~IChartView() = default;

    virtual void setBarWindow(std::span<const core::Bar> visible) = 0;
    virtual void appendBar(const core::Bar& bar) = 0;
    virtual void clearMarkers() = 0;
};

} // namespace bte::frontend
