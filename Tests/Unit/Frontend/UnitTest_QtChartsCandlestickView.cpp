#include "Bte/Frontend/QtChartsCandlestickView.h"

#include "WaitUntil.h"

#include <QCandlestickSeries>
#include <QCandlestickSet>
#include <QChart>
#include <QChartView>
#include <QCoreApplication>
#include <QDateTime>
#include <QDateTimeAxis>
#include <QElapsedTimer>
#include <QEvent>
#include <QGraphicsItem>
#include <QGraphicsLineItem>
#include <QGraphicsPixmapItem>
#include <QGraphicsRectItem>
#include <QGraphicsScene>
#include <QGraphicsSimpleTextItem>
#include <QImage>
#include <QLayout>
#include <QLineSeries>
#include <QMouseEvent>
#include <QPainter>
#include <QPoint>
#include <QScatterSeries>
#include <QSignalSpy>
#include <QTest>
#include <QTimeZone>
#include <QValueAxis>
#include <QWheelEvent>
#include <QtGlobal>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <iterator>
#include <limits>
#include <span>
#include <vector>

namespace {

bte::core::Timestamp makeTimestamp(const int day) {
  return bte::core::Timestamp{
      std::chrono::sys_days{std::chrono::year{2024} / 1 / day}};
}

bte::core::Bar makeBar(const int day, const double open, const double close) {
  return {
      .ts = makeTimestamp(day),
      .open = open,
      .high = std::max(open, close) + 2.0,
      .low = std::min(open, close) - 2.0,
      .close = close,
      .volume = 1'000'000.0,
  };
}

QChartView *chartViewport(bte::frontend::QtChartsCandlestickView &view) {
  auto *result = view.findChild<QChartView *>("replayChartView");
  Q_ASSERT(result != nullptr);
  return result;
}

int renderedCandleBodyPixels(QChartView &price, QCandlestickSeries &series,
                             const QCandlestickSet &candle) {
  const auto center = price.chart()->mapToPosition(
      {candle.timestamp(), (candle.open() + candle.close()) / 2.0}, &series);
  QImage rendered{price.size(), QImage::Format_ARGB32_Premultiplied};
  rendered.fill(Qt::transparent);
  QPainter painter{&rendered};
  price.render(&painter);
  painter.end();

  const auto centerX = static_cast<int>(std::round(center.x()));
  const auto centerY = static_cast<int>(std::round(center.y()));
  const auto isBodyPixel = [&](const int x) {
    if (x < 0 || x >= rendered.width() || centerY < 0 ||
        centerY >= rendered.height()) {
      return false;
    }
    const auto color = rendered.pixelColor(x, centerY);
    return color.green() > 140 && color.green() > color.red() + 40 &&
           color.green() > color.blue() + 20;
  };
  auto bodyPixels = isBodyPixel(centerX) ? 1 : 0;
  for (auto x = centerX - 1; isBodyPixel(x); --x) {
    ++bodyPixels;
  }
  for (auto x = centerX + 1; isBodyPixel(x); ++x) {
    ++bodyPixels;
  }
  return bodyPixels;
}

int visibleCrosshairCount(const QChartView &chartView) {
  return static_cast<int>(std::ranges::count_if(
      chartView.scene()->items(), [](const QGraphicsItem *item) {
        return item->type() == QGraphicsLineItem::Type &&
               item->zValue() == 20.0 && item->isVisible();
      }));
}

class QtChartsCandlestickViewTest final : public QObject {
  Q_OBJECT

private slots:
  void emptyWindowRendersSafely();
  void tinyLayoutHidesPreviouslyVisibleVolumeBarsSafely();
  void setBarWindowReplacesCandles();
  void literalBarsPreserveEveryOhlcvFieldAndTimestamp();
  void longRangeDoesNotTruncateCandles();
  void missingHoursKeepObservedCandlesEvenlySpaced();
  void axesScaleOnlyVisiblePriceAndVolume();
  void flatPriceAndZeroVolumeKeepFiniteIndependentScales();
  void priceAndVolumeFormOneContinuousSurface();
  void denseHourlyUtcLabelsDoNotOverlap();
  void resizeRecomputesVisibleSlotsWithoutLosingTail();
  void paneResizeOrderKeepsTimeGeometryAligned();
  void plotResizeDuringRangeUpdateDoesNotReenterAxes();
  void volumePlotAlignmentWaitsForLayoutCompletion();
  void renderedVolumeTimeGridMatchesPriceSlots();
  void extremeVolumeAtVisibleBoundarySettlesLayout();
  void progressivePlaybackDoesNotAccumulateLeftMargins();
  void dragReviewsReachedHistoryUntilFollowLatest();
  void fitReachedBarsShowsSmallPrefixAndKeepsFullCounts();
  void appendBarAddsOneCandle();
  void appendBarsAddsAPlaybackBatch();
  void batchVolumeUpdateAvoidsPerPointGeometryRebuilds();
  void invalidBarIsIgnoredAndMarkersCanBeCleared();
  void rendersSynchronizedVolumeAndTradeMarkers();
  void appendingMarkersKeepsEarlierFillsWithoutRebuildingThem();
  void tradeGlyphsAreAdjacentToTheirContainingCandles();
  void tradeGlyphClearanceSurvivesHeightOnlyResizeAtPriceExtrema();
  void onlyCoincidentSellFillsShowLeaders();
  void coincidentBuysShareOneGlyphWithoutChangingFinancialPoints();
  void buyGroupingTracksReachedFillsAndDisplayedTimeframe();
  void markersOutsideTheZoomedViewportAreHiddenWithoutChangingFinancialPoints();
  void invalidAndOutOfWindowMarkersAreRejectedAndReplacementClearsThem();
  void zoomControlsAreSafeToCall();
  void pointerInteractionPansAndTogglesCrosshair();
  void wheelInteractionZoomsInAndOut();
  void wheelZoomKeepsPriceAndVolumeAxesNearVisibleCandles();
  void oneBarHourlyTicksIncludeUtcHour();
  void sparseAndLongTailCandlesRenderWithReadableBodies();
  void hourlyAxisLabelsUseUtcTimestampRatherThanLocalTime();
  void volumeBarsOccupyAlignedLowerPaneWithoutChangingPriceScale();
  void resizingKeepsPriceAndVolumeTimestampsAligned();
  void dailyTimeframeUsesDateTicksAndKeepsVolumeAligned();
};

void QtChartsCandlestickViewTest::oneBarHourlyTicksIncludeUtcHour() {
  bte::frontend::QtChartsCandlestickView view;
  view.resize(900, 600);
  view.setBarWindow(std::array{makeBar(2, 100.0, 104.0)});
  view.show();
  QVERIFY(QTest::qWaitForWindowExposed(&view));
  QCoreApplication::processEvents();

  auto *price = chartViewport(view);
  const auto series = price->chart()->series();
  const auto candle = std::ranges::find_if(series, [](const auto *entry) {
    return qobject_cast<const QCandlestickSeries *>(entry) != nullptr;
  });
  QVERIFY(candle != series.end());
  const auto *candles = qobject_cast<const QCandlestickSeries *>(*candle);
  QVERIFY(candles != nullptr);
  const auto *timeAxis = qobject_cast<QDateTimeAxis *>(
      price->chart()->axes(Qt::Horizontal).front());
  QVERIFY(timeAxis != nullptr);
  QVERIFY(timeAxis->format().contains("hh"));
  const auto *volume = view.findChild<QChartView *>("replayVolumeChartView");
  QVERIFY(volume != nullptr);
  const auto *volumeTime = qobject_cast<QDateTimeAxis *>(
      volume->chart()->axes(Qt::Horizontal).front());
  QVERIFY(volumeTime != nullptr);
  QVERIFY(volumeTime->titleText().isEmpty());
}

void QtChartsCandlestickViewTest::
    sparseAndLongTailCandlesRenderWithReadableBodies() {
  std::vector<bte::core::Bar> bars;
  bars.reserve(200);
  for (auto index = 0; index < 200; ++index) {
    auto bar = makeBar(2, 100.0, 104.0);
    bar.ts += std::chrono::hours{index};
    bars.push_back(bar);
  }

  for (const auto width : {1024, 1200, 1440}) {
    for (const auto reached : {std::size_t{1}, std::size_t{200}}) {
      bte::frontend::QtChartsCandlestickView view;
      view.resize(width, 700);
      view.setBarWindow(std::span<const bte::core::Bar>{bars}.first(reached));
      view.show();
      QVERIFY(QTest::qWaitForWindowExposed(&view));
      QCoreApplication::processEvents();

      auto *price = chartViewport(view);
      auto *series =
          qobject_cast<QCandlestickSeries *>(price->chart()->series().front());
      QVERIFY(series != nullptr);
      const auto *last = series->sets().at(static_cast<qsizetype>(reached - 1));
      const auto bodyPixels = renderedCandleBodyPixels(*price, *series, *last);
      QVERIFY2(bodyPixels >= 16 && bodyPixels <= 20,
               qPrintable(QString{"Width %1, reached %2: candle body occupies "
                                  "%3 px"}
                              .arg(width)
                              .arg(reached)
                              .arg(bodyPixels)));
      if (reached == 1) {
        view.zoomIn();
        QCoreApplication::processEvents();
        QVERIFY(renderedCandleBodyPixels(*price, *series, *last) > bodyPixels);
      }
    }
  }
}

void QtChartsCandlestickViewTest::
    hourlyAxisLabelsUseUtcTimestampRatherThanLocalTime() {
  bte::frontend::QtChartsCandlestickView view;
  view.resize(900, 600);
  view.setBarWindow(std::array{makeBar(2, 100.0, 104.0)});
  view.show();
  QVERIFY(QTest::qWaitForWindowExposed(&view));
  QCoreApplication::processEvents();

  const auto *volume = view.findChild<QChartView *>("replayVolumeChartView");
  QVERIFY(volume != nullptr);
  const auto *axis = qobject_cast<QDateTimeAxis *>(
      volume->chart()->axes(Qt::Horizontal).front());
  QVERIFY(axis != nullptr);
  QVERIFY(!axis->labelsVisible());
  auto foundUtcMidnight = false;
  for (const auto *item : volume->scene()->items()) {
    if (item->data(0).toString() != "replayUtcTimeLabel" ||
        !item->isVisible()) {
      continue;
    }
    const auto *label = dynamic_cast<const QGraphicsSimpleTextItem *>(item);
    QVERIFY(label != nullptr);
    foundUtcMidnight |= label->text() == "2024-01-02 00:00 UTC";
  }
  QVERIFY(foundUtcMidnight);
}

void QtChartsCandlestickViewTest::
    volumeBarsOccupyAlignedLowerPaneWithoutChangingPriceScale() {
  bte::frontend::QtChartsCandlestickView view;
  view.resize(900, 600);
  auto first = makeBar(2, 100.0, 104.0);
  auto second = makeBar(3, 104.0, 101.0);
  first.volume = 1'000.0;
  second.volume = 1'000'000'000.0;
  view.setBarWindow(std::array{first, second});
  view.show();
  QVERIFY(QTest::qWaitForWindowExposed(&view));
  QCoreApplication::processEvents();

  const auto *price = chartViewport(view);
  const auto *volume = view.findChild<QChartView *>("replayVolumeChartView");
  QVERIFY(volume != nullptr);
  QVERIFY(price->geometry().bottom() < volume->geometry().top());
  QVERIFY(bte::test::waitUntil([&] {
    return (std::abs(price->chart()->plotArea().left() -
                     volume->chart()->plotArea().left()) <= 2.0 &&
            std::abs(price->chart()->plotArea().right() -
                     volume->chart()->plotArea().right()) <= 2.0);
  }));
  QCOMPARE(price->chart()->axes(Qt::Vertical).size(), 1);
  const auto *priceAxis =
      qobject_cast<QValueAxis *>(price->chart()->axes(Qt::Vertical).front());
  QVERIFY(priceAxis != nullptr);
  QVERIFY(priceAxis->max() < 120.0);
  const auto *volumeAxis =
      qobject_cast<QValueAxis *>(volume->chart()->axes(Qt::Vertical).front());
  QVERIFY(volumeAxis != nullptr);
  QVERIFY(volumeAxis->max() >= second.volume);
  QCOMPARE(volumeAxis->labelFormat(), QString{"%.0f"});
  const auto *priceTime = qobject_cast<QDateTimeAxis *>(
      price->chart()->axes(Qt::Horizontal).front());
  const auto *volumeTime = qobject_cast<QDateTimeAxis *>(
      volume->chart()->axes(Qt::Horizontal).front());
  QVERIFY(priceTime != nullptr);
  QVERIFY(volumeTime != nullptr);
  QCOMPARE(priceTime->min(), volumeTime->min());
  QCOMPARE(priceTime->max(), volumeTime->max());
  auto *candleSeries =
      qobject_cast<QCandlestickSeries *>(price->chart()->series().front());
  auto *volumeSeries =
      qobject_cast<QLineSeries *>(volume->chart()->series().front());
  QVERIFY(candleSeries != nullptr);
  QVERIFY(volumeSeries != nullptr);
  const auto candleTime = candleSeries->sets().front()->timestamp();
  const auto priceX =
      price->chart()
          ->mapToPosition({candleTime, first.close}, candleSeries)
          .x();
  const auto volumeX =
      volume->chart()
          ->mapToPosition({candleTime, first.volume}, volumeSeries)
          .x();
  QVERIFY(std::abs(priceX - volumeX) <= 2.0);
  auto barCount = 0;
  for (const auto *item : volume->scene()->items()) {
    if (item->data(0) == "replayVolumeBar" && item->isVisible()) {
      ++barCount;
    }
  }
  QCOMPARE(barCount, 2);
}

void QtChartsCandlestickViewTest::
    resizingKeepsPriceAndVolumeTimestampsAligned() {
  bte::frontend::QtChartsCandlestickView view;
  view.resize(900, 600);
  const std::array bars{makeBar(2, 100.0, 104.0), makeBar(3, 104.0, 101.0)};
  view.setBarWindow(bars);
  view.show();
  QVERIFY(QTest::qWaitForWindowExposed(&view));
  QCoreApplication::processEvents();

  for (const auto width : {900, 620, 1100, 3400}) {
    view.resize(width, 600);
    QCoreApplication::processEvents();
    const auto *price = chartViewport(view);
    const auto *volume = view.findChild<QChartView *>("replayVolumeChartView");
    QVERIFY(volume != nullptr);
    const auto *priceTime = qobject_cast<QDateTimeAxis *>(
        price->chart()->axes(Qt::Horizontal).front());
    const auto *volumeTime = qobject_cast<QDateTimeAxis *>(
        volume->chart()->axes(Qt::Horizontal).front());
    QVERIFY(priceTime != nullptr);
    QVERIFY(volumeTime != nullptr);
    QVERIFY(bte::test::waitUntil([&] {
      return (std::abs(price->chart()->plotArea().right() -
                       volume->chart()->plotArea().right()) <= 2.0 &&
              std::abs(price->chart()->plotArea().left() -
                       volume->chart()->plotArea().left()) <= 2.0);
    }));
    QVERIFY2(price->chart()->plotArea().right() >= price->width() - 160.0,
             qPrintable(QString{"requested=%1 view=%2 price=%3 plot=%4"}
                            .arg(width)
                            .arg(view.width())
                            .arg(price->width())
                            .arg(price->chart()->plotArea().right())));
    QCOMPARE(priceTime->min(), volumeTime->min());
    QCOMPARE(priceTime->max(), volumeTime->max());
    auto *candles =
        qobject_cast<QCandlestickSeries *>(price->chart()->series().front());
    auto *shares =
        qobject_cast<QLineSeries *>(volume->chart()->series().front());
    QVERIFY(candles != nullptr);
    QVERIFY(shares != nullptr);
    for (const auto *bar : candles->sets()) {
      const auto priceX =
          price->chart()
              ->mapToPosition({bar->timestamp(), bar->close()}, candles)
              .x();
      const auto volumeX =
          volume->chart()->mapToPosition({bar->timestamp(), 0.0}, shares).x();
      QVERIFY(std::abs(priceX - volumeX) <= 2.0);
    }
  }
}

void QtChartsCandlestickViewTest::
    dailyTimeframeUsesDateTicksAndKeepsVolumeAligned() {
  bte::frontend::QtChartsCandlestickView view;
  view.resize(900, 600);
  view.setDailyTimeframe(true);
  view.setBarWindow(
      std::array{makeBar(2, 100.0, 104.0), makeBar(3, 104.0, 101.0)});
  view.show();
  QVERIFY(QTest::qWaitForWindowExposed(&view));
  QCoreApplication::processEvents();
  auto *price = chartViewport(view);
  auto *volume = view.findChild<QChartView *>("replayVolumeChartView");
  QVERIFY(volume != nullptr);
  auto *priceTime = qobject_cast<QDateTimeAxis *>(
      price->chart()->axes(Qt::Horizontal).front());
  auto *volumeTime = qobject_cast<QDateTimeAxis *>(
      volume->chart()->axes(Qt::Horizontal).front());
  QVERIFY(priceTime != nullptr);
  QVERIFY(volumeTime != nullptr);
  QVERIFY(!volumeTime->format().contains("hh"));
  QCOMPARE(priceTime->min(), volumeTime->min());
  QCOMPARE(priceTime->max(), volumeTime->max());
  view.zoomIn();
  QCOMPARE(priceTime->min(), volumeTime->min());
  QCOMPARE(priceTime->max(), volumeTime->max());
  view.zoomOut();
  QCOMPARE(priceTime->min(), volumeTime->min());
  QCOMPARE(priceTime->max(), volumeTime->max());
}

void QtChartsCandlestickViewTest::emptyWindowRendersSafely() {
  bte::frontend::QtChartsCandlestickView view;

  view.setBarWindow({});

  QCOMPARE(view.candleCount(), 0U);
  QCOMPARE(view.volumePointCount(), 0U);
}

void QtChartsCandlestickViewTest::
    tinyLayoutHidesPreviouslyVisibleVolumeBarsSafely() {
  bte::frontend::QtChartsCandlestickView view;
  view.resize(900, 600);
  view.setBarWindow(
      std::array{makeBar(2, 100.0, 104.0), makeBar(3, 104.0, 101.0)});
  view.show();
  QVERIFY(QTest::qWaitForWindowExposed(&view));
  QCoreApplication::processEvents();

  auto *volume = view.findChild<QChartView *>("replayVolumeChartView");
  auto *price = view.findChild<QChartView *>("replayChartView");
  QVERIFY(volume != nullptr);
  QVERIFY(price != nullptr);
  const auto visibleVolumeBarCount = [volume] {
    return std::ranges::count_if(
        volume->scene()->items(), [](const auto *item) {
          return item->data(0).toString() == "replayVolumeBar" &&
                 item->isVisible();
        });
  };
  QVERIFY(visibleVolumeBarCount() > 0);

  price->chart()->setPlotArea(QRectF{0.0, 0.0, 1.0, 1.0});
  volume->chart()->setPlotArea(QRectF{0.0, 0.0, 1.0, 1.0});
  QCoreApplication::processEvents();

  QVERIFY(price->chart()->plotArea().width() < 16.0 ||
          volume->chart()->plotArea().height() < 16.0);
  QCOMPARE(visibleVolumeBarCount(), 0);
}

void QtChartsCandlestickViewTest::setBarWindowReplacesCandles() {
  bte::frontend::QtChartsCandlestickView view;
  const std::array bars{
      makeBar(2, 100.0, 104.0),
      makeBar(3, 104.0, 101.0),
      makeBar(4, 101.0, 108.0),
  };
  const std::array replacement{
      makeBar(5, 108.0, 109.0),
  };

  view.setBarWindow(bars);
  QCOMPARE(view.candleCount(), 3U);

  view.setBarWindow(replacement);
  QCOMPARE(view.candleCount(), 1U);
}

void QtChartsCandlestickViewTest::
    literalBarsPreserveEveryOhlcvFieldAndTimestamp() {
  const auto start = makeTimestamp(2);
  const std::array bars{bte::core::Bar{.ts = start + std::chrono::hours{23},
                                       .open = 101.25,
                                       .high = 109.75,
                                       .low = 99.5,
                                       .close = 107.125,
                                       .volume = 1234.0},
                        bte::core::Bar{.ts = start + std::chrono::hours{25},
                                       .open = 108.0,
                                       .high = 110.5,
                                       .low = 102.25,
                                       .close = 103.75,
                                       .volume = 4567.0},
                        bte::core::Bar{.ts = start + std::chrono::hours{49},
                                       .open = 103.75,
                                       .high = 105.5,
                                       .low = 95.25,
                                       .close = 96.5,
                                       .volume = 8910.0}};
  bte::frontend::QtChartsCandlestickView view;
  view.setBarWindow(bars);

  const auto *price = chartViewport(view);
  const auto *volume = view.findChild<QChartView *>("replayVolumeChartView");
  QVERIFY(volume != nullptr);
  const auto *candles =
      qobject_cast<QCandlestickSeries *>(price->chart()->series().front());
  const auto *shares =
      qobject_cast<QLineSeries *>(volume->chart()->series().front());
  QVERIFY(candles != nullptr);
  QVERIFY(shares != nullptr);
  QCOMPARE(candles->count(), static_cast<qsizetype>(bars.size()));
  QCOMPARE(shares->count(), static_cast<qsizetype>(bars.size()));
  for (auto index = std::size_t{0}; index < bars.size(); ++index) {
    const auto *candle = candles->sets().at(static_cast<qsizetype>(index));
    const auto point = shares->at(static_cast<int>(index));
    const auto timestamp = static_cast<qreal>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            bars[index].ts.time_since_epoch())
            .count());
    const auto slot = static_cast<qreal>(index) * 3'600'000.0;
    QCOMPARE(candle->timestamp(), slot);
    QCOMPARE(candle->property("replayUtcTimestamp").toDouble(), timestamp);
    QCOMPARE(candle->open(), bars[index].open);
    QCOMPARE(candle->high(), bars[index].high);
    QCOMPARE(candle->low(), bars[index].low);
    QCOMPARE(candle->close(), bars[index].close);
    QCOMPARE(point.x(), slot);
    QCOMPARE(point.y(), bars[index].volume);
  }
}

