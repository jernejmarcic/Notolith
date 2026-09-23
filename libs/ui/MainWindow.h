#pragma once

#include <QJsonObject>
#include <QMainWindow>
#include <QPointer>
#include <QThreadPool>
#include <QTimer>

#include <functional>
#include <memory>
#include <optional>

#include "Hotkeys.h"
#include "Index.h"
#include "Indexer.h"
#include "MarkdownEditor.h"
#include "Settings.h"
#include "Theme.h"
#include "Vault.h"

class QButtonGroup;
class QSplitter;
class QFileSystemModel;
class QFileSystemWatcher;
class QLabel;
class QLineEdit;
class QMenu;
class QModelIndex;
class QStackedWidget;
class QTabBar;
class QToolButton;
class QTreeView;

namespace noteAppC {

class SpellChecker;
class NoteHistory;
class PluginRegistry;
class SyncEngine;

class BacklinksPane;
class BookmarksPane;
class GraphView;
class MarkdownEditor;
class OutgoingLinksPane;
class OutlinePane;
class PagePreview;
class QuickSwitcher;
class SearchPane;
class SettingsPage;
class TagsPane;
class PropertiesPane;
class SlidesView;

/// The shell: a header bar in place of Obsidian's ribbon, a file explorer,
/// Obsidian's workspace of tabs in splits, a settings page in the main area,
/// and single-file mode for a note opened with no vault around it. This
/// class is only the chrome around the editor engine and the settings/theme
/// layers.
class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    /// Where a call to openNote() came from, so it can decide two things that
    /// depend on it: whether to push a back/forward history entry, and
    /// whether to reveal the note in the file tree (which already shows it
    /// selected when the click came from there in the first place).
    enum class NavSource { Tree, History, Other };

    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

    void openVault(const QString &path);
    /// Returns false when `path` was not opened - not a note, unsaved changes
    /// declined, or (only tolerated for NavSource::History, where the entry
    /// is simply skipped rather than erroring) deleted or renamed since it was
    /// visited.
    /// `fragment`, without the '#', is where in a view file to open: a
    /// book's position, a PDF's `page=N`.
    bool openNote(const QString &path, NavSource source = NavSource::Other, const QString &fragment = QString());
    /// Dispatches on `target.mode`: a vault (plus note), one file with no
    /// vault around it, or nothing - in which case the most recent vault is
    /// restored, if there is one.
    void openTarget(const LaunchTarget &target);

    /// Opens `target` only once the empty shell has been painted.
    ///
    /// A window that appears immediately and fills in reads as faster than one
    /// that appears complete after the same wait, so nothing here touches a
    /// vault before there is something on screen. Call it instead of
    /// openTarget() at startup, after show().
    void openWhenShown(const LaunchTarget &target);

