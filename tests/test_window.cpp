// Widget smoke test: drives the real MainWindow on the offscreen platform, so
// the Phase 0 exit criteria (open vault, file tree, open note, save) are
// checked by something other than a human looking at the screen.

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <future>
#include <thread>
#include <sys/socket.h>
#include <sys/un.h>
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
#include <QDrag>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFileSystemModel>
#include <QHeaderView>
#include <QHoverEvent>
#include <QListView>
#include <QTreeView>
#include <QFile>
#include <QFileInfo>
#include <QInputDialog>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QPointer>
#include <QScopeGuard>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QLayout>
#include <QLineEdit>
#include <QContextMenuEvent>
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

#include "InkTestSupport.h"
#include "WaitFor.h"
#include "SingleInstance.h"
#include "FileTree.h"
#include "MarkdownEditor.h"
#include "MetaNotesPane.h"
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
#include <QCryptographicHash>
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
#include "Indexer.h"
#include "Workspaces.h"
#include "CodeHighlighter.h"
#include "HtmlRenderer.h"
#include "MathRenderer.h"
#include "PdfPage.h"
#include "PublishDialog.h"
#include "MainWindow.h"
#include "PagePreview.h"
#include "PopoutWindow.h"
#include "OutlinePane.h"
#include "BookmarksPane.h"
#include "BacklinksPane.h"
#include "ModuleHost.h"
#include "ModuleRegistry.h"
#include "ToggleSwitch.h"
#include "QuickSwitcher.h"
#include "Snapshots.h"
#include "FileIcons.h"
#include "RecoveryDialog.h"
#include "Settings.h"
#ifdef NOTEAPPC_EXPECT_SYNC
#include "SyncEngine.h"
#endif
#include "Vault.h"

using noteAppC::Index;
using noteAppC::LaunchTarget;
using noteAppC::MainWindow;
using noteAppC::SingleInstance;
using noteAppC::MarkdownEditor;
using noteAppC::Settings;

namespace {

/// Settings builds a page when it is first shown: look on each in turn, the
/// way a person clicks through the nav.
template <typename T>
T *settingsWidget(QWidget &window, const QString &name)
{
    if (T *found = window.findChild<T *>(name))
        return found;
    auto *nav = window.findChild<QListWidget *>(QStringLiteral("settingsNav"));
    if (!nav)
        return nullptr;
    for (int row = 0; row < nav->count(); ++row) {
        nav->setCurrentRow(row);
        if (T *found = window.findChild<T *>(name))
            return found;
    }
    return nullptr;
}

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
    EXPECT_TRUE(window.windowTitle().endsWith(QStringLiteral(" - Notolith")));
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
    ASSERT_TRUE(QTest::qWaitFor([&] { return graph->containsNode(QStringLiteral("B.md")); }, 5000)); // read on a worker
    EXPECT_TRUE(graph->containsNode(QStringLiteral("A.md")));

    emit graph->noteActivated(QStringLiteral("B.md"));
    EXPECT_EQ(stack->currentIndex(), 0);
    EXPECT_EQ(window.findChild<MarkdownEditor *>()->noteText(), QStringLiteral("b\n"));

    command->trigger();
    EXPECT_EQ(stack->currentWidget(), graph);
    command->trigger(); // again: back to the note
    EXPECT_EQ(stack->currentIndex(), 0);
}

// Escape on the graph page goes back to the note it was opened from.
TEST_F(WindowTest, EscapeLeavesTheGraphForTheNoteItWasOpenedFrom)
{
    write(QStringLiteral("A.md"), "[[B]]\n");
    write(QStringLiteral("B.md"), "b\n");
    MainWindow window;
    QSignalSpy indexed(window.index(), &Index::changed);
    window.openVault(dir.path());
    ASSERT_TRUE(indexed.wait(5000));
    window.show();
    ASSERT_TRUE(QTest::qWaitForWindowExposed(&window));
    window.openNote(path(QStringLiteral("A.md")));

    window.findChild<QAction *>(QStringLiteral("graph:open"))->trigger();
    auto *graph = window.findChild<GraphView *>(QStringLiteral("graphView"));
    auto *stack = window.findChild<QStackedWidget *>(QStringLiteral("mainStack"));
    ASSERT_NE(graph, nullptr);
    ASSERT_EQ(stack->currentWidget(), graph);
    QTest::keyClick(graph, Qt::Key_Escape);
    EXPECT_EQ(stack->currentIndex(), 0);
    EXPECT_EQ(window.findChild<MarkdownEditor *>()->noteText(), QStringLiteral("[[B]]\n"));
}

// The node menu's "Open in new tab" opens the note beside the current tab.
TEST_F(WindowTest, GraphOpenInNewTabAddsATab)
{
    write(QStringLiteral("A.md"), "[[B]]\n");
    write(QStringLiteral("B.md"), "b\n");
    MainWindow window;
    QSignalSpy indexed(window.index(), &Index::changed);
    window.openVault(dir.path());
    ASSERT_TRUE(indexed.wait(5000));
    window.show();
    ASSERT_TRUE(QTest::qWaitForWindowExposed(&window));
    window.openNote(path(QStringLiteral("A.md")));
    auto *tabs = window.findChild<QTabBar *>(QStringLiteral("noteTabs"));
    ASSERT_NE(tabs, nullptr);
    const int before = tabs->count();

    window.findChild<QAction *>(QStringLiteral("graph:open"))->trigger();
    auto *graph = window.findChild<GraphView *>(QStringLiteral("graphView"));
    ASSERT_NE(graph, nullptr);
    emit graph->noteActivatedInNewTab(QStringLiteral("B.md"));
    EXPECT_EQ(window.findChild<QStackedWidget *>(QStringLiteral("mainStack"))->currentIndex(), 0);
    EXPECT_EQ(tabs->count(), before + 1);
    EXPECT_EQ(tabs->tabText(tabs->currentIndex()), QStringLiteral("B"));
}

// The header carries a graph button: nothing on screen opened the graph before.
TEST_F(WindowTest, TheHeaderGraphButtonOpensTheGraph)
{
    write(QStringLiteral("A.md"), "[[B]]\n");
    write(QStringLiteral("B.md"), "b\n");
    MainWindow window;
    QSignalSpy indexed(window.index(), &Index::changed);
    window.openVault(dir.path());
    ASSERT_TRUE(indexed.wait(5000));
    window.show();
    ASSERT_TRUE(QTest::qWaitForWindowExposed(&window));

    auto *button = window.findChild<QToolButton *>(QStringLiteral("headerGraphButton"));
    ASSERT_NE(button, nullptr);
    EXPECT_TRUE(button->isVisible());
    button->click();
    auto *graph = window.findChild<GraphView *>(QStringLiteral("graphView"));
    ASSERT_NE(graph, nullptr);
    EXPECT_EQ(window.findChild<QStackedWidget *>(QStringLiteral("mainStack"))->currentWidget(), graph);
}

// The graph controls are the app's own copy under the config directory; the
// vault's .obsidian/graph.json is read, never written.
TEST_F(WindowTest, GraphControlsSurviveARestartInTheAppsOwnCopy)
{
    QTemporaryDir config;
    const QByteArray previous = qgetenv("XDG_CONFIG_HOME");
    qputenv("XDG_CONFIG_HOME", config.path().toUtf8());
    write(QStringLiteral("A.md"), "[[B]]\n");
    write(QStringLiteral("B.md"), "b\n");
    {
        MainWindow window;
        window.openVault(dir.path());
        window.findChild<QAction *>(QStringLiteral("graph:open"))->trigger();
        auto *graph = window.findChild<GraphView *>(QStringLiteral("graphView"));
        ASSERT_NE(graph, nullptr);
        EXPECT_FALSE(graph->options().showTags);
        noteAppC::GraphOptions options = graph->options();
        options.showTags = true;
        options.search = QStringLiteral("path:A");
        graph->setOptions(options);
        emit graph->optionsChanged(); // what a control does
        QDir saved(config.path() + QStringLiteral("/noteAppC/vaults"));
        ASSERT_TRUE(QTest::qWaitFor([&] { return !saved.entryList(QDir::Dirs | QDir::NoDotAndDotDot).isEmpty(); }, 3000));
    }
    EXPECT_FALSE(QFileInfo::exists(path(QStringLiteral(".obsidian/graph.json")))) << "the vault's file is never written";
    {
        MainWindow window;
        window.openVault(dir.path());
        window.findChild<QAction *>(QStringLiteral("graph:open"))->trigger();
        auto *graph = window.findChild<GraphView *>(QStringLiteral("graphView"));
        ASSERT_NE(graph, nullptr);
        EXPECT_TRUE(graph->options().showTags);
        EXPECT_EQ(graph->options().search, QStringLiteral("path:A"));
    }
    qputenv("XDG_CONFIG_HOME", previous);
}

