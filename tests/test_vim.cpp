#include <gtest/gtest.h>

#include <QApplication>
#include <QClipboard>
#include <QInputMethodEvent>
#include <QSignalSpy>
#include <QtTest/QTest>

#include <memory>

#include "MarkdownEditor.h"

using namespace noteAppC;

namespace {

/// An editor with vim mode on, holding `text`, the caret at `caret`. Keys go
/// in as real key events, so what is tested is what a keystroke does.
struct Vim {
    MarkdownEditor editor;

    explicit Vim(const QString &text, qsizetype caret = 0)
    {
        editor.setTheme(Theme::builtin(Theme::Mode::Dark));
        editor.setNoteText(text);
        editor.setMode(MarkdownEditor::Mode::Source);
        editor.setVimMode(true);
        editor.setCursorOffset(caret);
    }
    void keys(const QString &sequence) { QTest::keyClicks(&editor, sequence); }
    void key(Qt::Key k, Qt::KeyboardModifiers modifiers = Qt::NoModifier)
    {
        QTest::keyClick(&editor, k, modifiers);
    }
    void esc() { key(Qt::Key_Escape); }
    QString text() const { return editor.noteText(); }
    qsizetype cursor() const { return editor.cursorOffset(); }
    QString mode() const { return editor.vimModeName(); }
};

} // namespace

TEST(VimTest, OffByDefaultAndTypesNormally)
{
    MarkdownEditor editor;
    editor.setNoteText(QStringLiteral(""));
    QTest::keyClicks(&editor, QStringLiteral("dd"));
    EXPECT_EQ(editor.noteText(), QStringLiteral("dd"));
    EXPECT_FALSE(editor.vimMode());
    EXPECT_TRUE(editor.vimStatus().isEmpty());
}

TEST(VimTest, StartsInNormalModeAndNormalKeysTypeNothing)
{
    Vim v(QStringLiteral("abc\n"));
    EXPECT_EQ(v.mode(), QStringLiteral("normal"));
    EXPECT_EQ(v.editor.vimStatus(), QStringLiteral("-- NORMAL --"));
    v.keys(QStringLiteral("zq"));
    EXPECT_EQ(v.text(), QStringLiteral("abc\n"));
}

TEST(VimTest, ModeSwitches)
{
    Vim v(QStringLiteral("abc def\n"));
    v.keys(QStringLiteral("i"));
    EXPECT_EQ(v.mode(), QStringLiteral("insert"));
    v.keys(QStringLiteral("X"));
    EXPECT_EQ(v.text(), QStringLiteral("Xabc def\n"));
    v.esc();
    EXPECT_EQ(v.mode(), QStringLiteral("normal"));
    EXPECT_EQ(v.cursor(), 0); // Escape steps back onto the last typed character
    v.keys(QStringLiteral("v"));
    EXPECT_EQ(v.mode(), QStringLiteral("visual"));
    v.keys(QStringLiteral("v"));
    EXPECT_EQ(v.mode(), QStringLiteral("normal"));
    v.keys(QStringLiteral("V"));
    EXPECT_EQ(v.mode(), QStringLiteral("visual line"));
    v.esc();
    EXPECT_EQ(v.mode(), QStringLiteral("normal"));
}

TEST(VimTest, InsertEntryPoints)
{
    {
        Vim v(QStringLiteral("  abc\n"), 3);
        v.keys(QStringLiteral("aX"));
        v.esc();
        EXPECT_EQ(v.text(), QStringLiteral("  abXc\n"));
    }
    {
        Vim v(QStringLiteral("  abc\n"), 3);
        v.keys(QStringLiteral("IX"));
        v.esc();
        EXPECT_EQ(v.text(), QStringLiteral("  Xabc\n"));
    }
    {
        Vim v(QStringLiteral("  abc\n"), 3);
        v.keys(QStringLiteral("AX"));
        v.esc();
        EXPECT_EQ(v.text(), QStringLiteral("  abcX\n"));
    }
    {
        Vim v(QStringLiteral("one\ntwo\n"), 1);
        v.keys(QStringLiteral("oX"));
        v.esc();
        EXPECT_EQ(v.text(), QStringLiteral("one\nX\ntwo\n"));
    }
    {
        Vim v(QStringLiteral("one\ntwo\n"), 5);
        v.keys(QStringLiteral("OX"));
        v.esc();
        EXPECT_EQ(v.text(), QStringLiteral("one\nX\ntwo\n"));
    }
}

