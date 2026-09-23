#pragma once

#include <QHash>
#include <QPixmap>
#include <QPointF>
#include <QRectF>
#include <QSet>
#include <QString>
#include <QUndoStack>
#include <QWidget>

#include <functional>

#include "CanvasDocument.h"
#include "Theme.h"

class QPainter;
class QPainterPath;

namespace noteAppC {

class MarkdownEditor;

/// Obsidian's infinite canvas over a JSON Canvas 1.0 file: pan and zoom,
/// nodes culled to the viewport, groups behind nodes, edges as curves with
/// arrowheads and labels, level of detail when zoomed far out, and the full
/// set of mouse/keyboard edits - select, move, resize, delete, create, edit
/// text in place, connect nodes - each undoable through undoStack().
///
/// A node's rendered content (a text node's Markdown, a note or image a file
/// node points at) is drawn once into a cached QPixmap and reused every
/// frame; the cache is checked against the node's current text/size on each
/// paint rather than invalidated from the edit side, so there is exactly one
/// place a stale image could hide and it is the one place that is checked.
class CanvasView : public QWidget {
    Q_OBJECT

public:
    explicit CanvasView(QWidget *parent = nullptr);
    ~CanvasView() override;

    void setTheme(const Theme &theme);
    const Theme &theme() const { return m_theme; }

    bool load(const QString &path, QString *error = nullptr);
    /// Writes back to the path load() used, or `path` the first time a new,
    /// not-yet-saved canvas is written.
    bool save(const QString &path = QString(), QString *error = nullptr);
    QString path() const { return m_document.path(); }
    bool isModified() const { return !m_undoStack.isClean(); }

    /// Turns a file node's `file` (a vault-relative path, the way a link is)
    /// into an absolute path to read its content from, or an empty string
    /// when it does not resolve - the window's job, the same shape as
    /// MarkdownEditor::setLinkResolver. Only used to render a node's content;
    /// noteActivated always carries the raw, unresolved relative path, the
    /// way every other pane's noteActivated does, and leaves resolving it to
    /// whoever opens it.
    void setFileResolver(std::function<QString(const QString &)> resolver);

    CanvasDocument &document() { return m_document; }
    const CanvasDocument &document() const { return m_document; }

    QUndoStack *undoStack() { return &m_undoStack; }

    qreal zoom() const { return m_zoom; }
    /// Changes the zoom level about the viewport's centre; clamped to the
    /// same range the wheel is. For tests and for a future zoom control.
    void setZoom(qreal zoom);
    /// Zooms and pans so every node shows, at most at 100%.
    void fitToContent();
    /// The canvas-space point that maps to the viewport's top-left pixel.
    QPointF origin() const { return m_origin; }

    QPointF mapToCanvas(const QPoint &viewportPoint) const;
    QPoint mapFromCanvas(const QPointF &canvasPoint) const;
    /// Sub-pixel precision version of mapFromCanvas, for a curve's control
    /// points - rounding those to whole pixels is what made edges judder
    /// while panning at anything but integer zoom.
    QPointF mapFromCanvasF(const QPointF &canvasPoint) const;
    /// The canvas-space rectangle currently visible in the viewport.
    QRectF viewportCanvasRect() const;

    void selectAll();
    void clearSelection();
    /// Removes every selected node (and, cascading, every edge touching one)
    /// and every selected edge, as one undo step. What Del does.
    void deleteSelection();
    QStringList selectedNodeIDs() const { return m_selectedNodes.values(); }
    QStringList selectedEdgeIDs() const { return m_selectedEdges.values(); }
    void selectNode(const QString &id, bool extend = false);

    /// The topmost node under a viewport point, or an empty string.
    QString nodeAt(const QPoint &viewportPoint) const;
    /// The edge under a viewport point (its curve, not its bounding box), or
    /// an empty string.
    QString edgeAt(const QPoint &viewportPoint) const;

    /// Creates an empty text node at a canvas point, default sized, and
    /// selects it - what double-clicking empty space does. Returns its id.
    QString createTextNodeAt(const QPointF &canvasPoint);

