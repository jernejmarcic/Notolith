// PlantUML, TikZ and Graphviz fences: MermaidBlock.cpp's diagramRenderer run with the real tools
// (plantuml and java, latex and dvisvgm, dot), each inside bubblewrap's sandbox. A test whose tool is
// absent is skipped, and fails instead when NOTEAPPC_REQUIRE_TOOLS=1. The Mermaid machinery they share
// (memo, pool, hints) is test_mermaid.cpp's.

#include <gtest/gtest.h>

#include <atomic>
#include <iostream>
#include <memory>

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThread>
#include <QTest>

#include <unistd.h>

#include "Bubblewrap.h"
#include "MainWindow.h"
#include "MarkdownEditor.h"
#include "MermaidBlock.h"
#include "MermaidCache.h"
#include "Settings.h"
#include "SyscallFilter.h"
#include "Theme.h"
#include "WaitFor.h"

using noteAppC::DiagramKind;
using noteAppC::MainWindow;
using noteAppC::MarkdownEditor;
using noteAppC::Settings;
using noteAppC::Theme;

namespace {

const QString kPlantUML = QStringLiteral("@startuml\nAlice -> Bob: hello\n@enduml");
const QString kDot = QStringLiteral("digraph G { Alpha -> Beta }");
const QString kTikZ = QStringLiteral("\\begin{tikzpicture}\\draw (0,0) -- (1,1) node {Gamma};\\end{tikzpicture}");

struct Case {
    DiagramKind kind;
    const char *language;
    const char *tool; ///< the executable the fence needs, besides bwrap
    QString source;
};
const Case kCases[] = {{DiagramKind::PlantUML, "plantuml", "plantuml", kPlantUML},
                       {DiagramKind::Graphviz, "dot", "dot", kDot},
                       {DiagramKind::TikZ, "tikz", "latex", kTikZ}};

class DiagramTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        ASSERT_TRUE(dir.isValid());
        // The disk cache is per test: an earlier test's diagram must not answer for this one.
        qputenv("XDG_CACHE_HOME", dir.filePath(QStringLiteral("cache")).toUtf8());
    }

    /// Skips (fails under NOTEAPPC_REQUIRE_TOOLS=1) when `tool` or the sandbox is not installed.
    static bool have(const char *tool)
    {
        const bool present = !QStandardPaths::findExecutable(QString::fromLatin1(tool)).isEmpty()
                             && !QStandardPaths::findExecutable(QStringLiteral("bwrap")).isEmpty();
        if (!present && qEnvironmentVariable("NOTEAPPC_REQUIRE_TOOLS") == QLatin1String("1"))
            ADD_FAILURE() << "NOTEAPPC_REQUIRE_TOOLS=1 and " << tool << " or bwrap is missing";
        return present;
    }
#define NEED(tool) \
    if (!have(tool)) \
    GTEST_SKIP() << tool << " (or bwrap) is not installed"

    struct Answer {
        std::atomic<bool> done{false};
        MarkdownEditor::CodeBlockResult result;
        qint64 ms = 0;
    };
    std::shared_ptr<Answer> ask(const MarkdownEditor::CodeBlockRenderer &renderer, const QString &language, const QString &source)
    {
        auto answer = std::make_shared<Answer>();
        QElapsedTimer timer;
        timer.start();
        renderer(language, source, 600, QStringLiteral("dark"), [answer, &timer](MarkdownEditor::CodeBlockResult result) {
            answer->ms = timer.elapsed();
            answer->result = std::move(result);
            answer->done = true;
        });
        EXPECT_TRUE(QTest::qWaitFor([&] { return answer->done.load(); }, 60000));
        return answer;
    }
    static MarkdownEditor::CodeBlockRenderer rendererFor(DiagramKind kind, const QString &tool, Theme::Mode mode = Theme::Mode::Dark)
    {
        return noteAppC::diagramRenderer(kind, tool, [mode] { return Theme::builtin(mode); });
    }
    static QByteArray cached(DiagramKind kind, const QString &source, Theme::Mode mode = Theme::Mode::Dark)
    {
        return noteAppC::diagramCachedSVG(kind, source, Theme::builtin(mode));
    }

    QTemporaryDir dir;
};

} // namespace

