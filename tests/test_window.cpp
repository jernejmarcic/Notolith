// Widget smoke test: drives the real MainWindow on the offscreen platform, so
// the Phase 0 exit criteria (open vault, file tree, open note, save) are
// checked by something other than a human looking at the screen.

#include <gtest/gtest.h>

#include <cstdio>

#include <QAbstractButton>
#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QElapsedTimer>
#include <QFileSystemModel>
#include <QFile>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QScrollBar>
#include <QStackedWidget>
#include <QTabBar>
#include <QTest>
#include <QTimer>
#include <QToolButton>

#include "MarkdownEditor.h"
#include <QSignalSpy>
#include <QAction>
#include <QDate>
#include <QTemporaryDir>
#include <QTreeView>
#include <QTreeWidget>

#include "Index.h"
#include "MainWindow.h"
#include "PagePreview.h"
#include "OutlinePane.h"
#include "BookmarksPane.h"
#include "Snapshots.h"
#include "RecoveryDialog.h"
#include "Settings.h"
#include "Vault.h"

using noteAppC::Index;
using noteAppC::LaunchTarget;
using noteAppC::MainWindow;
using noteAppC::MarkdownEditor;
using noteAppC::Settings;

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

    // Click the button a person actually clicks, not the slot behind it: a
    // disconnected button passes every test that calls openSettings().
    auto *settingsButton =
        window.findChild<QToolButton *>(QStringLiteral("headerSettingsButton"));
    ASSERT_NE(settingsButton, nullptr);
    settingsButton->click();
    EXPECT_EQ(stack->currentIndex(), 1);
    EXPECT_EQ(stack->currentWidget(), settingsPage);

    window.closeSettings();
    ASSERT_EQ(stack->currentIndex(), 0);
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

TEST_F(WindowTest, ClickingATreeRowLeavesSettingsEvenWhenTheNoteIsAlreadyOpen)
{
    write(QStringLiteral("Second.md"), "# Second\n");

    MainWindow window;
    QSignalSpy loaded(window.findChild<QFileSystemModel *>(), &QFileSystemModel::directoryLoaded);
    window.openVault(dir.path());
    ASSERT_TRUE(loaded.wait(5000));
    window.openNote(path(QStringLiteral("Welcome.md")));

    auto *stack = window.findChild<QStackedWidget *>(QStringLiteral("mainStack"));
    ASSERT_NE(stack, nullptr);
    auto *tree = window.findChild<QTreeView *>(QStringLiteral("fileExplorer"));
    ASSERT_NE(tree, nullptr);
    auto *model = qobject_cast<QFileSystemModel *>(tree->model());
    ASSERT_NE(model, nullptr);

    // Welcome.md is already open - openNote() used to return before ever
    // switching the stack back, so clicking the open note did nothing.
    const QModelIndex welcome = model->index(path(QStringLiteral("Welcome.md")));
    ASSERT_TRUE(welcome.isValid());
    window.openSettings();
    ASSERT_EQ(stack->currentIndex(), 1);
    ASSERT_TRUE(QMetaObject::invokeMethod(&window, "onTreeActivated", Q_ARG(QModelIndex, welcome)));
    EXPECT_EQ(stack->currentIndex(), 0);

    // A different note also leaves settings, and opens it.
    const QModelIndex second = model->index(path(QStringLiteral("Second.md")));
    ASSERT_TRUE(second.isValid());
    window.openSettings();
    ASSERT_TRUE(QMetaObject::invokeMethod(&window, "onTreeActivated", Q_ARG(QModelIndex, second)));
    EXPECT_EQ(stack->currentIndex(), 0);
    auto *editor = window.findChild<MarkdownEditor *>();
    ASSERT_NE(editor, nullptr);
    EXPECT_EQ(editor->noteText(), QStringLiteral("# Second\n"));

    // A folder never reaches openNote() at all, so it needs its own check.
    const QModelIndex projects = model->index(path(QStringLiteral("Projects")));
    ASSERT_TRUE(projects.isValid());
    window.openSettings();
    ASSERT_TRUE(QMetaObject::invokeMethod(&window, "onTreeClicked", Q_ARG(QModelIndex, projects)));
    EXPECT_EQ(stack->currentIndex(), 0);
}

