// Widget smoke test: drives the real MainWindow on the offscreen platform, so
// the Phase 0 exit criteria (open vault, file tree, open note, save) are
// checked by something other than a human looking at the screen.

#include <gtest/gtest.h>

#include <algorithm>
#include <unistd.h>
#include <cstdio>
#include <functional>

#include <QDialogButtonBox>
#include <QAbstractButton>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QPlainTextEdit>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFileSystemModel>
#include <QFile>
#include <QFileInfo>
#include <QInputDialog>
#include <QLabel>
#include <QLayout>
#include <QLineEdit>
#include <QMenu>
#include <QDialog>
#include <QPushButton>
#include <QPainter>
#include <QPdfWriter>
#include <QImage>
#include <QListWidget>
#include <QMessageBox>
#include <QScrollBar>
#include <QSplitter>
#include <QStackedWidget>
#include <QStatusBar>
#include <QTabBar>
#include <QTest>
#include <QTimer>
#include <QToolButton>
#include <QWheelEvent>

#include "WaitFor.h"
#include "MarkdownEditor.h"
#include "FootnotesPane.h"
#include "PropertiesPane.h"
#include "SlidesView.h"
#ifdef NOTEAPPC_HAVE_IMAGEVIEW
#include "ImageView.h"
#endif
#ifdef NOTEAPPC_HAVE_GRAPH
#include "GraphView.h"
#endif
#ifdef NOTEAPPC_HAVE_CANVAS
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include "CanvasView.h"
#endif
#ifdef NOTEAPPC_HAVE_BASES
#include <QTableView>
#include "BaseView.h"
#endif
#include <QSettings>
#include <QDropEvent>
#include <QMimeData>
#include <QClipboard>
#include <QSignalSpy>
#include <QAction>
#include <QDate>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QUuid>
#include <QTreeView>
#include <QTreeWidget>

#include "Index.h"
#include "CodeHighlighter.h"
#include "HtmlRenderer.h"
#include "MathRenderer.h"
#include "PdfPage.h"
#include "PublishDialog.h"
#include "MainWindow.h"
#include "PagePreview.h"
#include "OutlinePane.h"
#include "BookmarksPane.h"
#include "BacklinksPane.h"
#include "ToggleSwitch.h"
#include "QuickSwitcher.h"
#include "Snapshots.h"
#include "RecoveryDialog.h"
#include "Settings.h"
#ifdef NOTEAPPC_EXPECT_SYNC
#include "SyncEngine.h"
#endif
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

    // A hidden Tags pane only marks itself stale; showing it rebuilds.
    auto *hiddenList = window.findChild<QWidget *>(QStringLiteral("tagsPane"))->findChild<QTreeWidget *>();
    ASSERT_NE(hiddenList, nullptr);
    EXPECT_EQ(hiddenList->topLevelItemCount(), 0);
    window.show();
    auto *tagsTab = window.findChild<QToolButton *>(QStringLiteral("tagsTabButton"));
    ASSERT_NE(tagsTab, nullptr);
    tagsTab->click();
    auto *tagsPane = window.findChild<QWidget *>(QStringLiteral("tagsPane"));
    ASSERT_NE(tagsPane, nullptr);
    ASSERT_TRUE(tagsPane->isVisible());
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

TEST_F(WindowTest, ClickingATagSearchesForItAndAResultOpensTheNoteAtTheMatch)
{
    write(QStringLiteral("A.md"), "Intro line.\n#project #project/alpha\n");
    write(QStringLiteral("B.md"), "#project\n");
    write(QStringLiteral("C.md"), "#area\n");

    MainWindow window;
    QSignalSpy indexed(window.index(), &Index::changed);
    window.openVault(dir.path());
    ASSERT_TRUE(indexed.wait(5000));

    window.show();
    window.findChild<QToolButton *>(QStringLiteral("tagsTabButton"))->click(); // showing rebuilds a stale pane
    auto *tags = window.findChild<QWidget *>(QStringLiteral("tagsPane"))->findChild<QTreeWidget *>();
    ASSERT_NE(tags, nullptr);
    emit tags->itemClicked(tags->topLevelItem(0)->child(0), 0); // project/alpha

    auto *search = window.findChild<QWidget *>(QStringLiteral("searchPane"));
    ASSERT_NE(search, nullptr);
    auto *panes = window.findChild<QStackedWidget *>(QStringLiteral("rightSidebarPanes"));
    EXPECT_EQ(panes->currentWidget(), search);
    EXPECT_TRUE(window.findChild<QToolButton *>(QStringLiteral("searchTabButton"))->isChecked());
    EXPECT_EQ(search->findChild<QLineEdit *>()->text(), QStringLiteral("tag:#project/alpha"));

    auto *results = search->findChild<QTreeWidget *>();
    ASSERT_TRUE(QTest::qWaitFor([&] { return results->topLevelItemCount() == 1; }, 5000));
    EXPECT_EQ(results->topLevelItem(0)->data(0, Qt::UserRole).toString(), QStringLiteral("A.md"));

    emit results->itemClicked(results->topLevelItem(0), 0);
    auto *editor = window.findChild<MarkdownEditor *>();
    EXPECT_EQ(editor->noteText(), QStringLiteral("Intro line.\n#project #project/alpha\n"));
    EXPECT_EQ(editor->cursorOffset(), 21); // at "#project/alpha", not the note's start
}

TEST_F(WindowTest, SearchInAllFilesShowsTheSearchPaneAndFocusesItsBox)
{
    write(QStringLiteral("A.md"), "text\n");
    MainWindow window;
    window.openVault(dir.path());
    window.show();
    ASSERT_TRUE(QTest::qWaitForWindowExposed(&window));

    auto *command = window.findChild<QAction *>(QStringLiteral("global-search:open"));
    ASSERT_NE(command, nullptr);
    EXPECT_EQ(command->shortcut(), QKeySequence(QStringLiteral("Ctrl+Shift+F")));
    command->trigger();

    auto *search = window.findChild<QWidget *>(QStringLiteral("searchPane"));
    EXPECT_TRUE(search->isVisible());
    EXPECT_EQ(QApplication::focusWidget(), search->findChild<QLineEdit *>());
}

#ifdef NOTEAPPC_HAVE_GRAPH
using noteAppC::GraphView;

TEST_F(WindowTest, GraphViewOpensInTheMainAreaAndANodeOpensItsNote)
{
    write(QStringLiteral("A.md"), "[[B]]\n");
    write(QStringLiteral("B.md"), "b\n");
    MainWindow window;
    QSignalSpy indexed(window.index(), &Index::changed);
    window.openVault(dir.path());
    ASSERT_TRUE(indexed.wait(5000));
    window.show();
    ASSERT_TRUE(QTest::qWaitForWindowExposed(&window));

    EXPECT_EQ(window.findChild<GraphView *>(QStringLiteral("graphView")), nullptr); // not before first use
    auto *command = window.findChild<QAction *>(QStringLiteral("graph:open"));
    ASSERT_NE(command, nullptr);
    command->trigger();
    auto *graph = window.findChild<GraphView *>(QStringLiteral("graphView"));
    ASSERT_NE(graph, nullptr);
    auto *stack = window.findChild<QStackedWidget *>(QStringLiteral("mainStack"));
    EXPECT_EQ(stack->currentWidget(), graph);
    EXPECT_TRUE(graph->containsNode(QStringLiteral("A.md")));
    EXPECT_TRUE(graph->containsNode(QStringLiteral("B.md")));

    emit graph->noteActivated(QStringLiteral("B.md"));
    EXPECT_EQ(stack->currentIndex(), 0);
    EXPECT_EQ(window.findChild<MarkdownEditor *>()->noteText(), QStringLiteral("b\n"));

    command->trigger();
    EXPECT_EQ(stack->currentWidget(), graph);
    command->trigger(); // again: back to the note
    EXPECT_EQ(stack->currentIndex(), 0);
}

TEST_F(WindowTest, LocalGraphFollowsTheOpenNote)
{
    write(QStringLiteral("A.md"), "[[B]]\n");
    write(QStringLiteral("B.md"), "[[C]]\n");
    write(QStringLiteral("C.md"), "c\n");
    MainWindow window;
    QSignalSpy indexed(window.index(), &Index::changed);
    window.openVault(dir.path());
    ASSERT_TRUE(indexed.wait(5000));
    window.show();
    ASSERT_TRUE(QTest::qWaitForWindowExposed(&window));
    window.openNote(path(QStringLiteral("A.md")));

    window.findChild<QAction *>(QStringLiteral("graph:open-local"))->trigger();
    auto *local = window.findChild<GraphView *>(QStringLiteral("localGraph"));
    ASSERT_NE(local, nullptr);
    EXPECT_TRUE(local->isVisible());
    EXPECT_TRUE(window.findChild<QToolButton *>(QStringLiteral("localGraphTabButton"))->isChecked());
    EXPECT_TRUE(local->containsNode(QStringLiteral("B.md")));
    EXPECT_FALSE(local->containsNode(QStringLiteral("C.md"))); // two hops away

    window.openNote(path(QStringLiteral("C.md")));
    EXPECT_TRUE(local->containsNode(QStringLiteral("B.md")));
    EXPECT_FALSE(local->containsNode(QStringLiteral("A.md")));
}
#endif

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

namespace {

QStringList tabTexts(QTabBar *tabs)
{
    QStringList texts;
    for (int i = 0; i < tabs->count(); ++i)
        texts << tabs->tabText(i);
    return texts;
}

void flushDeletes()
{
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}

/// The tabs' editors: the page preview has one of its own.
QList<MarkdownEditor *> tabEditors(MainWindow &window)
{
    return window.findChild<QSplitter *>(QStringLiteral("workspace"))->findChildren<MarkdownEditor *>();
}

} // namespace

TEST_F(WindowTest, TabsEachKeepTheirOwnNoteAndCloseToTheirNeighbour)
{
    MainWindow window;
    window.openVault(dir.path());
    window.openNote(path(QStringLiteral("Welcome.md")));
    auto *tabs = window.findChild<QTabBar *>(QStringLiteral("noteTabs"));

    window.findChild<QAction *>(QStringLiteral("workspace:new-tab"))->trigger();
    EXPECT_EQ(tabTexts(tabs), (QStringList{QStringLiteral("Welcome"), QStringLiteral("New tab")}));
    EXPECT_EQ(tabs->currentIndex(), 1);
    window.openNote(path(QStringLiteral("Projects/Crlf.md")));
    MarkdownEditor *second = window.editor();
    EXPECT_EQ(tabTexts(tabs), (QStringList{QStringLiteral("Welcome"), QStringLiteral("Crlf")}));
    EXPECT_EQ(tabEditors(window).size(), 2);

    tabs->setCurrentIndex(0); // a click on the first tab
    EXPECT_EQ(window.editor()->noteText(), QStringLiteral("# Welcome\n\nBody.\n"));
    EXPECT_TRUE(window.windowTitle().startsWith(QStringLiteral("Welcome.md")));
    window.findChild<QAction *>(QStringLiteral("workspace:next-tab"))->trigger();
    EXPECT_EQ(window.editor()->noteText(), QStringLiteral("one\ntwo\n"));

    // An edit in a tab in the background still marks its own tab.
    tabs->setCurrentIndex(0);
    second->setCursorOffset(0);
    QTest::keyClicks(second, QStringLiteral("x"));
    EXPECT_EQ(tabs->tabText(1), QStringLiteral("Crlf *"));

    second->setModified(false);
    window.findChild<QAction *>(QStringLiteral("workspace:close"))->trigger(); // Welcome
    flushDeletes();
    EXPECT_EQ(tabTexts(tabs), QStringList{QStringLiteral("Crlf")});
    EXPECT_EQ(window.editor(), second);
    EXPECT_EQ(tabEditors(window).size(), 1);
}

TEST_F(WindowTest, SplitRightShowsTheSameNoteAndFollowsItsSave)
{
    MainWindow window;
    window.openVault(dir.path());
    window.show();
    ASSERT_TRUE(QTest::qWaitForWindowExposed(&window));
    window.openNote(path(QStringLiteral("Welcome.md")));
    MarkdownEditor *left = window.editor();

    window.findChild<QAction *>(QStringLiteral("workspace:split-vertical"))->trigger();
    QList<QTabBar *> bars = window.findChildren<QTabBar *>(QStringLiteral("noteTabs"));
    ASSERT_EQ(bars.size(), 2);
    // Left to right: child order is creation order, not screen order.
    std::sort(bars.begin(), bars.end(), [&window](QTabBar *a, QTabBar *b) {
        return a->mapTo(&window, QPoint()).x() < b->mapTo(&window, QPoint()).x();
    });
    MarkdownEditor *right = window.editor();
    ASSERT_NE(right, left);
    EXPECT_EQ(right->noteText(), left->noteText());
    EXPECT_GT(right->mapTo(&window, QPoint()).x(), left->mapTo(&window, QPoint()).x());
    EXPECT_EQ(tabTexts(bars.at(1)), QStringList{QStringLiteral("Welcome")});

    // Saving one split's edit reloads the other, which has none of its own.
    right->setCursorOffset(0);
    QTest::keyClicks(right, QStringLiteral("x"));
    ASSERT_TRUE(QMetaObject::invokeMethod(&window, "save"));
    EXPECT_EQ(left->noteText(), QStringLiteral("x# Welcome\n\nBody.\n"));

    EXPECT_FALSE(bars.at(0)->property("focusedSplit").toBool());
    EXPECT_TRUE(bars.at(1)->property("focusedSplit").toBool());

    // Focus decides which split the window follows, and which tab is marked.
    left->setFocus();
    QApplication::processEvents();
    EXPECT_EQ(window.editor(), left);
    EXPECT_TRUE(bars.at(0)->property("focusedSplit").toBool());
    EXPECT_FALSE(bars.at(1)->property("focusedSplit").toBool());

    // Closing a split's last tab closes the split.
    right->setFocus();
    QApplication::processEvents();
    window.findChild<QAction *>(QStringLiteral("workspace:close"))->trigger();
    flushDeletes();
    EXPECT_EQ(window.findChildren<QTabBar *>(QStringLiteral("noteTabs")).size(), 1);
    EXPECT_EQ(window.editor(), left);
}

TEST_F(WindowTest, SplitDownThenRightNestsTheSplits)
{
    MainWindow window;
    window.openVault(dir.path());
    window.openNote(path(QStringLiteral("Welcome.md")));
    window.findChild<QAction *>(QStringLiteral("workspace:split-horizontal"))->trigger();
    window.findChild<QAction *>(QStringLiteral("workspace:split-vertical"))->trigger();
    EXPECT_EQ(window.findChildren<QTabBar *>(QStringLiteral("noteTabs")).size(), 3);
    auto *root = window.findChild<QSplitter *>(QStringLiteral("workspace"));
    ASSERT_NE(root, nullptr);
    EXPECT_EQ(root->orientation(), Qt::Vertical);
    ASSERT_EQ(root->count(), 2);
    auto *nested = qobject_cast<QSplitter *>(root->widget(1));
    ASSERT_NE(nested, nullptr);
    EXPECT_EQ(nested->orientation(), Qt::Horizontal);
    EXPECT_EQ(nested->count(), 2);

    // Closing the newest collapses the nested split back into the root.
    window.findChild<QAction *>(QStringLiteral("workspace:close"))->trigger();
    flushDeletes();
    EXPECT_EQ(root->count(), 2);
    EXPECT_EQ(qobject_cast<QSplitter *>(root->widget(1)), nullptr);
}

TEST_F(WindowTest, DeletingANoteClosesEveryTabShowingIt)
{
    write(QStringLiteral("Other.md"), "other\n");
    MainWindow window;
    window.openVault(dir.path());
    window.openNote(path(QStringLiteral("Other.md")));
    window.findChild<QAction *>(QStringLiteral("workspace:new-tab"))->trigger();
    window.openNote(path(QStringLiteral("Welcome.md")));
    window.findChild<QAction *>(QStringLiteral("workspace:split-vertical"))->trigger();
    ASSERT_EQ(tabEditors(window).size(), 3);

    auto *files = window.findChild<QFileSystemModel *>();
    const QModelIndex welcome = files->index(path(QStringLiteral("Welcome.md")));
    ASSERT_TRUE(welcome.isValid());
    QTimer::singleShot(0, [] {
        if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget()))
            box->button(QMessageBox::Yes)->click();
    });
    ASSERT_TRUE(QMetaObject::invokeMethod(&window, "deletePathAt", Q_ARG(QModelIndex, welcome)));
    flushDeletes();
    EXPECT_EQ(tabEditors(window).size(), 1);
    EXPECT_EQ(window.editor()->noteText(), QStringLiteral("other\n"));
}

#ifdef NOTEAPPC_HAVE_CANVAS
TEST_F(WindowTest, ACanvasOpensInATabAndSavesItsEdits)
{
    write(QStringLiteral("Board.canvas"),
          R"({"nodes":[{"id":"a","type":"text","text":"hi","x":0,"y":0,"width":200,"height":100}],"edges":[]})");
    MainWindow window;
    window.openVault(dir.path());
    ASSERT_TRUE(window.openNote(path(QStringLiteral("Board.canvas"))));

    auto *canvas = window.findChild<noteAppC::CanvasView *>();
    ASSERT_NE(canvas, nullptr);
    auto *tabs = window.findChild<QTabBar *>(QStringLiteral("noteTabs"));
    EXPECT_EQ(tabs->tabText(0), QStringLiteral("Board"));
    EXPECT_TRUE(window.editor()->isHidden());

    canvas->createTextNodeAt(QPointF(400, 0));
    EXPECT_EQ(tabs->tabText(0), QStringLiteral("Board *"));
    EXPECT_TRUE(window.windowTitle().startsWith(QLatin1Char('*')));
    ASSERT_TRUE(QMetaObject::invokeMethod(&window, "save"));
    EXPECT_EQ(tabs->tabText(0), QStringLiteral("Board"));
    const QJsonArray nodes =
        QJsonDocument::fromJson(readAll(path(QStringLiteral("Board.canvas")))).object().value(QStringLiteral("nodes")).toArray();
    ASSERT_EQ(nodes.size(), 2);
    EXPECT_EQ(nodes.at(0).toObject().value(QStringLiteral("text")).toString(), QStringLiteral("hi"));

    // A note in the same tab puts the editor back.
    window.openNote(path(QStringLiteral("Welcome.md")));
    flushDeletes();
    EXPECT_EQ(window.findChild<noteAppC::CanvasView *>(), nullptr);
    EXPECT_FALSE(window.editor()->isHidden());
    EXPECT_EQ(window.editor()->noteText(), QStringLiteral("# Welcome\n\nBody.\n"));
}

TEST_F(WindowTest, ACanvasEmbedsFileCardOpensItsNoteOnAPlainClick)
{
    write(QStringLiteral("Target.md"), "# Target\n\nBody.\n");
    write(QStringLiteral("Board.canvas"),
          R"({"nodes":[{"id":"a","type":"file","file":"Target.md","x":0,"y":0,"width":200,"height":100}],)"
          R"("edges":[]})");
    write(QStringLiteral("Note.md"), "![[Board.canvas]]\n");
    MainWindow window;
    window.openVault(dir.path());
    ASSERT_TRUE(window.openNote(path(QStringLiteral("Note.md"))));

    MarkdownEditor *editor = window.editor();
    WAIT_TRUE(!editor->embedImageSizeAt(0).isEmpty(), 2000);
    const QRect rect = editor->hookedEmbedImageRectAt(0);
    ASSERT_FALSE(rect.isEmpty());

    // A Ctrl+wheel over the embed is accepted: it zoomed the embed rather
    // than leaving the note's own scroll stuck or moving it instead.
    QWheelEvent zoomWheel(QPointF(rect.center()), QPointF(rect.center()), QPoint(0, 0), QPoint(0, 240),
                         Qt::NoButton, Qt::ControlModifier, Qt::NoScrollPhase, false);
    QCoreApplication::sendEvent(editor->viewport(), &zoomWheel);
    EXPECT_TRUE(zoomWheel.isAccepted());

    // fitToContent() centres the sole node in the embed's frame, so its own
    // centre is inside the card regardless of exactly how it was sized.
    QTest::mouseClick(editor->viewport(), Qt::LeftButton, Qt::NoModifier, rect.center());
    EXPECT_EQ(window.editor()->noteText(), QStringLiteral("# Target\n\nBody.\n"));
}