    const Theme &theme() const { return m_theme; }
    void setThemeMode(Theme::Mode mode);
    void toggleSidebar();
    void toggleRightSidebar();
    void openQuickSwitcher();
    /// Obsidian's command palette: every command the window can run now, the
    /// focused note's editing commands with them, most recently used first.
    void openCommandPalette();
    /// Obsidian's "Toggle reading view" (Ctrl+E): reading, and back to the
    /// editing mode the tab was in.
    void toggleReadingView();
    /// "Toggle Live Preview/Source mode"; in reading view, the mode the tab
    /// will edit in.
    void toggleSourceMode();
    void openSettings();
    void closeSettings();
    /// Index for the open vault, so views can query links and tags.
    Index *index() { return &m_index; }
    /// The focused tab's editor, for tools and the benchmark.
    MarkdownEditor *editor() { return m_active->editor; }
    /// Alt+Left, the header's back button, and the mouse's back side button.
    void goBack();
    /// Alt+Right, the header's forward button, and the mouse's forward side
    /// button.
    void goForward();

protected:
    void closeEvent(QCloseEvent *event) override;
    /// Below a phone's width both sidebars fold away, and come back as they
    /// were once there is room again.
    void resizeEvent(QResizeEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private slots:
    void chooseVault();
    bool save();
    /// Enter/double-click/Return on the current row: opens a note, ignores a
    /// folder (which toggles from onTreeClicked() or the Enter key filter
    /// instead, so a double-click's extra activation does not toggle it a
    /// second time on top of the two plain clicks that make it up).
    void onTreeActivated(const QModelIndex &index);
    /// Every mouse click on a row, once per click: opens a note, or toggles a
    /// folder the way Obsidian does.
    void onTreeClicked(const QModelIndex &index);
    void onModificationChanged(bool modified);

    // --- file operations (New note/folder, Rename, Delete) -----------------
    // Slots, not plain methods, so tests can drive them with
    // QMetaObject::invokeMethod the same way they already drive save().

    void newNoteIn(const QString &folderRelative);
    void newFolderIn(const QString &folderRelative);
    void renamePathAt(const QModelIndex &index);
    void deletePathAt(const QModelIndex &index);

private:
    struct TabGroup;
    /// One tab: a note in an editor of its own, with its own back/forward
    /// history, as each of Obsidian's tabs has.
    struct Leaf {
        QWidget *area = nullptr; ///< the inline title over the editor
        MarkdownEditor *editor = nullptr;
        QLabel *inlineTitle = nullptr;
        Note note;        ///< line endings and BOM of the open file
        QString notePath; ///< absolute path, empty when nothing is open
        /// Absolute paths visited in this tab, oldest first; a fresh
        /// navigation truncates anything past historyIndex before appending,
        /// the way a browser's history does once you follow a new link after
        /// going back. -1 means nothing has been opened yet.
        QStringList history;
        int historyIndex = -1;
        TabGroup *group = nullptr;
        /// A canvas or a base shown in place of the editor, or null.
        QWidget *view = nullptr;
        /// What Ctrl+E goes back to from reading view.
        MarkdownEditor::Mode editingMode = MarkdownEditor::Mode::LivePreview;
        /// A restored tab's file, loaded the first time the tab is shown, as
        /// Obsidian defers its background tabs: a layout of ten tabs costs
        /// one note's open at startup, not ten.
        QString pendingPath;
    };
    /// Tabs in one split: a tab bar over a stack of their editors.
    struct TabGroup {
        QWidget *widget = nullptr;
        QWidget *strip = nullptr;
        QTabBar *tabs = nullptr;
        QStackedWidget *stack = nullptr;
        QList<Leaf *> leaves; ///< in tab order
    };

    QWidget *buildHeaderBar();
    QWidget *buildSidebar();
    QWidget *buildMainArea();
    QWidget *buildRightSidebar();
    void buildActions();
    void applyTheme();
    TabGroup *newGroup();
    /// A tab in `group`, empty, configured like every other editor.
    Leaf *newLeaf(TabGroup *group);
    /// Everything the settings, theme, hotkeys and spelling give an editor,
    /// for one just made; applySettings() and friends do it for all.
    void configureEditor(MarkdownEditor *editor);
    /// Makes `leaf` the tab the window's commands and side panes follow.
    void activateLeaf(Leaf *leaf);
    /// Matches `group`'s tab bar to its leaves: a tab each, except a window's
    /// only tab while nothing is open in it, which shows no tab at all.
    void syncTabs(TabGroup *group);
    QString tabTitle(const Leaf *leaf) const;
    /// The editor's modified flag, or the canvas's when the tab shows one.
    bool isModified(const Leaf *leaf) const;
    /// A file a tab shows in a view of its own rather than the editor: a
    /// .canvas or a .base, when those modules are built and switched on.
    bool isViewFile(const QString &path) const;
    /// The other tabs showing the note `leaf` shows in the editor.
    QList<Leaf *> viewsOfSameNote(const Leaf *leaf) const;
    /// maybeSave() for the focused tab leaving its note - closed, or opening
    /// another: no question while another tab keeps the same document.
    bool maybeLeave();
    /// Shows `path` in the focused tab's view; false when it could not be read.
    bool openView(const QString &path, NavSource source, const QString &fragment = QString());
    /// Puts the editor back in `leaf` in place of its view.
    void closeView(Leaf *leaf);
    /// Records a visit in the focused tab's back/forward history.
    void pushHistory(const QString &path, NavSource source);

    // --- workspace layouts (Obsidian's Workspaces core plugin) ------------
    /// The splits and tabs, in Obsidian's workspace.json shape.
    QJsonObject layoutJSON() const;
    /// Rebuilds the splits and tabs from `layout`; false when it has none.
    /// Only each split's visible tab is read now, the rest when shown.
    bool restoreLayout(const QJsonObject &layout);
    /// Keeps the open vault's layout for the next time it is opened.
    void saveLayout();
    void loadNamedLayout(const QString &name);
    /// "Manage workspace layouts": save, load and delete named ones.
    void manageLayouts();
    void gotoTab(int index);

    // --- optional features ---------------------------------------------------
    /// Whether an optional feature is on: noteAppC's own switch. A feature
    /// that is off is not built, connected or refreshed - it costs nothing.
    bool featureOn(const char *id) const;
    /// `editor.defaultMode`, for a new tab.
    MarkdownEditor::Mode defaultMode() const;
    /// Builds what is switched on and destroys what is not: the side panes
    /// and their tabs, the graph, page preview; and disables the commands of
    /// every feature that is off.
    void applyFeatures();
    /// Community plugins: the registry started or stopped to match the
    /// switch, after the first paint only. `rescan` rebuilds the commands
    /// and code-block languages even when the switch did not move - a
    /// plugin was turned on or off in settings.
    void syncPlugins(bool rescan = false);
    void notifyPlugins(const char *event, const QString &path);
    /// Points the note-following side panes at `path` (empty clears them).
    void showNoteInPanes(const QString &path);
    /// The hover preview, built the first time it is wanted.
    PagePreview *pagePreview();
    /// Git history of every save (the history module): opened for the vault
    /// when the feature is on, closed when it is not.
    void updateNoteHistory();
    /// Sync: an engine for this vault while the switch is on, a server and
    /// an account are set, and this session signed in - after the first
    /// paint only. syncSignIn() asks the password, here on the UI thread.
    void updateSync();
    void syncSignIn();
    /// Queues `text` as a new version of `absolutePath`, off the UI thread.
    void recordHistory(const QString &absolutePath, const QString &text);
    /// "Note history: Show this note's history".
    void openNoteHistory();
    /// Obsidian's close-tab: offers to save, then drops the tab and, with its
    /// last tab, the split. The window's last tab is emptied instead.
    void closeLeaf(Leaf *leaf);
    /// "Split right" (Qt::Horizontal) and "Split down": a new tab group beside
    /// the focused one, showing the same note.
    void splitActive(Qt::Orientation orientation);
    void newTab();
    /// Ctrl+Tab and Ctrl+Shift+Tab, within the focused split.
    void cycleTab(int step);
    /// Offers to save every modified tab. False means the user cancelled.
    bool maybeSaveAll();
    /// Back to one empty tab, for a vault switch or close.
    void resetWorkspace();
    /// Removes a tab without asking, activating its neighbour.
    void dropLeaf(Leaf *leaf);
    void followLink(const QString &target);
    void applyEditorSettings(MarkdownEditor *editor);
    QString tabBarStyle() const;
    /// Keeps `leaf`'s inline title on the same column as its note text.
    void updateInlineTitleMargins(Leaf *leaf);
    /// Rebuilds `m_theme` from Settings (mode, scheme, accent, font size),
    /// applies it, then every editor/appearance setting that has a live
    /// hook. Called at startup, on any settings-page edit, and whenever the
    /// vault changes (a new Obsidian layer, a new scheme search path).
    void applySettings();
    Theme themeFromSettings() const;
    /// Reloads Settings for `vaultRoot` (a fresh Obsidian layer, or none),
    /// then replays whatever the running session had customised but not yet
    /// saved - the user layer is global, not per-vault, so a setThemeMode()
    /// call or a settings-page edit made before switching vaults should
    /// survive the switch, not be silently dropped by re-reading disk.
    void reloadSettings(const QString &vaultRoot);
    /// Points m_themeWatcher at every directory Scheme::searchPaths() would
    /// look in (so adding or removing a theme file updates the settings
    /// page's list) plus the selected scheme's own file (so editing it
    /// restyles the window). A no-op when neither the vault nor the selected
    /// scheme has changed since the last call, so this costs nothing on the
    /// settings-page keystroke path.
    void watchThemeFiles();
    /// Shows the page preview for the link the editor last reported hovering.
    void showPagePreview();
    void openRecovery();
    /// Today's daily note (`step` 0), or the one before or after the daily
    /// note that is open (-1, +1), created from its template when missing.
    /// "Slides: Start presentation": the note's slides, full screen.
    void startPresentation();
    /// "Unique note creator": a note named by the time, per zk-prefixer.json.
    void createUniqueNote();
    /// Note composer: the current note appended to one picked, its links
    /// following it there, and it moved to the trash.
    void mergeCurrentNote();
    bool mergeNote(const QString &source, const QString &target);
    /// Note composer: the selection moved into a new note beside this one,
    /// a link (or, set so, an embed) left in its place.
    void extractSelection();
    void openDailyNote(int step);
    void insertTemplate();
    /// Moves the caret to the heading `subpath` names ("#Heading"), now or,
    /// for a note still being parsed, as soon as its headings arrive.
    /// Where `subpath` ("Heading", "#Heading", "^block-id") starts in
    /// `text`, or -1: the heading's line, or the line a block marker ends.
    static qsizetype subpathOffset(const QString &text, const QString &subpath);
    void jumpToHeading(const QString &subpath);
    /// The vault's own Obsidian configuration when it has one, else noteAppC's.
    class DailyNotes dailyNotes() const;
    /// Obsidian's Export to PDF, for the open note.
    void exportPdf();
    /// Puts a snapshot back as `notePath`'s content, keeping what it held as
    /// a snapshot of its own first.
    void restoreSnapshot(const QString &notePath, const QString &text);
    /// Gives every command its keys: noteAppC's hotkeys.json over the vault's
    /// Obsidian one over the built-in keys, for the window's commands and the
    /// editor's alike.
    void applyHotkeys();
    /// Spelling for the editor, from the spellcheck module when it is compiled
    /// in: the dictionaries load on the thread pool once the window has been
    /// painted, and the editor gets them when they are ready. A no-op in a
    /// build without the module.
    void updateSpelling();
    /// QFileSystemWatcher fired: re-applies the theme, puts back any watch a
    /// save-by-rename dropped, and refreshes the settings page's scheme list
    /// if it is open.
    void reloadThemeFiles();
    /// Offers to save a modified note. False means the user cancelled.
    bool maybeSave();
    /// Saves `leaf`'s note; `quiet` for autosave: no "Saved" message, and a
    /// failure in the status bar rather than a box.
    bool saveLeaf(Leaf *leaf, bool quiet);
    void reportSaveError(const QString &path, const QString &error, bool quiet);
    /// Every tab with unsaved changes, a moment after typing stops.
    void autosave();
    void updateTitle();
    void updateWordCount();
    void onIndexChanged();
    /// Opens a vault-relative path from the index or the switcher.
    void openRelative(const QString &relativePath);
    void openSingleFile(const QString &path);
    /// Drops the open vault: back to the empty-window state, chrome intact.
    void closeVault();
    /// Resets the editor, tab and both panes to "nothing open", without
    /// touching the vault itself. Shared by vault switching, the tab's close
    /// button, and deleting the open note.
    void clearOpenNote();
    QToolButton *iconButton(const QString &objectName, const QString &icon,
                            const QString &tooltip, int size);
    /// One icon-plus-label button in the right sidebar's flat tab row;
    /// muted, accented while its pane is showing.
    QToolButton *rightSidebarTabButton(QWidget *parent, const QString &objectName,
                                       const QString &icon, const QString &label,
                                       const QString &tooltip, QWidget *pane);
    void updateTabButtonIcon(QToolButton *button);
    /// Shows the right sidebar on `pane`, its tab checked.
    void showRightPane(QWidget *pane);
    /// "Search: Search in all files": the search pane, focused, with `query`
    /// run when one is given (a tag, a bookmarked search).
    void openSearch(const QString &query = QString());
    /// "Graph view: Open graph view", in the main area; again returns to the
    /// note. Built on first use: nothing optional before first paint.
    void openGraph();
    /// "Publish: Export the vault as a website": asks for a folder, then
    /// writes the site there off the UI thread (libs/ext/publish).
    void publishSite();
    /// (Re)builds the vault menu button's popup just before it shows, since
    /// its contents depend on OpenMode and the current vault.
    void rebuildVaultMenu();

    void showTreeContextMenu(const QPoint &pos);
    /// Vault-relative folder a new note/folder should land in: the selected
    /// directory, the parent of a selected file, or the vault root.
    QString folderRelativeFor(const QModelIndex &index) const;

    /// Any navigation - the tree, the switcher, a link, history - leaves the
    /// settings page if it happens to be open. openNote() calls this itself;
    /// a click on a folder never reaches openNote at all, so onTreeClicked()
    /// and the Enter-key handling in eventFilter() call it too.
    void leaveSettings();
    /// Captures the open note's caret and scroll into m_settings and saves it,
    /// so the next launch (or the next time this note is opened this session)
    /// can put them back. Called when a note is closed or replaced by
    /// another, and on quit - never on every caret move, which is why this is
    /// its own function rather than something hung off a cursor signal.
    void rememberCurrentPosition();
    /// Expands `path`'s ancestors, selects it and scrolls it into view.
    /// QFileSystemModel fills a directory lazily on a worker thread, so a note
    /// revealed right after a vault switch may have no index yet; this waits
    /// for that specific directory and tries again rather than doing nothing.
    void revealInTree(const QString &path);
    /// Enables/disables the header's back and forward buttons to match
    /// whether there is anywhere left to go.
    void updateHistoryButtons();
    /// A vault switch (or close) starts a fresh back/forward history; the
    /// previous vault's notes are not reachable from the new one's tree.
    void resetHistory();

    Theme m_theme = Theme::builtin(Theme::Mode::Dark);
    Settings m_settings = Settings::load();
    OpenMode m_openMode = OpenMode::Empty;
    Vault m_vault;
    Index m_index;
    Indexer *m_indexer = nullptr;
    /// What openWhenShown() is waiting to open, cleared by the first paint.
    std::optional<LaunchTarget> m_pendingOpen;

    /// Live reload for the selected colour scheme and the directories it can
    /// be picked from; owned because MainWindow is what applies the theme.
    QFileSystemWatcher *m_themeWatcher = nullptr;
    QPointer<PagePreview> m_preview; ///< null until a link is first hovered
    /// Resolves a link as the open note sees it; the editor, the preview and
    /// PDF export all use it.
    std::function<QString(const QString &)> m_resolveLink;
    /// A moment's pause before a preview opens, so sweeping the pointer across
    /// a paragraph of links does not flash one up for each.
    QTimer m_previewDelay;
    QString m_previewTarget;
    QPoint m_previewPos;
    /// The window's commands, each an action named by its command id and
    /// carrying its built-in keys as data.
    QList<QAction *> m_commands;

    std::shared_ptr<SpellChecker> m_spell;
    /// Every history call runs on this one thread, in order: libgit2's
    /// repository is not to be shared between threads.
    QThreadPool m_historyPool;
    std::shared_ptr<NoteHistory> m_noteHistory;
    QString m_noteHistoryRoot;
    int m_historyRecords = 0;
    bool m_spellLoading = false;
    bool m_publishing = false;
    PluginRegistry *m_plugins = nullptr;
    QList<QAction *> m_pluginCommands;
    QStringList m_pluginLanguages;
    std::function<void(MarkdownEditor *)> m_applyPluginRenderer; ///< for an editor made later
    bool m_spellHanded = false; ///< the editor has m_spell's callbacks now
    bool m_painted = false;     ///< optional work waits for the first paint
    bool m_narrow = false;      ///< the sidebars are folded for want of width
    bool m_leftBeforeNarrow = true;
    bool m_rightBeforeNarrow = true;
    /// What m_themeWatcher is currently set up for, so watchThemeFiles() can
    /// skip the rescan when nothing that would change it has moved.
    QString m_watchedSchemePath;
    QString m_watchedThemeVaultRoot;

    QFileSystemModel *m_files = nullptr;
    QTreeView *m_tree = nullptr;
    /// Every tab and split; m_active is the focused tab, never null once the
    /// main area is built.
    std::vector<std::unique_ptr<Leaf>> m_leaves;
    std::vector<std::unique_ptr<TabGroup>> m_groups;
    Leaf *m_active = nullptr;
    bool m_syncingTabs = false; ///< syncTabs() is moving tabs: not a user's switch
    QSplitter *m_workspace = nullptr; ///< the root split
    Hotkeys::Bindings m_editorBindings; ///< for editors made after applyHotkeys()
    MarkdownEditor::Spelling m_editorSpelling;
    QLabel *m_wordCount = nullptr;
    QLabel *m_syncStatus = nullptr;
    std::shared_ptr<SyncEngine> m_sync;
    QString m_syncFor;      ///< "root\nserver\nemail" the running engine was started for
    QString m_syncPassword; ///< from syncSignIn, handed to the next engine once
    QLabel *m_vaultLabel = nullptr;
    QWidget *m_sidebar = nullptr;
    QWidget *m_rightSidebar = nullptr;
    QStackedWidget *m_rightPanes = nullptr;
    QButtonGroup *m_rightTabs = nullptr;
    /// Each null while its feature is off (applyFeatures()), and m_graph
    /// until first opened; the graphs are null without their module.
    QPointer<SearchPane> m_search;
    QPointer<GraphView> m_graph;
    QPointer<GraphView> m_localGraph;
    QPointer<BacklinksPane> m_backlinks;
    QPointer<TagsPane> m_tags;
    QPointer<PropertiesPane> m_properties;
    QPointer<SlidesView> m_slides; ///< a main-area page, built on first use
    bool m_slidesLeftFullScreen = false; ///< whether ending a presentation restores the window
    QPointer<OutgoingLinksPane> m_outgoingLinks;
    QPointer<OutlinePane> m_outline;
    QPointer<BookmarksPane> m_bookmarks;
    class TemplatePicker *m_templatePicker = nullptr;
    QuickSwitcher *m_switcher = nullptr;
    QuickSwitcher *m_palette = nullptr; ///< null until first opened
    QHash<QString, QString> m_paletteIDs; ///< shown name -> command id, as last listed
    QStringList m_recentCommands;         ///< ids, most recent first, this session
    /// The default mode last applied to every tab: a settings pass changes
    /// an open tab's mode only when this setting is what changed.
    QString m_appliedDefaultMode;
    bool m_themeApplied = false; ///< applySettings has styled the window once
    QTimer m_autosave; ///< restarted by every edit

    QWidget *m_headerBar = nullptr;
    QToolButton *m_vaultMenuButton = nullptr;
    QMenu *m_vaultMenu = nullptr;
    QLineEdit *m_headerSearch = nullptr;
    QToolButton *m_backButton = nullptr;
    QToolButton *m_forwardButton = nullptr;

    QStackedWidget *m_mainStack = nullptr; ///< index 0: notes; index 1: settings
    SettingsPage *m_settingsPage = nullptr;
};

} // namespace noteAppC
