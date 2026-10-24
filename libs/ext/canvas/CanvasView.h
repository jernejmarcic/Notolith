#pragma once

#include <QHash>
#include <QLineF>
#include <QPixmap>
#include <QPointF>
#include <QRectF>
#include <QSet>
#include <QString>
#include <QUndoStack>
#include <QVector>
#include <QWidget>

#include <functional>
#include <memory>

#include "CanvasDocument.h"
#include "Theme.h"

class QPainter;
class QPainterPath;

namespace noteAppC {

/// The section of `text` a file card's subpath names (see CanvasRender.cpp).
QString canvasSectionOf(const QString &text, const QString &subpath);

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
    /// A rename or move: save() goes to `path` from now on.
    void setPath(const QString &path) { m_document.setPath(path); }
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
    /// Zooms about a viewport point by one wheel step - wheelEvent's own
    /// Ctrl/Space branch, factored out so a read-only embedded view of this
    /// same canvas (a note's `![[x.canvas]]`) can zoom about the pointer
    /// without duplicating the maths.
    void zoomAbout(const QPoint &viewportPoint, int angleDeltaY);
    /// Pans by a viewport-pixel delta - wheelEvent's own plain branch,
    /// factored out for the same reason; also what a drag over the
    /// embedded view above uses to pan, one incremental delta at a time.
    void panBy(const QPointF &pixelDelta);
    /// Zooms and pans so every node shows, at most at 100%.
    void fitToContent();
    /// The canvas-space box around every node; null for an empty canvas.
    QRectF contentBounds() const;
    /// Draws every card's content however small it is on screen, for a
    /// canvas drawn once into a note rather than panned about.
    void setAlwaysDrawContent(bool always) { m_alwaysDrawContent = always; }
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

    /// Obsidian's two canvas toggles. Snap to objects (on by default): while a
    /// card is moved or resized its edges and centre jump onto another card's
    /// within a few screen pixels, with a guide line drawn along the match.
    /// Snap to grid (off by default): what is not snapped to a card lands on
    /// the dot grid. Set live from the settings page.
    void setSnapToObjects(bool on) { m_snapToObjects = on; }
    void setSnapToGrid(bool on) { m_snapToGrid = on; }
    bool snapToObjects() const { return m_snapToObjects; }
    bool snapToGrid() const { return m_snapToGrid; }

    /// For tests: how far the selected card's content is scrolled and the
    /// furthest it can go, in canvas pixels (0 when no card is scrolled),
    /// and how many times its hidden scroll editor has been built.
    qreal cardScrollOffset() const;
    qreal cardScrollMaximum() const;
    quint64 cardScrollEditorBuilds() const { return m_scrollBuilds; }

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
    std::unique_ptr<MarkdownEditor> makeContentEditor(const QString &text, const QSize &physicalSize) const;
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
    /// What a text or note card shows, as Markdown; false for every other card.
    bool markdownTextOf(const CanvasNode &node, QString *text) const;
    static QString contentSignature(const CanvasNode &node);
    /// Whether the hidden scroll editor still shows `node`: it is the one
    /// selected card, and neither its content nor its size has changed since.
    bool scrollEditorShows(const CanvasNode &node) const;
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
    struct SnapResult {
        QPointF shift;
        bool objectX = false, objectY = false; ///< snapped to a card, not the grid
        qreal lineX = 0, lineY = 0;
    };
    void buildSnapLines();
    /// How far to move a box whose edges are at `xs` / `ys` (the first of each
    /// is the one the grid takes) to land on a neighbour or the grid.
    SnapResult snapShift(const qreal *xs, int nx, const qreal *ys, int ny) const;
    void setGuides(const QRectF &box, const SnapResult &snap);
    /// Scrolls the selected card under `pos` by a wheel delta; false when it
    /// has nothing to scroll, so the wheel pans instead.
    bool scrollCardBy(const QString &id, qreal screenDeltaY);
    void dropScrollEditor();
    static QRectF applyResize(const QRectF &start, HandleKind handle, const QPointF &canvasDelta);
    Qt::CursorShape cursorFor(HandleKind kind) const;
    void activateIfFileNode(const QString &id);
    void positionTextEditor();
    void clampZoom();

    static constexpr qreal kResizeHandleSize = 7.0;
    static constexpr qreal kResizeHitRadius = 6.0;
    static constexpr qreal kConnectHandleOffset = 12.0;
    static constexpr qreal kConnectHitRadius = 7.0;
    /// How close, in screen pixels, a dragged edge has to come to snap.
    static constexpr qreal kSnapReachPx = 6.0;
    static constexpr qreal kGridSpacingCanvas = 32.0;
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
    bool m_alwaysDrawContent = false;
    bool m_snapToObjects = true;
    bool m_snapToGrid = false;

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
        QRectF startBox;     ///< Move: the union of startRects
        /// Where other cards have an edge or centre, sorted by pos, with the
        /// extent of that card along the line - built once per drag, only
        /// when snapping to objects, and searched by binary search.
        struct SnapLine {
            qreal pos, lo, hi;
        };
        QVector<SnapLine> snapX, snapY;
        QVector<QLineF> guides; ///< canvas space, drawn while a drag is snapped
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

    /// A selected text or note card whose content is taller than the card
    /// scrolls under the wheel: a hidden Reading-mode editor holds the whole
    /// note and each frame grabs the scrolled part, bypassing the cache
    /// (which stays valid, at scroll 0, for after the card is deselected).
    std::unique_ptr<MarkdownEditor> m_scrollEditor;
    QString m_scrollNodeID;
    QString m_scrollSignature;
    qreal m_scrollY = 0; ///< physical pixels, i.e. canvas pixels x kContentScale
    quint64 m_scrollBuilds = 0;
};

} // namespace noteAppC
