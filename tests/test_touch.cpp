// The editor on a touchscreen. Its own binary: a test touch device is
// registered for the whole process, and every editor made after it would
// grab the touch gesture - not what the rest of the editor tests are about.

#include <gtest/gtest.h>

#include <QApplication>
#include <QMouseEvent>
#include <QPointingDevice>
#include <QScrollBar>
#include <QScroller>
#include <QStyleHints>
#include <QTest>

#include <memory>

#include "MarkdownEditor.h"

using noteAppC::MarkdownEditor;

namespace {

QPointingDevice *touchscreen = nullptr;

/// Where `offset` is drawn, asked of the editor the way an input method asks
/// for the caret, rather than guessed from a layout.
QPoint pointAt(MarkdownEditor &editor, qsizetype offset)
{
    editor.setCursorOffset(offset);
    const QPoint at = static_cast<QWidget &>(editor).inputMethodQuery(Qt::ImCursorRectangle).toRectF().center().toPoint();
    editor.setCursorOffset(0);
    return at + QPoint(1, 0);
}

void press(MarkdownEditor &editor, QPoint at, const QPointingDevice *device)
{
    QMouseEvent event(QEvent::MouseButtonPress, at, editor.viewport()->mapToGlobal(at), Qt::LeftButton,
                      Qt::LeftButton, Qt::NoModifier, device);
    QCoreApplication::sendEvent(editor.viewport(), &event);
}

void moveTo(MarkdownEditor &editor, QPoint at, const QPointingDevice *device)
{
    QMouseEvent event(QEvent::MouseMove, at, editor.viewport()->mapToGlobal(at), Qt::NoButton,
                      Qt::LeftButton, Qt::NoModifier, device);
    QCoreApplication::sendEvent(editor.viewport(), &event);
}

std::unique_ptr<MarkdownEditor> editorWith(const QString &text)
{
    auto editor = std::make_unique<MarkdownEditor>();
    editor->resize(600, 400);
    editor->setNoteText(text);
    editor->show();
    return editor;
}

} // namespace

TEST(TouchTest, ADragOnATouchscreenScrollsWithMomentum)
{
    auto editor = editorWith(QStringLiteral("text\n"));
    // hasScroller(), not grabbedGesture(): that one answers the same for a
    // widget nobody grabbed anything on.
    EXPECT_TRUE(QScroller::hasScroller(editor->viewport()));
}

TEST(TouchTest, AHeldFingerSelectsTheWordUnderIt)
{
    const QString text = QStringLiteral("alpha bravo charlie\n");
    auto editor = editorWith(text);
    const QPoint bravo = pointAt(*editor, text.indexOf(QStringLiteral("bravo")) + 2);
    ASSERT_FALSE(bravo.isNull());

    press(*editor, bravo, touchscreen);
    moveTo(*editor, bravo + QPoint(4, 3), touchscreen); // a finger is never quite still
    QTest::qWait(700);
    EXPECT_EQ(editor->selectedText(), QStringLiteral("bravo"));
}

TEST(TouchTest, AFingerThatMovesOrAMouseSelectsNoWordOnItsOwn)
{
    const QString text = QStringLiteral("alpha bravo charlie\n");
    auto editor = editorWith(text);
    const QPoint bravo = pointAt(*editor, text.indexOf(QStringLiteral("bravo")) + 2);

    press(*editor, bravo, touchscreen);
    moveTo(*editor, bravo + QPoint(60, 0), touchscreen);
    QTest::qWait(700);
    EXPECT_NE(editor->selectedText(), QStringLiteral("bravo"));

    auto mouseEditor = editorWith(text);
    press(*mouseEditor, bravo, QPointingDevice::primaryPointingDevice());
    QTest::qWait(700);
    EXPECT_TRUE(mouseEditor->selectedText().isEmpty());
}

TEST(TouchTest, AFingersSelectionHasHandlesThatDragItsEnds)
{
    const QString text = QStringLiteral("alpha bravo charlie delta\n");
    auto editor = editorWith(text);
    const QPoint bravo = pointAt(*editor, text.indexOf(QStringLiteral("bravo")) + 2);
    // Measured before selecting: pointAt moves the caret.
    const QPoint charlie = pointAt(*editor, text.indexOf(QStringLiteral("charlie")) + 7);
    press(*editor, bravo, touchscreen);
    QTest::qWait(700);
    ASSERT_EQ(editor->selectedText(), QStringLiteral("bravo"));
    const QRectF end = editor->touchSelectionHandle(true);
    ASSERT_FALSE(end.isEmpty()) << "a long press leaves handles";

    // Drag the end handle to the end of "charlie", the finger a line below
    // the text as it is on the drop.
    const QPoint grab = end.center().toPoint();
    const QPoint target(charlie.x(), grab.y());
    {
        QMouseEvent release(QEvent::MouseButtonRelease, bravo, editor->viewport()->mapToGlobal(bravo), Qt::LeftButton,
                            Qt::NoButton, Qt::NoModifier, touchscreen);
        QCoreApplication::sendEvent(editor->viewport(), &release);
    }
    press(*editor, grab, touchscreen);
    moveTo(*editor, target, touchscreen);
    QMouseEvent release(QEvent::MouseButtonRelease, target, editor->viewport()->mapToGlobal(target), Qt::LeftButton,
                        Qt::NoButton, Qt::NoModifier, touchscreen);
    QCoreApplication::sendEvent(editor->viewport(), &release);
    EXPECT_EQ(editor->selectedText(), QStringLiteral("bravo charlie"));

    // A mouse press anywhere puts the handles away.
    press(*editor, bravo, QPointingDevice::primaryPointingDevice());
    EXPECT_TRUE(editor->touchSelectionHandle(true).isEmpty());
}

