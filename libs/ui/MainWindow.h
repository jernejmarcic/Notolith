#pragma once

#include <QMainWindow>

#include <optional>

#include "Index.h"
#include "Indexer.h"
#include "MarkdownEditor.h"
#include "Settings.h"
#include "Theme.h"
#include "Vault.h"

class QFileSystemModel;
class QLabel;
class QLineEdit;
class QMenu;
class QModelIndex;
class QStackedWidget;
class QTabBar;
class QToolButton;
class QTreeView;

namespace noteAppC {

class BacklinksPane;
class MarkdownEditor;
class OutgoingLinksPane;
class QuickSwitcher;
class SettingsPage;
class TagsPane;

/// The shell: a header bar in place of Obsidian's ribbon, flat tabs, a file
/// explorer, a settings page in the main area, and single-file mode for a
/// note opened with no vault around it. The workspace splits and command
/// palette are still ahead; the editor engine and settings/theme layers are
/// not - this class is only the chrome around them.
class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    /// Where a call to openNote() came from, so it can decide two things that
    /// depend on it: whether to push a back/forward history entry, and
    /// whether to reveal the note in the file tree (which already shows it
    /// selected when the click came from there in the first place).
    enum class NavSource { Tree, History, Other };

    explicit MainWindow(QWidget *parent = nullptr);

    void openVault(const QString &path);
    /// Returns false when `path` was not opened - not a note, unsaved changes
    /// declined, or (only tolerated for NavSource::History, where the entry
    /// is simply skipped rather than erroring) deleted or renamed since it was
    /// visited.
    bool openNote(const QString &path, NavSource source = NavSource::Other);
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
    /// Obsidian's Ctrl+E: live preview <-> source.
    void toggleEditingMode();
    void openSettings();
    void closeSettings();
    /// Index for the open vault, so views can query links and tags.
    Index *index() { return &m_index; }
    /// The editor itself, for tools and the benchmark.
    MarkdownEditor *editor() { return m_editor; }
    /// Alt+Left, the header's back button, and the mouse's back side button.
    void goBack();
    /// Alt+Right, the header's forward button, and the mouse's forward side
    /// button.
    void goForward();

protected:
    void closeEvent(QCloseEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

public slots:
    /// Obsidian's Ctrl+E, exposed so tools and tests can drive it.
    void toggleEditingModeSlot() { toggleEditingMode(); }

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
    void onTextChanged();

    // --- file operations (New note/folder, Rename, Delete) -----------------
    // Slots, not plain methods, so tests can drive them with
    // QMetaObject::invokeMethod the same way they already drive save().

    void newNoteIn(const QString &folderRelative);
    void newFolderIn(const QString &folderRelative);
    void renamePathAt(const QModelIndex &index);
    void deletePathAt(const QModelIndex &index);

private:
    QWidget *buildHeaderBar();
    QWidget *buildSidebar();
    QWidget *buildMainArea();
    QWidget *buildRightSidebar();
    void buildActions();
    void applyTheme();
    /// Keeps the inline title on the same column as the note text.
    void updateInlineTitleMargins();
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
    /// Offers to save a modified note. False means the user cancelled.
    bool maybeSave();
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
                                       const QString &tooltip, int index);
    void updateTabButtonIcon(QToolButton *button);
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
    Note m_note;        ///< line endings and BOM of the open file
    QString m_notePath; ///< absolute path, empty when nothing is open
    /// What openWhenShown() is waiting to open, cleared by the first paint.
    std::optional<LaunchTarget> m_pendingOpen;

    /// Absolute paths of notes visited this session, oldest first; a fresh
    /// navigation truncates anything past m_historyIndex before appending, the
    /// way a browser's history does once you follow a new link after going
    /// back. -1 means nothing has been opened yet.
    QStringList m_history;
    int m_historyIndex = -1;

    QFileSystemModel *m_files = nullptr;
    QTreeView *m_tree = nullptr;
    MarkdownEditor *m_editor = nullptr;
    QTabBar *m_tabs = nullptr;
    QWidget *m_tabStrip = nullptr;
    QLabel *m_inlineTitle = nullptr;
    QLabel *m_wordCount = nullptr;
    QLabel *m_vaultLabel = nullptr;
    QWidget *m_sidebar = nullptr;
    QWidget *m_rightSidebar = nullptr;
    QStackedWidget *m_rightPanes = nullptr;
    BacklinksPane *m_backlinks = nullptr;
    TagsPane *m_tags = nullptr;
    OutgoingLinksPane *m_outgoingLinks = nullptr;
    QuickSwitcher *m_switcher = nullptr;

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
