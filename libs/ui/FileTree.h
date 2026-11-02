// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QColor>
#include <QFileInfo>
#include <QHash>
#include <QIcon>
#include <QPersistentModelIndex>
#include <QStyledItemDelegate>
#include <QTreeView>

#include <functional>

namespace noteAppC {

class Theme;

/// A bundled Lucide id goes through Icons::icon (cached per name, colour and
/// size); any other string - an emoji, one character - is drawn once into a
/// pixmap with the UI font. Null when `name` is empty. The one place that
/// turns an icon string into a QIcon, so the file tree and the tabs agree.
QIcon fileIcon(const QString &name, const QColor &colour, int size);

/// One file explorer row. Draws its own hover / selected pill inside the
/// row rectangle only: QTreeView paints the item background over the indent
/// columns too, so a QSS `::item:selected` background lights the gutter left
/// of the row. The stylesheet therefore keeps those two states transparent.
class FileRowDelegate : public QStyledItemDelegate {
    Q_OBJECT
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    /// What the row shows: the name without the extension for files (the
    /// extension becomes the tag), the plain name for folders.
    static QString displayName(const QFileInfo &info);
    /// Upper-case extension for anything that is not a note or a folder
    /// ("CANVAS", "PDF"); empty for `.md` and folders.
    static QString tagFor(const QFileInfo &info);

    void setColours(const Theme &theme);
    /// Absolute path -> icon id, empty for none. Unset: no icons.
    void setIconHook(std::function<QString(const QString &absolutePath)> hook);

    int rowHeight() const { return m_rowHeight; }
    /// Width the name has inside a row of `rowWidth` before it is cut off.
    bool isElided(const QModelIndex &index, int rowWidth, const QFont &font) const;

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override;
    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override;

    /// Inline rename. The editor never writes to the model: the new name is
    /// handed to renameRequested() so the window renames through the vault
    /// (links, history and plugins follow), not through QFileSystemModel.
    QWidget *createEditor(QWidget *parent, const QStyleOptionViewItem &option,
                          const QModelIndex &index) const override;
    void setEditorData(QWidget *editor, const QModelIndex &index) const override;
    void setModelData(QWidget *editor, QAbstractItemModel *model,
                      const QModelIndex &index) const override;

signals:
    void renameRequested(const QPersistentModelIndex &index, const QString &newName);

private:
    QString tagText(const QModelIndex &index) const;

    QColor m_text, m_textActive, m_hover, m_selected, m_active, m_tagText, m_tagBackground;
    int m_radius = 4;
    int m_rowHeight = 26;
    int m_tagPixelSize = 9;
    std::function<QString(const QString &)> m_iconHook;
    mutable QHash<QString, QIcon> m_icons;
};

/// The vault's file explorer: indent guides and themed chevrons drawn in
/// drawBranches() (so they follow the theme tokens in both schemes), and a
/// tooltip with the full name when a row is cut off.
class FileTree : public QTreeView {
    Q_OBJECT
public:
    explicit FileTree(QWidget *parent = nullptr);
    void setColours(const Theme &theme);
    FileRowDelegate *rowDelegate() const { return m_delegate; }

    /// The folder (absolute) a drop at `viewportPos` lands in: the folder
    /// row under it, a file row's own folder, or the tree's root on empty
    /// space. Empty when the row being dragged cannot go there (a folder into
    /// itself or one of its own subfolders).
    QString dropFolderAt(const QPoint &viewportPos) const;

    /// What a drag from this tree carries: the model's mime data for the
    /// selected row (its file URL) plus a marker naming this tree, so a drop
    /// can tell its own drag from another app's or another window's. Null
    /// when nothing is selected. The caller owns it.
    QMimeData *dragData() const;

    /// Takes files dragged in from another program (a file manager) too, for
    /// filesDropped(), when `wanted` says yes to at least one of them; empty,
    /// the tree only takes its own drags.
    void setAcceptFileDrops(std::function<bool(const QString &path)> wanted) { m_fileDrops = std::move(wanted); }

signals:
    /// Files from outside were dropped on `folder` (absolute).
    void filesDropped(const QStringList &files, const QString &folder);
    /// A drop wants `path` moved into `targetFolder`. The tree never moves a
    /// file itself - QFileSystemModel would skip the links, history and tabs
    /// the window keeps in step.
    void moveRequested(const QString &path, const QString &targetFolder);

protected:
    void startDrag(Qt::DropActions supportedActions) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dragMoveEvent(QDragMoveEvent *event) override;
    void dropEvent(QDropEvent *event) override;
    void drawBranches(QPainter *painter, const QRect &rect, const QModelIndex &index) const override;
    bool viewportEvent(QEvent *event) override;

private:
    bool isOwnDrag(const QMimeData *data) const;
    bool isFileDrop(const QMimeData *data) const;
    std::function<bool(const QString &)> m_fileDrops;

    FileRowDelegate *m_delegate = nullptr;
    QColor m_guide;
    int m_guideWidth = 1;
    QIcon m_open, m_closed;
};

} // namespace noteAppC
