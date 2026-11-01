// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej
// ```markmap fences, the pure part: libs/render/Markmap.cpp's tree (headings, lists, inline Markdown, the fold
// comment, the options, the cap) and the SVG it draws (well-formed, folding, colours, wrapping). The editor's
// renderer and the window's core plugin are test_markmap_block.cpp's.

#include <gtest/gtest.h>

#include <iostream>

#include <QApplication>
#include <QElapsedTimer>
#include <QRegularExpression>
#include <QSvgRenderer>
#include <QTemporaryDir>
#include <QXmlStreamReader>

#include "Markmap.h"
#include "MermaidCache.h"
#include "Theme.h"

using noteAppC::MarkmapNode;
using noteAppC::MarkmapTree;
using noteAppC::Theme;

namespace {

/// The texts of the nodes below `node`, as "a(b,c(d))": the tree in one line.
QString shape(const MarkmapTree &tree, int node = 0)
{
    const MarkmapNode &n = tree.nodes.at(node);
    QString out = n.plain();
    if (!n.children.isEmpty()) {
        QStringList kids;
        for (const int child : n.children)
            kids << shape(tree, child);
        out += QLatin1Char('(') + kids.join(QLatin1Char(',')) + QLatin1Char(')');
    }
    return out;
}

noteAppC::MarkmapLook lookOf(Theme::Mode mode = Theme::Mode::Dark)
{
    return noteAppC::markmapLook(noteAppC::diagramConfig(noteAppC::DiagramKind::Markmap, Theme::builtin(mode)));
}

bool wellFormed(const QByteArray &svg, QString *why = nullptr)
{
    QXmlStreamReader xml(svg);
    while (!xml.atEnd())
        xml.readNext();
    if (xml.hasError() && why)
        *why = xml.errorString();
    return !xml.hasError();
}

} // namespace

// --- the tree ----------------------------------------------------------------

TEST(MarkmapTreeTest, HeadingsNestByLevelAndListsByIndentationUnderTheirHeading)
{
    const MarkmapTree tree = noteAppC::parseMarkmap(QStringLiteral(
        "# Root\n## A\n- a1\n  - a2\n- a3\n## B\n1. b1\n2. b2\n### B deeper\n"));
    EXPECT_FALSE(tree.virtualRoot);
    EXPECT_EQ(shape(tree), QStringLiteral("Root(A(a1(a2),a3),B(b1,b2,B deeper))"));
    EXPECT_EQ(tree.nodes.at(0).depth, 0);
    EXPECT_EQ(tree.nodes.at(0).parent, -1);
}

TEST(MarkmapTreeTest, ASkippedHeadingLevelStillNestsUnderTheNearestLowerOne)
{
    const MarkmapTree tree = noteAppC::parseMarkmap(QStringLiteral("# R\n### deep\n## shallow\n#### deepest\n"));
    EXPECT_EQ(shape(tree), QStringLiteral("R(deep,shallow(deepest))"));
}

// What the note shows in place of source (a backslash escape, an entity) is what a map node shows too.
TEST(MarkmapTreeTest, EscapesAndEntitiesShowAsTheNoteShowsThem)
{
    const MarkmapTree tree = noteAppC::parseMarkmap(QStringLiteral("# R\n- \\*not bold\\* &amp; **bold**\n"));
    ASSERT_EQ(tree.nodes.size(), 2);
    EXPECT_EQ(tree.nodes.at(1).plain(), QStringLiteral("*not bold* & bold"));
}

TEST(MarkmapTreeTest, SeveralTopLevelItemsGetAnEmptyRootLikeMarkmapJs)
{
    const MarkmapTree tree = noteAppC::parseMarkmap(QStringLiteral("- one\n- two\n  - three\n"));
    EXPECT_TRUE(tree.virtualRoot);
    EXPECT_TRUE(tree.nodes.at(0).runs.isEmpty());
    EXPECT_EQ(shape(tree), QStringLiteral("(one,two(three))"));
}