void QtChartsCandlestickViewTest::
    extremeVolumeAtVisibleBoundarySettlesLayout() {
  bte::frontend::QtChartsCandlestickView view;
  view.resize(900, 600);
  view.show();
  QVERIFY(QTest::qWaitForWindowExposed(&view));
  std::vector<bte::core::Bar> bars;
  for (auto index = 0; index < 100; ++index) {
    auto bar = makeBar(1, 100.0, 101.0);
    bar.ts += std::chrono::hours{index};
    bar.volume = 1.0;
    bars.push_back(bar);
  }
  view.setBarWindow(bars);
  for (auto iteration = 0; iteration < 10; ++iteration) {
    QCoreApplication::processEvents();
  }
  const auto boundary = view.visibleSlots().first;
  QVERIFY(boundary > 0 && boundary + 1 < bars.size());
  auto *price = chartViewport(view);
  auto *volume = view.findChild<QChartView *>("replayVolumeChartView");
  QVERIFY(volume != nullptr);
  const auto ordinaryLeft = price->chart()->plotArea().left();
  auto *volumeAxis = volume->chart()->findChild<QValueAxis *>();
  QVERIFY(volumeAxis != nullptr);
  // Exercise the bars at and immediately around the visibility boundary:
  // changing label width must not alternately exclude and reinclude them.
  for (const auto index : {boundary - 1, boundary, boundary + 1}) {
    view.setBarWindow(bars);
    for (auto iteration = 0; iteration < 20; ++iteration) {
      QCoreApplication::processEvents();
    }
    QVERIFY(bte::test::waitUntil([&] {
      return (std::abs(price->chart()->plotArea().left() - ordinaryLeft) <=
              2.0);
    }));
    bars.at(index).volume = 1.0e18;
    view.setBarWindow(bars);
    const auto initiallyVisible = view.visibleSlots();
    QCOMPARE(index >= initiallyVisible.first && index < initiallyVisible.end,
             index >= boundary);
    if (index >= initiallyVisible.first && index < initiallyVisible.end) {
      QVERIFY(volumeAxis->max() >= bars.at(index).volume);
    } else {
      QVERIFY(volumeAxis->max() < 2.0);
    }
    for (auto iteration = 0; iteration < 20; ++iteration) {
      QCoreApplication::processEvents();
    }
    const auto settledPrice = price->chart()->plotArea();
    const auto settledVolume = volume->chart()->plotArea();
    const auto settledFirst = view.visibleSlots().first;
    const auto settledEnd = view.visibleSlots().end;
    if (index >= settledFirst && index < settledEnd) {
      QVERIFY(volumeAxis->max() >= bars.at(index).volume);
      QVERIFY(settledPrice.left() > ordinaryLeft);
    } else {
      QVERIFY(volumeAxis->max() < 2.0);
    }
    QSignalSpy priceChanges{price->chart(), &QChart::plotAreaChanged};
    QSignalSpy volumeChanges{volume->chart(), &QChart::plotAreaChanged};
    for (auto iteration = 0; iteration < 20; ++iteration) {
      QCoreApplication::processEvents();
    }
    QCOMPARE(priceChanges.size(), 0);
    QCOMPARE(volumeChanges.size(), 0);
    QCOMPARE(view.visibleSlots().first, settledFirst);
    QCOMPARE(price->chart()->plotArea(), settledPrice);
    QCOMPARE(volume->chart()->plotArea(), settledVolume);
    QVERIFY(std::abs(settledPrice.left() - settledVolume.left()) <= 2.0);
    QCOMPARE(view.candleCount(), bars.size());
    bars.at(index).volume = 1.0;
  }
  view.setBarWindow(bars);
  QVERIFY(bte::test::waitUntil([&] {
    return (std::abs(price->chart()->plotArea().left() - ordinaryLeft) <= 2.0);
  }));
}

