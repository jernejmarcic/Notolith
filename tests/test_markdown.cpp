#include <gtest/gtest.h>

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

TEST(WordCountTest, IgnoresFrontmatter)
{
    const Metadata metadata =
        parse(QStringLiteral("---\ntitle: one two three four\n---\n\nfive six seven\n"));
    EXPECT_EQ(metadata.wordCount, 3);
}
