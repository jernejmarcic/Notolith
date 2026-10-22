// The ink core (docs/superpowers/specs/2026-10-21-ink-core-design.md section 7
// and 12): model and history, the .ink format, the outline composer, input,
// the canvas. Pure functions and widgets on the offscreen platform.
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <limits>

#include <QApplication>
#include <QDir>
#include <QImage>
#include <QPainter>
#include <QPainterPath>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include <zlib.h>

#include "InkDocument.h"
#include "InkFormat.h"
#include "InkInput.h"
#include "InkTypes.h"
#include "SpatialGrid.h"
#include "StrokeOutline.h"

using namespace noteAppC::ink;

namespace {

std::shared_ptr<const Stroke> line(double x0, double y0, double x1, double y1, double width = 2.0)
{
    auto stroke = std::make_shared<Stroke>();
    stroke->style.width = width;
    stroke->points = {{x0, y0, 0.5}, {x1, y1, 0.5}};
    return stroke;
}

} // namespace

TEST(InkTypesTest, PressureCurvesMatchRnote)
{
    EXPECT_DOUBLE_EQ(applyPressureCurve(PressureCurve::Const, 4.0, 0.25), 4.0);
    EXPECT_DOUBLE_EQ(applyPressureCurve(PressureCurve::Linear, 4.0, 0.25), 1.0);
    EXPECT_DOUBLE_EQ(applyPressureCurve(PressureCurve::Sqrt, 4.0, 0.25), 2.0);
    EXPECT_NEAR(applyPressureCurve(PressureCurve::Cbrt, 8.0, 0.125), 4.0, 1e-9);
    EXPECT_DOUBLE_EQ(applyPressureCurve(PressureCurve::Pow2, 4.0, 0.5), 1.0);
    EXPECT_DOUBLE_EQ(applyPressureCurve(PressureCurve::Pow3, 8.0, 0.5), 1.0);
    EXPECT_DOUBLE_EQ(applyPressureCurve(PressureCurve::Linear, 4.0, 7.0), 4.0) << "pressure above 1 clamps";
    EXPECT_DOUBLE_EQ(applyPressureCurve(PressureCurve::Linear, 4.0, -1.0), 0.0) << "pressure below 0 clamps";
    for (PressureCurve curve : {PressureCurve::Const, PressureCurve::Linear, PressureCurve::Sqrt, PressureCurve::Cbrt,
                                PressureCurve::Pow2, PressureCurve::Pow3})
        EXPECT_EQ(pressureCurveFromName(pressureCurveName(curve)), curve);
    bool ok = true;
    pressureCurveFromName(QStringLiteral("wobbly"), &ok);
    EXPECT_FALSE(ok);
}

TEST(InkTypesTest, StrokeBoundsIncludeHalfTheWidth)
{
    const auto stroke = line(10, 20, 30, 20, 4.0);
    EXPECT_EQ(stroke->bounds(), QRectF(8, 18, 24, 4));
    Stroke tap;
    tap.style.width = 6.0;
    tap.points = {{5, 5, 1.0}};
    EXPECT_EQ(tap.bounds(), QRectF(2, 2, 6, 6));
    EXPECT_TRUE(Stroke().bounds().isNull());
}

TEST(InkTypesTest, ShapeBoundsCoverEveryPointAndAnArrowsHead)
{
    Shape segment;
    segment.kind = ShapeKind::Line;
    segment.style.width = 4.0;
    segment.points = {QPointF(0, 0), QPointF(100, 50)};
    EXPECT_EQ(segment.bounds(), QRectF(-2, -2, 104, 54)) << "every point, grown by half the width";
    Shape arrow = segment;
    arrow.kind = ShapeKind::Arrow;
    EXPECT_EQ(arrow.bounds(), QRectF(-14, -14, 128, 78)) << "the head reaches 3 widths back from the tip, sideways too";
    EXPECT_TRUE(Shape().bounds().isNull());
}

TEST(SpatialGridTest, QueriesReturnEachItemOnceAcrossCells)
{
    SpatialGrid grid(100.0);
    grid.insert(1, QRectF(50, 50, 200, 10)); // spans three cells
    grid.insert(2, QRectF(900, 900, 5, 5));
    std::vector<qint64> hits = grid.query(QRectF(0, 0, 300, 100));
    EXPECT_EQ(hits, (std::vector<qint64>{1}));
    EXPECT_TRUE(grid.query(QRectF(400, 400, 10, 10)).empty());
    grid.remove(1, QRectF(50, 50, 200, 10));
    EXPECT_TRUE(grid.query(QRectF(0, 0, 300, 100)).empty());
    EXPECT_EQ(grid.query(QRectF(-1000, -1000, 3000, 3000)), (std::vector<qint64>{2}));
}

TEST(InkDocumentTest, ItemsKeepCreationOrderAndPaintByLayerThenT)
{
    InkDocument doc;
    const qint64 a = doc.addStroke(line(0, 0, 10, 0), Layer::User);
    const qint64 b = doc.addStroke(line(0, 5, 10, 5), Layer::Highlighter);
    const qint64 c = doc.addStroke(line(0, 9, 10, 9), Layer::User);
    EXPECT_LT(a, b);
    EXPECT_LT(b, c);
    const auto order = doc.paintOrder(QRectF(-100, -100, 200, 200));
    ASSERT_EQ(order.size(), 3u);
    EXPECT_EQ(order[0]->t, b); // highlighter under the pen strokes
    EXPECT_EQ(order[1]->t, a);
    EXPECT_EQ(order[2]->t, c);
    EXPECT_EQ(doc.itemsIntersecting(QRectF(0, 8, 10, 2)), (std::vector<qint64>{c}));
    EXPECT_TRUE(doc.removeItem(a));
    EXPECT_FALSE(doc.removeItem(a));
    EXPECT_EQ(doc.item(a), nullptr);
    EXPECT_EQ(doc.items().size(), 2u);
    EXPECT_GT(doc.addStroke(line(0, 0, 1, 1)), c) << "t never goes back, even after a removal";
}

TEST(InkDocumentTest, UndoRedoAreSnapshotsAndRedoClearsOnANewEdit)
{
    InkDocument doc;
    EXPECT_FALSE(doc.canUndo());
    const qint64 a = doc.addStroke(line(0, 0, 10, 0));
    doc.commit();
    EXPECT_TRUE(doc.canUndo());
    EXPECT_FALSE(doc.canRedo());
    const qint64 b = doc.addStroke(line(0, 0, 10, 10));
    doc.commit();
    ASSERT_TRUE(doc.undo());
    EXPECT_EQ(doc.items().size(), 1u);
    EXPECT_NE(doc.item(a), nullptr);
    EXPECT_EQ(doc.item(b), nullptr);
    EXPECT_TRUE(doc.canRedo());
    ASSERT_TRUE(doc.redo());
    EXPECT_NE(doc.item(b), nullptr);
    ASSERT_TRUE(doc.undo());
    doc.addStroke(line(5, 5, 6, 6)); // a new edit
    doc.commit();
    EXPECT_FALSE(doc.canRedo()) << "redo is cleared by a new edit";
    doc.commit(); // nothing changed since: nothing is pushed
    EXPECT_TRUE(doc.undo());
    EXPECT_TRUE(doc.undo());
    EXPECT_FALSE(doc.undo()) << "two undos, not three: the empty commit pushed nothing";
}

TEST(InkDocumentTest, HistoryIsCappedAtOneHundred)
{
    InkDocument doc;
    for (int i = 0; i < InkDocument::kHistoryCap + 20; ++i) {
        doc.addStroke(line(i, 0, i + 1, 0));
        doc.commit();
    }
    int undone = 0;
    while (doc.undo())
        ++undone;
    EXPECT_EQ(undone, InkDocument::kHistoryCap);
    EXPECT_EQ(doc.items().size(), 20u);
}

TEST(InkDocumentTest, ModifiedTracksTheSavedRevisionAcrossUndo)
{
    InkDocument doc;
    EXPECT_FALSE(doc.isModified());
    doc.addStroke(line(0, 0, 1, 1));
    doc.commit();
    EXPECT_TRUE(doc.isModified());
    doc.markSaved();
    EXPECT_FALSE(doc.isModified());
    ASSERT_TRUE(doc.undo());
    EXPECT_TRUE(doc.isModified()) << "the file holds the stroke, the document does not";
}

TEST(InkDocumentTest, ContinuousVerticalGrowsByWholePagesAndFixedDoesNot)
{
    InkDocument doc;
    DocumentSettings settings;
    settings.format = {100, 200, 96};
    settings.extent = QRectF(0, 0, 100, 200);
    doc.setSettings(settings);
    EXPECT_EQ(doc.pageCount(), 1);
    EXPECT_TRUE(doc.growToContain(QRectF(10, 390, 10, 20))); // crosses into page 3
    EXPECT_EQ(doc.pageCount(), 3);
    EXPECT_EQ(doc.pageRect(2), QRectF(0, 400, 100, 200));
    EXPECT_FALSE(doc.growToContain(QRectF(10, 10, 10, 10)));
    settings.layout = LayoutKind::Fixed;
    settings.extent = QRectF(0, 0, 100, 200);
    doc.setSettings(settings);
    EXPECT_FALSE(doc.growToContain(QRectF(10, 390, 10, 20)));
    EXPECT_EQ(doc.pageCount(), 1);
    doc.addPage();
    EXPECT_EQ(doc.pageCount(), 2);
}

