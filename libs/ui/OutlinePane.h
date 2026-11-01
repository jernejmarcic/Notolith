// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej
#pragma once

#include <QHash>
#include <QWidget>

#include "Metadata.h"

class QLabel;
class QTreeWidget;
class QTreeWidgetItem;

namespace noteAppC {

/// Obsidian's "Outline" view: the active note's headings as a collapsible
/// tree, nested by level. Clicking one jumps the editor to its source offset;
/// the heading the caret currently sits under is highlighted, the way
/// Obsidian tracks the outline as you move through the note.
///
/// Takes no editor or index reference - MainWindow feeds it headings() and
/// cursorMoved() straight from the active MarkdownEditor, the same shape
/// BacklinksPane takes an Index.
class OutlinePane : public QWidget {
    Q_OBJECT

public:
    explicit OutlinePane(QWidget *parent = nullptr);

    /// Rebuilds the tree from the active note's headings, in source order.
    /// A no-op, so collapsed branches stay collapsed, when the headings are
    /// exactly what is already shown - which is most parses, since typing
    /// prose between headings reparses without changing a single one of them.
    void setHeadings(const QList<Heading> &headings);
    /// Clears the pane, as when no note is open.
    void clear();

    /// The caret moved to `sourceOffset`: highlights the last heading at or
    /// before it, i.e. the section the caret is in.
    void setCursorOffset(qsizetype sourceOffset);

signals:
    /// A heading was clicked; `sourceOffset` is the start of its line, what
    /// MarkdownEditor::setCursorOffset / scrollToOffset expect.
    void jumpRequested(qsizetype sourceOffset);

private:
    void rebuildTree();
    void updateHighlight();
    /// The heading in `m_headings` that contains `offset`, or -1 before the
    /// first heading / with no headings at all.
    int headingAt(qsizetype offset) const;

    QLabel *m_header = nullptr;
    QTreeWidget *m_tree = nullptr;
    QList<Heading> m_headings;
    /// Built alongside the tree, so updateHighlight() can find an item by
    /// heading index without walking the tree.
    QHash<int, QTreeWidgetItem *> m_itemsByHeading;
    qsizetype m_cursor = 0;
    int m_highlighted = -1;
};

} // namespace noteAppC
