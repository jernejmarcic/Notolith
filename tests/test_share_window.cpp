#include <atomic>
#include <functional>
#include <memory>
#include <tuple>

#include <gtest/gtest.h>

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QInputDialog>
#include <QMessageBox>
#include <QSaveFile>
#include <QStatusBar>
#include <QClipboard>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTabBar>
#include <QTreeWidget>
#include <QTemporaryDir>
#include <QTimer>
#include <QTreeView>
#include <QFileSystemModel>
#include <QUuid>
#include <QtTest/QTest>

#include "MainWindow.h"
#include "MarkdownEditor.h"
#include "SecretStore.h"
#include "Settings.h"
#include "ShareDialog.h"
#include "ShareEngine.h"
#include "ShareFrame.h"
#include "ShareSession.h"

// Sharing through the window (docs/SHARE.md "In the window"): two MainWindows on
// two temporary vaults in one process, loopback only, mDNS off, port 0, an
// in-memory secret store (never the real keyring), XDG directories in temporary
// folders. QTest::qWaitFor + ASSERT: QTRY_* returns silently under gtest.

using noteAppC::MainWindow;
using noteAppC::MarkdownEditor;
using noteAppC::Settings;
using noteAppC::CrdtDoc;
using noteAppC::JoinShareDialog;
using noteAppC::ShareDialog;
using noteAppC::ShareEngine;
using noteAppC::ShareSession;

namespace {

QHash<QString, QByteArray> &memoryStore()
{
    static QHash<QString, QByteArray> store;
    return store;
}

bool waitFor(const std::function<bool()> &pred, int ms = 15000)
{
    return QTest::qWaitFor(pred, ms);
}

QString readFile(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll()) : QString();
}

/// How another program replaces a file: a temporary one renamed over it.
void replaceFile(const QString &path, const QString &text)
{
    QSaveFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.write(text.toUtf8());
    ASSERT_TRUE(file.commit());
}

void setShareFeature(bool on)
{
    Settings settings = Settings::load();
    settings.setValue(Settings::featureKey("share"), on);
    ASSERT_TRUE(settings.save());
}

/// Set by the fixture: every share in a test runs on this engine.
CrdtDoc::Backend backendUnderTest = CrdtDoc::Backend::Yrs;

/// One person: a vault, a store, a device identity, and a window on them that
/// can be closed and opened again.
struct Side {
    QTemporaryDir vault, store;
    QString identity;
    std::atomic<bool> hold{false}; ///< while true, this side's outgoing edits are queued
    std::unique_ptr<MainWindow> window;
    int approvals = 0;

    explicit Side(const QString &key)
        : identity(key)
    {
        open();
    }

    void open()
    {
        window = std::make_unique<MainWindow>();
        window->setAutosaveIntervalsForTests(50, 200);
        ShareEngine::Config config;
        config.port = 0;
        config.mDNS = false;
        config.identityKey = identity;
        config.hosts = {QStringLiteral("127.0.0.1")};
        config.storeDirectory = store.path();
        config.resyncMs = 300;
        config.settleMs = 100;
        config.backend = backendUnderTest;
        config.holdFramesForTests = [this](const QByteArray &) { return hold.load(); };
        window->setShareConfigForTests(config);
        window->setShareApprovalForTests([this](const QString &, const QString &) {
            ++approvals;
            return true;
        });
        window->openTarget(noteAppC::resolveLaunchTarget(vault.path()));
    }

    QString path(const QString &relative) const { return vault.filePath(relative); }
    void write(const QString &relative, const QString &text) const
    {
        QDir().mkpath(QFileInfo(path(relative)).absolutePath());
        QFile file(path(relative));
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        file.write(text.toUtf8());
    }
    QString read(const QString &relative) const { return readFile(path(relative)); }
    MarkdownEditor *editor() const { return window->editor(); }
    QString text() const { return editor()->noteText(); }
    ShareEngine *engine() const { return window->shareEngine(); }
    QAction *command(const char *id) const { return window->findChild<QAction *>(QString::fromLatin1(id)); }

    void typeAt(qsizetype offset, const QString &text) const
    {
        editor()->setCursorOffset(offset);
        editor()->insertAtCaret(text);
    }
    void undo() const { QTest::keyClick(editor(), Qt::Key_Z, Qt::ControlModifier); }
    /// Every editor showing `text`, the active one first.
    QList<MarkdownEditor *> editorsShowing(const QString &note) const
    {
        QList<MarkdownEditor *> out{editor()};
        for (MarkdownEditor *other : window->findChildren<MarkdownEditor *>()) {
            if (other != editor() && other->isEnabled() && other->noteText() == note)
                out.append(other);
        }
        return out;
    }
};

/// `owner` shares `name` (holding `text`), `joiner` joins; returns the share id once
/// the joiner shows the note and both are connected. Autosave is off unless asked
/// for: a save also sends what the shared text lacks (the safety net), which would
/// hide a live edit that never went out.
QByteArray share(Side &owner, Side &joiner, const QString &name, const QString &text, bool autosave = false)
{
    owner.write(name, text);
    if (!owner.window->openNote(owner.path(name))) {
        ADD_FAILURE() << "could not open the note";
        return {};
    }
    QString error;
    const QString code = owner.window->shareNote(&error);
    if (code.isEmpty()) {
        ADD_FAILURE() << error.toStdString();
        return {};
    }
    const auto invite = ShareEngine::parseInvite(code, &error);
    if (!invite || invite->backend != (backendUnderTest == CrdtDoc::Backend::Yrs ? noteAppC::ShareFrame::kBackendYrs : noteAppC::ShareFrame::kBackendNative)) {
        ADD_FAILURE() << "the share is not on the engine under test";
        return {};
    }
    if (!joiner.window->joinShare(code, &error)) {
        ADD_FAILURE() << error.toStdString();
        return {};
    }
    const QByteArray id = owner.engine()->shares().value(0).shareID;
    const bool ready = waitFor([&] {
        return joiner.engine()->doc(id) && joiner.text() == text && owner.engine()->peersHere(id) == 1
               && joiner.engine()->peersHere(id) == 1;
    });
    if (!ready) {
        ADD_FAILURE() << "the note did not reach the joiner";
        return {};
    }
    if (!autosave) {
        owner.window->setAutosaveIntervalsForTests(600000, 600000);
        joiner.window->setAutosaveIntervalsForTests(600000, 600000);
    }
    return id;
}

