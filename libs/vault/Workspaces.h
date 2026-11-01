// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej Marčič
#pragma once

#include <QJsonObject>
#include <QString>

#include "ConfigFile.h"

namespace noteAppC {

/// Obsidian's workspace layouts, in its own JSON shape: `workspace.json`,
/// the layout a vault reopens with, and `workspaces.json`, the named ones
/// (`{"workspaces": {name: layout}, "active": name}`).
///
/// Like bookmarks, both are read from the vault's `.obsidian/` only until
/// Notolith has saved its own copy. The open layout is state, under
/// `~/.local/state/notolith/vaults/<digest>/`; the named ones are the user's
/// own, beside the bookmarks in `~/.local/share/notolith/vaults/<digest>/`.
/// The vault's files are never written.
class Workspaces {
public:
    /// The layout to reopen `vaultRoot` with; empty when there is none.
    static QJsonObject current(const QString &vaultRoot);
    static bool saveCurrent(const QString &vaultRoot, const QJsonObject &layout);

    static QJsonObject named(const QString &vaultRoot);
    static bool saveNamed(const QString &vaultRoot, const QJsonObject &all);

private:
    static QJsonObject read(const QString &vaultRoot, const QString &fileName, XDGroot root);
    static bool write(const QString &vaultRoot, const QString &fileName, XDGroot root, const QJsonObject &json);
};

} // namespace noteAppC
