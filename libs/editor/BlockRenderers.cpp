/// Block renderers: the three things the parser produces that are not text in
/// a line - Obsidian's frontmatter properties table, the chrome a fenced code
/// block's `#|` options ask for, and the folding a callout, a `:::` container
/// or a `#| fold: true` block can be in.
///
/// They live beside MarkdownEditor.cpp for the same reason the editing
/// commands do: they are all "measure a box, paint a box, turn a click into an
/// `applyEdit`", and the engine file is long enough already.

#include "MarkdownEditor.h"

#include "Frontmatter.h"
#include "Icons.h"

#include <QClipboard>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QLineEdit>
#include <QPainter>
#include <QPainterPath>
#include <QScrollBar>
#include <QTimer>

namespace noteAppC {

namespace {

/// Obsidian's property key column, near enough.
constexpr qreal kKeyColumn = 180;
/// Air on either side of a chip's text.
constexpr qreal kChipPadding = 8;
/// Room at a chip's right end for the "x" that removes it.
constexpr qreal kChipRemover = 12;
/// The narrowest the space after a list's chips may be and still take a click
/// that adds an item; any less and it moves to a line of its own.
constexpr qreal kChipAdder = 60;
/// The disclosure triangle, and the margin it sits in.
constexpr qreal kFoldHandle = 14;

/// Lucide icon for a property type, out of the set libs/theme registers.
/// There is no calendar in it yet, so a date borrows the arrow.
QString propertyIcon(Property::Type type)
{
    switch (type) {
    case Property::Type::List:
        return QStringLiteral("list");
    case Property::Type::Number:
        return QStringLiteral("hash");
    case Property::Type::Checkbox:
        return QStringLiteral("check");
    case Property::Type::Date:
    case Property::Type::DateTime:
        return QStringLiteral("calendar");
    case Property::Type::Text:
    case Property::Type::Unknown:
        break;
    }
    return QStringLiteral("type");
}

/// One line of a value, for the types that are shown as written. A nested map
/// arrives here as its re-emitted YAML, which is several lines.
QString oneLine(const QString &value)
{
    QString flat = value;
    flat.replace(QLatin1Char('\n'), QStringLiteral(" · "));
    return flat;
}

} // namespace

// --- folding --------------------------------------------------------------

bool MarkdownEditor::isFoldable(const BlockView &view) const
{
    if (view.span.kind == BlockSpan::Kind::Frontmatter)
        return m_mode != Mode::Source && !m_properties.isEmpty();
    if (view.callout)
        return view.callout->foldable;
    return view.options != nullptr;
}

bool MarkdownEditor::defaultFoldOf(const BlockView &view) const
{
    if (view.span.kind == BlockSpan::Kind::Frontmatter)
        return false;
    if (const CalloutSpan *callout = calloutFor(view))
        return callout->foldable && callout->collapsed;
    if (const CodeOptions *options = codeOptionsFor(view))
        return options->fold;
    return false;
}

void MarkdownEditor::setFolded(const BlockView &view, bool folded)
{
    m_folds.insert(firstPiece(view).span.range.start, folded);
    invalidateBlock(blockAt(view.span.range.start)); // what follows it moves, nothing else changes
    updateLayout();
    viewport()->update();
}

void MarkdownEditor::toggleFoldAt(qsizetype sourceOffset)
{
    // A nested callout is not a block, so it is looked up by its own start.
    for (const CalloutSpan &callout : m_decorations.callouts) {
        if (callout.depth > 0 && callout.foldable && callout.range.start == sourceOffset) {
            m_folds.insert(sourceOffset, !m_folds.value(sourceOffset, callout.collapsed));
            invalidateBlock(blockAt(sourceOffset)); // it lives inside its parent's layout
            updateLayout();
            viewport()->update();
            return;
        }
    }
    const qsizetype index = blockAt(sourceOffset);
    if (index < 0 || index >= qsizetype(m_blocks.size()))
        return;
    const BlockView &view = m_blocks.at(size_t(index));
    setFolded(view, !m_folds.value(firstPiece(view).span.range.start, defaultFoldOf(view)));
}

bool MarkdownEditor::isFoldedAt(qsizetype sourceOffset) const
{
    for (const CalloutSpan &callout : m_decorations.callouts) {
        if (callout.depth > 0 && callout.foldable && callout.range.start == sourceOffset)
            return m_folds.value(sourceOffset, callout.collapsed);
    }
    const qsizetype index = blockAt(sourceOffset);
    if (index < 0 || index >= qsizetype(m_blocks.size()))
        return false;
    const BlockView &view = m_blocks.at(size_t(index));
    return m_folds.value(firstPiece(view).span.range.start, defaultFoldOf(view));
}

QRectF MarkdownEditor::foldHandleRect(const BlockView &view, qreal left, qreal top) const
{
    const qreal middle = view.textTop > 0 ? view.textTop / 2 : propertyRowHeight() / 2;
    if (view.span.kind == BlockSpan::Kind::Frontmatter) {
        // In the margin, where Obsidian puts it: the key column stays flush
        // with the text of every other block.
        return QRectF(left - kFoldHandle - 6, top + middle - kFoldHandle / 2,
                      kFoldHandle, kFoldHandle);
    }
    // Callouts and code blocks fold from the right of their own header row.
    return QRectF(left + contentWidth() - kFoldHandle - 2, top + middle - kFoldHandle / 2,
                  kFoldHandle, kFoldHandle);
}

void MarkdownEditor::paintFoldArrow(QPainter &painter, const QRectF &rect, bool folded,
                                    const QColor &colour) const
{
    const QIcon icon = Icons::icon(folded ? QStringLiteral("chevron-right")
                                          : QStringLiteral("chevron-down"),
                                   colour, int(rect.width()));
    icon.paint(&painter, rect.toRect());
}

bool MarkdownEditor::handleBlockClick(const QPoint &point)
{
    const qreal scroll = verticalScrollBar()->value();
    const qreal left = contentLeft();

    for (const BlockView &view : m_blocks) {
        const qreal top = view.y - scroll;
        if (top + view.height < 0)
            continue;
        if (top > viewport()->height())
            break;
        if (!view.laidOut)
            continue;

        if (handleMediaClick(view, point, left, top))
            return true;
        if (handlePdfEmbedClick(view, point, left, top))
            return true;
        if (handleFileCardClick(view, point, left, top))
            return true;
        if (handleEmbedCellClick(view, point, left, top))
            return true;

        // A runnable code-file embed's own run control (NotebookEmbed part
        // of FileEmbedBlock.cpp): the same rect notebookRunRectFor places
        // for a fenced block's header, reached from here instead of the
        // Code-kind branch below since an embed's own span.kind is
        // Paragraph, not Code.
        if (view.embedPanel && view.notebookEligible) {
            const QRectF runRect = notebookRunRectFor(view, left, top);
            if (runRect.isValid() && touchable(runRect, 4).contains(point)) {
                runNotebookBlockAt(view.span.range.start);
                return true;
            }
        }

        if (view.isQuery) {
            if (view.queryCollapseRect.isValid()
                && touchable(view.queryCollapseRect.translated(left, top)).contains(point)) {
                toggleQueryCollapsed(view.span.range.start);
                return true;
            }
            if (view.querySortRect.isValid()
                && touchable(view.querySortRect.translated(left, top)).contains(point)) {
                cycleQuerySort(view.span.range.start);
                return true;
            }
            for (const BlockView::QueryRow &row : view.queryRows) {
                if (row.rect.translated(left, top).contains(point)) {
                    emit queryResultActivated(row.path, row.offset);
                    return true;
                }
            }
            // Anywhere else in a rendered query block: consumed rather than
            // falling through to offsetAt(), which has no source text of its
            // own to put a caret in - the block only shows raw source again
            // once the caret reaches it by keyboard, the way a formula does.
            if (view.laidOut && top <= point.y() && point.y() <= top + view.height)
                return true;
        }

        if (view.isBase) {
            for (const BlockView::BaseRow &row : view.baseRows) {
                if (row.rect.translated(left, top).contains(point)) {
                    emit baseRowActivated(row.path);
                    return true;
                }
            }
            // Same rationale as the isQuery branch above: a rendered base
            // table has no source text of its own to place a caret in.
            if (view.laidOut && top <= point.y() && point.y() <= top + view.height)
                return true;
        }

        for (const NestedCallout &nested : view.nested) {
            if (nested.span->foldable
                && touchable(nestedFoldHandleRect(view, nested, left, top), 6).contains(point)) {
                toggleFoldAt(nested.span->range.start);
                return true;
            }
        }

        if (view.span.kind == BlockSpan::Kind::Code && !view.isQuery && !view.isBase && !view.isPluginBlock) {
            if (view.notebookEligible) {
                const QRectF runRect = notebookRunRectFor(view, left, top);
                if (runRect.isValid() && touchable(runRect, 4).contains(point)) {
                    runNotebookBlockAt(view.span.range.start);
                    return true;
                }
            }
            const CodeLabel label = codeLabelOf(view, left, top);
            if (label.visible && touchable(label.rect, 4).contains(point)) {
                copyCodeBlock(view);
                return true;
            }
            for (const TabRect &tab : tabsOf(view, left, top)) {
                if (!tab.rect.contains(point))
                    continue;
                if (!tab.selected)
                    selectCodeTab(m_tabGroups.at(size_t(view.tabGroup)).span.start, tab.member);
                return true; // a click on the strip never moves the caret into the group
            }
        }

        if (view.span.kind == BlockSpan::Kind::Frontmatter && m_mode != Mode::Source
            && !m_properties.isEmpty()) {
            const QRectF handle = foldHandleRect(view, left, top);
            // Folded, the single row is the only thing left to click.
            if (touchable(handle, 6).contains(point)
                || (view.folded && top <= point.y() && point.y() <= top + view.height)) {
                setFolded(view, !view.folded);
                return true;
            }
            for (const PropertyRow &row : view.propertyRows) {
                if (!row.control.isNull()) {
                    if (touchable(row.control.translated(left, top), 4).contains(point)) {
                        toggleProperty(row.property);
                        return true;
                    }
                    continue;
                }
                const qsizetype at = m_properties.at(row.property).range.start;
                for (int i = 0; i < row.removers.size(); ++i) {
                    if (touchable(row.removers.at(i).translated(left, top)).contains(point)) {
                        removeListItemAt(at, i);
                        return true;
                    }
                }
                if (!row.adder.isNull() && touchable(row.adder.translated(left, top)).contains(point)
                    && addListItemAt(at))
                    return true;
                for (int i = 0; i < row.chips.size() && !row.removers.isEmpty(); ++i) {
                    if (touchable(row.chips.at(i).translated(left, top)).contains(point) && editListItemAt(at, i))
                        return true;
                }
                // The value column of a scalar: click to edit it in place.
                const QRectF value = row.rect.translated(left, top).adjusted(kKeyColumn, 0, 0, 0);
                if (m_properties.at(row.property).type != Property::Type::List
                    && value.contains(point) && editPropertyAt(at))
                    return true;
            }
            continue;
        }

        if (!isFoldable(view) || view.textTop <= 0)
            continue;
        // The whole header row toggles, which is what Obsidian's callout title
        // does; the panel it paints is a little wider than the column.
        const QRectF header(left - 14, top - 8, contentWidth() + 28, view.textTop + 8);
        if (header.contains(point)) {
            setFolded(view, !view.folded);
            return true;
        }
    }
    return false;
}

// --- the properties table -------------------------------------------------

qreal MarkdownEditor::propertyRowHeight() const
{
    return lineHeightFor({BlockSpan::Kind::Paragraph, {0, 0}, 0}) * 1.2;
}

bool MarkdownEditor::layoutProperties(BlockView &view)
{
    view.propertyRows.clear();
    if (m_properties.isEmpty())
        return false; // nothing to show: fall back to the hidden YAML

    // The YAML itself is syntax either way, so the table is drawn over the gap
    // hiding it leaves. Building the map keeps every offset inside the block
    // mapping to the same place it did.
    QList<Range> hidden;
    hidden.reserve(view.lastSyntax - view.firstSyntax);
    for (int i = view.firstSyntax; i < view.lastSyntax; ++i)
        hidden.append(m_decorations.syntax.at(i));
    view.map = DisplayMap::build(m_text, view.span.range, hidden);
    view.layout.reset();
    view.cells.clear();

    const qreal rowHeight = propertyRowHeight();
    view.folded = m_folds.value(view.span.range.start, false);
    if (view.folded) {
        view.height = rowHeight;
        view.laidOut = true;
        return true;
    }

    // One metrics object for the whole table: building one per row is the bug
    // that cost 375 ms once.
    const QFontMetricsF metrics(m_theme.textFont());
    const qreal chipHeight = metrics.height() + 4;
    const qreal valueWidth = qMax<qreal>(80, contentWidth() - kKeyColumn);

    qreal y = 0;
    for (int i = 0; i < m_properties.size(); ++i) {
        const Property &property = m_properties.at(i);
        PropertyRow row;
        row.property = i;
        qreal height = rowHeight;

        if (property.type == Property::Type::Checkbox) {
            const qreal box = metrics.height() * 0.8;
            row.control = QRectF(kKeyColumn, y + (rowHeight - box) / 2, box, box);
        } else if (property.type == Property::Type::List) {
            // Only a list that can be rewritten gets the controls to change it,
            // and not in reading view, which is for reading - and for print.
            const bool editable = m_mode != Mode::Reading && listIsRewritable(property);
            qreal x = kKeyColumn;
            qreal chipTop = y + (rowHeight - chipHeight) / 2;
            for (const QString &value : property.values) {
                const qreal width = metrics.horizontalAdvance(oneLine(value)) + 2 * kChipPadding
                                    + (editable ? kChipRemover : 0);
                if (x > kKeyColumn && x + width > kKeyColumn + valueWidth) {
                    x = kKeyColumn;
                    chipTop += chipHeight + 4;
                }
                row.chips.append(QRectF(x, chipTop, width, chipHeight));
                if (editable) {
                    row.removers.append(QRectF(x + width - kChipPadding - kChipRemover + 2,
                                               chipTop, kChipRemover + 4, chipHeight));
                }
                x += width + 6;
            }
            if (editable) {
                if (x > kKeyColumn && x + kChipAdder > kKeyColumn + valueWidth) {
                    x = kKeyColumn;
                    chipTop += chipHeight + 4;
                }
                row.adder = QRectF(x, chipTop, kKeyColumn + valueWidth - x, chipHeight);
            }
            height = qMax(rowHeight, chipTop + chipHeight + (rowHeight - chipHeight) / 2 - y);
        }

        row.rect = QRectF(0, y, contentWidth(), height);
        y += height;
        view.propertyRows.push_back(row);
    }

    // Obsidian closes the table with a hairline and a breath of space.
    view.height = y + blockSpacing() / 2;
    view.laidOut = true;
    return true;
}

void MarkdownEditor::paintProperties(QPainter &painter, const BlockView &view, qreal left,
                                     qreal top) const
{
    const QColor muted = m_theme.color(QStringLiteral("--text-muted"));
    const QColor faint = m_theme.color(QStringLiteral("--text-faint"));
    const QColor normal = m_theme.color(QStringLiteral("--text-normal"));
    const QColor border = m_theme.color(QStringLiteral("--background-modifier-border"));
    const QColor chip = m_theme.color(QStringLiteral("--background-modifier-hover"));

    if (!view.laidOut)
        return;

    painter.save();
    paintFoldArrow(painter, foldHandleRect(view, left, top), view.folded, faint);

    const QFont textFont = m_theme.textFont();
    const QFontMetricsF metrics(textFont);
    painter.setFont(textFont);

    if (view.folded) {
        painter.setPen(faint);
        painter.drawText(QRectF(left, top, contentWidth(), view.height),
                         Qt::AlignVCenter | Qt::AlignLeft,
                         tr("%1 properties").arg(m_properties.size()));
        painter.setPen(QPen(border, 1));
        painter.drawLine(QPointF(left, top + view.height),
                         QPointF(left + contentWidth(), top + view.height));
        painter.restore();
        return;
    }

    const qreal iconSize = qMin<qreal>(15, metrics.height());
    for (const PropertyRow &row : view.propertyRows) {
        const Property &property = m_properties.at(row.property);
        const QRectF rect = row.rect.translated(left, top);
        const qreal keyTop = rect.top() + (propertyRowHeight() - metrics.height()) / 2;

        const QIcon icon = Icons::icon(propertyIcon(property.type), faint, int(iconSize));
        icon.paint(&painter, QRectF(rect.left(), keyTop + (metrics.height() - iconSize) / 2,
                                    iconSize, iconSize).toRect());

        painter.setPen(muted);
        painter.drawText(QRectF(rect.left() + iconSize + 8, keyTop,
                                kKeyColumn - iconSize - 16, metrics.height()),
                         Qt::AlignVCenter | Qt::AlignLeft,
                         metrics.elidedText(property.key, Qt::ElideRight,
                                            kKeyColumn - iconSize - 16));

        if (!row.control.isNull()) {
            paintCheckBox(painter, row.control.translated(left, top),
                          property.values.value(0).compare(QLatin1String("true"),
                                                           Qt::CaseInsensitive) == 0);
            continue;
        }

        if (property.type == Property::Type::List) {
            for (int i = 0; i < row.chips.size(); ++i) {
                QRectF box = row.chips.at(i).translated(left, top);
                QPainterPath rounded;
                rounded.addRoundedRect(box, 4, 4);
                painter.fillPath(rounded, chip);
                if (i < row.removers.size()) {
                    // A small cross, drawn so it matches the text at any size.
                    const QRectF cross = row.removers.at(i).translated(left, top);
                    const QPointF centre = cross.center() - QPointF(2, 0);
                    const qreal arm = 3;
                    painter.setPen(QPen(faint, 1.2));
                    painter.drawLine(centre + QPointF(-arm, -arm), centre + QPointF(arm, arm));
                    painter.drawLine(centre + QPointF(-arm, arm), centre + QPointF(arm, -arm));
                    box.setRight(box.right() - kChipRemover);
                }
                painter.setPen(normal);
                painter.drawText(box, Qt::AlignCenter, oneLine(property.values.value(i)));
            }
            continue;
        }

        const qreal valueWidth = rect.width() - kKeyColumn;
        painter.setPen(normal);
        painter.drawText(QRectF(rect.left() + kKeyColumn, keyTop, valueWidth, metrics.height()),
                         Qt::AlignVCenter | Qt::AlignLeft,
                         metrics.elidedText(oneLine(property.values.join(QLatin1String(", "))),
                                            Qt::ElideRight, valueWidth));
    }

    painter.setPen(QPen(border, 1));
    painter.drawLine(QPointF(left, top + view.height - blockSpacing() / 4),
                     QPointF(left + contentWidth(), top + view.height - blockSpacing() / 4));
    painter.restore();
}

void MarkdownEditor::paintCheckBox(QPainter &painter, const QRectF &rect, bool done) const
{
    QPainterPath rounded;
    rounded.addRoundedRect(rect, 3, 3);

    painter.save();
    if (done) {
        painter.fillPath(rounded, m_theme.color(QStringLiteral("--interactive-accent")));
        painter.setPen(QPen(m_theme.color(QStringLiteral("--text-on-accent")), 2, Qt::SolidLine,
                            Qt::RoundCap, Qt::RoundJoin));
        // A tick, drawn rather than shipped as an icon so it scales with the
        // font it sits beside.
        painter.drawPolyline(QPolygonF({
            QPointF(rect.left() + rect.width() * 0.24, rect.top() + rect.height() * 0.52),
            QPointF(rect.left() + rect.width() * 0.44, rect.top() + rect.height() * 0.72),
            QPointF(rect.left() + rect.width() * 0.78, rect.top() + rect.height() * 0.30),
        }));
    } else {
        painter.setPen(
            QPen(m_theme.color(QStringLiteral("--background-modifier-border-hover")), 1.5));
        painter.drawPath(rounded);
    }
    painter.restore();
}

Range MarkdownEditor::editableValueOf(const Property &property) const
{
    switch (property.type) {
    case Property::Type::Text:
    case Property::Type::Number:
    case Property::Type::Date:
    case Property::Type::DateTime:
        break;
    default:
        return {}; // lists are chips, checkboxes toggle, maps stay as written
    }
    if (property.raw.size() > 1 || property.values.size() > 1)
        return {};
    // One line, key and value, and nothing after it that belongs to the
    // entry: a comment, a block scalar, a flow value run over lines.
    const qsizetype lineEnd = m_text.indexOf(QLatin1Char('\n'), property.range.start);
    if (lineEnd < 0 || lineEnd + 1 != property.range.end)
        return {};
    const qsizetype colon = m_text.indexOf(QLatin1Char(':'), property.range.start);
    if (colon < 0 || colon >= lineEnd)
        return {};

    if (property.raw.isEmpty() || property.raw.first().isEmpty()) {
        // `key:` with no value yet: it goes after the colon.
        if (!QStringView(m_text).mid(colon + 1, lineEnd - colon - 1).trimmed().isEmpty())
            return {};
        return {colon + 1, lineEnd};
    }
    const QString &raw = property.raw.first();
    const qsizetype at = m_text.indexOf(raw, colon + 1);
    if (at < 0 || at + raw.size() > lineEnd || raw.contains(QLatin1String(" #")))
        return {};
    // Only blanks may follow; anything else is a part of the line this does
    // not understand, and it is left alone.
    if (!QStringView(m_text).mid(at + raw.size(), lineEnd - at - raw.size()).trimmed().isEmpty())
        return {};
    return {at, at + raw.size()};
}

bool MarkdownEditor::listIsRewritable(const Property &property) const
{
    if (property.type != Property::Type::List || !property.range.isValid())
        return false;
    const QStringView entry = QStringView(m_text).mid(property.range.start, property.range.length());
    return Frontmatter::rewriteList(entry, property.raw).has_value();
}

const MarkdownEditor::PropertyRow *MarkdownEditor::propertyRowOf(int index, qreal *top) const
{
    for (const BlockView &view : m_blocks) {
        if (view.span.kind != BlockSpan::Kind::Frontmatter || view.propertyRows.empty()
            || view.folded)
            continue;
        for (const PropertyRow &row : view.propertyRows) {
            if (row.property == index) {
                *top = view.y - verticalScrollBar()->value();
                return &row;
            }
        }
    }
    return nullptr;
}

int MarkdownEditor::propertyAt(qsizetype sourceOffset) const
{
    for (int i = 0; i < m_properties.size(); ++i) {
        if (m_properties.at(i).range.contains(sourceOffset))
            return i;
    }
    return -1;
}

bool MarkdownEditor::editPropertyAt(qsizetype sourceOffset)
{
    if (m_mode == Mode::Source || m_mode == Mode::Reading)
        return false;
    finishPropertyEdit(true);
    const int index = propertyAt(sourceOffset);
    if (index < 0 || !editableValueOf(m_properties.at(index)).isValid())
        return false;
    qreal top = 0;
    const PropertyRow *row = propertyRowOf(index, &top);
    if (!row)
        return false;
    openPropertyEditor(index, row->rect.translated(contentLeft(), top).adjusted(kKeyColumn - 4, 0, 0, 0),
                       m_properties.at(index).values.value(0), kEditValue);
    return true;
}

bool MarkdownEditor::editListItemAt(qsizetype sourceOffset, int item)
{
    if (m_mode == Mode::Source || m_mode == Mode::Reading)
        return false;
    finishPropertyEdit(true);
    const int index = propertyAt(sourceOffset);
    if (index < 0 || !listIsRewritable(m_properties.at(index)))
        return false;
    qreal top = 0;
    const PropertyRow *row = propertyRowOf(index, &top);
    if (!row || item < 0 || item >= row->chips.size())
        return false;
    // Over the chip, wide enough to type into.
    QRectF cell = row->chips.at(item).translated(contentLeft(), top);
    cell.setWidth(qMax<qreal>(cell.width(), 140));
    openPropertyEditor(index, cell, m_properties.at(index).values.value(item), item);
    return true;
}

bool MarkdownEditor::addListItemAt(qsizetype sourceOffset)
{
    if (m_mode == Mode::Source || m_mode == Mode::Reading)
        return false;
    finishPropertyEdit(true);
    const int index = propertyAt(sourceOffset);
    if (index < 0 || !listIsRewritable(m_properties.at(index)))
        return false;
    qreal top = 0;
    const PropertyRow *row = propertyRowOf(index, &top);
    if (!row || row->adder.isNull())
        return false;
    openPropertyEditor(index, row->adder.translated(contentLeft(), top), QString(), kEditNewItem);
    return true;
}

bool MarkdownEditor::removeListItemAt(qsizetype sourceOffset, int item)
{
    const int index = propertyAt(sourceOffset);
    if (index < 0 || !listIsRewritable(m_properties.at(index))
        || item < 0 || item >= m_properties.at(index).raw.size())
        return false;
    QStringList raws = m_properties.at(index).raw;
    raws.removeAt(item);
    QStringList values = m_properties.at(index).values;
    values.removeAt(item);
    return replaceListEntry(index, raws, values);
}

bool MarkdownEditor::replaceListEntry(int index, const QStringList &raws, const QStringList &values)
{
    Property &property = m_properties[index];
    const Range range = property.range;
    const std::optional<QString> entry = Frontmatter::rewriteList(
        QStringView(m_text).mid(range.start, range.length()), raws);
    if (!entry)
        return false;
    // Before the edit: applying it relays the table out, and the chips are
    // laid out from these.
    property.raw = raws;
    property.values = values;
    const qsizetype delta = entry->size() - range.length();
    const qsizetype anchor = m_anchor >= range.end ? m_anchor + delta : m_anchor;
    const qsizetype caret = m_cursor >= range.end ? m_cursor + delta : m_cursor;
    pushUndo();
    m_lastEdit.invalidate();
    applyEdit(range.start, range.length(), *entry);
    setSelectionRange(anchor, caret);
    return true;
}

void MarkdownEditor::openPropertyEditor(int index, const QRectF &cell, const QString &text,
                                        int item)
{
    auto *edit = new QLineEdit(viewport());
    edit->setFrame(false);
    edit->setFont(m_theme.textFont());
    QPalette palette = edit->palette();
    palette.setColor(QPalette::Base, m_theme.color(QStringLiteral("--background-modifier-hover")));
    palette.setColor(QPalette::Text, m_theme.color(QStringLiteral("--text-normal")));
    palette.setColor(QPalette::Highlight, m_theme.color(QStringLiteral("--interactive-accent")));
    edit->setPalette(palette);
    edit->setGeometry(cell.toRect());
    edit->setText(text);
    edit->selectAll();
    edit->installEventFilter(this);
    // Enter, or the focus going elsewhere, keeps what was typed.
    connect(edit, &QLineEdit::editingFinished, this, [this] { finishPropertyEdit(true); });
    m_propertyEdit = edit;
    m_editedKey = m_properties.at(index).key;
    m_editedItem = item;
    edit->show();
    edit->setFocus();
}

void MarkdownEditor::finishPropertyEdit(bool commit)
{
    if (!m_propertyEdit)
        return;
    // Cleared first: hiding the editor moves the focus, which finishes it again.
    QLineEdit *edit = m_propertyEdit;
    m_propertyEdit = nullptr;
    const QString typed = edit->text();
    edit->removeEventFilter(this);
    edit->disconnect(this);
    edit->hide();
    edit->deleteLater();
    setFocus();

    if (!commit)
        return;
    // By key, not by index: a reparse may have renumbered the properties.
    const auto property = std::find_if(m_properties.begin(), m_properties.end(),
                                       [this](const Property &candidate) {
                                           return candidate.key == m_editedKey;
                                       });
    if (property == m_properties.end())
        return;
    if (m_editedItem != kEditValue) {
        const std::optional<QString> item = Frontmatter::scalarFor(typed, Property::Type::Text);
        if (typed.trimmed().isEmpty() || !item || !listIsRewritable(*property))
            return; // an item emptied out is removed with its cross, not here
        QStringList raws = property->raw;
        QStringList values = property->values;
        if (m_editedItem == kEditNewItem) {
            raws << *item;
            values << typed.trimmed();
        } else if (m_editedItem < raws.size() && typed.trimmed() != values.at(m_editedItem)) {
            raws[m_editedItem] = *item;
            values[m_editedItem] = typed.trimmed();
        } else {
            return;
        }
        replaceListEntry(int(property - m_properties.begin()), raws, values);
        return;
    }
    if (typed == property->values.value(0))
        return;
    const Range value = editableValueOf(*property);
    const std::optional<QString> yaml = Frontmatter::scalarFor(typed, property->type);
    if (!value.isValid() || !yaml)
        return; // a number that is not one, say: the file keeps what it had
    const QString written =
        property->raw.isEmpty() || property->raw.first().isEmpty() ? QLatin1Char(' ') + *yaml : *yaml;

    const qsizetype delta = written.size() - value.length();
    const qsizetype anchor = m_anchor > value.start ? m_anchor + delta : m_anchor;
    const qsizetype caret = m_cursor > value.start ? m_cursor + delta : m_cursor;
    pushUndo();
    m_lastEdit.invalidate();
    applyEdit(value.start, value.length(), written);
    property->values = QStringList{typed.trimmed()};
    property->raw = QStringList{*yaml};
    setSelectionRange(anchor, caret);
}

bool MarkdownEditor::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_propertyEdit && event->type() == QEvent::KeyPress) {
        // Handled here, not left to QLineEdit: it lets Return go on to its
        // parent, and the editor would type it into the note.
        switch (static_cast<QKeyEvent *>(event)->key()) {
        case Qt::Key_Escape:
            finishPropertyEdit(false);
            return true;
        case Qt::Key_Return:
        case Qt::Key_Enter:
            finishPropertyEdit(true);
            return true;
        default:
            break;
        }
    }
    return QAbstractScrollArea::eventFilter(watched, event);
}

