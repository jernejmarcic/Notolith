// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej Marčič
#include "PandocPrepare.h"

#include "Frontmatter.h"
#include "ObsidianSyntax.h"
#include "Parser.h"

#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>

#include <algorithm>

namespace noteAppC::pandoc {

namespace {

/// One piece of the source swapped for another: a replacement, a deletion
/// (empty text) or an insertion (an empty range).
struct Edit {
    qsizetype start = 0;
    qsizetype end = 0;
    QString text;
};

struct Walk {
    const PrepareContext &context;
    QString baseDir; ///< links are relative to the note being exported, whichever note they were written in
    QStringList stack;
    PrepareResult &result;
};

QString convert(const QString &text, const QString &notePath, Walk &walk, int depth);

bool isImageFile(const QString &path)
{
    static const QStringList suffixes = {QStringLiteral("png"), QStringLiteral("jpg"),  QStringLiteral("jpeg"),
                                         QStringLiteral("gif"), QStringLiteral("svg"),  QStringLiteral("webp"),
                                         QStringLiteral("bmp"), QStringLiteral("avif")};
    return suffixes.contains(QFileInfo(path).suffix().toLower());
}

bool isNoteFile(const QString &path)
{
    return QFileInfo(path).suffix().compare(QLatin1String("md"), Qt::CaseInsensitive) == 0;
}

/// Text that goes inside `[...]`: what would end the bracket or start markup.
QString escapedText(QString text)
{
    text.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
    text.replace(QLatin1Char('['), QStringLiteral("\\["));
    text.replace(QLatin1Char(']'), QStringLiteral("\\]"));
    return text;
}

/// A link destination, in angle brackets so a space or a parenthesis is part of it.
QString destination(QString path)
{
    path.replace(QLatin1Char('<'), QStringLiteral("%3C"));
    path.replace(QLatin1Char('>'), QStringLiteral("%3E"));
    path.replace(QLatin1Char('\n'), QStringLiteral("%0A"));
    return QLatin1Char('<') + path + QLatin1Char('>');
}

qsizetype lineStartOf(const QString &text, qsizetype at)
{
    return at <= 0 ? 0 : text.lastIndexOf(QLatin1Char('\n'), at - 1) + 1;
}

qsizetype lineEndOf(const QString &text, qsizetype at)
{
    const qsizetype end = text.indexOf(QLatin1Char('\n'), at);
    return end < 0 ? text.size() : end;
}

/// The last `#` part of a link's subpath that is a heading, "" for a block link or none.
QString headingOf(const QString &subpath)
{
    const QStringList parts = subpath.split(QLatin1Char('#'), Qt::SkipEmptyParts);
    return parts.isEmpty() || parts.last().startsWith(QLatin1Char('^')) ? QString() : parts.last().trimmed();
}

QString relativeTo(const QString &baseDir, const QString &path)
{
    return baseDir.isEmpty() ? path : QDir(baseDir).relativeFilePath(path);
}

/// What an embedded note brings: its text without the frontmatter, or the
/// part a `#Heading` or `#^block` names. Nothing when the part is not there.
std::optional<QString> embeddedText(const QString &full, const QString &subpath)
{
    QString body = full;
    const Range frontmatter = Frontmatter::find(body);
    if (frontmatter.isValid())
        body.remove(0, frontmatter.end);
    if (subpath.isEmpty())
        return body.trimmed();

    const QStringList parts = subpath.split(QLatin1Char('#'), Qt::SkipEmptyParts);
    if (parts.isEmpty())
        return body.trimmed();
    if (parts.last().startsWith(QLatin1Char('^'))) {
        // The parser's block ids, so a `^id` inside a code fence is not taken for one.
        const Parser::Result parsed = Parser().parseAll(body, QString());
        for (const BlockId &id : parsed.metadata.blockIds) {
            if (id.id != parts.last().mid(1))
                continue;
            const QStringList lines = body.split(QLatin1Char('\n'));
            const qsizetype last = body.left(id.range.start).count(QLatin1Char('\n'));
            qsizetype first = last;
            while (first > 0 && !lines.at(first - 1).trimmed().isEmpty())
                --first;
            return lines.mid(first, last - first + 1).join(QLatin1Char('\n'));
        }
        return std::nullopt;
    }
    const Range section = headingSection(body, parts.last().trimmed());
    if (!section.isValid())
        return std::nullopt;
    return body.mid(section.start, section.length()).trimmed();
}

/// The edits for one wikilink or embed at `range` (the whole `[[..]]`, `!` included).
void linkEdits(const QString &text, const Link &link, Range range, const QString &notePath, Walk &walk, int depth,
               QList<Edit> &edits)
{
    const PrepareContext &context = walk.context;
    QString shown = link.alias;
    if (!link.embed && shown.isEmpty()) {
        shown = link.target;
        if (const QString heading = headingOf(link.subpath); !heading.isEmpty())
            shown += (shown.isEmpty() ? QString() : QStringLiteral(" > ")) + heading;
        if (shown.isEmpty())
            shown = link.raw.section(QLatin1Char('|'), 0, 0).remove(QLatin1Char('^')).remove(QLatin1Char('#'));
    }

    if (!link.embed) {
        QString out;
        if (link.external) {
            out = QStringLiteral("[%1](%2)").arg(escapedText(shown), destination(link.target + link.subpath));
        } else if (link.target.isEmpty()) {
            const QString heading = headingOf(link.subpath);
            out = heading.isEmpty() ? escapedText(shown)
                                    : QStringLiteral("[%1](#%2)").arg(escapedText(shown), headingID(heading));
        } else {
            const QString path = context.resolve ? context.resolve(link.target, notePath) : QString();
            if (path.isEmpty()) {
                ++walk.result.unresolved;
                out = escapedText(shown);
            } else {
                QString href = relativeTo(walk.baseDir, path);
                if (const QString heading = headingOf(link.subpath); !heading.isEmpty() && isNoteFile(path))
                    href += QLatin1Char('#') + headingID(heading);
                out = QStringLiteral("[%1](%2)").arg(escapedText(shown), destination(href));
            }
        }
        edits.append({range.start, range.end, out});
        return;
    }

    // An embed: the alias slot holds a size or the alt text.
    static const QRegularExpression sizeRe(QStringLiteral("^(\\d+)(?:x(\\d+))?$"));
    const auto size = sizeRe.match(link.alias);
    const int width = size.hasMatch() ? size.captured(1).toInt() : 0;
    const int height = size.hasMatch() ? size.captured(2).toInt() : 0;
    const QString alt = size.hasMatch() ? QString() : link.alias;
    const QString name = link.target.isEmpty() ? QFileInfo(notePath).completeBaseName() : link.target;

    QString path = link.target.isEmpty() ? notePath : (context.resolve ? context.resolve(link.target, notePath) : QString());
    // Only the vault's own files, symlinks resolved: `![[../../.ssh/id_ed25519.pub]]` in a note from elsewhere
    // is a missing embed, not a file that travels with the export (as the reference block refuses it too).
    if (!link.target.isEmpty() && !path.isEmpty() && !context.vaultRoot.isEmpty()) {
        const QString real = QFileInfo(path).canonicalFilePath();
        const QString root = QDir(context.vaultRoot).canonicalPath();
        if (real.isEmpty() || !real.startsWith(root + QLatin1Char('/')))
            path.clear();
    }
    if (path.isEmpty()) {
        ++walk.result.unresolved;
        edits.append({range.start, range.end, QLatin1Char('*') + escapedText(name) + QLatin1Char('*')});
        return;
    }

    if (isImageFile(path)) {
        QString attributes;
        if (width > 0)
            attributes = QStringLiteral("{width=%1%2}").arg(width).arg(height > 0 ? QStringLiteral(" height=%1").arg(height) : QString());
        edits.append({range.start, range.end,
                      QStringLiteral("![%1](%2)%3").arg(escapedText(alt), destination(path), attributes)});
        return;
    }

    const QString linkText = escapedText(alt.isEmpty() ? (isNoteFile(path) ? QFileInfo(path).completeBaseName()
                                                                           : QFileInfo(path).fileName())
                                                       : alt);
    const QString asLink = QStringLiteral("[%1](%2)").arg(linkText, destination(relativeTo(walk.baseDir, path)));
    const qsizetype lineStart = lineStartOf(text, range.start);
    const QString prefix = text.mid(lineStart, range.start - lineStart);
    static const QRegularExpression onlyMarkers(QStringLiteral("^[ \\t>]*$"));
    const bool ownLine = onlyMarkers.match(prefix).hasMatch()
                         && text.mid(range.end, lineEndOf(text, range.end) - range.end).trimmed().isEmpty();
    if (!isNoteFile(path) || !ownLine || !context.readNote) {
        edits.append({range.start, range.end, asLink});
        return;
    }
    if (walk.stack.contains(path) || depth >= context.embedDepth) {
        ++walk.result.cycles;
        edits.append({range.start, range.end, asLink});
        return;
    }
    const std::optional<QString> source = context.readNote(path);
    const std::optional<QString> part = source ? embeddedText(*source, link.subpath) : std::nullopt;
    if (!part) {
        ++walk.result.unresolved;
        edits.append({range.start, range.end, asLink});
        return;
    }
    walk.stack.append(path);
    QString inner = convert(*part, path, walk, depth + 1).trimmed();
    walk.stack.removeLast();
    inner.replace(QLatin1Char('\n'), QLatin1Char('\n') + prefix);
    edits.append({range.start, range.end, inner});
}

/// `> [!type|attrs]+ Title` becomes `> **Title**`, with a quoted blank line after
/// it so the body is a paragraph of its own; a `:::` container becomes a pandoc div.
void calloutEdits(const QString &text, const CalloutSpan &callout, QList<Edit> &edits)
{
    const qsizetype first = callout.range.start;
    if (first < text.size() && text.at(first) == QLatin1Char(':')) {
        qsizetype colons = 0;
        while (first + colons < text.size() && text.at(first + colons) == QLatin1Char(':'))
            ++colons;
        QString type = callout.type;
        type.remove(QRegularExpression(QStringLiteral("[^\\w-]")));
        QString opening = QString(colons, QLatin1Char(':')) + QStringLiteral(" {.callout");
        if (!type.isEmpty())
            opening += QStringLiteral(" .callout-") + type;
        opening += QLatin1Char('}');
        if (!callout.title.isEmpty())
            opening += QStringLiteral("\n\n**") + callout.title + QStringLiteral("**\n");
        edits.append({first, lineEndOf(text, first), opening});
        return;
    }

    if (!callout.marker.isValid())
        return;
    const qsizetype lineStart = lineStartOf(text, callout.marker.start);
    QString prefix = text.mid(lineStart, callout.marker.start - lineStart);
    while (prefix.endsWith(QLatin1Char(' ')) || prefix.endsWith(QLatin1Char('\t')))
        prefix.chop(1);
    const qsizetype lineEnd = lineEndOf(text, callout.marker.start);

    qsizetype titleStart = callout.titleRange.isValid() ? callout.titleRange.start : lineEnd;
    qsizetype titleEnd = callout.titleRange.isValid() ? callout.titleRange.end : lineEnd;
    while (titleEnd > titleStart && text.at(titleEnd - 1).isSpace())
        --titleEnd;
    const bool written = titleEnd > titleStart;
    // The title written, kept as is (it may hold a link); else the type's name.
    const QString open = written ? QStringLiteral("**") : QStringLiteral("**") + callout.title + QStringLiteral("**");
    edits.append({callout.marker.start, written ? titleStart : lineEnd, open});
    const bool body = callout.range.end > lineEnd + 1;
    edits.append({written ? titleEnd : lineEnd, written ? lineEnd : lineEnd,
                  (written ? QStringLiteral("**") : QString()) + (body ? QStringLiteral("\n") + prefix : QString())});
}

QString convert(const QString &text, const QString &notePath, Walk &walk, int depth)
{
    const Parser parser;
    const Parser::Result parsed = parser.parseAll(text, notePath);
    const Decorations &decorations = parsed.decorations;
    QList<Edit> edits;

    for (Range comment : decorations.comments) {
        // A comment that is a whole line takes its line break too.
        if ((comment.start == 0 || text.at(comment.start - 1) == QLatin1Char('\n'))
            && comment.end < text.size() && text.at(comment.end) == QLatin1Char('\n'))
            ++comment.end;
        edits.append({comment.start, comment.end, {}});
    }
    for (const BlockId &id : parsed.metadata.blockIds) {
        qsizetype start = id.range.start;
        while (start > 0 && (text.at(start - 1) == QLatin1Char(' ') || text.at(start - 1) == QLatin1Char('\t')))
            --start;
        edits.append({start, id.range.end, {}});
    }
    for (const StyleSpan &style : decorations.styles) {
        if (style.kind != StyleSpan::Kind::Highlight)
            continue;
        edits.append({style.range.start, style.range.start + 2, QStringLiteral("[")});
        edits.append({style.range.end - 2, style.range.end, QStringLiteral("]{.mark}")});
    }
    for (const Link &link : parsed.metadata.links) {
        if (!link.wiki)
            continue;
        Range range = link.range;
        if (link.embed && range.start > 0 && text.at(range.start) != QLatin1Char('!') && text.at(range.start - 1) == QLatin1Char('!'))
            --range.start;
        if (!QStringView(text).mid(range.start, range.length()).contains(QLatin1String("[[")))
            continue;
        linkEdits(text, link, range, notePath, walk, depth, edits);
    }
    for (const CalloutSpan &callout : decorations.callouts)
        calloutEdits(text, callout, edits);
    if (walk.context.diagramImage) {
        for (const CodeFenceSpan &fence : decorations.codeFences) {
            if (fence.depth != 0 || fence.language.isEmpty())
                continue;
            const QString image = walk.context.diagramImage(fence.language.toLower(),
                                                            text.mid(fence.content.start, fence.content.length()));
            if (!image.isEmpty())
                edits.append({fence.block.start, fence.block.end, QStringLiteral("![](%1)").arg(destination(image))});
        }
    }

    // Back to front keeps every offset true; an edit that starts inside the one before it
    // (a link in a comment, which the parser does not find anyway) is dropped.
    std::stable_sort(edits.begin(), edits.end(), [](const Edit &l, const Edit &r) {
        return l.start != r.start ? l.start < r.start : l.end < r.end;
    });
    QList<Edit> kept;
    qsizetype reached = 0;
    for (const Edit &edit : std::as_const(edits)) {
        if (edit.start < reached || edit.end < edit.start || edit.end > text.size())
            continue;
        kept.append(edit);
        reached = edit.end;
    }
    QString out = text;
    for (auto it = kept.crbegin(); it != kept.crend(); ++it)
        out.replace(it->start, it->end - it->start, it->text);
    return out;
}

} // namespace

QString headingID(const QString &heading)
{
    QString id;
    bool space = false;
    for (const QChar c : heading.toLower()) {
        if (c.isSpace()) {
            space = !id.isEmpty();
        } else if (c.isLetterOrNumber() || c == QLatin1Char('_') || c == QLatin1Char('-')) {
            if (space)
                id += QLatin1Char('-');
            space = false;
            id += c;
        }
    }
    return id;
}

PrepareResult prepareMarkdown(const QString &text, const QString &notePath, const PrepareContext &context)
{
    PrepareResult result;
    Walk walk{context, QFileInfo(notePath).absolutePath(), {notePath}, result};
    result.markdown = convert(text, notePath, walk, 0);
    return result;
}

} // namespace noteAppC::pandoc
