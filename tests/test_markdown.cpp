#include <gtest/gtest.h>

#include <algorithm>

#include "Decorations.h"
#include "Frontmatter.h"
#include "ObsidianSyntax.h"
#include "Parser.h"
#include "SourceMap.h"

using namespace noteAppC;

namespace {

Metadata parse(const QString &text)
{
    static Parser parser;
    return parser.parse(text, QStringLiteral("/vault/Note.md"));
}

QString sliceOf(const QString &text, const Range &range)
{
    return text.mid(range.start, range.length());
}

QStringList targets(const Metadata &metadata)
{
    QStringList out;
    for (const Link &link : metadata.links)
        out << link.target;
    return out;
}

QStringList tagNames(const Metadata &metadata)
{
    QStringList out;
    for (const Tag &tag : metadata.tags)
        out << tag.name;
    return out;
}

} // namespace

TEST(SourceMapTest, OffsetsAndLines)
{
    const QString text = QStringLiteral("one\ntwo\n\nfour");
    const SourceMap map(text);

    EXPECT_EQ(map.lineCount(), 4);
    EXPECT_EQ(map.lineStart(0), 0);
    EXPECT_EQ(map.lineStart(1), 4);
    EXPECT_EQ(map.lineEnd(0), 3);
    EXPECT_EQ(map.lineAt(0), 0);
    EXPECT_EQ(map.lineAt(5), 1);
    EXPECT_EQ(map.lineAt(9), 3);
    // md4qt end columns are inclusive.
    EXPECT_EQ(sliceOf(text, map.range(0, 0, 0, 2)), QStringLiteral("one"));
    EXPECT_EQ(sliceOf(text, map.range(0, 0, 1, 2)), QStringLiteral("one\ntwo"));
}

TEST(FrontmatterTest, FoundOnlyAtTheTop)
{
    EXPECT_TRUE(Frontmatter::find(QStringLiteral("---\na: 1\n---\nbody\n")).isValid());
    EXPECT_FALSE(Frontmatter::find(QStringLiteral("body\n---\na: 1\n---\n")).isValid());
    EXPECT_FALSE(Frontmatter::find(QStringLiteral("--- not a fence\na: 1\n")).isValid());
    EXPECT_FALSE(Frontmatter::find(QStringLiteral("---\nunclosed: 1\n")).isValid());
}

TEST(FrontmatterTest, PropertiesAliasesTagsAndId)
{
    const QString text = QStringLiteral(
        "---\n"
        "title: A note\n"
        "aliases:\n"
        "  - Second name\n"
        "  - Third name\n"
        "tags: [project, \"#status/active\"]\n"
        "cssclasses: [wide]\n"
        "id: 01J9ZQ\n"
        "count: 42\n"
        "done: true\n"
        "due: 2026-09-22\n"
        "---\n"
        "\n"
        "Body.\n");
    const Metadata metadata = parse(text);

    EXPECT_EQ(metadata.aliases, QStringList({QStringLiteral("Second name"),
                                             QStringLiteral("Third name")}));
    EXPECT_EQ(metadata.cssClasses, QStringList({QStringLiteral("wide")}));
    EXPECT_EQ(metadata.id, QStringLiteral("01J9ZQ"));
    // The "#" of a frontmatter tag is optional and not part of the name.
    EXPECT_EQ(tagNames(metadata), QStringList({QStringLiteral("project"),
                                               QStringLiteral("status/active")}));
    EXPECT_EQ(metadata.properties.size(), 8);

    const auto typeOf = [&metadata](const QString &key) {
        for (const Property &property : metadata.properties) {
            if (property.key == key)
                return property.type;
        }
        return Property::Type::Unknown;
    };
    EXPECT_EQ(typeOf(QStringLiteral("title")), Property::Type::Text);
    EXPECT_EQ(typeOf(QStringLiteral("aliases")), Property::Type::List);
    EXPECT_EQ(typeOf(QStringLiteral("count")), Property::Type::Number);
    EXPECT_EQ(typeOf(QStringLiteral("done")), Property::Type::Checkbox);
    EXPECT_EQ(typeOf(QStringLiteral("due")), Property::Type::Date);

    // The id is an implicit alias, so a link to it resolves after a rename.
    EXPECT_TRUE(metadata.linkNames().contains(QStringLiteral("01J9ZQ")));
    EXPECT_TRUE(metadata.linkNames().contains(QStringLiteral("Note")));
}

