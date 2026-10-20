#include "Bte/Frontend/QtChartsCandlestickView.h"

// IWYU pragma: no_include <math>

#include <QBrush>
#include <QCandlestickSeries>
#include <QCandlestickSet>
#include <QChart>
#include <QChartView>
#include <QColor>
#include <QDateTime>
#include <QDateTimeAxis>
#include <QEvent> // IWYU pragma: keep
#include <QFrame>
#include <QGraphicsLayout>
#include <QGraphicsLineItem>
#include <QGraphicsScene>
#include <QLegend>
#include <QLegendMarker>
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
#include <QString>
#include <QTimeZone>
#include <QVBoxLayout>
#include <QValueAxis>
#include <QVariant>
#include <QtCharts/qabstractaxis.h>
#include <QtCore/Qt>
#include <QtGlobal>
#include <QtGui/qimage.h>
#include <QtGui/qpixmap.h>
#include <QtGui/qpolygon.h>

#include <algorithm>
#include <chrono>
#include <cmath> // IWYU pragma: keep
#include <compare>
#include <initializer_list>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

#include "Bte/Core/Bar.h"

namespace bte::frontend {

class InteractiveChartView final : public QChartView {
public:
  explicit InteractiveChartView(QChart *chart, QtChartsCandlestickView &owner)
      : QChartView(chart, &owner), owner_{owner} {
    setMouseTracking(true);
    setRubberBand(QChartView::NoRubberBand);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    const QPen pen{QColor{137, 165, 190, 160}, 1, Qt::DashLine};
    verticalCrosshair_ = chart->scene()->addLine(QLineF{}, pen);
    horizontalCrosshair_ = chart->scene()->addLine(QLineF{}, pen);
    verticalCrosshair_->setZValue(20);
    horizontalCrosshair_->setZValue(20);
    verticalCrosshair_->hide();
    horizontalCrosshair_->hide();
  }

  void resetWheelGesture() noexcept {
    lastWheelEvent_.reset();
    wheelBurstDelta_ = 0.0;
  }

protected:
  void wheelEvent(QWheelEvent *event) override {
    const auto angleDelta = event->angleDelta().y();
    const auto delta = angleDelta != 0 ? angleDelta : event->pixelDelta().y();
    const auto now = std::chrono::steady_clock::now();
    if (!lastWheelEvent_ || event->phase() == Qt::ScrollBegin ||
        (event->phase() == Qt::NoScrollPhase &&
         now - *lastWheelEvent_ > std::chrono::milliseconds{750})) {
      wheelBurstDelta_ = 0.0;
    }
    lastWheelEvent_ = now;
    const auto nextDelta =
        std::clamp(wheelBurstDelta_ + static_cast<qreal>(delta), -240.0, 240.0);
    const auto appliedDelta = nextDelta - wheelBurstDelta_;
    wheelBurstDelta_ = nextDelta;
    if (appliedDelta != 0.0) {
      const auto plot = chart()->plotArea();
      const auto scene = mapToScene(event->position().toPoint());
      const auto anchor =
          plot.width() > 0.0 ? (scene.x() - plot.left()) / plot.width() : 0.5;
      owner_.zoomAt(std::pow(1.2, appliedDelta / 120.0), anchor);
    }
    if (event->phase() == Qt::ScrollEnd) {
      resetWheelGesture();
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
      owner_.panByPixels(-static_cast<double>(delta.x()));
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
  QtChartsCandlestickView &owner_;
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
  std::optional<std::chrono::steady_clock::time_point> lastWheelEvent_;
  qreal wheelBurstDelta_ = 0.0;
  bool panning_ = false;
};

namespace {

qreal toEpochMilliseconds(const core::Timestamp timestamp) {
  return static_cast<qreal>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          timestamp.time_since_epoch())
          .count());
}

constexpr qreal slotIntervalMilliseconds = 60.0 * 60.0 * 1000.0;

qreal toSlotCoordinate(const std::size_t index) {
  return static_cast<qreal>(index) * slotIntervalMilliseconds;
}

QCandlestickSet *makeCandleSet(const core::Bar &bar, const qreal slot) {
  auto owner = std::make_unique<QCandlestickSet>(bar.open, bar.high, bar.low,
                                                 bar.close, slot);
  owner->setProperty("replayUtcTimestamp",
                     static_cast<qint64>(toEpochMilliseconds(bar.ts)));
  return owner.release();
}

QImage makeTradeGlyph(const bool isBuy, const QColor &color) {
  QImage image{16, 16, QImage::Format_ARGB32_Premultiplied};
  image.fill(Qt::transparent);
  QPainter painter{&image};
  painter.setRenderHint(QPainter::Antialiasing);
  painter.setPen(QPen{color, 1.0});
  painter.setBrush(color);
  const auto triangle = isBuy ? QPolygonF{{8.0, 1.0}, {1.0, 14.0}, {15.0, 14.0}}
                              : QPolygonF{{1.0, 1.0}, {15.0, 1.0}, {8.0, 14.0}};
  painter.drawPolygon(triangle);
  return image;
}

} // namespace

// Owns presentation-only graphics. Series points retain exact financial values;
// glyphs can move in pixel space without altering a fill or its price axis.
class TradeMarkerLayer final : public QObject {
public:
  TradeMarkerLayer(QChart &chart, QDateTimeAxis &axisX, QValueAxis &axisY)
      : chart_{chart} {
    connect(&chart, &QChart::plotAreaChanged, this, [this] { arrange(); });
    connect(
        &axisX, &QDateTimeAxis::rangeChanged, this, [this] { arrange(); },
        Qt::QueuedConnection);
    connect(
        &axisY, &QValueAxis::rangeChanged, this, [this] { arrange(); },
        Qt::QueuedConnection);
  }

