/// What Obsidian puts around a rendered table, painted rather than built from
/// widgets: a grip at the left of the hovered row and over the hovered column
/// (drag to reorder, click to select it), a "+" bar at the bottom and right
/// edges (click to append a row or a column), and a rectangle of selected
/// cells that copies, cuts, pastes and clears as a table.
///
/// Hover is a mouse-move path like HoverEdit.cpp's: a binary search over the
/// sorted blocks and cells, a repaint of the few small rects that changed,
/// nothing laid out. Every edit goes through the source - loadTableText,
/// storeTableText, formatTable - so it is one undo step and the table is
/// aligned afterwards. With `editor.tableHandles` off none of it runs.

#include "MarkdownEditor.h"

#include "Parser.h"
#include "TableFormat.h"

#include <QApplication>
#include <QClipboard>
#include <QKeyEvent>
#include <QMimeData>
#include <QPainter>
#include <QRegularExpression>
#include <QScrollBar>
#include <QScroller>
#include <QTextDocumentFragment>

#include <algorithm>

namespace noteAppC {

namespace {

constexpr qreal kGripLength = 28;
constexpr qreal kGripThick = 6;
constexpr qreal kBarThick = 6;
constexpr qreal kButtonClear = 22; // the `</>` button's height: the "+" column starts below it
constexpr int kRowGrip = 1;
constexpr int kColumnGrip = 2;
constexpr int kBars = 4;
constexpr int kEverything = kRowGrip | kColumnGrip | kBars;

/// The parts of the chrome a pointer part draws differently.
int maskOf(MarkdownEditor::TablePart part)
{
    switch (part) {
    case MarkdownEditor::TablePart::RowGrip:
        return kRowGrip;
    case MarkdownEditor::TablePart::ColumnGrip:
        return kColumnGrip;
    case MarkdownEditor::TablePart::AddRow:
    case MarkdownEditor::TablePart::AddColumn:
        return kBars;
    case MarkdownEditor::TablePart::None:
        break;
    }
    return 0;
}

} // namespace

void MarkdownEditor::setTableHandles(bool on)
{
    if (m_tableHandles == on)
        return;
    m_tableHandles = on;
    if (!on) {
        clearTableHover();
        m_tableDrag = TableDrag{};
        m_cellSel = CellSelection{};
        m_cellPress = -1;
        stopTableAutoScroll();
        viewport()->update();
    }
}

const TableSpan *MarkdownEditor::chromeTable(const BlockView &view) const
{
    if (!m_tableHandles || m_mode != Mode::LivePreview || !view.laidOut || view.tabHidden
        || view.piece != 0 || view.isBase || view.cells.empty())
        return nullptr;
    const QList<TableSpan> &tables = m_decorations.tables;
    const auto it = std::lower_bound(tables.cbegin(), tables.cend(), view.span.range.start,
                                     [](const TableSpan &table, qsizetype start) { return table.range.start < start; });
    if (it == tables.cend() || it->range.start != view.span.range.start || (it->html && !it->simple) || it->columns <= 0
        || size_t(it->rows.size()) * size_t(it->columns) != view.cells.size())
        return nullptr;
    return &*it;
}

QRectF MarkdownEditor::tableViewRect(const BlockView &view) const
{
    return QRectF(contentLeft(), view.y - verticalScrollBar()->value(),
                  qMin<qreal>(view.tableWidth, contentWidth()), view.height);
}

QRectF MarkdownEditor::cellViewRect(const BlockView &view, const Cell &cell) const
{
    return cell.rect.translated(contentLeft() - view.tableScroll, view.y - verticalScrollBar()->value());
}

int MarkdownEditor::cellRowAt(const BlockView &view, const TableSpan &table, qreal y) const
{
    const qreal local = y + verticalScrollBar()->value() - view.y;
    int low = 0;
    int high = int(table.rows.size()) - 1;
    while (low < high) { // the last row whose top is not below `local`
        const int mid = (low + high + 1) / 2;
        if (view.cells[size_t(mid) * size_t(table.columns)].rect.top() <= local)
            low = mid;
        else
            high = mid - 1;
    }
    return low;
}

int MarkdownEditor::cellColumnAt(const BlockView &view, const TableSpan &table, qreal x) const
{
    const qreal local = x - contentLeft() + view.tableScroll;
    int low = 0;
    int high = table.columns - 1;
    while (low < high) {
        const int mid = (low + high + 1) / 2;
        if (view.cells[size_t(mid)].rect.left() <= local)
            low = mid;
        else
            high = mid - 1;
    }
    return low;
}

QRectF MarkdownEditor::rowGripRect(const BlockView &view, const TableSpan &table, int row, bool hit) const
{
    const QRectF cell = cellViewRect(view, view.cells[size_t(row) * size_t(table.columns)]);
    const qreal length = qBound<qreal>(12, cell.height() - 8, kGripLength);
    const QRectF grip(contentLeft() - kGripThick / 2, cell.top() + (cell.height() - length) / 2, kGripThick, length);
    return hit ? grip.adjusted(-3, -2, 1, 2) : grip; // not over the cell's own padding
}

QRectF MarkdownEditor::columnGripRect(const BlockView &view, const TableSpan &table, int column, bool hit) const
{
    Q_UNUSED(table)
    const QRectF cell = cellViewRect(view, view.cells[size_t(column)]);
    const qreal middle = cell.center().x();
    // A column scrolled out of the table's window has no grip.
    if (middle < contentLeft() || middle > contentLeft() + qMin<qreal>(view.tableWidth, contentWidth()))
        return {};
    const qreal length = qBound<qreal>(12, cell.width() - 8, kGripLength);
    const QRectF grip(middle - length / 2, cell.top() - kGripThick / 2, length, kGripThick);
    return hit ? grip.adjusted(-2, -3, 2, 1) : grip;
}

QRectF MarkdownEditor::addRowRect(const BlockView &view) const
{
    const QRectF table = tableViewRect(view);
    // Below a sideways scroll bar when there is one.
    return QRectF(table.left(), table.bottom() + (view.tableWidth > contentWidth() ? 7 : 1), table.width(), kBarThick);
}

QRectF MarkdownEditor::addColumnRect(const BlockView &view) const
{
    const QRectF table = tableViewRect(view);
    const qreal top = table.top() + kButtonClear;
    return QRectF(table.right() + 1, top, kBarThick, qMax<qreal>(0, table.bottom() - top));
}

QRegion MarkdownEditor::chromeDirty(const TableChrome &chrome, int mask) const
{
    if (chrome.block < 0 || chrome.block >= qsizetype(m_blocks.size()))
        return {};
    const BlockView &view = m_blocks[size_t(chrome.block)];
    const TableSpan *table = chromeTable(view);
    if (!table)
        return {};
    QRegion dirty;
    // Within the block's 8 px panel, which the `</>` hover repaints anyway.
    const auto add = [&dirty](const QRectF &rect, int pad) {
        if (!rect.isEmpty())
            dirty += rect.adjusted(-pad, -pad, pad, pad).toAlignedRect();
    };
    if ((mask & kRowGrip) && chrome.row >= 0 && chrome.row < table->rows.size())
        add(rowGripRect(view, *table, chrome.row, false), 2);
    if ((mask & kColumnGrip) && chrome.column >= 0 && chrome.column < table->columns)
        add(columnGripRect(view, *table, chrome.column, false), 2);
    if (mask & kBars) {
        add(addRowRect(view), 1);
        add(addColumnRect(view), 1);
    }
    return dirty;
}

MarkdownEditor::TableChrome MarkdownEditor::tableChromeAt(const QPoint &point) const
{
    if (!m_tableHandles || m_mode != Mode::LivePreview)
        return {};
    const qreal y = point.y() + verticalScrollBar()->value();
    // The grips straddle the table's top edge and the bars sit below it, so
    // a block owns the points a little above it - and the block above may be
    // the one whose bar the pointer is on, when the next starts right under it.
    const auto it = std::partition_point(m_blocks.cbegin(), m_blocks.cend(),
                                         [y](const BlockView &view) { return view.y - 16 <= y; });
    for (int back = 1; back <= 2 && it - m_blocks.cbegin() >= back; ++back) {
        const BlockView &view = *(it - back);
        const TableSpan *table = chromeTable(view);
        if (!table || !tableViewRect(view).adjusted(-10, -10, 16, 18).contains(point))
            continue;

        TableChrome chrome;
        chrome.block = qsizetype(it - m_blocks.cbegin()) - back;
        chrome.row = cellRowAt(view, *table, point.y());
        chrome.column = cellColumnAt(view, *table, point.x());
        // The `</>` button is the one thing a press there means.
        if (m_hoverEditButton && hasEditButton(view) && editButtonRectOf(chrome.block).contains(point))
            return chrome;
        // A finger is bigger than a pointer (touchable: no growth for a mouse).
        const auto hit = [this, &point](const QRectF &rect) { return !rect.isEmpty() && touchable(rect, 0).contains(point); };
        if (hit(rowGripRect(view, *table, chrome.row, true)))
            chrome.part = TablePart::RowGrip;
        else if (hit(columnGripRect(view, *table, chrome.column, true)))
            chrome.part = TablePart::ColumnGrip;
        else if (hit(addRowRect(view)))
            chrome.part = TablePart::AddRow;
        else if (hit(addColumnRect(view)))
            chrome.part = TablePart::AddColumn;
        return chrome;
    }
    return {};
}

void MarkdownEditor::updateTableHover(const QPoint &point)
{
    const TableChrome now = tableChromeAt(point);
    if (now == m_tableHover)
        return;
    const TableChrome before = m_tableHover;
    // Another table: everything of both. The same table: what moved or
    // lit up, and only that.
    int mask = kEverything;
    if (before.block == now.block) {
        mask = maskOf(before.part) | maskOf(now.part);
        if (before.row != now.row)
            mask |= kRowGrip;
        if (before.column != now.column)
            mask |= kColumnGrip;
    }
    m_tableHover = now;
    viewport()->update(chromeDirty(before, mask) + chromeDirty(now, mask));
}

void MarkdownEditor::clearTableHover()
{
    if (m_tableHover.block < 0)
        return;
    const TableChrome before = m_tableHover;
    m_tableHover = TableChrome{};
    viewport()->update(chromeDirty(before, kEverything));
}

void MarkdownEditor::paintTableChrome(QPainter &painter) const
{
    const bool dragging = m_tableDrag.part != TablePart::None;
    const qsizetype index = dragging ? m_tableDrag.block : m_tableHover.block;
    if (index < 0 || index >= qsizetype(m_blocks.size()))
        return;
    const BlockView &view = m_blocks[size_t(index)];
    const TableSpan *table = chromeTable(view);
    if (!table)
        return;
    const QColor accent = m_theme.color(QStringLiteral("--interactive-accent"));
    QColor quiet = m_theme.color(QStringLiteral("--text-faint"));
    quiet.setAlphaF(0.7f);
    const qreal radius = kGripThick / 2;

    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    const auto grip = [&](const QRectF &rect, bool active) {
        if (rect.isEmpty())
            return;
        painter.setBrush(active ? accent : quiet);
        painter.drawRoundedRect(rect, radius, radius);
    };
    const bool rowActive = m_tableHover.part == TablePart::RowGrip || m_tableDrag.part == TablePart::RowGrip;
    const bool columnActive = m_tableHover.part == TablePart::ColumnGrip || m_tableDrag.part == TablePart::ColumnGrip;
    const int row = dragging && m_tableDrag.part == TablePart::RowGrip ? m_tableDrag.from : m_tableHover.row;
    const int column = dragging && m_tableDrag.part == TablePart::ColumnGrip ? m_tableDrag.from : m_tableHover.column;
    if (row >= 0 && row < table->rows.size())
        grip(rowGripRect(view, *table, row, false), rowActive);
    if (column >= 0 && column < table->columns)
        grip(columnGripRect(view, *table, column, false), columnActive);

    // The add bars: a faint strip with a "+", brighter under the pointer.
    const auto bar = [&](const QRectF &rect, bool active) {
        if (rect.isEmpty())
            return;
        QColor fill = m_theme.color(QStringLiteral("--background-modifier-hover"));
        fill.setAlphaF(active ? 1.0f : 0.55f);
        painter.setBrush(fill);
        painter.drawRoundedRect(rect, 2, 2);
        painter.setPen(QPen(active ? accent : quiet, 1.4));
        const QPointF middle = rect.center();
        const qreal arm = 2;
        painter.drawLine(QPointF(middle.x() - arm, middle.y()), QPointF(middle.x() + arm, middle.y()));
        painter.drawLine(QPointF(middle.x(), middle.y() - arm), QPointF(middle.x(), middle.y() + arm));
        painter.setPen(Qt::NoPen);
    };
    bar(addRowRect(view), m_tableHover.part == TablePart::AddRow || m_tableDrag.part == TablePart::AddRow);
    bar(addColumnRect(view), m_tableHover.part == TablePart::AddColumn || m_tableDrag.part == TablePart::AddColumn);

    // The rows or columns a dragged add bar would add: a tinted strip, one
    // cell to each.
    if (dragging && m_tableDrag.moved && m_tableDrag.to > 0
        && (m_tableDrag.part == TablePart::AddRow || m_tableDrag.part == TablePart::AddColumn)) {
        const QRectF ghost = addBarGhost(view, *table, m_tableDrag.to);
        QColor tint = accent;
        tint.setAlphaF(0.16f);
        painter.setBrush(tint);
        painter.drawRect(ghost);
        QColor line = accent;
        line.setAlphaF(0.55f);
        painter.setPen(QPen(line, 1));
        painter.setBrush(Qt::NoBrush);
        const bool rows = m_tableDrag.part == TablePart::AddRow;
        for (int i = 0; i <= m_tableDrag.to; ++i) {
            const qreal step = (rows ? ghost.height() : ghost.width()) * i / m_tableDrag.to;
            painter.drawLine(rows ? QPointF(ghost.left(), ghost.top() + step) : QPointF(ghost.left() + step, ghost.top()),
                             rows ? QPointF(ghost.right(), ghost.top() + step) : QPointF(ghost.left() + step, ghost.bottom()));
        }
        painter.setPen(Qt::NoPen);
        painter.setBrush(accent);
    }

    // Where a dragged row or column would land.
    if (dragging && m_tableDrag.moved) {
        const QRectF area = tableViewRect(view);
        painter.setBrush(accent);
        const int rows = int(table->rows.size());
        if (m_tableDrag.part == TablePart::RowGrip) {
            const qreal y = m_tableDrag.to >= rows
                                ? area.bottom()
                                : cellViewRect(view, view.cells[size_t(m_tableDrag.to) * size_t(table->columns)]).top();
            painter.drawRect(QRectF(area.left(), y - 1, area.width(), 2));
        } else {
            const qreal x = m_tableDrag.to >= table->columns
                                ? cellViewRect(view, view.cells[size_t(table->columns) - 1]).right()
                                : cellViewRect(view, view.cells[size_t(m_tableDrag.to)]).left();
            painter.drawRect(QRectF(x - 1, area.top(), 2, area.height()));
        }
    }
    painter.restore();
}

// --- the grips and the add bars ---------------------------------------------

bool MarkdownEditor::pressTableChrome(const QPoint &point, bool touch)
{
    const TableChrome chrome = tableChromeAt(point);
    // A finger has no hover: its first tap in a table brings the grips and
    // bars up (for the row and column it is on), and only then are they its.
    if (chrome.part == TablePart::None || (touch && chrome.block != m_tableHover.block)) {
        if (touch)
            updateTableHover(point);
        return false;
    }
    const BlockView &view = m_blocks[size_t(chrome.block)];
    const TableSpan *table = chromeTable(view);
    if (!table)
        return false;
    m_interacted = true;
    // The finger drags the grip, not the page.
    if (touch && QScroller::hasScroller(viewport()))
        QScroller::scroller(viewport())->stop();
    if (chrome.part == TablePart::AddRow || chrome.part == TablePart::AddColumn) {
        // A click adds one, a drag adds one for each row height or column
        // width it travels; the table changes on release.
        const bool column = chrome.part == TablePart::AddColumn;
        const qreal origin = column ? point.x() - contentLeft() + view.tableScroll
                                    : point.y() + verticalScrollBar()->value() - view.y;
        m_tableDrag = {chrome.part, chrome.block, -1, 0, false, point, origin};
        return true;
    }
    m_tableDrag = {chrome.part, chrome.block, chrome.part == TablePart::RowGrip ? chrome.row : chrome.column,
                   chrome.part == TablePart::RowGrip ? chrome.row : chrome.column, false, point};
    viewport()->setCursor(Qt::ClosedHandCursor);
    return true;
}

bool MarkdownEditor::dragTableChrome(const QPoint &point)
{
    if (m_tableDrag.part == TablePart::None)
        return false;
    if (m_tableDrag.block >= qsizetype(m_blocks.size()))
        return true;
    const BlockView &view = m_blocks[size_t(m_tableDrag.block)];
    const TableSpan *table = chromeTable(view);
    const bool column = m_tableDrag.part == TablePart::ColumnGrip;
    if (table && (m_tableDrag.part == TablePart::AddRow || m_tableDrag.part == TablePart::AddColumn)) {
        if (!m_tableDrag.moved && (point - m_tableDrag.start).manhattanLength() < 4)
            return true;
        const int count = addBarCount(view, *table, point);
        if (!m_tableDrag.moved || count != m_tableDrag.to) {
            // Both the strip it was and the one it is: only that is repainted.
            QRegion dirty;
            for (const int shown : {m_tableDrag.moved ? m_tableDrag.to : 0, count}) {
                if (shown > 0)
                    dirty += addBarGhost(view, *table, shown).adjusted(-2, -2, 2, 2).toAlignedRect();
            }
            m_tableDrag.moved = true;
            m_tableDrag.to = count;
            viewport()->update(dirty);
        }
        updateTableAutoScroll(point);
        return true;
    }
    // A header row stays where it is: its grip only selects. (An HTML table
    // may have several, or none.)
    const int firstRow = table && table->html ? table->headerRows : 1;
    if (!table || (!column && m_tableDrag.from < firstRow)
        || (!m_tableDrag.moved && (point - m_tableDrag.start).manhattanLength() < 4))
        return true;

    // The boundary nearest the pointer: before the first row or column whose
    // middle is past it, and no higher than the first body row.
    int to = column ? int(table->columns) : int(table->rows.size());
    const qreal local = column ? point.x() - contentLeft() + view.tableScroll
                               : point.y() + verticalScrollBar()->value() - view.y;
    for (int i = column ? 0 : firstRow; i < to; ++i) {
        const Cell &cell = view.cells[column ? size_t(i) : size_t(i) * size_t(table->columns)];
        if (local < (column ? cell.rect.center().x() : cell.rect.center().y())) {
            to = i;
            break;
        }
    }
    if (!m_tableDrag.moved || to != m_tableDrag.to) {
        m_tableDrag.moved = true;
        m_tableDrag.to = to;
        viewport()->update(tableViewRect(view).adjusted(-10, -10, 16, 16).toAlignedRect());
    }
    updateTableAutoScroll(point);
    return true;
}

int MarkdownEditor::addBarCount(const BlockView &view, const TableSpan &table, const QPoint &point) const
{
    const bool column = m_tableDrag.part == TablePart::AddColumn;
    const qreal local = column ? point.x() - contentLeft() + view.tableScroll
                               : point.y() + verticalScrollBar()->value() - view.y;
    const Cell &last = view.cells[column ? size_t(table.columns) - 1
                                         : (size_t(table.rows.size()) - 1) * size_t(table.columns)];
    const qreal unit = qMax<qreal>(8, column ? last.rect.width() : last.rect.height());
    return qBound(0, int((local - m_tableDrag.origin) / unit), 100);
}

QRectF MarkdownEditor::addBarGhost(const BlockView &view, const TableSpan &table, int count) const
{
    if (count <= 0)
        return {};
    const QRectF area = tableViewRect(view);
    if (m_tableDrag.part == TablePart::AddColumn) {
        const QRectF last = cellViewRect(view, view.cells[size_t(table.columns) - 1]);
        return QRectF(last.right(), area.top(), last.width() * count, area.height());
    }
    const QRectF last = cellViewRect(view, view.cells[(size_t(table.rows.size()) - 1) * size_t(table.columns)]);
    return QRectF(area.left(), last.bottom(), area.width(), last.height() * count);
}

// --- autoscroll ----------------------------------------------------------------

QPoint MarkdownEditor::tableScrollDelta(const QPoint &point, qsizetype &block) const
{
    block = m_tableDrag.part != TablePart::None ? m_tableDrag.block : m_cellPress >= 0 ? blockAt(m_cellPress) : -1;
    if (block < 0 || block >= qsizetype(m_blocks.size()))
        return {};
    const BlockView &view = m_blocks[size_t(block)];
    // Faster the farther past the edge, from a couple of pixels a tick.
    const auto step = [](qreal over) { return over == 0 ? 0 : (over < 0 ? -1 : 1) * qBound(2, 2 + int(qAbs(over) / 4), 40); };
    QPoint delta;
    // A wide table pans inside its own window, which is where its cells end.
    if (view.tableWidth > contentWidth()) {
        const qreal left = contentLeft();
        const qreal right = left + contentWidth();
        delta.setX(step(point.x() < left ? point.x() - left : point.x() > right ? point.x() - right : 0));
    }
    delta.setY(step(point.y() < 0 ? point.y() : point.y() > viewport()->height() ? point.y() - viewport()->height() : 0));
    return delta;
}

void MarkdownEditor::updateTableAutoScroll(const QPoint &point)
{
    qsizetype block = -1;
    m_tableScrollPoint = point;
    if (tableScrollDelta(point, block).isNull())
        stopTableAutoScroll();
    else if (!m_tableScrollTimer.isActive())
        m_tableScrollTimer.start();
}

void MarkdownEditor::stopTableAutoScroll()
{
    m_tableScrollTimer.stop();
}

void MarkdownEditor::tableAutoScrollTick()
{
    qsizetype block = -1;
    const QPoint delta = tableScrollDelta(m_tableScrollPoint, block);
    if (delta.isNull()) {
        stopTableAutoScroll();
        return;
    }
    BlockView &view = m_blocks[size_t(block)];
    bool scrolled = false;
    if (delta.x() != 0) {
        const qreal wanted = qBound<qreal>(0, view.tableScroll + delta.x(), view.tableWidth - contentWidth());
        if (!qFuzzyCompare(wanted + 1, view.tableScroll + 1)) {
            view.tableScroll = wanted;
            scrolled = true;
        }
    }
    if (delta.y() != 0) {
        QScrollBar *bar = verticalScrollBar();
        const int before = bar->value();
        bar->setValue(before + delta.y());
        scrolled = scrolled || bar->value() != before;
    }
    if (!scrolled)
        return;
    viewport()->update();
    // The cells moved under a pointer that did not: take them as it is.
    if (m_tableDrag.part != TablePart::None)
        dragTableChrome(m_tableScrollPoint);
    else if (m_cellPress >= 0)
        dragCellSelection(m_tableScrollPoint);
}

bool MarkdownEditor::releaseTableChrome(const QPoint &)
{
    if (m_tableDrag.part == TablePart::None)
        return false;
    const TableDrag drag = m_tableDrag;
    m_tableDrag = TableDrag{};
    stopTableAutoScroll();
    const bool bar = drag.part == TablePart::AddRow || drag.part == TablePart::AddColumn;
    viewport()->setCursor(bar ? (drag.part == TablePart::AddRow ? Qt::SizeVerCursor : Qt::SizeHorCursor)
                              : Qt::OpenHandCursor);
    if (drag.block >= qsizetype(m_blocks.size()))
        return true;
    const BlockView &view = m_blocks[size_t(drag.block)];
    const TableSpan *table = chromeTable(view);
    if (!table)
        return true;
    const qsizetype index = table - m_decorations.tables.constData();
    if (bar) {
        const int count = drag.moved ? drag.to : 1; // a click adds one
        if (count > 0)
            appendTableLine(index, drag.part == TablePart::AddColumn, count);
        viewport()->update();
        return true;
    }
    const bool column = drag.part == TablePart::ColumnGrip;
    const int rows = int(table->rows.size());
    const int columns = table->columns;
    if (!drag.moved || !moveTableLine(index, column, drag.from, drag.to)) {
        // A click, or a drop where it was picked up: the row or column is selected.
        if (column)
            setCellSelection(index, 0, drag.from, rows - 1, drag.from);
        else
            setCellSelection(index, drag.from, 0, drag.from, columns - 1);
    }
    viewport()->update();
    return true;
}

bool MarkdownEditor::moveTableLine(qsizetype index, bool column, int from, int to)
{
    const bool html = index >= 0 && index < m_decorations.tables.size() && m_decorations.tables.at(index).html;
    TableText text;
    if (!html && !loadTableText(index, text))
        return false;
    // Parser rows; an HTML table's header is as many rows as it says.
    const int count = html ? (column ? m_decorations.tables.at(index).columns : int(m_decorations.tables.at(index).rows.size()))
                      : column ? text.columns : int(text.rows.size()) - 1;
    const int first = column ? 0 : html ? m_decorations.tables.at(index).headerRows : 1;
    const int final = to > from ? to - 1 : to;
    if (from < first || final < first || from >= count || final >= count || final == from)
        return false;
    if (html) {
        if (!moveHTMLline(index, column, from, final))
            return false;
    } else if (column) {
        for (QStringList &row : text.rows)
            row.move(from, final); // the delimiter row's cell too: the alignment goes with it
    } else {
        text.rows.move(tableLine(from), tableLine(final));
    }
    if (!html && !storeTableText(text))
        return false;
    const TableSpan &after = m_decorations.tables.at(index);
    if (column)
        setCellSelection(index, 0, final, int(after.rows.size()) - 1, final);
    else
        setCellSelection(index, final, 0, final, after.columns - 1);
    return true;
}

bool MarkdownEditor::appendTableLine(qsizetype index, bool column, int count)
{
    const bool html = index >= 0 && index < m_decorations.tables.size() && m_decorations.tables.at(index).html;
    TableText text;
    if (!html && !loadTableText(index, text))
        return false;
    // The column index the new cells start at, for the caret below.
    const int before = html ? m_decorations.tables.at(index).columns : text.columns;
    if (html) {
        if (!appendHTMLline(index, column, count))
            return false;
    } else {
        for (int i = 0; i < count; ++i) {
            if (column) {
                for (int r = 0; r < text.rows.size(); ++r)
                    text.rows[r] << (r == 1 ? QStringLiteral("---") : QString());
            } else {
                text.rows << QStringList(text.columns, QString());
            }
        }
        if (!storeTableText(text))
            return false;
    }
    // The caret goes into the first cell of what was added.
    const TableSpan &after = m_decorations.tables.at(index);
    const Range cell = column ? after.rows.value(0).value(before)
                              : after.rows.value(int(after.rows.size()) - count).value(0);
    if (cell.isValid())
        moveCursor(cell.start, false);
    viewport()->update();
    return true;
}

// --- a rectangle of cells ----------------------------------------------------

const MarkdownEditor::CellSelection *MarkdownEditor::cellSelection() const
{
    if (m_cellSel.tableStart < 0 || m_cellSel.version != m_textVersion || m_cursor != m_cellSel.caret
        || m_anchor != m_cursor)
        return nullptr;
    return &m_cellSel;
}

QRect MarkdownEditor::cellSelectionRect() const
{
    const CellSelection *sel = cellSelection();
    if (!sel)
        return {};
    return QRect(qMin(sel->anchorColumn, sel->headColumn), qMin(sel->anchorRow, sel->headRow),
                 qAbs(sel->anchorColumn - sel->headColumn) + 1, qAbs(sel->anchorRow - sel->headRow) + 1);
}

void MarkdownEditor::setCellSelection(qsizetype index, int anchorRow, int anchorColumn, int headRow, int headColumn)
{
    if (index < 0 || index >= m_decorations.tables.size())
        return;
    // Leaving another table it edited aligns that one first, as any caret move does.
    if (m_tableEdited && m_alignTables && tableAround(m_cursor) != index) {
        m_tableEdited = false;
        formatTables(m_cursor);
    }
    const TableSpan &table = m_decorations.tables.at(index);
    const Range cell = table.rows.value(anchorRow).value(anchorColumn);
    if (!cell.isValid())
        return;
    // The caret goes to the anchor cell, which must not drag the page back to
    // it while a selection is being autoscrolled away from it.
    const int scrolled = verticalScrollBar()->value();
    moveCursor(cell.end, false);
    verticalScrollBar()->setValue(scrolled);
    if (anchorRow == headRow && anchorColumn == headColumn) {
        m_cellSel = CellSelection{}; // one cell is the caret
    } else {
        m_cellSel = {table.range.start, m_cursor, m_textVersion, anchorRow, anchorColumn, headRow, headColumn};
    }
    viewport()->update();
}

bool MarkdownEditor::dragCellSelection(const QPoint &point)
{
    const qsizetype block = blockAt(m_cellPress);
    if (block < 0 || block >= qsizetype(m_blocks.size()))
        return false;
    const BlockView &view = m_blocks[size_t(block)];
    const TableSpan *table = chromeTable(view);
    if (!table || table->range.start != m_cellPress)
        return false;
    const int row = cellRowAt(view, *table, point.y());
    const int column = cellColumnAt(view, *table, point.x());
    const CellSelection *sel = cellSelection();
    // Past the edge of a wide or tall table the pointer is still in it: that
    // is where it autoscrolls.
    QRectF reach = tableViewRect(view).adjusted(-2, -2, 2, 2);
    constexpr qreal kFar = 1e6;
    if (view.tableWidth > contentWidth()) {
        reach.setLeft(-kFar);
        reach.setRight(kFar);
    }
    if (reach.top() < 0)
        reach.setTop(-kFar);
    if (reach.bottom() > viewport()->height())
        reach.setBottom(kFar);
    // Out of the table, or back in the cell the press began in: a text
    // selection again, as a drag out of a table always was.
    if ((row == m_cellPressRow && column == m_cellPressColumn) || !reach.contains(point)) {
        if (sel) {
            m_cellSel = CellSelection{};
            m_anchor = m_pressOffset;
            viewport()->update();
        }
        stopTableAutoScroll();
        return false;
    }
    if (!sel || sel->headRow != row || sel->headColumn != column)
        setCellSelection(table - m_decorations.tables.constData(), m_cellPressRow, m_cellPressColumn, row, column);
    updateTableAutoScroll(point);
    return true;
}

bool MarkdownEditor::handleCellSelectionKey(QKeyEvent *event)
{
    const int key = event->key();
    if (key >= Qt::Key_Shift && key <= Qt::Key_Alt) // the modifier's own press ahead of the key
        return false;
    const bool arrow = key == Qt::Key_Left || key == Qt::Key_Right || key == Qt::Key_Up || key == Qt::Key_Down;
    const bool shift = event->modifiers().testFlag(Qt::ShiftModifier);
    const CellSelection *sel = cellSelection();
    if (!sel && !(shift && arrow && m_anchor == m_cursor))
        return false;
    if (event->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier))
        return false;

