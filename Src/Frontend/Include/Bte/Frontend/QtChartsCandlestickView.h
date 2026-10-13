#pragma once

#include "Bte/Core/Bar.h" // IWYU pragma: keep
#include "Bte/Frontend/IChartView.h"
#include "Bte/Frontend/ReplayBarViewport.h"

#include <QObject>
#include <QWidget>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span> // IWYU pragma: keep
#include <unordered_map>
#include <vector>

class QChart;
class QChartView;
class QCandlestickSeries;
class QDateTimeAxis;
class QGraphicsSimpleTextItem;
class QLineSeries;
class QScatterSeries;
class QValueAxis;

namespace bte::frontend {

class TradeMarkerLayer;
class VolumeBarLayer;
class InteractiveChartView;

class QtChartsCandlestickView final : public QWidget, public IChartView {
  Q_OBJECT

public:
  explicit QtChartsCandlestickView(QWidget *parent = nullptr);
  ~QtChartsCandlestickView() override;
  QtChartsCandlestickView(const QtChartsCandlestickView &) = delete;
  QtChartsCandlestickView &operator=(const QtChartsCandlestickView &) = delete;
  QtChartsCandlestickView(QtChartsCandlestickView &&) = delete;
  QtChartsCandlestickView &operator=(QtChartsCandlestickView &&) = delete;

  void setBarWindow(std::span<const core::Bar> visible) override;
  void appendBar(const core::Bar &bar) override;
  void appendBars(std::span<const core::Bar> bars) override;
  void setMarkers(std::span<const ChartMarker> markers) override;
  void appendMarkers(std::span<const ChartMarker> markers);
  void clearMarkers() override;

  [[nodiscard]] std::size_t candleCount() const noexcept;
  [[nodiscard]] std::size_t volumePointCount() const noexcept;
  [[nodiscard]] std::size_t markerCount() const noexcept;
  [[nodiscard]] ReplayVisibleSlots visibleSlots() const;
  [[nodiscard]] bool isFollowingLatest() const;
  [[nodiscard]] bool canFitReachedBars() const;
  [[nodiscard]] bool fitReachedBars();
  void followLatest();

  void zoomIn();
  void zoomOut();
  void resetZoom();
  void setDailyTimeframe(bool daily);

signals:
  void viewportModeChanged(bool followingLatest);

private:
  friend class InteractiveChartView;
  void zoomAt(double factor, double anchorFraction);
  void panByPixels(double pixelDelta);
  void resetAxes();
  void alignVolumePlot();
  void arrangeUtcTimeLabels();

  QChart *chart_ = nullptr;
  QChart *volumeChart_ = nullptr;
  InteractiveChartView *chartView_ = nullptr;
  QChartView *volumeChartView_ = nullptr;
  QCandlestickSeries *candles_ = nullptr;
  QLineSeries *volume_ = nullptr;
  QScatterSeries *buyMarkers_ = nullptr;
  QScatterSeries *sellMarkers_ = nullptr;
  QDateTimeAxis *axisX_ = nullptr;
  QDateTimeAxis *volumeAxisX_ = nullptr;
  QValueAxis *axisY_ = nullptr;
  QValueAxis *volumeAxis_ = nullptr;
  std::unique_ptr<TradeMarkerLayer> markerLayer_;
  std::unique_ptr<VolumeBarLayer> volumeBarLayer_;
  std::array<QGraphicsSimpleTextItem *, 5> utcTimeLabels_{};
  bool dailyTimeframe_ = false;
  ReplayBarViewport viewport_;
  double lastPlotWidth_ = 0.0;
  double lastPlotHeight_ = 0.0;
  double reservedLabelWidth_ = 0.0;
  std::vector<core::Bar> bars_;
  std::unordered_map<std::int64_t, double> slotByTimestamp_;
};

} // namespace bte::frontend
