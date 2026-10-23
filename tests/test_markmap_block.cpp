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
    // What publish and PDF export draw is the source's own, unfolded.
    EXPECT_TRUE(noteAppC::diagramCachedSVG(noteAppC::DiagramKind::Markmap, kMap, Theme::builtin(Theme::Mode::Dark)).contains("beta one"));
}

TEST_F(MarkmapBlockTest, TwoViewsOfTheSameMapAgreeWhicheverOneClicksFirst)
{
    // A note open in two splits: both show Alpha open. A folds it; B, still showing the old picture, clicks the
    // same circle - it asked to fold what it saw open, so it ends folded too, rather than flipping back.
    const auto renderer = rendererFor();
    const auto viewA = ask(renderer, kMap);
    const auto viewB = ask(renderer, kMap);
    const auto foldedByA = click(*viewA, 1);
    const auto foldedByB = click(*viewB, 1);
    EXPECT_LT(foldedByA->result.image.height(), viewA->result.image.height());
    EXPECT_EQ(foldedByB->result.image, foldedByA->result.image);
    // And back: B's picture is now the folded one, whose click opens it.
    EXPECT_EQ(click(*foldedByB, 1)->result.image, viewA->result.image);
}

TEST_F(MarkmapBlockTest, ARememberedFoldIsNotLostWhenManyOtherMapsHaveBeenFolded)
{
    const auto renderer = rendererFor();
    const auto onlyAlpha = click(*ask(renderer, kMap), 1);
    for (int i = 0; i < 255; ++i) // 256 sources in all: the limit
        click(*ask(renderer, QStringLiteral("# R\n- x%1").arg(i)), 0);
    // Clicking Beta of the map folded at Alpha must find Alpha still folded: two rows (Alpha, Beta), where
    // Beta folded alone leaves Alpha's two leaves and Beta, three.
    const auto both = click(*ask(renderer, kMap), 4);
    const auto onlyBeta = click(*ask(rendererFor(), kMap), 4); // what it would show had Alpha's fold been lost
    EXPECT_GT(onlyBeta->result.image.height(), onlyAlpha->result.image.height());
    EXPECT_EQ(both->result.image.height(), onlyAlpha->result.image.height());
}

TEST_F(MarkmapBlockTest, PublishAndPDFExportAskForTheMapAndGetItDrawnFromTheTextInTheirOwnTheme)
{
    // Nothing was drawn by an editor and nothing is kept: the map comes from the fence's text, in the theme asked.
    const QByteArray dark = noteAppC::diagramCachedSVG(noteAppC::DiagramKind::Markmap, kMap, Theme::builtin(Theme::Mode::Dark));
    const QByteArray light = noteAppC::diagramCachedSVG(noteAppC::DiagramKind::Markmap, kMap, Theme::builtin(Theme::Mode::Light));
    EXPECT_TRUE(wellFormed(dark));
    for (const char *text : {"Root", "Alpha", "alpha one", "Beta"})
        EXPECT_TRUE(dark.contains(text)) << text;
    EXPECT_TRUE(dark.contains(Theme::builtin(Theme::Mode::Dark).color(QStringLiteral("--background-primary")).name().toUtf8()));
    EXPECT_TRUE(light.contains(Theme::builtin(Theme::Mode::Light).color(QStringLiteral("--background-primary")).name().toUtf8()));
    EXPECT_NE(dark, light);
    // And the editor's own drawing leaves nothing on disk for them to find.
    ask(rendererFor(), kMap);
    EXPECT_FALSE(QDir(noteAppC::diagramCacheDirectory(noteAppC::DiagramKind::Markmap)).exists());
}

TEST_F(MarkmapBlockTest, PdfExportDrawsTheMapInTheLightThemeOnPaperAndAListingWhenThePluginIsOff)
{
    const auto exportPDF = [&](bool mindMaps) {
        const QString path = dir.filePath(QStringLiteral("out.pdf"));
        MarkdownEditor::PdfOptions options;
        options.mindMaps = mindMaps;
        QString error;
        EXPECT_TRUE(MarkdownEditor::exportPdf(path, kNote, QStringLiteral("T"), {}, options, &error)) << qPrintable(error);
        return path;
    };
    const auto imagesIn = [](const QString &path) {
        QFile file(path);
        EXPECT_TRUE(file.open(QIODevice::ReadOnly));
        return file.readAll().count("/Subtype /Image");
    };
    // An app in its dark theme has drawn the map in dark colours: the paper does not get that one.
    ask(rendererFor(Theme::Mode::Dark), kMap);
    const QString path = exportPDF(true);
    EXPECT_GE(imagesIn(path), 1) << "never opened in an editor, still a map";
    // The picture itself: the PDF holds it as a JPEG, whose corner is the map's own background (the card).
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::ReadOnly));
    const QByteArray pdf = file.readAll();
    const qsizetype filter = pdf.indexOf("/Filter /DCTDecode");
    ASSERT_GE(filter, 0);
    const qsizetype begin = pdf.indexOf("stream\n", filter) + 7;
    const QImage picture = QImage::fromData(pdf.mid(begin, pdf.indexOf("endstream", begin) - begin), "JPEG");
    ASSERT_FALSE(picture.isNull());
    const auto near = [](const QColor &a, const QColor &b) {
        return qAbs(a.red() - b.red()) < 14 && qAbs(a.green() - b.green()) < 14 && qAbs(a.blue() - b.blue()) < 14;
    };
    const QColor light = Theme::builtin(Theme::Mode::Light).color(QStringLiteral("--background-primary"));
    const QColor dark = Theme::builtin(Theme::Mode::Dark).color(QStringLiteral("--background-primary"));
    ASSERT_FALSE(near(light, dark));
    int lightPixels = 0, darkPixels = 0;
    for (int y = 0; y < picture.height(); ++y)
        for (int x = 0; x < picture.width(); ++x) {
            lightPixels += near(picture.pixelColor(x, y), light);
            darkPixels += near(picture.pixelColor(x, y), dark);
        }
    EXPECT_GT(lightPixels, picture.width() * picture.height() / 2) << "the paper's light card";
    EXPECT_LT(darkPixels, picture.width() * picture.height() / 50) << "none of the window's dark one";
    EXPECT_EQ(imagesIn(exportPDF(false)), 0) << "the plugin is off: the listing";
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