// The save is debounced 500 ms; opening another vault inside that window used
// to leave the change unwritten (the timer then saved the new vault's options).
TEST_F(WindowTest, AGraphChangeJustBeforeAVaultSwitchIsSavedForItsOwnVault)
{
    QTemporaryDir config;
    QTemporaryDir other;
    const QByteArray previous = qgetenv("XDG_CONFIG_HOME");
    qputenv("XDG_CONFIG_HOME", config.path().toUtf8());
    write(QStringLiteral("A.md"), "a\n");
    {
        MainWindow window;
        window.openVault(dir.path());
        window.findChild<QAction *>(QStringLiteral("graph:open"))->trigger();
        auto *graph = window.findChild<GraphView *>(QStringLiteral("graphView"));
        ASSERT_NE(graph, nullptr);
        noteAppC::GraphOptions options = graph->options();
        options.repelStrength = 42;
        graph->setOptions(options);
        emit graph->optionsChanged(); // what a control does
        window.openVault(other.path()); // well inside the 500 ms
        QTest::qWait(900);
    }
    const QString key = QString::fromLatin1(QCryptographicHash::hash(QDir(dir.path()).absolutePath().toUtf8(),
                                                                     QCryptographicHash::Sha1).toHex());
    QFile saved(config.path() + QStringLiteral("/noteAppC/vaults/%1/graph.json").arg(key));
    ASSERT_TRUE(saved.open(QIODevice::ReadOnly)) << "the first vault's change was never written";
    EXPECT_EQ(QJsonDocument::fromJson(saved.readAll()).object().value(QStringLiteral("repelStrength")).toDouble(), 42.0);
    qputenv("XDG_CONFIG_HOME", previous);
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
    ASSERT_TRUE(QTest::qWaitFor([&] { return local->containsNode(QStringLiteral("B.md")); }, 5000));
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

// --- pop-out windows (offscreen: nothing here reaches a real display) -------

namespace {

/// Every pop-out window of the process, hidden or not, once deletions have run.
QList<noteAppC::PopoutWindow *> popoutWindows()
{
    flushDeletes();
    QList<noteAppC::PopoutWindow *> found;
    for (QWidget *top : QApplication::topLevelWidgets()) {
        if (auto *popout = qobject_cast<noteAppC::PopoutWindow *>(top))
            found << popout;
    }
    return found;
}

QAction *command(MainWindow &window, const char *id)
{
    return window.findChild<QAction *>(QLatin1String(id));
}

QList<QTabBar *> mainBars(MainWindow &window)
{
    return window.findChildren<QTabBar *>(QStringLiteral("noteTabs"));
}

} // namespace

TEST_F(WindowTest, OpenInNewWindowShowsTheNoteInAWindowOfItsOwn)
{
    MainWindow window;
    window.openVault(dir.path());
    window.openNote(path(QStringLiteral("Welcome.md")));

    command(window, "workspace:open-in-new-window")->trigger();
    const auto popouts = popoutWindows();
    ASSERT_EQ(popouts.size(), 1);
    EXPECT_TRUE(popouts.first()->isVisible());
    EXPECT_EQ(popouts.first()->parentWidget(), nullptr) << "a top-level of its own, not a child";
    const auto editors = popouts.first()->findChildren<MarkdownEditor *>();
    ASSERT_EQ(editors.size(), 1);
    EXPECT_EQ(editors.first()->noteText(), QStringLiteral("# Welcome\n\nBody.\n"));
    EXPECT_EQ(tabEditors(window).size(), 1);
    EXPECT_TRUE(popouts.first()->windowTitle().startsWith(QStringLiteral("Welcome")));
    EXPECT_EQ(window.editor(), editors.first()) << "the new tab is the one the window follows";
}

TEST_F(WindowTest, MoveToNewWindowTakesTheTabAndCollapsesItsSplit)
{
    write(QStringLiteral("Other.md"), "other note text\n");
    MainWindow window;
    window.openVault(dir.path());
    window.openNote(path(QStringLiteral("Welcome.md")));
    command(window, "workspace:split-vertical")->trigger();
    window.openNote(path(QStringLiteral("Other.md")));
    ASSERT_EQ(mainBars(window).size(), 2);

    command(window, "workspace:move-to-new-window")->trigger();
    const auto popouts = popoutWindows();
    ASSERT_EQ(popouts.size(), 1);
    EXPECT_EQ(mainBars(window).size(), 1) << "the split Other left is gone";
    EXPECT_EQ(tabEditors(window).size(), 1);
    EXPECT_EQ(tabEditors(window).first()->noteText(), QStringLiteral("# Welcome\n\nBody.\n"));
    const auto editors = popouts.first()->findChildren<MarkdownEditor *>();
    ASSERT_EQ(editors.size(), 1);
    EXPECT_EQ(editors.first()->noteText(), QStringLiteral("other note text\n"));
    EXPECT_EQ(window.editor(), editors.first());
}

TEST_F(WindowTest, MovingTheLastTabOutLeavesAnEmptyTabInMain)
{
    MainWindow window;
    window.openVault(dir.path());
    window.openNote(path(QStringLiteral("Welcome.md")));

    command(window, "workspace:move-to-new-window")->trigger();
    const auto popouts = popoutWindows();
    ASSERT_EQ(popouts.size(), 1);
    // Main is never left without a tab, as when the only tab is closed.
    const auto bars = mainBars(window);
    ASSERT_EQ(bars.size(), 1);
    EXPECT_EQ(tabTexts(bars.first()), QStringList{QStringLiteral("New tab")});
    ASSERT_EQ(tabEditors(window).size(), 1);
    EXPECT_FALSE(tabEditors(window).first()->isEnabled()) << "nothing open in it";
    ASSERT_EQ(popouts.first()->findChildren<MarkdownEditor *>().size(), 1);
    EXPECT_EQ(popouts.first()->findChildren<MarkdownEditor *>().first()->noteText(),
              QStringLiteral("# Welcome\n\nBody.\n"));
}

TEST_F(WindowTest, ClosingAPopoutSavesItsTextAndClosesItsTabs)
{
    write(QStringLiteral("Other.md"), "other note text\n");
    // Autosave on explicitly: a close that had to ask would hang on a message box.
    Settings settings = Settings::load();
    settings.setValue(QStringLiteral("files.autosave"), true);
    ASSERT_TRUE(settings.save());
    MainWindow window;
    window.openVault(dir.path());
    window.openNote(path(QStringLiteral("Welcome.md")));
    command(window, "workspace:new-tab")->trigger();
    window.openNote(path(QStringLiteral("Other.md")));
    command(window, "workspace:move-to-new-window")->trigger();
    auto popouts = popoutWindows();
    ASSERT_EQ(popouts.size(), 1);
    MarkdownEditor *inPopout = window.editor();
    inPopout->setCursorOffset(0);
    QTest::keyClicks(inPopout, QStringLiteral("x"));

    popouts.first()->close();
    EXPECT_EQ(readAll(path(QStringLiteral("Other.md"))), QByteArray("xother note text\n"));
    EXPECT_TRUE(popoutWindows().isEmpty());
    EXPECT_EQ(tabEditors(window).size(), 1);
    EXPECT_EQ(window.editor()->noteText(), QStringLiteral("# Welcome\n\nBody.\n"));

    command(window, "workspace:undo-close-pane")->trigger();
    EXPECT_EQ(tabEditors(window).size(), 2) << "reopened in the main window, the pop-out being gone";
    EXPECT_EQ(window.editor()->noteText(), QStringLiteral("xother note text\n"));
    EXPECT_TRUE(popoutWindows().isEmpty());
}

TEST_F(WindowTest, ClosingTheMainWindowClosesEveryPopout)
{
    MainWindow window;
    window.show();
    window.openVault(dir.path());
    window.openNote(path(QStringLiteral("Welcome.md")));
    command(window, "workspace:open-in-new-window")->trigger();
    command(window, "workspace:open-in-new-window")->trigger();
    ASSERT_EQ(popoutWindows().size(), 2);
    command(window, "app:detach-left-sidebar")->trigger(); // the splitter has two children now
    ASSERT_EQ(popoutWindows().size(), 3);

    window.close();
    for (const noteAppC::PopoutWindow *popout : popoutWindows())
        EXPECT_FALSE(popout->isVisible());
    // The splitter state saved is the three-child main one (the sidebar was docked
    // before it was saved), which a fresh window restores.
    QDataStream saved(QSettings().value(QStringLiteral("window/splitter")).toByteArray());
    qint32 magic = 0, version = 0;
    QList<int> sizes;
    saved >> magic >> version >> sizes; // what QSplitter::saveState() starts with
    EXPECT_EQ(sizes.size(), 3);
    MainWindow fresh;
    auto *splitter = fresh.findChild<QSplitter *>(QStringLiteral("mainSplitter"));
    ASSERT_NE(splitter, nullptr);
    EXPECT_EQ(splitter->count(), 3);
    EXPECT_TRUE(splitter->restoreState(QSettings().value(QStringLiteral("window/splitter")).toByteArray()));
}

TEST_F(WindowTest, HotkeysWorkInsideAPopout)
{
    MainWindow window;
    window.openVault(dir.path());
    window.openNote(path(QStringLiteral("Welcome.md")));
    command(window, "workspace:move-to-new-window")->trigger();
    const auto popouts = popoutWindows();
    ASSERT_EQ(popouts.size(), 1);
    noteAppC::PopoutWindow *popout = popouts.first();
    // The same QAction objects, so a rebinding and setEnabled() reach it too.
    EXPECT_TRUE(popout->actions().contains(command(window, "workspace:new-tab")));
    EXPECT_TRUE(popout->actions().contains(command(window, "workspace:move-to-new-window")));

    ASSERT_TRUE(QTest::qWaitForWindowExposed(popout));
    popout->activateWindow();
    ASSERT_TRUE(QTest::qWaitForWindowActive(popout));
    QTest::keyClick(popout->findChild<MarkdownEditor *>(), Qt::Key_T, Qt::ControlModifier);
    const auto bars = popout->findChildren<QTabBar *>(QStringLiteral("noteTabs"));
    ASSERT_EQ(bars.size(), 1);
    EXPECT_EQ(bars.first()->count(), 2) << "Ctrl+T made a tab in the window it was pressed in";
    EXPECT_EQ(tabEditors(window).size(), 1) << "the tab opened in the pop-out, not in main";
}

TEST_F(WindowTest, ATabsContextMenuOffersTheWindowCommands)
{
    MainWindow window;
    window.show();
    window.openVault(dir.path());
    window.openNote(path(QStringLiteral("Welcome.md")));
    auto *tabs = mainBars(window).first();
    QStringList offered;
    QTimer::singleShot(0, [&offered] {
        auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
        if (!menu)
            return;
        for (const QAction *action : menu->actions())
            offered << action->text();
        menu->close();
    });
    const QPoint at = tabs->tabRect(0).center();
    QContextMenuEvent event(QContextMenuEvent::Mouse, at, tabs->mapToGlobal(at));
    QCoreApplication::sendEvent(tabs, &event);
    EXPECT_TRUE(offered.contains(QStringLiteral("Open in new window")));
    EXPECT_TRUE(offered.contains(QStringLiteral("Move to new window")));
    EXPECT_TRUE(offered.contains(QStringLiteral("Split right")));
}

TEST_F(WindowTest, PopoutWindowsOffBuildsNothingAndDocksEverything)
{
    write(QStringLiteral("Other.md"), "other note text\n");
    QTemporaryDir config; // the switch is the user's, kept apart from other tests
    const QByteArray previous = qgetenv("XDG_CONFIG_HOME");
    qputenv("XDG_CONFIG_HOME", config.path().toUtf8());
    {
        MainWindow window;
        window.show(); // the settings page builds its rows when shown
        window.openVault(dir.path());
        window.openNote(path(QStringLiteral("Welcome.md")));
        command(window, "workspace:new-tab")->trigger();
        window.openNote(path(QStringLiteral("Other.md")));
        command(window, "workspace:move-to-new-window")->trigger();
        command(window, "app:detach-left-sidebar")->trigger();
        ASSERT_EQ(popoutWindows().size(), 2);
        EXPECT_TRUE(command(window, "workspace:open-in-new-window")->isEnabled());
        EXPECT_TRUE(command(window, "app:detach-right-sidebar")->isEnabled());

        command(window, "app:open-settings")->trigger();
        noteAppC::ToggleSwitch *toggle = nullptr;
        ASSERT_TRUE(QTest::qWaitFor([&] {
            return (toggle = settingsWidget<noteAppC::ToggleSwitch>(window, QStringLiteral("settingsToggle_plugins.popoutWindows")));
        }));
        toggle->click();

        EXPECT_TRUE(popoutWindows().isEmpty()) << "off builds no window";
        EXPECT_EQ(tabEditors(window).size(), 2) << "Other came back to the main window";
        for (const QWidget *group : window.findChildren<QWidget *>(QStringLiteral("tabGroup")))
            EXPECT_FALSE(group->acceptDrops());
        EXPECT_FALSE(command(window, "workspace:open-in-new-window")->isEnabled());
        EXPECT_FALSE(command(window, "workspace:move-to-new-window")->isEnabled());
        EXPECT_FALSE(command(window, "app:detach-left-sidebar")->isEnabled());
        EXPECT_FALSE(command(window, "app:detach-right-sidebar")->isEnabled());
        auto *splitter = window.findChild<QSplitter *>(QStringLiteral("mainSplitter"));
        EXPECT_EQ(splitter->count(), 3) << "the sidebar came back to the splitter";
        EXPECT_EQ(window.findChild<QWidget *>(QStringLiteral("sidebar"))->window(), &window);

        toggle->click(); // back on: the commands work again
        EXPECT_TRUE(command(window, "workspace:move-to-new-window")->isEnabled());
        EXPECT_TRUE(command(window, "app:detach-left-sidebar")->isEnabled());
        for (const QWidget *group : window.findChildren<QWidget *>(QStringLiteral("tabGroup")))
            EXPECT_TRUE(group->acceptDrops());
    }
    qputenv("XDG_CONFIG_HOME", previous);
}

// --- detached sidebars and panes ---------------------------------------------

namespace {

/// Right-clicks `at` in `anchor` and takes the menu item `text`, if the menu has it.
void chooseFromContextMenu(QWidget *anchor, const QPoint &at, const QString &text)
{
    QTimer::singleShot(0, [text] {
        auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
        if (!menu)
            return;
        for (QAction *action : menu->actions()) {
            if (action->text() == text)
                action->trigger();
        }
        menu->close();
    });
    QContextMenuEvent event(QContextMenuEvent::Mouse, at, anchor->mapToGlobal(at));
    QCoreApplication::sendEvent(anchor, &event);
}

} // namespace

TEST_F(WindowTest, DetachingTheLeftSidebarPutsItInItsOwnWindowAndBackOnClose)
{
    MainWindow window;
    window.show();
    window.openVault(dir.path());
    auto *splitter = window.findChild<QSplitter *>(QStringLiteral("mainSplitter"));
    auto *sidebar = window.findChild<QWidget *>(QStringLiteral("sidebar"));
    ASSERT_EQ(splitter->count(), 3);

    command(window, "app:detach-left-sidebar")->trigger();
    const auto popouts = popoutWindows();
    ASSERT_EQ(popouts.size(), 1);
    EXPECT_EQ(popouts.first()->kind(), noteAppC::PopoutWindow::Kind::Sidebar);
    EXPECT_EQ(splitter->count(), 2);
    EXPECT_EQ(sidebar->window(), popouts.first());
    EXPECT_TRUE(sidebar->isVisible());
    EXPECT_NE(popouts.first()->findChild<QTreeView *>(QStringLiteral("fileExplorer")), nullptr);
    command(window, "app:toggle-left-sidebar")->trigger();
    EXPECT_TRUE(sidebar->isVisible()) << "the toggle leaves a detached sidebar alone";

    popouts.first()->close();
    EXPECT_TRUE(popoutWindows().isEmpty());
    EXPECT_EQ(splitter->count(), 3);
    EXPECT_EQ(splitter->indexOf(sidebar), 0) << "back where it was built";
    EXPECT_EQ(sidebar->window(), &window);
    EXPECT_TRUE(sidebar->isVisible());

    // The same through the context menu of the header.
    auto *header = window.findChild<QWidget *>(QStringLiteral("sidebarHeader"));
    chooseFromContextMenu(header, header->rect().center(), QStringLiteral("Detach sidebar"));
    const auto again = popoutWindows();
    ASSERT_EQ(again.size(), 1);
    EXPECT_EQ(sidebar->window(), again.first());
}

TEST_F(WindowTest, DetachingAPaneHidesItsTabAndShowRightPaneRaisesTheWindow)
{
    write(QStringLiteral("Target.md"), "Target text.\n");
    write(QStringLiteral("Source.md"), "See [[Target]].\n");
    MainWindow window;
    QSignalSpy indexed(window.index(), &Index::changed);
    window.openVault(dir.path());
    ASSERT_TRUE(indexed.wait(5000));
    window.show();
    ASSERT_TRUE(QTest::qWaitForWindowExposed(&window));
    window.openNote(path(QStringLiteral("Target.md")));
    auto *panes = window.findChild<QStackedWidget *>(QStringLiteral("rightSidebarPanes"));
    auto *pane = window.findChild<QWidget *>(QStringLiteral("backlinksPane"));
    auto *tab = window.findChild<QToolButton *>(QStringLiteral("backlinksTabButton"));
    ASSERT_TRUE(pane && tab);
    const auto mentions = [pane] { return pane->findChildren<QTreeWidget *>(QStringLiteral("backlinkList")).first()->topLevelItemCount(); };
    WAIT_TRUE(mentions() == 1, 5000);
    const int before = panes->count();

    auto *tabRow = window.findChild<QWidget *>(QStringLiteral("rightSidebarTabs"));
    chooseFromContextMenu(tabRow, tab->mapTo(tabRow, tab->rect().center()), QStringLiteral("Detach pane"));
    const auto popouts = popoutWindows();
    ASSERT_EQ(popouts.size(), 1);
    EXPECT_EQ(popouts.first()->kind(), noteAppC::PopoutWindow::Kind::Pane);
    EXPECT_EQ(pane->window(), popouts.first());
    EXPECT_TRUE(popouts.first()->isVisible());
    EXPECT_TRUE(pane->isVisible());
    EXPECT_EQ(panes->indexOf(pane), -1);
    EXPECT_EQ(panes->count(), before - 1);
    EXPECT_FALSE(tab->isVisible()) << "its tab went with it";
    EXPECT_TRUE(tab->isHidden());
    QToolButton *current = nullptr;
    for (auto *button : window.findChildren<QToolButton *>()) {
        if (button->isChecked() && button->property("pane").value<QObject *>() == panes->currentWidget())
            current = button;
    }
    EXPECT_NE(current, nullptr) << "the pane the sidebar shows now has its tab checked";

    // Asking for the pane brings its window; the sidebar's own stack does not switch.
    QWidget *stillShowing = panes->currentWidget();
    window.findChild<QToolButton *>(QStringLiteral("backlinkStatus"))->click();
    EXPECT_TRUE(popouts.first()->isVisible());
    EXPECT_EQ(panes->currentWidget(), stillShowing);
    EXPECT_FALSE(tab->isChecked());
    EXPECT_TRUE(current->isChecked());

    // The pane is still fed: opening Source (nothing links to it) empties it.
    window.openNote(path(QStringLiteral("Source.md")));
    WAIT_TRUE(mentions() == 0, 5000);

    popouts.first()->close();
    EXPECT_TRUE(popoutWindows().isEmpty());
    EXPECT_EQ(pane->window(), &window);
    EXPECT_GE(panes->indexOf(pane), 0);
    EXPECT_TRUE(tab->isVisible());
}

TEST_F(WindowTest, SwitchingAFeatureOffClosesTheWindowOfItsDetachedPane)
{
    QTemporaryDir config; // the switch is the user's, kept apart from other tests
    const QByteArray previous = qgetenv("XDG_CONFIG_HOME");
    qputenv("XDG_CONFIG_HOME", config.path().toUtf8());
    {
        MainWindow window;
        window.show(); // the settings page builds its rows when shown
        window.openVault(dir.path());
        auto *tab = window.findChild<QToolButton *>(QStringLiteral("backlinksTabButton"));
        auto *tabRow = window.findChild<QWidget *>(QStringLiteral("rightSidebarTabs"));
        ASSERT_TRUE(tab);
        chooseFromContextMenu(tabRow, tab->mapTo(tabRow, tab->rect().center()), QStringLiteral("Detach pane"));
        ASSERT_EQ(popoutWindows().size(), 1);

        command(window, "app:open-settings")->trigger();
        noteAppC::ToggleSwitch *toggle = nullptr;
        ASSERT_TRUE(QTest::qWaitFor([&] {
            return (toggle = settingsWidget<noteAppC::ToggleSwitch>(window, QStringLiteral("settingsToggle_plugins.backlink")));
        }));
        toggle->click(); // applyFeatures() deletes the pane; its window has nothing to show

        EXPECT_TRUE(QTest::qWaitFor([] { return popoutWindows().isEmpty(); })) << "closed once the pane is gone";
        EXPECT_EQ(window.findChild<QWidget *>(QStringLiteral("backlinksPane")), nullptr);
    }
    qputenv("XDG_CONFIG_HOME", previous);
}

TEST_F(WindowTest, ANarrowWindowLeavesADetachedSidebarAlone)
{
    MainWindow window;
    window.resize(1180, 760);
    window.show();
    window.openVault(dir.path());
    auto *sidebar = window.findChild<QWidget *>(QStringLiteral("sidebar"));
    auto *right = window.findChild<QWidget *>(QStringLiteral("rightSidebar"));
    command(window, "app:detach-right-sidebar")->trigger();
    const auto popouts = popoutWindows();
    ASSERT_EQ(popouts.size(), 1);
    ASSERT_TRUE(right->isVisible());

    window.resize(500, 760); // a phone held upright: the sidebar beside the note folds
    QApplication::processEvents();
    EXPECT_FALSE(sidebar->isVisible());
    EXPECT_TRUE(right->isVisible()) << "in its own window it has all the width it wants";
    EXPECT_EQ(right->window(), popouts.first());

    window.resize(1180, 760);
    QApplication::processEvents();
    EXPECT_TRUE(sidebar->isVisible());
    EXPECT_TRUE(right->isVisible());
    EXPECT_EQ(right->window(), popouts.first()) << "still detached";
    EXPECT_EQ(popoutWindows().size(), 1);
}


// --- drag-to-tile -----------------------------------------------------------
// Offscreen cannot start a platform drag, so QDrag::exec, the synthetic release
// that ends QTabBar's own move, a drag between two real windows, the overlay's
// painting and the drop-outside-every-window path (newPopout, tested above) are
// not exercised here: the tests deliver the enter and drop events themselves.

namespace {

struct DropResult {
    bool entered = false;
    bool dropped = false;
};

/// What the platform sends a drop target: an enter, then the drop, at `at`.
DropResult dropOn(QWidget *target, const QMimeData *mime, const QPoint &at, bool sendDrop = true)
{
    DropResult result;
    QDragEnterEvent enter(at, Qt::MoveAction, mime, Qt::LeftButton, Qt::NoModifier);
    enter.ignore();
    QCoreApplication::sendEvent(target, &enter);
    result.entered = enter.isAccepted();
    if (sendDrop) {
        QDropEvent drop(QPointF(at), Qt::MoveAction, mime, Qt::LeftButton, Qt::NoModifier);
        drop.ignore();
        QCoreApplication::sendEvent(target, &drop);
        result.dropped = drop.isAccepted();
    }
    return result;
}

} // namespace

TEST_F(WindowTest, ADraggedSidebarTabIsInsideAWindowAndOnlyDetachesOutsideAll)
{
    MainWindow window;
    window.show();
    window.openVault(dir.path());
    ASSERT_TRUE(QTest::qWaitForWindowExposed(&window));
    auto *panes = window.findChild<QStackedWidget *>(QStringLiteral("rightSidebarPanes"));
    auto *pane = window.findChild<QWidget *>(QStringLiteral("backlinksPane"));
    ASSERT_TRUE(panes && pane);
    const std::unique_ptr<QMimeData> mime(window.paneDragMimeForTests(pane));
    ASSERT_NE(mime, nullptr);
    EXPECT_TRUE(mime->hasFormat(QStringLiteral("application/x-notolith-pane")));

    // Over a window the pane drag is inside: accepted so the window hears it
    // leave, no tile zone, and a drop there does nothing.
    const DropResult over = dropOn(&window, mime.get(), window.rect().center());
    EXPECT_TRUE(over.entered);
    EXPECT_TRUE(over.dropped);
    EXPECT_EQ(window.findChild<QWidget *>(QStringLiteral("dropOverlay")), nullptr);
    EXPECT_NE(panes->indexOf(pane), -1);
    EXPECT_TRUE(popoutWindows().isEmpty());
    // Over a split's body too, where a tab would tile.
    auto *group = window.findChild<QWidget *>(QStringLiteral("tabGroup"));
    EXPECT_TRUE(dropOn(group, mime.get(), QPoint(group->width() - 5, group->height() / 2)).dropped);
    EXPECT_EQ(mainBars(window).size(), 1) << "a pane is not a tab: no split";
    EXPECT_TRUE(popoutWindows().isEmpty());

    // An address that is no pane, or text, is foreign.
    QMimeData stale;
    stale.setData(QStringLiteral("application/x-notolith-pane"), "12345");
    EXPECT_FALSE(dropOn(&window, &stale, window.rect().center()).entered);
    QMimeData text;
    text.setText(QStringLiteral("x"));
    EXPECT_FALSE(dropOn(&window, &text, window.rect().center()).entered);

    // A pane already in a window of its own cannot be dragged out of the sidebar.
    auto *tab = window.findChild<QToolButton *>(QStringLiteral("backlinksTabButton"));
    auto *tabRow = window.findChild<QWidget *>(QStringLiteral("rightSidebarTabs"));
    chooseFromContextMenu(tabRow, tab->mapTo(tabRow, tab->rect().center()), QStringLiteral("Detach pane"));
    ASSERT_EQ(popoutWindows().size(), 1);
    EXPECT_FALSE(dropOn(&window, mime.get(), window.rect().center()).entered);
}

TEST_F(WindowTest, DraggingASidebarTabOutOfEveryWindowDetachesItsPane)
{
    MainWindow window;
    window.show();
    window.openVault(dir.path());
    ASSERT_TRUE(QTest::qWaitForWindowExposed(&window));
    auto *panes = window.findChild<QStackedWidget *>(QStringLiteral("rightSidebarPanes"));
    auto *pane = window.findChild<QWidget *>(QStringLiteral("backlinksPane"));
    auto *tab = window.findChild<QToolButton *>(QStringLiteral("backlinksTabButton"));
    ASSERT_TRUE(panes && pane && tab);
    const QPoint at = tab->rect().center();

    // A press and a short move does not start a drag (a click stays a click).
    QTest::mousePress(tab, Qt::LeftButton, {}, at);
    QTest::mouseMove(tab, at + QPoint(2, 0));
    QTest::mouseRelease(tab, Qt::LeftButton, {}, at);
    EXPECT_NE(panes->indexOf(pane), -1);
    EXPECT_TRUE(popoutWindows().isEmpty());

    // Pulled past the drag distance the real QDrag starts. The offscreen
    // platform has no drag of its own, so exec() comes back at once as a drop
    // outside every window - which is the path under test (the press, the
    // move, the synthetic release, the detach). What a compositor does with
    // the pixmap, the cursor and a drop over a window is hand-only; that
    // case is the mime test above.
    QTest::mousePress(tab, Qt::LeftButton, {}, at);
    QTest::mouseMove(tab, at + QPoint(40, 0));
    const auto popouts = popoutWindows();
    ASSERT_EQ(popouts.size(), 1);
    EXPECT_EQ(popouts.first()->kind(), noteAppC::PopoutWindow::Kind::Pane);
    EXPECT_EQ(pane->window(), popouts.first());
    EXPECT_EQ(panes->indexOf(pane), -1);
    EXPECT_TRUE(tab->isHidden());
    EXPECT_FALSE(tab->isDown()) << "the platform swallowed the release; the synthetic one ended the press";
}

namespace {

/// Stands in for Qt's own drag, which eats every key from an application
/// filter it installs after ours, inside QDrag::exec().
class KeyEater : public QObject {
public:
    bool eventFilter(QObject *, QEvent *event) override { return event->type() == QEvent::KeyPress; }
};

} // namespace

TEST_F(WindowTest, EscapeDuringADragIsSeenAndCancelsIt)
{
    // The decision: a drag that no group took and that never was over a
    // window opens one, unless Escape cancelled it. The real drag, and the
    // pointer outside every window with an Escape, is hand-only: the
    // offscreen platform has no drag to cancel.
    EXPECT_TRUE(MainWindow::dropOpensWindow(false, false, false));
    EXPECT_FALSE(MainWindow::dropOpensWindow(true, false, false)) << "a group took it";
    EXPECT_FALSE(MainWindow::dropOpensWindow(false, true, false)) << "let go over a window";
    EXPECT_FALSE(MainWindow::dropOpensWindow(false, false, true)) << "Escape";

    MainWindow window;
    window.show();
    window.openVault(dir.path());
    ASSERT_TRUE(QTest::qWaitForWindowExposed(&window));
    KeyEater drag; // installed after the window's filter, so it runs first: as Qt's drag does
    qApp->installEventFilter(&drag);

    QTest::keyClick(window.editor(), Qt::Key_Escape);
    EXPECT_FALSE(window.dragEscapedForTests()) << "no drag is running: nothing is watched";

    window.watchDragForTests(true); // what dragEndedOutside() does from inside exec()
    QTest::keyClick(window.editor(), Qt::Key_A);
    EXPECT_FALSE(window.dragEscapedForTests()) << "only Escape cancels";
    QTest::keyClick(window.editor(), Qt::Key_Escape);
    EXPECT_TRUE(window.dragEscapedForTests()) << "ahead of the drag's own filter, which eats the key";

    window.watchDragForTests(false);
    QTest::keyClick(window.editor(), Qt::Key_Escape);
    EXPECT_FALSE(window.dragEscapedForTests());
    qApp->removeEventFilter(&drag);
}

TEST_F(WindowTest, DropZoneIsPickedByEdgeQuarters)
{
    const QRect rect(0, 0, 400, 300);
    EXPECT_EQ(MainWindow::dropZoneAt(rect, {10, 150}), MainWindow::DropZone::Left);
    EXPECT_EQ(MainWindow::dropZoneAt(rect, {390, 150}), MainWindow::DropZone::Right);
    EXPECT_EQ(MainWindow::dropZoneAt(rect, {200, 10}), MainWindow::DropZone::Top);
    EXPECT_EQ(MainWindow::dropZoneAt(rect, {200, 290}), MainWindow::DropZone::Bottom);
    EXPECT_EQ(MainWindow::dropZoneAt(rect, {200, 150}), MainWindow::DropZone::Center);
    // The quarter ends at 100 px of 400: just inside it is an edge, at it the centre.
    EXPECT_EQ(MainWindow::dropZoneAt(rect, {99, 150}), MainWindow::DropZone::Left);
    EXPECT_EQ(MainWindow::dropZoneAt(rect, {100, 150}), MainWindow::DropZone::Center);
    // A corner goes to the nearer edge, and a rect off the origin is measured from its own corner.
    EXPECT_EQ(MainWindow::dropZoneAt(QRect(50, 50, 400, 300), {60, 90}), MainWindow::DropZone::Left);
    EXPECT_EQ(MainWindow::dropZoneAt(QRect(50, 50, 400, 300), {200, 55}), MainWindow::DropZone::Top);
}

TEST_F(WindowTest, DroppingATabOnASplitsRightEdgeSplitsAndMovesIt)
{
    write(QStringLiteral("Other.md"), "other note text\n");
    MainWindow window;
    window.openVault(dir.path());
    window.show();
    window.openNote(path(QStringLiteral("Welcome.md")));
    command(window, "workspace:new-tab")->trigger();
    window.openNote(path(QStringLiteral("Other.md")));
    auto *root = window.findChild<QSplitter *>(QStringLiteral("workspace"));
    auto *group = window.findChild<QWidget *>(QStringLiteral("tabGroup"));
    auto *tabs = group->findChild<QTabBar *>(QStringLiteral("noteTabs"));
    ASSERT_EQ(tabs->count(), 2);
    const std::unique_ptr<QMimeData> mime(window.tabDragMimeForTests(tabs, 1));
    ASSERT_NE(mime, nullptr);

    const QPoint edge(group->width() - 5, group->height() / 2);
    const DropResult result = dropOn(group, mime.get(), edge, false);
    EXPECT_TRUE(result.entered);
    auto *overlay = window.findChild<QWidget *>(QStringLiteral("dropOverlay"));
    ASSERT_NE(overlay, nullptr) << "the zone is shown while the tab is over it";
    EXPECT_TRUE(overlay->isVisible());
    EXPECT_GE(overlay->geometry().left(), group->width() / 2 - 1) << "the right half";
    QDropEvent drop{QPointF(edge), Qt::MoveAction, mime.get(), Qt::LeftButton, Qt::NoModifier};
    drop.ignore();
    QCoreApplication::sendEvent(group, &drop);
    EXPECT_TRUE(drop.isAccepted());
    EXPECT_FALSE(overlay->isVisible());
    flushDeletes();

    ASSERT_EQ(mainBars(window).size(), 2);
    EXPECT_EQ(root->orientation(), Qt::Horizontal) << "right is beside, not below";
    ASSERT_EQ(root->count(), 2);
    EXPECT_EQ(tabTexts(root->widget(0)->findChild<QTabBar *>()), QStringList{QStringLiteral("Welcome")});
    EXPECT_EQ(tabTexts(root->widget(1)->findChild<QTabBar *>()), QStringList{QStringLiteral("Other")});
    EXPECT_EQ(tabEditors(window).size(), 2) << "the tab moved, none was made or lost";
    EXPECT_EQ(window.editor()->noteText(), QStringLiteral("other note text\n"));
}

TEST_F(WindowTest, DroppingATabOnTheCenterMovesItIntoThatGroup)
{
    write(QStringLiteral("Other.md"), "other note text\n");
    MainWindow window;
    window.openVault(dir.path());
    window.show();
    window.openNote(path(QStringLiteral("Welcome.md")));
    command(window, "workspace:split-vertical")->trigger();
    window.openNote(path(QStringLiteral("Other.md")));
    auto *root = window.findChild<QSplitter *>(QStringLiteral("workspace"));
    ASSERT_EQ(root->count(), 2);
    QWidget *first = root->widget(0);
    QWidget *second = root->widget(1);
    auto *firstTabs = first->findChild<QTabBar *>();
    ASSERT_EQ(firstTabs->count(), 1);
    const std::unique_ptr<QMimeData> mime(window.tabDragMimeForTests(firstTabs, 0));

    // Over the editor, which takes drops of its own (files): the group still gets the tab.
    QWidget *viewport = second->findChild<MarkdownEditor *>()->viewport();
    const DropResult result = dropOn(viewport, mime.get(), viewport->rect().center());
    EXPECT_TRUE(result.entered);
    EXPECT_TRUE(result.dropped);
    flushDeletes();

    ASSERT_EQ(mainBars(window).size(), 1) << "the group the tab left was emptied and went";
    EXPECT_EQ(root->count(), 1);
    EXPECT_EQ(tabTexts(mainBars(window).first()),
              (QStringList{QStringLiteral("Other"), QStringLiteral("Welcome")}));
    EXPECT_EQ(window.editor()->noteText(), QStringLiteral("# Welcome\n\nBody.\n"));
}

TEST_F(WindowTest, DropsIgnoreForeignMimeAndAreOffWhenTheFeatureIs)
{
    write(QStringLiteral("Other.md"), "other note text\n");
    QTemporaryDir config; // the switch is the user's, kept apart from other tests
    const QByteArray previous = qgetenv("XDG_CONFIG_HOME");
    qputenv("XDG_CONFIG_HOME", config.path().toUtf8());
    {
        MainWindow window;
        window.show();
        window.openVault(dir.path());
        window.openNote(path(QStringLiteral("Welcome.md")));
        command(window, "workspace:new-tab")->trigger();
        window.openNote(path(QStringLiteral("Other.md")));
        auto *group = window.findChild<QWidget *>(QStringLiteral("tabGroup"));
        auto *tabs = group->findChild<QTabBar *>(QStringLiteral("noteTabs"));
        const QPoint edge(group->width() - 5, group->height() / 2);

        QMimeData stranger;
        stranger.setText(QStringLiteral("not a tab"));
        const DropResult foreign = dropOn(group, &stranger, edge);
        EXPECT_FALSE(foreign.entered);
        EXPECT_FALSE(foreign.dropped);
        EXPECT_EQ(mainBars(window).size(), 1);
        EXPECT_EQ(window.findChild<QWidget *>(QStringLiteral("dropOverlay")), nullptr) << "no zone for what is not a tab";
        // A tab id that names nothing (the tab is gone) is foreign too.
        QMimeData stale;
        stale.setData(QStringLiteral("application/x-notolith-leaf"), "12345");
        EXPECT_FALSE(dropOn(group, &stale, edge).dropped);

        command(window, "app:open-settings")->trigger();
        noteAppC::ToggleSwitch *toggle = nullptr;
        ASSERT_TRUE(QTest::qWaitFor([&] {
            return (toggle = settingsWidget<noteAppC::ToggleSwitch>(window, QStringLiteral("settingsToggle_plugins.popoutWindows")));
        }));
        toggle->click();
        EXPECT_FALSE(window.acceptDrops());
        for (const QWidget *each : window.findChildren<QWidget *>(QStringLiteral("tabGroup")))
            EXPECT_FALSE(each->acceptDrops());

        // A group that still accepts drops (set by hand here) is refused by the filter itself.
        group->setAcceptDrops(true);
        const std::unique_ptr<QMimeData> mime(window.tabDragMimeForTests(tabs, 1));
        ASSERT_NE(mime, nullptr);
        const DropResult off = dropOn(group, mime.get(), edge);
        EXPECT_FALSE(off.entered);
        EXPECT_FALSE(off.dropped);
        flushDeletes();
        EXPECT_EQ(mainBars(window).size(), 1) << "off, a tab dropped on an edge splits nothing";
        EXPECT_EQ(tabs->count(), 2);
        group->setAcceptDrops(false);

        toggle->click(); // back on
        EXPECT_TRUE(window.acceptDrops());
        EXPECT_TRUE(dropOn(group, mime.get(), edge).dropped);
    }
    qputenv("XDG_CONFIG_HOME", previous);
}

// Typing with no pause long enough for the 2 s timer still saves: the max
// wait counts from the first edit after a save. The pause timer is set far
// beyond the test, so a save can only be the max wait's - however slow the
// machine is, no gap in the typing can let the pause timer do it.
TEST_F(WindowTest, ContinuousTypingIsSavedAtTheMaxWaitNotOnlyAtThePause)
{
    MainWindow window;
    window.setAutosaveIntervalsForTests(600000, 500);
    window.openVault(dir.path());
    const QString note = path(QStringLiteral("Welcome.md"));
    window.openNote(note);
    const QByteArray before = readAll(note);
    MarkdownEditor *editor = window.editor();
    editor->setCursorOffset(editor->noteText().size());
    QElapsedTimer typing;
    typing.start();
    bool savedWhileTyping = false;
    while (!savedWhileTyping && typing.elapsed() < 20000) { // a key every 40 ms until the save, or give up
        QTest::keyClicks(editor, QStringLiteral("x"));
        QTest::qWait(40);
        savedWhileTyping = readAll(note) != before;
    }
    EXPECT_TRUE(savedWhileTyping) << "the max wait never saved while the typing went on";
}

TEST_F(WindowTest, UndoCloseTabReopensInTheSameSplitWithTheCaret)
{
    write(QStringLiteral("Other.md"), "other note text\n");
    write(QStringLiteral("Third.md"), "third note text\n");
    MainWindow window;
    window.show(); // focus moves between the splits only in a shown window
    window.openVault(dir.path());
    window.openNote(path(QStringLiteral("Welcome.md")));
    window.findChild<QAction *>(QStringLiteral("workspace:split-vertical"))->trigger();
    const QList<QTabBar *> bars = window.findChildren<QTabBar *>(QStringLiteral("noteTabs"));
    ASSERT_EQ(bars.size(), 2);
    window.openNote(path(QStringLiteral("Other.md")));
    window.findChild<QAction *>(QStringLiteral("workspace:new-tab"))->trigger();
    window.openNote(path(QStringLiteral("Third.md")));
    QTabBar *group = nullptr; // the split that holds Third
    for (QTabBar *bar : bars)
        if (tabTexts(bar).contains(QStringLiteral("Third")))
            group = bar;
    ASSERT_NE(group, nullptr);
    EXPECT_EQ(tabTexts(group), (QStringList{QStringLiteral("Other"), QStringLiteral("Third")}));
    window.editor()->setCursorOffset(7);

    window.findChild<QAction *>(QStringLiteral("workspace:close"))->trigger();
    flushDeletes();
    EXPECT_EQ(tabTexts(group), QStringList{QStringLiteral("Other")});
    // Focus the other split: the note still returns to the one it left.
    for (MarkdownEditor *candidate : tabEditors(window))
        if (candidate->noteText().startsWith(QStringLiteral("# Welcome")))
            candidate->setFocus();
    QApplication::processEvents();
    ASSERT_TRUE(window.editor()->noteText().startsWith(QStringLiteral("# Welcome")));
    auto *undo = window.findChild<QAction *>(QStringLiteral("workspace:undo-close-pane"));
    undo->trigger();
    EXPECT_EQ(tabTexts(group), (QStringList{QStringLiteral("Other"), QStringLiteral("Third")}));
    EXPECT_EQ(window.editor()->noteText(), QStringLiteral("third note text\n"));
    EXPECT_EQ(window.editor()->cursorOffset(), 7);
    // Nothing more to undo: a further press changes nothing.
    undo->trigger();
    EXPECT_EQ(tabTexts(group).size(), 2);
}

TEST_F(WindowTest, UndoCloseTabRestoresTheScrollPosition)
{
    QByteArray longNote;
    for (int i = 0; i < 400; ++i)
        longNote += "line " + QByteArray::number(i) + "\n\n";
    write(QStringLiteral("Long.md"), longNote);
    MainWindow window;
    window.resize(900, 500);
    window.show();
    window.openVault(dir.path());
    window.openNote(path(QStringLiteral("Welcome.md")));
    window.findChild<QAction *>(QStringLiteral("workspace:new-tab"))->trigger();
    window.openNote(path(QStringLiteral("Long.md")));
    QScrollBar *bar = window.editor()->verticalScrollBar();
    WAIT_TRUE(bar->maximum() > 2000, 5000);
    bar->setValue(1500);
    ASSERT_EQ(bar->value(), 1500);

    window.findChild<QAction *>(QStringLiteral("workspace:close"))->trigger();
    flushDeletes();
    EXPECT_EQ(window.editor()->noteText(), QStringLiteral("# Welcome\n\nBody.\n"));
    window.findChild<QAction *>(QStringLiteral("workspace:undo-close-pane"))->trigger();
    ASSERT_TRUE(window.editor()->noteText().startsWith(QStringLiteral("line 0")));
    WAIT_EQ(window.editor()->verticalScrollBar()->value(), 1500, 5000);
}

#ifdef NOTEAPPC_HAVE_ENCRYPTION
// An encrypted note closed and then locked is passed over by undo, without a
// password prompt out of nowhere, and stays on the list for when it is unlocked.
TEST_F(WindowTest, UndoCloseTabSkipsALockedEncryptedNoteButKeepsIt)
{
    MainWindow window;
    int asked = 0;
    window.setEncryptionPromptForTests([&asked](bool, bool) {
        ++asked;
        return QStringLiteral("correct horse battery");
    });
    window.openVault(dir.path());
    window.openNote(path(QStringLiteral("Welcome.md")));
    window.findChild<QAction *>(QStringLiteral("workspace:new-tab"))->trigger();
    window.findChild<QAction *>(QStringLiteral("encrypted-notes:new"))->trigger();
    const QString locked = path(QStringLiteral("Untitled.mdlock"));
    ASSERT_TRUE(QFileInfo::exists(locked));
    QTest::keyClicks(window.editor(), QStringLiteral("hidden words"));
    window.findChild<QAction *>(QStringLiteral("editor:save-file"))->trigger();
    window.findChild<QAction *>(QStringLiteral("workspace:close"))->trigger();
    flushDeletes();
    window.findChild<QAction *>(QStringLiteral("encrypted-notes:lock"))->trigger();
    const int promptsBefore = asked;

    auto *undo = window.findChild<QAction *>(QStringLiteral("workspace:undo-close-pane"));
    undo->trigger();
    EXPECT_EQ(asked, promptsBefore) << "no password asked for a note undo cannot open";
    EXPECT_EQ(tabEditors(window).size(), 1);
    EXPECT_EQ(window.editor()->noteText(), QStringLiteral("# Welcome\n\nBody.\n"));

    // Unlocked again (a new encrypted note asks for the password): the closed one is still there.
    window.findChild<QAction *>(QStringLiteral("encrypted-notes:new"))->trigger();
    undo->trigger();
    EXPECT_TRUE(window.editor()->noteText().contains(QStringLiteral("hidden words")));
}
#endif

TEST_F(WindowTest, UndoCloseTabKeepsTwentyAndSkipsADeletedFile)
{
    for (int i = 1; i <= 21; ++i)
        write(QStringLiteral("N%1.md").arg(i), "n\n");
    MainWindow window;
    window.openVault(dir.path());
    window.openNote(path(QStringLiteral("Welcome.md")));
    for (int i = 1; i <= 21; ++i) {
        window.findChild<QAction *>(QStringLiteral("workspace:new-tab"))->trigger();
        window.openNote(path(QStringLiteral("N%1.md").arg(i)));
        window.findChild<QAction *>(QStringLiteral("workspace:close"))->trigger();
    }
    flushDeletes();
    ASSERT_EQ(tabEditors(window).size(), 1);
    // N21 was the newest; it is gone from disk, so the first undo opens N20.
    ASSERT_TRUE(QFile::remove(path(QStringLiteral("N21.md"))));
    auto *undo = window.findChild<QAction *>(QStringLiteral("workspace:undo-close-pane"));
    undo->trigger();
    EXPECT_TRUE(window.windowTitle().contains(QStringLiteral("N20.md")));
    for (int i = 0; i < 25; ++i)
        undo->trigger();
    // N21 skipped, N20 ... N2 reopened (20 were kept, N1 fell off the end).
    QStringList names;
    for (QTabBar *bar : window.findChildren<QTabBar *>(QStringLiteral("noteTabs")))
        names << tabTexts(bar);
    EXPECT_EQ(names.size(), 1 + 19);
    EXPECT_FALSE(names.contains(QStringLiteral("N1")));
    EXPECT_TRUE(names.contains(QStringLiteral("N2")));
    EXPECT_FALSE(names.contains(QStringLiteral("N21")));
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
TEST_F(WindowTest, TheSnapSwitchesReachAnOpenCanvasAsTheyAreFlipped)
{
    write(QStringLiteral("Board.canvas"), R"({"nodes":[],"edges":[]})");
    MainWindow window;
    window.show(); // the settings page builds its rows when shown
    window.openVault(dir.path());
    ASSERT_TRUE(window.openNote(path(QStringLiteral("Board.canvas"))));
    auto *canvas = window.findChild<noteAppC::CanvasView *>();
    ASSERT_NE(canvas, nullptr);
    EXPECT_TRUE(canvas->snapToObjects());
    EXPECT_FALSE(canvas->snapToGrid());

    command(window, "app:open-settings")->trigger();
    noteAppC::ToggleSwitch *objects = nullptr;
    noteAppC::ToggleSwitch *grid = nullptr;
    ASSERT_TRUE(QTest::qWaitFor([&] {
        objects = settingsWidget<noteAppC::ToggleSwitch>(window, QStringLiteral("settingsToggle_canvas.snapToObjects"));
        grid = settingsWidget<noteAppC::ToggleSwitch>(window, QStringLiteral("settingsToggle_canvas.snapToGrid"));
        return objects && grid;
    }));
    objects->click();
    grid->click();
    EXPECT_FALSE(canvas->snapToObjects());
    EXPECT_TRUE(canvas->snapToGrid());

    objects->click(); // put the shared test config back
    grid->click();
}

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

TEST_F(WindowTest, ABaseEditKeepsUnsavedTypingInTheSameNote)
{
    write(QStringLiteral("Task.md"), "---\nstatus: open\n---\nDo it.\n");
    write(QStringLiteral("Tasks.base"), "filters:\n  and:\n    - status == \"open\"\nviews:\n  - type: table\n"
                                        "    order: [file.name, status]\n");
    MainWindow window;
    window.setAutosaveIntervalsForTests(600000, 600000);
    QSignalSpy indexed(window.index(), &Index::changed);
    window.openVault(dir.path());
    ASSERT_TRUE(indexed.wait(5000));
    ASSERT_TRUE(window.openNote(path(QStringLiteral("Task.md"))));
    MarkdownEditor *editor = window.editor();
    editor->setCursorOffset(editor->noteText().size());
    QTest::keyClicks(editor, QStringLiteral("More."));
    ASSERT_TRUE(editor->isModified());

    window.findChild<QAction *>(QStringLiteral("workspace:new-tab"))->trigger();
    ASSERT_TRUE(window.openNote(path(QStringLiteral("Tasks.base"))));
    auto *table = window.findChild<noteAppC::bases::BaseView *>()->findChild<QTableView *>();
    WAIT_TRUE(table->model() && table->model()->rowCount() == 1, 5000);
    ASSERT_TRUE(table->model()->setData(table->model()->index(0, 1), QStringLiteral("done"), Qt::EditRole));

    const QByteArray both("---\nstatus: done\n---\nDo it.\nMore.");
    EXPECT_EQ(readAll(path(QStringLiteral("Task.md"))), both);
    // The note's own next save does not put the old value back.
    QTest::qWait(600);
    ASSERT_TRUE(window.openNote(path(QStringLiteral("Task.md"))));
    ASSERT_TRUE(QMetaObject::invokeMethod(&window, "save"));
    EXPECT_EQ(readAll(path(QStringLiteral("Task.md"))), both);
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

TEST_F(WindowTest, FloatingWindowsComeBackWithTheLayout)
{
    write(QStringLiteral("Other.md"), "other note text\n");
    {
        MainWindow window;
        window.openVault(dir.path());
        window.openNote(path(QStringLiteral("Welcome.md")));
        command(window, "workspace:new-tab")->trigger();
        window.openNote(path(QStringLiteral("Other.md")));
        command(window, "workspace:move-to-new-window")->trigger();
        command(window, "workspace:new-tab")->trigger(); // in the pop-out, where the focus is
        window.openNote(path(QStringLiteral("Projects/Crlf.md")));
        const auto popouts = popoutWindows();
        ASSERT_EQ(popouts.size(), 1);
        popouts.first()->resize(640, 480);
        // Other is the pop-out's shown tab and the active one; Crlf waits in its background.
        popouts.first()->findChild<QTabBar *>(QStringLiteral("noteTabs"))->setCurrentIndex(0);
        window.close();
    }
    EXPECT_TRUE(popoutWindows().isEmpty()) << "closing the window took its pop-out along";

    MainWindow window;
    window.openVault(dir.path());
    const auto popouts = popoutWindows();
    ASSERT_EQ(popouts.size(), 1);
    EXPECT_TRUE(popouts.first()->isVisible());
    EXPECT_EQ(popouts.first()->size(), QSize(640, 480));
    QTabBar *bar = popouts.first()->findChild<QTabBar *>(QStringLiteral("noteTabs"));
    ASSERT_NE(bar, nullptr);
    EXPECT_EQ(tabTexts(bar), (QStringList{QStringLiteral("Other"), QStringLiteral("Crlf")}));
    const auto editors = popouts.first()->findChildren<MarkdownEditor *>();
    ASSERT_EQ(editors.size(), 2);
    EXPECT_TRUE(editors.contains(window.editor())) << "the active tab was in the pop-out";
    EXPECT_EQ(window.editor()->noteText(), QStringLiteral("other note text\n"));
    EXPECT_EQ(tabEditors(window).size(), 1) << "Welcome stayed in the main window";
    // Crlf was deferred: nothing read until its tab is shown.
    int loaded = 0;
    for (MarkdownEditor *editor : editors)
        loaded += !editor->noteText().isEmpty();
    EXPECT_EQ(loaded, 1);
    bar->setCurrentIndex(1);
    EXPECT_EQ(window.editor()->noteText(), QStringLiteral("one\ntwo\n"));
}

TEST_F(WindowTest, TheActiveTabOfAPopoutIsTheActiveOneAgain)
{
    write(QStringLiteral("Other.md"), "other note text\n");
    {
        MainWindow window;
        window.openVault(dir.path());
        window.openNote(path(QStringLiteral("Welcome.md")));
        command(window, "workspace:new-tab")->trigger();
        window.openNote(path(QStringLiteral("Other.md")));
        command(window, "workspace:move-to-new-window")->trigger();
        command(window, "workspace:new-tab")->trigger();
        window.openNote(path(QStringLiteral("Projects/Crlf.md")));
        QTabBar *first = popoutWindows().first()->findChild<QTabBar *>(QStringLiteral("noteTabs"));
        command(window, "workspace:open-in-new-window")->trigger(); // a second window, now the active one
        ASSERT_EQ(popoutWindows().size(), 2);
        first->setCurrentIndex(0); // back to the first window's Other
        ASSERT_EQ(window.editor()->noteText(), QStringLiteral("other note text\n"));
        window.close();
    }
    MainWindow window;
    window.openVault(dir.path());
    ASSERT_EQ(popoutWindows().size(), 2);
    EXPECT_EQ(window.editor()->noteText(), QStringLiteral("other note text\n"));
}

TEST_F(WindowTest, ALayoutHasAFloatingNodeOnlyWhileThereAreWindows)
{
    {
        MainWindow window;
        window.openVault(dir.path());
        window.openNote(path(QStringLiteral("Welcome.md")));
        window.close();
    }
    EXPECT_FALSE(noteAppC::Workspaces::current(dir.path()).contains(QStringLiteral("floating")));
    {
        MainWindow window;
        window.openVault(dir.path());
        command(window, "workspace:open-in-new-window")->trigger();
        window.close();
    }
    EXPECT_TRUE(noteAppC::Workspaces::current(dir.path()).contains(QStringLiteral("floating")));
}

TEST_F(WindowTest, ObsidiansFloatingNodeIsReadAndNeverWritten)
{
    ASSERT_TRUE(QDir(dir.path()).mkpath(QStringLiteral(".obsidian")));
    const QByteArray obsidian = R"({
  "main": {"id": "5f", "type": "split", "direction": "vertical", "children": [
    {"id": "a1", "type": "tabs", "children": [
      {"id": "b1", "type": "leaf", "state": {"type": "markdown", "state": {"file": "Welcome.md"}}}]}]},
  "floating": {"id": "f0", "type": "floating", "children": [
    {"id": "w1", "type": "window", "direction": "vertical", "x": 10, "y": 20, "width": 700, "height": 500,
     "maximized": false, "zoom": 0, "children": [
      {"id": "t1", "type": "tabs", "children": [
        {"id": "c1", "type": "leaf", "state": {"type": "markdown", "state": {"file": "Projects/Crlf.md"}}},
        {"id": "c2", "type": "leaf", "state": {"type": "some-plugin-view", "state": {}}}]}]},
    {"id": "w2", "type": "window", "width": 300, "height": 300, "children": [
      {"id": "t2", "type": "tabs", "children": [
        {"id": "c3", "type": "leaf", "state": {"type": "some-plugin-view", "state": {}}}]}]},
    {"id": "w3", "type": "window", "width": 500, "height": 400, "children": [
      {"id": "s3", "type": "split", "direction": "vertical", "children": [
        {"id": "t3", "type": "tabs", "children": [
          {"id": "c4", "type": "leaf", "state": {"type": "markdown", "state": {"file": "Welcome.md"}}}]}]}]}]},
  "left": {"id": "l", "type": "split", "children": [], "collapsed": false},
  "right": {"id": "r", "type": "split", "children": [], "collapsed": false},
  "active": "b1",
  "lastOpenFiles": []
})";
    write(QStringLiteral(".obsidian/workspace.json"), obsidian);
    {
        MainWindow window;
        window.openVault(dir.path());
        const auto popouts = popoutWindows();
        ASSERT_EQ(popouts.size(), 2) << "the window with nothing we can show is not made";
        QList<QSize> sizes{popouts.at(0)->size(), popouts.at(1)->size()};
        std::sort(sizes.begin(), sizes.end(), [](const QSize &a, const QSize &b) { return a.width() < b.width(); });
        EXPECT_EQ(sizes, (QList<QSize>{QSize(500, 400), QSize(700, 500)}));
        for (const auto *popout : popouts) {
            EXPECT_EQ(popout->findChildren<MarkdownEditor *>().size(), 1) << "the unknown view is skipped";
            EXPECT_EQ(popout->findChildren<QTabBar *>(QStringLiteral("noteTabs")).size(), 1);
        }
        EXPECT_EQ(window.editor()->noteText(), QStringLiteral("# Welcome\n\nBody.\n"));
        window.close();
    }
    EXPECT_EQ(readAll(path(QStringLiteral(".obsidian/workspace.json"))), obsidian);
}

namespace {

/// The `state.type` of every leaf of every window in a saved layout's `floating` node.
QStringList floatingViewTypes(const QJsonObject &layout)
{
    QStringList types;
    for (const QJsonValue &window : layout.value(QStringLiteral("floating")).toObject().value(QStringLiteral("children")).toArray()) {
        for (const QJsonValue &tabs : window.toObject().value(QStringLiteral("children")).toArray()) {
            for (const QJsonValue &leaf : tabs.toObject().value(QStringLiteral("children")).toArray())
                types << leaf.toObject().value(QStringLiteral("state")).toObject().value(QStringLiteral("type")).toString();
        }
    }
    return types;
}

/// A widget by name wherever it is: in the window, or in the pop-out that took it.
QWidget *findAnywhere(MainWindow &window, const char *name)
{
    if (auto *found = window.findChild<QWidget *>(QLatin1String(name)))
        return found;
    for (auto *popout : popoutWindows()) {
        if (auto *found = popout->findChild<QWidget *>(QLatin1String(name)))
            return found;
    }
    return nullptr;
}

} // namespace

TEST_F(WindowTest, ADetachedSidebarIsRestoredAsAFileExplorerWindow)
{
    {
        MainWindow window;
        window.openVault(dir.path());
        command(window, "app:detach-left-sidebar")->trigger();
        command(window, "app:detach-right-sidebar")->trigger();
        const auto popouts = popoutWindows();
        ASSERT_EQ(popouts.size(), 2);
        for (auto *popout : popouts) {
            if (popout->findChild<QTreeView *>(QStringLiteral("fileExplorer")))
                popout->resize(333, 444);
        }
        window.close();
    }
    EXPECT_TRUE(popoutWindows().isEmpty());
    QJsonObject layout = noteAppC::Workspaces::current(dir.path());
    QStringList types = floatingViewTypes(layout);
    types.sort();
    EXPECT_EQ(types, (QStringList{QStringLiteral("file-explorer"), QStringLiteral("right-sidebar")}));
    // A view type Notolith does not have is skipped, whoever wrote it.
    QJsonArray windows = layout.value(QStringLiteral("floating")).toObject().value(QStringLiteral("children")).toArray();
    windows.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("window")}, {QStringLiteral("width"), 300}, {QStringLiteral("height"), 300},
                               {QStringLiteral("children"), QJsonArray{QJsonObject{
                                    {QStringLiteral("type"), QStringLiteral("tabs")},
                                    {QStringLiteral("children"), QJsonArray{QJsonObject{
                                         {QStringLiteral("type"), QStringLiteral("leaf")},
                                         {QStringLiteral("state"), QJsonObject{{QStringLiteral("type"), QStringLiteral("some-plugin-view")}}}}}}}}}});
    layout.insert(QStringLiteral("floating"), QJsonObject{{QStringLiteral("children"), windows}});
    ASSERT_TRUE(noteAppC::Workspaces::saveCurrent(dir.path(), layout));

    MainWindow window;
    window.openVault(dir.path());
    const auto popouts = popoutWindows();
    ASSERT_EQ(popouts.size(), 2) << "the unknown view made no window";
    for (auto *popout : popouts)
        EXPECT_EQ(popout->kind(), noteAppC::PopoutWindow::Kind::Sidebar);
    auto *sidebar = findAnywhere(window, "sidebar");
    auto *rightSidebar = findAnywhere(window, "rightSidebar");
    auto *splitter = window.findChild<QSplitter *>(QStringLiteral("mainSplitter"));
    ASSERT_TRUE(sidebar && rightSidebar && splitter);
    EXPECT_EQ(splitter->count(), 1) << "only the editor is left in the main window";
    auto *explorer = qobject_cast<noteAppC::PopoutWindow *>(sidebar->window());
    ASSERT_NE(explorer, nullptr);
    EXPECT_EQ(explorer->size(), QSize(333, 444));
    EXPECT_NE(explorer->findChild<QTreeView *>(QStringLiteral("fileExplorer")), nullptr);
    EXPECT_TRUE(explorer->isVisible());
    EXPECT_NE(qobject_cast<noteAppC::PopoutWindow *>(rightSidebar->window()), nullptr);
    EXPECT_NE(rightSidebar->window(), sidebar->window());

    // Closing the windows docks them again, and the next save forgets them.
    for (auto *popout : popouts)
        popout->close();
    EXPECT_TRUE(popoutWindows().isEmpty());
    EXPECT_EQ(splitter->count(), 3);
    window.close();
    EXPECT_FALSE(noteAppC::Workspaces::current(dir.path()).contains(QStringLiteral("floating")));
}

