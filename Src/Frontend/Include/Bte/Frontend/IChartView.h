#pragma once

#include "Bte/Core/Bar.h"

#include <optional>
#include <span>

namespace bte::frontend {

struct ChartMarker {
  core::Timestamp timestamp;
  double price = 0.0;
  bool isBuy = false;
  // Daily presentation anchors to its containing candle; timestamp above
  // remains the exact fill time for inspection and synchronized trade rows.
  std::optional<core::Timestamp> candleTimestamp = std::nullopt;
};

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
  virtual void appendBars(std::span<const core::Bar> bars) = 0;
  virtual void setMarkers(std::span<const ChartMarker> markers) = 0;
  virtual void clearMarkers() = 0;
};

} // namespace bte::frontend