void MarkdownEditor::toggleProperty(int index)
{
    if (index < 0 || index >= m_properties.size())
        return;
    Property &property = m_properties[index];
    // Only a plain `true`/`false` scalar round trips safely. Anything else -
    // a quoted value, a nested map, an anchor - stays read-only rather than
    // risk rewriting bytes nobody asked about.
    if (property.type != Property::Type::Checkbox || property.raw.size() != 1)
        return;

    const QString raw = property.raw.first();
    if (raw.compare(QLatin1String("true"), Qt::CaseInsensitive) != 0
        && raw.compare(QLatin1String("false"), Qt::CaseInsensitive) != 0)
        return;

    const qsizetype colon = m_text.indexOf(QLatin1Char(':'), property.range.start);
    if (colon < 0 || colon >= property.range.end)
        return;
    const qsizetype at = m_text.indexOf(raw, colon);
    if (at < 0 || at + raw.size() > property.range.end)
        return;

    const bool on = raw.compare(QLatin1String("true"), Qt::CaseInsensitive) == 0;
    const QString written = on ? QStringLiteral("false") : QStringLiteral("true");

    // Ticking a box must not take the caret with it; it is only ever a
    // character or two away from where it was.
    const qsizetype delta = written.size() - raw.size();
    const qsizetype anchor = m_anchor > at ? m_anchor + delta : m_anchor;
    const qsizetype caret = m_cursor > at ? m_cursor + delta : m_cursor;

    pushUndo();
    m_lastEdit.invalidate();
    applyEdit(at, raw.size(), written);
    // The table repaints now, on the buffer; the parse behind the debounce
    // will say the same thing in a moment.
    property.values = QStringList{written};
    property.raw = QStringList{written};
    setSelectionRange(anchor, caret);
}

