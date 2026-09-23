/// The editor under a finger: small targets grow while a touch press is
/// handled (touchable), and a word selected by a long press gets two handles
/// that drag either end of the selection, as a phone's text fields have.
/// A mouse never sees either: its presses leave m_touchSlop at zero and
/// clear the handles.

#include "MarkdownEditor.h"

#include <QPainter>
#include <QScroller>
#include <QScrollBar>

namespace noteAppC {

namespace {

constexpr qreal kHandleRadius = 7;

} // namespace

QRectF MarkdownEditor::touchable(const QRectF &rect, qreal mouseSlop) const
{
    const qreal slop = qMax(mouseSlop, m_touchSlop);
    return rect.adjusted(-slop, -slop, slop, slop);
}

QRectF MarkdownEditor::selectionHandleRect(bool end) const
{
    const Range range = selection();
    const QPointF point = pointFor(end ? range.end : range.start);
    // A drop hanging under the line, its point on the selection's edge.
    const qreal lineHeight = QFontMetricsF(font()).height() * 1.4;
    return QRectF(point.x() - kHandleRadius, point.y() + lineHeight, 2 * kHandleRadius, 2 * kHandleRadius);
}

void MarkdownEditor::paintSelectionHandles(QPainter &painter) const
{
    if (!m_touchHandles || selection().length() == 0 || m_mode == Mode::Reading)
        return;
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(m_theme.color(QStringLiteral("--interactive-accent")));
    for (const bool end : {false, true}) {
        const QRectF drop = selectionHandleRect(end);
        painter.drawEllipse(drop);
        painter.drawRect(QRectF(drop.center().x() - 1, drop.top() - 4, 2, 5)); // the stem up to the text
    }
    painter.restore();
}

bool MarkdownEditor::pressSelectionHandle(const QPoint &point)
{
    if (!m_touchHandles || selection().length() == 0)
        return false;
    const Range range = selection();
    for (const bool end : {true, false}) {
        if (!touchable(selectionHandleRect(end), 12).contains(point))
            continue;
        // The other end holds still; this one follows the finger.
        m_anchor = end ? range.start : range.end;
        m_cursor = end ? range.end : range.start;
        m_draggingHandle = end ? 1 : -1;
        // The finger drags the handle, not the page.
        if (QScroller::hasScroller(viewport()))
            QScroller::scroller(viewport())->stop();
        return true;
    }
    return false;
}

bool MarkdownEditor::dragSelectionHandle(const QPoint &point)
{
    if (m_draggingHandle == 0)
        return false;
    // The finger sits on the drop under the line; the text it points at is
    // a line above it.
    const qreal lineHeight = QFontMetricsF(font()).height() * 1.4;
    moveCursor(offsetAt(point - QPoint(0, int(lineHeight))), true);
    m_touchHandles = true;
    return true;
}

bool MarkdownEditor::releaseSelectionHandle()
{
    if (m_draggingHandle == 0)
        return false;
    m_draggingHandle = 0;
    viewport()->update();
    return true;
}

} // namespace noteAppC