TEST(MarkmapTreeTest, AnEmptyFenceIsAnEmptyRootNotACrash)
{
    const MarkmapTree tree = noteAppC::parseMarkmap(QString());
    ASSERT_EQ(tree.nodes.size(), 1);
    EXPECT_TRUE(tree.virtualRoot);
    EXPECT_TRUE(noteAppC::drawMarkmap(tree, lookOf()).svg.contains("<svg"));
}

TEST(MarkmapTreeTest, TextUnderAHeadingIsItsChildAndALazyContinuationJoinsTheItem)
{
    const MarkmapTree tree = noteAppC::parseMarkmap(QStringLiteral(
        "# R\nsome words\nmore words\n- item one\n  continues here\n\n  and a second paragraph\n- item two\n"));
    EXPECT_EQ(shape(tree), QStringLiteral("R(some words more words,item one continues here and a second paragraph,item two)"));
}

TEST(MarkmapTreeTest, TabsAndMixedMarkersNest)
{
    const MarkmapTree tree = noteAppC::parseMarkmap(QStringLiteral("# R\n* a\n\t+ b\n\t\t- c\n* d\n"));
    EXPECT_EQ(shape(tree), QStringLiteral("R(a(b(c)),d)"));
}

TEST(MarkmapTreeTest, InlineMarkdownBecomesStyledRunsAndItsMarkersVanish)
{
    const MarkmapTree tree = noteAppC::parseMarkmap(QStringLiteral(
        "# R\n- **bold** and *it* and `code` and [a link](https://example.com) and ~~gone~~\n"));
    ASSERT_EQ(tree.nodes.size(), 2);
    const QList<noteAppC::MarkmapRun> &runs = tree.nodes.at(1).runs;
    EXPECT_EQ(tree.nodes.at(1).plain(), QStringLiteral("bold and it and code and a link and gone"));
    const auto find = [&](const QString &text) -> const noteAppC::MarkmapRun * {
        for (const auto &run : runs)
            if (run.text == text)
                return &run;
        return nullptr;
    };
    ASSERT_TRUE(find(QStringLiteral("bold")));
    EXPECT_TRUE(find(QStringLiteral("bold"))->bold);
    ASSERT_TRUE(find(QStringLiteral("it")));
    EXPECT_TRUE(find(QStringLiteral("it"))->italic);
    ASSERT_TRUE(find(QStringLiteral("code")));
    EXPECT_TRUE(find(QStringLiteral("code"))->code);
    ASSERT_TRUE(find(QStringLiteral("a link")));
    EXPECT_TRUE(find(QStringLiteral("a link"))->link);
    ASSERT_TRUE(find(QStringLiteral("gone")));
    EXPECT_TRUE(find(QStringLiteral("gone"))->strike);
}

TEST(MarkmapTreeTest, AWikilinkShowsItsAliasAndAHeadingShowsItsStyles)
{
    const MarkmapTree tree = noteAppC::parseMarkmap(QStringLiteral("# The *big* [[Note|name]]\n"));
    EXPECT_EQ(tree.nodes.at(0).plain(), QStringLiteral("The big name"));
}

TEST(MarkmapTreeTest, TheFoldCommentFoldsItsNodeAndIsNotShown)
{
    const MarkmapTree tree = noteAppC::parseMarkmap(QStringLiteral(
        "# R\n- a <!-- markmap: fold -->\n  - a1\n- b\n  - b1\n"));
    EXPECT_EQ(shape(tree), QStringLiteral("R(a(a1),b(b1))"));
    EXPECT_TRUE(tree.nodes.at(1).folded);
    EXPECT_FALSE(tree.nodes.at(3).folded);
}

TEST(MarkmapTreeTest, TheFoldCommentOnALineOfItsOwnBelongsToTheNodeAbove)
{
    const MarkmapTree tree = noteAppC::parseMarkmap(QStringLiteral("# R\n## A\n<!-- markmap: fold -->\n### A1\n## B\n"));
    EXPECT_TRUE(tree.nodes.at(1).folded);
    EXPECT_EQ(shape(tree), QStringLiteral("R(A(A1),B)"));
}

