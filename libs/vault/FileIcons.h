// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QHash>
#include <QString>

namespace noteAppC {

/// Icons a writer gives files and folders in the file explorer and on tabs.
/// UI only: kept in Notolith's own `vaults/<digest>/icons.json` under the
/// data directory (`~/.local/share/notolith`), never in a file name and never in the vault. Keys are
/// vault-relative paths, files with their extension; a value is a bundled
/// icon id or one character.
class FileIcons {
public:
    explicit FileIcons(const QString &vaultRoot = {});
    static QString storagePathFor(const QString &vaultRoot);
    /// One character as a reader sees it, which may be several code points:
    /// an emoji with a variation selector, a skin tone, a flag.
    static bool isGlyph(const QString &text);

    /// A missing file is no icons; entries whose file or folder is gone are
    /// dropped (moved or deleted outside the app), so a new file at that path
    /// does not inherit one.
    void load();
    bool save(QString *error = nullptr) const;

    QString iconFor(const QString &relativePath) const { return m_icons.value(relativePath); }
    void set(const QString &relativePath, const QString &icon); ///< empty removes it
    /// A rename or move: the path itself and everything under it. True when
    /// an icon moved, so the caller saves only then.
    bool renamePrefix(const QString &oldRelative, const QString &newRelative);
    /// A delete: the path itself and everything under it. True when one went.
    bool removeUnder(const QString &relativePath);
    bool isEmpty() const { return m_icons.isEmpty(); }

private:
    QString m_root;
    QHash<QString, QString> m_icons;
};

} // namespace noteAppC