TEST(SpatialGridTest, HugeAndNonFiniteRectsNeitherHangNorAllocateWithoutBound)
{
    const double inf = std::numeric_limits<double>::infinity();
    SpatialGrid grid(100.0);
    grid.insert(1, QRectF(0, 0, 1e9, 1e9));       // ~1e14 cells if walked
    grid.insert(2, QRectF(10, 10, 5, 5));
    grid.insert(3, QRectF(0, 0, inf, inf));       // not indexed
    grid.insert(4, QRectF(std::nan(""), 0, 5, 5)); // not indexed
    grid.insert(5, QRectF(1e15, 1e15, 5, 5));      // far beyond the indexed range
    EXPECT_EQ(grid.query(QRectF(12, 12, 1, 1)), (std::vector<qint64>{1, 2}));
    EXPECT_EQ(grid.query(QRectF(5e8, 5e8, 1, 1)), (std::vector<qint64>{1}));
    EXPECT_TRUE(grid.query(QRectF(2e9, 2e9, 1, 1)).empty());
    EXPECT_EQ(grid.query(QRectF(-1e12, -1e12, 2e12, 2e12)), (std::vector<qint64>{1, 2})) << "a huge query walks the occupied cells, not its own";
    EXPECT_EQ(grid.query(QRectF(1e15, 1e15, 1, 1)), (std::vector<qint64>{5}));
    EXPECT_TRUE(grid.query(QRectF(0, 0, inf, inf)).empty()) << "a non-finite query matches nothing";
    grid.remove(1, QRectF(0, 0, 1e9, 1e9));
    grid.remove(5, QRectF(1e15, 1e15, 5, 5));
    EXPECT_EQ(grid.query(QRectF(-1e12, -1e12, 2e12, 2e12)), (std::vector<qint64>{2}));
}

TEST(SpatialGridTest, NegativeCoordinatesAndCellEdges)
{
    SpatialGrid grid(100.0);
    grid.insert(1, QRectF(-250, -30, 100, 10)); // negative cells only
    grid.insert(2, QRectF(0, 0, 100, 100));     // ends exactly on a cell edge
    EXPECT_EQ(grid.query(QRectF(-200, -25, 1, 1)), (std::vector<qint64>{1}));
    EXPECT_EQ(grid.query(QRectF(50, 50, 1, 1)), (std::vector<qint64>{2}));
    EXPECT_TRUE(grid.query(QRectF(150, 150, 1, 1)).empty());
}

TEST(InkDocumentTest, UndoWithAnUncommittedEditRestoresTheCommittedStateOnly)
{
    InkDocument doc;
    DocumentSettings settings;
    settings.format = {100, 200, 96};
    settings.extent = QRectF(0, 0, 100, 200);
    doc.setSettings(settings);
    doc.commit();
    const qint64 a = doc.addStroke(line(0, 0, 10, 10));
    doc.commit();
    ASSERT_TRUE(doc.growToContain(QRectF(10, 390, 10, 20))); // a live edit, never committed
    EXPECT_EQ(doc.pageCount(), 3);
    ASSERT_TRUE(doc.undo());
    EXPECT_EQ(doc.pageCount(), 1) << "the uncommitted growth is dropped";
    EXPECT_NE(doc.item(a), nullptr) << "and the committed stroke is not undone with it";
    EXPECT_FALSE(doc.canRedo()) << "the dirty state is discarded, not put on the redo stack";
    ASSERT_TRUE(doc.undo());
    EXPECT_EQ(doc.item(a), nullptr);
    ASSERT_TRUE(doc.redo());
    EXPECT_NE(doc.item(a), nullptr);
    EXPECT_EQ(doc.pageCount(), 1);
}

TEST(InkDocumentTest, GrowToContainTerminatesOnAZeroHeightPageAndOnHugeBounds)
{
    InkDocument doc;
    DocumentSettings settings;
    settings.format = {100, 0, 96};
    settings.extent = QRectF(0, 0, 100, 200);
    doc.setSettings(settings);
    EXPECT_FALSE(doc.growToContain(QRectF(10, 390, 10, 20)));
    EXPECT_EQ(doc.pageCount(), 1);
    settings.format = {100, 200, 96};
    doc.setSettings(settings);
    EXPECT_FALSE(doc.growToContain(QRectF(0, 1e15, 1, 1))) << "past the page cap: refused, not looped";
    EXPECT_FALSE(doc.growToContain(QRectF(0, std::numeric_limits<double>::infinity(), 1, 1)));
    EXPECT_EQ(doc.pageCount(), 1);
    EXPECT_TRUE(doc.growToContain(QRectF(0, 1e6, 1, 1)));
    EXPECT_EQ(doc.pageCount(), 5001);
}

TEST(InkDocumentTest, ReplaceKeepsTAndMovesTheGridCellAndUndoRestoresTheGrid)
{
    InkDocument doc;
    const qint64 a = doc.addStroke(line(0, 0, 10, 0));
    doc.commit();
    Item moved;
    moved.type = Item::Type::Stroke;
    moved.stroke = line(1000, 1000, 1010, 1000);
    EXPECT_TRUE(doc.replaceItem(a, moved));
    EXPECT_FALSE(doc.replaceItem(a + 99, moved));
    EXPECT_EQ(doc.item(a)->t, a);
    EXPECT_TRUE(doc.itemsIntersecting(QRectF(0, -5, 10, 10)).empty());
    EXPECT_EQ(doc.itemsIntersecting(QRectF(1000, 995, 10, 10)), (std::vector<qint64>{a}));
    doc.commit();
    EXPECT_TRUE(doc.removeItem(a));
    doc.commit();
    ASSERT_TRUE(doc.undo());
    EXPECT_EQ(doc.itemsIntersecting(QRectF(1000, 995, 10, 10)), (std::vector<qint64>{a})) << "the grid follows an undo";
    ASSERT_TRUE(doc.undo());
    EXPECT_EQ(doc.itemsIntersecting(QRectF(0, -5, 10, 10)), (std::vector<qint64>{a}));
    EXPECT_TRUE(doc.itemsIntersecting(QRectF(1000, 995, 10, 10)).empty());
    const qint64 next = doc.addStroke(line(0, 0, 1, 1));
    EXPECT_GT(next, a);
}

TEST(InkDocumentTest, TIsNeverReusedAfterAnUndo)
{
    InkDocument doc;
    doc.commit();
    const qint64 a = doc.addStroke(line(0, 0, 1, 1));
    doc.commit();
    ASSERT_TRUE(doc.undo());
    EXPECT_GT(doc.addStroke(line(0, 0, 1, 1)), a);
}

namespace {

/// A gzip member: zlib's own framing (windowBits 16 + MAX_WBITS), no hand-rolled CRC.
QByteArray gzipBytes(const QByteArray &plain)
{
    z_stream stream{};
    deflateInit2(&stream, Z_BEST_COMPRESSION, Z_DEFLATED, 16 + MAX_WBITS, 8, Z_DEFAULT_STRATEGY);
    QByteArray out(int(deflateBound(&stream, uLong(plain.size()))), Qt::Uninitialized);
    stream.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(plain.constData()));
    stream.avail_in = uInt(plain.size());
    stream.next_out = reinterpret_cast<Bytef *>(out.data());
    stream.avail_out = uInt(out.size());
    deflate(&stream, Z_FINISH);
    out.resize(int(stream.total_out));
    deflateEnd(&stream);
    return out;
}

InkDocument sampleDocument()
{
    InkDocument doc;
    auto stroke = std::make_shared<Stroke>();
    stroke->style = {2.0, QColor(0x1e, 0x1e, 0x1e), PressureCurve::Linear};
    stroke->points = {{12.5, 40.25, 0.43}, {13.0, 41.5, 0.51}, {13.337, 42.001, 0.6}};
    doc.addStroke(stroke);
    Item box;
    box.type = Item::Type::TextBox;
    box.textBox = std::make_shared<TextBox>(TextBox{QRectF(100, 200, 400, 60), QStringLiteral("See [[Lecture 3]] #exam")});
    doc.addItem(std::move(box));
    doc.commit();
    return doc;
}

} // namespace

TEST(InkFormatTest, WritesCompactJSONWithThreeDecimals)
{
    const QByteArray bytes = writeInk(sampleDocument());
    const QJsonDocument json = QJsonDocument::fromJson(bytes);
    ASSERT_TRUE(json.isObject()) << "the whole thing is valid JSON";
    EXPECT_EQ(bytes, json.toJson(QJsonDocument::Compact)) << "the writer is QJsonDocument's compact form and nothing else";
    EXPECT_FALSE(bytes.contains('\n')) << "compact: no line breaks";
    const QJsonObject top = json.object();
    EXPECT_EQ(top.value(QStringLiteral("format")).toString(), QStringLiteral("notolith-ink"));
    EXPECT_EQ(top.value(QStringLiteral("version")).toInt(), 1);
    const QJsonArray items = top.value(QStringLiteral("items")).toArray();
    ASSERT_EQ(items.size(), 2);
    const QJsonObject stroke = items[0].toObject();
    EXPECT_EQ(stroke.value(QStringLiteral("type")).toString(), QStringLiteral("stroke"));
    EXPECT_EQ(stroke.value(QStringLiteral("t")).toInt(), 1);
    EXPECT_EQ(stroke.value(QStringLiteral("layer")).toString(), QStringLiteral("user"));
    const QJsonArray points = stroke.value(QStringLiteral("points")).toArray();
    ASSERT_EQ(points.size(), 9);
    EXPECT_DOUBLE_EQ(points[6].toDouble(), 13.337) << "three decimals survive";
    EXPECT_DOUBLE_EQ(points[7].toDouble(), 42.001);
    EXPECT_TRUE(bytes.contains("13.337,42.001,0.6")) << bytes.toStdString();
    EXPECT_TRUE(bytes.contains("\"rect\":[100,200,400,60]"));
    const QJsonObject box = items[1].toObject();
    EXPECT_EQ(box.value(QStringLiteral("type")).toString(), QStringLiteral("textbox"));
    EXPECT_EQ(box.value(QStringLiteral("markdown")).toString(), QStringLiteral("See [[Lecture 3]] #exam"));
}

TEST(InkFormatTest, NumbersAreRoundedToThreeDecimals)
{
    EXPECT_DOUBLE_EQ(roundThree(1.23456), 1.235);
    EXPECT_DOUBLE_EQ(roundThree(2.0004), 2.0);
    EXPECT_DOUBLE_EQ(roundThree(-0.0004), 0.0);
    InkDocument doc;
    auto stroke = std::make_shared<Stroke>();
    stroke->points = {{1.23456, 2.00049, 0.123456}, {3.0, 4.0, 0.5}};
    doc.addStroke(stroke);
    doc.commit();
    EXPECT_TRUE(writeInk(doc).contains("[1.235,2,0.123,3,4,0.5]")) << writeInk(doc).toStdString();
}

