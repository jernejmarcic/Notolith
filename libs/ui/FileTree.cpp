// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej Marčič
#include "FileTree.h"

#include <QDir>
#include <QDrag>
#include <QDragMoveEvent>
#include <QFileSystemModel>
#include <QHelpEvent>
#include <QLineEdit>
#include <QMimeData>
#include <QPainter>
#include <QSet>
#include <QToolTip>
#include <QUrl>

#include <algorithm>

#include "Icons.h"
#include "Theme.h"

namespace noteAppC {

namespace {

constexpr int kIconSize = 16;
constexpr int kRowPadding = 6; // text starts this far inside the pill
/// The mime format that marks a drag as this tree's own (see dragData()).
constexpr const char *kDragMarker = "application/x-noteappc-file-tree";

/// The bundled ids, read once: a name that is not one of these is a glyph.
bool isBundledIcon(const QString &name)
{
    static const QSet<QString> names = [] {
        QSet<QString> set;
        for (const QString &file : QDir(QStringLiteral(":/icons")).entryList({QStringLiteral("*.svg")}))
            set.insert(file.chopped(4));
        return set;
    }();
    return names.contains(name);
}

} // namespace

QIcon fileIcon(const QString &name, const QColor &colour, int size)
{
    if (name.isEmpty())
        return {};
    if (isBundledIcon(name))
        return Icons::icon(name, colour, size);
    // An emoji or one character, drawn with whatever font has it.
    QPixmap pixmap(QSize(size, size) * 2);
    pixmap.setDevicePixelRatio(2);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    QFont font = painter.font();
    font.setPixelSize(size - 2);
    painter.setFont(font);
    painter.setPen(colour);
    painter.drawText(QRect(0, 0, size, size), Qt::AlignCenter, name);
    return QIcon(pixmap);
}

// --- FileRowDelegate --------------------------------------------------------

QString FileRowDelegate::displayName(const QFileInfo &info)
{
    return info.isDir() ? info.fileName() : info.completeBaseName();
}

QString FileRowDelegate::tagFor(const QFileInfo &info)
{
    if (info.isDir())
        return {};
    const QString suffix = info.suffix();
    return suffix.compare(QLatin1String("md"), Qt::CaseInsensitive) == 0 ? QString() : suffix.toUpper();
}

void FileRowDelegate::setColours(const Theme &theme)
{
    m_text = theme.color(QStringLiteral("--nav-item-color"));
    m_textActive = theme.color(QStringLiteral("--nav-item-color-active"));
    m_hover = theme.color(QStringLiteral("--nav-item-background-hover"));
    m_selected = theme.color(QStringLiteral("--nav-item-background-selected"));
    m_active = theme.color(QStringLiteral("--nav-item-background-active"));
    m_tagText = theme.color(QStringLiteral("--nav-file-tag-color"));
    m_tagBackground = theme.color(QStringLiteral("--nav-file-tag-background"));
    m_radius = theme.value(QStringLiteral("--radius-s")).toInt();
    m_tagPixelSize = theme.value(QStringLiteral("--nav-file-tag-size")).toInt() - 2;
    // Line height 1.5 plus the vertical padding on both sides: 26 px at 13 px.
    m_rowHeight = qRound(theme.value(QStringLiteral("--nav-item-size")).toInt() * 1.5)
                  + 2 * theme.value(QStringLiteral("--nav-item-padding")).toInt();
    m_icons.clear();
}

void FileRowDelegate::setIconHook(std::function<QString(const QString &)> hook)
{
    m_iconHook = std::move(hook);
}

QString FileRowDelegate::tagText(const QModelIndex &index) const
{
    const auto *model = qobject_cast<const QFileSystemModel *>(index.model());
    return model ? tagFor(model->fileInfo(index)) : QString();
}

bool FileRowDelegate::isElided(const QModelIndex &index, int rowWidth, const QFont &font) const
{
    const auto *model = qobject_cast<const QFileSystemModel *>(index.model());
    if (!model)
        return false;
    const QFileInfo info = model->fileInfo(index);
    int room = rowWidth - 2 * kRowPadding;
    if (m_iconHook && !m_iconHook(info.absoluteFilePath()).isEmpty())
        room -= kIconSize + 6;
    if (const QString tag = tagFor(info); !tag.isEmpty()) {
        QFont tagFont = font;
        tagFont.setPixelSize(m_tagPixelSize);
        room -= QFontMetrics(tagFont).horizontalAdvance(tag) + 8 + 6;
    }
    return QFontMetrics(font).horizontalAdvance(displayName(info)) > room;
}

QSize FileRowDelegate::sizeHint(const QStyleOptionViewItem &option, const QModelIndex &) const
{
    // Never depends on the row: the view keeps uniform row heights, and a
    // 20 000 note vault must not ask the model per row.
    return QSize(option.rect.width(), m_rowHeight);
}

void FileRowDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option,
                            const QModelIndex &index) const
{
    const auto *model = qobject_cast<const QFileSystemModel *>(index.model());
    if (!model)
        return;
    const QFileInfo info = model->fileInfo(index);
    const bool selected = option.state & QStyle::State_Selected;
    const bool hovered = option.state & QStyle::State_MouseOver;

    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);
    // The pill: the row rectangle and nothing left of it.
    const QRect pill = option.rect.adjusted(0, 1, 0, -1);
    const QColor *fill = selected ? &m_selected
                         : hovered ? &m_hover
                         : (option.state & QStyle::State_HasFocus) ? &m_active
                                                                   : nullptr;
    if (fill) {
        painter->setPen(Qt::NoPen);
        painter->setBrush(*fill);
        painter->drawRoundedRect(pill, m_radius, m_radius);
    }

    int left = pill.left() + kRowPadding;
    int right = pill.right() + 1 - kRowPadding;

    const QString tag = tagFor(info);
    if (!tag.isEmpty()) {
        QFont tagFont = option.font;
        tagFont.setPixelSize(m_tagPixelSize);
        painter->setFont(tagFont);
        const QFontMetrics metrics(tagFont);
        const QRect box(0, 0, metrics.horizontalAdvance(tag) + 8, metrics.height() + 2);
        const QRect placed = QRect(QPoint(right - box.width() + 1, pill.center().y() - box.height() / 2 + 1),
                                   box.size());
        painter->setBrush(m_tagBackground);
        painter->setPen(Qt::NoPen);
        painter->drawRoundedRect(placed, 3, 3);
        painter->setPen(m_tagText);
        painter->drawText(placed, Qt::AlignCenter, tag);
        right = placed.left() - 6;
    }

    if (m_iconHook) {
        const QString name = m_iconHook(info.absoluteFilePath());
        if (!name.isEmpty()) {
            auto cached = m_icons.find(name);
            if (cached == m_icons.end())
                cached = m_icons.insert(name, fileIcon(name, m_text, kIconSize));
            cached->paint(painter, QRect(left, pill.center().y() - kIconSize / 2, kIconSize, kIconSize));
            left += kIconSize + 6;
        }
    }

    painter->setFont(option.font);
    painter->setPen(selected || hovered ? m_textActive : m_text);
    const QString text = option.fontMetrics.elidedText(displayName(info), Qt::ElideRight, right - left + 1);
    painter->drawText(QRect(left, pill.top(), right - left + 1, pill.height()),
                      Qt::AlignVCenter | Qt::AlignLeft, text);
    painter->restore();
}

