// The autosave timers commit a note's file on a thread of its own: what is
// on disk, what the tab says is modified, what file recovery keeps, and what
// every operation that needs the file sees. A hook on the save thread holds a
// commit open (a semaphore or a fixed delay), so nothing here races a sleep.

#include <gtest/gtest.h>

#include <unistd.h>

#include <atomic>
#include <functional>

#include <QAbstractButton>
#include <QImageReader>
#include <QScopeGuard>
#include <thread>
#include <QApplication>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QMessageBox>
#include <QMetaObject>
#include <QSemaphore>
#include <QStatusBar>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>
#include <QTimer>

#include "MainWindow.h"
#include "MarkdownEditor.h"
#include "Settings.h"
#include "Snapshots.h"
#include "Vault.h"

using noteAppC::MainWindow;
using noteAppC::MarkdownEditor;
using noteAppC::Snapshots;

namespace {

QByteArray readAll(const QString &path)
{
    QFile file(path);
    EXPECT_TRUE(file.open(QIODevice::ReadOnly)) << path.toStdString();
    return file.readAll();
}

void writeAll(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly)) << path.toStdString();
    file.write(bytes);
}

/// What the save thread's hook does: counts commits, notes whether one ran
/// off the UI thread, and (when `hold`) waits for release().
struct Gate {
    QSemaphore sem;
    std::atomic<int> commits{0};
    std::atomic<bool> offUiThread{false};
    bool hold = true;
    int delayMs = 0;

    std::function<void()> hook()
    {
        return [this] {
            ++commits;
            if (QThread::currentThread() != QCoreApplication::instance()->thread())
                offUiThread = true;
            if (delayMs > 0)
                QThread::msleep(unsigned(delayMs));
            if (hold)
                sem.tryAcquire(1, 10000); // a failed test must not hang for long
        };
    }
    void release() { sem.release(); }
};

#ifdef NOTEAPPC_HAVE_ENCRYPTION
void clickNextBox(const QString &text)
{
    QTimer::singleShot(0, [text] {
        if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
            for (QAbstractButton *button : box->buttons()) {
                if (button->text() == text)
                    button->click();
            }
        }
    });
}
#endif

void flush(MainWindow &window)
{
    QMetaObject::invokeMethod(&window, "flushSaves");
}

void autosave(MainWindow &window)
{
    QMetaObject::invokeMethod(&window, "autosaveInBackground");
}

/// How many Qt image-format plugins this process has mapped.
int imagePluginsLoaded()
{
    QFile maps(QStringLiteral("/proc/self/maps"));
    if (!maps.open(QIODevice::ReadOnly))
        return -1;
    int count = 0;
    for (const QByteArray &line : maps.readAll().split('\n')) {
        if (line.contains("/imageformats/"))
            ++count;
    }
    return count;
}

class AutosaveTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        ASSERT_TRUE(dir.isValid());
        ASSERT_TRUE(QDir(dir.path()).mkpath(QStringLiteral("Sub")));
        writeAll(path(QStringLiteral("Note.md")), "# Note\n\nBody.\n");
        noteAppC::Settings settings = noteAppC::Settings::load();
        settings.setValue(QStringLiteral("files.autosave"), true);
        settings.setValue(QStringLiteral("files.confirmDelete"), false);
        settings.setValue(QStringLiteral("files.deletedFiles"), QStringLiteral("permanent"));
        ASSERT_TRUE(settings.save());
    }
    QString path(const QString &relative) const { return dir.filePath(relative); }
    QString note() const { return path(QStringLiteral("Note.md")); }

    /// A window with the note open, the caret at its end, the timers parked.
    void open(MainWindow &window, const QString &file = QString())
    {
        window.setAutosaveIntervalsForTests(600000, 600000);
        window.openVault(dir.path());
        ASSERT_TRUE(window.openNote(file.isEmpty() ? note() : file));
        window.editor()->setCursorOffset(window.editor()->noteText().size());
    }

    QTemporaryDir dir;
};

} // namespace