    const qsizetype index = tableAround(m_cursor);
    if (index < 0 || index >= m_decorations.tables.size()
        || (m_decorations.tables.at(index).html && !m_decorations.tables.at(index).simple))
        return false;
    const TableSpan &table = m_decorations.tables.at(index);
    int anchorRow = 0, anchorColumn = 0, headRow = 0, headColumn = 0;
    if (sel) {
        if (key == Qt::Key_Delete || key == Qt::Key_Backspace)
            return clearCellSelection();
        if (key == Qt::Key_Escape) {
            m_cellSel = CellSelection{};
            viewport()->update();
            return true;
        }
        if (!arrow || !shift) {
            m_cellSel = CellSelection{}; // an arrow without Shift moves the caret from the anchor cell
            viewport()->update();
            return false;
        }
        anchorRow = sel->anchorRow;
        anchorColumn = sel->anchorColumn;
        headRow = sel->headRow;
        headColumn = sel->headColumn;
    } else {
        // Shift+arrow in a cell: a text selection until the caret is at the
        // edge the arrow points over, and a rectangle from there.
        const CellRef cell = tableCellAt(m_cursor);
        if (!cell.valid() || cell.row < 0 || !tableRenderedAt(m_cursor))
            return false;
        anchorRow = headRow = cell.row;
        anchorColumn = headColumn = cell.column;
        if ((key == Qt::Key_Left && m_cursor != cell.range.start)
            || (key == Qt::Key_Right && m_cursor != cell.range.end))
            return false;
    }
    switch (key) {
    case Qt::Key_Left:
        --headColumn;
        break;
    case Qt::Key_Right:
        ++headColumn;
        break;
    case Qt::Key_Up:
        --headRow;
        break;
    default:
        ++headRow;
        break;
    }
    if (headRow < 0 || headRow >= table.rows.size() || headColumn < 0 || headColumn >= table.columns)
        return sel != nullptr; // the table's edge: nothing to extend to
    setCellSelection(index, anchorRow, anchorColumn, headRow, headColumn);
    return true;
}

