// PDF viewer and annotation module: PdfDocument's model (annotation
// round-trip through a real PDF, atomic save, recovery), PdfPagesArea's
// widget (layout before any render, the Lifeline guard, selection, undo/
// redo, find) and PdfExtract's note generation.

#include <gtest/gtest.h>

#include <QApplication>
#include <QColor>
#include <QCoreApplication>
#include <QEventLoop>
#include <QFile>
#include <QFont>
#include <QMouseEvent>
#include <QPageSize>
#include <QPainter>
#include <QPdfWriter>
#include <QProcess>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QThreadPool>

#include <memory>

#include "PdfAnnotationCommands.h"
#include "PdfDocument.h"
#include "PdfExtract.h"
#include "PdfPagesArea.h"
#include "PdfRecovery.h"

using namespace noteAppC;

namespace {

/// A real, if minimal, PDF with actual selectable/searchable text - built
/// with QPdfWriter (already a project dependency) rather than relying on
/// pdflatex, so these tests run wherever poppler itself does. Resolution
/// pinned to 72 so a QPainter coordinate is already a PDF point, which
/// keeps the numbers in this file easy to reason about - though every test
/// below still derives its click coordinates from the real word boxes
/// PdfDocument::words() reports, never from this function's own layout, so
/// it would keep working even if that stopped being true.
QString makeTestPdf(const QString &dir, int pages, const QString &name = QStringLiteral("doc.pdf"))
{
    const QString path = dir + QLatin1Char('/') + name;
    QPdfWriter writer(path);
    writer.setPageSize(QPageSize(QPageSize::A4));
    writer.setResolution(72);
    QFont font;
    font.setPointSize(14);
    QPainter painter(&writer);
    painter.setFont(font);
    for (int page = 0; page < pages; ++page) {
        if (page > 0)
            writer.newPage();
        painter.drawText(72, 100, QStringLiteral("Hello world on page %1.").arg(page + 1));
        painter.drawText(72, 140, QStringLiteral("A second line of text to select, page %1.").arg(page + 1));
    }
    painter.end();
    return path;
}

/// A word that appears on exactly one page (the second), for find().
QString makeSearchablePdf(const QString &dir)
{
    const QString path = dir + QStringLiteral("/search.pdf");
    QPdfWriter writer(path);
    writer.setPageSize(QPageSize(QPageSize::A4));
    writer.setResolution(72);
    QFont font;
    font.setPointSize(14);
    QPainter painter(&writer);
    painter.setFont(font);
    painter.drawText(72, 100, QStringLiteral("First page has nothing special on it."));
    writer.newPage();
    painter.drawText(72, 100, QStringLiteral("XYLOPHONE appears on the second page only."));
    painter.end();
    return path;
}

QByteArray readAll(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

/// Converts a point-space rect on `page` (as PdfDocument::words() reports)
/// to the viewport pixel rect PdfPagesArea is currently showing it at -
/// the same maths paintAnnotation uses, built here from public API only
/// (pageLayoutRect + document()->pageSizePoints), so a test can click
/// exactly on real text instead of guessing pixel coordinates.
QRectF screenRectFor(const PdfPagesArea &area, int page, const QRectF &pointsRect)
{
    const QRectF pageRect = area.pageLayoutRect(page);
    const QSizeF sizePts = const_cast<PdfPagesArea &>(area).document()->pageSizePoints(page);
    if (sizePts.width() <= 0 || sizePts.height() <= 0)
        return {};
    return QRectF(pageRect.x() + pointsRect.x() / sizePts.width() * pageRect.width(),
                 pageRect.y() + pointsRect.y() / sizePts.height() * pageRect.height(),
                 pointsRect.width() / sizePts.width() * pageRect.width(),
                 pointsRect.height() / sizePts.height() * pageRect.height());
}

/// Mouse events sent the way Qt Widgets delivers them - press, move (button
/// held, per Qt's own convention that a move event's own button() is
/// always NoButton), release. Copied from tests/test_canvas.cpp's own
/// helper of the same shape.
void send(QWidget *target, QEvent::Type type, QPoint at, Qt::MouseButton button,
          Qt::KeyboardModifiers modifiers = Qt::NoModifier)
{
    const Qt::MouseButton eventButton = type == QEvent::MouseMove ? Qt::NoButton : button;
    const Qt::MouseButtons held = type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::MouseButtons(button);
    QMouseEvent event(type, QPointF(at), target->mapToGlobal(QPointF(at)), eventButton, held, modifiers);
    QCoreApplication::sendEvent(target, &event);
}

void drag(QWidget *target, QPoint from, QPoint to, int steps = 4)
{
    send(target, QEvent::MouseButtonPress, from, Qt::LeftButton);
    for (int i = 1; i <= steps; ++i)
        send(target, QEvent::MouseMove, from + (to - from) * i / steps, Qt::LeftButton);
    send(target, QEvent::MouseButtonRelease, to, Qt::LeftButton);
}

void pump()
{
    QCoreApplication::processEvents();
    QTest::qWait(10);
    QCoreApplication::processEvents();
}

} // namespace

// --- PdfDocument: annotations, save, recovery -----------------------------

TEST(PdfDocumentTest, OpensAndReportsPageGeometry)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString pdf = makeTestPdf(dir.path(), 3);

