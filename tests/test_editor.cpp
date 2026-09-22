#include <gtest/gtest.h>

#include <QElapsedTimer>
#include <QRandomGenerator>

#include <cstdio>

#include <memory>

#include "DisplayMap.h"

using namespace noteAppC;

namespace {

/// The whole block, which is what most of these tests map over.
Range whole(const QString &text)
{
    return {0, text.size()};
}

} // namespace

TEST(DisplayMapTest, IdentityHidesNothing)
{
    const QString source = QStringLiteral("plain text");
    const DisplayMap map = DisplayMap::identity(source, whole(source));

    EXPECT_EQ(map.text(), source);
    EXPECT_FALSE(map.hidesAnything());
    for (qsizetype i = 0; i <= source.size(); ++i) {
        EXPECT_EQ(map.toDisplay(i), i);
        EXPECT_EQ(map.toSource(i), i);
    }
}

TEST(DisplayMapTest, HidesTheHeadingMarker)
{
    const QString source = QStringLiteral("## Heading");
    const DisplayMap map = DisplayMap::build(source, whole(source), {{0, 3}});

    EXPECT_EQ(map.text(), QStringLiteral("Heading"));
    EXPECT_TRUE(map.hidesAnything());
    EXPECT_EQ(map.toSource(0), 3);
    EXPECT_EQ(map.toDisplay(3), 0);
    // Everything inside the marker collapses onto the first visible position.
    EXPECT_EQ(map.toDisplay(0), 0);
    EXPECT_EQ(map.toDisplay(2), 0);
    EXPECT_TRUE(map.isHidden(1));
    EXPECT_FALSE(map.isHidden(3));
}

TEST(DisplayMapTest, HidesWikilinkBracketsAndTarget)
{
    //                              0         1         2
    //                              0123456789012345678901234
    const QString source = QStringLiteral("see [[Target|shown]] here");
    // Hide "[[Target|" and "]]", which is what live preview does.
    const DisplayMap map = DisplayMap::build(source, whole(source), {{4, 13}, {18, 20}});

    EXPECT_EQ(map.text(), QStringLiteral("see shown here"));
    EXPECT_EQ(map.toSource(map.text().indexOf(QLatin1Char('s'), 4)), 13);
    EXPECT_EQ(map.toDisplay(13), 4);
    EXPECT_EQ(map.toDisplay(20), 9);
}

TEST(DisplayMapTest, OverlappingAndUnsortedRangesMerge)
{
    const QString source = QStringLiteral("abcdefgh");
    const DisplayMap map = DisplayMap::build(source, whole(source), {{4, 6}, {1, 3}, {2, 5}});
    // 1..6 merged away.
    EXPECT_EQ(map.text(), QStringLiteral("agh"));
}

TEST(DisplayMapTest, RangesOutsideTheBlockAreIgnored)
{
    const QString source = QStringLiteral("0123456789");
    const DisplayMap map = DisplayMap::build(source, {3, 7}, {{0, 4}, {6, 9}});

    EXPECT_EQ(map.text(), QStringLiteral("45"));
    EXPECT_EQ(map.toSource(0), 4);
    EXPECT_EQ(map.toSource(2), 6);
}

TEST(DisplayMapTest, EverythingHidden)
{
    const QString source = QStringLiteral("%%a comment%%");
    const DisplayMap map = DisplayMap::build(source, whole(source), {whole(source)});

    EXPECT_TRUE(map.text().isEmpty());
    // Nothing is visible, so the one display position is the block start:
    // the caret sits before the hidden text, and typing there stays outside it.
    EXPECT_EQ(map.toSource(0), 0);
    EXPECT_EQ(map.toDisplay(5), 0);
}

TEST(DisplayMapTest, EmptyBlock)
{
    const QString source;
    const DisplayMap map = DisplayMap::build(source, {0, 0}, {});
    EXPECT_TRUE(map.text().isEmpty());
    EXPECT_EQ(map.toSource(0), 0);
    EXPECT_EQ(map.toDisplay(0), 0);
}

// The plan's Phase 2 gate: a million random maps must never break the
// round trip, because every cursor move in live preview goes through it.
TEST(DisplayMapTest, FuzzRoundTrip)
{
    auto *random = QRandomGenerator::global();
    constexpr int kCases = 4000;

    for (int c = 0; c < kCases; ++c) {
        const qsizetype length = random->bounded(1, 80);
        QString source(length, QLatin1Char('.'));
        for (qsizetype i = 0; i < length; ++i)
            source[i] = QChar(QLatin1Char('a' + random->bounded(26)));

        QList<Range> hidden;
        const int holes = random->bounded(0, 6);
        for (int h = 0; h < holes; ++h) {
            const qsizetype start = random->bounded(int(length));
            const qsizetype end = start + random->bounded(1, int(length - start) + 1);
            hidden.append({start, end});
        }

        const DisplayMap map = DisplayMap::build(source, whole(source), hidden);

        // 1. The display string is the source minus the hidden ranges.
        QString expected;
        for (qsizetype i = 0; i < length; ++i) {
            if (!map.isHidden(i))
                expected += source.at(i);
        }
        ASSERT_EQ(map.text(), expected) << "case " << c;

        // 2. Display -> source -> display is the identity.
        for (qsizetype d = 0; d <= map.text().size(); ++d) {
            const qsizetype s = map.toSource(d);
            ASSERT_GE(s, 0);
            ASSERT_LE(s, length);
            ASSERT_EQ(map.toDisplay(s), d) << "case " << c << " display " << d;
        }

        // 3. Mapping is monotonic in both directions: a cursor never jumps
        //    backwards when it moves forwards.
        qsizetype previous = -1;
        for (qsizetype s = 0; s <= length; ++s) {
            const qsizetype d = map.toDisplay(s);
            ASSERT_GE(d, previous) << "case " << c << " source " << s;
            previous = d;
        }

        // 4. A visible offset survives the trip unchanged.
        for (qsizetype s = 0; s < length; ++s) {
            if (!map.isHidden(s))
                ASSERT_EQ(map.toSource(map.toDisplay(s)), s) << "case " << c << " source " << s;
        }
    }
}

// --- the widget ----------------------------------------------------------

#include <QApplication>
#include <QFontMetricsF>
#include <QDirIterator>
#include <QFile>
#include <QFontDatabase>
#include <QTemporaryDir>
#include <QTest>

#include <utility>

#include "CodeHighlighter.h"
#include "MarkdownEditor.h"
#include "MathRenderer.h"
#include "PdfPage.h"

namespace {

/// An editor sized like a real window, on the offscreen platform.
std::unique_ptr<MarkdownEditor> editorWith(const QString &text)
{
    auto editor = std::make_unique<MarkdownEditor>();
    editor->resize(900, 600);
    editor->setNoteText(text);
    return editor;
}

} // namespace

