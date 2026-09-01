#include "Bte/Frontend/QtChartsCandlestickView.h"

#include <QBrush>
#include <QCandlestickSeries>
#include <QCandlestickSet>
#include <QChart>
#include <QChartView>
#include <QColor>
#include <QDateTime>
#include <QDateTimeAxis>
#include <QEvent> // IWYU pragma: keep
#include <QGraphicsLineItem>
#include <QGraphicsScene>
#include <QLegend>
#include <QLineF>
#include <QLineSeries>
#include <QList>
#include <QMargins>
#include <QMouseEvent>
#include <QPainter>
#include <QPen>
#include <QPoint>
#include <QRectF>
#include <QScatterSeries>
#include <QTimeZone>
#include <QVBoxLayout>
#include <QValueAxis>
#include <QtCore/Qt>
#include <QtGlobal>

#include <algorithm>
#include <chrono>
#include <limits>
#include <memory>
#include <span>

#include "Bte/Core/Bar.h"

namespace bte::frontend {
namespace {

class InteractiveChartView final : public QChartView {
public:
  explicit InteractiveChartView(QChart *chart, QWidget *parent = nullptr)
      : QChartView(chart, parent) {
    setMouseTracking(true);
    setRubberBand(QChartView::NoRubberBand);

    const QPen pen{QColor{137, 165, 190, 160}, 1, Qt::DashLine};
    verticalCrosshair_ = chart->scene()->addLine(QLineF{}, pen);
    horizontalCrosshair_ = chart->scene()->addLine(QLineF{}, pen);
    verticalCrosshair_->setZValue(20);
    horizontalCrosshair_->setZValue(20);
    verticalCrosshair_->hide();
    horizontalCrosshair_->hide();
  }

protected:
  void wheelEvent(QWheelEvent *event) override {
    if (event->angleDelta().y() > 0) {
      chart()->zoom(1.2);
    } else {
      chart()->zoom(1.0 / 1.2);
    }
    event->accept();
  }

  void mousePressEvent(QMouseEvent *event) override {
    const auto scenePos = mapToScene(event->pos());
    if (event->button() == Qt::LeftButton &&
        chart()->plotArea().contains(scenePos)) {
      panning_ = true;
      lastPanPos_ = event->pos();
      setCursor(Qt::ClosedHandCursor);
      event->accept();
      return;
    }
    QChartView::mousePressEvent(event);
  }

  void mouseMoveEvent(QMouseEvent *event) override {
    if (panning_) {
      const auto delta = event->pos() - lastPanPos_;
      chart()->scroll(-delta.x(), delta.y());
      lastPanPos_ = event->pos();
      event->accept();
      return;
    }

    updateCrosshair(mapToScene(event->pos()));
    QChartView::mouseMoveEvent(event);
  }

  void mouseReleaseEvent(QMouseEvent *event) override {
    if (event->button() == Qt::LeftButton && panning_) {
      panning_ = false;
      unsetCursor();
      event->accept();
      return;
    }
    QChartView::mouseReleaseEvent(event);
  }

  void leaveEvent(QEvent *event) override {
    verticalCrosshair_->hide();
    horizontalCrosshair_->hide();
    QChartView::leaveEvent(event);
  }

private:
  void updateCrosshair(const QPointF &scenePos) {
    const auto plot = chart()->plotArea();
    if (!plot.contains(scenePos)) {
      verticalCrosshair_->hide();
      horizontalCrosshair_->hide();
      return;
    }

    verticalCrosshair_->setLine(scenePos.x(), plot.top(), scenePos.x(),
                                plot.bottom());
    horizontalCrosshair_->setLine(plot.left(), scenePos.y(), plot.right(),
                                  scenePos.y());
    verticalCrosshair_->show();
    horizontalCrosshair_->show();
  }

  QGraphicsLineItem *verticalCrosshair_ = nullptr;
  QGraphicsLineItem *horizontalCrosshair_ = nullptr;
  QPoint lastPanPos_;
  bool panning_ = false;
};

qreal toEpochMilliseconds(const core::Timestamp timestamp) {
  return static_cast<qreal>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          timestamp.time_since_epoch())
          .count());
}

