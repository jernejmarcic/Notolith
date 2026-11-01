// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej Marčič
#include <gtest/gtest.h>

#include <QApplication>
#include <QClipboard>
#include <QElapsedTimer>
#include <QMimeData>
#include <QPaintEvent>
#include <QScrollBar>
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

// --- the table's handles, add bars and rectangular selection (TableHandles.cpp) ---

namespace {

const QString kGrid = QStringLiteral(
    "para\n\n| a | b | c |\n| --- | :-: | --: |\n| 1 | 2 | 3 |\n| 4 | 5 | 6 |\n| 7 | 8 | 9 |\n\nend\n");
// A table drawn as one, the caret outside it.
std::unique_ptr<MarkdownEditor> gridEditor(const QString &note = kGrid)
{
    auto editor = editorWith(note, 0, MarkdownEditor::Mode::LivePreview);
    editor->setFocus();
    return editor;
}

qsizetype tableAt(const QString &note = kGrid) { return note.indexOf(QStringLiteral("| a")); }

QPoint centre(const QRectF &rect) { return rect.center().toPoint(); }

void moveTo(MarkdownEditor &editor, const QPoint &point, Qt::MouseButtons held = Qt::NoButton)
{
    QMouseEvent move(QEvent::MouseMove, QPointF(point), editor.viewport()->mapToGlobal(QPointF(point)),
                     Qt::NoButton, held, Qt::NoModifier);
    QCoreApplication::sendEvent(editor.viewport(), &move);
}

void press(MarkdownEditor &editor, const QPoint &point, Qt::KeyboardModifiers modifiers = Qt::NoModifier)
{
    QTest::mousePress(editor.viewport(), Qt::LeftButton, modifiers, point);
}

void release(MarkdownEditor &editor, const QPoint &point)
{
    QTest::mouseRelease(editor.viewport(), Qt::LeftButton, Qt::NoModifier, point);
}

/// A press, a drag to `to` and a release, the way the mouse does it.
void drag(MarkdownEditor &editor, const QPoint &from, const QPoint &to)
{
    moveTo(editor, from);
    press(editor, from);
    moveTo(editor, QPoint((from.x() + to.x()) / 2, (from.y() + to.y()) / 2), Qt::LeftButton);
    moveTo(editor, to, Qt::LeftButton);
    release(editor, to);
}

QPoint cellCentre(const MarkdownEditor &editor, int row, int column)
{
    return centre(editor.tableCellRect(tableAt(), row, column));
}

QString pipeRows(const MarkdownEditor &editor)
{
    const QString text = editor.noteText();
    const qsizetype start = text.indexOf(QStringLiteral("| "));
    return text.mid(start, text.indexOf(QStringLiteral("\n\n"), start) - start);
}

} // namespace

TEST(TableHandlesTest, HoverFindsTheGripsAndTheBars)
{
    auto editor = gridEditor();
    const qsizetype at = tableAt();
    ASSERT_FALSE(editor->tableCellRect(at, 1, 1).isEmpty()) << "the table is drawn as a grid";

    moveTo(*editor, cellCentre(*editor, 2, 1));
    EXPECT_EQ(editor->hoveredTablePart(), MarkdownEditor::TablePart::None);
    moveTo(*editor, centre(editor->tableChromeRect(at, MarkdownEditor::TablePart::RowGrip, 2)));
    EXPECT_EQ(editor->hoveredTablePart(), MarkdownEditor::TablePart::RowGrip);
    moveTo(*editor, centre(editor->tableChromeRect(at, MarkdownEditor::TablePart::ColumnGrip, 1)));
    EXPECT_EQ(editor->hoveredTablePart(), MarkdownEditor::TablePart::ColumnGrip);
    moveTo(*editor, centre(editor->tableChromeRect(at, MarkdownEditor::TablePart::AddRow)));
    EXPECT_EQ(editor->hoveredTablePart(), MarkdownEditor::TablePart::AddRow);
    moveTo(*editor, centre(editor->tableChromeRect(at, MarkdownEditor::TablePart::AddColumn)));
    EXPECT_EQ(editor->hoveredTablePart(), MarkdownEditor::TablePart::AddColumn);
    moveTo(*editor, QPoint(5, 5));
    EXPECT_EQ(editor->hoveredTablePart(), MarkdownEditor::TablePart::None);

    // The `</>` button over the table is still the button.
    moveTo(*editor, cellCentre(*editor, 1, 1));
    ASSERT_TRUE(editor->editButtonRect().isValid());
    press(*editor, centre(editor->editButtonRect()));
    release(*editor, centre(editor->editButtonRect()));
    EXPECT_EQ(editor->selectedText(), kGrid.mid(at, kGrid.indexOf(QStringLiteral("\n\nend")) - at));
}

