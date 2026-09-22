#pragma once

#include <QMainWindow>

#include "Theme.h"
#include "Vault.h"

class QFileSystemModel;
class QLabel;
class QModelIndex;
class QTabBar;
class QToolButton;
class QTreeView;

namespace noteAppC {

class NoteEdit;

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

protected:
    void closeEvent(QCloseEvent *event) override;

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
    void buildActions();
    void applyTheme();
    /// Offers to save a modified note. False means the user cancelled.
    bool maybeSave();
    void updateTitle();
    void updateWordCount();
    QToolButton *iconButton(const QString &objectName, const QString &icon,
                            const QString &tooltip, int size);

    Theme m_theme = Theme::builtin(Theme::Mode::Dark);
    Vault m_vault;
    Note m_note;        ///< line endings and BOM of the open file
    QString m_notePath; ///< absolute path, empty when nothing is open

    QFileSystemModel *m_files = nullptr;
    QTreeView *m_tree = nullptr;
    NoteEdit *m_editor = nullptr;
    QTabBar *m_tabs = nullptr;
    QLabel *m_inlineTitle = nullptr;
    QLabel *m_vaultName = nullptr;
    QLabel *m_wordCount = nullptr;
    QLabel *m_vaultLabel = nullptr;
    QWidget *m_sidebar = nullptr;
    QToolButton *m_themeButton = nullptr;
};

} // namespace noteAppC
