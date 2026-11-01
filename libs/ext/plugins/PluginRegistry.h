// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej
#pragma once

#include "PluginHost.h"
#include "PluginManifest.h"

#include <QHash>
#include <QObject>
#include <QSet>
#include <QString>

namespace noteAppC {

/// One command the palette/hotkeys can bind, assembled from an enabled
/// plugin's manifest. `qualifiedID` is "pluginID:commandID", the form
/// docs/PLUGINS.md's MainWindow snippet names its QActions with;
/// `commandID` is the bare form PluginHost::runCommand wants.
struct PluginCommand {
    QString pluginID;
    QString commandID;
    QString qualifiedID;
    QString pluginName;
    /// From plugin.ini's `[commands]` section, or empty when the plugin
    /// declared none - MainWindow falls back to "pluginName: commandID" in
    /// that case (docs/PLUGINS.md's MainWindow snippet shows both).
    QString title;
};

/// What the Community plugins settings page shows for one installed
/// plugin: what it asked for, what the user actually granted, whether it is
/// switched on, and its last measured timing.
struct PluginStatus {
    PluginManifest manifest;
    bool enabled = false;
    QSet<PluginPermission> grantedPermissions;
    PluginHost::State state = PluginHost::State::Stopped;
    PluginHost::Timing timing;
};

/// Owns every enabled plugin's PluginHost (docs/PLUGINS.md, Tier 2). This is
/// the one object MainWindow talks to - it never touches a QProcess or the
/// wire protocol directly; those are PluginHost's job, one per plugin.
///
/// Nothing here reads a single byte off disk until scan() is called, and
/// scan() itself does nothing when the `community-plugins` feature is off -
/// "off means no plugin.ini is even read" is enforced right here, not by
/// the caller remembering to skip it.
class PluginRegistry : public QObject {
    Q_OBJECT
public:
    explicit PluginRegistry(QObject *parent = nullptr);
    ~PluginRegistry() override;

    /// `~/.local/share/notolith/plugins`: where a plugin is installed for the
    /// user.
    static QString pluginsRoot();
    /// Every directory scan() reads plugins from: pluginsRoot() first, then
    /// `notolith/plugins` under each `$XDG_DATA_DIRS` entry, where a package
    /// installs one for everybody. The first plugin of an id wins.
    static QStringList pluginsRoots();
    /// The registry's own small store of per-plugin enabled/granted state,
    /// `~/.config/notolith/plugins.ini` - never the vault, matching
    /// docs/PLUGINS.md's "Granted permissions are stored in the user's
    /// config... never in the vault".
    static QString configPath();

    /// Reads every `plugins/*/plugin.ini` and the granted-permissions
    /// config. A no-op, reading nothing, when `enabled` is false - call this
    /// once, after the first paint, with the `community-plugins` feature's
    /// current value.
    void scan(bool enabled);

    /// Every installed plugin, enabled or not, for the settings page.
    QList<PluginStatus> installedPlugins() const;
    bool setPluginEnabled(const QString &pluginID, bool enabled);
    bool setPermissionGranted(const QString &pluginID, PluginPermission permission, bool granted);

    /// Every command every *enabled* plugin declared, for MainWindow to
    /// register as QActions once after scan() and again after a plugin is
    /// toggled.
    QList<PluginCommand> commands() const;
    /// language -> owning plugin id, for the editor's code-block renderer
    /// hook. Only one plugin may claim a given language; a later one to
    /// declare it loses and is logged, since two renderers racing for the
    /// same cache key would be a worse bug than one being silently unusable.
    QHash<QString, QString> codeBlockOwners() const;

    /// Supplies the callbacks every PluginHost this registry creates will
    /// use to reach the vault/editor/clipboard. Must be called before the
    /// first runCommand/renderCodeBlock/notifyVaultEvent.
    void setCallbacks(PluginAppCallbacks callbacks) { m_callbacks = std::move(callbacks); }

    void runCommand(const QString &qualifiedCommandID, const PluginHost::ResultCallback &callback);
    void renderCodeBlock(const QString &language, const QString &source, int widthPx,
                         double devicePixelRatio, const QString &theme,
                         const PluginHost::ResultCallback &callback);
    /// Fans out to every enabled plugin that declared `event` in
    /// `[provides] events`, lazily starting each one - "an event it
    /// subscribed to" is one of the three lazy-start triggers
    /// docs/PLUGINS.md names, so a plugin that only wanted vault events and
    /// never a command or code block still starts, but only the first time
    /// its event actually fires.
    void notifyVaultEvent(const QString &event, const QString &path);

    /// Stops every running host - app shutdown.
    void shutdownAll();

private:
    PluginHost *hostFor(const QString &pluginID);
    void loadConfig();
    void saveConfig() const;

    bool m_enabled = false;
    QHash<QString, PluginManifest> m_manifests; ///< id -> manifest, every installed plugin
    QHash<QString, bool> m_pluginEnabled;
    QHash<QString, QSet<PluginPermission>> m_grantedPermissions;
    // Raw, not unique_ptr: QHash's copy-on-write storage needs a copyable
    // value type even for operations (erase(), operator[]) that never
    // actually copy at runtime - a move-only value does not compile. Parented
    // to `this` as a second line of defence; every path that removes one
    // from the map also explicitly deletes it (see hostFor()/shutdownAll()).
    QHash<QString, PluginHost *> m_hosts; ///< created lazily, one per enabled plugin ever used
    PluginAppCallbacks m_callbacks;
};

} // namespace noteAppC