TEST_F(WindowTest, ClickingAFolderTogglesItAndActivatedDoesNotToggleItAgain)
{
    MainWindow window;
    QSignalSpy loaded(window.findChild<QFileSystemModel *>(), &QFileSystemModel::directoryLoaded);
    window.openVault(dir.path());
    ASSERT_TRUE(loaded.wait(5000));

    auto *tree = window.findChild<QTreeView *>(QStringLiteral("fileExplorer"));
    ASSERT_NE(tree, nullptr);
    // Qt's own double-click expansion would add a third toggle on top of the
    // two plain clicks a double-click is made of; it must stay off.
    EXPECT_FALSE(tree->expandsOnDoubleClick());

    auto *model = qobject_cast<QFileSystemModel *>(tree->model());
    ASSERT_NE(model, nullptr);
    const QModelIndex projects = model->index(path(QStringLiteral("Projects")));
    ASSERT_TRUE(projects.isValid());
    EXPECT_FALSE(tree->isExpanded(projects));

    ASSERT_TRUE(QMetaObject::invokeMethod(&window, "onTreeClicked", Q_ARG(QModelIndex, projects)));
    EXPECT_TRUE(tree->isExpanded(projects));
    ASSERT_TRUE(QMetaObject::invokeMethod(&window, "onTreeClicked", Q_ARG(QModelIndex, projects)));
    EXPECT_FALSE(tree->isExpanded(projects));

    // A double-click is, from Qt's point of view, two ordinary clicks (each
    // toggling once here) plus one activated() on top of the second - which
    // must be a no-op for a folder, or the pair above would not cancel out.
    ASSERT_TRUE(QMetaObject::invokeMethod(&window, "onTreeClicked", Q_ARG(QModelIndex, projects)));
    ASSERT_TRUE(QMetaObject::invokeMethod(&window, "onTreeClicked", Q_ARG(QModelIndex, projects)));
    ASSERT_TRUE(QMetaObject::invokeMethod(&window, "onTreeActivated", Q_ARG(QModelIndex, projects)));
    EXPECT_FALSE(tree->isExpanded(projects));

    // Return, with the folder current, toggles it exactly once too.
    tree->setCurrentIndex(projects);
    QTest::keyClick(tree, Qt::Key_Return);
    EXPECT_TRUE(tree->isExpanded(projects));
}

TEST_F(WindowTest, BackAndForwardNavigateHistoryAndDisableAtTheEnds)
{
    write(QStringLiteral("Second.md"), "# Second\n");
    write(QStringLiteral("Third.md"), "# Third\n");

    MainWindow window;
    window.openVault(dir.path());
    window.openNote(path(QStringLiteral("Welcome.md")));
    window.openNote(path(QStringLiteral("Second.md")));
    window.openNote(path(QStringLiteral("Third.md")));

    auto *editor = window.findChild<MarkdownEditor *>();
    ASSERT_NE(editor, nullptr);
    auto *back = window.findChild<QToolButton *>(QStringLiteral("headerBackButton"));
    auto *forward = window.findChild<QToolButton *>(QStringLiteral("headerForwardButton"));
    ASSERT_NE(back, nullptr);
    ASSERT_NE(forward, nullptr);
    EXPECT_TRUE(back->isEnabled());
    EXPECT_FALSE(forward->isEnabled());

    window.goBack();
    EXPECT_EQ(editor->noteText(), QStringLiteral("# Second\n"));
    EXPECT_TRUE(back->isEnabled());
    EXPECT_TRUE(forward->isEnabled());

    window.goBack();
    EXPECT_EQ(editor->noteText(), QStringLiteral("# Welcome\n\nBody.\n"));
    EXPECT_FALSE(back->isEnabled()); // nowhere further back to go

    window.goForward();
    window.goForward();
    EXPECT_EQ(editor->noteText(), QStringLiteral("# Third\n"));
    EXPECT_FALSE(forward->isEnabled()); // nowhere further forward to go

    // Opening a note from history must not itself push a new entry: going
    // back twice from the end still lands on the first note opened, not on
    // some duplicate further down a lengthened list.
    window.goBack();
    window.goBack();
    EXPECT_EQ(editor->noteText(), QStringLiteral("# Welcome\n\nBody.\n"));
    EXPECT_FALSE(back->isEnabled());
}

