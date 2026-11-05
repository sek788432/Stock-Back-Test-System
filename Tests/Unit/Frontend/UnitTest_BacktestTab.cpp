#include "Bte/Frontend/BacktestTab.h"
#include "Bte/Frontend/StrategyTab.h"

#include <Bte/Bindings/ResultReplay.h>
#include <Bte/Data/ReleaseSnapshot.h>

#include "WaitUntil.h"

#include <QAbstractButton>
#include <QApplication>
#include <QByteArray>
#include <QCalendarWidget>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDateEdit>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMetaObject>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QSettings>
#include <QSignalSpy>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>
#include <QTimer>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace {

class ScopedEnvironmentRestore final {
public:
  explicit ScopedEnvironmentRestore(QByteArray name)
      : name_{std::move(name)},
        previous_{qEnvironmentVariableIsSet(name_.constData())
                      ? std::optional<QByteArray>{qgetenv(name_.constData())}
                      : std::nullopt} {}

  ~ScopedEnvironmentRestore() {
    if (previous_.has_value()) {
      qputenv(name_.constData(), *previous_);
    } else {
      qunsetenv(name_.constData());
    }
  }

  ScopedEnvironmentRestore(const ScopedEnvironmentRestore &) = delete;
  ScopedEnvironmentRestore &
  operator=(const ScopedEnvironmentRestore &) = delete;
  ScopedEnvironmentRestore(ScopedEnvironmentRestore &&) = delete;
  ScopedEnvironmentRestore &operator=(ScopedEnvironmentRestore &&) = delete;

private:
  QByteArray name_;
  std::optional<QByteArray> previous_;
};

bte::bindings::ResultCatalogSnapshot
draftCatalog(const std::vector<std::string> &ids) {
  bte::bindings::ResultCatalogSnapshot snapshot;
  for (const auto &id : ids) {
    snapshot.entries.emplace_back();
    snapshot.entries.back().resultId = id;
  }
  return snapshot;
}

bte::bindings::BacktestSnapshot filledSnapshot() {
  return bte::bindings::BacktestSnapshot{
      .outcome = bte::bindings::BacktestOutcome::filled,
      .fill =
          bte::bindings::BacktestFillSnapshot{
              .timestamp = bte::core::Timestamp{std::chrono::sys_days{
                  std::chrono::year{2024} / 1 / 3}},
              .quantityShares = 10,
              .price = 110.011,
              .amount = 1'100.11,
          },
      .fills = {bte::bindings::BacktestFillSnapshot{
          .timestamp = bte::core::Timestamp{std::chrono::sys_days{
              std::chrono::year{2024} / 1 / 3}},
          .quantityShares = 10,
          .price = 110.011,
          .amount = 1'100.11,
      }},
      .initialCapital = 2'000.0,
      .cash = 899.89,
      .marketValue = 1'200.0,
      .equity = 2'099.89,
      .pnl = 99.89,
      .finalPrice = 120.0,
      .positionShares = 10,
      .barsProcessed = 2,
      .resultId = {},
      .canonicalResultHash = {},
      .terminalReason = {},
      .strategyDiagnostics = {},
  };
}

bte::bindings::BacktestSnapshot
noFillSnapshot(const bte::bindings::BacktestOutcome outcome) {
  return bte::bindings::BacktestSnapshot{
      .outcome = outcome,
      .fill = {},
      .fills = {},
      .initialCapital = 100.0,
      .cash = 100.0,
      .marketValue = 0.0,
      .equity = 100.0,
      .pnl = 0.0,
      .finalPrice = 200.0,
      .positionShares = 0,
      .barsProcessed = 2,
      .resultId = {},
      .canonicalResultHash = {},
      .terminalReason = {},
      .strategyDiagnostics = {},
  };
}

struct PreviousDataDayControls final {
  QComboBox *strategy;
  QComboBox *metric;
  QComboBox *comparison;
  QDoubleSpinBox *threshold;
  QSpinBox *period;
  QPushButton *run;
};

PreviousDataDayControls
previousDataDayControls(bte::frontend::BacktestTab &tab) {
  return {
      .strategy = tab.findChild<QComboBox *>("backtestStrategyCombo"),
      .metric = tab.findChild<QComboBox *>("backtestBuyMetricCombo"),
      .comparison = tab.findChild<QComboBox *>("backtestBuyComparisonCombo"),
      .threshold =
          tab.findChild<QDoubleSpinBox *>("backtestBuyThresholdSpinBox"),
      .period = tab.findChild<QSpinBox *>("backtestBuyPeriodSpinBox"),
      .run = tab.findChild<QPushButton *>("backtestRunButton"),
  };
}

bool previousDataDayControlsArePresent(
    const PreviousDataDayControls &controls) {
  return controls.strategy != nullptr && controls.metric != nullptr &&
         controls.comparison != nullptr && controls.threshold != nullptr &&
         controls.period != nullptr && controls.run != nullptr;
}

bool previousDataDayUiContractIsVisible(
    const PreviousDataDayControls &controls) {
  const auto expectedHelp = std::array{
      QString{"CSV"},
      QString{"UTC data day"},
      QString{"extended-hours"},
      QString{"current UTC date"},
      QString{"inclusive"},
      QString{"missing history"},
  };
  const auto descriptions = std::array{
      controls.metric->toolTip(),
      controls.metric->accessibleDescription(),
      controls.threshold->toolTip(),
      controls.threshold->accessibleDescription(),
  };
  return controls.comparison->count() == 2 &&
         controls.comparison->itemText(0) == "Down at least" &&
         controls.comparison->itemText(1) == "Up at least" &&
         controls.threshold->minimum() == 0.0001 &&
         std::ranges::all_of(descriptions,
                             [&expectedHelp](const auto &help) {
                               return std::ranges::all_of(
                                   expectedHelp, [&help](const auto &phrase) {
                                     return help.contains(phrase,
                                                          Qt::CaseInsensitive);
                                   });
                             }) &&
         controls.period->toolTip().contains("UTC data days, not bars",
                                             Qt::CaseInsensitive) &&
         controls.period->accessibleDescription().contains(
             "UTC data days, not bars", Qt::CaseInsensitive);
}

bool runBacktestAndWait(QPushButton *run, std::atomic_int &runCount,
                        const int expectedRunCount) {
  QTest::mouseClick(run, Qt::LeftButton);
  return bte::test::waitUntil([&runCount, run, expectedRunCount] {
    return runCount.load() == expectedRunCount && run->isEnabled();
  });
}

bool capturedPreviousDataDayConditionMatches(
    std::mutex &captureMutex,
    const std::optional<bte::bindings::BacktestConfiguration> &captured,
    const bte::strategy::Comparison expectedComparison,
    const double expectedThreshold) {
  const std::scoped_lock lock{captureMutex};
  if (!captured.has_value() || !captured->selectableStrategy.has_value() ||
      captured->selectableStrategy->buy.conditions.size() != 1U) {
    return false;
  }
  const auto &condition = captured->selectableStrategy->buy.conditions[0];
  return condition.source == bte::strategy::ConditionSource::
                                 previousDataDayCloseChangePercent &&
         condition.comparison == expectedComparison &&
         condition.threshold == expectedThreshold &&
         condition.thresholdDomain == bte::indicators::NumericDomain::percent;
}

bool legacyConditionEditorIsRestored(const PreviousDataDayControls &controls) {
  return controls.comparison->count() == 6 &&
         controls.threshold->minimum() == -1'000'000'000.0;
}

class BacktestTabTest final : public QObject {
  Q_OBJECT

private slots:
  void initTestCase();
  void exposesAccessibleRunConfiguration();
  void completedDraftEnablesSaveActionsAndReplayUsesPublishedId();
  void invalidDisplayNameDoesNotStartResultPublication();
  void savingOneOfMultipleDraftsKeepsReplayOnThatResult();
  void saveFailureKeepsDraftAvailableAndNeverNavigates();
  void draftWorkerExceptionsRemainVisibleAndRetryable();
#if defined(BTE_TEST_MANAGED_RUNTIME_PROFILE_PATH) &&                          \
    defined(BTE_PYTHON_WORKER_BOOTSTRAP_PATH)
  void managedPythonRunCanBeSavedAndReplayedThroughTheGui();
#endif
  void nonPersistedTestConstructorsRejectSaveAndDiscard();
  void draftActionsIgnoreUnavailableAndReentrantSignals();
  void unsavedRunsLoadSurviveNewRunsAndCanBeDiscarded();
  void draftNoticesRemainSeparateFromValidRunActionsAndClearOnRefresh();
  void calendarYearsAreDirectlySelectable();
  void runExecutesOffTheUiThreadAndDisplaysFill();
  void failedRunClearsPriorResultAndPreservesError();
  void presentsRejectedAndCancelledOutcomes();
  void runSubmitsEveryVisibleConfigurationField();
  void destructionCancelsAndJoinsActiveRun();
  void workerExceptionIsPresentedAsAnError();
  void unknownWorkerExceptionIsPresentedAsAnError();
  void pythonDraftIsFrozenAndPassedToTheRunWorker();
  void pythonActionsExplainWhenNoDraftIsAvailable();
  void verificationWithoutRunnerShowsUnavailableStateAndIdleCancelIsIgnored();
  void backtestAndVerificationActionsRejectOverlap();
  void pythonBacktestRequiresConsentBeforeStarting();
  void strategyTabHandoffIsCapturedByTheBacktestRun();
  void reproducibilityVerificationReportsProgressAndCompletion();
  void reproducibilityVerificationCanBeCanceled();
  void reproducibilityVerificationShowsStatusesAndAttemptDetails();
  void verificationWorkerFailuresArePresented();
  void unknownVerificationAndRunStatusesUseFallbackLabels();
  void backtestDiagnosticsIncludeTerminalEvidence();
  void defaultRunnerExecutesTrackedBars();
  void applicationConfiguredRunnerUsesEnvironmentStorage();
  void applicationConfiguredRunnerWiresPersistedVerification();
  void selectableConditionsSubmitTypedPlanToBackend();
  void selectableMetricsSubmitTypedPlansToBackend();
  void previousDataDayCloseMetricUsesPositiveDownUpAndExplainsUtcSource();
  void selectableMetricUnitsDescribeEveryConditionRow();
  void metricLabelsDoNotRescaleSignedOrZeroThresholds();
  void selectableConditionStatusNamesItsSelectedStrategy();
  void selectableControlsPresentNoSignalAndSellFill();
};

void BacktestTabTest::initTestCase() {
  QStandardPaths::setTestModeEnabled(true);
  QCoreApplication::setOrganizationName("StockBackTestSystemTests");
  QCoreApplication::setApplicationName("BacktestTabTests");
  QSettings::setDefaultFormat(QSettings::IniFormat);
  QSettings::setPath(
      QSettings::IniFormat, QSettings::UserScope,
      QStandardPaths::writableLocation(QStandardPaths::TempLocation));
}

void BacktestTabTest::
    draftNoticesRemainSeparateFromValidRunActionsAndClearOnRefresh() {
  const auto resultId = std::string(32, 'a');
  const auto preservedId = std::string(32, 'b');
  const std::string diagnostic =
      "Legacy unleased <staging> was preserved unchanged.";
  std::atomic_int calls = 0;
  bte::frontend::BacktestTab tab{
      [](const bte::bindings::BacktestConfiguration &,
         const bte::core::CancellationToken &) {
        return bte::core::Result<bte::bindings::BacktestSnapshot>{
            filledSnapshot()};
      },
      [](const std::string &id, const std::string &,
         const bte::core::CancellationToken &) {
        return bte::core::Result<std::string>{id};
      },
      [&calls, &resultId, &preservedId,
       &diagnostic](const bte::core::CancellationToken &) {
        auto snapshot = draftCatalog({resultId});
        if (++calls == 1) {
          snapshot.notices.push_back({
              .resultId = preservedId,
              .reason = bte::bindings::ResultRecoveryReason::legacyUnleased,
              .diagnostic = diagnostic,
          });
        }
        return bte::core::Result<bte::bindings::ResultCatalogSnapshot>{
            std::move(snapshot)};
      },
      [](const std::string &, const bte::core::CancellationToken &) {
        return bte::core::Result<void>{};
      }};
  auto *notices = tab.findChild<QLabel *>("backtestRecoveryNoticesLabel");
  auto *drafts = tab.findChild<QComboBox *>("backtestUnsavedRunsCombo");
  auto *save = tab.findChild<QPushButton *>("backtestSaveResultButton");
  auto *refresh =
      tab.findChild<QPushButton *>("backtestRefreshUnsavedRunsButton");
  QVERIFY(notices != nullptr);
  QVERIFY(drafts != nullptr);
  QVERIFY(save != nullptr);
  QVERIFY(refresh != nullptr);
  QVERIFY(bte::test::waitUntil([&] {
    return !notices->isHidden() && drafts->count() == 1 && save->isEnabled();
  }));
  QVERIFY(notices->text().contains(QString::fromStdString(preservedId)));
  QVERIFY(notices->text().contains(QString::fromStdString(diagnostic)));
  QCOMPARE(notices->textFormat(), Qt::PlainText);
  QCOMPARE(drafts->currentData().toString(), QString::fromStdString(resultId));
  refresh->click();
  QVERIFY(
      bte::test::waitUntil([&] { return calls == 2 && notices->isHidden(); }));
  QVERIFY(notices->text().isEmpty());
  QCOMPARE(drafts->count(), 1);
  QVERIFY(save->isEnabled());
}