TEST(InkFormatTest, RoundTripsAndKeepsUnknownKeysAndItemTypes)
{
    const QByteArray original = QByteArrayLiteral(
        "{\"format\":\"notolith-ink\",\"version\":1,\"document\":{\"format\":{\"width\":100,\"height\":200,\"dpi\":96},"
        "\"background\":{\"color\":\"#ffffff\",\"pattern\":\"cornell\",\"patternSize\":[120,32],\"patternColor\":\"#c0c8d0\"},"
        "\"layout\":\"fixed\",\"extent\":{\"x\":0,\"y\":0,\"width\":100,\"height\":400},"
        "\"glyph\":{\"codepoint\":97,\"baseline\":150}},\"futureKey\":true,\"items\":[\n"
        "{\"type\":\"stroke\",\"t\":1,\"layer\":\"highlighter\",\"style\":{\"width\":12,\"color\":\"#ffee0080\",\"pressureCurve\":\"const\"},"
        "\"points\":[1,2,0.5,3,4,0.5],\"mood\":\"happy\"},\n"
        "{\"type\":\"section\",\"id\":\"s1\",\"t\":2,\"items\":[1]},\n"
        "{\"type\":\"shape\",\"t\":3,\"layer\":\"user\",\"kind\":\"rect\",\"style\":{\"width\":2,\"color\":\"#000000\",\"pressureCurve\":\"linear\"},\"points\":[10,10,50,30]}\n"
        "]}\n");
    InkDocument doc;
    const InkReadResult read = readInk(original, doc);
    ASSERT_TRUE(read.ok) << read.error.toStdString();
    EXPECT_FALSE(read.readOnly);
    EXPECT_EQ(read.version, 1);
    ASSERT_EQ(doc.items().size(), 3u);
    EXPECT_EQ(doc.items()[0].layer, Layer::Highlighter);
    EXPECT_EQ(doc.items()[0].stroke->style.pressureCurve, PressureCurve::Const);
    EXPECT_EQ(doc.items()[0].stroke->style.color, QColor(0xff, 0xee, 0x00, 0x80));
    EXPECT_EQ(doc.items()[1].type, Item::Type::Unknown);
    EXPECT_EQ(doc.items()[2].shape->kind, ShapeKind::Rect);
    EXPECT_EQ(doc.settings().layout, LayoutKind::Fixed);
    EXPECT_EQ(doc.settings().background.pattern, Pattern::Cornell);
    EXPECT_EQ(doc.settings().background.patternSize, QSizeF(120, 32));
    EXPECT_EQ(doc.pageCount(), 2);
    EXPECT_EQ(doc.settings().extra.value(QStringLiteral("glyph")).toObject().value(QStringLiteral("codepoint")).toInt(), 97);
    EXPECT_TRUE(doc.extraTopLevel().value(QStringLiteral("futureKey")).toBool());

    const QByteArray written = writeInk(doc);
    const QJsonObject writtenTop = QJsonDocument::fromJson(written).object();
    const QJsonArray writtenItems = writtenTop.value(QStringLiteral("items")).toArray();
    ASSERT_EQ(writtenItems.size(), 3);
    EXPECT_EQ(writtenItems[0].toObject().value(QStringLiteral("mood")).toString(), QStringLiteral("happy"));
    EXPECT_EQ(writtenItems[1].toObject(),
              QJsonDocument::fromJson(QByteArrayLiteral("{\"type\":\"section\",\"id\":\"s1\",\"t\":2,\"items\":[1]}")).object())
        << "an unknown item is written back as it was read: no key added, none changed";
    const QJsonObject writtenDocument = writtenTop.value(QStringLiteral("document")).toObject();
    EXPECT_EQ(writtenDocument.value(QStringLiteral("glyph")).toObject().value(QStringLiteral("codepoint")).toInt(), 97);
    EXPECT_TRUE(writtenTop.value(QStringLiteral("futureKey")).toBool());
    EXPECT_EQ(writtenDocument.value(QStringLiteral("background")).toObject().value(QStringLiteral("patternSize")).toArray(),
              (QJsonArray{120, 32}));
    InkDocument again;
    ASSERT_TRUE(readInk(written, again).ok);
    EXPECT_EQ(writeInk(again), written) << "a second round trip is byte-stable";
}

TEST(InkFormatTest, ReadsAGzipWrappedFile)
{
    const QByteArray plain = writeInk(sampleDocument());
    InkDocument doc;
    const InkReadResult read = readInk(gzipBytes(plain), doc);
    ASSERT_TRUE(read.ok) << read.error.toStdString();
    EXPECT_EQ(doc.items().size(), 2u);
    EXPECT_EQ(writeInk(doc), plain) << "saved back as plain JSON: gzip is a reader feature in v1";
}

TEST(InkFormatTest, AVersionTwoFileOpensReadOnly)
{
    QByteArray bytes = writeInk(sampleDocument());
    bytes.replace("\"version\":1", "\"version\":2");
    InkDocument doc;
    const InkReadResult read = readInk(bytes, doc);
    EXPECT_TRUE(read.ok);
    EXPECT_TRUE(read.readOnly);
    EXPECT_EQ(read.version, 2);
    EXPECT_EQ(doc.items().size(), 2u) << "read as far as v1 understands";
}

TEST(InkFormatTest, TruncatedAndForeignFilesAreRefusedWithTheName)
{
    const QByteArray plain = writeInk(sampleDocument());
    InkDocument doc = sampleDocument(); // populated, so "untouched" means something
    InkReadResult read = readInk(plain.left(plain.size() / 2), doc, QStringLiteral("Lecture 3.ink"));
    EXPECT_FALSE(read.ok);
    EXPECT_TRUE(read.error.contains(QStringLiteral("Lecture 3.ink")));
    EXPECT_EQ(writeInk(doc), plain) << "a failed read leaves the document untouched";
    const QByteArray gz = gzipBytes(plain);
    read = readInk(gz.left(gz.size() - 12), doc, QStringLiteral("x.ink"));
    EXPECT_FALSE(read.ok) << "a truncated gzip stream is an error, not a shorter note";
    read = readInk(QByteArrayLiteral("{\"format\":\"something-else\",\"version\":1,\"items\":[]}"), doc);
    EXPECT_FALSE(read.ok);
    read = readInk(QByteArrayLiteral("{\"format\":\"notolith-ink\",\"version\":1,\"items\":[{\"type\":\"stroke\",\"t\":1,\"points\":[1,2]}]}"), doc);
    EXPECT_FALSE(read.ok) << "points must be x,y,pressure triples";
    EXPECT_EQ(writeInk(doc), plain) << "none of the refused reads touched the document";
}

TEST(InkFormatTest, NonFiniteAndNonPositiveNumbersAreRefused)
{
    // JSON has no NaN literal; 1e999 is how a file gets an infinity toward the loader, and Qt's parser
    // refuses it (observed: the 1e999 cases pass with no finiteness check of our own). This test pins
    // that, because InkDocument::pageCount() is undefined behaviour on a NaN extent.
    const auto file = [](const char *document, const char *items = "") {
        return QByteArrayLiteral("{\"format\":\"notolith-ink\",\"version\":1,\"document\":") + document + QByteArrayLiteral(",\"items\":[")
            + items + QByteArrayLiteral("]}");
    };
    const struct {
        const char *what;
        QByteArray bytes;
    } bad[] = {
        {"infinite page height", file("{\"format\":{\"width\":100,\"height\":1e999,\"dpi\":96}}")},
        {"infinite extent height", file("{\"extent\":{\"x\":0,\"y\":0,\"width\":100,\"height\":1e999}}")},
        {"negative infinite extent", file("{\"extent\":{\"x\":0,\"y\":0,\"width\":100,\"height\":-1e999}}")},
        {"zero page height", file("{\"format\":{\"width\":100,\"height\":0,\"dpi\":96}}")},
        {"negative page height", file("{\"format\":{\"width\":100,\"height\":-5,\"dpi\":96}}")},
        {"zero page width", file("{\"format\":{\"width\":0,\"height\":100,\"dpi\":96}}")},
        {"zero dpi", file("{\"format\":{\"width\":100,\"height\":100,\"dpi\":0}}")},
        {"zero extent height", file("{\"extent\":{\"x\":0,\"y\":0,\"width\":100,\"height\":0}}")},
        {"zero pattern size", file("{\"background\":{\"patternSize\":0}}")},
        {"tiny pattern size", file("{\"background\":{\"patternSize\":1e-300}}")},
        {"infinite stroke point", file("{}", "{\"type\":\"stroke\",\"t\":1,\"points\":[1,1e999,0.5]}")},
        {"infinite stroke width", file("{}", "{\"type\":\"stroke\",\"t\":1,\"style\":{\"width\":1e999},\"points\":[1,2,0.5]}")},
        {"infinite text box", file("{}", "{\"type\":\"textbox\",\"t\":1,\"rect\":[0,0,1e999,10],\"markdown\":\"x\"}")},
        {"absurd t", file("{}", "{\"type\":\"stroke\",\"t\":1e300,\"points\":[1,2,0.5]}")},
        {"repeated t", file("{}", "{\"type\":\"stroke\",\"t\":1,\"points\":[1,2,0.5]},{\"type\":\"stroke\",\"t\":1,\"points\":[3,4,0.5]}")},
    };
    for (const auto &c : bad) {
        InkDocument doc = sampleDocument();
        const InkReadResult read = readInk(c.bytes, doc, QStringLiteral("bad.ink"));
        EXPECT_FALSE(read.ok) << c.what;
        EXPECT_TRUE(read.error.contains(QStringLiteral("bad.ink"))) << c.what << ": " << read.error.toStdString();
        EXPECT_EQ(doc.items().size(), 2u) << c.what << ": a refused file leaves the document as it was";
    }
    InkDocument doc;
    EXPECT_TRUE(readInk(file("{\"format\":{\"width\":100,\"height\":100,\"dpi\":96},\"extent\":{\"x\":0,\"y\":0,\"width\":100,\"height\":100}}"), doc).ok)
        << "the same shape with sane numbers loads";
}

