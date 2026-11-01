// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej Marčič
#pragma once

#include <QList>
#include <QString>
#include <QStringList>

namespace noteAppC {

/// One row of a rendered ```` ```base ```` block's table: display strings,
/// one per column of the shown view's `order:`, and the note the row came
/// from. Mirrors bases::BaseEditorRow field for field - see
/// MarkdownEditor::setBaseRunner. libs/editor never includes
/// libs/ext/bases (docs/PLUGINS.md's module boundary), so the shape is
/// repeated here rather than shared, the same choice QueryResult.h already
/// made for ```query``` blocks.
struct BaseTableRow {
    QStringList cells;
    QString notePath; ///< vault-relative
};

/// A `.base` file evaluated to what the editor can paint directly: every
/// view's name (so a block always has something to print above the table,
/// even though only `shownView` is actually rendered - there is no
/// view-switcher in a note the way BaseView's widget has one), the shown
/// view's column headers and rows, and an error string set - with
/// everything else left empty - on a YAML, schema or filter/sort problem.
struct BaseTableData {
    QStringList viewNames;
    int shownView = 0;
    QStringList headers;
    QList<BaseTableRow> rows;
    QStringList summaries; ///< one per column when the view has any, else empty
    QString error;
};

} // namespace noteAppC