void BacktestTabTest::
    completedDraftEnablesSaveActionsAndReplayUsesPublishedId() {
  auto snapshot = filledSnapshot();
  snapshot.resultId = "0123456789abcdef0123456789abcdef";
  std::atomic_int publishCount = 0;
  std::atomic_bool namesCorrect = true;
  bte::frontend::BacktestTab tab{
      [snapshot](bte::bindings::BacktestConfiguration,
                 bte::core::CancellationToken) {
        return bte::core::Result<bte::bindings::BacktestSnapshot>{snapshot};
      },
      [&publishCount, &namesCorrect](const std::string &resultId,
                                     const std::string &displayName,
                                     bte::core::CancellationToken) {
        ++publishCount;
        if (displayName != "AAPL May 2018") {
          namesCorrect = false;
        }
        return bte::core::Result<std::string>{resultId};
      }};
  auto *run = tab.findChild<QPushButton *>("backtestRunButton");
  auto *save = tab.findChild<QPushButton *>("backtestSaveResultButton");
  auto *saveAndReplay =
      tab.findChild<QPushButton *>("backtestSaveAndReplayButton");
  auto *resultName = tab.findChild<QLineEdit *>("backtestResultNameEdit");
  QVERIFY(run != nullptr);
  QVERIFY(save != nullptr);
  QVERIFY(saveAndReplay != nullptr);
  QVERIFY(resultName != nullptr);
  QVERIFY(!save->isEnabled());
  QVERIFY(!saveAndReplay->isEnabled());
  QCOMPARE(save->text(), QString{"Save Result"});
  QCOMPARE(saveAndReplay->text(), QString{"Save && Replay"});
  QVERIFY(!save->accessibleName().isEmpty());
  QVERIFY(!saveAndReplay->accessibleName().isEmpty());
  QSignalSpy opened{&tab, &bte::frontend::BacktestTab::openResultInReplay};

  QTest::mouseClick(run, Qt::LeftButton);
  QVERIFY(bte::test::waitUntil([save, saveAndReplay] {
    return save->isEnabled() && saveAndReplay->isEnabled();
  }));
  resultName->setText("  AAPL May 2018  ");
  QTest::mouseClick(save, Qt::LeftButton);
  QVERIFY(bte::test::waitUntil([&publishCount, save] {
    return publishCount.load() == 1 && save->isEnabled();
  }));
  QCOMPARE(opened.count(), 0);

  auto *symbol = tab.findChild<QComboBox *>("backtestSymbolCombo");
  QVERIFY(symbol != nullptr);
  const auto originalSymbol = symbol->currentText();
  symbol->setCurrentIndex((symbol->currentIndex() + 1) % symbol->count());
  QVERIFY(symbol->currentText() != originalSymbol);
  QTest::mouseClick(saveAndReplay, Qt::LeftButton);
  QTest::mouseClick(saveAndReplay, Qt::LeftButton);
  QVERIFY(bte::test::waitUntil([&publishCount, &opened] {
    return publishCount.load() == 2 && opened.count() == 1;
  }));

  QCOMPARE(opened.count(), 1);
  QCOMPARE(opened.first().first().toString(),
           QString{"0123456789abcdef0123456789abcdef"});
  QCOMPARE(publishCount.load(), 2);
  QVERIFY(namesCorrect.load());
}

void BacktestTabTest::invalidDisplayNameDoesNotStartResultPublication() {
  auto snapshot = filledSnapshot();
  snapshot.resultId = "0123456789abcdef0123456789abcdef";
  std::atomic_int publishCount = 0;
  bte::frontend::BacktestTab tab{
      [snapshot](bte::bindings::BacktestConfiguration,
                 bte::core::CancellationToken) {
        return bte::core::Result<bte::bindings::BacktestSnapshot>{snapshot};
      },
      [&publishCount](const std::string &, const std::string &,
                      bte::core::CancellationToken) {
        ++publishCount;
        return bte::core::Result<std::string>{"unexpected"};
      }};
  auto *run = tab.findChild<QPushButton *>("backtestRunButton");
  auto *save = tab.findChild<QPushButton *>("backtestSaveResultButton");
  auto *name = tab.findChild<QLineEdit *>("backtestResultNameEdit");
  auto *status = tab.findChild<QLabel *>("backtestStatusLabel");
  QVERIFY(run != nullptr);
  QVERIFY(save != nullptr);
  QVERIFY(name != nullptr);
  QVERIFY(status != nullptr);

  QTest::mouseClick(run, Qt::LeftButton);
  QVERIFY(bte::test::waitUntil([save] { return save->isEnabled(); }));
  // The editor limits Unicode characters, while persistence limits UTF-8
  // bytes. Exercise the byte limit with multibyte characters under 160 chars.
  name->setText(QString(54, QChar{0x4E2D}));
  QTest::mouseClick(save, Qt::LeftButton);

  QVERIFY(status->text().contains("Result name is too long"));
  QVERIFY(save->isEnabled());
  QCOMPARE(publishCount.load(), 0);
}

void BacktestTabTest::saveFailureKeepsDraftAvailableAndNeverNavigates() {
  auto snapshot = filledSnapshot();
  snapshot.resultId = "0123456789abcdef0123456789abcdef";
  std::atomic_int runCount = 0;
  std::atomic_int publishCount = 0;
  bte::frontend::BacktestTab tab{
      [&runCount, snapshot](bte::bindings::BacktestConfiguration,
                            bte::core::CancellationToken) {
        ++runCount;
        return bte::core::Result<bte::bindings::BacktestSnapshot>{snapshot};
      },
      [&publishCount](const std::string &, const std::string &,
                      bte::core::CancellationToken) {
        ++publishCount;
        return bte::core::Result<std::string>{bte::core::makeError(
            bte::core::ErrorCode::internal, "publication failed")};
      }};
  auto *run = tab.findChild<QPushButton *>("backtestRunButton");
  auto *save = tab.findChild<QPushButton *>("backtestSaveResultButton");
  auto *saveAndReplay =
      tab.findChild<QPushButton *>("backtestSaveAndReplayButton");
  auto *status = tab.findChild<QLabel *>("backtestStatusLabel");
  QVERIFY(run != nullptr);
  QVERIFY(save != nullptr);
  QVERIFY(saveAndReplay != nullptr);
  QVERIFY(status != nullptr);
  QSignalSpy opened{&tab, &bte::frontend::BacktestTab::openResultInReplay};

  QTest::mouseClick(run, Qt::LeftButton);
  QVERIFY(bte::test::waitUntil(
      [saveAndReplay] { return saveAndReplay->isEnabled(); }));
  QTest::mouseClick(saveAndReplay, Qt::LeftButton);
  QTest::mouseClick(saveAndReplay, Qt::LeftButton);

  QVERIFY(bte::test::waitUntil([status, save, saveAndReplay] {
    return status->text().contains("Cannot save") && save->isEnabled() &&
           saveAndReplay->isEnabled();
  }));
  QCOMPARE(runCount.load(), 1);
  QCOMPARE(publishCount.load(), 1);
  QCOMPARE(opened.count(), 0);
}

void BacktestTabTest::draftWorkerExceptionsRemainVisibleAndRetryable() {
  const auto resultId = std::string{"0123456789abcdef0123456789abcdef"};
  auto snapshot = filledSnapshot();
  snapshot.resultId = resultId;
  std::atomic_int listCount = 0;
  std::atomic_int publishCount = 0;
  std::atomic_int discardCount = 0;
  bte::frontend::BacktestTab tab{
      [snapshot](bte::bindings::BacktestConfiguration,
                 bte::core::CancellationToken) {
        return bte::core::Result<bte::bindings::BacktestSnapshot>{snapshot};
      },
      [&publishCount](
          const std::string &, const std::string &,
          bte::core::CancellationToken) -> bte::core::Result<std::string> {
        if (++publishCount == 1) {
          throw std::runtime_error{"publish exception"};
        }
        throw 42;
      },
      [&listCount, resultId](bte::core::CancellationToken)
          -> bte::core::Result<bte::bindings::ResultCatalogSnapshot> {
        if (++listCount == 1) {
          throw std::runtime_error{"list exception"};
        }
        if (listCount == 2) {
          throw 42;
        }
        return draftCatalog({resultId});
      },
      [&discardCount](const std::string &,
                      bte::core::CancellationToken) -> bte::core::Result<void> {
        if (++discardCount == 1) {
          throw std::runtime_error{"discard exception"};
        }
        throw 42;
      }};
  auto *status = tab.findChild<QLabel *>("backtestStatusLabel");
  auto *refresh =
      tab.findChild<QPushButton *>("backtestRefreshUnsavedRunsButton");
  auto *drafts = tab.findChild<QComboBox *>("backtestUnsavedRunsCombo");
  auto *discard = tab.findChild<QPushButton *>("backtestDiscardDraftButton");
  auto *run = tab.findChild<QPushButton *>("backtestRunButton");
  auto *save = tab.findChild<QPushButton *>("backtestSaveResultButton");
  QVERIFY(status != nullptr);
  QVERIFY(refresh != nullptr);
  QVERIFY(drafts != nullptr);
  QVERIFY(discard != nullptr);
  QVERIFY(run != nullptr);
  QVERIFY(save != nullptr);

  QVERIFY(bte::test::waitUntil([status] {
    return status->text().contains("Cannot load unsaved runs") &&
           status->text().contains("list exception");
  }));
  QTest::mouseClick(refresh, Qt::LeftButton);
  QVERIFY(bte::test::waitUntil([status, refresh] {
    return status->text().contains("draft list worker failed") &&
           refresh->isEnabled();
  }));
  QTest::mouseClick(refresh, Qt::LeftButton);
  QVERIFY(bte::test::waitUntil([drafts] { return drafts->count() == 1; }));
  QTest::mouseClick(refresh, Qt::LeftButton);
  QVERIFY(bte::test::waitUntil([&listCount, refresh] {
    return listCount.load() == 4 && refresh->isEnabled();
  }));
  QTest::mouseClick(discard, Qt::LeftButton);
  QVERIFY(bte::test::waitUntil([status, discard] {
    return status->text().contains("Cannot discard draft") &&
           status->text().contains("discard exception") && discard->isEnabled();
  }));
  QTest::mouseClick(discard, Qt::LeftButton);
  QVERIFY(bte::test::waitUntil([status, discard] {
    return status->text().contains("draft discard worker failed") &&
           discard->isEnabled();
  }));

  QTest::mouseClick(run, Qt::LeftButton);
  QVERIFY(bte::test::waitUntil([save] { return save->isEnabled(); }));
  QTest::mouseClick(save, Qt::LeftButton);
  QVERIFY(bte::test::waitUntil([status, save] {
    return status->text().contains("Cannot save") &&
           status->text().contains("publish exception") && save->isEnabled();
  }));
  QTest::mouseClick(save, Qt::LeftButton);
  QVERIFY(bte::test::waitUntil([status, save] {
    return status->text().contains("save worker failed") && save->isEnabled();
  }));
}

void BacktestTabTest::nonPersistedTestConstructorsRejectSaveAndDiscard() {
  auto snapshot = filledSnapshot();
  snapshot.resultId = "0123456789abcdef0123456789abcdef";
  const auto runner = [snapshot](bte::bindings::BacktestConfiguration,
                                 bte::core::CancellationToken) {
    return bte::core::Result<bte::bindings::BacktestSnapshot>{snapshot};
  };
  bte::frontend::BacktestTab unsaved{runner};
  auto *run = unsaved.findChild<QPushButton *>("backtestRunButton");
  auto *save = unsaved.findChild<QPushButton *>("backtestSaveResultButton");
  auto *status = unsaved.findChild<QLabel *>("backtestStatusLabel");
  QVERIFY(run != nullptr);
  QVERIFY(save != nullptr);
  QVERIFY(status != nullptr);
  QTest::mouseClick(run, Qt::LeftButton);
  QVERIFY(bte::test::waitUntil([save] { return save->isEnabled(); }));
  QTest::mouseClick(save, Qt::LeftButton);
  QVERIFY(bte::test::waitUntil([status] {
    return status->text().contains(
        "Result saving requires persisted Backtest storage");
  }));

  bte::frontend::BacktestTab noDraftStorage{
      runner, [](const std::string &resultId, const std::string &,
                 bte::core::CancellationToken) {
        return bte::core::Result<std::string>{resultId};
      }};
  auto *drafts =
      noDraftStorage.findChild<QComboBox *>("backtestUnsavedRunsCombo");
  auto *discard =
      noDraftStorage.findChild<QPushButton *>("backtestDiscardDraftButton");
  auto *discardStatus =
      noDraftStorage.findChild<QLabel *>("backtestStatusLabel");
  QVERIFY(drafts != nullptr);
  QVERIFY(discard != nullptr);
  QVERIFY(discardStatus != nullptr);
  const auto draftId = QString::fromStdString(snapshot.resultId);
  drafts->addItem(draftId, draftId);
  QVERIFY(bte::test::waitUntil([discard] { return discard->isEnabled(); }));
  QTest::mouseClick(discard, Qt::LeftButton);
  QVERIFY(bte::test::waitUntil([discardStatus] {
    return discardStatus->text().contains(
        "Draft discard requires persisted Backtest storage");
  }));
}

void BacktestTabTest::draftActionsIgnoreUnavailableAndReentrantSignals() {
  std::promise<void> allowList;
  auto listGate = allowList.get_future().share();
  std::atomic_int listCalls = 0;
  std::atomic_int publishCalls = 0;
  std::atomic_int discardCalls = 0;
  bte::frontend::BacktestTab tab{
      [](bte::bindings::BacktestConfiguration, bte::core::CancellationToken) {
        return bte::core::Result<bte::bindings::BacktestSnapshot>{
            filledSnapshot()};
      },
      [&publishCalls](const std::string &resultId, const std::string &,
                      bte::core::CancellationToken) {
        ++publishCalls;
        return bte::core::Result<std::string>{resultId};
      },
      [&listCalls, listGate](bte::core::CancellationToken) {
        ++listCalls;
        listGate.wait();
        return bte::core::Result<bte::bindings::ResultCatalogSnapshot>{
            draftCatalog({})};
      },
      [&discardCalls](const std::string &, bte::core::CancellationToken) {
        ++discardCalls;
        return bte::core::Result<void>{};
      }};
  auto *refresh =
      tab.findChild<QPushButton *>("backtestRefreshUnsavedRunsButton");
  auto *save = tab.findChild<QPushButton *>("backtestSaveResultButton");
  auto *discard = tab.findChild<QPushButton *>("backtestDiscardDraftButton");
  QVERIFY(refresh != nullptr);
  QVERIFY(save != nullptr);
  QVERIFY(discard != nullptr);
  const auto started =
      bte::test::waitUntil([&listCalls] { return listCalls.load() == 1; });
  const auto refreshSignal = QMetaObject::invokeMethod(refresh, "clicked");
  const auto saveSignal = QMetaObject::invokeMethod(save, "clicked");
  const auto discardSignal = QMetaObject::invokeMethod(discard, "clicked");
  allowList.set_value();
  QVERIFY(started);
  QVERIFY(refreshSignal);
  QVERIFY(saveSignal);
  QVERIFY(discardSignal);
  QVERIFY(bte::test::waitUntil([refresh] { return refresh->isEnabled(); }));
  QCOMPARE(listCalls.load(), 1);
  QCOMPARE(publishCalls.load(), 0);
  QCOMPARE(discardCalls.load(), 0);
}