TEST(InkFormatTest, GzipIsCappedWhenUnpackedAndRefusesLeftovers)
{
    const QByteArray plain = writeInk(sampleDocument());
    const QByteArray gz = gzipBytes(plain);
    InkDocument doc;
    EXPECT_TRUE(readInk(gz, doc, QStringLiteral("big.ink"), plain.size()).ok) << "exactly the cap is fine";
    doc = sampleDocument();
    InkReadResult read = readInk(gz, doc, QStringLiteral("big.ink"), plain.size() - 1);
    EXPECT_FALSE(read.ok) << "one byte over the cap is refused";
    EXPECT_TRUE(read.error.contains(QStringLiteral("big.ink")) && read.error.contains(QStringLiteral("too large"))) << read.error.toStdString();
    EXPECT_EQ(writeInk(doc), plain);
    const QByteArray bomb = gzipBytes(QByteArray(8 << 20, ' ')); // a few KB that inflate to 8 MiB
    EXPECT_LT(bomb.size(), 64 * 1024);
    EXPECT_FALSE(readInk(bomb, doc, QStringLiteral("bomb.ink"), 1 << 20).ok);
    EXPECT_GT(kMaxInflatedBytes, qint64(100) << 20) << "the real cap leaves room for a heavy note";
    EXPECT_FALSE(readInk(gz + QByteArrayLiteral("junk"), doc, QStringLiteral("x.ink")).ok) << "bytes after the stream are not silently dropped";
    EXPECT_FALSE(readInk(gz + gz, doc, QStringLiteral("x.ink")).ok) << "a second gzip member is not silently dropped";
    EXPECT_FALSE(readInk(QByteArray("\x1f\x8b", 2), doc, QStringLiteral("x.ink")).ok);

    QTemporaryDir tmp;
    const QString path = tmp.filePath(QStringLiteral("huge.ink"));
    QFile huge(path);
    ASSERT_TRUE(huge.open(QIODevice::WriteOnly));
    ASSERT_TRUE(huge.resize(kMaxInflatedBytes + 1)) << "a sparse file: no disk or memory used";
    huge.close();
    EXPECT_FALSE(loadInkFile(path, doc, &read));
    EXPECT_TRUE(read.error.contains(QStringLiteral("huge.ink")) && read.error.contains(QStringLiteral("too large"))) << read.error.toStdString();
}

TEST(InkFormatTest, TheWriterNeverWritesNullAndKeepsHugeNumbers)
{
    EXPECT_DOUBLE_EQ(roundThree(1e306), 1e306) << "value * 1000 would overflow";
    EXPECT_TRUE(std::isnan(roundThree(std::nan(""))));
    constexpr double inf = std::numeric_limits<double>::infinity();
    InkDocument doc;
    auto stroke = std::make_shared<Stroke>();
    stroke->style.width = 1e306;
    stroke->points = {{std::nan(""), inf, 0.5}, {1e306, -1e306, 0.5}};
    doc.addStroke(stroke);
    doc.commit();
    const QByteArray bytes = writeInk(doc);
    EXPECT_FALSE(bytes.contains("null")) << bytes.toStdString();
    InkDocument again;
    const InkReadResult read = readInk(bytes, again);
    ASSERT_TRUE(read.ok) << read.error.toStdString();
    const auto &read0 = again.items()[0].stroke;
    EXPECT_DOUBLE_EQ(read0->style.width, 1e306);
    EXPECT_EQ(read0->points[0].x, 0.0) << "a non-finite number is written as 0";
    EXPECT_EQ(read0->points[0].y, 0.0);
    EXPECT_DOUBLE_EQ(read0->points[1].x, 1e306);
    EXPECT_DOUBLE_EQ(read0->points[1].y, -1e306);
}

TEST(InkFormatTest, UnknownKeysSurviveAtEveryLevel)
{
    const QByteArray original = QByteArrayLiteral(
        "{\"format\":\"notolith-ink\",\"version\":1,\"top\":1,\"document\":{\"doc\":2,\"format\":{\"width\":100,\"height\":200,\"dpi\":96,\"fmt\":3},"
        "\"background\":{\"color\":\"#ffffff\",\"pattern\":\"grid\",\"patternSize\":32,\"patternColor\":\"#c0c8d0\",\"bg\":4},"
        "\"layout\":\"fixed\",\"extent\":{\"x\":0,\"y\":0,\"width\":100,\"height\":200,\"ext\":5}},\"items\":["
        "{\"type\":\"stroke\",\"t\":1,\"item\":6,\"style\":{\"width\":2,\"color\":\"#000000\",\"pressureCurve\":\"linear\",\"sty\":7},\"points\":[1,2,0.5]},"
        "{\"type\":\"shape\",\"t\":2,\"kind\":\"line\",\"style\":{\"width\":2,\"color\":\"#000000\",\"pressureCurve\":\"linear\",\"sty2\":8},\"points\":[1,2,3,4]}]}");
    InkDocument doc;
    ASSERT_TRUE(readInk(original, doc).ok);
    const QJsonObject top = QJsonDocument::fromJson(writeInk(doc)).object();
    const QJsonObject document = top.value(QStringLiteral("document")).toObject();
    const QJsonArray items = top.value(QStringLiteral("items")).toArray();
    EXPECT_EQ(top.value(QStringLiteral("top")).toInt(), 1);
    EXPECT_EQ(document.value(QStringLiteral("doc")).toInt(), 2);
    EXPECT_EQ(document.value(QStringLiteral("format")).toObject().value(QStringLiteral("fmt")).toInt(), 3);
    EXPECT_EQ(document.value(QStringLiteral("background")).toObject().value(QStringLiteral("bg")).toInt(), 4);
    EXPECT_EQ(document.value(QStringLiteral("extent")).toObject().value(QStringLiteral("ext")).toInt(), 5);
    EXPECT_EQ(items[0].toObject().value(QStringLiteral("item")).toInt(), 6);
    EXPECT_EQ(items[0].toObject().value(QStringLiteral("style")).toObject().value(QStringLiteral("sty")).toInt(), 7);
    EXPECT_EQ(items[1].toObject().value(QStringLiteral("style")).toObject().value(QStringLiteral("sty2")).toInt(), 8);
    EXPECT_EQ(document.value(QStringLiteral("format")).toObject().value(QStringLiteral("width")).toInt(), 100)
        << "the owned keys are still written from the model";
}

TEST(InkFormatTest, AWrongTypedContainerIsRefusedNotLoadedAsEmpty)
{
    const auto file = [](const char *document, const char *items) {
        return QByteArrayLiteral("{\"format\":\"notolith-ink\",\"version\":1,\"document\":") + document + QByteArrayLiteral(",\"items\":")
            + items + QByteArrayLiteral("}");
    };
    const QByteArray stroke = QByteArrayLiteral("{\"type\":\"stroke\",\"t\":1,\"points\":[1,2,0.5]");
    const struct {
        const char *what;
        QByteArray bytes;
    } bad[] = {
        {"items is an object", file("{}", "{\"a\":1}")},
        {"items is a string", file("{}", "\"x\"")},
        {"an item is a string", file("{}", "[\"x\"]")},
        {"document is an array", file("[]", "[]")},
        {"format is a string", file("{\"format\":\"a4\"}", "[]")},
        {"background is an array", file("{\"background\":[1]}", "[]")},
        {"extent is a number", file("{\"extent\":5}", "[]")},
        {"style is a string", file("{}", "[" + stroke + ",\"style\":\"bold\"}]")},
        {"points is an object", file("{}", "[{\"type\":\"stroke\",\"t\":1,\"points\":{\"a\":1}}]")},
        {"rect has three numbers", file("{}", "[{\"type\":\"textbox\",\"t\":1,\"rect\":[1,2,3],\"markdown\":\"x\"}]")},
        {"shape points is a string", file("{}", "[{\"type\":\"shape\",\"t\":1,\"kind\":\"line\",\"points\":\"x\"}]")},
    };
    for (const auto &c : bad) {
        InkDocument doc = sampleDocument();
        const QByteArray before = writeInk(doc);
        const InkReadResult read = readInk(c.bytes, doc, QStringLiteral("bad.ink"));
        EXPECT_FALSE(read.ok) << c.what;
        EXPECT_TRUE(read.error.contains(QStringLiteral("bad.ink"))) << c.what;
        EXPECT_EQ(writeInk(doc), before) << c.what;
    }
}

TEST(InkFormatTest, OnlyAnExactVersionOneIsWritable)
{
    const auto file = [](const char *version) {
        return QByteArrayLiteral("{\"format\":\"notolith-ink\",\"version\":") + version + QByteArrayLiteral(",\"items\":[]}");
    };
    const struct {
        const char *version;
        bool ok;
        bool readOnly;
        int reported;
    } cases[] = {
        {"1", true, false, 1},      {"2", true, true, 2},         {"3000000000", true, true, std::numeric_limits<int>::max()},
        {"1.5", true, true, 2},     {"\"2\"", false, false, 0},   {"0", false, false, 0},
        {"-1", false, false, 0},    {"0.5", false, false, 0},     {"true", false, false, 0},
        {"null", false, false, 0},  {"[1]", false, false, 0},
    };
    for (const auto &c : cases) {
        InkDocument doc;
        const InkReadResult read = readInk(file(c.version), doc, QStringLiteral("v.ink"));
        EXPECT_EQ(read.ok, c.ok) << c.version;
        EXPECT_EQ(read.readOnly, c.readOnly) << c.version;
        EXPECT_EQ(read.version, c.reported) << c.version;
        if (!c.ok) {
            EXPECT_TRUE(read.error.contains(QStringLiteral("v.ink")) && read.error.contains(QStringLiteral("version"))) << c.version << ": " << read.error.toStdString();
        }
    }
    InkDocument doc;
    EXPECT_FALSE(readInk(QByteArrayLiteral("{\"format\":\"notolith-ink\",\"items\":[]}"), doc).ok) << "no version at all";
}

