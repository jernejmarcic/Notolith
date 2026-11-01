// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej
#pragma once

#include <memory>
#include <vector>

#include <QByteArray>
#include <QPainterPath>
#include <QTransform>

#include "InkDocument.h"
#include "InkTypes.h"

/// Pure functions over the ink model that the tools use (Tasks 12-15 add to it).
namespace noteAppC::ink {

struct SplitResult {
    bool hit = false;
    std::vector<std::shared_ptr<const Stroke>> pieces; ///< the surviving runs, in order, with the style; hit with no pieces: the stroke is gone
};

/// The eraser's split mode. Segments whose outline meets `eraser` are cut out; every maximal run of
/// consecutive untouched segments (two points or more) survives as a new stroke.
SplitResult splitStroke(const Stroke &stroke, const QPainterPath &eraser);

/// The lasso and rectangle select. A stroke is selected when any of its points lies inside `region` or a segment
/// crosses it; a shape when its ink (shapePath) meets `region`; a text box or image when `region` holds the centre of
/// its rect or crosses its edge (a region wholly inside the rect selects what is drawn there, not the box).
/// Group and unknown items are never selected: nothing can move them. Returns the `t`s in document order.
std::vector<qint64> selectItems(const InkDocument &doc, const QPainterPath &region);

/// `item` mapped through `transform`, as new payloads (the document's snapshots share the old ones): a stroke's points,
/// a shape's points, a text box's or image's rect (the box of the mapped rect; a text box's text is not scaled, it reflows). A stroke's or shape's width scales by the
/// mean of the transform's x and y scale; t, layer and raw stay. A group or unknown item comes back unchanged.
Item transformItem(const Item &item, const QTransform &transform);

/// A shape's ink as one filled outline (the stroke of its centreline at `style.width`, round caps and joins): Line and
/// Arrow a segment (an Arrow adds an open head of kArrowHeadLengthInWidths widths at the end), Rect and Ellipse the
/// outline of the box of their two corners (never filled), Polyline an open and Polygon a closed run of the points.
/// Empty for a shape that has no ink: too few points, a non-finite point or no width. Paint it and hit-test with it.
QPainterPath shapePath(const Shape &shape);

/// The clipboard format (MIME kClipboardMime): `{"format":"notolith-ink-clip","items":[...]}`, each item written
/// exactly as in a .ink file, so style and unknown keys survive a copy and paste.
inline constexpr const char *kClipboardMime = "application/x-notolith-ink+json";
/// The most a clipboard payload may hold (another program's data too; paste() parses it on the UI thread, about 12 ms
/// a MiB). A heavy note is about 2 MB, so 16 MiB is a selection of the whole of several of them.
inline constexpr qsizetype kMaxClipboardBytes = qsizetype(16) << 20;
QByteArray itemsToClipboardJSON(const std::vector<const Item *> &items);
/// The items in `bytes` (their `t` is the source document's: the caller gives them new ones); `*ok` false, and nothing
/// returned, for anything that is not this format or holds an item the .ink reader would refuse.
std::vector<Item> itemsFromClipboardJSON(const QByteArray &bytes, bool *ok);

} // namespace noteAppC::ink