// --- `#|` code block options ----------------------------------------------

const CodeOptions *MarkdownEditor::codeOptionsFor(const BlockView &view) const
{
    // Every piece of a long fence takes the options its first line declares.
    const qsizetype start = firstPiece(view).span.range.start;
    for (const CodeOptions &options : m_decorations.codeOptions) {
        if (options.block.start == start)
            return &options;
    }
    return nullptr;
}

bool MarkdownEditor::hasCodeHeader(const BlockView &view) const
{
    if (plainSource(view))
        return false;
    if (view.options || view.tabGroup >= 0 || view.notebookEligible)
        return true;
    // Every fence with a language names itself in a strip. Matched on the
    // Code kind: the paragraph above a fence holds its opening line, and
    // codeFenceOf finds that fence from there too.
    const CodeFenceSpan *fence = view.span.kind == BlockSpan::Kind::Code ? codeFenceOf(view) : nullptr;
    return fence && !fence->language.trimmed().isEmpty();
}

QString MarkdownEditor::codeTitleOf(const BlockView &view) const
{
    if (view.tabGroup >= 0)
        return {}; // the tabs are the titles
    // Read from the source, not view.options, which is null while the caret
    // is in the block and would rename it under the cursor.
    if (const CodeOptions *options = codeOptionsFor(view); options && !options->title.isEmpty())
        return options->title;
    const CodeFenceSpan *fence = codeFenceOf(view);
    return fence && !fence->language.trimmed().isEmpty() ? styleOfFence(*fence).name : QString();
}

