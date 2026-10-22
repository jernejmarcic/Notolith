#pragma once

// Ported from Rnote (crates/rnote-engine/src/document/background.rs, the gen_*_pattern
// functions and Background::page_overlay_lines), copyright the Rnote contributors,
// GPL-3.0-or-later, https://github.com/flxzt/rnote. The page styles beyond the stock
// six are from Jernej's fork, branch jernej/features.
#include <QRectF>

#include "InkTypes.h"

class QPainter;

namespace noteAppC::ink {

/// Line widths and ratios from Rnote's Background, in document px (zoom scales them, as in Rnote).
inline constexpr double kLineWidth = 0.5;
inline constexpr double kDotsWidth = 1.5;
inline constexpr double kOverlayLineWidth = 1.0; ///< the margin and Cornell lines
inline constexpr int kMajorInterval = 5;         ///< every 5th line of GridMajor is a major line
inline constexpr double kMinorOpacity = 0.5;
inline constexpr double kStaffPeriodFactor = 10; ///< a staff period is this many staff gaps
/// A line is wanted when it, padded by this, meets the exposed rect: covers half a line width and the AA fringe.
inline constexpr double kLinePad = 1.0;
/// The margin line of LinesMargin and Seyes (Rnote's margin_color default); the model has no margin colour yet.
inline constexpr QRgb kMarginColor = 0xffdb4040;
/// A pattern whose period is under this many device px is not drawn: it is a grey wash, and a hostile
/// pattern size (the reader accepts 1e-3) would issue millions of draw calls per paint.
inline constexpr double kMinPeriodPx = 1.5;

/// Paints the pattern of `background` over `pageRect` (document px) into a painter already scaled to the zoom, clipped to `exposed`.
void paintPattern(QPainter &painter, const Background &background, const QRectF &pageRect, const QRectF &exposed, double zoom);
/// The same with the pattern's origin apart from the clip: lines sit at anchor + k * period, and nothing is drawn outside
/// `clip`. A paged layout anchors at the page's top-left (each page restarts the rhythm); a sheet layout at the document
/// origin, so growing the sheet up or left never moves the pattern under the ink (Rnote tiles from the origin too).
void paintPattern(QPainter &painter, const Background &background, const QRectF &clip, const QPointF &anchor, const QRectF &exposed, double zoom);

/// The indices of the lines a pattern must draw: lines sit at pageRect.top() + k * period, k >= 0, inside the page,
/// and a line is wanted when it, padded by a line's half width (kLinePad = 1.0), meets the exposed rect. Empty
/// (last < first) when none is. Pure: this is what keeps a 10 000 px page from looping over its whole height.
struct LineRange {
    int first = 0;
    int last = -1;
    bool isEmpty() const { return last < first; }
    bool operator==(const LineRange &) const = default;
};
LineRange visibleLineRange(const QRectF &exposed, const QRectF &pageRect, double period);
/// The general form: lines at anchorY + k * period (k may be negative), kept to those inside `clip`. With
/// anchorY == clip.top() it is the function above.
LineRange visibleLineRange(const QRectF &exposed, const QRectF &clip, double anchorY, double period);

} // namespace noteAppC::ink
