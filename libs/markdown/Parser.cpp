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
        break;
    }
    case MD::ItemType::Code:
    case MD::ItemType::Math:
    case MD::ItemType::RawHtml:
        walk.masked.append(rangeOf(*item, map));
        break;
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
        // A link's own label can hold emphasis but never a wikilink or a tag.
        walk.masked.append(out.range);
        break;
    }
    case MD::ItemType::List:
    case MD::ItemType::Blockquote:
    case MD::ItemType::Paragraph:
    case MD::ItemType::Footnote:
    case MD::ItemType::TableCell:
        walkBlock(static_cast<MD::Block *>(item), walk);
        break;
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
        for (const auto &row : table->rows())
            walkItem(row.data(), walk);
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
    Metadata metadata;
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

    return metadata;
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
