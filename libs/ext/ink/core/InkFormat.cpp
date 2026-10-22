#include "InkFormat.h"

#include <algorithm>
#include <climits>
#include <cmath>

#include <zlib.h>

#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QSet>

namespace noteAppC::ink {

namespace {

const QSet<QString> kTopKeys = {QStringLiteral("format"), QStringLiteral("version"), QStringLiteral("document"),
                                QStringLiteral("items")};

/// Inflates one gzip member. The output is capped (a few KB of gzip can unpack to gigabytes), and
/// bytes after the member are an error: they would be dropped without a word otherwise.
bool inflateGzip(const QByteArray &in, QByteArray *out, qint64 maxBytes, QString *error)
{
    z_stream stream{};
    if (inflateInit2(&stream, 16 + MAX_WBITS) != Z_OK) {
        *error = QStringLiteral("zlib could not start");
        return false;
    }
    stream.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(in.constData()));
    stream.avail_in = uInt(in.size());
    char buffer[1 << 16];
    int rc = Z_OK;
    while (rc != Z_STREAM_END) {
        stream.next_out = reinterpret_cast<Bytef *>(buffer);
        stream.avail_out = sizeof buffer;
        rc = inflate(&stream, Z_NO_FLUSH);
        if (rc != Z_OK && rc != Z_STREAM_END) {
            inflateEnd(&stream);
            *error = rc == Z_BUF_ERROR ? QStringLiteral("the gzip stream ends early") : QStringLiteral("the gzip stream is damaged");
            return false;
        }
        const qint64 produced = qint64(sizeof buffer - stream.avail_out);
        if (out->size() + produced > maxBytes) {
            inflateEnd(&stream);
            *error = QStringLiteral("it is too large when unpacked");
            return false;
        }
        out->append(buffer, int(produced));
    }
    const bool leftover = stream.avail_in != 0;
    inflateEnd(&stream);
    if (leftover) {
        *error = QStringLiteral("there is data after the gzip stream");
        return false;
    }
    return true;
}

/// Qt writes a non-finite number as null, which reads back as 0 or the default: write 0 instead,
/// so a document built in code (a NaN from a bad calculation) never produces a null in the file.
QJsonValue number(double value)
{
    const double rounded = roundThree(value);
    return QJsonValue(std::isfinite(rounded) ? rounded : 0.0);
}

/// A key that is present must hold the type the format gives it. Reading a wrong-typed `items` as
/// absent would load an empty note that the next save writes over the real one.
bool wrongType(const QJsonObject &object, const char *key, QJsonValue::Type type)
{
    const QJsonValue value = object.value(QLatin1StringView(key));
    return !value.isUndefined() && value.type() != type;
}

/// Containers of the top level and of `document`; items check their own below.
bool documentShapeIsValid(const QJsonObject &top, QString *why)
{
    if (wrongType(top, "items", QJsonValue::Array) || wrongType(top, "document", QJsonValue::Object)) {
        *why = QStringLiteral("items must be a list and document an object");
        return false;
    }
    const QJsonObject document = top.value(QStringLiteral("document")).toObject();
    for (const char *key : {"format", "background", "extent"}) {
        if (wrongType(document, key, QJsonValue::Object)) {
            *why = QStringLiteral("document.%1 must be an object").arg(QLatin1StringView(key));
            return false;
        }
    }
    return true;
}

QJsonArray rectToJSON(const QRectF &rect)
{
    return {number(rect.x()), number(rect.y()), number(rect.width()), number(rect.height())};
}

QRectF rectFromJSON(const QJsonArray &array)
{
    return array.size() == 4 ? QRectF(array[0].toDouble(), array[1].toDouble(), array[2].toDouble(), array[3].toDouble()) : QRectF();
}

/// A rect that is there must be four numbers (an absent one is the empty rect).
bool rectIsValid(const QJsonObject &object)
{
    const QJsonValue rect = object.value(QStringLiteral("rect"));
    return rect.isUndefined() || (rect.isArray() && rect.toArray().size() == 4);
}

int minimumShapePoints(ShapeKind kind) { return kind == ShapeKind::Polygon ? 3 : 2; }

/// `object` is the style as read: its unknown keys are kept, the three the model owns are overwritten.
QJsonObject styleToJSON(const StrokeStyle &style, QJsonObject object)
{
    object.insert(QStringLiteral("width"), number(style.width));
    object.insert(QStringLiteral("color"), colorName(style.color));
    object.insert(QStringLiteral("pressureCurve"), pressureCurveName(style.pressureCurve));
    return object;
}

StrokeStyle styleFromJSON(const QJsonObject &object)
{
    StrokeStyle style;
    style.width = object.value(QStringLiteral("width")).toDouble(style.width);
    style.color = colorFromName(object.value(QStringLiteral("color")).toString(), style.color);
    style.pressureCurve = pressureCurveFromName(object.value(QStringLiteral("pressureCurve")).toString(QStringLiteral("linear")));
    return style;
}

} // namespace

