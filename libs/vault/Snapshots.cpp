#include "Snapshots.h"
#include "Vault.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>

#include <algorithm>

namespace noteAppC {

namespace {

qint64 nowOr(qint64 nowMs)
{
    return nowMs > 0 ? nowMs : QDateTime::currentMSecsSinceEpoch();
}

QString noteDirectory(const QString &notePath)
{
    const QByteArray key =
        QCryptographicHash::hash(QFileInfo(notePath).absoluteFilePath().toUtf8(),
                                 QCryptographicHash::Sha1)
            .toHex();
    return QDir(Snapshots::directory()).filePath(QString::fromLatin1(key));
}

bool writeFile(const QString &path, const QByteArray &bytes)
{
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    file.write(bytes);
    return file.commit();
}

} // namespace

bool Snapshots::forget(const QString &notePath)
{
    return QDir(noteDirectory(notePath)).removeRecursively();
}

QString Snapshots::directory()
{
    // Data, not cache: a cache may be cleared at will, and these are the
    // copies someone reaches for after losing the original.
    const QString data = qEnvironmentVariableIsEmpty("XDG_DATA_HOME")
                             ? QDir::homePath() + QStringLiteral("/.local/share")
                             : qEnvironmentVariable("XDG_DATA_HOME");
    return data + QStringLiteral("/noteAppC/recovery");
}

bool Snapshots::record(const QString &notePath, const QString &text, qint64 minimumIntervalMs,
                       qint64 nowMs)
{
    // An encrypted note's text never reaches the disk, here included.
    if (Vault::isEncryptedNote(notePath))
        return false;
    const qint64 now = nowOr(nowMs);
    const QList<Version> existing = versions(notePath);
    if (!existing.isEmpty()) {
        if (now - existing.first().time < minimumIntervalMs)
            return false;
        if (read(existing.first()) == text)
            return false;
    }
    const QString dir = noteDirectory(notePath);
    if (!QDir().mkpath(dir))
        return false;
    const QString pathFile = QDir(dir).filePath(QStringLiteral("path"));
    if (!QFileInfo::exists(pathFile)
        && !writeFile(pathFile, QFileInfo(notePath).absoluteFilePath().toUtf8()))
        return false;
    return writeFile(QDir(dir).filePath(QString::number(now) + QStringLiteral(".md")), text.toUtf8());
}

QStringList Snapshots::notes()
{
    QList<QPair<qint64, QString>> found;
    const QDir root(directory());
    for (const QString &name : root.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        const QDir dir(root.filePath(name));
        QFile pathFile(dir.filePath(QStringLiteral("path")));
        if (!pathFile.open(QIODevice::ReadOnly))
            continue;
        const QString path = QString::fromUtf8(pathFile.readAll());
        const QList<Version> kept = versions(path);
        if (!kept.isEmpty())
            found.append({kept.first().time, path});
    }
    std::sort(found.begin(), found.end(), [](const auto &l, const auto &r) { return l.first > r.first; });
    QStringList paths;
    for (const auto &entry : std::as_const(found))
        paths << entry.second;
    return paths;
}

QList<Snapshots::Version> Snapshots::versions(const QString &notePath)
{
    QList<Version> out;
    const QDir dir(noteDirectory(notePath));
    for (const QString &name : dir.entryList({QStringLiteral("*.md")}, QDir::Files)) {
        bool ok = false;
        const qint64 time = QFileInfo(name).completeBaseName().toLongLong(&ok);
        if (ok)
            out.append({QFileInfo(notePath).absoluteFilePath(), time, dir.filePath(name)});
    }
    std::sort(out.begin(), out.end(), [](const Version &l, const Version &r) { return l.time > r.time; });
    return out;
}

QString Snapshots::read(const Version &version)
{
    QFile file(version.file);
    return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll()) : QString();
}

int Snapshots::prune(qint64 keepMs, qint64 nowMs)
{
    return pruneStore(directory(), QStringLiteral(".md"), keepMs, nowMs);
}

int Snapshots::pruneStore(const QString &directory, const QString &suffix, qint64 keepMs, qint64 nowMs)
{
    const qint64 cutoff = nowOr(nowMs) - keepMs;
    int removed = 0;
    const QString pattern = QLatin1Char('*') + suffix;
    QDir root(directory);
    for (const QString &name : root.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        QDir dir(root.filePath(name));
        for (const QString &file : dir.entryList({pattern}, QDir::Files)) {
            bool ok = false;
            const qint64 time = QFileInfo(file).completeBaseName().toLongLong(&ok);
            if (ok && time < cutoff && dir.remove(file))
                ++removed;
        }
        // A note with no versions left keeps no directory either.
        if (dir.entryList({pattern}, QDir::Files).isEmpty())
            dir.removeRecursively();
    }
    return removed;
}

} // namespace noteAppC