// The module scan asks about every suffix a module claims, on a worker. It must
// not load the image plugins to answer: the app opens Obsidian's image set only,
// so a suffix only a plugin reads (tiff, heic) is a module's to claim.
TEST(ModuleSuffixTest, NoSuffixAsksThePlugins)
{
    ASSERT_EQ(imagePluginsLoaded(), 0) << "an earlier test loaded them";
    for (const char *suffix : {"ink", "md", "png", "pdf", "mp3", "glyphs"})
        EXPECT_EQ(MainWindow::isBuiltInSuffixForTests(QString::fromLatin1(suffix)),
                  QByteArray(suffix) == "md" || QByteArray(suffix) == "png" || QByteArray(suffix) == "mp3") // a PDF is the pdfview module's
            << suffix;
    EXPECT_EQ(imagePluginsLoaded(), 0) << "the scan loaded the image plugins for suffixes none of them reads";
    for (const char *suffix : {"tiff", "heic", "jxl", "eps", "psd"})
        EXPECT_FALSE(MainWindow::isBuiltInSuffixForTests(QString::fromLatin1(suffix))) << suffix;
    EXPECT_EQ(imagePluginsLoaded(), 0) << "the scan loaded the image plugins";
}

// The commit is not on the UI thread: with it held open the autosave call has
// returned, the file is still the old one and the tab still modified; released,
// the file and the tab catch up. A synchronous autosave fails the first check.
TEST_F(AutosaveTest, TheCommitRunsOnAnotherThreadAndTheTabCatchesUp)
{
    MainWindow window;
    Gate gate;
    window.setSaveJobHookForTests(gate.hook());
    open(window);
    QTest::keyClicks(window.editor(), QStringLiteral("a"));
    const QString typed = window.editor()->noteText();

    QElapsedTimer clock;
    clock.start();
    autosave(window);
    EXPECT_LT(clock.elapsed(), 2000) << "autosave waited for the held commit";
    EXPECT_EQ(readAll(note()), QByteArray("# Note\n\nBody.\n"));
    EXPECT_TRUE(window.editor()->isModified());

    gate.release();
    flush(window);
    EXPECT_EQ(gate.commits.load(), 1);
    EXPECT_TRUE(gate.offUiThread.load());
    EXPECT_EQ(readAll(note()), typed.toUtf8());
    EXPECT_FALSE(window.editor()->isModified());
}

// Twenty keystrokes each followed by an autosave, the event loop running in
// between: the file only ever grows, one commit at a time, and ends with the
// last text.
TEST_F(AutosaveTest, RapidSavesWithTheEventLoopRunningLandInOrder)
{
    MainWindow window;
    Gate gate;
    gate.hold = false;
    gate.delayMs = 3;
    window.setSaveJobHookForTests(gate.hook());
    open(window);
    qint64 lastSize = readAll(note()).size();
    for (int i = 0; i < 20; ++i) {
        QTest::keyClicks(window.editor(), QStringLiteral("x"));
        autosave(window);
        QTest::qWait(4); // finishSave runs between the keystrokes
        const qint64 size = readAll(note()).size();
        EXPECT_GE(size, lastSize);
        lastSize = size;
    }
    flush(window);
    autosave(window); // what finishSave() arms the timer for when the tab is behind
    flush(window);
    EXPECT_EQ(readAll(note()), window.editor()->noteText().toUtf8());
    EXPECT_TRUE(readAll(note()).contains("xxxxxxxxxxxxxxxxxxxx"));
    EXPECT_FALSE(window.editor()->isModified());
    EXPECT_TRUE(gate.offUiThread.load());
}