TEST(VimTest, CharacterAndLineMotions)
{
    Vim v(QStringLiteral("  alpha beta\ngamma\n\ndelta epsilon\n"));
    v.keys(QStringLiteral("l"));
    EXPECT_EQ(v.cursor(), 1);
    v.keys(QStringLiteral("$"));
    EXPECT_EQ(v.cursor(), 11); // on the last character, not past it
    v.keys(QStringLiteral("l"));
    EXPECT_EQ(v.cursor(), 11);
    v.keys(QStringLiteral("0"));
    EXPECT_EQ(v.cursor(), 0);
    v.keys(QStringLiteral("^"));
    EXPECT_EQ(v.cursor(), 2);
    v.keys(QStringLiteral("h"));
    v.keys(QStringLiteral("h"));
    v.keys(QStringLiteral("h"));
    EXPECT_EQ(v.cursor(), 0);
    v.keys(QStringLiteral("G"));
    EXPECT_EQ(v.cursor(), v.text().indexOf(QStringLiteral("delta")));
    v.keys(QStringLiteral("gg"));
    EXPECT_EQ(v.cursor(), 2); // first non-blank
    v.keys(QStringLiteral("3G"));
    EXPECT_EQ(v.cursor(), v.text().indexOf(QStringLiteral("\n\n")) + 1); // the empty line
    v.keys(QStringLiteral("gg"));
    v.keys(QStringLiteral("j"));
    EXPECT_EQ(v.cursor(), v.text().indexOf(QStringLiteral("gamma")) + 2); // column kept
    v.keys(QStringLiteral("k"));
    EXPECT_EQ(v.cursor(), 2);
}

TEST(VimTest, VerticalMotionRemembersTheColumnAcrossAShortLine)
{
    Vim v(QStringLiteral("abcdef\nx\nabcdef\n"), 4);
    v.keys(QStringLiteral("j"));
    EXPECT_EQ(v.cursor(), 7); // the short line's only character
    v.keys(QStringLiteral("j"));
    EXPECT_EQ(v.cursor(), 9 + 4);
    v.keys(QStringLiteral("$j"));
    EXPECT_EQ(v.cursor(), 14); // no line below: stays on the last character
    v.keys(QStringLiteral("gg$j"));
    EXPECT_EQ(v.cursor(), 7);
}

TEST(VimTest, WordMotions)
{
    Vim v(QStringLiteral("foo.bar  baz\nqux\n"));
    v.keys(QStringLiteral("w"));
    EXPECT_EQ(v.cursor(), 3); // the dot is a word of its own
    v.keys(QStringLiteral("w"));
    EXPECT_EQ(v.cursor(), 4);
    v.keys(QStringLiteral("w"));
    EXPECT_EQ(v.cursor(), 9);
    v.keys(QStringLiteral("w"));
    EXPECT_EQ(v.cursor(), 13); // across the line break
    v.keys(QStringLiteral("b"));
    EXPECT_EQ(v.cursor(), 9);
    v.keys(QStringLiteral("b"));
    EXPECT_EQ(v.cursor(), 4);
    v.keys(QStringLiteral("0e"));
    EXPECT_EQ(v.cursor(), 2);
    v.keys(QStringLiteral("e"));
    EXPECT_EQ(v.cursor(), 3);
    v.keys(QStringLiteral("0W"));
    EXPECT_EQ(v.cursor(), 9); // WORD: foo.bar is one
    v.keys(QStringLiteral("B"));
    EXPECT_EQ(v.cursor(), 0);
}

TEST(VimTest, FindCharacterMotions)
{
    Vim v(QStringLiteral("a,b,c,d\n"));
    v.keys(QStringLiteral("f,"));
    EXPECT_EQ(v.cursor(), 1);
    v.keys(QStringLiteral(";"));
    EXPECT_EQ(v.cursor(), 3);
    v.keys(QStringLiteral(","));
    EXPECT_EQ(v.cursor(), 1);
    v.keys(QStringLiteral("0t,"));
    EXPECT_EQ(v.cursor(), 0);
    v.keys(QStringLiteral("2f,"));
    EXPECT_EQ(v.cursor(), 3);
    v.keys(QStringLiteral("$F,"));
    EXPECT_EQ(v.cursor(), 5);
}