QWidget *FileRowDelegate::createEditor(QWidget *parent, const QStyleOptionViewItem &,
                                       const QModelIndex &) const
{
    auto *edit = new QLineEdit(parent);
    edit->setObjectName(QStringLiteral("fileRenameEditor"));
    return edit;
}

void FileRowDelegate::setEditorData(QWidget *editor, const QModelIndex &index) const
{
    const auto *model = qobject_cast<const QFileSystemModel *>(index.model());
    auto *edit = static_cast<QLineEdit *>(editor);
    if (model)
        edit->setText(displayName(model->fileInfo(index)));
    edit->selectAll();
}

void FileRowDelegate::setModelData(QWidget *editor, QAbstractItemModel *,
                                   const QModelIndex &index) const
{
    // The signal is not const; the delegate API is.
    emit const_cast<FileRowDelegate *>(this)->renameRequested(
        QPersistentModelIndex(index), static_cast<QLineEdit *>(editor)->text().trimmed());
}

// --- FileTree ---------------------------------------------------------------

FileTree::FileTree(QWidget *parent) : QTreeView(parent)
{
    setObjectName(QStringLiteral("fileExplorer"));
    m_delegate = new FileRowDelegate(this);
    setItemDelegate(m_delegate);
    // Hover is a state the delegate paints; the view needs the moves for it.
    setMouseTracking(true);
    setAttribute(Qt::WA_Hover);
    setEditTriggers(QAbstractItemView::EditKeyPressed);
    setIndentation(16);
    setUniformRowHeights(true);
    // One row at a time (the tree is single-selection). Dropping is ours to
    // handle: see moveRequested().
    setDragEnabled(true);
    setAcceptDrops(true);
    setDropIndicatorShown(true);
    setDragDropMode(QAbstractItemView::DragDrop);
}

