// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QColor>
#include <QWidget>

class QLabel;
class QShowEvent;
class QTreeWidget;

namespace noteAppC {

class Index;
class Theme;

/// Obsidian's "Linked mentions" and "Unlinked mentions": every note that
/// links here, grouped by note with the source line as context, and below it
/// every note that mentions this note's title or an alias as plain text
/// without linking it - built on the same search engine SearchPane uses.
class BacklinksPane : public QWidget {
    Q_OBJECT

public:
    explicit BacklinksPane(QWidget *parent = nullptr);

    void setIndex(Index *index);
    /// Only needed for the colour the unlinked-mentions snippets highlight
    /// their match in; linked mentions have never needed a Theme.
    void setTheme(const Theme &theme);
    /// Absolute path of the note to show links to; empty clears the pane.
    void setNote(const QString &path);
    void refresh();
    /// Whether to look for unlinked mentions at all: a search of the vault
    /// for each note shown, so off skips the search, not only the list.
    void setUnlinkedMentions(bool on);

signals:
    void noteActivated(const QString &relativePath);

protected:
    void showEvent(QShowEvent *event) override;

private:
    void onIndexChanged();
    void refreshUnlinked();

    Index *m_index = nullptr;
    QString m_path;
    QLabel *m_header = nullptr;
    QTreeWidget *m_list = nullptr;
    QLabel *m_unlinkedHeader = nullptr;
    QTreeWidget *m_unlinkedList = nullptr;
    QColor m_highlightColor;
    bool m_unlinked = true;
    bool m_stale = false; ///< Index::changed arrived while hidden
};

} // namespace noteAppC