TEST_F(WindowTest, DoubleClickingEmptyCanvasEmbedSpaceOpensTheCanvasFileItself)
{
    write(QStringLiteral("Target.md"), "# Target\n\nBody.\n");
    write(QStringLiteral("Board.canvas"),
          R"({"nodes":[{"id":"a","type":"file","file":"Target.md","x":0,"y":0,"width":200,"height":100}],)"
          R"("edges":[]})");
    write(QStringLiteral("Note.md"), "![[Board.canvas]]\n");
    MainWindow window;
    window.openVault(dir.path());
    ASSERT_TRUE(window.openNote(path(QStringLiteral("Note.md"))));

    MarkdownEditor *editor = window.editor();
    WAIT_TRUE(!editor->embedImageSizeAt(0).isEmpty(), 2000);
    const QRect rect = editor->hookedEmbedImageRectAt(0);
    ASSERT_FALSE(rect.isEmpty());

    // fitToContent() always leaves a margin around the content, so a corner
    // is background no matter how the sole node above ended up sized.
    QTest::mouseDClick(editor->viewport(), Qt::LeftButton, Qt::NoModifier, rect.topLeft() + QPoint(5, 5));

    auto *canvas = window.findChild<noteAppC::CanvasView *>();
    ASSERT_NE(canvas, nullptr);
    EXPECT_EQ(canvas->path(), path(QStringLiteral("Board.canvas")));
}
#endif

#ifdef NOTEAPPC_HAVE_BASES
TEST_F(WindowTest, ABaseOpensInATabAndItsRowsOpenNotes)
{
    write(QStringLiteral("Task.md"), "---\nstatus: open\n---\nDo it.\n");
    write(QStringLiteral("Tasks.base"),
          "filters:\n  and:\n    - status == \"open\"\nviews:\n  - type: table\n    name: Open\n"
          "    order:\n      - file.name\n      - status\n");
    MainWindow window;
    QSignalSpy indexed(window.index(), &Index::changed);
    window.openVault(dir.path());
    ASSERT_TRUE(indexed.wait(5000));
    ASSERT_TRUE(window.openNote(path(QStringLiteral("Tasks.base"))));

    auto *base = window.findChild<noteAppC::bases::BaseView *>();
    ASSERT_NE(base, nullptr);
    auto *table = base->findChild<QTableView *>();
    ASSERT_NE(table, nullptr);
    // Evaluated on the pool: the rows come a moment after the tab.
    WAIT_TRUE(table->model() && table->model()->rowCount() == 1, 5000);

    emit base->noteActivated(QStringLiteral("Task.md"));
    EXPECT_EQ(window.editor()->noteText(), QStringLiteral("---\nstatus: open\n---\nDo it.\n"));
    EXPECT_FALSE(window.editor()->isHidden());
}
#endif

TEST_F(WindowTest, TheLayoutComesBackWhenTheVaultIsReopenedWithBackgroundTabsDeferred)
{
    {
        MainWindow window;
        window.openVault(dir.path());
        window.openNote(path(QStringLiteral("Welcome.md")));
        window.findChild<QAction *>(QStringLiteral("workspace:split-vertical"))->trigger();
        window.findChild<QAction *>(QStringLiteral("workspace:new-tab"))->trigger();
        window.openNote(path(QStringLiteral("Projects/Crlf.md")));
        window.close();
    }

    MainWindow window;
    window.openVault(dir.path());
    auto *root = window.findChild<QSplitter *>(QStringLiteral("workspace"));
    ASSERT_EQ(root->count(), 2);
    const QList<QTabBar *> bars{root->widget(0)->findChild<QTabBar *>(),
                                root->widget(1)->findChild<QTabBar *>()};
    EXPECT_EQ(tabTexts(bars.at(0)), QStringList{QStringLiteral("Welcome")});
    EXPECT_EQ(tabTexts(bars.at(1)), (QStringList{QStringLiteral("Welcome"), QStringLiteral("Crlf")}));
    EXPECT_EQ(bars.at(1)->currentIndex(), 1);
    EXPECT_EQ(window.editor()->noteText(), QStringLiteral("one\ntwo\n"));

    // The right split's first tab is in the background: not read yet.
    int loaded = 0;
    for (MarkdownEditor *editor : tabEditors(window))
        loaded += !editor->noteText().isEmpty();
    EXPECT_EQ(loaded, 2);
    bars.at(1)->setCurrentIndex(0);
    EXPECT_EQ(window.editor()->noteText(), QStringLiteral("# Welcome\n\nBody.\n"));
}

TEST_F(WindowTest, ObsidiansOwnLayoutIsReadAndNeverWritten)
{
    ASSERT_TRUE(QDir(dir.path()).mkpath(QStringLiteral(".obsidian")));
    const QByteArray obsidian = R"({
  "main": {"id": "5f", "type": "split", "direction": "horizontal", "children": [
    {"id": "a1", "type": "tabs", "children": [
      {"id": "b1", "type": "leaf", "state": {"type": "markdown", "state": {"file": "Welcome.md", "mode": "source"}}}]},
    {"id": "a2", "type": "tabs", "children": [
      {"id": "b2", "type": "leaf", "state": {"type": "markdown", "state": {"file": "Projects/Crlf.md"}}}]}]},
  "left": {"id": "l", "type": "split", "children": [], "collapsed": true},
  "right": {"id": "r", "type": "split", "children": [], "collapsed": false},
  "active": "b1",
  "lastOpenFiles": []
})";
    write(QStringLiteral(".obsidian/workspace.json"), obsidian);
    {
        MainWindow window;
        window.openVault(dir.path());
        auto *root = window.findChild<QSplitter *>(QStringLiteral("workspace"));
        EXPECT_EQ(root->orientation(), Qt::Vertical); // "horizontal": stacked
        EXPECT_EQ(root->count(), 2);
        EXPECT_EQ(window.editor()->noteText(), QStringLiteral("# Welcome\n\nBody.\n"));
        EXPECT_TRUE(window.findChild<QWidget *>(QStringLiteral("sidebar"))->isHidden())
            << "left.collapsed hides the file explorer";
        window.findChild<QAction *>(QStringLiteral("workspace:close"))->trigger();
        window.close();
    }
    EXPECT_EQ(readAll(path(QStringLiteral(".obsidian/workspace.json"))), obsidian);
    // noteAppC's own copy now wins: the tab closed above stays closed.
    MainWindow window;
    window.openVault(dir.path());
    EXPECT_EQ(window.findChildren<QTabBar *>(QStringLiteral("noteTabs")).size(), 1);
}

TEST_F(WindowTest, NamedLayoutsSaveAndLoad)
{
    MainWindow window;
    window.openVault(dir.path());
    window.openNote(path(QStringLiteral("Welcome.md")));

    const auto inDialog = [](const std::function<void(QDialog *)> &act) {
        QTimer::singleShot(0, [act] {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            ASSERT_NE(dialog, nullptr);
            act(dialog);
            if (dialog->isVisible())
                dialog->reject();
        });
    };
    inDialog([](QDialog *dialog) {
        dialog->findChild<QLineEdit *>(QStringLiteral("workspaceName"))->setText(QStringLiteral("Solo"));
        dialog->findChild<QPushButton *>(QStringLiteral("workspaceSave"))->click();
    });
    window.findChild<QAction *>(QStringLiteral("workspaces:open-modal"))->trigger();

    window.findChild<QAction *>(QStringLiteral("workspace:split-vertical"))->trigger();
    window.findChild<QAction *>(QStringLiteral("workspace:split-horizontal"))->trigger();
    ASSERT_EQ(window.findChildren<QTabBar *>(QStringLiteral("noteTabs")).size(), 3);

    inDialog([](QDialog *dialog) {
        auto *list = dialog->findChild<QListWidget *>(QStringLiteral("workspaceList"));
        ASSERT_EQ(list->count(), 1);
        list->setCurrentRow(0);
        dialog->findChild<QPushButton *>(QStringLiteral("workspaceLoad"))->click();
    });
    window.findChild<QAction *>(QStringLiteral("workspaces:open-modal"))->trigger();
    flushDeletes();
    EXPECT_EQ(window.findChildren<QTabBar *>(QStringLiteral("noteTabs")).size(), 1);
    EXPECT_EQ(window.editor()->noteText(), QStringLiteral("# Welcome\n\nBody.\n"));
}

TEST_F(WindowTest, AQueryBlockShowsTheVaultsSearchResults)
{
    write(QStringLiteral("Tagged.md"), "#project here\n");
    write(QStringLiteral("Board.md"), "Intro\n\n```query\ntag:#project\n```\n");
    MainWindow window;
    QSignalSpy indexed(window.index(), &Index::changed);
    window.openVault(dir.path());
    ASSERT_TRUE(indexed.wait(5000));
    window.show(); // a query runs when its block is laid out
    ASSERT_TRUE(QTest::qWaitForWindowExposed(&window));
    window.openNote(path(QStringLiteral("Board.md")));
    MarkdownEditor *editor = window.editor();
    editor->setCursorOffset(0); // in "Intro", so the query block renders

    const qsizetype query = editor->noteText().indexOf(QStringLiteral("tag:"));
    ASSERT_TRUE(QTest::qWaitFor([&] { return !editor->queryRowsAt(query).isEmpty(); }, 5000));
    const auto rows = editor->queryRowsAt(query);
    ASSERT_EQ(rows.size(), 1);
    EXPECT_EQ(rows.first().path, QStringLiteral("Tagged.md"));

    emit editor->queryResultActivated(rows.first().path, rows.first().offset);
    EXPECT_EQ(window.editor()->noteText(), QStringLiteral("#project here\n"));
}

#ifdef NOTEAPPC_HAVE_BASES
TEST_F(WindowTest, ABaseBlockListsTheVaultsNotes)
{
    write(QStringLiteral("Task.md"), "---\nstatus: open\n---\nDo it.\n");
    write(QStringLiteral("Board.md"), "Intro\n\n```base\nfilters:\n  and:\n    - status == \"open\"\n"
                                      "views:\n  - type: table\n    name: Open\n```\n");
    MainWindow window;
    QSignalSpy indexed(window.index(), &Index::changed);
    window.openVault(dir.path());
    ASSERT_TRUE(indexed.wait(5000));
    window.show(); // a block's work happens when it is laid out
    ASSERT_TRUE(QTest::qWaitForWindowExposed(&window));
    window.openNote(path(QStringLiteral("Board.md")));
    MarkdownEditor *editor = window.editor();
    editor->setCursorOffset(0);
    const qsizetype block = editor->noteText().indexOf(QStringLiteral("filters"));
    ASSERT_TRUE(QTest::qWaitFor([&] { return !editor->baseRowsAt(block).isEmpty(); }, 5000));
    const auto rows = editor->baseRowsAt(block);
    ASSERT_EQ(rows.size(), 1);
    EXPECT_EQ(rows.first().path, QStringLiteral("Task.md"));
    emit editor->baseRowActivated(rows.first().path);
    EXPECT_EQ(window.editor()->noteText(), QStringLiteral("---\nstatus: open\n---\nDo it.\n"));
}
#endif

TEST_F(WindowTest, AMiddleClickOnATreeRowOpensItInANewTab)
{
    MainWindow window;
    QSignalSpy loaded(window.findChild<QFileSystemModel *>(), &QFileSystemModel::directoryLoaded);
    window.openVault(dir.path());
    window.show();
    ASSERT_TRUE(QTest::qWaitForWindowExposed(&window));
    ASSERT_TRUE(!loaded.isEmpty() || loaded.wait(5000)); // may land while exposing
    window.openNote(path(QStringLiteral("Projects/Crlf.md")));

    auto *tree = window.findChild<QTreeView *>(QStringLiteral("fileExplorer"));
    auto *model = qobject_cast<QFileSystemModel *>(tree->model());
    const QModelIndex welcome = model->index(path(QStringLiteral("Welcome.md")));
    ASSERT_TRUE(welcome.isValid());
    QTest::mouseClick(tree->viewport(), Qt::MiddleButton, {}, tree->visualRect(welcome).center());

    auto *tabs = window.findChild<QTabBar *>(QStringLiteral("noteTabs"));
    EXPECT_EQ(tabTexts(tabs), (QStringList{QStringLiteral("Crlf"), QStringLiteral("Welcome")}));
    EXPECT_EQ(window.editor()->noteText(), QStringLiteral("# Welcome\n\nBody.\n"));
}

#ifdef NOTEAPPC_HAVE_CANVAS
TEST_F(WindowTest, CreateNewCanvasMakesAnEmptyOneAndOpensIt)
{
    MainWindow window;
    window.openVault(dir.path());
    window.findChild<QAction *>(QStringLiteral("canvas:new-file"))->trigger();
    EXPECT_EQ(readAll(path(QStringLiteral("Untitled.canvas"))), QByteArray("{}\n"));
    EXPECT_NE(window.findChild<noteAppC::CanvasView *>(), nullptr);
    window.findChild<QAction *>(QStringLiteral("canvas:new-file"))->trigger();
    EXPECT_TRUE(QFileInfo::exists(path(QStringLiteral("Untitled 1.canvas"))));
}
#endif

TEST_F(WindowTest, ANarrowWindowFoldsBothSidebarsAndGivesThemBack)
{
    MainWindow window;
    window.openVault(dir.path());
    window.resize(1200, 800);
    window.show();
    ASSERT_TRUE(QTest::qWaitForWindowExposed(&window));
    auto *left = window.findChild<QWidget *>(QStringLiteral("sidebar"));
    auto *right = window.findChild<QWidget *>(QStringLiteral("rightSidebar"));
    ASSERT_TRUE(left->isVisible() && right->isVisible());
    window.toggleRightSidebar(); // the reader's own choice, to be kept
    ASSERT_FALSE(right->isVisible());

    window.resize(480, 800); // a phone held upright
    QApplication::processEvents();
    EXPECT_FALSE(left->isVisible());
    EXPECT_FALSE(right->isVisible());

    window.resize(1200, 800);
    QApplication::processEvents();
    EXPECT_TRUE(left->isVisible());
    EXPECT_FALSE(right->isVisible()) << "it was closed before the window narrowed";
    window.toggleRightSidebar(); // put it back for the next test
}

TEST(RecoveryDiffTest, LinesAreKeptRemovedOrAddedInOrder)
{
    using Kind = noteAppC::RecoveryDialog::DiffLine::Kind;
    const auto diff = noteAppC::RecoveryDialog::lineDiff(
        {QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("c"), QStringLiteral("d")},
        {QStringLiteral("a"), QStringLiteral("c"), QStringLiteral("x"), QStringLiteral("d")});
    QStringList shown;
    for (const auto &line : diff)
        shown << (line.kind == Kind::Same ? QStringLiteral(" ") : line.kind == Kind::Removed ? QStringLiteral("-") : QStringLiteral("+"))
                     + line.text;
    EXPECT_EQ(shown, (QStringList{QStringLiteral(" a"), QStringLiteral("-b"), QStringLiteral(" c"),
                                  QStringLiteral("+x"), QStringLiteral(" d")}));
    // Identical: nothing but kept lines.
    const auto same = noteAppC::RecoveryDialog::lineDiff({QStringLiteral("a")}, {QStringLiteral("a")});
    ASSERT_EQ(same.size(), 1);
    EXPECT_EQ(same.first().kind, Kind::Same);
}

TEST_F(WindowTest, RecoveryShowsChangesAgainstTheNoteButRestoresTheSnapshot)
{
    const QString note = path(QStringLiteral("Welcome.md"));
    noteAppC::Snapshots::record(note, QStringLiteral("# Welcome\n\nOld body.\n"), 0);
    noteAppC::Vault vault(dir.path());
    noteAppC::RecoveryDialog dialog(vault);
    auto *notes = dialog.findChild<QListWidget *>(QStringLiteral("recoveryNotes"));
    for (int i = 0; i < notes->count(); ++i) {
        if (notes->item(i)->text() == QStringLiteral("Welcome.md"))
            notes->setCurrentRow(i);
    }
    auto *preview = dialog.findChild<QPlainTextEdit *>(QStringLiteral("recoveryPreview"));
    EXPECT_EQ(preview->toPlainText(), QStringLiteral("# Welcome\n\nOld body.\n"));

    dialog.findChild<QCheckBox *>(QStringLiteral("recoveryShowChanges"))->setChecked(true);
    EXPECT_TRUE(preview->toPlainText().contains(QStringLiteral("- Old body.")));
    EXPECT_TRUE(preview->toPlainText().contains(QStringLiteral("+ Body.")));

    QString restored;
    QObject::connect(&dialog, &noteAppC::RecoveryDialog::restoreRequested,
                     [&restored](const QString &, const QString &text) { restored = text; });
    QTimer::singleShot(0, [] {
        if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget()))
            box->button(QMessageBox::Yes)->click();
    });
    for (QPushButton *button : dialog.findChildren<QPushButton *>()) {
        if (button->text() == QStringLiteral("Restore"))
            button->click();
    }
    EXPECT_EQ(restored, QStringLiteral("# Welcome\n\nOld body.\n")) << "the snapshot, not the diff";
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

TEST_F(WindowTest, RenamingWithAModifiedLinkingNoteOpenKeepsTheLinkRewrite)
{
    write(QStringLiteral("Target.md"), "# Target\n");
    write(QStringLiteral("Home.md"), "See [[Target]].\n");

    MainWindow window;
    QSignalSpy indexed(window.index(), &Index::changed);
    window.openVault(dir.path());
    ASSERT_TRUE(indexed.wait(5000));
    window.openNote(path(QStringLiteral("Home.md")));
    auto *editor = window.editor();
    ASSERT_NE(editor, nullptr);
    editor->setCursorOffset(editor->noteText().size());
    QTest::keyClicks(editor, QStringLiteral("x"));
    ASSERT_TRUE(editor->isModified()) << "the rename must beat the autosave timer";

    auto *tree = window.findChild<QTreeView *>(QStringLiteral("fileExplorer"));
    auto *model = qobject_cast<QFileSystemModel *>(tree->model());
    const QModelIndex index = model->index(path(QStringLiteral("Target.md")));
    ASSERT_TRUE(index.isValid());
    QTimer::singleShot(0, &window, [] {
        if (auto *dialog = qobject_cast<QInputDialog *>(QApplication::activeModalWidget())) {
            dialog->setTextValue(QStringLiteral("Renamed"));
            dialog->accept();
        }
    });
    ASSERT_TRUE(QMetaObject::invokeMethod(&window, "renamePathAt", Q_ARG(QModelIndex, index)));

    // The typed text was saved first, so the rewrite worked on it.
    EXPECT_EQ(readAll(path(QStringLiteral("Home.md"))), QByteArray("See [[Renamed]].\nx"));
    // The watcher then brings the rewritten link into the open editor.
    WAIT_TRUE(editor->noteText().contains(QStringLiteral("[[Renamed]]")), 5000);

    // ...and the next autosave no longer writes [[Target]] back.
    QTest::keyClicks(editor, QStringLiteral("y"));
    WAIT_TRUE(!editor->isModified(), 5000);
    EXPECT_EQ(readAll(path(QStringLiteral("Home.md"))), QByteArray("See [[Renamed]].\nxy"));
}

namespace {
/// The inline title of the tab `editor` is in.
QLineEdit *titleOf(MarkdownEditor *editor)
{
    return editor->parentWidget()->findChild<QLineEdit *>(QStringLiteral("inlineTitle"));
}
} // namespace

TEST_F(WindowTest, TheInlineTitleRenamesTheNoteInEveryPaneAndFollowsLinks)
{
    write(QStringLiteral("Target.md"), "# Target\n");
    write(QStringLiteral("Source.md"), "See [[Target]].\n");

    MainWindow window;
    QSignalSpy indexed(window.index(), &Index::changed);
    window.openVault(dir.path());
    ASSERT_TRUE(indexed.wait(5000));
    window.show();
    ASSERT_TRUE(QTest::qWaitForWindowExposed(&window));
    window.openNote(path(QStringLiteral("Target.md")));
    MarkdownEditor *left = window.editor();
    window.findChild<QAction *>(QStringLiteral("workspace:split-vertical"))->trigger();
    MarkdownEditor *right = window.editor();
    ASSERT_NE(left, right);
    QLineEdit *rightTitle = titleOf(right);
    QLineEdit *leftTitle = titleOf(left);
    ASSERT_NE(rightTitle, nullptr);
    ASSERT_NE(leftTitle, nullptr);
    EXPECT_EQ(rightTitle->text(), QStringLiteral("Target"));
    EXPECT_FALSE(rightTitle->isReadOnly()) << "a click puts a caret in it";

    // A name Obsidian refuses, an existing note, a dot and nothing at all:
    // the old title comes back, the file stays, and the status bar says why.
    for (const QString &bad : {QStringLiteral("a/b"), QStringLiteral("Source"), QStringLiteral(".hidden"),
                               QStringLiteral("   ")}) {
        rightTitle->setText(bad);
        QTest::keyClick(rightTitle, Qt::Key_Return);
        EXPECT_EQ(rightTitle->text(), QStringLiteral("Target")) << bad.toStdString();
        EXPECT_TRUE(QFile::exists(path(QStringLiteral("Target.md")))) << bad.toStdString();
        EXPECT_TRUE(window.statusBar()->currentMessage().contains(QStringLiteral("Could not rename")))
            << bad.toStdString();
        window.statusBar()->clearMessage();
    }

    // Esc gives the edit up.
    rightTitle->setText(QStringLiteral("Nope"));
    QTest::keyClick(rightTitle, Qt::Key_Escape);
    EXPECT_EQ(rightTitle->text(), QStringLiteral("Target"));
    EXPECT_TRUE(QFile::exists(path(QStringLiteral("Target.md"))));

    // Typing a new name and Enter renames the file through the tree's path.
    rightTitle->setText(QStringLiteral("Renamed"));
    QTest::keyClick(rightTitle, Qt::Key_Return);
    EXPECT_FALSE(QFile::exists(path(QStringLiteral("Target.md"))));
    EXPECT_TRUE(QFile::exists(path(QStringLiteral("Renamed.md"))));
    EXPECT_EQ(readAll(path(QStringLiteral("Source.md"))), QByteArray("See [[Renamed]].\n"));
    EXPECT_EQ(leftTitle->text(), QStringLiteral("Renamed")) << "the other pane's title follows";
    EXPECT_EQ(rightTitle->text(), QStringLiteral("Renamed"));
    EXPECT_TRUE(window.windowTitle().startsWith(QStringLiteral("Renamed.md")));
    EXPECT_TRUE(right->hasFocus()) << "Enter moves on into the note";
}

