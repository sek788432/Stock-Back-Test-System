#include "Bte/Frontend/StrategyTab.h"

#include "PythonWorkerStrategy.h"
#include "WaitUntil.h"

#include <Bte/Core/Bar.h>
#include <Bte/Strategy/Strategy.h>

#include <QAbstractButton>
#include <QApplication>
#include <QByteArray>
#include <QFile>
#include <QFileDialog>
#include <QLabel>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSignalSpy>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <future>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

namespace {

bool writeSourceFile(const std::filesystem::path &path,
                     const QByteArray &source) {
  QFile file{QString::fromStdString(path.string())};
  return file.open(QIODevice::WriteOnly) &&
         file.write(source) == static_cast<qint64>(source.size());
}

std::shared_ptr<bool> chooseReplacementButton(const QString &buttonText) {
  const auto clicked = std::make_shared<bool>(false);
  QTimer::singleShot(0, qApp, [buttonText, clicked] {
    auto *dialog =
        qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
    if (dialog == nullptr) {
      return;
    }
    for (auto *button : dialog->buttons()) {
      if (button->text() == buttonText) {
        *clicked = true;
        button->click();
        return;
      }
    }
  });
  return clicked;
}

std::shared_ptr<bool> chooseFileDialogPath(const QString &path) {
  const auto selected = std::make_shared<bool>(false);
  QTimer::singleShot(0, qApp, [path, selected] {
    auto *dialog =
        qobject_cast<QFileDialog *>(QApplication::activeModalWidget());
    if (dialog == nullptr) {
      return;
    }
    dialog->selectFile(path);
    *selected =
        QMetaObject::invokeMethod(dialog, "accept", Qt::QueuedConnection);
  });
  QTimer::singleShot(5000, qApp, [selected] {
    if (!*selected) {
      if (auto *const dialog = QApplication::activeModalWidget();
          dialog != nullptr) {
        dialog->close();
      }
    }
  });
  return selected;
}

class StrategyTabTest final : public QObject {
  Q_OBJECT

private slots:
  void initTestCase();
  void exposesSampleAndPersistsRecoverableDrafts();
  void invalidDraftsRemainUntouchedAndFallBackToEmptyEditor();
  void restoresLegacyDraftsWithNormalizedLineEndings();
  void saveDraftButtonReportsInvalidSource();
  void importsUtf8NormalizesLineEndingsAndRejectsInvalidFiles();
  void fileDialogButtonsLoadAndExportPythonSource();
  void acceptsSourceAtTheOneMiBLimit();
  void autosavesAfterFiveHundredMillisecondsOfIdle();
  void savesDirtyDraftBeforeLoadingReplacementWhenRequested();
  void discardsDirtyDraftBeforeLoadingReplacementWhenRequested();
  void keepsDirtyDraftWhenReplacementIsCanceled();
  void validatesWithFrozenParametersAndConsent();
  void rejectsInvalidDraftsBeforeValidationOrHandoff();
  void asksForConsentBeforeValidatingTrustedSource();
  void reportsValidationWorkerFailures();
  void explainsWhenManagedValidationIsUnavailable();
  void usesDefaultManagedValidationRunnerFromApplicationResources();
  void cancellationReachesTheValidationWorker();
  void cannotSaveDraftWhenItsDestinationIsUnavailable();
  void autosaveReportsWhenItsDraftDirectoryIsUnavailable();
  void preservesCurrentDraftWhenItsRecoveryCopyCannotBeWritten();
  void ignoresValidationForAnEditedDraft();
  void useInBacktestEmitsTheCurrentSourceAndParameters();
  void exportsNormalizedUtf8SourceAtomically();
  void reportsWhenThePythonExportDestinationCannotBeWritten();
  void rejectsUnpairedSurrogatesBeforeValidationOrExport();
  void builtInSampleHandlesIncompleteHistoryAndEmitsValidOrder();
  void preservesUnrecoverableDraftsWhenTheTabCloses();
};

void StrategyTabTest::initTestCase() {
  QStandardPaths::setTestModeEnabled(true);
}

void StrategyTabTest::exposesSampleAndPersistsRecoverableDrafts() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const auto path = std::filesystem::path{directory.path().toStdString()};
  {
    bte::frontend::StrategyTab tab{path, {}};
    const auto *editor =
        tab.findChild<QPlainTextEdit *>("pythonStrategySourceEditor");
    QVERIFY(editor != nullptr);
    QVERIFY(editor->toPlainText().contains("def create_strategy(params):"));
    QVERIFY(tab.saveDraft());
    auto *mutableEditor =
        tab.findChild<QPlainTextEdit *>("pythonStrategySourceEditor");
    mutableEditor->setPlainText("first generation = True\n");
    QVERIFY(tab.saveDraft());
    mutableEditor->setPlainText("second generation = True\n");
    QVERIFY(tab.saveDraft());
    QFile current{
        QString::fromStdString((path / "python-draft.json").string())};
    QVERIFY(current.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QCOMPARE(current.write("{broken"), qint64{7});
  }

  bte::frontend::StrategyTab recovered{path, {}};
  const auto *editor =
      recovered.findChild<QPlainTextEdit *>("pythonStrategySourceEditor");
  QVERIFY(editor != nullptr);
  QCOMPARE(editor->toPlainText(), QString{"first generation = True\n"});
  QVERIFY(recovered.findChild<QLabel *>("pythonStrategyStatusLabel")
              ->text()
              .contains("recovered the previous valid generation"));
  QFile oversizedNumber{
      QString::fromStdString((path / "python-draft.json").string())};
  QVERIFY(oversizedNumber.open(QIODevice::WriteOnly | QIODevice::Truncate));
  constexpr auto invalidDraft =
      R"({"fastPeriod":9223372036854775808,"quantity":1,"schemaVersion":1,"slowPeriod":20,"source":"too large"})";
  QCOMPARE(oversizedNumber.write(invalidDraft),
           static_cast<qint64>(std::char_traits<char>::length(invalidDraft)));
  oversizedNumber.close();
  bte::frontend::StrategyTab overflowRecovered{path, {}};
  QCOMPARE(overflowRecovered.sourceUtf8(),
           std::string{"first generation = True\n"});
}

void StrategyTabTest::invalidDraftsRemainUntouchedAndFallBackToEmptyEditor() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const auto root = std::filesystem::path{directory.path().toStdString()};
  const auto validSourcePrefix =
      R"({"fastPeriod":5,"quantity":1,"schemaVersion":1,"slowPeriod":20,"source":")";
  const auto validSourceSuffix = R"("})";
  const auto oversizedSource =
      validSourcePrefix + std::string(1024 * 1024 + 1, 'x') + validSourceSuffix;
  const std::array invalidDrafts{
      QByteArray{
          R"({"fastPeriod":5,"quantity":1,"schemaVersion":1,"slowPeriod":20,"source":""})"},
      QByteArray{
          R"({"fastPeriod":5,"quantity":1,"schemaVersion":1,"slowPeriod":20,"source":"\u0000"})"},
      QByteArray::fromStdString(oversizedSource),
      QByteArray{
          R"({"fastPeriod":5,"quantity":0,"schemaVersion":1,"slowPeriod":20,"source":"valid"})"},
      QByteArray{
          R"({"fastPeriod":20,"quantity":1,"schemaVersion":1,"slowPeriod":20,"source":"valid"})"},
      QByteArray{
          R"({"fastPeriod":-1,"quantity":1,"schemaVersion":1,"slowPeriod":20,"source":"valid"})"},
      QByteArray{
          R"({"fastPeriod":5,"quantity":1,"schemaVersion":1,"slowPeriod":-1,"source":"valid"})"},
      QByteArray{
          R"({"fastPeriod":5,"quantity":-1,"schemaVersion":1,"slowPeriod":20,"source":"valid"})"},
  };

  for (std::size_t index = 0; index < invalidDrafts.size(); ++index) {
    const auto caseDirectory = root / std::to_string(index);
    QVERIFY(std::filesystem::create_directories(caseDirectory));
    const auto currentPath = caseDirectory / "python-draft.json";
    QVERIFY(writeSourceFile(currentPath, invalidDrafts[index]));

    bte::frontend::StrategyTab recovered{caseDirectory, {}};

    QCOMPARE(recovered.sourceUtf8(), std::string{});
    QVERIFY(recovered.findChild<QLabel *>("pythonStrategyStatusLabel")
                ->text()
                .contains("preserved for diagnosis"));
    QFile preserved{QString::fromStdString(currentPath.string())};
    QVERIFY(preserved.open(QIODevice::ReadOnly));
    QCOMPARE(preserved.readAll(), invalidDrafts[index]);
  }
}

