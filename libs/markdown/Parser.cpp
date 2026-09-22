#include "Parser.h"

#include <QFileInfo>
#include <QRegularExpression>
#include <QTextStream>

#include <algorithm>

#include <md4qt/doc.h>
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
            walk.decorations.styles.append({*kind, rangeOf(*item, map), 0, {}});
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
void describeListItem(const QString &text, const Range &range, const SourceMap &map,
                      Decorations &decorations)
{
    static const QRegularExpression head(QStringLiteral(
        "^([ \\t]*)(?:[-*+]|(\\d+)[.)])[ \\t]+(\\[([ xX])\\][ \\t]+)?"));

    // md4qt puts a list item's start at its bullet, not at the start of the
    // line, so the indent has to be read from the line itself.
    const qsizetype line = map.lineAt(range.start);
    const qsizetype lineStart = map.lineStart(line);
    const QString firstLine = text.mid(lineStart, map.lineEnd(line) - lineStart);
    const auto match = head.match(firstLine);
    if (!match.hasMatch())
        return;

    ListItemSpan span;
    span.range = {lineStart, range.end};
    span.marker = {lineStart, lineStart + match.capturedEnd(0)};
    // A tab counts as one level, as do two or four spaces.
    span.indent = int(match.captured(1).count(QLatin1Char('\t'))
                      + match.captured(1).count(QLatin1Char(' ')) / 2);
    span.ordered = !match.captured(2).isEmpty();
    span.number = match.captured(2).toInt();
    span.task = !match.captured(3).isEmpty();
    span.done = match.captured(4).compare(QLatin1String("x"), Qt::CaseInsensitive) == 0;
    if (span.task)
        span.checkbox = {lineStart + match.capturedStart(3), lineStart + match.capturedStart(3) + 3};

    decorations.syntax.append(span.marker);
    decorations.listItems.append(span);
}

bool isTrue(const QString &value)
{
    return value.compare(QLatin1String("true"), Qt::CaseInsensitive) == 0
           || value.compare(QLatin1String("yes"), Qt::CaseInsensitive) == 0
           || value == QLatin1String("1");
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
    } else {
        // Quarto's eval, echo, output, warning and error arrive here and stop.
        // noteAppC never runs code; keeping them only hides the lines.
        options.extra.insert(key, unquote(value));
    }
}

