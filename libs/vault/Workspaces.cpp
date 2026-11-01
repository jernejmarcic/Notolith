#include "Workspaces.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSaveFile>

#include "ConfigFile.h"

namespace noteAppC {

namespace {

/// The per-vault directory Notolith keeps its own copies in: the layout being open right now is state, the
/// named layouts a person made are data.
QString ownDirectory(const QString &vaultRoot, XDGroot root)
{
    return appDir(root, QStringLiteral("vaults/") + vaultDigest(vaultRoot));
}

QJsonObject readJSON(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return QJsonDocument::fromJson(file.readAll()).object();
}

} // namespace

QJsonObject Workspaces::read(const QString &vaultRoot, const QString &fileName, XDGroot root)
{
    const QString own = QDir(ownDirectory(vaultRoot, root)).filePath(fileName);
    if (QFileInfo::exists(own))
        return readJSON(own);
    return readJSON(QDir(vaultRoot).filePath(QStringLiteral(".obsidian/") + fileName));
}

bool Workspaces::write(const QString &vaultRoot, const QString &fileName, XDGroot root, const QJsonObject &json)
{
    const QString dir = ownDirectory(vaultRoot, root);
    if (!ensurePrivateDir(dir))
        return false;
    QSaveFile file(QDir(dir).filePath(fileName));
    if (!file.open(QIODevice::WriteOnly))
        return false;
    file.write(QJsonDocument(json).toJson(QJsonDocument::Indented));
    return file.commit();
}

QJsonObject Workspaces::current(const QString &vaultRoot)
{
    return read(vaultRoot, QStringLiteral("workspace.json"), XDGroot::State);
}

bool Workspaces::saveCurrent(const QString &vaultRoot, const QJsonObject &layout)
{
    return write(vaultRoot, QStringLiteral("workspace.json"), XDGroot::State, layout);
}

QJsonObject Workspaces::named(const QString &vaultRoot)
{
    return read(vaultRoot, QStringLiteral("workspaces.json"), XDGroot::Data);
}

bool Workspaces::saveNamed(const QString &vaultRoot, const QJsonObject &all)
{
    return write(vaultRoot, QStringLiteral("workspaces.json"), XDGroot::Data, all);
}

} // namespace noteAppC
