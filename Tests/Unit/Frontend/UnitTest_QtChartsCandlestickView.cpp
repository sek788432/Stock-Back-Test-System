#include "Bte/Frontend/QtChartsCandlestickView.h"

#include <QChart>
#include <QChartView>
#include <QCoreApplication>
#include <QEvent>
#include <QGraphicsItem>
#include <QGraphicsLineItem>
#include <QGraphicsScene>
#include <QMouseEvent>
#include <QPoint>
#include <QTest>
#include <QValueAxis>
#include <QWheelEvent>

#include <algorithm>
#include <array>
#include <chrono>

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
  void setBarWindowReplacesCandles();
  void appendBarAddsOneCandle();
  void invalidBarIsIgnoredAndMarkersCanBeCleared();
  void zoomControlsAreSafeToCall();
  void pointerInteractionPansAndTogglesCrosshair();
  void wheelInteractionZoomsInAndOut();
};

void QtChartsCandlestickViewTest::emptyWindowRendersSafely() {
  bte::frontend::QtChartsCandlestickView view;

  view.setBarWindow({});

  QCOMPARE(view.candleCount(), 0U);
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

void QtChartsCandlestickViewTest::appendBarAddsOneCandle() {
  bte::frontend::QtChartsCandlestickView view;
  const std::array bars{
      makeBar(2, 100.0, 104.0),
      makeBar(3, 104.0, 101.0),
  };

  view.setBarWindow(bars);
  view.appendBar(makeBar(4, 101.0, 108.0));

  QCOMPARE(view.candleCount(), 3U);
}

void QtChartsCandlestickViewTest::invalidBarIsIgnoredAndMarkersCanBeCleared() {
  bte::frontend::QtChartsCandlestickView view;
  auto invalid = makeBar(2, 100.0, 104.0);
  invalid.high = invalid.low - 1.0;

  view.appendBar(invalid);
  view.clearMarkers();

  QCOMPARE(view.candleCount(), 0U);
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
  view.setBarWindow(
      std::array{makeBar(2, 100.0, 104.0), makeBar(3, 104.0, 101.0)});
  view.show();
  QVERIFY(QTest::qWaitForWindowExposed(&view));
  QCoreApplication::processEvents();

  auto *chartView = chartViewport(view);
  auto *axisY = qobject_cast<QValueAxis *>(
      chartView->chart()->axes(Qt::Vertical).front());
  QVERIFY(axisY != nullptr);
  const auto initialSpan = axisY->max() - axisY->min();
  const auto localCenter =
      chartView->mapFromScene(chartView->chart()->plotArea().center());
  const auto globalCenter = chartView->viewport()->mapToGlobal(localCenter);

  QWheelEvent zoomInEvent{localCenter, globalCenter,      {},   {0, 120}, {},
                          {},          Qt::NoScrollPhase, false};
  QCoreApplication::sendEvent(chartView->viewport(), &zoomInEvent);
  QVERIFY(zoomInEvent.isAccepted());
  QVERIFY(axisY->max() - axisY->min() < initialSpan);

  QWheelEvent zoomOutEvent{localCenter, globalCenter,      {},   {0, -120}, {},
                           {},          Qt::NoScrollPhase, false};
  QCoreApplication::sendEvent(chartView->viewport(), &zoomOutEvent);
  QVERIFY(zoomOutEvent.isAccepted());
  QVERIFY(axisY->max() - axisY->min() >= initialSpan * 0.99);
}

} // namespace

QTEST_MAIN(QtChartsCandlestickViewTest)

#include "UnitTest_QtChartsCandlestickView.moc"
