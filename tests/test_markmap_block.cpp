// ```markmap fences in the editor: MarkmapBlock.cpp's renderer (a pool thread, the raster, the hotspots on the
// circles and the folding a click makes), the editor drawing and folding it in live preview and reading view,
// PDF export reading the SVG it kept, and the window's `markmap` core plugin and "Show as mind map" command.
// The tree and the SVG themselves are test_markmap.cpp's.

#include <gtest/gtest.h>

#include <atomic>
#include <memory>

#include <QAction>
#include <QApplication>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QSvgRenderer>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QTest>
#include <QXmlStreamReader>

#include "MainWindow.h"
#include "MarkdownEditor.h"
#include "Markmap.h"
#include "MarkmapBlock.h"
#include "MermaidCache.h"
#include "Settings.h"
#include "Theme.h"
#include "WaitFor.h"

using noteAppC::MainWindow;
using noteAppC::MarkdownEditor;
using noteAppC::Settings;
using noteAppC::Theme;

namespace {

noteAppC::MarkmapLook lookOf(Theme::Mode mode = Theme::Mode::Dark)
{
    return noteAppC::markmapLook(noteAppC::diagramConfig(noteAppC::DiagramKind::Markmap, Theme::builtin(mode)));
}

bool wellFormed(const QByteArray &svg)
{
    QXmlStreamReader xml(svg);
    while (!xml.atEnd())
        xml.readNext();
    return !xml.hasError();
}

} // namespace

// --- the editor's renderer ---------------------------------------------------

namespace {

// The text between the fences, which is what a renderer is given and the cache is keyed by.
const QString kMap = QStringLiteral("# Root\n## Alpha\n- alpha one\n- alpha two\n## Beta\n- beta one");
const QString kNote = QStringLiteral("```markmap\n") + kMap + QStringLiteral("\n```\n\nafter\n");

class MarkmapBlockTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        ASSERT_TRUE(dir.isValid());
        // The disk cache is per test: an earlier test's map must not answer for this one.
        qputenv("XDG_CACHE_HOME", dir.filePath(QStringLiteral("cache")).toUtf8());
    }

    struct Answer {
        std::atomic<bool> done{false};
        MarkdownEditor::CodeBlockResult result;
        QThread *thread = nullptr;
    };
    std::shared_ptr<Answer> ask(const MarkdownEditor::CodeBlockRenderer &renderer, const QString &source, int width = 800)
    {
        auto answer = std::make_shared<Answer>();
        renderer(QStringLiteral("markmap"), source, width, QStringLiteral("dark"), [answer](MarkdownEditor::CodeBlockResult result) {
            answer->result = std::move(result);
            answer->thread = QThread::currentThread();
            answer->done = true;
        });
        EXPECT_TRUE(QTest::qWaitFor([&] { return answer->done.load(); }, 15000));
        return answer;
    }
    /// The hotspot of `node`'s circle, activated and waited for.
    std::shared_ptr<Answer> click(const Answer &from, int node)
    {
        auto answer = std::make_shared<Answer>();
        EXPECT_TRUE(bool(from.result.activate));
        from.result.activate(node, [answer](MarkdownEditor::CodeBlockResult result) {
            answer->result = std::move(result);
            answer->done = true;
        });
        EXPECT_TRUE(QTest::qWaitFor([&] { return answer->done.load(); }, 15000));
        return answer;
    }
    static MarkdownEditor::CodeBlockRenderer rendererFor(Theme::Mode mode = Theme::Mode::Dark)
    {
        return noteAppC::markmapRenderer([mode] { return Theme::builtin(mode); });
    }
    static QList<int> idsOf(const MarkdownEditor::CodeBlockResult &result)
    {
        QList<int> ids;
        for (const auto &hotspot : result.hotspots)
            ids << hotspot.id;
        return ids;
    }

    QTemporaryDir dir;
};

} // namespace

TEST_F(MarkmapBlockTest, TheMapIsDrawnOnAnotherThreadWithAHotspotOnEachCircle)
{
    const auto answer = ask(rendererFor(), kMap);
    ASSERT_FALSE(answer->result.isMarkdown) << answer->result.text.toStdString();
    EXPECT_FALSE(answer->result.image.isNull());
    EXPECT_NE(answer->thread, QThread::currentThread());
    EXPECT_EQ(idsOf(answer->result), (QList<int>{0, 1, 4}));
    const QSizeF logical = QSizeF(answer->result.image.size()) / answer->result.image.devicePixelRatio();
    for (const auto &hotspot : answer->result.hotspots)
        EXPECT_TRUE(QRectF(QPointF(), logical).contains(hotspot.rect.center()));
}