  void clear() {
    buyEntryBySlot_.clear();
    entries_.clear();
  }

  void append(const ChartMarker &marker, const qreal slot,
              const double candleEdgePrice, QScatterSeries &series) {
    const auto slotKey = static_cast<qint64>(slot);
    if (marker.isBuy) {
      const auto existing = buyEntryBySlot_.find(slotKey);
      if (existing != buyEntryBySlot_.end()) {
        auto &entry = entries_.at(existing->second);
        ++entry.fillCount;
        entry.glyph->setToolTip(
            entry.firstFillTooltip +
            tr("\n%1 reached Buy fills in this candle; see the trade log for "
               "every fill.")
                .arg(static_cast<qulonglong>(entry.fillCount)));
        return;
      }
    }
    auto glyph = std::make_unique<QGraphicsPixmapItem>(&chart_);
    glyph->setPixmap(QPixmap::fromImage(series.lightMarker()));
    glyph->setData(0, "replayTradeGlyph");
    glyph->setZValue(10);
    glyph->setToolTip(
        tr("%1 fill — %2 — Price: %3")
            .arg(marker.isBuy ? tr("Buy ▲") : tr("Sell ▼"),
                 QDateTime::fromMSecsSinceEpoch(
                     static_cast<qint64>(toEpochMilliseconds(marker.timestamp)),
                     QTimeZone::UTC)
                     .toString(Qt::ISODate),
                 QString::number(marker.price, 'g', 15)));
    auto leader = std::make_unique<QGraphicsLineItem>(&chart_);
    leader->setPen(QPen{series.color(), 1.0});
    leader->setData(0, "replayTradeLeader");
    leader->setZValue(9);
    const auto firstFillTooltip = glyph->toolTip();
    if (marker.isBuy) {
      buyEntryBySlot_.emplace(slotKey, entries_.size());
    }
    entries_.push_back({.point = {slot, candleEdgePrice},
                        .isBuy = marker.isBuy,
                        .series = &series,
                        .firstFillTooltip = firstFillTooltip,
                        .glyph = std::move(glyph),
                        .leader = std::move(leader)});
  }