namespace {

/// Cell text as a spreadsheet wants it: a `\|` is a pipe again.
QString plainCell(QString text)
{
    return text.replace(QStringLiteral("\\|"), QStringLiteral("|"));
}

} // namespace

bool MarkdownEditor::copyCellSelection(bool cut)
{
    const CellSelection *sel = cellSelection();
    const qsizetype index = sel ? tableAround(m_cursor) : -1;
    if (index < 0)
        return false;
    const TableSpan &table = m_decorations.tables.at(index);
    const QRect rect = cellSelectionRect();
    QStringList pipe;
    QStringList delimiter;
    QStringList tsv;
    for (int r = rect.top(); r <= rect.bottom(); ++r) {
        QStringList cells;
        QStringList plain;
        for (int c = rect.left(); c <= rect.right(); ++c) {
            const Range range = table.rows.value(r).value(c);
            QString text = range.isValid() ? m_text.mid(range.start, range.length()) : QString();
            if (table.html) {
                // What the cell shows: tags and entities gone, one line, a
                // pipe escaped in the table and not in the plain text.
                // ponytail: one QTextDocument per cell; a rectangle of thousands
                // would want the laid-out cell's DisplayMap text instead.
                const QString shown = QTextDocumentFragment::fromHtml(text).toPlainText().simplified();
                cells << QString(shown).replace(QLatin1Char('|'), QStringLiteral("\\|"));
                plain << shown;
                continue;
            }
            cells << text;
            plain << plainCell(text);
        }
        pipe << QStringLiteral("| ") + cells.join(QStringLiteral(" | ")) + QStringLiteral(" |");
        tsv << plain.join(QLatin1Char('\t'));
    }
    for (int c = rect.left(); c <= rect.right(); ++c) {
        const Qt::Alignment alignment = table.alignment.value(c, Qt::AlignLeft);
        delimiter << (alignment.testFlag(Qt::AlignHCenter) ? QStringLiteral(":-:")
                      : alignment.testFlag(Qt::AlignRight) ? QStringLiteral("--:") : QStringLiteral("---"));
    }
    // The first row is the new table's header, as a table pasted from
    // anywhere else needs one.
    pipe.insert(1, QStringLiteral("| ") + delimiter.join(QStringLiteral(" | ")) + QStringLiteral(" |"));
    const QString markdown = formatTable(pipe.join(QLatin1Char('\n')));

    // Plain text is what a spreadsheet takes: tab-separated. The pipe table is
    // the markdown flavour, which is what a paste into a table here reads first.
    auto *mime = new QMimeData;
    mime->setText(tsv.join(QLatin1Char('\n')));
    mime->setData(QStringLiteral("text/markdown"), markdown.toUtf8());
    QApplication::clipboard()->setMimeData(mime);
    if (cut)
        clearCellSelection();
    return true;
}

