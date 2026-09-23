#include "PluginManifest.h"

#include <QDir>
#include <QFileInfo>
#include <QSettings>

namespace noteAppC {

namespace {

QStringList splitList(const QString &text)
{
    QStringList out;
    for (const QString &item : text.split(QLatin1Char(','), Qt::SkipEmptyParts))
        out << item.trimmed();
    return out;
}

} // namespace

QString PluginManifest::permissionID(PluginPermission permission)
{
    switch (permission) {
    case PluginPermission::ReadNotes:
        return QStringLiteral("read-notes");
    case PluginPermission::WriteNotes:
        return QStringLiteral("write-notes");
    case PluginPermission::Network:
        return QStringLiteral("network");
    case PluginPermission::Clipboard:
        return QStringLiteral("clipboard");
    }
    return {};
}

PluginManifest PluginManifest::load(const QString &directory)
{
    PluginManifest manifest;
    manifest.m_directory = directory;

    const QString iniPath = QDir(directory).filePath(QStringLiteral("plugin.ini"));
    if (!QFileInfo::exists(iniPath)) {
        manifest.m_error = QStringLiteral("no plugin.ini in %1").arg(directory);
        return manifest;
    }

    // IniFormat rather than a hand-rolled reader (unlike settings.ini, this
    // file has no round-trip-preserving-comments requirement - nothing in
    // the app ever rewrites a plugin's own plugin.ini): QSettings already
    // does section/key parsing correctly and this is the only place in the
    // plugin lane that needs it.
    const QSettings ini(iniPath, QSettings::IniFormat);

    manifest.m_id = ini.value(QStringLiteral("plugin/id")).toString();
    manifest.m_name = ini.value(QStringLiteral("plugin/name"), manifest.m_id).toString();
    manifest.m_version = ini.value(QStringLiteral("plugin/version")).toString();
    manifest.m_apiVersion = ini.value(QStringLiteral("plugin/api"), 0).toInt();
    manifest.m_library = ini.value(QStringLiteral("plugin/library")).toString();
    const QString engine = ini.value(QStringLiteral("plugin/engine"), QStringLiteral("native"))
                               .toString()
                               .trimmed()
                               .toLower();
    manifest.m_engine = engine == QLatin1String("quickjs") ? PluginEngineKind::QuickJS : PluginEngineKind::Native;

    manifest.m_commands = splitList(ini.value(QStringLiteral("provides/commands")).toString());
    manifest.m_codeBlockLanguages = splitList(ini.value(QStringLiteral("provides/code-blocks")).toString());
    manifest.m_vaultEvents = splitList(ini.value(QStringLiteral("provides/events")).toString());

    const auto wants = [&ini](const char *key) {
        return ini.value(QLatin1String("permissions/") + QLatin1String(key), false).toBool();
    };
    if (wants("read-notes"))
        manifest.m_permissions.insert(PluginPermission::ReadNotes);
    if (wants("write-notes"))
        manifest.m_permissions.insert(PluginPermission::WriteNotes);
    if (wants("network"))
        manifest.m_permissions.insert(PluginPermission::Network);
    if (wants("clipboard"))
        manifest.m_permissions.insert(PluginPermission::Clipboard);

    if (manifest.m_id.isEmpty()) {
        manifest.m_error = QStringLiteral("plugin.ini in %1 has no [plugin] id").arg(directory);
        return manifest;
    }
    if (manifest.m_library.isEmpty()) {
        manifest.m_error = QStringLiteral("plugin.ini for '%1' has no [plugin] library").arg(manifest.m_id);
        return manifest;
    }

    manifest.m_valid = true;
    return manifest;
}

QString PluginManifest::libraryPath() const
{
    return QDir(m_directory).filePath(m_library);
}

} // namespace noteAppC