TEST(FrontmatterTest, BrokenYamlStillIndexes)
{
    const Metadata metadata = parse(QStringLiteral("---\n: : :\n\tbad\n---\n\n# Heading\n"));
    EXPECT_FALSE(metadata.parseWarnings.isEmpty());
    ASSERT_EQ(metadata.headings.size(), 1);
    EXPECT_EQ(metadata.headings.first().text, QStringLiteral("Heading"));
}

TEST(HeadingTest, LevelsTextAndOffsets)
{
    const QString text = QStringLiteral("# One\n\n## Two *emphasised*\n\ntext\n");
    const Metadata metadata = parse(text);

    ASSERT_EQ(metadata.headings.size(), 2);
    EXPECT_EQ(metadata.headings.at(0).level, 1);
    EXPECT_EQ(metadata.headings.at(0).text, QStringLiteral("One"));
    EXPECT_EQ(sliceOf(text, metadata.headings.at(0).range), QStringLiteral("# One"));
    EXPECT_EQ(metadata.headings.at(1).level, 2);
    EXPECT_EQ(metadata.headings.at(1).text, QStringLiteral("Two emphasised"));
}

TEST(WikilinkTest, Parts)
{
    const Link plain = ObsidianSyntax::parseWikilink(QStringLiteral("Some note"), false);
    EXPECT_EQ(plain.target, QStringLiteral("Some note"));
    EXPECT_TRUE(plain.subpath.isEmpty());
    EXPECT_TRUE(plain.alias.isEmpty());

    const Link full = ObsidianSyntax::parseWikilink(
        QStringLiteral("folder/Some note#Heading|display"), true);
    EXPECT_EQ(full.target, QStringLiteral("folder/Some note"));
    EXPECT_EQ(full.subpath, QStringLiteral("#Heading"));
    EXPECT_EQ(full.alias, QStringLiteral("display"));
    EXPECT_TRUE(full.embed);

    const Link block = ObsidianSyntax::parseWikilink(QStringLiteral("Note#^abc123"), false);
    EXPECT_EQ(block.subpath, QStringLiteral("#^abc123"));

    const Link here = ObsidianSyntax::parseWikilink(QStringLiteral("#Heading"), false);
    EXPECT_TRUE(here.target.isEmpty());
    EXPECT_EQ(here.subpath, QStringLiteral("#Heading"));
}

TEST(LinkTest, WikilinksEmbedsAndMarkdownLinks)
{
    const QString text = QStringLiteral(
        "A [[Plain]] and an ![[Embed.png]] and [[A note#Section|alias]].\n"
        "\n"
        "A [markdown link](Other%20note.md) and <https://example.com>.\n");
    const Metadata metadata = parse(text);

    EXPECT_TRUE(targets(metadata).contains(QStringLiteral("Plain")));
    EXPECT_TRUE(targets(metadata).contains(QStringLiteral("Embed.png")));
    EXPECT_TRUE(targets(metadata).contains(QStringLiteral("A note")));
    EXPECT_TRUE(targets(metadata).contains(QStringLiteral("Other note.md")));

    for (const Link &link : metadata.links) {
        if (link.target == QStringLiteral("Embed.png"))
            EXPECT_TRUE(link.embed);
        if (link.target == QStringLiteral("A note")) {
            EXPECT_EQ(link.subpath, QStringLiteral("#Section"));
            EXPECT_EQ(link.alias, QStringLiteral("alias"));
            EXPECT_EQ(sliceOf(text, link.targetRange), QStringLiteral("A note"));
        }
        if (link.target.startsWith(QStringLiteral("https://")))
            EXPECT_TRUE(link.external);
    }
}

TEST(LinkTest, RangesPointAtTheSource)
{
    const QString text = QStringLiteral("see [[Target|shown]] here\n");
    const Metadata metadata = parse(text);
    ASSERT_EQ(metadata.links.size(), 1);
    EXPECT_EQ(sliceOf(text, metadata.links.first().range), QStringLiteral("[[Target|shown]]"));
    EXPECT_EQ(sliceOf(text, metadata.links.first().targetRange), QStringLiteral("Target"));
}

TEST(TagTest, InlineNestedAndNonTags)
{
    const QString text = QStringLiteral(
        "#project and #status/active and (#in-brackets)\n"
        "\n"
        "# A heading is not a tag, nor is C#, nor #1, nor a#b.\n");
    const Metadata metadata = parse(text);

    EXPECT_EQ(tagNames(metadata), QStringList({QStringLiteral("project"),
                                               QStringLiteral("status/active"),
                                               QStringLiteral("in-brackets")}));
}

