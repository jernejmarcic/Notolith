// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej
#include "InkTypes.h"

#include <algorithm>
#include <array>
#include <cmath>

#include <QJsonArray>

namespace noteAppC::ink {

namespace {
template <typename Enum, size_t N>
QString nameOf(const std::array<std::pair<Enum, const char *>, N> &table, Enum value)
{
    for (const auto &[key, name] : table)
        if (key == value)
            return QLatin1String(name);
    return QString();
}
template <typename Enum, size_t N>
Enum fromName(const std::array<std::pair<Enum, const char *>, N> &table, const QString &name, Enum fallback, bool *ok)
{
    for (const auto &[key, text] : table) {
        if (name == QLatin1String(text)) {
            if (ok) *ok = true;
            return key;
        }
    }
    if (ok) *ok = false;
    return fallback;
}
constexpr std::array<std::pair<PressureCurve, const char *>, 6> kCurves{{
    {PressureCurve::Const, "const"}, {PressureCurve::Linear, "linear"}, {PressureCurve::Sqrt, "sqrt"},
    {PressureCurve::Cbrt, "cbrt"}, {PressureCurve::Pow2, "pow2"}, {PressureCurve::Pow3, "pow3"}}};
constexpr std::array<std::pair<Layer, const char *>, 4> kLayers{{
    {Layer::Document, "document"}, {Layer::Image, "image"}, {Layer::Highlighter, "highlighter"}, {Layer::User, "user"}}};
constexpr std::array<std::pair<ShapeKind, const char *>, 6> kShapes{{
    {ShapeKind::Line, "line"}, {ShapeKind::Arrow, "arrow"}, {ShapeKind::Rect, "rect"},
    {ShapeKind::Ellipse, "ellipse"}, {ShapeKind::Polyline, "polyline"}, {ShapeKind::Polygon, "polygon"}}};
constexpr std::array<std::pair<LayoutKind, const char *>, 4> kLayouts{{
    {LayoutKind::Fixed, "fixed"}, {LayoutKind::ContinuousVertical, "continuousVertical"},
    {LayoutKind::SemiInfinite, "semiInfinite"}, {LayoutKind::Infinite, "infinite"}}};
constexpr std::array<std::pair<Pattern, const char *>, 4> kPatterns{{
    {Pattern::None, "none"}, {Pattern::Lines, "lines"}, {Pattern::Grid, "grid"}, {Pattern::Dots, "dots"}}};
} // namespace

QString pressureCurveName(PressureCurve curve) { return nameOf(kCurves, curve); }
PressureCurve pressureCurveFromName(const QString &name, bool *ok) { return fromName(kCurves, name, PressureCurve::Linear, ok); }
QString layerName(Layer layer) { return nameOf(kLayers, layer); }
Layer layerFromName(const QString &name) { return fromName(kLayers, name, Layer::User, nullptr); }
QString shapeKindName(ShapeKind kind) { return nameOf(kShapes, kind); }
ShapeKind shapeKindFromName(const QString &name, bool *ok) { return fromName(kShapes, name, ShapeKind::Line, ok); }
QString layoutName(LayoutKind layout) { return nameOf(kLayouts, layout); }
LayoutKind layoutFromName(const QString &name) { return fromName(kLayouts, name, LayoutKind::ContinuousVertical, nullptr); }
QString patternName(Pattern pattern) { return nameOf(kPatterns, pattern); }
Pattern patternFromName(const QString &name) { return fromName(kPatterns, name, Pattern::Lines, nullptr); }

// Ported from Rnote (rnote-compose/src/style/mod.rs, PressureCurve::apply),
// copyright the Rnote contributors, GPL-3.0-or-later, https://github.com/flxzt/rnote.
double applyPressureCurve(PressureCurve curve, double width, double pressure)
{
    const double p = std::clamp(pressure, 0.0, 1.0);
    switch (curve) {
    case PressureCurve::Const: return width;
    case PressureCurve::Linear: return width * p;
    case PressureCurve::Sqrt: return width * std::sqrt(p);
    case PressureCurve::Cbrt: return width * std::cbrt(p);
    case PressureCurve::Pow2: return width * p * p;
    case PressureCurve::Pow3: return width * p * p * p;
    }
    return width * p;
}

double Stroke::widthAt(size_t index) const
{
    return index < points.size() ? applyPressureCurve(style.pressureCurve, style.width, points[index].pressure) : 0.0;
}

QRectF Stroke::bounds() const
{
    if (points.empty())
        return QRectF();
    double x0 = points[0].x, y0 = points[0].y, x1 = x0, y1 = y0;
    for (const StrokePoint &p : points) {
        x0 = std::min(x0, p.x); y0 = std::min(y0, p.y);
        x1 = std::max(x1, p.x); y1 = std::max(y1, p.y);
    }
    const double width = std::fabs(style.width); // as Shape::bounds: a pasted negative width must not invert the rect
    const double half = width * 0.5;
    return QRectF(x0 - half, y0 - half, (x1 - x0) + width, (y1 - y0) + width);
}

QRectF Shape::bounds() const
{
    if (points.empty())
        return QRectF();
    // min/max by hand: QRectF::operator| returns the other operand when one is null, and a
    // rectangle built from one point is null, so a union of points would keep only the last.
    double x0 = points[0].x(), y0 = points[0].y(), x1 = x0, y1 = y0;
    for (const QPointF &p : points) {
        x0 = std::min(x0, p.x()); y0 = std::min(y0, p.y());
        x1 = std::max(x1, p.x()); y1 = std::max(y1, p.y());
    }
    // An arrow's open head reaches kArrowHeadLengthInWidths widths back from the tip, sideways too:
    // pad that far so the spatial grid and every invalidation see the whole head.
    const double width = std::fabs(style.width); // as shapePath: a negative width counts as its absolute value
    const double pad = width * 0.5 + (kind == ShapeKind::Arrow ? width * kArrowHeadLengthInWidths : 0.0);
    return QRectF(QPointF(x0 - pad, y0 - pad), QPointF(x1 + pad, y1 + pad));
}

QRectF Item::bounds() const
{
    switch (type) {
    case Type::Stroke: return stroke ? stroke->bounds() : QRectF();
    case Type::Shape: return shape ? shape->bounds() : QRectF();
    case Type::TextBox: return textBox ? textBox->rect : QRectF();
    case Type::Image: return image ? image->rect : QRectF();
    case Type::Group:
    case Type::Unknown: {
        const QJsonArray rect = raw.value(QStringLiteral("rect")).toArray();
        if (rect.size() == 4)
            return QRectF(rect[0].toDouble(), rect[1].toDouble(), rect[2].toDouble(), rect[3].toDouble());
        return QRectF();
    }
    }
    return QRectF();
}

} // namespace noteAppC::ink
