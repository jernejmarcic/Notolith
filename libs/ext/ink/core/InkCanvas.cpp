#include "InkCanvas.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include <QFocusEvent>
#include <QGestureEvent>
#include <QKeyEvent>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QPinchGesture>
#include <QTabletEvent>
#include <QTouchEvent>

#include <map>

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
    std::shared_ptr<const Stroke> stroke; ///< null: a shape, text box or image (a placeholder until Tasks 14-15)
    QRectF bounds;
};

struct TileSpec {
    Geometry geometry; ///< at the zoom the tile is rendered for
    QRectF rect;       ///< logical px of the tile space: where the tile sits in the widget at that zoom
    double dpr = 1.0;
    std::map<int, std::vector<TileItem>> items; ///< by page, in paint order
};

/// Runs on a pool thread: QImage + QPainter only (R5). Page background, pattern and items, transparent in the gaps of
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
                painter.fillRect(item.bounds, QColor(0, 0, 0, 12));
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
    setCursor(Qt::CrossCursor);
    documentReplaced();
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
    m_live = QImage();
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

void InkCanvas::setTool(Tool tool)
{
    if (tool == m_tool)
        return;
    m_tool = tool;
    emit toolChanged(m_tool);
}

void InkCanvas::setReadOnly(bool on)
{
    if (on)
        finishStroke(true);
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
    event->accept(); // no synthesised mouse events for this pen
    if (m_readOnly)
        return;
    const double seconds = event->timestamp() / 1000.0;
    const QPointF pos = event->position() - kFractionalScaleOffset;
    // R14: this device's learned maximum turns raw pressure into 0..1 (PADD never exceeds 0.657). Only
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
        pressAt(pos, normaliser->normalise(event->pressure()), seconds, eraseByDevice || eraseByButton || m_tool == Tool::Eraser);
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
        if (event->button() == Qt::LeftButton && m_tabletActive)
            releaseAt(pos, seconds);
        break;
    default:
        break;
    }
    if (normaliser && normaliser->takeChanged()) // learning moved the maximum: tell the owner once
        emit learnedPressureChanged(device, normaliser->learnedMax());
}

void InkCanvas::mousePressEvent(QMouseEvent *event)
{
    if (m_readOnly || !m_drawWithMouse || m_tabletActive || event->button() != Qt::LeftButton)
        return QWidget::mousePressEvent(event);
    pressAt(event->position(), 0.5, event->timestamp() / 1000.0, m_tool == Tool::Eraser);
}

void InkCanvas::mouseMoveEvent(QMouseEvent *event)
{
    if (m_drawing == Drawing::None || m_tabletActive)
        return QWidget::mouseMoveEvent(event);
    moveTo(event->position(), 0.5, event->timestamp() / 1000.0);
}

