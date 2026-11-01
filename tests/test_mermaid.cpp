// ```mermaid fences: MermaidBlock.cpp's renderer, driven with a fake `mmdr`
// (a shell script that counts its runs in a file and writes a fixed SVG), on
// its own and through the editor and the window's `render.mermaid` setting.
// Only TheInstalledMmdrDrawsADarkDiagram needs the real tool: it is skipped
// when `mmdr` is absent, and fails instead when NOTEAPPC_REQUIRE_TOOLS=1.
// mmdr runs in bubblewrap's sandbox where bwrap is installed and as a plain
// process where not: the fake keeps its books in files outside its own
// directory, which the sandbox hides, so the fixture hides bwrap from PATH
// (the unsandboxed run) and the tests of the sandbox put it back.

#include <gtest/gtest.h>

#include <atomic>
#include <memory>

#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThread>
#include <QTest>

#include "MainWindow.h"
#include "MarkdownEditor.h"
#include "MermaidBlock.h"
#include "DiagramRetry.h"
#include "MermaidCache.h"
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
        // A PATH of what the fake script runs (sh, cat, cp, sleep) and no bwrap: see the top of the file.
        oldPath = qgetenv("PATH");
        ASSERT_TRUE(QDir().mkpath(dir.filePath(QStringLiteral("bin"))));
        for (const QString &name : {QStringLiteral("sh"), QStringLiteral("cat"), QStringLiteral("cp"), QStringLiteral("sleep")})
            ASSERT_TRUE(QFile::link(QStandardPaths::findExecutable(name), dir.filePath(QStringLiteral("bin/") + name)));
        qputenv("PATH", dir.filePath(QStringLiteral("bin")).toUtf8());
    }
    void TearDown() override { qputenv("PATH", oldPath); }

    /// The tests of the sandbox: a PATH that finds bwrap, skipping (failing under NOTEAPPC_REQUIRE_TOOLS=1) without it.
    bool withBubblewrap()
    {
        qputenv("PATH", oldPath);
        if (!QStandardPaths::findExecutable(QStringLiteral("bwrap")).isEmpty())
            return true;
        EXPECT_NE(qEnvironmentVariable("NOTEAPPC_REQUIRE_TOOLS"), QLatin1String("1")) << "NOTEAPPC_REQUIRE_TOOLS=1 and bwrap is missing";
        return false;
    }

    /// A tool at dir/name that runs `script` as it is: for the sandbox, where the fake's books are out of reach.
    QString rawFake(const QString &name, const QString &script)
    {
        const QString path = dir.filePath(name);
        QFile file(path);
        EXPECT_TRUE(file.open(QIODevice::WriteOnly));
        file.write(("#!/bin/sh\n" + script).toUtf8());
        file.close();
        QFile::setPermissions(path, QFile::permissions(path) | QFile::ExeOwner);
        return path;
    }

    /// A tool at dir/name that appends a line to dir/runs per run, keeps the
    /// config it was given in dir/config, reads the source on stdin, waits
    /// `delay` seconds, and writes kSVG to stdout (mmdr's `-i - -c config`).
    /// `body` replaces that last part when set.
    QString fake(const QString &name, const QString &delay = QStringLiteral("0"), const QString &body = QString())
    {
        const QString path = dir.filePath(name);
        QDir().mkpath(QFileInfo(path).path());
        QFile file(path);
        EXPECT_TRUE(file.open(QIODevice::WriteOnly));
        QString script = QStringLiteral(
                             "#!/bin/sh\necho run >> '%1'\ncfg=\n"
                             "while [ $# -gt 0 ]; do case \"$1\" in -c) cfg=\"$2\";; esac; shift; done\n"
                             "[ -n \"$cfg\" ] && cp \"$cfg\" '%2'\ncat > '%4'\nsleep %3\n")
                             .arg(runsFile(), dir.filePath(QStringLiteral("config")), delay, dir.filePath(QStringLiteral("stdin")));
        script += body.isEmpty() ? QStringLiteral("cat <<'EOF'\n") + QLatin1String(kSVG) + QStringLiteral("\nEOF\n") : body;
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
    QByteArray oldPath;
};

} // namespace