TEST_F(WindowTest, ADetachedPaneComesBackDetached)
{
    const auto tabOf = [](MainWindow &window, const char *name) {
        return window.findChild<QToolButton *>(QLatin1String(name));
    };
    {
        MainWindow window;
        window.openVault(dir.path());
        window.show();
        ASSERT_TRUE(QTest::qWaitForWindowExposed(&window));
        auto *tabRow = window.findChild<QWidget *>(QStringLiteral("rightSidebarTabs"));
        for (const char *name : {"backlinksTabButton", "outlineTabButton"}) {
            QToolButton *tab = tabOf(window, name);
            ASSERT_NE(tab, nullptr);
            chooseFromContextMenu(tabRow, tab->mapTo(tabRow, tab->rect().center()), QStringLiteral("Detach pane"));
        }
        ASSERT_EQ(popoutWindows().size(), 2);
        window.close();
    }
    EXPECT_TRUE(popoutWindows().isEmpty());
    QStringList types = floatingViewTypes(noteAppC::Workspaces::current(dir.path()));
    types.sort();
    EXPECT_EQ(types, (QStringList{QStringLiteral("backlink"), QStringLiteral("outline")}));

    MainWindow window;
    window.openVault(dir.path());
    const auto popouts = popoutWindows();
    ASSERT_EQ(popouts.size(), 2);
    auto *panes = window.findChild<QStackedWidget *>(QStringLiteral("rightSidebarPanes"));
    for (const char *name : {"backlinksPane", "outlinePane"}) {
        auto *pane = findAnywhere(window, name);
        ASSERT_NE(pane, nullptr) << name;
        auto *popout = qobject_cast<noteAppC::PopoutWindow *>(pane->window());
        ASSERT_NE(popout, nullptr) << name << " is back in the sidebar";
        EXPECT_EQ(popout->kind(), noteAppC::PopoutWindow::Kind::Pane);
        EXPECT_EQ(panes->indexOf(pane), -1);
    }
    EXPECT_TRUE(tabOf(window, "backlinksTabButton")->isHidden());
    EXPECT_TRUE(tabOf(window, "outlineTabButton")->isHidden());
    EXPECT_FALSE(tabOf(window, "tagsTabButton")->isHidden()) << "the panes that were not detached stay";
    EXPECT_NE(panes->indexOf(window.findChild<QWidget *>(QStringLiteral("tagsPane"))), -1);
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

    EXPECT_FALSE(window.settingsOpen());
    auto *settingsPage = window.findChild<QWidget *>(QStringLiteral("settingsPage"));
    ASSERT_NE(settingsPage, nullptr);
    EXPECT_NE(window.findChild<QWidget *>(QStringLiteral("settingsNav")), nullptr);

    // Click the button a person actually clicks, not the slot behind it: a
    // disconnected button passes every test that calls openSettings().
    auto *settingsButton =
        window.findChild<QToolButton *>(QStringLiteral("headerSettingsButton"));
    ASSERT_NE(settingsButton, nullptr);
    settingsButton->click();
    EXPECT_TRUE(window.settingsOpen());
    EXPECT_TRUE(settingsPage->isVisible());
    // A window of its own, narrower than an ultrawide main window.
    EXPECT_NE(settingsPage->window(), &window);
    EXPECT_LE(settingsPage->window()->width(), 960);

    window.closeSettings();
    ASSERT_FALSE(window.settingsOpen());
    window.openSettings();
    EXPECT_TRUE(window.settingsOpen());

    window.closeSettings();
    EXPECT_FALSE(window.settingsOpen());

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

// A linking note that is not valid UTF-8 cannot be rewritten without changing
// its bytes: the rename still goes through, the status bar names the note.
TEST_F(WindowTest, RenamingNamesALinkingNoteThatWasNotUpdated)
{
    write(QStringLiteral("Target.md"), "# Target\n");
    const QByteArray lossy("caf\xe9 [[Target]]\n");
    write(QStringLiteral("A-lossy.md"), lossy);
    write(QStringLiteral("B-clean.md"), "See [[Target]].\n");

    MainWindow window;
    QSignalSpy indexed(window.index(), &Index::changed);
    window.openVault(dir.path());
    ASSERT_TRUE(indexed.wait(5000));

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

    const QString message = window.statusBar()->currentMessage();
    EXPECT_TRUE(message.contains(QStringLiteral("links in a note were not updated"))) << message.toStdString();
    EXPECT_TRUE(message.contains(QStringLiteral("A-lossy.md"))) << message.toStdString();
    EXPECT_FALSE(message.contains(QStringLiteral("B-clean.md"))) << message.toStdString();
    EXPECT_TRUE(QFile::exists(path(QStringLiteral("Renamed.md"))));
    EXPECT_EQ(readAll(path(QStringLiteral("A-lossy.md"))), lossy);
    EXPECT_EQ(readAll(path(QStringLiteral("B-clean.md"))), QByteArray("See [[Renamed]].\n"));
}

// The index only learns of a note's new link when the save is indexed; the
// rename must not read the links before that.
TEST_F(WindowTest, RenamingRewritesALinkTypedAMomentAgo)
{
    write(QStringLiteral("Target.md"), "# Target\n");
    write(QStringLiteral("Home.md"), "Nothing yet.\n");

    MainWindow window;
    QSignalSpy indexed(window.index(), &Index::changed);
    window.openVault(dir.path());
    ASSERT_TRUE(indexed.wait(5000));
    window.openNote(path(QStringLiteral("Home.md")));
    auto *editor = window.editor();
    ASSERT_NE(editor, nullptr);
    editor->setCursorOffset(editor->noteText().size());
    QTest::keyClicks(editor, QStringLiteral("[[Target]]"));
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

    EXPECT_EQ(readAll(path(QStringLiteral("Home.md"))), QByteArray("Nothing yet.\n[[Renamed]]"));
}

namespace {
/// The inline title of the tab `editor` is in.
QLineEdit *titleOf(MarkdownEditor *editor)
{
    return editor->parentWidget()->findChild<QLineEdit *>(QStringLiteral("inlineTitle"));
}
} // namespace

// The timer, not the rename, saved the note: its reindex has not been applied
// when the rename runs. The rename reads it from disk all the same, and does
// not wait for the indexer - its own changed() is the only one it causes.
TEST_F(WindowTest, RenamingRightAfterATimerSaveRewritesTheLinkWithoutWaitingForTheIndexer)
{
    write(QStringLiteral("Target.md"), "# Target\n");
    write(QStringLiteral("Home.md"), "Nothing yet.\n");

    MainWindow window;
    window.setAutosaveIntervalsForTests(1, 1);
    QSignalSpy indexed(window.index(), &Index::changed);
    window.openVault(dir.path());
    ASSERT_TRUE(indexed.wait(5000));
    window.openNote(path(QStringLiteral("Home.md")));
    auto *editor = window.editor();
    ASSERT_NE(editor, nullptr);
    editor->setCursorOffset(editor->noteText().size());
    QTest::keyClicks(editor, QStringLiteral("[[Target]]"));
    ASSERT_TRUE(editor->isModified());
    // Another connection holds the index's write lock, so the scan the timer's
    // save starts cannot write the new link yet: only the disk has it. Released
    // a moment after the rename starts, which then waits for its own writes.
    std::atomic<bool> locked{false}, release{false};
    std::thread holder([&] {
        {
            QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("index8-holder"));
            db.setDatabaseName(window.index()->databasePath());
            if (db.open()) {
                QSqlQuery lock(db);
                locked = lock.exec(QStringLiteral("BEGIN IMMEDIATE"));
                while (!release)
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                std::this_thread::sleep_for(std::chrono::milliseconds(300));
                lock.exec(QStringLiteral("ROLLBACK"));
            }
        }
        QSqlDatabase::removeDatabase(QStringLiteral("index8-holder"));
    });
    const auto joinHolder = qScopeGuard([&] { // a failed ASSERT must not leave the thread joinable
        release = true;
        if (holder.joinable())
            holder.join();
    });
    ASSERT_TRUE(QTest::qWaitFor([&] { return locked.load(); }, 5000));
    // The timer saves; the scan it starts is not applied before the rename below.
    ASSERT_TRUE(QTest::qWaitFor([&] { return !editor->isModified(); }, 5000));
    auto *indexer = window.findChild<noteAppC::Indexer *>();
    ASSERT_NE(indexer, nullptr);

    // The inline title renames without a dialog's event loop applying the scan first.
    window.show();
    window.openNote(path(QStringLiteral("Target.md")));
    QLineEdit *title = titleOf(window.editor());
    ASSERT_NE(title, nullptr);
    ASSERT_TRUE(QTest::qWaitFor([&] { return indexer->isRunning(); }, 5000)) << "the reindex of the save must still be unapplied";
    title->setText(QStringLiteral("Renamed"));
    const qsizetype before = indexed.size();
    release = true;
    QTest::keyClick(title, Qt::Key_Return);
    holder.join();

    EXPECT_EQ(readAll(path(QStringLiteral("Home.md"))), QByteArray("Nothing yet.\n[[Renamed]]"));
    EXPECT_EQ(indexed.size() - before, 1) << "the rename's own announcement, no scan applied meanwhile";
}

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

TEST_F(WindowTest, DeletingTheOpenNoteMovesItToTheVaultTrashAndClearsTheEditor)
{
    QDir(dir.path()).mkpath(QStringLiteral(".obsidian"));
    write(QStringLiteral(".obsidian/app.json"), R"({"trashOption": "local"})"); // Obsidian's own spelling
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

TEST_F(WindowTest, MakeACopyPutsANumberedCopyBesideIt)
{
    MainWindow window;
    window.setAutosaveIntervalsForTests(600000, 600000);
    window.openVault(dir.path());
    window.openNote(path(QStringLiteral("Welcome.md")));
    window.editor()->setCursorOffset(window.editor()->noteText().size());
    QTest::keyClicks(window.editor(), QStringLiteral("typed"));
    QString copy;
    ASSERT_TRUE(QMetaObject::invokeMethod(&window, "copyPath", Q_RETURN_ARG(QString, copy),
                                          Q_ARG(QString, path(QStringLiteral("Welcome.md")))));
    EXPECT_EQ(copy, path(QStringLiteral("Welcome 1.md")));
    EXPECT_EQ(readAll(copy), QByteArray("# Welcome\n\nBody.\ntyped")) << "what was on screen";
    EXPECT_TRUE(window.windowTitle().startsWith(QStringLiteral("Welcome 1.md")));
    ASSERT_TRUE(QMetaObject::invokeMethod(&window, "copyPath", Q_RETURN_ARG(QString, copy),
                                          Q_ARG(QString, path(QStringLiteral("attachment.txt")))));
    EXPECT_EQ(copy, path(QStringLiteral("attachment 1.txt")));
}

TEST_F(WindowTest, TheDeleteCommandAndTheDeleteKeyRemoveAFile)
{
    QDir(dir.path()).mkpath(QStringLiteral(".obsidian"));
    write(QStringLiteral(".obsidian/app.json"), R"({"trashOption": "local", "promptDelete": false})");
    write(QStringLiteral("Other.md"), "other\n");
    MainWindow window;
    window.openVault(dir.path());
    window.openNote(path(QStringLiteral("Welcome.md")));

    window.findChild<QAction *>(QStringLiteral("app:delete-file"))->trigger();
    EXPECT_FALSE(QFile::exists(path(QStringLiteral("Welcome.md"))));
    EXPECT_TRUE(QFile::exists(dir.filePath(QStringLiteral(".trash/Welcome.md"))));
    EXPECT_FALSE(window.editor()->isEnabled()) << "the deleted note's tab closed";

    auto *tree = window.findChild<QTreeView *>(QStringLiteral("fileExplorer"));
    auto *model = qobject_cast<QFileSystemModel *>(tree->model());
    const QModelIndex other = model->index(path(QStringLiteral("Other.md")));
    ASSERT_TRUE(other.isValid());
    tree->setCurrentIndex(other);
    QTest::keyClick(tree, Qt::Key_Delete);
    EXPECT_FALSE(QFile::exists(path(QStringLiteral("Other.md"))));
    EXPECT_TRUE(QFile::exists(dir.filePath(QStringLiteral(".trash/Other.md"))));
}

namespace {

/// Deletes `relative` through the tree's own path, answering the confirmation Yes.
void deleteThroughTheTree(MainWindow &window, const QString &absolute)
{
    auto *tree = window.findChild<QTreeView *>(QStringLiteral("fileExplorer"));
    auto *model = qobject_cast<QFileSystemModel *>(tree->model());
    const QModelIndex index = model->index(absolute);
    ASSERT_TRUE(index.isValid());
    QTimer::singleShot(0, &window, [] {
        if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget()))
            box->button(QMessageBox::Yes)->click();
    });
    ASSERT_TRUE(QMetaObject::invokeMethod(&window, "deletePathAt", Q_ARG(QModelIndex, index)));
}

} // namespace