TEST(MaskingTest, CodeAndCommentsHideEverything)
{
    const QString text = QStringLiteral(
        "Real [[Link]] and #realtag.\n"
        "\n"
        "`[[InlineCode]] #notatag`\n"
        "\n"
        "```\n"
        "[[Fenced]] #alsonot\n"
        "```\n"
        "\n"
        "%%[[Commented]] #hidden%%\n");
    const Metadata metadata = parse(text);

    EXPECT_EQ(targets(metadata), QStringList({QStringLiteral("Link")}));
    EXPECT_EQ(tagNames(metadata), QStringList({QStringLiteral("realtag")}));
}

TEST(MaskingTest, FrontmatterIsNotBody)
{
    const QString text = QStringLiteral("---\nnote: \"[[Not a link]] #nottag\"\n---\n\n[[Real]]\n");
    const Metadata metadata = parse(text);
    EXPECT_EQ(targets(metadata), QStringList({QStringLiteral("Real")}));
    EXPECT_TRUE(tagNames(metadata).isEmpty());
}

TEST(BlockIdTest, OnlyAtEndOfLine)
{
    const QString text = QStringLiteral("A paragraph. ^my-block\n\nNot ^here in the middle.\n");
    const Metadata metadata = parse(text);
    ASSERT_EQ(metadata.blockIds.size(), 1);
    EXPECT_EQ(metadata.blockIds.first().id, QStringLiteral("my-block"));
    EXPECT_EQ(sliceOf(text, metadata.blockIds.first().range), QStringLiteral("^my-block"));
}

TEST(TaskTest, DoneAndTodo)
{
    const QString text = QStringLiteral("- [ ] open task\n- [x] closed task\n- plain item\n");
    const Metadata metadata = parse(text);

    ASSERT_EQ(metadata.tasks.size(), 2);
    EXPECT_FALSE(metadata.tasks.at(0).done);
    EXPECT_EQ(metadata.tasks.at(0).text, QStringLiteral("open task"));
    EXPECT_TRUE(metadata.tasks.at(1).done);
}

// --- decorations: what live preview hides and styles ---------------------

namespace {

/// The display string live preview shows for the whole note, with no block
/// active - the quickest way to assert what gets hidden.
QString rendered(const QString &text)
{
    static Parser parser;
    const auto result = parser.parseAll(text, QStringLiteral("/vault/Note.md"));

    QString out;
    for (qsizetype i = 0; i < text.size(); ++i) {
        bool hidden = false;
        for (const Range &range : result.decorations.syntax)
            hidden = hidden || range.contains(i);
        if (!hidden)
            out += text.at(i);
    }
    return out;
}

bool hasStyle(const Decorations &decorations, StyleSpan::Kind kind, const QString &payload = {})
{
    for (const StyleSpan &span : decorations.styles) {
        if (span.kind == kind && (payload.isEmpty() || span.payload == payload))
            return true;
    }
    return false;
}

Decorations decorationsOf(const QString &text)
{
    static Parser parser;
    return parser.parseAll(text, QStringLiteral("/vault/Note.md")).decorations;
}

} // namespace

TEST(DecorationTest, HeadingMarkersAreHidden)
{
    EXPECT_EQ(rendered(QStringLiteral("## Heading\n")), QStringLiteral("Heading\n"));
    EXPECT_TRUE(hasStyle(decorationsOf(QStringLiteral("## Heading\n")),
                         StyleSpan::Kind::Heading));
}

TEST(DecorationTest, EmphasisDelimitersAreHidden)
{
    EXPECT_EQ(rendered(QStringLiteral("a **bold** and *thin* and ~~gone~~\n")),
              QStringLiteral("a bold and thin and gone\n"));

    const Decorations decorations =
        decorationsOf(QStringLiteral("a **bold** and *thin* and ~~gone~~\n"));
    EXPECT_TRUE(hasStyle(decorations, StyleSpan::Kind::Bold));
    EXPECT_TRUE(hasStyle(decorations, StyleSpan::Kind::Italic));
    EXPECT_TRUE(hasStyle(decorations, StyleSpan::Kind::Strikethrough));
}

TEST(DecorationTest, WikilinksShowTheirDisplayText)
{
    EXPECT_EQ(rendered(QStringLiteral("see [[Target]] here\n")),
              QStringLiteral("see Target here\n"));
    // With an alias, the target half disappears too.
    EXPECT_EQ(rendered(QStringLiteral("see [[Target|shown]] here\n")),
              QStringLiteral("see shown here\n"));
    EXPECT_TRUE(hasStyle(decorationsOf(QStringLiteral("see [[Target|shown]]\n")),
                         StyleSpan::Kind::Link, QStringLiteral("Target")));
}