TEST(InkFormatTest, ShapesWithTooFewPointsAreRefused)
{
    const auto shape = [](const char *kind, const char *points) {
        return QByteArrayLiteral("{\"format\":\"notolith-ink\",\"version\":1,\"items\":[{\"type\":\"shape\",\"t\":1,\"kind\":\"") + kind
            + QByteArrayLiteral("\",\"points\":[") + points + QByteArrayLiteral("]}]}");
    };
    const struct {
        const char *kind;
        const char *points;
        bool ok;
    } cases[] = {
        {"line", "1,2,3,4", true},   {"line", "1,2", false},        {"line", "", false},         {"arrow", "1,2", false},
        {"rect", "", false},         {"rect", "1,2", false},        {"rect", "1,2,3,4", true},   {"ellipse", "1,2", false},
        {"line", "1,2,3,4,5", false}, {"polyline", "1,2", false},   {"polygon", "1,2,3,4", false}, {"polygon", "1,2,3,4,5,6", true},
    };
    for (const auto &c : cases) {
        InkDocument doc;
        const InkReadResult read = readInk(shape(c.kind, c.points), doc, QStringLiteral("s.ink"));
        EXPECT_EQ(read.ok, c.ok) << c.kind << " [" << c.points << "] " << read.error.toStdString();
    }
}


TEST(InkFormatTest, SavesAndLoadsThroughAnAtomicReplace)
{
    QTemporaryDir tmp;
    const QString path = tmp.filePath(QStringLiteral("note.ink"));
    QString error;
    ASSERT_TRUE(saveInkFile(path, sampleDocument(), &error)) << error.toStdString();
    InkDocument doc;
    InkReadResult read;
    ASSERT_TRUE(loadInkFile(path, doc, &read)) << read.error.toStdString();
    EXPECT_EQ(doc.items().size(), 2u);
    EXPECT_FALSE(doc.isModified());
    EXPECT_TRUE(QDir(tmp.path()).entryList(QDir::Files | QDir::Hidden).size() == 1) << "no temp file left behind";
    EXPECT_FALSE(loadInkFile(tmp.filePath(QStringLiteral("missing.ink")), doc, &read));
    EXPECT_TRUE(read.error.contains(QStringLiteral("missing.ink")));
}

// ---- Task 5: the outline composer ----

namespace {

std::vector<std::shared_ptr<const Stroke>> goldenStrokes()
{
    std::vector<std::shared_ptr<const Stroke>> out;
    auto wave = std::make_shared<Stroke>();
    wave->style = {6.0, QColor(0x1e, 0x1e, 0x1e), PressureCurve::Linear};
    for (int i = 0; i <= 60; ++i) {
        const double x = 20 + i * 4.0;
        wave->points.push_back({x, 60 + 25 * std::sin(i / 6.0), 0.2 + 0.8 * std::abs(std::sin(i / 10.0))});
    }
    out.push_back(wave);
    // Crosses the wave: the overlap must stay one flat translucent yellow.
    auto marker = std::make_shared<Stroke>();
    marker->style = {14.0, QColor(0xff, 0xee, 0x00, 0x80), PressureCurve::Const};
    marker->points = {{30, 105, 1.0}, {230, 45, 1.0}};
    out.push_back(marker);
    // Crosses itself in an X: the middle must not be darker than the arms.
    auto cross = std::make_shared<Stroke>();
    cross->style = {12.0, QColor(0x00, 0xa0, 0x40, 0x80), PressureCurve::Const};
    cross->points = {{200, 120, 1.0}, {280, 170, 1.0}, {280, 120, 1.0}, {200, 170, 1.0}};
    out.push_back(cross);
    auto tap = std::make_shared<Stroke>();
    tap->style = {8.0, QColor(0xc0, 0x00, 0x00), PressureCurve::Sqrt};
    tap->points = {{250, 60, 0.7}};
    out.push_back(tap);
    auto zigzag = std::make_shared<Stroke>();
    zigzag->style = {3.0, QColor(0x00, 0x50, 0xc0), PressureCurve::Pow2};
    zigzag->points = {{20, 180, 0.9}, {60, 160, 0.3}, {100, 180, 0.9}, {140, 160, 0.3}, {180, 180, 0.9}};
    out.push_back(zigzag);
    return out;
}

/// Golden tolerance. A pixel "differs" when any channel is off by more than 8
/// (of 255), "strongly" when off by more than 64. The whole image matches when
/// under 0.5% of its pixels differ; that absorbs antialiasing drift between Qt
/// builds. A whole-image share dilutes small features, so every stroke's
/// padded box is checked on its own: there under 0.5% may differ and at most 2
/// pixels strongly (AA drift moves edge pixels by a fraction of the coverage,
/// a lost dot or cap moves tens of pixels by 100+).
constexpr double kGoldenMaxShare = 0.005;
constexpr int kGoldenMaxStrongPixelsPerRegion = 2;

bool regionMatches(const QImage &x, const QImage &y, const QRect &region, bool wholeImage, QString *why)
{
    const QRect r = region.intersected(x.rect());
    qint64 bad = 0, strong = 0;
    for (int row = r.top(); row <= r.bottom(); ++row) {
        const QRgb *p = reinterpret_cast<const QRgb *>(x.constScanLine(row));
        const QRgb *q = reinterpret_cast<const QRgb *>(y.constScanLine(row));
        for (int col = r.left(); col <= r.right(); ++col) {
            const int d = std::max({std::abs(qRed(p[col]) - qRed(q[col])), std::abs(qGreen(p[col]) - qGreen(q[col])),
                                    std::abs(qBlue(p[col]) - qBlue(q[col])), std::abs(qAlpha(p[col]) - qAlpha(q[col]))});
            bad += d > 8;
            strong += d > 64;
        }
    }
    const double share = r.isEmpty() ? 0.0 : double(bad) / (double(r.width()) * r.height());
    *why = QStringLiteral("region %1,%2 %3x%4: %5 of pixels differ by more than 8, %6 by more than 64")
               .arg(r.x()).arg(r.y()).arg(r.width()).arg(r.height()).arg(share, 0, 'f', 4).arg(strong);
    return share < kGoldenMaxShare && (wholeImage || strong <= kGoldenMaxStrongPixelsPerRegion);
}

/// One padded box per stroke, in physical pixels of an image rendered at `scale`.
std::vector<QRect> strokeRegions(const std::vector<std::shared_ptr<const Stroke>> &strokes, const QRectF &area, double scale)
{
    std::vector<QRect> out;
    for (const auto &stroke : strokes) {
        const QRectF box = stroke->bounds().adjusted(-2, -2, 2, 2).translated(-area.topLeft());
        out.push_back(QRectF(box.x() * scale, box.y() * scale, box.width() * scale, box.height() * scale).toAlignedRect());
    }
    return out;
}

bool imagesMatch(const QImage &a, const QImage &b, const std::vector<QRect> &regions, QString *why)
{
    if (a.size() != b.size()) {
        *why = QStringLiteral("size %1x%2 vs %3x%4").arg(a.width()).arg(a.height()).arg(b.width()).arg(b.height());
        return false;
    }
    const QImage x = a.convertToFormat(QImage::Format_ARGB32), y = b.convertToFormat(QImage::Format_ARGB32);
    if (!regionMatches(x, y, x.rect(), true, why))
        return false;
    for (const QRect &region : regions)
        if (!regionMatches(x, y, region, false, why))
            return false;
    return true;
}

void checkGolden(const QString &name, const QImage &image, const std::vector<QRect> &regions)
{
    const QString path = QStringLiteral(NOTEAPPC_INK_GOLDEN_DIR "/") + name;
    if (qEnvironmentVariableIsSet("NOTEAPPC_UPDATE_GOLDEN")) {
        QDir().mkpath(QStringLiteral(NOTEAPPC_INK_GOLDEN_DIR));
        ASSERT_TRUE(image.save(path));
        GTEST_SKIP() << "golden image written: " << path.toStdString() << " - look at it, then commit it";
    }
    QImage golden(path);
    ASSERT_FALSE(golden.isNull()) << "no golden image at " << path.toStdString() << "; run once with NOTEAPPC_UPDATE_GOLDEN=1";
    QString why;
    EXPECT_TRUE(imagesMatch(image, golden, regions, &why)) << name.toStdString() << ": " << why.toStdString();
}

bool pathIsFinite(const QPainterPath &path)
{
    for (int i = 0; i < path.elementCount(); ++i)
        if (!std::isfinite(path.elementAt(i).x) || !std::isfinite(path.elementAt(i).y))
            return false;
    return true;
}

} // namespace

TEST(StrokeOutlineTest, OutlineIsOnePathPerStrokeFilledOnceWithWindingFill)
{
    Stroke stroke;
    stroke.style = {4.0, Qt::black, PressureCurve::Const};
    stroke.points = {{0, 0, 1}, {10, 0, 1}, {20, 0, 1}};
    const QPainterPath path = strokeOutline(stroke);
    EXPECT_EQ(path.fillRule(), Qt::WindingFill);
    EXPECT_TRUE(path.contains(QPointF(10, 0)));
    EXPECT_TRUE(path.contains(QPointF(5, 1.5)));
    EXPECT_FALSE(path.contains(QPointF(5, 2.5)));
    EXPECT_TRUE(path.contains(QPointF(-1.5, 0))) << "round start cap";
    EXPECT_TRUE(path.contains(QPointF(21.5, 0))) << "round end cap";
    EXPECT_FALSE(path.contains(QPointF(-2.5, 0)));
    EXPECT_NEAR(path.boundingRect().left(), -2.0, 0.05);
    EXPECT_NEAR(path.boundingRect().right(), 22.0, 0.05);
}