bool MarkdownEditor::clearCellSelection()
{
    const CellSelection *sel = cellSelection();
    const qsizetype index = sel ? tableAround(m_cursor) : -1;
    if (index >= 0 && m_decorations.tables.at(index).html) {
        // An HTML cell's content, between its tags, goes; the tags stay.
        const TableSpan &table = m_decorations.tables.at(index);
        const CellSelection keep = *sel;
        const QRect rect = cellSelectionRect();
        QList<HTMLtableEdit> edits;
        for (int r = rect.top(); r <= rect.bottom(); ++r) {
            for (int c = rect.left(); c <= rect.right(); ++c) {
                const Range cell = table.rows.value(r).value(c);
                if (cell.isValid() && cell.length() > 0)
                    edits.append({cell.start, cell.length(), QString()});
            }
        }
        if (applyHTMLtableEdits(edits, index, keep.anchorRow, keep.anchorColumn))
            setCellSelection(index, keep.anchorRow, keep.anchorColumn, keep.headRow, keep.headColumn);
        return true;
    }
    TableText text;
    if (index < 0 || !loadTableText(index, text))
        return false;
    const CellSelection keep = *sel;
    const QRect rect = cellSelectionRect();
    for (int r = rect.top(); r <= rect.bottom(); ++r) {
        for (int c = rect.left(); c <= rect.right(); ++c)
            text.rows[tableLine(r)][c] = QString();
    }
    if (storeTableText(text))
        setCellSelection(index, keep.anchorRow, keep.anchorColumn, keep.headRow, keep.headColumn);
    return true;
}