// Obsidian's "Deleted files": the system trash (default), the vault's .trash, or gone.
TEST_F(WindowTest, DeletingGoesToTheSystemTrashByDefaultNotTheVaultTrash)
{
    QTemporaryDir data; // a private XDG_DATA_HOME: the real trash is never touched
    ASSERT_TRUE(data.isValid());
    const QByteArray previous = qgetenv("XDG_DATA_HOME");
    qputenv("XDG_DATA_HOME", data.path().toUtf8());
    {
        MainWindow window;
        window.openVault(dir.path());
        deleteThroughTheTree(window, path(QStringLiteral("Welcome.md")));
    }
    qputenv("XDG_DATA_HOME", previous);
    EXPECT_FALSE(QFile::exists(path(QStringLiteral("Welcome.md"))));
    EXPECT_TRUE(QFile::exists(data.filePath(QStringLiteral("Trash/files/Welcome.md"))));
    EXPECT_FALSE(QFile::exists(dir.filePath(QStringLiteral(".trash"))));
}

TEST_F(WindowTest, DeletingWithTrashOptionNoneIsPermanentForNotesAndFolders)
{
    QDir(dir.path()).mkpath(QStringLiteral(".obsidian"));
    write(QStringLiteral(".obsidian/app.json"), R"({"trashOption": "none"})");
    write(QStringLiteral("Projects/Deep.md"), "deep\n");
    QTemporaryDir data;
    ASSERT_TRUE(data.isValid());
    const QByteArray previous = qgetenv("XDG_DATA_HOME");
    qputenv("XDG_DATA_HOME", data.path().toUtf8());
    {
        MainWindow window;
        window.openVault(dir.path());
        deleteThroughTheTree(window, path(QStringLiteral("Welcome.md")));
        deleteThroughTheTree(window, path(QStringLiteral("Projects")));
    }
    qputenv("XDG_DATA_HOME", previous);
    EXPECT_FALSE(QFile::exists(path(QStringLiteral("Welcome.md"))));
    EXPECT_FALSE(QFile::exists(path(QStringLiteral("Projects"))));
    EXPECT_FALSE(QFile::exists(dir.filePath(QStringLiteral(".trash"))));
    EXPECT_FALSE(QFile::exists(data.filePath(QStringLiteral("Trash"))));
}

// No system trash must never become a permanent delete.
TEST_F(WindowTest, ASystemTrashThatFailsFallsBackToTheVaultTrashAndSaysSo)
{
    const QByteArray previous = qgetenv("XDG_DATA_HOME");
    qputenv("XDG_DATA_HOME", "/proc/no-such-trash"); // nothing can be created there
    {
        MainWindow window;
        window.openVault(dir.path());
        deleteThroughTheTree(window, path(QStringLiteral("Welcome.md")));
        EXPECT_TRUE(window.statusBar()->currentMessage().contains(QStringLiteral(".trash")));
    }
    qputenv("XDG_DATA_HOME", previous);
    EXPECT_FALSE(QFile::exists(path(QStringLiteral("Welcome.md"))));
    EXPECT_TRUE(QFile::exists(dir.filePath(QStringLiteral(".trash/Welcome.md"))));
}

TEST_F(WindowTest, SettingsStaysOpenBesideANoteOpenedFromTheTree)
{
    write(QStringLiteral("Second.md"), "# Second\n");

    MainWindow window;
    QSignalSpy loaded(window.findChild<QFileSystemModel *>(), &QFileSystemModel::directoryLoaded);
    window.openVault(dir.path());
    ASSERT_TRUE(loaded.wait(5000));
    window.openNote(path(QStringLiteral("Welcome.md")));
    auto *tree = window.findChild<QTreeView *>(QStringLiteral("fileExplorer"));
    auto *model = qobject_cast<QFileSystemModel *>(tree->model());
    const QModelIndex second = model->index(path(QStringLiteral("Second.md")));
    ASSERT_TRUE(second.isValid());

    window.openSettings();
    ASSERT_TRUE(QMetaObject::invokeMethod(&window, "onTreeActivated", Q_ARG(QModelIndex, second)));
    EXPECT_EQ(window.editor()->noteText(), QStringLiteral("# Second\n"));
    EXPECT_TRUE(window.settingsOpen()) << "a window of its own: the note opens beside it";
    window.closeSettings();
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

    ASSERT_FALSE(window.settingsOpen());
    QTest::keyClick(&window, Qt::Key_Comma, Qt::ControlModifier);
    EXPECT_TRUE(window.settingsOpen());

    auto *settingsPage = window.findChild<QWidget *>(QStringLiteral("settingsPage"));
    ASSERT_NE(settingsPage, nullptr);
    QTest::keyClick(settingsPage, Qt::Key_Escape);
    EXPECT_FALSE(window.settingsOpen());
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

    auto *combo = settingsWidget<QComboBox>(window, QStringLiteral("settingsSchemeCombo"));
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

    auto *combo = settingsWidget<QComboBox>(window, QStringLiteral("settingsSchemeCombo"));
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
        refreshed = settingsWidget<QComboBox>(window, QStringLiteral("settingsSchemeCombo"));
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

TEST_F(WindowTest, ANestedHeadingLinkOpensTheHeadingUnderItsParent)
{
    write(QStringLiteral("Other.md"), "# Other\n\n## First\n\n### Detail\n\n## Second\n\n### Detail\n\ntail\n");
    write(QStringLiteral("Linking.md"), "See [[Other#Second#Detail]].\n");
    MainWindow window;
    QSignalSpy indexed(window.index(), &Index::changed);
    window.openVault(dir.path());
    ASSERT_TRUE(indexed.wait(5000));
    window.openNote(path(QStringLiteral("Linking.md")));

    emit window.editor()->linkHovered(QStringLiteral("Other#Second#Detail"), QPoint(200, 200));
    noteAppC::PagePreview *preview = nullptr;
    WAIT_TRUE(
        (preview = window.findChild<noteAppC::PagePreview *>(QStringLiteral("pagePreview"))) && preview->isVisible(),
        2000);
    const QString shown = preview->editor()->noteText();
    EXPECT_EQ(preview->editor()->cursorOffset(), shown.lastIndexOf(QLatin1String("### Detail")));
    EXPECT_NE(preview->editor()->cursorOffset(), shown.indexOf(QLatin1String("### Detail")));

    // Opening the link jumps to the same heading.
    emit window.editor()->linkActivated(QStringLiteral("Other#Second#Detail"));
    WAIT_TRUE(window.editor()->noteText().startsWith(QLatin1String("# Other")), 2000);
    const QString opened = window.editor()->noteText();
    WAIT_EQ(window.editor()->cursorOffset(), opened.lastIndexOf(QLatin1String("### Detail")), 2000);
}

TEST_F(WindowTest, ClickingALinkToNoNoteCreatesIt)
{
    write(QStringLiteral("Linking.md"), "[[Nope]] [[Projects/Deep]] [[photo.png]]\n");
    MainWindow window;
    QSignalSpy indexed(window.index(), &Index::changed);
    window.openVault(dir.path());
    ASSERT_TRUE(indexed.wait(5000));
    window.openNote(path(QStringLiteral("Linking.md")));

    emit window.editor()->linkActivated(QStringLiteral("Nope"));
    EXPECT_TRUE(QFileInfo(path(QStringLiteral("Nope.md"))).isFile());
    EXPECT_TRUE(window.windowTitle().startsWith(QStringLiteral("Nope.md")));

    // A folder in the link is made; the note opens there.
    window.openNote(path(QStringLiteral("Linking.md")));
    emit window.editor()->linkActivated(QStringLiteral("Projects/Deep#Heading"));
    EXPECT_TRUE(QFileInfo(path(QStringLiteral("Projects/Deep.md"))).isFile());

    // A missing attachment is not made into a note.
    window.openNote(path(QStringLiteral("Linking.md")));
    emit window.editor()->linkActivated(QStringLiteral("photo.png"));
    EXPECT_FALSE(QFileInfo::exists(path(QStringLiteral("photo.png.md"))));
    EXPECT_TRUE(window.windowTitle().startsWith(QStringLiteral("Linking.md")));

    // A dotted name that is no attachment is a note's.
    emit window.editor()->linkActivated(QStringLiteral("Node.js"));
    EXPECT_TRUE(QFileInfo(path(QStringLiteral("Node.js.md"))).isFile());

    // Nothing lands outside the vault, whatever the link says.
    QTemporaryDir outside;
    window.openNote(path(QStringLiteral("Linking.md")));
    emit window.editor()->linkActivated(outside.filePath(QStringLiteral("Escape")));
    EXPECT_TRUE(QDir(outside.path()).isEmpty());

    // On disk but not indexed yet: opened, not copied as "Late 1".
    write(QStringLiteral("Late.md"), "late\n");
    emit window.editor()->linkActivated(QStringLiteral("Late"));
    EXPECT_FALSE(QFileInfo::exists(path(QStringLiteral("Late 1.md"))));
    EXPECT_TRUE(window.windowTitle().startsWith(QStringLiteral("Late.md")));
}

TEST_F(WindowTest, ALinkClickedWhileTheVaultIsReadCreatesNothing)
{
    write(QStringLiteral("Projects/Elsewhere.md"), "here\n");
    MainWindow window;
    window.openVault(dir.path()); // the scan has not reported yet: no event loop ran
    window.openNote(path(QStringLiteral("Welcome.md")));
    emit window.editor()->linkActivated(QStringLiteral("Elsewhere"));
    EXPECT_FALSE(QFileInfo::exists(path(QStringLiteral("Elsewhere.md"))));
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

// What heading completion inserts for a heading holding link syntax opens that
// heading, not nothing.
TEST_F(WindowTest, ACompletedLinkToAHeadingWithLinkSyntaxInItOpensTheHeading)
{
    write(QStringLiteral("Other.md"), "# Other\n\nIntro.\n\n## Q&A | notes [x] #1\n\nBody.\n");
    write(QStringLiteral("Linking.md"), "See it: ");
    MainWindow window;
    QSignalSpy indexed(window.index(), &Index::changed);
    window.openVault(dir.path());
    ASSERT_TRUE(indexed.wait(5000));
    window.openNote(path(QStringLiteral("Linking.md")));
    auto *editor = window.editor();
    editor->setCursorOffset(editor->noteText().size());
    QTest::keyClicks(editor, QStringLiteral("[[Other#Q&A"));
    QWidget *popup = nullptr;
    for (QWidget *widget : QApplication::allWidgets()) {
        if (widget->objectName() == QLatin1String("completerPopup"))
            popup = widget;
    }
    ASSERT_NE(popup, nullptr);
    ASSERT_TRUE(popup->isVisible());
    QTest::keyClick(popup, Qt::Key_Return);
    const QString text = editor->noteText();
    const qsizetype open = text.indexOf(QLatin1String("[[Other#"));
    ASSERT_GE(open, 0);
    const QString inserted = text.mid(open + 2, text.indexOf(QLatin1String("]]"), open) - open - 2);
    EXPECT_EQ(inserted, QStringLiteral("Other#Q&A notes x 1"));

    emit editor->linkActivated(inserted);
    const QString shown = window.editor()->noteText();
    ASSERT_TRUE(shown.startsWith(QLatin1String("# Other")));
    WAIT_EQ(window.editor()->cursorOffset(), shown.indexOf(QLatin1String("## Q&A")), 2000);
}

TEST_F(WindowTest, AHeadingLinkWrittenWithOtherPunctuationOpensTheHeading)
{
    write(QStringLiteral("Other.md"), "# Other\n\nIntro.\n\n## Part 1: Intro?\n\nBody.\n");
    write(QStringLiteral("Linking.md"), "See it.\n");
    MainWindow window;
    QSignalSpy indexed(window.index(), &Index::changed);
    window.openVault(dir.path());
    ASSERT_TRUE(indexed.wait(5000));
    window.openNote(path(QStringLiteral("Linking.md")));
    emit window.editor()->linkActivated(QStringLiteral("Other#Part 1: Intro!"));
    const QString shown = window.editor()->noteText();
    ASSERT_TRUE(shown.startsWith(QLatin1String("# Other")));
    WAIT_EQ(window.editor()->cursorOffset(), shown.indexOf(QLatin1String("## Part 1")), 2000);
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

TEST_F(WindowTest, NoteComposerMergeKeepsAnUnsavedLinkInAnotherTab)
{
    write(QStringLiteral("A.md"), "Alpha\n");
    write(QStringLiteral("B.md"), "Bravo\n");
    write(QStringLiteral("C.md"), "See [[A]]\n");
    MainWindow window;
    QSignalSpy indexed(window.index(), &Index::changed);
    window.openVault(dir.path());
    ASSERT_TRUE(indexed.wait(5000));
    window.openNote(path(QStringLiteral("C.md")));
    MarkdownEditor *editor = window.editor();
    editor->setCursorOffset(editor->noteText().size());
    QTest::keyClicks(editor, QStringLiteral("[[A]]"));
    ASSERT_TRUE(editor->isModified());
    window.findChild<QAction *>(QStringLiteral("workspace:new-tab"))->trigger();
    window.openNote(path(QStringLiteral("A.md")));

    window.findChild<QAction *>(QStringLiteral("note-composer:merge-file"))->trigger();
    auto *picker = window.findChild<noteAppC::QuickSwitcher *>(QStringLiteral("mergePicker"));
    ASSERT_NE(picker, nullptr);
    emit picker->accepted(QStringLiteral("B.md"));

    // The typed link was saved first, so the rewrite saw it too.
    EXPECT_EQ(readAll(path(QStringLiteral("C.md"))), QByteArray("See [[B]]\n[[B]]"));
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
            return (found = settingsWidget<noteAppC::ToggleSwitch>(window, QStringLiteral("settingsToggle_") + QLatin1String(key)));
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
    EXPECT_FALSE(window.findChild<QToolButton *>(QStringLiteral("headerGraphButton"))->isVisible());
    toggle("plugins.graph")->click();
    EXPECT_TRUE(window.findChild<QToolButton *>(QStringLiteral("headerGraphButton"))->isVisibleTo(&window));
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
        return (toggle = settingsWidget<noteAppC::ToggleSwitch>(window, QStringLiteral("settingsToggle_editor.vimMode")));
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
        return (wordCount = settingsWidget<noteAppC::ToggleSwitch>(window, QStringLiteral("settingsToggle_plugins.wordCount")));
    }));
    wordCount->click();
    wordCount->click();
    EXPECT_EQ(editor->mode(), MarkdownEditor::Mode::Reading);
    qputenv("XDG_CONFIG_HOME", previous);
}

namespace {

QAction *actionStartingWith(const QMenu &menu, const QString &name)
{
    for (QAction *action : menu.actions()) {
        if (action->text().startsWith(name))
            return action;
    }
    return nullptr;
}

} // namespace

TEST_F(WindowTest, LookUpShowsAPopoverAndEscapeClosesIt)
{
    write(QStringLiteral("Words.md"), "good words here\n");
    MainWindow window;
    window.openVault(dir.path());
    window.openNote(path(QStringLiteral("Words.md")));
    QStringList asked;
    // Never the network: the window's own client is not reached.
    window.setLookupFunction([&asked](const QString &word, std::function<void(const QString &)> done) {
        asked << word;
        done(QStringLiteral("## %1\n\n**fake definition**").arg(word));
    });

    const auto menu = window.editor()->contextMenuAt(window.editor()->noteText().indexOf(QLatin1String("words")) + 1);
    QAction *look = actionStartingWith(*menu, QStringLiteral("Look up"));
    ASSERT_TRUE(look) << "on by default";
    EXPECT_TRUE(actionStartingWith(*menu, QStringLiteral("Search for"))) << "Search is on by default too";
    EXPECT_TRUE(actionStartingWith(*menu, QStringLiteral("Search the web for")));
    EXPECT_EQ(window.findChild<noteAppC::PagePreview *>(QStringLiteral("lookupPopover")), nullptr)
        << "built when first used";
    look->trigger();
    EXPECT_EQ(asked, QStringList{QStringLiteral("words")});

    auto *popover = window.findChild<noteAppC::PagePreview *>(QStringLiteral("lookupPopover"));
    ASSERT_TRUE(popover);
    EXPECT_TRUE(popover->isVisible());
    EXPECT_TRUE(popover->editor()->noteText().contains(QLatin1String("fake definition")));
    QTest::keyClick(window.editor(), Qt::Key_Escape);
    EXPECT_FALSE(popover->isVisible());
}

TEST_F(WindowTest, ALateDefinitionDoesNotReopenADismissedPopover)
{
    write(QStringLiteral("Words.md"), "good words here\n");
    MainWindow window;
    window.openVault(dir.path());
    window.openNote(path(QStringLiteral("Words.md")));
    std::function<void(const QString &)> later;
    window.setLookupFunction([&later](const QString &, std::function<void(const QString &)> done) { later = done; });
    const auto menu = window.editor()->contextMenuAt(1);
    actionStartingWith(*menu, QStringLiteral("Look up"))->trigger();
    auto *popover = window.findChild<noteAppC::PagePreview *>(QStringLiteral("lookupPopover"));
    ASSERT_TRUE(popover);
    EXPECT_TRUE(popover->editor()->noteText().contains(QLatin1String("Looking up")));
    QTest::keyClick(window.editor(), Qt::Key_Escape);
    ASSERT_TRUE(later);
    later(QStringLiteral("## good\n\narrived"));
    EXPECT_FALSE(popover->isVisible());
}

TEST_F(WindowTest, LookUpIsAbsentWhenTheFeatureIsOff)
{
    write(QStringLiteral("Words.md"), "good words here\n");
    MainWindow window;
    window.openVault(dir.path());
    window.openNote(path(QStringLiteral("Words.md")));
    window.show();
    window.findChild<QAction *>(QStringLiteral("app:open-settings"))->trigger();
    noteAppC::ToggleSwitch *lookup = nullptr;
    ASSERT_TRUE(QTest::qWaitFor([&] {
        return (lookup = settingsWidget<noteAppC::ToggleSwitch>(window, QStringLiteral("settingsToggle_plugins.wordLookup")));
    }));
    lookup->click();
    const auto menu = window.editor()->contextMenuAt(1);
    EXPECT_FALSE(actionStartingWith(*menu, QStringLiteral("Look up")));
    EXPECT_TRUE(actionStartingWith(*menu, QStringLiteral("Search for"))) << "only the one switched off goes";
    lookup->click();
    EXPECT_TRUE(actionStartingWith(*window.editor()->contextMenuAt(1), QStringLiteral("Look up")));
}

#ifdef NOTEAPPC_HAVE_SPELLCHECK
TEST_F(WindowTest, AWordAddedInOneTabStopsBeingUnderlinedInTheOther)
{
    // A dictionary named for the language the system asks for, so the window
    // loads it as it would the real one.
    QString language = QLocale::system().uiLanguages().value(0);
    language.replace(QLatin1Char('-'), QLatin1Char('_'));
    if (language.isEmpty() || language == QLatin1String("C"))
        GTEST_SKIP() << "no system language to name a test dictionary after";
    QTemporaryDir dictionaries;
    ASSERT_TRUE(dictionaries.isValid());
    for (const auto &[extension, bytes] : {std::pair{"aff", "SET UTF-8\n"}, std::pair{"dic", "2\ngood\nhere\n"}}) {
        QFile file(QDir(dictionaries.path()).filePath(language + QLatin1Char('.') + QLatin1String(extension)));
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        file.write(bytes);
    }
    const QByteArray oldPath = qgetenv("DICPATH");
    qputenv("DICPATH", dictionaries.path().toUtf8());

    write(QStringLiteral("Words.md"), "good zzqxv here\n");
    MainWindow window;
    window.show();
    // The way main() opens: spellcheck loads only once the window has painted.
    window.openTarget(noteAppC::resolveLaunchTarget(dir.path()));
    window.openNote(path(QStringLiteral("Words.md")));
    MarkdownEditor *first = window.editor();
    window.findChild<QAction *>(QStringLiteral("workspace:split-vertical"))->trigger();
    MarkdownEditor *second = window.editor();
    ASSERT_NE(first, second);
    const qsizetype at = first->noteText().indexOf(QLatin1String("zzqxv")) + 1;
    const auto underlined = [at](const MarkdownEditor *editor) {
        return editor->formatAt(at).underlineStyle() == QTextCharFormat::SpellCheckUnderline;
    };
    // The dictionary loads off the UI thread after the first paint.
    const bool loaded = QTest::qWaitFor([&] { return underlined(first) && underlined(second); }, 5000);
    if (loaded) {
        actionStartingWith(*first->contextMenuAt(at), QStringLiteral("Add \"zzqxv\""))->trigger();
        EXPECT_FALSE(underlined(first));
        EXPECT_FALSE(underlined(second)) << "the other tab needs no click to catch up";
        const auto menu = first->contextMenuAt(at);
        QAction *remove = actionStartingWith(*menu, QStringLiteral("Remove \"zzqxv\""));
        ASSERT_TRUE(remove);
        remove->trigger();
        EXPECT_TRUE(underlined(second));
    }
    qputenv("DICPATH", oldPath);
    EXPECT_TRUE(loaded) << "the test dictionary never loaded";
}
#endif

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

// Invalid UTF-8 reads as U+FFFD; an autosave would write EF BF BD over the
// original byte after one keystroke.
TEST_F(WindowTest, ANoteWithInvalidUtf8IsNeverAutosavedAndSavesOnlyOnConsent)
{
    write(QStringLiteral("Latin.md"), "a\xE8" "b\n");
    const QString note = path(QStringLiteral("Latin.md"));
    MainWindow window;
    window.openVault(dir.path());
    window.openNote(note);
    EXPECT_TRUE(window.statusBar()->currentMessage().contains(QStringLiteral("not valid UTF-8")));
    MarkdownEditor *editor = window.editor();
    editor->setCursorOffset(editor->noteText().size());
    QTest::keyClicks(editor, QStringLiteral("x"));
    ASSERT_TRUE(editor->isModified());
    QTest::qWait(3500); // well past the 2 s autosave
    EXPECT_TRUE(editor->isModified());
    EXPECT_EQ(readAll(note), QByteArray("a\xE8" "b\n"));
    EXPECT_TRUE(window.statusBar()->currentMessage().contains(QStringLiteral("not saved automatically")));

    // By hand it asks; No leaves the bytes alone, Yes replaces them.
    QMessageBox::StandardButton answer = QMessageBox::No;
    QTimer watch;
    QObject::connect(&watch, &QTimer::timeout, [&answer] {
        if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget()))
            box->button(answer)->click();
    });
    watch.start(20);
    ASSERT_TRUE(QMetaObject::invokeMethod(&window, "save"));
    EXPECT_EQ(readAll(note), QByteArray("a\xE8" "b\n"));
    EXPECT_TRUE(editor->isModified());
    answer = QMessageBox::Yes;
    ASSERT_TRUE(QMetaObject::invokeMethod(&window, "save"));
    EXPECT_EQ(readAll(note), QByteArray("a\xEF\xBF\xBD" "b\nx"));
    EXPECT_FALSE(editor->isModified());
    // Consent given: from here it is an ordinary note.
    QTest::keyClicks(editor, QStringLiteral("y"));
    WAIT_TRUE(!editor->isModified(), 5000);
    EXPECT_EQ(readAll(note), QByteArray("a\xEF\xBF\xBD" "b\nxy"));
}

TEST_F(WindowTest, LeavingAnEditedNoteWithInvalidUtf8OffersItAndDiscardKeepsTheBytes)
{
    write(QStringLiteral("Latin.md"), "a\xE8" "b\n");
    MainWindow window;
    window.openVault(dir.path());
    window.openNote(path(QStringLiteral("Latin.md")));
    QTest::keyClicks(window.editor(), QStringLiteral("x"));
    bool asked = false;
    QTimer watch;
    QObject::connect(&watch, &QTimer::timeout, [&asked] {
        if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
            asked = box->text().contains(QStringLiteral("not valid UTF-8"));
            box->button(QMessageBox::Discard)->click();
        }
    });
    watch.start(20);
    window.openNote(path(QStringLiteral("Projects/Crlf.md"))); // autosave is on: still asks
    EXPECT_TRUE(asked);
    EXPECT_EQ(readAll(path(QStringLiteral("Latin.md"))), QByteArray("a\xE8" "b\n"));
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

        // Its own last save is no outside change: the next save keeps no other version.
        const auto before = noteAppC::Snapshots::versions(path(QStringLiteral("Analysis.ipynb"))).size();
        QTest::keyClicks(editor, QStringLiteral("x"));
        ASSERT_TRUE(QMetaObject::invokeMethod(&window, "save"));
        EXPECT_FALSE(window.statusBar()->currentMessage().contains(QStringLiteral("had changed on disk")));
        EXPECT_LE(noteAppC::Snapshots::versions(path(QStringLiteral("Analysis.ipynb"))).size(), before + 1);
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

TEST_F(WindowTest, ASaveBeforeTheWatcherSpeaksKeepsTheOtherVersion)
{
    MainWindow window;
    window.openVault(dir.path());
    const QString note = path(QStringLiteral("Welcome.md"));
    window.openNote(note);
    MarkdownEditor *editor = window.editor();
    editor->setCursorOffset(editor->noteText().size());
    QTest::keyClicks(editor, QStringLiteral("mine"));
    // No event loop between the outside write and the save: the watcher has not run.
    overwrite(note, "# Welcome\n\nTheirs.\n");
    ASSERT_TRUE(QMetaObject::invokeMethod(&window, "save")); // the same saveLeaf() an autosave runs
    EXPECT_EQ(readAll(note), QByteArray("# Welcome\n\nBody.\nmine"));
    bool kept = false;
    for (const auto &version : noteAppC::Snapshots::versions(note))
        kept = kept || noteAppC::Snapshots::read(version) == QStringLiteral("# Welcome\n\nTheirs.\n");
    EXPECT_TRUE(kept);
}

TEST_F(WindowTest, APinnedTabKeepsItsNote)
{
    write(QStringLiteral("Linking.md"), "[[Welcome]]\n");
    {
        MainWindow window;
        QSignalSpy indexed(window.index(), &Index::changed);
        window.openVault(dir.path());
        ASSERT_TRUE(indexed.wait(5000));
        window.openNote(path(QStringLiteral("Linking.md")));
        window.findChild<QAction *>(QStringLiteral("workspace:toggle-pin"))->trigger();
        auto *tabs = window.findChild<QTabBar *>(QStringLiteral("noteTabs"));

        // A link from it opens beside it; the pinned tab still holds Linking.
        emit window.editor()->linkActivated(QStringLiteral("Welcome"));
        ASSERT_EQ(tabs->count(), 2);
        EXPECT_TRUE(window.windowTitle().startsWith(QStringLiteral("Welcome.md")));
        EXPECT_EQ(tabs->tabText(0), QStringLiteral("Linking"));

        // Ctrl+W, a middle click and "Close all other tabs" leave it.
        tabs->setCurrentIndex(0);
        window.findChild<QAction *>(QStringLiteral("workspace:close"))->trigger();
        EXPECT_EQ(tabs->count(), 2);
        QTest::mouseClick(tabs, Qt::MiddleButton, Qt::NoModifier, tabs->tabRect(0).center());
        EXPECT_EQ(tabs->count(), 2);
        tabs->setCurrentIndex(1);
        window.findChild<QAction *>(QStringLiteral("workspace:close-others"))->trigger();
        EXPECT_EQ(tabs->count(), 2);
        tabs->setCurrentIndex(0);
#ifdef NOTEAPPC_HAVE_ENCRYPTION
        // An open that fails leaves no blank tab beside it.
        write(QStringLiteral("Secret.mdlock"), "not really encrypted");
        window.setEncryptionPromptForTests([](bool, bool) { return QString(); }); // cancelled
        QTimer dismiss; // whatever the failure says, in a box
        QObject::connect(&dismiss, &QTimer::timeout, [] {
            if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget()))
                box->reject();
        });
        dismiss.start(20);
        EXPECT_FALSE(window.openNote(path(QStringLiteral("Secret.mdlock"))));
        dismiss.stop();
        EXPECT_EQ(tabs->count(), 2);
        EXPECT_TRUE(window.windowTitle().startsWith(QStringLiteral("Linking.md")));