QString MarkdownEditor::codeTitleAt(qsizetype offset) const
{
    const qsizetype index = blockAt(offset);
    if (index < 0 || index >= qsizetype(m_blocks.size()))
        return {};
    const BlockView &view = m_blocks[size_t(index)];
    return view.span.kind == BlockSpan::Kind::Code && hasCodeHeader(view) ? codeTitleOf(view) : QString();
}

qreal MarkdownEditor::codeHeaderHeight() const
{
    return lineHeightFor({BlockSpan::Kind::Code, {0, 0}, 0}) * 1.5;
}

qreal MarkdownEditor::codeCaptionHeight(const BlockView &view) const
{
    if (!view.options || view.options->caption.isEmpty() || view.folded
        || view.piece != view.pieces - 1)
        return 0;
    return lineHeightFor({BlockSpan::Kind::Code, {0, 0}, 0}) * 1.4;
}

qreal MarkdownEditor::codeGutterWidth(const BlockView &view) const
{
    if (!view.options || !view.options->lineNumbers || !view.layout)
        return 0;
    // Every piece of a fence has to agree on the width, so a long one counts
    // its source lines - fences and `#|` lines too, which can only overcount
    // by a digit at a power of ten.
    const BlockView &first = firstPiece(view);
    const BlockView &closing = lastPiece(view);
    const int lines = view.pieces > 1
                          ? int(m_lines.lineAt(closing.span.range.end - 1)
                                - m_lines.lineAt(first.span.range.start))
                          : int(view.map.text().count(QLatin1Char('\n')));
    const int last = view.options->firstLine + lines;
    return QFontMetricsF(fontFor(view.span)).horizontalAdvance(QLatin1Char('0'))
               * qMax(2, QString::number(last).size())
           + 14;
}