void BacktestTabTest::savingOneOfMultipleDraftsKeepsReplayOnThatResult() {
  const auto firstId = std::string{"11111111111111111111111111111111"};
  const auto secondId = std::string{"22222222222222222222222222222222"};
  auto snapshot = filledSnapshot();
  snapshot.resultId = firstId;
  std::mutex draftsMutex;
  auto storedDrafts = std::vector<std::string>{firstId, secondId};
  auto publishedIds = std::vector<std::string>{};
  bte::frontend::BacktestTab tab{
      [snapshot](bte::bindings::BacktestConfiguration,
                 bte::core::CancellationToken) {
        return bte::core::Result<bte::bindings::BacktestSnapshot>{snapshot};
      },
      [&draftsMutex, &storedDrafts,
       &publishedIds](const std::string &resultId, const std::string &,
                      bte::core::CancellationToken) {
        const std::scoped_lock lock{draftsMutex};
        publishedIds.push_back(resultId);
        std::erase(storedDrafts, resultId);
        return bte::core::Result<std::string>{resultId};
      },
      [&draftsMutex, &storedDrafts](bte::core::CancellationToken) {
        const std::scoped_lock lock{draftsMutex};
        return bte::core::Result<bte::bindings::ResultCatalogSnapshot>{
            draftCatalog(storedDrafts)};
      },
      [](const std::string &, bte::core::CancellationToken) {
        return bte::core::Result<void>{};
      }};
  auto *drafts = tab.findChild<QComboBox *>("backtestUnsavedRunsCombo");
  auto *run = tab.findChild<QPushButton *>("backtestRunButton");
  auto *save = tab.findChild<QPushButton *>("backtestSaveResultButton");
  auto *saveAndReplay =
      tab.findChild<QPushButton *>("backtestSaveAndReplayButton");
  QVERIFY(drafts != nullptr);
  QVERIFY(run != nullptr);
  QVERIFY(save != nullptr);
  QVERIFY(saveAndReplay != nullptr);
  QVERIFY(bte::test::waitUntil([drafts] { return drafts->count() == 2; }));
  QSignalSpy opened{&tab, &bte::frontend::BacktestTab::openResultInReplay};

  QTest::mouseClick(run, Qt::LeftButton);
  QVERIFY(bte::test::waitUntil([save] { return save->isEnabled(); }));
  QTest::mouseClick(save, Qt::LeftButton);
  QVERIFY(bte::test::waitUntil([drafts, &secondId] {
    return drafts->count() == 1 &&
           drafts->itemData(0).toString() == QString::fromStdString(secondId);
  }));
  QVERIFY(bte::test::waitUntil(
      [saveAndReplay] { return saveAndReplay->isEnabled(); }));
  QCOMPARE(save->property("resultId").toString(),
           QString::fromStdString(firstId));
  QTest::mouseClick(saveAndReplay, Qt::LeftButton);
  QVERIFY(bte::test::waitUntil([&opened] { return opened.count() == 1; }));
  QCOMPARE(opened.first().first().toString(), QString::fromStdString(firstId));
  const auto expected = std::vector<std::string>{firstId, firstId};
  {
    const std::scoped_lock lock{draftsMutex};
    QCOMPARE(publishedIds, expected);
  }
}

void BacktestTabTest::unsavedRunsLoadSurviveNewRunsAndCanBeDiscarded() {
  const auto firstId = std::string{"11111111111111111111111111111111"};
  const auto secondId = std::string{"22222222222222222222222222222222"};
  const auto newId = std::string{"33333333333333333333333333333333"};
  std::mutex draftsMutex;
  auto storedDrafts = std::vector<std::string>{firstId, secondId};
  auto snapshot = filledSnapshot();
  snapshot.resultId = newId;
  bte::frontend::BacktestTab tab{
      [snapshot](bte::bindings::BacktestConfiguration,
                 bte::core::CancellationToken) {
        return bte::core::Result<bte::bindings::BacktestSnapshot>{snapshot};
      },
      [](const std::string &resultId, const std::string &,
         bte::core::CancellationToken) {
        return bte::core::Result<std::string>{resultId};
      },
      [&draftsMutex, &storedDrafts](bte::core::CancellationToken) {
        const std::scoped_lock lock{draftsMutex};
        return bte::core::Result<bte::bindings::ResultCatalogSnapshot>{
            draftCatalog(storedDrafts)};
      },
      [&draftsMutex, &storedDrafts](const std::string &resultId,
                                    bte::core::CancellationToken) {
        const std::scoped_lock lock{draftsMutex};
        std::erase(storedDrafts, resultId);
        return bte::core::Result<void>{};
      }};
  auto *drafts = tab.findChild<QComboBox *>("backtestUnsavedRunsCombo");
  auto *discard = tab.findChild<QPushButton *>("backtestDiscardDraftButton");
  auto *run = tab.findChild<QPushButton *>("backtestRunButton");
  QVERIFY(drafts != nullptr);
  QVERIFY(discard != nullptr);
  QVERIFY(run != nullptr);

  QVERIFY(bte::test::waitUntil([drafts] { return drafts->count() == 2; }));
  QCOMPARE(drafts->itemData(0).toString(), QString::fromStdString(firstId));
  QCOMPARE(drafts->itemData(1).toString(), QString::fromStdString(secondId));
  QTest::mouseClick(discard, Qt::LeftButton);
  QVERIFY(bte::test::waitUntil([drafts, &secondId] {
    return drafts->count() == 1 &&
           drafts->itemData(0).toString() == QString::fromStdString(secondId);
  }));

  QTest::mouseClick(run, Qt::LeftButton);
  QVERIFY(bte::test::waitUntil([drafts, &newId] {
    return drafts->findData(QString::fromStdString(newId)) >= 0;
  }));
  QCOMPARE(drafts->count(), 2);
  QVERIFY(drafts->findData(QString::fromStdString(secondId)) >= 0);
}

void BacktestTabTest::exposesAccessibleRunConfiguration() {
  const bte::frontend::BacktestTab tab;

  const auto *strategy = tab.findChild<QComboBox *>("backtestStrategyCombo");
  const auto *run = tab.findChild<QPushButton *>("backtestRunButton");
  QVERIFY(strategy != nullptr);
  QVERIFY(run != nullptr);
  QCOMPARE(strategy->currentText(), QString{"Starter market buy"});
  QVERIFY(!run->shortcut().isEmpty());
  QVERIFY(!tab.accessibleName().isEmpty());

  const auto widgets = tab.findChildren<QWidget *>();
  for (const auto *widget : widgets) {
    if (widget->objectName().startsWith("backtest")) {
      QVERIFY2(!widget->accessibleName().isEmpty(),
               qPrintable(widget->objectName()));
    }
  }
}

void BacktestTabTest::calendarYearsAreDirectlySelectable() {
  bte::frontend::BacktestTab tab;

  auto *start = tab.findChild<QDateEdit *>("backtestStartDateEdit");
  auto *startYear = tab.findChild<QComboBox *>("backtestStartYearCombo");
  auto *endYear = tab.findChild<QComboBox *>("backtestEndYearCombo");
  QVERIFY(start != nullptr);
  QVERIFY(startYear != nullptr);
  QVERIFY(endYear != nullptr);
  QVERIFY(start->calendarWidget()->isAncestorOf(startYear));
  const auto *end = tab.findChild<QDateEdit *>("backtestEndDateEdit");
  QVERIFY(end != nullptr);
  QVERIFY(end->calendarWidget()->isAncestorOf(endYear));
  QVERIFY(startYear->count() > 100);
  QCOMPARE(startYear->currentText(), QString{"2018"});
  QVERIFY(startYear->minimumWidth() >= 88);
  QVERIFY(endYear->minimumWidth() >= 88);

  startYear->setCurrentText("2035");
  QCOMPARE(start->calendarWidget()->yearShown(), 2035);

  start->setDate(QDate{2024, 2, 29});
  QCOMPARE(startYear->currentText(), QString{"2024"});

  startYear->setCurrentIndex(0);
  QCOMPARE(start->calendarWidget()->yearShown(), start->minimumDate().year());
  startYear->setCurrentIndex(startYear->count() - 1);
  QCOMPARE(start->calendarWidget()->yearShown(), start->maximumDate().year());
  startYear->setCurrentText("10000");
  QCOMPARE(start->calendarWidget()->yearShown(), start->maximumDate().year());
}

void BacktestTabTest::runExecutesOffTheUiThreadAndDisplaysFill() {
  std::atomic_bool ranOffUiThread = false;
  bte::frontend::BacktestTab tab{[&ranOffUiThread](
                                     bte::bindings::BacktestConfiguration,
                                     bte::core::CancellationToken) {
    ranOffUiThread =
        QThread::currentThread() != QCoreApplication::instance()->thread();
    return bte::core::Result<bte::bindings::BacktestSnapshot>{filledSnapshot()};
  }};

  auto *run = tab.findChild<QPushButton *>("backtestRunButton");
  auto *status = tab.findChild<QLabel *>("backtestStatusLabel");
  auto *equity = tab.findChild<QLabel *>("backtestEquityLabel");
  auto *pnl = tab.findChild<QLabel *>("backtestPnlLabel");
  auto *trades = tab.findChild<QTableWidget *>("backtestTradeLogTable");
  QVERIFY(run != nullptr);
  QVERIFY(status != nullptr);
  QVERIFY(equity != nullptr);
  QVERIFY(pnl != nullptr);
  QVERIFY(trades != nullptr);

  QTest::mouseClick(run, Qt::LeftButton);

  QVERIFY(bte::test::waitUntil(
      [&status] { return status->text().contains("Completed"); }));
  QVERIFY(ranOffUiThread);
  QVERIFY(run->isEnabled());
  QVERIFY(!equity->text().contains("--"));
  QVERIFY(!pnl->text().contains("--"));
  QCOMPARE(equity->accessibleName(), equity->text());
  QCOMPARE(status->accessibleName(), status->text());
  QCOMPARE(trades->rowCount(), 1);
}

void BacktestTabTest::failedRunClearsPriorResultAndPreservesError() {
  std::atomic_int invocation = 0;
  bte::frontend::BacktestTab tab{
      [&invocation](bte::bindings::BacktestConfiguration,
                    bte::core::CancellationToken) {
        if (invocation.fetch_add(1) == 0) {
          return bte::core::Result<bte::bindings::BacktestSnapshot>{
              filledSnapshot()};
        }
        return bte::core::Result<bte::bindings::BacktestSnapshot>{
            bte::core::makeError(bte::core::ErrorCode::notFound,
                                 "source file is missing")};
      }};

  auto *run = tab.findChild<QPushButton *>("backtestRunButton");
  auto *status = tab.findChild<QLabel *>("backtestStatusLabel");
  auto *cash = tab.findChild<QLabel *>("backtestCashLabel");
  auto *equity = tab.findChild<QLabel *>("backtestEquityLabel");
  auto *pnl = tab.findChild<QLabel *>("backtestPnlLabel");
  auto *bars = tab.findChild<QLabel *>("backtestBarsLabel");
  auto *trades = tab.findChild<QTableWidget *>("backtestTradeLogTable");
  QVERIFY(run != nullptr);
  QVERIFY(status != nullptr);
  QVERIFY(cash != nullptr);
  QVERIFY(equity != nullptr);
  QVERIFY(pnl != nullptr);
  QVERIFY(bars != nullptr);
  QVERIFY(trades != nullptr);

  QTest::mouseClick(run, Qt::LeftButton);
  QVERIFY(bte::test::waitUntil([&trades] { return trades->rowCount() == 1; }));
  QCOMPARE(trades->rowCount(), 1);
  QTest::mouseClick(run, Qt::LeftButton);

  QVERIFY(bte::test::waitUntil(
      [&status] { return status->text().contains("source file is missing"); }));
  QCOMPARE(trades->rowCount(), 0);
  QVERIFY(cash->text().contains("--"));
  QVERIFY(equity->text().contains("--"));
  QVERIFY(pnl->text().contains("--"));
  QVERIFY(bars->text().contains("--"));
}

void BacktestTabTest::presentsRejectedAndCancelledOutcomes() {
  const auto runOutcome = [](const bte::bindings::BacktestOutcome outcome,
                             const QString &expectedText) {
    bte::frontend::BacktestTab tab{
        [outcome](bte::bindings::BacktestConfiguration,
                  bte::core::CancellationToken) {
          return bte::core::Result<bte::bindings::BacktestSnapshot>{
              noFillSnapshot(outcome)};
        }};
    auto *run = tab.findChild<QPushButton *>("backtestRunButton");
    auto *status = tab.findChild<QLabel *>("backtestStatusLabel");
    auto *trades = tab.findChild<QTableWidget *>("backtestTradeLogTable");
    QVERIFY(run != nullptr);
    QVERIFY(status != nullptr);
    QVERIFY(trades != nullptr);

    QTest::mouseClick(run, Qt::LeftButton);
    QVERIFY(bte::test::waitUntil([&status, expectedText] {
      return status->text().contains(expectedText);
    }));
    QCOMPARE(trades->rowCount(), 0);
  };

  runOutcome(bte::bindings::BacktestOutcome::rejectedInsufficientCash,
             "insufficient cash");
  runOutcome(bte::bindings::BacktestOutcome::cancelledNoFutureMarketData,
             "no future market data");
}