// A second autosave while the first commit is open does not queue another;
// typing in the meantime stays in the tab, modified, and the next autosave
// writes it.
TEST_F(AutosaveTest, TypingDuringACommitIsKeptAndSavedNext)
{
    MainWindow window;
    Gate gate;
    window.setSaveJobHookForTests(gate.hook());
    open(window);
    QTest::keyClicks(window.editor(), QStringLiteral("a"));
    const QString first = window.editor()->noteText();
    autosave(window);
    QTest::keyClicks(window.editor(), QStringLiteral("b"));
    autosave(window); // the path is in flight: skipped
    gate.hold = false;
    gate.release();
    flush(window);
    EXPECT_EQ(gate.commits.load(), 1);
    EXPECT_EQ(readAll(note()), first.toUtf8());
    EXPECT_TRUE(window.editor()->noteText().contains(QLatin1String("ab")));
    EXPECT_TRUE(window.editor()->isModified()) << "the second letter is not on disk yet";

    autosave(window);
    flush(window);
    EXPECT_EQ(gate.commits.load(), 2);
    EXPECT_EQ(readAll(note()), window.editor()->noteText().toUtf8());
    EXPECT_FALSE(window.editor()->isModified());
}

// A commit that fails (a folder that cannot be written to): the person is
// told, the tab stays modified, and once the folder allows it the next
// autosave goes through.
TEST_F(AutosaveTest, AFailedCommitIsReportedAndTheTabStaysModified)
{
    if (geteuid() == 0)
        GTEST_SKIP() << "root writes anywhere";
    const QString file = path(QStringLiteral("Sub/Failing.md"));
    writeAll(file, "# Failing\n");
    MainWindow window;
    open(window, file);
    QTest::keyClicks(window.editor(), QStringLiteral("a"));
    const QFileDevice::Permissions normal = QFileInfo(path(QStringLiteral("Sub"))).permissions();
    ASSERT_TRUE(QFile::setPermissions(path(QStringLiteral("Sub")), QFileDevice::ReadOwner | QFileDevice::ExeOwner));
    const auto restore = qScopeGuard([&] { QFile::setPermissions(path(QStringLiteral("Sub")), normal); });
    autosave(window);
    flush(window);
    EXPECT_TRUE(window.statusBar()->currentMessage().contains(QLatin1String("Could not save")))
        << window.statusBar()->currentMessage().toStdString();
    EXPECT_TRUE(window.editor()->isModified());
    EXPECT_EQ(readAll(file), QByteArray("# Failing\n"));

    QFile::setPermissions(path(QStringLiteral("Sub")), normal);
    autosave(window);
    flush(window);
    EXPECT_FALSE(window.editor()->isModified());
    EXPECT_TRUE(readAll(file).contains("a"));
}

// close() with a commit in flight keeps the text: the window waits for it.
TEST_F(AutosaveTest, ClosingWithACommitInFlightKeepsTheText)
{
    Gate gate;
    gate.hold = false;
    gate.delayMs = 150;
    MainWindow window;
    window.setSaveJobHookForTests(gate.hook());
    open(window);
    QTest::keyClicks(window.editor(), QStringLiteral("a"));
    const QString typed = window.editor()->noteText();
    autosave(window);
    QTest::keyClicks(window.editor(), QStringLiteral("b")); // and one more the commit did not take
    const QString all = window.editor()->noteText();
    EXPECT_TRUE(window.close());
    EXPECT_EQ(readAll(note()), all.toUtf8());
    EXPECT_NE(all, typed);
}

TEST_F(AutosaveTest, RenameWithACommitInFlightDoesNotBringTheOldNameBack)
{
    Gate gate;
    gate.hold = false;
    gate.delayMs = 150;
    MainWindow window;
    window.setSaveJobHookForTests(gate.hook());
    open(window);
    QTest::keyClicks(window.editor(), QStringLiteral("a"));
    autosave(window);
    bool renamed = false;
    QString error;
    ASSERT_TRUE(QMetaObject::invokeMethod(&window, "renameVaultPath", Q_RETURN_ARG(bool, renamed),
                                          Q_ARG(QString, note()), Q_ARG(bool, false), Q_ARG(QString, QStringLiteral("Renamed")),
                                          Q_ARG(QString *, &error)));
    ASSERT_TRUE(renamed) << error.toStdString();
    flush(window);
    QTest::qWait(300);
    EXPECT_FALSE(QFileInfo::exists(note()));
    EXPECT_TRUE(readAll(path(QStringLiteral("Renamed.md"))).contains("a"));
}