QString FileTree::dropFolderAt(const QPoint &viewportPos) const
{
    const auto *files = qobject_cast<const QFileSystemModel *>(model());
    if (!files)
        return {};
    const QModelIndex row = indexAt(viewportPos);
    QString folder = files->filePath(rootIndex());
    if (row.isValid())
        folder = files->isDir(row) ? files->filePath(row) : files->filePath(row.parent());
    const QModelIndex dragged = currentIndex();
    if (dragged.isValid()) {
        const QString source = files->filePath(dragged);
        if (folder == source || folder.startsWith(source + QLatin1Char('/')))
            return {};
    }
    return folder;
}

void FileTree::startDrag(Qt::DropActions)
{
    // The base class deletes the dragged rows from the model when a drop
    // ends in a move, which for QFileSystemModel deletes the files. A drag
    // here only carries the paths; the window does the moving.
    QMimeData *data = dragData();
    if (!data)
        return;
    auto *drag = new QDrag(this);
    drag->setMimeData(data);
    drag->exec(Qt::MoveAction, Qt::MoveAction);
}

QMimeData *FileTree::dragData() const
{
    const QModelIndexList rows = selectionModel()->selectedRows(0);
    QMimeData *data = rows.isEmpty() ? nullptr : model()->mimeData(rows);
    if (data)
        data->setData(QLatin1String(kDragMarker), QByteArray::number(quintptr(this)));
    return data;
}

// Not QDropEvent::source(): that is the platform's drag in progress, which a
// test cannot start (the offscreen platform's drag ends at once).
bool FileTree::isOwnDrag(const QMimeData *data) const
{
    return data && data->data(QLatin1String(kDragMarker)) == QByteArray::number(quintptr(this));
}

bool FileTree::isFileDrop(const QMimeData *data) const
{
    if (!m_fileDrops || !data || isOwnDrag(data) || !data->hasUrls())
        return false;
    const QList<QUrl> urls = data->urls();
    return std::any_of(urls.cbegin(), urls.cend(),
                       [this](const QUrl &url) { return url.isLocalFile() && m_fileDrops(url.toLocalFile()); });
}

void FileTree::dragEnterEvent(QDragEnterEvent *event)
{
    if (isOwnDrag(event->mimeData()) || isFileDrop(event->mimeData()))
        event->acceptProposedAction();
    else
        event->ignore();
}

void FileTree::dragMoveEvent(QDragMoveEvent *event)
{
    // The base keeps the drop indicator, auto-scroll and hover-expand going;
    // whether this spot takes the drop is decided here.
    QTreeView::dragMoveEvent(event);
    if (isFileDrop(event->mimeData())) {
        event->setDropAction(Qt::CopyAction);
        event->accept();
    } else if (isOwnDrag(event->mimeData()) && !dropFolderAt(event->position().toPoint()).isEmpty()) {
        event->setDropAction(Qt::MoveAction);
        event->accept();
    } else {
        event->ignore();
    }
}

