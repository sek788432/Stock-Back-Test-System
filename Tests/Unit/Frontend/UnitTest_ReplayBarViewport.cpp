#include "Bte/Frontend/ReplayBarViewport.h"

#include <QTest>

#include <limits>

namespace {

class ReplayBarViewportTest final : public QObject {
  Q_OBJECT

private slots:
  void longPlaybackKeepsReadableFollowingWindow();
  void defaultDensityKeepsLargeCandlesAtLongPlaybackTail();
  void panReviewsHistoryWhileReplayAdvances();
  void zoomKeepsPointerAnchorAndFuturePlaybackFollowsLatest();
  void fitRejectsUnreadablePrefixWithoutChangingNavigation();
  void resizeKeepsReviewedCenterAndClampsToReachedBars();
};

void ReplayBarViewportTest::longPlaybackKeepsReadableFollowingWindow() {
  bte::frontend::ReplayBarViewport viewport;
  viewport.setPlotWidth(900.0);
  viewport.setReachedCount(23'777);

  const auto visible = viewport.visibleSlots();
  QVERIFY(viewport.isFollowingLatest());
  QCOMPARE(visible.end, 23'777U);
  QVERIFY(visible.first > 23'600U);
  QVERIFY(viewport.slotWidth() >= 22.0);
  QVERIFY(viewport.slotWidth() <= 26.0);
}

void ReplayBarViewportTest::
    defaultDensityKeepsLargeCandlesAtLongPlaybackTail() {
  bte::frontend::ReplayBarViewport viewport;
  viewport.setPlotWidth(900.0);
  viewport.setReachedCount(23'777);

  const auto visible = viewport.visibleSlots();
  QCOMPARE(visible.end, 23'777U);
  QVERIFY(visible.end - visible.first >= 36U);
  QVERIFY(visible.end - visible.first <= 40U);
  QVERIFY(viewport.slotWidth() >= 22.0);
  QVERIFY(viewport.slotWidth() <= 26.0);
}

void ReplayBarViewportTest::panReviewsHistoryWhileReplayAdvances() {
  bte::frontend::ReplayBarViewport viewport;
  viewport.setPlotWidth(240.0);
  viewport.setReachedCount(20);
  QCOMPARE(viewport.visibleSlots().first, 10U);
  QCOMPARE(viewport.visibleSlots().end, 20U);

  viewport.pan(-5.0);
  QVERIFY(!viewport.isFollowingLatest());
  QCOMPARE(viewport.visibleSlots().first, 5U);
  QCOMPARE(viewport.visibleSlots().end, 15U);

  viewport.setReachedCount(21);
  QCOMPARE(viewport.visibleSlots().first, 5U);
  QCOMPARE(viewport.visibleSlots().end, 15U);

  viewport.followLatest();
  QVERIFY(viewport.isFollowingLatest());
  QCOMPARE(viewport.visibleSlots().first, 11U);
  QCOMPARE(viewport.visibleSlots().end, 21U);
}

void ReplayBarViewportTest::
    zoomKeepsPointerAnchorAndFuturePlaybackFollowsLatest() {
  bte::frontend::ReplayBarViewport viewport;
  viewport.setPlotWidth(240.0);
  viewport.setReachedCount(30);
  viewport.zoom(2.0, 0.5);

  QCOMPARE(viewport.slotWidth(), 48.0);
  QCOMPARE(viewport.firstSlot(), 22.5);
  QCOMPARE(viewport.slotCapacity(), 5.0);
  QCOMPARE(viewport.visibleSlots().first, 22U);
  QCOMPARE(viewport.visibleSlots().end, 28U);
  QVERIFY(viewport.isFollowingLatest());

  viewport.setReachedCount(31);
  QCOMPARE(viewport.visibleSlots().first, 26U);
  QCOMPARE(viewport.visibleSlots().end, 31U);

  viewport.zoom(0.0, 0.5);
  viewport.zoom(-1.0, 0.5);
  viewport.zoom(std::numeric_limits<double>::infinity(), 0.5);
  QCOMPARE(viewport.slotWidth(), 48.0);

  viewport.pan(-3.0);
  viewport.reset();
  QCOMPARE(viewport.slotWidth(), 24.0);
  QVERIFY(viewport.isFollowingLatest());
  QCOMPARE(viewport.visibleSlots().first, 21U);
  QCOMPARE(viewport.visibleSlots().end, 31U);
}

void ReplayBarViewportTest::
    fitRejectsUnreadablePrefixWithoutChangingNavigation() {
  bte::frontend::ReplayBarViewport viewport;
  viewport.setPlotWidth(140.0);
  QVERIFY(!viewport.canFitReachedBars());
  QVERIFY(!viewport.fitReachedBars());
  QCOMPARE(viewport.visibleSlots().end, 0U);

  viewport.setReachedCount(30);
  QVERIFY(!viewport.canFitReachedBars());
  QVERIFY(!viewport.fitReachedBars());
  QCOMPARE(viewport.slotWidth(), 24.0);
  QCOMPARE(viewport.visibleSlots().first, 24U);

  viewport.setReachedCount(10);
  QVERIFY(viewport.canFitReachedBars());
  QVERIFY(viewport.fitReachedBars());
  QCOMPARE(viewport.visibleSlots().first, 0U);
  QCOMPARE(viewport.visibleSlots().end, 10U);
  QVERIFY(viewport.slotWidth() >= 6.0);
}

void ReplayBarViewportTest::resizeKeepsReviewedCenterAndClampsToReachedBars() {
  bte::frontend::ReplayBarViewport viewport;
  viewport.setPlotWidth(240.0);
  viewport.setReachedCount(30);
  viewport.pan(-10.0);
  QCOMPARE(viewport.visibleSlots().first, 10U);
  QCOMPARE(viewport.visibleSlots().end, 20U);

  viewport.setPlotWidth(480.0);
  QCOMPARE(viewport.visibleSlots().first, 5U);
  QCOMPARE(viewport.visibleSlots().end, 25U);
  QVERIFY(!viewport.isFollowingLatest());

  viewport.setReachedCount(6);
  QCOMPARE(viewport.visibleSlots().first, 0U);
  QCOMPARE(viewport.visibleSlots().end, 6U);
}

} // namespace

QTEST_MAIN(ReplayBarViewportTest)
#include "UnitTest_ReplayBarViewport.moc"
