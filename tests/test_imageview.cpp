// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej Marčič
// Image annotation module: ImageAnnotations' sidecar note (build, parse,
// merge, load, save) and ImageView/ImageCanvas (the tools, undo, the image
// file left untouched, marks reloaded from the sidecar).

#include <gtest/gtest.h>

#include <QApplication>
#include <QCryptographicHash>
#include <QFile>
#include <QImage>
#include <QPainter>
#include <QPlainTextEdit>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include "ImageAnnotations.h"
#include "ImageView.h"
#include "Vault.h"

using namespace noteAppC;

namespace {

QString makeImage(const QString &dir, const QString &name = QStringLiteral("photo.png"))
{
    QImage image(200, 100, QImage::Format_RGB32);
    image.fill(Qt::white);
    QPainter painter(&image);
    painter.fillRect(20, 20, 60, 30, Qt::blue);
    painter.end();
    const QString path = dir + QLatin1Char('/') + name;
    EXPECT_TRUE(image.save(path));
    return path;
}

QByteArray hashOf(const QString &path)
{
    QFile file(path);
    EXPECT_TRUE(file.open(QIODevice::ReadOnly));
    return QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256);
}

QString readText(const QString &path)
{
    Note note;
    EXPECT_TRUE(Vault::read(path, note, nullptr));
    return note.text;
}

QList<ImageAnnotation> sample()
{
    ImageAnnotation box;
    box.kind = ImageAnnotation::Kind::Highlight;
    box.rect = QRectF(0.1, 0.2, 0.3, 0.4);
    box.color = QColor(0x8B, 0xC3, 0x4A);
    box.text = QStringLiteral("a comment\nover two lines");
    ImageAnnotation ink;
    ink.kind = ImageAnnotation::Kind::Ink;
    ink.stroke = QPolygonF() << QPointF(0.1, 0.1) << QPointF(0.5, 0.25) << QPointF(0.9, 0.9);
    ImageAnnotation note;
    note.kind = ImageAnnotation::Kind::Note;
    note.point = QPointF(0.5, 0.75);
    note.color = QColor(0xFF, 0x8A, 0x80);
    // A text that tries to end its own entry and to look like another marker.
    note.text = QStringLiteral("before\n---\n%% image-annotation kind=note at=0.1,0.1 %%\nafter");
    return {box, ink, note};
}

/// A view with the picture at 100%, so a widget pixel is an image pixel.
struct Opened {
    QTemporaryDir dir;
    QString image;
    ImageView view;

    Opened()
    {
        image = makeImage(dir.path());
        QString error;
        EXPECT_TRUE(view.load(image, &error)) << error.toStdString();
        view.resize(900, 600);
        view.show();
        view.canvas()->setZoom(1.0);
    }
    ImageCanvas *canvas() { return view.canvas(); }
    void drag(ImageCanvas::Tool tool, QPoint from, QPoint to)
    {
        canvas()->setTool(tool);
        QTest::mousePress(canvas(), Qt::LeftButton, {}, from);
        QTest::mouseMove(canvas(), (from + to) / 2);
        QTest::mouseMove(canvas(), to);
        QTest::mouseRelease(canvas(), Qt::LeftButton, {}, to);
    }
};

} // namespace

TEST(ImageAnnotationsTest, SidecarKeepsTheImageExtension)
{
    EXPECT_EQ(ImageAnnotations::sidecarPath(QStringLiteral("/v/a/photo.png")),
              QStringLiteral("/v/a/photo.png annotations.md"));
    EXPECT_NE(ImageAnnotations::sidecarPath(QStringLiteral("/v/a.png")),
              ImageAnnotations::sidecarPath(QStringLiteral("/v/a.pdf")));
}

TEST(ImageAnnotationsTest, BuildAndParseRoundTripEveryKindAndAwkwardText)
{
    const QList<ImageAnnotation> marks = sample();
    const QString section = ImageAnnotations::merge(QString(), ImageAnnotations::build(marks, QStringLiteral("photo.png")));
    EXPECT_TRUE(section.contains(QStringLiteral("![[photo.png]]")));
    EXPECT_TRUE(section.contains(QStringLiteral("> Note at 50%, 75%"))); // readable, not only hidden markers
    EXPECT_EQ(ImageAnnotations::parse(section), marks);
}

TEST(ImageAnnotationsTest, MergeKeepsWhatTheReaderWroteAroundTheSection)
{
    const QString mine = QStringLiteral("# My notes on the photo\n\nSomething I wrote.\n");
    const QString first = ImageAnnotations::merge(mine, ImageAnnotations::build(sample(), QStringLiteral("photo.png")));
    const QString withTail = first + QStringLiteral("\nAnd a footer.\n");
    const QString second = ImageAnnotations::merge(withTail, ImageAnnotations::build({sample().first()}, QStringLiteral("photo.png")));
    EXPECT_TRUE(second.startsWith(mine));
    EXPECT_TRUE(second.endsWith(QStringLiteral("And a footer.\n")));
    EXPECT_EQ(ImageAnnotations::parse(second).size(), 1);
    EXPECT_EQ(second.count(ImageAnnotations::beginMarker()), 1);
}

