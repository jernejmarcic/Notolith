// The size of a block view and who owns its rarely used fields (BlockExtras).
// A 1 MB note is 57 000 views; at 1.8 kB each that was 105 MB of a note nobody had scrolled.
#include <gtest/gtest.h>

#include <QApplication>
#include <QSignalSpy>
#include <QtTest/QTest>

#include <memory>

#include "MarkdownEditor.h"

using namespace noteAppC;

namespace {

// Tightened a slice at a time while the cold fields move off the view; the end is 400.
constexpr std::size_t kBlockViewBudget = 1992; // slice 1: 1984 plus the extras pointer

/// An editor whose parse has landed (a note over 128 kB parses on the pool and is a
/// placeholder until it comes back).
std::unique_ptr<MarkdownEditor> parsedEditorWith(const QString &text)
{
    auto editor = std::make_unique<MarkdownEditor>();
    editor->resize(900, 600);
    QSignalSpy parsed(editor.get(), &MarkdownEditor::wordCountChanged);
    editor->setNoteText(text);
    while (!parsed.isEmpty() && parsed.last().at(0).toInt() < 0) {
        if (!parsed.wait(10000))
            return nullptr;
    }
    return editor;
}

/// Mixed top-level blocks, over 128 kB so the parse is the asynchronous one.
QString bigMixedNote()
{
    QString note;
    for (int i = 0; note.size() < 200000; ++i) {
        note += QStringLiteral("## Heading %1\n\nA paragraph with *emphasis* and a [[link]] number %1.\n\n").arg(i);
        note += QStringLiteral("- item %1\n- item two\n\n```cpp\nint a%1 = 1;\n```\n\n").arg(i);
        note += QStringLiteral("| a | b |\n|---|---|\n| %1 | 2 |\n\n").arg(i);
    }
    return note;
}

} // namespace

TEST(BlockViewTest, ASmallViewIsHowAMegabyteNoteStaysSmall)
{
    EXPECT_LE(MarkdownEditor::blockViewSizeForTest(), kBlockViewBudget);
}

TEST(BlockViewTest, ANoteNothingHasLaidOutHoldsNoExtras)
{
    auto editor = parsedEditorWith(bigMixedNote());
    ASSERT_TRUE(editor);
    editor->show();
    (void)editor->grab(); // the top of the note is laid out, the rest is not
    EXPECT_EQ(editor->untouchedBlockExtrasCountForTest(), 0);
    // Only the views that were drawn can have any, and a screenful is a few dozen of 8 000.
    EXPECT_LT(editor->blockExtrasCountForTest(), 100);
}

TEST(BlockViewTest, AReparseKeepsUntouchedViewsBare)
{
    auto editor = parsedEditorWith(bigMixedNote());
    ASSERT_TRUE(editor);
    editor->show();
    (void)editor->grab();
    // One character at the end: the same blocks again, the old views kept.
    editor->setCursorOffset(editor->noteText().size());
    QTest::keyClicks(editor.get(), QStringLiteral("x"));
    QTest::qWait(300);
    EXPECT_EQ(editor->untouchedBlockExtrasCountForTest(), 0);
    EXPECT_LT(editor->blockExtrasCountForTest(), 100);
}

TEST(BlockViewTest, APlainParagraphNeverAllocatesExtras)
{
    auto editor = std::make_unique<MarkdownEditor>();
    editor->resize(900, 600);
    editor->setNoteText(QStringLiteral("Just text.\n\nMore *text* and a [[link]].\n"));
    editor->show();
    (void)editor->grab();
    EXPECT_EQ(editor->blockExtrasCountForTest(), 0);
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