    PdfDocument doc;
    QString error;
    ASSERT_TRUE(doc.load(pdf, &error)) << error.toStdString();
    EXPECT_EQ(doc.pageCount(), 3);
    const QSizeF size = doc.pageSizePoints(0);
    // A4 in points, allowing for QPdfWriter's own rounding.
    EXPECT_NEAR(size.width(), 595, 2);
    EXPECT_NEAR(size.height(), 842, 2);
}

TEST(PdfDocumentTest, EachAnnotationKindSurvivesSaveAndReopen)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString pdf = makeTestPdf(dir.path(), 1);

    {
        PdfDocument doc;
        ASSERT_TRUE(doc.load(pdf));

        PdfAnnotationData highlight;
        highlight.kind = PdfAnnotationKind::Highlight;
        highlight.page = 0;
        highlight.quads = {QRectF(0.10, 0.10, 0.30, 0.03)};
        highlight.boundary = highlight.quads.first();
        highlight.color = QColor(255, 0, 0);
        doc.addAnnotation(highlight);

        PdfAnnotationData underline = highlight;
        underline.kind = PdfAnnotationKind::Underline;
        underline.boundary.translate(0, 0.06);
        for (QRectF &q : underline.quads)
            q.translate(0, 0.06);
        underline.color = QColor(0, 128, 0);
        doc.addAnnotation(underline);

        PdfAnnotationData strike = highlight;
        strike.kind = PdfAnnotationKind::StrikeOut;
        strike.boundary.translate(0, 0.12);
        for (QRectF &q : strike.quads)
            q.translate(0, 0.12);
        doc.addAnnotation(strike);

        PdfAnnotationData ink;
        ink.kind = PdfAnnotationKind::Ink;
        ink.page = 0;
        ink.strokes = {QPolygonF({QPointF(0.2, 0.5), QPointF(0.25, 0.55), QPointF(0.3, 0.5)})};
        ink.boundary = QRectF(0.2, 0.5, 0.1, 0.05);
        ink.color = QColor(0, 150, 0);
        ink.lineWidthPoints = 3.0;
        doc.addAnnotation(ink);

        PdfAnnotationData rect;
        rect.kind = PdfAnnotationKind::Rectangle;
        rect.page = 0;
        rect.boundary = QRectF(0.4, 0.6, 0.2, 0.1);
        rect.color = QColor(0, 0, 255);
        rect.lineWidthPoints = 2.5;
        doc.addAnnotation(rect);

        PdfAnnotationData note;
        note.kind = PdfAnnotationKind::Note;
        note.page = 0;
        note.boundary = QRectF(0.05, 0.05, 0.02, 0.02);
        note.contents = QStringLiteral("A test comment.");
        note.color = QColor(255, 235, 59);
        doc.addAnnotation(note);

        QString error;
        ASSERT_TRUE(doc.save(&error)) << error.toStdString();
    }

    // Reopened as poppler itself would - a fresh Document, not the one that
    // wrote it - which is the actual claim: "shown by other readers".
    PdfDocument reopened;
    QString error;
    ASSERT_TRUE(reopened.load(pdf, &error)) << error.toStdString();
    const QList<PdfAnnotationData> annotations = reopened.annotations(0);
    ASSERT_EQ(annotations.size(), 6);

    const auto findKind = [&](PdfAnnotationKind kind) -> const PdfAnnotationData * {
        for (const PdfAnnotationData &a : annotations) {
            if (a.kind == kind)
                return &a;
        }
        return nullptr;
    };

    const auto colorNear = [](const QColor &a, const QColor &b) {
        return std::abs(a.red() - b.red()) <= 2 && std::abs(a.green() - b.green()) <= 2
            && std::abs(a.blue() - b.blue()) <= 2;
    };

    const PdfAnnotationData *highlight = findKind(PdfAnnotationKind::Highlight);
    ASSERT_TRUE(highlight);
    EXPECT_EQ(highlight->page, 0);
    ASSERT_EQ(highlight->quads.size(), 1);
    EXPECT_NEAR(highlight->quads.first().x(), 0.10, 0.01);
    EXPECT_NEAR(highlight->quads.first().width(), 0.30, 0.01);
    EXPECT_TRUE(colorNear(highlight->color, QColor(255, 0, 0)));

    EXPECT_TRUE(findKind(PdfAnnotationKind::Underline));
    EXPECT_TRUE(findKind(PdfAnnotationKind::StrikeOut));

    const PdfAnnotationData *ink = findKind(PdfAnnotationKind::Ink);
    ASSERT_TRUE(ink);
    ASSERT_EQ(ink->strokes.size(), 1);
    EXPECT_GE(ink->strokes.first().size(), 3);
    EXPECT_NEAR(ink->lineWidthPoints, 3.0, 0.15);

    const PdfAnnotationData *rect = findKind(PdfAnnotationKind::Rectangle);
    ASSERT_TRUE(rect);
    EXPECT_NEAR(rect->boundary.x(), 0.4, 0.01);
    EXPECT_NEAR(rect->boundary.width(), 0.2, 0.01);
    EXPECT_NEAR(rect->lineWidthPoints, 2.5, 0.15);

    const PdfAnnotationData *note = findKind(PdfAnnotationKind::Note);
    ASSERT_TRUE(note);
    EXPECT_EQ(note->contents, QStringLiteral("A test comment."));
    EXPECT_TRUE(colorNear(note->color, QColor(255, 235, 59)));

    // Independent of poppler: qpdf's own parser (never used to write the
    // file) finds no structural error in it either.
    if (!QStandardPaths::findExecutable(QStringLiteral("qpdf")).isEmpty()) {
        QProcess qpdf;
        qpdf.start(QStringLiteral("qpdf"), {QStringLiteral("--check"), pdf});
        ASSERT_TRUE(qpdf.waitForFinished(5000));
        EXPECT_EQ(qpdf.exitCode(), 0) << qpdf.readAllStandardOutput().toStdString();
    }
}

