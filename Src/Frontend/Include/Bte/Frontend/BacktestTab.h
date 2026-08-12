#pragma once

#include "Bte/Bindings/BacktestSessionVm.h"

#include <QWidget>

#include <functional>
#include <memory>

namespace bte::frontend {

class BacktestTab final : public QWidget {
public:
  using BacktestRunner = std::function<core::Result<bindings::BacktestSnapshot>(
      bindings::BacktestConfiguration, core::CancellationToken)>;

  explicit BacktestTab(QWidget *parent = nullptr);
  explicit BacktestTab(BacktestRunner runner, QWidget *parent = nullptr);
  ~BacktestTab() override;

private:
  struct RunState;
  std::unique_ptr<RunState> runState_;
};

} // namespace bte::frontend