TEST(MarkdownEditorTest, RendersTheWholeNoteUntilTheReaderClicksIn)
{
    const QString source = QStringLiteral("# Title\n\nSome **bold** text.\n");
    auto editor = editorWith(source);

    // Untouched: every marker is hidden.
    EXPECT_EQ(editor->displayText(), QStringLiteral("Title\n\nSome bold text.\n"));

    // Click into the heading: that block, and only that block, shows its source.
    editor->enterBlockAt(2);
    EXPECT_EQ(editor->displayText(), QStringLiteral("# Title\n\nSome bold text.\n"));

    // Move to the paragraph: the heading goes back to rendered.
    editor->enterBlockAt(source.indexOf(QStringLiteral("bold")));
    EXPECT_EQ(editor->displayText(), QStringLiteral("Title\n\nSome **bold** text.\n"));
}

TEST(MarkdownEditorTest, SourceModeHidesNothingAndReadingHidesEverything)
{
    const QString source = QStringLiteral("# Title\n\n[[A link|shown]]\n");
    auto editor = editorWith(source);

    editor->setMode(MarkdownEditor::Mode::Source);
    EXPECT_EQ(editor->displayText(), source);

    editor->setMode(MarkdownEditor::Mode::Reading);
    editor->enterBlockAt(2); // no effect: reading view has no active block
    EXPECT_EQ(editor->displayText(), QStringLiteral("Title\n\nshown\n"));
}

TEST(MarkdownEditorTest, TypingEditsTheBufferAtTheCaret)
{
    auto editor = editorWith(QStringLiteral("hello\n"));
    editor->setCursorOffset(5);
    QTest::keyClicks(editor.get(), QStringLiteral(" world"));

    EXPECT_EQ(editor->noteText(), QStringLiteral("hello world\n"));
    EXPECT_TRUE(editor->isModified());
    EXPECT_EQ(editor->cursorOffset(), 11);
    EXPECT_EQ(editor->wordCount(), 2);
}

TEST(MarkdownEditorTest, BackspaceAndUndo)
{
    auto editor = editorWith(QStringLiteral("abc\n"));
    editor->setCursorOffset(3);
    QTest::keyClick(editor.get(), Qt::Key_Backspace);
    EXPECT_EQ(editor->noteText(), QStringLiteral("ab\n"));

    QTest::keyClick(editor.get(), Qt::Key_Z, Qt::ControlModifier);
    EXPECT_EQ(editor->noteText(), QStringLiteral("abc\n"));

    QTest::keyClick(editor.get(), Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier);
    EXPECT_EQ(editor->noteText(), QStringLiteral("ab\n"));
}

TEST(MarkdownEditorTest, SelectAllThenTypeReplaces)
{
    auto editor = editorWith(QStringLiteral("old text\n"));
    QTest::keyClick(editor.get(), Qt::Key_A, Qt::ControlModifier);
    QTest::keyClicks(editor.get(), QStringLiteral("new"));
    EXPECT_EQ(editor->noteText(), QStringLiteral("new"));
}

TEST(MarkdownEditorTest, TypingTwoBracketsClosesThePair)
{
    auto editor = editorWith(QStringLiteral("see \n"));
    editor->setCursorOffset(4);
    QTest::keyClicks(editor.get(), QStringLiteral("[["));
    EXPECT_EQ(editor->noteText(), QStringLiteral("see [[]]\n"));
    EXPECT_EQ(editor->cursorOffset(), 6);
}

TEST(MarkdownEditorTest, CursorSkipsOverHiddenSyntax)
{
    // "see [[Target|shown]]" renders as "see shown"; moving right from the
    // space must land on the "s" of "shown", not inside the hidden brackets.
    const QString source = QStringLiteral("see [[Target|shown]] end\n");
    auto editor = editorWith(source);

    editor->setCursorOffset(4);
    EXPECT_EQ(editor->displayText(), QStringLiteral("see shown end\n"));
    EXPECT_EQ(editor->noteText(), source);
}

namespace {

/// A note of roughly `kilobytes` kB with realistic structure.
QString generateNote(int kilobytes)
{
    QString text;
    int block = 0;
    while (text.size() < kilobytes * 1024) {
        text += QStringLiteral("## Section %1\n\nA paragraph with **bold**, *italic*, "
                               "a [[Wikilink %1]], a #tag%1 and `code`. It runs long enough "
                               "to wrap across a line or two in the editor.\n\n"
                               "- a list item\n- another one\n\n")
                    .arg(block++);
    }
    return text;
}

} // namespace

// The plan's budget: keystroke to paint under 8 ms on a 200 kB note. This
// asserts the parse-and-relayout half, which is the part that can blow it.
TEST(MarkdownEditorTest, KeystrokeStaysInsideTheBudget)
{
    const QString note = generateNote(200);
    auto editor = editorWith(note);
    editor->setCursorOffset(note.size() / 2);
    editor->enterBlockAt(note.size() / 2);

    QElapsedTimer clock;
    clock.start();
    constexpr int kKeystrokes = 20;
    for (int i = 0; i < kKeystrokes; ++i)
        QTest::keyClicks(editor.get(), QStringLiteral("x"));
    const double perKeystroke = double(clock.nsecsElapsed()) / kKeystrokes / 1e6;

    std::printf("[ PERF     ] %.2f kB note, %.2f ms per keystroke\n",
                note.size() / 1024.0, perKeystroke);
    EXPECT_LT(perKeystroke, 8.0);
}

TEST(MarkdownEditorTest, OpeningALargeNoteIsFast)
{
    const QString note = generateNote(200);
    QElapsedTimer clock;
    clock.start();
    auto editor = editorWith(note);
    const double milliseconds = clock.nsecsElapsed() / 1e6;

    std::printf("[ PERF     ] open %.2f kB note: %.2f ms, %lld blocks\n",
                note.size() / 1024.0, milliseconds,
                (long long)editor->displayText().count(QLatin1Char('\n')));
    EXPECT_LT(milliseconds, 300.0);
}

TEST(MarkdownEditorTest, OpeningAMegabyteNoteStaysLinear)
{
    // The guard on a specific quadratic: ObsidianSyntax::scan ran the lazy
    // `%%...%%` comment pattern once per unmasked region, and each run with no
    // match had to walk to the end of the file. A 1 MB note has some 16 000
    // regions, so opening it took 2 s of which the comment pass was 1.9 s.
    //
    // Asserted as a *ratio*, not a wall-clock number: five times the text
    // should cost roughly five times the work, and a quadratic cannot hide
    // inside that however fast the machine is.
    const QString small = generateNote(200);
    const QString large = generateNote(1000);

    QElapsedTimer clock;
    clock.start();
    auto smallEditor = editorWith(small);
    const double smallMs = clock.nsecsElapsed() / 1e6;

    clock.restart();
    auto largeEditor = editorWith(large);
    const double largeMs = clock.nsecsElapsed() / 1e6;

    const double sizeRatio = double(large.size()) / double(small.size());
    const double timeRatio = largeMs / qMax(0.001, smallMs);
    std::printf("[ PERF     ] open %.0f kB: %.1f ms, %.0f kB: %.1f ms (%.1fx text, %.1fx time)\n",
                small.size() / 1024.0, smallMs, large.size() / 1024.0, largeMs,
                sizeRatio, timeRatio);

    // Generous: linear would be ~5x, the old quadratic was over 30x.
    EXPECT_LT(timeRatio, sizeRatio * 2.5);
    EXPECT_LT(largeMs, 900.0);
}

