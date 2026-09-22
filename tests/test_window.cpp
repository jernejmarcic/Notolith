// Widget smoke test: drives the real MainWindow on the offscreen platform, so
// the Phase 0 exit criteria (open vault, file tree, open note, save) are
// checked by something other than a human looking at the screen.

#include <gtest/gtest.h>

#include <cstdio>

#include <QApplication>
#include <QDir>
#include <QFileSystemModel>
#include <QFile>
#include <QTest>

#include "MarkdownEditor.h"
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTreeView>

#include "MainWindow.h"
#include "Vault.h"

using noteAppC::MainWindow;
using noteAppC::MarkdownEditor;

namespace {

QByteArray readAll(const QString &path)
{
    QFile file(path);
    EXPECT_TRUE(file.open(QIODevice::ReadOnly)) << path.toStdString();
    return file.readAll();
}

/// A vault with one LF note, one CRLF note and one non-note file.
class WindowTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        ASSERT_TRUE(dir.isValid());
        ASSERT_TRUE(QDir(dir.path()).mkpath(QStringLiteral("Projects")));
        write(QStringLiteral("Welcome.md"), "# Welcome\n\nBody.\n");
        write(QStringLiteral("Projects/Crlf.md"), "one\r\ntwo\r\n");
        write(QStringLiteral("attachment.txt"), "not a note\n");
    }

    void write(const QString &relative, const QByteArray &bytes)
    {
        QFile file(dir.filePath(relative));
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        ASSERT_EQ(file.write(bytes), bytes.size());
    }

    QString path(const QString &relative) const { return dir.filePath(relative); }

    QTemporaryDir dir;
};

} // namespace

TEST_F(WindowTest, OpensAVaultAndShowsItsTree)
{
    MainWindow window;
    auto *files = window.findChild<QFileSystemModel *>();
    ASSERT_NE(files, nullptr);
    // QFileSystemModel fills the root on a worker thread.
    QSignalSpy loaded(files, &QFileSystemModel::directoryLoaded);

    window.openVault(dir.path());
    EXPECT_TRUE(window.windowTitle().contains(QDir(dir.path()).dirName()));
    ASSERT_TRUE(loaded.wait(5000));

    auto *tree = window.findChild<QTreeView *>(QStringLiteral("fileExplorer"));
    ASSERT_NE(tree, nullptr);
    ASSERT_TRUE(tree->rootIndex().isValid());
    // Projects/, Welcome.md, attachment.txt - .obsidian and dotfiles stay hidden.
    EXPECT_EQ(tree->model()->rowCount(tree->rootIndex()), 3);
}

TEST_F(WindowTest, OpensANoteIntoTheEditor)
{
    MainWindow window;
    window.openVault(dir.path());
    window.openNote(path(QStringLiteral("Welcome.md")));

    auto *editor = window.findChild<MarkdownEditor *>();
    ASSERT_NE(editor, nullptr);
    EXPECT_TRUE(editor->isEnabled());
    EXPECT_EQ(editor->noteText(), QStringLiteral("# Welcome\n\nBody.\n"));
    EXPECT_FALSE(editor->isModified());
    EXPECT_TRUE(window.windowTitle().startsWith(QStringLiteral("Welcome.md")));
}

TEST_F(WindowTest, RefusesANonNoteAndLeavesTheEditorAlone)
{
    MainWindow window;
    window.openVault(dir.path());
    window.openNote(path(QStringLiteral("Welcome.md")));
    window.openNote(path(QStringLiteral("attachment.txt")));

    auto *editor = window.findChild<MarkdownEditor *>();
    ASSERT_NE(editor, nullptr);
    EXPECT_EQ(editor->noteText(), QStringLiteral("# Welcome\n\nBody.\n"));
}

TEST_F(WindowTest, EditingMarksTheTitleAndSavingClearsIt)
{
    MainWindow window;
    window.openVault(dir.path());
    window.openNote(path(QStringLiteral("Welcome.md")));

    auto *editor = window.findChild<MarkdownEditor *>();
    ASSERT_NE(editor, nullptr);
    editor->setCursorOffset(0);
    QTest::keyClicks(editor, QStringLiteral("x"));
    EXPECT_TRUE(window.windowTitle().startsWith(QLatin1Char('*')));

    ASSERT_TRUE(QMetaObject::invokeMethod(&window, "save"));
    EXPECT_FALSE(window.windowTitle().startsWith(QLatin1Char('*')));
    EXPECT_EQ(readAll(path(QStringLiteral("Welcome.md"))), QByteArray("x# Welcome\n\nBody.\n"));
}

TEST_F(WindowTest, SavingAnUntouchedNoteChangesNoBytes)
{
    const QByteArray before = readAll(path(QStringLiteral("Projects/Crlf.md")));

    MainWindow window;
    window.openVault(dir.path());
    window.openNote(path(QStringLiteral("Projects/Crlf.md")));
    ASSERT_TRUE(QMetaObject::invokeMethod(&window, "save"));

    EXPECT_EQ(readAll(path(QStringLiteral("Projects/Crlf.md"))), before);
}

TEST_F(WindowTest, EditingACrlfNoteKeepsCrlf)
{
    MainWindow window;
    window.openVault(dir.path());
    window.openNote(path(QStringLiteral("Projects/Crlf.md")));

    auto *editor = window.findChild<MarkdownEditor *>();
    ASSERT_NE(editor, nullptr);
    EXPECT_EQ(editor->noteText(), QStringLiteral("one\ntwo\n"));

    editor->setNoteText(QStringLiteral("one\ntwo\nthree\n"));
    ASSERT_TRUE(QMetaObject::invokeMethod(&window, "save"));
    EXPECT_EQ(readAll(path(QStringLiteral("Projects/Crlf.md"))),
              QByteArray("one\r\ntwo\r\nthree\r\n"));
}

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("noteAppC-test"));
    QCoreApplication::setApplicationName(QStringLiteral("noteAppC-test"));
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
