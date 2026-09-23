#pragma once

#include <QKeySequence>
#include <QWidget>

#include <functional>

class QVBoxLayout;
class QListWidget;
class QStackedWidget;
class QLabel;
class QToolButton;
class QKeyEvent;
class QShowEvent;

namespace noteAppC {

class Settings;
class Theme;

/// The settings page: left nav (Appearance, Editor, Files and links, About)
/// beside scrolling rows of label+description and a control, read and
/// written straight through the `Settings` MainWindow owns. A page in the
/// main area, not a dialog - MainWindow swaps it in and out.
class SettingsPage : public QWidget {
    Q_OBJECT

public:
    explicit SettingsPage(QWidget *parent = nullptr);

    /// `settings` is not owned; MainWindow keeps it alive and saves it.
    /// Rebuilds every row from scratch, so call again after the vault (and
    /// so the scheme list and "set by this vault" tags) changes.
    /// Points the page at a settings object and a vault. The rows themselves
    /// are not built until the page is shown, because most sessions never
    /// open it.
    void setContext(Settings *settings, const QString &vaultRoot);
    void applyTheme(const Theme &theme);

    /// A command the Hotkeys page can rebind, with its built-in keys.
    struct HotkeyCommand {
        QString id;
        QString name;
        QList<QKeySequence> builtIn;
    };
    void setCommands(const QList<HotkeyCommand> &commands);

signals:
    /// A row wrote to `settings`; MainWindow re-applies live and will save()
    /// on close.
    void settingChanged();
    /// Escape, or the header's close affordance.
    void closeRequested();
    /// The Hotkeys page wrote noteAppC's hotkeys.json.
    void hotkeysChanged();
    /// "View snapshots" was pressed.
    void recoveryRequested();

protected:
    void keyPressEvent(QKeyEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;

private:
    void rebuild();
    bool m_needsRebuild = true;

    QWidget *buildAppearancePage();
    QWidget *buildEditorPage();
    QWidget *buildFilesPage();
    QWidget *buildAboutPage();
    QWidget *buildHotkeysPage();
    QWidget *buildCorePluginsPage();
    /// The keys `id` has now: noteAppC's own file, else the vault's
    /// Obsidian file, else the built-in ones.
    QList<QKeySequence> effectiveKeys(const HotkeyCommand &command) const;
    QList<HotkeyCommand> m_commands;

    /// One row: label + muted description on the left, `control` on the
    /// right, a "Reset" button that appears when any of `keys` differs from
    /// its compiled-in default, a "set by this vault" tag when the vault's
    /// Obsidian config supplied it, and a hairline underneath. `resetControl`
    /// puts `control` back in sync after Settings::reset().
    QWidget *addRow(QVBoxLayout *into, const QStringList &keys, const QString &label,
                    const QString &description, QWidget *control,
                    std::function<void()> resetControl);

    void addToggleRow(QVBoxLayout *into, const QString &key, const QString &label,
                      const QString &description);
    void addComboRow(QVBoxLayout *into, const QString &key, const QString &label,
                     const QString &description,
                     const QList<QPair<QString, QVariant>> &options);
    void addSpinRow(QVBoxLayout *into, const QString &key, const QString &label,
                    const QString &description, int minimum, int maximum,
                    const QString &suffix = {});
    void addLineEditRow(QVBoxLayout *into, const QString &key, const QString &label,
                        const QString &description, const QString &placeholder = {});
    void addAccentRow(QVBoxLayout *into);
    void addSchemeRow(QVBoxLayout *into);
    void addFolderRow(QVBoxLayout *into, const QString &key, const QString &label,
                      const QString &description);

    /// Re-checks every row's Reset/vault-tag visibility; called after any
    /// change so a row that now matches its default hides its own button.
    void refreshRowDecorations();
    void changed();

    Settings *m_settings = nullptr;
    QString m_vaultRoot;

    QListWidget *m_nav = nullptr;
    QStackedWidget *m_pages = nullptr;

    struct Row {
        QStringList keys;
        QToolButton *reset = nullptr;
        QLabel *vaultTag = nullptr;
    };
    QList<Row> m_rows;
};

} // namespace noteAppC