class ShareWindowTest : public ::testing::TestWithParam<CrdtDoc::Backend> {
protected:
    void SetUp() override
    {
        if (!CrdtDoc::available(GetParam()))
            GTEST_SKIP() << "this build has no such CRDT engine";
        backendUnderTest = GetParam();
        setShareFeature(true);
    }
    void TearDown() override
    {
        // A test that turns these on, failing halfway, must not leave them on for the next.
        Settings settings = Settings::load();
        settings.setValue(QStringLiteral("notebooks.allowHostTier"), false);
        settings.save();
    }
};

INSTANTIATE_TEST_SUITE_P(Engines, ShareWindowTest,
                         ::testing::Values(CrdtDoc::Backend::Yrs, CrdtDoc::Backend::Native),
                         [](const auto &info) {
                             return std::string(info.param == CrdtDoc::Backend::Yrs ? "Yrs" : "Native");
                         });

} // namespace

TEST(ShareFileName, IsNeverAPathAndNeverEmpty)
{
    const QByteArray id = QByteArray::fromHex("0123456789abcdef0123456789abcdef");
    EXPECT_EQ(ShareSession::fileNameFor(QStringLiteral("My note"), id), QStringLiteral("My note"));
    EXPECT_EQ(ShareSession::fileNameFor(QStringLiteral("../x"), id), QStringLiteral("x"));
    EXPECT_EQ(ShareSession::fileNameFor(QStringLiteral("a/b\\c"), id), QStringLiteral("abc"));
    EXPECT_EQ(ShareSession::fileNameFor(QStringLiteral("....//..\\.."), id), QStringLiteral("01234567"));
    EXPECT_EQ(ShareSession::fileNameFor(QStringLiteral(".hidden"), id), QStringLiteral("hidden"));
    EXPECT_EQ(ShareSession::fileNameFor(QStringLiteral("a\nb\x01\x7f"), id), QStringLiteral("ab"));
    EXPECT_EQ(ShareSession::fileNameFor(QString(), id), QStringLiteral("01234567"));
    EXPECT_EQ(ShareSession::fileNameFor(QString(300, u'x'), id).size(), 100);
    // a cut never leaves half of a pair
    const QString emoji = QString(99, u'x') + QString::fromUcs4(U"\U0001F600", 1);
    EXPECT_EQ(ShareSession::fileNameFor(emoji, id), QString(99, u'x'));
}

TEST_P(ShareWindowTest, TypingInOneWindowAppearsInTheOther)
{
    Side a(QStringLiteral("a")), b(QStringLiteral("b"));
    const QByteArray id = share(a, b, QStringLiteral("Note.md"), QStringLiteral("hello\n"));
    ASSERT_FALSE(id.isEmpty());
    EXPECT_EQ(a.approvals, 1);

    a.typeAt(5, QStringLiteral(" world"));
    ASSERT_TRUE(waitFor([&] { return b.text() == QStringLiteral("hello world\n"); }));
    b.typeAt(0, QStringLiteral(">> "));
    ASSERT_TRUE(waitFor([&] { return a.text() == QStringLiteral(">> hello world\n"); }));
    EXPECT_EQ(a.text(), b.text());
}

TEST_P(ShareWindowTest, RemoteEditsReachTheDisk)
{
    Side a(QStringLiteral("a")), b(QStringLiteral("b"));
    const QByteArray id = share(a, b, QStringLiteral("Note.md"), QStringLiteral("hello\n"), true);
    ASSERT_FALSE(id.isEmpty());
    ASSERT_TRUE(QFileInfo::exists(b.path(QStringLiteral("Shared/Note.md"))));

    a.typeAt(5, QStringLiteral(" there"));
    // B never types: its file catches up on its own, within the autosave interval.
    ASSERT_TRUE(waitFor([&] { return b.read(QStringLiteral("Shared/Note.md")) == QStringLiteral("hello there\n"); }, 5000));
    EXPECT_FALSE(b.editor()->isModified());
}

TEST_P(ShareWindowTest, TheJoinerGetsACopyUnderShared)
{
    Side a(QStringLiteral("a")), b(QStringLiteral("b"));
    const QByteArray id = share(a, b, QStringLiteral("Note.md"), QStringLiteral("first\n"), true);
    ASSERT_FALSE(id.isEmpty());
    EXPECT_EQ(b.read(QStringLiteral("Shared/Note.md")), QStringLiteral("first\n"));
    EXPECT_EQ(b.text(), QStringLiteral("first\n")) << "opened in the active tab";

    // The title is the owner's string: a hostile one stays inside Shared/.
    QString error;
    const auto hostile = a.engine()->create(QStringLiteral("Other.md"), QStringLiteral("../x"), QStringLiteral("second\n"), &error);
    ASSERT_TRUE(hostile) << error.toStdString();
    ASSERT_TRUE(b.window->joinShare(hostile->inviteCode, &error));
    ASSERT_TRUE(waitFor([&] { return QFileInfo::exists(b.path(QStringLiteral("Shared/x.md"))); }));
    EXPECT_EQ(b.read(QStringLiteral("Shared/x.md")), QStringLiteral("second\n"));
    EXPECT_FALSE(QFileInfo::exists(b.path(QStringLiteral("x.md"))));
    EXPECT_FALSE(QFileInfo::exists(QDir(b.vault.path()).absoluteFilePath(QStringLiteral("../x.md"))));

    // A second note with the same title does not replace the first.
    const auto same = a.engine()->create(QStringLiteral("Third.md"), QStringLiteral("Note"), QStringLiteral("third\n"), &error);
    ASSERT_TRUE(same) << error.toStdString();
    ASSERT_TRUE(b.window->joinShare(same->inviteCode, &error));
    ASSERT_TRUE(waitFor([&] { return QFileInfo::exists(b.path(QStringLiteral("Shared/Note (2).md"))); }));
    EXPECT_EQ(b.read(QStringLiteral("Shared/Note.md")), QStringLiteral("first\n"));
    EXPECT_EQ(b.read(QStringLiteral("Shared/Note (2).md")), QStringLiteral("third\n"));
}