QCandlestickSet *makeCandleSet(const core::Bar &bar) {
  return std::make_unique<QCandlestickSet>(bar.open, bar.high, bar.low,
                                           bar.close,
                                           toEpochMilliseconds(bar.ts))
      .release();
}

} // namespace

QtChartsCandlestickView::QtChartsCandlestickView(QWidget *parent)
    : QWidget(parent), chart_(std::make_unique<QChart>().release()),
      chartView_(
          std::make_unique<InteractiveChartView>(chart_, this).release()),
      candles_(std::make_unique<QCandlestickSeries>(chart_).release()),
      volume_(std::make_unique<QLineSeries>(chart_).release()),
      buyMarkers_(std::make_unique<QScatterSeries>(chart_).release()),
      sellMarkers_(std::make_unique<QScatterSeries>(chart_).release()),
      axisX_(std::make_unique<QDateTimeAxis>(chart_).release()),
      axisY_(std::make_unique<QValueAxis>(chart_).release()),
      volumeAxis_(std::make_unique<QValueAxis>(chart_).release()) {
  setObjectName("replayCandlestickChartView");
  setAccessibleName("K-line candlestick chart");

  candles_->setName(tr("OHLC"));
  candles_->setIncreasingColor(QColor{42, 178, 121});
  candles_->setDecreasingColor(QColor{232, 82, 86});
  candles_->setBodyWidth(0.34);
  candles_->setCapsWidth(0.22);
  volume_->setName(tr("Volume"));
  volume_->setColor(QColor{70, 145, 181, 150});
  buyMarkers_->setName(tr("Buy"));
  buyMarkers_->setColor(QColor{52, 211, 153});
  buyMarkers_->setMarkerShape(QScatterSeries::MarkerShapeTriangle);
  buyMarkers_->setMarkerSize(13.0);
  sellMarkers_->setName(tr("Sell"));
  sellMarkers_->setColor(QColor{251, 113, 133});
  sellMarkers_->setMarkerShape(QScatterSeries::MarkerShapeRectangle);
  sellMarkers_->setMarkerSize(11.0);

  chart_->addSeries(candles_);
  chart_->addSeries(volume_);
  chart_->addSeries(buyMarkers_);
  chart_->addSeries(sellMarkers_);
  chart_->legend()->setVisible(true);
  chart_->setBackgroundBrush(QColor{6, 18, 28});
  chart_->setPlotAreaBackgroundBrush(QColor{7, 21, 33});
  chart_->setPlotAreaBackgroundVisible(true);
  chart_->setMargins(QMargins{0, 0, 0, 0});
  chart_->addAxis(axisX_, Qt::AlignBottom);
  chart_->addAxis(axisY_, Qt::AlignLeft);
  chart_->addAxis(volumeAxis_, Qt::AlignRight);
  candles_->attachAxis(axisX_);
  candles_->attachAxis(axisY_);
  volume_->attachAxis(axisX_);
  volume_->attachAxis(volumeAxis_);
  buyMarkers_->attachAxis(axisX_);
  buyMarkers_->attachAxis(axisY_);
  sellMarkers_->attachAxis(axisX_);
  sellMarkers_->attachAxis(axisY_);

  axisX_->setFormat("MM-dd");
  axisY_->setLabelFormat("%.2f");
  volumeAxis_->setLabelFormat("%.0f");
  volumeAxis_->setTitleText(tr("Volume"));
  axisX_->setGridLineColor(QColor{31, 51, 70});
  axisY_->setGridLineColor(QColor{31, 51, 70});
  axisX_->setLinePenColor(QColor{74, 101, 126});
  axisY_->setLinePenColor(QColor{74, 101, 126});
  axisX_->setLabelsColor(QColor{198, 220, 239});
  axisY_->setLabelsColor(QColor{198, 220, 239});
  volumeAxis_->setLabelsColor(QColor{112, 172, 201});

  chartView_->setObjectName("replayChartView");
  chartView_->setAccessibleName("K-line chart viewport");
  chartView_->setRenderHint(QPainter::Antialiasing);
  chartView_->setStyleSheet("background-color: #06121d; border: 0;");
  chartView_->setBackgroundBrush(QColor{6, 18, 29});

  auto *layout = std::make_unique<QVBoxLayout>(this).release();
  layout->setContentsMargins(0, 0, 0, 0);
  layout->addWidget(chartView_);

  resetAxes();
}

