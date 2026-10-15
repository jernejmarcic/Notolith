#include "MermaidCache.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

#include <algorithm>

namespace noteAppC {

QByteArray mermaidConfig(const Theme &theme)
{
    const auto colour = [&](const char *name) { return theme.color(QString::fromLatin1(name)).name(); };
    const QString background = colour("--background-primary");
    const QString panel = colour("--background-secondary");
    const QString text = colour("--text-normal");
    const QString muted = colour("--text-muted");
    const QString accent = colour("--interactive-accent");
    const QString border = colour("--background-modifier-border");
    const QJsonObject vars{
        {QStringLiteral("darkMode"), theme.isDark()},
        {QStringLiteral("background"), background},
        {QStringLiteral("primaryColor"), panel},
        {QStringLiteral("primaryTextColor"), text},
        {QStringLiteral("primaryBorderColor"), accent},
        {QStringLiteral("secondaryColor"), panel},
        {QStringLiteral("tertiaryColor"), background},
        {QStringLiteral("lineColor"), muted},
        {QStringLiteral("textColor"), text},
        {QStringLiteral("mainBkg"), panel},
        {QStringLiteral("nodeBorder"), accent},
        {QStringLiteral("clusterBkg"), background},
        {QStringLiteral("clusterBorder"), border},
        {QStringLiteral("edgeLabelBackground"), background},
    };
    return QJsonDocument(QJsonObject{{QStringLiteral("theme"), theme.isDark() ? QStringLiteral("dark") : QStringLiteral("default")},
                                     {QStringLiteral("themeVariables"), vars}})
        .toJson(QJsonDocument::Compact);
}

QString mermaidCacheDirectory()
{
    // The same spelling as TexCompiler.cpp's: QStandardPaths folds the
    // application name in, and the cache should survive how noteAppC is run.
    QString base = qEnvironmentVariable("XDG_CACHE_HOME");
    if (base.isEmpty())
        base = QDir::homePath() + QStringLiteral("/.cache");
    return base + QStringLiteral("/noteAppC/mermaid");
}

static QString hashOf(const QByteArray &bytes)
{
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha1).toHex());
}

QString mermaidCachePath(const QString &source, const QByteArray &config)
{
    // "2": the cache's format (1 held re-coloured light diagrams).
    return mermaidCacheDirectory() + QLatin1Char('/') + hashOf(source.toUtf8()) + QLatin1Char('-')
           + hashOf(QByteArrayLiteral("2|") + config) + QStringLiteral(".svg");
}

QByteArray mermaidCachedSVG(const QString &path)
{
    QFile disk(path);
    if (!disk.open(QIODevice::ReadOnly))
        return {};
    const QByteArray svg = disk.readAll();
    // "Used" is the modification time, which relatime would not keep for the
    // access time; the prune goes by it.
    if (!svg.isEmpty())
        disk.setFileTime(QDateTime::currentDateTime(), QFileDevice::FileModificationTime);
    return svg;
}

QByteArray mermaidCachedSVG(const QString &source, const Theme &theme)
{
    if (const QByteArray exact = mermaidCachedSVG(mermaidCachePath(source, mermaidConfig(theme))); !exact.isEmpty())
        return exact;
    // Not in this theme's colours (the app's own may carry a user's accent, which
    // the built-in one lacks): the diagram most recently used, whatever its colours.
    const QFileInfoList others = QDir(mermaidCacheDirectory())
                                     .entryInfoList({hashOf(source.toUtf8()) + QStringLiteral("-*.svg")}, QDir::Files, QDir::Time);
    return others.isEmpty() ? QByteArray() : mermaidCachedSVG(others.first().absoluteFilePath());
}

bool mermaidCacheStore(const QString &path, const QByteArray &svg)
{
    if (!QDir().mkpath(QFileInfo(path).path()))
        return false;
    QSaveFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(svg) == svg.size() && file.commit();
}

void pruneMermaidCache(const QString &directory, int maxDays, qint64 maxBytes)
{
    QFileInfoList files = QDir(directory).entryInfoList({QStringLiteral("*.svg")}, QDir::Files);
    const QDateTime cutoff = QDateTime::currentDateTime().addDays(-maxDays);
    qint64 total = 0;
    QFileInfoList kept;
    for (const QFileInfo &file : std::as_const(files)) {
        if (file.lastModified() < cutoff)
            QFile::remove(file.absoluteFilePath());
        else {
            kept << file;
            total += file.size();
        }
    }
    std::sort(kept.begin(), kept.end(),
              [](const QFileInfo &l, const QFileInfo &r) { return l.lastModified() < r.lastModified(); });
    for (const QFileInfo &file : std::as_const(kept)) {
        if (total <= maxBytes)
            break;
        if (QFile::remove(file.absoluteFilePath()))
            total -= file.size();
    }
}

} // namespace noteAppC