// First in the file on purpose: the prune is once per process, and this is the
// one test that must see it happen.
TEST_F(MermaidTest, TheCachePrunesOnFirstUseAndNotBefore)
{
    const QString cache = dir.filePath(QStringLiteral("cache/notolith/mermaid"));
    ASSERT_TRUE(QDir().mkpath(cache));
    const auto seed = [&](const QString &name, int daysOld) {
        QFile file(cache + QLatin1Char('/') + name);
        EXPECT_TRUE(file.open(QIODevice::WriteOnly));
        file.write("<svg/>");
        file.flush(); // or closing it later would stamp the time again
        file.setFileTime(QDateTime::currentDateTime().addDays(-daysOld), QFileDevice::FileModificationTime);
        return file.fileName();
    };
    const QString stale = seed(QStringLiteral("stale.svg"), 45);
    const QString fresh = seed(QStringLiteral("fresh.svg"), 2);
    const auto renderer = rendererFor(fake(QStringLiteral("mmdr")));
    EXPECT_TRUE(QFile::exists(stale)); // making the renderer prunes nothing
    ask(renderer, QStringLiteral("graph TD\nA-->B"));
    EXPECT_TRUE(QTest::qWaitFor([&] { return !QFile::exists(stale); }, 15000));
    EXPECT_TRUE(QFile::exists(fresh));
}

TEST(MermaidPruneTest, DropsTheOldAndThenTheLeastRecentlyUsedBeyondTheSize)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const auto seed = [&](const QString &name, int bytes, int daysOld) {
        QFile file(dir.filePath(name));
        EXPECT_TRUE(file.open(QIODevice::WriteOnly));
        file.write(QByteArray(bytes, 'x'));
        file.flush();
        file.setFileTime(QDateTime::currentDateTime().addDays(-daysOld), QFileDevice::FileModificationTime);
    };
    seed(QStringLiteral("old.svg"), 10, 31);
    seed(QStringLiteral("a.svg"), 100, 5);
    seed(QStringLiteral("b.svg"), 100, 3);
    seed(QStringLiteral("c.svg"), 100, 1);
    seed(QStringLiteral("note.txt"), 1000, 90); // not the cache's: left alone
    noteAppC::pruneMermaidCache(dir.path(), 30, 250);
    QStringList left = QDir(dir.path()).entryList(QDir::Files, QDir::Name);
    EXPECT_EQ(left, (QStringList{QStringLiteral("b.svg"), QStringLiteral("c.svg"), QStringLiteral("note.txt")}));
    noteAppC::pruneMermaidCache(dir.path(), 30, 1000); // within both limits: nothing goes
    EXPECT_EQ(QDir(dir.path()).entryList(QDir::Files).size(), 3);
}

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

TEST_F(MermaidTest, TheSourceGoesInOnStdinAndTheToolsSVGIsCachedAsItIs)
{
    ask(rendererFor(fake(QStringLiteral("mmdr"))), QStringLiteral("graph TD\nA-->C"));
    QFile in(dir.filePath(QStringLiteral("stdin")));
    ASSERT_TRUE(in.open(QIODevice::ReadOnly));
    EXPECT_EQ(in.readAll(), QByteArray("graph TD\nA-->C"));
    const QByteArray cached = noteAppC::mermaidCachedSVG(QStringLiteral("graph TD\nA-->C"), Theme::builtin(Theme::Mode::Dark));
    EXPECT_TRUE(cached.contains("#ECECFF")) << "mmdr honours the colours it is given, so the SVG is not re-coloured";
}

