// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej Marčič
#include "Workspaces.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>

#include "ConfigFile.h"
#include "Vault.h"

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
    return QJsonDocument::fromJson(readConfigBytes(path)).object();
}

/// `value` without the "file" of any tab that names something outside `vaultRoot` (absolute, or climbing out
/// with ".."): the vault's own workspace.json is not trusted to say what to open. The tab itself stays, empty.
QJsonValue withoutFilesOutside(const QJsonValue &value, const QString &vaultRoot)
{
    if (value.isArray()) {
        QJsonArray out;
        for (const QJsonValue &item : value.toArray())
            out.append(withoutFilesOutside(item, vaultRoot));
        return out;
    }
    if (!value.isObject())
        return value;
    QJsonObject object = value.toObject();
    const QString file = object.value(QStringLiteral("file")).toString();
    if (!file.isEmpty() && Vault::pathInside(vaultRoot, file).isEmpty())
        object.remove(QStringLiteral("file"));
    for (auto it = object.begin(); it != object.end(); ++it)
        it.value() = withoutFilesOutside(it.value(), vaultRoot);
    return object;
}

} // namespace

QJsonObject Workspaces::read(const QString &vaultRoot, const QString &fileName, XDGroot root)
{
    const QString own = QDir(ownDirectory(vaultRoot, root)).filePath(fileName);
    if (QFileInfo::exists(own))
        return readJSON(own);
    // The vault's copy came with the vault, not from this app.
    return withoutFilesOutside(readJSON(QDir(vaultRoot).filePath(QStringLiteral(".obsidian/") + fileName)), vaultRoot)
        .toObject();
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
