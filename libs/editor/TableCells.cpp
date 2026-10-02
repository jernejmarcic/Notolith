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

#include "Parser.h"
#include "TableFormat.h"

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
    if (m_mode == Mode::Source)
        return {};
    for (const TableSpan &table : m_decorations.tables) {
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
        return qMin<qsizetype>(m_text.size(), table.range.end + 1);
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
    return {cell.rect.left() - view.tableScroll + kCellPadX + qMax<qreal>(0, offset),
            cell.rect.top() + kCellPadY};
}

const MarkdownEditor::Cell *MarkdownEditor::caretCell(const BlockView &view) const
{
    if (view.cells.empty() || m_anchor != m_cursor)
        return nullptr;
    for (const Cell &cell : view.cells) {
        if (cell.source.start <= m_cursor && m_cursor <= cell.source.end)
            return &cell;
    }
    return nullptr;
}

qsizetype MarkdownEditor::tableAround(qsizetype offset) const
{
    for (qsizetype i = 0; i < m_decorations.tables.size(); ++i) {
        const Range &range = m_decorations.tables.at(i).range;
        if (m_lines.lineStart(m_lines.lineAt(range.start)) <= offset && offset <= range.end)
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
    for (qsizetype i = 0; i < m_decorations.tables.size(); ++i) {
        if ((only >= 0 && i != only) || m_decorations.tables.at(i).html)
            continue; // an HTML table's source is left as written
        const Range &range = m_decorations.tables.at(i).range;
        const qsizetype start = m_lines.lineStart(m_lines.lineAt(range.start));
        const qsizetype end = m_lines.lineEnd(m_lines.lineAt(qMax(range.start, range.end - 1)));
        const QString source = m_text.mid(start, end - start);
        const QString aligned = formatTable(source);
        if (aligned != source)
            edits.append({i, start, end, aligned});
    }
    if (edits.isEmpty())
        return false;

    // The caret stays in its cell, at the same place in its text; outside
    // every table it moves with the text around it.
    const CellRef cell = tableCellAt(m_cursor);
    qsizetype inCell = cell.row >= 0 ? m_cursor - cell.range.start : -1;
    qsizetype caret = m_cursor;
    for (const Edit &edit : std::as_const(edits)) {
        if (edit.end <= m_cursor && !(cell.row >= 0 && tableAround(m_cursor) == edit.table))
            caret += edit.text.size() - (edit.end - edit.start);
    }
    const qsizetype caretTable = cell.row >= 0 ? tableAround(m_cursor) : -1;
    const int row = cell.row;
    const int column = cell.column;

    m_lastEdit.invalidate(); // one step of its own, not the end of the typing before it
    pushUndo();
    m_formattingTables = true;
    for (qsizetype i = edits.size() - 1; i >= 0; --i)
        applyEdit(edits.at(i).start, edits.at(i).end - edits.at(i).start, edits.at(i).text);
    m_formattingTables = false;
    m_lastEdit.invalidate();
    reparse();

    if (caretTable >= 0 && caretTable < m_decorations.tables.size()) {
        const TableSpan &table = m_decorations.tables.at(caretTable);
        const Range moved = table.rows.value(row).value(column);
        if (moved.isValid())
            caret = moved.start + qMin(inCell, moved.length());
    }
    m_tableEdited = false;
    moveCursor(caret, false);
    return true;
}

bool MarkdownEditor::tableRenderedAt(qsizetype offset) const
{
    const qsizetype index = blockAt(offset);
    return index >= 0 && index < qsizetype(m_blocks.size()) && !m_blocks[size_t(index)].cells.empty();
}

void MarkdownEditor::revealCaretCell()
{
    const qsizetype index = blockAt(m_cursor);
    if (index < 0 || index >= qsizetype(m_blocks.size()))
        return;
    BlockView &view = m_blocks[size_t(index)];
    const Cell *cell = caretCell(view);
    if (!cell)
        return;
    const qreal overflow = view.tableWidth - contentWidth();
    if (overflow <= 0)
        return;
    qreal scroll = view.tableScroll;
    if (cell->rect.left() < scroll)
        scroll = cell->rect.left();
    else if (cell->rect.right() > scroll + contentWidth())
        scroll = cell->rect.right() - contentWidth();
    view.tableScroll = qBound<qreal>(0, scroll, overflow);
}

void MarkdownEditor::insertTableRowAfter(const CellRef &cell, int column)
{
    const TableSpan &table = *cell.table;
    if (table.html) {
        insertHTMLrow(&table - m_decorations.tables.constData(), cell.row, true, column);
        return;
    }
    const qsizetype lineEnd = m_lines.lineEnd(m_lines.lineAt(cell.range.start));
    // The new row copies the one it follows up to its first pipe, so a row
    // inside a callout keeps its "> ".
    const qsizetype lineStart = m_lines.lineStart(m_lines.lineAt(cell.range.start));
    qsizetype prefixEnd = lineStart;
    while (prefixEnd < lineEnd && (m_text.at(prefixEnd) == QLatin1Char('>') || m_text.at(prefixEnd).isSpace()))
        ++prefixEnd;
    QString row = QLatin1Char('\n') + m_text.mid(lineStart, prefixEnd - lineStart) + QLatin1Char('|');
    for (int c = 0; c < table.columns; ++c)
        row += QStringLiteral("  |");

    m_lastEdit.invalidate(); // its own undo step, not part of the typing before it
    pushUndo();
    applyEdit(lineEnd, 0, row);
    // The row has to be a row before the caret can go into it: parse now,
    // not in 20 ms, or the caret lands in no cell and is snapped away.
    reparse();
    const qsizetype newLine = m_lines.lineAt(lineEnd + 1);
    const QList<Range> cells = tableRowCells(m_text, m_lines.lineStart(newLine), m_lines.lineEnd(newLine));
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
    if (table < 0 || table >= m_decorations.tables.size())
        return false;
    const TableSpan &span = m_decorations.tables.at(table);
    if (span.html)
        return fillShortHTMLrow(table, row);
    const QList<Range> cells = span.rows.value(row);
    if (cells.isEmpty() || cells.size() >= span.columns)
        return false;
    const qsizetype lineEnd = m_lines.lineEnd(m_lines.lineAt(cells.last().start));
    // A row written without its closing pipe gets one before the new cells.
    qsizetype last = lineEnd;
    while (last > cells.last().end && m_text.at(last - 1).isSpace())
        --last;
    QString added = last > cells.last().end && m_text.at(last - 1) == QLatin1Char('|') ? QString() : QStringLiteral(" |");
    for (qsizetype c = cells.size(); c < span.columns; ++c)
        added += QStringLiteral("  |");

    const qsizetype caret = m_cursor;
    m_lastEdit.invalidate();
    pushUndo();
    applyEdit(last, lineEnd - last, added);
    m_lastEdit.invalidate();
    reparse();
    // The caret stays where it was; the click or the key that asked moves it.
    m_cursor = m_anchor = caret <= last ? caret : caret + added.size() - (lineEnd - last);
    return true;
}

bool MarkdownEditor::fillShortRowAt(const QPoint &viewportPoint)
{
    const qreal y = viewportPoint.y() + verticalScrollBar()->value();
    const qreal x = viewportPoint.x() - contentLeft();
    for (const BlockView &view : m_blocks) {
        if (view.cells.empty() || y < view.y || y > view.y + view.height || m_mode == Mode::Source)
            continue;
        const TableSpan *table = tableFor(view);
        if (!table)
            return false; // a base's cells, not a table's
        for (const Cell &cell : view.cells) {
            if (!cell.source.isValid() && cell.rect.translated(-view.tableScroll, view.y).contains(QPointF(x, y)))
                return fillShortRow(table - m_decorations.tables.constData(), cell.row);
        }
        return false;
    }
    return false;
}

bool MarkdownEditor::handleTableKey(QKeyEvent *event)
{
    if (m_anchor != m_cursor)
        return false;
    const CellRef cell = tableCellAt(m_cursor);
    if (!cell.valid() || cell.row < 0 || !tableRenderedAt(m_cursor))
        return false;
    const TableSpan &table = *cell.table;
    const QList<Range> &row = table.rows.at(cell.row);
    const int columns = int(qMin<qsizetype>(row.size(), table.columns));

    switch (event->key()) {
    case Qt::Key_Tab: {
        // The next cell is one this short row lacks: give the row its cells.
        if (cell.column + 1 < table.columns && cell.column + 1 >= columns) {
            const qsizetype index = &table - m_decorations.tables.constData();
            const int rowIndex = cell.row;
            const int next = cell.column + 1;
            if (fillShortRow(index, rowIndex)) {
                moveCursor(m_decorations.tables.at(index).rows.value(rowIndex).value(next).start, false);
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
        return m_cursor == cell.range.start; // swallowed: the pipe before it stays
    case Qt::Key_Delete:
        return m_cursor == cell.range.end;
    default:
        return false;
    }
}

bool MarkdownEditor::runTableCommand(const QString &id)
{
    const CellRef cell = tableCellAt(m_cursor);
    if (cell.row < 0)
        return false;
    const qsizetype index = tableAround(m_cursor);
    if (m_decorations.tables.at(index).html)
        return runHTMLtableCommand(id, index, cell.row, cell.column);
    const Range &range = m_decorations.tables.at(index).range;
    const qsizetype start = m_lines.lineStart(m_lines.lineAt(range.start));
    const qsizetype end = m_lines.lineEnd(m_lines.lineAt(qMax(range.start, range.end - 1)));

    // The table as rows of cell text; line 1 is the delimiter row, so the
    // parser's row r is line r, or r + 1 past the header.
    QStringList lines = m_text.mid(start, end - start).split(QLatin1Char('\n'));
    QString prefix;
    QList<QStringList> rows;
    for (const QString &line : std::as_const(lines)) {
        qsizetype lead = 0;
        while (lead < line.size() && (line.at(lead) == QLatin1Char('>') || line.at(lead).isSpace()))
            ++lead;
        if (rows.isEmpty())
            prefix = line.left(lead);
        QStringList cells;
        for (const Range &text : tableRowCells(line, 0, line.size()))
            cells << line.mid(text.start, text.length());
        rows << cells;
    }
    int columns = 0;
    for (const QStringList &row : std::as_const(rows))
        columns = qMax(columns, int(row.size()));
    for (QStringList &row : rows) {
        while (row.size() < columns)
            row << QString();
    }

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
    } else {
        return false;
    }

    QStringList rebuilt;
    for (const QStringList &row : std::as_const(rows))
        rebuilt << prefix + QStringLiteral("| ") + row.join(QStringLiteral(" | ")) + QStringLiteral(" |");
    const QString text = formatTable(rebuilt.join(QLatin1Char('\n')));

    m_lastEdit.invalidate();
    pushUndo();
    m_formattingTables = true; // aligned already: leaving it has nothing to do
    applyEdit(start, end - start, text);
    m_formattingTables = false;
    m_lastEdit.invalidate();
    reparse();
    const QList<QList<Range>> &after = m_decorations.tables.value(index).rows;
    const int row = line == 0 ? 0 : line - 1;
    moveCursor(after.value(row).value(column, Range{start, start}).start, false);
    return true;
}

void MarkdownEditor::addTableActions(QMenu &menu, qsizetype offset)
{
    for (const Command &command : commands()) {
        if (!command.id.startsWith(QLatin1String("noteAppC:table-")))
            continue;
        const QString name = command.name.section(QStringLiteral(": "), 1);
        connect(menu.addAction(name), &QAction::triggered, this, [this, id = command.id, offset] {
            setCursorOffset(offset);
            runTableCommand(id);
        });
    }
}

} // namespace noteAppC
