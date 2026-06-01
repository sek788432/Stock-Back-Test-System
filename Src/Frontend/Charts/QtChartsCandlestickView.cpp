#include "Bte/Frontend/QtChartsCandlestickView.h"

#include <QCandlestickSeries>
#include <QCandlestickSet>
#include <QChart>
#include <QChartView>
#include <QColor>
#include <QDateTime>
#include <QDateTimeAxis>
#include <QLegend>
#include <QMargins>
#include <QPainter>
#include <QTimeZone>
#include <QVBoxLayout>
#include <QValueAxis>

#include <algorithm>
#include <chrono>
#include <limits>

namespace bte::frontend {
namespace {

qreal toEpochMilliseconds(const core::Timestamp timestamp) {
    return static_cast<qreal>(
        std::chrono::duration_cast<std::chrono::milliseconds>(timestamp.time_since_epoch()).count());
}

QCandlestickSet* makeCandleSet(const core::Bar& bar) {
    return new QCandlestickSet(bar.open, bar.high, bar.low, bar.close, toEpochMilliseconds(bar.ts));
}

} // namespace

QtChartsCandlestickView::QtChartsCandlestickView(QWidget* parent)
    : QWidget(parent), chart_(new QChart()), chartView_(new QChartView(chart_, this)),
      candles_(new QCandlestickSeries(chart_)), axisX_(new QDateTimeAxis(chart_)), axisY_(new QValueAxis(chart_)) {
    setObjectName("replayCandlestickChartView");
    setAccessibleName("K-line candlestick chart");

    candles_->setName(tr("OHLC"));
    candles_->setIncreasingColor(QColor{0, 160, 90});
    candles_->setDecreasingColor(QColor{210, 55, 55});

    chart_->addSeries(candles_);
    chart_->legend()->hide();
    chart_->setTitle(tr("Candlestick chart"));
    chart_->setMargins(QMargins{4, 4, 4, 4});
    chart_->addAxis(axisX_, Qt::AlignBottom);
    chart_->addAxis(axisY_, Qt::AlignLeft);
    candles_->attachAxis(axisX_);
    candles_->attachAxis(axisY_);

    axisX_->setFormat("MM-dd");
    axisX_->setTitleText(tr("Time"));
    axisY_->setTitleText(tr("Price"));
    axisY_->setLabelFormat("%.2f");

    chartView_->setObjectName("replayChartView");
    chartView_->setAccessibleName("K-line chart viewport");
    chartView_->setRenderHint(QPainter::Antialiasing);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(chartView_);

    resetAxes();
}

void QtChartsCandlestickView::setBarWindow(const std::span<const core::Bar> visible) {
    candles_->clear();
    for (const auto& bar : visible) {
        appendBar(bar);
    }
    resetAxes();
}

void QtChartsCandlestickView::appendBar(const core::Bar& bar) {
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

        for (auto* set : candles_->sets()) {
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
        axisX_->setRange(QDateTime::fromMSecsSinceEpoch(static_cast<qint64>(minTimestamp), QTimeZone::UTC),
                         QDateTime::fromMSecsSinceEpoch(static_cast<qint64>(maxTimestamp), QTimeZone::UTC));
        axisY_->setRange(std::max(0.0, minPrice - padding), maxPrice + padding);
    }
}

} // namespace bte::frontend
