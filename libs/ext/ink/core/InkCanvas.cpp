// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej
#include "InkCanvas.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <utility>

#include <QApplication>
#include <QFocusEvent>
#include <QGestureEvent>
#include <QKeyEvent>
#include <QLinearGradient>
#include <QLineF>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QPinchGesture>
#include <QPlainTextEdit>
#include <QScopeGuard>
#include <QTabletEvent>
#include <QTouchEvent>
#include <QWheelEvent>

#include "InkTools.h"
#include "PagePatterns.h"
#include "StrokeOutline.h"

namespace noteAppC::ink {

namespace {

constexpr double kPageEdgeSlack = 1e-9; ///< relative: page edges are page * height, not exact for a fractional height or zoom
constexpr int kShadowDepth = 4;       ///< widget px: the shadow under a page in ContinuousVertical
constexpr int kMaxOverlaySide = 8192; ///< device px: the live overlay never grows past this, whatever the widget's size

double cross(const QPointF &o, const QPointF &p, const QPointF &q)
{
    return (p.x() - o.x()) * (q.y() - o.y()) - (p.y() - o.y()) * (q.x() - o.x());
}

double pointToSegmentSquared(const QPointF &p, const QPointF &a, const QPointF &b)
{
    const QPointF ab = b - a;
    const double len2 = ab.x() * ab.x() + ab.y() * ab.y();
    const double t = len2 > 0 ? std::clamp(((p.x() - a.x()) * ab.x() + (p.y() - a.y()) * ab.y()) / len2, 0.0, 1.0) : 0.0;
    const QPointF d = p - (a + ab * t);
    return d.x() * d.x() + d.y() * d.y();
}

/// The squared distance between segments ab and cd: zero when they cross, otherwise the
/// closest of the four endpoint-to-segment distances.
double segmentDistanceSquared(const QPointF &a, const QPointF &b, const QPointF &c, const QPointF &d)
{
    if (((cross(a, b, c) > 0) != (cross(a, b, d) > 0)) && ((cross(c, d, a) > 0) != (cross(c, d, b) > 0)))
        return 0.0;
    return std::min({pointToSegmentSquared(a, c, d), pointToSegmentSquared(b, c, d), pointToSegmentSquared(c, a, b),
                     pointToSegmentSquared(d, a, b)});
}

/// Does the eraser (segment a-b swept by radius `r`, document px) touch the ink of `stroke`? Walks
/// the polyline: no outline path is built, so a 10 000-point scribble costs microseconds a sample.
bool eraserTouches(const Stroke &stroke, const QPointF &a, const QPointF &b, double r)
{
    const auto &pts = stroke.points;
    const QRectF box = QRectF(a, b).normalized().adjusted(-r, -r, r, r);
    for (size_t i = 0; i < pts.size(); ++i) {
        const size_t j = i + 1 < pts.size() ? i + 1 : i;
        const QPointF p(pts[i].x, pts[i].y), q(pts[j].x, pts[j].y);
        const double half = 0.5 * std::max(stroke.widthAt(i), stroke.widthAt(j));
        if (std::max(p.x(), q.x()) < box.left() - half || std::min(p.x(), q.x()) > box.right() + half ||
            std::max(p.y(), q.y()) < box.top() - half || std::min(p.y(), q.y()) > box.bottom() + half)
            continue; // the cheap cull: far from the capsule's box
        const double reach = r + half;
        if (segmentDistanceSquared(a, b, p, q) <= reach * reach)
            return true;
    }
    return false;
}

/// The eraser as a path: the segment a-b swept by radius `r` (a circle when a == b), filled once.
QPainterPath eraserCapsule(const QPointF &a, const QPointF &b, double r)
{
    QPainterPath path;
    path.setFillRule(Qt::WindingFill);
    path.addEllipse(a, r, r);
    if (a != b) {
        path.addEllipse(b, r, r);
        const QPointF d = b - a;
        const double length = std::hypot(d.x(), d.y());
        if (length > 0.0) {
            const QPointF side(-d.y() / length * r, d.x() / length * r);
            path.addPolygon(QPolygonF({a + side, b + side, b - side, a - side}));
            path.closeSubpath();
        }
    }
    return path;
}

/// Everything the page layout needs, by value: the canvas builds one for its own painting and hands one to every
/// tile job, which runs on a pool thread and may not touch the document.
struct Geometry {
    DocumentSettings settings;
    int pageCount = 1;
    double zoom = 1.0;

