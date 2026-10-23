#pragma once

#include <functional>
#include <optional>

#include <QHash>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>

class QPluginLoader;

namespace noteAppC {

struct ModuleHost;

/// One module's manifest (Q_PLUGIN_METADATA JSON), read without loading it.
struct ModuleInfo {
    QString id;
    QString featureID;
    QString name;
    QString description;
    QString path; ///< absolute path of the .so; empty for a static module
    /// Linked into the executable as a Qt static plugin (NOTEAPPC_STATIC_MODULES, the Android build: no
    /// directory to scan, nothing to dlopen). Found through QPluginLoader::staticPlugins().
    bool isStatic = false;
    QString ABItag;
    QStringList suffixes;      ///< lower-case, no dot
    struct Command {
        QString id;
        QString title;
    };
    QList<Command> commands;
    /// Empty when the module can be loaded; otherwise the notice Settings
    /// shows ("built for another Notolith", "claims a suffix another module owns").
    QString problem;
};

/// Discovers modules (metadata only, any thread), answers which module owns a
/// suffix or a command, and loads a module on first use (UI thread).
class ModuleRegistry : public QObject {
    Q_OBJECT

public:
    explicit ModuleRegistry(QObject *parent = nullptr);
    ~ModuleRegistry() override;

    /// PROJECT_VERSION "-" a random per-build-directory token "-" Qt6_VERSION: same build only.
    static QString ABItag();
    /// NOTEAPPC_MODULE_DIRS (':'-separated) when set - the tests and the bench - else <libdir>/noteAppC/modules alone.
    static QStringList defaultDirectories();
    using SuffixPredicate = std::function<bool(const QString &suffix)>;
    /// Reads every *.so's manifest with QPluginLoader::metaData(), which does
    /// not load the library, then the static plugins' (same IID check, same manifest; nothing is
    /// constructed). Files without a Notolith manifest are skipped.
    /// A module that claims a suffix `isBuiltInSuffix` accepts gets a
    /// `problem` (the app's own kinds keep their suffixes) and never loads.
    static QList<ModuleInfo> scan(const QStringList &directories, const SuffixPredicate &isBuiltInSuffix = {});

    void setModules(const QList<ModuleInfo> &modules);
    const QList<ModuleInfo> &modules() const { return m_modules; }

    /// The feature switch: MainWindow::featureOn. Without one every module is on.
    void setEnabledCheck(std::function<bool(const QString &featureID)> check);
    bool isEnabled(const ModuleInfo &module) const;

    /// Enabled, loadable modules only: a disabled module owns nothing. A copy, not a
    /// pointer into the registry's list.
    std::optional<ModuleInfo> moduleForSuffix(const QString &path) const;
    std::optional<ModuleInfo> moduleForCommand(const QString &commandID) const;
    /// The enabled, loadable module that owns a feature switch (the share module is asked for by feature: it owns no suffix or command).
    std::optional<ModuleInfo> moduleForFeature(const QString &featureID) const;

    /// What ensureLoaded last said about a module that failed to load (the loader's
    /// message, or "not a Notolith module"); empty when it has not failed.
    QString loadFailure(const QString &id) const { return m_failed.value(id); }

    /// QPluginLoader::instance() (QStaticPlugin::instance() for a static module) then attach(host), once per module id. False
    /// with `error` (the manifest's problem, or the loader's message) when it
    /// cannot. A loaded library stays loaded until the process ends.
    bool ensureLoaded(const ModuleInfo &module, ModuleHost &host, QString *error);
    bool isLoaded(const QString &id) const { return m_loaded.contains(id); }

private:
    QList<ModuleInfo> m_modules;
    QHash<QString, QPluginLoader *> m_loaded; ///< null for a static module
    QHash<QString, QString> m_failed;
    std::function<bool(const QString &)> m_enabled;
};

} // namespace noteAppC
