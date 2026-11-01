// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej
// Maths and backslash escapes in the editor: a display formula on lines of its
// own inside a paragraph, a formula kept drawn under its source while the caret
// is in it (and a diagram fence too), what MicroTeX is asked to draw, and the
// backslashes CommonMark does not show.
#include <gtest/gtest.h>

#include <QApplication>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QSignalSpy>
#include <QTest>

#include "MarkdownEditor.h"
#include "MathRenderer.h"
#include "Parser.h"

using namespace noteAppC;

namespace {

std::unique_ptr<MarkdownEditor> editorWith(const QString &text)
{
    auto editor = std::make_unique<MarkdownEditor>();
    editor->resize(900, 600);
    editor->setNoteText(text);
    return editor;
}

/// The lines a real note holds around its alphabet table.
const QString kAlphabets = QString::fromUtf8(
    "Examples of common alphabet assumptions:\n\n"
    "\\(A=26\\) (a\xe2\x80\x93z):\n"
    "$$\n"
    "\\sum_{k=4}^{9} 26^k = 5{,}646{,}683{,}807{,}856\n"
    "$$\n\n"
    "\\(A=52\\) (a\xe2\x80\x93z + A\xe2\x80\x93Z):\n"
    "$$\n"
    "\\sum_{k=4}^{9} 52^k = 2{,}834{,}413{,}841{,}995{,}008\n"
    "$$\n\n"
    "after\n");

int blocksOfKind(const Decorations &decorations, BlockSpan::Kind kind)
{
    return int(std::count_if(decorations.blocks.cbegin(), decorations.blocks.cend(),
                             [kind](const BlockSpan &block) { return block.kind == kind; }));
}

} // namespace

// --- display maths inside a paragraph --------------------------------------

TEST(DisplayMathTest, MicroTeXDrawsTheFormulasOfTheAlphabetNote)
{
    ASSERT_TRUE(MathRenderer::isAvailable());
    MathRenderer renderer;
    for (const QString &latex : {QStringLiteral("\\sum_{k=4}^{9} 26^k = 5{,}646{,}683{,}807{,}856"),
                                 QStringLiteral("\\text{candidates} = \\sum_{k=4}^{9} A^k"),
                                 QStringLiteral("\\sum_{k=4}^{9} 95^k = 636{,}954{,}190{,}678{,}260{,}000")}) {
        for (int width : {200, 700, 1500}) {
            const QImage image = renderer.render(latex, 21, Qt::white, width, 1.0);
            EXPECT_FALSE(image.isNull()) << latex.toStdString() << " at " << width << ": "
                                         << renderer.lastError().toStdString();
        }
    }
}

TEST(DisplayMathTest, AFormulaOnItsOwnLinesAfterTextIsABlockOfItsOwn)
{
    const QString note = QStringLiteral("intro:\n$$\nx^2\n$$\nouter\n\nafter\n");
    const auto parsed = Parser().parseAll(note);
    // The text before, the formula, the text after it, and the next paragraph.
    ASSERT_EQ(parsed.decorations.blocks.size(), 4);
    EXPECT_EQ(note.mid(parsed.decorations.blocks.at(0).range.start, parsed.decorations.blocks.at(0).range.length()),
              QStringLiteral("intro:\n"));
    EXPECT_EQ(note.mid(parsed.decorations.blocks.at(1).range.start, parsed.decorations.blocks.at(1).range.length()),
              QStringLiteral("$$\nx^2\n$$"));
    EXPECT_EQ(note.mid(parsed.decorations.blocks.at(2).range.start, parsed.decorations.blocks.at(2).range.length()),
              QStringLiteral("outer"));
}

TEST(DisplayMathTest, AFormulaSharingItsLineStaysInItsParagraph)
{
    const auto parsed = Parser().parseAll(QStringLiteral("a $$x^2$$ b\n\nafter\n"));
    EXPECT_EQ(blocksOfKind(parsed.decorations, BlockSpan::Kind::Paragraph), 2);
}

TEST(DisplayMathTest, TheAlphabetNotesFormulasAreDrawnNotShownAsSource)
{
    ASSERT_TRUE(MathRenderer::isAvailable());
    auto editor = editorWith(kAlphabets);
    editor->show();
    editor->setCursorOffset(0); // the caret is in the first paragraph, away from every formula
    const QString shown = editor->displayText();
    EXPECT_FALSE(shown.contains(QStringLiteral("\\sum"))) << shown.toStdString();
    EXPECT_FALSE(shown.contains(QStringLiteral("5{,}646")));
    EXPECT_TRUE(shown.contains(QString::fromUtf8("(A=26) (a\xe2\x80\x93z):"))) << shown.toStdString();
    EXPECT_TRUE(shown.contains(QStringLiteral("after")));
    // And every character is still in the note.
    EXPECT_EQ(editor->noteText(), kAlphabets);
}