TEST(TableHandlesTest, OffTracksNothingAndDragsSelectText)
{
    auto editor = gridEditor();
    editor->setTableHandles(false);
    moveTo(*editor, centre(editor->tableCellRect(tableAt(), 1, 0)));
    moveTo(*editor, centre(editor->tableChromeRect(tableAt(), MarkdownEditor::TablePart::RowGrip, 2)));
    EXPECT_EQ(editor->hoveredTablePart(), MarkdownEditor::TablePart::None);

    drag(*editor, cellCentre(*editor, 1, 0), cellCentre(*editor, 2, 1));
    EXPECT_FALSE(editor->cellSelectionRect().isValid());
    EXPECT_FALSE(editor->selectedText().isEmpty()) << "a text selection, as before";
}

TEST(TableHandlesTest, DraggingARowGripReordersBodyRowsInOneStep)
{
    auto editor = gridEditor();
    const qsizetype at = tableAt();
    const QRectF grip = editor->tableChromeRect(at, MarkdownEditor::TablePart::RowGrip, 1);
    const QRectF last = editor->tableCellRect(at, 3, 0);
    drag(*editor, centre(grip), QPoint(last.center().x(), int(last.bottom()) - 2)); // below the last row
    EXPECT_EQ(pipeRows(*editor), QStringLiteral(
        "| a   |  b  |   c |\n| --- | :-: | --: |\n| 4   |  5  |   6 |\n| 7   |  8  |   9 |\n| 1   |  2  |   3 |"))
        << pipeRows(*editor).toStdString();
    EXPECT_TRUE(editor->cellSelectionRect().isValid()) << "the moved row stays selected";
    editor->grab(); // paints
    EXPECT_TRUE(editor->tableRenderedAt(at)) << "and the table stays a grid, not its source";
    undo(*editor);
    EXPECT_EQ(editor->noteText(), kGrid) << "one undo step";

    // The header does not move, and nothing goes above it.
    const QRectF header = editor->tableChromeRect(at, MarkdownEditor::TablePart::RowGrip, 0);
    drag(*editor, centre(header), cellCentre(*editor, 3, 0));
    EXPECT_EQ(editor->noteText(), kGrid);
    drag(*editor, centre(editor->tableChromeRect(at, MarkdownEditor::TablePart::RowGrip, 2)),
         QPoint(cellCentre(*editor, 0, 0).x(), int(editor->tableCellRect(at, 0, 0).top()) + 1));
    const QStringList lines = editor->noteText().split(QLatin1Char('\n'));
    ASSERT_GE(lines.size(), 8);
    EXPECT_TRUE(lines.at(2).contains(QStringLiteral("a")) && lines.at(3).contains(QStringLiteral("---"))
                && lines.at(4).contains(QStringLiteral("4")) && lines.at(5).contains(QStringLiteral("1")))
        << "dropped at the top of the body: the header and the delimiter stay\n" << editor->noteText().toStdString();
}