TEST(DecorationTest, HighlightsCommentsAndFrontmatter)
{
    EXPECT_EQ(rendered(QStringLiteral("a ==lit== b\n")), QStringLiteral("a lit b\n"));
    EXPECT_EQ(rendered(QStringLiteral("a %%hidden%% b\n")), QStringLiteral("a  b\n"));
    EXPECT_EQ(rendered(QStringLiteral("---\ntitle: x\n---\nbody\n")),
              QStringLiteral("body\n"));
}

TEST(DecorationTest, BlocksCoverTheDocument)
{
    const Decorations decorations = decorationsOf(QStringLiteral(
        "---\na: 1\n---\n\n# Title\n\nA paragraph.\n\n- one\n- two\n\n```cpp\nint x;\n```\n"));

    QList<BlockSpan::Kind> kinds;
    for (const BlockSpan &block : decorations.blocks)
        kinds << block.kind;

    EXPECT_TRUE(kinds.contains(BlockSpan::Kind::Frontmatter));
    EXPECT_TRUE(kinds.contains(BlockSpan::Kind::Heading));
    EXPECT_TRUE(kinds.contains(BlockSpan::Kind::Paragraph));
    EXPECT_TRUE(kinds.contains(BlockSpan::Kind::List));
    EXPECT_TRUE(kinds.contains(BlockSpan::Kind::Code));
    EXPECT_TRUE(hasStyle(decorations, StyleSpan::Kind::CodeBlock, QStringLiteral("cpp")));
}

TEST(DecorationTest, TheActiveBlockShowsItsSource)
{
    const QString text = QStringLiteral("# Title\n\n**bold**\n");
    const Decorations decorations = decorationsOf(text);
    ASSERT_FALSE(decorations.blocks.isEmpty());

    const Range heading = decorations.blocks.first().range;
    // Cursor outside: the "# " is hidden. Cursor inside: nothing is.
    EXPECT_FALSE(decorations.hiddenFor(heading, text.size()).isEmpty());
    EXPECT_TRUE(decorations.hiddenFor(heading, heading.start + 1).isEmpty());
}

TEST(TableTest, CellsAlignmentAndHeader)
{
    const QString text = QStringLiteral(
        "| Phase | What | Weeks |\n"
        "|:------|:----:|------:|\n"
        "| 0 | Vault | 1 |\n"
        "| 1 | Index | 3 |\n");
    const Decorations decorations = decorationsOf(text);

    ASSERT_EQ(decorations.tables.size(), 1);
    const TableSpan &table = decorations.tables.first();
    EXPECT_EQ(table.columns, 3);
    EXPECT_EQ(table.alignment, QList<Qt::Alignment>({Qt::AlignLeft, Qt::AlignHCenter,
                                                     Qt::AlignRight}));
    // Header plus two body rows; md4qt does not report the delimiter row.
    ASSERT_EQ(table.rows.size(), 3);
    EXPECT_EQ(table.rows.first().size(), 3);

    const auto slice = [&text](const Range &range) {
        return text.mid(range.start, range.length()).trimmed();
    };
    EXPECT_EQ(slice(table.rows.at(0).at(0)), QStringLiteral("Phase"));
    EXPECT_EQ(slice(table.rows.at(1).at(1)), QStringLiteral("Vault"));
    EXPECT_EQ(slice(table.rows.at(2).at(2)), QStringLiteral("3"));
}

TEST(CalloutTest, TypeTitleAndFoldMarker)
{
    const Decorations plain = decorationsOf(QStringLiteral("> [!note] A title\n> Body.\n"));
    ASSERT_EQ(plain.callouts.size(), 1);
    EXPECT_EQ(plain.callouts.first().type, QStringLiteral("note"));
    EXPECT_EQ(plain.callouts.first().title, QStringLiteral("A title"));
    EXPECT_FALSE(plain.callouts.first().foldable);

    // No title: the type is the title, capitalised, as in Obsidian.
    const Decorations bare = decorationsOf(QStringLiteral("> [!warning]\n> Careful.\n"));
    ASSERT_EQ(bare.callouts.size(), 1);
    EXPECT_EQ(bare.callouts.first().title, QStringLiteral("Warning"));

    const Decorations folded = decorationsOf(QStringLiteral("> [!tip]- Hidden\n> Body.\n"));
    ASSERT_EQ(folded.callouts.size(), 1);
    EXPECT_TRUE(folded.callouts.first().foldable);
    EXPECT_TRUE(folded.callouts.first().collapsed);

    // A plain blockquote is not a callout.
    EXPECT_TRUE(decorationsOf(QStringLiteral("> Just a quote.\n")).callouts.isEmpty());
}