TEST_F(WindowTest, TheStatusBarShowsCountsAndSwitchesTheMode)
{
    write(QStringLiteral("Target.md"), "---\ntags: [a]\nstatus: draft\n---\nOne two three.\n");
    write(QStringLiteral("Source.md"), "See [[Target]].\n");

    MainWindow window;
    QSignalSpy indexed(window.index(), &Index::changed);
    window.openVault(dir.path());
    ASSERT_TRUE(indexed.wait(5000));
    window.show();
    ASSERT_TRUE(QTest::qWaitForWindowExposed(&window));
    window.openNote(path(QStringLiteral("Target.md")));
    MarkdownEditor *editor = window.editor();

    const auto text = [&window](const char *name) {
        return window.findChild<QToolButton *>(QLatin1String(name))->text();
    };
    WAIT_TRUE(text("backlinkStatus") == QStringLiteral("1 backlink"), 5000);
    EXPECT_EQ(text("propertyStatus"), QStringLiteral("2 properties"));
    const auto labels = window.statusBar()->findChildren<QLabel *>();
    const auto has = [&labels](const QString &wanted) {
        return std::any_of(labels.begin(), labels.end(), [&wanted](QLabel *label) { return label->text() == wanted; });
    };
    WAIT_TRUE(has(QStringLiteral("%1 characters").arg(QLocale().toString(editor->characterCount()))), 5000);
    EXPECT_LT(editor->characterCount(), editor->noteText().size() - 20) << "the frontmatter is not counted";
    EXPECT_TRUE(has(QStringLiteral("%1 words").arg(editor->wordCount())));
    // Typing moves the counts with the next parse, not with a rescan.
    editor->setCursorOffset(editor->noteText().size());
    QTest::keyClicks(editor, QStringLiteral("four"));
    WAIT_TRUE(has(QStringLiteral("%1 characters").arg(QLocale().toString(editor->characterCount()))), 5000);

    // A selection is counted instead, a moment after it settles.
    const qsizetype from = editor->noteText().indexOf(QStringLiteral("One"));
    editor->setCursorOffset(from);
    for (int i = 0; i < 7; ++i)
        QTest::keyClick(editor, Qt::Key_Right, Qt::ShiftModifier); // "One two"
    WAIT_TRUE(has(QStringLiteral("2 words, 7 characters")), 5000);
    editor->setCursorOffset(from);
    WAIT_TRUE(has(QStringLiteral("%1 characters").arg(QLocale().toString(editor->characterCount()))), 5000);

    // The pencil's menu: three modes, the check follows the current one.
    auto *menu = window.findChild<QMenu *>(QStringLiteral("modeMenu"));
    ASSERT_NE(menu, nullptr);
    EXPECT_EQ(menu->actions().size(), 3);
    const auto checked = [menu]() {
        for (QAction *action : menu->actions()) {
            if (action->isChecked())
                return action->objectName();
        }
        return QString();
    };
    EXPECT_EQ(editor->mode(), MarkdownEditor::Mode::LivePreview);
    EXPECT_EQ(checked(), QStringLiteral("mode-live"));
    menu->findChild<QAction *>(QStringLiteral("mode-source"))->trigger();
    EXPECT_EQ(editor->mode(), MarkdownEditor::Mode::Source);
    EXPECT_EQ(checked(), QStringLiteral("mode-source"));
    menu->findChild<QAction *>(QStringLiteral("mode-reading"))->trigger();
    EXPECT_EQ(editor->mode(), MarkdownEditor::Mode::Reading);
    EXPECT_EQ(checked(), QStringLiteral("mode-reading"));
    // Ctrl+E from reading returns to what was editing before it.
    window.toggleReadingView();
    EXPECT_EQ(editor->mode(), MarkdownEditor::Mode::Source);
    EXPECT_EQ(checked(), QStringLiteral("mode-source")) << "the button follows a shortcut too";
    menu->findChild<QAction *>(QStringLiteral("mode-live"))->trigger();
    EXPECT_EQ(editor->mode(), MarkdownEditor::Mode::LivePreview);
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

TEST(PublishDialogTest, AsksForAFolderAndARealHomeNote)
{
    noteAppC::PublishDialog::Choices remembered;
    remembered.siteName = QStringLiteral("Garden");
    noteAppC::PublishDialog dialog(remembered, {QStringLiteral("Home.md"), QStringLiteral("a/B.md")});
    auto *publish = dialog.findChild<QDialogButtonBox *>()->buttons().constFirst();
    for (QAbstractButton *button : dialog.findChild<QDialogButtonBox *>()->buttons()) {
        if (button->text() == QStringLiteral("Publish"))
            publish = button;
    }
    EXPECT_FALSE(publish->isEnabled()); // no folder yet
    dialog.findChild<QLineEdit *>(QStringLiteral("publishFolder"))->setText(QStringLiteral("/tmp/site"));
    EXPECT_TRUE(publish->isEnabled());
    auto *home = dialog.findChild<QLineEdit *>(QStringLiteral("publishHomeNote"));
    home->setText(QStringLiteral("Nowhere.md"));
    EXPECT_FALSE(publish->isEnabled()); // not a note
    home->setText(QStringLiteral("a/B.md"));
    EXPECT_TRUE(publish->isEnabled());

    const auto choices = dialog.choices();
    EXPECT_EQ(choices.folder, QStringLiteral("/tmp/site"));
    EXPECT_EQ(choices.siteName, QStringLiteral("Garden"));
    EXPECT_EQ(choices.homeNote, QStringLiteral("a/B.md"));
    EXPECT_TRUE(choices.onlyMarked); // the safe default
    EXPECT_EQ(choices.theme, 2);
}

TEST_F(WindowTest, NarrowSidebarsOpenAsDrawersOverTheNote)
{
    MainWindow window;
    window.resize(1180, 760);
    window.show();
    auto *splitter = window.findChild<QSplitter *>(QStringLiteral("mainSplitter"));
    ASSERT_NE(splitter, nullptr);
    const int panes = splitter->count();
    window.resize(500, 760); // a phone held upright
    QApplication::processEvents();

    auto *toggle = window.findChild<QAction *>(QStringLiteral("app:toggle-left-sidebar"));
    ASSERT_NE(toggle, nullptr);
    toggle->trigger();
    auto *scrim = window.findChild<QWidget *>(QStringLiteral("drawerScrim"));
    ASSERT_NE(scrim, nullptr);
    EXPECT_TRUE(scrim->isVisible());
    EXPECT_EQ(splitter->count(), panes - 1) << "the drawer is over the note, not beside it";
    QWidget *drawer = nullptr;
    for (QWidget *child : window.centralWidget()->findChildren<QWidget *>(Qt::FindDirectChildrenOnly)) {
        if (child != scrim && child->isVisible() && child->geometry().left() == scrim->geometry().left()
            && child->width() < scrim->width())
            drawer = child;
    }
    ASSERT_NE(drawer, nullptr);
    EXPECT_LE(drawer->width(), 320);

    // A tap on the dimmed note closes it and puts it back in the splitter.
    QTest::mouseClick(scrim, Qt::LeftButton, {}, QPoint(scrim->width() - 10, 50));
    EXPECT_FALSE(scrim->isVisible());
    EXPECT_EQ(splitter->count(), panes);

    // Wide again: sidebars sit beside the note as before.
    toggle->trigger();
    window.resize(1180, 760);
    QApplication::processEvents();
    EXPECT_FALSE(scrim->isVisible());
    EXPECT_EQ(splitter->count(), panes);
}

#ifdef NOTEAPPC_HAVE_ENCRYPTION
namespace {

/// Clicks the button labelled `text` on the next modal message box.
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

/// Closes whatever modal box comes up while it lives: a warning a test
/// expects, whenever it appears.
struct ModalCloser {
    QTimer timer;
    ModalCloser()
    {
        QObject::connect(&timer, &QTimer::timeout, [] {
            if (QWidget *box = QApplication::activeModalWidget())
                box->close();
        });
        timer.start(20);
    }
};

QByteArray bytesOf(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

} // namespace

TEST_F(WindowTest, AnEncryptedNoteNeverReachesTheDiskInPlainText)
{
    MainWindow window;
    int asked = 0;
    window.setEncryptionPromptForTests([&asked](bool, bool) {
        ++asked;
        return QStringLiteral("correct horse battery");
    });
    window.openVault(dir.path());
    ASSERT_TRUE(window.openNote(path(QStringLiteral("Welcome.md"))));
    window.editor()->setCursorOffset(window.editor()->noteText().size());
    QTest::keyClicks(window.editor(), QStringLiteral("TOPSECRET"));
    window.findChild<QAction *>(QStringLiteral("editor:save-file"))->trigger(); // a plain save first: file recovery keeps a snapshot

    clickNextBox(QStringLiteral("Encrypt"));
    window.findChild<QAction *>(QStringLiteral("encrypted-notes:encrypt"))->trigger();
    const QString locked = path(QStringLiteral("Welcome.mdlock"));
    ASSERT_TRUE(QFileInfo::exists(locked));
    EXPECT_FALSE(QFileInfo::exists(path(QStringLiteral("Welcome.md"))));
    EXPECT_EQ(asked, 1); // the first encrypted note sets the password
    EXPECT_FALSE(bytesOf(locked).contains("TOPSECRET"));
    EXPECT_FALSE(bytesOf(locked).contains("Body."));
    EXPECT_TRUE(noteAppC::Snapshots::versions(path(QStringLiteral("Welcome.md"))).isEmpty())
        << "the plaintext snapshots went with the plain file";

    // Edited and saved again: still only ciphertext, and still no snapshot.
    QTest::keyClicks(window.editor(), QStringLiteral(" MORESECRET"));
    window.findChild<QAction *>(QStringLiteral("editor:save-file"))->trigger();
    EXPECT_FALSE(bytesOf(locked).contains("MORESECRET"));
    EXPECT_TRUE(noteAppC::Snapshots::versions(locked).isEmpty());
    EXPECT_EQ(asked, 1) << "unlocked for the session";

    // Locked: the tab closes. Opened again: asked again, then the text is back.
    window.findChild<QAction *>(QStringLiteral("encrypted-notes:lock"))->trigger();
    EXPECT_FALSE(window.editor()->noteText().contains(QStringLiteral("TOPSECRET")));
    ASSERT_TRUE(window.openNote(locked));
    EXPECT_EQ(asked, 2);
    EXPECT_TRUE(window.editor()->noteText().contains(QStringLiteral("TOPSECRET MORESECRET")));
}

TEST_F(WindowTest, AWrongOrCancelledPasswordOpensNothing)
{
    MainWindow window;
    window.setEncryptionPromptForTests([](bool, bool) { return QStringLiteral("the right one, long"); });
    window.openVault(dir.path());
    window.findChild<QAction *>(QStringLiteral("encrypted-notes:new"))->trigger();
    const QString locked = path(QStringLiteral("Untitled.mdlock"));
    ASSERT_TRUE(QFileInfo::exists(locked));
    QTest::keyClicks(window.editor(), QStringLiteral("hidden"));
    window.findChild<QAction *>(QStringLiteral("editor:save-file"))->trigger();
    window.findChild<QAction *>(QStringLiteral("encrypted-notes:lock"))->trigger();
    ASSERT_TRUE(window.openNote(path(QStringLiteral("Welcome.md"))));

    window.setEncryptionPromptForTests([](bool, bool) { return QStringLiteral("wrong"); });
    {
        ModalCloser closer; // "Wrong password" is reported in a box
        EXPECT_FALSE(window.openNote(locked));
    }
    window.setEncryptionPromptForTests([](bool, bool) { return QString(); });
    EXPECT_FALSE(window.openNote(locked));
    EXPECT_FALSE(window.editor()->noteText().contains(QStringLiteral("hidden")));
}

TEST_F(WindowTest, UnlockOnStartupAsksWhenTheVaultOpensNotLater)
{
    const QByteArray previousConfigHome = qgetenv("XDG_CONFIG_HOME");
    QTemporaryDir configHome;
    qputenv("XDG_CONFIG_HOME", configHome.path().toUtf8());
    {
        MainWindow first;
        first.setEncryptionPromptForTests([](bool, bool) { return QStringLiteral("a long password"); });
        first.openVault(dir.path());
        ASSERT_TRUE(first.openNote(path(QStringLiteral("Welcome.md"))));
        clickNextBox(QStringLiteral("Encrypt"));
        first.findChild<QAction *>(QStringLiteral("encrypted-notes:encrypt"))->trigger();
        ASSERT_TRUE(QFileInfo::exists(path(QStringLiteral("Welcome.mdlock"))));
    }
    noteAppC::Settings settings = noteAppC::Settings::load();
    settings.setValue(QStringLiteral("encryption.unlockAtStartup"), true);
    ASSERT_TRUE(settings.save());

    MainWindow second;
    int asked = 0;
    second.setEncryptionPromptForTests([&asked](bool creating, bool) {
        EXPECT_FALSE(creating);
        ++asked;
        return QStringLiteral("a long password");
    });
    second.openVault(dir.path());
    ASSERT_TRUE(QTest::qWaitFor([&] { return asked == 1; }, 2000)) << "asked as the vault opened";
    ASSERT_TRUE(second.openNote(path(QStringLiteral("Welcome.mdlock"))));
    EXPECT_EQ(asked, 1) << "and not again on opening the note";
    EXPECT_TRUE(second.editor()->noteText().contains(QStringLiteral("Body.")));
    qputenv("XDG_CONFIG_HOME", previousConfigHome);
}

TEST_F(WindowTest, ChangingThePasswordReencryptsEveryNote)
{
    MainWindow window;
    QString password = QStringLiteral("first password");
    window.setEncryptionPromptForTests([&password](bool, bool) { return password; });
    window.openVault(dir.path());
    ASSERT_TRUE(window.openNote(path(QStringLiteral("Welcome.md"))));
    clickNextBox(QStringLiteral("Encrypt"));
    window.findChild<QAction *>(QStringLiteral("encrypted-notes:encrypt"))->trigger();
    const QString locked = path(QStringLiteral("Welcome.mdlock"));
    const QByteArray before = bytesOf(locked);

    password = QStringLiteral("second password");
    window.findChild<QAction *>(QStringLiteral("encrypted-notes:change-password"))->trigger();
    EXPECT_NE(bytesOf(locked), before);
    window.findChild<QAction *>(QStringLiteral("encrypted-notes:lock"))->trigger();
    password = QStringLiteral("first password");
    {
        ModalCloser closer;
        EXPECT_FALSE(window.openNote(locked)) << "the old password no longer opens it";
    }
    password = QStringLiteral("second password");
    ASSERT_TRUE(window.openNote(locked));
    EXPECT_TRUE(window.editor()->noteText().contains(QStringLiteral("Body.")));
}
#endif

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
    // File recovery snapshots and the index cache too: never the real ones.
    QTemporaryDir dataHome;
    QTemporaryDir cacheHome;
    qputenv("XDG_DATA_HOME", dataHome.path().toUtf8());
    qputenv("XDG_CACHE_HOME", cacheHome.path().toUtf8());
    QApplication app(argc, argv);
#ifdef NOTEAPPC_EXPECT_SYNC
    // A window with sync configured looks in the keyring for this device's
    // keys as it opens: never the real one (a silent secret-service costs
    // 15 s a call). An empty in-memory store - nothing is signed in here.
    static QHash<QString, QByteArray> secrets;
    noteAppC::SyncEngine::setSecretStore(
        {[](const QString &key, QByteArray *value) {
             if (!secrets.contains(key))
                 return false;
             *value = secrets.value(key);
             return true;
         },
         [](const QString &key, const QByteArray &value) { return secrets.insert(key, value) != secrets.end(); },
         [](const QString &key) { return secrets.remove(key) > 0; }});
#endif
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
    WAIT_TRUE(!window.findChildren<QAction *>().isEmpty(), 1000);
    window.openNote(path(QStringLiteral("Linking.md")));

    // What the editor sends when the pointer rests on the link with Ctrl held.
    emit window.editor()->linkHovered(QStringLiteral("Other#Part two"), QPoint(200, 200));
    // Built the first time a link is hovered, not before.
    noteAppC::PagePreview *preview = nullptr;
    WAIT_TRUE(
        (preview = window.findChild<noteAppC::PagePreview *>(QStringLiteral("pagePreview"))) && preview->isVisible(),
        2000);
    const QString shown = preview->editor()->noteText();
    EXPECT_TRUE(shown.startsWith(QLatin1String("# Other")));
    EXPECT_EQ(preview->editor()->cursorOffset(), shown.indexOf(QLatin1String("## Part two")));
    EXPECT_EQ(preview->editor()->mode(), MarkdownEditor::Mode::Reading);

    // Moving off the link lets it go, after a moment to reach the preview.
    emit window.editor()->linkHoverEnded();
    WAIT_TRUE(!preview->isVisible(), 2000);
}

TEST_F(WindowTest, CtrlClickingALinkToAHeadingOpensTheNoteThere)
{
    write(QStringLiteral("Other.md"), "# Other\n\nIntro.\n\n## Part two\n\nSecond part.\n");
    write(QStringLiteral("Linking.md"), "[[Other#Part two]]\n");
    MainWindow window;
    QSignalSpy indexed(window.index(), &Index::changed);
    window.openVault(dir.path());
    ASSERT_TRUE(indexed.wait(5000));
    window.resize(1000, 700);
    window.show();
    window.openNote(path(QStringLiteral("Linking.md")));
    QApplication::processEvents();

    // Wherever the link landed on screen: the click is what is under test.
    MarkdownEditor *editor = window.editor();
    QSignalSpy clicked(editor, &MarkdownEditor::linkActivated);
    for (int y = 2; y < 300 && clicked.isEmpty(); y += 4) {
        for (int x = 2; x < 600 && clicked.isEmpty(); x += 6)
            QTest::mouseClick(editor->viewport(), Qt::LeftButton, Qt::ControlModifier, QPoint(x, y));
    }
    ASSERT_FALSE(clicked.isEmpty());
    EXPECT_EQ(clicked.first().first().toString(), QStringLiteral("Other#Part two"));
    const QString shown = window.editor()->noteText();
    EXPECT_TRUE(shown.startsWith(QLatin1String("# Other")));
    WAIT_EQ(window.editor()->cursorOffset(), shown.indexOf(QLatin1String("## Part two")), 2000);
}

TEST_F(WindowTest, ALinkToABlockOpensTheNoteAtThatBlock)
{
    write(QStringLiteral("Other.md"), "# Other\n\nIntro.\n\nThe second part. ^part\n\n- a list\n- here\n\n^list\n");
    write(QStringLiteral("Linking.md"), "See it.\n");
    MainWindow window;
    QSignalSpy indexed(window.index(), &Index::changed);
    window.openVault(dir.path());
    ASSERT_TRUE(indexed.wait(5000));
    window.openNote(path(QStringLiteral("Linking.md")));

    emit window.editor()->linkActivated(QStringLiteral("Other#^part"));
    const QString shown = window.editor()->noteText();
    ASSERT_TRUE(shown.startsWith(QLatin1String("# Other")));
    EXPECT_EQ(window.editor()->cursorOffset(), shown.indexOf(QLatin1String("The second part")));

    // A marker on a line of its own, after a list; and within the note itself.
    emit window.editor()->linkActivated(QStringLiteral("#^list"));
    EXPECT_EQ(window.editor()->cursorOffset(), shown.indexOf(QLatin1String("^list")));
}

TEST_F(WindowTest, AnImageOpensInATabOfItsOwn)
{
    QImage picture(400, 200, QImage::Format_RGB32);
    picture.fill(Qt::darkCyan);
    ASSERT_TRUE(picture.save(path(QStringLiteral("pic.png"))));
    MainWindow window;
    window.openVault(dir.path());
    window.resize(1000, 700);
    window.show();
    ASSERT_TRUE(window.openNote(path(QStringLiteral("pic.png"))));
    auto *media = window.findChild<MarkdownEditor *>(QStringLiteral("mediaView"));
    ASSERT_NE(media, nullptr);
    WAIT_TRUE(media->isVisibleTo(&window), 1000); // a layout shows a new child a turn later
    EXPECT_EQ(media->mode(), MarkdownEditor::Mode::Reading);
    // Drawn: the picture's own colour is on screen.
    const auto drawn = [media] {
        const QImage shot = media->grab().toImage();
        for (int y = 0; y < shot.height(); y += 4) {
            for (int x = 0; x < shot.width(); x += 4) {
                if (shot.pixelColor(x, y) == QColor(Qt::darkCyan))
                    return true;
            }
        }
        return false;
    };
    WAIT_TRUE(drawn(), 3000);
    // A file of no kind noteAppC opens still says so, and opens nothing.
    write(QStringLiteral("data.bin"), "\x01\x02");
    EXPECT_FALSE(window.openNote(path(QStringLiteral("data.bin"))));
}

#ifdef NOTEAPPC_HAVE_IMAGEVIEW
TEST_F(WindowTest, AnImageOpensAnnotatableOnlyWithImageAnnotationOn)
{
    QImage picture(400, 200, QImage::Format_RGB32);
    picture.fill(Qt::darkCyan);
    ASSERT_TRUE(picture.save(path(QStringLiteral("pic.png"))));
    Settings config = Settings::load();
    config.setValue(Settings::featureKey("image-annotation"), true);
    ASSERT_TRUE(config.save());
    {
        MainWindow window;
        window.openVault(dir.path());
        window.resize(1000, 700);
        window.show();
        ASSERT_TRUE(window.openNote(path(QStringLiteral("pic.png"))));
        EXPECT_NE(window.findChild<noteAppC::ImageView *>(), nullptr);
        EXPECT_EQ(window.findChild<MarkdownEditor *>(QStringLiteral("mediaView")), nullptr);
    }
    config.setValue(Settings::featureKey("image-annotation"), false);
    config.save();
    MainWindow window;
    window.openVault(dir.path());
    ASSERT_TRUE(window.openNote(path(QStringLiteral("pic.png"))));
    EXPECT_EQ(window.findChild<noteAppC::ImageView *>(), nullptr); // off: the plain picture, as before
    EXPECT_NE(window.findChild<MarkdownEditor *>(QStringLiteral("mediaView")), nullptr);
}
#endif

TEST_F(WindowTest, ARandomNoteAndAUniqueNote)
{
    write(QStringLiteral("One.md"), "one\n");
    write(QStringLiteral("Two.md"), "two\n");
    QDir(dir.path()).mkpath(QStringLiteral(".obsidian"));
    QDir(dir.path()).mkpath(QStringLiteral("zk"));
    write(QStringLiteral(".obsidian/zk-prefixer.json"), R"({"folder": "zk", "format": "[ZK-]YYYY"})");
    MainWindow window;
    QSignalSpy indexed(window.index(), &Index::changed);
    window.openVault(dir.path());
    ASSERT_TRUE(indexed.wait(5000));
    window.openNote(path(QStringLiteral("One.md")));

    auto *random = window.findChild<QAction *>(QStringLiteral("random-note"));
    ASSERT_NE(random, nullptr);
    random->trigger();
    EXPECT_NE(window.editor()->noteText(), QStringLiteral("one\n")); // never the note already open

    auto *unique = window.findChild<QAction *>(QStringLiteral("zk-prefixer"));
    ASSERT_NE(unique, nullptr);
    unique->trigger();
    EXPECT_TRUE(QFileInfo::exists(
        path(QStringLiteral("zk/ZK-%1.md").arg(QDate::currentDate().year()))));
}

TEST_F(WindowTest, SlidesSplitAtRulesAndStepWithTheKeys)
{
    EXPECT_EQ(noteAppC::SlidesView::split(QStringLiteral("---\ntitle: x\n---\n# One\n\n---\n\n# Two\n\n---\n\n```\n---\n```\n")),
              (QStringList{QStringLiteral("# One"), QStringLiteral("# Two"),
                           QStringLiteral("```\n---\n```")})); // frontmatter out, a fence kept whole

    write(QStringLiteral("Deck.md"), "# One\n\n---\n\n# Two\n\n---\n\n# Three\n");
    MainWindow window;
    window.openVault(dir.path());
    window.resize(1000, 700);
    window.show();
    window.openNote(path(QStringLiteral("Deck.md")));
    window.editor()->setCursorOffset(window.editor()->noteText().indexOf(QLatin1String("Two")));
    window.findChild<QAction *>(QStringLiteral("slides:start"))->trigger();

    auto *slides = window.findChild<noteAppC::SlidesView *>();
    ASSERT_NE(slides, nullptr);
    EXPECT_EQ(slides->count(), 3);
    EXPECT_EQ(slides->current(), 1); // where the caret was
    QTest::keyClick(slides->editor(), Qt::Key_Right);
    EXPECT_EQ(slides->current(), 2);
    EXPECT_TRUE(slides->editor()->noteText().contains(QLatin1String("Three")));
    QTest::keyClick(slides->editor(), Qt::Key_Right); // the last stays the last
    EXPECT_EQ(slides->current(), 2);
    QTest::keyClick(slides->editor(), Qt::Key_Home);
    EXPECT_EQ(slides->current(), 0);
    QTest::keyClick(slides->editor(), Qt::Key_Escape);
    WAIT_TRUE(!slides->isVisible() && window.editor()->isVisible(), 1000);
}

TEST_F(WindowTest, NoteComposerMergesANoteAndItsLinksFollow)
{
    write(QStringLiteral("A.md"), "---\nx: 1\n---\nAlpha\n");
    write(QStringLiteral("B.md"), "Bravo\n");
    write(QStringLiteral("C.md"), "See [[A]]\n");
    MainWindow window;
    QSignalSpy indexed(window.index(), &Index::changed);
    window.openVault(dir.path());
    ASSERT_TRUE(indexed.wait(5000));
    window.openNote(path(QStringLiteral("A.md")));

    window.findChild<QAction *>(QStringLiteral("note-composer:merge-file"))->trigger();
    auto *picker = window.findChild<noteAppC::QuickSwitcher *>(QStringLiteral("mergePicker"));
    ASSERT_NE(picker, nullptr);
    emit picker->accepted(QStringLiteral("B.md"));

    QFile merged(path(QStringLiteral("B.md")));
    ASSERT_TRUE(merged.open(QIODevice::ReadOnly));
    EXPECT_EQ(merged.readAll(), QByteArray("Bravo\n\nAlpha\n")); // the body, not a second frontmatter
    EXPECT_FALSE(QFileInfo::exists(path(QStringLiteral("A.md"))));
    QFile linking(path(QStringLiteral("C.md")));
    ASSERT_TRUE(linking.open(QIODevice::ReadOnly));
    EXPECT_EQ(linking.readAll(), QByteArray("See [[B]]\n"));
    EXPECT_EQ(window.editor()->noteText(), QStringLiteral("Bravo\n\nAlpha\n"));
}

TEST_F(WindowTest, NoteComposerExtractsTheSelectionIntoANewNote)
{
    write(QStringLiteral("Long.md"), "Intro\n# Part\nbody\nend\n");
    MainWindow window;
    QSignalSpy indexed(window.index(), &Index::changed);
    window.openVault(dir.path());
    ASSERT_TRUE(indexed.wait(5000));
    window.openNote(path(QStringLiteral("Long.md")));
    MarkdownEditor *editor = window.editor();
    editor->setCursorOffset(editor->noteText().indexOf(QLatin1String("# Part")));
    for (int i = 0; i < 12; ++i) // "# Part\nbody\n"
        QTest::keyClick(editor, Qt::Key_Right, Qt::ShiftModifier);
    ASSERT_EQ(editor->selectedText(), QStringLiteral("# Part\nbody\n"));

    // The name dialog, answered as soon as it is up.
    QTimer::singleShot(0, [] {
        for (QWidget *top : QApplication::topLevelWidgets()) {
            if (auto *dialog = qobject_cast<QInputDialog *>(top)) {
                EXPECT_EQ(dialog->textValue(), QStringLiteral("Part")); // named by its heading
                dialog->accept();
            }
        }
    });
    window.findChild<QAction *>(QStringLiteral("note-composer:split-file"))->trigger();
    QFile part(path(QStringLiteral("Part.md")));
    ASSERT_TRUE(part.open(QIODevice::ReadOnly));
    EXPECT_EQ(part.readAll(), QByteArray("# Part\nbody\n"));
    EXPECT_EQ(editor->noteText(), QStringLiteral("Intro\n[[Part]]end\n"));
}

TEST_F(WindowTest, SlashCommandsListTheWindowsCommandsWhenSwitchedOn)
{
    write(QStringLiteral("Note.md"), "\n");
    MainWindow window;
    window.openVault(dir.path());
    window.openNote(path(QStringLiteral("Note.md")));
    MarkdownEditor *editor = window.editor();
    QTest::keyClicks(editor, QStringLiteral("/"));
    EXPECT_FALSE(editor->slashPopupVisible()) << "off by default, as in Obsidian";

    Settings on = Settings::load();
    on.setValue(QStringLiteral("plugins.slashCommand"), true);
    ASSERT_TRUE(on.save());
    MainWindow second;
    second.openVault(dir.path());
    second.openNote(path(QStringLiteral("Note.md")));
    editor = second.editor();
    QTest::keyClicks(editor, QStringLiteral("/rand"));
    ASSERT_TRUE(editor->slashPopupVisible());
    EXPECT_TRUE(editor->slashSuggestions().contains(QStringLiteral("Random note: Open random note")));
    QTest::keyClick(editor, Qt::Key_Escape);
    Settings off = Settings::load();
    off.setValue(QStringLiteral("plugins.slashCommand"), false);
    ASSERT_TRUE(off.save());
}

TEST_F(WindowTest, APastedImageIsSavedToTheAttachmentFolderAndADroppedFileCopiedIn)
{
    write(QStringLiteral("Note.md"), "\n");
    QDir(dir.path()).mkpath(QStringLiteral(".obsidian"));
    write(QStringLiteral(".obsidian/app.json"), R"({"attachmentFolderPath": "assets"})");
    MainWindow window;
    window.openVault(dir.path());
    window.openNote(path(QStringLiteral("Note.md")));
    MarkdownEditor *editor = window.editor();

    QImage image(3, 2, QImage::Format_RGB32);
    image.fill(Qt::green);
    QGuiApplication::clipboard()->setImage(image);
    QTest::keyClick(editor, Qt::Key_V, Qt::ControlModifier);
    const QStringList saved = QDir(path(QStringLiteral("assets"))).entryList({QStringLiteral("Pasted image *.png")});
    ASSERT_EQ(saved.size(), 1);
    EXPECT_TRUE(editor->noteText().contains(QStringLiteral("![[") + saved.first() + QStringLiteral("]]")));

    QTemporaryDir outside;
    const QString dropped = outside.filePath(QStringLiteral("report.pdf"));
    QFile(dropped).open(QIODevice::WriteOnly);
    QMimeData mime;
    mime.setUrls({QUrl::fromLocalFile(dropped)});
    QDragEnterEvent enter(QPoint(20, 20), Qt::CopyAction, &mime, Qt::NoButton, Qt::NoModifier);
    QCoreApplication::sendEvent(editor->viewport(), &enter);
    QDropEvent drop(QPointF(20, 20), Qt::CopyAction, &mime, Qt::NoButton, Qt::NoModifier);
    QCoreApplication::sendEvent(editor->viewport(), &drop);
    EXPECT_TRUE(QFileInfo::exists(path(QStringLiteral("assets/report.pdf"))));
    EXPECT_TRUE(editor->noteText().contains(QStringLiteral("![[report.pdf]]")));
}

TEST_F(WindowTest, TheFootnotesViewListsTheNotesFootnotesAndJumps)
{
    write(QStringLiteral("Note.md"), "One.[^a] Two.^[inline]\n\n[^a]: The body.\n");
    MainWindow window;
    window.openVault(dir.path());
    window.openNote(path(QStringLiteral("Note.md")));
    auto *pane = window.findChild<noteAppC::FootnotesPane *>();
    ASSERT_NE(pane, nullptr);
    auto *list = pane->findChild<QListWidget *>();
    ASSERT_NE(list, nullptr);
    WAIT_TRUE(list->count() == 2, 2000);
    EXPECT_EQ(list->item(0)->text(), QStringLiteral("1   The body."));
    EXPECT_EQ(list->item(1)->text(), QStringLiteral("2   inline"));
    emit list->itemClicked(list->item(0));
    EXPECT_EQ(window.editor()->cursorOffset(), window.editor()->noteText().indexOf(QLatin1String("The body.")));
}

TEST_F(WindowTest, APagePreviewShowsALinkedImage)
{
    QImage picture(800, 400, QImage::Format_RGB32);
    picture.fill(Qt::darkCyan);
    ASSERT_TRUE(picture.save(path(QStringLiteral("pic.png"))));
    write(QStringLiteral("Linking.md"), "See [[pic.png]] and [[Other]].\n");
    write(QStringLiteral("Other.md"), "# Other\n");
    MainWindow window;
    QSignalSpy indexed(window.index(), &Index::changed);
    window.openVault(dir.path());
    ASSERT_TRUE(indexed.wait(5000));
    window.openNote(path(QStringLiteral("Linking.md")));

    emit window.editor()->linkHovered(QStringLiteral("pic.png"), QPoint(200, 200));
    noteAppC::PagePreview *preview = nullptr;
    WAIT_TRUE(
        (preview = window.findChild<noteAppC::PagePreview *>(QStringLiteral("pagePreview"))) && preview->isVisible(),
        2000);
    auto *image = preview->findChild<QLabel *>();
    ASSERT_NE(image, nullptr);
    EXPECT_TRUE(image->isVisibleTo(preview));
    EXPECT_FALSE(preview->editor()->isVisibleTo(preview));
    // Fitted into the popover, its shape kept.
    const QPixmap shown = image->pixmap();
    EXPECT_LE(shown.width(), 520);
    EXPECT_EQ(shown.width(), shown.height() * 2);

    // A note after it puts the editor back.
    emit window.editor()->linkHovered(QStringLiteral("Other"), QPoint(200, 200));
    WAIT_TRUE(preview->editor()->isVisibleTo(preview), 2000);
}

#ifdef NOTEAPPC_HAVE_HISTORY
TEST_F(WindowTest, EverySaveIsInTheNotesHistoryAndOneCanBePutBack)
{
    QTemporaryDir data;
    const QByteArray previous = qgetenv("XDG_DATA_HOME");
    qputenv("XDG_DATA_HOME", data.path().toUtf8());
    {
        MainWindow window;
        window.openVault(dir.path());
        window.openNote(path(QStringLiteral("Welcome.md")));
        MarkdownEditor *editor = window.editor();
        editor->setCursorOffset(editor->noteText().size());
        QTest::keyClicks(editor, QStringLiteral("one"));
        ASSERT_TRUE(QMetaObject::invokeMethod(&window, "save"));
        QTest::keyClicks(editor, QStringLiteral(" two"));
        ASSERT_TRUE(QMetaObject::invokeMethod(&window, "save"));

        QTimer::singleShot(0, [] {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            if (!dialog) {
                ADD_FAILURE() << "no history dialog";
                return;
            }
            auto *versions = dialog->findChild<QListWidget *>(QStringLiteral("noteHistoryVersions"));
            EXPECT_EQ(versions->count(), 2);
            if (versions->count() != 2) {
                dialog->reject(); // never leave a modal dialog open: the test would hang
                return;
            }
            versions->setCurrentRow(1); // the first save
            auto *preview = dialog->findChild<QPlainTextEdit *>(QStringLiteral("noteHistoryPreview"));
            EXPECT_EQ(preview->toPlainText(), QStringLiteral("# Welcome\n\nBody.\none"));
            QTimer::singleShot(0, [] {
                if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget()))
                    box->button(QMessageBox::Yes)->click();
            });
            dialog->findChild<QPushButton *>(QStringLiteral("noteHistoryRestore"))->click();
        });
        window.findChild<QAction *>(QStringLiteral("note-history:open"))->trigger();
        EXPECT_EQ(window.editor()->noteText(), QStringLiteral("# Welcome\n\nBody.\none"));
        EXPECT_EQ(readAll(path(QStringLiteral("Welcome.md"))), QByteArray("# Welcome\n\nBody.\none"));
    }
    // The history lives in the data home, and the vault gained nothing.
    EXPECT_TRUE(QDir(data.path() + QStringLiteral("/noteAppC/history")).exists());
    EXPECT_FALSE(QFileInfo::exists(path(QStringLiteral(".git"))));
    qputenv("XDG_DATA_HOME", previous);
}