TEST(TableHandlesTest, DraggingAColumnGripMovesItsAlignmentToo)
{
    auto editor = gridEditor();
    const qsizetype at = tableAt();
    const QRectF right = editor->tableCellRect(at, 1, 2);
    drag(*editor, centre(editor->tableChromeRect(at, MarkdownEditor::TablePart::ColumnGrip, 0)),
         QPoint(int(right.right()) - 2, int(right.center().y()))); // past the last column
    EXPECT_EQ(pipeRows(*editor), QStringLiteral(
        "|  b  |   c | a   |\n| :-: | --: | --- |\n|  2  |   3 | 1   |\n|  5  |   6 | 4   |\n|  8  |   9 | 7   |"))
        << pipeRows(*editor).toStdString();
    undo(*editor);
    EXPECT_EQ(editor->noteText(), kGrid);
}

TEST(TableHandlesTest, AClickOnAGripSelectsTheRowOrColumn)
{
    auto editor = gridEditor();
    const qsizetype at = tableAt();
    const QPoint grip = centre(editor->tableChromeRect(at, MarkdownEditor::TablePart::RowGrip, 2));
    moveTo(*editor, grip);
    press(*editor, grip);
    release(*editor, grip);
    EXPECT_EQ(editor->cellSelectionRect(), QRect(0, 2, 3, 1));
    EXPECT_EQ(editor->noteText(), kGrid) << "selecting writes nothing";

    const QPoint column = centre(editor->tableChromeRect(at, MarkdownEditor::TablePart::ColumnGrip, 1));
    moveTo(*editor, column);
    press(*editor, column);
    release(*editor, column);
    EXPECT_EQ(editor->cellSelectionRect(), QRect(1, 0, 1, 4));
}

TEST(TableHandlesTest, TheAddBarsAppendARowAndAColumn)
{
    auto editor = gridEditor();
    const qsizetype at = tableAt();
    const QPoint row = centre(editor->tableChromeRect(at, MarkdownEditor::TablePart::AddRow));
    moveTo(*editor, row);
    press(*editor, row);
    release(*editor, row);
    EXPECT_TRUE(pipeRows(*editor).endsWith(QStringLiteral("| 7   |  8  |   9 |\n|     |     |     |")))
        << pipeRows(*editor).toStdString();
    // The caret is in the new row's first cell, so typing goes there.
    QTest::keyClicks(editor.get(), QStringLiteral("x"));
    EXPECT_TRUE(editor->noteText().contains(QStringLiteral("| x ")));
    undo(*editor);
    undo(*editor);
    EXPECT_EQ(editor->noteText(), kGrid) << "the row, then the x";

    const QPoint column = centre(editor->tableChromeRect(at, MarkdownEditor::TablePart::AddColumn));
    moveTo(*editor, column);
    press(*editor, column);
    release(*editor, column);
    const QStringList lines = pipeRows(*editor).split(QLatin1Char('\n'));
    ASSERT_EQ(lines.size(), 5);
    EXPECT_TRUE(lines.at(0).endsWith(QStringLiteral("|     |"))) << lines.at(0).toStdString();
    EXPECT_TRUE(lines.at(1).endsWith(QStringLiteral("| --- |"))) << lines.at(1).toStdString();
    QTest::keyClicks(editor.get(), QStringLiteral("d"));
    EXPECT_TRUE(editor->noteText().contains(QStringLiteral("| d ")) || editor->noteText().contains(QStringLiteral("| d|")))
        << editor->noteText().toStdString();
}

TEST(TableHandlesTest, ADragAcrossCellsSelectsARectangleThatCopiesAsATable)
{
    auto editor = gridEditor();
    drag(*editor, cellCentre(*editor, 1, 0), cellCentre(*editor, 2, 1));
    EXPECT_EQ(editor->cellSelectionRect(), QRect(0, 1, 2, 2));
    EXPECT_EQ(editor->noteText(), kGrid);

    QTest::keyClick(editor.get(), Qt::Key_C, Qt::ControlModifier);
    const QMimeData *mime = QApplication::clipboard()->mimeData();
    ASSERT_NE(mime, nullptr);
    const QString markdown = QString::fromUtf8(mime->data(QStringLiteral("text/markdown")));
    EXPECT_EQ(markdown, QStringLiteral("| 1   |  2  |\n| --- | :-: |\n| 4   |  5  |"))
        << "the first row becomes the header, the alignment goes along\n" << markdown.toStdString();
    EXPECT_EQ(mime->text(), QStringLiteral("1\t2\n4\t5")) << "plain text is what a spreadsheet takes";
    EXPECT_EQ(editor->noteText(), kGrid) << "a copy writes nothing";

    // Pasted into prose, the note gets the table, not the TSV.
    editor->setCursorOffset(editor->noteText().size());
    QTest::keyClick(editor.get(), Qt::Key_V, Qt::ControlModifier);
    EXPECT_TRUE(editor->noteText().endsWith(markdown)) << editor->noteText().toStdString();
}

