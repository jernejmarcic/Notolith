#pragma once

#include <functional>
#include <optional>

#include <QList>
#include <QPair>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QWidget>

class QTabletEvent;

namespace noteAppC {

class Settings;
class Theme;
class Vault;

/// A tab's widget supplied by a module: what MainWindow::openView needs from
/// a view it cannot name (save, modified, theme), as PdfView and CanvasView
/// each provide under their own names. Its moc is compiled into
/// noteAppC_modules only; a module subclasses it from this header.
class ModuleView : public QWidget {
    Q_OBJECT

public:
    using QWidget::QWidget;
    virtual bool load(const QString &path, QString *error) = 0;
    virtual bool save(QString *error) = 0;
    virtual bool isModified() const = 0;
    virtual void setTheme(const Theme &theme) = 0;
    /// Settings is not a QObject and has no change signal: MainWindow's
    /// settings pass calls this on every module view after a change.
    virtual void applySettings(const Settings &settings) { Q_UNUSED(settings) }
    /// A reload is parsing: save() refuses until it lands, so closing a tab that holds unsaved edits waits for it.
    virtual bool isLoading() const { return false; }
    /// The file behind the view may have changed on disk (the host's watcher saw it, or a rename rewrote the links in it).
    /// A view with nothing unsaved shows what is on disk now; one with unsaved edits keeps them and decides at its next save.
    /// The view tells its own writes from outside ones (it knows what it wrote); the host calls this for either.
    virtual void reloadFromDisk() {}
    /// The window is going away (or the view is): write what the view keeps back in the host's Settings to disk. The
    /// view's own destructor runs after the window's Settings are gone, so the host calls this first.
    virtual void flushSettings() {}

signals:
    void modificationChanged(bool modified);
    /// A link inside the view, resolved by MainWindow::openRelative.
    void noteActivated(const QString &relativePath);
    void statusMessage(const QString &text, int timeoutMs);
};

/// The seams a module attaches to (spec section 6) plus the three
/// registrations (views by suffix, commands, settings rows). Plain
/// std::functions filled by MainWindow; every one is null when the host does
/// not offer it, so a module checks before calling.
struct ModuleHost {
    using ViewFactory = std::function<ModuleView *(const QString &path, QWidget *parent)>;
    using IndexReader = std::function<std::optional<QString>(const QByteArray &bytes)>;
    /// Seam 2: declared for the host struct's shape; wired by Spec 3.
    using TabletHandler = std::function<bool(QTabletEvent *event, const QString &path)>;

    /// A settings row: a toggle when `defaultValue` is a bool, a combo when
    /// `options` is non-empty, a spin box when `maximum > minimum` (a double
    /// spin box with `decimals` places and `step` when `decimals > 0`).
    struct SettingRow {
        QString key;
        QVariant defaultValue;
        QString label;
        QString description;
        QList<QPair<QString, QVariant>> options;
        double minimum = 0;
        double maximum = 0;
        double step = 1;
        int decimals = 0;
    };

    std::function<void(const QStringList &suffixes, ViewFactory factory)> registerView;
    std::function<void(const QString &id, const QString &title, std::function<void()> run)> registerCommand;
    std::function<void(const SettingRow &row)> registerSetting;
    std::function<void(const QStringList &suffixes, IndexReader reader)> registerIndexReader;
    std::function<void(const QStringList &suffixes, TabletHandler handler)> registerTabletHandler;
    /// Shows a view that belongs to no file (the glyph studio) in a new tab.
    std::function<void(ModuleView *view, const QString &title)> showTab;
    std::function<void(const QString &text, int timeoutMs)> showStatus;
    std::function<bool(const QString &absolutePath)> openFile;
    /// The folder of the active note, else the vault root; empty without a vault.
    std::function<QString()> currentFolder;

    Settings *settings = nullptr;
    const Theme *theme = nullptr;
    const Vault *vault = nullptr;
};

} // namespace noteAppC
