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

#include "Icons.h"

#include <QPainter>
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
    if (m_hoverEditButton == on)
        return;
    m_hoverEditButton = on;
    if (!on)
        clearHover();
}

bool MarkdownEditor::hasEditButton(const BlockView &view) const
{
    if (m_mode != Mode::LivePreview || !view.laidOut || view.tabHidden || view.piece != 0)
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
    const auto it = std::partition_point(m_blocks.cbegin(), m_blocks.cend(),
                                         [y](const BlockView &view) { return view.y - 8 <= y; });
    if (it == m_blocks.cbegin())
        return -1;
    const BlockView &view = *(it - 1);
    if (y > view.y + view.height + 8)
        return -1;
    return qsizetype(it - m_blocks.cbegin()) - 1 - view.piece;
}

QRectF MarkdownEditor::panelRectOf(qsizetype first) const
{
    const BlockView &view = m_blocks[size_t(first)];
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
    while (range.end > range.start && m_text.at(range.end - 1) == QLatin1Char('\n'))
        --range.end;
    return range;
}

QRectF MarkdownEditor::blockPanelAt(qsizetype sourceOffset) const
{
    const qsizetype index = blockAt(sourceOffset);
    if (index < 0)
        return {};
    return panelRectOf(index - m_blocks[size_t(index)].piece);
}

QRectF MarkdownEditor::editButtonRectOf(qsizetype first) const
{
    const QRectF panel = panelRectOf(first);
    const qreal clear = isFoldable(m_blocks[size_t(first)]) ? kFoldClearance : 0;
    return QRectF(panel.right() - kButtonInset - clear - kButtonSize, panel.top() + kButtonInset,
                  kButtonSize, kButtonSize);
}

QRectF MarkdownEditor::editButtonRect() const
{
    if (m_hoveredBlock < 0 || m_hoveredBlock >= qsizetype(m_blocks.size())
        || !hasEditButton(m_blocks[size_t(m_hoveredBlock)]))
        return {};
    return editButtonRectOf(m_hoveredBlock);
}

qsizetype MarkdownEditor::editButtonAt(const QPoint &point) const
{
    if (!m_hoverEditButton)
        return -1;
    const qsizetype index = blockAtPoint(point);
    if (index < 0 || !hasEditButton(m_blocks[size_t(index)]))
        return -1;
    return editButtonRectOf(index).contains(point) ? index : -1;
}

void MarkdownEditor::editBlockSource(qsizetype blockIndex)
{
    if (blockIndex < 0 || blockIndex >= qsizetype(m_blocks.size()))
        return;
    const BlockView &first = firstPiece(m_blocks[size_t(blockIndex)]);
    const Range whole = wholeBlockRange(first);
    m_interacted = true;
    m_sourceReveal = first.span.range.start;
    m_sourceRange = whole;
    // Anchor at the end and the caret at the start, as Obsidian's own
    // select-the-widget does: the caret lands on the block's first line.
    setSelectionRange(whole.end, whole.start);
    setFocus(Qt::MouseFocusReason);
}

void MarkdownEditor::updateHover(const QPoint &point)
{
    // The code label shows "Copy" while the pointer is over a listing; the
    // edit button belongs to the rendered kinds. One lookup serves both.
    const qsizetype found = blockAtPoint(point);
    qsizetype codeHover = -1;
    qsizetype editHover = -1;
    if (found >= 0) {
        const BlockView &view = m_blocks[size_t(found)];
        if (view.span.kind == BlockSpan::Kind::Code)
            codeHover = view.span.range.start;
        if (m_hoverEditButton && hasEditButton(view))
            editHover = found;
    }

    const bool overButton = editHover >= 0 && editButtonRectOf(editHover).contains(point);
    if (codeHover != m_hoveredCodeBlock || editHover != m_hoveredBlock || overButton != m_overEditButton) {
        // Only the panel the pointer left and the one it entered change.
        const QRect old = m_hoverRect;
        m_hoveredCodeBlock = codeHover;
        m_hoveredBlock = editHover;
        m_overEditButton = overButton;
        const qsizetype shown = editHover >= 0 ? editHover : found;
        m_hoverRect = (editHover >= 0 || codeHover >= 0) ? panelRectOf(shown).toAlignedRect() : QRect();
        if (!old.isNull())
            viewport()->update(old);
        if (!m_hoverRect.isNull())
            viewport()->update(m_hoverRect);
        viewport()->setToolTip(overButton ? tr("Edit this block") : QString());
    }
}

void MarkdownEditor::clearHover()
{
    const QRect old = m_hoverRect;
    m_hoveredCodeBlock = -1;
    m_hoveredBlock = -1;
    m_overEditButton = false;
    m_hoverRect = QRect();
    viewport()->setToolTip(QString());
    if (!old.isNull())
        viewport()->update(old);
}

void MarkdownEditor::paintEditHover(QPainter &painter) const
{
    if (m_hoveredBlock < 0)
        return;
    const QRectF button = editButtonRect();
    if (!button.isValid())
        return;
    const QRectF panel = panelRectOf(m_hoveredBlock);
    const qreal radius = m_theme.value(QStringLiteral("--radius-s")).toDouble();

    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(m_theme.color(QStringLiteral("--background-modifier-border")), 1));
    painter.drawRoundedRect(panel.adjusted(0.5, 0.5, -0.5, -0.5), radius, radius);
    if (m_overEditButton) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(m_theme.color(QStringLiteral("--background-modifier-hover")));
        painter.drawRoundedRect(button, radius, radius);
    }
    painter.restore();

    const QIcon icon = Icons::icon(QStringLiteral("code-2"),
                                   m_theme.color(QStringLiteral("--text-muted")), kIconSize);
    const QRectF target(button.center().x() - kIconSize / 2.0, button.center().y() - kIconSize / 2.0,
                        kIconSize, kIconSize);
    icon.paint(&painter, target.toRect());
}

} // namespace noteAppC
