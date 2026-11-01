#include "PdfRecovery.h"
#include "ConfigFile.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>

#include "Snapshots.h"

#include <algorithm>

namespace noteAppC {

namespace {

qint64 nowOr(qint64 nowMs)
{
    return nowMs > 0 ? nowMs : QDateTime::currentMSecsSinceEpoch();
}

QString pdfDirectory(const QString &pdfPath)
{
    const QByteArray key =
        QCryptographicHash::hash(QFileInfo(pdfPath).absoluteFilePath().toUtf8(), QCryptographicHash::Sha1)
            .toHex();
    return QDir(PdfRecovery::directory()).filePath(QString::fromLatin1(key));
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

QString PdfRecovery::directory()
{
    return appDir(XDGroot::Data, QStringLiteral("pdf-recovery"));
}

bool PdfRecovery::record(const QString &pdfPath, qint64 minimumIntervalMs, qint64 nowMs)
{
    QFile current(pdfPath);
    if (!current.open(QIODevice::ReadOnly))
        return false; // nothing on disk yet - nothing to protect
    const QByteArray bytes = current.readAll();

    const qint64 now = nowOr(nowMs);
    const QList<Version> existing = versions(pdfPath);
    if (!existing.isEmpty()) {
        if (now - existing.first().time < minimumIntervalMs)
            return false;
        if (read(existing.first()) == bytes)
            return false;
    }

    const QString dir = pdfDirectory(pdfPath);
    if (!ensurePrivateDir(dir))
        return false;
    const QString pathFile = QDir(dir).filePath(QStringLiteral("path"));
    if (!QFileInfo::exists(pathFile)
        && !writeFile(pathFile, QFileInfo(pdfPath).absoluteFilePath().toUtf8()))
        return false;
    return writeFile(QDir(dir).filePath(QString::number(now) + QStringLiteral(".pdf")), bytes);
}

QList<PdfRecovery::Version> PdfRecovery::versions(const QString &pdfPath)
{
    QList<Version> out;
    const QDir dir(pdfDirectory(pdfPath));
    for (const QString &name : dir.entryList({QStringLiteral("*.pdf")}, QDir::Files)) {
        bool ok = false;
        const qint64 time = QFileInfo(name).completeBaseName().toLongLong(&ok);
        if (ok)
            out.append({QFileInfo(pdfPath).absoluteFilePath(), time, dir.filePath(name)});
    }
    std::sort(out.begin(), out.end(), [](const Version &l, const Version &r) { return l.time > r.time; });
    return out;
}

int PdfRecovery::prune(qint64 keepMs, qint64 nowMs)
{
    return Snapshots::pruneStore(directory(), QStringLiteral(".pdf"), keepMs, nowMs);
}

QByteArray PdfRecovery::read(const Version &version)
{
    QFile file(version.file);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

} // namespace noteAppC