TEST(VimTest, ParagraphMotions)
{
    Vim v(QStringLiteral("a\nb\n\nc\nd\n\ne\n"));
    v.keys(QStringLiteral("}"));
    EXPECT_EQ(v.cursor(), 4); // the empty line
    v.keys(QStringLiteral("}"));
    EXPECT_EQ(v.cursor(), 9);
    v.keys(QStringLiteral("{"));
    EXPECT_EQ(v.cursor(), 4);
}

TEST(VimTest, CountsMultiplyAndPrefix)
{
    Vim v(QStringLiteral("one two three four five six seven eight\n"));
    v.keys(QStringLiteral("3w"));
    EXPECT_EQ(v.cursor(), v.text().indexOf(QStringLiteral("four")));
    v.keys(QStringLiteral("0d2w"));
    EXPECT_EQ(v.text(), QStringLiteral("three four five six seven eight\n"));
    v.keys(QStringLiteral("2d2w"));
    EXPECT_EQ(v.text(), QStringLiteral("seven eight\n"));
    v.keys(QStringLiteral("10x"));
    EXPECT_EQ(v.text(), QStringLiteral("t\n"));
}

TEST(VimTest, DeleteWithMotions)
{
    Vim v(QStringLiteral("alpha beta gamma\n"));
    v.keys(QStringLiteral("dw"));
    EXPECT_EQ(v.text(), QStringLiteral("beta gamma\n"));
    v.keys(QStringLiteral("$db"));
    EXPECT_EQ(v.text(), QStringLiteral("beta a\n"));
    v.keys(QStringLiteral("0d$"));
    EXPECT_EQ(v.text(), QStringLiteral("\n"));
}

TEST(VimTest, DwOnTheLastWordKeepsTheLineBreak)
{
    Vim v(QStringLiteral("one two\nthree\n"), 4);
    v.keys(QStringLiteral("dw"));
    EXPECT_EQ(v.text(), QStringLiteral("one \nthree\n"));
}

TEST(VimTest, DeleteLinesAndPut)
{
    Vim v(QStringLiteral("one\ntwo\nthree\nfour\n"));
    v.keys(QStringLiteral("dd"));
    EXPECT_EQ(v.text(), QStringLiteral("two\nthree\nfour\n"));
    EXPECT_EQ(v.cursor(), 0);
    v.keys(QStringLiteral("jp"));
    EXPECT_EQ(v.text(), QStringLiteral("two\nthree\none\nfour\n"));
    EXPECT_EQ(v.cursor(), v.text().indexOf(QStringLiteral("one")));
    v.keys(QStringLiteral("2dd"));
    EXPECT_EQ(v.text(), QStringLiteral("two\nthree\n"));
    v.keys(QStringLiteral("P"));
    EXPECT_EQ(v.text(), QStringLiteral("two\none\nfour\nthree\n"));
    // The last line goes with the break before it, and the caret lands above.
    Vim w(QStringLiteral("a\nb\nc"), 4);
    w.keys(QStringLiteral("dd"));
    EXPECT_EQ(w.text(), QStringLiteral("a\nb"));
    EXPECT_EQ(w.cursor(), 2);
}

TEST(VimTest, DeletingAndPuttingTheLastLine)
{
    Vim v(QStringLiteral("a\nb"), 0);
    v.keys(QStringLiteral("jddp"));
    EXPECT_EQ(v.text(), QStringLiteral("a\nb"));
    v.keys(QStringLiteral("yyp"));
    EXPECT_EQ(v.text(), QStringLiteral("a\nb\nb"));
}

TEST(VimTest, YankAndPutCharacterwise)
{
    Vim v(QStringLiteral("abc def\n"));
    v.keys(QStringLiteral("yw"));
    EXPECT_EQ(v.text(), QStringLiteral("abc def\n"));
    EXPECT_EQ(v.cursor(), 0);
    v.keys(QStringLiteral("$p"));
    EXPECT_EQ(v.text(), QStringLiteral("abc defabc \n"));
    v.keys(QStringLiteral("0P"));
    EXPECT_EQ(v.text(), QStringLiteral("abc abc defabc \n"));
    v.keys(QStringLiteral("0yyp"));
    EXPECT_EQ(v.text(), QStringLiteral("abc abc defabc \nabc abc defabc \n"));
}

TEST(VimTest, YankDoesNotTouchTheClipboard)
{
    Vim v(QStringLiteral("abc\n"));
    QApplication::clipboard()->setText(QStringLiteral("clip"));
    v.keys(QStringLiteral("yy"));
    EXPECT_EQ(QApplication::clipboard()->text(), QStringLiteral("clip"));
}

