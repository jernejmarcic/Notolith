// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej
// HTML in live preview edited where it is drawn (libs/editor/HtmlEdit.cpp):
// an HTML table cell by cell like a pipe table, a <p> or an <li> like
// Markdown text with its tags hidden, every byte of the note accounted for.
// Every edit test asserts the whole note, and that undo gives it back.

#include <gtest/gtest.h>

#include <QApplication>
#include <QClipboard>
#include <QMimeData>
#include <QMouseEvent>
#include <QSignalSpy>
#include <QtTest/QTest>

#include <memory>

#include "MarkdownEditor.h"
#include "Parser.h"
#include "WaitFor.h"

using namespace noteAppC;

namespace {

std::unique_ptr<MarkdownEditor> editorWith(const QString &text)
{
    auto editor = std::make_unique<MarkdownEditor>();
    editor->resize(900, 600);
    editor->setNoteText(text);
    editor->show();
    return editor;
}

/// The parse that follows an edit has landed.
void settle(MarkdownEditor &editor)
{
    QSignalSpy parsed(&editor, &MarkdownEditor::headingsChanged);
    parsed.wait(2000);
}

/// Undoes until nothing is left to undo; the note must be back as it was.
void undoAll(MarkdownEditor &editor)
{
    for (int i = 0; i < 40; ++i)
        QTest::keyClick(&editor, Qt::Key_Z, Qt::ControlModifier);
}

/// Clicks across and down the note until the caret lands inside `range`.
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

const QString kTable = QStringLiteral(
    "above\n\n<table>\n  <tr><th>name</th><th>qty</th></tr>\n  <tr><td>apple</td><td>3</td></tr>\n</table>\n\nbelow\n");

} // namespace

#ifdef NOTEAPPC_EXPECT_HTML_EDIT

TEST(HtmlTableTest, AClickLandsInTheCellAndTypingEscapes)
{
    auto editor = editorWith(kTable);
    const qsizetype apple = kTable.indexOf(QStringLiteral("apple"));
    ASSERT_TRUE(editor->tableRenderedAt(apple)) << "drawn as a grid, not litehtml's image";
    EXPECT_FALSE(editor->htmlImageSizeAt(apple).isValid());
    ASSERT_FALSE(clickInto(*editor, {apple, apple + 5}).isNull());
    EXPECT_TRUE(editor->tableRenderedAt(apple)); // still a table under the caret

    editor->setCursorOffset(apple + 5);
    QTest::keyClicks(editor.get(), QStringLiteral("s & <x>"));
    QString expected = kTable;
    expected.replace(QStringLiteral("<td>apple</td>"), QStringLiteral("<td>apples &amp; &lt;x&gt;</td>"));
    EXPECT_EQ(editor->noteText(), expected);
    EXPECT_TRUE(editor->tableRenderedAt(apple));

    undoAll(*editor);
    EXPECT_EQ(editor->noteText(), kTable);
}

TEST(HtmlTableTest, TabCrossesCellsAndAddsARowAtTheEnd)
{
    auto editor = editorWith(kTable);
    const qsizetype apple = kTable.indexOf(QStringLiteral("apple"));
    const qsizetype three = kTable.indexOf(QStringLiteral("3</td>"));
    editor->setCursorOffset(apple);
    QTest::keyClick(editor.get(), Qt::Key_Backspace); // at a cell's start: nothing
    EXPECT_EQ(editor->noteText(), kTable);
    QTest::keyClick(editor.get(), Qt::Key_Tab);
    EXPECT_EQ(editor->cursorOffset(), three + 1);
    QTest::keyClick(editor.get(), Qt::Key_Backtab);
    EXPECT_EQ(editor->cursorOffset(), apple + 5);
    QTest::keyClick(editor.get(), Qt::Key_Right); // past the cell's end: the next cell
    EXPECT_EQ(editor->cursorOffset(), three);

    editor->setCursorOffset(three + 1);
    QTest::keyClick(editor.get(), Qt::Key_Tab); // from the last cell: a new row
    QTest::keyClicks(editor.get(), QStringLiteral("fig"));
    QString expected = kTable;
    expected.replace(QStringLiteral("<td>3</td></tr>\n"),
                     QStringLiteral("<td>3</td></tr>\n  <tr><td>fig</td><td></td></tr>\n"));
    EXPECT_EQ(editor->noteText(), expected);
    EXPECT_TRUE(editor->tableRenderedAt(apple));

    undoAll(*editor);
    EXPECT_EQ(editor->noteText(), kTable);
}

TEST(HtmlTableTest, EnterAddsARowWrittenLikeTheTable)
{
    // Upper case, optional end tags left out, a thead: the new row copies
    // the case and closes its cells.
    const QString note = QStringLiteral(
        "<TABLE>\n<THEAD><TR><TH>a<TH>b</THEAD>\n<TBODY>\n<TR><TD>1<TD>2\n</TBODY>\n</TABLE>\n\nafter\n");
    auto editor = editorWith(note);
    const qsizetype two = note.indexOf(QLatin1Char('2'));
    ASSERT_TRUE(editor->tableRenderedAt(two));
    editor->setCursorOffset(two + 1);
    QTest::keyClick(editor.get(), Qt::Key_Return); // the last row: a new one, same column
    QTest::keyClicks(editor.get(), QStringLiteral("9"));
    const QString expected = QStringLiteral(
        "<TABLE>\n<THEAD><TR><TH>a<TH>b</THEAD>\n<TBODY>\n<TR><TD>1<TD>2\n<TR><TD></TD><TD>9</TD></TR>\n</TBODY>\n</TABLE>\n\nafter\n");
    EXPECT_EQ(editor->noteText(), expected);

    undoAll(*editor);
    EXPECT_EQ(editor->noteText(), note);
}

