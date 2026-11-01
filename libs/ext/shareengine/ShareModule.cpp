// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej Marčič
// The share module: what Notolith dlopens when a vault holds a share or the user shares a note. All it does
// is hand the window the way to build a ShareEngine; the engine is plain Qt and the link layer.
#include <QObject>
#include <QtPlugin>

#include "ModuleHost.h"
#include "NotolithModule.h"
#include "ShareEngine.h"

// Global namespace on purpose: Q_PLUGIN_METADATA and moc want the plugin class plain.
class ShareModule : public QObject, public noteAppC::NotolithModule {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID NotolithModule_iid FILE "manifest.json")
    Q_INTERFACES(noteAppC::NotolithModule)
public:
    // No state of its own: a second window's attach() runs on this same object (NotolithModule.h).
    void attach(noteAppC::ModuleHost &host) override
    {
        if (host.registerShareEngine)
            host.registerShareEngine([](const QString &vaultRoot, const noteAppC::ShareEngineConfig &config, QObject *parent)
                                         -> noteAppC::ShareEngineBase * {
                return new noteAppC::ShareEngine(vaultRoot, config, parent);
            });
    }
};

#include "ShareModule.moc"