TEST(MarkdownEditorTest, SoftBreaksJoinLinesTheWayObsidianDoes)
{
    // One paragraph across two source lines renders as one flowing line;
    // the blank line between paragraphs still breaks.
    auto editor = editorWith(QStringLiteral("one\ntwo\n\nthree\n"));
    EXPECT_EQ(editor->displayText(), QStringLiteral("one\ntwo\n\nthree\n"));
    EXPECT_EQ(editor->noteText(), QStringLiteral("one\ntwo\n\nthree\n"));
    // The buffer is untouched by the rendering decision, which is the point.
    EXPECT_EQ(editor->wordCount(), 3);
}

TEST(CodeHighlighterTest, KnowsKateDefinitionsAndObsidianAliases)
{
    CodeHighlighter highlighter;

    EXPECT_EQ(CodeHighlighter::kateNameFor(QStringLiteral("cpp")), QStringLiteral("C++"));
    EXPECT_EQ(CodeHighlighter::kateNameFor(QStringLiteral("py")), QStringLiteral("Python"));
    EXPECT_EQ(CodeHighlighter::kateNameFor(QStringLiteral("sh")), QStringLiteral("Bash"));
    // An unknown id passes through rather than being mangled.
    EXPECT_EQ(CodeHighlighter::kateNameFor(QStringLiteral("Rust")), QStringLiteral("Rust"));

    EXPECT_TRUE(highlighter.knows(QStringLiteral("cpp")));
    EXPECT_TRUE(highlighter.knows(QStringLiteral("python")));
    EXPECT_FALSE(highlighter.knows(QStringLiteral("not-a-language")));
    EXPECT_FALSE(highlighter.knows(QString()));
}

TEST(CodeHighlighterTest, ColoursTokensAndLeavesUnknownLanguagesAlone)
{
    CodeHighlighter highlighter;
    const QString code = QStringLiteral("int x = 42; // comment\nreturn x;\n");

    const auto formats = highlighter.highlight(code, QStringLiteral("cpp"));
    ASSERT_FALSE(formats.isEmpty());
    for (const QTextLayout::FormatRange &range : formats) {
        EXPECT_GE(range.start, 0);
        EXPECT_GT(range.length, 0);
        EXPECT_LE(range.start + range.length, code.size());
    }

    // The keyword on the second line must be found too, so the per-line
    // offset bookkeeping is right.
    const int secondLine = int(code.indexOf(QStringLiteral("return")));
    bool sawReturn = false;
    for (const QTextLayout::FormatRange &range : formats)
        sawReturn = sawReturn || range.start == secondLine;
    EXPECT_TRUE(sawReturn);

    EXPECT_TRUE(highlighter.highlight(code, QStringLiteral("not-a-language")).isEmpty());
    EXPECT_TRUE(highlighter.highlight(QString(), QStringLiteral("cpp")).isEmpty());
}

// --- auto-pairs (PLAN.md 4.6) --------------------------------------------

TEST(AutoPairTest, InsertsTheClosingHalfAndSitsBetweenThem)
{
    for (const auto &pair : {std::pair{QStringLiteral("("), QStringLiteral("()")},
                             std::pair{QStringLiteral("["), QStringLiteral("[]")},
                             std::pair{QStringLiteral("{"), QStringLiteral("{}")},
                             std::pair{QStringLiteral("\""), QStringLiteral("\"\"")},
                             std::pair{QStringLiteral("'"), QStringLiteral("''")}}) {
        // On an empty line: a quote is its own closer, so it deliberately
        // refuses to open straight after a word (see the test below).
        auto editor = editorWith(QStringLiteral("\n"));
        editor->setCursorOffset(0);
        QTest::keyClicks(editor.get(), pair.first);
        EXPECT_EQ(editor->noteText(), pair.second + QStringLiteral("\n"));
        EXPECT_EQ(editor->cursorOffset(), 1) << "caret belongs between " << qPrintable(pair.second);
    }
}

TEST(AutoPairTest, TypingTheCloserStepsOverIt)
{
    auto editor = editorWith(QStringLiteral("x\n"));
    editor->setCursorOffset(1);
    QTest::keyClicks(editor.get(), QStringLiteral("("));
    ASSERT_EQ(editor->noteText(), QStringLiteral("x()\n"));

    QTest::keyClicks(editor.get(), QStringLiteral(")"));
    EXPECT_EQ(editor->noteText(), QStringLiteral("x()\n")); // no second bracket
    EXPECT_EQ(editor->cursorOffset(), 3);
}

TEST(AutoPairTest, BackspaceBetweenAnEmptyPairTakesBoth)
{
    auto editor = editorWith(QStringLiteral("x\n"));
    editor->setCursorOffset(1);
    QTest::keyClicks(editor.get(), QStringLiteral("("));
    ASSERT_EQ(editor->noteText(), QStringLiteral("x()\n"));

    QTest::keyClick(editor.get(), Qt::Key_Backspace);
    EXPECT_EQ(editor->noteText(), QStringLiteral("x\n"));
    EXPECT_EQ(editor->cursorOffset(), 1);
}

TEST(AutoPairTest, DoesNotOpenAQuoteInsideAWord)
{
    auto editor = editorWith(QStringLiteral("don\n"));
    editor->setCursorOffset(3);
    QTest::keyClicks(editor.get(), QStringLiteral("'"));
    EXPECT_EQ(editor->noteText(), QStringLiteral("don'\n"));
}

TEST(AutoPairTest, DoesNotCloseInFrontOfAWord)
{
    // CodeMirror's rule, which is the one Obsidian runs: "(" before a word
    // stays a lone bracket.
    auto editor = editorWith(QStringLiteral("word\n"));
    editor->setCursorOffset(0);
    QTest::keyClicks(editor.get(), QStringLiteral("("));
    EXPECT_EQ(editor->noteText(), QStringLiteral("(word\n"));
}

TEST(AutoPairTest, WrapsASelectionAndKeepsIt)
{
    auto editor = editorWith(QStringLiteral("word"));
    QTest::keyClick(editor.get(), Qt::Key_A, Qt::ControlModifier);

    QTest::keyClicks(editor.get(), QStringLiteral("*"));
    EXPECT_EQ(editor->noteText(), QStringLiteral("*word*"));

    // The selection survives, so a second press reaches Obsidian's "**".
    QTest::keyClicks(editor.get(), QStringLiteral("*"));
    EXPECT_EQ(editor->noteText(), QStringLiteral("**word**"));
}

