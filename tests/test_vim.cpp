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

// --- text objects, % * #, > <, registers, marks, macros, block-visual, :s -------------

TEST(VimTest, WordObjects)
{
    {
        Vim v(QStringLiteral("foo bar baz\n"), 5);
        v.keys(QStringLiteral("ciwX"));
        v.esc();
        EXPECT_EQ(v.text(), QStringLiteral("foo X baz\n"));
    }
    {
        Vim v(QStringLiteral("foo bar baz\n"), 5);
        v.keys(QStringLiteral("daw"));
        EXPECT_EQ(v.text(), QStringLiteral("foo baz\n"));
    }
    {
        Vim v(QStringLiteral("foo bar\n"), 5);
        v.keys(QStringLiteral("daw")); // no space after: the one before goes
        EXPECT_EQ(v.text(), QStringLiteral("foo\n"));
    }
    {
        Vim v(QStringLiteral("a b c d\n"));
        v.keys(QStringLiteral("d2aw"));
        EXPECT_EQ(v.text(), QStringLiteral("c d\n"));
    }
    {
        Vim v(QStringLiteral("foo   bar\n"), 4);
        v.keys(QStringLiteral("diw")); // on space: the space
        EXPECT_EQ(v.text(), QStringLiteral("foobar\n"));
    }
    {
        Vim v(QStringLiteral("foo-bar baz\n"), 1);
        v.keys(QStringLiteral("diW"));
        EXPECT_EQ(v.text(), QStringLiteral(" baz\n"));
    }
    {
        Vim v(QStringLiteral("foo-bar baz\n"), 1);
        v.keys(QStringLiteral("diw"));
        EXPECT_EQ(v.text(), QStringLiteral("-bar baz\n"));
    }
    {
        Vim v(QStringLiteral("foo bar baz\n"), 5);
        v.keys(QStringLiteral("viw"));
        EXPECT_EQ(v.editor.selectedText(), QStringLiteral("bar"));
        v.esc();
        v.keys(QStringLiteral("vawy"));
        v.keys(QStringLiteral("$p"));
        EXPECT_EQ(v.text(), QStringLiteral("foo bar bazbar \n"));
    }
    {
        Vim v(QStringLiteral("foo bar baz\n"));
        v.keys(QStringLiteral("ciwX"));
        v.esc();
        v.keys(QStringLiteral("w."));
        EXPECT_EQ(v.text(), QStringLiteral("X X baz\n")); // . repeats the object too
    }
}

TEST(VimTest, SentenceObjects)
{
    {
        Vim v(QStringLiteral("One. Two three. Four.\n"), 6);
        v.keys(QStringLiteral("dis"));
        EXPECT_EQ(v.text(), QStringLiteral("One.  Four.\n"));
    }
    {
        Vim v(QStringLiteral("One. Two three. Four.\n"), 6);
        v.keys(QStringLiteral("das"));
        EXPECT_EQ(v.text(), QStringLiteral("One. Four.\n"));
    }
    {
        Vim v(QStringLiteral("One. Two three. Four.\n"), 18);
        v.keys(QStringLiteral("das")); // the last: the space before goes
        EXPECT_EQ(v.text(), QStringLiteral("One. Two three.\n"));
    }
}

TEST(VimTest, ParagraphObjects)
{
    {
        Vim v(QStringLiteral("a\nb\n\nc\nd\n\ne\n"), 2);
        v.keys(QStringLiteral("dip"));
        EXPECT_EQ(v.text(), QStringLiteral("\nc\nd\n\ne\n"));
    }
    {
        Vim v(QStringLiteral("a\nb\n\nc\nd\n\ne\n"), 2);
        v.keys(QStringLiteral("dap"));
        EXPECT_EQ(v.text(), QStringLiteral("c\nd\n\ne\n"));
    }
    {
        Vim v(QStringLiteral("a\nb\n\nc\nd\n\ne\n"), 10);
        v.keys(QStringLiteral("dap")); // the last paragraph: the blank line before goes
        EXPECT_EQ(v.text(), QStringLiteral("a\nb\n\nc\nd\n"));
    }
    {
        Vim v(QStringLiteral("a\nb\n\nc\n"), 0);
        v.keys(QStringLiteral("vip"));
        EXPECT_EQ(v.mode(), QStringLiteral("visual line"));
        EXPECT_EQ(v.editor.selectedText(), QStringLiteral("a\nb"));
    }
}

TEST(VimTest, QuoteObjects)
{
    {
        Vim v(QStringLiteral("say \"hello world\" now\n"), 7);
        v.keys(QStringLiteral("ci\"X"));
        v.esc();
        EXPECT_EQ(v.text(), QStringLiteral("say \"X\" now\n"));
    }
    {
        Vim v(QStringLiteral("say \"hello world\" now\n"));
        v.keys(QStringLiteral("di\"")); // not inside one: the first after the caret
        EXPECT_EQ(v.text(), QStringLiteral("say \"\" now\n"));
    }
    {
        Vim v(QStringLiteral("say \"hello world\" now\n"), 7);
        v.keys(QStringLiteral("da\""));
        EXPECT_EQ(v.text(), QStringLiteral("say now\n"));
    }
    {
        Vim v(QStringLiteral("a 'b c' d `e f` g\n"), 4);
        v.keys(QStringLiteral("di'"));
        EXPECT_EQ(v.text(), QStringLiteral("a '' d `e f` g\n"));
        v.keys(QStringLiteral("f`ci`Z"));
        v.esc();
        EXPECT_EQ(v.text(), QStringLiteral("a '' d `Z` g\n"));
    }
    {
        Vim v(QStringLiteral("a \"b\\\"c\" d\n"), 4);
        v.keys(QStringLiteral("di\"")); // the escaped quote does not end it
        EXPECT_EQ(v.text(), QStringLiteral("a \"\" d\n"));
    }
}