void StrategyTabTest::restoresLegacyDraftsWithNormalizedLineEndings() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const auto root = std::filesystem::path{directory.path().toStdString()};
  const auto draftPath = root / "python-draft.json";
  const QByteArray legacyDraft{
      R"({"fastPeriod":5,"quantity":1,"schemaVersion":1,"slowPeriod":20,"source":"first\r\nsecond\r"})"};
  QVERIFY(writeSourceFile(draftPath, legacyDraft));

  bte::frontend::StrategyTab tab{root, {}};

  QCOMPARE(tab.sourceUtf8(), std::string{"first\nsecond\n"});
}

void StrategyTabTest::saveDraftButtonReportsInvalidSource() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const auto drafts =
      std::filesystem::path{directory.path().toStdString()} / "drafts";
  bte::frontend::StrategyTab tab{drafts, {}};
  auto *editor = tab.findChild<QPlainTextEdit *>("pythonStrategySourceEditor");
  auto *save = tab.findChild<QPushButton *>("pythonStrategySaveDraftButton");
  auto *status = tab.findChild<QLabel *>("pythonStrategyStatusLabel");
  QVERIFY(editor != nullptr);
  QVERIFY(save != nullptr);
  QVERIFY(status != nullptr);
  QVERIFY(tab.saveDraft());

  QFile original{
      QString::fromStdString((drafts / "python-draft.json").string())};
  QVERIFY(original.open(QIODevice::ReadOnly));
  const auto originalContents = original.readAll();
  original.close();

  editor->clear();
  QTest::mouseClick(save, Qt::LeftButton);

  QVERIFY(status->text().contains("Cannot save the Python Strategy draft"));
  QFile preserved{
      QString::fromStdString((drafts / "python-draft.json").string())};
  QVERIFY(preserved.open(QIODevice::ReadOnly));
  QCOMPARE(preserved.readAll(), originalContents);
}

void StrategyTabTest::importsUtf8NormalizesLineEndingsAndRejectsInvalidFiles() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const auto path = std::filesystem::path{directory.path().toStdString()};
  const auto sourcePath = path / "sample.py";
  QFile source{QString::fromStdString(sourcePath.string())};
  QVERIFY(source.open(QIODevice::WriteOnly));
  const QByteArray valid{"\xef\xbb\xbfx=1\r\n", 8};
  QCOMPARE(source.write(valid), static_cast<qint64>(valid.size()));
  source.close();

  bte::frontend::StrategyTab tab{path / "drafts", {}};
  QVERIFY(tab.saveDraft());
  QVERIFY(tab.loadSourceFile(sourcePath));
  QCOMPARE(QString::fromStdString(tab.sourceUtf8()), QString{"x=1\n"});
  QVERIFY(tab.saveDraft());

  const auto invalidPath = path / "invalid.py";
  QFile invalid{QString::fromStdString(invalidPath.string())};
  QVERIFY(invalid.open(QIODevice::WriteOnly));
  QCOMPARE(invalid.write(QByteArray::fromHex("fffe")), qint64{2});
  invalid.close();
  QVERIFY(!tab.loadSourceFile(invalidPath));
  QCOMPARE(QString::fromStdString(tab.sourceUtf8()), QString{"x=1\n"});

  const auto oversizedPath = path / "oversized.py";
  QFile oversized{QString::fromStdString(oversizedPath.string())};
  QVERIFY(oversized.open(QIODevice::WriteOnly));
  QCOMPARE(oversized.write(QByteArray(1024 * 1024 + 1, 'x')),
           qint64{1024 * 1024 + 1});
  oversized.close();
  QVERIFY(!tab.loadSourceFile(oversizedPath));
  QCOMPARE(QString::fromStdString(tab.sourceUtf8()), QString{"x=1\n"});

  const auto nulPath = path / "nul.py";
  QVERIFY(writeSourceFile(nulPath, QByteArray{"x\0y", 3}));
  QVERIFY(!tab.loadSourceFile(nulPath));
  QVERIFY(tab.findChild<QLabel *>("pythonStrategyStatusLabel")
              ->text()
              .contains("StrategySourceContainsNul"));
  QCOMPARE(QString::fromStdString(tab.sourceUtf8()), QString{"x=1\n"});

  const auto unsupportedPath = path / "strategy.txt";
  QVERIFY(writeSourceFile(unsupportedPath, QByteArray{"ignored\n"}));
  QVERIFY(!tab.loadSourceFile(unsupportedPath));
  QVERIFY(tab.findChild<QLabel *>("pythonStrategyStatusLabel")
              ->text()
              .contains("Only .py files can be loaded"));
}

