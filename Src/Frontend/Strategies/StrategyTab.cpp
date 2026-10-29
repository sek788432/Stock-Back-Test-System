#include "Bte/Frontend/StrategyTab.h"

#include "Bte/Core/CanonicalJson.h"
#include "Bte/Core/Digest.h"

#include <QChar>
#include <QCoreApplication>
#include <QDebug>
#include <QFile>
#include <QFileDialog>
#include <QFont>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSaveFile>
#include <QSettings>
#include <QSpinBox>
#include <QStandardPaths>
#include <QStringDecoder>
#include <QTimer>
#include <QVariant>
#include <QWidget>
#include <QtConcurrent/QtConcurrentRun>
#include <QtCore/qbytearray.h>
#include <QtCore/qiodevice.h>
#include <QtGlobal>
#include <cstdint>
#include <exception>
#include <map>

#include <cstddef>
#include <filesystem>
#include <memory>
#include <nlohmann/detail/json_ref.hpp>
#include <nlohmann/detail/output/serializer.hpp>
#include <nlohmann/json.hpp>
#include <nlohmann/json_fwd.hpp>
#include <optional>
#include <string>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>

namespace bte::frontend {
namespace {

constexpr std::size_t maximumStrategySourceBytes =
    std::size_t{1024} * std::size_t{1024};
constexpr auto draftSchemaVersion = 1;

constexpr auto sampleSource = R"PY(import stockbt

STOCKBT_API_VERSION = "1"
STOCKBT_TEMPLATE_VERSION = "1.0.0"


def create_strategy(params):
    fast_period = params["fast_period"]
    slow_period = params["slow_period"]
    quantity = params["quantity"]

    class SmaCrossover:
        max_history_bars = slow_period
        indicators = {
            "fast": stockbt.SMA("fast", period=fast_period),
            "slow": stockbt.SMA("slow", period=slow_period),
        }

        def __init__(self):
            self.previous = None

        def on_market(self, ctx):
            fast = ctx.indicators["fast"]
            slow = ctx.indicators["slow"]
            if fast is None or slow is None:
                return

            previous = self.previous
            self.previous = (fast, slow)
            if previous is None:
                return

            previous_fast, previous_slow = previous
            position = ctx.portfolio.positions.get(ctx.symbol)
            shares = 0 if position is None else position.quantity_shares

            if shares == 0 and previous_fast <= previous_slow and fast > slow:
                ctx.orders.buy_market(ctx.symbol, quantity=quantity)
            elif shares > 0 and previous_fast >= previous_slow and fast < slow:
                ctx.orders.close_position(ctx.symbol)