bool itemFromJSON(const QJsonObject &object, Item *item, QString *error)
{
    item->raw = object;
    const double t = object.value(QStringLiteral("t")).toDouble(-1);
    if (!(t >= 0 && t < 9e15)) { // finite and well inside qint64: t + 1 is the next id
        *error = QStringLiteral("an item has no usable t");
        return false;
    }
    item->t = qint64(t);
    item->layer = layerFromName(object.value(QStringLiteral("layer")).toString(QStringLiteral("user")));
    const QString type = object.value(QStringLiteral("type")).toString();
    if (type == QLatin1String("stroke")) {
        if (wrongType(object, "style", QJsonValue::Object) || wrongType(object, "points", QJsonValue::Array)) {
            *error = QStringLiteral("stroke %1: style must be an object and points a list").arg(item->t);
            return false;
        }
        auto stroke = std::make_shared<Stroke>();
        stroke->style = styleFromJSON(object.value(QStringLiteral("style")).toObject());
        const QJsonArray points = object.value(QStringLiteral("points")).toArray();
        if (points.size() % 3 != 0) {
            *error = QStringLiteral("stroke %1: points are not x, y, pressure triples").arg(item->t);
            return false;
        }
        stroke->points.reserve(size_t(points.size() / 3));
        for (qsizetype i = 0; i + 2 < points.size(); i += 3)
            stroke->points.push_back({points[i].toDouble(), points[i + 1].toDouble(), points[i + 2].toDouble()});
        item->type = Item::Type::Stroke;
        item->stroke = std::move(stroke);
    } else if (type == QLatin1String("shape")) {
        auto shape = std::make_shared<Shape>();
        bool ok = false;
        shape->kind = shapeKindFromName(object.value(QStringLiteral("kind")).toString(), &ok);
        if (!ok) {
            item->type = Item::Type::Unknown; // a shape kind this version does not draw: kept, not drawn
            return true;
        }
        if (wrongType(object, "style", QJsonValue::Object) || wrongType(object, "points", QJsonValue::Array)) {
            *error = QStringLiteral("shape %1: style must be an object and points a list").arg(item->t);
            return false;
        }
        shape->style = styleFromJSON(object.value(QStringLiteral("style")).toObject());
        const QJsonArray points = object.value(QStringLiteral("points")).toArray();
        if (points.size() % 2 != 0 || points.size() / 2 < minimumShapePoints(shape->kind)) {
            *error = QStringLiteral("shape %1: too few points (or an odd coordinate) for a %2").arg(item->t).arg(shapeKindName(shape->kind));
            return false;
        }
        for (qsizetype i = 0; i + 1 < points.size(); i += 2)
            shape->points.emplace_back(points[i].toDouble(), points[i + 1].toDouble());
        item->type = Item::Type::Shape;
        item->shape = std::move(shape);
    } else if ((type == QLatin1String("textbox") || type == QLatin1String("image")) && !rectIsValid(object)) {
        *error = QStringLiteral("%1 %2: rect must be four numbers").arg(type).arg(item->t);
        return false;
    } else if (type == QLatin1String("textbox")) {
        item->type = Item::Type::TextBox;
        item->textBox = std::make_shared<TextBox>(TextBox{rectFromJSON(object.value(QStringLiteral("rect")).toArray()),
                                                          object.value(QStringLiteral("markdown")).toString()});
    } else if (type == QLatin1String("image")) {
        item->type = Item::Type::Image;
        item->image = std::make_shared<ImageRef>(ImageRef{rectFromJSON(object.value(QStringLiteral("rect")).toArray()),
                                                          object.value(QStringLiteral("path")).toString()});
    } else if (type == QLatin1String("group")) {
        item->type = Item::Type::Group;
    } else {
        item->type = Item::Type::Unknown;
    }
    return true;
}

