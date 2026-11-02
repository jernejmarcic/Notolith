// SPDX-License-Identifier: GPL-3.0-or-later
#include <gtest/gtest.h>

#include <QApplication>
#include <QtTest/QTest>

#include <memory>

#include "MarkdownEditor.h"

using namespace noteAppC;

namespace {

constexpr Qt::KeyboardModifier kCtrl = Qt::ControlModifier;
constexpr Qt::KeyboardModifiers kCtrlShift = Qt::ControlModifier | Qt::ShiftModifier;

/// An editor holding `text` with the caret at `caret` (the end by default).
std::unique_ptr<MarkdownEditor> editorWith(const QString &text, qsizetype caret = -1)
{
    auto editor = std::make_unique<MarkdownEditor>();
    editor->resize(900, 600);
    editor->setNoteText(text);
    editor->setCursorOffset(caret < 0 ? text.size() : caret);
    return editor;
}

/// The note after `key` with `modifiers` at `caret`; then Ctrl+Z must give the start back.
QString after(const QString &text, qsizetype caret, Qt::Key key, qsizetype *landed = nullptr)
{
    auto editor = editorWith(text, caret);
    QTest::keyClick(editor.get(), key, kCtrl);
    const QString result = editor->noteText();
    if (landed)
        *landed = editor->cursorOffset();
    if (result != text) {
        QTest::keyClick(editor.get(), Qt::Key_Z, kCtrl);
        EXPECT_EQ(editor->noteText(), text) << "one undo step takes the deletion back";
        EXPECT_EQ(editor->cursorOffset(), caret);
    }
    return result;
}

QString ctrlBackspace(const QString &text, qsizetype caret, qsizetype *landed = nullptr)
{
    return after(text, caret, Qt::Key_Backspace, landed);
}

QString ctrlDelete(const QString &text, qsizetype caret, qsizetype *landed = nullptr)
{
    return after(text, caret, Qt::Key_Delete, landed);
}

const QString kTable = QStringLiteral("above\n\n| red apple | qty |\n|---|---:|\n| pear | 3 |\n\nbelow\n");

} // namespace

TEST(WordKeysTest, CtrlBackspaceDeletesToTheStartOfTheWord)
{
    qsizetype caret = 0;
    EXPECT_EQ(ctrlBackspace(QStringLiteral("one two three"), 13, &caret), QStringLiteral("one two "));
    EXPECT_EQ(caret, 8);
    // mid-word: only what is before the caret
    EXPECT_EQ(ctrlBackspace(QStringLiteral("one two three"), 6, &caret), QStringLiteral("one o three"));
    EXPECT_EQ(caret, 4);
    // at a word's start: the word before it, with the space between
    EXPECT_EQ(ctrlBackspace(QStringLiteral("one two"), 4, &caret), QStringLiteral("two"));
    EXPECT_EQ(caret, 0);
}

TEST(WordKeysTest, CtrlBackspaceOverSpacesAndPunctuation)
{
    // one space is skipped and the word behind it goes; two or more are a group of their own
    EXPECT_EQ(ctrlBackspace(QStringLiteral("one two "), 8), QStringLiteral("one "));
    EXPECT_EQ(ctrlBackspace(QStringLiteral("one two   "), 10), QStringLiteral("one two"));
    EXPECT_EQ(ctrlDelete(QStringLiteral("one   two"), 3), QStringLiteral("onetwo"));
    // a run of punctuation is one step
    EXPECT_EQ(ctrlBackspace(QStringLiteral("wait..."), 7), QStringLiteral("wait"));
    EXPECT_EQ(ctrlBackspace(QStringLiteral("wait, "), 6), QStringLiteral("wait"));
    // CodeMirror's groups: an apostrophe or a decimal point is punctuation, not part of the word
    EXPECT_EQ(ctrlBackspace(QStringLiteral("say don't"), 9), QStringLiteral("say don'"));
    EXPECT_EQ(ctrlBackspace(QStringLiteral("pi 3.14"), 7), QStringLiteral("pi 3."));
    EXPECT_EQ(ctrlBackspace(QStringLiteral("see https://x.com"), 17), QStringLiteral("see https://x."));
    // an underscore is a word character
    EXPECT_EQ(ctrlBackspace(QStringLiteral("a foo_bar"), 9), QStringLiteral("a "));
    // indentation alone: back to the line start, not into the line above
    EXPECT_EQ(ctrlBackspace(QStringLiteral("a\n    "), 6), QStringLiteral("a\n"));
}