TEST(MarkmapTreeTest, FoldAllFoldsEveryDescendantThatHasChildren)
{
    const MarkmapTree tree = noteAppC::parseMarkmap(QStringLiteral("# R <!-- markmap: foldAll -->\n- a\n  - a1\n    - a2\n- b\n"));
    EXPECT_TRUE(tree.nodes.at(0).folded);
    EXPECT_TRUE(tree.nodes.at(1).folded);
    EXPECT_TRUE(tree.nodes.at(2).folded);
    EXPECT_FALSE(tree.nodes.at(3).folded) << "a leaf has nothing to fold";
}

TEST(MarkmapTreeTest, TheOptionsAreReadFromTheFrontmatterNestedOrFlat)
{
    for (const QString &front : {QStringLiteral("---\nmarkmap:\n  colorFreezeLevel: 2\n  initialExpandLevel: 2\n  maxWidth: 240\n---\n"),
                                 QStringLiteral("---\ncolorFreezeLevel: 2\ninitialExpandLevel: 2\nmaxWidth: 240\n---\n")}) {
        SCOPED_TRACE(front.toStdString());
        const MarkmapTree tree = noteAppC::parseMarkmap(front + QStringLiteral("# R\n## A\n- a1\n## B\n"));
        EXPECT_EQ(tree.options.colorFreezeLevel, 2);
        EXPECT_EQ(tree.options.initialExpandLevel, 2);
        EXPECT_EQ(tree.options.maxWidth, 240);
        EXPECT_EQ(shape(tree), QStringLiteral("R(A(a1),B)")) << "the frontmatter is not drawn";
        EXPECT_FALSE(tree.nodes.at(0).folded);
        EXPECT_TRUE(tree.nodes.at(1).folded) << "A is on level 2 and has children";
    }
}

TEST(MarkmapTreeTest, InitialExpandLevelCountsTheRootAsLevelOneLikeMarkmapView)
{
    // markmap-view's view.ts, _initializeData: `depth += 1` runs before the first node is numbered, so the
    // root has `state.depth` 1, and `initialExpandLevel >= 0 && item.state.depth >= initialExpandLevel`
    // sets `fold: 1`. Level 1 therefore folds the root itself, 2 folds the root's children, and so on.
    const QString body = QStringLiteral("# R\n## A\n### a1\n#### a2\n## B\n- b1\n");
    const auto folded = [&](int level) {
        const MarkmapTree tree = noteAppC::parseMarkmap(QStringLiteral("---\ninitialExpandLevel: %1\n---\n").arg(level) + body);
        QStringList names;
        for (const MarkmapNode &node : tree.nodes)
            if (node.folded)
                names << node.plain();
        return names.join(QLatin1Char(','));
    };
    EXPECT_EQ(folded(0), QStringLiteral("R,A,a1,B")) << "everything with something to hide";
    EXPECT_EQ(folded(1), QStringLiteral("R,A,a1,B")) << "the root is folded: only it shows";
    EXPECT_EQ(folded(2), QStringLiteral("A,a1,B")) << "the root and its children show";
    EXPECT_EQ(folded(3), QStringLiteral("a1")) << "a2 is on level 4, in a1";
    EXPECT_EQ(folded(4), QString());
    EXPECT_EQ(folded(5), QString());
    // With no single top node the empty root is level 1 as well (markmap-lib's own).
    const MarkmapTree several = noteAppC::parseMarkmap(QStringLiteral("---\ninitialExpandLevel: 2\n---\n- x\n  - y\n- z\n"));
    ASSERT_TRUE(several.virtualRoot);
    EXPECT_FALSE(several.nodes.at(0).folded);
    EXPECT_TRUE(several.nodes.at(1).folded) << "x is level 2";
}