void StrategyTabTest::fileDialogButtonsLoadAndExportPythonSource() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const auto root = std::filesystem::path{directory.path().toStdString()};
  const auto sourcePath = root / "loaded.py";
  const auto exportPath = root / "exported.py";
  QVERIFY(writeSourceFile(sourcePath, QByteArray{"# loaded through UI\r\n"}));

  bte::frontend::StrategyTab tab{root / "drafts", {}};
  QVERIFY(tab.saveDraft());
  auto *load = tab.findChild<QPushButton *>("pythonStrategyLoadButton");
  auto *exportSource =
      tab.findChild<QPushButton *>("pythonStrategyExportButton");
  QVERIFY(load != nullptr);
  QVERIFY(exportSource != nullptr);

  const auto loadSelection =
      chooseFileDialogPath(QString::fromStdString(sourcePath.string()));
  QTest::mouseClick(load, Qt::LeftButton);
  QVERIFY(*loadSelection);
  QCOMPARE(tab.sourceUtf8(), std::string{"# loaded through UI\n"});

  const auto exportSelection =
      chooseFileDialogPath(QString::fromStdString(exportPath.string()));
  QTest::mouseClick(exportSource, Qt::LeftButton);
  QVERIFY(*exportSelection);
  QFile exported{QString::fromStdString(exportPath.string())};
  QVERIFY(exported.open(QIODevice::ReadOnly));
  QCOMPARE(exported.readAll(), QByteArray{"# loaded through UI\n"});
}

void StrategyTabTest::acceptsSourceAtTheOneMiBLimit() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const auto root = std::filesystem::path{directory.path().toStdString()};
  const auto sourcePath = root / "boundary.py";
  const auto expectedSource = QByteArray(1024 * 1024, '#');
  QFile source{QString::fromStdString(sourcePath.string())};
  QVERIFY(source.open(QIODevice::WriteOnly));
  QCOMPARE(source.write(expectedSource),
           static_cast<qint64>(expectedSource.size()));
  source.close();

  bte::frontend::StrategyTab tab{root / "drafts", {}};
  QVERIFY(tab.saveDraft());
  QVERIFY(tab.loadSourceFile(sourcePath));
  QCOMPARE(QString::fromStdString(tab.sourceUtf8()),
           QString::fromUtf8(expectedSource));
  QCOMPARE(tab.sourceUtf8().size(), std::size_t{1024 * 1024});
}

void StrategyTabTest::autosavesAfterFiveHundredMillisecondsOfIdle() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const auto drafts =
      std::filesystem::path{directory.path().toStdString()} / "drafts";
  const auto draftPath = drafts / "python-draft.json";
  bte::frontend::StrategyTab tab{drafts, {}};
  auto *editor = tab.findChild<QPlainTextEdit *>("pythonStrategySourceEditor");
  QVERIFY(editor != nullptr);

  editor->setPlainText("first idle generation = True\n");
  QTest::qWait(350);
  editor->setPlainText("second idle generation = True\n");
  QTest::qWait(350);
  QVERIFY(!std::filesystem::exists(draftPath));
  QVERIFY(bte::test::waitUntil(
      [&draftPath] { return std::filesystem::exists(draftPath); },
      std::chrono::milliseconds{1500}));

  QFile draft{QString::fromStdString(draftPath.string())};
  QVERIFY(draft.open(QIODevice::ReadOnly));
  const auto saved = QString::fromUtf8(draft.readAll());
  QVERIFY(saved.contains("second idle generation = True"));
  QVERIFY(!saved.contains("first idle generation = True"));
}

void StrategyTabTest::savesDirtyDraftBeforeLoadingReplacementWhenRequested() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const auto root = std::filesystem::path{directory.path().toStdString()};
  const auto drafts = root / "drafts";
  const auto replacementPath = root / "replacement.py";
  QVERIFY(writeSourceFile(replacementPath, QByteArray{"# replacement\n"}));

  bte::frontend::StrategyTab tab{drafts, {}};
  auto *editor = tab.findChild<QPlainTextEdit *>("pythonStrategySourceEditor");
  QVERIFY(editor != nullptr);
  editor->setPlainText("saved generation = True\n");
  QVERIFY(tab.saveDraft());
  editor->setPlainText("unsaved generation = True\n");

  const auto clicked = chooseReplacementButton("Save Draft");
  QVERIFY(tab.loadSourceFile(replacementPath));
  QVERIFY(*clicked);
  QCOMPARE(tab.sourceUtf8(), std::string{"# replacement\n"});
  QVERIFY(tab.autosaveDraft());

  QFile previous{
      QString::fromStdString((drafts / "python-draft.previous.json").string())};
  QVERIFY(previous.open(QIODevice::ReadOnly));
  QVERIFY(QString::fromUtf8(previous.readAll())
              .contains("unsaved generation = True"));
}

void StrategyTabTest::
    discardsDirtyDraftBeforeLoadingReplacementWhenRequested() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const auto root = std::filesystem::path{directory.path().toStdString()};
  const auto drafts = root / "drafts";
  const auto replacementPath = root / "replacement.py";
  QVERIFY(writeSourceFile(replacementPath, QByteArray{"# replacement\n"}));

  bte::frontend::StrategyTab tab{drafts, {}};
  auto *editor = tab.findChild<QPlainTextEdit *>("pythonStrategySourceEditor");
  QVERIFY(editor != nullptr);
  editor->setPlainText("saved generation = True\n");
  QVERIFY(tab.saveDraft());
  editor->setPlainText("discarded generation = True\n");

  const auto clicked = chooseReplacementButton("Discard");
  QVERIFY(tab.loadSourceFile(replacementPath));
  QVERIFY(*clicked);
  QCOMPARE(tab.sourceUtf8(), std::string{"# replacement\n"});
  QVERIFY(tab.autosaveDraft());

  QFile current{
      QString::fromStdString((drafts / "python-draft.json").string())};
  QVERIFY(current.open(QIODevice::ReadOnly));
  const auto saved = QString::fromUtf8(current.readAll());
  QVERIFY(saved.contains("# replacement"));
  QVERIFY(!saved.contains("discarded generation = True"));
}

