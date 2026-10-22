#include "InkCanvas.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <utility>

#include <QGestureEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QPinchGesture>
#include <QTabletEvent>
#include <QTouchEvent>

#include "StrokeOutline.h"

namespace noteAppC::ink {

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
    return QSize(int(std::ceil(extent.width() * m_zoom)), int(std::ceil(extent.height() * m_zoom)));
}

void InkCanvas::documentReplaced()
{
    m_live = QImage();
    resize(sizeHint());
    updateGeometry();
    update();
}

void InkCanvas::setZoom(double zoom)
{
    zoom = std::clamp(zoom, 0.25, 8.0);
    if (qFuzzyCompare(zoom, m_zoom))
        return;
    m_zoom = zoom;
    m_live = QImage();
    resize(sizeHint());
    updateGeometry();
    update();
    emit zoomChanged(m_zoom);
}

QRect InkCanvas::widgetRect(const QRectF &documentRect) const
{
    return QRectF(documentRect.topLeft() * m_zoom, documentRect.size() * m_zoom).toAlignedRect().adjusted(-2, -2, 2, 2);
}

void InkCanvas::setTool(Tool tool)
{
    if (tool == m_tool)
        return;
    m_tool = tool;
    emit toolChanged(m_tool);
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
    // R14: this device's learned maximum turns raw pressure into 0..1 (PADD never exceeds 0.657).
    const QString device = event->pointingDevice() ? event->pointingDevice()->name() : QString();
    PressureNormaliser &normaliser = normaliserFor(device);
    const double pressure = normaliser.normalise(event->pressure());
    struct Persist { // after the switch: tell the owner once when learning moved the maximum
        InkCanvas *canvas; PressureNormaliser &normaliser; const QString &device;
        ~Persist() { if (normaliser.takeChanged()) emit canvas->learnedPressureChanged(device, normaliser.learnedMax()); }
    } persist{this, normaliser, device};
    const QPointF pos = event->position() - kFractionalScaleOffset;
    const bool eraseByDevice = kTrustEraserPointerType && event->pointerType() == QPointingDevice::PointerType::Eraser;
    const bool eraseByButton = event->buttons().testFlag(kHeldEraserButton);
    switch (event->type()) {
    case QEvent::TabletPress:
        m_tabletActive = true;
        pressAt(pos, pressure, seconds, eraseByDevice || eraseByButton || m_tool == Tool::Eraser);
        break;
    case QEvent::TabletMove:
        if (m_tabletActive)
            moveTo(pos, pressure, seconds);
        break;
    case QEvent::TabletRelease:
        if (m_tabletActive)
            releaseAt(pos, pressure, seconds);
        m_tabletActive = false;
        break;
    default:
        break;
    }
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
    releaseAt(event->position(), 0.5, event->timestamp() / 1000.0);
}

