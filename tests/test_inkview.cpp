// InkView without dlopen (the static noteAppC_ink linked in, as Track S1's tests do): what the tab does
// about a file that changes under it (the vault is Syncthing-synced) and about a newer file's read-only
// state. The window-level behaviour of the same view is in test_window.cpp.
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>

#include <QApplication>
#include <QDateTime>
#include <QComboBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QFile>
#include <QLabel>
#include <QMenu>
#include <QPlainTextEdit>
#include <QSignalSpy>
#include <QToolBar>
#include <QTemporaryDir>

#include <QScrollArea>
#include <QScrollBar>
#include <QtTest/QTest>

#include "InkTestSupport.h"
#include "InkView.h"
#include "MarkdownEditor.h"
#include "WaitFor.h"
#include "core/InkFormat.h"

using namespace noteAppC::ink;
using namespace inktest;

namespace {

std::shared_ptr<const Stroke> line(double x0, double y0, double x1, double y1)
{
    auto stroke = std::make_shared<Stroke>();
    stroke->style.width = 2.0;
    stroke->points = {{x0, y0, 0.5}, {x1, y1, 0.5}};
    return stroke;
}

QByteArray documentWith(int strokes)
{
    InkDocument doc;
    for (int i = 0; i < strokes; ++i)
        doc.addStroke(line(10 + i, 10, 50 + i, 50));
    doc.commit();
    return writeInk(doc);
}

/// What another machine's sync does: replaces the bytes and gives the file a later modification time.
void replaceExternally(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    ASSERT_EQ(file.write(bytes), bytes.size());
    ASSERT_TRUE(file.setFileTime(QDateTime::currentDateTime().addSecs(5), QFileDevice::FileModificationTime));
}

QByteArray readAll(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

QStringList conflictFiles(const QString &dir)
{
    return QDir(dir).entryList({QStringLiteral("Sketch (conflict *).ink")}, QDir::Files);
}

class InkViewFileTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        ASSERT_TRUE(dir.isValid());
        path = dir.filePath(QStringLiteral("Sketch.ink"));
        QFile file(path);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        file.write(documentWith(1));
    }
    QTemporaryDir dir;
    QString path;
};

} // namespace

TEST_F(InkViewFileTest, ANewerFileIsReadOnlyFlaggedAnnouncedAndNeverWritten)
{
    const QByteArray newer = "{\"format\":\"notolith-ink\",\"version\":2,\"document\":{},\"items\":[]}\n";
    {
        QFile file(path);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write(newer);
    }
    InkView view(nullptr);
    QSignalSpy status(&view, &noteAppC::ModuleView::statusMessage);
    QString error;
    ASSERT_TRUE(view.load(path, &error)) << error.toStdString();
    EXPECT_TRUE(view.isReadOnly());
    EXPECT_TRUE(view.property("readOnly").toBool()) << "the flag a test without the class can read";
    ASSERT_EQ(status.count(), 1);
    EXPECT_TRUE(status.first().first().toString().contains(QStringLiteral("read-only")));
    view.resize(320, 500); // narrower than the toolbar: the notice is a banner above the canvas, never in the toolbar's overflow menu
    view.show();
    auto *label = view.findChild<QLabel *>(QStringLiteral("inkReadOnlyLabel"));
    ASSERT_NE(label, nullptr);
    EXPECT_FALSE(label->isHidden()) << "a persistent indicator, not only a status message";
    auto *toolBar = view.findChild<QToolBar *>();
    ASSERT_NE(toolBar, nullptr);
    EXPECT_FALSE(toolBar->isAncestorOf(label)) << "outside the toolbar";
    EXPECT_TRUE(label->isVisible()) << "on screen at any width";
    EXPECT_FALSE(view.findChild<QAction *>(QStringLiteral("inkTool_pen"))->isEnabled());
    EXPECT_FALSE(view.findChild<QAction *>(QStringLiteral("inkTool_eraser"))->isEnabled());
    EXPECT_FALSE(view.findChild<QAction *>(QStringLiteral("inkEraserSplitAction"))->isEnabled());
    EXPECT_FALSE(view.findChild<QAction *>(QStringLiteral("inkTool_lasso"))->isEnabled());
    EXPECT_FALSE(view.findChild<QAction *>(QStringLiteral("inkSelectRectAction"))->isEnabled());
    EXPECT_FALSE(view.findChild<QAction *>(QStringLiteral("inkTool_shape"))->isEnabled());
    EXPECT_FALSE(view.findChild<QAction *>(QStringLiteral("inkTool_text"))->isEnabled());
    EXPECT_FALSE(view.save(&error));
    EXPECT_EQ(readAll(path), newer);
}

TEST_F(InkViewFileTest, AnExternalChangeWithoutLocalEditsIsReloadedNotOverwritten)
{
    InkView view(nullptr);
    QString error;
    ASSERT_TRUE(view.load(path, &error));
    ASSERT_EQ(view.canvas()->document().items().size(), 1u);
    const QByteArray theirs = documentWith(3);
    replaceExternally(path, theirs);
    QSignalSpy status(&view, &noteAppC::ModuleView::statusMessage);
    EXPECT_TRUE(view.save(&error)) << error.toStdString();
    EXPECT_EQ(readAll(path), theirs) << "their file is untouched";
    EXPECT_EQ(view.canvas()->document().items().size(), 3u) << "the view shows what is on disk now";
    EXPECT_FALSE(view.isModified());
    EXPECT_TRUE(conflictFiles(dir.path()).isEmpty());
    ASSERT_GE(status.count(), 1);
    EXPECT_TRUE(status.last().first().toString().contains(QStringLiteral("changed on disk")));
}

