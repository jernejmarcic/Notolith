// The ink core (docs/superpowers/specs/2026-10-21-ink-core-design.md section 7
// and 12): model and history, the .ink format, the outline composer, input,
// the canvas. Pure functions and widgets on the offscreen platform.
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>

#include <QApplication>
#include <QDir>
#include <QImage>
#include <QPaintDevice>
#include <QPaintEngine>
#include <QPainter>
#include <QPainterPath>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QClipboard>
#include <QKeyEvent>
#include <QMimeData>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QElapsedTimer>
#include <QScrollArea>
#include <QSemaphore>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QThread>
#include <QThreadPool>
#include <QtTest/QTest>

#include <atomic>
#include <thread>

#include <zlib.h>

#include "InkCanvas.h"
#include "InkDocument.h"
#include "InkFormat.h"
#include "InkInput.h"
#include "InkTools.h"
#include "InkTestSupport.h"
#include "InkTypes.h"
#include "PagePatterns.h"
#include "SpatialGrid.h"
#include "StrokeOutline.h"
#include "TileCache.h"
#include "WaitFor.h"

using namespace noteAppC::ink;
using namespace inktest;

namespace {

std::shared_ptr<const Stroke> line(double x0, double y0, double x1, double y1, double width = 2.0)
{
    auto stroke = std::make_shared<Stroke>();
    stroke->style.width = width;
    stroke->points = {{x0, y0, 0.5}, {x1, y1, 0.5}};
    return stroke;
}

/// The canvas never renders in paintEvent (Task 11): a grab shows the page background until its tiles arrive from the
/// pool. This grabs until a grab asks for nothing (every tile it painted is fresh at the current zoom, nothing is in
/// flight: a request is only ever made during a paint, and finishes in the event loop) and returns that frame.
/// Stale tiles outside what is painted may remain: a cache keeps them until they are evicted.
QImage settledGrab(InkCanvas &canvas)
{
    const bool settled = QTest::qWaitFor(
        [&] {
            canvas.grab();
            return canvas.tilesScale() == canvas.zoom() && canvas.pendingTiles() == 0;
        },
        5000);
    if (!settled)
        ADD_FAILURE() << "the tiles never settled";
    return canvas.grab().toImage();
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

TEST(InkDocumentTest, DiscardLiveEditsRestoresTheCommitAndLeavesTheHistoryAlone)
{
    InkDocument doc;
    doc.addStroke(line(0, 0, 10, 10));
    doc.commit();
    doc.addStroke(line(0, 20, 10, 20));
    doc.commit();
    ASSERT_TRUE(doc.undo()); // one redo step in the stack
    ASSERT_TRUE(doc.canRedo());
    doc.addStroke(line(0, 30, 10, 30));
    doc.commit(); // clears redo; one undo step per commit
    doc.addStroke(line(0, 40, 10, 40)); // live: no commit closes it
    ASSERT_EQ(doc.items().size(), 3u);
    const qint64 first = doc.items().front().t;
    doc.removeItem(first); // a live removal too
    ASSERT_EQ(doc.items().size(), 2u);
    EXPECT_TRUE(doc.discardLiveEdits());
    ASSERT_EQ(doc.items().size(), 2u) << "the committed state: the removal is back, the live stroke is gone";
    EXPECT_EQ(doc.items().front().t, first);
    EXPECT_EQ(doc.itemsIntersecting(QRectF(-5, -5, 30, 60)).size(), 2u) << "the grid was rebuilt";
    EXPECT_FALSE(doc.discardLiveEdits()) << "clean now: nothing to discard";
    ASSERT_TRUE(doc.undo());
    EXPECT_EQ(doc.items().size(), 1u) << "undo steps back exactly one real commit";
    ASSERT_TRUE(doc.undo());
    EXPECT_TRUE(doc.items().empty());
    EXPECT_FALSE(doc.undo()) << "the stack held two steps, the discard added none";
    ASSERT_TRUE(doc.redo());
    ASSERT_TRUE(doc.redo());
    EXPECT_EQ(doc.items().size(), 2u);
    EXPECT_FALSE(doc.redo());
}

TEST(InkDocumentTest, DiscardLiveEditsKeepsARedoStackIntact)
{
    InkDocument doc;
    doc.addStroke(line(0, 0, 10, 10));
    doc.commit();
    doc.addStroke(line(0, 20, 10, 20));
    doc.commit();
    ASSERT_TRUE(doc.undo());
    doc.removeItem(doc.items().front().t); // live edit while a redo is pending
    EXPECT_TRUE(doc.discardLiveEdits());
    EXPECT_EQ(doc.items().size(), 1u);
    ASSERT_TRUE(doc.canRedo());
    ASSERT_TRUE(doc.redo());
    EXPECT_EQ(doc.items().size(), 2u);
}

TEST(InkDocumentTest, DiscardLiveEditsOnACleanDocumentChangesNothing)
{
    InkDocument doc;
    doc.addStroke(line(0, 0, 10, 10));
    doc.commit();
    doc.markSaved();
    const quint64 revision = doc.revision();
    EXPECT_FALSE(doc.discardLiveEdits());
    EXPECT_EQ(doc.revision(), revision);
    EXPECT_FALSE(doc.isModified());
    EXPECT_EQ(doc.items().size(), 1u);
}

TEST(InkDocumentTest, DiscardLiveEditsBackToTheSavedCommitIsNotModified)
{
    InkDocument doc;
    doc.addStroke(line(0, 0, 10, 10));
    doc.commit();
    doc.markSaved();
    doc.removeItem(doc.items().front().t);
    ASSERT_TRUE(doc.isModified());
    EXPECT_TRUE(doc.discardLiveEdits());
    EXPECT_FALSE(doc.isModified()) << "the file holds exactly this state";
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
    doc.reset(settings, {}, {}); // setSettings refuses this; the loop guard is for whatever else reaches the document
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
    EXPECT_EQ(kMaxInflatedBytes, qint64(64) << 20) << "a heavy note is about 2 MB; the parse peak is about 10x the file, so 64 MiB keeps it near 640 MB";
    EXPECT_GT(kMaxInflatedBytes, qint64(16) << 20) << "and still leaves room for a heavy note";
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

// Task 8: the cap guards the bytes the parser sees, plain JSON included, not only what gzip inflates to
// (readers other than loadInkFile hand readInk bytes without a size check of their own).
TEST(InkFormatTest, PlainJSONIsCappedLikeGzip)
{
    const QByteArray plain = writeInk(sampleDocument());
    InkDocument doc;
    EXPECT_TRUE(readInk(plain, doc, QStringLiteral("big.ink"), plain.size()).ok) << "exactly the cap is fine";
    doc = sampleDocument();
    const InkReadResult read = readInk(plain, doc, QStringLiteral("big.ink"), plain.size() - 1);
    EXPECT_FALSE(read.ok) << "one byte over the cap is refused before the parser sees it";
    EXPECT_TRUE(read.error.contains(QStringLiteral("big.ink")) && read.error.contains(QStringLiteral("too large"))) << read.error.toStdString();
    EXPECT_EQ(writeInk(doc), plain) << "a refused file leaves the document as it was";
}

TEST(InkDocumentTest, SetSettingsRefusesSizesTheReaderWouldRefuse)
{
    InkDocument doc;
    const DocumentSettings good = doc.settings();
    const quint64 revision = doc.revision();
    DocumentSettings next = good;
    next.format.width = 400;
    EXPECT_TRUE(doc.setSettings(next));
    EXPECT_NE(doc.revision(), revision);
    EXPECT_EQ(doc.settings().format.width, 400);
    const quint64 kept = doc.revision();
    const auto refused = [&](const char *what, const std::function<void(DocumentSettings &)> &spoil) {
        DocumentSettings bad = doc.settings();
        spoil(bad);
        EXPECT_FALSE(doc.setSettings(bad)) << what;
        EXPECT_EQ(doc.revision(), kept) << what << ": a refused change touches nothing";
        EXPECT_EQ(doc.settings().format.width, 400) << what;
    };
    refused("zero page width", [](DocumentSettings &s) { s.format.width = 0; });
    refused("negative page height", [](DocumentSettings &s) { s.format.height = -1; });
    refused("zero dpi", [](DocumentSettings &s) { s.format.dpi = 0; });
    refused("empty extent width", [](DocumentSettings &s) { s.extent.setWidth(0); });
    refused("empty extent height", [](DocumentSettings &s) { s.extent.setHeight(0); });
    refused("pattern size below 1e-3", [](DocumentSettings &s) { s.background.patternSize = QSizeF(0.0005, 32); });
    refused("NaN page width", [](DocumentSettings &s) { s.format.width = std::nan(""); });
    DocumentSettings edge = doc.settings();
    edge.background.patternSize = QSizeF(1e-3, 1e-3);
    EXPECT_TRUE(doc.setSettings(edge)) << "1e-3 is the smallest usable pattern size";
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


// Fix round 1, Important 1: the modeler refuses an input gap of about 1.67 s (max_outputs_per_call
// 200 at 120 outputs/s) and used to stay failing for the rest of the stroke. A pen resting still
// on the page for any time must not end the smoothing.
TEST(InkInputTest, SmoothingContinuesAfterAPauseInContact)
{
    for (const double pause : {1.7, 3.0, 60.0}) {
        SCOPED_TRACE(pause);
        InkInput input;
        const double dt = 1.0 / kInputRateHz;
        double t = 0;
        input.begin({QPointF(0, 50), 0.5, t}, StrokeStyle{});
        for (int i = 1; i <= 100; ++i) {
            t += dt + (i == 50 ? pause : 0.0);
            input.extend({QPointF(2.0 * i, 50), 0.5, t});
        }
        EXPECT_EQ(input.droppedSamples(), 0) << "no sample falls through unsmoothed";
        ASSERT_FALSE(input.prediction().empty()) << "the tail is fresh, not left over from before the pause";
        EXPECT_GT(input.prediction().back().x, 195.0);
        const size_t beforeEnd = input.stroke().points.size();
        input.end({QPointF(202, 50), 0.5, t + dt});
        EXPECT_EQ(input.droppedSamples(), 0);
        const auto stroke = input.finish();
        ASSERT_TRUE(stroke);
        EXPECT_GT(stroke->points.size(), beforeEnd);
        EXPECT_NEAR(stroke->points.back().x, 202.0, 1.0);
        for (size_t i = 1; i < stroke->points.size(); ++i) {
            EXPECT_TRUE(std::isfinite(stroke->points[i].x));
            EXPECT_GE(stroke->points[i].x, stroke->points[i - 1].x - 0.1) << "the spring settles at the pause, at most a hair back";
            EXPECT_LT(stroke->points[i].x - stroke->points[i - 1].x, 8.0) << "no jump across the pause at point " << i;
        }
    }
}

// Fix round 1, Important 2: a stray or stored out-of-range number must not ruin a device for good.
TEST(PressureNormaliserTest, OutOfRangeAndNonFiniteSamplesNeverPoisonTheLearnedMaximum)
{
    const double inf = std::numeric_limits<double>::infinity();
    PressureNormaliser normaliser;
    normaliser.normalise(2.0);
    normaliser.normalise(inf);
    normaliser.normalise(-inf);
    for (int i = 0; i < PressureNormaliser::kCalibrationSamples; ++i)
        normaliser.normalise(0.657);
    EXPECT_NEAR(normaliser.learnedMax(), 0.657, 1e-9) << "the 2.0 and the inf were clamped or ignored";
    EXPECT_NEAR(normaliser.normalise(0.657), 1.0, 1e-9);
    EXPECT_TRUE(std::isfinite(normaliser.learnedMax()));
}

TEST(PressureNormaliserTest, ABadStoredMaximumIsClampedOrRejectedAndTheDeviceStillCalibrates)
{
    const double bad[] = {1e9, std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN(), -3.0};
    for (const double stored : bad) {
        SCOPED_TRACE(stored);
        PressureNormaliser normaliser;
        normaliser.setLearnedMax(stored);
        EXPECT_TRUE(std::isfinite(normaliser.learnedMax()));
        EXPECT_GE(normaliser.learnedMax(), PressureNormaliser::kMinimumMax);
        EXPECT_LE(normaliser.learnedMax(), 1.0);
        EXPECT_FALSE(normaliser.takeChanged());
        for (int i = 0; i < PressureNormaliser::kCalibrationSamples + 5; ++i)
            normaliser.normalise(0.657);
        EXPECT_NEAR(normaliser.normalise(0.657), 1.0, 1e-6);
    }
}

// Fix round 1, Important 1 (second half): whatever the modeler refuses - here a coordinate so large the
// spring state overflows - it is restarted from that sample, never left failing for the rest of the stroke.
TEST(InkInputTest, AModelerFailureRestartsTheModelerAndTheStrokeStaysSmoothed)
{
    InkInput input;
    const double dt = 1.0 / kInputRateHz;
    input.begin({QPointF(0, 50), 0.5, 0.0}, StrokeStyle{});
    for (int i = 1; i <= 60; ++i)
        input.extend({QPointF(i == 30 ? 3.0e38 : 2.0 * i, 50), 0.5, i * dt});
    EXPECT_GE(input.modelerFailures(), 1) << "the enormous coordinate was refused";
    EXPECT_EQ(input.droppedSamples(), 0);
    ASSERT_FALSE(input.prediction().empty()) << "a fresh tail after the restart";
    for (const StrokePoint &p : input.prediction())
        EXPECT_TRUE(std::isfinite(p.x) && std::isfinite(p.y));
    const int failuresAfter = input.modelerFailures();
    for (int i = 61; i <= 100; ++i)
        input.extend({QPointF(2.0 * i, 50), 0.5, i * dt});
    input.end({QPointF(202, 50), 0.5, 101 * dt});
    EXPECT_EQ(input.modelerFailures(), failuresAfter) << "once restarted it takes samples again";
    const auto stroke = input.finish();
    ASSERT_TRUE(stroke);
    EXPECT_GT(stroke->points.size(), 130u) << "smoothed output is denser than one point per sample";
    EXPECT_NEAR(stroke->points.back().x, 202.0, 1.0);
    for (const StrokePoint &p : stroke->points)
        EXPECT_TRUE(std::isfinite(p.x) && std::isfinite(p.y));
}

namespace {

struct CanvasFixture {
    InkCanvas canvas;
    CanvasFixture()
    {
        DocumentSettings settings;
        settings.format = {400, 300, 96};
        settings.extent = QRectF(0, 0, 400, 300);
        settings.layout = LayoutKind::Fixed;
        canvas.document().setSettings(settings);
        canvas.document().markSaved();
        canvas.documentReplaced();
        canvas.resize(400, 300);
        canvas.show();
        if (!QTest::qWaitForWindowExposed(&canvas))
            ADD_FAILURE() << "the canvas was never exposed";
    }
};

} // namespace

TEST(InkCanvasTest, APenStrokeBecomesOneCommittedItemWithTheCurrentStyle)
{
    CanvasFixture f;
    StrokeStyle style;
    style.width = 3.0;
    style.color = QColor(0x00, 0x50, 0xc0);
    f.canvas.setPenStyle(style);
    QSignalSpy edited(&f.canvas, &InkCanvas::documentEdited);
    penStroke(&f.canvas, QPointF(20, 20), QPointF(120, 60), 1000);
    ASSERT_EQ(f.canvas.document().items().size(), 1u);
    const Item &item = f.canvas.document().items()[0];
    EXPECT_EQ(item.type, Item::Type::Stroke);
    EXPECT_EQ(item.stroke->style, style);
    EXPECT_GE(item.stroke->points.size(), 10u);
    EXPECT_NEAR(item.stroke->points.front().x, 20.0, 1.0);
    EXPECT_NEAR(item.stroke->points.back().x, 120.0, 1.0);
    EXPECT_TRUE(f.canvas.document().canUndo());
    EXPECT_TRUE(f.canvas.document().isModified());
    EXPECT_EQ(edited.count(), 1);
    EXPECT_FALSE(f.canvas.isDrawing());
}

TEST(InkCanvasTest, ZoomMapsWidgetToDocumentCoordinates)
{
    CanvasFixture f;
    f.canvas.setZoom(2.0);
    EXPECT_EQ(f.canvas.sizeHint(), QSize(800, 600));
    EXPECT_EQ(f.canvas.toDocument(QPointF(100, 50)), QPointF(50, 25));
    EXPECT_EQ(f.canvas.toWidget(QPointF(50, 25)), QPointF(100, 50));
    f.canvas.resize(f.canvas.sizeHint());
    penStroke(&f.canvas, QPointF(100, 100), QPointF(300, 100), 1000);
    ASSERT_EQ(f.canvas.document().items().size(), 1u);
    EXPECT_NEAR(f.canvas.document().items()[0].stroke->points.front().x, 50.0, 1.0) << "stored in document px";
}

TEST(InkCanvasTest, TheEraserRemovesWholeStrokesItTouches)
{
    CanvasFixture f;
    penStroke(&f.canvas, QPointF(20, 50), QPointF(200, 50), 1000);
    penStroke(&f.canvas, QPointF(20, 200), QPointF(200, 200), 2000);
    ASSERT_EQ(f.canvas.document().items().size(), 2u);
    f.canvas.setTool(InkCanvas::Tool::Eraser);
    penStroke(&f.canvas, QPointF(100, 30), QPointF(100, 70), 3000); // crosses the first stroke only
    ASSERT_EQ(f.canvas.document().items().size(), 1u);
    EXPECT_NEAR(f.canvas.document().items()[0].stroke->points.front().y, 200.0, 1.0);
    f.canvas.setTool(InkCanvas::Tool::Pen);
    // The pen's eraser end erases while the pen tool is selected (kTrustEraserPointerType stays
    // true: PADD has no eraser end, other pens do). The plain tip with the Eraser tool, above,
    // is the way in that needs no hardware; this branch only keeps the test honest if the
    // constant is ever flipped.
    if (kTrustEraserPointerType) {
        penStroke(&f.canvas, QPointF(100, 180), QPointF(100, 220), 4000, Qt::LeftButton, QPointingDevice::PointerType::Eraser);
    } else {
        f.canvas.setTool(InkCanvas::Tool::Eraser);
        penStroke(&f.canvas, QPointF(100, 180), QPointF(100, 220), 4000);
        f.canvas.setTool(InkCanvas::Tool::Pen);
    }
    EXPECT_EQ(f.canvas.document().items().size(), 0u);
    // A held barrel button erases too.
    penStroke(&f.canvas, QPointF(20, 100), QPointF(200, 100), 5000);
    penStroke(&f.canvas, QPointF(100, 80), QPointF(100, 120), 6000, Qt::LeftButton | kHeldEraserButton);
    EXPECT_EQ(f.canvas.document().items().size(), 0u);
}

TEST(InkCanvasTest, UndoRedoFromTheKeyboard)
{
    CanvasFixture f;
    penStroke(&f.canvas, QPointF(20, 50), QPointF(200, 50), 1000);
    penStroke(&f.canvas, QPointF(20, 100), QPointF(200, 100), 2000);
    QTest::keyClick(&f.canvas, Qt::Key_Z, Qt::ControlModifier);
    EXPECT_EQ(f.canvas.document().items().size(), 1u);
    QTest::keyClick(&f.canvas, Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier);
    EXPECT_EQ(f.canvas.document().items().size(), 2u);
    QTest::keyClick(&f.canvas, Qt::Key_Z, Qt::ControlModifier);
    QTest::keyClick(&f.canvas, Qt::Key_Y, Qt::ControlModifier);
    EXPECT_EQ(f.canvas.document().items().size(), 2u);
}

TEST(InkCanvasTest, EraseEverythingAutosaveThenUndoBringsTheStrokesBack)
{
    // Review Focus item 5.
    CanvasFixture f;
    QTemporaryDir tmp;
    penStroke(&f.canvas, QPointF(20, 50), QPointF(200, 50), 1000);
    penStroke(&f.canvas, QPointF(20, 100), QPointF(200, 100), 2000);
    f.canvas.setTool(InkCanvas::Tool::Eraser);
    penStroke(&f.canvas, QPointF(100, 30), QPointF(100, 120), 3000); // crosses both
    ASSERT_EQ(f.canvas.document().items().size(), 0u);
    ASSERT_TRUE(saveInkFile(tmp.filePath(QStringLiteral("n.ink")), f.canvas.document()));
    f.canvas.document().markSaved(); // what InkView::save does
    EXPECT_FALSE(f.canvas.document().isModified());
    ASSERT_TRUE(f.canvas.undo());
    EXPECT_EQ(f.canvas.document().items().size(), 2u);
    EXPECT_EQ(f.canvas.document().itemsIntersecting(QRectF(0, 0, 400, 300)).size(), 2u) << "the spatial grid came back";
    EXPECT_TRUE(f.canvas.document().isModified()) << "the file holds the erased state";
    const QImage frame = settledGrab(f.canvas);
    EXPECT_NE(frame.pixelColor(100, 50), QColor(Qt::white)) << "the canvas repainted the strokes";
}

TEST(InkCanvasTest, MouseDrawsOnlyWithDrawWithMouseOn)
{
    CanvasFixture f;
    QTest::mousePress(&f.canvas, Qt::LeftButton, Qt::NoModifier, QPoint(20, 20));
    QTest::mouseMove(&f.canvas, QPoint(100, 60));
    QTest::mouseRelease(&f.canvas, Qt::LeftButton, Qt::NoModifier, QPoint(100, 60));
    EXPECT_TRUE(f.canvas.document().items().empty());
    f.canvas.setDrawWithMouse(true);
    QTest::mousePress(&f.canvas, Qt::LeftButton, Qt::NoModifier, QPoint(20, 20));
    QTest::mouseMove(&f.canvas, QPoint(60, 40));
    QTest::mouseMove(&f.canvas, QPoint(100, 60));
    QTest::mouseRelease(&f.canvas, Qt::LeftButton, Qt::NoModifier, QPoint(100, 60));
    ASSERT_EQ(f.canvas.document().items().size(), 1u);
    for (const StrokePoint &p : f.canvas.document().items()[0].stroke->points)
        EXPECT_DOUBLE_EQ(p.pressure, 0.5) << "a mouse has no pressure";
}

TEST(InkCanvasTest, TouchPansAndNeverDraws)
{
    CanvasFixture f;
    QSignalSpy pans(&f.canvas, &InkCanvas::panRequested);
    QTest::touchEvent(&f.canvas, finger()).press(0, QPoint(100, 100), &f.canvas);
    QTest::touchEvent(&f.canvas, finger()).move(0, QPoint(130, 90), &f.canvas);
    QTest::touchEvent(&f.canvas, finger()).release(0, QPoint(130, 90), &f.canvas);
    EXPECT_TRUE(f.canvas.document().items().empty());
    ASSERT_GE(pans.count(), 1);
    EXPECT_EQ(pans.last().at(0).toPoint(), QPoint(30, -10));
}

TEST(InkCanvasTest, TouchPanFollowsTheFingerWhileTheScrollAreaMovesTheCanvas)
{
    // The scroll area moves the canvas under the finger. Widget-local positions
    // would then report no movement and the pan would oscillate; global ones do not.
    CanvasFixture f;
    QSignalSpy pans(&f.canvas, &InkCanvas::panRequested);
    const QPoint origin = f.canvas.mapToGlobal(QPoint(0, 0));
    const auto local = [&](QPoint global) { return f.canvas.mapFromGlobal(global); };
    QTest::touchEvent(&f.canvas, finger()).press(0, local(origin + QPoint(100, 100)), &f.canvas);
    QTest::touchEvent(&f.canvas, finger()).move(0, local(origin + QPoint(130, 90)), &f.canvas);
    ASSERT_GE(pans.count(), 1);
    EXPECT_EQ(pans.last().at(0).toPoint(), QPoint(30, -10));
    f.canvas.move(f.canvas.pos() + QPoint(30, -10)); // what the scroll area does: the content follows the finger
    QTest::touchEvent(&f.canvas, finger()).move(0, local(origin + QPoint(160, 80)), &f.canvas);
    EXPECT_EQ(pans.last().at(0).toPoint(), QPoint(30, -10)) << "the finger moved 30 px on screen again";
    QTest::touchEvent(&f.canvas, finger()).release(0, local(origin + QPoint(160, 80)), &f.canvas);
    EXPECT_TRUE(f.canvas.document().items().empty());
}

// R14: the PADD pen never reports above 0.657. After the calibration samples the strokes reach full pressure.
TEST(InkCanvasTest, PressureIsNormalisedByTheLearnedMaximumOfTheDevice)
{
    CanvasFixture f;
    QSignalSpy learned(&f.canvas, &InkCanvas::learnedPressureChanged);
    for (int i = 0; i < 25; ++i) // 10 samples a stroke: the 200 calibration samples are done in the 20th
        penStroke(&f.canvas, QPointF(20, 10 + 10 * i), QPointF(200, 10 + 10 * i), 1000 + i * 100, Qt::LeftButton,
                  QPointingDevice::PointerType::Pen, 0.657);
    ASSERT_EQ(f.canvas.document().items().size(), 25u);
    EXPECT_NEAR(f.canvas.document().items().front().stroke->points.front().pressure, 0.657, 0.05) << "before calibration: raw";
    EXPECT_NEAR(f.canvas.document().items().back().stroke->points.front().pressure, 1.0, 0.05) << "after: full pressure";
    ASSERT_GE(learned.count(), 1);
    EXPECT_EQ(learned.last().at(0).toString(), QStringLiteral("test pen"));
    EXPECT_NEAR(learned.last().at(1).toDouble(), 0.657, 1e-9);
    EXPECT_NEAR(f.canvas.learnedPressure(QStringLiteral("test pen")), 0.657, 1e-9);
    EXPECT_DOUBLE_EQ(f.canvas.learnedPressure(QStringLiteral("some other pen")), 1.0);
}

TEST(InkCanvasTest, AStoredMaximumOrTheSeedAppliesFromTheFirstStroke)
{
    {
        CanvasFixture f;
        f.canvas.setLearnedPressure(QStringLiteral("test pen"), 0.657);
        penStroke(&f.canvas, QPointF(20, 20), QPointF(120, 20), 1000, Qt::LeftButton, QPointingDevice::PointerType::Pen, 0.3285);
        ASSERT_EQ(f.canvas.document().items().size(), 1u);
        EXPECT_NEAR(f.canvas.document().items()[0].stroke->points.front().pressure, 0.5, 0.05);
    }
    CanvasFixture g; // the seed (ink.pressureMax) is for a device nothing is stored for
    g.canvas.setPressureSeed(0.657);
    penStroke(&g.canvas, QPointF(20, 20), QPointF(120, 20), 1000, Qt::LeftButton, QPointingDevice::PointerType::Pen, 0.3285);
    ASSERT_EQ(g.canvas.document().items().size(), 1u);
    EXPECT_NEAR(g.canvas.document().items()[0].stroke->points.front().pressure, 0.5, 0.05);
    g.canvas.setDrawWithMouse(true); // a mouse has no pressure and is not scaled
    QTest::mousePress(&g.canvas, Qt::LeftButton, Qt::NoModifier, QPoint(20, 80));
    QTest::mouseMove(&g.canvas, QPoint(60, 80));
    QTest::mouseRelease(&g.canvas, Qt::LeftButton, Qt::NoModifier, QPoint(100, 80));
    ASSERT_EQ(g.canvas.document().items().size(), 2u);
    for (const StrokePoint &p : g.canvas.document().items()[1].stroke->points)
        EXPECT_DOUBLE_EQ(p.pressure, 0.5);
}

// R14: PADD's pen has no eraser end and no working buttons, so nothing may need them.
TEST(InkCanvasTest, EveryToolIsReachableFromTheKeyboardAndATipAloneDrawsAndErases)
{
    CanvasFixture f;
    QSignalSpy changed(&f.canvas, &InkCanvas::toolChanged);
    const std::pair<int, InkCanvas::Tool> keys[] = {{Qt::Key_E, InkCanvas::Tool::Eraser}, {Qt::Key_L, InkCanvas::Tool::Lasso},
                                                    {Qt::Key_S, InkCanvas::Tool::Shape}, {Qt::Key_T, InkCanvas::Tool::Text},
                                                    {Qt::Key_P, InkCanvas::Tool::Pen}};
    for (const auto &[key, tool] : keys) {
        QTest::keyClick(&f.canvas, Qt::Key(key));
        EXPECT_EQ(f.canvas.tool(), tool);
    }
    EXPECT_EQ(changed.count(), 5);
    QTest::keyClick(&f.canvas, Qt::Key_P); // already the pen
    EXPECT_EQ(changed.count(), 5) << "no change, no signal";
    penStroke(&f.canvas, QPointF(20, 50), QPointF(200, 50), 1000); // the tip alone: no button, no eraser end
    ASSERT_EQ(f.canvas.document().items().size(), 1u);
    QTest::keyClick(&f.canvas, Qt::Key_E);
    penStroke(&f.canvas, QPointF(100, 30), QPointF(100, 70), 2000);
    EXPECT_EQ(f.canvas.document().items().size(), 0u);
    f.canvas.setReadOnly(true);
    QTest::keyClick(&f.canvas, Qt::Key_P);
    EXPECT_EQ(f.canvas.tool(), InkCanvas::Tool::Eraser) << "a read-only canvas ignores the tool keys";
}

TEST(InkCanvasTest, LiveStrokeRepaintsOnlyTheNewSegment)
{
    CanvasFixture f;
    settledGrab(f.canvas); // a tile arriving repaints its own 512 px: not what this is about
    PaintRects paints(&f.canvas);
    tablet(&f.canvas, QEvent::TabletPress, QPointF(20, 20), 0.5, 1000);
    tablet(&f.canvas, QEvent::TabletMove, QPointF(40, 20), 0.5, 1005);
    tablet(&f.canvas, QEvent::TabletMove, QPointF(50, 20), 0.5, 1010);
    QCoreApplication::processEvents(); // the first paints (the first is the whole canvas)
    paints.rects.clear();
    tablet(&f.canvas, QEvent::TabletMove, QPointF(60, 20), 0.5, 1015);
    QCoreApplication::processEvents();
    EXPECT_TRUE(f.canvas.isDrawing());
    EXPECT_TRUE(f.canvas.document().items().empty()) << "nothing is committed until pen-up";
    ASSERT_FALSE(paints.rects.isEmpty());
    EXPECT_LT(paints.united().width(), f.canvas.width() / 2) << "a 10 px segment never repaints the canvas";
    EXPECT_LT(paints.united().height(), f.canvas.height() / 2);
    // The real invariant lives in the bench (Track S1 Task 18): a 200 Hz feed costs under 1 ms per sample.
}

TEST(InkCanvasTest, PenUpRepaintsWhereThePredictedTailWas)
{
    // The tail is painted straight onto the widget; pen-up must repaint its
    // area or it stays on screen as a ghost. (T6's PredictionExtends... test
    // pins that the tail exists for a steady line; with no tail this is vacuous.)
    CanvasFixture f;
    settledGrab(f.canvas); // a tile arriving repaints its own 512 px, which would pass for the tail
    PaintRects paints(&f.canvas);
    tablet(&f.canvas, QEvent::TabletPress, QPointF(20, 100), 0.5, 1000);
    for (int i = 1; i <= 8; ++i)
        tablet(&f.canvas, QEvent::TabletMove, QPointF(20 + 10 * i, 100), 0.5, 1000 + 5 * i);
    QCoreApplication::processEvents();
    paints.rects.clear();
    tablet(&f.canvas, QEvent::TabletMove, QPointF(110, 100), 0.5, 1045);
    QCoreApplication::processEvents();
    ASSERT_FALSE(paints.rects.isEmpty());
    const int tailRight = paints.united().right();
    paints.rects.clear();
    tablet(&f.canvas, QEvent::TabletRelease, QPointF(110, 100), 0.5, 1050, Qt::NoButton);
    QCoreApplication::processEvents();
    ASSERT_FALSE(paints.rects.isEmpty());
    EXPECT_GE(paints.united().right(), tailRight) << "pen-up repaints out to where the tail was";
}

TEST(InkCanvasTest, TabletTrackingIsOnSoHoverReachesTheCanvas)
{
    InkCanvas canvas;
    EXPECT_TRUE(canvas.hasTabletTracking());
}

TEST(InkCanvasTest, AReadOnlyCanvasIgnoresThePenAndTheKeysButStillPans)
{
    CanvasFixture f;
    penStroke(&f.canvas, QPointF(20, 50), QPointF(200, 50), 1000);
    ASSERT_EQ(f.canvas.document().items().size(), 1u);
    f.canvas.setReadOnly(true);
    EXPECT_TRUE(f.canvas.isEnabled()) << "disabling the widget would swallow wheel and touch too";
    penStroke(&f.canvas, QPointF(20, 100), QPointF(200, 100), 2000);
    EXPECT_EQ(f.canvas.document().items().size(), 1u);
    QTest::keyClick(&f.canvas, Qt::Key_Z, Qt::ControlModifier);
    EXPECT_EQ(f.canvas.document().items().size(), 1u) << "undo is an edit";
    EXPECT_FALSE(f.canvas.undo());
    QSignalSpy pans(&f.canvas, &InkCanvas::panRequested);
    QTest::touchEvent(&f.canvas, finger()).press(0, QPoint(100, 100), &f.canvas);
    QTest::touchEvent(&f.canvas, finger()).move(0, QPoint(130, 90), &f.canvas);
    QTest::touchEvent(&f.canvas, finger()).release(0, QPoint(130, 90), &f.canvas);
    EXPECT_GE(pans.count(), 1);
}

// R14 (ruling): a pen that reports no buttons and pointer type Pen still erases once the tool is switched from outside.
TEST(InkCanvasTest, SwitchingTheToolWithSetToolMakesTheNextButtonlessPenStrokeErase)
{
    CanvasFixture f;
    penStroke(&f.canvas, QPointF(20, 50), QPointF(200, 50), 1000, Qt::LeftButton, QPointingDevice::PointerType::Pen);
    ASSERT_EQ(f.canvas.document().items().size(), 1u);
    f.canvas.setTool(InkCanvas::Tool::Eraser); // what the toolbar does (Task 16)
    penStroke(&f.canvas, QPointF(100, 30), QPointF(100, 70), 2000, Qt::NoButton, QPointingDevice::PointerType::Pen);
    EXPECT_TRUE(f.canvas.document().items().empty());
    f.canvas.setTool(InkCanvas::Tool::Pen);
    penStroke(&f.canvas, QPointF(20, 50), QPointF(200, 50), 3000, Qt::NoButton, QPointingDevice::PointerType::Pen);
    EXPECT_EQ(f.canvas.document().items().size(), 1u) << "and back: the tip draws again";
}

TEST(InkCanvasTest, UndoAndRedoWaitForThePenToLiftSoNoLiveStrokeIsOrphaned)
{
    CanvasFixture f;
    penStroke(&f.canvas, QPointF(20, 50), QPointF(200, 50), 1000);
    tablet(&f.canvas, QEvent::TabletPress, QPointF(20, 100), 0.5, 2000);
    tablet(&f.canvas, QEvent::TabletMove, QPointF(60, 100), 0.5, 2005);
    EXPECT_FALSE(f.canvas.undo());
    QTest::keyClick(&f.canvas, Qt::Key_Z, Qt::ControlModifier);
    EXPECT_TRUE(f.canvas.isDrawing());
    EXPECT_EQ(f.canvas.document().items().size(), 1u);
    tablet(&f.canvas, QEvent::TabletRelease, QPointF(100, 100), 0.5, 2010, Qt::NoButton);
    EXPECT_EQ(f.canvas.document().items().size(), 2u) << "the stroke in flight survived the undo attempt";
    EXPECT_TRUE(f.canvas.undo());
    EXPECT_EQ(f.canvas.document().items().size(), 1u);
}

// --- fix round 1: interruptions, eraser sweep, release pressure, overlay size ---

namespace {

/// A pen down and moving, no release yet.
void penDown(InkCanvas &canvas, QPointF a, QPointF b, qint64 ms)
{
    tablet(&canvas, QEvent::TabletPress, a, 0.5, ms);
    for (int i = 1; i <= 4; ++i)
        tablet(&canvas, QEvent::TabletMove, a + (b - a) * (i / 4.0), 0.5, ms + i * 5);
}

/// What a canvas must do after an interruption: no stroke in flight, mouse inking and undo alive.
void expectIdleAndAlive(InkCanvas &canvas)
{
    EXPECT_FALSE(canvas.isDrawing());
    const size_t before = canvas.document().items().size();
    canvas.setDrawWithMouse(true);
    QTest::mousePress(&canvas, Qt::LeftButton, Qt::NoModifier, QPoint(20, 250));
    QTest::mouseMove(&canvas, QPoint(60, 250));
    QTest::mouseRelease(&canvas, Qt::LeftButton, Qt::NoModifier, QPoint(100, 250));
    EXPECT_EQ(canvas.document().items().size(), before + 1) << "the mouse is not swallowed by a stale tablet state";
    EXPECT_TRUE(canvas.undo());
    EXPECT_EQ(canvas.document().items().size(), before);
    canvas.setDrawWithMouse(false);
}

} // namespace

TEST(InkCanvasTest, HidingTheCanvasMidStrokeEndsAndKeepsTheStroke)
{
    CanvasFixture f;
    penDown(f.canvas, QPointF(20, 50), QPointF(120, 50), 1000);
    ASSERT_TRUE(f.canvas.isDrawing());
    f.canvas.hide();
    EXPECT_FALSE(f.canvas.isDrawing());
    ASSERT_EQ(f.canvas.document().items().size(), 1u) << "what the user drew stays";
    EXPECT_NEAR(f.canvas.document().items()[0].stroke->points.back().x, 120.0, 1.0) << "ended at the last received sample";
    f.canvas.show();
    expectIdleAndAlive(f.canvas);
    EXPECT_EQ(f.canvas.document().items().size(), 1u);
}

TEST(InkCanvasTest, LosingFocusMidStrokeEndsAndKeepsTheStrokeButAPopupDoesNot)
{
    CanvasFixture f;
    penDown(f.canvas, QPointF(20, 50), QPointF(120, 50), 1000);
    QFocusEvent popup(QEvent::FocusOut, Qt::PopupFocusReason);
    QApplication::sendEvent(&f.canvas, &popup);
    EXPECT_TRUE(f.canvas.isDrawing()) << "a popup taking focus is not a lost pen";
    QFocusEvent out(QEvent::FocusOut, Qt::ActiveWindowFocusReason);
    QApplication::sendEvent(&f.canvas, &out);
    EXPECT_FALSE(f.canvas.isDrawing());
    EXPECT_EQ(f.canvas.document().items().size(), 1u);
    expectIdleAndAlive(f.canvas);
}

TEST(InkCanvasTest, DisablingTheCanvasMidStrokeEndsAndKeepsTheStroke)
{
    CanvasFixture f;
    penDown(f.canvas, QPointF(20, 50), QPointF(120, 50), 1000);
    f.canvas.setEnabled(false);
    EXPECT_FALSE(f.canvas.isDrawing());
    EXPECT_EQ(f.canvas.document().items().size(), 1u);
    f.canvas.setEnabled(true);
    expectIdleAndAlive(f.canvas);
}

TEST(InkCanvasTest, SettingReadOnlyMidStrokeEndsAndKeepsTheStroke)
{
    CanvasFixture f;
    penDown(f.canvas, QPointF(20, 50), QPointF(120, 50), 1000);
    f.canvas.setReadOnly(true);
    EXPECT_FALSE(f.canvas.isDrawing());
    EXPECT_EQ(f.canvas.document().items().size(), 1u);
    tablet(&f.canvas, QEvent::TabletRelease, QPointF(120, 50), 0.5, 1030, Qt::NoButton); // the late release changes nothing
    EXPECT_EQ(f.canvas.document().items().size(), 1u);
    f.canvas.setReadOnly(false);
    expectIdleAndAlive(f.canvas);
}

TEST(InkCanvasTest, ReplacingTheDocumentMidStrokeCancelsAndNeverCommitsIntoTheNewOne)
{
    CanvasFixture f;
    penDown(f.canvas, QPointF(20, 50), QPointF(120, 50), 1000);
    ASSERT_TRUE(f.canvas.isDrawing());
    f.canvas.document().reset(f.canvas.document().settings(), {}, {});
    f.canvas.documentReplaced();
    EXPECT_FALSE(f.canvas.isDrawing());
    tablet(&f.canvas, QEvent::TabletMove, QPointF(140, 50), 0.5, 1030);
    tablet(&f.canvas, QEvent::TabletRelease, QPointF(140, 50), 0.5, 1035, Qt::NoButton);
    EXPECT_TRUE(f.canvas.document().items().empty()) << "the old stroke is dropped, not committed into the new document";
    EXPECT_FALSE(f.canvas.document().canUndo());
    expectIdleAndAlive(f.canvas);
}

TEST(InkCanvasTest, ZoomingMidStrokeEndsAndKeepsTheStroke)
{
    CanvasFixture f;
    penDown(f.canvas, QPointF(20, 50), QPointF(120, 50), 1000);
    f.canvas.setZoom(2.0);
    EXPECT_FALSE(f.canvas.isDrawing());
    ASSERT_EQ(f.canvas.document().items().size(), 1u);
    EXPECT_NEAR(f.canvas.document().items()[0].stroke->points.back().x, 120.0, 1.0) << "in document px, at the zoom it was drawn in";
    tablet(&f.canvas, QEvent::TabletMove, QPointF(300, 100), 0.5, 1030); // late events of the old stroke are ignored
    tablet(&f.canvas, QEvent::TabletRelease, QPointF(300, 100), 0.5, 1035, Qt::NoButton);
    EXPECT_EQ(f.canvas.document().items().size(), 1u);
}

TEST(InkCanvasTest, TheMouseLeavingMidStrokeEndsAndKeepsTheStroke)
{
    CanvasFixture f;
    f.canvas.setDrawWithMouse(true);
    QTest::mousePress(&f.canvas, Qt::LeftButton, Qt::NoModifier, QPoint(20, 20));
    QTest::mouseMove(&f.canvas, QPoint(60, 40));
    QEvent leave(QEvent::Leave);
    QApplication::sendEvent(&f.canvas, &leave);
    EXPECT_FALSE(f.canvas.isDrawing());
    EXPECT_EQ(f.canvas.document().items().size(), 1u);
}

TEST(InkCanvasTest, AnEraserSweepOpenWhenInterruptedCommitsAsItsOwnUndoStep)
{
    CanvasFixture f;
    penStroke(&f.canvas, QPointF(20, 50), QPointF(200, 50), 1000);
    f.canvas.setTool(InkCanvas::Tool::Eraser);
    tablet(&f.canvas, QEvent::TabletPress, QPointF(100, 30), 0.5, 2000);
    tablet(&f.canvas, QEvent::TabletMove, QPointF(100, 60), 0.5, 2005);
    ASSERT_TRUE(f.canvas.document().items().empty());
    f.canvas.hide();
    EXPECT_FALSE(f.canvas.isDrawing());
    f.canvas.show();
    f.canvas.setTool(InkCanvas::Tool::Pen);
    penStroke(&f.canvas, QPointF(20, 150), QPointF(200, 150), 3000);
    ASSERT_EQ(f.canvas.document().items().size(), 1u);
    ASSERT_TRUE(f.canvas.undo()); // the new stroke only: the erase was its own step
    EXPECT_TRUE(f.canvas.document().items().empty());
    ASSERT_TRUE(f.canvas.undo());
    EXPECT_EQ(f.canvas.document().items().size(), 1u) << "undoing the erase brings the first stroke back";
}

TEST(InkCanvasTest, ALostReleaseThenAFarAwayPressAddsNoConnectingSegment)
{
    CanvasFixture f;
    penDown(f.canvas, QPointF(20, 20), QPointF(60, 20), 1000); // no release ever arrives
    tablet(&f.canvas, QEvent::TabletPress, QPointF(350, 250), 0.5, 2000);
    tablet(&f.canvas, QEvent::TabletMove, QPointF(360, 260), 0.5, 2005);
    tablet(&f.canvas, QEvent::TabletRelease, QPointF(370, 270), 0.5, 2010, Qt::NoButton);
    ASSERT_EQ(f.canvas.document().items().size(), 2u);
    const QRectF old = f.canvas.document().items()[0].stroke->bounds();
    EXPECT_LT(old.right(), 100.0) << "the old stroke ended at its own last sample";
    EXPECT_LT(old.bottom(), 50.0);
    EXPECT_GT(f.canvas.document().items()[1].stroke->bounds().left(), 300.0);
}

TEST(InkCanvasTest, TheEraserCatchesAThinStrokeBetweenTwoSamplesFarApart)
{
    CanvasFixture f;
    StrokeStyle thin;
    thin.width = 1.0;
    f.canvas.setPenStyle(thin);
    penStroke(&f.canvas, QPointF(20, 100), QPointF(300, 100), 1000);
    ASSERT_EQ(f.canvas.document().items().size(), 1u);
    f.canvas.setTool(InkCanvas::Tool::Eraser);
    tablet(&f.canvas, QEvent::TabletPress, QPointF(150, 20), 0.5, 2000); // 80 px above, then 80 px below: no sample touches it
    tablet(&f.canvas, QEvent::TabletRelease, QPointF(150, 180), 0.5, 2008, Qt::NoButton);
    EXPECT_TRUE(f.canvas.document().items().empty()) << "the swept segment crossed the stroke";
}

TEST(InkCanvasTest, TheEraserStillLeavesAStrokeItsSweepMissesEvenWhenTheBoxesOverlap)
{
    CanvasFixture f;
    penStroke(&f.canvas, QPointF(20, 100), QPointF(300, 100), 1000);
    f.canvas.setTool(InkCanvas::Tool::Eraser);
    tablet(&f.canvas, QEvent::TabletPress, QPointF(150, 20), 0.5, 2000);
    tablet(&f.canvas, QEvent::TabletRelease, QPointF(260, 60), 0.5, 2008, Qt::NoButton); // ends 40 px above the stroke
    EXPECT_EQ(f.canvas.document().items().size(), 1u);
}

TEST(InkCanvasTest, ErasingNearALongStrokeDoesNotRebuildItsOutline)
{
    // A 10 000-point scribble whose box covers the eraser path but which the eraser never touches:
    // the hit test walks its points, it must not build the outline path each time.
    CanvasFixture f;
    auto scribble = std::make_shared<Stroke>(); // 10 000 points round the page's edge: its box is the whole canvas
    for (int i = 0; i < 10000; ++i) {
        const double u = 4.0 * i / 10000.0; // 0..4: four sides
        const double side = std::floor(u), along = u - side;
        const QPointF p = side == 0 ? QPointF(10 + 380 * along, 10) : side == 1 ? QPointF(390, 10 + 280 * along)
                        : side == 2 ? QPointF(390 - 380 * along, 290) : QPointF(10, 290 - 280 * along);
        const double jitter = (i % 2 ? 6.0 : -6.0) * (1 + i % 3); // a hand-drawn scribble, not a straight line: no point is skipped as a duplicate
        scribble->points.push_back({p.x() + (side == 1 || side == 3 ? jitter : 0), p.y() + (side == 0 || side == 2 ? jitter : 0), 0.5});
    }
    f.canvas.document().addStroke(scribble);
    f.canvas.document().commit();
    f.canvas.setTool(InkCanvas::Tool::Eraser);
    // A ratio, not a wall-clock number: one eraser sample must cost a fraction of one outline build.
    QElapsedTimer timer;
    qint64 outlineNs = std::numeric_limits<qint64>::max();
    for (int i = 0; i < 3; ++i) {
        timer.restart();
        const QPainterPath path = strokeOutline(*scribble);
        outlineNs = std::min<qint64>(outlineNs, timer.nsecsElapsed());
        ASSERT_FALSE(path.isEmpty());
    }
    constexpr int kGestures = 20; // two samples each
    timer.restart();
    for (int i = 0; i < kGestures; ++i) {
        tablet(&f.canvas, QEvent::TabletPress, QPointF(150 + i, 130), 0.5, 1000 + i * 10);
        tablet(&f.canvas, QEvent::TabletRelease, QPointF(150 + i, 170), 0.5, 1005 + i * 10, Qt::NoButton);
    }
    const qint64 gesturesNs = timer.nsecsElapsed();
    EXPECT_EQ(f.canvas.document().items().size(), 1u) << "never touched";
    EXPECT_LT(gesturesNs, outlineNs * 2 * kGestures / 4)
        << "an eraser sample cost over a quarter of an outline build: " << gesturesNs / (2 * kGestures) << " ns vs " << outlineNs;
}

TEST(InkCanvasTest, TheReleaseSampleKeepsTheLastContactPressureNotTheLiftedZero)
{
    CanvasFixture f;
    f.canvas.setRawInput(true); // the points are the samples: nothing smooths a zero away
    tablet(&f.canvas, QEvent::TabletPress, QPointF(20, 50), 0.5, 1000);
    for (int i = 1; i <= 8; ++i)
        tablet(&f.canvas, QEvent::TabletMove, QPointF(20 + 15 * i, 50), 0.5, 1000 + 5 * i);
    tablet(&f.canvas, QEvent::TabletRelease, QPointF(140, 50), 0.0, 1045, Qt::NoButton); // real pens report 0 at lift
    ASSERT_EQ(f.canvas.document().items().size(), 1u);
    const auto &points = f.canvas.document().items()[0].stroke->points;
    ASSERT_GE(points.size(), 3u);
    EXPECT_NEAR(points.back().pressure, 0.5, 0.05) << "no pinched tail";
    EXPECT_NEAR(points.back().pressure, points[points.size() - 2].pressure, 0.1);
    EXPECT_EQ(f.canvas.learnedPressure(QStringLiteral("test pen")), 1.0) << "the lifted zero taught the normaliser nothing";
}

TEST(InkCanvasTest, ABarrelButtonChangeIsNotAPenDownOrUp)
{
    CanvasFixture f;
    penDown(f.canvas, QPointF(20, 50), QPointF(120, 50), 1000);
    QTabletEvent barrel(QEvent::TabletPress, stylus(), QPointF(120, 50), f.canvas.mapToGlobal(QPoint(120, 50)), 0.5, 0, 0, 0, 0, 0,
                        Qt::NoModifier, Qt::RightButton, Qt::LeftButton | Qt::RightButton);
    barrel.setTimestamp(1030);
    QApplication::sendEvent(&f.canvas, &barrel);
    EXPECT_TRUE(f.canvas.isDrawing());
    EXPECT_TRUE(f.canvas.document().items().empty()) << "the barrel press did not close the stroke";
    QTabletEvent barrelUp(QEvent::TabletRelease, stylus(), QPointF(120, 50), f.canvas.mapToGlobal(QPoint(120, 50)), 0.5, 0, 0, 0, 0, 0,
                          Qt::NoModifier, Qt::RightButton, Qt::LeftButton);
    barrelUp.setTimestamp(1035);
    QApplication::sendEvent(&f.canvas, &barrelUp);
    EXPECT_TRUE(f.canvas.isDrawing()) << "nor did releasing it";
}

TEST(InkCanvasTest, TouchIsIgnoredWhileAPenStrokeIsInProgress)
{
    CanvasFixture f;
    QSignalSpy pans(&f.canvas, &InkCanvas::panRequested);
    penDown(f.canvas, QPointF(20, 50), QPointF(120, 50), 1000);
    QTest::touchEvent(&f.canvas, finger()).press(0, QPoint(200, 200), &f.canvas);
    QTest::touchEvent(&f.canvas, finger()).move(0, QPoint(230, 190), &f.canvas);
    QTest::touchEvent(&f.canvas, finger()).release(0, QPoint(230, 190), &f.canvas);
    EXPECT_EQ(pans.count(), 0) << "a palm during a stroke does not move the page";
}

TEST(InkCanvasTest, TheLiveOverlayCoversOnlyTheViewportNeverTheWholeDocument)
{
    QScrollArea area;
    area.resize(400, 300);
    auto *canvas = new InkCanvas;
    DocumentSettings settings;
    settings.format = {400, 300, 96};
    settings.extent = QRectF(0, 0, 400, 100 * 300.0); // 100 pages
    settings.layout = LayoutKind::ContinuousVertical;
    canvas->document().setSettings(settings);
    canvas->documentReplaced();
    area.setWidget(canvas);
    area.show();
    if (!QTest::qWaitForWindowExposed(&area))
        ADD_FAILURE() << "never exposed";
    ASSERT_GT(canvas->height(), 25000);
    tablet(canvas, QEvent::TabletPress, QPointF(20, 20), 0.5, 1000);
    tablet(canvas, QEvent::TabletMove, QPointF(60, 20), 0.5, 1005);
    const QSize overlay = canvas->liveOverlaySize();
    EXPECT_GT(overlay.width(), 0);
    const double dpr = canvas->devicePixelRatio();
    EXPECT_LE(overlay.height(), area.viewport()->height() * dpr + 1) << "not a 30 000 px image";
    EXPECT_LE(overlay.width(), area.viewport()->width() * dpr + 1);
    tablet(canvas, QEvent::TabletRelease, QPointF(100, 20), 0.5, 1010, Qt::NoButton);
    EXPECT_EQ(canvas->document().items().size(), 1u);
}

// The same test runs in the ink_dpr2 ctest entry with QT_SCALE_FACTOR=2 (tests/CMakeLists.txt).
TEST(InkCanvasDprTest, PositionsAndTheLiveOverlayAreRightAtTheScreensPixelRatio)
{
    CanvasFixture f;
    const double expected = qEnvironmentVariableIsSet("QT_SCALE_FACTOR") ? qEnvironmentVariable("QT_SCALE_FACTOR").toDouble() : 1.0;
    ASSERT_NEAR(f.canvas.devicePixelRatio(), expected, 1e-9) << "the platform honours QT_SCALE_FACTOR";
    StrokeStyle style;
    style.width = 8.0;
    style.color = QColor(Qt::black);
    f.canvas.setPenStyle(style);
    f.canvas.setRawInput(true);
    penDown(f.canvas, QPointF(40, 60), QPointF(240, 60), 1000); // mid-stroke: the overlay is what paints it
    const QImage frame = settledGrab(f.canvas);
    ASSERT_EQ(frame.size(), QSize(int(400 * expected), int(300 * expected)));
    EXPECT_LT(qGray(frame.pixel(int(120 * expected), int(60 * expected))), 80) << "ink under the pen, in device px";
    EXPECT_GT(qGray(frame.pixel(int(120 * expected), int(100 * expected))), 200) << "paper 40 px below";
    EXPECT_GT(qGray(frame.pixel(int(120 * expected), int(30 * expected))), 200) << "paper 30 px above";
    tablet(&f.canvas, QEvent::TabletRelease, QPointF(240, 60), 0.5, 1030, Qt::NoButton);
    ASSERT_EQ(f.canvas.document().items().size(), 1u);
    const auto &points = f.canvas.document().items()[0].stroke->points;
    EXPECT_NEAR(points.front().x, 40.0, 0.5) << "document px, whatever the pixel ratio";
    EXPECT_NEAR(points.front().y, 60.0, 0.5);
    EXPECT_NEAR(points.back().x, 240.0, 0.5);
}

TEST(InkCanvasTest, ReplacingTheSameDocumentMidEraseBringsTheUncommittedRemovalBack)
{
    CanvasFixture f;
    penStroke(&f.canvas, QPointF(20, 50), QPointF(200, 50), 1000);
    penStroke(&f.canvas, QPointF(20, 150), QPointF(200, 150), 2000);
    f.canvas.document().markSaved();
    f.canvas.setTool(InkCanvas::Tool::Eraser);
    tablet(&f.canvas, QEvent::TabletPress, QPointF(100, 30), 0.5, 3000);
    tablet(&f.canvas, QEvent::TabletMove, QPointF(100, 70), 0.5, 3005); // removes the first stroke, uncommitted
    ASSERT_EQ(f.canvas.document().items().size(), 1u);
    f.canvas.documentReplaced(); // the same document object: its live removal must not survive the cancel
    EXPECT_FALSE(f.canvas.isDrawing());
    EXPECT_EQ(f.canvas.document().items().size(), 2u) << "the erased stroke is back";
    EXPECT_FALSE(f.canvas.document().isModified());
    tablet(&f.canvas, QEvent::TabletRelease, QPointF(100, 70), 0.5, 3010, Qt::NoButton); // the late release changes nothing
    EXPECT_EQ(f.canvas.document().items().size(), 2u);
    QTest::keyClick(&f.canvas, Qt::Key_Z, Qt::ControlModifier);
    EXPECT_EQ(f.canvas.document().items().size(), 1u) << "Ctrl+Z undoes the last real step (the second stroke) only";
    EXPECT_NEAR(f.canvas.document().items()[0].stroke->points.front().y, 50.0, 1.0);
}

// --- Task 9: page layouts -------------------------------------------------------

namespace {

/// An Infinite canvas whose extent starts left of and above the origin: widget (0, 0) is document (-200, -150).
struct InfiniteFixture {
    InkCanvas canvas;
    InfiniteFixture()
    {
        DocumentSettings settings;
        settings.format = {400, 300, 96};
        settings.layout = LayoutKind::Infinite;
        settings.extent = QRectF(-200, -150, 400, 300);
        canvas.document().setSettings(settings);
        canvas.document().commit(); // the sheet is the base of the history: an undo goes back to it, not to a default page
        canvas.document().markSaved();
        canvas.documentReplaced();
        canvas.resize(canvas.sizeHint());
        canvas.show();
        if (!QTest::qWaitForWindowExposed(&canvas))
            ADD_FAILURE() << "the canvas was never exposed";
    }
};

} // namespace

TEST(InkLayoutTest, SemiInfiniteGrowsRightAndDownOnly)
{
    InkDocument doc;
    DocumentSettings s;
    s.layout = LayoutKind::SemiInfinite;
    s.format = {100, 100, 96};
    s.extent = QRectF(0, 0, 100, 100);
    ASSERT_TRUE(doc.setSettings(s));
    EXPECT_TRUE(doc.growToContain(QRectF(150, 20, 10, 10)));
    EXPECT_GE(doc.settings().extent.right(), 160 + 100) << "a page width of margin past the ink";
    EXPECT_EQ(doc.settings().extent.left(), 0);
    EXPECT_EQ(doc.settings().extent.bottom(), 100) << "ink inside the height does not grow it";
    EXPECT_FALSE(doc.growToContain(QRectF(-50, -50, 10, 10))) << "never left or up";
    EXPECT_EQ(doc.settings().extent.left(), 0);
    EXPECT_EQ(doc.settings().extent.top(), 0);
    EXPECT_TRUE(doc.growToContain(QRectF(20, 150, 10, 10)));
    EXPECT_GE(doc.settings().extent.bottom(), 160 + 100) << "a page height of margin below the ink";
    EXPECT_EQ(doc.pageCount(), 1);
}

TEST(InkLayoutTest, InfiniteGrowsEveryWay)
{
    InkDocument doc;
    DocumentSettings s;
    s.layout = LayoutKind::Infinite;
    s.format = {100, 100, 96};
    s.extent = QRectF(0, 0, 100, 100);
    ASSERT_TRUE(doc.setSettings(s));
    EXPECT_TRUE(doc.growToContain(QRectF(-50, -50, 10, 10)));
    EXPECT_LE(doc.settings().extent.left(), -150);
    EXPECT_LE(doc.settings().extent.top(), -150);
    EXPECT_EQ(doc.settings().extent.right(), 100) << "ink inside the right and bottom edges does not grow them";
    EXPECT_EQ(doc.settings().extent.bottom(), 100);
    EXPECT_TRUE(doc.growToContain(QRectF(250, 300, 10, 10)));
    EXPECT_GE(doc.settings().extent.right(), 260 + 100);
    EXPECT_GE(doc.settings().extent.bottom(), 310 + 100);
    EXPECT_FALSE(doc.growToContain(QRectF(0, 0, 10, 10))) << "ink already inside changes nothing";
    EXPECT_EQ(doc.pageCount(), 1) << "one sheet however far it grows";
    EXPECT_EQ(doc.pageRect(0), doc.settings().extent);
    EXPECT_TRUE(doc.settings().isUsable());
}

TEST(InkLayoutTest, TheInfiniteLayoutsRefuseHostileBoundsAndTheSameCapAsPages)
{
    for (LayoutKind layout : {LayoutKind::SemiInfinite, LayoutKind::Infinite}) {
        InkDocument doc;
        DocumentSettings s;
        s.layout = layout;
        s.format = {100, 100, 96};
        s.extent = QRectF(0, 0, 100, 100);
        ASSERT_TRUE(doc.setSettings(s));
        const double inf = std::numeric_limits<double>::infinity();
        EXPECT_FALSE(doc.growToContain(QRectF(0, 0, inf, 10)));
        EXPECT_FALSE(doc.growToContain(QRectF(0, 0, 10, inf)));
        EXPECT_FALSE(doc.growToContain(QRectF(1e15, 0, 1, 1))) << "past kMaxPages sheets wide";
        EXPECT_FALSE(doc.growToContain(QRectF(0, 1e15, 1, 1)));
        EXPECT_FALSE(doc.growToContain(QRectF(0, 0, 0, 0))) << "an empty rect";
        EXPECT_EQ(doc.settings().extent, QRectF(0, 0, 100, 100));
        EXPECT_TRUE(doc.growToContain(QRectF(0, 1e6, 1, 1))) << "a million px is 10 000 sheets: inside the cap";
    }
    InkDocument doc;
    DocumentSettings s;
    s.layout = LayoutKind::Infinite;
    s.format = {100, 100, 96};
    s.extent = QRectF(0, 0, 100, 100);
    ASSERT_TRUE(doc.setSettings(s));
    EXPECT_FALSE(doc.growToContain(QRectF(-1e15, -1e15, 1, 1)));
    EXPECT_EQ(doc.settings().extent, QRectF(0, 0, 100, 100));
}

TEST(InkLayoutTest, CanvasAccountsForPageGapsInContinuousVertical)
{
    InkCanvas canvas;
    DocumentSettings s;
    s.format = {100, 200, 96};
    s.extent = QRectF(0, 0, 100, 400); // two pages
    ASSERT_TRUE(canvas.document().setSettings(s));
    canvas.documentReplaced();
    EXPECT_EQ(canvas.sizeHint(), QSize(100, 400 + int(InkDocument::kPageGap)));
    EXPECT_EQ(canvas.toWidget(QPointF(10, 210)), QPointF(10, 210 + InkDocument::kPageGap));
    EXPECT_EQ(canvas.toDocument(QPointF(10, 210 + InkDocument::kPageGap)), QPointF(10, 210));
    EXPECT_EQ(canvas.toDocument(QPointF(10, 205)), QPointF(10, 200)) << "in the gap: the page above's bottom edge";
    EXPECT_EQ(canvas.toDocument(QPointF(10, 199)), QPointF(10, 199));
    EXPECT_EQ(canvas.toDocument(QPointF(10, 216)), QPointF(10, 200)) << "the top edge of page two";
    canvas.setZoom(2.0);
    EXPECT_EQ(canvas.sizeHint(), QSize(200, 2 * (400 + int(InkDocument::kPageGap))));
    EXPECT_EQ(canvas.toWidget(QPointF(10, 210)), QPointF(20, 2 * (210 + InkDocument::kPageGap)));
    EXPECT_EQ(canvas.toDocument(QPointF(20, 2 * (210 + InkDocument::kPageGap))), QPointF(10, 210));
}

TEST(InkLayoutTest, GapPixelsBelowAPageCarryTheShadow)
{
    InkCanvas canvas;
    DocumentSettings s;
    s.format = {100, 200, 96};
    s.extent = QRectF(0, 0, 100, 400);
    ASSERT_TRUE(canvas.document().setSettings(s));
    canvas.documentReplaced();
    canvas.resize(canvas.sizeHint());
    const QImage shot = settledGrab(canvas);
    const QColor window = canvas.palette().color(QPalette::Window);
    const int gapTop = 200;                              // the first page's bottom edge, widget px at zoom 1
    const int gapBottom = 200 + int(InkDocument::kPageGap) - 1;
    EXPECT_EQ(shot.pixelColor(50, gapBottom), window) << "the far side of the gap is the plain window colour";
    EXPECT_LT(shot.pixelColor(50, gapTop + 1).lightness(), window.lightness()) << "the shadow darkens the gap just under the page";
    EXPECT_LE(shot.pixelColor(50, gapTop + 1).lightness(), shot.pixelColor(50, gapTop + 3).lightness())
        << "and fades with distance";
    EXPECT_EQ(shot.pixelColor(50, 100), s.background.color) << "the page itself is the background colour";
    EXPECT_EQ(shot.pixelColor(50, 300 + int(InkDocument::kPageGap)), s.background.color) << "so is the second page";
    EXPECT_EQ(shot.pixelColor(0, 100).rgb(), canvas.palette().color(QPalette::Mid).rgb()) << "with a 1 px border";
}

TEST(InkLayoutTest, InkAcrossAPageBreakIsCutByTheGapAndLandsInDocumentCoordinates)
{
    InkCanvas canvas;
    DocumentSettings s;
    s.format = {100, 200, 96};
    s.extent = QRectF(0, 0, 100, 400);
    ASSERT_TRUE(canvas.document().setSettings(s));
    canvas.documentReplaced();
    canvas.resize(canvas.sizeHint());
    canvas.show();
    ASSERT_TRUE(QTest::qWaitForWindowExposed(&canvas));
    StrokeStyle style;
    style.width = 6.0;
    style.color = QColor(0, 0, 0);
    canvas.setPenStyle(style);
    penStroke(&canvas, QPointF(50, 150), QPointF(50, 290), 1000); // widget px: page one, the gap, page two
    ASSERT_EQ(canvas.document().items().size(), 1u);
    const Stroke &stroke = *canvas.document().items()[0].stroke;
    EXPECT_NEAR(stroke.points.front().y, 150.0, 1.0);
    EXPECT_NEAR(stroke.points.back().y, 290.0 - InkDocument::kPageGap, 1.0) << "stored without the gap";
    const QImage shot = settledGrab(canvas);
    const QColor window = canvas.palette().color(QPalette::Window);
    EXPECT_LT(shot.pixelColor(50, 180).lightness(), 60) << "ink on page one";
    EXPECT_LT(shot.pixelColor(50, 260).lightness(), 60) << "ink on page two";
    EXPECT_GE(shot.pixelColor(50, 208).lightness(), window.lightness() - 40) << "the gap is not inked over";
    EXPECT_EQ(shot.pixelColor(50, 200 + int(InkDocument::kPageGap) - 1), window);
}

TEST(InkLayoutTest, TheLiveOverlayFollowsThePageGaps)
{
    InkCanvas canvas;
    DocumentSettings s;
    s.format = {100, 200, 96};
    s.extent = QRectF(0, 0, 100, 400);
    ASSERT_TRUE(canvas.document().setSettings(s));
    canvas.documentReplaced();
    canvas.resize(canvas.sizeHint());
    canvas.show();
    ASSERT_TRUE(QTest::qWaitForWindowExposed(&canvas));
    StrokeStyle style;
    style.width = 6.0;
    style.color = QColor(0, 0, 0);
    canvas.setPenStyle(style);
    tablet(&canvas, QEvent::TabletPress, QPointF(50, 150), 0.5, 1000);
    for (int i = 1; i <= 14; ++i)
        tablet(&canvas, QEvent::TabletMove, QPointF(50, 150 + 10 * i), 0.5, 1000 + 5 * i);
    ASSERT_TRUE(canvas.isDrawing());
    const QImage shot = settledGrab(canvas); // mid-stroke: the overlay paints it
    EXPECT_LT(shot.pixelColor(50, 180).lightness(), 60) << "live ink on page one";
    EXPECT_LT(shot.pixelColor(50, 235).lightness(), 60) << "live ink on page two (the smoothed stroke trails the pen)";
    EXPECT_GT(shot.pixelColor(50, 208).lightness(), 60) << "none in the gap";
    tablet(&canvas, QEvent::TabletRelease, QPointF(50, 290), 0.5, 1100, Qt::NoButton);
}

TEST(InkLayoutTest, TheInfiniteCanvasMapsANegativeExtentOrigin)
{
    InfiniteFixture f;
    EXPECT_EQ(f.canvas.sizeHint(), QSize(400, 300)) << "no gaps outside continuous vertical";
    EXPECT_EQ(f.canvas.toWidget(QPointF(-200, -150)), QPointF(0, 0));
    EXPECT_EQ(f.canvas.toWidget(QPointF(0, 0)), QPointF(200, 150));
    EXPECT_EQ(f.canvas.toDocument(QPointF(0, 0)), QPointF(-200, -150));
    EXPECT_EQ(f.canvas.toDocument(QPointF(250, 200)), QPointF(50, 50));
    f.canvas.setZoom(2.0);
    EXPECT_EQ(f.canvas.sizeHint(), QSize(800, 600));
    EXPECT_EQ(f.canvas.toWidget(QPointF(-100, -50)), QPointF(200, 200));
    EXPECT_EQ(f.canvas.toDocument(QPointF(200, 200)), QPointF(-100, -50));
}

TEST(InkLayoutTest, InkInTheNegativeQuadrantIsDrawnFoundErasedAndSaved)
{
    InfiniteFixture f;
    StrokeStyle style;
    style.width = 4.0;
    style.color = QColor(0, 0, 0);
    f.canvas.setPenStyle(style);
    penStroke(&f.canvas, QPointF(40, 40), QPointF(160, 40), 1000); // document (-160, -110) to (-40, -110)
    ASSERT_EQ(f.canvas.document().items().size(), 1u);
    const Stroke &stroke = *f.canvas.document().items()[0].stroke;
    EXPECT_NEAR(stroke.points.front().x, -160.0, 1.0);
    EXPECT_NEAR(stroke.points.front().y, -110.0, 1.0);
    EXPECT_EQ(f.canvas.document().itemsIntersecting(QRectF(-100, -112, 4, 4)).size(), 1u) << "the grid finds ink at negative coordinates";
    EXPECT_LT(settledGrab(f.canvas).pixelColor(100, 40).lightness(), 60) << "painted where the pen was";

    // The file keeps the negative extent and the negative points.
    InkDocument loaded;
    const InkReadResult read = readInk(writeInk(f.canvas.document()), loaded);
    ASSERT_TRUE(read.ok) << read.error.toStdString();
    EXPECT_EQ(loaded.settings().extent, f.canvas.document().settings().extent);
    EXPECT_EQ(loaded.settings().layout, LayoutKind::Infinite);
    ASSERT_EQ(loaded.items().size(), 1u);
    EXPECT_NEAR(loaded.items()[0].stroke->points.front().x, stroke.points.front().x, 0.001);
    EXPECT_EQ(loaded.itemsIntersecting(QRectF(-100, -112, 4, 4)).size(), 1u);

    f.canvas.setTool(InkCanvas::Tool::Eraser);
    penStroke(&f.canvas, QPointF(100, 20), QPointF(100, 60), 2000);
    EXPECT_EQ(f.canvas.document().items().size(), 0u) << "the eraser reaches ink at negative coordinates";
}

TEST(InkLayoutTest, AFileWithANegativeExtentRoundTripsAndStaysUsable)
{
    InkDocument doc;
    DocumentSettings s;
    s.layout = LayoutKind::Infinite;
    s.format = {400, 300, 96};
    s.extent = QRectF(-1234.5, -678.25, 3000, 2000);
    ASSERT_TRUE(doc.setSettings(s));
    doc.addStroke(line(-1000, -600, -900, -500));
    doc.commit();
    InkDocument back;
    const InkReadResult read = readInk(writeInk(doc), back);
    ASSERT_TRUE(read.ok) << read.error.toStdString();
    EXPECT_EQ(back.settings().extent, s.extent);
    EXPECT_EQ(back.settings().layout, LayoutKind::Infinite);
    EXPECT_EQ(back.pageCount(), 1);
}

TEST(InkLayoutTest, GrowingLeftOrUpTellsTheOwnerHowFarTheContentMoved)
{
    InfiniteFixture f;
    StrokeStyle style;
    style.width = 8.0;
    style.color = QColor(0, 0, 0);
    f.canvas.setPenStyle(style);
    QSignalSpy shifted(&f.canvas, &InkCanvas::originShifted);
    const QRectF before = f.canvas.document().settings().extent;
    penStroke(&f.canvas, QPointF(2, 100), QPointF(60, 100), 1000); // the ink pokes out past the left edge
    ASSERT_EQ(f.canvas.document().items().size(), 1u);
    const QRectF after = f.canvas.document().settings().extent;
    EXPECT_LT(after.left(), before.left());
    EXPECT_EQ(after.top(), before.top()) << "only the left grew";
    ASSERT_EQ(shifted.count(), 1);
    EXPECT_EQ(shifted.first().first().toPoint(), QPoint(int(std::lround(before.left() - after.left())), 0));
    EXPECT_EQ(f.canvas.size(), f.canvas.sizeHint()) << "the widget grew with the extent";
    EXPECT_EQ(f.canvas.toWidget(QPointF(before.left(), before.top())).x(), before.left() - after.left())
        << "old content sits further right by exactly that";
}

TEST(InkLayoutTest, TheLastPagesBottomEdgeHasNoGapBelowIt)
{
    InkCanvas canvas;
    DocumentSettings s;
    s.format = {100, 200, 96};
    s.extent = QRectF(0, 0, 100, 400);
    ASSERT_TRUE(canvas.document().setSettings(s));
    canvas.documentReplaced();
    EXPECT_EQ(canvas.toWidget(QPointF(10, 400)).y(), canvas.sizeHint().height()) << "the bottom of the last page is the bottom of the widget";
    EXPECT_EQ(canvas.toWidget(QPointF(10, 200)).y(), 200 + InkDocument::kPageGap) << "a boundary between two pages is the top of the lower one";
    EXPECT_GT(canvas.toWidget(QPointF(10, 400.5)).y(), canvas.sizeHint().height()) << "past the extent the pattern continues, so growth lines up";
}

TEST(InkLayoutTest, ThreePagesAtANonZeroOriginAndFractionalZoomMapBothWaysAndAccumulateGaps)
{
    InkCanvas canvas;
    DocumentSettings s;
    const double h = 297.64; // not an integer: page edges are not exact in floating point
    s.format = {200, h, 96};
    s.extent = QRectF(30, -70, 200, 3 * h);
    ASSERT_TRUE(canvas.document().setSettings(s));
    canvas.setZoom(1.37);
    canvas.documentReplaced();
    EXPECT_EQ(canvas.sizeHint(), QSize(int(std::ceil(200 * 1.37)), int(std::ceil((3 * h + 2 * InkDocument::kPageGap) * 1.37))));
    for (int page = 0; page < 3; ++page) {
        for (double fraction : {0.0, 0.01, 0.3, 0.5, 0.99}) {
            const QPointF p(30 + 150 * fraction, -70 + page * h + fraction * h);
            const QPointF w = canvas.toWidget(p);
            EXPECT_NEAR(w.y(), (fraction * h + page * (h + InkDocument::kPageGap)) * 1.37, 1e-6) << "page " << page << " keeps " << page << " gaps above it";
            EXPECT_NEAR(w.x(), 150 * fraction * 1.37, 1e-9);
            const QPointF back = canvas.toDocument(w);
            EXPECT_NEAR(back.x(), p.x(), 1e-9);
            EXPECT_NEAR(back.y(), p.y(), 1e-9) << "page " << page << " fraction " << fraction;
        }
    }
    // The widget never maps into a gap from a document point, and a gap point lands on the bottom edge above.
    const QPointF inGap = canvas.toDocument(QPointF(10, (h + 5) * 1.37));
    EXPECT_NEAR(inGap.y(), -70 + h, 1e-9);
    const QPointF inSecondGap = canvas.toDocument(QPointF(10, (2 * h + InkDocument::kPageGap + 5) * 1.37));
    EXPECT_NEAR(inSecondGap.y(), -70 + 2 * h, 1e-9);
}

TEST(InkLayoutTest, TheEraserSweepsAcrossAGapInDocumentSpace)
{
    InkCanvas canvas;
    DocumentSettings s;
    s.format = {200, 200, 96};
    s.extent = QRectF(0, 0, 200, 400);
    ASSERT_TRUE(canvas.document().setSettings(s));
    canvas.documentReplaced();
    canvas.resize(canvas.sizeHint());
    canvas.show();
    ASSERT_TRUE(QTest::qWaitForWindowExposed(&canvas));
    penStroke(&canvas, QPointF(20, 170), QPointF(180, 170), 1000);               // page one
    penStroke(&canvas, QPointF(20, 240), QPointF(180, 240), 2000);               // page two (document y 224)
    penStroke(&canvas, QPointF(20, 380), QPointF(180, 380), 3000);               // page two, further down: out of the way
    ASSERT_EQ(canvas.document().items().size(), 3u);
    canvas.setTool(InkCanvas::Tool::Eraser);
    penStroke(&canvas, QPointF(100, 150), QPointF(100, 250), 4000); // widget px: from page one through the gap into page two
    ASSERT_EQ(canvas.document().items().size(), 1u) << "both strokes under the drag are gone, the far one stays";
    EXPECT_NEAR(canvas.document().items()[0].stroke->points.front().y, 380.0 - InkDocument::kPageGap, 1.0);
}

TEST(InkLayoutTest, OriginShiftFollowsEveryExtentOriginChangeIncludingUndoAndRedoButNotZoom)
{
    InfiniteFixture f;
    StrokeStyle style;
    style.width = 8.0;
    style.color = QColor(0, 0, 0);
    f.canvas.setPenStyle(style);
    QSignalSpy shifted(&f.canvas, &InkCanvas::originShifted);
    penStroke(&f.canvas, QPointF(2, 3), QPointF(60, 50), 1000); // pokes out past the left and the top edge
    ASSERT_EQ(shifted.count(), 1);
    const QPoint grown = shifted.last().first().toPoint();
    EXPECT_GT(grown.x(), 0);
    EXPECT_GT(grown.y(), 0);
    ASSERT_TRUE(f.canvas.undo());
    ASSERT_EQ(shifted.count(), 2);
    EXPECT_EQ(shifted.last().first().toPoint(), -grown) << "the origin went back: the content moves back";
    ASSERT_TRUE(f.canvas.redo());
    ASSERT_EQ(shifted.count(), 3);
    EXPECT_EQ(shifted.last().first().toPoint(), grown);
    f.canvas.setZoom(2.0);
    f.canvas.setZoom(0.5);
    EXPECT_EQ(shifted.count(), 3) << "a zoom keeps the origin";
    penStroke(&f.canvas, QPointF(100, 100), QPointF(160, 100), 2000); // inside the sheet: nothing grows
    EXPECT_EQ(shifted.count(), 3);
}

// --- page patterns (Task 10) ---------------------------------------------------

namespace {

const QRectF kA4Page(0, 0, 793.701, 1122.52);

Background backgroundOf(Pattern pattern)
{
    Background background;
    background.pattern = pattern;
    background.patternSize = pattern == Pattern::LinesMargin ? QSizeF(120, 32) : pattern == Pattern::Seyes ? QSizeF(64, 32) : QSizeF(32, 32);
    return background;
}

/// The corner of an A4 page, 240x160 document px, at zoom 1 and device ratio 1, on white.
QImage renderPattern(Pattern pattern)
{
    QImage image(240, 160, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::white);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    paintPattern(painter, backgroundOf(pattern), kA4Page, QRectF(0, 0, 240, 160), 1.0);
    return image;
}

/// 32x32 px tiles over the image: a whole-image share dilutes a missing dot, a tile does not.
std::vector<QRect> patternRegions(const QImage &image)
{
    std::vector<QRect> out;
    for (int y = 0; y < image.height(); y += 32)
        for (int x = 0; x < image.width(); x += 32)
            out.push_back(QRect(x, y, 32, 32) & image.rect());
    return out;
}

/// Counts what the painter hands the paint engine, so the cost tests need no clock.
class CountingEngine : public QPaintEngine
{
public:
    CountingEngine() : QPaintEngine(AllFeatures) {}
    bool begin(QPaintDevice *) override { return true; }
    bool end() override { return true; }
    void updateState(const QPaintEngineState &) override {}
    void drawPixmap(const QRectF &, const QPixmap &, const QRectF &) override { ++calls; }
    void drawLines(const QLineF *, int count) override { calls += count; }
    void drawLines(const QLine *, int count) override { calls += count; }
    void drawRects(const QRectF *, int count) override { calls += count; }
    void drawRects(const QRect *, int count) override { calls += count; }
    void drawPoints(const QPointF *, int count) override { calls += count; }
    void drawPoints(const QPoint *, int count) override { calls += count; }
    void drawPath(const QPainterPath &) override { ++calls; }
    void drawPolygon(const QPointF *, int, PolygonDrawMode) override { ++calls; }
    void drawPolygon(const QPoint *, int, PolygonDrawMode) override { ++calls; }
    Type type() const override { return User; }
    int calls = 0;
};

class CountingDevice : public QPaintDevice
{
public:
    QPaintEngine *paintEngine() const override { return &engine; }
    int calls() const { return engine.calls; }

protected:
    int metric(PaintDeviceMetric metric) const override
    {
        switch (metric) {
        case PdmWidth: case PdmHeight: return 4000;
        case PdmDepth: return 32;
        case PdmDpiX: case PdmDpiY: case PdmPhysicalDpiX: case PdmPhysicalDpiY: return 96;
        case PdmDevicePixelRatio: return 1;
        case PdmDevicePixelRatioScaled: return int(QPaintDevice::devicePixelRatioFScale());
        default: return 0;
        }
    }

private:
    mutable CountingEngine engine;
};

int drawCalls(const Background &background, const QRectF &page, const QRectF &exposed, double zoom = 1.0)
{
    CountingDevice device;
    QPainter painter(&device);
    painter.setRenderHint(QPainter::Antialiasing);
    paintPattern(painter, background, page, exposed, zoom);
    painter.end();
    return device.calls();
}

/// Exact pixel equality inside `area` (both images came from the same code, so anything else is a missing line).
bool imagesMatchIn(const QImage &a, const QImage &b, const QRect &area, QString *why)
{
    for (int y = area.top(); y <= area.bottom(); ++y)
        for (int x = area.left(); x <= area.right(); ++x)
            if (a.pixel(x, y) != b.pixel(x, y)) {
                *why = QStringLiteral("pixel %1,%2 differs: %3 vs %4").arg(x).arg(y).arg(a.pixel(x, y), 0, 16).arg(b.pixel(x, y), 0, 16);
                return false;
            }
    return true;
}

bool hasInk(const QImage &image, const QRect &area)
{
    for (int y = area.top(); y <= area.bottom(); ++y)
        for (int x = area.left(); x <= area.right(); ++x)
            if (image.pixel(x, y) != qRgb(255, 255, 255))
                return true;
    return false;
}

} // namespace

TEST(PagePatternTest, MatchesTheGoldenImages)
{
    for (const auto &[pattern, name] : {std::pair{Pattern::None, "none"}, {Pattern::Lines, "lines"}, {Pattern::Grid, "grid"},
                                         {Pattern::Dots, "dots"}, {Pattern::LinesMargin, "linesMargin"},
                                         {Pattern::GridMajor, "gridMajor"}, {Pattern::MusicStaff, "musicStaff"},
                                         {Pattern::Calligraphy, "calligraphy"}, {Pattern::Seyes, "seyes"},
                                         {Pattern::Cornell, "cornell"}, {Pattern::HexGrid, "hexGrid"}}) {
        SCOPED_TRACE(name);
        const QImage image = renderPattern(pattern);
        EXPECT_EQ(image.size(), QSize(240, 160));
        if (pattern == Pattern::None)
            EXPECT_FALSE(hasInk(image, image.rect())) << "None paints nothing";
        else
            EXPECT_TRUE(hasInk(image, image.rect()));
        checkGolden(QStringLiteral("pattern-%1.png").arg(QLatin1String(name)), image, patternRegions(image));
    }
}

TEST(PagePatternTest, AnExposedRectPaintsExactlyWhatTheFullPageHasThere)
{
    // Every pattern, over regions that start mid-period: a line the range wrongly leaves out shows as a difference.
    const QRectF regions[] = {QRectF(137.3, 93.7, 90, 70), QRectF(0, 1060, 200, 62.52), QRectF(700, 11.1, 93.7, 50), QRectF(301, 500, 40, 25)};
    const QRectF window(0, 0, 800, 1130); // the page plus its margin, so the full render is one image
    for (Pattern pattern : {Pattern::Lines, Pattern::Grid, Pattern::Dots, Pattern::LinesMargin, Pattern::GridMajor, Pattern::MusicStaff,
                            Pattern::Calligraphy, Pattern::Seyes, Pattern::Cornell, Pattern::HexGrid}) {
        QImage full(800, 1130, QImage::Format_ARGB32_Premultiplied);
        full.fill(Qt::white);
        {
            QPainter painter(&full);
            painter.setRenderHint(QPainter::Antialiasing);
            paintPattern(painter, backgroundOf(pattern), kA4Page, window, 1.0);
        }
        for (const QRectF &region : regions) {
            SCOPED_TRACE(QStringLiteral("%1 at %2,%3").arg(patternName(pattern)).arg(region.x()).arg(region.y()).toStdString());
            QImage part(full.size(), QImage::Format_ARGB32_Premultiplied);
            part.fill(Qt::white);
            {
                QPainter painter(&part);
                painter.setRenderHint(QPainter::Antialiasing);
                paintPattern(painter, backgroundOf(pattern), kA4Page, region, 1.0);
            }
            const QRect inside = region.toRect().adjusted(1, 1, -1, -1); // clip pixels on the fractional edge differ in coverage
            QString why;
            EXPECT_TRUE(imagesMatchIn(part, full, inside, &why)) << why.toStdString();
        }
    }
}

TEST(PagePatternTest, VisibleLineRangeCoversOnlyTheExposedLines)
{
    const QRectF page(0, 0, 793.7, 1122.52);
    EXPECT_EQ(visibleLineRange(QRectF(0, 500, 100, 100), page, 32.0), (LineRange{16, 18})) << "lines at 512, 544 and 576";
    EXPECT_EQ(visibleLineRange(QRectF(0, 0, 100, 40), page, 32.0), (LineRange{0, 1}));
}

TEST(PagePatternTest, VisibleLineRangeIsEmptyAbovePastAndClippedToThePage)
{
    const QRectF page(0, 500, 100, 100); // lines at 500, 532, 564, 596
    EXPECT_TRUE(visibleLineRange(QRectF(0, 0, 100, 100), page, 32.0).isEmpty()) << "exposed rect above the page";
    EXPECT_TRUE(visibleLineRange(QRectF(0, 800, 100, 100), page, 32.0).isEmpty()) << "exposed rect below the page";
    EXPECT_EQ(visibleLineRange(QRectF(0, 590, 100, 400), page, 32.0), (LineRange{3, 3})) << "clipped to the page's last line";
}

TEST(PagePatternTest, VisibleLineRangeSurvivesHostileNumbers)
{
    const QRectF page(0, 0, 100, 100);
    const double inf = std::numeric_limits<double>::infinity();
    EXPECT_TRUE(visibleLineRange(QRectF(0, 0, 10, 10), page, 0.0).isEmpty());
    EXPECT_TRUE(visibleLineRange(QRectF(0, 0, 10, 10), page, -3.0).isEmpty());
    EXPECT_TRUE(visibleLineRange(QRectF(0, 0, 10, 10), page, std::numeric_limits<double>::quiet_NaN()).isEmpty());
    EXPECT_TRUE(visibleLineRange(QRectF(0, 1e300, 10, 10), page, 1e-3).isEmpty()) << "no int overflow on the way";
    const LineRange all = visibleLineRange(QRectF(0, -inf, 10, inf), QRectF(0, 0, 100, 1e12), 1e6);
    EXPECT_EQ(all.first, 0);
    EXPECT_LE(all.last, 1000000);
}

TEST(PagePatternTest, OnlyTheExposedRectIsPainted)
{
    QImage image(100, 100, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::white);
    const Background background = backgroundOf(Pattern::Lines);
    {
        QPainter painter(&image);
        painter.translate(0, -500); // the exposed rect (0,500,100,100) of the page lands on the image
        paintPattern(painter, background, kA4Page, QRectF(0, 500, 100, 100), 1.0);
    }
    EXPECT_TRUE(hasInk(image, image.rect())) << "lines at 512, 544 and 576";
    image.fill(Qt::white);
    {
        QPainter painter(&image);
        paintPattern(painter, background, kA4Page, QRectF(0, 5000, 100, 100), 1.0); // far below the page
        paintPattern(painter, background, kA4Page, QRectF(0, -5000, 100, 100), 1.0); // far above it
    }
    EXPECT_FALSE(hasInk(image, image.rect())) << "nothing outside the page";
}

TEST(PagePatternTest, PaintingIsClippedToTheExposedRect)
{
    QImage image(100, 100, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::white);
    {
        QPainter painter(&image);
        paintPattern(painter, backgroundOf(Pattern::Grid), kA4Page, QRectF(0, 0, 50, 50), 1.0);
    }
    EXPECT_TRUE(hasInk(image, QRect(0, 0, 50, 50)));
    EXPECT_FALSE(hasInk(image, QRect(52, 0, 48, 100))) << "right of the exposed rect";
    EXPECT_FALSE(hasInk(image, QRect(0, 52, 100, 48))) << "below the exposed rect";
}

TEST(PagePatternTest, DrawCallsFollowTheExposedRectNotThePageHeight)
{
    const QRectF tallPage(0, 0, 793.701, 100000.0);
    const QRectF small(0, 50000, 200, 100);
    for (Pattern pattern : {Pattern::Lines, Pattern::Grid, Pattern::Dots, Pattern::LinesMargin, Pattern::GridMajor, Pattern::MusicStaff,
                            Pattern::Calligraphy, Pattern::Seyes, Pattern::Cornell, Pattern::HexGrid}) {
        SCOPED_TRACE(patternName(pattern).toStdString());
        EXPECT_GT(drawCalls(backgroundOf(pattern), tallPage, small), 0);
        EXPECT_LT(drawCalls(backgroundOf(pattern), tallPage, small), 300)
            << "about 5 rows x 8 columns, 100 dashes a row for Calligraphy; the 100 000 px page has 3125 rows";
    }
    // Lines: one drawLine per ruled line, 100 px at a 32 px period meets 4 of them (padded by kLinePad) at most.
    const int lines = drawCalls(backgroundOf(Pattern::Lines), tallPage, small);
    EXPECT_GE(lines, 3);
    EXPECT_LE(lines, 5);
}

TEST(PagePatternTest, APatternFinerThanADevicePixelIsSkipped)
{
    Background fine = backgroundOf(Pattern::Grid);
    fine.patternSize = QSizeF(0.01, 0.01); // the reader accepts 1e-3: a million lines a paint
    EXPECT_EQ(drawCalls(fine, kA4Page, QRectF(0, 0, 1000, 1000)), 0);
    fine.patternSize = QSizeF(32, 32);
    EXPECT_EQ(drawCalls(fine, kA4Page, QRectF(0, 0, 1000, 1000), 0.01), 0) << "zoomed out until the lines are 0.32 px apart";
    EXPECT_GT(drawCalls(fine, kA4Page, QRectF(0, 0, 1000, 1000), 1.0), 0);
}

TEST(PagePatternTest, CornellDrawsTheSummaryLineAtTheBottomFifth)
{
    QImage image(200, 100, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::white);
    {
        QPainter painter(&image);
        painter.setRenderHint(QPainter::Antialiasing, false);
        Background background = backgroundOf(Pattern::Cornell);
        background.patternSize = QSizeF(50, 1000); // no ruled line inside the page
        paintPattern(painter, background, QRectF(0, 0, 200, 100), QRectF(0, 0, 200, 100), 1.0);
    }
    EXPECT_NE(image.pixel(100, 80), qRgb(255, 255, 255)) << "summary line at 100 - 0.2 * 100";
    EXPECT_NE(image.pixel(199, 80), qRgb(255, 255, 255)) << "across the whole page";
    EXPECT_EQ(image.pixel(100, 60), qRgb(255, 255, 255));
    EXPECT_EQ(image.pixel(100, 95), qRgb(255, 255, 255));
    EXPECT_NE(image.pixel(50, 40), qRgb(255, 255, 255)) << "margin line at x = patternSize.width, above the summary";
    EXPECT_EQ(image.pixel(50, 90), qRgb(255, 255, 255)) << "the margin stops at the summary line";
}

TEST(PagePatternTest, CanvasPaintsThePatternOnEachPageAnchoredToThatPageNotTheDocument)
{
    InkCanvas canvas;
    DocumentSettings s;
    s.format = {100, 200, 96};
    s.extent = QRectF(0, 0, 100, 400); // two pages, a gap between them
    s.background.pattern = Pattern::Lines;
    s.background.patternSize = QSizeF(32, 32);
    ASSERT_TRUE(canvas.document().setSettings(s));
    canvas.documentReplaced();
    canvas.resize(canvas.sizeHint());
    const QImage shot = settledGrab(canvas);
    for (int page = 0; page < 2; ++page) {
        SCOPED_TRACE(page);
        const int top = int(canvas.toWidget(QPointF(0, 200.0 * page)).y());
        EXPECT_TRUE(hasInk(shot, QRect(10, top + 31, 80, 4))) << "the line 32 px below this page's top";
        EXPECT_FALSE(hasInk(shot, QRect(10, top + 8, 80, 20))) << "nothing between the first two lines: page 2 does not continue page 1's rhythm";
    }
}

TEST(PagePatternTest, AFixedDocumentGrownByAPagePatternsTheWholeExtent)
{
    InkCanvas canvas;
    DocumentSettings s;
    s.layout = LayoutKind::Fixed;
    s.format = {100, 200, 96};
    s.extent = QRectF(0, 0, 100, 200);
    s.background.pattern = Pattern::Lines;
    ASSERT_TRUE(canvas.document().setSettings(s));
    canvas.document().addPage(); // the extent is now two pages tall; Fixed has no gap, so it is one sheet
    canvas.documentReplaced();
    canvas.resize(canvas.sizeHint());
    ASSERT_EQ(canvas.sizeHint().height(), 400);
    const QImage shot = settledGrab(canvas);
    EXPECT_TRUE(hasInk(shot, QRect(10, 31, 80, 4))) << "the first page";
    EXPECT_TRUE(hasInk(shot, QRect(10, 319, 80, 4))) << "the lower half: a line at 320";
    EXPECT_TRUE(hasInk(shot, QRect(10, 383, 80, 4))) << "and at 384";
}

TEST(PagePatternTest, AnInfiniteSheetKeepsItsPatternOnTheSameDocumentPositionsWhenItGrowsUpAndLeft)
{
    InkCanvas canvas;
    DocumentSettings s;
    s.layout = LayoutKind::Infinite;
    s.format = {400, 300, 96};
    s.extent = QRectF(-200, -150, 400, 300);
    s.background.pattern = Pattern::Grid;
    s.background.patternSize = QSizeF(32, 32);
    ASSERT_TRUE(canvas.document().setSettings(s));
    canvas.document().commit();
    canvas.documentReplaced();
    const QRectF patch(50, 0, 140, 140); // document px, away from the ink
    const auto render = [&] {
        canvas.resize(canvas.sizeHint());
        const QPointF origin = canvas.toWidget(patch.topLeft());
        EXPECT_DOUBLE_EQ(origin.x(), std::round(origin.x())) << "an integral extent origin, or the AA coverage would differ";
        EXPECT_DOUBLE_EQ(origin.y(), std::round(origin.y()));
        return settledGrab(canvas).copy(QRect(origin.toPoint(), patch.size().toSize()));
    };
    const QImage before = render();
    ASSERT_TRUE(hasInk(before, before.rect()));
    auto stroke = std::make_shared<Stroke>();
    stroke->style = {2.0, Qt::black, PressureCurve::Const};
    stroke->points = {{-250, -220, 1}, {-240, -210, 1}}; // pokes out up and left by amounts that are no multiple of 32
    canvas.document().addStroke(stroke);
    ASSERT_TRUE(canvas.document().growToContain(stroke->bounds()));
    canvas.document().commit();
    canvas.documentReplaced();
    const QPointF grownOrigin = canvas.document().settings().extent.topLeft();
    EXPECT_NE(std::fmod(grownOrigin.x() + 3200, 32.0), 0.0);
    EXPECT_NE(std::fmod(grownOrigin.y() + 3200, 32.0), 0.0);
    QString why;
    EXPECT_TRUE(imagesMatchIn(render(), before, before.rect(), &why)) << "after growth: " << why.toStdString();
    ASSERT_TRUE(canvas.undo());
    EXPECT_TRUE(imagesMatchIn(render(), before, before.rect(), &why)) << "after undo: " << why.toStdString();
}

TEST(PagePatternTest, AHexRowAnchoredPastThePageFootStillDrawsItsSidesAboveTheFoot)
{
    // r = 32, row period 48: rows at 0..480 fit a 520 px page, row 11 (anchor 528) does not, but its sides start at 512.
    QImage image(300, 520, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::white);
    {
        QPainter painter(&image);
        painter.setRenderHint(QPainter::Antialiasing);
        paintPattern(painter, backgroundOf(Pattern::HexGrid), QRectF(0, 0, 300, 520), QRectF(0, 0, 300, 520), 1.0);
    }
    EXPECT_TRUE(hasInk(image, QRect(54, 513, 3, 6))) << "the side at x = sqrt(3) * 32 on the last 8 px of the page";
}

TEST(PagePatternTest, DrawCallsAlsoFollowTheExposedRectOnAVeryWidePage)
{
    const QRectF widePage(0, 0, 100000.0, 1122.52);
    const QRectF small(50000, 500, 100, 200);
    for (Pattern pattern : {Pattern::Lines, Pattern::Grid, Pattern::Dots, Pattern::LinesMargin, Pattern::GridMajor, Pattern::MusicStaff,
                            Pattern::Calligraphy, Pattern::Seyes, Pattern::Cornell, Pattern::HexGrid}) {
        SCOPED_TRACE(patternName(pattern).toStdString());
        EXPECT_LT(drawCalls(backgroundOf(pattern), widePage, small), 300) << "the page has 3125 columns and 12 500 dashes a line";
    }
}

TEST(PagePatternTest, ARangeOnAPageWithANegativeOriginCountsFromThePageTop)
{
    const QRectF page(-500, -700, 400, 300); // lines at -700 + 32k
    EXPECT_EQ(visibleLineRange(QRectF(-500, -600, 50, 40), page, 32.0), (LineRange{4, 4})) << "the line at -572 (-604 is 4 px above)";
    EXPECT_EQ(visibleLineRange(QRectF(-500, -800, 50, 110), page, 32.0), (LineRange{0, 0})) << "the page's first line at -700";
    EXPECT_TRUE(visibleLineRange(QRectF(-500, -900, 50, 100), page, 32.0).isEmpty()) << "above the page";
}

// --- the tile cache (Task 11, R5) ------------------------------------------------------------------

namespace {

QImage tileImage()
{
    QImage image(TileCache::kTileSize, TileCache::kTileSize, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::white);
    return image;
}

constexpr qsizetype kTileBytes = qsizetype(TileCache::kTileSize) * TileCache::kTileSize * 4;

/// Renders tile (tx, ty) through the cache and waits for it to be fresh.
void renderTile(TileCache &cache, int tx, int ty = 0)
{
    cache.request(tx, ty, [] { return tileImage(); });
    ASSERT_TRUE(QTest::qWaitFor([&] { return cache.find(tx, ty).fresh; }, 5000)) << "tile " << tx << "," << ty;
}

} // namespace

TEST(TileCacheTest, RendersOnceAndServesFromCache)
{
    QSemaphore gate;
    std::atomic<int> jobs{0};
    TileCache cache;
    const auto job = [&] {
        ++jobs;
        gate.acquire();
        return tileImage();
    };
    EXPECT_EQ(cache.find(0, 0).image, nullptr) << "nothing was rendered yet, and find never renders";
    EXPECT_TRUE(cache.request(0, 0, job));
    EXPECT_FALSE(cache.request(0, 0, job)) << "one render in flight per tile";
    EXPECT_TRUE(cache.isPending(0, 0));
    EXPECT_EQ(cache.pendingCount(), 1);
    gate.release();
    WAIT_TRUE(cache.find(0, 0).fresh, 5000);
    EXPECT_EQ(jobs.load(), 1);
    EXPECT_FALSE(cache.isPending(0, 0));
    const TileCache::Lookup hit = cache.find(0, 0);
    ASSERT_NE(hit.image, nullptr);
    EXPECT_EQ(hit.image->size(), QSize(TileCache::kTileSize, TileCache::kTileSize));
    EXPECT_EQ(jobs.load(), 1) << "a fresh tile is served without another job";
    EXPECT_EQ(cache.count(), 1);
    EXPECT_EQ(cache.bytes(), kTileBytes);
}

TEST(TileCacheTest, TheJobRunsOffTheUIThread)
{
    TileCache cache;
    QThread *jobThread = nullptr;
    QThread *readyThread = nullptr;
    QObject::connect(&cache, &TileCache::tileReady, &cache, [&] { readyThread = QThread::currentThread(); });
    QSignalSpy ready(&cache, &TileCache::tileReady);
    cache.request(0, 0, [&] {
        jobThread = QThread::currentThread();
        return tileImage();
    });
    WAIT_TRUE(ready.count() == 1, 5000);
    ASSERT_NE(jobThread, nullptr);
    EXPECT_NE(jobThread, QThread::currentThread()) << "rasterising a tile on the UI thread would stall the pen";
    EXPECT_EQ(readyThread, QThread::currentThread()) << "tileReady arrives on the thread of the cache";
    EXPECT_EQ(ready.at(0).at(0).toInt(), 0);
}

TEST(TileCacheTest, InvalidateMarksOnlyTouchedTilesStale)
{
    TileCache cache;
    for (int ty = 0; ty < 2; ++ty)
        for (int tx = 0; tx < 2; ++tx)
            renderTile(cache, tx, ty);
    ASSERT_EQ(cache.count(), 4);
    EXPECT_EQ(cache.staleCount(), 0);
    cache.invalidate(QRectF(10, 10, 100, 100)); // inside tile 0,0
    EXPECT_FALSE(cache.find(0, 0).fresh);
    EXPECT_NE(cache.find(0, 0).image, nullptr) << "the stale image is still returned: the canvas paints it blurry meanwhile";
    EXPECT_TRUE(cache.find(1, 0).fresh);
    EXPECT_TRUE(cache.find(0, 1).fresh);
    EXPECT_TRUE(cache.find(1, 1).fresh);
    EXPECT_EQ(cache.staleCount(), 1);
    cache.invalidate(QRectF(500, 500, 20, 20)); // across the corner of all four
    EXPECT_EQ(cache.staleCount(), 4);
    cache.invalidateAll();
    EXPECT_EQ(cache.staleCount(), 4);
}

TEST(TileCacheTest, AnInvalidateEndingOnATileEdgeLeavesTheNextTileFresh)
{
    TileCache cache;
    renderTile(cache, 0);
    renderTile(cache, 1);
    cache.invalidate(QRectF(0, 0, TileCache::kTileSize, 100));
    EXPECT_FALSE(cache.find(0, 0).fresh);
    EXPECT_TRUE(cache.find(1, 0).fresh);
}

TEST(TileCacheTest, AResultFinishedAfterAnInvalidateIsKeptButStale)
{
    // I4: the render started before the content changed again, but it holds everything before that. It is shown
    // (blurry stand-in) and asked for again; throwing it away would starve a tile a continuous eraser sweep keeps hitting.
    QSemaphore gate;
    TileCache cache;
    QSignalSpy ready(&cache, &TileCache::tileReady);
    cache.request(0, 0, [&] {
        gate.acquire();
        return tileImage();
    });
    cache.invalidate(QRectF(0, 0, 10, 10));
    gate.release();
    WAIT_TRUE(ready.count() == 1, 5000);
    EXPECT_FALSE(cache.isPending(0, 0));
    const TileCache::Lookup hit = cache.find(0, 0);
    EXPECT_NE(hit.image, nullptr) << "stored";
    EXPECT_FALSE(hit.fresh) << "but stale: the canvas requests it again";
    EXPECT_EQ(cache.staleCount(), 1);
    EXPECT_TRUE(cache.request(0, 0, [] { return tileImage(); }));
    WAIT_TRUE(cache.find(0, 0).fresh, 5000);
}

TEST(TileCacheTest, AResultFinishedAfterAScaleChangeIsDiscarded)
{
    for (const double dpr : {1.0, 2.0}) { // the zoom or the cell size changed: the render is of something else
        SCOPED_TRACE(dpr);
        QSemaphore gate;
        TileCache cache;
        QSignalSpy ready(&cache, &TileCache::tileReady);
        cache.request(0, 0, [&] {
            gate.acquire();
            return tileImage();
        });
        cache.setScale(2.0, dpr);
        gate.release();
        WAIT_TRUE(ready.count() == 1, 5000);
        EXPECT_FALSE(cache.isPending(0, 0));
        EXPECT_EQ(cache.find(0, 0).image, nullptr) << "never stored";
        EXPECT_EQ(cache.bytes(), 0);
        EXPECT_TRUE(cache.request(0, 0, [] { return tileImage(); }));
        WAIT_TRUE(cache.find(0, 0).fresh, 5000);
        EXPECT_EQ(cache.find(0, 0).scale, 2.0);
    }
}

TEST(TileCacheTest, ARenderAcrossAnInvalidateReplacesTheOldImageAndStaysStale)
{
    TileCache cache;
    renderTile(cache, 0);
    QSemaphore gate;
    QSignalSpy ready(&cache, &TileCache::tileReady);
    cache.invalidate(QRectF(0, 0, 1, 1));
    cache.request(0, 0, [&] {
        gate.acquire();
        QImage image = tileImage();
        image.fill(Qt::black);
        return image;
    });
    cache.invalidate(QRectF(0, 0, 1, 1)); // the content changed again while it rendered
    gate.release();
    WAIT_TRUE(ready.count() == 1, 5000);
    const TileCache::Lookup hit = cache.find(0, 0);
    EXPECT_FALSE(hit.fresh);
    ASSERT_NE(hit.image, nullptr);
    EXPECT_EQ(hit.image->pixel(5, 5), qRgb(0, 0, 0)) << "the newer render is what is shown meanwhile";
}

TEST(TileCacheTest, SetScaleKeepsTheImagesStaleAndSaysWhatZoomTheyHave)
{
    TileCache cache;
    renderTile(cache, 0);
    EXPECT_EQ(cache.find(0, 0).scale, 1.0);
    cache.setScale(2.0, 1.0);
    const TileCache::Lookup stale = cache.find(0, 0);
    EXPECT_FALSE(stale.fresh);
    ASSERT_NE(stale.image, nullptr) << "images stay until replaced";
    EXPECT_EQ(stale.scale, 1.0) << "and know what zoom they were rendered at";
    renderTile(cache, 0);
    EXPECT_EQ(cache.find(0, 0).scale, 2.0);
    cache.setScale(2.0, 1.0);
    EXPECT_TRUE(cache.find(0, 0).fresh) << "the same scale again changes nothing";
    cache.setScale(2.0, 2.0);
    EXPECT_EQ(cache.count(), 0) << "a cell is another size at another pixel ratio: the old images belong to no cell";
}

TEST(TileCacheTest, EvictsLeastRecentlyPaintedPastTheByteBudget)
{
    const qsizetype tile = TileCache::kTileSize * TileCache::kTileSize * 4;
    TileCache cache(3 * tile);
    QSignalSpy ready(&cache, &TileCache::tileReady);
    const auto render = [&](int tx) {
        cache.request(tx, 0, [] { return QImage(TileCache::kTileSize, TileCache::kTileSize, QImage::Format_ARGB32_Premultiplied); });
        WAIT_TRUE(cache.find(tx, 0).fresh, 5000);
    };
    render(1);
    render(2);
    render(3);
    ASSERT_NE(cache.find(1, 0).image, nullptr); // 1 is now the most recently painted; 2 the least
    render(4);
    EXPECT_EQ(cache.find(2, 0).image, nullptr);
    EXPECT_NE(cache.find(1, 0).image, nullptr);
    EXPECT_NE(cache.find(3, 0).image, nullptr);
    EXPECT_NE(cache.find(4, 0).image, nullptr);
    EXPECT_LE(cache.bytes(), 3 * tile);
}

TEST(TileCacheTest, LoweringTheBudgetEvictsAtOnce)
{
    TileCache cache(4 * kTileBytes);
    for (int tx = 1; tx <= 4; ++tx)
        renderTile(cache, tx);
    ASSERT_EQ(cache.count(), 4);
    ASSERT_NE(cache.find(1, 0).image, nullptr); // painted again: 1 is the most recent, then 4, 3, 2
    cache.setMaxBytes(2 * kTileBytes);
    EXPECT_EQ(cache.count(), 2);
    EXPECT_EQ(cache.maxBytes(), 2 * kTileBytes);
    EXPECT_LE(cache.bytes(), 2 * kTileBytes);
    EXPECT_NE(cache.find(1, 0).image, nullptr);
    EXPECT_NE(cache.find(4, 0).image, nullptr);
    EXPECT_EQ(cache.find(3, 0).image, nullptr);
    EXPECT_EQ(cache.find(2, 0).image, nullptr);
}

TEST(TileCacheTest, ATileWithARenderInFlightIsNeverEvicted)
{
    TileCache cache(2 * kTileBytes);
    renderTile(cache, 1); // the least recently painted of the lot
    renderTile(cache, 2);
    QSemaphore gate;
    QSignalSpy ready(&cache, &TileCache::tileReady);
    cache.request(1, 0, [&] { // re-rendering tile 1, whose stale image is the eviction candidate
        gate.acquire();
        return tileImage();
    });
    cache.invalidateAll();
    cache.setMaxBytes(kTileBytes);
    EXPECT_NE(cache.find(1, 0).image, nullptr) << "kept: its render is in flight";
    EXPECT_EQ(cache.find(2, 0).image, nullptr) << "evicted instead, though it was painted later";
    gate.release();
    WAIT_TRUE(ready.count() == 1, 5000);
}

TEST(TileCacheTest, TheDestructorWaitsForAJobThatHasStarted)
{
    std::atomic<bool> started{false}, finished{false};
    {
        TileCache cache;
        cache.request(0, 0, [&] {
            started = true;
            QThread::msleep(50);
            finished = true;
            return tileImage();
        });
        ASSERT_TRUE(QTest::qWaitFor([&] { return started.load(); }, 5000));
    }
    EXPECT_TRUE(finished.load()) << "a job in flight outlives nothing it uses";
}

TEST(TileCacheTest, TheDestructorDoesNotWaitBehindJobsThatNeverStarted)
{
    // I3: the pool is shared and first-in first-out; closing a tab after a fast scroll must not wait for its whole queue.
    QThreadPool *pool = QThreadPool::globalInstance();
    const int threads = pool->maxThreadCount();
    pool->setMaxThreadCount(1);
    QSemaphore gate;
    std::atomic<bool> running{false}, queuedRan{false};
    std::thread opener([&] { // lets the first job go once the cache is being destroyed
        while (!running.load())
            QThread::msleep(1);
        QThread::msleep(100);
        gate.release();
    });
    {
        TileCache cache;
        cache.request(0, 0, [&] {
            running = true;
            gate.acquire();
            return tileImage();
        });
        for (int tx = 1; tx <= 5; ++tx)
            cache.request(tx, 0, [&] {
                queuedRan = true;
                return tileImage();
            });
        ASSERT_TRUE(QTest::qWaitFor([&] { return running.load(); }, 5000));
    }
    opener.join();
    pool->setMaxThreadCount(threads);
    EXPECT_FALSE(queuedRan.load()) << "cancelled before they started: the destructor only waits for the one that runs";
}

TEST(InkCanvasTest, TheTileCacheFollowsTheSettingInMB)
{
    InkCanvas canvas;
    EXPECT_EQ(canvas.tileCacheBytes(), qsizetype(64) * 1024 * 1024) << "the default, R5";
    canvas.setTileCacheMB(16);
    EXPECT_EQ(canvas.tileCacheBytes(), qsizetype(16) * 1024 * 1024);
    canvas.setTileCacheMB(0);
    EXPECT_EQ(canvas.tileCacheBytes(), qsizetype(8) * 1024 * 1024) << "clamped to 8 MB";
    canvas.setTileCacheMB(5000);
    EXPECT_EQ(canvas.tileCacheBytes(), qsizetype(1024) * 1024 * 1024) << "and to 1024 MB";
}

TEST(InkCanvasTest, PaintNeverWaitsForATileRender)
{
    QSemaphore gate; // declared first: destroyed after the canvas, whose destructor waits for the blocked jobs
    CanvasFixture f;
    settledGrab(f.canvas); // the initial (empty) tiles are in
    auto stroke = std::make_shared<Stroke>();
    stroke->style = {8.0, Qt::black, PressureCurve::Const};
    stroke->points = {{100, 100, 1}, {200, 100, 1}};
    f.canvas.document().addStroke(stroke);
    f.canvas.document().commit();
    f.canvas.setTileJobHook([&gate] { gate.acquire(); });
    f.canvas.documentReplaced(); // every tile is stale: the next paint asks for them all again
    const QImage first = f.canvas.grab().toImage(); // returns at once: the tiles are rendering, blocked
    EXPECT_GT(f.canvas.pendingTiles(), 0);
    EXPECT_GT(first.pixelColor(150, 100).lightness(), 200) << "the page was painted, the stroke not yet";
    gate.release(100);
    WAIT_TRUE(f.canvas.pendingTiles() == 0, 5000);
    const QImage second = settledGrab(f.canvas);
    EXPECT_LT(second.pixelColor(150, 100).lightness(), 60) << "the tile arrived with the stroke";
}

TEST(InkCanvasTest, AFinishedStrokeStaysOnScreenUntilItsTilesArrive)
{
    QSemaphore gate;
    CanvasFixture f;
    StrokeStyle style;
    style.width = 8.0;
    style.color = Qt::black;
    f.canvas.setPenStyle(style);
    settledGrab(f.canvas);
    f.canvas.setTileJobHook([&gate] { gate.acquire(); });
    f.canvas.setRawInput(true);
    penStroke(&f.canvas, QPointF(100, 100), QPointF(200, 100), 1000);
    const QImage held = f.canvas.grab().toImage(); // pen-up, tile still rendering: the live image bridges the gap
    EXPECT_LT(held.pixelColor(150, 100).lightness(), 60) << "no frame without the stroke between pen-up and the new tile";
    gate.release(100);
    const QImage after = settledGrab(f.canvas);
    EXPECT_LT(after.pixelColor(150, 100).lightness(), 60);
}

TEST(InkCanvasTest, ZoomDuringPinchIsBlurryThenSettlesSharp)
{
    CanvasFixture f;
    StrokeStyle style;
    style.width = 8.0;
    style.color = Qt::black;
    f.canvas.setPenStyle(style);
    f.canvas.setRawInput(true);
    penStroke(&f.canvas, QPointF(100, 100), QPointF(200, 100), 1000);
    settledGrab(f.canvas);
    ASSERT_EQ(f.canvas.tilesScale(), 1.0);
    f.canvas.setZoom(2.0);
    EXPECT_EQ(f.canvas.tilesScale(), 1.0) << "the tiles keep their zoom for 400 ms after the last change";
    const QImage blurry = f.canvas.grab().toImage();
    EXPECT_EQ(f.canvas.pendingTiles(), 0) << "nothing is rendered for a zoom that may still change";
    EXPECT_LT(blurry.pixelColor(300, 200).lightness(), 100) << "the current tiles, scaled: the stroke is there at once";
    f.canvas.setZoom(3.0); // another pinch step restarts the timer
    EXPECT_EQ(f.canvas.tilesScale(), 1.0);
    WAIT_TRUE(f.canvas.tilesScale() == 3.0, 5000);
    const QImage sharp = settledGrab(f.canvas);
    EXPECT_EQ(f.canvas.pendingTiles(), 0);
    EXPECT_EQ(f.canvas.staleTileCount(), 0);
    EXPECT_LT(sharp.pixelColor(450, 300).lightness(), 60);
}

TEST(InkCanvasTest, AZoomBackToTheTilesScaleChangesNothing)
{
    CanvasFixture f;
    settledGrab(f.canvas);
    f.canvas.setZoom(2.0);
    f.canvas.setZoom(1.0);
    EXPECT_EQ(f.canvas.tilesScale(), 1.0);
    QTest::qWait(500);
    EXPECT_EQ(f.canvas.tilesScale(), 1.0);
    EXPECT_EQ(f.canvas.staleTileCount(), 0) << "the tiles were never made stale: the zoom they have is the zoom it is";
}

TEST(InkCanvasTest, EditsInvalidateOnlyTheirTiles)
{
    InkCanvas canvas;
    DocumentSettings settings;
    settings.format = {2000, 2000, 96};
    settings.extent = QRectF(0, 0, 2000, 2000);
    settings.layout = LayoutKind::Fixed;
    ASSERT_TRUE(canvas.document().setSettings(settings));
    canvas.documentReplaced();
    canvas.resize(canvas.sizeHint());
    canvas.show();
    ASSERT_TRUE(QTest::qWaitForWindowExposed(&canvas));
    settledGrab(canvas); // 16 tiles: 2000 / 512 = 3.9 each way
    ASSERT_EQ(canvas.staleTileCount(), 0);
    canvas.setRawInput(true);
    penStroke(&canvas, QPointF(50, 50), QPointF(120, 90), 1000); // inside the top-left tile
    const int stale = canvas.staleTileCount();
    EXPECT_GE(stale, 1);
    EXPECT_LE(stale, 4) << "the stroke's tiles, not the 16 of the page";
    settledGrab(canvas);
    EXPECT_EQ(canvas.staleTileCount(), 0);
    ASSERT_TRUE(canvas.undo());
    EXPECT_EQ(canvas.staleTileCount(), 16) << "undo and redo invalidate everything";
}

TEST(InkCanvasTest, ErasingInvalidatesTheErasedStrokesTiles)
{
    CanvasFixture f;
    f.canvas.setRawInput(true);
    penStroke(&f.canvas, QPointF(100, 100), QPointF(200, 100), 1000);
    settledGrab(f.canvas);
    ASSERT_EQ(f.canvas.staleTileCount(), 0);
    f.canvas.setTool(InkCanvas::Tool::Eraser);
    penStroke(&f.canvas, QPointF(150, 90), QPointF(150, 110), 2000);
    ASSERT_EQ(f.canvas.document().items().size(), 0u);
    EXPECT_GE(f.canvas.staleTileCount(), 1);
    const QImage frame = settledGrab(f.canvas);
    EXPECT_GT(frame.pixelColor(150, 100).lightness(), 200) << "the erased ink is gone from the tile";
}

TEST(InkCanvasTest, ABudgetSmallerThanTheWorkingSetSettlesInsteadOfThrashing)
{
    // I1: 16 visible tiles against an 8 MB budget used to evict wanted tiles, request them again, for ever.
    std::atomic<int> jobs{0};
    InkCanvas canvas;
    DocumentSettings settings;
    settings.format = {2000, 2000, 96};
    settings.extent = QRectF(0, 0, 2000, 2000);
    settings.layout = LayoutKind::Fixed;
    ASSERT_TRUE(canvas.document().setSettings(settings));
    canvas.documentReplaced();
    canvas.resize(canvas.sizeHint());
    canvas.setTileCacheMB(8);
    canvas.setTileJobHook([&jobs] { ++jobs; });
    canvas.show();
    ASSERT_TRUE(QTest::qWaitForWindowExposed(&canvas));
    settledGrab(canvas);
    const int atSettle = jobs.load();
    EXPECT_GE(atSettle, 16);
    for (int i = 0; i < 10; ++i) {
        canvas.grab();
        QTest::qWait(20);
    }
    EXPECT_EQ(jobs.load(), atSettle) << "no tile is rendered twice once they are all in";
    EXPECT_EQ(canvas.pendingTiles(), 0);
    EXPECT_EQ(canvas.tileCacheBytes(), qsizetype(8) * 1024 * 1024) << "the setting is reported as set; the room for the visible tiles is on top";
}

TEST(InkCanvasTest, AFinishedStrokeIsReleasedFromTheLiveImageWhenItsTilesArriveAndAtTheNextPenDown)
{
    QSemaphore gate;
    CanvasFixture f;
    StrokeStyle style;
    style.width = 8.0;
    style.color = Qt::black;
    f.canvas.setPenStyle(style);
    f.canvas.setRawInput(true);
    settledGrab(f.canvas);
    f.canvas.setTileJobHook([&gate] { gate.acquire(); });
    penStroke(&f.canvas, QPointF(100, 100), QPointF(200, 100), 1000);
    EXPECT_TRUE(f.canvas.liveHeld()) << "the live image bridges pen-up and the new tile";
    gate.release(100);
    WAIT_TRUE(!f.canvas.liveHeld(), 5000);
    EXPECT_EQ(f.canvas.pendingTiles(), 0);
    // A new stroke ends the hold of the one before it, whatever its tiles are doing.
    f.canvas.setTileJobHook([&gate] { gate.acquire(); });
    penStroke(&f.canvas, QPointF(100, 200), QPointF(200, 200), 2000);
    ASSERT_TRUE(f.canvas.liveHeld());
    tablet(&f.canvas, QEvent::TabletPress, QPointF(50, 50), 0.5, 3000);
    EXPECT_FALSE(f.canvas.liveHeld());
    tablet(&f.canvas, QEvent::TabletRelease, QPointF(50, 50), 0.5, 3010, Qt::NoButton);
    gate.release(100);
    settledGrab(f.canvas);
}

TEST(InkCanvasTest, ATileDiscardedByAZoomSettleIsRequestedAgainAtTheNewZoom)
{
    QSemaphore gate;
    CanvasFixture f;
    f.canvas.setTileJobHook([&gate] { gate.acquire(); });
    f.canvas.documentReplaced(); // everything stale: the paint requests tiles, all blocked
    f.canvas.grab();
    ASSERT_GT(f.canvas.pendingTiles(), 0);
    f.canvas.setZoom(2.0);
    WAIT_TRUE(f.canvas.tilesScale() == 2.0, 5000); // the settle rescales while they are in flight: their results are of the old zoom
    gate.release(1000);
    settledGrab(f.canvas);
    EXPECT_EQ(f.canvas.staleTileCount(), 0);
    EXPECT_EQ(f.canvas.tilesScale(), 2.0);
}

TEST(InkCanvasTest, AnEditDuringAPinchInvalidatesTheTilesUnderIt)
{
    InkCanvas canvas;
    DocumentSettings settings;
    settings.format = {2000, 2000, 96};
    settings.extent = QRectF(0, 0, 2000, 2000);
    settings.layout = LayoutKind::Fixed;
    ASSERT_TRUE(canvas.document().setSettings(settings));
    canvas.documentReplaced();
    canvas.resize(canvas.sizeHint());
    canvas.show();
    ASSERT_TRUE(QTest::qWaitForWindowExposed(&canvas));
    settledGrab(canvas);
    ASSERT_EQ(canvas.staleTileCount(), 0);
    canvas.setZoom(2.0); // the pinch is pending: widget px are twice the tile space
    ASSERT_EQ(canvas.tilesScale(), 1.0);
    canvas.setRawInput(true);
    penStroke(&canvas, QPointF(200, 200), QPointF(300, 240), 1000); // document (100,100)-(150,120): the top-left tile
    EXPECT_EQ(canvas.staleTileCount(), 1) << "mapped back to the tiles' zoom, not 16 and not the wrong tile";
}

namespace {
void expectTilesMatchADirectRender()
{
    InkCanvas canvas;
    DocumentSettings settings;
    settings.layout = LayoutKind::Fixed;
    settings.format = {1100, 1100, 96};
    settings.extent = QRectF(0, 0, 1100, 1100);
    settings.background.pattern = Pattern::Grid;
    settings.background.patternSize = QSizeF(24, 24);
    ASSERT_TRUE(canvas.document().setSettings(settings));
    auto stroke = std::make_shared<Stroke>();
    stroke->style = {6.0, QColor(0x20, 0x40, 0xc0), PressureCurve::Const};
    stroke->points = {{40, 500, 1}, {300, 515, 1}, {600, 510, 1}, {1000, 520, 1}};
    canvas.document().addStroke(stroke);
    canvas.document().commit();
    canvas.documentReplaced();
    canvas.resize(canvas.sizeHint());
    const QImage tiled = settledGrab(canvas);
    QImage direct(tiled.size(), QImage::Format_ARGB32_Premultiplied);
    direct.setDevicePixelRatio(tiled.devicePixelRatio());
    direct.fill(settings.background.color);
    {
        QPainter painter(&direct);
        painter.setRenderHint(QPainter::Antialiasing);
        paintPattern(painter, settings.background, settings.extent, settings.extent.topLeft(), settings.extent, 1.0);
        paintStroke(painter, *stroke);
    }
    // Not bit for bit: a tile paints under another translation, so a coverage value may round a few levels apart (4 seen at DPR 2, 0 at 1 and 1.5). A seam
    // would be a missing line or a doubled edge, 50 levels and up.
    const QImage a = tiled.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    int worst = 0;
    for (int y = 0; y < a.height(); ++y)
        for (int x = 0; x < a.width(); ++x) {
            const QRgb p = a.pixel(x, y), q = direct.pixel(x, y);
            worst = std::max({worst, std::abs(qRed(p) - qRed(q)), std::abs(qGreen(p) - qGreen(q)), std::abs(qBlue(p) - qBlue(q))});
        }
    EXPECT_EQ(a.size(), direct.size());
    EXPECT_LE(worst, 5) << "tiled against direct";
}
} // namespace

TEST(InkCanvasTest, TilesJoinWithoutASeamAndMatchADirectRender) { expectTilesMatchADirectRender(); }

// Runs in the ink_dpr2 ctest entry too (QT_SCALE_FACTOR=2): a cell is 256 logical px there.
// A text box's picture comes from a widget grab, which is at the screen's pixel ratio: the whole of it fills the box.
TEST(InkCanvasDprTest, ATextBoxPictureAtTheScreensPixelRatioFillsItsBox)
{
    CanvasFixture f;
    const double ratio = f.canvas.devicePixelRatio();
    f.canvas.setTextRenderer([ratio](const QString &, const QSizeF &size) {
        QPixmap pixmap((size * ratio).toSize());
        pixmap.setDevicePixelRatio(ratio);
        pixmap.fill(QColor(220, 0, 0));
        QPainter painter(&pixmap);
        painter.fillRect(QRectF(size.width() / 2, 0, size.width() / 2, size.height()), QColor(0, 0, 220)); // the right half, in logical px
        return pixmap;
    });
    f.canvas.document().addItem([] { Item box; box.type = Item::Type::TextBox; box.textBox = std::make_shared<TextBox>(TextBox{QRectF(40, 60, 200, 100), QStringLiteral("x")}); return box; }());
    f.canvas.documentReplaced();
    const QImage frame = settledGrab(f.canvas);
    const auto at = [&](double x, double y) { return frame.pixelColor(int(x * ratio), int(y * ratio)); };
    EXPECT_EQ(at(50, 70), QColor(220, 0, 0)) << "the left edge of the box";
    EXPECT_EQ(at(130, 150), QColor(220, 0, 0)) << "just left of the middle, bottom";
    EXPECT_EQ(at(150, 70), QColor(0, 0, 220)) << "just right of the middle";
    EXPECT_EQ(at(230, 150), QColor(0, 0, 220)) << "the bottom-right corner area of the box";
    EXPECT_NE(at(250, 150), QColor(0, 0, 220)) << "and nothing past it";
}

TEST(InkCanvasDprTest, TilesJoinWithoutASeamAtTheScreensPixelRatio) { expectTilesMatchADirectRender(); }

TEST(InkCanvasTest, ThePatternOfAPagedLayoutRestartsOnEveryPageInsideTheTiles)
{
    InkCanvas canvas;
    DocumentSettings s;
    s.format = {300, 700, 96};
    s.extent = QRectF(0, 0, 300, 1400); // two pages: the second starts inside the second tile row
    s.background.pattern = Pattern::Lines;
    s.background.patternSize = QSizeF(32, 32);
    ASSERT_TRUE(canvas.document().setSettings(s));
    canvas.documentReplaced();
    canvas.resize(canvas.sizeHint());
    const QImage shot = settledGrab(canvas);
    for (int page = 0; page < 2; ++page) {
        SCOPED_TRACE(page);
        const int top = int(canvas.toWidget(QPointF(0, 700.0 * page)).y());
        for (int line : {1, 9, 10, 16, 20}) // lines 32 px apart from this page's top, across the 512 px seams
            EXPECT_TRUE(hasInk(shot, QRect(10, top + 32 * line - 1, 280, 4))) << "line " << line;
        EXPECT_FALSE(hasInk(shot, QRect(10, top + 33 + 32 * 3 - 30, 280, 20))) << "none between the lines";
    }
}


namespace {

/// Ten points 10 px apart along y = 0 (x = 0, 10 ... 90), width 2 at pressure 0.5: segments are 0.5 px either side.
Stroke tenPointLine()
{
    Stroke stroke;
    stroke.style.width = 2.0;
    stroke.style.color = QColor(0x20, 0x40, 0x80);
    for (int i = 0; i < 10; ++i)
        stroke.points.push_back({10.0 * i, 0.0, 0.5});
    return stroke;
}

QPainterPath circleAt(double x, double y, double r)
{
    QPainterPath path;
    path.addEllipse(QPointF(x, y), r, r);
    return path;
}

} // namespace

TEST(InkToolsTest, SplitCutsTheMiddleAndKeepsBothEnds)
{
    const Stroke stroke = tenPointLine();
    const SplitResult result = splitStroke(stroke, circleAt(45, 0, 7)); // over points 4 and 5
    ASSERT_TRUE(result.hit);
    ASSERT_EQ(result.pieces.size(), 2u);
    ASSERT_EQ(result.pieces[0]->points.size(), 4u);
    ASSERT_EQ(result.pieces[1]->points.size(), 4u);
    EXPECT_EQ(result.pieces[0]->points.front().x, 0.0);
    EXPECT_EQ(result.pieces[0]->points.back().x, 30.0);
    EXPECT_EQ(result.pieces[1]->points.front().x, 60.0);
    EXPECT_EQ(result.pieces[1]->points.back().x, 90.0);
    for (const auto &piece : result.pieces) {
        EXPECT_EQ(piece->style, stroke.style);
        EXPECT_EQ(piece->points.front().pressure, 0.5);
    }
}

TEST(InkToolsTest, SplitAtTheEndTrimsInPlace)
{
    const SplitResult result = splitStroke(tenPointLine(), circleAt(90, 0, 3)); // over point 9
    ASSERT_TRUE(result.hit);
    ASSERT_EQ(result.pieces.size(), 1u);
    ASSERT_EQ(result.pieces[0]->points.size(), 9u);
    EXPECT_EQ(result.pieces[0]->points.front().x, 0.0);
    EXPECT_EQ(result.pieces[0]->points.back().x, 80.0);
}

TEST(InkToolsTest, SplitOfATwoPointStrokeRemovesIt)
{
    const auto two = line(0, 0, 20, 0);
    const SplitResult result = splitStroke(*two, circleAt(10, 0, 3));
    EXPECT_TRUE(result.hit);
    EXPECT_TRUE(result.pieces.empty());
    Stroke dot;
    dot.points = {{5, 5, 0.5}};
    const SplitResult tap = splitStroke(dot, circleAt(5, 5, 3));
    EXPECT_TRUE(tap.hit) << "a tap is one dot: taken whole";
    EXPECT_TRUE(tap.pieces.empty());
}

TEST(InkToolsTest, SplitMissingReturnsNoHit)
{
    const SplitResult result = splitStroke(tenPointLine(), circleAt(45, 40, 7));
    EXPECT_FALSE(result.hit);
    EXPECT_TRUE(result.pieces.empty());
    EXPECT_FALSE(splitStroke(Stroke{}, circleAt(0, 0, 5)).hit) << "no points, nothing to cut";
}

TEST(InkToolsTest, SplitDropsAnEdgeSegmentThatLeavesOnePoint)
{
    // Points 0 and 1 survive on the left (one segment = a two-point stroke), the hit is at 2..3.
    const SplitResult result = splitStroke(tenPointLine(), circleAt(25, 0, 7));
    ASSERT_TRUE(result.hit);
    ASSERT_EQ(result.pieces.size(), 2u);
    EXPECT_EQ(result.pieces[0]->points.size(), 2u);
    EXPECT_EQ(result.pieces[0]->points.back().x, 10.0);
    EXPECT_EQ(result.pieces[1]->points.front().x, 40.0);
    EXPECT_EQ(result.pieces[1]->points.back().x, 90.0);
}

TEST(InkToolsTest, SplitSeesAnEraserInsideAWideStroke)
{
    Stroke stroke = tenPointLine();
    stroke.style.width = 40.0; // the stroke is 20 px wide on each side: the eraser circle sits wholly inside it
    stroke.points.resize(3);
    const SplitResult result = splitStroke(stroke, circleAt(10, 0, 2));
    EXPECT_TRUE(result.hit);
}

TEST(InkToolsTest, SplitSurvivesHostileCoordinates)
{
    Stroke stroke = tenPointLine();
    stroke.points[5].x = std::numeric_limits<double>::quiet_NaN();
    stroke.points[6].y = std::numeric_limits<double>::infinity();
    const SplitResult result = splitStroke(stroke, circleAt(15, 0, 3)); // returns, whatever it decides
    EXPECT_TRUE(result.hit);
    for (const auto &piece : result.pieces)
        EXPECT_GE(piece->points.size(), 2u);
}

TEST(InkCanvasTest, SplitEraserLeavesTwoStrokesAndUndoRestoresOne)
{
    CanvasFixture f;
    f.canvas.setRawInput(true);
    f.canvas.setEraserMode(InkCanvas::EraserMode::Split);
    penStroke(&f.canvas, QPointF(20, 150), QPointF(380, 150), 1000);
    ASSERT_EQ(f.canvas.document().items().size(), 1u);
    const qint64 originalT = f.canvas.document().items()[0].t;
    const StrokeStyle style = f.canvas.document().items()[0].stroke->style;
    f.canvas.setTool(InkCanvas::Tool::Eraser);
    penStroke(&f.canvas, QPointF(200, 120), QPointF(200, 180), 2000);
    const auto &items = f.canvas.document().items();
    ASSERT_EQ(items.size(), 2u);
    EXPECT_EQ(items[0].t, originalT) << "the first piece keeps the item, so the undo diff is small";
    EXPECT_GT(items[1].t, originalT);
    EXPECT_EQ(items[0].layer, items[1].layer);
    EXPECT_EQ(items[0].stroke->style, style);
    EXPECT_EQ(items[1].stroke->style, style);
    EXPECT_LT(items[0].stroke->points.back().x, 200.0);
    EXPECT_GT(items[1].stroke->points.front().x, 200.0);
    EXPECT_GT(items[1].stroke->points.front().x - items[0].stroke->points.back().x, 8.0) << "a gap the eraser's width cut";
    ASSERT_TRUE(f.canvas.undo());
    EXPECT_EQ(f.canvas.document().items().size(), 1u);
    ASSERT_TRUE(f.canvas.redo());
    EXPECT_EQ(f.canvas.document().items().size(), 2u);
}

TEST(InkCanvasTest, TheSplitEraserShowsTheGapAndTheEndsAfterTheTilesSettle)
{
    CanvasFixture f;
    f.canvas.setRawInput(true);
    StrokeStyle style;
    style.width = 6.0;
    f.canvas.setPenStyle(style);
    penStroke(&f.canvas, QPointF(20, 150), QPointF(380, 150), 1000);
    settledGrab(f.canvas);
    ASSERT_EQ(f.canvas.staleTileCount(), 0);
    f.canvas.setEraserMode(InkCanvas::EraserMode::Split);
    f.canvas.setTool(InkCanvas::Tool::Eraser);
    penStroke(&f.canvas, QPointF(200, 120), QPointF(200, 180), 2000);
    ASSERT_EQ(f.canvas.document().items().size(), 2u);
    EXPECT_GE(f.canvas.staleTileCount(), 1);
    const QImage frame = settledGrab(f.canvas);
    EXPECT_LT(frame.pixelColor(100, 150).lightness(), 100) << "the left end is still ink";
    EXPECT_LT(frame.pixelColor(300, 150).lightness(), 100) << "the right end is still ink";
    EXPECT_GT(frame.pixelColor(200, 150).lightness(), 200) << "the cut is gone from the tile";
}

TEST(InkCanvasTest, TheWholeStrokeModeStaysTheDefaultAndTakesTheWholeStroke)
{
    CanvasFixture f;
    EXPECT_EQ(f.canvas.eraserMode(), InkCanvas::EraserMode::Stroke);
    f.canvas.setRawInput(true);
    penStroke(&f.canvas, QPointF(20, 150), QPointF(380, 150), 1000);
    f.canvas.setTool(InkCanvas::Tool::Eraser);
    penStroke(&f.canvas, QPointF(200, 120), QPointF(200, 180), 2000);
    EXPECT_TRUE(f.canvas.document().items().empty());
}

TEST(InkCanvasTest, ACancelledSplitEraseRestoresEveryStrokeExactly)
{
    CanvasFixture f;
    f.canvas.setRawInput(true);
    penStroke(&f.canvas, QPointF(20, 150), QPointF(380, 150), 1000);
    penStroke(&f.canvas, QPointF(20, 100), QPointF(380, 100), 1500);
    const std::vector<Item> before = f.canvas.document().items();
    ASSERT_EQ(before.size(), 2u);
    f.canvas.setEraserMode(InkCanvas::EraserMode::Split);
    f.canvas.setTool(InkCanvas::Tool::Eraser);
    tablet(&f.canvas, QEvent::TabletPress, QPointF(200, 80), 0.5, 2000);
    tablet(&f.canvas, QEvent::TabletMove, QPointF(200, 180), 0.5, 2005); // cuts both
    ASSERT_EQ(f.canvas.document().items().size(), 4u);
    f.canvas.documentReplaced(); // what a reload or a document swap does mid-erase: cancel
    EXPECT_FALSE(f.canvas.isDrawing());
    const auto &after = f.canvas.document().items();
    ASSERT_EQ(after.size(), 2u);
    for (size_t i = 0; i < 2; ++i) {
        EXPECT_EQ(after[i].t, before[i].t);
        EXPECT_EQ(after[i].stroke, before[i].stroke) << "the very same stroke, not a copy";
    }
    EXPECT_EQ(f.canvas.document().itemsIntersecting(QRectF(190, 140, 20, 20)).size(), 1u) << "the grid knows the whole stroke again";
}

TEST(InkCanvasTest, TheSplitEraserTakesShapesWhole)
{
    CanvasFixture f;
    auto shape = std::make_shared<Shape>();
    shape->kind = ShapeKind::Line;
    shape->points = {QPointF(20, 150), QPointF(380, 150)};
    Item item;
    item.type = Item::Type::Shape;
    item.shape = shape;
    f.canvas.document().addItem(item);
    f.canvas.document().commit();
    f.canvas.setEraserMode(InkCanvas::EraserMode::Split);
    f.canvas.setTool(InkCanvas::Tool::Eraser);
    penStroke(&f.canvas, QPointF(200, 120), QPointF(200, 180), 2000);
    EXPECT_TRUE(f.canvas.document().items().empty()) << "split applies to strokes only";
}

TEST(InkCanvasTest, TheEraserModeHasAKeyAndASignalAndReadOnlyIgnoresIt)
{
    CanvasFixture f;
    QSignalSpy changed(&f.canvas, &InkCanvas::eraserModeChanged);
    QTest::keyClick(&f.canvas, Qt::Key_E, Qt::ShiftModifier);
    EXPECT_EQ(f.canvas.eraserMode(), InkCanvas::EraserMode::Split);
    EXPECT_EQ(f.canvas.tool(), InkCanvas::Tool::Eraser) << "the key also picks the eraser";
    QTest::keyClick(&f.canvas, Qt::Key_E, Qt::ShiftModifier);
    EXPECT_EQ(f.canvas.eraserMode(), InkCanvas::EraserMode::Stroke);
    EXPECT_EQ(changed.count(), 2);
    f.canvas.setEraserMode(InkCanvas::EraserMode::Stroke);
    EXPECT_EQ(changed.count(), 2) << "no change, no signal";
    f.canvas.setReadOnly(true);
    QTest::keyClick(&f.canvas, Qt::Key_E, Qt::ShiftModifier);
    EXPECT_EQ(f.canvas.eraserMode(), InkCanvas::EraserMode::Stroke);
}


// --- Task 13: lasso and rectangle selection -------------------------------------------------------------

namespace {

std::shared_ptr<const Stroke> pointsStroke(std::initializer_list<QPointF> points, double width = 4.0)
{
    auto stroke = std::make_shared<Stroke>();
    stroke->style.width = width;
    for (const QPointF &p : points)
        stroke->points.push_back({p.x(), p.y(), 0.5});
    return stroke;
}

QPainterPath polygonPath(const QPolygonF &polygon)
{
    QPainterPath path;
    path.addPolygon(polygon);
    path.closeSubpath();
    return path;
}

/// A pen drag along `path` in widget px: press at the first point, a move to each of the others, release at the last.
void penDrag(QWidget *w, const QList<QPointF> &path, qint64 ms, Qt::MouseButtons buttons = Qt::LeftButton)
{
    tablet(w, QEvent::TabletPress, path.first(), 0.5, ms, buttons);
    for (int i = 1; i < path.size(); ++i)
        tablet(w, QEvent::TabletMove, path[i], 0.5, ms + i * 5, buttons);
    tablet(w, QEvent::TabletRelease, path.last(), 0.5, ms + path.size() * 5, Qt::NoButton);
}

const QList<QPointF> kAroundFirstStroke = {{10, 30}, {140, 30}, {140, 120}, {10, 120}, {10, 32}};

} // namespace

TEST(InkToolsTest, SelectItemsByLassoPolygon)
{
    InkDocument doc;
    const qint64 a = doc.addStroke(pointsStroke({{20, 20}, {40, 40}, {60, 60}}));
    const qint64 b = doc.addStroke(pointsStroke({{200, 200}, {240, 240}}));
    const qint64 far = doc.addStroke(pointsStroke({{10, 10}, {300, 10}})); // two points only, both outside the lasso
    Item box;
    box.type = Item::Type::TextBox;
    box.textBox = std::make_shared<TextBox>(TextBox{QRectF(30, 70, 40, 20), QStringLiteral("hi")});
    const qint64 text = doc.addItem(box);
    Item unknown;
    unknown.type = Item::Type::Unknown;
    unknown.raw = QJsonObject{{QStringLiteral("type"), QStringLiteral("future")}, {QStringLiteral("rect"), QJsonArray{30, 70, 40, 20}}};
    const qint64 mystery = doc.addItem(unknown);
    const QPainterPath region = polygonPath(QPolygonF({{0, 15}, {100, 15}, {100, 100}, {0, 100}}));
    EXPECT_EQ(selectItems(doc, region), (std::vector<qint64>{a, text})) << "document order; b is outside, far has no point inside, the unknown item is never selected";
    EXPECT_EQ(selectItems(doc, QPainterPath()), std::vector<qint64>{});
    QPainterPath rect; // the rectangle select hands over a rect path
    rect.addRect(QRectF(190, 190, 70, 70));
    EXPECT_EQ(selectItems(doc, rect), std::vector<qint64>{b});
    (void)far;
    (void)mystery;
}

TEST(InkToolsTest, TransformMovesAndScalesAStroke)
{
    Item item;
    item.type = Item::Type::Stroke;
    item.t = 7;
    item.layer = Layer::Highlighter;
    item.stroke = pointsStroke({{10, 10}, {20, 30}}, 4.0);
    item.raw = QJsonObject{{QStringLiteral("mark"), 42}};
    QTransform scale;
    scale.scale(2.0, 4.0);
    const Item scaled = transformItem(item, scale);
    ASSERT_TRUE(scaled.stroke);
    EXPECT_NE(scaled.stroke.get(), item.stroke.get()) << "a new payload: snapshots share the old one";
    EXPECT_EQ(scaled.t, 7);
    EXPECT_EQ(scaled.layer, Layer::Highlighter);
    EXPECT_EQ(scaled.raw.value(QStringLiteral("mark")).toInt(), 42);
    EXPECT_NEAR(scaled.stroke->points[1].x, 40.0, 1e-9);
    EXPECT_NEAR(scaled.stroke->points[1].y, 120.0, 1e-9);
    EXPECT_NEAR(scaled.stroke->style.width, 12.0, 1e-9) << "the mean of sx and sy (Rnote)";
    EXPECT_DOUBLE_EQ(scaled.stroke->points[1].pressure, 0.5);
    EXPECT_NEAR(item.stroke->points[1].x, 20.0, 1e-9) << "the original is untouched";
    const Item moved = transformItem(item, QTransform::fromTranslate(30, -5));
    EXPECT_NEAR(moved.stroke->points[0].x, 40.0, 1e-9);
    EXPECT_NEAR(moved.stroke->points[0].y, 5.0, 1e-9);
    EXPECT_NEAR(moved.stroke->style.width, 4.0, 1e-9);
    Item box;
    box.type = Item::Type::TextBox;
    box.textBox = std::make_shared<TextBox>(TextBox{QRectF(10, 10, 20, 10), QStringLiteral("x")});
    EXPECT_EQ(transformItem(box, scale).textBox->rect, QRectF(20, 40, 40, 40));
    Item shape;
    shape.type = Item::Type::Shape;
    auto shapeData = std::make_shared<Shape>();
    shapeData->points = {{0, 0}, {10, 10}};
    shapeData->style.width = 2.0;
    shape.shape = shapeData;
    const Item shaped = transformItem(shape, scale);
    EXPECT_EQ(shaped.shape->points[1], QPointF(20, 40));
    EXPECT_NEAR(shaped.shape->style.width, 6.0, 1e-9);
    Item unknown;
    unknown.type = Item::Type::Unknown;
    unknown.raw = QJsonObject{{QStringLiteral("type"), QStringLiteral("future")}};
    EXPECT_EQ(transformItem(unknown, scale).raw, unknown.raw);
}

TEST(InkToolsTest, ClipboardRoundTripKeepsStyleAndRaw)
{
    const QByteArray file = R"({"format":"notolith-ink","version":1,"items":[
        {"t":5,"type":"stroke","layer":"user","mark":"keep","style":{"width":3,"color":"#336699","pressureCurve":"sqrt","extra":1},"points":[1,2,0.5,3,4,0.25]},
        {"t":9,"type":"textbox","layer":"user","rect":[10,20,30,40],"markdown":"hello"}]})";
    InkDocument doc;
    ASSERT_TRUE(readInk(file, doc).ok);
    ASSERT_EQ(doc.items().size(), 2u);
    const QByteArray bytes = itemsToClipboardJSON({&doc.items()[0], &doc.items()[1]});
    bool ok = false;
    const std::vector<Item> back = itemsFromClipboardJSON(bytes, &ok);
    ASSERT_TRUE(ok);
    ASSERT_EQ(back.size(), 2u);
    ASSERT_EQ(back[0].type, Item::Type::Stroke);
    EXPECT_EQ(back[0].stroke->style, doc.items()[0].stroke->style);
    EXPECT_EQ(back[0].stroke->style.pressureCurve, PressureCurve::Sqrt);
    EXPECT_EQ(back[0].stroke->points.size(), 2u);
    EXPECT_NEAR(back[0].stroke->points[1].pressure, 0.25, 1e-9);
    EXPECT_EQ(back[0].raw.value(QStringLiteral("mark")).toString(), QStringLiteral("keep"));
    EXPECT_EQ(back[0].raw.value(QStringLiteral("style")).toObject().value(QStringLiteral("extra")).toInt(), 1) << "unknown keys inside style";
    EXPECT_EQ(back[0].t, 5);
    ASSERT_EQ(back[1].type, Item::Type::TextBox);
    EXPECT_EQ(back[1].textBox->markdown, QStringLiteral("hello"));
    EXPECT_EQ(back[1].textBox->rect, QRectF(10, 20, 30, 40));
    for (const QByteArray &bad : {QByteArray("not json"), QByteArray("{}"), QByteArray(R"({"format":"other","items":[]})"),
                                  QByteArray(R"({"format":"notolith-ink-clip","items":[{"type":"stroke"}]})")}) {
        ok = true;
        EXPECT_TRUE(itemsFromClipboardJSON(bad, &ok).empty()) << bad.toStdString();
        EXPECT_FALSE(ok) << bad.toStdString();
    }
}

TEST(InkCanvasTest, LassoSelectMoveAndDelete)
{
    CanvasFixture f;
    f.canvas.setPenStyle([] { StrokeStyle s; s.width = 6.0; return s; }());
    penStroke(&f.canvas, QPointF(20, 40), QPointF(120, 100), 1000);
    penStroke(&f.canvas, QPointF(200, 200), QPointF(300, 260), 2000);
    ASSERT_EQ(f.canvas.document().items().size(), 2u);
    const qint64 tA = f.canvas.document().items()[0].t;
    const std::vector<StrokePoint> before = f.canvas.document().items()[0].stroke->points;
    const std::vector<StrokePoint> otherBefore = f.canvas.document().items()[1].stroke->points;
    f.canvas.setTool(InkCanvas::Tool::Lasso);
    QSignalSpy edited(&f.canvas, &InkCanvas::documentEdited);
    penDrag(&f.canvas, kAroundFirstStroke, 3000);
    EXPECT_EQ(f.canvas.selection(), std::vector<qint64>{tA});
    EXPECT_EQ(edited.count(), 0) << "selecting is not an edit";
    EXPECT_EQ(f.canvas.document().items().size(), 2u);
    EXPECT_TRUE(f.canvas.selectionBounds().contains(QPointF(70, 70)));
    EXPECT_FALSE(f.canvas.isDrawing());
    penStroke(&f.canvas, QPointF(70, 70), QPointF(100, 70), 4000); // inside the selection: a move by (30, 0)
    ASSERT_EQ(f.canvas.document().items().size(), 2u);
    const Item &moved = *f.canvas.document().item(tA);
    ASSERT_EQ(moved.stroke->points.size(), before.size());
    for (size_t i = 0; i < before.size(); ++i) {
        EXPECT_NEAR(moved.stroke->points[i].x, before[i].x + 30.0, 1e-6);
        EXPECT_NEAR(moved.stroke->points[i].y, before[i].y, 1e-6);
    }
    const Item &other = f.canvas.document().items()[1];
    for (size_t i = 0; i < otherBefore.size(); ++i)
        EXPECT_NEAR(other.stroke->points[i].x, otherBefore[i].x, 1e-9) << "the unselected stroke stays";
    EXPECT_EQ(edited.count(), 1) << "one move, one history step";
    EXPECT_EQ(f.canvas.selection(), std::vector<qint64>{tA}) << "the selection follows the item";
    QTest::keyClick(&f.canvas, Qt::Key_Delete);
    EXPECT_EQ(f.canvas.document().items().size(), 1u);
    EXPECT_TRUE(f.canvas.selection().empty());
    EXPECT_EQ(edited.count(), 2);
    ASSERT_TRUE(f.canvas.undo());
    EXPECT_EQ(f.canvas.document().items().size(), 2u);
    ASSERT_TRUE(f.canvas.undo());
    ASSERT_EQ(f.canvas.document().items().size(), 2u);
    const Item *original = f.canvas.document().item(tA);
    ASSERT_TRUE(original);
    for (size_t i = 0; i < before.size(); ++i)
        EXPECT_NEAR(original->stroke->points[i].x, before[i].x, 1e-9) << "back at the original place";
}

TEST(InkCanvasTest, ResizingFromACornerHandleScalesAboutTheOppositeCorner)
{
    CanvasFixture f;
    f.canvas.setPenStyle([] { StrokeStyle s; s.width = 6.0; return s; }());
    penStroke(&f.canvas, QPointF(20, 40), QPointF(120, 100), 1000);
    f.canvas.setTool(InkCanvas::Tool::Lasso);
    penDrag(&f.canvas, kAroundFirstStroke, 3000);
    ASSERT_EQ(f.canvas.selection().size(), 1u);
    const QRectF box = f.canvas.selectionBounds();
    const double width = f.canvas.document().items()[0].stroke->style.width;
    penStroke(&f.canvas, f.canvas.toWidget(box.bottomRight()), f.canvas.toWidget(box.bottomRight()) + QPointF(51, 31), 4000);
    const QRectF now = f.canvas.selectionBounds();
    // The box is the points' box grown by half the width, and the width scales too: the corner moves by a fraction of a px.
    EXPECT_NEAR(now.left(), box.left(), 0.5) << "the opposite corner stays";
    EXPECT_NEAR(now.top(), box.top(), 0.5);
    EXPECT_NEAR(now.right(), box.right() + 51.0, 1.0);
    EXPECT_NEAR(now.bottom(), box.bottom() + 31.0, 1.0);
    EXPECT_GT(f.canvas.document().items()[0].stroke->style.width, width) << "the width scales with the box";
    EXPECT_EQ(f.canvas.document().items().size(), 1u);
    ASSERT_TRUE(f.canvas.undo());
    EXPECT_NEAR(f.canvas.document().items()[0].stroke->style.width, width, 1e-9) << "one step";
}

TEST(InkCanvasTest, RectangleSelectionSelectsByTheBoxAndEscapeClears)
{
    CanvasFixture f;
    penStroke(&f.canvas, QPointF(20, 40), QPointF(120, 100), 1000);
    penStroke(&f.canvas, QPointF(200, 200), QPointF(300, 260), 2000);
    f.canvas.setTool(InkCanvas::Tool::Lasso);
    f.canvas.setSelectShape(InkCanvas::SelectShape::Rect);
    penDrag(&f.canvas, {{180, 180}, {320, 280}}, 3000); // two points: a box, not a path
    ASSERT_EQ(f.canvas.selection().size(), 1u);
    EXPECT_EQ(f.canvas.selection()[0], f.canvas.document().items()[1].t);
    QTest::keyClick(&f.canvas, Qt::Key_Escape);
    EXPECT_TRUE(f.canvas.selection().empty());
    penDrag(&f.canvas, {{5, 5}, {390, 290}}, 4000);
    EXPECT_EQ(f.canvas.selection().size(), 2u);
    penDrag(&f.canvas, {{350, 20}, {360, 22}}, 5000); // a drag over nothing selects nothing
    EXPECT_TRUE(f.canvas.selection().empty());
}

TEST(InkCanvasTest, MovingASelectionRepaintsTheOldAndTheNewPlace)
{
    // 1200 px wide: the stroke starts in the first 512 px tile and is moved into the second, so dropping the invalidation of
    // the old bounds or of the new bounds each fails on its own.
    CanvasFixture f;
    DocumentSettings wide = f.canvas.document().settings();
    wide.format = {1200, 300, 96};
    wide.extent = QRectF(0, 0, 1200, 300);
    ASSERT_TRUE(f.canvas.document().setSettings(wide));
    f.canvas.documentReplaced();
    f.canvas.resize(1200, 300);
    f.canvas.setPenStyle([] { StrokeStyle s; s.width = 8.0; s.pressureCurve = PressureCurve::Const; return s; }());
    penStroke(&f.canvas, QPointF(400, 40), QPointF(500, 100), 1000);
    const QImage first = settledGrab(f.canvas);
    ASSERT_FALSE(first.isNull());
    EXPECT_LT(qGray(first.pixel(450, 70)), 128) << "ink on the line";
    EXPECT_GT(qGray(first.pixel(650, 70)), 200);
    f.canvas.setTool(InkCanvas::Tool::Lasso);
    penDrag(&f.canvas, {{380, 30}, {520, 30}, {520, 120}, {380, 120}, {380, 32}}, 3000);
    ASSERT_EQ(f.canvas.selection().size(), 1u);
    // One jump, so the old and the new bounds are far apart (the samples of a slow drag overlap and cover for each other).
    tablet(&f.canvas, QEvent::TabletPress, QPointF(450, 70), 0.5, 4000);
    tablet(&f.canvas, QEvent::TabletMove, QPointF(650, 70), 0.5, 4005); // right by 200, into the next tile
    tablet(&f.canvas, QEvent::TabletRelease, QPointF(650, 70), 0.5, 4010, Qt::NoButton);
    const QImage after = settledGrab(f.canvas);
    EXPECT_GT(qGray(after.pixel(450, 70)), 200) << "the old place shows the page again";
    EXPECT_LT(qGray(after.pixel(650, 70)), 128) << "the new place shows the ink";
}

TEST(InkCanvasTest, CopyPasteOffsetsAndAssignsNewT)
{
    CanvasFixture f;
    penStroke(&f.canvas, QPointF(20, 40), QPointF(120, 100), 1000);
    const qint64 tA = f.canvas.document().items()[0].t;
    f.canvas.setTool(InkCanvas::Tool::Lasso);
    penDrag(&f.canvas, kAroundFirstStroke, 3000);
    QTest::keyClick(&f.canvas, Qt::Key_C, Qt::ControlModifier);
    const QMimeData *mime = QApplication::clipboard()->mimeData();
    ASSERT_TRUE(mime);
    EXPECT_TRUE(mime->hasFormat(QString::fromLatin1(kClipboardMime)));
    QSignalSpy edited(&f.canvas, &InkCanvas::documentEdited);
    QTest::keyClick(&f.canvas, Qt::Key_V, Qt::ControlModifier);
    ASSERT_EQ(f.canvas.document().items().size(), 2u);
    EXPECT_EQ(edited.count(), 1) << "one paste, one history step";
    const Item &pasted = f.canvas.document().items()[1];
    EXPECT_GT(pasted.t, tA) << "a new t";
    const QPointF centre = f.canvas.document().items()[1].bounds().center();
    EXPECT_NEAR(centre.x(), 200.0 + 20.0, 1.0) << "the view centre, +20 right";
    EXPECT_NEAR(centre.y(), 150.0 + 20.0, 1.0) << "and +20 down";
    EXPECT_EQ(f.canvas.selection(), std::vector<qint64>{pasted.t}) << "the pasted items are selected";
    EXPECT_EQ(pasted.stroke->points.size(), f.canvas.document().items()[0].stroke->points.size());
    ASSERT_TRUE(f.canvas.undo());
    EXPECT_EQ(f.canvas.document().items().size(), 1u);
}

TEST(InkCanvasTest, CopyPutsTheTextBoxContentsOnTheClipboardAsPlainText)
{
    CanvasFixture f;
    Item box;
    box.type = Item::Type::TextBox;
    box.textBox = std::make_shared<TextBox>(TextBox{QRectF(50, 150, 100, 40), QStringLiteral("hello **there**")});
    f.canvas.document().addItem(box);
    f.canvas.document().commit();
    f.canvas.setTool(InkCanvas::Tool::Lasso);
    penDrag(&f.canvas, {{40, 140}, {160, 140}, {160, 200}, {40, 200}, {40, 142}}, 1000);
    ASSERT_EQ(f.canvas.selection().size(), 1u);
    ASSERT_TRUE(f.canvas.copySelection());
    EXPECT_EQ(QApplication::clipboard()->mimeData()->text(), QStringLiteral("hello **there**"));
}

TEST(InkCanvasTest, HeldLassoButtonSelectsWithoutChangingTheTool)
{
    CanvasFixture f;
    penStroke(&f.canvas, QPointF(20, 40), QPointF(120, 100), 1000);
    ASSERT_EQ(f.canvas.tool(), InkCanvas::Tool::Pen);
    EXPECT_TRUE(f.canvas.isHeldLassoEnabled()) << "the default";
    penDrag(&f.canvas, kAroundFirstStroke, 3000, Qt::LeftButton | kHeldLassoButton);
    EXPECT_EQ(f.canvas.tool(), InkCanvas::Tool::Pen);
    EXPECT_EQ(f.canvas.document().items().size(), 1u) << "no stroke was drawn";
    EXPECT_EQ(f.canvas.selection().size(), 1u);
    penStroke(&f.canvas, QPointF(200, 200), QPointF(300, 260), 4000); // the next plain stroke draws and deselects
    EXPECT_EQ(f.canvas.document().items().size(), 2u);
    EXPECT_TRUE(f.canvas.selection().empty());
}

TEST(InkCanvasTest, WithHeldLassoDisabledTheBarrelButtonDrawsAnOrdinaryStroke)
{
    CanvasFixture f;
    f.canvas.setHeldLassoEnabled(false);
    EXPECT_FALSE(f.canvas.isHeldLassoEnabled());
    penStroke(&f.canvas, QPointF(20, 40), QPointF(120, 100), 1000, Qt::LeftButton | kHeldLassoButton);
    EXPECT_EQ(f.canvas.document().items().size(), 1u);
    EXPECT_TRUE(f.canvas.selection().empty());
    EXPECT_EQ(f.canvas.tool(), InkCanvas::Tool::Pen);
}

TEST(InkCanvasTest, CtrlDragWithTheMouseSelectsWithoutDrawingWithMouse)
{
    CanvasFixture f;
    penStroke(&f.canvas, QPointF(20, 40), QPointF(120, 100), 1000);
    ASSERT_FALSE(f.canvas.drawWithMouse());
    QTest::mousePress(&f.canvas, Qt::LeftButton, Qt::NoModifier, QPoint(10, 30)); // a plain drag scrolls, it never selects or draws
    QTest::mouseMove(&f.canvas, QPoint(140, 30));
    QTest::mouseMove(&f.canvas, QPoint(140, 120));
    QTest::mouseRelease(&f.canvas, Qt::LeftButton, Qt::NoModifier, QPoint(10, 120));
    EXPECT_TRUE(f.canvas.selection().empty());
    EXPECT_EQ(f.canvas.document().items().size(), 1u);
    QTest::mousePress(&f.canvas, Qt::LeftButton, Qt::ControlModifier, QPoint(10, 30));
    QTest::mouseMove(&f.canvas, QPoint(140, 30));
    QTest::mouseMove(&f.canvas, QPoint(140, 120));
    QTest::mouseMove(&f.canvas, QPoint(10, 120));
    QTest::mouseRelease(&f.canvas, Qt::LeftButton, Qt::ControlModifier, QPoint(10, 118));
    EXPECT_EQ(f.canvas.selection().size(), 1u);
    EXPECT_EQ(f.canvas.document().items().size(), 1u) << "no stroke was added";
    EXPECT_EQ(f.canvas.tool(), InkCanvas::Tool::Pen) << "any tool, none changed";
}


namespace {

/// A selected stroke A (20,40)-(120,100) on the select tool, ready to be dragged from inside its box.
struct SelectedStrokeFixture : CanvasFixture {
    qint64 tA = 0;
    std::vector<StrokePoint> original;
    SelectedStrokeFixture()
    {
        canvas.setPenStyle([] { StrokeStyle s; s.width = 8.0; s.pressureCurve = PressureCurve::Const; return s; }());
        penStroke(&canvas, QPointF(20, 40), QPointF(120, 100), 1000);
        tA = canvas.document().items()[0].t;
        original = canvas.document().items()[0].stroke->points;
        canvas.setTool(InkCanvas::Tool::Lasso);
        penDrag(&canvas, kAroundFirstStroke, 3000);
    }
    /// Pen down inside the selection and two moves to the right: a Move drag in flight.
    void startDragging()
    {
        tablet(&canvas, QEvent::TabletPress, QPointF(70, 70), 0.5, 4000);
        tablet(&canvas, QEvent::TabletMove, QPointF(90, 70), 0.5, 4005);
        tablet(&canvas, QEvent::TabletMove, QPointF(110, 70), 0.5, 4010);
    }
    void expectBackAtTheOriginalPlace()
    {
        const Item *item = canvas.document().item(tA);
        ASSERT_TRUE(item);
        ASSERT_EQ(item->stroke->points.size(), original.size());
        for (size_t i = 0; i < original.size(); ++i)
            EXPECT_NEAR(item->stroke->points[i].x, original[i].x, 1e-9);
    }
};

} // namespace

TEST(InkToolsTest, ASegmentThatCrossesTheRegionSelectsAStrokeWithNoPointInside)
{
    InkDocument doc;
    const qint64 crossing = doc.addStroke(pointsStroke({{0, 50}, {300, 50}}));
    doc.addStroke(pointsStroke({{0, 200}, {300, 200}}));                       // passes below the box
    const qint64 diagonal = doc.addStroke(pointsStroke({{90, 0}, {110, 100}, {150, 0}})); // a vertex outside, segments through the box
    QPainterPath rect;
    rect.addRect(QRectF(100, 20, 60, 60));
    EXPECT_EQ(selectItems(doc, rect), (std::vector<qint64>{crossing, diagonal}));
    const QPainterPath lasso = polygonPath(QPolygonF({{100, 20}, {160, 20}, {130, 80}})); // a triangle: the horizontal line cuts it
    EXPECT_EQ(selectItems(doc, lasso), (std::vector<qint64>{crossing, diagonal})) << "the line below the box is out in both";
}

TEST(InkCanvasTest, EscapeMidDragCancelsTheMoveWithNoUndoStep)
{
    SelectedStrokeFixture f;
    QSignalSpy edited(&f.canvas, &InkCanvas::documentEdited);
    const QImage before = settledGrab(f.canvas);
    EXPECT_LT(qGray(before.pixel(70, 70)), 128);
    f.startDragging();
    ASSERT_TRUE(f.canvas.isDrawing());
    EXPECT_NEAR(f.canvas.document().item(f.tA)->stroke->points[0].x, f.original[0].x + 40.0, 1e-6) << "the drag is live";
    settledGrab(f.canvas); // a paint between the samples
    QTest::keyClick(&f.canvas, Qt::Key_Escape);
    EXPECT_FALSE(f.canvas.isDrawing());
    f.expectBackAtTheOriginalPlace();
    EXPECT_EQ(f.canvas.selection(), std::vector<qint64>{f.tA}) << "Escape cancelled the drag; the selection stays";
    tablet(&f.canvas, QEvent::TabletMove, QPointF(150, 70), 0.5, 4015); // the pen keeps moving: nothing follows it
    tablet(&f.canvas, QEvent::TabletRelease, QPointF(150, 70), 0.5, 4020, Qt::NoButton);
    f.expectBackAtTheOriginalPlace();
    EXPECT_EQ(edited.count(), 0);
    const QImage after = settledGrab(f.canvas);
    EXPECT_LT(qGray(after.pixel(70, 70)), 128) << "the ink is shown at its own place again";
    EXPECT_GT(qGray(after.pixel(110, 70)), 200) << "and not at the cancelled drag's place";
    ASSERT_TRUE(f.canvas.undo());
    EXPECT_TRUE(f.canvas.document().items().empty()) << "no step for the move: undo takes the stroke itself back";
    EXPECT_TRUE(f.canvas.selection().empty());
}

TEST(InkCanvasTest, ASecondEscapeThenClearsTheSelection)
{
    SelectedStrokeFixture f;
    f.startDragging();
    QTest::keyClick(&f.canvas, Qt::Key_Escape);
    ASSERT_EQ(f.canvas.selection().size(), 1u);
    QTest::keyClick(&f.canvas, Qt::Key_Escape);
    EXPECT_TRUE(f.canvas.selection().empty());
}

TEST(InkCanvasTest, AToolKeyMidDragCancelsTheMoveToo)
{
    SelectedStrokeFixture f;
    QSignalSpy edited(&f.canvas, &InkCanvas::documentEdited);
    settledGrab(f.canvas);
    f.startDragging();
    QTest::keyClick(&f.canvas, Qt::Key_E);
    EXPECT_EQ(f.canvas.tool(), InkCanvas::Tool::Eraser);
    EXPECT_FALSE(f.canvas.isDrawing());
    EXPECT_TRUE(f.canvas.selection().empty());
    f.expectBackAtTheOriginalPlace();
    tablet(&f.canvas, QEvent::TabletMove, QPointF(150, 70), 0.5, 4015);
    tablet(&f.canvas, QEvent::TabletRelease, QPointF(150, 70), 0.5, 4020, Qt::NoButton);
    f.expectBackAtTheOriginalPlace();
    EXPECT_EQ(edited.count(), 0);
    const QImage after = settledGrab(f.canvas);
    EXPECT_LT(qGray(after.pixel(70, 70)), 128);
    EXPECT_GT(qGray(after.pixel(110, 70)), 200);
}

TEST(InkCanvasTest, ADragThatReturnsToItsStartCommitsNoUndoStep)
{
    SelectedStrokeFixture f;
    QSignalSpy edited(&f.canvas, &InkCanvas::documentEdited);
    tablet(&f.canvas, QEvent::TabletPress, QPointF(70, 70), 0.5, 4000);
    tablet(&f.canvas, QEvent::TabletMove, QPointF(120, 90), 0.5, 4005);
    tablet(&f.canvas, QEvent::TabletMove, QPointF(70, 70), 0.5, 4010);
    tablet(&f.canvas, QEvent::TabletRelease, QPointF(70, 70), 0.5, 4015, Qt::NoButton);
    f.expectBackAtTheOriginalPlace();
    EXPECT_EQ(edited.count(), 0);
    ASSERT_TRUE(f.canvas.undo());
    EXPECT_TRUE(f.canvas.document().items().empty()) << "no step for the round trip";
}

TEST(InkCanvasTest, CancellingAScaleDragRestoresPointsPressureAndWidthExactly)
{
    SelectedStrokeFixture f;
    QSignalSpy edited(&f.canvas, &InkCanvas::documentEdited);
    const double width = f.canvas.document().item(f.tA)->stroke->style.width;
    const QRectF box = f.canvas.selectionBounds();
    const QPointF corner = f.canvas.toWidget(box.bottomRight());
    tablet(&f.canvas, QEvent::TabletPress, corner, 0.5, 4000);
    tablet(&f.canvas, QEvent::TabletMove, corner + QPointF(30, 20), 0.5, 4005);
    tablet(&f.canvas, QEvent::TabletMove, corner + QPointF(61, 43), 0.5, 4010);
    ASSERT_TRUE(f.canvas.isSelectGesture());
    const Item *scaled = f.canvas.document().item(f.tA);
    ASSERT_TRUE(scaled);
    EXPECT_GT(scaled->stroke->style.width, width) << "the scale is live";
    EXPECT_NE(scaled->stroke->points.back().x, f.original.back().x);
    QTest::keyClick(&f.canvas, Qt::Key_Escape);
    EXPECT_FALSE(f.canvas.isDrawing());
    const Item *item = f.canvas.document().item(f.tA);
    ASSERT_TRUE(item);
    ASSERT_EQ(item->stroke->points.size(), f.original.size());
    for (size_t i = 0; i < f.original.size(); ++i) { // exactly: the snapshot's own payload comes back, nothing is mapped back
        EXPECT_EQ(item->stroke->points[i].x, f.original[i].x) << i;
        EXPECT_EQ(item->stroke->points[i].y, f.original[i].y) << i;
        EXPECT_EQ(item->stroke->points[i].pressure, f.original[i].pressure) << i;
    }
    EXPECT_EQ(item->stroke->style.width, width);
    EXPECT_EQ(edited.count(), 0);
    EXPECT_EQ(f.canvas.selectionBounds(), box) << "the selection box goes back too";
    ASSERT_TRUE(f.canvas.undo());
    EXPECT_TRUE(f.canvas.document().items().empty()) << "no step for the scale";
}

TEST(InkCanvasTest, PasteRefusesHostileAndEmptyItemsAndAForeignClipboard)
{
    CanvasFixture f;
    penStroke(&f.canvas, QPointF(20, 40), QPointF(120, 100), 1000);
    const auto setClipboard = [](const QByteArray &json, const char *format = kClipboardMime) {
        auto *mime = new QMimeData;
        mime->setData(QString::fromLatin1(format), json);
        QApplication::clipboard()->setMimeData(mime);
    };
    const auto clip = [](const QByteArray &items) { return QByteArray(R"({"format":"notolith-ink-clip","items":[)") + items + "]}"; };
    QSignalSpy edited(&f.canvas, &InkCanvas::documentEdited);
    // Bounds that are not finite (a box 2e308 wide), a stroke with no point, a textbox with a non-finite rect.
    for (const QByteArray &bad : {clip(R"({"t":1,"type":"stroke","points":[-1e308,0,0.5,1e308,0,0.5]})"),
                                  clip(R"({"t":1,"type":"stroke","points":[]})"),
                                  clip(R"({"t":1,"type":"textbox","rect":[0,0,1e999,10],"markdown":"x"})"),
                                  clip(R"({"t":1,"type":"future"})"), clip("")}) {
        setClipboard(bad);
        EXPECT_FALSE(f.canvas.paste()) << bad.toStdString();
        EXPECT_EQ(f.canvas.document().items().size(), 1u) << bad.toStdString();
    }
    setClipboard(QByteArray("some text"), "text/plain");
    EXPECT_FALSE(f.canvas.paste()) << "a foreign clipboard";
    EXPECT_EQ(edited.count(), 0);
    setClipboard(clip(R"({"t":1,"type":"stroke","points":[]},{"t":2,"type":"stroke","points":[10,10,0.5,40,40,0.5]})"));
    EXPECT_TRUE(f.canvas.paste()) << "the good item is pasted, the empty one dropped";
    EXPECT_EQ(f.canvas.document().items().size(), 2u);
    EXPECT_EQ(f.canvas.selection().size(), 1u);
    for (const StrokePoint &p : f.canvas.document().items()[1].stroke->points)
        EXPECT_TRUE(std::isfinite(p.x) && std::isfinite(p.y));
}

TEST(InkCanvasTest, PasteFromThePenToolSwitchesToTheSelectToolSoTheSelectionStaysReachable)
{
    CanvasFixture f;
    penStroke(&f.canvas, QPointF(20, 40), QPointF(120, 100), 1000);
    penDrag(&f.canvas, kAroundFirstStroke, 3000, Qt::LeftButton | kHeldLassoButton); // the pen tool stays
    ASSERT_EQ(f.canvas.selection().size(), 1u);
    ASSERT_TRUE(f.canvas.copySelection());
    ASSERT_EQ(f.canvas.tool(), InkCanvas::Tool::Pen);
    ASSERT_TRUE(f.canvas.paste());
    EXPECT_EQ(f.canvas.tool(), InkCanvas::Tool::Lasso);
    EXPECT_EQ(f.canvas.selection().size(), 1u) << "switching the tool did not drop the pasted selection";
    EXPECT_EQ(f.canvas.document().items().size(), 2u);
}


TEST(InkCanvasTest, ADragInterruptedByHidingTheCanvasCommitsOneStepAtTheLastSample)
{
    SelectedStrokeFixture f;
    QSignalSpy edited(&f.canvas, &InkCanvas::documentEdited);
    f.startDragging(); // the pen is at x = 110: 40 px right of where it went down
    f.canvas.hide();
    EXPECT_FALSE(f.canvas.isDrawing());
    EXPECT_EQ(edited.count(), 1);
    EXPECT_NEAR(f.canvas.document().item(f.tA)->stroke->points[0].x, f.original[0].x + 40.0, 1e-6);
    ASSERT_TRUE(f.canvas.undo());
    f.expectBackAtTheOriginalPlace();
}

TEST(InkCanvasTest, AnEdgeHandleResizesOneAxisOnly)
{
    SelectedStrokeFixture f;
    const QRectF box = f.canvas.selectionBounds();
    const QPointF handle = f.canvas.toWidget(QPointF(box.right(), box.center().y())); // the right edge's middle
    penStroke(&f.canvas, handle, handle + QPointF(40, 0), 4000);
    const QRectF now = f.canvas.selectionBounds();
    EXPECT_NEAR(now.left(), box.left(), 2.0);
    EXPECT_GT(now.width(), box.width() + 30.0);
    EXPECT_NEAR(now.height(), box.height(), 2.0) << "only x was stretched (the width grows a little with it)";
}

// --- Task 14: shapes ----------------------------------------------------------------------------------------

namespace {

Shape makeShape(ShapeKind kind, std::vector<QPointF> points, double width = 4.0)
{
    Shape shape;
    shape.kind = kind;
    shape.style.width = width;
    shape.points = std::move(points);
    return shape;
}

qint64 addShape(InkDocument &doc, const Shape &shape)
{
    Item item;
    item.type = Item::Type::Shape;
    item.shape = std::make_shared<Shape>(shape);
    return doc.addItem(item);
}

} // namespace

TEST(InkToolsTest, ShapePathsHaveTheRightTopology)
{
    // A line: the segment grown by half the width, round caps.
    const Shape lineShape = makeShape(ShapeKind::Line, {{10, 20}, {110, 20}}, 6.0);
    const QPainterPath line = shapePath(lineShape);
    const QRectF box = line.boundingRect();
    const QRectF padded = lineShape.bounds();
    EXPECT_NEAR(box.left(), padded.left(), 0.3);
    EXPECT_NEAR(box.right(), padded.right(), 0.3);
    EXPECT_NEAR(box.top(), padded.top(), 0.3);
    EXPECT_NEAR(box.bottom(), padded.bottom(), 0.3);
    EXPECT_TRUE(line.contains(QPointF(60, 20)));
    EXPECT_TRUE(line.contains(QPointF(60, 22.5)));
    EXPECT_FALSE(line.contains(QPointF(60, 24))) << "half the width is 3";
    EXPECT_TRUE(line.contains(QPointF(8, 20))) << "a round cap reaches 3 px past the end";
    EXPECT_FALSE(line.contains(QPointF(6.5, 20)));

    // A rectangle: its outline, never its inside; the corners are round.
    const QPainterPath rect = shapePath(makeShape(ShapeKind::Rect, {{20, 20}, {120, 80}}));
    EXPECT_TRUE(rect.contains(QPointF(70, 21.5))) << "an edge point";
    EXPECT_TRUE(rect.contains(QPointF(120, 50)));
    EXPECT_FALSE(rect.contains(QPointF(70, 50))) << "not the centre: no fill in v1";
    EXPECT_FALSE(rect.contains(QPointF(70, 23.5)));
    EXPECT_FALSE(rect.contains(QPointF(121.6, 81.6))) << "a round join, not a mitre";
    const QPainterPath flipped = shapePath(makeShape(ShapeKind::Rect, {{120, 80}, {20, 20}}));
    EXPECT_TRUE(flipped.contains(QPointF(70, 21.5))) << "the corners may come in any order";
    EXPECT_FALSE(flipped.contains(QPointF(70, 50)));

    // An ellipse: likewise.
    const QPainterPath ellipse = shapePath(makeShape(ShapeKind::Ellipse, {{20, 20}, {120, 80}}));
    EXPECT_TRUE(ellipse.contains(QPointF(20, 50)));
    EXPECT_TRUE(ellipse.contains(QPointF(70, 20)));
    EXPECT_TRUE(ellipse.contains(QPointF(70, 80)));
    EXPECT_FALSE(ellipse.contains(QPointF(70, 50)));
    EXPECT_FALSE(ellipse.contains(QPointF(22, 22)));

    // An imported polygon is closed, a polyline is not.
    const std::vector<QPointF> triangle = {{0, 0}, {100, 0}, {50, 80}};
    const QPainterPath polygon = shapePath(makeShape(ShapeKind::Polygon, triangle));
    const QPainterPath polyline = shapePath(makeShape(ShapeKind::Polyline, triangle));
    EXPECT_TRUE(polygon.contains(QPointF(25, 40))) << "the closing edge from the last point back to the first";
    EXPECT_FALSE(polyline.contains(QPointF(25, 40)));
    EXPECT_TRUE(polyline.contains(QPointF(75, 40)));
    EXPECT_TRUE(polygon.contains(QPointF(75, 40)));
    EXPECT_FALSE(polygon.contains(QPointF(50, 25))) << "the inside is not inked";

    // An arrow is its line plus an open head at the end, inside the padding Shape::bounds promises.
    const Shape arrowShape = makeShape(ShapeKind::Arrow, {{10, 20}, {110, 20}});
    const QPainterPath arrow = shapePath(arrowShape);
    const QPainterPath plain = shapePath(makeShape(ShapeKind::Line, {{10, 20}, {110, 20}}));
    const QPointF barb(104.8, 17.0); // half way along the upper barb (30 degrees back from the tip, 3 widths long)
    EXPECT_TRUE(arrow.contains(barb));
    EXPECT_FALSE(plain.contains(barb));
    EXPECT_TRUE(arrow.contains(QPointF(104.8, 23.0))) << "and the lower one";
    EXPECT_TRUE(arrow.contains(QPointF(108, 20))) << "where the shaft and both barbs overlap, the ink does not cancel out";
    EXPECT_TRUE(arrowShape.bounds().adjusted(-0.01, -0.01, 0.01, 0.01).contains(arrow.boundingRect()));
    EXPECT_GT(arrow.boundingRect().height(), plain.boundingRect().height() + 6.0);
}

TEST(InkToolsTest, AShapeWithNoInkHasAnEmptyPath)
{
    const double nan = std::numeric_limits<double>::quiet_NaN();
    EXPECT_TRUE(shapePath(makeShape(ShapeKind::Line, {{1, 1}})).isEmpty()) << "one point";
    EXPECT_TRUE(shapePath(makeShape(ShapeKind::Rect, {})).isEmpty());
    EXPECT_TRUE(shapePath(makeShape(ShapeKind::Line, {{0, 0}, {nan, 5}})).isEmpty());
    EXPECT_TRUE(shapePath(makeShape(ShapeKind::Polygon, {{0, 0}, {10, 0}, {5, std::numeric_limits<double>::infinity()}})).isEmpty());
    EXPECT_TRUE(shapePath(makeShape(ShapeKind::Ellipse, {{0, 0}, {10, 10}}, 0.0)).isEmpty()) << "no width, no ink";
    EXPECT_TRUE(shapePath(makeShape(ShapeKind::Ellipse, {{0, 0}, {10, 10}}, nan)).isEmpty());
    const QPainterPath huge = shapePath(makeShape(ShapeKind::Line, {{0, 0}, {10, 0}}, std::numeric_limits<double>::infinity()));
    EXPECT_TRUE(huge.isEmpty() || pathIsFinite(huge)) << "an infinite width is clamped, never an overflow";
    EXPECT_FALSE(shapePath(makeShape(ShapeKind::Line, {{0, 0}, {10, 0}}, -4.0)).isEmpty()) << "a negative width counts as its absolute value (strokes do)";
}

TEST(InkToolsTest, ARegionSelectsAShapeByItsOutlineNotByTheCentreOfItsBounds)
{
    InkDocument doc;
    const qint64 rect = addShape(doc, makeShape(ShapeKind::Rect, {{20, 20}, {220, 120}}));
    QPainterPath inside;
    inside.addRect(QRectF(100, 60, 40, 20)); // holds the centre of the bounds (120, 70), touches no ink
    EXPECT_TRUE(selectItems(doc, inside).empty()) << "the hollow inside of a rectangle is not the rectangle";
    QPainterPath crossing;
    crossing.addRect(QRectF(100, 10, 40, 20)); // over the top edge, nowhere near the centre
    EXPECT_EQ(selectItems(doc, crossing), std::vector<qint64>{rect});
    QPainterPath around;
    around.addRect(QRectF(0, 0, 300, 200));
    EXPECT_EQ(selectItems(doc, around), std::vector<qint64>{rect}) << "enclosing it selects it";
    QPainterPath elsewhere;
    elsewhere.addRect(QRectF(300, 0, 40, 40));
    EXPECT_TRUE(selectItems(doc, elsewhere).empty());
    EXPECT_EQ(selectItems(doc, polygonPath(QPolygonF({{10, 100}, {40, 100}, {25, 140}}))), std::vector<qint64>{rect}) << "a lasso over the left edge";
}

TEST(StrokeOutlineTest, ShapesMatchTheGoldenImage)
{
    const std::vector<Shape> shapes = {
        makeShape(ShapeKind::Line, {{20, 30}, {130, 60}}, 6.0),
        [] { Shape s = makeShape(ShapeKind::Arrow, {{160, 40}, {280, 40}}, 4.0); s.style.color = QColor(0xc0, 0x00, 0x00); return s; }(),
        [] { Shape s = makeShape(ShapeKind::Rect, {{20, 90}, {130, 180}}, 3.0); s.style.color = QColor(0x00, 0x50, 0xc0); return s; }(),
        [] { Shape s = makeShape(ShapeKind::Ellipse, {{160, 90}, {280, 180}}, 8.0); s.style.color = QColor(0x00, 0xa0, 0x40, 0x80); return s; }(),
    };
    QImage image(300, 200, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::white);
    std::vector<QRect> regions;
    {
        QPainter painter(&image);
        painter.setRenderHint(QPainter::Antialiasing);
        for (const Shape &shape : shapes) {
            paintShape(painter, shape);
            regions.push_back(shape.bounds().adjusted(-2, -2, 2, 2).toAlignedRect());
        }
    }
    checkGolden(QStringLiteral("shapes-z1-d1.png"), image, regions);
}

TEST(InkCanvasTest, DragDrawsARectAndEscapeCancelsTheDrag)
{
    CanvasFixture f;
    f.canvas.setPenStyle([] { StrokeStyle s; s.width = 3.0; s.color = QColor(0x20, 0x40, 0x80); return s; }());
    f.canvas.setTool(InkCanvas::Tool::Shape);
    f.canvas.setShapeKind(ShapeKind::Rect);
    ASSERT_EQ(f.canvas.shapeKind(), ShapeKind::Rect);
    QSignalSpy edited(&f.canvas, &InkCanvas::documentEdited);
    penDrag(&f.canvas, {{50, 60}, {150, 100}, {200, 140}}, 1000);
    ASSERT_EQ(f.canvas.document().items().size(), 1u);
    const Item &item = f.canvas.document().items()[0];
    ASSERT_EQ(item.type, Item::Type::Shape);
    EXPECT_EQ(item.shape->kind, ShapeKind::Rect);
    EXPECT_EQ(item.layer, Layer::User);
    ASSERT_EQ(item.shape->points.size(), 2u);
    EXPECT_NEAR(item.shape->points[0].x(), 50.0, 1e-6);
    EXPECT_NEAR(item.shape->points[0].y(), 60.0, 1e-6);
    EXPECT_NEAR(item.shape->points[1].x(), 200.0, 1e-6) << "the last sample, not the middle one";
    EXPECT_NEAR(item.shape->points[1].y(), 140.0, 1e-6);
    EXPECT_DOUBLE_EQ(item.shape->style.width, 3.0);
    EXPECT_EQ(item.shape->style.color, QColor(0x20, 0x40, 0x80));
    EXPECT_FALSE(f.canvas.isDrawing());
    EXPECT_EQ(edited.count(), 1) << "one drag, one history step";
    // A second drag, cancelled by Escape, adds nothing.
    tablet(&f.canvas, QEvent::TabletPress, QPointF(60, 200), 0.5, 2000);
    tablet(&f.canvas, QEvent::TabletMove, QPointF(120, 250), 0.5, 2005);
    EXPECT_TRUE(f.canvas.isDrawing());
    QTest::keyClick(&f.canvas, Qt::Key_Escape);
    EXPECT_FALSE(f.canvas.isDrawing());
    tablet(&f.canvas, QEvent::TabletMove, QPointF(300, 280), 0.5, 2010);
    tablet(&f.canvas, QEvent::TabletRelease, QPointF(300, 280), 0.5, 2015, Qt::NoButton);
    EXPECT_EQ(f.canvas.document().items().size(), 1u);
    EXPECT_EQ(edited.count(), 1);
    ASSERT_TRUE(f.canvas.undo());
    EXPECT_TRUE(f.canvas.document().items().empty()) << "no step for the cancelled drag: undo takes the rectangle itself back";
}

TEST(InkCanvasTest, EveryShapeKindDrawsAndPolylineAndPolygonAreRefused)
{
    CanvasFixture f;
    f.canvas.setTool(InkCanvas::Tool::Shape);
    int y = 30;
    for (const ShapeKind kind : {ShapeKind::Line, ShapeKind::Arrow, ShapeKind::Rect, ShapeKind::Ellipse}) {
        f.canvas.setShapeKind(kind);
        EXPECT_EQ(f.canvas.shapeKind(), kind);
        penDrag(&f.canvas, {{40, double(y)}, {160, double(y + 20)}}, 1000 + y);
        y += 50;
    }
    ASSERT_EQ(f.canvas.document().items().size(), 4u);
    const ShapeKind kinds[] = {ShapeKind::Line, ShapeKind::Arrow, ShapeKind::Rect, ShapeKind::Ellipse};
    for (size_t i = 0; i < 4; ++i) {
        ASSERT_EQ(f.canvas.document().items()[i].type, Item::Type::Shape);
        EXPECT_EQ(f.canvas.document().items()[i].shape->kind, kinds[i]);
        EXPECT_EQ(f.canvas.document().items()[i].shape->points.size(), 2u);
    }
    QSignalSpy changed(&f.canvas, &InkCanvas::shapeKindChanged);
    f.canvas.setShapeKind(ShapeKind::Polygon);
    f.canvas.setShapeKind(ShapeKind::Polyline);
    EXPECT_EQ(f.canvas.shapeKind(), ShapeKind::Ellipse) << "no tool draws these";
    f.canvas.setShapeKind(ShapeKind::Ellipse);
    EXPECT_EQ(changed.count(), 0) << "no change, no signal";
    f.canvas.setShapeKind(ShapeKind::Line);
    EXPECT_EQ(changed.count(), 1);
}

TEST(InkCanvasTest, AClickWithoutADragCommitsNoShape)
{
    CanvasFixture f;
    f.canvas.setTool(InkCanvas::Tool::Shape);
    penDrag(&f.canvas, {{100, 100}, {100, 100}}, 1000);
    penDrag(&f.canvas, {{100, 100}, {101, 100}}, 2000);
    EXPECT_TRUE(f.canvas.document().items().empty()) << "a shape needs a drag of a few px";
    EXPECT_FALSE(f.canvas.document().isModified());
    f.canvas.setShapeKind(ShapeKind::Rect);
    penDrag(&f.canvas, {{100, 100}, {100, 150}}, 3000); // a box with no width: a vertical drag is still a drag
    EXPECT_EQ(f.canvas.document().items().size(), 1u);
}

TEST(InkCanvasTest, TheShapeIsPreviewedWhileTheDragIsInFlightAndPaintedByTheTilesAfterwards)
{
    CanvasFixture f;
    f.canvas.setPenStyle([] { StrokeStyle s; s.width = 6.0; s.color = Qt::black; return s; }());
    f.canvas.setTool(InkCanvas::Tool::Shape);
    f.canvas.setShapeKind(ShapeKind::Rect);
    settledGrab(f.canvas);
    tablet(&f.canvas, QEvent::TabletPress, QPointF(50, 60), 0.5, 1000);
    tablet(&f.canvas, QEvent::TabletMove, QPointF(250, 200), 0.5, 1005);
    const QImage mid = f.canvas.grab().toImage();
    EXPECT_LT(qGray(mid.pixel(150, 60)), 100) << "the top edge, from the preview";
    EXPECT_LT(qGray(mid.pixel(250, 130)), 100) << "the right edge";
    EXPECT_TRUE(f.canvas.document().items().empty()) << "nothing is committed until the release";
    tablet(&f.canvas, QEvent::TabletMove, QPointF(150, 120), 0.5, 1010); // shrinks: the old edges must go
    const QImage shrunk = f.canvas.grab().toImage();
    EXPECT_GT(qGray(shrunk.pixel(250, 130)), 150) << "the preview moved with the pointer";
    EXPECT_LT(qGray(shrunk.pixel(100, 120)), 100) << "the new bottom edge";
    tablet(&f.canvas, QEvent::TabletMove, QPointF(250, 200), 0.5, 1015);
    tablet(&f.canvas, QEvent::TabletRelease, QPointF(250, 200), 0.5, 1020, Qt::NoButton);
    ASSERT_EQ(f.canvas.document().items().size(), 1u);
    const QImage done = settledGrab(f.canvas);
    EXPECT_LT(qGray(done.pixel(150, 60)), 100) << "the tiles paint the committed rectangle (not a grey placeholder)";
    EXPECT_LT(qGray(done.pixel(50, 130)), 100);
    EXPECT_LT(qGray(done.pixel(150, 200)), 100);
    EXPECT_GT(qGray(done.pixel(150, 130)), 180) << "and its inside stays empty";
    f.canvas.setShapeKind(ShapeKind::Ellipse);
    penDrag(&f.canvas, {{280, 60}, {380, 200}}, 2000);
    const QImage two = settledGrab(f.canvas);
    EXPECT_LT(qGray(two.pixel(281, 130)), 100) << "the ellipse's left extreme";
    EXPECT_GT(qGray(two.pixel(284, 64)), 180) << "and not its box corner";
}

TEST(InkCanvasTest, ACommittedShapeStaysOnScreenUntilItsTilesArrive)
{
    CanvasFixture f;
    f.canvas.setPenStyle([] { StrokeStyle s; s.width = 6.0; s.color = Qt::black; return s; }());
    f.canvas.setTool(InkCanvas::Tool::Shape);
    f.canvas.setShapeKind(ShapeKind::Line);
    settledGrab(f.canvas);
    penDrag(&f.canvas, {{50, 100}, {250, 100}}, 1000);
    ASSERT_EQ(f.canvas.document().items().size(), 1u);
    ASSERT_GT(f.canvas.staleTileCount(), 0) << "the tile under the line has not been rendered again yet";
    EXPECT_LT(qGray(f.canvas.grab().toImage().pixel(150, 100)), 100) << "no frame without the line between the preview and the tile";
    EXPECT_LT(qGray(settledGrab(f.canvas).pixel(150, 100)), 100);
    f.canvas.undo();
    EXPECT_GT(qGray(settledGrab(f.canvas).pixel(150, 100)), 150) << "an undo takes the shape away";
}

TEST(InkCanvasTest, AToolChangeOrAReadOnlyCanvasCancelsOrIgnoresTheShapeDrag)
{
    CanvasFixture f;
    f.canvas.setTool(InkCanvas::Tool::Shape);
    tablet(&f.canvas, QEvent::TabletPress, QPointF(60, 60), 0.5, 1000);
    tablet(&f.canvas, QEvent::TabletMove, QPointF(160, 160), 0.5, 1005);
    ASSERT_TRUE(f.canvas.isDrawing());
    QTest::keyClick(&f.canvas, Qt::Key_P);
    EXPECT_FALSE(f.canvas.isDrawing());
    tablet(&f.canvas, QEvent::TabletRelease, QPointF(160, 160), 0.5, 1010, Qt::NoButton);
    EXPECT_TRUE(f.canvas.document().items().empty()) << "the tool change cancelled the drag";
    f.canvas.setTool(InkCanvas::Tool::Shape);
    f.canvas.setReadOnly(true);
    penDrag(&f.canvas, {{60, 60}, {160, 160}}, 2000);
    EXPECT_TRUE(f.canvas.document().items().empty());
}

TEST(InkCanvasTest, ShiftSCyclesTheShapeKindAndPicksTheShapeTool)
{
    CanvasFixture f;
    QSignalSpy changed(&f.canvas, &InkCanvas::shapeKindChanged);
    ASSERT_EQ(f.canvas.shapeKind(), ShapeKind::Line);
    const ShapeKind order[] = {ShapeKind::Arrow, ShapeKind::Rect, ShapeKind::Ellipse, ShapeKind::Line};
    for (const ShapeKind kind : order) {
        QTest::keyClick(&f.canvas, Qt::Key_S, Qt::ShiftModifier);
        EXPECT_EQ(f.canvas.tool(), InkCanvas::Tool::Shape);
        EXPECT_EQ(f.canvas.shapeKind(), kind);
    }
    EXPECT_EQ(changed.count(), 4);
    f.canvas.setReadOnly(true);
    QTest::keyClick(&f.canvas, Qt::Key_S, Qt::ShiftModifier);
    EXPECT_EQ(f.canvas.shapeKind(), ShapeKind::Line) << "a read-only canvas ignores it";
}

TEST(InkCanvasTest, TheEraserTakesAShapeOnlyWhereItsOutlineIs)
{
    for (const auto mode : {InkCanvas::EraserMode::Stroke, InkCanvas::EraserMode::Split}) {
        SCOPED_TRACE(int(mode));
        CanvasFixture f;
        addShape(f.canvas.document(), makeShape(ShapeKind::Rect, {{50, 50}, {350, 250}}));
        addShape(f.canvas.document(), makeShape(ShapeKind::Line, {{20, 150}, {380, 150}}, 6.0));
        f.canvas.document().commit();
        f.canvas.setEraserMode(mode);
        f.canvas.setTool(InkCanvas::Tool::Eraser);
        penStroke(&f.canvas, QPointF(100, 100), QPointF(100, 130), 1000); // inside the rectangle, clear of its edges and the line
        EXPECT_EQ(f.canvas.document().items().size(), 2u) << "the hollow inside is not the rectangle";
        penStroke(&f.canvas, QPointF(100, 175), QPointF(100, 215), 2000); // below the line, inside the box: still nothing
        EXPECT_EQ(f.canvas.document().items().size(), 2u);
        penStroke(&f.canvas, QPointF(200, 30), QPointF(200, 70), 3000); // over the top edge
        ASSERT_EQ(f.canvas.document().items().size(), 1u);
        EXPECT_EQ(f.canvas.document().items()[0].shape->kind, ShapeKind::Line);
        penStroke(&f.canvas, QPointF(300, 120), QPointF(300, 180), 4000); // across the line
        EXPECT_TRUE(f.canvas.document().items().empty()) << "taken whole, in the split mode too";
    }
}

TEST(InkCanvasTest, ASelectedShapeMovesWithItsPoints)
{
    CanvasFixture f;
    f.canvas.setTool(InkCanvas::Tool::Shape);
    f.canvas.setShapeKind(ShapeKind::Rect);
    penDrag(&f.canvas, {{50, 60}, {150, 140}}, 1000);
    ASSERT_EQ(f.canvas.document().items().size(), 1u);
    f.canvas.setTool(InkCanvas::Tool::Lasso);
    penDrag(&f.canvas, {{30, 40}, {170, 40}, {170, 160}, {30, 160}, {30, 42}}, 2000);
    ASSERT_EQ(f.canvas.selection().size(), 1u);
    penStroke(&f.canvas, QPointF(100, 100), QPointF(140, 130), 3000); // inside the box: a move by (40, 30)
    ASSERT_EQ(f.canvas.document().items()[0].type, Item::Type::Shape);
    const Shape &moved = *f.canvas.document().items()[0].shape;
    EXPECT_NEAR(moved.points[0].x(), 90.0, 1e-6);
    EXPECT_NEAR(moved.points[0].y(), 90.0, 1e-6);
    EXPECT_NEAR(moved.points[1].x(), 190.0, 1e-6);
    EXPECT_EQ(f.canvas.selection().size(), 1u);
}

// --- Task 15: text boxes ----------------------------------------------------------------------------------------

namespace {

/// The renderer the canvas tests install: it records every call (and the thread it ran on) and returns a pale blue
/// pixmap of the asked size with the Markdown drawn on it by QPainter::drawText.
struct TextStub {
    std::atomic<int> calls{0};
    std::vector<QThread *> threads;
    QSizeF lastSize;
    QString lastMarkdown;
    InkCanvas::TextRenderer renderer()
    {
        return [this](const QString &markdown, const QSizeF &size) {
            ++calls;
            threads.push_back(QThread::currentThread());
            lastSize = size;
            lastMarkdown = markdown;
            QPixmap pixmap(size.toSize());
            pixmap.fill(QColor(0xc0, 0xe0, 0xff));
            QPainter painter(&pixmap);
            painter.setPen(Qt::black);
            painter.drawText(pixmap.rect(), Qt::TextWordWrap, markdown);
            return pixmap;
        };
    }
};

qint64 addTextBox(InkCanvas &canvas, const QRectF &rect, const QString &markdown)
{
    Item box;
    box.type = Item::Type::TextBox;
    box.textBox = std::make_shared<TextBox>(TextBox{rect, markdown});
    const qint64 t = canvas.document().addItem(box);
    canvas.document().commit();
    canvas.documentReplaced();
    return t;
}

/// The editor in use: one that was closed lingers hidden until the event loop deletes it.
QPlainTextEdit *textEditorOf(InkCanvas &canvas)
{
    for (auto *edit : canvas.findChildren<QPlainTextEdit *>(QStringLiteral("inkTextEditor")))
        if (!edit->isHidden())
            return edit;
    return nullptr;
}

void click(InkCanvas &canvas, QPoint at) { QTest::mouseClick(&canvas, Qt::LeftButton, Qt::NoModifier, at); }

} // namespace

TEST(InkCanvasTest, ClickWithTextToolCreatesABoxAndEditingCommitsMarkdown)
{
    CanvasFixture f;
    TextStub stub;
    f.canvas.setTextRenderer(stub.renderer());
    QSignalSpy edited(&f.canvas, &InkCanvas::documentEdited);
    f.canvas.setTool(InkCanvas::Tool::Text);
    click(f.canvas, QPoint(60, 40)); // the mouse does not draw (drawWithMouse is off), but a click with the Text tool is no drawing
    QPlainTextEdit *editor = textEditorOf(f.canvas);
    ASSERT_NE(editor, nullptr);
    EXPECT_TRUE(f.canvas.isEditingText());
    EXPECT_EQ(f.canvas.editingTextItem(), -1) << "a new box is not in the document until it has text";
    EXPECT_TRUE(f.canvas.document().items().empty());
    EXPECT_EQ(editor->geometry().topLeft(), QPoint(60, 40)) << "the editor sits where the box will be";
    QTest::keyClicks(editor, QStringLiteral("slate [[Welcome]] **bold**")); // s, l and t are tool keys: the editor keeps them
    EXPECT_EQ(f.canvas.tool(), InkCanvas::Tool::Text) << "typing never reaches the canvas shortcuts";
    QTest::keyClick(editor, Qt::Key_Escape);
    EXPECT_FALSE(f.canvas.isEditingText());
    EXPECT_EQ(textEditorOf(f.canvas), nullptr) << "the editor is gone (hidden at once, deleted by the event loop)";
    ASSERT_EQ(f.canvas.document().items().size(), 1u);
    const Item &item = f.canvas.document().items()[0];
    ASSERT_EQ(item.type, Item::Type::TextBox);
    EXPECT_EQ(item.textBox->markdown, QStringLiteral("slate [[Welcome]] **bold**"));
    EXPECT_NEAR(item.textBox->rect.x(), 60.0, 1e-6);
    EXPECT_NEAR(item.textBox->rect.y(), 40.0, 1e-6);
    EXPECT_EQ(item.textBox->rect.size(), InkCanvas::kNewTextBoxSize);
    EXPECT_EQ(item.layer, Layer::User);
    EXPECT_EQ(edited.count(), 1) << "one box, one history step";
    ASSERT_TRUE(f.canvas.undo());
    EXPECT_TRUE(f.canvas.document().items().empty());
}

TEST(InkCanvasTest, ABoxNearTheRightEdgeStaysInsideThePage)
{
    CanvasFixture f; // 400 px wide
    f.canvas.setTool(InkCanvas::Tool::Text);
    click(f.canvas, QPoint(350, 40));
    QPlainTextEdit *editor = textEditorOf(f.canvas);
    ASSERT_NE(editor, nullptr);
    QTest::keyClicks(editor, QStringLiteral("x"));
    QTest::keyClick(editor, Qt::Key_Escape);
    ASSERT_EQ(f.canvas.document().items().size(), 1u);
    EXPECT_LE(f.canvas.document().items()[0].textBox->rect.right(), 400.0 + 1e-6);
}

TEST(InkCanvasTest, ASingleClickOnAnExistingBoxEditsIt)
{
    CanvasFixture f;
    TextStub stub;
    f.canvas.setTextRenderer(stub.renderer());
    const qint64 t = addTextBox(f.canvas, QRectF(50, 50, 200, 60), QStringLiteral("old"));
    f.canvas.setTool(InkCanvas::Tool::Text);
    QSignalSpy edited(&f.canvas, &InkCanvas::documentEdited);
    click(f.canvas, QPoint(100, 70));
    QPlainTextEdit *editor = textEditorOf(f.canvas);
    ASSERT_NE(editor, nullptr) << "one click, no double-click";
    EXPECT_EQ(f.canvas.editingTextItem(), t);
    EXPECT_EQ(editor->toPlainText(), QStringLiteral("old"));
    EXPECT_EQ(editor->geometry(), QRect(50, 50, 200, 60));
    QTest::keyClicks(editor, QStringLiteral(" more"));
    QTest::keyClick(editor, Qt::Key_Escape);
    ASSERT_EQ(f.canvas.document().items().size(), 1u) << "edited in place, not a second box";
    EXPECT_EQ(f.canvas.document().items()[0].t, t);
    EXPECT_EQ(f.canvas.document().items()[0].textBox->markdown, QStringLiteral("old more"));
    EXPECT_EQ(f.canvas.document().items()[0].textBox->rect, QRectF(50, 50, 200, 60));
    EXPECT_EQ(edited.count(), 1);
}

TEST(InkCanvasTest, ATapOfThePenEditsAndLeavesTheMouseWorking)
{
    CanvasFixture f;
    addTextBox(f.canvas, QRectF(50, 50, 200, 60), QStringLiteral("old"));
    f.canvas.setTool(InkCanvas::Tool::Text);
    tablet(&f.canvas, QEvent::TabletPress, QPointF(100, 70), 0.5, 1000);
    tablet(&f.canvas, QEvent::TabletRelease, QPointF(100, 70), 0.5, 1010, Qt::NoButton);
    ASSERT_NE(textEditorOf(f.canvas), nullptr) << "the pen tip alone, no button";
    QTest::keyClick(textEditorOf(f.canvas), Qt::Key_Escape);
    EXPECT_FALSE(f.canvas.isEditingText());
    EXPECT_FALSE(f.canvas.isDrawing());
    click(f.canvas, QPoint(100, 70)); // a stuck tablet flag would swallow this
    EXPECT_TRUE(f.canvas.isEditingText());
}

TEST(InkCanvasTest, AnUnchangedBoxCommitsNothingAnEmptiedOneIsDeletedAndAnEmptyNewOneIsNeverAdded)
{
    CanvasFixture f;
    const qint64 t = addTextBox(f.canvas, QRectF(50, 50, 200, 60), QStringLiteral("keep"));
    f.canvas.document().markSaved();
    f.canvas.setTool(InkCanvas::Tool::Text);
    QSignalSpy edited(&f.canvas, &InkCanvas::documentEdited);
    click(f.canvas, QPoint(100, 70));
    QTest::keyClick(textEditorOf(f.canvas), Qt::Key_Escape);
    EXPECT_EQ(edited.count(), 0) << "unchanged";
    EXPECT_FALSE(f.canvas.document().isModified());
    click(f.canvas, QPoint(300, 200)); // empty page: a new box in the making
    ASSERT_NE(textEditorOf(f.canvas), nullptr);
    QTest::keyClicks(textEditorOf(f.canvas), QStringLiteral("  "));
    QTest::keyClick(textEditorOf(f.canvas), Qt::Key_Escape);
    EXPECT_EQ(f.canvas.document().items().size(), 1u) << "blank: no box";
    EXPECT_EQ(edited.count(), 0);
    click(f.canvas, QPoint(100, 70));
    textEditorOf(f.canvas)->clear();
    QTest::keyClick(textEditorOf(f.canvas), Qt::Key_Escape);
    EXPECT_EQ(f.canvas.document().item(t), nullptr) << "emptied: the box goes";
    EXPECT_EQ(edited.count(), 1);
    ASSERT_TRUE(f.canvas.undo());
    ASSERT_NE(f.canvas.document().item(t), nullptr);
    EXPECT_EQ(f.canvas.document().item(t)->textBox->markdown, QStringLiteral("keep"));
}

TEST(InkCanvasTest, AToolChangeAndUndoAndAPressElsewhereEndTheEdit)
{
    CanvasFixture f;
    f.canvas.setTool(InkCanvas::Tool::Text);
    click(f.canvas, QPoint(40, 40));
    QTest::keyClicks(textEditorOf(f.canvas), QStringLiteral("one"));
    f.canvas.setTool(InkCanvas::Tool::Pen);
    EXPECT_FALSE(f.canvas.isEditingText());
    ASSERT_EQ(f.canvas.document().items().size(), 1u) << "a tool change commits";
    f.canvas.setTool(InkCanvas::Tool::Text);
    click(f.canvas, QPoint(40, 200));
    QTest::keyClicks(textEditorOf(f.canvas), QStringLiteral("two"));
    click(f.canvas, QPoint(300, 150)); // the press commits the first and starts the next
    ASSERT_EQ(f.canvas.document().items().size(), 2u);
    EXPECT_TRUE(f.canvas.isEditingText()) << "the press began a new box";
    EXPECT_EQ(f.canvas.editingTextItem(), -1);
    QTest::keyClicks(textEditorOf(f.canvas), QStringLiteral("three"));
    ASSERT_TRUE(f.canvas.undo()) << "undo commits the open edit, then takes it back";
    EXPECT_FALSE(f.canvas.isEditingText());
    EXPECT_EQ(f.canvas.document().items().size(), 2u);
}

TEST(InkCanvasTest, TextRendersThroughTheInstalledRenderer)
{
    CanvasFixture f;
    TextStub stub;
    f.canvas.setTextRenderer(stub.renderer());
    const qint64 t = addTextBox(f.canvas, QRectF(50, 50, 200, 60), QStringLiteral("hello"));
    const QImage shot = settledGrab(f.canvas);
    EXPECT_EQ(stub.calls, 1);
    EXPECT_EQ(stub.lastMarkdown, QStringLiteral("hello"));
    EXPECT_EQ(stub.lastSize, QSizeF(200, 60)) << "the box's size in document px";
    EXPECT_EQ(shot.pixelColor(240, 100), QColor(0xc0, 0xe0, 0xff)) << "the pixmap is painted over the tiles";
    EXPECT_NE(shot.pixelColor(30, 100), QColor(0xc0, 0xe0, 0xff)) << "and only inside the box";
    f.canvas.grab();
    f.canvas.grab();
    EXPECT_EQ(stub.calls, 1) << "cached: a second paint at the same zoom renders nothing";
    f.canvas.setZoom(2.0);
    settledGrab(f.canvas);
    EXPECT_EQ(stub.calls, 1) << "the layout does not depend on the zoom: the pixmap is only scaled";
    f.canvas.setZoom(1.0);
    settledGrab(f.canvas);
    // Moving the box keeps its pixmap; changing the text or the size draws a new one.
    Item moved = *f.canvas.document().item(t);
    moved.textBox = std::make_shared<TextBox>(TextBox{QRectF(80, 90, 200, 60), QStringLiteral("hello")});
    f.canvas.document().replaceItem(t, moved);
    f.canvas.documentReplaced();
    f.canvas.grab();
    EXPECT_EQ(stub.calls, 1) << "a move is not a re-render";
    moved.textBox = std::make_shared<TextBox>(TextBox{QRectF(80, 90, 200, 60), QStringLiteral("changed")});
    f.canvas.document().replaceItem(t, moved);
    f.canvas.documentReplaced();
    f.canvas.grab();
    EXPECT_EQ(stub.calls, 2);
    f.canvas.invalidateTextCache();
    f.canvas.grab();
    EXPECT_EQ(stub.calls, 3) << "a theme change asks for new pixmaps";
}

TEST(InkCanvasTest, ANewTextBoxIsNotRenderedWhileThePenIsMoving)
{
    CanvasFixture f;
    TextStub stub;
    f.canvas.setTextRenderer(stub.renderer());
    addTextBox(f.canvas, QRectF(50, 150, 200, 60), QStringLiteral("hello"));
    tablet(&f.canvas, QEvent::TabletPress, QPointF(20, 20), 0.5, 1000);
    tablet(&f.canvas, QEvent::TabletMove, QPointF(40, 30), 0.5, 1005);
    f.canvas.grab();
    EXPECT_EQ(stub.calls, 0) << "the pen is down: no render in paintEvent";
    tablet(&f.canvas, QEvent::TabletRelease, QPointF(60, 40), 0.5, 1010, Qt::NoButton);
    f.canvas.grab();
    EXPECT_EQ(stub.calls, 1);
}

// R5: a QPixmap, a grab and widget fonts belong to the GUI thread: the pool only ever sees strokes and shapes.
TEST(InkCanvasTest, TextBoxesAreRenderedOnTheUIThreadOnly)
{
    CanvasFixture f;
    TextStub stub;
    f.canvas.setTextRenderer(stub.renderer());
    f.canvas.document().addStroke(line(10, 10, 300, 30, 4.0));
    addTextBox(f.canvas, QRectF(50, 100, 200, 60), QStringLiteral("hello"));
    settledGrab(f.canvas); // the pixmap is rendered (and cached) here, on this thread
    ASSERT_EQ(stub.calls, 1);
    std::atomic<int> jobs{0};
    std::atomic<bool> rendererCalledInsideAJob{false};
    f.canvas.setTileJobHook([&] {
        ++jobs;
        const int before = stub.calls;
        QThread::msleep(30);
        if (stub.calls != before)
            rendererCalledInsideAJob = true;
    });
    f.canvas.documentReplaced(); // every tile is stale: the pool renders them again, with the text box in the page
    settledGrab(f.canvas);
    EXPECT_GT(jobs, 0) << "the tiles were rendered on the pool";
    EXPECT_FALSE(rendererCalledInsideAJob);
    EXPECT_EQ(stub.calls, 1) << "no tile job rendered the text";
    for (QThread *thread : stub.threads)
        EXPECT_EQ(thread, QThread::currentThread());
}

TEST(InkCanvasTest, TheEraserAndTheSelectionHitATextBoxByItsRectNotItsBoundingBoxOrCentre)
{
    CanvasFixture f;
    const qint64 t = addTextBox(f.canvas, QRectF(100, 100, 100, 60), QStringLiteral("box"));
    f.canvas.setTool(InkCanvas::Tool::Eraser);
    f.canvas.setEraserWidth(20.0);
    penDrag(&f.canvas, {{40, 100}, {100, 40}}, 1000); // its box meets the text box's corner, the eraser itself does not
    EXPECT_NE(f.canvas.document().item(t), nullptr) << "a diagonal pass beside the corner leaves the box";
    penDrag(&f.canvas, {{110, 105}, {120, 105}}, 2000); // inside the rect, far from the centre
    EXPECT_EQ(f.canvas.document().item(t), nullptr) << "the eraser takes the box wherever its rect is touched";
    ASSERT_TRUE(f.canvas.undo());
    ASSERT_NE(f.canvas.document().item(t), nullptr);

    f.canvas.setTool(InkCanvas::Tool::Lasso);
    f.canvas.setSelectShape(InkCanvas::SelectShape::Rect);
    penDrag(&f.canvas, {{105, 105}, {125, 120}}, 3000); // wholly inside the box, not over its centre or its edge
    EXPECT_TRUE(f.canvas.selection().empty()) << "ink drawn over a text box can be lassoed without taking the box";
    penDrag(&f.canvas, {{180, 90}, {230, 130}}, 4000); // across its right edge
    EXPECT_EQ(f.canvas.selection(), std::vector<qint64>{t});
    f.canvas.clearSelection();
    penDrag(&f.canvas, {{120, 120}, {180, 140}}, 5000); // over the centre (150, 130)
    EXPECT_EQ(f.canvas.selection(), std::vector<qint64>{t});
}

TEST(InkCanvasTest, TextBoxesSurviveCopyAndPaste)
{
    CanvasFixture f;
    addTextBox(f.canvas, QRectF(50, 150, 100, 40), QStringLiteral("# Title\n- [[Lecture 3]]"));
    f.canvas.setTool(InkCanvas::Tool::Lasso);
    penDrag(&f.canvas, {{40, 140}, {160, 140}, {160, 200}, {40, 200}, {40, 142}}, 1000);
    ASSERT_EQ(f.canvas.selection().size(), 1u);
    ASSERT_TRUE(f.canvas.copySelection());
    ASSERT_TRUE(f.canvas.paste());
    ASSERT_EQ(f.canvas.document().items().size(), 2u);
    const Item &pasted = f.canvas.document().items()[1];
    ASSERT_EQ(pasted.type, Item::Type::TextBox);
    EXPECT_EQ(pasted.textBox->markdown, QStringLiteral("# Title\n- [[Lecture 3]]"));
    EXPECT_EQ(pasted.textBox->rect.size(), QSizeF(100, 40));
    EXPECT_EQ(f.canvas.selection(), std::vector<qint64>{pasted.t});
}

TEST(InkCanvasTest, ResizingATextBoxReflowsItsTextAndKeepsTheMarkdown)
{
    CanvasFixture f;
    TextStub stub;
    f.canvas.setTextRenderer(stub.renderer());
    const qint64 t = addTextBox(f.canvas, QRectF(50, 50, 100, 40), QStringLiteral("reflow"));
    f.canvas.setTool(InkCanvas::Tool::Lasso);
    penDrag(&f.canvas, {{40, 40}, {160, 40}, {160, 100}, {40, 100}, {40, 42}}, 1000);
    ASSERT_EQ(f.canvas.selection().size(), 1u);
    f.canvas.grab();
    ASSERT_EQ(stub.calls, 1);
    penDrag(&f.canvas, {{150, 90}, {250, 90}}, 2000); // the bottom-right handle, 100 px to the right
    const Item *box = f.canvas.document().item(t);
    ASSERT_NE(box, nullptr);
    EXPECT_NEAR(box->textBox->rect.width(), 200.0, 1.0) << "the box is wider; the text is not scaled";
    EXPECT_EQ(box->textBox->markdown, QStringLiteral("reflow"));
    f.canvas.grab();
    EXPECT_EQ(stub.calls, 2);
    EXPECT_EQ(stub.lastSize.width(), qRound(box->textBox->rect.width()));
}

TEST(InkCanvasTest, ReadOnlyCanvasOpensNoEditorAndEndingReadOnlyCommits)
{
    CanvasFixture f;
    const qint64 t = addTextBox(f.canvas, QRectF(50, 50, 200, 60), QStringLiteral("old"));
    f.canvas.setTool(InkCanvas::Tool::Text);
    click(f.canvas, QPoint(100, 70));
    ASSERT_TRUE(f.canvas.isEditingText());
    QTest::keyClicks(textEditorOf(f.canvas), QStringLiteral("!"));
    f.canvas.setReadOnly(true);
    EXPECT_FALSE(f.canvas.isEditingText());
    EXPECT_EQ(f.canvas.document().item(t)->textBox->markdown, QStringLiteral("old!")) << "what was typed is kept";
    click(f.canvas, QPoint(100, 70));
    EXPECT_FALSE(f.canvas.isEditingText());
    EXPECT_FALSE(f.canvas.beginEditingText(t));
}

TEST(InkCanvasTest, BeginEditingTextRefusesAnItemThatIsNotATextBox)
{
    CanvasFixture f;
    const qint64 stroke = f.canvas.document().addStroke(line(10, 10, 50, 50));
    EXPECT_FALSE(f.canvas.beginEditingText(stroke));
    EXPECT_FALSE(f.canvas.beginEditingText(9999));
    EXPECT_FALSE(f.canvas.isEditingText());
}

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
