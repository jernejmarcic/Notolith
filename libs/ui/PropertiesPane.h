#pragma once

#include <QColor>
#include <QWidget>

class QLabel;
class QToolButton;
class QTreeWidget;

namespace noteAppC {

class Index;
class Theme;

/// Obsidian's "All properties" view: every property key in the vault with
/// its type's icon and how many notes carry it, by count or by name. A click
/// searches for the notes that have it.
class PropertiesPane : public QWidget {
    Q_OBJECT

public:
    explicit PropertiesPane(QWidget *parent = nullptr);

    void setIndex(Index *index);
    void setTheme(const Theme &theme);
    void refresh();

signals:
    void propertyActivated(const QString &key);

private:
    Index *m_index = nullptr;
    bool m_sortByName = false;
    QColor m_mutedColor;
    QColor m_iconColor;
    QLabel *m_header = nullptr;
    QToolButton *m_sortButton = nullptr;
    QTreeWidget *m_list = nullptr;
};

} // namespace noteAppC
