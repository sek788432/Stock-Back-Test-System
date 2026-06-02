#include "Bte/Frontend/QtChartsCandlestickView.h"

#include <QTest>

#include <array>
#include <chrono>

namespace {

bte::core::Timestamp makeTimestamp(const int day) {
    using namespace std::chrono;
    return bte::core::Timestamp{sys_days{year{2024} / 1 / day}};
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

class QtChartsCandlestickViewTest final : public QObject {
    Q_OBJECT

  private slots:
    void emptyWindowRendersSafely();
    void setBarWindowReplacesCandles();
    void appendBarAddsOneCandle();
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

} // namespace

QTEST_MAIN(QtChartsCandlestickViewTest)

#include "UnitTest_QtChartsCandlestickView.moc"