TEST_F(DiagramTest, EachKindDrawsADiagramAndKeepsItsSVG)
{
    for (const Case &c : kCases) {
        SCOPED_TRACE(c.language);
        NEED(c.tool);
        const auto answer = ask(rendererFor(c.kind, QString()), QString::fromLatin1(c.language), c.source);
        std::cout << "first render, " << c.language << ": " << answer->ms << " ms" << std::endl;
        ASSERT_FALSE(answer->result.isMarkdown) << answer->result.text.toStdString();
        EXPECT_FALSE(answer->result.image.isNull());
        const QByteArray svg = cached(c.kind, c.source);
        ASSERT_FALSE(svg.isEmpty());
        EXPECT_TRUE(svg.contains("<path") || svg.contains("<text") || svg.contains("<polygon")) << svg.left(300).constData();
    }
}

TEST_F(DiagramTest, ThePlantUMLAndGraphvizLabelsAreInTheSVG)
{
    NEED("plantuml");
    NEED("dot");
    ask(rendererFor(DiagramKind::PlantUML, QString()), QStringLiteral("plantuml"), kPlantUML);
    EXPECT_TRUE(cached(DiagramKind::PlantUML, kPlantUML).contains(">Alice<"));
    ask(rendererFor(DiagramKind::Graphviz, QString()), QStringLiteral("dot"), kDot);
    EXPECT_TRUE(cached(DiagramKind::Graphviz, kDot).contains(">Beta</text>"));
}

TEST_F(DiagramTest, ADiagramAlreadyMadeIsReadFromDiskNotMadeAgain)
{
    for (const Case &c : kCases) {
        SCOPED_TRACE(c.language);
        NEED(c.tool);
        ask(rendererFor(c.kind, QString()), QString::fromLatin1(c.language), c.source);
        // A renderer of its own whose tool does not exist: only the disk can answer it, so no tool was asked for.
        const auto again = ask(rendererFor(c.kind, dir.filePath(QStringLiteral("no-such-tool"))), QString::fromLatin1(c.language), c.source);
        EXPECT_FALSE(again->result.isMarkdown) << again->result.text.toStdString();
        EXPECT_LT(again->ms, 2000);
    }
}

TEST_F(DiagramTest, AMissingToolShowsTheSourceAndAHint)
{
    for (const Case &c : kCases) {
        SCOPED_TRACE(c.language);
        const auto named = ask(rendererFor(c.kind, QStringLiteral("no-such-tool-for-diagram-tests")), QString::fromLatin1(c.language), c.source);
        ASSERT_TRUE(named->result.isMarkdown);
        EXPECT_TRUE(named->result.text.contains(c.source.trimmed().left(12)));
        EXPECT_TRUE(named->result.text.contains(QLatin1String("no-such-tool-for-diagram-tests was not found")));
    }
}

TEST_F(DiagramTest, WithoutBubblewrapThereIsNoDiagramAndNeverAnUnsandboxedRun)
{
    NEED("dot");
    const QString dot = QStandardPaths::findExecutable(QStringLiteral("dot"));
    const QByteArray oldPath = qgetenv("PATH");
    qputenv("PATH", dir.path().toUtf8()); // an empty directory: no bwrap to find
    const auto answer = ask(rendererFor(DiagramKind::Graphviz, dot), QStringLiteral("dot"), kDot);
    qputenv("PATH", oldPath);
    ASSERT_TRUE(answer->result.isMarkdown);
    EXPECT_TRUE(answer->result.text.contains(QLatin1String("bubblewrap"))) << answer->result.text.toStdString();
    EXPECT_TRUE(cached(DiagramKind::Graphviz, kDot).isEmpty());
}

TEST_F(DiagramTest, ASyntaxErrorShowsTheSourceAndTheToolsOwnError)
{
    NEED("dot");
    const auto answer = ask(rendererFor(DiagramKind::Graphviz, QString()), QStringLiteral("dot"), QStringLiteral("digraph { A -> "));
    ASSERT_TRUE(answer->result.isMarkdown);
    EXPECT_TRUE(answer->result.text.contains(QLatin1String("syntax error"))) << answer->result.text.toStdString();
    NEED("latex");
    const auto tex = ask(rendererFor(DiagramKind::TikZ, QString()), QStringLiteral("tikz"), QStringLiteral("\\begin{tikzpicture}\\draw (0,0) -- ;"));
    ASSERT_TRUE(tex->result.isMarkdown);
    EXPECT_TRUE(tex->result.text.contains(QLatin1String("TikZ: "))) << tex->result.text.toStdString();
}