TEST(VimTest, BracketObjects)
{
    const QString nested = QStringLiteral("f(a, (b), c) x\n");
    {
        Vim v(nested, 6);
        v.keys(QStringLiteral("di("));
        EXPECT_EQ(v.text(), QStringLiteral("f(a, (), c) x\n"));
    }
    {
        Vim v(nested, 6);
        v.keys(QStringLiteral("d2i("));
        EXPECT_EQ(v.text(), QStringLiteral("f() x\n"));
    }
    {
        Vim v(nested, 6);
        v.keys(QStringLiteral("dab"));
        EXPECT_EQ(v.text(), QStringLiteral("f(a, , c) x\n"));
    }
    {
        Vim v(nested, 7); // on the closing bracket
        v.keys(QStringLiteral("da)"));
        EXPECT_EQ(v.text(), QStringLiteral("f(a, , c) x\n"));
    }
    {
        Vim v(QStringLiteral("x[1, 2] <a> {b}\n"), 3);
        v.keys(QStringLiteral("ci[Q"));
        v.esc();
        EXPECT_EQ(v.text(), QStringLiteral("x[Q] <a> {b}\n"));
        v.keys(QStringLiteral("fadi>"));
        EXPECT_EQ(v.text(), QStringLiteral("x[Q] <> {b}\n"));
        v.keys(QStringLiteral("fbdiB"));
        EXPECT_EQ(v.text(), QStringLiteral("x[Q] <> {}\n"));
    }
    {
        Vim v(QStringLiteral("fn {\n  a\n  b\n}\n"), 7);
        v.keys(QStringLiteral("di{")); // brackets on their own lines: the lines between
        EXPECT_EQ(v.text(), QStringLiteral("fn {\n}\n"));
    }
    {
        Vim v(QStringLiteral("fn {\n  a\n  b\n}\n"), 7);
        v.keys(QStringLiteral("ci{X"));
        v.esc();
        EXPECT_EQ(v.text(), QStringLiteral("fn {\nX\n}\n"));
    }
    {
        Vim v(QStringLiteral("no brackets\n"), 3);
        v.keys(QStringLiteral("di("));
        EXPECT_EQ(v.text(), QStringLiteral("no brackets\n"));
    }
    {
        Vim v(QStringLiteral("g(foo bar)\n"), 4);
        v.keys(QStringLiteral("vi("));
        EXPECT_EQ(v.editor.selectedText(), QStringLiteral("foo bar"));
        v.esc();
        v.keys(QStringLiteral("ya(P"));
        EXPECT_EQ(v.text(), QStringLiteral("g(foo bar)(foo bar)\n"));
    }
}

TEST(VimTest, PercentJumpsToTheMatchingBracket)
{
    Vim v(QStringLiteral("x (a [b] {c})\n"));
    v.keys(QStringLiteral("%")); // the first bracket on the line
    EXPECT_EQ(v.cursor(), 12);
    v.keys(QStringLiteral("%"));
    EXPECT_EQ(v.cursor(), 2);
    v.keys(QStringLiteral("fb%"));
    EXPECT_EQ(v.cursor(), 5); // from b the first bracket ahead is ], whose partner is [
    v.keys(QStringLiteral("0d%"));
    EXPECT_EQ(v.text(), QStringLiteral("\n"));
}

TEST(VimTest, PercentOverMultipleLines)
{
    Vim v(QStringLiteral("{\n  (a)\n}\n"));
    v.keys(QStringLiteral("%"));
    EXPECT_EQ(v.cursor(), 8);
    v.keys(QStringLiteral("%"));
    EXPECT_EQ(v.cursor(), 0);
}

