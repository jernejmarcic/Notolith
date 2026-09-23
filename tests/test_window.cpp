// Widget smoke test: drives the real MainWindow on the offscreen platform, so
// the Phase 0 exit criteria (open vault, file tree, open note, save) are
// checked by something other than a human looking at the screen.

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
#include <functional>

#include <QAbstractButton>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QPlainTextEdit>
#include <QDir>
#include <QElapsedTimer>
#include <QFileSystemModel>
#include <QFile>
#include <QFileInfo>
#include <QInputDialog>
#include <QLabel>
#include <QLayout>
#include <QLineEdit>
#include <QDialog>
#include <QPushButton>
#include <QListWidget>
#include <QMessageBox>
#include <QScrollBar>
#include <QSplitter>
#include <QStackedWidget>
#include <QTabBar>
#include <QTest>
#include <QTimer>
#include <QToolButton>

#include "WaitFor.h"
#include "MarkdownEditor.h"
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
#include "BacklinksPane.h"
#include "ToggleSwitch.h"
#include "QuickSwitcher.h"
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

TEST_F(WindowTest, ClickingATagSearchesForItAndAResultOpensTheNoteAtTheMatch)
{
    write(QStringLiteral("A.md"), "Intro line.\n#project #project/alpha\n");
    write(QStringLiteral("B.md"), "#project\n");
    write(QStringLiteral("C.md"), "#area\n");

    MainWindow window;
    QSignalSpy indexed(window.index(), &Index::changed);
    window.openVault(dir.path());
    ASSERT_TRUE(indexed.wait(5000));

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
    // Configured, but no password this session: no engine, no dialog, no network.
    EXPECT_TRUE(status->isVisibleTo(&window));
    EXPECT_EQ(status->text(), QStringLiteral("Sync: not signed in"));
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
