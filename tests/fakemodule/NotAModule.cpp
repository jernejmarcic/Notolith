// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej Marčič
// A Qt plugin that carries a Notolith manifest and the Notolith IID but does not
// implement NotolithModule: ModuleRegistry::ensureLoaded must say so (M6).
#include <QObject>
#include <QtPlugin>

#include "NotolithModule.h"

class NotAModule : public QObject {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID NotolithModule_iid FILE "manifest.json")
};

#include "NotAModule.moc"