TEST(DisplayMathTest, TextAfterTheClosingDollarsIsStillShown)
{
    auto editor = editorWith(QStringLiteral("$$\nx^2\n$$\ntrailing words\n\nafter\n"));
    editor->setCursorOffset(editor->noteText().indexOf(QStringLiteral("after")));
    const QString shown = editor->displayText();
    EXPECT_FALSE(shown.contains(QStringLiteral("x^2")));
    EXPECT_TRUE(shown.contains(QStringLiteral("trailing words")));
}

TEST(DisplayMathTest, StarredAlignmentAndBareAmpersandsAreDrawn)
{
    MathRenderer renderer;
    // MathJax draws these; MicroTeX alone refuses the star and an `&` outside an environment.
    for (const QString &latex : {QStringLiteral("\\begin{align*} a &= b \\\\ c &= d \\end{align*}"),
                                 QStringLiteral("a &= b \\\\ c &= d")}) {
        const QImage image = renderer.render(latex, 21, Qt::white, 700, 1.0);
        EXPECT_FALSE(image.isNull()) << latex.toStdString() << ": " << renderer.lastError().toStdString();
        EXPECT_GT(image.height(), 30) << "two lines tall: " << latex.toStdString();
    }
}

TEST(DisplayMathTest, AFormulaThatCannotBeDrawnShowsItsSourceAndSaysWhy)
{
    ASSERT_TRUE(MathRenderer::isAvailable());
    // An `&` after the environment that closed: nothing to align it in.
    const QString broken = QStringLiteral("\\begin{array}{c} a & b \\end{array} & c");
    MathRenderer renderer;
    ASSERT_TRUE(renderer.render(broken, 21, Qt::white, 700, 1.0).isNull()) << "pick a formula MicroTeX refuses";

    const QString note = QStringLiteral("before\n\n$$\n") + broken + QStringLiteral("\n$$\n\nafter\n");
    auto editor = editorWith(note);
    editor->setCursorOffset(0);
    EXPECT_TRUE(editor->displayText().contains(broken)) << "the source stays visible";
    EXPECT_FALSE(editor->previewHintAt(note.indexOf(QStringLiteral("a & b"))).isEmpty()) << "with the reason under it";
}

// --- the formula under its source while the caret is in it -----------------

TEST(PreviewTest, TheCaretInADisplayFormulaKeepsItDrawnUnderItsSource)
{
    ASSERT_TRUE(MathRenderer::isAvailable());
    const QString note = QStringLiteral("before\n\n$$\nE = mc^2\n$$\n\nafter\n");
    auto editor = editorWith(note);
    const qsizetype formula = note.indexOf(QStringLiteral("mc^2"));

    editor->setCursorOffset(0);
    EXPECT_TRUE(editor->previewSizeAt(formula).isEmpty()) << "drawn in place of its source, not under it";
    const qreal drawnHeight = editor->documentHeight();

    editor->enterBlockAt(formula);
    const QString shown = editor->displayText();
    EXPECT_TRUE(shown.contains(QStringLiteral("E = mc^2"))) << "the source is what the caret edits";
    EXPECT_TRUE(shown.contains(QStringLiteral("$$")));
    const QSize preview = editor->previewSizeAt(formula);
    EXPECT_GT(preview.width(), 20);
    EXPECT_GT(preview.height(), 10);
    // The drawing is room the document makes for it, under three lines of source.
    EXPECT_GT(editor->documentHeight(), drawnHeight);
}

TEST(PreviewTest, TheFormulaFollowsWhatIsTyped)
{
    ASSERT_TRUE(MathRenderer::isAvailable());
    const QString note = QStringLiteral("$$\nx\n$$\n\nafter\n");
    auto editor = editorWith(note);
    editor->show();
    editor->enterBlockAt(note.indexOf(QLatin1Char('x')) + 1);
    const QSize before = editor->previewSizeAt(3);
    ASSERT_FALSE(before.isEmpty());
    QTest::keyClicks(editor.get(), QStringLiteral(" + y^2 + z^2 + w^2"));
    EXPECT_TRUE(editor->noteText().contains(QStringLiteral("x + y^2 + z^2 + w^2")));
    EXPECT_GT(editor->previewSizeAt(3).width(), before.width()) << "a longer formula, a wider drawing";
}