void BacktestTabTest::runSubmitsEveryVisibleConfigurationField() {
  std::mutex captureMutex;
  std::optional<bte::bindings::BacktestConfiguration> captured;
  bte::frontend::BacktestTab tab{
      [&captureMutex, &captured](bte::bindings::BacktestConfiguration config,
                                 bte::core::CancellationToken) {
        const std::scoped_lock lock{captureMutex};
        captured = std::move(config);
        return bte::core::Result<bte::bindings::BacktestSnapshot>{
            filledSnapshot()};
      }};
  auto *symbol = tab.findChild<QComboBox *>("backtestSymbolCombo");
  auto *start = tab.findChild<QDateEdit *>("backtestStartDateEdit");
  auto *end = tab.findChild<QDateEdit *>("backtestEndDateEdit");
  auto *capital =
      tab.findChild<QDoubleSpinBox *>("backtestInitialCapitalSpinBox");
  auto *quantity = tab.findChild<QSpinBox *>("backtestQuantitySpinBox");
  auto *run = tab.findChild<QPushButton *>("backtestRunButton");
  auto *status = tab.findChild<QLabel *>("backtestStatusLabel");
  QVERIFY(symbol != nullptr);
  QVERIFY(start != nullptr);
  QVERIFY(end != nullptr);
  QVERIFY(capital != nullptr);
  QVERIFY(quantity != nullptr);
  QVERIFY(run != nullptr);
  QVERIFY(status != nullptr);

  symbol->setCurrentText("MSFT");
  start->setDate(QDate{2020, 2, 3});
  end->setDate(QDate{2021, 4, 5});
  capital->setValue(12'345.67);
  quantity->setValue(321);
  QTest::mouseClick(run, Qt::LeftButton);
  QVERIFY(bte::test::waitUntil(
      [&status] { return status->text().contains("Completed"); }));

  const std::scoped_lock lock{captureMutex};
  QVERIFY(captured.has_value());
  QCOMPARE(captured->symbol, QString{"MSFT"});
  QCOMPARE(captured->schema, QString{"ohlcv-1h"});
  QCOMPARE(captured->startDate, QDate(2020, 2, 3));
  QCOMPARE(captured->endDate, QDate(2021, 4, 5));
  QCOMPARE(captured->initialCapital, 12'345.67);
  QCOMPARE(captured->quantityShares, 321);
}

void BacktestTabTest::destructionCancelsAndJoinsActiveRun() {
  std::atomic_bool started = false;
  std::atomic_bool observedCancellation = false;
  auto tab = std::make_unique<bte::frontend::BacktestTab>(
      [&started, &observedCancellation](bte::bindings::BacktestConfiguration,
                                        bte::core::CancellationToken token) {
        started.store(true);
        while (!token.isCancellationRequested()) {
          QThread::msleep(1);
        }
        observedCancellation.store(true);
        return bte::core::Result<bte::bindings::BacktestSnapshot>{
            bte::core::makeError(bte::core::ErrorCode::cancelled,
                                 "backtest was cancelled")};
      });
  auto *run = tab->findChild<QPushButton *>("backtestRunButton");
  QVERIFY(run != nullptr);
  QTest::mouseClick(run, Qt::LeftButton);
  QVERIFY(bte::test::waitUntil([&started] { return started.load(); }));

  tab.reset();

  QVERIFY(observedCancellation.load());
}

void BacktestTabTest::workerExceptionIsPresentedAsAnError() {
  bte::frontend::BacktestTab tab{
      [](bte::bindings::BacktestConfiguration, bte::core::CancellationToken)
          -> bte::core::Result<bte::bindings::BacktestSnapshot> {
        throw std::runtime_error{"unexpected runner failure"};
      }};
  auto *run = tab.findChild<QPushButton *>("backtestRunButton");
  auto *status = tab.findChild<QLabel *>("backtestStatusLabel");
  QVERIFY(run != nullptr);
  QVERIFY(status != nullptr);

  QTest::mouseClick(run, Qt::LeftButton);

  QVERIFY(bte::test::waitUntil([&status] {
    return status->text().contains("unexpected runner failure");
  }));
  QVERIFY(run->isEnabled());
}

void BacktestTabTest::unknownWorkerExceptionIsPresentedAsAnError() {
  bte::frontend::BacktestTab tab{
      [](bte::bindings::BacktestConfiguration, bte::core::CancellationToken)
          -> bte::core::Result<bte::bindings::BacktestSnapshot> { throw 7; }};
  auto *run = tab.findChild<QPushButton *>("backtestRunButton");
  auto *status = tab.findChild<QLabel *>("backtestStatusLabel");
  QVERIFY(run != nullptr);
  QVERIFY(status != nullptr);

  QTest::mouseClick(run, Qt::LeftButton);

  QVERIFY(bte::test::waitUntil(
      [&status] { return status->text().contains("backtest worker failed"); }));
  QVERIFY(run->isEnabled());
}

void BacktestTabTest::pythonDraftIsFrozenAndPassedToTheRunWorker() {
  std::mutex captureMutex;
  std::optional<bte::bindings::BacktestConfiguration> captured;
  std::atomic_int calls = 0;
  bte::frontend::BacktestTab tab{
      [&captureMutex, &captured,
       &calls](bte::bindings::BacktestConfiguration configuration,
               bte::core::CancellationToken) {
        {
          const std::scoped_lock lock{captureMutex};
          captured = std::move(configuration);
        }
        ++calls;
        return bte::core::Result<bte::bindings::BacktestSnapshot>{
            filledSnapshot()};
      }};
  tab.setConsentProvider([](const std::string &) { return true; });
  tab.setPythonStrategyDraft("def create_strategy(params):\n    return None\n",
                             "{\"quantity\":3}");
  auto *run = tab.findChild<QPushButton *>("backtestRunButton");
  QVERIFY(run != nullptr);
  QVERIFY(run->isEnabled());
  QTest::mouseClick(run, Qt::LeftButton);
  QVERIFY(bte::test::waitUntil(
      [&calls, run] { return calls.load() == 1 && run->isEnabled(); }));
  const std::scoped_lock lock{captureMutex};
  QVERIFY(captured.has_value());
  QVERIFY(captured->pythonStrategy.has_value());
  QCOMPARE(QString::fromStdString(captured->pythonStrategy->sourceUtf8),
           QString{"def create_strategy(params):\n    return None\n"});
  QCOMPARE(
      QString::fromStdString(captured->pythonStrategy->canonicalParametersJson),
      QString{"{\"quantity\":3}"});
  QVERIFY(captured->pythonStrategy->sourceConsentGranted);
  QVERIFY(!captured->selectableStrategy.has_value());
}

void BacktestTabTest::pythonActionsExplainWhenNoDraftIsAvailable() {
  std::atomic_int runCalls = 0;
  std::atomic_int verificationCalls = 0;
  bte::frontend::BacktestTab tab{
      [&runCalls](const bte::bindings::BacktestConfiguration &,
                  const bte::core::CancellationToken &) {
        ++runCalls;
        return bte::core::Result<bte::bindings::BacktestSnapshot>{
            filledSnapshot()};
      }};
  auto *strategy = tab.findChild<QComboBox *>("backtestStrategyCombo");
  auto *run = tab.findChild<QPushButton *>("backtestRunButton");
  auto *verify =
      tab.findChild<QPushButton *>("backtestVerifyReproducibilityButton");
  auto *status = tab.findChild<QLabel *>("backtestStatusLabel");
  QVERIFY(strategy != nullptr);
  QVERIFY(run != nullptr);
  QVERIFY(verify != nullptr);
  QVERIFY(status != nullptr);

  strategy->setCurrentText("Python Strategy draft");
  QVERIFY(!verify->isHidden());
  QVERIFY(!verify->isEnabled());
  QTest::mouseClick(run, Qt::LeftButton);
  QCOMPARE(status->text(), QString{"Choose a Python Strategy draft first"});
  QCOMPARE(runCalls.load(), 0);

  tab.setVerificationRunner(
      [&verificationCalls](
          const bte::bindings::BacktestConfiguration &,
          const bte::core::CancellationToken &,
          const bte::bindings::PythonVerificationProgressObserver &) {
        ++verificationCalls;
        return bte::core::Result<bte::results::VerificationEvidence>{
            bte::results::VerificationEvidence{}};
      });
  QVERIFY(verify->isEnabled());
  QTest::mouseClick(verify, Qt::LeftButton);
  QCOMPARE(status->text(), QString{"Choose a Python Strategy draft first"});
  QCOMPARE(verificationCalls.load(), 0);
}

void BacktestTabTest::
    verificationWithoutRunnerShowsUnavailableStateAndIdleCancelIsIgnored() {
  bte::frontend::BacktestTab tab{
      [](const bte::bindings::BacktestConfiguration &,
         const bte::core::CancellationToken &) {
        return bte::core::Result<bte::bindings::BacktestSnapshot>{
            filledSnapshot()};
      }};
  tab.setPythonStrategyDraft("source", "{}");
  auto *strategy = tab.findChild<QComboBox *>("backtestStrategyCombo");
  auto *verify =
      tab.findChild<QPushButton *>("backtestVerifyReproducibilityButton");
  auto *cancel = tab.findChild<QPushButton *>("backtestCancelOperationButton");
  auto *status = tab.findChild<QLabel *>("backtestStatusLabel");
  QVERIFY(strategy != nullptr);
  QVERIFY(verify != nullptr);
  QVERIFY(cancel != nullptr);
  QVERIFY(status != nullptr);

  strategy->setCurrentIndex(2);
  verify->setEnabled(true);
  QTest::mouseClick(verify, Qt::LeftButton);
  QCOMPARE(status->text(),
           QString{"Managed reproducibility verification is unavailable"});

  cancel->setEnabled(true);
  QTest::mouseClick(cancel, Qt::LeftButton);
  QCOMPARE(status->text(),
           QString{"Managed reproducibility verification is unavailable"});
}

void BacktestTabTest::backtestAndVerificationActionsRejectOverlap() {
  std::atomic_bool allowBacktestToFinish = false;
  std::atomic_int backtestCalls = 0;
  std::atomic_int verificationCalls = 0;
  bte::frontend::BacktestTab tab{
      [&allowBacktestToFinish,
       &backtestCalls](const bte::bindings::BacktestConfiguration &,
                       const bte::core::CancellationToken &) {
        ++backtestCalls;
        while (!allowBacktestToFinish.load()) {
          QThread::msleep(1);
        }
        return bte::core::Result<bte::bindings::BacktestSnapshot>{
            filledSnapshot()};
      }};
  tab.setConsentProvider([](const std::string &) { return true; });
  tab.setPythonStrategyDraft("source", "{}");
  tab.setVerificationRunner(
      [&verificationCalls](
          const bte::bindings::BacktestConfiguration &,
          const bte::core::CancellationToken &,
          const bte::bindings::PythonVerificationProgressObserver &) {
        ++verificationCalls;
        return bte::core::Result<bte::results::VerificationEvidence>{
            bte::results::VerificationEvidence{}};
      });
  auto *strategy = tab.findChild<QComboBox *>("backtestStrategyCombo");
  auto *run = tab.findChild<QPushButton *>("backtestRunButton");
  auto *verify =
      tab.findChild<QPushButton *>("backtestVerifyReproducibilityButton");
  QVERIFY(strategy != nullptr);
  QVERIFY(run != nullptr);
  QVERIFY(verify != nullptr);
  strategy->setCurrentIndex(2);

  QTest::mouseClick(run, Qt::LeftButton);
  QVERIFY(bte::test::waitUntil(
      [&backtestCalls] { return backtestCalls.load() == 1; }));
  verify->setEnabled(true);
  QTest::mouseClick(verify, Qt::LeftButton);
  run->setEnabled(true);
  QTest::mouseClick(run, Qt::LeftButton);
  QCOMPARE(verificationCalls.load(), 0);
  QCOMPARE(backtestCalls.load(), 1);

  allowBacktestToFinish.store(true);
  QVERIFY(bte::test::waitUntil([run] { return run->isEnabled(); }));
}

void BacktestTabTest::pythonBacktestRequiresConsentBeforeStarting() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  std::atomic_int calls = 0;
  auto promptCount = std::make_shared<int>(0);
  auto informativeText = std::make_shared<QString>();
  bte::frontend::BacktestTab tab{
      [&calls](const bte::bindings::BacktestConfiguration &,
               const bte::core::CancellationToken &) {
        ++calls;
        return bte::core::Result<bte::bindings::BacktestSnapshot>{
            filledSnapshot()};
      }};
  const auto source = QString{"source-%1"}.arg(directory.path());
  tab.setPythonStrategyDraft(source, "{}");
  auto *run = tab.findChild<QPushButton *>("backtestRunButton");
  auto *status = tab.findChild<QLabel *>("backtestStatusLabel");
  QVERIFY(run != nullptr);
  QVERIFY(status != nullptr);

  const auto scheduleConsentChoice = [promptCount, informativeText](
                                         const QString &buttonText) {
    auto timer = std::make_unique<QTimer>();
    timer->setInterval(10);
    const QPointer<QTimer> timerGuard{timer.get()};
    QObject::connect(timer.get(), &QTimer::timeout, qApp,
                     [buttonText, promptCount, informativeText, timerGuard] {
                       if (timerGuard.isNull()) {
                         return;
                       }
                       auto *dialog = qobject_cast<QMessageBox *>(
                           QApplication::activeModalWidget());
                       if (dialog == nullptr) {
                         return;
                       }
                       for (auto *button : dialog->buttons()) {
                         if (button->text() == buttonText) {
                           ++*promptCount;
                           *informativeText = dialog->informativeText();
                           timerGuard->stop();
                           button->click();
                           return;
                         }
                       }
                     });
    QTimer::singleShot(5000, timer.get(), [timerGuard] {
      if (timerGuard.isNull() || !timerGuard->isActive()) {
        return;
      }
      timerGuard->stop();
      if (auto *const dialog = QApplication::activeModalWidget();
          dialog != nullptr) {
        dialog->close();
      }
    });
    timer->start();
    return timer;
  };

  auto consentTimer = scheduleConsentChoice("Cancel");
  QTest::mouseClick(run, Qt::LeftButton);
  consentTimer.reset();
  QVERIFY(bte::test::waitUntil(
      [status] { return status->text().contains("Consent was not granted"); },
      std::chrono::milliseconds{5000}));
  QCOMPARE(calls.load(), 0);
  QCOMPARE(*promptCount, 1);
  QVERIFY(informativeText->contains("API version: 1"));
  QVERIFY(informativeText->contains("Source SHA-256:"));

  consentTimer = scheduleConsentChoice("Allow this source");
  QTest::mouseClick(run, Qt::LeftButton);
  consentTimer.reset();
  const auto backtestCompleted = bte::test::waitUntil(
      [&calls, run] { return calls.load() == 1 && run->isEnabled(); });
  QVERIFY2(backtestCompleted,
           qPrintable(QString{"Backtest did not complete: calls=%1, "
                              "status='%2', consent prompts=%3"}
                          .arg(calls.load())
                          .arg(status->text())
                          .arg(*promptCount)));
  QCOMPARE(*promptCount, 2);

  consentTimer = scheduleConsentChoice("Cancel");
  QTest::mouseClick(run, Qt::LeftButton);
  consentTimer.reset();
  QVERIFY(bte::test::waitUntil(
      [&calls, run] { return calls.load() == 2 && run->isEnabled(); }));
  QCOMPARE(*promptCount, 2);

  tab.setPythonStrategyDraft(source + "-changed", "{}");
  consentTimer = scheduleConsentChoice("Cancel");
  QTest::mouseClick(run, Qt::LeftButton);
  consentTimer.reset();
  QVERIFY(bte::test::waitUntil(
      [status] { return status->text().contains("Consent was not granted"); },
      std::chrono::milliseconds{5000}));
  QCOMPARE(calls.load(), 2);
  QCOMPARE(*promptCount, 3);
}

void BacktestTabTest::strategyTabHandoffIsCapturedByTheBacktestRun() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  std::mutex captureMutex;
  std::optional<bte::bindings::BacktestConfiguration> captured;
  std::atomic_int calls = 0;
  bte::frontend::BacktestTab backtest{
      [&captureMutex, &captured,
       &calls](bte::bindings::BacktestConfiguration configuration,
               bte::core::CancellationToken) {
        {
          const std::scoped_lock lock{captureMutex};
          captured = std::move(configuration);
        }
        ++calls;
        return bte::core::Result<bte::bindings::BacktestSnapshot>{
            filledSnapshot()};
      }};
  backtest.setConsentProvider([](const std::string &) { return true; });

  bte::frontend::StrategyTab strategy{
      std::filesystem::path{directory.path().toStdString()} / "drafts",
      {},
      [](const std::string &) { return true; }};
  auto *editor =
      strategy.findChild<QPlainTextEdit *>("pythonStrategySourceEditor");
  auto *fast =
      strategy.findChild<QSpinBox *>("pythonStrategyFastPeriodSpinBox");
  auto *slow =
      strategy.findChild<QSpinBox *>("pythonStrategySlowPeriodSpinBox");
  auto *quantity =
      strategy.findChild<QSpinBox *>("pythonStrategyQuantitySpinBox");
  auto *use =
      strategy.findChild<QPushButton *>("pythonStrategyUseInBacktestButton");
  auto *run = backtest.findChild<QPushButton *>("backtestRunButton");
  auto *selectedStrategy =
      backtest.findChild<QComboBox *>("backtestStrategyCombo");
  QVERIFY(editor != nullptr);
  QVERIFY(fast != nullptr);
  QVERIFY(slow != nullptr);
  QVERIFY(quantity != nullptr);
  QVERIFY(use != nullptr);
  QVERIFY(run != nullptr);
  QVERIFY(selectedStrategy != nullptr);

  const auto source =
      QString{"def create_strategy(params):\n    return None\n"};
  editor->setPlainText(source);
  fast->setValue(7);
  slow->setValue(13);
  quantity->setValue(42);
  connect(&strategy, &bte::frontend::StrategyTab::useInBacktest, &backtest,
          [&backtest](const QString &sourceUtf8,
                      const QString &canonicalParametersJson) {
            backtest.setPythonStrategyDraft(sourceUtf8,
                                            canonicalParametersJson);
          });

  QTest::mouseClick(use, Qt::LeftButton);
  QCOMPARE(selectedStrategy->currentIndex(), 2);
  QVERIFY(run->isEnabled());
  QTest::mouseClick(run, Qt::LeftButton);
  QVERIFY(bte::test::waitUntil(
      [&calls, run] { return calls.load() == 1 && run->isEnabled(); }));

  const std::scoped_lock lock{captureMutex};
  QVERIFY(captured.has_value());
  QVERIFY(captured->pythonStrategy.has_value());
  QCOMPARE(QString::fromStdString(captured->pythonStrategy->sourceUtf8),
           source);
  QCOMPARE(
      QString::fromStdString(captured->pythonStrategy->canonicalParametersJson),
      QString{"{\"fast_period\":7,\"quantity\":42,\"slow_period\":13}"});
  QVERIFY(captured->pythonStrategy->sourceConsentGranted);
}

