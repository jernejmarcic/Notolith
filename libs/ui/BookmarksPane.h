// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QWidget>
#include <memory>

#include "Bookmarks.h"

class QLabel;
class QTreeWidget;
class QTreeWidgetItem;

namespace noteAppC {

/// Obsidian's "Bookmarks" view: a tree of groups and items - notes, headings
/// and saved searches - backed by a Bookmarks model. Self-contained: give it
/// a vault root and it loads, saves and rebuilds its own tree.
class BookmarksPane : public QWidget {
    Q_OBJECT

public:
    explicit BookmarksPane(QWidget *parent = nullptr);

    /// Loads noteAppC's bookmarks for this vault (seeding once from the
    /// vault's own `.obsidian/bookmarks.json` if noteAppC has never saved its
    /// own copy). Empty clears the pane.
    void setVaultRoot(const QString &vaultRoot);

    /// What a "Bookmark this note/heading/search" command calls; each saves
    /// immediately. `relativePath` is vault-relative, `subpath` is
    /// "#Heading" or "#^blockid" as a link's own subpath is spelled.
    void bookmarkNote(const QString &relativePath, const QString &title = {});
    void bookmarkHeading(const QString &relativePath, const QString &subpath,
                         const QString &title = {});
    void bookmarkSearch(const QString &query, const QString &title);

    void refresh();

signals:
    void noteActivated(const QString &relativePath);
    void headingActivated(const QString &relativePath, const QString &subpath);
    void searchActivated(const QString &query);

private:
    void rebuildTree();
    void populate(const QList<BookmarkItem> &items, QTreeWidgetItem *parent);
    void activate(QTreeWidgetItem *item);
    void showContextMenu(const QPoint &pos);
    void addGroup();
    void removeItemWithId(const QString &id);
    void renameGroupWithId(const QString &id, const QString &currentTitle);
    void saveAndRefresh();

    QLabel *m_header = nullptr;
    QTreeWidget *m_tree = nullptr;
    std::unique_ptr<Bookmarks> m_bookmarks;
    QString m_vaultRoot;
};

} // namespace noteAppC