TEST_P(ShareWindowTest, UndoSurvivesARemoteEditElsewhere)
{
    Side a(QStringLiteral("a")), b(QStringLiteral("b"));
    const QByteArray id = share(a, b, QStringLiteral("Note.md"), QStringLiteral("alpha beta\n"));
    ASSERT_FALSE(id.isEmpty());

    a.typeAt(10, QStringLiteral("abc")); // one burst: one undo step
    ASSERT_TRUE(waitFor([&] { return b.text() == QStringLiteral("alpha betaabc\n"); }));
    b.typeAt(b.text().size(), QStringLiteral("Z"));
    ASSERT_TRUE(waitFor([&] { return a.text() == QStringLiteral("alpha betaabc\nZ"); }));

    a.undo();
    EXPECT_EQ(a.text(), QStringLiteral("alpha beta\nZ")) << "only A's own typing is undone";
    ASSERT_TRUE(waitFor([&] { return b.text() == QStringLiteral("alpha beta\nZ"); }));
}

TEST_P(ShareWindowTest, UndoOverThereIsSentAsADiffNotAWholeBufferOp)
{
    Side a(QStringLiteral("a")), b(QStringLiteral("b"));
    const QByteArray id = share(a, b, QStringLiteral("Note.md"), QStringLiteral("alpha beta gamma\n"));
    ASSERT_FALSE(id.isEmpty());

    a.typeAt(16, QStringLiteral("!!!"));
    ASSERT_TRUE(waitFor([&] { return b.text() == QStringLiteral("alpha beta gamma!!!\n"); }));
    b.hold = true; // B's next edit is "in flight"
    b.typeAt(5, QStringLiteral("Z"));
    a.undo(); // a whole-text change in the editor: a diff on the wire
    ASSERT_TRUE(waitFor([&] { return b.text() == QStringLiteral("alphaZ beta gamma\n"); }));
    b.hold = false;
    b.engine()->releaseHeldFramesForTests();
    ASSERT_TRUE(waitFor([&] { return a.text() == b.text(); }));
    EXPECT_EQ(a.text(), QStringLiteral("alphaZ beta gamma\n")) << "B's edit stays where it was made";
}

TEST_P(ShareWindowTest, ADiskChangeUnderASharedNoteBecomesOps)
{
    Side a(QStringLiteral("a")), b(QStringLiteral("b"));
    const QByteArray id = share(a, b, QStringLiteral("Note.md"), QStringLiteral("top\nmiddle\n"));
    ASSERT_FALSE(id.isEmpty());
    a.window->setAutosaveIntervalsForTests(60000, 60000); // B's edit stays unsaved over here

    b.typeAt(0, QStringLiteral("B: "));
    ASSERT_TRUE(waitFor([&] { return a.text() == QStringLiteral("B: top\nmiddle\n"); }));
    ASSERT_TRUE(a.editor()->isModified());
    replaceFile(a.path(QStringLiteral("Note.md")), QStringLiteral("top\nmiddle\nappended line\n"));

    const QString both = QStringLiteral("B: top\nmiddle\nappended line\n");
    ASSERT_TRUE(waitFor([&] { return a.text() == both && b.text() == both; }));
}

TEST_P(ShareWindowTest, OverlappingDiskChangeKeepsWhatIsTyped)
{
    Side a(QStringLiteral("a")), b(QStringLiteral("b"));
    const QByteArray id = share(a, b, QStringLiteral("Note.md"), QStringLiteral("alpha beta gamma\n"));
    ASSERT_FALSE(id.isEmpty());
    a.window->setAutosaveIntervalsForTests(60000, 60000);

    a.typeAt(8, QStringLiteral("!!")); // "alpha be!!ta gamma"
    ASSERT_TRUE(waitFor([&] { return b.text() == QStringLiteral("alpha be!!ta gamma\n"); }));
    replaceFile(a.path(QStringLiteral("Note.md")), QStringLiteral("alpha BETA gamma\n")); // across the typed spot
    ASSERT_TRUE(waitFor([&] { return a.window->statusBar()->currentMessage().contains(QStringLiteral("changed on disk")); }));
    EXPECT_EQ(a.text(), QStringLiteral("alpha be!!ta gamma\n")) << "what is typed wins, as before";
    QTest::qWait(300);
    EXPECT_EQ(b.text(), QStringLiteral("alpha be!!ta gamma\n"));
}

