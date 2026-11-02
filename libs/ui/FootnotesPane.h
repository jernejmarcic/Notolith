// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QWidget>

#include "MarkdownEditor.h"

class QLabel;
class QListWidget;

namespace noteAppC {

/// Obsidian's Footnotes view: the active note's footnotes, numbered, each
/// with the first line of its text. A click goes to it in the note.
///
/// Fed by MainWindow from MarkdownEditor::footnotes() whenever the active
/// editor says they changed, the way the Outline is fed its headings.
class FootnotesPane : public QWidget {
    Q_OBJECT

public:
    explicit FootnotesPane(QWidget *parent = nullptr);

    /// A no-op when they are what is already shown, as most parses are.
    void setFootnotes(const QList<MarkdownEditor::FootnoteEntry> &footnotes);
    void clear();

signals:
    void jumpRequested(qsizetype sourceOffset);

private:
    QLabel *m_header = nullptr;
    QListWidget *m_list = nullptr;
    QList<MarkdownEditor::FootnoteEntry> m_shown;
};

} // namespace noteAppC
