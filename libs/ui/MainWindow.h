#pragma once

#include <QMainWindow>

#include "Index.h"
#include "Indexer.h"
#include "MarkdownEditor.h"
#include "Theme.h"
#include "Vault.h"

class QFileSystemModel;
class QLabel;
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
class TagsPane;

/// Phase 0 shell, wearing Obsidian's chrome: ribbon, file explorer, tab strip,
/// inline title, status bar, dark by default. The workspace splits, command
/// palette and settings dialog are Phase 4; the editor engine is Phase 2.
class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);

    void openVault(const QString &path);
    void openNote(const QString &path);

    const Theme &theme() const { return m_theme; }
    void setThemeMode(Theme::Mode mode);
    void toggleThemeMode();
    void toggleSidebar();
    void toggleRightSidebar();
    void openQuickSwitcher();
    /// Obsidian's Ctrl+E: live preview <-> source.
    void toggleEditingMode();
    /// Index for the open vault, so views can query links and tags.
    Index *index() { return &m_index; }

protected:
    void closeEvent(QCloseEvent *event) override;

public slots:
    /// Obsidian's Ctrl+E, exposed so tools and tests can drive it.
    void toggleEditingModeSlot() { toggleEditingMode(); }

private slots:
    void chooseVault();
    bool save();
    void onTreeActivated(const QModelIndex &index);
    void onModificationChanged(bool modified);
    void onTextChanged();

private:
    QWidget *buildRibbon();
    QWidget *buildSidebar();
    QWidget *buildMainArea();
    QWidget *buildRightSidebar();
    void buildActions();
    void applyTheme();
    /// Offers to save a modified note. False means the user cancelled.
    bool maybeSave();
    void updateTitle();
    void updateWordCount();
    void onIndexChanged();
    /// Opens a vault-relative path from the index or the switcher.
    void openRelative(const QString &relativePath);
    QToolButton *iconButton(const QString &objectName, const QString &icon,
                            const QString &tooltip, int size);
    /// One icon-only button in the right sidebar's tab strip; muted, accented
    /// while its pane is showing.
    QToolButton *rightSidebarTabButton(QWidget *parent, const QString &objectName,
                                       const QString &icon, const QString &tooltip, int index);
    void updateTabButtonIcon(QToolButton *button);

    Theme m_theme = Theme::builtin(Theme::Mode::Dark);
    Vault m_vault;
    Index m_index;
    Indexer *m_indexer = nullptr;
    Note m_note;        ///< line endings and BOM of the open file
    QString m_notePath; ///< absolute path, empty when nothing is open

    QFileSystemModel *m_files = nullptr;
    QTreeView *m_tree = nullptr;
    MarkdownEditor *m_editor = nullptr;
    QTabBar *m_tabs = nullptr;
    QLabel *m_inlineTitle = nullptr;
    QLabel *m_vaultName = nullptr;
    QLabel *m_wordCount = nullptr;
    QLabel *m_vaultLabel = nullptr;
    QWidget *m_sidebar = nullptr;
    QWidget *m_rightSidebar = nullptr;
    QStackedWidget *m_rightPanes = nullptr;
    BacklinksPane *m_backlinks = nullptr;
    TagsPane *m_tags = nullptr;
    OutgoingLinksPane *m_outgoingLinks = nullptr;
    QuickSwitcher *m_switcher = nullptr;
    QToolButton *m_themeButton = nullptr;
};

} // namespace noteAppC
