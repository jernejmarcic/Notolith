#pragma once

#include <memory>
#include <vector>

#include <QColor>
#include <QJsonObject>
#include <QPointF>
#include <QRectF>
#include <QSizeF>
#include <QString>

/// The ink model (spec section 5 and 7). Units are CSS px at 96 dpi, as in
/// Rnote; A4 is 793.701 x 1122.52. Strokes are immutable once finished and
/// shared, so a history snapshot is a vector of pointers.
namespace noteAppC::ink {

enum class PressureCurve { Const, Linear, Sqrt, Cbrt, Pow2, Pow3 };
QString pressureCurveName(PressureCurve curve);
PressureCurve pressureCurveFromName(const QString &name, bool *ok = nullptr);
/// `width * curve(pressure)`, pressure clamped to 0..1.
double applyPressureCurve(PressureCurve curve, double width, double pressure);

/// Paint order is ascending (layer, t): document < image < highlighter < user.
enum class Layer { Document = 0, Image = 1, Highlighter = 2, User = 3 };
QString layerName(Layer layer);
Layer layerFromName(const QString &name);

struct StrokeStyle {
    double width = 2.0;
    QColor color{0x1e, 0x1e, 0x1e};
    PressureCurve pressureCurve = PressureCurve::Linear;
    bool operator==(const StrokeStyle &) const = default;
};

struct StrokePoint {
    double x = 0;
    double y = 0;
    double pressure = 0.5;
};

struct Stroke {
    StrokeStyle style;
    std::vector<StrokePoint> points;
    /// The points' box grown by half the style width (the widest the ink can be).
    QRectF bounds() const;
    double widthAt(size_t index) const;
};

enum class ShapeKind { Line, Arrow, Rect, Ellipse, Polyline, Polygon };
QString shapeKindName(ShapeKind kind);
ShapeKind shapeKindFromName(const QString &name, bool *ok = nullptr);

/// An Arrow's open head reaches this many stroke widths back from its tip.
inline constexpr double kArrowHeadLengthInWidths = 3.0;

struct Shape {
    ShapeKind kind = ShapeKind::Line;
    StrokeStyle style;
    std::vector<QPointF> points; ///< line/arrow: 2; rect/ellipse: 2 corners; polyline/polygon: n (model types only: no tool draws the last two, .rnote files carry them)
    QRectF bounds() const;       ///< the points' box grown by half the width, and by the arrowhead for an Arrow
};

struct TextBox {
    QRectF rect;
    QString markdown;
};

struct ImageRef {
    QRectF rect;
    QString path; ///< vault-relative
};

struct Item {
    enum class Type { Stroke, Shape, TextBox, Image, Group, Unknown };
    Type type = Type::Unknown;
    qint64 t = 0;
    Layer layer = Layer::User;
    std::shared_ptr<const Stroke> stroke;
    std::shared_ptr<const Shape> shape;
    std::shared_ptr<const TextBox> textBox;
    std::shared_ptr<const ImageRef> image;
    /// The item as read from the file, every key. Saving starts from it and
    /// overwrites the keys the model owns, so unknown keys and unknown types
    /// pass through (spec section 5).
    QJsonObject raw;
    QRectF bounds() const;
};

enum class LayoutKind { Fixed, ContinuousVertical, SemiInfinite, Infinite };
QString layoutName(LayoutKind layout);
LayoutKind layoutFromName(const QString &name);

enum class Pattern { None, Lines, Grid, Dots, LinesMargin, GridMajor, MusicStaff, Calligraphy, Seyes, Cornell, HexGrid };
QString patternName(Pattern pattern);
Pattern patternFromName(const QString &name);

struct PageFormat {
    double width = 793.701;
    double height = 1122.52;
    double dpi = 96;
};

struct Background {
    QColor color = Qt::white;
    Pattern pattern = Pattern::Lines;
    QSizeF patternSize{32, 32};
    QColor patternColor{0xc0, 0xc8, 0xd0};
};

struct DocumentSettings {
    PageFormat format;
    Background background;
    LayoutKind layout = LayoutKind::ContinuousVertical;
    QRectF extent{0, 0, 793.701, 1122.52};
    QJsonObject extra; ///< document keys the model does not own (a studio sample's "glyph")
};

} // namespace noteAppC::ink
