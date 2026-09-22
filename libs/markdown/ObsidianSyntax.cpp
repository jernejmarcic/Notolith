#include "ObsidianSyntax.h"

#include <QRegularExpression>
#include <algorithm>

namespace noteAppC {

namespace {

/// Sorted, merged copy of `ranges`, so masking is a single forward walk.
QList<Range> normalise(QList<Range> ranges)
{
    ranges.removeIf([](const Range &r) { return !r.isValid() || r.length() <= 0; });
    std::sort(ranges.begin(), ranges.end(),
              [](const Range &l, const Range &r) { return l.start < r.start; });

    QList<Range> merged;
    for (const Range &range : std::as_const(ranges)) {
        if (!merged.isEmpty() && range.start <= merged.last().end)
            merged.last().end = qMax(merged.last().end, range.end);
        else
            merged.append(range);
    }
    return merged;
}

/// The spans of `text` that are not masked.
QList<Range> unmasked(qsizetype length, const QList<Range> &masked)
{
    QList<Range> open;
    qsizetype cursor = 0;
    for (const Range &range : masked) {
        if (range.start > cursor)
            open.append({cursor, qMin(range.start, length)});
        cursor = qMax(cursor, range.end);
    }
    if (cursor < length)
        open.append({cursor, length});
    return open;
}

/// Obsidian starts a tag only at the beginning of a line or after whitespace
/// or an opening bracket - `C#` in prose is not a tag.
bool canStartTag(const QString &text, qsizetype at)
{
    if (at == 0)
        return true;
    const QChar before = text.at(at - 1);
    return before.isSpace() || before == QLatin1Char('(') || before == QLatin1Char('[')
           || before == QLatin1Char('{') || before == QLatin1Char('>')
           || before == QLatin1Char('"') || before == QLatin1Char('\'');
}

const QRegularExpression &wikilinkRe()
{
    // The target may not contain a closing bracket; the alias may not either.
    static const QRegularExpression re(QStringLiteral(R"((!?)\[\[([^\[\]\n]*)\]\])"));
    return re;
}

const QRegularExpression &tagRe()
{
    // At least one character that is not a digit, so "#1" stays a number.
    static const QRegularExpression re(
        QStringLiteral(R"(#([\w/\-]*[^\W\d_][\w/\-]*))"),
        QRegularExpression::UseUnicodePropertiesOption);
    return re;
}

const QRegularExpression &blockIdRe()
{
    // Only at the very end of a line, which is the only place Obsidian looks.
    static const QRegularExpression re(QStringLiteral(R"(\s\^([A-Za-z0-9][A-Za-z0-9-]*)[ \t]*$)"),
                                       QRegularExpression::MultilineOption);
    return re;
}

const QRegularExpression &highlightRe()
{
    static const QRegularExpression re(QStringLiteral(R"(==([^\n]+?)==)"));
    return re;
}

const QRegularExpression &commentRe()
{
    static const QRegularExpression re(QStringLiteral(R"(%%([\s\S]*?)%%)"));
    return re;
}

} // namespace

bool ObsidianSyntax::isExternalTarget(const QString &target)
{
    static const QRegularExpression scheme(QStringLiteral(R"(^[A-Za-z][A-Za-z0-9+.\-]*:)"));
    return scheme.match(target).hasMatch() || target.startsWith(QLatin1String("//"));
}

Link ObsidianSyntax::parseWikilink(const QString &raw, bool embed)
{
    Link link;
    link.raw = raw;
    link.embed = embed;
    link.wiki = true;

    QString rest = raw;
    const qsizetype pipe = rest.indexOf(QLatin1Char('|'));
    if (pipe >= 0) {
        link.alias = rest.mid(pipe + 1).trimmed();
        rest = rest.left(pipe);
    }

    // "#" splits off a heading path or block reference, but a leading "#"
    // means the link points inside this same note.
    const qsizetype hash = rest.indexOf(QLatin1Char('#'));
    if (hash >= 0) {
        link.subpath = rest.mid(hash).trimmed();
        rest = rest.left(hash);
    }

    link.target = rest.trimmed();
    link.external = isExternalTarget(link.target);
    return link;
}

ObsidianSyntax::Result ObsidianSyntax::scan(const QString &text, const QList<Range> &masked)
{
    Result result;
    const QList<Range> regions = unmasked(text.size(), normalise(masked));

    // Comments first: whatever they cover is masked for everything else,
    // exactly as Obsidian treats %%...%%.
    QList<Range> commentRanges;
    for (const Range &region : regions) {
        auto it = commentRe().globalMatch(text, region.start);
        while (it.hasNext()) {
            const auto match = it.next();
            if (match.capturedStart() >= region.end)
                break;
            const Range range{match.capturedStart(), match.capturedEnd()};
            result.comments.append(range);
            commentRanges.append(range);
        }
    }

    const QList<Range> live = unmasked(text.size(), normalise(masked + commentRanges));

    for (const Range &region : live) {
        const QString slice = text.mid(region.start, region.length());
        const qsizetype base = region.start;

        auto links = wikilinkRe().globalMatch(slice);
        while (links.hasNext()) {
            const auto match = links.next();
            Link link = parseWikilink(match.captured(2), match.captured(1) == QLatin1String("!"));
            link.range = {base + match.capturedStart(), base + match.capturedEnd()};
            // The target sits right after "[[" (plus one for a leading "!").
            const qsizetype targetStart = base + match.capturedStart(2);
            link.targetRange = {targetStart, targetStart + link.target.size()};
            result.links.append(link);
        }

        auto tags = tagRe().globalMatch(slice);
        while (tags.hasNext()) {
            const auto match = tags.next();
            const qsizetype start = base + match.capturedStart();
            if (!canStartTag(text, start))
                continue;
            // A tag inside a wikilink is part of the link, not a tag.
            const Range range{start, base + match.capturedEnd()};
            const bool insideLink = std::any_of(
                result.links.cbegin(), result.links.cend(),
                [&range](const Link &link) { return link.range.overlaps(range); });
            if (insideLink)
                continue;
            result.tags.append(Tag{match.captured(1), range});
        }

        auto ids = blockIdRe().globalMatch(slice);
        while (ids.hasNext()) {
            const auto match = ids.next();
            result.blockIds.append(BlockId{
                match.captured(1),
                {base + match.capturedStart(1) - 1, base + match.capturedEnd(1)}});
        }

        auto highlights = highlightRe().globalMatch(slice);
        while (highlights.hasNext()) {
            const auto match = highlights.next();
            result.highlights.append({base + match.capturedStart(), base + match.capturedEnd()});
        }
    }

    return result;
}

} // namespace noteAppC
