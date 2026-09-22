// Widget smoke test: drives the real MainWindow on the offscreen platform, so
// the Phase 0 exit criteria (open vault, file tree, open note, save) are
// checked by something other than a human looking at the screen.

#include <gtest/gtest.h>

#include <cstdio>

#include <QAbstractButton>
#include <QApplication>
#include <QDir>
#include <QFileSystemModel>
#include <QFile>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QStackedWidget>
#include <QTabBar>
#include <QTest>
#include <QTimer>
#include <QToolButton>

#include "MarkdownEditor.h"
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTreeView>
#include <QTreeWidget>

#include "Index.h"
#include "MainWindow.h"
#include "Vault.h"

using noteAppC::Index;
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

TEST_F(WindowTest, TagsPaneListsTagsNestedAndSortedByCount)
{
    write(QStringLiteral("A.md"), "#project #project/alpha\n");
    write(QStringLiteral("B.md"), "#project\n");
    write(QStringLiteral("C.md"), "#area\n");

    MainWindow window;
    QSignalSpy indexed(window.index(), &Index::changed);
    window.openVault(dir.path());
    ASSERT_TRUE(indexed.wait(5000));

    auto *tagsPane = window.findChild<QWidget *>(QStringLiteral("tagsPane"));
    ASSERT_NE(tagsPane, nullptr);
    auto *list = tagsPane->findChild<QTreeWidget *>();
    ASSERT_NE(list, nullptr);

    // "project" covers A and B (2 notes); its nested child "project/alpha"
    // sits under it rather than at the top level; "area" covers only C.
    // Default order is by count, so "project" (2) comes before "area" (1).
    ASSERT_EQ(list->topLevelItemCount(), 2);
    EXPECT_EQ(list->topLevelItem(0)->text(0), QStringLiteral("project"));
    EXPECT_EQ(list->topLevelItem(0)->text(1), QStringLiteral("2"));
    ASSERT_EQ(list->topLevelItem(0)->childCount(), 1);
    EXPECT_EQ(list->topLevelItem(0)->child(0)->text(0), QStringLiteral("alpha"));
    EXPECT_EQ(list->topLevelItem(1)->text(0), QStringLiteral("area"));
}

TEST_F(WindowTest, OutgoingLinksPaneGroupsByTargetAndFlagsUnresolved)
{
    write(QStringLiteral("Target.md"), "# Target\n");
    write(QStringLiteral("Source.md"), "See [[Target]] and [[Nowhere]].\n");

    MainWindow window;
    QSignalSpy indexed(window.index(), &Index::changed);
    window.openVault(dir.path());
    ASSERT_TRUE(indexed.wait(5000));
    window.openNote(path(QStringLiteral("Source.md")));

    auto *pane = window.findChild<QWidget *>(QStringLiteral("outgoingLinksPane"));
    ASSERT_NE(pane, nullptr);
    auto *list = pane->findChild<QTreeWidget *>();
    ASSERT_NE(list, nullptr);

    ASSERT_EQ(list->topLevelItemCount(), 2);
    EXPECT_EQ(list->topLevelItem(0)->text(0), QStringLiteral("Target"));
    ASSERT_EQ(list->topLevelItem(0)->childCount(), 1);
    EXPECT_EQ(list->topLevelItem(0)->child(0)->text(0),
             QStringLiteral("See [[Target]] and [[Nowhere]]."));
    EXPECT_EQ(list->topLevelItem(1)->text(0), QStringLiteral("Unresolved links"));
    ASSERT_EQ(list->topLevelItem(1)->childCount(), 1);
}

TEST_F(WindowTest, BacklinksPaneIsPopulatedAfterAColdCacheIndexSettles)
{
    write(QStringLiteral("Target.md"), "# Target\n");
    write(QStringLiteral("Source.md"), "See [[Target]].\n");

    MainWindow window;
    QSignalSpy indexed(window.index(), &Index::changed);
    window.openVault(dir.path());
    ASSERT_TRUE(indexed.wait(5000));

    // openNote() used to trust its argument verbatim; a path relative to the
    // working directory (what the screenshot tool passes when a user types a
    // relative argument) broke every Index lookup keyed on m_notePath, so the
    // backlinks pane stayed empty no matter how long the index had settled.
    const QString relative = QDir::current().relativeFilePath(path(QStringLiteral("Target.md")));
    window.openNote(relative);

    auto *pane = window.findChild<QWidget *>(QStringLiteral("backlinksPane"));
    ASSERT_NE(pane, nullptr);
    auto *list = pane->findChild<QTreeWidget *>();
    ASSERT_NE(list, nullptr);
    ASSERT_EQ(list->topLevelItemCount(), 1);
    EXPECT_EQ(list->topLevelItem(0)->text(0), QStringLiteral("Source"));
}

