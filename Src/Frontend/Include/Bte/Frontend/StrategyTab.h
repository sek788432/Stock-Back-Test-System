#pragma once

#include "Bte/Bindings/BacktestSessionVm.h"
#include "Bte/Core/Cancellation.h"
#include "Bte/Core/Result.h"

#include <QWidget>

#include <filesystem>
#include <functional>
#include <memory>
#include <string>

class QString;

namespace bte::frontend {

class StrategyTab final : public QWidget {
  Q_OBJECT

public:
  struct State;

  using ValidationRunner = std::function<core::Result<void>(
      const bindings::PythonStrategyConfiguration &,
      const core::CancellationToken &)>;
  using ConsentProvider = std::function<bool(const std::string &sourceHash)>;

  explicit StrategyTab(QWidget *parent = nullptr);
  StrategyTab(std::filesystem::path draftDirectory,
              ValidationRunner validationRunner,
              ConsentProvider consentProvider = {}, QWidget *parent = nullptr);
  ~StrategyTab() override;
  StrategyTab(const StrategyTab &) = delete;
  StrategyTab &operator=(const StrategyTab &) = delete;
  StrategyTab(StrategyTab &&) = delete;
  StrategyTab &operator=(StrategyTab &&) = delete;

  [[nodiscard]] bool saveDraft();
  [[nodiscard]] bool autosaveDraft();
  [[nodiscard]] bool loadSourceFile(const std::filesystem::path &path);
  [[nodiscard]] bool exportSourceFile(const std::filesystem::path &path);
  [[nodiscard]] std::string sourceUtf8() const;
  [[nodiscard]] std::string canonicalParametersJson() const;

signals:
  void useInBacktest(const QString &sourceUtf8,
                     const QString &canonicalParametersJson);

private:
  void buildUi();
  void connectUi();
  void restoreSavedDraft();
  void markDraftChanged();
  void handleDraftTimerTimeout();
  void handleSaveDraft();
  void chooseSourceFile();
  void chooseExportSourceFile();
  void beginValidation();
  void requestValidationCancellation();
  void finishValidation();
  void useDraftInBacktest();

  std::unique_ptr<State> state_;
};

} // namespace bte::frontend