TEST(PdfDocumentTest, UpdateAndRemoveMutateTheSameAnnotation)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString pdf = makeTestPdf(dir.path(), 1);

    PdfDocument doc;
    ASSERT_TRUE(doc.load(pdf));
    PdfAnnotationData note;
    note.kind = PdfAnnotationKind::Note;
    note.page = 0;
    note.boundary = QRectF(0.1, 0.1, 0.02, 0.02);
    note.contents = QStringLiteral("first");
    const QString id = doc.addAnnotation(note);
    ASSERT_FALSE(id.isEmpty());

    PdfAnnotationData updated = note;
    updated.id = id;
    updated.contents = QStringLiteral("second");
    updated.boundary.translate(0.1, 0.1);
    ASSERT_TRUE(doc.updateAnnotation(updated));

    const QList<PdfAnnotationData> after = doc.annotations(0);
    ASSERT_EQ(after.size(), 1);
    EXPECT_EQ(after.first().contents, QStringLiteral("second"));
    EXPECT_NEAR(after.first().boundary.x(), 0.2, 0.001);

    ASSERT_TRUE(doc.removeAnnotation(0, id));
    EXPECT_TRUE(doc.annotations(0).isEmpty());
    EXPECT_FALSE(doc.removeAnnotation(0, id)); // already gone
}