TEST(WordKeysTest, CtrlBackspaceAtALineStartJoinsTheLinesLikeBackspace)
{
    qsizetype caret = 0;
    EXPECT_EQ(ctrlBackspace(QStringLiteral("one\ntwo"), 4, &caret), QStringLiteral("onetwo"));
    EXPECT_EQ(caret, 3);
    EXPECT_EQ(ctrlBackspace(QStringLiteral("one"), 0), QStringLiteral("one"));
}

TEST(WordKeysTest, AccentedAndNonLatinWords)
{
    const QString slovene = QStringLiteral("Živjo čudoviti svet");
    EXPECT_EQ(ctrlBackspace(slovene, slovene.size()), QStringLiteral("Živjo čudoviti "));
    EXPECT_EQ(ctrlBackspace(slovene, 14), QStringLiteral("Živjo  svet"));
    // a combining mark never parts from its letter
    const QString combining = QStringLiteral("abc éte");
    EXPECT_EQ(ctrlBackspace(combining, combining.size()), QStringLiteral("abc "));
    // every CJK character is a word character: the run goes whole
    const QString cjk = QStringLiteral("日本語のテスト");
    EXPECT_EQ(ctrlBackspace(cjk, cjk.size()), QString());
    EXPECT_EQ(ctrlBackspace(QStringLiteral("abc 日本語"), 7), QStringLiteral("abc "));
}

TEST(WordKeysTest, EmojiAreOneClusterAndPunctuation)
{
    const QString family = QString::fromUcs4(U"a\U0001F468\u200D\U0001F469\u200D\U0001F467", 6);
    EXPECT_EQ(ctrlBackspace(family, family.size()), QStringLiteral("a"));
    const QString thumb = QStringLiteral("hi ") + QString::fromUcs4(U"\U0001F44D\U0001F3FD", 2);
    EXPECT_EQ(ctrlBackspace(thumb, thumb.size()), QStringLiteral("hi "));
    const QString flag = QString::fromUcs4(U"\U0001F1F8\U0001F1EE", 2);
    qsizetype landed = -1;
    after(flag, flag.size(), Qt::Key_Left, &landed);
    EXPECT_EQ(landed, 0) << "a flag is one step";
}

TEST(WordKeysTest, CtrlBackspaceWithASelectionDeletesJustTheSelection)
{
    auto editor = editorWith(QStringLiteral("one two three"), 13);
    editor->selectRange({4, 7});
    QTest::keyClick(editor.get(), Qt::Key_Backspace, kCtrl);
    EXPECT_EQ(editor->noteText(), QStringLiteral("one  three"));
    EXPECT_EQ(editor->cursorOffset(), 4);

    editor->selectRange({4, 5});
    QTest::keyClick(editor.get(), Qt::Key_Delete, kCtrl);
    EXPECT_EQ(editor->noteText(), QStringLiteral("one three"));
}

TEST(WordKeysTest, ConsecutiveCtrlBackspacesUndoTogetherLikeTyping)
{
    auto editor = editorWith(QStringLiteral("one two three"), 13);
    QTest::keyClick(editor.get(), Qt::Key_Backspace, kCtrl);
    QTest::keyClick(editor.get(), Qt::Key_Backspace, kCtrl);
    EXPECT_EQ(editor->noteText(), QStringLiteral("one "));
    QTest::keyClick(editor.get(), Qt::Key_Z, kCtrl);
    EXPECT_EQ(editor->noteText(), QStringLiteral("one two three"));
}