TEST_F(WindowTest, GoingBackSkipsAHistoryEntryThatWasDeleted)
{
    write(QStringLiteral("Second.md"), "# Second\n");
    write(QStringLiteral("Third.md"), "# Third\n");

    MainWindow window;
    window.openVault(dir.path());
    window.openNote(path(QStringLiteral("Welcome.md")));
    window.openNote(path(QStringLiteral("Second.md")));
    window.openNote(path(QStringLiteral("Third.md")));

    auto *editor = window.findChild<MarkdownEditor *>();
    ASSERT_NE(editor, nullptr);
    ASSERT_TRUE(QFile::remove(path(QStringLiteral("Second.md"))));

    // history is [Welcome, Second, Third]; Second is gone, so going back from
    // Third must land on Welcome without popping a warning dialog.
    window.goBack();
    EXPECT_EQ(editor->noteText(), QStringLiteral("# Welcome\n\nBody.\n"));
    EXPECT_EQ(QApplication::activeModalWidget(), nullptr);
}

TEST_F(WindowTest, MouseBackButtonNavigatesHistoryFromAnyWidget)
{
    write(QStringLiteral("Second.md"), "# Second\n");

    MainWindow window;
    window.openVault(dir.path());
    window.openNote(path(QStringLiteral("Welcome.md")));
    window.openNote(path(QStringLiteral("Second.md")));

    auto *editor = window.findChild<MarkdownEditor *>();
    ASSERT_NE(editor, nullptr);
    ASSERT_EQ(editor->noteText(), QStringLiteral("# Second\n"));

    // The filter is installed on the application (see MainWindow's
    // constructor), so the click is delivered to the editor here and still
    // has to reach MainWindow rather than only working on one specific
    // widget.
    QTest::mouseClick(editor, Qt::BackButton);
    EXPECT_EQ(editor->noteText(), QStringLiteral("# Welcome\n\nBody.\n"));

    QTest::mouseClick(editor, Qt::ForwardButton);
    EXPECT_EQ(editor->noteText(), QStringLiteral("# Second\n"));
}

TEST_F(WindowTest, AltLeftAndAltRightNavigateHistory)
{
    write(QStringLiteral("Second.md"), "# Second\n");

    MainWindow window;
    window.openVault(dir.path());
    window.openNote(path(QStringLiteral("Welcome.md")));
    window.openNote(path(QStringLiteral("Second.md")));
    window.show();
    ASSERT_TRUE(QTest::qWaitForWindowExposed(&window));

    auto *editor = window.findChild<MarkdownEditor *>();
    ASSERT_NE(editor, nullptr);

    QTest::keyClick(&window, Qt::Key_Left, Qt::AltModifier);
    EXPECT_EQ(editor->noteText(), QStringLiteral("# Welcome\n\nBody.\n"));

    QTest::keyClick(&window, Qt::Key_Right, Qt::AltModifier);
    EXPECT_EQ(editor->noteText(), QStringLiteral("# Second\n"));
}

TEST_F(WindowTest, ReopeningANoteRestoresCaretAndScroll)
{
    QString longText;
    for (int i = 0; i < 500; ++i)
        longText += QStringLiteral("line %1\n").arg(i);
    write(QStringLiteral("Long.md"), longText.toUtf8());
    write(QStringLiteral("Second.md"), "# Second\n");

    MainWindow window;
    window.openVault(dir.path());
    window.openNote(path(QStringLiteral("Long.md")));

    auto *editor = window.findChild<MarkdownEditor *>();
    ASSERT_NE(editor, nullptr);
    editor->setCursorOffset(200);
    editor->verticalScrollBar()->setValue(editor->verticalScrollBar()->maximum());
    const int scrolled = editor->verticalScrollBar()->value();
    ASSERT_GT(scrolled, 0);

    window.openNote(path(QStringLiteral("Second.md")));
    EXPECT_EQ(editor->cursorOffset(), 0); // a fresh note starts at the top

    window.openNote(path(QStringLiteral("Long.md")));
    EXPECT_EQ(editor->cursorOffset(), 200);
    EXPECT_EQ(editor->verticalScrollBar()->value(), scrolled);
}

