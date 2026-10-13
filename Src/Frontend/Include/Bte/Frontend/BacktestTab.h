#pragma once

#include "Bte/Bindings/BacktestSessionVm.h"
#include "Bte/Core/Cancellation.h"
#include "Bte/Core/Result.h"

#include <QObject>
#include <QString>
#include <QWidget>

#include <functional>
#include <memory>
#include <string>
#include <vector>

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
  using DraftLister = std::function<core::Result<std::vector<std::string>>(
      const core::CancellationToken &)>;
  using DraftDiscarder = std::function<core::Result<void>(
      const std::string &, const core::CancellationToken &)>;

  explicit BacktestTab(QWidget *parent = nullptr);
  explicit BacktestTab(BacktestRunner runner, QWidget *parent = nullptr);
  BacktestTab(BacktestRunner runner, ResultPublisher publisher,
              QWidget *parent = nullptr);
  BacktestTab(BacktestRunner runner, ResultPublisher publisher,
              DraftLister draftLister, DraftDiscarder draftDiscarder,
              QWidget *parent = nullptr);
  [[nodiscard]] static std::unique_ptr<BacktestTab>
  createApplicationConfigured(QWidget *parent = nullptr);
  ~BacktestTab() override;
  BacktestTab(const BacktestTab &) = delete;
  BacktestTab &operator=(const BacktestTab &) = delete;
  BacktestTab(BacktestTab &&) = delete;
  BacktestTab &operator=(BacktestTab &&) = delete;

signals:
  void openResultInReplay(const QString &resultId);

private:
  struct RunState;
  std::unique_ptr<RunState> runState_;
};

} // namespace bte::frontend
