// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QList>
#include <QString>

namespace noteAppC {

/// One entry in Obsidian's bookmarks.json shape. A heading or block bookmark
/// is a File item with `subpath` set ("#Heading" or "#^blockid") - Obsidian
/// has no separate type for those, and neither does this.
struct BookmarkItem {
    enum class Type { File, Search, Group };
    Type type = Type::File;

    QString path;               ///< File: vault-relative note path
    QString subpath;            ///< File: "#Heading" / "#^blockid", empty for a plain note
    QString query;               ///< Search: the search string
    QString title;               ///< optional display title; a Group's name
    QList<BookmarkItem> items;   ///< Group's children

    /// Stable across a save/load of noteAppC's own copy; not part of
    /// Obsidian's shape, so it is freshly generated for anything imported
    /// from the vault's own bookmarks.json.
    QString id;

    bool isHeading() const { return type == Type::File && !subpath.isEmpty(); }
};

/// Obsidian's core Bookmarks plugin: notes, headings, searches and groups.
///
/// When noteAppC's own copy for this vault has never been saved, load() seeds
/// it once from the vault's own `.obsidian/bookmarks.json`, if there is one.
/// Every change after that lives only in Notolith's own file under
/// `~/.local/share/notolith/vaults/<digest>/`; the vault's copy is never written to, so opening a
/// vault that already has Obsidian's bookmarks never creates a dotfile change
/// and never needs Obsidian's format to be writable, only readable.
///
/// Limit: a bookmarked folder, graph or web link (Obsidian has all three)
/// is dropped on import rather than round-tripped, since nothing here can act
/// on them yet. Add a pass-through type if that turns out to matter.
class Bookmarks {
public:
    explicit Bookmarks(const QString &vaultRoot);

    void load();
    bool save(QString *error = nullptr) const;

    const QList<BookmarkItem> &items() const { return m_items; }
    /// The item anywhere in the tree with this id (including nested in a
    /// group), or nullptr.
    const BookmarkItem *find(const QString &id) const;

    /// Each adds at the top level, or inside the first group titled
    /// `groupTitle` when one exists (top level otherwise). Returns the new
    /// item's id.
    QString addNote(const QString &path, const QString &title = {}, const QString &groupTitle = {});
    QString addHeading(const QString &path, const QString &subpath, const QString &title = {},
                       const QString &groupTitle = {});
    QString addSearch(const QString &query, const QString &title, const QString &groupTitle = {});
    QString addGroup(const QString &title);

    /// True when `id` was found (anywhere, including nested in a group) and removed.
    bool remove(const QString &id);
    /// True when `id` names an existing group, which is then retitled.
    bool renameGroup(const QString &id, const QString &newTitle);

    /// Notolith's own storage path for this vault - public so the tests can
    /// point isolated fixtures at it without a whole vault on disk.
    static QString storagePathFor(const QString &vaultRoot);

private:
    QList<BookmarkItem> &targetList(const QString &groupTitle);

    QString m_vaultRoot;
    QList<BookmarkItem> m_items;
};

} // namespace noteAppC