TEST_F(WindowTest, NoteHistoryCreatesItsRepositoryOnTheFirstSaveNotWhenTheVaultOpens)
{
    QTemporaryDir data;
    const QByteArray previous = qgetenv("XDG_DATA_HOME");
    qputenv("XDG_DATA_HOME", data.path().toUtf8());
    const QDir histories(data.path() + QStringLiteral("/noteAppC/history"));
    {
        MainWindow window;
        window.openVault(dir.path());
        window.openNote(path(QStringLiteral("Welcome.md")));
        QTest::qWait(100); // a lazy write on the history pool would have landed by now
        EXPECT_FALSE(histories.exists()) << "opening a vault is not a snapshot";

        QTest::keyClicks(window.editor(), QStringLiteral("x"));
        ASSERT_TRUE(QMetaObject::invokeMethod(&window, "save"));
        WAIT_TRUE(histories.entryList(QDir::Dirs | QDir::NoDotAndDotDot).size() == 1, 5000);
    }
    qputenv("XDG_DATA_HOME", previous);
}

TEST_F(WindowTest, NoteHistoryOffMeansNoRepositoryAtAll)
{
    QTemporaryDir data;
    const QByteArray previous = qgetenv("XDG_DATA_HOME");
    qputenv("XDG_DATA_HOME", data.path().toUtf8());
    Settings off = Settings::load();
    off.setValue(QStringLiteral("plugins.noteHistory"), false);
    ASSERT_TRUE(off.save());
    {
        MainWindow window;
        window.openVault(dir.path());
        window.openNote(path(QStringLiteral("Welcome.md")));
        QTest::keyClicks(window.editor(), QStringLiteral("x"));
        ASSERT_TRUE(QMetaObject::invokeMethod(&window, "save"));
    }
    EXPECT_FALSE(QDir(data.path() + QStringLiteral("/noteAppC/history")).exists());
    Settings on = Settings::load();
    on.setValue(QStringLiteral("plugins.noteHistory"), true);
    ASSERT_TRUE(on.save());
    qputenv("XDG_DATA_HOME", previous);
}
#endif

#ifdef NOTEAPPC_EXPECT_SYNC
TEST_F(WindowTest, SyncWaitsForASignInAndNeverAsksOnItsOwn)
{
    MainWindow window;
    window.openTarget(noteAppC::resolveLaunchTarget(dir.path()));
    auto *status = window.findChild<QLabel *>(QStringLiteral("syncStatus"));
    ASSERT_NE(status, nullptr);
    EXPECT_FALSE(status->isVisibleTo(&window)); // no server set: nothing to show
    ASSERT_NE(window.findChild<QAction *>(QStringLiteral("sync:sign-in")), nullptr);

    Settings settings = Settings::load();
    settings.setValue(QStringLiteral("sync.serverURL"), QStringLiteral("https://sync.invalid"));
    settings.setValue(QStringLiteral("sync.email"), QStringLiteral("me@example.invalid"));
    ASSERT_TRUE(settings.save());
    window.openTarget(noteAppC::resolveLaunchTarget(dir.path()));
    // Configured, nothing in the keyring, no password this session: the
    // engine asks the keyring and finds nothing - no dialog, no network.
    EXPECT_TRUE(status->isVisibleTo(&window));
    ASSERT_TRUE(QTest::qWaitFor([&] { return status->text() == QStringLiteral("Sync: not signed in"); }, 5000))
        << status->text().toStdString();
    for (const QDialog *dialog : window.findChildren<QDialog *>())
        EXPECT_FALSE(dialog->isVisible()) << dialog->metaObject()->className();

    settings.setValue(QStringLiteral("sync.serverURL"), QString());
    settings.setValue(QStringLiteral("sync.email"), QString());
    ASSERT_TRUE(settings.save());
}
#endif

