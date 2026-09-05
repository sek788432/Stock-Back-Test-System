#pragma once

#include "Bte/Core/Bar.h"

#include <span>

namespace bte::frontend {

class IChartView {
public:
  IChartView() = default;
  virtual ~IChartView() = default;
  IChartView(const IChartView &) = delete;
  IChartView &operator=(const IChartView &) = delete;
  IChartView(IChartView &&) = delete;
  IChartView &operator=(IChartView &&) = delete;

  virtual void setBarWindow(std::span<const core::Bar> visible) = 0;
  virtual void appendBar(const core::Bar &bar) = 0;
};

} // namespace bte::frontend