/// The `#|` lines at the head of a fenced block. `content` is the code between
/// the fences, which is where md4qt puts a Code item's own range.
CodeOptions parseCodeOptions(const QString &text, const Range &content, const QString &language)
{
    CodeOptions options;
    options.block = content;
    QString rawHighlight;

    const QRegularExpression option(
        QStringLiteral("^[ \\t]*%1\\|[ \\t]*([\\w-]+)[ \\t]*:[ \\t]*(.*)$")
            .arg(QRegularExpression::escape(commentPrefix(language))));

    qsizetype cursor = content.start;
    while (cursor < content.end) {
        qsizetype lineEnd = text.indexOf(QLatin1Char('\n'), cursor);
        if (lineEnd < 0 || lineEnd > content.end)
            lineEnd = content.end;
        const auto match = option.match(text.mid(cursor, lineEnd - cursor));
        if (!match.hasMatch())
            break;
        applyCodeOption(options, match.captured(1).toLower(), match.captured(2).trimmed(),
                        rawHighlight);
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

/// What a `:::` fence line says about its container: Pandoc's
/// `{#id .class key="value"}` or the bare `name key="value"` form.
struct FenceSpec {
    QString type;
    QMap<QString, QString> attributes;
};

FenceSpec parseFenceSpec(QString spec)
{
    static const QRegularExpression token(
        QStringLiteral(R"(([.#]?[\w-]+)(?:[ \t]*=[ \t]*("[^"]*"|'[^']*'|\S+))?)"));

    FenceSpec out;
    spec = spec.trimmed();
    if (spec.startsWith(QLatin1Char('{')) && spec.endsWith(QLatin1Char('}')))
        spec = spec.mid(1, spec.size() - 2);

    auto it = token.globalMatch(spec);
    while (it.hasNext()) {
        const auto match = it.next();
        const QString name = match.captured(1);
        if (name.startsWith(QLatin1Char('#')))
            continue; // an id names nothing that renders
        if (match.captured(2).isEmpty()) {
            // `.note` in braces and `note` in the bare form both name the class.
            if (out.type.isEmpty())
                out.type = name.startsWith(QLatin1Char('.')) ? name.mid(1) : name;
            continue;
        }
        out.attributes.insert(name.toLower(), unquote(match.captured(2)));
    }
    return out;
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

/// `:::` containers (PLAN.md 16.1). md4qt reads the fence lines as ordinary
/// paragraph text, so they are found here in the source, where the offsets are
/// already true. Nesting goes by fence length, as in MyST: `::::` wraps `:::`.
void scanColonFences(const QString &text, const QList<Range> &masked, Decorations &decorations)
{
    static const QRegularExpression fence(
        QStringLiteral("^(:{3,})[ \\t]*(\\{[^}]*\\}|[\\w-][^\\n]*)?$"),
        QRegularExpression::MultilineOption);

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
        QStringLiteral(R"(^ {0,3}\[[^\]\n]+\]:[ \t]*\S[^\n]*$)"),
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
        QStringLiteral(R"(^ {0,3}\[([^\]\n]+)\]:[ \t]*(\S[^\n]*)$)"),
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
                {StyleSpan::Kind::Heading, out.textRange, out.level, out.text});
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
            MathSpan span;
            span.range = range;
            span.display = !math->isInline();
            const Range open = rangeOf(math->startDelim(), map);
            const Range close = rangeOf(math->endDelim(), map);
            span.formula = {open.isValid() ? open.end : range.start,
                            close.isValid() ? close.start : range.end};
            walk.decorations.maths.append(span);
        }
        if (item->type() == MD::ItemType::Code || item->type() == MD::ItemType::Math) {
            auto *code = static_cast<MD::Code *>(item);
            const bool block = !code->isInline();
            walk.decorations.styles.append(
                {block ? StyleSpan::Kind::CodeBlock : kind, range, 0, code->syntax()});
            // The fences are syntax; their content is not.
            walk.decorations.syntax.append(rangeOf(code->startDelim(), map));
            walk.decorations.syntax.append(rangeOf(code->endDelim(), map));
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
        } else {
            walk.decorations.styles.append({kind, range, 0, {}});
            if (walk.depth == 0)
                walk.decorations.blocks.append({BlockSpan::Kind::Html, range, 0});
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
            walk.decorations.styles.append({StyleSpan::Kind::Link, out.range, 0, out.target});
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
        if (item->type() == MD::ItemType::Paragraph)
            walk.decorations.paragraphs.append(rangeOf(*item, map));
        if (walk.depth == 0) {
            const auto kind = item->type() == MD::ItemType::List ? BlockSpan::Kind::List
                                                                 : BlockSpan::Kind::Paragraph;
            walk.decorations.blocks.append({kind, rangeOf(*item, map), 0});
        }
        ++walk.depth;
        walkBlock(static_cast<MD::Block *>(item), walk);
        --walk.depth;
        break;
    }
    case MD::ItemType::ListItem: {
        auto *listItem = static_cast<MD::ListItem *>(item);
        if (walk.text)
            describeListItem(*walk.text, rangeOf(*listItem, map), map, walk.decorations);
        if (listItem->isTaskList()) {
            Task task;
            task.done = listItem->isChecked();
            task.range = rangeOf(*listItem, map);
            if (!listItem->items().isEmpty()
                && listItem->items().first()->type() == MD::ItemType::Paragraph) {
                task.text = plainText(static_cast<MD::Paragraph *>(
                    listItem->items().first().data()));
            }
            walk.tasks.append(task);
        }
        walkBlock(listItem, walk);
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
        for (const auto &row : table->rows()) {
            QList<Range> cells;
            for (const auto &cell : row->cells())
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
        walkBlock(static_cast<MD::Block *>(item), walk);
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

} // namespace

struct Parser::Private {
    mutable MD::Parser parser;
};

Parser::Parser()
    : d(std::make_unique<Private>())
{
}

Parser::~Parser() = default;

Metadata Parser::parse(const QString &text, const QString &path) const
{
    return parseAll(text, path).metadata;
}

Parser::Result Parser::parseAll(const QString &text, const QString &path) const
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
    if (document) {
        for (const auto &item : document->items())
            walkItem(item.data(), walk);
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
            walk.decorations.styles.append({StyleSpan::Kind::Link, out.range, 0, out.target});
        }
    }

    const auto obsidian = ObsidianSyntax::scan(text, walk.masked);

    metadata.headings = walk.headings;
    metadata.links = walk.links + obsidian.links;
    metadata.tags += obsidian.tags;
    metadata.blockIds = obsidian.blockIds;
    metadata.tasks = walk.tasks;
    metadata.wordCount = countWords(
        QStringView(text).mid(frontmatter.isValid() ? frontmatter.end : 0));

    // Before the move, or the spans would be written into a Decorations that
    // is about to be overwritten.
    detectDisplayMath(text, walk.decorations, walk.masked);
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
        result.decorations.styles.append({StyleSpan::Kind::Link, link.range, 0, link.target});
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
        result.decorations.styles.append({StyleSpan::Kind::Tag, tag.range, 0, tag.name});
    }
    for (const Range &range : obsidian.highlights) {
        result.decorations.styles.append({StyleSpan::Kind::Highlight, range, 0, {}});
        result.decorations.syntax.append({range.start, range.start + 2});
        result.decorations.syntax.append({range.end - 2, range.end});
    }
    for (const Range &range : obsidian.comments)
        result.decorations.syntax.append(range);

    // Two things md4qt leaves on the page: the `:::` fences it read as prose,
    // and the definition half of a reference link, which it reads and forgets.
    scanColonFences(text, walk.masked, result.decorations);
    hideLinkDefinitions(text, walk.masked, result.decorations);

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

} // namespace noteAppC
