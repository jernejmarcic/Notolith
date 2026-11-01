// Regression test for demo/: opens the whole demo vault in a real MainWindow
// and walks every note, canvas and base file, painting each so a parser or
// layout crash shows up as a test failure instead of something a person has
// to notice by eye. See demo/Welcome.md for what the vault covers and
// docs/DEMO-ASSETS.md for where its binary assets came from.
//
// NOTEAPPC_DEMO_VAULT_PATH is the absolute path to demo/, set by
// tests/CMakeLists.txt so this does not depend on the working directory a
// test runner happens to use.

#include <gtest/gtest.h>

#include <QApplication>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QImage>
#include <QListWidget>
#include <QRegularExpression>
#include <QScrollBar>
#include <QAbstractButton>
#include <QAction>
#include <QDir>
#include <QSignalSpy>
#include <QTabBar>
#include <QTimer>
#include <QWidget>
#include <QTest>
#include <QTemporaryDir>

#include <algorithm>
#include <functional>

#include "WaitFor.h"
#include "Index.h"
#include "MainWindow.h"
#include "MarkdownEditor.h"
#include "PopoutWindow.h"

using noteAppC::Index;
using noteAppC::MainWindow;
using noteAppC::MarkdownEditor;

namespace {

QString vaultPath()
{
    return QStringLiteral(NOTEAPPC_DEMO_VAULT_PATH);
}

void copyTree(const QString &from, const QString &to)
{
    QDir().mkpath(to);
    const QFileInfoList entries = QDir(from).entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden);
    for (const QFileInfo &entry : entries) {
        if (entry.isDir())
            copyTree(entry.filePath(), to + QLatin1Char('/') + entry.fileName());
        else
            QFile::copy(entry.filePath(), to + QLatin1Char('/') + entry.fileName());
    }
}

// Every note, canvas, base, PDF and book under the vault, vault-relative, sorted -
// deterministic order for a reproducible failure. .obsidian's own JSON is
// vault configuration, not something MainWindow::openNote ever opens, so it
// is not walked here.
QStringList demoFiles()
{
    QStringList files;
    // Only what this build can open: a minimal build has no canvas or bases.
    QStringList kinds{QStringLiteral("*.md")};
#ifdef NOTEAPPC_HAVE_CANVAS
    kinds << QStringLiteral("*.canvas");
#endif
#ifdef NOTEAPPC_HAVE_BASES
    kinds << QStringLiteral("*.base");
#endif
#ifdef NOTEAPPC_HAVE_PDFVIEW
    kinds << QStringLiteral("*.pdf");
#endif
#ifdef NOTEAPPC_HAVE_EPUB
    kinds << QStringLiteral("*.epub");
#endif
    QDirIterator it(vaultPath(), kinds, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext())
        files << it.next();
    std::sort(files.begin(), files.end());
    return files;
}

// du -sb equivalent: every byte under demo/, hidden files included, the same
// total the "under 5 MB" rule is measured against.
qint64 totalVaultBytes()
{
    qint64 total = 0;
    QDirIterator it(vaultPath(), QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        total += it.fileInfo().size();
    }
    return total;
}

// Every qWarning/qCritical/qFatal the walk below emits. A demo file that makes
// the app complain (a bad regex, a null pixmap, a QObject::connect that found
// no signal) is a bug even when nothing crashes, and nobody reads the console
// while a test runs. Collected by a message handler installed in main().
QStringList g_messages;

void collectMessage(QtMsgType type, const QMessageLogContext &, const QString &message)
{
    if (type == QtWarningMsg || type == QtCriticalMsg || type == QtFatalMsg)
        g_messages << message;
    else
        return;
    fprintf(stderr, "%s\n", qPrintable(message));
}

// Messages that come from the offscreen platform or the harness rather than
// from noteAppC, one substring each. Keep it short; anything new is a failure.
const QStringList &allowedMessages()
{
    static const QStringList allowed{
        // The offscreen QPA plugin has no window manager to propagate hints to.
        QStringLiteral("This plugin does not support propagateSizeHints"),
        QStringLiteral("This plugin does not support grabbing the keyboard"),
        QStringLiteral("This plugin does not support raise()"),
    };
    return allowed;
}

QStringList unexpectedMessages()
{
    QStringList bad;
    for (const QString &message : std::as_const(g_messages)) {
        const bool allowed = std::any_of(allowedMessages().cbegin(), allowedMessages().cend(),
                                         [&](const QString &needle) { return message.contains(needle); });
        if (!allowed)
            bad << message;
    }
    return bad;
}