void FileTree::dropEvent(QDropEvent *event)
{
    // Not the base: its InternalMove path would hand the drop to
    // QFileSystemModel::dropMimeData, which moves the files itself.
    if (isFileDrop(event->mimeData())) {
        QStringList files;
        for (const QUrl &url : event->mimeData()->urls()) {
            if (url.isLocalFile() && m_fileDrops(url.toLocalFile()))
                files << url.toLocalFile();
        }
        const auto *model = qobject_cast<const QFileSystemModel *>(this->model());
        const QModelIndex row = indexAt(event->position().toPoint());
        const QString folder = !model ? QString()
                               : row.isValid() ? (model->isDir(row) ? model->filePath(row) : model->filePath(row.parent()))
                                               : model->filePath(rootIndex());
        event->setDropAction(Qt::CopyAction);
        event->accept();
        stopAutoScroll();
        setState(NoState);
        viewport()->update();
        if (!folder.isEmpty())
            emit filesDropped(files, folder);
        return;
    }
    const QString folder = dropFolderAt(event->position().toPoint());
    const QModelIndex dragged = currentIndex();
    const auto *files = qobject_cast<const QFileSystemModel *>(model());
    const bool ours = isOwnDrag(event->mimeData()) && files && dragged.isValid() && !folder.isEmpty();
    if (ours) {
        event->setDropAction(Qt::MoveAction);
        event->accept();
    } else {
        event->ignore();
    }
    stopAutoScroll();
    setState(NoState);
    viewport()->update();
    if (ours)
        emit moveRequested(files->filePath(dragged), folder);
}

void FileTree::setColours(const Theme &theme)
{
    m_delegate->setColours(theme);
    m_guide = theme.color(QStringLiteral("--nav-indentation-guide-color"));
    m_guideWidth = qMax(1, theme.value(QStringLiteral("--nav-indentation-guide-width")).toInt());
    m_open = Icons::icon(QStringLiteral("chevron-down"),
                         theme.color(QStringLiteral("--nav-collapse-icon-color")), 12);
    m_closed = Icons::icon(QStringLiteral("chevron-right"),
                           theme.color(QStringLiteral("--nav-collapse-icon-color-collapsed")), 12);
    setIndentation(16);
    viewport()->update();
}

void FileTree::drawBranches(QPainter *painter, const QRect &rect, const QModelIndex &index) const
{
    // Depth below the vault root: a root row has none and gets no guide.
    int depth = 0;
    for (QModelIndex p = index.parent(); p.isValid() && p != rootIndex(); p = p.parent())
        ++depth;

    const int step = indentation();
    // Obsidian also brightens the guide of the hovered folder; this one is
    // always the same faint line.
    for (int level = 0; level < depth; ++level) {
        const int x = rect.left() + level * step + step / 2;
        painter->fillRect(QRect(x, rect.top(), m_guideWidth, rect.height()), m_guide);
    }

    if (model()->hasChildren(index)) {
        const QRect cell(rect.left() + depth * step, rect.top(), step, rect.height());
        (isExpanded(index) ? m_open : m_closed).paint(painter, cell, Qt::AlignCenter);
    }
}

bool FileTree::viewportEvent(QEvent *event)
{
    if (event->type() == QEvent::ToolTip) {
        const auto *help = static_cast<QHelpEvent *>(event);
        const QModelIndex index = indexAt(help->pos());
        const auto *files = qobject_cast<const QFileSystemModel *>(model());
        if (index.isValid() && files && m_delegate->isElided(index, visualRect(index).width(), font())) {
            QToolTip::showText(help->globalPos(), files->fileName(index), viewport());
            return true;
        }
        QToolTip::hideText();
        event->ignore();
        return true;
    }
    return QTreeView::viewportEvent(event);
}

} // namespace noteAppC
