// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej Marčič
#pragma once

#include "BaseFile.h"
#include "Value.h"

#include <QCache>
#include <QFutureWatcher>
#include <QPixmap>
#include <QWidget>

#include <functional>
#include <memory>
#include <optional>

class QComboBox;
class QGridLayout;
class QLabel;
class QScrollArea;
class QStackedWidget;
class QTableView;

namespace noteAppC {
class Index;
class Vault;
}

namespace noteAppC::bases {

class BaseQuery;
class BaseTableModel;

/// Renders a `.base` file: a view switcher over its `views:`, a table (a
/// `BaseTableModel` in a `QTableView`, sortable by clicking a header, a
/// `file.name` cell that opens its note) and cards (a scrolling grid, an
/// optional cover image, read-only - see the module report for why cards
/// does not share the table's cell editor).
///
/// Takes no ownership of `Index`/`Vault`, the way `BacklinksPane` takes an
/// `Index*` - MainWindow is expected to own both for the life of the window
/// and hand this widget pointers into them, the same shape every other pane
/// in `libs/ui` already uses.
class BaseView : public QWidget {
    Q_OBJECT

public:
    explicit BaseView(QWidget *parent = nullptr);
    ~BaseView() override;

    void setIndex(Index *index);
    void setVault(const Vault *vault);
    /// Resolves a raw link/image target (a wikilink's inside text, a plain
    /// path) to a vault-relative path, the same job MainWindow's own
    /// `m_resolveLink` does for the editor and canvas - wire it the same
    /// way (`setFileResolver`) once this view opens a real `.base` file.
    /// Unset, a link cell falls back to treating the target as already a
    /// path (appending ".md" if it looks bare), which is only right for the
    /// simplest vaults - good enough for a click to usually work, not a
    /// substitute for the real resolver.
    void setLinkResolver(std::function<QString(const QString &)> resolver);
    /// See BaseTableModel::setBeforeWrite; set before open().
    void setBeforeWrite(std::function<bool(const QString &absolutePath)> beforeWrite) { m_beforeWrite = std::move(beforeWrite); }

    /// Parses `yamlText` (already read from `absolutePath` by the caller)
    /// and opens its first view, whose rows arrive with viewOpened(). False
    /// with lastError() set on a YAML or schema error. `absolutePath` is
    /// kept: a header click's sort write-back (see onHeaderSortChanged) and
    /// a cell edit (BaseTableModel) are this module's only file I/O, and
    /// both touch only the bytes they changed.
    bool open(const QString &absolutePath, const QString &yamlText);
    void clear();
    QString lastError() const { return m_lastError; }
    /// Problems seen evaluating the current view's filters/sort/formulas -
    /// each shown once, however many rows hit it; empty when nothing did.
    QStringList warnings() const;

signals:
    /// The note at `relativePath` should be opened in the editor - a
    /// double-clicked row, or the `file.name` cell.
    void noteActivated(const QString &relativePath);
    /// The current view's rows are showing (or its error is).
    void viewOpened();

private slots:
    void onViewSwitched(int index);
    void onTableActivated(const QModelIndex &index);
    void onTableClicked(const QModelIndex &index);
    void onCellEdited(const QString &relativePath);
    /// A header click, once Qt has already resorted the table: also saves
    /// that single (property, direction) into the `.base` file's `sort:` -
    /// see BaseFile::rewriteViewSort - unless this view came from a
    /// ```` ```base ```` block in a note (m_path empty), which this
    /// deliberately never rewrites; see the module report for why.
    void onHeaderSortChanged(int column, Qt::SortOrder order);

private:
    void rebuildViewSwitcher();
    void openCurrentView();
    void rebuildCards();
    QWidget *makeCard(int position);
    /// One card field: a checkbox for a Bool, chips (+ a comma editor) for a
    /// List, a line edit for anything else editable, a plain label
    /// otherwise - `position` is this row's place in resultRows(), the same
    /// index BaseTableModel's own rows use, so both write through
    /// `BaseTableModel::setData` on the identical (row, column).
    QWidget *makeCardField(int position, const QString &reference, const Value &value);
    /// One chip: a plain pill for anything, a clickable one (opens the note
    /// on click) when `item` is a Link - shared between a list field's row
    /// of chips and a single Link field, which is just a row of one.
    QLabel *makeChipLabel(const Value &item);
    /// Resolves `rawTarget` (a link cell's target, or a card field's
    /// image link) and, if it names a note, emits noteActivated();
    /// `#anchor`/`^block` suffixes are stripped before resolving, same as
    /// file.hasLink()'s own comparisons.
    void openLinkTarget(const QString &rawTarget);
    /// Off the UI thread, cached, scaled to `size` - a card's cover image.
    /// `absolutePath` already resolved (a `[[...]]`/plain path; never a
    /// remote URL, which showOpenedView's caller never asks this for).
    void loadCardImage(QLabel *target, const QString &absolutePath, const QSize &size);
    /// The currently selected view's config, null before anything is open.
    const ViewConfig *view() const;
    void showOpenedView();

    /// A view evaluated on the pool: the notes and the evaluation were 54
    /// and 71 ms on 20 000 notes, paid on every open, switch and cell edit.
    struct Opened {
        std::shared_ptr<const BaseFile> file; ///< what the query points into
        std::shared_ptr<const Vault> vault;   ///< the query's, for file.ctime
        std::shared_ptr<BaseQuery> query;     ///< null on error
        QString error;
    };
    QFutureWatcher<Opened> m_opening;

    Index *m_index = nullptr;
    const Vault *m_vault = nullptr;
    std::function<QString(const QString &)> m_linkResolver;
    std::function<bool(const QString &)> m_beforeWrite;
    /// Shared with a view evaluating on the pool, whose query points into it.
    std::shared_ptr<const BaseFile> m_file;
    Opened m_shown; ///< keeps the query shown, and what it points into, alive
    BaseQuery *m_query = nullptr;
    QString m_lastError;
    int m_currentView = 0;
    QString m_path;     ///< open()'s absolutePath - empty for a ```base``` block, which is never rewritten
    QString m_yamlText; ///< open()'s yamlText, kept current through a sort write-back
    QCache<QString, QPixmap> m_imageCache{200}; ///< card covers, keyed by resolved absolute path

    QComboBox *m_viewSwitcher = nullptr;
    QLabel *m_status = nullptr;
    QStackedWidget *m_stack = nullptr;
    QTableView *m_table = nullptr;
    BaseTableModel *m_tableModel = nullptr;
    QScrollArea *m_cardsArea = nullptr;
    QWidget *m_cardsContent = nullptr;
    QGridLayout *m_cardsLayout = nullptr;
};

} // namespace noteAppC::bases