TEST(WordKeysTest, CtrlDeleteDeletesToTheEndOfTheWord)
{
    qsizetype caret = 0;
    EXPECT_EQ(ctrlDelete(QStringLiteral("one two three"), 0, &caret), QStringLiteral(" two three"));
    EXPECT_EQ(caret, 0);
    EXPECT_EQ(ctrlDelete(QStringLiteral("one two three"), 5), QStringLiteral("one t three"));
    // at a word's end: the space and the next word
    EXPECT_EQ(ctrlDelete(QStringLiteral("one two three"), 3), QStringLiteral("one three"));
    EXPECT_EQ(ctrlDelete(QStringLiteral("wait... ok"), 4), QStringLiteral("wait ok"));
    EXPECT_EQ(ctrlDelete(QStringLiteral("a...  b"), 1), QStringLiteral("a  b"));
    EXPECT_EQ(ctrlDelete(QStringLiteral("živjo čudoviti"), 5), QStringLiteral("živjo"));
    // spaces up to the line end go, the line break stays; at the end it is the break
    EXPECT_EQ(ctrlDelete(QStringLiteral("a  \nb"), 1), QStringLiteral("a\nb"));
    EXPECT_EQ(ctrlDelete(QStringLiteral("a\nb"), 1), QStringLiteral("ab"));
    EXPECT_EQ(ctrlDelete(QStringLiteral("a"), 1), QStringLiteral("a"));
}

TEST(WordKeysTest, CtrlLeftAndRightMoveByWord)
{
    const QString text = QStringLiteral("one, two  three");
    qsizetype landed = 0;
    after(text, 15, Qt::Key_Left, &landed);
    EXPECT_EQ(landed, 10);
    after(text, 10, Qt::Key_Left, &landed);
    EXPECT_EQ(landed, 5);
    after(text, 5, Qt::Key_Left, &landed);
    EXPECT_EQ(landed, 3) << "the comma is a stop of its own";
    after(text, 3, Qt::Key_Left, &landed);
    EXPECT_EQ(landed, 0);
    after(text, 0, Qt::Key_Left, &landed);
    EXPECT_EQ(landed, 0);

    after(text, 0, Qt::Key_Right, &landed);
    EXPECT_EQ(landed, 3);
    after(text, 3, Qt::Key_Right, &landed);
    EXPECT_EQ(landed, 4) << "the comma";
    after(text, 4, Qt::Key_Right, &landed);
    EXPECT_EQ(landed, 8);
    after(text, 8, Qt::Key_Right, &landed);
    EXPECT_EQ(landed, 15);
    after(text, 15, Qt::Key_Right, &landed);
    EXPECT_EQ(landed, 15);
    // a line break is space: the move goes on into the next line's word, as in CodeMirror
    after(QStringLiteral("ab\ncd"), 2, Qt::Key_Right, &landed);
    EXPECT_EQ(landed, 5);
    after(QStringLiteral("ab\ncd"), 3, Qt::Key_Left, &landed);
    EXPECT_EQ(landed, 0);
    after(QStringLiteral("ab\n\n  cd"), 2, Qt::Key_Right, &landed);
    EXPECT_EQ(landed, 8);
    after(QStringLiteral("ab\n"), 3, Qt::Key_Left, &landed);
    EXPECT_EQ(landed, 0);
}

TEST(WordKeysTest, CtrlShiftLeftAndRightExtendTheSelection)
{
    auto editor = editorWith(QStringLiteral("one two three"), 4);
    QTest::keyClick(editor.get(), Qt::Key_Right, kCtrlShift);
    EXPECT_EQ(editor->selectedText(), QStringLiteral("two"));
    QTest::keyClick(editor.get(), Qt::Key_Right, kCtrlShift);
    EXPECT_EQ(editor->selectedText(), QStringLiteral("two three"));
    QTest::keyClick(editor.get(), Qt::Key_Left, kCtrlShift);
    EXPECT_EQ(editor->selectedText(), QStringLiteral("two "));
    QTest::keyClick(editor.get(), Qt::Key_Left, kCtrlShift);
    EXPECT_EQ(editor->selectedText(), QString());
    QTest::keyClick(editor.get(), Qt::Key_Left, kCtrlShift);
    EXPECT_EQ(editor->selectedText(), QStringLiteral("one "));
    EXPECT_EQ(editor->noteText(), QStringLiteral("one two three")) << "moving never edits";
}

TEST(WordKeysTest, CtrlArrowsWithASelectionAndNoShiftCollapseToItsEdge)
{
    auto editor = editorWith(QStringLiteral("one two three"), 13);
    editor->selectRange({4, 7});
    QTest::keyClick(editor.get(), Qt::Key_Left, kCtrl);
    EXPECT_EQ(editor->cursorOffset(), 4);
    EXPECT_EQ(editor->selectedText(), QString());
    editor->selectRange({4, 7});
    QTest::keyClick(editor.get(), Qt::Key_Right, kCtrl);
    EXPECT_EQ(editor->cursorOffset(), 7);
    EXPECT_EQ(editor->selectedText(), QString());
}