TEST(MarkmapTreeTest, NoOptionsMeansExpandedUnfrozenUnwrapped)
{
    const MarkmapTree tree = noteAppC::parseMarkmap(QStringLiteral("# R\n## A\n- a1\n"));
    EXPECT_EQ(tree.options.colorFreezeLevel, 0);
    EXPECT_EQ(tree.options.initialExpandLevel, -1);
    EXPECT_EQ(tree.options.maxWidth, 0);
    for (const MarkmapNode &node : tree.nodes)
        EXPECT_FALSE(node.folded);
}

TEST(MarkmapTreeTest, ABiggerOutlineThanTheCapIsCutAndCounted)
{
    QString source = QStringLiteral("# R\n");
    for (int i = 0; i < 3000; ++i)
        source += QStringLiteral("- item %1\n").arg(i);
    const MarkmapTree tree = noteAppC::parseMarkmap(source);
    EXPECT_EQ(tree.nodes.size(), noteAppC::kMarkmapMaxNodes);
    EXPECT_EQ(tree.total, 3001);
    const auto drawn = noteAppC::drawMarkmap(tree, lookOf());
    EXPECT_TRUE(drawn.svg.contains("3001")) << "the notice says how many there are";
    EXPECT_TRUE(drawn.svg.contains(QByteArray::number(noteAppC::kMarkmapMaxNodes)));
}

TEST(MarkmapTreeTest, ADroppedItemsContinuationLinesAreNotMoreNodes)
{
    QString source = QStringLiteral("# R\n");
    for (int i = 0; i < 3000; ++i)
        source += QStringLiteral("- item %1\n  second line\n\n").arg(i);
    const MarkmapTree tree = noteAppC::parseMarkmap(source);
    EXPECT_EQ(tree.nodes.size(), noteAppC::kMarkmapMaxNodes);
    EXPECT_EQ(tree.total, 3001) << "3000 items and the heading, not their second lines";
}

TEST(MarkmapTreeTest, TheMapOfANoteIsItsWholeBodyLikeMarkmapJsLessFrontmatterAndCode)
{
    const QString note = QStringLiteral("---\ntitle: x\n# not a heading\n---\n# Top\ntext\n- buy x\n- call y\n## One\n```\n# in code\n- not a node\n```\n"
                                       "### Two\n###### Six\n####### not a heading\n## Three\n~~~\n# also code\n~~~\n");
    const QString source = noteAppC::markmapSourceOfNote(note);
    EXPECT_FALSE(source.contains(QStringLiteral("in code")));
    EXPECT_FALSE(source.contains(QStringLiteral("also code")));
    EXPECT_FALSE(source.contains(QStringLiteral("title: x")));
    const MarkmapTree tree = noteAppC::parseMarkmap(source);
    EXPECT_EQ(shape(tree), QStringLiteral("Top(text,buy x,call y,One(Two(Six(####### not a heading))),Three)"))
        << "a sixth-level heading nests as deep as it is; seven #s are text";
    EXPECT_EQ(tree.nodes.size(), 9);
    EXPECT_TRUE(noteAppC::markmapSourceOfNote(QStringLiteral("---\na: b\n---\n```\ncode\n```\n")).isEmpty());
    EXPECT_TRUE(noteAppC::markmapSourceOfNote(QString()).isEmpty());
}

TEST(MarkmapTreeTest, ACharacterXMLCannotHoldDoesNotBreakTheSVG)
{
    const QString source = QStringLiteral("# R\x01 a\x0e b\n- lone ") + QChar(0xD83D) + QStringLiteral(" high\n- lone ") + QChar(0xDE00)
                           + QStringLiteral(" low\n- pair \U0001F600 ok\n- \uFFFE end\n");
    const auto drawn = noteAppC::drawMarkmap(noteAppC::parseMarkmap(source), lookOf());
    QString why;
    EXPECT_TRUE(wellFormed(drawn.svg, &why)) << why.toStdString();
    EXPECT_TRUE(QString::fromUtf8(drawn.svg).contains(QStringLiteral("\U0001F600"))) << "a real pair stays";
    QSvgRenderer renderer(drawn.svg);
    EXPECT_TRUE(renderer.isValid());
}