  void arrange() {
    const auto plot = chart_.plotArea();
    if (plot.width() < 16.0 || plot.height() < 16.0) {
      for (const auto &entry : entries_) {
        entry.glyph->hide();
        entry.leader->hide();
      }
      return;
    }
    std::unordered_map<qint64, std::size_t> buyTotals;
    std::unordered_map<qint64, std::size_t> sellTotals;
    for (const auto &entry : entries_) {
      auto &totals = entry.isBuy ? buyTotals : sellTotals;
      ++totals[static_cast<qint64>(entry.point.x())];
    }
    std::unordered_map<qint64, std::size_t> buyOrdinals;
    std::unordered_map<qint64, std::size_t> sellOrdinals;
    const auto capacity = static_cast<std::size_t>(
        std::max(1.0, std::floor((plot.width() - 16.0) / 18.0) + 1.0));
    for (const auto &entry : entries_) {
      const auto key = static_cast<qint64>(entry.point.x());
      auto &totals = entry.isBuy ? buyTotals : sellTotals;
      auto &ordinals = entry.isBuy ? buyOrdinals : sellOrdinals;
      const auto ordinal = ordinals[key]++;
      const auto anchor = chart_.mapToPosition(entry.point, entry.series);
      if (!plot.adjusted(-0.01, -0.01, 0.01, 0.01).contains(anchor)) {
        entry.glyph->hide();
        entry.leader->hide();
        continue;
      }
      const auto columns = std::min(totals[key], capacity);
      const auto row = ordinal / columns;
      const auto width = static_cast<qreal>(columns - 1) * 18.0;
      const auto left = std::clamp(anchor.x() - width / 2.0, plot.left() + 8.0,
                                   plot.right() - 8.0 - width);
      const auto direction = entry.isBuy ? 1.0 : -1.0;
      const QPointF center{
          left + static_cast<qreal>(ordinal % columns) * 18.0,
          std::clamp(anchor.y() +
                         direction * (16.0 + static_cast<qreal>(row) * 18.0),
                     plot.top() + 8.0, plot.bottom() - 8.0)};
      entry.glyph->setPos(center - QPointF{8.0, 8.0});
      entry.glyph->show();
      entry.leader->setLine(QLineF{anchor, center});
      entry.leader->setVisible(totals[key] > 1);
    }
  }

private:
  struct Entry {
    QPointF point;
    bool isBuy = false;
    QScatterSeries *series = nullptr;
    QString firstFillTooltip;
    std::size_t fillCount = 1;
    std::unique_ptr<QGraphicsPixmapItem> glyph;
    std::unique_ptr<QGraphicsLineItem> leader;
  };
  QChart &chart_;
  std::vector<Entry> entries_;
  std::unordered_map<qint64, std::size_t> buyEntryBySlot_;
};

class VolumeBarLayer final : public QObject {
public:
  VolumeBarLayer(QChart &chart, QLineSeries &series, QDateTimeAxis &axisX,
                 QValueAxis &axisY)
      : chart_{chart}, series_{series} {
    connect(&chart, &QChart::plotAreaChanged, this, [this] { arrange(); });
    connect(
        &axisX, &QDateTimeAxis::rangeChanged, this, [this] { arrange(); },
        Qt::QueuedConnection);
    connect(
        &axisY, &QValueAxis::rangeChanged, this, [this] { arrange(); },
        Qt::QueuedConnection);
  }

  void clear() {
    entries_.clear();
    visibleFirst_ = 0;
    visibleEnd_ = 0;
  }

  void append(const core::Bar &bar, const qreal slot) {
    auto rectangle = std::make_unique<QGraphicsRectItem>(&chart_);
    const auto rising = bar.close >= bar.open;
    rectangle->setPen(Qt::NoPen);
    rectangle->setBrush(rising ? QColor{42, 178, 121, 190}
                               : QColor{232, 82, 86, 190});
    rectangle->setData(0, "replayVolumeBar");
    rectangle->setToolTip(
        tr("Volume: %1 shares").arg(QString::number(bar.volume, 'f', 0)));
    rectangle->hide();
    entries_.push_back({slot, bar.volume, std::move(rectangle)});
  }