void InkCanvas::mouseReleaseEvent(QMouseEvent *event)
{
    if (m_drawing == Drawing::None || m_tabletActive)
        return QWidget::mouseReleaseEvent(event);
    releaseAt(event->position(), event->timestamp() / 1000.0);
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
    if (m_readOnly) {
        QWidget::keyPressEvent(event);
    } else if (event->matches(QKeySequence::Undo)) {
        undo();
    } else if (event->matches(QKeySequence::Redo) || (event->key() == Qt::Key_Y && event->modifiers() == Qt::ControlModifier)) {
        redo();
    } else if (event->key() == Qt::Key_E && event->modifiers() == Qt::ShiftModifier) {
        setTool(Tool::Eraser); // R14: the whole/parts switch without a toolbar or a pen button
        setEraserMode(m_eraserMode == EraserMode::Split ? EraserMode::Stroke : EraserMode::Split);
    } else if (event->modifiers() == Qt::NoModifier && toolForKey(event->key(), &keyTool)) {
        setTool(keyTool); // R14: every tool without pen hardware (PADD's pen has no eraser end and dead buttons)
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
    finishStroke(true);
    QWidget::hideEvent(event);
}

void InkCanvas::changeEvent(QEvent *event)
{
    if (event->type() == QEvent::EnabledChange && !isEnabled())
        finishStroke(true);
    QWidget::changeEvent(event);
}

// --- the stroke in flight ----------------------------------------------------

void InkCanvas::pressAt(const QPointF &widgetPos, double pressure, double seconds, bool erase)
{
    if (m_drawing != Drawing::None)
        finishStroke(true); // a press without the release: close the old stroke where IT ended, not at this press
    setFocus(Qt::MouseFocusReason);
    m_holdLive = false;
    m_lastPos = widgetPos;
    m_lastPressure = pressure;
    m_lastSeconds = seconds;
    if (erase) {
        m_drawing = Drawing::Eraser;
        m_erasedSomething = false;
        m_lastErase = toDocument(widgetPos);
        eraseSweep(m_lastErase, m_lastErase);
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
        // Pen strokes are always Layer::User; Layer::Highlighter is only read from .rnote (Spec 2).
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

/// The eraser is the segment a-b swept by half its width (document px): a fast pen skips no
/// stroke between two samples. Candidates come from the spatial grid; a stroke is tested on its
/// polyline, never by building its outline.
void InkCanvas::eraseSweep(const QPointF &a, const QPointF &b)
{
    const double r = m_eraserWidth * 0.5;
    const QRectF area = QRectF(a, b).normalized().adjusted(-r, -r, r, r);
    QRectF dirty;
    QPainterPath eraser; // built on the first stroke that needs it, Split mode only
    for (qint64 t : m_document.itemsIntersecting(area)) {
        const Item *item = m_document.item(t);
        if (!item)
            continue;
        const bool isStroke = item->type == Item::Type::Stroke;
        if (isStroke ? !eraserTouches(*item->stroke, a, b, r) : !item->bounds().intersects(area))
            continue;
        if (isStroke && m_eraserMode == EraserMode::Split) {
            if (eraser.isEmpty())
                eraser = eraserCapsule(a, b, r);
            const SplitResult split = splitStroke(*item->stroke, eraser);
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
    if (m_readOnly || isDrawing() || !m_document.undo())
        return false;
    documentReplaced();
    emit documentEdited();
    return true;
}

bool InkCanvas::redo()
{
    if (m_readOnly || isDrawing() || !m_document.redo())
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
    if (m_holdLive && tilesFresh(m_holdRect)) {
        m_holdLive = false;
        update(m_holdRect);
    }
}

/// Everything a tile job paints, copied here on the UI thread. The job itself is pure data (R5).
std::function<QImage()> InkCanvas::tileJob(int tx, int ty) const
{
    const double side = TileCache::kTileSize / m_tiles.devicePixelRatio();
    TileSpec spec{geometryOf(m_document, m_tilesZoom), QRectF(tx * side, ty * side, side, side), m_tiles.devicePixelRatio(), {}};
    for (const Geometry::Slice &s : spec.geometry.slices(spec.rect.toAlignedRect())) {
        auto &items = spec.items[s.page];
        for (const Item *item : m_document.paintOrder(s.exposed.adjusted(-4, -4, 4, 4))) {
            if (item->type == Item::Type::Stroke)
                items.push_back({item->stroke, item->bounds()});
            else if (item->type == Item::Type::Shape || item->type == Item::Type::TextBox || item->type == Item::Type::Image)
                items.push_back({nullptr, item->bounds()});
        }
    }
    return [spec = std::move(spec), hook = m_tileJobHook] {
        if (hook)
            hook();
        return renderTile(spec);
    };
}

/// Paints the tiles over `area` and asks the pool for the missing and stale ones; never renders (R5).
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
    // A budget under what is on screen would evict wanted tiles and render them again for ever (I1): the visible
    // tiles always have room (plus one), the ring only when the user's budget holds it too.
    m_tiles.setMaxBytes(std::max(m_userCacheBytes, qsizetype(count(visibleCells) + 1) * tileBytes));
    const bool ringFits = count(ringCells) * tileBytes <= m_userCacheBytes;
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
    drawTiles(painter, event->rect()); // the page pattern and the committed items
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
    if ((m_drawing == Drawing::Pen || m_holdLive) && !m_live.isNull()) {
        const QRect target = event->rect() & m_liveRect;
        const qreal dpr = m_live.devicePixelRatio();
        if (!target.isEmpty())
            painter.drawImage(target, m_live, QRectF(QPointF(target.topLeft() - m_liveRect.topLeft()) * dpr, QSizeF(target.size()) * dpr));
    }
}

} // namespace noteAppC::ink