TEST(PreviewTest, EveryDollarLineOpensWithTheCaretInTheMiddleOfTheFormula)
{
    const QString note = QStringLiteral("$$\nx^2\n$$\n\nafter\n");
    auto editor = editorWith(note);
    editor->enterBlockAt(note.indexOf(QStringLiteral("x^2")) + 1);
    EXPECT_TRUE(editor->displayText().startsWith(QStringLiteral("$$\nx^2\n$$"))) << editor->displayText().toStdString();
}

TEST(PreviewTest, SourceModeHasNoPreview)
{
    const QString note = QStringLiteral("$$\nx^2\n$$\n\nafter\n");
    auto editor = editorWith(note);
    editor->setMode(MarkdownEditor::Mode::Source);
    editor->enterBlockAt(5);
    EXPECT_TRUE(editor->previewSizeAt(5).isEmpty());
}

TEST(PreviewTest, ADiagramFenceStaysDrawnUnderItsSourceWhileItIsEdited)
{
    const QString note = QStringLiteral("before\n\n```mermaid\ngraph TD; A-->B\n```\n\nafter\n");
    auto editor = editorWith(note);
    editor->show();
    int asked = 0;
    editor->setCodeBlockRenderer(
        {QStringLiteral("mermaid")},
        [&asked](const QString &, const QString &source, int, const QString &,
                 std::function<void(MarkdownEditor::CodeBlockResult)> done) {
            ++asked;
            MarkdownEditor::CodeBlockResult result;
            result.isMarkdown = false;
            result.image = QImage(120 + source.size(), 70, QImage::Format_ARGB32_Premultiplied);
            result.image.fill(Qt::red);
            done(std::move(result));
        });
    const qsizetype inside = note.indexOf(QStringLiteral("A-->B"));

    editor->setCursorOffset(0);
    QTest::qWait(30);
    EXPECT_EQ(asked, 1) << "the diagram is drawn in place of the fence";
    EXPECT_TRUE(editor->previewSizeAt(inside).isEmpty());

    editor->enterBlockAt(inside);
    EXPECT_TRUE(editor->displayText().contains(QStringLiteral("graph TD; A-->B"))) << "the source is shown";
    EXPECT_FALSE(editor->previewSizeAt(inside).isEmpty()) << "and the drawing under it";
    EXPECT_EQ(asked, 1) << "the same source is not asked again";

    // Typing keeps the last drawing up, and asks the tool once typing pauses.
    QTest::keyClicks(editor.get(), QStringLiteral(";C-->D"));
    EXPECT_FALSE(editor->previewSizeAt(inside).isEmpty()) << "the earlier drawing stays while typing";
    EXPECT_EQ(asked, 1) << "no tool run per keystroke";
    ASSERT_TRUE(QTest::qWaitFor([&] { return asked == 2; }, 3000));
    EXPECT_TRUE(QTest::qWaitFor([&] { return editor->previewSizeAt(inside).width() > 125 + 15; }, 3000))
        << "the new drawing replaces it: " << editor->previewSizeAt(inside).width();
}

// --- caret on a formatted line ----------------------------------------------

TEST(RevealTest, StylingStaysOnTheCaretsLine)
{
    const QString note = QStringLiteral("para **bold** x *it* ==hl== [link](https://x.org) `code`\n\nafter\n");
    auto editor = editorWith(note);
    editor->show();
    const qsizetype bold = note.indexOf(QStringLiteral("bold"));
    const qsizetype italic = note.indexOf(QStringLiteral("it*"));
    const qsizetype highlight = note.indexOf(QStringLiteral("hl"));
    const qsizetype link = note.indexOf(QStringLiteral("link]"));
    for (qsizetype caret : {bold, italic, highlight, link, qsizetype(0)}) {
        editor->enterBlockAt(caret);
        SCOPED_TRACE(caret);
        EXPECT_GE(editor->formatAt(bold).fontWeight(), QFont::Bold);
        EXPECT_TRUE(editor->formatAt(italic).fontItalic());
        EXPECT_NE(editor->formatAt(highlight).background().style(), Qt::NoBrush);
        EXPECT_TRUE(editor->formatAt(link).fontUnderline() || editor->formatAt(link).foreground().style() != Qt::NoBrush);
    }
}

// --- backslash escapes -------------------------------------------------------

