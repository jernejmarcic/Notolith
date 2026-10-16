#include <gtest/gtest.h>

#include <QApplication>
#include <QElapsedTimer>
#include <QtTest/QTest>

#include <memory>

#include "MarkdownEditor.h"

using namespace noteAppC;

namespace {

/// An editor holding `text` with the caret at `caret` (the end by default).
std::unique_ptr<MarkdownEditor> editorWith(const QString &text, qsizetype caret = -1,
                                           MarkdownEditor::Mode mode = MarkdownEditor::Mode::Source)
{
    auto editor = std::make_unique<MarkdownEditor>();
    editor->resize(900, 600);
    editor->setNoteText(text);
    editor->setMode(mode);
    editor->setCursorOffset(caret < 0 ? text.size() : caret);
    return editor;
}

/// The offset just after `needle`, for a caret placed by reading, not counting.
qsizetype after(const QString &text, const QString &needle)
{
    const qsizetype at = text.indexOf(needle);
    return at < 0 ? -1 : at + needle.size();
}

void undo(MarkdownEditor &editor)
{
    QTest::keyClick(&editor, Qt::Key_Z, Qt::ControlModifier);
}

const QString kHeader = QStringLiteral("para\n\n| a | b |\n");
const QString kGenerated = QStringLiteral("para\n\n| a   | b   |\n| --- | --- |\n|     |     |\n");

} // namespace

TEST(TableAutoTest, HeaderRowEnterWritesSeparatorAndRow)
{
    // Both modes: live preview shows the paragraph as text while the caret is in it.
    for (const auto mode : {MarkdownEditor::Mode::Source, MarkdownEditor::Mode::LivePreview}) {
        auto editor = editorWith(kHeader, after(kHeader, QStringLiteral("| b |")), mode);
        QTest::keyClick(editor.get(), Qt::Key_Return);
        EXPECT_EQ(editor->noteText(), kGenerated) << editor->noteText().toStdString();
        // The caret is in the first cell of the new row, so typing goes there.
        EXPECT_EQ(editor->cursorOffset(), kGenerated.indexOf(QStringLiteral("|     |     |")) + 2);
        QTest::keyClicks(editor.get(), QStringLiteral("x"));
        EXPECT_TRUE(editor->noteText().contains(QStringLiteral("| x    |     |")));

        undo(*editor);
        undo(*editor);
        EXPECT_EQ(editor->noteText(), kHeader) << "one step for the whole table, one for the x";
    }
}

TEST(TableAutoTest, AQuotedHeaderKeepsItsMarker)
{
    const QString note = QStringLiteral("> | name | qty |\n");
    auto editor = editorWith(note, note.size() - 1);
    QTest::keyClick(editor.get(), Qt::Key_Return);
    EXPECT_EQ(editor->noteText(), QStringLiteral("> | name | qty |\n> | ---- | --- |\n> |      |     |\n"));
}

TEST(TableAutoTest, AnEscapedPipeIsNotACellBorder)
{
    // Two cells, not three: `a\|b` is one. (A pipe in a code span does split a
    // cell, in GFM and in tableRowCells alike, so the count follows the parser.)
    const QString note = QStringLiteral("| a\\|b | c |\n");
    auto editor = editorWith(note, note.size() - 1);
    QTest::keyClick(editor.get(), Qt::Key_Return);
    const QStringList lines = editor->noteText().split(QLatin1Char('\n'));
    ASSERT_GE(lines.size(), 3);
    EXPECT_EQ(lines.at(1), QStringLiteral("| ---- | --- |")) << editor->noteText().toStdString();
}

TEST(TableAutoTest, EnterOnABodyRowDoesNotInjectASeparator)
{
    // Below an existing table's rows, with no delimiter row of its own in between...
    const QString note = QStringLiteral("| a | b |\n| --- | --- |\n| 1 | 2 |\n");
    auto editor = editorWith(note, after(note, QStringLiteral("| 1 | 2 |")), MarkdownEditor::Mode::Source);
    editor->setAlignTables(false); // so the source-mode key does not take it either
    QTest::keyClick(editor.get(), Qt::Key_Return);
    EXPECT_EQ(editor->noteText(), note + QStringLiteral("\n"));

    // ...and a header row stacked under another pipe line.
    const QString stacked = QStringLiteral("| a | b |\n| c | d |\n");
    auto second = editorWith(stacked, after(stacked, QStringLiteral("| c | d |")));
    QTest::keyClick(second.get(), Qt::Key_Return);
    EXPECT_EQ(second->noteText(), stacked + QStringLiteral("\n"));

    // A header already followed by its delimiter row.
    const QString existing = QStringLiteral("| a | b |\n| --- | --- |\n");
    auto third = editorWith(existing, after(existing, QStringLiteral("| a | b |")));
    QTest::keyClick(third.get(), Qt::Key_Return);
    // Source mode walks to the row below instead (one is added); no second delimiter row.
    EXPECT_EQ(third->noteText().count(QStringLiteral("---")), 2) << third->noteText().toStdString();
}