TEST_F(DiagramTest, PlantUMLWrapsABareSourceAndCannotIncludeAFileOrAURL)
{
    NEED("plantuml");
    const auto renderer = rendererFor(DiagramKind::PlantUML, QString());
    const auto bare = ask(renderer, QStringLiteral("plantuml"), QStringLiteral("Alice -> Bob: no wrapper"));
    EXPECT_FALSE(bare->result.isMarkdown) << bare->result.text.toStdString();
    // Not a diagram: PlantUML's own security profile refuses an include of a file or a URL.
    for (const QString &include : {QStringLiteral("!include /etc/hostname"), QStringLiteral("!include /etc/passwd"),
                                   QStringLiteral("!includeurl http://127.0.0.1:9/x.puml"),
                                   QStringLiteral("!include https://example.invalid/x.puml")}) {
        SCOPED_TRACE(include.toStdString());
        const auto answer = ask(renderer, QStringLiteral("plantuml"), QStringLiteral("@startuml\n") + include + QStringLiteral("\nAlice -> Bob\n@enduml"));
        EXPECT_TRUE(answer->result.isMarkdown) << "the include must not draw a diagram";
        EXPECT_TRUE(cached(DiagramKind::PlantUML, QStringLiteral("@startuml\n") + include + QStringLiteral("\nAlice -> Bob\n@enduml")).isEmpty());
    }
}

TEST_F(DiagramTest, PlantUMLIsDrawnDarkOnADarkTheme)
{
    NEED("plantuml");
    ask(rendererFor(DiagramKind::PlantUML, QString(), Theme::Mode::Dark), QStringLiteral("plantuml"), kPlantUML);
    ask(rendererFor(DiagramKind::PlantUML, QString(), Theme::Mode::Light), QStringLiteral("plantuml"), kPlantUML);
    EXPECT_TRUE(cached(DiagramKind::PlantUML, kPlantUML, Theme::Mode::Dark).contains("background:#1B1B1B"));
    EXPECT_FALSE(cached(DiagramKind::PlantUML, kPlantUML, Theme::Mode::Light).contains("background:#1B1B1B"));
}

TEST_F(DiagramTest, GraphvizTakesTheThemesColoursAsDefaultsAndTheGraphsOwnWin)
{
    NEED("dot");
    const QString source = QStringLiteral("digraph G { node [color=red]; A -> B }");
    ask(rendererFor(DiagramKind::Graphviz, QString()), QStringLiteral("dot"), source);
    const QByteArray svg = cached(DiagramKind::Graphviz, source);
    const QByteArray text = Theme::builtin(Theme::Mode::Dark).color(QStringLiteral("--text-normal")).name().toUtf8();
    EXPECT_TRUE(svg.contains("stroke=\"" + text + "\"")) << "the edge, in the theme's text colour";
    EXPECT_TRUE(svg.contains("stroke=\"red\"")) << "the node's own colour";
    EXPECT_TRUE(svg.contains(Theme::builtin(Theme::Mode::Dark).color(QStringLiteral("--background-primary")).name().toUtf8()))
        << "the theme's background, so another theme's page shows a card";
}

TEST_F(DiagramTest, TikZAcceptsTikZJaxsBlockAndABareTikzpicture)
{
    NEED("latex");
    const QString block = QStringLiteral("\\usepackage{tikz}\n\\usetikzlibrary{calc}\n\\begin{document}\n"
                                         "\\begin{tikzpicture}\\draw[red] (0,0) -- ($(1,1)+(1,0)$);\\end{tikzpicture}\n\\end{document}");
    const auto renderer = rendererFor(DiagramKind::TikZ, QString());
    const auto first = ask(renderer, QStringLiteral("tikz"), block);
    EXPECT_FALSE(first->result.isMarkdown) << first->result.text.toStdString();
    EXPECT_TRUE(cached(DiagramKind::TikZ, block).contains("<path"));
    const auto bare = ask(renderer, QStringLiteral("tikz"), kTikZ);
    EXPECT_FALSE(bare->result.isMarkdown) << bare->result.text.toStdString();
}

