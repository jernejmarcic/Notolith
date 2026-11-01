// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej
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
    if (d->m_mode == Mode::Source || view.editedAsSource)
        return;

    QPixmap image;
    QString hint;
    bool centred = false;
    const qsizetype blockStart = firstPiece(view).span.range.start;

    if (const MathSpan *math = d->m_features.maths ? displayMathOf(view) : nullptr) {
        // Drawn without the caret in it, a formula is the block itself
        // (layoutMath); only one that cannot be drawn gets here, to say so.
        const Range source = math->formula.isValid() && math->formula.length() > 0 ? math->formula : math->range;
        const QString latex = d->m_text.mid(source.start, source.length()).trimmed();
        const QColor colour = d->m_theme.color(QStringLiteral("--text-normal"));
        const qreal pointSize = d->m_theme.textFont().pixelSize() * 1.2;
        // layoutMath's own key: a formula drawn while the caret was elsewhere is not drawn again.
        const QString drawnKey = QStringLiteral("math|%1|%2|%3")
                                     .arg(latex)
                                     .arg(d->m_theme.value(QStringLiteral("--font-text-size")))
                                     .arg(contentWidth());
        const QString key = QStringLiteral("%1|%2|%3|%4").arg(drawnKey).arg(pointSize)
                                .arg(colour.name(QColor::HexArgb)).arg(devicePixelRatioF());
        QPixmap drawn = d->m_images.value(drawnKey);
        if (drawn.isNull() && d->m_previewMath.key == key)
            drawn = d->m_previewMath.image;
        QString error = d->m_previewMathErrors.value(key);
        if (drawn.isNull() && error.isEmpty() && !latex.isEmpty()) {
            const QImage rendered = d->m_math.render(latex, pointSize, colour, contentWidth(), devicePixelRatioF());
            if (!rendered.isNull()) {
                drawn = QPixmap::fromImage(rendered);
                d->m_previewMath = {key, drawn, QString()}; // one kept, not one per keystroke
            } else {
                error = d->m_math.lastError().isEmpty() ? tr("cannot be drawn") : d->m_math.lastError();
                if (d->m_previewMathErrors.size() > 32)
                    d->m_previewMathErrors.clear();
                d->m_previewMathErrors.insert(key, error);
            }
        }
        if (view.active) {
            if (!drawn.isNull()) {
                d->m_previewMathGood = drawn;
                d->m_previewMathGoodBlock = blockStart;
            } else if (d->m_previewMathGoodBlock == blockStart) {
                drawn = d->m_previewMathGood; // not parseable yet: what it drew a moment ago holds the room
            }
            image = drawn;
        }
        if (image.isNull() && drawn.isNull() && !latex.isEmpty())
            hint = tr("Math: %1").arg(error.isEmpty() ? tr("cannot be drawn") : error);
        centred = true;
    } else if (view.active && d->m_mode == Mode::LivePreview && view.span.kind == BlockSpan::Kind::Code
               && view.piece == view.pieces - 1 && !d->m_codeBlockRenderers.isEmpty()) {
        const CodeFenceSpan *fence = codeFenceOf(view);
        const QString language = fence ? fence->language.toLower() : QString();
        if (!fence || !d->m_codeBlockRenderers.contains(language))
            return;
        const QString source = d->m_text.mid(fence->content.start, fence->content.length());
        const int width = contentWidth();
        const QString key = codeBlockKeyFor(language, source, width);
        const bool known = d->m_codeBlockCache.contains(key);
        if (known || key == d->m_previewAllowed) {
            // The drawing this replaces is no longer wanted, unless a block still shows it.
            if (!d->m_previewRequested.isEmpty() && d->m_previewRequested != key) {
                const bool shown = std::any_of(d->m_blocks.cbegin(), d->m_blocks.cend(), [this](const BlockView &block) {
                    return block.extras && block.extras->codeBlockKey == d->m_previewRequested;
                });
                if (!shown)
                    d->m_codeBlockCache.remove(d->m_previewRequested);
            }
            QString requested;
            const CodeBlockCacheEntry *entry = requestCodeBlock(language, source, width, &requested);
            d->m_previewRequested = requested;
            view.extrasMut().codeBlockKey = requested; // deliverCodeBlock lays this block out again when the drawing lands
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
        view.extrasMut().previewImage = image;
        view.extrasMut().previewRect = QRectF(centred ? (contentWidth() - width) / 2.0 : 0, view.height + kGap, width, height);
        view.height += kGap + height;
    } else if (!hint.isEmpty()) {
        const qreal height = QFontMetricsF(d->m_theme.textFont()).height();
        view.extrasMut().previewHint = hint;
        view.extrasMut().previewRect = QRectF(0, view.height + kGap / 2, contentWidth(), height);
        view.height += kGap / 2 + height;
    }
}

void MarkdownEditor::paintPreview(QPainter &painter, const BlockView &view, qreal left, qreal top) const
{
    if (view.x().previewRect.isEmpty())
        return;
    const QRectF rect = view.x().previewRect.translated(left, top);
    if (!view.x().previewImage.isNull()) {
        painter.drawPixmap(rect.toRect(), view.x().previewImage);
    } else if (!view.x().previewHint.isEmpty()) {
        painter.save();
        painter.setFont(d->m_theme.textFont());
        painter.setPen(d->m_theme.color(QStringLiteral("--text-muted")));
        painter.drawText(rect, Qt::AlignVCenter | Qt::AlignLeft, view.x().previewHint);
        painter.restore();
    }
}

QSize MarkdownEditor::previewSizeAt(qsizetype offset) const
{
    const qsizetype index = blockAt(offset);
    if (index < 0 || index >= qsizetype(d->m_blocks.size()))
        return {};
    const BlockView &view = lastPiece(d->m_blocks[size_t(index)]);
    return view.x().previewImage.isNull() ? QSize() : view.x().previewRect.size().toSize();
}

QString MarkdownEditor::previewHintAt(qsizetype offset) const
{
    const qsizetype index = blockAt(offset);
    if (index < 0 || index >= qsizetype(d->m_blocks.size()))
        return {};
    return lastPiece(d->m_blocks[size_t(index)]).x().previewHint;
}

} // namespace noteAppC
