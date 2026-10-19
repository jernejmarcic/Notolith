/// Footnotes in the editor, as Obsidian shows them: a reference `[^1]` or
/// `^[inline]` is its number, raised and in the accent colour; a numbered
/// definition's `[^1]:` is a small muted "1." - both drawn over one visible
/// carrier character, the trick inline maths and heading links use. Reading
/// view lists the definitions at the end of the note instead of where they
/// are, each with a way back, and Ctrl+click on either end jumps to the
/// other. The parser has already numbered everything (Parser.cpp's
/// scanFootnotes); nothing here counts.

#include "MarkdownEditor.h"
#include "MarkdownEditor_p.h"

#include <QFontMetricsF>
#include <QPainter>
#include <QScrollBar>
#include <QTextLayout>

#include <algorithm>

namespace noteAppC {

namespace {

/// The arrow back to a footnote's reference, as text rather than an emoji.
QString backReferenceGlyph()
{
    return QString(QChar(0x21A9)) + QChar(0xFE0E);
}

/// The first line of a footnote's text, for a list row or the pane.
QString firstLineOf(const QString &text, const Range &content)
{
    const QString body = text.mid(content.start, content.length()).trimmed();
    const qsizetype newline = body.indexOf(QLatin1Char('\n'));
    return (newline < 0 ? body : body.left(newline)).trimmed();
}

} // namespace

QList<MarkdownEditor::FootnoteEntry> MarkdownEditor::footnotes() const
{
    QList<FootnoteEntry> entries;
    if (d->m_decorations.footnotes.isEmpty())
        return entries;
    QSet<int> seen;
    for (const FootnoteSpan &span : d->m_decorations.footnotes) {
        if (seen.contains(span.number))
            continue;
        seen.insert(span.number);
        FootnoteEntry entry;
        entry.number = span.number;
        entry.label = span.label;
        entry.preview = firstLineOf(d->m_text, span.content);
        // A labelled footnote opens at its definition; an inline one is
        // its own text, where it was written.
        entry.jumpOffset = span.content.isValid() ? span.content.start : span.range.start;
        entries.append(entry);
    }
    std::sort(entries.begin(), entries.end(),
              [](const FootnoteEntry &a, const FootnoteEntry &b) { return a.number < b.number; });
    return entries;
}

QStringList MarkdownEditor::footnoteMarkTexts() const
{
    QStringList texts;
    for (const BlockView &view : d->m_blocks) {
        for (const BlockView::FootnoteMark &mark : view.footnoteMarks)
            texts << mark.text;
    }
    return texts;
}

int MarkdownEditor::footnotesListRowCount() const
{
    for (const BlockView &view : d->m_blocks) {
        if (view.isFootnotesList)
            return int(view.footnoteListRows.size());
    }
    return 0;
}

QFont MarkdownEditor::footnoteMarkFont(const QFont &blockFont, bool superscript) const
{
    QFont font = blockFont;
    const qreal size = blockFont.pixelSize() > 0 ? blockFont.pixelSize() : blockFont.pointSizeF();
    const qreal scaled = qMax<qreal>(8, size * (superscript ? 0.72 : 0.85));
    if (blockFont.pixelSize() > 0)
        font.setPixelSize(int(std::round(scaled)));
    else
        font.setPointSizeF(scaled);
    font.setBold(false);
    font.setItalic(false);
    return font;
}

void MarkdownEditor::layoutFootnotes(BlockView &view, QList<Range> &hidden)
{
    if (d->m_decorations.footnotes.isEmpty() && d->m_decorations.footnoteDefinitions.isEmpty())
        return;
    const auto hides = [&hidden](qsizetype at) {
        return std::any_of(hidden.cbegin(), hidden.cend(), [at](const Range &range) { return range.contains(at); });
    };
    const QColor footnote = d->m_theme.color(QStringLiteral("--footnote-color"));
    const QColor muted = d->m_theme.color(QStringLiteral("--text-muted"));
    for (int i = view.firstStyle; i < view.lastStyle; ++i) {
        const StyleSpan &span = d->m_decorations.styles.at(i);
        if (span.kind != StyleSpan::Kind::Footnote || span.range.length() < 2)
            continue;
        // Only a rendered footnote: with the caret on it the source shows.
        if (!hides(span.range.start + 1))
            continue;
        const auto reference =
            std::find_if(d->m_decorations.footnotes.cbegin(), d->m_decorations.footnotes.cend(),
                         [&span](const FootnoteSpan &footnote) { return footnote.range == span.range; });
        if (reference != d->m_decorations.footnotes.cend()) {
            view.footnoteMarks.push_back({span.range.start, 0, QString::number(reference->number), footnote, true});
            continue;
        }
        const auto definition =
            std::find_if(d->m_decorations.footnoteDefinitions.cbegin(), d->m_decorations.footnoteDefinitions.cend(),
                         [&span](const FootnoteDefinition &def) { return def.range.start == span.range.start; });
        if (definition != d->m_decorations.footnoteDefinitions.cend() && definition->number > 0) {
            view.footnoteMarks.push_back(
                {span.range.start, 0, QString::number(definition->number) + QStringLiteral(". "), muted, false});
        }
    }
}

void MarkdownEditor::paintFootnoteMarks(QPainter &painter, const BlockView &view, qreal left, qreal top) const
{
    if (view.footnoteMarks.empty() || !view.layout)
        return;
    painter.save();
    for (const BlockView::FootnoteMark &mark : view.footnoteMarks) {
        const QTextLine line = view.layout->lineForTextPosition(int(mark.displayPos));
        if (!line.isValid())
            continue;
        const QFont font = footnoteMarkFont(view.layout->font(), mark.superscript);
        painter.setFont(font);
        painter.setPen(mark.colour);
        // A reference sits raised, its top at the line's; a definition's
        // label on the line's own baseline.
        const qreal baseline = mark.superscript ? line.y() + QFontMetricsF(font).ascent() + 1
                                                : line.y() + line.ascent();
        painter.drawText(QPointF(left + line.cursorToX(int(mark.displayPos)), top + baseline), mark.text);
    }
    painter.restore();
}

bool MarkdownEditor::footnoteDefinitionBlock(const BlockView &view) const
{
    for (const FootnoteDefinition &definition : d->m_decorations.footnoteDefinitions) {
        if (definition.number <= 0)
            continue;
        // The block is the definition, or the part of one an indented line
        // continued; the newline after it goes with it.
        // A block may begin on the blank line before the definition.
        const qsizetype start = view.span.range.start;
        const bool begins = start >= definition.range.start
                            || (start < definition.range.start
                                && QStringView(d->m_text).mid(start, definition.range.start - start).trimmed().isEmpty());
        // ... and take the blank lines after it.
        const qsizetype end = view.span.range.end;
        const bool ends = end <= definition.range.end + 1
                          || QStringView(d->m_text).mid(definition.range.end, end - definition.range.end).trimmed().isEmpty();
        if (begins && start < definition.range.end + 1 && ends)
            return true;
    }
    return false;
}

void MarkdownEditor::layoutFootnotesList(BlockView &view)
{
    view.footnoteListRows.clear();
    view.layout.reset();
    view.map = DisplayMap();
    view.textTop = 0;

    QList<const FootnoteDefinition *> numbered;
    for (const FootnoteDefinition &definition : d->m_decorations.footnoteDefinitions) {
        if (definition.number > 0)
            numbered.append(&definition);
    }
    std::sort(numbered.begin(), numbered.end(),
              [](const FootnoteDefinition *a, const FootnoteDefinition *b) { return a->number < b->number; });
    if (numbered.isEmpty()) {
        view.height = 0;
        view.laidOut = true;
        return;
    }

    const QFont font = footnoteMarkFont(d->m_theme.textFont(), false);
    const QFontMetricsF metrics(font);
    const qreal rule = 24; // air, then the rule, above the list
    const qreal width = contentWidth();
    const QString back = backReferenceGlyph();
    const qreal backWidth = metrics.horizontalAdvance(back) + 8;
    qreal y = rule;
    for (const FootnoteDefinition *definition : std::as_const(numbered)) {
        BlockView::FootnotesListRow row;
        const QString body = d->m_text.mid(definition->content.start, definition->content.length()).simplified();
        row.layout = std::make_unique<QTextLayout>(QString::number(definition->number) + QStringLiteral(". ") + body,
                                                   font);
        row.layout->setCacheEnabled(true);
        QTextOption option;
        option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
        row.layout->setTextOption(option);
        row.layout->beginLayout();
        qreal height = 0;
        qreal lastWidth = 0;
        while (true) {
            QTextLine line = row.layout->createLine();
            if (!line.isValid())
                break;
            line.setLineWidth(width - backWidth);
            line.setPosition(QPointF(0, height));
            height += line.height();
            lastWidth = line.naturalTextWidth();
        }
        row.layout->endLayout();
        row.rect = QRectF(0, y, width, qMax(height, metrics.height()));
        row.backRef = QRectF(lastWidth + 4, y + height - metrics.height(), backWidth, metrics.height());
        // The way back: the footnote's first reference.
        const auto first = std::find_if(d->m_decorations.footnotes.cbegin(), d->m_decorations.footnotes.cend(),
                                        [definition](const FootnoteSpan &span) {
                                            return span.label == definition->label;
                                        });
        row.jumpOffset = first != d->m_decorations.footnotes.cend() ? first->range.end : definition->range.start;
        y += row.rect.height() + 4;
        view.footnoteListRows.push_back(std::move(row));
    }
    view.height = y + 8;
    view.laidOut = true;
    view.heightEstimated = false;
}

void MarkdownEditor::paintFootnotesList(QPainter &painter, const BlockView &view, qreal left, qreal top) const
{
    if (view.footnoteListRows.empty())
        return;
    painter.save();
    painter.setPen(QPen(d->m_theme.color(QStringLiteral("--background-modifier-border")), 1));
    painter.drawLine(QPointF(left, top + 12.5), QPointF(left + contentWidth(), top + 12.5));
    const QFont font = footnoteMarkFont(d->m_theme.textFont(), false);
    painter.setFont(font);
    const QColor muted = d->m_theme.color(QStringLiteral("--text-muted"));
    const QColor accent = d->m_theme.color(QStringLiteral("--text-accent"));
    for (const BlockView::FootnotesListRow &row : view.footnoteListRows) {
        painter.setPen(muted);
        row.layout->draw(&painter, QPointF(left + row.rect.x(), top + row.rect.y()));
        painter.setPen(accent);
        painter.drawText(row.backRef.translated(left, top), Qt::AlignLeft | Qt::AlignVCenter,
                         backReferenceGlyph());
    }
    painter.restore();
}

bool MarkdownEditor::jumpToFootnote(const QPoint &viewportPoint)
{
    const qreal scroll = verticalScrollBar()->value();
    const qreal left = contentLeft();
    const auto goTo = [this](qsizetype offset) {
        setCursorOffset(offset);
        ensureCursorVisible();
    };
    for (const BlockView &view : d->m_blocks) {
        const qreal top = view.y - scroll;
        if (view.foldHidden || viewportPoint.y() < top || viewportPoint.y() > top + view.height)
            continue;

        if (view.isFootnotesList) {
            for (const BlockView::FootnotesListRow &row : view.footnoteListRows) {
                if (row.rect.translated(left, top).contains(viewportPoint)) {
                    goTo(row.jumpOffset);
                    return true;
                }
            }
            return false;
        }
        if (!view.layout)
            continue;
        const qreal indent = view.callout ? 14 : (view.embedPanel ? 2 : 0);
        for (const BlockView::FootnoteMark &mark : view.footnoteMarks) {
            const QTextLine line = view.layout->lineForTextPosition(int(mark.displayPos));
            if (!line.isValid())
                continue;
            const QFontMetricsF metrics(footnoteMarkFont(view.layout->font(), mark.superscript));
            const QRectF rect(left + indent + line.cursorToX(int(mark.displayPos)) - 2,
                              top + view.textTop + line.y(), metrics.horizontalAdvance(mark.text) + 4,
                              line.height());
            if (!rect.contains(viewportPoint))
                continue;
            if (mark.superscript) {
                // A reference: to its definition, or in reading view (where
                // the definitions are listed at the end) to its row there.
                const auto reference = std::find_if(
                    d->m_decorations.footnotes.cbegin(), d->m_decorations.footnotes.cend(),
                    [&mark](const FootnoteSpan &span) { return span.range.start == mark.sourcePos; });
                if (reference == d->m_decorations.footnotes.cend() || !reference->content.isValid())
                    return false;
                if (d->m_mode == Mode::Reading) {
                    for (const BlockView &list : d->m_blocks) {
                        if (!list.isFootnotesList)
                            continue;
                        for (const BlockView::FootnotesListRow &row : list.footnoteListRows) {
                            if (row.layout->text().startsWith(QString::number(reference->number) + QLatin1Char('.'))) {
                                verticalScrollBar()->setValue(int(list.y + row.rect.y() - viewport()->height() / 3));
                                return true;
                            }
                        }
                    }
                    return false;
                }
                goTo(reference->content.start);
                return true;
            }
            // A definition's label: back to the first reference.
            const auto definition = std::find_if(
                d->m_decorations.footnoteDefinitions.cbegin(), d->m_decorations.footnoteDefinitions.cend(),
                [&mark](const FootnoteDefinition &def) { return def.range.start == mark.sourcePos; });
            if (definition == d->m_decorations.footnoteDefinitions.cend())
                return false;
            const auto first = std::find_if(d->m_decorations.footnotes.cbegin(), d->m_decorations.footnotes.cend(),
                                            [&definition](const FootnoteSpan &span) {
                                                return span.label == definition->label;
                                            });
            if (first == d->m_decorations.footnotes.cend())
                return false;
            goTo(first->range.end);
            return true;
        }
        return false;
    }
    return false;
}

} // namespace noteAppC