TEST(PdfDocumentTest, SaveIsAtomicAndThePreviousVersionIsRecoverable)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString pdf = makeTestPdf(dir.path(), 1);
    const QByteArray originalBytes = readAll(pdf);
    ASSERT_FALSE(originalBytes.isEmpty());

    PdfDocument doc;
    ASSERT_TRUE(doc.load(pdf));
    PdfAnnotationData note;
    note.kind = PdfAnnotationKind::Note;
    note.page = 0;
    note.boundary = QRectF(0.1, 0.1, 0.02, 0.02);
    note.contents = QStringLiteral("x");
    doc.addAnnotation(note);

    QString error;
    ASSERT_TRUE(doc.save(&error)) << error.toStdString();

    const QByteArray savedBytes = readAll(pdf);
    EXPECT_NE(savedBytes, originalBytes);

    const QList<PdfRecovery::Version> versions = PdfRecovery::versions(pdf);
    ASSERT_FALSE(versions.isEmpty());
    EXPECT_EQ(PdfRecovery::read(versions.first()), originalBytes);
}

TEST(PdfDocumentTest, AFailedSaveLeavesTheOriginalFileUntouched)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString pdf = makeTestPdf(dir.path(), 1);
    const QByteArray originalBytes = readAll(pdf);

    PdfDocument doc;
    ASSERT_TRUE(doc.load(pdf));
    PdfAnnotationData note;
    note.kind = PdfAnnotationKind::Note;
    note.page = 0;
    note.boundary = QRectF(0.1, 0.1, 0.02, 0.02);
    doc.addAnnotation(note);

    // Force QSaveFile::open() to fail: read-only, so the atomic-replace
    // temp file cannot be renamed over it either.
    ASSERT_TRUE(QFile::setPermissions(pdf, QFileDevice::ReadOwner));
    QString error;
    const bool saved = doc.save(&error);
    QFile::setPermissions(pdf, QFileDevice::ReadOwner | QFileDevice::WriteOwner); // restore, or cleanup fails too

    if (::geteuid() == 0) {
        GTEST_SKIP() << "running as root - permissions do not block a write";
    }
    EXPECT_FALSE(saved);
    EXPECT_FALSE(error.isEmpty());
    EXPECT_EQ(readAll(pdf), originalBytes) << "a failed save must never touch the file it could not replace";
}

TEST(PdfDocumentTest, FindAcrossDocumentLocatesAWordOnItsOwnPage)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString pdf = makeSearchablePdf(dir.path());

    const QList<QPair<int, QRectF>> matches = PdfDocument::findAcrossDocument(pdf, QStringLiteral("XYLOPHONE"), 2);
    ASSERT_EQ(matches.size(), 1);
    EXPECT_EQ(matches.first().first, 1); // the second page, 0-based

    EXPECT_TRUE(PdfDocument::findAcrossDocument(pdf, QStringLiteral("nonexistentword"), 2).isEmpty());
}

// --- PdfExtract -------------------------------------------------------

TEST(PdfExtractTest, DefaultNotePathSitsBesideThePdf)
{
    EXPECT_EQ(defaultAnnotationsNotePath(QStringLiteral("/vault/Papers/Report.pdf")),
             QStringLiteral("/vault/Papers/Report annotations.md"));
}

