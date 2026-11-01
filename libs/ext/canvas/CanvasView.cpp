// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej
#include "CanvasView.h"

#include <QGuiApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QScrollBar>
#include <QWheelEvent>

#include <cmath>

#include "CanvasCommands.h"
#include "MarkdownEditor.h"

namespace noteAppC {

namespace {
constexpr qreal kMinZoom = 0.05;
constexpr qreal kMaxZoom = 8.0;
constexpr qreal kDefaultNodeWidth = 260;
constexpr qreal kDefaultNodeHeight = 100;
}

CanvasView::CanvasView(QWidget *parent)
    : QWidget(parent)
    , m_theme(Theme::builtin())
{
    setObjectName(QStringLiteral("canvasView"));
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setAutoFillBackground(false);
    // isModified()/save() track QUndoStack's own clean flag rather than
    // keeping a second, easily-out-of-sync "has this changed" bool: it is
    // already set clean on construction, flips on every push/undo/redo, and
    // finds its way back to clean if a redo lands back where setClean() was
    // called - exactly what a modified indicator and a save prompt want.
    connect(&m_undoStack, &QUndoStack::cleanChanged, this,
           [this](bool clean) { emit modificationChanged(!clean); });
}

CanvasView::~CanvasView() = default;

void CanvasView::setTheme(const Theme &theme)
{
    m_theme = theme;
    m_contentCache.clear(); // callout/text colours baked into the cached pixmaps
    if (m_textEditor)
        m_textEditor->setTheme(m_theme);
    update();
}

bool CanvasView::load(const QString &path, QString *error)
{
    cancelEditingText();
    if (!m_document.load(path, error))
        return false;
    clearSelection();
    m_contentCache.clear();
    m_undoStack.clear();
    m_undoStack.setClean();
    m_zoom = 1.0;
    m_origin = QPointF(0, 0);
    // Framed at the first paint, when the layout has given the view its real
    // size (a tab's layout resizes it several times first): a canvas is
    // rarely drawn around the origin, and Obsidian opens one showing all of it.
    m_fitPending = true;
    update();
    return true;
}

QRectF CanvasView::contentBounds() const
{
    QRectF bounds;
    for (const CanvasNode &node : m_document.nodes())
        bounds = bounds.isNull() ? node.rect() : bounds.united(node.rect());
    return bounds;
}

void CanvasView::fitToContent()
{
    m_fitPending = false;
    const QRectF bounds = contentBounds();
    if (bounds.isEmpty() || width() <= 0 || height() <= 0)
        return;
    const qreal margin = 40;
    m_zoom = qMin((width() - 2 * margin) / bounds.width(), (height() - 2 * margin) / bounds.height());
    m_zoom = qMin(m_zoom, 1.0); // never blown up past its own size
    clampZoom();
    m_origin = bounds.center() - QPointF(width(), height()) / (2 * m_zoom);
    update();
}

bool CanvasView::save(const QString &path, QString *error)
{
    if (isEditingText())
        commitEditingText();
    if (!m_document.save(path, error))
        return false;
    m_undoStack.setClean();
    return true;
}

void CanvasView::setFileResolver(std::function<QString(const QString &)> resolver)
{
    m_fileResolver = std::move(resolver);
    m_contentCache.clear();
    update();
}

void CanvasView::clampZoom()
{
    m_zoom = qBound(kMinZoom, m_zoom, kMaxZoom);
}

void CanvasView::setZoom(qreal zoom)
{
    const QPointF centre = mapToCanvas(QPoint(width() / 2, height() / 2));
    m_zoom = zoom;
    clampZoom();
    m_origin = centre - QPointF(width() / 2, height() / 2) / m_zoom;
    if (m_textEditor)
        positionTextEditor();
    update();
}

QPointF CanvasView::mapToCanvas(const QPoint &viewportPoint) const
{
    return QPointF(viewportPoint) / m_zoom + m_origin;
}

QPoint CanvasView::mapFromCanvas(const QPointF &canvasPoint) const
{
    return mapFromCanvasF(canvasPoint).toPoint();
}

QPointF CanvasView::mapFromCanvasF(const QPointF &canvasPoint) const
{
    return (canvasPoint - m_origin) * m_zoom;
}

QRectF CanvasView::viewportCanvasRect() const
{
    return QRectF(m_origin, QSizeF(width(), height()) / m_zoom);
}

void CanvasView::selectAll()
{
    dropScrollEditor();
    m_selectedNodes.clear();
    m_selectedEdges.clear();
    for (const CanvasNode &node : m_document.nodes())
        m_selectedNodes.insert(node.id());
    update();
}

void CanvasView::clearSelection()
{
    if (m_selectedNodes.isEmpty() && m_selectedEdges.isEmpty())
        return;
    dropScrollEditor();
    m_selectedNodes.clear();
    m_selectedEdges.clear();
    update();
}

void CanvasView::selectNode(const QString &id, bool extend)
{
    dropScrollEditor();
    if (!extend)
        m_selectedEdges.clear();
    if (!extend)
        m_selectedNodes.clear();
    m_selectedNodes.insert(id);
    update();
}

void CanvasView::deleteSelection()
{
    if (m_selectedNodes.isEmpty() && m_selectedEdges.isEmpty())
        return;
    if (isEditingText() && m_selectedNodes.contains(m_editingNodeID))
        cancelEditingText();

    // Every edge touching a node being removed goes too, cascading, plus
    // whatever edges were independently selected - each its own command so
    // undo restores them all, but pushed inside one macro so Del is one step.
    QSet<QString> edgesToRemove = m_selectedEdges;
    for (const CanvasEdge &edge : m_document.edges()) {
        if (m_selectedNodes.contains(edge.fromNode()) || m_selectedNodes.contains(edge.toNode()))
            edgesToRemove.insert(edge.id());
    }

    m_undoStack.beginMacro(tr("Delete"));
    for (const QString &id : std::as_const(edgesToRemove))
        m_undoStack.push(new RemoveEdgeCommand(&m_document, id));
    for (const QString &id : std::as_const(m_selectedNodes))
        m_undoStack.push(new RemoveNodeCommand(&m_document, id));
    m_undoStack.endMacro();

    for (const QString &id : std::as_const(m_selectedNodes))
        m_contentCache.remove(id);
    clearSelection();
}

QString CanvasView::createTextNodeAt(const QPointF &canvasPoint)
{
    CanvasNode node;
    node.setType(CanvasNodeType::Text);
    node.setRect(QRectF(canvasPoint.x() - kDefaultNodeWidth / 2, canvasPoint.y() - kDefaultNodeHeight / 2,
                        kDefaultNodeWidth, kDefaultNodeHeight));
    node.setText(QString());
    auto *command = new AddNodeCommand(&m_document, node);
    m_undoStack.push(command);
    const QString id = command->nodeID();
    selectNode(id, false);
    return id;
}

bool CanvasView::beginEditingText(const QString &id)
{
    const CanvasNode *node = m_document.node(id);
    if (!node || node->type() != CanvasNodeType::Text)
        return false;
    if (m_editingNodeID == id)
        return true;
    commitEditingText();
    dropScrollEditor();

    m_editingNodeID = id;
    m_textEditor = new MarkdownEditor(this);
    m_textEditor->setObjectName(QStringLiteral("canvasTextEditor"));
    m_textEditor->setTheme(m_theme);
    m_textEditor->setMode(MarkdownEditor::Mode::LivePreview);
    m_textEditor->setNoteText(node->text());
    m_textEditor->installEventFilter(this);
    positionTextEditor();
    m_textEditor->show();
    m_textEditor->setFocus();
    update();
    return true;
}

void CanvasView::positionTextEditor()
{
    if (!m_textEditor)
        return;
    const CanvasNode *node = m_document.node(m_editingNodeID);
    if (!node) {
        cancelEditingText();
        return;
    }
    const QRectF r = node->rect();
    m_textEditor->setGeometry(QRect(mapFromCanvas(r.topLeft()), mapFromCanvas(r.bottomRight())));
}

void CanvasView::commitEditingText()
{
    if (!m_textEditor)
        return;
    const QString id = m_editingNodeID;
    const QString newText = m_textEditor->noteText();
    const CanvasNode *node = m_document.node(id);
    const QString oldText = node ? node->text() : QString();
    m_textEditor->removeEventFilter(this);
    m_textEditor->deleteLater();
    m_textEditor = nullptr;
    m_editingNodeID.clear();
    if (node && newText != oldText) {
        m_undoStack.push(new EditTextCommand(&m_document, id, oldText, newText));
        m_contentCache.remove(id);
    }
    setFocus();
    update();
}

void CanvasView::cancelEditingText()
{
    if (!m_textEditor)
        return;
    m_textEditor->removeEventFilter(this);
    m_textEditor->deleteLater();
    m_textEditor = nullptr;
    m_editingNodeID.clear();
    setFocus();
    update();
}

void CanvasView::paintEvent(QPaintEvent *)
{
    if (m_fitPending)
        fitToContent();
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    painter.fillRect(rect(), m_theme.color(QStringLiteral("--background-primary")));
    paintGrid(painter);
    paintGroups(painter);
    paintEdges(painter);
    paintNodes(painter);
    paintOverlay(painter);
}

void CanvasView::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    if (m_textEditor)
        positionTextEditor();
}

