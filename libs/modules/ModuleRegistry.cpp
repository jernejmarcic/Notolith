#include "ModuleRegistry.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QLibrary>
#include <QPluginLoader>
#include <QtGlobal>

#include "ModuleABI.h"
#include "ModuleHost.h"
#include "NotolithModule.h"

namespace noteAppC {

ModuleRegistry::ModuleRegistry(QObject *parent) : QObject(parent) {}

ModuleRegistry::~ModuleRegistry() = default; // loaders are parented to this; libraries stay mapped

QString ModuleRegistry::ABItag()
{
    return QStringLiteral(NOTEAPPC_ABI_TAG);
}

QStringList ModuleRegistry::defaultDirectories()
{
    const QByteArray override = qgetenv("NOTEAPPC_MODULE_DIRS");
    if (!override.isEmpty())
        return QString::fromLocal8Bit(override).split(QLatin1Char(':'), Qt::SkipEmptyParts);
    return {QStringLiteral(NOTEAPPC_MODULE_LIBDIR)};
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
            const bool noID = info.id.isEmpty();
            if (noID)
                info.id = file.completeBaseName(); // something to name it by in Settings
            info.featureID = meta.value(QStringLiteral("feature")).toString(info.id);
            info.name = meta.value(QStringLiteral("name")).toString(info.id);
            info.description = meta.value(QStringLiteral("description")).toString();
            info.path = file.absoluteFilePath();
            info.ABItag = meta.value(QStringLiteral("abi")).toString();
            for (const QJsonValue &suffix : meta.value(QStringLiteral("suffixes")).toArray())
                info.suffixes << suffix.toString().toLower();
            for (const QJsonValue &command : meta.value(QStringLiteral("commands")).toArray()) {
                const QJsonObject object = command.toObject();
                info.commands.append({object.value(QStringLiteral("id")).toString(),
                                      object.value(QStringLiteral("title")).toString()});
            }
            if (noID) {
                info.problem = tr("%1: manifest has no id; not loaded.").arg(file.fileName());
            } else if (info.ABItag != ABItag()) {
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
    // First claim wins (M3): the first module without a problem to claim a suffix owns it, in
    // scan order (directories as given, files by name). A later claimant is listed with a
    // problem and never loads, so the registry and the window's view map cannot disagree.
    QHash<QString, QString> owner; // suffix -> id
    for (ModuleInfo &info : found) {
        if (!info.problem.isEmpty())
            continue;
        for (const QString &suffix : std::as_const(info.suffixes)) {
            const auto taken = owner.constFind(suffix);
            if (taken != owner.cend()) {
                info.problem = tr("Module %1 claims .%2, which module %3 already claims, and was not loaded.")
                                   .arg(info.id, suffix, *taken);
                break;
            }
        }
        if (info.problem.isEmpty())
            for (const QString &suffix : std::as_const(info.suffixes))
                owner.insert(suffix, info.id);
    }
    return found;
}

void ModuleRegistry::setModules(const QList<ModuleInfo> &modules) { m_modules = modules; }

void ModuleRegistry::setEnabledCheck(std::function<bool(const QString &)> check) { m_enabled = std::move(check); }

bool ModuleRegistry::isEnabled(const ModuleInfo &module) const
{
    return !m_enabled || m_enabled(module.featureID);
}

std::optional<ModuleInfo> ModuleRegistry::moduleForSuffix(const QString &path) const
{
    const QString suffix = QFileInfo(path).suffix().toLower();
    if (suffix.isEmpty())
        return std::nullopt;
    for (const ModuleInfo &module : m_modules)
        if (module.problem.isEmpty() && isEnabled(module) && module.suffixes.contains(suffix))
            return module;
    return std::nullopt;
}

std::optional<ModuleInfo> ModuleRegistry::moduleForCommand(const QString &commandID) const
{
    for (const ModuleInfo &module : m_modules) {
        if (!module.problem.isEmpty() || !isEnabled(module))
            continue;
        for (const ModuleInfo::Command &command : module.commands)
            if (command.id == commandID)
                return module;
    }
    return std::nullopt;
}

std::optional<ModuleInfo> ModuleRegistry::moduleForFeature(const QString &featureID) const
{
    for (const ModuleInfo &module : m_modules)
        if (module.problem.isEmpty() && isEnabled(module) && module.featureID == featureID)
            return module;
    return std::nullopt;
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
    if (const auto failed = m_failed.constFind(module.id); failed != m_failed.cend()) {
        if (error)
            *error = *failed; // a library that failed to open fails the same way again: not retried
        return false;
    }
    auto *loader = new QPluginLoader(module.path, this);
    // Every symbol now: a module missing one of the host's symbols - or libnotolith-inkcore.so
    // itself - fails here with its name, instead of crashing the first time it is called.
    loader->setLoadHints(QLibrary::ResolveAllSymbolsHint);
    QObject *instance = loader->instance();
    auto *plugin = instance ? qobject_cast<NotolithModule *>(instance) : nullptr;
    if (!plugin) {
        const QString message = instance ? tr("Module %1 is not a Notolith module.").arg(module.id)
                                         : tr("Module %1 could not be loaded: %2").arg(module.id, loader->errorString());
        m_failed.insert(module.id, message);
        if (error)
            *error = message;
        delete loader;
        return false;
    }
    m_loaded.insert(module.id, loader);
    plugin->attach(host);
    return true;
}

} // namespace noteAppC