TEST(VimTest, IndentAndOutdent)
{
    {
        Vim v(QStringLiteral("a\nb\n"));
        v.keys(QStringLiteral(">>"));
        EXPECT_EQ(v.text(), QStringLiteral("    a\nb\n"));
        EXPECT_EQ(v.cursor(), 4);
        v.keys(QStringLiteral("<<"));
        EXPECT_EQ(v.text(), QStringLiteral("a\nb\n"));
    }
    {
        Vim v(QStringLiteral("a\n\nb\nc\n"));
        v.keys(QStringLiteral("3>>")); // empty lines stay empty
        EXPECT_EQ(v.text(), QStringLiteral("    a\n\n    b\nc\n"));
        v.keys(QStringLiteral("u"));
        EXPECT_EQ(v.text(), QStringLiteral("a\n\nb\nc\n"));
    }
    {
        Vim v(QStringLiteral("a\nb\nc\n"));
        v.keys(QStringLiteral(">j"));
        EXPECT_EQ(v.text(), QStringLiteral("    a\n    b\nc\n"));
        v.keys(QStringLiteral("gg>ip"));
        EXPECT_EQ(v.text(), QStringLiteral("        a\n        b\n    c\n"));
    }
    {
        Vim v(QStringLiteral("a\nb\nc\n"));
        v.editor.setTabWidth(2);
        v.keys(QStringLiteral("Vj>"));
        EXPECT_EQ(v.text(), QStringLiteral("  a\n  b\nc\n"));
        EXPECT_EQ(v.mode(), QStringLiteral("normal"));
        v.keys(QStringLiteral("gg"));
        v.keys(QStringLiteral("Vj2>"));
        EXPECT_EQ(v.text(), QStringLiteral("      a\n      b\nc\n"));
        v.keys(QStringLiteral("gg"));
        v.keys(QStringLiteral("Vj<"));
        EXPECT_EQ(v.text(), QStringLiteral("    a\n    b\nc\n"));
    }
    {
        Vim v(QStringLiteral("\tfoo\n  bar\n"));
        v.keys(QStringLiteral("<<"));
        EXPECT_EQ(v.text(), QStringLiteral("foo\n  bar\n")); // a tab counts as one level
        v.keys(QStringLiteral("j<<"));
        EXPECT_EQ(v.text(), QStringLiteral("foo\nbar\n"));
    }
}

TEST(VimTest, NamedRegisters)
{
    {
        Vim v(QStringLiteral("one\ntwo\n"));
        v.keys(QStringLiteral("\"ayyj\"byy"));
        v.keys(QStringLiteral("\"ap"));
        EXPECT_EQ(v.text(), QStringLiteral("one\ntwo\none\n"));
        v.keys(QStringLiteral("\"bP"));
        EXPECT_EQ(v.text(), QStringLiteral("one\ntwo\ntwo\none\n"));
        v.keys(QStringLiteral("\"Ayy")); // appends the current line ("two") to a
        v.keys(QStringLiteral("G\"ap"));
        EXPECT_EQ(v.text(), QStringLiteral("one\ntwo\ntwo\none\none\ntwo\n"));
    }
    {
        Vim v(QStringLiteral("foo bar\n"));
        v.keys(QStringLiteral("yiwwdiw")); // the delete overwrites "" but not "0
        v.keys(QStringLiteral("\"0P"));
        EXPECT_EQ(v.text(), QStringLiteral("foofoo \n"));
        v.keys(QStringLiteral("u"));
        v.keys(QStringLiteral("\"\"P"));
        EXPECT_EQ(v.text(), QStringLiteral("foobar \n"));
    }
    {
        Vim v(QStringLiteral("a\nb\n"));
        v.keys(QStringLiteral("yyj\"_ddp")); // the black hole keeps what was yanked
        EXPECT_EQ(v.text(), QStringLiteral("a\na\n"));
    }
    {
        Vim v(QStringLiteral("abc def\n"));
        v.keys(QStringLiteral("\"xdw"));
        v.keys(QStringLiteral("\"xp"));
        EXPECT_EQ(v.text(), QStringLiteral("dabc ef\n"));
    }
}

TEST(VimTest, ClipboardRegister)
{
    {
        Vim v(QStringLiteral("one\ntwo\n"));
        v.keys(QStringLiteral("\"+yy"));
        EXPECT_EQ(QApplication::clipboard()->text(), QStringLiteral("one\n"));
        v.keys(QStringLiteral("\"+yiw"));
        EXPECT_EQ(QApplication::clipboard()->text(), QStringLiteral("one"));
    }
    {
        QApplication::clipboard()->setText(QStringLiteral("clip"));
        Vim v(QStringLiteral("ab\n"));
        v.keys(QStringLiteral("\"+p"));
        EXPECT_EQ(v.text(), QStringLiteral("aclipb\n"));
    }
    {
        QApplication::clipboard()->setText(QStringLiteral("line\n"));
        Vim v(QStringLiteral("ab\n"));
        v.keys(QStringLiteral("\"*p")); // text ending in a newline goes in as a line
        EXPECT_EQ(v.text(), QStringLiteral("ab\nline\n"));
    }
    {
        QApplication::clipboard()->setText(QStringLiteral("keep"));
        Vim v(QStringLiteral("ab\n"));
        v.keys(QStringLiteral("yy"));
        EXPECT_EQ(QApplication::clipboard()->text(), QStringLiteral("keep")); // plain yanks stay off it
    }
}

TEST(VimTest, Marks)
{
    {
        Vim v(QStringLiteral("one\ntwo\nthree\nfour\n"), 2);
        v.keys(QStringLiteral("ma"));
        v.keys(QStringLiteral("G'a"));
        EXPECT_EQ(v.cursor(), 0); // ' goes to the line's first non-blank
        v.keys(QStringLiteral("G`a"));
        EXPECT_EQ(v.cursor(), 2); // ` to the exact place
    }
    {
        Vim v(QStringLiteral("one\ntwo\nthree\nfour\n"));
        v.keys(QStringLiteral("majjd'a")); // a linewise motion
        EXPECT_EQ(v.text(), QStringLiteral("four\n"));
    }
    {
        Vim v(QStringLiteral("one two\n"));
        v.keys(QStringLiteral("wmb0d`b")); // a characterwise one
        EXPECT_EQ(v.text(), QStringLiteral("two\n"));
    }
    {
        Vim v(QStringLiteral("one\ntwo\nthree\n"));
        v.keys(QStringLiteral("jma"));
        v.keys(QStringLiteral("ggOnew"));
        v.esc();
        v.keys(QStringLiteral("'a")); // the mark moved with its text
        EXPECT_EQ(v.cursor(), v.text().indexOf(QStringLiteral("two")));
    }
    {
        Vim v(QStringLiteral("one\ntwo\nthree\n"));
        v.keys(QStringLiteral("G``"));
        EXPECT_EQ(v.cursor(), 0); // back to where the jump began
        v.keys(QStringLiteral("''"));
        EXPECT_EQ(v.cursor(), v.text().indexOf(QStringLiteral("three")));
    }
    {
        Vim v(QStringLiteral("one\n"));
        v.keys(QStringLiteral("'z"));
        EXPECT_TRUE(v.editor.vimStatus().startsWith(QStringLiteral("E20")));
    }
}