TEST_F(InkViewFileTest, AnExternalChangeWithLocalEditsKeepsBothVersions)
{
    InkView view(nullptr);
    QString error;
    ASSERT_TRUE(view.load(path, &error));
    view.canvas()->document().addStroke(line(100, 100, 200, 200));
    view.canvas()->document().commit();
    ASSERT_TRUE(view.isModified());
    const QByteArray theirs = documentWith(3);
    replaceExternally(path, theirs);
    QSignalSpy status(&view, &noteAppC::ModuleView::statusMessage);
    EXPECT_TRUE(view.save(&error)) << error.toStdString();
    EXPECT_EQ(readAll(path), theirs) << "the original on disk is untouched";
    const QStringList conflicts = conflictFiles(dir.path());
    ASSERT_EQ(conflicts.size(), 1);
    InkDocument mine;
    ASSERT_TRUE(readInk(readAll(dir.filePath(conflicts.first())), mine).ok);
    EXPECT_EQ(mine.items().size(), 2u) << "the conflict file holds the local document: the old stroke and the new one";
    EXPECT_EQ(view.canvas()->document().items().size(), 3u) << "the view now shows the disk version";
    EXPECT_FALSE(view.isModified());
    ASSERT_GE(status.count(), 1);
    EXPECT_TRUE(status.last().first().toString().contains(conflicts.first())) << status.last().first().toString().toStdString();
}

TEST_F(InkViewFileTest, OwnSavesAreNotConflicts)
{
    InkView view(nullptr);
    QString error;
    ASSERT_TRUE(view.load(path, &error));
    for (int i = 0; i < 3; ++i) {
        view.canvas()->document().addStroke(line(20 * i, 5, 20 * i + 10, 60));
        view.canvas()->document().commit();
        ASSERT_TRUE(view.save(&error)) << error.toStdString();
    }
    EXPECT_TRUE(conflictFiles(dir.path()).isEmpty());
    InkDocument onDisk;
    ASSERT_TRUE(readInk(readAll(path), onDisk).ok);
    EXPECT_EQ(onDisk.items().size(), 4u);
}

namespace {

QByteArray bigDocument() { return documentWith(3000); } // above kOffThreadParseBytes: parsed on the pool

/// Bigger than kOffThreadParseBytes and not JSON: the pool's parse fails.
QByteArray corruptBigFile()
{
    return QByteArray("{\"format\":\"notolith-ink\",\"version\":1,\"document\":{},\"items\":[") + QByteArray(300 * 1024, 'x');
}

void writeFile(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    ASSERT_EQ(file.write(bytes), bytes.size());
}

} // namespace

TEST_F(InkViewFileTest, ACorruptBigFileLeavesAReadOnlyTabWithAPersistentError)
{
    // I2a: load() returned true before the parse failed; the tab must not be a blank, drawable, unsavable canvas.
    writeFile(path, corruptBigFile());
    InkView view(nullptr);
    QSignalSpy status(&view, &noteAppC::ModuleView::statusMessage);
    QString error;
    ASSERT_TRUE(view.load(path, &error));
    ASSERT_TRUE(view.isLoading());
    WAIT_TRUE(!view.isLoading(), 5000);
    view.show();
    EXPECT_TRUE(view.canvas()->isReadOnly());
    EXPECT_FALSE(view.findChild<QAction *>(QStringLiteral("inkTool_pen"))->isEnabled());
    EXPECT_FALSE(view.findChild<QAction *>(QStringLiteral("inkTool_eraser"))->isEnabled());
    EXPECT_FALSE(view.findChild<QAction *>(QStringLiteral("inkEraserSplitAction"))->isEnabled());
    EXPECT_FALSE(view.findChild<QAction *>(QStringLiteral("inkTool_lasso"))->isEnabled());
    EXPECT_FALSE(view.findChild<QAction *>(QStringLiteral("inkSelectRectAction"))->isEnabled());
    auto *label = view.findChild<QLabel *>(QStringLiteral("inkErrorLabel"));
    ASSERT_NE(label, nullptr);
    EXPECT_TRUE(label->isVisible()) << "a persistent notice, not only an 8 s status message";
    EXPECT_FALSE(view.findChild<QToolBar *>()->isAncestorOf(label)) << "outside the toolbar, so it cannot fall into its overflow menu";
    EXPECT_TRUE(label->text().contains(QStringLiteral("Sketch.ink"))) << label->text().toStdString();
    ASSERT_GE(status.count(), 1);
    penStroke(view.canvas(), QPointF(50, 50), QPointF(120, 90), 1000);
    EXPECT_FALSE(view.isModified()) << "the pen is ignored";
    EXPECT_FALSE(view.save(&error));
    EXPECT_EQ(readAll(path), corruptBigFile()) << "the unreadable file is left as it was";
}

TEST_F(InkViewFileTest, AFailedReloadIsLookedAtOnceNotAtEveryAutosave)
{
    // I2b: the stamp of the unreadable file is taken, or every autosave sees "changed on disk" and writes another conflict file.
    writeFile(path, bigDocument());
    InkView view(nullptr);
    QString error;
    ASSERT_TRUE(view.load(path, &error));
    WAIT_TRUE(!view.isLoading(), 5000);
    ASSERT_EQ(view.canvas()->document().items().size(), 3000u);
    view.canvas()->document().addStroke(line(100, 100, 200, 200));
    view.canvas()->document().commit();
    ASSERT_TRUE(view.isModified());
    replaceExternally(path, corruptBigFile());
    EXPECT_TRUE(view.save(&error)) << error.toStdString(); // writes the conflict file, starts the reload
    WAIT_TRUE(!view.isLoading(), 5000);
    EXPECT_EQ(conflictFiles(dir.path()).size(), 1);
    EXPECT_TRUE(view.save(&error));
    EXPECT_TRUE(view.save(&error));
    EXPECT_EQ(conflictFiles(dir.path()).size(), 1) << "not one per autosave";
}

TEST_F(InkViewFileTest, ATabClosedDuringAParseIsSafe)
{
    writeFile(path, bigDocument());
    for (int i = 0; i < 3; ++i) {
        auto *view = new InkView(nullptr);
        QString error;
        ASSERT_TRUE(view->load(path, &error));
        ASSERT_TRUE(view->isLoading());
        delete view; // the parse is still running: its result goes nowhere
    }
    QTest::qWait(300);
    SUCCEED();
}