TEST_P(ShareWindowTest, OpeningASharedNoteInASecondTabKeepsOneDocument)
{
    Side a(QStringLiteral("a")), b(QStringLiteral("b"));
    const QByteArray id = share(a, b, QStringLiteral("Note.md"), QStringLiteral("hello\n"));
    ASSERT_FALSE(id.isEmpty());
    int batches = 0;
    QObject::connect(b.engine(), &ShareEngine::remoteEdits, b.engine(), [&batches] { ++batches; });

    MarkdownEditor *first = a.editor();
    a.command("workspace:new-tab")->trigger();
    ASSERT_TRUE(a.window->openNote(a.path(QStringLiteral("Note.md"))));
    MarkdownEditor *second = a.editor();
    ASSERT_NE(first, second);
    ASSERT_EQ(second->noteText(), QStringLiteral("hello\n"));

    a.typeAt(0, QStringLiteral("x"));
    ASSERT_TRUE(waitFor([&] { return b.text() == QStringLiteral("xhello\n"); }));
    QTest::qWait(300);
    EXPECT_EQ(batches, 1) << "one edit, sent once";
    EXPECT_EQ(first->noteText(), QStringLiteral("xhello\n"));
    EXPECT_EQ(second->noteText(), QStringLiteral("xhello\n"));
    // another round after the tabs were moved around once
    a.typeAt(1, QStringLiteral("y"));
    ASSERT_TRUE(waitFor([&] { return b.text() == QStringLiteral("xyhello\n"); }));
    QTest::qWait(300);
    EXPECT_EQ(batches, 2);
    EXPECT_EQ(first->noteText(), QStringLiteral("xyhello\n"));

    // B's edit reaches both of A's tabs
    b.typeAt(0, QStringLiteral("B"));
    ASSERT_TRUE(waitFor([&] { return first->noteText() == QStringLiteral("Bxyhello\n") && second->noteText() == first->noteText(); }));
}

TEST_P(ShareWindowTest, ClosingTheTabKeepsTheSessionAlive)
{
    Side a(QStringLiteral("a")), b(QStringLiteral("b"));
    const QByteArray id = share(a, b, QStringLiteral("Note.md"), QStringLiteral("hello\n"), true);
    ASSERT_FALSE(id.isEmpty());

    a.command("workspace:close")->trigger();
    ASSERT_TRUE(a.editor()->noteText().isEmpty());
    b.typeAt(5, QStringLiteral(" there"));
    ASSERT_TRUE(waitFor([&] { return a.engine()->doc(id)->text() == QStringLiteral("hello there\n"); }));
    EXPECT_EQ(a.read(QStringLiteral("Note.md")), QStringLiteral("hello\n")) << "nothing writes the file while it is closed";

    ASSERT_TRUE(a.window->openNote(a.path(QStringLiteral("Note.md"))));
    EXPECT_EQ(a.text(), QStringLiteral("hello there\n")) << "the shared text, not the stale file";
    ASSERT_TRUE(waitFor([&] { return a.read(QStringLiteral("Note.md")) == QStringLiteral("hello there\n"); }, 5000));
}

TEST_P(ShareWindowTest, AnEditMadeWhileTheAppWasClosedFoldsInOnReopen)
{
    Side a(QStringLiteral("a")), b(QStringLiteral("b"));
    const QByteArray id = share(a, b, QStringLiteral("Note.md"), QStringLiteral("top\nmiddle\n"), true);
    ASSERT_FALSE(id.isEmpty());
    a.window.reset();

    a.write(QStringLiteral("Note.md"), QStringLiteral("top\nmiddle\nappended line\n")); // another program, while closed
    b.typeAt(0, QStringLiteral("B: "));
    a.open();
    ASSERT_TRUE(a.engine()->isRunning()) << "the vault holds a share";
    a.engine()->dial(QStringLiteral("127.0.0.1"), b.engine()->port());
    ASSERT_TRUE(a.window->openNote(a.path(QStringLiteral("Note.md"))));

    const QString both = QStringLiteral("B: top\nmiddle\nappended line\n");
    ASSERT_TRUE(waitFor([&] { return a.text() == both && b.text() == both; }));
    ASSERT_TRUE(waitFor([&] { return a.read(QStringLiteral("Note.md")) == both; }, 5000));
    QFile base(a.store.filePath(QString::fromLatin1(id.toHex()) + QStringLiteral("/base.md")));
    ASSERT_TRUE(waitFor([&] { return base.exists() && readFile(base.fileName()) == both; }, 5000))
        << "base.md follows the save";
}

TEST_P(ShareWindowTest, ASharedVaultResumesOnReopen)
{
    Side a(QStringLiteral("a")), b(QStringLiteral("b"));
    const QByteArray id = share(a, b, QStringLiteral("Note.md"), QStringLiteral("hello\n"));
    ASSERT_FALSE(id.isEmpty());
    a.window.reset();
    a.open();
    ASSERT_NE(a.engine(), nullptr);
    EXPECT_TRUE(a.engine()->isRunning());
    EXPECT_EQ(a.engine()->shares().size(), 1);
    ASSERT_TRUE(a.window->openNote(a.path(QStringLiteral("Note.md"))));
    a.engine()->dial(QStringLiteral("127.0.0.1"), b.engine()->port());
    ASSERT_TRUE(waitFor([&] { return a.engine()->peersHere(id) == 1 && b.engine()->peersHere(id) == 1; }));

    b.typeAt(5, QStringLiteral(" again"));
    ASSERT_TRUE(waitFor([&] { return a.text() == QStringLiteral("hello again\n"); })) << "the session is attached on open";
    a.typeAt(0, QStringLiteral("A "));
    ASSERT_TRUE(waitFor([&] { return b.text() == QStringLiteral("A hello again\n"); }));
}

