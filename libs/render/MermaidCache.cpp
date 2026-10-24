#include "MermaidCache.h"

#include "Markmap.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFont>
#include <QFileInfo>
#include <QJsonArray>
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

QStringList diagramLanguages(DiagramKind kind)
{
    switch (kind) {
    case DiagramKind::Mermaid: return {QStringLiteral("mermaid")};
    case DiagramKind::PlantUML: return {QStringLiteral("plantuml"), QStringLiteral("puml")};
    case DiagramKind::TikZ: return {QStringLiteral("tikz")};
    case DiagramKind::Graphviz: return {QStringLiteral("dot"), QStringLiteral("graphviz")};
    case DiagramKind::Markmap: return {QStringLiteral("markmap")};
    }
    return {};
}

QByteArray diagramConfig(DiagramKind kind, const Theme &theme)
{
    if (kind == DiagramKind::Mermaid)
        return mermaidConfig(theme);
    QJsonObject colours{{QStringLiteral("dark"), theme.isDark()},
                        {QStringLiteral("text"), theme.color(QStringLiteral("--text-normal")).name()},
                        {QStringLiteral("background"), theme.color(QStringLiteral("--background-primary")).name()}};
    if (kind == DiagramKind::Markmap) {
        // The branch colours are the theme's own named colours, the order markmap.js's palette has them in spirit.
        QJsonArray palette;
        for (const char *token : {"--color-blue", "--color-orange", "--color-green", "--color-red", "--color-purple", "--color-cyan",
                                  "--color-pink", "--color-yellow"})
            palette.append(theme.color(QString::fromLatin1(token)).name());
        colours.insert(QStringLiteral("muted"), theme.color(QStringLiteral("--text-muted")).name());
        colours.insert(QStringLiteral("accent"), theme.color(QStringLiteral("--interactive-accent")).name());
        colours.insert(QStringLiteral("palette"), palette);
        colours.insert(QStringLiteral("font"), theme.textFont().family());
        colours.insert(QStringLiteral("mono"), theme.monospaceFont().family());
    }
    return QJsonDocument(colours).toJson(QJsonDocument::Compact);
}

QString diagramCacheDirectory(DiagramKind kind)
{
    // The same spelling as TexCompiler.cpp's: QStandardPaths folds the
    // application name in, and the cache should survive how noteAppC is run.
    QString base = qEnvironmentVariable("XDG_CACHE_HOME");
    if (base.isEmpty())
        base = QDir::homePath() + QStringLiteral("/.cache");
    return base + QStringLiteral("/noteAppC/") + diagramLanguages(kind).constFirst();
}

QString mermaidCacheDirectory()
{
    return diagramCacheDirectory(DiagramKind::Mermaid);
}

static QString hashOf(const QByteArray &bytes)
{
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha1).toHex());
}

QString diagramCachePath(DiagramKind kind, const QString &source, const QByteArray &config)
{
    // "2": the cache's format (1 held re-coloured light diagrams).
    return diagramCacheDirectory(kind) + QLatin1Char('/') + hashOf(source.toUtf8()) + QLatin1Char('-')
           + hashOf(QByteArrayLiteral("2|") + config) + QStringLiteral(".svg");
}

QString mermaidCachePath(const QString &source, const QByteArray &config)
{
    return diagramCachePath(DiagramKind::Mermaid, source, config);
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

QByteArray diagramCachedSVG(DiagramKind kind, const QString &source, const Theme &theme)
{
    // No tool and nothing worth keeping: the map is drawn from the text, in `theme`'s own colours.
    if (kind == DiagramKind::Markmap)
        return drawMarkmap(parseMarkmap(source), markmapLook(diagramConfig(kind, theme))).svg;
    if (const QByteArray exact = mermaidCachedSVG(diagramCachePath(kind, source, diagramConfig(kind, theme))); !exact.isEmpty())
        return exact;
    // Not in this theme's colours (the app's own may carry a user's accent, which
    // the built-in one lacks): the diagram most recently used, whatever its colours.
    const QFileInfoList others = QDir(diagramCacheDirectory(kind))
                                     .entryInfoList({hashOf(source.toUtf8()) + QStringLiteral("-*.svg")}, QDir::Files, QDir::Time);
    return others.isEmpty() ? QByteArray() : mermaidCachedSVG(others.first().absoluteFilePath());
}

QByteArray mermaidCachedSVG(const QString &source, const Theme &theme)
{
    return diagramCachedSVG(DiagramKind::Mermaid, source, theme);
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