namespace {

/// What a clipboard holds as a grid of cells ready to go into a pipe table's
/// source: a pipe table (its delimiter row dropped), or tab-separated values
/// (a spreadsheet's), or else one cell a line; `structured` says it was one of
/// the first two.
QList<QStringList> cellGrid(const QString &text, bool &structured)
{
    QString body = text;
    body.remove(QLatin1Char('\r'));
    while (body.endsWith(QLatin1Char('\n')))
        body.chop(1);
    const QStringList lines = body.split(QLatin1Char('\n'));
    QList<QStringList> grid;
    structured = text.contains(QLatin1Char('\t'));
    static const QRegularExpression delimiter(QStringLiteral("^\\s*\\|?\\s*:?-+:?\\s*(\\|\\s*:?-+:?\\s*)*\\|?\\s*$"));
    if (lines.size() >= 2 && lines.first().contains(QLatin1Char('|')) && delimiter.match(lines.at(1)).hasMatch()) {
        for (int i = 0; i < lines.size(); ++i) {
            if (i == 1)
                continue;
            QStringList cells;
            for (const Range &cell : tableRowCells(lines.at(i), 0, lines.at(i).size()))
                cells << lines.at(i).mid(cell.start, cell.length());
            grid << cells;
        }
        structured = true;
        return grid;
    }
    for (const QString &line : lines) {
        QStringList cells = line.split(QLatin1Char('\t'));
        for (QString &cell : cells)
            cell = cell.trimmed().replace(QLatin1Char('|'), QStringLiteral("\\|"));
        grid << cells;
    }
    return grid;
}

} // namespace

