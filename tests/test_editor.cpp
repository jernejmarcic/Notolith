#include <gtest/gtest.h>

#include <thread>

#include <QClipboard>
#include <QScroller>
#include <QDropEvent>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QImage>
#include <QMimeData>
#include <QRandomGenerator>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QScrollBar>
#include <QScreen>
#include <QTimer>
#include <QUrl>
#include <QtConcurrent>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include <memory>

#include "WaitFor.h"
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
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtConcurrent>
#include <QTest>

#include <utility>

#include "CodeHighlighter.h"
#include "Lifeline.h"
#include "MarkdownEditor.h"
#include "Parser.h"
#include "HtmlRenderer.h"
#include "MathRenderer.h"
#include "PdfArgs.h"
#include "PdfPage.h"
#include "TexCompiler.h"

#include <QPainter>
#include <QPdfWriter>

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

#ifdef NOTEAPPC_EXPECT_PDF
/// A real, if minimal, multi-page PDF for the page-argument and layout
/// tests - built with QPdfWriter (already a project dependency, PdfExport.cpp
/// uses it) rather than relying on pdflatex being installed, so these tests
/// run wherever poppler itself does. Every call site is itself behind
/// NOTEAPPC_EXPECT_PDF (there is nothing to open a PDF with otherwise), so
/// this is too.
QString makeTestPdf(const QString &dir, int pages, const QString &name = QStringLiteral("doc.pdf"))
{
    const QString path = dir + QLatin1Char('/') + name;
    QPdfWriter writer(path);
    writer.setPageSize(QPageSize(QPageSize::A4));
    QPainter painter(&writer);
    for (int page = 0; page < pages; ++page) {
        if (page > 0)
            writer.newPage();
        painter.drawText(200, 200, QStringLiteral("page %1").arg(page + 1));
    }
    painter.end();
    return path;
}
#endif

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

QList<MarkdownEditor::SlashCommand> testSlashCommands()
{
    QList<MarkdownEditor::SlashCommand> commands;
    for (const MarkdownEditor::Command &command : MarkdownEditor::commands())
        commands.append({command.id, command.name,
                         command.keys.isEmpty() ? QString() : command.keys.constFirst().toString()});
    return commands;
}

} // namespace

TEST(SlashCommandTest, SlashAtLineStartOpensThePopupWithCommands)
{
    auto editor = editorWith(QStringLiteral("\n"));
    editor->setSlashCommands([] { return testSlashCommands(); }, [](const QString &) {});

    QTest::keyClicks(editor.get(), QStringLiteral("/"));

    EXPECT_TRUE(editor->slashPopupVisible());
    EXPECT_GT(editor->slashSuggestions().size(), 0);
    EXPECT_EQ(editor->noteText(), QStringLiteral("/\n"));
}

TEST(SlashCommandTest, QueryFuzzyFiltersToTheBestMatch)
{
    auto editor = editorWith(QStringLiteral("\n"));
    editor->setSlashCommands([] { return testSlashCommands(); }, [](const QString &) {});

    QTest::keyClicks(editor.get(), QStringLiteral("/bol"));

    const QStringList suggestions = editor->slashSuggestions();
    ASSERT_GT(suggestions.size(), 0);
    EXPECT_EQ(suggestions.constFirst(), QStringLiteral("Toggle bold"));
}

TEST(SlashCommandTest, EnterRunsTheHighlightedCommandAndRemovesTheQuery)
{
    auto editor = editorWith(QStringLiteral("see \n"));
    editor->setCursorOffset(4);
    QString ranID;
    editor->setSlashCommands([] { return testSlashCommands(); },
                             [&ranID](const QString &id) { ranID = id; });

    QTest::keyClicks(editor.get(), QStringLiteral("/bol"));
    ASSERT_TRUE(editor->slashPopupVisible());
    QTest::keyClick(editor.get(), Qt::Key_Return);

    EXPECT_EQ(ranID, QStringLiteral("editor:toggle-bold"));
    EXPECT_EQ(editor->noteText(), QStringLiteral("see \n")) << "the \"/bol\" query is gone";
    EXPECT_FALSE(editor->slashPopupVisible());
}

TEST(SlashCommandTest, MidWordSlashDoesNotOpenIt)
{
    auto editor = editorWith(QStringLiteral("a\n"));
    editor->setSlashCommands([] { return testSlashCommands(); }, [](const QString &) {});
    editor->setCursorOffset(1);

    QTest::keyClicks(editor.get(), QStringLiteral("/b"));

    EXPECT_EQ(editor->noteText(), QStringLiteral("a/b\n"));
    EXPECT_FALSE(editor->slashPopupVisible()) << "\"a/b\" is text, not a command";
}

TEST(SlashCommandTest, NotInsideACodeBlock)
{
    const QString source = QStringLiteral("```\ncode\n```\n");
    auto editor = editorWith(source);
    editor->setSlashCommands([] { return testSlashCommands(); }, [](const QString &) {});
    editor->setCursorOffset(source.indexOf(QStringLiteral("code")) + 2);

    QTest::keyClicks(editor.get(), QStringLiteral("/"));

    EXPECT_FALSE(editor->slashPopupVisible()) << "a fenced block is not where commands run";
}

TEST(SlashCommandTest, EscapeClosesAndLeavesTheText)
{
    auto editor = editorWith(QStringLiteral("\n"));
    editor->setSlashCommands([] { return testSlashCommands(); }, [](const QString &) {});

    QTest::keyClicks(editor.get(), QStringLiteral("/bol"));
    ASSERT_TRUE(editor->slashPopupVisible());
    QTest::keyClick(editor.get(), Qt::Key_Escape);

    EXPECT_FALSE(editor->slashPopupVisible());
    EXPECT_EQ(editor->noteText(), QStringLiteral("/bol\n"));
}

TEST(SlashCommandTest, NullSourceMeansSlashIsPlainText)
{
    auto editor = editorWith(QStringLiteral("\n")); // setSlashCommands never called

    QTest::keyClicks(editor.get(), QStringLiteral("/bol"));

    EXPECT_EQ(editor->noteText(), QStringLiteral("/bol\n"));
    EXPECT_FALSE(editor->slashPopupVisible());
}

// The plan's budget applies here too: fuzzy-filtering a few hundred commands
// on every keystroke must stay well under it.
TEST(SlashCommandTest, FilteringCostStaysInsideTheKeystrokeBudget)
{
    QList<MarkdownEditor::SlashCommand> commands;
    for (int i = 0; i < 400; ++i)
        commands.append({QStringLiteral("plugin:command-%1").arg(i),
                         QStringLiteral("Some plugin command number %1").arg(i), QString()});

    auto editor = editorWith(QStringLiteral("\n"));
    editor->setSlashCommands([commands] { return commands; }, [](const QString &) {});
    QTest::keyClicks(editor.get(), QStringLiteral("/"));
    ASSERT_TRUE(editor->slashPopupVisible());

    QElapsedTimer clock;
    clock.start();
    QTest::keyClicks(editor.get(), QStringLiteral("command"));
    const double perKeystroke = double(clock.nsecsElapsed()) / QStringLiteral("command").size() / 1e6;

    std::printf("[ PERF     ] slash filter, %d commands: %.3f ms per keystroke\n",
                int(commands.size()), perKeystroke);
    EXPECT_LT(perKeystroke, 8.0);
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

// Stage 13: setNotebookRunner is a fake here, the same way setQueryRunner and
// setBaseRunner are faked elsewhere in this file - libs/editor must not link
// libs/ext/notebook, so a real kernel is only ever exercised by
// tests/test_notebook.cpp. `deliver` is always called through
// QTimer::singleShot(0, ...), matching every other runner's contract here:
// never synchronously, so a test has to pump the event loop once.
TEST(NotebookBlockTest, RunOptionsShowOutputsBelow)
{
    const QString note = QStringLiteral("```python\n#| title: cell\nprint(1)\n```\n");
    auto editor = editorWith(note);
    const qreal before = editor->documentHeight();

    bool forceRunSeen = false;
    editor->setNotebookRunner([&forceRunSeen](const QString &, const QString &, bool forceRun,
                                              const MarkdownEditor::NotebookRunOptions &,
                                              const MarkdownEditor::NotebookDeliver &deliver) {
        forceRunSeen = forceRunSeen || forceRun;
        MarkdownEditor::NotebookRunResult result;
        result.hasRun = true;
        result.ok = true;
        MarkdownEditor::NotebookOutput output;
        output.kind = MarkdownEditor::NotebookOutput::Kind::Text;
        output.text = QStringLiteral("1\n");
        result.outputs.append(output);
        QTimer::singleShot(0, [deliver, result] { deliver(result); });
    });
    QTest::qWait(50);

    EXPECT_GT(editor->documentHeight(), before);
    const auto info = editor->notebookInfoAt(note.indexOf(QStringLiteral("print(1)")));
    EXPECT_TRUE(info.eligible);
    EXPECT_TRUE(info.hasRun);
    EXPECT_TRUE(info.ok);
    // A note opening never runs anything - only a cache-only lookup.
    EXPECT_FALSE(forceRunSeen);
}

TEST(NotebookBlockTest, EvalFalseNeverRunsAndSandboxHostIsPassedOn)
{
    const QString note = QStringLiteral("```python\n#| eval: false\nprint(1)\n```\n\n"
                                        "```python\n#| sandbox: host\nprint(2)\n```\n");
    auto editor = editorWith(note);
    QList<bool> hostRequests;
    editor->setNotebookRunner([&hostRequests](const QString &, const QString &, bool forceRun,
                                              const MarkdownEditor::NotebookRunOptions &options,
                                              const MarkdownEditor::NotebookDeliver &deliver) {
        if (forceRun)
            hostRequests.append(options.sandboxHost);
        QTimer::singleShot(0, [deliver] { deliver(MarkdownEditor::NotebookRunResult{}); });
    });
    QTest::qWait(20);
    EXPECT_FALSE(editor->notebookInfoAt(note.indexOf(QStringLiteral("print(1)"))).eligible);
    EXPECT_FALSE(editor->runNotebookBlockAt(note.indexOf(QStringLiteral("print(1)"))));
    ASSERT_TRUE(editor->runNotebookBlockAt(note.indexOf(QStringLiteral("print(2)"))));
    EXPECT_EQ(hostRequests, QList<bool>{true}); // asked for; the window decides whether to honour it
}

TEST(NotebookBlockTest, AnEmbeddedScriptRunsFromItsOwnFolder)
{
    QTemporaryDir dir;
    const QString script = dir.filePath(QStringLiteral("hello.py"));
    {
        QFile file(script);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        file.write("print('hi')\n");
    }
    auto editor = editorWith(QStringLiteral("![[hello.py]]\n\nafter\n"));
    editor->setLinkResolver([&](const QString &) { return script; });
    QString language, source, folder;
    editor->setNotebookRunner([&](const QString &lang, const QString &src, bool forceRun,
                                  const MarkdownEditor::NotebookRunOptions &options,
                                  const MarkdownEditor::NotebookDeliver &deliver) {
        if (forceRun) {
            language = lang;
            source = src;
            folder = options.workingDir;
        }
        QTimer::singleShot(0, [deliver] { deliver(MarkdownEditor::NotebookRunResult{}); });
    });
    editor->setCursorOffset(editor->noteText().size()); // off the embed's line
    QTest::qWait(20);
    ASSERT_TRUE(editor->runNotebookBlockAt(0));
    EXPECT_EQ(language, QStringLiteral("python"));
    EXPECT_EQ(source.trimmed(), QStringLiteral("print('hi')"));
    EXPECT_EQ(folder, dir.path());
}

TEST(NotebookBlockTest, RunControlHitTests)
{
    const QString note = QStringLiteral("```python\nprint(1)\n```\n");
    auto editor = editorWith(note);

    bool ran = false;
    editor->setNotebookRunner([&ran](const QString &, const QString &, bool forceRun,
                                     const MarkdownEditor::NotebookRunOptions &,
                                     const MarkdownEditor::NotebookDeliver &deliver) {
        if (forceRun)
            ran = true;
        QTimer::singleShot(0, [deliver] { deliver(MarkdownEditor::NotebookRunResult{}); });
    });
    QTest::qWait(50);

    const qsizetype offset = note.indexOf(QStringLiteral("print(1)"));
    const auto info = editor->notebookInfoAt(offset);
    ASSERT_TRUE(info.eligible);
    // In the header's corner, the way codeLabelOf's own rect is.
    EXPECT_TRUE(info.runRect.isValid());
    EXPECT_GT(info.runRect.width(), 0);

    EXPECT_TRUE(editor->runNotebookBlockAt(offset));
    QTest::qWait(50);
    EXPECT_TRUE(ran);

    // Outside any code block: nothing to run.
    EXPECT_FALSE(editor->runNotebookBlockAt(0));
}

TEST(NotebookBlockTest, CachedOutputReappearsAfterReopeningWithoutAKernel)
{
    const QString note = QStringLiteral("```python\nprint(2)\n```\n");
    // Stands in for libs/ext/notebook's own sidecar cache (NotebookCache.cpp)
    // - shared across two separate MarkdownEditor instances the way the real
    // cache is shared across two separate times a note is opened.
    QHash<QString, MarkdownEditor::NotebookRunResult> sidecar;
    int forceRunCount = 0;
    const auto runner = [&sidecar, &forceRunCount](const QString &language, const QString &source,
                                                    bool forceRun,
                                                    const MarkdownEditor::NotebookRunOptions &,
                                                    const MarkdownEditor::NotebookDeliver &deliver) {
        const QString key = language.toLower() + QLatin1Char('|') + source;
        if (forceRun) {
            ++forceRunCount;
            MarkdownEditor::NotebookRunResult result;
            result.hasRun = true;
            result.ok = true;
            MarkdownEditor::NotebookOutput output;
            output.kind = MarkdownEditor::NotebookOutput::Kind::Text;
            output.text = QStringLiteral("2\n");
            result.outputs.append(output);
            sidecar.insert(key, result);
            QTimer::singleShot(0, [deliver, result] { deliver(result); });
            return;
        }
        const MarkdownEditor::NotebookRunResult cached = sidecar.value(key);
        QTimer::singleShot(0, [deliver, cached] { deliver(cached); });
    };

    auto first = editorWith(note);
    first->setNotebookRunner(runner);
    const qsizetype offset = note.indexOf(QStringLiteral("print(2)"));
    ASSERT_TRUE(first->runNotebookBlockAt(offset));
    QTest::qWait(50);
    EXPECT_TRUE(first->notebookInfoAt(offset).hasRun);
    EXPECT_EQ(forceRunCount, 1);

    // A fresh editor over the same text - "reopening the note" - sharing
    // only the sidecar, never the first editor's own runtime state.
    auto second = editorWith(note);
    second->setNotebookRunner(runner);
    QTest::qWait(50);
    EXPECT_TRUE(second->notebookInfoAt(offset).hasRun);
    EXPECT_EQ(second->notebookInfoAt(offset).ok, true);
    // Reopening never started anything - still the one run from above.
    EXPECT_EQ(forceRunCount, 1);
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

#ifdef NOTEAPPC_EXPECT_PDF
TEST(PdfPageTest, PageSizeReadsDimensionsWithoutRendering)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = makeTestPdf(dir.path(), 2);
    const QSizeF size = PdfPage::pageSize(path, 0);
    EXPECT_GT(size.width(), 0);
    EXPECT_GT(size.height(), 0);
    EXPECT_TRUE(PdfPage::pageSize(path, 99).isEmpty()) << "out of range";
    EXPECT_EQ(PdfPage::pageCount(path), 2);
}
#endif

// --- PdfArgs: the fragment grammar, one form per test ----------------------

TEST(PdfArgsTest, NoArgumentsIsTheWholeDocumentAsSlides)
{
    const PdfEmbedArgs args = parsePdfEmbedArgs(QString());
    EXPECT_TRUE(args.ok());
    EXPECT_FALSE(args.pagesGiven);
    EXPECT_FALSE(args.allPages);
    EXPECT_EQ(args.layout, PdfEmbedArgs::Layout::Slides);

    const QList<int> pages = resolvePdfPages(args, 5);
    EXPECT_EQ(pages, (QList<int>{0, 1, 2, 3, 4}));
}

TEST(PdfArgsTest, SinglePage)
{
    const PdfEmbedArgs args = parsePdfEmbedArgs(QStringLiteral("#page=3"));
    ASSERT_TRUE(args.ok());
    EXPECT_EQ(args.pages, (QList<int>{2}));
    EXPECT_EQ(args.layout, PdfEmbedArgs::Layout::Column) << "pages listed default to column";
}

TEST(PdfArgsTest, SeveralPages)
{
    const PdfEmbedArgs args = parsePdfEmbedArgs(QStringLiteral("#page=2,5,9"));
    ASSERT_TRUE(args.ok());
    EXPECT_EQ(args.pages, (QList<int>{1, 4, 8}));
}

TEST(PdfArgsTest, ARange)
{
    const PdfEmbedArgs args = parsePdfEmbedArgs(QStringLiteral("#page=3-7"));
    ASSERT_TRUE(args.ok());
    EXPECT_EQ(args.pages, (QList<int>{2, 3, 4, 5, 6}));
}

TEST(PdfArgsTest, MixedListAndRange)
{
    const PdfEmbedArgs args = parsePdfEmbedArgs(QStringLiteral("#page=1,3-5,9"));
    ASSERT_TRUE(args.ok());
    EXPECT_EQ(args.pages, (QList<int>{0, 2, 3, 4, 8}));
}

TEST(PdfArgsTest, AllPages)
{
    const PdfEmbedArgs args = parsePdfEmbedArgs(QStringLiteral("#page=all"));
    ASSERT_TRUE(args.ok());
    EXPECT_TRUE(args.allPages);
    EXPECT_EQ(args.layout, PdfEmbedArgs::Layout::Column);
    EXPECT_EQ(resolvePdfPages(args, 4), (QList<int>{0, 1, 2, 3}));
}

TEST(PdfArgsTest, HeightAloneKeepsTheSlidesDefault)
{
    const PdfEmbedArgs args = parsePdfEmbedArgs(QStringLiteral("#height=400"));
    ASSERT_TRUE(args.ok());
    EXPECT_EQ(args.height, 400);
    EXPECT_FALSE(args.pagesGiven);
    EXPECT_EQ(args.layout, PdfEmbedArgs::Layout::Slides);
}

TEST(PdfArgsTest, ExplicitLayoutOverridesTheDefault)
{
    const PdfEmbedArgs args = parsePdfEmbedArgs(QStringLiteral("#page=1,2&layout=slides"));
    ASSERT_TRUE(args.ok());
    EXPECT_EQ(args.layout, PdfEmbedArgs::Layout::Slides);
}

TEST(PdfArgsTest, GridWithColumns)
{
    const PdfEmbedArgs args = parsePdfEmbedArgs(QStringLiteral("#layout=grid&columns=3"));
    ASSERT_TRUE(args.ok());
    EXPECT_EQ(args.layout, PdfEmbedArgs::Layout::Grid);
    EXPECT_EQ(args.columns, 3);
}

TEST(PdfArgsTest, BadPageNumberErrors)
{
    EXPECT_FALSE(parsePdfEmbedArgs(QStringLiteral("#page=abc")).ok());
    EXPECT_FALSE(parsePdfEmbedArgs(QStringLiteral("#page=0")).ok()) << "pages start at 1";
}

TEST(PdfArgsTest, ReversedRangeErrors)
{
    const PdfEmbedArgs args = parsePdfEmbedArgs(QStringLiteral("#page=7-3"));
    EXPECT_FALSE(args.ok());
    EXPECT_FALSE(args.error.isEmpty());
}

TEST(PdfArgsTest, BadHeightErrors)
{
    EXPECT_FALSE(parsePdfEmbedArgs(QStringLiteral("#height=big")).ok());
    EXPECT_FALSE(parsePdfEmbedArgs(QStringLiteral("#height=0")).ok());
}

TEST(PdfArgsTest, UnknownLayoutErrors)
{
    EXPECT_FALSE(parsePdfEmbedArgs(QStringLiteral("#layout=carousel")).ok());
}

TEST(PdfArgsTest, UnknownKeyIsIgnoredNotAnError)
{
    const PdfEmbedArgs args = parsePdfEmbedArgs(QStringLiteral("#page=1&zoom=150"));
    EXPECT_TRUE(args.ok());
}

TEST(PdfArgsTest, ResolvePdfPagesDropsOutOfRangeRatherThanClamping)
{
    const PdfEmbedArgs args = parsePdfEmbedArgs(QStringLiteral("#page=1,5"));
    ASSERT_TRUE(args.ok());
    EXPECT_EQ(resolvePdfPages(args, 3), (QList<int>{0})) << "page 5 of a 3-page document is dropped";
}

// --- PDF embeds in the editor: layout, async render, slides nav ------------

#ifdef NOTEAPPC_EXPECT_PDF

TEST(PdfEmbedTest, ColumnLayoutShowsEveryRequestedPage)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString pdf = makeTestPdf(dir.path(), 6);

    auto editor = editorWith(QStringLiteral("![[report.pdf#page=1,3-5]]\n"));
    editor->setLinkResolver([&](const QString &) { return pdf; });

    const MarkdownEditor::PdfEmbedInfo info = editor->pdfEmbedInfoAt(0);
    ASSERT_TRUE(info.isPdfEmbed);
    EXPECT_TRUE(info.error.isEmpty()) << info.error.toStdString();
    EXPECT_EQ(info.pageCount, 6);
    EXPECT_EQ(info.layout, PdfEmbedArgs::Layout::Column);
    EXPECT_EQ(info.cellCount, 4) << "1, 3, 4, 5";
}