    double gap() const { return settings.layout == LayoutKind::ContinuousVertical ? InkDocument::kPageGap : 0.0; }
    bool paged() const { return gap() > 0; }
    QSize size() const ///< widget px; a QWidget cannot be larger than QWIDGETSIZE_MAX, the clamp also keeps the int conversion defined for a hostile extent
    {
        const double height = settings.extent.height() + (pageCount - 1) * gap();
        const auto px = [this](double documentPx) { return int(std::min(std::ceil(documentPx * zoom), double(QWIDGETSIZE_MAX))); };
        return QSize(px(settings.extent.width()), px(height));
    }
    QRect pageWidgetRect(int index) const
    {
        const double h = settings.format.height;
        const double top = index * (h + gap());
        const int topPx = qRound(top * zoom);
        const int bottom = std::min(qRound((top + h) * zoom), size().height()); // the last page may be cut by the extent
        return QRect(0, topPx, size().width(), bottom - topPx);
    }
    QRectF pageRect(int index) const ///< document px
    {
        if (settings.layout == LayoutKind::SemiInfinite || settings.layout == LayoutKind::Infinite)
            return settings.extent;
        return QRectF(settings.extent.x(), settings.extent.y() + index * settings.format.height, settings.format.width, settings.format.height);
    }
    std::pair<int, int> visiblePages(const QRect &area) const
    {
        const double stride = (settings.format.height + gap()) * zoom;
        const int last = pageCount - 1;
        if (!paged() || !(stride > 0))
            return {0, 0};
        return {std::clamp(int(std::floor(area.top() / stride)), 0, last), std::clamp(int(std::floor(area.bottom() / stride)), 0, last)};
    }
    /// What forEachPage does per page: `shown` is the part of the widget-px `area` on page `page`, `exposed` the
    /// same in document px of that page.
    struct Slice {
        int page;
        QRect shown;
        QRectF exposed;
    };
    std::vector<Slice> slices(const QRect &area) const
    {
        std::vector<Slice> out;
        const auto [first, last] = visiblePages(area);
        for (int i = first; i <= last; ++i) {
            QRect shown = area;
            if (paged()) {
                shown = area & pageWidgetRect(i);
                if (shown.isEmpty())
                    continue;
            }
            out.push_back({i, shown, QRectF(shown.x() / zoom + settings.extent.x(), shown.y() / zoom - i * gap() + settings.extent.y(),
                                            shown.width() / zoom, shown.height() / zoom)});
        }
        return out;
    }
    /// Calls draw(documentRect, pageIndex) once per page the widget-px `area` touches, with the painter (already in widget
    /// px) set up so document coordinates draw in place, clipped to that page in ContinuousVertical. Ink that
    /// crosses a page break is therefore cut by the gap, not smeared over it.
    template <typename Draw>
    void forEachPage(QPainter &painter, const QRect &area, Draw draw) const
    {
        for (const Slice &s : slices(area)) {
            painter.save();
            if (paged())
                painter.setClipRect(pageWidgetRect(s.page), Qt::IntersectClip);
            painter.scale(zoom, zoom);
            painter.translate(-settings.extent.x(), -settings.extent.y() + s.page * gap());
            draw(s.exposed, s.page);
            painter.restore();
        }
    }
    /// The pattern of one page (or of the whole sheet). Paged layouts: one pattern per page, restarting at its top-left.
    /// Fixed and the sheets: the whole extent is one sheet (Fixed has no gap, addPage grows its extent); a sheet anchors
    /// at the document origin, so growing it up or left moves no line under the ink - and the tile seams move none either.
    void paintPageBackground(QPainter &painter, int page, const QRectF &exposed) const
    {
        const bool pagedLayout = settings.layout == LayoutKind::ContinuousVertical;
        const QRectF sheet = pagedLayout ? pageRect(page) : settings.extent;
        const bool sheetLayout = settings.layout == LayoutKind::SemiInfinite || settings.layout == LayoutKind::Infinite;
        paintPattern(painter, settings.background, sheet, sheetLayout ? QPointF(0, 0) : sheet.topLeft(), exposed, zoom);
    }
};

Geometry geometryOf(const InkDocument &document, double zoom) { return {document.settings(), document.pageCount(), zoom}; }

/// What a tile job paints, copied on the UI thread: shared stroke payloads and the bounds of the rest, never the document.
struct TileItem {
    std::shared_ptr<const Stroke> stroke; ///< exactly one of the two: both are pure data, painted in the job (geometry only)
    std::shared_ptr<const Shape> shape;   ///< text boxes and images are not here: they need the GUI thread (paintUIItems)
};

struct TileSpec {
    Geometry geometry; ///< at the zoom the tile is rendered for
    QRectF rect;       ///< logical px of the tile space: where the tile sits in the widget at that zoom
    double dpr = 1.0;
    std::map<int, std::vector<TileItem>> items; ///< by page, in paint order
};

/// Runs on a pool thread: QImage + QPainter only. Page background, pattern and items, transparent in the gaps of
/// ContinuousVertical; the shadows, borders, the prediction tail and the live stroke are the canvas's own.
QImage renderTile(const TileSpec &spec)
{
    QImage image(TileCache::kTileSize, TileCache::kTileSize, QImage::Format_ARGB32_Premultiplied);
    if (image.isNull())
        return image;
    image.setDevicePixelRatio(spec.dpr);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.translate(-spec.rect.topLeft());
    const Geometry &geo = spec.geometry;
    const QRect area = spec.rect.toAlignedRect();
    const QColor background = geo.settings.background.color;
    if (!geo.paged()) {
        painter.fillRect(spec.rect, background);
    } else {
        for (const Geometry::Slice &s : geo.slices(area))
            painter.fillRect(geo.pageWidgetRect(s.page) & area, background);
    }
    geo.forEachPage(painter, area, [&](const QRectF &exposed, int page) {
        geo.paintPageBackground(painter, page, exposed);
        const auto it = spec.items.find(page);
        if (it == spec.items.end())
            return;
        for (const TileItem &item : it->second) {
            if (item.stroke)
                paintStroke(painter, *item.stroke);
            else
                paintShape(painter, *item.shape);
        }
    });
    return image;
}

} // namespace

InkCanvas::InkCanvas(QWidget *parent) : QWidget(parent)
{
    m_zoomTimer.setSingleShot(true);
    m_zoomTimer.setInterval(kZoomSettleMs);
    connect(&m_zoomTimer, &QTimer::timeout, this, &InkCanvas::settleZoom);
    connect(&m_tiles, &TileCache::tileReady, this, &InkCanvas::onTileReady);
    setFocusPolicy(Qt::StrongFocus);
    setTabletTracking(true); // hover events while the pen is near the tablet (spec section 7, Sources)
    setAttribute(Qt::WA_AcceptTouchEvents);
    setAttribute(Qt::WA_OpaquePaintEvent);
    grabGesture(Qt::PinchGesture);
    updateCursor();
    documentReplaced();
}

void InkCanvas::setDrawWithMouse(bool on)
{
    m_drawWithMouse = on;
    updateCursor();
}

/// The Text tool takes the mouse; otherwise a mouse that cannot draw shows a hand (it scrolls), and the one that can, a cross.
void InkCanvas::updateCursor()
{
    if (m_tool == Tool::Text)
        setCursor(Qt::IBeamCursor);
    else if (m_mousePanning)
        setCursor(Qt::ClosedHandCursor);
    else
        setCursor(m_drawWithMouse ? Qt::CrossCursor : Qt::OpenHandCursor);
}

void InkCanvas::setTileCacheMB(int mb)
{
    m_userCacheBytes = qsizetype(std::clamp(mb, kMinTileCacheMB, kMaxTileCacheMB)) * 1024 * 1024;
    m_tiles.setMaxBytes(m_userCacheBytes);
    update(); // the next paint raises it again if the visible tiles need more room
}
qsizetype InkCanvas::tileCacheBytes() const { return m_userCacheBytes; }
double InkCanvas::tilesScale() const { return m_tilesZoom; }
int InkCanvas::pendingTiles() const { return m_tiles.pendingCount(); }
int InkCanvas::staleTileCount() const { return m_tiles.staleCount(); }
void InkCanvas::setTileJobHook(std::function<void()> hook) { m_tileJobHook = std::move(hook); }

QSize InkCanvas::sizeHint() const { return geometryOf(m_document, m_zoom).size(); }

double InkCanvas::pageGap() const
{
    return m_document.settings().layout == LayoutKind::ContinuousVertical ? InkDocument::kPageGap : 0.0;
}

QPointF InkCanvas::toWidget(const QPointF &documentPos) const
{
    const DocumentSettings &s = m_document.settings();
    const double y = documentPos.y() - s.extent.y();
    const double gap = pageGap();
    double pagesAbove = 0.0;
    if (gap > 0 && s.format.height > 0) {
        const double h = s.format.height;
        pagesAbove = std::floor(y / h + kPageEdgeSlack); // slack: a page edge computed as page * h is a hair under it
        const int pages = m_document.pageCount();
        if (y <= pages * h * (1 + kPageEdgeSlack))
            pagesAbove = std::min(pagesAbove, double(pages - 1)); // the last page's bottom edge belongs to it: no gap there
    }
    return QPointF((documentPos.x() - s.extent.x()) * m_zoom, (y + pagesAbove * gap) * m_zoom);
}

QPointF InkCanvas::toDocument(const QPointF &widgetPos) const
{
    const DocumentSettings &s = m_document.settings();
    double y = widgetPos.y() / m_zoom;
    const double gap = pageGap();
    const double h = s.format.height;
    if (gap > 0 && h > 0) {
        const double stride = h + gap;
        const double page = std::floor(y / stride + kPageEdgeSlack);
        y = page * h + std::clamp(y - page * stride, 0.0, h); // inside a gap: the page above's bottom edge
    }
    return QPointF(widgetPos.x() / m_zoom + s.extent.x(), y + s.extent.y());
}

QRect InkCanvas::pageWidgetRect(int index) const { return geometryOf(m_document, m_zoom).pageWidgetRect(index); }

std::pair<int, int> InkCanvas::visiblePages(const QRect &area) const { return geometryOf(m_document, m_zoom).visiblePages(area); }

void InkCanvas::documentReplaced()
{
    finishStroke(false); // the stroke in flight belongs to the document that went away
    closeTextEditor();   // so does an open box: undo and the owner's reload end it first (commitText), nothing here may write into the new document
    std::erase_if(m_textCache, [this](const auto &entry) { return !m_document.item(entry.first); }); // a moved box keeps its picture
    setSelection({});    // its items are gone (an undo, a load) or must be picked again
    m_live = QImage();
    m_holdShape.reset();
    m_tiles.invalidateAll(); // the content (or its place in the widget) changed: the images stay as the blurry stand-in
    resize(sizeHint());
    updateGeometry();
    update();
    // An extent that now starts further left or up (an Infinite sheet grew that way, or an undo took the
    // growth back) moved every item inside the widget; the owner scrolls by the same amount to keep the
    // view on the same ink. A zoom keeps the origin and never gets here.
    const QPointF origin = m_document.settings().extent.topLeft();
    const QPointF moved = (m_origin - origin) * m_zoom;
    m_origin = origin;
    if (!moved.isNull())
        emit originShifted(QPoint(qRound(moved.x()), qRound(moved.y())));
}

void InkCanvas::setZoom(double zoom)
{
    zoom = std::clamp(zoom, 0.25, 8.0);
    if (qFuzzyCompare(zoom, m_zoom))
        return;
    finishStroke(true); // in the old zoom: its samples are widget px of it
    m_zoom = zoom;
    m_live = QImage();
    layoutTextEditor();
    m_zoomTimer.start(); // the tiles are scaled until the pinch has been still for kZoomSettleMs
    resize(sizeHint());
    updateGeometry();
    update();
    emit zoomChanged(m_zoom);
}

QRect InkCanvas::widgetRect(const QRectF &documentRect) const
{
    return QRectF(toWidget(documentRect.topLeft()), toWidget(documentRect.bottomRight())).toAlignedRect().adjusted(-2, -2, 2, 2);
}

void InkCanvas::setEraserMode(EraserMode mode)
{
    if (mode == m_eraserMode)
        return;
    m_eraserMode = mode;
    emit eraserModeChanged(m_eraserMode);
}

void InkCanvas::setShapeKind(ShapeKind kind)
{
    if (kind == m_shapeKind || kind == ShapeKind::Polyline || kind == ShapeKind::Polygon) // no tool draws the last two
        return;
    m_shapeKind = kind;
    emit shapeKindChanged(m_shapeKind);
}

void InkCanvas::setSelectShape(SelectShape shape)
{
    if (shape == m_selectShape)
        return;
    m_selectShape = shape;
    emit selectShapeChanged(m_selectShape);
}
void InkCanvas::setTool(Tool tool)
{
    if (tool == m_tool)
        return;
    commitText();
    if (m_drawing == Drawing::Shape)
        finishStroke(false); // a shape drag belongs to the Shape tool
    m_tool = tool;
    updateCursor();
    if (tool != Tool::Lasso)
        clearSelection(); // the selection belongs to the select tool: one made with the held button stays for it
    emit toolChanged(m_tool);
}

void InkCanvas::setReadOnly(bool on)
{
    if (on) {
        finishStroke(true);
        commitText(); // while the canvas still takes edits: what was typed is kept
    }
    m_readOnly = on;
}

PressureNormaliser &InkCanvas::normaliserFor(const QString &device)
{
    auto it = m_pressure.find(device);
    if (it == m_pressure.end()) {
        it = m_pressure.emplace(device, PressureNormaliser{}).first;
        it->second.setLearnedMax(m_pressureSeed); // 1.0 = nothing known: learn it
    }
    return it->second;
}

double InkCanvas::learnedPressure(const QString &device) const
{
    const auto it = m_pressure.find(device);
    return it == m_pressure.end() ? 1.0 : it->second.learnedMax();
}

void InkCanvas::setLearnedPressure(const QString &device, double max)
{
    m_pressure[device].setLearnedMax(max);
}

// --- input -----------------------------------------------------------------

void InkCanvas::tabletEvent(QTabletEvent *event)
{
    // The pen is in the open editor (the event came up from it): Qt sends the editor mouse events instead. Not while a press
    // of this pen is being followed (a tap that opened the editor still has its release to come: it clears the flag).
    if (!m_tabletActive && m_textEdit && m_textEdit->geometry().contains(event->position().toPoint())) {
        event->ignore();
        return;
    }
    event->accept(); // no synthesised mouse events for this pen
    if (m_readOnly)
        return;
    const double seconds = event->timestamp() / 1000.0;
    const QPointF pos = event->position() - kFractionalScaleOffset;
    // This device's learned maximum turns raw pressure into 0..1 (a FydeTab Duo's pen never exceeds 0.657). Only
    // pressed samples teach it: the release carries the lifted 0, and the stroke ends with the last real pressure.
    const QString device = event->pointingDevice() ? event->pointingDevice()->name() : QString();
    PressureNormaliser *normaliser = nullptr;
    switch (event->type()) {
    case QEvent::TabletPress: {
        if (event->button() != Qt::LeftButton) // a barrel button changing is not the tip going down
            break;
        normaliser = &normaliserFor(device);
        const bool eraseByDevice = kTrustEraserPointerType && event->pointerType() == QPointingDevice::PointerType::Eraser;
        const bool eraseByButton = event->buttons().testFlag(kHeldEraserButton);
        const bool erase = eraseByDevice || eraseByButton || m_tool == Tool::Eraser;
        // The select tool, or the pen with the barrel button held (for this stroke only; the glyph studio turns that off).
        const bool select = !erase && (m_tool == Tool::Lasso || (m_heldLassoEnabled && m_tool == Tool::Pen && event->buttons().testFlag(kHeldLassoButton)));
        pressAt(pos, normaliser->normalise(event->pressure()), seconds, erase, select);
        m_tabletActive = true; // after pressAt: closing an old stroke there clears it
        break;
    }
    case QEvent::TabletMove:
        if (m_tabletActive) {
            normaliser = &normaliserFor(device);
            moveTo(pos, normaliser->normalise(event->pressure()), seconds);
        }
        break;
    case QEvent::TabletRelease:
        if (event->button() == Qt::LeftButton && m_tabletActive) {
            releaseAt(pos, seconds);
            m_tabletActive = false; // a press that began no gesture (the Text tool) has no release to clear it
        }
        break;
    default:
        break;
    }
    if (normaliser && normaliser->takeChanged()) // learning moved the maximum: tell the owner once
        emit learnedPressureChanged(device, normaliser->learnedMax());
}

void InkCanvas::mousePressEvent(QMouseEvent *event)
{
    if (m_tabletActive || event->button() != Qt::LeftButton || (m_textEdit && m_textEdit->geometry().contains(event->position().toPoint())))
        return QWidget::mousePressEvent(event); // (a press the open editor did not take is still not an edit-ender)
    // Ctrl+drag selects in any tool, drawWithMouse or not: the mouse user needs no tool switch. The select tool
    // takes the mouse when it draws at all. The Text tool takes a click from a mouse too.
    const bool select = event->modifiers().testFlag(Qt::ControlModifier) || (m_drawWithMouse && m_tool == Tool::Lasso);
    if (m_readOnly || (!m_drawWithMouse && !select && m_tool != Tool::Text)) { // a drag is no drawing: it scrolls
        commitText();
        setFocus(Qt::MouseFocusReason);
        m_mousePanning = true;
        m_lastMouse = event->globalPosition(); // not position(): the scroll area moves this widget under the pointer
        updateCursor();
        event->accept();
        return;
    }
    pressAt(event->position(), 0.5, event->timestamp() / 1000.0, m_tool == Tool::Eraser, select);
}

void InkCanvas::mouseMoveEvent(QMouseEvent *event)
{
    if (m_mousePanning) {
        const QPoint step = (event->globalPosition() - m_lastMouse).toPoint();
        if (!step.isNull()) {
            emit panRequested(step);
            m_lastMouse += step; // only what was scrolled is consumed: the fraction under fractional scaling is kept for the next move
        }
        return;
    }
    if (m_drawing == Drawing::None || m_tabletActive)
        return QWidget::mouseMoveEvent(event);
    moveTo(event->position(), 0.5, event->timestamp() / 1000.0);
}

void InkCanvas::mouseReleaseEvent(QMouseEvent *event)
{
    if (m_mousePanning) {
        m_mousePanning = false;
        updateCursor();
        return;
    }
    if (m_drawing == Drawing::None || m_tabletActive)
        return QWidget::mouseReleaseEvent(event);
    releaseAt(event->position(), event->timestamp() / 1000.0);
}

void InkCanvas::wheelEvent(QWheelEvent *event)
{
    if (!event->modifiers().testFlag(Qt::ControlModifier))
        return QWidget::wheelEvent(event); // ignored: the scroll area scrolls
    const double steps = event->angleDelta().y() / 120.0;
    if (steps != 0)
        setZoom(m_zoom * std::pow(1.1, steps));
    event->accept();
}

bool InkCanvas::event(QEvent *event)
{
    switch (event->type()) {
    case QEvent::TouchBegin:
    case QEvent::TouchUpdate:
    case QEvent::TouchEnd: {
        auto *touch = static_cast<QTouchEvent *>(event);
        touch->accept();
        if (isDrawing()) { // a palm beside the pen must not move the page under it
            m_touchPanning = false;
            return true;
        }
        const auto &points = touch->points();
        if (points.size() == 1) {
            const QPointF now = points[0].globalPosition(); // not position(): the scroll area moves this widget under the finger
            if (event->type() == QEvent::TouchBegin) {
                m_touchPanning = true;
            } else if (m_touchPanning && event->type() == QEvent::TouchUpdate) {
                emit panRequested((now - m_lastTouch).toPoint());
            }
            m_lastTouch = now;
        }
        if (event->type() == QEvent::TouchEnd)
            m_touchPanning = false;
        return true; // touch is pan and zoom, never ink (spec section 7, Input)
    }
    case QEvent::Leave:
    case QEvent::TabletLeaveProximity:
        finishStroke(true); // the pointer left: no release will come
        return QWidget::event(event);
    case QEvent::Gesture: {
        auto *gesture = static_cast<QGestureEvent *>(event);
        if (auto *pinch = static_cast<QPinchGesture *>(gesture->gesture(Qt::PinchGesture))) {
            if (pinch->changeFlags().testFlag(QPinchGesture::ScaleFactorChanged))
                setZoom(m_zoom * pinch->scaleFactor());
            m_touchPanning = false;
        }
        return true;
    }
    default:
        return QWidget::event(event);
    }
}

namespace {
bool toolForKey(int key, InkCanvas::Tool *tool)
{
    switch (key) {
    case Qt::Key_P: *tool = InkCanvas::Tool::Pen; return true;
    case Qt::Key_E: *tool = InkCanvas::Tool::Eraser; return true;
    case Qt::Key_L: *tool = InkCanvas::Tool::Lasso; return true;
    case Qt::Key_S: *tool = InkCanvas::Tool::Shape; return true;
    case Qt::Key_T: *tool = InkCanvas::Tool::Text; return true;
    default: return false;
    }
}
} // namespace

void InkCanvas::keyPressEvent(QKeyEvent *event)
{
    InkCanvas::Tool keyTool = InkCanvas::Tool::Pen;
    if (m_textEdit) { // keys the editor ignored (Escape, Ctrl+Y, F-keys) must not reach the tool and edit shortcuts below
        if (event->key() == Qt::Key_Escape)
            commitText();
        else
            QWidget::keyPressEvent(event);
        return;
    }
    if (m_readOnly) {
        QWidget::keyPressEvent(event);
    } else if (event->matches(QKeySequence::Undo)) {
        undo();
    } else if (event->matches(QKeySequence::Redo) || (event->key() == Qt::Key_Y && event->modifiers() == Qt::ControlModifier)) {
        redo();
    } else if (event->matches(QKeySequence::Copy)) {
        copySelection();
    } else if (event->matches(QKeySequence::Paste)) {
        paste();
    } else if (event->key() == Qt::Key_Delete && event->modifiers() == Qt::NoModifier) {
        deleteSelection();
    } else if (event->key() == Qt::Key_Escape && event->modifiers() == Qt::NoModifier &&
               (isSelectGesture() || m_drawing == Drawing::Shape || !m_selection.empty())) {
        if (isSelectGesture() || m_drawing == Drawing::Shape)
            finishStroke(false); // the first Escape cancels the drag in flight and keeps the selection, the next one clears it
        else
            clearSelection();
    } else if (event->key() == Qt::Key_L && event->modifiers() == Qt::ShiftModifier) {
        setTool(Tool::Lasso); // lasso or rectangle without a toolbar or a pen button
        setSelectShape(m_selectShape == SelectShape::Rect ? SelectShape::Lasso : SelectShape::Rect);
    } else if (event->key() == Qt::Key_S && event->modifiers() == Qt::ShiftModifier) {
        setTool(Tool::Shape); // line, arrow, rectangle, ellipse without a toolbar or a pen button
        static constexpr ShapeKind kinds[] = {ShapeKind::Line, ShapeKind::Arrow, ShapeKind::Rect, ShapeKind::Ellipse};
        const auto at = std::find(std::begin(kinds), std::end(kinds), m_shapeKind);
        setShapeKind(kinds[(at - std::begin(kinds) + 1) % std::size(kinds)]);
    } else if (event->key() == Qt::Key_E && event->modifiers() == Qt::ShiftModifier) {
        setTool(Tool::Eraser); // the whole/parts switch without a toolbar or a pen button
        setEraserMode(m_eraserMode == EraserMode::Split ? EraserMode::Stroke : EraserMode::Split);
    } else if (event->modifiers() == Qt::NoModifier && toolForKey(event->key(), &keyTool)) {
        setTool(keyTool); // every tool without pen hardware (a FydeTab Duo's pen has no eraser end and dead buttons)
    } else {
        QWidget::keyPressEvent(event);
    }
}

void InkCanvas::focusOutEvent(QFocusEvent *event)
{
    if (event->reason() != Qt::PopupFocusReason) // a popup (a menu over the toolbar) is not a lost pen
        finishStroke(true);
    QWidget::focusOutEvent(event);
}

void InkCanvas::hideEvent(QHideEvent *event)
{
    m_mousePanning = false;
    updateCursor();
    finishStroke(true);
    commitText();
    QWidget::hideEvent(event);
}

void InkCanvas::changeEvent(QEvent *event)
{
    if (event->type() == QEvent::EnabledChange && !isEnabled())
        finishStroke(true);
    QWidget::changeEvent(event);
}

// --- the stroke in flight ----------------------------------------------------

void InkCanvas::pressAt(const QPointF &widgetPos, double pressure, double seconds, bool erase, bool select)
{
    if (m_drawing != Drawing::None)
        finishStroke(true); // a press without the release: close the old stroke where IT ended, not at this press
    commitText();         // a press anywhere but in the editor ends the box
    setFocus(Qt::MouseFocusReason);
    m_holdLive = false;
    m_holdShape.reset();
    m_lastPos = widgetPos;
    m_lastPressure = pressure;
    m_lastSeconds = seconds;
    if (select) {
        pressSelect(widgetPos);
        return;
    }
    clearSelection(); // drawing or erasing leaves the selection behind
    if (erase) {
        m_drawing = Drawing::Eraser;
        m_erasedSomething = false;
        m_lastErase = toDocument(widgetPos);
        eraseSweep(m_lastErase, m_lastErase);
        return;
    }
    if (m_tool == Tool::Text) { // a click, not a gesture: nothing is in flight
        pressText(toDocument(widgetPos));
        return;
    }
    if (m_tool == Tool::Shape) {
        const QPointF at = toDocument(widgetPos);
        m_drawing = Drawing::Shape;
        m_shapeDraft = Shape{m_shapeKind, m_penStyle, {at, at}};
        return;
    }
    m_drawing = Drawing::Pen;
    prepareLiveOverlay();
    m_input.begin({toDocument(widgetPos), pressure, seconds}, m_penStyle);
    rasteriseNewSegments();
}

void InkCanvas::moveTo(const QPointF &widgetPos, double pressure, double seconds)
{
    m_lastPos = widgetPos;
    m_lastPressure = pressure;
    m_lastSeconds = seconds;
    if (m_drawing == Drawing::Eraser) {
        const QPointF now = toDocument(widgetPos);
        eraseSweep(m_lastErase, now);
        m_lastErase = now;
        return;
    }
    if (m_drawing == Drawing::Select) {
        updateLasso(toDocument(widgetPos));
        return;
    }
    if (m_drawing == Drawing::Shape) {
        updateShape(toDocument(widgetPos));
        return;
    }
    if (m_drawing == Drawing::Move || m_drawing == Drawing::Scale) {
        dragSelection(toDocument(widgetPos));
        return;
    }
    if (m_drawing != Drawing::Pen)
        return;
    m_input.extend({toDocument(widgetPos), pressure, seconds});
    rasteriseNewSegments();
}

void InkCanvas::releaseAt(const QPointF &widgetPos, double seconds)
{
    if (m_drawing == Drawing::None)
        return;
    m_lastPos = widgetPos; // the pressure stays the last one in contact: a real pen reports 0 at lift
    m_lastSeconds = seconds;
    if (m_drawing == Drawing::Eraser) {
        const QPointF now = toDocument(widgetPos);
        eraseSweep(m_lastErase, now);
        m_lastErase = now;
    } else if (m_drawing == Drawing::Select) {
        updateLasso(toDocument(widgetPos));
    } else if (m_drawing == Drawing::Shape) {
        updateShape(toDocument(widgetPos));
    } else if (m_drawing == Drawing::Move || m_drawing == Drawing::Scale) {
        dragSelection(toDocument(widgetPos));
    }
    finishStroke(true);
}

/// The one way a stroke ends, however it ends. keep = true ends it at the last received sample and
/// commits what was drawn or erased (pen-up, a lost release, focus loss, hide, disable, read-only,
/// zoom, the pointer leaving). keep = false (documentReplaced) drops the live stroke and commits
/// nothing, and discards the eraser's uncommitted removals (InkDocument::discardLiveEdits).
void InkCanvas::finishStroke(bool keep)
{
    const Drawing mode = std::exchange(m_drawing, Drawing::None);
    m_tabletActive = false;
    const QRectF tail = std::exchange(m_lastPrediction, QRectF()); // the tail goes with the pen: repaint where it was
    if (mode == Drawing::None)
        return;
    if (mode == Drawing::Select || mode == Drawing::Move || mode == Drawing::Scale) {
        finishSelect(mode, keep);
        return;
    }
    if (mode == Drawing::Shape) {
        finishShape(keep);
        return;
    }
    if (mode == Drawing::Eraser) {
        if (std::exchange(m_erasedSomething, false)) {
            if (keep)
                finishEdit();
            else
                m_document.discardLiveEdits(); // a cancel: the removals were never a history step; a replaced (reset) document is clean, so this is a no-op there
        }
        return;
    }
    m_input.end({toDocument(m_lastPos), m_lastPressure, m_lastSeconds});
    m_input.takeNewPoints();
    const std::shared_ptr<const Stroke> stroke = m_input.finish();
    // A one-point dot is a stroke the reader accepts; only a stroke with no point at all is not committed.
    if (keep && stroke && !stroke->points.empty()) {
        // Pen strokes are always Layer::User; Layer::Highlighter is only read from a file.
        // A highlighter tool does nothing over handwriting (spec section 2), so none exists here.
        m_document.addStroke(stroke, Layer::User);
        const bool grew = m_document.growToContain(stroke->bounds());
        finishEdit();
        invalidateTiles(stroke->bounds());
        m_holdLive = !m_live.isNull(); // the stroke stays on screen from the live image until its tiles show it
        m_holdRect = widgetRect(stroke->bounds());
        if (grew)
            documentReplaced(); // the page grew: resize the widget (m_drawing is None, nothing to cancel)
        update(widgetRect(stroke->bounds() | tail));
    } else {
        update();
    }
}

/// The overlay is the visible part of the canvas only (the viewport inside InkView's scroll area):
/// a ten-page document must not cost a 140 MB image and a 140 MB clear at every pen-down.
void InkCanvas::prepareLiveOverlay()
{
    QRect want = visibleRegion().boundingRect();
    if (want.isEmpty())
        want = rect();
    const qreal dpr = devicePixelRatioF();
    const int cap = std::max(1, int(kMaxOverlaySide / dpr)); // the cap is in device px: the image is dpr times its widget size
    want.setSize(want.size().boundedTo(QSize(cap, cap)));
    if (m_live.isNull() || m_liveRect != want || m_live.devicePixelRatio() != dpr) {
        m_liveRect = want;
        m_live = QImage(want.size() * dpr, QImage::Format_ARGB32_Premultiplied);
        m_live.setDevicePixelRatio(dpr);
        m_live.fill(Qt::transparent);
    } else if (!m_liveDirty.isEmpty()) { // only what the last stroke painted
        QPainter painter(&m_live);
        painter.setCompositionMode(QPainter::CompositionMode_Source);
        painter.fillRect(m_liveDirty.translated(-m_liveRect.topLeft()), Qt::transparent);
    }
    m_liveDirty = QRect();
}

void InkCanvas::rasteriseNewSegments()
{
    const size_t fresh = m_input.takeNewPoints();
    const Stroke &stroke = m_input.stroke();
    if (fresh == 0 || stroke.points.empty() || m_live.isNull())
        return;
    // The segments ending at the new points: from the last point of the
    // previous batch, so nothing is missed between batches.
    const size_t last = stroke.points.size() - 1;
    const size_t first = last + 1 >= fresh + 1 ? last - fresh : 0;
    const QPainterPath path = strokeOutline(stroke, first, last);
    QPainter painter(&m_live);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.translate(-m_liveRect.topLeft());
    painter.setPen(Qt::NoPen);
    painter.setBrush(stroke.style.color);
    geometryOf(m_document, m_zoom).forEachPage(painter, widgetRect(path.boundingRect()), [&](const QRectF &, int) { painter.drawPath(path); });
    painter.end();
    m_liveDirty |= widgetRect(path.boundingRect());
    QRectF tail; // the predicted tail's box: repainted now, and again where it was when it moves
    for (const StrokePoint &p : m_input.prediction())
        tail |= QRectF(p.x - stroke.style.width, p.y - stroke.style.width, 2 * stroke.style.width, 2 * stroke.style.width);
    const QRectF dirty = path.boundingRect() | tail | m_lastPrediction; // a null rect adds nothing to a union
    m_lastPrediction = tail;
    update(widgetRect(dirty));
}

// --- text boxes ---------------------------------------------------------------------------

namespace {
constexpr double kTextEditorFontPx = 16.0; ///< document px at zoom 1: the size the editor over a box types in
constexpr double kTextScaleMin = 0.0625;  ///< device px per document px: the smallest step of textScaleStep

/// The smallest of 2^k and 1.5 * 2^k that is at least `wanted`: a zoom between two steps is drawn at the one above,
/// and a zoom-out down to half of it reuses the picture.
double textScaleStep(double wanted)
{
    double step = kTextScaleMin;
    while (step < wanted * (1 - 1e-9) && step < 1024) {
        if (step * 1.5 >= wanted * (1 - 1e-9))
            return step * 1.5;
        step *= 2;
    }
    return step;
}

/// The Markdown the renderer is given: the first kMaxTextRenderChars, cut at a line end when there is one, and ended
/// with an ellipsis so that the cut shows. The box keeps its whole text.
QString textForRender(const QString &markdown)
{
    constexpr qsizetype limit = InkCanvas::kMaxTextRenderChars;
    if (markdown.size() <= limit)
        return markdown;
    qsizetype cut = markdown.lastIndexOf(QLatin1Char('\n'), limit - 1);
    if (cut < limit / 2)
        cut = limit; // no usable line end: cut anywhere (not between the halves of a surrogate pair)
    if (cut < markdown.size() && markdown.at(cut).isLowSurrogate())
        --cut;
    return markdown.left(cut) + QStringLiteral("\n\n\u2026");
}
} // namespace

void InkCanvas::setTextRenderer(TextRenderer renderer)
{
    m_textRenderer = std::move(renderer);
    invalidateTextCache();
}

void InkCanvas::invalidateTextCache()
{
    m_textCache.clear();
    update();
}

qsizetype InkCanvas::textCacheBytes() const
{
    qsizetype bytes = 0;
    for (const auto &entry : m_textCache)
        bytes += qsizetype(entry.second.pixmap.width()) * entry.second.pixmap.height() * 4;
    return bytes;
}

void InkCanvas::setTextEditorFactory(TextEditorFactory factory)
{
    m_textEditorFactory = std::move(factory);
}

bool InkCanvas::beginEditingText(qint64 t)
{
    const Item *item = m_document.item(t);
    if (m_readOnly || !item || item->type != Item::Type::TextBox)
        return false;
    openTextEditor(t, item->textBox->rect, item->textBox->markdown);
    return true;
}

/// A press with the Text tool: the newest box under the pointer is edited, otherwise a new one starts at the pointer.
void InkCanvas::pressText(const QPointF &at)
{
    qint64 hit = 0;
    for (qint64 t : m_document.itemsIntersecting(QRectF(at, QSizeF(0, 0)).adjusted(-1, -1, 1, 1))) {
        const Item *item = m_document.item(t);
        if (item && item->type == Item::Type::TextBox && item->textBox->rect.contains(at))
            hit = std::max(hit, t); // the highest t paints last: it is the one on top
    }
    if (hit) {
        beginEditingText(hit);
        return;
    }
    QRectF rect(at, kNewTextBoxSize);
    const QRectF page = m_document.settings().extent; // a box starting near the right edge slides left rather than hang off the page
    rect.moveLeft(std::clamp(rect.left(), page.left(), std::max(page.left(), page.right() - rect.width())));
    openTextEditor(-1, rect, QString());
}

void InkCanvas::openTextEditor(qint64 t, const QRectF &rect, const QString &markdown)
{
    commitText();
    clearSelection();
    QWidget *edit = m_textEditorFactory.create ? m_textEditorFactory.create(markdown) : nullptr;
    if (edit) {
        edit->setParent(this);
    } else {
        auto *plain = new QPlainTextEdit(this);
        plain->setPlainText(markdown);
        plain->moveCursor(QTextCursor::End);
        const QColor page = m_document.settings().background.color;
        QPalette colours = plain->palette();
        colours.setColor(QPalette::Base, page);
        colours.setColor(QPalette::Text, page.lightness() > 128 ? QColor(0x1e, 0x1e, 0x1e) : QColor(0xe0, 0xe0, 0xe0));
        colours.setColor(QPalette::WindowText, palette().highlight().color()); // the frame
        plain->setPalette(colours);
        plain->setFrameShape(QFrame::Box);
        edit = plain;
    }
    edit->setObjectName(QStringLiteral("inkTextEditor"));
    edit->installEventFilter(this); // Escape and focus loss end the edit (eventFilter)
    if (edit->metaObject()->indexOfSignal("textChanged()") >= 0) // a QPlainTextEdit and the host's MarkdownEditor both have it
        connect(edit, SIGNAL(textChanged()), this, SIGNAL(textEditChanged()));
    m_textEdit = edit;
    m_textEditT = t;
    m_textEditRect = rect;
    m_textEditZoom = 0;
    m_textEditOriginal = readTextEditor(); // what the editor makes of it (CRLF, NBSP), so that an untouched box is not an edit
    layoutTextEditor();
    edit->show();
    edit->setFocus(Qt::OtherFocusReason);
    update(widgetRect(rect)); // the box's picture is not painted while its editor is over it
}

QString InkCanvas::readTextEditor() const
{
    if (!m_textEdit)
        return QString();
    if (const auto *plain = qobject_cast<const QPlainTextEdit *>(m_textEdit.data()))
        return plain->toPlainText();
    return m_textEditorFactory.read ? m_textEditorFactory.read(m_textEdit) : QString();
}

void InkCanvas::layoutTextEditor()
{
    if (!m_textEdit)
        return;
    m_textEdit->setGeometry(QRectF(toWidget(m_textEditRect.topLeft()), toWidget(m_textEditRect.bottomRight())).toAlignedRect());
    if (qobject_cast<QPlainTextEdit *>(m_textEdit.data())) {
        QFont font = m_textEdit->font();
        font.setPixelSize(std::max(6, qRound(kTextEditorFontPx * m_zoom)));
        m_textEdit->setFont(font);
    } else if (m_textEditorFactory.setZoom && !qFuzzyCompare(m_textEditZoom, m_zoom)) {
        m_textEditorFactory.setZoom(m_textEdit, m_zoom);
    }
    m_textEditZoom = m_zoom;
}

QString InkCanvas::closeTextEditor()
{
    QWidget *edit = m_textEdit;
    if (!edit)
        return QString();
    const QString text = readTextEditor();
    m_textEdit = nullptr; // first: hiding it sends the focus-out that would come back here
    m_textEditT = 0;
    edit->removeEventFilter(this);
    const bool focused = edit->hasFocus();
    edit->hide();
    edit->deleteLater(); // never delete it directly: this runs from its own event filter
    if (focused)
        setFocus(Qt::OtherFocusReason);
    return text;
}

void InkCanvas::discardTextEdit()
{
    m_textEditRect = QRectF();
    closeTextEditor();
}

/// Ends the edit and commits it as one history step (Escape, a press elsewhere, a tool change, undo, hiding,
/// read-only): a new box with no text is never added, an unchanged box commits nothing, a box emptied of text goes.
void InkCanvas::commitText()
{
    if (!m_textEdit)
        return;
    // The editor is gone: text it held no longer counts as unsaved. Said only when it did hold some (a clean editor closing while its
    // owner is being destroyed has nobody to tell).
    const auto closed = qScopeGuard([this, dirty = isTextEditDirty()] { if (dirty) emit textEditChanged(); });
    const qint64 t = m_textEditT;
    const QRectF rect = std::exchange(m_textEditRect, QRectF());
    const QString text = closeTextEditor();
    update(widgetRect(rect));
    const bool blank = text.trimmed().isEmpty();
    if (t > 0) {
        const Item *old = m_document.item(t);
        if (!old || old->type != Item::Type::TextBox || text == m_textEditOriginal)
            return;
        if (blank) {
            m_document.removeItem(t);
        } else {
            Item changed = *old;
            changed.textBox = std::make_shared<TextBox>(TextBox{old->textBox->rect, text});
            m_document.replaceItem(t, std::move(changed));
        }
        finishEdit();
        return;
    }
    if (blank || m_readOnly)
        return;
    Item item;
    item.type = Item::Type::TextBox;
    item.layer = Layer::User;
    item.textBox = std::make_shared<TextBox>(TextBox{rect, text});
    m_document.addItem(std::move(item));
    const bool grew = m_document.growToContain(rect);
    finishEdit();
    if (grew)
        documentReplaced(); // the page grew: resize the widget (nothing is in flight)
    update(widgetRect(rect));
}

bool InkCanvas::isTextEditDirty() const
{
    return m_textEdit && readTextEditor() != m_textEditOriginal;
}

bool InkCanvas::eventFilter(QObject *watched, QEvent *event)
{
    if (m_textEdit && watched == m_textEdit) {
        if (event->type() == QEvent::KeyPress && static_cast<QKeyEvent *>(event)->key() == Qt::Key_Escape) {
            commitText();
            return true;
        }
        if (event->type() == QEvent::FocusOut && static_cast<QFocusEvent *>(event)->reason() != Qt::PopupFocusReason) {
            // a popup (the editor's own context menu) and a child of the editor (a host editor's inline field) are not a lost editor
            const QWidget *now = QApplication::focusWidget();
            if (!(now && m_textEdit->isAncestorOf(now)))
                commitText();
        }
    }
    return QWidget::eventFilter(watched, event);
}

double InkCanvas::textScale(const QSizeF &logical) const
{
    const double wanted = textScaleStep(m_tilesZoom * devicePixelRatioF());
    const double cap = kMaxTextDevicePx / std::max(1.0, std::max(logical.width(), logical.height()));
    return std::max(kTextScaleMin, std::min(wanted, std::floor(cap * 1000) / 1000));
}

/// The picture of a text box. Rendered here, on the UI thread (a QPixmap and a widget grab), the first time a paint
/// needs it and whenever its text, size, page colour or the scale it needs changed; never while a gesture is in flight
/// (a resize drag would build an editor per sample): the stale picture, if any, is stretched into the new rect until
/// the next quiet paint. The layout is the box's size in document px at every zoom, so the text wraps the same however
/// far the page is zoomed; the picture has the pixels for the settled zoom (m_tilesZoom: during a pinch the picture
/// of the old one is stretched, as the tiles are), and one drawn for up to twice the scale needed serves a zoom-out.
const QPixmap *InkCanvas::textPixmap(const Item &item)
{
    const TextBox &box = *item.textBox;
    const QSizeF logical = box.rect.size().boundedTo(QSizeF(kMaxTextBoxSide, kMaxTextBoxSide));
    const QSize size(std::max(1, qRound(logical.width())), std::max(1, qRound(logical.height())));
    const double scale = textScale(QSizeF(size));
    const qint64 need = qRound64(scale * 1000);
    const QColor background = m_document.settings().background.color;
    const auto it = m_textCache.find(item.t);
    const auto use = [&](TextPixmap &entry) { entry.used = m_textTick; };
    if (it != m_textCache.end() && it->second.markdown == box.markdown && it->second.size == size && it->second.background == background &&
        it->second.scaleKey >= need && it->second.scaleKey <= 2 * need) {
        use(it->second);
        return &it->second.pixmap;
    }
    // A paint renders pictures only up to the cache budget (a file with many large boxes in view would otherwise spend
    // seconds, and hold 64 MB for each, in one paint). What a paint used is held and counts for nothing here: only the
    // pictures it rendered do. The boxes it did not get to are drawn as they are (the stale picture, or a placeholder)
    // and paintUIItems asks for one more paint, which carries on with them: every box gets its picture within a few
    // paints, and no box is starved by the ones before it in paint order.
    // Over the ceiling the box is left as it is and nothing is asked for: the follow-up paint would hit the same wall.
    const auto bytesOf = [](const QPixmap &pixmap) { return qsizetype(pixmap.width()) * pixmap.height() * 4; };
    const qsizetype ownBytes = it != m_textCache.end() ? bytesOf(it->second.pixmap) : 0;
    if (m_textRenderer && !isDrawing() && m_textHeldBytes - ownBytes < kTextCacheCeilingBytes) {
        if (m_textPaintBytes >= kTextCacheBytes) {
            m_textSkipped = true;
        } else {
            QPixmap pixmap = m_textRenderer(textForRender(box.markdown), QSizeF(size), scale);
            if (!pixmap.isNull()) {
                m_textPaintBytes += bytesOf(pixmap);
                m_textHeldBytes += bytesOf(pixmap) - ownBytes;
                TextPixmap &entry = m_textCache[item.t];
                entry = {box.markdown, size, need, background, std::move(pixmap), m_textTick};
                return &entry.pixmap;
            }
        }
    }
    if (it != m_textCache.end())
        use(it->second);
    return it != m_textCache.end() ? &it->second.pixmap : nullptr;
}

/// What is not tile content: text boxes (over the tiles, so over every stroke whatever their z-order) and a placeholder
/// for an image (no image tool yet). The box being edited is not painted: its editor covers it.
void InkCanvas::paintUIItems(QPainter &painter, const QRect &area)
{
    ++m_textTick; // the pictures this paint uses are stamped with it, and are not evicted below
    m_textPaintBytes = 0;
    m_textSkipped = false;
    m_textHeldBytes = textCacheBytes();
    painter.save();
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    geometryOf(m_document, m_zoom).forEachPage(painter, area, [&](const QRectF &exposed, int) {
        for (const Item *item : m_document.paintOrder(exposed.adjusted(-4, -4, 4, 4))) {
            if (item->type == Item::Type::Image) {
                painter.fillRect(item->bounds(), QColor(0, 0, 0, 12));
                continue;
            }
            if (item->type != Item::Type::TextBox || item->t == m_textEditT)
                continue;
            const QRectF &rect = item->textBox->rect;
            if (!(rect.width() > 0 && rect.height() > 0) || !std::isfinite(rect.x()) || !std::isfinite(rect.y()))
                continue;
            if (const QPixmap *picture = textPixmap(*item))
                painter.drawPixmap(rect, *picture, QRectF(QPointF(), QSizeF(picture->size())));
            else
                painter.fillRect(rect, QColor(0, 0, 0, 12)); // no renderer (or it failed): the box is at least visible
        }
    });
    painter.restore();
    if (m_textSkipped) // boxes were left for a later paint: one more, after this one is done
        QMetaObject::invokeMethod(this, qOverload<>(&QWidget::update), Qt::QueuedConnection);
    // Gone boxes first, then the least recently painted until the budget holds (what this paint used stays).
    std::erase_if(m_textCache, [this](const auto &entry) { return !m_document.item(entry.first); });
    qsizetype bytes = textCacheBytes();
    while (bytes > kTextCacheBytes) {
        auto oldest = m_textCache.end();
        for (auto it = m_textCache.begin(); it != m_textCache.end(); ++it)
            if (it->second.used < m_textTick && (oldest == m_textCache.end() || it->second.used < oldest->second.used))
                oldest = it;
        if (oldest == m_textCache.end())
            break;
        bytes -= qsizetype(oldest->second.pixmap.width()) * oldest->second.pixmap.height() * 4;
        m_textCache.erase(oldest);
    }
}

// --- shapes -----------------------------------------------------------------------------

QRect InkCanvas::shapePaintRect() const { return widgetRect(m_shapeDraft.bounds()); }

void InkCanvas::updateShape(const QPointF &documentPos)
{
    const QRect old = shapePaintRect();
    m_shapeDraft.points[1] = documentPos;
    update(old | shapePaintRect());
}

/// The drag ends: keep = true commits the shape as one history step (a tap, which is no shape, commits nothing),
/// keep = false drops it. The shape stays painted over the tiles until the ones under it show it (m_holdShape).
void InkCanvas::finishShape(bool keep)
{
    const QRect paint = shapePaintRect();
    auto shape = std::make_shared<Shape>(std::exchange(m_shapeDraft, Shape{}));
    if (keep && shape->points.size() == 2 && QLineF(toWidget(shape->points[0]), toWidget(shape->points[1])).length() >= kMinShapeDragPx &&
        !shapePath(*shape).isEmpty()) {
        Item item;
        item.type = Item::Type::Shape;
        item.layer = Layer::User;
        item.shape = shape;
        m_document.addItem(std::move(item));
        const bool grew = m_document.growToContain(shape->bounds());
        finishEdit();
        invalidateTiles(shape->bounds());
        if (grew)
            documentReplaced(); // the page grew: resize the widget (m_drawing is None, nothing to cancel)
        m_holdShape = shape;
        m_holdRect = widgetRect(shape->bounds());
    }
    update(paint | m_holdRect);
}

void InkCanvas::paintShapeOver(QPainter &painter, const Shape &shape)
{
    geometryOf(m_document, m_zoom).forEachPage(painter, widgetRect(shape.bounds()), [&](const QRectF &, int) { paintShape(painter, shape); });
}

/// The eraser is the segment a-b swept by half its width (document px): a fast pen skips no
/// stroke between two samples. Candidates come from the spatial grid; a stroke is tested on its
/// polyline, never by building its outline.
void InkCanvas::eraseSweep(const QPointF &a, const QPointF &b)
{
    const double r = m_eraserWidth * 0.5;
    const QRectF area = QRectF(a, b).normalized().adjusted(-r, -r, r, r);
    QRectF dirty;
    QPainterPath eraser; // built on the first item that needs it: a shape, or a stroke in Split mode
    const auto capsule = [&]() -> const QPainterPath & {
        if (eraser.isEmpty())
            eraser = eraserCapsule(a, b, r);
        return eraser;
    };
    for (qint64 t : m_document.itemsIntersecting(area)) {
        const Item *item = m_document.item(t);
        if (!item)
            continue;
        const bool isStroke = item->type == Item::Type::Stroke;
        if (isStroke) {
            if (!eraserTouches(*item->stroke, a, b, r))
                continue;
        } else if (!item->bounds().intersects(area)) {
            continue;
        } else if (item->type == Item::Type::Shape && !shapePath(*item->shape).intersects(capsule())) {
            continue; // the ink only: the hollow inside a rectangle is not the rectangle. A hit shape goes whole in either mode (Split cuts strokes only)
        } else if ((item->type == Item::Type::TextBox || item->type == Item::Type::Image) && !capsule().intersects(item->bounds())) {
            continue; // the box itself, not the box of the swept capsule: a pass beside its corner leaves it
        }
        if (isStroke && m_eraserMode == EraserMode::Split) {
            const SplitResult split = splitStroke(*item->stroke, capsule());
            if (!split.hit)
                continue; // the polyline test is a little generous: the outline did not meet the eraser
            const QRectF oldBounds = item->bounds();
            const Item original = *item; // replaceItem and addItem invalidate `item`
            dirty |= oldBounds;
            invalidateTiles(oldBounds); // the pieces lie inside the old bounds
            if (split.pieces.empty()) {
                m_document.removeItem(t);
            } else {
                Item piece = original;
                piece.stroke = split.pieces.front();
                m_document.replaceItem(t, piece); // the first piece keeps t: a small undo diff
                for (size_t i = 1; i < split.pieces.size(); ++i) {
                    piece.stroke = split.pieces[i];
                    m_document.addItem(piece);
                }
            }
            m_erasedSomething = true;
            continue;
        }
        dirty |= item->bounds();
        invalidateTiles(item->bounds());
        m_document.removeItem(t);
        m_erasedSomething = true;
    }
    if (dirty.isValid())
        update(widgetRect(dirty));
}

void InkCanvas::finishEdit()
{
    m_document.commit();
    emit documentEdited();
}

bool InkCanvas::undo()
{
    if (m_readOnly || isDrawing())
        return false;
    commitText(); // the open box is one more step: undo takes that back
    if (!m_document.undo())
        return false;
    documentReplaced();
    emit documentEdited();
    return true;
}

bool InkCanvas::redo()
{
    if (m_readOnly || isDrawing())
        return false;
    commitText();
    if (!m_document.redo())
        return false;
    documentReplaced();
    emit documentEdited();
    return true;
}

// --- painting ---------------------------------------------------------------------

void InkCanvas::invalidateTiles(const QRectF &documentRect)
{
    const QRectF w = widgetRect(documentRect);
    const double k = m_tilesZoom / m_zoom; // widgetRect is at the current zoom, the cache's grid at the tiles' zoom
    m_tiles.invalidate(QRectF(w.x() * k, w.y() * k, w.width() * k, w.height() * k));
}

void InkCanvas::settleZoom()
{
    if (qFuzzyCompare(m_zoom, m_tilesZoom))
        return; // pinched out and back: the tiles are right as they are
    m_ghostScale = m_tilesZoom;
    m_tilesZoom = m_zoom;
    m_tiles.setScale(m_tilesZoom, devicePixelRatioF()); // every tile is stale; the old images keep standing in
    update();
}

namespace {
/// The cells of a grid whose cells are `side` wide (widget px) that the widget-px rect touches, within the content.
QRect cellsTouching(const QRectF &area, double side, const QSize &content)
{
    const int x0 = std::max(0, int(std::floor(area.left() / side)));
    const int y0 = std::max(0, int(std::floor(area.top() / side)));
    const int x1 = std::min(int(std::ceil(content.width() / side)) - 1, int(std::floor((area.right() - 1e-6) / side)));
    const int y1 = std::min(int(std::ceil(content.height() / side)) - 1, int(std::floor((area.bottom() - 1e-6) / side)));
    return QRect(QPoint(x0, y0), QPoint(x1, y1));
}
} // namespace

bool InkCanvas::tilesFresh(const QRect &widgetArea) const
{
    const double side = TileCache::kTileSize / m_tiles.devicePixelRatio() * m_zoom / m_tilesZoom;
    const QRect cells = cellsTouching(widgetArea, side, size());
    for (int ty = cells.top(); ty <= cells.bottom(); ++ty)
        for (int tx = cells.left(); tx <= cells.right(); ++tx)
            if (!m_tiles.isFresh(tx, ty))
                return false;
    return true;
}

void InkCanvas::onTileReady(int tx, int ty)
{
    const double side = TileCache::kTileSize / m_tiles.devicePixelRatio() * m_zoom / m_tilesZoom;
    update(QRectF(tx * side, ty * side, side, side).toAlignedRect());
    if ((m_holdLive || m_holdShape) && tilesFresh(m_holdRect)) {
        m_holdLive = false;
        m_holdShape.reset();
        update(m_holdRect);
    }
}

/// Everything a tile job paints, copied here on the UI thread. The job itself is pure data.
std::function<QImage()> InkCanvas::tileJob(int tx, int ty) const
{
    const double side = TileCache::kTileSize / m_tiles.devicePixelRatio();
    TileSpec spec{geometryOf(m_document, m_tilesZoom), QRectF(tx * side, ty * side, side, side), m_tiles.devicePixelRatio(), {}};
    for (const Geometry::Slice &s : spec.geometry.slices(spec.rect.toAlignedRect())) {
        auto &items = spec.items[s.page];
        for (const Item *item : m_document.paintOrder(s.exposed.adjusted(-4, -4, 4, 4))) {
            if (item->type == Item::Type::Stroke)
                items.push_back({item->stroke, nullptr});
            else if (item->type == Item::Type::Shape)
                items.push_back({nullptr, item->shape});
            // a text box or an image is painted over the tiles by the canvas itself, on the UI thread
        }
    }
    return [spec = std::move(spec), hook = m_tileJobHook] {
        if (hook)
            hook();
        return renderTile(spec);
    };
}

/// Paints the tiles over `area` and asks the pool for the missing and stale ones; never renders.
/// A tile (tx, ty) is a cell of the grid at the tiles' zoom; during a pinch the widget is at m_zoom and the cells are
/// drawn scaled by m_zoom / m_tilesZoom. After a settle the old images still sit in their old grid's cells (they say
/// at what zoom they were rendered): those are painted scaled underneath until the new tiles replace them.
void InkCanvas::drawTiles(QPainter &painter, const QRect &area)
{
    const double dpr = devicePixelRatioF();
    if (!qFuzzyCompare(dpr, m_tiles.devicePixelRatio()))
        m_tiles.setScale(m_tilesZoom, dpr); // moved to another screen: the cells change size
    const double side = TileCache::kTileSize / dpr;
    const double f = m_zoom / m_tilesZoom; // 1 unless a pinch is pending
    const bool scaled = !qFuzzyCompare(f, 1.0);
    painter.save();
    painter.setRenderHint(QPainter::SmoothPixmapTransform, scaled);
    const auto target = [scaled](const QRectF &r) { return scaled ? QRectF(r.toAlignedRect()) : r; }; // scaled cells overlap by a pixel, never leave a hairline

    struct Cell {
        int tx, ty;
        const QImage *image;
    };
    std::vector<Cell> cells;
    bool needGhosts = false;
    const QRect grid = cellsTouching(area, side * f, size());
    for (int ty = grid.top(); ty <= grid.bottom(); ++ty)
        for (int tx = grid.left(); tx <= grid.right(); ++tx) {
            const TileCache::Lookup hit = m_tiles.find(tx, ty);
            const bool own = hit.image && qFuzzyCompare(hit.scale, m_tilesZoom); // an image of an older zoom belongs to the old grid
            cells.push_back({tx, ty, own ? hit.image : nullptr});
            needGhosts |= !hit.fresh;
        }
    if (needGhosts && m_ghostScale > 0 && !qFuzzyCompare(m_ghostScale, m_tilesZoom)) {
        const double ghostSide = side * m_zoom / m_ghostScale;
        const QRect old = cellsTouching(area, ghostSide, size());
        for (int ty = old.top(); ty <= old.bottom(); ++ty)
            for (int tx = old.left(); tx <= old.right(); ++tx) {
                const TileCache::Lookup hit = m_tiles.find(tx, ty);
                if (hit.image && qFuzzyCompare(hit.scale, m_ghostScale))
                    painter.drawImage(QRectF(tx * ghostSide, ty * ghostSide, ghostSide, ghostSide).toAlignedRect(), *hit.image);
            }
    }
    for (const Cell &cell : cells)
        if (cell.image)
            painter.drawImage(target(QRectF(cell.tx * side * f, cell.ty * side * f, side * f, side * f)), *cell.image);
    painter.restore();
    if (scaled)
        return; // the pinch may not be over: nothing is rendered for a zoom that may still change
    // The visible tiles first, then one ring around them so a pan finds its tiles rendered (M1).
    const QRect visible = visibleRegion().boundingRect();
    const QRectF seen = QRectF(area | visible);
    const QRect visibleCells = cellsTouching(seen, side, size());
    const QRect ringCells = cellsTouching(seen.adjusted(-side, -side, side, side), side, size());
    const auto count = [](const QRect &r) { return qint64(std::max(0, r.width())) * std::max(0, r.height()); };
    constexpr qsizetype tileBytes = qsizetype(TileCache::kTileSize) * TileCache::kTileSize * 4;
    // A budget under what is on screen would evict wanted tiles and render them again for ever: the visible
    // tiles always have room (plus one), the ring only when the user's budget holds it too.
    m_tiles.setMaxBytes(std::max(m_userCacheBytes, qsizetype(count(visibleCells) + 1) * tileBytes));
    const bool ringFits = count(ringCells) * tileBytes <= m_userCacheBytes;
    m_tiles.keepOnly(ringFits ? ringCells : visibleCells); // a fast scroll must not leave renders queued for the cells it passed
    const auto wanted = [&](const QRect &cells, bool paintedRecently) {
        for (int ty = cells.top(); ty <= cells.bottom(); ++ty)
            for (int tx = cells.left(); tx <= cells.right(); ++tx) {
                if (paintedRecently)
                    m_tiles.find(tx, ty); // visible tiles are the last to go
                if (!m_tiles.isFresh(tx, ty) && !m_tiles.isPending(tx, ty))
                    m_tiles.request(tx, ty, tileJob(tx, ty));
            }
    };
    wanted(visibleCells, true);
    if (ringFits)
        wanted(ringCells, false); // the visible ones are fresh or pending by now: nothing is asked twice
}

void InkCanvas::paintEvent(QPaintEvent *event)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const QColor background = m_document.settings().background.color;
    const bool paged = pageGap() > 0;
    // ContinuousVertical: the window colour shows in the gaps between pages. The other layouts are one sheet.
    painter.fillRect(event->rect(), paged ? palette().color(QPalette::Window) : background);
    const auto [firstPage, lastPage] = visiblePages(event->rect());
    if (paged)
        for (int i = firstPage; i <= lastPage; ++i)
            painter.fillRect(pageWidgetRect(i) & event->rect(), background);
    drawTiles(painter, event->rect()); // the page pattern and the committed strokes and shapes
    paintUIItems(painter, event->rect()); // text boxes and image placeholders: not tile content
    if (m_drawing == Drawing::Pen) {
        const auto &prediction = m_input.prediction();
        if (!prediction.empty()) {
            Stroke tail;
            tail.style = m_input.stroke().style;
            tail.points.push_back(m_input.stroke().points.back());
            tail.points.insert(tail.points.end(), prediction.begin(), prediction.end());
            geometryOf(m_document, m_zoom).forEachPage(painter, event->rect(), [&](const QRectF &, int) {
                painter.setOpacity(0.6);
                paintStroke(painter, tail);
                painter.setOpacity(1.0);
            });
        }
    }
    if (paged) {
        painter.setRenderHint(QPainter::Antialiasing, false);
        const int pages = m_document.pageCount();
        for (int i = firstPage; i <= lastPage; ++i) {
            const QRect page = pageWidgetRect(i);
            painter.setPen(palette().color(QPalette::Mid));
            painter.setBrush(Qt::NoBrush);
            painter.drawRect(page.adjusted(0, 0, -1, -1));
            if (i + 1 < pages) { // the shadow falls into the gap below the page, fading over 4 px
                const int gapTop = page.bottom() + 1;
                const int depth = std::min(kShadowDepth, pageWidgetRect(i + 1).top() - gapTop);
                QLinearGradient shadow(0, gapTop, 0, gapTop + kShadowDepth);
                shadow.setColorAt(0, QColor(0, 0, 0, 40));
                shadow.setColorAt(1, QColor(0, 0, 0, 0));
                painter.fillRect(QRect(page.left(), gapTop, page.width(), depth) & event->rect(), shadow);
            }
        }
        painter.setRenderHint(QPainter::Antialiasing, true);
    }
    if (m_drawing == Drawing::Shape)
        paintShapeOver(painter, m_shapeDraft);
    else if (m_holdShape)
        paintShapeOver(painter, *m_holdShape);
    paintSelection(painter);
    if ((m_drawing == Drawing::Pen || m_holdLive) && !m_live.isNull()) {
        const QRect target = event->rect() & m_liveRect;
        const qreal dpr = m_live.devicePixelRatio();
        if (!target.isEmpty())
            painter.drawImage(target, m_live, QRectF(QPointF(target.topLeft() - m_liveRect.topLeft()) * dpr, QSizeF(target.size()) * dpr));
    }
}

} // namespace noteAppC::ink
