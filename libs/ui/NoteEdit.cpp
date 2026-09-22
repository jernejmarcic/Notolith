#include "NoteEdit.h"

#include <QResizeEvent>
#include <QScrollBar>
#include <QTextBlock>
#include <QTextCursor>

namespace noteAppC {

NoteEdit::NoteEdit(QWidget *parent)
    : QPlainTextEdit(parent)
{
    setFrameStyle(QFrame::NoFrame);
    setLineWrapMode(QPlainTextEdit::WidgetWidth);
    setCursorWidth(2);
    setTabStopDistance(4 * fontMetrics().horizontalAdvance(QLatin1Char(' ')));
    setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    connect(verticalScrollBar(), &QScrollBar::rangeChanged, this, [this] { updateMargins(); });
    updateMargins();
}

void NoteEdit::setNoteText(const QString &text)
{
    setPlainText(text);
    applyLineHeight();
    document()->setModified(false);
}

void NoteEdit::setReadableLineLength(bool on)
{
    if (m_readable == on)
        return;
    m_readable = on;
    updateMargins();
}

void NoteEdit::setContentWidth(int pixels)
{
    m_contentWidth = qMax(200, pixels);
    updateMargins();
}

void NoteEdit::setTopPadding(int pixels)
{
    m_verticalPadding = qMax(0, pixels);
    updateMargins();
}

void NoteEdit::setLineHeightPercent(int percent)
{
    m_lineHeightPercent = qBound(100, percent, 300);
    applyLineHeight();
}

void NoteEdit::resizeEvent(QResizeEvent *event)
{
    QPlainTextEdit::resizeEvent(event);
    updateMargins();
}

void NoteEdit::updateMargins()
{
    const int scrollbar = verticalScrollBar()->isVisible() ? verticalScrollBar()->width() : 0;
    const int available = width() - scrollbar;

    int side = m_minimumSideMargin;
    if (m_readable && available > m_contentWidth + 2 * m_minimumSideMargin)
        side = (available - m_contentWidth) / 2;

    setViewportMargins(side, m_verticalPadding, side, m_verticalPadding);

    if (side != m_sideMargin) {
        m_sideMargin = side;
        emit sideMarginChanged(side);
    }
}

void NoteEdit::applyLineHeight()
{
    // Formatting only - the characters, and therefore what gets saved, are
    // untouched.
    QTextBlockFormat format;
    format.setLineHeight(m_lineHeightPercent, QTextBlockFormat::ProportionalHeight);

    const bool modified = document()->isModified();
    QTextCursor cursor(document());
    cursor.beginEditBlock();
    cursor.select(QTextCursor::Document);
    cursor.mergeBlockFormat(format);
    cursor.endEditBlock();
    document()->setModified(modified);
}

} // namespace noteAppC