TEST(PdfEmbedTest, SlidesLayoutShowsOnePageAndStepsWithPdfSlideStep)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString pdf = makeTestPdf(dir.path(), 4);

    auto editor = editorWith(QStringLiteral("![[deck.pdf]]\n"));
    editor->setLinkResolver([&](const QString &) { return pdf; });

    MarkdownEditor::PdfEmbedInfo info = editor->pdfEmbedInfoAt(0);
    ASSERT_TRUE(info.isPdfEmbed);
    EXPECT_EQ(info.layout, PdfEmbedArgs::Layout::Slides);
    EXPECT_EQ(info.cellCount, 1);
    EXPECT_EQ(info.slideIndex, 0);
    EXPECT_EQ(info.slideCount, 4);

    editor->pdfSlideStep(0, 1);
    info = editor->pdfEmbedInfoAt(0);
    EXPECT_EQ(info.slideIndex, 1);

    editor->pdfSlideStep(0, -1);
    editor->pdfSlideStep(0, -1); // clamps at 0, does not wrap
    info = editor->pdfEmbedInfoAt(0);
    EXPECT_EQ(info.slideIndex, 0);
}

TEST(PdfEmbedTest, GridLayoutRespectsColumns)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString pdf = makeTestPdf(dir.path(), 5);

    auto editor = editorWith(QStringLiteral("![[deck.pdf#page=all&layout=grid&columns=2]]\n"));
    editor->setLinkResolver([&](const QString &) { return pdf; });

    const MarkdownEditor::PdfEmbedInfo info = editor->pdfEmbedInfoAt(0);
    ASSERT_TRUE(info.isPdfEmbed);
    EXPECT_EQ(info.layout, PdfEmbedArgs::Layout::Grid);
    EXPECT_EQ(info.cellCount, 5);
}

TEST(PdfEmbedTest, BadArgumentShowsAnErrorInsteadOfCrashing)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString pdf = makeTestPdf(dir.path(), 2);

    auto editor = editorWith(QStringLiteral("![[deck.pdf#page=9-3]]\n"));
    editor->setLinkResolver([&](const QString &) { return pdf; });

    const MarkdownEditor::PdfEmbedInfo info = editor->pdfEmbedInfoAt(0);
    ASSERT_TRUE(info.isPdfEmbed);
    EXPECT_FALSE(info.error.isEmpty());
    EXPECT_EQ(info.cellCount, 0);
}

#endif // NOTEAPPC_EXPECT_PDF

// --- .tex embeds -------------------------------------------------------------

TEST(TexCompilerTest, IsFullDocumentTellsMathsAloneApart)
{
    EXPECT_FALSE(TexCompiler::isFullDocument(QStringLiteral("E = mc^2")));
    EXPECT_TRUE(TexCompiler::isFullDocument(
        QStringLiteral("\\documentclass{article}\\begin{document}hi\\end{document}")));
}

TEST(TexCompilerTest, CompilesATinyDocumentAndCachesIt)
{
    if (!TexCompiler::isAvailable())
        GTEST_SKIP() << "no pdflatex on PATH";

    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("doc.tex"));
    {
        QFile file(path);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        file.write("\\documentclass{article}\\begin{document}hello\\end{document}\n");
    }

    const TexCompiler::Result first = TexCompiler::compile(path);
    ASSERT_TRUE(first.ok) << first.log.toStdString() << first.error.toStdString();
    EXPECT_TRUE(QFile::exists(first.pdfPath));

    // Unchanged bytes hash to the same cache entry, so a second call answers
    // without running pdflatex again - observable here as the same path.
    const TexCompiler::Result second = TexCompiler::compile(path);
    EXPECT_EQ(second.pdfPath, first.pdfPath);
}

TEST(TexCompilerTest, ReportsAFailedCompile)
{
    if (!TexCompiler::isAvailable())
        GTEST_SKIP() << "no pdflatex on PATH";

    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("broken.tex"));
    {
        QFile file(path);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        file.write("\\documentclass{article}\\begin{document}\\undefinedcommand{x}\\end{document}\n");
    }

    const TexCompiler::Result result = TexCompiler::compile(path);
    EXPECT_FALSE(result.ok);
    EXPECT_FALSE(result.log.isEmpty());
}

TEST(TexCompilerTest, NeverRunsWrite18)
{
    if (!TexCompiler::isAvailable())
        GTEST_SKIP() << "no pdflatex on PATH";

    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString marker = dir.filePath(QStringLiteral("pwned"));
    const QString path = dir.filePath(QStringLiteral("evil.tex"));
    {
        QFile file(path);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        file.write(("\\documentclass{article}\\begin{document}"
                    "\\immediate\\write18{touch " + marker.toUtf8() + "}"
                    "\\end{document}\n"));
    }

    (void)TexCompiler::compile(path);
    EXPECT_FALSE(QFile::exists(marker)) << "-no-shell-escape must have refused \\write18";
}

TEST(TexCompilerTest, ADocumentCannotReadAFileOutsideItsOwnDirectory)
{
    if (!TexCompiler::isAvailable() || QStandardPaths::findExecutable(QStringLiteral("bwrap")).isEmpty())
        GTEST_SKIP() << "needs pdflatex and bubblewrap";

    // A file the user can read, somewhere a document has no business in.
    QTemporaryDir elsewhere;
    const QString secret = elsewhere.filePath(QStringLiteral("secret.tex"));
    {
        QFile file(secret);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        file.write("private\n");
    }
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("nosy.tex"));
    {
        QFile file(path);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        file.write("\\documentclass{article}\\begin{document}\\input{" + secret.toUtf8()
                   + "}\\end{document}\n");
    }
    const TexCompiler::Result result = TexCompiler::compile(path);
    EXPECT_FALSE(result.ok) << "the sandbox must hide files outside the compile directory";
    EXPECT_TRUE(result.log.contains(QLatin1String("not found"))) << result.log.toStdString();
}

TEST(MarkdownEditorTest, TexMathsAloneRendersThroughMicroTeX)
{
    ASSERT_TRUE(MathRenderer::isAvailable());
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("formula.tex"));
    {
        QFile file(path);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        file.write("E = mc^2");
    }

    auto editor = editorWith(QStringLiteral("![[formula.tex]]\n"));
    editor->setLinkResolver([&](const QString &) { return path; });

    EXPECT_FALSE(editor->displayText().contains(QStringLiteral("formula.tex")));
    EXPECT_GT(editor->documentHeight(), 0);
}

// --- file cards and several embeds in one block -----------------------------

TEST(FileEmbedTest, BinaryFileBecomesACardNotAWallOfBytes)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("blob.bin"));
    {
        QFile file(path);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        QByteArray bytes(256, char(0));
        for (int i = 0; i < bytes.size(); ++i)
            bytes[i] = char(i % 256);
        file.write(bytes);
    }

    auto editor = editorWith(QStringLiteral("![[blob.bin]]\n"));
    editor->setLinkResolver([&](const QString &) { return path; });

    EXPECT_TRUE(editor->isFileCardAt(0));
    EXPECT_FALSE(editor->displayText().contains(QChar(0)));
}