/// The vertical band a source range occupies inside a laid-out block, in the
/// block's own coordinates - the top of its first display line to the bottom
/// of its last.
///
/// Anything drawn *behind* text that is only part of a block needs this: a
/// listing's panel inside a callout, and a nested callout's own panel. Returns
/// an empty band when the range is hidden or falls outside the block.
QRectF MarkdownEditor::bandOf(const BlockView &view, const Range &source) const
{
    if (!view.layout)
        return {};
    const Range shown = view.map.toDisplay(source);
    if (shown.length() <= 0)
        return {};

    const QTextLine first = view.layout->lineForTextPosition(int(shown.start));
    // The end offset is one past the last character, which on a line boundary
    // belongs to the next line; step back so the band stops where the text does.
    const QTextLine last = view.layout->lineForTextPosition(int(shown.end) - 1);
    if (!first.isValid() || !last.isValid())
        return {};
    return QRectF(0, first.y(), 0, last.y() + last.height() - first.y());
}

/// Callouts inside a callout. Only the outermost one is a block with a header
/// of its own; a nested one keeps its title in the layout, where the format
/// pass styles it, and gets its panel and icon drawn here behind and beside
/// the lines it covers.
void MarkdownEditor::paintNestedCallouts(QPainter &painter, const BlockView &view, qreal left,
                                         qreal top) const
{
    for (const NestedCallout &nested : view.nested) {
        const QRectF band = bandOf(view, nested.span->range);
        if (band.height() <= 0)
            continue;

        const qreal inset = left + 14 + kCalloutIndent * (nested.span->depth - 1);
        // One that carries on from the piece above, or into the one below,
        // runs flush to that edge with square corners there, so the pieces
        // meet without a seam - the outer callout's own rule in paintCallout.
        const bool opens = nested.span->range.start >= view.span.range.start;
        const bool closes = nested.span->range.end <= view.span.range.end;
        const qreal panelTop = top + view.textTop + band.y() - (opens ? 4 : 0);
        const qreal panelBottom = closes ? top + view.textTop + band.bottom() + 4 : top + view.height;
        const QRectF panel(inset, panelTop, qMax(qreal(40), left + contentWidth() - 14 - inset),
                           panelBottom - panelTop);
        QPainterPath rounded;
        rounded.addRoundedRect(panel.adjusted(0, opens ? 0 : -8, 0, closes ? 0 : 8), 6, 6);
        painter.save();
        painter.setClipRect(panel, Qt::IntersectClip);
        painter.fillPath(rounded, nested.style.background);
        painter.restore();
        painter.fillRect(QRectF(panel.left(), panel.top(), 3, panel.height()), nested.style.bar);

        // The icon goes in the step the nesting indent opened up, so it never
        // collides with the title however large the title's font is.
        const QRectF title = bandOf(view, nested.span->titleRange);
        // The icon has to fit the step the indent opened, whatever size the
        // style gives the title - a larger heading font must not push the icon
        // out over the text.
        const qreal iconSize = qMin(QFontMetricsF(nested.style.titleFont(m_theme)).height(),
                                    kCalloutIndent - 8);
        const QIcon icon = Icons::icon(nested.style.icon, nested.style.bar, int(iconSize));
        if (!icon.isNull() && title.height() > 0) {
            const QRectF iconRect(panel.left() + 5, top + view.textTop + title.y()
                                                        + (title.height() - iconSize) / 2,
                                  iconSize, iconSize);
            icon.paint(&painter, iconRect.toRect());
        }
        if (nested.span->foldable && title.height() > 0) {
            paintFoldArrow(painter, nestedFoldHandleRect(view, nested, left, top),
                           m_folds.value(nested.span->range.start, nested.span->collapsed),
                           nested.style.bar);
        }
    }
}

/// Where a nested callout's fold arrow sits: the right end of its title row,
/// inside its own panel. One function for painting and for clicking.
QRectF MarkdownEditor::nestedFoldHandleRect(const BlockView &view, const NestedCallout &nested,
                                            qreal left, qreal top) const
{
    const QRectF title = bandOf(view, nested.span->titleRange);
    if (title.height() <= 0)
        return {};
    const qreal right = left + contentWidth() - 14 - 6;
    const qreal y = top + view.textTop + title.y() + (title.height() - kFoldHandle) / 2;
    return QRectF(right - kFoldHandle, y, kFoldHandle, kFoldHandle);
}