TEST(VimTest, Macros)
{
    {
        Vim v(QStringLiteral("a\nb\nc\nd\n"));
        v.keys(QStringLiteral("qa"));
        EXPECT_TRUE(v.editor.vimStatus().contains(QStringLiteral("recording @a")));
        v.keys(QStringLiteral("A!"));
        v.esc();
        v.keys(QStringLiteral("jq"));
        EXPECT_FALSE(v.editor.vimStatus().contains(QStringLiteral("recording")));
        EXPECT_EQ(v.text(), QStringLiteral("a!\nb\nc\nd\n"));
        v.keys(QStringLiteral("@a"));
        EXPECT_EQ(v.text(), QStringLiteral("a!\nb!\nc\nd\n"));
        v.keys(QStringLiteral("@@"));
        EXPECT_EQ(v.text(), QStringLiteral("a!\nb!\nc!\nd\n"));
        v.keys(QStringLiteral("u")); // the replayed insert is one undo step
        EXPECT_EQ(v.text(), QStringLiteral("a!\nb!\nc\nd\n"));
    }
    {
        Vim v(QStringLiteral("1\n2\n3\n4\n5\n"));
        v.keys(QStringLiteral("qqI-"));
        v.esc();
        v.keys(QStringLiteral("jq"));
        v.keys(QStringLiteral("3@q"));
        EXPECT_EQ(v.text(), QStringLiteral("-1\n-2\n-3\n-4\n5\n"));
    }
    {
        Vim v(QStringLiteral("one two three\n"));
        v.keys(QStringLiteral("qwdwq")); // normal-mode commands only
        v.keys(QStringLiteral("@w"));
        EXPECT_EQ(v.text(), QStringLiteral("three\n"));
    }
    {
        Vim v(QStringLiteral("x\n"));
        v.keys(QStringLiteral("qaAy"));
        v.esc();
        v.keys(QStringLiteral("q"));
        v.keys(QStringLiteral("qAAz"));
        v.esc();
        v.keys(QStringLiteral("q")); // qA appends to a
        EXPECT_EQ(v.text(), QStringLiteral("xyz\n"));
        v.keys(QStringLiteral("@a"));
        EXPECT_EQ(v.text(), QStringLiteral("xyzyz\n"));
    }
    {
        Vim v(QStringLiteral("x\n"));
        v.keys(QStringLiteral("@b")); // nothing recorded: nothing happens
        EXPECT_EQ(v.text(), QStringLiteral("x\n"));
        v.keys(QStringLiteral("qaq")); // an empty macro that runs itself
        v.keys(QStringLiteral("qa@aq"));
        v.keys(QStringLiteral("@a"));
        EXPECT_EQ(v.text(), QStringLiteral("x\n"));
    }
}

TEST(VimTest, LAndHAtALineEdgeFailARunningMacro)
{
    {
        Vim v(QStringLiteral("abcdef\n"));
        v.keys(QStringLiteral("qalxq")); // "acdef", the caret on c
        v.keys(QStringLiteral("10@a"));  // the third l finds the last character: the macro ends
        EXPECT_EQ(v.text(), QStringLiteral("ace\n"));
    }
    {
        Vim v(QStringLiteral("abc\n"), 2);
        v.keys(QStringLiteral("qahxq")); // "ac", the caret on c
        v.keys(QStringLiteral("5@a"));   // the second h finds column 0
        EXPECT_EQ(v.text(), QStringLiteral("c\n"));
    }
    {
        // Outside a macro they still just stay put, and `dl` still takes the last character.
        Vim v(QStringLiteral("ab\n"), 1);
        v.keys(QStringLiteral("lh"));
        EXPECT_EQ(v.cursor(), 0);
        v.keys(QStringLiteral("$dl"));
        EXPECT_EQ(v.text(), QStringLiteral("a\n"));
    }
}

TEST(VimTest, BlockVisualDelete)
{
    Vim v(QStringLiteral("abcd\nefgh\nijkl\n"), 1);
    v.key(Qt::Key_V, Qt::ControlModifier);
    EXPECT_EQ(v.mode(), QStringLiteral("visual block"));
    v.keys(QStringLiteral("jl"));
    EXPECT_EQ(v.editor.vimStatus(), QStringLiteral("-- VISUAL BLOCK --"));
    v.keys(QStringLiteral("d"));
    EXPECT_EQ(v.text(), QStringLiteral("ad\neh\nijkl\n"));
    EXPECT_EQ(v.mode(), QStringLiteral("normal"));
    v.keys(QStringLiteral("u")); // one undo step for the whole block
    EXPECT_EQ(v.text(), QStringLiteral("abcd\nefgh\nijkl\n"));
}