TEST(TableHandlesTest, ShiftArrowsGrowTheRectangleFromACell)
{
    auto editor = gridEditor();
    editor->setCursorOffset(kGrid.indexOf(QStringLiteral("| 1")) + 3); // end of the "1" cell
    QTest::keyClick(editor.get(), Qt::Key_Down, Qt::ShiftModifier);
    EXPECT_EQ(editor->cellSelectionRect(), QRect(0, 1, 1, 2));
    QTest::keyClick(editor.get(), Qt::Key_Right, Qt::ShiftModifier);
    EXPECT_EQ(editor->cellSelectionRect(), QRect(0, 1, 2, 2));
    QTest::keyClick(editor.get(), Qt::Key_Up, Qt::ShiftModifier);
    EXPECT_EQ(editor->cellSelectionRect(), QRect(0, 1, 2, 1));
    QTest::keyClick(editor.get(), Qt::Key_Escape);
    EXPECT_FALSE(editor->cellSelectionRect().isValid());
    EXPECT_EQ(editor->noteText(), kGrid);
}

TEST(TableHandlesTest, DeleteAndCutClearTheCellsAsOneStep)
{
    auto editor = gridEditor();
    drag(*editor, cellCentre(*editor, 1, 1), cellCentre(*editor, 3, 2));
    ASSERT_EQ(editor->cellSelectionRect(), QRect(1, 1, 2, 3));
    QTest::keyClick(editor.get(), Qt::Key_Delete);
    EXPECT_EQ(pipeRows(*editor), QStringLiteral(
        "| a   |  b  |   c |\n| --- | :-: | --: |\n| 1   |     |     |\n| 4   |     |     |\n| 7   |     |     |"))
        << pipeRows(*editor).toStdString();
    undo(*editor);
    EXPECT_EQ(editor->noteText(), kGrid);

    drag(*editor, cellCentre(*editor, 1, 0), cellCentre(*editor, 1, 1));
    ASSERT_EQ(editor->cellSelectionRect(), QRect(0, 1, 2, 1));
    QTest::keyClick(editor.get(), Qt::Key_X, Qt::ControlModifier);
    EXPECT_EQ(QApplication::clipboard()->mimeData()->text(), QStringLiteral("1\t2"));
    EXPECT_TRUE(pipeRows(*editor).contains(QStringLiteral("|     |     | 3 |"))
                || pipeRows(*editor).contains(QStringLiteral("|     |     |   3 |")))
        << pipeRows(*editor).toStdString();
    undo(*editor);
    EXPECT_EQ(editor->noteText(), kGrid);
}

