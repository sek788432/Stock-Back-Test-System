#include "Bte/Frontend/ReplayTab.h"

#include "Bte/Frontend/QtChartsCandlestickView.h"

#include <QComboBox>
#include <QDateEdit>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QTest>
#include <QToolButton>

namespace {

class ReplayTabTest final : public QObject {
    Q_OBJECT

  private slots:
    void exposesReplaySetupControls();
    void exposesPlaybackControls();
    void exposesChartAndPortfolioPlaceholders();
    void stepButtonAppendsOneCandle();
};

void ReplayTabTest::exposesReplaySetupControls() {
    const bte::frontend::ReplayTab tab;

    QVERIFY(tab.findChild<QComboBox*>("replaySymbolCombo") != nullptr);
    QVERIFY(tab.findChild<QComboBox*>("replaySchemaCombo") != nullptr);
    QVERIFY(tab.findChild<QDateEdit*>("replayStartDateEdit") != nullptr);
    QVERIFY(tab.findChild<QDateEdit*>("replayEndDateEdit") != nullptr);
    QVERIFY(tab.findChild<QDoubleSpinBox*>("replayInitialCapitalSpinBox") != nullptr);
    QVERIFY(tab.findChild<QPushButton*>("replayLoadButton") != nullptr);
}

void ReplayTabTest::exposesPlaybackControls() {
    const bte::frontend::ReplayTab tab;

    QVERIFY(tab.findChild<QToolButton*>("replayStepBackButton") != nullptr);
    QVERIFY(tab.findChild<QToolButton*>("replayPlayPauseButton") != nullptr);
    QVERIFY(tab.findChild<QToolButton*>("replayStepForwardButton") != nullptr);

    const auto* speedCombo = tab.findChild<QComboBox*>("replaySpeedCombo");
    QVERIFY(speedCombo != nullptr);
    QCOMPARE(speedCombo->count(), 4);
    QCOMPARE(speedCombo->itemText(0), QString{"1x"});
    QCOMPARE(speedCombo->itemText(1), QString{"5x"});
    QCOMPARE(speedCombo->itemText(2), QString{"10x"});
    QCOMPARE(speedCombo->itemText(3), QString{"max"});

    QVERIFY(tab.findChild<QProgressBar*>("replayProgressBar") != nullptr);
}

void ReplayTabTest::exposesChartAndPortfolioPlaceholders() {
    const bte::frontend::ReplayTab tab;

    QVERIFY(tab.findChild<QWidget*>("replayChartPanel") != nullptr);
    const auto* chartView = tab.findChild<bte::frontend::QtChartsCandlestickView*>("replayCandlestickChartView");
    QVERIFY(chartView != nullptr);
    QCOMPARE(chartView->candleCount(), 0U);
    QVERIFY(tab.findChild<QLabel*>("replayVolumePlaceholder") != nullptr);
    QVERIFY(tab.findChild<QLabel*>("replayCashLabel") != nullptr);
    QVERIFY(tab.findChild<QLabel*>("replayPositionLabel") != nullptr);
    QVERIFY(tab.findChild<QLabel*>("replayEquityLabel") != nullptr);
    QVERIFY(tab.findChild<QLabel*>("replayPnlLabel") != nullptr);
}

void ReplayTabTest::stepButtonAppendsOneCandle() {
    bte::frontend::ReplayTab tab;
    auto* stepButton = tab.findChild<QToolButton*>("replayStepForwardButton");
    auto* chartView = tab.findChild<bte::frontend::QtChartsCandlestickView*>("replayCandlestickChartView");
    QVERIFY(stepButton != nullptr);
    QVERIFY(chartView != nullptr);

    QTest::mouseClick(stepButton, Qt::LeftButton);

    QCOMPARE(chartView->candleCount(), 1U);
}

} // namespace

QTEST_MAIN(ReplayTabTest)

#include "UnitTest_ReplayTab.moc"
