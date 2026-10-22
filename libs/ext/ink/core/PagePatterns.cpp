// Ported from Rnote (crates/rnote-engine/src/document/background.rs, the gen_*_pattern
// functions and Background::page_overlay_lines), copyright the Rnote contributors,
// GPL-3.0-or-later, https://github.com/flxzt/rnote. The page styles beyond the stock
// six are from Jernej's fork, branch jernej/features.
//
// Rnote tiles an SVG pattern; this draws the same lines directly, and only the lines
// that meet the exposed rect (visibleLineRange, once per axis). Where the fork's
// geometry differs from a plain "line at k * period" it wins: a ruled line is a band of
// kLineWidth whose top edge sits on the cell edge, so its centre is half a width down.
#include "PagePatterns.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include <QPainter>
#include <QPen>
#include <QPolygonF>

namespace noteAppC::ink {

namespace {

constexpr double kSqrtThree = 1.7320508075688772;
constexpr double kCalligraphyBaselineFraction = 0.6;
constexpr double kCalligraphyDashPx = 4; ///< SVG "4 4": 4 px on, 4 px off (Qt counts dashes in pen widths)
constexpr double kCornellSummaryFraction = 0.2;
constexpr double kMaxLineIndex = std::numeric_limits<int>::max();

QRectF transposed(const QRectF &r) { return QRectF(r.y(), r.x(), r.height(), r.width()); }

/// Rows of a pattern whose content, anchored at k * period, reaches `above` up and `below` down from it.
LineRange rowsOf(const QRectF &exposed, const QRectF &page, double period, double above = 0, double below = 0)
{
    return visibleLineRange(exposed.adjusted(0, -below, 0, above), page, period);
}

/// The same along x, `before` and `after` the anchored position.
LineRange columnsOf(const QRectF &exposed, const QRectF &page, double period, double before = 0, double after = 0)
{
    return visibleLineRange(transposed(exposed).adjusted(0, -after, 0, before), transposed(page), period);
}

struct Pen {
    QPainter &painter;
    QRectF page;
    QRectF view; ///< page & exposed, padded by kLinePad: what a line needs to span
    QColor color;
    QColor marginColor{kMarginColor};

