#pragma once

#include "Bte/Bindings/BacktestSessionVm.h"
#include "Bte/Core/Cancellation.h"
#include "Bte/Core/Result.h"

#include <QObject>
#include <QString>
#include <QWidget>

#include <functional>
#include <memory>

namespace bte::frontend {

class BacktestTab final : public QWidget {
  Q_OBJECT

public:
  using BacktestRunner = std::function<core::Result<bindings::BacktestSnapshot>(
      const bindings::BacktestConfiguration &,
      const core::CancellationToken &)>;

  explicit BacktestTab(QWidget *parent = nullptr);
  explicit BacktestTab(BacktestRunner runner, QWidget *parent = nullptr);
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