QJsonObject itemToJSON(const Item &item)
{
    if (item.type == Item::Type::Unknown || item.type == Item::Type::Group)
        return item.raw; // read back verbatim: no key added, none changed (spec section 5, pass-through)
    QJsonObject object = item.raw; // unknown keys first; the model's keys overwrite
    object.insert(QStringLiteral("t"), double(item.t));
    object.insert(QStringLiteral("layer"), layerName(item.layer));
    switch (item.type) {
    case Item::Type::Stroke: {
        object.insert(QStringLiteral("type"), QStringLiteral("stroke"));
        object.insert(QStringLiteral("style"), styleToJSON(item.stroke->style, item.raw.value(QStringLiteral("style")).toObject()));
        QJsonArray points;
        for (const StrokePoint &p : item.stroke->points) {
            points.append(number(p.x));
            points.append(number(p.y));
            points.append(number(p.pressure));
        }
        object.insert(QStringLiteral("points"), points);
        break;
    }
    case Item::Type::Shape: {
        object.insert(QStringLiteral("type"), QStringLiteral("shape"));
        object.insert(QStringLiteral("kind"), shapeKindName(item.shape->kind));
        object.insert(QStringLiteral("style"), styleToJSON(item.shape->style, item.raw.value(QStringLiteral("style")).toObject()));
        QJsonArray points;
        for (const QPointF &p : item.shape->points) {
            points.append(number(p.x()));
            points.append(number(p.y()));
        }
        object.insert(QStringLiteral("points"), points);
        break;
    }
    case Item::Type::TextBox:
        object.insert(QStringLiteral("type"), QStringLiteral("textbox"));
        object.insert(QStringLiteral("rect"), rectToJSON(item.textBox->rect));
        object.insert(QStringLiteral("markdown"), item.textBox->markdown);
        break;
    case Item::Type::Image:
        object.insert(QStringLiteral("type"), QStringLiteral("image"));
        object.insert(QStringLiteral("rect"), rectToJSON(item.image->rect));
        object.insert(QStringLiteral("path"), item.image->path);
        break;
    case Item::Type::Group:
    case Item::Type::Unknown:
        break; // returned verbatim above
    }
    return object;
}

// Above 1e12 a double has no digits left after the point (and value * 1000 would overflow past
// 1.8e305): rounding is a no-op there, so the value goes through untouched.
double roundThree(double value)
{
    if (!std::isfinite(value) || std::fabs(value) > 1e12)
        return value;
    return std::round(value * 1000.0) / 1000.0;
}

// "#rrggbb", or "#rrggbbaa" when translucent (CSS order; QColor's own
// HexArgb form is "#aarrggbb", so both directions are spelled out here).
QString colorName(const QColor &color)
{
    if (color.alpha() == 255)
        return color.name(QColor::HexRgb);
    return color.name(QColor::HexRgb) + QString::number(color.alpha(), 16).rightJustified(2, QLatin1Char('0'));
}

QColor colorFromName(const QString &name, const QColor &fallback)
{
    if (name.size() == 9 && name.startsWith(QLatin1Char('#'))) {
        bool ok = false;
        const int alpha = name.mid(7).toInt(&ok, 16);
        QColor color = QColor::fromString(name.left(7));
        if (ok && color.isValid()) {
            color.setAlpha(alpha);
            return color;
        }
        return fallback;
    }
    return QColor::isValidColorName(name) ? QColor::fromString(name) : fallback;
}