TEST_F(InkViewFileTest, ASmallFileThatFailsAfterAnInterruptedBigLoadDoesNotLeaveTheCanvasReadOnly)
{
    // M2: the big load made the canvas read-only for its parse; a small load that fails must give the pen back.
    const QString big = dir.filePath(QStringLiteral("Big.ink"));
    writeFile(big, bigDocument());
    const QString broken = dir.filePath(QStringLiteral("Broken.ink"));
    writeFile(broken, "{not json");
    InkView view(nullptr);
    QString error;
    ASSERT_TRUE(view.load(big, &error));
    ASSERT_TRUE(view.isLoading());
    EXPECT_FALSE(view.load(broken, &error));
    EXPECT_FALSE(view.isLoading());
    EXPECT_FALSE(view.canvas()->isReadOnly());
}

namespace {

/// A view over an .ink file with the given layout and extent, shown in a 400x300 window.
struct SheetView {
    QTemporaryDir dir;
    InkView view{nullptr};
    QScrollArea *area = nullptr;
    SheetView(LayoutKind layout, const QRectF &extent, double zoom = 1.0)
    {
        EXPECT_TRUE(dir.isValid());
        InkDocument doc;
        DocumentSettings s;
        s.layout = layout;
        s.format = {400, 300, 96};
        s.extent = extent;
        EXPECT_TRUE(doc.setSettings(s));
        doc.commit();
        const QString path = dir.filePath(QStringLiteral("Sheet.ink"));
        QFile file(path);
        EXPECT_TRUE(file.open(QIODevice::WriteOnly));
        file.write(writeInk(doc));
        file.close();
        QString error;
        EXPECT_TRUE(view.load(path, &error)) << error.toStdString();
        view.canvas()->setZoom(zoom);
        view.resize(400, 300);
        view.show();
        EXPECT_TRUE(QTest::qWaitForWindowExposed(&view));
        area = view.findChild<QScrollArea *>();
        EXPECT_NE(area, nullptr);
        StrokeStyle style;
        style.width = 8.0;
        style.color = QColor(0, 0, 0);
        view.canvas()->setPenStyle(style);
    }
    /// Where document point `p` is in the view's own coordinates (what the user sees), not rounded.
    QPointF onScreen(const QPointF &p) const
    {
        return QPointF(view.canvas()->mapTo(&view, QPoint(0, 0))) + view.canvas()->toWidget(p);
    }
    QScrollBar *h() const { return area->horizontalScrollBar(); }
    QScrollBar *v() const { return area->verticalScrollBar(); }
};

void expectNear(const QPointF &a, const QPointF &b, double tolerance, const char *what)
{
    EXPECT_NEAR(a.x(), b.x(), tolerance) << what;
    EXPECT_NEAR(a.y(), b.y(), tolerance) << what;
}

} // namespace

TEST(InkViewLayoutTest, APageLayoutStaysCentredAndASheetLayoutIsLeftAligned)
{
    SheetView paged(LayoutKind::ContinuousVertical, QRectF(0, 0, 400, 300));
    EXPECT_TRUE(paged.area->alignment() & Qt::AlignHCenter);
    SheetView infinite(LayoutKind::Infinite, QRectF(0, 0, 2000, 2000));
    EXPECT_TRUE(infinite.area->alignment() & Qt::AlignLeft);
    EXPECT_FALSE(infinite.area->alignment() & Qt::AlignHCenter);
    SheetView semi(LayoutKind::SemiInfinite, QRectF(0, 0, 2000, 2000));
    EXPECT_TRUE(semi.area->alignment() & Qt::AlignLeft);
}

TEST(InkViewLayoutTest, GrowingLeftOnALargeSheetKeepsTheSameInkInPlaceThroughUndoAndRedo)
{
    for (double zoom : {1.0, 1.5}) {
        SheetView f(LayoutKind::Infinite, QRectF(0, 0, 2000, 2000), zoom);
        InkCanvas *canvas = f.view.canvas();
        ASSERT_GT(f.v()->maximum(), 250);
        f.v()->setValue(250);
        ASSERT_EQ(f.h()->value(), 0);
        const QPointF probe(40, 280); // visible: the view shows document y 250/zoom and below
        const QPointF before = f.onScreen(probe);
        const double tolerance = zoom == 1.0 ? 1e-9 : 1.0;
        penStroke(canvas, QPointF(2, 330), QPointF(80, 330), 1000); // pokes out past the left edge only
        ASSERT_EQ(canvas->document().items().size(), 1u);
        const int shift = f.h()->value();
        EXPECT_GT(shift, 0) << "the view scrolled right by the growth";
        EXPECT_NEAR(shift, -canvas->document().settings().extent.left() * zoom, 1.0) << "by exactly how far the content moved";
        EXPECT_EQ(f.v()->value(), 250) << "no vertical shift: the top did not grow";
        expectNear(f.onScreen(probe), before, tolerance, "same ink after the growth");
        ASSERT_TRUE(canvas->undo());
        EXPECT_EQ(f.h()->value(), 0);
        expectNear(f.onScreen(probe), before, tolerance, "same ink after undo");
        ASSERT_TRUE(canvas->redo());
        EXPECT_EQ(f.h()->value(), shift);
        expectNear(f.onScreen(probe), before, tolerance, "same ink after redo");
    }
}

TEST(InkViewLayoutTest, GrowingUpOnALargeSheetKeepsTheSameInkInPlaceThroughUndoAndRedo)
{
    SheetView f(LayoutKind::Infinite, QRectF(0, 0, 2000, 2000));
    InkCanvas *canvas = f.view.canvas();
    f.h()->setValue(250);
    ASSERT_EQ(f.v()->value(), 0);
    const QPointF probe(280, 40);
    const QPointF before = f.onScreen(probe);
    penStroke(canvas, QPointF(330, 2), QPointF(330, 80), 1000);
    ASSERT_EQ(canvas->document().items().size(), 1u);
    const int shift = f.v()->value();
    EXPECT_GT(shift, 0);
    EXPECT_EQ(f.h()->value(), 250);
    expectNear(f.onScreen(probe), before, 1e-9, "same ink after the growth");
    ASSERT_TRUE(canvas->undo());
    EXPECT_EQ(f.v()->value(), 0);
    expectNear(f.onScreen(probe), before, 1e-9, "same ink after undo");
    ASSERT_TRUE(canvas->redo());
    EXPECT_EQ(f.v()->value(), shift);
    expectNear(f.onScreen(probe), before, 1e-9, "same ink after redo");
}