TEST_F(DiagramTest, TikZBlackBecomesTheTextColourOnADarkThemeAndOtherColoursStay)
{
    NEED("latex");
    const QString source = QStringLiteral("\\begin{tikzpicture}\\draw (0,0) -- (1,1);\\draw[red] (0,1) -- (1,0);\\end{tikzpicture}");
    const auto drawn = ask(rendererFor(DiagramKind::TikZ, QString(), Theme::Mode::Dark), QStringLiteral("tikz"), source);
    ask(rendererFor(DiagramKind::TikZ, QString(), Theme::Mode::Light), QStringLiteral("tikz"), source);
    ASSERT_FALSE(drawn->result.isMarkdown);
    EXPECT_LT(drawn->result.image.pixelColor(2, 2).lightnessF(), 0.3) << "a dark card, not transparent";
    const QByteArray dark = cached(DiagramKind::TikZ, source, Theme::Mode::Dark);
    const QByteArray text = Theme::builtin(Theme::Mode::Dark).color(QStringLiteral("--text-normal")).name().toUtf8();
    EXPECT_TRUE(dark.contains("stroke='" + text + "'")) << dark.constData();
    EXPECT_FALSE(dark.contains("stroke='#000'"));
    EXPECT_TRUE(dark.contains("stroke='#f00'") || dark.contains("stroke='#ff0000'")) << "red stays red: " << dark.constData();
    EXPECT_TRUE(cached(DiagramKind::TikZ, source, Theme::Mode::Light).contains("stroke='#000'")) << "a light theme keeps black";
    EXPECT_TRUE(dark.contains("<rect x='-")) << "a background card in the theme's colour: " << dark.constData();
    EXPECT_TRUE(dark.contains(Theme::builtin(Theme::Mode::Dark).color(QStringLiteral("--background-primary")).name().toUtf8()));
}

TEST_F(DiagramTest, TikZCannotReadAFileOutsideTheSandboxOrStartAShell)
{
    NEED("latex");
    QTemporaryDir outside;
    ASSERT_TRUE(outside.isValid());
    const QString marker = outside.filePath(QStringLiteral("shell-ran"));
    const QString written = outside.filePath(QStringLiteral("written"));
    const auto renderer = rendererFor(DiagramKind::TikZ, QString());
    // \input of a file outside /usr and /etc: not in the sandbox, so the picture is not made.
    QFile secret(outside.filePath(QStringLiteral("secret.tex")));
    ASSERT_TRUE(secret.open(QIODevice::WriteOnly));
    secret.write("SECRETTEXT");
    secret.close();
    const QString read = QStringLiteral("\\input{%1}\\begin{tikzpicture}\\draw (0,0) -- (1,1);\\end{tikzpicture}").arg(secret.fileName());
    const auto input = ask(renderer, QStringLiteral("tikz"), read);
    EXPECT_TRUE(input->result.isMarkdown) << "\\input of a file outside the sandbox drew a diagram";
    EXPECT_TRUE(cached(DiagramKind::TikZ, read).isEmpty());
    // \write18 and \openout: no shell, and nothing appears outside the temporary directory.
    const QString shell = QStringLiteral("\\immediate\\write18{touch %1}\\immediate\\openout3=%2 \\immediate\\write3{x}"
                                         "\\begin{tikzpicture}\\draw (0,0) -- (1,1);\\end{tikzpicture}")
                              .arg(marker, written);
    ask(renderer, QStringLiteral("tikz"), shell);
    EXPECT_FALSE(QFile::exists(marker));
    EXPECT_FALSE(QFile::exists(written));
}

// The sandbox itself, whatever tool runs in it.
TEST(DiagramSandboxTest, SeesTheSystemAndOneWritableDirectoryAndNothingElse)
{
    if (QStandardPaths::findExecutable(QStringLiteral("bwrap")).isEmpty()) {
        ASSERT_NE(qEnvironmentVariable("NOTEAPPC_REQUIRE_TOOLS"), QLatin1String("1")) << "bwrap is missing";
        GTEST_SKIP() << "bwrap is not installed";
    }
    QTemporaryDir work, outside;
    ASSERT_TRUE(work.isValid() && outside.isValid());
    qputenv("DIAGRAM_SANDBOX_LEAK", "leaked");
    const QString script = QStringLiteral("echo in > %1/inside; echo out > %2/outside; echo \"env=$DIAGRAM_SANDBOX_LEAK\" "
                                          "> %1/env; ls %2 > %1/listing 2>&1; cat /etc/hostname > /dev/null && echo etc > %1/etc")
                               .arg(work.path(), outside.path());
    const noteAppC::sandbox::Command command =
        noteAppC::sandbox::bubblewrapCommand(work.path(), QStringLiteral("/bin/sh"), {QStringLiteral("-c"), script});
    ASSERT_FALSE(command.program.isEmpty());
    QProcess process;
    process.setChildProcessModifier([fd = command.filterFD] { noteAppC::sandbox::keepInChild(fd); });
    process.start(command.program, command.arguments);
    if (command.filterFD >= 0)
        close(command.filterFD);
    ASSERT_TRUE(process.waitForFinished(15000));
    qunsetenv("DIAGRAM_SANDBOX_LEAK");
    const auto read = [&](const QString &name) {
        QFile f(work.filePath(name));
        return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray("<none>");
    };
    EXPECT_EQ(read(QStringLiteral("inside")), QByteArray("in\n"));
    EXPECT_FALSE(QFile::exists(outside.filePath(QStringLiteral("outside")))) << "a write outside the directory reached the real disk";
    EXPECT_EQ(read(QStringLiteral("env")), QByteArray("env=\n")) << "the environment is not inherited";
    EXPECT_TRUE(read(QStringLiteral("listing")).contains("No such file")) << "the other directory is not there";
    EXPECT_EQ(read(QStringLiteral("etc")), QByteArray("etc\n")) << "/etc is";
}