// `extra` is the document object as read, so the nested objects' unknown keys come back too
// (spec section 5): each is seeded from it and the keys the model owns are written over.
QJsonObject settingsToJSON(const DocumentSettings &s)
{
    QJsonObject object = s.extra;
    QJsonObject format = s.extra.value(QStringLiteral("format")).toObject();
    format.insert(QStringLiteral("width"), number(s.format.width));
    format.insert(QStringLiteral("height"), number(s.format.height));
    format.insert(QStringLiteral("dpi"), number(s.format.dpi));
    object.insert(QStringLiteral("format"), format);
    const QJsonValue patternSize = s.background.patternSize.width() == s.background.patternSize.height()
        ? QJsonValue(number(s.background.patternSize.width()))
        : QJsonValue(QJsonArray{number(s.background.patternSize.width()), number(s.background.patternSize.height())});
    QJsonObject background = s.extra.value(QStringLiteral("background")).toObject();
    background.insert(QStringLiteral("color"), colorName(s.background.color));
    background.insert(QStringLiteral("pattern"), patternName(s.background.pattern));
    background.insert(QStringLiteral("patternSize"), patternSize);
    background.insert(QStringLiteral("patternColor"), colorName(s.background.patternColor));
    object.insert(QStringLiteral("background"), background);
    object.insert(QStringLiteral("layout"), layoutName(s.layout));
    QJsonObject extent = s.extra.value(QStringLiteral("extent")).toObject();
    extent.insert(QStringLiteral("x"), number(s.extent.x()));
    extent.insert(QStringLiteral("y"), number(s.extent.y()));
    extent.insert(QStringLiteral("width"), number(s.extent.width()));
    extent.insert(QStringLiteral("height"), number(s.extent.height()));
    object.insert(QStringLiteral("extent"), extent);
    return object;
}

DocumentSettings settingsFromJSON(const QJsonObject &object)
{
    DocumentSettings s;
    const QJsonObject format = object.value(QStringLiteral("format")).toObject();
    s.format.width = format.value(QStringLiteral("width")).toDouble(s.format.width);
    s.format.height = format.value(QStringLiteral("height")).toDouble(s.format.height);
    s.format.dpi = format.value(QStringLiteral("dpi")).toDouble(s.format.dpi);
    const QJsonObject background = object.value(QStringLiteral("background")).toObject();
    s.background.color = colorFromName(background.value(QStringLiteral("color")).toString(), s.background.color);
    s.background.pattern = patternFromName(background.value(QStringLiteral("pattern")).toString(QStringLiteral("lines")));
    const QJsonValue size = background.value(QStringLiteral("patternSize"));
    if (size.isArray() && size.toArray().size() == 2)
        s.background.patternSize = QSizeF(size.toArray()[0].toDouble(32), size.toArray()[1].toDouble(32));
    else if (size.isDouble())
        s.background.patternSize = QSizeF(size.toDouble(), size.toDouble());
    s.background.patternColor = colorFromName(background.value(QStringLiteral("patternColor")).toString(), s.background.patternColor);
    s.layout = layoutFromName(object.value(QStringLiteral("layout")).toString(QStringLiteral("continuousVertical")));
    const QJsonObject extent = object.value(QStringLiteral("extent")).toObject();
    if (!extent.isEmpty())
        s.extent = QRectF(extent.value(QStringLiteral("x")).toDouble(), extent.value(QStringLiteral("y")).toDouble(),
                          extent.value(QStringLiteral("width")).toDouble(s.format.width),
                          extent.value(QStringLiteral("height")).toDouble(s.format.height));
    else
        s.extent = QRectF(0, 0, s.format.width, s.format.height);
    s.extra = object;
    return s;
}

InkReadResult readInk(const QByteArray &input, InkDocument &out, const QString &nameForErrors)
{
    return readInk(input, out, nameForErrors, kMaxInflatedBytes);
}

