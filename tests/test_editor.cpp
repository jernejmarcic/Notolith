#include <gtest/gtest.h>

#include <QClipboard>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QRandomGenerator>
#include <QSignalSpy>
#include <QScrollBar>
#include <QScreen>

#include <algorithm>
#include <cstdio>
#include <cstring>

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
            if (!map.isHidden(s)) {
                ASSERT_EQ(map.toSource(map.toDisplay(s)), s) << "case " << c << " source " << s;
            }
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
#include "Parser.h"
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

/// An editor whose parse has landed. Above kAsyncParseThreshold the parse runs
/// on the thread pool and the note is plain paragraphs until it comes back, so
/// a timing taken before then measures the placeholder: two guards once did.
/// Null if the parse never returns.
std::unique_ptr<MarkdownEditor> parsedEditorWith(const QString &text)
{
    auto editor = std::make_unique<MarkdownEditor>();
    editor->resize(900, 600);
    QSignalSpy parsed(editor.get(), &MarkdownEditor::wordCountChanged);
    editor->setNoteText(text);
    // A small note parses before setNoteText returns. A big one sends -1 at
    // once, "not counted yet", and its real count with the parse.
    while (!parsed.isEmpty() && parsed.last().at(0).toInt() < 0) {
        if (!parsed.wait(10000))
            return nullptr;
    }
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
    // Counted with the parse, after the typing pause, not per keystroke.
    QTest::qWait(80);
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
    auto editor = parsedEditorWith(note);
    ASSERT_TRUE(editor) << "the parse never came back";
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

TEST(MarkdownEditorTest, OpeningAMegabyteNoteIsImmediate)
{
    // Above kAsyncParseThreshold a note is shown before it is parsed: the text
    // goes up as plain paragraphs and the parse runs on the thread pool. So
    // what this measures is the time to something readable, which is what the
    // reader waits for. The parser's own cost is guarded separately, by
    // ParserPerfTest::ParsingStaysLinearInFileSize in test_markdown.cpp.
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

    // Showing five times the text must not cost five times the wait, because
    // almost none of this is proportional to the file any more.
    EXPECT_LT(timeRatio, sizeRatio);
    EXPECT_LT(largeMs, 60.0);
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
    if (!CodeHighlighter::isAvailable())
        GTEST_SKIP() << "built without KF6 SyntaxHighlighting";

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
    if (!CodeHighlighter::isAvailable())
        GTEST_SKIP() << "built without KF6 SyntaxHighlighting";

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

    // Selected text that is not a URL becomes the target. Which form that
    // takes is links.useWikilinks, which defaults to on as it does in
    // Obsidian, so the wikilink is the default answer here.
    auto wiki = editorWith(QStringLiteral("click me"));
    QTest::keyClick(wiki.get(), Qt::Key_A, Qt::ControlModifier);
    QTest::keyClick(wiki.get(), Qt::Key_K, Qt::ControlModifier);
    EXPECT_EQ(wiki->noteText(), QStringLiteral("[[click me]]"));
    EXPECT_EQ(wiki->cursorOffset(), 10) << "a wikilink has no second slot to fill";

    auto text = editorWith(QStringLiteral("click me"));
    text->setUseWikilinks(false);
    QTest::keyClick(text.get(), Qt::Key_A, Qt::ControlModifier);
    QTest::keyClick(text.get(), Qt::Key_K, Qt::ControlModifier);
    EXPECT_EQ(text->noteText(), QStringLiteral("[click me]()"));
    EXPECT_EQ(text->cursorOffset(), 11);

    // An empty selection opens whichever form the vault prefers.
    auto empty = editorWith(QString());
    QTest::keyClick(empty.get(), Qt::Key_K, Qt::ControlModifier);
    EXPECT_EQ(empty->noteText(), QStringLiteral("[[]]"));
    EXPECT_EQ(empty->cursorOffset(), 2);
}

TEST(EditorSettingsTest, TabWidthStrictBreaksAndGuidesAllApply)
{
    // Each of these was shown on the settings page and stored, with nothing
    // reading it. The point of the test is that they now reach the editor.
    auto editor = editorWith(QStringLiteral("one\ntwo\n\nthree\n"));

    // Off, a single newline inside a paragraph joins, which is Obsidian's
    // default. On, CommonMark's rule applies and it breaks.
    EXPECT_FALSE(editor->strictLineBreaks());
    editor->setStrictLineBreaks(true);
    EXPECT_TRUE(editor->strictLineBreaks());
    editor->setStrictLineBreaks(false);

    editor->setTabWidth(2);
    EXPECT_EQ(editor->tabWidth(), 2);
    editor->setCursorOffset(0);
    QTest::keyClick(editor.get(), Qt::Key_Tab);
    EXPECT_TRUE(editor->noteText().startsWith(QStringLiteral("  one")))
        << "Tab inserts tabWidth spaces, not a hardcoded four";

    // Out-of-range values are clamped rather than taken literally.
    editor->setTabWidth(99);
    EXPECT_EQ(editor->tabWidth(), 8);
    editor->setTabWidth(0);
    EXPECT_EQ(editor->tabWidth(), 1);

    EXPECT_FALSE(editor->indentGuides());
    editor->setIndentGuides(true);
    EXPECT_TRUE(editor->indentGuides());
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
    // Available exactly when the build found poppler-qt6; a build without it -
    // a permissive one, or one for a phone - reports itself unavailable rather
    // than pretending. The rest holds either way.
#ifdef NOTEAPPC_EXPECT_PDF
    EXPECT_TRUE(PdfPage::isAvailable());
#else
    EXPECT_FALSE(PdfPage::isAvailable());
#endif
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

TEST(MarkdownEditorTest, InlineMathRendersAsOneCharacterWhenNotActive)
{
    ASSERT_TRUE(MathRenderer::isAvailable());
    const QString source = QStringLiteral("Inline $e^{i\\pi}+1=0$ formula.\n");
    auto editor = editorWith(source);

    // Everything the formula's image now stands for - the LaTeX and the
    // closing "$" - is gone from the display string; only the opening "$"
    // that anchors the image remains.
    EXPECT_EQ(editor->displayText(), QStringLiteral("Inline $ formula.\n"));
}

TEST(MarkdownEditorTest, InlineMathShowsSourceWhenTheCaretIsInIt)
{
    const QString source = QStringLiteral("Inline $e^{i\\pi}+1=0$ formula.\n");
    auto editor = editorWith(source);

    // Only the element the caret is in shows its source.
    editor->enterBlockAt(source.indexOf(QLatin1String("pi")));
    EXPECT_EQ(editor->displayText(), source);

    // Elsewhere in the same paragraph the formula stays drawn.
    editor->enterBlockAt(source.indexOf(QLatin1String("formula")));
    EXPECT_FALSE(editor->displayText().contains(QLatin1String("\\pi")));
}

TEST(MarkdownEditorTest, InlineMathShowsSourceInSourceMode)
{
    const QString source = QStringLiteral("Inline $e^{i\\pi}+1=0$ formula.\n");
    auto editor = editorWith(source);

    editor->setMode(MarkdownEditor::Mode::Source);
    EXPECT_EQ(editor->displayText(), source);
}

TEST(MarkdownEditorTest, UnrenderableInlineMathKeepsItsSourceVisible)
{
    // MicroTeX forgives nearly everything - an unknown command, an unclosed
    // brace, a dangling ^ all still render. A bare `&` outside an array is one
    // of the few things it refuses, and a real mistake to make in prose maths.
    // It must fall back to the raw source rather than to an empty gap, the
    // same rule display maths follows.
    const QString source = QStringLiteral("Inline $a & b$ formula.\n");
    auto editor = editorWith(source);

    EXPECT_EQ(editor->displayText(), source);
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

// --- what a selection reveals ----------------------------------------------

namespace {

/// A point on the first line of the first block, `dx` pixels into the text.
QPoint firstLinePoint(const MarkdownEditor &editor, int dx)
{
    const int viewport = editor.viewport()->width();
    const int column = qMax(120, qMin(viewport - 48, 700));
    const int left = qMax(24, (viewport - column) / 2);
    const qreal fontHeight = QFontMetricsF(Theme::builtin().textFont()).height();
    return QPoint(left + dx, int(28 + fontHeight / 2));
}

/// Mouse events sent the way Qt Widgets delivers them, so a test sees the same
/// sequence a real double-click produces: press, release, double-click,
/// release - the second press arrives as the double-click, not as a press.
void send(QWidget *target, QEvent::Type type, QPoint at, Qt::MouseButtons held)
{
    const Qt::MouseButton button =
        type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton;
    QMouseEvent event(type, QPointF(at), target->mapToGlobal(QPointF(at)), button, held,
                      Qt::NoModifier);
    QCoreApplication::sendEvent(target, &event);
}

const QString kRevealNote = QStringLiteral("Some **bold** words to select across.\n\nSecond.\n");

} // namespace

TEST(RevealTest, DraggingASelectionNeverRevealsSyntax)
{
    // Obsidian keeps rendering while a selection is dragged. Revealing the
    // markers mid-drag moved the text out from under the pointer.
    auto editor = editorWith(kRevealNote);
    const QString rendered = editor->displayText();
    ASSERT_FALSE(rendered.contains(QLatin1String("**")));

    QWidget *viewport = editor->viewport();
    send(viewport, QEvent::MouseButtonPress, firstLinePoint(*editor, 4), Qt::LeftButton);
    EXPECT_EQ(editor->displayText(), rendered) << "nothing may move while the button is down";
    send(viewport, QEvent::MouseMove, firstLinePoint(*editor, 180), Qt::LeftButton);
    EXPECT_EQ(editor->displayText(), rendered);
    send(viewport, QEvent::MouseButtonRelease, firstLinePoint(*editor, 180), Qt::NoButton);

    EXPECT_FALSE(editor->selectedText().isEmpty());
    EXPECT_EQ(editor->displayText(), rendered) << "a finished selection stays rendered too";
}

TEST(RevealTest, APlainClickRevealsOnRelease)
{
    auto editor = editorWith(kRevealNote);
    const QString rendered = editor->displayText();

    // Aim inside "bold": a caret shows the markers of the element it is in.
    const qreal advance = QFontMetricsF(Theme::builtin().textFont())
                              .horizontalAdvance(rendered.left(rendered.indexOf(QLatin1String("bold")) + 2));
    const QPoint onBold = firstLinePoint(*editor, int(advance));
    QWidget *viewport = editor->viewport();
    send(viewport, QEvent::MouseButtonPress, onBold, Qt::LeftButton);
    EXPECT_EQ(editor->displayText(), rendered);
    send(viewport, QEvent::MouseButtonRelease, onBold, Qt::NoButton);
    EXPECT_TRUE(editor->displayText().contains(QLatin1String("**")))
        << "a bare caret in an element shows that element's markers";
}

TEST(RevealTest, AKeyboardSelectionRendersAgain)
{
    auto editor = editorWith(kRevealNote);
    const QString rendered = editor->displayText();
    editor->enterBlockAt(kRevealNote.indexOf(QLatin1String("bold")) + 1);
    ASSERT_TRUE(editor->displayText().contains(QLatin1String("**")));

    QTest::keyClick(editor.get(), Qt::Key_End, Qt::ShiftModifier);
    EXPECT_FALSE(editor->selectedText().isEmpty());
    EXPECT_EQ(editor->displayText(), rendered);
}

TEST(RevealTest, DoubleClickSelectsTheWordThatWasAimedAt)
{
    // The first click's release reveals the block, which shifts everything
    // after the markers four characters to the right. The word selected has
    // to be the one under the pointer before that happened.
    auto editor = editorWith(kRevealNote);
    const QString rendered = editor->displayText();
    const qsizetype wordsAt = rendered.indexOf(QLatin1String("words"));
    ASSERT_GT(wordsAt, 0);
    const qreal advance = QFontMetricsF(Theme::builtin().textFont())
                              .horizontalAdvance(rendered.left(wordsAt + 2));
    const QPoint onWords = firstLinePoint(*editor, int(advance));

    QWidget *viewport = editor->viewport();
    send(viewport, QEvent::MouseButtonPress, onWords, Qt::LeftButton);
    send(viewport, QEvent::MouseButtonRelease, onWords, Qt::NoButton);
    send(viewport, QEvent::MouseButtonDblClick, onWords, Qt::LeftButton);
    send(viewport, QEvent::MouseButtonRelease, onWords, Qt::NoButton);

    EXPECT_EQ(editor->selectedText(), QStringLiteral("words"));
}

// --- style files reload live ----------------------------------------------

namespace {

/// Pixels of exactly this colour in the editor as it would paint now. The
/// callout rail is the only thing drawn in a pure primary, so counting its
/// pixels is how a test sees which style the editor resolved.
int pixelsOf(MarkdownEditor &editor, QRgb colour)
{
    const QImage frame = editor.grab().toImage().convertToFormat(QImage::Format_RGB32);
    int count = 0;
    for (int y = 0; y < frame.height(); ++y) {
        const QRgb *row = reinterpret_cast<const QRgb *>(frame.constScanLine(y));
        for (int x = 0; x < frame.width(); ++x)
            count += (row[x] & 0xFFFFFF) == (colour & 0xFFFFFF);
    }
    return count;
}

void writeAtomically(const QString &path, const QByteArray &bytes)
{
    // Write a new file and rename it over the old one, the way most editors
    // save. inotify drops a watch on the renamed-over file, which is exactly
    // the case the reload has to survive.
    const QString temporary = path + QStringLiteral(".new");
    QFile file(temporary);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    file.write(bytes);
    file.close();
    QFile::remove(path);
    ASSERT_TRUE(QFile::rename(temporary, path));
}

} // namespace

TEST(StyleFilesTest, EditingCalloutsIniRestylesWithoutARestart)
{
    QTemporaryDir home;
    ASSERT_TRUE(home.isValid());
    const QByteArray previous = qgetenv("XDG_CONFIG_HOME");
    qputenv("XDG_CONFIG_HOME", home.path().toUtf8());
    ASSERT_TRUE(QDir(home.path()).mkpath(QStringLiteral("noteAppC")));
    const QString ini = QDir(home.path()).filePath(QStringLiteral("noteAppC/callouts.ini"));
    writeAtomically(ini, "[note]\nbar = #FF0000ff\n");

    auto editor = editorWith(QStringLiteral("> [!note] Title\n> Body.\n"));
    EXPECT_GT(pixelsOf(*editor, qRgb(255, 0, 0)), 0) << "the file's colour at start";

    writeAtomically(ini, "[note]\nbar = #0000FFff\n");
    QElapsedTimer waited;
    waited.start();
    while (pixelsOf(*editor, qRgb(0, 0, 255)) == 0 && waited.elapsed() < 3000)
        QTest::qWait(20);
    EXPECT_GT(pixelsOf(*editor, qRgb(0, 0, 255)), 0) << "reloaded after a rename-over save";
    EXPECT_EQ(pixelsOf(*editor, qRgb(255, 0, 0)), 0) << "and the old colour is gone";

    // A second save still reaches it: the rename took the file's watch away,
    // and the reload has to have put it back.
    writeAtomically(ini, "[note]\nbar = #00FF00ff\n");
    waited.restart();
    while (pixelsOf(*editor, qRgb(0, 255, 0)) == 0 && waited.elapsed() < 3000)
        QTest::qWait(20);
    EXPECT_GT(pixelsOf(*editor, qRgb(0, 255, 0)), 0);

    qputenv("XDG_CONFIG_HOME", previous);
}

// --- the code block label -------------------------------------------------

namespace {

/// Clicks across the top-right of the first code block until something lands
/// on the clipboard. Sweeping, rather than computing the label's rectangle,
/// keeps the test from restating the editor's spacing - it only asserts that
/// the label is up there and does the right thing when hit.
bool clickLabelUntilCopied(MarkdownEditor &editor)
{
    QGuiApplication::clipboard()->clear();
    const int viewport = editor.viewport()->width();
    const int column = qMax(120, qMin(viewport - 48, 700));
    const int left = qMax(24, (viewport - column) / 2);
    const int right = left + column + 4;
    for (int y = 12; y < 110; y += 4) {
        for (int x = right - 6; x > right - 140; x -= 6) {
            QTest::mouseClick(editor.viewport(), Qt::LeftButton, Qt::NoModifier, QPoint(x, y));
            if (!QGuiApplication::clipboard()->text().isEmpty())
                return true;
        }
    }
    return false;
}

} // namespace

TEST(CodeLabelTest, ClickingTheLanguageCopiesTheListing)
{
    const QString note = QStringLiteral("```python\nprint(1)\nprint(2)\n```\n\nafter\n");
    auto editor = editorWith(note);
    ASSERT_TRUE(clickLabelUntilCopied(*editor)) << "no label in the block's top-right corner";

    // The listing as the reader sees it: no fences, no language tag, and no
    // trailing newline dragged along from before the closing fence.
    EXPECT_EQ(QGuiApplication::clipboard()->text(), QStringLiteral("print(1)\nprint(2)"));
    EXPECT_EQ(editor->noteText(), note) << "copying must not touch the note";
}

TEST(CodeLabelTest, OptionLinesAreNotCopied)
{
    auto editor = editorWith(
        QStringLiteral("```python\n#| title: Setup\nprint(1)\n```\n\nafter\n"));
    ASSERT_TRUE(clickLabelUntilCopied(*editor)) << "no label in the header strip";
    EXPECT_EQ(QGuiApplication::clipboard()->text(), QStringLiteral("print(1)"));
}

// --- folding a callout inside a callout -------------------------------------

namespace {

/// Where the parser says the nested callout starts - the key its fold state
/// lives under - rather than an offset the test would have to guess.
qsizetype nestedCalloutStart(const QString &text)
{
    const Decorations decorations = Parser().parseAll(text).decorations;
    for (const CalloutSpan &callout : decorations.callouts) {
        if (callout.depth > 0)
            return callout.range.start;
    }
    return -1;
}

} // namespace

TEST(NestedCalloutTest, AMinusStartsFoldedAndOpensAgain)
{
    // The empty `>` line is what steps back out to the outer callout. Without
    // it, `> Back out.` would be a lazy continuation of the inner paragraph -
    // CommonMark's rule, which md4qt and Obsidian both follow - and folding
    // would rightly hide it with the rest of the inner callout.
    const QString note = QStringLiteral(
        "> [!tip] Outer\n> Outer body.\n> > [!info]- Inner\n> > Hidden body.\n>\n> Back out.\n");
    auto editor = editorWith(note);
    const qsizetype inner = nestedCalloutStart(note);
    ASSERT_GE(inner, 0);

    EXPECT_TRUE(editor->isFoldedAt(inner));
    EXPECT_FALSE(editor->displayText().contains(QLatin1String("Hidden body")));
    EXPECT_TRUE(editor->displayText().contains(QLatin1String("Inner")))
        << "the title stays, to be clicked open";
    EXPECT_TRUE(editor->displayText().contains(QLatin1String("Back out")))
        << "folding the inner callout must not swallow the outer one's text";

    editor->toggleFoldAt(inner);
    EXPECT_FALSE(editor->isFoldedAt(inner));
    EXPECT_TRUE(editor->displayText().contains(QLatin1String("Hidden body")));
    EXPECT_EQ(editor->noteText(), note) << "folding is a view, never an edit";
}

TEST(NestedCalloutTest, APlusStartsOpenAndFolds)
{
    const QString note = QStringLiteral(
        "> [!tip] Outer\n> Outer body.\n> > [!info]+ Inner\n> > Shown body.\n");
    auto editor = editorWith(note);
    const qsizetype inner = nestedCalloutStart(note);
    ASSERT_GE(inner, 0);

    EXPECT_FALSE(editor->isFoldedAt(inner));
    EXPECT_TRUE(editor->displayText().contains(QLatin1String("Shown body")));
    editor->toggleFoldAt(inner);
    EXPECT_FALSE(editor->displayText().contains(QLatin1String("Shown body")));
}

namespace {

/// Pixels in a code block label's corner - the top right of the first block -
/// where one channel (0 red, 1 green, 2 blue) clearly outweighs the other two.
/// Not an exact colour match: the label is antialiased text, and only a few of
/// its pixels ever land on the exact colour - how many depends on the state of
/// Qt's glyph cache, which is why an exact count passed alone and failed after
/// other tests had drawn text first.
int labelPixelsDominatedBy(MarkdownEditor &editor, int channel)
{
    const QImage frame = editor.grab().toImage().convertToFormat(QImage::Format_RGB32);
    int count = 0;
    for (int y = 0; y < qMin(frame.height(), 110); ++y) {
        const QRgb *row = reinterpret_cast<const QRgb *>(frame.constScanLine(y));
        for (int x = frame.width() * 3 / 5; x < frame.width(); ++x) {
            const int c[3] = {qRed(row[x]), qGreen(row[x]), qBlue(row[x])};
            if (c[channel] - qMax(c[(channel + 1) % 3], c[(channel + 2) % 3]) > 70)
                ++count;
        }
    }
    return count;
}

} // namespace

TEST(CodeLabelTest, TheFenceItselfOverridesCodeblocksIni)
{
    QTemporaryDir home;
    ASSERT_TRUE(home.isValid());
    const QByteArray previous = qgetenv("XDG_CONFIG_HOME");
    qputenv("XDG_CONFIG_HOME", home.path().toUtf8());
    ASSERT_TRUE(QDir(home.path()).mkpath(QStringLiteral("noteAppC")));
    writeAtomically(QDir(home.path()).filePath(QStringLiteral("noteAppC/codeblocks.ini")),
                    "[python]\nname = Python\ncolor = #0000FFff\n");

    // The file's colour, then the fence's own, the same three layers a
    // callout has: the language, codeblocks.ini, the block itself.
    auto fromFile = editorWith(QStringLiteral("```python\nx = 1\n```\n"));
    EXPECT_GT(labelPixelsDominatedBy(*fromFile, 2), 3);

    auto overridden = editorWith(QStringLiteral("```python color=#FF0000ff name=Setup\nx = 1\n```\n"));
    EXPECT_GT(labelPixelsDominatedBy(*overridden, 0), 3);
    EXPECT_EQ(labelPixelsDominatedBy(*overridden, 2), 0);
    EXPECT_FALSE(overridden->displayText().contains(QLatin1String("color=")))
        << "the attributes are hidden with the fence";

    qputenv("XDG_CONFIG_HOME", previous);
}

TEST(ShiftTest, ACalloutBelowTheCaretKeepsItsPanelWhileTyping)
{
    // A guard, not a regression that happened. Between a keystroke and the
    // debounced reparse only syntax, styles, blocks and properties are
    // shifted; callout ranges are not. It holds anyway because a block below
    // the edit keeps its cached layout and its pointer to the callout, so the
    // stale range is never consulted - unless that block is laid out again in
    // those 20 ms. If a change ever relays out every block per keystroke,
    // this is what will notice the callouts vanishing.
    QTemporaryDir home;
    ASSERT_TRUE(home.isValid());
    const QByteArray previous = qgetenv("XDG_CONFIG_HOME");
    qputenv("XDG_CONFIG_HOME", home.path().toUtf8());
    ASSERT_TRUE(QDir(home.path()).mkpath(QStringLiteral("noteAppC")));
    writeAtomically(QDir(home.path()).filePath(QStringLiteral("noteAppC/callouts.ini")),
                    "[note]\nbar = #FF0000ff\n");

    auto editor = editorWith(QStringLiteral("Typing here.\n\n> [!note] Title\n> Body.\n"));
    ASSERT_GT(pixelsOf(*editor, qRgb(255, 0, 0)), 0);

    editor->enterBlockAt(0);
    QTest::keyClicks(editor.get(), QStringLiteral("abc"));
    // Straight away, before the parse has had its 20 ms.
    EXPECT_GT(pixelsOf(*editor, qRgb(255, 0, 0)), 0) << "the callout's rail vanished mid-typing";

    qputenv("XDG_CONFIG_HOME", previous);
}

TEST(ShiftTest, TypingDoesNotRunIntoTheNextBlock)
{
    // The edited block's end was shifted twice, once by shiftRanges and once
    // more by applyEdit, so until the reparse it showed the start of the next
    // block as well - a flicker of duplicated text on every keystroke.
    const QString note = QStringLiteral("abc\n\ndef **x**\n");
    auto editor = editorWith(note);
    editor->enterBlockAt(1);
    editor->grab();
    for (int i = 0; i < 5; ++i) {
        QTest::keyClick(editor.get(), Qt::Key_Y);
        editor->grab();
    }
    // Before the parse has had its 20 ms.
    EXPECT_EQ(editor->displayText(), QStringLiteral("ayyyyybc\n\ndef x\n"));
}

TEST(ShiftTest, TheNextItemKeepsItsBulletWhileTyping)
{
    // List items are found by a binary search on their starts, so they have
    // to move with the text: a stale start would leave every item after the
    // edit out of its block, and its bullet undrawn, until the reparse.
    auto editor = editorWith(QStringLiteral("- one\n- two\n- three\n"));
    editor->enterBlockAt(3);
    const QImage before = editor->grab().toImage();
    // Longer than an item, or a stale slice can pick up the item after the
    // one it lost and draw its bullet in the same place.
    QTest::keyClicks(editor.get(), QStringLiteral("abcdefghijkl"));
    const QImage after = editor->grab().toImage(); // before the 20 ms reparse
    // Only the edited line may change; the rows of the other two items, their
    // bullets included, must come out exactly as they were.
    ASSERT_EQ(before.size(), after.size());
    int first = -1;
    int last = -1;
    for (int y = 0; y < before.height(); ++y) {
        if (std::memcmp(before.constScanLine(y), after.constScanLine(y),
                        size_t(before.bytesPerLine()))
            != 0) {
            if (first < 0)
                first = y;
            last = y;
        }
    }
    ASSERT_GE(first, 0) << "typing drew nothing";
    EXPECT_LT(last - first, 30) << "rows below the edited line changed";
}

// --- code block tabs ---------------------------------------------------------

namespace {

const QString kTabbedNote = QStringLiteral(
    "Before.\n\n"
    "```python tab=\"Python\"\nprint('hi')\n```\n\n"
    "```cpp tab\nputs(\"hi\");\n```\n\n"
    "After.\n");

} // namespace

TEST(CodeTabTest, OnlyTheSelectedTabIsShown)
{
    auto editor = editorWith(kTabbedNote);
    const QString shown = editor->displayText();
    EXPECT_TRUE(shown.contains(QLatin1String("print('hi')")));
    EXPECT_FALSE(shown.contains(QLatin1String("puts(\"hi\")")));
    EXPECT_TRUE(shown.contains(QLatin1String("Before.")));
    EXPECT_TRUE(shown.contains(QLatin1String("After.")))
        << "the group hides its own fences, nothing around it";

    editor->selectCodeTab(kTabbedNote.indexOf(QLatin1String("```python")), 1);
    const QString second = editor->displayText();
    EXPECT_FALSE(second.contains(QLatin1String("print('hi')")));
    EXPECT_TRUE(second.contains(QLatin1String("puts(\"hi\")")));
    EXPECT_EQ(editor->noteText(), kTabbedNote) << "switching tabs is a view, never an edit";
}

TEST(CodeTabTest, TheCaretInsideTheGroupShowsEveryFence)
{
    // So the caret can never be inside a tab nobody can see.
    auto editor = editorWith(kTabbedNote);
    editor->enterBlockAt(kTabbedNote.indexOf(QLatin1String("print")));
    const QString shown = editor->displayText();
    EXPECT_TRUE(shown.contains(QLatin1String("print('hi')")));
    EXPECT_TRUE(shown.contains(QLatin1String("puts(\"hi\")")));
}

TEST(CodeTabTest, SourceModeShowsEveryFence)
{
    auto editor = editorWith(kTabbedNote);
    editor->setMode(MarkdownEditor::Mode::Source);
    EXPECT_EQ(editor->displayText(), kTabbedNote);
}

TEST(CodeTabTest, TheSelectionSurvivesTypingAboveTheGroup)
{
    // The chosen tab is keyed by the group's position, which moves with the
    // text - and survives the reparse that follows.
    auto editor = editorWith(kTabbedNote);
    editor->selectCodeTab(kTabbedNote.indexOf(QLatin1String("```python")), 1);
    editor->enterBlockAt(0);
    QTest::keyClicks(editor.get(), QStringLiteral("More "));
    QTest::qWait(80); // past the parse debounce
    editor->enterBlockAt(editor->noteText().indexOf(QLatin1String("After.")));
    const QString shown = editor->displayText();
    EXPECT_TRUE(shown.contains(QLatin1String("puts(\"hi\")")));
    EXPECT_FALSE(shown.contains(QLatin1String("print('hi')")));
}

// --- revealing per element, as Obsidian does ---------------------------------

TEST(PerElementRevealTest, OnlyTheElementUnderTheCaretShowsItsMarkers)
{
    const QString note = QStringLiteral("One **first** and **second** and `code`.\n");
    auto editor = editorWith(note);

    editor->enterBlockAt(note.indexOf(QLatin1String("first")) + 1);
    EXPECT_EQ(editor->displayText(), QStringLiteral("One **first** and second and code.\n"));

    editor->enterBlockAt(note.indexOf(QLatin1String("code")) + 1);
    EXPECT_EQ(editor->displayText(), QStringLiteral("One first and second and `code`.\n"));

    // Plain text between elements reveals nothing at all.
    editor->enterBlockAt(note.indexOf(QLatin1String(" and ")) + 2);
    EXPECT_EQ(editor->displayText(), QStringLiteral("One first and second and code.\n"));
}

TEST(PerElementRevealTest, TouchingAnElementsEdgeCounts)
{
    // A caret just after the closing marker is still at the element, which is
    // where it sits after typing the last character of it.
    const QString note = QStringLiteral("A **bold** word.\n");
    auto editor = editorWith(note);
    editor->enterBlockAt(note.indexOf(QLatin1String("** word")) + 2);
    EXPECT_TRUE(editor->displayText().contains(QLatin1String("**bold**")));
}

TEST(PerElementRevealTest, NestedMarkersJoinTheirElement)
{
    const QString note = QStringLiteral("Very ***strong*** text.\n");
    auto editor = editorWith(note);
    editor->enterBlockAt(note.indexOf(QLatin1String("strong")) + 2);
    EXPECT_TRUE(editor->displayText().contains(QLatin1String("***strong***")));
}

TEST(PerElementRevealTest, AHeadingShowsItsHashOnItsOwnLine)
{
    const QString note = QStringLiteral("# Title\n\nBody.\n");
    auto editor = editorWith(note);
    editor->enterBlockAt(3);
    EXPECT_TRUE(editor->displayText().startsWith(QLatin1String("# Title")));
}

TEST(PerElementRevealTest, AListKeepsItsBulletsWhileTheCaretIsInTheText)
{
    const QString note = QStringLiteral("- one **x**\n- two\n");
    auto editor = editorWith(note);
    editor->enterBlockAt(note.indexOf(QLatin1String("one")) + 1);
    EXPECT_EQ(editor->displayText(), QStringLiteral("one x\ntwo\n"))
        << "markers stay hidden and drawn as bullets";

    editor->enterBlockAt(note.indexOf(QLatin1String("x")));
    EXPECT_EQ(editor->displayText(), QStringLiteral("one **x**\ntwo\n"));
}

TEST(PerElementRevealTest, ALinkShowsItsTargetOnlyWithTheCaretInIt)
{
    const QString note = QStringLiteral("See [the docs](https://example.org) here.\n");
    auto editor = editorWith(note);
    editor->enterBlockAt(note.indexOf(QLatin1String("docs")));
    EXPECT_TRUE(editor->displayText().contains(QLatin1String("[the docs](https://example.org)")));
    editor->enterBlockAt(note.indexOf(QLatin1String("here")));
    EXPECT_EQ(editor->displayText(), QStringLiteral("See the docs here.\n"));
}

TEST(PerElementRevealTest, ACalloutStillShowsItsWholeSource)
{
    // Callouts, tables and code are widgets in Obsidian: clicking into one
    // shows all of its source, not one element of it.
    const QString note = QStringLiteral("> [!note] Title\n> Some **bold** text.\n");
    auto editor = editorWith(note);
    editor->enterBlockAt(note.indexOf(QLatin1String("text")));
    EXPECT_EQ(editor->displayText(), note);
}

TEST(ListPerfTest, TypingInALongListDoesNotDependOnItsLength)
{
    // A list used to be one block, laid out whole on every keystroke and on
    // every caret move into an element: 11.6 ms per keystroke at 1 000 items,
    // over the budget, and 154 ms at 5 000. Now each top-level item is its own
    // block. Measured with a paint after each step - without one, the relayout
    // is deferred and a timing measures nothing, which is exactly how the
    // first version of this guard passed while the cost was 150 ms.
    const auto measure = [](int items, double *typing, double *crossing) {
        QString note;
        for (int i = 0; i < items; ++i)
            note += QStringLiteral("- item %1 **b** x\n").arg(i);
        auto editor = editorWith(note);
        const qsizetype middle = note.indexOf(QStringLiteral("- item %1 ").arg(items / 2));
        const qsizetype bold = note.indexOf(QLatin1String("**b**"), middle);
        editor->enterBlockAt(bold - 3);
        editor->grab();

        QElapsedTimer clock;
        clock.start();
        QTest::keyClick(editor.get(), Qt::Key_Y);
        editor->grab();
        *typing = clock.nsecsElapsed() / 1e6;

        clock.restart();
        editor->enterBlockAt(bold + 3); // into the bold: what is revealed changes
        editor->grab();
        *crossing = clock.nsecsElapsed() / 1e6;
    };

    double shortTyping = 0, shortCrossing = 0, longTyping = 0, longCrossing = 0;
    measure(100, &shortTyping, &shortCrossing);
    measure(5000, &longTyping, &longCrossing);
    std::printf("[ PERF     ] list of 100: type %.2f ms, cross %.2f ms; of 5000: type %.2f ms, "
                "cross %.2f ms\n",
                shortTyping, shortCrossing, longTyping, longCrossing);

    EXPECT_LT(longTyping, 8.0);
    EXPECT_LT(longCrossing, 8.0);
    // Fifty times the list must not cost anything like fifty times the step.
    EXPECT_LT(longTyping, qMax(1.0, shortTyping) * 4);
}

TEST(CodeBlockPerfTest, TypingInALongCodeBlockStaysInBudget)
{
    // A fence was one block however long it was, relaid and re-highlighted
    // whole on every keystroke: 26 ms at 1 000 lines, and the async parse
    // hid how much worse 5 000 was. Median of several keystrokes, each with a
    // paint after it.
    const auto measure = [](int lines) {
        QString note = QStringLiteral("# Listing\n\n```cpp\n");
        for (int i = 0; i < lines; ++i)
            note += QStringLiteral("int value%1 = compute(%1, \"text\"); // line %1\n").arg(i);
        note += QStringLiteral("```\n");
        auto editor = parsedEditorWith(note);
        if (!editor)
            return -1.0; // never parsed: fails the check below
        const qsizetype middle = note.indexOf(QStringLiteral("value%1 ").arg(lines / 2));
        editor->setCursorOffset(middle);
        editor->enterBlockAt(middle);
        editor->grab();

        std::vector<double> samples;
        for (int i = 0; i < 9; ++i) {
            QElapsedTimer clock;
            clock.start();
            QTest::keyClick(editor.get(), Qt::Key_Y);
            editor->grab();
            samples.push_back(clock.nsecsElapsed() / 1e6);
        }
        std::sort(samples.begin(), samples.end());
        return samples[samples.size() / 2];
    };

    const double shortTyping = measure(100);
    const double longTyping = measure(5000);
    ASSERT_GE(longTyping, 0.0) << "the parse never came back";
    std::printf("[ PERF     ] code block of 100 lines: type %.2f ms; of 5000: %.2f ms\n",
                shortTyping, longTyping);

    EXPECT_LT(longTyping, 8.0);
    // Relaying out the whole listing was about 60 times the short one. What
    // is left grows with the note through the per-keystroke O(document)
    // passes, which weigh more in an unoptimised build: 1.1x in Release,
    // 3.7x at -O0 without highlighting. Eight catches the one that matters.
    EXPECT_LT(longTyping, qMax(1.0, shortTyping) * 8);
}

// --- long fences, in pieces --------------------------------------------------

namespace {

/// A C++ fence of `lines` numbered lines, with a block comment running from
/// line `open` to line `close` - long enough that the comment crosses from
/// one piece into the next ones.
QString longFence(int lines, int open, int close)
{
    QString note = QStringLiteral("Before.\n\n```cpp\n");
    for (int i = 0; i < lines; ++i) {
        if (i == open)
            note += QStringLiteral("/* comment opens\n");
        else if (i == close)
            note += QStringLiteral("comment closes */\n");
        else
            note += QStringLiteral("int value%1 = %1;\n").arg(i);
    }
    return note + QStringLiteral("```\n\nAfter.\n");
}

QColor colourAt(const MarkdownEditor &editor, qsizetype offset)
{
    return editor.formatAt(offset).foreground().color();
}

} // namespace

TEST(CodePieceTest, AnOpenCommentColoursTheLaterPiecesToo)
{
    if (!CodeHighlighter::isAvailable())
        GTEST_SKIP() << "built without KF6 SyntaxHighlighting";
    const QString note = longFence(100, 5, 80);
    auto editor = editorWith(note);
    const qsizetype opener = note.indexOf(QLatin1String("/* comment"));
    const qsizetype inside = note.indexOf(QLatin1String("int value50"));
    const qsizetype after = note.indexOf(QLatin1String("int value90"));
    editor->enterBlockAt(opener + 3);
    editor->grab();
    const QColor comment = colourAt(*editor, opener + 3);
    const QColor keyword = colourAt(*editor, note.indexOf(QLatin1String("int value0")) + 1);
    ASSERT_NE(comment, keyword) << "the highlighter tells a comment from a type";

    editor->enterBlockAt(after); // lays the last lines out, wherever they are
    editor->grab();
    EXPECT_EQ(colourAt(*editor, inside + 1), comment) << "line 50 is inside the comment";
    EXPECT_EQ(colourAt(*editor, after + 1), keyword) << "line 90 is after it";
}

TEST(CodePieceTest, RemovingTheOpenerRecoloursTheLaterPiecesAtOnce)
{
    if (!CodeHighlighter::isAvailable())
        GTEST_SKIP() << "built without KF6 SyntaxHighlighting";
    const QString note = longFence(100, 5, 80);
    auto editor = editorWith(note);
    const qsizetype opener = note.indexOf(QLatin1String("/* comment"));
    const qsizetype inside = note.indexOf(QLatin1String("int value50"));
    editor->enterBlockAt(opener + 2);
    editor->grab();
    const QColor keyword = colourAt(*editor, note.indexOf(QLatin1String("int value0")) + 1);
    ASSERT_NE(colourAt(*editor, inside + 1), keyword) << "line 50 starts in the comment";

    // Backspace twice: "/*" is gone, before the reparse has had its 20 ms.
    QTest::keyClick(editor.get(), Qt::Key_Backspace);
    QTest::keyClick(editor.get(), Qt::Key_Backspace);
    editor->grab();
    EXPECT_EQ(colourAt(*editor, inside - 1), keyword)
        << "a piece below the edit kept the colours of a comment that is gone";
}

TEST(CodePieceTest, APieceLaidOutEarlierFarBelowIsRecolouredToo)
{
    // Only the piece after the edit is relaid at once; one further down that
    // was laid out on an earlier visit still holds the old colours, and has
    // to lose them rather than show them when the reader scrolls back.
    if (!CodeHighlighter::isAvailable())
        GTEST_SKIP() << "built without KF6 SyntaxHighlighting";
    const QString note = longFence(300, 5, 290);
    auto editor = editorWith(note);
    const qsizetype opener = note.indexOf(QLatin1String("/* comment"));
    const qsizetype far = note.indexOf(QLatin1String("int value250"));
    editor->enterBlockAt(opener + 2);
    editor->grab();
    const QColor comment = colourAt(*editor, opener + 3);
    const QColor keyword = colourAt(*editor, note.indexOf(QLatin1String("int value0")) + 1);

    editor->enterBlockAt(far);
    editor->grab();
    ASSERT_EQ(colourAt(*editor, far + 1), comment) << "line 250 starts in the comment";

    editor->enterBlockAt(opener + 2);
    editor->grab();
    QTest::keyClick(editor.get(), Qt::Key_Backspace);
    QTest::keyClick(editor.get(), Qt::Key_Backspace);
    editor->grab();

    editor->enterBlockAt(far - 2);
    editor->grab();
    EXPECT_EQ(colourAt(*editor, far - 1), keyword) << "line 250 kept the comment's colour";
}

TEST(CodePieceTest, TheCaretAnywhereRevealsEveryPiece)
{
    // With the caret in the fence its options show as written - they are on
    // the first piece, and the caret is two pieces further down. C++ takes
    // Quarto's `//|` for them.
    const QString note = QString(longFence(100, 5, 80))
                             .replace(QStringLiteral("```cpp\n"),
                                      QStringLiteral("```cpp\n//| title: t\n"));
    auto editor = editorWith(note);
    EXPECT_FALSE(editor->displayText().contains(QLatin1String("//| title")));
    editor->enterBlockAt(note.indexOf(QLatin1String("int value90")));
    EXPECT_TRUE(editor->displayText().contains(QLatin1String("//| title: t")));
}

TEST(CodePieceTest, CopyingTakesTheWholeListing)
{
    const QString note = longFence(100, 5, 80);
    auto editor = editorWith(note);
    // A title puts the label in a header strip the sweep can find.
    const QString titled = QString(note).replace(QStringLiteral("```cpp\n"),
                                                 QStringLiteral("```cpp\n//| title: t\n"));
    editor->setNoteText(titled);
    ASSERT_TRUE(clickLabelUntilCopied(*editor));
    const qsizetype from = titled.indexOf(QLatin1String("int value0"));
    const qsizetype to = titled.lastIndexOf(QLatin1String("\n```"));
    EXPECT_EQ(QGuiApplication::clipboard()->text(), titled.mid(from, to - from));
}

TEST(CodePieceTest, FoldingFoldsEveryPiece)
{
    const QString note = QString(longFence(100, 5, 80))
                             .replace(QStringLiteral("```cpp\n"),
                                      QStringLiteral("```cpp\n//| title: t\n//| fold: true\n"));
    auto editor = editorWith(note);
    const qsizetype late = note.indexOf(QLatin1String("int value90"));
    EXPECT_TRUE(editor->isFoldedAt(late));
    const qreal folded = editor->documentHeight();
    editor->toggleFoldAt(late);
    EXPECT_FALSE(editor->isFoldedAt(late));
    EXPECT_GT(editor->documentHeight(), folded + 100 * 10) << "a hundred lines came back";
}

// --- scrolling -------------------------------------------------------------

TEST(ScrollTest, AScrolledFrameMatchesAFreshOne)
{
    // A small scroll moves the pixels already on screen and paints only the
    // strip that came in. Whatever that leaves on screen has to be exactly
    // what painting the whole frame would have drawn.
    QString note;
    for (int i = 0; i < 400; ++i) {
        if (i % 12 == 0)
            note += QStringLiteral("## Section %1\n\n").arg(i / 12);
        note += QStringLiteral("Line %1 with **bold**, `code` and a [[link %1]].\n").arg(i);
        if (i % 30 == 29)
            note += QStringLiteral("\n```cpp\nint x = %1;\n```\n\n").arg(i);
    }
    auto editor = parsedEditorWith(note);
    ASSERT_TRUE(editor);
    editor->show();
    ASSERT_TRUE(QTest::qWaitForWindowExposed(editor.get()));

    QScrollBar *bar = editor->verticalScrollBar();
    for (int step = 0; step < 60; ++step) {
        bar->setValue(bar->value() + (step < 40 ? 37 : -53)); // odd sizes, both ways
        QCoreApplication::processEvents();
    }
    QCoreApplication::processEvents();

    QScreen *screen = editor->screen();
    const QImage shown = screen->grabWindow(editor->winId()).toImage().convertToFormat(QImage::Format_RGB32);
    const QImage fresh = editor->grab().toImage().convertToFormat(QImage::Format_RGB32);
    ASSERT_EQ(shown.size(), fresh.size());
    EXPECT_TRUE(shown == fresh) << "what the scroll left on screen differs from a full repaint";
}