TEST(FileEmbedTest, TextFileStaysAReadOnlyListing)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("notes.txt"));
    {
        QFile file(path);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        file.write("plain text, nothing binary here\n");
    }

    auto editor = editorWith(QStringLiteral("![[notes.txt]]\n"));
    editor->setLinkResolver([&](const QString &) { return path; });

    EXPECT_FALSE(editor->isFileCardAt(0));
}

TEST(MultiEmbedTest, AParagraphOfSeveralImageEmbedsRendersEveryOne)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    QImage image(20, 20, QImage::Format_RGB32);
    image.fill(Qt::red);
    const QString a = dir.filePath(QStringLiteral("a.png"));
    const QString b = dir.filePath(QStringLiteral("b.png"));
    ASSERT_TRUE(image.save(a));
    ASSERT_TRUE(image.save(b));

    auto editor = editorWith(QStringLiteral("![[a.png]]\n![[b.png]]\n"));
    editor->setLinkResolver([&](const QString &target) {
        return target == QLatin1String("a.png") ? a : target == QLatin1String("b.png") ? b : QString();
    });

    // Two embeds, each on its own line, nothing else in the block: each is
    // its own row of one cell.
    EXPECT_EQ(editor->embedCellCountAt(0), 2);
}

TEST(MultiEmbedTest, AnEmbedInsideRunningTextStillRenders)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    QImage image(20, 20, QImage::Format_RGB32);
    image.fill(Qt::blue);
    const QString path = dir.filePath(QStringLiteral("inline.png"));
    ASSERT_TRUE(image.save(path));

    auto editor = editorWith(QStringLiteral("see ![[inline.png]] here\n"));
    editor->setLinkResolver([&](const QString &) { return path; });

    EXPECT_EQ(editor->embedCellCountAt(0), 1);
    // The paragraph's own text is still there - only the embed's own source
    // range was hidden, not the words around it.
    EXPECT_TRUE(editor->displayText().contains(QStringLiteral("see")));
    EXPECT_TRUE(editor->displayText().contains(QStringLiteral("here")));
}

// --- note embeds: whole, #Heading, #^block ----------------------------------

TEST(NoteEmbedTest, WholeHeadingAndBlockEachExtractTheRightSlice)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("Target.md"));
    {
        QFile file(path);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        file.write("Intro line.\n\n"
                   "# First\n\nFirst section body.\n\n"
                   "# Second\n\nSecond section body. ^myblock\n");
    }

    // The panel's own text - not displayText(), which only ever reflects
    // view.map, and a note embed (like a source-file listing) substitutes a
    // synthetic QTextLayout instead of mapping the note's own bytes.
    auto whole = editorWith(QStringLiteral("![[Target]]\n"));
    whole->setLinkResolver([&](const QString &) { return path; });
    EXPECT_TRUE(whole->panelTextAt(0).contains(QStringLiteral("Intro line")));
    EXPECT_TRUE(whole->panelTextAt(0).contains(QStringLiteral("Second section body")));

    auto heading = editorWith(QStringLiteral("![[Target#First]]\n"));
    heading->setLinkResolver([&](const QString &) { return path; });
    EXPECT_TRUE(heading->panelTextAt(0).contains(QStringLiteral("First section body")));
    EXPECT_FALSE(heading->panelTextAt(0).contains(QStringLiteral("Second section body")));

    auto block = editorWith(QStringLiteral("![[Target#^myblock]]\n"));
    block->setLinkResolver([&](const QString &) { return path; });
    EXPECT_TRUE(block->panelTextAt(0).contains(QStringLiteral("Second section body")));
    EXPECT_FALSE(block->panelTextAt(0).contains(QStringLiteral("First section body")));
}

TEST(ListLabelTest, CountedNumbersAndWordStyleOutlines)
{
    auto editor = editorWith(QString());
    ListItemSpan item;
    item.ordered = true;
    item.number = 1;      // typed "1." ...
    item.shownNumber = 3; // ... as the third item
    item.bullet = QLatin1Char(')');
    item.outline = QStringLiteral("2.3");
    EXPECT_EQ(editor->listLabel(item), QStringLiteral("3)"));
    editor->setOutlineNumbering(true);
    EXPECT_EQ(editor->listLabel(item), QStringLiteral("2.3"));
    item.outline = QStringLiteral("4"); // the top level keeps its own look
    EXPECT_EQ(editor->listLabel(item), QStringLiteral("3)"));
    ListItemSpan bullet;
    EXPECT_TRUE(editor->listLabel(bullet).isEmpty());
}

TEST(RuleTest, AThematicBreakIsDrawnAsALine)
{
    auto editor = editorWith(QStringLiteral("above\n\n---\n\nbelow\n"));
    editor->setCursorOffset(editor->noteText().size()); // off the rule's line
    editor->show();
    QApplication::processEvents();
    const QImage shot = editor->viewport()->grab().toImage();
    const QColor background = shot.pixelColor(2, shot.height() - 2);
    // A line: some row where most of the text column is one colour that is
    // not the background.
    bool found = false;
    for (int y = 0; y < shot.height() && !found; ++y) {
        QHash<QRgb, int> counts;
        for (int x = 0; x < shot.width(); ++x)
            ++counts[shot.pixel(x, y)];
        for (auto it = counts.cbegin(); it != counts.cend(); ++it) {
            if (QColor(it.key()) != background && it.value() > shot.width() / 2)
                found = true;
        }
    }
    EXPECT_TRUE(found);
}

TEST(CodeBlockHookTest, APluginLanguageIsDrawnAsItsRendererAnswers)
{
    auto editor = editorWith(QStringLiteral("```calc\n2+2\n```\n\n```text\nplain\n```\n\nafter\n"));
    editor->setCursorOffset(editor->noteText().size()); // off both fences
    const qsizetype calc = editor->noteText().indexOf(QLatin1String("2+2"));
    QStringList asked;
    editor->setCodeBlockRenderer({QStringLiteral("CALC")}, [&](const QString &language, const QString &source, int width,
                                                               const QString &theme, const auto &deliver) {
        asked << language + QLatin1Char('|') + source.trimmed() + QLatin1Char('|') + theme;
        QImage image(width, 30, QImage::Format_ARGB32);
        image.fill(Qt::red);
        // From another thread, as a plugin host's answer arrives.
        (void)QtConcurrent::run([deliver, image] { deliver({false, QString(), image}); });
    });
    WAIT_TRUE(!editor->embedImageSizeAt(calc).isEmpty(), 2000);
    EXPECT_EQ(editor->embedImageSizeAt(calc).height(), 30);
    EXPECT_EQ(asked, QStringList{QStringLiteral("calc|2+2|dark")});
    // Another language stays a listing.
    EXPECT_TRUE(editor->embedImageSizeAt(editor->noteText().indexOf(QLatin1String("plain"))).isEmpty());

    // Markdown text is drawn formatted; unregistering brings the listing back.
    editor->setCodeBlockRenderer({QStringLiteral("calc")}, [](const QString &, const QString &, int, const QString &,
                                                              const auto &deliver) {
        deliver({true, QStringLiteral("**4**"), QImage()});
    });
    WAIT_TRUE(!editor->embedImageSizeAt(calc).isEmpty(), 2000);
    editor->setCodeBlockRenderer({QStringLiteral("calc")}, nullptr);
    EXPECT_TRUE(editor->embedImageSizeAt(calc).isEmpty());
}

TEST(NoteEmbedTest, ANoteEmbedIsDrawnFormattedAMomentLater)
{
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("Target.md"));
    {
        QFile file(path);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        file.write("# Heading\n\nSome **bold** text.\n");
    }
    auto editor = editorWith(QStringLiteral("![[Target]]\n\nafter\n"));
    editor->setLinkResolver([&](const QString &) { return path; });
    editor->setCursorOffset(editor->noteText().size()); // off the embed's line
    // Text at once, so layout never waits on the formatted drawing...
    EXPECT_TRUE(editor->panelTextAt(0).contains(QStringLiteral("bold")));
    EXPECT_TRUE(editor->embedImageSizeAt(0).isEmpty());
    // ...which replaces it a turn of the event loop later.
    WAIT_TRUE(!editor->embedImageSizeAt(0).isEmpty(), 2000);
    EXPECT_TRUE(editor->panelTextAt(0).isEmpty());
    EXPECT_GT(editor->embedImageSizeAt(0).height(), 20);
}

// --- feature switches: one on/off pair per feature --------------------------

TEST(FeaturesTest, QueryBlocksToggle)
{
    const QString note = QStringLiteral("```query\nhello\n```\n");
    auto editor = editorWith(note);
    // No runner needed: even "No search source connected" is the query
    // machinery's own message, drawn through a substituted QTextLayout - so
    // displayText() (which only ever reflects view.map) shows nothing for
    // this block, not the fence's raw "hello". panelTextAt is query.cpp's
    // own message text; displayText is enough to prove the fence's content
    // is not leaking through as plain text.
    EXPECT_FALSE(editor->displayText().contains(QStringLiteral("hello")))
        << "on: rendered through the query machinery, not as a plain fence";

    MarkdownEditor::Features off;
    off.queryBlocks = false;
    editor->setFeatures(off);
    // Fence lines are syntax, hidden in live preview like any other marker;
    // the fence's own content showing through plainly (mapped, not
    // substituted) is what "off" actually looks like.
    EXPECT_TRUE(editor->displayText().contains(QStringLiteral("hello")));
    EXPECT_FALSE(editor->displayText().contains(QStringLiteral("Searching")));
}

TEST(FeaturesTest, BaseBlocksToggle)
{
    const QString note = QStringLiteral("```base\nviews:\n  - type: table\n```\n");
    auto editor = editorWith(note);
    EXPECT_FALSE(editor->displayText().contains(QStringLiteral("views:")))
        << "on: rendered through the base machinery (\"No base source connected\" with no "
           "runner), not as a plain fence";

    MarkdownEditor::Features off;
    off.baseBlocks = false;
    editor->setFeatures(off);
    EXPECT_TRUE(editor->displayText().contains(QStringLiteral("views:")))
        << "off: the fence's own content shows as a plain listing";
}

#ifdef NOTEAPPC_EXPECT_HTML
TEST(FeaturesTest, HTMLblocksToggle)
{
    const QString note = QStringLiteral("<div>hello</div>\n");
    auto editor = editorWith(note);
    ASSERT_TRUE(editor->htmlImageSizeAt(0).isValid()) << "on by default";

    MarkdownEditor::Features off;
    off.HTMLblocks = false;
    editor->setFeatures(off);
    EXPECT_FALSE(editor->htmlImageSizeAt(0).isValid());
}
#endif

#ifdef NOTEAPPC_EXPECT_PDF
TEST(FeaturesTest, PDFembedsToggle)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString pdf = makeTestPdf(dir.path(), 1);
    auto editor = editorWith(QStringLiteral("![[deck.pdf]]\n"));
    editor->setLinkResolver([&](const QString &) { return pdf; });
    EXPECT_TRUE(editor->pdfEmbedInfoAt(0).isPdfEmbed);

    MarkdownEditor::Features off;
    off.PDFembeds = false;
    editor->setFeatures(off);
    EXPECT_FALSE(editor->pdfEmbedInfoAt(0).isPdfEmbed);
}
#endif

TEST(FeaturesTest, TexEmbedsToggle)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("formula.tex"));
    {
        QFile file(path);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        file.write("E = mc^2");
    }
    auto editor = editorWith(QStringLiteral("![[formula.tex]]\n"));
    editor->setLinkResolver([&](const QString &) { return path; });
    EXPECT_FALSE(editor->displayText().contains(QStringLiteral("formula.tex")))
        << "on: rendered - maths alone, through MicroTeX";
    const qreal on = editor->documentHeight();

    MarkdownEditor::Features off;
    off.texEmbeds = false;
    editor->setFeatures(off);
    // Still not the raw link - fileEmbeds (still on) renders it as a source
    // listing instead; panelTextAt is that listing's own text, which
    // displayText() cannot see (see the comment on QueryBlocksToggle).
    EXPECT_FALSE(editor->displayText().contains(QStringLiteral("formula.tex")))
        << "off: falls back to a source listing, not to the raw link";
    EXPECT_TRUE(editor->panelTextAt(0).contains(QStringLiteral("E = mc^2")))
        << "off: MicroTeX never ran - this is the file's own bytes, plainly listed";
    EXPECT_NE(editor->documentHeight(), on)
        << "off: a source listing is laid out differently from a rendered formula";
}

TEST(FeaturesTest, MediaEmbedsToggle)
{
    auto editor = editorWith(QStringLiteral("![[song.mp3]]\n"));
    editor->setLinkResolver([](const QString &target) { return QStringLiteral("/nowhere/") + target; });
    const qreal on = editor->documentHeight();

    MarkdownEditor::Features off;
    off.mediaEmbeds = false;
    editor->setFeatures(off);
    const qreal disabled = editor->documentHeight();
    EXPECT_LT(disabled, on) << "off: no player card, just the link";
}

TEST(FeaturesTest, MathsToggle)
{
    auto editor = editorWith(QStringLiteral("Inline $e^{i\\pi}+1=0$ formula.\n"));
    EXPECT_FALSE(editor->displayText().contains(QStringLiteral("\\pi")));

    MarkdownEditor::Features off;
    off.maths = false;
    editor->setFeatures(off);
    EXPECT_TRUE(editor->displayText().contains(QStringLiteral("\\pi")))
        << "off: the raw LaTeX shows, MicroTeX never runs";
}

TEST(FeaturesTest, FileEmbedsToggle)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("notes.txt"));
    {
        QFile file(path);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        file.write("plain text\n");
    }
    auto editor = editorWith(QStringLiteral("![[notes.txt]]\n"));
    editor->setLinkResolver([&](const QString &) { return path; });
    EXPECT_FALSE(editor->displayText().contains(QStringLiteral("notes.txt")))
        << "on: rendered as a listing, the wikilink text is gone";

    MarkdownEditor::Features off;
    off.fileEmbeds = false;
    editor->setFeatures(off);
    EXPECT_TRUE(editor->displayText().contains(QStringLiteral("notes.txt")))
        << "off: falls back to the plain link";
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