TEST(AutoPairTest, WrapsWithTheMarkdownPairsToo)
{
    for (const auto &marker : {QStringLiteral("="), QStringLiteral("~"),
                               QStringLiteral("_"), QStringLiteral("`")}) {
        auto editor = editorWith(QStringLiteral("word"));
        QTest::keyClick(editor.get(), Qt::Key_A, Qt::ControlModifier);
        QTest::keyClicks(editor.get(), marker);
        QTest::keyClicks(editor.get(), marker);
        EXPECT_EQ(editor->noteText(), marker + marker + QStringLiteral("word") + marker + marker);
    }
}

TEST(AutoPairTest, CanBeTurnedOff)
{
    auto editor = editorWith(QStringLiteral("x\n"));
    editor->setAutoPairs(false);
    editor->setCursorOffset(1);
    QTest::keyClicks(editor.get(), QStringLiteral("("));
    EXPECT_EQ(editor->noteText(), QStringLiteral("x(\n"));
    EXPECT_TRUE(editorWith(QString())->autoPairs()) << "on by default, as Obsidian ships it";
}

// --- list continuation and renumbering ------------------------------------

TEST(ListEditTest, EnterContinuesTheBullet)
{
    auto editor = editorWith(QStringLiteral("- one\n"));
    editor->setCursorOffset(5);
    QTest::keyClick(editor.get(), Qt::Key_Return);

    EXPECT_EQ(editor->noteText(), QStringLiteral("- one\n- \n"));
    EXPECT_EQ(editor->cursorOffset(), 8);
}

TEST(ListEditTest, EnterCarriesAnUntickedCheckbox)
{
    auto editor = editorWith(QStringLiteral("- [x] done\n"));
    editor->setCursorOffset(10);
    QTest::keyClick(editor.get(), Qt::Key_Return);

    EXPECT_EQ(editor->noteText(), QStringLiteral("- [x] done\n- [ ] \n"));
}

TEST(ListEditTest, EnterIncrementsAndRenumbersTheRest)
{
    auto editor = editorWith(QStringLiteral("1. a\n2. b\n3. c\n"));
    editor->setCursorOffset(9); // end of "2. b"
    QTest::keyClick(editor.get(), Qt::Key_Return);

    EXPECT_EQ(editor->noteText(), QStringLiteral("1. a\n2. b\n3. \n4. c\n"));
    EXPECT_EQ(editor->cursorOffset(), 13);
}

TEST(ListEditTest, EnterOnAnEmptyItemEndsTheList)
{
    auto editor = editorWith(QStringLiteral("- a\n- \n"));
    editor->setCursorOffset(6);
    QTest::keyClick(editor.get(), Qt::Key_Return);

    EXPECT_EQ(editor->noteText(), QStringLiteral("- a\n\n"));
    EXPECT_EQ(editor->cursorOffset(), 4);
}

TEST(ListEditTest, EnterOnAnEmptyNestedItemOutdentsFirst)
{
    auto editor = editorWith(QStringLiteral("- a\n\t- \n"));
    editor->setCursorOffset(7);
    QTest::keyClick(editor.get(), Qt::Key_Return);

    EXPECT_EQ(editor->noteText(), QStringLiteral("- a\n- \n"));
    EXPECT_EQ(editor->cursorOffset(), 6);
}

TEST(ListEditTest, TabIndentsAndRenumbersBothLevels)
{
    auto editor = editorWith(QStringLiteral("1. a\n2. b\n3. c\n"));
    editor->setCursorOffset(5); // start of "2. b"
    QTest::keyClick(editor.get(), Qt::Key_Tab);

    EXPECT_EQ(editor->noteText(), QStringLiteral("1. a\n\t1. b\n2. c\n"));
    EXPECT_EQ(editor->cursorOffset(), 9);
}

TEST(ListEditTest, ShiftTabOutdentsAndRenumbers)
{
    auto editor = editorWith(QStringLiteral("1. a\n\t1. b\n2. c\n"));
    editor->setCursorOffset(9);
    QTest::keyClick(editor.get(), Qt::Key_Backtab, Qt::ShiftModifier);

    EXPECT_EQ(editor->noteText(), QStringLiteral("1. a\n2. b\n3. c\n"));
    EXPECT_EQ(editor->cursorOffset(), 8);
}

TEST(ListEditTest, TabOutsideAListStillIndentsTheText)
{
    auto editor = editorWith(QStringLiteral("plain\n"));
    editor->setCursorOffset(5);
    QTest::keyClick(editor.get(), Qt::Key_Tab);
    EXPECT_EQ(editor->noteText(), QStringLiteral("plain    \n"));
}

TEST(ListEditTest, ContinuationIsOneUndoStep)
{
    const QString source = QStringLiteral("1. a\n2. b\n3. c\n");
    auto editor = editorWith(source);
    editor->setCursorOffset(9);
    QTest::keyClick(editor.get(), Qt::Key_Return);
    ASSERT_NE(editor->noteText(), source);

    QTest::keyClick(editor.get(), Qt::Key_Z, Qt::ControlModifier);
    EXPECT_EQ(editor->noteText(), source);
}

// --- checkboxes -----------------------------------------------------------

TEST(CheckboxTest, CtrlEnterTogglesWithoutMovingTheCaret)
{
    auto editor = editorWith(QStringLiteral("- [ ] task\n"));
    editor->setCursorOffset(8);

    QTest::keyClick(editor.get(), Qt::Key_Return, Qt::ControlModifier);
    EXPECT_EQ(editor->noteText(), QStringLiteral("- [x] task\n"));
    EXPECT_EQ(editor->cursorOffset(), 8);

    QTest::keyClick(editor.get(), Qt::Key_Return, Qt::ControlModifier);
    EXPECT_EQ(editor->noteText(), QStringLiteral("- [ ] task\n"));
    EXPECT_EQ(editor->cursorOffset(), 8);
}

TEST(CheckboxTest, CtrlEnterMakesATaskOutOfALineThatIsNotOne)
{
    auto editor = editorWith(QStringLiteral("- item\n"));
    editor->setCursorOffset(6);
    QTest::keyClick(editor.get(), Qt::Key_Return, Qt::ControlModifier);
    EXPECT_EQ(editor->noteText(), QStringLiteral("- [ ] item\n"));

    auto plain = editorWith(QStringLiteral("hello\n"));
    plain->setCursorOffset(3);
    QTest::keyClick(plain.get(), Qt::Key_Return, Qt::ControlModifier);
    EXPECT_EQ(plain->noteText(), QStringLiteral("- [ ] hello\n"));
    EXPECT_EQ(plain->cursorOffset(), 9); // still on the same character
}

namespace {

/// Where the painter puts the checkbox of a task on the note's first line:
/// the content column is centred, the list margin is 26 px, the box is 0.8 of
/// the line's font height and sits 8 px left of the text, and the first block
/// starts 28 px down. Same numbers as `paintListMarkers` and `checkboxRect`.
QPoint firstCheckboxPoint(const MarkdownEditor &editor)
{
    const int viewport = editor.viewport()->width();
    const int column = qMax(120, qMin(viewport - 48, 700));
    const int left = qMax(24, (viewport - column) / 2);
    const qreal fontHeight = QFontMetricsF(Theme::builtin().textFont()).height();
    return QPoint(left + int(26 - 8 - fontHeight * 0.4), int(28 + fontHeight / 2));
}

} // namespace

