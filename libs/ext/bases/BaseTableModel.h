// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <functional>

#include "Value.h"

#include <QAbstractTableModel>
#include <QMetaType>

namespace noteAppC {
class Index;
class Vault;
}

namespace noteAppC::bases {

class BaseQuery;

/// One chip a List or Link cell paints (BaseCellDelegate) - `isLink` is what
/// tells the delegate to colour it as a link and BaseView's click handling
/// to treat it as one, since a plain-string chip is neither.
struct BaseChip {
    QString text;
    bool isLink = false;
};

/// One row per matching note, one column per the open view's `order:`
/// entries. Sorting is Qt's own: `QTableView::setSortingEnabled(true)`
/// calls `sort()` below when a header is clicked, which is the "sortable by
/// clicking a header" the module report promises.
///
/// Editing a cell writes straight to the note's frontmatter through
/// `Frontmatter::scalarFor`/`Frontmatter::rewriteList` and `Vault::write` -
/// only that property's bytes change, per the project's file-is-truth rule -
/// then refreshes the index so every other open view sees the edit
/// immediately. Every *existing* property is editable, including List (the
/// default text editor shows/accepts a comma-joined line, split back into
/// items on commit - a comma inside an item cannot round-trip, the one
/// corner this cuts); a `file.x`/`formula.x` column is computed and never
/// editable, and a property that does not yet exist in the note's
/// frontmatter cannot be created from a table cell - adding a property is
/// Obsidian's own "Properties" UI, out of scope for this pass.
class BaseTableModel : public QAbstractTableModel {
    Q_OBJECT

public:
    /// data()'s chips for a List or Link cell - a `QList<BaseChip>`, for
    /// BaseCellDelegate's painting and nothing else (BaseView's click
    /// handling reads the underlying Value directly through valueAt()
    /// instead, since it needs to know a chip's *target*, not just its
    /// label).
    static constexpr int ChipRole = Qt::UserRole + 1;

    /// None of the three pointers are owned. `query` must outlive this model
    /// and already have a view open; `index`/`vault` are only used to write
    /// an edit back and refresh the cache - pass null to make every cell
    /// read-only.
    BaseTableModel(BaseQuery *query, Index *index, const Vault *vault, QObject *parent = nullptr);
    /// Called with a note's absolute path just before an edit is written into
    /// it: the window saves unsaved typing there first, or its next save would
    /// write the old value back. False refuses the edit.
    void setBeforeWrite(std::function<bool(const QString &absolutePath)> beforeWrite) { m_beforeWrite = std::move(beforeWrite); }

    /// Call after `query`'s results changed (a new view opened, a filter
    /// re-run) to make the model match.
    void refresh();

    int rowCount(const QModelIndex &parent = {}) const override;
    int columnCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    bool setData(const QModelIndex &index, const QVariant &value, int role = Qt::EditRole) override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;
    Qt::ItemFlags flags(const QModelIndex &index) const override;
    void sort(int column, Qt::SortOrder order) override;

    /// The note `row` shows, vault-relative - empty if `row` is out of range.
    QString notePathAt(int row) const;
    /// True when `column`'s property reference is the note's own name
    /// (`file.name`) - BaseView makes that specific cell open the note.
    bool isNameColumn(int column) const;
    /// A last row of the view's summaries, under the notes, when it has any.
    bool hasFooter() const;
    bool isFooter(int row) const;
    /// The column showing `propertyReference`, or -1.
    int columnOf(const QString &propertyReference) const;
    /// The raw, still-typed Value at `row`/`column` - what BaseView's click
    /// handling reads to find a Link cell's (or a list's first Link's)
    /// target, and what BaseCellDelegate's chips are built from.
    Value valueAt(int row, int column) const;

signals:
    /// A cell edit wrote to a note's frontmatter and the index was
    /// refreshed; BaseView reruns the query so every column reflects it.
    void noteEdited(const QString &relativePath);

private:
    std::function<bool(const QString &)> m_beforeWrite;
    QString propertyReferenceAt(int column) const;
    bool writePropertyEdit(const QString &relativePath, const QString &key,
                           const QString &newText, QString *error);

    BaseQuery *m_query;
    Index *m_index;
    const Vault *m_vault;
};

} // namespace noteAppC::bases

Q_DECLARE_METATYPE(noteAppC::bases::BaseChip)
Q_DECLARE_METATYPE(QList<noteAppC::bases::BaseChip>)