    /// Opens the in-place editor over a text node - what double-clicking one
    /// does. False when `id` names no node, or one that is not a text node.
    bool beginEditingText(const QString &id);
    bool isEditingText() const { return m_textEditor != nullptr; }
    QString editingNodeID() const { return m_editingNodeID; }
    /// Commits the overlay's text as an EditTextCommand (only if it changed)
    /// and closes it - what Escape and a click outside it both do.
    void commitEditingText();
    /// Closes the overlay without writing anything back.
    void cancelEditingText();

signals:
    /// A file node was double-clicked or Ctrl-clicked: `relativePath` is its
    /// `file` field, exactly as BacklinksPane/BookmarksPane/etc. already hand
    /// MainWindow::openRelative a relative path to resolve and open.
    void noteActivated(const QString &relativePath);
    void modificationChanged(bool modified);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void keyReleaseEvent(QKeyEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void leaveEvent(QEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    // --- CanvasRender.cpp ---
    void paintGrid(QPainter &painter) const;
    void paintGroups(QPainter &painter) const;
    void paintEdges(QPainter &painter) const;
    void paintNodes(QPainter &painter) const;
    void paintOverlay(QPainter &painter) const;
    void paintNodeChrome(QPainter &painter, const CanvasNode &node, const QRectF &screenRect) const;
    QPixmap contentPixmap(const CanvasNode &node) const;
    QPixmap renderMarkdownContent(const QString &text, const QSize &physicalSize) const;
    QPixmap renderImageContent(const QString &absolutePath, const QSize &physicalSize) const;
    QPixmap renderLinkCard(const QString &url, const QSize &physicalSize) const;
    QPixmap renderFileCard(const QString &fileName, const QSize &physicalSize, bool missing) const;
    QPainterPath edgePath(const CanvasEdge &edge, QPointF *labelAnchor) const;
    static QPointF sideAnchor(const QRectF &rect, const QString &side);
    static QPointF sideNormal(const QString &side);
    /// `explicitSide` if it is not empty, else whichever side of `ownerRect`
    /// faces `otherRect` - how an edge with no recorded side is routed.
    QString effectiveSide(const QString &explicitSide, const QRectF &ownerRect, const QRectF &otherRect) const;
    bool isNodeVisible(const CanvasNode &node, QRectF *screenRect) const;
    /// `node`'s rect as it should be drawn/hit-tested right now: the live,
    /// not-yet-committed rect while it is being dragged, else the document's.
    QRectF currentRect(const CanvasNode &node) const;

    // --- CanvasEditing.cpp ---
    enum class HandleKind {
        None, Move,
        ResizeTop, ResizeRight, ResizeBottom, ResizeLeft,
        ResizeTopLeft, ResizeTopRight, ResizeBottomLeft, ResizeBottomRight,
        Connect
    };
    struct HitResult {
        HandleKind kind = HandleKind::None;
        QString nodeID;
        QString side; ///< set when kind == Connect
    };
    HitResult hitTest(const QPoint &viewportPoint) const;
    void beginPan(const QPoint &viewportPoint);
    void updatePan(const QPoint &viewportPoint);
    void beginRubberBand(const QPoint &viewportPoint, bool extend);
    void updateRubberBand(const QPoint &viewportPoint);
    void finishRubberBand();
    void beginMove(const HitResult &hit, const QPoint &viewportPoint, Qt::KeyboardModifiers modifiers);
    void updateMoveOrResize(const QPoint &viewportPoint);
    void finishMoveOrResize();
    void beginConnect(const HitResult &hit);
    void updateConnect(const QPoint &viewportPoint);
    void finishConnect(const QPoint &viewportPoint);
    void updateHover(const QPoint &viewportPoint);
    static QRectF applyResize(const QRectF &start, HandleKind handle, const QPointF &canvasDelta);
    Qt::CursorShape cursorFor(HandleKind kind) const;
    void activateIfFileNode(const QString &id);
    void positionTextEditor();
    void clampZoom();

    static constexpr qreal kResizeHandleSize = 7.0;
    static constexpr qreal kResizeHitRadius = 6.0;
    static constexpr qreal kConnectHandleOffset = 12.0;
    static constexpr qreal kConnectHitRadius = 7.0;
    /// Below this on-screen width or height, a node draws as a plain box
    /// instead of its content - too small to read either way, and skipping
    /// the content pixmap here is most of the saving when zoomed far out.
    static constexpr qreal kLodMinPixels = 56.0;
    /// Cached content is rendered once at this many pixels per canvas pixel,
    /// so it stays sharp up to a 2x zoom-in on the node's own size without
    /// being rebuilt - only a resize or an edit invalidates it, never a zoom.
    static constexpr int kContentScale = 2;

    CanvasDocument m_document;
    Theme m_theme;
    std::function<QString(const QString &)> m_fileResolver;
    QUndoStack m_undoStack;

    qreal m_zoom = 1.0;
    QPointF m_origin{0, 0};
    bool m_fitPending = false; ///< loaded, not painted yet: frame it first

    QSet<QString> m_selectedNodes;
    QSet<QString> m_selectedEdges;
    QString m_hoveredNode;
    QString m_hoveredConnectSide;
    QString m_hoveredEdge;
    bool m_spaceHeld = false;

    enum class DragKind { None, Pan, RubberBand, Move, Resize, Connect };
    struct DragState {
        DragKind kind = DragKind::None;
        QPoint startViewport;
        QPointF startOrigin;
        QPointF startCanvas;
        QPointF currentCanvas;
        bool additive = false; ///< rubber band: add to the existing selection
        bool moved = false;    ///< past the click-vs-drag threshold
        QHash<QString, QRectF> startRects;
        QPointF liveDelta;   ///< Move: current offset from startRects, canvas space
        QRectF liveRect;     ///< Resize: the single node's current rect
        HandleKind resizeHandle = HandleKind::None;
        QString connectFromNode;
        QString connectFromSide;
        QString connectHoverTarget;
    } m_drag;

    MarkdownEditor *m_textEditor = nullptr;
    QString m_editingNodeID;

    struct ContentCacheEntry {
        QPixmap pixmap;
        QString signature;
    };
    mutable QHash<QString, ContentCacheEntry> m_contentCache;
};

} // namespace noteAppC
