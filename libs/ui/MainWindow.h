#pragma once

#include <QJsonObject>
#include <QMainWindow>
#include <QPointer>
#include <QFutureWatcher>
#include <QThreadPool>
#include <QSet>
#include <QTimer>

#include <functional>
#include <memory>
#include <optional>

#include "Hotkeys.h"
#include "Index.h"
#include "Indexer.h"
#include "MarkdownEditor.h"
#include "ModuleHost.h"
#include "ModuleRegistry.h"
#include "Settings.h"
#include "Theme.h"
#include "FileIcons.h"
#include "Vault.h"
#ifdef NOTEAPPC_HAVE_SHARE
#include "ShareEngineBase.h"
#endif

class QDialog;
class QAbstractButton;
class QButtonGroup;
class QSplitter;
class QFileSystemModel;
class QFileSystemWatcher;
class QDrag;
class QDropEvent;
class QLabel;
class QLineEdit;
class QMenu;
class QMimeData;
class QModelIndex;
class QStackedWidget;
class QTabBar;
class QToolButton;
class QTreeView;

namespace noteAppC {

class SpellChecker;
class DictClient;
class NoteHistory;
class PluginRegistry;
class SyncEngine;
class ShareSession;
class AudioRecorder;
namespace pandoc {
class Runner;
}
namespace crypt {
class EncryptionSession;
}
namespace notebook {
class NotebookController;
}

class BacklinksPane;
class BookmarksPane;
class GraphView;
class InlineTitleEdit;
class MarkdownEditor;
class OutgoingLinksPane;
class MetaNotesPane;
class OutlinePane;
class PagePreview;
class PopoutWindow;
class QuickSwitcher;
class SearchPane;
class SettingsPage;
class TagsPane;
class PropertiesPane;
class FootnotesPane;
class SlidesView;
#ifdef NOTEAPPC_HAVE_CANVAS
class CanvasView;
#endif

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

    /// Where "Look up" gets its definition: `done` is called with markdown,
    /// now or later. The default asks a DICT server (DictClient); a test
    /// sets its own, so none ever reaches a network.
    using LookupFunction =
        std::function<void(const QString &word, std::function<void(const QString &markdown)> done)>;
    void setLookupFunction(LookupFunction lookup) { m_lookupFunction = std::move(lookup); }

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

    /// A second launch handed this window `argument` (a path, or empty for
    /// "just show yourself"): opens it as the command line would - a note in
    /// the open vault in a tab, another vault the way the vault menu does,
    /// a loose file as a tab rather than replacing the vault - and brings
    /// the window to the front.
    void openFromOtherInstance(const QString &argument);

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
    bool settingsOpen() const;
    /// Index for the open vault, so views can query links and tags.
    Index *index() { return &m_index; }
    /// The focused tab's editor, for tools and the benchmark.
    MarkdownEditor *editor() { return m_active->editor; }
    /// The loadable-module registry: scanned after first paint, or now by
    /// scanModulesNow() (tests, the bench).
    ModuleRegistry *modules() const { return m_modules; }
    void scanModulesNow();
    const Settings &settings() const { return m_settings; }
    /// A command's QAction by id (the objectName every command carries); null when there is none.
    QAction *command(const QString &id) const;
    /// Alt+Left, the header's back button, and the mouse's back side button.
    void goBack();
    /// Alt+Right, the header's forward button, and the mouse's forward side
    /// button.
    void goForward();

    /// Encrypted notes: what asks for the password, in place of the dialog.
    /// For the tests. `creating` true when the password is being set.
    void setEncryptionPromptForTests(std::function<QString(bool creating, bool retry)> prompt);
    /// The autosave pause and its longest wait, in ms. For the tests.
    void setAutosaveIntervalsForTests(int pause, int maxWait);
    /// Runs on the save thread at the start of every background commit, before
    /// anything is read or written: a test holds a commit open with it.
    void setSaveJobHookForTests(std::function<void()> hook);
    /// What the module scan asks about each suffix a module claims.
    static bool isBuiltInSuffixForTests(const QString &suffix);

#ifdef NOTEAPPC_HAVE_SHARE
    /// Sharing (docs/SHARE.md): null while the "Share a note" feature is off,
    /// no vault is open, or nothing has asked for it: the engine is the
    /// notolith-share module, loaded when the vault already holds shares (after
    /// the first paint) or on ensureShare(). Built, it is not running (no keys
    /// read, no socket) until a note is shared or joined, or the vault holds shares.
    ShareEngineBase *shareEngine() const { return m_share.get(); }
    /// Builds the engine now, loading the module (scanning for modules first if that has not
    /// happened): what sharing or joining a note does, and the tests. False, with *error, when
    /// the feature is off, no vault is open, or the module is missing or will not load.
    bool ensureShare(QString *error = nullptr);
    /// Shares the note in the active tab; returns its invitation code, or ""
    /// with *error. "Share: Share this note" shows the code.
    QString shareNote(QString *error);
    /// Joins the share an invitation code names, as this device's name; the
    /// note appears under Shared/ when the owner approves. False with *error
    /// for a code that cannot be used.
    bool joinShare(const QString &code, QString *error);
    /// Stops sharing the active note on this device; the others keep it.
    void stopSharingNote();
    /// For the tests: the engine's settings (ports, keyring entry, store),
    /// read when the engine is built; and who answers the "device asks to join"
    /// question, in place of the dialog.
    void setShareConfigForTests(ShareEngineBase::Config config);
    void setShareApprovalForTests(std::function<bool(const QString &name, const QString &safetyNumber)> approve);
    /// Who answers "run this block of a shared note?", in place of the dialog;
    /// gets the members' names, one per line.
    void setSharedRunApprovalForTests(std::function<bool(const QString &members)> approve);
#endif

