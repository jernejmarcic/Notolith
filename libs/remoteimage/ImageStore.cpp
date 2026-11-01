// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej Marčič
#include "ImageStore.h"

#include "ConfigFile.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>

#include <algorithm>
#include <vector>

namespace noteAppC::remoteimage {

namespace {

bool saveSecret(const QString &path, const QByteArray &bytes)
{
    QSaveFile file(path);
    // The temporary file is the one that is renamed into place, so its mode is the final one.
    if (!file.open(QIODevice::WriteOnly))
        return false;
    file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    file.write(bytes);
    return file.commit();
}

} // namespace

ImageAllowlist::ImageAllowlist(QString path) : m_path(std::move(path))
{
    QFile file(m_path);
    if (!file.open(QIODevice::ReadOnly))
        return;
    for (const QByteArray &line : file.readAll().split('\n')) {
        const QString origin = QString::fromUtf8(line).trimmed();
        if (!origin.isEmpty())
            m_origins.insert(origin);
    }
}

QString ImageAllowlist::defaultPath()
{
    const QString dir = appDir(XDGroot::State);
    return dir.isEmpty() ? QString() : dir + QStringLiteral("/remote-image-hosts");
}

QStringList ImageAllowlist::origins() const
{
    QStringList list(m_origins.begin(), m_origins.end());
    list.sort();
    return list;
}

bool ImageAllowlist::add(const QString &origin)
{
    if (origin.isEmpty() || m_origins.contains(origin))
        return !origin.isEmpty();
    m_origins.insert(origin);
    return save();
}

bool ImageAllowlist::clear()
{
    m_origins.clear();
    return save();
}

bool ImageAllowlist::save() const
{
    if (m_path.isEmpty() || !ensurePrivateDir(QFileInfo(m_path).absolutePath()))
        return false;
    return saveSecret(m_path, origins().join(QLatin1Char('\n')).toUtf8() + (m_origins.isEmpty() ? "" : "\n"));
}

ImageCache::ImageCache(QString dir, qint64 capBytes) : m_dir(std::move(dir)), m_cap(capBytes) {}

QString ImageCache::defaultDir()
{
    return appDir(XDGroot::Cache, QStringLiteral("remote-images"));
}

QString ImageCache::pathFor(const QUrl &url) const
{
    // The fragment is not sent, so two URLs that differ in it are one image.
    const QByteArray key = url.adjusted(QUrl::RemoveFragment).toEncoded();
    return m_dir + QLatin1Char('/') + QString::fromLatin1(QCryptographicHash::hash(key, QCryptographicHash::Sha256).toHex());
}

QByteArray ImageCache::read(const QUrl &url) const
{
    QFile file(pathFor(url));
    return !m_dir.isEmpty() && file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

bool ImageCache::write(const QUrl &url, const QByteArray &bytes)
{
    const QString path = pathFor(url);
    if (m_dir.isEmpty() || !ensurePrivateDir(m_dir) || !saveSecret(path, bytes))
        return false;
    evict(path);
    return true;
}

qint64 ImageCache::totalBytes() const
{
    qint64 total = 0;
    for (QDirIterator it(m_dir, QDir::Files); it.hasNext();) {
        it.next();
        total += it.fileInfo().size();
    }
    return total;
}

// `keep` is the file just written: with equal times it must not be the one that goes.
void ImageCache::evict(const QString &keep) const
{
    struct Entry {
        QDateTime modified;
        qint64 size;
        QString path;
    };
    std::vector<Entry> files;
    qint64 total = 0;
    for (QDirIterator it(m_dir, QDir::Files); it.hasNext();) {
        it.next();
        files.push_back({it.fileInfo().lastModified(), it.fileInfo().size(), it.filePath()});
        total += files.back().size;
    }
    std::sort(files.begin(), files.end(), [](const Entry &l, const Entry &r) { return l.modified < r.modified; });
    for (const Entry &entry : files) {
        if (total <= m_cap)
            break;
        if (entry.path != keep && QFile::remove(entry.path))
            total -= entry.size;
    }
}

} // namespace noteAppC::remoteimage
