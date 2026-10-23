/// What Obsidian keeps on screen while its source is edited: a `$$` formula
/// and a diagram fence (mermaid, tikz, plantuml, dot) stay drawn under the
/// source the caret is in, and follow it as it is typed. A formula whose
/// source MicroTeX cannot draw says why on one dim line where the drawing
/// would be, in the block's own place whether the caret is in it or not.
///
/// Formulas are drawn on the spot (MicroTeX takes milliseconds) and one is
/// kept, not one per keystroke. A diagram is made by an external tool, so it
/// is asked for only once typing has paused, and the last drawing stays until
/// the next arrives. The request itself is the fence renderer's, the same
/// cache entry the fence uses once the caret leaves it.

#include "MarkdownEditor.h"
#include "MarkdownEditor_p.h"

#include <QFontMetricsF>
#include <QPainter>

namespace noteAppC {

namespace {

constexpr qreal kGap = 8;      ///< between the source and what is drawn under it
constexpr int kPauseMs = 500;  ///< typing this long in a diagram's source asks the tool

} // namespace

void MarkdownEditor::appendPreview(BlockView &view)
{
    view.previewImage = QPixmap();
    view.previewHint.clear();
    view.previewRect = QRectF();
    if (d->m_mode == Mode::Source || view.editedAsSource)
        return;

    QPixmap image;
    QString hint;
    bool centred = false;

    if (const MathSpan *math = d->m_features.maths ? displayMathOf(view) : nullptr) {
        // Drawn without the caret in it, a formula is the block itself
        // (layoutMath); only one that cannot be drawn gets here, to say so.
        const Range source = math->formula.isValid() && math->formula.length() > 0 ? math->formula : math->range;
        const QString latex = d->m_text.mid(source.start, source.length()).trimmed();
        const QColor colour = d->m_theme.color(QStringLiteral("--text-normal"));
        const qreal pointSize = d->m_theme.textFont().pixelSize() * 1.2;
        const QString key = QStringLiteral("%1|%2|%3|%4|%5")
                                .arg(latex).arg(pointSize).arg(colour.name(QColor::HexArgb)).arg(contentWidth())
                                .arg(devicePixelRatioF());
        if (d->m_previewMath.key != key) {
            const QImage drawn = d->m_math.render(latex, pointSize, colour, contentWidth(), devicePixelRatioF());
            d->m_previewMath = {key, drawn.isNull() ? QPixmap() : QPixmap::fromImage(drawn),
                                drawn.isNull() ? d->m_math.lastError() : QString()};
        }
        if (view.active && !d->m_previewMath.image.isNull())
            image = d->m_previewMath.image;
        else if (d->m_previewMath.image.isNull() && !latex.isEmpty())
            hint = tr("Math: %1").arg(d->m_previewMath.error.isEmpty() ? tr("cannot be drawn")
                                                                         : d->m_previewMath.error);
        centred = true;
    } else if (view.active && d->m_mode == Mode::LivePreview && view.span.kind == BlockSpan::Kind::Code
               && view.piece == view.pieces - 1 && !d->m_codeBlockRenderers.isEmpty()) {
        const CodeFenceSpan *fence = codeFenceOf(view);
        const QString language = fence ? fence->language.toLower() : QString();
        if (!fence || !d->m_codeBlockRenderers.contains(language))
            return;
        const QString source = d->m_text.mid(fence->content.start, fence->content.length());
        const qsizetype blockStart = firstPiece(view).span.range.start;
        const int width = contentWidth();
        const QString key = language + QLatin1Char('|') + QString::number(width) + QLatin1Char('|')
                            + embedThemeTag() + QLatin1Char('|') + source;
        const bool known = d->m_codeBlockCache.contains(key);
                if (known || key == d->m_previewAllowed) {
            QString requested;
            const CodeBlockCacheEntry *entry = requestCodeBlock(language, source, width, &requested);
                    view.codeBlockKey = requested; // deliverCodeBlock lays this block out again when the drawing lands
            if (entry && !entry->pending && !entry->image.isNull()) {
                image = QPixmap::fromImage(entry->image);
                d->m_previewStale = image;
                d->m_previewStaleBlock = blockStart;
            }
        } else if (key != d->m_previewWanted) {
            d->m_previewWanted = key;
            if (!d->m_previewTimerReady) {
                d->m_previewTimerReady = true;
                d->m_previewTimer.setSingleShot(true);
                connect(&d->m_previewTimer, &QTimer::timeout, this, [this] {
                    d->m_previewAllowed = d->m_previewWanted;
                    for (BlockView &block : d->m_blocks) {
                        if (!block.active)
                            continue;
                        block.layout.reset();
                        block.laidOut = false;
                        block.heightEstimated = false;
                    }
                    updateLayout();
                    viewport()->update();
                });
            }
            d->m_previewTimer.start(kPauseMs);
        }
        if (image.isNull() && d->m_previewStaleBlock == blockStart)
            image = d->m_previewStale;
    } else {
        return;
    }

    if (!image.isNull()) {
        const qreal ratio = image.devicePixelRatio();
        const qreal width = qMin<qreal>(contentWidth(), image.width() / ratio);
        const qreal height = image.height() / ratio;
        view.previewImage = image;
        view.previewRect = QRectF(centred ? (contentWidth() - width) / 2.0 : 0, view.height + kGap, width, height);
        view.height += kGap + height;
    } else if (!hint.isEmpty()) {
        const qreal height = QFontMetricsF(d->m_theme.textFont()).height();
        view.previewHint = hint;
        view.previewRect = QRectF(0, view.height + kGap / 2, contentWidth(), height);
        view.height += kGap / 2 + height;
    }
}

void MarkdownEditor::paintPreview(QPainter &painter, const BlockView &view, qreal left, qreal top) const
{
    if (view.previewRect.isEmpty())
        return;
    const QRectF rect = view.previewRect.translated(left, top);
    if (!view.previewImage.isNull()) {
        painter.drawPixmap(rect.toRect(), view.previewImage);
    } else if (!view.previewHint.isEmpty()) {
        painter.save();
        painter.setFont(d->m_theme.textFont());
        painter.setPen(d->m_theme.color(QStringLiteral("--text-muted")));
        painter.drawText(rect, Qt::AlignVCenter | Qt::AlignLeft, view.previewHint);
        painter.restore();
    }
}

QSize MarkdownEditor::previewSizeAt(qsizetype offset) const
{
    const qsizetype index = blockAt(offset);
    if (index < 0 || index >= qsizetype(d->m_blocks.size()))
        return {};
    const BlockView &view = lastPiece(d->m_blocks[size_t(index)]);
    return view.previewImage.isNull() ? QSize() : view.previewRect.size().toSize();
}

QString MarkdownEditor::previewHintAt(qsizetype offset) const
{
    const qsizetype index = blockAt(offset);
    if (index < 0 || index >= qsizetype(d->m_blocks.size()))
        return {};
    return lastPiece(d->m_blocks[size_t(index)]).previewHint;
}

} // namespace noteAppC
