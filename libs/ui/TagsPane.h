#pragma once

#include <QColor>
#include <QWidget>

class QLabel;
class QToolButton;
class QTreeWidget;

namespace noteAppC {

class Index;
class Theme;

/// Obsidian's "Tags" view: every tag in the vault, nested on '/' so
/// `#inbox/to-read` sits under `inbox`, each row showing the tag name and its
/// note count right-aligned and muted. Sorted by count then name by default;
/// the header button switches to name order.
class TagsPane : public QWidget {
    Q_OBJECT

public:
    explicit TagsPane(QWidget *parent = nullptr);

    void setIndex(Index *index);
    void setTheme(const Theme &theme);
    void refresh();

signals:
    /// Phase 5's search pane takes this over; for now the caller opens the
    /// first note carrying the tag.
    void tagActivated(const QString &tag);

private:
    void toggleSort();

    Index *m_index = nullptr;
    bool m_sortByName = false;
    QColor m_mutedColor;
    QLabel *m_header = nullptr;
    QToolButton *m_sortButton = nullptr;
    QTreeWidget *m_list = nullptr;
};

} // namespace noteAppC
