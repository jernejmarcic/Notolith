// SPDX-License-Identifier: GPL-3.0-or-later
#include "Slugs.h"

#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QRegularExpression>

namespace noteAppC::publish {

namespace {

/// Shared by slugifySegment() and headingAnchor(): collapse every run of
/// characters outside `keep` into one '-', trim the ends.
QString collapseToSlug(const QString &text, const QRegularExpression &disallowed)
{
    QString slug = text.toLower();
    slug.replace(disallowed, QStringLiteral("-"));
    while (slug.startsWith(QLatin1Char('-')))
        slug.remove(0, 1);
    while (slug.endsWith(QLatin1Char('-')))
        slug.chop(1);
    // A run of separators in the source (e.g. "a -- b") can leave a doubled
    // '-' after replacement; one more pass folds it rather than special
    // casing the regex, which already has to run once per character class.
    static const QRegularExpression doubled(QStringLiteral("-{2,}"));
    slug.replace(doubled, QStringLiteral("-"));
    return slug;
}

} // namespace

QString Slugs::slugifySegment(const QString &text)
{
    static const QRegularExpression disallowed(QStringLiteral("[^a-z0-9]+"));
    const QString slug = collapseToSlug(text, disallowed);
    return slug.isEmpty() ? QStringLiteral("note") : slug;
}

QString Slugs::defaultPagePath(const QString &vaultRelativePath)
{
    QString stem = vaultRelativePath;
    if (stem.endsWith(QLatin1String(".md"), Qt::CaseInsensitive))
        stem.chop(3);

    const QStringList segments = stem.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    QStringList slugged;
    slugged.reserve(segments.size());
    for (const QString &segment : segments)
        slugged << slugifySegment(segment);
    if (slugged.isEmpty())
        slugged << QStringLiteral("note");
    return slugged.join(QLatin1Char('/')) + QStringLiteral(".html");
}

bool Slugs::isReservedOutput(const QString &path)
{
    // The folders and files the exporter writes itself are not a note's to take.
    return path.startsWith(QLatin1String("attachments/")) || path.startsWith(QLatin1String("tags/"))
        || path == QLatin1String("index.html") || path == QLatin1String("site.css")
        || path == QLatin1String("search.js") || path == QLatin1String("search-index.json");
}

QString Slugs::permalinkOverride(const QString &raw)
{
    QString path = raw.trimmed();
    if (path.isEmpty())
        return {};
    while (path.startsWith(QLatin1Char('/')))
        path.remove(0, 1);
    while (path.endsWith(QLatin1Char('/')))
        path.chop(1);
    if (path.isEmpty())
        return {};
    // Each segment is slugified like a default page path, keeping `.`, `_` and `~` as well: whatever is not a
    // URL-safe character (":", a quote, "<", a space) is folded into '-'. A segment that comes out empty, "." or
    // ".." sends the whole permalink back to the default path.
    static const QRegularExpression disallowed(QStringLiteral("[^a-z0-9._~-]+"));
    QStringList safe;
    for (const QString &segment : path.split(QLatin1Char('/'), Qt::SkipEmptyParts)) {
        const QString slug = collapseToSlug(segment, disallowed);
        if (slug.isEmpty() || slug == QLatin1String(".") || slug == QLatin1String(".."))
            return {};
        safe << slug;
    }
    path = safe.join(QLatin1Char('/'));
    if (!path.endsWith(QLatin1String(".html"), Qt::CaseInsensitive))
        path += QStringLiteral(".html");
    if (isReservedOutput(path))
        return {};
    return path;
}

QString Slugs::headingAnchor(const QString &headingText)
{
    static const QRegularExpression disallowed(QStringLiteral("[^a-z0-9]+"));
    const QString slug = collapseToSlug(headingText, disallowed);
    return slug.isEmpty() ? QStringLiteral("heading") : slug;
}

QString Slugs::blockAnchor(const QString &blockId)
{
    return QStringLiteral("block-") + blockId;
}

QString Slugs::tagPagePath(const QString &tagName)
{
    const QStringList segments = tagName.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    QStringList slugged;
    slugged.reserve(segments.size() + 1);
    slugged << QStringLiteral("tags");
    for (const QString &segment : segments)
        slugged << slugifySegment(segment);
    return slugged.join(QLatin1Char('/')) + QStringLiteral(".html");
}

void Slugs::deduplicate(QStringList &paths)
{
    QHash<QString, int> seen; // path -> next suffix to try
    for (QString &path : paths) {
        auto it = seen.find(path);
        if (it == seen.end()) {
            seen.insert(path, 2);
            continue;
        }
        // Find a "-N" suffix on the final segment nobody has claimed yet;
        // bump the counter each time so two later collisions on the same
        // stem do not both try "-2".
        QString candidate;
        const qsizetype dot = path.lastIndexOf(QLatin1Char('.'));
        const QString base = dot >= 0 ? path.left(dot) : path;
        const QString extension = dot >= 0 ? path.mid(dot) : QString();
        int suffix = it.value();
        do {
            candidate = base + QLatin1Char('-') + QString::number(suffix) + extension;
            ++suffix;
        } while (seen.contains(candidate));
        it.value() = suffix;
        seen.insert(candidate, 2);
        path = candidate;
    }
}

QString Slugs::relativeHref(const QString &fromOutputPath, const QString &toOutputPath)
{
    // QDir::relativeFilePath() only walks ".." correctly between two
    // *absolute* paths - given two relative ones, verified empirically, it
    // just returns the target unchanged instead (undocumented, but
    // consistent: relative-to-relative is not really what the method is
    // for). Site-root-relative output paths are relative by construction -
    // this never touches a real filesystem path - so both are anchored to a
    // fake absolute root purely for this call, then handed back relative
    // again.
    const QString fromDir = QFileInfo(fromOutputPath).path();
    const QString fromAbs =
        QLatin1Char('/') + (fromDir == QLatin1String(".") ? QString() : fromDir);
    const QString toAbs = QLatin1Char('/') + toOutputPath;
    QString href = QDir(fromAbs).relativeFilePath(toAbs);
    if (href.isEmpty())
        href = QFileInfo(toOutputPath).fileName();
    return href;
}

} // namespace noteAppC::publish