TEST_F(MermaidTest, WithoutBubblewrapTheToolKeepsTheSandboxsLimitsAndNoneOfTheEnvironment)
{
    // The plain-process fallback is what a shared note's fence reaches where bwrap is missing: it must not be
    // a way round the limits, nor see this process's variables. The fake records what it was given.
    qputenv("NOTEAPPC_TEST_SECRET", "hunter2");
    const QString books = dir.filePath(QStringLiteral("seen"));
    ask(rendererFor(fake(QStringLiteral("mmdr"), QStringLiteral("0"),
                         QStringLiteral("{ ulimit -v; ulimit -t; export -p; } > '%1'\ncat <<'EOF'\n%2\nEOF\n")
                             .arg(books, QLatin1String(kSVG)))),
        QStringLiteral("graph TD\nA-->G"));
    qunsetenv("NOTEAPPC_TEST_SECRET");
    QFile seen(books);
    ASSERT_TRUE(seen.open(QIODevice::ReadOnly));
    const QStringList lines = QString::fromUtf8(seen.readAll()).split(QLatin1Char('\n'));
    ASSERT_GE(lines.size(), 3);
    EXPECT_EQ(lines[0], QStringLiteral("4194304")) << "RLIMIT_AS, in KiB";
    EXPECT_EQ(lines[1], QStringLiteral("25")) << "RLIMIT_CPU";
    QStringList names;
    for (const QString &line : lines.mid(2))
        if (line.startsWith(QLatin1String("export ")))
            names << line.mid(7).section(QLatin1Char('='), 0, 0);
    for (const char *own : {"PWD", "OLDPWD", "SHLVL", "_"}) // what the shell itself exports
        names.removeAll(QLatin1String(own));
    names.sort();
    EXPECT_EQ(names, (QStringList{QStringLiteral("HOME"), QStringLiteral("PATH")}));
}

TEST_F(MermaidTest, ResolvesTheToolFromTheCargoHomeBin)
{
    // A launcher's PATH has no cargo bin: the stub is only in $CARGO_HOME/bin.
    const QByteArray oldCargo = qgetenv("CARGO_HOME");
    qputenv("CARGO_HOME", dir.filePath(QStringLiteral("cargo")).toUtf8());
    fake(QStringLiteral("cargo/bin/mmdr"));
    const auto answer = ask(rendererFor(QStringLiteral("mmdr")), QStringLiteral("graph TD\nA-->I"));
    if (oldCargo.isNull())
        qunsetenv("CARGO_HOME");
    else
        qputenv("CARGO_HOME", oldCargo);
    EXPECT_FALSE(answer->result.isMarkdown) << answer->result.text.toStdString();
    EXPECT_EQ(runs(), 1);
}

TEST_F(MermaidTest, PdfExportDrawsTheCachedDiagramAndOtherwiseTheListing)
{
    const QString note = QStringLiteral("```mermaid\ngraph TD\nA-->B\n```\n");
    const auto exportImages = [&] {
        const QString path = dir.filePath(QStringLiteral("out.pdf"));
        QString error;
        EXPECT_TRUE(MarkdownEditor::exportPdf(path, note, QStringLiteral("T"), {}, MarkdownEditor::PdfOptions{}, &error))
            << qPrintable(error);
        QFile file(path);
        EXPECT_TRUE(file.open(QIODevice::ReadOnly));
        return file.readAll().count("/Subtype /Image");
    };
    EXPECT_EQ(exportImages(), 0) << "never drawn: the listing";
    ASSERT_TRUE(noteAppC::mermaidCacheStore(
        noteAppC::mermaidCachePath(QStringLiteral("graph TD\nA-->B"), noteAppC::mermaidConfig(Theme::builtin(Theme::Mode::Dark))),
        kSVG));
    EXPECT_GE(exportImages(), 1) << "drawn in the editor: its image";
}

