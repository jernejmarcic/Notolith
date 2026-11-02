// SPDX-License-Identifier: GPL-3.0-or-later
#include <gtest/gtest.h>

#include <QApplication>
#include <QComboBox>
#include <QElapsedTimer>
#include <QImage>
#include <QPainter>
#include <QtTest/QTest>

#include <memory>

#include "BulletGlyphs.h"
#include "MarkdownEditor.h"
#include "Parser.h"
#include "Perf.h"

using namespace noteAppC;

namespace {

/// An editor holding `text` with the caret at `caret` (the end by default).
std::unique_ptr<MarkdownEditor> editorWith(const QString &text, qsizetype caret = -1)
{
    auto editor = std::make_unique<MarkdownEditor>();
    editor->resize(900, 600);
    editor->setNoteText(text);
    editor->setCursorOffset(caret < 0 ? text.size() : caret);
    return editor;
}

/// `marker`'s position in `text`, for a caret placed by reading rather than counting.
qsizetype after(const QString &text, const QString &needle)
{
    const qsizetype at = text.indexOf(needle);
    return at < 0 ? -1 : at + needle.size();
}

struct Result {
    QString text;
    qsizetype caret = 0;
};

/// The note and caret after one key; then Ctrl+Z must give the start back exactly.
Result press(const QString &text, qsizetype caret, Qt::Key key, Qt::KeyboardModifiers modifiers = {})
{
    auto editor = editorWith(text, caret);
    QTest::keyClick(editor.get(), key, modifiers);
    Result result{editor->noteText(), editor->cursorOffset()};
    QTest::keyClick(editor.get(), Qt::Key_Z, Qt::ControlModifier);
    EXPECT_EQ(editor->noteText(), text) << "undo is one step and byte-exact";
    EXPECT_EQ(editor->cursorOffset(), caret);
    return result;
}

Result enter(const QString &text, qsizetype caret = -1)
{
    return press(text, caret < 0 ? text.size() : caret, Qt::Key_Return);
}

} // namespace

// --- Enter ----------------------------------------------------------------

TEST(ListEnter, EveryBulletMarkerContinues)
{
    for (const QString marker : {QStringLiteral("-"), QStringLiteral("*"), QStringLiteral("+")}) {
        const QString note = marker + QStringLiteral(" one");
        const Result r = enter(note);
        EXPECT_EQ(r.text, note + QStringLiteral("\n") + marker + QStringLiteral(" "));
        EXPECT_EQ(r.caret, r.text.size());
    }
}

TEST(ListEnter, OrderedCountsOnWhateverTheDelimiter)
{
    EXPECT_EQ(enter(QStringLiteral("1. Should continue")).text, QStringLiteral("1. Should continue\n2. "));
    EXPECT_EQ(enter(QStringLiteral("7) seven")).text, QStringLiteral("7) seven\n8) "));
    EXPECT_EQ(enter(QStringLiteral("9. nine")).text, QStringLiteral("9. nine\n10. "));
}

TEST(ListEnter, TaskContinuesUnchecked)
{
    EXPECT_EQ(enter(QStringLiteral("- [x] done")).text, QStringLiteral("- [x] done\n- [ ] "));
    EXPECT_EQ(enter(QStringLiteral("1. [ ] todo")).text, QStringLiteral("1. [ ] todo\n2. [ ] "));
}

TEST(ListEnter, AfterACustomStateTheNextItemIsOpen)
{
    auto editor = std::make_unique<MarkdownEditor>();
    editor->resize(900, 600);
    ParseOptions options = editor->parseOptions();
    options.taskStates = true;
    editor->setParseOptions(options);
    editor->setNoteText(QStringLiteral("- [-] a"));
    editor->setCursorOffset(7);
    QTest::keyClick(editor.get(), Qt::Key_Return);
    EXPECT_EQ(editor->noteText(), QStringLiteral("- [-] a\n- [ ] "));

    // Off, `[-]` is text and the next item has no box.
    EXPECT_EQ(enter(QStringLiteral("- [-] a")).text, QStringLiteral("- [-] a\n- "));
}