TEST(StrokeOutlineTest, WidthFollowsPressureAlongTheStroke)
{
    Stroke stroke;
    stroke.style = {10.0, Qt::black, PressureCurve::Linear};
    stroke.points = {{0, 0, 0.2}, {100, 0, 1.0}};
    const QPainterPath path = strokeOutline(stroke);
    EXPECT_TRUE(path.contains(QPointF(0, 0.9)));
    EXPECT_FALSE(path.contains(QPointF(0, 1.1))) << "2 px wide at the start";
    EXPECT_TRUE(path.contains(QPointF(100, 4.9)));
    EXPECT_FALSE(path.contains(QPointF(100, 5.1))) << "10 px wide at the end";
}

TEST(StrokeOutlineTest, ATapAndCoincidingPointsDrawADotAndNoNaN)
{
    Stroke tap;
    tap.style = {8.0, Qt::black, PressureCurve::Const};
    tap.points = {{50, 50, 1.0}};
    QPainterPath path = strokeOutline(tap);
    EXPECT_TRUE(path.contains(QPointF(53.5, 50)));
    EXPECT_FALSE(path.contains(QPointF(54.5, 50)));
    tap.points = {{50, 50, 1.0}, {50, 50, 1.0}, {50, 50, 1.0}};
    path = strokeOutline(tap);
    EXPECT_TRUE(path.contains(QPointF(53.5, 50))) << "every point the same: still a dot";
    const QRectF box = path.boundingRect();
    EXPECT_FALSE(std::isnan(box.left()) || std::isnan(box.width()));
    EXPECT_TRUE(strokeOutline(Stroke()).isEmpty());
    Stroke mixed;
    mixed.style = {4.0, Qt::black, PressureCurve::Const};
    mixed.points = {{0, 0, 1}, {0, 0, 1}, {10, 0, 1}}; // a duplicate then a real segment
    EXPECT_TRUE(strokeOutline(mixed).contains(QPointF(5, 0)));
}

TEST(StrokeOutlineTest, HostileDataNeverGivesNaNOrHangs)
{
    struct Case {
        const char *name;
        double width;
        PressureCurve curve;
        std::vector<StrokePoint> points;
    };
    const double big = 1e12;
    const std::vector<Case> cases = {
        {"zero width", 0.0, PressureCurve::Const, {{0, 0, 1}, {10, 0, 1}, {20, 5, 1}}},
        {"zero pressure", 4.0, PressureCurve::Linear, {{0, 0, 0}, {10, 0, 0}, {20, 5, 0}}},
        {"zero pressure tap", 4.0, PressureCurve::Linear, {{3, 3, 0}}},
        {"zero then full pressure", 4.0, PressureCurve::Linear, {{0, 0, 0}, {10, 0, 1}, {20, 0, 0}}},
        {"huge coordinates", 4.0, PressureCurve::Const, {{-big, -big, 1}, {big, big, 1}, {big, -big, 0.3}}},
        {"huge tap", 4.0, PressureCurve::Const, {{big, big, 1}}},
        {"huge width", 1e300, PressureCurve::Const, {{0, 0, 1}, {big, 0, 1}}},
        {"denormal step", 4.0, PressureCurve::Const, {{0, 0, 1}, {1e-310, 0, 1}, {1, 0, 1}}},
        {"out of range pressure", 4.0, PressureCurve::Pow3, {{0, 0, -3}, {10, 0, 7}}},
        {"duplicates around a turn", 4.0, PressureCurve::Const, {{0, 0, 1}, {0, 0, 1}, {5, 5, 1}, {5, 5, 1}, {0, 0, 1}}},
    };
    for (const Case &c : cases) {
        SCOPED_TRACE(c.name);
        Stroke stroke;
        stroke.style = {c.width, Qt::black, c.curve};
        stroke.points = c.points;
        const QPainterPath path = strokeOutline(stroke);
        EXPECT_TRUE(pathIsFinite(path));
        EXPECT_EQ(path.fillRule(), Qt::WindingFill);
        for (size_t first = 0; first < c.points.size(); ++first)
            for (size_t last = first; last < c.points.size(); ++last)
                EXPECT_TRUE(pathIsFinite(strokeOutline(stroke, first, last)));
        // Painting must also return (the rasteriser clips to the image).
        QImage image(64, 64, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::white);
        QPainter painter(&image);
        painter.setRenderHint(QPainter::Antialiasing);
        paintStroke(painter, stroke);
    }
    // A range that does not fit the stroke is empty, not undefined.
    Stroke two;
    two.points = {{0, 0, 1}, {1, 0, 1}};
    EXPECT_TRUE(strokeOutline(two, 1, 0).isEmpty());
    EXPECT_TRUE(strokeOutline(two, 0, 5).isEmpty());
}

TEST(StrokeOutlineTest, NonFiniteInputDropsOnlyTheBadSegment)
{
    const double inf = std::numeric_limits<double>::infinity();
    const double nan = std::numeric_limits<double>::quiet_NaN();
    // The bad segment goes; every good one still draws; nothing stray, nothing non-finite.
    const auto boundsWithin = [](const QPainterPath &p, double l, double t, double r, double b) {
        const QRectF box = p.boundingRect();
        return pathIsFinite(p) && box.left() >= l && box.top() >= t && box.right() <= r && box.bottom() <= b;
    };
    {
        SCOPED_TRACE("NaN pressure mid-stroke: width falls back to the base width");
        Stroke stroke;
        stroke.style = {4.0, Qt::black, PressureCurve::Linear};
        stroke.points = {{0, 0, 1}, {10, 0, nan}, {20, 0, 1}};
        const QPainterPath path = strokeOutline(stroke);
        EXPECT_TRUE(path.contains(QPointF(5, 0)));
        EXPECT_TRUE(path.contains(QPointF(15, 0)));
        EXPECT_TRUE(path.contains(QPointF(10, 1.9))) << "base width 4 at the NaN point";
        EXPECT_TRUE(boundsWithin(path, -2.05, -2.05, 22.05, 2.05)) << "no stray line to the start";
    }
    {
        SCOPED_TRACE("inf coordinate in the middle");
        Stroke stroke;
        stroke.style = {4.0, Qt::black, PressureCurve::Const};
        stroke.points = {{0, 0, 1}, {10, 0, 1}, {inf, 0, 1}, {30, 0, 1}, {40, 0, 1}};
        const QPainterPath path = strokeOutline(stroke);
        EXPECT_TRUE(path.contains(QPointF(5, 0)));
        EXPECT_TRUE(path.contains(QPointF(35, 0)));
        EXPECT_FALSE(path.contains(QPointF(20, 0))) << "the segments touching the bad point are dropped";
        EXPECT_TRUE(boundsWithin(path, -2.05, -2.05, 42.05, 2.05));
    }
    {
        SCOPED_TRACE("NaN first point");
        Stroke stroke;
        stroke.style = {4.0, Qt::black, PressureCurve::Const};
        stroke.points = {{nan, 0, 1}, {10, 0, 1}, {20, 0, 1}};
        const QPainterPath path = strokeOutline(stroke);
        EXPECT_TRUE(path.contains(QPointF(15, 0)));
        EXPECT_TRUE(boundsWithin(path, 7.95, -2.05, 22.05, 2.05));
    }
    {
        SCOPED_TRACE("negative width is its absolute value");
        Stroke stroke;
        stroke.style = {-4.0, Qt::black, PressureCurve::Const};
        stroke.points = {{0, 0, 1}, {10, 0, 1}, {20, 0, 1}};
        const QPainterPath path = strokeOutline(stroke);
        EXPECT_TRUE(path.contains(QPointF(5, 1.5)));
        EXPECT_FALSE(path.contains(QPointF(5, 2.5)));
        EXPECT_TRUE(path.contains(QPointF(-1.5, 0))) << "round caps, as for width 4";
        EXPECT_TRUE(boundsWithin(path, -2.05, -2.05, 22.05, 2.05));
    }
    {
        SCOPED_TRACE("1e303 width is clamped, not dropped");
        Stroke stroke;
        stroke.style = {1e303, Qt::black, PressureCurve::Const};
        stroke.points = {{0, 0, 1}, {10, 0, 1}, {20, 0, 1}};
        const QPainterPath path = strokeOutline(stroke);
        EXPECT_FALSE(path.isEmpty());
        EXPECT_TRUE(pathIsFinite(path));
        EXPECT_TRUE(path.contains(QPointF(5, 0.49 * kMaxOutlineWidth)));
        EXPECT_FALSE(path.contains(QPointF(5, 0.51 * kMaxOutlineWidth)));
    }
    {
        SCOPED_TRACE("infinite width");
        Stroke stroke;
        stroke.style = {inf, Qt::black, PressureCurve::Const};
        stroke.points = {{0, 0, 1}, {10, 0, 1}};
        EXPECT_TRUE(pathIsFinite(strokeOutline(stroke)));
    }
}

TEST(StrokeOutlineTest, ASharpReversalHasNoSpikeAndNoGap)
{
    Stroke stroke;
    stroke.style = {10.0, Qt::black, PressureCurve::Const};
    stroke.points = {{0, 0, 1}, {50, 0, 1}, {0, 1, 1}};
    const QPainterPath path = strokeOutline(stroke);
    const QRectF box = path.boundingRect();
    EXPECT_LE(box.right(), 55.05) << "no miter spike past the round cap at the turn";
    EXPECT_GE(box.left(), -5.05);
    EXPECT_GE(box.top(), -5.05);
    EXPECT_LE(box.bottom(), 6.05);
    EXPECT_TRUE(path.contains(QPointF(52, 0))) << "the turn is capped round";
    EXPECT_TRUE(path.contains(QPointF(25, 0.5)));
    EXPECT_TRUE(path.contains(QPointF(50, 4.5)));
    EXPECT_TRUE(path.contains(QPointF(50, -4.5)));
    stroke.points = {{0, 0, 1}, {50, 0, 1}, {0, 0, 1}}; // exactly back on itself
    const QPainterPath back = strokeOutline(stroke);
    EXPECT_TRUE(pathIsFinite(back));
    EXPECT_NEAR(back.boundingRect().right(), 55.0, 0.05);
    EXPECT_TRUE(back.contains(QPointF(25, 0)));
}