// --- the SVG -----------------------------------------------------------------

TEST(MarkmapDrawTest, TheSVGIsWellFormedAndQtDrawsIt)
{
    const MarkmapTree tree = noteAppC::parseMarkmap(QStringLiteral(
        "# R & <b>\n## A \"quoted\"\n- **bold** `a<b` [x](http://e.com?a=1&b=2)\n- ünï ✓ 日本語\n## B\n"));
    const auto drawn = noteAppC::drawMarkmap(tree, lookOf());
    QString why;
    EXPECT_TRUE(wellFormed(drawn.svg, &why)) << why.toStdString() << "\n" << drawn.svg.constData();
    QSvgRenderer renderer(drawn.svg);
    EXPECT_TRUE(renderer.isValid());
    EXPECT_GT(drawn.size.width(), 100);
    EXPECT_GT(drawn.size.height(), 40);
    EXPECT_EQ(renderer.defaultSize(), drawn.size.toSize());
    EXPECT_EQ(drawn.drawn, 5);
    EXPECT_TRUE(drawn.svg.contains("R &amp; &lt;b&gt;"));
}

TEST(MarkmapDrawTest, EveryNodesTextIsInTheSVGAndAFoldedBranchIsNot)
{
    const MarkmapTree tree = noteAppC::parseMarkmap(QStringLiteral("# Root\n## Alpha\n- alpha child\n## Beta\n- beta child <!-- markmap: fold -->\n  - beta grandchild\n"));
    const auto drawn = noteAppC::drawMarkmap(tree, lookOf());
    for (const char *text : {"Root", "Alpha", "alpha child", "Beta", "beta child"})
        EXPECT_TRUE(drawn.svg.contains(text)) << text;
    EXPECT_FALSE(drawn.svg.contains("beta grandchild"));
    EXPECT_EQ(drawn.drawn, 5);
}

TEST(MarkmapDrawTest, ThereIsOneCircleToFoldForEachDrawnNodeWithChildren)
{
    const MarkmapTree tree = noteAppC::parseMarkmap(QStringLiteral("# Root\n## Alpha\n- a1\n- a2\n## Beta\n"));
    const auto drawn = noteAppC::drawMarkmap(tree, lookOf());
    QList<int> nodes;
    for (const auto &toggle : drawn.toggles) {
        nodes << toggle.node;
        EXPECT_GT(toggle.rect.width(), 8);
        EXPECT_TRUE(QRectF(QPointF(), drawn.size).contains(toggle.rect.center()));
    }
    EXPECT_EQ(nodes, (QList<int>{0, 1}));
}

TEST(MarkmapDrawTest, ATogglingFlipsTheSourcesFoldInBothDirections)
{
    const MarkmapTree open = noteAppC::parseMarkmap(QStringLiteral("# R\n## A\n- a1\n- a2\n"));
    const auto base = noteAppC::drawMarkmap(open, lookOf());
    const auto folded = noteAppC::drawMarkmap(open, lookOf(), {1});
    EXPECT_TRUE(base.svg.contains("a1"));
    EXPECT_FALSE(folded.svg.contains("a1"));
    EXPECT_EQ(folded.drawn, 2);
    EXPECT_LT(folded.size.height(), base.size.height());

    const MarkmapTree closed = noteAppC::parseMarkmap(QStringLiteral("# R\n## A <!-- markmap: fold -->\n- a1\n- a2\n"));
    EXPECT_FALSE(noteAppC::drawMarkmap(closed, lookOf()).svg.contains("a1"));
    EXPECT_TRUE(noteAppC::drawMarkmap(closed, lookOf(), {1}).svg.contains("a1"));
}

TEST(MarkmapDrawTest, AFoldedNodeKeepsItsCircleSoItCanBeOpened)
{
    const MarkmapTree tree = noteAppC::parseMarkmap(QStringLiteral("# R\n## A\n- a1\n"));
    const auto drawn = noteAppC::drawMarkmap(tree, lookOf(), {1});
    bool found = false;
    for (const auto &toggle : drawn.toggles)
        found |= toggle.node == 1;
    EXPECT_TRUE(found);
}