    void set(const QColor &c, double width, double opacity = 1.0, Qt::PenCapStyle cap = Qt::FlatCap) const
    {
        QColor withOpacity = c;
        withOpacity.setAlphaF(c.alphaF() * opacity);
        QPen pen(withOpacity, width, Qt::SolidLine, cap, Qt::MiterJoin);
        painter.setPen(pen);
    }
    void horizontal(double y, double width, double opacity = 1.0) const
    {
        set(color, width, opacity);
        painter.drawLine(QPointF(view.left(), y), QPointF(view.right(), y));
    }
    void vertical(double x, double width, double opacity = 1.0) const
    {
        set(color, width, opacity);
        painter.drawLine(QPointF(x, view.top()), QPointF(x, view.bottom()));
    }
};

/// Ruled lines every `period`: Lines, LinesMargin and Cornell.
void ruledLines(const Pen &pen, const QRectF &exposed, double period)
{
    const LineRange rows = rowsOf(exposed, pen.page, period, 0, kLineWidth);
    for (int k = rows.first; k <= rows.last; ++k)
        pen.horizontal(pen.page.top() + k * period + kLineWidth / 2, kLineWidth);
}

void grid(const Pen &pen, const QRectF &exposed, QSizeF size)
{
    const LineRange rows = rowsOf(exposed, pen.page, size.height(), 0, kLineWidth);
    const LineRange columns = columnsOf(exposed, pen.page, size.width(), 0, kLineWidth);
    for (int k = rows.first; k <= rows.last; ++k)
        pen.horizontal(pen.page.top() + k * size.height() + kLineWidth / 2, kLineWidth);
    for (int k = columns.first; k <= columns.last; ++k)
        pen.vertical(pen.page.left() + k * size.width() + kLineWidth / 2, kLineWidth);
}

void dots(const Pen &pen, const QRectF &exposed, QSizeF size)
{
    const LineRange rows = rowsOf(exposed, pen.page, size.height(), 0, kDotsWidth);
    const LineRange columns = columnsOf(exposed, pen.page, size.width(), 0, kDotsWidth);
    pen.painter.setPen(Qt::NoPen);
    pen.painter.setBrush(pen.color);
    for (int row = rows.first; row <= rows.last; ++row)
        for (int column = columns.first; column <= columns.last; ++column)
            pen.painter.drawRoundedRect(QRectF(pen.page.left() + column * size.width(), pen.page.top() + row * size.height(), kDotsWidth, kDotsWidth),
                                        kDotsWidth / 3, kDotsWidth / 3);
    pen.painter.setBrush(Qt::NoBrush);
}

/// A vertical line at `x` from `top` to `bottom` (document px), trimmed to the exposed rect, with square caps.
void overlayVertical(const Pen &pen, const QColor &color, double x, double top, double bottom)
{
    const double from = std::max(top, pen.view.top()), to = std::min(bottom, pen.view.bottom());
    if (from > to || x < pen.view.left() || x > pen.view.right())
        return; // outside what is exposed: a reversed line would draw where the line is not
    pen.set(color, kOverlayLineWidth, 1.0, Qt::SquareCap);
    pen.painter.drawLine(QPointF(x, from), QPointF(x, to));
}

void gridMajor(const Pen &pen, const QRectF &exposed, QSizeF size)
{
    // Every 5th line is a major one: twice as wide, full opacity, its band starting on the cell edge.
    const auto style = [](int k, double &width, double &offset, double &opacity) {
        const bool major = k % kMajorInterval == 0;
        width = major ? 2 * kLineWidth : kLineWidth;
        offset = major ? kLineWidth : kLineWidth / 2;
        opacity = major ? 1.0 : kMinorOpacity;
    };
    const LineRange rows = rowsOf(exposed, pen.page, size.height(), 0, 2 * kLineWidth);
    const LineRange columns = columnsOf(exposed, pen.page, size.width(), 0, 2 * kLineWidth);
    double width, offset, opacity;
    for (int k = rows.first; k <= rows.last; ++k) {
        style(k, width, offset, opacity);
        pen.horizontal(pen.page.top() + k * size.height() + offset, width, opacity);
    }
    for (int k = columns.first; k <= columns.last; ++k) {
        style(k, width, offset, opacity);
        pen.vertical(pen.page.left() + k * size.width() + offset, width, opacity);
    }
}

void musicStaff(const Pen &pen, const QRectF &exposed, double gap)
{
    constexpr double staffLineWidth = 2 * kLineWidth;
    const double period = gap * kStaffPeriodFactor;
    const LineRange periods = rowsOf(exposed, pen.page, period, 0, 4 * gap + staffLineWidth);
    for (int k = periods.first; k <= periods.last; ++k)
        for (int line = 0; line < 5; ++line)
            pen.horizontal(pen.page.top() + k * period + line * gap + staffLineWidth / 2, staffLineWidth);
}

void calligraphy(const Pen &pen, const QRectF &exposed, double period)
{
    const LineRange periods = rowsOf(exposed, pen.page, period, 0, 0.8 * period + 2 * kLineWidth);
    // The dashes start at the page's left edge; begin on a dash boundary so they do not shift with the exposed rect.
    const double dash = 2 * kCalligraphyDashPx;
    const double dashedFrom = pen.page.left() + std::floor((pen.view.left() - pen.page.left()) / dash) * dash;
    for (int k = periods.first; k <= periods.last; ++k) {
        const double top = pen.page.top() + k * period;
        pen.horizontal(top + kLineWidth / 2, kLineWidth);
        pen.horizontal(top + kCalligraphyBaselineFraction * period + kLineWidth, 2 * kLineWidth);
        // Drawn dash by dash in one drawLines: a Qt dash pattern counts in pen widths, and under 1 px wide it does not scale as SVG's does.
        pen.set(pen.color, kLineWidth);
        for (const double y : {0.3 * period + kLineWidth / 2, 0.8 * period + kLineWidth / 2}) {
            QList<QLineF> dashes;
            for (double x = dashedFrom; x < pen.view.right(); x += dash)
                dashes << QLineF(x, top + y, x + kCalligraphyDashPx, top + y);
            pen.painter.drawLines(dashes);
        }
    }
}

/// French ruled paper: squares of patternSize.height, three lighter lines between the horizontals, a margin at patternSize.width.
void seyes(const Pen &pen, const QRectF &exposed, QSizeF size)
{
    const double square = size.height();
    const LineRange rows = rowsOf(exposed, pen.page, square, 0, 0.75 * square + kLineWidth);
    const LineRange columns = columnsOf(exposed, pen.page, square, 0, kLineWidth);
    for (int k = rows.first; k <= rows.last; ++k) {
        const double top = pen.page.top() + k * square;
        pen.horizontal(top + kLineWidth / 2, kLineWidth);
        for (int i = 1; i < 4; ++i)
            pen.horizontal(top + square * i / 4 + kLineWidth / 2, kLineWidth, kMinorOpacity);
    }
    for (int k = columns.first; k <= columns.last; ++k)
        pen.vertical(pen.page.left() + k * square + kLineWidth / 2, kLineWidth);
    overlayVertical(pen, pen.marginColor, pen.page.left() + size.width(), pen.page.top(), pen.page.bottom());
}

/// Pointy-top hexagons of circumradius r = patternSize.height, drawn as Rnote's tile does: per row the zigzag
/// along the hexagons' lower edges and the vertical sides, so no edge is stroked twice (a translucent colour
/// would double there). Rows sit 1.5 r apart; every second row is half a cell over.
void hexGrid(const Pen &pen, const QRectF &exposed, double r)
{
    const double width = kSqrtThree * r, half = width / 2;
    const LineRange rows = rowsOf(exposed, pen.page, 1.5 * r, 0.5 * r, r);
    const LineRange columns = columnsOf(exposed, pen.page, width, width, width); // a cell reaches half a width either side
    if (columns.isEmpty())
        return;
    pen.set(pen.color, kLineWidth);
    for (int row = rows.first; row <= rows.last; ++row) {
        const double centreY = pen.page.top() + 1.5 * r * row;
        const double offset = pen.page.left() + (row % 2 ? half : 0.0);
        QPolygonF zigzag;
        QList<QLineF> sides;
        for (int i = columns.first; i <= columns.last; ++i) {
            const double centreX = offset + i * width;
            zigzag << QPointF(centreX - half, centreY + 0.5 * r) << QPointF(centreX, centreY + r);
            sides << QLineF(centreX - half, centreY - 0.5 * r, centreX - half, centreY + 0.5 * r);
        }
        const double lastX = offset + columns.last * width + half;
        zigzag << QPointF(lastX, centreY + 0.5 * r);
        sides << QLineF(lastX, centreY - 0.5 * r, lastX, centreY + 0.5 * r);
        pen.painter.drawPolyline(zigzag);
        pen.painter.drawLines(sides);
    }
}

/// The smallest gap between two parallel lines a pattern draws, in document px.
double finestPeriod(const Background &background)
{
    const double w = background.patternSize.width(), h = background.patternSize.height();
    switch (background.pattern) {
    case Pattern::Grid: case Pattern::Dots: case Pattern::GridMajor: return std::min(w, h);
    case Pattern::Seyes: return h / 4;
    case Pattern::Calligraphy: return 0.2 * h;
    case Pattern::HexGrid: return 0.5 * h;
    default: return h;
    }
}

} // namespace

LineRange visibleLineRange(const QRectF &exposed, const QRectF &pageRect, double period)
{
    if (!(period > 0) || !std::isfinite(period))
        return {};
    // In doubles until the end: a hostile rect or a 1e-3 period must not overflow an int on the way.
    const double first = std::max(0.0, std::ceil((exposed.top() - pageRect.top() - kLinePad) / period));
    const double last = std::min({std::floor(pageRect.height() / period), std::floor((exposed.bottom() - pageRect.top() + kLinePad) / period), kMaxLineIndex});
    if (!(first <= last))
        return {};
    return {int(first), int(last)};
}

void paintPattern(QPainter &painter, const Background &background, const QRectF &pageRect, const QRectF &exposed, double zoom)
{
    const QRectF clip = pageRect & exposed;
    if (background.pattern == Pattern::None || clip.isEmpty() || !(zoom > 0))
        return;
    const double period = finestPeriod(background);
    if (!std::isfinite(period) || period * zoom < kMinPeriodPx)
        return;
    const QSizeF size = background.patternSize;
    const Pen pen{painter, pageRect, (clip.adjusted(-kLinePad, -kLinePad, kLinePad, kLinePad)) & pageRect.adjusted(-kLinePad, -kLinePad, kLinePad, kLinePad),
                  background.patternColor};
    painter.save();
    painter.setClipRect(clip, Qt::IntersectClip);
    switch (background.pattern) {
    case Pattern::None: break;
    case Pattern::Lines: ruledLines(pen, exposed, size.height()); break;
    case Pattern::Grid: grid(pen, exposed, size); break;
    case Pattern::Dots: dots(pen, exposed, size); break;
    case Pattern::LinesMargin:
        ruledLines(pen, exposed, size.height());
        overlayVertical(pen, pen.marginColor, pageRect.left() + size.width(), pageRect.top(), pageRect.bottom());
        break;
    case Pattern::GridMajor: gridMajor(pen, exposed, size); break;
    case Pattern::MusicStaff: musicStaff(pen, exposed, size.height()); break;
    case Pattern::Calligraphy: calligraphy(pen, exposed, size.height()); break;
    case Pattern::Seyes: seyes(pen, exposed, size); break;
    case Pattern::Cornell: {
        ruledLines(pen, exposed, size.height());
        const double summaryY = pageRect.bottom() - kCornellSummaryFraction * pageRect.height();
        overlayVertical(pen, pen.color, pageRect.left() + size.width(), pageRect.top(), summaryY);
        if (summaryY >= pen.view.top() && summaryY <= pen.view.bottom()) {
            pen.set(pen.color, kOverlayLineWidth, 1.0, Qt::SquareCap);
            painter.drawLine(QPointF(pen.view.left(), summaryY), QPointF(pen.view.right(), summaryY));
        }
        break;
    }
    case Pattern::HexGrid: hexGrid(pen, exposed, size.height()); break;
    }
    painter.restore();
}

} // namespace noteAppC::ink
