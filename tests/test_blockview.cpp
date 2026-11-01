// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej Marčič
// The size of a block view and who owns its rarely used fields (BlockExtras).
// A 1 MB note is 57 000 views; at 1.8 kB each that was 105 MB of a note nobody had scrolled.
#include <gtest/gtest.h>

#include <QApplication>
#include <QImage>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest/QTest>

#include <memory>

#include "MarkdownEditor.h"

using namespace noteAppC;

namespace {

// 1984 before the cold fields moved into BlockExtras. Deliberately kept inline, because every layout writes them
// and a write through extrasMut() would allocate for every laid-out block: revealed, caretWord, codeEnd,
// listTextX/listParent, the kind flags (isQuery, isBase, isHtml, isPluginBlock, notebookEligible).
constexpr std::size_t kBlockViewBudget = 400;

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
    EXPECT_GT(editor->blockCountForTest(), 1000);
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
    const int drawn = editor->blockExtrasCountForTest();
    ASSERT_GE(drawn, 1) << "the table on the first screen was laid out";
    // One character at the end: the same blocks again, so the old untouched views are kept.
    const int parses = editor->parseCountForTest();
    editor->setCursorOffset(editor->noteText().size());
    QTest::keyClicks(editor.get(), QStringLiteral("x"));
    ASSERT_TRUE(QTest::qWaitFor([&] { return editor->parseCountForTest() > parses; }, 5000)) << "no reparse ran";
    QTest::qWait(300); // a note this size parses on the pool: let it land
    (void)editor->grab();
    EXPECT_EQ(editor->untouchedBlockExtrasCountForTest(), 0);
    EXPECT_GE(editor->blockExtrasCountForTest(), 1) << "the screen was drawn again";
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

TEST(BlockViewTest, AnImageEmbedOwnsExtrasAndItsNeighboursDoNot)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    QImage image(20, 20, QImage::Format_RGB32);
    image.fill(Qt::red);
    const QString path = dir.filePath(QStringLiteral("a.png"));
    ASSERT_TRUE(image.save(path));
    auto editor = std::make_unique<MarkdownEditor>();
    editor->resize(900, 600);
    editor->setLinkResolver([&](const QString &target) { return target == QLatin1String("a.png") ? path : QString(); });
    editor->setNoteText(QStringLiteral("before\n\n![[a.png]]\n\nafter\n"));
    editor->show();
    (void)editor->grab();
    EXPECT_EQ(editor->blockExtrasCountForTest(), 1);
    // The picture is still there for the paint step, not just allocated.
    EXPECT_EQ(editor->embedCellCountAt(0), 0);
    EXPECT_GT(editor->documentHeight(), 100);
}

TEST(BlockViewTest, ATableOwnsExtrasAndTheParagraphsAroundItDoNot)
{
    auto editor = std::make_unique<MarkdownEditor>();
    editor->resize(900, 600);
    editor->setNoteText(QStringLiteral("before\n\n| a | b |\n|---|---|\n| 1 | 2 |\n\nafter\n"));
    editor->show();
    (void)editor->grab();
    EXPECT_EQ(editor->blockExtrasCountForTest(), 1);
    const auto rows = editor->tableCellTextsAt(editor->noteText().indexOf(QLatin1Char('|')));
    ASSERT_EQ(rows.size(), 2);
    EXPECT_EQ(rows.at(1), (QStringList{QStringLiteral("1"), QStringLiteral("2")}));
}

TEST(BlockViewTest, ACalloutOwnsExtrasAndAParagraphDoesNot)
{
    auto editor = std::make_unique<MarkdownEditor>();
    editor->resize(900, 600);
    editor->setNoteText(QStringLiteral("before\n\n> [!note] Title\n> body\n\nafter\n"));
    editor->show();
    (void)editor->grab();
    EXPECT_EQ(editor->blockExtrasCountForTest(), 1);
}

TEST(BlockViewTest, ACodeBlockAndAListWithoutARunnerHoldNoExtras)
{
    auto editor = std::make_unique<MarkdownEditor>();
    editor->resize(900, 600);
    editor->setNoteText(QStringLiteral("```cpp\nint a = 1;\n```\n\n- one\n- two\n  - nested\n\n# Heading\n"));
    editor->show();
    (void)editor->grab();
    EXPECT_EQ(editor->blockExtrasCountForTest(), 0);
}

TEST(BlockViewTest, AFootnoteReferenceOwnsExtrasAndPlainTextDoesNot)
{
    auto editor = std::make_unique<MarkdownEditor>();
    editor->resize(900, 600);
    editor->setNoteText(QStringLiteral("plain one\n\nSee this[^1] note.\n\nplain two\n\n[^1]: the footnote\n"));
    editor->setCursorOffset(0);
    editor->show();
    (void)editor->grab();
    EXPECT_GE(editor->blockExtrasCountForTest(), 1);
    EXPECT_LE(editor->blockExtrasCountForTest(), 2); // the reference's block and the definition's
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