bool InkCanvas::event(QEvent *event)
{
    switch (event->type()) {
    case QEvent::TouchBegin:
    case QEvent::TouchUpdate:
    case QEvent::TouchEnd: {
        auto *touch = static_cast<QTouchEvent *>(event);
        touch->accept();
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

// --- the stroke in flight ----------------------------------------------------

void InkCanvas::pressAt(const QPointF &widgetPos, double pressure, double seconds, bool erase)
{
    if (m_drawing != Drawing::None)
        releaseAt(widgetPos, pressure, seconds); // a press without the release: close the old stroke first
    setFocus(Qt::MouseFocusReason);
    if (erase) {
        m_drawing = Drawing::Eraser;
        m_erasedSomething = false;
        eraseAround(toDocument(widgetPos));
        return;
    }
    m_drawing = Drawing::Pen;
    if (m_live.size() != size() * devicePixelRatio()) {
        m_live = QImage(size() * devicePixelRatio(), QImage::Format_ARGB32_Premultiplied);
        m_live.setDevicePixelRatio(devicePixelRatio());
    }
    m_live.fill(Qt::transparent);
    m_input.begin({toDocument(widgetPos), pressure, seconds}, m_penStyle);
    rasteriseNewSegments();
}

void InkCanvas::moveTo(const QPointF &widgetPos, double pressure, double seconds)
{
    if (m_drawing == Drawing::Eraser) {
        eraseAround(toDocument(widgetPos));
        return;
    }
    m_input.extend({toDocument(widgetPos), pressure, seconds});
    rasteriseNewSegments();
}

void InkCanvas::releaseAt(const QPointF &widgetPos, double pressure, double seconds)
{
    if (m_drawing == Drawing::Eraser) {
        eraseAround(toDocument(widgetPos));
        m_drawing = Drawing::None;
        if (m_erasedSomething)
            finishEdit();
        return;
    }
    if (m_drawing != Drawing::Pen)
        return;
    m_input.end({toDocument(widgetPos), pressure, seconds});
    m_input.takeNewPoints();
    std::shared_ptr<const Stroke> stroke = m_input.finish();
    m_drawing = Drawing::None;
    const QRectF tail = std::exchange(m_lastPrediction, QRectF()); // the tail goes with the pen: repaint where it was
    if (stroke && !stroke->points.empty()) {
        // Pen strokes are always Layer::User; Layer::Highlighter is only read from .rnote (Spec 2).
        // A highlighter tool does nothing over handwriting (spec section 2), so none exists here.
        m_document.addStroke(stroke, Layer::User);
        if (m_document.growToContain(stroke->bounds()))
            documentReplaced();
        finishEdit();
        update(widgetRect(stroke->bounds() | tail));
    } else {
        update();
    }
}

void InkCanvas::rasteriseNewSegments()
{
    const size_t fresh = m_input.takeNewPoints();
    const Stroke &stroke = m_input.stroke();
    if (fresh == 0 || stroke.points.empty())
        return;
    // The segments ending at the new points: from the last point of the
    // previous batch, so nothing is missed between batches.
    const size_t last = stroke.points.size() - 1;
    const size_t first = last + 1 >= fresh + 1 ? last - fresh : 0;
    const QPainterPath path = strokeOutline(stroke, first, last);
    QPainter painter(&m_live);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.scale(m_zoom, m_zoom);
    painter.setPen(Qt::NoPen);
    painter.setBrush(stroke.style.color);
    painter.drawPath(path);
    painter.end();
    QRectF tail; // the predicted tail's box: repainted now, and again where it was when it moves
    for (const StrokePoint &p : m_input.prediction())
        tail |= QRectF(p.x - stroke.style.width, p.y - stroke.style.width, 2 * stroke.style.width, 2 * stroke.style.width);
    const QRectF dirty = path.boundingRect() | tail | m_lastPrediction; // a null rect adds nothing to a union
    m_lastPrediction = tail;
    update(widgetRect(dirty));
}

void InkCanvas::eraseAround(const QPointF &documentPos)
{
    const double r = m_eraserWidth * 0.5;
    const QRectF area(documentPos.x() - r, documentPos.y() - r, 2 * r, 2 * r);
    QPainterPath circle;
    circle.addEllipse(area);
    QRectF dirty;
    for (qint64 t : m_document.itemsIntersecting(area)) {
        const Item *item = m_document.item(t);
        if (!item)
            continue;
        const bool hit = item->type == Item::Type::Stroke ? strokeOutline(*item->stroke).intersects(circle)
                                                          : item->bounds().intersects(area);
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
    painter.fillRect(event->rect(), m_document.settings().background.color);
    // Track S1 Task 10 paints the page pattern here; Task 11 replaces the
    // item loop with the tile cache. In A0 the exposed rect's items are
    // painted straight from the document.
    painter.save();
    painter.scale(m_zoom, m_zoom);
    const QRectF exposed(QPointF(event->rect().topLeft()) / m_zoom, QSizeF(event->rect().size()) / m_zoom);
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
    painter.restore();
    if (m_drawing == Drawing::Pen && !m_live.isNull())
        painter.drawImage(event->rect(), m_live, QRectF(QPointF(event->rect().topLeft()) * devicePixelRatio(),
                                                        QSizeF(event->rect().size()) * devicePixelRatio()));
}

} // namespace noteAppC::ink