void StrategyTabTest::keepsDirtyDraftWhenReplacementIsCanceled() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const auto root = std::filesystem::path{directory.path().toStdString()};
  const auto drafts = root / "drafts";
  const auto replacementPath = root / "replacement.py";
  QVERIFY(writeSourceFile(replacementPath, QByteArray{"# replacement\n"}));

  bte::frontend::StrategyTab tab{drafts, {}};
  auto *editor = tab.findChild<QPlainTextEdit *>("pythonStrategySourceEditor");
  QVERIFY(editor != nullptr);
  editor->setPlainText("saved generation = True\n");
  QVERIFY(tab.saveDraft());
  editor->setPlainText("unsaved generation = True\n");

  const auto clicked = chooseReplacementButton("Cancel");
  QVERIFY(!tab.loadSourceFile(replacementPath));
  QVERIFY(*clicked);
  QCOMPARE(tab.sourceUtf8(), std::string{"unsaved generation = True\n"});
}

void StrategyTabTest::validatesWithFrozenParametersAndConsent() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  std::atomic_int calls = 0;
  auto runner =
      [&calls](const bte::bindings::PythonStrategyConfiguration &config,
               const bte::core::CancellationToken &cancellation) {
        ++calls;
        if (cancellation.isCancellationRequested() ||
            config.sourceUtf8 !=
                "def create_strategy(params):\n    return None\n" ||
            config.canonicalParametersJson !=
                "{\"fast_period\":5,\"quantity\":1,\"slow_period\":20}" ||
            !config.sourceConsentGranted) {
          return bte::core::Result<void>{
              bte::core::makeError(bte::core::ErrorCode::invalidArgument,
                                   "frozen configuration differs")};
        }
        return bte::core::Result<void>{};
      };
  bte::frontend::StrategyTab tab{
      std::filesystem::path{directory.path().toStdString()}, runner,
      [](const std::string &) { return true; }};
  tab.findChild<QPlainTextEdit *>("pythonStrategySourceEditor")
      ->setPlainText("def create_strategy(params):\n    return None\n");
  auto *validate = tab.findChild<QPushButton *>("pythonStrategyValidateButton");
  QVERIFY(validate != nullptr);
  QTest::mouseClick(validate, Qt::LeftButton);
  QVERIFY(bte::test::waitUntil(
      [&tab] {
        return tab.findChild<QLabel *>("pythonStrategyStatusLabel")
            ->text()
            .contains("Strategy is valid");
      },
      std::chrono::milliseconds{5000}));
  QCOMPARE(calls.load(), 1);
}

void StrategyTabTest::rejectsInvalidDraftsBeforeValidationOrHandoff() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  std::atomic_int validationCalls = 0;
  std::atomic_int consentCalls = 0;
  bte::frontend::StrategyTab tab{
      std::filesystem::path{directory.path().toStdString()},
      [&validationCalls](const bte::bindings::PythonStrategyConfiguration &,
                         const bte::core::CancellationToken &) {
        ++validationCalls;
        return bte::core::Result<void>{};
      },
      [&consentCalls](const std::string &) {
        ++consentCalls;
        return false;
      }};
  auto *editor = tab.findChild<QPlainTextEdit *>("pythonStrategySourceEditor");
  auto *fast = tab.findChild<QSpinBox *>("pythonStrategyFastPeriodSpinBox");
  auto *slow = tab.findChild<QSpinBox *>("pythonStrategySlowPeriodSpinBox");
  auto *validate = tab.findChild<QPushButton *>("pythonStrategyValidateButton");
  auto *use = tab.findChild<QPushButton *>("pythonStrategyUseInBacktestButton");
  auto *status = tab.findChild<QLabel *>("pythonStrategyStatusLabel");
  auto *diagnostics =
      tab.findChild<QPlainTextEdit *>("pythonStrategyDiagnostics");
  QVERIFY(editor != nullptr);
  QVERIFY(fast != nullptr);
  QVERIFY(slow != nullptr);
  QVERIFY(validate != nullptr);
  QVERIFY(use != nullptr);
  QVERIFY(status != nullptr);
  QVERIFY(diagnostics != nullptr);
  QSignalSpy handoff{&tab, &bte::frontend::StrategyTab::useInBacktest};

  editor->setPlainText("def create_strategy(params):\n    return None\n");
  fast->setValue(20);
  slow->setValue(20);
  QTest::mouseClick(validate, Qt::LeftButton);
  QVERIFY(status->text().contains("Parameters are invalid"));
  QVERIFY(diagnostics->toPlainText().contains("InvalidStrategyParameters"));
  QTest::mouseClick(use, Qt::LeftButton);
  QVERIFY(status->text().contains("fast_period must be less than slow_period"));
  QCOMPARE(handoff.size(), 0);

  fast->setValue(5);
  slow->setValue(20);
  editor->clear();
  QTest::mouseClick(validate, Qt::LeftButton);
  QVERIFY(status->text().contains("Source is invalid"));
  QVERIFY(diagnostics->toPlainText().contains("valid UTF-8, non-empty"));
  QTest::mouseClick(use, Qt::LeftButton);
  QVERIFY(status->text().contains("Source must be valid UTF-8, non-empty"));
  QCOMPARE(handoff.size(), 0);

  editor->setPlainText(QString(1024 * 1024 + 1, QChar{u'x'}));
  QTest::mouseClick(validate, Qt::LeftButton);
  QVERIFY(status->text().contains("Source is invalid"));
  QTest::mouseClick(use, Qt::LeftButton);
  QVERIFY(status->text().contains("at most 1 MiB"));
  QCOMPARE(handoff.size(), 0);

  auto malformedUtf16 = QString{"# malformed source: "};
  malformedUtf16.append(QChar{char16_t{0xd800}});
  malformedUtf16.append("\ndef create_strategy(params):\n    return None\n");
  editor->setPlainText(malformedUtf16);
  QTest::mouseClick(validate, Qt::LeftButton);
  QVERIFY(status->text().contains("Source is invalid"));
  QTest::mouseClick(use, Qt::LeftButton);
  QVERIFY(status->text().contains("Source must be valid UTF-8, non-empty"));
  QCOMPARE(handoff.size(), 0);

  auto orphanedLowSurrogate = QString{"# malformed source: "};
  orphanedLowSurrogate.append(QChar{char16_t{0xdc00}});
  orphanedLowSurrogate.append(
      "\ndef create_strategy(params):\n    return None\n");
  editor->setPlainText(orphanedLowSurrogate);
  QTest::mouseClick(validate, Qt::LeftButton);
  QVERIFY(status->text().contains("Source is invalid"));
  QTest::mouseClick(use, Qt::LeftButton);
  QVERIFY(status->text().contains("Source must be valid UTF-8, non-empty"));
  QCOMPARE(handoff.size(), 0);
  QCOMPARE(consentCalls.load(), 0);
  QCOMPARE(validationCalls.load(), 0);

  editor->setPlainText("def create_strategy(params):\n    return None\n");
  QTest::mouseClick(validate, Qt::LeftButton);
  QVERIFY(status->text().contains("Consent was not granted"));
  QCOMPARE(consentCalls.load(), 1);
  QCOMPARE(validationCalls.load(), 0);
  QCOMPARE(handoff.size(), 0);
}