bool MarkdownEditor::pasteIntoCells()
{
    if (!m_tableHandles || m_mode != Mode::LivePreview)
        return false;
    const QMimeData *mime = QApplication::clipboard()->mimeData();
    if (!mime)
        return false;
    const QString text = mime->hasFormat(QStringLiteral("text/markdown"))
                             ? QString::fromUtf8(mime->data(QStringLiteral("text/markdown")))
                             : mime->text();
    const CellSelection *sel = cellSelection();
    const CellRef cell = tableCellAt(m_cursor);
    if (text.isEmpty() || !cell.valid() || cell.row < 0 || !tableRenderedAt(m_cursor) || cell.table->html)
        return false;
    bool structured = false;
    QList<QStringList> cells = cellGrid(text, structured);
    if ((!sel && !structured) || cells.isEmpty())
        return false; // a word or a line: the ordinary paste puts it in the cell
    int columnsWide = 0;
    for (const QStringList &row : std::as_const(cells))
        columnsWide = qMax(columnsWide, int(row.size()));

    const QRect rect = sel ? cellSelectionRect() : QRect(cell.column, cell.row, 1, 1);
    const qsizetype index = tableAround(m_cursor);
    TableText table;
    if (index < 0 || !loadTableText(index, table))
        return false;
    // One value into several cells fills them all, as a spreadsheet does.
    const bool fill = cells.size() == 1 && columnsWide == 1 && (rect.width() > 1 || rect.height() > 1);
    const int rowsWide = fill ? rect.height() : int(cells.size());
    const int across = fill ? rect.width() : columnsWide;
    int bodyRows = int(table.rows.size()) - 1; // parser rows
    while (bodyRows < rect.top() + rowsWide) {
        table.rows << QStringList(table.columns, QString());
        ++bodyRows;
    }
    while (table.columns < rect.left() + across) {
        for (int r = 0; r < table.rows.size(); ++r)
            table.rows[r] << (r == 1 ? QStringLiteral("---") : QString());
        ++table.columns;
    }
    for (int r = 0; r < rowsWide; ++r) {
        for (int c = 0; c < across; ++c) {
            const QString &value = fill ? cells.first().first() : cells.at(r).value(c);
            table.rows[tableLine(rect.top() + r)][rect.left() + c] = value;
        }
    }
    if (storeTableText(table))
        setCellSelection(index, rect.top(), rect.left(), rect.top() + rowsWide - 1, rect.left() + across - 1);
    return true;
}

