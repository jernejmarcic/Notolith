/// A table in live preview is edited where it is drawn, as in Obsidian: a
/// click lands in the cell under the pointer, the caret is drawn there, and
/// the table stays a table. The cell holding the caret shows its source, the
/// way any element under the caret does; the rest of the table does not.
///
/// The keys keep the caret in cells: Tab and Shift+Tab step between them,
/// Enter goes down a row (adding one below the last, as Advanced Tables
/// does), the arrows cross from one cell into the next, and Backspace or
/// Delete at a cell's edge does nothing rather than eat a pipe. A "|" typed
/// or pasted into a cell is escaped, a line break becomes a space. Source
/// mode is still where the pipes themselves are edited.
///
/// An HTML `<table>` the editor lays out itself (HtmlEdit.cpp) is the same
/// TableSpan, its cells the text between the tags: everything here applies,
/// and the few places that write Markdown hand over to an HTML counterpart.

#include "MarkdownEditor.h"
#include "MarkdownEditor_p.h"

#include "Parser.h"
#include "TableFormat.h"

#include <QCollator>
#include <QKeyEvent>
#include <QMenu>
#include <QScrollBar>
#include <QTextLayout>

namespace noteAppC {

namespace {

constexpr qreal kCellPadX = 10;
constexpr qreal kCellPadY = 6;

} // namespace

MarkdownEditor::CellRef MarkdownEditor::tableCellAt(qsizetype source) const
{
    if (d->m_mode == Mode::Source)
        return {};
    for (const TableSpan &table : d->m_decorations.tables) {
        if (source < table.range.start || source > table.range.end)
            continue;
        for (int r = 0; r < table.rows.size(); ++r) {
            const QList<Range> &row = table.rows.at(r);
            for (int c = 0; c < row.size() && c < table.columns; ++c) {
                if (row.at(c).start <= source && source <= row.at(c).end)
                    return {&table, r, c, row.at(c)};
            }
        }
        return {&table, -1, -1, {}};
    }
    return {};
}

qsizetype MarkdownEditor::snapToTableCell(qsizetype offset, qsizetype from) const
{
    const CellRef here = tableCellAt(offset);
    if (!here.valid() || here.row >= 0 || !tableRenderedAt(here.table->range.start))
        return offset; // not in a drawn table, or already in a cell
    const TableSpan &table = *here.table;
    const bool forward = offset >= from;
    const Range *best = nullptr;
    for (const QList<Range> &row : table.rows) {
        for (int c = 0; c < row.size() && c < table.columns; ++c) {
            const Range &cell = row.at(c);
            if (forward && cell.start >= offset && (!best || cell.start < best->start))
                best = &cell;
            if (!forward && cell.end <= offset && (!best || cell.end > best->end))
                best = &cell;
        }
    }
    if (best)
        return forward ? best->start : best->end;
    // Past the last cell, or before the first: out of the table altogether.
    if (forward)
        return qMin<qsizetype>(d->m_text.size(), table.range.end + 1);
    return table.range.start > 0 ? table.range.start - 1 : table.rows.value(0).value(0).start;
}

QPointF MarkdownEditor::cellTextOrigin(const BlockView &view, const Cell &cell) const
{
    const qreal width = cell.rect.width() - 2 * kCellPadX;
    qreal offset = 0;
    if (cell.layout && cell.layout->lineCount() > 0) {
        const qreal lineWidth = cell.layout->lineAt(0).naturalTextWidth();
        if (cell.alignment.testFlag(Qt::AlignRight))
            offset = width - lineWidth;
        else if (cell.alignment.testFlag(Qt::AlignHCenter))
            offset = (width - lineWidth) / 2;
    }
    return {cell.rect.left() - view.sideScroll + kCellPadX + qMax<qreal>(0, offset),
            cell.rect.top() + kCellPadY};
}

const MarkdownEditor::Cell *MarkdownEditor::caretCell(const BlockView &view) const
{
    if (view.x().cells.empty() || d->m_anchor != d->m_cursor)
        return nullptr;
    for (const Cell &cell : view.x().cells) {
        if (cell.source.start <= d->m_cursor && d->m_cursor <= cell.source.end)
            return &cell;
    }
    return nullptr;
}

qsizetype MarkdownEditor::tableAround(qsizetype offset) const
{
    for (qsizetype i = 0; i < d->m_decorations.tables.size(); ++i) {
        const Range &range = d->m_decorations.tables.at(i).range;
        if (d->m_lines.lineStart(d->m_lines.lineAt(range.start)) <= offset && offset <= range.end)
            return i;
    }
    return -1;
}

bool MarkdownEditor::formatTables(qsizetype offset)
{
    // Whole lines, so a table in a callout keeps its "> " markers.
    struct Edit {
        qsizetype table;
        qsizetype start;
        qsizetype end;
        QString text;
    };
    QList<Edit> edits;
    const qsizetype only = offset < 0 ? -1 : tableAround(offset);
    if (offset >= 0 && only < 0)
        return false;
    for (qsizetype i = 0; i < d->m_decorations.tables.size(); ++i) {
        if ((only >= 0 && i != only) || d->m_decorations.tables.at(i).html)
            continue; // an HTML table's source is left as written
        const Range &range = d->m_decorations.tables.at(i).range;
        const qsizetype start = d->m_lines.lineStart(d->m_lines.lineAt(range.start));
        const qsizetype end = d->m_lines.lineEnd(d->m_lines.lineAt(qMax(range.start, range.end - 1)));
        const QString source = d->m_text.mid(start, end - start);
        const QString aligned = formatTable(source);
        if (aligned != source)
            edits.append({i, start, end, aligned});
    }
    if (edits.isEmpty())
        return false;

    // The caret stays in its cell, at the same place in its text; outside
    // every table it moves with the text around it.
    const CellRef cell = tableCellAt(d->m_cursor);
    qsizetype inCell = cell.row >= 0 ? d->m_cursor - cell.range.start : -1;
    qsizetype caret = d->m_cursor;
    for (const Edit &edit : std::as_const(edits)) {
        if (edit.end <= d->m_cursor && !(cell.row >= 0 && tableAround(d->m_cursor) == edit.table))
            caret += edit.text.size() - (edit.end - edit.start);
    }
    const qsizetype caretTable = cell.row >= 0 ? tableAround(d->m_cursor) : -1;
    const int row = cell.row;
    const int column = cell.column;

    d->m_lastEdit.invalidate(); // one step of its own, not the end of the typing before it
    pushUndo();
    d->m_formattingTables = true;
    for (qsizetype i = edits.size() - 1; i >= 0; --i)
        applyEdit(edits.at(i).start, edits.at(i).end - edits.at(i).start, edits.at(i).text);
    d->m_formattingTables = false;
    d->m_lastEdit.invalidate();
    reparse();

    if (caretTable >= 0 && caretTable < d->m_decorations.tables.size()) {
        const TableSpan &table = d->m_decorations.tables.at(caretTable);
        const Range moved = table.rows.value(row).value(column);
        if (moved.isValid())
            caret = moved.start + qMin(inCell, moved.length());
    }
    d->m_tableEdited = false;
    moveCursor(caret, false);
    return true;
}

bool MarkdownEditor::tableRenderedAt(qsizetype offset) const
{
    const qsizetype index = blockAt(offset);
    return index >= 0 && index < qsizetype(d->m_blocks.size()) && !d->m_blocks[size_t(index)].x().cells.empty();
}

void MarkdownEditor::revealCaretCell()
{
    const qsizetype index = blockAt(d->m_cursor);
    if (index < 0 || index >= qsizetype(d->m_blocks.size()))
        return;
    BlockView &view = d->m_blocks[size_t(index)];
    const Cell *cell = caretCell(view);
    if (!cell)
        return;
    const qreal overflow = view.sideWidth - contentWidth();
    if (overflow <= 0)
        return;
    qreal scroll = view.sideScroll;
    if (cell->rect.left() < scroll)
        scroll = cell->rect.left();
    else if (cell->rect.right() > scroll + contentWidth())
        scroll = cell->rect.right() - contentWidth();
    view.sideScroll = qBound<qreal>(0, scroll, overflow);
}

void MarkdownEditor::insertTableRowAfter(const CellRef &cell, int column)
{
    const TableSpan &table = *cell.table;
    if (table.html) {
        insertHTMLrow(&table - d->m_decorations.tables.constData(), cell.row, true, column);
        return;
    }
    const qsizetype lineEnd = d->m_lines.lineEnd(d->m_lines.lineAt(cell.range.start));
    // The new row copies the one it follows up to its first pipe, so a row
    // inside a callout keeps its "> ".
    const qsizetype lineStart = d->m_lines.lineStart(d->m_lines.lineAt(cell.range.start));
    qsizetype prefixEnd = lineStart;
    while (prefixEnd < lineEnd && (d->m_text.at(prefixEnd) == QLatin1Char('>') || d->m_text.at(prefixEnd).isSpace()))
        ++prefixEnd;
    QString row = QLatin1Char('\n') + d->m_text.mid(lineStart, prefixEnd - lineStart) + QLatin1Char('|');
    for (int c = 0; c < table.columns; ++c)
        row += QStringLiteral("  |");

    d->m_lastEdit.invalidate(); // its own undo step, not part of the typing before it
    pushUndo();
    applyEdit(lineEnd, 0, row);
    // The row has to be a row before the caret can go into it: parse now,
    // not in 20 ms, or the caret lands in no cell and is snapped away. (The
    // newline in the row may have been parsed by applyEdit already.)
    if (d->m_parsedVersion != d->m_textVersion)
        reparse();
    const qsizetype newLine = d->m_lines.lineAt(lineEnd + 1);
    const QList<Range> cells = tableRowCells(d->m_text, d->m_lines.lineStart(newLine), d->m_lines.lineEnd(newLine));
    moveCursor(cells.value(qBound(0, column, int(cells.size()) - 1)).start, false);
}

QString MarkdownEditor::cellSafe(const QString &text) const
{
    const Range selected = selection();
    if (const HtmlTextSpan *html = HTMLtextAt(selected.start); html && selected.end <= html->content.end)
        return HTMLsafe(text, *html); // an HTML cell or paragraph: `&` is `&amp;`
    const CellRef cell = tableCellAt(selected.start);
    if (cell.row < 0 || selected.end > cell.range.end || !tableRenderedAt(selected.start))
        return text;
    // A pipe would end the cell and a line break the row.
    QString safe = text;
    safe.replace(QLatin1Char('\n'), QLatin1Char(' '));
    safe.replace(QLatin1Char('|'), QStringLiteral("\\|"));
    return safe;
}

bool MarkdownEditor::fillShortRow(qsizetype table, int row)
{
    if (d->m_mode == Mode::Reading) // filling writes to the note
        return false;
    if (table < 0 || table >= d->m_decorations.tables.size())
        return false;
    const TableSpan &span = d->m_decorations.tables.at(table);
    if (span.html)
        return fillShortHTMLrow(table, row);
    const QList<Range> cells = span.rows.value(row);
    if (cells.isEmpty() || cells.size() >= span.columns)
        return false;
    const qsizetype lineEnd = d->m_lines.lineEnd(d->m_lines.lineAt(cells.last().start));
    // A row written without its closing pipe gets one before the new cells.
    qsizetype last = lineEnd;
    while (last > cells.last().end && d->m_text.at(last - 1).isSpace())
        --last;
    QString added = last > cells.last().end && d->m_text.at(last - 1) == QLatin1Char('|') ? QString() : QStringLiteral(" |");
    for (qsizetype c = cells.size(); c < span.columns; ++c)
        added += QStringLiteral("  |");

    const qsizetype caret = d->m_cursor;
    d->m_lastEdit.invalidate();
    pushUndo();
    applyEdit(last, lineEnd - last, added);
    d->m_lastEdit.invalidate();
    reparse();
    // The caret stays where it was; the click or the key that asked moves it.
    d->m_cursor = d->m_anchor = caret <= last ? caret : caret + added.size() - (lineEnd - last);
    return true;
}

bool MarkdownEditor::fillShortRowAt(const QPoint &viewportPoint)
{
    const qreal y = viewportPoint.y() + verticalScrollBar()->value();
    const qreal x = viewportPoint.x() - contentLeft();
    for (const BlockView &view : d->m_blocks) {
        if (view.x().cells.empty() || view.foldHidden || y < view.y || y > view.y + view.height
            || d->m_mode == Mode::Source)
            continue;
        const TableSpan *table = tableFor(view);
        if (!table)
            return false; // a base's cells, not a table's
        for (const Cell &cell : view.x().cells) {
            if (!cell.source.isValid() && cell.rect.translated(-view.sideScroll, view.y).contains(QPointF(x, y)))
                return fillShortRow(table - d->m_decorations.tables.constData(), cell.row);
        }
        return false;
    }
    return false;
}

bool MarkdownEditor::handleTableKey(QKeyEvent *event)
{
    if (d->m_anchor != d->m_cursor)
        return false;
    const CellRef cell = tableCellAt(d->m_cursor);
    if (!cell.valid() || cell.row < 0 || !tableRenderedAt(d->m_cursor))
        return false;
    const TableSpan &table = *cell.table;
    const QList<Range> &row = table.rows.at(cell.row);
    const int columns = int(qMin<qsizetype>(row.size(), table.columns));

    switch (event->key()) {
    case Qt::Key_Tab: {
        // The next cell is one this short row lacks: give the row its cells.
        if (cell.column + 1 < table.columns && cell.column + 1 >= columns) {
            const qsizetype index = &table - d->m_decorations.tables.constData();
            const int rowIndex = cell.row;
            const int next = cell.column + 1;
            if (fillShortRow(index, rowIndex)) {
                moveCursor(d->m_decorations.tables.at(index).rows.value(rowIndex).value(next).start, false);
                return true;
            }
        }
        if (cell.column + 1 < columns) {
            moveCursor(row.at(cell.column + 1).end, false);
        } else if (cell.row + 1 < table.rows.size()) {
            moveCursor(table.rows.at(cell.row + 1).value(0).end, false);
        } else {
            insertTableRowAfter(cell, 0);
        }
        return true;
    }
    case Qt::Key_Backtab: {
        if (cell.column > 0)
            moveCursor(row.at(cell.column - 1).end, false);
        else if (cell.row > 0)
            moveCursor(table.rows.at(cell.row - 1).value(table.columns - 1, table.rows.at(cell.row - 1).last()).end, false);
        return true;
    }
    case Qt::Key_Return:
    case Qt::Key_Enter: {
        if (cell.row + 1 < table.rows.size()) {
            const QList<Range> &below = table.rows.at(cell.row + 1);
            if (!below.isEmpty())
                moveCursor(below.value(cell.column, below.last()).end, false);
        } else {
            insertTableRowAfter(cell, cell.column);
        }
        return true;
    }
    case Qt::Key_Home:
        moveCursor(cell.range.start, event->modifiers().testFlag(Qt::ShiftModifier));
        return true;
    case Qt::Key_End:
        moveCursor(cell.range.end, event->modifiers().testFlag(Qt::ShiftModifier));
        return true;
    case Qt::Key_Backspace:
        return d->m_cursor == cell.range.start; // swallowed: the pipe before it stays
    case Qt::Key_Delete:
        return d->m_cursor == cell.range.end;
    default:
        return false;
    }
}

void MarkdownEditor::rewriteTable(qsizetype start, qsizetype end, const QString &text, bool reparseNow)
{
    d->m_lastEdit.invalidate(); // its own undo step, not the end of the typing before it
    pushUndo();
    d->m_formattingTables = true; // aligned already: leaving it has nothing to do
    applyEdit(start, end - start, text);
    d->m_formattingTables = false;
    d->m_lastEdit.invalidate();
    d->m_tableEdited = false;
    if (reparseNow)
        reparse(); // else the debounced parse applyEdit started catches up
}

bool MarkdownEditor::handleSourceTableKey(int key)
{
    if (!d->m_alignTables || d->m_mode == Mode::Reading || d->m_anchor != d->m_cursor)
        return false;
    // Prose is the common case, and a line without a pipe is not a row.
    const qsizetype caretLine = d->m_lines.lineAt(d->m_cursor);
    const qsizetype lineStart = d->m_lines.lineStart(caretLine);
    if (!QStringView(d->m_text).mid(lineStart, d->m_lines.lineEnd(caretLine) - lineStart).contains(QLatin1Char('|'))
        || tableRenderedAt(d->m_cursor)) // a drawn table's cells are handleTableKey's
        return false;
    // Inside the parse debounce the last parse's table still holds: every
    // edit since has shifted its range and cells (shiftRanges), and the cells
    // are read again from its lines below. Whole-note reparse, synchronous,
    // was 340 ms a Tab in a 1 MB note. ponytail: a table typed since that
    // parse is not in it yet, and that first Tab still reparses the note.
    qsizetype index = tableAround(d->m_cursor);
    if (index < 0 && d->m_parsedVersion != d->m_textVersion) {
        reparse();
        index = tableAround(d->m_cursor);
    }
    if (index < 0 || d->m_decorations.tables.at(index).html)
        return false;

    TableSpan table = d->m_decorations.tables.at(index);
    const qsizetype firstLine = d->m_lines.lineAt(table.range.start);
    const qsizetype lastLine = d->m_lines.lineAt(qMax(table.range.start, table.range.end - 1));
    const qsizetype start = d->m_lines.lineStart(firstLine);
    const qsizetype end = d->m_lines.lineEnd(lastLine);
    // The rows as the text has them now, by the parser's own rule
    // (Parser.cpp: line by line, the delimiter row skipped).
    table.rows.clear();
    for (qsizetype line = firstLine; line <= lastLine; ++line) {
        if (line != firstLine + 1)
            table.rows << tableRowCells(d->m_text, d->m_lines.lineStart(line), d->m_lines.lineEnd(line));
    }
    if (table.rows.isEmpty())
        return false;
    // A header that no longer has the table's columns (a `|` typed into it)
    // may be no table at all: that one is the parser's to say.
    if (d->m_parsedVersion != d->m_textVersion && table.rows.first().size() != table.columns) {
        reparse();
        return handleSourceTableKey(key);
    }
    const int lineIndex = int(d->m_lines.lineAt(d->m_cursor) - firstLine); // 0 header, 1 delimiter
    const int columns = table.columns;

    // Which row and cell the caret is in, then where the key sends it.
    int row = lineIndex <= 1 ? 0 : lineIndex - 1;
    int column = 0;
    for (int c = 0; c < table.rows.at(row).size() && c < columns; ++c) {
        if (table.rows.at(row).at(c).start <= d->m_cursor)
            column = c;
    }
    int toRow = row;
    int toColumn = column;
    if (lineIndex == 1) { // the delimiter row: on to the body, or back to the header
        toRow = key == Qt::Key_Backtab ? 0 : 1;
        toColumn = key == Qt::Key_Backtab ? columns - 1 : 0;
    } else if (key == Qt::Key_Tab) {
        if (column + 1 < columns) {
            ++toColumn;
        } else {
            ++toRow;
            toColumn = 0;
        }
    } else if (key == Qt::Key_Backtab) {
        if (column > 0) {
            --toColumn;
        } else if (row > 0) {
            --toRow;
            toColumn = columns - 1;
        }
    } else {
        ++toRow; // Enter: the cell below
    }

    // Past the last row there is none: a new one is written with the table.
    const QString source = d->m_text.mid(start, end - start);
    QString text = source;
    if (toRow >= table.rows.size()) {
        qsizetype lead = 0;
        const QString lastText = d->m_text.mid(d->m_lines.lineStart(lastLine), end - d->m_lines.lineStart(lastLine));
        while (lead < lastText.size() && (lastText.at(lead) == QLatin1Char('>') || lastText.at(lead).isSpace()))
            ++lead;
        text += QLatin1Char('\n') + lastText.left(lead) + QLatin1Char('|');
        for (int c = 0; c < columns; ++c)
            text += QStringLiteral("  |");
    }
    text = formatTable(text);
    // Source mode draws no table, so the parse can wait for the debounce. In
    // live preview this is the moment a raw table starts being drawn.
    if (text != source)
        rewriteTable(start, end, text, d->m_mode != Mode::Source);

    // Where the cell is now, from the rewritten lines: row r is line r, or
    // r + 1 past the header.
    const qsizetype line = firstLine + (toRow == 0 ? 0 : toRow + 1);
    if (line < d->m_lines.lineCount()) {
        const Range cell = tableRowCells(d->m_text, d->m_lines.lineStart(line), d->m_lines.lineEnd(line)).value(toColumn);
        if (cell.isValid())
            moveCursor(cell.start, false);
    }
    return true;
}

bool MarkdownEditor::loadTableText(qsizetype index, TableText &table) const
{
    if (index < 0 || index >= d->m_decorations.tables.size() || d->m_decorations.tables.at(index).html)
        return false;
    const Range &range = d->m_decorations.tables.at(index).range;
    table.index = index;
    table.start = d->m_lines.lineStart(d->m_lines.lineAt(range.start));
    table.end = d->m_lines.lineEnd(d->m_lines.lineAt(qMax(range.start, range.end - 1)));

    // The table as rows of cell text; line 1 is the delimiter row, so the
    // parser's row r is line r, or r + 1 past the header.
    const QStringList lines = d->m_text.mid(table.start, table.end - table.start).split(QLatin1Char('\n'));
    table.rows.clear();
    table.prefix.clear();
    for (const QString &line : lines) {
        qsizetype lead = 0;
        while (lead < line.size() && (line.at(lead) == QLatin1Char('>') || line.at(lead).isSpace()))
            ++lead;
        if (table.rows.isEmpty())
            table.prefix = line.left(lead);
        QStringList cells;
        for (const Range &text : tableRowCells(line, 0, line.size()))
            cells << line.mid(text.start, text.length());
        table.rows << cells;
    }
    table.columns = 0;
    for (const QStringList &row : std::as_const(table.rows))
        table.columns = qMax(table.columns, int(row.size()));
    for (QStringList &row : table.rows) {
        while (row.size() < table.columns)
            row << QString();
    }
    return table.rows.size() >= 2;
}

bool MarkdownEditor::storeTableText(const TableText &table)
{
    QStringList rebuilt;
    for (const QStringList &row : table.rows)
        rebuilt << table.prefix + QStringLiteral("| ") + row.join(QStringLiteral(" | ")) + QStringLiteral(" |");
    const QString text = formatTable(rebuilt.join(QLatin1Char('\n')));
    if (text == d->m_text.mid(table.start, table.end - table.start))
        return false; // already so: no step on the undo stack for nothing
    rewriteTable(table.start, table.end, text);
    return true;
}

bool MarkdownEditor::runTableCommand(const QString &id)
{
    const CellRef cell = tableCellAt(d->m_cursor);
    if (cell.row < 0)
        return false;
    const qsizetype index = tableAround(d->m_cursor);
    if (d->m_decorations.tables.at(index).html)
        return runHTMLtableCommand(id, index, cell.row, cell.column);
    TableText table;
    if (!loadTableText(index, table))
        return false;
    const qsizetype start = table.start;
    QList<QStringList> &rows = table.rows;
    const int columns = table.columns;

    int line = cell.row == 0 ? 0 : cell.row + 1;
    int column = cell.column;
    const bool header = line == 0;
    const bool last = line == rows.size() - 1;
    const auto emptyRow = [columns] { return QStringList(columns, QString()); };
    if (id.endsWith(QLatin1String("insert-row-before"))) {
        line = header ? 2 : line; // nothing goes above the header
        rows.insert(line, emptyRow());
    } else if (id.endsWith(QLatin1String("insert-row-after"))) {
        line = header ? 2 : line + 1;
        rows.insert(line, emptyRow());
    } else if (id.endsWith(QLatin1String("delete-row"))) {
        if (header)
            return false;
        rows.removeAt(line);
        line = qMin(line, int(rows.size()) - 1);
        if (line == 1)
            line = 0; // the last body row went: back to the header
    } else if (id.endsWith(QLatin1String("move-row-up"))) {
        if (header || line == 2)
            return false;
        rows.swapItemsAt(line, line - 1);
        --line;
    } else if (id.endsWith(QLatin1String("move-row-down"))) {
        if (header || last)
            return false;
        rows.swapItemsAt(line, line + 1);
        ++line;
    } else if (id.endsWith(QLatin1String("insert-column-before")) || id.endsWith(QLatin1String("insert-column-after"))) {
        column += id.endsWith(QLatin1String("after")) ? 1 : 0;
        for (int r = 0; r < rows.size(); ++r)
            rows[r].insert(column, r == 1 ? QStringLiteral("---") : QString());
    } else if (id.endsWith(QLatin1String("delete-column"))) {
        if (columns <= 1)
            return false;
        for (QStringList &row : rows)
            row.removeAt(column);
        column = qMin(column, columns - 2);
    } else if (id.endsWith(QLatin1String("move-column-left")) || id.endsWith(QLatin1String("move-column-right"))) {
        const int other = column + (id.endsWith(QLatin1String("left")) ? -1 : 1);
        if (other < 0 || other >= columns)
            return false;
        for (QStringList &row : rows)
            row.swapItemsAt(column, other);
        column = other;
    } else if (id.contains(QLatin1String("table-align-column-"))) {
        // The delimiter row's cell is the alignment; formatTable redraws it.
        rows[1][column] = id.endsWith(QLatin1String("left")) ? QStringLiteral(":--")
                          : id.endsWith(QLatin1String("center")) ? QStringLiteral(":-:")
                                                                  : QStringLiteral("--:");
    } else if (id.endsWith(QLatin1String("duplicate-row"))) {
        if (header)
            return false; // the header is the table's only one
        rows.insert(line + 1, rows.at(line));
        ++line;
    } else if (id.endsWith(QLatin1String("duplicate-column"))) {
        for (QStringList &row : rows)
            row.insert(column + 1, row.at(column)); // the delimiter row's cell too: the alignment goes with it
        ++column;
    } else if (id.contains(QLatin1String("table-sort-column-"))) {
        // Numbers by value ("2" before "10"), the rest by locale; the header and
        // the delimiter row stay where they are, equal cells keep their order.
        const bool descending = id.endsWith(QLatin1String("descending"));
        QCollator collator;
        collator.setNumericMode(true);
        collator.setCaseSensitivity(Qt::CaseInsensitive);
        std::stable_sort(rows.begin() + 2, rows.end(), [&](const QStringList &l, const QStringList &r) {
            const int order = collator.compare(l.at(column).trimmed(), r.at(column).trimmed());
            return descending ? order > 0 : order < 0;
        });
    } else {
        return false;
    }

    if (!storeTableText(table))
        return true;
    const QList<QList<Range>> &after = d->m_decorations.tables.value(index).rows;
    const int row = line == 0 ? 0 : line - 1;
    moveCursor(after.value(row).value(column, Range{start, start}).start, false);
    return true;
}

void MarkdownEditor::addTableActions(QMenu &menu, qsizetype offset)
{
    QMenu *align = nullptr;
    QMenu *sort = nullptr;
    for (const Command &command : commands()) {
        if (!command.id.startsWith(QLatin1String("noteAppC:table-")))
            continue;
        QString name = command.name.section(QStringLiteral(": "), 1);
        // The alignments and the two sort orders are a submenu each: "Align
        // column left" is "Left" under "Align column".
        QMenu *target = &menu;
        QMenu **slot = command.id.contains(QLatin1String("table-align-")) ? &align
                       : command.id.contains(QLatin1String("table-sort-")) ? &sort : nullptr;
        if (slot) {
            const QString title = name.section(QLatin1Char(' '), 0, 1);
            if (!*slot)
                *slot = menu.addMenu(title);
            name = name.mid(title.size() + 1);
            name[0] = name.at(0).toUpper();
            target = *slot;
        }
        connect(target->addAction(name), &QAction::triggered, this, [this, id = command.id, offset] {
            setCursorOffset(offset);
            runTableCommand(id);
        });
    }
}

} // namespace noteAppC