TEST(TableAutoTest, NoAutogenInsideACodeFence)
{
    const QString note = QStringLiteral("```\n| a | b |\n```\n");
    auto editor = editorWith(note, after(note, QStringLiteral("| a | b |")));
    QTest::keyClick(editor.get(), Qt::Key_Return);
    EXPECT_EQ(editor->noteText(), QStringLiteral("```\n| a | b |\n\n```\n"));
}

TEST(TableAutoTest, OffMeansEnterIsANewline)
{
    auto editor = editorWith(kHeader, after(kHeader, QStringLiteral("| b |")));
    editor->setAlignTables(false);
    QTest::keyClick(editor.get(), Qt::Key_Return);
    EXPECT_EQ(editor->noteText(), kHeader + QStringLiteral("\n"));

    // Tab in a source-mode table is a tab too, not an alignment.
    const QString note = QStringLiteral("|a|b|\n|-|-|\n|1|2|\n");
    auto tabbed = editorWith(note, after(note, QStringLiteral("|a")));
    tabbed->setAlignTables(false);
    QTest::keyClick(tabbed.get(), Qt::Key_Tab);
    EXPECT_NE(tabbed->noteText(), note);
    EXPECT_FALSE(tabbed->noteText().contains(QStringLiteral("| --- |")));
}

TEST(TableAutoTest, InsertTableCommandWritesObsidianTemplate)
{
    const QString table = QStringLiteral("|     |     |\n| --- | --- |\n|     |     |");
    auto blank = editorWith(QStringLiteral("para\n\n\nafter\n"), 6);
    ASSERT_TRUE(blank->runCommand(QStringLiteral("noteAppC:insert-table")));
    EXPECT_EQ(blank->noteText(), QStringLiteral("para\n\n") + table + QStringLiteral("\n\nafter\n"))
        << blank->noteText().toStdString();
    EXPECT_EQ(blank->cursorOffset(), 8); // in the first cell
    undo(*blank);
    EXPECT_EQ(blank->noteText(), QStringLiteral("para\n\n\nafter\n"));

    // A line with text keeps it; the table is a paragraph of its own below.
    auto text = editorWith(QStringLiteral("some words\n"), 4);
    ASSERT_TRUE(text->runCommand(QStringLiteral("noteAppC:insert-table")));
    EXPECT_EQ(text->noteText(), QStringLiteral("some words\n\n") + table + QStringLiteral("\n"))
        << text->noteText().toStdString();
    EXPECT_EQ(text->cursorOffset(), 14);
    QTest::keyClicks(text.get(), QStringLiteral("q"));
    EXPECT_TRUE(text->noteText().contains(QStringLiteral("| q")));
}

TEST(TableAutoTest, SlashTableInsertsIt)
{
    auto editor = editorWith(QStringLiteral("\n"), 0, MarkdownEditor::Mode::LivePreview);
    QList<MarkdownEditor::SlashCommand> commands;
    for (const MarkdownEditor::Command &command : MarkdownEditor::commands())
        commands.append({command.id, command.name, QString()});
    editor->setSlashCommands([commands] { return commands; },
                             [e = editor.get()](const QString &id) { e->runCommand(id); });
    QTest::keyClicks(editor.get(), QStringLiteral("/insert table"));
    ASSERT_TRUE(editor->slashPopupVisible());
    ASSERT_FALSE(editor->slashSuggestions().isEmpty());
    EXPECT_EQ(editor->slashSuggestions().constFirst(), QStringLiteral("Insert table")) << editor->slashSuggestions().join(QStringLiteral(" / ")).toStdString();
    QTest::keyClick(editor.get(), Qt::Key_Return);
    EXPECT_TRUE(editor->noteText().startsWith(QStringLiteral("|     |     |\n| --- | --- |\n")))
        << editor->noteText().toStdString();
}

TEST(TableAutoTest, SlashTableOffersInsertTableFirst)
{
    auto editor = editorWith(QStringLiteral("\n"), 0, MarkdownEditor::Mode::LivePreview);
    QList<MarkdownEditor::SlashCommand> commands;
    for (const MarkdownEditor::Command &command : MarkdownEditor::commands())
        commands.append({command.id, command.name, QString()});
    editor->setSlashCommands([commands] { return commands; },
                             [e = editor.get()](const QString &id) { e->runCommand(id); });
    QTest::keyClicks(editor.get(), QStringLiteral("/table"));
    ASSERT_TRUE(editor->slashPopupVisible());
    const QStringList names = editor->slashSuggestions();
    ASSERT_GT(names.size(), 3);
    EXPECT_EQ(names.constFirst(), QStringLiteral("Insert table")) << names.join(QStringLiteral(" / ")).toStdString();
    EXPECT_TRUE(names.at(1).contains(QStringLiteral("table"), Qt::CaseInsensitive)) << "the other table commands follow";
}

