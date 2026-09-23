#pragma once

#include <QJsonObject>
#include <QString>

namespace noteAppC {

/// Obsidian's workspace layouts, in its own JSON shape: `workspace.json`,
/// the layout a vault reopens with, and `workspaces.json`, the named ones
/// (`{"workspaces": {name: layout}, "active": name}`).
///
/// Like bookmarks, both are read from the vault's `.obsidian/` only until
/// noteAppC has saved its own copy, which lives beside its bookmarks under
/// `~/.config/noteAppC/vaults/<hash>/`. The vault's files are never written.
class Workspaces {
public:
    /// The layout to reopen `vaultRoot` with; empty when there is none.
    static QJsonObject current(const QString &vaultRoot);
    static bool saveCurrent(const QString &vaultRoot, const QJsonObject &layout);

    static QJsonObject named(const QString &vaultRoot);
    static bool saveNamed(const QString &vaultRoot, const QJsonObject &all);

private:
    static QJsonObject read(const QString &vaultRoot, const QString &fileName);
    static bool write(const QString &vaultRoot, const QString &fileName, const QJsonObject &json);
};

} // namespace noteAppC