TEST(VimTest, BlockVisualYankAndPaste)
{
    Vim v(QStringLiteral("abcd\nefgh\nijkl\n"), 1);
    v.key(Qt::Key_V, Qt::ControlModifier);
    v.keys(QStringLiteral("jly"));
    EXPECT_EQ(v.cursor(), 1);
    v.keys(QStringLiteral("jjp")); // a block goes in as a block, adding lines it needs
    EXPECT_EQ(v.text(), QStringLiteral("abcd\nefgh\nijbckl\n  fg\n"));
}

TEST(VimTest, BlockVisualInsertAndAppend)
{
    {
        Vim v(QStringLiteral("abcd\nefgh\nijkl\n"));
        v.key(Qt::Key_V, Qt::ControlModifier);
        v.keys(QStringLiteral("jjIX"));
        v.esc();
        EXPECT_EQ(v.text(), QStringLiteral("Xabcd\nXefgh\nXijkl\n"));
        EXPECT_EQ(v.cursor(), 0);
        v.keys(QStringLiteral("u"));
        EXPECT_EQ(v.text(), QStringLiteral("abcd\nefgh\nijkl\n"));
    }
    {
        Vim v(QStringLiteral("abcd\nefgh\nijkl\n"), 1);
        v.key(Qt::Key_V, Qt::ControlModifier);
        v.keys(QStringLiteral("jjlA!"));
        v.esc();
        EXPECT_EQ(v.text(), QStringLiteral("abc!d\nefg!h\nijk!l\n"));
    }
    {
        Vim v(QStringLiteral("abcd\nx\nefgh\n"), 3);
        v.key(Qt::Key_V, Qt::ControlModifier);
        v.keys(QStringLiteral("jjA!")); // a short line is padded out to the block
        v.esc();
        EXPECT_EQ(v.text(), QStringLiteral("abcd!\nx   !\nefgh!\n"));
    }
    {
        Vim v(QStringLiteral("ab\nabcd\nx\n"));
        v.key(Qt::Key_V, Qt::ControlModifier);
        v.keys(QStringLiteral("jj$A;")); // $A: every line's own end
        v.esc();
        EXPECT_EQ(v.text(), QStringLiteral("ab;\nabcd;\nx;\n"));
    }
}

TEST(VimTest, BlockVisualChange)
{
    Vim v(QStringLiteral("abcd\nefgh\nijkl\n"), 1);
    v.key(Qt::Key_V, Qt::ControlModifier);
    v.keys(QStringLiteral("jlcZ"));
    v.esc();
    EXPECT_EQ(v.text(), QStringLiteral("aZd\neZh\nijkl\n"));
}

TEST(VimTest, BlockVisualIsPaintedAsStrips)
{
    // Same corners, same text: a block draws two short strips, a characterwise
    // selection draws the span between them, so the pictures differ.
    Vim block(QStringLiteral("abcd\nefgh\nijkl\n"), 1);
    block.key(Qt::Key_V, Qt::ControlModifier);
    block.keys(QStringLiteral("jl"));
    Vim span(QStringLiteral("abcd\nefgh\nijkl\n"), 1);
    span.keys(QStringLiteral("vjl"));
    EXPECT_NE(block.editor.viewport()->grab().toImage(), span.editor.viewport()->grab().toImage());
    block.esc();
    EXPECT_EQ(block.mode(), QStringLiteral("normal"));
}

TEST(VimTest, BlockVisualShift)
{
    Vim v(QStringLiteral("a\nb\nc\n"));
    v.key(Qt::Key_V, Qt::ControlModifier);
    v.keys(QStringLiteral("j>"));
    EXPECT_EQ(v.text(), QStringLiteral("    a\n    b\nc\n"));
}

TEST(VimTest, SubstituteOnALine)
{
    Vim v(QStringLiteral("foo foo\nfoo bar\nbaz\n"));
    v.keys(QStringLiteral(":s/foo/X/"));
    v.key(Qt::Key_Return);
    EXPECT_EQ(v.text(), QStringLiteral("X foo\nfoo bar\nbaz\n"));
    v.keys(QStringLiteral(":s/foo/Y/g"));
    v.key(Qt::Key_Return);
    EXPECT_EQ(v.text(), QStringLiteral("X Y\nfoo bar\nbaz\n"));
    v.keys(QStringLiteral("u"));
    EXPECT_EQ(v.text(), QStringLiteral("X foo\nfoo bar\nbaz\n"));
    v.keys(QStringLiteral(":s/nothing/Z/"));
    v.key(Qt::Key_Return);
    EXPECT_TRUE(v.editor.vimStatus().startsWith(QStringLiteral("E486")));
    EXPECT_EQ(v.text(), QStringLiteral("X foo\nfoo bar\nbaz\n"));
}

