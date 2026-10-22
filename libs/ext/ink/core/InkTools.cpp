#include "InkTools.h"

#include <algorithm>
#include <cmath>

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

} // namespace noteAppC::ink
