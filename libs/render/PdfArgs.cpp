// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej
#include "PdfArgs.h"

#include <QRegularExpression>
#include <QStringList>

namespace noteAppC {

namespace {

/// Parses one `page=` value into 0-based page numbers, appended to `out`.
/// Returns an error message, or an empty string when every token was good.
QString parsePageList(const QString &value, QList<int> &out)
{
    static const QRegularExpression single(QStringLiteral("^(\\d+)$"));
    static const QRegularExpression range(QStringLiteral("^(\\d+)-(\\d+)$"));

    const QStringList tokens = value.split(QLatin1Char(','), Qt::SkipEmptyParts);
    if (tokens.isEmpty())
        return QStringLiteral("empty page argument");

    for (const QString &raw : tokens) {
        const QString token = raw.trimmed();
        if (auto match = single.match(token); match.hasMatch()) {
            const int page = match.captured(1).toInt();
            if (page < 1)
                return QStringLiteral("page numbers start at 1");
            out.append(page - 1);
            continue;
        }
        if (auto match = range.match(token); match.hasMatch()) {
            const int from = match.captured(1).toInt();
            const int to = match.captured(2).toInt();
            if (from < 1 || to < 1)
                return QStringLiteral("page numbers start at 1");
            if (from > to)
                return QStringLiteral("page range \"%1\" runs backwards").arg(token);
            for (int page = from; page <= to; ++page)
                out.append(page - 1);
            continue;
        }
        return QStringLiteral("bad page argument \"%1\"").arg(token);
    }
    return QString();
}

} // namespace

PdfEmbedArgs parsePdfEmbedArgs(const QString &subpath)
{
    PdfEmbedArgs args;

    QString fragment = subpath;
    if (fragment.startsWith(QLatin1Char('#')))
        fragment.remove(0, 1);
    fragment = fragment.trimmed();

    if (!fragment.isEmpty()) {
        const QStringList parts = fragment.split(QLatin1Char('&'), Qt::SkipEmptyParts);
        for (const QString &part : parts) {
            const qsizetype eq = part.indexOf(QLatin1Char('='));
            const QString key = (eq < 0 ? part : part.left(eq)).trimmed().toLower();
            const QString value = (eq < 0 ? QString() : part.mid(eq + 1)).trimmed();

            if (key == QLatin1String("page")) {
                args.pagesGiven = true;
                if (value.compare(QLatin1String("all"), Qt::CaseInsensitive) == 0) {
                    args.allPages = true;
                } else {
                    const QString error = parsePageList(value, args.pages);
                    if (!error.isEmpty() && args.error.isEmpty())
                        args.error = error;
                }
            } else if (key == QLatin1String("height")) {
                bool numeric = false;
                const int height = value.toInt(&numeric);
                if (!numeric || height <= 0) {
                    if (args.error.isEmpty())
                        args.error = QStringLiteral("bad height \"%1\"").arg(value);
                } else {
                    args.height = height;
                }
            } else if (key == QLatin1String("layout")) {
                if (value.compare(QLatin1String("column"), Qt::CaseInsensitive) == 0) {
                    args.layout = PdfEmbedArgs::Layout::Column;
                    args.layoutGiven = true;
                } else if (value.compare(QLatin1String("slides"), Qt::CaseInsensitive) == 0) {
                    args.layout = PdfEmbedArgs::Layout::Slides;
                    args.layoutGiven = true;
                } else if (value.compare(QLatin1String("grid"), Qt::CaseInsensitive) == 0) {
                    args.layout = PdfEmbedArgs::Layout::Grid;
                    args.layoutGiven = true;
                } else if (args.error.isEmpty()) {
                    args.error = QStringLiteral("unknown layout \"%1\"").arg(value);
                }
            } else if (key == QLatin1String("columns")) {
                bool numeric = false;
                const int columns = value.toInt(&numeric);
                if (!numeric || columns < 1) {
                    if (args.error.isEmpty())
                        args.error = QStringLiteral("bad columns \"%1\"").arg(value);
                } else {
                    args.columns = columns;
                }
            }
            // An unrecognised key is left for a future argument to mean
            // something; it never turns into a visible error.
        }
    }

    // The default layout: pages actually listed stack in a column, unless a
    // layout was named outright; nothing listed at all is the closest match
    // to Obsidian's own embedded viewer - the whole document, one page on
    // screen at a time.
    if (!args.layoutGiven)
        args.layout = (args.pagesGiven || args.allPages) ? PdfEmbedArgs::Layout::Column
                                                          : PdfEmbedArgs::Layout::Slides;

    return args;
}

QList<int> resolvePdfPages(const PdfEmbedArgs &args, int pageCount)
{
    QList<int> pages;
    if (pageCount <= 0)
        return pages;

    if (!args.pagesGiven || args.allPages) {
        pages.reserve(pageCount);
        for (int page = 0; page < pageCount; ++page)
            pages.append(page);
        return pages;
    }

    pages.reserve(args.pages.size());
    for (int page : args.pages) {
        if (page >= 0 && page < pageCount)
            pages.append(page);
    }
    return pages;
}

} // namespace noteAppC