TEST(InkViewLayoutTest, ASheetSmallerThanTheViewportShiftsByWhatTheScrollRangeCannotTake)
{
    // 200 px wide in a 400 px viewport: left-aligned, no scroll range. The growth makes it wider than the
    // viewport, but the range is only widget - viewport, so part of the shift stays on screen. Pinned, not hidden.
    SheetView f(LayoutKind::Infinite, QRectF(0, 0, 200, 150));
    InkCanvas *canvas = f.view.canvas();
    ASSERT_EQ(f.h()->maximum(), 0);
    const QPointF probe(40, 100);
    const QPointF before = f.onScreen(probe);
    penStroke(canvas, QPointF(2, 100), QPointF(80, 100), 1000);
    ASSERT_EQ(canvas->document().items().size(), 1u);
    const int shift = int(std::lround(-canvas->document().settings().extent.left()));
    EXPECT_EQ(f.h()->value(), f.h()->maximum()) << "scrolled as far as it can";
    EXPECT_EQ(f.h()->value(), std::min(shift, canvas->width() - f.area->viewport()->width()));
    const QPointF after = f.onScreen(probe);
    EXPECT_NEAR(after.x() - before.x(), shift - f.h()->value(), 1e-9) << "the ink moves by the part of the shift the range lacks";
    EXPECT_NEAR(after.x() - before.x(), f.area->viewport()->width() - 200, 1.5) << "which is the empty space that was to its right";
    EXPECT_EQ(after.y(), before.y());
    EXPECT_EQ(f.v()->value(), 0);
}

TEST(InkViewLayoutTest, LoadingAnotherDocumentDoesNotScrollToCompensate)
{
    SheetView f(LayoutKind::Infinite, QRectF(0, 0, 2000, 2000));
    f.v()->setValue(300);
    InkDocument other;
    DocumentSettings s;
    s.layout = LayoutKind::Infinite;
    s.format = {400, 300, 96};
    s.extent = QRectF(-1500, -1500, 3000, 3000);
    ASSERT_TRUE(other.setSettings(s));
    other.commit();
    const QString path = f.dir.filePath(QStringLiteral("Other.ink"));
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.write(writeInk(other));
    file.close();
    QString error;
    ASSERT_TRUE(f.view.load(path, &error));
    EXPECT_EQ(f.v()->value(), 300) << "a different file is not a shift of the old one";
    EXPECT_EQ(f.h()->value(), 0);
}

TEST(InkViewToolbarTest, TheEraseWholeOrPartsToggleIsOnTheToolbarAndOnShiftE)
{
    InkView view(nullptr);
    auto *toggle = view.findChild<QAction *>(QStringLiteral("inkEraserSplitAction"));
    ASSERT_NE(toggle, nullptr);
    EXPECT_TRUE(toggle->isCheckable());
    EXPECT_FALSE(toggle->isChecked());
    EXPECT_EQ(view.canvas()->eraserMode(), InkCanvas::EraserMode::Stroke);
    toggle->trigger();
    EXPECT_EQ(view.canvas()->eraserMode(), InkCanvas::EraserMode::Split) << "the toolbar drives the canvas";
    QTest::keyClick(view.canvas(), Qt::Key_E, Qt::ShiftModifier);
    EXPECT_EQ(view.canvas()->eraserMode(), InkCanvas::EraserMode::Stroke);
    EXPECT_FALSE(toggle->isChecked()) << "the key moves the toolbar too";
    QTest::keyClick(view.canvas(), Qt::Key_E, Qt::ShiftModifier);
    EXPECT_TRUE(toggle->isChecked());
}

TEST(InkViewToolbarTest, SelectAndRectangleSelectAreOnTheToolbarAndOnTheirKeys)
{
    InkView view(nullptr);
    auto *select = view.findChild<QAction *>(QStringLiteral("inkTool_lasso"));
    auto *rect = view.findChild<QAction *>(QStringLiteral("inkSelectRectAction"));
    ASSERT_NE(select, nullptr);
    ASSERT_NE(rect, nullptr);
    EXPECT_TRUE(select->isCheckable());
    EXPECT_TRUE(rect->isCheckable());
    select->trigger();
    EXPECT_EQ(view.canvas()->tool(), InkCanvas::Tool::Lasso);
    EXPECT_EQ(view.canvas()->selectShape(), InkCanvas::SelectShape::Lasso);
    rect->trigger();
    EXPECT_EQ(view.canvas()->selectShape(), InkCanvas::SelectShape::Rect) << "the toolbar drives the canvas";
    EXPECT_EQ(view.canvas()->tool(), InkCanvas::Tool::Lasso);
    QTest::keyClick(view.canvas(), Qt::Key_P);
    EXPECT_FALSE(select->isChecked()) << "the key moves the toolbar too";
    EXPECT_TRUE(view.findChild<QAction *>(QStringLiteral("inkTool_pen"))->isChecked());
    QTest::keyClick(view.canvas(), Qt::Key_L, Qt::ShiftModifier);
    EXPECT_EQ(view.canvas()->tool(), InkCanvas::Tool::Lasso);
    EXPECT_EQ(view.canvas()->selectShape(), InkCanvas::SelectShape::Lasso);
    EXPECT_FALSE(rect->isChecked());
    EXPECT_TRUE(select->isChecked());
    QTest::keyClick(view.canvas(), Qt::Key_L, Qt::ShiftModifier);
    EXPECT_TRUE(rect->isChecked());
}