void BacktestTabTest::
    reproducibilityVerificationReportsProgressAndCompletion() {
  std::atomic_int calls = 0;
  std::atomic_int progress = 0;
  bte::frontend::BacktestTab tab{[](bte::bindings::BacktestConfiguration,
                                    bte::core::CancellationToken) {
    return bte::core::Result<bte::bindings::BacktestSnapshot>{filledSnapshot()};
  }};
  tab.setConsentProvider([](const std::string &) { return true; });
  tab.setPythonStrategyDraft("source", "{}");
  tab.setVerificationRunner(
      [&calls, &progress](
          const bte::bindings::BacktestConfiguration &configuration,
          const bte::core::CancellationToken &,
          const bte::bindings::PythonVerificationProgressObserver &observe) {
        ++calls;
        if (!configuration.pythonStrategy.has_value()) {
          return bte::core::Result<bte::results::VerificationEvidence>{
              bte::core::makeError(bte::core::ErrorCode::invalidArgument,
                                   "python strategy missing")};
        }
        bte::results::VerificationEvidence evidence;
        evidence.verificationId = "verification-id";
        evidence.status = bte::results::VerificationStatus::running;
        evidence.progressOrdinal = 1;
        observe(evidence);
        ++progress;
        evidence.status = bte::results::VerificationStatus::verified;
        evidence.progressOrdinal = 3;
        observe(evidence);
        ++progress;
        return bte::core::Result<bte::results::VerificationEvidence>{evidence};
      });
  auto *verify =
      tab.findChild<QPushButton *>("backtestVerifyReproducibilityButton");
  auto *run = tab.findChild<QPushButton *>("backtestRunButton");
  auto *status = tab.findChild<QLabel *>("backtestStatusLabel");
  QVERIFY(verify != nullptr);
  QVERIFY(run != nullptr);
  QVERIFY(status != nullptr);
  QVERIFY(!verify->isHidden());
  QVERIFY(verify->isEnabled());
  QTest::mouseClick(verify, Qt::LeftButton);
  QVERIFY(bte::test::waitUntil([&calls, &progress, status, run] {
    return calls.load() == 1 && progress.load() == 2 && run->isEnabled() &&
           status->text().contains("verified across 3 fresh runs");
  }));
}

void BacktestTabTest::reproducibilityVerificationCanBeCanceled() {
  std::atomic_bool started = false;
  std::atomic_bool observedCancellation = false;
  std::atomic_int verificationCalls = 0;
  bte::frontend::BacktestTab tab{[](bte::bindings::BacktestConfiguration,
                                    bte::core::CancellationToken) {
    return bte::core::Result<bte::bindings::BacktestSnapshot>{filledSnapshot()};
  }};
  tab.setConsentProvider([](const std::string &) { return true; });
  tab.setPythonStrategyDraft("source", "{}");
  tab.setVerificationRunner(
      [&started, &observedCancellation, &verificationCalls](
          const bte::bindings::BacktestConfiguration &,
          const bte::core::CancellationToken &cancellation,
          const bte::bindings::PythonVerificationProgressObserver &) {
        ++verificationCalls;
        started.store(true);
        while (!cancellation.isCancellationRequested()) {
          QThread::msleep(1);
        }
        observedCancellation.store(true);
        bte::results::VerificationEvidence evidence;
        evidence.status = bte::results::VerificationStatus::canceled;
        return bte::core::Result<bte::results::VerificationEvidence>{evidence};
      });
  auto *verify =
      tab.findChild<QPushButton *>("backtestVerifyReproducibilityButton");
  auto *cancel = tab.findChild<QPushButton *>("backtestCancelOperationButton");
  auto *status = tab.findChild<QLabel *>("backtestStatusLabel");
  auto *run = tab.findChild<QPushButton *>("backtestRunButton");
  auto *strategy = tab.findChild<QComboBox *>("backtestStrategyCombo");
  QVERIFY(verify != nullptr);
  QVERIFY(cancel != nullptr);
  QVERIFY(status != nullptr);
  QVERIFY(run != nullptr);
  QVERIFY(strategy != nullptr);
  QTest::mouseClick(verify, Qt::LeftButton);
  QVERIFY(bte::test::waitUntil([&started] { return started.load(); }));
  strategy->setCurrentIndex(0);
  strategy->setCurrentIndex(2);
  QVERIFY(!verify->isEnabled());
  QTest::mouseClick(verify, Qt::LeftButton);
  QTest::qWait(50);
  QCOMPARE(verificationCalls.load(), 1);
  QTest::mouseClick(cancel, Qt::LeftButton);
  QVERIFY(bte::test::waitUntil([&observedCancellation, status, run] {
    return observedCancellation.load() && run->isEnabled() &&
           status->text().contains("verification canceled");
  }));
}

void BacktestTabTest::
    reproducibilityVerificationShowsStatusesAndAttemptDetails() {
  const auto statuses = std::array{
      std::pair{bte::results::VerificationStatus::running,
                QString{"attempt 3 of 3"}},
      std::pair{bte::results::VerificationStatus::verified,
                QString{"verified across 3 fresh runs"}},
      std::pair{bte::results::VerificationStatus::mismatch,
                QString{"mismatch after 1 attempts"}},
      std::pair{bte::results::VerificationStatus::failed,
                QString{"failed after 0 attempts"}},
      std::pair{bte::results::VerificationStatus::canceled,
                QString{"canceled after 0 attempts"}},
      std::pair{bte::results::VerificationStatus::interrupted,
                QString{"was interrupted"}},
  };
  std::atomic_int calls = 0;
  bte::frontend::BacktestTab tab{
      [](const bte::bindings::BacktestConfiguration &,
         const bte::core::CancellationToken &) {
        return bte::core::Result<bte::bindings::BacktestSnapshot>{
            filledSnapshot()};
      }};
  tab.setConsentProvider([](const std::string &) { return true; });
  tab.setPythonStrategyDraft("source", "{}");
  tab.setVerificationRunner(
      [&calls,
       &statuses](const bte::bindings::BacktestConfiguration &,
                  const bte::core::CancellationToken &,
                  const bte::bindings::PythonVerificationProgressObserver &) {
        const auto index = static_cast<std::size_t>(calls.fetch_add(1));
        bte::results::VerificationEvidence evidence;
        evidence.verificationId = "verification-id";
        evidence.status = statuses.at(index).first;
        evidence.progressOrdinal = index == 0 ? 3U : 0U;
        if (index == 0) {
          evidence.attempts = {
              {.ordinal = 1,
               .resultId = std::string{"completed-result"},
               .terminalStatus = bte::results::RunStatus::completed,
               .canonicalResultHash = std::nullopt,
               .error = std::nullopt},
              {.ordinal = 2,
               .resultId = std::nullopt,
               .terminalStatus = bte::results::RunStatus::failed,
               .canonicalResultHash = std::nullopt,
               .error =
                   bte::results::VerificationError{
                       .code = "StrategyRuntimeError",
                       .message = "worker exited unexpectedly"}},
              {.ordinal = 3,
               .resultId = std::string{"interrupted-result"},
               .terminalStatus = bte::results::RunStatus::interrupted,
               .canonicalResultHash = std::nullopt,
               .error = std::nullopt},
          };
        } else if (index == 1) {
          evidence.attempts = {
              {.ordinal = 1,
               .resultId = std::string{"canceled-result"},
               .terminalStatus = bte::results::RunStatus::canceled,
               .canonicalResultHash = std::nullopt,
               .error = std::nullopt},
              {.ordinal = 2,
               .resultId = std::nullopt,
               .terminalStatus = bte::results::RunStatus::incomplete,
               .canonicalResultHash = std::nullopt,
               .error =
                   bte::results::VerificationError{.code = "Incomplete",
                                                   .message =
                                                       "run did not complete"}},
              {.ordinal = 3,
               .resultId = std::string{"running-result"},
               .terminalStatus = bte::results::RunStatus::running,
               .canonicalResultHash = std::nullopt,
               .error = std::nullopt},
          };
        } else if (index == 2) {
          evidence.attempts = {
              {.ordinal = 1,
               .resultId = std::nullopt,
               .terminalStatus = std::nullopt,
               .canonicalResultHash = std::nullopt,
               .error = std::nullopt},
          };
        }
        return bte::core::Result<bte::results::VerificationEvidence>{
            std::move(evidence)};
      });
  auto *verify =
      tab.findChild<QPushButton *>("backtestVerifyReproducibilityButton");
  auto *run = tab.findChild<QPushButton *>("backtestRunButton");
  auto *status = tab.findChild<QLabel *>("backtestStatusLabel");
  auto *diagnostics =
      tab.findChild<QPlainTextEdit *>("backtestStrategyDiagnostics");
  QVERIFY(verify != nullptr);
  QVERIFY(run != nullptr);
  QVERIFY(status != nullptr);
  QVERIFY(diagnostics != nullptr);

  for (auto index = std::size_t{}; index < statuses.size(); ++index) {
    QTest::mouseClick(verify, Qt::LeftButton);
    QVERIFY(bte::test::waitUntil([&calls, &statuses, index, run, status] {
      return calls.load() == static_cast<int>(index + 1) && run->isEnabled() &&
             status->text().contains(statuses.at(index).second);
    }));
    if (index == 0) {
      const auto text = diagnostics->toPlainText();
      QVERIFY(text.contains("verification-id"));
      QVERIFY(text.contains("completed-result (completed)"));
      QVERIFY(
          text.contains("StrategyRuntimeError: worker exited unexpectedly"));
      QVERIFY(text.contains("interrupted-result (interrupted)"));
    } else if (index == 1) {
      const auto text = diagnostics->toPlainText();
      QVERIFY(text.contains("canceled-result (canceled)"));
      QVERIFY(text.contains("Incomplete: run did not complete (incomplete)"));
      QVERIFY(text.contains("running-result (running)"));
    } else if (index == 2) {
      QVERIFY(diagnostics->toPlainText().contains("Attempt 1: "));
    }
  }
}

void BacktestTabTest::verificationWorkerFailuresArePresented() {
  std::atomic_int invocation = 0;
  bte::frontend::BacktestTab tab{
      [](const bte::bindings::BacktestConfiguration &,
         const bte::core::CancellationToken &) {
        return bte::core::Result<bte::bindings::BacktestSnapshot>{
            filledSnapshot()};
      }};
  tab.setConsentProvider([](const std::string &) { return true; });
  tab.setPythonStrategyDraft("source", "{}");
  tab.setVerificationRunner(
      [&invocation](const bte::bindings::BacktestConfiguration &,
                    const bte::core::CancellationToken &,
                    const bte::bindings::PythonVerificationProgressObserver &)
          -> bte::core::Result<bte::results::VerificationEvidence> {
        switch (invocation.fetch_add(1)) {
        case 0:
          return bte::core::Result<bte::results::VerificationEvidence>{
              bte::core::makeError(bte::core::ErrorCode::internal,
                                   "stored evidence unavailable")};
        case 1:
          throw std::runtime_error{"verification exception"};
        default:
          throw 42;
        }
      });
  auto *verify =
      tab.findChild<QPushButton *>("backtestVerifyReproducibilityButton");
  auto *run = tab.findChild<QPushButton *>("backtestRunButton");
  auto *status = tab.findChild<QLabel *>("backtestStatusLabel");
  QVERIFY(verify != nullptr);
  QVERIFY(run != nullptr);
  QVERIFY(status != nullptr);

  QTest::mouseClick(verify, Qt::LeftButton);
  QVERIFY(bte::test::waitUntil([&invocation, run, status] {
    return invocation.load() == 1 && run->isEnabled() &&
           status->text().contains("stored evidence unavailable");
  }));
  QTest::mouseClick(verify, Qt::LeftButton);
  QVERIFY(bte::test::waitUntil([&invocation, run, status] {
    return invocation.load() == 2 && run->isEnabled() &&
           status->text().contains("verification worker failed: "
                                   "verification exception");
  }));
  QTest::mouseClick(verify, Qt::LeftButton);
  QVERIFY(bte::test::waitUntil([&invocation, run, status] {
    return invocation.load() == 3 && run->isEnabled() &&
           status->text().contains("Verification could not complete") &&
           status->text().contains("verification worker failed");
  }));
}

