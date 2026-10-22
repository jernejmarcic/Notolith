#pragma once

#include <QImage>
#include <QTimer>
#include <QWidget>

#include <functional>
#include <map>

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
    double eraserWidth() const { return m_eraserWidth; }
    void setEraserWidth(double documentPx) { m_eraserWidth = documentPx; }
    bool drawWithMouse() const { return m_drawWithMouse; }
    void setDrawWithMouse(bool on) { m_drawWithMouse = on; }
    void setRawInput(bool raw) { m_input.setRawInput(raw); }

    /// Widget px <-> document px. The widget starts at the extent's top-left (an Infinite extent may sit at
    /// negative document coordinates), and in ContinuousVertical every page after the first is pushed down
    /// by kPageGap: a widget point in a gap maps to the bottom edge of the page above.
    QPointF toDocument(const QPointF &widgetPos) const;
    QPointF toWidget(const QPointF &documentPos) const;
    QSize sizeHint() const override;
    bool isDrawing() const { return m_drawing != Drawing::None; }
    QSize liveOverlaySize() const { return m_live.size(); } ///< device px; a test hook for the overlay bound (I4)

    /// The tile cache budget in MB (the setting `ink.tileCacheMB`, R5), clamped to kMinTileCacheMB..kMaxTileCacheMB.
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
    /// A read-only canvas (a file written by a newer Notolith) ignores the pen,
    /// mouse drawing and the edit keys but keeps touch pan, wheel and zoom:
    /// disabling the widget would block those too.
    void setReadOnly(bool on); ///< true first ends a stroke in flight (kept)
    bool isReadOnly() const { return m_readOnly; }

    /// R14: pressure is divided by a learned maximum per QPointingDevice::name()
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
    void learnedPressureChanged(const QString &device, double max);
    void zoomChanged(double zoom);
    void panRequested(QPoint deltaWidgetPx);
    /// The extent grew left or up (Infinite): the content moved this far (widget px) right and down inside the
    /// widget. The scroll area's owner adds it to the scroll position so the view stays on the same ink.
    void originShifted(QPoint deltaWidgetPx);

protected:
    void tabletEvent(QTabletEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    bool event(QEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
    void focusOutEvent(QFocusEvent *event) override;
    void hideEvent(QHideEvent *event) override;
    void changeEvent(QEvent *event) override;

private:
    enum class Drawing { None, Pen, Eraser };
    void pressAt(const QPointF &widgetPos, double pressure, double seconds, bool erase);
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
    double m_zoom = 1.0;
    QPointF m_origin;            ///< document px: the extent's top-left as documentReplaced() last saw it
    bool m_drawWithMouse = false;
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
    bool m_readOnly = false;
    QPointF m_lastTouch;
    bool m_touchPanning = false;
    std::map<QString, PressureNormaliser> m_pressure; ///< by QPointingDevice::name()
    double m_pressureSeed = 1.0;
    TileCache m_tiles;
    QTimer m_zoomTimer;          ///< single shot, kZoomSettleMs after the last zoom change
    double m_tilesZoom = 1.0;    ///< the zoom the tiles are rendered for; m_zoom runs ahead of it during a pinch
    double m_ghostScale = 0;     ///< the zoom before the last settle: images rendered at it are still painted, scaled, until replaced
    std::function<void()> m_tileJobHook;
    bool m_holdLive = false;     ///< after pen-up the live image stays until the tiles under the stroke have it too
    QRect m_holdRect;            ///< widget px
};

} // namespace noteAppC::ink
