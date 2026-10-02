// ```mermaid fences: MermaidBlock.cpp's renderer, driven with a fake `mmdr`
// (a shell script that counts its runs in a file and writes a fixed SVG), on
// its own and through the editor and the window's `render.mermaid` setting.
// Nothing here needs the real tool installed.

#include <gtest/gtest.h>

#include <atomic>
#include <memory>

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QThread>
#include <QTest>

#include "MainWindow.h"
#include "MarkdownEditor.h"
#include "MermaidBlock.h"
#include "Settings.h"
#include "Theme.h"
#include "WaitFor.h"

using noteAppC::MainWindow;
using noteAppC::MarkdownEditor;
using noteAppC::Settings;
using noteAppC::Theme;

namespace {

const char kSVG[] = R"(<svg xmlns="http://www.w3.org/2000/svg" width="300" height="100" viewBox="0 0 300 100">
<rect width="300" height="100" fill="#ECECFF"/><text x="10" y="50" fill="#333333">hi</text></svg>)";

const QString kNote = QStringLiteral("```mermaid\ngraph TD\nA-->B\n```\n\nafter\n");

class MermaidTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        ASSERT_TRUE(dir.isValid());
        // The disk cache is per test: an earlier test's diagram must not answer for this one.
        qputenv("XDG_CACHE_HOME", dir.filePath(QStringLiteral("cache")).toUtf8());
    }

    /// A tool at dir/name that appends a line to dir/runs per run, keeps the
    /// config it was given in dir/config, waits `delay` seconds, and writes
    /// kSVG to its -o argument. `body` replaces that last part when set.
    QString fake(const QString &name, const QString &delay = QStringLiteral("0"), const QString &body = QString())
    {
        const QString path = dir.filePath(name);
        QFile file(path);
        EXPECT_TRUE(file.open(QIODevice::WriteOnly));
        QString script = QStringLiteral(
                             "#!/bin/sh\necho run >> '%1'\nout=; cfg=\n"
                             "while [ $# -gt 0 ]; do case \"$1\" in -o) out=\"$2\";; -c) cfg=\"$2\";; esac; shift; done\n"
                             "[ -n \"$cfg\" ] && cp \"$cfg\" '%2'\nsleep %3\n")
                             .arg(runsFile(), dir.filePath(QStringLiteral("config")), delay);
        script += body.isEmpty() ? QStringLiteral("cat > \"$out\" <<'EOF'\n") + QLatin1String(kSVG) + QStringLiteral("\nEOF\n")
                                 : body;
        file.write(script.toUtf8());
        file.close();
        QFile::setPermissions(path, QFile::permissions(path) | QFile::ExeOwner);
        return path;
    }
    QString runsFile() const { return dir.filePath(QStringLiteral("runs")); }
    int runs() const
    {
        QFile file(runsFile());
        return file.open(QIODevice::ReadOnly) ? file.readAll().count('\n') : 0;
    }

    struct Answer {
        std::atomic<bool> done{false};
        MarkdownEditor::CodeBlockResult result;
        QThread *thread = nullptr;
        qint64 callMs = 0;
    };
    /// One call of the renderer, waited for.
    std::shared_ptr<Answer> ask(const MarkdownEditor::CodeBlockRenderer &renderer, const QString &source)
    {
        auto answer = std::make_shared<Answer>();
        QElapsedTimer timer;
        timer.start();
        renderer(QStringLiteral("mermaid"), source, 600, QStringLiteral("dark"),
                 [answer](MarkdownEditor::CodeBlockResult result) {
                     answer->result = std::move(result);
                     answer->thread = QThread::currentThread();
                     answer->done = true;
                 });
        answer->callMs = timer.elapsed();
        EXPECT_TRUE(QTest::qWaitFor([&] { return answer->done.load(); }, 15000));
        return answer;
    }
    static MarkdownEditor::CodeBlockRenderer rendererFor(const QString &tool)
    {
        return noteAppC::mermaidRenderer(tool, [] { return Theme::builtin(Theme::Mode::Dark); });
    }

    QTemporaryDir dir;
};

} // namespace

TEST_F(MermaidTest, ARenderIsAJobOnAnotherThreadNotACallOnTheUIThread)
{
    const auto answer = ask(rendererFor(fake(QStringLiteral("mmdr"), QStringLiteral("0.5"))), QStringLiteral("graph TD\nA-->B"));
    EXPECT_LT(answer->callMs, 200); // the tool takes 500 ms; the call that asked did not wait for it
    EXPECT_NE(answer->thread, QThread::currentThread());
    ASSERT_FALSE(answer->result.isMarkdown);
    EXPECT_EQ(answer->result.image.width(), int(300 * answer->result.image.devicePixelRatio()));
    EXPECT_EQ(runs(), 1);
}