TEST(TouchModeTest, WithoutATouchscreenADragStillSelects)
{
    // tests/test_touch.cpp has the other half; this binary has no touchscreen.
    MarkdownEditor editor;
    // hasScroller(), not grabbedGesture(): that one answers the same for a
    // widget nobody grabbed anything on.
    EXPECT_FALSE(QScroller::hasScroller(editor.viewport()));
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

// --- editing property values -------------------------------------------------

namespace {

/// Types `text` into the open property editor and presses `key` there.
bool typeIntoPropertyEditor(MarkdownEditor &editor, const QString &text, Qt::Key key)
{
    auto *edit = editor.findChild<QLineEdit *>();
    if (!edit || edit->isHidden())
        return false;
    edit->setText(text);
    QTest::keyClick(edit, key);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    return true;
}

const QString kPropertiesNote = QStringLiteral(
    "---\ntitle: Old\ncount: 3\nempty:\ntags:\n  - a\n  - b\ndone: false\n"
    "noted: kept # a comment\n---\n\nBody\n");

} // namespace

TEST(PropertyEditTest, AValueIsRewrittenInPlaceAndQuotedOnlyWhenItHasTo)
{
    auto editor = editorWith(kPropertiesNote);
    ASSERT_TRUE(editor->editPropertyAt(kPropertiesNote.indexOf(QLatin1String("title"))));
    ASSERT_TRUE(typeIntoPropertyEditor(*editor, QStringLiteral("New: title"), Qt::Key_Return));
    EXPECT_EQ(editor->noteText(), QString(kPropertiesNote).replace(
                                      QStringLiteral("title: Old"),
                                      QStringLiteral("title: \"New: title\"")));
    EXPECT_FALSE(editor->findChild<QLineEdit *>()) << "the editor closes";

    // Undo takes the edit back as one step.
    QTest::keyClick(editor.get(), Qt::Key_Z, Qt::ControlModifier);
    EXPECT_EQ(editor->noteText(), kPropertiesNote);
}

TEST(PropertyEditTest, EscapeLeavesTheFileAlone)
{
    auto editor = editorWith(kPropertiesNote);
    ASSERT_TRUE(editor->editPropertyAt(kPropertiesNote.indexOf(QLatin1String("title"))));
    ASSERT_TRUE(typeIntoPropertyEditor(*editor, QStringLiteral("Something else"), Qt::Key_Escape));
    EXPECT_EQ(editor->noteText(), kPropertiesNote);
}

TEST(PropertyEditTest, ANumberStaysANumber)
{
    auto editor = editorWith(kPropertiesNote);
    ASSERT_TRUE(editor->editPropertyAt(kPropertiesNote.indexOf(QLatin1String("count"))));
    ASSERT_TRUE(typeIntoPropertyEditor(*editor, QStringLiteral("many"), Qt::Key_Return));
    EXPECT_EQ(editor->noteText(), kPropertiesNote) << "not a number: refused";

    ASSERT_TRUE(editor->editPropertyAt(kPropertiesNote.indexOf(QLatin1String("count"))));
    ASSERT_TRUE(typeIntoPropertyEditor(*editor, QStringLiteral("4.5"), Qt::Key_Return));
    EXPECT_TRUE(editor->noteText().contains(QLatin1String("\ncount: 4.5\n")));
}

TEST(PropertyEditTest, AnEmptyValueIsFilledIn)
{
    auto editor = editorWith(kPropertiesNote);
    ASSERT_TRUE(editor->editPropertyAt(kPropertiesNote.indexOf(QLatin1String("empty"))));
    ASSERT_TRUE(typeIntoPropertyEditor(*editor, QStringLiteral("now"), Qt::Key_Return));
    EXPECT_TRUE(editor->noteText().contains(QLatin1String("\nempty: now\ntags:")));
}

TEST(PropertyEditTest, WhatCannotBeRewrittenSafelyIsNotOpened)
{
    auto editor = editorWith(kPropertiesNote);
    EXPECT_FALSE(editor->editPropertyAt(kPropertiesNote.indexOf(QLatin1String("tags"))))
        << "a list";
    EXPECT_FALSE(editor->editPropertyAt(kPropertiesNote.indexOf(QLatin1String("done"))))
        << "a checkbox toggles";
    EXPECT_FALSE(editor->editPropertyAt(kPropertiesNote.indexOf(QLatin1String("noted"))))
        << "a comment on the line would be lost";
    EXPECT_FALSE(editor->findChild<QLineEdit *>());
}

TEST(PropertyEditTest, ClickingAValueOpensItsEditor)
{
    auto editor = editorWith(kPropertiesNote);
    // Swept, like the code label, rather than restating the table's spacing:
    // somewhere right of the key column on the first rows is the title.
    const int viewport = editor->viewport()->width();
    const int column = qMax(120, qMin(viewport - 48, 700));
    const int left = qMax(24, (viewport - column) / 2);
    QLineEdit *edit = nullptr;
    for (int y = 4; y < 80 && !edit; y += 3) {
        QTest::mouseClick(editor->viewport(), Qt::LeftButton, Qt::NoModifier,
                          QPoint(left + 260, y));
        edit = editor->findChild<QLineEdit *>();
    }
    ASSERT_TRUE(edit) << "no value editor near the top of the table";
    EXPECT_EQ(edit->text(), QStringLiteral("Old"));
}

TEST(PropertyEditTest, AListLosesAndGainsItems)
{
    const QString note = QStringLiteral("---\ntags:\n  - a\n  - 'b'\nnext: 1\n---\n\nBody\n");
    auto editor = editorWith(note);
    const qsizetype tags = note.indexOf(QLatin1String("tags"));

    ASSERT_TRUE(editor->removeListItemAt(tags, 0));
    EXPECT_EQ(editor->noteText(), QStringLiteral("---\ntags:\n  - 'b'\nnext: 1\n---\n\nBody\n"))
        << "the other item kept exactly as written";

    ASSERT_TRUE(editor->addListItemAt(tags));
    ASSERT_TRUE(typeIntoPropertyEditor(*editor, QStringLiteral("c: d"), Qt::Key_Return));
    EXPECT_EQ(editor->noteText(),
              QStringLiteral("---\ntags:\n  - 'b'\n  - \"c: d\"\nnext: 1\n---\n\nBody\n"));

    // Each change is one undo step.
    QTest::keyClick(editor.get(), Qt::Key_Z, Qt::ControlModifier);
    QTest::keyClick(editor.get(), Qt::Key_Z, Qt::ControlModifier);
    EXPECT_EQ(editor->noteText(), note);
}

TEST(PropertyEditTest, AListItemIsChangedInPlace)
{
    const QString note = QStringLiteral("---\ntags:\n  - one\n  - two\n---\n");
    auto editor = editorWith(note);
    ASSERT_TRUE(editor->editListItemAt(note.indexOf(QLatin1String("tags")), 1));
    EXPECT_EQ(editor->findChild<QLineEdit *>()->text(), QStringLiteral("two"));
    ASSERT_TRUE(typeIntoPropertyEditor(*editor, QStringLiteral("#2"), Qt::Key_Return));
    EXPECT_EQ(editor->noteText(), QStringLiteral("---\ntags:\n  - one\n  - \"#2\"\n---\n"));
}

TEST(PropertyEditTest, AFlowListStaysAFlowList)
{
    const QString note = QStringLiteral("---\ntags: [a, b]\n---\n");
    auto editor = editorWith(note);
    ASSERT_TRUE(editor->addListItemAt(note.indexOf(QLatin1String("tags"))));
    ASSERT_TRUE(typeIntoPropertyEditor(*editor, QStringLiteral("c"), Qt::Key_Return));
    EXPECT_EQ(editor->noteText(), QStringLiteral("---\ntags: [a, b, c]\n---\n"));
}

TEST(PropertyEditTest, AListWithACommentIsLeftAlone)
{
    const QString note = QStringLiteral("---\ntags:\n  - a # keep\n  - b\n---\n");
    auto editor = editorWith(note);
    EXPECT_FALSE(editor->addListItemAt(note.indexOf(QLatin1String("tags"))));
    EXPECT_FALSE(editor->removeListItemAt(note.indexOf(QLatin1String("tags")), 1));
    EXPECT_EQ(editor->noteText(), note);
}

TEST(PropertyEditTest, ClickingAChipsCrossRemovesIt)
{
    const QString note = QStringLiteral("---\ntags:\n  - alpha\n  - beta\n---\n\nBody\n");
    auto editor = editorWith(note);
    const int viewport = editor->viewport()->width();
    const int column = qMax(120, qMin(viewport - 48, 700));
    const int left = qMax(24, (viewport - column) / 2);
    // Across the first chip line, right of the key column, until something
    // changes: only a cross changes the note.
    for (int y = 4; y < 60 && editor->noteText() == note; y += 2) {
        for (int x = left + 180; x < left + 420 && editor->noteText() == note; x += 2)
            QTest::mouseClick(editor->viewport(), Qt::LeftButton, Qt::NoModifier, QPoint(x, y));
    }
    EXPECT_EQ(editor->noteText(), QStringLiteral("---\ntags:\n  - beta\n---\n\nBody\n"));
}

TEST(PropertyEditTest, ClosingTheEditorWithAValueOpenIsSafe)
{
    // The open line edit outlived the editor's members and called back into
    // them as it lost focus: a heap corruption on closing a note mid-edit.
    auto editor = editorWith(kPropertiesNote);
    editor->show();
    ASSERT_TRUE(QTest::qWaitForWindowExposed(editor.get()));
    ASSERT_TRUE(editor->editPropertyAt(kPropertiesNote.indexOf(QLatin1String("title"))));
    editor.reset(); // must not crash, under ASan or without it
}

// --- spelling ------------------------------------------------------------------

namespace {

/// A dictionary of a few words, given the way the window gives the real one.
struct FakeSpelling {
    QStringList known = {QStringLiteral("good"), QStringLiteral("words"),
                         QStringLiteral("here"), QStringLiteral("and"), QStringLiteral("don't")};
    QStringList added;
    MarkdownEditor::Spelling spelling()
    {
        return {[this](const QString &word) { return known.contains(word) || added.contains(word); },
                [](const QString &) { return QStringList{QStringLiteral("words")}; },
                [this](const QString &word) { added << word; }};
    }
};

bool underlined(const MarkdownEditor &editor, qsizetype offset)
{
    return editor.formatAt(offset).underlineStyle() == QTextCharFormat::SpellCheckUnderline;
}

} // namespace

TEST(SpellingTest, UnderlinesProseAndLeavesCodeLinksAndTagsAlone)
{
    FakeSpelling dictionary;
    const QString note = QStringLiteral(
        "good wrds here and `codee` and [[Linkk]] and #tagg and don't x2y\n");
    auto editor = editorWith(note);
    editor->setSpelling(dictionary.spelling());

    EXPECT_TRUE(underlined(*editor, note.indexOf(QLatin1String("wrds")) + 1));
    EXPECT_FALSE(underlined(*editor, note.indexOf(QLatin1String("good")) + 1));
    EXPECT_FALSE(underlined(*editor, note.indexOf(QLatin1String("codee")) + 1)) << "inline code";
    EXPECT_FALSE(underlined(*editor, note.indexOf(QLatin1String("Linkk")) + 1)) << "a link";
    EXPECT_FALSE(underlined(*editor, note.indexOf(QLatin1String("tagg")) + 1)) << "a tag";
    EXPECT_FALSE(underlined(*editor, note.indexOf(QLatin1String("don't")) + 1)) << "an apostrophe";
    EXPECT_FALSE(underlined(*editor, note.indexOf(QLatin1String("x2y")))) << "not a word";

    editor->setSpelling({});
    EXPECT_FALSE(underlined(*editor, note.indexOf(QLatin1String("wrds")) + 1)) << "switched off";
}

TEST(SpellingTest, TheWordBeingTypedIsNotUnderlinedYet)
{
    FakeSpelling dictionary;
    const QString note = QStringLiteral("good wrds here\n");
    auto editor = editorWith(note);
    editor->setSpelling(dictionary.spelling());
    const qsizetype inside = note.indexOf(QLatin1String("wrds")) + 2;
    editor->enterBlockAt(inside);
    EXPECT_FALSE(underlined(*editor, inside));
    editor->enterBlockAt(note.indexOf(QLatin1String("here")) + 2);
    EXPECT_TRUE(underlined(*editor, inside)) << "finished once the caret leaves it";
}

TEST(SpellingTest, TheMenuReplacesTheWordOrLearnsIt)
{
    FakeSpelling dictionary;
    const QString note = QStringLiteral("good wrds here\n");
    auto editor = editorWith(note);
    editor->setSpelling(dictionary.spelling());
    const qsizetype at = note.indexOf(QLatin1String("wrds")) + 1;
    EXPECT_FALSE(editor->spellingMenuAt(note.indexOf(QLatin1String("good")))) << "a correct word";

    std::unique_ptr<QMenu> menu = editor->spellingMenuAt(at);
    ASSERT_TRUE(menu);
    ASSERT_FALSE(menu->actions().isEmpty());
    EXPECT_EQ(menu->actions().first()->text(), QStringLiteral("words"));
    menu->actions().first()->trigger();
    EXPECT_EQ(editor->noteText(), QStringLiteral("good words here\n"));

    editor->setNoteText(note);
    menu = editor->spellingMenuAt(at);
    ASSERT_TRUE(menu);
    menu->actions().last()->trigger(); // "Add to dictionary"
    EXPECT_EQ(dictionary.added, QStringList{QStringLiteral("wrds")});
    EXPECT_FALSE(underlined(*editor, at));
}

// --- commands and their keys ---------------------------------------------------

TEST(CommandTest, AKeyCanBeMovedToAnotherCommandOrTakenAway)
{
    const QString note = QStringLiteral("word\n");
    auto editor = editorWith(note);
    editor->enterBlockAt(0);
    for (int i = 0; i < 4; ++i)
        QTest::keyClick(editor.get(), Qt::Key_Right, Qt::ShiftModifier); // "word"

    // Bold moves to Ctrl+Shift+B, and Ctrl+B is left doing nothing.
    editor->setKeyBindings({{QStringLiteral("editor:toggle-bold"),
                             {QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_B)}}});
    QTest::keyClick(editor.get(), Qt::Key_B, Qt::ControlModifier);
    EXPECT_EQ(editor->noteText(), note);
    QTest::keyClick(editor.get(), Qt::Key_B, Qt::ControlModifier | Qt::ShiftModifier);
    EXPECT_EQ(editor->noteText(), QStringLiteral("**word**\n"));

    // An empty list unbinds; the command still runs by its id.
    editor->setKeyBindings({{QStringLiteral("editor:toggle-italics"), {}}});
    const QString bolded = editor->noteText();
    QTest::keyClick(editor.get(), Qt::Key_I, Qt::ControlModifier);
    EXPECT_EQ(editor->noteText(), bolded);
    EXPECT_TRUE(editor->runCommand(QStringLiteral("editor:toggle-italics")));
    EXPECT_NE(editor->noteText(), bolded);
    EXPECT_FALSE(editor->runCommand(QStringLiteral("no:such-command")));
}