TEST(VimTest, SubstituteWithRanges)
{
    const auto run = [](const QString &text, const QString &keys) {
        Vim v(text);
        v.keys(keys);
        v.key(Qt::Key_Return);
        return v.text();
    };
    EXPECT_EQ(run(QStringLiteral("foo foo\nfoo bar\nbaz\n"), QStringLiteral(":%s/foo/[&]/g")),
              QStringLiteral("[foo] [foo]\n[foo] bar\nbaz\n"));
    EXPECT_EQ(run(QStringLiteral("a a\nb a\nc a\nd a\n"), QStringLiteral(":2,3s/a/A/")),
              QStringLiteral("a a\nb A\nc A\nd a\n"));
    EXPECT_EQ(run(QStringLiteral("a\na\na\na\n"), QStringLiteral(":2,$s/a/b/")),
              QStringLiteral("a\nb\nb\nb\n"));
    EXPECT_EQ(run(QStringLiteral("a\na\na\na\n"), QStringLiteral(":.,+1s/a/b/")),
              QStringLiteral("b\nb\na\na\n"));
    EXPECT_EQ(run(QStringLiteral("foo bar\n"), QStringLiteral(":s/(\\w+) (\\w+)/\\2 \\1/")),
              QStringLiteral("bar foo\n"));
    EXPECT_EQ(run(QStringLiteral("a/b\n"), QStringLiteral(":s#/#-#")), QStringLiteral("a-b\n"));
    EXPECT_EQ(run(QStringLiteral("a/b\n"), QStringLiteral(":s/\\//-/")), QStringLiteral("a-b\n"));
    EXPECT_EQ(run(QStringLiteral("Foo foo\n"), QStringLiteral(":s/foo/x/gi")), QStringLiteral("x x\n"));
    EXPECT_EQ(run(QStringLiteral("Foo foo\n"), QStringLiteral(":s/foo/x/g")), QStringLiteral("Foo x\n"));
    EXPECT_EQ(run(QStringLiteral("a\nb\n"), QStringLiteral(":%s/^/> /")), QStringLiteral("> a\n> b\n"));
    EXPECT_EQ(run(QStringLiteral("a,b\n"), QStringLiteral(":s/,/\\r/")), QStringLiteral("a\nb\n"));
    EXPECT_EQ(run(QStringLiteral("a\nb\n"), QStringLiteral(":$s/b/c/")), QStringLiteral("a\nc\n"));
}

TEST(VimTest, SubstituteOverTheVisualSelection)
{
    Vim v(QStringLiteral("foo\nboo\nzoo\n"));
    v.keys(QStringLiteral("Vj:"));
    EXPECT_EQ(v.editor.vimStatus(), QStringLiteral(":'<,'>"));
    v.keys(QStringLiteral("s/o/0/"));
    v.key(Qt::Key_Return);
    EXPECT_EQ(v.text(), QStringLiteral("f0o\nb0o\nzoo\n"));
    EXPECT_EQ(v.mode(), QStringLiteral("normal"));
}

TEST(VimTest, SubstituteWithMarkRanges)
{
    Vim v(QStringLiteral("a\nb\nc\nd\n"));
    v.keys(QStringLiteral("jmajmb"));
    v.keys(QStringLiteral(":'a,'bs/$/!/"));
    v.key(Qt::Key_Return);
    EXPECT_EQ(v.text(), QStringLiteral("a\nb!\nc!\nd\n"));
}

TEST(VimTest, ExLineJumpsWithAddresses)
{
    Vim v(QStringLiteral("a\nb\nc\nd\n"));
    v.keys(QStringLiteral(":$"));
    v.key(Qt::Key_Return);
    EXPECT_EQ(v.cursor(), 6);
    v.keys(QStringLiteral(":2"));
    v.key(Qt::Key_Return);
    EXPECT_EQ(v.cursor(), 2);
    v.keys(QStringLiteral(":.+1"));
    v.key(Qt::Key_Return);
    EXPECT_EQ(v.cursor(), 4);
}

TEST(VimTest, StarAndHashSearchTheWordUnderTheCaret)
{
    Vim v(QStringLiteral("foo bar foo baz foo\nfoobar\n"));
    v.keys(QStringLiteral("*"));
    EXPECT_EQ(v.cursor(), 8); // whole words only: foobar is not foo
    v.keys(QStringLiteral("*"));
    EXPECT_EQ(v.cursor(), 16);
    v.keys(QStringLiteral("*"));
    EXPECT_EQ(v.cursor(), 0); // wraps
    v.keys(QStringLiteral("n"));
    EXPECT_EQ(v.cursor(), 8);
    v.keys(QStringLiteral("#"));
    EXPECT_EQ(v.cursor(), 0);
    v.keys(QStringLiteral("l#")); // from inside a word, not that word again
    EXPECT_EQ(v.cursor(), 16);
    v.keys(QStringLiteral("0w*"));
    EXPECT_EQ(v.cursor(), 4); // bar occurs once: stays
}

TEST(VimTest, StarWithAnOperatorAndACount)
{
    Vim v(QStringLiteral("a x a x a\n"));
    v.keys(QStringLiteral("2*"));
    EXPECT_EQ(v.cursor(), 8);
    v.keys(QStringLiteral("0d*"));
    EXPECT_EQ(v.text(), QStringLiteral("a x a\n"));
}