/// The panel behind a fenced listing that sits inside a callout or a
/// blockquote. A block-level listing gets this from paintCodeChrome; one
/// nested inside another block has no block of its own to paint it, and a
/// per-character background leaves gaps between the glyphs and stops dead at a
/// wrapped line.
void MarkdownEditor::paintNestedCodePanels(QPainter &painter, const BlockView &view, qreal left,
                                           qreal top) const
{
    const QColor panelColour = m_theme.color(QStringLiteral("--background-primary-alt"));
    for (int i = view.firstStyle; i < view.lastStyle; ++i) {
        const StyleSpan &span = m_decorations.styles.at(i);
        if (span.kind != StyleSpan::Kind::CodeBlock || !span.range.overlaps(view.span.range))
            continue;
        const QRectF band = bandOf(view, span.range);
        if (band.height() <= 0)
            continue;
        // A listing inside a nested callout steps in with it, or its panel
        // would cover the callout's own and hide the nesting entirely.
        const qreal indent = (view.callout ? 14 : 0)
                             + kCalloutIndent * nestingDepthAt(view, span.range.start);
        QPainterPath rounded;
        rounded.addRoundedRect(QRectF(left + indent - 6, top + view.textTop + band.y() - 3,
                                      qMax(qreal(40), contentWidth() - indent - 8),
                                      band.height() + 6),
                               4, 4);
        const LanguageStyle style = styleOfLanguage(span.payload);
        painter.fillPath(rounded, style.bg.isValid() ? style.bg : panelColour);
        if (style.border.isValid()) {
            painter.save();
            painter.setPen(QPen(style.border, 1));
            painter.setBrush(Qt::NoBrush);
            painter.drawPath(rounded);
            painter.restore();
        }
    }
}

const CodeFenceSpan *MarkdownEditor::codeFenceOf(const BlockView &view) const
{
    // codeFences is sorted by start, and a note can hold thousands of them, so
    // this is a binary search rather than a walk: layout asks it per block.
    // Matched by the listing, not the start - md4qt's code item covers the
    // listing alone, so the two ranges agree about the middle and not
    // necessarily about the fences.
    const QList<CodeFenceSpan> &fences = m_decorations.codeFences;
    auto it = std::lower_bound(fences.cbegin(), fences.cend(), view.span.range.start,
                               [](const CodeFenceSpan &fence, qsizetype start) {
                                   return fence.block.end <= start;
                               });
    for (; it != fences.cend() && it->block.start < view.span.range.end; ++it) {
        if (it->depth == 0
            && (it->content.overlaps(view.span.range) || view.span.range.contains(it->block.start)))
            return &*it;
    }
    return nullptr;
}

LanguageStyle MarkdownEditor::styleOfFence(const CodeFenceSpan &fence) const
{
    // Three layers, as callouts have: the language as written, the section
    // codeblocks.ini gives it, then whatever the fence itself says after the
    // language - `name`, `icon` and `color`, the keys codeblocks.ini uses.
    LanguageStyle style = styleOfLanguage(fence.language);
    const QMap<QString, QString> &own = fence.attributes;
    if (!own.value(QStringLiteral("name")).isEmpty())
        style.name = own.value(QStringLiteral("name"));
    if (!own.value(QStringLiteral("icon")).isEmpty())
        style.icon = own.value(QStringLiteral("icon"));
    const QColor colour = CalloutStyle::parseColor(own.value(QStringLiteral("color")));
    if (colour.isValid())
        style.color = colour;
    if (m_complexCodeBlocks)
        style.applyColours(own); // `bg=`, `fg=`, `border=`, `syntax.<capture>=`
    return style;
}

LanguageStyle MarkdownEditor::styleOfLanguage(const QString &language) const
{
    LanguageStyle style = m_languageStyles.resolve(language);
    if (!m_complexCodeBlocks) {
        // Off is "as if codeblocks.ini never said any of it".
        style.bg = style.fg = style.border = QColor();
        style.syntax.clear();
    }
    return style;
}

bool MarkdownEditor::syntaxMapFor(const LanguageStyle &style, SyntaxMap *out) const
{
    if (!m_complexCodeBlocks || (style.syntax.isEmpty() && m_themeSyntax.isEmpty()))
        return false;
    *out = SyntaxMap({style.syntax, m_themeSyntax});
    return !out->isEmpty();
}

void MarkdownEditor::setComplexCodeBlocks(bool on)
{
    if (m_complexCodeBlocks == on)
        return;
    m_complexCodeBlocks = on;
    invalidateLayouts();
    updateLayout();
    viewport()->update();
}

LanguageStyle MarkdownEditor::codeStyleOf(const BlockView &view) const
{
    if (const CodeFenceSpan *fence = codeFenceOf(view))
        return styleOfFence(*fence);
    QString language;
    codeContentOf(view, &language);
    return styleOfLanguage(language);
}

void MarkdownEditor::selectCodeTab(qsizetype sourceOffset, int member)
{
    const TabGroup *group = tabGroupAt(sourceOffset);
    if (!group || member < 0 || member >= group->count)
        return;
    m_selectedTabs.insert(group->span.start, member);
    // Only the group's own blocks change what they show; relaying out the
    // whole note for a tab click is what this used to do.
    for (BlockView &view : m_blocks) {
        if (view.span.range.start < group->span.start || view.span.range.start >= group->span.end)
            continue;
        view.layout.reset();
        view.laidOut = false;
        view.heightEstimated = false;
        view.height = 0;
    }
    updateLayout();
    viewport()->update();
}

void MarkdownEditor::collectTabGroups()
{
    // Rebuilt once per parse, so layout finds a block's group by binary
    // search instead of scanning every fence for every block.
    m_tabGroups.clear();
    const QList<CodeFenceSpan> &fences = m_decorations.codeFences;
    for (int i = 0; i < fences.size(); ++i) {
        if (fences.at(i).tabGroup != i)
            continue; // only a group's first member carries its own index
        int end = i;
        while (end < fences.size() && fences.at(end).tabGroup == i)
            ++end;
        TabGroup group;
        group.span = {fences.at(i).block.start, fences.at(end - 1).block.end};
        group.hiddenEnd = fences.at(end - 1).content.end;
        group.first = i;
        group.count = end - i;
        m_tabGroups.push_back(group);
        i = end - 1;
    }
}

const MarkdownEditor::TabGroup *MarkdownEditor::tabGroupAt(qsizetype source) const
{
    auto it = std::upper_bound(m_tabGroups.cbegin(), m_tabGroups.cend(), source,
                               [](qsizetype at, const TabGroup &group) {
                                   return at < group.span.start;
                               });
    if (it == m_tabGroups.cbegin())
        return nullptr;
    --it;
    return source < it->span.end ? &*it : nullptr;
}

const MarkdownEditor::TabGroup *MarkdownEditor::caretTabGroup() const
{
    if (m_mode != Mode::LivePreview || !m_interacted || m_tabGroups.empty())
        return nullptr;
    if (const TabGroup *group = tabGroupAt(m_cursor))
        return group;
    // Just past the closing fence still counts as inside.
    const TabGroup *before = m_cursor > 0 ? tabGroupAt(m_cursor - 1) : nullptr;
    return before && m_cursor == before->span.end ? before : nullptr;
}

int MarkdownEditor::selectedTabOf(const TabGroup &group) const
{
    return qBound(0, m_selectedTabs.value(group.span.start, 0), group.count - 1);
}