void CanvasView::leaveEvent(QEvent *event)
{
    QWidget::leaveEvent(event);
    if (m_drag.kind == DragKind::None) {
        m_hoveredNode.clear();
        m_hoveredConnectSide.clear();
        m_hoveredEdge.clear();
        update();
    }
}

bool CanvasView::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_textEditor && event->type() == QEvent::KeyPress) {
        auto *key = static_cast<QKeyEvent *>(event);
        if (key->key() == Qt::Key_Escape) {
            commitEditingText();
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void CanvasView::zoomAbout(const QPoint &viewportPoint, int angleDeltaY)
{
    const QPointF before = mapToCanvas(viewportPoint);
    const qreal factor = std::pow(1.0015, angleDeltaY);
    m_zoom = qBound(kMinZoom, m_zoom * factor, kMaxZoom);
    m_origin = before - QPointF(viewportPoint) / m_zoom;
    if (m_textEditor)
        positionTextEditor();
    update();
}

void CanvasView::panBy(const QPointF &pixelDelta)
{
    m_origin -= pixelDelta / m_zoom;
    if (m_textEditor)
        positionTextEditor();
    update();
}

void CanvasView::dropScrollEditor()
{
    m_scrollEditor.reset();
    m_scrollNodeID.clear();
    m_scrollY = 0;
}

bool CanvasView::scrollEditorShows(const CanvasNode &node) const
{
    const QRectF r = node.rect();
    return m_scrollEditor && node.id() == m_scrollNodeID && m_selectedNodes.size() == 1
        && m_selectedNodes.contains(node.id()) && contentSignature(node) == m_scrollSignature
        && m_scrollEditor->size() == QSize(qMax(1, qRound(r.width())), qMax(1, qRound(r.height()))) * kContentScale;
}

qreal CanvasView::cardScrollOffset() const
{
    const CanvasNode *node = m_document.node(m_scrollNodeID);
    return node && scrollEditorShows(*node) ? m_scrollEditor->verticalScrollBar()->value() / qreal(kContentScale) : 0;
}

qreal CanvasView::cardScrollMaximum() const
{
    const CanvasNode *node = m_document.node(m_scrollNodeID);
    return node && scrollEditorShows(*node) ? m_scrollEditor->verticalScrollBar()->maximum() / qreal(kContentScale) : 0;
}

bool CanvasView::scrollCardBy(const QString &id, qreal screenDeltaY)
{
    const CanvasNode *node = m_document.node(id);
    QString text;
    if (!node || !markdownTextOf(*node, &text))
        return false;
    if (!scrollEditorShows(*node)) {
        const QRectF r = node->rect();
        m_scrollEditor = makeContentEditor(text, QSize(qMax(1, qRound(r.width())), qMax(1, qRound(r.height()))) * kContentScale);
        m_scrollNodeID = id;
        m_scrollSignature = contentSignature(*node);
        m_scrollY = 0;
        ++m_scrollBuilds;
        // The editor lays out what it shows when it paints: until it has, its
        // scroll range is a guess from the note's length.
        m_scrollEditor->grab();
    }
    QScrollBar *bar = m_scrollEditor->verticalScrollBar();
    if (bar->maximum() <= 0)
        return false; // it all fits: nothing to scroll
    m_scrollY = qBound(0.0, m_scrollY - screenDeltaY * kContentScale / m_zoom, qreal(bar->maximum()));
    bar->setValue(qRound(m_scrollY));
    update();
    return true;
}

void CanvasView::wheelEvent(QWheelEvent *event)
{
    // Obsidian's canvas: the wheel pans, Shift turns it sideways, and Ctrl
    // or a held Space makes it zoom about the pointer. A touchpad's pixel
    // deltas pan as they come; a mouse wheel's degrees are halved into
    // pixels the way the editor's tables pan.
    if (event->modifiers().testFlag(Qt::ControlModifier) || m_spaceHeld) {
        zoomAbout(event->position().toPoint(), event->angleDelta().y());
    } else {
        QPointF delta = event->pixelDelta().isNull() ? QPointF(event->angleDelta()) / 2.0
                                                    : QPointF(event->pixelDelta());
        // The one selected card, when its content runs past its bottom,
        // takes the wheel; any other card, or empty canvas, still pans.
        const QString id = nodeAt(event->position().toPoint());
        if (!event->modifiers().testFlag(Qt::ShiftModifier) && !id.isEmpty() && m_selectedNodes.size() == 1
            && m_selectedNodes.contains(id) && scrollCardBy(id, delta.y())) {
            event->accept();
            return;
        }
        if (event->modifiers().testFlag(Qt::ShiftModifier) && qFuzzyIsNull(delta.x()))
            delta = QPointF(delta.y(), 0);
        panBy(delta);
    }
    event->accept();
}

void CanvasView::keyReleaseEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Space) {
        m_spaceHeld = false;
        setCursor(Qt::ArrowCursor);
    }
    QWidget::keyReleaseEvent(event);
}

} // namespace noteAppC
