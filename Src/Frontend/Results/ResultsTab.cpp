#include "Bte/Frontend/ResultsTab.h"

// IWYU pragma: no_include "Bte/Results/ResultStore.h"

#include "Bte/Bindings/ResultLibraryRequests.h"
#include "Bte/Core/Time.h"

#include <QAbstractItemView>
#include <QComboBox>
#include <QDateTime>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QList>
#include <QMessageBox>
#include <QPushButton>
#include <QStandardPaths>
#include <QTableWidget>
#include <QTimeZone>
#include <QVariant>
#include <QtCore/Qt>
#include <QtCore/qbytearray.h>
#include <QtCore/qtenvironmentvariables.h>
#include <QtWidgets/qlineedit.h>

#include <algorithm> // IWYU pragma: keep
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace bte::frontend {
namespace {

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): distinct path roles
std::filesystem::path applicationStore(const char *environmentName,
                                       const char *leaf) {
  const auto configured = qEnvironmentVariable(environmentName);
  if (!configured.isEmpty()) {
    return std::filesystem::path{configured.toStdString()};
  }
  return std::filesystem::path{QStandardPaths::writableLocation(
                                   QStandardPaths::AppLocalDataLocation)
                                   .toStdString()} /
         leaf;
}

QString statusText(const results::RunStatus status) {
  switch (status) {
  case results::RunStatus::completed:
    return "Completed";
  case results::RunStatus::failed:
    return "Failed";
  case results::RunStatus::canceled:
    return "Canceled";
  case results::RunStatus::interrupted:
    return "Interrupted";
  case results::RunStatus::running:
    return "Running";
  case results::RunStatus::incomplete:
    return "Incomplete";
  }
  return "Unknown";
}

QString moneyText(const std::optional<std::int64_t> microdollars) {
  return microdollars.has_value()
             ? QString{"$%1"}.arg(
                   static_cast<double>(*microdollars) / 1'000'000.0, 0, 'f', 2)
             : QString{"Unavailable"};
}

} // namespace

struct ResultsTab::State final {
  std::unique_ptr<bindings::ResultLibraryRequests> catalogRequests;
  std::unique_ptr<bindings::ResultLibraryRequests> detailsRequests;
  std::unique_ptr<bindings::ResultLibraryRequests> actionRequests;
  QComboBox *location{};
  QTableWidget *table{};
  QLabel *message{};
  QLabel *details{};
  QPushButton *refreshButton{};
  QPushButton *viewButton{};
  QPushButton *openButton{};
  QPushButton *renameButton{};
  QPushButton *trashButton{};
  QPushButton *restoreButton{};
  std::vector<results::ResultSummary> summaries;
  bool busy = false;

  [[nodiscard]] bool showingTrash() const {
    return location->currentIndex() == 1;
  }

  [[nodiscard]] QString selectedId() const {
    const auto row = table->currentRow();
    if (row < 0 || row >= table->rowCount()) {
      return {};
    }
    const auto *item = table->item(row, 0);
    return item == nullptr ? QString{} : item->data(Qt::UserRole).toString();
  }

  [[nodiscard]] const results::ResultSummary *selectedSummary() const {
    const auto resultId = selectedId().toStdString();
    const auto entry =
        std::ranges::find_if(summaries, [&resultId](const auto &summary) {
          return summary.resultId == resultId;
        });
    return entry == summaries.end() ? nullptr : &*entry;
  }

  void updateActions() const {
    const auto *selected = selectedSummary();
    const auto canUse = !busy && selected != nullptr;
    viewButton->setEnabled(canUse && !showingTrash() && selected->available);
    openButton->setEnabled(canUse && !showingTrash() && selected->available);
    renameButton->setEnabled(canUse && !showingTrash() && selected->available);
    trashButton->setEnabled(canUse && !showingTrash());
    restoreButton->setEnabled(canUse && showingTrash() && selected->available);
  }
};

