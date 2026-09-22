#include "Parser.h"

#include <QFileInfo>
#include <QRegularExpression>
#include <QTextStream>

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
        } else {
            walk.decorations.styles.append({kind, range, 0, {}});
            if (walk.depth == 0)
                walk.decorations.blocks.append({BlockSpan::Kind::Html, range, 0});
        }
        break;
    }
    case MD::ItemType::Link:
    case MD::ItemType::Image: {
        auto *link = static_cast<MD::Link *>(item);
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
        const qsizetype hash = target.indexOf(QLatin1Char('#'));
        if (hash > 0) {
            out.subpath = target.mid(hash);
            target = target.left(hash);
        }
        out.raw = target;
        out.target = QUrl::fromPercentEncoding(target.toUtf8());
        out.external = ObsidianSyntax::isExternalTarget(out.target);
        walk.links.append(out);
        walk.decorations.styles.append({StyleSpan::Kind::Link, out.range, 0, out.target});
        // A link's own label can hold emphasis but never a wikilink or a tag.
        walk.masked.append(out.range);
        break;
    }
    case MD::ItemType::List:
    case MD::ItemType::Blockquote:
    case MD::ItemType::Paragraph:
    case MD::ItemType::Footnote:
    case MD::ItemType::TableCell: {
        if (walk.depth == 0) {
            const auto kind = item->type() == MD::ItemType::List ? BlockSpan::Kind::List
                              : item->type() == MD::ItemType::Blockquote
                                  ? BlockSpan::Kind::Quote
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
        if (walk.depth == 0)
            walk.decorations.blocks.append({BlockSpan::Kind::Table, rangeOf(*table, map), 0});
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

    const auto obsidian = ObsidianSyntax::scan(text, walk.masked);

    metadata.headings = walk.headings;
    metadata.links = walk.links + obsidian.links;
    metadata.tags += obsidian.tags;
    metadata.blockIds = obsidian.blockIds;
    metadata.tasks = walk.tasks;
    metadata.wordCount = countWords(
        QStringView(text).mid(frontmatter.isValid() ? frontmatter.end : 0));

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
