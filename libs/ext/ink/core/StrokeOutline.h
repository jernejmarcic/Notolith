// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej Marčič
#pragma once

#include <memory>
#include <vector>

#include <QColor>
#include <QImage>
#include <QPainterPath>

#include "InkTypes.h"

class QPainter;

namespace noteAppC::ink {

/// The widest ink a segment draws, in document px; a larger (or infinite) width
/// is clamped to it, so a hostile file cannot make a stroke vanish or overflow.
inline constexpr double kMaxOutlineWidth = 1e6;

/// One QPainterPath per stroke, filled once with Qt::WindingFill, so overlaps
/// never darken a translucent colour (spec section 7, Rendering). Each segment's
/// closed subpath (a quad at `width * curve(pressure)` with cubic round caps) is
/// appended into that one path.
///
/// Safe on hostile model data: a single point or all-equal points give a dot,
/// duplicate neighbours are skipped, zero width or pressure shrinks to a line,
/// and a path that would hold a non-finite element (overflowing coordinates or
/// width) comes back empty instead.
QPainterPath strokeOutline(const Stroke &stroke);
/// The segments between points `firstPoint` and `lastPoint` inclusive: what
/// the live stroke rasterises per tablet event.
QPainterPath strokeOutline(const Stroke &stroke, size_t firstPoint, size_t lastPoint);
void paintStroke(QPainter &painter, const Stroke &stroke);
/// The shape's ink (shapePath, InkTools.h) filled once in the style's colour.
void paintShape(QPainter &painter, const Shape &shape);
/// `documentRect` of the document at `zoom`, into an image of
/// `documentRect.size() * zoom * devicePixelRatio` physical pixels.
QImage renderStrokes(const std::vector<std::shared_ptr<const Stroke>> &strokes, const QRectF &documentRect, double zoom,
                     double devicePixelRatio, const QColor &background);

} // namespace noteAppC::ink