TEST(EscapeTest, ABackslashBeforePunctuationIsNotShown)
{
    auto editor = editorWith(QStringLiteral("\\(A=26\\) is \\*not italic\\* and a \\\\ here\n\nafter\n"));
    editor->setCursorOffset(editor->noteText().indexOf(QStringLiteral("after")));
    EXPECT_EQ(editor->displayText(), QStringLiteral("(A=26) is *not italic* and a \\ here\n\nafter\n"));
    EXPECT_FALSE(editor->formatAt(editor->noteText().indexOf(QStringLiteral("not italic"))).fontItalic());
}

TEST(EscapeTest, ABackslashBeforeAnythingElseStays)
{
    auto editor = editorWith(QStringLiteral("C:\\Users\\name and \\n and a\\\nnext\n\nafter\n"));
    editor->setCursorOffset(editor->noteText().indexOf(QStringLiteral("after")));
    EXPECT_TRUE(editor->displayText().startsWith(QStringLiteral("C:\\Users\\name and \\n and a\\\nnext")))
        << editor->displayText().toStdString();
}

TEST(EscapeTest, CodeAndMathKeepTheirBackslashes)
{
    const QString note = QStringLiteral("a `\\(x\\)` b $\\(y\\)$ c\n\n```\n\\(z\\) \\*\n```\n\nafter \\* end\n");
    auto editor = editorWith(note);
    editor->setCursorOffset(note.indexOf(QStringLiteral("end")));
    const QString shown = editor->displayText();
    EXPECT_TRUE(shown.contains(QStringLiteral("\\(x\\)"))) << shown.toStdString();
    EXPECT_TRUE(shown.contains(QStringLiteral("\\(z\\) \\*"))) << shown.toStdString();
    EXPECT_TRUE(shown.contains(QStringLiteral("after * end"))) << shown.toStdString();
}

TEST(EscapeTest, TheFrontmatterAndSourceModeKeepTheirBackslashes)
{
    const QString note = QStringLiteral("---\ntitle: a \\* b\n---\n\nbody \\* x\n");
    auto editor = editorWith(note);
    editor->setMode(MarkdownEditor::Mode::Source);
    EXPECT_EQ(editor->displayText(), note);
    const auto parsed = Parser().parseAll(note);
    ASSERT_EQ(parsed.decorations.substitutions.size(), 1) << "only the body's";
    EXPECT_GT(parsed.decorations.substitutions.first().range.start, note.indexOf(QStringLiteral("body")) - 1);
}

TEST(EscapeTest, AnEscapedAmpersandIsNotAnEntity)
{
    auto editor = editorWith(QStringLiteral("a \\&amp; b &amp; c\n\nafter\n"));
    editor->setCursorOffset(editor->noteText().indexOf(QStringLiteral("after")));
    EXPECT_TRUE(editor->displayText().startsWith(QStringLiteral("a &amp; b & c"))) << editor->displayText().toStdString();
}

TEST(EscapeTest, TheCaretOnAnEscapeShowsItsBackslashAndOnlyThatOne)
{
    const QString note = QStringLiteral("\\(a\\) and \\*b\\*\n\nafter\n");
    auto editor = editorWith(note);
    editor->enterBlockAt(1); // between the first escape's "\" and its "("
    const QString shown = editor->displayText();
    EXPECT_TRUE(shown.startsWith(QStringLiteral("\\(a) and *b*"))) << shown.toStdString();
    editor->enterBlockAt(note.indexOf(QStringLiteral("after")));
    EXPECT_TRUE(editor->displayText().startsWith(QStringLiteral("(a) and *b*")));
}

// --- what is written stays what is shown ----------------------

TEST(EscapeTest, AFormulaMdFourQtDidNotParseKeepsItsBackslashes)
{
    // A blank line inside `$$` ends md4qt's formula, so the parser finds it itself.
    const QString note = QStringLiteral("intro\n$$\na \\{ b &amp; c\n\nc \\, d\n$$\n\nafter \\* end\n");
    const auto parsed = Parser().parseAll(note);
    ASSERT_EQ(parsed.decorations.substitutions.size(), 1) << "only the one in \"after\"";
    EXPECT_GT(parsed.decorations.substitutions.first().range.start, note.indexOf(QStringLiteral("after")));

    auto editor = editorWith(note);
    editor->enterBlockAt(note.indexOf(QStringLiteral("a \\{")));
    EXPECT_TRUE(editor->displayText().contains(QStringLiteral("a \\{ b &amp; c\n\nc \\, d")))
        << editor->displayText().toStdString();
}