TEST_F(WindowTest, HeaderBarHasItsWidgets)
{
    MainWindow window;
    window.openVault(dir.path());

    auto *headerBar = window.findChild<QWidget *>(QStringLiteral("headerBar"));
    ASSERT_NE(headerBar, nullptr);
    EXPECT_EQ(headerBar->minimumHeight(), 44);
    EXPECT_EQ(headerBar->maximumHeight(), 44);

    auto *vaultMenu = window.findChild<QToolButton *>(QStringLiteral("vaultMenuButton"));
    ASSERT_NE(vaultMenu, nullptr);
    EXPECT_EQ(vaultMenu->text(), QDir(dir.path()).dirName());

    auto *search = window.findChild<QLineEdit *>(QStringLiteral("headerSearch"));
    ASSERT_NE(search, nullptr);
    EXPECT_LE(search->maximumWidth(), 420);

    EXPECT_NE(window.findChild<QToolButton *>(QStringLiteral("headerSidebarToggle")), nullptr);
    EXPECT_NE(window.findChild<QToolButton *>(QStringLiteral("headerNewNoteButton")), nullptr);
    EXPECT_NE(window.findChild<QToolButton *>(QStringLiteral("headerRightSidebarToggle")), nullptr);
    EXPECT_NE(window.findChild<QToolButton *>(QStringLiteral("headerSettingsButton")), nullptr);
}

TEST_F(WindowTest, FlatTabShowsTheOpenNoteWithACloseButton)
{
    MainWindow window;
    window.openVault(dir.path());
    window.openNote(path(QStringLiteral("Welcome.md")));

    auto *tabs = window.findChild<QTabBar *>(QStringLiteral("noteTabs"));
    ASSERT_NE(tabs, nullptr);
    ASSERT_EQ(tabs->count(), 1);
    EXPECT_EQ(tabs->tabText(0), QStringLiteral("Welcome"));

    auto *close = window.findChild<QToolButton *>(QStringLiteral("tabCloseButton"));
    ASSERT_NE(close, nullptr);
    close->click();

    auto *editor = window.findChild<MarkdownEditor *>();
    ASSERT_NE(editor, nullptr);
    EXPECT_FALSE(editor->isEnabled());
    EXPECT_EQ(tabs->count(), 0);
}

TEST_F(WindowTest, SidebarFilesRowButtonsCreateNoteAndFolder)
{
    MainWindow window;
    window.openVault(dir.path());

    EXPECT_NE(window.findChild<QLabel *>(QStringLiteral("filesLabel")), nullptr);

    auto *newNote = window.findChild<QToolButton *>(QStringLiteral("sidebarNewNoteButton"));
    ASSERT_NE(newNote, nullptr);
    newNote->click();
    EXPECT_TRUE(QFile::exists(path(QStringLiteral("Untitled.md"))));

    auto *newFolder = window.findChild<QToolButton *>(QStringLiteral("sidebarNewFolderButton"));
    ASSERT_NE(newFolder, nullptr);
    newFolder->click();
    EXPECT_TRUE(QDir(path(QStringLiteral("New folder"))).exists());
}

TEST_F(WindowTest, SettingsPageOpensAndClosesRestoringThePreviousNote)
{
    MainWindow window;
    window.openVault(dir.path());
    window.openNote(path(QStringLiteral("Welcome.md")));

    auto *stack = window.findChild<QStackedWidget *>(QStringLiteral("mainStack"));
    ASSERT_NE(stack, nullptr);
    EXPECT_EQ(stack->currentIndex(), 0);

    auto *settingsPage = window.findChild<QWidget *>(QStringLiteral("settingsPage"));
    ASSERT_NE(settingsPage, nullptr);
    EXPECT_NE(window.findChild<QWidget *>(QStringLiteral("settingsNav")), nullptr);

    window.openSettings();
    EXPECT_EQ(stack->currentIndex(), 1);
    EXPECT_EQ(stack->currentWidget(), settingsPage);

    window.closeSettings();
    EXPECT_EQ(stack->currentIndex(), 0);

    // The note underneath was never touched by opening the settings page.
    auto *editor = window.findChild<MarkdownEditor *>();
    ASSERT_NE(editor, nullptr);
    EXPECT_EQ(editor->noteText(), QStringLiteral("# Welcome\n\nBody.\n"));
}