#ifdef NOTEAPPC_WORD_STATS_PLUGIN_DIR
TEST_F(WindowTest, AnEnabledPluginsCommandIsAnActionAndItsFenceIsDrawn)
{
    QTemporaryDir data;
    const QByteArray previous = qgetenv("XDG_DATA_HOME");
    qputenv("XDG_DATA_HOME", data.path().toUtf8());
    qputenv("NOTEAPPC_PLUGIN_HOST", NOTEAPPC_PLUGIN_HOST_PATH);
    const QString installed = data.path() + QStringLiteral("/noteAppC/plugins/word-stats");
    ASSERT_TRUE(QDir().mkpath(installed));
    for (const QString file : {QStringLiteral("plugin.ini"), QStringLiteral("libword-stats.so")})
        ASSERT_TRUE(QFile::copy(QStringLiteral(NOTEAPPC_WORD_STATS_PLUGIN_DIR "/") + file, installed + u'/' + file));
    const QString grants = qEnvironmentVariable("XDG_CONFIG_HOME") + QStringLiteral("/noteAppC/plugins.ini");
    {
        QSettings config(grants, QSettings::IniFormat);
        config.setValue(QStringLiteral("word-stats/enabled"), true);
    }
    Settings on = Settings::load();
    on.setValue(QStringLiteral("plugins.communityPlugins"), true);
    ASSERT_TRUE(on.save());
    write(QStringLiteral("Calc.md"), "```calc\n2+2\n```\n\nafter\n");
    {
        MainWindow window;
        // openTarget, as main.cpp reaches it after the first paint: plugins
        // start there, never before.
        window.openTarget(noteAppC::resolveLaunchTarget(dir.path()));
        window.resize(1000, 700);
        window.show();
        WAIT_TRUE(window.findChild<QAction *>(QStringLiteral("word-stats:count-words")), 5000);
        window.openNote(path(QStringLiteral("Calc.md")));
        MarkdownEditor *editor = window.editor();
        editor->setCursorOffset(editor->noteText().size());
        const qsizetype calc = editor->noteText().indexOf(QLatin1String("2+2"));
        WAIT_TRUE(!editor->embedImageSizeAt(calc).isEmpty(), 10000);
    }
    Settings off = Settings::load();
    off.setValue(QStringLiteral("plugins.communityPlugins"), false);
    ASSERT_TRUE(off.save());
    QFile::remove(grants);
    qputenv("XDG_DATA_HOME", previous);
}
#endif

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


TEST_F(WindowTest, AFeatureSwitchedOffIsNotBuiltAndItsCommandsStop)
{
    QTemporaryDir config; // the switches are the user's, kept apart from other tests
    const QByteArray previous = qgetenv("XDG_CONFIG_HOME");
    qputenv("XDG_CONFIG_HOME", config.path().toUtf8());
    MainWindow window;
    window.show(); // the settings page builds its rows when shown
    window.openVault(dir.path());
    window.openNote(path(QStringLiteral("Welcome.md")));
    window.findChild<QAction *>(QStringLiteral("app:open-settings"))->trigger();
    const auto toggle = [&window](const char *key) {
        noteAppC::ToggleSwitch *found = nullptr;
        EXPECT_TRUE(QTest::qWaitFor([&] {
            return (found = window.findChild<noteAppC::ToggleSwitch *>(QStringLiteral("settingsToggle_") + QLatin1String(key)));
        }));
        return found;
    };
    ASSERT_NE(window.findChild<noteAppC::BacklinksPane *>(), nullptr);

    toggle("plugins.backlink")->click();
    EXPECT_EQ(window.findChild<noteAppC::BacklinksPane *>(), nullptr) << "off is not built at all";
    EXPECT_EQ(window.findChild<QToolButton *>(QStringLiteral("backlinksTabButton")), nullptr);
    toggle("plugins.globalSearch")->click();
    EXPECT_FALSE(window.findChild<QAction *>(QStringLiteral("global-search:open"))->isEnabled());
    EXPECT_EQ(window.findChild<QToolButton *>(QStringLiteral("searchTabButton")), nullptr);

    // Back on: built again, pointed at the open note, its tab where it was.
    toggle("plugins.backlink")->click();
    toggle("plugins.globalSearch")->click();
    auto *backlinks = window.findChild<noteAppC::BacklinksPane *>();
    ASSERT_NE(backlinks, nullptr);
    EXPECT_TRUE(window.findChild<QAction *>(QStringLiteral("global-search:open"))->isEnabled());
    const QLayout *tabs = window.findChild<QWidget *>(QStringLiteral("rightSidebarTabs"))->layout();
    const auto at = [&](const char *name) {
        return tabs->indexOf(window.findChild<QToolButton *>(QLatin1String(name)));
    };
    EXPECT_EQ(at("searchTabButton"), 0);
    EXPECT_EQ(at("backlinksTabButton"), 1);
    EXPECT_EQ(at("tagsTabButton"), 2);
#ifdef NOTEAPPC_HAVE_GRAPH
    toggle("plugins.graph")->click();
    EXPECT_EQ(window.findChild<noteAppC::GraphView *>(QStringLiteral("localGraph")), nullptr);
    EXPECT_FALSE(window.findChild<QAction *>(QStringLiteral("graph:open"))->isEnabled());
    toggle("plugins.graph")->click();
    EXPECT_NE(window.findChild<noteAppC::GraphView *>(QStringLiteral("localGraph")), nullptr);
#endif
    qputenv("XDG_CONFIG_HOME", previous);
}

// Vim mode is a setting, off until chosen; on, the status bar shows the mode
// and :w saves the note through the window's own save.
TEST_F(WindowTest, VimModeFollowsItsSettingShowsItsModeAndSaves)
{
    QTemporaryDir config;
    const QByteArray previous = qgetenv("XDG_CONFIG_HOME");
    qputenv("XDG_CONFIG_HOME", config.path().toUtf8());
    MainWindow window;
    window.show(); // the settings page builds its rows when shown
    window.openVault(dir.path());
    window.openNote(path(QStringLiteral("Welcome.md")));
    auto *editor = window.findChild<MarkdownEditor *>();
    ASSERT_NE(editor, nullptr);
    auto *label = window.findChild<QLabel *>(QStringLiteral("vimStatus"));
    ASSERT_NE(label, nullptr);
    EXPECT_FALSE(editor->vimMode());
    EXPECT_FALSE(label->isVisible());

    window.findChild<QAction *>(QStringLiteral("app:open-settings"))->trigger();
    noteAppC::ToggleSwitch *toggle = nullptr;
    ASSERT_TRUE(QTest::qWaitFor([&] {
        return (toggle = window.findChild<noteAppC::ToggleSwitch *>(
                    QStringLiteral("settingsToggle_editor.vimMode")));
    }));
    toggle->click();
    EXPECT_TRUE(editor->vimMode());
    EXPECT_TRUE(label->isVisible());
    EXPECT_EQ(label->text(), QStringLiteral("-- NORMAL --"));

    editor->setCursorOffset(0);
    QTest::keyClicks(editor, QStringLiteral("x"));
    EXPECT_TRUE(window.windowTitle().startsWith(QLatin1Char('*')));
    QTest::keyClicks(editor, QStringLiteral(":w"));
    EXPECT_EQ(label->text(), QStringLiteral(":w"));
    QTest::keyClick(editor, Qt::Key_Return);
    EXPECT_FALSE(window.windowTitle().startsWith(QLatin1Char('*')));
    EXPECT_EQ(readAll(path(QStringLiteral("Welcome.md"))), QByteArray(" Welcome\n\nBody.\n"));

    QTest::keyClicks(editor, QStringLiteral("i"));
    EXPECT_EQ(label->text(), QStringLiteral("-- INSERT --"));

    toggle->click(); // off again: the label goes, typing is plain
    EXPECT_FALSE(editor->vimMode());
    EXPECT_FALSE(label->isVisible());
    qputenv("XDG_CONFIG_HOME", previous);
}

TEST_F(WindowTest, ThePropertiesViewListsEveryPropertyWithItsCount)
{
    write(QStringLiteral("A.md"), "---\nstatus: draft\ntags: [x]\n---\nA\n");
    write(QStringLiteral("B.md"), "---\nstatus: done\n---\nB\n");
    MainWindow window;
    QSignalSpy indexed(window.index(), &Index::changed);
    window.openVault(dir.path());
    ASSERT_TRUE(indexed.wait(5000));
    auto *pane = window.findChild<noteAppC::PropertiesPane *>();
    ASSERT_NE(pane, nullptr);
    auto *list = pane->findChild<QTreeWidget *>();
    ASSERT_NE(list, nullptr);
    WAIT_TRUE(list->topLevelItemCount() == 2, 2000);
    EXPECT_EQ(list->topLevelItem(0)->text(0), QStringLiteral("status")); // two notes, first
    EXPECT_EQ(list->topLevelItem(0)->text(1), QStringLiteral("2"));
    EXPECT_EQ(list->topLevelItem(1)->text(0), QStringLiteral("tags"));
}

TEST_F(WindowTest, AVaultsCorePluginsDecideWhatIsBuilt)
{
    ASSERT_TRUE(QDir(dir.path()).mkpath(QStringLiteral(".obsidian")));
    write(QStringLiteral(".obsidian/core-plugins.json"), R"({"backlink": false, "canvas": false, "graph": false})");
    write(QStringLiteral("Board.canvas"), "{}\n");
    MainWindow window;
    window.openVault(dir.path());
    EXPECT_EQ(window.findChild<noteAppC::BacklinksPane *>(), nullptr);
    EXPECT_NE(window.findChild<noteAppC::OutlinePane *>(), nullptr);
#ifdef NOTEAPPC_HAVE_GRAPH
    EXPECT_EQ(window.findChild<noteAppC::GraphView *>(QStringLiteral("localGraph")), nullptr);
    EXPECT_FALSE(window.findChild<QAction *>(QStringLiteral("graph:open"))->isEnabled());
#endif
#ifdef NOTEAPPC_HAVE_CANVAS
    EXPECT_FALSE(window.openNote(path(QStringLiteral("Board.canvas"))));
    EXPECT_EQ(window.findChild<noteAppC::CanvasView *>(), nullptr);
    EXPECT_FALSE(window.findChild<QAction *>(QStringLiteral("canvas:new-file"))->isEnabled());
#endif
}

TEST_F(WindowTest, TheCommandPaletteRunsCommandsAndPutsTheLastFirst)
{
    MainWindow window;
    window.show();
    window.openVault(dir.path());
    window.openNote(path(QStringLiteral("Welcome.md")));
    EXPECT_EQ(window.findChild<noteAppC::QuickSwitcher *>(QStringLiteral("commandPalette")), nullptr)
        << "built only when first opened";
    auto *rightSidebar = window.findChild<QWidget *>(QStringLiteral("rightSidebar"));
    const bool wasVisible = rightSidebar->isVisible();

    const auto run = [&window](const QString &typed) {
        window.findChild<QAction *>(QStringLiteral("command-palette:open"))->trigger();
        auto *palette = window.findChild<noteAppC::QuickSwitcher *>(QStringLiteral("commandPalette"));
        auto *query = palette->findChild<QLineEdit *>(QStringLiteral("quickSwitcherQuery"));
        QTest::keyClicks(query, typed);
        QTest::keyClick(query, Qt::Key_Return);
        return palette;
    };
    auto *palette = run(QStringLiteral("toggle right"));
    ASSERT_NE(palette, nullptr);
    EXPECT_NE(rightSidebar->isVisible(), wasVisible);

    // Opened again with nothing typed, the command just run leads.
    window.findChild<QAction *>(QStringLiteral("command-palette:open"))->trigger();
    auto *results = palette->findChild<QListWidget *>(QStringLiteral("quickSwitcherResults"));
    ASSERT_GT(results->count(), 0);
    EXPECT_EQ(results->item(0)->text(), QStringLiteral("Toggle right sidebar"));
    palette->hide();

    // An editing command reaches the note's editor.
    MarkdownEditor *editor = window.editor();
    editor->enterBlockAt(2);
    for (int i = 0; i < 7; ++i)
        QTest::keyClick(editor, Qt::Key_Right, Qt::ShiftModifier); // "Welcome"
    run(QStringLiteral("toggle bold"));
    EXPECT_TRUE(editor->noteText().startsWith(QLatin1String("# **Welcome**")));
}

TEST_F(WindowTest, TheQuickSwitcherCreatesTheNoteYouTyped)
{
    MainWindow window;
    window.show();
    window.openVault(dir.path());
    window.openNote(path(QStringLiteral("Welcome.md")));
    const auto open = [&window] {
        window.findChild<QAction *>(QStringLiteral("switcher:open"))->trigger();
        auto *switcher = window.findChild<noteAppC::QuickSwitcher *>(QStringLiteral("quickSwitcher"));
        return switcher->findChild<QLineEdit *>(QStringLiteral("quickSwitcherQuery"));
    };

    // Nothing matches: Enter creates it in the vault root and opens it.
    auto *query = open();
    QTest::keyClicks(query, QStringLiteral("Brand New"));
    EXPECT_EQ(query->parent()->findChild<QListWidget *>(QStringLiteral("quickSwitcherResults"))->count(), 0);
    QTest::keyClick(query, Qt::Key_Return);
    EXPECT_TRUE(QFileInfo::exists(path(QStringLiteral("Brand New.md"))));
    EXPECT_TRUE(window.windowTitle().startsWith(QStringLiteral("Brand New.md")));

    // Shift+Enter creates even though "Welcome" matches; ".md" is not doubled,
    // and a typed "Folder/Name" makes the folder.
    query = open();
    QTest::keyClicks(query, QStringLiteral("Welc"));
    QTest::keyClick(query, Qt::Key_Return, Qt::ShiftModifier);
    EXPECT_TRUE(QFileInfo::exists(path(QStringLiteral("Welc.md"))));
    EXPECT_TRUE(window.windowTitle().startsWith(QStringLiteral("Welc.md")));
    query = open();
    QTest::keyClicks(query, QStringLiteral("Meetings/Standup.md"));
    QTest::keyClick(query, Qt::Key_Return);
    EXPECT_TRUE(QFileInfo::exists(path(QStringLiteral("Meetings/Standup.md"))));
    EXPECT_TRUE(window.windowTitle().startsWith(QStringLiteral("Meetings/Standup.md")));
}

TEST_F(WindowTest, NewNotesGoWhereObsidiansDefaultLocationSays)
{
    QDir(dir.path()).mkpath(QStringLiteral(".obsidian"));
    QDir(dir.path()).mkpath(QStringLiteral("Deep/Er"));
    write(QStringLiteral("Deep/Er/Here.md"), "\n");
    write(QStringLiteral(".obsidian/app.json"), R"({"newFileLocation": "folder", "newFileFolderPath": "Inbox"})");
    {
        MainWindow window;
        window.show();
        window.openVault(dir.path());
        window.findChild<QAction *>(QStringLiteral("switcher:open"))->trigger();
        auto *switcher = window.findChild<noteAppC::QuickSwitcher *>(QStringLiteral("quickSwitcher"));
        auto *query = switcher->findChild<QLineEdit *>(QStringLiteral("quickSwitcherQuery"));
        QTest::keyClicks(query, QStringLiteral("Typed"));
        QTest::keyClick(query, Qt::Key_Return);
        EXPECT_TRUE(QFileInfo::exists(path(QStringLiteral("Inbox/Typed.md")))) << "the folder, made when missing";
    }
    write(QStringLiteral(".obsidian/app.json"), R"({"newFileLocation": "current"})");
    MainWindow window;
    window.openVault(dir.path());
    QAction *newNote = window.findChild<QAction *>(QStringLiteral("file-explorer:new-file"));
    newNote->trigger();
    EXPECT_TRUE(QFileInfo::exists(path(QStringLiteral("Untitled.md")))) << "nothing open: the root";
    window.openNote(path(QStringLiteral("Deep/Er/Here.md")));
    newNote->trigger();
    EXPECT_TRUE(QFileInfo::exists(path(QStringLiteral("Deep/Er/Untitled.md")))) << "the open note's folder";
}

TEST_F(WindowTest, TheCommandPaletteNeverCreatesANote)
{
    MainWindow window;
    window.show();
    window.openVault(dir.path());
    window.findChild<QAction *>(QStringLiteral("command-palette:open"))->trigger();
    auto *palette = window.findChild<noteAppC::QuickSwitcher *>(QStringLiteral("commandPalette"));
    auto *query = palette->findChild<QLineEdit *>(QStringLiteral("quickSwitcherQuery"));
    QTest::keyClicks(query, QStringLiteral("zzzqqq"));
    QTest::keyClick(query, Qt::Key_Return);
    QTest::keyClick(query, Qt::Key_Return, Qt::ShiftModifier);
    EXPECT_FALSE(QFileInfo::exists(path(QStringLiteral("zzzqqq.md"))));
    EXPECT_TRUE(palette->isVisible()) << "nothing to run, so it stays open";
}

#ifdef NOTEAPPC_HAVE_BASES
TEST_F(WindowTest, AnEmbeddedBaseShowsItsRowsInTheNote)
{
    write(QStringLiteral("Task.md"), "---\nstatus: open\n---\nDo it.\n");
    write(QStringLiteral("Tasks.base"),
          "filters:\n  and:\n    - status == \"open\"\nviews:\n  - type: table\n    order: [file.name, status]\n");
    write(QStringLiteral("Board.md"), "![[Tasks.base]]\n");
    MainWindow window;
    window.show();
    QSignalSpy indexed(window.index(), &Index::changed);
    window.openVault(dir.path());
    ASSERT_TRUE(indexed.wait(5000));
    ASSERT_TRUE(window.openNote(path(QStringLiteral("Board.md"))));
    MarkdownEditor *editor = window.editor();
    editor->setCursorOffset(editor->noteText().size()); // the caret off the embed's line
    WAIT_TRUE(editor->baseRowsAt(0).size() == 1, 5000);
    EXPECT_EQ(editor->baseRowsAt(0).constFirst().path, QStringLiteral("Task.md"));
}
#endif

TEST_F(WindowTest, TheCorePluginsSwitchesReachTheEditor)
{
    QTemporaryDir config;
    const QByteArray previous = qgetenv("XDG_CONFIG_HOME");
    qputenv("XDG_CONFIG_HOME", config.path().toUtf8());
    Settings off = Settings::load();
    off.setValue(QStringLiteral("plugins.pdfEmbeds"), false);
    off.setValue(QStringLiteral("plugins.globalSearch"), false);
    ASSERT_TRUE(off.save());
    {
        MainWindow window;
        window.openVault(dir.path());
        window.openNote(path(QStringLiteral("Welcome.md")));
        const MarkdownEditor::Features features = window.editor()->features();
        EXPECT_FALSE(features.PDFembeds);
        EXPECT_FALSE(features.queryBlocks) << "embedded search is Search's";
        EXPECT_TRUE(features.baseBlocks);
    }
    qputenv("XDG_CONFIG_HOME", previous);
}

TEST_F(WindowTest, CtrlETogglesReadingViewAndComesBackToTheModeItLeft)
{
    QTemporaryDir config; // the switch below is the user's
    const QByteArray previous = qgetenv("XDG_CONFIG_HOME");
    qputenv("XDG_CONFIG_HOME", config.path().toUtf8());
    MainWindow window;
    window.openVault(dir.path());
    window.openNote(path(QStringLiteral("Welcome.md")));
    MarkdownEditor *editor = window.editor();
    QAction *reading = window.findChild<QAction *>(QStringLiteral("markdown:toggle-preview"));
    QAction *source = window.findChild<QAction *>(QStringLiteral("editor:toggle-source"));
    ASSERT_EQ(editor->mode(), MarkdownEditor::Mode::LivePreview);

    reading->trigger();
    EXPECT_EQ(editor->mode(), MarkdownEditor::Mode::Reading);
    reading->trigger();
    EXPECT_EQ(editor->mode(), MarkdownEditor::Mode::LivePreview);

    source->trigger();
    EXPECT_EQ(editor->mode(), MarkdownEditor::Mode::Source);
    reading->trigger();
    reading->trigger();
    EXPECT_EQ(editor->mode(), MarkdownEditor::Mode::Source) << "back to the mode it left";

    // A settings pass - any switch on the settings page - keeps the tab
    // where it is.
    reading->trigger();
    window.show();
    window.findChild<QAction *>(QStringLiteral("app:open-settings"))->trigger();
    noteAppC::ToggleSwitch *wordCount = nullptr;
    ASSERT_TRUE(QTest::qWaitFor([&] {
        return (wordCount = window.findChild<noteAppC::ToggleSwitch *>(QStringLiteral("settingsToggle_plugins.wordCount")));
    }));
    wordCount->click();
    wordCount->click();
    EXPECT_EQ(editor->mode(), MarkdownEditor::Mode::Reading);
    qputenv("XDG_CONFIG_HOME", previous);
}

