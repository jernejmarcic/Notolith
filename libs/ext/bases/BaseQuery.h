// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej Marčič
#pragma once

#include "BaseFile.h"
#include "Expression.h"
#include "Index.h"

#include <memory>

namespace noteAppC {
class Vault;
}

namespace noteAppC::bases {

/// Turns one `Index::BaseNote` into the `Row` Expression reads: file facts
/// split out of its vault-relative path, tags and links copied straight
/// over, and each frontmatter property converted to the `Value` its declared
/// type says it is (a `List` property becomes `Value::List`, `Number`
/// becomes `Value::Number`, and so on - see `libs/markdown/Metadata.h`'s
/// `Property::Type`). `file.ctime` is left invalid; `BaseQuery::openView`
/// backfills it with a single stat() pass, and only for a view that actually
/// reads it.
Row rowFromNote(const Index::BaseNote &note);

/// One of Obsidian's built-in summaries over a column's values, as the
/// footer shows it: Average, Min, Max, Sum, Range, Median, Stddev (numbers),
/// Earliest, Latest, Range (dates), Checked, Unchecked (checkboxes), Empty,
/// Filled, Unique (anything). Case-insensitive; "" for a name it does not
/// know or a column with nothing to summarise.
QString summarize(const QString &name, const QList<Value> &values);

/// Filters, sorts, groups and limits a `.base` file's rows for one of its
/// views. Independent of Index and Vault beyond `rowFromNote` above: tests
/// can hand it synthetic rows directly, which is what makes the 20 000-row
/// performance guard fast to run.
///
/// Every property reference - a filter statement, an `order:`/`sort:`/
/// `groupBy:` entry, a formula - is compiled once when the view is opened
/// and reused for every row, which is what keeps a full pass well under the
/// 100 ms budget; see the module's test for the measured number.
class BaseQuery {
public:
    /// `vault`, when given, is used only to stat() `file.ctime` - once, for
    /// every row, and only if `openView` finds the view actually reads it.
    BaseQuery(const BaseFile &file, QList<Row> rows, const Vault *vault = nullptr);
    ~BaseQuery();
    BaseQuery(const BaseQuery &) = delete;
    BaseQuery &operator=(const BaseQuery &) = delete;

    /// Compiles and runs `viewIndex`'s filters, sort and grouping. False with
    /// `*error` only for a problem that makes the whole view unusable (a
    /// bad view index, or a filter/sort expression that fails to parse); a
    /// problem evaluating one particular row (an unknown property, a type
    /// mismatch) instead excludes that row and is recorded in warnings().
    bool openView(int viewIndex, QString *error = nullptr);

    const BaseFile &file() const { return m_file; }
    const ViewConfig &view() const;
    const QList<Row> &rows() const { return m_rows; }
    /// Indices into rows(), filtered/sorted/grouped/limited - what a table or
    /// card grid iterates to build itself.
    const QList<qsizetype> &resultRows() const { return m_result; }

    /// `propertyReference`'s value for `rows()[rowIndex]` - `file.x`,
    /// `note.x`/a bare key, or `formula.x`, resolving formulas (with cycle
    /// protection) the same way a filter or a formula-of-a-formula would.
    Value valueOf(qsizetype rowIndex, const QString &propertyReference) const;
    /// The view's summary for `propertyReference` over resultRows(), "" when
    /// the view gives it none: a file's own top-level summary formula of
    /// that name (an expression over `values`), else a built-in.
    QString summaryOf(const QString &propertyReference) const;

    /// Re-orders resultRows() by an arbitrary property - a clicked table
    /// header - without touching the view's own configured sort or the
    /// `.base` file. A value that fails to evaluate sorts last, next to
    /// Value::empty(); see Value.h's compareForSort for the exact rule.
    void sortBy(const QString &propertyReference, SortDirection direction);

    /// This row's value for the view's `groupBy` property, "" when the view
    /// is not grouped or the key could not be computed - BaseView inserts a
    /// section break where this changes between consecutive result rows.
    QString groupKeyOf(qsizetype rowIndex) const;

    /// Distinct, human-readable evaluation problems seen while opening or
    /// sorting the view - shown once each, however many rows hit them.
    const QStringList &warnings() const { return m_warnings; }

private:
    class Resolver;

    std::shared_ptr<Expression> compiled(const QString &text) const;
    void warnOnce(const QString &message) const;
    bool referencesCtime(const QString &text) const;

    const BaseFile &m_file;
    QList<Row> m_rows;
    const Vault *m_vault = nullptr;
    int m_viewIndex = -1;
    QList<qsizetype> m_result;
    QDateTime m_now;
    std::unique_ptr<Resolver> m_resolver;
    mutable QHash<QString, std::shared_ptr<Expression>> m_compiled;
    mutable QStringList m_warnings;
};

} // namespace noteAppC::bases