TEST(StrokeOutlineTest, PartialOutlineCoversOnlyTheAskedSegments)
{
    Stroke stroke;
    stroke.style = {4.0, Qt::black, PressureCurve::Const};
    stroke.points = {{0, 0, 1}, {10, 0, 1}, {20, 0, 1}, {30, 0, 1}};
    const QPainterPath tail = strokeOutline(stroke, 2, 3);
    EXPECT_TRUE(tail.contains(QPointF(25, 0)));
    EXPECT_FALSE(tail.contains(QPointF(5, 0)));
}

TEST(StrokeOutlineTest, MatchesTheGoldenImagesAtTwoZoomsAndThreeRatios)
{
    const auto strokes = goldenStrokes();
    const QRectF area(0, 0, 300, 200);
    // zoom 1 / ratio 1; zoom 2; a fractional ratio (1.5, 450x300 exactly); zoom 2 at ratio 2.
    const struct { const char *name; double zoom, dpr; } cases[] = {
        {"outline-z1-d1.png", 1.0, 1.0}, {"outline-z2-d1.png", 2.0, 1.0}, {"outline-z1-d1p5.png", 1.0, 1.5}, {"outline-z2-d2.png", 2.0, 2.0}};
    for (const auto &c : cases) {
        SCOPED_TRACE(c.name);
        const QImage image = renderStrokes(strokes, area, c.zoom, c.dpr, Qt::white);
        EXPECT_EQ(image.size(), QSize(int(300 * c.zoom * c.dpr), int(200 * c.zoom * c.dpr)));
        EXPECT_DOUBLE_EQ(image.devicePixelRatio(), c.dpr);
        checkGolden(QString::fromLatin1(c.name), image, strokeRegions(strokes, area, c.zoom * c.dpr));
    }
}

namespace {

/// A straight pen line from (0,100) to (200,100), `n` samples at `hz` (a 200 Hz pen is faster than PADD's 120), rising pressure.
std::vector<InkSample> syntheticLine(int n, double hz = 200.0)
{
    std::vector<InkSample> out;
    for (int i = 0; i < n; ++i)
        out.push_back({QPointF(200.0 * i / (n - 1), 100.0), 0.3 + 0.6 * i / (n - 1), i / hz});
    return out;
}

} // namespace

TEST(InkInputTest, ModelledStrokeFollowsTheSamplesAndIsDenserThanTheInput)
{
    InkInput input;
    const auto samples = syntheticLine(40);
    input.begin(samples.front(), StrokeStyle{});
    for (size_t i = 1; i + 1 < samples.size(); ++i)
        input.extend(samples[i]);
    input.end(samples.back());
    const auto stroke = input.finish();
    ASSERT_TRUE(stroke);
    EXPECT_FALSE(input.active());
    EXPECT_GE(stroke->points.size(), samples.size()) << "min_output_rate 120 at 200 Hz input never drops below the input count";
    for (size_t i = 1; i < stroke->points.size(); ++i) {
        EXPECT_GE(stroke->points[i].x, stroke->points[i - 1].x - 1e-6) << "a straight line stays monotonic";
        EXPECT_NEAR(stroke->points[i].y, 100.0, 0.5);
        EXPECT_GE(stroke->points[i].pressure, 0.0);
        EXPECT_LE(stroke->points[i].pressure, 1.0);
        EXPECT_FALSE(std::isnan(stroke->points[i].x));
    }
    EXPECT_NEAR(stroke->points.back().x, 200.0, 0.5) << "the end-of-stroke iterations reach the pen-up position";
    EXPECT_NEAR(stroke->points.back().pressure, 0.9, 0.05);
}

TEST(InkInputTest, TheInputRateIsPaddsMeasured120Hz)
{
    EXPECT_DOUBLE_EQ(kInputRateHz, 120.0);
    EXPECT_EQ(kFractionalScaleOffset, QPointF(0.0, 0.0));
}

// R14: the spike saw 128 pairs of equal timestamps and none backwards at 120 Hz.
TEST(InkInputTest, APaddFeedAt120HzWithEqualTimestampPairsCoalescesWithoutDrops)
{
    InkInput input;
    const double dt = 1.0 / kInputRateHz;
    int pairs = 0;
    double t = 0, x = 0;
    input.begin({QPointF(0, 50), 0.4, 0.0}, StrokeStyle{});
    for (int i = 1; i <= 120; ++i) {
        t += dt;
        x += 1.5;
        input.extend({QPointF(x, 50), 0.4, t});
        if (i % 8 == 0) {
            input.extend({QPointF(x + 0.1, 50), 0.4, t}); // the compositor's equal-timestamp pair: the newer replaces the older
            ++pairs;
        }
    }
    input.end({QPointF(x + 1.5, 50), 0.4, t + dt});
    EXPECT_EQ(pairs, 15);
    EXPECT_EQ(input.droppedSamples(), 0) << "equal timestamps coalesce, they are not drops";
    const auto stroke = input.finish();
    ASSERT_TRUE(stroke);
    for (const StrokePoint &p : stroke->points) {
        EXPECT_FALSE(std::isnan(p.x));
        EXPECT_FALSE(std::isnan(p.y));
    }
    EXPECT_NEAR(stroke->points.back().x, x + 1.5, 1.0);
}

TEST(InkInputTest, NewPointCountingDrivesIncrementalRasterisation)
{
    InkInput input;
    const auto samples = syntheticLine(10);
    input.begin(samples[0], StrokeStyle{});
    EXPECT_EQ(input.takeNewPoints(), 1u) << "the down sample is a point";
    input.extend(samples[1]);
    input.extend(samples[2]);
    const size_t fresh = input.newPointsSinceLastTake();
    EXPECT_GT(fresh, 0u);
    EXPECT_EQ(input.takeNewPoints(), fresh);
    EXPECT_EQ(input.newPointsSinceLastTake(), 0u);
    EXPECT_EQ(input.stroke().points.size(), 1u + fresh);
}

TEST(InkInputTest, PredictionExtendsPastTheLastModelledPointAndClearsOnEnd)
{
    InkInput input;
    const auto samples = syntheticLine(20);
    input.begin(samples[0], StrokeStyle{});
    for (int i = 1; i < 10; ++i)
        input.extend(samples[i]);
    ASSERT_FALSE(input.prediction().empty());
    EXPECT_GT(input.prediction().back().x, input.stroke().points.back().x) << "the tail runs ahead along the line";
    input.end(samples[10]);
    EXPECT_TRUE(input.prediction().empty());
}

TEST(InkInputTest, NonMonotonicTimestampsAreSkippedAndCounted)
{
    InkInput input;
    auto samples = syntheticLine(10);
    input.begin(samples[0], StrokeStyle{});
    input.extend(samples[1]);
    InkSample back = samples[2];
    back.timeSeconds = samples[0].timeSeconds; // the clock went backwards
    input.extend(back);
    EXPECT_EQ(input.droppedSamples(), 1);
    input.extend(samples[3]);
    input.end(samples[4]);
    EXPECT_EQ(input.droppedSamples(), 1);
    EXPECT_TRUE(input.finish());
}

TEST(InkInputTest, PressureOutsideZeroOneIsClampedAndUnknownBecomesHalf)
{
    InkInput raw; // raw mode keeps every sample, so each pressure is visible
    raw.setRawInput(true);
    raw.begin({QPointF(0, 0), 7.0, 0.0}, StrokeStyle{});
    raw.extend({QPointF(10, 0), -1.0, 0.005});
    raw.end({QPointF(20, 0), 0.5, 0.010});
    const auto exact = raw.finish();
    ASSERT_TRUE(exact);
    ASSERT_EQ(exact->points.size(), 3u);
    EXPECT_DOUBLE_EQ(exact->points[0].pressure, 1.0) << "7 clamps to 1";
    EXPECT_DOUBLE_EQ(exact->points[1].pressure, 0.5) << "-1 is 'unknown': a mid-width line";
    EXPECT_DOUBLE_EQ(exact->points[2].pressure, 0.5);

    InkInput input; // through the modeler every output stays in range
    input.begin({QPointF(0, 0), 7.0, 0.0}, StrokeStyle{});
    input.extend({QPointF(10, 0), -1.0, 0.005});
    input.end({QPointF(20, 0), 0.5, 0.010});
    const auto stroke = input.finish();
    ASSERT_TRUE(stroke);
    ASSERT_FALSE(stroke->points.empty());
    for (const StrokePoint &p : stroke->points) {
        EXPECT_GE(p.pressure, 0.0);
        EXPECT_LE(p.pressure, 1.0);
    }
}

TEST(InkInputTest, SamplesSharingATimestampCoalesceTheNewerReplacesTheOlder)
{
    InkInput raw; // raw mode: every accepted sample is a point, so the replacement is visible
    raw.setRawInput(true);
    raw.begin({QPointF(0, 0), 0.5, 0.000}, StrokeStyle{});
    raw.extend({QPointF(10, 0), 0.5, 0.005});
    raw.extend({QPointF(11, 0), 0.5, 0.005}); // the same millisecond: replaces the sample above
    raw.extend({QPointF(20, 0), 0.5, 0.010});
    raw.end({QPointF(30, 0), 0.5, 0.015});
    EXPECT_EQ(raw.droppedSamples(), 0) << "coalescing is not a drop";
    const auto exact = raw.finish();
    ASSERT_TRUE(exact);
    ASSERT_EQ(exact->points.size(), 4u);
    EXPECT_DOUBLE_EQ(exact->points[1].x, 11.0);
    EXPECT_DOUBLE_EQ(exact->points[2].x, 20.0);
    EXPECT_DOUBLE_EQ(exact->points[3].x, 30.0);

    InkInput modelled; // through the modeler: finite, and it still ends at the pen-up
    modelled.begin({QPointF(0, 0), 0.5, 0.000}, StrokeStyle{});
    modelled.extend({QPointF(10, 0), 0.5, 0.005});
    modelled.extend({QPointF(11, 0), 0.5, 0.005});
    modelled.extend({QPointF(20, 0), 0.5, 0.010});
    modelled.end({QPointF(30, 0), 0.5, 0.015});
    const auto smoothed = modelled.finish();
    ASSERT_TRUE(smoothed);
    for (const StrokePoint &p : smoothed->points)
        EXPECT_FALSE(std::isnan(p.x));
    EXPECT_NEAR(smoothed->points.back().x, 30.0, 1.0);
}

