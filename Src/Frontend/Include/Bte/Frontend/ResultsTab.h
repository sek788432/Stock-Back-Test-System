#pragma once

#include <QObject>
#include <QString>
#include <QWidget>

#include <filesystem>
#include <memory>

namespace bte::frontend {

class ResultsTab final : public QWidget {
  Q_OBJECT

public:
  explicit ResultsTab(QWidget *parent = nullptr);
  ResultsTab(std::filesystem::path resultStore, std::filesystem::path dataStore,
             QWidget *parent = nullptr);
  ~ResultsTab() override;
  ResultsTab(const ResultsTab &) = delete;
  ResultsTab &operator=(const ResultsTab &) = delete;
  ResultsTab(ResultsTab &&) = delete;
  ResultsTab &operator=(ResultsTab &&) = delete;

  Q_SLOT void refresh();

signals:
  void openResultInReplay(const QString &resultId);
  void resultMovedToTrash(const QString &resultId);

private:
  struct State;
  std::unique_ptr<State> state_;
};

} // namespace bte::frontend
