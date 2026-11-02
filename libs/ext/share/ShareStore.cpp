// SPDX-License-Identifier: GPL-3.0-or-later
#include "ShareStore.h"
#include "ConfigFile.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

namespace noteAppC {

namespace {

QByteArray fromHex(const QJsonObject &o, const char *name, int size)
{
    const QByteArray value = QByteArray::fromHex(o.value(QLatin1String(name)).toString().toLatin1());
    return value.size() == size ? value : QByteArray();
}

bool writeWhole(const QString &path, const QByteArray &bytes)
{
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    if (file.write(bytes) != bytes.size())
        return false;
    return file.commit();
}

QByteArray readWhole(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

} // namespace

ShareStore::ShareStore(QString directory)
    : m_directory(std::move(directory))
{
}

QString ShareStore::directoryFor(const QString &vaultRoot)
{
    return appDir(XDGroot::Data, QStringLiteral("share/") + vaultDigest(vaultRoot));
}

QString ShareStore::shareDirectory(const QByteArray &shareID) const
{
    return m_directory + QLatin1Char('/') + QString::fromLatin1(shareID.toHex());
}

QList<ShareRecord> ShareStore::all() const
{
    QList<ShareRecord> records;
    const QDir dir(m_directory);
    for (const QString &name : dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        const QJsonObject o = QJsonDocument::fromJson(readWhole(dir.filePath(name) + QStringLiteral("/share.json"))).object();
        ShareRecord r;
        r.shareID = fromHex(o, "id", 16);
        r.owner = fromHex(o, "owner", 32);
        r.key = fromHex(o, "key", 32);
        r.members = QByteArray::fromBase64(o.value(QLatin1String("members")).toString().toLatin1());
        r.epoch = quint32(o.value(QLatin1String("epoch")).toDouble());
        r.backend = o.value(QLatin1String("backend")).toInt();
        r.clientID = o.value(QLatin1String("clientID")).toString().toULongLong();
        r.relativePath = o.value(QLatin1String("path")).toString();
        r.title = o.value(QLatin1String("title")).toString();
        r.owned = o.value(QLatin1String("owned")).toBool();
        for (const QJsonValue &host : o.value(QLatin1String("hosts")).toArray())
            r.hosts.append(host.toString());
        if (o.value(QLatin1String("v")).toInt() != 1 || r.shareID.isEmpty() || r.owner.isEmpty() || r.key.isEmpty()
            || r.members.isEmpty() || r.epoch == 0 || (r.backend != 1 && r.backend != 2)
            || r.shareID.toHex() != name.toLatin1())
            continue;
        records.append(r);
    }
    return records;
}

bool ShareStore::save(const ShareRecord &r, const QByteArray &state, const QString &base)
{
    const QString dir = shareDirectory(r.shareID);
    if (!ensurePrivateDir(dir))
        return false;
    QFile::setPermissions(m_directory, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    QFile::setPermissions(dir, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    const QJsonObject o{
        {QStringLiteral("v"), 1},
        {QStringLiteral("id"), QString::fromLatin1(r.shareID.toHex())},
        {QStringLiteral("owner"), QString::fromLatin1(r.owner.toHex())},
        {QStringLiteral("key"), QString::fromLatin1(r.key.toHex())},
        {QStringLiteral("epoch"), double(r.epoch)},
        {QStringLiteral("members"), QString::fromLatin1(r.members.toBase64())},
        {QStringLiteral("path"), r.relativePath},
        {QStringLiteral("title"), r.title},
        {QStringLiteral("backend"), r.backend},
        {QStringLiteral("clientID"), QString::number(r.clientID)},
        {QStringLiteral("owned"), r.owned},
        {QStringLiteral("hosts"), QJsonArray::fromStringList(r.hosts)},
    };
    return writeWhole(dir + QStringLiteral("/state.bin"), state)
        && writeWhole(dir + QStringLiteral("/base.md"), base.toUtf8())
        && writeWhole(dir + QStringLiteral("/share.json"), QJsonDocument(o).toJson(QJsonDocument::Compact));
}

QByteArray ShareStore::state(const QByteArray &shareID) const
{
    return readWhole(shareDirectory(shareID) + QStringLiteral("/state.bin"));
}

QString ShareStore::base(const QByteArray &shareID) const
{
    return QString::fromUtf8(readWhole(shareDirectory(shareID) + QStringLiteral("/base.md")));
}

void ShareStore::remove(const QByteArray &shareID)
{
    if (shareID.size() == 16)
        QDir(shareDirectory(shareID)).removeRecursively();
}

} // namespace noteAppC