TEST_F(WindowTest, ANoteInTwoTabsIsOneDocument)
{
    MainWindow window;
    window.openVault(dir.path());
    window.openNote(path(QStringLiteral("Welcome.md")));
    MarkdownEditor *first = window.editor();
    window.findChild<QAction *>(QStringLiteral("workspace:split-vertical"))->trigger();
    MarkdownEditor *second = window.editor();
    ASSERT_NE(first, second);

    // An edit in either is made in the other.
    second->setCursorOffset(second->noteText().size());
    QTest::keyClicks(second, QStringLiteral("more"));
    EXPECT_EQ(first->noteText(), second->noteText());
    EXPECT_TRUE(first->noteText().endsWith(QLatin1String("more")));
    first->setCursorOffset(0);
    QTest::keyClicks(first, QStringLiteral("# "));
    EXPECT_EQ(first->noteText(), second->noteText());
    EXPECT_EQ(second->cursorOffset(), second->noteText().size()) << "its caret kept its place";
    QTest::keyClick(first, Qt::Key_Z, Qt::ControlModifier);
    EXPECT_EQ(first->noteText(), second->noteText()) << "an undo too";

    // A third tab opened on it shows the unsaved document, not the file.
    window.findChild<QAction *>(QStringLiteral("workspace:new-tab"))->trigger();
    window.openNote(path(QStringLiteral("Welcome.md")));
    EXPECT_EQ(window.editor()->noteText(), first->noteText());
    EXPECT_TRUE(window.editor()->isModified());

    // Closing one of them asks nothing: the others still hold it.
    bool asked = false;
    QTimer watch;
    QObject::connect(&watch, &QTimer::timeout, [&asked] {
        if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
            asked = true;
            box->button(QMessageBox::Cancel)->click();
        }
    });
    watch.start(20);
    window.findChild<QAction *>(QStringLiteral("workspace:close"))->trigger();
    EXPECT_FALSE(asked);

    // One save is every view's.
    ASSERT_TRUE(QMetaObject::invokeMethod(&window, "save"));
    EXPECT_FALSE(first->isModified());
    EXPECT_FALSE(second->isModified());
    EXPECT_EQ(readAll(path(QStringLiteral("Welcome.md"))), first->noteText().toUtf8());
}

TEST_F(WindowTest, TypingIsSavedAMomentAfterItStopsAndLeavingNeverAsks)
{
    MainWindow window;
    window.openVault(dir.path());
    window.openNote(path(QStringLiteral("Welcome.md")));
    MarkdownEditor *editor = window.editor();
    editor->setCursorOffset(editor->noteText().size());
    QTest::keyClicks(editor, QStringLiteral("typed"));
    ASSERT_TRUE(editor->isModified());
    EXPECT_FALSE(readAll(path(QStringLiteral("Welcome.md"))).contains("typed")) << "not on every keystroke";
    WAIT_TRUE(!editor->isModified(), 5000);
    EXPECT_TRUE(readAll(path(QStringLiteral("Welcome.md"))).endsWith("typed"));

    // Leaving a note with unsaved changes saves it; nothing asks.
    bool asked = false;
    QTimer watch;
    QObject::connect(&watch, &QTimer::timeout, [&asked] {
        if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
            asked = true;
            box->button(QMessageBox::Cancel)->click();
        }
    });
    watch.start(20);
    QTest::keyClicks(editor, QStringLiteral(" more"));
    ASSERT_TRUE(window.openNote(path(QStringLiteral("Projects/Crlf.md"))));
    EXPECT_FALSE(asked);
    EXPECT_TRUE(readAll(path(QStringLiteral("Welcome.md"))).endsWith("typed more"));
}

namespace {
// Makes `dir` read-only (QSaveFile cannot create its temporary file there, so
// every save fails) and puts it back, so the temp dir can clean itself up.
struct ReadOnlyDir {
    explicit ReadOnlyDir(const QString &path) : dir(path)
    {
        QFile::setPermissions(dir, QFileDevice::ReadOwner | QFileDevice::ExeOwner);
    }
    ~ReadOnlyDir() { restore(); }
    void restore() { QFile::setPermissions(dir, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner); }
    QString dir;
};
} // namespace

TEST_F(WindowTest, ClosingWithAnAutosaveThatKeepsFailingAsksInsteadOfRefusingSilently)
{
    if (geteuid() == 0)
        GTEST_SKIP() << "root ignores directory permissions";
    MainWindow window;
    window.openVault(dir.path());
    window.openNote(path(QStringLiteral("Welcome.md")));
    MarkdownEditor *editor = window.editor();
    editor->setCursorOffset(editor->noteText().size());
    QTest::keyClicks(editor, QStringLiteral("typed"));
    ASSERT_TRUE(editor->isModified());

    ReadOnlyDir readOnly(dir.path());
    QString boxText;
    QTimer watch;
    QObject::connect(&watch, &QTimer::timeout, [&boxText] {
        if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
            boxText = box->text();
            box->button(QMessageBox::Discard)->click();
        }
    });
    watch.start(20);
    EXPECT_TRUE(window.close()) << "a save that cannot work must not trap the window";
    EXPECT_FALSE(boxText.isEmpty()) << "the person is asked, not left with a status-bar line";
    EXPECT_TRUE(boxText.contains(QStringLiteral("Welcome.md")));
    watch.stop();
    readOnly.restore();
    EXPECT_FALSE(readAll(path(QStringLiteral("Welcome.md"))).contains("typed"));
}

TEST_F(WindowTest, AFailedAutosaveIsTriedAgainAtTheNextPauseWithoutAnotherEdit)
{
    if (geteuid() == 0)
        GTEST_SKIP() << "root ignores directory permissions";
    MainWindow window;
    window.openVault(dir.path());
    window.openNote(path(QStringLiteral("Welcome.md")));
    MarkdownEditor *editor = window.editor();
    editor->setCursorOffset(editor->noteText().size());
    ReadOnlyDir readOnly(dir.path());
    QTest::keyClicks(editor, QStringLiteral("typed"));
    ASSERT_TRUE(editor->isModified());

    // The first pause fails and says so.
    WAIT_TRUE(window.statusBar()->currentMessage().contains(QStringLiteral("Could not save")), 5000);
    ASSERT_TRUE(editor->isModified());

    // Nobody types again; the folder works again; the next pause saves.
    readOnly.restore();
    WAIT_TRUE(!editor->isModified(), 5000);
    EXPECT_TRUE(readAll(path(QStringLiteral("Welcome.md"))).endsWith("typed"));
}

TEST_F(WindowTest, WithAutosaveOffNothingSavesUntilAsked)
{
    QTemporaryDir config;
    const QByteArray previous = qgetenv("XDG_CONFIG_HOME");
    qputenv("XDG_CONFIG_HOME", config.path().toUtf8());
    Settings off = Settings::load();
    off.setValue(QStringLiteral("files.autosave"), false);
    ASSERT_TRUE(off.save());
    {
        MainWindow window;
        window.openVault(dir.path());
        window.openNote(path(QStringLiteral("Welcome.md")));
        QTest::keyClicks(window.editor(), QStringLiteral("x"));
        QTest::qWait(2600);
        EXPECT_TRUE(window.editor()->isModified());
        bool asked = false;
        QTimer watch;
        QObject::connect(&watch, &QTimer::timeout, [&asked] {
            if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
                asked = true;
                box->button(QMessageBox::Discard)->click();
            }
        });
        watch.start(20);
        window.openNote(path(QStringLiteral("Projects/Crlf.md")));
        EXPECT_TRUE(asked);
    }
    EXPECT_EQ(readAll(path(QStringLiteral("Welcome.md"))), QByteArray("# Welcome\n\nBody.\n"));
    qputenv("XDG_CONFIG_HOME", previous);
}

#ifdef NOTEAPPC_HAVE_NOTEBOOK
// A .ipynb in the vault opens as a note: cells as text, its stored outputs
// shown on the blocks, an untouched save changing no bytes, an edit saved
// back into the same notebook, and the notebooks feature switch turning it
// all off.
TEST_F(WindowTest, ANotebookFileOpensAsANoteAndSavesBackAsANotebook)
{
    const QByteArray original = readAll(QStringLiteral(NOTEAPPC_FIXTURES_DIR "/notebook/sample.ipynb"));
    write(QStringLiteral("Analysis.ipynb"), original);
    {
        MainWindow window;
        window.show();
        window.openVault(dir.path());
        ASSERT_TRUE(window.openNote(path(QStringLiteral("Analysis.ipynb"))));
        MarkdownEditor *editor = window.editor();
        EXPECT_TRUE(editor->noteText().startsWith(QStringLiteral("# Title\n\n")));
        EXPECT_TRUE(editor->noteText().contains(QStringLiteral("```python\nimport math")));

        // The stored output is on the block, with nothing run.
        editor->setCursorOffset(editor->noteText().size());
        const qsizetype code = editor->noteText().indexOf(QStringLiteral("import math"));
        ASSERT_TRUE(QTest::qWaitFor(
            [&] {
                const auto info = editor->notebookInfoAt(code);
                return info.eligible && info.hasRun;
            },
            5000));
        EXPECT_EQ(editor->notebookInfoAt(code).outputCellCount, 5);

        // Saving what was not touched writes the file's own bytes.
        ASSERT_TRUE(QMetaObject::invokeMethod(&window, "save"));
        EXPECT_EQ(readAll(path(QStringLiteral("Analysis.ipynb"))), original);

        editor->setCursorOffset(editor->noteText().indexOf(QStringLiteral("# Title")) + 2);
        QTest::keyClicks(editor, QStringLiteral("Big "));
        ASSERT_TRUE(QMetaObject::invokeMethod(&window, "save"));
        const QByteArray saved = readAll(path(QStringLiteral("Analysis.ipynb")));
        EXPECT_NE(saved, original);
        EXPECT_TRUE(saved.contains("# Big Title"));
        EXPECT_TRUE(saved.contains("\"nbformat\": 4"));
        EXPECT_TRUE(saved.contains("ZeroDivisionError")) << "the cells that were not edited keep their outputs";
        EXPECT_FALSE(window.windowTitle().startsWith(QLatin1Char('*')));
    }
}

TEST_F(WindowTest, ANotebookFileIsNotANoteWithTheNotebooksFeatureOff)
{
    write(QStringLiteral("Analysis.ipynb"), readAll(QStringLiteral(NOTEAPPC_FIXTURES_DIR "/notebook/sample.ipynb")));
    Settings config = Settings::load();
    config.setValue(Settings::featureKey("notebooks"), false);
    ASSERT_TRUE(config.save());
    {
        MainWindow window;
        window.openVault(dir.path());
        window.openNote(path(QStringLiteral("Welcome.md")));
        EXPECT_FALSE(window.openNote(path(QStringLiteral("Analysis.ipynb"))));
        EXPECT_EQ(window.editor()->noteText(), QStringLiteral("# Welcome\n\nBody.\n"));
    }
    config.setValue(Settings::featureKey("notebooks"), true);
    config.save();
}

// The index reads a notebook as the text it opens as: found by a search whose
// offsets land in that text, by the quick switcher and by `[[Analysis.ipynb]]`;
// with the notebooks feature off it is a plain file again, in a cache that
// was written with the feature on, too.
TEST_F(WindowTest, ANotebookIsInTheIndexOnlyWhileTheNotebooksFeatureIsOn)
{
    write(QStringLiteral("Analysis.ipynb"), readAll(QStringLiteral(NOTEAPPC_FIXTURES_DIR "/notebook/sample.ipynb")));
    write(QStringLiteral("Mention.md"), "See [[Analysis.ipynb]].\n");
    const QString relative = QStringLiteral("Analysis.ipynb");
    Settings config = Settings::load();
    ASSERT_TRUE(config.boolean(Settings::featureKey("notebooks"))) << "on by default";
    {
        MainWindow window;
        window.openVault(dir.path());
        WAIT_TRUE(window.index()->resolve(QStringLiteral("Analysis.ipynb")) == relative, 15000); // the names land last
        EXPECT_TRUE(window.index()->allPaths().contains(relative));
        EXPECT_EQ(window.index()->findByName(QStringLiteral("analysis.ipy")), QStringList{relative});
        const auto backlinks = window.index()->backlinks(path(relative));
        ASSERT_EQ(backlinks.size(), 1);
        EXPECT_EQ(backlinks.first().fromPath, QStringLiteral("Mention.md"));

        // The match is where the opened note shows the word.
        const auto found = window.index()->search(QStringLiteral("unicode"));
        ASSERT_EQ(found.size(), 1);
        ASSERT_EQ(found.first().path, relative);
        ASSERT_FALSE(found.first().matches.isEmpty());
        ASSERT_TRUE(window.openNote(path(relative)));
        const noteAppC::SearchMatch first = found.first().matches.first();
        EXPECT_EQ(window.editor()->noteText().mid(first.offset, first.length).toLower(), QStringLiteral("unicode"));
    }
    config.setValue(Settings::featureKey("notebooks"), false);
    ASSERT_TRUE(config.save());
    {
        // The same cache, written with the feature on.
        MainWindow window;
        window.openVault(dir.path());
        // The cache still holds it as a note until the scan has seen that it is not one now.
        WAIT_TRUE(!window.index()->allPaths().contains(relative), 15000);
        EXPECT_TRUE(window.index()->search(QStringLiteral("unicode")).isEmpty());
        WAIT_TRUE(window.index()->resolve(QStringLiteral("Analysis.ipynb")) == relative, 15000); // still a file a link finds
    }
    config.setValue(Settings::featureKey("notebooks"), true);
    config.save();
}

// "Share one kernel between a note's code blocks": through the window's own
// runner (the settings, the note's path as the kernel's scope, the sandbox),
// a variable one block sets is there for the next, and the second block shows
// the kernel's execution count.
TEST_F(WindowTest, ANotesBlocksShareOneKernelWhenTheSettingIsOn)
{
    if (QStandardPaths::locateAll(QStandardPaths::GenericDataLocation,
                                  QStringLiteral("jupyter/kernels/python3/kernel.json")).isEmpty()
        || QStandardPaths::findExecutable(QStringLiteral("bwrap")).isEmpty())
        GTEST_SKIP() << "no Jupyter python kernel, or no bubblewrap, on this machine";
    Settings config = Settings::load();
    config.setValue(QStringLiteral("notebooks.sharedKernel"), true);
    ASSERT_TRUE(config.save());

    // A marker in the code: the output cache outlives the run in the test's cache home.
    const QString marker = QUuid::createUuid().toString(QUuid::Id128);
    write(QStringLiteral("Shared.md"),
          QStringLiteral("```python\nvalue = 41\n```\n\n```python\nprint(value + 1, '%1')\n```\n\nend\n")
              .arg(marker).toUtf8());
    {
        MainWindow window;
        window.show(); // blocks are laid out when they are on screen
        window.openVault(dir.path());
        window.openNote(path(QStringLiteral("Shared.md")));
        MarkdownEditor *editor = window.editor();
        editor->setCursorOffset(editor->noteText().size()); // off the blocks: they render
        QTest::qWait(200);
        const qsizetype second = editor->noteText().indexOf(QStringLiteral("print(value"));
        ASSERT_TRUE(editor->notebookInfoAt(second).eligible);
        EXPECT_EQ(editor->runAllNotebookBlocks(), 2);
        // A NameError would also be an output cell, so the run must be ok.
        ASSERT_TRUE(QTest::qWaitFor(
            [&] {
                const auto info = editor->notebookInfoAt(second);
                return info.hasRun && !info.running;
            },
            30000));
        const auto info = editor->notebookInfoAt(second);
        EXPECT_TRUE(info.ok);
        EXPECT_GE(info.executionCount, 2);
        EXPECT_EQ(info.outputCellCount, 1);
    }
    config.setValue(QStringLiteral("notebooks.sharedKernel"), false);
    config.save();
}

// input() from a block, through the window's own runner and a real kernel: the
// prompt and its line edit appear under the block, Return sends the line back
// to the running code, and Esc interrupts the run while the kernel lives on.
TEST_F(WindowTest, InputInABlockIsAskedUnderItAndAnsweredWithReturnOrInterruptedWithEsc)
{
    if (QStandardPaths::locateAll(QStandardPaths::GenericDataLocation,
                                  QStringLiteral("jupyter/kernels/python3/kernel.json")).isEmpty()
        || QStandardPaths::findExecutable(QStringLiteral("bwrap")).isEmpty()) {
        ASSERT_NE(qEnvironmentVariable("NOTEAPPC_REQUIRE_TOOLS"), QLatin1String("1"))
            << "NOTEAPPC_REQUIRE_TOOLS=1 and no Jupyter python kernel, or no bubblewrap";
        GTEST_SKIP() << "no Jupyter python kernel, or no bubblewrap, on this machine";
    }
    Settings config = Settings::load();
    config.setValue(QStringLiteral("notebooks.sharedKernel"), true);
    ASSERT_TRUE(config.save());

    // A marker in the code keeps each run's cached output apart from older runs of the same code.
    const QString marker = QUuid::createUuid().toString(QUuid::Id128);
    // The assert is the check: a wrong or missing answer is an error cell, an unsuccessful run.
    write(QStringLiteral("Ask.md"),
          QStringLiteral("```python\nname = input('Name? ')\nassert name == 'Ada', repr(name)  # %1\n```\n\n"
                         "```python\nimport getpass\ngetpass.getpass('Secret? ')  # %1\nprint('not reached')\n```\n\nend\n")
              .arg(marker).toUtf8());
    {
        MainWindow window;
        window.show();
        window.openVault(dir.path());
        window.openNote(path(QStringLiteral("Ask.md")));
        MarkdownEditor *editor = window.editor();
        editor->setCursorOffset(editor->noteText().size()); // off the blocks: they render
        QTest::qWait(200);
        const QString written = editor->noteText();
        const qsizetype first = editor->noteText().indexOf(QStringLiteral("name = input"));
        const qsizetype second = editor->noteText().indexOf(QStringLiteral("import getpass"));
        ASSERT_TRUE(editor->notebookInfoAt(first).eligible);

        ASSERT_TRUE(editor->runNotebookBlockAt(first));
        WAIT_TRUE(editor->notebookInputInfo().active && editor->notebookInputInfo().visible, 30000);
        EXPECT_EQ(editor->notebookInputInfo().prompt, QStringLiteral("Name? "));
        EXPECT_FALSE(editor->notebookInputInfo().password);
        EXPECT_TRUE(editor->notebookInfoAt(first).running) << "the run waits for the answer";
        QLineEdit *edit = editor->notebookInputEdit();
        ASSERT_NE(edit, nullptr);
        QTest::keyClicks(edit, QStringLiteral("Ada"));
        QTest::keyClick(edit, Qt::Key_Return);
        WAIT_TRUE(!editor->notebookInputInfo().active, 5000);
        WAIT_TRUE(editor->notebookInfoAt(first).hasRun && !editor->notebookInfoAt(first).running, 30000);
        EXPECT_TRUE(editor->notebookInfoAt(first).ok) << "the kernel got exactly what was typed";
        EXPECT_EQ(editor->noteText(), written) << "Return typed no newline into the note";

        // getpass is masked, and Esc is a KeyboardInterrupt in the same kernel.
        ASSERT_TRUE(editor->runNotebookBlockAt(second));
        WAIT_TRUE(editor->notebookInputInfo().active && editor->notebookInputInfo().visible, 30000);
        EXPECT_TRUE(editor->notebookInputInfo().password);
        EXPECT_EQ(editor->notebookInputEdit()->echoMode(), QLineEdit::Password);
        QTest::keyClick(editor->notebookInputEdit(), Qt::Key_Escape);
        WAIT_TRUE(editor->notebookInfoAt(second).hasRun && !editor->notebookInfoAt(second).running, 30000);
        EXPECT_FALSE(editor->notebookInfoAt(second).ok);
        EXPECT_FALSE(editor->notebookInputInfo().active);
        EXPECT_EQ(editor->noteText(), written) << "the note is as written";
    }
    config.setValue(QStringLiteral("notebooks.sharedKernel"), false);
    config.save();
}