    /// Where a dragged tab lands over a tab group's body: the outer quarter
    /// along an edge splits there, the rest joins the group.
    enum class DropZone { Left, Right, Top, Bottom, Center };
    static DropZone dropZoneAt(const QRect &rect, const QPoint &pos);
    /// What dragging tab `index` of `tabs` carries. For the tests: the drag
    /// itself cannot run offscreen.
    QMimeData *tabDragMimeForTests(const QTabBar *tabs, int index) const;
    /// The same for a right-sidebar pane's tab.
    QMimeData *paneDragMimeForTests(const QWidget *pane) const;
    /// Whether a drag that no window took opens one: only when it was not
    /// dropped on a group, never over one of our windows and not cancelled with Escape.
    static bool dropOpensWindow(bool handled, bool inside, bool escaped);
    /// For the tests, which cannot run a drag: act as if one were running
    /// (`on`), and whether an Escape was pressed meanwhile.
    void watchDragForTests(bool on);
    bool dragEscapedForTests() const;

protected:
    void closeEvent(QCloseEvent *event) override;
    /// Below a phone's width both sidebars fold away, and come back as they
    /// were once there is room again.
    void resizeEvent(QResizeEvent *event) override;
    void changeEvent(QEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private slots:
    void chooseVault();
    bool save();
    /// What the autosave timers run: every modified plain note is encoded here
    /// and its file committed (fdatasync) on a thread of its own; flushSaves()
    /// waits for them. Anything that must see the file on disk goes through
    /// saveLeaf(), autosave() or reloadChangedNotes(), which flush first.
    void autosaveInBackground();
    /// Waits for every background save and runs what follows each (history,
    /// plugins, the tab's modified mark, the index).
    void flushSaves();
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
    /// Where a new note goes when nothing chose a folder: files.newNoteLocation
    /// (Obsidian's newFileLocation) - the vault root, the open note's folder,
    /// or files.newNoteFolder. Vault-relative, "" the root.
    QString newNoteFolder() const;
    /// The quick switcher's "create": a note named as typed, "Folder/Name" too.
    void createNoteNamed(const QString &typed);
    void newFolderIn(const QString &folderRelative);
    void renamePathAt(const QModelIndex &index);
    /// The rename itself, shared by the tree and the inline title: moves the
    /// file, then links, history, tabs and the index follow. False with `error`.
    bool renameVaultPath(const QString &oldPath, bool isDir, const QString &newName, QString *error);
    /// The move behind a tree drag, "Move to..." and the move command: the
    /// same follow-up as a rename. `targetFolder` is absolute. False with `error`.
    bool moveVaultPath(const QString &oldPath, bool isDir, const QString &targetFolder, QString *error);
    /// Everything that follows a file or folder to its new path once the vault
    /// has moved it: icons, plugins, history, links, tabs, the index.
    /// `unindexed`: notes saved or being indexed that the index may not hold yet
    /// (vault-relative, at their old paths), whose links are read from disk too.
    void followPathChange(const QString &oldPath, bool isDir, const QString &newPath,
                          const QStringList &unindexed);
    /// The notes `saved` (absolute) and those the indexer is still reading,
    /// vault-relative: what a rename must open besides what the index lists.
    QStringList unindexedNotes(const QStringList &saved) const;
    /// moveVaultPath for a UI caller: a refusal goes to the status bar.
    void moveToFolder(const QString &path, const QString &folder);
    /// Folder picker over the vault's visible folders, then moveToFolder().
    void chooseMoveFolder(const QString &path);
    void deletePathAt(const QModelIndex &index);
    void deletePath(const QString &absolutePath);
    /// Obsidian's "Make a copy": "Name 1.ext" beside it, opened when a note.
    QString copyPath(const QString &absolutePath);
    /// Saves unsaved typing in the note, before something else writes the file.
    bool saveTyping(const QString &absolutePath); ///< false: it could not be saved
    /// A UI-only icon for a file or folder in the tree and on its tabs: a
    /// bundled icon id or one character; empty removes it. Kept outside the
    /// vault (FileIcons).
    Q_INVOKABLE void setFileIcon(const QString &absolutePath, const QString &icon);

private:
    struct TabGroup;
    /// One tab: a note in an editor of its own, with its own back/forward
    /// history, as each of Obsidian's tabs has.
    struct Leaf {
        QWidget *area = nullptr; ///< the inline title over the editor
        MarkdownEditor *editor = nullptr;
        InlineTitleEdit *inlineTitle = nullptr;
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
        QString viewTitle; ///< a module view with no file (the glyph studio): what the tab shows
        /// What Ctrl+E goes back to from reading view.
        MarkdownEditor::Mode editingMode = MarkdownEditor::Mode::LivePreview;
        /// A restored tab's file, loaded the first time the tab is shown, as
        /// Obsidian defers its background tabs: a layout of ten tabs costs
        /// one note's open at startup, not ten.
        QString pendingPath;
        /// A restored tab's file that nothing could open (a PDF with no pdfview package): the tab shows nothing,
        /// and the saved workspace still names the file. Never retried by focus; retried when the set of
        /// modules changes or the user picks the tab from the tab bar. Cleared by any open into the tab.
        QString keptPath;
        /// Opening another note here opens it in a new tab instead; the tab's
        /// close button is a pin that unpins it.
        bool pinned = false;
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
    QIcon tabIcon(const Leaf *leaf) const;
    /// appearance.fileIcons; off hides every icon and the menu items, the icons are kept.
    bool fileIconsOn() const;
    void chooseFileIcon(const QString &absolutePath);
    /// The tree and every tab, after an icon or the setting changed.
    void refreshFileIcons();
    /// The editor's modified flag, or the canvas's when the tab shows one.
    bool isModified(const Leaf *leaf) const;
    /// A file a tab shows in a view of its own rather than the editor: a
    /// .canvas or a .base, when those modules are built and switched on.
    bool isViewFile(const QString &path) const;
    /// Why a .pdf has no viewer: the package is missing, the module was refused, or the feature is switched off.
    QString pdfViewerProblem() const;
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

    // --- pop-out windows -------------------------------------------------
    // Top-level windows MainWindow owns: tabs in splits of their own, a
    // detached sidebar, or a detached sidebar pane. m_leaves and m_active
    // stay global, so every command, side pane and save follows a tab
    // wherever it is.
    /// The root splits tabs live in: m_workspace and each pop-out's own.
    bool isRootSplit(const QSplitter *split) const;
    QList<QSplitter *> rootSplits() const;
    /// This window or one of its pop-outs: where an app-wide event may belong to the workspace.
    bool isWorkspaceWindow(const QWidget *window) const;
    /// Moves `leaf` (its view with it) into `to` at tab `index`, collapses the
    /// group it left, and makes it the active tab.
    void moveLeaf(Leaf *leaf, TabGroup *to, int index);
    /// A new, empty group next to `beside`, after it or before; splits `beside`'s parent.
    TabGroup *splitBeside(TabGroup *beside, Qt::Orientation orientation, bool after);
    /// Removes `group` when it holds no tab, collapsing the splits it leaves
    /// with one child, and a pop-out left empty with it. The main workspace
    /// never ends empty: it gets a fresh empty tab. True when the group is gone.
    bool collapseIfEmpty(TabGroup *group);
    /// A pop-out window with one split: holds `leaf` when given, else a new empty tab.
    TabGroup *newPopout(Leaf *leaf = nullptr);
    /// The user closed `window`: its tabs close like any other (a save that
    /// fails or is declined vetoes the close), and can be undone.
    void closePopout(PopoutWindow *window, bool *veto);
    /// Forgets `window` and deletes it, unasked and unsaved: after its last tab
    /// is gone, or at shutdown (tabs it still holds leave the model).
    void deletePopout(PopoutWindow *window);
    /// Every pop-out's tabs back into the main workspace; the windows go.
    void dockAll();
    void buildPopoutActions();
    /// Adds a new pop-out to the ones owned, with every command a hotkey of.
    void registerPopout(PopoutWindow *window);
    /// The pop-out `widget` is in, or null while it is in the main window.
    PopoutWindow *popoutHolding(QWidget *widget) const;
    /// The whole left or right sidebar in a window of its own, lifted out of
    /// mainSplitter the way a narrow drawer is; closing the window docks it again.
    void detachSidebar(QWidget *side);
    void dockSidebar(QWidget *side);
    /// One right-sidebar pane in a window of its own, its tab hidden meanwhile.
    void detachPane(QWidget *pane);
    void dockPane(QWidget *pane);
    /// The tab of the right sidebar that shows `pane`.
    QAbstractButton *tabButtonOf(QWidget *pane) const;
    /// "Detach" in the context menu of the left header and the right tab row.
    void showDetachMenu(QWidget *side, QWidget *anchor, const QPoint &pos);
    void buildSidebarPopoutActions();
    void showTabMenu(TabGroup *group, const QPoint &globalPos);
    /// The pop-out `group` is in follows its current tab's title.
    void retitlePopout(TabGroup *group);
    /// The feature switch: commands, drop targets, and docking when it goes off.
    void applyPopoutFeature();
    /// Drag-to-tile: a tab dragged out of its bar is dropped on a group's
    /// body (zone), on a tab bar (position) or outside every window (a pop-out).
    /// Called first by eventFilter(); true when it consumed the event.
    bool popoutEventFilter(QObject *watched, QEvent *event);
    void startTabDrag(TabGroup *group, Leaf *leaf, const QPoint &pos);
    /// Dragging a right-sidebar tab: let go outside every window, `pane` is detached.
    void startPaneDrag(QAbstractButton *tab, QWidget *pane);
    /// Runs `drag`; true when it was let go outside every window of ours
    /// (taken by none and never over one), which is what opens a window.
    bool dragEndedOutside(QDrag *drag);
    void watchDragKeys();
    /// application/x-notolith-pane carrying a pane, and the pane one carries (null when foreign, gone or detached).
    QMimeData *paneMime(const QWidget *pane) const;
    QWidget *paneFromMime(const QMimeData *mime) const;
    /// application/x-notolith-leaf carrying a leaf, and the leaf one carries (null when foreign or gone).
    QMimeData *leafMime(const Leaf *leaf) const;
    Leaf *leafFromMime(const QMimeData *mime) const;
    /// The translucent zone over `group`'s body; one child widget made on first use.
    void showDropOverlay(TabGroup *group, DropZone zone);
    void hideDropOverlay();
    /// A leaf dropped on `group` at `pos` (in the group's own coordinates); false when it is not ours.
    bool handleGroupDrop(TabGroup *group, QDropEvent *event, const QPoint &pos);
    QPointer<QWidget> m_dropOverlay;
    Leaf *m_pressedLeaf = nullptr; ///< the tab the left button went down on, until it is released
    QPointer<QWidget> m_pressedPane; ///< the same for a right-sidebar tab
    QPoint m_pressPos;
    bool m_dragInside = false;  ///< the drag in progress is over one of our windows
    bool m_dragHandled = false; ///< and a group took its drop
    bool m_dragRunning = false; ///< a drag of ours is inside exec()
    bool m_dragEscaped = false; ///< and Escape was pressed during it
    std::vector<std::unique_ptr<PopoutWindow>> m_popouts;
    bool m_popoutsOn = false; ///< the popout-windows feature, as last applied

    // --- optional features ---------------------------------------------------
    /// Whether an optional feature is on: noteAppC's own switch. A feature
    /// that is off is not built, connected or refreshed - it costs nothing.
    bool featureOn(const char *id) const;
    /// A Jupyter notebook the window opens as a note (the notebooks feature is on).
    bool isNotebookFile(const QString &path) const;
    /// `editor.defaultMode`, for a new tab.
    MarkdownEditor::Mode defaultMode() const;
    /// Builds what is switched on and destroys what is not: the side panes
    /// and their tabs, the graph, page preview; and disables the commands of
    /// every feature that is off.
    void applyFeatures();
    /// What the parse looks for, from the editor settings (meta notes, counts).
    ParseOptions parseOptions() const;
    /// Tells the indexer whether a `.ipynb` is a note it reads (notebooks on) or a plain file.
    void updateNotebookIndex();
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
    /// The popover "Look up" fills, built the first time it is used.
    PagePreview *lookupPopover();
    /// What the editors' context menus offer that the window answers.
    MarkdownEditor::ContextActions contextActions() const;
    /// "Look up": the popover at `globalPos` says so at once and is filled
    /// when the definition arrives; nothing is sent before this is called.
    void lookUp(const QString &word, const QPoint &globalPos);
    /// Git history of every save (the history module): opened for the vault
    /// when the feature is on, closed when it is not.
    void updateNoteHistory();
    /// Sync: an engine for this vault while the switch is on, a server and
    /// an account are set, and this session signed in - after the first
    /// paint only. syncSignIn() asks the password, here on the UI thread.
    void updateSync();
    void syncSignIn();
    /// Sharing: an engine for this vault while the feature is on (after the
    /// first paint only) and the vault holds shares or one was asked for
    /// (ensureShare); it is the notolith-share module's, loaded here, and
    /// started when the vault holds shares already.
    void updateShare();
#ifdef NOTEAPPC_HAVE_SHARE
    bool startShare();
    ShareSession *shareSession(const QByteArray &shareID);
    /// The session of the share that note at `absolutePath` is, or null.
    ShareSession *sessionFor(const QString &absolutePath) const;
    /// Makes every session follow the tabs that show its note, and only those.
    void syncShareViews();
    /// A note under Shared/ for a share just joined.
    void placeJoinedNote(const QByteArray &shareID, const QString &title);
    /// The share's dialog, window-modal; `inviteCode` when one was just made.
    void showShareDialog(const QByteArray &shareID, const QString &inviteCode);
    void stopShare(QByteArray shareID);
#endif
    /// The status bar's "Shared - N here" and the tabs' shared marker.
    void updateShareStatus();
    /// Queues `text` as a new version of `absolutePath`, off the UI thread.
    void recordHistory(const QString &absolutePath, const QString &text);
    /// "Note history: Show this note's history".
    void openNoteHistory();
    /// Obsidian's close-tab: offers to save, then drops the tab and, with its
    /// last tab, the split. The window's last tab is emptied instead.
    void closeLeaf(Leaf *leaf);
    /// Ctrl+Shift+T: reopens the newest closed tab (20 are kept) in its split.
    void undoCloseTab();
    /// "Split right" (Qt::Horizontal) and "Split down": a new tab group beside
    /// the focused one, showing the same note.
    void splitActive(Qt::Orientation orientation);
    void newTab();
    void togglePin();
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
    /// Enter or focus-out in a tab's inline title: rename the note, or refuse
    /// with a notice and put the old name back.
    void commitInlineTitle(Leaf *leaf);
    /// Rebuilds `m_theme` from Settings (mode, scheme, accent, font size),
    /// applies it, then every editor/appearance setting that has a live
    /// hook. Called at startup, on any settings-page edit, and whenever the
    /// vault changes (a new Obsidian layer, a new scheme search path).
    void applySettings();
    /// files.hiddenExtensions: hides (or shows again) the file rows of every
    /// loaded folder. Rows that load later are handled by hideTypedRows().
    void applyHiddenTypes();
    void hideTypedRows(const QModelIndex &parent, int first, int last);
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

    // --- encrypted notes (docs/ENCRYPTION.md) ------------------------------
    /// Any note, read or written: an encrypted one is decrypted into `out`
    /// in memory only, asking for the password if the session is locked,
    /// and sealed again on write. Cancelling the password fails quietly.
    bool readNote(const QString &path, Note &out, QString *error);
    bool writeNote(const QString &path, const Note &note, QString *error);
    /// The bytes of an encrypted note of this vault, preferring `path`: what
    /// a typed password is checked against. Empty when there is none yet.
    QByteArray encryptionSample(const QString &path = QString()) const;
    /// The editor shows an encrypted note: nothing of it reaches a plugin,
    /// a kernel's output cache, a snapshot or the note history.
    bool showsEncryptedNote(const MarkdownEditor *editor) const;
    /// The note an editor shows, for whose kernel its code blocks run in.
    QString notePathOf(const MarkdownEditor *editor) const;
    /// The editor shows a note that is shared (P2P) right now.
    bool isSharedNote(const MarkdownEditor *editor) const;
    void encryptCurrentNote();
    void decryptCurrentNote();
    void newEncryptedNote();
    void changeEncryptionPassword();
    /// Saves every encrypted note that is open (the keys are still there),
    /// then, `close`, closes them: what locking does.
    void saveEncryptedLeaves();
    void closeEncryptedLeaves();
    /// Audio recorder: records into the attachment folder and embeds the
    /// file in the note it was started from once stopped.
    void startRecording();
    bool mergeNote(const QString &source, const QString &target);
    /// A user's delete, to where `files.deletedFiles` says: the system trash,
    /// the vault's .trash or nowhere. No system trash falls back to .trash,
    /// never to a permanent delete.
    bool deleteVaultPath(const QString &path, QString *error);
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
#ifdef NOTEAPPC_HAVE_PANDOC
    // --- pandoc (MainWindowPandoc.cpp) ---
    /// The pandoc commands, as actions of the "pandoc" core plugin.
    void buildPandocActions();
    /// The program to run, looked up now (never earlier): empty after telling the person it is missing.
    QString pandocProgram();
    /// A dialog: pandoc was not found, how to get it, and a button to its path in Settings.
    void showPandocMissing();
    /// Marks the pandoc commands that cannot run (no pandoc) in the palette; looks pandoc up only when the plugin is on.
    void refreshPandocCommands();
    /// `formatID`: one of pandoc::exportFormats(), "other" (asks which) or "other:name". `notePath` is a note of
    /// the vault; `outputPath` is where to write, empty to ask. Invokable, so a test can drive it past the dialogs.
    Q_INVOKABLE void exportWithPandoc(const QString &notePath, const QString &formatID, const QString &outputPath = {});
    /// Makes a note in `folderRelative` from `file`, which is never written to; the note never replaces another.
    Q_INVOKABLE void importWithPandoc(const QString &file, const QString &folderRelative);
    void importWithPandocChosen();
    /// A pandoc run with a cancel button. `done` runs when it succeeds; a failure shows pandoc's own words, a cancel nothing.
    void runPandoc(const QString &program, const QStringList &arguments, const QByteArray &input,
                   const QString &workingDirectory, const QString &title,
                   std::function<void()> done);
    pandoc::Runner *m_pandocRunner = nullptr;
#endif
    /// Puts a snapshot back as `notePath`'s content, keeping what it held as
    /// a snapshot of its own first.
    void restoreSnapshot(const QString &notePath, const QString &text);
    /// Gives every command its keys: noteAppC's hotkeys.json over the vault's
    /// Obsidian one over the built-in keys, for the window's commands and the
    /// editor's alike.
    void applyHotkeys();
    void rebuildHotkeyCommands(); ///< the tail of the command builder: the `rebindable` list, setCommands() and applyHotkeys()
    void startModuleScan();       ///< on the thread pool, after first paint
    void onModulesScanned();      ///< registers features, commands and rows; does nothing when no module is installed
    /// ModuleRegistry::ensureLoaded, then - once, after a fresh attach - the stored values of the
    /// keys the module registered and a refresh of the Settings page (its rows, or its load failure).
    bool loadModule(const ModuleInfo &module, QString *error);
    QList<QPair<QString, QString>> moduleProblems() const; ///< (id, notice) of every module that cannot load
    QStringList moduleFeatures() const;                    ///< the feature ids of the modules that can
    QAction *addModuleCommand(const QString &id, const QString &title);
    bool openModuleView(const QString &path, QString *error, QWidget **view, const QString &fragment = QString());
    void showModuleTab(ModuleView *view, const QString &title);
    /// A module view that is still parsing a reload (`ModuleView::isLoading()`) holds the document the load is about to
    /// replace, and its save() refuses until then. Closing or quitting waits for the load (an event loop, at most
    /// kModuleLoadWaitMs) and then judges what is modified: "still loading" is retry-after-load, not a failed save.
    void waitForModuleLoad(Leaf *leaf);
    static constexpr int kModuleLoadWaitMs = 15000;
    /// Spelling for the editor, from the spellcheck module when it is compiled
    /// in: the dictionaries load on the thread pool once the window has been
    /// painted, and the editor gets them when they are ready. A no-op in a
    /// build without the module.
    void updateSpelling();
    /// QFileSystemWatcher fired: re-applies the theme, puts back any watch a
    /// save-by-rename dropped, and refreshes the settings page's scheme list
    /// if it is open.
    void reloadThemeFiles();
    /// Offers to save a modified note (with autosave on, saves it, and asks
    /// only when that fails). False means the user cancelled.
    bool maybeSave();
    /// Saves `leaf`'s note; `quiet` for autosave: no "Saved" message, and a
    /// failure in the status bar rather than a box.
    bool saveLeaf(Leaf *leaf, bool quiet, bool background = false);
    struct SaveResult {
        QString error; ///< empty: the file is on disk
    };
    struct SaveJob {
        QString path;
        Note note; ///< as written
        QFutureWatcher<SaveResult> *watcher = nullptr;
    };
    void finishSave(QFutureWatcher<SaveResult> *watcher);
    /// `note` is what `path` holds on disk now: every tab showing it takes
    /// it, carets and scroll kept, and is unmodified after.
    void showDiskNote(const QString &path, const Note &note);
    /// Points m_noteWatcher at the file of every open note, and at its folder:
    /// the folder is what reports a delete-and-recreate or a move.
    void watchOpenNotes();
    /// Open notes whose file is gone: one that came back is reloaded, one that
    /// turns up elsewhere in the vault byte-identical was moved - its tabs
    /// follow it - and one that does not is reported once (`final`: after the
    /// grace a sync client's delete-then-write needs).
    void followLostNotes(bool final);
    /// Where the vault now holds `known` (what `lost` held), or nothing when
    /// that is not exactly one file.
    QString findMovedNote(const QString &lost, const Note &known);
    /// An open note changed on disk - sync, a plugin, a link update, another
    /// editor: its tabs show the change, so no autosave writes over it.
    void reloadChangedNotes();
    void reportSaveError(const QString &path, const QString &error, bool quiet);
    /// Every tab with unsaved changes, a moment after typing stops - or
    /// m_autosaveMax after the first edit, however long the typing goes on.
    /// Returns the notes it saved.
    QStringList autosave(bool background = false);
    /// An edit was made: restarts the pause, starts the max wait if idle.
    void armAutosave();
    void updateTitle();
    void updateWordCount();
    /// The word and character labels alone: the note's, or the selection's.
    void updateTextCount();
    /// The status bar's backlink count for the tab in front: one query.
    void updateBacklinkCount();
    /// The status bar's "N meta notes" for the tab in front: `size()` of the
    /// editor's runs; hidden at zero and while the feature is off.
    void updateMetaCount();
    /// Alt+] / Alt+[: selects the next (+1) or previous (-1) meta note and
    /// says which in the status bar.
    void jumpMetaNote(int direction);
    /// Live preview, source or reading for the tab in front, as the status
    /// bar's menu picks it; keeps what Ctrl+E returns to.
    void setViewMode(MarkdownEditor::Mode mode);
    /// The mode button's icon and the menu's check mark follow the editor.
    void updateModeButton();
void updateVimStatus();
    void onIndexChanged();
    /// Opens a vault-relative path from the index or the switcher.
    void openRelative(const QString &relativePath);
    void openRelativeInNewTab(const QString &relativePath); ///< the graph's node menu
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
    /// Where this vault's graph controls are kept: a file of the app's own
    /// under the config directory, never `.obsidian/graph.json`, which is
    /// only read (as the starting point when there is no copy of ours).
    QString graphOptionsPath() const;
    /// Applies the saved (or the vault's, or the default) options to `view`
    /// and saves what its controls change from then on, after a pause.
    void wireGraphOptions(GraphView *view);
    void saveGraphOptions();
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
    FileIcons m_fileIcons;
    Index m_index;
    Indexer *m_indexer = nullptr;
    /// What openWhenShown() is waiting to open, cleared by the first paint.
    std::optional<LaunchTarget> m_pendingOpen;

    /// Live reload for the selected colour scheme and the directories it can
    /// be picked from; owned because MainWindow is what applies the theme.
    QFileSystemWatcher *m_themeWatcher = nullptr;
    QPointer<PagePreview> m_preview; ///< null until a link is first hovered
    QPointer<PagePreview> m_lookupPopover; ///< null until a word is first looked up
    DictClient *m_dictClient = nullptr;    ///< likewise
    QPoint m_lookupPos;
    quint64 m_lookupToken = 0; ///< a newer lookup's answer replaces an older one
    LookupFunction m_lookupFunction;
    /// Resolves a link as the open note sees it; the editor, the preview and
    /// PDF export all use it.
    std::function<QString(const QString &)> m_resolveLink;
#ifdef NOTEAPPC_HAVE_CANVAS
    /// One hidden, never-shown CanvasView per `.canvas` embed an open editor
    /// asked to draw, keyed by that editor and the file/subpath it embeds -
    /// kept alive between renders so a wheel or a drag reuses its own pan,
    /// zoom and loaded document instead of reloading the file from scratch,
    /// and so the embed's view state really does last "until the note is
    /// closed": erased only when the editor that asked for it is destroyed.
    /// shared_ptr, not unique_ptr: QHash detaches its nodes through a copy
    /// constructor, which a unique_ptr value has none of.
    QHash<QString, std::shared_ptr<CanvasView>> m_canvasEmbedViews;
    /// Finds or creates the entry above, loading `path` the first time.
    CanvasView &canvasEmbedView(MarkdownEditor *editor, const QString &path, const QString &subpath);
    /// The same lookup, but for the input-handler side: an interaction can
    /// only ever land on an embed that has already been rendered once, so a
    /// miss here (an edit racing a click, say) is just declined.
    CanvasView *canvasEmbedViewIfExists(MarkdownEditor *editor, const QString &path,
                                       const QString &subpath) const;
    /// Rasterises `canvas`'s current view - its own size, zoom and origin -
    /// at this window's device pixel ratio. What both the cold render and
    /// every interactive re-render call.
    QImage renderCanvasEmbed(CanvasView &canvas) const;
#endif
    /// A moment's pause before a preview opens, so sweeping the pointer across
    /// a paragraph of links does not flash one up for each.
    QTimer m_previewDelay;
    QString m_previewTarget;
    QPoint m_previewPos;
    /// The window's commands, each an action named by its command id and
    /// carrying its built-in keys as data.
    QList<QAction *> m_commands;
    ModuleRegistry *m_modules = nullptr;
    ModuleHost m_moduleHost;
    QHash<QString, ModuleHost::ViewFactory> m_moduleViews; ///< by lower-case suffix
    QHash<QString, std::function<void()>> m_moduleCommands;
    QList<ModuleHost::SettingRow> m_moduleSettingRows;
    bool m_modulesScanned = false;
    bool m_modulesScanStarted = false;

    std::shared_ptr<SpellChecker> m_spell;
    /// Every history call runs on this one thread, in order: libgit2's
    /// repository is not to be shared between threads.
    QThreadPool m_historyPool;
    /// One thread commits the autosaves, in the order they were queued.
    QThreadPool m_savePool;
    QList<SaveJob> m_saveJobs; ///< in flight, oldest first
    std::function<void()> m_saveJobHook;
    bool m_closing = false; ///< closeEvent() accepted: no more focus-loss commits
    std::shared_ptr<NoteHistory> m_noteHistory;
    QString m_noteHistoryRoot;
    int m_historyRecords = 0;
    bool m_spellLoading = false;
    bool m_publishing = false;
    PluginRegistry *m_plugins = nullptr;
    QList<QAction *> m_pluginCommands;
    QStringList m_pluginLanguages;
    std::function<void(MarkdownEditor *)> m_applyPluginRenderer; ///< for an editor made later
    /// Per diagram kind (Mermaid, PlantUML, TikZ, Graphviz): "" when it is off, else the tool setting; the
    /// renderer is rebuilt (forgetting a missing tool) only when this changes, and an editor is handed it
    /// only when its own "diagramKey<kind>" differs.
    QString m_diagramKey[4];
    MarkdownEditor::CodeBlockRenderer m_diagramRenderer[4];
    bool m_spellHanded = false; ///< the editor has m_spell's callbacks now
    bool m_painted = false;     ///< optional work waits for the first paint
    bool m_narrow = false;      ///< the sidebars are folded for want of width
    /// Narrow, a sidebar opens as a drawer over the note: lifted out of the
    /// splitter, a dimmed layer behind it that closes it when tapped.
    void openDrawer(QWidget *side);
    void closeDrawer();
    QWidget *m_drawer = nullptr;
    QWidget *m_scrim = nullptr;
    int m_drawerIndex = -1;
    bool m_leftBeforeNarrow = true;
    bool m_rightBeforeNarrow = true;
    /// What m_themeWatcher is currently set up for, so watchThemeFiles() can
    /// skip the rescan when nothing that would change it has moved.
    QString m_watchedSchemePath;
    QString m_watchedThemeVaultRoot;

    QFileSystemModel *m_files = nullptr;
    QStringList m_hiddenTypes; ///< files.hiddenExtensions, normalised
    QTreeView *m_tree = nullptr;
    /// Every tab and split; m_active is the focused tab, never null once the
    /// main area is built.
    std::vector<std::unique_ptr<Leaf>> m_leaves;
    std::vector<std::unique_ptr<TabGroup>> m_groups;
    /// A closed tab, for undoCloseTab(); `group` is only compared, never used
    /// until found in m_groups.
    struct ClosedTab {
        QString path;
        TabGroup *group;
        qsizetype caret;
        int scroll;
        qsizetype top; ///< MarkdownEditor::topOffset(): where the pixel is, in text
    };
    QList<ClosedTab> m_closedTabs; ///< oldest first, at most 20
    Leaf *m_active = nullptr;
    bool m_syncingTabs = false; ///< syncTabs() is moving tabs: not a user's switch
    QSplitter *m_workspace = nullptr; ///< the root split
    Hotkeys::Bindings m_editorBindings; ///< for editors made after applyHotkeys()
    MarkdownEditor::Spelling m_editorSpelling;
    QLabel *m_wordCount = nullptr;
    QLabel *m_charCount = nullptr;
    QToolButton *m_backlinkStatus = nullptr;
    QToolButton *m_propertyStatus = nullptr;
    QToolButton *m_metaStatus = nullptr;
    QToolButton *m_modeButton = nullptr;
    QTimer m_selectionCountTimer; ///< the selection's count, 200 ms after it moves
    QTimer m_backlinkCountTimer; ///< a burst of index changes is one count
QLabel *m_vimStatus = nullptr;
    QLabel *m_syncStatus = nullptr;
    QLabel *m_shareStatus = nullptr; ///< "Shared - N here" while the active note is shared
    QLabel *m_recordingStatus = nullptr;
    AudioRecorder *m_recorder = nullptr; ///< made on the first recording
    /// Made when the first block runs; owns one kernel per language.
    notebook::NotebookController *m_notebook = nullptr;
    /// Whether anything can run a code block's language, asked once each
    /// (MarkdownEditor::setNotebookRunnable).
    QHash<QString, bool> m_runnableLanguages;
    crypt::EncryptionSession *m_encryption = nullptr; ///< null in a build without encryption
    QString m_recordingNote;
    std::shared_ptr<SyncEngine> m_sync;
    QString m_syncFor;      ///< "root\nserver\nemail" the running engine was started for
    QString m_syncPassword; ///< from syncSignIn, handed to the next engine once
    bool m_syncNeedsPassword = false; ///< the engine found no keys in the keyring and has no password
#ifdef NOTEAPPC_HAVE_SHARE
    std::unique_ptr<ShareEngineBase> m_share;
    QString m_shareFor; ///< the vault root updateShare last settled for (an engine, or none wanted)
    QString m_shareProblem; ///< why updateShare could not build the engine, "" when it could
    bool m_shareDemanded = false; ///< a command or a test asked for the engine: build it even though the vault holds no share
    ModuleHost::ShareEngineFactory m_makeShare; ///< what the notolith-share module registered when it attached
    ShareEngineBase::Config m_shareConfig;
    /// The records the store holds for this vault, read without the engine.
    QList<ShareRecord> storedShares() const;
    QHash<QByteArray, ShareSession *> m_sessions; ///< by share id, children of the window
    std::function<bool(const QString &, const QString &)> m_shareApproval;
    std::function<bool(const QString &)> m_sharedRunApproval;
    /// sha256 of language + source of the shared-note blocks allowed to run.
    /// ponytail: this run of the app only; persist it if asking again after a restart grates.
    QSet<QByteArray> m_sharedRunsAllowed;
    bool allowSharedRun(const MarkdownEditor *editor, const QString &language, const QString &source);
#endif
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
    QPointer<GraphView> m_graphChanged; ///< the graph whose controls were touched last
    QTimer m_layoutSave;                ///< the workspace a second after the tabs change, so a crash keeps it
    QTimer m_graphSave;                 ///< writes them 500 ms after the last touch
    QPointer<BacklinksPane> m_backlinks;
    QPointer<TagsPane> m_tags;
    QPointer<PropertiesPane> m_properties;
    QPointer<FootnotesPane> m_footnotes;
    QPointer<SlidesView> m_slides; ///< a main-area page, built on first use
    bool m_slidesLeftFullScreen = false; ///< whether ending a presentation restores the window
    QPointer<OutgoingLinksPane> m_outgoingLinks;
    QPointer<OutlinePane> m_outline;
    QPointer<MetaNotesPane> m_metaPane; ///< built only while editor.metaNotes is on
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
    QTimer m_autosave; ///< restarted by every edit, and by a failed save
    QTimer m_autosaveMax; ///< started by the first edit after a save, never restarted
    QString m_lastSaveError; ///< why the last quiet save failed; maybeSave() shows it
    QFileSystemWatcher *m_noteWatcher = nullptr; ///< the open notes' files
    QSet<QString> m_changedNotes;                ///< what it reported, read when m_noteChanges fires
    QTimer m_noteChanges;
    QHash<QString, bool> m_lostNotes; ///< open notes whose file is gone -> reported
    QTimer m_lostRetry;               ///< one more look before reporting

    QWidget *m_headerBar = nullptr;
    QToolButton *m_vaultMenuButton = nullptr;
    QMenu *m_vaultMenu = nullptr;
    QLineEdit *m_headerSearch = nullptr;
    QToolButton *m_headerGraphButton = nullptr; ///< the ribbon-style graph entry; null without NOTEAPPC_HAVE_GRAPH
    QToolButton *m_backButton = nullptr;
    QToolButton *m_forwardButton = nullptr;

    QStackedWidget *m_mainStack = nullptr; ///< index 0: notes; index 1: settings
    QDialog *m_settingsWindow = nullptr;
    SettingsPage *m_settingsPage = nullptr;
};

} // namespace noteAppC
