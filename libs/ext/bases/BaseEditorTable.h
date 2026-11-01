// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej Marčič
#pragma once

#include "BaseFile.h"
#include "Index.h"

#include <QList>
#include <QString>
#include <QStringList>

namespace noteAppC {
class Vault;
}

namespace noteAppC::bases {

/// One row of a `.base` file's shown view, evaluated to plain display
/// strings: one per column of `order:`, and the note it came from.
struct BaseEditorRow {
    QStringList cells;
    QString notePath; ///< vault-relative
};

/// A `.base` file's views, and one of them evaluated to a plain table -
/// what a ```` ```base ```` block in a note can paint directly, with no
/// widget in sight. Empty (with `error` set) on a YAML/schema/filter
/// problem.
struct BaseEditorTable {
    QStringList viewNames; ///< every view's name, in file order
    int shownView = 0;     ///< index into viewNames/headers/rows came from
    QStringList headers;   ///< the shown view's column headers
    QList<BaseEditorRow> rows;
    QStringList summaries; ///< one per column when the view has any, else empty
    QString error;
};

/// Parses `yamlText` (BaseFile::parse), evaluates a view (BaseQuery -
/// filters, sort, limit all applied) against `notes`, and formats every cell
/// the same way BaseTableModel::data() does for its Qt::DisplayRole -
/// Value::toDisplayString() - so this is a second *renderer* for the same
/// evaluated values, never a second implementation of what a column shows.
///
/// The view comes from `viewName` when it names one (`![[x.base#Reading]]`'s
/// fragment, case-insensitive - Obsidian's own embed syntax), else from
/// `viewIndex` (a ```` ```base ```` block has no fragment to name one, so it
/// always passes the default). Either an unmatched `viewName` or a
/// `viewIndex` out of range (including the default, -1) falls back to the
/// first view - the file's own order is always a defined answer, so there is
/// never a reason to show an error for a view that just does not exist.
///
/// Free of QWidget - unlike BaseView/BaseTableModel, everything this touches
/// (BaseFile, BaseQuery, Value) already is - so it can run on a worker
/// thread the way MarkdownEditor already runs Index::search() for a
/// ```` ```query ```` block. `notes` must already be fetched by the caller:
/// Index::allNotesForBases() reads m_db directly and, unlike search(), is
/// only safe on the thread that opened the Index, so it cannot be called
/// from inside this function if that is meant to run elsewhere. `vault` is
/// used only by BaseQuery to stat() file.ctime, and only if a sort/filter
/// actually reads it - see BaseQuery's own constructor.
BaseEditorTable evaluateBaseForEditor(const QString &yamlText, const QList<Index::BaseNote> &notes,
                                      const Vault *vault = nullptr, int viewIndex = -1,
                                      const QString &viewName = QString());

} // namespace noteAppC::bases