    return SmaCrossover()
)PY";

struct StrategyDraft final {
  QString source;
  int fastPeriod = 5;
  int slowPeriod = 20;
  int quantity = 1;
};

QString normalizedSource(QString source) {
  source.replace("\r\n", "\n");
  source.replace('\r', '\n');
  return source;
} // GCOVR_EXCL_LINE: the compiler emits no executable instruction here.

std::optional<StrategyDraft> readDraft(const std::filesystem::path &path) {
  QFile file{QString::fromStdString(path.string())};
  if (!file.open(QIODevice::ReadOnly) ||
      file.size() > qint64{2} * 1024 * 1024) {
    return std::nullopt;
  }
  const auto parsed =
      nlohmann::json::parse(file.readAll().toStdString(), nullptr, false);
  if (parsed.is_discarded() || !parsed.is_object() ||
      !parsed.contains("schemaVersion") ||
      !parsed["schemaVersion"].is_number_integer() ||
      parsed["schemaVersion"] != draftSchemaVersion ||
      !parsed.contains("source") || !parsed["source"].is_string() ||
      !parsed.contains("fastPeriod") ||
      !parsed["fastPeriod"].is_number_integer() ||
      !parsed.contains("slowPeriod") ||
      !parsed["slowPeriod"].is_number_integer() ||
      !parsed.contains("quantity") || !parsed["quantity"].is_number_integer()) {
    return std::nullopt;
  }
  const auto source = QString::fromUtf8(
      QByteArray::fromStdString(parsed["source"].get<std::string>()));
  const auto sourceBytes = normalizedSource(source).toUtf8();
  if (source.contains(QChar::Null) || sourceBytes.size() < 1 ||
      static_cast<std::size_t>(sourceBytes.size()) >
          maximumStrategySourceBytes) {
    return std::nullopt;
  }
  const auto boundedInteger =
      [](const nlohmann::json &value, const std::int64_t minimum,
         const std::int64_t maximum) -> std::optional<std::int64_t> {
    if (value.is_number_unsigned()) {
      const auto number = value.get<std::uint64_t>();
      if (maximum < 0 || number > static_cast<std::uint64_t>(maximum) ||
          (minimum > 0 && number < static_cast<std::uint64_t>(minimum))) {
        return std::nullopt;
      }
      return static_cast<std::int64_t>(number);
    }
    const auto number = value.get<std::int64_t>();
    if (number < minimum || number > maximum) {
      return std::nullopt;
    }
    return number;
  };
  const auto fast = boundedInteger(parsed["fastPeriod"], 1, 10'000);
  const auto slow = boundedInteger(parsed["slowPeriod"], 1, 10'000);
  const auto quantity = boundedInteger(parsed["quantity"], 1, 1'000'000'000);
  if (!fast.has_value() || !slow.has_value() || !quantity.has_value() ||
      *fast >= *slow) {
    return std::nullopt;
  }
  return StrategyDraft{.source = normalizedSource(source),
                       .fastPeriod = static_cast<int>(*fast),
                       .slowPeriod = static_cast<int>(*slow),
                       .quantity = static_cast<int>(*quantity)};
}

QByteArray draftBytes(const StrategyDraft &draft) {
  const nlohmann::json value{
      {"fastPeriod", draft.fastPeriod},
      {"quantity", draft.quantity},
      {"schemaVersion", draftSchemaVersion},
      {"slowPeriod", draft.slowPeriod},
      {"source", draft.source.toUtf8().toStdString()},
  };
  return QByteArray::fromStdString(
      value.dump(-1, ' ', false, nlohmann::json::error_handler_t::strict));
}

bool writeAtomically(const std::filesystem::path &path,
                     const QByteArray &bytes) {
  std::error_code error;
  std::filesystem::create_directories(path.parent_path(), error);
  if (error) {
    return false;
  }
  QSaveFile file{QString::fromStdString(path.string())};
  if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size()) {
    file.cancelWriting();
    return false;
  }
  return file.commit();
}

QString validationErrorText(const core::Error &error) {
  return QString::fromStdString(error.message);
}

core::Result<void>
validationWorkerFailure(const char *detail = nullptr) noexcept {
  try {
    auto message = std::string{"validation worker failed"};
    if (detail != nullptr) {
      message.append(": ");
      message.append(detail);
    }
    return core::Result<void>{
        core::makeError(core::ErrorCode::internal, std::move(message))};
  } catch (...) {
    auto error = core::Error{};
    error.code = core::ErrorCode::internal;
    return core::Result<void>{std::move(error)};
  } // GCOVR_EXCL_LINE: the compiler emits no executable instruction here.
}

core::Result<void>
runValidationSafely(const StrategyTab::ValidationRunner &runner,
                    const bindings::PythonStrategyConfiguration &configuration,
                    const core::CancellationToken &cancellation) noexcept {
  try {
    return runner(configuration, cancellation);
  } catch (const std::exception &error) {
    return validationWorkerFailure(error.what());
  } catch (...) {
    return validationWorkerFailure();
  }
}

} // namespace

struct StrategyTab::State final {
  using ValidationResult = core::Result<void>;

  State() = default;
  ~State() {
    validationCancellation.requestCancellation();
    validationWatcher.waitForFinished();
  }

  State(const State &) = delete;
  State &operator=(const State &) = delete;
  State(State &&) = delete;
  State &operator=(State &&) = delete;

  StrategyTab *owner = nullptr;
  std::filesystem::path draftDirectory;
  ValidationRunner validationRunner;
  ConsentProvider consentProvider;
  QPlainTextEdit *editor = nullptr;
  QSpinBox *fastPeriod = nullptr;
  QSpinBox *slowPeriod = nullptr;
  QSpinBox *quantity = nullptr;
  QPushButton *saveDraft = nullptr;
  QPushButton *loadSource = nullptr;
  QPushButton *exportSource = nullptr;
  QPushButton *validate = nullptr;
  QPushButton *cancelValidation = nullptr;
  QPushButton *useInBacktest = nullptr;
  QLabel *status = nullptr;
  QPlainTextEdit *diagnostics = nullptr;
  QTimer *draftTimer = nullptr;
  QFutureWatcher<ValidationResult> validationWatcher;
  core::CancellationSource validationCancellation;
  QString savedFingerprint;
  std::uint64_t draftGeneration = 0;
  std::uint64_t validationGeneration = 0;
  bool loadingDraft = false;
  bool preserveUnrecoverableDrafts = false;
};

namespace {

StrategyDraft currentDraft(const StrategyTab::State &state) {
  return {.source = normalizedSource(state.editor->toPlainText()),
          .fastPeriod = state.fastPeriod->value(),
          .slowPeriod = state.slowPeriod->value(),
          .quantity = state.quantity->value()};
} // GCOVR_EXCL_LINE: the compiler emits no executable instruction here.

QByteArray fingerprint(const StrategyDraft &draft) { return draftBytes(draft); }

QString consentKey(const std::string &sourceHash) {
  return QString{"pythonStrategy/consent/api-1/%1"}.arg(
      QString::fromStdString(sourceHash));
} // GCOVR_EXCL_LINE: the compiler emits no executable instruction here.

bool requestSourceConsent(const std::string &sourceHash) {
  QSettings settings;
  const auto key = consentKey(sourceHash);
  if (settings.value(key, false).toBool()) {
    return true;
  }
  QMessageBox dialog{QMessageBox::Warning,
                     QObject::tr("Allow Python Strategy code?"),
                     QObject::tr("Python strategies run trusted local code. "
                                 "They are not sandboxed and may access the "
                                 "same account and files as this application."),
                     QMessageBox::NoButton};
  dialog.setInformativeText(QObject::tr("API version: 1\nSource SHA-256: %1")
                                .arg(QString::fromStdString(sourceHash)));
  auto *allow = dialog.addButton(QObject::tr("Allow this source"),
                                 QMessageBox::AcceptRole);
  dialog.addButton(QMessageBox::Cancel);
  dialog.exec();
  if (dialog.clickedButton() != allow) {
    return false;
  }
  settings.setValue(key, true);
  settings.sync();
  return settings.status() == QSettings::NoError;
}

bool confirmBufferReplacement(StrategyTab::State &state) {
  if (fingerprint(currentDraft(state)) == state.savedFingerprint) {
    return true;
  }
  QMessageBox dialog{QMessageBox::Warning,
                     QObject::tr("Unsaved Strategy changes"),
                     QObject::tr("Save the current draft before replacing it?"),
                     QMessageBox::NoButton, state.owner};
  auto *save =
      dialog.addButton(QObject::tr("Save Draft"), QMessageBox::AcceptRole);
  auto *discard =
      dialog.addButton(QObject::tr("Discard"), QMessageBox::DestructiveRole);
  dialog.addButton(QObject::tr("Cancel"), QMessageBox::RejectRole);
  dialog.exec();
  if (dialog.clickedButton() == save) {
    return state.owner->saveDraft();
  }
  return dialog.clickedButton() == discard;
}

bool validSource(QString &source, std::string &encoded) {
  source = normalizedSource(std::move(source));
  if (source.contains(QChar::Null)) {
    return false;
  }
  for (qsizetype index = 0; index < source.size(); ++index) {
    if (source[index].isHighSurrogate()) {
      if (index + 1 >= source.size() || !source[index + 1].isLowSurrogate()) {
        return false;
      }
      ++index;
    } else if (source[index].isLowSurrogate()) {
      return false;
    }
  }
  const auto bytes = source.toUtf8();
  if (bytes.isEmpty() ||
      static_cast<std::size_t>(bytes.size()) > maximumStrategySourceBytes) {
    return false;
  }
  encoded = bytes.toStdString();
  return true;
}

} // namespace

StrategyTab::StrategyTab(QWidget *parent)
    : StrategyTab(
          std::filesystem::path{QStandardPaths::writableLocation(
                                    QStandardPaths::AppLocalDataLocation)
                                    .toStdString()} /
              "Strategies" / "PythonDraft",
          [](const bindings::PythonStrategyConfiguration &configuration,
             const core::CancellationToken &cancellation) {
            const auto applicationPath = std::filesystem::path{
                QCoreApplication::applicationDirPath().toStdString()};
            auto resources = applicationPath / "Resources";
            if (applicationPath.filename() == "MacOS" &&
                applicationPath.parent_path().filename() == "Contents") {
              resources = applicationPath.parent_path() / "Resources";
            } // GCOVR_EXCL_LINE: the compiler emits no executable instruction
              // here.
            const auto bootstrap = resources / "Python" / "worker_bootstrap.py";
            return bindings::validatePythonStrategy(
                configuration,
                bindings::PythonWorkerPaths{
                    .resourcesRoot = resources,
                    .workerBootstrapPath = bootstrap,
                },
                cancellation);
          },
          {}, parent) {}

StrategyTab::StrategyTab(std::filesystem::path draftDirectory,
                         ValidationRunner validationRunner,
                         ConsentProvider consentProvider, QWidget *parent)
    : QWidget(parent), state_{std::make_unique<State>()} {
  state_->owner = this;
  state_->draftDirectory = std::move(draftDirectory);
  state_->validationRunner = std::move(validationRunner);
  state_->consentProvider = std::move(consentProvider);
  buildUi();
  connectUi();
  restoreSavedDraft();
}

void StrategyTab::buildUi() {
  auto *root = std::make_unique<QVBoxLayout>(this).release();
  root->setContentsMargins(18, 16, 18, 18);
  auto *title = std::make_unique<QLabel>(tr("Python Strategy"), this).release();
  title->setObjectName("pythonStrategyTitleLabel");
  title->setAccessibleName(title->text());
  root->addWidget(title);
  auto *description =
      std::make_unique<QLabel>(
          tr("Edit a Python strategy, validate it in the "
             "managed worker, then send this draft to Backtest."),
          this)
          .release();
  description->setObjectName("pythonStrategyDescriptionLabel");
  description->setWordWrap(true);
  root->addWidget(description);

  auto *parameters = std::make_unique<QWidget>(this).release();
  parameters->setObjectName("pythonStrategyParameters");
  auto *form = std::make_unique<QFormLayout>(parameters).release();
  state_->fastPeriod = std::make_unique<QSpinBox>(parameters).release();
  state_->fastPeriod->setObjectName("pythonStrategyFastPeriodSpinBox");
  state_->fastPeriod->setAccessibleName(tr("Fast SMA period"));
  state_->fastPeriod->setRange(1, 10'000);
  state_->fastPeriod->setValue(5);
  state_->slowPeriod = std::make_unique<QSpinBox>(parameters).release();
  state_->slowPeriod->setObjectName("pythonStrategySlowPeriodSpinBox");
  state_->slowPeriod->setAccessibleName(tr("Slow SMA period"));
  state_->slowPeriod->setRange(1, 10'000);
  state_->slowPeriod->setValue(20);
  state_->quantity = std::make_unique<QSpinBox>(parameters).release();
  state_->quantity->setObjectName("pythonStrategyQuantitySpinBox");
  state_->quantity->setAccessibleName(tr("Whole-share quantity"));
  state_->quantity->setRange(1, 1'000'000'000);
  state_->quantity->setValue(1);
  form->addRow(tr("Fast SMA period"), state_->fastPeriod);
  form->addRow(tr("Slow SMA period"), state_->slowPeriod);
  form->addRow(tr("Quantity (shares)"), state_->quantity);
  root->addWidget(parameters);

  auto *actions = std::make_unique<QWidget>(this).release();
  auto *actionLayout = std::make_unique<QHBoxLayout>(actions).release();
  actionLayout->setContentsMargins(0, 0, 0, 0);
  state_->loadSource =
      std::make_unique<QPushButton>(tr("Load .py"), actions).release();
  state_->loadSource->setObjectName("pythonStrategyLoadButton");
  state_->loadSource->setAccessibleName(tr("Load Python source file"));
  state_->exportSource =
      std::make_unique<QPushButton>(tr("Export .py"), actions).release();
  state_->exportSource->setObjectName("pythonStrategyExportButton");
  state_->exportSource->setAccessibleName(tr("Export Python source file"));
  state_->saveDraft =
      std::make_unique<QPushButton>(tr("Save Draft"), actions).release();
  state_->saveDraft->setObjectName("pythonStrategySaveDraftButton");
  state_->saveDraft->setAccessibleName(tr("Save Python Strategy draft"));
  state_->validate =
      std::make_unique<QPushButton>(tr("Validate"), actions).release();
  state_->validate->setObjectName("pythonStrategyValidateButton");
  state_->validate->setAccessibleName(tr("Validate Python Strategy"));
  state_->cancelValidation =
      std::make_unique<QPushButton>(tr("Cancel"), actions).release();
  state_->cancelValidation->setObjectName(
      "pythonStrategyCancelValidationButton");
  state_->cancelValidation->setAccessibleName(
      tr("Cancel Python Strategy validation"));
  state_->cancelValidation->setEnabled(false);
  state_->useInBacktest =
      std::make_unique<QPushButton>(tr("Use in Backtest"), actions).release();
  state_->useInBacktest->setObjectName("pythonStrategyUseInBacktestButton");
  state_->useInBacktest->setAccessibleName(tr("Use Python draft in Backtest"));
  actionLayout->addWidget(state_->loadSource);
  actionLayout->addWidget(state_->exportSource);
  actionLayout->addWidget(state_->saveDraft);
  actionLayout->addWidget(state_->validate);
  actionLayout->addWidget(state_->cancelValidation);
  actionLayout->addStretch(1);
  actionLayout->addWidget(state_->useInBacktest);
  root->addWidget(actions);

  state_->editor = std::make_unique<QPlainTextEdit>(this).release();
  state_->editor->setObjectName("pythonStrategySourceEditor");
  state_->editor->setAccessibleName(tr("Python Strategy source editor"));
  state_->editor->setLineWrapMode(QPlainTextEdit::NoWrap);
  auto editorFont = state_->editor->font();
  editorFont.setFamily("Menlo");
  editorFont.setStyleHint(QFont::Monospace);
  state_->editor->setFont(editorFont);
  state_->editor->setMinimumHeight(360);
  root->addWidget(state_->editor, 1);

  state_->status = std::make_unique<QLabel>(tr("Ready"), this).release();
  state_->status->setObjectName("pythonStrategyStatusLabel");
  state_->status->setAccessibleName(tr("Python Strategy status"));
  root->addWidget(state_->status);
  state_->diagnostics = std::make_unique<QPlainTextEdit>(this).release();
  state_->diagnostics->setObjectName("pythonStrategyDiagnostics");
  state_->diagnostics->setAccessibleName(tr("Python Strategy diagnostics"));
  state_->diagnostics->setReadOnly(true);
  state_->diagnostics->setMaximumHeight(150);
  root->addWidget(state_->diagnostics);

  state_->draftTimer = std::make_unique<QTimer>(this).release();
  state_->draftTimer->setSingleShot(true);
  state_->draftTimer->setInterval(500);
}

void StrategyTab::connectUi() {
  QObject::connect(state_->draftTimer, &QTimer::timeout, this,
                   &StrategyTab::handleDraftTimerTimeout);
  QObject::connect(state_->editor, &QPlainTextEdit::textChanged, this,
                   &StrategyTab::markDraftChanged);
  QObject::connect(state_->fastPeriod, qOverload<int>(&QSpinBox::valueChanged),
                   this, &StrategyTab::markDraftChanged);
  QObject::connect(state_->slowPeriod, qOverload<int>(&QSpinBox::valueChanged),
                   this, &StrategyTab::markDraftChanged);
  QObject::connect(state_->quantity, qOverload<int>(&QSpinBox::valueChanged),
                   this, &StrategyTab::markDraftChanged);
  QObject::connect(state_->saveDraft, &QPushButton::clicked, this,
                   &StrategyTab::handleSaveDraft);
  QObject::connect(state_->loadSource, &QPushButton::clicked, this,
                   &StrategyTab::chooseSourceFile);
  QObject::connect(state_->exportSource, &QPushButton::clicked, this,
                   &StrategyTab::chooseExportSourceFile);
  QObject::connect(state_->validate, &QPushButton::clicked, this,
                   &StrategyTab::beginValidation);
  QObject::connect(state_->cancelValidation, &QPushButton::clicked, this,
                   &StrategyTab::requestValidationCancellation);
  QObject::connect(&state_->validationWatcher,
                   &QFutureWatcher<State::ValidationResult>::finished, this,
                   &StrategyTab::finishValidation);
  QObject::connect(state_->useInBacktest, &QPushButton::clicked, this,
                   &StrategyTab::useDraftInBacktest);
}

void StrategyTab::restoreSavedDraft() {
  const auto currentPath = state_->draftDirectory / "python-draft.json";
  const auto previousPath =
      state_->draftDirectory / "python-draft.previous.json";
  auto draft = readDraft(currentPath);
  auto warning = QString{};
  auto preserveUnrecoverableDrafts = false;
  if (!draft.has_value()) {
    const auto currentExists = std::filesystem::exists(currentPath);
    draft = readDraft(previousPath);
    if (draft.has_value()) {
      warning = tr("The latest draft was invalid; recovered the previous "
                   "valid generation.");
    } else if (currentExists || std::filesystem::exists(previousPath)) {
      preserveUnrecoverableDrafts = true;
      warning = tr("Saved drafts could not be validated. The source files "
                   "were preserved for diagnosis.");
    }
  }
  state_->loadingDraft = true;
  state_->preserveUnrecoverableDrafts = preserveUnrecoverableDrafts;
  if (draft.has_value()) {
    state_->editor->setPlainText(draft->source);
    state_->fastPeriod->setValue(draft->fastPeriod);
    state_->slowPeriod->setValue(draft->slowPeriod);
    state_->quantity->setValue(draft->quantity);
    state_->savedFingerprint = fingerprint(*draft);
  } else if (preserveUnrecoverableDrafts) {
    state_->editor->clear();
    state_->savedFingerprint = fingerprint(currentDraft(*state_));
  } else {
    state_->editor->setPlainText(QString::fromUtf8(sampleSource));
    state_->savedFingerprint = {};
  }
  state_->loadingDraft = false;
  state_->draftGeneration = 0;
  state_->status->setText(warning.isEmpty() ? tr("Ready") : warning);
}

void StrategyTab::markDraftChanged() {
  if (state_->loadingDraft) {
    return;
  }
  ++state_->draftGeneration;
  state_->diagnostics->clear();
  state_->status->setText(tr("Draft has unsaved changes"));
  state_->draftTimer->start();
}

void StrategyTab::handleDraftTimerTimeout() {
  if (!autosaveDraft()) {
    state_->status->setText(tr("Cannot save draft to the local data folder"));
  }
}

void StrategyTab::handleSaveDraft() {
  if (!saveDraft()) {
    state_->status->setText(tr("Cannot save the Python Strategy draft"));
  }
}

void StrategyTab::chooseSourceFile() {
  const auto path = QFileDialog::getOpenFileName(this, tr("Load Python source"),
                                                 {}, tr("Python files (*.py)"));
  if (!path.isEmpty()) {
    static_cast<void>(loadSourceFile(path.toStdString()));
  }
}

void StrategyTab::chooseExportSourceFile() {
  const auto path =
      QFileDialog::getSaveFileName(this, tr("Export Python source"),
                                   "strategy.py", tr("Python files (*.py)"));
  if (!path.isEmpty()) {
    static_cast<void>(exportSourceFile(path.toStdString()));
  }
}

void StrategyTab::beginValidation() {
  if (!state_->validationRunner || state_->validationWatcher.isRunning()) {
    state_->status->setText(tr("Managed Python validation is unavailable"));
    return;
  }
  if (state_->fastPeriod->value() >= state_->slowPeriod->value()) {
    state_->diagnostics->setPlainText(
        tr("InvalidStrategyParameters: fast_period must be less than "
           "slow_period."));
    state_->status->setText(
        tr("Parameters are invalid; no worker was started"));
    return;
  }
  auto source = normalizedSource(state_->editor->toPlainText());
  std::string encodedSource;
  if (!validSource(source, encodedSource)) {
    state_->diagnostics->setPlainText(
        tr("InvalidStrategyEncoding or StrategySourceTooLarge: source must "
           "be valid UTF-8, non-empty, and at most 1 MiB."));
    state_->status->setText(tr("Source is invalid; no worker was started"));
    return;
  }
  const auto parameters = canonicalParametersJson();
  if (parameters.empty()) {
    state_->status->setText(tr("Cannot encode Strategy parameters"));
    return;
  }
  const auto sourceHash = core::sha256(encodedSource);
  const auto consented = state_->consentProvider
                             ? state_->consentProvider(sourceHash)
                             : requestSourceConsent(sourceHash);
  if (!consented) {
    state_->status->setText(tr("Consent was not granted"));
    return;
  }

  state_->validationGeneration = state_->draftGeneration;
  const auto configuration = bindings::PythonStrategyConfiguration{
      .sourceUtf8 = std::move(encodedSource),
      .canonicalParametersJson = parameters,
      .sourceConsentGranted = true,
  };
  state_->validationCancellation = core::CancellationSource{};
  const auto cancellation = state_->validationCancellation.token();
  const auto runner = state_->validationRunner;
  state_->validate->setEnabled(false);
  state_->cancelValidation->setEnabled(true);
  state_->status->setText(tr("Validating in a fresh managed worker…"));
  state_->diagnostics->clear();

  state_->validationWatcher.setFuture(QtConcurrent::run(
      runValidationSafely, runner, configuration, cancellation));
}

void StrategyTab::requestValidationCancellation() {
  state_->validationCancellation.requestCancellation();
  state_->cancelValidation->setEnabled(false);
  state_->status->setText(tr("Cancel requested…"));
}

void StrategyTab::finishValidation() {
  state_->validate->setEnabled(true);
  state_->cancelValidation->setEnabled(false);
  if (state_->validationGeneration != state_->draftGeneration) {
    state_->status->setText(tr("Draft changed during validation; the "
                               "earlier result was ignored."));
    return;
  }
  const auto result = state_->validationWatcher.result();
  if (!result.ok()) {
    state_->diagnostics->setPlainText(validationErrorText(result.error()));
    state_->status->setText(tr("Validation failed"));
    return;
  }
  state_->status->setText(tr("Strategy is valid for API 1 and template 1.0.0"));
  state_->diagnostics->setPlainText(tr("No Result was created."));
}

void StrategyTab::useDraftInBacktest() {
  if (state_->fastPeriod->value() >= state_->slowPeriod->value()) {
    state_->status->setText(tr("InvalidStrategyParameters: fast_period must "
                               "be less than slow_period"));
    return;
  }
  auto source = normalizedSource(state_->editor->toPlainText());
  std::string encodedSource;
  if (!validSource(source, encodedSource)) {
    state_->status->setText(
        tr("Source must be valid UTF-8, non-empty, and at most 1 MiB"));
    return;
  }
  emit useInBacktest(QString::fromStdString(encodedSource),
                     QString::fromStdString(canonicalParametersJson()));
}

StrategyTab::~StrategyTab() {
  try {
    if (fingerprint(currentDraft(*state_)) != state_->savedFingerprint) {
      static_cast<void>(autosaveDraft());
    }
  } catch (const std::exception &error) {
    qWarning() << "Failed to autosave Python Strategy draft during shutdown:"
               << error.what();
  } catch (...) {
    qWarning() << "Failed to autosave Python Strategy draft during shutdown";
  } // GCOVR_EXCL_LINE: the compiler emits no executable instruction here.
}

bool StrategyTab::autosaveDraft() {
  state_->draftTimer->stop();
  if (state_->preserveUnrecoverableDrafts) {
    return true;
  }
  return saveDraft();
}

bool StrategyTab::saveDraft() {
  state_->draftTimer->stop();
  auto draft = currentDraft(*state_);
  std::string encoded;
  auto source = draft.source;
  if (!validSource(source, encoded) || draft.fastPeriod >= draft.slowPeriod) {
    state_->status->setText(tr("Draft was not saved because its source or "
                               "parameters are invalid"));
    return false;
  }
  const auto currentPath = state_->draftDirectory / "python-draft.json";
  const auto previousPath =
      state_->draftDirectory / "python-draft.previous.json";
  if (const auto previous = readDraft(currentPath); previous.has_value()) {
    if (!writeAtomically(previousPath, draftBytes(*previous))) {
      state_->status->setText(tr("Cannot preserve the previous valid draft"));
      return false;
    }
  }
  if (!writeAtomically(currentPath, draftBytes(draft))) {
    state_->status->setText(tr("Cannot atomically write the Strategy draft"));
    return false;
  }
  state_->savedFingerprint = fingerprint(draft);
  state_->preserveUnrecoverableDrafts = false;
  state_->status->setText(tr("Draft saved"));
  return true;
}

bool StrategyTab::loadSourceFile(const std::filesystem::path &path) {
  if (path.extension() != ".py") {
    state_->status->setText(tr("Only .py files can be loaded"));
    return false;
  }
  QFile file{QString::fromStdString(path.string())};
  if (!file.open(QIODevice::ReadOnly) ||
      static_cast<std::size_t>(file.size()) > maximumStrategySourceBytes) {
    state_->status->setText(
        tr("Python source file is unavailable or exceeds 1 MiB"));
    return false;
  }
  const auto bytes = file.readAll();
  QStringDecoder decoder{QStringDecoder::Utf8};
  QString source = decoder.decode(bytes);
  if (decoder.hasError()) {
    state_->status->setText(tr("InvalidStrategyEncoding: file is not UTF-8"));
    return false;
  }
  source = normalizedSource(std::move(source));
  if (source.contains(QChar::Null) ||
      static_cast<std::size_t>(source.toUtf8().size()) >
          maximumStrategySourceBytes) {
    state_->status->setText(
        tr("StrategySourceContainsNul or StrategySourceTooLarge"));
    return false;
  }
  if (!confirmBufferReplacement(*state_)) {
    state_->status->setText(tr("Load canceled; current draft is unchanged"));
    return false;
  }
  state_->editor->setPlainText(source);
  state_->diagnostics->clear();
  state_->status->setText(tr("Loaded Python source; draft will be saved"));
  state_->draftTimer->start();
  return true;
}

bool StrategyTab::exportSourceFile(const std::filesystem::path &path) {
  if (path.extension() != ".py") {
    state_->status->setText(tr("Export path must end in .py"));
    return false;
  }
  auto source = normalizedSource(state_->editor->toPlainText());
  std::string encoded;
  if (!validSource(source, encoded)) {
    state_->status->setText(tr("Source must be valid UTF-8 and at most 1 MiB"));
    return false;
  }
  QSaveFile file{QString::fromStdString(path.string())};
  const auto bytes = QByteArray::fromStdString(encoded);
  if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size()) {
    file.cancelWriting();
    state_->status->setText(tr("Cannot write Python source file"));
    return false;
  }
  if (!file.commit()) {
    state_->status->setText(tr("Cannot atomically export Python source file"));
    return false;
  }
  state_->status->setText(tr("Source exported as UTF-8 with LF line endings"));
  return true;
}

std::string StrategyTab::sourceUtf8() const {
  return normalizedSource(state_->editor->toPlainText()).toUtf8().toStdString();
} // GCOVR_EXCL_LINE: the compiler emits no executable instruction here.

std::string StrategyTab::canonicalParametersJson() const {
  const nlohmann::json parameters{
      {"fast_period", state_->fastPeriod->value()},
      {"quantity", state_->quantity->value()},
      {"slow_period", state_->slowPeriod->value()},
  };
  const auto canonical = core::serializeCanonicalJson(parameters);
  return canonical.ok() ? canonical.value() : std::string{};
}

} // namespace bte::frontend