TEST(TableHandlesTest, PasteFillsFromTheCaretCellAndGrowsTheTable)
{
    auto editor = gridEditor();
    auto *mime = new QMimeData;
    mime->setText(QStringLiteral("x\ty\tz\nu\tv\tw"));
    QApplication::clipboard()->setMimeData(mime);

    editor->setCursorOffset(kGrid.indexOf(QStringLiteral("| 7")) + 3); // the "7" cell: last row
    QTest::keyClick(editor.get(), Qt::Key_V, Qt::ControlModifier);
    const QStringList lines = pipeRows(*editor).split(QLatin1Char('\n'));
    ASSERT_EQ(lines.size(), 6) << pipeRows(*editor).toStdString(); // grew by one row
    EXPECT_TRUE(lines.at(4).contains(QStringLiteral("x"))) << pipeRows(*editor).toStdString();
    EXPECT_TRUE(lines.at(4).contains(QStringLiteral(" y ")) && lines.at(4).contains(QStringLiteral(" z ")));
    EXPECT_TRUE(lines.at(5).contains(QStringLiteral("u")) && lines.at(5).contains(QStringLiteral("w")));
    EXPECT_EQ(lines.at(4).count(QLatin1Char('|')), 4) << "x goes in column 0, the 3-wide block fits";
    EXPECT_EQ(editor->cellSelectionRect(), QRect(0, 3, 3, 2)) << "what was pasted is selected";
    undo(*editor);
    EXPECT_EQ(editor->noteText(), kGrid) << "one undo step";

    // A pipe table pastes too, its delimiter row left out; a word is the ordinary paste.
    auto *table = new QMimeData;
    table->setText(QStringLiteral("| p | q |\n| --- | --- |\n| r | s |"));
    QApplication::clipboard()->setMimeData(table);
    editor->setCursorOffset(kGrid.indexOf(QStringLiteral("| 2")) + 3);
    QTest::keyClick(editor.get(), Qt::Key_V, Qt::ControlModifier);
    EXPECT_TRUE(pipeRows(*editor).contains(QStringLiteral(" p ")) && pipeRows(*editor).contains(QStringLiteral(" r ")))
        << pipeRows(*editor).toStdString();
    EXPECT_EQ(pipeRows(*editor).split(QLatin1Char('\n')).at(2).count(QLatin1Char('|')), 4);
    undo(*editor);

    auto *word = new QMimeData;
    word->setText(QStringLiteral("word"));
    QApplication::clipboard()->setMimeData(word);
    QTest::keyClick(editor.get(), Qt::Key_V, Qt::ControlModifier);
    EXPECT_TRUE(editor->noteText().contains(QStringLiteral("word")));
}

TEST(TableHandlesTest, OneValuePastedOverASelectionFillsIt)
{
    auto editor = gridEditor();
    auto *mime = new QMimeData;
    mime->setText(QStringLiteral("z"));
    QApplication::clipboard()->setMimeData(mime);
    drag(*editor, cellCentre(*editor, 1, 0), cellCentre(*editor, 2, 1));
    QTest::keyClick(editor.get(), Qt::Key_V, Qt::ControlModifier);
    EXPECT_EQ(editor->noteText().count(QLatin1Char('z')), 4) << editor->noteText().toStdString();
}

TEST(TableHandlesTest, DraggingAnAddBarAddsOneRowOrColumnForEachCellTravelled)
{
    auto editor = gridEditor();
    const qsizetype at = tableAt();
    const QRectF last = editor->tableCellRect(at, 3, 0);
    const QPoint bar = centre(editor->tableChromeRect(at, MarkdownEditor::TablePart::AddRow));
    moveTo(*editor, bar);
    press(*editor, bar);
    // Three and a bit row heights down.
    const QPoint to = bar + QPoint(0, int(last.height() * 3.4));
    moveTo(*editor, bar + QPoint(0, 6), Qt::LeftButton);
    moveTo(*editor, to, Qt::LeftButton);
    EXPECT_EQ(editor->tableAddPreview(), 3) << "previewed while dragging";
    EXPECT_EQ(editor->noteText(), kGrid) << "and nothing written until the release";
    moveTo(*editor, bar + QPoint(0, int(last.height() * 1.2)), Qt::LeftButton);
    EXPECT_EQ(editor->tableAddPreview(), 1) << "dragging back takes them away";
    moveTo(*editor, to, Qt::LeftButton);
    release(*editor, to);
    const QStringList lines = pipeRows(*editor).split(QLatin1Char('\n'));
    ASSERT_EQ(lines.size(), 8) << pipeRows(*editor).toStdString();
    for (int i = 5; i < 8; ++i)
        EXPECT_EQ(lines.at(i), QStringLiteral("|     |     |     |"));
    EXPECT_EQ(editor->tableAddPreview(), 0);
    undo(*editor);
    EXPECT_EQ(editor->noteText(), kGrid) << "one undo step for all three";

    // Columns: two widths to the right.
    const QRectF lastColumn = editor->tableCellRect(at, 1, 2);
    const QPoint side = centre(editor->tableChromeRect(at, MarkdownEditor::TablePart::AddColumn));
    moveTo(*editor, side);
    press(*editor, side);
    moveTo(*editor, side + QPoint(5, 0), Qt::LeftButton);
    const QPoint across = side + QPoint(int(lastColumn.width() * 2.5), 0);
    moveTo(*editor, across, Qt::LeftButton);
    EXPECT_EQ(editor->tableAddPreview(), 2);
    release(*editor, across);
    const QStringList columns = pipeRows(*editor).split(QLatin1Char('\n'));
    ASSERT_EQ(columns.size(), 5);
    EXPECT_EQ(columns.at(0).count(QLatin1Char('|')), 6) << columns.at(0).toStdString();
    EXPECT_TRUE(columns.at(1).endsWith(QStringLiteral("| --- | --- |"))) << columns.at(1).toStdString();
    undo(*editor);
    EXPECT_EQ(editor->noteText(), kGrid);
}