TEST(HtmlTableTest, RowAndColumnCommands)
{
    auto editor = editorWith(kTable);
    const qsizetype apple = kTable.indexOf(QStringLiteral("apple"));
    editor->setCursorOffset(apple);
    ASSERT_TRUE(editor->runCommand(QStringLiteral("noteAppC:table-insert-column-after")));
    QString expected = kTable;
    expected.replace(QStringLiteral("<th>name</th>"), QStringLiteral("<th>name</th><th></th>"));
    expected.replace(QStringLiteral("<td>apple</td>"), QStringLiteral("<td>apple</td><td></td>"));
    EXPECT_EQ(editor->noteText(), expected);

    editor->setCursorOffset(expected.indexOf(QStringLiteral("apple")));
    ASSERT_TRUE(editor->runCommand(QStringLiteral("noteAppC:table-insert-row-before")));
    const QString withRow = QStringLiteral(
        "above\n\n<table>\n  <tr><th>name</th><th></th><th>qty</th></tr>\n  <tr><td></td><td></td><td></td></tr>\n"
        "  <tr><td>apple</td><td></td><td>3</td></tr>\n</table>\n\nbelow\n");
    EXPECT_EQ(editor->noteText(), withRow);

    ASSERT_TRUE(editor->runCommand(QStringLiteral("noteAppC:table-delete-row")));
    EXPECT_EQ(editor->noteText(), expected);
    editor->setCursorOffset(expected.indexOf(QStringLiteral("apple")));
    ASSERT_TRUE(editor->runCommand(QStringLiteral("noteAppC:table-move-column-right")));
    QString moved = kTable;
    moved.replace(QStringLiteral("<th>name</th>"), QStringLiteral("<th></th><th>name</th>"));
    moved.replace(QStringLiteral("<td>apple</td>"), QStringLiteral("<td></td><td>apple</td>"));
    EXPECT_EQ(editor->noteText(), moved);

    undoAll(*editor);
    EXPECT_EQ(editor->noteText(), kTable);
}

TEST(HtmlTableTest, LeavingAnEditedTableLeavesItsSourceAsWritten)
{
    // A pipe table is aligned when the caret leaves it; HTML never is.
    auto editor = editorWith(kTable);
    editor->setCursorOffset(kTable.indexOf(QStringLiteral("apple")) + 5);
    QTest::keyClicks(editor.get(), QStringLiteral("s"));
    editor->setCursorOffset(kTable.indexOf(QStringLiteral("below")) + 1);
    QString expected = kTable;
    expected.replace(QStringLiteral("apple"), QStringLiteral("apples"));
    EXPECT_EQ(editor->noteText(), expected);
}

namespace {

const QString kHandles = QStringLiteral(
    "above\n\n<table class=\"t\">\n"
    "  <tr><th>name</th><th>qty</th></tr>\n"
    "  <tr id=\"a\"><td>apple &amp; co</td><td>3</td></tr>\n"
    "  <tr id=\"b\" class=\"x\"><td style=\"color:red\">pear</td><td><b>5</b></td></tr>\n"
    "  <tr><td>fig</td><td>7</td></tr>\n"
    "</table>\n\nbelow\n");

QPoint centre(const QRectF &rect) { return rect.center().toPoint(); }

void moveTo(MarkdownEditor &editor, const QPoint &point, Qt::MouseButtons held = Qt::NoButton)
{
    QMouseEvent move(QEvent::MouseMove, QPointF(point), editor.viewport()->mapToGlobal(QPointF(point)),
                     Qt::NoButton, held, Qt::NoModifier);
    QCoreApplication::sendEvent(editor.viewport(), &move);
}

void drag(MarkdownEditor &editor, const QPoint &from, const QPoint &to)
{
    moveTo(editor, from);
    QTest::mousePress(editor.viewport(), Qt::LeftButton, Qt::NoModifier, from);
    moveTo(editor, QPoint((from.x() + to.x()) / 2, (from.y() + to.y()) / 2), Qt::LeftButton);
    moveTo(editor, to, Qt::LeftButton);
    QTest::mouseRelease(editor.viewport(), Qt::LeftButton, Qt::NoModifier, to);
}

/// The parse and the relayout an edit leaves for later have happened.
void quiet(MarkdownEditor &editor)
{
    QTest::qWait(80);
    editor.grab();
}

/// A shown, laid-out editor on `text` with the caret out of the table.
std::unique_ptr<MarkdownEditor> handlesEditor(const QString &text = kHandles)
{
    auto editor = editorWith(text);
    editor->setCursorOffset(text.size());
    quiet(*editor);
    editor->grab();
    return editor;
}

qsizetype handlesAt(const QString &text = kHandles) { return text.indexOf(QStringLiteral("<table")); }

} // namespace

TEST(HtmlTableHandlesTest, HoverFindsTheGripsAndBarsOfASimpleGrid)
{
    auto editor = handlesEditor();
    const qsizetype at = handlesAt();
    ASSERT_FALSE(editor->tableCellRect(at, 1, 1).isEmpty());
    moveTo(*editor, centre(editor->tableChromeRect(at, MarkdownEditor::TablePart::RowGrip, 2)));
    EXPECT_EQ(editor->hoveredTablePart(), MarkdownEditor::TablePart::RowGrip);
    moveTo(*editor, centre(editor->tableChromeRect(at, MarkdownEditor::TablePart::ColumnGrip, 1)));
    EXPECT_EQ(editor->hoveredTablePart(), MarkdownEditor::TablePart::ColumnGrip);
    moveTo(*editor, centre(editor->tableChromeRect(at, MarkdownEditor::TablePart::AddRow)));
    EXPECT_EQ(editor->hoveredTablePart(), MarkdownEditor::TablePart::AddRow);
}

