#include "Parser.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QRegularExpression>
#include <QTextStream>

#include <QHash>

#include <algorithm>

#include <md4qt/doc.h>
#include <md4qt/entities_map.h>
#include <md4qt/parser.h>

#include "Frontmatter.h"
#include "ObsidianSyntax.h"

namespace noteAppC {

namespace {

/// What the Obsidian pass must not look inside: code, math, raw HTML.
struct Walk {
    const SourceMap *map = nullptr;
    const QString *text = nullptr;
    QList<Range> masked;
    QList<Heading> headings;
    QList<Link> links;
    QList<Task> tasks;
    Decorations decorations;
    int depth = 0;
    /// Walking a footnote's body, which is not the note's flow of paragraphs.
    bool inFootnote = false;
    /// One per list the walk is inside: how many items it has had, and the
    /// last number it gave out.
    struct ListFrame {
        int items = 0;
        int last = 0;
    };
    QList<ListFrame> lists;
    /// The outline ("2.1") of each list item the walk is inside; empty for a
    /// bullet, which starts Word-style numbering afresh beneath it.
    QStringList itemOutlines;
    /// ParseOptions::taskStates.
    bool anyTaskState = false;

    /// A reference-style link or embed - `[text][ref]`, the collapsed
    /// `[ref][]`, or the shortcut `[ref]` - whose label was recorded during
    /// the walk but whose target was not: resolving it needs every
    /// `[ref]: url` definition in the file, including ones after the link, so
    /// it waits until the walk is done.
    struct PendingReference {
        qsizetype linkIndex = -1; ///< index into `links`
        QString label;           ///< the label exactly as written at the use site
    };
    QList<PendingReference> pendingReferences;
};

Range rangeOf(const MD::WithPosition &position, const SourceMap &map)
{
    return map.range(position.startLine(), position.startColumn(),
                     position.endLine(), position.endColumn());
}

/// Concatenated text of a paragraph, with inline markup dropped - what a
/// heading or a task should read as in a list.
QString plainText(const MD::Paragraph *paragraph)
{
    QString text;
    if (!paragraph)
        return text;
    for (const auto &item : paragraph->items()) {
        switch (item->type()) {
        case MD::ItemType::Text:
            text += static_cast<MD::Text *>(item.data())->text();
            break;
        case MD::ItemType::Code:
            text += static_cast<MD::Code *>(item.data())->text();
            break;
        case MD::ItemType::Link: {
            auto *link = static_cast<MD::Link *>(item.data());
            text += link->text().isEmpty() ? link->url() : link->text();
            break;
        }
        case MD::ItemType::LineBreak:
            text += QLatin1Char(' ');
            break;
        default:
            break;
        }
    }
    return text.simplified();
}

/// Styling markers md4qt reports as opening and closing delimiters: `**`,
/// `_`, `~~`. Live preview hides the delimiters and styles what is between.
void collectStyles(MD::ItemWithOpts *item, Walk &walk)
{
    const SourceMap &map = *walk.map;
    const auto styleKind = [](int style) -> std::optional<StyleSpan::Kind> {
        switch (style) {
        case MD::TextOption::BoldText:
            return StyleSpan::Kind::Bold;
        case MD::TextOption::ItalicText:
            return StyleSpan::Kind::Italic;
        case MD::TextOption::StrikethroughText:
            return StyleSpan::Kind::Strikethrough;
        default:
            return std::nullopt;
        }
    };

    for (const auto &delim : item->openStyles()) {
        walk.decorations.syntax.append(rangeOf(delim, map));
        if (const auto kind = styleKind(delim.style()))
            walk.decorations.styles.append({*kind, rangeOf(*item, map), 0, {}, {}, {}});
    }
    for (const auto &delim : item->closeStyles())
        walk.decorations.syntax.append(rangeOf(delim, map));
}

void walkItem(MD::Item *item, Walk &walk);

/// Obsidian puts the display size in the alias slot: `|300` or `|300x200`.
EmbedSpan embedOf(const Link &link)
{
    EmbedSpan embed;
    embed.range = link.range;
    embed.target = link.target;
    embed.subpath = link.subpath;
    embed.alias = link.alias;

    static const QRegularExpression size(QStringLiteral("^(\\d+)(?:x(\\d+))?$"));
    const auto match = size.match(link.alias.trimmed());
    if (match.hasMatch()) {
        embed.width = match.captured(1).toInt();
        embed.height = match.captured(2).toInt();
    }
    return embed;
}

/// An inline `<img src="..." width="..." height="..." alt="...">` is an
/// image embed spelled in HTML: the same EmbedSpan `![alt](src)` produces,
/// with the tag's own size, so everything that draws the one draws the other.
/// Only a well-formed tag with a `src` qualifies - an unclosed quote, a stray
/// token or a missing `src` leaves it as source text - and only a relative
/// path or an http(s) URL: a `javascript:`, `data:` or `file:` source, or a
/// protocol-relative `//host`, is never an embed (a remote URL is still the
/// resolver's call, exactly as for a Markdown image).
void HTMLimageEmbed(const QString &text, const Range &range, Decorations &decorations)
{
    const QString tag = text.mid(range.start, range.length());
    static const QRegularExpression open(QStringLiteral("^<img\\b(.*?)/?>$"),
                                         QRegularExpression::CaseInsensitiveOption
                                             | QRegularExpression::DotMatchesEverythingOption);
    const QRegularExpressionMatch head = open.match(tag);
    if (!head.hasMatch())
        return;

    static const QRegularExpression attribute(
        QStringLiteral("\\s*([A-Za-z_:][-\\w:.]*)\\s*=\\s*(?:\"([^\"]*)\"|'([^']*)'|([^\\s\"'<>=`]+))"));
    QString src;
    QString alt;
    int width = 0;
    int height = 0;
    qsizetype consumed = 0;
    const QString attributes = head.captured(1);
    const auto number = [](const QString &value) {
        static const QRegularExpression px(QStringLiteral("^\\s*(\\d{1,5})\\s*(?:px)?\\s*$"),
                                           QRegularExpression::CaseInsensitiveOption);
        const QRegularExpressionMatch match = px.match(value);
        return match.hasMatch() ? match.captured(1).toInt() : 0;
    };
    for (auto it = attribute.globalMatch(attributes); it.hasNext();) {
        const QRegularExpressionMatch match = it.next();
        if (match.capturedStart() != consumed)
            return; // junk between two attributes
        consumed = match.capturedEnd();
        const QString name = match.captured(1).toLower();
        const QString value = !match.captured(2).isNull() ? match.captured(2)
                              : !match.captured(3).isNull() ? match.captured(3)
                                                            : match.captured(4);
        if (name == QLatin1String("src"))
            src = value.trimmed();
        else if (name == QLatin1String("alt"))
            alt = value;
        else if (name == QLatin1String("width"))
            width = number(value);
        else if (name == QLatin1String("height"))
            height = number(value);
    }
    if (!attributes.mid(consumed).trimmed().isEmpty() || src.isEmpty())
        return;
    if (ObsidianSyntax::isExternalTarget(src)
        && !src.startsWith(QLatin1String("http://"), Qt::CaseInsensitive)
        && !src.startsWith(QLatin1String("https://"), Qt::CaseInsensitive))
        return;

    EmbedSpan embed;
    embed.range = range;
    embed.html = true;
    const qsizetype hash = src.indexOf(QLatin1Char('#'));
    if (hash > 0) {
        embed.subpath = src.mid(hash);
        src = src.left(hash);
    }
    embed.target = QUrl::fromPercentEncoding(src.toUtf8());
    embed.alias = alt;
    embed.width = width;
    embed.height = height;
    decorations.embeds.append(embed);
}

/// Every `>` that starts a line inside a blockquote, so live preview can hide
/// them the way Obsidian does.
void hideQuoteMarkers(const QString &text, const Range &range, Decorations &decorations)
{
    // Every level at once: `> > ` in a nested quote is two markers, and the
    // inner blockquote's own pass cannot reach the second one because `^`
    // anchors it back to the start of the line.
    static const QRegularExpression marker(QStringLiteral("^[ \\t]*(?:>[ \\t]?)+"),
                                           QRegularExpression::MultilineOption);
    auto it = marker.globalMatch(text, range.start);
    while (it.hasNext()) {
        const auto match = it.next();
        if (match.capturedStart() >= range.end)
            break;
        decorations.syntax.append({match.capturedStart(), match.capturedEnd()});
    }
}

/// The source marker of a list item: the bullet or number, plus a task's
/// checkbox. Live preview hides it and draws its own in the margin.
void describeListItem(const QString &text, const Range &range, const SourceMap &map, int depth,
                      int shownNumber, const QString &outline, bool anyState, Decorations &decorations)
{
    // A quote's or callout's ">" markers first: they are hidden on their own
    // (hideQuoteMarkers), and the list marker is what follows them.
    static const QRegularExpression head(QStringLiteral(
        "^((?:[ \\t]*>[ \\t]?)*)[ \\t]*(?:([-*+])|(\\d+)([.)]))[ \\t]+(\\[([ xX])\\][ \\t]+)?"));
    // Any one UTF-16 unit but a bracket, so `- [[Note]]` and `- [x](url)` stay
    // text; an astral code point is two units and cannot be a state's QChar.
    static const QRegularExpression headAny(QStringLiteral(
        "^((?:[ \\t]*>[ \\t]?)*)[ \\t]*(?:([-*+])|(\\d+)([.)]))[ \\t]+(\\[([^\\[\\]\\n\\x{10000}-\\x{10FFFF}])\\][ \\t]+)?"));

    // md4qt puts a list item's start at its bullet, not at the start of the
    // line, so the marker has to be read from the line itself.
    const qsizetype line = map.lineAt(range.start);
    const qsizetype lineStart = map.lineStart(line);
    const QString firstLine = text.mid(lineStart, map.lineEnd(line) - lineStart);
    const auto match = (anyState ? headAny : head).match(firstLine);
    if (!match.hasMatch())
        return;

    ListItemSpan span;
    span.range = {lineStart, range.end};
    span.marker = {lineStart + match.capturedEnd(1), lineStart + match.capturedEnd(0)};
    // The nesting md4qt found, not the whitespace: an ordered list nests at
    // three spaces, a bullet at two, and a count of spaces got one of them
    // wrong either way.
    span.indent = depth;
    span.ordered = !match.captured(3).isEmpty();
    span.number = match.captured(3).toInt();
    span.bullet = span.ordered ? match.captured(4).at(0) : match.captured(2).at(0);
    span.shownNumber = shownNumber;
    span.outline = outline;
    span.task = !match.captured(5).isEmpty();
    if (span.task) {
        span.state = match.captured(6).at(0);
        span.checkbox = {lineStart + match.capturedStart(5), lineStart + match.capturedStart(5) + 3};
    }

    decorations.syntax.append(span.marker);
    decorations.listItems.append(span);
}

/// md4qt reads a list inside a quote flat - "> 1. a" then ">    1. b" come
/// back as two items of one list - so a quoted list's nesting is worked out
/// here, CommonMark's way: an item nests under the one before it when its
/// marker starts at or past that item's text. Its numbering follows.
void nestQuotedLists(const QString &text, const SourceMap &map, QList<ListItemSpan> &items)
{
    struct Open {
        qsizetype content; ///< the column the item's text starts at
        int index;
    };
    QList<Open> open;
    struct Count {
        bool ordered = false;
        int last = 0;
        bool started = false;
    };
    QList<Count> counts; // one per depth, for the items at it now
    for (int i = 0; i < items.size(); ++i) {
        ListItemSpan &item = items[i];
        const qsizetype lineStart = map.lineStart(map.lineAt(item.range.start));
        if (!text.mid(lineStart, item.marker.start - lineStart).contains(QLatin1Char('>'))) {
            open.clear(); // not in a quote: md4qt's nesting stands
            counts.clear();
            continue;
        }
        qsizetype bullet = item.marker.start;
        while (bullet < item.marker.end && text.at(bullet).isSpace())
            ++bullet;
        const qsizetype column = bullet - lineStart;
        while (!open.isEmpty() && column < open.last().content)
            open.removeLast();
        const int depth = int(open.size());
        item.indent = depth;
        for (const Open &outer : std::as_const(open)) // each item it sits in now holds it
            items[outer.index].range.end = qMax(items[outer.index].range.end, item.range.end);

        counts.resize(depth + 1);
        Count &count = counts[depth];
        if (!count.started || count.ordered != item.ordered)
            count = {item.ordered, item.number - 1, true}; // a new list: its first number counts
        if (item.ordered) {
            item.shownNumber = ++count.last;
            const ListItemSpan *parent = open.isEmpty() ? nullptr : &items[open.last().index];
            item.outline = parent && parent->ordered
                               ? parent->outline + QLatin1Char('.') + QString::number(item.shownNumber)
                               : QString::number(item.shownNumber);
        }
        open.append({item.marker.end - lineStart, i});
    }
}

} // namespace

QList<Range> tableRowCells(const QString &text, qsizetype lineStart, qsizetype lineEnd)
{
    // A pipe after an odd run of backslashes is text, not a cell border.
    const auto escaped = [&text, lineStart](qsizetype at) {
        qsizetype slashes = 0;
        while (at - slashes - 1 >= lineStart && text.at(at - slashes - 1) == QLatin1Char('\\'))
            ++slashes;
        return slashes % 2 == 1;
    };
    qsizetype start = lineStart;
    while (start < lineEnd && (text.at(start) == QLatin1Char('>') || text.at(start).isSpace()))
        ++start;
    qsizetype end = lineEnd;
    while (end > start && text.at(end - 1).isSpace())
        --end;
    if (start < end && text.at(start) == QLatin1Char('|'))
        ++start;
    if (end > start && text.at(end - 1) == QLatin1Char('|') && !escaped(end - 1))
        --end;

    QList<Range> cells;
    qsizetype from = start;
    for (qsizetype i = start; i <= end; ++i) {
        if (i < end && (text.at(i) != QLatin1Char('|') || escaped(i)))
            continue;
        qsizetype a = from;
        qsizetype b = i;
        while (a < b && text.at(a).isSpace())
            ++a;
        while (b > a && text.at(b - 1).isSpace())
            --b;
        if (a == b) // empty: one space in, so "|  |" types as "| x |"
            a = b = from + (i - from >= 2 ? 1 : 0);
        cells << Range{a, b};
        from = i + 1;
    }
    return cells;
}

namespace {

bool isTrue(const QString &value)
{
    return value.compare(QLatin1String("true"), Qt::CaseInsensitive) == 0
           || value.compare(QLatin1String("yes"), Qt::CaseInsensitive) == 0
           || value == QLatin1String("1");
}

bool isFalse(const QString &value)
{
    return value.compare(QLatin1String("false"), Qt::CaseInsensitive) == 0
           || value.compare(QLatin1String("no"), Qt::CaseInsensitive) == 0
           || value.compare(QLatin1String("off"), Qt::CaseInsensitive) == 0 || value == QLatin1String("0");
}

/// A value written the way an attribute or an option writes one, with the
/// quotes that made it one word taken back off.
QString unquote(QString value)
{
    value = value.trimmed();
    if (value.size() >= 2 && value.startsWith(value.back())
        && (value.startsWith(QLatin1Char('"')) || value.startsWith(QLatin1Char('\''))))
        return value.mid(1, value.size() - 2);
    return value;
}

/// The line-comment token a language would use, which is what `#|` options are
/// written behind so the block still compiles (PLAN.md 16.2).
QString commentPrefix(const QString &language)
{
    static const QHash<QString, QString> prefixes = {
        {QStringLiteral("c"), QStringLiteral("//")},
        {QStringLiteral("cpp"), QStringLiteral("//")},
        {QStringLiteral("c++"), QStringLiteral("//")},
        {QStringLiteral("cxx"), QStringLiteral("//")},
        {QStringLiteral("h"), QStringLiteral("//")},
        {QStringLiteral("hpp"), QStringLiteral("//")},
        {QStringLiteral("java"), QStringLiteral("//")},
        {QStringLiteral("js"), QStringLiteral("//")},
        {QStringLiteral("javascript"), QStringLiteral("//")},
        {QStringLiteral("jsx"), QStringLiteral("//")},
        {QStringLiteral("ts"), QStringLiteral("//")},
        {QStringLiteral("typescript"), QStringLiteral("//")},
        {QStringLiteral("tsx"), QStringLiteral("//")},
        {QStringLiteral("go"), QStringLiteral("//")},
        {QStringLiteral("golang"), QStringLiteral("//")},
        {QStringLiteral("rust"), QStringLiteral("//")},
        {QStringLiteral("rs"), QStringLiteral("//")},
        {QStringLiteral("sql"), QStringLiteral("--")},
        {QStringLiteral("lua"), QStringLiteral("--")},
        {QStringLiteral("haskell"), QStringLiteral("--")},
        {QStringLiteral("hs"), QStringLiteral("--")},
        {QStringLiteral("lisp"), QStringLiteral(";;")},
        {QStringLiteral("commonlisp"), QStringLiteral(";;")},
        {QStringLiteral("elisp"), QStringLiteral(";;")},
        {QStringLiteral("scheme"), QStringLiteral(";;")},
        {QStringLiteral("tex"), QStringLiteral("%")},
        {QStringLiteral("latex"), QStringLiteral("%")},
        {QStringLiteral("matlab"), QStringLiteral("%")},
    };
    return prefixes.value(language.trimmed().toLower(), QStringLiteral("#"));
}

/// `highlight: 2,5-7`, expanded and clamped to the block so a typo cannot ask
/// for a million lines.
QList<int> parseHighlight(const QString &value, int lineCount)
{
    QList<int> lines;
    const auto parts = QStringView(value).split(QLatin1Char(','), Qt::SkipEmptyParts);
    for (const QStringView part : parts) {
        const qsizetype dash = part.indexOf(QLatin1Char('-'), 1);
        const int from = (dash < 0 ? part : part.left(dash)).trimmed().toInt();
        const int to = dash < 0 ? from : part.mid(dash + 1).trimmed().toInt();
        for (int line = qMax(1, from); line <= qMin(to, lineCount); ++line)
            lines.append(line);
    }
    return lines;
}

void applyCodeOption(CodeOptions &options, const QString &key, const QString &value,
                     QString &rawHighlight)
{
    if (key == QLatin1String("title") || key == QLatin1String("filename")) {
        options.title = unquote(value);
    } else if (key == QLatin1String("caption")) {
        options.caption = unquote(value);
    } else if (key == QLatin1String("lines")) {
        bool ok = false;
        const int from = unquote(value).toInt(&ok);
        if (ok) {
            options.lineNumbers = from > 0;
            options.firstLine = qMax(1, from);
        } else {
            options.lineNumbers = isTrue(value);
        }
    } else if (key == QLatin1String("highlight")) {
        // Kept raw: the listing's length is only known once every option line
        // has been consumed, and that is what the numbers count.
        rawHighlight = unquote(value);
    } else if (key == QLatin1String("wrap")) {
        options.wrap = isTrue(value);
    } else if (key == QLatin1String("fold")) {
        options.fold = isTrue(value);
    } else if (key == QLatin1String("eval")) {
        options.eval = isTrue(value);
    } else if (key == QLatin1String("echo")) {
        options.echo = isTrue(value);
    } else if (key == QLatin1String("output")) {
        options.output = !isFalse(unquote(value));
    } else if (key == QLatin1String("include")) {
        // Quarto: include: false is echo: false and output: false together.
        if (isFalse(unquote(value))) {
            options.echo = false;
            options.output = false;
        }
    } else if (key == QLatin1String("cache")) {
        options.cache = !isFalse(unquote(value));
    } else if (key == QLatin1String("implicit")) {
        options.implicit = !isFalse(unquote(value));
    } else if (key == QLatin1String("timeout")) {
        options.timeoutSeconds = qBound(0, unquote(value).toInt(), 86'400);
    } else if (key == QLatin1String("kernel")) {
        const QString name = unquote(value).trimmed();
        if (isFalse(name))
            options.kernel = QStringLiteral("off");
        else if (isTrue(name))
            options.kernel = QStringLiteral("on");
        else
            options.kernel = name;
    } else if (key == QLatin1String("sandbox")) {
        // Anything but "host" or "wasm" keeps the tier-1 default rather than
        // failing a typo closed - "bubblewrap" written out, or a future tier's
        // name this build does not implement yet, both land here as bubblewrap.
        const QString tier = unquote(value);
        options.sandboxHost = tier.compare(QLatin1String("host"), Qt::CaseInsensitive) == 0;
        options.sandboxWasm = tier.compare(QLatin1String("wasm"), Qt::CaseInsensitive) == 0;
    } else {
        // Quarto's output, warning and error arrive here and stop - noteAppC
        // never surfaces them; keeping them only hides the lines.
        options.extra.insert(key, unquote(value));
    }
}

/// One `<prefix>| key: value` line: `^[ \t]*prefix\|[ \t]*([\w-]+)[ \t]*:[ \t]*(.*)$` by hand,
/// since compiling that expression per fenced block cost more than the parse of the block.
bool splitCodeOption(QStringView line, const QString &prefix, QStringView &key, QStringView &value)
{
    const auto skipBlanks = [&line](qsizetype at) {
        while (at < line.size() && (line.at(at) == QLatin1Char(' ') || line.at(at) == QLatin1Char('\t')))
            ++at;
        return at;
    };
    qsizetype at = skipBlanks(0);
    if (!line.mid(at).startsWith(prefix))
        return false;
    at += prefix.size();
    if (at >= line.size() || line.at(at) != QLatin1Char('|'))
        return false;
    at = skipBlanks(at + 1);
    const qsizetype keyStart = at;
    while (at < line.size() && (line.at(at).isLetterOrNumber() || line.at(at) == QLatin1Char('_')
                                || line.at(at) == QLatin1Char('-')))
        ++at;
    if (at == keyStart)
        return false;
    key = line.mid(keyStart, at - keyStart);
    at = skipBlanks(at);
    if (at >= line.size() || line.at(at) != QLatin1Char(':'))
        return false;
    value = line.mid(skipBlanks(at + 1));
    return true;
}

/// The `#|` lines at the head of a fenced block. `content` is the code between
/// the fences, which is where md4qt puts a Code item's own range.
CodeOptions parseCodeOptions(const QString &text, const Range &content, const QString &language)
{
    CodeOptions options;
    options.block = content;
    QString rawHighlight;

    const QString prefix = commentPrefix(language);

    qsizetype cursor = content.start;
    while (cursor < content.end) {
        qsizetype lineEnd = text.indexOf(QLatin1Char('\n'), cursor);
        if (lineEnd < 0 || lineEnd > content.end)
            lineEnd = content.end;
        QStringView key;
        QStringView value;
        if (!splitCodeOption(QStringView(text).mid(cursor, lineEnd - cursor), prefix, key, value))
            break;
        applyCodeOption(options, key.toString().toLower(), value.trimmed().toString(), rawHighlight);
        cursor = qMin(content.end, lineEnd + 1);
        options.range = {content.start, cursor};
    }

    // `highlight` counts the lines the reader sees, so it is clamped against
    // the listing with the option lines already taken off the front.
    if (!rawHighlight.isEmpty()) {
        const QStringView listing = QStringView(text).mid(cursor, content.end - cursor);
        const int visibleLines = listing.isEmpty()
            ? 0
            : 1 + int(listing.count(QLatin1Char('\n'))) - (listing.endsWith(QLatin1Char('\n')) ? 1 : 0);
        options.highlight = parseHighlight(rawHighlight, visibleLines);
    }
    return options;
}

/// `$$ ... $$` display maths. md4qt only reports the inline `$...$` form, so
/// the block form is found here, outside anything md4qt already claimed.
void detectDisplayMath(const QString &text, Decorations &decorations,
                       const QList<Range> &masked)
{
    static const QRegularExpression block(QStringLiteral("\\$\\$([\\s\\S]*?)\\$\\$"));

    auto it = block.globalMatch(text);
    while (it.hasNext()) {
        const auto match = it.next();
        const Range range{match.capturedStart(), match.capturedEnd()};

        const bool claimed = std::any_of(masked.cbegin(), masked.cend(),
                                         [&range](const Range &other) {
                                             return other.overlaps(range);
                                         });
        if (claimed || match.captured(1).trimmed().isEmpty())
            continue;

        MathSpan span;
        span.range = range;
        span.formula = {match.capturedStart(1), match.capturedEnd(1)};
        span.display = true;
        decorations.maths.append(span);
        decorations.syntax.append(range);

        // A formula that owns its lines is a block of its own.
        decorations.blocks.append({BlockSpan::Kind::Math, range, 0});
    }
}

/// One `key=value`, `key="quoted value"`, `key='quoted'` or bare-word token
/// from a fence's attribute tail, with an optional surrounding `{}` already
/// peeled off. `hasValue` is what tells a bare word apart from an explicit
/// empty value (`flag` against `flag=""`), which print the same but mean
/// different things to a caller.
struct FenceToken {
    QString name;
    QString value;
    bool hasValue = false;
};

/// Tokenises the part of a fence line that is not the type or the language:
/// a callout's `|` slot, a `:::` container's brace or bare form, and a code
/// fence's info string all write this same shape, so the tokenising lives
/// here once rather than in each of them. A name may contain dots when a `=`
/// follows (`syntax.keyword=#C678DD`); a bare `.a.b` is still two classes.
QList<FenceToken> tokeniseFenceSpec(QString spec)
{
    static const QRegularExpression token(
        QStringLiteral(R"(([.#]?[\w-]+(?:\.[\w-]+)+(?=[ \t]*=)|[.#]?[\w-]+)(?:[ \t]*=[ \t]*("[^"]*"|'[^']*'|\S+))?)"));

    spec = spec.trimmed();
    if (spec.startsWith(QLatin1Char('{')) && spec.endsWith(QLatin1Char('}')))
        spec = spec.mid(1, spec.size() - 2);

    QList<FenceToken> tokens;
    auto it = token.globalMatch(spec);
    while (it.hasNext()) {
        const auto match = it.next();
        FenceToken out;
        out.name = match.captured(1);
        out.hasValue = !match.captured(2).isEmpty();
        if (out.hasValue)
            out.value = unquote(match.captured(2));
        tokens.append(out);
    }
    return tokens;
}

/// What a `:::` fence line says about its container: Pandoc's
/// `{#id .class key="value"}` or the bare `name key="value"` form.
struct FenceSpec {
    QString type;
    QMap<QString, QString> attributes;
};

FenceSpec parseFenceSpec(const QString &spec)
{
    FenceSpec out;
    for (const FenceToken &token : tokeniseFenceSpec(spec)) {
        if (token.name.startsWith(QLatin1Char('#')))
            continue; // an id names nothing that renders
        if (!token.hasValue) {
            // `.note` in braces and `note` in the bare form both name the class.
            if (out.type.isEmpty())
                out.type = token.name.startsWith(QLatin1Char('.')) ? token.name.mid(1) : token.name;
            continue;
        }
        out.attributes.insert(token.name.toLower(), token.value);
    }
    return out;
}

/// A code fence's info string has no type word to peel off the way a callout
/// or a `:::` container does - CommonMark already spent that word on the
/// language - so every bare word left after it is a flag with an empty
/// value: `tab` alone means `attributes["tab"] == ""`.
QMap<QString, QString> parseFenceAttributes(const QString &spec)
{
    QMap<QString, QString> attributes;
    for (const FenceToken &token : tokeniseFenceSpec(spec))
        attributes.insert(token.name.toLower(), token.hasValue ? token.value : QString());
    return attributes;
}

/// Where a fence's attributes sit on its opening line. md4qt positions only
/// the language itself (`syntaxPos`); everything after it, to the end of that
/// line, is noteAppC's own and has to be found from the raw text rather than
/// asked of md4qt.
Range fenceAttributesRange(const Range &language, const SourceMap &map)
{
    if (!language.isValid())
        return {};
    const qsizetype lineEnd = map.lineEnd(map.lineAt(language.start));
    if (language.end >= lineEnd)
        return {};
    return {language.end, lineEnd};
}

/// `> [!note]+ Title` on the first line of a blockquote makes it a callout.
bool detectCallout(const QString &text, const Range &range, const SourceMap &map, int depth,
                   Decorations &decorations)
{
    // `[!type]`, `[!type]-`, and noteAppC's own `[!type|bar=#FF5A5Fff ...]`,
    // which rides in the metadata slot Obsidian parses and ignores.
    static const QRegularExpression head(
        QStringLiteral("^[ \\t]*>[ \\t]*(\\[!([\\w-]+)(?:\\|([^\\]]*))?\\]([+-])?)[ \\t]*(.*)$"));

    const qsizetype lineEnd = map.lineEnd(map.lineAt(range.start));
    const QString firstLine = text.mid(range.start, lineEnd - range.start);
    const auto match = head.match(firstLine);
    if (!match.hasMatch())
        return false;

    CalloutSpan callout;
    callout.range = range;
    callout.depth = depth;
    callout.type = match.captured(2).toLower();
    callout.attributes = parseFenceSpec(match.captured(3)).attributes;
    callout.foldable = !match.captured(4).isEmpty();
    callout.collapsed = match.captured(4) == QLatin1String("-");
    callout.title = match.captured(5).trimmed();
    if (callout.title.isEmpty()) {
        callout.title = callout.type;
        callout.title[0] = callout.title.at(0).toUpper();
    }
    callout.marker = {range.start + match.capturedStart(1),
                      range.start + match.capturedEnd(1)};

    // A top-level callout's header is drawn by the painter, so its whole first
    // line is syntax as far as the body layout is concerned. A nested one has
    // no header of its own to draw into, so only its marker is hidden and the
    // title stays in the layout, where the painter styles it in place.
    if (depth == 0)
        decorations.syntax.append({range.start, qMin(range.start + firstLine.size() + 1, range.end)});
    else
        decorations.syntax.append(callout.marker);
    callout.titleRange = {range.start + match.capturedStart(5),
                          range.start + match.capturedEnd(5)};
    decorations.callouts.append(callout);
    return true;
}

/// A `:::` container renders as a callout: by name for the 13 Obsidian types
/// and for `details`, as a bordered block carrying its class for the rest.
CalloutSpan calloutOf(const FenceSpec &spec, const Range &range, const Range &marker)
{
    CalloutSpan callout;
    callout.range = range;
    callout.marker = marker;
    callout.type = spec.type.toLower();
    // A disclosure spells its header `summary=`, a callout `title=`.
    callout.title = spec.attributes.value(QStringLiteral("title"),
                                          spec.attributes.value(QStringLiteral("summary")));
    if (callout.title.isEmpty() && !callout.type.isEmpty()) {
        callout.title = callout.type;
        callout.title[0] = callout.title.at(0).toUpper();
    }
    callout.attributes = spec.attributes;
    // A disclosure folds by definition; anything else only when `open` says so.
    const QString open = spec.attributes.value(QStringLiteral("open"));
    callout.foldable = !open.isEmpty() || callout.type == QLatin1String("details");
    callout.collapsed = callout.foldable && !isTrue(open);
    return callout;
}

/// Two or more adjacent depth-0 fences that each carry a `tab` attribute join
/// into one tabbed block (TODO.md, "Code block tabs"). "Adjacent" is read the
/// same way `scanColonFences` reads an unclosed container running to the end
/// of the note: nothing but whitespace may sit between them, so one stray
/// line of text or a fence with no `tab` ends the run right there. Called
/// once every fence in the file is known, which is only true once the whole
/// walk is done - a single fence cannot tell whether it is in a group by
/// looking only at itself.
void groupCodeFenceTabs(const QString &text, QList<CodeFenceSpan> &fences)
{
    const auto hasTab = [](const CodeFenceSpan &fence) {
        return fence.depth == 0 && fence.attributes.contains(QStringLiteral("tab"));
    };
    // Only spaces, tabs, CRs and newlines are allowed in the gap - which is
    // also what lets blank lines sit between two members of the same group.
    const auto onlyBlankBetween = [&text](const Range &end, const Range &start) {
        for (qsizetype i = end.end; i < start.start; ++i) {
            const QChar c = text.at(i);
            if (c != QLatin1Char(' ') && c != QLatin1Char('\t')
                && c != QLatin1Char('\n') && c != QLatin1Char('\r'))
                return false;
        }
        return true;
    };

    qsizetype i = 0;
    while (i < fences.size()) {
        if (!hasTab(fences.at(i))) {
            ++i;
            continue;
        }
        qsizetype j = i + 1;
        while (j < fences.size() && hasTab(fences.at(j))
               && onlyBlankBetween(fences.at(j - 1).block, fences.at(j).block)) {
            ++j;
        }
        // A lone `tab` fence has nobody to share a strip with, so it is left
        // as it started: not part of any group.
        if (j - i >= 2) {
            for (qsizetype k = i; k < j; ++k) {
                fences[k].tabGroup = int(i);
                fences[k].tabLabel = fences.at(k).attributes.value(QStringLiteral("tab"));
            }
        }
        i = j;
    }
}

/// `:::` containers (PLAN.md 16.1). md4qt reads the fence lines as ordinary
/// paragraph text, so they are found here in the source, where the offsets are
/// already true. Nesting goes by fence length, as in MyST: `::::` wraps `:::`.
const QRegularExpression &colonFenceRe()
{
    static const QRegularExpression fence(
        QStringLiteral("^(:{3,})[ \\t]*(\\{[^}]*\\}|[\\w-][^\\n]*)?$"),
        QRegularExpression::MultilineOption);
    return fence;
}

/// A `:::` fence's own line is noteAppC's syntax, not the note's text: its
/// `{bar="#4C8DFFff"}` attributes are not tags, nor its title links.
QList<Range> withColonFenceLines(const QString &text, QList<Range> masked)
{
    if (!text.contains(QLatin1String(":::")))
        return masked;
    auto it = colonFenceRe().globalMatch(text);
    while (it.hasNext()) {
        const auto match = it.next();
        masked.append({match.capturedStart(), match.capturedEnd()});
    }
    return masked;
}

bool maskedAt(const QList<Range> &masked, qsizetype at)
{
    return std::any_of(masked.cbegin(), masked.cend(), [at](const Range &range) { return range.contains(at); });
}

/// The character a reference like `&amp;`, `&#169;` or `&#xA9;` names (";"
/// included, as CommonMark requires), from md4qt's own table of every name HTML
/// defines. Empty for an unknown name, a number that is no character, and a
/// control character: those stay as written.
QString decodeEntity(QStringView entity)
{
    const QStringView body = entity.mid(1, entity.size() - 2);
    QString decoded;
    if (body.startsWith(QLatin1Char('#'))) {
        const bool hex = body.size() > 1 && (body.at(1) == QLatin1Char('x') || body.at(1) == QLatin1Char('X'));
        bool ok = false;
        const uint code = hex ? body.mid(2).toUInt(&ok, 16) : body.mid(1).toUInt(&ok, 10);
        if (!ok || code == 0 || code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF))
            return {};
        const char32_t point = char32_t(code);
        decoded = QString::fromUcs4(&point, 1);
    } else {
        decoded = MD::s_entityMap.value(entity.toString());
    }
    return !decoded.isEmpty() && decoded.at(0).category() == QChar::Other_Control ? QString() : decoded;
}

const QRegularExpression &entityRe()
{
    static const QRegularExpression entity(
        QStringLiteral("&(#[0-9]{1,7}|#[xX][0-9a-fA-F]{1,6}|[A-Za-z][A-Za-z0-9]{1,31});"));
    return entity;
}

/// CommonMark entity references in the note's own text, as substitutions for
/// the editor to draw as the character they name. Not in `masked` (code, maths,
/// HTML, frontmatter, links and the places md4qt left alone), nor after a backslash that
/// escapes the `&`. HTML blocks have their own pass in the editor.
void scanEntities(const QString &text, const QList<Range> &masked, Decorations &decorations)
{
    if (!text.contains(QLatin1Char('&')))
        return;
    for (auto it = entityRe().globalMatchView(text); it.hasNext();) {
        const QRegularExpressionMatch match = it.next();
        const qsizetype start = match.capturedStart();
        qsizetype slashes = 0;
        while (start - slashes > 0 && text.at(start - slashes - 1) == QLatin1Char('\\'))
            ++slashes;
        if (slashes % 2 || maskedAt(masked, start))
            continue;
        const QString decoded = decodeEntity(match.capturedView(0));
        if (!decoded.isEmpty())
            decorations.substitutions.append({{start, match.capturedEnd()}, decoded});
    }
}

/// CommonMark's backslash escapes: a backslash before ASCII punctuation is
/// not shown, only the punctuation is (`\(` is "(", `\\` is "\"). As
/// substitutions, like entities, so the caret opens one by touching it. Not in
/// `masked` (code, maths, HTML, links, the frontmatter): there a backslash is
/// the author's own.
void scanEscapes(const QString &text, QList<Range> masked, Decorations &decorations)
{
    qsizetype at = text.indexOf(QLatin1Char('\\'));
    if (at < 0)
        return;
    std::sort(masked.begin(), masked.end(), [](const Range &l, const Range &r) { return l.start < r.start; });
    qsizetype next = 0;
    qsizetype maskedEnd = 0; // the furthest end among the ranges that start at or before `at`
    while (at >= 0 && at + 1 < text.size()) {
        while (next < masked.size() && masked.at(next).start <= at)
            maskedEnd = qMax(maskedEnd, masked.at(next++).end);
        if (at < maskedEnd) {
            at = text.indexOf(QLatin1Char('\\'), maskedEnd);
            continue;
        }
        const char16_t escaped = text.at(at + 1).unicode();
        const bool punctuation = (escaped >= '!' && escaped <= '/') || (escaped >= ':' && escaped <= '@')
                                 || (escaped >= '[' && escaped <= '`') || (escaped >= '{' && escaped <= '~');
        if (punctuation)
            decorations.substitutions.append({{at, at + 2}, QString(QChar(escaped))});
        at = text.indexOf(QLatin1Char('\\'), at + (punctuation ? 2 : 1));
    }
}

/// A paragraph that holds `$$ ... $$` on lines of its own - md4qt does not end
/// a paragraph where a formula starts - is cut into the text before it, the
/// formula, and the text after, so the formula is a block the editor can draw.
/// A formula sharing its line with other text stays part of its paragraph.
void splitParagraphsAtDisplayMath(const QString &text, Decorations &decorations)
{
    QList<Range> formulas;
    for (const MathSpan &math : std::as_const(decorations.maths)) {
        const bool ownsLines = math.display && math.range.isValid() && math.range.end <= text.size()
                               && (math.range.start == 0 || text.at(math.range.start - 1) == QLatin1Char('\n'))
                               && (math.range.end == text.size() || text.at(math.range.end) == QLatin1Char('\n'));
        if (ownsLines)
            formulas.append(math.range);
    }
    if (formulas.isEmpty())
        return;
    std::sort(formulas.begin(), formulas.end(), [](const Range &l, const Range &r) { return l.start < r.start; });

    QList<BlockSpan> blocks;
    blocks.reserve(decorations.blocks.size() + formulas.size());
    for (const BlockSpan &block : std::as_const(decorations.blocks)) {
        if (block.kind != BlockSpan::Kind::Paragraph) {
            blocks.append(block);
            continue;
        }
        qsizetype from = block.range.start;
        bool split = false;
        for (const Range &formula : std::as_const(formulas)) {
            if (formula.start < from || formula.end > block.range.end)
                continue;
            if (formula.start > from)
                blocks.append({BlockSpan::Kind::Paragraph, {from, formula.start}, 0});
            blocks.append({BlockSpan::Kind::Paragraph, formula, 0});
            from = formula.end < text.size() ? formula.end + 1 : formula.end; // the formula's own newline
            split = true;
        }
        if (!split)
            blocks.append(block);
        else if (from < block.range.end && !QStringView(text).mid(from, block.range.end - from).trimmed().isEmpty())
            blocks.append({BlockSpan::Kind::Paragraph, {from, block.range.end}, 0});
    }
    decorations.blocks = blocks;
}

const QRegularExpression &htmlHeadingRe()
{
    static const QRegularExpression heading(
        QStringLiteral("<h([1-6])\\b[^>]*>(.*?)</h\\1\\s*>"),
        QRegularExpression::CaseInsensitiveOption | QRegularExpression::DotMatchesEverythingOption);
    return heading;
}

/// One match of htmlHeadingRe() as a Heading; the matched string starts at
/// `base` in the note.
Heading htmlHeadingOf(const QRegularExpressionMatch &match, qsizetype base)
{
    static const QRegularExpression tag(QStringLiteral("<[^>]*>"));
    Heading out;
    out.level = match.captured(1).toInt();
    out.range = {base + match.capturedStart(), base + match.capturedEnd()};
    out.textRange = {base + match.capturedStart(2), base + match.capturedEnd(2)};
    QString inner = match.captured(2);
    inner.remove(tag);
    // Entities last to first, so the offsets in front stay true.
    QList<QRegularExpressionMatch> entities;
    for (auto e = entityRe().globalMatch(inner); e.hasNext();)
        entities << e.next();
    for (auto e = entities.crbegin(); e != entities.crend(); ++e) {
        const QString decoded = decodeEntity(e->capturedView(0));
        if (!decoded.isEmpty())
            inner.replace(e->capturedStart(), e->capturedLength(), decoded);
    }
    out.text = inner.simplified();
    return out;
}

/// The h1-h6 of a top-level HTML block, for the outline and the index: they
/// are headings of the note however the editor draws the block (its own
/// layout, litehtml, or the source), and the Parser knows neither the editor's
/// HTML option nor gumbo. Comments are not looked into; a heading inside
/// another block element (nested tags) is found by its own tags.
void htmlHeadings(const QString &text, const Range &block, QList<Heading> &headings)
{
    static const QRegularExpression comment(QStringLiteral("<!--.*?(?:-->|$)"),
                                            QRegularExpression::DotMatchesEverythingOption);
    const QStringView view = QStringView(text).mid(block.start, block.length());
    if (view.size() < 9 || !view.contains(QLatin1String("</"))) // <h1>x</h1> is the shortest
        return;
    QString html = view.toString();
    for (auto it = comment.globalMatch(html); it.hasNext();) {
        const QRegularExpressionMatch match = it.next();
        html.replace(match.capturedStart(), match.capturedLength(), QString(match.capturedLength(), QLatin1Char(' ')));
    }
    for (auto it = htmlHeadingRe().globalMatch(html); it.hasNext();)
        headings.append(htmlHeadingOf(it.next(), block.start));
}

/// `[^label]: text` definitions, `[^label]` references and inline `^[text]`,
/// outside code, maths and comments. A definition runs on through indented
/// lines, and through lines that simply continue its paragraph.
void scanFootnotes(const QString &text, const QList<Range> &masked, Decorations &decorations)
{
    if (!text.contains(QLatin1String("[^")) && !text.contains(QLatin1String("^[")))
        return;
    static const QRegularExpression definition(QStringLiteral(R"(^\[\^([^\]\s]+)\]:[ \t]?)"),
                                               QRegularExpression::MultilineOption);
    static const QRegularExpression reference(QStringLiteral(R"(\[\^([^\]\s]+)\](?!:))"));

    const auto lineEndAt = [&text](qsizetype from) {
        const qsizetype at = text.indexOf(QLatin1Char('\n'), from);
        return at < 0 ? text.size() : at;
    };
    const auto isIndented = [&text](qsizetype from) {
        return text.mid(from, 1) == QLatin1String("\t") || text.mid(from, 4) == QLatin1String("    ");
    };
    QHash<QString, int> byLabel; // label -> index into footnoteDefinitions
    auto defs = definition.globalMatch(text);
    while (defs.hasNext()) {
        const auto match = defs.next();
        if (maskedAt(masked, match.capturedStart()))
            continue;
        qsizetype end = lineEndAt(match.capturedEnd());
        for (qsizetype line = end + 1; line < text.size();) {
            const qsizetype lineEnd = lineEndAt(line);
            const QStringView view = QStringView(text).mid(line, lineEnd - line);
            if (view.trimmed().isEmpty()) {
                // A blank line ends it, unless an indented line carries it on.
                qsizetype next = lineEnd + 1;
                while (next < text.size() && QStringView(text).mid(next, lineEndAt(next) - next).trimmed().isEmpty())
                    next = lineEndAt(next) + 1;
                if (next >= text.size() || !isIndented(next))
                    break;
                line = next;
                continue;
            }
            if (definition.match(view.toString()).hasMatch())
                break; // the next definition
            end = lineEnd;
            line = lineEnd + 1;
        }
        while (end > match.capturedEnd() && text.at(end - 1).isSpace())
            --end;
        byLabel.insert(match.captured(1), int(decorations.footnoteDefinitions.size()));
        decorations.footnoteDefinitions.append(
            {match.captured(1), {match.capturedStart(), end}, {match.capturedEnd(), end}});
    }

    QList<FootnoteSpan> found;
    auto refs = reference.globalMatch(text);
    while (refs.hasNext()) {
        const auto match = refs.next();
        const auto def = byLabel.constFind(match.captured(1));
        if (def == byLabel.cend() || maskedAt(masked, match.capturedStart()))
            continue;
        const FootnoteDefinition &target = decorations.footnoteDefinitions.at(*def);
        found.append({{match.capturedStart(), match.capturedEnd()}, match.captured(1), target.content, 0});
    }
    // ^[inline], its brackets balanced so a [[link]] inside does not end it.
    for (qsizetype at = text.indexOf(QLatin1String("^[")); at >= 0; at = text.indexOf(QLatin1String("^["), at + 2)) {
        if (maskedAt(masked, at))
            continue;
        int depth = 0;
        qsizetype close = -1;
        for (qsizetype i = at + 1; i < text.size(); ++i) {
            const QChar c = text.at(i);
            if (c == QLatin1Char('\n') && i + 1 < text.size() && text.at(i + 1) == QLatin1Char('\n'))
                break; // not past a paragraph
            if (c == QLatin1Char('['))
                ++depth;
            else if (c == QLatin1Char(']') && --depth == 0) {
                close = i;
                break;
            }
        }
        if (close > at + 2)
            found.append({{at, close + 1}, QString(), {at + 2, close}, 0});
    }
    std::sort(found.begin(), found.end(),
              [](const FootnoteSpan &a, const FootnoteSpan &b) { return a.range.start < b.range.start; });
    QHash<QString, int> numbers;
    int next = 0;
    for (FootnoteSpan &span : found) {
        if (span.label.isEmpty())
            span.number = ++next;
        else
            span.number = numbers.contains(span.label) ? numbers.value(span.label)
                                                       : numbers.insert(span.label, ++next).value();
    }
    decorations.footnotes = found;
    for (FootnoteDefinition &definition : decorations.footnoteDefinitions)
        definition.number = numbers.value(definition.label, 0);

    // The editor shows a reference as its number, superscript, and a
    // definition's marker as a small muted number - neither is real text
    // DisplayMap can substitute, so one carrier character (the opening "["
    // or "^") is left visible and the rest hidden like any other syntax;
    // the editor paints the number over the carrier. Only markers on a line
    // the caret is not touching stay hidden, the same as bold or a link.
    for (const FootnoteSpan &span : found) {
        decorations.syntax.append({span.range.start + 1, span.range.end});
        decorations.styles.append({StyleSpan::Kind::Footnote, span.range, 0, span.label, {}, {}});
    }
    for (const FootnoteDefinition &definition : decorations.footnoteDefinitions) {
        if (definition.number <= 0)
            continue; // never referenced: shown as written, nothing to number
        const Range marker{definition.range.start, definition.content.start};
        decorations.syntax.append({marker.start + 1, marker.end});
        decorations.styles.append({StyleSpan::Kind::Footnote, marker, 0, definition.label, {}, {}});
    }
}

/// Characters that already mean something in Markdown: `*`/`_` emphasis, `~`
/// strikethrough, `=` highlight, a backtick code, `$` maths, `%` comments, `#`
/// tags and headings, `|` table cells.
constexpr QLatin1StringView metaNoteReserved{"*_~=`$%#|"};

/// A line that starts a block of its own - a heading, a list item, a fence -
/// so a meta note cannot run into it from the line above. `at` is the line's
/// first character; quote markers are looked past.
bool startsBlock(const QString &text, qsizetype at)
{
    const qsizetype size = text.size();
    while (at < size && (text.at(at) == QLatin1Char(' ') || text.at(at) == QLatin1Char('>')))
        ++at;
    if (at >= size)
        return false;
    const QChar c = text.at(at);
    const auto spaceAfter = [&](qsizetype i) {
        return i < size && (text.at(i) == QLatin1Char(' ') || text.at(i) == QLatin1Char('\t'));
    };
    if (c == QLatin1Char('#')) {
        qsizetype i = at;
        while (i < size && text.at(i) == QLatin1Char('#') && i - at < 6)
            ++i;
        return spaceAfter(i);
    }
    if (c == QLatin1Char('-') || c == QLatin1Char('*') || c == QLatin1Char('+'))
        return spaceAfter(at + 1);
    if (c.isDigit()) {
        qsizetype i = at;
        while (i < size && text.at(i).isDigit() && i - at < 9)
            ++i;
        return i < size && (text.at(i) == QLatin1Char('.') || text.at(i) == QLatin1Char(')')) && spaceAfter(i + 1);
    }
    const QStringView three = QStringView(text).mid(at, 3);
    return three == QLatin1String("```") || three == QLatin1String("~~~");
}

/// `- [x]`, `1. [ ]`: a task box, not a note. `at` is the opening "[".
bool isTaskBox(const QString &text, qsizetype at)
{
    qsizetype i = at;
    while (i > 0 && text.at(i - 1) == QLatin1Char(' '))
        --i;
    if (i == at || i == 0)
        return false; // no space before it, or nothing: no list marker
    qsizetype marker = i - 1;
    if (text.at(marker) == QLatin1Char('.') || text.at(marker) == QLatin1Char(')')) {
        while (marker > 0 && text.at(marker - 1).isDigit())
            --marker;
        if (marker == i - 1)
            return false;
    } else if (text.at(marker) != QLatin1Char('-') && text.at(marker) != QLatin1Char('*')
               && text.at(marker) != QLatin1Char('+')) {
        return false;
    }
    while (marker > 0 && (text.at(marker - 1) == QLatin1Char(' ') || text.at(marker - 1) == QLatin1Char('>')))
        --marker;
    return marker == 0 || text.at(marker - 1) == QLatin1Char('\n');
}

/// Runs of prose between a pair of delimiters, `[like this]` or whatever the
/// caller passes: bare brackets, meta notes and the word count's exclusion all
/// come from here. One pass over the text with a stack of open delimiters, so
/// it is linear however many are left unclosed. A run holds nested pairs
/// (`[a [b] c]` is one run, the outermost), may span soft line breaks, and
/// ends at a blank line, a line that starts another block, or - in a table -
/// the cell. `masked`, sorted, is what the pass must not look inside: code,
/// maths, HTML, links, footnotes, comments. With the brackets themselves, a
/// run is not one if it is really a link (`[a](u)`, `[a][r]`, `[a][]`), a
/// link definition (`[a]: u`), a task box or empty, and an escaped `\[` does
/// not open one. Sorted by position.
QList<Range> scanRuns(const QString &text, const QList<Range> &masked, const QString &open,
                      const QString &close, const QList<TableSpan> &tables)
{
    const bool same = open == close;
    const bool brackets = open == QLatin1String("[") && close == QLatin1String("]");
    if (!text.contains(open) || (!same && !text.contains(close)))
        return {};

    qsizetype maskIndex = 0;
    QList<Range> found;
    // Closed runs under a delimiter still open: kept only if it never closes.
    QList<Range> pending;
    QList<qsizetype> stack;
    const auto flush = [&] {
        found += pending;
        pending.clear();
        stack.clear();
    };
    const auto addRun = [&](Range run) {
        // A run swallows the ones inside it.
        while (!pending.isEmpty() && pending.last().start >= run.start)
            pending.removeLast();
        if (stack.isEmpty())
            found.append(run);
        else
            pending.append(run);
    };

    const qsizetype size = text.size();
    qsizetype tableIndex = 0;
    const auto inTable = [&](qsizetype at) {
        while (tableIndex < tables.size() && tables.at(tableIndex).range.end <= at)
            ++tableIndex;
        return tableIndex < tables.size() && tables.at(tableIndex).range.start <= at;
    };

    for (qsizetype i = 0; i < size;) {
        while (maskIndex < masked.size() && masked.at(maskIndex).end <= i)
            ++maskIndex;
        // Masked ranges overlap and nest: the furthest end of those holding i.
        qsizetype skipTo = i;
        for (qsizetype m = maskIndex; m < masked.size() && masked.at(m).start <= i; ++m)
            skipTo = qMax(skipTo, masked.at(m).end);
        if (skipTo > i) {
            i = skipTo;
            continue;
        }
        const QChar c = text.at(i);
        if (c == QLatin1Char('\\')) {
            // A backslash before a newline is a hard break, not an escape.
            i += i + 1 < size && text.at(i + 1) != QLatin1Char('\n') ? 2 : 1;
            continue;
        }
        if (c == QLatin1Char('\n')) {
            qsizetype next = i + 1;
            qsizetype line = next;
            while (line < size && (text.at(line) == QLatin1Char(' ') || text.at(line) == QLatin1Char('\t')))
                ++line;
            if (line >= size || text.at(line) == QLatin1Char('\n') || startsBlock(text, next) || inTable(i))
                flush();
            i = next;
            continue;
        }
        if (c == QLatin1Char('|') && inTable(i)) {
            flush(); // a table cell ends the note
            ++i;
            continue;
        }

        // A closer wins over an opener where both would match, so `>>` ends
        // a `<<` note rather than starting one.
        const bool closes = !stack.isEmpty() && QStringView(text).mid(i, close.size()) == QStringView(close);
        const bool opens = !closes && QStringView(text).mid(i, open.size()) == QStringView(open);
        if (same && (closes || opens)) {
            // Identical delimiters pair left to right.
            if (stack.isEmpty()) {
                stack.append(i);
                i += open.size();
            } else {
                const Range run{stack.takeLast(), i + close.size()};
                addRun(run);
                i += close.size();
            }
            continue;
        }
        if (opens) {
            // A bare URL is not scanned: its `[1]` or `{y}` is part of it.
            qsizetype word = i;
            while (word > 0 && i - word < 256 && !text.at(word - 1).isSpace())
                --word;
            const bool skip = QStringView(text).mid(word, i - word).contains(QLatin1String("://"));
            if (!skip)
                stack.append(i);
            i += open.size();
            continue;
        }
        if (closes) {
            const qsizetype start = stack.takeLast();
            const qsizetype end = i + close.size();
            i = end;
            if (brackets) {
                const QChar after = end < size ? text.at(end) : QChar();
                // `[text][ref]` and `[text][]`: the second pair is the label,
                // closed on the same line; an unclosed `[` after it is not one.
                const qsizetype closer = after == QLatin1Char('[') ? text.indexOf(QLatin1Char(']'), end) : -1;
                const qsizetype eol = closer < 0 ? -1 : text.indexOf(QLatin1Char('\n'), end);
                if (closer >= 0 && (eol < 0 || closer < eol)) {
                    i = closer + 1;
                    while (!pending.isEmpty() && pending.last().start >= start)
                        pending.removeLast();
                    continue;
                }
                const bool lineStart = [&] {
                    qsizetype b = start;
                    while (b > 0 && text.at(b - 1) == QLatin1Char(' '))
                        --b;
                    return (b == 0 || text.at(b - 1) == QLatin1Char('\n')) && start - b < 4;
                }();
                if (after == QLatin1Char('(') || (after == QLatin1Char(':') && lineStart)) {
                    if (after == QLatin1Char(':')) { // a link definition owns its line
                        const qsizetype eol = text.indexOf(QLatin1Char('\n'), end);
                        i = eol < 0 ? size : eol;
                    }
                    while (!pending.isEmpty() && pending.last().start >= start)
                        pending.removeLast();
                    continue;
                }
                const QStringView inner = QStringView(text).mid(start + 1, end - start - 2);
                if (inner.trimmed().isEmpty() || (inner.size() == 1 && isTaskBox(text, start)))
                    continue;
            } else if (QStringView(text).mid(start + open.size(), i - close.size() - start - open.size()).trimmed().isEmpty()) {
                continue; // `{}` says nothing
            }
            addRun({start, end});
            continue;
        }
        ++i;
    }
    flush();
    std::sort(found.begin(), found.end(), [](const Range &a, const Range &b) { return a.start < b.start; });
    return found;
}

void scanColonFences(const QString &text, const QList<Range> &masked, Decorations &decorations)
{
    const QRegularExpression &fence = colonFenceRe();

    struct Open {
        qsizetype length = 0;
        Range marker;
        FenceSpec spec;
    };
    QList<Open> stack;
    QList<CalloutSpan> found;

    const auto close = [&stack, &found, &decorations](const Range &end, qsizetype length) {
        while (!stack.isEmpty()) {
            const int depth = int(stack.size()) - 1;
            const Open open = stack.takeLast();
            decorations.syntax.append(open.marker);
            CalloutSpan span = calloutOf(open.spec, {open.marker.start, end.end}, open.marker);
            span.depth = depth;
            found.append(span);
            if (open.length <= length)
                break;
        }
    };

    auto it = fence.globalMatch(text);
    while (it.hasNext()) {
        const auto match = it.next();
        const Range line{match.capturedStart(), qMin(text.size(), match.capturedEnd() + 1)};
        const bool inCode = std::any_of(masked.cbegin(), masked.cend(), [&line](const Range &range) {
            return range.contains(line.start);
        });
        if (inCode)
            continue;

        const qsizetype length = match.captured(1).size();
        const QString spec = match.captured(2).trimmed();
        // A bare row of colons closes the innermost fence it is long enough
        // for; with nothing open it starts an unnamed container instead.
        const bool closes = spec.isEmpty()
            && std::any_of(stack.cbegin(), stack.cend(),
                           [length](const Open &open) { return open.length <= length; });
        if (!closes) {
            stack.append({length, line, parseFenceSpec(spec)});
            continue;
        }
        close(line, length);
        decorations.syntax.append(line);
    }
    // An unclosed container runs to the end of the note, the way a code fence does.
    close({text.size(), text.size()}, 0);

    if (found.isEmpty())
        return;
    // The container is the block now, so whatever md4qt made of its lines is not.
    decorations.blocks.removeIf([&found](const BlockSpan &block) {
        return std::any_of(found.cbegin(), found.cend(), [&block](const CalloutSpan &callout) {
            return block.range.start >= callout.range.start
                   && block.range.end <= callout.range.end;
        });
    });
    for (const CalloutSpan &callout : std::as_const(found)) {
        decorations.blocks.append({BlockSpan::Kind::Callout, callout.range, 0});
        decorations.callouts.append(callout);
    }
}

/// `[ref]: https://example.com "Title"` - the other half of a reference link.
/// md4qt swallows the line without reporting it, so live preview has to find it
/// to hide it.
void hideLinkDefinitions(const QString &text, const QList<Range> &masked,
                         Decorations &decorations)
{
    static const QRegularExpression definition(
        QStringLiteral(R"(^ {0,3}\[(?!\^)[^\]\n]+\]:[ \t]*\S[^\n]*$)"), // "[^1]:" is a footnote
        QRegularExpression::MultilineOption);

    auto it = definition.globalMatch(text);
    while (it.hasNext()) {
        const auto match = it.next();
        const Range line{match.capturedStart(), qMin(text.size(), match.capturedEnd() + 1)};
        const bool inCode = std::any_of(masked.cbegin(), masked.cend(), [&line](const Range &range) {
            return range.contains(line.start);
        });
        if (!inCode)
            decorations.syntax.append(line);
    }
}

/// CommonMark folds a reference label's case and collapses its internal
/// whitespace before matching it against a definition.
QString normaliseLabel(const QString &label)
{
    return label.simplified().toCaseFolded();
}

/// `[ref]: url "title"` again - the same line `hideLinkDefinitions()` finds,
/// this time captured into label and URL so `[text][ref]` can be resolved to
/// what the definition actually points at.
QHash<QString, QString> collectLinkDefinitions(const QString &text, const QList<Range> &masked)
{
    static const QRegularExpression definition(
        QStringLiteral(R"(^ {0,3}\[(?!\^)([^\]\n]+)\]:[ \t]*(\S[^\n]*)$)"),
        QRegularExpression::MultilineOption);

    QHash<QString, QString> definitions;
    auto it = definition.globalMatch(text);
    while (it.hasNext()) {
        const auto match = it.next();
        const Range line{match.capturedStart(), qMin(text.size(), match.capturedEnd() + 1)};
        const bool inCode = std::any_of(masked.cbegin(), masked.cend(), [&line](const Range &range) {
            return range.contains(line.start);
        });
        if (inCode)
            continue;

        // Only the first token after the colon is the destination; a title
        // may follow it, quoted or in parentheses.
        QString url = match.captured(2);
        const qsizetype space = url.indexOf(QRegularExpression(QStringLiteral("[ \\t]")));
        if (space > 0)
            url = url.left(space);
        if (url.size() >= 2 && url.startsWith(QLatin1Char('<')) && url.endsWith(QLatin1Char('>')))
            url = url.mid(1, url.size() - 2);

        const QString label = normaliseLabel(match.captured(1));
        if (!definitions.contains(label)) // the first definition for a label wins
            definitions.insert(label, url);
    }
    return definitions;
}

void walkBlock(MD::Block *block, Walk &walk)
{
    if (!block)
        return;
    for (const auto &child : block->items())
        walkItem(child.data(), walk);
}

void walkItem(MD::Item *item, Walk &walk)
{
    if (!item)
        return;
    const SourceMap &map = *walk.map;

    if (auto *withOpts = dynamic_cast<MD::ItemWithOpts *>(item))
        collectStyles(withOpts, walk);

    switch (item->type()) {
    case MD::ItemType::HorizontalLine:
        // `---`, `***`, `___`: a rule, drawn as one; its characters are syntax.
        if (walk.depth == 0) {
            const Range range = rangeOf(*item, map);
            walk.decorations.blocks.append({BlockSpan::Kind::Rule, range, 0});
            walk.decorations.syntax.append(range);
        }
        break;
    case MD::ItemType::Heading: {
        auto *heading = static_cast<MD::Heading *>(item);
        Heading out;
        out.level = heading->level();
        out.range = rangeOf(*heading, map);
        if (heading->text()) {
            out.text = plainText(heading->text().data());
            out.textRange = rangeOf(*heading->text(), map);
        }
        walk.headings.append(out);
        walk.decorations.blocks.append({BlockSpan::Kind::Heading, out.range, out.level});
        for (const auto &delim : heading->delims()) {
            Range marker = rangeOf(delim, map);
            // The space between "##" and the text is part of the marker as far
            // as live preview is concerned, and so is the space before a
            // closing "##".
            if (out.textRange.isValid()) {
                if (marker.end <= out.textRange.start)
                    marker.end = out.textRange.start;
                else if (marker.start >= out.textRange.end)
                    marker.start = out.textRange.end;
            }
            walk.decorations.syntax.append(marker);
        }
        if (out.textRange.isValid()) {
            walk.decorations.styles.append(
                {StyleSpan::Kind::Heading, out.textRange, out.level, out.text, {}, {}});
        }
        if (heading->text())
            walkBlock(heading->text().data(), walk);
        break;
    }
    case MD::ItemType::Code:
    case MD::ItemType::Math:
    case MD::ItemType::RawHtml: {
        const Range range = rangeOf(*item, map);
        walk.masked.append(range);

        const auto kind = item->type() == MD::ItemType::Math      ? StyleSpan::Kind::Math
                          : item->type() == MD::ItemType::RawHtml ? StyleSpan::Kind::Html
                                                                  : StyleSpan::Kind::InlineCode;
        if (item->type() == MD::ItemType::Math) {
            auto *math = static_cast<MD::Code *>(item);
            const Range open = rangeOf(math->startDelim(), map);
            const Range close = rangeOf(math->endDelim(), map);
            MathSpan span;
            // Unlike `range` above - which, for a Math item, is the LaTeX
            // alone, md4qt's own start/end excluding both delimiters -
            // `span.range` carries the delimiters too, matching every other
            // caller's assumption that it is the whole "$...$" or "$$...$$".
            span.range = {open.isValid() ? open.start : range.start,
                          close.isValid() ? close.end : range.end};
            span.display = !math->isInline();
            span.formula = {open.isValid() ? open.end : range.start,
                            close.isValid() ? close.start : range.end};
            walk.decorations.maths.append(span);
        }
        if (item->type() == MD::ItemType::Code || item->type() == MD::ItemType::Math) {
            auto *code = static_cast<MD::Code *>(item);
            const bool block = !code->isInline();
            walk.decorations.styles.append(
                {block ? StyleSpan::Kind::CodeBlock : kind, range, 0, code->syntax(), {}, {}});
            if (item->type() == MD::ItemType::Math && code->isInline()) {
                // Inline maths hides everything from just after the opening
                // "$" through the closing one, so live preview keeps that one
                // "$" in the display string as the anchor an inline formula
                // image gets drawn over - the editor gives it letter spacing
                // equal to the image's width. A formula the renderer cannot
                // draw is handled by the editor leaving this range out of
                // what it actually hides, not by anything decided here.
                //
                // `rangeOf(*item, map)` is the LaTeX alone - md4qt's own
                // start/end for a Math item exclude both delimiters, unlike
                // its comment above suggests - so the closing "$" comes from
                // its own endDelim(), the same source `formula` uses below.
                const Range open = rangeOf(code->startDelim(), map);
                const Range close = rangeOf(code->endDelim(), map);
                walk.decorations.syntax.append(
                    {open.isValid() ? open.end : range.start, close.isValid() ? close.end : range.end});
            } else {
                // The fences are syntax; their content is not.
                walk.decorations.syntax.append(rangeOf(code->startDelim(), map));
                walk.decorations.syntax.append(rangeOf(code->endDelim(), map));
            }
            if (code->syntaxPos().startLine() >= 0)
                walk.decorations.syntax.append(rangeOf(code->syntaxPos(), map));
            if (block && walk.depth == 0)
                walk.decorations.blocks.append({BlockSpan::Kind::Code, range, 0});
            if (block && walk.text) {
                // `#|` options belong to the file but not to the listing.
                const CodeOptions options = parseCodeOptions(*walk.text, range, code->syntax());
                if (options.range.isValid()) {
                    walk.decorations.syntax.append(options.range);
                    walk.decorations.codeOptions.append(options);
                }
            }
            if (item->type() == MD::ItemType::Code && code->isFensedCode() && block && walk.text) {
                // Tab groups are resolved once every fence in the file is
                // known, so only the per-fence half of the work happens here;
                // see groupCodeFenceTabs, called at the end of parseAll.
                CodeFenceSpan fence;
                fence.content = range;
                const Range startDelim = rangeOf(code->startDelim(), map);
                const Range endDelim = rangeOf(code->endDelim(), map);
                // An unclosed fence, like an unclosed `:::` container, runs to
                // wherever md4qt stopped reading content - there is no closing
                // delimiter to measure instead.
                fence.block = {startDelim.isValid() ? startDelim.start : range.start,
                               endDelim.isValid() ? endDelim.end : range.end};
                fence.language = code->syntax();
                fence.depth = walk.depth;
                fence.attributesRange = fenceAttributesRange(rangeOf(code->syntaxPos(), map), map);
                if (fence.attributesRange.isValid()) {
                    fence.attributes = parseFenceAttributes(
                        walk.text->mid(fence.attributesRange.start, fence.attributesRange.length()));
                    walk.decorations.syntax.append(fence.attributesRange);
                }
                walk.decorations.codeFences.append(fence);
            }
        } else {
            walk.decorations.styles.append({kind, range, 0, {}, {}, {}});
            if (walk.depth == 0) {
                walk.decorations.blocks.append({BlockSpan::Kind::Html, range, 0});
                htmlHeadings(*walk.text, range, walk.headings);
            } else if (walk.text) {
                HTMLimageEmbed(*walk.text, range, walk.decorations);
            }
        }
        break;
    }
    case MD::ItemType::Link:
    case MD::ItemType::Image: {
        // MD::Link and MD::Image are siblings, not parent and child; only what
        // they share is reachable from here.
        auto *link = static_cast<MD::LinkBase *>(item);
        Link out;
        out.wiki = false;
        out.embed = item->type() == MD::ItemType::Image;
        out.alias = link->text();
        out.range = rangeOf(*link, map);
        out.targetRange = rangeOf(link->urlPos(), map);

        // md4qt resolves a relative URL against the file's directory, which
        // would store "/home/.../PLAN.md" where the note says "PLAN.md".
        // The index needs what the source says, so a rename rewrites it and
        // so resolution follows Obsidian's rules rather than the filesystem's.
        QString target = out.targetRange.isValid() && walk.text
                             ? walk.text->mid(out.targetRange.start, out.targetRange.length())
                             : link->url();

        // A reference-style link or embed has no URL at this position at
        // all: md4qt puts the *label* at urlPos, because the real target
        // lives on a `[ref]: url` line elsewhere, and its own url() is an
        // internal bookkeeping marker rather than that URL. Every such form -
        // `[text][ref]`, `[ref][]`, `[ref]` - opens with "[" immediately
        // before the label, which an inline link's "(" or an autolink's "<"
        // never does, so that one character tells them apart.
        const bool referenceStyle = out.targetRange.start > 0 && walk.text
            && walk.text->at(out.targetRange.start - 1) == QLatin1Char('[');

        if (referenceStyle) {
            walk.links.append(out);
            walk.pendingReferences.append({walk.links.size() - 1, target});
        } else {
            // CommonMark's `<path with spaces.md>` form: the brackets are
            // syntax, not part of the target.
            if (target.size() >= 2 && target.startsWith(QLatin1Char('<')) && target.endsWith(QLatin1Char('>')))
                target = target.mid(1, target.size() - 2);
            const qsizetype hash = target.indexOf(QLatin1Char('#'));
            if (hash > 0) {
                out.subpath = target.mid(hash);
                target = target.left(hash);
            }
            out.raw = target;
            out.target = QUrl::fromPercentEncoding(target.toUtf8());
            if (out.embed)
                walk.decorations.embeds.append(embedOf(out));
            out.external = ObsidianSyntax::isExternalTarget(out.target);
            walk.links.append(out);
            walk.decorations.styles.append({StyleSpan::Kind::Link, out.range, 0, out.target, {}, {}});
        }

        // Everything around the label is syntax: the `[` or `![`, then the
        // `](url "title")`, the `][ref]` of a reference link, or the angle
        // brackets of an autolink. md4qt reports the label the same way for
        // all of them, so one rule covers the lot.
        const Range label = rangeOf(link->textPos(), map);
        if (label.isValid() && label.length() > 0 && label.start >= out.range.start
            && label.end <= out.range.end) {
            walk.decorations.syntax.append({out.range.start, label.start});
            walk.decorations.syntax.append({label.end, out.range.end});
        } else {
            // `[](url)` has nothing to show; only the embed or the URL remains.
            walk.decorations.syntax.append(out.range);
        }

        // A link's own label can hold emphasis but never a wikilink or a tag.
        walk.masked.append(out.range);
        break;
    }
    case MD::ItemType::Blockquote: {
        const Range range = rangeOf(*item, map);
        const bool callout = walk.text && detectCallout(*walk.text, range, map, walk.depth, walk.decorations);
        if (walk.depth == 0) {
            walk.decorations.blocks.append(
                {callout ? BlockSpan::Kind::Callout : BlockSpan::Kind::Quote, range, 0});
        }
        // The "> " that starts each line is syntax, hidden the way Obsidian
        // hides it in live preview.
        if (walk.text)
            hideQuoteMarkers(*walk.text, range, walk.decorations);
        ++walk.depth;
        walkBlock(static_cast<MD::Block *>(item), walk);
        --walk.depth;
        break;
    }
    case MD::ItemType::List:
    case MD::ItemType::Paragraph:
    case MD::ItemType::Footnote:
    case MD::ItemType::TableCell: {
        if (item->type() == MD::ItemType::Paragraph && !walk.inFootnote)
            walk.decorations.paragraphs.append(rangeOf(*item, map));
        if (walk.depth == 0) {
            const auto kind = item->type() == MD::ItemType::List ? BlockSpan::Kind::List
                                                                 : BlockSpan::Kind::Paragraph;
            walk.decorations.blocks.append({kind, rangeOf(*item, map), 0});
        }
        const bool list = item->type() == MD::ItemType::List;
        if (list)
            walk.lists.append(Walk::ListFrame{});
        ++walk.depth;
        walkBlock(static_cast<MD::Block *>(item), walk);
        --walk.depth;
        if (list)
            walk.lists.removeLast();
        break;
    }
    case MD::ItemType::ListItem: {
        auto *listItem = static_cast<MD::ListItem *>(item);
        if (walk.lists.isEmpty())
            walk.lists.append(Walk::ListFrame{}); // md4qt always wraps items in a List; never trust it blindly
        Walk::ListFrame &frame = walk.lists.last();
        const bool ordered = listItem->listType() == MD::ListItem::Ordered;
        int shown = 0;
        QString outline;
        if (ordered) {
            shown = frame.items == 0 ? listItem->startNumber() : frame.last + 1;
            frame.last = shown;
            const QString parent = walk.itemOutlines.isEmpty() ? QString() : walk.itemOutlines.last();
            outline = parent.isEmpty() ? QString::number(shown) : parent + QLatin1Char('.') + QString::number(shown);
        }
        ++frame.items;
        const qsizetype itemsBefore = walk.decorations.listItems.size();
        if (walk.text)
            describeListItem(*walk.text, rangeOf(*listItem, map), map, int(walk.lists.size()) - 1, shown,
                             outline, walk.anyTaskState, walk.decorations);
        // md4qt only knows `[ ]` and `[x]`; with task states our own span says
        // what a box holds, and the box is still in md4qt's text for the rest.
        const bool customBox = walk.anyTaskState && walk.decorations.listItems.size() > itemsBefore
                               && walk.decorations.listItems.last().task;
        if (customBox || listItem->isTaskList()) {
            Task task;
            task.state = customBox ? walk.decorations.listItems.last().state
                                   : QLatin1Char(listItem->isChecked() ? 'x' : ' ');
            task.range = rangeOf(*listItem, map);
            if (!listItem->items().isEmpty()
                && listItem->items().first()->type() == MD::ItemType::Paragraph) {
                task.text = plainText(static_cast<MD::Paragraph *>(
                    listItem->items().first().data()));
                if (!listItem->isTaskList()) {
                    static const QRegularExpression box(QStringLiteral("^\\[[^\\[\\]\\x{10000}-\\x{10FFFF}]\\][ \\t]+"));
                    task.text.remove(box);
                }
            }
            walk.tasks.append(task);
        }
        walk.itemOutlines.append(outline);
        walkBlock(listItem, walk);
        walk.itemOutlines.removeLast();
        break;
    }
    case MD::ItemType::Table: {
        auto *table = static_cast<MD::Table *>(item);
        const Range tableRange = rangeOf(*table, map);
        if (walk.depth == 0)
            walk.decorations.blocks.append({BlockSpan::Kind::Table, tableRange, 0});

        TableSpan span;
        span.range = tableRange;
        span.columns = table->columnsCount();
        for (int column = 0; column < span.columns; ++column) {
            switch (table->columnAlignment(column)) {
            case MD::Table::AlignRight:
                span.alignment << Qt::AlignRight;
                break;
            case MD::Table::AlignCenter:
                span.alignment << Qt::AlignHCenter;
                break;
            default:
                span.alignment << Qt::AlignLeft;
                break;
            }
        }
        // Row by row from the source lines, the delimiter row skipped, since
        // md4qt's cell positions go wrong for an empty cell.
        qsizetype line = map.lineAt(tableRange.start);
        for (qsizetype r = 0; r < table->rows().size(); ++r, ++line) {
            if (r == 1)
                ++line;
            if (walk.text && line < map.lineCount()) {
                span.rows << tableRowCells(*walk.text, map.lineStart(line), map.lineEnd(line));
                continue;
            }
            QList<Range> cells;
            for (const auto &cell : table->rows().at(r)->cells())
                cells << rangeOf(*cell, map);
            span.rows << cells;
        }
        walk.decorations.tables.append(span);

        ++walk.depth;
        for (const auto &row : table->rows())
            walkItem(row.data(), walk);
        --walk.depth;
        break;
    }
    case MD::ItemType::TableRow:
        // A row is an Item holding cells, not a Block: casting it to one read
        // the cell list through Block's layout (UBSan: downcast of an address
        // that is not a Block).
        for (const auto &cell : static_cast<MD::TableRow *>(item)->cells())
            walkItem(cell.data(), walk);
        break;
    default:
        break;
    }
}

int countWords(QStringView text)
{
    int words = 0;
    bool inWord = false;
    for (const QChar c : text) {
        const bool part = c.isLetterOrNumber() || c == QLatin1Char('_') || c == QLatin1Char('\'');
        if (part && !inWord)
            ++words;
        inWord = part;
    }
    return words;
}

/// What the counts leave out: `excluded`, and with `ignoreFormatting` the
/// markers live preview hides (heading and list markers, link targets and
/// brackets, fences, `%%` comments - their text too, as it is not rendered),
/// a callout's marker, a task's box and every HTML tag. Sorted and merged.
QList<Range> uncountedRanges(const QString &text, QList<Range> ranges, bool ignoreFormatting,
                             const Decorations &decorations, qsizetype from)
{
    if (ignoreFormatting) {
        ranges += decorations.syntax;
        for (const ListItemSpan &item : decorations.listItems)
            ranges.append(item.marker);
        for (const CalloutSpan &callout : decorations.callouts)
            ranges.append(callout.marker);
        static const QRegularExpression tag(QStringLiteral("<!--.*?-->|<[^>]*>"),
                                            QRegularExpression::DotMatchesEverythingOption);
        for (const StyleSpan &span : decorations.styles) {
            if (span.kind != StyleSpan::Kind::Html)
                continue;
            for (auto it = tag.globalMatchView(QStringView(text).mid(span.range.start, span.range.length())); it.hasNext();) {
                const auto match = it.next();
                ranges.append({span.range.start + match.capturedStart(), span.range.start + match.capturedEnd()});
            }
        }
    }
    // Only what lies after the frontmatter, which is not counted at all.
    for (Range &range : ranges)
        range.start = qMax(range.start, from);
    ranges.removeIf([&text](const Range &range) { return range.length() <= 0 || range.end > text.size(); });
    std::sort(ranges.begin(), ranges.end(), [](const Range &a, const Range &b) { return a.start < b.start; });
    QList<Range> merged;
    for (const Range &range : std::as_const(ranges)) {
        if (!merged.isEmpty() && range.start <= merged.last().end)
            merged.last().end = qMax(merged.last().end, range.end);
        else
            merged.append(range);
    }
    return merged;
}

} // namespace

struct Parser::Private {
    mutable MD::Parser parser;
};

Parser::Parser()
    : d(std::make_unique<Private>())
{
}

Parser::~Parser() = default;

QString metaNoteDelimitersError(const QString &open, const QString &close, bool anySyntax)
{
    for (const QString &delimiter : {open, close}) {
        if (delimiter.isEmpty() || delimiter.size() > 3)
            return QCoreApplication::translate("MetaNotes", "A delimiter is 1 to 3 characters.");
        for (const QChar c : delimiter) {
            if (c.isSpace() || c == QLatin1Char('\\'))
                return QCoreApplication::translate("MetaNotes", "A delimiter cannot contain a space or a backslash.");
            if (!anySyntax && c.unicode() < 128 && metaNoteReserved.contains(c))
                return QCoreApplication::translate("MetaNotes", "\"%1\" is Markdown syntax (emphasis, strikethrough, highlight, "
                                   "code, maths, comments, tags or table cells).").arg(c);
        }
    }
    return {};
}

Metadata Parser::parse(const QString &text, const QString &path, const ParseOptions &options) const
{
    return parseAll(text, path, options).metadata;
}

Parser::Result Parser::parseAll(const QString &text, const QString &path,
                                const ParseOptions &options) const
{
    Result result;
    Metadata &metadata = result.metadata;
    metadata.path = path;
    metadata.size = text.size();

    const SourceMap map(text);
    const Range frontmatter = Frontmatter::find(text);
    Frontmatter::parse(text, frontmatter, metadata);

    // md4qt knows nothing about frontmatter; feeding it the block would make
    // the `---` fences a thematic break and a setext heading. Blank the block
    // out with spaces rather than dropping it, so every line number and column
    // md4qt reports still indexes the real file.
    QString body = text;
    if (frontmatter.isValid()) {
        for (qsizetype i = frontmatter.start; i < frontmatter.end; ++i) {
            if (body.at(i) != QLatin1Char('\n'))
                body[i] = QLatin1Char(' ');
        }
    }

    QTextStream stream(&body, QIODevice::ReadOnly);
    const auto document = d->parser.parse(stream, QFileInfo(path).absolutePath(),
                                          QFileInfo(path).fileName());

    Walk walk;
    walk.map = &map;
    walk.text = &text;
    walk.anyTaskState = options.taskStates;
    if (document) {
        for (const auto &item : document->items())
            walkItem(item.data(), walk);
        // Footnote bodies live apart from the items, so their emphasis and
        // links were never seen. Below block level: they are not blocks of
        // their own in the note's flow.
        ++walk.depth;
        walk.inFootnote = true;
        for (const auto &footnote : document->footnotesMap())
            walkItem(footnote.data(), walk);
        walk.inFootnote = false;
        --walk.depth;
    }

    if (frontmatter.isValid())
        walk.masked.append(frontmatter);

    // Reference-style links and embeds found their label during the walk but
    // not their target; resolve them now that every `[ref]: url` definition
    // in the file - masked ranges included, so one inside a code fence does
    // not count - is known.
    if (!walk.pendingReferences.isEmpty()) {
        const auto definitions = collectLinkDefinitions(text, walk.masked);
        for (const auto &pending : std::as_const(walk.pendingReferences)) {
            Link &out = walk.links[pending.linkIndex];
            const QString resolved = definitions.value(normaliseLabel(pending.label));
            // No definition: stays unresolved rather than inventing a target
            // out of the label.
            QString target = resolved;
            const qsizetype hash = target.indexOf(QLatin1Char('#'));
            if (hash > 0) {
                out.subpath = target.mid(hash);
                target = target.left(hash);
            }
            out.raw = target;
            out.target = QUrl::fromPercentEncoding(target.toUtf8());
            out.external = ObsidianSyntax::isExternalTarget(out.target);
            if (out.embed)
                walk.decorations.embeds.append(embedOf(out));
            walk.decorations.styles.append({StyleSpan::Kind::Link, out.range, 0, out.target, {}, {}});
        }
    }

    const auto obsidian = ObsidianSyntax::scan(text, withColonFenceLines(text, walk.masked));

    metadata.headings = walk.headings;
    metadata.links = walk.links + obsidian.links;
    metadata.tags += obsidian.tags;
    metadata.blockIds = obsidian.blockIds;
    metadata.tasks = walk.tasks;

    // Before the move, or the spans would be written into a Decorations that
    // is about to be overwritten.
    detectDisplayMath(text, walk.decorations, walk.masked);
    {
        // walk.masked already holds each [text](url) whole, so a link's text
        // and destination keep their references as written.
        QList<Range> notText = withColonFenceLines(text, walk.masked);
        for (const Link &link : obsidian.links)
            notText.append(link.range);
        // Every formula, the ones detectDisplayMath found after md4qt gave up
        // on them (a blank line inside) included: its text is TeX.
        for (const MathSpan &math : std::as_const(walk.decorations.maths))
            notText.append(math.range);
        scanEntities(text, notText, walk.decorations);
        notText += obsidian.comments;
        if (frontmatter.isValid())
            notText.append(frontmatter);
        // CommonMark: escapes do not work inside an autolink.
        static const QRegularExpression autolink(QStringLiteral("<[A-Za-z][A-Za-z0-9+.-]{1,31}:[^<>\\s]*>"));
        for (auto it = autolink.globalMatchView(text); it.hasNext();) {
            const QRegularExpressionMatch match = it.next();
            notText.append({match.capturedStart(), match.capturedEnd()});
        }
        scanEscapes(text, notText, walk.decorations);
    }
    splitParagraphsAtDisplayMath(text, walk.decorations);
    result.decorations = std::move(walk.decorations);
    if (frontmatter.isValid()) {
        result.decorations.blocks.prepend({BlockSpan::Kind::Frontmatter, frontmatter, 0});
        // Properties render as a table, so the whole YAML block is syntax.
        result.decorations.syntax.append(frontmatter);
    }

    // Obsidian's own syntax, which md4qt never saw.
    for (const Link &link : obsidian.links) {
        if (!link.wiki)
            continue;
        if (link.embed)
            result.decorations.embeds.append(embedOf(link));
        StyleSpan span{StyleSpan::Kind::Link, link.range, 0, link.target, {}, {}};
        // A heading or block reference's anchor sits right after the target:
        // find the "#" there rather than re-deriving it from `link.subpath`,
        // which was already trimmed and so cannot be trusted for offsets.
        // This is groundwork only - see the doc comment on anchorHash.
        if (!link.embed && !link.subpath.isEmpty() && link.targetRange.isValid()) {
            const qsizetype hashPos = text.indexOf(QLatin1Char('#'), link.targetRange.end);
            if (hashPos >= 0 && hashPos < link.range.end) {
                span.anchorHash = {hashPos, hashPos + 1};
                span.anchorText = {hashPos + 1, hashPos + link.subpath.size()};
            }
        }
        result.decorations.styles.append(span);
        const qsizetype openEnd = link.targetRange.isValid() ? link.targetRange.start
                                                             : link.range.start + 2;
        // Hide "[[" plus the target and the pipe when an alias is shown, else
        // just the brackets.
        if (!link.alias.isEmpty()) {
            result.decorations.syntax.append(
                {link.range.start, link.range.end - link.alias.size() - 2});
        } else {
            result.decorations.syntax.append({link.range.start, openEnd});
        }
        result.decorations.syntax.append({link.range.end - 2, link.range.end});
    }
    for (const Tag &tag : obsidian.tags) {
        if (tag.range == frontmatter)
            continue; // a frontmatter tag has no inline marker
        result.decorations.styles.append({StyleSpan::Kind::Tag, tag.range, 0, tag.name, {}, {}});
    }
    for (const Range &range : obsidian.highlights) {
        result.decorations.styles.append({StyleSpan::Kind::Highlight, range, 0, {}, {}, {}});
        result.decorations.syntax.append({range.start, range.start + 2});
        result.decorations.syntax.append({range.end - 2, range.end});
    }
    for (const Range &range : obsidian.comments)
        result.decorations.syntax.append(range);
    nestQuotedLists(text, map, result.decorations.listItems);
    scanFootnotes(text, walk.masked + obsidian.comments, result.decorations);
    // Bare brackets, meta notes and the word count's exclusion are all runs
    // between delimiters, found over what has no syntax of its own.
    QList<Range> excluded;
    const bool metaOn = options.metaNotes && metaNoteDelimitersError(options.open, options.close).isEmpty();
    const bool excludeOn = options.excludeBetween
                           && metaNoteDelimitersError(options.excludeOpen, options.excludeClose, true).isEmpty();
    if (options.brackets || metaOn || excludeOn) {
        QList<Range> opaque = withColonFenceLines(text, walk.masked) + obsidian.comments;
        for (const Link &link : obsidian.links)
            opaque.append(link.range);
        for (const FootnoteSpan &span : result.decorations.footnotes)
            opaque.append(span.range);
        for (const FootnoteDefinition &definition : result.decorations.footnoteDefinitions)
            opaque.append({definition.range.start, definition.content.start});
        for (const CalloutSpan &callout : result.decorations.callouts)
            opaque.append(callout.marker);
        std::sort(opaque.begin(), opaque.end(), [](const Range &a, const Range &b) { return a.start < b.start; });
        QHash<QString, QList<Range>> cache; // the same pair is scanned once
        const auto runsFor = [&](const QString &open, const QString &close) {
            const QString key = open + QLatin1Char('\n') + close;
            if (!cache.contains(key))
                cache.insert(key, scanRuns(text, opaque, open, close, result.decorations.tables));
            return cache.value(key);
        };
        const bool bracketsAreNotes = metaOn && options.open == QLatin1String("[")
                                      && options.close == QLatin1String("]");
        if (options.brackets && !bracketsAreNotes) { // a meta note's colour wins
            for (const Range &run : runsFor(QStringLiteral("["), QStringLiteral("]")))
                result.decorations.styles.append({StyleSpan::Kind::BareBracket, run, 0, {}, {}, {}});
        }
        if (metaOn) {
            result.decorations.metaNotes = runsFor(options.open, options.close);
            metadata.metaNotes = result.decorations.metaNotes;
            for (const Range &run : std::as_const(result.decorations.metaNotes))
                result.decorations.styles.append({StyleSpan::Kind::MetaNote, run, 0, {}, {}, {}});
        }
        if (excludeOn)
            excluded = runsFor(options.excludeOpen, options.excludeClose);
    }

    // Two things md4qt leaves on the page: the `:::` fences it read as prose,
    // and the definition half of a reference link, which it reads and forgets.
    scanColonFences(text, walk.masked, result.decorations);
    hideLinkDefinitions(text, walk.masked, result.decorations);

    // Every fence in the file is known only now, which is what a tab group
    // needs to see across.
    groupCodeFenceTabs(text, result.decorations.codeFences);

    const qsizetype counted = frontmatter.isValid() ? frontmatter.end : 0;
    result.decorations.uncounted = uncountedRanges(text, excluded, options.ignoreFormatting,
                                                   result.decorations, counted);
    if (result.decorations.uncounted.isEmpty()) {
        metadata.wordCount = countWords(QStringView(text).mid(counted));
    } else {
        QString shown = text;
        for (const Range &range : std::as_const(result.decorations.uncounted))
            std::fill_n(shown.data() + range.start, range.length(), QLatin1Char(' '));
        metadata.wordCount = countWords(QStringView(shown).mid(counted));
    }

    return result;
}

QStringList Metadata::linkNames() const
{
    QStringList names;
    const QString base = QFileInfo(path).completeBaseName();
    if (!base.isEmpty())
        names << base;
    names += aliases;
    if (!id.isEmpty())
        names << id; // Ideas.md 16.5: the id is an implicit alias
    names.removeDuplicates();
    return names;
}

Range headingSection(const QString &text, const QString &wanted)
{
    static const QRegularExpression atx(QStringLiteral("^ {0,3}(#{1,6})[ \\t]+(.*?)(?:[ \\t]+#+)?\\s*$"));
    const Range front = Frontmatter::find(text);
    const QString want = wanted.simplified();
    QChar fence;
    qsizetype fenceLength = 0;
    bool inComment = false;
    qsizetype start = -1;
    int startLevel = 0;
    for (qsizetype pos = 0; pos <= text.size();) {
        qsizetype eol = text.indexOf(QLatin1Char('\n'), pos);
        if (eol < 0)
            eol = text.size();
        qsizetype next = eol + 1;
        const QStringView line = QStringView(text).mid(pos, eol - pos);
        qsizetype indent = 0;
        while (indent < line.size() && indent < 4 && line.at(indent) == QLatin1Char(' '))
            ++indent;
        const QStringView body = line.sliced(indent);
        int level = 0;
        QString title;
        if (front.isValid() && pos < front.end) {
            // YAML, whose `# comment` lines are not headings
        } else if (!fence.isNull()) {
            qsizetype run = 0;
            while (run < body.size() && body.at(run) == fence)
                ++run;
            if (indent < 4 && run >= fenceLength && body.sliced(run).trimmed().isEmpty())
                fence = QChar();
        } else if (inComment) {
            inComment = !line.contains(QLatin1String("-->"));
        } else if (indent < 4 && (body.startsWith(QLatin1String("```")) || body.startsWith(QLatin1String("~~~")))) {
            fence = body.at(0);
            fenceLength = 0;
            while (fenceLength < body.size() && body.at(fenceLength) == fence)
                ++fenceLength;
        } else if (indent < 4 && body.startsWith(QLatin1String("<!--"))) {
            inComment = !line.contains(QLatin1String("-->"));
        } else if (const auto match = atx.matchView(line); match.hasMatch()) {
            level = int(match.capturedLength(1));
            title = match.captured(2);
        } else if (indent < 4 && body.size() > 3 && body.at(0) == QLatin1Char('<')
                   && (body.at(1) == QLatin1Char('h') || body.at(1) == QLatin1Char('H'))) {
            const auto html = htmlHeadingRe().match(text, pos + indent, QRegularExpression::NormalMatch,
                                                      QRegularExpression::AnchorAtOffsetMatchOption);
            if (html.hasMatch()) {
                const Heading heading = htmlHeadingOf(html, 0);
                level = heading.level;
                title = heading.text;
                // the element's own lines are not looked at again
                const qsizetype after = text.indexOf(QLatin1Char('\n'), html.capturedEnd());
                next = after < 0 ? text.size() + 1 : after + 1;
            }
        }
        if (level) {
            if (start >= 0 && level <= startLevel)
                return {start, pos};
            if (start < 0 && title.simplified().compare(want, Qt::CaseInsensitive) == 0) {
                start = pos;
                startLevel = level;
            }
        }
        pos = next;
    }
    return start >= 0 ? Range{start, text.size()} : Range{};
}

int wordsIn(QStringView text)
{
    return countWords(text);
}

} // namespace noteAppC