TEST(TouchTest, AFingerReachesATaskBoxAMouseWouldMiss)
{
    const QString text = QStringLiteral("- [ ] buy milk\n\nafter\n");
    const qsizetype word = text.indexOf(QStringLiteral("buy"));
    const auto ticks = [&](int dx, const QPointingDevice *device) {
        auto editor = editorWith(text);
        const QPoint label = pointAt(*editor, word);
        press(*editor, QPoint(label.x() - dx, label.y()), device);
        return editor->noteText().startsWith(QStringLiteral("- [x]"));
    };
    // The farthest left a mouse still ticks it: the box's edge plus its slop.
    int edge = 0;
    for (int dx = 1; dx < 80; ++dx) {
        if (ticks(dx, QPointingDevice::primaryPointingDevice()))
            edge = dx;
    }
    ASSERT_GT(edge, 0) << "a mouse never reached the box";
    EXPECT_FALSE(ticks(edge + 4, QPointingDevice::primaryPointingDevice()));
    EXPECT_TRUE(ticks(edge + 4, touchscreen)) << "a finger reaches a little further";
}

namespace {

const QString kGrid = QStringLiteral(
    "para\n\n| a | b | c |\n| --- | :-: | --: |\n| 1 | 2 | 3 |\n| 4 | 5 | 6 |\n| 7 | 8 | 9 |\n\nend\n");

std::unique_ptr<MarkdownEditor> gridEditor()
{
    auto editor = editorWith(kGrid);
    editor->setMode(MarkdownEditor::Mode::LivePreview);
    editor->setCursorOffset(0);
    editor->setFocus();
    return editor;
}

QPoint at(const QRectF &rect) { return rect.center().toPoint(); }

QString pipeRows(const MarkdownEditor &editor)
{
    const QString text = editor.noteText();
    const qsizetype start = text.indexOf(QStringLiteral("| "));
    return text.mid(start, text.indexOf(QStringLiteral("\n\n"), start) - start);
}

/// One finger down, moved along `path`, and up, as touch events (which the
/// editor sees as a touch screen's synthesized mouse events).
void finger(MarkdownEditor &editor, const QList<QPoint> &path, int holdMs = 0, bool quick = false)
{
    // Not within a double-click of the last one, unless that is the point.
    if (!quick)
        QTest::qWait(QGuiApplication::styleHints()->mouseDoubleClickInterval() + 30);
    QWidget *viewport = editor.viewport();
    auto sequence = QTest::touchEvent(viewport, touchscreen);
    sequence.press(0, path.first(), viewport).commit();
    if (holdMs > 0)
        QTest::qWait(holdMs);
    for (qsizetype i = 1; i < path.size(); ++i)
        sequence.move(0, path.at(i), viewport).commit();
    sequence.release(0, path.last(), viewport).commit();
    QTest::qWait(30);
    editor.viewport()->repaint(); // the relayout an edit leaves for the next paint
}

} // namespace

TEST(TouchTest, AFingerDraggedUpTheNoteScrollsItAndSelectsNothing)
{
    // Jernej on PADD 2026-10-19: "u should add finger scrolling rn it selects
    // stuff". Real touch events, as the compositor sends them, not mouse
    // events that merely name a touch screen.
    QString text;
    for (int i = 0; i < 200; ++i)
        text += QStringLiteral("Line %1 of a note long enough to scroll.\n").arg(i);
    auto editor = editorWith(text);
    QTest::qWait(QGuiApplication::styleHints()->mouseDoubleClickInterval() + 30);
    QWidget *viewport = editor->viewport();
    auto sequence = QTest::touchEvent(viewport, touchscreen);
    QPoint at(300, 330);
    sequence.press(0, at, viewport).commit();
    for (int i = 0; i < 15; ++i) {
        at.ry() -= 18;
        QTest::qWait(16);
        sequence.move(0, at, viewport).commit();
    }
    sequence.release(0, at, viewport).commit();
    EXPECT_TRUE(QTest::qWaitFor([&] { return editor->verticalScrollBar()->value() > 100; }, 2000))
        << "scrolled to " << editor->verticalScrollBar()->value();
    EXPECT_TRUE(editor->selectedText().isEmpty()) << editor->selectedText().left(40).toStdString();
}