// Where two neighbouring blocks' boxes cross, or empty. The note is scrolled
// through first so every block is laid out for real, not at its estimate.
QString firstBlockOverlap(MarkdownEditor *editor)
{
    QScrollBar *bar = editor->verticalScrollBar();
    const int step = qMax(100, editor->viewport()->height() / 2);
    QString found;
    for (int at = 0; at <= bar->maximum() + step && found.isEmpty(); at += step) {
        bar->setValue(at);
        QList<MarkdownEditor::BlockBox> boxes = editor->blockBoxes();
        boxes.removeIf([](const MarkdownEditor::BlockBox &box) { return box.rect.isEmpty(); });
        for (qsizetype i = 1; i < boxes.size(); ++i) {
            if (boxes.at(i).rect.top() < boxes.at(i - 1).rect.bottom() - 0.01) {
                found = QStringLiteral("block at source %1 starts %2 px inside the one before it")
                            .arg(boxes.at(i).source.start)
                            .arg(boxes.at(i - 1).rect.bottom() - boxes.at(i).rect.top());
                break;
            }
        }
    }
    bar->setValue(0);
    return found;
}

// Writes the grab as <NOTEAPPC_DEMO_SHOTS>/<vault-relative path, / as __>.<mode>.png
// when that variable is set, for looking at the renders by eye. Scratch only.
void maybeSaveShot(const QImage &image, const QString &path, const char *mode)
{
    const QString dir = qEnvironmentVariable("NOTEAPPC_DEMO_SHOTS");
    if (dir.isEmpty())
        return;
    QDir().mkpath(dir);
    QString name = QDir(vaultPath()).relativeFilePath(path);
    name.replace(QLatin1Char('/'), QLatin1String("__"));
    image.save(QStringLiteral("%1/%2.%3.png").arg(dir, name, QLatin1String(mode)));
}

} // namespace

TEST(DemoVaultTest, StaysUnderTheFiveMegabyteBudget)
{
    ASSERT_TRUE(QFileInfo::exists(vaultPath())) << vaultPath().toStdString();
    const qint64 bytes = totalVaultBytes();
    EXPECT_LT(bytes, qint64(5) * 1024 * 1024)
        << bytes << " bytes - see docs/DEMO-ASSETS.md before adding anything larger.";
}

TEST(DemoVaultTest, HasContentToWalk)
{
    // A guard against the file-listing logic itself silently finding
    // nothing and the walk test below passing for the wrong reason.
    ASSERT_GT(demoFiles().size(), 50);
}

TEST(DemoVaultTest, EveryFileOpensAndPaintsInEveryMode)
{
    QElapsedTimer clock;
    clock.start();

    MainWindow window;
    window.openVault(vaultPath());
#ifdef NOTEAPPC_HAVE_PDFVIEW
    window.scanModulesNow(); // the PDF viewer is a module: found before the demo's PDF is opened
#endif
    // Every note indexed, however warm the cache was: a warm one says
    // Index::changed only inside openVault, before a spy could wait for it.
    const QStringList all = demoFiles();
    const qsizetype notes = std::count_if(all.cbegin(), all.cend(), [](const QString &file) {
        return file.endsWith(QLatin1String(".md"));
    });
    WAIT_TRUE(window.index()->noteCount() >= notes, 15000);
    window.resize(1280, 900);
    window.show();
    QApplication::processEvents();

    const QStringList files = demoFiles();
    ASSERT_FALSE(files.isEmpty());
    g_messages.clear();

    for (const QString &path : files) {
        SCOPED_TRACE(path.toStdString());

        ASSERT_TRUE(window.openNote(path)) << "failed to open " << path.toStdString();
        QApplication::processEvents();

        const QImage livePreview = window.grab().toImage();
        EXPECT_FALSE(livePreview.isNull());
        maybeSaveShot(livePreview, path, "live");

        // Reading and live-preview mode only mean anything for a Markdown
        // note - a canvas or base file has no editor mode to switch. Through
        // Ctrl+E's own command, there and back.
        if (path.endsWith(QLatin1String(".md"), Qt::CaseInsensitive)) {
            MarkdownEditor *editor = window.editor();
            ASSERT_NE(editor, nullptr);
            QAction *toggle = window.findChild<QAction *>(QStringLiteral("markdown:toggle-preview"));
            ASSERT_NE(toggle, nullptr);

            EXPECT_EQ(firstBlockOverlap(editor).toStdString(), "") << "live preview";
            editor->setMode(MarkdownEditor::Mode::Source);
            QApplication::processEvents();
            const QImage source = window.grab().toImage();
            EXPECT_FALSE(source.isNull());
            maybeSaveShot(source, path, "source");
            editor->setMode(MarkdownEditor::Mode::LivePreview);

            toggle->trigger();
            ASSERT_EQ(editor->mode(), MarkdownEditor::Mode::Reading);
            QApplication::processEvents();
            const QImage reading = window.grab().toImage();
            EXPECT_FALSE(reading.isNull());
            maybeSaveShot(reading, path, "reading");
            EXPECT_EQ(firstBlockOverlap(editor).toStdString(), "") << "reading";

            toggle->trigger();
            ASSERT_EQ(editor->mode(), MarkdownEditor::Mode::LivePreview);
            QApplication::processEvents();
        }
    }

    // "well under a minute" for the whole walk.
    EXPECT_LT(clock.elapsed(), 60000);
    EXPECT_TRUE(unexpectedMessages().isEmpty()) << unexpectedMessages().join(QLatin1Char('\n')).toStdString();
}

