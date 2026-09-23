#pragma once

#include <QColor>
#include <QWidget>

class QLabel;
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

signals:
    void noteActivated(const QString &relativePath);

private:
    void refreshUnlinked();

    Index *m_index = nullptr;
    QString m_path;
    QLabel *m_header = nullptr;
    QTreeWidget *m_list = nullptr;
    QLabel *m_unlinkedHeader = nullptr;
    QTreeWidget *m_unlinkedList = nullptr;
    QColor m_highlightColor;
};

} // namespace noteAppC