TEST(WordKeysTest, AnEntityDrawnAsOneCharacterGoesWholeOrNotAtAll)
{
    const QString text = QStringLiteral("a &amp; b");
    auto editor = editorWith(text, 8);
    editor->show();
    QTest::keyClick(editor.get(), Qt::Key_Left, kCtrl);
    EXPECT_EQ(editor->cursorOffset(), 2) << "never between the halves of what is drawn as one";
    editor->setCursorOffset(8);
    QTest::keyClick(editor.get(), Qt::Key_Backspace, kCtrl);
    EXPECT_EQ(editor->noteText(), QStringLiteral("a b"));
}

TEST(WordKeysTest, ABackspaceAfterAListMarkerTakesTheMarkerOffLikeBackspace)
{
    qsizetype caret = 0;
    EXPECT_EQ(ctrlBackspace(QStringLiteral("- foo"), 2, &caret), QStringLiteral("foo"));
    EXPECT_EQ(caret, 0);
    EXPECT_EQ(ctrlBackspace(QStringLiteral("- "), 2), QString());
    // further into the item it is a word like any other
    EXPECT_EQ(ctrlBackspace(QStringLiteral("- foo bar"), 9), QStringLiteral("- foo "));
    EXPECT_EQ(ctrlBackspace(QStringLiteral("- foo"), 5), QStringLiteral("- "));
}

TEST(WordKeysTest, AutoPairsGoTogether)
{
    EXPECT_EQ(ctrlBackspace(QStringLiteral("foo ()"), 5), QStringLiteral("foo "));
    // not between a pair: the word before the caret
    EXPECT_EQ(ctrlBackspace(QStringLiteral("(foo)"), 4), QStringLiteral("()"));
}

TEST(WordKeysTest, ReadingModeEditsNothing)
{
    auto editor = editorWith(QStringLiteral("one two"), 7);
    editor->setMode(MarkdownEditor::Mode::Reading);
    QTest::keyClick(editor.get(), Qt::Key_Backspace, kCtrl);
    QTest::keyClick(editor.get(), Qt::Key_Delete, kCtrl);
    EXPECT_EQ(editor->noteText(), QStringLiteral("one two"));
}

TEST(WordKeysTest, LivePreviewStopsAtTheMarkersLikeAnyPunctuation)
{
    // The markers are punctuation: a stop of their own, at offsets a plain
    // arrow reaches too (moveCursor reveals the syntax the caret touches).
    const QString text = QStringLiteral("one **two** three");
    auto editor = editorWith(text, text.size());
    ASSERT_EQ(editor->mode(), MarkdownEditor::Mode::LivePreview);
    for (const qsizetype expected : {12, 9, 6, 4, 0}) {
        QTest::keyClick(editor.get(), Qt::Key_Left, kCtrl);
        EXPECT_EQ(editor->cursorOffset(), expected);
    }
    EXPECT_EQ(ctrlBackspace(text, 9), QStringLiteral("one **** three"));
    EXPECT_EQ(editor->noteText(), text);
}

// Raw CodeMirror 6 behaviour, kept on purpose: the markers are Other characters,
// so a word step from outside the bold span takes the closing "**" and the
// space with it (Ctrl+Delete from before it takes just the markers). One Ctrl+Z gives them back (after() checks).
TEST(WordKeysTest, AWordStepMayTakeAMarkerThatIsHiddenAtTheCaret)
{
    const QString text = QStringLiteral("one **two** three");
    EXPECT_EQ(ctrlBackspace(text, 12), QStringLiteral("one **twothree"));
    EXPECT_EQ(ctrlDelete(text, 9), QStringLiteral("one **two three"));
}

TEST(WordKeysTest, SourceModeWorksToo)
{
    auto editor = editorWith(QStringLiteral("one **two** three"), 17);
    editor->setMode(MarkdownEditor::Mode::Source);
    QTest::keyClick(editor.get(), Qt::Key_Backspace, kCtrl);
    EXPECT_EQ(editor->noteText(), QStringLiteral("one **two** "));
}

