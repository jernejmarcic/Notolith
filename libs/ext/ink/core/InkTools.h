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

/// The lasso and rectangle select. A stroke is selected when any of its points lies inside `region` (Rnote); a shape,
/// text box or image when the centre of its bounds does (a real hit test per kind comes with Tasks 14-15, carry N5).
/// Group and unknown items are never selected: nothing can move them. Returns the `t`s in document order.
std::vector<qint64> selectItems(const InkDocument &doc, const QPainterPath &region);

/// `item` mapped through `transform`, as new payloads (the document's snapshots share the old ones): a stroke's points,
/// a shape's points, a text box's or image's rect (the box of the mapped rect). A stroke's or shape's width scales by the
/// mean of the transform's x and y scale (Rnote); t, layer and raw stay. A group or unknown item comes back unchanged.
Item transformItem(const Item &item, const QTransform &transform);

/// The clipboard format (MIME kClipboardMime): `{"format":"notolith-ink-clip","items":[...]}`, each item written
/// exactly as in a .ink file, so style and unknown keys survive a copy and paste.
inline constexpr const char *kClipboardMime = "application/x-notolith-ink+json";
QByteArray itemsToClipboardJSON(const std::vector<const Item *> &items);
/// The items in `bytes` (their `t` is the source document's: the caller gives them new ones); `*ok` false, and nothing
/// returned, for anything that is not this format or holds an item the .ink reader would refuse.
std::vector<Item> itemsFromClipboardJSON(const QByteArray &bytes, bool *ok);

} // namespace noteAppC::ink