void QtChartsCandlestickViewTest::
    progressivePlaybackDoesNotAccumulateLeftMargins() {
  bte::frontend::QtChartsCandlestickView view;
  view.resize(1200, 700);
  view.show();
  QVERIFY(QTest::qWaitForWindowExposed(&view));
  auto *price = chartViewport(view)->chart();
  auto *volume = view.findChild<QChartView *>("replayVolumeChartView");
  QVERIFY(volume != nullptr);
  auto bar = makeBar(1, 100.0, 101.0);
  bar.volume = 1'000.0;
  view.setBarWindow(std::array{bar});
  for (auto pass = 0; pass < 20; ++pass) {
    QCoreApplication::processEvents();
  }
  const auto initialLeft = price->plotArea().left();
  for (auto index = 1; index <= 500; ++index) {
    bar.ts += std::chrono::hours{1};
    bar.open = 100.0 + static_cast<double>(index % 31);
    bar.close = bar.open + 1.0;
    bar.low = bar.open - 2.0;
    bar.high = bar.open + 3.0;
    bar.volume = 1'234'567.0 + static_cast<double>(index) * 7'919.0;
    view.appendBar(bar);
    for (auto pass = 0; pass < 4; ++pass) {
      QCoreApplication::processEvents();
    }
    QVERIFY2(
        price->plotArea().left() < initialLeft + 20.0,
        "Changing ordinary axis labels must not accumulate blank left margins");
  }
  QCOMPARE(view.candleCount(), std::size_t{501});
  QCOMPARE(view.visibleSlots().end, std::size_t{501});
  QVERIFY(price->plotArea().width() > 800.0);
  QVERIFY(std::abs(price->plotArea().left() -
                   volume->chart()->plotArea().left()) <= 2.0);
  for (auto pass = 0; pass < 20; ++pass) {
    QCoreApplication::processEvents();
  }
  const auto settledPrice = price->plotArea();
  const auto settledVolume = volume->chart()->plotArea();
  QSignalSpy priceChanges{price, &QChart::plotAreaChanged};
  QSignalSpy volumeChanges{volume->chart(), &QChart::plotAreaChanged};
  for (auto pass = 0; pass < 20; ++pass) {
    QCoreApplication::processEvents();
  }
  QCOMPARE(priceChanges.size(), 0);
  QCOMPARE(volumeChanges.size(), 0);
  QCOMPARE(price->plotArea(), settledPrice);
  QCOMPARE(volume->chart()->plotArea(), settledVolume);
}