TEST(InkViewToolbarTest, TheShapeToolIsOnTheToolbarWithAMenuOfFourKindsAndOnItsKeys)
{
    InkView view(nullptr);
    auto *shape = view.findChild<QAction *>(QStringLiteral("inkTool_shape"));
    ASSERT_NE(shape, nullptr);
    EXPECT_TRUE(shape->isCheckable());
    QAction *kinds[4] = {};
    const char *names[4] = {"inkShapeLineAction", "inkShapeArrowAction", "inkShapeRectAction", "inkShapeEllipseAction"};
    for (int i = 0; i < 4; ++i) {
        kinds[i] = view.findChild<QAction *>(QLatin1String(names[i]));
        ASSERT_NE(kinds[i], nullptr) << names[i];
        EXPECT_TRUE(kinds[i]->isCheckable());
    }
    ASSERT_NE(shape->menu(), nullptr);
    EXPECT_EQ(shape->menu()->actions().size(), 4);
    EXPECT_TRUE(kinds[0]->isChecked()) << "a line to begin with";
    EXPECT_FALSE(shape->isChecked());
    shape->trigger();
    EXPECT_EQ(view.canvas()->tool(), InkCanvas::Tool::Shape);
    EXPECT_TRUE(shape->isChecked());
    EXPECT_FALSE(view.findChild<QAction *>(QStringLiteral("inkTool_pen"))->isChecked()) << "not the pen in disguise";
    view.findChild<QAction *>(QStringLiteral("inkTool_eraser"))->trigger();
    EXPECT_FALSE(shape->isChecked());
    kinds[3]->trigger();
    EXPECT_EQ(view.canvas()->shapeKind(), ShapeKind::Ellipse) << "the menu drives the canvas";
    EXPECT_EQ(view.canvas()->tool(), InkCanvas::Tool::Shape) << "and one click is enough: it also picks the tool";
    EXPECT_TRUE(shape->isChecked());
    QTest::keyClick(view.canvas(), Qt::Key_S, Qt::ShiftModifier);
    EXPECT_EQ(view.canvas()->shapeKind(), ShapeKind::Line);
    EXPECT_TRUE(kinds[0]->isChecked()) << "the key moves the menu too";
    EXPECT_FALSE(kinds[3]->isChecked());
    QTest::keyClick(view.canvas(), Qt::Key_P);
    EXPECT_FALSE(shape->isChecked());
    EXPECT_TRUE(view.findChild<QAction *>(QStringLiteral("inkTool_pen"))->isChecked());
    QTest::keyClick(view.canvas(), Qt::Key_S);
    EXPECT_TRUE(shape->isChecked()) << "S picks the tool, whatever the kind";
}

// Closing a tab asks isModified(), then save(): text still being typed is neither committed nor in the document yet.
TEST_F(InkViewFileTest, TextStillBeingTypedCountsAsUnsavedAndIsSavedWithTheTab)
{
    InkView view(nullptr);
    QString error;
    ASSERT_TRUE(view.load(path, &error)) << error.toStdString();
    view.resize(800, 600);
    view.show();
    ASSERT_TRUE(QTest::qWaitForWindowExposed(&view));
    view.canvas()->setTool(InkCanvas::Tool::Text);
    QTest::mouseClick(view.canvas(), Qt::LeftButton, Qt::NoModifier, QPoint(60, 400));
    auto *edit = view.canvas()->findChild<noteAppC::MarkdownEditor *>(QStringLiteral("inkTextEditor"));
    ASSERT_NE(edit, nullptr) << "the host's note editor (I3), not a plain-text one";
    EXPECT_EQ(view.canvas()->findChild<QPlainTextEdit *>(QStringLiteral("inkTextEditor")), nullptr);
    EXPECT_FALSE(view.isModified()) << "an editor with nothing typed is not an edit";
    QTest::keyClicks(edit, QStringLiteral("unsaved thought"));
    EXPECT_TRUE(view.isModified());
    ASSERT_TRUE(view.save(&error)) << error.toStdString();
    EXPECT_FALSE(view.isModified());
    EXPECT_TRUE(readAll(path).contains("unsaved thought")) << "save() committed the box first";
}

TEST(InkViewToolbarTest, TheTextToolIsOnTheToolbarAndOnItsKeyAndIsNotThePenInDisguise)
{
    InkView view(nullptr);
    auto *text = view.findChild<QAction *>(QStringLiteral("inkTool_text"));
    auto *pen = view.findChild<QAction *>(QStringLiteral("inkTool_pen"));
    ASSERT_NE(text, nullptr);
    EXPECT_TRUE(text->isCheckable());
    EXPECT_FALSE(text->isChecked());
    text->trigger();
    EXPECT_EQ(view.canvas()->tool(), InkCanvas::Tool::Text);
    EXPECT_TRUE(text->isChecked());
    EXPECT_FALSE(pen->isChecked()) << "Text no longer draws like the pen, nor shows it checked";
    QTest::keyClick(view.canvas(), Qt::Key_P);
    EXPECT_TRUE(pen->isChecked());
    EXPECT_FALSE(text->isChecked());
    QTest::keyClick(view.canvas(), Qt::Key_T);
    EXPECT_EQ(view.canvas()->tool(), InkCanvas::Tool::Text) << "the T key picks it without pen hardware (R14)";
    EXPECT_TRUE(text->isChecked());
    EXPECT_FALSE(pen->isChecked());
}