TEST(TableHandlesTest, ASmallWobbleOnABarStillAddsOne)
{
    auto editor = gridEditor();
    const QPoint bar = centre(editor->tableChromeRect(tableAt(), MarkdownEditor::TablePart::AddRow));
    moveTo(*editor, bar);
    press(*editor, bar);
    moveTo(*editor, bar + QPoint(1, 1), Qt::LeftButton);
    release(*editor, bar + QPoint(1, 1));
    EXPECT_EQ(pipeRows(*editor).split(QLatin1Char('\n')).size(), 6) << "one row, as a click does";
}

namespace {

/// A table wider than the editor: `columns` columns of long cells.
QString wideNote(int columns)
{
    QStringList head, rule, body;
    for (int c = 0; c < columns; ++c) {
        head << QStringLiteral("column-number-%1-header").arg(c);
        rule << QStringLiteral("---");
        body << QStringLiteral("cell-%1-text-text-text").arg(c);
    }
    const auto row = [](const QStringList &cells) { return QStringLiteral("| ") + cells.join(QStringLiteral(" | ")) + QStringLiteral(" |"); };
    return QStringLiteral("para\n\n") + row(head) + QLatin1Char('\n') + row(rule) + QLatin1Char('\n') + row(body)
           + QLatin1Char('\n') + row(body) + QStringLiteral("\n\nend\n");
}

} // namespace

TEST(TableHandlesTest, DraggingAColumnGripPastTheEdgeAutoscrollsAWideTableUntilRelease)
{
    const QString note = wideNote(12);
    auto editor = gridEditor(note);
    const qsizetype at = note.indexOf(QStringLiteral("| column"));
    const qreal startLeft = editor->tableCellRect(at, 0, 0).left();
    const QPoint grip = centre(editor->tableChromeRect(at, MarkdownEditor::TablePart::ColumnGrip, 0));
    moveTo(*editor, grip);
    press(*editor, grip);
    moveTo(*editor, grip + QPoint(20, 0), Qt::LeftButton);
    EXPECT_FALSE(editor->tableAutoScrolling()) << "inside the table's window: no scroll";
    const int past = editor->viewport()->width() + 30;
    moveTo(*editor, QPoint(past, grip.y()), Qt::LeftButton);
    ASSERT_TRUE(editor->tableAutoScrolling());
    ASSERT_TRUE(QTest::qWaitFor([&] { return editor->tableCellRect(at, 0, 0).left() < startLeft - 100; }, 2000))
        << "the table pans while the pointer stays put";
    // Farther past the edge is faster.
    qreal mark = editor->tableCellRect(at, 0, 0).left();
    QTest::qWait(100);
    const qreal slow = mark - editor->tableCellRect(at, 0, 0).left();
    moveTo(*editor, QPoint(past + 400, grip.y()), Qt::LeftButton);
    mark = editor->tableCellRect(at, 0, 0).left();
    QTest::qWait(100);
    const qreal fast = mark - editor->tableCellRect(at, 0, 0).left();
    EXPECT_GT(fast, slow);

    release(*editor, QPoint(past, grip.y()));
    EXPECT_FALSE(editor->tableAutoScrolling());
    QTest::qWait(150); // the drop's own edit settles first
    editor->grab();
    const qreal stopped = editor->tableCellRect(at, 0, 0).left();
    QTest::qWait(120);
    editor->grab();
    EXPECT_EQ(editor->tableCellRect(at, 0, 0).left(), stopped) << "nothing scrolls after the release";
}

