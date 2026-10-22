// The ink core (docs/superpowers/specs/2026-10-21-ink-core-design.md section 7
// and 12): model and history, the .ink format, the outline composer, input,
// the canvas. Pure functions and widgets on the offscreen platform.
#include <gtest/gtest.h>

#include <cmath>
#include <limits>

#include <QApplication>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include <zlib.h>

#include "InkDocument.h"
#include "InkFormat.h"
#include "InkTypes.h"
#include "SpatialGrid.h"

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

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