void StrategyTabTest::asksForConsentBeforeValidatingTrustedSource() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  std::atomic_int validationCalls = 0;
  auto promptCount = std::make_shared<int>(0);
  auto informativeText = std::make_shared<QString>();
  bte::frontend::StrategyTab tab{
      std::filesystem::path{directory.path().toStdString()},
      [&validationCalls](const bte::bindings::PythonStrategyConfiguration &,
                         const bte::core::CancellationToken &) {
        ++validationCalls;
        return bte::core::Result<void>{};
      }};
  auto *editor = tab.findChild<QPlainTextEdit *>("pythonStrategySourceEditor");
  auto *validate = tab.findChild<QPushButton *>("pythonStrategyValidateButton");
  auto *status = tab.findChild<QLabel *>("pythonStrategyStatusLabel");
  QVERIFY(editor != nullptr);
  QVERIFY(validate != nullptr);
  QVERIFY(status != nullptr);
  const auto source =
      QString{
          "# consent test %1\ndef create_strategy(params):\n    return None\n"}
          .arg(directory.path());
  editor->setPlainText(source);

  const auto scheduleConsentChoice =
      [promptCount, informativeText](const QString &buttonText) {
        QTimer::singleShot(0, qApp, [buttonText, promptCount, informativeText] {
          auto *dialog =
              qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
          if (dialog == nullptr) {
            return;
          }
          ++*promptCount;
          *informativeText = dialog->informativeText();
          for (auto *button : dialog->buttons()) {
            if (button->text() == buttonText) {
              button->click();
              return;
            }
          }
        });
      };

  scheduleConsentChoice("Cancel");
  QTest::mouseClick(validate, Qt::LeftButton);
  QVERIFY(bte::test::waitUntil(
      [status] { return status->text().contains("Consent was not granted"); },
      std::chrono::milliseconds{5000}));
  QCOMPARE(validationCalls.load(), 0);
  QCOMPARE(*promptCount, 1);
  QVERIFY(informativeText->contains("API version: 1"));
  QVERIFY(informativeText->contains("Source SHA-256:"));

  scheduleConsentChoice("Allow this source");
  QTest::mouseClick(validate, Qt::LeftButton);
  QVERIFY(bte::test::waitUntil(
      [status] { return status->text().contains("Strategy is valid"); },
      std::chrono::milliseconds{5000}));
  QCOMPARE(validationCalls.load(), 1);
  QCOMPARE(*promptCount, 2);

  QTest::mouseClick(validate, Qt::LeftButton);
  QVERIFY(bte::test::waitUntil(
      [&validationCalls] { return validationCalls.load() == 2; },
      std::chrono::milliseconds{5000}));
  QVERIFY(bte::test::waitUntil([validate] { return validate->isEnabled(); },
                               std::chrono::milliseconds{5000}));
  QCOMPARE(*promptCount, 2);

  editor->setPlainText(source + "# edited source identity\n");
  scheduleConsentChoice("Cancel");
  QTest::mouseClick(validate, Qt::LeftButton);
  QVERIFY(bte::test::waitUntil(
      [status] { return status->text().contains("Consent was not granted"); },
      std::chrono::milliseconds{5000}));
  QCOMPARE(validationCalls.load(), 2);
  QCOMPARE(*promptCount, 3);
}

void StrategyTabTest::reportsValidationWorkerFailures() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  std::atomic_int invocation = 0;
  bte::frontend::StrategyTab tab{
      std::filesystem::path{directory.path().toStdString()},
      [&invocation](
          const bte::bindings::PythonStrategyConfiguration &,
          const bte::core::CancellationToken &) -> bte::core::Result<void> {
        switch (invocation.fetch_add(1)) {
        case 0:
          return bte::core::Result<void>{bte::core::makeError(
              bte::core::ErrorCode::invalidArgument, "strategy rejected")};
        case 1:
          throw std::runtime_error{"worker exception"};
        default:
          throw 42;
        }
      },
      [](const std::string &) { return true; }};
  tab.findChild<QPlainTextEdit *>("pythonStrategySourceEditor")
      ->setPlainText("def create_strategy(params):\n    return None\n");
  auto *validate = tab.findChild<QPushButton *>("pythonStrategyValidateButton");
  auto *status = tab.findChild<QLabel *>("pythonStrategyStatusLabel");
  auto *diagnostics =
      tab.findChild<QPlainTextEdit *>("pythonStrategyDiagnostics");
  QVERIFY(validate != nullptr);
  QVERIFY(status != nullptr);
  QVERIFY(diagnostics != nullptr);

  QTest::mouseClick(validate, Qt::LeftButton);
  QVERIFY(bte::test::waitUntil(
      [status] { return status->text() == "Validation failed"; },
      std::chrono::milliseconds{5000}));
  QCOMPARE(diagnostics->toPlainText(), QString{"strategy rejected"});

  QTest::mouseClick(validate, Qt::LeftButton);
  QVERIFY(bte::test::waitUntil(
      [status] { return status->text() == "Validation failed"; },
      std::chrono::milliseconds{5000}));
  QCOMPARE(diagnostics->toPlainText(),
           QString{"validation worker failed: worker exception"});

  QTest::mouseClick(validate, Qt::LeftButton);
  QVERIFY(bte::test::waitUntil(
      [status] { return status->text() == "Validation failed"; },
      std::chrono::milliseconds{5000}));
  QCOMPARE(diagnostics->toPlainText(), QString{"validation worker failed"});
  QCOMPARE(invocation.load(), 3);
}

