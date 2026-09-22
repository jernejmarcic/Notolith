/// Block renderers: the three things the parser produces that are not text in
/// a line - Obsidian's frontmatter properties table, the chrome a fenced code
/// block's `#|` options ask for, and the folding a callout, a `:::` container
/// or a `#| fold: true` block can be in.
///
/// They live beside MarkdownEditor.cpp for the same reason the editing
/// commands do: they are all "measure a box, paint a box, turn a click into an
/// `applyEdit`", and the engine file is long enough already.

#include "MarkdownEditor.h"

#include "Icons.h"

#include <QClipboard>
#include <QGuiApplication>
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
    m_folds.insert(view.span.range.start, folded);
    invalidateLayouts();
    updateLayout();
    viewport()->update();
}

void MarkdownEditor::toggleFoldAt(qsizetype sourceOffset)
{
    const qsizetype index = blockAt(sourceOffset);
    if (index < 0 || index >= qsizetype(m_blocks.size()))
        return;
    const BlockView &view = m_blocks.at(size_t(index));
    setFolded(view, !m_folds.value(view.span.range.start, defaultFoldOf(view)));
}

bool MarkdownEditor::isFoldedAt(qsizetype sourceOffset) const
{
    const qsizetype index = blockAt(sourceOffset);
    if (index < 0 || index >= qsizetype(m_blocks.size()))
        return false;
    const BlockView &view = m_blocks.at(size_t(index));
    return m_folds.value(view.span.range.start, defaultFoldOf(view));
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

        if (view.span.kind == BlockSpan::Kind::Code) {
            const CodeLabel label = codeLabelOf(view, left, top);
            if (label.visible && label.rect.adjusted(-4, -3, 4, 3).contains(point)) {
                copyCodeBlock(view);
                return true;
            }
        }

        if (view.span.kind == BlockSpan::Kind::Frontmatter && m_mode != Mode::Source
            && !m_properties.isEmpty()) {
            const QRectF handle = foldHandleRect(view, left, top);
            // Folded, the single row is the only thing left to click.
            if (handle.adjusted(-6, -6, 6, 6).contains(point)
                || (view.folded && top <= point.y() && point.y() <= top + view.height)) {
                setFolded(view, !view.folded);
                return true;
            }
            for (const PropertyRow &row : view.propertyRows) {
                if (row.control.isNull())
                    continue;
                if (row.control.translated(left, top).adjusted(-4, -4, 4, 4).contains(point)) {
                    toggleProperty(row.property);
                    return true;
                }
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
            qreal x = kKeyColumn;
            qreal chipTop = y + (rowHeight - chipHeight) / 2;
            for (const QString &value : property.values) {
                const qreal width = metrics.horizontalAdvance(oneLine(value)) + 2 * kChipPadding;
                if (x > kKeyColumn && x + width > kKeyColumn + valueWidth) {
                    x = kKeyColumn;
                    chipTop += chipHeight + 4;
                }
                row.chips.append(QRectF(x, chipTop, width, chipHeight));
                x += width + 6;
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

        if (!row.chips.isEmpty()) {
            for (int i = 0; i < row.chips.size(); ++i) {
                const QRectF box = row.chips.at(i).translated(left, top);
                QPainterPath rounded;
                rounded.addRoundedRect(box, 4, 4);
                painter.fillPath(rounded, chip);
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
    for (const CodeOptions &options : m_decorations.codeOptions) {
        if (options.block.start == view.span.range.start)
            return &options;
    }
    return nullptr;
}

qreal MarkdownEditor::codeHeaderHeight() const
{
    return lineHeightFor({BlockSpan::Kind::Code, {0, 0}, 0}) * 1.5;
}

qreal MarkdownEditor::codeCaptionHeight(const BlockView &view) const
{
    if (!view.options || view.options->caption.isEmpty() || view.folded)
        return 0;
    return lineHeightFor({BlockSpan::Kind::Code, {0, 0}, 0}) * 1.4;
}

qreal MarkdownEditor::codeGutterWidth(const BlockView &view) const
{
    if (!view.options || !view.options->lineNumbers || !view.layout)
        return 0;
    const int last = view.options->firstLine
                     + int(view.map.text().count(QLatin1Char('\n')));
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
        const QRectF panel(inset, top + view.textTop + band.y() - 4,
                           qMax(qreal(40), left + contentWidth() - 14 - inset),
                           band.height() + 8);
        QPainterPath rounded;
        rounded.addRoundedRect(panel, 6, 6);
        painter.fillPath(rounded, nested.style.background);
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
    }
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
        painter.fillPath(rounded, panelColour);
    }
}

LanguageStyle MarkdownEditor::codeStyleOf(const BlockView &view) const
{
    QString language;
    codeContentOf(view, &language);
    return m_languageStyles.resolve(language);
}

MarkdownEditor::CodeLabel MarkdownEditor::codeLabelOf(const BlockView &view, qreal left,
                                                      qreal top) const
{
    CodeLabel label;
    const LanguageStyle style = codeStyleOf(view);
    const bool copied = m_copiedBlock == view.span.range.start;
    const bool hovered = m_hoveredCodeBlock == view.span.range.start;

    // A block with a language always names it. One without shows "Copy" only
    // while the pointer is over it, so a plain listing is copyable without
    // carrying a label nobody asked for.
    label.text = copied ? tr("Copied") : !style.name.isEmpty() ? style.name
                                     : hovered ? tr("Copy") : QString();
    if (label.text.isEmpty())
        return label;

    label.font = m_theme.uiFont();
    label.font.setPixelSize(m_theme.value(QStringLiteral("--font-ui-smaller")).toInt());
    const QColor faint = m_theme.color(QStringLiteral("--text-faint"));
    const QColor muted = m_theme.color(QStringLiteral("--text-muted"));
    label.colour = hovered || copied ? muted : style.color.isValid() ? style.color : faint;

    const QFontMetricsF metrics(label.font);
    const qreal iconSize = metrics.height();
    if (!copied && !style.icon.isEmpty())
        label.icon = Icons::icon(style.icon, label.colour, int(iconSize));
    const qreal width = metrics.horizontalAdvance(label.text)
                        + (label.icon.isNull() ? 0 : iconSize + 5);

    const QRectF panel(left - 14, top - 8, contentWidth() + 28, view.height + 16);
    // With `#|` options there is a header strip, and the label sits at its
    // right end, clear of the fold arrow; without one it takes the corner.
    const qreal right = view.options ? panel.right() - 22 - kFoldHandle : panel.right() - 10;
    const qreal y = view.options ? panel.top() + (view.textTop + 8 - metrics.height()) / 2
                                 : panel.top() + 4;
    label.rect = QRectF(right - width, y, width, metrics.height());
    label.visible = true;
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
        if (label.visible && label.rect.adjusted(-4, -3, 4, 3).contains(point))
            return &view;
    }
    return nullptr;
}

void MarkdownEditor::copyCodeBlock(const BlockView &view)
{
    // The listing as the reader sees it: no fences, no language tag, and no
    // `#|` option lines, which codeContentOf already leaves out.
    const Range content = codeContentOf(view, nullptr);
    QString code = m_text.mid(content.start, content.length());
    if (code.endsWith(QLatin1Char('\n')))
        code.chop(1);
    QGuiApplication::clipboard()->setText(code);

    m_copiedBlock = view.span.range.start;
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
    QRectF text = label.rect;
    if (!label.icon.isNull()) {
        const qreal size = label.rect.height();
        label.icon.paint(&painter, QRectF(text.left(), text.top(), size, size).toRect());
        text.setLeft(text.left() + size + 5);
    }
    painter.drawText(text, Qt::AlignVCenter | Qt::AlignLeft, label.text);
    painter.restore();
}

void MarkdownEditor::paintCodeChrome(QPainter &painter, const BlockView &view, qreal left,
                                     qreal top) const
{
    const QRectF panel(left - 14, top - 8, contentWidth() + 28, view.height + 16);
    QPainterPath rounded;
    rounded.addRoundedRect(panel, 6, 6);
    painter.fillPath(rounded, m_theme.color(QStringLiteral("--background-primary-alt")));

    QString language;
    codeContentOf(view, &language);

    QFont badgeFont = m_theme.uiFont();
    badgeFont.setPixelSize(m_theme.value(QStringLiteral("--font-ui-smaller")).toInt());
    const QColor faint = m_theme.color(QStringLiteral("--text-faint"));
    const QColor muted = m_theme.color(QStringLiteral("--text-muted"));

    if (!view.options) {
        // No `#|` options: the label alone, in the corner.
        paintCodeLabel(painter, view, left, top);
        return;
    }

    painter.save();
    // The header strip: title on the left, language on the right, and a
    // hairline under it when there is a listing below.
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

    painter.setFont(badgeFont);
    painter.setPen(muted);
    const QRectF titleRect(header.left() + 14, header.top(),
                           header.width() - 36 - kFoldHandle, header.height());
    painter.drawText(titleRect, Qt::AlignVCenter | Qt::AlignLeft, view.options->title);
    paintCodeLabel(painter, view, left, top);
    paintFoldArrow(painter, foldHandleRect(view, left, top), view.folded, faint);

    if (view.folded) {
        painter.restore();
        return;
    }

    // Highlighted rows and the line-number gutter, both counted in display
    // lines: the `#|` lines themselves are hidden, so line 1 is the first line
    // of the listing.
    const qreal textTop = top + view.textTop;
    if (view.layout) {
        // `--text-highlight-bg` is meant for a run of words; a whole row of it
        // would drown the listing, so it goes on at a fraction of its alpha.
        QColor tint = findHighlightColor();
        tint.setAlphaF(m_theme.isDark() ? 0.13f : 0.22f);

        const qreal gutter = codeGutterWidth(view);
        painter.setFont(fontFor(view.span));
        int sourceLine = 0;
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

    const qreal caption = codeCaptionHeight(view);
    if (caption > 0) {
        QFont captionFont = m_theme.textFont();
        captionFont.setPixelSize(m_theme.value(QStringLiteral("--font-ui-small")).toInt());
        captionFont.setItalic(true);
        painter.setFont(captionFont);
        painter.setPen(muted);
        painter.drawText(QRectF(left, panel.bottom() - 8 - caption, contentWidth(), caption),
                         Qt::AlignVCenter | Qt::AlignLeft, view.options->caption);
    }
    painter.restore();
}

} // namespace noteAppC