// The second "Library" pane in the walk's screenshots is the vault's own saved
// workspace (demo/.obsidian/workspace.json puts Library.base in a right-hand
// split), not a leftover: opening a base or a canvas, like a note, replaces the
// tab it is opened in and adds no pane.
#ifdef NOTEAPPC_HAVE_BASES // the saved split holds a base: without bases it would be an empty tab
TEST(DemoVaultTest, OpeningABaseOrCanvasReplacesTheCurrentTabAndAddsNoPane)
{
    // On a copy with the committed workspace: using the app on demo/ saves
    // whatever layout was left open into its workspace.json.
    QTemporaryDir copy;
    ASSERT_TRUE(copy.isValid());
    const QString root = copy.path() + QStringLiteral("/demo");
    copyTree(vaultPath(), root);
    QFile::remove(root + QStringLiteral("/.obsidian/workspace.json"));
    ASSERT_TRUE(QFile::copy(QStringLiteral(NOTEAPPC_FIXTURES_DIR "/demo-workspace.json"),
                            root + QStringLiteral("/.obsidian/workspace.json")));

    MainWindow window;
    window.openVault(root);
    window.resize(1280, 900);
    window.show();
    QApplication::processEvents();

    const auto tabs = [&window] {
        QStringList titles;
        const auto bars = window.findChildren<QTabBar *>(QStringLiteral("noteTabs"));
        for (QTabBar *bar : bars) {
            for (int i = 0; i < bar->count(); ++i)
                titles << bar->tabText(i);
        }
        titles.sort(); // the bars come in no order
        return titles;
    };
    const auto panes = [&window] { return window.findChildren<QTabBar *>(QStringLiteral("noteTabs")).size(); };

    // What workspace.json says: Welcome and Callouts on the left, Library on the right.
    EXPECT_EQ(panes(), 2);
    EXPECT_EQ(tabs(), (QStringList{QStringLiteral("Callouts"), QStringLiteral("Library"), QStringLiteral("Welcome")}));

    ASSERT_TRUE(window.openNote(root + QStringLiteral("/Welcome.md")));
    for (const QString &file : {
#ifdef NOTEAPPC_HAVE_CANVAS
             QStringLiteral("/Canvas/Board.canvas"),
#endif
             QStringLiteral("/Library/Library.base"),
             QStringLiteral("/Welcome.md")}) {
        SCOPED_TRACE(file.toStdString());
        ASSERT_TRUE(window.openNote(root + file));
        QApplication::processEvents();
        EXPECT_EQ(panes(), 2) << "no pane is added";
        EXPECT_EQ(tabs().size(), 3) << "the active tab is reused, no tab is added";
        EXPECT_TRUE(tabs().contains(QFileInfo(file).completeBaseName())) << tabs().join(QLatin1Char(',')).toStdString();
    }
}
#endif