TEST_F(AutosaveTest, DeleteWithACommitInFlightDoesNotBringTheFileBack)
{
    Gate gate;
    gate.hold = false;
    gate.delayMs = 150;
    MainWindow window;
    window.setSaveJobHookForTests(gate.hook());
    open(window);
    QTest::keyClicks(window.editor(), QStringLiteral("a"));
    autosave(window);
    ASSERT_TRUE(QMetaObject::invokeMethod(&window, "deletePath", Q_ARG(QString, note())));
    flush(window);
    QTest::qWait(300);
    EXPECT_FALSE(QFileInfo::exists(note()));
}

#ifdef NOTEAPPC_HAVE_ENCRYPTION
TEST_F(AutosaveTest, EncryptWithACommitInFlightDoesNotBringThePlainFileBack)
{
    Gate gate;
    gate.hold = false;
    gate.delayMs = 150;
    MainWindow window;
    window.setEncryptionPromptForTests([](bool, bool) { return QStringLiteral("correct horse battery"); });
    window.setSaveJobHookForTests(gate.hook());
    open(window);
    QTest::keyClicks(window.editor(), QStringLiteral("a"));
    autosave(window);
    clickNextBox(QStringLiteral("Encrypt"));
    window.findChild<QAction *>(QStringLiteral("encrypted-notes:encrypt"))->trigger();
    flush(window);
    QTest::qWait(300);
    EXPECT_TRUE(QFileInfo::exists(path(QStringLiteral("Note.mdlock"))));
    EXPECT_FALSE(QFileInfo::exists(note())) << "the plain file is back";
}
#endif

// Encrypted, notebook and lossy notes keep the synchronous path: no commit
// reaches the save thread, and the file is already written when the call
// returns.
TEST_F(AutosaveTest, EncryptedNotebookAndLossyNotesStaySynchronous)
{
#ifdef NOTEAPPC_HAVE_ENCRYPTION
    {
        Gate gate;
        gate.hold = false;
        MainWindow window;
        window.setEncryptionPromptForTests([](bool, bool) { return QStringLiteral("correct horse battery"); });
        open(window);
        clickNextBox(QStringLiteral("Encrypt"));
        window.findChild<QAction *>(QStringLiteral("encrypted-notes:encrypt"))->trigger();
        const QString locked = path(QStringLiteral("Note.mdlock"));
        ASSERT_TRUE(QFileInfo::exists(locked));
        window.setSaveJobHookForTests(gate.hook());
        const QByteArray before = readAll(locked);
        window.editor()->setCursorOffset(window.editor()->noteText().size());
        QTest::keyClicks(window.editor(), QStringLiteral("zz"));
        autosave(window);
        EXPECT_EQ(gate.commits.load(), 0);
        EXPECT_NE(readAll(locked), before) << "an encrypted note is sealed before the call returns";
        EXPECT_FALSE(window.editor()->isModified());
    }
#endif
#ifdef NOTEAPPC_HAVE_NOTEBOOK // an .ipynb opens as a note only with the notebook module
    {
        const QByteArray original = readAll(QStringLiteral(NOTEAPPC_FIXTURES_DIR "/notebook/sample.ipynb"));
        writeAll(path(QStringLiteral("Analysis.ipynb")), original);
        Gate gate;
        gate.hold = false;
        MainWindow window;
        window.setSaveJobHookForTests(gate.hook());
        open(window, path(QStringLiteral("Analysis.ipynb")));
        window.editor()->setCursorOffset(window.editor()->noteText().indexOf(QStringLiteral("# Title")) + 2);
        QTest::keyClicks(window.editor(), QStringLiteral("Big "));
        autosave(window);
        EXPECT_EQ(gate.commits.load(), 0);
        EXPECT_TRUE(readAll(path(QStringLiteral("Analysis.ipynb"))).contains("# Big Title"));
    }
#endif
    {
        const QString lossy = path(QStringLiteral("Lossy.md"));
        writeAll(lossy, QByteArray("# Lossy \xff\xfe bytes\n"));
        Gate gate;
        gate.hold = false;
        MainWindow window;
        window.setSaveJobHookForTests(gate.hook());
        open(window, lossy);
        QTest::keyClicks(window.editor(), QStringLiteral("a"));
        autosave(window);
        EXPECT_EQ(gate.commits.load(), 0);
        EXPECT_EQ(readAll(lossy), QByteArray("# Lossy \xff\xfe bytes\n")) << "an autosave never rewrites bytes nobody edited";
        EXPECT_TRUE(window.editor()->isModified());
    }
}

