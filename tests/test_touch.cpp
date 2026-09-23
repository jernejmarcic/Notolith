// The editor on a touchscreen. Its own binary: a test touch device is
// registered for the whole process, and every editor made after it would
// grab the touch gesture - not what the rest of the editor tests are about.

#include <gtest/gtest.h>

#include <QApplication>
#include <QMouseEvent>
#include <QPointingDevice>
#include <QScroller>
#include <QTest>

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

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    // Before any editor exists: the editor looks for a touchscreen once.
    touchscreen = QTest::createTouchDevice();
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