ResultsTab::ResultsTab(QWidget *parent)
    : ResultsTab(applicationStore("BTE_RESULT_STORE", "Results"),
                 applicationStore("BTE_DATA_STORE", "Data"), parent) {}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): Qt event wiring
ResultsTab::ResultsTab(std::filesystem::path resultStore,
                       std::filesystem::path dataStore, QWidget *parent)
    : QWidget(parent), state_(std::make_unique<State>()) {
  state_->catalogRequests =
      std::make_unique<bindings::ResultLibraryRequests>(resultStore, dataStore);
  state_->detailsRequests =
      std::make_unique<bindings::ResultLibraryRequests>(resultStore, dataStore);
  state_->actionRequests = std::make_unique<bindings::ResultLibraryRequests>(
      std::move(resultStore), std::move(dataStore));
  setObjectName("resultsTab");
  setAccessibleName("Saved Backtest results");
  setStyleSheet(R"(
    QWidget { background: #07111b; color: #dce9f5; }
    QPushButton, QComboBox { background: #0b2232; border: 1px solid #2b526b;
                           border-radius: 4px; padding: 8px; }
    QPushButton:enabled:hover { background: #123448; }
    QPushButton:disabled { color: #6f879b; }
    QTableWidget { background: #091d2b; gridline-color: #18384b; }
  )");

  auto *layout = std::make_unique<QVBoxLayout>(this).release();
  auto *toolbar = std::make_unique<QHBoxLayout>().release();
  state_->location = std::make_unique<QComboBox>(this).release();
  state_->location->setObjectName("resultsLocationCombo");
  state_->location->setAccessibleName("Result location");
  state_->location->addItems({tr("Saved Results"), tr("Trash")});
  state_->refreshButton =
      std::make_unique<QPushButton>(tr("Refresh"), this).release();
  state_->refreshButton->setObjectName("resultsRefreshButton");
  state_->viewButton =
      std::make_unique<QPushButton>(tr("View Details"), this).release();
  state_->viewButton->setObjectName("resultsViewDetailsButton");
  state_->openButton =
      std::make_unique<QPushButton>(tr("Open Replay"), this).release();
  state_->openButton->setObjectName("resultsOpenReplayButton");
  state_->renameButton =
      std::make_unique<QPushButton>(tr("Rename"), this).release();
  state_->renameButton->setObjectName("resultsRenameButton");
  state_->trashButton =
      std::make_unique<QPushButton>(tr("Move to Trash"), this).release();
  state_->trashButton->setObjectName("resultsMoveToTrashButton");
  state_->restoreButton =
      std::make_unique<QPushButton>(tr("Restore"), this).release();
  state_->restoreButton->setObjectName("resultsRestoreButton");
  toolbar->addWidget(state_->location);
  toolbar->addWidget(state_->refreshButton);
  toolbar->addStretch();
  toolbar->addWidget(state_->viewButton);
  toolbar->addWidget(state_->openButton);
  toolbar->addWidget(state_->renameButton);
  toolbar->addWidget(state_->trashButton);
  toolbar->addWidget(state_->restoreButton);
  layout->addLayout(toolbar);

  state_->message =
      std::make_unique<QLabel>(tr("Loading saved results…"), this).release();
  state_->message->setObjectName("resultsStatusLabel");
  layout->addWidget(state_->message);
  state_->table = std::make_unique<QTableWidget>(0, 5, this).release();
  state_->table->setObjectName("resultsTable");
  state_->table->setHorizontalHeaderLabels({tr("Name"), tr("Saved (UTC)"),
                                            tr("Result ID"), tr("Status"),
                                            tr("Availability")});
  state_->table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
  state_->table->setSelectionBehavior(QAbstractItemView::SelectRows);
  state_->table->setSelectionMode(QAbstractItemView::SingleSelection);
  state_->table->setEditTriggers(QAbstractItemView::NoEditTriggers);
  layout->addWidget(state_->table, 3);
  state_->details =
      std::make_unique<QLabel>(tr("Select a result to view its details."), this)
          .release();
  state_->details->setObjectName("resultsDetailsLabel");
  state_->details->setWordWrap(true);
  state_->details->setTextInteractionFlags(Qt::TextSelectableByMouse);
  layout->addWidget(state_->details, 1);

  QObject::connect(state_->location, &QComboBox::currentIndexChanged, this,
                   [this]() { refresh(); });
  QObject::connect(state_->refreshButton, &QPushButton::clicked, this,
                   &ResultsTab::refresh);
  QObject::connect(state_->table, &QTableWidget::itemSelectionChanged, this,
                   [this]() {
                     state_->detailsRequests->cancel();
                     state_->details->setText(
                         tr("Select View Details to inspect this result."));
                     state_->updateActions();
                   });
  QObject::connect(state_->viewButton, &QPushButton::clicked, this, [this]() {
    const auto resultId = state_->selectedId();
    if (resultId.isEmpty()) {
      return;
    }
    state_->details->setText(tr("Loading result details…"));
    state_->detailsRequests->requestDetails(
        resultId.toStdString(), [this, resultId](auto result) {
          if (!result.ok()) {
            state_->details->setText(
                tr("Cannot view result — %1")
                    .arg(QString::fromStdString(result.error().message)));
            return;
          }
          const auto &opened = result.value();
          // ResultStore validates that a readable Result has a nonempty
          // universe.
          const auto symbols =
              QString::fromStdString(opened.descriptor.universe.front());
          state_->details->setText(
              tr("Name: %1\nResult ID: %2\nSymbol: %3\nRange (UTC): %4 to %5 "
                 "(end exclusive)\nStatus: %6\nFinal equity: %7\nPnL: %8\n"
                 "Canonical hash: %9\nData: Available\nTrades/advanced "
                 "metrics: Unavailable")
                  .arg(opened.displayName.empty()
                           ? resultId
                           : QString::fromStdString(opened.displayName),
                       resultId, symbols,
                       QString::fromStdString(core::time::toIso8601(
                           opened.descriptor.range.start)),
                       QString::fromStdString(
                           core::time::toIso8601(opened.descriptor.range.end)),
                       statusText(opened.status),
                       moneyText(opened.summary.finalEquityMicrodollars),
                       moneyText(opened.summary.pnlMicrodollars),
                       QString::fromStdString(opened.canonicalResultHash)));
        });
  });
  QObject::connect(state_->openButton, &QPushButton::clicked, this, [this]() {
    if (state_->openButton->isEnabled()) {
      emit openResultInReplay(state_->selectedId());
    }
  });
  QObject::connect(state_->renameButton, &QPushButton::clicked, this, [this]() {
    const auto resultId = state_->selectedId();
    const auto *selected = state_->selectedSummary();
    if (selected == nullptr || !state_->renameButton->isEnabled()) {
      return;
    }
    bool accepted = false;
    const auto current = QString::fromStdString(selected->displayName);
    const auto name =
        QInputDialog::getText(this, tr("Rename Result"), tr("Result name"),
                              QLineEdit::Normal, current, &accepted,
                              Qt::WindowFlags{}, Qt::ImhNone)
            .trimmed();
    if (!accepted) {
      return;
    }
    if (name.isEmpty() || name.toUtf8().size() > 160) {
      state_->message->setText(tr("Cannot rename — enter a name of at most "
                                  "160 UTF-8 bytes."));
      return;
    }
    state_->busy = true;
    state_->updateActions();
    state_->actionRequests->requestRename(
        resultId.toStdString(), name.toStdString(), [this](auto result) {
          state_->busy = false;
          if (!result.ok()) {
            state_->message->setText(
                tr("Cannot rename result — %1")
                    .arg(QString::fromStdString(result.error().message)));
            state_->updateActions();
            return;
          }
          refresh();
        });
  });
  QObject::connect(state_->trashButton, &QPushButton::clicked, this, [this]() {
    const auto resultId = state_->selectedId();
    if (resultId.isEmpty() || state_->busy || state_->showingTrash()) {
      return;
    }
    if (QMessageBox::question(this, tr("Move Result to Trash"),
                              tr("Move result %1 to Trash? You can restore it "
                                 "later.")
                                  .arg(resultId),
                              QMessageBox::Yes | QMessageBox::Cancel,
                              QMessageBox::Cancel) != QMessageBox::Yes) {
      return;
    }
    state_->busy = true;
    state_->updateActions();
    state_->actionRequests->requestMoveToTrash(
        resultId.toStdString(), [this, resultId](auto result) {
          state_->busy = false;
          if (!result.ok()) {
            state_->message->setText(
                tr("Cannot move result to Trash — %1")
                    .arg(QString::fromStdString(result.error().message)));
            state_->updateActions();
            return;
          }
          emit resultMovedToTrash(resultId);
          refresh();
        });
  });
  QObject::connect(
      state_->restoreButton, &QPushButton::clicked, this, [this]() {
        const auto resultId = state_->selectedId();
        if (resultId.isEmpty() || state_->busy || !state_->showingTrash()) {
          return;
        }
        state_->busy = true;
        state_->updateActions();
        state_->actionRequests->requestRestore(
            resultId.toStdString(), [this](auto result) {
              state_->busy = false;
              if (!result.ok()) {
                state_->message->setText(
                    tr("Cannot restore result — %1")
                        .arg(QString::fromStdString(result.error().message)));
                state_->updateActions();
                return;
              }
              refresh();
            });
      });
  state_->updateActions();
  refresh();
}

ResultsTab::~ResultsTab() = default;

void ResultsTab::refresh() {
  if (state_->busy) {
    return;
  }
  state_->detailsRequests->cancel();
  state_->summaries.clear();
  state_->table->setRowCount(0);
  state_->details->setText(tr("Select a result to view its details."));
  state_->message->setText(tr("Loading results…"));
  state_->updateActions();
  state_->catalogRequests->requestCatalog(
      state_->showingTrash(), [this](auto result) {
        if (!result.ok()) {
          state_->message->setText(
              tr("Cannot load results — %1")
                  .arg(QString::fromStdString(result.error().message)));
          return;
        }
        state_->summaries = std::move(result).value();
        state_->table->setRowCount(static_cast<int>(state_->summaries.size()));
        for (auto row = 0; row < state_->table->rowCount(); ++row) {
          const auto &entry =
              state_->summaries.at(static_cast<std::size_t>(row));
          auto *name = std::make_unique<QTableWidgetItem>(
                           entry.displayName.empty()
                               ? QString::fromStdString(entry.resultId)
                               : QString::fromStdString(entry.displayName))
                           .release();
          name->setData(Qt::UserRole, QString::fromStdString(entry.resultId));
          state_->table->setItem(row, 0, name);
          auto *saved = std::make_unique<QTableWidgetItem>(
                            QDateTime::fromMSecsSinceEpoch(entry.savedUtcMillis,
                                                           QTimeZone::UTC)
                                .toString(Qt::ISODate))
                            .release();
          state_->table->setItem(row, 1, saved);
          state_->table->setItem(row, 2,
                                 std::make_unique<QTableWidgetItem>(
                                     QString::fromStdString(entry.resultId))
                                     .release());
          state_->table->setItem(
              row, 3,
              std::make_unique<QTableWidgetItem>(statusText(entry.status))
                  .release());
          state_->table->setItem(row, 4,
                                 std::make_unique<QTableWidgetItem>(
                                     entry.available
                                         ? tr("Available")
                                         : tr("Unavailable — %1")
                                               .arg(QString::fromStdString(
                                                   entry.unavailableReason)))
                                     .release());
        }
        state_->message->setText(
            state_->summaries.empty()
                ? tr("No results in this location.")
                : tr("%1 results").arg(state_->summaries.size()));
        state_->updateActions();
      });
}

} // namespace bte::frontend
