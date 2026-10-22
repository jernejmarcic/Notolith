#include "ModuleRegistry.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QLibrary>
#include <QPluginLoader>
#include <QSet>
#include <QStandardPaths>
#include <QtGlobal>

#include "ModuleHost.h"
#include "NotolithModule.h"

namespace noteAppC {

ModuleRegistry::ModuleRegistry(QObject *parent) : QObject(parent) {}

ModuleRegistry::~ModuleRegistry() = default; // loaders are parented to this; libraries stay mapped

QString ModuleRegistry::ABItag()
{
    return QStringLiteral(NOTEAPPC_VERSION "-" NOTEAPPC_GIT_HASH "-" QT_VERSION_STR);
}

QStringList ModuleRegistry::defaultDirectories()
{
    const QByteArray override = qgetenv("NOTEAPPC_MODULE_DIRS");
    if (!override.isEmpty())
        return QString::fromLocal8Bit(override).split(QLatin1Char(':'), Qt::SkipEmptyParts);
    return {QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + QStringLiteral("/noteAppC/modules"),
            QStringLiteral(NOTEAPPC_MODULE_LIBDIR)};
}

QList<ModuleInfo> ModuleRegistry::scan(const QStringList &directories, const SuffixPredicate &isBuiltInSuffix)
{
    QList<ModuleInfo> found;
    for (const QString &directory : directories) {
        const QFileInfoList files = QDir(directory).entryInfoList({QStringLiteral("*.so")}, QDir::Files, QDir::Name);
        for (const QFileInfo &file : files) {
            // metaData() reads the .qtmetadata section from the file; it does not dlopen.
            QPluginLoader loader(file.absoluteFilePath());
            const QJsonObject outer = loader.metaData();
            if (outer.value(QStringLiteral("IID")).toString() != QLatin1String(NotolithModule_iid))
                continue; // not a Notolith module: some other library, or not a plugin at all
            const QJsonObject meta = outer.value(QStringLiteral("MetaData")).toObject();
            ModuleInfo info;
            info.id = meta.value(QStringLiteral("id")).toString();
            if (info.id.isEmpty())
                continue;
            info.featureID = meta.value(QStringLiteral("feature")).toString(info.id);
            info.name = meta.value(QStringLiteral("name")).toString(info.id);
            info.description = meta.value(QStringLiteral("description")).toString();
            info.path = file.absoluteFilePath();
            info.ABItag = meta.value(QStringLiteral("abi")).toString();
            for (const QJsonValue &suffix : meta.value(QStringLiteral("suffixes")).toArray())
                info.suffixes << suffix.toString().toLower();
            for (const QJsonValue &suffix : meta.value(QStringLiteral("indexSuffixes")).toArray())
                info.indexSuffixes << suffix.toString().toLower();
            for (const QJsonValue &dependency : meta.value(QStringLiteral("dependencies")).toArray())
                info.dependencies << dependency.toString();
            for (const QJsonValue &command : meta.value(QStringLiteral("commands")).toArray()) {
                const QJsonObject object = command.toObject();
                info.commands.append({object.value(QStringLiteral("id")).toString(),
                                      object.value(QStringLiteral("title")).toString()});
            }
            if (info.ABItag != ABItag()) {
                info.problem = tr("Module %1 was built for another Notolith (%2; this one is %3) and was not loaded.")
                                   .arg(info.id, info.ABItag, ABItag());
            } else if (isBuiltInSuffix) {
                for (const QString &suffix : std::as_const(info.suffixes)) {
                    if (isBuiltInSuffix(suffix)) {
                        info.problem = tr("Module %1 claims the built-in file type .%2 and was not loaded.").arg(info.id, suffix);
                        break;
                    }
                }
            }
            found << info;
        }
    }
    QSet<QString> ids;
    for (const ModuleInfo &info : found)
        if (info.problem.isEmpty())
            ids.insert(info.id);
    for (ModuleInfo &info : found) {
        for (const QString &dependency : info.dependencies) {
            if (!ids.contains(dependency) && info.problem.isEmpty())
                info.problem = tr("Module %1 needs module %2, which is not installed, and was not loaded.")
                                   .arg(info.id, dependency);
        }
    }
    return found;
}

void ModuleRegistry::setModules(const QList<ModuleInfo> &modules) { m_modules = modules; }

void ModuleRegistry::setEnabledCheck(std::function<bool(const QString &)> check) { m_enabled = std::move(check); }

bool ModuleRegistry::isEnabled(const ModuleInfo &module) const
{
    return !m_enabled || m_enabled(module.featureID);
}

const ModuleInfo *ModuleRegistry::moduleForSuffix(const QString &path) const
{
    const QString suffix = QFileInfo(path).suffix().toLower();
    if (suffix.isEmpty())
        return nullptr;
    for (const ModuleInfo &module : m_modules)
        if (module.problem.isEmpty() && isEnabled(module) && module.suffixes.contains(suffix))
            return &module;
    return nullptr;
}

const ModuleInfo *ModuleRegistry::moduleForCommand(const QString &commandID) const
{
    for (const ModuleInfo &module : m_modules) {
        if (!module.problem.isEmpty() || !isEnabled(module))
            continue;
        for (const ModuleInfo::Command &command : module.commands)
            if (command.id == commandID)
                return &module;
    }
    return nullptr;
}

bool ModuleRegistry::ensureLoaded(const ModuleInfo &module, ModuleHost &host, QString *error)
{
    if (m_loaded.contains(module.id))
        return true;
    if (!module.problem.isEmpty()) {
        if (error)
            *error = module.problem;
        return false;
    }
    auto *loader = new QPluginLoader(module.path, this);
    // Every symbol now: a module missing one of the host's symbols fails here
    // with its name, instead of crashing the first time that symbol is called.
    loader->setLoadHints(QLibrary::ResolveAllSymbolsHint);
    QObject *instance = loader->instance();
    auto *plugin = instance ? qobject_cast<NotolithModule *>(instance) : nullptr;
    if (!plugin) {
        if (error)
            *error = tr("Module %1 could not be loaded: %2").arg(module.id, loader->errorString());
        delete loader;
        return false;
    }
    m_loaded.insert(module.id, loader);
    plugin->attach(host);
    return true;
}

} // namespace noteAppC