TEST(ListEnter, KeepsTheIndentation)
{
    EXPECT_EQ(enter(QStringLiteral("- a\n\t- b")).text, QStringLiteral("- a\n\t- b\n\t- "));
    EXPECT_EQ(enter(QStringLiteral("1. a\n   2. b")).text, QStringLiteral("1. a\n   2. b\n   3. "));
}

TEST(ListEnter, ContinuesInsideAQuoteAndACallout)
{
    EXPECT_EQ(enter(QStringLiteral("> - x")).text, QStringLiteral("> - x\n> - "));
    EXPECT_EQ(enter(QStringLiteral("> 1. x")).text, QStringLiteral("> 1. x\n> 2. "));
    EXPECT_EQ(enter(QStringLiteral("> > * x")).text, QStringLiteral("> > * x\n> > * "));
    const QString callout = QStringLiteral("> [!note] Title\n> - one\n> - [ ] two");
    EXPECT_EQ(enter(callout).text, callout + QStringLiteral("\n> - [ ] "));
    EXPECT_EQ(enter(QStringLiteral("> 1. a\n> 2. b\n> 3. c"), after(QStringLiteral("> 1. a\n> 2. b\n> 3. c"), QStringLiteral("2. b"))).text,
              QStringLiteral("> 1. a\n> 2. b\n> 3. \n> 4. c"));
}

TEST(ListEnter, AnEmptyItemEndsTheListInAQuoteToo)
{
    // Out of the quote level first, then out of the indent, then the marker goes.
    EXPECT_EQ(enter(QStringLiteral("> - a\n> - ")).text, QStringLiteral("> - a\n- "));
    EXPECT_EQ(enter(QStringLiteral("> > - a\n> > - ")).text, QStringLiteral("> > - a\n> - "));
    EXPECT_EQ(enter(QStringLiteral("> - a\n>     - ")).text, QStringLiteral("> - a\n    - "));
    EXPECT_EQ(enter(QStringLiteral("> - a\n    - ")).text, QStringLiteral("> - a\n- "));
}

TEST(ListEnter, AnEmptyItemEndsTheList)
{
    const Result r = enter(QStringLiteral("- a\n- "));
    EXPECT_EQ(r.text, QStringLiteral("- a\n"));
    EXPECT_EQ(r.caret, 4);
    EXPECT_EQ(enter(QStringLiteral("1. a\n2. [ ] ")).text, QStringLiteral("1. a\n"));
}

TEST(ListEnter, AnEmptyNestedItemOutdentsFirst)
{
    const Result r = enter(QStringLiteral("1. a\n\t1. "));
    EXPECT_EQ(r.text, QStringLiteral("1. a\n2. "));
    EXPECT_EQ(r.caret, r.text.size());
}

TEST(ListEnter, SplitsTheItemAtTheCaret)
{
    const QString note = QStringLiteral("- alpha beta");
    const Result r = enter(note, after(note, QStringLiteral("alpha")));
    EXPECT_EQ(r.text, QStringLiteral("- alpha\n-  beta"));
    EXPECT_EQ(r.caret, after(r.text, QStringLiteral("\n- ")));

    const QString ordered = QStringLiteral("1. a\n2. bc\n3. d");
    const Result split = enter(ordered, after(ordered, QStringLiteral("2. b")));
    EXPECT_EQ(split.text, QStringLiteral("1. a\n2. b\n3. c\n4. d"));
    EXPECT_EQ(split.caret, after(split.text, QStringLiteral("3. ")));
}

TEST(ListEnter, RenumbersTheFollowingSiblingsOnly)
{
    const QString note = QStringLiteral("1. a\n2. b\n3. c\n\n\ntext 7. kept");
    const Result r = enter(note, after(note, QStringLiteral("2. b")));
    EXPECT_EQ(r.text, QStringLiteral("1. a\n2. b\n3. \n4. c\n\n\ntext 7. kept"));

    // Items above the edit and a second list are left as typed.
    const QString irregular = QStringLiteral("5. a\n5. b\n5. c\n\n\n- x\n3. y");
    const Result keep = enter(irregular, after(irregular, QStringLiteral("5. b")));
    EXPECT_EQ(keep.text, QStringLiteral("5. a\n5. b\n6. \n7. c\n\n\n- x\n3. y"));
}

