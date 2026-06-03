#pragma once

#include "Bte/Frontend/IChartView.h"

#include <QWidget>

#include <cstddef>
#include <memory>

class QChart;
class QChartView;
class QCandlestickSeries;
class QDateTimeAxis;
class QValueAxis;

namespace bte::frontend {

class QtChartsCandlestickView final : public QWidget, public IChartView {
  Q_OBJECT

public:
  explicit QtChartsCandlestickView(QWidget *parent = nullptr);

  void setBarWindow(std::span<const core::Bar> visible) override;
  void appendBar(const core::Bar &bar) override;
  void clearMarkers() override;

  [[nodiscard]] std::size_t candleCount() const noexcept;

public slots:
  void zoomIn();
  void zoomOut();
  void resetZoom();

private:
  void resetAxes();

  QChart *chart_ = nullptr;
  QChartView *chartView_ = nullptr;
  QCandlestickSeries *candles_ = nullptr;
  QDateTimeAxis *axisX_ = nullptr;
  QValueAxis *axisY_ = nullptr;
};

} // namespace bte::frontend