void QtChartsCandlestickView::setBarWindow(
    const std::span<const core::Bar> visible) {
  candles_->clear();
  volume_->clear();
  for (const auto &bar : visible) {
    if (bar.isValid()) {
      candles_->append(makeCandleSet(bar));
      volume_->append(toEpochMilliseconds(bar.ts), bar.volume);
    }
  }
  resetAxes();
}

void QtChartsCandlestickView::appendBar(const core::Bar &bar) {
  if (!bar.isValid()) {
    return;
  }
  candles_->append(makeCandleSet(bar));
  volume_->append(toEpochMilliseconds(bar.ts), bar.volume);
  resetAxes();
}

void QtChartsCandlestickView::setMarkers(
    const std::span<const ChartMarker> markers) {
  clearMarkers();
  for (const auto &marker : markers) {
    auto *series = marker.isBuy ? buyMarkers_ : sellMarkers_;
    series->append(toEpochMilliseconds(marker.timestamp), marker.price);
  }
}

void QtChartsCandlestickView::clearMarkers() {
  buyMarkers_->clear();
  sellMarkers_->clear();
}

std::size_t QtChartsCandlestickView::candleCount() const noexcept {
  return static_cast<std::size_t>(candles_->count());
}

std::size_t QtChartsCandlestickView::volumePointCount() const noexcept {
  return static_cast<std::size_t>(volume_->count());
}

std::size_t QtChartsCandlestickView::markerCount() const noexcept {
  return static_cast<std::size_t>(buyMarkers_->count() + sellMarkers_->count());
}

void QtChartsCandlestickView::zoomIn() { chart_->zoom(1.2); }

void QtChartsCandlestickView::zoomOut() { chart_->zoom(1.0 / 1.2); }

void QtChartsCandlestickView::resetZoom() {
  chart_->zoomReset();
  resetAxes();
}

void QtChartsCandlestickView::resetAxes() {
  if (candles_->count() == 0) {
    const auto now = QDateTime::currentDateTimeUtc();
    axisX_->setRange(now.addDays(-1), now);
    axisY_->setRange(0.0, 1.0);
    volumeAxis_->setRange(0.0, 1.0);
  } else {
    qreal minTimestamp = std::numeric_limits<qreal>::max();
    qreal maxTimestamp = std::numeric_limits<qreal>::lowest();
    qreal minPrice = std::numeric_limits<qreal>::max();
    qreal maxPrice = std::numeric_limits<qreal>::lowest();
    qreal maxVolume = 0.0;

    for (auto *set : candles_->sets()) {
      minTimestamp = std::min(minTimestamp, set->timestamp());
      maxTimestamp = std::max(maxTimestamp, set->timestamp());
      minPrice = std::min(minPrice, set->low());
      maxPrice = std::max(maxPrice, set->high());
    }
    for (const auto &point : volume_->points()) {
      maxVolume = std::max(maxVolume, point.y());
    }

    if (minTimestamp == maxTimestamp) {
      minTimestamp -= 12.0 * 60.0 * 60.0 * 1000.0;
      maxTimestamp += 12.0 * 60.0 * 60.0 * 1000.0;
    }

    const auto padding = std::max((maxPrice - minPrice) * 0.08, 1.0);
    axisX_->setRange(QDateTime::fromMSecsSinceEpoch(
                         static_cast<qint64>(minTimestamp), QTimeZone::UTC),
                     QDateTime::fromMSecsSinceEpoch(
                         static_cast<qint64>(maxTimestamp), QTimeZone::UTC));
    axisY_->setRange(std::max(0.0, minPrice - padding), maxPrice + padding);
    volumeAxis_->setRange(0.0, std::max(1.0, maxVolume * 4.0));
  }
}

} // namespace bte::frontend
