// SPDX-License-Identifier: GPL-3.0-or-later
#include "TableFormat.h"

#include "Parser.h"

#include <QRegularExpression>
#include <QStringList>

namespace noteAppC {

namespace {

enum class ColumnAlign { None, Left, Right, Center };

/// East Asian Wide/Fullwidth blocks (Unicode EastAsianWidth, the ranges that
/// matter for source text). Not the whole table - just enough of it that a
/// CJK table lines up the way Obsidian's own tables do.
bool isWideCodepoint(char32_t cp)
{
    struct Block { char32_t lo, hi; };
    static constexpr Block wide[] = {
        {0x1100, 0x115F}, {0x2E80, 0x303E}, {0x3040, 0xA4CF}, {0xAC00, 0xD7A3},
        {0xF900, 0xFAFF}, {0xFE30, 0xFE4F}, {0xFF00, 0xFF60}, {0xFFE0, 0xFFE6},
        {0x1F300, 0x1F64F}, {0x1F900, 0x1F9FF}, {0x20000, 0x2FFFD}, {0x30000, 0x3FFFD},
    };
    for (const Block &b : wide) {
        if (cp >= b.lo && cp <= b.hi)
            return true;
    }
    return false;
}

/// Zero for combining marks and format characters (they draw on top of the
/// previous glyph), two for wide/fullwidth East Asian characters, one
/// otherwise. Self-contained rather than `wcwidth()`: glibc's version
/// returns -1 for non-ASCII under the "C" locale, which is what tests and
/// headless builds run under.
int CJKwidth(char32_t cp)
{
    const QChar::Category category = QChar::category(cp);
    if (category == QChar::Mark_NonSpacing || category == QChar::Mark_Enclosing
        || category == QChar::Other_Format)
        return 0;
    return isWideCodepoint(cp) ? 2 : 1;
}

int displayWidth(const QString &text)
{
    int width = 0;
    for (const char32_t cp : text.toUcs4())
        width += CJKwidth(cp);
    return width;
}

/// The prefix a table line carries inside a quote or callout: its ">"
/// markers and the indentation around them. Kept verbatim; it belongs to
/// the container, not the table.
qsizetype prefixLength(const QString &line)
{
    qsizetype i = 0;
    while (i < line.size() && (line.at(i) == QLatin1Char('>') || line.at(i).isSpace()))
        ++i;
    return i;
}

/// One row's cells, split by the parser's own rule (tableRowCells), so the
/// formatter and the editor can never disagree about where a cell ends.
QStringList parseRow(const QString &line)
{
    QStringList cells;
    for (const Range &cell : tableRowCells(line, 0, line.size()))
        cells << line.mid(cell.start, cell.length());
    return cells;
}

QString delimiterCell(int width, ColumnAlign align)
{
    switch (align) {
    case ColumnAlign::Left:
        return QLatin1Char(':') + QString(width - 1, QLatin1Char('-'));
    case ColumnAlign::Right:
        return QString(width - 1, QLatin1Char('-')) + QLatin1Char(':');
    case ColumnAlign::Center:
        return QLatin1Char(':') + QString(width - 2, QLatin1Char('-')) + QLatin1Char(':');
    case ColumnAlign::None:
        break;
    }
    return QString(width, QLatin1Char('-'));
}

QString padCell(const QString &content, int width, ColumnAlign align)
{
    const int space = qMax(0, width - displayWidth(content));
    switch (align) {
    case ColumnAlign::Right:
        return QString(space, QLatin1Char(' ')) + content;
    case ColumnAlign::Center: {
        const int left = space / 2; // odd padding goes on the right
        return QString(left, QLatin1Char(' ')) + content + QString(space - left, QLatin1Char(' '));
    }
    case ColumnAlign::Left:
    case ColumnAlign::None:
        break;
    }
    return content + QString(space, QLatin1Char(' '));
}

} // namespace

QString formatTable(const QString &table)
{
    const bool trailingNewline = table.endsWith(QLatin1Char('\n'));
    const QString body = trailingNewline ? table.chopped(1) : table;
    const QStringList lines = body.split(QLatin1Char('\n'));

    struct Line {
        QString prefix;
        QString rest;
    };
    QList<Line> tableLines;
    int tableEnd = lines.size();
    for (int i = 0; i < lines.size(); ++i) {
        const QString prefix = lines[i].left(prefixLength(lines[i]));
        const QString rest = lines[i].mid(prefix.size());
        if (!rest.contains(QLatin1Char('|'))) {
            tableEnd = i;
            break;
        }
        tableLines.append({prefix, rest});
    }
    if (tableLines.size() < 2)
        return table; // no delimiter row possible - not a table

    QList<QStringList> rows;
    rows.reserve(tableLines.size());
    for (const Line &line : tableLines)
        rows.append(parseRow(line.prefix + line.rest));

    // The delimiter row (line 2) decides both validity and alignment.
    static const QRegularExpression delimiterRe(QStringLiteral("^(:?)-+(:?)$"));
    QList<ColumnAlign> alignment;
    for (const QString &cell : rows[1]) {
        const auto match = delimiterRe.match(cell.trimmed());
        if (!match.hasMatch())
            return table; // line 2 is not a delimiter row - not a table
        const bool left = !match.captured(1).isEmpty();
        const bool right = !match.captured(2).isEmpty();
        alignment.append(left && right ? ColumnAlign::Center
                          : left       ? ColumnAlign::Left
                          : right      ? ColumnAlign::Right
                                       : ColumnAlign::None);
    }

    // Column count = the widest row anywhere in the table (Advanced Tables'
    // behaviour); short rows, delimiter included, are padded out below.
    int columns = 0;
    for (const QStringList &row : rows)
        columns = qMax(columns, int(row.size()));
    while (alignment.size() < columns)
        alignment.append(ColumnAlign::None);

    QList<int> width(columns, 3); // minimum column width
    for (int r = 0; r < rows.size(); ++r) {
        if (r == 1)
            continue; // the delimiter row has no content to measure
        for (int c = 0; c < rows[r].size(); ++c)
            width[c] = qMax(width[c], displayWidth(rows[r][c].trimmed()));
    }

    QStringList out;
    for (int r = 0; r < tableLines.size(); ++r) {
        QStringList cells;
        for (int c = 0; c < columns; ++c) {
            if (r == 1)
                cells << delimiterCell(width[c], alignment[c]);
            else
                cells << padCell(c < rows[r].size() ? rows[r][c].trimmed() : QString(),
                                  width[c], alignment[c]);
        }
        out << tableLines[r].prefix + QLatin1Char('|') + QLatin1Char(' ')
                   + cells.join(QLatin1String(" | ")) + QLatin1String(" |");
    }
    for (int i = tableEnd; i < lines.size(); ++i)
        out << lines[i];

    QString result = out.join(QLatin1Char('\n'));
    if (trailingNewline)
        result += QLatin1Char('\n');
    return result;
}

} // namespace noteAppC
