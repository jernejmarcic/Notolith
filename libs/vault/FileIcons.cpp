#include "FileIcons.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

#include "Settings.h"

namespace noteAppC {

namespace {

bool isUnder(const QString &path, const QString &prefix)
{
    return path == prefix || (path.startsWith(prefix) && path.at(prefix.size()) == QLatin1Char('/'));
}

} // namespace

FileIcons::FileIcons(const QString &vaultRoot)
    : m_root(vaultRoot)
{
}

QString FileIcons::storagePathFor(const QString &vaultRoot)
{
    // The digest Bookmarks uses, so a vault's files sit in one directory.
    const QString root = QDir(vaultRoot).absolutePath();
    const QByteArray digest =
        QCryptographicHash::hash(root.toUtf8(), QCryptographicHash::Sha256).toHex().left(16);
    return QDir(Settings::configDirectory())
        .filePath(QStringLiteral("vaults/%1/icons.json").arg(QString::fromLatin1(digest)));
}

void FileIcons::load()
{
    m_icons.clear();
    QFile file(storagePathFor(m_root));
    if (!file.open(QIODevice::ReadOnly))
        return;
    const QJsonObject icons = QJsonDocument::fromJson(file.readAll()).object().value(QLatin1String("icons")).toObject();
    for (auto it = icons.begin(); it != icons.end(); ++it) {
        const QString icon = it.value().toString();
        if (!it.key().isEmpty() && !icon.isEmpty())
            m_icons.insert(it.key(), icon);
    }
}

bool FileIcons::save(QString *error) const
{
    const QString path = storagePathFor(m_root);
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
        if (error)
            *error = QStringLiteral("cannot create %1").arg(QFileInfo(path).absolutePath());
        return false;
    }
    QJsonObject icons;
    for (auto it = m_icons.cbegin(); it != m_icons.cend(); ++it)
        icons.insert(it.key(), it.value());
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error)
            *error = file.errorString();
        return false;
    }
    file.write(QJsonDocument(QJsonObject{{QStringLiteral("icons"), icons}}).toJson(QJsonDocument::Indented));
    if (!file.commit()) {
        if (error)
            *error = file.errorString();
        return false;
    }
    return true;
}

void FileIcons::set(const QString &relativePath, const QString &icon)
{
    if (icon.isEmpty())
        m_icons.remove(relativePath);
    else
        m_icons.insert(relativePath, icon);
}

void FileIcons::renamePrefix(const QString &oldRelative, const QString &newRelative)
{
    QHash<QString, QString> moved;
    for (auto it = m_icons.begin(); it != m_icons.end();) {
        if (isUnder(it.key(), oldRelative)) {
            moved.insert(newRelative + it.key().mid(oldRelative.size()), it.value());
            it = m_icons.erase(it);
        } else {
            ++it;
        }
    }
    m_icons.insert(moved);
}

void FileIcons::removeUnder(const QString &relativePath)
{
    m_icons.removeIf([&](const auto &entry) { return isUnder(entry.key(), relativePath); });
}

} // namespace noteAppC
