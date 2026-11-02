// SPDX-License-Identifier: GPL-3.0-or-later
#include "BookmarksPane.h"

#include <QFileInfo>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace noteAppC {

namespace {

QString labelFor(const BookmarkItem &item)
{
    if (!item.title.isEmpty())
        return item.title;
    switch (item.type) {
    case BookmarkItem::Type::File: {
        const QString name = QFileInfo(item.path).completeBaseName();
        return item.isHeading() ? QStringLiteral("%1 > %2").arg(name, item.subpath.mid(1)) : name;
    }
    case BookmarkItem::Type::Search:
        return QObject::tr("Search: %1").arg(item.query);
    case BookmarkItem::Type::Group:
        return QObject::tr("Group");
    }
    return {};
}

} // namespace

BookmarksPane::BookmarksPane(QWidget *parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("bookmarksPane"));

    m_header = new QLabel(tr("Bookmarks"), this);
    m_header->setObjectName(QStringLiteral("paneHeader"));

    m_tree = new QTreeWidget(this);
    m_tree->setObjectName(QStringLiteral("bookmarksTree"));
    m_tree->setHeaderHidden(true);
    m_tree->setColumnCount(1);
    m_tree->setFrameShape(QFrame::NoFrame);
    m_tree->setIndentation(12);
    m_tree->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_tree->setContextMenuPolicy(Qt::CustomContextMenu);

    connect(m_tree, &QTreeWidget::itemActivated, this, &BookmarksPane::activate);
    connect(m_tree, &QTreeWidget::itemClicked, this, &BookmarksPane::activate);
    connect(m_tree, &QTreeWidget::customContextMenuRequested, this, &BookmarksPane::showContextMenu);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(10, 10, 6, 6);
    layout->setSpacing(6);
    layout->addWidget(m_header);
    layout->addWidget(m_tree, 1);
}

void BookmarksPane::setVaultRoot(const QString &vaultRoot)
{
    m_vaultRoot = vaultRoot;
    if (vaultRoot.isEmpty()) {
        m_bookmarks.reset();
        rebuildTree();
        return;
    }
    m_bookmarks = std::make_unique<Bookmarks>(vaultRoot);
    m_bookmarks->load();
    rebuildTree();
}

void BookmarksPane::bookmarkNote(const QString &relativePath, const QString &title)
{
    if (!m_bookmarks)
        return;
    m_bookmarks->addNote(relativePath, title);
    saveAndRefresh();
}

void BookmarksPane::bookmarkHeading(const QString &relativePath, const QString &subpath,
                                    const QString &title)
{
    if (!m_bookmarks)
        return;
    m_bookmarks->addHeading(relativePath, subpath, title);
    saveAndRefresh();
}

void BookmarksPane::bookmarkSearch(const QString &query, const QString &title)
{
    if (!m_bookmarks)
        return;
    m_bookmarks->addSearch(query, title);
    saveAndRefresh();
}

void BookmarksPane::refresh()
{
    rebuildTree();
}

void BookmarksPane::saveAndRefresh()
{
    m_bookmarks->save();
    rebuildTree();
}

void BookmarksPane::rebuildTree()
{
    m_tree->clear();
    if (!m_bookmarks) {
        m_header->setText(tr("Bookmarks"));
        return;
    }
    m_header->setText(tr("Bookmarks  %1").arg(m_bookmarks->items().size()));
    populate(m_bookmarks->items(), nullptr);
}

void BookmarksPane::populate(const QList<BookmarkItem> &items, QTreeWidgetItem *parent)
{
    for (const BookmarkItem &item : items) {
        auto *treeItem = parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(m_tree);
        treeItem->setText(0, labelFor(item));
        treeItem->setData(0, Qt::UserRole, item.id);
        treeItem->setExpanded(true);
        if (item.type == BookmarkItem::Type::Group)
            populate(item.items, treeItem);
    }
}

void BookmarksPane::activate(QTreeWidgetItem *treeItem)
{
    if (!treeItem || !m_bookmarks)
        return;
    const QString id = treeItem->data(0, Qt::UserRole).toString();
    const BookmarkItem *item = m_bookmarks->find(id);
    if (!item)
        return;

    switch (item->type) {
    case BookmarkItem::Type::File:
        if (item->isHeading())
            emit headingActivated(item->path, item->subpath);
        else
            emit noteActivated(item->path);
        break;
    case BookmarkItem::Type::Search:
        emit searchActivated(item->query);
        break;
    case BookmarkItem::Type::Group:
        treeItem->setExpanded(!treeItem->isExpanded());
        break;
    }
}

void BookmarksPane::showContextMenu(const QPoint &pos)
{
    if (!m_bookmarks)
        return;

    QTreeWidgetItem *treeItem = m_tree->itemAt(pos);
    const BookmarkItem *item = treeItem
        ? m_bookmarks->find(treeItem->data(0, Qt::UserRole).toString())
        : nullptr;

    QMenu menu(this);
    QAction *newGroupAction = menu.addAction(tr("New group"));
    QAction *renameAction = nullptr;
    QAction *removeAction = nullptr;
    if (item && item->type == BookmarkItem::Type::Group)
        renameAction = menu.addAction(tr("Rename group"));
    if (item)
        removeAction = menu.addAction(tr("Remove"));

    const QString id = treeItem ? treeItem->data(0, Qt::UserRole).toString() : QString();
    const QString currentTitle = item ? item->title : QString();

    QAction *chosen = menu.exec(m_tree->viewport()->mapToGlobal(pos));
    if (!chosen)
        return;
    if (chosen == newGroupAction)
        addGroup();
    else if (chosen == renameAction)
        renameGroupWithId(id, currentTitle);
    else if (chosen == removeAction)
        removeItemWithId(id);
}

void BookmarksPane::addGroup()
{
    bool ok = false;
    const QString title =
        QInputDialog::getText(this, tr("New group"), tr("Group name:"), QLineEdit::Normal, {}, &ok);
    if (!ok || title.isEmpty())
        return;
    m_bookmarks->addGroup(title);
    saveAndRefresh();
}

void BookmarksPane::removeItemWithId(const QString &id)
{
    if (id.isEmpty())
        return;
    m_bookmarks->remove(id);
    saveAndRefresh();
}

void BookmarksPane::renameGroupWithId(const QString &id, const QString &currentTitle)
{
    if (id.isEmpty())
        return;

    bool ok = false;
    const QString title = QInputDialog::getText(this, tr("Rename group"), tr("Group name:"),
                                                QLineEdit::Normal, currentTitle, &ok);
    if (!ok || title.isEmpty() || title == currentTitle)
        return;
    m_bookmarks->renameGroup(id, title);
    saveAndRefresh();
}

} // namespace noteAppC
