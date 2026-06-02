#include "Bte/Frontend/ReplaySessionVm.h"

#include <QSignalSpy>
#include <QTest>

#include <chrono>
#include <limits>
#include <vector>

namespace {

bte::core::Timestamp makeTimestamp(const int day) {
    using namespace std::chrono;
    return bte::core::Timestamp{sys_days{year{2024} / 1 / day}};
}

bte::core::Bar makeBar(const int day) {
    return {
        .ts = makeTimestamp(day),
        .open = 100.0 + day,
        .high = 103.0 + day,
        .low = 99.0 + day,
        .close = 102.0 + day,
        .volume = 1'000'000.0,
    };
}

std::vector<bte::core::Bar> makeBars() {
    return {
        makeBar(2),
        makeBar(3),
        makeBar(4),
    };
}

class ReplaySessionVmTest final : public QObject {
    Q_OBJECT

  private slots:
    void stepEmitsOneBarUpdate();
    void playEmitsMultipleBarUpdates();
    void pauseStopsUpdates();
    void speedMultiplierChangesTickRate();
    void endOfStreamStopsPlaybackSafely();
};

void ReplaySessionVmTest::stepEmitsOneBarUpdate() {
    bte::frontend::ReplaySessionVm vm;
    int appendedCount = 0;
    QObject::connect(&vm, &bte::frontend::ReplaySessionVm::barAppended, &vm,
                     [&appendedCount](const bte::core::Bar&) { ++appendedCount; });
    vm.setBars(makeBars());

    vm.step();

    QCOMPARE(appendedCount, 1);
    QCOMPARE(vm.currentIndex(), 1);
}

void ReplaySessionVmTest::playEmitsMultipleBarUpdates() {
    bte::frontend::ReplaySessionVm vm;
    int appendedCount = 0;
    QObject::connect(&vm, &bte::frontend::ReplaySessionVm::barAppended, &vm,
                     [&appendedCount](const bte::core::Bar&) { ++appendedCount; });
    vm.setBars(makeBars());
    vm.setSpeedMultiplier(std::numeric_limits<double>::infinity());

    vm.play();
    QTRY_COMPARE_WITH_TIMEOUT(appendedCount, 3, 100);

    QVERIFY(vm.isCompleted());
    QVERIFY(!vm.isPlaying());
}

void ReplaySessionVmTest::pauseStopsUpdates() {
    bte::frontend::ReplaySessionVm vm;
    int appendedCount = 0;
    QObject::connect(&vm, &bte::frontend::ReplaySessionVm::barAppended, &vm,
                     [&appendedCount](const bte::core::Bar&) { ++appendedCount; });
    vm.setBars(makeBars());

    vm.play();
    vm.pause();
    QTest::qWait(1200);

    QCOMPARE(appendedCount, 0);
    QVERIFY(!vm.isPlaying());
}

void ReplaySessionVmTest::speedMultiplierChangesTickRate() {
    bte::frontend::ReplaySessionVm vm;
    vm.setBars(makeBars());

    vm.setSpeedMultiplier(5.0);

    QCOMPARE(vm.speedMultiplier(), 5.0);
}

void ReplaySessionVmTest::endOfStreamStopsPlaybackSafely() {
    bte::frontend::ReplaySessionVm vm;
    QSignalSpy completedSpy{&vm, &bte::frontend::ReplaySessionVm::completed};
    vm.setBars({makeBar(2)});

    vm.step();
    vm.step();

    QCOMPARE(completedSpy.count(), 1);
    QVERIFY(vm.isCompleted());
    QVERIFY(!vm.isPlaying());
}

} // namespace

QTEST_MAIN(ReplaySessionVmTest)

#include "UnitTest_ReplaySessionVm.moc"