TEST(HtmlTableHandlesTest, AMergedCellMeansNoHandles)
{
    const QString merged = QStringLiteral(
        "above\n\n<table>\n  <tr><th colspan=\"2\">both</th></tr>\n  <tr><td>a</td><td>b</td></tr>\n</table>\n\nbelow\n");
    auto editor = handlesEditor(merged);
    const qsizetype at = handlesAt(merged);
    ASSERT_TRUE(editor->tableRenderedAt(at)) << "still edited cell by cell";
    EXPECT_TRUE(editor->tableChromeRect(at, MarkdownEditor::TablePart::AddRow).isEmpty());
    EXPECT_TRUE(editor->tableChromeRect(at, MarkdownEditor::TablePart::RowGrip, 1).isEmpty());
    // A row with fewer cells than the others is not a simple grid either.
    const QString ragged = QStringLiteral(
        "above\n\n<table>\n  <tr><th>a</th><th>b</th></tr>\n  <tr><td>c</td></tr>\n</table>\n\nbelow\n");
    auto other = handlesEditor(ragged);
    EXPECT_TRUE(other->tableChromeRect(handlesAt(ragged), MarkdownEditor::TablePart::AddRow).isEmpty());
}

TEST(HtmlTableHandlesTest, DraggingARowGripMovesTheRowAndNothingElse)
{
    auto editor = handlesEditor();
    const qsizetype at = handlesAt();
    const QRectF last = editor->tableCellRect(at, 3, 0);
    // The first body row (apple) to the end.
    drag(*editor, centre(editor->tableChromeRect(at, MarkdownEditor::TablePart::RowGrip, 1)),
         QPoint(int(last.center().x()), int(last.bottom()) - 2));
    quiet(*editor);
    const QString expected = QStringLiteral(
        "above\n\n<table class=\"t\">\n"
        "  <tr><th>name</th><th>qty</th></tr>\n"
        "  <tr id=\"b\" class=\"x\"><td style=\"color:red\">pear</td><td><b>5</b></td></tr>\n"
            "  <tr><td>fig</td><td>7</td></tr>\n"
        "  <tr id=\"a\"><td>apple &amp; co</td><td>3</td></tr>\n"
        "</table>\n\nbelow\n");
    EXPECT_EQ(editor->noteText().toStdString(), expected.toStdString());
    EXPECT_EQ(editor->cellSelectionRect(), QRect(0, 3, 2, 1)) << "the moved row stays selected";
    editor->grab();
    EXPECT_TRUE(editor->tableRenderedAt(at)) << "and the table stays a grid";
    undoAll(*editor);
    EXPECT_EQ(editor->noteText().toStdString(), kHandles.toStdString());
}

TEST(HtmlTableHandlesTest, TheHeaderRowGripOnlySelects)
{
    auto editor = handlesEditor();
    const qsizetype at = handlesAt();
    const QRectF last = editor->tableCellRect(at, 3, 0);
    drag(*editor, centre(editor->tableChromeRect(at, MarkdownEditor::TablePart::RowGrip, 0)),
         QPoint(int(last.center().x()), int(last.bottom()) - 2));
    EXPECT_EQ(editor->noteText().toStdString(), kHandles.toStdString());
    // A drop above the first body row stays at the top of the body.
    drag(*editor, centre(editor->tableChromeRect(at, MarkdownEditor::TablePart::RowGrip, 3)),
         QPoint(int(last.center().x()), int(editor->tableCellRect(at, 0, 0).top()) + 1));
    quiet(*editor);
    EXPECT_LT(editor->noteText().indexOf(QStringLiteral("<td>fig")), editor->noteText().indexOf(QStringLiteral("apple")));
    EXPECT_LT(editor->noteText().indexOf(QStringLiteral("<th>name")), editor->noteText().indexOf(QStringLiteral("<td>fig")));
}

TEST(HtmlTableHandlesTest, DraggingAColumnGripMovesItsCellsInEveryRow)
{
    auto editor = handlesEditor();
    const qsizetype at = handlesAt();
    const QRectF right = editor->tableCellRect(at, 1, 1);
    drag(*editor, centre(editor->tableChromeRect(at, MarkdownEditor::TablePart::ColumnGrip, 0)),
         QPoint(int(right.right()) - 2, int(right.center().y())));
    quiet(*editor);
    const QString expected = QStringLiteral(
        "above\n\n<table class=\"t\">\n"
        "  <tr><th>qty</th><th>name</th></tr>\n"
        "  <tr id=\"a\"><td>3</td><td>apple &amp; co</td></tr>\n"
            "  <tr id=\"b\" class=\"x\"><td><b>5</b></td><td style=\"color:red\">pear</td></tr>\n"
        "  <tr><td>7</td><td>fig</td></tr>\n"
        "</table>\n\nbelow\n");
    EXPECT_EQ(editor->noteText().toStdString(), expected.toStdString());
    undoAll(*editor);
    EXPECT_EQ(editor->noteText().toStdString(), kHandles.toStdString());
}

TEST(HtmlTableHandlesTest, TheAddBarsAppendAndADragAddsSeveral)
{
    auto editor = handlesEditor();
    const qsizetype at = handlesAt();
    const QPoint bar = centre(editor->tableChromeRect(at, MarkdownEditor::TablePart::AddRow));
    moveTo(*editor, bar);
    QTest::mousePress(editor->viewport(), Qt::LeftButton, Qt::NoModifier, bar);
    QTest::mouseRelease(editor->viewport(), Qt::LeftButton, Qt::NoModifier, bar);
    quiet(*editor);
    QString expected = kHandles;
    expected.replace(QStringLiteral("  <tr><td>fig</td><td>7</td></tr>\n"),
                     QStringLiteral("  <tr><td>fig</td><td>7</td></tr>\n  <tr><td></td><td></td></tr>\n"));
    EXPECT_EQ(editor->noteText().toStdString(), expected.toStdString());
    undoAll(*editor);
    EXPECT_EQ(editor->noteText().toStdString(), kHandles.toStdString());
}