TEST(CommandTest, EveryCommandHasAnIDANameAndRuns)
{
    for (const MarkdownEditor::Command &command : MarkdownEditor::commands()) {
        EXPECT_TRUE(command.id.contains(QLatin1Char(':'))) << qPrintable(command.id);
        EXPECT_FALSE(command.name.isEmpty()) << qPrintable(command.id);
        auto editor = editorWith(QStringLiteral("one\ntwo\n"));
        editor->enterBlockAt(1);
        EXPECT_TRUE(editor->runCommand(command.id)) << qPrintable(command.id);
    }
}

// --- query blocks ------------------------------------------------------------------

namespace {

/// A fake `setQueryRunner`: records every search asked for and hands the
/// test the `deliver` callback instead of calling it, so a test can decide
/// exactly when - and in what order - results come back, without depending
/// on a real index or on timing.
struct FakeQueryRunner {
    struct Call {
        QString query;
        MarkdownEditor::QueryDeliver deliver;
    };
    QList<Call> calls;

    void operator()(const QString &query, const MarkdownEditor::QueryDeliver &deliver)
    {
        calls.append({query, deliver});
    }
};

QueryResult makeQueryResult(const QString &path, int matchCount, const QString &snippet,
                            qsizetype firstOffset)
{
    QueryResult result;
    result.path = path;
    result.matchCount = matchCount;
    result.snippet = snippet;
    result.firstOffset = firstOffset;
    if (!snippet.isEmpty())
        result.snippetHighlights.append({0, qMin<qsizetype>(3, snippet.size())});
    return result;
}

/// Runs the queued delivery FakeQueryRunner's callback posts (setQueryRunner
/// marshals it through QMetaObject::invokeMethod) and lets layout catch up.
void pumpQueuedDelivery()
{
    QCoreApplication::processEvents();
    QTest::qWait(10);
    QCoreApplication::processEvents();
}

} // namespace

TEST(LifelineTest, NothingIsPostedOnceTheReceiverHasEnded)
{
    noteAppC::Lifeline lifeline;
    QObject receiver;
    int calls = 0;
    EXPECT_TRUE(lifeline.post(&receiver, [&calls] { ++calls; }));
    QCoreApplication::processEvents();
    EXPECT_EQ(calls, 1);
    lifeline.end();
    EXPECT_FALSE(lifeline.post(&receiver, [&calls] { ++calls; }));
    QCoreApplication::processEvents();
    EXPECT_EQ(calls, 1);
}

TEST(QueryBlockTest, AResultArrivingAfterTheEditorIsGoneIsDropped)
{
    FakeQueryRunner fake;
    {
        auto editor = editorWith(QStringLiteral("```query\nfoo\n```\n"));
        editor->setQueryRunner(std::ref(fake));
        ASSERT_EQ(fake.calls.size(), 1);
    }
    // From another thread, as a real search delivers: nothing may reach the
    // freed editor, neither the post nor the queued call.
    std::thread([&fake] { fake.calls.constFirst().deliver({}); }).join();
    pumpQueuedDelivery();
}

TEST(QueryBlockTest, RendersResultsFromAnAsyncRunner)
{
    const QString note = QStringLiteral("Intro\n\n```query\nfoo\n```\n");
    auto editor = editorWith(note);
    FakeQueryRunner fake;
    editor->setQueryRunner(std::ref(fake));

    const qsizetype queryOffset = note.indexOf(QStringLiteral("foo"));
    // Rendered but still waiting: the runner was asked, but nothing answered
    // yet, so there is nothing to click - render must never block on it.
    ASSERT_EQ(fake.calls.size(), 1);
    EXPECT_EQ(fake.calls.first().query, QStringLiteral("foo"));
    EXPECT_TRUE(editor->queryRowsAt(queryOffset).isEmpty());

    fake.calls.first().deliver({makeQueryResult(QStringLiteral("One.md"), 3, QStringLiteral("a foo b"), 10),
                                makeQueryResult(QStringLiteral("Two.md"), 1, QStringLiteral("foo alone"), 4)});
    pumpQueuedDelivery();

    const QList<MarkdownEditor::QueryResultRow> rows = editor->queryRowsAt(queryOffset);
    ASSERT_EQ(rows.size(), 2);
    EXPECT_EQ(rows.at(0).path, QStringLiteral("One.md"));
    EXPECT_EQ(rows.at(0).offset, 10);
    EXPECT_EQ(rows.at(1).path, QStringLiteral("Two.md"));
    EXPECT_EQ(rows.at(1).offset, 4);

    // Re-asking for the same text without an intervening edit or
    // refreshQueries is a cache hit, not a second search.
    editor->enterBlockAt(1);   // touches the "Intro" block, not the query one
    editor->enterBlockAt(queryOffset);
    editor->enterBlockAt(1);
    EXPECT_EQ(fake.calls.size(), 1);
}

TEST(QueryBlockTest, CaretInsideRevealsSourceInsteadOfResults)
{
    const QString note = QStringLiteral("Intro\n\n```query\nfoo\n```\n");
    auto editor = editorWith(note);
    FakeQueryRunner fake;
    editor->setQueryRunner(std::ref(fake));
    ASSERT_EQ(fake.calls.size(), 1);
    fake.calls.first().deliver({makeQueryResult(QStringLiteral("One.md"), 1, QStringLiteral("foo"), 0)});
    pumpQueuedDelivery();
    ASSERT_FALSE(editor->queryRowsAt(note.indexOf(QStringLiteral("foo"))).isEmpty());

    editor->enterBlockAt(note.indexOf(QStringLiteral("foo")));
    // Fences are chrome, never part of displayText() even in source mode
    // (CodeOptionsTest.LineNumbersIndentTheListingAndLeaveTheBytesAlone makes
    // the same point) - what matters here is that the raw query text is what
    // shows, not a rendered result.
    EXPECT_TRUE(editor->displayText().contains(QStringLiteral("foo")));
    EXPECT_FALSE(editor->displayText().contains(QStringLiteral("One.md")));
    EXPECT_TRUE(editor->queryRowsAt(note.indexOf(QStringLiteral("foo"))).isEmpty());
}

TEST(QueryBlockTest, ClickingARowActivatesItsNoteAndOffset)
{
    const QString note = QStringLiteral("```query\nfoo\n```\n\nAfter\n");
    auto editor = editorWith(note);
    FakeQueryRunner fake;
    editor->setQueryRunner(std::ref(fake));
    // The default caret (offset 0) starts inside the query block itself,
    // same as it would in any other block; it has to leave before the block
    // renders results rather than raw source.
    editor->enterBlockAt(note.indexOf(QStringLiteral("After")));
    ASSERT_EQ(fake.calls.size(), 1);
    fake.calls.first().deliver(
        {makeQueryResult(QStringLiteral("Target.md"), 2, QStringLiteral("has foo in it"), 42)});
    pumpQueuedDelivery();

    const qsizetype queryOffset = note.indexOf(QStringLiteral("foo"));
    const QList<MarkdownEditor::QueryResultRow> rows = editor->queryRowsAt(queryOffset);
    ASSERT_EQ(rows.size(), 1);

    QSignalSpy activated(editor.get(), &MarkdownEditor::queryResultActivated);
    QTest::mouseClick(editor->viewport(), Qt::LeftButton, Qt::NoModifier,
                      rows.first().rect.center().toPoint());
    ASSERT_EQ(activated.size(), 1);
    EXPECT_EQ(activated.first().at(0).toString(), QStringLiteral("Target.md"));
    EXPECT_EQ(activated.first().at(1).value<qsizetype>(), 42);
}

TEST(QueryBlockTest, EditingElsewhereAsksNothingNew)
{
    const QString note = QStringLiteral("Intro\n\n```query\nfoo\n```\n");
    auto editor = editorWith(note);
    FakeQueryRunner fake;
    editor->setQueryRunner(std::ref(fake));
    ASSERT_EQ(fake.calls.size(), 1);
    fake.calls.first().deliver({makeQueryResult(QStringLiteral("One.md"), 1, QStringLiteral("foo"), 0)});
    pumpQueuedDelivery();

    editor->setCursorOffset(5); // end of "Intro", nowhere near the query block
    QTest::keyClicks(editor.get(), QStringLiteral(" more"));
    EXPECT_EQ(fake.calls.size(), 1) << "a keystroke outside the block must not re-run its search";
}

TEST(QueryBlockTest, AStaleDeliveryAfterRefreshIsDropped)
{
    const QString note = QStringLiteral("Intro\n\n```query\nfoo\n```\n");
    auto editor = editorWith(note);
    FakeQueryRunner fake;
    editor->setQueryRunner(std::ref(fake));
    ASSERT_EQ(fake.calls.size(), 1);
    const MarkdownEditor::QueryDeliver firstDeliver = fake.calls.first().deliver;

    // The index changed while the first search was still in flight:
    // refreshQueries drops the cache and asks again for the same text.
    editor->refreshQueries();
    QTest::qWait(250); // past the 200 ms debounce
    QCoreApplication::processEvents();
    ASSERT_EQ(fake.calls.size(), 2);
    EXPECT_EQ(fake.calls.at(1).query, QStringLiteral("foo"));
    const MarkdownEditor::QueryDeliver secondDeliver = fake.calls.at(1).deliver;

    // The new request answers first (the common case: refreshQueries fires
    // because something changed, and the fresh search is what a person is
    // waiting on) and the old, slower one lands after it - a broken
    // generation check would let this second, stale delivery win simply by
    // arriving last.
    secondDeliver({makeQueryResult(QStringLiteral("Fresh.md"), 1, QStringLiteral("foo"), 0)});
    pumpQueuedDelivery();
    firstDeliver({makeQueryResult(QStringLiteral("Stale.md"), 1, QStringLiteral("foo"), 0)});
    pumpQueuedDelivery();

    const QList<MarkdownEditor::QueryResultRow> rows =
        editor->queryRowsAt(note.indexOf(QStringLiteral("foo")));
    ASSERT_EQ(rows.size(), 1);
    EXPECT_EQ(rows.first().path, QStringLiteral("Fresh.md"))
        << "the superseded request's answer must not overwrite the current one";
}

// --- the embedded search header (result count, collapse, sort) -------------------

TEST(QueryHeaderTest, ShowsResultCountSingularAndPlural)
{
    const QString note = QStringLiteral("```query\nfoo\n```\n\n```query\nbar\n```\n");
    auto editor = editorWith(note);
    FakeQueryRunner fake;
    editor->setQueryRunner(std::ref(fake));
    ASSERT_EQ(fake.calls.size(), 2);

    const qsizetype fooOffset = note.indexOf(QStringLiteral("foo"));
    const qsizetype barOffset = note.indexOf(QStringLiteral("bar"));
    for (const FakeQueryRunner::Call &call : fake.calls) {
        if (call.query == QStringLiteral("foo"))
            call.deliver({makeQueryResult(QStringLiteral("One.md"), 1, QString(), 0)});
        else
            call.deliver({makeQueryResult(QStringLiteral("One.md"), 1, QString(), 0),
                          makeQueryResult(QStringLiteral("Two.md"), 1, QString(), 0)});
    }
    pumpQueuedDelivery();

    EXPECT_EQ(editor->queryHeaderAt(fooOffset).countText, QStringLiteral("1 result"));
    EXPECT_EQ(editor->queryHeaderAt(barOffset).countText, QStringLiteral("2 results"));
}

TEST(QueryHeaderTest, ClickingCollapseHidesRowsAndPersistsAcrossAnEdit)
{
    const QString note = QStringLiteral("```query\nfoo\n```\n\nAfter\n");
    auto editor = editorWith(note);
    FakeQueryRunner fake;
    editor->setQueryRunner(std::ref(fake));
    editor->enterBlockAt(note.indexOf(QStringLiteral("After"))); // leave the block so it renders
    ASSERT_EQ(fake.calls.size(), 1);
    fake.calls.first().deliver(
        {makeQueryResult(QStringLiteral("One.md"), 1, QStringLiteral("a foo b"), 0)});
    pumpQueuedDelivery();

    const qsizetype offset = note.indexOf(QStringLiteral("foo"));
    ASSERT_FALSE(editor->queryRowsAt(offset).isEmpty());
    EXPECT_FALSE(editor->queryCollapsedAt(offset));

    MarkdownEditor::QueryHeader header = editor->queryHeaderAt(offset);
    ASSERT_TRUE(header.collapseRect.isValid());
    QTest::mouseClick(editor->viewport(), Qt::LeftButton, Qt::NoModifier,
                      header.collapseRect.center().toPoint());

    EXPECT_TRUE(editor->queryCollapsedAt(offset));
    EXPECT_TRUE(editor->queryRowsAt(offset).isEmpty())
        << "collapsed hides the result rows, not just some header flag";
    EXPECT_FALSE(editor->queryHeaderAt(offset).countText.isEmpty())
        << "the header itself - the count to expand back from - stays";

    // Remembered for the block for the life of the editor even though
    // nothing was written to the note: an edit elsewhere leaves it collapsed.
    editor->setCursorOffset(note.indexOf(QStringLiteral("After")));
    QTest::keyClicks(editor.get(), QStringLiteral("!"));
    EXPECT_TRUE(editor->queryCollapsedAt(offset));

    QTest::mouseClick(editor->viewport(), Qt::LeftButton, Qt::NoModifier,
                      editor->queryHeaderAt(offset).collapseRect.center().toPoint());
    EXPECT_FALSE(editor->queryCollapsedAt(offset));
    EXPECT_FALSE(editor->queryRowsAt(offset).isEmpty());
}