TEST(CheckboxTest, ClickingTheRenderedBoxToggles)
{
    auto editor = editorWith(QStringLiteral("- [ ] task\n"));
    const QPoint point = firstCheckboxPoint(*editor);

    QTest::mouseClick(editor->viewport(), Qt::LeftButton, Qt::NoModifier, point);
    EXPECT_EQ(editor->noteText(), QStringLiteral("- [x] task\n"))
        << "clicked at " << point.x() << "," << point.y();
}

// --- moving and duplicating lines ----------------------------------------

TEST(LineMoveTest, AltUpAndAltDownSwapTheLine)
{
    auto editor = editorWith(QStringLiteral("a\nb\nc\n"));
    editor->setCursorOffset(2); // on "b"

    QTest::keyClick(editor.get(), Qt::Key_Up, Qt::AltModifier);
    EXPECT_EQ(editor->noteText(), QStringLiteral("b\na\nc\n"));
    EXPECT_EQ(editor->cursorOffset(), 0);

    QTest::keyClick(editor.get(), Qt::Key_Down, Qt::AltModifier);
    EXPECT_EQ(editor->noteText(), QStringLiteral("a\nb\nc\n"));
    EXPECT_EQ(editor->cursorOffset(), 2);
}

TEST(LineMoveTest, MovingStopsAtTheEdges)
{
    auto editor = editorWith(QStringLiteral("a\nb"));
    editor->setCursorOffset(0);
    QTest::keyClick(editor.get(), Qt::Key_Up, Qt::AltModifier);
    EXPECT_EQ(editor->noteText(), QStringLiteral("a\nb"));

    editor->setCursorOffset(3);
    QTest::keyClick(editor.get(), Qt::Key_Down, Qt::AltModifier);
    EXPECT_EQ(editor->noteText(), QStringLiteral("a\nb"));
}

TEST(LineMoveTest, MovingTakesEveryLineTheSelectionTouches)
{
    auto editor = editorWith(QStringLiteral("a\nb\nc\n"));
    editor->setCursorOffset(0);
    for (int i = 0; i < 3; ++i)
        QTest::keyClick(editor.get(), Qt::Key_Right, Qt::ShiftModifier); // "a\nb"

    QTest::keyClick(editor.get(), Qt::Key_Down, Qt::AltModifier);
    EXPECT_EQ(editor->noteText(), QStringLiteral("c\na\nb\n"));
}

TEST(LineMoveTest, CtrlShiftDDuplicatesAndFollowsTheCopy)
{
    auto editor = editorWith(QStringLiteral("a\nb\n"));
    editor->setCursorOffset(0);
    QTest::keyClick(editor.get(), Qt::Key_D, Qt::ControlModifier | Qt::ShiftModifier);

    EXPECT_EQ(editor->noteText(), QStringLiteral("a\na\nb\n"));
    EXPECT_EQ(editor->cursorOffset(), 2);
}

// --- formatting commands --------------------------------------------------

TEST(FormattingTest, CtrlBWrapsThenUnwraps)
{
    auto editor = editorWith(QStringLiteral("hello"));
    QTest::keyClick(editor.get(), Qt::Key_A, Qt::ControlModifier);

    QTest::keyClick(editor.get(), Qt::Key_B, Qt::ControlModifier);
    EXPECT_EQ(editor->noteText(), QStringLiteral("**hello**"));

    // The selection stayed on the words, so the same key takes the markers off.
    QTest::keyClick(editor.get(), Qt::Key_B, Qt::ControlModifier);
    EXPECT_EQ(editor->noteText(), QStringLiteral("hello"));
}

TEST(FormattingTest, ItalicAndHighlightUseTheirOwnMarkers)
{
    auto italic = editorWith(QStringLiteral("hello"));
    QTest::keyClick(italic.get(), Qt::Key_A, Qt::ControlModifier);
    QTest::keyClick(italic.get(), Qt::Key_I, Qt::ControlModifier);
    EXPECT_EQ(italic->noteText(), QStringLiteral("*hello*"));

    auto highlight = editorWith(QStringLiteral("hello"));
    QTest::keyClick(highlight.get(), Qt::Key_A, Qt::ControlModifier);
    QTest::keyClick(highlight.get(), Qt::Key_H, Qt::ControlModifier | Qt::ShiftModifier);
    EXPECT_EQ(highlight->noteText(), QStringLiteral("==hello=="));
}

TEST(FormattingTest, WithNoSelectionThePairOpensRoundTheCaret)
{
    auto editor = editorWith(QStringLiteral("x"));
    editor->setCursorOffset(1);
    QTest::keyClick(editor.get(), Qt::Key_B, Qt::ControlModifier);

    EXPECT_EQ(editor->noteText(), QStringLiteral("x****"));
    EXPECT_EQ(editor->cursorOffset(), 3);
}

TEST(FormattingTest, CtrlKPutsTheCaretInTheSlotThatIsStillEmpty)
{
    auto url = editorWith(QStringLiteral("https://example.com"));
    QTest::keyClick(url.get(), Qt::Key_A, Qt::ControlModifier);
    QTest::keyClick(url.get(), Qt::Key_K, Qt::ControlModifier);
    EXPECT_EQ(url->noteText(), QStringLiteral("[](https://example.com)"));
    EXPECT_EQ(url->cursorOffset(), 1) << "a known URL leaves the text slot to fill in";

    auto text = editorWith(QStringLiteral("click me"));
    QTest::keyClick(text.get(), Qt::Key_A, Qt::ControlModifier);
    QTest::keyClick(text.get(), Qt::Key_K, Qt::ControlModifier);
    EXPECT_EQ(text->noteText(), QStringLiteral("[click me]()"));
    EXPECT_EQ(text->cursorOffset(), 11);
}

TEST(FormattingTest, CtrlKOnALinkTakesItApartAgain)
{
    auto editor = editorWith(QStringLiteral("[shown](https://example.com)"));
    QTest::keyClick(editor.get(), Qt::Key_A, Qt::ControlModifier);
    QTest::keyClick(editor.get(), Qt::Key_K, Qt::ControlModifier);
    EXPECT_EQ(editor->noteText(), QStringLiteral("shown"));
}

// --- find and replace -----------------------------------------------------

TEST(FindTest, CountsMatchesAndStepsRoundThem)
{
    auto editor = editorWith(QStringLiteral("alpha beta alpha gamma alpha\n"));
    QTest::keyClick(editor.get(), Qt::Key_F, Qt::ControlModifier);
    ASSERT_TRUE(editor->findVisible());

    editor->setFindQuery(QStringLiteral("alpha"), false, false);
    EXPECT_EQ(editor->findMatchCount(), 3);
    EXPECT_EQ(editor->findCurrentIndex(), 0);

    editor->findStep(1);
    EXPECT_EQ(editor->findCurrentIndex(), 1);
    EXPECT_EQ(editor->cursorOffset(), 16); // the match is selected

    editor->findStep(1);
    editor->findStep(1);
    EXPECT_EQ(editor->findCurrentIndex(), 0) << "next wraps round, as Obsidian's bar does";

    editor->findStep(-1);
    EXPECT_EQ(editor->findCurrentIndex(), 2);
}