TEST(VimTest, ChangeEntersInsertAndIsOneUndoStep)
{
    Vim v(QStringLiteral("alpha beta\n"));
    v.keys(QStringLiteral("cwxyz"));
    EXPECT_EQ(v.mode(), QStringLiteral("insert"));
    EXPECT_EQ(v.text(), QStringLiteral("xyz beta\n"));
    v.esc();
    v.keys(QStringLiteral("u"));
    EXPECT_EQ(v.text(), QStringLiteral("alpha beta\n"));
    v.key(Qt::Key_R, Qt::ControlModifier);
    EXPECT_EQ(v.text(), QStringLiteral("xyz beta\n"));
}

TEST(VimTest, CcAndCReplaceTheLineAndTheRest)
{
    Vim v(QStringLiteral("one\ntwo three\nfour\n"));
    v.keys(QStringLiteral("jccX"));
    v.esc();
    EXPECT_EQ(v.text(), QStringLiteral("one\nX\nfour\n"));
    Vim w(QStringLiteral("one two\n"), 4);
    w.keys(QStringLiteral("CX"));
    w.esc();
    EXPECT_EQ(w.text(), QStringLiteral("one X\n"));
    Vim x(QStringLiteral("one two\n"), 0);
    x.keys(QStringLiteral("sX"));
    x.esc();
    EXPECT_EQ(x.text(), QStringLiteral("Xne two\n"));
    x.keys(QStringLiteral("SY"));
    x.esc();
    EXPECT_EQ(x.text(), QStringLiteral("Y\n"));
}

TEST(VimTest, UndoRedoOneStepPerCommand)
{
    Vim v(QStringLiteral("one\ntwo\nthree\n"));
    v.keys(QStringLiteral("dd"));
    v.keys(QStringLiteral("dd"));
    EXPECT_EQ(v.text(), QStringLiteral("three\n"));
    v.keys(QStringLiteral("u"));
    EXPECT_EQ(v.text(), QStringLiteral("two\nthree\n"));
    v.keys(QStringLiteral("u"));
    EXPECT_EQ(v.text(), QStringLiteral("one\ntwo\nthree\n"));
    v.keys(QStringLiteral("u")); // nothing left to undo
    EXPECT_EQ(v.text(), QStringLiteral("one\ntwo\nthree\n"));
    v.key(Qt::Key_R, Qt::ControlModifier);
    v.key(Qt::Key_R, Qt::ControlModifier);
    EXPECT_EQ(v.text(), QStringLiteral("three\n"));
}

TEST(VimTest, AnInsertSessionIsOneUndoStepEvenWhenTypedSlowly)
{
    Vim v(QStringLiteral("x\n"));
    v.keys(QStringLiteral("A"));
    v.keys(QStringLiteral("ab"));
    QTest::qWait(650); // longer than the typing-burst window
    v.keys(QStringLiteral("cd"));
    v.esc();
    EXPECT_EQ(v.text(), QStringLiteral("xabcd\n"));
    v.keys(QStringLiteral("u"));
    EXPECT_EQ(v.text(), QStringLiteral("x\n"));
}

TEST(VimTest, AnEmptyInsertLeavesNoUndoStep)
{
    Vim v(QStringLiteral("a\nb\n"));
    v.keys(QStringLiteral("x"));
    v.keys(QStringLiteral("i"));
    v.esc();
    v.keys(QStringLiteral("u"));
    EXPECT_EQ(v.text(), QStringLiteral("a\nb\n")); // the x, not the empty insert
}

TEST(VimTest, XAndFriends)
{
    Vim v(QStringLiteral("abcdef\n"), 2);
    v.keys(QStringLiteral("x"));
    EXPECT_EQ(v.text(), QStringLiteral("abdef\n"));
    v.keys(QStringLiteral("X"));
    EXPECT_EQ(v.text(), QStringLiteral("adef\n"));
    v.keys(QStringLiteral("p"));
    EXPECT_EQ(v.text(), QStringLiteral("adbef\n"));
    v.keys(QStringLiteral("$x"));
    EXPECT_EQ(v.cursor(), v.text().size() - 2); // the caret follows onto the new last character
    Vim w(QStringLiteral("abc\n"), 1);
    w.keys(QStringLiteral("D"));
    EXPECT_EQ(w.text(), QStringLiteral("a\n"));
    EXPECT_EQ(w.cursor(), 0);
}