// Every command in the palette once, on a scratch copy of the vault so an
// editing command cannot dirty demo/. Modal dialogs (the file pickers, the
// publish and recovery dialogs) are dismissed by a timer the way a person
// would press Escape. Commands that leave the process (sync), need hardware
// (the recorder) or run user code (notebooks) are skipped, and the vault
// picker is a native dialog this offscreen run cannot answer.
TEST(DemoVaultTest, EveryCommandRunsWithoutWarnings)
{
    QTemporaryDir copy;
    ASSERT_TRUE(copy.isValid());
    const QString root = copy.path() + QStringLiteral("/demo");
    copyTree(vaultPath(), root);

    MainWindow window;
    window.openVault(root);
    WAIT_TRUE(window.index()->noteCount() > 20, 15000);
    window.resize(1280, 900);
    window.show();
    ASSERT_TRUE(window.openNote(root + QStringLiteral("/Welcome.md")));
    QApplication::processEvents();
    g_messages.clear();

    QTimer dismiss;
    dismiss.setInterval(20);
    QObject::connect(&dismiss, &QTimer::timeout, [] {
        if (QWidget *modal = QApplication::activeModalWidget())
            modal->close();
    });
    dismiss.start();

    const QStringList skip{QStringLiteral("app:open-vault"), QStringLiteral("sync:sign-in"),
                           QStringLiteral("sync:sync-now"), QStringLiteral("audio-recorder:start"),
                           QStringLiteral("audio-recorder:stop"), QStringLiteral("command-palette:open")};
    int ran = 0;
    const auto actions = window.findChildren<QAction *>(Qt::FindDirectChildrenOnly);
    for (QAction *action : actions) {
        const QString id = action->objectName();
        if (!id.contains(QLatin1Char(':')) || skip.contains(id) || id.startsWith(QLatin1String("notebooks:")))
            continue;
        if (!action->isEnabled())
            continue;
        SCOPED_TRACE(id.toStdString());
        action->trigger();
        QApplication::processEvents();
        window.grab();
        ++ran;
    }
    // The detach commands took the sidebars out into windows; closing those puts
    // them back, as a user would, for the pane check below.
    for (QWidget *top : QApplication::topLevelWidgets()) {
        auto *popout = qobject_cast<noteAppC::PopoutWindow *>(top);
        if (popout && popout->kind() != noteAppC::PopoutWindow::Kind::Leaves)
            popout->close();
    }
    // Every right-sidebar pane (search, backlinks, tags, properties, outline,
    // ...): each has a tab button, and showing the pane fills it for the note.
    int panes = 0;
    // Snapshot only the tab buttons: a snapshot of every button also holds the
    // RecoveryDialog's (WA_DeleteOnClose, closed by the timer above), which are
    // freed mid-loop by the event processing below.
    for (QAbstractButton *button : window.findChildren<QAbstractButton *>(QRegularExpression(QStringLiteral("TabButton$")))) {
        SCOPED_TRACE(button->objectName().toStdString());
        button->click();
        QApplication::processEvents();
        window.grab();
        ++panes;
    }
    EXPECT_GE(panes, 5);
    // Each settings page: the nav builds them lazily, so only showing one
    // proves it builds and paints.
    window.findChild<QAction *>(QStringLiteral("app:open-settings"))->trigger();
    QApplication::processEvents();
    auto *nav = window.findChild<QListWidget *>(QStringLiteral("settingsNav"));
    ASSERT_NE(nav, nullptr);
    for (int row = 0; row < nav->count(); ++row) {
        nav->setCurrentRow(row);
        QApplication::processEvents();
        const QImage page = nav->window()->grab().toImage(); // settings is a window of its own
        EXPECT_FALSE(page.isNull());
        maybeSaveShot(page, vaultPath() + QStringLiteral("/settings-") + nav->item(row)->text(), "page");
    }
    window.closeSettings();
    // Back to a normal view for the editor's own commands.
    ASSERT_TRUE(window.openNote(root + QStringLiteral("/Welcome.md")));
    for (const MarkdownEditor::Command &command : MarkdownEditor::commands()) {
        SCOPED_TRACE(command.id.toStdString());
        window.editor()->runCommand(command.id);
        QApplication::processEvents();
        ++ran;
    }
    window.grab();
    EXPECT_GT(ran, 40);
    EXPECT_TRUE(unexpectedMessages().isEmpty()) << unexpectedMessages().join(QLatin1Char('\n')).toStdString();
}

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    // Settings::configDirectory() is fixed to ~/.config/notolith regardless
    // of QCoreApplication's org/app name, so every test that builds a
    // MainWindow needs its own XDG_CONFIG_HOME - otherwise this would read
    // and could write whoever runs it their real settings file.
    QTemporaryDir configHome;
    qputenv("XDG_CONFIG_HOME", configHome.path().toUtf8());
    qInstallMessageHandler(collectMessage);
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("noteAppC-test"));
    QCoreApplication::setApplicationName(QStringLiteral("noteAppC-test"));
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