// Ctrl+Space in a block lists what the note's own kernel knows - once a block
// has started it; before that the key asks for nothing and starts nothing.
TEST_F(WindowTest, CtrlSpaceCompletesFromTheNotesKernelOnceOneIsRunning)
{
    if (QStandardPaths::locateAll(QStandardPaths::GenericDataLocation,
                                  QStringLiteral("jupyter/kernels/python3/kernel.json")).isEmpty()
        || QStandardPaths::findExecutable(QStringLiteral("bwrap")).isEmpty())
        GTEST_SKIP() << "no Jupyter python kernel, or no bubblewrap, on this machine";
    Settings config = Settings::load();
    config.setValue(QStringLiteral("notebooks.sharedKernel"), true);
    ASSERT_TRUE(config.save());
    const QString marker = QUuid::createUuid().toString(QUuid::Id128);
    write(QStringLiteral("Complete.md"),
          QStringLiteral("```python\nvalue_alpha = 1\nvalue_beta = '%1'\n```\n\n```python\nvalue_\n```\n\nend\n")
              .arg(marker).toUtf8());
    {
        MainWindow window;
        window.show();
        window.openVault(dir.path());
        window.openNote(path(QStringLiteral("Complete.md")));
        MarkdownEditor *editor = window.editor();
        const qsizetype caret = editor->noteText().indexOf(QStringLiteral("value_\n")) + 6;
        editor->setCursorOffset(caret);
        QTest::keyClick(editor, Qt::Key_Space, Qt::ControlModifier);
        QTest::qWait(150);
        EXPECT_TRUE(editor->notebookCompletionRows().isEmpty()) << "no kernel is running yet";

        editor->setCursorOffset(editor->noteText().size());
        QTest::qWait(150);
        const qsizetype first = editor->noteText().indexOf(QStringLiteral("value_alpha"));
        ASSERT_TRUE(editor->runNotebookBlockAt(first));
        ASSERT_TRUE(QTest::qWaitFor(
            [&] {
                const auto info = editor->notebookInfoAt(first);
                return info.hasRun && !info.running;
            },
            30000));

        editor->setCursorOffset(caret);
        QTest::keyClick(editor, Qt::Key_Space, Qt::ControlModifier);
        ASSERT_TRUE(QTest::qWaitFor([&] { return !editor->notebookCompletionRows().isEmpty(); }, 20000));
        const QStringList rows = editor->notebookCompletionRows();
        EXPECT_TRUE(rows.contains(QStringLiteral("value_alpha")) && rows.contains(QStringLiteral("value_beta"))) << rows.join(QLatin1Char(',')).toStdString();
        const int beta = int(rows.indexOf(QStringLiteral("value_beta")));
        for (int i = 0; i < beta; ++i)
            QTest::keyClick(editor, Qt::Key_Down);
        QTest::keyClick(editor, Qt::Key_Return);
        EXPECT_TRUE(editor->noteText().contains(QStringLiteral("value_beta\n```\n\nend")));

        // Shift+Tab on a name: its documentation.
        editor->setCursorOffset(editor->noteText().indexOf(QStringLiteral("value_beta\n```\n\nend")) + 10);
        QTest::keyClick(editor, Qt::Key_Backtab, Qt::ShiftModifier);
        ASSERT_TRUE(QTest::qWaitFor([&] { return !editor->notebookInspectText().isEmpty(); }, 20000));
        EXPECT_TRUE(editor->notebookInspectText().contains(marker));
        QTest::keyClick(editor, Qt::Key_Escape);
    }
    config.setValue(QStringLiteral("notebooks.sharedKernel"), false);
    config.save();
}
#endif

// --- one note in two panes, and what reaches it from outside ----------------

namespace {

/// The two editors of a split made on `relative`: the first, then the new one.
std::pair<MarkdownEditor *, MarkdownEditor *> splitOn(MainWindow &window, const QString &path)
{
    window.openNote(path);
    MarkdownEditor *first = window.editor();
    window.findChild<QAction *>(QStringLiteral("workspace:split-vertical"))->trigger();
    return {first, window.editor()};
}

void overwrite(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    ASSERT_EQ(file.write(bytes), bytes.size());
}

/// What a sync client or another editor does: a new file renamed over it.
void replaceByRename(const QString &path, const QByteArray &bytes)
{
    const QString temporary = path + QStringLiteral(".incoming");
    overwrite(temporary, bytes);
    ASSERT_EQ(std::rename(QFile::encodeName(temporary).constData(), QFile::encodeName(path).constData()), 0);
}

} // namespace

TEST_F(WindowTest, EditsInEitherPaneAutosaveAndClosingOneKeepsTheOther)
{
    MainWindow window;
    window.openVault(dir.path());
    auto [left, right] = splitOn(window, path(QStringLiteral("Welcome.md")));
    ASSERT_NE(left, right);
    left->setCursorOffset(0);
    QTest::keyClicks(left, QStringLiteral("L"));
    right->setCursorOffset(right->noteText().size());
    QTest::keyClicks(right, QStringLiteral("R"));
    ASSERT_EQ(left->noteText(), QStringLiteral("L# Welcome\n\nBody.\nR"));
    WAIT_TRUE(!left->isModified() && !right->isModified(), 5000);
    EXPECT_EQ(readAll(path(QStringLiteral("Welcome.md"))), QByteArray("L# Welcome\n\nBody.\nR"));

    // The right pane goes; the left one still saves what is typed in it.
    right->setFocus();
    QApplication::processEvents();
    window.findChild<QAction *>(QStringLiteral("workspace:close"))->trigger();
    flushDeletes();
    ASSERT_EQ(window.editor(), left);
    left->setCursorOffset(left->noteText().size());
    QTest::keyClicks(left, QStringLiteral("!"));
    WAIT_TRUE(!left->isModified(), 5000);
    EXPECT_EQ(readAll(path(QStringLiteral("Welcome.md"))), QByteArray("L# Welcome\n\nBody.\nR!"));
}

TEST_F(WindowTest, RenamingANoteOpenInTwoPanesMovesBothAndKeepsItsLineEndings)
{
    MainWindow window;
    window.openVault(dir.path());
    auto [left, right] = splitOn(window, path(QStringLiteral("Projects/Crlf.md")));

    auto *tree = window.findChild<QTreeView *>(QStringLiteral("fileExplorer"));
    auto *model = qobject_cast<QFileSystemModel *>(tree->model());
    const QModelIndex index = model->index(path(QStringLiteral("Projects/Crlf.md")));
    ASSERT_TRUE(index.isValid());
    QTimer::singleShot(0, &window, [] {
        if (auto *dialog = qobject_cast<QInputDialog *>(QApplication::activeModalWidget())) {
            dialog->setTextValue(QStringLiteral("Moved"));
            dialog->accept();
        }
    });
    ASSERT_TRUE(QMetaObject::invokeMethod(&window, "renamePathAt", Q_ARG(QModelIndex, index)));
    ASSERT_TRUE(QFile::exists(path(QStringLiteral("Projects/Moved.md"))));
    for (QTabBar *bar : window.findChildren<QTabBar *>(QStringLiteral("noteTabs")))
        EXPECT_EQ(tabTexts(bar), QStringList{QStringLiteral("Moved")});

    left->setCursorOffset(left->noteText().size());
    QTest::keyClicks(left, QStringLiteral("three"));
    WAIT_TRUE(!left->isModified() && !right->isModified(), 5000);
    EXPECT_EQ(readAll(path(QStringLiteral("Projects/Moved.md"))), QByteArray("one\r\ntwo\r\nthree"));
    EXPECT_FALSE(QFile::exists(path(QStringLiteral("Projects/Crlf.md")))) << "nothing saved to the old name";
}

TEST_F(WindowTest, AChangeOnDiskReachesEveryPaneAndIsNotSavedOver)
{
    MainWindow window;
    window.openVault(dir.path());
    const QString note = path(QStringLiteral("Welcome.md"));
    auto [left, right] = splitOn(window, note);
    right->setCursorOffset(3);

    // A sync client's way: a new file renamed over the old one.
    replaceByRename(note, "# Welcome\n\nChanged by sync.\n");
    WAIT_TRUE(left->noteText() == QStringLiteral("# Welcome\n\nChanged by sync.\n"), 3000);
    EXPECT_EQ(right->noteText(), left->noteText());
    EXPECT_FALSE(left->isModified());
    EXPECT_FALSE(right->isModified());
    EXPECT_EQ(right->cursorOffset(), 3) << "the caret keeps its place";

    // Another editor's way: the same file written in place. Still watched,
    // although the rename above replaced the file being watched.
    overwrite(note, "# Welcome\n\nChanged in place.\n");
    WAIT_TRUE(left->noteText() == QStringLiteral("# Welcome\n\nChanged in place.\n"), 3000);
    EXPECT_EQ(right->noteText(), left->noteText());

    // Typing after it saves on top of it, not over it.
    left->setCursorOffset(left->noteText().size());
    QTest::keyClicks(left, QStringLiteral("typed"));
    WAIT_TRUE(!left->isModified(), 5000);
    EXPECT_EQ(readAll(note), QByteArray("# Welcome\n\nChanged in place.\ntyped"));
}

TEST_F(WindowTest, ItsOwnSavesAreNotTakenForAChangeOnDisk)
{
    MainWindow window;
    window.openVault(dir.path());
    window.openNote(path(QStringLiteral("Welcome.md")));
    MarkdownEditor *editor = window.editor();
    editor->setCursorOffset(editor->noteText().size());
    QTest::keyClicks(editor, QStringLiteral("abc"));
    WAIT_TRUE(!editor->isModified(), 5000);
    QTest::qWait(400); // the watcher's own notice of that save
    EXPECT_EQ(editor->cursorOffset(), editor->noteText().size());
    QTest::keyClick(editor, Qt::Key_Z, Qt::ControlModifier);
    EXPECT_FALSE(editor->noteText().endsWith(QLatin1String("abc"))) << "the undo history is still there";
}

TEST_F(WindowTest, AChangeOnDiskUnderUnsavedTypingKeepsBoth)
{
    MainWindow window;
    window.openVault(dir.path());
    const QString note = path(QStringLiteral("Welcome.md"));
    window.openNote(note);
    MarkdownEditor *editor = window.editor();
    editor->setCursorOffset(editor->noteText().size());
    QTest::keyClicks(editor, QStringLiteral("mine"));
    overwrite(note, "# Welcome\n\nTheirs.\n");
    // What was typed stays, and is what gets saved...
    WAIT_TRUE(!editor->isModified(), 5000);
    EXPECT_EQ(readAll(note), QByteArray("# Welcome\n\nBody.\nmine"));
    // ...but the other version is not lost: file recovery holds it.
    bool kept = false;
    for (const auto &version : noteAppC::Snapshots::versions(note))
        kept = kept || noteAppC::Snapshots::read(version) == QStringLiteral("# Welcome\n\nTheirs.\n");
    EXPECT_TRUE(kept);
}

TEST_F(WindowTest, ANoteDeletedAndRecreatedOutsideIsWatchedAgain)
{
    MainWindow window;
    window.openVault(dir.path());
    const QString note = path(QStringLiteral("Welcome.md"));
    window.openNote(note);
    MarkdownEditor *editor = window.editor();
    ASSERT_TRUE(QFile::remove(note));
    QTest::qWait(300);
    EXPECT_EQ(editor->noteText(), QStringLiteral("# Welcome\n\nBody.\n")) << "a delete changes nothing shown";

    overwrite(note, "# Welcome\n\nBack.\n"); // recreated, with nothing saved from this window since
    WAIT_TRUE(editor->noteText() == QStringLiteral("# Welcome\n\nBack.\n"), 3000);
    EXPECT_FALSE(editor->isModified());
    overwrite(note, "# Welcome\n\nBack again.\n"); // and its new file is watched like the first was
    WAIT_TRUE(editor->noteText() == QStringLiteral("# Welcome\n\nBack again.\n"), 3000);
}

TEST_F(WindowTest, ANoteMovedOutsideIsFollowedWithItsCaretAndUnsavedEdits)
{
    MainWindow window;
    window.openVault(dir.path());
    const QString note = path(QStringLiteral("Welcome.md"));
    const QString moved = path(QStringLiteral("Projects/Elsewhere.md"));
    auto [left, right] = splitOn(window, note);
    left->setCursorOffset(left->noteText().size());
    QTest::keyClicks(left, QStringLiteral("typed"));
    ASSERT_TRUE(left->isModified());
    const int caret = left->cursorOffset();
    ASSERT_TRUE(QFile::rename(note, moved));

    // Both panes follow, and the autosave then writes the new name, not the old.
    WAIT_TRUE(window.windowTitle().contains(QStringLiteral("Elsewhere")), 3000);
    WAIT_TRUE(!left->isModified(), 5000);
    EXPECT_EQ(left->cursorOffset(), caret);
    EXPECT_EQ(readAll(moved), QByteArray("# Welcome\n\nBody.\ntyped"));
    EXPECT_FALSE(QFile::exists(note)) << "the old name is not recreated";
    EXPECT_EQ(right->noteText(), left->noteText());
}

TEST_F(WindowTest, ANoteMovedAndChangedOutsideStaysOnItsOldNameAndIsNotSavedBack)
{
    MainWindow window;
    window.openVault(dir.path());
    const QString note = path(QStringLiteral("Welcome.md"));
    window.openNote(note);
    MarkdownEditor *editor = window.editor();
    editor->setCursorOffset(editor->noteText().size());
    QTest::keyClicks(editor, QStringLiteral("typed")); // unsaved when it goes
    ASSERT_TRUE(editor->isModified());
    ASSERT_TRUE(QFile::rename(note, path(QStringLiteral("Projects/Other.md"))));
    overwrite(path(QStringLiteral("Projects/Other.md")), "# Something else\n");
    WAIT_TRUE(window.statusBar()->currentMessage().contains(QStringLiteral("gone from disk")), 4000);
    EXPECT_TRUE(window.windowTitle().contains(QStringLiteral("Welcome.md")));
    // Well past the autosave: the old name is not written back on its own...
    QTest::qWait(3500);
    EXPECT_FALSE(QFile::exists(note));
    EXPECT_TRUE(editor->isModified());
    EXPECT_EQ(readAll(path(QStringLiteral("Projects/Other.md"))), QByteArray("# Something else\n"));
    // ...but saving it by hand is the person's choice.
    ASSERT_TRUE(QMetaObject::invokeMethod(&window, "save"));
    EXPECT_EQ(readAll(note), QByteArray("# Welcome\n\nBody.\ntyped"));
    EXPECT_FALSE(editor->isModified());
}

// Leaving a lost note asks, even with autosave on: saving would recreate it.
TEST_F(WindowTest, LeavingANoteGoneFromDiskAsksBeforeWritingItBack)
{
    MainWindow window;
    window.openVault(dir.path());
    const QString note = path(QStringLiteral("Welcome.md"));
    window.openNote(note);
    QTest::keyClicks(window.editor(), QStringLiteral("typed"));
    ASSERT_TRUE(QFile::rename(note, path(QStringLiteral("Projects/Other.md"))));
    overwrite(path(QStringLiteral("Projects/Other.md")), "# Something else\n");
    WAIT_TRUE(window.statusBar()->currentMessage().contains(QStringLiteral("gone from disk")), 4000);
    bool asked = false;
    QTimer watch;
    QObject::connect(&watch, &QTimer::timeout, [&asked] {
        if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
            asked = true;
            box->button(QMessageBox::Discard)->click();
        }
    });
    watch.start(20);
    window.openNote(path(QStringLiteral("Projects/Crlf.md")));
    EXPECT_TRUE(asked);
    EXPECT_FALSE(QFile::exists(note));
}

TEST_F(WindowTest, RestoringASnapshotReachesEveryPaneAndKeepsTheFilesBomAndLineEndings)
{
    const QString note = path(QStringLiteral("Both.md"));
    write(QStringLiteral("Both.md"), "\xEF\xBB\xBFnew\r\nbody\r\n");
    noteAppC::Snapshots::record(note, QStringLiteral("old\nbody\n"), 0);
    MainWindow window;
    window.openVault(dir.path());
    auto [left, right] = splitOn(window, note);

    window.findChild<QAction *>(QStringLiteral("file-recovery:open"))->trigger();
    auto *dialog = window.findChild<noteAppC::RecoveryDialog *>();
    ASSERT_NE(dialog, nullptr);
    auto *notes = dialog->findChild<QListWidget *>(QStringLiteral("recoveryNotes"));
    for (int i = 0; i < notes->count(); ++i) {
        if (notes->item(i)->text() == QStringLiteral("Both.md"))
            notes->setCurrentRow(i);
    }
    QTimer::singleShot(0, [] {
        if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget()))
            box->button(QMessageBox::Yes)->click();
    });
    for (QPushButton *button : dialog->findChildren<QPushButton *>()) {
        if (button->text() == QStringLiteral("Restore"))
            button->click();
    }
    EXPECT_EQ(readAll(note), QByteArray("\xEF\xBB\xBFold\r\nbody\r\n"));
    EXPECT_EQ(right->noteText(), QStringLiteral("old\nbody\n"));
    EXPECT_EQ(left->noteText(), QStringLiteral("old\nbody\n")) << "the pane that was not focused too";

    // Typing in the other pane builds on the restored text.
    left->setCursorOffset(left->noteText().size());
    QTest::keyClicks(left, QStringLiteral("more"));
    WAIT_TRUE(!left->isModified() && !right->isModified(), 5000);
    EXPECT_EQ(readAll(note), QByteArray("\xEF\xBB\xBFold\r\nbody\r\nmore"));
}

TEST_F(WindowTest, FrontMatterBomLineEndingsAndALastLineSurviveEverySave)
{
    // A BOM, CRLF, YAML with a comment, a quoted value and a list, and no
    // newline at the end: every save keeps each byte nobody edited.
    const QByteArray original = "\xEF\xBB\xBF---\r\n"
                                "tags: [alpha, beta]\r\n"
                                "aliases:\r\n"
                                "  - Other name # kept\r\n"
                                "title: \"Quoted: yes\"\r\n"
                                "status: draft\r\n"
                                "---\r\n"
                                "Body without a newline";
    write(QStringLiteral("Meta.md"), original);
    const QString note = path(QStringLiteral("Meta.md"));
    MainWindow window;
    QSignalSpy indexed(window.index(), &Index::changed);
    window.openVault(dir.path());
    window.resize(1000, 700);
    window.show();
    ASSERT_TRUE(QTest::qWaitForWindowExposed(&window));
    window.openNote(note);
    MarkdownEditor *editor = window.editor();

    // Explicit save of an untouched note: nothing changes.
    ASSERT_TRUE(QMetaObject::invokeMethod(&window, "save"));
    EXPECT_EQ(readAll(note), original);

    // Autosave of a typed edit.
    editor->setCursorOffset(editor->noteText().size());
    QTest::keyClicks(editor, QStringLiteral("!"));
    WAIT_TRUE(!editor->isModified(), 5000);
    EXPECT_EQ(readAll(note), original + "!");

    // A property edited in its table: that value, and only it.
    const qsizetype status = editor->noteText().indexOf(QLatin1String("status"));
    ASSERT_TRUE(editor->editPropertyAt(status));
    auto *field = editor->viewport()->findChild<QLineEdit *>();
    ASSERT_NE(field, nullptr);
    field->setText(QStringLiteral("final"));
    QTest::keyClick(field, Qt::Key_Return);
    WAIT_TRUE(!editor->isModified(), 5000);
    QByteArray expected = original + "!";
    expected.replace("status: draft", "status: final");
    EXPECT_EQ(readAll(note), expected);

    // The index reads the saved front matter: the alias resolves.
    WAIT_TRUE(!window.index()->resolve(QStringLiteral("Other name")).isEmpty(), 5000);
    EXPECT_EQ(QFileInfo(window.index()->resolve(QStringLiteral("Other name"))).fileName(), QStringLiteral("Meta.md"));
}

TEST_F(WindowTest, ALinkRewrittenByARenameReachesTheOpenNoteThatHoldsIt)
{
    write(QStringLiteral("Target.md"), "# Target\n");
    write(QStringLiteral("Source.md"), "See [[Target]].\n");
    MainWindow window;
    QSignalSpy indexed(window.index(), &Index::changed);
    window.openVault(dir.path());
    ASSERT_TRUE(indexed.wait(5000));
    window.openNote(path(QStringLiteral("Source.md")));
    MarkdownEditor *editor = window.editor();

    auto *tree = window.findChild<QTreeView *>(QStringLiteral("fileExplorer"));
    auto *model = qobject_cast<QFileSystemModel *>(tree->model());
    const QModelIndex index = model->index(path(QStringLiteral("Target.md")));
    QTimer::singleShot(0, &window, [] {
        if (auto *dialog = qobject_cast<QInputDialog *>(QApplication::activeModalWidget())) {
            dialog->setTextValue(QStringLiteral("Renamed"));
            dialog->accept();
        }
    });
    ASSERT_TRUE(QMetaObject::invokeMethod(&window, "renamePathAt", Q_ARG(QModelIndex, index)));
    WAIT_TRUE(editor->noteText() == QStringLiteral("See [[Renamed]].\n"), 3000);

    // The next autosave keeps the rewritten link instead of the old one.
    editor->setCursorOffset(editor->noteText().size());
    QTest::keyClicks(editor, QStringLiteral("x"));
    WAIT_TRUE(!editor->isModified(), 5000);
    EXPECT_EQ(readAll(path(QStringLiteral("Source.md"))), QByteArray("See [[Renamed]].\nx"));
}

// --- encrypted notes: two panes, every place plaintext could land ------------

#ifdef NOTEAPPC_HAVE_ENCRYPTION
namespace {

/// Every file under `root` that holds `needle`, any case.
QStringList filesHolding(const QString &root, const QByteArray &needle)
{
    QStringList found;
    QDirIterator it(root, QDir::Files | QDir::Hidden | QDir::System, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString file = it.next();
        QFile in(file);
        if (in.open(QIODevice::ReadOnly) && in.readAll().toLower().contains(needle.toLower()))
            found << file;
    }
    return found;
}

} // namespace