TEST_F(MarkmapBlockTest, AMapWiderThanTheEditorIsScaledAndItsHotspotsWithIt)
{
    const auto wide = ask(rendererFor(), kMap, 2000);
    const auto narrow = ask(rendererFor(), kMap, 200);
    const qreal wideWidth = wide->result.image.width() / wide->result.image.devicePixelRatio();
    const qreal narrowWidth = narrow->result.image.width() / narrow->result.image.devicePixelRatio();
    EXPECT_NEAR(narrowWidth, 200, 1.5);
    ASSERT_EQ(wide->result.hotspots.size(), narrow->result.hotspots.size());
    EXPECT_NEAR(narrow->result.hotspots.first().rect.center().x() / narrowWidth, wide->result.hotspots.first().rect.center().x() / wideWidth, 0.01);
}

TEST_F(MarkmapBlockTest, AClickOnACircleFoldsItsNodeAndAnotherUnfoldsIt)
{
    const auto renderer = rendererFor();
    const auto open = ask(renderer, kMap);
    const auto folded = click(*open, 1); // Alpha
    EXPECT_LT(folded->result.image.height(), open->result.image.height());
    EXPECT_TRUE(idsOf(folded->result).contains(1)) << "the circle stays, to open it again";
    EXPECT_FALSE(idsOf(folded->result).contains(2) || idsOf(folded->result).contains(3));
    const auto again = click(*folded, 1);
    EXPECT_EQ(again->result.image.size(), open->result.image.size());
    EXPECT_EQ(again->result.image, open->result.image);
}

TEST_F(MarkmapBlockTest, TheFoldingIsKeptByTheRendererPerSourceAndNeverWrittenBack)
{
    const auto renderer = rendererFor();
    const auto open = ask(renderer, kMap);
    click(*open, 1); // Alpha
    // Asked again (another width, a split of the same note): still folded.
    EXPECT_LT(ask(renderer, kMap)->result.image.height(), open->result.image.height());
    // A renderer nobody has clicked, and another source, are not.
    EXPECT_EQ(ask(rendererFor(), kMap)->result.image.size(), open->result.image.size());
    EXPECT_EQ(ask(renderer, kMap + QStringLiteral("\n- extra"))->result.image.size(),
              ask(rendererFor(), kMap + QStringLiteral("\n- extra"))->result.image.size());
    // The map the cache holds - what publish and PDF export read - is the source's own, unfolded.
    EXPECT_TRUE(noteAppC::diagramCachedSVG(noteAppC::DiagramKind::Markmap, kMap, Theme::builtin(Theme::Mode::Dark)).contains("beta one"));
}

TEST_F(MarkmapBlockTest, TheSVGIsKeptWhereDiagramsAreSoPublishAndPDFExportFindIt)
{
    EXPECT_TRUE(noteAppC::diagramCachedSVG(noteAppC::DiagramKind::Markmap, kMap, Theme::builtin(Theme::Mode::Dark)).isEmpty());
    ask(rendererFor(), kMap);
    const QByteArray svg = noteAppC::diagramCachedSVG(noteAppC::DiagramKind::Markmap, kMap, Theme::builtin(Theme::Mode::Dark));
    EXPECT_TRUE(wellFormed(svg));
    for (const char *text : {"Root", "Alpha", "alpha one", "Beta"})
        EXPECT_TRUE(svg.contains(text)) << text;
    // In any theme, as the other kinds: the most recent in other colours when none is for this theme.
    EXPECT_FALSE(noteAppC::diagramCachedSVG(noteAppC::DiagramKind::Markmap, kMap, Theme::builtin(Theme::Mode::Light)).isEmpty());
}

TEST_F(MarkmapBlockTest, PdfExportDrawsTheMapTheEditorMadeAndOtherwiseTheListing)
{
    const auto exportImages = [&] {
        const QString path = dir.filePath(QStringLiteral("out.pdf"));
        QString error;
        EXPECT_TRUE(MarkdownEditor::exportPdf(path, kNote, QStringLiteral("T"), {}, MarkdownEditor::PdfOptions{}, &error)) << qPrintable(error);
        QFile file(path);
        EXPECT_TRUE(file.open(QIODevice::ReadOnly));
        return file.readAll().count("/Subtype /Image");
    };
    EXPECT_EQ(exportImages(), 0) << "never drawn: the listing";
    ask(rendererFor(), kMap);
    EXPECT_GE(exportImages(), 1) << "drawn in the editor: its image";
}