TEST_F(DiagramTest, PdfExportDrawsACachedDotDiagramAndOtherwiseTheListing)
{
    const QString note = QStringLiteral("```dot\n") + kDot + QStringLiteral("\n```\n");
    const auto exportImages = [&] {
        const QString path = dir.filePath(QStringLiteral("out.pdf"));
        QString error;
        EXPECT_TRUE(MarkdownEditor::exportPdf(path, note, QStringLiteral("T"), {}, MarkdownEditor::PdfOptions{}, &error)) << qPrintable(error);
        QFile file(path);
        EXPECT_TRUE(file.open(QIODevice::ReadOnly));
        return file.readAll().count("/Subtype /Image");
    };
    EXPECT_EQ(exportImages(), 0) << "never drawn: the listing";
    ASSERT_TRUE(noteAppC::mermaidCacheStore(
        noteAppC::diagramCachePath(DiagramKind::Graphviz, kDot, noteAppC::diagramConfig(DiagramKind::Graphviz, Theme::builtin(Theme::Mode::Dark))),
        "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"300\" height=\"100\" viewBox=\"0 0 300 100\">"
        "<rect width=\"300\" height=\"100\" fill=\"#ECECFF\"/></svg>"));
    EXPECT_GE(exportImages(), 1) << "drawn in the editor: its image";
}

// --- the window's settings ---------------------------------------------------

namespace {
class DiagramWindowTest : public DiagramTest {
protected:
    void SetUp() override
    {
        DiagramTest::SetUp();
        QFile note(dir.filePath(QStringLiteral("Note.md")));
        ASSERT_TRUE(note.open(QIODevice::WriteOnly));
        for (const Case &c : kCases)
            note.write(("```" + QByteArray(c.language) + "\n" + c.source.toUtf8() + "\n```\n\n").data());
        note.write("after\n");
        note.close();
    }
    void TearDown() override
    {
        Settings config = Settings::load();
        for (const char *key : {"render.plantuml", "render.tikz", "render.graphviz"})
            config.reset(QString::fromLatin1(key));
        config.save();
    }
    void configure(const char *key, bool on)
    {
        Settings config = Settings::load();
        config.setValue(QString::fromLatin1(key), on);
        ASSERT_TRUE(config.save());
    }
    bool drawn(MarkdownEditor *editor, const QString &source)
    {
        return !editor->embedImageSizeAt(editor->noteText().indexOf(source)).isEmpty();
    }
};
} // namespace

TEST_F(DiagramWindowTest, TheSettingsAreOnByDefault)
{
    for (const char *key : {"render.plantuml", "render.tikz", "render.graphviz"})
        EXPECT_TRUE(Settings::defaultValue(QString::fromLatin1(key)).toBool()) << key;
    EXPECT_EQ(Settings::defaultValue(QStringLiteral("render.plantumlTool")).toString(), QStringLiteral("plantuml"));
    EXPECT_EQ(Settings::defaultValue(QStringLiteral("render.latexTool")).toString(), QStringLiteral("latex"));
    EXPECT_EQ(Settings::defaultValue(QStringLiteral("render.dotTool")).toString(), QStringLiteral("dot"));
}

TEST_F(DiagramWindowTest, OnTheDiagramsAreDrawnInTheWindowAndOffTheSourceShows)
{
    NEED("plantuml");
    NEED("latex");
    NEED("dot");
    configure("render.tikz", false);
    MainWindow window;
    window.show();
    window.openVault(dir.path());
    window.openNote(dir.filePath(QStringLiteral("Note.md")));
    MarkdownEditor *editor = window.editor();
    editor->setCursorOffset(editor->noteText().size());
    WAIT_TRUE(drawn(editor, kPlantUML), 30000);
    WAIT_TRUE(drawn(editor, kDot), 30000);
    QTest::qWait(500);
    EXPECT_FALSE(drawn(editor, kTikZ)) << "off";
    // Publish and PDF export find what the window drew by the fence's content.
    EXPECT_FALSE(cached(DiagramKind::Graphviz, kDot).isEmpty());
    EXPECT_TRUE(cached(DiagramKind::TikZ, kTikZ).isEmpty());
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
