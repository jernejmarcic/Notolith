// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej Marčič
#include "InkTools.h"

#include <algorithm>
#include <cmath>
#include <numbers>

#include <QJsonArray>
#include <QLineF>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainterPathStroker>
#include <QPolygonF>

#include "InkFormat.h"
#include "StrokeOutline.h"

namespace noteAppC::ink {

// Ported from Rnote (rnote-engine/src/store/trash_comp.rs, split_colliding_strokes),
// copyright the Rnote contributors, GPL-3.0-or-later, https://github.com/flxzt/rnote.
// A segment is hit when its outline path meets the eraser path; each run of points joined
// by untouched segments is kept, and the first run stays the original stroke.
SplitResult splitStroke(const Stroke &stroke, const QPainterPath &eraser)
{
    const std::vector<StrokePoint> &pts = stroke.points;
    const size_t n = pts.size();
    SplitResult result;
    if (n == 0 || eraser.isEmpty())
        return result;
    const QRectF eraserBox = eraser.boundingRect();
    // The cheap cull first (a stroke is thousands of segments, the outline test builds a path): the
    // segment's box grown by its widest half, plus a pixel for the round caps.
    const auto meets = [&](size_t i, size_t j) {
        const double half = 0.5 * std::max(std::fabs(stroke.widthAt(i)), std::fabs(stroke.widthAt(j))) + 1.0;
        const QRectF box = QRectF(QPointF(pts[i].x, pts[i].y), QPointF(pts[j].x, pts[j].y)).normalized().adjusted(-half, -half, half, half);
        return box.intersects(eraserBox) && strokeOutline(stroke, i, j).intersects(eraser);
    };
    if (n == 1) { // a tap: one dot, taken whole or not at all
        result.hit = meets(0, 0);
        return result;
    }
    std::vector<bool> cut(n - 1);
    for (size_t i = 0; i + 1 < n; ++i) {
        cut[i] = meets(i, i + 1);
        result.hit = result.hit || cut[i];
    }
    if (!result.hit)
        return result;
    for (size_t i = 0; i + 1 < n;) {
        if (cut[i]) {
            ++i;
            continue;
        }
        size_t end = i; // segments i..end-1 are untouched: points i..end
        while (end + 1 < n && !cut[end])
            ++end;
        auto piece = std::make_shared<Stroke>();
        piece->style = stroke.style;
        piece->points.assign(pts.begin() + i, pts.begin() + end + 1);
        result.pieces.push_back(std::move(piece));
        i = end;
    }
    return result;
}

std::vector<qint64> selectItems(const InkDocument &doc, const QPainterPath &region)
{
    std::vector<qint64> out;
    if (region.isEmpty())
        return out;
    std::vector<QLineF> edges; ///< the region's outline, built when the first stroke needs it
    for (qint64 t : doc.itemsIntersecting(region.boundingRect())) {
        const Item *item = doc.item(t);
        if (!item)
            continue;
        bool hit = false;
        if (item->type == Item::Type::Stroke) {
            // any point inside
            const auto &pts = item->stroke->points;
            hit = std::any_of(pts.begin(), pts.end(), [&](const StrokePoint &p) { return region.contains(QPointF(p.x, p.y)); });
            // And a segment that crosses the region with no point inside it (a sparse stroke through a box): the brief's
            // "outline intersects", on the centreline. Limit: a segment is tested against every edge of the region
            // (one edge list per call, a box cull per segment); a huge sparse stroke under a huge lasso is the ceiling.
            if (!hit && pts.size() >= 2) {
                if (edges.empty())
                    for (const QPolygonF &polygon : region.toFillPolygons())
                        for (qsizetype i = 0; i < polygon.size(); ++i)
                            edges.push_back(QLineF(polygon[i], polygon[(i + 1) % polygon.size()]));
                const QRectF box = region.boundingRect();
                for (size_t i = 0; i + 1 < pts.size() && !hit; ++i) {
                    const QLineF segment(pts[i].x, pts[i].y, pts[i + 1].x, pts[i + 1].y);
                    // by hand: QRectF::intersects is false for a flat rect, which a horizontal segment's box is
                    if (std::max(segment.x1(), segment.x2()) < box.left() || std::min(segment.x1(), segment.x2()) > box.right() ||
                        std::max(segment.y1(), segment.y2()) < box.top() || std::min(segment.y1(), segment.y2()) > box.bottom())
                        continue;
                    hit = std::any_of(edges.begin(), edges.end(), [&](const QLineF &edge) {
                        return segment.intersects(edge, nullptr) == QLineF::BoundedIntersection;
                    });
                }
            }
        } else if (item->type == Item::Type::TextBox || item->type == Item::Type::Image) {
            // The box itself: the region holds its centre, or crosses its edge. A region wholly inside the box
            // (ink drawn over a text box) takes the ink and not the box.
            const QRectF box = item->bounds();
            hit = region.contains(box.center()) || (region.intersects(box) && !box.contains(region.boundingRect()));
        } else if (item->type == Item::Type::Shape) {
            // Its ink: a lasso inside a rectangle that touches no edge selects nothing. Limit: one stroker run per
            // candidate shape (those whose box meets the region's box), a large drawing of shapes under one lasso is the ceiling.
            hit = shapePath(*item->shape).intersects(region);
        }
        if (hit)
            out.push_back(t);
    }
    std::sort(out.begin(), out.end()); // the grid's order is not the document's
    return out;
}

/// The arrow's head: two barbs, each this angle (radians) off the line, running back from the tip.
constexpr double kArrowHeadHalfAngle = std::numbers::pi / 6.0;

QPainterPath shapePath(const Shape &shape)
{
    const std::vector<QPointF> &pts = shape.points;
    const double width = std::fabs(shape.style.width); // as a stroke: a negative width counts as its absolute value
    if (pts.size() < 2 || !(width > 0.0))             // NaN fails the comparison too
        return QPainterPath();
    for (const QPointF &p : pts)
        if (!std::isfinite(p.x()) || !std::isfinite(p.y()))
            return QPainterPath();
    const double w = std::min(width, kMaxOutlineWidth);
    QPainterPath centre;
    switch (shape.kind) {
    case ShapeKind::Line:
    case ShapeKind::Arrow: {
        centre.moveTo(pts[0]);
        centre.lineTo(pts[1]);
        const QPointF d = pts[1] - pts[0];
        const double length = std::hypot(d.x(), d.y());
        if (shape.kind == ShapeKind::Arrow && length > 0.0) { // an open head: barb, tip, barb
            const QPointF back = -d / length * (kArrowHeadLengthInWidths * w);
            const auto turned = [&back](double angle) {
                return QPointF(back.x() * std::cos(angle) - back.y() * std::sin(angle), back.x() * std::sin(angle) + back.y() * std::cos(angle));
            };
            centre.moveTo(pts[1] + turned(kArrowHeadHalfAngle));
            centre.lineTo(pts[1]);
            centre.lineTo(pts[1] + turned(-kArrowHeadHalfAngle));
        }
        break;
    }
    case ShapeKind::Rect:
        centre.addRect(QRectF(pts[0], pts[1]).normalized());
        break;
    case ShapeKind::Ellipse:
        centre.addEllipse(QRectF(pts[0], pts[1]).normalized());
        break;
    case ShapeKind::Polyline:
    case ShapeKind::Polygon:
        centre.moveTo(pts[0]);
        for (size_t i = 1; i < pts.size(); ++i)
            centre.lineTo(pts[i]);
        if (shape.kind == ShapeKind::Polygon)
            centre.closeSubpath();
        break;
    }
    QPainterPathStroker stroker;
    stroker.setWidth(w);
    stroker.setCapStyle(Qt::RoundCap);
    stroker.setJoinStyle(Qt::RoundJoin);
    QPainterPath outline = stroker.createStroke(centre);
    outline.setFillRule(Qt::WindingFill);
    for (int i = 0; i < outline.elementCount(); ++i) // a path that holds a non-finite element would garble the fill
        if (!std::isfinite(outline.elementAt(i).x) || !std::isfinite(outline.elementAt(i).y))
            return QPainterPath();
    return outline;
}

// Ported from Rnote (rnote-engine/src/strokes/stroke.rs, Stroke::scale / Stroke::translate),
// copyright the Rnote contributors, GPL-3.0-or-later, https://github.com/flxzt/rnote.
// The width scales by the mean of the x and y scale.
Item transformItem(const Item &item, const QTransform &transform)
{
    const double meanScale = 0.5 * (std::hypot(transform.m11(), transform.m12()) + std::hypot(transform.m21(), transform.m22()));
    Item out = item;
    switch (item.type) {
    case Item::Type::Stroke: {
        auto stroke = std::make_shared<Stroke>(*item.stroke);
        for (StrokePoint &p : stroke->points) {
            const QPointF q = transform.map(QPointF(p.x, p.y));
            p.x = q.x();
            p.y = q.y();
        }
        stroke->style.width *= meanScale;
        out.stroke = std::move(stroke);
        break;
    }
    case Item::Type::Shape: {
        auto shape = std::make_shared<Shape>(*item.shape);
        for (QPointF &p : shape->points)
            p = transform.map(p);
        shape->style.width *= meanScale;
        out.shape = std::move(shape);
        break;
    }
    case Item::Type::TextBox: // the box is mapped, the text is not scaled: a resized box reflows its Markdown at the same type size
        out.textBox = std::make_shared<TextBox>(TextBox{transform.mapRect(item.textBox->rect), item.textBox->markdown});
        break;
    case Item::Type::Image:
        out.image = std::make_shared<ImageRef>(ImageRef{transform.mapRect(item.image->rect), item.image->path});
        break;
    case Item::Type::Group:
    case Item::Type::Unknown:
        break; // nothing to map: the item is kept as it was read
    }
    return out;
}

QByteArray itemsToClipboardJSON(const std::vector<const Item *> &items)
{
    QJsonArray array;
    for (const Item *item : items)
        array.append(itemToJSON(*item));
    return QJsonDocument(QJsonObject{{QStringLiteral("format"), QStringLiteral("notolith-ink-clip")}, {QStringLiteral("items"), array}})
        .toJson(QJsonDocument::Compact);
}

std::vector<Item> itemsFromClipboardJSON(const QByteArray &bytes, bool *ok)
{
    std::vector<Item> out;
    const auto fail = [&] {
        if (ok)
            *ok = false;
        return std::vector<Item>();
    };
    if (bytes.size() > kMaxClipboardBytes) // the clipboard is another program's data too, and this runs on the UI thread
        return fail();
    const QJsonDocument json = QJsonDocument::fromJson(bytes);
    if (!json.isObject() || json.object().value(QStringLiteral("format")).toString() != QLatin1String("notolith-ink-clip")
        || !json.object().value(QStringLiteral("items")).isArray())
        return fail();
    for (const QJsonValue &value : json.object().value(QStringLiteral("items")).toArray()) {
        Item item;
        QString why;
        if (!value.isObject() || !itemFromJSON(value.toObject(), &item, &why))
            return fail();
        out.push_back(std::move(item));
    }
    if (ok)
        *ok = true;
    return out;
}

} // namespace noteAppC::ink
