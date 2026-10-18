/// The `</>` button over a rendered block, which is how Obsidian gets from a
/// callout, formula, embed or table to its source with the mouse: the pointer
/// over the block rings it and shows the button, a click on the button
/// selects the whole block with the caret at its start, and a block with a
/// selection that is exactly itself shows its source (updateLayout). A click
/// anywhere else in the block is unchanged - it puts the caret there.
///
/// Hover is a mouse-move path: one lower_bound over the sorted blocks, and a
/// repaint of the two panels that changed - nothing is laid out, the button
/// is painted rather than a widget, and with the setting off none of it runs.

#include "MarkdownEditor.h"
#include "MarkdownEditor_p.h"

#include "Icons.h"

#include <QClipboard>
#include <QGuiApplication>
#include <QMenu>
#include <QPainter>
#include <QRegularExpression>
#include <QScrollBar>

#include <algorithm>

namespace noteAppC {

namespace {

constexpr qreal kButtonSize = 22; // hit area; the icon inside is 16
constexpr qreal kButtonInset = 4;
constexpr int kIconSize = 16;
// A foldable block's arrow sits at the right of its header row (kFoldHandle
// in BlockRenderers.cpp, 14, plus its 2 px margin and a gap): the button
// steps left of it rather than over it.
constexpr qreal kFoldClearance = 28;

} // namespace

void MarkdownEditor::setHoverEditButton(bool on)
{
    if (d->m_hoverEditButton == on)
        return;
    d->m_hoverEditButton = on;
    if (!on)
        clearHover();
}

bool MarkdownEditor::hasEditButton(const BlockView &view) const
{
    if (d->m_mode != Mode::LivePreview || !view.laidOut || view.tabHidden || view.piece != 0)
        return false;
    // A table is edited where it is drawn, so its grid stays while the caret
    // is in a cell; every other block drops its widget for the source.
    if (view.span.kind == BlockSpan::Kind::Table)
        return !view.isBase && !view.cells.empty();
    if (view.active || view.span.kind == BlockSpan::Kind::Frontmatter)
        return false;
    return view.callout || view.isQuery || view.isBase || view.isPluginBlock || view.isHtml
           || view.embedPanel || view.isPdfEmbed || view.isFileCard || !view.mediaPath.isEmpty()
           || !view.image.isNull(); // a formula, or an embedded image
}

qsizetype MarkdownEditor::blockAtPoint(const QPoint &point) const
{
    const qreal y = point.y() + verticalScrollBar()->value();
    // Panels reach 8 px past the block, so the block that owns a point is the
    // last one whose panel has started.
    const auto it = std::partition_point(d->m_blocks.cbegin(), d->m_blocks.cend(),
                                         [y](const BlockView &view) { return view.y - 8 <= y; });
    if (it == d->m_blocks.cbegin())
        return -1;
    const BlockView &view = *(it - 1);
    if (y > view.y + view.height + 8)
        return -1;
    return qsizetype(it - d->m_blocks.cbegin()) - 1 - view.piece;
}

QRectF MarkdownEditor::panelRectOf(qsizetype first) const
{
    const BlockView &view = d->m_blocks[size_t(first)];
    const BlockView &last = lastPiece(view);
    const qreal scroll = verticalScrollBar()->value();
    // The panel paintCallout and codeLabelOf draw: wider than the column by
    // 14 a side, 8 above and below.
    const qreal top = view.y - scroll - 8;
    return QRectF(contentLeft() - 14, top, contentWidth() + 28, last.y + last.height - view.y + 16);
}

Range MarkdownEditor::wholeBlockRange(const BlockView &first) const
{
    Range range{first.span.range.start, lastPiece(first).span.range.end};
    // md4qt's code item is the listing alone; the fences around it belong to
    // the block as far as anyone editing its source is concerned.
    if (first.span.kind == BlockSpan::Kind::Code) {
        if (const CodeFenceSpan *fence = codeFenceOf(first))
            range = {qMin(range.start, fence->block.start), qMax(range.end, fence->block.end)};
    }
    while (range.end > range.start && d->m_text.at(range.end - 1) == QLatin1Char('\n'))
        --range.end;
    return range;
}

QRectF MarkdownEditor::blockPanelAt(qsizetype sourceOffset) const
{
    const qsizetype index = blockAt(sourceOffset);
    if (index < 0)
        return {};
    return panelRectOf(index - d->m_blocks[size_t(index)].piece);
}

QRectF MarkdownEditor::editButtonRectOf(qsizetype first) const
{
    const QRectF panel = panelRectOf(first);
    const qreal clear = isFoldable(d->m_blocks[size_t(first)]) ? kFoldClearance : 0;
    return QRectF(panel.right() - kButtonInset - clear - kButtonSize, panel.top() + kButtonInset,
                  kButtonSize, kButtonSize);
}

QRectF MarkdownEditor::editButtonRect() const
{
    if (d->m_hoveredBlock < 0 || d->m_hoveredBlock >= qsizetype(d->m_blocks.size())
        || !hasEditButton(d->m_blocks[size_t(d->m_hoveredBlock)]))
        return {};
    return editButtonRectOf(d->m_hoveredBlock);
}

qsizetype MarkdownEditor::editButtonAt(const QPoint &point) const
{
    if (!d->m_hoverEditButton)
        return -1;
    const qsizetype index = blockAtPoint(point);
    if (index < 0 || !hasEditButton(d->m_blocks[size_t(index)]))
        return -1;
    return editButtonRectOf(index).contains(point) ? index : -1;
}

void MarkdownEditor::editBlockSource(qsizetype blockIndex)
{
    if (blockIndex < 0 || blockIndex >= qsizetype(d->m_blocks.size()))
        return;
    const BlockView &first = firstPiece(d->m_blocks[size_t(blockIndex)]);
    const Range whole = wholeBlockRange(first);
    d->m_interacted = true;
    d->m_sourceReveal = first.span.range.start;
    d->m_sourceRange = whole;
    // Anchor at the end and the caret at the start, as Obsidian's own
    // select-the-widget does: the caret lands on the block's first line.
    setSelectionRange(whole.end, whole.start);
    setFocus(Qt::MouseFocusReason);
}

// --- the callout's context menu ---------------------------------------------

const CalloutSpan *MarkdownEditor::topCalloutAt(qsizetype offset) const
{
    for (const CalloutSpan &callout : d->m_decorations.callouts) {
        if (callout.depth == 0 && callout.range.contains(offset))
            return &callout;
    }
    return nullptr;
}

QString MarkdownEditor::calloutSourceAt(qsizetype offset) const
{
    const CalloutSpan *callout = topCalloutAt(offset);
    if (!callout)
        return {};
    QString source = d->m_text.mid(callout->range.start, callout->range.length());
    while (source.endsWith(QLatin1Char('\n')))
        source.chop(1);
    return source;
}

bool MarkdownEditor::changeCalloutType(qsizetype offset, const QString &type)
{
    const CalloutSpan *callout = topCalloutAt(offset);
    static const QRegularExpression valid(QStringLiteral("^[\\w-]+$"));
    if (!callout || type == callout->type || !valid.match(type).hasMatch())
        return false;

    // Where the type word sits: after `[!` in a quote, after the colons (and
    // `{`, `.`) of a `:::` fence.
    qsizetype at = -1;
    if (d->m_text.at(callout->marker.start) == QLatin1Char('[')) {
        at = callout->marker.start + 2;
    } else {
        static const QRegularExpression fence(QStringLiteral("^:{3,}[ \\t]*\\{?[ \\t]*(?:#[^\\s}]+[ \\t]+)*\\.?"));
        const qsizetype lineEnd = d->m_text.indexOf(QLatin1Char('\n'), callout->marker.start);
        const auto match = fence.match(QStringView(d->m_text).mid(
            callout->marker.start, (lineEnd < 0 ? d->m_text.size() : lineEnd) - callout->marker.start));
        if (match.hasMatch())
            at = callout->marker.start + match.capturedLength();
    }
    const qsizetype length = callout->type.size();
    if (at < 0 || d->m_text.mid(at, length).toLower() != callout->type)
        return false;

    const qsizetype caret = d->m_cursor;
    pushUndo();
    d->m_lastEdit.invalidate();
    applyEdit(at, length, type);
    setCursorOffset(caret >= at + length ? caret + type.size() - length : qMin(caret, at));
    return true;
}

bool MarkdownEditor::removeCallout(qsizetype offset)
{
    const CalloutSpan *callout = topCalloutAt(offset);
    if (!callout)
        return false;
    const Range range = callout->range;
    QString text = d->m_text.mid(range.start, range.length());
    const bool newline = text.endsWith(QLatin1Char('\n'));
    if (newline)
        text.chop(1);
    QStringList lines = text.split(QLatin1Char('\n'));

    QString title;
    if (d->m_text.at(callout->marker.start) == QLatin1Char('[')) {
        // `> [!note]+ Title`: one level of `>` off every line, then the
        // first line is the title alone.
        static const QRegularExpression prefix(QStringLiteral("^[ \\t]*>[ \\t]?"));
        if (callout->titleRange.isValid())
            title = d->m_text.mid(callout->titleRange.start, callout->titleRange.length()).trimmed();
        for (QString &line : lines)
            line.remove(prefix);
        lines.removeFirst();
    } else {
        // `:::` fences: the opening and the closing line go.
        static const QRegularExpression close(QStringLiteral("^:{3,}[ \\t]*\\r?$"));
        title = callout->attributes.value(QStringLiteral("title"),
                                          callout->attributes.value(QStringLiteral("summary"))).trimmed();
        lines.removeFirst();
        if (!lines.isEmpty() && close.match(lines.constLast()).hasMatch())
            lines.removeLast();
    }
    if (!title.isEmpty()) {
        // A blank line, or the title would run into the first paragraph.
        if (!lines.isEmpty() && !lines.constFirst().trimmed().isEmpty())
            lines.prepend(QString());
        lines.prepend(title);
    }
    QString result = lines.join(QLatin1Char('\n'));
    if (newline && !result.isEmpty())
        result += QLatin1Char('\n');

    pushUndo();
    d->m_lastEdit.invalidate();
    applyEdit(range.start, range.length(), result);
    setCursorOffset(qMin<qsizetype>(range.start, d->m_text.size()));
    return true;
}

void MarkdownEditor::addCalloutActions(QMenu &menu, qsizetype blockIndex)
{
    const BlockView &view = firstPiece(d->m_blocks[size_t(blockIndex)]);
    if (!view.callout)
        return;
    const qsizetype at = view.callout->range.start;
    menu.addAction(tr("Edit block source"), this, [this, blockIndex] { editBlockSource(blockIndex); });
    menu.addAction(tr("Copy"), this, [this, at] {
        QGuiApplication::clipboard()->setText(calloutSourceAt(at));
    });
    QMenu *types = menu.addMenu(tr("Change type"));
    const QString current = view.callout->type;
    QStringList known = d->m_calloutStyles.knownTypes();
    if (!known.contains(current))
        known.prepend(current);
    for (const QString &type : std::as_const(known)) {
        QAction *action = types->addAction(type, this, [this, at, type] { changeCalloutType(at, type); });
        action->setCheckable(true);
        action->setChecked(type == current);
    }
    menu.addAction(tr("Remove callout"), this, [this, at] { removeCallout(at); });
}

void MarkdownEditor::updateHover(const QPoint &point, bool tables)
{
    // The code label shows "Copy" while the pointer is over a listing; the
    // edit button belongs to the rendered kinds. One lookup serves both.
    const qsizetype found = blockAtPoint(point);
    qsizetype codeHover = -1;
    qsizetype editHover = -1;
    if (found >= 0) {
        const BlockView &view = d->m_blocks[size_t(found)];
        if (view.span.kind == BlockSpan::Kind::Code)
            codeHover = view.span.range.start;
        if (d->m_hoverEditButton && hasEditButton(view))
            editHover = found;
    }

    const bool overButton = editHover >= 0 && editButtonRectOf(editHover).contains(point);
    if (codeHover != d->m_hoveredCodeBlock || editHover != d->m_hoveredBlock || overButton != d->m_overEditButton) {
        // Only the panel the pointer left and the one it entered change.
        const QRect old = d->m_hoverRect;
        d->m_hoveredCodeBlock = codeHover;
        d->m_hoveredBlock = editHover;
        d->m_overEditButton = overButton;
        const qsizetype shown = editHover >= 0 ? editHover : found;
        d->m_hoverRect = (editHover >= 0 || codeHover >= 0) ? panelRectOf(shown).toAlignedRect() : QRect();
        if (!old.isNull())
            viewport()->update(old);
        if (!d->m_hoverRect.isNull())
            viewport()->update(d->m_hoverRect);
        viewport()->setToolTip(overButton ? tr("Edit this block") : QString());
    }
    if (d->m_tableHandles && tables)
        updateTableHover(point); // the grips and bars of a table (TableHandles.cpp)
}

void MarkdownEditor::clearHover()
{
    const QRect old = d->m_hoverRect;
    d->m_hoveredCodeBlock = -1;
    d->m_hoveredBlock = -1;
    d->m_overEditButton = false;
    d->m_hoverRect = QRect();
    viewport()->setToolTip(QString());
    if (!old.isNull())
        viewport()->update(old);
    clearTableHover();
}

void MarkdownEditor::paintEditHover(QPainter &painter) const
{
    if (d->m_hoveredBlock < 0)
        return;
    const QRectF button = editButtonRect();
    if (!button.isValid())
        return;
    const QRectF panel = panelRectOf(d->m_hoveredBlock);
    const qreal radius = d->m_theme.value(QStringLiteral("--radius-s")).toDouble();

    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(d->m_theme.color(QStringLiteral("--background-modifier-border")), 1));
    painter.drawRoundedRect(panel.adjusted(0.5, 0.5, -0.5, -0.5), radius, radius);
    if (d->m_overEditButton) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(d->m_theme.color(QStringLiteral("--background-modifier-hover")));
        painter.drawRoundedRect(button, radius, radius);
    }
    painter.restore();

    const QIcon icon = Icons::icon(QStringLiteral("code-2"),
                                   d->m_theme.color(QStringLiteral("--text-muted")), kIconSize);
    const QRectF target(button.center().x() - kIconSize / 2.0, button.center().y() - kIconSize / 2.0,
                        kIconSize, kIconSize);
    icon.paint(&painter, target.toRect());
}

} // namespace noteAppC
