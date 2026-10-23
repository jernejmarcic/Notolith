// The autosave timers commit a note's file on a thread of their own: what is
// on disk, what the tab says is modified, and what a later save sees.

#include <gtest/gtest.h>

#include <QApplication>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QMetaObject>
#include <QTemporaryDir>
#include <QTest>

#include "MainWindow.h"
#include "MarkdownEditor.h"

using noteAppC::MainWindow;
using noteAppC::MarkdownEditor;

namespace {

QByteArray readAll(const QString &path)
{
    QFile file(path);
    EXPECT_TRUE(file.open(QIODevice::ReadOnly)) << path.toStdString();
    return file.readAll();
}

class AutosaveTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        ASSERT_TRUE(dir.isValid());
        QFile file(dir.filePath(QStringLiteral("Note.md")));
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        file.write("# Note\n\nBody.\n");
    }
    QString note() const { return dir.filePath(QStringLiteral("Note.md")); }
    QTemporaryDir dir;
};

} // namespace

// Twenty rapid saves of growing text (one still in flight is not overtaken: the
// tab is saved again when it lands): the file ends up holding the last text,
// with nothing left to save.
TEST_F(AutosaveTest, RapidSavesLandInOrderAndTheFileHoldsTheLast)
{
    MainWindow window;
    window.setAutosaveIntervalsForTests(600000, 600000);
    window.openVault(dir.path());
    window.openNote(note());
    MarkdownEditor *editor = window.editor();
    editor->setCursorOffset(editor->noteText().size());
    for (int i = 0; i < 20; ++i) {
        QTest::keyClicks(editor, QStringLiteral("x"));
        QMetaObject::invokeMethod(&window, "autosaveInBackground");
    }
    QMetaObject::invokeMethod(&window, "flushSaves");
    QMetaObject::invokeMethod(&window, "autosaveInBackground"); // what finishSave() arms the timer for
    QMetaObject::invokeMethod(&window, "flushSaves");
    EXPECT_EQ(readAll(note()), editor->noteText().toUtf8());
    EXPECT_TRUE(readAll(note()).contains("xxxxxxxxxxxxxxxxxxxx"));
    EXPECT_FALSE(editor->isModified());
}

// Typing while a save is being committed: the tab keeps the newer text, stays
// modified, and the next autosave writes it.
TEST_F(AutosaveTest, TypingDuringACommitIsKeptAndSavedNext)
{
    MainWindow window;
    window.setAutosaveIntervalsForTests(600000, 600000);
    window.openVault(dir.path());
    window.openNote(note());
    MarkdownEditor *editor = window.editor();
    editor->setCursorOffset(editor->noteText().size());
    QTest::keyClicks(editor, QStringLiteral("a"));
    const QString first = editor->noteText();
    QMetaObject::invokeMethod(&window, "autosaveInBackground");
    QTest::keyClicks(editor, QStringLiteral("b")); // after the text was taken, before the commit is reported
    QMetaObject::invokeMethod(&window, "flushSaves");
    EXPECT_EQ(readAll(note()), first.toUtf8());
    EXPECT_TRUE(editor->noteText().contains(QLatin1String("ab")));
    EXPECT_TRUE(editor->isModified()) << "the second letter is not on disk yet";

    QMetaObject::invokeMethod(&window, "autosaveInBackground");
    QMetaObject::invokeMethod(&window, "flushSaves");
    EXPECT_EQ(readAll(note()), editor->noteText().toUtf8());
    EXPECT_FALSE(editor->isModified());
}

// An explicit save right after an autosave sees the file the autosave wrote:
// no "changed on disk" for its own write.
TEST_F(AutosaveTest, AnExplicitSaveAfterAnAutosaveIsConsistent)
{
    MainWindow window;
    window.setAutosaveIntervalsForTests(600000, 600000);
    window.openVault(dir.path());
    window.openNote(note());
    MarkdownEditor *editor = window.editor();
    editor->setCursorOffset(editor->noteText().size());
    QTest::keyClicks(editor, QStringLiteral("a"));
    QMetaObject::invokeMethod(&window, "autosaveInBackground");
    QTest::keyClicks(editor, QStringLiteral("b"));
    bool saved = false;
    QMetaObject::invokeMethod(&window, "save", Qt::DirectConnection, Q_RETURN_ARG(bool, saved));
    EXPECT_TRUE(saved);
    EXPECT_EQ(readAll(note()), editor->noteText().toUtf8());
    EXPECT_FALSE(editor->isModified());
}

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    // The window reads and writes the settings, the recovery copies and the index cache: temporary ones.
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