// The app's own commit is not an outside change: no "changed on disk" message,
// no recovery copy of it, once the watcher has had its turn.
TEST_F(AutosaveTest, TheAppsOwnWriteIsNotTakenAsAnExternalChange)
{
    MainWindow window;
    open(window);
    QTest::keyClicks(window.editor(), QStringLiteral("a"));
    autosave(window);
    flush(window);
    window.statusBar()->clearMessage();
    QTest::qWait(500); // the watcher's debounce and its reload
    EXPECT_FALSE(window.statusBar()->currentMessage().contains(QLatin1String("changed on disk")))
        << window.statusBar()->currentMessage().toStdString();
    const auto versions = Snapshots::versions(note());
    ASSERT_EQ(versions.size(), 1) << "only the text the note held before its first save";
    EXPECT_EQ(Snapshots::read(versions.first()), QStringLiteral("# Note\n\nBody.\n"));
    EXPECT_FALSE(window.editor()->isModified());
}

// A writer inside the app (a plugin, the PDF/image/EPUB sidecars) that lands
// while the commit is open is not lost: the commit overwrites it, but its
// version is in file recovery.
TEST_F(AutosaveTest, AnInAppWriteDuringACommitReachesFileRecovery)
{
    MainWindow window;
    Gate gate;
    window.setSaveJobHookForTests(gate.hook());
    open(window);
    QTest::keyClicks(window.editor(), QStringLiteral("a"));
    autosave(window);
    noteAppC::Note other;
    other.text = QStringLiteral("# Written by a plugin\n");
    ASSERT_TRUE(noteAppC::Vault::write(note(), other));
    gate.release();
    flush(window);
    EXPECT_EQ(readAll(note()), window.editor()->noteText().toUtf8());
    bool kept = false;
    for (const auto &version : Snapshots::versions(note()))
        kept = kept || Snapshots::read(version) == other.text;
    EXPECT_TRUE(kept) << "the plugin's version is nowhere";
}

// File recovery keeps a note's history in order: what the note held before its
// first save, then the version found on disk that the window had not seen,
// then (later) the saves. The older text is never the newest.
TEST_F(AutosaveTest, RecoveryCopiesOfOneSaveAreInOrder)
{
    MainWindow window;
    Gate gate;
    gate.hold = false;
    window.setSaveJobHookForTests(gate.hook());
    open(window);
    QTest::keyClicks(window.editor(), QStringLiteral("a"));
    writeAll(note(), "# Changed by sync\n"); // the watcher has not reported it
    autosave(window);
    flush(window);
    const auto versions = Snapshots::versions(note()); // newest first
    ASSERT_GE(versions.size(), 2);
    EXPECT_EQ(Snapshots::read(versions.at(0)), QStringLiteral("# Changed by sync\n"));
    EXPECT_EQ(Snapshots::read(versions.at(1)), QStringLiteral("# Note\n\nBody.\n"));
    EXPECT_GT(versions.at(0).time, versions.at(1).time);
}

// The watcher reports the folder while a commit is held open: the reload does
// not wait for it (the window stays responsive on a slow disk).
TEST_F(AutosaveTest, TheWatcherDoesNotBlockOnACommitInFlight)
{
    MainWindow window;
    Gate gate;
    gate.sem.release(0);
    window.setSaveJobHookForTests(gate.hook());
    open(window);
    QTest::keyClicks(window.editor(), QStringLiteral("a"));
    autosave(window);
    writeAll(path(QStringLiteral("Other.md")), "x\n"); // the watched folder changes: the reload runs in 100 ms
    QElapsedTimer clock;
    clock.start();
    QTest::qWait(400);
    EXPECT_LT(clock.elapsed(), 1500) << "the reload waited for the held commit";
    gate.release();
    flush(window);
    EXPECT_FALSE(window.editor()->isModified());
}