TEST(TableAutoTest, TabInSourceModeRealignsOnlyThatTable)
{
    const QString second = QStringLiteral("|x|y|\n|-|-|\n|1|2|\n");
    const QString note = QStringLiteral("|a|bbbb|\n|-|:-:|\n|1|2|\n\n") + second;
    auto editor = editorWith(note, after(note, QStringLiteral("|a")));
    QTest::keyClick(editor.get(), Qt::Key_Tab);
    // Cells padded, centred and left alike; the other table untouched.
    EXPECT_TRUE(editor->noteText().endsWith(second)) << editor->noteText().toStdString();
    EXPECT_TRUE(editor->noteText().startsWith(QStringLiteral("| a   |"))) << editor->noteText().toStdString();
    // The caret went to the next cell, so typing lands in "bbbb".
    QTest::keyClicks(editor.get(), QStringLiteral("!"));
    EXPECT_TRUE(editor->noteText().contains(QStringLiteral("!bbbb"))) << editor->noteText().toStdString();

    undo(*editor);
    undo(*editor);
    EXPECT_EQ(editor->noteText(), note) << "the alignment is one step, the typing another";
}

TEST(TableAutoTest, TabAndEnterWalkASourceTableAndAppendRows)
{
    const QString note = QStringLiteral("| a | b |\n| --- | --- |\n| 1 | 2 |\n");
    auto editor = editorWith(note, after(note, QStringLiteral("| 1")));
    QTest::keyClick(editor.get(), Qt::Key_Tab); // the last column: past it, a new row
    QTest::keyClick(editor.get(), Qt::Key_Tab);
    QTest::keyClicks(editor.get(), QStringLiteral("z"));
    QString text = editor->noteText();
    EXPECT_EQ(text, QStringLiteral("| a   | b   |\n| --- | --- |\n| 1   | 2   |\n| z    |     |\n")) << text.toStdString();

    QTest::keyClick(editor.get(), Qt::Key_Backtab);
    EXPECT_EQ(editor->cursorOffset(), editor->noteText().indexOf(QStringLiteral("| 2   |")) + 2);

    QTest::keyClick(editor.get(), Qt::Key_Return); // the cell below
    QTest::keyClicks(editor.get(), QStringLiteral("w"));
    EXPECT_TRUE(editor->noteText().contains(QStringLiteral("| z   | w")));
    QTest::keyClick(editor.get(), Qt::Key_Return); // below the last row: another
    EXPECT_EQ(editor->noteText().count(QLatin1Char('\n')), 5) << editor->noteText().toStdString();
}

TEST(TableAutoTest, KeysInsideTheParseDebounceDoWhatTheyDoAfterIt)
{
    // Inside the debounce a source table is read from its shifted cells and
    // its own lines, not reparsed; the keys must land exactly where they do
    // once the parse has caught up. The `|` typed into the header last makes
    // it no table at all, which only the parser can say.
    const QString note = QStringLiteral("para\n\n| a | b |\n| --- | --- |\n| 1 | 2 |\n\nafter\n");
    const auto play = [&](bool waitForParse) {
        auto editor = editorWith(note, after(note, QStringLiteral("| 1")));
        const auto step = [&](auto &&key) {
            key(editor.get());
            if (waitForParse)
                QTest::qWait(60);
        };
        step([](QWidget *w) { QTest::keyClicks(w, QStringLiteral("23")); });
        step([](QWidget *w) { QTest::keyClick(w, Qt::Key_Tab); });
        step([](QWidget *w) { QTest::keyClicks(w, QStringLiteral("x")); });
        step([](QWidget *w) { QTest::keyClick(w, Qt::Key_Tab); }); // past the last cell: a new row
        step([](QWidget *w) { QTest::keyClicks(w, QStringLiteral("q")); });
        step([](QWidget *w) { QTest::keyClick(w, Qt::Key_Backtab); });
        step([](QWidget *w) { QTest::keyClick(w, Qt::Key_Return); });
        step([](QWidget *w) { QTest::keyClicks(w, QStringLiteral("w")); });
        editor->setCursorOffset(after(editor->noteText(), QStringLiteral("| a")));
        step([](QWidget *w) { QTest::keyClicks(w, QStringLiteral("|")); });
        step([](QWidget *w) { QTest::keyClick(w, Qt::Key_Tab); });
        return std::make_pair(editor->noteText(), editor->cursorOffset());
    };
    const auto settled = play(true);
    const auto hurried = play(false);
    EXPECT_EQ(hurried.first, settled.first) << hurried.first.toStdString();
    EXPECT_EQ(hurried.second, settled.second);
    EXPECT_TRUE(settled.first.contains(QStringLiteral("| 123 | x2  |"))) << settled.first.toStdString();
}