TEST(ListEnter, RenumbersAcrossWrappedAndBlankLines)
{
    const QString note = QStringLiteral("1. a\n   wrapped\n2. b\n\n   para\n3. c");
    const Result r = enter(note, after(note, QStringLiteral("1. a")));
    EXPECT_EQ(r.text, QStringLiteral("1. a\n2. \n   wrapped\n3. b\n\n   para\n4. c"));
}

TEST(ListEnter, RenumberingStopsAtTheFirstSiblingThatAlreadyMatches)
{
    const QString note = QStringLiteral("1. a\n3. b\n3. c");
    EXPECT_EQ(enter(note, after(note, QStringLiteral("1. a"))).text, QStringLiteral("1. a\n2. \n3. b\n3. c"));
}

TEST(ListEnter, SubListsKeepTheirOwnCounts)
{
    const QString note = QStringLiteral("1. a\n\t1. x\n\t2. y\n2. b");
    const Result r = enter(note, after(note, QStringLiteral("\t2. y")));
    EXPECT_EQ(r.text, QStringLiteral("1. a\n\t1. x\n\t2. y\n\t3. \n2. b"));
}

TEST(ListEnter, ShiftEnterBreaksTheLineInsideTheItem)
{
    const Result plain = press(QStringLiteral("- one"), 5, Qt::Key_Return, Qt::ShiftModifier);
    EXPECT_EQ(plain.text, QStringLiteral("- one\n  "));
    EXPECT_EQ(plain.caret, plain.text.size());
    EXPECT_EQ(press(QStringLiteral("2. [ ] t"), 8, Qt::Key_Return, Qt::ShiftModifier).text,
              QStringLiteral("2. [ ] t\n       "));
    EXPECT_EQ(press(QStringLiteral("> - q"), 5, Qt::Key_Return, Qt::ShiftModifier).text, QStringLiteral("> - q\n>   "));
    EXPECT_EQ(press(QStringLiteral("text"), 4, Qt::Key_Return, Qt::ShiftModifier).text, QStringLiteral("text\n"));
}

TEST(ListEnter, ACodeFenceIsNotAList)
{
    const QString note = QStringLiteral("```\n- not a list\n```\n");
    auto editor = editorWith(note, after(note, QStringLiteral("a list")));
    QTest::keyClick(editor.get(), Qt::Key_Return);
    EXPECT_EQ(editor->noteText(), QStringLiteral("```\n- not a list\n\n```\n"));
}

// Typed with no pause for the 20 ms parse: the decorations know nothing of any
// fence yet, so the editor has to read the fence lines itself.
TEST(ListEnter, AFenceTypedWithinTheParseDelayIsSeen)
{
    auto editor = editorWith(QString());
    const auto typeLine = [&](const QString &line) {
        QTest::keyClicks(editor.get(), line);
        QTest::keyClick(editor.get(), Qt::Key_Return);
    };
    typeLine(QStringLiteral("```"));
    QTest::keyClicks(editor.get(), QStringLiteral("- a"));
    QTest::keyClick(editor.get(), Qt::Key_Return);
    EXPECT_EQ(editor->noteText(), QStringLiteral("```\n- a\n")) << "inside the fence: a newline, not a bullet";

    // Closed again, still before any parse: the list after it continues.
    typeLine(QStringLiteral("```"));
    QTest::keyClicks(editor.get(), QStringLiteral("- b"));
    QTest::keyClick(editor.get(), Qt::Key_Return);
    EXPECT_EQ(editor->noteText(), QStringLiteral("```\n- a\n```\n- b\n- "));
}

// The same, with the parse landing between the lines: a fence nothing closes yet
// runs to the end of the note, and what is typed in it is still code.
TEST(ListEnter, AnUnclosedFenceTheParseSawHoldsWhatIsTypedAfterIt)
{
    auto editor = editorWith(QString());
    QTest::keyClicks(editor.get(), QStringLiteral("```"));
    QTest::keyClick(editor.get(), Qt::Key_Return);
    QTest::qWait(100); // the parse has seen the fence and nothing in it
    QTest::keyClicks(editor.get(), QStringLiteral("- a"));
    QTest::keyClick(editor.get(), Qt::Key_Return);
    EXPECT_EQ(editor->noteText(), QStringLiteral("```\n- a\n")) << "inside the fence: a newline, not a bullet";
}