namespace {

/// A view over a sheet of plain white paper holding text boxes, which the real renderer (the host's note editor) draws.
struct TextSheet {
    QTemporaryDir dir;
    InkView view{nullptr};
    explicit TextSheet(const std::vector<std::pair<QRectF, QString>> &boxes)
    {
        EXPECT_TRUE(dir.isValid());
        InkDocument doc;
        DocumentSettings settings;
        settings.layout = LayoutKind::Fixed;
        settings.format = {1000, 600, 96};
        settings.extent = QRectF(0, 0, 1000, 600);
        settings.background.color = Qt::white;
        settings.background.pattern = Pattern::None;
        EXPECT_TRUE(doc.setSettings(settings));
        for (const auto &[rect, markdown] : boxes) {
            Item box;
            box.type = Item::Type::TextBox;
            box.textBox = std::make_shared<TextBox>(TextBox{rect, markdown});
            doc.addItem(box);
        }
        doc.commit();
        const QString path = dir.filePath(QStringLiteral("Text.ink"));
        QFile file(path);
        EXPECT_TRUE(file.open(QIODevice::WriteOnly));
        file.write(writeInk(doc));
        file.close();
        QString error;
        EXPECT_TRUE(view.load(path, &error)) << error.toStdString();
        EXPECT_TRUE(QTest::qWaitFor([&] { return !view.isLoading(); }, 10000)) << "a big file parses on the pool";
        view.resize(1100, 700);
        QElapsedTimer timer;
        timer.start();
        view.show();
        EXPECT_TRUE(QTest::qWaitForWindowExposed(&view));
        view.canvas()->grab(); // the boxes are rendered by the first paint: exposing the window or this
        paintMs = timer.elapsed();
    }
    qint64 paintMs = 0; ///< showing the view and painting it once
    QImage grab() const { return view.canvas()->grab().toImage(); }
};

/// The x of the first dark pixel (text) in the rows of `box`, relative to its left edge; -1 when there is none.
int firstInkOffset(const QImage &shot, const QRectF &box)
{
    const qreal dpr = shot.devicePixelRatio();
    for (int x = int(box.left() * dpr); x < int(box.right() * dpr); ++x)
        for (int y = int(box.top() * dpr); y < int(box.bottom() * dpr); ++y)
            if (shot.pixelColor(x, y).lightness() < 128)
                return int(std::round(x / dpr - box.left()));
    return -1;
}

} // namespace

// Fix round 1 (I2): the editor's readable line length (a 700 px column, centred) must not apply to a text box.
TEST(InkViewTextTest, AWideBoxStartsItsTextAtTheSameEdgeAsANarrowOne)
{
    const QRectF narrow(20, 20, 300, 80);
    const QRectF wide(20, 150, 780, 80);
    TextSheet sheet({{narrow, QStringLiteral("# Hello")}, {wide, QStringLiteral("# Hello")}});
    const QImage shot = sheet.grab();
    const int a = firstInkOffset(shot, narrow);
    const int b = firstInkOffset(shot, wide);
    ASSERT_GE(a, 0) << "the narrow box shows ink";
    ASSERT_GE(b, 0) << "the wide box shows ink";
    EXPECT_LE(a, 12);
    EXPECT_LE(b, 12) << "the text is at the box's left edge, not centred in a 700 px column";
    EXPECT_NEAR(a, b, 4);
}

// Fix round 1 (I4): a hostile file with a huge box must not freeze the paint.
TEST(InkViewTextTest, ATwoMegabyteBoxRendersTruncatedAndFast)
{
    QString huge;
    huge.reserve(2 * 1024 * 1024);
    for (int line = 0; huge.size() < 2 * 1024 * 1024; ++line)
        huge += QStringLiteral("line %1 of a very long box with some *emphasis* and a [[Link]]\n").arg(line);
    TextSheet sheet({{QRectF(20, 20, 400, 300), huge}});
    const QImage shot = sheet.grab();
    EXPECT_LT(sheet.paintMs, 500) << "the render is cut at a bounded length (took " << sheet.paintMs << " ms)";
    EXPECT_GE(firstInkOffset(shot, QRectF(20, 20, 400, 300)), 0) << "and still draws the start of the text";
}

// I3: the box is edited in the host's editor: Markdown typed into it is committed as the box's text, keys typed into it
// are not tool keys, and its text sits where the rendered picture's does.
TEST(InkViewTextTest, TheEditorOverABoxIsTheNoteEditorAndWhatIsTypedIsCommitted)
{
    const QRectF box(20, 20, 400, 100);
    TextSheet sheet({{box, QStringLiteral("# Hello")}});
    InkCanvas *canvas = sheet.view.canvas();
    canvas->setTool(InkCanvas::Tool::Text);
    const QImage before = sheet.grab();
    ASSERT_GE(firstInkOffset(before, box), 0);
    QTest::mouseClick(canvas, Qt::LeftButton, Qt::NoModifier, QPoint(100, 60));
    ASSERT_TRUE(canvas->isEditingText());
    auto *edit = canvas->findChild<noteAppC::MarkdownEditor *>(QStringLiteral("inkTextEditor"));
    ASSERT_NE(edit, nullptr);
    EXPECT_EQ(edit->noteText(), QStringLiteral("# Hello"));
    EXPECT_EQ(edit->geometry().topLeft(), QPoint(20, 20));
    QTest::keyClicks(edit, QStringLiteral(" slate"));
    EXPECT_EQ(canvas->tool(), InkCanvas::Tool::Text) << "s, l and t typed into the editor are not tool keys";
    QTest::keyClick(edit, Qt::Key_Escape);
    EXPECT_FALSE(canvas->isEditingText());
    ASSERT_EQ(canvas->document().items().size(), 1u);
    EXPECT_EQ(canvas->document().items()[0].textBox->markdown, QStringLiteral("# Hello slate"));
}

namespace {
/// The editor in use: one that was closed lingers hidden until the event loop deletes it.
noteAppC::MarkdownEditor *liveEditor(InkCanvas *canvas)
{
    for (auto *edit : canvas->findChildren<noteAppC::MarkdownEditor *>(QStringLiteral("inkTextEditor")))
        if (!edit->isHidden())
            return edit;
    return nullptr;
}
} // namespace