TEST(TouchTest, AFingerStillScrollsOnceTheGPUViewportIsOn)
{
    // setGPUacceleration() swaps the viewport, and the touch grab was on the
    // old one.
    auto editor = editorWith(QStringLiteral("text\n"));
    editor->setGPUacceleration(true);
    if (!editor->GPUacceleration())
        GTEST_SKIP() << "no OpenGL here";
    EXPECT_TRUE(QScroller::hasScroller(editor->viewport()));
    EXPECT_TRUE(editor->viewport()->acceptDrops());
}

TEST(TouchTableTest, AFirstTapBringsTheGripsUpAndTheNextOneIsTheirs)
{
    auto editor = gridEditor();
    const qsizetype table = kGrid.indexOf(QStringLiteral("| a"));
    EXPECT_FALSE(editor->tableChromeShown());
    const QPoint grip = at(editor->tableChromeRect(table, MarkdownEditor::TablePart::RowGrip, 2));
    finger(*editor, {grip});
    EXPECT_FALSE(editor->cellSelectionRect().isValid()) << "no grips yet: a tap on the edge is only a tap";

    finger(*editor, {at(editor->tableCellRect(table, 2, 1))});
    EXPECT_TRUE(editor->tableChromeShown());
    const QPoint rowGrip = at(editor->tableChromeRect(table, MarkdownEditor::TablePart::RowGrip, 2));
    finger(*editor, {rowGrip}, 0, true); // quick: Qt hands it over as a double click
    EXPECT_EQ(editor->cellSelectionRect(), QRect(0, 2, 3, 1)) << "a tap on a grip selects the row";
    EXPECT_EQ(editor->noteText(), kGrid);
}

TEST(TouchTableTest, AFingerDragsAGripToReorderAndAddBarsToAddRows)
{
    auto editor = gridEditor();
    const qsizetype table = kGrid.indexOf(QStringLiteral("| a"));
    finger(*editor, {at(editor->tableCellRect(table, 1, 1))}); // brings the chrome up
    const QRectF last = editor->tableCellRect(table, 3, 0);
    finger(*editor, {at(editor->tableChromeRect(table, MarkdownEditor::TablePart::RowGrip, 1)),
                     at(editor->tableChromeRect(table, MarkdownEditor::TablePart::RowGrip, 1)) + QPoint(20, 20),
                     QPoint(int(last.center().x()), int(last.bottom()) - 2)});
    EXPECT_TRUE(pipeRows(*editor).contains(QStringLiteral("| 4   |  5  |   6 |\n| 7   |  8  |   9 |\n| 1   |  2  |   3 |")))
        << pipeRows(*editor).toStdString();

    finger(*editor, {at(editor->tableCellRect(table, 1, 1))});
    const QPoint bar = at(editor->tableChromeRect(table, MarkdownEditor::TablePart::AddRow));
    const qreal rowHeight = editor->tableCellRect(table, 3, 0).height();
    finger(*editor, {bar, bar + QPoint(0, 8), bar + QPoint(0, int(rowHeight * 2.5))});
    EXPECT_EQ(pipeRows(*editor).split(QLatin1Char('\n')).size(), 7) << "two rows added by one drag";
    finger(*editor, {at(editor->tableCellRect(table, 1, 1))});
    finger(*editor, {at(editor->tableChromeRect(table, MarkdownEditor::TablePart::AddRow))});
    EXPECT_EQ(pipeRows(*editor).split(QLatin1Char('\n')).size(), 8) << "a tap adds one";
}

TEST(TouchTableTest, ALongPressInACellStartsARectangleAndAPlainDragDoesNot)
{
    auto editor = gridEditor();
    const qsizetype table = kGrid.indexOf(QStringLiteral("| a"));
    const QPoint from = at(editor->tableCellRect(table, 1, 0));
    const QPoint to = at(editor->tableCellRect(table, 2, 1));
    finger(*editor, {from, from + QPoint(20, 10), to});
    EXPECT_FALSE(editor->cellSelectionRect().isValid()) << "a finger that moves at once is a scroll, not a selection";

    finger(*editor, {from, from + QPoint(1, 1), to}, 700);
    EXPECT_EQ(editor->cellSelectionRect(), QRect(0, 1, 2, 2)) << "held, then dragged across cells";
    EXPECT_EQ(editor->noteText(), kGrid);
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    // Before any editor exists: the editor looks for a touchscreen once.
    touchscreen = QTest::createTouchDevice();
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
