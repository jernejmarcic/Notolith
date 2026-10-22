#include "StrokeOutline.h"

#include <cmath>

#include <QPainter>

namespace noteAppC::ink {

namespace {

bool allFinite(const QPainterPath &path)
{
    for (int i = 0; i < path.elementCount(); ++i) {
        const QPainterPath::Element e = path.elementAt(i);
        if (!std::isfinite(e.x) || !std::isfinite(e.y))
            return false;
    }
    return true;
}

} // namespace

// Ported from Rnote (rnote-compose/src/style/smooth/mod.rs,
// compose_lines_variable_width), copyright the Rnote contributors,
// GPL-3.0-or-later, https://github.com/flxzt/rnote. The page styles beyond the
// stock six are from Jernej's fork, branch jernej/features.
// Rnote composes each pen path segment into a closed polygon - offset points
// at half the width on either side, a cubic round cap at both ends whose
// control points sit 2/3 of the width beyond the end - and appends every
// segment into one path filled with the non-zero rule. Only LineTo segments
// exist in files the default builder writes (rnote-study.md section 1.6), so a
// segment here is one line.
QPainterPath strokeOutline(const Stroke &stroke, size_t firstPoint, size_t lastPoint)
{
    QPainterPath path;
    path.setFillRule(Qt::WindingFill);
    const std::vector<StrokePoint> &pts = stroke.points;
    if (pts.empty() || firstPoint > lastPoint || lastPoint >= pts.size())
        return path;
    bool anySegment = false;
    for (size_t i = firstPoint; i < lastPoint; ++i) {
        const QPointF a(pts[i].x, pts[i].y), b(pts[i + 1].x, pts[i + 1].y);
        const QPointF d = b - a;
        const double length = std::hypot(d.x(), d.y());
        if (!(length > 0.0) || !std::isfinite(length))
            continue; // a duplicate sample has no direction: skipped, as Rnote filters zero-length lines
        anySegment = true;
        const double w0 = stroke.widthAt(i), w1 = stroke.widthAt(i + 1);
        const QPointF dir = d / length;
        const QPointF orth(-dir.y(), dir.x());
        const QPointF aPos = a + orth * (w0 * 0.5), aNeg = a - orth * (w0 * 0.5);
        const QPointF bPos = b + orth * (w1 * 0.5), bNeg = b - orth * (w1 * 0.5);
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
        // A tap, or every point the same: Rnote draws a circle of the start width.
        const double radius = stroke.widthAt(firstPoint) * 0.5;
        if (radius > 0.0 && std::isfinite(radius))
            path.addEllipse(QPointF(pts[firstPoint].x, pts[firstPoint].y), radius, radius);
    }
    if (!allFinite(path)) {
        QPainterPath empty;
        empty.setFillRule(Qt::WindingFill);
        return empty;
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