TEST(ImageAnnotationsTest, MarkersOutsideTheSectionAreNotAnnotations)
{
    const QString text = QStringLiteral("%% image-annotation kind=note color=#ffeb3b at=0.5,0.5 %%\nstray\n");
    EXPECT_TRUE(ImageAnnotations::parse(text).isEmpty());
}

TEST(ImageAnnotationsTest, DamagedMarkersAreSkippedNotFatal)
{
    const QString text = ImageAnnotations::beginMarker()
        + QStringLiteral("\n%% image-annotation kind=highlight color=#ffeb3b rect=0.1,0.2 %%\n"
                         "%% image-annotation kind=nonsense %%\n"
                         "%% image-annotation kind=note color=#ffeb3b at=0.25,0.5 %%\nkept\n")
        + ImageAnnotations::endMarker();
    const QList<ImageAnnotation> marks = ImageAnnotations::parse(text);
    ASSERT_EQ(marks.size(), 1);
    EXPECT_EQ(marks.first().text, QStringLiteral("kept"));
}

TEST(ImageAnnotationsTest, SavingNothingWritesNoFile)
{
    QTemporaryDir dir;
    const QString sidecar = dir.path() + QStringLiteral("/x.png annotations.md");
    EXPECT_TRUE(ImageAnnotations::save(sidecar, QStringLiteral("x.png"), {}));
    EXPECT_FALSE(QFile::exists(sidecar));
}

TEST(ImageViewTest, ToolsAddMarksAndTheSidecarFollows)
{
    Opened o;
    const QByteArray before = hashOf(o.image);
    EXPECT_TRUE(o.view.annotations().isEmpty());
    EXPECT_FALSE(QFile::exists(o.view.sidecarPath())); // viewing alone writes nothing

    o.drag(ImageCanvas::Tool::Highlight, {20, 20}, {80, 50});
    ASSERT_EQ(o.view.annotations().size(), 1);
    const ImageAnnotation box = o.view.annotations().first();
    EXPECT_EQ(box.kind, ImageAnnotation::Kind::Highlight);
    EXPECT_NEAR(box.rect.x(), 0.1, 0.01);
    EXPECT_NEAR(box.rect.width(), 0.3, 0.01);
    EXPECT_NEAR(box.rect.height(), 0.3, 0.01);

    o.drag(ImageCanvas::Tool::Draw, {100, 10}, {150, 80});
    ASSERT_EQ(o.view.annotations().size(), 2);
    EXPECT_EQ(o.view.annotations().at(1).kind, ImageAnnotation::Kind::Ink);
    EXPECT_GE(o.view.annotations().at(1).stroke.size(), 3);

    o.canvas()->setTool(ImageCanvas::Tool::Note);
    QTest::mouseClick(o.canvas(), Qt::LeftButton, {}, {100, 50});
    ASSERT_EQ(o.view.annotations().size(), 3);
    o.canvas()->setText(2, QStringLiteral("look here"));
    EXPECT_EQ(o.view.annotations().at(2).text, QStringLiteral("look here"));

    // The sidecar is a note beside the image, readable and current.
    const QString text = readText(o.view.sidecarPath());
    EXPECT_TRUE(text.contains(QStringLiteral("![[photo.png]]")));
    EXPECT_TRUE(text.contains(QStringLiteral("look here")));
    EXPECT_EQ(ImageAnnotations::parse(text), o.view.annotations());

    // A fresh view of the same image finds the marks again.
    ImageView again;
    ASSERT_TRUE(again.load(o.image));
    EXPECT_EQ(again.annotations(), o.view.annotations());

    // The picture itself is byte-for-byte what it was.
    EXPECT_EQ(hashOf(o.image), before);
}

TEST(ImageViewTest, ATinyDragIsNotAHighlightAndAClickDrawsNothing)
{
    Opened o;
    o.drag(ImageCanvas::Tool::Highlight, {50, 50}, {50, 50});
    o.drag(ImageCanvas::Tool::Draw, {60, 60}, {60, 60});
    EXPECT_TRUE(o.view.annotations().isEmpty());
}