TEST_P(ShareWindowTest, AJoinerFindsTheOwnerAgainWithoutMDNS)
{
    Side a(QStringLiteral("a")), b(QStringLiteral("b"));
    const QByteArray id = share(a, b, QStringLiteral("Note.md"), QStringLiteral("hello\n"));
    ASSERT_FALSE(id.isEmpty());
    b.window.reset();
    ASSERT_TRUE(waitFor([&] { return a.engine()->peersHere(id) == 0; }));
    b.open(); // mDNS is off and nobody dials: only the invite's addresses bring it back
    ASSERT_NE(b.engine(), nullptr);
    ASSERT_TRUE(waitFor([&] { return a.engine()->peersHere(id) == 1 && b.engine()->peersHere(id) == 1; }));
}

TEST_P(ShareWindowTest, ASharedNotesBlockRunsOnlyOnceAllowed)
{
    Side a(QStringLiteral("a")), b(QStringLiteral("b"));
    const QString marker = QUuid::createUuid().toString(QUuid::Id128); // no cached output from an earlier run
    const QString note = QStringLiteral("```python\nprint('ran')  # %1\n```\n\nend\n").arg(marker);
    ASSERT_FALSE(share(a, b, QStringLiteral("Code.md"), note).isEmpty());
    int asked = 0;
    bool allow = false;
    QString named;
    b.window->setSharedRunApprovalForTests([&](const QString &members) {
        ++asked;
        named = members;
        return allow;
    });
    b.window->show(); // blocks are laid out as they are painted
    MarkdownEditor *editor = b.editor();
    editor->setCursorOffset(editor->noteText().size()); // off the block: it renders
    QTest::qWait(100);
    const qsizetype block = editor->noteText().indexOf(QStringLiteral("print"));
    ASSERT_TRUE(editor->notebookInfoAt(block).eligible);

    ASSERT_TRUE(editor->runNotebookBlockAt(block));
    ASSERT_TRUE(waitFor([&] { return editor->notebookInfoAt(block).hasRun; }));
    EXPECT_EQ(asked, 1);
    EXPECT_FALSE(named.isEmpty()) << "the owner is named";
    EXPECT_FALSE(editor->notebookInfoAt(block).ok);
    EXPECT_TRUE(editor->notebookOutputTextAt(block).join(QLatin1Char(' ')).contains(QStringLiteral("Not run")));

    allow = true;
    ASSERT_TRUE(editor->runNotebookBlockAt(block));
    ASSERT_TRUE(waitFor([&] { return asked == 2 && !editor->notebookInfoAt(block).running; }, 30000));
    ASSERT_TRUE(editor->runNotebookBlockAt(block));
    ASSERT_TRUE(waitFor([&] { return !editor->notebookInfoAt(block).running; }, 30000));
    EXPECT_EQ(asked, 2) << "an allowed block does not ask again";
}

TEST_P(ShareWindowTest, ASharedNotesBlockNeverLeavesTheSandbox)
{
    Settings settings = Settings::load();
    settings.setValue(QStringLiteral("notebooks.allowHostTier"), true);
    ASSERT_TRUE(settings.save());
    Side a(QStringLiteral("a")), b(QStringLiteral("b"));
    const QString marker = QUuid::createUuid().toString(QUuid::Id128);
    const QString note = QStringLiteral("```python\n#| sandbox: host\nprint('ran')  # %1\n```\n\nend\n").arg(marker);
    ASSERT_FALSE(share(a, b, QStringLiteral("Host.md"), note).isEmpty());
    b.window->setSharedRunApprovalForTests([](const QString &) { return true; });
    b.window->show();
    MarkdownEditor *editor = b.editor();
    editor->setCursorOffset(editor->noteText().size());
    QTest::qWait(100);
    const qsizetype block = editor->noteText().indexOf(QStringLiteral("print"));
    ASSERT_TRUE(editor->notebookInfoAt(block).eligible);
    ASSERT_TRUE(editor->runNotebookBlockAt(block));
    EXPECT_TRUE(b.window->statusBar()->currentMessage().contains(QStringLiteral("shared note")))
        << b.window->statusBar()->currentMessage().toStdString();
    ASSERT_TRUE(waitFor([&] { return !editor->notebookInfoAt(block).running; }, 30000));
}

TEST_P(ShareWindowTest, ASharedNoteStaysGuardedWhileSharingIsOff)
{
    Settings settings = Settings::load();
    settings.setValue(QStringLiteral("notebooks.allowHostTier"), true);
    ASSERT_TRUE(settings.save());
    Side a(QStringLiteral("a")), b(QStringLiteral("b"));
    const QString marker = QUuid::createUuid().toString(QUuid::Id128);
    const QString note = QStringLiteral("```python\n#| sandbox: host\nprint('ran')  # %1\n```\n\nend\n").arg(marker);
    ASSERT_FALSE(share(a, b, QStringLiteral("Off.md"), note, /*autosave=*/true).isEmpty());
    ASSERT_TRUE(waitFor([&] { return b.read(QStringLiteral("Shared/Off.md")) == note; }));
    b.window.reset(); // the record is written on the way out
    setShareFeature(false);
    b.open();
    ASSERT_EQ(b.engine(), nullptr) << "sharing is off";
    ASSERT_TRUE(b.window->openNote(b.path(QStringLiteral("Shared/Off.md"))));
    int asked = 0;
    b.window->setSharedRunApprovalForTests([&](const QString &) {
        ++asked;
        return true;
    });
    b.window->show();
    MarkdownEditor *editor = b.editor();
    editor->setCursorOffset(editor->noteText().size());
    QTest::qWait(100);
    const qsizetype block = editor->noteText().indexOf(QStringLiteral("print"));
    ASSERT_TRUE(editor->notebookInfoAt(block).eligible);
    ASSERT_TRUE(editor->runNotebookBlockAt(block));
    EXPECT_EQ(asked, 1) << "the store still says whose text this is";
    EXPECT_TRUE(b.window->statusBar()->currentMessage().contains(QStringLiteral("shared note")))
        << b.window->statusBar()->currentMessage().toStdString();
    ASSERT_TRUE(waitFor([&] { return !editor->notebookInfoAt(block).running; }, 30000));
}

