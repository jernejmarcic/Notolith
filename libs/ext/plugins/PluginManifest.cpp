// SPDX-License-Identifier: GPL-3.0-or-later
#include "PluginManifest.h"

#include <QDir>
#include <QFileInfo>
#include <QSettings>

namespace noteAppC {

namespace {

// QSettings reads "a, b" as a QStringList and "a" as a QString; toString()
// on the list is empty, which dropped every plugin's second command.
QStringList splitList(const QVariant &value)
{
    const QString text = value.typeId() == QMetaType::QStringList ? value.toStringList().join(QLatin1Char(','))
                                                                  : value.toString();
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
    // plugin code that needs it.
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
    if (engine != QLatin1String("native")) {
        manifest.m_error = QStringLiteral("plugin.ini in %1: unsupported engine '%2' (only native)").arg(directory, engine);
        return manifest;
    }
    manifest.m_engine = PluginEngineKind::Native;

    manifest.m_commands = splitList(ini.value(QStringLiteral("provides/commands")));
    manifest.m_codeBlockLanguages = splitList(ini.value(QStringLiteral("provides/code-blocks")));
    manifest.m_vaultEvents = splitList(ini.value(QStringLiteral("provides/events")));

    // [commands] is optional titles, not a list: "id = Title" per line,
    // e.g. "summarize = Summarize this note". allKeys() rather than
    // beginGroup()/childKeys() because `ini` is const here, the same reason
    // every other section above reads "section/key" paths directly.
    for (const QString &key : ini.allKeys()) {
        if (key.startsWith(QLatin1String("commands/")))
            manifest.m_commandTitles.insert(key.mid(9), ini.value(key).toString());
    }

    int timeoutMs = ini.value(QStringLiteral("plugin/timeout-ms"), 0).toInt();
    manifest.m_requestTimeoutMs = timeoutMs > 0 ? qBound(1000, timeoutMs, 300000) : 0;
    manifest.m_envPassthrough = splitList(ini.value(QStringLiteral("plugin/env-passthrough")));

    const auto wants = [&ini](const char *key) {
        return ini.value(QLatin1String("permissions/") + QLatin1String(key), false).toBool();
    };
    if (wants("read-notes"))
        manifest.m_permissions.insert(PluginPermission::ReadNotes);
    if (wants("write-notes"))
        manifest.m_permissions.insert(PluginPermission::WriteNotes);
#ifdef NOTEAPPC_HAVE_NETWORK
    if (wants("network"))
        manifest.m_permissions.insert(PluginPermission::Network);
#endif
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
