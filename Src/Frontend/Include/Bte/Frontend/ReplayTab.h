#pragma once

#include <QString>
#include <QWidget>

#include <filesystem>
#include <memory>

namespace bte::frontend {

class ReplayTab final : public QWidget {
  Q_OBJECT

public:
  explicit ReplayTab(QWidget *parent = nullptr);
  ReplayTab(std::filesystem::path resultStore, std::filesystem::path dataStore,
            QWidget *parent = nullptr);
  ~ReplayTab() override;
  ReplayTab(const ReplayTab &) = delete;
  ReplayTab &operator=(const ReplayTab &) = delete;

public slots:
  void openResult(const QString &resultId);

private:
  struct State;
  std::unique_ptr<State> state_;
};

} // namespace bte::frontend