TEST_F(MermaidTest, TheInstalledMmdrDrawsADarkDiagram)
{
    // The real tool, in the sandbox where bwrap is installed, on a machine that may be busy: asked again on a timeout.
    withBubblewrap();
    const auto answer = ask(diagramtest::retryingOnTimeout([] { return rendererFor(QStringLiteral("mmdr")); }),
                            QStringLiteral("flowchart LR\n  A[Start] --> B{Ok?}\n  B -- yes --> C[Done]\n  B -- no --> D[Retry]"));
    if (answer->result.isMarkdown && answer->result.text.contains(QLatin1String("install mmdr"))) {
        ASSERT_NE(qEnvironmentVariable("NOTEAPPC_REQUIRE_TOOLS"), QLatin1String("1"))
            << "NOTEAPPC_REQUIRE_TOOLS=1 and mmdr gave: " << answer->result.text.toStdString();
        GTEST_SKIP() << "mmdr is not installed";
    }
    ASSERT_FALSE(answer->result.isMarkdown) << "mmdr is installed and gave: " << answer->result.text.toStdString();
    const QImage image = answer->result.image;
    ASSERT_FALSE(image.isNull());
    EXPECT_LT(image.pixelColor(2, 2).lightnessF(), 0.3) << "the diagram's own background is the theme's dark one";
    int light = 0;
    for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x)
            light += image.pixelColor(x, y).lightnessF() > 0.6;
    EXPECT_GT(light, 50) << "light text and lines on it";
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
    WAIT_TRUE(!editor.embedImageSizeAt(fence).isEmpty(), 15000);
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
    const int afterFirst = runs(); // the tool's own answer is final: no second run without the config
    EXPECT_EQ(afterFirst, 1);
    ask(renderer, QStringLiteral("graph TD\nA--"));
    EXPECT_EQ(runs(), afterFirst) << "the failure is cached with the source";
}

TEST_F(MermaidTest, AToolThatHangsIsKilled)
{
    const QString tool = fake(QStringLiteral("mmdr"), QStringLiteral("30"));
    QElapsedTimer timer;
    timer.start();
    const auto answer = ask(rendererFor(tool), QStringLiteral("graph TD\nA-->H"));
    // Killed at 5 s, not left to sleep its 30 s: well inside ask()'s 15 s wait even when loaded.
    EXPECT_LT(timer.elapsed(), 14000);
    ASSERT_TRUE(answer->result.isMarkdown);
    EXPECT_TRUE(answer->result.text.contains(QLatin1String("timed out")));
}

// One hung fence must not hold the others: the hang (30 s of sleep, killed at 5 s) has a thread, the next fence
// of the kind has the other, and answers long before the hang is killed.
TEST_F(MermaidTest, AHungToolDoesNotBlockTheOtherFences)
{
    const QString tool = rawFake(QStringLiteral("mmdr"),
                                 QStringLiteral("in=$(cat)\ncase \"$in\" in *HANG*) sleep 30;; esac\ncat <<'EOF'\n") + QLatin1String(kSVG)
                                     + QStringLiteral("\nEOF\n"));
    const auto renderer = rendererFor(tool);
    auto hung = std::make_shared<Answer>();
    auto hungAgain = std::make_shared<Answer>();
    QElapsedTimer timer;
    timer.start();
    // The same text twice, as the editor asks at two widths: the second waits for the first, not on a thread.
    for (const auto &answer : {hung, hungAgain})
        renderer(QStringLiteral("mermaid"), QStringLiteral("graph TD\nHANG-->A"), 600, QStringLiteral("dark"),
                 [answer](MarkdownEditor::CodeBlockResult result) {
                     answer->result = std::move(result);
                     answer->done = true;
                 });
    QTest::qWait(300); // the hang has its thread and is sleeping
    const auto fast = ask(renderer, QStringLiteral("graph TD\nFAST-->B"));
    EXPECT_FALSE(fast->result.isMarkdown) << fast->result.text.toStdString();
    EXPECT_LT(timer.elapsed(), 3000) << "behind the hang's 5 s kill";
    EXPECT_FALSE(hung->done.load());
    // Not left for the test to end: the kill comes, and says so.
    ASSERT_TRUE(QTest::qWaitFor([&] { return hung->done.load() && hungAgain->done.load(); }, 15000));
    EXPECT_TRUE(hung->result.text.contains(QLatin1String("timed out")));
    EXPECT_TRUE(hungAgain->result.text.contains(QLatin1String("timed out")));
}

// --- the sandbox --------------------------------------------------------------

