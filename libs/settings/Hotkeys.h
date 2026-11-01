// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej Marčič
#pragma once

#include <QHash>
#include <QJsonArray>
#include <QKeySequence>
#include <QList>
#include <QString>

namespace noteAppC {

/// Keyboard shortcuts, in Obsidian's own `hotkeys.json` format: a command id
/// mapped to a list of `{"modifiers": ["Mod", "Shift"], "key": "B"}`, where
/// "Mod" is Ctrl on Linux and an empty list unbinds the command.
///
/// Two files, the later winning per command: the vault's
/// `.obsidian/hotkeys.json`, read when present so an Obsidian user keeps their
/// keys - but only for a vault the person trusts: an untrusted vault could
/// bind "run all code blocks" to Ctrl+S - and Notolith's own
/// `~/.config/notolith/hotkeys.json`, which is the only one ever written. A
/// command in neither keeps its built-in keys.
class Hotkeys {
public:
    using Bindings = QHash<QString, QList<QKeySequence>>;

    /// `vaultRoot` is read only when `vaultTrusted`.
    static Bindings load(const QString &vaultRoot = {}, bool vaultTrusted = false);
    /// noteAppC's own file alone: what the settings page has changed.
    static Bindings loadOwn();
    /// Writes `bindings` as noteAppC's own file. Only what differs from the
    /// built-in keys belongs in it; the caller decides that.
    static bool save(const Bindings &bindings);
    static QString filePath();

    static QList<QKeySequence> fromJSON(const QJsonArray &keys);
    static QJsonArray toJSON(const QList<QKeySequence> &keys);
};

} // namespace noteAppC