TEST(PdfExtractTest, ProducesExpectedMarkdownAndKeepsUserTextOnRerun)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString pdf = makeTestPdf(dir.path(), 1);

    PdfDocument doc;
    ASSERT_TRUE(doc.load(pdf));
    const QList<PdfWordBox> words = doc.words(0);
    ASSERT_FALSE(words.isEmpty());
    const QSizeF sizePts = doc.pageSizePoints(0);
    const QRectF wordPts = words.first().rect;
    const QRectF normalized(wordPts.x() / sizePts.width(), wordPts.y() / sizePts.height(),
                            wordPts.width() / sizePts.width(), wordPts.height() / sizePts.height());

    PdfAnnotationData highlight;
    highlight.kind = PdfAnnotationKind::Highlight;
    highlight.page = 0;
    highlight.quads = {normalized};
    highlight.boundary = normalized;
    doc.addAnnotation(highlight);

    PdfAnnotationData note;
    note.kind = PdfAnnotationKind::Note;
    note.page = 0;
    note.boundary = QRectF(0.05, 0.05, 0.02, 0.02);
    note.contents = QStringLiteral("A generated comment.");
    doc.addAnnotation(note);

    const QString markdown = buildAnnotationsMarkdown(doc, QStringLiteral("doc.pdf"));
    EXPECT_TRUE(markdown.contains(QStringLiteral("[[doc.pdf#page=1]]")));
    EXPECT_TRUE(markdown.contains(words.first().text)) << markdown.toStdString();
    EXPECT_TRUE(markdown.contains(QStringLiteral("A generated comment.")));

    const QString notePath = dir.path() + QStringLiteral("/doc annotations.md");
    QFile seed(notePath);
    ASSERT_TRUE(seed.open(QIODevice::WriteOnly));
    seed.write("My own heading\n\nSome notes I wrote by hand.\n");
    seed.close();

    QString error;
    ASSERT_TRUE(extractAnnotationsToNote(doc, QStringLiteral("doc.pdf"), notePath, &error)) << error.toStdString();
    QString firstRun = QString::fromUtf8(readAll(notePath));
    EXPECT_TRUE(firstRun.contains(QStringLiteral("My own heading")));
    EXPECT_TRUE(firstRun.contains(QStringLiteral("Some notes I wrote by hand.")));
    EXPECT_TRUE(firstRun.contains(QStringLiteral("[[doc.pdf#page=1]]")));

    // A user edit next to the generated section, kept on the next re-run.
    QFile edited(notePath);
    ASSERT_TRUE(edited.open(QIODevice::Append));
    edited.write("\nA line I added by hand after the generated part.\n");
    edited.close();

    ASSERT_TRUE(extractAnnotationsToNote(doc, QStringLiteral("doc.pdf"), notePath, &error)) << error.toStdString();
    const QString secondRun = QString::fromUtf8(readAll(notePath));
    EXPECT_EQ(secondRun.count(QStringLiteral("[[doc.pdf#page=1]]")), 1) << "re-running must not duplicate the section";
    EXPECT_TRUE(secondRun.contains(QStringLiteral("My own heading")));
    EXPECT_TRUE(secondRun.contains(QStringLiteral("A line I added by hand after the generated part.")));
}

TEST(PdfExtractTest, IgnoresAnnotationKindsWithNoTextOfTheirOwn)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString pdf = makeTestPdf(dir.path(), 1);
    PdfDocument doc;
    ASSERT_TRUE(doc.load(pdf));

    PdfAnnotationData rect;
    rect.kind = PdfAnnotationKind::Rectangle;
    rect.page = 0;
    rect.boundary = QRectF(0.1, 0.1, 0.1, 0.1);
    doc.addAnnotation(rect);

    PdfAnnotationData ink;
    ink.kind = PdfAnnotationKind::Ink;
    ink.page = 0;
    ink.strokes = {QPolygonF({QPointF(0.1, 0.1), QPointF(0.2, 0.2)})};
    ink.boundary = QRectF(0.1, 0.1, 0.1, 0.1);
    doc.addAnnotation(ink);

    EXPECT_TRUE(buildAnnotationsMarkdown(doc, QStringLiteral("doc.pdf")).isEmpty());
}

