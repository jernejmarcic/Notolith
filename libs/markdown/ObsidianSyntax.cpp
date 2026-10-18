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

QString ObsidianSyntax::headingLinkText(const QString &heading)
{
    static const QRegularExpression unlinkable(QStringLiteral(R"(%%|[#|^\[\]:\\])"));
    QString text = heading;
    text.replace(unlinkable, QStringLiteral(" "));
    return text.simplified();
}

QString ObsidianSyntax::headingKey(const QString &heading)
{
    QString key = heading.normalized(QString::NormalizationForm_C);
    for (QChar &c : key) {
        if (!c.isLetterOrNumber() && !c.isSurrogate())
            c = QLatin1Char(' ');
    }
    return key.simplified();
}

namespace {

/// The heading in [first, last) a link names: the one whose link text is `name` (case aside) wins, so `C++`
/// finds `## C++` after `## C`; else the first with the same key, which forgives punctuation.
qsizetype matchHeading(const QList<Heading> &headings, qsizetype first, qsizetype last, const QString &name)
{
    const auto exact = [](const QString &text) {
        return ObsidianSyntax::headingLinkText(text).normalized(QString::NormalizationForm_C);
    };
    const QString wantedText = exact(name);
    for (qsizetype i = first; i < last; ++i) {
        if (exact(headings.at(i).text).compare(wantedText, Qt::CaseInsensitive) == 0)
            return i;
    }
    const QString key = ObsidianSyntax::headingKey(name);
    for (qsizetype i = first; i < last; ++i) {
        if (ObsidianSyntax::headingKey(headings.at(i).text).compare(key, Qt::CaseInsensitive) == 0)
            return i;
    }
    return -1;
}

} // namespace

QList<qsizetype> ObsidianSyntax::headingsUnder(const QList<Heading> &headings, const QStringList &parents)
{
    qsizetype first = 0;
    qsizetype last = headings.size();
    for (const QString &parent : parents) {
        const qsizetype found = matchHeading(headings, first, last, parent);
        if (found < 0)
            return {};
        first = found + 1;
        const int level = headings.at(found).level;
        for (last = first; last < headings.size() && headings.at(last).level > level;)
            ++last;
    }
    QList<qsizetype> under;
    for (qsizetype i = first; i < last; ++i)
        under << i;
    return under;
}

qsizetype ObsidianSyntax::findHeading(const QList<Heading> &headings, const QString &subpath)
{
    const QString wanted = subpath.startsWith(QLatin1Char('#')) ? subpath.mid(1) : subpath;
    QStringList path = wanted.split(QLatin1Char('#'), Qt::SkipEmptyParts);
    if (path.isEmpty())
        return -1;
    const auto firstNamed = [&headings](const QList<qsizetype> &among, const QString &name) {
        return among.isEmpty() ? qsizetype(-1) : matchHeading(headings, among.first(), among.last() + 1, name); // a contiguous range
    };
    const QString whole = path.size() > 1 ? wanted : QString();
    const qsizetype nested = firstNamed(headingsUnder(headings, path.mid(0, path.size() - 1)), path.last());
    if (nested >= 0 || whole.isEmpty())
        return nested;
    return firstNamed(headingsUnder(headings, {}), whole);
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
    // One pass over the whole text, not one per region. `%%...%%` is lazy and
    // unanchored, so when a file holds no comment at all PCRE has to walk to
    // the end before it can say so - and running that once per unmasked region
    // made this O(regions x file size). A 1 MB note has some 16 000 regions,
    // which was almost the entire cost of opening it.
    QList<Range> commentRanges;
    auto comments = commentRe().globalMatch(text);
    while (comments.hasNext()) {
        const auto match = comments.next();
        const Range range{match.capturedStart(), match.capturedEnd()};
        // `regions` is sorted and disjoint, so the one that could hold this
        // match is the last whose start is not past it.
        const auto after = std::upper_bound(
            regions.cbegin(), regions.cend(), range.start,
            [](qsizetype value, const Range &region) { return value < region.start; });
        if (after == regions.cbegin() || (after - 1)->end <= range.start)
            continue; // the comment opens inside code or another masked span
        result.comments.append(range);
        commentRanges.append(range);
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