std::vector<MarkdownEditor::TabRect> MarkdownEditor::tabsOf(const BlockView &view, qreal left,
                                                            qreal top) const
{
    std::vector<TabRect> tabs;
    if (view.tabGroup < 0 || view.tabGroup >= int(m_tabGroups.size()) || view.piece > 0)
        return tabs;
    const TabGroup &group = m_tabGroups.at(size_t(view.tabGroup));
    const int selected = selectedTabOf(group);

    QFont font = m_theme.uiFont();
    font.setPixelSize(m_theme.value(QStringLiteral("--font-ui-small")).toInt());
    const QFontMetricsF metrics(font);
    const qreal iconSize = metrics.height();
    const QRectF panel(left - 14, top - 8, contentWidth() + 28, view.height + 16);
    const qreal height = view.textTop + 8;

    qreal x = panel.left() + 8;
    for (int i = 0; i < group.count; ++i) {
        const CodeFenceSpan &fence = m_decorations.codeFences.at(group.first + i);
        const LanguageStyle style = styleOfFence(fence);
        TabRect tab;
        tab.member = i;
        tab.selected = i == selected;
        // A bare `tab` takes the language's style name; one with no language
        // at all is numbered, so no tab is ever blank.
        tab.text = !fence.tabLabel.isEmpty() ? fence.tabLabel
                   : !style.name.isEmpty()   ? style.name
                                             : tr("Tab %1").arg(i + 1);
        if (!style.icon.isEmpty())
            tab.icon = Icons::icon(style.icon,
                                   style.color.isValid() ? style.color
                                                         : m_theme.color(QStringLiteral("--text-muted")),
                                   int(iconSize));
        const qreal width = 12 + (tab.icon.isNull() ? 0 : iconSize + 5)
                            + metrics.horizontalAdvance(tab.text) + 12;
        // 4 px under the header's top: the selected tab is a tab, not a bar.
        tab.rect = QRectF(x, panel.top() + 4, width, height - 4);
        x += width;
        tabs.push_back(tab);
    }
    return tabs;
}

QRectF MarkdownEditor::selectedTabRectAt(qsizetype offset) const
{
    const qsizetype index = blockAt(offset);
    if (index < 0 || index >= qsizetype(m_blocks.size()))
        return {};
    const BlockView &view = m_blocks[size_t(index)];
    for (const TabRect &tab : tabsOf(view, contentLeft(), view.y - verticalScrollBar()->value())) {
        if (tab.selected)
            return tab.rect;
    }
    return {};
}

MarkdownEditor::CodeLabel MarkdownEditor::codeLabelOf(const BlockView &view, qreal left,
                                                      qreal top) const
{
    CodeLabel label;
    // A long fence's label is on its first piece, plain source has none, and
    // an embed's panel is not a listing to copy.
    if (view.piece > 0 || plainSource(view) || view.span.kind != BlockSpan::Kind::Code)
        return label;
    const bool copied = m_copiedBlock == view.span.range.start;
    const bool hovered = m_hoveredCodeBlock == view.span.range.start;
    const bool header = hasCodeHeader(view);

    // The copy button. The header already names the block (codeTitleOf), so
    // this says "Copy" only while the pointer is over the block, or "Copied"
    // just after. Under a header its place is reserved either way, so the run
    // control beside it never moves; a block with no header has no room to
    // reserve and shows it in the corner when hovered.
    if (!header && !copied && !hovered)
        return label;
    label.text = copied ? tr("Copied") : tr("Copy");

    label.font = m_theme.uiFont();
    label.font.setPixelSize(m_theme.value(QStringLiteral("--font-ui-smaller")).toInt());
    label.colour = m_theme.color(QStringLiteral("--text-muted"));
    const QFontMetricsF metrics(label.font);
    const qreal width = metrics.horizontalAdvance(label.text);

    const QRectF panel(left - 14, top - 8, contentWidth() + 28, view.height + 16);
    // With `#|` options, the header's right end is clear of the fold arrow.
    const qreal right = view.options ? panel.right() - 22 - kFoldHandle
                        : header     ? panel.right() - 14
                                     : panel.right() - 10;
    const qreal y = header ? panel.top() + (view.textTop + 8 - metrics.height()) / 2
                           : panel.top() + 4;
    label.rect = QRectF(right - width, y, width, metrics.height());
    label.slotLeft = right - qMax(width, metrics.horizontalAdvance(tr("Copied")));
    label.visible = copied || hovered;
    return label;
}

const MarkdownEditor::BlockView *MarkdownEditor::codeLabelAt(const QPoint &point) const
{
    const qreal scroll = verticalScrollBar()->value();
    const qreal left = contentLeft();
    for (const BlockView &view : m_blocks) {
        const qreal top = view.y - scroll;
        if (top + view.height < 0)
            continue;
        if (top > viewport()->height())
            break;
        if (view.span.kind != BlockSpan::Kind::Code || !view.laidOut)
            continue;
        const CodeLabel label = codeLabelOf(view, left, top);
        if (label.visible && touchable(label.rect, 4).contains(point))
            return &view;
    }
    return nullptr;
}

void MarkdownEditor::copyCodeBlock(const BlockView &view)
{
    // The listing as the reader sees it: no fences, no language tag, and no
    // `#|` option lines, which codeContentOf already leaves out. All of it,
    // for a fence long enough to be in pieces.
    const Range content{codeContentOf(firstPiece(view), nullptr).start,
                        codeContentOf(lastPiece(view), nullptr).end};
    QString code = m_text.mid(content.start, content.length());
    if (code.endsWith(QLatin1Char('\n')))
        code.chop(1);
    QGuiApplication::clipboard()->setText(code);

    m_copiedBlock = firstPiece(view).span.range.start;
    const qsizetype copied = m_copiedBlock;
    QTimer::singleShot(1500, this, [this, copied] {
        if (m_copiedBlock == copied) {
            m_copiedBlock = -1;
            viewport()->update();
        }
    });
    viewport()->update();
}

void MarkdownEditor::paintCodeLabel(QPainter &painter, const BlockView &view, qreal left,
                                    qreal top) const
{
    const CodeLabel label = codeLabelOf(view, left, top);
    if (!label.visible)
        return;
    painter.save();
    painter.setFont(label.font);
    painter.setPen(label.colour);
    painter.drawText(label.rect, Qt::AlignVCenter | Qt::AlignLeft, label.text);
    painter.restore();
}

void MarkdownEditor::paintCodeChrome(QPainter &painter, const BlockView &view, qreal left,
                                     qreal top) const
{
    // One panel behind the whole fence. A long fence is several pieces, and
    // each paints only its own band of that panel, so they join seamlessly.
    const BlockView &first = firstPiece(view);
    const BlockView &closing = lastPiece(view);
    const bool isFirst = view.piece == 0;
    const bool isLast = view.piece == view.pieces - 1;
    const qreal fenceTop = top - (view.y - first.y);
    const QRectF panel(left - 14, fenceTop - 8, contentWidth() + 28,
                       closing.y + closing.height - first.y + 16);

    painter.save();
    if (view.pieces > 1) {
        const qreal bandTop = isFirst ? panel.top() : top;
        const qreal bandBottom = isLast ? panel.bottom() : top + view.height;
        painter.setClipRect(QRectF(panel.left(), bandTop, panel.width(), bandBottom - bandTop));
    }
    QPainterPath rounded;
    rounded.addRoundedRect(panel, 6, 6);
    const LanguageStyle style = codeStyleOf(view);
    painter.fillPath(rounded, style.bg.isValid()
                                  ? style.bg
                                  : m_theme.color(QStringLiteral("--background-primary-alt")));

    if (isFirst)
        paintCodeHeader(painter, view, left, top, panel);
    if (view.options && !view.folded)
        paintCodeRows(painter, view, left, top, panel);

    const qreal caption = codeCaptionHeight(view);
    if (caption > 0) {
        QFont captionFont = m_theme.textFont();
        captionFont.setPixelSize(m_theme.value(QStringLiteral("--font-ui-small")).toInt());
        captionFont.setItalic(true);
        painter.setFont(captionFont);
        painter.setPen(m_theme.color(QStringLiteral("--text-muted")));
        painter.drawText(QRectF(left, panel.bottom() - 8 - caption, contentWidth(), caption),
                         Qt::AlignVCenter | Qt::AlignLeft, view.options->caption);
    }
    if (style.border.isValid()) {
        // Half a pixel in, so the 1 px line lands on whole pixels inside the
        // clip a long fence's pieces paint through.
        painter.setPen(QPen(style.border, 1));
        painter.setBrush(Qt::NoBrush);
        painter.drawRoundedRect(panel.adjusted(0.5, 0.5, -0.5, -0.5), 6, 6);
    }
    painter.restore();
}