// Losing the window's focus saves what was typed (in the background).
TEST_F(AutosaveTest, LosingFocusSavesWhatWasTyped)
{
    MainWindow window;
    open(window);
    QTest::keyClicks(window.editor(), QStringLiteral("a"));
    ASSERT_FALSE(window.isActiveWindow());
    QEvent change(QEvent::ActivationChange);
    QCoreApplication::sendEvent(&window, &change);
    flush(window);
    EXPECT_EQ(readAll(note()), window.editor()->noteText().toUtf8());
}

/// Runs `step` on the next modal box, then clicks `button` on it.
void onNextBox(std::function<void()> step, const QString &button)
{
    auto poll = std::make_shared<std::function<void()>>();
    *poll = [step = std::move(step), button, poll] {
        auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
        if (!box) {
            QTimer::singleShot(5, *poll);
            return;
        }
        step();
        for (QAbstractButton *candidate : box->buttons()) {
            if (candidate->text() == button)
                candidate->click();
        }
    };
    QTimer::singleShot(0, *poll);
}

// The confirmation box of Encrypt: the window deactivates, and a commit of the
// plain note comes up while the box is open (here the timer's, held on the save
// thread). It must be finished before the plain file and its recovery copies
// go, or it brings the plain text back.
#ifdef NOTEAPPC_HAVE_ENCRYPTION
TEST_F(AutosaveTest, ACommitQueuedWhileTheEncryptBoxIsOpenLeavesNoPlainTextBehind)
{
    Gate gate;
    MainWindow window;
    window.setEncryptionPromptForTests([](bool, bool) { return QStringLiteral("correct horse battery"); });
    window.setSaveJobHookForTests(gate.hook());
    open(window);
    QTest::keyClicks(window.editor(), QStringLiteral("SECRETWORD"));
    onNextBox(
        [&] {
            QEvent change(QEvent::ActivationChange);
            QCoreApplication::sendEvent(&window, &change); // the box took the focus
            autosave(window);                              // and the pause timer fired in it
            std::thread([&gate] {
                QThread::msleep(300);
                gate.release(); // the slow commit finishes after the box is answered
            }).detach();
        },
        QStringLiteral("Encrypt"));
    window.findChild<QAction *>(QStringLiteral("encrypted-notes:encrypt"))->trigger();
    QTest::qWait(600);
    EXPECT_TRUE(QFileInfo::exists(path(QStringLiteral("Note.mdlock"))));
    EXPECT_FALSE(QFileInfo::exists(note())) << "the plain file is back";
    EXPECT_TRUE(Snapshots::versions(note()).isEmpty()) << "a recovery copy of the plain text is back";
    EXPECT_FALSE(readAll(path(QStringLiteral("Note.mdlock"))).contains("SECRETWORD"));
}
#endif

#ifdef NOTEAPPC_HAVE_ENCRYPTION
// Losing focus to a box does not queue a commit behind it: the focus-loss
// autosave waits for the box to be answered.
TEST_F(AutosaveTest, NoFocusLossCommitIsQueuedWhileABoxIsOpen)
{
    Gate gate;
    gate.hold = false;
    MainWindow window;
    window.setEncryptionPromptForTests([](bool, bool) { return QStringLiteral("correct horse battery"); });
    window.setSaveJobHookForTests(gate.hook());
    open(window);
    QTest::keyClicks(window.editor(), QStringLiteral("a"));
    onNextBox(
        [&] {
            QEvent change(QEvent::ActivationChange);
            QCoreApplication::sendEvent(&window, &change);
            EXPECT_EQ(gate.commits.load(), 0) << "a commit was queued behind the open box";
        },
        QStringLiteral("Cancel"));
    window.findChild<QAction *>(QStringLiteral("encrypted-notes:encrypt"))->trigger();
    flush(window);
    EXPECT_EQ(gate.commits.load(), 0);
}
#endif

