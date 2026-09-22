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
    explicit MainWindow(QWidget *parent = nullptr);

    void openVault(const QString &path);
    void openNote(const QString &path);
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
    void onTreeActivated(const QModelIndex &index);
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

    QStackedWidget *m_mainStack = nullptr; ///< index 0: notes; index 1: settings
    SettingsPage *m_settingsPage = nullptr;
};

} // namespace noteAppC