TEST(EscapeTest, AnAutolinkKeepsItsBackslashes)
{
    const auto parsed = Parser().parseAll(QStringLiteral("see <https://x.org/\\(y\\)> and \\(z\\)\n"));
    EXPECT_EQ(parsed.decorations.substitutions.size(), 2) << "only the text's own";
}

TEST(PreviewTest, AnUnfinishedFormulaKeepsTheLastDrawingAndTheLayout)
{
    ASSERT_TRUE(MathRenderer::isAvailable());
    const QString note = QStringLiteral("$$\n\\begin{array}{c} a \\end{array}\n$$\n\nafter\n");
    auto editor = editorWith(note);
    editor->show();
    const qsizetype end = note.indexOf(QStringLiteral("end{array}")) + 10;
    editor->enterBlockAt(end);
    const QSize drawn = editor->previewSizeAt(end);
    ASSERT_FALSE(drawn.isEmpty());
    const qreal height = editor->documentHeight();

    QTest::keyClicks(editor.get(), QStringLiteral(" &")); // an `&` outside any array: MicroTeX refuses it
    ASSERT_TRUE(editor->noteText().contains(QStringLiteral("end{array} &")));
    EXPECT_EQ(editor->previewSizeAt(end), drawn) << "the last good drawing stays";
    EXPECT_TRUE(editor->previewHintAt(end).isEmpty()) << "and nothing says it is stale";
    EXPECT_DOUBLE_EQ(editor->documentHeight(), height) << "nothing below moves";

    // With the caret gone there is no earlier drawing to keep: the source and the reason.
    editor->enterBlockAt(note.size() - 2);
    EXPECT_FALSE(editor->previewHintAt(end).isEmpty());
}

TEST(PreviewTest, TypingInALongFormulaStaysInsideTheKeystrokeBudget)
{
    ASSERT_TRUE(MathRenderer::isAvailable());
    QString rows;
    for (int i = 0; i < 12; ++i)
        rows += QStringLiteral("x_{%1} &= \\frac{a_{%1}+b_{%1}}{c} + \\sum_{k=0}^{%1} k^2 \\\\\n").arg(i);
    const QString note = QStringLiteral("$$\n\\begin{aligned}\n") + rows + QStringLiteral("\\end{aligned}\n$$\n\nafter\n");
    auto editor = editorWith(note);
    editor->show();
    editor->enterBlockAt(note.indexOf(QStringLiteral("\\end{aligned}")) - 1);
    QList<qint64> costs;
    for (int i = 0; i < 15; ++i) {
        QElapsedTimer timer;
        timer.start();
        QTest::keyClick(editor.get(), Qt::Key_A);
        costs.append(timer.nsecsElapsed());
    }
    std::sort(costs.begin(), costs.end());
    std::printf("[ PERF     ] keystroke in a 12-row aligned formula: median %.2f ms, max %.2f ms\n",
                double(costs.at(costs.size() / 2)) / 1e6, double(costs.last()) / 1e6);
    EXPECT_LT(double(costs.at(costs.size() / 2)) / 1e6, 10.0);
}

TEST(PreviewTest, ManyPausesInADiagramLeaveABoundedCache)
{
    const QString note = QStringLiteral("```mermaid\ngraph TD; A-->B\n```\n\nafter\n");
    auto editor = editorWith(note);
    editor->show();
    int asked = 0;
    editor->setCodeBlockRenderer(
        {QStringLiteral("mermaid")},
        [&asked](const QString &, const QString &source, int, const QString &,
                 const std::function<void(MarkdownEditor::CodeBlockResult)> &done) {
            ++asked;
            MarkdownEditor::CodeBlockResult result;
            result.isMarkdown = false;
            result.image = QImage(100 + source.size(), 60, QImage::Format_ARGB32_Premultiplied);
            result.image.fill(Qt::red);
            done(std::move(result));
        });
    editor->enterBlockAt(note.indexOf(QStringLiteral("A-->B")));
    for (int pause = 1; pause <= 6; ++pause) {
        QTest::keyClick(editor.get(), Qt::Key_X);
        ASSERT_TRUE(QTest::qWaitFor([&] { return asked == pause; }, 3000)) << "pause " << pause;
        QTest::qWait(20);
        EXPECT_LE(editor->codeBlockCacheSizeForTest(), 2) << "after " << pause << " pauses";
    }
}

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("noteAppC-test"));
    QCoreApplication::setApplicationName(QStringLiteral("noteAppC-test"));
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