TEST(HtmlTableHandlesTest, ADragOnAnAddBarAddsOneRowOrColumnForEachCellTravelled)
{
    auto editor = handlesEditor();
    const qsizetype at = handlesAt();
    // Two row heights down: two rows, one undo step.
    const qreal rowHeight = editor->tableCellRect(at, 3, 0).height();
    const QPoint start = centre(editor->tableChromeRect(at, MarkdownEditor::TablePart::AddRow));
    drag(*editor, start, start + QPoint(0, int(rowHeight * 2.4)));
    quiet(*editor);
    EXPECT_EQ(editor->noteText().count(QStringLiteral("<tr><td></td><td></td></tr>")), 2) << editor->noteText().toStdString();
    EXPECT_EQ(editor->cursorOffset(), editor->noteText().indexOf(QStringLiteral("<tr><td></td>")) + 8)
        << "the caret is in the first new cell";
    editor->grab();
    EXPECT_TRUE(editor->tableRenderedAt(at));
    QTest::keyClick(editor.get(), Qt::Key_Z, Qt::ControlModifier);
    EXPECT_EQ(editor->noteText().toStdString(), kHandles.toStdString()) << "one undo step";

    // And a column: <td></td> after every row's last cell, <th> in the header.
    editor = handlesEditor();
    const QPoint side = centre(editor->tableChromeRect(at, MarkdownEditor::TablePart::AddColumn));
    const qreal width = editor->tableCellRect(at, 1, 1).width();
    drag(*editor, side, side + QPoint(int(width * 1.6), 0));
    quiet(*editor);
    EXPECT_TRUE(editor->noteText().contains(QStringLiteral("<th>qty</th><th></th></tr>"))) << editor->noteText().toStdString();
    EXPECT_TRUE(editor->noteText().contains(QStringLiteral("<td>7</td><td></td></tr>")));
    EXPECT_EQ(editor->noteText().count(QStringLiteral("<td></td>")) + editor->noteText().count(QStringLiteral("<th></th>")), 4)
        << "one new cell in each of four rows";
    undoAll(*editor);
    EXPECT_EQ(editor->noteText().toStdString(), kHandles.toStdString());
}

TEST(HtmlTableHandlesTest, ARectangleCopiesAsATableAndTabSeparatedValues)
{
    auto editor = handlesEditor();
    const qsizetype at = handlesAt();
    drag(*editor, centre(editor->tableCellRect(at, 1, 0)), centre(editor->tableCellRect(at, 2, 1)));
    EXPECT_EQ(editor->cellSelectionRect(), QRect(0, 1, 2, 2));
    QTest::keyClick(editor.get(), Qt::Key_C, Qt::ControlModifier);
    const QMimeData *mime = QApplication::clipboard()->mimeData();
    ASSERT_NE(mime, nullptr);
    EXPECT_EQ(mime->text(), QStringLiteral("apple & co\t3\npear\t5")) << "what the cells show, not their tags";
    EXPECT_EQ(QString::fromUtf8(mime->data(QStringLiteral("text/markdown"))),
              QStringLiteral("| apple & co | 3   |\n| ---------- | --- |\n| pear       | 5   |"))
        << QString::fromUtf8(mime->data(QStringLiteral("text/markdown"))).toStdString();
    EXPECT_EQ(editor->noteText(), kHandles) << "a copy writes nothing";

    // Delete empties the cells' content and keeps their tags, as one step.
    QTest::keyClick(editor.get(), Qt::Key_Delete);
    quiet(*editor);
    QString cleared = kHandles;
    cleared.replace(QStringLiteral("<td>apple &amp; co</td><td>3</td>"), QStringLiteral("<td></td><td></td>"));
    cleared.replace(QStringLiteral("<td style=\"color:red\">pear</td><td><b>5</b></td>"),
                    QStringLiteral("<td style=\"color:red\"></td><td></td>"));
    EXPECT_EQ(editor->noteText(), cleared) << editor->noteText().toStdString();
    QTest::keyClick(editor.get(), Qt::Key_Z, Qt::ControlModifier);
    EXPECT_EQ(editor->noteText().toStdString(), kHandles.toStdString());
}

TEST(HtmlTableHandlesTest, PasteFillsFromTheCaretCellAndGrowsTheTableLikeItsSource)
{
    auto editor = handlesEditor();
    auto *mime = new QMimeData;
    mime->setText(QStringLiteral("a<b\tx\ty\nu\tv\tw"));
    QApplication::clipboard()->setMimeData(mime);
    editor->setCursorOffset(kHandles.indexOf(QStringLiteral("fig")) + 1); // the last row's first cell
    editor->grab();
    QTest::keyClick(editor.get(), Qt::Key_V, Qt::ControlModifier);
    quiet(*editor);
    const QString expected = QStringLiteral(
        "above\n\n<table class=\"t\">\n"
        "  <tr><th>name</th><th>qty</th><th></th></tr>\n"
        "  <tr id=\"a\"><td>apple &amp; co</td><td>3</td><td></td></tr>\n"
        "  <tr id=\"b\" class=\"x\"><td style=\"color:red\">pear</td><td><b>5</b></td><td></td></tr>\n"
        "  <tr><td>a&lt;b</td><td>x</td><td>y</td></tr>\n"
        "  <tr><td>u</td><td>v</td><td>w</td></tr>\n"
        "</table>\n\nbelow\n");
    EXPECT_EQ(editor->noteText().toStdString(), expected.toStdString());
    EXPECT_EQ(editor->cellSelectionRect(), QRect(0, 3, 3, 2)) << "what was pasted is selected";
    QTest::keyClick(editor.get(), Qt::Key_Z, Qt::ControlModifier);
    EXPECT_EQ(editor->noteText().toStdString(), kHandles.toStdString()) << "one undo step";

    // A pipe table pastes too (text/markdown first), its escaped pipes as pipes.
    auto *table = new QMimeData;
    table->setText(QStringLiteral("p\tq"));
    table->setData(QStringLiteral("text/markdown"), QByteArray("| p\\|1 | q |\n| --- | --- |\n| r | s |"));
    QApplication::clipboard()->setMimeData(table);
    editor->setCursorOffset(kHandles.indexOf(QStringLiteral("apple")) + 1);
    editor->grab();
    QTest::keyClick(editor.get(), Qt::Key_V, Qt::ControlModifier);
    quiet(*editor);
    QString filled = kHandles;
    filled.replace(QStringLiteral("<td>apple &amp; co</td><td>3</td>"), QStringLiteral("<td>p|1</td><td>q</td>"));
    filled.replace(QStringLiteral("<td style=\"color:red\">pear</td><td><b>5</b></td>"),
                   QStringLiteral("<td style=\"color:red\">r</td><td>s</td>"));
    EXPECT_EQ(editor->noteText().toStdString(), filled.toStdString()) << "attributes stay, the text goes in";
}