InkReadResult readInk(const QByteArray &input, InkDocument &out, const QString &nameForErrors, qint64 maxInflatedBytes)
{
    InkReadResult result;
    const QString name = nameForErrors.isEmpty() ? QStringLiteral("the ink file") : nameForErrors;
    if (input.size() > maxInflatedBytes) { // plain JSON too: the parse peak is about ten times the file
        result.error = QStringLiteral("%1 is too large to open").arg(name);
        return result;
    }
    QByteArray bytes = input;
    if (bytes.size() >= 2 && quint8(bytes[0]) == 0x1f && quint8(bytes[1]) == 0x8b) {
        QByteArray inflated;
        QString why;
        if (!inflateGzip(bytes, &inflated, maxInflatedBytes, &why)) {
            result.error = QStringLiteral("%1 could not be read: %2").arg(name, why);
            return result;
        }
        bytes = inflated;
    }
    QJsonParseError parseError;
    const QJsonDocument json = QJsonDocument::fromJson(bytes, &parseError);
    if (!json.isObject()) {
        result.error = QStringLiteral("%1 could not be read: %2 at byte %3").arg(name, parseError.errorString()).arg(parseError.offset);
        return result;
    }
    const QJsonObject top = json.object();
    if (top.value(QStringLiteral("format")).toString() != QLatin1String("notolith-ink")) {
        result.error = QStringLiteral("%1 is not a Notolith ink file").arg(name);
        return result;
    }
    // Exactly 1 is the one version this code writes. Any other number from 1 up (2, 1.5, 3e9) is a
    // newer file and opens read-only, reported as the next whole version (capped); anything that is
    // not a number, or is below 1, is refused.
    const QJsonValue versionValue = top.value(QStringLiteral("version"));
    if (!versionValue.isDouble() || !(versionValue.toDouble() >= 1)) {
        result.error = QStringLiteral("%1 has no valid version").arg(name);
        return result;
    }
    result.version = int(std::min(std::ceil(versionValue.toDouble()), double(INT_MAX)));
    result.readOnly = versionValue.toDouble() != 1;
    QString shapeProblem;
    if (!documentShapeIsValid(top, &shapeProblem)) {
        result.error = QStringLiteral("%1 could not be read: %2").arg(name, shapeProblem);
        return result;
    }
    const DocumentSettings settings = settingsFromJSON(top.value(QStringLiteral("document")).toObject());
    if (!settings.isUsable()) { // sizes the document divides by or loops over (pinned by NonFiniteAndNonPositiveNumbersAreRefused)
        result.error = QStringLiteral("%1 could not be read: its page, extent or pattern size is not a usable size").arg(name);
        return result;
    }
    std::vector<Item> items;
    for (const QJsonValue &value : top.value(QStringLiteral("items")).toArray()) {
        Item item;
        QString why;
        if (!value.isObject()) {
            result.error = QStringLiteral("%1 could not be read: an item is not an object").arg(name);
            return result;
        }
        if (!itemFromJSON(value.toObject(), &item, &why)) {
            result.error = QStringLiteral("%1 could not be read: %2").arg(name, why);
            return result;
        }
        items.push_back(std::move(item));
    }
    std::vector<qint64> ids;
    ids.reserve(items.size());
    for (const Item &item : items)
        ids.push_back(item.t);
    std::sort(ids.begin(), ids.end());
    if (std::adjacent_find(ids.begin(), ids.end()) != ids.end()) {
        result.error = QStringLiteral("%1 could not be read: two items share one t").arg(name);
        return result;
    }
    QJsonObject extraTop;
    for (auto it = top.begin(); it != top.end(); ++it)
        if (!kTopKeys.contains(it.key()))
            extraTop.insert(it.key(), it.value());
    out.reset(settings, std::move(items), extraTop);
    result.ok = true;
    return result;
}

QByteArray writeInk(const InkDocument &document)
{
    QJsonObject top = document.extraTopLevel(); // unknown top-level keys first; the format's own keys overwrite
    top.insert(QStringLiteral("format"), QStringLiteral("notolith-ink"));
    top.insert(QStringLiteral("version"), 1);
    top.insert(QStringLiteral("document"), settingsToJSON(document.settings()));
    QJsonArray items;
    for (const Item &item : document.items())
        items.append(itemToJSON(item));
    top.insert(QStringLiteral("items"), items);
    // R4: no hand-rolled emitter. QJsonDocument writes keys sorted and each number in its shortest form.
    return QJsonDocument(top).toJson(QJsonDocument::Compact);
}

bool loadInkFile(const QString &path, InkDocument &out, InkReadResult *result)
{
    InkReadResult local;
    InkReadResult &r = result ? *result : local;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        r = InkReadResult{};
        r.error = QStringLiteral("%1 could not be opened: %2").arg(QFileInfo(path).fileName(), file.errorString());
        return false;
    }
    if (file.size() > kMaxInflatedBytes) { // before readAll(): a multi-GB file must not be loaded to be refused
        r = InkReadResult{};
        r.error = QStringLiteral("%1 is too large to open").arg(QFileInfo(path).fileName());
        return false;
    }
    r = readInk(file.readAll(), out, QFileInfo(path).fileName());
    return r.ok;
}

bool saveInkFile(const QString &path, const InkDocument &document, QString *error)
{
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error) *error = file.errorString();
        return false;
    }
    const QByteArray bytes = writeInk(document);
    if (file.write(bytes) != bytes.size() || !file.commit()) {
        if (error) *error = file.errorString();
        return false;
    }
    return true;
}

} // namespace noteAppC::ink