// --- the tests' view of the chrome -----------------------------------------

QRectF MarkdownEditor::tableCellRect(qsizetype offset, int row, int column) const
{
    const qsizetype block = blockAt(offset);
    if (block < 0 || block >= qsizetype(m_blocks.size()))
        return {};
    const BlockView &view = m_blocks[size_t(block)];
    const TableSpan *table = chromeTable(view);
    if (!table || row < 0 || row >= table->rows.size() || column < 0 || column >= table->columns)
        return {};
    return cellViewRect(view, view.cells[size_t(row) * size_t(table->columns) + size_t(column)]);
}

QRectF MarkdownEditor::tableChromeRect(qsizetype offset, TablePart part, int index) const
{
    const qsizetype block = blockAt(offset);
    if (block < 0 || block >= qsizetype(m_blocks.size()))
        return {};
    const BlockView &view = m_blocks[size_t(block)];
    const TableSpan *table = chromeTable(view);
    if (!table)
        return {};
    switch (part) {
    case TablePart::RowGrip:
        return index >= 0 && index < table->rows.size() ? rowGripRect(view, *table, index, false) : QRectF();
    case TablePart::ColumnGrip:
        return index >= 0 && index < table->columns ? columnGripRect(view, *table, index, false) : QRectF();
    case TablePart::AddRow:
        return addRowRect(view);
    case TablePart::AddColumn:
        return addColumnRect(view);
    case TablePart::None:
        break;
    }
    return {};
}

} // namespace noteAppC
