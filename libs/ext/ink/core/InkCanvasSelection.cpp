#include "InkCanvas.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include <QApplication>
#include <QClipboard>
#include <QMimeData>
#include <QPainter>

#include "InkTools.h"

// The select tool's half of InkCanvas: the selection state, the lasso, the move and resize drags, delete, copy and
// paste. Split out of InkCanvas.cpp as it was (Task 14); the state lives in the class (InkCanvas.h).

namespace noteAppC::ink {

namespace {

constexpr double kHandleSize = 6.0;      ///< widget px: the selection's handle squares
constexpr double kHandleReach = 6.0;     ///< widget px: how far from a handle's centre a press still grabs it
constexpr double kEdgeHandleMinBox = 24.0; ///< widget px: the four edge handles need a box at least this big both ways, or they would sit on the move area
constexpr double kMinScale = 0.02;       ///< a resize never squeezes the selection below this fraction

/// Handle `h` of `box`: 0..7 clockwise from the top-left (corners are even).
QPointF handlePoint(const QRectF &box, int h)
{
    const double x[] = {box.left(), box.center().x(), box.right(), box.right(), box.right(), box.center().x(), box.left(), box.left()};
    const double y[] = {box.top(), box.top(), box.top(), box.center().y(), box.bottom(), box.bottom(), box.bottom(), box.center().y()};
    return QPointF(x[h], y[h]);
}

bool finiteRect(const QRectF &r) { return std::isfinite(r.x()) && std::isfinite(r.y()) && std::isfinite(r.width()) && std::isfinite(r.height()); }

bool handleShown(const QRectF &box, int h) { return h % 2 == 0 || (box.width() >= kEdgeHandleMinBox && box.height() >= kEdgeHandleMinBox); }


} // namespace

// --- selection -----------------------------------------------------------------------

QRect InkCanvas::selectionPaintRect() const
{
    if (m_selection.empty() || m_selectionBounds.isNull())
        return QRect();
    return widgetRect(m_selectionBounds).adjusted(-kHandleSize, -kHandleSize, kHandleSize, kHandleSize);
}

QRect InkCanvas::lassoPaintRect() const
{
    QRectF box;
    for (const QPointF &p : m_lasso) {
        const QPointF w = toWidget(p);
        box |= QRectF(w, QSizeF(0.001, 0.001)); // a null rect adds nothing to a union, so a point is a hair of area
    }
    return box.toAlignedRect().adjusted(-3, -3, 3, 3);
}

void InkCanvas::recomputeSelectionBounds()
{
    m_selectionBounds = QRectF();
    for (qint64 t : m_selection)
        if (const Item *item = m_document.item(t))
            m_selectionBounds |= item->bounds();
}

void InkCanvas::setSelection(std::vector<qint64> items)
{
    if (items == m_selection)
        return;
    const QRect old = selectionPaintRect();
    m_selection = std::move(items);
    recomputeSelectionBounds();
    update(old | selectionPaintRect());
    emit selectionChanged();
}

void InkCanvas::clearSelection()
{
    if (isSelectGesture())
        finishStroke(false); // a drag in flight is cancelled first: its items go back where they were
    setSelection({});
}

int InkCanvas::handleAt(const QPointF &widgetPos) const
{
    if (m_selection.empty() || m_selectionBounds.isNull())
        return -1;
    const QRectF box = QRectF(toWidget(m_selectionBounds.topLeft()), toWidget(m_selectionBounds.bottomRight())).normalized();
    int best = -1;
    double bestDistance = kHandleReach * kHandleReach * 2 + 1;
    for (int h = 0; h < 8; ++h) {
        if (!handleShown(box, h))
            continue;
        const QPointF d = widgetPos - handlePoint(box, h);
        if (std::fabs(d.x()) > kHandleReach || std::fabs(d.y()) > kHandleReach)
            continue;
        const double distance = d.x() * d.x() + d.y() * d.y();
        if (distance < bestDistance) {
            best = h;
            bestDistance = distance;
        }
    }
    return best;
}

void InkCanvas::pressSelect(const QPointF &widgetPos)
{
    const QPointF at = toDocument(widgetPos);
    m_dragStart = at;
    m_dragMoved = false;
    m_dragTransform = QTransform();
    const int handle = handleAt(widgetPos);
    const double slack = 4.0 / m_zoom; // a thin stroke's box is a hair: grab a few px around it
    const bool inside = !m_selection.empty() && m_selectionBounds.adjusted(-slack, -slack, slack, slack).contains(at);
    if (handle >= 0 || inside) { // grabbing the selection: a resize at a handle, otherwise a move
        m_dragOriginals.clear();
        for (qint64 t : m_selection)
            if (const Item *item = m_document.item(t))
                m_dragOriginals.push_back(*item);
        m_dragBounds = m_selectionBounds;
        m_dragHandle = handle;
        m_drawing = handle >= 0 ? Drawing::Scale : Drawing::Move;
        return;
    }
    clearSelection(); // a new lasso replaces the old selection
    m_lasso = {at};
    m_drawing = Drawing::Select;
}

void InkCanvas::updateLasso(const QPointF &documentPos)
{
    const QRect old = lassoPaintRect();
    if (m_selectShape == SelectShape::Rect)
        m_lasso = {m_lasso.first(), documentPos}; // the start and the pointer
    else if (m_lasso.last() != documentPos)
        m_lasso.push_back(documentPos);
    update(old | lassoPaintRect());
}

/// Applies the Move or Scale drag so far, live: the items in the document are the originals mapped by the drag's
/// transform, so the tiles show the result while the pen is down and one commit() closes it. A cancel restores the
/// commit (discardLiveEdits).
void InkCanvas::dragSelection(const QPointF &documentPos)
{
    const QPointF d = documentPos - m_dragStart;
    QTransform transform;
    if (m_drawing == Drawing::Move) {
        transform = QTransform::fromTranslate(d.x(), d.y());
    } else {
        const int h = m_dragHandle;
        const bool left = h == 0 || h == 6 || h == 7, right = h == 2 || h == 3 || h == 4;
        const bool top = h == 0 || h == 1 || h == 2, bottom = h == 4 || h == 5 || h == 6;
        const QRectF &b = m_dragBounds;
        double sx = 1.0, sy = 1.0;
        if ((left || right) && b.width() > 1e-6)
            sx = std::max(kMinScale, (b.width() + (right ? d.x() : -d.x())) / b.width());
        if ((top || bottom) && b.height() > 1e-6)
            sy = std::max(kMinScale, (b.height() + (bottom ? d.y() : -d.y())) / b.height());
        const QPointF anchor(left ? b.right() : b.left(), top ? b.bottom() : b.top()); // the side opposite the handle stays
        transform.translate(anchor.x(), anchor.y());
        transform.scale(sx, sy);
        transform.translate(-anchor.x(), -anchor.y());
    }
    if (transform == m_dragTransform)
        return; // the pointer did not move since the last sample (a release at the same place)
    m_dragTransform = transform;
    const QRectF oldBounds = m_selectionBounds;
    const QRect oldPaint = selectionPaintRect();
    // ponytail: every sample re-maps the whole selection from its originals; a selection of tens of thousands of points
    // would want the drag shown as a transformed image and the real mapping done once at release.
    for (const Item &original : m_dragOriginals)
        m_document.replaceItem(original.t, transformItem(original, transform));
    m_dragMoved = true;
    recomputeSelectionBounds();
    invalidateTiles(oldBounds);
    invalidateTiles(m_selectionBounds);
    update(oldPaint | selectionPaintRect());
}

void InkCanvas::finishSelect(Drawing mode, bool keep)
{
    if (mode == Drawing::Select) {
        const QRect paint = lassoPaintRect();
        QPainterPath region;
        if (keep && !m_lasso.isEmpty()) {
            if (m_selectShape == SelectShape::Rect) {
                region.addRect(QRectF(m_lasso.first(), m_lasso.last()).normalized());
            } else if (m_lasso.size() >= 3) {
                region.setFillRule(Qt::WindingFill); // a lasso that crosses itself selects inside every loop
                region.addPolygon(m_lasso);
                region.closeSubpath();
            }
        }
        m_lasso.clear();
        if (!region.isEmpty())
            setSelection(selectItems(m_document, region));
        update(paint);
        return;
    }
    const bool moved = std::exchange(m_dragMoved, false);
    m_dragOriginals.clear();
    if (!moved)
        return;
    if (!keep || m_dragTransform.isIdentity()) { // a cancel, or a drag that came back to its start: no history step
        const QRectF dragged = m_selectionBounds;
        const QRect draggedPaint = selectionPaintRect();
        m_document.discardLiveEdits();
        recomputeSelectionBounds();
        invalidateTiles(dragged); // the ink was shown there; it goes back where it was
        invalidateTiles(m_selectionBounds);
        update(draggedPaint | selectionPaintRect());
        return;
    }
    const bool grew = m_document.growToContain(m_selectionBounds);
    finishEdit();
    update(selectionPaintRect()); // a text box that was resized is drawn again now that the drag is over (never during it)
    if (grew) { // the page grew: the widget is resized, and the selection survives it
        const std::vector<qint64> keepSelection = m_selection;
        documentReplaced();
        setSelection(keepSelection);
    }
}

bool InkCanvas::deleteSelection()
{
    if (m_readOnly || isDrawing() || m_selection.empty())
        return false;
    const QRect paint = selectionPaintRect();
    for (qint64 t : m_selection) {
        if (const Item *item = m_document.item(t))
            invalidateTiles(item->bounds());
        m_document.removeItem(t);
    }
    m_selection.clear();
    m_selectionBounds = QRectF();
    finishEdit();
    update(paint);
    emit selectionChanged();
    return true;
}

bool InkCanvas::copySelection()
{
    if (m_selection.empty())
        return false;
    std::vector<const Item *> items;
    QStringList text;
    for (qint64 t : m_selection) {
        const Item *item = m_document.item(t);
        if (!item)
            continue;
        items.push_back(item);
        if (item->type == Item::Type::TextBox)
            text << item->textBox->markdown;
    }
    auto *mime = new QMimeData;
    mime->setData(QString::fromLatin1(kClipboardMime), itemsToClipboardJSON(items));
    if (!text.isEmpty())
        mime->setText(text.join(QLatin1Char('\n')));
    QApplication::clipboard()->setMimeData(mime); // the clipboard owns it
    return true;
}

bool InkCanvas::paste()
{
    if (m_readOnly || isDrawing())
        return false;
    const QMimeData *mime = QApplication::clipboard()->mimeData();
    if (!mime || !mime->hasFormat(QString::fromLatin1(kClipboardMime)))
        return false;
    bool ok = false;
    std::vector<Item> items = itemsFromClipboardJSON(mime->data(QString::fromLatin1(kClipboardMime)), &ok);
    // Another program may have put this on the clipboard: nothing that cannot be drawn is pasted (the reader is as open
    // to these, carry N1, but a paste is where they would arrive by accident).
    std::erase_if(items, [](const Item &item) {
        if (item.type == Item::Type::Group || item.type == Item::Type::Unknown || (item.type == Item::Type::Stroke && item.stroke->points.empty()))
            return true;
        const QRectF b = item.bounds();
        return b.isNull() || !finiteRect(b);
    });
    if (!ok || items.empty())
        return false;
    QRectF box;
    for (const Item &item : items)
        box |= item.bounds();
    QRect view = visibleRegion().boundingRect();
    if (view.isEmpty())
        view = rect();
    const QPointF target = toDocument(view.center()) + QPointF(20, 20);
    const QTransform shift = QTransform::fromTranslate(target.x() - box.center().x(), target.y() - box.center().y());
    std::vector<Item> moved;
    QRectF placed;
    for (const Item &item : items) {
        moved.push_back(transformItem(item, shift));
        placed |= moved.back().bounds();
    }
    if (!finiteRect(placed)) // a box so far out that the shift overflowed
        return false;
    std::vector<qint64> added;
    for (Item &item : moved)
        added.push_back(m_document.addItem(std::move(item))); // a new t at the end of the list
    const bool grew = m_document.growToContain(placed);
    finishEdit();
    invalidateTiles(placed);
    if (grew)
        documentReplaced();
    setTool(Tool::Lasso); // the selection belongs to the select tool: from the pen it would be gone at the next touch
    setSelection(std::move(added));
    update(widgetRect(placed));
    return true;
}

/// The lasso in flight and the selection's box with its handles: on the widget, over the tiles, never in a tile job.
void InkCanvas::paintSelection(QPainter &painter)
{
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, false);
    const QColor highlight = palette().highlight().color();
    painter.setBrush(Qt::NoBrush);
    if (m_drawing == Drawing::Select && m_lasso.size() >= 2) {
        painter.setPen(QPen(highlight, 1, Qt::DashLine));
        if (m_selectShape == SelectShape::Rect) {
            painter.drawRect(QRectF(toWidget(m_lasso.first()), toWidget(m_lasso.last())).normalized());
        } else {
            QPolygonF path;
            for (const QPointF &p : m_lasso)
                path << toWidget(p);
            painter.drawPolygon(path);
        }
    }
    if (!m_selection.empty() && !m_selectionBounds.isNull()) {
        const QRectF box = QRectF(toWidget(m_selectionBounds.topLeft()), toWidget(m_selectionBounds.bottomRight())).normalized();
        painter.setPen(QPen(highlight, 1));
        painter.drawRect(box);
        painter.setPen(palette().color(QPalette::Base));
        painter.setBrush(highlight);
        for (int h = 0; h < 8; ++h)
            if (handleShown(box, h))
                painter.drawRect(QRectF(handlePoint(box, h) - QPointF(kHandleSize / 2, kHandleSize / 2), QSizeF(kHandleSize, kHandleSize)));
    }
    painter.restore();
}

} // namespace noteAppC::ink