TEST_F(WindowTest, SingleFileModeHidesChromeAndShowsTheFileName)
{
    QTemporaryDir loose; // no .obsidian ancestor, so resolveLaunchTarget sees no vault
    ASSERT_TRUE(loose.isValid());
    const QString loosePath = loose.filePath(QStringLiteral("Loose.md"));
    {
        QFile file(loosePath);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        ASSERT_TRUE(file.write("# Loose\n") > 0);
    }

    MainWindow window;
    window.openTarget(noteAppC::resolveLaunchTarget(loosePath));

    auto *sidebar = window.findChild<QWidget *>(QStringLiteral("sidebar"));
    auto *rightSidebar = window.findChild<QWidget *>(QStringLiteral("rightSidebar"));
    auto *tabStrip = window.findChild<QWidget *>(QStringLiteral("tabStrip"));
    ASSERT_NE(sidebar, nullptr);
    ASSERT_NE(rightSidebar, nullptr);
    ASSERT_NE(tabStrip, nullptr);
    // isVisible() reflects real screen mapping, which needs a shown window;
    // isHidden() reflects the explicit setVisible(false) these tests care
    // about regardless of whether the window itself was ever shown.
    EXPECT_TRUE(sidebar->isHidden());
    EXPECT_TRUE(rightSidebar->isHidden());
    EXPECT_TRUE(tabStrip->isHidden());

    auto *vaultMenu = window.findChild<QToolButton *>(QStringLiteral("vaultMenuButton"));
    ASSERT_NE(vaultMenu, nullptr);
    EXPECT_EQ(vaultMenu->text(), QStringLiteral("Loose.md"));

    auto *editor = window.findChild<MarkdownEditor *>();
    ASSERT_NE(editor, nullptr);
    EXPECT_EQ(editor->noteText(), QStringLiteral("# Loose\n"));
}

TEST_F(WindowTest, RenamingTheOpenNoteUpdatesLinksAndTheEditorsPath)
{
    write(QStringLiteral("Target.md"), "# Target\n");
    write(QStringLiteral("Source.md"), "See [[Target]].\n");

    MainWindow window;
    QSignalSpy indexed(window.index(), &Index::changed);
    window.openVault(dir.path());
    ASSERT_TRUE(indexed.wait(5000));
    window.openNote(path(QStringLiteral("Target.md")));

    auto *tree = window.findChild<QTreeView *>(QStringLiteral("fileExplorer"));
    ASSERT_NE(tree, nullptr);
    auto *model = qobject_cast<QFileSystemModel *>(tree->model());
    ASSERT_NE(model, nullptr);
    const QModelIndex index = model->index(path(QStringLiteral("Target.md")));
    ASSERT_TRUE(index.isValid());

    // renamePathAt() pops a modal QInputDialog; answer it once its own event
    // loop is actually running.
    QTimer::singleShot(0, &window, [] {
        auto *dialog = qobject_cast<QInputDialog *>(QApplication::activeModalWidget());
        if (!dialog)
            return;
        dialog->setTextValue(QStringLiteral("Renamed"));
        dialog->accept();
    });
    ASSERT_TRUE(QMetaObject::invokeMethod(&window, "renamePathAt", Q_ARG(QModelIndex, index)));

    EXPECT_FALSE(QFile::exists(path(QStringLiteral("Target.md"))));
    EXPECT_TRUE(QFile::exists(path(QStringLiteral("Renamed.md"))));
    // links.autoUpdate defaults on, so the wikilink in Source.md follows -
    // this is Index::updateLinksAfterRename actually being wired in, not
    // just Vault::renamePath moving the file.
    EXPECT_EQ(readAll(path(QStringLiteral("Source.md"))), QByteArray("See [[Renamed]].\n"));
    EXPECT_TRUE(window.windowTitle().startsWith(QStringLiteral("Renamed.md")));
}

TEST_F(WindowTest, DeletingTheOpenNoteMovesItToTrashAndClearsTheEditor)
{
    MainWindow window;
    window.openVault(dir.path());
    window.openNote(path(QStringLiteral("Welcome.md")));

    auto *tree = window.findChild<QTreeView *>(QStringLiteral("fileExplorer"));
    ASSERT_NE(tree, nullptr);
    auto *model = qobject_cast<QFileSystemModel *>(tree->model());
    ASSERT_NE(model, nullptr);
    const QModelIndex index = model->index(path(QStringLiteral("Welcome.md")));
    ASSERT_TRUE(index.isValid());

    // files.confirmDelete defaults on, so deletePathAt() pops a modal
    // QMessageBox; click its Yes button once shown.
    QTimer::singleShot(0, &window, [] {
        auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
        if (!box)
            return;
        if (QAbstractButton *yes = box->button(QMessageBox::Yes))
            yes->click();
    });
    ASSERT_TRUE(QMetaObject::invokeMethod(&window, "deletePathAt", Q_ARG(QModelIndex, index)));

    EXPECT_FALSE(QFile::exists(path(QStringLiteral("Welcome.md"))));
    EXPECT_TRUE(QFile::exists(dir.filePath(QStringLiteral(".trash/Welcome.md"))));

    auto *editor = window.findChild<MarkdownEditor *>();
    ASSERT_NE(editor, nullptr);
    EXPECT_FALSE(editor->isEnabled());
}

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    // noteAppC::Settings::configDirectory() is deliberately fixed to
    // ~/.config/noteAppC regardless of QCoreApplication's org/app name (a
    // vault or single file must not gain its own copy), so MainWindow now
    // reads and can save() there too. Every test here builds a MainWindow,
    // so without this the suite would read the developer's real settings
    // file and, on any test that saves, write to it.
    QTemporaryDir configHome;
    qputenv("XDG_CONFIG_HOME", configHome.path().toUtf8());
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("noteAppC-test"));
    QCoreApplication::setApplicationName(QStringLiteral("noteAppC-test"));
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
