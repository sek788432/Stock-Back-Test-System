#pragma once

#include "Bte/Bindings/BacktestSessionVm.h"
#include "Bte/Bindings/ResultCatalogSnapshot.h"
#include "Bte/Core/Cancellation.h"
#include "Bte/Core/Result.h"
#include "Bte/Results/ReproducibilityStore.h"

#include <QObject>
#include <QString>
#include <QWidget>

#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace bte::frontend {

class BacktestTab final : public QWidget {
  Q_OBJECT

public:
  using BacktestRunner = std::function<core::Result<bindings::BacktestSnapshot>(
      const bindings::BacktestConfiguration &,
      const core::CancellationToken &)>;
  using ResultPublisher = std::function<core::Result<std::string>(
      const std::string &, const std::string &,
      const core::CancellationToken &)>;
  using DraftLister =
      std::function<core::Result<bindings::ResultCatalogSnapshot>(
          const core::CancellationToken &)>;
  using DraftDiscarder = std::function<core::Result<void>(
      const std::string &, const core::CancellationToken &)>;
  using VerificationRunner =
      std::function<core::Result<results::VerificationEvidence>(
          const bindings::BacktestConfiguration &,
          const core::CancellationToken &,
          const bindings::PythonVerificationProgressObserver &)>;
  using ConsentProvider = std::function<bool(const std::string &sourceHash)>;

  explicit BacktestTab(QWidget *parent = nullptr);
  explicit BacktestTab(BacktestRunner runner, QWidget *parent = nullptr);
  BacktestTab(BacktestRunner runner, ResultPublisher publisher,
              QWidget *parent = nullptr);
  BacktestTab(BacktestRunner runner, ResultPublisher publisher,
              DraftLister draftLister, DraftDiscarder draftDiscarder,
              QWidget *parent = nullptr);
  [[nodiscard]] static std::unique_ptr<BacktestTab>
  createApplicationConfigured(QWidget *parent = nullptr);
  void setPythonStrategyDraft(QString sourceUtf8,
                              QString canonicalParametersJson);
  void setVerificationRunner(VerificationRunner runner);
  void setConsentProvider(ConsentProvider provider);
  ~BacktestTab() override;
  BacktestTab(const BacktestTab &) = delete;
  BacktestTab &operator=(const BacktestTab &) = delete;
  BacktestTab(BacktestTab &&) = delete;
  BacktestTab &operator=(BacktestTab &&) = delete;

signals:
  void openResultInReplay(const QString &resultId);
  void verificationProgressUpdated(QString statusText);

private:
  void connectConditionControls();
  void connectOperationControls(BacktestRunner runner,
                                const std::function<void()> &beginDraftRefresh);
  void handleStrategySelectionChanged(int index);
  void handleBacktestFinished(const std::function<void()> &beginDraftRefresh);
  void handleVerificationRequested();
  void showVerificationProgress(const QString &statusText);
  void
  handleVerificationFinished(const std::function<void()> &beginDraftRefresh);
  void handleCancelRequested();
  void handleBacktestRequested(const BacktestRunner &runner);
  [[nodiscard]] std::optional<bindings::PythonStrategyConfiguration>
  currentPythonStrategyDraft();
  [[nodiscard]] bindings::BacktestConfiguration
  backtestConfigurationFromWidgets() const;
  void addSelectableConditions(bindings::BacktestConfiguration &configuration);
  void resetBacktestView();
  void startBacktest(bindings::BacktestConfiguration configuration,
                     const BacktestRunner &runner);

  struct RunState;
  std::unique_ptr<RunState> runState_;
};

} // namespace bte::frontend
