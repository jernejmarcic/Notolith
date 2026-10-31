#pragma once

#include <QImage>
#include <QPixmap>
#include <QPointer>
#include <QPolygonF>
#include <QTransform>
#include <QTimer>
#include <QWidget>

#include <functional>
#include <map>
#include <memory>
#include <vector>

#include "InkDocument.h"
#include "InkInput.h"
#include "TileCache.h"

class QPainter;

namespace noteAppC::ink {

/// The drawing surface (spec section 7, Canvas): a plain QWidget sized to the
/// document at the zoom, living inside InkView's QScrollArea. Tablet events
/// do not route through QAbstractScrollArea::viewportEvent, which is why the
/// canvas is the widget and not the scroll area. Paints committed items from
/// the document through the tile cache (TileCache.h: 512 px tiles rendered on
/// the pool, never in paintEvent) and the live stroke from an overlay image
/// that only the newest segment is rasterised into.
class InkCanvas : public QWidget {
    Q_OBJECT
public:
    enum class Tool { Pen, Eraser, Lasso, Shape, Text };
    Q_ENUM(Tool)
    /// What the eraser takes: the whole stroke it touches, or only the segments it touches (the rest of the
    /// stroke survives as one or two new strokes). Shapes, text and images are always taken whole.
    enum class EraserMode { Stroke, Split };
    Q_ENUM(EraserMode)
    /// What the select tool draws: a free lasso path or a rectangle (items inside it are selected on release).
    enum class SelectShape { Lasso, Rect };
    Q_ENUM(SelectShape)

    explicit InkCanvas(QWidget *parent = nullptr);

    InkDocument &document() { return m_document; }
    const InkDocument &document() const { return m_document; }
    void documentReplaced();

    double zoom() const { return m_zoom; }
    void setZoom(double zoom);
    Tool tool() const { return m_tool; }
    void setTool(Tool tool);
    StrokeStyle penStyle() const { return m_penStyle; }
    void setPenStyle(const StrokeStyle &style) { m_penStyle = style; }
    EraserMode eraserMode() const { return m_eraserMode; }
    void setEraserMode(EraserMode mode);
    double eraserWidth() const { return m_eraserWidth; }
    void setEraserWidth(double documentPx) { m_eraserWidth = documentPx; }
    SelectShape selectShape() const { return m_selectShape; }
    void setSelectShape(SelectShape shape);
    /// Holding kHeldLassoButton on the pen in the Pen tool selects for that stroke only. The glyph studio turns it
    /// off: a glyph cell has no selection and uses the barrel click to save.
    bool isHeldLassoEnabled() const { return m_heldLassoEnabled; }
    void setHeldLassoEnabled(bool on) { m_heldLassoEnabled = on; }
    /// What the Shape tool draws: Line, Arrow, Rect or Ellipse (a Polyline or Polygon is refused: no tool draws them).
    ShapeKind shapeKind() const { return m_shapeKind; }
    void setShapeKind(ShapeKind kind);
    /// A drag of less than this many widget px commits no shape: a tap is not a line.
    static constexpr double kMinShapeDragPx = 3.0;
    bool drawWithMouse() const { return m_drawWithMouse; }
    /// With it off, a left drag of the mouse scrolls the owner's scroll area (panRequested; a hand cursor) and Ctrl+drag
    /// still selects: the mouse is for moving around, the pen is for ink. With it on the mouse draws like the pen.
    void setDrawWithMouse(bool on);
    void setRawInput(bool raw) { m_input.setRawInput(raw); }

    /// Widget px <-> document px. The widget starts at the extent's top-left (an Infinite extent may sit at
    /// negative document coordinates), and in ContinuousVertical every page after the first is pushed down
    /// by kPageGap: a widget point in a gap maps to the bottom edge of the page above.
    QPointF toDocument(const QPointF &widgetPos) const;
    QPointF toWidget(const QPointF &documentPos) const;
    QSize sizeHint() const override;
    bool isDrawing() const { return m_drawing != Drawing::None; }
    bool isSelectGesture() const { return m_drawing == Drawing::Select || m_drawing == Drawing::Move || m_drawing == Drawing::Scale; }
    QSize liveOverlaySize() const { return m_live.size(); } ///< device px; a test hook for the overlay bound