TEST_F(MermaidTest, InTheSandboxTheToolStillGetsTheSourceAndTheColoursAndItsSVGIsCached)
{
    if (!withBubblewrap())
        GTEST_SKIP() << "bwrap is not installed";
    // It cannot keep books outside its directory: what it was given comes back in its SVG.
    const QString tool = rawFake(QStringLiteral("mmdr"),
                                 QStringLiteral("cfg=\nwhile [ $# -gt 0 ]; do case \"$1\" in -c) cfg=\"$2\";; esac; shift; done\n"
                                                "echo '<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"300\" height=\"100\"><desc>'\n"
                                                "cat \"$cfg\"; echo; cat\necho '</desc></svg>'\n"));
    const auto answer = ask(rendererFor(tool), QStringLiteral("graph TD\nA-->C"));
    ASSERT_FALSE(answer->result.isMarkdown) << answer->result.text.toStdString();
    const QByteArray cached = noteAppC::mermaidCachedSVG(QStringLiteral("graph TD\nA-->C"), Theme::builtin(Theme::Mode::Dark));
    EXPECT_TRUE(cached.contains("\"theme\":\"dark\"")) << cached.constData();
    EXPECT_TRUE(cached.contains(Theme::builtin().color(QStringLiteral("--background-primary")).name().toUtf8())) << cached.constData();
    EXPECT_TRUE(cached.contains("graph TD\nA-->C")) << cached.constData();
}

TEST_F(MermaidTest, InTheSandboxTheToolCannotReadTheHomeOrWriteOutsideItsDirectory)
{
    if (!withBubblewrap())
        GTEST_SKIP() << "bwrap is not installed";
    const QString secret = dir.filePath(QStringLiteral("secret"));
    const QString written = dir.filePath(QStringLiteral("written"));
    QFile file(secret);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.write("SECRETTEXT");
    file.close();
    // A fence is a stranger's input: a compromised mmdr tries the files a note's reader owns.
    const QString tool = rawFake(QStringLiteral("mmdr"),
                                 QStringLiteral("echo '<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"300\" height=\"100\"><desc>'\n"
                                                "cat '%1' 2>&1; cat /etc/passwd 2>&1; echo x > '%2' 2>&1\n"
                                                "echo '</desc></svg>'\n")
                                     .arg(secret, written));
    const auto answer = ask(rendererFor(tool), QStringLiteral("graph TD\nA-->R"));
    ASSERT_FALSE(answer->result.isMarkdown) << answer->result.text.toStdString();
    const QByteArray cached = noteAppC::mermaidCachedSVG(QStringLiteral("graph TD\nA-->R"), Theme::builtin(Theme::Mode::Dark));
    EXPECT_FALSE(cached.isEmpty());
    EXPECT_FALSE(cached.contains("SECRETTEXT")) << "the tool read a file in the home";
    EXPECT_FALSE(cached.contains("root:")) << "the tool read /etc/passwd";
    EXPECT_FALSE(QFile::exists(written)) << "the tool wrote outside its directory";
}

TEST_F(MermaidTest, WithoutBubblewrapTheDiagramIsStillDrawnByAPlainProcess)
{
    // The fixture's PATH has no bwrap: unlike the other tools, Mermaid is not refused (it is on by default).
    ASSERT_TRUE(QStandardPaths::findExecutable(QStringLiteral("bwrap")).isEmpty());
    const auto answer = ask(rendererFor(fake(QStringLiteral("mmdr"))), QStringLiteral("graph TD\nA-->N"));
    EXPECT_FALSE(answer->result.isMarkdown) << answer->result.text.toStdString();
    EXPECT_EQ(runs(), 1);
    EXPECT_FALSE(noteAppC::mermaidCachedSVG(QStringLiteral("graph TD\nA-->N"), Theme::builtin(Theme::Mode::Dark)).isEmpty());
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
        MermaidTest::TearDown();
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
    WAIT_TRUE(!editor->embedImageSizeAt(fence).isEmpty(), 15000);
    EXPECT_EQ(runs(), 1);
    // Publish and PDF export find what the window drew by the fence's content, the same text.
    EXPECT_FALSE(noteAppC::mermaidCachedSVG(QStringLiteral("graph TD\nA-->B"), Theme::builtin(Theme::Mode::Dark)).isEmpty());
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
