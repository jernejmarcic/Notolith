#include "NoteEdit.h"

#include <QAbstractItemView>
#include <QCompleter>
#include <QKeyEvent>
#include <QResizeEvent>
#include <QScrollBar>
#include <QTextBlock>
#include <QStringListModel>
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

    m_completer = new QCompleter(new QStringListModel(this), this);
    m_completer->setWidget(this);
    m_completer->setCompletionMode(QCompleter::PopupCompletion);
    m_completer->setCaseSensitivity(Qt::CaseInsensitive);
    m_completer->setFilterMode(Qt::MatchContains);
    m_completer->popup()->setObjectName(QStringLiteral("completerPopup"));
    connect(m_completer, qOverload<const QString &>(&QCompleter::activated),
            this, &NoteEdit::insertCompletion);

    connect(this, &NoteEdit::textChanged, this, &NoteEdit::updateSuggestions);
    connect(this, &NoteEdit::cursorPositionChanged, this, &NoteEdit::updateSuggestions);

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

void NoteEdit::setLinkTargets(const QStringList &targets)
{
    if (auto *model = qobject_cast<QStringListModel *>(m_completer->model()))
        model->setStringList(targets);
}

std::optional<QString> NoteEdit::wikilinkPrefix() const
{
    const QTextCursor cursor = textCursor();
    const QString line = cursor.block().text().left(cursor.positionInBlock());

    const qsizetype open = line.lastIndexOf(QLatin1String("[["));
    if (open < 0)
        return std::nullopt;
    const QStringView typed = QStringView(line).mid(open + 2);
    // A closed link, or one that already carries an alias, is finished.
    if (typed.contains(QLatin1String("]]")) || typed.contains(QLatin1Char('|')))
        return std::nullopt;
    return typed.toString();
}

void NoteEdit::updateSuggestions()
{
    const auto prefix = wikilinkPrefix();
    if (!prefix) {
        m_completer->popup()->hide();
        return;
    }

    m_completer->setCompletionPrefix(*prefix);
    if (m_completer->completionCount() == 0) {
        m_completer->popup()->hide();
        return;
    }
    m_completer->popup()->setCurrentIndex(m_completer->completionModel()->index(0, 0));

    QRect anchor = cursorRect();
    anchor.setWidth(m_completer->popup()->sizeHintForColumn(0)
                    + m_completer->popup()->verticalScrollBar()->sizeHint().width() + 24);
    m_completer->complete(anchor);
}

void NoteEdit::insertCompletion(const QString &completion)
{
    QTextCursor cursor = textCursor();
    const QString prefix = m_completer->completionPrefix();
    cursor.movePosition(QTextCursor::Left, QTextCursor::KeepAnchor, int(prefix.size()));

    // The closing brackets are usually already there from the auto-pair.
    const QString after = cursor.block().text().mid(cursor.positionInBlock() + int(prefix.size()));
    const bool closed = after.startsWith(QLatin1String("]]"));
    cursor.insertText(closed ? completion : completion + QLatin1String("]]"));
    if (closed)
        cursor.movePosition(QTextCursor::Right, QTextCursor::MoveAnchor, 2);
    setTextCursor(cursor);
}

void NoteEdit::keyPressEvent(QKeyEvent *event)
{
    if (m_completer->popup()->isVisible()) {
        switch (event->key()) {
        case Qt::Key_Return:
        case Qt::Key_Enter:
        case Qt::Key_Tab:
        case Qt::Key_Up:
        case Qt::Key_Down:
        case Qt::Key_Escape:
            event->ignore(); // the popup handles it
            return;
        default:
            break;
        }
    }

    QPlainTextEdit::keyPressEvent(event);

    // Typing the second bracket auto-closes the pair, the way Obsidian does.
    if (event->text() == QLatin1String("[")) {
        QTextCursor cursor = textCursor();
        const QString line = cursor.block().text().left(cursor.positionInBlock());
        if (line.endsWith(QLatin1String("[["))) {
            cursor.insertText(QStringLiteral("]]"));
            cursor.movePosition(QTextCursor::Left, QTextCursor::MoveAnchor, 2);
            setTextCursor(cursor);
        }
    }
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