#endif
        window.close(); // the layout keeps the pin
    }
    MainWindow window;
    window.openVault(dir.path());
    auto *tabs = window.findChild<QTabBar *>(QStringLiteral("noteTabs"));
    ASSERT_EQ(tabs->count(), 2);
    auto *pin = qobject_cast<QToolButton *>(tabs->tabButton(0, QTabBar::RightSide));
    ASSERT_NE(pin, nullptr);
    EXPECT_EQ(pin->property("iconName").toString(), QStringLiteral("pin"));
    pin->click();
    EXPECT_EQ(pin->property("iconName").toString(), QStringLiteral("x"));
    EXPECT_EQ(tabs->count(), 2) << "unpinned, not closed";
}

TEST_F(WindowTest, TheTabsOutliveACrash)
{
    {
        MainWindow window;
        window.openVault(dir.path());
        window.openNote(path(QStringLiteral("Welcome.md")));
        window.findChild<QAction *>(QStringLiteral("workspace:new-tab"))->trigger();
        window.openNote(path(QStringLiteral("Projects/Crlf.md")));
        QTest::qWait(1500);
        // Gone without close(): nothing saved on the way out.
    }
    MainWindow window;
    window.openVault(dir.path());
    const auto tabs = window.findChildren<QTabBar *>(QStringLiteral("noteTabs"));
    ASSERT_EQ(tabs.size(), 1);
    EXPECT_EQ(tabs.first()->count(), 2);
}

TEST_F(WindowTest, MixedLineEndingsSurviveOneSaveAfterAnother)
{
    write(QStringLiteral("Mixed.md"), "a\r\nb\nc\r\nd\r\n");
    MainWindow window;
    window.openVault(dir.path());
    window.openNote(path(QStringLiteral("Mixed.md")));
    MarkdownEditor *editor = window.editor();
    editor->setCursorOffset(0);
    QTest::keyClicks(editor, QStringLiteral("X"));
    ASSERT_TRUE(QMetaObject::invokeMethod(&window, "save"));
    EXPECT_EQ(readAll(path(QStringLiteral("Mixed.md"))), QByteArray("Xa\r\nb\nc\r\nd\r\n"));
    editor->setCursorOffset(editor->noteText().size());
    QTest::keyClicks(editor, QStringLiteral("Y"));
    ASSERT_TRUE(QMetaObject::invokeMethod(&window, "save"));
    EXPECT_EQ(readAll(path(QStringLiteral("Mixed.md"))), QByteArray("Xa\r\nb\nc\r\nd\r\nY"));
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

namespace {

/// Three pixels of a file explorer row, read from a grab of the viewport:
/// the two columns left of the row's indent guide (the gutter), the same
/// columns on the empty viewport below the last row, and a pixel inside the row.
struct RowPixels {
    QRgb gutterNear, gutterFar, belowNear, belowFar, inside;
};

RowPixels sampleRow(QTreeView *tree, const QModelIndex &index)
{
    const QImage image = tree->viewport()->grab().toImage();
    const QRect row = tree->visualRect(index);
    const int y = row.center().y();
    const int near = 2;
    const int far = tree->indentation() - 2;
    const int below = image.height() - 3;
    return {image.pixel(near, y), image.pixel(far, y), image.pixel(near, below),
            image.pixel(far, below), image.pixel(row.left() + 8, y)};
}

} // namespace

TEST_F(WindowTest, TheSelectedTreeRowDoesNotLightTheIndentGutter)
{
    MainWindow window;
    window.resize(1000, 700);
    window.openVault(dir.path());
    window.show();
    ASSERT_TRUE(QTest::qWaitForWindowExposed(&window));
    window.openNote(path(QStringLiteral("Projects/Crlf.md")));

    auto *tree = window.findChild<QTreeView *>(QStringLiteral("fileExplorer"));
    ASSERT_NE(tree, nullptr);
    ASSERT_TRUE(QTest::qWaitFor(
        [&] { return tree->currentIndex().isValid() && tree->visualRect(tree->currentIndex()).height() > 0; },
        5000));
    const QModelIndex current = tree->currentIndex();
    ASSERT_TRUE(tree->selectionModel()->isSelected(current));
    // Expanding Projects animates; a grab taken meanwhile shows the old rows.
    QTest::qWait(500);
    ASSERT_GT(tree->visualRect(current).left(), tree->indentation()) << "a depth-1 row";

    const RowPixels pixels = sampleRow(tree, current);
    EXPECT_EQ(pixels.gutterNear, pixels.belowNear);
    EXPECT_EQ(pixels.gutterFar, pixels.belowFar);
    EXPECT_NE(pixels.inside, pixels.belowNear) << "the selected row is drawn at all";
}

TEST_F(WindowTest, HoveringATreeRowDoesNotLightTheIndentGutter)
{
    MainWindow window;
    window.resize(1000, 700);
    window.openVault(dir.path());
    window.show();
    ASSERT_TRUE(QTest::qWaitForWindowExposed(&window));

    auto *tree = window.findChild<QTreeView *>(QStringLiteral("fileExplorer"));
    auto *model = tree ? qobject_cast<QFileSystemModel *>(tree->model()) : nullptr;
    ASSERT_NE(model, nullptr);
    // A plain QModelIndex goes stale when the model sorts; this one follows.
    QPersistentModelIndex welcome;
    ASSERT_TRUE(QTest::qWaitFor(
        [&] {
            welcome = model->index(path(QStringLiteral("Welcome.md")));
            return welcome.isValid() && tree->visualRect(welcome).isValid();
        },
        5000));
    QTest::qWait(300); // the sort settles
    tree->selectionModel()->clearSelection();
    tree->setCurrentIndex(QModelIndex());

    // A hover event straight to the viewport: the view's hover row follows it,
    // whether or not the offscreen platform generates hover events itself.
    const QPointF over = tree->visualRect(welcome).center();
    QHoverEvent hover(QEvent::HoverMove, over, tree->viewport()->mapToGlobal(over), QPointF(-1, -1));
    QApplication::sendEvent(tree->viewport(), &hover);
    QTest::qWait(50);

    const RowPixels pixels = sampleRow(tree, QModelIndex(welcome));
    EXPECT_EQ(pixels.gutterNear, pixels.belowNear);
    EXPECT_EQ(pixels.gutterFar, pixels.belowFar);
    EXPECT_NE(pixels.inside, pixels.belowNear) << "the hovered row is drawn at all";
}

TEST_F(WindowTest, F2RenamesThroughTheVaultSoLinksFollow)
{
    write(QStringLiteral("Linker.md"), "See [[Welcome]].\n");

    MainWindow window;
    QSignalSpy indexed(window.index(), &Index::changed);
    window.openVault(dir.path());
    ASSERT_TRUE(indexed.wait(5000));
    window.show();
    ASSERT_TRUE(QTest::qWaitForWindowExposed(&window));

    auto *tree = window.findChild<QTreeView *>(QStringLiteral("fileExplorer"));
    auto *model = tree ? qobject_cast<QFileSystemModel *>(tree->model()) : nullptr;
    ASSERT_NE(model, nullptr);
    QPersistentModelIndex welcome;
    ASSERT_TRUE(QTest::qWaitFor(
        [&] {
            welcome = model->index(path(QStringLiteral("Welcome.md")));
            return welcome.isValid() && tree->visualRect(welcome).isValid();
        },
        5000));
    QTest::qWait(300); // the sort settles
    tree->setCurrentIndex(welcome);
    tree->setFocus();

    QTest::keyClick(tree, Qt::Key_F2);
    auto *edit = tree->findChild<QLineEdit *>(QStringLiteral("fileRenameEditor"));
    ASSERT_NE(edit, nullptr) << "F2 opens the vault's own rename editor";
    EXPECT_EQ(edit->text(), QStringLiteral("Welcome")) << "no extension in the editor";
    edit->setText(QStringLiteral("Hello"));
    QTest::keyClick(edit, Qt::Key_Return);

    // The rename is queued out of the editor's own close; wait for it.
    ASSERT_TRUE(QTest::qWaitFor([&] { return QFile::exists(path(QStringLiteral("Hello.md"))); }, 5000));
    EXPECT_FALSE(QFile::exists(path(QStringLiteral("Welcome.md"))));
    EXPECT_EQ(readAll(path(QStringLiteral("Linker.md"))), QByteArray("See [[Hello]].\n"));
}

TEST_F(WindowTest, SidebarHeaderHasCollapseAllAndSortButtons)
{
    MainWindow window;
    window.resize(1000, 700);
    window.openVault(dir.path());
    window.show();
    ASSERT_TRUE(QTest::qWaitForWindowExposed(&window));

    auto *tree = window.findChild<QTreeView *>(QStringLiteral("fileExplorer"));
    auto *model = tree ? qobject_cast<QFileSystemModel *>(tree->model()) : nullptr;
    ASSERT_NE(model, nullptr);
    QPersistentModelIndex projects;
    ASSERT_TRUE(QTest::qWaitFor(
        [&] {
            projects = model->index(path(QStringLiteral("Projects")));
            return projects.isValid() && tree->visualRect(projects).isValid();
        },
        5000));
    tree->expand(projects);
    ASSERT_TRUE(tree->isExpanded(projects));

    auto *collapse = window.findChild<QToolButton *>(QStringLiteral("sidebarCollapseButton"));
    ASSERT_NE(collapse, nullptr);
    collapse->click();
    EXPECT_FALSE(tree->isExpanded(projects));

    auto *sort = window.findChild<QToolButton *>(QStringLiteral("sidebarSortButton"));
    auto *menu = window.findChild<QMenu *>(QStringLiteral("sidebarSortMenu"));
    ASSERT_NE(sort, nullptr);
    ASSERT_NE(menu, nullptr);
    const auto trigger = [&](const QString &text) {
        for (QAction *action : menu->actions()) {
            if (action->text() == text) {
                action->trigger();
                return true;
            }
        }
        return false;
    };
    ASSERT_TRUE(trigger(QStringLiteral("File name (Z to A)")));
    EXPECT_EQ(tree->header()->sortIndicatorSection(), 0);
    EXPECT_EQ(tree->header()->sortIndicatorOrder(), Qt::DescendingOrder);
    ASSERT_TRUE(trigger(QStringLiteral("Modified time (old to new)")));
    EXPECT_EQ(tree->header()->sortIndicatorSection(), 3);
    EXPECT_EQ(tree->header()->sortIndicatorOrder(), Qt::AscendingOrder);
}

TEST_F(WindowTest, TreeRowsHideTheMarkdownExtensionAndTagOtherTypes)
{
    // Pure functions on the file name: no pixels, no model.
    EXPECT_EQ(noteAppC::FileRowDelegate::displayName(QFileInfo(path(QStringLiteral("Welcome.md")))), QStringLiteral("Welcome"));
    EXPECT_EQ(noteAppC::FileRowDelegate::displayName(QFileInfo(path(QStringLiteral("Projects")))), QStringLiteral("Projects"));
    EXPECT_EQ(noteAppC::FileRowDelegate::displayName(QFileInfo(path(QStringLiteral("attachment.txt")))),
              QStringLiteral("attachment"));
    EXPECT_EQ(noteAppC::FileRowDelegate::tagFor(QFileInfo(path(QStringLiteral("attachment.txt")))), QStringLiteral("TXT"));
    EXPECT_EQ(noteAppC::FileRowDelegate::tagFor(QFileInfo(path(QStringLiteral("Welcome.md")))), QString());
    EXPECT_EQ(noteAppC::FileRowDelegate::tagFor(QFileInfo(path(QStringLiteral("Projects")))), QString()) << "a folder has no tag";
    EXPECT_EQ(noteAppC::FileRowDelegate::tagFor(QFileInfo(QStringLiteral("x/Board.canvas"))), QStringLiteral("CANVAS"));
}

// --- the Meta notes pane, status count and Alt+] / Alt+[ -----------------------

namespace {

/// A private settings directory for a test that flips switches, put back after.
struct PrivateConfig {
    QTemporaryDir dir;
    QByteArray previous = qgetenv("XDG_CONFIG_HOME");
    PrivateConfig() { qputenv("XDG_CONFIG_HOME", dir.path().toUtf8()); }
    ~PrivateConfig() { qputenv("XDG_CONFIG_HOME", previous); }
};

noteAppC::ToggleSwitch *settingsToggle(MainWindow &window, const char *key)
{
    noteAppC::ToggleSwitch *found = nullptr;
    EXPECT_TRUE(QTest::qWaitFor([&] {
        return (found = settingsWidget<noteAppC::ToggleSwitch>(window, QStringLiteral("settingsToggle_") + QLatin1String(key)));
    }));
    return found;
}

/// Opens the settings page, flips editor.metaNotes and comes back to the note.
void switchMetaNotesOn(MainWindow &window)
{
    window.findChild<QAction *>(QStringLiteral("app:open-settings"))->trigger();
    settingsToggle(window, "editor.metaNotes")->click();
    window.closeSettings();
}

QTreeWidget *metaTree(MainWindow &window)
{
    auto *pane = window.findChild<noteAppC::MetaNotesPane *>();
    return pane ? pane->findChild<QTreeWidget *>() : nullptr;
}

QString metaStatus(MainWindow &window, bool *visible = nullptr)
{
    auto *button = window.findChild<QToolButton *>(QStringLiteral("metaNoteStatus"));
    if (visible)
        *visible = button && button->isVisibleTo(&window);
    return button ? button->text() : QString();
}

constexpr QLatin1StringView kMetaNote{"# One\n\nA [first] b.\n\n# Two\n\nplain\n\n# Three\n\nC [second] d [third].\n"};

} // namespace

namespace {

/// A vault of one file per type the hidden-types tests care about.
struct TypesVault {
    QTemporaryDir dir;
    TypesVault()
    {
        for (const QString &name : {QStringLiteral("a.md"), QStringLiteral("b.png"), QStringLiteral("c.pdf"),
                                    QStringLiteral("d.tar.gz"), QStringLiteral("sub/e.PNG")}) {
            QDir(dir.path()).mkpath(QFileInfo(name).path());
            QFile file(dir.filePath(name));
            EXPECT_TRUE(file.open(QIODevice::WriteOnly));
            file.write("x\n");
        }
    }
};

/// The names the tree shows under `parent` (hidden rows left out), sorted: the
/// model's own order follows the sidebar's sort and is not what is tested here.
QStringList visibleRows(QTreeView *tree, const QModelIndex &parent)
{
    auto *model = qobject_cast<QFileSystemModel *>(tree->model());
    QStringList names;
    for (int row = 0; row < model->rowCount(parent); ++row) {
        if (!tree->isRowHidden(row, parent))
            names << model->fileName(model->index(row, 0, parent));
    }
    names.sort();
    return names;
}

} // namespace

TEST_F(WindowTest, HiddenFileTypesLeaveTheSidebarAndComeBackWithoutReopeningTheVault)
{
    PrivateConfig config;
    TypesVault vault;
    MainWindow window;
    window.show();
    window.openVault(vault.dir.path());
    auto *tree = window.findChild<QTreeView *>(QStringLiteral("fileExplorer"));
    auto *model = qobject_cast<QFileSystemModel *>(tree->model());
    const QStringList everything{QStringLiteral("a.md"), QStringLiteral("b.png"), QStringLiteral("c.pdf"),
                                 QStringLiteral("d.tar.gz"), QStringLiteral("sub")}; // sorted
    WAIT_TRUE(tree->rootIndex().isValid() && model->rowCount(tree->rootIndex()) == 5, 5000);
    EXPECT_EQ(visibleRows(tree, tree->rootIndex()), everything) << "empty setting: everything shows";

    // Case and a leading dot do not matter, and "tar.gz" matches the end of the name.
    window.findChild<QAction *>(QStringLiteral("app:open-settings"))->trigger();
    QLineEdit *edit = nullptr;
    WAIT_TRUE((edit = settingsWidget<QLineEdit>(window, QStringLiteral("settingsLineEdit_files.hiddenExtensions"))), 2000);
    QTest::keyClicks(edit, QStringLiteral("PNG, .tar.gz"));
    window.closeSettings();
    EXPECT_EQ(visibleRows(tree, tree->rootIndex()),
              (QStringList{QStringLiteral("a.md"), QStringLiteral("c.pdf"), QStringLiteral("sub")}));

    // sub/ loads after the setting was made: its rows are hidden as they arrive.
    const QModelIndex sub = model->index(vault.dir.filePath(QStringLiteral("sub")));
    tree->expand(sub);
    WAIT_TRUE(model->rowCount(sub) == 1, 5000);
    EXPECT_TRUE(visibleRows(tree, sub).isEmpty()) << "sub/e.PNG";

    // Cleared, they are back, live.
    window.findChild<QAction *>(QStringLiteral("app:open-settings"))->trigger();
    QTest::keyClick(edit, Qt::Key_A, Qt::ControlModifier);
    QTest::keyClick(edit, Qt::Key_Backspace);
    window.closeSettings();
    EXPECT_EQ(visibleRows(tree, tree->rootIndex()), everything);
    EXPECT_EQ(visibleRows(tree, sub), QStringList{QStringLiteral("e.PNG")});
}

TEST_F(WindowTest, HiddenFileTypesAreHiddenFromTheFileListOnlyAndFromTheFirstLoad)
{
    PrivateConfig config;
    {
        Settings settings = Settings::load();
        settings.setValue(QStringLiteral("files.hiddenExtensions"), QStringList{QStringLiteral("MD")});
        ASSERT_TRUE(settings.save());
    }
    TypesVault vault;
    MainWindow window;
    window.show();
    window.openVault(vault.dir.path());
    auto *tree = window.findChild<QTreeView *>(QStringLiteral("fileExplorer"));
    auto *model = qobject_cast<QFileSystemModel *>(tree->model());
    WAIT_TRUE(tree->rootIndex().isValid() && model->rowCount(tree->rootIndex()) == 5, 5000);
    EXPECT_EQ(visibleRows(tree, tree->rootIndex()),
              (QStringList{QStringLiteral("b.png"), QStringLiteral("c.pdf"), QStringLiteral("d.tar.gz"), QStringLiteral("sub")}));

    // A hidden note still opens, revealing it in the tree quietly does nothing,
    // and the quick switcher still lists it.
    window.openNote(vault.dir.filePath(QStringLiteral("a.md")));
    EXPECT_TRUE(window.windowTitle().startsWith(QStringLiteral("a.md")));
    EXPECT_FALSE(tree->currentIndex().isValid() && model->fileName(tree->currentIndex()) == QStringLiteral("a.md"));
    window.findChild<QAction *>(QStringLiteral("switcher:open"))->trigger();
    auto *switcher = window.findChild<noteAppC::QuickSwitcher *>(QStringLiteral("quickSwitcher"));
    auto *query = switcher->findChild<QLineEdit *>(QStringLiteral("quickSwitcherQuery"));
    QTest::keyClicks(query, QStringLiteral("a.md"));
    auto *results = switcher->findChild<QListWidget *>(QStringLiteral("quickSwitcherResults"));
    WAIT_TRUE(results->count() == 1, 5000);
}

TEST_F(WindowTest, TheMetaNotesPaneIsAbsentWhileTheSettingIsOff)
{
    PrivateConfig config;
    write(QStringLiteral("Meta.md"), QByteArray(kMetaNote.data()));
    MainWindow window;
    window.show();
    window.openVault(dir.path());
    window.openNote(path(QStringLiteral("Meta.md")));

    EXPECT_EQ(window.findChild<noteAppC::MetaNotesPane *>(), nullptr);
    EXPECT_EQ(window.findChild<QToolButton *>(QStringLiteral("metaNotesTabButton")), nullptr);
    EXPECT_FALSE(window.findChild<QAction *>(QStringLiteral("noteAppC:next-meta-note"))->isEnabled());
    EXPECT_FALSE(window.findChild<QAction *>(QStringLiteral("noteAppC:previous-meta-note"))->isEnabled());
    bool visible = true;
    metaStatus(window, &visible);
    EXPECT_FALSE(visible);

    switchMetaNotesOn(window);
    EXPECT_NE(window.findChild<noteAppC::MetaNotesPane *>(), nullptr);
    const QLayout *tabs = window.findChild<QWidget *>(QStringLiteral("rightSidebarTabs"))->layout();
    const int outline = tabs->indexOf(window.findChild<QToolButton *>(QStringLiteral("outlineTabButton")));
    EXPECT_EQ(tabs->indexOf(window.findChild<QToolButton *>(QStringLiteral("metaNotesTabButton"))), outline + 1);
    EXPECT_TRUE(window.findChild<QAction *>(QStringLiteral("noteAppC:next-meta-note"))->isEnabled());

    // And off again: gone, with nothing left in the status bar.
    window.findChild<QAction *>(QStringLiteral("app:open-settings"))->trigger();
    settingsToggle(window, "editor.metaNotes")->click();
    window.closeSettings();
    EXPECT_EQ(window.findChild<noteAppC::MetaNotesPane *>(), nullptr);
    EXPECT_FALSE(window.findChild<QAction *>(QStringLiteral("noteAppC:next-meta-note"))->isEnabled());
    metaStatus(window, &visible);
    EXPECT_FALSE(visible);
}

TEST_F(WindowTest, TheMetaNotesPaneGroupsRunsUnderHeadingsAndJumps)
{
    PrivateConfig config;
    write(QStringLiteral("Meta.md"), QByteArray(kMetaNote.data()));
    MainWindow window;
    window.show();
    window.openVault(dir.path());
    switchMetaNotesOn(window);
    window.openNote(path(QStringLiteral("Meta.md")));
    MarkdownEditor *editor = window.editor();

    QTreeWidget *tree = nullptr;
    WAIT_TRUE((tree = metaTree(window)) && tree->topLevelItemCount() == 2, 2000);
    EXPECT_EQ(tree->topLevelItem(0)->text(0), QStringLiteral("One"));
    EXPECT_EQ(tree->topLevelItem(1)->text(0), QStringLiteral("Three")) << "Two has none and is not listed";
    EXPECT_EQ(tree->topLevelItem(0)->childCount(), 1);
    EXPECT_EQ(tree->topLevelItem(1)->childCount(), 2);
    const auto rowText = [&](QTreeWidgetItem *item) {
        auto *label = qobject_cast<QLabel *>(tree->itemWidget(item, 0));
        return label ? label->text() : QString();
    };
    EXPECT_TRUE(rowText(tree->topLevelItem(0)->child(0)).contains(QStringLiteral(">first<")));
    EXPECT_EQ(metaStatus(window), QStringLiteral("3 meta notes"));

    // A click on a row selects the whole run in the editor.
    emit tree->itemClicked(tree->topLevelItem(0)->child(0), 0);
    EXPECT_EQ(editor->selectedText(), QStringLiteral("[first]"));

    // The row the caret is in is the highlighted one.
    editor->setCursorOffset(editor->noteText().indexOf(QStringLiteral("second")) + 1);
    auto *second = qobject_cast<QLabel *>(tree->itemWidget(tree->topLevelItem(1)->child(0), 0));
    ASSERT_NE(second, nullptr);
    EXPECT_TRUE(second->property("current").toBool());
    const QPointer<QLabel> label = second; // a rebuild deletes it, and a new one may take its address

    // Typing a letter outside the runs leaves the rows alone (no rebuild)...
    QTreeWidgetItem *before = tree->topLevelItem(1)->child(0);
    editor->setCursorOffset(editor->noteText().indexOf(QStringLiteral("plain")));
    QTest::keyClicks(editor, QStringLiteral("x"));
    QTest::qWait(150); // the parse
    EXPECT_EQ(tree->topLevelItem(1)->child(0), before);
    EXPECT_FALSE(label.isNull());
    // ...typing inside a run rewrites that row's label, not the list...
    editor->setCursorOffset(editor->noteText().indexOf(QStringLiteral("second]")) + 6);
    QTest::keyClicks(editor, QStringLiteral("ly"));
    WAIT_TRUE(rowText(tree->topLevelItem(1)->child(0)).contains(QStringLiteral(">secondly<")), 2000);
    EXPECT_EQ(tree->topLevelItem(1)->child(0), before);
    EXPECT_FALSE(label.isNull()) << "the same label widget, not a rebuilt one";
    // ...a fourth run adds a row and bumps the count once the parse lands.
    editor->setCursorOffset(editor->noteText().size());
    QTest::keyClicks(editor, QStringLiteral(" [fourth]"));
    WAIT_TRUE(metaStatus(window) == QStringLiteral("4 meta notes"), 2000);
    EXPECT_EQ(tree->topLevelItem(1)->childCount(), 3);
}

TEST_F(WindowTest, AltBracketJumpsAndTheStatusBarSaysWhich)
{
    PrivateConfig config;
    write(QStringLiteral("Meta.md"), QByteArray(kMetaNote.data()));
    MainWindow window;
    window.show();
    window.openVault(dir.path());
    switchMetaNotesOn(window);
    window.openNote(path(QStringLiteral("Meta.md")));
    MarkdownEditor *editor = window.editor();
    ASSERT_TRUE(QTest::qWaitForWindowActive(&window));
    WAIT_TRUE(editor->metaNoteRanges().size() == 3, 2000);
    const QString text = editor->noteText();
    editor->setCursorOffset(0);
    editor->setFocus();

    QTest::keyClick(editor, Qt::Key_BracketRight, Qt::AltModifier);
    EXPECT_EQ(editor->selectedText(), QStringLiteral("[first]"));
    QTest::keyClick(editor, Qt::Key_BracketRight, Qt::AltModifier);
    EXPECT_EQ(editor->selectedText(), QStringLiteral("[second]"));
    EXPECT_EQ(window.statusBar()->currentMessage(), QStringLiteral("Meta note 2 of 3"));
    QTest::keyClick(editor, Qt::Key_BracketRight, Qt::AltModifier);
    QTest::keyClick(editor, Qt::Key_BracketRight, Qt::AltModifier);
    EXPECT_EQ(editor->selectedText(), QStringLiteral("[first]"));
    EXPECT_EQ(window.statusBar()->currentMessage(), QStringLiteral("Meta note 1 of 3 (wrapped around)"));
    QTest::keyClick(editor, Qt::Key_BracketLeft, Qt::AltModifier);
    EXPECT_EQ(editor->selectedText(), QStringLiteral("[third]"));
    EXPECT_EQ(window.statusBar()->currentMessage(), QStringLiteral("Meta note 3 of 3 (wrapped around)"));
    EXPECT_EQ(editor->noteText(), text) << "the editor typed no bracket";
}

TEST_F(WindowTest, LinkCompletionReadsHeadingsAndAliasesFromTheIndex)
{
    PrivateConfig config;
    write(QStringLiteral("Sections.md"), "---\naliases: [Parts]\n---\n# Setup\n\n## Usage\n\ntext ^blk1\n");
    write(QStringLiteral("Host.md"), "see \n");
    MainWindow window;
    window.show();
    auto *indexer = window.findChild<noteAppC::Indexer *>();
    ASSERT_NE(indexer, nullptr);
    QSignalSpy scanned(indexer, &noteAppC::Indexer::finished);
    window.openVault(dir.path());
    ASSERT_TRUE(scanned.wait(10000)); // its changed() has fed the open editors by now
    window.openNote(path(QStringLiteral("Host.md")));
    auto *editor = window.findChild<MarkdownEditor *>();
    ASSERT_NE(editor, nullptr);
    editor->setCursorOffset(4);

    const auto popupRows = [] {
        QStringList rows;
        for (QWidget *widget : QApplication::allWidgets()) {
            auto *popup = qobject_cast<QListView *>(widget);
            if (!popup || popup->objectName() != QLatin1String("completerPopup") || !popup->isVisible())
                continue;
            for (int row = 0; row < popup->model()->rowCount(); ++row)
                rows << popup->model()->index(row, 0).data().toString();
        }
        return rows;
    };
    QTest::keyClicks(editor, QStringLiteral("[[Par"));
    EXPECT_EQ(popupRows(), QStringList{QStringLiteral("Parts -> Sections")});
    for (int i = 0; i < 3; ++i)
        QTest::keyClick(editor, Qt::Key_Backspace);
    QTest::keyClicks(editor, QStringLiteral("Sections#"));
    EXPECT_EQ(popupRows(), (QStringList{QStringLiteral("Setup"), QStringLiteral("Usage")}));
    QTest::keyClicks(editor, QStringLiteral("^"));
    EXPECT_EQ(popupRows(), QStringList{QStringLiteral("blk1")});
}

TEST_F(WindowTest, ChangingTheDelimitersRefreshesThePaneAndTheCount)
{
    PrivateConfig config;
    write(QStringLiteral("Meta.md"), "Some [a] and << b >> here.\n");
    MainWindow window;
    window.show();
    window.openVault(dir.path());
    switchMetaNotesOn(window);
    window.openNote(path(QStringLiteral("Meta.md")));
    WAIT_TRUE(metaStatus(window) == QStringLiteral("2 meta notes") || metaStatus(window) == QStringLiteral("1 meta note"), 2000);

    auto *indexer = window.findChild<noteAppC::Indexer *>();
    QSignalSpy scans(indexer, &noteAppC::Indexer::finished);
    const auto settled = [&] { // no scan running, and none started for a while
        for (qsizetype seen = -1; seen != scans.count();) {
            seen = scans.count();
            EXPECT_TRUE(QTest::qWaitFor([&] { return !indexer->isRunning(); }, 5000));
            QTest::qWait(300);
        }
        return scans.count();
    };
    const qsizetype before = settled();
    window.findChild<QAction *>(QStringLiteral("app:open-settings"))->trigger();
    const auto type = [&window](const char *key, const QString &text) {
        QLineEdit *edit = nullptr;
        EXPECT_TRUE(QTest::qWaitFor([&] {
            return (edit = settingsWidget<QLineEdit>(window, QStringLiteral("settingsLineEdit_") + QLatin1String(key)));
        }));
        QTest::keyClick(edit, Qt::Key_A, Qt::ControlModifier);
        QTest::keyClicks(edit, text);
    };
    type("editor.metaNoteOpen", QStringLiteral("<<"));
    type("editor.metaNoteClose", QStringLiteral(">>"));
    window.closeSettings();

    QTreeWidget *tree = nullptr;
    WAIT_TRUE((tree = metaTree(window)) && !tree->isHidden() && tree->topLevelItemCount() == 1, 2000);
    EXPECT_EQ(tree->topLevelItem(0)->childCount(), 0) << "no headings: the rows are flat";
    // Stored once typing pauses.
    QLabel *label = nullptr;
    WAIT_TRUE((label = qobject_cast<QLabel *>(tree->itemWidget(tree->topLevelItem(0), 0)))
                  && label->text().contains(QStringLiteral(">b<")),
              2000);
    WAIT_TRUE(metaStatus(window) == QStringLiteral("1 meta note"), 2000);
    EXPECT_EQ(settled() - before, 1) << "one rescan for the new pair, not one per key";
}

TEST_F(WindowTest, TheMetaNotesPaneVaultModeListsEveryNoteWithRunsAndDrillsDown)
{
    PrivateConfig config;
    write(QStringLiteral("Alpha.md"), "A [one] x.\n");
    write(QStringLiteral("Beta.md"), "B [two] and [three].\n");
    MainWindow window;
    window.show();
    window.openVault(dir.path());
    switchMetaNotesOn(window);
    window.openNote(path(QStringLiteral("Alpha.md")));
    auto *pane = window.findChild<noteAppC::MetaNotesPane *>();
    ASSERT_NE(pane, nullptr);
    EXPECT_EQ(pane->findChild<QWidget *>(QStringLiteral("metaVaultSearch")), nullptr) << "built on the first press";

    const auto queryBoxes = window.findChildren<QLineEdit *>(QStringLiteral("quickSwitcherQuery")).size();
    window.findChild<QToolButton *>(QStringLiteral("metaNotesVaultButton"))->click();
    QTreeWidget *list = nullptr;
    WAIT_TRUE((list = pane->findChild<QTreeWidget *>(QStringLiteral("metaVaultList"))) && list->topLevelItemCount() == 2, 8000);
    QTreeWidgetItem *beta = nullptr;
    for (int i = 0; i < list->topLevelItemCount(); ++i) {
        if (list->topLevelItem(i)->text(0) == QStringLiteral("Beta"))
            beta = list->topLevelItem(i);
    }
    ASSERT_NE(beta, nullptr);
    EXPECT_EQ(beta->text(1), QStringLiteral("2")) << "one count per note: its runs";

    // The window's own Search pane is still the only one by its names.
    EXPECT_EQ(window.findChildren<QLineEdit *>(QStringLiteral("quickSwitcherQuery")).size(), queryBoxes);
    EXPECT_EQ(window.findChildren<QWidget *>(QStringLiteral("searchPane")).size(), 1);

    // A row opens the note at its first run and the pane lists that note's runs.
    emit list->itemClicked(beta->child(0), 0);
    WAIT_TRUE(window.editor()->noteText().startsWith(QStringLiteral("B [two]")), 2000);
    EXPECT_EQ(window.editor()->cursorOffset(), window.editor()->noteText().indexOf(QStringLiteral("[two]")));
    EXPECT_FALSE(window.findChild<QToolButton *>(QStringLiteral("metaNotesVaultButton"))->isChecked());
    QTreeWidget *tree = nullptr;
    WAIT_TRUE((tree = metaTree(window)) && !tree->isHidden() && tree->topLevelItemCount() == 2, 2000);
}

TEST_F(WindowTest, AnIconSetFromTheTreeSurvivesRestartAndRename)
{
    PrivateConfig config;
    {
        MainWindow window;
        window.openVault(dir.path());
        ASSERT_TRUE(QMetaObject::invokeMethod(&window, "setFileIcon", Q_ARG(QString, path(QStringLiteral("Welcome.md"))),
                                              Q_ARG(QString, QStringLiteral("zap"))));
        // The tree's F2 and the inline title both come here.
        const auto rename = [&window](const QString &from, bool isDir, const QString &to) {
            bool renamed = false;
            QString error;
            EXPECT_TRUE(QMetaObject::invokeMethod(&window, "renameVaultPath", Q_RETURN_ARG(bool, renamed),
                                                  Q_ARG(QString, from), Q_ARG(bool, isDir), Q_ARG(QString, to),
                                                  Q_ARG(QString *, &error)));
            return renamed;
        };
        ASSERT_TRUE(rename(path(QStringLiteral("Welcome.md")), false, QStringLiteral("Target")));
        ASSERT_TRUE(QMetaObject::invokeMethod(&window, "setFileIcon", Q_ARG(QString, path(QStringLiteral("Projects"))),
                                              Q_ARG(QString, QStringLiteral("folder-open"))));
        ASSERT_TRUE(rename(path(QStringLiteral("Projects")), true, QStringLiteral("Work")));
    }
    noteAppC::FileIcons icons(dir.path());
    icons.load();
    EXPECT_EQ(icons.iconFor(QStringLiteral("Target.md")), QStringLiteral("zap"));
    EXPECT_TRUE(icons.iconFor(QStringLiteral("Welcome.md")).isEmpty());
    EXPECT_EQ(icons.iconFor(QStringLiteral("Work")), QStringLiteral("folder-open"));
    EXPECT_FALSE(QFileInfo::exists(path(QStringLiteral(".obsidian/icons.json"))));
    EXPECT_FALSE(QDir(dir.path()).entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot)
                     .contains(QStringLiteral("icons.json")));
}