// Fix round 2 (R1): the pen's tap inside the open editor belongs to the editor. Qt hands a tablet event the canvas
// ignored to the widget under the pen as mouse events; before the fix the canvas took it, committed the box and opened a
// new editor with the caret at the end.
TEST(InkViewTextTest, APenTapInsideTheOpenEditorKeepsTheEditorAndPlacesTheCaret)
{
    const QRectF box(20, 20, 400, 100);
    const QString text = QStringLiteral("hello world, hello pen");
    TextSheet sheet({{box, text}});
    InkCanvas *canvas = sheet.view.canvas();
    canvas->setTool(InkCanvas::Tool::Text);
    QTest::mouseClick(canvas, Qt::LeftButton, Qt::NoModifier, QPoint(100, 60));
    auto *edit = liveEditor(canvas);
    ASSERT_NE(edit, nullptr);
    const qint64 item = canvas->editingTextItem();
    ASSERT_GT(item, 0);
    ASSERT_EQ(edit->cursorOffset(), text.size()) << "a click opens it with the caret at the end";
    const QRectF rect = canvas->document().item(item)->textBox->rect;
    const QPoint inside(4, 10); // viewport coordinates: the first letters
    const auto pen = [&](QEvent::Type type, qint64 ms) {
        QWidget *target = edit->viewport();
        static const QPointingDevice *device = QPointingDevice::primaryPointingDevice(); // any device: the canvas keys on the position
        QTabletEvent event(type, device, QPointF(inside), QPointF(target->mapToGlobal(inside)), 0.5, 0, 0, 0, 0, 0, Qt::NoModifier,
                           type == QEvent::TabletMove ? Qt::NoButton : Qt::LeftButton, type == QEvent::TabletRelease ? Qt::NoButton : Qt::LeftButton);
        event.setTimestamp(quint64(ms));
        QApplication::sendEvent(target, &event);
        return event.isAccepted();
    };
    EXPECT_FALSE(pen(QEvent::TabletPress, 1000)) << "ignored, so that Qt sends the mouse events";
    EXPECT_FALSE(pen(QEvent::TabletRelease, 1010));
    // What Qt does with an ignored tablet event: the mouse press and release at the same place.
    QTest::mouseClick(edit->viewport(), Qt::LeftButton, Qt::NoModifier, inside);
    EXPECT_TRUE(canvas->isEditingText());
    EXPECT_EQ(canvas->editingTextItem(), item);
    EXPECT_EQ(liveEditor(canvas), edit) << "the same editor object, not a reopened one";
    EXPECT_EQ(canvas->document().item(item)->textBox->rect, rect) << "the box did not move";
    EXPECT_LT(edit->cursorOffset(), 6) << "the caret is where the pen tapped, not at the end";
}

TEST(InkViewTextTest, APenTapInAnEmptyNewBoxDoesNotMoveTheBox)
{
    TextSheet sheet({});
    InkCanvas *canvas = sheet.view.canvas();
    canvas->setTool(InkCanvas::Tool::Text);
    QTest::mouseClick(canvas, Qt::LeftButton, Qt::NoModifier, QPoint(100, 60));
    auto *edit = liveEditor(canvas);
    ASSERT_NE(edit, nullptr);
    ASSERT_EQ(canvas->editingTextItem(), -1);
    const QRect where = edit->geometry();
    const QPoint inside(10, 10);
    for (const auto type : {QEvent::TabletPress, QEvent::TabletRelease}) {
        QTabletEvent event(type, QPointingDevice::primaryPointingDevice(), QPointF(inside), QPointF(edit->viewport()->mapToGlobal(inside)), 0.5, 0, 0, 0, 0, 0,
                           Qt::NoModifier, Qt::LeftButton, type == QEvent::TabletRelease ? Qt::NoButton : Qt::LeftButton);
        QApplication::sendEvent(edit->viewport(), &event);
    }
    EXPECT_EQ(liveEditor(canvas), edit);
    EXPECT_EQ(edit->geometry(), where) << "the box stayed where it was started";
}

// The mouse (with drawWithMouse on) and touch must not close the editor either.
TEST(InkViewTextTest, AMouseClickOrATouchInsideTheOpenEditorKeepsIt)
{
    TextSheet sheet({{QRectF(20, 20, 400, 100), QStringLiteral("hello world, hello pen")}});
    InkCanvas *canvas = sheet.view.canvas();
    canvas->setDrawWithMouse(true);
    canvas->setTool(InkCanvas::Tool::Text);
    QTest::mouseClick(canvas, Qt::LeftButton, Qt::NoModifier, QPoint(100, 60));
    auto *edit = liveEditor(canvas);
    ASSERT_NE(edit, nullptr);
    QTest::mouseClick(edit->viewport(), Qt::LeftButton, Qt::NoModifier, QPoint(4, 10));
    EXPECT_EQ(liveEditor(canvas), edit) << "mouse";
    EXPECT_LT(edit->cursorOffset(), 6);
    static QPointingDevice *touch = QTest::createTouchDevice();
    QTest::touchEvent(edit->viewport(), touch).press(0, QPoint(30, 10), edit->viewport()).commit();
    QTest::touchEvent(edit->viewport(), touch).release(0, QPoint(30, 10), edit->viewport()).commit();
    EXPECT_TRUE(canvas->isEditingText());
    EXPECT_EQ(liveEditor(canvas), edit) << "touch";
}

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}