// R14: PADD's pen peaks at 0.657 with a median of 0.21. A synthetic device with that range.
TEST(PressureNormaliserTest, BeforeCalibrationPressureIsUntouchedAndNothingIsReportedChanged)
{
    PressureNormaliser normaliser;
    EXPECT_DOUBLE_EQ(normaliser.learnedMax(), 1.0);
    EXPECT_DOUBLE_EQ(normaliser.normalise(0.5), 0.5);
    for (int i = 0; i < 1000; ++i)
        normaliser.normalise(0.1); // weak samples (below 0.3 of the maximum) never count towards calibration
    EXPECT_DOUBLE_EQ(normaliser.learnedMax(), 1.0);
    EXPECT_FALSE(normaliser.takeChanged());
}

TEST(PressureNormaliserTest, ADeviceThatTopsOutAt0657ReachesFullPressureOnceCalibrated)
{
    PressureNormaliser normaliser;
    for (int i = 0; i < PressureNormaliser::kCalibrationSamples + 100; ++i)
        normaliser.normalise(0.35 + 0.307 * (i % 100) / 99.0); // 0.35 .. 0.657, all above 0.3 of 1.0
    EXPECT_NEAR(normaliser.learnedMax(), 0.657, 1e-9);
    EXPECT_NEAR(normaliser.normalise(0.657), 1.0, 1e-9);
    EXPECT_NEAR(normaliser.normalise(0.3285), 0.5, 1e-9);
    EXPECT_TRUE(normaliser.takeChanged());
    EXPECT_FALSE(normaliser.takeChanged()) << "reported once";
}

TEST(PressureNormaliserTest, OneSampleShortOfTheCalibrationCountChangesNothing)
{
    PressureNormaliser normaliser;
    for (int i = 0; i < PressureNormaliser::kCalibrationSamples - 1; ++i)
        normaliser.normalise(0.657);
    EXPECT_DOUBLE_EQ(normaliser.learnedMax(), 1.0);
    normaliser.normalise(0.657);
    EXPECT_NEAR(normaliser.learnedMax(), 0.657, 1e-9);
}

TEST(PressureNormaliserTest, ALaterHarderPressRaisesTheMaximumAndNeverLowersIt)
{
    PressureNormaliser normaliser;
    for (int i = 0; i < PressureNormaliser::kCalibrationSamples; ++i)
        normaliser.normalise(0.657);
    ASSERT_TRUE(normaliser.takeChanged());
    EXPECT_DOUBLE_EQ(normaliser.normalise(0.7), 1.0);
    EXPECT_NEAR(normaliser.learnedMax(), 0.7, 1e-9);
    EXPECT_TRUE(normaliser.takeChanged());
    for (int i = 0; i < 1000; ++i)
        normaliser.normalise(0.35);
    EXPECT_NEAR(normaliser.learnedMax(), 0.7, 1e-9) << "light pressing never lowers it again";
    EXPECT_NEAR(normaliser.normalise(0.35), 0.5, 1e-9);
}

TEST(PressureNormaliserTest, AStoredMaximumAppliesFromTheFirstSample)
{
    PressureNormaliser normaliser;
    normaliser.setLearnedMax(0.657);
    EXPECT_NEAR(normaliser.normalise(0.3285), 0.5, 1e-9);
    EXPECT_FALSE(normaliser.takeChanged()) << "restoring a stored value is not a change to persist";
    PressureNormaliser untouched;
    untouched.setLearnedMax(1.0); // the default: not learned yet, so it still calibrates
    for (int i = 0; i < PressureNormaliser::kCalibrationSamples; ++i)
        untouched.normalise(0.5);
    EXPECT_NEAR(untouched.learnedMax(), 0.5, 1e-9);
}

TEST(PressureNormaliserTest, UnknownAndOutOfRangePressurePassThroughSafely)
{
    PressureNormaliser normaliser;
    EXPECT_DOUBLE_EQ(normaliser.normalise(-1.0), -1.0) << "unknown stays unknown: InkInput turns it into 0.5";
    EXPECT_TRUE(std::isnan(normaliser.normalise(std::nan(""))));
    EXPECT_DOUBLE_EQ(normaliser.normalise(2.0), 1.0);
    normaliser.setLearnedMax(0.0);
    EXPECT_NEAR(normaliser.learnedMax(), PressureNormaliser::kMinimumMax, 1e-12) << "a broken device cannot divide by ~0";
}

TEST(InkInputTest, ATapStaysWhereItLandedAndRawModeKeepsEverySample)
{
    InkInput tap;
    tap.begin({QPointF(5, 5), 0.8, 0.0}, StrokeStyle{});
    tap.end({QPointF(5, 5), 0.8, 0.004});
    const auto dot = tap.finish();
    ASSERT_TRUE(dot);
    ASSERT_GE(dot->points.size(), 1u);
    for (const StrokePoint &p : dot->points) {
        EXPECT_NEAR(p.x, 5.0, 0.05);
        EXPECT_NEAR(p.y, 5.0, 0.05);
    }

    InkInput raw;
    raw.setRawInput(true);
    const auto samples = syntheticLine(7);
    raw.begin(samples[0], StrokeStyle{});
    for (size_t i = 1; i + 1 < samples.size(); ++i)
        raw.extend(samples[i]);
    raw.end(samples.back());
    const auto stroke = raw.finish();
    ASSERT_EQ(stroke->points.size(), samples.size());
    for (size_t i = 0; i < samples.size(); ++i)
        EXPECT_DOUBLE_EQ(stroke->points[i].x, samples[i].position.x());
    EXPECT_TRUE(raw.prediction().empty());
}
// Input boundary: NaN or infinity must never reach the modeler (it may assert) or the stroke
// (the writer would turn it into null). A finite double too large for a float counts as non-finite.
TEST(InkInputTest, NonFiniteSamplesNeverReachTheModelerOrTheStroke)
{
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    for (const bool rawMode : {false, true}) {
        SCOPED_TRACE(rawMode ? "raw" : "modelled");
        InkInput input;
        input.setRawInput(rawMode);
        input.begin({QPointF(0, 0), 0.5, 0.000}, StrokeStyle{});
        input.extend({QPointF(nan, 0), 0.5, 0.005});
        input.extend({QPointF(5, inf), 0.5, 0.005});
        input.extend({QPointF(1e300, 0), 0.5, 0.005}); // finite as a double, infinite as a float
        input.extend({QPointF(5, 0), 0.5, nan});
        input.extend({QPointF(5, 0), 0.5, inf});
        input.extend({QPointF(10, 0), nan, 0.010});
        input.extend({QPointF(20, 0), inf, 0.015});
        input.end({QPointF(nan, nan), 0.5, 0.020}); // a bad pen-up still ends the stroke
        EXPECT_FALSE(input.active());
        EXPECT_EQ(input.droppedSamples(), 5 + 1);
        const auto stroke = input.finish();
        ASSERT_TRUE(stroke);
        ASSERT_FALSE(stroke->points.empty());
        for (const StrokePoint &p : stroke->points) {
            EXPECT_TRUE(std::isfinite(p.x));
            EXPECT_TRUE(std::isfinite(p.y));
            EXPECT_GE(p.pressure, 0.0);
            EXPECT_LE(p.pressure, 1.0);
        }
    }
    InkInput bad; // a pen-down that is not a number starts nothing
    bad.begin({QPointF(nan, 0), 0.5, 0.0}, StrokeStyle{});
    EXPECT_FALSE(bad.active());
    EXPECT_FALSE(bad.finish()) << "never a stroke without points";
    bad.extend({QPointF(1, 1), 0.5, 0.005});
    bad.end({QPointF(2, 2), 0.5, 0.010});
    EXPECT_FALSE(bad.finish());
}

// Carry (Task 4 re-review): a tap must become a stroke the reader accepts and the renderer draws.
TEST(InkInputTest, ATapIsAValidDotStrokeThatRoundTripsThroughTheFile)
{
    for (const bool rawMode : {false, true}) {
        SCOPED_TRACE(rawMode ? "raw" : "modelled");
        InkInput input;
        input.setRawInput(rawMode);
        input.begin({QPointF(40, 60), 0.7, 0.0}, StrokeStyle{});
        input.end({QPointF(40, 60), 0.7, 0.008});
        const auto dot = input.finish();
        ASSERT_TRUE(dot);
        ASSERT_GE(dot->points.size(), 1u);
        EXPECT_FALSE(input.finish()) << "a finished stroke is handed out once";

        InkDocument doc;
        doc.addStroke(dot);
        doc.commit();
        const QByteArray bytes = writeInk(doc);
        InkDocument back;
        const InkReadResult result = readInk(bytes, back);
        EXPECT_TRUE(result.ok) << result.error.toStdString();
        ASSERT_EQ(back.items().size(), 1u);
        ASSERT_TRUE(back.items()[0].stroke);
        EXPECT_EQ(back.items()[0].stroke->points.size(), dot->points.size());
        EXPECT_TRUE(strokeOutline(*back.items()[0].stroke).contains(QPointF(40, 60))) << "the dot is drawn where it was tapped";
    }
}

// R14 end to end: a pen that tops out at 0.657 draws full-pressure lines once calibrated.
TEST(InkInputTest, ACalibratedPaddPenReachesFullPressureThroughTheModeler)
{
    PressureNormaliser normaliser;
    normaliser.setLearnedMax(0.657);
    InkInput input;
    input.begin({QPointF(0, 0), normaliser.normalise(0.657), 0.0}, StrokeStyle{});
    for (int i = 1; i < 30; ++i)
        input.extend({QPointF(2.0 * i, 0), normaliser.normalise(0.657), i / kInputRateHz});
    input.end({QPointF(60, 0), normaliser.normalise(0.657), 30 / kInputRateHz});
    const auto stroke = input.finish();
    ASSERT_TRUE(stroke);
    EXPECT_NEAR(stroke->points.back().pressure, 1.0, 0.02);
}


int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