// Discard in the could-not-save box means no write afterwards, even though the
// box deactivated the window and the folder has since become writable.
TEST_F(AutosaveTest, DiscardInTheCouldNotSaveBoxWritesNothingAfterwards)
{
    if (geteuid() == 0)
        GTEST_SKIP() << "root writes anywhere";
    const QString file = path(QStringLiteral("Sub/Discard.md"));
    writeAll(file, "# Discard\n");
    MainWindow window;
    open(window, file);
    QTest::keyClicks(window.editor(), QStringLiteral("a"));
    const QFileDevice::Permissions normal = QFileInfo(path(QStringLiteral("Sub"))).permissions();
    ASSERT_TRUE(QFile::setPermissions(path(QStringLiteral("Sub")), QFileDevice::ReadOwner | QFileDevice::ExeOwner));
    const auto restore = qScopeGuard([&] { QFile::setPermissions(path(QStringLiteral("Sub")), normal); });
    onNextBox(
        [&] {
            QFile::setPermissions(path(QStringLiteral("Sub")), normal); // a retry would now succeed
            QEvent change(QEvent::ActivationChange);
            QCoreApplication::sendEvent(&window, &change);
        },
        QStringLiteral("Discard"));
    EXPECT_TRUE(window.close());
    flush(window);
    QTest::qWait(300);
    EXPECT_EQ(readAll(file), QByteArray("# Discard\n")) << "a discarded note was written";
}

// Mixed line endings go through the background path as they do through Ctrl+S.
TEST_F(AutosaveTest, MixedLineEndingsRoundTripThroughTheBackgroundPath)
{
    const QByteArray mixed("# Mixed\r\nline two\nline three\r\n\r\nend\n");
    writeAll(path(QStringLiteral("A.md")), mixed);
    writeAll(path(QStringLiteral("B.md")), mixed);
    {
        MainWindow window;
        open(window, path(QStringLiteral("A.md")));
        window.editor()->setCursorOffset(5);
        QTest::keyClicks(window.editor(), QStringLiteral("zz"));
        autosave(window);
        flush(window);
    }
    {
        MainWindow window;
        open(window, path(QStringLiteral("B.md")));
        window.editor()->setCursorOffset(5);
        QTest::keyClicks(window.editor(), QStringLiteral("zz"));
        bool saved = false;
        QMetaObject::invokeMethod(&window, "save", Qt::DirectConnection, Q_RETURN_ARG(bool, saved));
        ASSERT_TRUE(saved);
    }
    EXPECT_EQ(readAll(path(QStringLiteral("A.md"))), readAll(path(QStringLiteral("B.md"))));
    EXPECT_TRUE(readAll(path(QStringLiteral("A.md"))).contains("\r\n"));
}

// An explicit save right after an autosave sees the file the autosave wrote.
TEST_F(AutosaveTest, AnExplicitSaveAfterAnAutosaveIsConsistent)
{
    MainWindow window;
    open(window);
    QTest::keyClicks(window.editor(), QStringLiteral("a"));
    autosave(window);
    QTest::keyClicks(window.editor(), QStringLiteral("b"));
    bool saved = false;
    QMetaObject::invokeMethod(&window, "save", Qt::DirectConnection, Q_RETURN_ARG(bool, saved));
    EXPECT_TRUE(saved);
    EXPECT_EQ(readAll(note()), window.editor()->noteText().toUtf8());
    EXPECT_FALSE(window.editor()->isModified());
}

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QTemporaryDir configHome;
    QTemporaryDir dataHome;
    QTemporaryDir cacheHome;
    qputenv("XDG_CONFIG_HOME", configHome.path().toUtf8());
    qputenv("XDG_DATA_HOME", dataHome.path().toUtf8());
    qputenv("XDG_CACHE_HOME", cacheHome.path().toUtf8());
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("noteAppC-test"));
    QCoreApplication::setApplicationName(QStringLiteral("noteAppC-test"));
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
