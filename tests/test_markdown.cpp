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

TEST(FrontmatterTest, TypesComeFromTheYamlNotTheString)
{
    const QString text = QStringLiteral(
        "---\n"
        "quoted: \"42\"\n"
        "count: 42\n"
        "one: [project]\n"
        "when: 2026-09-22T10:00:00\n"
        "empty:\n"
        "---\n"
        "\n"
        "Body.\n");
    const Metadata metadata = parse(text);
    ASSERT_EQ(metadata.properties.size(), 5);

    const auto typeOf = [&metadata](const QString &key) {
        for (const Property &property : metadata.properties) {
            if (property.key == key)
                return property.type;
        }
        return Property::Type::Unknown;
    };
    // Quoting is how a file says "this is text", so 42 and "42" differ.
    EXPECT_EQ(typeOf(QStringLiteral("quoted")), Property::Type::Text);
    EXPECT_EQ(typeOf(QStringLiteral("count")), Property::Type::Number);
    // A list of one is still a list; only the YAML knows that.
    EXPECT_EQ(typeOf(QStringLiteral("one")), Property::Type::List);
    EXPECT_EQ(typeOf(QStringLiteral("when")), Property::Type::DateTime);
    EXPECT_EQ(typeOf(QStringLiteral("empty")), Property::Type::Text);
}

