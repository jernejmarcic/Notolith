// The ink core (docs/superpowers/specs/2026-10-21-ink-core-design.md section 7
// and 12): model and history, the .ink format, the outline composer, input,
// the canvas. Pure functions and widgets on the offscreen platform.
#include <gtest/gtest.h>

#include <QApplication>

#include "InkDocument.h"
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
    Shape line;
    line.kind = ShapeKind::Line;
    line.style.width = 4.0;
    line.points = {QPointF(0, 0), QPointF(100, 50)};
    EXPECT_EQ(line.bounds(), QRectF(-2, -2, 104, 54)) << "every point, grown by half the width";
    Shape arrow = line;
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

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