void StrategyTabTest::explainsWhenManagedValidationIsUnavailable() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  bte::frontend::StrategyTab tab{
      std::filesystem::path{directory.path().toStdString()}, {}};
  auto *validate = tab.findChild<QPushButton *>("pythonStrategyValidateButton");
  auto *status = tab.findChild<QLabel *>("pythonStrategyStatusLabel");
  QVERIFY(validate != nullptr);
  QVERIFY(status != nullptr);

  QTest::mouseClick(validate, Qt::LeftButton);

  QCOMPARE(status->text(), QString{"Managed Python validation is unavailable"});
  QVERIFY(validate->isEnabled());
}

void StrategyTabTest::
    usesDefaultManagedValidationRunnerFromApplicationResources() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  bte::frontend::StrategyTab tab;
  auto *editor = tab.findChild<QPlainTextEdit *>("pythonStrategySourceEditor");
  auto *validate = tab.findChild<QPushButton *>("pythonStrategyValidateButton");
  auto *status = tab.findChild<QLabel *>("pythonStrategyStatusLabel");
  auto *diagnostics =
      tab.findChild<QPlainTextEdit *>("pythonStrategyDiagnostics");
  QVERIFY(editor != nullptr);
  QVERIFY(validate != nullptr);
  QVERIFY(status != nullptr);
  QVERIFY(diagnostics != nullptr);
  editor->setPlainText(
      QString{"# unique validation source %1\n"
              "def create_strategy(params):\n    return None\n"}
          .arg(directory.path()));

  const auto consent = chooseReplacementButton("Allow this source");
  QTest::mouseClick(validate, Qt::LeftButton);

  QVERIFY(bte::test::waitUntil(
      [status] { return status->text() == "Validation failed"; },
      std::chrono::milliseconds{5000}));
  QVERIFY(*consent);
  QVERIFY(diagnostics->toPlainText().contains("runtime"));
}

void StrategyTabTest::cancellationReachesTheValidationWorker() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  std::atomic_bool started = false;
  std::atomic_bool observedCancellation = false;
  bte::frontend::StrategyTab tab{
      std::filesystem::path{directory.path().toStdString()},
      [&started, &observedCancellation](
          const bte::bindings::PythonStrategyConfiguration &,
          const bte::core::CancellationToken &cancellation) {
        started.store(true);
        while (!cancellation.isCancellationRequested()) {
          std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
        observedCancellation.store(true);
        return bte::core::Result<void>{bte::core::makeError(
            bte::core::ErrorCode::cancelled, "validation canceled")};
      },
      [](const std::string &) { return true; }};
  auto *validate = tab.findChild<QPushButton *>("pythonStrategyValidateButton");
  auto *cancel =
      tab.findChild<QPushButton *>("pythonStrategyCancelValidationButton");
  auto *status = tab.findChild<QLabel *>("pythonStrategyStatusLabel");
  auto *diagnostics =
      tab.findChild<QPlainTextEdit *>("pythonStrategyDiagnostics");
  QVERIFY(validate != nullptr);
  QVERIFY(cancel != nullptr);
  QVERIFY(status != nullptr);
  QVERIFY(diagnostics != nullptr);

  QTest::mouseClick(validate, Qt::LeftButton);
  QVERIFY(bte::test::waitUntil([&started] { return started.load(); },
                               std::chrono::milliseconds{5000}));
  QVERIFY(cancel->isEnabled());
  QTest::mouseClick(cancel, Qt::LeftButton);
  QVERIFY(bte::test::waitUntil(
      [&observedCancellation, validate] {
        return observedCancellation.load() && validate->isEnabled();
      },
      std::chrono::milliseconds{5000}));
  QCOMPARE(status->text(), QString{"Validation failed"});
  QCOMPARE(diagnostics->toPlainText(), QString{"validation canceled"});
  QVERIFY(!cancel->isEnabled());
}

void StrategyTabTest::cannotSaveDraftWhenItsDestinationIsUnavailable() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const auto root = std::filesystem::path{directory.path().toStdString()};
  const auto draftDirectory = root / "drafts";
  QVERIFY(std::filesystem::create_directories(draftDirectory));
  const auto blockedDraftPath = draftDirectory / "python-draft.json";
  QVERIFY(std::filesystem::create_directory(blockedDraftPath));
  bte::frontend::StrategyTab tab{draftDirectory, {}};
  tab.findChild<QPlainTextEdit *>("pythonStrategySourceEditor")
      ->setPlainText("def create_strategy(params):\n    return None\n");
  QVERIFY(!tab.saveDraft());
  QVERIFY(tab.findChild<QLabel *>("pythonStrategyStatusLabel")
              ->text()
              .contains("Cannot atomically write the Strategy draft"));
}

void StrategyTabTest::autosaveReportsWhenItsDraftDirectoryIsUnavailable() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const auto root = std::filesystem::path{directory.path().toStdString()};
  const auto blockedDirectory = root / "drafts";
  QVERIFY(writeSourceFile(blockedDirectory, QByteArray{"not a directory"}));
  bte::frontend::StrategyTab tab{blockedDirectory, {}};
  auto *editor = tab.findChild<QPlainTextEdit *>("pythonStrategySourceEditor");
  auto *status = tab.findChild<QLabel *>("pythonStrategyStatusLabel");
  QVERIFY(editor != nullptr);
  QVERIFY(status != nullptr);

  editor->setPlainText("def create_strategy(params):\n    return None\n");

  QVERIFY(bte::test::waitUntil(
      [status] {
        return status->text().contains(
            "Cannot save draft to the local data folder");
      },
      std::chrono::milliseconds{2000}));
  QVERIFY(std::filesystem::is_regular_file(blockedDirectory));
}