TEST_F(MermaidTest, TheToolIsGivenTheAppsColours)
{
    ask(rendererFor(fake(QStringLiteral("mmdr"))), QStringLiteral("graph TD\nA-->B"));
    QFile config(dir.filePath(QStringLiteral("config")));
    ASSERT_TRUE(config.open(QIODevice::ReadOnly));
    const QByteArray json = config.readAll();
    EXPECT_TRUE(json.contains("\"theme\":\"dark\"")) << json.constData();
    EXPECT_TRUE(json.contains(Theme::builtin().color(QStringLiteral("--background-primary")).name().toUtf8()));
}

TEST_F(MermaidTest, ALightDiagramInADarkThemeIsRecoloured)
{
    ask(rendererFor(fake(QStringLiteral("mmdr"))), QStringLiteral("graph TD\nA-->C"));
    bool found = false;
    for (const QFileInfo &info : QDir(noteAppC::mermaidCacheDirectory()).entryInfoList({QStringLiteral("*.svg")})) {
        QFile file(info.filePath());
        ASSERT_TRUE(file.open(QIODevice::ReadOnly));
        const QByteArray svg = file.readAll().toLower();
        if (svg.contains("<text")) {
            found = true;
            EXPECT_FALSE(svg.contains("#ececff")) << "the light fill was kept";
        }
    }
    EXPECT_TRUE(found);
}

TEST_F(MermaidTest, ADiagramIsDrawnInPlaceOfItsFenceAndTheCaretBringsTheSourceBack)
{
    const QString tool = fake(QStringLiteral("mmdr"), QStringLiteral("0.2"));
    MarkdownEditor editor;
    editor.resize(900, 600);
    editor.setNoteText(kNote);
    editor.setCodeBlockRenderer({QStringLiteral("mermaid")}, rendererFor(tool));
    editor.setCursorOffset(editor.noteText().size()); // off the fence
    editor.show();
    const qsizetype fence = editor.noteText().indexOf(QLatin1String("graph"));

    // The tool takes 200 ms: the editor laid the block out without it.
    editor.viewport()->grab();
    EXPECT_TRUE(editor.embedImageSizeAt(fence).isEmpty());
    WAIT_TRUE(!editor.embedImageSizeAt(fence).isEmpty(), 5000);
    EXPECT_EQ(editor.embedImageSizeAt(fence).height(), 100);
    EXPECT_EQ(runs(), 1);

    editor.enterBlockAt(fence); // inside: the source, to edit
    editor.viewport()->grab(); // laid out when painted
    EXPECT_TRUE(editor.embedImageSizeAt(fence).isEmpty());
    editor.setCursorOffset(editor.noteText().size());
    editor.viewport()->grab();
    EXPECT_FALSE(editor.embedImageSizeAt(fence).isEmpty());
    EXPECT_EQ(runs(), 1) << "the diagram came from the cache";
}

TEST_F(MermaidTest, ADiagramAlreadyMadeIsReadFromDiskNotMadeAgain)
{
    const QString tool = fake(QStringLiteral("mmdr"));
    ask(rendererFor(tool), QStringLiteral("graph TD\nA-->D"));
    ASSERT_EQ(runs(), 1);
    // A renderer of its own, as a new window or a changed setting makes: nothing in memory.
    const auto again = ask(rendererFor(tool), QStringLiteral("graph TD\nA-->D"));
    EXPECT_FALSE(again->result.isMarkdown);
    EXPECT_EQ(runs(), 1);
}

TEST_F(MermaidTest, AMissingToolShowsTheSourceAndAHintAndIsNotLookedForAgain)
{
    const QString tool = dir.filePath(QStringLiteral("not-yet"));
    const auto renderer = rendererFor(tool);
    const auto first = ask(renderer, QStringLiteral("graph TD\nA-->E"));
    ASSERT_TRUE(first->result.isMarkdown);
    EXPECT_TRUE(first->result.text.contains(QLatin1String("A-->E")));
    EXPECT_TRUE(first->result.text.contains(QLatin1String("not-yet was not found")));

    // The tool turns up, and a keystroke later the fence is another text: the miss is remembered.
    fake(QStringLiteral("not-yet"));
    const auto second = ask(renderer, QStringLiteral("graph TD\nA-->F"));
    EXPECT_TRUE(second->result.isMarkdown);
    EXPECT_EQ(runs(), 0);
    // A changed setting makes a new renderer, which looks again.
    EXPECT_FALSE(ask(rendererFor(tool), QStringLiteral("graph TD\nA-->F"))->result.isMarkdown);
    EXPECT_EQ(runs(), 1);
}