TEST(FrontmatterTest, KeyOrderRawValuesAndRanges)
{
    const QString text = QStringLiteral(
        "---\n"
        "title: \"A note\"\n"
        "flow: [a, \"b c\"]\n"
        "block:\n"
        "  - first\n"
        "  - second\n"
        "---\n"
        "\n"
        "Body.\n");
    const Metadata metadata = parse(text);
    ASSERT_EQ(metadata.properties.size(), 3);

    // The file's order, not the YAML map's.
    EXPECT_EQ(metadata.properties.at(0).key, QStringLiteral("title"));
    EXPECT_EQ(metadata.properties.at(1).key, QStringLiteral("flow"));
    EXPECT_EQ(metadata.properties.at(2).key, QStringLiteral("block"));

    // The raw text is what the file says, quotes included, so an editor can put
    // a value back byte for byte.
    EXPECT_EQ(metadata.properties.at(0).values,
              QStringList({QStringLiteral("A note")}));
    EXPECT_EQ(metadata.properties.at(0).raw,
              QStringList({QStringLiteral("\"A note\"")}));
    EXPECT_EQ(metadata.properties.at(1).values,
              QStringList({QStringLiteral("a"), QStringLiteral("b c")}));
    EXPECT_EQ(metadata.properties.at(1).raw,
              QStringList({QStringLiteral("a"), QStringLiteral("\"b c\"")}));
    EXPECT_EQ(metadata.properties.at(2).raw,
              QStringList({QStringLiteral("first"), QStringLiteral("second")}));

    // An entry covers every line it is written over.
    EXPECT_EQ(sliceOf(text, metadata.properties.at(0).range),
              QStringLiteral("title: \"A note\"\n"));
    EXPECT_EQ(sliceOf(text, metadata.properties.at(2).range),
              QStringLiteral("block:\n  - first\n  - second\n"));
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
        if (link.target == QStringLiteral("Embed.png")) {
            EXPECT_TRUE(link.embed);
        }
        if (link.target == QStringLiteral("A note")) {
            EXPECT_EQ(link.subpath, QStringLiteral("#Section"));
            EXPECT_EQ(link.alias, QStringLiteral("alias"));
            EXPECT_EQ(sliceOf(text, link.targetRange), QStringLiteral("A note"));
        }
        if (link.target.startsWith(QStringLiteral("https://"))) {
            EXPECT_TRUE(link.external);
        }
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

TEST(LinkTest, ReferenceLinkFormsResolveToTheirDefinition)
{
    // The full `[text][ref]` form, its collapsed `[ref][]` shorthand, and the
    // bare shortcut `[ref]` must all end up pointing at the definition's URL,
    // not at the label md4qt reports as the target.
    const QString full = QStringLiteral("A [text][ref] here.\n\n[ref]: https://example.com \"Title\"\n");
    const QString collapsed = QStringLiteral("A [ref][] here.\n\n[ref]: https://example.com\n");
    const QString shortcut = QStringLiteral("A [ref] here.\n\n[ref]: https://example.com\n");

    for (const QString &text : {full, collapsed, shortcut}) {
        const Metadata metadata = parse(text);
        ASSERT_EQ(metadata.links.size(), 1) << text.toStdString();
        const Link &link = metadata.links.first();
        EXPECT_EQ(link.target, QStringLiteral("https://example.com")) << text.toStdString();
        EXPECT_TRUE(link.raw == QStringLiteral("https://example.com")) << text.toStdString();
        EXPECT_FALSE(link.wiki) << text.toStdString();
        // A reference link whose definition is external is external the same
        // way an inline link written straight to that URL would be.
        EXPECT_TRUE(link.external) << text.toStdString();
    }
}

TEST(LinkTest, ReferenceLabelMatchingFoldsCaseAndWhitespace)
{
    // CommonMark folds a label's case and collapses its internal whitespace
    // before matching it against a definition.
    const QString text = QStringLiteral(
        "A [text][My   Ref] here.\n\n[my ref]: https://example.com\n");
    const Metadata metadata = parse(text);
    ASSERT_EQ(metadata.links.size(), 1);
    EXPECT_EQ(metadata.links.first().target, QStringLiteral("https://example.com"));
}

TEST(LinkTest, ReferenceDefinitionMayFollowTheLink)
{
    const QString text = QStringLiteral(
        "A [text][ref] here.\n\nSome other paragraph.\n\n[ref]: https://example.com\n");
    const Metadata metadata = parse(text);
    ASSERT_EQ(metadata.links.size(), 1);
    EXPECT_EQ(metadata.links.first().target, QStringLiteral("https://example.com"));
}

TEST(LinkTest, UnresolvedReferenceLabelStaysUnresolved)
{
    // No definition anywhere - md4qt does not even see a link, so nothing is
    // invented for one; the brackets are left as plain text.
    const QString text = QStringLiteral("A [text][nope] here.\n");
    const Metadata metadata = parse(text);
    EXPECT_TRUE(metadata.links.isEmpty());
}

TEST(LinkTest, ReferenceDefinitionInsideACodeFenceIsNotADefinition)
{
    // The only "[ref]: ..." line is inside a fence, so it does not count and
    // the reference link it would have fed stays unresolved.
    const QString onlyFenced = QStringLiteral(
        "A [text][ref] here.\n\n```\n[ref]: https://example.com\n```\n");
    EXPECT_TRUE(parse(onlyFenced).links.isEmpty());

    // A real definition elsewhere must win over a fenced decoy with the same
    // label, even when the decoy comes first in the file.
    const QString decoyFirst = QStringLiteral(
        "```\n[ref]: https://decoy.example\n```\n\n"
        "A [text][ref] here.\n\n"
        "[ref]: https://real.example\n");
    const Metadata metadata = parse(decoyFirst);
    ASSERT_EQ(metadata.links.size(), 1);
    EXPECT_EQ(metadata.links.first().target, QStringLiteral("https://real.example"));
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

TEST(DecorationTest, InlineLinksShowOnlyTheirText)
{
    EXPECT_EQ(rendered(QStringLiteral("A [Obsidian](https://obsidian.md) here.\n")),
              QStringLiteral("A Obsidian here.\n"));
    // A title is part of the syntax, not part of the label.
    EXPECT_EQ(rendered(QStringLiteral("A [text](url \"a title\") here.\n")),
              QStringLiteral("A text here.\n"));
    // An autolink shows its URL, without the angle brackets.
    EXPECT_EQ(rendered(QStringLiteral("At <https://example.com> today.\n")),
              QStringLiteral("At https://example.com today.\n"));
    // An image is an embed: the alt text stands in for it until it is drawn,
    // exactly as the target does for a "![[file.png]]" embed.
    EXPECT_EQ(rendered(QStringLiteral("![alt](diagram.png)\n")), QStringLiteral("alt\n"));
    // Nothing to show at all, so nothing is left behind either.
    EXPECT_EQ(rendered(QStringLiteral("x [](url) y\n")), QStringLiteral("x  y\n"));

    EXPECT_TRUE(hasStyle(decorationsOf(QStringLiteral("[Obsidian](https://obsidian.md)\n")),
                         StyleSpan::Kind::Link, QStringLiteral("https://obsidian.md")));
}

TEST(DecorationTest, ReferenceLinksAndTheirDefinitions)
{
    const QString text = QStringLiteral(
        "A [text][ref] here.\n"
        "\n"
        "[ref]: https://example.com \"Title\"\n");
    EXPECT_EQ(rendered(text), QStringLiteral("A text here.\n\n"));

    // A shortcut reference is its own label.
    EXPECT_EQ(rendered(QStringLiteral("A [ref] here.\n\n[ref]: https://example.com\n")),
              QStringLiteral("A ref here.\n\n"));

    // A definition inside a code block is code.
    EXPECT_EQ(rendered(QStringLiteral("```\n[ref]: https://example.com\n```\n")),
              QStringLiteral("\n[ref]: https://example.com\n\n"));
}

TEST(LinkTest, ReferenceImageResolvesToItsDefinitionToo)
{
    // The same mistarget a reference *link* gets from md4qt also leaks into a
    // reference-style embed; it needs the same fix.
    const QString full = QStringLiteral("![alt][ref]\n\n[ref]: diagram.png\n");
    const QString shortcut = QStringLiteral("![ref]\n\n[ref]: diagram.png\n");

    for (const QString &text : {full, shortcut}) {
        const Metadata metadata = parse(text);
        ASSERT_EQ(metadata.links.size(), 1) << text.toStdString();
        const Link &link = metadata.links.first();
        EXPECT_TRUE(link.embed) << text.toStdString();
        EXPECT_EQ(link.target, QStringLiteral("diagram.png")) << text.toStdString();

        const Decorations decorations = decorationsOf(text);
        ASSERT_EQ(decorations.embeds.size(), 1) << text.toStdString();
        EXPECT_EQ(decorations.embeds.first().target, QStringLiteral("diagram.png")) << text.toStdString();
    }
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

TEST(CalloutTest, MetadataSlotCarriesStyle)
{
    // noteAppC's own styling rides in the `|` slot Obsidian already parses and
    // ignores, so a styled note still opens there - just unstyled.
    const Decorations styled = decorationsOf(
        QStringLiteral("> [!deadline|bar=#FF5A5Fff bg=\"#FF5A5F26\" icon=zap] Ship it\n> Body.\n"));
    ASSERT_EQ(styled.callouts.size(), 1);
    const CalloutSpan &callout = styled.callouts.first();
    EXPECT_EQ(callout.type, QStringLiteral("deadline"));
    EXPECT_EQ(callout.title, QStringLiteral("Ship it"));
    EXPECT_EQ(callout.attributes.value(QStringLiteral("bar")), QStringLiteral("#FF5A5Fff"));
    EXPECT_EQ(callout.attributes.value(QStringLiteral("bg")), QStringLiteral("#FF5A5F26"));
    EXPECT_EQ(callout.attributes.value(QStringLiteral("icon")), QStringLiteral("zap"));

    // The fold marker still reads after the slot, and the whole marker is
    // hidden, attributes included.
    const QString folded = QStringLiteral("> [!tip|bg=#0F0F0F80]- Hidden\n> Body.\n");
    const Decorations decorations = decorationsOf(folded);
    ASSERT_EQ(decorations.callouts.size(), 1);
    EXPECT_TRUE(decorations.callouts.first().foldable);
    EXPECT_TRUE(decorations.callouts.first().collapsed);
    EXPECT_EQ(sliceOf(folded, decorations.callouts.first().marker),
              QStringLiteral("[!tip|bg=#0F0F0F80]-"));
    EXPECT_EQ(rendered(folded), QStringLiteral("Body.\n"));

    // The `:::` container carries the same keys in its braces.
    const Decorations fenced = decorationsOf(
        QStringLiteral("::: {.deadline bar=\"#FF5A5Fff\" title=\"Ship it\"}\nBody.\n:::\n"));
    ASSERT_EQ(fenced.callouts.size(), 1);
    EXPECT_EQ(fenced.callouts.first().type, QStringLiteral("deadline"));
    EXPECT_EQ(fenced.callouts.first().attributes.value(QStringLiteral("bar")),
              QStringLiteral("#FF5A5Fff"));
}

TEST(CalloutTest, NestingIsRecorded)
{
    const Decorations nested = decorationsOf(QStringLiteral(
        "> [!tip] Outer\n> Outer body.\n> > [!info] Inner\n> > Inner body.\n"));
    ASSERT_EQ(nested.callouts.size(), 2);
    // Sorted by where they start, so the outer one comes first.
    const CalloutSpan *outer = &nested.callouts.first();
    const CalloutSpan *inner = &nested.callouts.last();
    if (outer->depth > inner->depth)
        std::swap(outer, inner);
    EXPECT_EQ(outer->type, QStringLiteral("tip"));
    EXPECT_EQ(outer->depth, 0);
    EXPECT_EQ(inner->type, QStringLiteral("info"));
    EXPECT_EQ(inner->depth, 1);
    EXPECT_TRUE(outer->range.contains(inner->range.start));
}

TEST(CalloutTest, QuoteMarkersAreHidden)
{
    EXPECT_EQ(rendered(QStringLiteral("> one\n> two\n")), QStringLiteral("one\ntwo\n"));
    // The whole first line of a callout is drawn by its header.
    EXPECT_EQ(rendered(QStringLiteral("> [!note] Title\n> Body.\n")), QStringLiteral("Body.\n"));
}

TEST(ColonFenceTest, BareAttributeAndDetailsForms)
{
    const QString bare = QStringLiteral("::: note\nBody text.\n:::\n");
    const Decorations plain = decorationsOf(bare);
    ASSERT_EQ(plain.callouts.size(), 1);
    EXPECT_EQ(plain.callouts.first().type, QStringLiteral("note"));
    EXPECT_EQ(plain.callouts.first().title, QStringLiteral("Note"));
    EXPECT_EQ(sliceOf(bare, plain.callouts.first().range), bare);
    EXPECT_EQ(sliceOf(bare, plain.callouts.first().marker), QStringLiteral("::: note\n"));
    // Both fence lines are syntax, so live preview shows only the body.
    EXPECT_EQ(rendered(bare), QStringLiteral("Body text.\n"));

    const Decorations attributes = decorationsOf(
        QStringLiteral("::: {.note title=\"Read me\" open=false}\nBody.\n:::\n"));
    ASSERT_EQ(attributes.callouts.size(), 1);
    EXPECT_EQ(attributes.callouts.first().type, QStringLiteral("note"));
    EXPECT_EQ(attributes.callouts.first().title, QStringLiteral("Read me"));
    EXPECT_TRUE(attributes.callouts.first().foldable);
    EXPECT_TRUE(attributes.callouts.first().collapsed);

    // A disclosure names its header "summary" and starts closed.
    const Decorations details =
        decorationsOf(QStringLiteral("::: details summary=\"The long version\"\nBody.\n:::\n"));
    ASSERT_EQ(details.callouts.size(), 1);
    EXPECT_EQ(details.callouts.first().type, QStringLiteral("details"));
    EXPECT_EQ(details.callouts.first().title, QStringLiteral("The long version"));
    EXPECT_TRUE(details.callouts.first().collapsed);

    // An unknown class still gets a callout, so the editor can draw a border
    // and the colour scheme can style it by name.
    const Decorations unknown = decorationsOf(QStringLiteral("::: sidenote\nBody.\n:::\n"));
    ASSERT_EQ(unknown.callouts.size(), 1);
    EXPECT_EQ(unknown.callouts.first().type, QStringLiteral("sidenote"));
}

TEST(ColonFenceTest, NestingGoesByFenceLength)
{
    const QString text = QStringLiteral(
        ":::: warning\n"
        "outer\n"
        "\n"
        "::: note\n"
        "inner\n"
        ":::\n"
        "\n"
        "::::\n");
    const Decorations decorations = decorationsOf(text);
    ASSERT_EQ(decorations.callouts.size(), 2);

    // The inner fence closes the inner container only; the whole of it sits
    // inside the outer one.
    const auto byType = [&decorations](const QString &type) {
        return *std::find_if(decorations.callouts.cbegin(), decorations.callouts.cend(),
                             [&type](const CalloutSpan &callout) { return callout.type == type; });
    };
    const CalloutSpan outer = byType(QStringLiteral("warning"));
    const CalloutSpan inner = byType(QStringLiteral("note"));
    EXPECT_EQ(sliceOf(text, outer.range), text);
    EXPECT_EQ(sliceOf(text, inner.range), QStringLiteral("::: note\ninner\n:::\n"));

    EXPECT_EQ(rendered(text), QStringLiteral("outer\n\ninner\n\n"));

    // One Callout block per container, and it owns the lines md4qt read as
    // paragraphs inside it.
    int containers = 0;
    for (const BlockSpan &block : decorations.blocks) {
        if (block.kind == BlockSpan::Kind::Callout)
            ++containers;
        EXPECT_FALSE(block.kind == BlockSpan::Kind::Paragraph
                     && block.range.start >= outer.range.start
                     && block.range.end <= outer.range.end);
    }
    EXPECT_EQ(containers, 2);
}

TEST(ColonFenceTest, NotAFenceInsideCodeOrWithoutAClose)
{
    // Inside a code block colons are just colons.
    EXPECT_TRUE(decorationsOf(QStringLiteral("```\n::: note\nx\n:::\n```\n")).callouts.isEmpty());

    // An unclosed container runs to the end of the note, as a code fence does.
    const QString text = QStringLiteral("::: tip\nTo the end.\n");
    const Decorations decorations = decorationsOf(text);
    ASSERT_EQ(decorations.callouts.size(), 1);
    EXPECT_EQ(decorations.callouts.first().range.end, text.size());
}

TEST(CodeOptionTest, RecognisedKeysAndHiddenLines)
{
    const QString text = QStringLiteral(
        "```python\n"
        "#| title: hello.py\n"
        "#| lines: 3\n"
        "#| highlight: 1,3-4\n"
        "#| wrap: true\n"
        "#| caption: A greeting\n"
        "print(1)\n"
        "print(2)\n"
        "print(3)\n"
        "print(4)\n"
        "```\n");
    const Decorations decorations = decorationsOf(text);
    ASSERT_EQ(decorations.codeOptions.size(), 1);

    const CodeOptions &options = decorations.codeOptions.first();
    EXPECT_EQ(options.title, QStringLiteral("hello.py"));
    EXPECT_EQ(options.caption, QStringLiteral("A greeting"));
    EXPECT_TRUE(options.lineNumbers);
    EXPECT_EQ(options.firstLine, 3);
    EXPECT_EQ(options.highlight, QList<int>({1, 3, 4}));
    EXPECT_TRUE(options.wrap);
    EXPECT_FALSE(options.fold);

    // The options belong to the code block, and are found by its range.
    const auto block = std::find_if(decorations.blocks.cbegin(), decorations.blocks.cend(),
                                    [](const BlockSpan &span) {
                                        return span.kind == BlockSpan::Kind::Code;
                                    });
    ASSERT_NE(block, decorations.blocks.cend());
    EXPECT_EQ(options.block, block->range);

    // They stay in the file and leave the listing.
    EXPECT_EQ(rendered(text),
              QStringLiteral("\nprint(1)\nprint(2)\nprint(3)\nprint(4)\n\n"));
}

TEST(CodeOptionTest, PrefixFollowsTheLanguage)
{
    const Decorations sql = decorationsOf(QStringLiteral(
        "```sql\n--| title: query.sql\nSELECT 1;\n```\n"));
    ASSERT_EQ(sql.codeOptions.size(), 1);
    EXPECT_EQ(sql.codeOptions.first().title, QStringLiteral("query.sql"));

    const Decorations cpp = decorationsOf(QStringLiteral(
        "```cpp\n//| title: main.cpp\n//| lines: true\nint x;\n```\n"));
    ASSERT_EQ(cpp.codeOptions.size(), 1);
    EXPECT_EQ(cpp.codeOptions.first().title, QStringLiteral("main.cpp"));
    EXPECT_TRUE(cpp.codeOptions.first().lineNumbers);
    EXPECT_EQ(cpp.codeOptions.first().firstLine, 1);

    const Decorations lisp = decorationsOf(QStringLiteral(
        "```scheme\n;;| fold: true\n(+ 1 1)\n```\n"));
    ASSERT_EQ(lisp.codeOptions.size(), 1);
    EXPECT_TRUE(lisp.codeOptions.first().fold);

    const Decorations tex = decorationsOf(QStringLiteral(
        "```latex\n%| title: proof.tex\n\\alpha\n```\n"));
    ASSERT_EQ(tex.codeOptions.size(), 1);
    EXPECT_EQ(tex.codeOptions.first().title, QStringLiteral("proof.tex"));

    // The wrong comment token is code, not an option.
    const QString wrong = QStringLiteral("```sql\n#| title: query.sql\nSELECT 1;\n```\n");
    EXPECT_TRUE(decorationsOf(wrong).codeOptions.isEmpty());
    EXPECT_EQ(rendered(wrong), QStringLiteral("\n#| title: query.sql\nSELECT 1;\n\n"));
}

TEST(CodeOptionTest, QuartoExecutionKeysAreParsedAndIgnored)
{
    const QString text = QStringLiteral(
        "```r\n"
        "#| eval: false\n"
        "#| echo: true\n"
        "#| warning: false\n"
        "#| label: fig-one\n"
        "plot(x)\n"
        "```\n");
    const Decorations decorations = decorationsOf(text);
    ASSERT_EQ(decorations.codeOptions.size(), 1);

    const CodeOptions &options = decorations.codeOptions.first();
    EXPECT_EQ(options.extra.value(QStringLiteral("eval")), QStringLiteral("false"));
    EXPECT_EQ(options.extra.value(QStringLiteral("echo")), QStringLiteral("true"));
    EXPECT_EQ(options.extra.value(QStringLiteral("label")), QStringLiteral("fig-one"));
    // Nothing here runs anything, and nothing here renders either.
    EXPECT_TRUE(options.title.isEmpty());
    EXPECT_EQ(rendered(text), QStringLiteral("\nplot(x)\n\n"));

    // Only the leading run of option lines counts.
    const Decorations later = decorationsOf(
        QStringLiteral("```r\nplot(x)\n#| eval: false\n```\n"));
    EXPECT_TRUE(later.codeOptions.isEmpty());
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

TEST(OffsetTest, EveryRangeStaysInsideTheDocument)
{
    // The editor's DisplayMap is built from these offsets; one that is out by a
    // character corrupts every position after it.
    const QString text = QStringLiteral(
        "---\ntitle: Ünïcode\ntags: [a, b]\n---\n"
        "\n"
        "# Hëading with [a link](https://example.com \"t\") and ![img](pic.png)\n"
        "\n"
        "::: {.note title=\"Ünïcode\"}\n"
        "Body with [[Wiki|alias]], ==lit==, #tag and <https://example.com>.\n"
        ":::\n"
        "\n"
        "```python\n#| title: hé.py\n#| highlight: 1-2\nprint(1)\n```\n"
        "\n"
        "[ref]: https://example.com \"Title\"\n");
    const Decorations decorations = decorationsOf(text);

    const auto sane = [&text](const Range &range) {
        return range.isValid() && range.start >= 0 && range.end <= text.size();
    };
    for (const Range &range : decorations.syntax)
        EXPECT_TRUE(sane(range)) << range.start << ".." << range.end;
    for (const BlockSpan &block : decorations.blocks)
        EXPECT_TRUE(sane(block.range));
    for (const CalloutSpan &callout : decorations.callouts) {
        EXPECT_TRUE(sane(callout.range));
        EXPECT_TRUE(sane(callout.marker));
    }
    for (const CodeOptions &options : decorations.codeOptions) {
        EXPECT_TRUE(sane(options.block));
        EXPECT_TRUE(sane(options.range));
    }
    for (const StyleSpan &style : decorations.styles)
        EXPECT_TRUE(sane(style.range));

    // The frontmatter ranges are cut from a UTF-8 parse; non-ASCII must not
    // shift them.
    const Metadata metadata = parse(text);
    ASSERT_EQ(metadata.properties.size(), 2);
    EXPECT_EQ(sliceOf(text, metadata.properties.first().range),
              QStringLiteral("title: Ünïcode\n"));
    EXPECT_EQ(metadata.properties.first().raw,
              QStringList({QStringLiteral("Ünïcode")}));
}

TEST(WordCountTest, IgnoresFrontmatter)
{
    const Metadata metadata =
        parse(QStringLiteral("---\ntitle: one two three four\n---\n\nfive six seven\n"));
    EXPECT_EQ(metadata.wordCount, 3);
}

// `highlight` numbers the lines the reader sees. Clamping them against a count
// that still included the `#|` lines silently accepted numbers past the end.
TEST(CodeOptionTest, HighlightCountsTheVisibleListingNotTheOptionLines)
{
    const QString text = QStringLiteral(
        "```python\n"
        "#| title: t.py\n"
        "#| lines: true\n"
        "#| highlight: 1,3,9\n"
        "one\n"
        "two\n"
        "three\n"
        "```\n");

    const Decorations decorations = decorationsOf(text);
    ASSERT_EQ(decorations.codeOptions.size(), 1);
    const CodeOptions &options = decorations.codeOptions.first();
    // Three lines of listing, so 9 is out of range and 1 and 3 survive.
    EXPECT_EQ(options.highlight, QList<int>({1, 3}));
}