TEST_F(WindowTest, FoldsAreRememberedPerNoteAcrossSwitchesRestartsAndRenames)
{
    PrivateConfig config;
    write(QStringLiteral("A.md"), "intro\n\n> [!note]+ Fold me\n> body\n\n> [!warning]- Open me\n> body\n");
    write(QStringLiteral("B.md"), "> [!note]+ Fold me\n> other note, same title\n");
    const QString noteA = path(QStringLiteral("A.md"));
    const auto foldAt = [](MarkdownEditor *editor, const QString &line) {
        return editor->noteText().indexOf(line);
    };
    {
        MainWindow window;
        window.openVault(dir.path());
        window.openNote(noteA);
        MarkdownEditor *editor = window.editor();
        const qsizetype fold = foldAt(editor, QStringLiteral("> [!note]+ Fold me"));
        const qsizetype open = foldAt(editor, QStringLiteral("> [!warning]- Open me"));
        editor->toggleFoldAt(fold);
        editor->toggleFoldAt(open);
        ASSERT_TRUE(editor->isFoldedAt(fold));
        ASSERT_FALSE(editor->isFoldedAt(open));

        window.openNote(path(QStringLiteral("B.md"))); // same pane
        EXPECT_FALSE(window.editor()->isFoldedAt(0)) << "B has its own folds";
        for (MarkdownEditor *other : window.findChildren<MarkdownEditor *>())
            EXPECT_FALSE(other->noteText().startsWith(QStringLiteral("intro"))) << "A is not kept in another tab";
        window.openNote(noteA);
        EXPECT_TRUE(window.editor()->isFoldedAt(fold));
        EXPECT_FALSE(window.editor()->isFoldedAt(open));
        window.close(); // saves where every open note was left
    }
    {
        MainWindow window; // a restart on the same vault
        window.openVault(dir.path());
        window.openNote(noteA);
        const qsizetype fold = foldAt(window.editor(), QStringLiteral("> [!note]+ Fold me"));
        const qsizetype open = foldAt(window.editor(), QStringLiteral("> [!warning]- Open me"));
        EXPECT_TRUE(window.editor()->isFoldedAt(fold));
        EXPECT_FALSE(window.editor()->isFoldedAt(open));

        // Renamed while no tab holds it: the remembered state follows the file.
        window.openNote(path(QStringLiteral("B.md")));
        bool renamed = false;
        QString error;
        ASSERT_TRUE(QMetaObject::invokeMethod(&window, "renameVaultPath", Q_RETURN_ARG(bool, renamed),
                                              Q_ARG(QString, noteA), Q_ARG(bool, false),
                                              Q_ARG(QString, QStringLiteral("Renamed")), Q_ARG(QString *, &error)));
        ASSERT_TRUE(renamed);
        window.openNote(path(QStringLiteral("Renamed.md")));
        EXPECT_TRUE(window.editor()->isFoldedAt(fold));
        EXPECT_FALSE(window.editor()->isFoldedAt(open));
    }
}

TEST_F(WindowTest, ARememberedHeadingFoldAroundTheRememberedCaretStaysFolded)
{
    PrivateConfig config;
    const QString text = "# A\n\nbody a\n\n## A1\n\nbody a1\n\n# B\n\nbody b\n";
    write(QStringLiteral("Fold.md"), text.toUtf8());
    write(QStringLiteral("Other.md"), "other\n");
    const QString note = path(QStringLiteral("Fold.md"));
    const qsizetype inside = text.indexOf(QLatin1String("body a1"));
    {
        MainWindow window;
        window.openVault(dir.path());
        window.openNote(note);
        MarkdownEditor *editor = window.editor();
        // The caret is under the fold, as a position saved by an older build
        // or a note edited on the side leaves it; setFoldState is no click.
        editor->setCursorOffset(inside);
        editor->setFoldState({QStringLiteral("10 # A")});
        ASSERT_TRUE(editor->isFoldedAt(0));

        window.openNote(path(QStringLiteral("Other.md")));
        window.openNote(note);
        EXPECT_EQ(window.editor()->cursorOffset(), inside);
        EXPECT_TRUE(window.editor()->isFoldedAt(0)) << "caret first, then folds: back from another note";
        window.close();
    }
    MainWindow window; // a restart
    window.openVault(dir.path());
    window.openNote(note);
    EXPECT_EQ(window.editor()->cursorOffset(), inside);
    EXPECT_TRUE(window.editor()->isFoldedAt(0)) << "and after reopening the window";
    EXPECT_FALSE(window.editor()->displayText().contains(QLatin1String("body a1")));
    QTest::keyClick(window.editor(), Qt::Key_Right);
    EXPECT_FALSE(window.editor()->isFoldedAt(0)) << "the first key reveals it";
}

TEST_F(WindowTest, FoldCommandsAreInThePaletteAndRebindable)
{
    PrivateConfig config;
    write(QStringLiteral("Fold.md"), "# A\n\nbody a\n\n# B\n\nbody b\n");
    ASSERT_TRUE(QDir(dir.path()).mkpath(QStringLiteral(".obsidian")));
    write(QStringLiteral(".obsidian/hotkeys.json"),
          R"({"editor:unfold-all": [{"modifiers": ["Mod", "Alt", "Shift"], "key": "U"}]})");
    MainWindow window;
    window.show();
    window.openVault(dir.path());
    window.openNote(path(QStringLiteral("Fold.md")));
    MarkdownEditor *editor = window.editor();

    window.findChild<QAction *>(QStringLiteral("command-palette:open"))->trigger();
    auto *palette = window.findChild<noteAppC::QuickSwitcher *>(QStringLiteral("commandPalette"));
    ASSERT_NE(palette, nullptr);
    auto *query = palette->findChild<QLineEdit *>(QStringLiteral("quickSwitcherQuery"));
    auto *results = palette->findChild<QListWidget *>(QStringLiteral("quickSwitcherResults"));
    QTest::keyClicks(query, QStringLiteral("fold"));
    QStringList listed;
    for (int i = 0; i < results->count(); ++i)
        listed.append(results->item(i)->text());
    for (const char *name : {"Toggle fold on the current line", "Fold all headings and lists",
                             "Unfold all headings and lists", "Fold more", "Fold less", "Toggle Properties fold"})
        EXPECT_TRUE(listed.contains(QLatin1String(name))) << name;

    QTest::keyClick(query, Qt::Key_Escape);
    window.findChild<QAction *>(QStringLiteral("command-palette:open"))->trigger();
    query = palette->findChild<QLineEdit *>(QStringLiteral("quickSwitcherQuery"));
    QTest::keyClicks(query, QStringLiteral("fold all"));
    QTest::keyClick(query, Qt::Key_Return);
    EXPECT_TRUE(editor->isFoldedAt(0));
    EXPECT_TRUE(editor->isFoldedAt(editor->noteText().indexOf(QLatin1String("# B")))) << "run from the palette";

    // The vault's hotkeys.json binds one; the others have no key.
    editor->setFocus();
    QTest::keyClick(editor, Qt::Key_U, Qt::ControlModifier | Qt::AltModifier | Qt::ShiftModifier);
    EXPECT_TRUE(editor->foldState().isEmpty()) << "Ctrl+Alt+Shift+U runs editor:unfold-all";
}

TEST_F(WindowTest, TheFoldHeadingAndFoldIndentSettingsReachTheEditor)
{
    PrivateConfig config;
    write(QStringLiteral("Fold.md"), "# A\n\nbody\n\n- p\n  - child-c\n");
    MainWindow window;
    window.show();
    window.openVault(dir.path());
    window.openNote(path(QStringLiteral("Fold.md")));
    const qsizetype list = window.editor()->noteText().indexOf(QLatin1String("- p"));
    window.editor()->toggleFoldAt(0);
    window.editor()->toggleFoldAt(list);
    ASSERT_TRUE(window.editor()->isFoldedAt(0));
    ASSERT_TRUE(window.editor()->isFoldedAt(list));

    window.findChild<QAction *>(QStringLiteral("app:open-settings"))->trigger();
    auto *heading = settingsToggle(window, "editor.foldHeading");
    auto *indent = settingsToggle(window, "editor.foldIndent");
    EXPECT_TRUE(heading->isChecked());
    EXPECT_TRUE(indent->isChecked());
    heading->click();
    indent->click();
    EXPECT_TRUE(window.editor()->displayText().contains(QLatin1String("body"))) << "no heading fold with it off";
    EXPECT_TRUE(window.editor()->displayText().contains(QLatin1String("child-c"))) << "and no list fold";
    heading->click();
    indent->click();
    EXPECT_FALSE(window.editor()->displayText().contains(QLatin1String("body"))) << "back on, the folds are back";
    EXPECT_FALSE(window.editor()->displayText().contains(QLatin1String("child-c")));
    window.closeSettings();
}

TEST_F(WindowTest, AnIconShowsOnTheTabAndGoesWhenTheToggleIsOff)
{
    PrivateConfig config;
    MainWindow window;
    window.show();
    window.openVault(dir.path());
    window.openNote(path(QStringLiteral("Welcome.md")));
    auto *tabs = window.findChild<QTabBar *>(QStringLiteral("noteTabs"));
    ASSERT_NE(tabs, nullptr);
    ASSERT_GE(tabs->count(), 1);
    EXPECT_TRUE(tabs->tabIcon(0).isNull());
    ASSERT_TRUE(QMetaObject::invokeMethod(&window, "setFileIcon", Q_ARG(QString, path(QStringLiteral("Welcome.md"))),
                                          Q_ARG(QString, QStringLiteral("zap"))));
    EXPECT_FALSE(tabs->tabIcon(0).isNull());

    window.findChild<QAction *>(QStringLiteral("app:open-settings"))->trigger();
    settingsToggle(window, "appearance.fileIcons")->click();
    window.closeSettings();
    EXPECT_TRUE(tabs->tabIcon(0).isNull()) << "off hides it";
    noteAppC::FileIcons icons(dir.path());
    icons.load();
    EXPECT_EQ(icons.iconFor(QStringLiteral("Welcome.md")), QStringLiteral("zap")) << "and keeps it";
}

TEST_F(WindowTest, ARenameWithoutIconsWritesNoIconFile)
{
    PrivateConfig config;
    MainWindow window;
    window.openVault(dir.path());
    bool renamed = false;
    QString error;
    ASSERT_TRUE(QMetaObject::invokeMethod(&window, "renameVaultPath", Q_RETURN_ARG(bool, renamed),
                                          Q_ARG(QString, path(QStringLiteral("Welcome.md"))), Q_ARG(bool, false),
                                          Q_ARG(QString, QStringLiteral("Target")), Q_ARG(QString *, &error)));
    ASSERT_TRUE(renamed) << error.toStdString();
    EXPECT_FALSE(QFileInfo::exists(noteAppC::FileIcons::storagePathFor(dir.path())));
}

TEST_F(WindowTest, DeletingANoteDropsItsIcon)
{
    PrivateConfig config;
    MainWindow window;
    window.openVault(dir.path());
    ASSERT_TRUE(QMetaObject::invokeMethod(&window, "setFileIcon", Q_ARG(QString, path(QStringLiteral("Welcome.md"))),
                                          Q_ARG(QString, QStringLiteral("zap"))));
    auto *tree = window.findChild<QTreeView *>(QStringLiteral("fileExplorer"));
    auto *model = qobject_cast<QFileSystemModel *>(tree->model());
    const QModelIndex index = model->index(path(QStringLiteral("Welcome.md")));
    ASSERT_TRUE(index.isValid());
    QTimer::singleShot(0, &window, [] {
        if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget()))
            box->button(QMessageBox::Yes)->click();
    });
    ASSERT_TRUE(QMetaObject::invokeMethod(&window, "deletePathAt", Q_ARG(QModelIndex, index)));
    ASSERT_FALSE(QFile::exists(path(QStringLiteral("Welcome.md"))));
    noteAppC::FileIcons icons(dir.path());
    icons.load();
    EXPECT_TRUE(icons.iconFor(QStringLiteral("Welcome.md")).isEmpty());
}

// The vault's meta notes are indexed by the settings in force when it opens:
// a warm cache built with them on is trusted, not cleared and re-parsed.
TEST_F(WindowTest, AWarmOpenWithMetaNotesOnReparsesNothing)
{
    PrivateConfig config;
    {
        Settings settings = Settings::load();
        settings.setValue(QStringLiteral("editor.metaNotes"), true);
        ASSERT_TRUE(settings.save());
    }
    write(QStringLiteral("Meta.md"), QByteArray(kMetaNote.data()));
    const auto open = [this](int *indexed) {
        MainWindow window;
        auto *indexer = window.findChild<noteAppC::Indexer *>();
        ASSERT_NE(indexer, nullptr);
        QSignalSpy done(indexer, &noteAppC::Indexer::finished);
        window.openVault(dir.path());
        ASSERT_TRUE(QTest::qWaitFor([&] { return !done.isEmpty() && !indexer->isRunning(); }, 20000));
        QTest::qWait(200); // a queued second scan would have started by now
        ASSERT_FALSE(indexer->isRunning());
        *indexed = 0;
        for (const QList<QVariant> &scan : std::as_const(done))
            *indexed += scan.at(0).toInt();
        EXPECT_EQ(window.index()->search(QStringLiteral("meta:")).size(), 1);
    };
    int cold = 0;
    open(&cold);
    EXPECT_GT(cold, 0);
    int warm = -1;
    open(&warm);
    EXPECT_EQ(warm, 0) << "nothing changed on disk or in the settings";
}

namespace {
bool moveVault(MainWindow &window, const QString &from, bool isDir, const QString &toFolder, QString *error)
{
    bool moved = false;
    EXPECT_TRUE(QMetaObject::invokeMethod(&window, "moveVaultPath", Q_RETURN_ARG(bool, moved),
                                          Q_ARG(QString, from), Q_ARG(bool, isDir), Q_ARG(QString, toFolder),
                                          Q_ARG(QString *, error)));
    return moved;
}
} // namespace

// "Move file to another folder" and a tree drag end in moveVaultPath: the
// same follow-up as a rename - links, the open tab, the icon, the unsaved text.
TEST_F(WindowTest, MovingAnOpenLinkedNoteKeepsLinksTabIconAndTypedText)
{
    ASSERT_TRUE(QDir(dir.path()).mkpath(QStringLiteral("Src")));
    write(QStringLiteral("Src/Target.md"), "# Target\n");
    write(QStringLiteral("Home.md"), "Bare [[Target]] and path [[Src/Target]].\n");

    MainWindow window;
    QSignalSpy indexed(window.index(), &Index::changed);
    window.openVault(dir.path());
    ASSERT_TRUE(indexed.wait(5000));
    window.openNote(path(QStringLiteral("Src/Target.md")));
    ASSERT_TRUE(QMetaObject::invokeMethod(&window, "setFileIcon", Q_ARG(QString, path(QStringLiteral("Src/Target.md"))),
                                          Q_ARG(QString, QStringLiteral("zap"))));
    auto *editor = window.editor();
    editor->setCursorOffset(editor->noteText().size());
    QTest::keyClicks(editor, QStringLiteral("x"));
    ASSERT_TRUE(editor->isModified()) << "the move must beat the autosave timer";

    QString error;
    ASSERT_TRUE(moveVault(window, path(QStringLiteral("Src/Target.md")), false, path(QStringLiteral("Projects")), &error))
        << error.toStdString();

    EXPECT_FALSE(QFile::exists(path(QStringLiteral("Src/Target.md"))));
    EXPECT_EQ(readAll(path(QStringLiteral("Projects/Target.md"))), QByteArray("# Target\nx")) << "typed text came along";
    EXPECT_EQ(readAll(path(QStringLiteral("Home.md"))), QByteArray("Bare [[Target]] and path [[Projects/Target]].\n"));
    EXPECT_EQ(editor->noteText(), QStringLiteral("# Target\nx"));

    // The tab now saves to the new path, not the old one.
    QTest::keyClicks(editor, QStringLiteral("y"));
    WAIT_TRUE(!editor->isModified(), 5000);
    EXPECT_EQ(readAll(path(QStringLiteral("Projects/Target.md"))), QByteArray("# Target\nxy"));
    EXPECT_FALSE(QFile::exists(path(QStringLiteral("Src/Target.md"))));

    noteAppC::FileIcons icons(dir.path());
    icons.load();
    EXPECT_EQ(icons.iconFor(QStringLiteral("Projects/Target.md")), QStringLiteral("zap"));
    EXPECT_TRUE(icons.iconFor(QStringLiteral("Src/Target.md")).isEmpty());
}

TEST_F(WindowTest, MovingAFolderRewritesPathLinksIntoItAndMovesToTheRoot)
{
    ASSERT_TRUE(QDir(dir.path()).mkpath(QStringLiteral("Src")));
    write(QStringLiteral("Src/Target.md"), "# Target\n");
    write(QStringLiteral("Home.md"), "See [[Src/Target]].\n");

    MainWindow window;
    QSignalSpy indexed(window.index(), &Index::changed);
    window.openVault(dir.path());
    ASSERT_TRUE(indexed.wait(5000));

    QString error;
    ASSERT_TRUE(moveVault(window, path(QStringLiteral("Src")), true, path(QStringLiteral("Projects")), &error))
        << error.toStdString();
    EXPECT_TRUE(QFile::exists(path(QStringLiteral("Projects/Src/Target.md"))));
    EXPECT_EQ(readAll(path(QStringLiteral("Home.md"))), QByteArray("See [[Projects/Src/Target]].\n"));

    // Back out to the vault root, where a drop on empty space goes.
    ASSERT_TRUE(moveVault(window, path(QStringLiteral("Projects/Src")), true, dir.path(), &error)) << error.toStdString();
    EXPECT_TRUE(QFile::exists(path(QStringLiteral("Src/Target.md"))));
}

TEST_F(WindowTest, AMoveIntoItsOwnSubfolderOrOntoAClashIsRefused)
{
    ASSERT_TRUE(QDir(dir.path()).mkpath(QStringLiteral("Projects/Sub")));
    ASSERT_TRUE(QDir(dir.path()).mkpath(QStringLiteral("Other")));
    write(QStringLiteral("Other/Crlf.md"), "other\n");

    MainWindow window;
    window.openVault(dir.path());
    QString error;
    EXPECT_FALSE(moveVault(window, path(QStringLiteral("Projects")), true, path(QStringLiteral("Projects/Sub")), &error));
    EXPECT_FALSE(error.isEmpty());
    EXPECT_FALSE(moveVault(window, path(QStringLiteral("Projects")), true, path(QStringLiteral("Projects")), &error));
    EXPECT_TRUE(QFile::exists(path(QStringLiteral("Projects/Sub"))));

    error.clear();
    EXPECT_FALSE(moveVault(window, path(QStringLiteral("Other/Crlf.md")), false, path(QStringLiteral("Projects")), &error));
    EXPECT_FALSE(error.isEmpty());
    EXPECT_EQ(readAll(path(QStringLiteral("Other/Crlf.md"))), QByteArray("other\n"));
    EXPECT_EQ(readAll(path(QStringLiteral("Projects/Crlf.md"))), QByteArray("one\r\ntwo\r\n"));
}