TEST(InkViewToolbarTest, TheSelectButtonHasALassoAndARectangleKindAndTheShapeFaceShowsTheKind)
{
    InkView view(nullptr);
    auto *select = view.findChild<QAction *>(QStringLiteral("inkTool_lasso"));
    auto *lasso = view.findChild<QAction *>(QStringLiteral("inkSelectLassoAction"));
    auto *rect = view.findChild<QAction *>(QStringLiteral("inkSelectRectAction"));
    ASSERT_NE(select, nullptr);
    ASSERT_NE(lasso, nullptr);
    ASSERT_NE(rect, nullptr);
    ASSERT_NE(select->menu(), nullptr);
    EXPECT_EQ(select->menu()->actions().size(), 2);
    EXPECT_TRUE(lasso->isChecked());
    rect->trigger();
    EXPECT_EQ(view.canvas()->selectShape(), InkCanvas::SelectShape::Rect);
    EXPECT_FALSE(lasso->isChecked());
    lasso->trigger();
    EXPECT_EQ(view.canvas()->selectShape(), InkCanvas::SelectShape::Lasso);
    EXPECT_EQ(view.canvas()->tool(), InkCanvas::Tool::Lasso) << "one click is enough";

    auto *shape = view.findChild<QAction *>(QStringLiteral("inkTool_shape"));
    ASSERT_NE(shape, nullptr);
    EXPECT_EQ(shape->text(), QStringLiteral("Line")) << "the face shows the kind";
    view.findChild<QAction *>(QStringLiteral("inkShapeEllipseAction"))->trigger();
    EXPECT_EQ(shape->text(), QStringLiteral("Ellipse"));
    QTest::keyClick(view.canvas(), Qt::Key_S, Qt::ShiftModifier);
    EXPECT_EQ(shape->text(), QStringLiteral("Line"));
    QTest::keyClick(view.canvas(), Qt::Key_S, Qt::ShiftModifier);
    EXPECT_EQ(view.canvas()->shapeKind(), ShapeKind::Arrow) << "from Line the next kind, not Line again";
    EXPECT_EQ(shape->text(), QStringLiteral("Arrow"));
}

TEST_F(InkViewFileTest, ALeftDragWithTheMouseScrollsTheAreaUnlessDrawWithMouseIsOn)
{
    InkView view(nullptr);
    QString error;
    ASSERT_TRUE(view.load(path, &error)) << error.toStdString();
    view.resize(500, 300);
    view.show();
    ASSERT_TRUE(QTest::qWaitForWindowExposed(&view));
    auto *area = view.findChild<QScrollArea *>();
    ASSERT_NE(area, nullptr);
    EXPECT_EQ(view.canvas()->cursor().shape(), Qt::OpenHandCursor) << "the mouse cannot draw: a hand";
    ASSERT_GT(area->verticalScrollBar()->maximum(), 100) << "an A4 page in a 300 px view";
    const int before = view.canvas()->document().items().size();
    QTest::mousePress(view.canvas(), Qt::LeftButton, Qt::NoModifier, QPoint(100, 200));
    EXPECT_EQ(view.canvas()->cursor().shape(), Qt::ClosedHandCursor) << "while it is held";
    QTest::mouseMove(view.canvas(), QPoint(100, 150));
    QTest::mouseRelease(view.canvas(), Qt::LeftButton, Qt::NoModifier, QPoint(100, 150));
    EXPECT_EQ(area->verticalScrollBar()->value(), 50) << "the page follows the pointer";
    EXPECT_EQ(view.canvas()->cursor().shape(), Qt::OpenHandCursor);
    EXPECT_EQ(view.canvas()->document().items().size(), size_t(before)) << "nothing was drawn";
    EXPECT_FALSE(view.isModified());
    view.canvas()->setDrawWithMouse(true);
    EXPECT_EQ(view.canvas()->cursor().shape(), Qt::CrossCursor);
    area->verticalScrollBar()->setValue(0);
    QTest::mousePress(view.canvas(), Qt::LeftButton, Qt::NoModifier, QPoint(100, 100));
    QTest::mouseMove(view.canvas(), QPoint(160, 130));
    QTest::mouseRelease(view.canvas(), Qt::LeftButton, Qt::NoModifier, QPoint(160, 130));
    EXPECT_EQ(area->verticalScrollBar()->value(), 0) << "drawing, not scrolling";
    EXPECT_TRUE(view.isModified());
}

TEST_F(InkViewFileTest, APageSettingTheFileCouldNotBeReadBackWithIsRefusedAndNothingChanges)
{
    InkView view(nullptr);
    QString error;
    ASSERT_TRUE(view.load(path, &error)) << error.toStdString();
    QSignalSpy status(&view, &noteAppC::ModuleView::statusMessage);
    auto *width = view.findChild<QDoubleSpinBox *>(QStringLiteral("inkPageWidthSpin"));
    ASSERT_NE(width, nullptr);
    const DocumentSettings before = view.canvas()->document().settings();
    const double shown = width->value();
    width->setRange(-100, 3000); // the spin's own range keeps a person from this; the document refuses it all the same
    width->setValue(-5);
    EXPECT_EQ(view.canvas()->document().settings().format.width, before.format.width) << "setSettings said no: nothing changed";
    EXPECT_FALSE(view.canvas()->document().canUndo()) << "and no history step was made";
    EXPECT_FALSE(view.isModified());
    EXPECT_EQ(width->value(), shown) << "the menu shows the page as it is";
    ASSERT_EQ(status.count(), 1);
    EXPECT_TRUE(status.first().first().toString().contains(QStringLiteral("cannot be used")));
}

TEST_F(InkViewFileTest, EveryPageChangeIsOneUndoStepAndAnExtentFollowsTheFormat)
{
    InkView view(nullptr);
    QString error;
    ASSERT_TRUE(view.load(path, &error)) << error.toStdString();
    InkDocument &doc = view.canvas()->document();
    auto *add = view.findChild<QAction *>(QStringLiteral("inkAddPageAction"));
    ASSERT_NE(add, nullptr);
    add->trigger();
    add->trigger();
    ASSERT_EQ(doc.pageCount(), 3);
    auto *format = view.findChild<QComboBox *>(QStringLiteral("inkPageFormatCombo"));
    ASSERT_NE(format, nullptr);
    format->setCurrentIndex(format->findData(QStringLiteral("letter")));
    EXPECT_EQ(doc.pageCount(), 3) << "the pages stay three, at the new size";
    EXPECT_NEAR(doc.settings().extent.height(), 3 * doc.settings().format.height, 1e-6);
    ASSERT_TRUE(view.canvas()->undo());
    EXPECT_EQ(doc.pageCount(), 3) << "one step took the format back";
    EXPECT_NEAR(doc.settings().format.width, 793.701, 1e-6);
    ASSERT_TRUE(view.canvas()->undo());
    EXPECT_EQ(doc.pageCount(), 2) << "one step per added page";
    EXPECT_EQ(format->currentData().toString(), QStringLiteral("a4")) << "the menu follows";
}