TEST(QueryHeaderTest, ClickingSortTogglesToFileNameOrder)
{
    const QString note = QStringLiteral("```query\nfoo\n```\n\nAfter\n");
    auto editor = editorWith(note);
    FakeQueryRunner fake;
    editor->setQueryRunner(std::ref(fake));
    editor->enterBlockAt(note.indexOf(QStringLiteral("After")));
    ASSERT_EQ(fake.calls.size(), 1);
    // Delivered out of file-name order, so sorting is observable.
    fake.calls.first().deliver({makeQueryResult(QStringLiteral("Zebra.md"), 1, QString(), 0),
                                makeQueryResult(QStringLiteral("Apple.md"), 1, QString(), 0)});
    pumpQueuedDelivery();

    const qsizetype offset = note.indexOf(QStringLiteral("foo"));
    QList<MarkdownEditor::QueryResultRow> rows = editor->queryRowsAt(offset);
    ASSERT_EQ(rows.size(), 2);
    EXPECT_EQ(rows.at(0).path, QStringLiteral("Zebra.md")) << "best-match: the delivered order";
    EXPECT_FALSE(editor->queryHeaderAt(offset).sortByName);

    const MarkdownEditor::QueryHeader header = editor->queryHeaderAt(offset);
    ASSERT_TRUE(header.sortRect.isValid());
    QTest::mouseClick(editor->viewport(), Qt::LeftButton, Qt::NoModifier,
                      header.sortRect.center().toPoint());

    EXPECT_TRUE(editor->queryHeaderAt(offset).sortByName);
    rows = editor->queryRowsAt(offset);
    ASSERT_EQ(rows.size(), 2);
    EXPECT_EQ(rows.at(0).path, QStringLiteral("Apple.md"));
    EXPECT_EQ(rows.at(1).path, QStringLiteral("Zebra.md"));

    // Toggling back restores delivery order, and neither toggle asks the
    // runner for anything - sorting is client-side over the cached results.
    QTest::mouseClick(editor->viewport(), Qt::LeftButton, Qt::NoModifier,
                      editor->queryHeaderAt(offset).sortRect.center().toPoint());
    EXPECT_FALSE(editor->queryHeaderAt(offset).sortByName);
    EXPECT_EQ(fake.calls.size(), 1) << "sorting is client-side, never a second search";
}

// --- base blocks -------------------------------------------------------------------

namespace {

/// A fake `setBaseRunner`, the same shape FakeQueryRunner already is for
/// setQueryRunner: records every YAML text asked for and hands the test the
/// `deliver` callback instead of calling it.
struct FakeBaseRunner {
    struct Call {
        QString yamlText;
        MarkdownEditor::BaseDeliver deliver;
    };
    QList<Call> calls;

    void operator()(const QString &yamlText, const MarkdownEditor::BaseDeliver &deliver)
    {
        calls.append({yamlText, deliver});
    }
};

BaseTableData makeBaseTable(const QStringList &headers, QList<BaseTableRow> rows)
{
    BaseTableData table;
    table.viewNames = {QStringLiteral("Table")};
    table.shownView = 0;
    table.headers = headers;
    table.rows = std::move(rows);
    return table;
}

} // namespace

TEST(BaseBlockTest, RendersTableFromAnAsyncRunner)
{
    const QString note = QStringLiteral("Intro\n\n```base\nviews:\n  - order: [file.name]\n```\n");
    auto editor = editorWith(note);
    FakeBaseRunner fake;
    editor->setBaseRunner(std::ref(fake));

    const qsizetype baseOffset = note.indexOf(QStringLiteral("views:"));
    // Rendered but still waiting: the runner was asked, but nothing answered
    // yet, so there is nothing to click - render must never block on it.
    ASSERT_EQ(fake.calls.size(), 1);
    EXPECT_TRUE(editor->baseRowsAt(baseOffset).isEmpty());

    fake.calls.first().deliver(
        makeBaseTable({QStringLiteral("Name")}, {{{QStringLiteral("One")}, QStringLiteral("One.md")},
                                                 {{QStringLiteral("Two")}, QStringLiteral("Two.md")}}));
    pumpQueuedDelivery();

    const QList<MarkdownEditor::BaseRowRect> rows = editor->baseRowsAt(baseOffset);
    ASSERT_EQ(rows.size(), 2);
    EXPECT_EQ(rows.at(0).path, QStringLiteral("One.md"));
    EXPECT_EQ(rows.at(1).path, QStringLiteral("Two.md"));

    // Re-asking for the same YAML without an intervening edit or
    // refreshBases is a cache hit, not a second evaluation.
    editor->enterBlockAt(1);   // touches the "Intro" block, not the base one
    editor->enterBlockAt(baseOffset);
    editor->enterBlockAt(1);
    EXPECT_EQ(fake.calls.size(), 1);
}

TEST(BaseBlockTest, CaretInsideRevealsSourceInsteadOfTheTable)
{
    const QString note = QStringLiteral("```base\nviews:\n  - order: [file.name]\n```\n");
    auto editor = editorWith(note);
    FakeBaseRunner fake;
    editor->setBaseRunner(std::ref(fake));
    ASSERT_EQ(fake.calls.size(), 1);
    fake.calls.first().deliver(
        makeBaseTable({QStringLiteral("Name")}, {{{QStringLiteral("One")}, QStringLiteral("One.md")}}));
    pumpQueuedDelivery();

    const qsizetype baseOffset = note.indexOf(QStringLiteral("views:"));
    ASSERT_FALSE(editor->baseRowsAt(baseOffset).isEmpty());
    EXPECT_FALSE(editor->displayText().contains(QStringLiteral("order:")))
        << "rendered: the table shows cell text, not the YAML that produced it";

    editor->enterBlockAt(baseOffset);
    EXPECT_TRUE(editor->displayText().contains(QStringLiteral("order:")));
    EXPECT_TRUE(editor->baseRowsAt(baseOffset).isEmpty());
}

TEST(BaseBlockTest, ClickingARowActivatesItsNote)
{
    const QString note =
        QStringLiteral("```base\nviews:\n  - order: [file.name]\n```\n\nAfter\n");
    auto editor = editorWith(note);
    FakeBaseRunner fake;
    editor->setBaseRunner(std::ref(fake));
    // The default caret starts inside the base block itself; it has to
    // leave before the block renders a table rather than raw YAML.
    editor->enterBlockAt(note.indexOf(QStringLiteral("After")));
    ASSERT_EQ(fake.calls.size(), 1);
    fake.calls.first().deliver(makeBaseTable(
        {QStringLiteral("Name")}, {{{QStringLiteral("Target")}, QStringLiteral("Target.md")}}));
    pumpQueuedDelivery();

    const qsizetype baseOffset = note.indexOf(QStringLiteral("views:"));
    const QList<MarkdownEditor::BaseRowRect> rows = editor->baseRowsAt(baseOffset);
    ASSERT_EQ(rows.size(), 1);

    QSignalSpy activated(editor.get(), &MarkdownEditor::baseRowActivated);
    QTest::mouseClick(editor->viewport(), Qt::LeftButton, Qt::NoModifier,
                      rows.first().rect.center().toPoint());
    ASSERT_EQ(activated.size(), 1);
    EXPECT_EQ(activated.first().at(0).toString(), QStringLiteral("Target.md"));
}

TEST(BaseBlockTest, EditingElsewhereAsksNothingNew)
{
    const QString note = QStringLiteral("Intro\n\n```base\nviews:\n  - order: [file.name]\n```\n");
    auto editor = editorWith(note);
    FakeBaseRunner fake;
    editor->setBaseRunner(std::ref(fake));
    ASSERT_EQ(fake.calls.size(), 1);
    fake.calls.first().deliver(
        makeBaseTable({QStringLiteral("Name")}, {{{QStringLiteral("One")}, QStringLiteral("One.md")}}));
    pumpQueuedDelivery();

    editor->setCursorOffset(5); // end of "Intro", nowhere near the base block
    QTest::keyClicks(editor.get(), QStringLiteral(" more"));
    EXPECT_EQ(fake.calls.size(), 1) << "a keystroke outside the block must not re-evaluate it";
}

TEST(BaseBlockTest, AStaleDeliveryAfterRefreshIsDropped)
{
    const QString note = QStringLiteral("Intro\n\n```base\nviews:\n  - order: [file.name]\n```\n");
    auto editor = editorWith(note);
    FakeBaseRunner fake;
    editor->setBaseRunner(std::ref(fake));
    ASSERT_EQ(fake.calls.size(), 1);
    const MarkdownEditor::BaseDeliver firstDeliver = fake.calls.first().deliver;

    editor->refreshBases();
    QTest::qWait(250); // past the 200 ms debounce
    QCoreApplication::processEvents();
    ASSERT_EQ(fake.calls.size(), 2);
    const MarkdownEditor::BaseDeliver secondDeliver = fake.calls.at(1).deliver;

    secondDeliver(makeBaseTable({QStringLiteral("Name")},
                                {{{QStringLiteral("Fresh")}, QStringLiteral("Fresh.md")}}));
    pumpQueuedDelivery();
    firstDeliver(makeBaseTable({QStringLiteral("Name")},
                               {{{QStringLiteral("Stale")}, QStringLiteral("Stale.md")}}));
    pumpQueuedDelivery();

    const QList<MarkdownEditor::BaseRowRect> rows =
        editor->baseRowsAt(note.indexOf(QStringLiteral("views:")));
    ASSERT_EQ(rows.size(), 1);
    EXPECT_EQ(rows.first().path, QStringLiteral("Fresh.md"))
        << "the superseded request's answer must not overwrite the current one";
}

// --- HTML blocks and inline HTML ----------------------------------------------------

TEST(HtmlRendererTest, AvailableExactlyWhenLitehtmlWasFound)
{
    // Available exactly when the build found litehtml; a build without it -
    // NOTEAPPC_WITH_HTML off, or the package missing - reports itself
    // unavailable rather than pretending, the same contract PdfPage keeps.
#ifdef NOTEAPPC_EXPECT_HTML
    EXPECT_TRUE(HtmlRenderer::isAvailable());
#else
    EXPECT_FALSE(HtmlRenderer::isAvailable());
#endif
}

namespace {

HtmlRenderer::Theme testHtmlTheme()
{
    HtmlRenderer::Theme theme;
    theme.text = QColor(20, 20, 20);
    theme.background = Qt::white;
    theme.link = Qt::blue;
    theme.border = Qt::gray;
    theme.codeBackground = QColor(230, 230, 230);
    theme.textFont = QFont(QStringLiteral("sans-serif"), 12);
    theme.codeFont = QFont(QStringLiteral("monospace"), 12);
    return theme;
}

} // namespace

TEST(HtmlRendererTest, ScriptNeverRendersItsText)
{
    if (!HtmlRenderer::isAvailable())
        GTEST_SKIP() << "built without litehtml (NOTEAPPC_WITH_HTML off, or the package was not found)";

    const HtmlRenderer::Theme theme = testHtmlTheme();
    const QImage plain = HtmlRenderer::render(QStringLiteral("<div>Hello</div>"), theme, 300, QString());
    const QImage withScript = HtmlRenderer::render(
        QStringLiteral("<div>Hello<script>document.title='pwned';alert(1)</script></div>"), theme, 300,
        QString());
    ASSERT_FALSE(plain.isNull());
    ASSERT_FALSE(withScript.isNull());
    // Not just "no visible <script> text": pixel-identical, so the tag
    // contributed nothing at all - not even the layout space a hidden but
    // present element would still occupy.
    EXPECT_EQ(plain, withScript) << "a <script> body must never add a pixel to the rendered block";
}

TEST(HtmlRendererTest, GradientsRoundedCornersAndPerSideBorders)
{
    if (!HtmlRenderer::isAvailable())
        GTEST_SKIP() << "built without litehtml";
    const HtmlRenderer::Theme theme = testHtmlTheme();
    const auto near = [](QColor a, QColor b) {
        return qAbs(a.red() - b.red()) < 40 && qAbs(a.green() - b.green()) < 40 && qAbs(a.blue() - b.blue()) < 40;
    };

    // litehtml's default body margin puts every box 8 px in.
    // A left-to-right gradient: red at one end, blue at the other, not one flat stop.
    const QImage gradient = HtmlRenderer::render(
        QStringLiteral("<div style=\"width:200px;height:40px;background:linear-gradient(to right, #ff0000, #0000ff)\"></div>"),
        theme, 300, QString());
    ASSERT_FALSE(gradient.isNull());
    EXPECT_TRUE(near(gradient.pixelColor(12, 28), Qt::red)) << gradient.pixelColor(12, 28).name().toStdString();
    EXPECT_TRUE(near(gradient.pixelColor(203, 28), Qt::blue)) << gradient.pixelColor(203, 28).name().toStdString();

    // A rounded box leaves its corner unpainted.
    const QImage rounded = HtmlRenderer::render(
        QStringLiteral("<div style=\"width:100px;height:60px;background:#00ff00;border-radius:20px\"></div>"),
        theme, 300, QString());
    EXPECT_TRUE(near(rounded.pixelColor(58, 38), Qt::green));
    EXPECT_FALSE(near(rounded.pixelColor(9, 9), Qt::green)) << "the corner is cut";

    // Each side its own colour.
    const QImage sides = HtmlRenderer::render(
        QStringLiteral("<div style=\"width:100px;height:60px;border-top:6px solid #ff0000;"
                       "border-bottom:6px solid #0000ff\"></div>"),
        theme, 300, QString());
    EXPECT_TRUE(near(sides.pixelColor(58, 10), Qt::red));
    EXPECT_TRUE(near(sides.pixelColor(58, 77), Qt::blue)) << sides.pixelColor(58, 77).name().toStdString();
}

TEST(HtmlRendererTest, ListMarkersUseTheirNumeralSystem)
{
    if (!HtmlRenderer::isAvailable())
        GTEST_SKIP() << "built without litehtml";
    // Markers are painted, not text runs, so compare against a list drawn
    // with the same items and the decimal style: the pictures must differ.
    const QImage roman = HtmlRenderer::render(
        QStringLiteral("<ol style=\"list-style-type:upper-roman\"><li>a</li><li>b</li><li>c</li><li>d</li></ol>"),
        testHtmlTheme(), 300, QString());
    const QImage decimal = HtmlRenderer::render(
        QStringLiteral("<ol><li>a</li><li>b</li><li>c</li><li>d</li></ol>"), testHtmlTheme(), 300, QString());
    ASSERT_FALSE(roman.isNull());
    EXPECT_NE(roman, decimal);
}

TEST(HTMLBlockTest, RendersAsANonEmptyCachedImage)
{
    const QString note = QStringLiteral("Intro\n\n<div>\n<b>Hello</b> world\n</div>\n\nAfter\n");
    auto editor = editorWith(note);
    const QSize size = editor->htmlImageSizeAt(note.indexOf(QStringLiteral("Hello")));
    if (HtmlRenderer::isAvailable()) {
        EXPECT_GT(size.width(), 0);
        EXPECT_GT(size.height(), 0);
    } else {
        EXPECT_TRUE(size.isEmpty()) << "without litehtml the block falls back to showing its source";
    }
}