TEST(ListEnter, AFenceTheParseAlreadySawStillCounts)
{
    const QString note = QStringLiteral("```\n- x\n```\n\n- b");
    auto editor = editorWith(note);
    QTest::keyClick(editor.get(), Qt::Key_Return);
    EXPECT_EQ(editor->noteText(), note + QStringLiteral("\n- "));
    editor->setCursorOffset(after(note, QStringLiteral("- x")));
    QTest::keyClick(editor.get(), Qt::Key_Return);
    EXPECT_EQ(editor->noteText(), QStringLiteral("```\n- x\n\n```\n\n- b\n- "));
}

TEST(ListEnter, InsideTheMarkerIsJustANewline)
{
    EXPECT_EQ(enter(QStringLiteral("- a"), 1).text, QStringLiteral("-\n a"));
}

TEST(ListEnter, IsItsOwnUndoStepEvenAfterTyping)
{
    auto editor = editorWith(QStringLiteral("- "));
    QTest::keyClicks(editor.get(), QStringLiteral("abc"));
    QTest::keyClick(editor.get(), Qt::Key_Return);
    EXPECT_EQ(editor->noteText(), QStringLiteral("- abc\n- "));
    QTest::keyClick(editor.get(), Qt::Key_Z, Qt::ControlModifier);
    EXPECT_EQ(editor->noteText(), QStringLiteral("- abc"));
}

// --- Tab and Shift+Tab ------------------------------------------------------

TEST(ListIndent, TabIndentsFromAnywhereInTheItem)
{
    const QString note = QStringLiteral("- a\n- bcd");
    const Result r = press(note, after(note, QStringLiteral("- b")), Qt::Key_Tab);
    EXPECT_EQ(r.text, QStringLiteral("- a\n\t- bcd"));
    EXPECT_EQ(r.caret, after(r.text, QStringLiteral("- b")));
}

TEST(ListIndent, OrderedRestartsAtOneInTheSubListAndTheRestCloseUp)
{
    const QString note = QStringLiteral("1. a\n2. b\n3. c");
    EXPECT_EQ(press(note, after(note, QStringLiteral("2. ")), Qt::Key_Tab).text, QStringLiteral("1. a\n\t1. b\n2. c"));
    const QString more = QStringLiteral("1. a\n\t1. b\n2. c");
    EXPECT_EQ(press(more, after(more, QStringLiteral("2. ")), Qt::Key_Tab).text, QStringLiteral("1. a\n\t1. b\n\t2. c"));
}

TEST(ListIndent, ShiftTabContinuesTheParentNumbering)
{
    const QString note = QStringLiteral("1. a\n\t1. b\n2. c");
    const Result r = press(note, after(note, QStringLiteral("\t1. ")), Qt::Key_Backtab, Qt::ShiftModifier);
    EXPECT_EQ(r.text, QStringLiteral("1. a\n2. b\n3. c"));
    EXPECT_EQ(r.caret, after(r.text, QStringLiteral("2. ")));
    // Flush left already: nothing to do, and the keystroke is spent.
    EXPECT_EQ(press(QStringLiteral("- a"), 3, Qt::Key_Backtab, Qt::ShiftModifier).text, QStringLiteral("- a"));
}

TEST(ListIndent, InAQuoteItIsSpaces)
{
    const QString note = QStringLiteral("> - a\n> - b");
    const Result r = press(note, note.size(), Qt::Key_Tab);
    EXPECT_EQ(r.text, QStringLiteral("> - a\n>     - b"));
    EXPECT_EQ(press(r.text, r.text.size(), Qt::Key_Backtab, Qt::ShiftModifier).text, note);
}

TEST(ListIndent, RefusesWhatCommonMarkCannotNest)
{
    // The first item has nothing to nest under; nor can an item go two levels in.
    EXPECT_EQ(press(QStringLiteral("- a"), 3, Qt::Key_Tab).text, QStringLiteral("- a"));
    const QString note = QStringLiteral("- a\n\t- b");
    EXPECT_EQ(press(note, note.size(), Qt::Key_Tab).text, note);
}

