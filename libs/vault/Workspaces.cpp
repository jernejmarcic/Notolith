#include "Workspaces.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSaveFile>

#include "Bookmarks.h"

namespace noteAppC {

namespace {

/// The per-vault directory noteAppC keeps its own copies in.
QString ownDirectory(const QString &vaultRoot)
{
    return QFileInfo(Bookmarks::storagePathFor(vaultRoot)).absolutePath();
}

QJsonObject readJSON(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return QJsonDocument::fromJson(file.readAll()).object();
}

} // namespace

QJsonObject Workspaces::read(const QString &vaultRoot, const QString &fileName)
{
    const QString own = QDir(ownDirectory(vaultRoot)).filePath(fileName);
    if (QFileInfo::exists(own))
        return readJSON(own);
    return readJSON(QDir(vaultRoot).filePath(QStringLiteral(".obsidian/") + fileName));
}

bool Workspaces::write(const QString &vaultRoot, const QString &fileName, const QJsonObject &json)
{
    const QString dir = ownDirectory(vaultRoot);
    if (!QDir().mkpath(dir))
        return false;
    QSaveFile file(QDir(dir).filePath(fileName));
    if (!file.open(QIODevice::WriteOnly))
        return false;
    file.write(QJsonDocument(json).toJson(QJsonDocument::Indented));
    return file.commit();
}

QJsonObject Workspaces::current(const QString &vaultRoot)
{
    return read(vaultRoot, QStringLiteral("workspace.json"));
}

bool Workspaces::saveCurrent(const QString &vaultRoot, const QJsonObject &layout)
{
    return write(vaultRoot, QStringLiteral("workspace.json"), layout);
}

QJsonObject Workspaces::named(const QString &vaultRoot)
{
    return read(vaultRoot, QStringLiteral("workspaces.json"));
}

bool Workspaces::saveNamed(const QString &vaultRoot, const QJsonObject &all)
{
    return write(vaultRoot, QStringLiteral("workspaces.json"), all);
}

} // namespace noteAppC