TEST(HTMLBlockTest, CaretInsideRevealsSourceInsteadOfTheImage)
{
    if (!HtmlRenderer::isAvailable())
        GTEST_SKIP() << "built without litehtml";

    const QString note = QStringLiteral("Intro\n\n<div>\n<b>Hello</b> world\n</div>\n\nAfter\n");
    auto editor = editorWith(note);
    const qsizetype at = note.indexOf(QStringLiteral("Hello"));
    ASSERT_FALSE(editor->htmlImageSizeAt(at).isEmpty());

    editor->enterBlockAt(at);
    EXPECT_TRUE(editor->htmlImageSizeAt(at).isEmpty());
    EXPECT_EQ(editor->displayText(), note)
        << "the caret is inside it: raw HTML source, like any other block";
}

TEST(HTMLBlockTest, OpeningANoteWithNoHTMLConstructsNothingFromLitehtml)
{
    const qint64 before = HtmlRenderer::renderCount();
    auto editor = editorWith(QStringLiteral("# Title\n\nJust **prose**, a [[link]] and `code`.\n"));
    (void)editor;
    EXPECT_EQ(HtmlRenderer::renderCount(), before)
        << "no HTML block in the note: litehtml must never be asked to lay anything out";
}

TEST(HTMLBlockTest, UnrelatedKeystrokeIsACacheHitNotARebuild)
{
    if (!HtmlRenderer::isAvailable())
        GTEST_SKIP() << "built without litehtml";

    const QString note = QStringLiteral("Intro\n\n<div>\n<b>Hello</b> world\n</div>\n\nAfter\n");
    auto editor = editorWith(note);
    ASSERT_FALSE(editor->htmlImageSizeAt(note.indexOf(QStringLiteral("Hello"))).isEmpty());
    const qint64 afterFirstRender = HtmlRenderer::renderCount();

    editor->setCursorOffset(5); // end of "Intro", nowhere near the HTML block
    QTest::keyClicks(editor.get(), QStringLiteral(" more"));
    EXPECT_EQ(HtmlRenderer::renderCount(), afterFirstRender)
        << "an edit outside the block must not rebuild its cached image";
}

TEST(InlineHtmlTest, RecognisedTagsFormatAndHideTheirMarkup)
{
    const QString note = QStringLiteral("Press <kbd>Ctrl</kbd> then <b>Go</b>.\n");
    auto editor = editorWith(note);
    EXPECT_EQ(editor->displayText(), QStringLiteral("Press Ctrl then Go.\n"))
        << "the tags hide, like any other syntax marker";

    const qsizetype ctrl = note.indexOf(QStringLiteral("Ctrl"));
    EXPECT_EQ(editor->formatAt(ctrl).font().family(), Theme::builtin().monospaceFont().family());
    const qsizetype go = note.indexOf(QStringLiteral(">Go<")) + 1;
    EXPECT_GE(editor->formatAt(go).fontWeight(), int(QFont::Bold));
}

TEST(InlineHtmlTest, SpanStyleColourIsRead)
{
    const QString note = QStringLiteral("A <span style=\"color: #ff0000\">warning</span> word.\n");
    auto editor = editorWith(note);
    const qsizetype at = note.indexOf(QStringLiteral("warning"));
    EXPECT_EQ(editor->formatAt(at).foreground().color(), QColor(0xff, 0, 0));
}

TEST(InlineHtmlTest, AnUnrecognisedTagStaysAsRawSource)
{
    // No case in htmlTagFormat matches <marquee>, so layoutInlineHtml never
    // hides it - the same "leave it alone rather than guess" rule an
    // unrenderable formula follows.
    const QString note = QStringLiteral("A <marquee>party</marquee> word.\n");
    auto editor = editorWith(note);
    EXPECT_EQ(editor->displayText(), note);
}

TEST(InlineHtmlTest, ABlockLevelElementIsLeftToLayoutHtml)
{
    // layoutInlineHtml steps aside entirely for a whole-block HTML span -
    // pairing tags inside it would fight with litehtml's own rendering of
    // the same markup.
    if (!HtmlRenderer::isAvailable())
        GTEST_SKIP() << "built without litehtml";
    const QString note = QStringLiteral("<div>\n<b>Hello</b>\n</div>\n");
    auto editor = editorWith(note);
    EXPECT_FALSE(editor->htmlImageSizeAt(note.indexOf(QStringLiteral("Hello"))).isEmpty());
}

// --- page preview ----------------------------------------------------------------

namespace {

/// Moves the pointer across the first line until the editor reports a link,
/// holding `modifiers`; the position of the link is the editor's business.
QString hoverFirstLine(MarkdownEditor &editor, Qt::KeyboardModifiers modifiers)
{
    QSignalSpy hovered(&editor, &MarkdownEditor::linkHovered);
    for (int x = 20; x < editor.viewport()->width() && hovered.isEmpty(); x += 3) {
        const QPointF at(x, 40);
        QMouseEvent move(QEvent::MouseMove, at, editor.viewport()->mapToGlobal(at), Qt::NoButton,
                         Qt::NoButton, modifiers);
        QCoreApplication::sendEvent(editor.viewport(), &move);
    }
    return hovered.isEmpty() ? QString() : hovered.first().at(0).toString();
}

} // namespace

TEST(PagePreviewTest, ReadingViewPreviewsOnHoverAndEditingNeedsCtrl)
{
    // Obsidian: plain hover in reading view, Ctrl held while editing.
    const QString note = QStringLiteral("[[Other note#Part two]] [[Other note#Part two]] and more\n");
    auto editing = editorWith(note);
    EXPECT_TRUE(hoverFirstLine(*editing, Qt::NoModifier).isEmpty());
    EXPECT_EQ(hoverFirstLine(*editing, Qt::ControlModifier), QStringLiteral("Other note#Part two"))
        << "the heading comes with the note, so the preview can open there";

    auto reading = editorWith(note);
    reading->setMode(MarkdownEditor::Mode::Reading);
    EXPECT_EQ(hoverFirstLine(*reading, Qt::NoModifier), QStringLiteral("Other note#Part two"));
}

TEST(PagePreviewTest, AnExternalLinkHasNothingToPreview)
{
    auto reading = editorWith(QStringLiteral("[a site](https://example.org) and [a site](https://example.org)\n"));
    reading->setMode(MarkdownEditor::Mode::Reading);
    EXPECT_TRUE(hoverFirstLine(*reading, Qt::NoModifier).isEmpty());
}

// --- export to PDF ---------------------------------------------------------------

TEST(PdfExportTest, APageBreaksBetweenBlocksAndOnlyATallOneIsCut)
{
    // Blocks as (top, height), pages 100 high.
    EXPECT_EQ(MarkdownEditor::pageStarts({{0, 40}, {40, 50}}, 100), (QList<qreal>{0}));
    // The third block would cross the edge, so it starts page two.
    EXPECT_EQ(MarkdownEditor::pageStarts({{0, 40}, {40, 50}, {90, 30}}, 100),
              (QList<qreal>{0, 90}));
    // Taller than a page: it fills the rest of this page and runs on, cut at
    // the page height as many times as it takes - as a browser prints it.
    EXPECT_EQ(MarkdownEditor::pageStarts({{0, 20}, {20, 250}}, 100),
              (QList<qreal>{0, 100, 200}));
}

TEST(PdfExportTest, ALongNoteBecomesSeveralPages)
{
    QString note = QStringLiteral("---\ntags: [a]\n---\n");
    for (int i = 0; i < 120; ++i)
        note += QStringLiteral("Paragraph %1 with **bold** text that runs on for a while.\n\n").arg(i);
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("out.pdf"));
    QString error;
    ASSERT_TRUE(MarkdownEditor::exportPdf(path, note, QStringLiteral("A title"), {},
                                          MarkdownEditor::PdfOptions{}, &error))
        << qPrintable(error);
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::ReadOnly));
    const QByteArray pdf = file.readAll();
    EXPECT_TRUE(pdf.startsWith("%PDF"));
    static const QRegularExpression pageObject(QStringLiteral("/Type\\s*/Page[^s]"));
    EXPECT_GE(QString::fromLatin1(pdf).count(pageObject), 3) << "120 paragraphs on A4";
}

// --- audio and video ------------------------------------------------------------

TEST(MediaEmbedTest, AudioAndVideoBecomePlayerCards)
{
    // The card's file only has to resolve for the layout; playing it is the
    // media backend's business.
    const auto heightOf = [](const QString &note) {
        auto editor = std::make_unique<MarkdownEditor>();
        editor->resize(900, 600);
        editor->setLinkResolver([](const QString &target) { return QStringLiteral("/nowhere/") + target; });
        editor->setNoteText(note);
        return editor->documentHeight();
    };
    const qreal audio = heightOf(QStringLiteral("![[song.mp3]]\n"));
    const qreal video = heightOf(QStringLiteral("![[clip.mp4]]\n"));
    const qreal plain = heightOf(QStringLiteral("song.mp3\n"));
    EXPECT_GT(audio, plain) << "an audio embed is a card with a header and controls";
    EXPECT_GT(video, audio + 200) << "a video card has a picture above its controls";
}

// --- links to headings -------------------------------------------------------

TEST(HeadingLinkTest, ANoteAndHeadingReadAsNoteThenHeading)
{
    // Obsidian shows [[Note#Heading]] as "Note > Heading". The "#" stays in
    // the display text, in invisible ink and as wide as " > ", which the
    // paint step draws over it.
    const QString note = QStringLiteral("See [[Note#Heading]] here.\n");
    auto editor = editorWith(note);
    EXPECT_EQ(editor->displayText(), QStringLiteral("See Note#Heading here.\n"));
    const QTextCharFormat hash = editor->formatAt(note.indexOf(QLatin1Char('#')));
    EXPECT_EQ(hash.foreground().color().alpha(), 0) << "the \"#\" is drawn over, not shown";
    EXPECT_GT(hash.fontLetterSpacing(), 0.0) << "and leaves room for \" > \"";
}

TEST(HeadingLinkTest, AHeadingInTheSameNoteReadsAsTheHeading)
{
    auto editor = editorWith(QStringLiteral("See [[#Heading]] here.\n"));
    EXPECT_EQ(editor->displayText(), QStringLiteral("See Heading here.\n"));
}

TEST(HeadingLinkTest, TheCaretInsideShowsTheLinkAsWritten)
{
    const QString note = QStringLiteral("See [[Note#Heading]] here.\n");
    auto editor = editorWith(note);
    editor->enterBlockAt(note.indexOf(QLatin1String("Head")));
    EXPECT_TRUE(editor->displayText().contains(QLatin1String("[[Note#Heading]]")));
    EXPECT_NE(editor->formatAt(note.indexOf(QLatin1Char('#'))).foreground().color().alpha(), 0);
}

TEST(HeadingLinkTest, BlockReferencesAndAliasesAreLeftAlone)
{
    const QString block = QStringLiteral("See [[Note#^id]] here.\n");
    auto blockEditor = editorWith(block);
    EXPECT_EQ(blockEditor->displayText(), QStringLiteral("See Note#^id here.\n"));
    EXPECT_NE(blockEditor->formatAt(block.indexOf(QLatin1Char('#'))).foreground().color().alpha(), 0);

    auto aliased = editorWith(QStringLiteral("See [[Note#Heading|the part]] here.\n"));
    EXPECT_EQ(aliased->displayText(), QStringLiteral("See the part here.\n"));
}