void StrategyTabTest::
    preservesCurrentDraftWhenItsRecoveryCopyCannotBeWritten() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const auto root = std::filesystem::path{directory.path().toStdString()};
  const auto draftDirectory = root / "drafts";
  bte::frontend::StrategyTab tab{draftDirectory, {}};
  auto *editor = tab.findChild<QPlainTextEdit *>("pythonStrategySourceEditor");
  auto *status = tab.findChild<QLabel *>("pythonStrategyStatusLabel");
  QVERIFY(editor != nullptr);
  QVERIFY(status != nullptr);
  editor->setPlainText("first valid generation\n");
  QVERIFY(tab.saveDraft());
  const auto currentPath = draftDirectory / "python-draft.json";
  QFile currentBefore{QString::fromStdString(currentPath.string())};
  QVERIFY(currentBefore.open(QIODevice::ReadOnly));
  const auto currentBytes = currentBefore.readAll();
  QVERIFY(std::filesystem::create_directory(draftDirectory /
                                            "python-draft.previous.json"));

  editor->setPlainText("second generation\n");

  QVERIFY(!tab.saveDraft());
  QVERIFY(status->text().contains("Cannot preserve the previous valid draft"));
  QFile currentAfter{QString::fromStdString(currentPath.string())};
  QVERIFY(currentAfter.open(QIODevice::ReadOnly));
  QCOMPARE(currentAfter.readAll(), currentBytes);
}

void StrategyTabTest::ignoresValidationForAnEditedDraft() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  std::promise<void> entered;
  std::promise<void> release;
  auto releaseFuture = release.get_future().share();
  auto runner = [&entered, releaseFuture](
                    const bte::bindings::PythonStrategyConfiguration &,
                    const bte::core::CancellationToken &) {
    entered.set_value();
    releaseFuture.wait();
    return bte::core::Result<void>{};
  };
  bte::frontend::StrategyTab tab{
      std::filesystem::path{directory.path().toStdString()}, runner,
      [](const std::string &) { return true; }};
  auto *editor = tab.findChild<QPlainTextEdit *>("pythonStrategySourceEditor");
  editor->setPlainText("def create_strategy(params):\n    return None\n");
  QTest::mouseClick(
      tab.findChild<QPushButton *>("pythonStrategyValidateButton"),
      Qt::LeftButton);
  entered.get_future().wait();
  editor->appendPlainText("# changed while worker was running");
  release.set_value();
  QVERIFY(bte::test::waitUntil(
      [&tab] {
        return tab.findChild<QLabel *>("pythonStrategyStatusLabel")
            ->text()
            .contains("earlier result was ignored");
      },
      std::chrono::milliseconds{5000}));
}

void StrategyTabTest::useInBacktestEmitsTheCurrentSourceAndParameters() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  bte::frontend::StrategyTab tab{
      std::filesystem::path{directory.path().toStdString()}, {}};
  auto *editor = tab.findChild<QPlainTextEdit *>("pythonStrategySourceEditor");
  editor->setPlainText("def create_strategy(params):\n    return None\n");
  QSignalSpy spy{&tab, &bte::frontend::StrategyTab::useInBacktest};
  QTest::mouseClick(
      tab.findChild<QPushButton *>("pythonStrategyUseInBacktestButton"),
      Qt::LeftButton);
  QCOMPARE(spy.size(), 1);
  QCOMPARE(spy.at(0).at(0).toString(), editor->toPlainText());
  QCOMPARE(spy.at(0).at(1).toString(), tab.canonicalParametersJson());
}

void StrategyTabTest::exportsNormalizedUtf8SourceAtomically() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  bte::frontend::StrategyTab tab{
      std::filesystem::path{directory.path().toStdString()}, {}};
  const QString source{u"# strategy \U0001F9EA\r\n"
                       u"def create_strategy(params):\r\n    return None\r"};
  tab.findChild<QPlainTextEdit *>("pythonStrategySourceEditor")
      ->setPlainText(source);
  const auto destination =
      std::filesystem::path{directory.path().toStdString()} / "strategy.py";
  QVERIFY(tab.exportSourceFile(destination));
  QFile exported{QString::fromStdString(destination.string())};
  QVERIFY(exported.open(QIODevice::ReadOnly));
  const QByteArray normalizedSource{
      "# strategy \xf0\x9f\xa7\xaa\n"
      "def create_strategy(params):\n    return None\n"};
  QCOMPARE(exported.readAll(), normalizedSource);
  QVERIFY(!tab.exportSourceFile(destination.parent_path() / "strategy.txt"));
  QCOMPARE(tab.sourceUtf8(), normalizedSource.toStdString());
}

void StrategyTabTest::reportsWhenThePythonExportDestinationCannotBeWritten() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const auto root = std::filesystem::path{directory.path().toStdString()};
  const auto blockedParent = root / "file-parent";
  QVERIFY(writeSourceFile(blockedParent, QByteArray{"not a directory"}));
  bte::frontend::StrategyTab tab{root / "drafts", {}};
  tab.findChild<QPlainTextEdit *>("pythonStrategySourceEditor")
      ->setPlainText("def create_strategy(params):\n    return None\n");

  QVERIFY(!tab.exportSourceFile(blockedParent / "strategy.py"));

  QVERIFY(tab.findChild<QLabel *>("pythonStrategyStatusLabel")
              ->text()
              .contains("Cannot write Python source file"));

  const auto occupiedTarget = root / "occupied.py";
  QVERIFY(std::filesystem::create_directories(occupiedTarget));
  const auto preservedFile = occupiedTarget / "existing.txt";
  QVERIFY(writeSourceFile(preservedFile, QByteArray{"keep existing data"}));
  QVERIFY(!tab.exportSourceFile(occupiedTarget));
  const auto status =
      tab.findChild<QLabel *>("pythonStrategyStatusLabel")->text();
  QVERIFY(status.contains("Cannot write Python source file") ||
          status.contains("Cannot atomically export Python source file"));
  QFile preserved{QString::fromStdString(preservedFile.string())};
  QVERIFY(preserved.open(QIODevice::ReadOnly));
  QCOMPARE(preserved.readAll(), QByteArray{"keep existing data"});
}

