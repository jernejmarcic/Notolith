// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej
// Mouse and keyboard handling: selection, drag-to-move, drag-to-resize,
// drag-from-a-side-handle to connect, rubber-band selection, panning, and the
// small amount of hit-testing geometry all of those share with painting the
// handles themselves (CanvasRender.cpp).

#include "CanvasView.h"

#include <QKeyEvent>
#include <QLineF>
#include <QMouseEvent>
#include <QPainterPath>
#include <QPainterPathStroker>

#include <algorithm>
#include <cmath>

#include "CanvasCommands.h"

namespace noteAppC {

namespace {
constexpr qreal kDragThreshold = 3.0; // pixels, click vs. drag
constexpr qreal kMinNodeSize = 40.0;
}

QRectF CanvasView::currentRect(const CanvasNode &node) const
{
    if (m_drag.moved && m_drag.startRects.contains(node.id())) {
        if (m_drag.kind == DragKind::Move)
            return m_drag.startRects.value(node.id()).translated(m_drag.liveDelta);
        if (m_drag.kind == DragKind::Resize)
            return m_drag.liveRect;
    }
    return node.rect();
}

QString CanvasView::nodeAt(const QPoint &viewportPoint) const
{
    const auto &nodes = m_document.nodes();
    for (auto it = nodes.crbegin(); it != nodes.crend(); ++it) {
        if (it->type() == CanvasNodeType::Group)
            continue;
        QRectF screen;
        if (isNodeVisible(*it, &screen) && screen.contains(viewportPoint))
            return it->id();
    }
    for (auto it = nodes.crbegin(); it != nodes.crend(); ++it) {
        if (it->type() != CanvasNodeType::Group)
            continue;
        QRectF screen;
        if (isNodeVisible(*it, &screen) && screen.contains(viewportPoint))
            return it->id();
    }
    return QString();
}

QString CanvasView::edgeAt(const QPoint &viewportPoint) const
{
    for (auto it = m_document.edges().crbegin(); it != m_document.edges().crend(); ++it) {
        const QPainterPath path = edgePath(*it, nullptr);
        QPainterPathStroker stroker;
        stroker.setWidth(10);
        if (stroker.createStroke(path).contains(QPointF(viewportPoint)))
            return it->id();
    }
    return QString();
}

CanvasView::HitResult CanvasView::hitTest(const QPoint &viewportPoint) const
{
    // Connector handles live just outside the node the pointer is already
    // hovering - mouseMoveEvent keeps m_hoveredNode current before any press
    // can land, so this never has to scan every node to find them.
    if (!m_hoveredNode.isEmpty()) {
        if (const CanvasNode *node = m_document.node(m_hoveredNode)) {
            const QRectF s(mapFromCanvas(currentRect(*node).topLeft()),
                           mapFromCanvas(currentRect(*node).bottomRight()));
            static const QString sides[] = {QStringLiteral("top"), QStringLiteral("right"),
                                            QStringLiteral("bottom"), QStringLiteral("left")};
            for (const QString &side : sides) {
                const QPointF handle = sideAnchor(s, side) + sideNormal(side) * kConnectHandleOffset;
                if (QLineF(handle, QPointF(viewportPoint)).length() <= kConnectHitRadius)
                    return {HandleKind::Connect, node->id(), side};
            }
        }
    }

    // Resize handles, only meaningful with exactly one node selected.
    if (m_selectedNodes.size() == 1) {
        const QString id = *m_selectedNodes.begin();
        if (const CanvasNode *node = m_document.node(id)) {
            const QRectF s(mapFromCanvas(currentRect(*node).topLeft()),
                           mapFromCanvas(currentRect(*node).bottomRight()));
            const std::pair<HandleKind, QPointF> handles[] = {
                {HandleKind::ResizeTopLeft, s.topLeft()},
                {HandleKind::ResizeTopRight, s.topRight()},
                {HandleKind::ResizeBottomLeft, s.bottomLeft()},
                {HandleKind::ResizeBottomRight, s.bottomRight()},
                {HandleKind::ResizeTop, QPointF((s.left() + s.right()) / 2, s.top())},
                {HandleKind::ResizeBottom, QPointF((s.left() + s.right()) / 2, s.bottom())},
                {HandleKind::ResizeLeft, QPointF(s.left(), (s.top() + s.bottom()) / 2)},
                {HandleKind::ResizeRight, QPointF(s.right(), (s.top() + s.bottom()) / 2)},
            };
            for (const auto &[kind, point] : handles)
                if (QLineF(point, QPointF(viewportPoint)).length() <= kResizeHitRadius)
                    return {kind, id, QString()};
        }
    }

    const QString id = nodeAt(viewportPoint);
    if (!id.isEmpty())
        return {HandleKind::Move, id, QString()};
    return {};
}

Qt::CursorShape CanvasView::cursorFor(HandleKind kind) const
{
    switch (kind) {
    case HandleKind::ResizeTop:
    case HandleKind::ResizeBottom: return Qt::SizeVerCursor;
    case HandleKind::ResizeLeft:
    case HandleKind::ResizeRight: return Qt::SizeHorCursor;
    case HandleKind::ResizeTopLeft:
    case HandleKind::ResizeBottomRight: return Qt::SizeFDiagCursor;
    case HandleKind::ResizeTopRight:
    case HandleKind::ResizeBottomLeft: return Qt::SizeBDiagCursor;
    case HandleKind::Connect: return Qt::CrossCursor;
    case HandleKind::Move: return Qt::SizeAllCursor;
    case HandleKind::None: break;
    }
    return m_spaceHeld ? Qt::OpenHandCursor : Qt::ArrowCursor;
}

void CanvasView::beginPan(const QPoint &viewportPoint)
{
    m_drag = DragState{};
    m_drag.kind = DragKind::Pan;
    m_drag.startViewport = viewportPoint;
    m_drag.startOrigin = m_origin;
    setCursor(Qt::ClosedHandCursor);
}

void CanvasView::updatePan(const QPoint &viewportPoint)
{
    const QPointF deltaScreen = QPointF(viewportPoint - m_drag.startViewport);
    m_origin = m_drag.startOrigin - deltaScreen / m_zoom;
    if (m_textEditor)
        positionTextEditor();
    update();
}

void CanvasView::beginRubberBand(const QPoint &viewportPoint, bool extend)
{
    m_drag = DragState{};
    m_drag.kind = DragKind::RubberBand;
    m_drag.startCanvas = mapToCanvas(viewportPoint);
    m_drag.currentCanvas = m_drag.startCanvas;
    m_drag.additive = extend;
    if (!extend) {
        dropScrollEditor();
        m_selectedNodes.clear();
        m_selectedEdges.clear();
    }
}

void CanvasView::updateRubberBand(const QPoint &viewportPoint)
{
    m_drag.currentCanvas = mapToCanvas(viewportPoint);
    m_drag.moved = true;
    update();
}

void CanvasView::finishRubberBand()
{
    if (m_drag.moved) {
        const QRectF band = QRectF(m_drag.startCanvas, m_drag.currentCanvas).normalized();
        for (const CanvasNode &node : m_document.nodes())
            if (band.intersects(node.rect()))
                m_selectedNodes.insert(node.id());
    }
    m_drag = DragState{};
    update();
}

void CanvasView::beginMove(const HitResult &hit, const QPoint &viewportPoint, Qt::KeyboardModifiers modifiers)
{
    const bool resizing = hit.kind != HandleKind::Move;
    if (!resizing) {
        if ((modifiers & Qt::ShiftModifier) || !m_selectedNodes.contains(hit.nodeID))
            dropScrollEditor();
        if (modifiers & Qt::ShiftModifier) {
            if (m_selectedNodes.contains(hit.nodeID))
                m_selectedNodes.remove(hit.nodeID);
            else
                m_selectedNodes.insert(hit.nodeID);
        } else if (!m_selectedNodes.contains(hit.nodeID)) {
            m_selectedNodes.clear();
            m_selectedEdges.clear();
            m_selectedNodes.insert(hit.nodeID);
        }
        update();
    }

    m_drag = DragState{};
    m_drag.kind = resizing ? DragKind::Resize : DragKind::Move;
    m_drag.resizeHandle = hit.kind;
    m_drag.startViewport = viewportPoint;
    if (resizing) {
        if (const CanvasNode *node = m_document.node(hit.nodeID)) {
            m_drag.startRects.insert(hit.nodeID, node->rect());
            m_drag.liveRect = node->rect();
        }
    } else {
        for (const QString &id : std::as_const(m_selectedNodes))
            if (const CanvasNode *node = m_document.node(id))
                m_drag.startRects.insert(id, node->rect());
        // A group carries what sits inside it, as in Obsidian: every card
        // wholly within a moved group's frame when the drag starts moves too,
        // in the same undo step.
        const QHash<QString, QRectF> moving = m_drag.startRects;
        for (auto it = moving.cbegin(); it != moving.cend(); ++it) {
            const CanvasNode *group = m_document.node(it.key());
            if (!group || group->type() != CanvasNodeType::Group)
                continue;
            for (const CanvasNode &node : m_document.nodes()) {
                if (node.id() != group->id() && it.value().contains(node.rect()))
                    m_drag.startRects.insert(node.id(), node.rect());
            }
        }
        for (const QRectF &rect : std::as_const(m_drag.startRects))
            m_drag.startBox = m_drag.startBox.isNull() ? rect : m_drag.startBox.united(rect);
    }
}

void CanvasView::buildSnapLines()
{
    m_drag.snapX.reserve(m_document.nodes().size() * 3);
    m_drag.snapY.reserve(m_document.nodes().size() * 3);
    for (const CanvasNode &node : m_document.nodes()) {
        if (m_drag.startRects.contains(node.id()))
            continue;
        const QRectF r = node.rect();
        for (const qreal x : {r.left(), r.center().x(), r.right()})
            m_drag.snapX.append({x, r.top(), r.bottom()});
        for (const qreal y : {r.top(), r.center().y(), r.bottom()})
            m_drag.snapY.append({y, r.left(), r.right()});
    }
    const auto byPos = [](const DragState::SnapLine &a, const DragState::SnapLine &b) { return a.pos < b.pos; };
    std::sort(m_drag.snapX.begin(), m_drag.snapX.end(), byPos);
    std::sort(m_drag.snapY.begin(), m_drag.snapY.end(), byPos);
}

namespace {
/// The signed distance from one of `candidates` to the nearest of `lines`
/// (sorted by pos) when that is within `reach`; -1 in `index` when none is.
template <typename Lines>
qreal nearestLine(const Lines &lines, const qreal *candidates, int count, qreal reach, qsizetype *index)
{
    qreal best = 0;
    *index = -1;
    for (int i = 0; i < count; ++i) {
        const auto above = std::lower_bound(lines.cbegin(), lines.cend(), candidates[i],
                                            [](const auto &l, qreal v) { return l.pos < v; });
        for (auto it : {above, above == lines.cbegin() ? above : above - 1}) {
            if (it == lines.cend())
                continue;
            const qreal d = it->pos - candidates[i];
            if (std::abs(d) <= reach && (*index < 0 || std::abs(d) < std::abs(best))) {
                best = d;
                *index = it - lines.cbegin();
            }
        }
    }
    return best;
}
}

QRectF CanvasView::applyResize(const QRectF &start, HandleKind handle, const QPointF &canvasDelta)
{
    QRectF r = start;
    switch (handle) {
    case HandleKind::ResizeTop: r.setTop(r.top() + canvasDelta.y()); break;
    case HandleKind::ResizeBottom: r.setBottom(r.bottom() + canvasDelta.y()); break;
    case HandleKind::ResizeLeft: r.setLeft(r.left() + canvasDelta.x()); break;
    case HandleKind::ResizeRight: r.setRight(r.right() + canvasDelta.x()); break;
    case HandleKind::ResizeTopLeft: r.setTopLeft(r.topLeft() + canvasDelta); break;
    case HandleKind::ResizeTopRight: r.setTopRight(r.topRight() + canvasDelta); break;
    case HandleKind::ResizeBottomLeft: r.setBottomLeft(r.bottomLeft() + canvasDelta); break;
    case HandleKind::ResizeBottomRight: r.setBottomRight(r.bottomRight() + canvasDelta); break;
    default: break;
    }
    if (r.width() < kMinNodeSize) {
        const bool leftAnchored = handle == HandleKind::ResizeLeft || handle == HandleKind::ResizeTopLeft
            || handle == HandleKind::ResizeBottomLeft;
        if (leftAnchored)
            r.setLeft(r.right() - kMinNodeSize);
        else
            r.setRight(r.left() + kMinNodeSize);
    }
    if (r.height() < kMinNodeSize) {
        const bool topAnchored = handle == HandleKind::ResizeTop || handle == HandleKind::ResizeTopLeft
            || handle == HandleKind::ResizeTopRight;
        if (topAnchored)
            r.setTop(r.bottom() - kMinNodeSize);
        else
            r.setBottom(r.top() + kMinNodeSize);
    }
    return r;
}

CanvasView::SnapResult CanvasView::snapShift(const qreal *xs, int nx, const qreal *ys, int ny) const
{
    SnapResult result;
    const qreal reach = kSnapReachPx / m_zoom;
    if (m_snapToObjects) {
        qsizetype ix, iy;
        const qreal dx = nearestLine(m_drag.snapX, xs, nx, reach, &ix);
        const qreal dy = nearestLine(m_drag.snapY, ys, ny, reach, &iy);
        if (ix >= 0) {
            result.shift.setX(dx);
            result.objectX = true;
            result.lineX = m_drag.snapX.at(ix).pos;
        }
        if (iy >= 0) {
            result.shift.setY(dy);
            result.objectY = true;
            result.lineY = m_drag.snapY.at(iy).pos;
        }
    }
    // A card beats the grid, axis by axis.
    if (m_snapToGrid) {
        if (!result.objectX && nx > 0)
            result.shift.setX(std::round(xs[0] / kGridSpacingCanvas) * kGridSpacingCanvas - xs[0]);
        if (!result.objectY && ny > 0)
            result.shift.setY(std::round(ys[0] / kGridSpacingCanvas) * kGridSpacingCanvas - ys[0]);
    }
    return result;
}

void CanvasView::setGuides(const QRectF &box, const SnapResult &snap)
{
    // One line per axis, running from the dragged box out to the full extent
    // of every card sharing that line.
    const auto guide = [](const QVector<DragState::SnapLine> &lines, qreal pos, qreal lo, qreal hi) {
        const auto first = std::lower_bound(lines.cbegin(), lines.cend(), pos - 0.01,
                                            [](const DragState::SnapLine &l, qreal v) { return l.pos < v; });
        for (auto it = first; it != lines.cend() && it->pos <= pos + 0.01; ++it) {
            lo = std::min(lo, it->lo);
            hi = std::max(hi, it->hi);
        }
        return std::pair(lo, hi);
    };
    m_drag.guides.clear();
    if (snap.objectX) {
        const auto [lo, hi] = guide(m_drag.snapX, snap.lineX, box.top(), box.bottom());
        m_drag.guides.append(QLineF(snap.lineX, lo, snap.lineX, hi));
    }
    if (snap.objectY) {
        const auto [lo, hi] = guide(m_drag.snapY, snap.lineY, box.left(), box.right());
        m_drag.guides.append(QLineF(lo, snap.lineY, hi, snap.lineY));
    }
}

void CanvasView::updateMoveOrResize(const QPoint &viewportPoint)
{
    if (!m_drag.moved && QLineF(m_drag.startViewport, viewportPoint).length() < kDragThreshold)
        return;
    if (!m_drag.moved) {
        m_drag.moved = true;
        if (m_snapToObjects)
            buildSnapLines(); // on the first real move, not on every click
    }
    const QPointF canvasDelta = (QPointF(viewportPoint) - QPointF(m_drag.startViewport)) / m_zoom;
    if (m_drag.kind == DragKind::Move) {
        // The whole selection is one box: its edges and centre are what snap.
        const QRectF box = m_drag.startBox.translated(canvasDelta);
        const qreal xs[] = {box.left(), box.center().x(), box.right()};
        const qreal ys[] = {box.top(), box.center().y(), box.bottom()};
        const SnapResult snap = snapShift(xs, 3, ys, 3);
        m_drag.liveDelta = canvasDelta + snap.shift;
        setGuides(box.translated(snap.shift), snap);
    } else {
        // Only the edges the handle drags snap; the centre is not one of them.
        const QRectF start = m_drag.startRects.constBegin().value();
        const QRectF dragged = applyResize(start, m_drag.resizeHandle, canvasDelta);
        const HandleKind h = m_drag.resizeHandle;
        const bool left = h == HandleKind::ResizeLeft || h == HandleKind::ResizeTopLeft || h == HandleKind::ResizeBottomLeft;
        const bool right = h == HandleKind::ResizeRight || h == HandleKind::ResizeTopRight || h == HandleKind::ResizeBottomRight;
        const bool top = h == HandleKind::ResizeTop || h == HandleKind::ResizeTopLeft || h == HandleKind::ResizeTopRight;
        const bool bottom = h == HandleKind::ResizeBottom || h == HandleKind::ResizeBottomLeft || h == HandleKind::ResizeBottomRight;
        const qreal xs[] = {left ? dragged.left() : dragged.right()};
        const qreal ys[] = {top ? dragged.top() : dragged.bottom()};
        const SnapResult snap = snapShift(xs, left || right ? 1 : 0, ys, top || bottom ? 1 : 0);
        m_drag.liveRect = applyResize(start, h, canvasDelta + snap.shift);
        setGuides(m_drag.liveRect, snap);
    }
    if (m_textEditor && m_drag.startRects.contains(m_editingNodeID))
        positionTextEditor();
    update();
}

void CanvasView::finishMoveOrResize()
{
    if (m_drag.moved) {
        QHash<QString, QRectF> newRects;
        for (auto it = m_drag.startRects.cbegin(); it != m_drag.startRects.cend(); ++it) {
            newRects.insert(it.key(), m_drag.kind == DragKind::Resize ? m_drag.liveRect
                                                                       : it.value().translated(m_drag.liveDelta));
        }
        const QString text = m_drag.kind == DragKind::Resize ? tr("Resize") : tr("Move");
        m_undoStack.push(new MoveResizeNodesCommand(&m_document, m_drag.startRects, newRects, text));
    }
    m_drag = DragState{};
    update();
}

void CanvasView::beginConnect(const HitResult &hit)
{
    m_drag = DragState{};
    m_drag.kind = DragKind::Connect;
    m_drag.connectFromNode = hit.nodeID;
    m_drag.connectFromSide = hit.side;
}

void CanvasView::updateConnect(const QPoint &viewportPoint)
{
    m_drag.currentCanvas = mapToCanvas(viewportPoint);
    QString target = nodeAt(viewportPoint);
    if (target == m_drag.connectFromNode)
        target.clear();
    m_drag.connectHoverTarget = target;
    update();
}

void CanvasView::finishConnect(const QPoint &viewportPoint)
{
    const QString target = nodeAt(viewportPoint);
    if (!target.isEmpty() && target != m_drag.connectFromNode) {
        const CanvasNode *from = m_document.node(m_drag.connectFromNode);
        const CanvasNode *to = m_document.node(target);
        if (from && to) {
            CanvasEdge edge;
            edge.setFromNode(from->id());
            edge.setToNode(to->id());
            edge.setFromSide(m_drag.connectFromSide);
            edge.setToSide(effectiveSide(QString(), to->rect(), from->rect()));
            m_undoStack.push(new AddEdgeCommand(&m_document, edge));
        }
    }
    m_drag = DragState{};
    update();
}

void CanvasView::updateHover(const QPoint &viewportPoint)
{
    const QString hoveredNode = nodeAt(viewportPoint);
    const QString hoveredEdge = hoveredNode.isEmpty() ? edgeAt(viewportPoint) : QString();
    if (hoveredNode != m_hoveredNode || hoveredEdge != m_hoveredEdge) {
        m_hoveredNode = hoveredNode;
        m_hoveredEdge = hoveredEdge;
        update();
    }
    setCursor(cursorFor(hitTest(viewportPoint).kind));
}

void CanvasView::activateIfFileNode(const QString &id)
{
    const CanvasNode *node = m_document.node(id);
    if (!node || node->type() != CanvasNodeType::File || node->file().isEmpty())
        return;
    emit noteActivated(node->file());
}

void CanvasView::mousePressEvent(QMouseEvent *event)
{
    setFocus();
    if (m_textEditor)
        commitEditingText(); // reaching CanvasView at all means the click was outside it

    if (event->button() == Qt::MiddleButton) {
        beginPan(event->pos());
        return;
    }
    if (event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }
    if (m_spaceHeld) {
        beginPan(event->pos());
        return;
    }

    const HitResult hit = hitTest(event->pos());
    if (hit.kind == HandleKind::Connect) {
        beginConnect(hit);
        return;
    }
    if (hit.kind != HandleKind::None && hit.kind != HandleKind::Move) {
        beginMove(hit, event->pos(), event->modifiers());
        return;
    }
    if (!hit.nodeID.isEmpty()) {
        if (event->modifiers() & Qt::ControlModifier) {
            activateIfFileNode(hit.nodeID);
            return;
        }
        beginMove(hit, event->pos(), event->modifiers());
        return;
    }

    const QString edgeID = edgeAt(event->pos());
    if (!edgeID.isEmpty()) {
        if (event->modifiers() & Qt::ShiftModifier) {
            if (m_selectedEdges.contains(edgeID))
                m_selectedEdges.remove(edgeID);
            else
                m_selectedEdges.insert(edgeID);
        } else {
            dropScrollEditor();
            m_selectedNodes.clear();
            m_selectedEdges.clear();
            m_selectedEdges.insert(edgeID);
        }
        update();
        return;
    }

    // A drag on the empty canvas draws a selection box, as in Obsidian;
    // Shift adds to what is selected. Panning is Space or the middle button.
    beginRubberBand(event->pos(), event->modifiers() & Qt::ShiftModifier);
}

void CanvasView::mouseMoveEvent(QMouseEvent *event)
{
    switch (m_drag.kind) {
    case DragKind::Pan: updatePan(event->pos()); return;
    case DragKind::RubberBand: updateRubberBand(event->pos()); return;
    case DragKind::Move:
    case DragKind::Resize: updateMoveOrResize(event->pos()); return;
    case DragKind::Connect: updateConnect(event->pos()); return;
    case DragKind::None: updateHover(event->pos()); return;
    }
}

void CanvasView::mouseReleaseEvent(QMouseEvent *event)
{
    switch (m_drag.kind) {
    case DragKind::Pan:
        setCursor(m_spaceHeld ? Qt::OpenHandCursor : Qt::ArrowCursor);
        m_drag = DragState{};
        break;
    case DragKind::RubberBand: finishRubberBand(); break;
    case DragKind::Move:
    case DragKind::Resize: finishMoveOrResize(); break;
    case DragKind::Connect: finishConnect(event->pos()); break;
    case DragKind::None: break;
    }
    updateHover(event->pos());
}

void CanvasView::mouseDoubleClickEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton)
        return;
    const HitResult hit = hitTest(event->pos());
    if (hit.kind == HandleKind::Move) {
        const CanvasNode *node = m_document.node(hit.nodeID);
        if (!node)
            return;
        if (node->type() == CanvasNodeType::Text)
            beginEditingText(hit.nodeID);
        else if (node->type() == CanvasNodeType::File)
            activateIfFileNode(hit.nodeID);
        return;
    }
    if (hit.kind != HandleKind::None)
        return; // a handle: nothing to do on a double-click
    const QString id = createTextNodeAt(mapToCanvas(event->pos()));
    beginEditingText(id);
}

void CanvasView::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Space && !event->isAutoRepeat()) {
        m_spaceHeld = true;
        setCursor(Qt::OpenHandCursor);
        return;
    }
    if (event->matches(QKeySequence::Delete) || event->key() == Qt::Key_Backspace) {
        deleteSelection();
        return;
    }
    if (event->matches(QKeySequence::SelectAll)) {
        selectAll();
        return;
    }
    if (event->matches(QKeySequence::Undo)) {
        m_undoStack.undo();
        return;
    }
    if (event->matches(QKeySequence::Redo)) {
        m_undoStack.redo();
        return;
    }
    if (event->key() == Qt::Key_Escape) {
        if (isEditingText())
            commitEditingText();
        else
            clearSelection();
        return;
    }
    QWidget::keyPressEvent(event);
}

} // namespace noteAppC