TEST(VimTest, NumberedRegistersShiftOnDelete)
{
    {
        Vim v(QStringLiteral("a\nb\nc\nd\n"));
        v.keys(QStringLiteral("dddddd")); // "1 is the newest, "3 the oldest
        EXPECT_EQ(v.text(), QStringLiteral("d\n"));
        v.keys(QStringLiteral("\"1p\"2p\"3p"));
        EXPECT_EQ(v.text(), QStringLiteral("d\nc\nb\na\n"));
    }
    {
        Vim v(QStringLiteral("a\nb\nc\nd\ne\nf\ng\nh\ni\nj\nk\n"));
        v.keys(QStringLiteral("dddddddddddddddddddd")); // ten deletes: the first one falls off "9
        EXPECT_EQ(v.text(), QStringLiteral("k\n"));
        v.keys(QStringLiteral("\"9p\"1p"));
        EXPECT_EQ(v.text(), QStringLiteral("k\nb\nj\n"));
    }
    { // a small delete fills "- and leaves "1 alone
        Vim v(QStringLiteral("foo bar\nx\ny\n"));
        v.keys(QStringLiteral("dwjdd"));
        EXPECT_EQ(v.text(), QStringLiteral("bar\ny\n"));
        v.keys(QStringLiteral("\"-P\"1P"));
        EXPECT_EQ(v.text(), QStringLiteral("bar\nx\nfoo y\n"));
    }
    { // d% always fills "1, and "- too
        Vim v(QStringLiteral("(a) b\nz\n"));
        v.keys(QStringLiteral("d%"));
        EXPECT_EQ(v.text(), QStringLiteral(" b\nz\n"));
        v.keys(QStringLiteral("j\"1P"));
        EXPECT_EQ(v.text(), QStringLiteral(" b\n(a)z\n"));
    }
    { // a yank touches "0 only; a named delete of lines still shifts "1
        Vim v(QStringLiteral("a\nb\nc\n"));
        v.keys(QStringLiteral("yyjdd\"0p"));
        EXPECT_EQ(v.text(), QStringLiteral("a\nc\na\n"));
        v.keys(QStringLiteral("gg\"add\"2P")); // "1 now holds a, "2 the earlier b
        EXPECT_EQ(v.text(), QStringLiteral("b\nc\na\n"));
    }
}

TEST(VimTest, InnerAndAroundTagObjects)
{
    const auto run = [](const QString &text, qsizetype caret, const QString &keys) {
        Vim v(text, caret);
        v.keys(keys);
        return v.text();
    };
    const QString nested = QStringLiteral("<div><b>bold</b> text</div>");
    EXPECT_EQ(run(nested, 8, QStringLiteral("dit")), QStringLiteral("<div><b></b> text</div>"));
    EXPECT_EQ(run(nested, 8, QStringLiteral("dat")), QStringLiteral("<div> text</div>"));
    EXPECT_EQ(run(nested, 8, QStringLiteral("d2it")), QStringLiteral("<div></div>"));
    EXPECT_EQ(run(nested, 17, QStringLiteral("dit")), QStringLiteral("<div></div>")); // between the tags
    EXPECT_EQ(run(nested, 1, QStringLiteral("dit")), QStringLiteral("<div></div>")); // on the open tag
    EXPECT_EQ(run(QStringLiteral("<a><a>x</a>y</a>"), 6, QStringLiteral("dit")), QStringLiteral("<a><a></a>y</a>"));
    EXPECT_EQ(run(QStringLiteral("<a><a>x</a>y</a>"), 6, QStringLiteral("d2it")), QStringLiteral("<a></a>"));
    EXPECT_EQ(run(QStringLiteral("<p>one<br>two</p>"), 10, QStringLiteral("dit")), QStringLiteral("<p></p>"));
    EXPECT_EQ(run(QStringLiteral("<a title=\"a>b\">link</a>"), 16, QStringLiteral("dit")),
              QStringLiteral("<a title=\"a>b\"></a>"));
    EXPECT_EQ(run(QStringLiteral("1 < 2 <i>x</i>"), 10, QStringLiteral("dat")), QStringLiteral("1 < 2 "));
    EXPECT_EQ(run(QStringLiteral("<B>x</b>"), 3, QStringLiteral("dit")), QStringLiteral("<B></b>")); // names ignore case
    EXPECT_EQ(run(QStringLiteral("<b>x</b>"), 3, QStringLiteral("citY\x1b")), QStringLiteral("<b>Y</b>"));
    EXPECT_EQ(run(QStringLiteral("<b>x</b>"), 3, QStringLiteral("yitP")), QStringLiteral("<b>xx</b>"));
    EXPECT_EQ(run(QStringLiteral("no tags here"), 3, QStringLiteral("dit")), QStringLiteral("no tags here"));
    EXPECT_EQ(run(QStringLiteral("<b>x"), 3, QStringLiteral("dit")), QStringLiteral("<b>x")); // never closed
    // inside an HTML block of a note, over lines
    EXPECT_EQ(run(QStringLiteral("para\n\n<div>\n<span>hi</span>\n</div>\n"), 19, QStringLiteral("dit")),
              QStringLiteral("para\n\n<div>\n<span></span>\n</div>\n"));
    EXPECT_EQ(run(QStringLiteral("para\n\n<div>\n<span>hi</span>\n</div>\n"), 19, QStringLiteral("d2it")),
              QStringLiteral("para\n\n<div></div>\n"));
    { // visual
        Vim v(nested, 8);
        v.keys(QStringLiteral("vitd"));
        EXPECT_EQ(v.text(), QStringLiteral("<div><b></b> text</div>"));
        Vim w(nested, 8);
        w.keys(QStringLiteral("vatd"));
        EXPECT_EQ(w.text(), QStringLiteral("<div> text</div>"));
    }
}

