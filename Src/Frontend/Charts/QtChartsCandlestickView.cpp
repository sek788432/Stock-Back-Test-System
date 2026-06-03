#include "Bte/Frontend/QtChartsCandlestickView.h"

#include <QCandlestickSeries>
#include <QCandlestickSet>
#include <QChart>
#include <QChartView>
#include <QColor>
#include <QDateTime>
#include <QDateTimeAxis>
#include <QGraphicsLineItem>
#include <QLegend>
#include <QMargins>
#include <QMouseEvent>
#include <QPainter>
#include <QPen>
#include <QTimeZone>
#include <QVBoxLayout>
#include <QValueAxis>
#include <QWheelEvent>

#include <algorithm>
#include <chrono>
#include <limits>

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
  return new QCandlestickSet(bar.open, bar.high, bar.low, bar.close,
                             toEpochMilliseconds(bar.ts));
}

} // namespace

QtChartsCandlestickView::QtChartsCandlestickView(QWidget *parent)
    : QWidget(parent), chart_(new QChart()),
      chartView_(new InteractiveChartView(chart_, this)),
      candles_(new QCandlestickSeries(chart_)),
      axisX_(new QDateTimeAxis(chart_)), axisY_(new QValueAxis(chart_)) {
  setObjectName("replayCandlestickChartView");
  setAccessibleName("K-line candlestick chart");

  candles_->setName(tr("OHLC"));
  candles_->setIncreasingColor(QColor{42, 178, 121});
  candles_->setDecreasingColor(QColor{232, 82, 86});
  candles_->setBodyWidth(0.34);
  candles_->setCapsWidth(0.22);

  chart_->addSeries(candles_);
  chart_->legend()->hide();
  chart_->setBackgroundBrush(QColor{6, 18, 28});
  chart_->setPlotAreaBackgroundBrush(QColor{7, 21, 33});
  chart_->setPlotAreaBackgroundVisible(true);
  chart_->setMargins(QMargins{0, 0, 0, 0});
  chart_->addAxis(axisX_, Qt::AlignBottom);
  chart_->addAxis(axisY_, Qt::AlignLeft);
  candles_->attachAxis(axisX_);
  candles_->attachAxis(axisY_);

  axisX_->setFormat("MM-dd");
  axisY_->setLabelFormat("%.2f");
  axisX_->setGridLineColor(QColor{31, 51, 70});
  axisY_->setGridLineColor(QColor{31, 51, 70});
  axisX_->setLinePenColor(QColor{74, 101, 126});
  axisY_->setLinePenColor(QColor{74, 101, 126});
  axisX_->setLabelsColor(QColor{198, 220, 239});
  axisY_->setLabelsColor(QColor{198, 220, 239});

  chartView_->setObjectName("replayChartView");
  chartView_->setAccessibleName("K-line chart viewport");
  chartView_->setRenderHint(QPainter::Antialiasing);
  chartView_->setStyleSheet("background-color: #06121d; border: 0;");
  chartView_->setBackgroundBrush(QColor{6, 18, 29});

  auto *layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->addWidget(chartView_);

  resetAxes();
}

void QtChartsCandlestickView::setBarWindow(
    const std::span<const core::Bar> visible) {
  candles_->clear();
  for (const auto &bar : visible) {
    appendBar(bar);
  }
  resetAxes();
}

void QtChartsCandlestickView::appendBar(const core::Bar &bar) {
  if (!bar.isValid()) {
    return;
  }
  candles_->append(makeCandleSet(bar));
  resetAxes();
}

void QtChartsCandlestickView::clearMarkers() {}

std::size_t QtChartsCandlestickView::candleCount() const noexcept {
  return static_cast<std::size_t>(candles_->count());
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
  } else {
    qreal minTimestamp = std::numeric_limits<qreal>::max();
    qreal maxTimestamp = std::numeric_limits<qreal>::lowest();
    qreal minPrice = std::numeric_limits<qreal>::max();
    qreal maxPrice = std::numeric_limits<qreal>::lowest();

    for (auto *set : candles_->sets()) {
      minTimestamp = std::min(minTimestamp, set->timestamp());
      maxTimestamp = std::max(maxTimestamp, set->timestamp());
      minPrice = std::min(minPrice, set->low());
      maxPrice = std::max(maxPrice, set->high());
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
  }
}

} // namespace bte::frontend