TEST_P(ShareWindowTest, NothingIsConstructedWhileTheFeatureIsOff)
{
    setShareFeature(false);
    {
        Side a(QStringLiteral("a"));
        a.write(QStringLiteral("Note.md"), QStringLiteral("hello\n"));
        ASSERT_TRUE(a.window->openNote(a.path(QStringLiteral("Note.md"))));
        EXPECT_EQ(a.engine(), nullptr);
        ASSERT_NE(a.command("share:share-note"), nullptr);
        EXPECT_FALSE(a.command("share:share-note")->isEnabled());
        a.command("share:share-note")->trigger();
        a.command("share:join")->trigger();
        EXPECT_EQ(a.engine(), nullptr) << "the commands do nothing";
        QString error;
        EXPECT_TRUE(a.window->shareNote(&error).isEmpty());
        EXPECT_FALSE(error.isEmpty());
    }
    setShareFeature(true);
    Side on(QStringLiteral("a"));
    on.write(QStringLiteral("Note.md"), QStringLiteral("hello\n"));
    ASSERT_TRUE(on.window->openNote(on.path(QStringLiteral("Note.md"))));
    ASSERT_NE(on.engine(), nullptr);
    EXPECT_FALSE(on.engine()->isRunning()) << "no keys read, no socket, until a note is shared";
    EXPECT_EQ(on.engine()->port(), 0);
}

#ifdef NOTEAPPC_HAVE_ENCRYPTION
TEST_P(ShareWindowTest, AnEncryptedNoteCannotBeShared)
{
    Side a(QStringLiteral("a"));
    a.write(QStringLiteral("Note.md"), QStringLiteral("secret\n"));
    a.window->setEncryptionPromptForTests([](bool, bool) { return QStringLiteral("a long password"); });
    ASSERT_TRUE(a.window->openNote(a.path(QStringLiteral("Note.md"))));
    QTimer::singleShot(0, [] {
        if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
            for (QAbstractButton *button : box->buttons()) {
                if (button->text() == QStringLiteral("Encrypt"))
                    button->click();
            }
        }
    });
    a.command("encrypted-notes:encrypt")->trigger();
    ASSERT_TRUE(QFileInfo::exists(a.path(QStringLiteral("Note.mdlock"))));

    QString error;
    EXPECT_TRUE(a.window->shareNote(&error).isEmpty());
    EXPECT_TRUE(error.contains(QStringLiteral("encrypted"))) << error.toStdString();
    EXPECT_FALSE(a.engine()->hasStoredShares());
    EXPECT_FALSE(a.engine()->isRunning());
}
#endif

TEST_P(ShareWindowTest, ARenamedSharedNoteKeepsSharing)
{
    Side a(QStringLiteral("a")), b(QStringLiteral("b"));
    const QByteArray id = share(a, b, QStringLiteral("Note.md"), QStringLiteral("hello\n"));
    ASSERT_FALSE(id.isEmpty());

    auto *tree = a.window->findChild<QTreeView *>(QStringLiteral("fileExplorer"));
    ASSERT_NE(tree, nullptr);
    auto *model = qobject_cast<QFileSystemModel *>(tree->model());
    ASSERT_NE(model, nullptr);
    const QModelIndex index = model->index(a.path(QStringLiteral("Note.md")));
    ASSERT_TRUE(index.isValid());
    QTimer::singleShot(0, a.window.get(), [] {
        auto *dialog = qobject_cast<QInputDialog *>(QApplication::activeModalWidget());
        if (!dialog)
            return;
        dialog->setTextValue(QStringLiteral("Renamed"));
        dialog->accept();
    });
    ASSERT_TRUE(QMetaObject::invokeMethod(a.window.get(), "renamePathAt", Q_ARG(QModelIndex, index)));
    ASSERT_TRUE(QFileInfo::exists(a.path(QStringLiteral("Renamed.md"))));
    EXPECT_EQ(a.engine()->shares().value(0).relativePath, QStringLiteral("Renamed.md"));

    a.typeAt(5, QStringLiteral(" renamed"));
    ASSERT_TRUE(waitFor([&] { return b.text() == QStringLiteral("hello renamed\n"); }));
    b.typeAt(0, QStringLiteral("B "));
    ASSERT_TRUE(waitFor([&] { return a.text() == QStringLiteral("B hello renamed\n"); }));
}

namespace {

QLabel *shareBadge(const Side &side)
{
    return side.window->findChild<QLabel *>(QStringLiteral("shareStatus"));
}

bool tabMarked(const Side &side, const QString &name)
{
    for (const QTabBar *bar : side.window->findChildren<QTabBar *>())
        for (int i = 0; i < bar->count(); ++i)
            if (bar->tabText(i).startsWith(name) && bar->tabText(i).contains(QChar(0x21C4)))
                return true;
    return false;
}

QTreeWidgetItem *memberRow(ShareDialog *dialog, const QByteArray &key)
{
    auto *members = dialog->findChild<QTreeWidget *>(QStringLiteral("shareMembers"));
    for (int i = 0; members && i < members->topLevelItemCount(); ++i)
        if (members->topLevelItem(i)->data(0, Qt::UserRole).toByteArray() == key)
            return members->topLevelItem(i);
    return nullptr;
}

} // namespace