TEST(VimTest, ReplaceJoinAndToggleCase)
{
    Vim v(QStringLiteral("abcd\n"));
    v.keys(QStringLiteral("rX"));
    EXPECT_EQ(v.text(), QStringLiteral("Xbcd\n"));
    v.keys(QStringLiteral("2rZ"));
    EXPECT_EQ(v.text(), QStringLiteral("ZZcd\n"));
    EXPECT_EQ(v.cursor(), 1);
    v.keys(QStringLiteral("0~~"));
    EXPECT_EQ(v.text(), QStringLiteral("zzcd\n"));
    v.keys(QStringLiteral("9rQ")); // more than the line holds: refused
    EXPECT_EQ(v.text(), QStringLiteral("zzcd\n"));

    Vim j(QStringLiteral("one\n   two\nthree\nfour\n"));
    j.keys(QStringLiteral("J"));
    EXPECT_EQ(j.text(), QStringLiteral("one two\nthree\nfour\n"));
    EXPECT_EQ(j.cursor(), 3);
    j.keys(QStringLiteral("3J"));
    EXPECT_EQ(j.text(), QStringLiteral("one two three four\n"));
    j.keys(QStringLiteral("u"));
    EXPECT_EQ(j.text(), QStringLiteral("one two\nthree\nfour\n"));
}

TEST(VimTest, DotRepeatsTheLastChange)
{
    Vim v(QStringLiteral("a b c d e f\n"));
    v.keys(QStringLiteral("dw"));
    v.keys(QStringLiteral("."));
    EXPECT_EQ(v.text(), QStringLiteral("c d e f\n"));
    v.keys(QStringLiteral("cwX"));
    v.esc();
    v.keys(QStringLiteral("w."));
    EXPECT_EQ(v.text(), QStringLiteral("X X e f\n"));
    v.keys(QStringLiteral("0A!"));
    v.esc();
    v.keys(QStringLiteral("."));
    EXPECT_EQ(v.text(), QStringLiteral("X X e f!!\n"));
    v.keys(QStringLiteral("u"));
    EXPECT_EQ(v.text(), QStringLiteral("X X e f!\n"));
}

TEST(VimTest, VisualCharacterwiseOperators)
{
    Vim v(QStringLiteral("hello world\n"));
    v.keys(QStringLiteral("vlld"));
    EXPECT_EQ(v.text(), QStringLiteral("lo world\n"));
    EXPECT_EQ(v.mode(), QStringLiteral("normal"));
    v.keys(QStringLiteral("vey"));
    EXPECT_EQ(v.text(), QStringLiteral("lo world\n"));
    v.keys(QStringLiteral("$p"));
    EXPECT_EQ(v.text(), QStringLiteral("lo worldlo\n"));
    v.keys(QStringLiteral("0vec"));
    EXPECT_EQ(v.mode(), QStringLiteral("insert"));
    v.keys(QStringLiteral("X"));
    v.esc();
    EXPECT_EQ(v.text(), QStringLiteral("X worldlo\n"));
}

TEST(VimTest, VisualSelectionIncludesTheCharacterUnderTheCaret)
{
    Vim v(QStringLiteral("abcdef\n"), 1);
    v.keys(QStringLiteral("vll"));
    EXPECT_EQ(v.editor.selectedText(), QStringLiteral("bcd"));
    v.keys(QStringLiteral("o"));
    v.keys(QStringLiteral("h"));
    EXPECT_EQ(v.editor.selectedText(), QStringLiteral("abcd"));
}

TEST(VimTest, VisualLineOperators)
{
    Vim v(QStringLiteral("one\ntwo\nthree\nfour\n"));
    v.keys(QStringLiteral("Vjd"));
    EXPECT_EQ(v.text(), QStringLiteral("three\nfour\n"));
    v.keys(QStringLiteral("Vjy"));
    v.keys(QStringLiteral("Gp"));
    EXPECT_EQ(v.text(), QStringLiteral("three\nfour\nthree\nfour\n"));
    v.keys(QStringLiteral("ggVcX"));
    v.esc();
    EXPECT_EQ(v.text(), QStringLiteral("X\nfour\nthree\nfour\n"));
}

