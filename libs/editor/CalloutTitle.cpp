/// A top-level callout's title, edited where it is drawn: a click on it opens
/// a line edit over exactly that text (the property editor's pattern,
/// BlockRenderers.cpp), and one `applyEdit` on the title's source range
/// commits it. The `> [!type]` marker stays hidden throughout.
#include "MarkdownEditor.h"

#include <QFontMetricsF>
#include <QLineEdit>
#include <QScrollBar>

#include <algorithm>

namespace noteAppC {

QRectF MarkdownEditor::calloutTitleRect(const BlockView &view, qreal left, qreal top) const
{
    const CalloutSpan *callout = view.callout;
    // `:::` containers and nested callouts have no title range of their own.
    if (!callout || callout->depth != 0 || view.piece != 0 || !callout->titleRange.isValid()
        || m_text.at(callout->marker.start) != QLatin1Char('['))
        return {};
    const QFontMetricsF metrics(view.calloutStyle.titleFont(m_theme));
    const qreal height = metrics.height();
    // The same box paintCallout draws the text in: from the icon to the fold arrow.
    const qreal titleLeft = left + height + 8;
    const qreal room = qMax(qreal(0), left + contentWidth() + 14 - 36 - titleLeft);
    const qreal width = qMin(room, qMax(qreal(140), metrics.horizontalAdvance(view.callout->title) + 12));
    return QRectF(titleLeft, top, width, height * 1.4);
}

QRectF MarkdownEditor::calloutTitleRectAt(qsizetype offset) const
{
    const qsizetype index = blockAt(offset);
    if (index < 0 || index >= qsizetype(m_blocks.size()))
        return {};
    const BlockView &view = m_blocks[size_t(index)];
    return calloutTitleRect(view, contentLeft(), view.y - verticalScrollBar()->value());
}

bool MarkdownEditor::pressCalloutTitle(const QPoint &point)
{
    // Reading draws the title but never edits it.
    if (m_mode == Mode::Reading)
        return false;
    const qreal scroll = verticalScrollBar()->value();
    const qreal left = contentLeft();
    for (const BlockView &view : m_blocks) {
        const qreal top = view.y - scroll;
        if (top + view.height < 0 || !view.laidOut || !view.callout)
            continue;
        if (top > viewport()->height())
            break;
        const QRectF rect = calloutTitleRect(view, left, top);
        if (rect.contains(point)) {
            openCalloutTitleEditor(*view.callout, rect, view);
            return true;
        }
    }
    return false;
}

void MarkdownEditor::openCalloutTitleEditor(const CalloutSpan &callout, const QRectF &rect,
                                            const BlockView &view)
{
    finishPropertyEdit(true); // also closes a title already open
    auto *edit = new QLineEdit(viewport());
    edit->setObjectName(QStringLiteral("calloutTitleEdit"));
    edit->setFrame(false);
    edit->setFont(view.calloutStyle.titleFont(m_theme));
    // The panel's own colour, so the field looks like the header it sits in: the
    // callout background is translucent over the page.
    const QColor page = m_theme.color(QStringLiteral("--background-primary"));
    const QColor tint = view.calloutStyle.background;
    const auto mix = [&tint](int below, int above) { return int(below + (above - below) * tint.alphaF() + 0.5); };
    QPalette palette = edit->palette();
    palette.setColor(QPalette::Base, QColor(mix(page.red(), tint.red()), mix(page.green(), tint.green()),
                                            mix(page.blue(), tint.blue())));
    palette.setColor(QPalette::Text, view.calloutStyle.headingColor);
    palette.setColor(QPalette::Highlight, m_theme.color(QStringLiteral("--interactive-accent")));
    edit->setPalette(palette);
    // QLineEdit insets its text by 2 px; start that far left so the text stays put.
    edit->setGeometry(rect.adjusted(-2, 0, 0, 0).toRect());
    edit->setText(m_text.mid(callout.titleRange.start, callout.titleRange.length()).trimmed());
    edit->installEventFilter(this);
    // Enter, or the focus going elsewhere, keeps what was typed.
    connect(edit, &QLineEdit::editingFinished, this, [this] { finishCalloutTitleEdit(true); });
    m_calloutTitleEdit = edit;
    m_calloutTitleAt = callout.range.start;
    viewport()->update(); // the drawn title steps aside
    edit->show();
    edit->setFocus();
}

void MarkdownEditor::finishCalloutTitleEdit(bool commit)
{
    if (!m_calloutTitleEdit)
        return;
    // Cleared first: hiding the editor moves the focus, which finishes it again.
    QLineEdit *edit = m_calloutTitleEdit;
    m_calloutTitleEdit = nullptr;
    const QString typed = edit->text().trimmed();
    // Only a field that held the focus hands it back: one closed by an edit
    // from another tab must not take the focus off whatever is being typed in.
    const bool focused = edit->hasFocus();
    edit->removeEventFilter(this);
    edit->disconnect(this);
    edit->hide();
    edit->deleteLater();
    if (focused)
        setFocus();
    viewport()->update();
    if (!commit)
        return;

    // By offset, not by pointer: a reparse has replaced the spans.
    const auto found = std::find_if(m_decorations.callouts.cbegin(), m_decorations.callouts.cend(),
                                    [this](const CalloutSpan &callout) {
                                        return callout.range.start == m_calloutTitleAt;
                                    });
    if (found == m_decorations.callouts.cend() || !found->titleRange.isValid())
        return;
    // The title without the whitespace (a CR, say) the range may trail with.
    const Range old = found->titleRange;
    const qsizetype oldLength = m_text.mid(old.start, old.length()).trimmed().size();
    if (typed == m_text.mid(old.start, oldLength))
        return;
    // `> [!note]` followed directly by the line end has no gap to put it in.
    const bool spaced = old.start > 0 && (m_text.at(old.start - 1) == QLatin1Char(' ')
                                          || m_text.at(old.start - 1) == QLatin1Char('\t'));
    const QString written = oldLength == 0 && !typed.isEmpty() && !spaced ? QLatin1Char(' ') + typed : typed;

    const qsizetype delta = written.size() - oldLength;
    const auto moved = [&](qsizetype at) { return at >= old.start + oldLength ? at + delta : qMin(at, old.start + written.size()); };
    const qsizetype anchor = moved(m_anchor);
    const qsizetype caret = moved(m_cursor);
    pushUndo();
    m_lastEdit.invalidate();
    applyEdit(old.start, oldLength, written);
    setSelectionRange(anchor, caret);
}

} // namespace noteAppC