TEST(HtmlTableHandlesTest, ClearingACellTakesItsSpacesUnlessItSpansLines)
{
    const QString source = QStringLiteral(
        "<table>\n"
        "  <tr>\n    <th>h</th>\n    <th>i</th>\n  </tr>\n"
        "  <tr>\n    <td> one </td>\n    <td>\n      two\n    </td>\n  </tr>\n"
        "</table>\n\nbelow\n");
    auto editor = handlesEditor(source);
    const qsizetype at = handlesAt(source);
    drag(*editor, centre(editor->tableCellRect(at, 1, 0)), centre(editor->tableCellRect(at, 1, 1)));
    ASSERT_EQ(editor->cellSelectionRect(), QRect(0, 1, 2, 1));
    QTest::keyClick(editor.get(), Qt::Key_Delete);
    quiet(*editor);
    QString cleared = source;
    cleared.replace(QStringLiteral("<td> one </td>"), QStringLiteral("<td></td>"));
    cleared.replace(QStringLiteral("      two\n"), QStringLiteral("      \n"));
    EXPECT_EQ(editor->noteText().toStdString(), cleared.toStdString());
    QTest::keyClick(editor.get(), Qt::Key_Z, Qt::ControlModifier);
    EXPECT_EQ(editor->noteText().toStdString(), source.toStdString());

    // A new row and column keep the table's one-cell-a-line layout.
    auto *mime = new QMimeData;
    mime->setText(QStringLiteral("x\ty\tz\nu\tv\tw"));
    QApplication::clipboard()->setMimeData(mime);
    editor->setCursorOffset(source.indexOf(QStringLiteral("one")) + 1);
    editor->grab();
    QTest::keyClick(editor.get(), Qt::Key_V, Qt::ControlModifier);
    quiet(*editor);
    const QString grown = QStringLiteral(
        "<table>\n"
        "  <tr>\n    <th>h</th>\n    <th>i</th>\n    <th></th>\n  </tr>\n"
        "  <tr>\n    <td>x</td>\n    <td>\n      y\n    </td>\n    <td>z</td>\n  </tr>\n"
        "  <tr>\n    <td>u</td>\n    <td>v</td>\n    <td>w</td>\n  </tr>\n"
        "</table>\n\nbelow\n");
    EXPECT_EQ(editor->noteText().toStdString(), grown.toStdString());
}

TEST(HtmlTableHandlesTest, ShiftArrowsGrowARectangleAndEscapeDropsIt)
{
    auto editor = handlesEditor();
    editor->setCursorOffset(kHandles.indexOf(QStringLiteral("pear")) + 4); // the end of a cell: the edge Shift+Right needs
    editor->grab();
    QTest::keyClick(editor.get(), Qt::Key_Down, Qt::ShiftModifier);
    EXPECT_EQ(editor->cellSelectionRect(), QRect(0, 2, 1, 2));
    QTest::keyClick(editor.get(), Qt::Key_Right, Qt::ShiftModifier);
    EXPECT_EQ(editor->cellSelectionRect(), QRect(0, 2, 2, 2));
    QTest::keyClick(editor.get(), Qt::Key_Escape);
    EXPECT_FALSE(editor->cellSelectionRect().isValid());
    EXPECT_EQ(editor->noteText().toStdString(), kHandles.toStdString());
}

TEST(HtmlTableHandlesTest, OffMeansNoGripsOnAnHTMLTable)
{
    auto editor = handlesEditor();
    editor->setTableHandles(false);
    moveTo(*editor, centre(editor->tableCellRect(handlesAt(), 1, 0)));
    moveTo(*editor, centre(editor->tableChromeRect(handlesAt(), MarkdownEditor::TablePart::RowGrip, 1)));
    EXPECT_EQ(editor->hoveredTablePart(), MarkdownEditor::TablePart::None);
}

namespace {

const QString kText = QStringLiteral(
    "<p>Hello <b>bold</b> world</p>\n\n<ul>\n  <li>one</li>\n  <li>two</li>\n</ul>\n\nend\n");

} // namespace

TEST(HtmlTextTest, TagsHideAndTheCaretRevealsWhatItTouches)
{
    auto editor = editorWith(kText);
    editor->enterBlockAt(kText.indexOf(QStringLiteral("end")));
    const QString shown = editor->displayText();
    EXPECT_TRUE(shown.startsWith(QStringLiteral("Hello bold world\n"))) << shown.toStdString();
    EXPECT_FALSE(shown.contains(QLatin1Char('<'))) << shown.toStdString();
    EXPECT_TRUE(shown.contains(QStringLiteral("one\ntwo"))) << shown.toStdString();
    EXPECT_TRUE(editor->formatAt(kText.indexOf(QStringLiteral("bold")) + 1).fontWeight() == QFont::Bold);

    // In "bold": its two tags show, the paragraph's do not.
    editor->enterBlockAt(kText.indexOf(QStringLiteral("bold")) + 2);
    QString caret = editor->displayText();
    EXPECT_TRUE(caret.startsWith(QStringLiteral("Hello <b>bold</b> world\n"))) << caret.toStdString();
    // Touching "<p>": it shows.
    editor->enterBlockAt(3);
    caret = editor->displayText();
    EXPECT_TRUE(caret.startsWith(QStringLiteral("<p>Hello bold"))) << caret.toStdString();
    // Away again: hidden.
    editor->enterBlockAt(kText.indexOf(QStringLiteral("end")));
    EXPECT_EQ(editor->displayText(), shown);
}