TEST_P(ShareWindowTest, TheShareDialogGivesTheCodeAndTheJoinDialogTakesIt)
{
    Side a(QStringLiteral("a")), b(QStringLiteral("b"));
    a.write(QStringLiteral("Note.md"), QStringLiteral("hello\n"));
    ASSERT_TRUE(a.window->openNote(a.path(QStringLiteral("Note.md"))));
    a.command("share:share-note")->trigger();
    auto *dialog = a.window->findChild<ShareDialog *>();
    ASSERT_NE(dialog, nullptr);
    const QString code = dialog->inviteCode();
    ASSERT_FALSE(code.isEmpty());
    dialog->findChild<QPushButton *>(QStringLiteral("shareCopy"))->click();
    EXPECT_EQ(QApplication::clipboard()->text(), code);
    EXPECT_TRUE(tabMarked(a, QStringLiteral("Note")));
    ASSERT_NE(shareBadge(a), nullptr);
    EXPECT_FALSE(shareBadge(a)->isHidden());
    EXPECT_EQ(shareBadge(a)->text(), QStringLiteral("Shared - 0 here"));

    b.command("share:join")->trigger();
    auto *join = b.window->findChild<JoinShareDialog *>();
    ASSERT_NE(join, nullptr);
    auto *field = join->findChild<QLineEdit *>(QStringLiteral("joinCode"));
    auto *ok = join->findChild<QPushButton *>(QStringLiteral("joinOK"));
    auto *error = join->findChild<QLabel *>(QStringLiteral("joinError"));
    field->setText(QStringLiteral("not a code"));
    EXPECT_FALSE(ok->isEnabled());
    EXPECT_FALSE(error->isHidden()) << "what is wrong shows under the field";
    EXPECT_FALSE(error->text().isEmpty());
    field->setText(code);
    EXPECT_TRUE(ok->isEnabled());
    EXPECT_TRUE(error->isHidden());
    ok->click();
    ASSERT_TRUE(waitFor([&] { return b.text() == QStringLiteral("hello\n"); }));

    // Both devices show the same safety number for each other, and the owner's list follows.
    const QByteArray bKey = b.engine()->publicKey();
    ASSERT_TRUE(waitFor([&] { return memberRow(dialog, bKey) && memberRow(dialog, bKey)->text(3).startsWith(QStringLiteral("here")); }));
    EXPECT_EQ(memberRow(dialog, bKey)->text(2), a.engine()->safetyNumberWith(bKey));
    EXPECT_EQ(memberRow(dialog, bKey)->text(2), b.engine()->safetyNumberWith(a.engine()->publicKey()));
    EXPECT_FALSE(memberRow(dialog, bKey)->text(2).isEmpty());
    EXPECT_EQ(memberRow(dialog, bKey)->text(1), ShareEngine::fingerprintOf(bKey));
    ASSERT_TRUE(waitFor([&] { return shareBadge(a)->text() == QStringLiteral("Shared - 1 here"); }));
    EXPECT_TRUE(tabMarked(b, QStringLiteral("Note")));
}

TEST_P(ShareWindowTest, TheOwnerRemovesADeviceFromTheDialog)
{
    Side a(QStringLiteral("a")), b(QStringLiteral("b"));
    const QByteArray id = share(a, b, QStringLiteral("Note.md"), QStringLiteral("hello\n"));
    ASSERT_FALSE(id.isEmpty());
    a.command("share:who")->trigger();
    auto *dialog = a.window->findChild<ShareDialog *>();
    ASSERT_NE(dialog, nullptr);
    EXPECT_TRUE(dialog->inviteCode().isEmpty()) << "an old code is not shown again";
    const QByteArray bKey = b.engine()->publicKey();
    ASSERT_TRUE(waitFor([&] { return memberRow(dialog, bKey) != nullptr; }));
    auto *remove = dialog->findChild<QPushButton *>(QStringLiteral("shareRevoke"));
    ASSERT_NE(remove, nullptr);
    EXPECT_FALSE(remove->isEnabled()) << "nothing picked";
    QString asked;
    dialog->setConfirmForTests([&](const QString &name) {
        asked = name;
        return true;
    });
    dialog->findChild<QTreeWidget *>(QStringLiteral("shareMembers"))->setCurrentItem(memberRow(dialog, bKey));
    ASSERT_TRUE(remove->isEnabled());
    remove->click();
    EXPECT_FALSE(asked.isEmpty());
    ASSERT_TRUE(waitFor([&] { return a.engine()->members(id).size() == 1 && !memberRow(dialog, bKey); }));
}

TEST_P(ShareWindowTest, StoppingFromTheDialogClearsTheBadgeAndTheMarker)
{
    Side a(QStringLiteral("a")), b(QStringLiteral("b"));
    ASSERT_FALSE(share(a, b, QStringLiteral("Note.md"), QStringLiteral("hello\n")).isEmpty());
    ASSERT_TRUE(tabMarked(a, QStringLiteral("Note")));
    a.command("share:who")->trigger();
    auto *dialog = a.window->findChild<ShareDialog *>();
    ASSERT_NE(dialog, nullptr);
    dialog->findChild<QPushButton *>(QStringLiteral("shareStop"))->click();
    EXPECT_TRUE(a.engine()->shares().isEmpty());
    EXPECT_TRUE(shareBadge(a)->isHidden());
    EXPECT_FALSE(tabMarked(a, QStringLiteral("Note")));
}

TEST_P(ShareWindowTest, StoppingStopsOnlyHere)
{
    Side a(QStringLiteral("a")), b(QStringLiteral("b"));
    const QByteArray id = share(a, b, QStringLiteral("Note.md"), QStringLiteral("hello\n"));
    ASSERT_FALSE(id.isEmpty());
    a.command("share:stop")->trigger();
    EXPECT_TRUE(a.engine()->shares().isEmpty());
    a.typeAt(0, QStringLiteral("local "));
    QTest::qWait(500);
    EXPECT_EQ(b.text(), QStringLiteral("hello\n"));
    b.typeAt(0, QStringLiteral("B "));
    ASSERT_TRUE(waitFor([&] { return b.engine()->doc(id)->text() == QStringLiteral("B hello\n"); }));
    QTest::qWait(300);
    EXPECT_EQ(a.text(), QStringLiteral("local hello\n")) << "nothing arrives once it is stopped here";
}