TEST(TableHandlesTest, ASelectionDraggedBelowTheViewportAutoscrollsATallTable)
{
    QString note = QStringLiteral("| a | b |\n| --- | --- |\n");
    for (int i = 0; i < 40; ++i)
        note += QStringLiteral("| r%1 | v%1 |\n").arg(i);
    note += QStringLiteral("\nend\n");
    auto editor = gridEditor(note);
    editor->resize(900, 220);
    QCoreApplication::processEvents();
    const qsizetype at = note.indexOf(QStringLiteral("| a"));
    QScrollBar *bar = editor->verticalScrollBar();
    ASSERT_GT(bar->maximum(), 100) << "the table is taller than the view";
    ASSERT_EQ(bar->value(), 0);

    const QPoint from = centre(editor->tableCellRect(at, 1, 0));
    moveTo(*editor, from);
    press(*editor, from);
    moveTo(*editor, from + QPoint(40, 20), Qt::LeftButton);
    const QPoint below(from.x() + 60, editor->viewport()->height() + 25);
    moveTo(*editor, below, Qt::LeftButton);
    ASSERT_TRUE(editor->tableAutoScrolling());
    ASSERT_TRUE(QTest::qWaitFor([&] { return bar->value() > 120; }, 3000));
    EXPECT_GT(editor->cellSelectionRect().height(), 4) << "the rectangle grows as the rows pass";
    EXPECT_EQ(editor->cellSelectionRect().top(), 1) << "from the row it began in";
    release(*editor, below);
    EXPECT_FALSE(editor->tableAutoScrolling());
    const int stopped = bar->value();
    QTest::qWait(120);
    EXPECT_EQ(bar->value(), stopped);
    EXPECT_EQ(editor->noteText(), note) << "selecting writes nothing";
}

namespace {

/// Records the region of every paint the viewport gets.
class PaintSpy : public QObject {
public:
    QList<QRect> regions;

protected:
    bool eventFilter(QObject *, QEvent *event) override
    {
        if (event->type() == QEvent::Paint)
            regions.append(static_cast<QPaintEvent *>(event)->region().boundingRect());
        return false;
    }
};

} // namespace

TEST(TableHandlesTest, HoverRepaintsOnlyTheChangedHandles)
{
    auto editor = gridEditor();
    editor->show();
    ASSERT_TRUE(QTest::qWaitForWindowExposed(editor.get()));
    QCoreApplication::processEvents();
    const qsizetype at = tableAt();
    const QRectF table = editor->tableCellRect(at, 0, 0).united(editor->tableCellRect(at, 3, 2));
    PaintSpy spy;
    editor->viewport()->installEventFilter(&spy);

    moveTo(*editor, cellCentre(*editor, 1, 1)); // into the table: its grips and bars come
    QCoreApplication::processEvents();
    spy.regions.clear();
    moveTo(*editor, cellCentre(*editor, 2, 1)); // next row, same column: one grip moves
    QCoreApplication::processEvents();
    ASSERT_FALSE(spy.regions.isEmpty());
    for (const QRect &region : spy.regions) {
        EXPECT_LT(region.width() * region.height(), table.width() * table.height() / 4)
            << "a grip, not the table";
    }
    spy.regions.clear();
    moveTo(*editor, cellCentre(*editor, 2, 1) + QPoint(2, 1)); // within the cell: nothing changed
    QCoreApplication::processEvents();
    EXPECT_TRUE(spy.regions.isEmpty()) << "nothing repainted when nothing changed";
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