TEST(HtmlTextTest, TypingInAParagraphAndAnItemEscapes)
{
    auto editor = editorWith(kText);
    const qsizetype world = kText.indexOf(QStringLiteral("world"));
    editor->enterBlockAt(world);
    QTest::keyClicks(editor.get(), QStringLiteral("a&b "));
    settle(*editor);
    editor->enterBlockAt(editor->noteText().indexOf(QStringLiteral("one")) + 3);
    QTest::keyClicks(editor.get(), QStringLiteral("<1>"));
    const QString expected = QStringLiteral(
        "<p>Hello <b>bold</b> a&amp;b world</p>\n\n<ul>\n  <li>one&lt;1&gt;</li>\n  <li>two</li>\n</ul>\n\nend\n");
    EXPECT_EQ(editor->noteText(), expected);
    settle(*editor);
    EXPECT_TRUE(editor->displayText().startsWith(QStringLiteral("Hello bold a&b world\n")))
        << editor->displayText().toStdString();

    undoAll(*editor);
    EXPECT_EQ(editor->noteText(), kText);
}

TEST(HtmlTextTest, EnterSplitsAParagraphAndAnItem)
{
    auto editor = editorWith(kText);
    editor->enterBlockAt(kText.indexOf(QStringLiteral("one")) + 2);
    QTest::keyClick(editor.get(), Qt::Key_Return);
    QTest::keyClicks(editor.get(), QStringLiteral("x"));
    editor->enterBlockAt(kText.indexOf(QStringLiteral(" world")));
    QTest::keyClick(editor.get(), Qt::Key_Return);
    const QString expected = QStringLiteral(
        "<p>Hello <b>bold</b></p>\n<p> world</p>\n\n<ul>\n  <li>on</li>\n  <li>xe</li>\n  <li>two</li>\n</ul>\n\nend\n");
    EXPECT_EQ(editor->noteText(), expected);
    undoAll(*editor);
    EXPECT_EQ(editor->noteText(), kText);
}

TEST(HtmlTextTest, AnEntityIsOneCharacter)
{
    const QString note = QStringLiteral("<p>a &amp; b &copy; c&nbsp;d &#x263A;</p>\n\nend\n");
    auto editor = editorWith(note);
    editor->enterBlockAt(note.indexOf(QStringLiteral("end")));
    EXPECT_TRUE(editor->displayText().startsWith(QStringLiteral("a & b © c d ☺\n")))
        << editor->displayText().toStdString();

    // The caret on an entity opens it into its source, as it does a `**`:
    // five steps through "&amp;", then it closes again.
    const qsizetype amp = note.indexOf(QStringLiteral("&amp;"));
    editor->enterBlockAt(amp);
    EXPECT_TRUE(editor->displayText().startsWith(QStringLiteral("a &amp; b © c\u00A0d ☺\n")))
        << editor->displayText().toStdString();
    QTest::keyClick(editor.get(), Qt::Key_Right);
    EXPECT_EQ(editor->cursorOffset(), amp + 1);
    QTest::keyClicks(editor.get(), QStringLiteral("x"));
    EXPECT_EQ(editor->noteText(), QStringLiteral("<p>a &xamp; b &copy; c&nbsp;d &#x263A;</p>\n\nend\n"));
    QTest::keyClick(editor.get(), Qt::Key_Z, Qt::ControlModifier);
    editor->enterBlockAt(amp + 5);
    EXPECT_TRUE(editor->displayText().startsWith(QStringLiteral("a &amp; b "))) << "the end touches it too";
    QTest::keyClick(editor.get(), Qt::Key_Right);
    EXPECT_EQ(editor->cursorOffset(), amp + 6);
    editor->grab(); // a paint settles what the caret now reveals
    EXPECT_TRUE(editor->displayText().startsWith(QStringLiteral("a & b © c\u00A0d ☺\n")))
        << editor->displayText().toStdString();
    undoAll(*editor);
    EXPECT_EQ(editor->noteText(), note);
}

TEST(HtmlTextTest, CRLFAndABOMSurviveAnEdit)
{
    const QString note = QStringLiteral("﻿title\r\n\r\n<p>a\r\n  b</p>\r\n\r\nend\r\n");
    auto editor = editorWith(note);
    editor->enterBlockAt(note.indexOf(QStringLiteral("end")));
    EXPECT_TRUE(editor->displayText().contains(QStringLiteral("a b"))) << editor->displayText().toStdString();
    editor->enterBlockAt(note.indexOf(QStringLiteral("a\r\n")) + 1);
    QTest::keyClicks(editor.get(), QStringLiteral("<"));
    EXPECT_EQ(editor->noteText(), QStringLiteral("﻿title\r\n\r\n<p>a&lt;\r\n  b</p>\r\n\r\nend\r\n"));
    undoAll(*editor);
    EXPECT_EQ(editor->noteText(), note);
}

TEST(HtmlTextTest, ListsHeadingsAndQuotesTakeTheMarkdownLook)
{
    const QString note = QStringLiteral(
        "<div>\n<h2>Title</h2>\n<blockquote>quoted</blockquote>\n<ol start=\"3\">\n<li>three</li>\n</ol>\n<hr>\n</div>\n\nend\n");
    auto editor = editorWith(note);
    editor->enterBlockAt(note.indexOf(QStringLiteral("end")));
    EXPECT_EQ(editor->displayText(), QStringLiteral("Title\nquoted\nthree\n\n\nend\n"));
    EXPECT_FALSE(editor->htmlImageSizeAt(note.indexOf(QStringLiteral("Title"))).isValid());
}