TEST_F(WindowTest, LaunchWithNoArgumentReopensTheLastVaultAndNote)
{
    const QByteArray previousConfigHome = qgetenv("XDG_CONFIG_HOME");
    QTemporaryDir configHome;
    ASSERT_TRUE(configHome.isValid());
    qputenv("XDG_CONFIG_HOME", configHome.path().toUtf8());

    {
        MainWindow first;
        first.openVault(dir.path());
        first.openNote(path(QStringLiteral("Welcome.md")));
        // A real quit records the position and saves it; close() delivers
        // the same QCloseEvent without needing a shown, on-screen window.
        first.close();
    }

    MainWindow second;
    second.openTarget(LaunchTarget{});

    EXPECT_EQ(second.windowTitle().contains(QDir(dir.path()).dirName()), true);
    auto *editor = second.findChild<MarkdownEditor *>();
    ASSERT_NE(editor, nullptr);
    EXPECT_EQ(editor->noteText(), QStringLiteral("# Welcome\n\nBody.\n"));

    qputenv("XDG_CONFIG_HOME", previousConfigHome);
}

TEST_F(WindowTest, OpeningANoteFromOutsideTheTreeRevealsItThere)
{
    MainWindow window;
    window.openVault(dir.path());
    // No wait for QFileSystemModel here on purpose: revealInTree() has to
    // cope with the nested folder not being loaded yet on its own.
    window.openNote(path(QStringLiteral("Projects/Crlf.md")));

    auto *tree = window.findChild<QTreeView *>(QStringLiteral("fileExplorer"));
    ASSERT_NE(tree, nullptr);
    auto *model = qobject_cast<QFileSystemModel *>(tree->model());
    ASSERT_NE(model, nullptr);

    QModelIndex current;
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 5000) {
        QCoreApplication::processEvents();
        current = tree->currentIndex();
        if (current.isValid())
            break;
    }
    ASSERT_TRUE(current.isValid());
    EXPECT_EQ(model->filePath(current), path(QStringLiteral("Projects/Crlf.md")));
    EXPECT_TRUE(tree->isExpanded(model->parent(current)));
}

TEST_F(WindowTest, CtrlCommaOpensSettingsAndEscapeCloses)
{
    MainWindow window;
    window.openVault(dir.path());
    window.show();
    ASSERT_TRUE(QTest::qWaitForWindowExposed(&window));

    auto *stack = window.findChild<QStackedWidget *>(QStringLiteral("mainStack"));
    ASSERT_NE(stack, nullptr);
    ASSERT_EQ(stack->currentIndex(), 0);

    QTest::keyClick(&window, Qt::Key_Comma, Qt::ControlModifier);
    EXPECT_EQ(stack->currentIndex(), 1);

    auto *settingsPage = window.findChild<QWidget *>(QStringLiteral("settingsPage"));
    ASSERT_NE(settingsPage, nullptr);
    QTest::keyClick(settingsPage, Qt::Key_Escape);
    EXPECT_EQ(stack->currentIndex(), 0);
}