TEST(TableAutoTest, AlignmentMenuRewritesDelimiter)
{
    const QString note = QStringLiteral("| a | b |\n| --- | --- |\n| 1 | 2 |\n");
    for (const auto &[id, delimiter] : {std::pair{"left", ":--"}, {"center", ":-:"}, {"right", "--:"}}) {
        auto editor = editorWith(note, after(note, QStringLiteral("| b")), MarkdownEditor::Mode::LivePreview);
        ASSERT_TRUE(editor->runCommand(QStringLiteral("noteAppC:table-align-column-") + QString::fromLatin1(id)));
        const QStringList lines = editor->noteText().split(QLatin1Char('\n'));
        ASSERT_GE(lines.size(), 3);
        EXPECT_TRUE(lines.at(1).startsWith(QStringLiteral("| --- | "))) << "only the caret's column: " << lines.at(1).toStdString();
        EXPECT_TRUE(lines.at(1).contains(QString::fromLatin1(delimiter))) << lines.at(1).toStdString();
        undo(*editor);
        EXPECT_EQ(editor->noteText(), note);
    }
    // The exact centre: ":---:" once the column is 5 wide.
    const QString wide = QStringLiteral("| a | bbbbb |\n| --- | --- |\n| 1 | 2 |\n");
    auto editor = editorWith(wide, after(wide, QStringLiteral("| bb")), MarkdownEditor::Mode::LivePreview);
    ASSERT_TRUE(editor->runCommand(QStringLiteral("noteAppC:table-align-column-center")));
    EXPECT_TRUE(editor->noteText().contains(QStringLiteral(":---:"))) << editor->noteText().toStdString();
}

TEST(TableAutoTest, SortByColumnKeepsHeader)
{
    const QString note = QStringLiteral("| name | n |\n| --- | --- |\n| pear | 10 |\n| Apple | 2 |\n| fig | 1 |\n");
    const auto run = [&](const char *id) {
        auto editor = editorWith(note, after(note, QStringLiteral("| pear | 1")), MarkdownEditor::Mode::LivePreview);
        EXPECT_TRUE(editor->runCommand(QString::fromLatin1(id)));
        return editor->noteText().split(QLatin1Char('\n'));
    };
    // Numbers by value: 1, 2, 10 - not 1, 10, 2.
    QStringList lines = run("noteAppC:table-sort-column-ascending");
    ASSERT_GE(lines.size(), 6);
    EXPECT_TRUE(lines.at(0).contains(QStringLiteral("name")));
    EXPECT_TRUE(lines.at(1).contains(QStringLiteral("---")));
    EXPECT_TRUE(lines.at(2).contains(QStringLiteral("fig"))) << lines.join(QLatin1Char('\n')).toStdString();
    EXPECT_TRUE(lines.at(3).contains(QStringLiteral("Apple")));
    EXPECT_TRUE(lines.at(4).contains(QStringLiteral("pear")));
    lines = run("noteAppC:table-sort-column-descending");
    EXPECT_TRUE(lines.at(0).contains(QStringLiteral("name")));
    EXPECT_TRUE(lines.at(2).contains(QStringLiteral("pear")));
    EXPECT_TRUE(lines.at(4).contains(QStringLiteral("fig")));
}

TEST(TableAutoTest, DuplicateRowAndColumn)
{
    const QString note = QStringLiteral("| a | b |\n| --- | --: |\n| 1 | 2 |\n");
    auto row = editorWith(note, after(note, QStringLiteral("| 1")), MarkdownEditor::Mode::LivePreview);
    ASSERT_TRUE(row->runCommand(QStringLiteral("noteAppC:table-duplicate-row")));
    EXPECT_EQ(row->noteText(), QStringLiteral("| a   |   b |\n| --- | --: |\n| 1   |   2 |\n| 1   |   2 |\n"))
        << row->noteText().toStdString();

    auto column = editorWith(note, after(note, QStringLiteral("| b")), MarkdownEditor::Mode::LivePreview);
    ASSERT_TRUE(column->runCommand(QStringLiteral("noteAppC:table-duplicate-column")));
    EXPECT_EQ(column->noteText(), QStringLiteral("| a   |   b |   b |\n| --- | --: | --: |\n| 1   |   2 |   2 |\n"))
        << column->noteText().toStdString();

    auto header = editorWith(note, after(note, QStringLiteral("| a")), MarkdownEditor::Mode::LivePreview);
    header->runCommand(QStringLiteral("noteAppC:table-duplicate-row"));
    EXPECT_EQ(header->noteText(), note) << "the header is the table's only one";
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
