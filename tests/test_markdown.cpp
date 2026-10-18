#include <gtest/gtest.h>

#include <algorithm>

#include <QElapsedTimer>

#include "Decorations.h"
#include "Frontmatter.h"
#include "ObsidianSyntax.h"
#include "Parser.h"
#include "SourceMap.h"
#include "TableFormat.h"
#include "Perf.h"

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

TEST(SourceMapTest, AnEditIsTheSameAsRebuilding)
{
    // The editor moves its line starts on every keystroke instead of
    // rescanning the note; any edit must land where a rebuild would.
    QString text = QStringLiteral("one\ntwo\n\nfour\nfive\n");
    SourceMap map(text);
    const QStringList pieces = {QString(), QStringLiteral("x"), QStringLiteral("\n"),
                                QStringLiteral("a\nb"), QStringLiteral("\n\n"),
                                QStringLiteral("tail\n")};
    quint32 seed = 7;
    const auto next = [&seed](quint32 bound) {
        seed = seed * 1664525u + 1013904223u; // any fixed sequence will do
        return bound ? (seed >> 8) % bound : 0;
    };
    for (int step = 0; step < 2000; ++step) {
        const qsizetype at = next(quint32(text.size() + 1));
        const qsizetype removed = qMin<qsizetype>(next(4), text.size() - at);
        const QString inserted = pieces.at(next(quint32(pieces.size())));
        text.replace(at, removed, inserted);
        map.applyEdit(at, removed, inserted);

        const SourceMap rebuilt(text);
        ASSERT_EQ(map.lineCount(), rebuilt.lineCount()) << "step " << step;
        for (qsizetype line = 0; line < rebuilt.lineCount(); ++line)
            ASSERT_EQ(map.lineStart(line), rebuilt.lineStart(line)) << "step " << step;
        ASSERT_EQ(map.lineEnd(rebuilt.lineCount() - 1), text.size()) << "step " << step;
    }
}

TEST(FrontmatterTest, AWrittenValueReadsBackTheSame)
{
    // What the properties table writes has to come back as the same value
    // and the same type, whatever the characters.
    const auto readBack = [](const QString &yaml) {
        const QString text = QStringLiteral("---\nkey: %1\n---\n").arg(yaml);
        Metadata metadata;
        Frontmatter::parse(text, Frontmatter::find(text), metadata);
        return metadata.properties.value(0);
    };
    const QStringList texts = {QStringLiteral("plain words"), QStringLiteral("a: b"),
                               QStringLiteral("#tag"), QStringLiteral("[x]"),
                               QStringLiteral("42"), QStringLiteral("true"),
                               QStringLiteral("2026-09-23"), QStringLiteral("say \"hi\""),
                               QStringLiteral("Škofja Loka")};
    for (const QString &value : texts) {
        const std::optional<QString> yaml = Frontmatter::scalarFor(value, Property::Type::Text);
        ASSERT_TRUE(yaml.has_value()) << qPrintable(value);
        const Property property = readBack(*yaml);
        EXPECT_EQ(property.values.value(0), value) << qPrintable(*yaml);
        EXPECT_EQ(property.type, Property::Type::Text) << qPrintable(*yaml);
    }
    EXPECT_EQ(Frontmatter::scalarFor(QStringLiteral("plain words"), Property::Type::Text),
              QStringLiteral("plain words")) << "quoted only when it has to be";

    EXPECT_EQ(readBack(*Frontmatter::scalarFor(QStringLiteral("3.5"), Property::Type::Number)).type,
              Property::Type::Number);
    EXPECT_EQ(readBack(*Frontmatter::scalarFor(QStringLiteral("2026-09-23"), Property::Type::Date)).type,
              Property::Type::Date);
    EXPECT_FALSE(Frontmatter::scalarFor(QStringLiteral("soon"), Property::Type::Number));
    EXPECT_FALSE(Frontmatter::scalarFor(QStringLiteral("2026-13-40"), Property::Type::Date));
    EXPECT_FALSE(Frontmatter::scalarFor(QStringLiteral("a"), Property::Type::List));
}

