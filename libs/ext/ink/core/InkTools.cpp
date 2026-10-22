#include "InkTools.h"

#include <algorithm>
#include <cmath>

#include <QJsonArray>
#include <QLineF>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPolygonF>

#include "InkFormat.h"
#include "StrokeOutline.h"

namespace noteAppC::ink {

// Ported from Rnote (rnote-engine/src/store/trash_comp.rs, split_colliding_strokes),
// copyright the Rnote contributors, GPL-3.0-or-later, https://github.com/flxzt/rnote.
// Rnote cuts a brush stroke's path at every segment the eraser hits and keeps each
// run of the others that has a segment left; the first run stays the original
// stroke. Here a segment is hit when its outline path meets the eraser path, and a
// run is the points joined by consecutive untouched segments.
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
            // Rnote: any point inside.
            const auto &pts = item->stroke->points;
            hit = std::any_of(pts.begin(), pts.end(), [&](const StrokePoint &p) { return region.contains(QPointF(p.x, p.y)); });
            // And a segment that crosses the region with no point inside it (a sparse stroke through a box): the brief's
            // "outline intersects", on the centreline. ponytail: a segment is tested against every edge of the region
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
        } else if (item->type == Item::Type::Shape || item->type == Item::Type::TextBox || item->type == Item::Type::Image) {
            hit = region.contains(item->bounds().center()); // ponytail: a real per-kind hit test comes with Tasks 14-15 (carry N5)
        }
        if (hit)
            out.push_back(t);
    }
    std::sort(out.begin(), out.end()); // the grid's order is not the document's
    return out;
}

// Ported from Rnote (rnote-engine/src/strokes/stroke.rs, Stroke::scale / Stroke::translate and
// rnote-compose/src/shapes), copyright the Rnote contributors, GPL-3.0-or-later,
// https://github.com/flxzt/rnote. Rnote scales the stroke width by the mean of the x and y scale.
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
    case Item::Type::TextBox:
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
    if (bytes.size() > kMaxInflatedBytes) // the clipboard is another program's data too
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