void MarkdownEditor::paintCodeHeader(QPainter &painter, const BlockView &view, qreal left,
                                     qreal top, const QRectF &panel) const
{
    if (!hasCodeHeader(view)) {
        // No language to name, no `#|` options, no tabs, not a notebook
        // block: the copy button alone, in the corner.
        paintCodeLabel(painter, view, left, top);
        return;
    }

    QFont badgeFont = m_theme.uiFont();
    badgeFont.setPixelSize(m_theme.value(QStringLiteral("--font-ui-smaller")).toInt());
    const QColor faint = m_theme.color(QStringLiteral("--text-faint"));
    const QColor muted = m_theme.color(QStringLiteral("--text-muted"));

    // The header strip: the block's name on the left, the copy button and run
    // control on the right, and a hairline under it when there is a listing
    // below.
    const QRectF header(panel.left(), panel.top(), panel.width(), view.textTop + 8);
    QPainterPath headerPath;
    headerPath.addRoundedRect(header, 6, 6);
    painter.fillPath(headerPath, m_theme.color(QStringLiteral("--background-secondary")));
    if (!view.folded) {
        painter.fillRect(QRectF(header.left(), header.bottom() - 4, header.width(), 4),
                         m_theme.color(QStringLiteral("--background-secondary")));
        painter.setPen(QPen(m_theme.color(QStringLiteral("--background-modifier-border")), 1));
        painter.drawLine(header.bottomLeft(), header.bottomRight());
    }

    if (view.tabGroup >= 0) {
        // The strip: the tab being shown is attached to the listing under it -
        // a rounded-topped tab in the panel's own colour, edged on three
        // sides, with the hairline under the strip left out beneath it. The
        // others are muted text on the strip. The tabs own the header, so a
        // `#|` title has nowhere to go here and is not drawn.
        QFont tabFont = m_theme.uiFont();
        tabFont.setPixelSize(m_theme.value(QStringLiteral("--font-ui-small")).toInt());
        painter.setFont(tabFont);
        const QColor normal = m_theme.color(QStringLiteral("--text-normal"));
        const QColor panelColour = codeStyleOf(view).bg.isValid()
                                       ? codeStyleOf(view).bg
                                       : m_theme.color(QStringLiteral("--background-primary-alt"));
        const QColor border = m_theme.color(QStringLiteral("--background-modifier-border"));
        for (const TabRect &tab : tabsOf(view, left, top)) {
            if (tab.selected) {
                painter.save();
                // Down to the hairline's row, no further: the rounded rect is
                // taller than the clip so its bottom corners never show.
                painter.setClipRect(tab.rect.adjusted(-1, -1, 1, 1));
                painter.setRenderHint(QPainter::Antialiasing);
                const QRectF body = tab.rect.adjusted(0, 0, 0, 6);
                painter.setPen(Qt::NoPen);
                painter.setBrush(panelColour);
                painter.drawRoundedRect(body, 5, 5);
                painter.setPen(QPen(border, 1));
                painter.setBrush(Qt::NoBrush);
                painter.drawRoundedRect(body.adjusted(0.5, 0.5, -0.5, -0.5), 5, 5);
                // The border's own left and right sides end at the header's
                // bottom; under it the panel carries on without a seam.
                painter.setRenderHint(QPainter::Antialiasing, false);
                painter.fillRect(QRectF(tab.rect.left() + 1, tab.rect.bottom(), tab.rect.width() - 2, 1),
                                 panelColour);
                painter.restore();
            }
            QRectF text = tab.rect.adjusted(12, 0, -12, 0);
            if (!tab.icon.isNull()) {
                const qreal size = QFontMetricsF(tabFont).height();
                tab.icon.paint(&painter, QRectF(text.left(), tab.rect.center().y() - size / 2,
                                                size, size)
                                             .toRect());
                text.setLeft(text.left() + size + 5);
            }
            painter.setPen(tab.selected ? normal : muted);
            painter.drawText(text, Qt::AlignVCenter | Qt::AlignLeft, tab.text);
        }
        paintCodeLabel(painter, view, left, top);
        paintNotebookRunControl(painter, view, left, top);
        paintNotebookHostBadge(painter, view, left, top);
        return;
    }

    // The name: `#| title:`, else the style's (the language, or `name="..."`),
    // with the style's icon in front.
    QFont titleFont = m_theme.uiFont();
    titleFont.setPixelSize(m_theme.value(QStringLiteral("--font-ui-small")).toInt());
    painter.setFont(titleFont);
    QRectF titleRect(header.left() + 14, header.top(), header.width() - 36 - kFoldHandle, header.height());
    QColor titleColour = muted;
    if (const CodeFenceSpan *fence = codeFenceOf(view)) {
        const LanguageStyle style = styleOfFence(*fence);
        if (style.color.isValid())
            titleColour = style.color; // codeblocks.ini's or the fence's own `color=`
        if (!style.icon.isEmpty()) {
            const qreal size = QFontMetricsF(titleFont).height();
            const QIcon icon = Icons::icon(style.icon, titleColour, int(size));
            icon.paint(&painter, QRectF(titleRect.left(), titleRect.center().y() - size / 2, size, size).toRect());
            titleRect.setLeft(titleRect.left() + size + 5);
        }
    }
    painter.setPen(titleColour);
    painter.drawText(titleRect, Qt::AlignVCenter | Qt::AlignLeft, codeTitleOf(view));
    paintCodeLabel(painter, view, left, top);
    paintNotebookRunControl(painter, view, left, top);
    paintNotebookHostBadge(painter, view, left, top);
    if (view.options)
        paintFoldArrow(painter, foldHandleRect(view, left, top), view.folded, faint);
}

void MarkdownEditor::paintCodeRows(QPainter &painter, const BlockView &view, qreal left,
                                   qreal top, const QRectF &panel) const
{
    // Highlighted rows and the line-number gutter, both counted in display
    // lines: the `#|` lines themselves are hidden, so line 1 is the first line
    // of the listing. A later piece of a long fence counts on from where the
    // pieces before it stopped.
    if (!view.layout)
        return;
    int sourceLine = 0;
    if (view.piece > 0) {
        sourceLine = int(m_lines.lineAt(view.span.range.start)
                         - m_lines.lineAt(codeContentOf(firstPiece(view), nullptr).start));
    }

    // `--text-highlight-bg` is meant for a run of words; a whole row of it
    // would drown the listing, so it goes on at a fraction of its alpha.
    QColor tint = findHighlightColor();
    tint.setAlphaF(m_theme.isDark() ? 0.13f : 0.22f);
    const QColor faint = m_theme.color(QStringLiteral("--text-faint"));

    const qreal textTop = top + view.textTop;
    const qreal gutter = codeGutterWidth(view);
    painter.setFont(fontFor(view.span));
    qsizetype previousStart = -1;
    for (int i = 0; i < view.layout->lineCount(); ++i) {
        const QTextLine line = view.layout->lineAt(i);
        const qsizetype start = view.map.toSource(line.textStart());
        const bool wrapped = previousStart >= 0
                             && !QStringView(m_text)
                                     .mid(previousStart, start - previousStart)
                                     .contains(QLatin1Char('\n'));
        if (i > 0 && !wrapped)
            ++sourceLine;
        previousStart = start;

        if (view.options->highlight.contains(sourceLine + 1)) {
            painter.fillRect(QRectF(panel.left(), textTop + line.y(), panel.width(),
                                    line.height()),
                             tint);
        }
        if (gutter > 0 && (i == 0 || !wrapped)) {
            painter.setPen(faint);
            painter.drawText(QRectF(left, textTop + line.y(), gutter - 10, line.height()),
                             Qt::AlignRight | Qt::AlignVCenter,
                             QString::number(view.options->firstLine + sourceLine));
        }
    }
}

} // namespace noteAppC