TEST(VimTest, AClickMovingTheCaretEndsVisualMode)
{
    Vim v(QStringLiteral("abcdef\n"));
    v.keys(QStringLiteral("vll"));
    v.editor.setCursorOffset(5); // what a mouse press does
    v.keys(QStringLiteral("x"));
    EXPECT_EQ(v.mode(), QStringLiteral("normal"));
    EXPECT_EQ(v.text(), QStringLiteral("abcde\n")); // x deleted one character, not a selection
}

TEST(VimTest, VisualLineShowsWholeLinesAsTheSelection)
{
    Vim v(QStringLiteral("one\ntwo\nthree\n"), 1);
    v.keys(QStringLiteral("Vj"));
    EXPECT_EQ(v.editor.selectedText(), QStringLiteral("one\ntwo"));
}

TEST(VimTest, Search)
{
    Vim v(QStringLiteral("foo bar\nbaz foo\nqux foo\n"));
    v.keys(QStringLiteral("/foo"));
    EXPECT_EQ(v.editor.vimStatus(), QStringLiteral("/foo"));
    v.key(Qt::Key_Return);
    EXPECT_EQ(v.cursor(), 12);
    v.keys(QStringLiteral("n"));
    EXPECT_EQ(v.cursor(), 20);
    v.keys(QStringLiteral("n")); // wraps
    EXPECT_EQ(v.cursor(), 0);
    v.keys(QStringLiteral("N"));
    EXPECT_EQ(v.cursor(), 20);
    v.keys(QStringLiteral("?baz"));
    v.key(Qt::Key_Return);
    EXPECT_EQ(v.cursor(), 8);
    v.keys(QStringLiteral("/nothing"));
    v.key(Qt::Key_Return);
    EXPECT_EQ(v.cursor(), 8);
    EXPECT_TRUE(v.editor.vimStatus().startsWith(QStringLiteral("E486")));
}

TEST(VimTest, DeleteToSearchMatch)
{
    Vim v(QStringLiteral("one two three\n"));
    v.keys(QStringLiteral("/three"));
    v.key(Qt::Key_Return);
    v.keys(QStringLiteral("0dn"));
    EXPECT_EQ(v.text(), QStringLiteral("three\n"));
}

TEST(VimTest, EscapeCancelsAPendingCommandAndTheCommandLine)
{
    Vim v(QStringLiteral("abc\n"));
    v.keys(QStringLiteral("2d"));
    EXPECT_EQ(v.editor.vimStatus(), QStringLiteral("-- NORMAL --  2d"));
    v.esc();
    v.keys(QStringLiteral("x"));
    EXPECT_EQ(v.text(), QStringLiteral("bc\n"));
    v.keys(QStringLiteral(":wq"));
    v.esc();
    EXPECT_EQ(v.editor.vimStatus(), QStringLiteral("-- NORMAL --"));
}

TEST(VimTest, ExCommandsEmitSignals)
{
    Vim v(QStringLiteral("abc\n"));
    QSignalSpy save(&v.editor, &MarkdownEditor::vimSaveRequested);
    QSignalSpy close(&v.editor, &MarkdownEditor::vimCloseRequested);
    v.keys(QStringLiteral(":w"));
    v.key(Qt::Key_Return);
    EXPECT_EQ(save.count(), 1);
    EXPECT_EQ(close.count(), 0);
    v.keys(QStringLiteral(":q"));
    v.key(Qt::Key_Return);
    EXPECT_EQ(close.count(), 1);
    v.keys(QStringLiteral("x:q"));
    v.key(Qt::Key_Return);
    EXPECT_EQ(close.count(), 1); // modified: refused
    EXPECT_TRUE(v.editor.vimStatus().startsWith(QStringLiteral("E37")));
    v.keys(QStringLiteral(":wq"));
    v.key(Qt::Key_Return);
    EXPECT_EQ(save.count(), 2);
    EXPECT_EQ(close.count(), 2);
}

TEST(VimTest, ColonNumberGoesToALine)
{
    Vim v(QStringLiteral("a\n  b\nc\n"));
    v.keys(QStringLiteral(":2"));
    v.key(Qt::Key_Return);
    EXPECT_EQ(v.cursor(), 4);
}

TEST(VimTest, InsertModeLeavesEveryOtherKeyAlone)
{
    Vim v(QStringLiteral("\n"));
    v.keys(QStringLiteral("i"));
    v.keys(QStringLiteral("dd jk")); // vim commands are plain text here
    EXPECT_EQ(v.text(), QStringLiteral("dd jk\n"));
    v.key(Qt::Key_Z, Qt::ControlModifier); // the editor's own undo still works
    EXPECT_EQ(v.text(), QStringLiteral("\n"));
}