TEST(WordKeysTest, VimInsertModeDeletesWordsAndNormalModeLeavesTheKeyAlone)
{
    auto editor = editorWith(QStringLiteral("one two"), 6);
    editor->setVimMode(true);
    QTest::keyClick(editor.get(), Qt::Key_Left, kCtrl); // normal mode: the plain step, as before
    EXPECT_EQ(editor->cursorOffset(), 5);
    // Not a vim binding: nothing is deleted, in normal or in visual mode
    QTest::keyClick(editor.get(), Qt::Key_Backspace, kCtrl);
    QTest::keyClick(editor.get(), Qt::Key_Delete, kCtrl);
    EXPECT_EQ(editor->noteText(), QStringLiteral("one two"));
    QTest::keyClick(editor.get(), 'v');
    QTest::keyClick(editor.get(), 'l');
    QTest::keyClick(editor.get(), Qt::Key_Backspace, kCtrl);
    EXPECT_EQ(editor->noteText(), QStringLiteral("one two"));
    QTest::keyClick(editor.get(), Qt::Key_Escape);
    QTest::keyClick(editor.get(), 'A'); // append at the line's end: insert mode
    QTest::keyClick(editor.get(), Qt::Key_Backspace, kCtrl);
    EXPECT_EQ(editor->noteText(), QStringLiteral("one "));
    QTest::keyClick(editor.get(), Qt::Key_Escape);
}

TEST(WordKeysTest, InADrawnTableACellKeepsItsPipes)
{
    auto editor = editorWith(kTable, 0);
    editor->show();
    const qsizetype red = kTable.indexOf(QStringLiteral("red apple"));
    const qsizetype apple = red + 4;
    editor->setCursorOffset(apple + 5);
    QTest::keyClick(editor.get(), Qt::Key_Backspace, kCtrl);
    EXPECT_EQ(editor->noteText(), QString(kTable).remove(apple, 5));
    QTest::keyClick(editor.get(), Qt::Key_Backspace, kCtrl);
    EXPECT_EQ(editor->noteText(), QString(kTable).remove(red, 9)) << "up to the cell's start, no further";
    const QString emptied = editor->noteText();
    QTest::keyClick(editor.get(), Qt::Key_Backspace, kCtrl); // at the cell's start: swallowed
    EXPECT_EQ(editor->noteText(), emptied);

    // forward, the same at the cell's end
    editor->setNoteText(kTable);
    editor->setCursorOffset(red + 4);
    QTest::keyClick(editor.get(), Qt::Key_Delete, kCtrl);
    EXPECT_EQ(editor->noteText(), QString(kTable).remove(red + 4, 5));
    editor->setNoteText(kTable);
    editor->setCursorOffset(red);
    QTest::keyClick(editor.get(), Qt::Key_Delete, kCtrl);
    QTest::keyClick(editor.get(), Qt::Key_Delete, kCtrl);
    EXPECT_EQ(editor->noteText(), QString(kTable).remove(red, 9));
    const QString cleared = editor->noteText();
    QTest::keyClick(editor.get(), Qt::Key_Delete, kCtrl);
    EXPECT_EQ(editor->noteText(), cleared);
}

TEST(WordKeysTest, InADrawnTableCtrlArrowsStayInTheCellThenCross)
{
    auto editor = editorWith(kTable, 0);
    editor->show();
    const qsizetype red = kTable.indexOf(QStringLiteral("red apple"));
    const qsizetype qty = kTable.indexOf(QStringLiteral("qty"));
    editor->setCursorOffset(red);
    QTest::keyClick(editor.get(), Qt::Key_Right, kCtrl);
    EXPECT_EQ(editor->cursorOffset(), red + 3);
    QTest::keyClick(editor.get(), Qt::Key_Right, kCtrl);
    EXPECT_EQ(editor->cursorOffset(), red + 9) << "the cell's end, not past its pipe";
    QTest::keyClick(editor.get(), Qt::Key_Right, kCtrl); // out of the cell as Right does
    EXPECT_EQ(editor->cursorOffset(), qty);
    QTest::keyClick(editor.get(), Qt::Key_Left, kCtrl);
    EXPECT_EQ(editor->cursorOffset(), red + 9);
    QTest::keyClick(editor.get(), Qt::Key_Left, kCtrl);
    EXPECT_EQ(editor->cursorOffset(), red + 4);
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
