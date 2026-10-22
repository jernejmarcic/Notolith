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

#include "StrokeOutline.h"

namespace noteAppC::ink {

namespace {

constexpr double kPageEdgeSlack = 1e-9; ///< relative: page edges are page * height, not exact for a fractional height or zoom
constexpr int kShadowDepth = 4;       ///< widget px: the shadow under a page in ContinuousVertical
constexpr int kMaxOverlaySide = 8192; ///< widget px: the live overlay never grows past this, whatever the widget's size

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

} // namespace

InkCanvas::InkCanvas(QWidget *parent) : QWidget(parent)
{
    setFocusPolicy(Qt::StrongFocus);
    setTabletTracking(true); // hover events while the pen is near the tablet (spec section 7, Sources)
    setAttribute(Qt::WA_AcceptTouchEvents);
    setAttribute(Qt::WA_OpaquePaintEvent);
    grabGesture(Qt::PinchGesture);
    setCursor(Qt::CrossCursor);
    documentReplaced();
}

QSize InkCanvas::sizeHint() const
{
    const QRectF extent = m_document.settings().extent;
    const double height = extent.height() + (m_document.pageCount() - 1) * pageGap();
    // A QWidget cannot be larger than QWIDGETSIZE_MAX; the clamp also keeps the int conversion defined for a hostile extent.
    const auto px = [this](double documentPx) { return int(std::min(std::ceil(documentPx * m_zoom), double(QWIDGETSIZE_MAX))); };
    return QSize(px(extent.width()), px(height));
}

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

QRect InkCanvas::pageWidgetRect(int index) const
{
    const double h = m_document.settings().format.height;
    const double top = index * (h + pageGap());
    const int topPx = qRound(top * m_zoom);
    const int bottom = std::min(qRound((top + h) * m_zoom), sizeHint().height()); // the last page may be cut by the extent
    return QRect(0, topPx, sizeHint().width(), bottom - topPx);
}

std::pair<int, int> InkCanvas::visiblePages(const QRect &area) const
{
    const double stride = (m_document.settings().format.height + pageGap()) * m_zoom;
    const int last = m_document.pageCount() - 1;
    if (pageGap() <= 0 || !(stride > 0))
        return {0, 0};
    return {std::clamp(int(std::floor(area.top() / stride)), 0, last), std::clamp(int(std::floor(area.bottom() / stride)), 0, last)};
}

template <typename Draw>
void InkCanvas::forEachPage(QPainter &painter, const QRect &area, Draw draw) const
{
    const QRectF extent = m_document.settings().extent;
    const double gap = pageGap();
    const auto [first, last] = visiblePages(area);
    for (int i = first; i <= last; ++i) {
        QRect shown = area;
        if (gap > 0) {
            shown = area & pageWidgetRect(i);
            if (shown.isEmpty())
                continue;
        }
        painter.save();
        if (gap > 0)
            painter.setClipRect(pageWidgetRect(i), Qt::IntersectClip);
        painter.scale(m_zoom, m_zoom);
        painter.translate(-extent.x(), -extent.y() + i * gap);
        draw(QRectF(shown.x() / m_zoom + extent.x(), shown.y() / m_zoom - i * gap + extent.y(), shown.width() / m_zoom,
                    shown.height() / m_zoom));
        painter.restore();
    }
}

void InkCanvas::documentReplaced()
{
    finishStroke(false); // the stroke in flight belongs to the document that went away
    m_live = QImage();
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
    resize(sizeHint());
    updateGeometry();
    update();
    emit zoomChanged(m_zoom);
}

QRect InkCanvas::widgetRect(const QRectF &documentRect) const
{
    return QRectF(toWidget(documentRect.topLeft()), toWidget(documentRect.bottomRight())).toAlignedRect().adjusted(-2, -2, 2, 2);
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
    want.setSize(want.size().boundedTo(QSize(kMaxOverlaySide, kMaxOverlaySide)));
    const qreal dpr = devicePixelRatio();
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
    forEachPage(painter, widgetRect(path.boundingRect()), [&](const QRectF &) { painter.drawPath(path); });
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
    for (qint64 t : m_document.itemsIntersecting(area)) {
        const Item *item = m_document.item(t);
        if (!item)
            continue;
        const bool hit = item->type == Item::Type::Stroke ? eraserTouches(*item->stroke, a, b, r) : item->bounds().intersects(area);
        if (!hit)
            continue;
        dirty |= item->bounds();
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

// --- painting ------------------------------------------------------------------

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
    // Track S1 Task 10 paints the page pattern here; Task 11 replaces the
    // item loop with the tile cache. In A0 the exposed rect's items are
    // painted straight from the document.
    forEachPage(painter, event->rect(), [&](const QRectF &exposed) {
        for (const Item *item : m_document.paintOrder(exposed.adjusted(-4, -4, 4, 4))) {
            if (item->type == Item::Type::Stroke)
                paintStroke(painter, *item->stroke);
            else if (item->type == Item::Type::Shape || item->type == Item::Type::TextBox || item->type == Item::Type::Image)
                painter.fillRect(item->bounds(), QColor(0, 0, 0, 12)); // placeholder until Tasks 14-15 draw them
        }
        if (m_drawing == Drawing::Pen) {
            const auto &prediction = m_input.prediction();
            if (!prediction.empty()) {
                Stroke tail;
                tail.style = m_input.stroke().style;
                tail.points.push_back(m_input.stroke().points.back());
                tail.points.insert(tail.points.end(), prediction.begin(), prediction.end());
                painter.setOpacity(0.6);
                paintStroke(painter, tail);
                painter.setOpacity(1.0);
            }
        }
    });
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
    if (m_drawing == Drawing::Pen && !m_live.isNull()) {
        const QRect target = event->rect() & m_liveRect;
        const qreal dpr = m_live.devicePixelRatio();
        if (!target.isEmpty())
            painter.drawImage(target, m_live, QRectF(QPointF(target.topLeft() - m_liveRect.topLeft()) * dpr, QSizeF(target.size()) * dpr));
    }
}

} // namespace noteAppC::ink