void BacktestTabTest::unknownVerificationAndRunStatusesUseFallbackLabels() {
  bte::frontend::BacktestTab tab{
      [](const bte::bindings::BacktestConfiguration &,
         const bte::core::CancellationToken &) {
        return bte::core::Result<bte::bindings::BacktestSnapshot>{
            filledSnapshot()};
      }};
  tab.setConsentProvider([](const std::string &) { return true; });
  tab.setPythonStrategyDraft("source", "{\"quantity\":1}");
  tab.setVerificationRunner(
      [](const bte::bindings::BacktestConfiguration &,
         const bte::core::CancellationToken &,
         const bte::bindings::PythonVerificationProgressObserver &) {
        auto evidence = bte::results::VerificationEvidence{};
        evidence.verificationId = "unknown-status";
        evidence.status = static_cast<bte::results::VerificationStatus>(255);
        evidence.attempts = {
            {.ordinal = 1,
             .resultId = std::string{"result-with-unknown-status"},
             .terminalStatus = static_cast<bte::results::RunStatus>(255),
             .canonicalResultHash = std::nullopt,
             .error = std::nullopt},
        };
        return bte::core::Result<bte::results::VerificationEvidence>{
            std::move(evidence)};
      });
  auto *verify =
      tab.findChild<QPushButton *>("backtestVerifyReproducibilityButton");
  auto *run = tab.findChild<QPushButton *>("backtestRunButton");
  auto *status = tab.findChild<QLabel *>("backtestStatusLabel");
  auto *diagnostics =
      tab.findChild<QPlainTextEdit *>("backtestStrategyDiagnostics");
  QVERIFY(verify != nullptr);
  QVERIFY(run != nullptr);
  QVERIFY(status != nullptr);
  QVERIFY(diagnostics != nullptr);

  QTest::mouseClick(verify, Qt::LeftButton);

  QVERIFY(bte::test::waitUntil([run, status] {
    return run->isEnabled() && status->text().contains("status is unavailable");
  }));
  QVERIFY(diagnostics->toPlainText().contains(
      "result-with-unknown-status (unknown)"));
}

void BacktestTabTest::backtestDiagnosticsIncludeTerminalEvidence() {
  auto snapshot = filledSnapshot();
  snapshot.resultId = "diagnostic-result-id";
  snapshot.strategyDiagnostics = {"strategy warning", "order note"};
  snapshot.terminalReason = "Strategy stopped with a diagnostic.";
  snapshot.reproducibilityVerified = true;
  std::atomic_int calls = 0;
  bte::frontend::BacktestTab tab{
      [&calls, &snapshot](const bte::bindings::BacktestConfiguration &,
                          const bte::core::CancellationToken &) {
        ++calls;
        return bte::core::Result<bte::bindings::BacktestSnapshot>{snapshot};
      }};
  auto *run = tab.findChild<QPushButton *>("backtestRunButton");
  auto *diagnostics =
      tab.findChild<QPlainTextEdit *>("backtestStrategyDiagnostics");
  auto *drafts = tab.findChild<QComboBox *>("backtestUnsavedRunsCombo");
  QVERIFY(run != nullptr);
  QVERIFY(diagnostics != nullptr);
  QVERIFY(drafts != nullptr);

  QTest::mouseClick(run, Qt::LeftButton);
  QVERIFY(bte::test::waitUntil(
      [&calls, run] { return calls.load() == 1 && run->isEnabled(); }));
  QCOMPARE(diagnostics->toPlainText(),
           QString{"strategy warning\norder note\n"
                   "Strategy stopped with a diagnostic.\n"
                   "Reproducibility: verified"});
  QCOMPARE(drafts->count(), 1);
  QCOMPARE(drafts->currentData().toString(), QString{"diagnostic-result-id"});

  QTest::mouseClick(run, Qt::LeftButton);
  QVERIFY(bte::test::waitUntil(
      [&calls, run] { return calls.load() == 2 && run->isEnabled(); }));
  QCOMPARE(drafts->count(), 1);
  snapshot.reproducibilityVerified = false;
  snapshot.reproducibilityDiagnostic = "verification store could not be read";
  QTest::mouseClick(run, Qt::LeftButton);
  QVERIFY(bte::test::waitUntil(
      [&calls, run] { return calls.load() == 3 && run->isEnabled(); }));
  QVERIFY(diagnostics->toPlainText().contains(
      "Reproducibility status unavailable: verification store could not be "
      "read"));
  QCOMPARE(drafts->count(), 1);
}

void BacktestTabTest::defaultRunnerExecutesTrackedBars() {
  bte::frontend::BacktestTab tab;
  auto *run = tab.findChild<QPushButton *>("backtestRunButton");
  auto *status = tab.findChild<QLabel *>("backtestStatusLabel");
  auto *bars = tab.findChild<QLabel *>("backtestBarsLabel");
  QVERIFY(run != nullptr);
  QVERIFY(status != nullptr);
  QVERIFY(bars != nullptr);

  QTest::mouseClick(run, Qt::LeftButton);

  QVERIFY(bte::test::waitUntil(
      [&status] { return status->text().contains("Completed"); }));
  QVERIFY(!bars->text().contains("--"));
  QVERIFY(run->isEnabled());
}

void BacktestTabTest::applicationConfiguredRunnerUsesEnvironmentStorage() {
  const ScopedEnvironmentRestore resultStoreRestore{"BTE_RESULT_STORE"};
  const ScopedEnvironmentRestore dataStoreRestore{"BTE_DATA_STORE"};
  const ScopedEnvironmentRestore snapshotRestore{"BTE_DATA_SNAPSHOT_ID"};
  const auto configuredRoot =
      std::filesystem::temp_directory_path() / "bte-configured-backtest-tab";
  std::filesystem::remove_all(configuredRoot);
  QVERIFY(qputenv("BTE_RESULT_STORE",
                  (configuredRoot / "Results").string().c_str()));
  QVERIFY(
      qputenv("BTE_DATA_STORE", (configuredRoot / "Data").string().c_str()));
  QVERIFY(qputenv("BTE_DATA_SNAPSHOT_ID", std::string(64, 'd').c_str()));
  auto tab = bte::frontend::BacktestTab::createApplicationConfigured();
  auto *run = tab->findChild<QPushButton *>("backtestRunButton");
  auto *status = tab->findChild<QLabel *>("backtestStatusLabel");
  QVERIFY(run != nullptr);
  QVERIFY(status != nullptr);

  QTest::mouseClick(run, Qt::LeftButton);

  QVERIFY(bte::test::waitUntil([status] {
    return status->text().contains("snapshot", Qt::CaseInsensitive);
  }));
  QVERIFY(run->isEnabled());
  QVERIFY(!std::filesystem::exists(configuredRoot / "Results" / "Results"));
  std::filesystem::remove_all(configuredRoot);
}

void BacktestTabTest::applicationConfiguredRunnerWiresPersistedVerification() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const ScopedEnvironmentRestore resultStoreRestore{"BTE_RESULT_STORE"};
  const ScopedEnvironmentRestore dataStoreRestore{"BTE_DATA_STORE"};
  const ScopedEnvironmentRestore snapshotRestore{"BTE_DATA_SNAPSHOT_ID"};
  const auto root = std::filesystem::path{directory.path().toStdString()};
  QVERIFY(qputenv("BTE_RESULT_STORE", (root / "Results").string().c_str()));
  QVERIFY(qputenv("BTE_DATA_STORE", (root / "Data").string().c_str()));
  QVERIFY(qputenv("BTE_DATA_SNAPSHOT_ID", std::string(64, 'e').c_str()));
  auto tab = bte::frontend::BacktestTab::createApplicationConfigured();
  tab->setConsentProvider([](const std::string &) { return true; });
  tab->setPythonStrategyDraft("def create_strategy(params):\n    return None\n",
                              "{\"quantity\":1}");
  auto *verify =
      tab->findChild<QPushButton *>("backtestVerifyReproducibilityButton");
  auto *run = tab->findChild<QPushButton *>("backtestRunButton");
  auto *status = tab->findChild<QLabel *>("backtestStatusLabel");
  QVERIFY(verify != nullptr);
  QVERIFY(run != nullptr);
  QVERIFY(status != nullptr);
  QVERIFY(verify->isEnabled());

  QTest::mouseClick(verify, Qt::LeftButton);

  QVERIFY(bte::test::waitUntil([run, status] {
    return run->isEnabled() &&
           status->text().startsWith("Verification could not complete");
  }));
}

void BacktestTabTest::selectableConditionsSubmitTypedPlanToBackend() {
  std::mutex captureMutex;
  std::optional<bte::bindings::BacktestConfiguration> captured;
  bte::frontend::BacktestTab tab{
      [&captureMutex,
       &captured](bte::bindings::BacktestConfiguration configuration,
                  bte::core::CancellationToken) {
        const std::scoped_lock lock{captureMutex};
        captured = std::move(configuration);
        return bte::core::Result<bte::bindings::BacktestSnapshot>{
            filledSnapshot()};
      }};
  auto *strategy = tab.findChild<QComboBox *>("backtestStrategyCombo");
  auto *buyLogic = tab.findChild<QComboBox *>("backtestBuyLogicCombo");
  auto *buyMetric = tab.findChild<QComboBox *>("backtestBuyMetricCombo");
  auto *buyPeriod = tab.findChild<QSpinBox *>("backtestBuyPeriodSpinBox");
  auto *buyThreshold =
      tab.findChild<QDoubleSpinBox *>("backtestBuyThresholdSpinBox");
  auto *buySecond =
      tab.findChild<QCheckBox *>("backtestBuySecondEnabledCheckBox");
  auto *buySecondMetric =
      tab.findChild<QComboBox *>("backtestBuySecondMetricCombo");
  auto *run = tab.findChild<QPushButton *>("backtestRunButton");
  auto *status = tab.findChild<QLabel *>("backtestStatusLabel");
  QVERIFY(strategy != nullptr);
  QVERIFY(buyLogic != nullptr);
  QVERIFY(buyMetric != nullptr);
  QVERIFY(buyPeriod != nullptr);
  QVERIFY(buyThreshold != nullptr);
  QVERIFY(buySecond != nullptr);
  QVERIFY(buySecondMetric != nullptr);
  QVERIFY(run != nullptr);
  QVERIFY(status != nullptr);

  strategy->setCurrentText("Selectable conditions");
  buyLogic->setCurrentIndex(1);
  buyMetric->setCurrentText("MACD histogram");
  buyPeriod->setValue(16);
  buyThreshold->setValue(60.0);
  buySecond->setChecked(true);
  buySecondMetric->setCurrentText("Close change %");
  QTest::mouseClick(run, Qt::LeftButton);
  QVERIFY(bte::test::waitUntil(
      [&status] { return status->text().contains("Completed"); }));

  const std::scoped_lock lock{captureMutex};
  QVERIFY(captured.has_value());
  QVERIFY(captured->selectableStrategy.has_value());
  const auto &buy = captured->selectableStrategy->buy;
  QCOMPARE(buy.logic, bte::strategy::ConditionLogic::any);
  QCOMPARE(buy.conditions.size(), 2U);
  QCOMPARE(buy.conditions[0].source, bte::strategy::ConditionSource::indicator);
  QCOMPARE(buy.conditions[0].indicator.kind,
           bte::indicators::IndicatorKind::macd);
  QCOMPARE(buy.conditions[0].indicator.period, 8);
  QCOMPARE(buy.conditions[0].indicator.secondaryPeriod, 16);
  QCOMPARE(buy.conditions[0].indicator.signalPeriod, 5);
  QCOMPARE(buy.conditions[0].threshold, 60.0);
  QCOMPARE(buy.conditions[0].thresholdDomain,
           bte::indicators::NumericDomain::price);
  QCOMPARE(buy.conditions[1].source,
           bte::strategy::ConditionSource::closeChangePercent);
  QCOMPARE(buy.conditions[1].thresholdDomain,
           bte::indicators::NumericDomain::percent);
}

