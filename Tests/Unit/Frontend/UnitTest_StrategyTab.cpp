#include "Bte/Frontend/StrategyTab.h"

#include "PythonWorkerStrategy.h"

#include <Bte/Core/Bar.h>
#include <Bte/Strategy/Strategy.h>

#include <QFile>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSignalSpy>
#include <QSpinBox>
#include <QTemporaryDir>
#include <QTest>

#include <atomic>
#include <array>
#include <chrono>
#include <filesystem>
#include <future>
#include <optional>
#include <string>

namespace {

class StrategyTabTest final : public QObject {
  Q_OBJECT

private slots:
  void exposesSampleAndPersistsRecoverableDrafts();
  void importsUtf8NormalizesLineEndingsAndRejectsInvalidFiles();
  void validatesWithFrozenParametersAndConsent();
  void ignoresValidationForAnEditedDraft();
  void useInBacktestEmitsTheCurrentSourceAndParameters();
  void exportsNormalizedUtf8SourceAtomically();
  void builtInSampleHandlesIncompleteHistoryAndEmitsValidOrder();
  void preservesUnrecoverableDraftsWhenTheTabCloses();
};

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
  QTRY_VERIFY_WITH_TIMEOUT(tab.findChild<QLabel *>("pythonStrategyStatusLabel")
                               ->text()
                               .contains("Strategy is valid"),
                           5000);
  QCOMPARE(calls.load(), 1);
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
  QTRY_VERIFY_WITH_TIMEOUT(tab.findChild<QLabel *>("pythonStrategyStatusLabel")
                               ->text()
                               .contains("earlier result was ignored"),
                           5000);
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
  const QString source{"def create_strategy(params):\r\n    return None\r"};
  tab.findChild<QPlainTextEdit *>("pythonStrategySourceEditor")
      ->setPlainText(source);
  const auto destination =
      std::filesystem::path{directory.path().toStdString()} / "strategy.py";
  QVERIFY(tab.exportSourceFile(destination));
  QFile exported{QString::fromStdString(destination.string())};
  QVERIFY(exported.open(QIODevice::ReadOnly));
  QCOMPARE(exported.readAll(),
           QByteArray{"def create_strategy(params):\n    return None\n"});
  QVERIFY(!tab.exportSourceFile(destination.parent_path() / "strategy.txt"));
  QCOMPARE(tab.sourceUtf8(),
           std::string{"def create_strategy(params):\n    return None\n"});
}

void StrategyTabTest::builtInSampleHandlesIncompleteHistoryAndEmitsValidOrder() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  bte::frontend::StrategyTab tab{
      std::filesystem::path{directory.path().toStdString()}, {}};
  auto created = bte::bindings::python::PythonWorkerStrategy::createForTesting(
      BTE_TEST_PYTHON_EXECUTABLE, BTE_PYTHON_WORKER_BOOTSTRAP_PATH,
      tab.sourceUtf8(),
      nlohmann::json{{"fast_period", 2}, {"slow_period", 3},
                     {"quantity", 1}},
      true);
  QVERIFY2(created.ok(),
           created.ok() ? "" : created.error().message.c_str());
  auto strategy = std::move(created).value();

  const auto market = [&strategy](const double fast,
                                  const std::optional<double> slow,
                                  const std::int64_t timestamp) {
    const auto indicator = [](const double value) {
      return bte::indicators::IndicatorValue{
          .value = value,
          .domain = bte::indicators::NumericDomain::scalar};
    };
    const std::array<bte::strategy::StrategyIndicatorValue, 2> indicators{{
        {.name = "fast", .value = indicator(fast)},
        {.name = "slow",
         .value = slow.has_value()
                      ? std::optional{indicator(*slow)}
                      : std::nullopt},
    }};
    const bte::strategy::StrategyContext context{
        .symbol = "AAPL",
        .bar = bte::core::Bar{
            .ts = bte::core::Timestamp{std::chrono::milliseconds{timestamp}},
            .open = 10.0,
            .high = 11.0,
            .low = 9.0,
            .close = 10.5,
            .volume = 100.0},
        .cashMicrodollars = 100'000'000,
        .equityMicrodollars = 100'000'000,
        .positionShares = 0,
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