void QtChartsCandlestickViewTest::longRangeDoesNotTruncateCandles() {
  constexpr auto candleCount = 23'777;
  std::vector<bte::core::Bar> bars;
  bars.reserve(candleCount);
  const auto start = makeTimestamp(1);
  for (auto index = 0; index < candleCount; ++index) {
    const auto open = 100.0 + static_cast<double>(index % 17);
    bars.push_back({.ts = start + std::chrono::hours{index},
                    .open = open,
                    .high = open + 2.0,
                    .low = open - 1.0,
                    .close = open + 1.0,
                    .volume = 1'000'000.0 + index});
  }
  bte::frontend::QtChartsCandlestickView view;
  view.resize(900, 600);
  view.show();
  QVERIFY(QTest::qWaitForWindowExposed(&view));
  QCoreApplication::processEvents();
  QElapsedTimer timer;
  timer.start();

  view.setBarWindow(bars);

  qInfo("23,777-candle chart load: %lld ms", timer.elapsed());
  QCOMPARE(view.candleCount(), static_cast<std::size_t>(candleCount));
  QCOMPARE(view.volumePointCount(), static_cast<std::size_t>(candleCount));
  const auto visible = view.visibleSlots();
  QCOMPARE(visible.end, static_cast<std::size_t>(candleCount));
  QVERIFY(visible.first > static_cast<std::size_t>(candleCount - 150));
  view.show();
  QVERIFY(QTest::qWaitForWindowExposed(&view));
  QCoreApplication::processEvents();
  auto *price = chartViewport(view);
  auto *series =
      qobject_cast<QCandlestickSeries *>(price->chart()->series().front());
  QVERIFY(series != nullptr);
  const auto sets = series->sets();
  const auto *penultimate = sets.at(candleCount - 2);
  const auto *last = sets.at(candleCount - 1);
  const auto penultimateX =
      price->chart()
          ->mapToPosition({penultimate->timestamp(), penultimate->close()},
                          series)
          .x();
  const auto lastX =
      price->chart()
          ->mapToPosition({last->timestamp(), last->close()}, series)
          .x();
  QVERIFY2(lastX - penultimateX >= 6.0,
           "The last two reached candles must remain separately readable");
  QVERIFY2(bte::test::waitUntil([&] {
             return (renderedCandleBodyPixels(*price, *series, *last) >= 16 &&
                     renderedCandleBodyPixels(*price, *series, *last) <= 20);
           }),
           "The long-range tail must retain a readable candle body");

  std::vector<bte::frontend::ChartMarker> markers;
  markers.reserve(5'261);
  for (auto index = 0; index < 5'261; ++index) {
    const auto &bar = bars.at(static_cast<std::size_t>(index * 4));
    markers.push_back({.timestamp = bar.ts,
                       .price = bar.close,
                       .isBuy = index % 2 == 0,
                       .candleTimestamp = bar.ts});
  }
  timer.restart();
  view.setMarkers(markers);
  qInfo("5,261-marker chart load: %lld ms", timer.elapsed());
  QCOMPARE(view.markerCount(), markers.size());
}

void QtChartsCandlestickViewTest::
    batchVolumeUpdateAvoidsPerPointGeometryRebuilds() {
  bte::frontend::QtChartsCandlestickView view;
  view.resize(900, 600);
  view.show();
  QVERIFY(QTest::qWaitForWindowExposed(&view));
  view.setBarWindow(std::array{makeBar(2, 100.0, 104.0)});
  auto *volume = view.findChild<QChartView *>("replayVolumeChartView");
  QVERIFY(volume != nullptr);
  auto *shares = qobject_cast<QLineSeries *>(volume->chart()->series().front());
  QVERIFY(shares != nullptr);
  QSignalSpy pointAdded{shares, &QLineSeries::pointAdded};
  QSignalSpy pointsReplaced{shares, &QLineSeries::pointsReplaced};
  std::vector<bte::core::Bar> bars;
  for (auto index = 1; index <= 200; ++index) {
    auto bar = makeBar(2, 100.0, 104.0);
    bar.ts += std::chrono::hours{index};
    bar.volume += index;
    bars.push_back(bar);
  }
  view.appendBars(bars);
  QCOMPARE(pointAdded.size(), 0);
  QCOMPARE(pointsReplaced.size(), 1);
  QCOMPARE(view.volumePointCount(), 201U);
  QCOMPARE(shares->points().back().y(), bars.back().volume);
}

void QtChartsCandlestickViewTest::
    missingHoursKeepObservedCandlesEvenlySpaced() {
  bte::frontend::QtChartsCandlestickView view;
  view.resize(900, 600);
  auto first = makeBar(2, 100.0, 104.0);
  auto second = makeBar(2, 104.0, 105.0);
  second.ts += std::chrono::hours{1};
  auto third = makeBar(3, 105.0, 101.0);
  view.setBarWindow(std::array{first, second, third});
  view.show();
  QVERIFY(QTest::qWaitForWindowExposed(&view));
  QCoreApplication::processEvents();

  auto *price = chartViewport(view);
  auto *series =
      qobject_cast<QCandlestickSeries *>(price->chart()->series().front());
  QVERIFY(series != nullptr);
  const auto sets = series->sets();
  const auto firstX =
      price->chart()
          ->mapToPosition({sets.at(0)->timestamp(), sets.at(0)->close()},
                          series)
          .x();
  const auto secondX =
      price->chart()
          ->mapToPosition({sets.at(1)->timestamp(), sets.at(1)->close()},
                          series)
          .x();
  const auto thirdX =
      price->chart()
          ->mapToPosition({sets.at(2)->timestamp(), sets.at(2)->close()},
                          series)
          .x();
  QVERIFY(firstX < secondX && secondX < thirdX);
  QVERIFY(std::abs((secondX - firstX) - (thirdX - secondX)) <= 2.0);
}

void QtChartsCandlestickViewTest::axesScaleOnlyVisiblePriceAndVolume() {
  std::vector<bte::core::Bar> bars;
  bars.reserve(200);
  for (auto index = 0; index < 200; ++index) {
    const auto ordinary = index >= 100;
    const auto price = ordinary ? 100.0 : 10'000.0;
    bars.push_back({.ts = makeTimestamp(2) + std::chrono::hours{index},
                    .open = price,
                    .high = price + 2.0,
                    .low = price - 2.0,
                    .close = price + 1.0,
                    .volume = ordinary ? 100.0 : 1'000'000.0});
  }
  bte::frontend::QtChartsCandlestickView view;
  view.resize(900, 600);
  view.setBarWindow(bars);
  view.show();
  QVERIFY(QTest::qWaitForWindowExposed(&view));
  QCoreApplication::processEvents();

  auto *priceAxis = qobject_cast<QValueAxis *>(
      chartViewport(view)->chart()->axes(Qt::Vertical).front());
  auto *volumeView = view.findChild<QChartView *>("replayVolumeChartView");
  QVERIFY(volumeView != nullptr);
  auto *volumeAxis = qobject_cast<QValueAxis *>(
      volumeView->chart()->axes(Qt::Vertical).front());
  QVERIFY(priceAxis != nullptr);
  QVERIFY(volumeAxis != nullptr);
  QCOMPARE(view.visibleSlots().end, bars.size());
  QVERIFY(view.visibleSlots().first >= 100);
  QVERIFY(priceAxis->max() < 120.0);
  QVERIFY(priceAxis->min() > 80.0);
  QVERIFY(volumeAxis->max() < 120.0);
  QCOMPARE(volumeAxis->min(), 0.0);
}

void QtChartsCandlestickViewTest::
    flatPriceAndZeroVolumeKeepFiniteIndependentScales() {
  bte::frontend::QtChartsCandlestickView view;
  view.resize(900, 600);
  const std::array bars{bte::core::Bar{.ts = makeTimestamp(2),
                                       .open = 10'000.0,
                                       .high = 10'000.0,
                                       .low = 10'000.0,
                                       .close = 10'000.0,
                                       .volume = 0.0}};
  view.setBarWindow(bars);
  view.show();
  QVERIFY(QTest::qWaitForWindowExposed(&view));
  QCoreApplication::processEvents();
  auto *priceAxis = qobject_cast<QValueAxis *>(
      chartViewport(view)->chart()->axes(Qt::Vertical).front());
  auto *volumeView = view.findChild<QChartView *>("replayVolumeChartView");
  QVERIFY(volumeView != nullptr);
  auto *volumeAxis = qobject_cast<QValueAxis *>(
      volumeView->chart()->axes(Qt::Vertical).front());
  QVERIFY(priceAxis != nullptr);
  QVERIFY(volumeAxis != nullptr);
  QVERIFY(std::isfinite(priceAxis->min()));
  QVERIFY(std::isfinite(priceAxis->max()));
  QVERIFY(priceAxis->min() < 10'000.0);
  QVERIFY(priceAxis->max() > 10'000.0);
  QVERIFY(priceAxis->max() - priceAxis->min() < 10.0);
  QCOMPARE(volumeAxis->min(), 0.0);
  QVERIFY(volumeAxis->max() >= 1.0);
  QVERIFY(std::isfinite(volumeAxis->max()));
}

void QtChartsCandlestickViewTest::priceAndVolumeFormOneContinuousSurface() {
  for (const auto size :
       {QSize{1024, 768}, QSize{1200, 800}, QSize{1440, 900}}) {
    bte::frontend::QtChartsCandlestickView view;
    view.resize(size);
    view.setBarWindow(std::array{makeBar(2, 100.0, 104.0)});
    view.show();
    QVERIFY(QTest::qWaitForWindowExposed(&view));
    QCoreApplication::processEvents();
    auto *price = chartViewport(view);
    auto *volume = view.findChild<QChartView *>("replayVolumeChartView");
    QVERIFY(volume != nullptr);
    const auto share = static_cast<double>(price->height()) /
                       static_cast<double>(price->height() + volume->height());
    QVERIFY2(share >= 0.77 && share <= 0.82,
             qPrintable(QString{"price widget share %1 at %2x%3"}
                            .arg(share)
                            .arg(size.width())
                            .arg(size.height())));
    const auto pricePlot = price->chart()->plotArea().height();
    const auto volumePlot = volume->chart()->plotArea().height();
    const auto plotShare = pricePlot / (pricePlot + volumePlot);
    QVERIFY2(plotShare >= 0.79 && plotShare <= 0.87,
             qPrintable(QString{"price plot share %1 (%2 / %3) at %4x%5"}
                            .arg(plotShare)
                            .arg(pricePlot)
                            .arg(volumePlot)
                            .arg(size.width())
                            .arg(size.height())));
    QCOMPARE(price->frameShape(), QFrame::NoFrame);
    QCOMPARE(volume->frameShape(), QFrame::NoFrame);
    QVERIFY(std::abs(price->geometry().bottom() + 1 -
                     volume->geometry().top()) <= 1);
    QVERIFY(!price->chart()->axes(Qt::Horizontal).front()->isVisible() ||
            !qobject_cast<QDateTimeAxis *>(
                 price->chart()->axes(Qt::Horizontal).front())
                 ->labelsVisible());
  }
}

void QtChartsCandlestickViewTest::denseHourlyUtcLabelsDoNotOverlap() {
  bte::frontend::QtChartsCandlestickView view;
  view.resize(1200, 800);
  std::array<bte::core::Bar, 13> bars;
  for (auto index = 0; index < static_cast<int>(bars.size()); ++index) {
    bars[static_cast<std::size_t>(index)] = makeBar(2, 100.0, 104.0);
    bars[static_cast<std::size_t>(index)].ts += std::chrono::hours{index};
  }
  view.setBarWindow(bars);
  view.show();
  QVERIFY(QTest::qWaitForWindowExposed(&view));
  QCoreApplication::processEvents();

  const auto *volume = view.findChild<QChartView *>("replayVolumeChartView");
  QVERIFY(volume != nullptr);
  std::vector<QRectF> labels;
  for (const auto *item : volume->scene()->items()) {
    if (item->data(0).toString() == "replayUtcTimeLabel" && item->isVisible()) {
      labels.push_back(item->sceneBoundingRect());
    }
  }
  QVERIFY(!labels.empty());
  for (auto first = std::size_t{0}; first < labels.size(); ++first) {
    for (auto second = first + 1; second < labels.size(); ++second) {
      QVERIFY2(!labels[first].intersects(labels[second]),
               "UTC tick labels must not overlap at an early replay prefix");
    }
  }
}

void QtChartsCandlestickViewTest::
    resizeRecomputesVisibleSlotsWithoutLosingTail() {
  std::vector<bte::core::Bar> bars;
  bars.reserve(200);
  for (auto index = 0; index < 200; ++index) {
    auto bar = makeBar(2, 100.0, 104.0);
    bar.ts += std::chrono::hours{index};
    bars.push_back(bar);
  }
  bte::frontend::QtChartsCandlestickView view;
  view.resize(1024, 768);
  view.setBarWindow(bars);
  view.show();
  QVERIFY(QTest::qWaitForWindowExposed(&view));
  QCoreApplication::processEvents();
  const auto wide = view.visibleSlots();
  QCOMPARE(wide.end, bars.size());

  view.resize(620, 768);
  QCoreApplication::processEvents();
  const auto narrow = view.visibleSlots();
  QCOMPARE(narrow.end, bars.size());
  QVERIFY(narrow.first > wide.first);
  view.resize(1200, 768);
  QCoreApplication::processEvents();
  const auto wider = view.visibleSlots();
  QCOMPARE(wider.end, bars.size());
  QVERIFY(wider.first < narrow.first);
}

void QtChartsCandlestickViewTest::
    plotResizeDuringRangeUpdateDoesNotReenterAxes() {
  bte::frontend::QtChartsCandlestickView view;
  view.resize(900, 600);
  view.show();
  QVERIFY(QTest::qWaitForWindowExposed(&view));
  std::vector<bte::core::Bar> bars;
  for (auto index = 0; index < 200; ++index) {
    auto bar = makeBar(1, 100.0 + index, 102.0 + index);
    bar.ts += std::chrono::hours{index};
    bars.push_back(bar);
  }
  view.setBarWindow(bars);
  QCoreApplication::processEvents();
  auto *chart = chartViewport(view)->chart();
  auto *axis = chart->findChild<QValueAxis *>();
  QVERIFY(axis != nullptr);
  auto depth = 0;
  auto maximumDepth = 0;
  auto resized = false;
  auto slotsBeforeResize = view.visibleSlots();
  const auto connection =
      connect(axis, &QValueAxis::rangeChanged, &view, [&](double, double) {
        ++depth;
        maximumDepth = std::max(maximumDepth, depth);
        if (!resized) {
          resized = true;
          slotsBeforeResize = view.visibleSlots();
          chart->resize(chart->size() + QSizeF{120.0, 0.0});
        }
        --depth;
      });
  view.zoomIn();
  QVERIFY(resized);
  QCOMPARE(maximumDepth, 1);
  QVERIFY(bte::test::waitUntil([&] {
    return (view.visibleSlots().end - view.visibleSlots().first >
            slotsBeforeResize.end - slotsBeforeResize.first);
  }));
  const auto slotsAfterResize = view.visibleSlots();
  QVERIFY(slotsAfterResize.end - slotsAfterResize.first >
          slotsBeforeResize.end - slotsBeforeResize.first);
  for (auto pass = 0; pass < 3; ++pass) {
    QCoreApplication::processEvents();
    QCOMPARE(view.visibleSlots().first, slotsAfterResize.first);
    QCOMPARE(view.visibleSlots().end, slotsAfterResize.end);
  }
  const auto *priceTime = chart->findChild<QDateTimeAxis *>();
  const auto *volume = view.findChild<QChartView *>("replayVolumeChartView");
  QVERIFY(priceTime != nullptr);
  QVERIFY(volume != nullptr);
  const auto *volumeTime = volume->chart()->findChild<QDateTimeAxis *>();
  QVERIFY(volumeTime != nullptr);
  QCOMPARE(volumeTime->min(), priceTime->min());
  QCOMPARE(volumeTime->max(), priceTime->max());
  QVERIFY(std::isfinite(axis->min()));
  QVERIFY(axis->max() > axis->min());
  QCOMPARE(view.candleCount(), bars.size());
  disconnect(connection);
}

void QtChartsCandlestickViewTest::renderedVolumeTimeGridMatchesPriceSlots() {
  bte::frontend::QtChartsCandlestickView view;
  view.resize(1200, 700);
  std::vector<bte::core::Bar> bars;
  for (auto index = 0; index < 100; ++index) {
    auto bar = makeBar(1, 100.0 + index, 102.0 + index);
    bar.ts += std::chrono::hours{index};
    bar.volume = 0.0;
    bars.push_back(bar);
  }
  view.setBarWindow(bars);
  view.show();
  QVERIFY(QTest::qWaitForWindowExposed(&view));
  auto *volume = view.findChild<QChartView *>("replayVolumeChartView");
  QVERIFY(volume != nullptr);
  auto *time = volume->chart()->findChild<QDateTimeAxis *>();
  QVERIFY(time != nullptr);
  time->setGridLineColor(QColor{255, 0, 255});
  for (auto pass = 0; pass < 4; ++pass) {
    QCoreApplication::processEvents();
  }
  QImage rendered{volume->size(), QImage::Format_ARGB32_Premultiplied};
  rendered.fill(Qt::transparent);
  QPainter painter{&rendered};
  volume->render(&painter);
  painter.end();
  const auto pricePlot = chartViewport(view)->chart()->plotArea();
  const auto volumePlot = volume->chart()->plotArea();
  const auto x = static_cast<int>(pricePlot.left() + pricePlot.width() / 4.0);
  const auto y = static_cast<int>(volumePlot.center().y() - 8.0);
  auto alignedGrid = false;
  for (auto offset = -2; offset <= 2; ++offset) {
    const auto pixel = rendered.pixelColor(x + offset, y);
    alignedGrid = alignedGrid || (pixel.red() > 180 && pixel.blue() > 180 &&
                                  pixel.green() < 100);
  }
  QVERIFY2(alignedGrid,
           "Rendered volume time grid must align with the price time grid");
}

void QtChartsCandlestickViewTest::
    volumePlotAlignmentWaitsForLayoutCompletion() {
  bte::frontend::QtChartsCandlestickView view;
  view.resize(900, 600);
  view.setBarWindow(std::array{makeBar(2, 100.0, 104.0)});
  view.show();
  QVERIFY(QTest::qWaitForWindowExposed(&view));
  QCoreApplication::processEvents();
  auto *price = chartViewport(view)->chart();
  auto *volumeView = view.findChild<QChartView *>("replayVolumeChartView");
  QVERIFY(volumeView != nullptr);
  auto *volume = volumeView->chart();
  QSignalSpy changes{volume, &QChart::plotAreaChanged};
  auto transientMargins = volume->margins();
  transientMargins.setLeft(transientMargins.left() + 120);
  volume->setMargins(transientMargins);
  QVERIFY(bte::test::waitUntil([&] { return (!changes.isEmpty()); }));
  QVERIFY(bte::test::waitUntil([&] {
    return (std::abs(price->plotArea().right() - volume->plotArea().right()) <=
                2.0 &&
            std::abs(price->plotArea().left() - volume->plotArea().left()) <=
                2.0);
  }));
  const auto settledChanges = changes.size();
  for (auto pass = 0; pass < 3; ++pass) {
    QCoreApplication::processEvents();
    QCOMPARE(changes.size(), settledChanges);
  }
}

void QtChartsCandlestickViewTest::paneResizeOrderKeepsTimeGeometryAligned() {
  bte::frontend::QtChartsCandlestickView view;
  view.resize(900, 600);
  view.setBarWindow(std::array{makeBar(2, 100.0, 104.0)});
  view.show();
  QVERIFY(QTest::qWaitForWindowExposed(&view));
  QCoreApplication::processEvents();
  auto *price = chartViewport(view);
  auto *volume = view.findChild<QChartView *>("replayVolumeChartView");
  QVERIFY(volume != nullptr);
  view.layout()->setEnabled(false);
  auto *volumeAxis = volume->chart()->findChild<QValueAxis *>();
  QVERIFY(volumeAxis != nullptr);
  volumeAxis->setGridLineColor(QColor{255, 0, 255});
  // Native full-screen transitions can resize the price child before volume.
  price->resize(1800, price->height());
  QCoreApplication::processEvents();
  QSignalSpy resizedVolumePlot{volume->chart(), &QChart::plotAreaChanged};
  volume->resize(1800, volume->height());
  QCoreApplication::processEvents();
  QVERIFY2(!resizedVolumePlot.isEmpty(),
           "Resizing a manually aligned volume chart must refresh its layout");
  QVERIFY2(std::abs(price->chart()->plotArea().right() -
                    volume->chart()->plotArea().right()) <= 2.0,
           qPrintable(QString{"price right=%1 volume right=%2"}
                          .arg(price->chart()->plotArea().right())
                          .arg(volume->chart()->plotArea().right())));
  QImage rendered{volume->size(), QImage::Format_ARGB32_Premultiplied};
  rendered.fill(Qt::transparent);
  QPainter painter{&rendered};
  volume->render(&painter);
  painter.end();
  const auto plot = volume->chart()->plotArea();
  const auto x = static_cast<int>(plot.right() - 10.0);
  const auto y = static_cast<int>(plot.center().y());
  auto gridReachesRight = false;
  for (auto offset = -2; offset <= 2; ++offset) {
    const auto pixel = rendered.pixelColor(x, y + offset);
    gridReachesRight =
        gridReachesRight ||
        (pixel.red() > 180 && pixel.blue() > 180 && pixel.green() < 100);
  }
  QVERIFY2(gridReachesRight,
           "Rendered volume grid must reach the aligned plot's right edge");
}

void QtChartsCandlestickViewTest::dragReviewsReachedHistoryUntilFollowLatest() {
  std::vector<bte::core::Bar> bars;
  bars.reserve(200);
  for (auto index = 0; index < 200; ++index) {
    auto bar = makeBar(2, 100.0, 104.0);
    bar.ts += std::chrono::hours{index};
    bars.push_back(bar);
  }
  bte::frontend::QtChartsCandlestickView view;
  view.resize(900, 600);
  view.setBarWindow(bars);
  view.show();
  QVERIFY(QTest::qWaitForWindowExposed(&view));
  QCoreApplication::processEvents();
  auto *price = chartViewport(view);
  const auto inside = price->mapFromScene(price->chart()->plotArea().center());
  const auto atTail = view.visibleSlots();
  QCOMPARE(atTail.end, bars.size());
  QVERIFY(view.isFollowingLatest());
  QVERIFY(!view.canFitReachedBars());
  QVERIFY(!view.fitReachedBars());
  QTest::mousePress(price->viewport(), Qt::LeftButton, {}, inside);
  QTest::mouseMove(price->viewport(), inside + QPoint{140, 0});
  QTest::mouseRelease(price->viewport(), Qt::LeftButton, {},
                      inside + QPoint{140, 0});
  QVERIFY(!view.isFollowingLatest());
  const auto reviewed = view.visibleSlots();
  QVERIFY(reviewed.first < atTail.first);
  auto next = makeBar(2, 104.0, 108.0);
  next.ts += std::chrono::hours{200};
  view.appendBar(next);
  QCOMPARE(view.visibleSlots().first, reviewed.first);
  QCOMPARE(view.visibleSlots().end, reviewed.end);
  view.followLatest();
  QVERIFY(view.isFollowingLatest());
  QCOMPARE(view.visibleSlots().end, bars.size() + 1);
  view.resetZoom();
  QVERIFY(view.isFollowingLatest());
  QCOMPARE(view.visibleSlots().end, bars.size() + 1);
}

void QtChartsCandlestickViewTest::
    fitReachedBarsShowsSmallPrefixAndKeepsFullCounts() {
  bte::frontend::QtChartsCandlestickView view;
  view.resize(900, 600);
  std::vector<bte::core::Bar> bars;
  bars.reserve(20);
  for (auto index = 0; index < 20; ++index) {
    auto bar = makeBar(2, 100.0, 104.0);
    bar.ts += std::chrono::hours{index};
    bars.push_back(bar);
  }
  view.setBarWindow(bars);
  view.show();
  QVERIFY(QTest::qWaitForWindowExposed(&view));
  QCoreApplication::processEvents();
  QVERIFY(view.canFitReachedBars());
  QVERIFY(view.fitReachedBars());
  QCOMPARE(view.visibleSlots().first, 0U);
  QCOMPARE(view.visibleSlots().end, bars.size());
  QCOMPARE(view.candleCount(), bars.size());
  QCOMPARE(view.volumePointCount(), bars.size());
}

void QtChartsCandlestickViewTest::appendBarAddsOneCandle() {
  bte::frontend::QtChartsCandlestickView view;
  const std::array bars{
      makeBar(2, 100.0, 104.0),
      makeBar(3, 104.0, 101.0),
  };

  view.setBarWindow(bars);
  view.appendBar(makeBar(4, 101.0, 108.0));

  QCOMPARE(view.candleCount(), 3U);
  QCOMPARE(view.volumePointCount(), 3U);
}

void QtChartsCandlestickViewTest::appendBarsAddsAPlaybackBatch() {
  bte::frontend::QtChartsCandlestickView view;
  view.setBarWindow(std::array{makeBar(2, 100.0, 104.0)});
  const std::array batch{makeBar(3, 104.0, 101.0), makeBar(4, 101.0, 108.0)};

  view.appendBars(batch);

  QCOMPARE(view.candleCount(), 3U);
  QCOMPARE(view.volumePointCount(), 3U);
}

void QtChartsCandlestickViewTest::invalidBarIsIgnoredAndMarkersCanBeCleared() {
  bte::frontend::QtChartsCandlestickView view;
  auto invalid = makeBar(2, 100.0, 104.0);
  invalid.high = invalid.low - 1.0;

  view.appendBar(invalid);
  QCOMPARE(view.candleCount(), 0U);
  view.clearMarkers();
  QCOMPARE(view.markerCount(), 0U);
}

void QtChartsCandlestickViewTest::rendersSynchronizedVolumeAndTradeMarkers() {
  bte::frontend::QtChartsCandlestickView view;
  const std::array bars{makeBar(2, 100.0, 104.0), makeBar(3, 104.0, 101.0)};
  const std::array markers{
      bte::frontend::ChartMarker{
          .timestamp = makeTimestamp(2), .price = 101.0, .isBuy = true},
      bte::frontend::ChartMarker{
          .timestamp = makeTimestamp(3), .price = 103.0, .isBuy = false}};

  view.setBarWindow(bars);
  view.setMarkers(markers);
  QCOMPARE(view.volumePointCount(), 2U);
  QCOMPARE(view.markerCount(), 2U);
  const auto *chartView = view.findChild<QChartView *>("replayChartView");
  QVERIFY(chartView != nullptr);
  const auto series = chartView->chart()->series();
  const auto findMarkers = [&](const QString &name) {
    const auto found =
        std::ranges::find(series, name, &QAbstractSeries::objectName);
    return found == series.end() ? nullptr
                                 : qobject_cast<QScatterSeries *>(*found);
  };
  const auto *buy = findMarkers("replayBuyMarkers");
  const auto *sell = findMarkers("replaySellMarkers");
  QVERIFY(buy != nullptr);
  QVERIFY(sell != nullptr);
  QCOMPARE(sell->markerShape(), QScatterSeries::MarkerShapeTriangle);
  // Inspect the actual glyph images supplied to the renderer, not an enum that
  // cannot distinguish upward and downward triangles in Qt Charts.
  const auto &buyGlyph = buy->lightMarker();
  const auto &sellGlyph = sell->lightMarker();
  QVERIFY(!buyGlyph.isNull());
  QVERIFY(!sellGlyph.isNull());
  QCOMPARE(buyGlyph.size(), QSize(16, 16));
  QCOMPARE(sellGlyph.size(), QSize(16, 16));
  QVERIFY(buyGlyph.pixelColor(8, 3).alpha() > 0);
  QCOMPARE(buyGlyph.pixelColor(3, 3).alpha(), 0);
  QVERIFY(buyGlyph.pixelColor(3, 12).alpha() > 0);
  QVERIFY(sellGlyph.pixelColor(3, 3).alpha() > 0);
  QCOMPARE(sellGlyph.pixelColor(3, 12).alpha(), 0);
  QVERIFY(sellGlyph.pixelColor(8, 12).alpha() > 0);
  QCOMPARE(buy->points().front(), QPointF(0.0, 101.0));
  QCOMPARE(sell->points().front(), QPointF(3'600'000.0, 103.0));
  QCOMPARE(buy->color(), QColor(52, 211, 153));
  QCOMPARE(sell->color(), QColor(251, 113, 133));

  view.clearMarkers();
  QCOMPARE(view.markerCount(), 0U);
}

void QtChartsCandlestickViewTest::
    appendingMarkersKeepsEarlierFillsWithoutRebuildingThem() {
  bte::frontend::QtChartsCandlestickView view;
  view.setBarWindow(
      std::array{makeBar(2, 100.0, 104.0), makeBar(3, 104.0, 101.0)});
  view.appendMarkers(std::array{bte::frontend::ChartMarker{
      .timestamp = makeTimestamp(2), .price = 101.0, .isBuy = true}});
  QCOMPARE(view.markerCount(), 1U);
  view.appendMarkers(std::array{bte::frontend::ChartMarker{
      .timestamp = makeTimestamp(3), .price = 103.0, .isBuy = false}});
  QCOMPARE(view.markerCount(), 2U);
  view.setMarkers(std::array{bte::frontend::ChartMarker{
      .timestamp = makeTimestamp(3), .price = 103.0, .isBuy = false}});
  QCOMPARE(view.markerCount(), 1U);
}

void QtChartsCandlestickViewTest::
    tradeGlyphsAreAdjacentToTheirContainingCandles() {
  bte::frontend::QtChartsCandlestickView view;
  view.resize(900, 520);
  const std::array bars{makeBar(2, 100.0, 104.0), makeBar(3, 112.0, 106.0),
                        makeBar(4, 96.0, 108.0), makeBar(5, 108.0, 102.0)};
  const auto dailyFillTime = makeTimestamp(3) + std::chrono::hours{9};
  const std::array markers{
      bte::frontend::ChartMarker{.timestamp = dailyFillTime,
                                 .price = 109.0,
                                 .isBuy = true,
                                 .candleTimestamp = makeTimestamp(3)},
      bte::frontend::ChartMarker{
          .timestamp = makeTimestamp(4), .price = 105.0, .isBuy = false}};
  view.setBarWindow(bars);
  view.setMarkers(markers);
  view.show();
  QVERIFY(QTest::qWaitForWindowExposed(&view));
  QCoreApplication::processEvents();

  auto *viewport = chartViewport(view);
  const auto series = viewport->chart()->series();
  const auto findSeries = [&](const QString &name) {
    const auto found =
        std::ranges::find(series, name, &QAbstractSeries::objectName);
    return found == series.end() ? nullptr
                                 : qobject_cast<QScatterSeries *>(*found);
  };
  auto *buySeries = findSeries("replayBuyMarkers");
  auto *sellSeries = findSeries("replaySellMarkers");
  QVERIFY(buySeries != nullptr);
  QVERIFY(sellSeries != nullptr);

  QGraphicsPixmapItem *buyGlyph = nullptr;
  QGraphicsPixmapItem *sellGlyph = nullptr;
  for (auto *item : viewport->scene()->items()) {
    if (item->data(0).toString() != "replayTradeGlyph") {
      continue;
    }
    auto *glyph = qgraphicsitem_cast<QGraphicsPixmapItem *>(item);
    QVERIFY(glyph != nullptr);
    if (glyph->toolTip().startsWith("Buy")) {
      buyGlyph = glyph;
    } else if (glyph->toolTip().startsWith("Sell")) {
      sellGlyph = glyph;
    }
  }
  QVERIFY(buyGlyph != nullptr);
  QVERIFY(sellGlyph != nullptr);

  const auto *candles = qobject_cast<QCandlestickSeries *>(series.front());
  QVERIFY(candles != nullptr);
  const auto sets = candles->sets();
  const auto buyCandleLow = viewport->chart()->mapToPosition(
      {sets.at(1)->timestamp(), bars[1].low}, buySeries);
  const auto sellCandleHigh = viewport->chart()->mapToPosition(
      {sets.at(2)->timestamp(), bars[2].high}, sellSeries);
  const auto buyCenter = buyGlyph->pos() + QPointF{8.0, 8.0};
  const auto sellCenter = sellGlyph->pos() + QPointF{8.0, 8.0};
  const auto buyBounds = buyGlyph->mapRectToParent(buyGlyph->boundingRect());
  const auto sellBounds = sellGlyph->mapRectToParent(sellGlyph->boundingRect());
  QVERIFY2(buyBounds.top() >= buyCandleLow.y() + 8.0,
           "Buy triangle must leave eight logical pixels below its wick");
  QVERIFY2(sellBounds.bottom() <= sellCandleHigh.y() - 8.0,
           "Sell triangle must leave eight logical pixels above its wick");
  QVERIFY(buyBounds.top() <= buyCandleLow.y() + 9.0);
  QVERIFY(sellBounds.bottom() >= sellCandleHigh.y() - 9.0);
  QVERIFY(
      std::ranges::none_of(viewport->scene()->items(), [](const auto *item) {
        return item->data(0).toString() == "replayTradeLeader" &&
               item->isVisible();
      }));

  QVERIFY(std::abs(buyCenter.x() - buyCandleLow.x()) <= 1.0);
  QVERIFY2(buyCenter.y() >= buyCandleLow.y() + 8.0,
           qPrintable(QString{"buy center=%1 low=%2"}
                          .arg(buyCenter.y())
                          .arg(buyCandleLow.y())));
  QVERIFY(std::abs(sellCenter.x() - sellCandleHigh.x()) <= 1.0);
  QVERIFY2(sellCenter.y() <= sellCandleHigh.y() - 8.0,
           qPrintable(QString{"sell center=%1 high=%2"}
                          .arg(sellCenter.y())
                          .arg(sellCandleHigh.y())));

  view.resize(640, 400);
  view.zoomIn();
  view.setDailyTimeframe(true);
  QVERIFY(bte::test::waitUntil([&] {
    return (buyGlyph->mapRectToParent(buyGlyph->boundingRect()).top() >=
            viewport->chart()
                    ->mapToPosition({sets.at(1)->timestamp(), bars[1].low},
                                    buySeries)
                    .y() +
                8.0);
  }));
  QVERIFY(bte::test::waitUntil([&] {
    return (sellGlyph->mapRectToParent(sellGlyph->boundingRect()).bottom() <=
            viewport->chart()
                    ->mapToPosition({sets.at(2)->timestamp(), bars[2].high},
                                    sellSeries)
                    .y() -
                8.0);
  }));
  QVERIFY(viewport->chart()->plotArea().contains(
      buyGlyph->mapRectToParent(buyGlyph->boundingRect())));
  QVERIFY(viewport->chart()->plotArea().contains(
      sellGlyph->mapRectToParent(sellGlyph->boundingRect())));
}

void QtChartsCandlestickViewTest::
    tradeGlyphClearanceSurvivesHeightOnlyResizeAtPriceExtrema() {
  bte::frontend::QtChartsCandlestickView view;
  view.resize(900, 520);
  const bte::core::Bar bar{.ts = makeTimestamp(2),
                           .open = 1.0,
                           .high = 200.0,
                           .low = 0.01,
                           .close = 199.0,
                           .volume = 1000.0};
  view.setBarWindow(std::array{bar});
  view.setMarkers(
      std::array{bte::frontend::ChartMarker{
                     .timestamp = bar.ts, .price = 1.0, .isBuy = true},
                 bte::frontend::ChartMarker{
                     .timestamp = bar.ts, .price = 199.0, .isBuy = false}});
  view.show();
  QVERIFY(QTest::qWaitForWindowExposed(&view));
  QCOMPARE(view.candleCount(), 1U);
  QCOMPARE(view.markerCount(), 2U);
  auto *viewport = chartViewport(view);
  const auto clearAndVisible = [&] {
    auto count = 0;
    for (auto *item : viewport->scene()->items()) {
      if (item->data(0).toString() != "replayTradeGlyph") {
        continue;
      }
      const auto *glyph = qgraphicsitem_cast<QGraphicsPixmapItem *>(item);
      if (glyph == nullptr || !glyph->isVisible()) {
        return false;
      }
      const auto buy = glyph->toolTip().startsWith("Buy");
      const auto edge =
          viewport->chart()->mapToPosition({0.0, buy ? bar.low : bar.high});
      const auto bounds = glyph->mapRectToParent(glyph->boundingRect());
      const auto gap =
          buy ? bounds.top() - edge.y() : edge.y() - bounds.bottom();
      if (gap < 8.0 - 0.000001 ||
          !viewport->chart()->plotArea().contains(bounds)) {
        return false;
      }
      ++count;
    }
    return count == 2;
  };
  QVERIFY(bte::test::waitUntil([&] { return (clearAndVisible()); }));
  view.resize(900, 360);
  QVERIFY(bte::test::waitUntil([&] { return (clearAndVisible()); }));
  view.setDailyTimeframe(true);
  QVERIFY(bte::test::waitUntil([&] { return (clearAndVisible()); }));
  QCOMPARE(view.markerCount(), 2U);
  QCOMPARE(view.candleCount(), 1U);
}

void QtChartsCandlestickViewTest::onlyCoincidentSellFillsShowLeaders() {
  bte::frontend::QtChartsCandlestickView view;
  view.resize(900, 520);
  view.setBarWindow(std::array{makeBar(2, 100.0, 104.0)});
  view.setMarkers(std::array{
      bte::frontend::ChartMarker{
          .timestamp = makeTimestamp(2), .price = 101.0, .isBuy = true},
      bte::frontend::ChartMarker{
          .timestamp = makeTimestamp(2), .price = 103.0, .isBuy = false}});
  view.show();
  QVERIFY(QTest::qWaitForWindowExposed(&view));
  auto *viewport = chartViewport(view);
  const auto visibleItems = [&](const QString &kind) {
    return std::ranges::count_if(
        viewport->scene()->items(), [&](const auto *item) {
          return item->data(0).toString() == kind && item->isVisible();
        });
  };
  QVERIFY(bte::test::waitUntil(
      [&] { return (visibleItems("replayTradeGlyph")) == (2); }));
  QCOMPARE(visibleItems("replayTradeGlyph"), 2);
  QCOMPARE(visibleItems("replayTradeLeader"), 0);
  view.appendMarkers(std::array{bte::frontend::ChartMarker{
      .timestamp = makeTimestamp(2), .price = 102.0, .isBuy = true}});
  QVERIFY(bte::test::waitUntil(
      [&] { return (visibleItems("replayTradeGlyph")) == (2); }));
  QCOMPARE(visibleItems("replayTradeGlyph"), 2);
  QCOMPARE(visibleItems("replayTradeLeader"), 0);
  QCOMPARE(view.markerCount(), 3U);
  QVERIFY(std::ranges::any_of(viewport->scene()->items(), [](const auto *item) {
    return item->data(0).toString() == "replayTradeGlyph" &&
           item->toolTip().startsWith("Buy") &&
           item->toolTip().contains("2 reached Buy fills");
  }));
  view.appendMarkers(std::array{bte::frontend::ChartMarker{
      .timestamp = makeTimestamp(2), .price = 102.5, .isBuy = false}});
  QVERIFY(bte::test::waitUntil(
      [&] { return (visibleItems("replayTradeGlyph")) == (3); }));
  QCOMPARE(visibleItems("replayTradeGlyph"), 3);
  QCOMPARE(visibleItems("replayTradeLeader"), 2);
  QCOMPARE(view.markerCount(), 4U);
  view.clearMarkers();
  QCOMPARE(visibleItems("replayTradeGlyph"), 0);
  QCOMPARE(visibleItems("replayTradeLeader"), 0);
  QCOMPARE(view.markerCount(), 0U);
}

void QtChartsCandlestickViewTest::
    coincidentBuysShareOneGlyphWithoutChangingFinancialPoints() {
  bte::frontend::QtChartsCandlestickView view;
  view.resize(800, 500);
  view.setBarWindow(std::array{makeBar(2, 100.0, 104.0)});
  std::vector<bte::frontend::ChartMarker> markers;
  for (auto index = 0; index < 10; ++index) {
    markers.push_back({.timestamp = makeTimestamp(2),
                       .price = 101.0,
                       .isBuy = index % 2 == 0});
  }
  view.setMarkers(markers);
  view.show();
  QVERIFY(QTest::qWaitForWindowExposed(&view));
  QCoreApplication::processEvents();
  auto *viewport = chartViewport(view);
  const auto verifyGlyphs = [&] {
    std::vector<QRectF> bounds;
    for (const auto *item : viewport->scene()->items()) {
      if (item->data(0).toString() != "replayTradeGlyph") {
        continue;
      }
      if (!item->isVisible()) {
        return false;
      }
      if (item->type() != QGraphicsPixmapItem::Type) {
        return false;
      }
      const auto rectangle = item->sceneBoundingRect();
      if (!viewport->chart()
               ->mapRectToScene(viewport->chart()->plotArea())
               .contains(rectangle)) {
        return false;
      }
      if (std::ranges::any_of(bounds, [&](const QRectF &other) {
            return other.intersects(rectangle);
          })) {
        return false;
      }
      if (!item->toolTip().contains("2024-01-02T00:00:00Z") ||
          !item->toolTip().contains("101")) {
        return false;
      }
      bounds.push_back(rectangle);
    }
    return bounds.size() == 6;
  };
  QVERIFY(verifyGlyphs());
  QCOMPARE(view.markerCount(), 10U);
  QCOMPARE(std::ranges::count_if(viewport->scene()->items(),
                                 [](const auto *item) {
                                   return item->data(0).toString() ==
                                              "replayTradeLeader" &&
                                          item->isVisible();
                                 }),
           5);
  for (const auto *series : viewport->chart()->series()) {
    if (const auto *scatter = qobject_cast<const QScatterSeries *>(series)) {
      QCOMPARE(scatter->count(), 5);
      for (const auto &point : scatter->points()) {
        QCOMPARE(point, QPointF(0.0, 101.0));
      }
    }
  }
  view.zoomIn();
  QCoreApplication::processEvents();
  QVERIFY(verifyGlyphs());
  view.resize(640, 400);
  QCoreApplication::processEvents();
  QVERIFY(verifyGlyphs());
  view.clearMarkers();
  QCOMPARE(view.markerCount(), 0U);
  QVERIFY(
      std::ranges::none_of(viewport->scene()->items(), [](const auto *item) {
        return item->data(0).toString() == "replayTradeGlyph";
      }));
}

void QtChartsCandlestickViewTest::
    buyGroupingTracksReachedFillsAndDisplayedTimeframe() {
  bte::frontend::QtChartsCandlestickView view;
  view.resize(900, 520);
  view.setDailyTimeframe(true);
  view.setBarWindow(std::array{makeBar(2, 100.0, 104.0)});
  const bte::frontend::ChartMarker first{.timestamp = makeTimestamp(2) +
                                                      std::chrono::hours{1},
                                         .price = 101.0,
                                         .isBuy = true,
                                         .candleTimestamp = makeTimestamp(2)};
  const bte::frontend::ChartMarker second{.timestamp = makeTimestamp(2) +
                                                       std::chrono::hours{3},
                                          .price = 102.0,
                                          .isBuy = true,
                                          .candleTimestamp = makeTimestamp(2)};
  view.appendMarkers(std::array{first});
  view.show();
  QVERIFY(QTest::qWaitForWindowExposed(&view));
  auto *viewport = chartViewport(view);
  const auto glyphs = [&] {
    std::vector<QGraphicsItem *> result;
    for (auto *item : viewport->scene()->items()) {
      if (item->data(0).toString() == "replayTradeGlyph" && item->isVisible()) {
        result.push_back(item);
      }
    }
    return result;
  };
  QVERIFY(bte::test::waitUntil([&] { return (glyphs().size()) == (1U); }));
  QCOMPARE(glyphs().size(), 1U);
  QVERIFY(!glyphs().front()->toolTip().contains("2 reached"));
  view.appendMarkers(std::array{second});
  QVERIFY(bte::test::waitUntil([&] { return (glyphs().size()) == (1U); }));
  QCOMPARE(glyphs().size(), 1U);
  QVERIFY(glyphs().front()->toolTip().contains("2 reached Buy fills"));
  QVERIFY(glyphs().front()->toolTip().contains("2024-01-02T01:00:00Z"));
  QVERIFY(glyphs().front()->toolTip().contains("Price: 101"));
  QVERIFY(!glyphs().front()->toolTip().contains("2024-01-02T03:00:00Z"));
  const auto *buy =
      viewport->chart()->findChild<QScatterSeries *>("replayBuyMarkers");
  QVERIFY(buy != nullptr);
  QCOMPARE(buy->points().size(), 2);
  QCOMPARE(buy->points().at(0), QPointF(0.0, 101.0));
  QCOMPARE(buy->points().at(1), QPointF(0.0, 102.0));
  QCOMPARE(view.markerCount(), 2U);
  view.appendMarkers(std::array{
      bte::frontend::ChartMarker{
          .timestamp = makeTimestamp(3), .price = 101.0, .isBuy = true},
      bte::frontend::ChartMarker{.timestamp = first.timestamp,
                                 .price = 0.0,
                                 .isBuy = true,
                                 .candleTimestamp = makeTimestamp(2)}});
  QCOMPARE(view.markerCount(), 2U);
  QCOMPARE(glyphs().size(), 1U);
  QVERIFY(glyphs().front()->toolTip().contains("2 reached Buy fills"));

  view.setDailyTimeframe(false);
  auto firstHour = makeBar(2, 100.0, 104.0);
  firstHour.ts = first.timestamp;
  auto secondHour = firstHour;
  secondHour.ts = second.timestamp;
  view.setBarWindow(std::array{firstHour, secondHour});
  auto hourlyFirst = first;
  hourlyFirst.candleTimestamp.reset();
  auto hourlySecond = second;
  hourlySecond.candleTimestamp.reset();
  view.setMarkers(std::array{hourlyFirst, hourlySecond});
  QVERIFY(bte::test::waitUntil([&] { return (glyphs().size()) == (2U); }));
  QCOMPARE(glyphs().size(), 2U);
  QCOMPARE(view.markerCount(), 2U);
  for (const auto *glyph : glyphs()) {
    QVERIFY(!glyph->toolTip().contains("2 reached"));
  }
  view.setBarWindow(std::array{firstHour});
  view.setMarkers(std::array{hourlyFirst});
  QVERIFY(bte::test::waitUntil([&] { return (glyphs().size()) == (1U); }));
  QCOMPARE(glyphs().size(), 1U);
  QCOMPARE(view.markerCount(), 1U);
  view.setDailyTimeframe(true);
  view.setBarWindow(std::array{makeBar(2, 100.0, 104.0)});
  view.setMarkers(std::array{first});
  QVERIFY(bte::test::waitUntil([&] { return (glyphs().size()) == (1U); }));
  QCOMPARE(glyphs().size(), 1U);
  QVERIFY(!glyphs().front()->toolTip().contains("2 reached"));
  view.clearMarkers();
  QCOMPARE(glyphs().size(), 0U);
  QCOMPARE(view.markerCount(), 0U);
}

void QtChartsCandlestickViewTest::
    markersOutsideTheZoomedViewportAreHiddenWithoutChangingFinancialPoints() {
  bte::frontend::QtChartsCandlestickView view;
  view.resize(800, 500);
  const std::array bars{makeBar(2, 100.0, 104.0), makeBar(3, 104.0, 101.0),
                        makeBar(4, 101.0, 108.0), makeBar(5, 108.0, 105.0),
                        makeBar(6, 105.0, 109.0), makeBar(7, 109.0, 106.0),
                        makeBar(8, 106.0, 110.0), makeBar(9, 110.0, 107.0)};
  view.setBarWindow(bars);
  view.setMarkers(std::array{bte::frontend::ChartMarker{
      .timestamp = makeTimestamp(2), .price = 101.0, .isBuy = true}});
  view.show();
  QVERIFY(QTest::qWaitForWindowExposed(&view));
  QCoreApplication::processEvents();

  auto *viewport = chartViewport(view);
  const auto glyphs = [&] {
    std::vector<QGraphicsItem *> result;
    std::ranges::copy_if(viewport->scene()->items(), std::back_inserter(result),
                         [](const auto *item) {
                           return item->data(0).toString() ==
                                  "replayTradeGlyph";
                         });
    return result;
  };
  QCOMPARE(glyphs().size(), 1U);
  QVERIFY(glyphs().front()->isVisible());

  auto *timeAxis = qobject_cast<QDateTimeAxis *>(
      viewport->chart()->axes(Qt::Horizontal).front());
  QVERIFY(timeAxis != nullptr);
  timeAxis->setRange(
      QDateTime::fromMSecsSinceEpoch(5 * 3'600'000, QTimeZone::UTC),
      QDateTime::fromMSecsSinceEpoch(7 * 3'600'000, QTimeZone::UTC));
  QCoreApplication::processEvents();

  QVERIFY(!glyphs().front()->isVisible());
  QCOMPARE(view.markerCount(), 1U);
  const auto series = viewport->chart()->series();
  const auto found = std::ranges::find(series, QString{"replayBuyMarkers"},
                                       &QAbstractSeries::objectName);
  QVERIFY(found != series.end());
  const auto *buyMarkers = qobject_cast<QScatterSeries *>(*found);
  QVERIFY(buyMarkers != nullptr);
  QCOMPARE(buyMarkers->points().front(), QPointF(0.0, 101.0));
}

void QtChartsCandlestickViewTest::
    invalidAndOutOfWindowMarkersAreRejectedAndReplacementClearsThem() {
  bte::frontend::QtChartsCandlestickView view;
  const std::array markers{
      bte::frontend::ChartMarker{
          .timestamp = makeTimestamp(2), .price = 101.0, .isBuy = true},
      bte::frontend::ChartMarker{.timestamp = makeTimestamp(2),
                                 .price =
                                     std::numeric_limits<double>::quiet_NaN()},
      bte::frontend::ChartMarker{.timestamp = makeTimestamp(2), .price = 0.0},
      bte::frontend::ChartMarker{.timestamp = makeTimestamp(3),
                                 .price = 101.0}};
  view.setMarkers(markers);
  QCOMPARE(view.markerCount(), 0U);
  view.setBarWindow(std::array{makeBar(2, 100.0, 104.0)});
  view.setMarkers(markers);
  QCOMPARE(view.markerCount(), 1U);
  view.setBarWindow(std::array{makeBar(3, 100.0, 104.0)});
  QCOMPARE(view.markerCount(), 0U);
}

void QtChartsCandlestickViewTest::zoomControlsAreSafeToCall() {
  bte::frontend::QtChartsCandlestickView view;
  const std::array bars{
      makeBar(2, 100.0, 104.0),
      makeBar(3, 104.0, 101.0),
  };

  view.setBarWindow(bars);
  view.zoomIn();
  view.zoomOut();
  view.resetZoom();

  QCOMPARE(view.candleCount(), 2U);
}

void QtChartsCandlestickViewTest::pointerInteractionPansAndTogglesCrosshair() {
  bte::frontend::QtChartsCandlestickView view;
  view.resize(800, 500);
  view.setBarWindow(
      std::array{makeBar(2, 100.0, 104.0), makeBar(3, 104.0, 101.0)});
  view.show();
  QVERIFY(QTest::qWaitForWindowExposed(&view));
  QCoreApplication::processEvents();

  auto *chartView = chartViewport(view);
  const auto inside =
      chartView->mapFromScene(chartView->chart()->plotArea().center());
  const auto outside = chartView->viewport()->rect().bottomRight();

  QTest::mouseMove(chartView->viewport(), inside);
  QCoreApplication::processEvents();
  QCOMPARE(visibleCrosshairCount(*chartView), 2);

  QTest::mouseMove(chartView->viewport(), outside);
  QCoreApplication::processEvents();
  QCOMPARE(visibleCrosshairCount(*chartView), 0);

  QTest::mousePress(chartView->viewport(), Qt::LeftButton, {}, inside);
  QCOMPARE(chartView->cursor().shape(), Qt::ClosedHandCursor);
  QTest::mouseMove(chartView->viewport(), inside + QPoint{20, 10});
  QTest::mouseRelease(chartView->viewport(), Qt::LeftButton, {},
                      inside + QPoint{20, 10});
  QVERIFY(chartView->cursor().shape() != Qt::ClosedHandCursor);

  QTest::mousePress(chartView->viewport(), Qt::RightButton, {}, inside);
  QTest::mouseRelease(chartView->viewport(), Qt::RightButton, {}, inside);
  QVERIFY(chartView->cursor().shape() != Qt::ClosedHandCursor);

  QEvent leave{QEvent::Leave};
  QCoreApplication::sendEvent(chartView, &leave);
  QCOMPARE(visibleCrosshairCount(*chartView), 0);
}

void QtChartsCandlestickViewTest::wheelInteractionZoomsInAndOut() {
  bte::frontend::QtChartsCandlestickView view;
  view.resize(800, 500);
  std::vector<bte::core::Bar> bars;
  for (auto index = 0; index < 100; ++index) {
    auto bar = makeBar(2, 100.0, 104.0);
    bar.ts += std::chrono::hours{index};
    bars.push_back(bar);
  }
  view.setBarWindow(bars);
  view.show();
  QVERIFY(QTest::qWaitForWindowExposed(&view));
  QCoreApplication::processEvents();

  auto *chartView = chartViewport(view);
  auto *axisY = qobject_cast<QValueAxis *>(
      chartView->chart()->axes(Qt::Vertical).front());
  QVERIFY(axisY != nullptr);
  const auto initialSpan = axisY->max() - axisY->min();
  const auto initial = view.visibleSlots();
  const auto localCenter =
      chartView->mapFromScene(chartView->chart()->plotArea().center());
  const auto globalCenter = chartView->viewport()->mapToGlobal(localCenter);

  QWheelEvent zoomInEvent{localCenter, globalCenter,      {},   {0, 120}, {},
                          {},          Qt::NoScrollPhase, false};
  QCoreApplication::sendEvent(chartView->viewport(), &zoomInEvent);
  QVERIFY(zoomInEvent.isAccepted());
  const auto zoomed = view.visibleSlots();
  QVERIFY(zoomed.end - zoomed.first < initial.end - initial.first);
  QCOMPARE(axisY->max() - axisY->min(), initialSpan);

  QWheelEvent zoomOutEvent{localCenter, globalCenter,      {},   {0, -120}, {},
                           {},          Qt::NoScrollPhase, false};
  QCoreApplication::sendEvent(chartView->viewport(), &zoomOutEvent);
  QVERIFY(zoomOutEvent.isAccepted());
  const auto restored = view.visibleSlots();
  QVERIFY(restored.end - restored.first >= zoomed.end - zoomed.first);
  QCOMPARE(axisY->max() - axisY->min(), initialSpan);
}

void QtChartsCandlestickViewTest::
    wheelZoomKeepsPriceAndVolumeAxesNearVisibleCandles() {
  bte::frontend::QtChartsCandlestickView view;
  view.resize(800, 500);
  std::array<bte::core::Bar, 100> bars;
  for (int index = 0; index < static_cast<int>(bars.size()); ++index) {
    bars[static_cast<std::size_t>(index)] =
        makeBar(2, 165.0 + index * 0.4, 166.0 + index * 0.4);
    bars[static_cast<std::size_t>(index)].ts = bte::core::Timestamp{
        std::chrono::sys_days{std::chrono::year{2018} / 5 / 1} +
        std::chrono::hours{index}};
    bars[static_cast<std::size_t>(index)].volume =
        1'000'000.0 + index * 300'000.0;
  }
  view.setBarWindow(bars);
  view.show();
  QVERIFY(QTest::qWaitForWindowExposed(&view));
  QCoreApplication::processEvents();

  auto *chartView = chartViewport(view);
  auto *volumeView = view.findChild<QChartView *>("replayVolumeChartView");
  QVERIFY(volumeView != nullptr);
  const auto verticalAxes = chartView->chart()->axes(Qt::Vertical);
  auto *priceAxis = qobject_cast<QValueAxis *>(verticalAxes.at(0));
  auto *volumeAxis = qobject_cast<QValueAxis *>(
      volumeView->chart()->axes(Qt::Vertical).front());
  auto *timeAxis = qobject_cast<QDateTimeAxis *>(
      chartView->chart()->axes(Qt::Horizontal).front());
  QVERIFY(priceAxis != nullptr);
  QVERIFY(volumeAxis != nullptr);
  QVERIFY(timeAxis != nullptr);

  const auto localCenter =
      chartView->mapFromScene(chartView->chart()->plotArea().center());
  const auto globalCenter = chartView->viewport()->mapToGlobal(localCenter);
  const auto initial = view.visibleSlots();
  for (int index = 0; index < 40; ++index) {
    QWheelEvent zoomEvent{localCenter, globalCenter,      {},   {0, 120}, {},
                          {},          Qt::NoScrollPhase, false};
    QCoreApplication::sendEvent(chartView->viewport(), &zoomEvent);
    QVERIFY(zoomEvent.isAccepted());
    QVERIFY2(priceAxis->min() > 100.0 && priceAxis->max() < 250.0,
             "Wheel zoom must not scale the price axis to epoch-sized values");
    QVERIFY2(volumeAxis->min() >= 0.0 && volumeAxis->max() < 40'000'000.0,
             "Wheel zoom must not scale the volume axis out of view");
  }
  const auto zoomed = view.visibleSlots();
  QVERIFY(zoomed.end > zoomed.first);
  QVERIFY(zoomed.end - zoomed.first < initial.end - initial.first);
  QVERIFY(timeAxis->max() > timeAxis->min());
  QCOMPARE(view.candleCount(), bars.size());
  QCOMPARE(view.volumePointCount(), bars.size());

  view.resetZoom();
  const auto reset = view.visibleSlots();
  QVERIFY(reset.end - reset.first >= zoomed.end - zoomed.first);
  QWheelEvent endedGesture{localCenter, globalCenter,  {},   {}, {},
                           {},          Qt::ScrollEnd, false};
  QCoreApplication::sendEvent(chartView->viewport(), &endedGesture);
  QVERIFY(endedGesture.isAccepted());
  QWheelEvent afterEndedGesture{localCenter,       globalCenter, {},
                                {0, 120},          {},           {},
                                Qt::NoScrollPhase, false};
  QCoreApplication::sendEvent(chartView->viewport(), &afterEndedGesture);
  QVERIFY(afterEndedGesture.isAccepted());
  const auto afterReset = view.visibleSlots();
  QVERIFY(afterReset.end - afterReset.first < reset.end - reset.first);
}

} // namespace

QTEST_MAIN(QtChartsCandlestickViewTest)

#include "UnitTest_QtChartsCandlestickView.moc"
