// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej Marčič
// Ported from Rnote (crates/rnote-engine/src/document/background.rs, the gen_*_pattern functions),
// copyright the Rnote contributors, GPL-3.0-or-later, https://github.com/flxzt/rnote.
//
// Only the lines that meet the exposed rect are drawn (visibleLineRange, once per axis). A ruled
// line is a band of kLineWidth whose top edge sits on the cell edge, so its centre is half a width down.
#include "PagePatterns.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include <QPainter>
#include <QPen>

namespace noteAppC::ink {

namespace {

constexpr double kMaxLineIndex = std::numeric_limits<int>::max();

QRectF transposed(const QRectF &r) { return QRectF(r.y(), r.x(), r.height(), r.width()); }

struct Pen {
    QPainter &painter;
    QRectF page;    ///< the area the pattern covers (a clip: nothing is drawn outside it)
    QPointF anchor; ///< where line 0 sits: the page's top-left on a paged layout, the document origin on a sheet
    QRectF view; ///< page & exposed, padded by kLinePad: what a line needs to span
    QColor color;

    void set(const QColor &c, double width) const { painter.setPen(QPen(c, width, Qt::SolidLine, Qt::FlatCap, Qt::MiterJoin)); }
    /// Rows of a pattern whose content, anchored at k * period, reaches `above` up and `below` down from it.
    LineRange rows(const QRectF &exposed, double period, double above = 0, double below = 0) const
    {
        return visibleLineRange(exposed.adjusted(0, -below, 0, above), page.adjusted(0, -below, 0, above), anchor.y(), period);
    }
    /// The same along x: content reaches `before` left of and `after` right of the anchored position.
    LineRange columns(const QRectF &exposed, double period, double before = 0, double after = 0) const
    {
        return visibleLineRange(transposed(exposed).adjusted(0, -after, 0, before), transposed(page).adjusted(0, -after, 0, before), anchor.x(), period);
    }
    void horizontal(double y, double width) const
    {
        set(color, width);
        painter.drawLine(QPointF(view.left(), y), QPointF(view.right(), y));
    }
    void vertical(double x, double width) const
    {
        set(color, width);
        painter.drawLine(QPointF(x, view.top()), QPointF(x, view.bottom()));
    }
};

/// Ruled lines every `period`.
void ruledLines(const Pen &pen, const QRectF &exposed, double period)
{
    const LineRange rows = pen.rows(exposed, period, 0, kLineWidth);
    for (int k = rows.first; k <= rows.last; ++k)
        pen.horizontal(pen.anchor.y() + k * period + kLineWidth / 2, kLineWidth);
}

void grid(const Pen &pen, const QRectF &exposed, QSizeF size)
{
    const LineRange rows = pen.rows(exposed, size.height(), 0, kLineWidth);
    const LineRange columns = pen.columns(exposed, size.width(), 0, kLineWidth);
    for (int k = rows.first; k <= rows.last; ++k)
        pen.horizontal(pen.anchor.y() + k * size.height() + kLineWidth / 2, kLineWidth);
    for (int k = columns.first; k <= columns.last; ++k)
        pen.vertical(pen.anchor.x() + k * size.width() + kLineWidth / 2, kLineWidth);
}

void dots(const Pen &pen, const QRectF &exposed, QSizeF size)
{
    const LineRange rows = pen.rows(exposed, size.height(), 0, kDotsWidth);
    const LineRange columns = pen.columns(exposed, size.width(), 0, kDotsWidth);
    pen.painter.setPen(Qt::NoPen);
    pen.painter.setBrush(pen.color);
    for (int row = rows.first; row <= rows.last; ++row)
        for (int column = columns.first; column <= columns.last; ++column)
            pen.painter.drawRoundedRect(QRectF(pen.anchor.x() + column * size.width(), pen.anchor.y() + row * size.height(), kDotsWidth, kDotsWidth),
                                        kDotsWidth / 3, kDotsWidth / 3);
    pen.painter.setBrush(Qt::NoBrush);
}

/// The smallest gap between two parallel lines a pattern draws, in document px.
double finestPeriod(const Background &background)
{
    const double w = background.patternSize.width(), h = background.patternSize.height();
    switch (background.pattern) {
    case Pattern::Grid: case Pattern::Dots: return std::min(w, h);
    default: return h;
    }
}

} // namespace

LineRange visibleLineRange(const QRectF &exposed, const QRectF &pageRect, double period)
{
    return visibleLineRange(exposed, pageRect, pageRect.top(), period);
}

LineRange visibleLineRange(const QRectF &exposed, const QRectF &clip, double anchorY, double period)
{
    if (!(period > 0) || !std::isfinite(period))
        return {};
    // In doubles until the end: a hostile rect or a 1e-3 period must not overflow an int on the way.
    const double first = std::ceil((std::max(exposed.top() - kLinePad, clip.top()) - anchorY) / period);
    const double last = std::floor((std::min(exposed.bottom() + kLinePad, clip.bottom()) - anchorY) / period);
    if (!(first <= last))
        return {};
    return {int(std::clamp(first, -kMaxLineIndex, kMaxLineIndex)), int(std::clamp(last, -kMaxLineIndex, kMaxLineIndex))};
}

void paintPattern(QPainter &painter, const Background &background, const QRectF &pageRect, const QRectF &exposed, double zoom)
{
    paintPattern(painter, background, pageRect, pageRect.topLeft(), exposed, zoom);
}

void paintPattern(QPainter &painter, const Background &background, const QRectF &pageRect, const QPointF &anchor, const QRectF &exposed, double zoom)
{
    const QRectF clip = pageRect & exposed;
    if (background.pattern == Pattern::None || clip.isEmpty() || !(zoom > 0))
        return;
    const double period = finestPeriod(background);
    if (!std::isfinite(period) || period * zoom < kMinPeriodPx)
        return;
    const QSizeF size = background.patternSize;
    const Pen pen{painter, pageRect, anchor, (clip.adjusted(-kLinePad, -kLinePad, kLinePad, kLinePad)) & pageRect.adjusted(-kLinePad, -kLinePad, kLinePad, kLinePad),
                  background.patternColor};
    painter.save();
    painter.setClipRect(clip, Qt::IntersectClip);
    switch (background.pattern) {
    case Pattern::None: break;
    case Pattern::Lines: ruledLines(pen, exposed, size.height()); break;
    case Pattern::Grid: grid(pen, exposed, size); break;
    case Pattern::Dots: dots(pen, exposed, size); break;
    }
    painter.restore();
}

} // namespace noteAppC::ink
