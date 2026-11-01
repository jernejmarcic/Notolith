// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej Marčič
#pragma once

#include <QList>
#include <QString>

namespace noteAppC {

/// A half-open span of the source text, in UTF-16 code units, which is what
/// QString indexes with.
struct Range {
    qsizetype start = -1;
    qsizetype end = -1;

    bool isValid() const { return start >= 0 && end >= start; }
    qsizetype length() const { return end - start; }
    bool contains(qsizetype offset) const { return offset >= start && offset < end; }
    bool overlaps(const Range &other) const { return start < other.end && other.start < end; }
};

inline bool operator==(const Range &l, const Range &r)
{
    return l.start == r.start && l.end == r.end;
}

/// Line starts of a document, so md4qt's (line, column) positions and plain
/// offsets can be exchanged. Built once per parse.
class SourceMap {
public:
    SourceMap() = default;
    explicit SourceMap(const QString &text);

    /// The text had `removed` characters at `at` replaced by `inserted`: the
    /// same as rebuilding from the new text, at the cost of moving the line
    /// starts after the edit rather than rescanning the document.
    void applyEdit(qsizetype at, qsizetype removed, QStringView inserted);

    qsizetype lineCount() const { return m_lineStarts.size(); }
    /// Offset of the first character of `line`, clamped to the document.
    qsizetype lineStart(qsizetype line) const;
    /// Offset just past the last character of `line`, excluding the newline.
    qsizetype lineEnd(qsizetype line) const;
    qsizetype offset(qsizetype line, qsizetype column) const;
    /// Line holding `offset`; 0 for anything before the start.
    qsizetype lineAt(qsizetype offset) const;

    /// md4qt reports inclusive end columns; this turns a pair of its positions
    /// into a half-open range.
    Range range(qsizetype startLine, qsizetype startColumn,
                qsizetype endLine, qsizetype endColumn) const;

private:
    QList<qsizetype> m_lineStarts;
    qsizetype m_length = 0;
};

} // namespace noteAppC