TEST(HtmlTextTest, HTMLHeadingsAreInTheOutlineLikeMarkdownOnes)
{
    const QString note = QStringLiteral("# Top\n\n<h2>Sub &amp; <i>x</i></h2>\n\nend\n");
    auto editor = editorWith(note);
    const QList<Heading> headings = editor->headings();
    ASSERT_EQ(headings.size(), 2);
    EXPECT_EQ(headings.at(1).level, 2);
    EXPECT_EQ(headings.at(1).text, QStringLiteral("Sub & x"));
    EXPECT_EQ(headings.at(1).range.start, note.indexOf(QStringLiteral("<h2>")));
    // And the editor's headings are the parser's, which is what the index stores.
    const Metadata metadata = Parser().parse(note, QStringLiteral("/vault/Note.md"));
    ASSERT_EQ(metadata.headings.size(), headings.size());
    EXPECT_EQ(metadata.headings.at(1).range.start, headings.at(1).range.start);
}

TEST(HtmlTableTest, AStyleOnACellKeepsTheTableEditable)
{
    const QString note = QStringLiteral(
        "<table>\n<tr><th style=\"text-align:right\">n</th><th style=\"Color: red; BACKGROUND-COLOR:#eee\">q</th></tr>\n"
        "<tr><td style=\"font-weight:bold\">a</td><td>3</td></tr>\n</table>\n");
    Decorations decorations = Parser().parseAll(note).decorations;
    expandEditableHTML(decorations, note);
    ASSERT_EQ(decorations.tables.size(), 1);
    EXPECT_EQ(decorations.tables.first().alignment.value(0), Qt::AlignRight);
    EXPECT_EQ(decorations.tables.first().alignment.value(1), Qt::AlignLeft);

    auto editor = editorWith(QStringLiteral("above\n\n") + note);
    EXPECT_TRUE(editor->tableRenderedAt(editor->noteText().indexOf(QStringLiteral(">a<")) + 1));

    // Anything else in a cell's style, or a style on a row, is still CSS for litehtml.
    for (const char *style : {"width:50%", "text-align:right; background:url(x.png)", "color"}) {
        const QString other = QStringLiteral("<table>\n<tr><td style=\"%1\">a</td></tr>\n</table>\n").arg(QLatin1String(style));
        Decorations plain = Parser().parseAll(other).decorations;
        expandEditableHTML(plain, other);
        EXPECT_TRUE(plain.tables.isEmpty()) << style;
    }
}

TEST(HtmlTableTest, ACellsOwnColourIsDrawn)
{
    const auto count = [](const QString &cell, const QColor &color) {
        auto editor = editorWith(QStringLiteral("above\n\n<table>\n<tr><td%1>apple</td></tr>\n</table>\n\nbelow\n").arg(cell));
        QTest::qWait(50);
        const QImage image = editor->viewport()->grab().toImage().convertToFormat(QImage::Format_RGB32);
        int n = 0;
        for (int y = 0; y < image.height(); ++y)
            for (int x = 0; x < image.width(); ++x)
                n += image.pixelColor(x, y) == color;
        return n;
    };
    const QColor red(255, 0, 0), green(0, 255, 0);
    EXPECT_EQ(count(QString(), red), 0);
    EXPECT_EQ(count(QString(), green), 0);
    EXPECT_GT(count(QStringLiteral(" style=\"background: #ff0000\""), red), 1000); // the cell's fill
    EXPECT_GT(count(QStringLiteral(" style=\"BACKGROUND-COLOR:rgb(255, 0, 0)\""), red), 1000);
    EXPECT_GT(count(QStringLiteral(" style=\"color:#00ff00\""), green), 20);        // the text
    EXPECT_EQ(count(QStringLiteral(" style=\"color:#00ff00\""), red), 0);
    EXPECT_EQ(count(QStringLiteral(" style=\"background:url(x.png)\""), red), 0);   // not an owned style
    // font-weight changes what is drawn, and a fill leaves the text readable
    EXPECT_NE(count(QStringLiteral(" style=\"color:#00ff00;font-weight:bold\""), green),
              count(QStringLiteral(" style=\"color:#00ff00\""), green));
    EXPECT_EQ(count(QStringLiteral(" style=\"color:#00ff00;font-weight:400\""), green),
              count(QStringLiteral(" style=\"color:#00ff00\""), green));
}

TEST(HtmlTextTest, VimStepsOverAnHTMLEntityAsOneCharacter)
{
    const QString note = QStringLiteral("<p>a &amp;b &copy;</p>\n\nend\n");
    auto editor = editorWith(note);
    editor->setVimMode(true);
    editor->setCursorOffset(3); // a
    QTest::keyClicks(editor.get(), QStringLiteral("ll"));
    EXPECT_EQ(editor->cursorOffset(), 5); // on the &
    QTest::keyClicks(editor.get(), QStringLiteral("l"));
    EXPECT_EQ(editor->cursorOffset(), 10); // b: past all of &amp;
    QTest::keyClicks(editor.get(), QStringLiteral("hh"));
    EXPECT_EQ(editor->cursorOffset(), 4);
    QTest::keyClicks(editor.get(), QStringLiteral("lx"));
    EXPECT_EQ(editor->noteText(), QStringLiteral("<p>a b &copy;</p>\n\nend\n"));
    undoAll(*editor);
    EXPECT_EQ(editor->noteText(), note);
}

#endif // NOTEAPPC_EXPECT_HTML_EDIT

