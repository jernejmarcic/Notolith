#pragma once

#include <functional>

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
    QString path; ///< absolute path of the .so
    QString ABItag;
    QStringList suffixes;      ///< lower-case, no dot
    QStringList indexSuffixes; ///< lower-case, no dot: the module is loaded after the scan when the vault holds such files, so its index reader registers
    QStringList dependencies;  ///< module ids
    struct Command {
        QString id;
        QString title;
    };
    QList<Command> commands;
    /// Empty when the module can be loaded; otherwise the notice Settings
    /// shows ("built for another Notolith", "needs module X").
    QString problem;
};

/// Discovers modules (metadata only, any thread), answers which module owns a
/// suffix or a command, and loads a module on first use (UI thread).
class ModuleRegistry : public QObject {
    Q_OBJECT

public:
    explicit ModuleRegistry(QObject *parent = nullptr);
    ~ModuleRegistry() override;

    /// PROJECT_VERSION "-" git hash "-" QT_VERSION_STR: same build only.
    static QString ABItag();
    /// NOTEAPPC_MODULE_DIRS (':'-separated) when set - the tests and the
    /// bench - else $XDG_DATA_HOME/noteAppC/modules then <libdir>/noteAppC/modules.
    static QStringList defaultDirectories();
    using SuffixPredicate = std::function<bool(const QString &suffix)>;
    /// Reads every *.so's manifest with QPluginLoader::metaData(), which does
    /// not load the library. Files without a Notolith manifest are skipped.
    /// A module that claims a suffix `isBuiltInSuffix` accepts gets a
    /// `problem` (the app's own kinds keep their suffixes) and never loads.
    static QList<ModuleInfo> scan(const QStringList &directories, const SuffixPredicate &isBuiltInSuffix = {});

    void setModules(const QList<ModuleInfo> &modules);
    const QList<ModuleInfo> &modules() const { return m_modules; }

    /// The feature switch: MainWindow::featureOn. Without one every module is on.
    void setEnabledCheck(std::function<bool(const QString &featureID)> check);
    bool isEnabled(const ModuleInfo &module) const;

    /// Enabled, loadable modules only: a disabled module owns nothing.
    const ModuleInfo *moduleForSuffix(const QString &path) const;
    const ModuleInfo *moduleForCommand(const QString &commandID) const;

    /// QPluginLoader::instance() then attach(host), once per module id. False
    /// with `error` (the manifest's problem, or the loader's message) when it
    /// cannot. A loaded library stays loaded until the process ends.
    bool ensureLoaded(const ModuleInfo &module, ModuleHost &host, QString *error);
    bool isLoaded(const QString &id) const { return m_loaded.contains(id); }

private:
    QList<ModuleInfo> m_modules;
    QHash<QString, QPluginLoader *> m_loaded;
    std::function<bool(const QString &)> m_enabled;
};

} // namespace noteAppC