TEST(VimTest, VisualJoinReplaceAndToggleCase)
{
    {
        Vim v(QStringLiteral("a\nb\nc\nd\n"));
        v.keys(QStringLiteral("VjJ"));
        EXPECT_EQ(v.text(), QStringLiteral("a b\nc\nd\n"));
        EXPECT_EQ(v.mode(), QStringLiteral("normal"));
        v.keys(QStringLiteral("vjJ")); // two lines of the rest
        EXPECT_EQ(v.text(), QStringLiteral("a b c\nd\n"));
        v.keys(QStringLiteral("u"));
        EXPECT_EQ(v.text(), QStringLiteral("a b\nc\nd\n"));
    }
    {
        Vim v(QStringLiteral("a\nb\nc\nd\n"), 2);
        v.keys(QStringLiteral("vjjJ")); // three lines, the selection's top first
        EXPECT_EQ(v.text(), QStringLiteral("a\nb c d\n"));
        Vim w(QStringLiteral("a\n  b\nc\n"));
        w.keys(QStringLiteral("VJ")); // one line: it and the next
        EXPECT_EQ(w.text(), QStringLiteral("a b\nc\n"));
    }
    {
        Vim v(QStringLiteral("abc\ndef\n"));
        v.keys(QStringLiteral("vjlrx")); // every selected character, not the line break
        EXPECT_EQ(v.text(), QStringLiteral("xxx\nxxf\n"));
        EXPECT_EQ(v.mode(), QStringLiteral("normal"));
        EXPECT_EQ(v.cursor(), 0);
        v.keys(QStringLiteral("u"));
        EXPECT_EQ(v.text(), QStringLiteral("abc\ndef\n"));
        v.keys(QStringLiteral("ggVr-"));
        EXPECT_EQ(v.text(), QStringLiteral("---\ndef\n"));
    }
    {
        Vim v(QStringLiteral("abcd\nefgh\nijkl\n"), 1);
        v.key(Qt::Key_V, Qt::ControlModifier);
        v.keys(QStringLiteral("jlr#"));
        EXPECT_EQ(v.text(), QStringLiteral("a##d\ne##h\nijkl\n"));
    }
    {
        Vim v(QStringLiteral("Hello World\n"));
        v.keys(QStringLiteral("veU"));
        EXPECT_EQ(v.text(), QStringLiteral("HELLO World\n"));
        v.keys(QStringLiteral("veu"));
        EXPECT_EQ(v.text(), QStringLiteral("hello World\n"));
        v.keys(QStringLiteral("V~"));
        EXPECT_EQ(v.text(), QStringLiteral("HELLO wORLD\n"));
        v.keys(QStringLiteral("u")); // normal mode: still undo
        EXPECT_EQ(v.text(), QStringLiteral("hello World\n"));
        v.keys(QStringLiteral("U")); // line-undo is not covered: a no-op, not a command
        EXPECT_EQ(v.text(), QStringLiteral("hello World\n"));
    }
}

TEST(VimTest, PercentWithACountGoesToThatPercentOfTheFile)
{
    QString ten;
    for (int i = 1; i <= 10; ++i)
        ten += QString::number(i) + QLatin1Char('\n');
    Vim v(ten);
    v.keys(QStringLiteral("50%"));
    EXPECT_EQ(v.cursor(), 8); // line 5
    v.keys(QStringLiteral("100%"));
    EXPECT_EQ(v.cursor(), 18); // line 10
    v.keys(QStringLiteral("1%"));
    EXPECT_EQ(v.cursor(), 0);
    v.keys(QStringLiteral("35%")); // rounds up: line 4
    EXPECT_EQ(v.cursor(), 6);
    v.keys(QStringLiteral("101%")); // out of range: stays
    EXPECT_EQ(v.cursor(), 6);
    v.keys(QStringLiteral("gg"));
    v.keys(QStringLiteral("d50%")); // linewise with an operator
    EXPECT_EQ(v.text(), QStringLiteral("6\n7\n8\n9\n10\n"));
    Vim b(QStringLiteral("(a)\n"));
    b.keys(QStringLiteral("%")); // without a count it still matches brackets
    EXPECT_EQ(b.cursor(), 2);
}

TEST(VimTest, AMacroStopsAtItsFirstFailedMotion)
{
    Vim v(QStringLiteral("1\n2\n3\n"));
    v.keys(QStringLiteral("qqA!"));
    v.esc();
    v.keys(QStringLiteral("jq"));
    v.keys(QStringLiteral("10@q")); // line 3 is the last: its j fails and ends the run
    EXPECT_EQ(v.text(), QStringLiteral("1!\n2!\n3!\n"));
    v.keys(QStringLiteral("gg@q")); // the flag does not outlive the run
    EXPECT_EQ(v.text(), QStringLiteral("1!!\n2!\n3!\n"));
    Vim w(QStringLiteral("a-b\n"));
    w.keys(QStringLiteral("qwf-x"));
    w.keys(QStringLiteral("q0@w")); // the second run finds no "-": it never reaches the x
    EXPECT_EQ(w.text(), QStringLiteral("ab\n"));
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