namespace {

/// A note "x", an emoji (two UTF-16 units), "y". `act` makes an edit in A's editor that
/// leaves half of the pair behind (what vim's x or a Backspace on an emoji does);
/// the editor's own edit must be `(at, removed, inserted)`, so the test is about the
/// case it names, and both sides must end up holding `expected`.
void splitCase(const std::function<void(MarkdownEditor *)> &act, std::tuple<qsizetype, qsizetype, QString> editOf,
               const QString &expected)
{
    Side a(QStringLiteral("a")), b(QStringLiteral("b"));
    const QString emoji = QString::fromUcs4(U"\U0001F600", 1);
    const QByteArray id = share(a, b, QStringLiteral("Note.md"), QStringLiteral("x") + emoji + QStringLiteral("y\n"));
    ASSERT_FALSE(id.isEmpty());

    QList<std::tuple<qsizetype, qsizetype, QString>> edits;
    QObject::connect(a.editor(), &MarkdownEditor::edited, a.editor(),
                     [&edits](qsizetype at, qsizetype removed, const QString &inserted) { edits.append({at, removed, inserted}); });
    act(a.editor());
    ASSERT_EQ(edits.size(), 1);
    ASSERT_TRUE(edits[0] == editOf) << "the editor made another edit than the case is about";

    ASSERT_TRUE(waitFor([&] { return b.text() == expected; })) << "the other side never sees half an emoji";
    EXPECT_EQ(a.text(), expected) << "the editor follows the document";
    EXPECT_EQ(a.engine()->doc(id)->text(), expected);
    EXPECT_EQ(a.engine()->counters(id).dropped, 0);
}

} // namespace

TEST_P(ShareWindowTest, ADeleteOnTheFirstHalfOfAPairTakesTheWholePair)
{
    splitCase(
        [](MarkdownEditor *editor) {
            editor->setCursorOffset(1);
            QTest::keyClick(editor, Qt::Key_Delete);
        },
        {1, 1, QString()}, QStringLiteral("xy\n"));
}

TEST_P(ShareWindowTest, ABackspaceOnTheSecondHalfOfAPairTakesTheWholePair)
{
    splitCase(
        [](MarkdownEditor *editor) {
            editor->setCursorOffset(3);
            QTest::keyClick(editor, Qt::Key_Backspace);
        },
        {2, 1, QString()}, QStringLiteral("xy\n"));
}

TEST_P(ShareWindowTest, TypingBetweenTheHalvesOfAPairLandsAfterThePair)
{
    splitCase(
        [](MarkdownEditor *editor) {
            editor->setCursorOffset(2);
            editor->insertAtCaret(QStringLiteral("Z"));
        },
        {2, 0, QStringLiteral("Z")}, QStringLiteral("x") + QString::fromUcs4(U"\U0001F600", 1) + QStringLiteral("Zy\n"));
}

TEST_P(ShareWindowTest, AKeystrokeNeverWritesTheStore)
{
    QString big;
    for (int i = 0; i < 600; ++i)
        big += QStringLiteral("line %1 of a long shared note\n").arg(i);
    Side a(QStringLiteral("a")), b(QStringLiteral("b"));
    const QByteArray id = share(a, b, QStringLiteral("Note.md"), big);
    ASSERT_FALSE(id.isEmpty());
    // The engine saves on a 2 s debounce and when the note is saved, never per
    // keystroke (encoding the whole state costs O(note)). The burst below never
    // returns to the event loop, so any write in it was made by a keystroke.
    const QString state = a.store.filePath(QString::fromLatin1(id.toHex()) + QStringLiteral("/state.bin"));
    const QString base = a.store.filePath(QString::fromLatin1(id.toHex()) + QStringLiteral("/base.md"));
    const auto bytes = [](const QString &path) {
        QFile file(path);
        return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
    };
    const QByteArray stateBefore = bytes(state), baseBefore = bytes(base);
    ASSERT_FALSE(stateBefore.isEmpty());

    a.editor()->setCursorOffset(a.text().size() / 2);
    for (int i = 0; i < 200; ++i)
        a.editor()->insertAtCaret(QStringLiteral("a"));
    EXPECT_EQ(bytes(state), stateBefore);
    EXPECT_EQ(bytes(base), baseBefore);
    // ... and it does get written, by the debounce
    ASSERT_TRUE(waitFor([&] { return bytes(state) != stateBefore; }, 6000));
}

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    // Settings, snapshots, the link's log and the index cache: never the real ones.
    QTemporaryDir configHome, dataHome, cacheHome, stateHome;
    qputenv("XDG_CONFIG_HOME", configHome.path().toUtf8());
    qputenv("XDG_DATA_HOME", dataHome.path().toUtf8());
    qputenv("XDG_CACHE_HOME", cacheHome.path().toUtf8());
    qputenv("XDG_STATE_HOME", stateHome.path().toUtf8());
    QApplication app(argc, argv);
    // Never the real keyring (a silent secret-service costs 15 s a call).
    noteAppC::secrets::setStore({[](const QString &key, QByteArray *value) {
                           if (!memoryStore().contains(key))
                               return false;
                           *value = memoryStore().value(key);
                           return true;
                       },
                       [](const QString &key, const QByteArray &value) { return memoryStore().insert(key, value) != memoryStore().end(); },
                       [](const QString &key) { return memoryStore().remove(key) > 0; }});
    QCoreApplication::setOrganizationName(QStringLiteral("noteAppC-test"));
    QCoreApplication::setApplicationName(QStringLiteral("noteAppC-test"));
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
