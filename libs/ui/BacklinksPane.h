#pragma once

#include <QWidget>

class QLabel;
class QTreeWidget;

namespace noteAppC {

class Index;

/// Obsidian's "Linked mentions": every note that links here, grouped by note,
/// with the source line as context. Unlinked mentions come in Phase 5 with
/// the search engine.
class BacklinksPane : public QWidget {
    Q_OBJECT

public:
    explicit BacklinksPane(QWidget *parent = nullptr);

    void setIndex(Index *index);
    /// Absolute path of the note to show links to; empty clears the pane.
    void setNote(const QString &path);
    void refresh();

signals:
    void noteActivated(const QString &relativePath);

private:
    Index *m_index = nullptr;
    QString m_path;
    QLabel *m_header = nullptr;
    QTreeWidget *m_list = nullptr;
};

} // namespace noteAppC