TEST_F(MermaidTest, ThePlainNameInTheDefaultHint)
{
    const auto answer = ask(rendererFor(QStringLiteral("no-such-tool-for-mermaid-tests")), QStringLiteral("graph TD\nA-->G"));
    EXPECT_TRUE(answer->result.text.contains(QLatin1String("was not found")));
    const auto defaultName = ask(rendererFor(QStringLiteral("mmdr")), QStringLiteral("graph TD\nA-->G"));
    // mmdr may really be installed where this runs; then it is a diagram, not a hint.
    if (defaultName->result.isMarkdown)
        EXPECT_TRUE(defaultName->result.text.contains(QLatin1String("install mmdr to render")));
}

TEST_F(MermaidTest, AFailingToolShowsTheSourceAndItsFirstErrorLineOnce)
{
    const QString tool = fake(QStringLiteral("mmdr"), QStringLiteral("0"),
                              QStringLiteral("echo 'Error: parse error on line 2' >&2; echo more >&2; exit 1\n"));
    const auto renderer = rendererFor(tool);
    const auto first = ask(renderer, QStringLiteral("graph TD\nA--"));
    ASSERT_TRUE(first->result.isMarkdown);
    EXPECT_TRUE(first->result.text.contains(QLatin1String("A--")));
    EXPECT_TRUE(first->result.text.contains(QLatin1String("parse error on line 2")));
    EXPECT_FALSE(first->result.text.contains(QLatin1String("more")));
    const int afterFirst = runs(); // with the config, and again without it
    EXPECT_EQ(afterFirst, 2);
    ask(renderer, QStringLiteral("graph TD\nA--"));
    EXPECT_EQ(runs(), afterFirst) << "the failure is cached with the source";
}

TEST_F(MermaidTest, AToolThatHangsIsKilled)
{
    const QString tool = fake(QStringLiteral("mmdr"), QStringLiteral("30"));
    QElapsedTimer timer;
    timer.start();
    const auto answer = ask(rendererFor(tool), QStringLiteral("graph TD\nA-->H"));
    EXPECT_LT(timer.elapsed(), 8000);
    ASSERT_TRUE(answer->result.isMarkdown);
    EXPECT_TRUE(answer->result.text.contains(QLatin1String("timed out")));
}

// --- the window's setting ---------------------------------------------------

namespace {
class MermaidWindowTest : public MermaidTest {
protected:
    void SetUp() override
    {
        MermaidTest::SetUp();
        QFile note(dir.filePath(QStringLiteral("Note.md")));
        ASSERT_TRUE(note.open(QIODevice::WriteOnly));
        note.write(kNote.toUtf8());
        note.close();
    }
    void TearDown() override
    {
        Settings config = Settings::load();
        config.reset(QStringLiteral("render.mermaid"));
        config.reset(QStringLiteral("render.mermaidTool"));
        config.save();
    }
    void configure(bool on, const QString &tool)
    {
        Settings config = Settings::load();
        config.setValue(QStringLiteral("render.mermaid"), on);
        config.setValue(QStringLiteral("render.mermaidTool"), tool);
        ASSERT_TRUE(config.save());
    }
};
} // namespace

TEST_F(MermaidWindowTest, TheSettingIsOnByDefaultAndTheToolIsMmdr)
{
    EXPECT_TRUE(Settings::defaultValue(QStringLiteral("render.mermaid")).toBool());
    EXPECT_EQ(Settings::defaultValue(QStringLiteral("render.mermaidTool")).toString(), QStringLiteral("mmdr"));
}

TEST_F(MermaidWindowTest, OnADiagramIsDrawnInTheWindow)
{
    configure(true, fake(QStringLiteral("mmdr")));
    MainWindow window;
    window.show();
    window.openVault(dir.path());
    window.openNote(dir.filePath(QStringLiteral("Note.md")));
    MarkdownEditor *editor = window.editor();
    editor->setCursorOffset(editor->noteText().size());
    const qsizetype fence = editor->noteText().indexOf(QLatin1String("graph"));
    WAIT_TRUE(!editor->embedImageSizeAt(fence).isEmpty(), 5000);
    EXPECT_EQ(runs(), 1);
}

TEST_F(MermaidWindowTest, OffTheSourceShowsAndTheToolIsNeverStarted)
{
    configure(false, fake(QStringLiteral("mmdr")));
    MainWindow window;
    window.show();
    window.openVault(dir.path());
    window.openNote(dir.filePath(QStringLiteral("Note.md")));
    MarkdownEditor *editor = window.editor();
    editor->setCursorOffset(editor->noteText().size());
    const qsizetype fence = editor->noteText().indexOf(QLatin1String("graph"));
    QTest::qWait(700);
    EXPECT_TRUE(editor->embedImageSizeAt(fence).isEmpty());
    EXPECT_EQ(runs(), 0);
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
