// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <PluginManifest.h>

#include <noteappc/plugin.h>

#include <QString>

namespace noteAppC {

/// What loads a plugin's code inside the host process and runs its three
/// hooks. One `notolith-plugin-host` process picks exactly one Engine,
/// decided by `plugin.ini`'s `engine` key (PluginManifest::engine()); the
/// host's main.cpp and the protocol do not care which one is running.
class Engine {
public:
    virtual ~Engine() = default;

    /// Loads the plugin and calls its init() hook. False (with `error` set)
    /// on any failure - a missing library, an unresolved symbol, an API
    /// version mismatch, or init() itself returning non-zero.
    virtual bool load(const PluginManifest &manifest, const noteappc_host *host, QString *error) = 0;
    /// Calls the plugin's shutdown() hook, if it has one. Never fails; the
    /// host process exits either way once this returns.
    virtual void unload() = 0;

    /// `commandID` is bare, as declared in plugin.ini. Returns the plugin's
    /// own return code (0 == success); a language/engine that has no such
    /// command at all is a caller error (main.cpp only calls this for a
    /// command the manifest declared) and returns non-zero here too.
    virtual int runCommand(const QString &commandID) = 0;

    /// Fills `out` with either `out->text` or `out->image` (never both) and
    /// returns 0 on success. Ownership of anything malloc'd into `out`
    /// passes to the caller, which frees it after copying the bytes into a
    /// PluginMessage.
    virtual int renderCodeBlock(const QString &language, const QString &source, int widthPx,
                                float devicePixelRatio, noteappc_block_result *out) = 0;

    /// Fire-and-forget; no return value crosses back to the app.
    virtual void vaultEvent(const QString &event, const QString &path) = 0;
};

} // namespace noteAppC