void BacktestTabTest::selectableMetricsSubmitTypedPlansToBackend() {
  struct MetricCase final {
    QString label;
    bte::strategy::ConditionSource source;
    bte::indicators::NumericDomain domain;
    std::optional<bte::indicators::IndicatorKind> indicatorKind;
    bte::indicators::IndicatorOutput indicatorOutput =
        bte::indicators::IndicatorOutput::value;
    int expectedLookbackPeriod = 0;
  };
  const auto metricCases = std::vector<MetricCase>{
      {"Close change %",
       bte::strategy::ConditionSource::closeChangePercent,
       bte::indicators::NumericDomain::percent,
       {}},
      {"Previous data-day close change %",
       bte::strategy::ConditionSource::previousDataDayCloseChangePercent,
       bte::indicators::NumericDomain::percent,
       {}},
      {"Up X% over selected period",
       bte::strategy::ConditionSource::dataDayGainPercent,
       bte::indicators::NumericDomain::percent,
       {},
       bte::indicators::IndicatorOutput::value,
       30},
      {"Down X% from period high",
       bte::strategy::ConditionSource::periodHighDrawdownPercent,
       bte::indicators::NumericDomain::percent,
       {},
       bte::indicators::IndicatorOutput::value,
       21},
      {"Up X% from period low",
       bte::strategy::ConditionSource::periodLowReboundPercent,
       bte::indicators::NumericDomain::percent,
       {},
       bte::indicators::IndicatorOutput::value,
       21},
      {"Break period high",
       bte::strategy::ConditionSource::breakPeriodHigh,
       bte::indicators::NumericDomain::scalar,
       {},
       bte::indicators::IndicatorOutput::value,
       21},
      {"Break period low",
       bte::strategy::ConditionSource::breakPeriodLow,
       bte::indicators::NumericDomain::scalar,
       {},
       bte::indicators::IndicatorOutput::value,
       21},
      {"Close",
       bte::strategy::ConditionSource::barField,
       bte::indicators::NumericDomain::price,
       {}},
      {"SMA", bte::strategy::ConditionSource::indicator,
       bte::indicators::NumericDomain::price,
       bte::indicators::IndicatorKind::sma},
      {"EMA", bte::strategy::ConditionSource::indicator,
       bte::indicators::NumericDomain::price,
       bte::indicators::IndicatorKind::ema},
      {"WMA", bte::strategy::ConditionSource::indicator,
       bte::indicators::NumericDomain::price,
       bte::indicators::IndicatorKind::wma},
      {"RSI", bte::strategy::ConditionSource::indicator,
       bte::indicators::NumericDomain::percent,
       bte::indicators::IndicatorKind::rsi},
      {"MACD histogram", bte::strategy::ConditionSource::indicator,
       bte::indicators::NumericDomain::price,
       bte::indicators::IndicatorKind::macd,
       bte::indicators::IndicatorOutput::histogram},
      {"Bollinger upper", bte::strategy::ConditionSource::indicator,
       bte::indicators::NumericDomain::price,
       bte::indicators::IndicatorKind::bollingerBands,
       bte::indicators::IndicatorOutput::upper},
      {"ATR", bte::strategy::ConditionSource::indicator,
       bte::indicators::NumericDomain::price,
       bte::indicators::IndicatorKind::atr},
      {"ADX", bte::strategy::ConditionSource::indicator,
       bte::indicators::NumericDomain::percent,
       bte::indicators::IndicatorKind::adx},
      {"Stochastic %K", bte::strategy::ConditionSource::indicator,
       bte::indicators::NumericDomain::percent,
       bte::indicators::IndicatorKind::stochastic,
       bte::indicators::IndicatorOutput::percentK},
      {"Donchian upper", bte::strategy::ConditionSource::indicator,
       bte::indicators::NumericDomain::price,
       bte::indicators::IndicatorKind::donchian,
       bte::indicators::IndicatorOutput::upper},
      {"VWAP", bte::strategy::ConditionSource::indicator,
       bte::indicators::NumericDomain::price,
       bte::indicators::IndicatorKind::vwap},
      {"OBV", bte::strategy::ConditionSource::indicator,
       bte::indicators::NumericDomain::volume,
       bte::indicators::IndicatorKind::obv},
      {"ROC", bte::strategy::ConditionSource::indicator,
       bte::indicators::NumericDomain::percent,
       bte::indicators::IndicatorKind::roc},
      {"Momentum", bte::strategy::ConditionSource::indicator,
       bte::indicators::NumericDomain::price,
       bte::indicators::IndicatorKind::momentum},
      {"True range", bte::strategy::ConditionSource::indicator,
       bte::indicators::NumericDomain::price,
       bte::indicators::IndicatorKind::trueRange},
      {"Volume",
       bte::strategy::ConditionSource::barField,
       bte::indicators::NumericDomain::volume,
       {}},
  };

  std::mutex captureMutex;
  std::optional<bte::bindings::BacktestConfiguration> captured;
  std::atomic_int runCount = 0;
  bte::frontend::BacktestTab tab{
      [&captureMutex, &captured,
       &runCount](bte::bindings::BacktestConfiguration configuration,
                  bte::core::CancellationToken) {
        const std::scoped_lock lock{captureMutex};
        captured = std::move(configuration);
        runCount.fetch_add(1);
        return bte::core::Result<bte::bindings::BacktestSnapshot>{
            filledSnapshot()};
      }};
  auto *strategy = tab.findChild<QComboBox *>("backtestStrategyCombo");
  auto *metric = tab.findChild<QComboBox *>("backtestBuyMetricCombo");
  auto *period = tab.findChild<QSpinBox *>("backtestBuyPeriodSpinBox");
  auto *run = tab.findChild<QPushButton *>("backtestRunButton");
  QVERIFY(strategy != nullptr);
  QVERIFY(metric != nullptr);
  QVERIFY(period != nullptr);
  QVERIFY(run != nullptr);

  strategy->setCurrentText("Selectable conditions");
  period->setValue(18);
  for (std::size_t index = 0; index < metricCases.size(); ++index) {
    const auto &metricCase = metricCases[index];
    period->setValue(18);
    metric->setCurrentText(metricCase.label);
    QTest::mouseClick(run, Qt::LeftButton);
    const auto expectedRuns = static_cast<int>(index + 1U);
    QVERIFY(bte::test::waitUntil([&runCount, expectedRuns, &run] {
      return runCount.load() == expectedRuns && run->isEnabled();
    }));

    const std::scoped_lock lock{captureMutex};
    QVERIFY(captured.has_value());
    QVERIFY(captured->selectableStrategy.has_value());
    const auto &condition = captured->selectableStrategy->buy.conditions[0];
    QCOMPARE(condition.source, metricCase.source);
    QCOMPARE(condition.thresholdDomain, metricCase.domain);
    if (metricCase.expectedLookbackPeriod > 0) {
      QCOMPARE(condition.lookbackPeriod, metricCase.expectedLookbackPeriod);
      QCOMPARE(period->value(), metricCase.expectedLookbackPeriod);
      QCOMPARE(condition.indicator.period, 14);
      QVERIFY(metric->toolTip().contains("CSV/UTC data days"));
      QVERIFY(period->toolTip().contains("CSV/UTC dates"));
      if (metricCase.source ==
              bte::strategy::ConditionSource::breakPeriodHigh ||
          metricCase.source == bte::strategy::ConditionSource::breakPeriodLow) {
        QCOMPARE(condition.comparison, bte::strategy::Comparison::greaterThan);
        QCOMPARE(condition.threshold, 0.0);
      } else {
        QCOMPARE(condition.comparison,
                 bte::strategy::Comparison::greaterThanOrEqual);
        QCOMPARE(condition.threshold, 5.0);
      }
    }
    if (metricCase.label == "MACD histogram") {
      QCOMPARE(condition.indicator.period, 9);
      QCOMPARE(condition.indicator.secondaryPeriod, 18);
      QCOMPARE(condition.indicator.signalPeriod, 6);
    } else if (metricCase.expectedLookbackPeriod == 0) {
      QCOMPARE(condition.indicator.period, 18);
    }
    if (metricCase.indicatorKind.has_value()) {
      QCOMPARE(condition.indicator.kind, metricCase.indicatorKind.value());
      QCOMPARE(condition.indicator.output, metricCase.indicatorOutput);
    }
  }
}

void BacktestTabTest::
    previousDataDayCloseMetricUsesPositiveDownUpAndExplainsUtcSource() {
  QVERIFY(thread() == QCoreApplication::instance()->thread());
  std::mutex captureMutex;
  std::optional<bte::bindings::BacktestConfiguration> captured;
  std::atomic_int runCount = 0;
  bte::frontend::BacktestTab tab{
      [&captureMutex, &captured,
       &runCount](bte::bindings::BacktestConfiguration configuration,
                  const bte::core::CancellationToken &) {
        const std::scoped_lock lock{captureMutex};
        captured = std::move(configuration);
        runCount.fetch_add(1);
        return bte::core::Result<bte::bindings::BacktestSnapshot>{
            filledSnapshot()};
      }};
  const auto controls = previousDataDayControls(tab);
  QVERIFY(previousDataDayControlsArePresent(controls));
  controls.strategy->setCurrentText("Selectable conditions");
  controls.metric->setCurrentText("Previous data-day close change %");
  QVERIFY(previousDataDayUiContractIsVisible(controls));
  controls.threshold->setValue(0.0);
  QCOMPARE(controls.threshold->value(), 0.0001);

  controls.threshold->setValue(5.0);
  controls.comparison->setCurrentText("Down at least");
  QVERIFY(runBacktestAndWait(controls.run, runCount, 1));
  QVERIFY(capturedPreviousDataDayConditionMatches(
      captureMutex, captured, bte::strategy::Comparison::lessThanOrEqual,
      -5.0));

  controls.threshold->setValue(2.5);
  controls.comparison->setCurrentText("Up at least");
  QVERIFY(runBacktestAndWait(controls.run, runCount, 2));
  QVERIFY(capturedPreviousDataDayConditionMatches(
      captureMutex, captured, bte::strategy::Comparison::greaterThanOrEqual,
      2.5));

  controls.metric->setCurrentText("Close change %");
  QVERIFY(legacyConditionEditorIsRestored(controls));
}

void BacktestTabTest::selectableMetricUnitsDescribeEveryConditionRow() {
  bte::frontend::BacktestTab tab{[](bte::bindings::BacktestConfiguration,
                                    bte::core::CancellationToken) {
    return bte::core::Result<bte::bindings::BacktestSnapshot>{filledSnapshot()};
  }};
  auto *strategy = tab.findChild<QComboBox *>("backtestStrategyCombo");
  QVERIFY(strategy != nullptr);
  strategy->setCurrentText("Selectable conditions");
  auto *buySecondEnabled =
      tab.findChild<QCheckBox *>("backtestBuySecondEnabledCheckBox");
  auto *sellSecondEnabled =
      tab.findChild<QCheckBox *>("backtestSellSecondEnabledCheckBox");
  QVERIFY(buySecondEnabled != nullptr);
  QVERIFY(sellSecondEnabled != nullptr);
  buySecondEnabled->setChecked(true);
  sellSecondEnabled->setChecked(true);

  struct MetricDisplayCase {
    QString label;
    QString suffix;
    bool usesPeriod;
    QString meaning;
    QString example;
  };
  const auto metrics = std::array{
      MetricDisplayCase{"Close change %", " %", false,
                        "previous actual bar close in percent points",
                        "5 means a 5% rise; -5 means a 5% fall"},
      MetricDisplayCase{"Close", " USD", false, "Current close price in USD",
                        "100 means $100 per share"},
      MetricDisplayCase{"SMA", " USD", true, "Indicator price in USD",
                        "100 means $100 per share"},
      MetricDisplayCase{"EMA", " USD", true, "Indicator price in USD",
                        "100 means $100 per share"},
      MetricDisplayCase{"WMA", " USD", true, "Indicator price in USD",
                        "100 means $100 per share"},
      MetricDisplayCase{"RSI", " level", true,
                        "level from 0 to 100, not a price change",
                        "30 means oscillator level 30, not a 30% price drop"},
      MetricDisplayCase{"MACD histogram", " USD", true,
                        "Signed price difference in USD, not a percentage",
                        "-2 means a $2 negative difference"},
      MetricDisplayCase{"Bollinger upper", " USD", true,
                        "Indicator price in USD", "100 means $100 per share"},
      MetricDisplayCase{"ATR", " USD", true,
                        "Average price range in USD, not a percentage",
                        "2 means a $2 range"},
      MetricDisplayCase{"ADX", " level", true,
                        "level from 0 to 100, not a price change",
                        "30 means oscillator level 30, not a 30% price drop"},
      MetricDisplayCase{"Stochastic %K", " level", true,
                        "level from 0 to 100, not a price change",
                        "30 means oscillator level 30, not a 30% price drop"},
      MetricDisplayCase{"Donchian upper", " USD", true,
                        "Indicator price in USD", "100 means $100 per share"},
      MetricDisplayCase{"VWAP", " USD", true, "Indicator price in USD",
                        "100 means $100 per share"},
      MetricDisplayCase{"OBV", " shares", false,
                        "on-balance volume in shares, not dollars",
                        "-100 means net negative 100 shares"},
      MetricDisplayCase{"ROC", " %", true,
                        "selected bar lookback in percent points",
                        "5 means a 5% rise, not 0.05%"},
      MetricDisplayCase{"Momentum", " USD", true,
                        "Signed price difference in USD, not a percentage",
                        "-2 means a $2 negative difference"},
      MetricDisplayCase{"True range", " USD", false,
                        "Current bar true range in USD, not a percentage",
                        "2 means a $2 range"},
      MetricDisplayCase{"Volume", " shares", false,
                        "Current bar trading volume in shares",
                        "100 means 100 shares"},
  };
  const auto rows = std::array{
      std::tuple{"backtestBuyMetricCombo", "backtestBuyThresholdSpinBox",
                 "backtestBuyPeriodSpinBox"},
      std::tuple{"backtestBuySecondMetricCombo",
                 "backtestBuySecondThresholdSpinBox",
                 "backtestBuySecondPeriodSpinBox"},
      std::tuple{"backtestSellMetricCombo", "backtestSellThresholdSpinBox",
                 "backtestSellPeriodSpinBox"},
      std::tuple{"backtestSellSecondMetricCombo",
                 "backtestSellSecondThresholdSpinBox",
                 "backtestSellSecondPeriodSpinBox"},
  };
  for (const auto &[metricName, thresholdName, periodName] : rows) {
    auto *metric = tab.findChild<QComboBox *>(metricName);
    auto *threshold = tab.findChild<QDoubleSpinBox *>(thresholdName);
    auto *period = tab.findChild<QSpinBox *>(periodName);
    QVERIFY(metric != nullptr);
    QVERIFY(threshold != nullptr);
    QVERIFY(period != nullptr);
    threshold->setValue(5.0);
    for (const auto &expected : metrics) {
      metric->setCurrentText(expected.label);
      QCOMPARE(threshold->suffix(), expected.suffix);
      QCOMPARE(threshold->value(), 5.0);
      const auto descriptions =
          std::array{metric->toolTip(), metric->accessibleDescription(),
                     threshold->toolTip(), threshold->accessibleDescription()};
      for (const auto &description : descriptions) {
        QVERIFY(description.contains(expected.label));
        QVERIFY(description.contains(expected.meaning));
        QVERIFY(description.contains(expected.example));
      }
      QVERIFY(period->isEnabled());
      if (expected.usesPeriod) {
        QVERIFY(period->toolTip().contains("bars"));
        QVERIFY(period->toolTip().contains("warm-up"));
        QVERIFY(period->accessibleDescription().contains("bars"));
        QVERIFY(period->accessibleDescription().contains("warm-up"));
      } else {
        QVERIFY(period->toolTip().contains("does not use a period"));
        QVERIFY(
            period->accessibleDescription().contains("does not use a period"));
        QVERIFY(period->toolTip().contains("strategy identity"));
        QVERIFY(period->accessibleDescription().contains("strategy identity"));
      }
    }
  }
  buySecondEnabled->setChecked(false);
  sellSecondEnabled->setChecked(false);
  for (const auto &periodName : std::array{"backtestBuySecondPeriodSpinBox",
                                           "backtestSellSecondPeriodSpinBox"}) {
    auto *period = tab.findChild<QSpinBox *>(periodName);
    QVERIFY(period != nullptr);
    QVERIFY(!period->isEnabled());
  }
}