TEST(MarkdownEntityTest, AReferenceShowsAsItsCharacterAndIsOneStep)
{
    const QString note = QStringLiteral("a &amp; b &copy; c `&amp;` \\&amp;\n\nend\n");
    auto editor = editorWith(note);
    editor->enterBlockAt(note.indexOf(QStringLiteral("end")));
    EXPECT_TRUE(editor->displayText().startsWith(QStringLiteral("a & b \u00A9 c &amp; &amp;\n")))
        << editor->displayText().toStdString();

    const qsizetype amp = note.indexOf(QStringLiteral("&amp;"));
    editor->enterBlockAt(amp);
    EXPECT_TRUE(editor->displayText().startsWith(QStringLiteral("a &amp; b \u00A9 c &amp; &amp;\n")))
        << "the caret on it opens it: " << editor->displayText().toStdString();
    QTest::keyClick(editor.get(), Qt::Key_Right);
    EXPECT_EQ(editor->cursorOffset(), amp + 1);
    QTest::keyClick(editor.get(), Qt::Key_Left);
    EXPECT_EQ(editor->cursorOffset(), amp);
    // Away from it, closed again, and a click-sized step on is one step.
    editor->enterBlockAt(note.indexOf(QStringLiteral("end")));
    EXPECT_TRUE(editor->displayText().startsWith(QStringLiteral("a & b \u00A9 c")));
    editor->enterBlockAt(amp - 1);
    QTest::keyClick(editor.get(), Qt::Key_Right);
    editor->grab();
    EXPECT_EQ(editor->cursorOffset(), amp) << "arriving opens it";
    EXPECT_TRUE(editor->displayText().startsWith(QStringLiteral("a &amp; b "))) << editor->displayText().toStdString();
    editor->enterBlockAt(amp + 5);
    QTest::keyClick(editor.get(), Qt::Key_Backspace);
    QString expected = note;
    expected.remove(amp + 4, 1);
    EXPECT_EQ(editor->noteText(), expected) << "open, so a Backspace takes a character of its source";
    undoAll(*editor);
    EXPECT_EQ(editor->noteText(), note);

    // A numeric one opens like a named one, anywhere from its first character
    // to its last.
    editor->setNoteText(QStringLiteral("x &#x2014; y\n\nend\n"));
    editor->enterBlockAt(4);
    EXPECT_TRUE(editor->displayText().startsWith(QStringLiteral("x &#x2014; y")));
    editor->enterBlockAt(11);
    EXPECT_TRUE(editor->displayText().startsWith(QStringLiteral("x \u2014 y")));
    editor->setNoteText(note);

    // Source mode shows every byte as written.
    editor->setMode(MarkdownEditor::Mode::Source);
    EXPECT_TRUE(editor->displayText().startsWith(QStringLiteral("a &amp; b &copy;"))) << editor->displayText().toStdString();
}

TEST(MarkdownEntityTest, VimStepsOverOneAsOneCharacter)
{
    const QString note = QStringLiteral("a &amp;b &copy;\n\nend\n");
    auto editor = editorWith(note);
    editor->setVimMode(true);
    editor->setCursorOffset(0);
    QTest::keyClicks(editor.get(), QStringLiteral("ll"));
    EXPECT_EQ(editor->cursorOffset(), 2);
    QTest::keyClicks(editor.get(), QStringLiteral("l"));
    EXPECT_EQ(editor->cursorOffset(), 7); // b
    QTest::keyClicks(editor.get(), QStringLiteral("h"));
    EXPECT_EQ(editor->cursorOffset(), 2);
    QTest::keyClicks(editor.get(), QStringLiteral("x"));
    EXPECT_EQ(editor->noteText(), QStringLiteral("a b &copy;\n\nend\n"));
    // The last character of the line is the entity: l stops on it, x takes all of it.
    editor->setNoteText(QStringLiteral("a &copy;\n\nend\n"));
    editor->setCursorOffset(0);
    QTest::keyClicks(editor.get(), QStringLiteral("lllll"));
    EXPECT_EQ(editor->cursorOffset(), 2);
    QTest::keyClicks(editor.get(), QStringLiteral("x"));
    EXPECT_EQ(editor->noteText(), QStringLiteral("a \n\nend\n"));
}

#ifdef NOTEAPPC_EXPECT_HTML
TEST(HtmlBoundaryTest, StyleAndImagesStayLitehtml)
{
    for (const QString &block : {QStringLiteral("<div style=\"color: red\">styled</div>"),
                                 QStringLiteral("<p>an image <img src=\"x.png\"></p>"),
                                 QStringLiteral("<details><summary>s</summary>body</details>"),
                                 QStringLiteral("<p>one</p><p>two on the same line</p>"),
                                 QStringLiteral("<table><tr><td><table><tr><td>x</td></tr></table></td></tr></table>"),
                                 QStringLiteral("<div><!-- a comment -->text</div>"),
                                 QStringLiteral("<p><b><i>misnested</b></i></p>")}) {
        const QString note = QStringLiteral("above\n\n") + block + QStringLiteral("\n\nbelow\n");
        auto editor = editorWith(note);
        const qsizetype start = note.indexOf(block);
        EXPECT_TRUE(editor->htmlImageSizeAt(start).isValid()) << block.toStdString();
        EXPECT_FALSE(editor->tableRenderedAt(start));
        // Clicked into, it shows its source, as before.
        editor->enterBlockAt(start + 2);
        EXPECT_TRUE(editor->displayText().contains(block)) << editor->displayText().toStdString();
    }
}
#endif

TEST(HtmlBoundaryTest, WithTheFeatureOffHTMLShowsAsSource)
{
    auto editor = editorWith(QStringLiteral("<p>a &amp; b</p>\n\nend\n"));
    MarkdownEditor::Features off;
    off.HTMLblocks = false;
    editor->setFeatures(off);
    editor->enterBlockAt(editor->noteText().indexOf(QStringLiteral("end")));
    EXPECT_TRUE(editor->displayText().startsWith(QStringLiteral("<p>a &amp; b</p>")))
        << editor->displayText().toStdString();
}

#ifndef NOTEAPPC_EXPECT_HTML_EDIT
TEST(HtmlBoundaryTest, WithoutGumboHTMLIsAsBefore)
{
    // The minimal build: no structure pass, no grid; typing writes what is typed.
    auto editor = editorWith(kTable);
    EXPECT_FALSE(editor->tableRenderedAt(kTable.indexOf(QStringLiteral("apple"))));
    editor->setCursorOffset(kTable.indexOf(QStringLiteral("apple")) + 5);
    QTest::keyClicks(editor.get(), QStringLiteral("&"));
    QString expected = kTable;
    expected.replace(QStringLiteral("apple"), QStringLiteral("apple&"));
    EXPECT_EQ(editor->noteText(), expected);
}
#endif

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("noteAppC-test"));
    QCoreApplication::setApplicationName(QStringLiteral("noteAppC-test"));
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