    /// The tile cache budget in MB (the setting `ink.tileCacheMB`), clamped to kMinTileCacheMB..kMaxTileCacheMB.
    static constexpr int kMinTileCacheMB = 8;
    static constexpr int kMaxTileCacheMB = 1024;
    void setTileCacheMB(int mb);
    static constexpr int kZoomSettleMs = 400; ///< the tiles follow the zoom this long after its last change; until then the current ones are scaled
    /// Tile test hooks: the budget in bytes, the zoom the tiles were last rendered for (it follows the zoom 400 ms
    /// after the last change), the renders in flight, the tiles that no longer show the document, and a function run
    /// on the pool thread at the start of every tile render (a test blocks it to prove a paint never waits for one).
    qsizetype tileCacheBytes() const;
    double tilesScale() const;
    int pendingTiles() const;
    int staleTileCount() const;
    void setTileJobHook(std::function<void()> hook);
    bool liveHeld() const { return m_holdLive; } ///< the live image still stands in for the committed stroke until its tiles arrive
    /// The selection (Tool::Lasso): the `t`s of the selected items, and the box that holds them (document px; null when
    /// nothing is selected). Moving, resizing and deleting are one history step each; copy and paste go through the
    /// application clipboard (InkTools.h, kClipboardMime). Each returns false when it did nothing.
    const std::vector<qint64> &selection() const { return m_selection; }
    QRectF selectionBounds() const { return m_selectionBounds; }
    void clearSelection();
    bool deleteSelection();
    bool copySelection();
    bool paste(); ///< at the centre of the visible canvas, +20 px right and down, as new items; they become the selection
    /// Text boxes (Tool::Text; spec section 7: no double-click needed with a pen). A press with the Text tool edits the
    /// box under it, or starts a new one there; the editor is a plain-text child (objectName inkTextEditor) over the
    /// box. Escape, a press elsewhere, a tool change, undo, hiding and losing focus end it and commit it as one history
    /// step (an unchanged text commits nothing, an emptied box is deleted, a new box with no text is never added).
    /// The box is drawn through `renderer` from its Markdown: on the UI thread in paintEvent, never in a tile job,
    /// never during a pen gesture, and cached per item (setTextRenderer; InkView builds the host's MarkdownEditor).
    /// `size` is the box's size in document px: the layout is the same at every zoom. `scale` is device px per
    /// document px: the settled zoom (rounded up to a step, so a zoom-out reuses the picture) times the pixel ratio,
    /// kept so that neither side passes kMaxTextDevicePx. The renderer returns a pixmap of size * scale device px
    /// whose devicePixelRatio is `scale`, so that painting it is one pixel to one pixel. It is given at most
    /// kMaxTextRenderChars of the Markdown, cut at a line end and ended with an ellipsis; the stored text is whole.
    /// The pictures are cached up to kTextCacheBytes, the least recently painted going first.
    using TextRenderer = std::function<QPixmap(const QString &markdown, const QSizeF &size, qreal scale)>;
    void setTextRenderer(TextRenderer renderer);
    static constexpr int kMaxTextRenderChars = 16384;
    static constexpr int kMaxTextDevicePx = 4096;
    static constexpr qsizetype kTextCacheBytes = 32 * 1024 * 1024;
    /// A hard ceiling on all cached pictures together: what a paint uses is exempt from the eviction above, so without
    /// it a view of many large boxes would end up holding every one of them. Past it a box keeps its stale picture or
    /// the placeholder (and asks for no further paint).
    static constexpr qsizetype kTextCacheCeilingBytes = 4 * kTextCacheBytes;
    qsizetype textCacheBytes() const; ///< what the cached pictures of text boxes hold
    /// The editor over a box. Without a factory it is a QPlainTextEdit (inkcore links no editor); a host installs its
    /// own: `create` makes a parentless widget holding `markdown` (caret at the end), `read` gives back what it holds,
    /// and `setZoom` (optional) is called when the canvas zoom changes while it is open. The canvas names it
    /// inkTextEditor, places it over the box, and ends the edit on Escape or when it loses focus (a child of it taking
    /// the focus is not losing it).
    struct TextEditorFactory {
        std::function<QWidget *(const QString &markdown)> create;
        std::function<QString(const QWidget *editor)> read;
        std::function<void(QWidget *editor, double zoom)> setZoom;
    };
    void setTextEditorFactory(TextEditorFactory factory);
    void invalidateTextCache(); ///< the renderer would draw differently now (theme, fonts, page colour)
    static constexpr QSizeF kNewTextBoxSize{260, 72}; ///< document px
    static constexpr double kMaxTextBoxSide = 4096;   ///< document px: a larger box is laid out at this size and stretched
    bool beginEditingText(qint64 t); ///< false for a read-only canvas or an item that is not a text box
    bool isEditingText() const { return !m_textEdit.isNull(); }
    qint64 editingTextItem() const { return m_textEditT; } ///< 0: none; -1: a new box not yet in the document
    void commitText();
    /// The owner is being destroyed: closes the editor without committing the box or saying anything. A destruction
    /// hides the canvas and takes the focus from the editor while the owner's members are already gone, and the commit
    /// that follows would call back into it. The typed text is dropped: a teardown is a "Don't save".
    void discardTextEdit();
    bool isTextEditDirty() const; ///< the open editor holds text that is not the box's yet (the owner counts it as unsaved and commits it before a save)