// --- PdfPagesArea: layout, selection, undo, find --------------------------

TEST(PdfPagesAreaTest, PagesLayOutWithRightSizesBeforeAnyRenderLands)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString pdf = makeTestPdf(dir.path(), 3);

    PdfPagesArea area;
    ASSERT_TRUE(area.load(pdf));
    area.resize(800, 600);
    // Not shown, not painted, no event loop run yet - if geometry depended
    // on a render having landed, every rect below would still be null.
    ASSERT_EQ(area.pageCount(), 3);
    for (int page = 0; page < 3; ++page) {
        const QRectF rect = area.pageLayoutRect(page);
        EXPECT_GT(rect.width(), 0) << "page " << page;
        EXPECT_GT(rect.height(), 0) << "page " << page;
        // A4 portrait: taller than wide.
        EXPECT_GT(rect.height(), rect.width()) << "page " << page;
    }
    // Pages stack top to bottom with no overlap.
    EXPECT_LT(area.pageLayoutRect(0).bottom(), area.pageLayoutRect(1).top());
    EXPECT_LT(area.pageLayoutRect(1).bottom(), area.pageLayoutRect(2).top());
}

// A file renamed keeps its modification time, so a page already drawn is still the page: the next paint after setPath()
// shows it at once instead of a blank page waiting for a render.
TEST(PdfPagesAreaTest, APageAlreadyDrawnIsStillDrawnAfterTheFileIsRenamed)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString pdf = makeTestPdf(dir.path(), 1);

    PdfPagesArea area;
    ASSERT_TRUE(area.load(pdf));
    area.resize(800, 600);
    area.show();
    const QImage blank = area.viewport()->grab().toImage();
    QImage drawn;
    ASSERT_TRUE(QTest::qWaitFor([&] {
        drawn = area.viewport()->grab().toImage();
        return drawn != blank;
    }, 10000)) << "the render landed";
    QTest::qWait(200); // settled: nothing more to come for this page
    drawn = area.viewport()->grab().toImage();

    const QString renamed = dir.path() + QStringLiteral("/renamed.pdf");
    ASSERT_TRUE(QFile::rename(pdf, renamed));
    area.setPath(renamed);
    EXPECT_EQ(area.viewport()->grab().toImage(), drawn) << "no event loop ran: the picture came from the cache, under the new name";
}

TEST(PdfPagesAreaTest, ARenderDeliveredAfterTheViewIsDestroyedDoesNothing)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString pdf = makeTestPdf(dir.path(), 1);

    // Issued, then torn down immediately: the worker this starts is very
    // likely still in flight (a real render, 20-50ms/page per PdfPage.h)
    // when the destructor runs. Repeated a few times so the race is not
    // left to one lucky/unlucky sample.
    for (int i = 0; i < 5; ++i) {
        auto area = std::make_unique<PdfPagesArea>();
        ASSERT_TRUE(area->load(pdf));
        area->resize(1600, 1200); // a bigger raster takes longer to render
        area->show();
    }
    // Whether or not any of the above actually raced, every one of those
    // jobs has now been asked to finish; if Lifeline::end() (the first line
    // of ~PdfPagesArea) had not stopped a late post from reaching a freed
    // widget, this is where it would crash or ASan would flag a
    // use-after-free.
    QThreadPool::globalInstance()->waitForDone();
    pump();
    SUCCEED();
}