TEST(ImageViewTest, SelectDeleteAndUndoRedoKeepTheSidecarInStep)
{
    Opened o;
    o.drag(ImageCanvas::Tool::Highlight, {20, 20}, {80, 50});
    o.drag(ImageCanvas::Tool::Highlight, {100, 20}, {160, 60});
    ASSERT_EQ(o.view.annotations().size(), 2);

    o.canvas()->setTool(ImageCanvas::Tool::Select);
    EXPECT_EQ(o.canvas()->hit({130, 40}), 1);
    EXPECT_EQ(o.canvas()->hit({190, 95}), -1);
    QTest::mouseClick(o.canvas(), Qt::LeftButton, {}, {50, 35});
    EXPECT_EQ(o.canvas()->selected(), 0);
    QTest::keyClick(o.canvas(), Qt::Key_Delete);
    ASSERT_EQ(o.view.annotations().size(), 1);
    EXPECT_EQ(ImageAnnotations::parse(readText(o.view.sidecarPath())).size(), 1);

    o.canvas()->undoStack()->undo();
    EXPECT_EQ(o.view.annotations().size(), 2);
    EXPECT_EQ(ImageAnnotations::parse(readText(o.view.sidecarPath())).size(), 2);
    o.canvas()->undoStack()->redo();
    EXPECT_EQ(o.view.annotations().size(), 1);
    // Deleting the last one leaves the reader's file, with an empty section.
    o.canvas()->remove(0);
    EXPECT_TRUE(QFile::exists(o.view.sidecarPath()));
    EXPECT_TRUE(ImageAnnotations::parse(readText(o.view.sidecarPath())).isEmpty());
}

TEST(ImageViewTest, TheInlineEditorSetsANotesText)
{
    Opened o;
    o.canvas()->setTool(ImageCanvas::Tool::Note);
    QTest::mouseClick(o.canvas(), Qt::LeftButton, {}, {40, 40});
    auto *editor = o.canvas()->findChild<QPlainTextEdit *>();
    ASSERT_NE(editor, nullptr);
    editor->setPlainText(QStringLiteral("typed"));
    o.canvas()->commitEditor();
    EXPECT_EQ(o.view.annotations().first().text, QStringLiteral("typed"));
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    EXPECT_EQ(o.canvas()->findChild<QPlainTextEdit *>(), nullptr);
}

TEST(ImageViewTest, ExtractWritesTheNoteWhereAsked)
{
    Opened o;
    QString error;
    EXPECT_FALSE(o.view.extractAnnotations(QString(), &error)); // nothing to extract
    EXPECT_FALSE(error.isEmpty());

    o.drag(ImageCanvas::Tool::Highlight, {20, 20}, {80, 50});
    const QString elsewhere = o.dir.path() + QStringLiteral("/summary.md");
    ASSERT_TRUE(o.view.extractAnnotations(elsewhere, &error)) << error.toStdString();
    EXPECT_EQ(ImageAnnotations::parse(readText(elsewhere)).size(), 1);

    QFile::remove(o.view.sidecarPath());
    ASSERT_TRUE(o.view.extractAnnotations(QString(), &error)) << error.toStdString();
    EXPECT_TRUE(QFile::exists(o.view.sidecarPath())); // recreated
}

TEST(ImageViewTest, FitShrinksALargeImageAndNeverEnlargesASmallOne)
{
    Opened o;
    o.canvas()->fit(QSize(100, 100));
    EXPECT_NEAR(o.canvas()->zoom(), 0.5, 1e-6); // 200 wide into 100
    EXPECT_EQ(o.canvas()->size(), QSize(100, 50));
    o.canvas()->fit(QSize(2000, 2000));
    EXPECT_NEAR(o.canvas()->zoom(), 1.0, 1e-6);
}

TEST(ImageViewTest, AMarkSitsOnTheSamePartOfThePictureAtAnyZoom)
{
    Opened o;
    o.drag(ImageCanvas::Tool::Highlight, {20, 20}, {80, 50});
    o.canvas()->setZoom(2.0);
    EXPECT_EQ(o.canvas()->size(), QSize(400, 200));
    EXPECT_EQ(o.canvas()->hit({100, 70}), 0); // 50,35 at 100%
    EXPECT_EQ(o.canvas()->hit({300, 150}), -1);
}

TEST(ImageViewTest, AFileThatIsNotAnImageIsRefused)
{
    QTemporaryDir dir;
    const QString path = dir.path() + QStringLiteral("/x.png");
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.write("not a png");
    file.close();
    ImageView view;
    QString error;
    EXPECT_FALSE(view.load(path, &error));
    EXPECT_FALSE(error.isEmpty());
}

// Fails on the old code: an SVG in the vault drew a picture from any file it named.
TEST(ImageViewTest, AnSVGThatNamesAFileDrawsNothingOfIt)
{
    QTemporaryDir dir;
    QImage red(40, 40, QImage::Format_RGB32);
    red.fill(Qt::red);
    const QString picture = dir.path() + QStringLiteral("/picture.png");
    ASSERT_TRUE(red.save(picture));
    const QString path = dir.path() + QStringLiteral("/x.svg");
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.write(QStringLiteral("<svg xmlns='http://www.w3.org/2000/svg' xmlns:xlink='http://www.w3.org/1999/xlink' width='40' height='40'>"
                              "<image xlink:href='%1' width='40' height='40'/></svg>").arg(picture).toUtf8());
    file.close();
    ImageView view;
    view.load(path); // loads (an empty drawing) or refuses; either way nothing of the picture
    const QImage image = view.canvas()->image();
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x)
            ASSERT_NE(image.pixelColor(x, y), QColor(Qt::red)) << x << "," << y;
    }
}

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