TEST_F(WindowTest, AnEncryptedNoteInTwoPanesSavesOnlyCiphertextAnywhere)
{
    const QByteArray secret = "zqxjunlikelyword";
    // A word of a plain note does reach the index cache: the scan below
    // would see the secret if it were there.
    write(QStringLiteral("Plain.md"), "zqxjcontrolword\n");
    int asked = 0;
    {
        MainWindow window;
        window.setEncryptionPromptForTests([&asked](bool, bool) {
            ++asked;
            return QStringLiteral("correct horse battery");
        });
        QSignalSpy indexed(window.index(), &Index::changed);
        window.openVault(dir.path());
        window.findChild<QAction *>(QStringLiteral("encrypted-notes:new"))->trigger();
        const QString locked = path(QStringLiteral("Untitled.mdlock"));
        ASSERT_EQ(window.editor()->noteText(), QString());
        MarkdownEditor *left = window.editor();
        window.findChild<QAction *>(QStringLiteral("workspace:split-vertical"))->trigger();
        MarkdownEditor *right = window.editor();
        ASSERT_NE(left, right);

        // Typed in one pane, autosaved, shown in both: ciphertext on disk.
        QTest::keyClicks(right, QString::fromLatin1(secret) + QStringLiteral(" alpha"));
        EXPECT_EQ(left->noteText(), right->noteText());
        const QByteArray before = bytesOf(locked);
        WAIT_TRUE(!left->isModified() && !right->isModified(), 5000);
        EXPECT_NE(bytesOf(locked), before);
        EXPECT_FALSE(bytesOf(locked).contains(secret));
        EXPECT_EQ(asked, 1) << "one password for the session, not one per pane or save";

        // The index has nothing to find, the search pane nothing to show.
        QTest::qWait(500);
        EXPECT_TRUE(window.index()->search(QString::fromLatin1(secret)).isEmpty());

        // Typed in the other pane, then locked before the autosave: the lock
        // saves it first, then closes both.
        left->setCursorOffset(left->noteText().size());
        QTest::keyClicks(left, QStringLiteral(" beta"));
        window.findChild<QAction *>(QStringLiteral("encrypted-notes:lock"))->trigger();
        flushDeletes();
        for (MarkdownEditor *editor : tabEditors(window)) {
            EXPECT_FALSE(editor->noteText().contains(QString::fromLatin1(secret)));
            QTest::keyClick(editor, Qt::Key_Z, Qt::ControlModifier);
            EXPECT_FALSE(editor->noteText().contains(QString::fromLatin1(secret))) << "nothing to undo back";
        }
        ASSERT_TRUE(window.openNote(locked));
        EXPECT_EQ(asked, 2);
        EXPECT_EQ(window.editor()->noteText(), QString::fromLatin1(secret) + QStringLiteral(" alpha beta"));
    }
    // Nowhere: not the vault (temporary files included), the index cache,
    // file recovery, the note history, settings or the workspace.
    EXPECT_EQ(filesHolding(dir.path(), secret), QStringList());
    for (const char *home : {"XDG_CACHE_HOME", "XDG_DATA_HOME", "XDG_CONFIG_HOME"})
        EXPECT_EQ(filesHolding(qEnvironmentVariable(home), secret), QStringList()) << home;
    EXPECT_FALSE(filesHolding(qEnvironmentVariable("XDG_CACHE_HOME"), "zqxjcontrolword").isEmpty());
}
#endif

#ifdef NOTEAPPC_HAVE_ENCRYPTION
TEST_F(WindowTest, EncryptingANoteInTwoPanesKeepsBothAndItsLineEndingsComeBackDecrypted)
{
    MainWindow window;
    window.setEncryptionPromptForTests([](bool, bool) { return QStringLiteral("a long password"); });
    window.openVault(dir.path());
    auto [left, right] = splitOn(window, path(QStringLiteral("Projects/Crlf.md")));
    clickNextBox(QStringLiteral("Encrypt"));
    window.findChild<QAction *>(QStringLiteral("encrypted-notes:encrypt"))->trigger();
    const QString locked = path(QStringLiteral("Projects/Crlf.mdlock"));
    ASSERT_TRUE(QFileInfo::exists(locked));
    for (QTabBar *bar : window.findChildren<QTabBar *>(QStringLiteral("noteTabs")))
        EXPECT_EQ(tabTexts(bar), QStringList{QStringLiteral("Crlf")});

    // The pane that did not encrypt it saves to the encrypted file.
    const QByteArray before = bytesOf(locked);
    left->setCursorOffset(left->noteText().size());
    QTest::keyClicks(left, QStringLiteral("three"));
    WAIT_TRUE(!left->isModified() && !right->isModified(), 5000);
    EXPECT_NE(bytesOf(locked), before);
    EXPECT_FALSE(QFileInfo::exists(path(QStringLiteral("Projects/Crlf.md"))));

    // Decrypted again: the CRLF went through the ciphertext and back.
    QTimer::singleShot(0, [] {
        if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget()))
            box->button(QMessageBox::Yes)->click();
    });
    window.findChild<QAction *>(QStringLiteral("encrypted-notes:decrypt"))->trigger();
    EXPECT_FALSE(QFileInfo::exists(locked));
    EXPECT_EQ(readAll(path(QStringLiteral("Projects/Crlf.md"))), QByteArray("one\r\ntwo\r\nthree"));
}
#endif

#ifdef NOTEAPPC_AI_PLUGIN_DIR
// A plugin's question (host->prompt) is a small dialog of the window's own:
// OK sends the line back, Esc and the plugin's timeout both answer "cancelled".
// The AI plugin's "Ask a question" is the real asker; a blank line sent back
// ends before any network call, so nothing leaves the process.
TEST_F(WindowTest, APluginsQuestionIsADialogThatOKEscAndTheTimeoutAllAnswer)
{
    QTemporaryDir data;
    const QByteArray previous = qgetenv("XDG_DATA_HOME");
    qputenv("XDG_DATA_HOME", data.path().toUtf8());
    qputenv("NOTEAPPC_PLUGIN_HOST", NOTEAPPC_PLUGIN_HOST_PATH);
    const QString installed = data.path() + QStringLiteral("/noteAppC/plugins/ai");
    ASSERT_TRUE(QDir().mkpath(installed));
    for (const QString file : {QStringLiteral("plugin.ini"), QStringLiteral("libai.so")})
        ASSERT_TRUE(QFile::copy(QStringLiteral(NOTEAPPC_AI_PLUGIN_DIR "/") + file, installed + u'/' + file));
    const QString configHome = qEnvironmentVariable("XDG_CONFIG_HOME") + QStringLiteral("/noteAppC");
    const QString grants = configHome + QStringLiteral("/plugins.ini");
    const QString aiConfig = configHome + QStringLiteral("/plugins/ai/ai.ini");
    ASSERT_TRUE(QDir().mkpath(QFileInfo(aiConfig).absolutePath()));
    {
        QFile file(aiConfig);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        file.write("[ai]\nask-timeout-ms = 1000\n"); // the plugin's own floor
        QSettings config(grants, QSettings::IniFormat);
        config.setValue(QStringLiteral("ai/enabled"), true);
    }
    Settings on = Settings::load();
    on.setValue(QStringLiteral("plugins.communityPlugins"), true);
    ASSERT_TRUE(on.save());
    {
        MainWindow window;
        window.openTarget(noteAppC::resolveLaunchTarget(dir.path()));
        window.show();
        QAction *ask = nullptr;
        WAIT_TRUE((ask = window.findChild<QAction *>(QStringLiteral("ai:ask-question"))), 5000);
        const auto notice = [&window] { return window.statusBar()->currentMessage(); };
        const auto dialog = [&window] { return window.findChild<QInputDialog *>(); };
        const auto asked = [&](const char *what) {
            window.statusBar()->clearMessage();
            ask->trigger();
            QTest::qWaitFor([&] { return dialog() != nullptr; }, 15000);
            EXPECT_NE(dialog(), nullptr) << what;
            return dialog();
        };

        // OK with a blank line: answered, nothing typed (a cancel says something else).
        QInputDialog *box = asked("ok");
        ASSERT_NE(box, nullptr);
        EXPECT_EQ(box->windowTitle(), QStringLiteral("AI")) << "the app names the asker";
        EXPECT_FALSE(box->labelText().isEmpty());
        EXPECT_EQ(box->textEchoMode(), QLineEdit::Normal);
        box->setTextValue(QStringLiteral("   "));
        box->accept();
        WAIT_TRUE(notice().contains(QStringLiteral("nothing typed")), 15000);
        WAIT_TRUE(dialog() == nullptr, 5000); // deleted on close

        // Esc: cancelled.
        box = asked("esc");
        ASSERT_NE(box, nullptr);
        QTest::keyClick(box, Qt::Key_Escape);
        WAIT_TRUE(notice().contains(QStringLiteral("cancelled")), 15000);
        WAIT_TRUE(dialog() == nullptr, 5000);

        // Nobody answers: the dialog closes itself at the plugin's timeout, and that is a cancel too.
        box = asked("timeout");
        ASSERT_NE(box, nullptr);
        QElapsedTimer waited;
        waited.start();
        WAIT_TRUE(notice().contains(QStringLiteral("cancelled")), 15000);
        EXPECT_GE(waited.elapsed(), 500) << "it was not cancelled at once";
        EXPECT_EQ(dialog(), nullptr);
    }
    Settings off = Settings::load();
    off.setValue(QStringLiteral("plugins.communityPlugins"), false);
    ASSERT_TRUE(off.save());
    QFile::remove(grants);
    QFile::remove(aiConfig);
    qputenv("XDG_DATA_HOME", previous);
}
#endif

#if defined(NOTEAPPC_HAVE_ENCRYPTION) && defined(NOTEAPPC_WORD_STATS_PLUGIN_DIR)
TEST_F(WindowTest, APluginReadsNothingOfAnEncryptedNote)
{
    QTemporaryDir data;
    const QByteArray previous = qgetenv("XDG_DATA_HOME");
    qputenv("XDG_DATA_HOME", data.path().toUtf8());
    qputenv("NOTEAPPC_PLUGIN_HOST", NOTEAPPC_PLUGIN_HOST_PATH);
    const QString installed = data.path() + QStringLiteral("/noteAppC/plugins/word-stats");
    ASSERT_TRUE(QDir().mkpath(installed));
    for (const QString file : {QStringLiteral("plugin.ini"), QStringLiteral("libword-stats.so")})
        ASSERT_TRUE(QFile::copy(QStringLiteral(NOTEAPPC_WORD_STATS_PLUGIN_DIR "/") + file, installed + u'/' + file));
    const QString grants = qEnvironmentVariable("XDG_CONFIG_HOME") + QStringLiteral("/noteAppC/plugins.ini");
    {
        QSettings config(grants, QSettings::IniFormat);
        config.setValue(QStringLiteral("word-stats/enabled"), true);
        config.setValue(QStringLiteral("word-stats/permissions.readNotes"), true);
    }
    Settings on = Settings::load();
    on.setValue(QStringLiteral("plugins.communityPlugins"), true);
    ASSERT_TRUE(on.save());
    write(QStringLiteral("Three.md"), "one two three\n");
    {
        MainWindow window;
        window.setEncryptionPromptForTests([](bool, bool) { return QStringLiteral("a long password"); });
        window.openTarget(noteAppC::resolveLaunchTarget(dir.path()));
        window.show();
        WAIT_TRUE(window.findChild<QAction *>(QStringLiteral("word-stats:count-words")), 5000);
        QAction *count = window.findChild<QAction *>(QStringLiteral("word-stats:count-words"));
        const auto notice = [&window] { return window.statusBar()->currentMessage(); };

        // A plain note: the plugin reads its text.
        window.openNote(path(QStringLiteral("Three.md")));
        count->trigger();
        QTest::qWaitFor([&] { return notice() == QStringLiteral("3 words"); }, 5000);
        ASSERT_EQ(notice(), QStringLiteral("3 words"));

        // An encrypted one: it reads nothing.
        window.findChild<QAction *>(QStringLiteral("encrypted-notes:new"))->trigger();
        QTest::keyClicks(window.editor(), QStringLiteral("secret words here"));
        window.statusBar()->clearMessage();
        count->trigger();
        WAIT_TRUE(notice().endsWith(QLatin1String(" words")), 5000);
        EXPECT_EQ(notice(), QStringLiteral("0 words"));
    }
    Settings off = Settings::load();
    off.setValue(QStringLiteral("plugins.communityPlugins"), false);
    ASSERT_TRUE(off.save());
    QFile::remove(grants);
    qputenv("XDG_DATA_HOME", previous);
}
#endif

// --- every kind of block and embed renders inline, through the real window ---

TEST_F(WindowTest, EveryKindOfBlockAndEmbedRendersInline)
{
    {
        QImage picture(200, 120, QImage::Format_RGB32);
        picture.fill(QColor(255, 0, 0));
        ASSERT_TRUE(picture.save(path(QStringLiteral("pic.png"))));
        QPdfWriter pdf(path(QStringLiteral("doc.pdf")));
        QPainter painter(&pdf);
        painter.drawText(100, 100, QStringLiteral("page one"));
    }
    write(QStringLiteral("Other.md"), "Intro of other.\n\n# Part two\n\nThe second part. ^blk\n");
    write(QStringLiteral("song.mp3"), "not really audio");
    write(QStringLiteral("clip.mp4"), "not really video");
    write(QStringLiteral("book.epub"), QByteArray("PK\x03\x04", 4) + QByteArray(64, '\0')); // a zip, as a book is
    write(QStringLiteral("Tagged.md"), "#project here\n");
#ifdef NOTEAPPC_HAVE_CANVAS
    write(QStringLiteral("Board.canvas"),
          R"({"nodes":[{"id":"a","type":"text","text":"card","x":0,"y":0,"width":200,"height":100}],"edges":[]})");
#endif
#ifdef NOTEAPPC_HAVE_BASES
    write(QStringLiteral("Tasks.base"), "views:\n  - type: table\n    order: [file.name]\n");
#endif
    write(QStringLiteral("Plain.md"), "intro\n\nplain\n\nafter\n");

    using Probe = std::function<bool(MarkdownEditor *, qsizetype)>;
    struct Kind {
        QString name;
        QString markdown; ///< goes between "intro" and "after"
        QString anchor;   ///< where in it the probe looks
        Probe rendered;
    };
    const auto image = [](MarkdownEditor *e, qsizetype at) { return !e->embedImageSizeAt(at).isEmpty(); };
    const auto paintsRed = [](MarkdownEditor *e, qsizetype) {
        const QImage shown = e->viewport()->grab().toImage().convertToFormat(QImage::Format_RGB32);
        int red = 0;
        for (int y = 0; y < shown.height(); y += 4) {
            for (int x = 0; x < shown.width(); x += 4)
                red += shown.pixel(x, y) == qRgb(255, 0, 0);
        }
        return red > 100; // the picture itself, not a red underline
    };
    const auto shownWithout = [](const QString &source) {
        return [source](MarkdownEditor *e, qsizetype) { return !e->displayText().contains(source); };
    };
    qreal plainHeight = 0;
    QList<Kind> kinds = {
        {QStringLiteral("image"), QStringLiteral("![[pic.png]]"), QStringLiteral("!"), paintsRed},
        {QStringLiteral("note"), QStringLiteral("![[Other]]"), QStringLiteral("!"), image},
        {QStringLiteral("note heading"), QStringLiteral("![[Other#Part two]]"), QStringLiteral("!"), image},
        {QStringLiteral("note block"), QStringLiteral("![[Other#^blk]]"), QStringLiteral("!"), image},
        {QStringLiteral("audio"), QStringLiteral("![[song.mp3]]"), QStringLiteral("!"),
         [&plainHeight](MarkdownEditor *e, qsizetype at) {
             return e->isMediaCardAt(at) && e->documentHeight() > plainHeight + 20;
         }},
        {QStringLiteral("video"), QStringLiteral("![[clip.mp4]]"), QStringLiteral("!"),
         [&plainHeight](MarkdownEditor *e, qsizetype at) {
             return e->isMediaCardAt(at) && e->documentHeight() > plainHeight + 200;
         }},
        {QStringLiteral("epub, as a card"), QStringLiteral("![[book.epub]]"), QStringLiteral("!"),
         [](MarkdownEditor *e, qsizetype at) { return e->isFileCardAt(at); }},
        {QStringLiteral("callout"), QStringLiteral("> [!note] Title\n> Callout body."), QStringLiteral(">"),
         shownWithout(QStringLiteral("[!note]"))},
        {QStringLiteral("table"), QStringLiteral("| a | b |\n|---|---|\n| 1 | 2 |"), QStringLiteral("|"),
         [](MarkdownEditor *e, qsizetype at) { return e->tableRenderedAt(at); }},
        {QStringLiteral("task"), QStringLiteral("- [ ] a task"), QStringLiteral("-"),
         shownWithout(QStringLiteral("[ ]"))},
        {QStringLiteral("footnote"), QStringLiteral("Text[^1].\n\n[^1]: The note."), QStringLiteral("T"),
         [](MarkdownEditor *e, qsizetype) { return e->footnoteMarkTexts().contains(QStringLiteral("1")); }},
        {QStringLiteral("query"), QStringLiteral("```query\ntag:#project\n```"), QStringLiteral("tag:"),
         [](MarkdownEditor *e, qsizetype at) { return !e->queryRowsAt(at).isEmpty(); }},
    };
    if (noteAppC::PdfPage::isAvailable()) {
        kinds.append({QStringLiteral("pdf"), QStringLiteral("![[doc.pdf]]"), QStringLiteral("!"),
                      [](MarkdownEditor *e, qsizetype at) {
                          const auto info = e->pdfEmbedInfoAt(at);
                          return info.isPdfEmbed && info.pageCount == 1 && info.error.isEmpty();
                      }});
    }
    if (noteAppC::MathRenderer::isAvailable()) {
        kinds.append({QStringLiteral("display maths"), QStringLiteral("$$\nE = mc^2\n$$"), QStringLiteral("E"),
                      shownWithout(QStringLiteral("E = mc^2"))});
        kinds.append({QStringLiteral("inline maths"), QStringLiteral("Inline $e^{i\\pi}$ maths."),
                      QStringLiteral("I"), shownWithout(QStringLiteral("\\pi"))});
    }
    if (noteAppC::CodeHighlighter::isAvailable()) {
        kinds.append({QStringLiteral("highlighted code"), QStringLiteral("```cpp\nreturn 42;\n```"),
                      QStringLiteral("return"), [](MarkdownEditor *e, qsizetype at) {
                          const QColor code = e->formatAt(at).foreground().color();
                          const QColor prose = e->formatAt(0).foreground().color();
                          return e->formatAt(at).hasProperty(QTextFormat::ForegroundBrush) && code != prose;
                      }});
    }
    if (noteAppC::HtmlRenderer::isAvailable()) {
        kinds.append({QStringLiteral("html block"), QStringLiteral("<div style=\"color: red\">An HTML block</div>"),
                      QStringLiteral("<"),
                      [](MarkdownEditor *e, qsizetype at) { return !e->htmlImageSizeAt(at).isEmpty(); }});
    }
#ifdef NOTEAPPC_HAVE_CANVAS
    kinds.append({QStringLiteral("canvas"), QStringLiteral("![[Board.canvas]]"), QStringLiteral("!"), image});
#endif
#ifdef NOTEAPPC_HAVE_BASES
    kinds.append({QStringLiteral("base"), QStringLiteral("![[Tasks.base]]"), QStringLiteral("!"),
                  [](MarkdownEditor *e, qsizetype at) { return !e->baseRowsAt(at).isEmpty(); }});
#endif
    for (int i = 0; i < kinds.size(); ++i)
        write(QStringLiteral("Kind %1.md").arg(i), ("intro\n\n" + kinds.at(i).markdown + "\n\nafter\n").toUtf8());

    MainWindow window;
    QSignalSpy indexed(window.index(), &Index::changed);
    window.openVault(dir.path());
    ASSERT_TRUE(indexed.wait(5000));
    window.resize(1200, 900);
    window.show(); // layout, and so rendering, only happens in a shown widget
    ASSERT_TRUE(QTest::qWaitForWindowExposed(&window));
    ASSERT_TRUE(window.openNote(path(QStringLiteral("Plain.md"))));
    QTest::qWait(50);
    plainHeight = window.editor()->documentHeight();

    for (int i = 0; i < kinds.size(); ++i) {
        const Kind &kind = kinds.at(i);
        ASSERT_TRUE(window.openNote(path(QStringLiteral("Kind %1.md").arg(i))));
        MarkdownEditor *editor = window.editor();
        editor->setCursorOffset(0); // in "intro": the block itself is not being edited
        const qsizetype at = editor->noteText().indexOf(kind.anchor, 7);
        EXPECT_TRUE(QTest::qWaitFor([&] { return kind.rendered(editor, at); }, 5000))
            << kind.name.toStdString() << " is not rendered inline: " << editor->displayText().toStdString();
    }

    // Properties are the note's first block, so they have a note of their own.
    write(QStringLiteral("Props.md"), "---\nstatus: draft\n---\nintro\n");
    ASSERT_TRUE(window.openNote(path(QStringLiteral("Props.md"))));
    window.editor()->setCursorOffset(window.editor()->noteText().size());
    EXPECT_TRUE(QTest::qWaitFor([&] { return !window.editor()->displayText().contains(QStringLiteral("status:")); },
                                3000))
        << "properties are not a table";
}