TEST_F(WindowTest, EditingTheSelectedThemeFileRestylesLive)
{
    // Its own private config home: the suite-wide one in main() is shared by
    // every test in this binary, and this test writes an actual theme file
    // into it rather than just settings.ini.
    const QByteArray previousConfigHome = qgetenv("XDG_CONFIG_HOME");
    QTemporaryDir configHome;
    ASSERT_TRUE(configHome.isValid());
    qputenv("XDG_CONFIG_HOME", configHome.path().toUtf8());

    const QString themesDir = Settings::themesDirectory();
    ASSERT_TRUE(QDir().mkpath(themesDir));
    const QString schemePath = QDir(themesDir).filePath(QStringLiteral("Live.json"));
    {
        QFile file(schemePath);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        file.write(QByteArrayLiteral(R"({"name":"Live","dark":{"--background-primary":"#111111"}})"));
    }

    MainWindow window;
    // openTarget, as main.cpp reaches it through openWhenShown: watching theme
    // files starts there, after the first paint, not in the constructor.
    window.openTarget(noteAppC::resolveLaunchTarget(dir.path()));
    // SettingsPage builds its rows lazily on showEvent(), which only fires
    // once the window itself is actually shown - not just given focus in the
    // stacked widget.
    window.show();
    ASSERT_TRUE(QTest::qWaitForWindowExposed(&window));

    auto *settingsButton = window.findChild<QToolButton *>(QStringLiteral("headerSettingsButton"));
    ASSERT_NE(settingsButton, nullptr);
    settingsButton->click();

    auto *combo = window.findChild<QComboBox *>(QStringLiteral("settingsSchemeCombo"));
    ASSERT_NE(combo, nullptr);
    const int index = combo->findData(schemePath);
    ASSERT_GE(index, 0) << "Live.json was not offered - themesDirectory() and "
                           "Scheme::searchPaths() have drifted apart again";
    combo->setCurrentIndex(index);
    EXPECT_EQ(window.theme().value(QStringLiteral("--background-primary")),
              QStringLiteral("#111111"));

    // Edited the way a person's editor does - in place - with no call back
    // into MainWindow at all: only the file on disk changed.
    {
        QFile file(schemePath);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write(QByteArrayLiteral(R"({"name":"Live","dark":{"--background-primary":"#222222"}})"));
    }

    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 5000
           && window.theme().value(QStringLiteral("--background-primary"))
                  != QStringLiteral("#222222")) {
        QCoreApplication::processEvents();
        QTest::qWait(10);
    }
    EXPECT_EQ(window.theme().value(QStringLiteral("--background-primary")),
              QStringLiteral("#222222"));

    qputenv("XDG_CONFIG_HOME", previousConfigHome);
}

TEST_F(WindowTest, AddingAThemeFileRefreshesTheOpenSettingsPagesList)
{
    const QByteArray previousConfigHome = qgetenv("XDG_CONFIG_HOME");
    QTemporaryDir configHome;
    ASSERT_TRUE(configHome.isValid());
    qputenv("XDG_CONFIG_HOME", configHome.path().toUtf8());

    // The themes directory exists before launch, as it does for anyone who
    // has themes: noteAppC never creates it just to watch it, since opening a
    // note must leave nothing behind in ~/.config.
    const QString themesDir = Settings::themesDirectory();
    ASSERT_TRUE(QDir().mkpath(themesDir));

    MainWindow window;
    window.openTarget(noteAppC::resolveLaunchTarget(dir.path()));
    window.show();
    ASSERT_TRUE(QTest::qWaitForWindowExposed(&window));

    auto *settingsButton = window.findChild<QToolButton *>(QStringLiteral("headerSettingsButton"));
    ASSERT_NE(settingsButton, nullptr);
    settingsButton->click();

    auto *combo = window.findChild<QComboBox *>(QStringLiteral("settingsSchemeCombo"));
    ASSERT_NE(combo, nullptr);
    const int before = combo->count();

    {
        QFile file(QDir(themesDir).filePath(QStringLiteral("New.json")));
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        file.write(QByteArrayLiteral(R"({"name":"New","dark":{}})"));
    }

    // setContext() only rebuilds this instant if the page is visible, which it
    // is here - so the combo (a fresh widget each rebuild, not the one above)
    // should gain the new entry without the page ever being closed.
    QElapsedTimer timer;
    timer.start();
    QComboBox *refreshed = nullptr;
    while (timer.elapsed() < 5000) {
        QCoreApplication::processEvents();
        refreshed = window.findChild<QComboBox *>(QStringLiteral("settingsSchemeCombo"));
        if (refreshed && refreshed->count() > before)
            break;
        QTest::qWait(10);
    }
    ASSERT_NE(refreshed, nullptr);
    EXPECT_GT(refreshed->count(), before);
    EXPECT_GE(refreshed->findData(QDir(themesDir).filePath(QStringLiteral("New.json"))), 0);

    qputenv("XDG_CONFIG_HOME", previousConfigHome);
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

TEST_F(WindowTest, AVaultsObsidianHotkeysRebindTheWindowAndTheEditor)
{
    // An Obsidian user's keys come with their vault: read, never written.
    ASSERT_TRUE(QDir(dir.path()).mkpath(QStringLiteral(".obsidian")));
    write(QStringLiteral(".obsidian/hotkeys.json"),
          R"({"app:open-settings": [{"modifiers": ["Mod", "Shift"], "key": "P"}],
              "editor:toggle-bold": [{"modifiers": ["Mod", "Shift"], "key": "B"}]})");
    MainWindow window;
    window.openVault(dir.path());

    auto *settings = window.findChild<QAction *>(QStringLiteral("app:open-settings"));
    ASSERT_NE(settings, nullptr);
    EXPECT_EQ(settings->shortcut(), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_P));

    window.openNote(path(QStringLiteral("Welcome.md")));
    MarkdownEditor *editor = window.editor();
    editor->enterBlockAt(2);
    for (int i = 0; i < 7; ++i)
        QTest::keyClick(editor, Qt::Key_Right, Qt::ShiftModifier); // "Welcome"
    QTest::keyClick(editor, Qt::Key_B, Qt::ControlModifier | Qt::ShiftModifier);
    EXPECT_TRUE(editor->noteText().startsWith(QLatin1String("# **Welcome**")));
}

TEST_F(WindowTest, APagePreviewOpensTheLinkedNoteAtItsHeading)
{
    write(QStringLiteral("Other.md"), "# Other\n\nIntro.\n\n## Part two\n\nSecond part.\n");
    write(QStringLiteral("Linking.md"), "See [[Other#Part two]].\n");
    MainWindow window;
    window.openVault(dir.path());
    QTRY_VERIFY_WITH_TIMEOUT(!window.findChildren<QAction *>().isEmpty(), 1000);
    window.openNote(path(QStringLiteral("Linking.md")));

    // What the editor sends when the pointer rests on the link with Ctrl held.
    emit window.editor()->linkHovered(QStringLiteral("Other#Part two"), QPoint(200, 200));
    auto *preview = window.findChild<noteAppC::PagePreview *>(QStringLiteral("pagePreview"));
    ASSERT_NE(preview, nullptr);
    QTRY_VERIFY_WITH_TIMEOUT(preview->isVisible(), 2000);
    const QString shown = preview->editor()->noteText();
    EXPECT_TRUE(shown.startsWith(QLatin1String("# Other")));
    EXPECT_EQ(preview->editor()->cursorOffset(), shown.indexOf(QLatin1String("## Part two")));
    EXPECT_EQ(preview->editor()->mode(), MarkdownEditor::Mode::Reading);

    // Moving off the link lets it go, after a moment to reach the preview.
    emit window.editor()->linkHoverEnded();
    QTRY_VERIFY_WITH_TIMEOUT(!preview->isVisible(), 2000);
}

TEST_F(WindowTest, SavingKeepsSnapshotsAndOneCanBePutBack)
{
    QTemporaryDir data;
    const QByteArray previous = qgetenv("XDG_DATA_HOME");
    qputenv("XDG_DATA_HOME", data.path().toUtf8());
    MainWindow window;
    window.openVault(dir.path());
    window.openNote(path(QStringLiteral("Welcome.md")));
    MarkdownEditor *editor = window.editor();
    editor->setCursorOffset(editor->noteText().size());
    editor->enterBlockAt(editor->noteText().size());
    QTest::keyClicks(editor, QStringLiteral(" Changed."));
    ASSERT_TRUE(window.findChild<QAction *>(QStringLiteral("editor:save-file")) != nullptr);
    window.findChild<QAction *>(QStringLiteral("editor:save-file"))->trigger();

    // The first save keeps what the note held before it, as well as the save.
    const QList<noteAppC::Snapshots::Version> versions =
        noteAppC::Snapshots::versions(path(QStringLiteral("Welcome.md")));
    ASSERT_GE(versions.size(), 1);
    EXPECT_EQ(noteAppC::Snapshots::read(versions.last()), QStringLiteral("# Welcome\n\nBody.\n"));

    // Opened from the settings page; restoring writes the file and the editor.
    window.findChild<QAction *>(QStringLiteral("file-recovery:open"))->trigger();
    auto *dialog = window.findChild<noteAppC::RecoveryDialog *>();
    ASSERT_NE(dialog, nullptr);
    emit dialog->restoreRequested(path(QStringLiteral("Welcome.md")),
                                  noteAppC::Snapshots::read(versions.last()));
    EXPECT_EQ(readAll(path(QStringLiteral("Welcome.md"))), QByteArray("# Welcome\n\nBody.\n"));
    EXPECT_EQ(editor->noteText(), QStringLiteral("# Welcome\n\nBody.\n"));
    qputenv("XDG_DATA_HOME", previous);
}

// --- core plugins in the window ---------------------------------------------------

TEST_F(WindowTest, TheOutlineShowsTheNotesHeadingsAndJumpsToThem)
{
    write(QStringLiteral("Outlined.md"), "# Top\n\nText.\n\n## Second\n\nMore.\n");
    MainWindow window;
    window.openVault(dir.path());
    window.openNote(path(QStringLiteral("Outlined.md")));
    auto *outline = window.findChild<noteAppC::OutlinePane *>();
    ASSERT_NE(outline, nullptr);
    auto *tree = outline->findChild<QTreeWidget *>();
    ASSERT_NE(tree, nullptr);
    ASSERT_EQ(tree->topLevelItemCount(), 1);
    EXPECT_EQ(tree->topLevelItem(0)->text(0), QStringLiteral("Top"));
    ASSERT_EQ(tree->topLevelItem(0)->childCount(), 1);

    emit outline->jumpRequested(QStringLiteral("# Top\n\nText.\n\n").size());
    EXPECT_EQ(window.editor()->cursorOffset(), QStringLiteral("# Top\n\nText.\n\n").size());
}

TEST_F(WindowTest, TodaysDailyNoteIsMadeWhereTheVaultSays)
{
    ASSERT_TRUE(QDir(dir.path()).mkpath(QStringLiteral(".obsidian")));
    write(QStringLiteral(".obsidian/daily-notes.json"), R"({"folder": "Journal", "format": "YYYY-MM-DD"})");
    MainWindow window;
    window.openVault(dir.path());
    window.findChild<QAction *>(QStringLiteral("daily-notes"))->trigger();
    const QString today = path(QStringLiteral("Journal/%1.md")
                                   .arg(QDate::currentDate().toString(QStringLiteral("yyyy-MM-dd"))));
    EXPECT_TRUE(QFileInfo::exists(today));
    EXPECT_TRUE(window.windowTitle().contains(QDate::currentDate().toString(QStringLiteral("yyyy-MM-dd"))));
}

TEST_F(WindowTest, ATemplateGoesInAtTheCaret)
{
    ASSERT_TRUE(QDir(dir.path()).mkpath(QStringLiteral(".obsidian")));
    ASSERT_TRUE(QDir(dir.path()).mkpath(QStringLiteral("Templates")));
    write(QStringLiteral(".obsidian/templates.json"), R"({"folder": "Templates"})");
    write(QStringLiteral("Templates/Meeting.md"), "Notes on {{title}}\n");
    MainWindow window;
    window.openVault(dir.path());
    window.openNote(path(QStringLiteral("Welcome.md")));
    window.editor()->setCursorOffset(0);
    window.findChild<QAction *>(QStringLiteral("insert-template"))->trigger();
    auto *query = window.findChild<QLineEdit *>(QStringLiteral("templatePickerQuery"));
    ASSERT_NE(query, nullptr);
    QTest::keyClick(query, Qt::Key_Return); // the one template there is
    EXPECT_TRUE(window.editor()->noteText().startsWith(QLatin1String("Notes on Welcome\n")));
}

TEST_F(WindowTest, BookmarkingANoteKeepsItOutsideTheVault)
{
    QTemporaryDir config;
    const QByteArray previous = qgetenv("XDG_CONFIG_HOME");
    qputenv("XDG_CONFIG_HOME", config.path().toUtf8());
    MainWindow window;
    window.openVault(dir.path());
    window.openNote(path(QStringLiteral("Welcome.md")));
    window.findChild<QAction *>(QStringLiteral("bookmarks:bookmark-current-view"))->trigger();
    auto *tree = window.findChild<noteAppC::BookmarksPane *>()->findChild<QTreeWidget *>();
    ASSERT_NE(tree, nullptr);
    EXPECT_EQ(tree->topLevelItemCount(), 1);
    EXPECT_FALSE(QFileInfo::exists(path(QStringLiteral(".obsidian/bookmarks.json"))))
        << "the vault is never written";
    qputenv("XDG_CONFIG_HOME", previous);
}