// --- the window's core plugin -------------------------------------------------

namespace {

class MarkmapWindowTest : public MarkmapBlockTest {
protected:
    void SetUp() override
    {
        MarkmapBlockTest::SetUp();
        QFile note(dir.filePath(QStringLiteral("Note.md")));
        ASSERT_TRUE(note.open(QIODevice::WriteOnly));
        note.write(kNote.toUtf8());
        note.close();
    }
    void TearDown() override
    {
        Settings config = Settings::load();
        config.reset(QStringLiteral("plugins.markmap"));
        config.save();
    }
    void configure(bool on)
    {
        Settings config = Settings::load();
        config.setValue(QStringLiteral("plugins.markmap"), on);
        ASSERT_TRUE(config.save());
    }
    /// What the editor shows for the note's map after it has had time to draw one.
    QSize openAndWait(MainWindow &window)
    {
        window.show();
        window.openVault(dir.path());
        window.openNote(dir.filePath(QStringLiteral("Note.md")));
        MarkdownEditor *editor = window.editor();
        editor->setCursorOffset(editor->noteText().size());
        const qsizetype fence = editor->noteText().indexOf(QLatin1String("Alpha"));
        QTest::qWaitFor([&] { return !editor->embedImageSizeAt(fence).isEmpty(); }, 3000);
        return editor->embedImageSizeAt(fence);
    }
};

} // namespace

TEST_F(MarkmapWindowTest, TheCorePluginIsOnByDefaultAndListedWithTheOthers)
{
    EXPECT_TRUE(Settings::defaultValue(QStringLiteral("plugins.markmap")).toBool());
    bool listed = false;
    for (const auto &feature : Settings::features())
        listed |= QByteArray(feature.id) == "markmap";
    EXPECT_TRUE(listed);
}

TEST_F(MarkmapWindowTest, OnAMapIsDrawnInTheWindow)
{
    MainWindow window;
    EXPECT_FALSE(openAndWait(window).isEmpty());
}

TEST_F(MarkmapWindowTest, OffTheFenceIsAPlainCodeBlock)
{
    configure(false);
    MainWindow window;
    EXPECT_TRUE(openAndWait(window).isEmpty());
}

TEST_F(MarkmapWindowTest, ShowAsMindMapIsACommandOfThePluginAndDrawsTheNotesHeadings)
{
    {
        MainWindow window;
        window.show();
        window.openVault(dir.path());
        QFile note(dir.filePath(QStringLiteral("Headings.md")));
        ASSERT_TRUE(note.open(QIODevice::WriteOnly));
        note.write("# Top\ntext\n## First\n### Deep\n## Second\n");
        note.close();
        window.openNote(dir.filePath(QStringLiteral("Headings.md")));
        auto *action = window.findChild<QAction *>(QStringLiteral("markmap:show-note"));
        ASSERT_TRUE(action);
        EXPECT_TRUE(action->isEnabled());
        EXPECT_TRUE(action->text().contains(QStringLiteral("mind map"), Qt::CaseInsensitive));

        // The modal dialog: found while it is up, asked what it draws, closed.
        bool drawn = false;
        QTimer::singleShot(0, [&] {
            QTimer *poll = new QTimer;
            QObject::connect(poll, &QTimer::timeout, [&drawn, poll] {
                for (QWidget *top : QApplication::topLevelWidgets()) {
                    if (top->objectName() != QLatin1String("mindMapDialog"))
                        continue;
                    auto *editor = top->findChild<MarkdownEditor *>();
                    if (editor && !editor->embedImageSizeAt(editor->noteText().indexOf(QLatin1String("First"))).isEmpty()) {
                        drawn = true;
                        static_cast<QDialog *>(top)->accept();
                        poll->deleteLater();
                    }
                }
            });
            poll->start(50);
        });
        QTimer::singleShot(10000, [&] {
            for (QWidget *top : QApplication::topLevelWidgets())
                if (top->objectName() == QLatin1String("mindMapDialog"))
                    static_cast<QDialog *>(top)->reject();
        });
        action->trigger();
        EXPECT_TRUE(drawn);
    }
    configure(false);
    MainWindow off;
    off.show();
    auto *action = off.findChild<QAction *>(QStringLiteral("markmap:show-note"));
    ASSERT_TRUE(action);
    EXPECT_FALSE(action->isEnabled()) << "the plugin's command goes dark with it";
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