    /// A read-only canvas (a file written by a newer Notolith) ignores the pen,
    /// mouse drawing and the edit keys but keeps touch pan, wheel and zoom:
    /// disabling the widget would block those too.
    void setReadOnly(bool on); ///< true first ends a stroke in flight (kept)
    bool isReadOnly() const { return m_readOnly; }

    /// Pressure is divided by a learned maximum per QPointingDevice::name()
    /// (PressureNormaliser, InkInput.h). Tablet pressure only: a mouse stroke stays 0.5.
    double learnedPressure(const QString &device) const;
    void setLearnedPressure(const QString &device, double max);
    void setPressureSeed(double seed) { m_pressureSeed = seed; } ///< for a device nothing is stored for

public slots:
    bool undo();
    bool redo();

signals:
    void documentEdited();
    void toolChanged(Tool tool);
    void eraserModeChanged(EraserMode mode);
    void selectShapeChanged(SelectShape shape);
    void selectionChanged();
    void shapeKindChanged(ShapeKind kind);
    void learnedPressureChanged(const QString &device, double max);
    /// What the open text editor holds may now differ from the box (typing), or the editor just closed: the owner
    /// asks isTextEditDirty() again (its "modified" mark follows the typing, not only the commit).
    void textEditChanged();
    void zoomChanged(double zoom);
    /// A short line for the owner's status bar: why an action did nothing (a copy or paste over the clipboard cap).
    void notice(const QString &message);
    void panRequested(QPoint deltaWidgetPx);
    /// The extent grew left or up (Infinite): the content moved this far (widget px) right and down inside the
    /// widget. The scroll area's owner adds it to the scroll position so the view stays on the same ink.
    void originShifted(QPoint deltaWidgetPx);

protected:
    void tabletEvent(QTabletEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    /// Ctrl+wheel zooms by 10 % a step (through the settle timer); the plain wheel is left to the scroll area.
    void wheelEvent(QWheelEvent *event) override;
    bool event(QEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
    void focusOutEvent(QFocusEvent *event) override;
    void hideEvent(QHideEvent *event) override;
    void changeEvent(QEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    enum class Drawing { None, Pen, Eraser, Select, Move, Scale, Shape };
    void pressAt(const QPointF &widgetPos, double pressure, double seconds, bool erase, bool select = false);
    void updateShape(const QPointF &documentPos);
    void finishShape(bool keep);
    QRect shapePaintRect() const; ///< widget px: the shape in flight
    void paintShapeOver(QPainter &painter, const Shape &shape); ///< the shape in flight, or the one held until its tiles arrive
    void pressText(const QPointF &documentPos);
    void openTextEditor(qint64 t, const QRectF &rect, const QString &markdown);
    QString closeTextEditor(); ///< tears the editor down, committing nothing; returns what it held
    void layoutTextEditor();
    const QPixmap *textPixmap(const Item &item); ///< the cached picture of a text box, rendered now if it is missing and the pen is idle
    void paintUIItems(QPainter &painter, const QRect &area); ///< text boxes and the image placeholder: UI thread, over the tiles
    void pressSelect(const QPointF &widgetPos);
    void finishSelect(Drawing mode, bool keep);
    void dragSelection(const QPointF &documentPos);
    void updateLasso(const QPointF &documentPos);
    void setSelection(std::vector<qint64> items);
    void updateCursor();
    int handleAt(const QPointF &widgetPos) const; ///< 0..7 clockwise from the top-left, or -1
    QRect selectionPaintRect() const;             ///< widget px: the box, its handles and a margin; null when nothing is selected
    QRect lassoPaintRect() const;                 ///< widget px: the lasso or rectangle in flight
    void recomputeSelectionBounds();
    void paintSelection(QPainter &painter);
    void moveTo(const QPointF &widgetPos, double pressure, double seconds);
    void releaseAt(const QPointF &widgetPos, double seconds);
    void finishStroke(bool keep);
    void prepareLiveOverlay();
    void rasteriseNewSegments();
    void eraseSweep(const QPointF &fromDocumentPos, const QPointF &toDocumentPos);
    void finishEdit();
    QRect widgetRect(const QRectF &documentRect) const;
    double pageGap() const; ///< document px between pages: kPageGap in ContinuousVertical, else 0
    QRect pageWidgetRect(int index) const; ///< widget px of page `index` (ContinuousVertical)
    std::pair<int, int> visiblePages(const QRect &area) const; ///< first and last page the widget-px `area` touches
    /// Paints the tiles covering the widget-px `area` (and asks the pool for the ones that are missing or stale).
    void drawTiles(QPainter &painter, const QRect &area);
    std::function<QImage()> tileJob(int tx, int ty) const;
    void invalidateTiles(const QRectF &documentRect);
    void settleZoom();
    void onTileReady(int tx, int ty);
    bool tilesFresh(const QRect &widgetArea) const;
    PressureNormaliser &normaliserFor(const QString &device);

    InkDocument m_document;
    InkInput m_input;
    Tool m_tool = Tool::Pen;
    StrokeStyle m_penStyle;
    double m_eraserWidth = 20.0;
    EraserMode m_eraserMode = EraserMode::Stroke;
    double m_zoom = 1.0;
    QPointF m_origin;            ///< document px: the extent's top-left as documentReplaced() last saw it
    bool m_drawWithMouse = false;
    bool m_mousePanning = false; ///< a left drag of the mouse that scrolls (drawWithMouse off); m_lastMouse is its previous global position
    QPointF m_lastMouse;
    Drawing m_drawing = Drawing::None;
    bool m_tabletActive = false; ///< Qt synthesises mouse events from unaccepted tablet events; we accept, this guards the rest
    QImage m_live;               ///< the live stroke, transparent: the visible part of the canvas only (m_liveRect), at the screen's pixel ratio
    QRect m_liveRect;            ///< widget px: where m_live sits
    QRect m_liveDirty;           ///< widget px: what the stroke painted into it, the only part cleared at the next pen-down
    QPointF m_lastPos;           ///< the last received sample of the stroke in flight (widget px, pressure, seconds): where it ends if the release never comes
    double m_lastPressure = 0.5;
    double m_lastSeconds = 0;
    QPointF m_lastErase;         ///< document px: the previous eraser sample, the other end of the swept segment
    QRectF m_lastPrediction;     ///< document px: the predicted tail as last painted; repainted when it moves and at pen-up
    bool m_erasedSomething = false;
    ShapeKind m_shapeKind = ShapeKind::Line;
    Shape m_shapeDraft;          ///< the shape being dragged out: two points, the pen style
    SelectShape m_selectShape = SelectShape::Lasso;
    bool m_heldLassoEnabled = true;
    std::vector<qint64> m_selection;
    QRectF m_selectionBounds;         ///< document px
    QPolygonF m_lasso;                ///< document px: the path of the select drag in flight (Rect: its start point and the pointer)
    std::vector<Item> m_dragOriginals; ///< the selected items as the Move or Scale drag found them
    QRectF m_dragBounds;              ///< document px: the selection box when the drag began
    QPointF m_dragStart;              ///< document px
    int m_dragHandle = -1;
    QTransform m_dragTransform;       ///< the mapping the items currently have relative to m_dragOriginals (identity: not moved)
    bool m_dragMoved = false;
    bool m_readOnly = false;
    QPointF m_lastTouch;
    bool m_touchPanning = false;
    std::map<QString, PressureNormaliser> m_pressure; ///< by QPointingDevice::name()
    double m_pressureSeed = 1.0;
    TileCache m_tiles;
    qsizetype m_userCacheBytes = TileCache::kDefaultMaxBytes; ///< the setting; the cache itself may hold more while the visible tiles need it
    QTimer m_zoomTimer;          ///< single shot, kZoomSettleMs after the last zoom change
    double m_tilesZoom = 1.0;    ///< the zoom the tiles are rendered for; m_zoom runs ahead of it during a pinch
    double m_ghostScale = 0;     ///< the zoom before the last settle: images rendered at it are still painted, scaled, until replaced
    std::function<void()> m_tileJobHook;
    bool m_holdLive = false;     ///< after pen-up the live image stays until the tiles under the stroke have it too
    std::shared_ptr<const Shape> m_holdShape; ///< a committed shape stays painted over the tiles until the ones under it have it too
    QRect m_holdRect;            ///< widget px
    TextRenderer m_textRenderer;
    struct TextPixmap { ///< what a box was drawn from: a move keeps the picture, new text or a new size or page colour does not
        QString markdown;
        QSize size;
        qint64 scaleKey = 0; ///< the scale it was drawn at, x 1000 rounded
        QColor background;
        QPixmap pixmap;
        quint64 used = 0;    ///< m_textTick when it was last painted
    };
    std::map<qint64, TextPixmap> m_textCache;
    quint64 m_textTick = 0;
    qsizetype m_textPaintBytes = 0; ///< what the pictures the current paint has rendered hold: past kTextCacheBytes it renders no more
    qsizetype m_textHeldBytes = 0;  ///< what every cached picture holds, kept while the paint runs (textCacheBytes() is a scan)
    bool m_textSkipped = false;     ///< the current paint left a box unrendered for the budget: it asks for one more paint
    TextEditorFactory m_textEditorFactory;
    QString readTextEditor() const; ///< what the open editor holds
    double textScale(const QSizeF &logical) const; ///< device px per document px for a box of that size, at the settled zoom
    QPointer<QWidget> m_textEdit; ///< the editor over the box in flight (a QPlainTextEdit, or the factory's)
    qint64 m_textEditT = 0;
    QRectF m_textEditRect;       ///< document px: where the box is (a new box is not in the document yet)
    QString m_textEditOriginal;  ///< what the editor held when it opened (its own reading of the box's Markdown)
    double m_textEditZoom = 0;   ///< the zoom the editor was last told (0: not yet)
};

} // namespace noteAppC::ink
