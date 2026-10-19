#pragma once

#include "Bte/Bindings/ResultCatalogSnapshot.h"

#include <QLabel>
#include <QString>
#include <QStringList>
#include <QWidget>
#include <QtCore/Qt>

#include <span>

namespace bte::frontend {

class ResultRecoveryNoticeLabel final : public QLabel {
public:
  ResultRecoveryNoticeLabel(const QString &name, QWidget *parent)
      : QLabel(parent) {
    setObjectName(name);
    setAccessibleName(tr("Preserved result recovery notices"));
    setTextFormat(Qt::PlainText);
    setTextInteractionFlags(Qt::TextSelectableByMouse);
    setWordWrap(true);
    hide();
  }

  void present(const std::span<const results::ResultRecoveryNotice> notices) {
    QStringList lines;
    for (const auto &notice : notices) {
      lines.push_back(
          tr("%1: %2").arg(QString::fromStdString(notice.resultId),
                           QString::fromStdString(notice.diagnostic)));
    }
    const auto message =
        lines.isEmpty()
            ? QString{}
            : tr("Recovery notices — artifacts were preserved:\n%1")
                  .arg(lines.join('\n'));
    setText(message);
    setAccessibleName(message);
    setVisible(!lines.isEmpty());
  }
};

} // namespace bte::frontend
