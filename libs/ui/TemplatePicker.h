// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej
#pragma once

#include <QDialog>
#include <QStringList>
#include <optional>

#include "Templates.h"

class QLineEdit;
class QListWidget;

namespace noteAppC {

/// A small fuzzy picker over the templates folder, the same shape as
/// QuickSwitcher: type, see the best matches, Enter to insert.
///
/// Renders the chosen template itself - templateChosen() carries the already
/// substituted text, not just a name - so the caller only has to insert it at
/// the caret (MarkdownEditor::insertText).
class TemplatePicker : public QDialog {
    Q_OBJECT

public:
    explicit TemplatePicker(QWidget *parent = nullptr);

    /// Where templates come from and how {{date}}/{{time}} default-format.
    void setTemplates(Templates templates);

    /// Shows centred on the parent with an empty query. `noteTitle` is the
    /// active note's title, which fills a chosen template's {{title}}.
    void popUp(const QString &noteTitle);

signals:
    void templateChosen(const QString &renderedText);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void refill();
    void chooseCurrent();

    QLineEdit *m_query = nullptr;
    QListWidget *m_results = nullptr;
    QStringList m_candidates;
    std::optional<Templates> m_templates;
    QString m_noteTitle;
};

} // namespace noteAppC
