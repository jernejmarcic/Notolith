#pragma once

#include <QHash>
#include <QString>

namespace noteAppC {

/// Icons a writer gives files and folders in the file explorer and on tabs.
/// UI only: kept in noteAppC's own `vaults/<digest>/icons.json` under the
/// config directory, never in a file name and never in the vault. Keys are
/// vault-relative paths, files with their extension; a value is a bundled
/// icon id or one character.
class FileIcons {
public:
    explicit FileIcons(const QString &vaultRoot = {});
    static QString storagePathFor(const QString &vaultRoot);

    void load(); ///< a missing file is no icons
    bool save(QString *error = nullptr) const;

    QString iconFor(const QString &relativePath) const { return m_icons.value(relativePath); }
    void set(const QString &relativePath, const QString &icon); ///< empty removes it
    /// A rename or move: the path itself and everything under it.
    void renamePrefix(const QString &oldRelative, const QString &newRelative);
    /// A delete: the path itself and everything under it.
    void removeUnder(const QString &relativePath);
    bool isEmpty() const { return m_icons.isEmpty(); }

private:
    QString m_root;
    QHash<QString, QString> m_icons;
};

} // namespace noteAppC