  void arrange() {
    const auto plot = chart_.plotArea();
    if (plot.width() < 16.0 || plot.height() < 16.0 || entries_.empty()) {
      for (auto index = visibleFirst_; index < visibleEnd_; ++index) {
        entries_.at(index).rectangle->hide();
      }
      visibleFirst_ = 0;
      visibleEnd_ = 0;
      return;
    }
    auto *const range = chart_.axes(Qt::Horizontal).front();
    const auto *axis = qobject_cast<const QDateTimeAxis *>(range);
    const auto minTime = static_cast<qreal>(axis->min().toMSecsSinceEpoch());
    const auto maxTime = static_cast<qreal>(axis->max().toMSecsSinceEpoch());
    const auto first = std::lower_bound(
        entries_.begin(), entries_.end(), minTime,
        [](const Entry &entry, const qreal time) { return entry.time < time; });
    const auto last = std::upper_bound(
        first, entries_.end(), maxTime,
        [](const qreal time, const Entry &entry) { return time < entry.time; });
    const auto firstIndex = static_cast<std::size_t>(first - entries_.begin());
    const auto endIndex = static_cast<std::size_t>(last - entries_.begin());
    const auto visibleCount = static_cast<qreal>(last - first);
    const auto width = std::clamp(
        plot.width() / std::max(1.0, visibleCount) * 0.68, 1.0, 12.0);
    for (auto index = visibleFirst_; index < visibleEnd_; ++index) {
      if (index < firstIndex || index >= endIndex) {
        entries_.at(index).rectangle->hide();
      }
    }
    const auto zero = chart_.mapToPosition({minTime, 0.0}, &series_).y();
    for (auto it = first; it != last; ++it) {
      const auto top = chart_.mapToPosition({it->time, it->volume}, &series_);
      it->rectangle->setRect(top.x() - width / 2.0, top.y(), width,
                             std::max(1.0, zero - top.y()));
      it->rectangle->show();
    }
    visibleFirst_ = firstIndex;
    visibleEnd_ = endIndex;
  }

private:
  struct Entry {
    qreal time = 0.0;
    qreal volume = 0.0;
    std::unique_ptr<QGraphicsRectItem> rectangle;
  };
  QChart &chart_;
  QLineSeries &series_;
  std::vector<Entry> entries_;
  std::size_t visibleFirst_ = 0;
  std::size_t visibleEnd_ = 0;
};

QtChartsCandlestickView::QtChartsCandlestickView(QWidget *parent)
    : QWidget(parent), chart_(std::make_unique<QChart>().release()),
      volumeChart_(std::make_unique<QChart>().release()),
      chartView_(
          std::make_unique<InteractiveChartView>(chart_, *this).release()),
      volumeChartView_(
          std::make_unique<QChartView>(volumeChart_, this).release()),
      candles_(std::make_unique<QCandlestickSeries>(chart_).release()),
      volume_(std::make_unique<QLineSeries>(volumeChart_).release()),
      buyMarkers_(std::make_unique<QScatterSeries>(chart_).release()),
      sellMarkers_(std::make_unique<QScatterSeries>(chart_).release()),
      axisX_(std::make_unique<QDateTimeAxis>(chart_).release()),
      volumeAxisX_(std::make_unique<QDateTimeAxis>(volumeChart_).release()),
      axisY_(std::make_unique<QValueAxis>(chart_).release()),
      volumeAxis_(std::make_unique<QValueAxis>(volumeChart_).release()) {
  setObjectName("replayCandlestickChartView");
  setAccessibleName("K-line candlestick chart");

  candles_->setName(tr("OHLC"));
  candles_->setIncreasingColor(QColor{42, 178, 121});
  candles_->setDecreasingColor(QColor{232, 82, 86});
  candles_->setBodyWidth(0.75);
  candles_->setMaximumColumnWidth(18.0);
  candles_->setMinimumColumnWidth(1.0);
  candles_->setCapsWidth(0.22);
  volume_->setName(tr("Volume"));
  volume_->setColor(QColor{70, 145, 181, 150});
  buyMarkers_->setObjectName("replayBuyMarkers");
  buyMarkers_->setName(tr("Buy ▲"));
  buyMarkers_->setColor(QColor{52, 211, 153});
  buyMarkers_->setMarkerShape(QScatterSeries::MarkerShapeTriangle);
  buyMarkers_->setLightMarker(makeTradeGlyph(true, buyMarkers_->color()));
  buyMarkers_->setMarkerSize(16.0);
  sellMarkers_->setObjectName("replaySellMarkers");
  sellMarkers_->setName(tr("Sell ▼"));
  sellMarkers_->setColor(QColor{251, 113, 133});
  sellMarkers_->setMarkerShape(QScatterSeries::MarkerShapeTriangle);
  sellMarkers_->setLightMarker(makeTradeGlyph(false, sellMarkers_->color()));
  sellMarkers_->setMarkerSize(16.0);

  chart_->addSeries(candles_);
  volumeChart_->addSeries(volume_);
  chart_->addSeries(buyMarkers_);
  chart_->addSeries(sellMarkers_);
  chart_->legend()->setVisible(true);
  chart_->setBackgroundBrush(QColor{6, 18, 28});
  chart_->setPlotAreaBackgroundBrush(QColor{7, 21, 33});
  chart_->setPlotAreaBackgroundVisible(true);
  chart_->setMargins(QMargins{0, 0, 110, 0});
  chart_->addAxis(axisX_, Qt::AlignBottom);
  chart_->addAxis(axisY_, Qt::AlignLeft);
  volumeChart_->addAxis(volumeAxisX_, Qt::AlignBottom);
  volumeChart_->addAxis(volumeAxis_, Qt::AlignLeft);
  candles_->attachAxis(axisX_);
  candles_->attachAxis(axisY_);
  volume_->attachAxis(volumeAxisX_);
  volume_->attachAxis(volumeAxis_);
  buyMarkers_->attachAxis(axisX_);
  buyMarkers_->attachAxis(axisY_);
  sellMarkers_->attachAxis(axisX_);
  sellMarkers_->attachAxis(axisY_);

  axisX_->setFormat("yyyy-MM-dd hh:mm");
  volumeAxisX_->setFormat(axisX_->format());
  axisX_->setLabelsVisible(false);
  volumeAxisX_->setLabelsVisible(false);
  volumeAxisX_->setTitleText({});
  axisY_->setLabelFormat("%.2f");
  axisY_->setTitleText(tr("Price (USD)"));
  volumeAxis_->setLabelFormat("%.0f");
  volumeAxis_->setTickCount(3);
  volumeAxis_->setTitleText({});
  axisX_->setGridLineColor(QColor{31, 51, 70});
  axisY_->setGridLineColor(QColor{31, 51, 70});
  axisX_->setLinePenColor(QColor{74, 101, 126});
  axisY_->setLinePenColor(QColor{74, 101, 126});
  axisX_->setLabelsColor(QColor{198, 220, 239});
  axisY_->setLabelsColor(QColor{198, 220, 239});
  volumeAxis_->setLabelsColor(QColor{112, 172, 201});
  volumeAxisX_->setLabelsColor(QColor{198, 220, 239});
  volumeAxisX_->setGridLineColor(QColor{31, 51, 70});
  volumeChart_->legend()->hide();
  volumeChart_->setBackgroundBrush(QColor{6, 18, 28});
  volumeChart_->setPlotAreaBackgroundBrush(QColor{7, 21, 33});
  volumeChart_->setPlotAreaBackgroundVisible(true);
  volumeChart_->setMargins(QMargins{0, 0, 110, 0});
  volume_->hide();
  for (auto *&label : utcTimeLabels_) {
    label = std::make_unique<QGraphicsSimpleTextItem>().release();
    label->setParentItem(volumeChart_);
    label->setData(0, "replayUtcTimeLabel");
    label->setBrush(QColor{198, 220, 239});
    label->setZValue(10.0);
  }
  connect(axisX_, &QDateTimeAxis::rangeChanged, volumeAxisX_,
          [this](const QDateTime &minimum, const QDateTime &maximum) {
            volumeAxisX_->setRange(minimum, maximum);
            arrangeUtcTimeLabels();
          });

  chartView_->setObjectName("replayChartView");
  chartView_->setAccessibleName("K-line chart viewport");
  chartView_->setRenderHint(QPainter::Antialiasing);
  chartView_->setStyleSheet("background-color: #06121d; border: 0;");
  chartView_->setBackgroundBrush(QColor{6, 18, 29});
  chartView_->setFrameShape(QFrame::NoFrame);
  volumeChartView_->setObjectName("replayVolumeChartView");
  volumeChartView_->setAccessibleName("K-line volume viewport (shares)");
  volumeChartView_->setMinimumHeight(96);
  volumeChartView_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  volumeChartView_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  volumeChartView_->setRenderHint(QPainter::Antialiasing);
  volumeChartView_->setStyleSheet("background-color: #06121d; border: 0;");
  volumeChartView_->setFrameShape(QFrame::NoFrame);

  auto *layout = std::make_unique<QVBoxLayout>(this).release();
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(0);
  layout->addWidget(chartView_, 4);
  layout->addWidget(volumeChartView_, 1);

  markerLayer_ = std::make_unique<TradeMarkerLayer>(*chart_, *axisX_, *axisY_);
  volumeBarLayer_ = std::make_unique<VolumeBarLayer>(
      *volumeChart_, *volume_, *volumeAxisX_, *volumeAxis_);
  // Qt can change label margins in the middle of a shared-axis range update.
  // Defer viewport rescaling until all series domains finish that update;
  // synchronous rescaling can reenter Qt's axis/domain notification loop.
  connect(
      chart_, &QChart::plotAreaChanged, this,
      [this] {
        const auto width = std::max(1.0, chart_->plotArea().width());
        const auto height = std::max(1.0, chart_->plotArea().height());
        if (std::fabs(width - lastPlotWidth_) > 0.5 ||
            std::fabs(height - lastPlotHeight_) > 0.5) {
          lastPlotWidth_ = width;
          lastPlotHeight_ = height;
          viewport_.setPlotWidth(width);
          resetAxes();
        }
        alignVolumePlot();
      },
      Qt::QueuedConnection);
  connect(
      volumeChart_, &QChart::plotAreaChanged, this,
      [this] {
        alignVolumePlot();
        arrangeUtcTimeLabels();
      },
      Qt::QueuedConnection);
  // Custom pixel-space glyphs avoid Qt's overlapping scatter rendering. Keep
  // the source points and legends, but do not draw duplicate scatter icons.
  for (auto *series : {buyMarkers_, sellMarkers_}) {
    series->hide();
    for (auto *marker : chart_->legend()->markers(series)) {
      marker->setVisible(true);
    }
  }
  resetAxes();
}

QtChartsCandlestickView::~QtChartsCandlestickView() = default;

void QtChartsCandlestickView::setBarWindow(
    const std::span<const core::Bar> visible) {
  chartView_->resetWheelGesture();
  clearMarkers();
  candles_->clear();
  volume_->clear();
  volumeBarLayer_->clear();
  bars_.clear();
  slotByTimestamp_.clear();
  viewport_.reset();
  viewport_.setReachedCount(0);
  reservedLabelWidth_ = 0.0;
  appendBars(visible);
  chart_->setMargins(QMargins{0, 0, 110, 0});
  volumeChart_->setMargins(QMargins{0, 0, 110, 0});
  chart_->layout()->activate();
  volumeChart_->layout()->activate();
}

void QtChartsCandlestickView::appendBar(const core::Bar &bar) {
  appendBars(std::span<const core::Bar>{&bar, 1});
}

void QtChartsCandlestickView::appendBars(
    const std::span<const core::Bar> bars) {
  QList<QCandlestickSet *> candleSets;
  QList<QPointF> volumePoints;
  std::vector<core::Bar> validBars;
  std::vector<std::unique_ptr<QCandlestickSet>> candleOwners;
  candleSets.reserve(static_cast<qsizetype>(bars.size()));
  volumePoints.reserve(static_cast<qsizetype>(bars.size()));
  candleOwners.reserve(bars.size());
  validBars.reserve(bars.size());
  for (const auto &bar : bars) {
    if (bar.isValid()) {
      const auto slot = toSlotCoordinate(bars_.size() + validBars.size());
      candleOwners.emplace_back(makeCandleSet(bar, slot));
      candleSets.push_back(candleOwners.back().get());
      volumePoints.emplace_back(slot, bar.volume);
      validBars.push_back(bar);
    }
  }
  if (!candleSets.empty() && candles_->append(candleSets)) {
    for (auto &owner : candleOwners) {
      [[maybe_unused]] auto *qtOwnedCandle = owner.release();
    }
    if (volumePoints.size() == 1) {
      volume_->append(volumePoints.front());
    } else {
      // QXYSeries::append(list) emits one signal per point and rebuilds the
      // hidden line geometry repeatedly while the chart is displayed.
      auto reachedVolume = volume_->points();
      reachedVolume.reserve(reachedVolume.size() + volumePoints.size());
      reachedVolume.append(volumePoints);
      volume_->replace(reachedVolume);
    }
    for (const auto &bar : validBars) {
      const auto slot = toSlotCoordinate(bars_.size());
      slotByTimestamp_.emplace(
          static_cast<std::int64_t>(toEpochMilliseconds(bar.ts)), slot);
      bars_.push_back(bar);
      volumeBarLayer_->append(bar, slot);
    }
  }
  viewport_.setPlotWidth(chart_->plotArea().width());
  viewport_.setReachedCount(static_cast<std::size_t>(candles_->count()));
  resetAxes();
  volumeBarLayer_->arrange();
  arrangeUtcTimeLabels();
  emit viewportModeChanged(viewport_.isFollowingLatest());
}

void QtChartsCandlestickView::arrangeUtcTimeLabels() {
  const auto plot = volumeChart_->plotArea();
  const auto visible = viewport_.visibleSlots();
  if (plot.width() < 16.0 || plot.height() < 16.0 ||
      visible.first == visible.end) {
    for (auto *label : utcTimeLabels_) {
      label->hide();
    }
    return;
  }
  const auto labelCount = static_cast<int>(std::min<std::size_t>(
      visible.end - visible.first, plot.width() >= 900.0 ? 5 : 3));
  axisX_->setTickCount(std::max(3, labelCount));
  volumeAxisX_->setTickCount(axisX_->tickCount());
  const auto format =
      dailyTimeframe_ ? QString{"yyyy-MM-dd"} : QString{"yyyy-MM-dd hh:mm"};
  qreal previousLabelRight = -std::numeric_limits<qreal>::infinity();
  for (auto index = 0; index < static_cast<int>(utcTimeLabels_.size());
       ++index) {
    auto *label = utcTimeLabels_.at(static_cast<std::size_t>(index));
    if (index >= labelCount) {
      label->hide();
      continue;
    }
    const auto slot =
        visible.first + (labelCount == 1
                             ? 0
                             : (visible.end - visible.first - 1) *
                                   static_cast<std::size_t>(index) /
                                   static_cast<std::size_t>(labelCount - 1));
    label->setText(
        QDateTime::fromMSecsSinceEpoch(
            static_cast<qint64>(toEpochMilliseconds(bars_.at(slot).ts)),
            QTimeZone::UTC)
            .toString(format) +
        " UTC");
    const auto center =
        volumeChart_->mapToPosition({toSlotCoordinate(slot), 0.0}, volume_).x();
    const auto textWidth = label->boundingRect().width();
    const auto left = std::clamp(center - textWidth / 2.0, plot.left(),
                                 plot.right() - textWidth);
    if (left < previousLabelRight + 8.0) {
      label->hide();
      continue;
    }
    label->setPos(left, plot.bottom() + 5.0);
    label->show();
    previousLabelRight = left + textWidth;
  }
}

void QtChartsCandlestickView::setMarkers(
    const std::span<const ChartMarker> markers) {
  clearMarkers();
  appendMarkers(markers);
}

void QtChartsCandlestickView::appendMarkers(
    const std::span<const ChartMarker> markers) {
  for (const auto &marker : markers) {
    const auto candleTimestamp = static_cast<qint64>(
        toEpochMilliseconds(marker.candleTimestamp.value_or(marker.timestamp)));
    const auto slot = slotByTimestamp_.find(candleTimestamp);
    if (!std::isfinite(marker.price) || marker.price <= 0.0 ||
        slot == slotByTimestamp_.end()) {
      continue;
    }
    const auto barIndex =
        static_cast<std::size_t>(slot->second / slotIntervalMilliseconds);
    const auto &bar = bars_.at(barIndex);
    auto *series = marker.isBuy ? buyMarkers_ : sellMarkers_;
    series->append(slot->second, marker.price);
    markerLayer_->append(marker, slot->second,
                         marker.isBuy ? bar.low : bar.high, *series);
  }
  markerLayer_->arrange();
}

void QtChartsCandlestickView::clearMarkers() {
  markerLayer_->clear();
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
  return static_cast<std::size_t>(buyMarkers_->count()) +
         static_cast<std::size_t>(sellMarkers_->count());
}

ReplayVisibleSlots QtChartsCandlestickView::visibleSlots() const {
  return viewport_.visibleSlots();
}

bool QtChartsCandlestickView::isFollowingLatest() const {
  return viewport_.isFollowingLatest();
}

bool QtChartsCandlestickView::canFitReachedBars() const {
  return viewport_.canFitReachedBars();
}

bool QtChartsCandlestickView::fitReachedBars() {
  if (!viewport_.fitReachedBars()) {
    return false;
  }
  resetAxes();
  arrangeUtcTimeLabels();
  emit viewportModeChanged(true);
  return true;
}

void QtChartsCandlestickView::followLatest() {
  viewport_.followLatest();
  resetAxes();
  arrangeUtcTimeLabels();
  emit viewportModeChanged(true);
}

void QtChartsCandlestickView::zoomAt(const double factor,
                                     const double anchorFraction) {
  viewport_.zoom(factor, anchorFraction);
  resetAxes();
  arrangeUtcTimeLabels();
  emit viewportModeChanged(viewport_.isFollowingLatest());
}

void QtChartsCandlestickView::panByPixels(const double pixelDelta) {
  viewport_.pan(pixelDelta / viewport_.slotWidth());
  resetAxes();
  arrangeUtcTimeLabels();
  emit viewportModeChanged(false);
}

void QtChartsCandlestickView::zoomIn() { zoomAt(1.2, 0.5); }

void QtChartsCandlestickView::zoomOut() { zoomAt(1.0 / 1.2, 0.5); }

void QtChartsCandlestickView::resetZoom() {
  chartView_->resetWheelGesture();
  viewport_.reset();
  resetAxes();
  emit viewportModeChanged(true);
}

void QtChartsCandlestickView::setDailyTimeframe(const bool daily) {
  if (dailyTimeframe_ == daily) {
    return;
  }
  dailyTimeframe_ = daily;
  reservedLabelWidth_ = 0.0;
  chart_->setMargins(QMargins{0, 0, 110, 0});
  volumeChart_->setMargins(QMargins{0, 0, 110, 0});
  axisX_->setFormat(daily ? "yyyy-MM-dd" : "yyyy-MM-dd hh:mm");
  volumeAxisX_->setFormat(axisX_->format());
  resetAxes();
  chart_->layout()->activate();
  volumeChart_->layout()->activate();
}

void QtChartsCandlestickView::resetAxes() {
  candles_->setMaximumColumnWidth(viewport_.slotWidth() * 0.75);
  if (candles_->count() == 0) {
    const auto now = QDateTime::currentDateTimeUtc();
    axisX_->setRange(now.addDays(-1), now);
    axisY_->setRange(0.0, 1.0);
    volumeAxis_->setRange(0.0, 1.0);
  } else {
    const auto visible = viewport_.visibleSlots();
    const auto minTimestamp = viewport_.firstSlot() * slotIntervalMilliseconds;
    const auto maxTimestamp =
        (viewport_.firstSlot() + viewport_.slotCapacity()) *
        slotIntervalMilliseconds;
    qreal minPrice = std::numeric_limits<qreal>::max();
    qreal maxPrice = std::numeric_limits<qreal>::lowest();
    qreal maxVolume = 0.0;

    for (auto index = visible.first; index < visible.end; ++index) {
      const auto &bar = bars_.at(index);
      minPrice = std::min(minPrice, bar.low);
      maxPrice = std::max(maxPrice, bar.high);
      maxVolume = std::max(maxVolume, bar.volume);
    }

    // Reserve a bounded pixel margin for a 16-pixel glyph plus its wick gap.
    // Only visible candle prices determine this display range, never fills.
    const auto priceSpan = maxPrice - minPrice;
    const auto pixelPadding =
        priceSpan * 25.0 / std::max(50.0, chart_->plotArea().height() - 50.0);
    const auto padding = std::max({priceSpan * 0.08, 1.0, pixelPadding});
    axisX_->setRange(QDateTime::fromMSecsSinceEpoch(
                         static_cast<qint64>(minTimestamp), QTimeZone::UTC),
                     QDateTime::fromMSecsSinceEpoch(
                         static_cast<qint64>(maxTimestamp), QTimeZone::UTC));
    axisY_->setRange(minPrice - padding, maxPrice + padding);
    volumeAxis_->setRange(0.0, std::max(1.0, maxVolume * 1.08));
  }
  axisX_->setTickCount(candles_->count() < 3 ? 3 : 5);
  volumeAxisX_->setTickCount(axisX_->tickCount());
  volumeBarLayer_->arrange();
}

void QtChartsCandlestickView::alignVolumePlot() {
  chart_->layout()->activate();
  volumeChart_->layout()->activate();
  const auto pricePlot = chart_->plotArea();
  const auto volumePlot = volumeChart_->plotArea();
  if (pricePlot.width() < 16.0 || volumePlot.height() < 16.0) {
    return;
  }
  // QChart::setPlotArea moves series domains, but Qt's axes still use the
  // automatic layout rectangle. Align through margins so candles, custom
  // volume columns AND rendered time grids share the same geometry.
  auto priceMargins = chart_->margins();
  auto volumeMargins = volumeChart_->margins();
  const auto priceLabelWidth = pricePlot.left() - priceMargins.left();
  const auto volumeLabelWidth = volumePlot.left() - volumeMargins.left();
  // Retain intrinsic label space, not the compensated plot position: the
  // latter feeds rounded margins back into the next update and accumulates
  // blank space as labels change. Retention prevents a large-volume edge bar
  // from repeatedly leaving and reentering the viewport as its labels resize.
  reservedLabelWidth_ =
      std::max({reservedLabelWidth_, priceLabelWidth, volumeLabelWidth});
  priceMargins.setLeft(
      static_cast<int>(std::round(reservedLabelWidth_ - priceLabelWidth)));
  volumeMargins.setLeft(
      static_cast<int>(std::round(reservedLabelWidth_ - volumeLabelWidth)));
  if (chart_->margins() != priceMargins) {
    chart_->setMargins(priceMargins);
  }
  if (volumeChart_->margins() != volumeMargins) {
    volumeChart_->setMargins(volumeMargins);
  }
}

} // namespace bte::frontend