TEST(ListIndent, OutsideAListAndInsideACodeFenceTabIsStillTab)
{
    EXPECT_EQ(press(QStringLiteral("word"), 4, Qt::Key_Tab).text, QStringLiteral("word    "));
    const QString note = QStringLiteral("```\n- x\n```\n");
    auto editor = editorWith(note, after(note, QStringLiteral("- x")));
    QTest::keyClick(editor.get(), Qt::Key_Tab);
    EXPECT_EQ(editor->noteText(), QStringLiteral("```\n- x    \n```\n"));
}

TEST(ListIndent, ASelectionIsReplacedAsBefore)
{
    auto editor = editorWith(QStringLiteral("- a\n- b"), 3);
    QTest::keyClick(editor.get(), Qt::Key_Left, Qt::ShiftModifier); // selects the space
    QTest::keyClick(editor.get(), Qt::Key_Tab);
    EXPECT_EQ(editor->noteText(), QStringLiteral("-     \n- b"));
}

// --- Backspace --------------------------------------------------------------

TEST(ListBackspace, AtTheStartOfTheTextTakesTheMarkerOffAndTheRestCloseUp)
{
    const QString note = QStringLiteral("1. a\n2. b\n3. c");
    const Result r = press(note, after(note, QStringLiteral("2. ")), Qt::Key_Backspace);
    EXPECT_EQ(r.text, QStringLiteral("1. a\nb\n2. c"));
    EXPECT_EQ(r.caret, after(r.text, QStringLiteral("a\n")));
    EXPECT_EQ(press(QStringLiteral("- [ ] t"), 6, Qt::Key_Backspace).text, QStringLiteral("t"));
}

TEST(ListBackspace, ElsewhereItIsAnOrdinaryBackspace)
{
    EXPECT_EQ(press(QStringLiteral("- ab"), 4, Qt::Key_Backspace).text, QStringLiteral("- a"));
    EXPECT_EQ(press(QStringLiteral("- ab"), 1, Qt::Key_Backspace).text, QStringLiteral(" ab")) << "inside the marker";
}

// --- Bullet glyphs ------------------------------------------------------------

TEST(BulletGlyphs, DefaultsGiveEachMarkerItsOwnLook)
{
    const BulletGlyphMap map;
    EXPECT_EQ(map.glyphFor(QLatin1Char('-')), BulletGlyph::Dash);
    EXPECT_EQ(map.glyphFor(QLatin1Char('*')), BulletGlyph::Dot);
    EXPECT_EQ(map.glyphFor(QLatin1Char('+')), BulletGlyph::Diamond);
    EXPECT_EQ(map.toSpec(), QStringLiteral("-=dash,*=dot,+=diamond"));
    EXPECT_EQ(BulletGlyphMap::parse(map.toSpec()), map);
}

TEST(BulletGlyphs, ParsingKeepsWhatItCanReadAndDefaultsTheRest)
{
    const BulletGlyphMap map = BulletGlyphMap::parse(QStringLiteral("*=star, -=typed =bar +=nonsense x=dot"));
    EXPECT_EQ(map.glyphFor(QLatin1Char('*')), BulletGlyph::Star);
    EXPECT_EQ(map.glyphFor(QLatin1Char('-')), BulletGlyph::AsTyped);
    EXPECT_EQ(map.glyphFor(QLatin1Char('+')), BulletGlyph::Diamond) << "unknown name: the default";
    EXPECT_EQ(BulletGlyphMap::parse(QString()), BulletGlyphMap());
    // A fixed set: every name round-trips.
    for (const QString &name : bulletGlyphNames())
        EXPECT_EQ(bulletGlyphName(bulletGlyphFromName(name, BulletGlyph::Dot)), name);
}