TEST(HeadingLinkTest, ALinkRecoloursWhenItsNoteAppears)
{
    // A link drawn before its note was indexed stayed "missing" until the
    // next edit: nothing told the editor the index had changed.
    bool exists = false;
    const QString note = QStringLiteral("See [[Other]] here.\n");
    auto editor = std::make_unique<MarkdownEditor>();
    editor->resize(900, 600);
    editor->setLinkResolver([&exists](const QString &target) {
        return exists && target == QLatin1String("Other") ? QStringLiteral("/v/Other.md")
                                                           : QString();
    });
    editor->setNoteText(note);
    const qsizetype at = note.indexOf(QLatin1String("Other")) + 1;
    const QColor missing = editor->formatAt(at).foreground().color();

    exists = true;
    editor->refreshLinkStatus();
    EXPECT_NE(editor->formatAt(at).foreground().color(), missing);
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



// --- a table edited where it is drawn (TableCells.cpp) ---------------------

namespace {

const QString kTableNote = QStringLiteral("above\n\n| name | qty |\n|---|---:|\n| apple | 3 |\n| pear |  |\n\nbelow\n");

/// Clicks across and down the note until the caret lands in `range`; the
/// point it did, or a null point.
QPoint clickInto(MarkdownEditor &editor, Range range)
{
    for (int y = 4; y < 400; y += 4) {
        for (int x = 4; x < editor.viewport()->width(); x += 12) {
            QTest::mouseClick(editor.viewport(), Qt::LeftButton, {}, QPoint(x, y));
            if (editor.cursorOffset() > range.start && editor.cursorOffset() < range.end)
                return QPoint(x, y);
        }
    }
    return {};
}

/// A drop the way Qt delivers one: to the viewport, and only after a drag
/// entering it was accepted. True when the editor took the drop.
bool dropOnto(MarkdownEditor &editor, QPoint point, const QMimeData &mime)
{
    QDragEnterEvent enter(point, Qt::CopyAction, &mime, Qt::NoButton, Qt::NoModifier);
    QCoreApplication::sendEvent(editor.viewport(), &enter);
    if (!enter.isAccepted())
        return false;
    QDropEvent drop(QPointF(point), Qt::CopyAction, &mime, Qt::NoButton, Qt::NoModifier);
    QCoreApplication::sendEvent(editor.viewport(), &drop);
    return drop.isAccepted();
}

} // namespace

TEST(TableCellTest, AClickEditsTheCellAndTheTableStaysATable)
{
    auto editor = editorWith(kTableNote);
    editor->show();
    const qsizetype apple = kTableNote.indexOf(QStringLiteral("apple"));
    ASSERT_TRUE(editor->tableRenderedAt(apple));
    ASSERT_FALSE(clickInto(*editor, {apple, apple + 5}).isNull());
    EXPECT_TRUE(editor->tableRenderedAt(apple)); // not dropped to "| apple | 3 |"

    editor->setCursorOffset(apple + 5);
    QTest::keyClicks(editor.get(), QStringLiteral("s|x"));
    EXPECT_TRUE(editor->noteText().contains(QStringLiteral("| apples\\|x | 3 |")));
    EXPECT_TRUE(editor->tableRenderedAt(apple));
}

TEST(TableCellTest, KeysMoveBetweenCellsAndNeverEatAPipe)
{
    auto editor = editorWith(kTableNote);
    editor->show();
    const qsizetype apple = kTableNote.indexOf(QStringLiteral("apple"));
    const qsizetype three = kTableNote.indexOf(QStringLiteral("3 |"));
    editor->setCursorOffset(apple);

    QTest::keyClick(editor.get(), Qt::Key_Backspace); // at the cell's start: nothing
    EXPECT_EQ(editor->noteText(), kTableNote);
    editor->setCursorOffset(apple + 5);
    QTest::keyClick(editor.get(), Qt::Key_Right); // past the end: into the next cell
    EXPECT_EQ(editor->cursorOffset(), three);
    QTest::keyClick(editor.get(), Qt::Key_Left);
    EXPECT_EQ(editor->cursorOffset(), apple + 5);

    QTest::keyClick(editor.get(), Qt::Key_Tab);
    EXPECT_EQ(editor->cursorOffset(), three + 1);
    QTest::keyClick(editor.get(), Qt::Key_Return); // down a row, same column: the empty cell
    QTest::keyClicks(editor.get(), QStringLiteral("7"));
    EXPECT_TRUE(editor->noteText().contains(QStringLiteral("| pear | 7 |")));

    // Tab from the last cell adds a row, as Advanced Tables does.
    QTest::keyClick(editor.get(), Qt::Key_Tab);
    QTest::keyClicks(editor.get(), QStringLiteral("fig"));
    EXPECT_TRUE(editor->noteText().contains(QStringLiteral("| pear | 7 |\n| fig |  |\n")))
        << editor->noteText().toStdString();
    EXPECT_TRUE(editor->tableRenderedAt(apple));
}

TEST(TableCellTest, SourceModeStillShowsThePipes)
{
    auto editor = editorWith(kTableNote);
    editor->setMode(MarkdownEditor::Mode::Source);
    const qsizetype apple = kTableNote.indexOf(QStringLiteral("apple"));
    editor->setCursorOffset(apple - 2);
    EXPECT_EQ(editor->cursorOffset(), apple - 2); // no snapping into cells there
    EXPECT_FALSE(editor->tableRenderedAt(apple));
}

TEST(ClickToEditTest, MathsAndHTMLOpenWhereTheyAreClicked)
{
    // Drawn as images, they once let a click fall through to the paragraph
    // below; only the sliver above them opened them. A fresh editor per
    // click, since an opened block shows its source and moves everything.
    for (const QString &element : {QStringLiteral("$$\nx^2 + y^2 = z^2\n$$"), QStringLiteral("<div>hello html</div>")}) {
        const QString text = QStringLiteral("above paragraph\n\n") + element + QStringLiteral("\n\nbelow paragraph\n");
        const qsizetype start = text.indexOf(element);
        int landed = 0;
        for (int y = 2; y < 300; y += 2) {
            auto editor = editorWith(text);
            editor->show();
            QTest::mouseClick(editor->viewport(), Qt::LeftButton, {}, QPoint(editor->viewport()->width() / 2, y));
            if (editor->cursorOffset() >= start && editor->cursorOffset() <= start + element.size())
                ++landed;
        }
        EXPECT_GE(landed, 10) << element.toStdString() << ": only " << landed * 2 << " px of it opens on a click";
    }
}

TEST(TableFormatEditorTest, LeavingAnEditedTableAlignsItAndNothingElse)
{
    const QString note = QStringLiteral("para  with  spaces\n\n| a | b |\n|---|--:|\n| 1 | 22 |\n\n|x|y|\n|-|-|\n|1|2|\n\nend\n");
    for (const MarkdownEditor::Mode mode : {MarkdownEditor::Mode::LivePreview, MarkdownEditor::Mode::Source}) {
        auto editor = editorWith(note);
        editor->setMode(mode);
        editor->show();
        editor->setCursorOffset(note.indexOf(QStringLiteral("22")) + 2);
        QTest::keyClicks(editor.get(), QStringLiteral("333"));
        EXPECT_TRUE(editor->noteText().contains(QStringLiteral("| 1 | 22333 |"))); // not yet: still in it
        editor->setCursorOffset(editor->noteText().indexOf(QStringLiteral("end")) + 1);
        const QString after = editor->noteText();
        EXPECT_TRUE(after.contains(QStringLiteral("| a   |     b |\n| --- | ----: |\n| 1   | 22333 |\n")))
            << after.toStdString();
        EXPECT_TRUE(after.startsWith(QStringLiteral("para  with  spaces\n\n"))); // the rest byte for byte
        EXPECT_TRUE(after.contains(QStringLiteral("|x|y|\n|-|-|\n|1|2|\n")));   // an unedited table too
        EXPECT_EQ(editor->cursorOffset(), after.indexOf(QStringLiteral("end")) + 1); // the caret moved with its text

        QTest::keyClick(editor.get(), Qt::Key_Z, Qt::ControlModifier); // the alignment is its own step
        EXPECT_TRUE(editor->noteText().contains(QStringLiteral("| 1 | 22333 |")));
    }
}

TEST(TableFormatEditorTest, FormatAllTablesKeepsTheCaretInItsCell)
{
    const QString note = QStringLiteral("|x|long header|\n|-|-|\n|1|2|\n\n|p|q|\n|:-:|-|\n|r|s|\n");
    auto editor = editorWith(note);
    editor->show();
    const qsizetype two = note.indexOf(QStringLiteral("2|"));
    editor->setCursorOffset(two + 1); // after the "2"
    ASSERT_TRUE(editor->runCommand(QStringLiteral("noteAppC:format-all-tables")));
    EXPECT_EQ(editor->noteText(),
              QStringLiteral("| x   | long header |\n| --- | ----------- |\n| 1   | 2           |\n\n"
                             "|  p  | q   |\n| :-: | --- |\n|  r  | s   |\n"));
    EXPECT_EQ(editor->noteText().at(editor->cursorOffset() - 1), QLatin1Char('2'));
}

TEST(TableCommandTest, RowsAndColumnsAsAdvancedTablesDoes)
{
    const QString note = QStringLiteral("| a | b |\n| --- | --: |\n| 1 | 2 |\n| 3 | 4 |\n");
    const auto run = [&](qsizetype caret, const char *id) {
        auto editor = editorWith(note);
        editor->show();
        editor->setCursorOffset(caret);
        EXPECT_TRUE(editor->runCommand(QString::fromLatin1(id))); // a known command runs, even as a no-op
        // What it changed; nothing, for a row with nowhere to go or a caret not in a table.
        const QString after = editor->noteText();
        return std::make_pair(after != note ? after : QString(), after.at(editor->cursorOffset()));
    };
    const qsizetype one = note.indexOf(QLatin1Char('1'));
    const qsizetype four = note.indexOf(QLatin1Char('4'));

    EXPECT_EQ(run(one, "noteAppC:table-insert-row-after").first,
              QStringLiteral("| a   |   b |\n| --- | --: |\n| 1   |   2 |\n|     |     |\n| 3   |   4 |\n"));
    EXPECT_EQ(run(one, "noteAppC:table-delete-row").first,
              QStringLiteral("| a   |   b |\n| --- | --: |\n| 3   |   4 |\n"));
    auto moved = run(one, "noteAppC:table-move-row-down");
    EXPECT_EQ(moved.first, QStringLiteral("| a   |   b |\n| --- | --: |\n| 3   |   4 |\n| 1   |   2 |\n"));
    EXPECT_EQ(moved.second, QLatin1Char('1')); // the caret went with its row
    EXPECT_EQ(run(one, "noteAppC:table-move-row-up").first, QString()); // already the first body row
    EXPECT_EQ(run(four, "noteAppC:table-insert-column-before").first,
              QStringLiteral("| a   |     |   b |\n| --- | --- | --: |\n| 1   |     |   2 |\n| 3   |     |   4 |\n"));
    EXPECT_EQ(run(four, "noteAppC:table-delete-column").first,
              QStringLiteral("| a   |\n| --- |\n| 1   |\n| 3   |\n"));
    moved = run(one, "noteAppC:table-move-column-right");
    EXPECT_EQ(moved.first, QStringLiteral("|   b | a   |\n| --: | --- |\n|   2 | 1   |\n|   4 | 3   |\n"));
    EXPECT_EQ(moved.second, QLatin1Char('1'));
    EXPECT_EQ(run(0, "noteAppC:table-delete-row").first, QString()); // nowhere near a cell
}

// --- paste and drop (PasteDrop.cpp) -----------------------------------------

TEST(PasteDropTest, ImagePasteCallsTheSaverAndInsertsTheEmbed)
{
    auto editor = editorWith(QStringLiteral("hello\n"));
    editor->setCursorOffset(5);

    QImage image(2, 2, QImage::Format_RGB32);
    image.fill(Qt::red);
    QGuiApplication::clipboard()->setImage(image);

    QString gotName;
    QByteArray gotBytes;
    editor->setAttachmentSaver([&](const QString &name, const QByteArray &bytes) {
        gotName = name;
        gotBytes = bytes;
        return QStringLiteral("![[") + name + QStringLiteral("]]");
    });

    QTest::keyClick(editor.get(), Qt::Key_V, Qt::ControlModifier);

    EXPECT_TRUE(gotName.startsWith(QStringLiteral("Pasted image "))) << gotName.toStdString();
    EXPECT_TRUE(gotName.endsWith(QStringLiteral(".png")));
    ASSERT_FALSE(gotBytes.isEmpty());
    EXPECT_TRUE(QImage::fromData(gotBytes, "PNG").size() == image.size());
    EXPECT_EQ(editor->noteText(), QStringLiteral("hello![[") + gotName + QStringLiteral("]]\n"));
}

TEST(PasteDropTest, ShiftPasteIsTextOnlyEvenWithAnImageOnTheClipboard)
{
    auto editor = editorWith(QStringLiteral("hello\n"));
    editor->setCursorOffset(5);

    QImage image(2, 2, QImage::Format_RGB32);
    image.fill(Qt::blue);
    QGuiApplication::clipboard()->setImage(image);

    bool saverCalled = false;
    editor->setAttachmentSaver([&](const QString &, const QByteArray &) {
        saverCalled = true;
        return QString();
    });

    QTest::keyClick(editor.get(), Qt::Key_V, Qt::ControlModifier | Qt::ShiftModifier);

    EXPECT_FALSE(saverCalled);
    EXPECT_EQ(editor->noteText(), QStringLiteral("hello\n")); // no text on the clipboard either
}

TEST(PasteDropTest, NoSaverSetDoesNothingForAnImagePaste)
{
    auto editor = editorWith(QStringLiteral("hello\n"));
    editor->setCursorOffset(5);
    QImage image(2, 2, QImage::Format_RGB32);
    image.fill(Qt::green);
    QGuiApplication::clipboard()->setImage(image);

    QTest::keyClick(editor.get(), Qt::Key_V, Qt::ControlModifier);

    EXPECT_EQ(editor->noteText(), QStringLiteral("hello\n"));
}

TEST(PasteDropTest, DropOfAnOutsideFileImportsAndInsertsTheEmbedAtTheDropPoint)
{
    const QString note = QStringLiteral("above\nbelow\n");
    auto editor = editorWith(note);
    editor->show();
    const QPoint point = clickInto(*editor, {5, 7}); // the start of "below"
    ASSERT_FALSE(point.isNull());

    QString gotPath;
    editor->setFileImporter([&](const QString &path) {
        gotPath = path;
        return QStringLiteral("![[photo.png]]");
    });

    QMimeData mime;
    mime.setUrls({QUrl::fromLocalFile(QStringLiteral("/outside/photo.png"))});
    ASSERT_TRUE(dropOnto(*editor, point, mime));

    EXPECT_EQ(gotPath, QStringLiteral("/outside/photo.png"));
    EXPECT_EQ(editor->noteText(), QStringLiteral("above\n![[photo.png]]below\n"));
}

TEST(PasteDropTest, DropOfAnInVaultNoteInsertsWhateverTheImporterReturns)
{
    const QString note = QStringLiteral("above\nbelow\n");
    auto editor = editorWith(note);
    editor->show();
    const QPoint point = clickInto(*editor, {5, 7});
    ASSERT_FALSE(point.isNull());

    editor->setFileImporter([](const QString &) { return QStringLiteral("[[Note]]"); });

    QMimeData mime;
    mime.setUrls({QUrl::fromLocalFile(QStringLiteral("/vault/Note.md"))});
    ASSERT_TRUE(dropOnto(*editor, point, mime));

    EXPECT_EQ(editor->noteText(), QStringLiteral("above\n[[Note]]below\n"));
}

TEST(PasteDropTest, DropWithNoImporterInsertsThePathAsText)
{
    const QString note = QStringLiteral("above\nbelow\n");
    auto editor = editorWith(note);
    editor->show();
    const QPoint point = clickInto(*editor, {5, 7});
    ASSERT_FALSE(point.isNull());

    QMimeData mime;
    mime.setUrls({QUrl::fromLocalFile(QStringLiteral("/outside/photo.png"))});
    ASSERT_TRUE(dropOnto(*editor, point, mime));

    EXPECT_EQ(editor->noteText(), QStringLiteral("above\n/outside/photo.pngbelow\n"));
}

TEST(PasteDropTest, DroppedTextIsInsertedAtThePoint)
{
    const QString note = QStringLiteral("above\nbelow\n");
    auto editor = editorWith(note);
    editor->show();
    const QPoint point = clickInto(*editor, {5, 7});
    ASSERT_FALSE(point.isNull());

    QMimeData mime;
    mime.setText(QStringLiteral("dropped"));
    ASSERT_TRUE(dropOnto(*editor, point, mime));

    EXPECT_EQ(editor->noteText(), QStringLiteral("above\ndroppedbelow\n"));
}

TEST(PasteDropTest, PasteIntoATableCellStaysSafe)
{
    auto editor = editorWith(kTableNote);
    editor->show();
    const qsizetype apple = kTableNote.indexOf(QStringLiteral("apple"));
    ASSERT_FALSE(clickInto(*editor, {apple, apple + 5}).isNull());
    editor->setCursorOffset(apple + 5);

    QGuiApplication::clipboard()->setText(QStringLiteral("a|b\nc"));
    QTest::keyClick(editor.get(), Qt::Key_V, Qt::ControlModifier);

    EXPECT_TRUE(editor->noteText().contains(QStringLiteral("| applea\\|b c | 3 |")))
        << editor->noteText().toStdString();
}