TEST(VimTest, CtrlChordsOutsideInsertStillReachTheEditor)
{
    Vim v(QStringLiteral("abc\n"));
    v.keys(QStringLiteral("x"));
    v.key(Qt::Key_Z, Qt::ControlModifier);
    EXPECT_EQ(v.text(), QStringLiteral("abc\n"));
}

TEST(VimTest, ComposedTextIsNotTypedInNormalMode)
{
    Vim v(QStringLiteral("abc\n"));
    QInputMethodEvent event;
    event.setCommitString(QStringLiteral("zzz"));
    QApplication::sendEvent(&v.editor, &event);
    EXPECT_EQ(v.text(), QStringLiteral("abc\n"));
    v.keys(QStringLiteral("i"));
    QInputMethodEvent insert;
    insert.setCommitString(QStringLiteral("zzz"));
    QApplication::sendEvent(&v.editor, &insert);
    EXPECT_EQ(v.text(), QStringLiteral("zzzabc\n"));
}

TEST(VimTest, ArrowsMoveLikeHjklAndNeverPastTheLastCharacter)
{
    Vim v(QStringLiteral("abc\n"));
    v.key(Qt::Key_Right);
    v.key(Qt::Key_Right);
    v.key(Qt::Key_Right);
    EXPECT_EQ(v.cursor(), 2);
    v.key(Qt::Key_Left);
    EXPECT_EQ(v.cursor(), 1);
    v.key(Qt::Key_End);
    EXPECT_EQ(v.cursor(), 2);
    v.key(Qt::Key_Home);
    EXPECT_EQ(v.cursor(), 0);
}

TEST(VimTest, TurningItOffRestoresPlainEditing)
{
    Vim v(QStringLiteral("abc\n"));
    v.editor.setVimMode(false);
    v.keys(QStringLiteral("dd"));
    EXPECT_EQ(v.text(), QStringLiteral("ddabc\n"));
    EXPECT_TRUE(v.editor.vimStatus().isEmpty());
}

TEST(VimTest, StatusSignalFollowsTheMode)
{
    Vim v(QStringLiteral("abc\n"));
    QSignalSpy spy(&v.editor, &MarkdownEditor::vimStatusChanged);
    v.keys(QStringLiteral("i"));
    ASSERT_GE(spy.count(), 1);
    EXPECT_EQ(spy.last().at(0).toString(), QStringLiteral("-- INSERT --"));
    v.esc();
    EXPECT_EQ(spy.last().at(0).toString(), QStringLiteral("-- NORMAL --"));
}

// The caret is a block over the character in normal mode and a bar in insert
// mode: looked for in the pixels, since that is what a person sees.
TEST(VimTest, TheCaretIsABlockInNormalModeAndABarInInsertMode)
{
    Vim v(QStringLiteral("Wide text here\n"));
    v.editor.resize(600, 300);
    v.editor.show();
    ASSERT_TRUE(QTest::qWaitForWindowExposed(&v.editor));
    v.editor.activateWindow();
    v.editor.setFocus();
    ASSERT_TRUE(QTest::qWaitFor([&] { return v.editor.hasFocus(); }, 2000));

    // Count the pixels of the caret's colour in the first character's cell.
    const auto caretPixels = [&v] {
        v.editor.setCursorOffset(0); // restarts the blink with the caret shown
        const QImage image = v.editor.viewport()->grab().toImage();
        const QColor background = Theme::builtin(Theme::Mode::Dark).color(QStringLiteral("--background-primary"));
        int changed = 0;
        for (int y = 0; y < image.height() / 3; ++y)
            for (int x = 0; x < image.width() / 3; ++x)
                changed += image.pixelColor(x, y) != background;
        return changed;
    };
    v.editor.setVimMode(false);
    const int withoutVim = caretPixels();
    v.editor.setVimMode(true);
    const int block = caretPixels();
    v.keys(QStringLiteral("i"));
    const int bar = caretPixels();
    EXPECT_GT(block, bar) << "a block covers more than a 2 px bar (block " << block << ", bar " << bar << ")";
    EXPECT_EQ(bar, withoutVim) << "insert mode draws the editor's own caret";
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