TEST(FrontmatterTest, AListIsRewrittenInTheStyleItWasWrittenIn)
{
    const QString block = QStringLiteral("tags:\n    - one\n    - 'two'\n\n");
    EXPECT_EQ(Frontmatter::rewriteList(block, {QStringLiteral("one"), QStringLiteral("'two'"),
                                               QStringLiteral("three")}),
              QStringLiteral("tags:\n    - one\n    - 'two'\n    - three\n\n"))
        << "indent kept, items kept as written, the blank line after left where it was";
    EXPECT_EQ(Frontmatter::rewriteList(block, {QStringLiteral("'two'")}),
              QStringLiteral("tags:\n    - 'two'\n\n"));
    EXPECT_EQ(Frontmatter::rewriteList(block, {}), QStringLiteral("tags: []\n\n"));

    EXPECT_EQ(Frontmatter::rewriteList(QStringLiteral("tags: [a, b]\n"), {QStringLiteral("a")}),
              QStringLiteral("tags: [a]\n"));
    EXPECT_EQ(Frontmatter::rewriteList(QStringLiteral("tags: []\n"), {QStringLiteral("x")}),
              QStringLiteral("tags: [x]\n"));

    EXPECT_FALSE(Frontmatter::rewriteList(QStringLiteral("tags:\n  - a # why\n"), {}));
    EXPECT_FALSE(Frontmatter::rewriteList(QStringLiteral("tags:\n  # note\n  - a\n"), {}));
    EXPECT_FALSE(Frontmatter::rewriteList(QStringLiteral("tags:\n  - a:\n      b\n"), {}));
    EXPECT_FALSE(Frontmatter::rewriteList(QStringLiteral("tags: [a,\n  b]\n"), {}));

    // What comes out reads back as the same list.
    const QString text = QStringLiteral("---\n%1---\n")
                             .arg(*Frontmatter::rewriteList(block, {QStringLiteral("one"),
                                                                    *Frontmatter::scalarFor(QStringLiteral("a: b"),
                                                                                            Property::Type::Text)}));
    Metadata metadata;
    Frontmatter::parse(text, Frontmatter::find(text), metadata);
    ASSERT_EQ(metadata.properties.size(), 1);
    EXPECT_EQ(metadata.properties.first().type, Property::Type::List);
    EXPECT_EQ(metadata.properties.first().values, (QStringList{QStringLiteral("one"), QStringLiteral("a: b")}));
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

TEST(FrontmatterTest, AScalarTagsOrAliasesValueIsSplitLikeObsidian)
{
    // Tags split on commas and whitespace, aliases on commas only; the
    // property itself stays as written, so nothing is rewritten on disk.
    const Metadata metadata = parse(QStringLiteral(
        "---\ntags: \"#old, project urgent\"\naliases: Brain, Mind\n---\n"));
    EXPECT_EQ(tagNames(metadata), QStringList({QStringLiteral("old"), QStringLiteral("project"),
                                               QStringLiteral("urgent")}));
    EXPECT_EQ(metadata.aliases, QStringList({QStringLiteral("Brain"), QStringLiteral("Mind")}));
    ASSERT_EQ(metadata.properties.size(), 2);
    EXPECT_EQ(metadata.properties.at(0).type, Property::Type::Text);
    EXPECT_EQ(metadata.properties.at(0).values,
              QStringList({QStringLiteral("#old, project urgent")}));
    EXPECT_EQ(metadata.properties.at(1).values, QStringList({QStringLiteral("Brain, Mind")}));

    // A lone alias with no comma is untouched, spaces and all.
    EXPECT_EQ(parse(QStringLiteral("---\nalias: Second brain\n---\n")).aliases,
              QStringList({QStringLiteral("Second brain")}));
}

TEST(FrontmatterTest, AScalarTagsValueSplitsWithoutQuotes)
{
    const Metadata metadata = parse(QStringLiteral("---\ntags: project, urgent #old\n---\n"));
    // " #old" is a YAML comment, so only the two real words are tags.
    EXPECT_EQ(tagNames(metadata), QStringList({QStringLiteral("project"), QStringLiteral("urgent")}));
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

// `[[N#A#B]]` names the B nested under A, not the first B in the note.
TEST(WikilinkTest, ANestedHeadingPathFindsTheHeadingUnderItsParent)
{
    const QString text = QStringLiteral(
        "# Intro\n### Detail\n# A\n## Plan\n### Detail\n# C\n## Plan\n### Detail\n### Other\n");
    const QList<Heading> headings = Parser().parseAll(text).metadata.headings;
    ASSERT_EQ(headings.size(), 9);
    // Single segment: first match anywhere, as before.
    EXPECT_EQ(ObsidianSyntax::findHeading(headings, QStringLiteral("#Detail")), 1);
    EXPECT_EQ(ObsidianSyntax::findHeading(headings, QStringLiteral("Detail")), 1);
    // Nested: inside the parent's section only, however far down.
    EXPECT_EQ(ObsidianSyntax::findHeading(headings, QStringLiteral("#A#Detail")), 4);
    EXPECT_EQ(ObsidianSyntax::findHeading(headings, QStringLiteral("#C#Detail")), 7);
    EXPECT_EQ(ObsidianSyntax::findHeading(headings, QStringLiteral("#C#Plan#Detail")), 7);
    EXPECT_EQ(ObsidianSyntax::findHeading(headings, QStringLiteral("#a#plan!#detail")), 4);
    // Not under that parent, or the parent is absent: no match.
    EXPECT_EQ(ObsidianSyntax::findHeading(headings, QStringLiteral("#A#Other")), -1);
    EXPECT_EQ(ObsidianSyntax::findHeading(headings, QStringLiteral("#Nowhere#Detail")), -1);
    // A heading with a `#` in its text still answers to the whole path.
    const QList<Heading> sharp = Parser().parseAll(QStringLiteral("# C# tips\n")).metadata.headings;
    EXPECT_EQ(ObsidianSyntax::findHeading(sharp, QStringLiteral("#C# tips")), 0);

    // What `[[N#A#` offers: the headings in A's section.
    EXPECT_EQ(ObsidianSyntax::headingsUnder(headings, {QStringLiteral("A")}), (QList<qsizetype>{3, 4}));
    EXPECT_EQ(ObsidianSyntax::headingsUnder(headings, {QStringLiteral("C"), QStringLiteral("Plan")}),
              (QList<qsizetype>{7, 8}));
    EXPECT_EQ(ObsidianSyntax::headingsUnder(headings, {}).size(), 9);
    EXPECT_TRUE(ObsidianSyntax::headingsUnder(headings, {QStringLiteral("Detail"), QStringLiteral("X")}).isEmpty());
}

// The key forgives punctuation, which made `C`, `C++` and `C#` one heading, the first: the exact text wins.
TEST(WikilinkTest, AHeadingLinkPrefersTheHeadingWithTheSameText)
{
    const QList<Heading> headings = Parser().parseAll(QStringLiteral("# C\n# C++\n## Notes\n# cafe\u0301\n")).metadata.headings;
    ASSERT_EQ(headings.size(), 4);
    EXPECT_EQ(ObsidianSyntax::findHeading(headings, QStringLiteral("#C++")), 1);
    EXPECT_EQ(ObsidianSyntax::findHeading(headings, QStringLiteral("#C")), 0);
    EXPECT_EQ(ObsidianSyntax::findHeading(headings, QStringLiteral("#c++")), 1);
    EXPECT_EQ(ObsidianSyntax::findHeading(headings, QStringLiteral("#C+")), 0) << "no exact text: the key answers, first match";
    EXPECT_EQ(ObsidianSyntax::findHeading(headings, QStringLiteral("#C++#Notes")), 2);
    // Decomposed and precomposed accents are the same heading.
    EXPECT_EQ(ObsidianSyntax::findHeading(headings, QStringLiteral("#caf\u00e9")), 3);
    EXPECT_EQ(ObsidianSyntax::headingKey(QStringLiteral("e\u0301")), ObsidianSyntax::headingKey(QStringLiteral("\u00e9")));
}

TEST(WikilinkTest, AHeadingLinkMatchesWithPunctuationIgnored)
{
    EXPECT_EQ(ObsidianSyntax::headingKey(QStringLiteral("Part 1: Intro?")),
              ObsidianSyntax::headingKey(QStringLiteral("Part 1 Intro!")));
    EXPECT_EQ(ObsidianSyntax::headingKey(QStringLiteral("Q&A | notes [x] #1")),
              QStringLiteral("Q A notes x 1"));
    EXPECT_EQ(ObsidianSyntax::headingKey(QStringLiteral("Caf\u00e9 \U00020000")),
              QStringLiteral("Caf\u00e9 \U00020000"));
    EXPECT_NE(ObsidianSyntax::headingKey(QStringLiteral("Part 1")),
              ObsidianSyntax::headingKey(QStringLiteral("Part 2")));
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

TEST(TagTest, AColonFencesAttributesAreNotTags)
{
    const QString text = QStringLiteral(
        "::: {.tip bar=\"#4C8DFFff\" title=\"Styled\"}\n"
        "Inside, #real is a tag.\n"
        ":::\n");
    EXPECT_EQ(tagNames(parse(text)), QStringList{QStringLiteral("real")});
}

TEST(FootnoteTest, ReferencesInlineNotesAndDefinitionsAreNumberedInOrder)
{
    const QString text = QStringLiteral(
        "First.[^a] Inline.^[With a [[Link]] inside.] Again.[^a] Missing.[^none]\n"
        "`[^a]` in code is text.\n"
        "\n"
        "[^a]: The body,\n"
        "continued lazily.\n"
        "\n"
        "    And an indented paragraph.\n"
        "\n"
        "After the footnote.\n");
    const Decorations decorations = Parser().parseAll(text).decorations;
    ASSERT_EQ(decorations.footnoteDefinitions.size(), 1);
    const FootnoteDefinition &definition = decorations.footnoteDefinitions.first();
    EXPECT_EQ(definition.label, QStringLiteral("a"));
    const QString body = text.mid(definition.content.start, definition.content.length());
    EXPECT_TRUE(body.startsWith(QLatin1String("The body,")));
    EXPECT_TRUE(body.endsWith(QLatin1String("And an indented paragraph.")));

    ASSERT_EQ(decorations.footnotes.size(), 3); // [^none] has no definition, the code span is code
    EXPECT_EQ(decorations.footnotes.at(0).number, 1);
    EXPECT_EQ(decorations.footnotes.at(1).label, QString());
    EXPECT_EQ(decorations.footnotes.at(1).number, 2);
    EXPECT_EQ(text.mid(decorations.footnotes.at(1).content.start, decorations.footnotes.at(1).content.length()),
              QStringLiteral("With a [[Link]] inside."));
    EXPECT_EQ(decorations.footnotes.at(2).number, 1); // the same label, the same number
    // Not a link reference definition, whose whole line would be hidden.
    for (const Range &hidden : decorations.syntax)
        EXPECT_FALSE(hidden.contains(definition.content.start));
}

TEST(ListTest, NestingCountingBulletsAndOutlines)
{
    const QString text = QStringLiteral(
        "1. a\n"
        "1. b\n"
        "   1. b.1\n"
        "   5. b.2\n"
        "      1. b.2.1\n"
        "- dash\n"
        "  * star\n"
        "    + plus\n"
        "    1. under a bullet\n");
    const QList<ListItemSpan> items = Parser().parseAll(text).decorations.listItems;
    ASSERT_EQ(items.size(), 9);
    const auto at = [&](int i) { return items.at(i); };
    EXPECT_EQ(at(0).shownNumber, 1);
    EXPECT_EQ(at(1).shownNumber, 2); // typed "1.", counts as the second
    EXPECT_EQ(at(2).indent, 1);
    EXPECT_EQ(at(3).shownNumber, 2);
    EXPECT_EQ(at(3).outline, QStringLiteral("2.2"));
    EXPECT_EQ(at(4).indent, 2); // six spaces: two levels, not three
    EXPECT_EQ(at(4).outline, QStringLiteral("2.2.1"));
    EXPECT_EQ(at(5).bullet, QLatin1Char('-'));
    EXPECT_EQ(at(6).bullet, QLatin1Char('*'));
    EXPECT_EQ(at(7).bullet, QLatin1Char('+'));
    EXPECT_EQ(at(8).outline, QStringLiteral("1")); // a bullet starts the outline afresh
}

TEST(ListTest, ListsInsideAQuoteNestToo)
{
    const QString text = QStringLiteral("> 1. one\n> 2. two\n>    1. nested\n> - dash\n>   - dash nested\n");
    const QList<ListItemSpan> items = Parser().parseAll(text).decorations.listItems;
    ASSERT_EQ(items.size(), 5);
    EXPECT_EQ(items.at(2).indent, 1);
    EXPECT_EQ(items.at(4).indent, 1);
    EXPECT_EQ(text.mid(items.at(2).marker.start, items.at(2).marker.length()), QStringLiteral("   1. ")); // after the ">", as a list line starts at its indent
    EXPECT_TRUE(items.at(1).range.contains(items.at(2).range.start)) << items.at(1).range.start << "-" << items.at(1).range.end << " vs " << items.at(2).range.start;
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

TEST(DecorationTest, InlineMathHidesEverythingAfterTheOpeningDollar)
{
    // Only the opening "$" stays in the display string; the editor turns it
    // into the anchor a rendered formula image is drawn over.
    EXPECT_EQ(rendered(QStringLiteral("a $x^2$ b\n")), QStringLiteral("a $ b\n"));
    // Display maths is unaffected: it still hides each delimiter on its own,
    // so a renderer that cannot draw it falls back to showing the bare
    // formula rather than an empty block.
    EXPECT_EQ(rendered(QStringLiteral("$$x^2$$\n")), QStringLiteral("x^2\n"));

    const Decorations decorations = decorationsOf(QStringLiteral("a $x^2$ b\n"));
    ASSERT_EQ(decorations.maths.size(), 1);
    const MathSpan &math = decorations.maths.first();
    EXPECT_FALSE(math.display);

    const bool hidesFormulaThroughClosingDollar =
        std::any_of(decorations.syntax.cbegin(), decorations.syntax.cend(),
                    [&](const Range &range) {
                        return range.start == math.formula.start && range.end == math.range.end;
                    });
    EXPECT_TRUE(hidesFormulaThroughClosingDollar);
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

TEST(CodeOptionTest, NotebookKeysOutputIncludeCacheTimeoutAndKernel)
{
    const auto optionsOf = [](const QString &lines) {
        const Decorations decorations = decorationsOf(QStringLiteral("```python\n") + lines + QStringLiteral("x = 1\n```\n"));
        return decorations.codeOptions.isEmpty() ? CodeOptions() : decorations.codeOptions.first();
    };
    CodeOptions options = optionsOf(QString());
    EXPECT_TRUE(options.output);
    EXPECT_TRUE(options.cache);
    EXPECT_EQ(options.timeoutSeconds, 0);
    EXPECT_TRUE(options.kernel.isEmpty());

    options = optionsOf(QStringLiteral("#| output: false\n#| cache: no\n#| timeout: 12\n#| kernel: true\n"));
    EXPECT_FALSE(options.output);
    EXPECT_FALSE(options.cache);
    EXPECT_EQ(options.timeoutSeconds, 12);
    EXPECT_EQ(options.kernel, QStringLiteral("on"));
    EXPECT_TRUE(options.echo);

    // Quarto's include: false is echo and output together.
    options = optionsOf(QStringLiteral("#| include: false\n"));
    EXPECT_FALSE(options.echo);
    EXPECT_FALSE(options.output);

    options = optionsOf(QStringLiteral("#| kernel: false\n"));
    EXPECT_EQ(options.kernel, QStringLiteral("off"));
    options = optionsOf(QStringLiteral("#| kernel: python3\n"));
    EXPECT_EQ(options.kernel, QStringLiteral("python3"));
    // Nonsense falls to the default, not to an error.
    options = optionsOf(QStringLiteral("#| timeout: soon\n"));
    EXPECT_EQ(options.timeoutSeconds, 0);
}

TEST(CodeOptionTest, SandboxWasmIsItsOwnTierNotHost)
{
    const auto optionsOf = [](const QString &line) {
        const Decorations decorations = decorationsOf(QStringLiteral("```python\n") + line + QStringLiteral("print(1)\n```\n"));
        EXPECT_EQ(decorations.codeOptions.size(), 1);
        return decorations.codeOptions.value(0);
    };
    CodeOptions options = optionsOf(QStringLiteral("#| sandbox: wasm\n"));
    EXPECT_TRUE(options.sandboxWasm);
    EXPECT_FALSE(options.sandboxHost);
    EXPECT_TRUE(optionsOf(QStringLiteral("#| sandbox: WASM\n")).sandboxWasm);
    options = optionsOf(QStringLiteral("#| sandbox: host\n"));
    EXPECT_TRUE(options.sandboxHost);
    EXPECT_FALSE(options.sandboxWasm);
    options = optionsOf(QStringLiteral("#| sandbox: bubblewrap\n"));
    EXPECT_FALSE(options.sandboxHost);
    EXPECT_FALSE(options.sandboxWasm);
}

TEST(CodeOptionTest, QuartoExecutionKeysAreParsed)
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
    EXPECT_FALSE(options.eval); // a notebook block that never runs
    EXPECT_TRUE(options.echo);
    EXPECT_FALSE(options.sandboxHost);
    EXPECT_EQ(options.extra.value(QStringLiteral("warning")), QStringLiteral("false"));
    EXPECT_EQ(options.extra.value(QStringLiteral("label")), QStringLiteral("fig-one"));
    // The options themselves render nothing.
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

    // The marker starts at the line, not at the bullet; the level is the
    // nesting, one list deep, however many spaces it took.
    const auto nested = *std::find_if(items.cbegin(), items.cend(),
                                      [](const ListItemSpan &item) { return item.indent > 0; });
    EXPECT_EQ(nested.indent, 1);
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

TEST(ColonFenceTest, AdjacentContainersLeaveAGapForTheBlankLineBetween)
{
    // A container's range must stop at the end of its own closing fence line
    // rather than reaching into the blank line after it, or two stacked
    // containers paint with no gap between their panels.
    const QString text = QStringLiteral("::: a\nA\n:::\n\n::: b\nB\n:::\n");
    const Decorations decorations = decorationsOf(text);
    ASSERT_EQ(decorations.callouts.size(), 2);

    const CalloutSpan *first = &decorations.callouts.first();
    const CalloutSpan *second = &decorations.callouts.last();
    if (first->range.start > second->range.start)
        std::swap(first, second);

    EXPECT_EQ(sliceOf(text, first->range), QStringLiteral("::: a\nA\n:::\n"));
    EXPECT_EQ(sliceOf(text, second->range), QStringLiteral("::: b\nB\n:::\n"));
    // The blank line sits strictly between the two - neither range touches it.
    EXPECT_LT(first->range.end, second->range.start);
}

TEST(LinkTest, HeadingAndBlockAnchorsRecordTheirSourceRanges)
{
    // Where the "#" and the anchor text sit: the editor draws a heading
    // link's "#" as " > " (HeadingLinkTest in test_editor.cpp).
    const auto linkStyle = [](const Decorations &decorations) -> const StyleSpan * {
        for (const StyleSpan &span : decorations.styles) {
            if (span.kind == StyleSpan::Kind::Link)
                return &span;
        }
        return nullptr;
    };

    const QString heading = QStringLiteral("See [[Second note#Details]] please.\n");
    // Kept, not a temporary: the span pointer points into it.
    const Decorations headingDecorations = decorationsOf(heading);
    const StyleSpan *headingLink = linkStyle(headingDecorations);
    ASSERT_NE(headingLink, nullptr);
    EXPECT_EQ(sliceOf(heading, headingLink->anchorHash), QStringLiteral("#"));
    EXPECT_EQ(sliceOf(heading, headingLink->anchorText), QStringLiteral("Details"));

    const QString block = QStringLiteral("See [[Second note#^anchor]] please.\n");
    const Decorations blockDecorations = decorationsOf(block);
    const StyleSpan *blockLink = linkStyle(blockDecorations);
    ASSERT_NE(blockLink, nullptr);
    EXPECT_EQ(sliceOf(block, blockLink->anchorHash), QStringLiteral("#"));
    EXPECT_EQ(sliceOf(block, blockLink->anchorText), QStringLiteral("^anchor"));

    // No subpath, no anchor.
    const Decorations plainDecorations = decorationsOf(QStringLiteral("See [[Second note]].\n"));
    const StyleSpan *plainLink = linkStyle(plainDecorations);
    ASSERT_NE(plainLink, nullptr);
    EXPECT_FALSE(plainLink->anchorHash.isValid());
    EXPECT_FALSE(plainLink->anchorText.isValid());
}

TEST(ParserPerfTest, ParsingStaysLinearInFileSize)
{
    // The guard on a specific quadratic, and it belongs here rather than in
    // the editor's tests: ObsidianSyntax::scan ran the lazy `%%...%%` pattern
    // once per unmasked region, and a run with no match has to walk to the end
    // of the text before it can say so. A 1 MB note has some 16 000 regions,
    // which made parsing it take 1 992 ms instead of 142 ms.
    //
    // Asserted as a ratio, not a wall-clock number: five times the text should
    // cost roughly five times the work, and a quadratic cannot fit inside that
    // however fast the machine is.
    const QString chunk = QStringLiteral(
        "## Heading\n\nA paragraph with **bold**, *italic*, `code`, a [[wikilink]] and a "
        "[link](https://example.org), long enough to wrap.\n\n- one\n- two\n\n");
    QString small;
    while (small.size() < 200 * 1024)
        small += chunk;
    QString large;
    while (large.size() < 1000 * 1024)
        large += chunk;

    Parser parser;
    // One untimed pass, so neither measurement pays for a cold regex cache.
    parser.parseAll(chunk, QStringLiteral("/vault/Note.md"));

    SKIP_WALL_CLOCK_GUARD_UNDER_SANITIZER();
    // Best of three for each: load only ever adds time to either side.
    const auto bestParseMs = [&](const QString &text) {
        return perf::bestOf([&] {
            QElapsedTimer clock;
            clock.start();
            parser.parseAll(text, QStringLiteral("/vault/Note.md"));
            return clock.nsecsElapsed() / 1e6;
        });
    };
    const double smallMs = bestParseMs(small);
    const double largeMs = bestParseMs(large);

    const double sizeRatio = double(large.size()) / double(small.size());
    const double timeRatio = largeMs / std::max(0.001, smallMs);
    std::printf("[ PERF     ] parse %.0f kB: %.1f ms, %.0f kB: %.1f ms (%.1fx text, %.1fx time)\n",
                small.size() / 1024.0, smallMs, large.size() / 1024.0, largeMs,
                sizeRatio, timeRatio);

    // Linear is ~5x. The quadratic this replaced was over 30x.
    EXPECT_LT(timeRatio, sizeRatio * 2.0);
}

// --- code fence attributes and tab groups (TODO.md, "Code blocks use the
// callout style format" and "Code block tabs") -----------------------------

TEST(CodeFenceTest, AttributesParsedFromTheInfoString)
{
    const Decorations decorations = decorationsOf(QStringLiteral(
        "```python name=\"Setup script\" icon=U+E73C tab\nprint(1)\n```\n"));
    ASSERT_EQ(decorations.codeFences.size(), 1);
    const CodeFenceSpan &fence = decorations.codeFences.first();

    EXPECT_EQ(fence.language, QStringLiteral("python"));
    EXPECT_EQ(fence.attributes.value(QStringLiteral("name")), QStringLiteral("Setup script"));
    EXPECT_EQ(fence.attributes.value(QStringLiteral("icon")), QStringLiteral("U+E73C"));
    // A bare word is a flag with an empty value, not a type the way a
    // callout's first bare word would be - a fence has no type word to spend.
    ASSERT_TRUE(fence.attributes.contains(QStringLiteral("tab")));
    EXPECT_TRUE(fence.attributes.value(QStringLiteral("tab")).isEmpty());

    // A single-quoted value works the same as a double-quoted one.
    const Decorations single = decorationsOf(
        QStringLiteral("```python name='Setup script'\nprint(1)\n```\n"));
    ASSERT_EQ(single.codeFences.size(), 1);
    EXPECT_EQ(single.codeFences.first().attributes.value(QStringLiteral("name")),
              QStringLiteral("Setup script"));
}

TEST(CodeFenceTest, BraceFormIsAccepted)
{
    const Decorations decorations = decorationsOf(
        QStringLiteral("```python {name=\"Setup\" icon=zap}\nprint(1)\n```\n"));
    ASSERT_EQ(decorations.codeFences.size(), 1);
    const CodeFenceSpan &fence = decorations.codeFences.first();
    EXPECT_EQ(fence.language, QStringLiteral("python"));
    EXPECT_EQ(fence.attributes.value(QStringLiteral("name")), QStringLiteral("Setup"));
    EXPECT_EQ(fence.attributes.value(QStringLiteral("icon")), QStringLiteral("zap"));
}

TEST(CodeFenceTest, AttributesRangeIsExactAndHidden)
{
    const QString text = QStringLiteral(
        "```python name=\"Setup script\" icon=U+E73C\nprint(1)\n```\n");
    const Decorations decorations = decorationsOf(text);
    ASSERT_EQ(decorations.codeFences.size(), 1);
    const CodeFenceSpan &fence = decorations.codeFences.first();

    // Exactly the text after the language, nothing of the language itself and
    // nothing of the listing that follows.
    EXPECT_EQ(sliceOf(text, fence.attributesRange),
              QStringLiteral(" name=\"Setup script\" icon=U+E73C"));
    EXPECT_EQ(rendered(text), QStringLiteral("\nprint(1)\n\n"));
}

TEST(CodeFenceTest, LanguageStaysExactlyWhatTheStyleSpanCarries)
{
    const Decorations decorations = decorationsOf(
        QStringLiteral("```Python name=\"Setup\"\nprint(1)\n```\n"));
    ASSERT_EQ(decorations.codeFences.size(), 1);
    // Case is kept, and attributes on the fence do not disturb the language
    // syntax highlighting already keys off.
    EXPECT_EQ(decorations.codeFences.first().language, QStringLiteral("Python"));
    EXPECT_TRUE(hasStyle(decorations, StyleSpan::Kind::CodeBlock, QStringLiteral("Python")));
}

TEST(CodeFenceTest, NoAttributesIsAnEmptyMapAndAnInvalidRange)
{
    const Decorations decorations = decorationsOf(QStringLiteral("```python\nprint(1)\n```\n"));
    ASSERT_EQ(decorations.codeFences.size(), 1);
    const CodeFenceSpan &fence = decorations.codeFences.first();
    EXPECT_TRUE(fence.attributes.isEmpty());
    EXPECT_FALSE(fence.attributesRange.isValid());
}

TEST(CodeFenceTest, TwoAdjacentTabFencesShareAGroupAndThreeDoToo)
{
    const Decorations two = decorationsOf(QStringLiteral(
        "```python tab=\"Python\"\nprint(1)\n```\n```cpp tab=\"C++\"\nint x;\n```\n"));
    ASSERT_EQ(two.codeFences.size(), 2);
    EXPECT_EQ(two.codeFences.at(0).tabGroup, 0);
    EXPECT_EQ(two.codeFences.at(1).tabGroup, 0);
    EXPECT_EQ(two.codeFences.at(0).tabLabel, QStringLiteral("Python"));
    EXPECT_EQ(two.codeFences.at(1).tabLabel, QStringLiteral("C++"));

    const Decorations three = decorationsOf(QStringLiteral(
        "```a tab\nx\n```\n```b tab\nx\n```\n```c tab\nx\n```\n"));
    ASSERT_EQ(three.codeFences.size(), 3);
    EXPECT_EQ(three.codeFences.at(0).tabGroup, 0);
    EXPECT_EQ(three.codeFences.at(1).tabGroup, 0);
    EXPECT_EQ(three.codeFences.at(2).tabGroup, 0);
}

TEST(CodeFenceTest, TextBetweenEndsATabGroup)
{
    const Decorations decorations = decorationsOf(QStringLiteral(
        "```a tab\nx\n```\nstray text\n```b tab\nx\n```\n"));
    ASSERT_EQ(decorations.codeFences.size(), 2);
    // Real text sits between them, so each is alone rather than sharing a
    // group with the other.
    EXPECT_EQ(decorations.codeFences.at(0).tabGroup, -1);
    EXPECT_EQ(decorations.codeFences.at(1).tabGroup, -1);
}

TEST(CodeFenceTest, FenceWithoutTabBetweenEndsAGroup)
{
    const Decorations decorations = decorationsOf(QStringLiteral(
        "```a tab\nx\n```\n```b\nx\n```\n```c tab\nx\n```\n"));
    ASSERT_EQ(decorations.codeFences.size(), 3);
    EXPECT_EQ(decorations.codeFences.at(0).tabGroup, -1); // alone: "b" has no tab
    EXPECT_EQ(decorations.codeFences.at(1).tabGroup, -1); // "b" itself never groups
    EXPECT_EQ(decorations.codeFences.at(2).tabGroup, -1); // alone on the other side
}

TEST(CodeFenceTest, LoneTabFenceIsNotAGroup)
{
    const Decorations decorations = decorationsOf(QStringLiteral("```python tab\nprint(1)\n```\n"));
    ASSERT_EQ(decorations.codeFences.size(), 1);
    EXPECT_EQ(decorations.codeFences.first().tabGroup, -1);
}

TEST(CodeFenceTest, BlankLinesBetweenAreAllowed)
{
    const Decorations decorations = decorationsOf(QStringLiteral(
        "```a tab\nx\n```\n\n\n```b tab\nx\n```\n"));
    ASSERT_EQ(decorations.codeFences.size(), 2);
    EXPECT_EQ(decorations.codeFences.at(0).tabGroup, 0);
    EXPECT_EQ(decorations.codeFences.at(1).tabGroup, 0);
}

TEST(CodeFenceTest, NestedFencesGetDepthAndNoGroup)
{
    const Decorations decorations = decorationsOf(QStringLiteral(
        "> [!note]\n> ```cpp tab\n> x\n> ```\n> ```rust tab\n> x\n> ```\n"));
    ASSERT_EQ(decorations.codeFences.size(), 2);
    // Grouping only ever looks at depth-0 fences today, even though these two
    // are otherwise adjacent - only whitespace sits between them.
    EXPECT_EQ(decorations.codeFences.at(0).depth, 1);
    EXPECT_EQ(decorations.codeFences.at(1).depth, 1);
    EXPECT_EQ(decorations.codeFences.at(0).tabGroup, -1);
    EXPECT_EQ(decorations.codeFences.at(1).tabGroup, -1);
}

// --- TableFormat -----------------------------------------------------------

TEST(TableFormatTest, BasicAlignment)
{
    const QString input = QStringLiteral(
        "|Name|Age|\n"
        "|---|---|\n"
        "|Al|5|\n"
        "|Bob|30|\n");
    const QString expected = QStringLiteral(
        "| Name | Age |\n"
        "| ---- | --- |\n"
        "| Al   | 5   |\n"
        "| Bob  | 30  |\n");
    EXPECT_EQ(formatTable(input), expected);
}

TEST(TableFormatTest, FourDelimiterAlignmentsPadCorrectly)
{
    const QString input = QStringLiteral(
        "|A|B|C|D|\n"
        "|---|:---|---:|:---:|\n"
        "|1|22|333|4|\n");
    const QString expected = QStringLiteral(
        "| A   | B   |   C |  D  |\n"
        "| --- | :-- | --: | :-: |\n"
        "| 1   | 22  | 333 |  4  |\n");
    EXPECT_EQ(formatTable(input), expected);
}

TEST(TableFormatTest, MissingOuterPipesGetAdded)
{
    const QString input = QStringLiteral(
        "Name | Age\n"
        "--- | ---\n"
        "Al | 5\n");
    const QString expected = QStringLiteral(
        "| Name | Age |\n"
        "| ---- | --- |\n"
        "| Al   | 5   |\n");
    EXPECT_EQ(formatTable(input), expected);
}

TEST(TableFormatTest, ShortRowIsPaddedWithEmptyCells)
{
    const QString input = QStringLiteral(
        "|Name|Age|\n"
        "|---|---|\n"
        "|Al|\n");
    const QString expected = QStringLiteral(
        "| Name | Age |\n"
        "| ---- | --- |\n"
        "| Al   |     |\n");
    EXPECT_EQ(formatTable(input), expected);
}

TEST(TableFormatTest, ExtraCellExtendsColumnCount)
{
    const QString input = QStringLiteral(
        "|A|B|\n"
        "|---|---|\n"
        "|1|2|3|\n");
    const QString formatted = formatTable(input);
    const QStringList lines = formatted.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    ASSERT_EQ(lines.size(), 3);
    // Three columns everywhere now, header included, and every row the same
    // overall width.
    for (const QString &line : lines)
        EXPECT_EQ(line.count(QLatin1Char('|')), 4);
    EXPECT_EQ(lines.at(0).size(), lines.at(1).size());
    EXPECT_EQ(lines.at(0).size(), lines.at(2).size());
    EXPECT_TRUE(lines.at(2).contains(QStringLiteral("3")));
}

TEST(TableFormatTest, EscapedPipeStaysInOneCell)
{
    const QString input = QStringLiteral(
        "|A|B|\n"
        "|---|---|\n"
        "|x\\|y|z|\n");
    const QString formatted = formatTable(input);
    const QStringList lines = formatted.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    ASSERT_EQ(lines.size(), 3);
    // Still two columns - the escaped pipe did not split the cell (its own
    // "|" plus the three real separators is 4 pipe characters on the line).
    EXPECT_EQ(lines.at(2).count(QLatin1Char('|')), 4);
    EXPECT_TRUE(lines.at(2).contains(QStringLiteral("x\\|y")));
    EXPECT_TRUE(formatTable(formatted) == formatted);
}

TEST(TableFormatTest, CJKwidthAlignsWithAscii)
{
    // "日本" is "日本", two fullwidth characters - display width 4,
    // same as "abcd", though it is only two QChars where "abcd" is four.
    const QString cjk = QStringLiteral("日本");
    const QString input = QStringLiteral("|H|\n|---|\n|abcd|\n|") + cjk + QStringLiteral("|\n");
    const QString expected = QStringLiteral("| H    |\n| ---- |\n| abcd |\n| ") + cjk
        + QStringLiteral(" |\n");
    EXPECT_EQ(formatTable(input), expected);
}

TEST(TableFormatTest, CombiningMarkIsZeroWidth)
{
    const QString combining = QStringLiteral("ábc"); // "a" + combining acute + "bc"
    const QString input = QStringLiteral("|H|\n|---|\n|abc|\n|") + combining + QStringLiteral("|\n");
    const QString formatted = formatTable(input);
    const QStringList lines = formatted.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    ASSERT_EQ(lines.size(), 4);
    // Both cells display as width 3, so both get the same single margin
    // space either side - no extra padding on "abc" for the combining row's
    // (irrelevant) extra code unit.
    EXPECT_EQ(lines.at(2), QStringLiteral("| abc |"));
    EXPECT_EQ(lines.at(3), QLatin1String("| ") + combining + QLatin1String(" |"));
}

TEST(TableFormatTest, WideAndCombiningCharactersCountByDisplayWidth)
{
    // The column is as wide as "abcd" (4); every other cell is padded by its
    // display width, not its code-unit count: "日本" 4 (no pad), "e" + U+0301 1
    // (3 pad), U+1F600 2 (2 pad), fullwidth U+FF11 2 (2 pad), halfwidth
    // katakana U+FF71 1 (3 pad).
    const QString cjk = QString::fromUcs4(U"\u65E5\u672C");
    const QString combining = QString::fromUcs4(U"e\u0301");
    const QString emoji = QString::fromUcs4(U"\U0001F600");
    const QString fullwidth = QString::fromUcs4(U"\uFF11");
    const QString halfwidth = QString::fromUcs4(U"\uFF71");
    const QString input = QStringLiteral("|H|\n|---|\n|abcd|\n|") + cjk + QStringLiteral("|\n|") + combining
        + QStringLiteral("|\n|") + emoji + QStringLiteral("|\n|") + fullwidth + QStringLiteral("|\n|")
        + halfwidth + QStringLiteral("|\n");
    const QString expected = QStringLiteral("| H    |\n| ---- |\n| abcd |\n| ") + cjk
        + QStringLiteral(" |\n| ") + combining + QStringLiteral("    |\n| ") + emoji
        + QStringLiteral("   |\n| ") + fullwidth + QStringLiteral("   |\n| ") + halfwidth
        + QStringLiteral("    |\n");
    EXPECT_EQ(formatTable(input), expected);
}

TEST(TableFormatTest, BlockquotePrefixIsPreserved)
{
    const QString input = QStringLiteral(
        "> |A|B|\n"
        "> |---|---|\n"
        "> |1|2|\n");
    const QString expected = QStringLiteral(
        "> | A   | B   |\n"
        "> | --- | --- |\n"
        "> | 1   | 2   |\n");
    EXPECT_EQ(formatTable(input), expected);
}

TEST(TableFormatTest, IsIdempotent)
{
    const QStringList inputs = {
        QStringLiteral("|Name|Age|\n|---|---|\n|Al|5|\n|Bob|30|\n"),
        QStringLiteral("Name | Age\n--- | ---\nAl | 5\n"),
        QStringLiteral("|A|B|C|D|\n|---|:---|---:|:---:|\n|1|22|333|4|\n"),
        QStringLiteral("> |A|B|\n> |---|---|\n> |1|2|\n"),
    };
    for (const QString &input : inputs) {
        const QString once = formatTable(input);
        EXPECT_EQ(formatTable(once), once);
    }
}

TEST(TableFormatTest, NotATableIsReturnedUnchanged)
{
    const QString noPipes = QStringLiteral("just text\nsecond line\n");
    EXPECT_EQ(formatTable(noPipes), noPipes);

    const QString badDelimiter = QStringLiteral("|A|B|\n|foo|bar|\n|1|2|\n");
    EXPECT_EQ(formatTable(badDelimiter), badDelimiter);

    const QString oneLine = QStringLiteral("|A|B|\n");
    EXPECT_EQ(formatTable(oneLine), oneLine);
}

TEST(TableFormatTest, TrailingNewlineIsKeptOrNot)
{
    const QString withNewline = QStringLiteral("|A|B|\n|---|---|\n|1|2|\n");
    EXPECT_TRUE(formatTable(withNewline).endsWith(QLatin1Char('\n')));

    const QString withoutNewline = QStringLiteral("|A|B|\n|---|---|\n|1|2|");
    EXPECT_FALSE(formatTable(withoutNewline).endsWith(QLatin1Char('\n')));
}

TEST(TableFormatTest, ParserSeesSameTableBeforeAndAfter)
{
    const QString before = QStringLiteral(
        "| Name | Model |\n"
        "|---|---:|\n"
        "| BME680 Breakout | BME680 (PIM357) |\n");
    const QString after = formatTable(before);
    ASSERT_NE(after, before); // formatting actually changed the padding

    const Decorations beforeDecorations = decorationsOf(before);
    const Decorations afterDecorations = decorationsOf(after);
    ASSERT_EQ(beforeDecorations.tables.size(), 1);
    ASSERT_EQ(afterDecorations.tables.size(), 1);

    const TableSpan &beforeTable = beforeDecorations.tables.first();
    const TableSpan &afterTable = afterDecorations.tables.first();
    EXPECT_EQ(beforeTable.columns, afterTable.columns);
    EXPECT_EQ(beforeTable.alignment, afterTable.alignment);
    ASSERT_EQ(beforeTable.rows.size(), afterTable.rows.size());
    for (int r = 0; r < beforeTable.rows.size(); ++r) {
        ASSERT_EQ(beforeTable.rows[r].size(), afterTable.rows[r].size());
        for (int c = 0; c < beforeTable.rows[r].size(); ++c) {
            EXPECT_EQ(sliceOf(before, beforeTable.rows[r][c]).trimmed(),
                      sliceOf(after, afterTable.rows[r][c]).trimmed());
        }
    }
}

TEST(TableFormatTest, RealWorldTableAlignsAndStaysIdempotent)
{
    // – en dash, ± plus-minus, £ pound sign - written as
    // universal-character-name escapes rather than raw UTF-8 bytes, since a
    // narrow byte literal concatenated into QStringLiteral's u"" literal is
    // reinterpreted one byte per QChar instead of being UTF-8 decoded.
    const QString input = QStringLiteral(
        "| Name | Model | Interface | Supply | Typical power/current | Key spec | Price | "
        "Delivery (UK/EU) | Datasheet | Buy |\n"
        "|---|---|---|---|---|---|---:|---|---|---|\n"
        "| BME680 Breakout | BME680 (PIM357) | I2C, SPI | 1.71–3.6 V IC (breakout "
        "3.3/5 V friendly) | 2.1 uA sleep (typ) | RH accuracy ±3 %RH | £15.75 | "
        "UK £3.90 / EU weight-based | "
        "https://cdn.shopify.com/s/files/1/0174/1800/files/bst-bme680-ds001.pdf?v=1629474140 | "
        "https://shop.pimoroni.com/products/bme680-breakout |\n");

    const QString formatted = formatTable(input);
    EXPECT_EQ(formatTable(formatted), formatted); // idempotent

    const QStringList lines = formatted.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    ASSERT_EQ(lines.size(), 3);
    // Every row padded to the same overall width - the "looks good in plain
    // text" ask.
    EXPECT_EQ(lines.at(0).size(), lines.at(1).size());
    EXPECT_EQ(lines.at(0).size(), lines.at(2).size());

    EXPECT_TRUE(formatted.contains(QStringLiteral("BME680 Breakout")));
    EXPECT_TRUE(formatted.contains(QStringLiteral("shop.pimoroni.com/products/bme680-breakout")));

    // The parser still sees the same cells, just better padded.
    const Decorations before = decorationsOf(input);
    const Decorations after = decorationsOf(formatted);
    ASSERT_EQ(before.tables.size(), 1);
    ASSERT_EQ(after.tables.size(), 1);
    EXPECT_EQ(before.tables.first().columns, after.tables.first().columns);
}

// --- HTML headings and entity references -------------------------------------

TEST(HeadingTest, HTMLHeadingsOfATopLevelBlockAreHeadings)
{
    const QString text = QStringLiteral(
        "# One\n\n<div>\n<h2 id=\"x\">Two <b>bold</b> &amp; &#169;</h2>\n<!-- <h3>hidden</h3> -->\n"
        "<H4>\n  four\n</H4>\n</div>\n\n## Five\n\n> <h5>quoted</h5>\n");
    const Metadata metadata = parse(text);

    ASSERT_EQ(metadata.headings.size(), 4);
    EXPECT_EQ(metadata.headings.at(1).level, 2);
    EXPECT_EQ(metadata.headings.at(1).text, QStringLiteral("Two bold & ©"));
    EXPECT_EQ(sliceOf(text, metadata.headings.at(1).textRange), QStringLiteral("Two <b>bold</b> &amp; &#169;"));
    EXPECT_TRUE(sliceOf(text, metadata.headings.at(1).range).startsWith(QStringLiteral("<h2")));
    EXPECT_EQ(metadata.headings.at(2).level, 4);
    EXPECT_EQ(metadata.headings.at(2).text, QStringLiteral("four"));
    // In document order, so the outline and a section search agree.
    EXPECT_EQ(metadata.headings.at(3).text, QStringLiteral("Five"));
    // A heading inside a quote is not a top-level block: not listed.
    for (const Heading &heading : metadata.headings)
        EXPECT_NE(heading.text, QStringLiteral("quoted"));
}

TEST(EntityTest, ReferencesInTextAreSubstitutions)
{
    const QString text = QStringLiteral(
        "a &amp; b &copy; c &#169; d &#xA9; e\n\n`&amp;` and \\&amp; and [x](http://a/?q=1&amp;r=2) and "
        "[[Note&amp;]] &unknown; &amp\n\n```\n&amp;\n```\n\n<p>&amp;</p>\n\n[AT&amp;T](u) kept &amp;\n");
    const Decorations decorations = Parser().parseAll(text).decorations;
    QStringList seen;
    for (const Substitution &substitution : decorations.substitutions) {
        // Byte-exact: each range is the reference as written.
        EXPECT_TRUE(sliceOf(text, substitution.range).startsWith(QLatin1Char('&')));
        EXPECT_TRUE(sliceOf(text, substitution.range).endsWith(QLatin1Char(';')));
        seen << substitution.text;
    }
    // The text's four, then the last; nothing in code, a fence, HTML, an
    // escape, a link (its text or destination), a wikilink or an unknown name.
    EXPECT_EQ(seen, (QStringList{QStringLiteral("&"), QStringLiteral("©"), QStringLiteral("©"),
                                 QStringLiteral("©"), QStringLiteral("&")}))
        << seen.join(QLatin1Char('|')).toStdString().c_str();
}

TEST(HeadingSectionTest, MarkdownAndHTMLHeadingsBothNameASection)
{
    const QString note = QStringLiteral(
        "---\n# yaml comment\ntitle: x\n---\n"
        "intro\n\n# Top\ntext\n"
        "<h2 class=\"a\">Two &amp; <b>Three</b></h2>\n\nhtml body\n\n"
        "```\n# in a fence\n<h2>Two &amp; Three</h2>\n```\n\n"
        "<!--\n# in a comment\n-->\n"
        "### Deep\nmore\n"
        "<H2>\nMulti\nline\n</H2>\nlast\n"
        "## Md two\nend\n");
    const auto section = [&](const QString &wanted) {
        const Range range = headingSection(note, wanted);
        return range.isValid() ? note.mid(range.start, range.length()) : QStringLiteral("<none>");
    };
    // An HTML heading runs to the next heading at its level or above, here the
    // second <h2>; the fenced and commented ones are not headings.
    const QString html = section(QStringLiteral("two & three"));
    EXPECT_TRUE(html.startsWith(QStringLiteral("<h2 class=\"a\">Two &amp; <b>Three</b></h2>")));
    EXPECT_TRUE(html.contains(QStringLiteral("html body")));
    EXPECT_TRUE(html.contains(QStringLiteral("### Deep")));
    EXPECT_TRUE(html.contains(QStringLiteral("# in a fence"))) << "inside the section, as text";
    EXPECT_FALSE(html.contains(QStringLiteral("<H2>")));
    EXPECT_EQ(section(QStringLiteral("Multi line")), QStringLiteral("<H2>\nMulti\nline\n</H2>\nlast\n"));
    EXPECT_TRUE(section(QStringLiteral("Top")).startsWith(QStringLiteral("# Top\ntext")));
    EXPECT_TRUE(section(QStringLiteral("Top")).endsWith(QStringLiteral("end\n"))) << "h1 holds every lower level";
    EXPECT_EQ(section(QStringLiteral("md two")), QStringLiteral("## Md two\nend\n"));
    EXPECT_EQ(section(QStringLiteral("yaml comment")), QStringLiteral("<none>"));
    EXPECT_EQ(section(QStringLiteral("in a fence")), QStringLiteral("<none>"));
    EXPECT_EQ(section(QStringLiteral("in a comment")), QStringLiteral("<none>"));
    EXPECT_EQ(section(QStringLiteral("Missing")), QStringLiteral("<none>"));
}

TEST(WordsInTest, CountsRunsOfLettersDigitsAndApostrophes)
{
    EXPECT_EQ(wordsIn(u"one two, don't  3rd_x"), 4);
    EXPECT_EQ(wordsIn(u""), 0);
}

// --- meta notes --------------------------------------------------------------

namespace {

/// The text of every meta note in `text`, with `open`/`close` as delimiters.
QStringList metaNotesOf(const QString &text, const QString &open = QStringLiteral("["),
                        const QString &close = QStringLiteral("]"))
{
    static Parser parser;
    ParseOptions options;
    options.metaNotes = true;
    options.open = open;
    options.close = close;
    QStringList out;
    for (const Range &range : parser.parseAll(text, {}, options).decorations.metaNotes)
        out << sliceOf(text, range);
    return out;
}

} // namespace

TEST(MetaNoteTest, ABracketedRunInProseIsOne)
{
    EXPECT_EQ(metaNotesOf(QStringLiteral("She left early [check the date] and never came back.")),
              QStringList{QStringLiteral("[check the date]")});
    EXPECT_EQ(metaNotesOf(QStringLiteral("[a] and [b]")), (QStringList{"[a]", "[b]"}));
}

TEST(MetaNoteTest, AlsoGivesAStyleSpanPerRun)
{
    const QString text = QStringLiteral("x [note] y");
    ParseOptions options;
    options.metaNotes = true;
    const auto styles = Parser().parseAll(text, {}, options).decorations.styles;
    const auto it = std::find_if(styles.cbegin(), styles.cend(),
                                 [](const StyleSpan &s) { return s.kind == StyleSpan::Kind::MetaNote; });
    ASSERT_NE(it, styles.cend());
    EXPECT_EQ(sliceOf(text, it->range), QStringLiteral("[note]"));
}

TEST(MetaNoteTest, OffUnlessAskedFor)
{
    const QString text = QStringLiteral("x [note] y");
    EXPECT_TRUE(Parser().parseAll(text).decorations.metaNotes.isEmpty());
    EXPECT_TRUE(metaNotesOf(text, QStringLiteral("{"), QStringLiteral("}")).isEmpty());
}

TEST(MetaNoteTest, MetadataCarriesTheRunsWhenAskedFor)
{
    // The index stores the runs from the metadata, so the meta: search can
    // find them without parsing every note again.
    const QString text = QStringLiteral("She left early [check the date] and never came back.");
    ParseOptions options;
    options.metaNotes = true;
    const QList<Range> runs = Parser().parse(text, {}, options).metaNotes;
    ASSERT_EQ(runs.size(), 1);
    EXPECT_EQ(sliceOf(text, runs.first()), QStringLiteral("[check the date]"));
    EXPECT_TRUE(Parser().parse(text).metaNotes.isEmpty()) << "off unless asked for";
}

TEST(MetaNoteTest, AnUnclosedLabelDoesNotSwallowTheRestOfTheNote)
{
    // `[a][` with no `]` on its line is no reference link: `[a]` is a run, and
    // the label search does not run on to a `]` paragraphs later.
    EXPECT_EQ(metaNotesOf(QStringLiteral("see [a][ here\n\nand later [real note] too.\n")),
              (QStringList{QStringLiteral("[a]"), QStringLiteral("[real note]")}));
    EXPECT_EQ(metaNotesOf(QStringLiteral("a [text][ref] link and [note]\n")), QStringList{QStringLiteral("[note]")});
}

TEST(MetaNoteTest, NestedBracketsAreOneOutermostRun)
{
    EXPECT_EQ(metaNotesOf(QStringLiteral("a [b [c] d] e")), QStringList{QStringLiteral("[b [c] d]")});
}

TEST(MetaNoteTest, AnUnclosedOpenerIsNotARun)
{
    EXPECT_TRUE(metaNotesOf(QStringLiteral("a [b c")).isEmpty());
    EXPECT_TRUE(metaNotesOf(QStringLiteral("a b] c")).isEmpty());
    // The closed pair inside an unclosed one still counts.
    EXPECT_EQ(metaNotesOf(QStringLiteral("a [b [c] d")), QStringList{QStringLiteral("[c]")});
}

TEST(MetaNoteTest, LinksAreNotNotes)
{
    EXPECT_TRUE(metaNotesOf(QStringLiteral("a [[wikilink]] b")).isEmpty());
    EXPECT_TRUE(metaNotesOf(QStringLiteral("a [[wiki|alias]] b")).isEmpty());
    EXPECT_TRUE(metaNotesOf(QStringLiteral("a ![[embed.png]] b")).isEmpty());
    EXPECT_TRUE(metaNotesOf(QStringLiteral("a [text](https://example.com) b")).isEmpty());
    EXPECT_TRUE(metaNotesOf(QStringLiteral("a ![alt](img.png) b")).isEmpty());
    EXPECT_TRUE(metaNotesOf(QStringLiteral("a [text][ref] b\n\n[ref]: https://example.com")).isEmpty());
    EXPECT_TRUE(metaNotesOf(QStringLiteral("a [text][] b\n\n[text]: https://example.com")).isEmpty());
    EXPECT_TRUE(metaNotesOf(QStringLiteral("a [text][nodef] b")).isEmpty()) << "an undefined label is not a note either";
    EXPECT_TRUE(metaNotesOf(QStringLiteral("a [text] b\n\n[text]: https://example.com")).isEmpty())
        << "a shortcut that matches a definition is a link";
    EXPECT_EQ(metaNotesOf(QStringLiteral("a [text] b\n\n[other]: https://example.com")),
              QStringList{QStringLiteral("[text]")})
        << "no definition for it: plain text";
}

TEST(MetaNoteTest, LinkDefinitionsAreNotNotes)
{
    EXPECT_TRUE(metaNotesOf(QStringLiteral("x\n\n[ref]: https://example.com \"Title [t]\"\n")).isEmpty());
}

TEST(MetaNoteTest, FootnotesTasksAndCalloutsAreNotNotes)
{
    EXPECT_TRUE(metaNotesOf(QStringLiteral("a[^1] b\n\n[^1]: the note\n")).isEmpty());
    EXPECT_TRUE(metaNotesOf(QStringLiteral("a ^[inline note] b")).isEmpty());
    EXPECT_TRUE(metaNotesOf(QStringLiteral("- [ ] open\n- [x] done\n1. [X] numbered\n  * [ ] nested\n")).isEmpty());
    EXPECT_TRUE(metaNotesOf(QStringLiteral("> [!note] Title\n> body\n")).isEmpty());
    EXPECT_TRUE(metaNotesOf(QStringLiteral("> [!warning]- Folded\n> body\n")).isEmpty());
    // But a box mid-sentence, or a note in a task's text, is one.
    EXPECT_EQ(metaNotesOf(QStringLiteral("- [ ] write [chapter 2] today\n")), QStringList{QStringLiteral("[chapter 2]")});
}

TEST(MetaNoteTest, EscapedBracketsDoNotOpenOrClose)
{
    EXPECT_TRUE(metaNotesOf(QStringLiteral("a \\[not a note] b")).isEmpty());
    EXPECT_TRUE(metaNotesOf(QStringLiteral("a [not a note\\] b")).isEmpty());
    EXPECT_EQ(metaNotesOf(QStringLiteral("a [x \\] y] b")), QStringList{QStringLiteral("[x \\] y]")});
}

TEST(MetaNoteTest, CodeMathHtmlFrontmatterAndURLsAreOpaque)
{
    EXPECT_TRUE(metaNotesOf(QStringLiteral("a `[code]` b")).isEmpty());
    EXPECT_TRUE(metaNotesOf(QStringLiteral("```\n[block]\n```\n")).isEmpty());
    EXPECT_TRUE(metaNotesOf(QStringLiteral("a $[x]$ b\n\n$$\n[y]\n$$\n")).isEmpty());
    EXPECT_TRUE(metaNotesOf(QStringLiteral("a <span title=\"[t]\">x</span> b")).isEmpty());
    EXPECT_TRUE(metaNotesOf(QStringLiteral("<div>\n[in html]\n</div>\n")).isEmpty());
    EXPECT_TRUE(metaNotesOf(QStringLiteral("---\ntitle: [a, b]\ntags: [x]\n---\n\nbody\n")).isEmpty());
    EXPECT_TRUE(metaNotesOf(QStringLiteral("see https://example.com/a[1] now")).isEmpty());
    EXPECT_TRUE(metaNotesOf(QStringLiteral("a %%[hidden]%% b")).isEmpty());
    // A closing bracket inside code does not end a run.
    EXPECT_EQ(metaNotesOf(QStringLiteral("a [see `x]` here] b")), QStringList{QStringLiteral("[see `x]` here]")});
}

TEST(MetaNoteTest, ARunMayCrossASoftBreakButNotABlock)
{
    EXPECT_EQ(metaNotesOf(QStringLiteral("a [one\ntwo] b")), QStringList{QStringLiteral("[one\ntwo]")});
    EXPECT_TRUE(metaNotesOf(QStringLiteral("a [one\n\ntwo] b")).isEmpty()) << "a blank line ends the paragraph";
    EXPECT_TRUE(metaNotesOf(QStringLiteral("a [one\n   \ntwo] b")).isEmpty());
    EXPECT_TRUE(metaNotesOf(QStringLiteral("a [one\n# Heading\ntwo] b")).isEmpty());
    EXPECT_TRUE(metaNotesOf(QStringLiteral("- a [one\n- two] b")).isEmpty()) << "the next item is another block";
}

TEST(MetaNoteTest, ATableCellHoldsItsOwnRun)
{
    const QString table = QStringLiteral("| A | B |\n|---|---|\n| x [one] y | [two] |\n| [a | b] | c |\n");
    EXPECT_EQ(metaNotesOf(table), (QStringList{"[one]", "[two]"})) << "a run does not cross a cell";
}

TEST(MetaNoteTest, EmphasisAndHeadingsKeepTheirRuns)
{
    EXPECT_EQ(metaNotesOf(QStringLiteral("**bold [note] text**")), QStringList{QStringLiteral("[note]")});
    EXPECT_EQ(metaNotesOf(QStringLiteral("# Title [todo]\n")), QStringList{QStringLiteral("[todo]")});
    EXPECT_TRUE(metaNotesOf(QStringLiteral("a [] b and [ ] c")).isEmpty()) << "an empty pair says nothing";
}

TEST(MetaNoteTest, AnyPairOfDelimitersWorks)
{
    EXPECT_EQ(metaNotesOf(QStringLiteral("a {the date} b {c {d} e} f"), QStringLiteral("{"), QStringLiteral("}")),
              (QStringList{"{the date}", "{c {d} e}"}));
    EXPECT_EQ(metaNotesOf(QStringLiteral("a << fix this >> b [kept] c"), QStringLiteral("<<"), QStringLiteral(">>")),
              QStringList{QStringLiteral("<< fix this >>")});
    // The brackets are ordinary text then, and so are wikilinks' - never a note.
    EXPECT_TRUE(metaNotesOf(QStringLiteral("a [[link]] b [c] d"), QStringLiteral("{"), QStringLiteral("}")).isEmpty());
    EXPECT_EQ(metaNotesOf(QStringLiteral("a ( aside ) b"), QStringLiteral("("), QStringLiteral(")")),
              QStringList{QStringLiteral("( aside )")});
}

TEST(MetaNoteTest, IdenticalDelimitersPairLeftToRight)
{
    EXPECT_EQ(metaNotesOf(QStringLiteral("a ;; one ;; b ;; two ;; c ;; open"), QStringLiteral(";;"), QStringLiteral(";;")),
              (QStringList{";; one ;;", ";; two ;;"}));
    EXPECT_EQ(metaNotesOf(QStringLiteral("a !x! b !y! c"), QStringLiteral("!"), QStringLiteral("!")),
              (QStringList{"!x!", "!y!"}));
}

TEST(MetaNoteTest, CustomDelimitersStillSkipCodeAndLinks)
{
    EXPECT_TRUE(metaNotesOf(QStringLiteral("a `{x}` b <https://e.com/{y}> c"), QStringLiteral("{"), QStringLiteral("}")).isEmpty());
    EXPECT_TRUE(metaNotesOf(QStringLiteral("a [t](https://e.com/{y}) c"), QStringLiteral("{"), QStringLiteral("}")).isEmpty());
    EXPECT_TRUE(metaNotesOf(QStringLiteral("---\nk: {a: b}\n---\n"), QStringLiteral("{"), QStringLiteral("}")).isEmpty());
}

TEST(MetaNoteTest, MarkdownSyntaxCannotBeADelimiter)
{
    EXPECT_TRUE(metaNoteDelimitersError(QStringLiteral("["), QStringLiteral("]")).isEmpty());
    for (const char *bad : {"*", "_", "~", "=", "`", "$", "%", "#", "|", "**", "a|", "\\"})
        EXPECT_FALSE(metaNoteDelimitersError(QString::fromLatin1(bad), QStringLiteral("]")).isEmpty()) << bad;
    EXPECT_FALSE(metaNoteDelimitersError(QStringLiteral("["), QStringLiteral("=")).isEmpty()) << "either side";
    EXPECT_FALSE(metaNoteDelimitersError(QString(), QStringLiteral("]")).isEmpty());
    EXPECT_FALSE(metaNoteDelimitersError(QStringLiteral("<<<<"), QStringLiteral(">>")).isEmpty()) << "four characters";
    EXPECT_FALSE(metaNoteDelimitersError(QStringLiteral("a b"), QStringLiteral("]")).isEmpty()) << "a space";
    EXPECT_TRUE(metaNoteDelimitersError(QStringLiteral("<<"), QStringLiteral(">>")).isEmpty());
    EXPECT_TRUE(metaNoteDelimitersError(QStringLiteral(";;"), QStringLiteral(";;")).isEmpty());
    // The parser takes it as no notes at all rather than guessing.
    EXPECT_TRUE(metaNotesOf(QStringLiteral("a *b* c"), QStringLiteral("*"), QStringLiteral("*")).isEmpty());
}

namespace {

/// Source of every style span of `kind`.
QStringList spansOf(const QString &text, const ParseOptions &options, StyleSpan::Kind kind)
{
    QStringList out;
    for (const StyleSpan &span : Parser().parseAll(text, {}, options).decorations.styles) {
        if (span.kind == kind)
            out << sliceOf(text, span.range);
    }
    return out;
}

} // namespace

TEST(BareBracketTest, ABareBracketIsStyledButALinkIsNot)
{
    ParseOptions options;
    options.brackets = true;
    const auto bare = [&](const QString &text) { return spansOf(text, options, StyleSpan::Kind::BareBracket); };
    EXPECT_EQ(bare(QStringLiteral("She left [check the date] early")), QStringList{QStringLiteral("[check the date]")});
    EXPECT_EQ(bare(QStringLiteral("a [b [c] d] e")), QStringList{QStringLiteral("[b [c] d]")});
    EXPECT_TRUE(bare(QStringLiteral("[[wiki]] ![[embed.png]] [t](u) ![a](b.png) [a][r] [^1] - [ ] t\n\n[r]: http://e.com\n[^1]: n")).isEmpty());
    EXPECT_TRUE(bare(QStringLiteral("a [text] b\n\n[text]: https://example.com")).isEmpty()) << "a resolved shortcut is a link";
    EXPECT_TRUE(bare(QStringLiteral("`[code]` $[m]$ <b title=\"[t]\">x</b> \\[no]")).isEmpty());
    EXPECT_TRUE(bare(QStringLiteral("> [!note] Title\n> body")).isEmpty());
}

TEST(BareBracketTest, NotLookedForUnlessAsked)
{
    EXPECT_TRUE(spansOf(QStringLiteral("a [note] b"), ParseOptions{}, StyleSpan::Kind::BareBracket).isEmpty());
}

TEST(BareBracketTest, AMetaNoteOnTheSameBracketsReplacesIt)
{
    const QString text = QStringLiteral("a [note] b");
    ParseOptions options;
    options.brackets = true;
    options.metaNotes = true;
    EXPECT_EQ(spansOf(text, options, StyleSpan::Kind::MetaNote), QStringList{QStringLiteral("[note]")});
    EXPECT_TRUE(spansOf(text, options, StyleSpan::Kind::BareBracket).isEmpty()) << "the meta colour wins";
    options.open = QStringLiteral("{");
    options.close = QStringLiteral("}");
    EXPECT_EQ(spansOf(text, options, StyleSpan::Kind::BareBracket), QStringList{QStringLiteral("[note]")});
    EXPECT_TRUE(spansOf(text, options, StyleSpan::Kind::MetaNote).isEmpty());
}

namespace {

int wordCountOf(const QString &text, const ParseOptions &options = {})
{
    return Parser().parseAll(text, {}, options).metadata.wordCount;
}

} // namespace

TEST(WordCountTest, ExcludesWhatIsBetweenTheDelimiters)
{
    const QString text = QStringLiteral("one two [three four] five {six seven} [[a wikilink]] [a link](u v)");
    const int all = wordCountOf(text);
    ParseOptions options;
    options.excludeBetween = true;
    // [three four] goes; a wikilink and a link's text and target are not "between brackets".
    EXPECT_EQ(wordCountOf(text, options), all - 2);
    options.excludeOpen = QStringLiteral("{");
    options.excludeClose = QStringLiteral("}");
    EXPECT_EQ(wordCountOf(text, options), all - 2);
    options.excludeBetween = false;
    EXPECT_EQ(wordCountOf(text, options), all);
}

TEST(WordCountTest, ExclusionAlsoListsTheCharactersLeftOut)
{
    const QString text = QStringLiteral("one [two] three");
    ParseOptions options;
    options.excludeBetween = true;
    const auto uncounted = Parser().parseAll(text, {}, options).decorations.uncounted;
    ASSERT_EQ(uncounted.size(), 1);
    EXPECT_EQ(sliceOf(text, uncounted.first()), QStringLiteral("[two]"));
    EXPECT_TRUE(Parser().parseAll(text).decorations.uncounted.isEmpty());
}

TEST(WordCountTest, IgnoreFormattingCountsRenderedWordsOnly)
{
    const QString text = QStringLiteral(
        "# Title\n\n"
        "1. first **bold** item\n"
        "2. [a link](https://example.com/some/path) and [[Other Note|alias]]\n\n"
        "<div class=\"box\">html text</div>\n\n"
        "visible %%hidden comment words%% end\n");
    const int plain = wordCountOf(text);
    ParseOptions options;
    options.ignoreFormatting = true;
    // Title, first, bold, item, a, link, and, alias, html, text, visible, end
    EXPECT_EQ(wordCountOf(text, options), 12);
    EXPECT_GT(plain, 12);
}

TEST(WordCountTest, ExclusionAndFormattingCompose)
{
    ParseOptions options;
    options.ignoreFormatting = true;
    options.excludeBetween = true;
    EXPECT_EQ(wordCountOf(QStringLiteral("- one [skip this] **two**\n"), options), 2);
}