TEST(MarkmapDrawTest, TheTreeIsLeftToRightWithTheRootAtTheLeftAndChildrenStackedDownward)
{
    const MarkmapTree tree = noteAppC::parseMarkmap(QStringLiteral("# Root\n## A\n## B\n## C\n"));
    const auto drawn = noteAppC::drawMarkmap(tree, lookOf());
    // Circle centres: only the root has children, so the one toggle is the root's, at its right end.
    ASSERT_EQ(drawn.toggles.size(), 1);
    const QString svg = QString::fromUtf8(drawn.svg);
    const auto xOf = [&](const QString &text) {
        const qsizetype at = svg.indexOf(QLatin1Char('>') + text + QStringLiteral("</text>"));
        EXPECT_GE(at, 0) << text.toStdString();
        const qsizetype start = svg.lastIndexOf(QStringLiteral("<text "), at);
        QRegularExpression re(QStringLiteral("x=\"([-0-9.]+)\" y=\"([-0-9.]+)\""));
        const auto m = re.match(svg, start);
        return QPointF(m.captured(1).toDouble(), m.captured(2).toDouble());
    };
    const QPointF root = xOf(QStringLiteral("Root")), a = xOf(QStringLiteral("A")), b = xOf(QStringLiteral("B")), c = xOf(QStringLiteral("C"));
    EXPECT_LT(root.x(), a.x());
    EXPECT_DOUBLE_EQ(a.x(), b.x());
    EXPECT_LT(a.y(), b.y());
    EXPECT_LT(b.y(), c.y());
    EXPECT_NEAR(root.y(), b.y(), 1.0) << "the parent is centred on its children";
}

TEST(MarkmapDrawTest, EachBranchHasItsColourAndTheFreezeLevelMakesItsDescendantsShareIt)
{
    const QString body = QStringLiteral("# R\n## A\n- a1\n  - a2\n## B\n- b1\n");
    const auto strokes = [&](const QString &source) {
        const auto drawn = noteAppC::drawMarkmap(noteAppC::parseMarkmap(source), lookOf());
        QSet<QString> colours;
        QRegularExpression re(QStringLiteral("<path[^>]*stroke=\"(#[0-9a-fA-F]{6})\""));
        auto it = re.globalMatch(QString::fromUtf8(drawn.svg));
        while (it.hasNext())
            colours.insert(it.next().captured(1));
        return colours.size();
    };
    const int free = strokes(body);
    // markmap-view's util.ts keeps `path.split('.').slice(0, colorFreezeLevel)`: the first N ids of a node's
    // path, the root's being the first, so the node on level N (the root is level 1) lends its colour.
    const int levelTwo = strokes(QStringLiteral("---\ncolorFreezeLevel: 2\n---\n") + body);
    const int levelOne = strokes(QStringLiteral("---\ncolorFreezeLevel: 1\n---\n") + body);
    EXPECT_EQ(free, 5) << "a link for each of the five children, each its own colour";
    EXPECT_EQ(levelTwo, 2) << "A's and B's descendants keep A's and B's colour";
    EXPECT_EQ(levelOne, 1) << "everything keeps the root's";
}

