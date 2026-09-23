#include "PluginRegistry.h"

#include <QDir>
#include <QSettings>

#include <utility>

namespace noteAppC {

namespace {

QString permissionConfigKey(PluginPermission permission)
{
    switch (permission) {
    case PluginPermission::ReadNotes:
        return QStringLiteral("readNotes");
    case PluginPermission::WriteNotes:
        return QStringLiteral("writeNotes");
    case PluginPermission::Network:
        return QStringLiteral("network");
    case PluginPermission::Clipboard:
        return QStringLiteral("clipboard");
    }
    return {};
}

constexpr PluginPermission kAllPermissions[] = {PluginPermission::ReadNotes, PluginPermission::WriteNotes,
                                                PluginPermission::Network, PluginPermission::Clipboard};

} // namespace

PluginRegistry::PluginRegistry(QObject *parent) : QObject(parent) { }

PluginRegistry::~PluginRegistry()
{
    shutdownAll();
}

QString PluginRegistry::pluginsRoot()
{
    QString base = qEnvironmentVariable("XDG_DATA_HOME");
    if (base.isEmpty())
        base = QDir::homePath() + QStringLiteral("/.local/share");
    return base + QStringLiteral("/noteAppC/plugins");
}

QString PluginRegistry::configPath()
{
    QString base = qEnvironmentVariable("XDG_CONFIG_HOME");
    if (base.isEmpty())
        base = QDir::homePath() + QStringLiteral("/.config");
    return base + QStringLiteral("/noteAppC/plugins.ini");
}

void PluginRegistry::loadConfig()
{
    const QSettings config(configPath(), QSettings::IniFormat);
    for (const QString &group : config.childGroups()) {
        m_pluginEnabled.insert(group, config.value(group + QStringLiteral("/enabled"), false).toBool());
        QSet<PluginPermission> granted;
        for (const PluginPermission permission : kAllPermissions) {
            const QString key = group + QStringLiteral("/permissions.") + permissionConfigKey(permission);
            if (config.value(key, false).toBool())
                granted.insert(permission);
        }
        m_grantedPermissions.insert(group, granted);
    }
}

void PluginRegistry::saveConfig() const
{
    QSettings config(configPath(), QSettings::IniFormat);
    config.clear();
    for (auto it = m_manifests.constBegin(); it != m_manifests.constEnd(); ++it) {
        const QString &id = it.key();
        config.setValue(id + QStringLiteral("/enabled"), m_pluginEnabled.value(id, false));
        const QSet<PluginPermission> granted = m_grantedPermissions.value(id);
        for (const PluginPermission permission : kAllPermissions) {
            config.setValue(id + QStringLiteral("/permissions.") + permissionConfigKey(permission),
                            granted.contains(permission));
        }
    }
}

void PluginRegistry::scan(bool enabled)
{
    m_enabled = enabled;
    if (!enabled)
        return; // "off means no plugin.ini is even read" - not even the directory is opened

    const QDir root(pluginsRoot());
    for (const QString &entry :
        root.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
        PluginManifest manifest = PluginManifest::load(root.filePath(entry));
        if (!manifest.isValid()) {
            if (m_callbacks.log)
                m_callbacks.log(entry, QStringLiteral("skipped: ") + manifest.error());
            continue;
        }
        m_manifests.insert(manifest.id(), manifest);
    }
    loadConfig();
}

QList<PluginStatus> PluginRegistry::installedPlugins() const
{
    QList<PluginStatus> out;
    for (auto it = m_manifests.constBegin(); it != m_manifests.constEnd(); ++it) {
        PluginStatus status;
        status.manifest = it.value();
        status.enabled = m_pluginEnabled.value(it.key(), false);
        status.grantedPermissions = m_grantedPermissions.value(it.key());
        if (const auto hostIt = m_hosts.constFind(it.key()); hostIt != m_hosts.constEnd() && hostIt.value()) {
            status.state = hostIt.value()->state();
            status.timing = hostIt.value()->timing();
        }
        out << status;
    }
    return out;
}

bool PluginRegistry::setPluginEnabled(const QString &pluginID, bool enabled)
{
    if (!m_manifests.contains(pluginID))
        return false;
    m_pluginEnabled.insert(pluginID, enabled);
    if (!enabled) {
        if (const auto it = m_hosts.find(pluginID); it != m_hosts.end() && it.value()) {
            PluginHost *host = it.value();
            host->stop();
            m_hosts.erase(it);
            delete host;
        }
    }
    saveConfig();
    return true;
}

bool PluginRegistry::setPermissionGranted(const QString &pluginID, PluginPermission permission, bool granted)
{
    if (!m_manifests.contains(pluginID))
        return false;
    QSet<PluginPermission> &set = m_grantedPermissions[pluginID];
    if (granted)
        set.insert(permission);
    else
        set.remove(permission);
    // A running host was started with the old grant baked into its "init"
    // handshake; the simplest correct thing is to make the next request
    // restart it rather than trying to renegotiate permissions live.
    if (const auto it = m_hosts.find(pluginID); it != m_hosts.end() && it.value()) {
        PluginHost *host = it.value();
        host->stop();
        m_hosts.erase(it);
        delete host;
    }
    saveConfig();
    return true;
}

QList<PluginCommand> PluginRegistry::commands() const
{
    QList<PluginCommand> out;
    for (auto it = m_manifests.constBegin(); it != m_manifests.constEnd(); ++it) {
        if (!m_pluginEnabled.value(it.key(), false))
            continue;
        for (const QString &commandID : it.value().commands()) {
            PluginCommand command;
            command.pluginID = it.key();
            command.commandID = commandID;
            command.qualifiedID = it.key() + QLatin1Char(':') + commandID;
            command.pluginName = it.value().name();
            command.title = it.value().commandTitle(commandID);
            out << command;
        }
    }
    return out;
}

QHash<QString, QString> PluginRegistry::codeBlockOwners() const
{
    QHash<QString, QString> out;
    for (auto it = m_manifests.constBegin(); it != m_manifests.constEnd(); ++it) {
        if (!m_pluginEnabled.value(it.key(), false))
            continue;
        for (const QString &language : it.value().codeBlockLanguages()) {
            if (out.contains(language)) {
                if (m_callbacks.log)
                    m_callbacks.log(it.key(), QStringLiteral("code-block language '%1' already claimed by '%2', ignored")
                                                  .arg(language, out.value(language)));
                continue;
            }
            out.insert(language, it.key());
        }
    }
    return out;
}

PluginHost *PluginRegistry::hostFor(const QString &pluginID)
{
    if (!m_enabled || !m_pluginEnabled.value(pluginID, false))
        return nullptr;
    const auto it = m_manifests.constFind(pluginID);
    if (it == m_manifests.constEnd())
        return nullptr;
    PluginHost *&host = m_hosts[pluginID];
    if (!host)
        host = new PluginHost(it.value(), m_grantedPermissions.value(pluginID), m_callbacks, this);
    return host;
}

void PluginRegistry::runCommand(const QString &qualifiedCommandID, const PluginHost::ResultCallback &callback)
{
    const qsizetype colon = qualifiedCommandID.indexOf(QLatin1Char(':'));
    if (colon < 0) {
        if (callback)
            callback(false, {}, QStringLiteral("not a plugin command: ") + qualifiedCommandID);
        return;
    }
    const QString pluginID = qualifiedCommandID.left(colon);
    const QString commandID = qualifiedCommandID.mid(colon + 1);
    PluginHost *host = hostFor(pluginID);
    if (!host) {
        if (callback)
            callback(false, {}, QStringLiteral("plugin '%1' is not enabled").arg(pluginID));
        return;
    }
    host->runCommand(commandID, callback);
}

void PluginRegistry::renderCodeBlock(const QString &language, const QString &source, int widthPx,
                                     double devicePixelRatio, const QString &theme,
                                     const PluginHost::ResultCallback &callback)
{
    const QString pluginID = codeBlockOwners().value(language);
    PluginHost *host = pluginID.isEmpty() ? nullptr : hostFor(pluginID);
    if (!host) {
        if (callback)
            callback(false, {}, QStringLiteral("no enabled plugin renders '%1' blocks").arg(language));
        return;
    }
    host->renderCodeBlock(language, source, widthPx, devicePixelRatio, theme, callback);
}

void PluginRegistry::notifyVaultEvent(const QString &event, const QString &path)
{
    if (!m_enabled)
        return;
    for (auto it = m_manifests.constBegin(); it != m_manifests.constEnd(); ++it) {
        if (!m_pluginEnabled.value(it.key(), false) || !it.value().vaultEvents().contains(event))
            continue;
        PluginHost *host = hostFor(it.key());
        if (!host)
            continue;
        host->ensureStarted();
        host->notifyVaultEvent(event, path);
    }
}

void PluginRegistry::shutdownAll()
{
    for (PluginHost *host : std::as_const(m_hosts)) {
        if (!host)
            continue;
        host->stop();
        delete host;
    }
    m_hosts.clear();
}

} // namespace noteAppC
