// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej Marčič
#pragma once

#include <QHash>
#include <QSet>
#include <QString>
#include <QStringList>

namespace noteAppC {

/// One capability a plugin can ask for in `plugin.ini`'s `[permissions]`
/// section, and the app enforces on every request a host sends
/// (docs/PLUGINS.md, Tier 2). Declaring one in the manifest only means the
/// plugin *wants* it - PluginHost is handed the separately-stored *granted*
/// set, which the user controls from the Community plugins settings page.
enum class PluginPermission { ReadNotes, WriteNotes, Network, Clipboard };

/// Which engine loads the plugin's code. Native (dlopen of a shared library)
/// is the only one; any other `engine =` value makes the manifest invalid.
enum class PluginEngineKind { Native };

/// One plugin directory's `plugin.ini`, parsed but not loaded - reading it
/// costs a handful of small file reads and no dlopen (Tier 2's "how it stays
/// free until used"). `directory()` is what the host is pointed at with
/// `--plugin` and what the app sandboxes read-only.
class PluginManifest {
public:
    /// Parses `directory/plugin.ini`. `isValid()` is false, with error()
    /// set, when the file is missing, unparsable, or missing `id`/`library`.
    static PluginManifest load(const QString &directory);

    bool isValid() const { return m_valid; }
    QString error() const { return m_error; }

    QString directory() const { return m_directory; }
    QString id() const { return m_id; }
    QString name() const { return m_name; }
    QString version() const { return m_version; }
    int apiVersion() const { return m_apiVersion; }
    PluginEngineKind engine() const { return m_engine; }
    /// Absolute path to the shared library, `directory()/library`.
    QString libraryPath() const;

    /// Bare command ids (no "pluginID:" prefix - the app adds that when it
    /// registers the QAction, see docs/PLUGINS.md's MainWindow snippet).
    const QStringList &commands() const { return m_commands; }
    /// A command's human title from `plugin.ini`'s `[commands]` section
    /// (`id = Title`), or empty when the plugin declared none - the caller
    /// then falls back to "pluginName: commandID" the way it always has
    /// (docs/PLUGINS.md's MainWindow snippet shows both).
    QString commandTitle(const QString &commandID) const { return m_commandTitles.value(commandID); }
    /// Fenced code-block languages this plugin renders.
    const QStringList &codeBlockLanguages() const { return m_codeBlockLanguages; }
    /// Vault events this plugin wants ("note-opened", "note-saved",
    /// "note-renamed") - empty unless `[provides] events = ...` lists them,
    /// so a plugin that never asked is never woken for one.
    const QStringList &vaultEvents() const { return m_vaultEvents; }

    /// Declared in `[permissions]`, i.e. requested - not necessarily
    /// granted. Shown to the user before they enable the plugin.
    const QSet<PluginPermission> &requestedPermissions() const { return m_permissions; }

    static QString permissionID(PluginPermission permission);

    /// The hard per-request kill timeout PluginHost should use for this
    /// plugin (command.run and codeBlock.render alike), from `[plugin]
    /// timeout-ms`, already clamped to [1000, 300000] - 0 means "not
    /// declared", and PluginHost keeps its own 4000ms default in that case.
    /// docs/PLUGINS.md, "Timeouts": a plugin whose work is inherently slow
    /// (an AI plugin waiting on a language model) declares a realistic
    /// budget instead of a command meant to finish in milliseconds forcing
    /// one on it.
    int requestTimeoutMs() const { return m_requestTimeoutMs; }

    /// Environment variable NAMES (never values - this is metadata, not a
    /// secret) this plugin may read inside its sandboxed process, from
    /// `[plugin] env-passthrough`. PluginHost::sandboxArguments() clears the
    /// child's entire environment before forwarding exactly these, and only
    /// once the plugin's `network` permission is granted - see its comment
    /// for why this exists (docs/PLUGINS.md's AI plugin reads an API key
    /// this way, never from a file).
    const QStringList &envPassthrough() const { return m_envPassthrough; }

private:
    bool m_valid = false;
    QString m_error;
    QString m_directory;
    QString m_id;
    QString m_name;
    QString m_version;
    int m_apiVersion = 0;
    PluginEngineKind m_engine = PluginEngineKind::Native;
    QString m_library;
    QStringList m_commands;
    QStringList m_codeBlockLanguages;
    QStringList m_vaultEvents;
    QSet<PluginPermission> m_permissions;
    QHash<QString, QString> m_commandTitles;
    int m_requestTimeoutMs = 0;
    QStringList m_envPassthrough;
};

} // namespace noteAppC