TEST(CalloutTest, QuoteMarkersAreHidden)
{
    EXPECT_EQ(rendered(QStringLiteral("> one\n> two\n")), QStringLiteral("one\ntwo\n"));
    // The whole first line of a callout is drawn by its header.
    EXPECT_EQ(rendered(QStringLiteral("> [!note] Title\n> Body.\n")), QStringLiteral("Body.\n"));
}

TEST(ParagraphTest, SoftBreaksAreMarked)
{
    const QString text = QStringLiteral("one\ntwo\n\nthree\n");
    const Decorations decorations = decorationsOf(text);
    ASSERT_FALSE(decorations.paragraphs.isEmpty());

    // The newline after "one" is inside the first paragraph; the blank line
    // between the paragraphs is not.
    const Range first = decorations.paragraphs.first();
    const qsizetype inside = text.indexOf(QLatin1Char('\n'));
    EXPECT_GT(inside, first.start);
    EXPECT_LT(inside, first.end);
}

TEST(ListTest, MarkersIndentAndCheckboxes)
{
    const QString text = QStringLiteral(
        "- [x] Todo\n"
        "- [ ] Not done\n"
        "- plain\n"
        "    - nested\n"
        "\n"
        "1. first\n"
        "2. second\n");
    const Decorations decorations = decorationsOf(text);
    ASSERT_EQ(decorations.listItems.size(), 6);

    const auto &items = decorations.listItems;
    EXPECT_TRUE(items.at(0).task);
    EXPECT_TRUE(items.at(0).done);
    EXPECT_EQ(text.mid(items.at(0).checkbox.start, items.at(0).checkbox.length()),
              QStringLiteral("[x]"));
    EXPECT_TRUE(items.at(1).task);
    EXPECT_FALSE(items.at(1).done);
    EXPECT_FALSE(items.at(2).task);

    // The marker starts at the line, not at the bullet, so the indent is real.
    const auto nested = *std::find_if(items.cbegin(), items.cend(),
                                      [](const ListItemSpan &item) { return item.indent > 0; });
    EXPECT_EQ(nested.indent, 2);
    EXPECT_EQ(text.mid(nested.marker.start, nested.marker.length()),
              QStringLiteral("    - "));

    const auto ordered = *std::find_if(items.cbegin(), items.cend(),
                                       [](const ListItemSpan &item) { return item.ordered; });
    EXPECT_EQ(ordered.number, 1);

    // Every marker is hidden in live preview.
    EXPECT_EQ(rendered(QStringLiteral("- [x] Todo\n- plain\n")),
              QStringLiteral("Todo\nplain\n"));
}

TEST(EmbedTest, SizeSpecInTheAliasSlot)
{
    const Decorations plain = decorationsOf(QStringLiteral("![[diagram.png]]\n"));
    ASSERT_EQ(plain.embeds.size(), 1);
    EXPECT_EQ(plain.embeds.first().target, QStringLiteral("diagram.png"));
    EXPECT_EQ(plain.embeds.first().width, 0);

    const Decorations sized = decorationsOf(QStringLiteral("![[diagram.png|240]]\n"));
    ASSERT_EQ(sized.embeds.size(), 1);
    EXPECT_EQ(sized.embeds.first().width, 240);
    EXPECT_EQ(sized.embeds.first().height, 0);

    const Decorations both = decorationsOf(QStringLiteral("![[diagram.png|240x160]]\n"));
    ASSERT_EQ(both.embeds.size(), 1);
    EXPECT_EQ(both.embeds.first().width, 240);
    EXPECT_EQ(both.embeds.first().height, 160);

    // A real alias is not a size.
    const Decorations alias = decorationsOf(QStringLiteral("![[Note|a caption]]\n"));
    ASSERT_EQ(alias.embeds.size(), 1);
    EXPECT_EQ(alias.embeds.first().width, 0);
}

TEST(WordCountTest, IgnoresFrontmatter)
{
    const Metadata metadata =
        parse(QStringLiteral("---\ntitle: one two three four\n---\n\nfive six seven\n"));
    EXPECT_EQ(metadata.wordCount, 3);
}