TEST_F(MarkmapBlockTest, AMapIsDrawnInPlaceOfItsFenceAndTheCaretBringsTheSourceBack)
{
    MarkdownEditor editor;
    editor.resize(900, 600);
    editor.setNoteText(kNote);
    editor.setCodeBlockRenderer({QStringLiteral("markmap")}, rendererFor());
    editor.setCursorOffset(editor.noteText().size());
    editor.show();
    const qsizetype fence = editor.noteText().indexOf(QLatin1String("Alpha"));
    editor.viewport()->grab();
    WAIT_TRUE(!editor.embedImageSizeAt(fence).isEmpty(), 15000);

    editor.enterBlockAt(fence);
    editor.viewport()->grab();
    EXPECT_TRUE(editor.embedImageSizeAt(fence).isEmpty()) << "inside: the source, to edit";
    editor.setCursorOffset(editor.noteText().size());
    editor.viewport()->grab();
    EXPECT_FALSE(editor.embedImageSizeAt(fence).isEmpty());
}

TEST_F(MarkmapBlockTest, ClickingACircleInTheEditorFoldsTheNodeAndAClickElsewhereDoesNot)
{
    for (const auto mode : {MarkdownEditor::Mode::LivePreview, MarkdownEditor::Mode::Reading}) {
        SCOPED_TRACE(int(mode));
        MarkdownEditor editor;
        editor.resize(900, 600);
        editor.setNoteText(kNote);
        editor.setMode(mode);
        editor.setCodeBlockRenderer({QStringLiteral("markmap")}, rendererFor());
        editor.setCursorOffset(editor.noteText().size());
        editor.show();
        const qsizetype fence = editor.noteText().indexOf(QLatin1String("Alpha"));
        editor.viewport()->grab();
        WAIT_TRUE(!editor.embedImageSizeAt(fence).isEmpty(), 15000);
        const QSize before = editor.embedImageSizeAt(fence);
        ASSERT_FALSE(editor.codeBlockImageRectAt(fence).isEmpty());

        // Where Alpha's circle is in the picture as it is now: the map drawn again here, scaled to the image.
        const auto circleOfAlpha = [&](const QSet<int> &folded) {
            const QRect rect = editor.codeBlockImageRectAt(fence);
            const auto drawn = noteAppC::drawMarkmap(noteAppC::parseMarkmap(kMap), lookOf(), folded);
            const qreal scale = qreal(rect.width()) / drawn.size.width();
            for (const auto &toggle : drawn.toggles)
                if (toggle.node == 1)
                    return rect.topLeft() + QPoint(qRound(toggle.rect.center().x() * scale), qRound(toggle.rect.center().y() * scale));
            return QPoint();
        };

        QTest::mouseClick(editor.viewport(), Qt::LeftButton, {}, circleOfAlpha({}));
        WAIT_TRUE(editor.embedImageSizeAt(fence).height() < before.height(), 15000);
        EXPECT_EQ(editor.noteText(), kNote) << "the folding is not written to the note";

        QTest::mouseClick(editor.viewport(), Qt::LeftButton, {}, circleOfAlpha({1}));
        WAIT_TRUE(editor.embedImageSizeAt(fence).height() == before.height(), 15000);

        if (mode == MarkdownEditor::Mode::LivePreview) {
            // Anywhere else on the picture is a click into the block, as for any diagram: the source, to edit.
            QTest::mouseClick(editor.viewport(), Qt::LeftButton, {}, editor.codeBlockImageRectAt(fence).topLeft() + QPoint(3, 3));
            editor.viewport()->grab();
            EXPECT_TRUE(editor.embedImageSizeAt(fence).isEmpty());
        }
    }
}

TEST_F(MarkmapBlockTest, WithNoRendererTheFenceIsAPlainCodeBlock)
{
    MarkdownEditor editor;
    editor.resize(900, 600);
    editor.setNoteText(kNote);
    editor.setCursorOffset(editor.noteText().size());
    editor.show();
    editor.viewport()->grab();
    QTest::qWait(200);
    EXPECT_TRUE(editor.embedImageSizeAt(editor.noteText().indexOf(QLatin1String("Alpha"))).isEmpty());
}

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    // Settings and the caches are under these, never the real ones.
    QTemporaryDir configHome, dataHome, cacheHome;
    qputenv("XDG_CONFIG_HOME", configHome.path().toUtf8());
    qputenv("XDG_DATA_HOME", dataHome.path().toUtf8());
    qputenv("XDG_CACHE_HOME", cacheHome.path().toUtf8());
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("noteAppC-test"));
    QCoreApplication::setApplicationName(QStringLiteral("noteAppC-test"));
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