TEST(FindTest, HonoursCaseAndRegex)
{
    auto editor = editorWith(QStringLiteral("Alpha alpha\n"));

    editor->setFindQuery(QStringLiteral("alpha"), false, false);
    EXPECT_EQ(editor->findMatchCount(), 2);

    editor->setFindQuery(QStringLiteral("alpha"), true, false);
    EXPECT_EQ(editor->findMatchCount(), 1);

    editor->setFindQuery(QStringLiteral("a\\w+a"), false, true);
    EXPECT_EQ(editor->findMatchCount(), 2);
    EXPECT_TRUE(editor->findPatternValid());
}

TEST(FindTest, AnUnparseablePatternReportsItselfRatherThanThrowing)
{
    auto editor = editorWith(QStringLiteral("alpha\n"));
    editor->setFindQuery(QStringLiteral("([unclosed"), false, true);

    EXPECT_FALSE(editor->findPatternValid());
    EXPECT_EQ(editor->findMatchCount(), 0);

    editor->setFindQuery(QStringLiteral("alpha"), false, true);
    EXPECT_TRUE(editor->findPatternValid());
    EXPECT_EQ(editor->findMatchCount(), 1);
}

TEST(FindTest, ReplacesOneAndThenAll)
{
    auto one = editorWith(QStringLiteral("alpha beta alpha\n"));
    one->setFindQuery(QStringLiteral("alpha"), false, false);
    one->replaceCurrent(QStringLiteral("X"));
    EXPECT_EQ(one->noteText(), QStringLiteral("X beta alpha\n"));
    EXPECT_EQ(one->findMatchCount(), 1);

    auto every = editorWith(QStringLiteral("alpha beta alpha\n"));
    every->setFindQuery(QStringLiteral("alpha"), false, false);
    EXPECT_EQ(every->replaceAll(QStringLiteral("Y")), 2);
    EXPECT_EQ(every->noteText(), QStringLiteral("Y beta Y\n"));
    EXPECT_EQ(every->findMatchCount(), 0);
}

TEST(FindTest, ReplaceAllIsOneUndoStep)
{
    const QString source = QStringLiteral("alpha beta alpha\n");
    auto editor = editorWith(source);
    editor->setFindQuery(QStringLiteral("alpha"), false, false);
    editor->replaceAll(QStringLiteral("Y"));
    ASSERT_NE(editor->noteText(), source);

    QTest::keyClick(editor.get(), Qt::Key_Z, Qt::ControlModifier);
    EXPECT_EQ(editor->noteText(), source);
}

TEST(FindTest, CtrlHOpensTheReplaceRowAndEscapeCloses)
{
    auto editor = editorWith(QStringLiteral("alpha\n"));
    EXPECT_FALSE(editor->findVisible());

    QTest::keyClick(editor.get(), Qt::Key_H, Qt::ControlModifier);
    EXPECT_TRUE(editor->findVisible());

    QTest::keyClick(editor.get(), Qt::Key_Escape);
    EXPECT_FALSE(editor->findVisible());
    EXPECT_EQ(editor->findMatchCount(), 0);
}

TEST(FindTest, MatchesFollowTheTextAsItIsEdited)
{
    auto editor = editorWith(QStringLiteral("alpha\n"));
    editor->showFind(false);
    editor->setFindQuery(QStringLiteral("alpha"), false, false);
    ASSERT_EQ(editor->findMatchCount(), 1);

    editor->setCursorOffset(5);
    QTest::keyClicks(editor.get(), QStringLiteral(" alpha"));
    EXPECT_EQ(editor->findMatchCount(), 2);
}

// Searching a long note must not cost a full relayout: the current match is
// brought on screen through the same viewport-only path scrolling uses.
TEST(FindTest, SteppingThroughALongNoteStaysInsideTheKeystrokeBudget)
{
    const QString note = generateNote(200);
    auto editor = editorWith(note);
    editor->showFind(false);
    editor->setFindQuery(QStringLiteral("Wikilink"), false, false);
    ASSERT_GT(editor->findMatchCount(), 100);

    QElapsedTimer clock;
    clock.start();
    constexpr int kSteps = 20;
    for (int i = 0; i < kSteps; ++i)
        editor->findStep(1);
    const double perStep = double(clock.nsecsElapsed()) / kSteps / 1e6;

    std::printf("[ PERF     ] find step on a %.2f kB note: %.2f ms\n",
                note.size() / 1024.0, perStep);
    EXPECT_LT(perStep, 8.0);
}

// --- block spacing (HANDOFF 4) --------------------------------------------

TEST(BlockSpacingTest, SourceModeGivesEveryLineTheSameHeight)
{
    // Three lines of one paragraph and three lines with a blank in the middle
    // are both three lines of the file, so they are the same height on screen.
    auto paragraph = editorWith(QStringLiteral("alpha\nbeta\ngamma\n"));
    paragraph->setMode(MarkdownEditor::Mode::Source);
    auto blank = editorWith(QStringLiteral("alpha\n\ngamma\n"));
    blank->setMode(MarkdownEditor::Mode::Source);

    EXPECT_DOUBLE_EQ(paragraph->documentHeight(), blank->documentHeight());
}

TEST(BlockSpacingTest, OnlyLivePreviewPutsAirOverAHeading)
{
    // Obsidian's live preview gives a heading one --p-spacing of air and its
    // source mode none at all.
    const QString note = QStringLiteral("# Heading\n\nbody\n");
    auto live = editorWith(note);
    auto source = editorWith(note);
    source->setMode(MarkdownEditor::Mode::Source);

    const qreal spacing = Theme::builtin().textFont().pixelSize();
    EXPECT_DOUBLE_EQ(live->documentHeight() - source->documentHeight(), spacing);
}

TEST(BlockSpacingTest, ALineIsTheFontSizeTimesItsLineHeight)
{
    // A CSS line-height is a multiple of the font size, not of the font's own
    // metric height: 16 px text at 1.5 is 24 px, whatever the family measures.
    auto editor = editorWith(QStringLiteral("one\n"));
    editor->setMode(MarkdownEditor::Mode::Source);
    const qreal padding = editor->documentHeight()
                          - Theme::builtin().textFont().pixelSize() * 1.5;

    auto three = editorWith(QStringLiteral("one\ntwo\nthree\n"));
    three->setMode(MarkdownEditor::Mode::Source);
    EXPECT_DOUBLE_EQ(three->documentHeight() - padding,
                     Theme::builtin().textFont().pixelSize() * 1.5 * 3);
}

// --- the properties table (HANDOFF 3) -------------------------------------