// The tree only reports a drop; the window moves. A drop that did not start in
// the tree (a file from another app) is ignored.
TEST_F(WindowTest, ADropInTheTreeMovesThroughTheWindowAndOnlyIfItStartedThere)
{
    ASSERT_TRUE(QDir(dir.path()).mkpath(QStringLiteral("Src")));
    write(QStringLiteral("Src/Target.md"), "# Target\n");

    MainWindow window;
    window.show();
    window.openVault(dir.path());
    auto *tree = window.findChild<noteAppC::FileTree *>(QStringLiteral("fileExplorer"));
    ASSERT_NE(tree, nullptr);
    auto *model = qobject_cast<QFileSystemModel *>(tree->model());
    WAIT_TRUE(tree->rootIndex().isValid() && model->rowCount(tree->rootIndex()) >= 4, 5000);

    const QModelIndex projects = model->index(path(QStringLiteral("Projects")));
    tree->expand(model->index(path(QStringLiteral("Src"))));
    WAIT_TRUE(model->index(path(QStringLiteral("Src/Target.md"))).isValid() && tree->visualRect(model->index(path(QStringLiteral("Src/Target.md")))).isValid(), 5000);
    tree->setCurrentIndex(model->index(path(QStringLiteral("Src/Target.md"))));
    const QPoint onFolder = tree->visualRect(projects).center();
    EXPECT_EQ(tree->dropFolderAt(onFolder), path(QStringLiteral("Projects")));
    EXPECT_EQ(tree->dropFolderAt(tree->visualRect(model->index(path(QStringLiteral("Welcome.md")))).center()), dir.path())
        << "a file row drops into its own folder";
    EXPECT_EQ(tree->dropFolderAt(QPoint(5, tree->viewport()->height() - 2)), dir.path()) << "empty space is the root";
    tree->setCurrentIndex(model->index(path(QStringLiteral("Projects"))));
    EXPECT_TRUE(tree->dropFolderAt(onFolder).isEmpty()) << "a folder never drops into itself";
    tree->setCurrentIndex(model->index(path(QStringLiteral("Src/Target.md"))));

    // A drop with no drag of the tree's own behind it moves nothing.
    QMimeData foreign;
    foreign.setUrls({QUrl::fromLocalFile(path(QStringLiteral("Src/Target.md")))});
    QDropEvent stranger(QPointF(onFolder), Qt::MoveAction, &foreign, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(tree->viewport(), &stranger);
    QTest::qWait(50);
    EXPECT_TRUE(QFile::exists(path(QStringLiteral("Src/Target.md"))));

    // What a real drop emits goes through the window's own move.
    emit tree->moveRequested(path(QStringLiteral("Src/Target.md")), path(QStringLiteral("Projects")));
    WAIT_TRUE(QFile::exists(path(QStringLiteral("Projects/Target.md"))), 5000);
    EXPECT_FALSE(QFile::exists(path(QStringLiteral("Src/Target.md"))));
}

// The drag events the tree gets, sent to its viewport with the mime data its
// own startDrag would carry. (A real QDrag cannot run here: the offscreen
// platform ends a drag at once, so QDropEvent::source() is never the tree.)
TEST_F(WindowTest, TheTreesDragEventsAcceptOnlyItsOwnDragOntoARoomThatIsNotItself)
{
    ASSERT_TRUE(QDir(dir.path()).mkpath(QStringLiteral("Src/Sub")));
    write(QStringLiteral("Src/Target.md"), "# Target\n");

    MainWindow window;
    window.show();
    window.openVault(dir.path());
    auto *tree = window.findChild<noteAppC::FileTree *>(QStringLiteral("fileExplorer"));
    ASSERT_NE(tree, nullptr);
    auto *model = qobject_cast<QFileSystemModel *>(tree->model());
    WAIT_TRUE(tree->rootIndex().isValid() && model->rowCount(tree->rootIndex()) >= 4, 5000);
    tree->expand(model->index(path(QStringLiteral("Src"))));
    WAIT_TRUE(model->index(path(QStringLiteral("Src/Sub"))).isValid() && tree->visualRect(model->index(path(QStringLiteral("Src/Sub")))).isValid(), 5000);

    QSignalSpy moved(tree, &noteAppC::FileTree::moveRequested);
    QWidget *viewport = tree->viewport();
    const auto rowAt = [&](const QString &relative) {
        return QPointF(tree->visualRect(model->index(path(relative))).center());
    };
    const auto select = [&](const QString &relative) {
        const QModelIndex index = model->index(path(relative));
        tree->setCurrentIndex(index);
        tree->selectionModel()->select(index, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    };
    // Whether a drag over `where` is accepted, and, then dropped there, what it asks for.
    const auto over = [&](QMimeData *data, const QPointF &where) {
        QDragEnterEvent enter(where.toPoint(), Qt::MoveAction, data, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(viewport, &enter);
        QDragMoveEvent move(where.toPoint(), Qt::MoveAction, data, Qt::LeftButton, Qt::NoModifier);
        move.setAccepted(false);
        QApplication::sendEvent(viewport, &move);
        return enter.isAccepted() && move.isAccepted();
    };
    const auto drop = [&](QMimeData *data, const QPointF &where) {
        QDropEvent event(where, Qt::MoveAction, data, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(viewport, &event);
        return event.isAccepted();
    };

    // A folder cannot be dropped onto itself or into its own subfolder.
    select(QStringLiteral("Src"));
    std::unique_ptr<QMimeData> folderDrag(tree->dragData());
    ASSERT_NE(folderDrag, nullptr);
    EXPECT_FALSE(over(folderDrag.get(), rowAt(QStringLiteral("Src"))));
    EXPECT_FALSE(drop(folderDrag.get(), rowAt(QStringLiteral("Src"))));
    EXPECT_FALSE(over(folderDrag.get(), rowAt(QStringLiteral("Src/Sub"))));
    EXPECT_FALSE(drop(folderDrag.get(), rowAt(QStringLiteral("Src/Sub"))));
    EXPECT_EQ(moved.size(), 0);

    // The same URL list without the tree's marker is another app's drag.
    select(QStringLiteral("Src/Target.md"));
    std::unique_ptr<QMimeData> noteDrag(tree->dragData());
    ASSERT_NE(noteDrag, nullptr);
    QMimeData foreign;
    foreign.setUrls(noteDrag->urls());
    EXPECT_FALSE(over(&foreign, rowAt(QStringLiteral("Projects"))));
    EXPECT_FALSE(drop(&foreign, rowAt(QStringLiteral("Projects"))));
    EXPECT_EQ(moved.size(), 0);

    // Its own drag onto a folder row is accepted and asks for the move.
    EXPECT_TRUE(over(noteDrag.get(), rowAt(QStringLiteral("Projects"))));
    EXPECT_TRUE(drop(noteDrag.get(), rowAt(QStringLiteral("Projects"))));
    ASSERT_EQ(moved.size(), 1);
    EXPECT_EQ(moved.first().at(0).toString(), path(QStringLiteral("Src/Target.md")));
    EXPECT_EQ(moved.first().at(1).toString(), path(QStringLiteral("Projects")));
    WAIT_TRUE(QFile::exists(path(QStringLiteral("Projects/Target.md"))), 5000);
    EXPECT_FALSE(QFile::exists(path(QStringLiteral("Src/Target.md"))));
}

// The gesture that starts a drag: a press and a move on a row reach startDrag,
// whose exec ends at once offscreen. Nothing moves, and the file stays.
TEST_F(WindowTest, PressingAndDraggingARowStartsADragThatDeletesNothing)
{
    ASSERT_TRUE(QDir(dir.path()).mkpath(QStringLiteral("Src")));
    write(QStringLiteral("Src/Target.md"), "# Target\n");

    MainWindow window;
    window.show();
    window.openVault(dir.path());
    auto *tree = window.findChild<noteAppC::FileTree *>(QStringLiteral("fileExplorer"));
    ASSERT_NE(tree, nullptr);
    auto *model = qobject_cast<QFileSystemModel *>(tree->model());
    WAIT_TRUE(tree->rootIndex().isValid() && model->rowCount(tree->rootIndex()) >= 4, 5000);
    tree->expand(model->index(path(QStringLiteral("Src"))));
    WAIT_TRUE(model->index(path(QStringLiteral("Src/Target.md"))).isValid() && tree->visualRect(model->index(path(QStringLiteral("Src/Target.md")))).isValid(), 5000);

    QSignalSpy moved(tree, &noteAppC::FileTree::moveRequested);
    const QModelIndex row = model->index(path(QStringLiteral("Src/Target.md")));
    QTest::qWait(300); // the model sorts a loaded folder after it announces it
    const QPoint from = tree->visualRect(row).center();
    ASSERT_EQ(tree->indexAt(from), row);
    QTest::mousePress(tree->viewport(), Qt::LeftButton, Qt::NoModifier, from);
    QTest::mouseMove(tree->viewport(), from + QPoint(0, 12));
    // The QDrag startDrag made is still there until the event loop runs.
    const auto *drag = tree->findChild<QDrag *>();
    ASSERT_NE(drag, nullptr) << "the move did not start a drag";
    EXPECT_EQ(drag->mimeData()->urls(), QList<QUrl>({QUrl::fromLocalFile(path(QStringLiteral("Src/Target.md")))}));
    EXPECT_EQ(model->filePath(tree->currentIndex()), path(QStringLiteral("Src/Target.md")));
    QTest::mouseRelease(tree->viewport(), Qt::LeftButton, Qt::NoModifier, from + QPoint(0, 12));
    QTest::qWait(100);
    EXPECT_EQ(moved.size(), 0);
    EXPECT_TRUE(QFile::exists(path(QStringLiteral("Src/Target.md"))));
}

TEST_F(WindowTest, TheMoveCommandOffersVisibleFoldersAndMovesTheActiveNote)
{
    ASSERT_TRUE(QDir(dir.path()).mkpath(QStringLiteral(".hidden/Inner")));
    ASSERT_TRUE(QDir(dir.path()).mkpath(QStringLiteral("Projects/Sub")));
    MainWindow window;
    window.show();
    window.openVault(dir.path());
    window.openNote(path(QStringLiteral("Welcome.md")));
    auto *command = window.findChild<QAction *>(QStringLiteral("file-explorer:move-file"));
    ASSERT_NE(command, nullptr);
    command->trigger();
    auto *picker = window.findChild<QDialog *>(QStringLiteral("moveFolderPicker"));
    ASSERT_NE(picker, nullptr);
    auto *results = picker->findChild<QListWidget *>();
    ASSERT_NE(results, nullptr);
    QStringList shown;
    for (int i = 0; i < results->count(); ++i)
        shown << results->item(i)->text();
    EXPECT_TRUE(shown.contains(QStringLiteral("/")));
    EXPECT_TRUE(shown.contains(QStringLiteral("Projects/Sub")));
    EXPECT_FALSE(shown.join(QLatin1Char('\n')).contains(QStringLiteral(".hidden")));

    auto *query = picker->findChild<QLineEdit *>();
    QTest::keyClicks(query, QStringLiteral("Sub"));
    QTest::keyClick(query, Qt::Key_Return);
    WAIT_TRUE(QFile::exists(path(QStringLiteral("Projects/Sub/Welcome.md"))), 5000);
    EXPECT_FALSE(QFile::exists(path(QStringLiteral("Welcome.md"))));
}

namespace {

/// A second launch, as its own process would make it: its own SingleInstance,
/// asked on a thread because handOver() blocks and the window under test needs
/// this one for its event loop.
std::future<bool> secondLaunch(const QString &name, const QString &argument, const QByteArray &token = {})
{
    return std::async(std::launch::async, [=] {
        SingleInstance launch(name);
        return launch.handOver(argument, token);
    });
}

bool ready(std::future<bool> &result)
{
    return result.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready;
}

} // namespace

// A second launch (a note opened from the file manager) hands its path to the
// running window instead of starting another copy. The socket is the test's
// own, never the one a running Notolith listens on.
TEST_F(WindowTest, ASecondLaunchOpensItsNoteInTheRunningWindow)
{
    const QString name = dir.filePath(QStringLiteral("instance.sock"));
    MainWindow window;
    window.openTarget(LaunchTarget{}); // startup over: the window has been painted
    window.openVault(dir.path());
    window.show();
    ASSERT_TRUE(QTest::qWaitForWindowExposed(&window));
    SingleInstance instance(name);
    ASSERT_FALSE(instance.handOver(QString())); // nobody there: this one is first
    instance.serve(&window);
    ASSERT_TRUE(instance.ok());

    auto handed = secondLaunch(name, path(QStringLiteral("Projects/Crlf.md")));
    auto *tabs = window.findChild<QTabBar *>(QStringLiteral("noteTabs"));
    ASSERT_NE(tabs, nullptr);
    WAIT_TRUE(tabTexts(tabs).contains(QStringLiteral("Crlf")), 5000);
    WAIT_TRUE(ready(handed), 5000); // the answer needs this event loop
    EXPECT_TRUE(handed.get());
    EXPECT_EQ(tabs->count(), 1);

    // Same vault, another note: opened in the open window, the vault not reopened.
    handed = secondLaunch(name, path(QStringLiteral("Welcome.md")));
    WAIT_TRUE(tabTexts(tabs).contains(QStringLiteral("Welcome")), 5000);
    WAIT_TRUE(ready(handed), 5000); // the answer needs this event loop
    EXPECT_TRUE(handed.get());
    EXPECT_EQ(tabs->count(), 1); // replaces the tab, as clicking a link does

    // A note from outside the vault opens too, not the vault replaced by it.
    QTemporaryDir elsewhere;
    ASSERT_TRUE(elsewhere.isValid());
    QFile loose(elsewhere.filePath(QStringLiteral("Loose.md")));
    ASSERT_TRUE(loose.open(QIODevice::WriteOnly));
    loose.write("# Loose\n");
    loose.close();
    handed = secondLaunch(name, loose.fileName());
    WAIT_TRUE(tabTexts(tabs).contains(QStringLiteral("Loose")), 5000);
    WAIT_TRUE(ready(handed), 5000); // the answer needs this event loop
    EXPECT_TRUE(handed.get());
    EXPECT_FALSE(window.findChild<QWidget *>(QStringLiteral("sidebar"))->isHidden()); // still the vault

    // The launcher's activation token reaches the window, where Qt's Wayland
    // plugin picks it up for activateWindow(); focus itself needs a compositor.
    qunsetenv("XDG_ACTIVATION_TOKEN");
    handed = secondLaunch(name, QString(), "tok-123");
    WAIT_EQ(qgetenv("XDG_ACTIVATION_TOKEN"), QByteArray("tok-123"), 5000);
    WAIT_TRUE(ready(handed), 5000); // the answer needs this event loop
    EXPECT_TRUE(handed.get());
    qunsetenv("XDG_ACTIVATION_TOKEN");
    EXPECT_EQ(tabTexts(tabs).value(0), QStringLiteral("Loose")); // nothing to open: left as it was
}

// A hand-over that names another vault (a folder, or a note inside one) opens
// that vault in the running window; with nothing unsaved there is no prompt.
TEST_F(WindowTest, ASecondLaunchNamingAnotherVaultOpensThatVault)
{
    const QString name = dir.filePath(QStringLiteral("instance.sock"));
    QTemporaryDir other;
    ASSERT_TRUE(other.isValid());
    ASSERT_TRUE(QDir(other.path()).mkpath(QStringLiteral(".obsidian")));
    QFile note(other.filePath(QStringLiteral("Other.md")));
    ASSERT_TRUE(note.open(QIODevice::WriteOnly));
    note.write("# Other\n");
    note.close();

    MainWindow window;
    window.openTarget(LaunchTarget{}); // startup over: the window has been painted
    window.openVault(dir.path());
    window.show();
    ASSERT_TRUE(QTest::qWaitForWindowExposed(&window));
    SingleInstance instance(name);
    ASSERT_FALSE(instance.handOver(QString()));
    instance.serve(&window);
    ASSERT_TRUE(instance.ok());
    const auto explorerRoot = [&] {
        auto *tree = window.findChild<QTreeView *>(QStringLiteral("fileExplorer"));
        auto *model = qobject_cast<QFileSystemModel *>(tree->model());
        return model->filePath(tree->rootIndex());
    };
    ASSERT_EQ(explorerRoot(), dir.path());

    auto handed = secondLaunch(name, other.path());
    WAIT_EQ(explorerRoot(), other.path(), 5000);
    WAIT_TRUE(ready(handed), 5000); // the answer needs this event loop
    EXPECT_TRUE(handed.get());
    EXPECT_TRUE(window.windowTitle().contains(QDir(other.path()).dirName())) << window.windowTitle().toStdString();

    handed = secondLaunch(name, other.filePath(QStringLiteral("Other.md")));
    auto *tabs = window.findChild<QTabBar *>(QStringLiteral("noteTabs"));
    ASSERT_NE(tabs, nullptr);
    WAIT_TRUE(tabTexts(tabs).contains(QStringLiteral("Other")), 5000);
    WAIT_TRUE(ready(handed), 5000);
    EXPECT_TRUE(handed.get());
    EXPECT_EQ(explorerRoot(), other.path()) << "the same vault, not reopened or switched back";
}

// A note outside the vault opened by a hand-over is a tab like any other: the
// layout saved when the window closes brings it back when the vault reopens.
TEST_F(WindowTest, ANoteFromOutsideTheVaultHandedOverComesBackWithTheLayout)
{
    const QString name = dir.filePath(QStringLiteral("instance.sock"));
    QTemporaryDir elsewhere;
    ASSERT_TRUE(elsewhere.isValid());
    QFile loose(elsewhere.filePath(QStringLiteral("Loose.md")));
    ASSERT_TRUE(loose.open(QIODevice::WriteOnly));
    loose.write("# Loose\n");
    loose.close();
    {
        MainWindow window;
        window.openTarget(LaunchTarget{});
        window.openVault(dir.path());
        window.show();
        ASSERT_TRUE(QTest::qWaitForWindowExposed(&window));
        SingleInstance instance(name);
        ASSERT_FALSE(instance.handOver(QString()));
        instance.serve(&window);
        auto handed = secondLaunch(name, loose.fileName());
        auto *tabs = window.findChild<QTabBar *>(QStringLiteral("noteTabs"));
        ASSERT_NE(tabs, nullptr);
        WAIT_TRUE(tabTexts(tabs).contains(QStringLiteral("Loose")), 5000);
        WAIT_TRUE(ready(handed), 5000);
        EXPECT_TRUE(handed.get());
        window.close(); // saves the layout
    }

    MainWindow window;
    window.openVault(dir.path());
    auto *tabs = window.findChild<QTabBar *>(QStringLiteral("noteTabs"));
    ASSERT_NE(tabs, nullptr);
    EXPECT_TRUE(tabTexts(tabs).contains(QStringLiteral("Loose"))) << tabTexts(tabs).join(QLatin1Char(',')).toStdString();
    EXPECT_EQ(window.editor()->noteText(), QStringLiteral("# Loose\n"));
}

// Handed a path before its first paint, the window must not let the open it
// has queued for startup (the last vault) overwrite it - and two launches in
// the same moment, the second arriving while the first is still building its
// window, must end up as one window, not two.
TEST_F(WindowTest, ALaunchDuringAnotherOnesStartupIsHandedOnceItListens)
{
    const QString name = dir.filePath(QStringLiteral("early.sock"));
    SingleInstance first(name);
    ASSERT_FALSE(first.handOver(QString())); // first: holds the lock, not listening yet

    auto handed = secondLaunch(name, path(QStringLiteral("Welcome.md")));
    QElapsedTimer waited;
    waited.start();
    QTest::qWait(200); // it finds the lock held and waits rather than starting a window
    const bool early = ready(handed);
    // handOver gives up after 40 tries 50 ms apart (2 s): past that a starved
    // test thread proves nothing, so the check only counts when it ran in time.
    if (waited.elapsed() < 1500)
        EXPECT_FALSE(early) << "it must wait for the lock holder to listen";

    MainWindow window;
    first.serve(&window);
    window.openWhenShown(LaunchTarget{}); // what main() queues for a bare launch
    WAIT_TRUE(ready(handed), 5000);
    EXPECT_TRUE(handed.get());
    window.show();
    auto *tabs = window.findChild<QTabBar *>(QStringLiteral("noteTabs"));
    ASSERT_NE(tabs, nullptr);
    WAIT_TRUE(tabTexts(tabs).contains(QStringLiteral("Welcome")), 5000);
}

TEST_F(WindowTest, ASocketFileLeftByACrashDoesNotStopTheServer)
{
    const QString name = dir.filePath(QStringLiteral("stale.sock"));
    // Bound and abandoned, as a killed process leaves it.
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    ASSERT_GE(fd, 0);
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::snprintf(address.sun_path, sizeof address.sun_path, "%s", name.toLocal8Bit().constData());
    ASSERT_EQ(::bind(fd, reinterpret_cast<sockaddr *>(&address), sizeof address), 0);
    ::close(fd);
    ASSERT_TRUE(QFileInfo::exists(name));

    MainWindow window;
    SingleInstance instance(name);
    ASSERT_FALSE(instance.handOver(QString())); // nothing answers on a dead socket
    instance.serve(&window);
    EXPECT_TRUE(instance.ok());
}

TEST_F(WindowTest, FormattingColourRowsResetAndRecolourTheEditor)
{
    PrivateConfig config;
    write(QStringLiteral("Fmt.md"), "start\n\nSome **bold** and *ital* words.\n");
    Settings settings = Settings::load();
    settings.setValue(QStringLiteral("appearance.boldColorDark"), QStringLiteral("#ff0000"));
    settings.setValue(QStringLiteral("appearance.italicColorDark"), QStringLiteral("#00ff00"));
    ASSERT_TRUE(settings.save());

    MainWindow window;
    window.show(); // the settings page builds its rows when it is shown
    window.openVault(dir.path());
    window.openNote(path(QStringLiteral("Fmt.md")));
    MarkdownEditor *editor = window.editor();
    ASSERT_NE(editor, nullptr);
    const auto format = [editor](const QString &needle) {
        editor->grab(); // lays the visible blocks out
        return editor->formatAt(editor->noteText().indexOf(needle) + 1);
    };
    EXPECT_EQ(format(QStringLiteral("bold")).foreground().color(), QColor(0xff, 0, 0)) << "set in the settings file";

    window.openSettings();
    const auto swatch = [&window](const char *key) {
        QToolButton *found = nullptr;
        EXPECT_TRUE(QTest::qWaitFor([&] {
            return (found = settingsWidget<QToolButton>(window, QStringLiteral("settingsColour_") + QLatin1String(key)));
        }));
        return found;
    };
    QToolButton *bold = swatch("appearance.boldColorDark");
    ASSERT_NE(bold, nullptr);
    EXPECT_NE(swatch("appearance.boldColorLight"), nullptr) << "one swatch per mode";
    // Its row's own Reset, the button a person clicks.
    auto *reset = bold->parentWidget()->parentWidget()->findChild<QToolButton *>(QStringLiteral("settingsResetButton"));
    ASSERT_NE(reset, nullptr);
    reset->click();
    EXPECT_FALSE(format(QStringLiteral("bold")).hasProperty(QTextFormat::ForegroundBrush)) << "reset follows the theme again";
    EXPECT_EQ(format(QStringLiteral("ital")).foreground().color(), QColor(0, 0xff, 0)) << "the other row stays";

    auto *all = settingsWidget<QToolButton>(window, QStringLiteral("settingsResetAllColours"));
    ASSERT_NE(all, nullptr);
    all->click();
    EXPECT_FALSE(format(QStringLiteral("ital")).hasProperty(QTextFormat::ForegroundBrush));
    window.closeSettings();
    EXPECT_EQ(editor->noteText(), QStringLiteral("start\n\nSome **bold** and *ital* words.\n"));
}

// Loadable modules through the window (spec sections 4 and 6, seam 1): the
// fake module from tests/fakemodule, found in NOTEAPPC_MODULE_DIRS.
TEST_F(WindowTest, AModuleFileOpensInTheModuleViewOnlyWhileItsFeatureIsOn)
{
    write(QStringLiteral("sketch.fake"), "anything");
    MainWindow window;
    window.show();
    window.openVault(dir.path());
    window.scanModulesNow();
    ASSERT_FALSE(window.modules()->modules().isEmpty()) << "NOTEAPPC_MODULE_DIRS does not point at the fake modules";
    EXPECT_FALSE(window.modules()->isLoaded(QStringLiteral("fake"))) << "a scan must not load";
    ASSERT_TRUE(window.openNote(path(QStringLiteral("sketch.fake"))));
    EXPECT_TRUE(window.modules()->isLoaded(QStringLiteral("fake")));
    QWidget *view = window.findChild<QWidget *>(QStringLiteral("fakeView"));
    ASSERT_NE(view, nullptr);
    EXPECT_TRUE(QTest::qWaitFor([&] { return view->isVisible(); }, 3000)); // a widget added to a shown layout is shown from the event loop
    EXPECT_NE(qobject_cast<noteAppC::ModuleView *>(view), nullptr) << "one ModuleView metaobject across the boundary";

    // The feature row exists because the module is installed, and is on.
    command(window, "app:open-settings")->trigger();
    noteAppC::ToggleSwitch *toggle = nullptr;
    ASSERT_TRUE(QTest::qWaitFor([&] {
        toggle = settingsWidget<noteAppC::ToggleSwitch>(window, QStringLiteral("settingsToggle_plugins.fake"));
        return toggle != nullptr;
    }, 3000));
    EXPECT_TRUE(toggle->isChecked());
    toggle->setChecked(false);
    // Off: the suffix is unregistered, the command greyed, the library stays loaded.
    command(window, "workspace:new-tab")->trigger();
    EXPECT_FALSE(window.openNote(path(QStringLiteral("sketch.fake"))));
    EXPECT_FALSE(command(window, "fake:hello")->isEnabled());
    EXPECT_TRUE(window.modules()->isLoaded(QStringLiteral("fake")));
}

TEST_F(WindowTest, AModuleCommandLoadsTheModuleOnFirstUse)
{
    MainWindow window;
    window.show();
    window.openVault(dir.path());
    window.scanModulesNow();
    QAction *hello = command(window, "fake:hello");
    ASSERT_NE(hello, nullptr);
    EXPECT_FALSE(window.modules()->isLoaded(QStringLiteral("fake")));
    hello->trigger();
    EXPECT_TRUE(window.modules()->isLoaded(QStringLiteral("fake")));
    EXPECT_EQ(window.statusBar()->currentMessage(), QStringLiteral("hello from the fake module"));
}

TEST_F(WindowTest, ModulesWithAProblemAreListedInSettingsAndNeverLoaded)
{
    MainWindow window;
    window.show();
    window.openVault(dir.path());
    window.scanModulesNow();
    command(window, "app:open-settings")->trigger();
    QLabel *notice = nullptr;
    ASSERT_TRUE(QTest::qWaitFor([&] {
        notice = settingsWidget<QLabel>(window, QStringLiteral("moduleProblem_fake-badabi")); // built when its page is shown
        return notice != nullptr;
    }, 3000));
    EXPECT_TRUE(notice->text().contains(QStringLiteral("another Notolith")));
    auto *twin = settingsWidget<QLabel>(window, QStringLiteral("moduleProblem_fake-twin"));
    ASSERT_NE(twin, nullptr);
    EXPECT_TRUE(twin->text().contains(QStringLiteral("already claims")));
    auto *noID = settingsWidget<QLabel>(window, QStringLiteral("moduleProblem_fake_noid"));
    ASSERT_NE(noID, nullptr);
    EXPECT_TRUE(noID->text().contains(QStringLiteral("manifest has no id")));
    EXPECT_EQ(settingsWidget<noteAppC::ToggleSwitch>(window, QStringLiteral("settingsToggle_plugins.fakeBadabi")), nullptr)
        << "a module that cannot load gets a notice, never a switch";
    EXPECT_FALSE(window.modules()->isLoaded(QStringLiteral("fake-badabi")));
    EXPECT_FALSE(window.modules()->isLoaded(QStringLiteral("fake-twin")));
}

TEST_F(WindowTest, AModuleSettingRowWithDecimalsIsADoubleSpinBox)
{
    MainWindow window;
    window.show();
    window.openVault(dir.path());
    window.scanModulesNow();
    command(window, "fake:hello")->trigger(); // loading the module registers its rows
    command(window, "app:open-settings")->trigger();
    QDoubleSpinBox *spin = nullptr;
    WAIT_TRUE((spin = settingsWidget<QDoubleSpinBox>(window, QStringLiteral("settingsSpin_fake.width"))) != nullptr, 3000);
    EXPECT_EQ(spin->decimals(), 1);
    EXPECT_DOUBLE_EQ(spin->singleStep(), 0.5);
    EXPECT_DOUBLE_EQ(spin->minimum(), 0.5);
    EXPECT_DOUBLE_EQ(spin->maximum(), 40.0);
    EXPECT_DOUBLE_EQ(spin->value(), 2.0);
    spin->setValue(6.5);
    EXPECT_DOUBLE_EQ(window.settings().value(QStringLiteral("fake.width")).toDouble(), 6.5);
}

TEST_F(WindowTest, AModuleAsksTheWindowForTheCurrentFolder)
{
    MainWindow window;
    window.show();
    window.openVault(dir.path());
    window.scanModulesNow();
    ASSERT_TRUE(window.openNote(path(QStringLiteral("Projects/Crlf.md"))));
    command(window, "fake:where")->trigger();
    EXPECT_EQ(QFileInfo(window.statusBar()->currentMessage()).canonicalFilePath(),
              QFileInfo(path(QStringLiteral("Projects"))).canonicalFilePath());
    command(window, "workspace:new-tab")->trigger(); // no note in the active tab
    command(window, "fake:where")->trigger();
    EXPECT_EQ(QFileInfo(window.statusBar()->currentMessage()).canonicalFilePath(), QFileInfo(dir.path()).canonicalFilePath())
        << "no note: the vault root";
}

// R1: indexing never counts as use. A vault file with a module's suffix leaves the module
// unloaded until the file is opened. (No fixed wait: the index pass is awaited, then the
// absence is asserted.)
TEST_F(WindowTest, AVaultFileOfAModuleSuffixDoesNotLoadTheModuleUntilItIsOpened)
{
    write(QStringLiteral("notes.fake"), "anything");
    MainWindow window;
    window.show();
    window.openVault(dir.path());
    window.scanModulesNow();
    WAIT_TRUE(window.index()->attachmentCount() >= 1, 5000); // the first index pass has landed
    EXPECT_FALSE(window.modules()->isLoaded(QStringLiteral("fake")));
    ASSERT_TRUE(window.openNote(path(QStringLiteral("notes.fake"))));
    EXPECT_TRUE(window.modules()->isLoaded(QStringLiteral("fake")));
}

// M4: a command the scan adds reaches a pop-out window that already exists. The main window
// is not shown, so no first paint starts the automatic scan before the explicit one.
TEST_F(WindowTest, AModuleCommandAppearsInAnAlreadyOpenPopoutWindow)
{
    MainWindow window;
    window.openVault(dir.path());
    window.openNote(path(QStringLiteral("Welcome.md")));
    command(window, "workspace:open-in-new-window")->trigger();
    const auto popouts = popoutWindows();
    ASSERT_EQ(popouts.size(), 1);
    ASSERT_EQ(command(window, "fake:hello"), nullptr) << "the scan has not run yet";
    window.scanModulesNow();
    QAction *hello = command(window, "fake:hello");
    ASSERT_NE(hello, nullptr);
    EXPECT_TRUE(popouts.first()->actions().contains(hello));
}

// M5: a stored value of a module's setting key is adopted once the module has loaded (the
// file was read when the window was made, before the key existed). Its own ctest entry
// (`window_modules_adopt`): in the full `window` process an earlier test has already registered
// fake.width, so the stored value would be read at load() and adoptStored() would not be needed.
TEST_F(WindowTest, AStoredValueOfAModuleSettingIsAdoptedWhenTheModuleLoads)
{
    // Its own ctest entry (window_modules_adopt sets this): it needs fake.width unregistered when the window is
    // made, which an earlier test in the same process spoils. The `window` entry skips it, so that entry
    // carries no gtest filter of its own and GTEST_FILTER can focus it.
    if (!qEnvironmentVariableIsSet("NOTEAPPC_WINDOW_ADOPT_ENTRY"))
        GTEST_SKIP() << "run by the window_modules_adopt entry";
    const QString configPath = noteAppC::Settings::configFile();
    auto cleanup = qScopeGuard([configPath] { QFile::remove(configPath); }); // declared first: destroyed after the window
    QDir().mkpath(QFileInfo(configPath).absolutePath());
    QFile config(configPath);
    ASSERT_TRUE(config.open(QIODevice::WriteOnly | QIODevice::Truncate));
    config.write("[fake]\nwidth = 7.5\n");
    config.close();
    MainWindow window;
    window.show();
    window.openVault(dir.path());
    window.scanModulesNow();
    command(window, "fake:hello")->trigger();
    EXPECT_DOUBLE_EQ(window.settings().value(QStringLiteral("fake.width")).toDouble(), 7.5);
}

// M1: a module's setting row is saved like any other: change it in the page, close the
// window, read the file back.
TEST_F(WindowTest, AModuleSettingChangedInSettingsIsSavedWhenTheWindowCloses)
{
    const QString configPath = noteAppC::Settings::configFile();
    auto cleanup = qScopeGuard([configPath] { QFile::remove(configPath); });
    {
        MainWindow window;
        window.show();
        window.openVault(dir.path());
        window.scanModulesNow();
        command(window, "fake:hello")->trigger(); // loading the module registers its rows
        command(window, "app:open-settings")->trigger();
        noteAppC::ToggleSwitch *toggle = nullptr;
        WAIT_TRUE((toggle = settingsWidget<noteAppC::ToggleSwitch>(window, QStringLiteral("settingsToggle_fake.greeting"))) != nullptr, 3000);
        ASSERT_TRUE(toggle->isChecked());
        toggle->setChecked(false);
        window.close();
    }
    EXPECT_FALSE(noteAppC::Settings::load().boolean(QStringLiteral("fake.greeting")));
}

// R2: no dependency check; a library that cannot resolve a symbol fails to load and the
// loader's message is what Settings shows.
TEST_F(WindowTest, AModuleThatFailsToLoadIsReportedInSettingsWithTheLoadersMessage)
{
    MainWindow window;
    window.show();
    window.openVault(dir.path());
    window.scanModulesNow();
    QAction *hello = command(window, "fake-unresolved:hello");
    ASSERT_NE(hello, nullptr);
    hello->trigger();
    EXPECT_FALSE(window.modules()->isLoaded(QStringLiteral("fake-unresolved")));
    EXPECT_TRUE(window.statusBar()->currentMessage().contains(QStringLiteral("noteappcFakeMissingSymbol")));
    command(window, "app:open-settings")->trigger();
    QLabel *notice = nullptr;
    WAIT_TRUE((notice = settingsWidget<QLabel>(window, QStringLiteral("moduleProblem_fake-unresolved"))) != nullptr, 3000);
    EXPECT_TRUE(notice->text().contains(QStringLiteral("noteappcFakeMissingSymbol")));
}

// Answers a modal box with `answer`, or closes it when it has no such button (a different box than
// the one the test expects): the test then fails on its text assertion, not on a null deref that
// would kill the whole window process.
static void closeBoxWith(QMessageBox *box, QMessageBox::StandardButton answer)
{
    if (QAbstractButton *button = box->button(answer))
        button->click();
    else
        box->close();
}

#ifdef NOTEAPPC_HAVE_INK_MODULE // set when the build has notolith-ink; with NOTEAPPC_WITH_INK=OFF these tests do not exist
// The handwritten-note module (spec section 4, Task 8): the real notolith-ink.so from
// ${CMAKE_BINARY_DIR}/modules, found through NOTEAPPC_MODULE_DIRS.
TEST_F(WindowTest, AnInkFileOpensInTheInkViewAndSavesThroughTheWindow)
{
    write(QStringLiteral("Sketch.ink"),
          "{\"format\":\"notolith-ink\",\"version\":1,\"document\":{},\"items\":[\n"
          "{\"type\":\"stroke\",\"t\":1,\"layer\":\"user\",\"style\":{\"width\":2,\"color\":\"#1e1e1e\",\"pressureCurve\":\"linear\"},"
          "\"points\":[10,10,0.5,50,50,0.5]}\n]}\n");
    MainWindow window;
    window.openVault(dir.path());
    window.show();
    ASSERT_TRUE(QTest::qWaitForWindowExposed(&window));
    window.scanModulesNow();
    ASSERT_TRUE(window.modules()) << "the module registry exists after the scan";
    ASSERT_TRUE(window.modules()->moduleForSuffix(path(QStringLiteral("Sketch.ink")))) << "notolith-ink was found in CMAKE_BINARY_DIR/modules";
    ASSERT_TRUE(window.openNote(path(QStringLiteral("Sketch.ink"))));
    QWidget *view = nullptr;
    WAIT_TRUE((view = window.findChild<QWidget *>(QStringLiteral("inkView"))) != nullptr, 5000);
    auto *moduleView = qobject_cast<noteAppC::ModuleView *>(view);
    ASSERT_NE(moduleView, nullptr);
    EXPECT_TRUE(window.modules()->isLoaded(QStringLiteral("ink")));
    auto *tabs = window.findChild<QTabBar *>(QStringLiteral("noteTabs"));
    ASSERT_NE(tabs, nullptr);
    EXPECT_EQ(tabs->tabText(tabs->currentIndex()), QStringLiteral("Sketch"));
    auto *canvas = view->findChild<QWidget *>(QStringLiteral("inkCanvas"));
    ASSERT_TRUE(canvas);
    EXPECT_GT(canvas->height(), 1000) << "an A4 page at zoom 1";
    // Draw through the module view's canvas with a synthetic pen, then save with the window's command.
    using inktest::tablet;
    tablet(canvas, QEvent::TabletPress, QPointF(100, 100), 0.5, 1000);
    tablet(canvas, QEvent::TabletMove, QPointF(150, 120), 0.5, 1005);
    tablet(canvas, QEvent::TabletMove, QPointF(200, 140), 0.5, 1010);
    tablet(canvas, QEvent::TabletRelease, QPointF(200, 140), 0.5, 1015, Qt::NoButton);
    WAIT_TRUE(moduleView->isModified(), 5000);
    EXPECT_TRUE(tabs->tabText(tabs->currentIndex()).contains(QStringLiteral("Sketch"))) << "the modified marker keeps the name";
    command(window, "editor:save-file")->trigger();
    WAIT_TRUE(!moduleView->isModified(), 5000);
    QFile saved(path(QStringLiteral("Sketch.ink")));
    ASSERT_TRUE(saved.open(QIODevice::ReadOnly));
    const QByteArray bytes = saved.readAll();
    EXPECT_EQ(bytes.count("\"type\":\"stroke\""), 2) << "the old stroke and the new one";
    EXPECT_TRUE(bytes.contains("\"format\":\"notolith-ink\"")) << "written by writeInk, compact JSON (R4)";
}

// Task 11: a file above kOffThreadParseBytes (256 KB) is parsed on a pool thread; the tab says so meanwhile and
// refuses to save, so a half-loaded document is never written over the file.
TEST_F(WindowTest, ABigInkFileParsesOffTheUIThreadAndSavesAfter)
{
    constexpr int kStrokes = 3000;
    QByteArray body = "{\"format\":\"notolith-ink\",\"version\":1,\"document\":{},\"items\":[\n";
    for (int i = 0; i < kStrokes; ++i) {
        const int x = 10 + (i % 60) * 12, y = 10 + (i / 60) * 20;
        body += QByteArray("{\"type\":\"stroke\",\"t\":") + QByteArray::number(i + 1)
            + ",\"layer\":\"user\",\"style\":{\"width\":2,\"color\":\"#1e1e1e\",\"pressureCurve\":\"linear\"},\"points\":["
            + QByteArray::number(x) + ',' + QByteArray::number(y) + ",0.5," + QByteArray::number(x + 4) + ',' + QByteArray::number(y + 3)
            + ",0.5," + QByteArray::number(x + 8) + ',' + QByteArray::number(y + 1) + ",0.5," + QByteArray::number(x + 11) + ','
            + QByteArray::number(y + 6) + ",0.5," + QByteArray::number(x + 12) + ',' + QByteArray::number(y + 9) + ",0.5]}"
            + (i + 1 < kStrokes ? ",\n" : "\n");
    }
    body += "]}\n";
    write(QStringLiteral("Big.ink"), body);
    ASSERT_GT(QFileInfo(path(QStringLiteral("Big.ink"))).size(), 256 * 1024) << "above kOffThreadParseBytes";
    MainWindow window;
    window.openVault(dir.path());
    window.show();
    ASSERT_TRUE(QTest::qWaitForWindowExposed(&window));
    window.scanModulesNow();
    ASSERT_TRUE(window.openNote(path(QStringLiteral("Big.ink"))));
    auto *view = window.findChild<noteAppC::ModuleView *>(QStringLiteral("inkView"));
    ASSERT_NE(view, nullptr);
    auto *loading = view->findChild<QLabel *>(QStringLiteral("inkLoadingLabel"));
    ASSERT_NE(loading, nullptr);
    EXPECT_TRUE(view->property("loading").toBool()) << "load() returned at once, the parse is still running";
    auto *loadingAction = view->findChild<QAction *>(QStringLiteral("inkLoadingAction")); // the toolbar shows its widgets a layout pass later
    ASSERT_NE(loadingAction, nullptr);
    EXPECT_TRUE(loadingAction->isVisible()) << "the toolbar says Loading...";
    EXPECT_TRUE(loading->text().contains(QStringLiteral("Loading")));
    QString error;
    EXPECT_FALSE(view->save(&error)) << "a half-loaded document is never written over the file";
    EXPECT_TRUE(error.contains(QStringLiteral("still loading"))) << error.toStdString();
    WAIT_TRUE(!view->property("loading").toBool(), 5000);
    EXPECT_FALSE(loadingAction->isVisible());
    EXPECT_FALSE(view->isModified()) << "a load is not an edit";
    error.clear();
    EXPECT_TRUE(view->save(&error)) << error.toStdString();
    QFile saved(path(QStringLiteral("Big.ink")));
    ASSERT_TRUE(saved.open(QIODevice::ReadOnly));
    EXPECT_EQ(saved.readAll().count("\"type\":\"stroke\""), kStrokes) << "every stroke came through the parse and the save";
}

TEST_F(WindowTest, WithInkOffAnInkFileIsNotAModuleFile)
{
    write(QStringLiteral("Sketch.ink"), "{\"format\":\"notolith-ink\",\"version\":1,\"document\":{},\"items\":[]}\n");
    MainWindow window;
    window.openVault(dir.path());
    window.show();
    ASSERT_TRUE(QTest::qWaitForWindowExposed(&window));
    window.scanModulesNow();
    ASSERT_TRUE(window.modules()->moduleForSuffix(path(QStringLiteral("Sketch.ink"))));
    command(window, "app:open-settings")->trigger(); // SettingsPage builds on showEvent
    noteAppC::ToggleSwitch *toggle = nullptr;
    ASSERT_TRUE(QTest::qWaitFor([&] {
        toggle = settingsWidget<noteAppC::ToggleSwitch>(window, QStringLiteral("settingsToggle_plugins.ink"));
        return toggle != nullptr;
    }, 3000)) << "the module's feature row is in Settings > Plugins";
    toggle->setChecked(false);
    WAIT_TRUE(!window.settings().boolean(Settings::featureKey("ink")), 5000);
    EXPECT_FALSE(window.modules()->moduleForSuffix(path(QStringLiteral("Sketch.ink"))));
    command(window, "workspace:new-tab")->trigger();
    EXPECT_FALSE(window.openNote(path(QStringLiteral("Sketch.ink")))) << "with the feature off a .ink is not a view file";
    EXPECT_FALSE(window.modules()->isLoaded(QStringLiteral("ink"))) << "never loaded: nothing in this test used it";
}

TEST_F(WindowTest, NewHandwrittenNoteIsCreatedBesideTheActiveNoteElseInTheVaultRoot)
{
    ASSERT_TRUE(QDir(dir.path()).mkpath(QStringLiteral("Sub")));
    write(QStringLiteral("Sub/Anchor.md"), "# Anchor\n");
    MainWindow window;
    window.openVault(dir.path());
    window.show();
    ASSERT_TRUE(QTest::qWaitForWindowExposed(&window));
    window.scanModulesNow();
    ASSERT_TRUE(window.openNote(path(QStringLiteral("Sub/Anchor.md"))));
    ASSERT_NE(command(window, "ink:new"), nullptr) << "the manifest's command is in the palette before the module loads";
    command(window, "ink:new")->trigger();
    const QString beside = path(QStringLiteral("Sub/Handwritten note.ink"));
    WAIT_TRUE(QFileInfo::exists(beside), 5000);
    EXPECT_FALSE(QFileInfo::exists(path(QStringLiteral("Handwritten note.ink")))) << "not the vault root while a note in Sub is active";
    QFile created(beside);
    ASSERT_TRUE(created.open(QIODevice::ReadOnly));
    EXPECT_TRUE(created.readAll().contains("\"format\":\"notolith-ink\"")) << "written through writeInk";

    command(window, "workspace:new-tab")->trigger(); // an empty tab: no active note
    command(window, "ink:new")->trigger();
    WAIT_TRUE(QFileInfo::exists(path(QStringLiteral("Handwritten note.ink"))), 5000);
}

// A file written by a newer Notolith opens read-only: the pen does nothing, the save command
// refuses (writeInk stamps version 1 and would downgrade the file), the bytes stay as they were.
TEST_F(WindowTest, ANewerInkFileOpensReadOnlyAndIsNeverWrittenBack)
{
    const QByteArray newer = "{\"format\":\"notolith-ink\",\"version\":2,\"document\":{},\"items\":[],\"future\":{\"keep\":true}}\n";
    write(QStringLiteral("Future.ink"), newer);
    MainWindow window;
    window.openVault(dir.path());
    window.show();
    ASSERT_TRUE(QTest::qWaitForWindowExposed(&window));
    window.scanModulesNow();
    ASSERT_TRUE(window.openNote(path(QStringLiteral("Future.ink"))));
    auto *view = window.findChild<noteAppC::ModuleView *>(QStringLiteral("inkView"));
    ASSERT_NE(view, nullptr);
    EXPECT_TRUE(window.statusBar()->currentMessage().contains(QStringLiteral("read-only")))
        << "the open says so: " << window.statusBar()->currentMessage().toStdString();
    EXPECT_TRUE(view->property("readOnly").toBool());
    auto *canvas = view->findChild<QWidget *>(QStringLiteral("inkCanvas"));
    ASSERT_NE(canvas, nullptr);
    using inktest::penStroke;
    penStroke(canvas, QPointF(100, 100), QPointF(200, 200), 1000);
    EXPECT_FALSE(view->isModified()) << "a read-only canvas ignores the pen";
    QString error;
    EXPECT_FALSE(view->save(&error)) << "saving a newer file would downgrade it";
    EXPECT_TRUE(error.contains(QStringLiteral("read-only"))) << error.toStdString();
    QString warning;
    QTimer watch; // the save command reports a refusal in a box: read it and close it
    QObject::connect(&watch, &QTimer::timeout, [&warning] {
        if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
            warning = box->text();
            closeBoxWith(box, QMessageBox::Ok);
        }
    });
    watch.start(20);
    command(window, "editor:save-file")->trigger();
    EXPECT_TRUE(warning.contains(QStringLiteral("read-only"))) << "the person is told: " << warning.toStdString();
    QFile file(path(QStringLiteral("Future.ink")));
    ASSERT_TRUE(file.open(QIODevice::ReadOnly));
    EXPECT_EQ(file.readAll(), newer) << "the file on disk is untouched";
}

// A file that is not an ink file is refused by the view, with the reader's message, and no tab opens.
TEST_F(WindowTest, AnInkFileThatCannotBeReadIsReportedAndNoViewOpens)
{
    write(QStringLiteral("Broken.ink"), "{not json");
    MainWindow window;
    window.openVault(dir.path());
    window.show();
    window.scanModulesNow();
    QString warning;
    QTimer watch; // the open refusal is a box: read it and close it
    QObject::connect(&watch, &QTimer::timeout, [&warning] {
        if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
            warning = box->text();
            closeBoxWith(box, QMessageBox::Ok);
        }
    });
    watch.start(20);
    EXPECT_FALSE(window.openNote(path(QStringLiteral("Broken.ink"))));
    EXPECT_TRUE(warning.contains(QStringLiteral("Broken.ink could not be read"))) << "the reader's message: " << warning.toStdString();
    EXPECT_EQ(window.findChild<QWidget *>(QStringLiteral("inkView")), nullptr);
}
#endif

// R6: a file-less module tab shows the modified mark, the save command calls its save(), and
// closing it saves it like a note (autosave is on by default) without asking.
TEST_F(WindowTest, AFileLessModuleTabShowsTheModifiedMarkAndIsSavedByTheSaveCommandAndOnClose)
{
    QTemporaryDir marks;
    const QString saves = marks.filePath(QStringLiteral("saves"));
    qputenv("NOTEAPPC_FAKE_TAB_SAVES", saves.toLocal8Bit());
    auto unset = qScopeGuard([] { qunsetenv("NOTEAPPC_FAKE_TAB_SAVES"); });
    const auto savedCount = [&saves] {
        QFile file(saves);
        return file.open(QIODevice::ReadOnly) ? int(file.readAll().count("saved")) : 0;
    };
    MainWindow window;
    window.show();
    window.openVault(dir.path());
    window.scanModulesNow();
    ASSERT_TRUE(window.settings().boolean(QStringLiteral("files.autosave"))) << "the default: closing saves instead of asking";
    command(window, "fake:tab")->trigger();
    auto *view = window.findChild<noteAppC::ModuleView *>(QStringLiteral("fakeTab"));
    ASSERT_NE(view, nullptr);
    ASSERT_FALSE(mainBars(window).isEmpty());
    QTabBar *tabs = mainBars(window).first();
    EXPECT_EQ(tabs->tabText(tabs->currentIndex()), QStringLiteral("Fake tab"));

    view->setProperty("modified", true);
    emit view->modificationChanged(true);
    EXPECT_EQ(tabs->tabText(tabs->currentIndex()), QStringLiteral("Fake tab *")) << "the asterisk";
    command(window, "editor:save-file")->trigger();
    EXPECT_EQ(savedCount(), 1) << "the save command reaches the view's save()";
    EXPECT_EQ(tabs->tabText(tabs->currentIndex()), QStringLiteral("Fake tab"));

    view->setProperty("modified", true);
    emit view->modificationChanged(true);
    bool asked = false;
    QTimer watch;
    QObject::connect(&watch, &QTimer::timeout, [&asked] {
        if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
            asked = true;
            closeBoxWith(box, QMessageBox::Cancel);
        }
    });
    watch.start(20);
    command(window, "workspace:close")->trigger();
    EXPECT_FALSE(asked) << "autosave on: leaving saves, nothing asks";
    EXPECT_EQ(savedCount(), 2) << "closing the modified tab saved it";
}

TEST_F(WindowTest, AFileLessModuleTabAsksBeforeClosingWhenAutosaveIsOffAndSavesOnSave)
{
    QTemporaryDir config;
    const QByteArray previous = qgetenv("XDG_CONFIG_HOME");
    qputenv("XDG_CONFIG_HOME", config.path().toUtf8());
    auto restore = qScopeGuard([&previous] { qputenv("XDG_CONFIG_HOME", previous); });
    Settings off = Settings::load();
    off.setValue(QStringLiteral("files.autosave"), false);
    ASSERT_TRUE(off.save());
    QTemporaryDir marks;
    const QString saves = marks.filePath(QStringLiteral("saves"));
    qputenv("NOTEAPPC_FAKE_TAB_SAVES", saves.toLocal8Bit());
    auto unset = qScopeGuard([] { qunsetenv("NOTEAPPC_FAKE_TAB_SAVES"); });
    MainWindow window;
    window.show();
    window.openVault(dir.path());
    window.scanModulesNow();
    command(window, "fake:tab")->trigger();
    auto *view = window.findChild<noteAppC::ModuleView *>(QStringLiteral("fakeTab"));
    ASSERT_NE(view, nullptr);
    view->setProperty("modified", true);
    emit view->modificationChanged(true);
    QString question;
    QTimer watch;
    QObject::connect(&watch, &QTimer::timeout, [&question] {
        if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
            question = box->text();
            closeBoxWith(box, QMessageBox::Save);
        }
    });
    watch.start(20);
    command(window, "workspace:close")->trigger();
    EXPECT_TRUE(question.contains(QStringLiteral("Fake tab"))) << "asked, naming the tab: " << question.toStdString();
    QFile file(saves);
    ASSERT_TRUE(file.open(QIODevice::ReadOnly));
    EXPECT_EQ(file.readAll().count("saved"), 1) << "Save called the view's save()";
}