void StrategyTabTest::rejectsUnpairedSurrogatesBeforeValidationOrExport() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  std::atomic_int validationCalls = 0;
  const auto root = std::filesystem::path{directory.path().toStdString()};
  bte::frontend::StrategyTab tab{
      root,
      [&validationCalls](const bte::bindings::PythonStrategyConfiguration &,
                         const bte::core::CancellationToken &) {
        ++validationCalls;
        return bte::core::Result<void>{};
      },
      [](const std::string &) { return true; }};
  auto *editor = tab.findChild<QPlainTextEdit *>("pythonStrategySourceEditor");
  auto *validate = tab.findChild<QPushButton *>("pythonStrategyValidateButton");
  auto *status = tab.findChild<QLabel *>("pythonStrategyStatusLabel");
  QVERIFY(editor != nullptr);
  QVERIFY(validate != nullptr);
  QVERIFY(status != nullptr);

  editor->setPlainText(QString{QChar{0xd800}});
  QCOMPARE(editor->toPlainText().size(), 1);
  QCOMPARE(editor->toPlainText().front().unicode(), ushort{0xd800});
  QTest::mouseClick(validate, Qt::LeftButton);
  QCOMPARE(validationCalls.load(), 0);
  QVERIFY(status->text().contains("Source is invalid"));

  editor->setPlainText(QString{QChar{0xdc00}});
  QCOMPARE(editor->toPlainText().size(), 1);
  QCOMPARE(editor->toPlainText().front().unicode(), ushort{0xdc00});
  const auto exportPath = root / "invalid-surrogate.py";
  QVERIFY(!tab.exportSourceFile(exportPath));
  QVERIFY(status->text().contains("Source must be valid UTF-8"));
  QVERIFY(!std::filesystem::exists(exportPath));
}

void StrategyTabTest::
    builtInSampleHandlesIncompleteHistoryAndEmitsValidOrder() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  bte::frontend::StrategyTab tab{
      std::filesystem::path{directory.path().toStdString()}, {}};
  auto created = bte::bindings::python::PythonWorkerStrategy::createForTesting(
      BTE_TEST_PYTHON_EXECUTABLE, BTE_PYTHON_WORKER_BOOTSTRAP_PATH,
      tab.sourceUtf8(),
      nlohmann::json{{"fast_period", 2}, {"slow_period", 3}, {"quantity", 1}},
      true);
  QVERIFY2(created.ok(), created.ok() ? "" : created.error().message.c_str());
  auto strategy = std::move(created).value();

  auto positionShares = std::int64_t{};
  const auto market = [&strategy, &positionShares](
                          const double fast, const std::optional<double> slow,
                          const std::int64_t timestamp) {
    const auto indicator = [](const double value) {
      return bte::indicators::IndicatorValue{
          .value = value, .domain = bte::indicators::NumericDomain::scalar};
    };
    const std::array<bte::strategy::StrategyIndicatorValue, 2> indicators{{
        {.name = "fast", .value = indicator(fast)},
        {.name = "slow",
         .value =
             slow.has_value() ? std::optional{indicator(*slow)} : std::nullopt},
    }};
    const bte::strategy::StrategyContext context{
        .symbol = "AAPL",
        .bar =
            bte::core::Bar{.ts = bte::core::Timestamp{std::chrono::milliseconds{
                               timestamp}},
                           .open = 10.0,
                           .high = 11.0,
                           .low = 9.0,
                           .close = 10.5,
                           .volume = 100.0},
        .cashMicrodollars = 100'000'000,
        .equityMicrodollars = 100'000'000,
        .positionShares = positionShares,
        .markPriceNanodollars = 10'500'000'000,
        .indicatorValues = indicators};
    return strategy->onMarket(context);
  };

  const auto incomplete = market(1.0, std::nullopt, 1'700'000'000'000LL);
  QVERIFY2(incomplete.ok(),
           incomplete.ok() ? "" : incomplete.error().message.c_str());
  QVERIFY(incomplete.value().empty());

  const auto firstComplete = market(1.0, 2.0, 1'700'000'003'600'000LL);
  QVERIFY2(firstComplete.ok(),
           firstComplete.ok() ? "" : firstComplete.error().message.c_str());
  QVERIFY(firstComplete.value().empty());

  const auto crossover = market(3.0, 2.0, 1'700'000'007'200'000LL);
  QVERIFY2(crossover.ok(),
           crossover.ok() ? "" : crossover.error().message.c_str());
  QCOMPARE(crossover.value().size(), 1U);
  QCOMPARE(crossover.value().front().kind,
           bte::strategy::StrategyCommandKind::buyMarket);
  QCOMPARE(crossover.value().front().quantityShares, 1);
  positionShares = 1;

  const auto downCross = market(1.0, 2.0, 1'700'000'010'800'000LL);
  QVERIFY2(downCross.ok(),
           downCross.ok() ? "" : downCross.error().message.c_str());
  QCOMPARE(downCross.value().size(), 1U);
  QCOMPARE(downCross.value().front().kind,
           bte::strategy::StrategyCommandKind::closePosition);
}

void StrategyTabTest::preservesUnrecoverableDraftsWhenTheTabCloses() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const auto path = std::filesystem::path{directory.path().toStdString()};
  const auto currentPath = path / "python-draft.json";
  const auto previousPath = path / "python-draft.previous.json";
  const QByteArray currentBytes{"{invalid-current"};
  const QByteArray previousBytes{"{invalid-previous"};
  for (const auto &[filePath, bytes] :
       {std::pair{currentPath, currentBytes},
        std::pair{previousPath, previousBytes}}) {
    QFile file{QString::fromStdString(filePath.string())};
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write(bytes), static_cast<qint64>(bytes.size()));
  }

  {
    bte::frontend::StrategyTab tab{path, {}};
    const auto *editor =
        tab.findChild<QPlainTextEdit *>("pythonStrategySourceEditor");
    QVERIFY(editor != nullptr);
    QVERIFY(editor->toPlainText().isEmpty());
  }

  for (const auto &[filePath, expected] :
       {std::pair{currentPath, currentBytes},
        std::pair{previousPath, previousBytes}}) {
    QFile file{QString::fromStdString(filePath.string())};
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.readAll(), expected);
  }

  {
    bte::frontend::StrategyTab tab{path, {}};
    tab.findChild<QPlainTextEdit *>("pythonStrategySourceEditor")
        ->setPlainText("def create_strategy(params):\n    return None\n");
    QVERIFY(tab.autosaveDraft());
    QFile current{QString::fromStdString(currentPath.string())};
    QVERIFY(current.open(QIODevice::ReadOnly));
    QCOMPARE(current.readAll(), currentBytes);
    QVERIFY(tab.saveDraft());
  }
  QFile replaced{QString::fromStdString(currentPath.string())};
  QVERIFY(replaced.open(QIODevice::ReadOnly));
  QVERIFY(replaced.readAll() != currentBytes);
}

} // namespace

QTEST_MAIN(StrategyTabTest)

#include "UnitTest_StrategyTab.moc"