namespace {

/// Columns of a 40x24 glyph swatch that painted anything.
int widthOf(BulletGlyph glyph)
{
    QImage image(40, 24, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    paintBulletGlyph(painter, glyph, QLatin1Char('-'), QPointF(20, 12), Qt::white, QFont());
    painter.end();
    int first = -1;
    int last = -1;
    for (int x = 0; x < image.width(); ++x) {
        for (int y = 0; y < image.height(); ++y) {
            if (qAlpha(image.pixel(x, y)) > 40) {
                first = first < 0 ? x : first;
                last = x;
            }
        }
    }
    return first < 0 ? 0 : last - first + 1;
}

} // namespace

TEST(BulletGlyphs, EveryGlyphDrawsAndADashIsWiderThanADot)
{
    for (const QString &name : bulletGlyphNames())
        EXPECT_GT(widthOf(bulletGlyphFromName(name, BulletGlyph::Dot)), 0) << qPrintable(name);
    EXPECT_GT(widthOf(BulletGlyph::Dash), widthOf(BulletGlyph::Dot) + 3);
}

TEST(BulletGlyphs, TheEditorDrawsTheMappedGlyphPerMarker)
{
    const QString note = QStringLiteral("- a\n* b\n+ c\n\nend");
    auto editor = editorWith(note);
    editor->show();
    QApplication::processEvents();
    const QImage original = editor->grab().toImage();
    EXPECT_EQ(editor->bulletGlyphs().glyphFor(QLatin1Char('*')), BulletGlyph::Dot);

    editor->setBulletGlyphs(QStringLiteral("-=dash,*=dot,+=diamond")); // what it already is
    EXPECT_EQ(editor->grab().toImage(), original);

    editor->setBulletGlyphs(QStringLiteral("*=star"));
    EXPECT_EQ(editor->bulletGlyphs().glyphFor(QLatin1Char('*')), BulletGlyph::Star);
    const QImage starred = editor->grab().toImage();
    EXPECT_NE(starred, original);

    // Only the starred marker's row changed: the other two bullets are as they were.
    QList<int> changedRows;
    for (int y = 0; y < original.height(); ++y) {
        for (int x = 0; x < original.width() && (changedRows.isEmpty() || changedRows.last() != y); ++x) {
            if (original.pixel(x, y) != starred.pixel(x, y))
                changedRows.append(y);
        }
    }
    ASSERT_FALSE(changedRows.isEmpty());
    EXPECT_LT(changedRows.last() - changedRows.first(), 20) << "one bullet, one row of text";
    editor->hide();
}

TEST(BulletGlyphs, NestedItemsDrawTheTurnArrowByDefault)
{
    const QString note = QStringLiteral("- a\n  - b\n    - c\n\nend");
    auto editor = editorWith(note, note.size());
    editor->show();
    QApplication::processEvents();
    EXPECT_TRUE(editor->nestedBulletGlyphs()) << "on unless switched off";
    const QImage turned = editor->grab().toImage();
    editor->setNestedBulletGlyphs(false);
    const QImage perMarker = editor->grab().toImage();
    EXPECT_NE(turned, perMarker) << "depth >= 1 is drawn differently";
    editor->setNestedBulletGlyphs(true);
    EXPECT_EQ(editor->grab().toImage(), turned);

    // "typed" is the person's own choice and stays.
    editor->setBulletGlyphs(QStringLiteral("-=typed"));
    const QImage typed = editor->grab().toImage();
    editor->setNestedBulletGlyphs(false);
    EXPECT_EQ(editor->grab().toImage(), typed);

    // A note with no nesting looks the same either way.
    editor->setBulletGlyphs(QStringLiteral("-=dash"));
    editor->setNoteText(QStringLiteral("- a\n- b\n\nend"));
    QApplication::processEvents();
    const QImage flat = editor->grab().toImage();
    editor->setNestedBulletGlyphs(true);
    EXPECT_EQ(editor->grab().toImage(), flat);
    editor->hide();
}

TEST(BulletGlyphs, TheTurnGlyphHasAStemAndAHead)
{
    EXPECT_EQ(bulletGlyphFromName(QStringLiteral("turn"), BulletGlyph::Dot), BulletGlyph::Turn);
    QImage image(16, 16, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    {
        QPainter painter(&image);
        paintBulletGlyph(painter, BulletGlyph::Turn, QLatin1Char('-'), QPointF(8, 8), Qt::white, QFont());
    }
    auto painted = [&](int x0, int y0, int x1, int y1) {
        for (int y = y0; y < y1; ++y)
            for (int x = x0; x < x1; ++x)
                if (qAlpha(image.pixel(x, y)) > 40)
                    return true;
        return false;
    };
    EXPECT_TRUE(painted(0, 0, 6, 6)) << "the stem above the bar, to the left";
    EXPECT_TRUE(painted(9, 7, 16, 16)) << "the head, to the right";
    EXPECT_FALSE(painted(9, 0, 16, 4)) << "nothing top right";
}

TEST(BulletGlyphs, AChooserShowsAndReportsTheSpec)
{
    BulletGlyphChooser chooser;
    chooser.setSpec(QStringLiteral("-=ring,*=square,+=arrow"));
    EXPECT_EQ(chooser.spec(), QStringLiteral("-=ring,*=square,+=arrow"));
    QString reported;
    QObject::connect(&chooser, &BulletGlyphChooser::specChanged, [&](const QString &spec) { reported = spec; });
    chooser.comboFor(QLatin1Char('+'))->setCurrentIndex(chooser.comboFor(QLatin1Char('+'))->findData(QStringLiteral("star")));
    EXPECT_EQ(reported, QStringLiteral("-=ring,*=square,+=star"));
    chooser.setSpec(QString()); // quiet, and back to the defaults
    EXPECT_EQ(reported, QStringLiteral("-=ring,*=square,+=star"));
    EXPECT_EQ(chooser.spec(), BulletGlyphMap().toSpec());
}

// --- Number display ---------------------------------------------------------

namespace {

/// What the editor would draw for each ordered item of `note`.
QStringList labelsOf(MarkdownEditor &editor, const QString &note)
{
    QStringList labels;
    const auto result = Parser().parseAll(note);
    for (const ListItemSpan &item : result.decorations.listItems)
        labels.append(item.ordered ? editor.listLabel(item) : QStringLiteral("-"));
    return labels;
}

const QString kNested = QStringLiteral("1. a\n\t1. b\n\t2. c\n\t\t1. d\n2. e\n\t1. f\n- g\n\t1. h");

} // namespace

TEST(ListNumbers, PlainShowsEachListCountingOnItsOwn)
{
    auto editor = editorWith(QString());
    EXPECT_FALSE(editor->outlineNumbering());
    EXPECT_EQ(labelsOf(*editor, kNested),
              (QStringList{"1.", "1.", "2.", "1.", "2.", "1.", "-", "1."}));
}

TEST(ListNumbers, HierarchicalComposesTheNumbersOfTheItemsItSitsIn)
{
    auto editor = editorWith(QString());
    editor->setOutlineNumbering(true);
    EXPECT_EQ(labelsOf(*editor, kNested),
              (QStringList{"1.", "1.1.", "1.2.", "1.2.1.", "2.", "2.1.", "-", "1."}));
    // Display only: the note is untouched.
    EXPECT_EQ(editorWith(kNested)->noteText(), kNested);
}

TEST(ListNumbers, TheEndingIsASettingAndAnythingGoes)
{
    auto editor = editorWith(QString());
    editor->setOutlineNumbering(true);
    editor->setListNumberSuffix(QStringLiteral(":"));
    EXPECT_EQ(labelsOf(*editor, QStringLiteral("1. a\n\t1. b\n\t2. c\n2) d")),
              (QStringList{"1:", "1.1:", "1.2:", "2:"}));
    editor->setListNumberSuffix(QStringLiteral(" >"));
    EXPECT_EQ(labelsOf(*editor, QStringLiteral("1. a")), (QStringList{"1 >"}));
    editor->setListNumberSuffix(QString()); // as the note wrote it
    EXPECT_EQ(labelsOf(*editor, QStringLiteral("1. a\n2) b")), (QStringList{"1.", "2)"}));
}

TEST(ListEnter, ALongListIsRenumberedInAFewMilliseconds)
{
    QString note;
    for (int i = 1; i <= 5000; ++i)
        note += QString::number(i) + QStringLiteral(". item\n");
    SKIP_WALL_CLOCK_GUARD_UNDER_SANITIZER();
    // Best of three, each on a fresh editor: load only ever adds time.
    const qint64 took = perf::bestOf([&] {
        auto editor = editorWith(note, after(note, QStringLiteral("1. item")));
        QElapsedTimer timer;
        timer.start();
        QTest::keyClick(editor.get(), Qt::Key_Return);
        const qint64 ms = timer.elapsed();
        EXPECT_TRUE(editor->noteText().contains(QStringLiteral("\n5001. item\n")));
        return ms;
    }, 3, 100);
    printf("enter in a 5000-item list: %lld ms\n", took);
    EXPECT_LT(took, 100);
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