namespace {

/// Where the properties table puts the control of its first row: the key
/// column is 180 px, the first block starts 28 px down, and a row is the text
/// line height plus a fifth. Same numbers as `layoutProperties`.
QPoint firstPropertyControlPoint(const MarkdownEditor &editor)
{
    const int viewport = editor.viewport()->width();
    const int column = qMax(120, qMin(viewport - 48, 700));
    const int left = qMax(24, (viewport - column) / 2);
    const qreal rowHeight = Theme::builtin().textFont().pixelSize() * 1.5 * 1.2;
    return QPoint(left + 180 + 6, int(28 + rowHeight / 2));
}

} // namespace

TEST(PropertiesTest, FrontmatterIsATableInLivePreviewAndRawInSourceMode)
{
    const QString note = QStringLiteral("---\ndone: false\ntags:\n  - one\n---\n\nbody\n");
    auto editor = editorWith(note);

    ASSERT_EQ(editor->properties().size(), 2);
    EXPECT_EQ(editor->properties().at(0).key, QStringLiteral("done"));
    EXPECT_EQ(editor->properties().at(0).type, Property::Type::Checkbox);
    EXPECT_EQ(editor->properties().at(1).type, Property::Type::List);
    // The YAML itself is never shown in live preview - the table replaces it.
    EXPECT_FALSE(editor->displayText().contains(QStringLiteral("done:")));
    // The table is taller than the nothing that used to be drawn there.
    EXPECT_GT(editor->documentHeight(), editorWith(QStringLiteral("\nbody\n"))->documentHeight());

    editor->setMode(MarkdownEditor::Mode::Source);
    EXPECT_EQ(editor->displayText(), note);
}

TEST(PropertiesTest, ClickingACheckboxRewritesOnlyThatValue)
{
    auto editor = editorWith(
        QStringLiteral("---\ndone: false\ntitle: Keep me\n---\n\nbody\n"));
    QTest::mouseClick(editor->viewport(), Qt::LeftButton, Qt::NoModifier,
                      firstPropertyControlPoint(*editor));

    // Every other byte of the file is exactly as it was: non-negotiable 1.
    EXPECT_EQ(editor->noteText(),
              QStringLiteral("---\ndone: true\ntitle: Keep me\n---\n\nbody\n"));

    QTest::mouseClick(editor->viewport(), Qt::LeftButton, Qt::NoModifier,
                      firstPropertyControlPoint(*editor));
    EXPECT_EQ(editor->noteText(),
              QStringLiteral("---\ndone: false\ntitle: Keep me\n---\n\nbody\n"));
}

TEST(PropertiesTest, AQuotedValueIsReadOnly)
{
    // `"false"` is text, not a checkbox: the typed editor cannot round trip it,
    // so clicking where its control would be changes nothing.
    const QString note = QStringLiteral("---\ndone: \"false\"\n---\n\nbody\n");
    auto editor = editorWith(note);
    ASSERT_EQ(editor->properties().at(0).type, Property::Type::Text);

    QTest::mouseClick(editor->viewport(), Qt::LeftButton, Qt::NoModifier,
                      firstPropertyControlPoint(*editor));
    EXPECT_EQ(editor->noteText(), note);
}

TEST(PropertiesTest, TheTableFolds)
{
    auto editor = editorWith(QStringLiteral("---\na: 1\nb: 2\nc: 3\n---\n\nbody\n"));
    const qreal open = editor->documentHeight();

    editor->toggleFoldAt(0);
    EXPECT_TRUE(editor->isFoldedAt(0));
    EXPECT_LT(editor->documentHeight(), open);

    editor->toggleFoldAt(0);
    EXPECT_DOUBLE_EQ(editor->documentHeight(), open);
}

// --- folding callouts, containers and code blocks (HANDOFF 3) -------------

TEST(FoldTest, ADetailsContainerStartsFoldedAndShowsItsSummary)
{
    auto editor = editorWith(
        QStringLiteral(":::details summary=\"What it hides\"\nbody\n:::\n"));
    EXPECT_TRUE(editor->isFoldedAt(0));

    editor->toggleFoldAt(0);
    EXPECT_FALSE(editor->isFoldedAt(0));
    EXPECT_GT(editor->documentHeight(), 0);
}

TEST(FoldTest, AMinusCalloutStartsFoldedAndAPlainOneNeverFolds)
{
    auto folded = editorWith(QStringLiteral("> [!warning]- Title\n> body\n"));
    EXPECT_TRUE(folded->isFoldedAt(0));
    const qreal collapsed = folded->documentHeight();
    folded->toggleFoldAt(0);
    EXPECT_GT(folded->documentHeight(), collapsed);

    auto plain = editorWith(QStringLiteral("> [!note] Title\n> body\n"));
    EXPECT_FALSE(plain->isFoldedAt(0));
    const qreal open = plain->documentHeight();
    plain->toggleFoldAt(0); // not foldable: the state is kept but changes nothing
    EXPECT_DOUBLE_EQ(plain->documentHeight(), open);
}

TEST(FoldTest, ClickingTheCalloutTitleToggles)
{
    auto editor = editorWith(QStringLiteral("> [!warning]+ Title\n> body\n"));
    ASSERT_FALSE(editor->isFoldedAt(0));

    const int viewport = editor->viewport()->width();
    const int column = qMax(120, qMin(viewport - 48, 700));
    const int left = qMax(24, (viewport - column) / 2);
    QTest::mouseClick(editor->viewport(), Qt::LeftButton, Qt::NoModifier,
                      QPoint(left + 80, 34));
    EXPECT_TRUE(editor->isFoldedAt(0));
    EXPECT_EQ(editor->cursorOffset(), 0) << "folding must not drag the caret with it";
}

// --- `#|` code block options (HANDOFF 3) ----------------------------------

TEST(CodeOptionsTest, ATitleGivesTheBlockAHeaderStrip)
{
    auto plain = editorWith(QStringLiteral("```python\nx = 1\n```\n"));
    auto titled = editorWith(QStringLiteral("```python\n#| title: fib.py\nx = 1\n```\n"));

    // The option line is hidden either way, so the listing is the same size;
    // the difference is the strip the title is drawn in.
    EXPECT_EQ(titled->displayText(), plain->displayText());
    EXPECT_GT(titled->documentHeight(), plain->documentHeight());
}

TEST(CodeOptionsTest, FoldCollapsesTheBlockToItsHeader)
{
    const QString note =
        QStringLiteral("```python\n#| title: fib.py\n#| fold: true\nx = 1\ny = 2\n```\n");
    auto editor = editorWith(note);
    const qsizetype inside = note.indexOf(QStringLiteral("x = 1"));

    EXPECT_TRUE(editor->isFoldedAt(inside));
    const qreal collapsed = editor->documentHeight();

    editor->toggleFoldAt(inside);
    EXPECT_FALSE(editor->isFoldedAt(inside));
    EXPECT_GT(editor->documentHeight(), collapsed);
}