void BacktestTabTest::metricLabelsDoNotRescaleSignedOrZeroThresholds() {
  std::mutex captureMutex;
  std::optional<bte::bindings::BacktestConfiguration> captured;
  std::atomic_int runCount = 0;
  bte::frontend::BacktestTab tab{
      [&captureMutex, &captured,
       &runCount](bte::bindings::BacktestConfiguration configuration,
                  bte::core::CancellationToken) {
        const std::scoped_lock lock{captureMutex};
        captured = std::move(configuration);
        ++runCount;
        return bte::core::Result<bte::bindings::BacktestSnapshot>{
            filledSnapshot()};
      }};
  auto *strategy = tab.findChild<QComboBox *>("backtestStrategyCombo");
  auto *logic = tab.findChild<QComboBox *>("backtestBuyLogicCombo");
  auto *metric = tab.findChild<QComboBox *>("backtestBuyMetricCombo");
  auto *threshold =
      tab.findChild<QDoubleSpinBox *>("backtestBuyThresholdSpinBox");
  auto *secondEnabled =
      tab.findChild<QCheckBox *>("backtestBuySecondEnabledCheckBox");
  auto *secondMetric =
      tab.findChild<QComboBox *>("backtestBuySecondMetricCombo");
  auto *secondThreshold =
      tab.findChild<QDoubleSpinBox *>("backtestBuySecondThresholdSpinBox");
  auto *run = tab.findChild<QPushButton *>("backtestRunButton");
  QVERIFY(strategy != nullptr);
  QVERIFY(logic != nullptr);
  QVERIFY(metric != nullptr);
  QVERIFY(threshold != nullptr);
  QVERIFY(secondEnabled != nullptr);
  QVERIFY(secondMetric != nullptr);
  QVERIFY(secondThreshold != nullptr);
  QVERIFY(run != nullptr);
  strategy->setCurrentText("Selectable conditions");
  secondEnabled->setChecked(true);
  logic->setCurrentText("Match ANY (OR)");
  metric->setCurrentText("Close change %");
  threshold->setValue(5.0);
  secondMetric->setCurrentText("ROC");
  secondThreshold->setValue(0.05);
  QTest::mouseClick(run, Qt::LeftButton);
  QVERIFY(bte::test::waitUntil(
      [&] { return runCount.load() == 1 && run->isEnabled(); }));
  {
    const std::scoped_lock lock{captureMutex};
    QVERIFY(captured->selectableStrategy.has_value());
    const auto &buy = captured->selectableStrategy->buy;
    QCOMPARE(buy.logic, bte::strategy::ConditionLogic::any);
    QCOMPARE(buy.conditions.size(), std::size_t{2});
    QCOMPARE(buy.conditions[0].threshold, 5.0);
    QCOMPARE(buy.conditions[1].threshold, 0.05);
  }
  logic->setCurrentText("Match ALL (AND)");
  metric->setCurrentText("Momentum");
  threshold->setValue(-5.0);
  secondMetric->setCurrentText("OBV");
  secondThreshold->setValue(0.0);
  QTest::mouseClick(run, Qt::LeftButton);
  QVERIFY(bte::test::waitUntil(
      [&] { return runCount.load() == 2 && run->isEnabled(); }));
  {
    const std::scoped_lock lock{captureMutex};
    QVERIFY(captured->selectableStrategy.has_value());
    const auto &buy = captured->selectableStrategy->buy;
    QCOMPARE(buy.logic, bte::strategy::ConditionLogic::all);
    QCOMPARE(buy.conditions[0].threshold, -5.0);
    QCOMPARE(buy.conditions[1].threshold, 0.0);
  }
}

void BacktestTabTest::selectableConditionStatusNamesItsSelectedStrategy() {
  bte::frontend::BacktestTab tab{[](bte::bindings::BacktestConfiguration,
                                    bte::core::CancellationToken) {
    return bte::core::Result<bte::bindings::BacktestSnapshot>{filledSnapshot()};
  }};
  auto *strategy = tab.findChild<QComboBox *>("backtestStrategyCombo");
  auto *run = tab.findChild<QPushButton *>("backtestRunButton");
  auto *status = tab.findChild<QLabel *>("backtestStatusLabel");
  QVERIFY(strategy != nullptr);
  QVERIFY(run != nullptr);
  QVERIFY(status != nullptr);

  strategy->setCurrentText("Selectable conditions");
  QTest::mouseClick(run, Qt::LeftButton);

  QVERIFY(bte::test::waitUntil(
      [&status] { return status->text().contains("selectable strategy"); }));
  QVERIFY(!status->text().contains("starter order"));
}

void BacktestTabTest::selectableControlsPresentNoSignalAndSellFill() {
  std::atomic_int invocation = 0;
  std::mutex captureMutex;
  std::optional<bte::bindings::BacktestConfiguration> captured;
  bte::frontend::BacktestTab tab{
      [&captureMutex, &captured,
       &invocation](bte::bindings::BacktestConfiguration configuration,
                    bte::core::CancellationToken) {
        {
          const std::scoped_lock lock{captureMutex};
          captured = std::move(configuration);
        }
        if (invocation.fetch_add(1) == 0) {
          return bte::core::Result<bte::bindings::BacktestSnapshot>{
              noFillSnapshot(
                  bte::bindings::BacktestOutcome::completedNoSignal)};
        }
        auto snapshot = filledSnapshot();
        snapshot.fills.front().side = bte::bindings::BacktestFillSide::sell;
        return bte::core::Result<bte::bindings::BacktestSnapshot>{snapshot};
      }};
  auto *strategy = tab.findChild<QComboBox *>("backtestStrategyCombo");
  auto *sellSecond =
      tab.findChild<QCheckBox *>("backtestSellSecondEnabledCheckBox");
  auto *sellLogic = tab.findChild<QComboBox *>("backtestSellLogicCombo");
  auto *sellMetric = tab.findChild<QComboBox *>("backtestSellMetricCombo");
  auto *sellThreshold =
      tab.findChild<QDoubleSpinBox *>("backtestSellThresholdSpinBox");
  auto *sellSecondMetric =
      tab.findChild<QComboBox *>("backtestSellSecondMetricCombo");
  auto *sellSecondThreshold =
      tab.findChild<QDoubleSpinBox *>("backtestSellSecondThresholdSpinBox");
  auto *run = tab.findChild<QPushButton *>("backtestRunButton");
  auto *status = tab.findChild<QLabel *>("backtestStatusLabel");
  auto *trades = tab.findChild<QTableWidget *>("backtestTradeLogTable");
  QVERIFY(strategy != nullptr);
  QVERIFY(sellSecond != nullptr);
  QVERIFY(sellLogic != nullptr);
  QVERIFY(sellMetric != nullptr);
  QVERIFY(sellThreshold != nullptr);
  QVERIFY(sellSecondMetric != nullptr);
  QVERIFY(sellSecondThreshold != nullptr);
  QVERIFY(run != nullptr);
  QVERIFY(status != nullptr);
  QVERIFY(trades != nullptr);

  strategy->setCurrentText("Selectable conditions");
  const auto sellAnyIndex = sellLogic->findText("Match ANY (OR)");
  QVERIFY(sellAnyIndex >= 0);
  sellLogic->setCurrentIndex(sellAnyIndex);
  sellMetric->setCurrentText("ROC");
  sellThreshold->setValue(-2.0);
  QVERIFY(!sellSecondMetric->isEnabled());
  QTest::mouseClick(sellSecond, Qt::LeftButton);
  sellSecondMetric->setCurrentText("Stochastic %K");
  sellSecondThreshold->setValue(20.0);
  QVERIFY(sellSecondMetric->isEnabled());
  QTest::mouseClick(run, Qt::LeftButton);
  QVERIFY(bte::test::waitUntil([&status] {
    return status->text().contains("no selectable condition");
  }));
  {
    const std::scoped_lock lock{captureMutex};
    QVERIFY(captured.has_value());
    QVERIFY(captured->selectableStrategy.has_value());
    const auto &sell = captured->selectableStrategy->sell;
    QCOMPARE(sell.logic, bte::strategy::ConditionLogic::any);
    QCOMPARE(sell.conditions.size(), 2U);
    QCOMPARE(sell.conditions[0].indicator.kind,
             bte::indicators::IndicatorKind::roc);
    QCOMPARE(sell.conditions[0].threshold, -2.0);
    QCOMPARE(sell.conditions[1].indicator.kind,
             bte::indicators::IndicatorKind::stochastic);
    QCOMPARE(sell.conditions[1].indicator.output,
             bte::indicators::IndicatorOutput::percentK);
    QCOMPARE(sell.conditions[1].threshold, 20.0);
  }

  QTest::mouseClick(run, Qt::LeftButton);
  QVERIFY(bte::test::waitUntil([&trades] { return trades->rowCount() == 1; }));
  QCOMPARE(trades->rowCount(), 1);
  QCOMPARE(trades->item(0, 1)->text(), QString{"Sell"});
}

#if defined(BTE_TEST_MANAGED_RUNTIME_PROFILE_PATH) &&                          \
    defined(BTE_PYTHON_WORKER_BOOTSTRAP_PATH)
void BacktestTabTest::managedPythonRunCanBeSavedAndReplayedThroughTheGui() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const auto root = std::filesystem::path{directory.path().toStdString()};
  const auto resourcesRoot = root / "Resources";
  const auto profileRoot =
      std::filesystem::path{BTE_TEST_MANAGED_RUNTIME_PROFILE_PATH};
  const auto runtimeProfiles = resourcesRoot / "RuntimeProfiles";
  std::filesystem::create_directories(runtimeProfiles);
  std::error_code symlinkError;
  std::filesystem::create_directory_symlink(
      profileRoot, runtimeProfiles / profileRoot.filename(), symlinkError);
  QVERIFY2(!symlinkError, symlinkError.message().c_str());

  const auto bootstrapDirectory = resourcesRoot / "Python";
  std::filesystem::create_directories(bootstrapDirectory);
  const auto packagedBootstrap = bootstrapDirectory / "worker_bootstrap.py";
  std::filesystem::copy_file(BTE_PYTHON_WORKER_BOOTSTRAP_PATH,
                             packagedBootstrap);
  ScopedEnvironmentRestore resourcesOverride{"BTE_PYTHON_RESOURCES_ROOT"};
  ScopedEnvironmentRestore bootstrapOverride{"BTE_PYTHON_WORKER_BOOTSTRAP"};
  QVERIFY(qputenv("BTE_PYTHON_RESOURCES_ROOT",
                  QByteArray::fromStdString(resourcesRoot.string())));
  QVERIFY(qputenv("BTE_PYTHON_WORKER_BOOTSTRAP", QByteArray{}));

  const auto sourceDirectory = root / "Source";
  std::filesystem::create_directories(sourceDirectory);
  std::ofstream sourceFile{sourceDirectory / "AAPL.csv"};
  sourceFile
      << "symbol,ts,open,high,low,close,volume,schemaName\n"
         "AAPL,2024-01-02 00:00:00+00:00,100,101,99,100,1200,ohlcv-1h\n"
         "AAPL,2024-01-02 01:00:00+00:00,101,102,100,101,1200,ohlcv-1h\n"
         "AAPL,2024-01-02 02:00:00+00:00,102,103,101,102,1200,ohlcv-1h\n";
  sourceFile.close();
  const auto built = bte::data::buildReleaseSnapshot({
      .sourceDirectory = sourceDirectory,
      .storeDirectory = root / "Data",
      .symbols = {"AAPL"},
      .rowsPerSegment = 2,
      .calendarHash = std::string(64, 'a'),
      .splitManifestHash = std::string(64, 'b'),
  });
  QVERIFY2(built.ok(), built.error().message.c_str());

  const auto storage = bte::bindings::PersistedBacktestStorage{
      .resultStore = root / "Results",
      .dataStore = root / "Data",
      .snapshotId = built.value().snapshotId,
      .strategyHash = {},
  };
  const std::string source = R"python(import stockbt

STOCKBT_API_VERSION = "1"
STOCKBT_TEMPLATE_VERSION = "1.0.0"

class Example:
    max_history_bars = 1
    indicators = {}

    def on_market(self, ctx):
        if ctx.bar.close == 100.0:
            ctx.orders.buy_market(ctx.symbol, quantity=2)

def create_strategy(params):
    return Example()
)python";
  const auto runPersisted =
      [storage](const bte::bindings::BacktestConfiguration &configuration,
                const bte::core::CancellationToken &cancellation) {
        return bte::bindings::runPersistedBacktestConfiguration(
            configuration, storage, cancellation);
      };
  const auto publish =
      [storage](const std::string &resultId, const std::string &displayName,
                const bte::core::CancellationToken &cancellation) {
        return bte::bindings::publishBacktestDraft(storage, resultId,
                                                   displayName, cancellation);
      };
  bte::frontend::BacktestTab tab{runPersisted, publish};
  tab.setConsentProvider([](const std::string &) { return true; });
  tab.setPythonStrategyDraft(
      QString::fromStdString(source),
      QString::fromLatin1(
          R"json({"fast_period":1,"quantity":2,"slow_period":2})json"));
  auto *start = tab.findChild<QDateEdit *>("backtestStartDateEdit");
  auto *end = tab.findChild<QDateEdit *>("backtestEndDateEdit");
  auto *run = tab.findChild<QPushButton *>("backtestRunButton");
  auto *saveAndReplay =
      tab.findChild<QPushButton *>("backtestSaveAndReplayButton");
  auto *trades = tab.findChild<QTableWidget *>("backtestTradeLogTable");
  auto *status = tab.findChild<QLabel *>("backtestStatusLabel");
  QVERIFY(start != nullptr);
  QVERIFY(end != nullptr);
  QVERIFY(run != nullptr);
  QVERIFY(saveAndReplay != nullptr);
  QVERIFY(trades != nullptr);
  QVERIFY(status != nullptr);
  start->setDate(QDate{2024, 1, 2});
  end->setDate(QDate{2024, 1, 2});
  QSignalSpy opened{&tab, &bte::frontend::BacktestTab::openResultInReplay};
  std::unique_ptr<bte::bindings::ResultReplay> replay;
  QString replayError;
  QObject::connect(
      &tab, &bte::frontend::BacktestTab::openResultInReplay, &tab,
      [&](const QString &resultId) {
        auto openedReplay = bte::bindings::ResultReplay::open(
            storage.resultStore, storage.dataStore, resultId.toStdString(),
            bte::bindings::ResultReplayTimeframe::hourly);
        if (!openedReplay.ok()) {
          replayError = QString::fromStdString(openedReplay.error().message);
          return;
        }
        replay = std::move(openedReplay).value();
      });

  QTest::mouseClick(run, Qt::LeftButton);
  QVERIFY2(bte::test::waitUntil(
               [saveAndReplay, status] {
                 return saveAndReplay->isEnabled() ||
                        !status->text().contains("Running");
               },
               std::chrono::milliseconds{30'000}),
           qPrintable(status->text()));
  QVERIFY2(saveAndReplay->isEnabled(), qPrintable(status->text()));
  QVERIFY2(bte::test::waitUntil([trades] { return trades->rowCount() == 1; },
                                std::chrono::milliseconds{5'000}),
           qPrintable(status->text()));
  QCOMPARE(trades->item(0, 1)->text(), QString{"Buy"});
  QTest::mouseClick(saveAndReplay, Qt::LeftButton);
  QVERIFY2(bte::test::waitUntil([&opened] { return opened.count() == 1; },
                                std::chrono::milliseconds{10'000}),
           qPrintable(status->text()));
  QVERIFY2(replay != nullptr, qPrintable(replayError));
  QCOMPARE(replay->resultId(), opened.first().first().toString().toStdString());
  QCOMPARE(replay->totalFrames(), 3U);
  QVERIFY(replay->seek(1));
  QVERIFY(replay->current() != nullptr);
  QCOMPARE(replay->current()->fills.size(), 1U);
  QVERIFY(replay->current()->fills.front().isBuy);
  QCOMPARE(replay->current()->fills.front().quantityShares, 2);
}
#endif

} // namespace

QTEST_MAIN(BacktestTabTest)

#include "UnitTest_BacktestTab.moc"