TEST(MarkmapDrawTest, ColoursComeFromTheThemeAndTheLightAndDarkMapsDiffer)
{
    const MarkmapTree tree = noteAppC::parseMarkmap(QStringLiteral("# R\n## A\n"));
    const Theme dark = Theme::builtin(Theme::Mode::Dark), light = Theme::builtin(Theme::Mode::Light);
    const auto darkSVG = noteAppC::drawMarkmap(tree, lookOf(Theme::Mode::Dark)).svg;
    const auto lightSVG = noteAppC::drawMarkmap(tree, lookOf(Theme::Mode::Light)).svg;
    EXPECT_TRUE(darkSVG.contains(dark.color(QStringLiteral("--background-primary")).name().toUtf8()));
    EXPECT_TRUE(lightSVG.contains(light.color(QStringLiteral("--background-primary")).name().toUtf8()));
    EXPECT_TRUE(darkSVG.contains(dark.color(QStringLiteral("--text-normal")).name().toUtf8()));
    EXPECT_TRUE(lightSVG.contains(light.color(QStringLiteral("--color-blue")).name().toUtf8()) || lightSVG.contains(light.color(QStringLiteral("--color-orange")).name().toUtf8()));
    EXPECT_NE(noteAppC::diagramConfig(noteAppC::DiagramKind::Markmap, dark), noteAppC::diagramConfig(noteAppC::DiagramKind::Markmap, light));
}

TEST(MarkmapDrawTest, MaxWidthWrapsALongNodeIntoLines)
{
    const QString text = QStringLiteral("# R\n- one two three four five six seven eight nine ten eleven twelve thirteen fourteen\n");
    const auto wide = noteAppC::drawMarkmap(noteAppC::parseMarkmap(text), lookOf());
    const auto narrow = noteAppC::drawMarkmap(noteAppC::parseMarkmap(QStringLiteral("---\nmaxWidth: 120\n---\n") + text), lookOf());
    EXPECT_GT(wide.size.width(), narrow.size.width() + 100);
    EXPECT_GT(narrow.size.height(), wide.size.height() + 20);
    EXPECT_TRUE(narrow.svg.contains("fourteen")) << "wrapped, not cut";
    EXPECT_TRUE(wellFormed(narrow.svg));
}

TEST(MarkmapDrawTest, CodeIsMonospacedAndLinksAreUnderlined)
{
    const auto drawn = noteAppC::drawMarkmap(noteAppC::parseMarkmap(QStringLiteral("# R\n- `mono` [link](http://x.y)\n")), lookOf());
    const QString svg = QString::fromUtf8(drawn.svg);
    const qsizetype code = svg.indexOf(QStringLiteral(">mono</text>"));
    ASSERT_GE(code, 0);
    EXPECT_TRUE(svg.mid(svg.lastIndexOf(QStringLiteral("<text "), code), code).contains(lookOf().monoFamily));
    EXPECT_TRUE(svg.contains(QStringLiteral("class=\"link\"")));
    EXPECT_TRUE(svg.contains(QStringLiteral("class=\"code\"")));
}

TEST(MarkmapDrawTest, ABigOutlineLaysOutAndDrawsFast)
{
    QString source = QStringLiteral("# R\n");
    for (int i = 0; i < 40; ++i) {
        source += QStringLiteral("## Section %1\n").arg(i);
        for (int j = 0; j < 12; ++j)
            source += QStringLiteral("- **item** %1 of *section* %2\n  - child [link](http://x)\n").arg(j).arg(i);
    }
    QElapsedTimer timer;
    timer.start();
    const MarkmapTree tree = noteAppC::parseMarkmap(source);
    const auto drawn = noteAppC::drawMarkmap(tree, lookOf());
    const qint64 ms = timer.elapsed();
    EXPECT_EQ(drawn.drawn, noteAppC::kMarkmapMaxNodes);
    EXPECT_TRUE(wellFormed(drawn.svg));
    EXPECT_LT(ms, 1500) << "1000 nodes parsed and drawn (off the UI thread, but not slow)";
    std::cout << "1000 nodes parsed and drawn: " << ms << " ms" << std::endl;
}

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    // Settings and the caches are under these, never the real ones.
    QTemporaryDir configHome, dataHome, cacheHome;
    qputenv("XDG_CONFIG_HOME", configHome.path().toUtf8());
    qputenv("XDG_DATA_HOME", dataHome.path().toUtf8());
    qputenv("XDG_CACHE_HOME", cacheHome.path().toUtf8());
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("noteAppC-test"));
    QCoreApplication::setApplicationName(QStringLiteral("noteAppC-test"));
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