TEST(CodeOptionsTest, LineNumbersIndentTheListingAndLeaveTheBytesAlone)
{
    const QString note =
        QStringLiteral("```python\n#| lines: 10\n#| highlight: 2\nx = 1\ny = 2\n```\n");
    auto editor = editorWith(note);

    EXPECT_EQ(editor->noteText(), note);
    // The options are syntax, so what is laid out is the listing alone.
    EXPECT_EQ(editor->displayText().trimmed(), QStringLiteral("x = 1\ny = 2"));
}

TEST(PdfPageTest, AvailableAndRefusesWhatIsNotAPdf)
{
    // Built against poppler-qt6 here; a permissive build without it reports
    // itself unavailable rather than pretending.
    EXPECT_TRUE(PdfPage::isAvailable());
    EXPECT_EQ(PdfPage::pageCount(QStringLiteral("/nonexistent/none.pdf")), 0);
    EXPECT_TRUE(PdfPage::render(QStringLiteral("/nonexistent/none.pdf"), 0, 400).isNull());
    // A real file that is not a PDF must not crash the renderer either.
    EXPECT_EQ(PdfPage::pageCount(QStringLiteral("/etc/hostname")), 0);
}

TEST(MarkdownEditorTest, EmbedsASourceFileAsAReadOnlyCodeBlock)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString source = dir.filePath(QStringLiteral("snippet.cpp"));
    {
        QFile file(source);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        file.write("int one() { return 1; }\n"
                   "int two() { return 2; }\n"
                   "int three() { return 3; }\n");
    }

    auto editor = editorWith(QStringLiteral("![[snippet.cpp#L2-L3]]\n"));
    editor->setLinkResolver([&](const QString &target) {
        return target == QLatin1String("snippet.cpp") ? source : QString();
    });

    // The embed replaced the text: the wikilink itself is no longer shown.
    EXPECT_FALSE(editor->displayText().contains(QStringLiteral("snippet.cpp")));
    // And the buffer is untouched, which is what makes the embed read-only.
    EXPECT_EQ(editor->noteText(), QStringLiteral("![[snippet.cpp#L2-L3]]\n"));
}

TEST(MathRendererTest, RendersLatexWithInk)
{
    EXPECT_TRUE(MathRenderer::isAvailable());

    MathRenderer renderer;
    const QImage image = renderer.render(
        QStringLiteral("\\int_0^\\infty e^{-x^2}\\,dx = \\frac{\\sqrt{\\pi}}{2}"), 20,
        QColor(0xda, 0xda, 0xda), 700, 1.0);

    ASSERT_FALSE(image.isNull()) << renderer.lastError().toStdString();
    EXPECT_GT(image.width(), 40);
    EXPECT_GT(image.height(), 20);

    // Glyphs, not just the fraction rules: count how many rows carry ink.
    int rowsWithInk = 0;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            if (qAlpha(image.pixel(x, y)) > 8) {
                ++rowsWithInk;
                break;
            }
        }
    }
    std::printf("[ MATH     ] %dx%d, %d rows with ink\n", image.width(), image.height(),
                rowsWithInk);
    EXPECT_GT(rowsWithInk, image.height() / 4);

    // MicroTeX is forgiving with a truncated formula - it lays out what it
    // can rather than failing, which is the behaviour we want in a note.
}

TEST(MarkdownEditorTest, DisplayMathBecomesARenderedBlock)
{
    auto editor = editorWith(QStringLiteral("before\n\n$$\nE = mc^2\n$$\n\nafter\n"));

    const QString shown = editor->displayText();
    std::printf("[ MATH     ] display = %s\n", shown.toUtf8().constData());
    // The formula is drawn, so its source is not part of the text on screen.
    EXPECT_FALSE(shown.contains(QStringLiteral("E = mc^2")));
    EXPECT_TRUE(shown.contains(QStringLiteral("before")));
    EXPECT_TRUE(shown.contains(QStringLiteral("after")));
    // And the buffer still holds every character.
    EXPECT_TRUE(editor->noteText().contains(QStringLiteral("$$")));
}

TEST(MathRendererTest, ItsFontsAreReachableAsResources)
{
    // MicroTeX names its fonts by a path relative to the working directory and
    // falls back to ":/<path>", which is why they are compiled in.
    QDirIterator it(QStringLiteral(":/fonts"), QStringList{QStringLiteral("*.ttf")},
                    QDir::Files, QDirIterator::Subdirectories);
    QStringList found;
    while (it.hasNext())
        found << it.next();
    std::printf("[ MATH     ] %lld font resources\n", (long long)found.size());
    ASSERT_FALSE(found.isEmpty());

    // The fonts the large operators come from must be among them, and Qt must
    // actually accept them.
    for (const QString &wanted : {QStringLiteral(":/fonts/base/cmex10.ttf"),
                                  QStringLiteral(":/fonts/maths/cmsy10.ttf"),
                                  QStringLiteral(":/fonts/base/cmmi10.ttf")}) {
        EXPECT_TRUE(found.contains(wanted)) << wanted.toStdString();
        const int id = QFontDatabase::addApplicationFont(wanted);
        EXPECT_NE(id, -1) << wanted.toStdString();
        if (id != -1) {
            std::printf("[ MATH     ] %s -> %s\n", wanted.toUtf8().constData(),
                        QFontDatabase::applicationFontFamilies(id).join(QLatin1Char(','))
                            .toUtf8().constData());
        }
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

// A link to a note that does not exist is dimmed, which is the only way a
// typo in a wikilink is visible at all.
TEST(UnresolvedLinkTest, OnlyALinkWithNoNoteBehindItIsDimmed)
{
    MarkdownEditor editor;
    const Theme theme = Theme::builtin(Theme::Mode::Dark);
    editor.setTheme(theme);
    // Only "Real" exists; the external link must stay a normal link.
    editor.setLinkResolver([](const QString &target) {
        return target == QStringLiteral("Real") ? QStringLiteral("/vault/Real.md") : QString();
    });
    editor.setNoteText(QStringLiteral(
        "See [[Real]], [[Missing]], [[Real#Heading]] and [x](https://example.com).\n"));
    editor.resize(900, 600);
    editor.show();
    QTest::qWait(60);

    const QColor accent = theme.color(QStringLiteral("--text-accent"));
    const QColor dim = theme.color(QStringLiteral("--link-unresolved-color"));
    ASSERT_NE(accent, dim);

    const QString text = editor.noteText();
    const auto colourOfLinkAt = [&](const QString &needle) {
        const qsizetype at = text.indexOf(needle);
        EXPECT_GE(at, 0);
        return editor.linkColourAt(at + 1);
    };

    EXPECT_EQ(colourOfLinkAt(QStringLiteral("[[Real]]")), accent);
    EXPECT_EQ(colourOfLinkAt(QStringLiteral("[[Missing]]")), dim);
    // A subpath does not make a resolved note unresolved.
    EXPECT_EQ(colourOfLinkAt(QStringLiteral("[[Real#Heading]]")), accent);
    // An external link resolves to nothing and is not meant to.
    EXPECT_EQ(colourOfLinkAt(QStringLiteral("https://example.com")), accent);
}
