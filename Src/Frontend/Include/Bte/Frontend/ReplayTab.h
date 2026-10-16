#pragma once

#include <QObject>
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
  ReplayTab(ReplayTab &&) = delete;
  ReplayTab &operator=(ReplayTab &&) = delete;

  Q_SLOT void openResult(const QString &resultId);

private:
  struct State;
  std::unique_ptr<State> state_;
};

} // namespace bte::frontend