TEST(PdfPagesAreaTest, DragSelectsTextAndCopiesIt)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString pdf = makeTestPdf(dir.path(), 1);

    PdfPagesArea area;
    ASSERT_TRUE(area.load(pdf));
    area.resize(900, 700);
    area.show();
    pump();

    const QList<PdfWordBox> words = area.document()->words(0);
    ASSERT_GE(words.size(), 2);
    const QRectF from = screenRectFor(area, 0, words.first().rect);
    const QRectF to = screenRectFor(area, 0, words.at(words.size() - 1).rect);
    ASSERT_FALSE(from.isEmpty());

    area.setTool(PdfPagesArea::Tool::Select);
    // Corner to corner (not centre to centre): a drag rect built from the
    // two centre points would clip the left half of the first word's own
    // box out of the selection.
    drag(area.viewport(), from.topLeft().toPoint(), to.bottomRight().toPoint());

    EXPECT_FALSE(area.selectedText().isEmpty());
    EXPECT_TRUE(area.selectedText().contains(QStringLiteral("Hello")));
}

TEST(PdfPagesAreaTest, HighlightToolAddsAnAnnotationFromTheDrag)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString pdf = makeTestPdf(dir.path(), 1);

    PdfPagesArea area;
    ASSERT_TRUE(area.load(pdf));
    area.resize(900, 700);
    area.show();
    pump();

    const QList<PdfWordBox> words = area.document()->words(0);
    ASSERT_FALSE(words.isEmpty());
    const QRectF box = screenRectFor(area, 0, words.first().rect);
    ASSERT_FALSE(box.isEmpty());

    area.setAnnotationColor(QColor(0, 200, 0));
    area.setTool(PdfPagesArea::Tool::Highlight);
    drag(area.viewport(), box.topLeft().toPoint(), box.bottomRight().toPoint());

    const QList<PdfAnnotationData> annotations = area.document()->annotations(0);
    ASSERT_EQ(annotations.size(), 1);
    EXPECT_EQ(annotations.first().kind, PdfAnnotationKind::Highlight);
    EXPECT_FALSE(annotations.first().quads.isEmpty());
    EXPECT_TRUE(area.isModified());
}

TEST(PdfPagesAreaTest, UndoRedoOfAnAddedAnnotation)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString pdf = makeTestPdf(dir.path(), 1);

    PdfPagesArea area;
    ASSERT_TRUE(area.load(pdf));
    EXPECT_FALSE(area.isModified());

    PdfAnnotationData rect;
    rect.kind = PdfAnnotationKind::Rectangle;
    rect.page = 0;
    rect.boundary = QRectF(0.3, 0.3, 0.2, 0.1);
    area.undoStack()->push(new AddPdfAnnotationCommand(area.document(), rect));
    EXPECT_TRUE(area.isModified());
    EXPECT_EQ(area.document()->annotations(0).size(), 1);
    const QString id = area.document()->annotations(0).first().id;

    area.undoStack()->undo();
    EXPECT_FALSE(area.isModified());
    EXPECT_TRUE(area.document()->annotations(0).isEmpty());

    area.undoStack()->redo();
    EXPECT_TRUE(area.isModified());
    ASSERT_EQ(area.document()->annotations(0).size(), 1);
    EXPECT_EQ(area.document()->annotations(0).first().id, id);
}

TEST(PdfPagesAreaTest, FindLocatesAWordAndStepsThroughMatches)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString pdf = makeSearchablePdf(dir.path());

    PdfPagesArea area;
    ASSERT_TRUE(area.load(pdf));
    area.resize(800, 600);
    area.show();

    QSignalSpy spy(&area, &PdfPagesArea::findStatusChanged);
    area.findStart(QStringLiteral("XYLOPHONE"));
    ASSERT_TRUE(spy.wait(3000));
    EXPECT_EQ(area.findMatchCount(), 1);
    EXPECT_EQ(area.findCurrentIndex(), 0);
    // Only one match, so stepping wraps back to itself rather than losing
    // the selection.
    area.findStep(1);
    EXPECT_EQ(area.findCurrentIndex(), 0);

    area.findClear();
    EXPECT_EQ(area.findMatchCount(), 0);
}

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