// R6, quit: two modified file-less tabs are two documents (maybeSaveAll keys them apart): quitting asks
// about each, and Save reaches each view's save().
TEST_F(WindowTest, QuittingAsksAboutEveryModifiedFileLessModuleTabAndSavesEach)
{
    QTemporaryDir config;
    const QByteArray previous = qgetenv("XDG_CONFIG_HOME");
    qputenv("XDG_CONFIG_HOME", config.path().toUtf8());
    auto restore = qScopeGuard([&previous] { qputenv("XDG_CONFIG_HOME", previous); });
    Settings off = Settings::load();
    off.setValue(QStringLiteral("files.autosave"), false);
    ASSERT_TRUE(off.save());
    QTemporaryDir marks;
    const QString saves = marks.filePath(QStringLiteral("saves"));
    qputenv("NOTEAPPC_FAKE_TAB_SAVES", saves.toLocal8Bit());
    auto unset = qScopeGuard([] { qunsetenv("NOTEAPPC_FAKE_TAB_SAVES"); });
    MainWindow window;
    window.show();
    window.openVault(dir.path());
    window.scanModulesNow();
    command(window, "fake:tab")->trigger();
    command(window, "fake:tab")->trigger();
    const auto views = window.findChildren<noteAppC::ModuleView *>(QStringLiteral("fakeTab"));
    ASSERT_EQ(views.size(), 2);
    for (noteAppC::ModuleView *view : views) {
        view->setProperty("modified", true);
        emit view->modificationChanged(true);
    }
    int questions = 0;
    QTimer watch;
    QObject::connect(&watch, &QTimer::timeout, [&questions] {
        if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
            ++questions;
            closeBoxWith(box, QMessageBox::Save);
        }
    });
    watch.start(20);
    window.close();
    EXPECT_EQ(questions, 2) << "one question per tab, not one for both";
    QFile file(saves);
    ASSERT_TRUE(file.open(QIODevice::ReadOnly));
    EXPECT_EQ(file.readAll().count("saved"), 2) << "Save called each view's save()";
}

#ifdef NOTEAPPC_HAVE_INK_MODULE
// Jernej's decision: the mouse draws only when inking with it is explicitly on (a pen always draws).
// The row is registered by the module and reaches an already open view through applySettings.
TEST_F(WindowTest, TheMouseDrawsInAnInkTabOnlyAfterDrawWithMouseIsSwitchedOnInSettings)
{
    write(QStringLiteral("Sketch.ink"), "{\"format\":\"notolith-ink\",\"version\":1,\"document\":{},\"items\":[]}\n");
    MainWindow window;
    window.openVault(dir.path());
    window.show();
    ASSERT_TRUE(QTest::qWaitForWindowExposed(&window));
    window.scanModulesNow();
    ASSERT_TRUE(window.openNote(path(QStringLiteral("Sketch.ink"))));
    auto *view = window.findChild<noteAppC::ModuleView *>(QStringLiteral("inkView"));
    ASSERT_NE(view, nullptr);
    auto *canvas = view->findChild<QWidget *>(QStringLiteral("inkCanvas"));
    ASSERT_NE(canvas, nullptr);
    const auto drag = [canvas] {
        QTest::mousePress(canvas, Qt::LeftButton, Qt::NoModifier, QPoint(100, 100));
        QTest::mouseMove(canvas, QPoint(150, 120));
        QTest::mouseMove(canvas, QPoint(200, 140));
        QTest::mouseRelease(canvas, Qt::LeftButton, Qt::NoModifier, QPoint(200, 140));
    };
    drag();
    EXPECT_FALSE(view->isModified()) << "off by default: the mouse draws nothing";

    command(window, "app:open-settings")->trigger();
    noteAppC::ToggleSwitch *toggle = nullptr;
    WAIT_TRUE((toggle = settingsWidget<noteAppC::ToggleSwitch>(window, QStringLiteral("settingsToggle_ink.drawWithMouse"))) != nullptr, 3000);
    EXPECT_FALSE(toggle->isChecked());
    toggle->setChecked(true);
    window.closeSettings();
    WAIT_TRUE(canvas->isVisible(), 3000);
    drag();
    EXPECT_TRUE(view->isModified()) << "on: the live view took the setting without being reopened";
}
#endif
