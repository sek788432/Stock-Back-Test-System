#pragma once

#include "Bte/Core/Bar.h" // IWYU pragma: keep
#include "Bte/Frontend/IChartView.h"

#include <QObject>
#include <QWidget>

#include <cstddef>
#include <span> // IWYU pragma: keep

class QChart;
class QChartView;
class QCandlestickSeries;
class QDateTimeAxis;
class QLineSeries;
class QScatterSeries;
class QValueAxis;

namespace bte::frontend {

class QtChartsCandlestickView final : public QWidget, public IChartView {
  Q_OBJECT

public:
  explicit QtChartsCandlestickView(QWidget *parent = nullptr);

  void setBarWindow(std::span<const core::Bar> visible) override;
  void appendBar(const core::Bar &bar) override;
  void setMarkers(std::span<const ChartMarker> markers) override;
  void clearMarkers() override;

  [[nodiscard]] std::size_t candleCount() const noexcept;
  [[nodiscard]] std::size_t volumePointCount() const noexcept;
  [[nodiscard]] std::size_t markerCount() const noexcept;

  void zoomIn();
  void zoomOut();
  void resetZoom();

private:
  void resetAxes();

  QChart *chart_ = nullptr;
  QChartView *chartView_ = nullptr;
  QCandlestickSeries *candles_ = nullptr;
  QLineSeries *volume_ = nullptr;
  QScatterSeries *buyMarkers_ = nullptr;
  QScatterSeries *sellMarkers_ = nullptr;
  QDateTimeAxis *axisX_ = nullptr;
  QValueAxis *axisY_ = nullptr;
  QValueAxis *volumeAxis_ = nullptr;
};

} // namespace bte::frontend
