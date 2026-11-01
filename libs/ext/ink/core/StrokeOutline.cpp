#include "StrokeOutline.h"

#include <algorithm>
#include <cmath>

#include <QPainter>

#include "InkTools.h"

namespace noteAppC::ink {

namespace {

/// The width at point `index`, always finite and in [0, kMaxOutlineWidth]. A
/// negative style width counts as its absolute value; a non-finite pressure
/// falls back to the base width; a NaN base width is no ink; infinity clamps.
double outlineWidth(const Stroke &stroke, size_t index)
{
    const double base = std::fabs(stroke.style.width);
    if (std::isnan(base))
        return 0.0;
    const double clampedBase = std::min(base, kMaxOutlineWidth);
    const double w = std::isfinite(stroke.points[index].pressure)
                         ? applyPressureCurve(stroke.style.pressureCurve, clampedBase, stroke.points[index].pressure)
                         : clampedBase;
    return std::isfinite(w) ? std::clamp(w, 0.0, kMaxOutlineWidth) : 0.0;
}

} // namespace

// Ported from Rnote (rnote-compose/src/style/smooth/mod.rs, compose_lines_variable_width),
// copyright the Rnote contributors, GPL-3.0-or-later, https://github.com/flxzt/rnote.
// Each segment becomes a closed polygon - offset points at half the width on either side,
// a cubic round cap at both ends whose control points sit 2/3 of the width beyond the end -
// appended into one path filled with the non-zero rule.
QPainterPath strokeOutline(const Stroke &stroke, size_t firstPoint, size_t lastPoint)
{
    QPainterPath path;
    path.setFillRule(Qt::WindingFill);
    const std::vector<StrokePoint> &pts = stroke.points;
    if (pts.empty() || firstPoint > lastPoint || lastPoint >= pts.size())
        return path;
    path.reserve(int(std::min<size_t>(9 * (lastPoint - firstPoint) + 8, 1u << 20))); // 9 elements per segment
    bool anySegment = false;
    for (size_t i = firstPoint; i < lastPoint; ++i) {
        const QPointF a(pts[i].x, pts[i].y), b(pts[i + 1].x, pts[i + 1].y);
        const QPointF d = b - a;
        const double length = std::hypot(d.x(), d.y());
        if (!(length > 0.0) || !std::isfinite(length))
            continue; // a duplicate sample has no direction (skipped); a non-finite point has none either
        const double w0 = outlineWidth(stroke, i), w1 = outlineWidth(stroke, i + 1);
        const QPointF dir = d / length;
        const QPointF orth(-dir.y(), dir.x());
        const QPointF aPos = a + orth * (w0 * 0.5), aNeg = a - orth * (w0 * 0.5);
        const QPointF bPos = b + orth * (w1 * 0.5), bNeg = b - orth * (w1 * 0.5);
        if (!std::isfinite(aPos.x() + aPos.y() + aNeg.x() + aNeg.y() + bPos.x() + bPos.y() + bNeg.x() + bNeg.y()))
            continue; // only this segment drops; QPainterPath would silently ignore the bad point and garble the rest
        anySegment = true;
        if (w0 > 0.0) {
            path.moveTo(aNeg);
            path.cubicTo(aNeg - dir * (w0 * 2.0 / 3.0), aPos - dir * (w0 * 2.0 / 3.0), aPos);
        } else {
            path.moveTo(aPos);
        }
        path.lineTo(bPos);
        if (w1 > 0.0)
            path.cubicTo(bPos + dir * (w1 * 2.0 / 3.0), bNeg + dir * (w1 * 2.0 / 3.0), bNeg);
        else
            path.lineTo(bNeg);
        path.lineTo(aNeg);
        path.closeSubpath();
    }
    if (!anySegment) {
        // A tap, or every point the same: a circle of the start width.
        const double radius = outlineWidth(stroke, firstPoint) * 0.5;
        if (radius > 0.0 && std::isfinite(pts[firstPoint].x + pts[firstPoint].y))
            path.addEllipse(QPointF(pts[firstPoint].x, pts[firstPoint].y), radius, radius);
    }
    return path;
}

QPainterPath strokeOutline(const Stroke &stroke)
{
    return stroke.points.empty() ? QPainterPath() : strokeOutline(stroke, 0, stroke.points.size() - 1);
}

void paintStroke(QPainter &painter, const Stroke &stroke)
{
    painter.setPen(Qt::NoPen);
    painter.setBrush(stroke.style.color);
    painter.drawPath(strokeOutline(stroke));
}

void paintShape(QPainter &painter, const Shape &shape)
{
    painter.setPen(Qt::NoPen);
    painter.setBrush(shape.style.color);
    painter.drawPath(shapePath(shape));
}

QImage renderStrokes(const std::vector<std::shared_ptr<const Stroke>> &strokes, const QRectF &documentRect, double zoom,
                     double devicePixelRatio, const QColor &background)
{
    const QSize logical(int(std::ceil(documentRect.width() * zoom)), int(std::ceil(documentRect.height() * zoom)));
    QImage image(logical * devicePixelRatio, QImage::Format_ARGB32_Premultiplied);
    if (image.isNull())
        return image; // empty or absurd size: nothing to paint on
    image.setDevicePixelRatio(devicePixelRatio);
    image.fill(background);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.scale(zoom, zoom);
    painter.translate(-documentRect.topLeft());
    for (const auto &stroke : strokes)
        if (stroke)
            paintStroke(painter, *stroke);
    return image;
}

} // namespace noteAppC::ink
