// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej Marčič
/// PDF page embeds (`![[x.pdf#page=...]]`) and `.tex` embeds.
///
/// A PDF page is rendered by PdfPage off the UI thread - 20-50 ms/page,
/// poppler's own number - so a block never waits on it: it shows a
/// placeholder sized from the real page's own dimensions (a metadata read,
/// not a render, so that part is cheap enough for the UI thread) until the
/// render lands, the same "cache, generation counter, deliver through
/// QMetaObject::invokeMethod" shape QueryBlock.cpp and BaseBlock.cpp already
/// use for their own async work - see QueryBlock.cpp's header comment for
/// why the generation counter exists.
///
/// `.tex` reuses this file's own PDF layout for a document (compiled by
/// TexCompiler, itself off the UI thread and its own cache); maths alone
/// goes through MathRenderer directly, the same as a `$$...$$` block - see
/// layoutTexEmbed.

#include "MarkdownEditor.h"
#include "MarkdownEditor_p.h"
#include "MermaidBlock.h"

#include "PdfPage.h"
#include "Vault.h"

#include <QApplication>
#include <QDesktopServices>
#include <QFile>
#include <QFileInfo>
#include <QPainter>
#include <QNativeGestureEvent>
#include <QPainterPath>
#include <QScroller>
#include <QScrollBar>
#include <QThreadPool>
#include <QUrl>
#include <QtConcurrent>

#include <cmath>
#include <utility>

namespace noteAppC {

namespace {

constexpr qreal kPdfMaxZoom = 4;  ///< PdfPagesArea::setZoom's own ceiling; below 1 a fixed cell only letterboxes
constexpr qreal kPdfWheelStep = 1.1; ///< per wheel notch, as the full viewer
constexpr int kPdfMaxRenderWidth = 2600; ///< beyond this the raster is upscaled

QString pdfPageKey(const QString &path, qint64 mtime, int page, int width)
{
    return QStringLiteral("%1|%2|%3|%4").arg(path).arg(mtime).arg(page).arg(width);
}

/// What a zoomed cell wants rendered: its own width times the zoom, in 5 %
/// buckets so a continuous wheel does not render every frame (PdfPagesArea
/// does the same), capped; never below the base raster.
int pdfZoomWidth(int cellWidth, qreal zoom)
{
    return qMax(cellWidth, qMin(int(cellWidth * (qRound(zoom * 20) / 20.0)), kPdfMaxRenderWidth));
}

/// The visible window's top-left kept inside the cell.
QPointF clampPdfPan(const QPointF &pan, qreal zoom, const QSizeF &cell)
{
    return {qBound<qreal>(0, pan.x(), cell.width() - cell.width() / zoom),
            qBound<qreal>(0, pan.y(), cell.height() - cell.height() / zoom)};
}

} // namespace

bool MarkdownEditor::layoutPdf(BlockView &view, const EmbedSpan &embed, const QString &path)
{
    QFileInfo info(path);
    if (!info.isFile())
        return false;
    const PdfEmbedArgs args = parsePdfEmbedArgs(embed.subpath);
    return buildPdfBlock(view, path, info.fileName(), args, embed.width);
}

bool MarkdownEditor::layoutTexEmbed(BlockView &view, const EmbedSpan &embed, const QString &path)
{
    QFileInfo info(path);
    if (!info.isFile())
        return false;

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return false;
    const QString source = QString::fromUtf8(file.readAll());

    if (!TexCompiler::isFullDocument(source)) {
        // Maths alone: MicroTeX draws it directly, exactly as a `$$...$$`
        // block does - no process, no PDF, no cache directory involved.
        const QString latex = source.trimmed();
        if (latex.isEmpty())
            return false;
        const qreal ratio = devicePixelRatioF();
        const QString key = QStringLiteral("tex-math|%1|%2|%3")
                                .arg(path)
                                .arg(info.lastModified().toSecsSinceEpoch())
                                .arg(contentWidth());
        QPixmap pixmap = d->m_images.value(key);
        if (pixmap.isNull()) {
            const QImage image = d->m_math.render(latex, d->m_theme.textFont().pixelSize() * 1.2,
                                               d->m_theme.color(QStringLiteral("--text-normal")),
                                               contentWidth(), ratio);
            if (image.isNull())
                return false; // falls back to showing the source, as layoutMath does
            pixmap = QPixmap::fromImage(image);
            d->m_images.insert(key, pixmap);
        }
        const int width = int(pixmap.width() / pixmap.devicePixelRatio());
        const int height = int(pixmap.height() / pixmap.devicePixelRatio());
        view.extrasMut().image = pixmap;
        view.extrasMut().imageSize = QSize(width, height);
        view.extrasMut().imageCentred = true;
        view.extrasMut().imagePath = path;
        view.layout.reset();
        view.extrasMut().cells.clear();
        view.height = height + 12;
        hideWholeBlockText(view);
        view.laidOut = true;
        return true;
    }

    // A vault that is not trusted does not get a process started by being looked at: the source, and a click.
    const QString gateSource = QStringLiteral("%% %1\n").arg(path) + source;
    if (d->m_processGate && !d->m_processGate(QStringLiteral("tex"), gateSource, false))
        return layoutTexNotRun(view, info.fileName(), gateSource, source);

    if (!TexCompiler::isAvailable()) {
        return layoutTextPanel(
            view, info.fileName(),
            tr("No TeX engine found on PATH (install a TeX distribution) - showing the source.\n\n")
                + source,
            true);
    }

    const qint64 mtime = info.lastModified().toSecsSinceEpoch();
    const TexCacheEntry &entry = ensureTexCompiled(path, mtime);
    if (entry.pending)
        return layoutTextPanel(view, info.fileName(), tr("Compiling with pdflatex…"), false);
    if (!entry.result.ok) {
        const QString message = !entry.result.error.isEmpty()
                                    ? entry.result.error
                                    : tr("pdflatex failed:\n\n%1").arg(entry.result.log);
        return layoutTextPanel(view, info.fileName(), message, true);
    }

    const PdfEmbedArgs args = parsePdfEmbedArgs(embed.subpath);
    return buildPdfBlock(view, entry.result.pdfPath, info.fileName(), args, embed.width);
}

bool MarkdownEditor::layoutTexNotRun(BlockView &view, const QString &name, const QString &gateSource,
                                     const QString &source)
{
    const int width = contentWidth();
    const QString key = codeBlockKeyFor(QStringLiteral("tex"), gateSource, width);
    auto it = d->m_codeBlockCache.find(key);
    if (it == d->m_codeBlockCache.end() || it->image.isNull()) {
        CodeBlockResult shown = diagramNotRendered(tr("Render"), source, width, devicePixelRatioF(), d->m_theme);
        CodeBlockCacheEntry entry;
        entry.generation = ++d->m_codeBlockGeneration;
        entry.pending = false;
        entry.image = std::move(shown.image);
        entry.hotspots = std::move(shown.hotspots);
        // The answer is a relayout: the gate says yes now, so layoutTexEmbed compiles. No answer, no change.
        entry.activate = [this, gateSource](int, const std::function<void(CodeBlockResult)> &deliver) {
            if (!d->m_processGate || !d->m_processGate(QStringLiteral("tex"), gateSource, true))
                return;
            CodeBlockResult done;
            done.isMarkdown = false;
            deliver(std::move(done));
        };
        pruneCodeBlockCache(key);
        it = d->m_codeBlockCache.insert(key, std::move(entry));
    }
    view.isPluginBlock = true;
    view.extrasMut().codeBlockKey = key;
    view.extrasMut().embedLabel = name;
    view.extrasMut().embedPanel = true;
    view.layout.reset();
    view.extrasMut().cells.clear();
    view.textTop = codeHeaderHeight();
    hideWholeBlockText(view);
    const QPixmap pixmap = QPixmap::fromImage(it->image);
    const qreal ratio = pixmap.devicePixelRatio();
    view.extrasMut().image = pixmap;
    view.extrasMut().imageSize = QSize(qMin(width, int(pixmap.width() / ratio)), int(pixmap.height() / ratio));
    view.extrasMut().imageCentred = false;
    view.height = view.x().imageSize.height() + view.textTop;
    view.laidOut = true;
    return true;
}

void MarkdownEditor::pdfMetadata(const QString &path, qint64 mtime, int &count,
                                 QList<QSizeF> &sizes) const
{
    const QString key = QStringLiteral("%1|%2").arg(path).arg(mtime);
    auto it = d->m_pdfMetaCache.find(key);
    if (it == d->m_pdfMetaCache.end()) {
        PdfMeta meta;
        meta.count = PdfPage::pageCount(path);
        meta.sizes.reserve(meta.count);
        for (int page = 0; page < meta.count; ++page)
            meta.sizes.append(PdfPage::pageSize(path, page));
        it = d->m_pdfMetaCache.insert(key, meta);
    }
    count = it->count;
    sizes = it->sizes;
}

bool MarkdownEditor::buildPdfBlock(BlockView &view, const QString &pdfPath, const QString &label,
                                   const PdfEmbedArgs &args, int requestedWidth)
{
    view.extrasMut().isPdfEmbed = true;
    view.extrasMut().pdfPath = pdfPath;
    view.extrasMut().pdfArgs = args;
    view.extrasMut().pdfCells.clear();
    view.extrasMut().pdfError.clear();
    view.extrasMut().pdfPrevRect = QRectF();
    view.extrasMut().pdfNextRect = QRectF();
    view.extrasMut().pdfCounterText.clear();
    view.extrasMut().image = QPixmap();
    view.layout.reset();
    view.extrasMut().cells.clear();
    view.extrasMut().embedLabel = label;
    view.extrasMut().embedPanel = true;
    view.textTop = codeHeaderHeight();
    hideWholeBlockText(view); // every path below returns true - see the comment on hideWholeBlockText

    if (!args.ok()) {
        view.extrasMut().pdfError = args.error;
        view.height = view.textTop + 24;
        view.laidOut = true;
        return true;
    }

    const qint64 mtime = QFileInfo(pdfPath).lastModified().toSecsSinceEpoch();
    view.extrasMut().pdfMtime = mtime;

    int count = 0;
    QList<QSizeF> sizes;
    pdfMetadata(pdfPath, mtime, count, sizes);
    view.extrasMut().pdfPageCount = count;
    if (count <= 0) {
        // No poppler to ask (a build without poppler-qt6) is not a broken file.
        view.extrasMut().pdfError = PdfPage::isAvailable() ? tr("could not open as a PDF")
                                               : tr("PDF viewing needs Notolith built with poppler-qt6");
        view.height = view.textTop + 24;
        view.laidOut = true;
        return true;
    }

    const QList<int> pages = resolvePdfPages(args, count);
    if (pages.isEmpty()) {
        view.extrasMut().pdfError = tr("no pages to show");
        view.height = view.textTop + 24;
        view.laidOut = true;
        return true;
    }

    const int fullWidth = qMin(requestedWidth > 0 ? requestedWidth : contentWidth(), contentWidth());
    // A zoomed embed shows a finished zoomed raster when there is one; it is
    // only looked up here - the settle timer requests them, so a burst of
    // relayouts cannot start a burst of renders.
    const qreal zoom = d->m_pdfZoom.value(view.span.range.start).zoom;
    const auto imageFor = [&](int page, int width) {
        const QPixmap base = ensurePdfPageAsync(pdfPath, mtime, page, width);
        if (zoom == 1)
            return base;
        const auto it = d->m_pdfPageCache.constFind(pdfPageKey(pdfPath, mtime, page, pdfZoomWidth(width, zoom)));
        return it != d->m_pdfPageCache.cend() && !it->pending && !it->image.isNull() ? it->image : base;
    };
    const auto pointSizeOf = [&sizes](int page) {
        return page >= 0 && page < sizes.size() ? sizes.at(page) : QSizeF(612, 792);
    };

    if (view.x().pdfArgs.layout == PdfEmbedArgs::Layout::Slides) {
        const qsizetype key = view.span.range.start;
        const int index = qBound(0, d->m_pdfSlideIndex.value(key, 0), int(pages.size()) - 1);
        d->m_pdfSlideIndex.insert(key, index);
        view.extrasMut().pdfSlideIndex = index;
        view.extrasMut().pdfSlideCount = int(pages.size());
        view.extrasMut().pdfCounterText = tr("%1 / %2").arg(index + 1).arg(pages.size());

        const int page = pages.at(index);
        const QSizeF pointSize = pointSizeOf(page);
        const int cellHeight = args.height > 0
                                   ? args.height
                                   : int(qreal(fullWidth) * pointSize.height()
                                         / qMax<qreal>(1, pointSize.width()));
        BlockExtras::PdfCell cell;
        cell.page = page;
        cell.rect = QRectF(0, 0, fullWidth, cellHeight);
        cell.pointSize = pointSize;
        cell.image = imageFor(page, fullWidth);
        view.extrasMut().pdfCells.push_back(cell);

        constexpr qreal kNavHeight = 26;
        view.extrasMut().pdfPrevRect = QRectF(0, cellHeight + 6, 70, kNavHeight);
        view.extrasMut().pdfNextRect = QRectF(fullWidth - 70, cellHeight + 6, 70, kNavHeight);
        view.height = view.textTop + cellHeight + kNavHeight + 12;
    } else if (view.x().pdfArgs.layout == PdfEmbedArgs::Layout::Grid) {
        const int columns = qMax(1, args.columns);
        constexpr qreal gap = 10;
        const qreal cellWidth = (fullWidth - gap * (columns - 1)) / columns;
        qreal y = 0;
        for (int rowStart = 0; rowStart < pages.size(); rowStart += columns) {
            const int rowEnd = qMin(rowStart + columns, int(pages.size()));
            qreal rowHeight = 0;
            for (int i = rowStart; i < rowEnd; ++i) {
                const int page = pages.at(i);
                const QSizeF pointSize = pointSizeOf(page);
                const qreal cellHeight = args.height > 0
                                            ? args.height
                                            : cellWidth * pointSize.height()
                                                  / qMax<qreal>(1, pointSize.width());
                BlockExtras::PdfCell cell;
                cell.page = page;
                cell.rect = QRectF((i - rowStart) * (cellWidth + gap), y, cellWidth, cellHeight);
                cell.pointSize = pointSize;
                cell.image = imageFor(page, int(cellWidth));
                view.extrasMut().pdfCells.push_back(cell);
                rowHeight = qMax(rowHeight, cellHeight);
            }
            y += rowHeight + gap;
        }
        view.height = view.textTop + qMax<qreal>(0, y - gap);
    } else { // Column
        constexpr qreal gap = 12;
        qreal y = 0;
        for (int page : pages) {
            const QSizeF pointSize = pointSizeOf(page);
            const qreal cellHeight = args.height > 0
                                        ? args.height
                                        : fullWidth * pointSize.height()
                                              / qMax<qreal>(1, pointSize.width());
            BlockExtras::PdfCell cell;
            cell.page = page;
            cell.rect = QRectF(0, y, fullWidth, cellHeight);
            cell.pointSize = pointSize;
            cell.image = imageFor(page, fullWidth);
            view.extrasMut().pdfCells.push_back(cell);
            y += cellHeight + gap;
        }
        view.height = view.textTop + qMax<qreal>(0, y - gap);
    }

    view.laidOut = true;
    return true;
}

QPixmap MarkdownEditor::ensurePdfPageAsync(const QString &path, qint64 mtime, int page, int width)
{
    const QString key = pdfPageKey(path, mtime, page, width);
    auto it = d->m_pdfPageCache.find(key);
    if (it != d->m_pdfPageCache.end())
        return it->pending ? QPixmap() : it->image;

    PdfPageCacheEntry entry;
    entry.pending = true;
    entry.generation = ++d->m_pdfGeneration;
    it = d->m_pdfPageCache.insert(key, entry);
    const quint64 generation = entry.generation;

    // Through m_lifeline: a render that outlives the editor (its tab was
    // closed, say) is dropped rather than posted to freed memory.
    (void)QtConcurrent::run([this, lifeline = d->m_lifeline, key, generation, path, page, width] {
        QImage image = PdfPage::render(path, page, width);
        lifeline->post(this, [this, key, generation, path, image = std::move(image)]() mutable {
            deliverPdfPage(key, generation, path, std::move(image));
        });
    });
    return QPixmap();
}

void MarkdownEditor::deliverPdfPage(const QString &key, quint64 generation, const QString &path,
                                    QImage image)
{
    auto it = d->m_pdfPageCache.find(key);
    // A newer request for the same (path, mtime, page, width) landed first -
    // cannot happen today (nothing re-requests before delivery), kept for
    // the same reason QueryBlock's own generation guard is kept: so a future
    // caller that does re-request stays safe by construction.
    if (it == d->m_pdfPageCache.end() || it->generation != generation)
        return;
    it->pending = false;
    it->bytes = image.sizeInBytes();
    it->image = image.isNull() ? QPixmap() : QPixmap::fromImage(image);
    if (it->zoomed) {
        d->m_pdfZoomKeys.append(key);
        d->m_pdfZoomBytes += it->bytes;
        // The newest stays, so a raster bigger than the whole budget is
        // still shown once rather than rendered and dropped in a loop.
        while (d->m_pdfZoomBytes > d->m_pdfZoomBudget && d->m_pdfZoomKeys.size() > 1) {
            d->m_pdfZoomBytes -= d->m_pdfPageCache.take(d->m_pdfZoomKeys.takeFirst()).bytes;
        }
    }

    bool touched = false;
    for (BlockView &block : d->m_blocks) {
        if (!block.extras || !block.extras->isPdfEmbed || block.extras->pdfPath != path)
            continue;
        block.layout.reset();
        block.laidOut = false;
        block.heightEstimated = false;
        touched = true;
    }
    if (touched) {
        updateLayout();
        viewport()->update();
    }
}

const MarkdownEditor::TexCacheEntry &MarkdownEditor::ensureTexCompiled(const QString &path,
                                                                        qint64 mtime)
{
    const QString key = QStringLiteral("%1|%2").arg(path).arg(mtime);
    auto it = d->m_texCache.find(key);
    if (it != d->m_texCache.end())
        return *it;

    TexCacheEntry entry;
    entry.pending = true;
    entry.generation = ++d->m_texGeneration;
    it = d->m_texCache.insert(key, entry);
    const quint64 generation = entry.generation;

    // Two compiles at a time, as the diagrams: a note of thirty embeds on a 24-thread machine would otherwise
    // start twenty-four sandboxed TeX runs at once, each with its own gigabyte.
    static QThreadPool texPool;
    static const bool sized = (texPool.setMaxThreadCount(2), true);
    Q_UNUSED(sized);
    (void)QtConcurrent::run(&texPool, [this, lifeline = d->m_lifeline, key, generation, path] {
        TexCompiler::Result result = TexCompiler::compile(path);
        lifeline->post(this, [this, key, generation, result = std::move(result)]() mutable {
            deliverTexCompiled(key, generation, std::move(result));
        });
    });
    return *it;
}

void MarkdownEditor::deliverTexCompiled(const QString &key, quint64 generation,
                                        TexCompiler::Result result)
{
    auto it = d->m_texCache.find(key);
    if (it == d->m_texCache.end() || it->generation != generation)
        return;
    it->result = std::move(result);
    it->pending = false;

    // Every block currently showing a .tex embed's placeholder or error
    // needs a chance to re-check, whichever .tex file this was - relaying
    // out all of them is simpler than tracking each block's own source path
    // for what is, in practice, at most a handful of .tex embeds in a note.
    bool touched = false;
    for (BlockView &block : d->m_blocks) {
        if (!block.extras || !block.extras->isPdfEmbed)
            continue;
        block.layout.reset();
        block.laidOut = false;
        block.heightEstimated = false;
        touched = true;
    }
    if (touched) {
        updateLayout();
        viewport()->update();
    }
}

void MarkdownEditor::paintPdfEmbed(QPainter &painter, const BlockView &view, qreal left,
                                   qreal top) const
{
    const QColor muted = d->m_theme.color(QStringLiteral("--text-muted"));
    const QColor placeholder = d->m_theme.color(QStringLiteral("--background-modifier-border"));

    if (!view.x().pdfError.isEmpty()) {
        painter.save();
        painter.setFont(d->m_theme.textFont());
        painter.setPen(muted);
        painter.drawText(QRectF(left, top + view.textTop, contentWidth(),
                                qMax<qreal>(24, view.height - view.textTop)),
                         Qt::AlignLeft | Qt::TextWordWrap, view.x().pdfError);
        painter.restore();
        return;
    }

    const Private::PdfZoom zoom = d->m_pdfZoom.value(view.span.range.start);
    for (const BlockExtras::PdfCell &cell : view.x().pdfCells) {
        const QRectF rect = cell.rect.translated(left, top + view.textTop);
        if (cell.image.isNull()) {
            painter.fillRect(rect, placeholder);
            painter.save();
            painter.setPen(muted);
            painter.drawText(rect, Qt::AlignCenter, tr("Rendering…"));
            painter.restore();
            continue;
        }
        if (zoom.zoom == 1) {
            painter.drawPixmap(rect.toRect(), cell.image);
            continue;
        }
        // The raster is whichever one the cell holds, so the window is cut
        // in its own pixels - the same cut at the base size and the hi-res one.
        const QPointF pan = clampPdfPan(zoom.pan, zoom.zoom, rect.size());
        const QSizeF scale(cell.image.width() / rect.width(), cell.image.height() / rect.height());
        painter.drawPixmap(rect.toRect(), cell.image,
                           QRectF(pan.x() * scale.width(), pan.y() * scale.height(),
                                  rect.width() / zoom.zoom * scale.width(),
                                  rect.height() / zoom.zoom * scale.height()));
        // A raster short of the zoom is a stretched one: have the settle
        // timer fetch it (a page scrolled into view at this zoom, say).
        const int cellWidth = int(cell.rect.width());
        if (!d->m_pdfZoomSettle.isActive()
            && !d->m_pdfPageCache.contains(pdfPageKey(view.x().pdfPath, view.x().pdfMtime, cell.page,
                                                      pdfZoomWidth(cellWidth, zoom.zoom))))
            d->m_pdfZoomSettle.start();
    }
    if (zoom.zoom != 1 && !view.x().pdfCells.empty()) {
        // Only feedback that a drag pans now; it takes no input.
        painter.save();
        QFont small = d->m_theme.uiFont();
        small.setPixelSize(d->m_theme.value(QStringLiteral("--font-ui-smaller")).toInt());
        painter.setFont(small);
        const QRectF first = view.x().pdfCells.front().rect.translated(left, top + view.textTop);
        const QString text = tr("%1%").arg(qRound(zoom.zoom * 100));
        const QRectF badge(first.right() - 8 - QFontMetricsF(small).horizontalAdvance(text) - 12,
                           first.top() + 8, QFontMetricsF(small).horizontalAdvance(text) + 12, 20);
        painter.setPen(Qt::NoPen);
        painter.setBrush(d->m_theme.color(QStringLiteral("--background-primary-alt")));
        painter.drawRoundedRect(badge, 4, 4);
        painter.setPen(muted);
        painter.drawText(badge, Qt::AlignCenter, text);
        painter.restore();
    }

    if (view.x().pdfArgs.layout == PdfEmbedArgs::Layout::Slides && !view.x().pdfCells.empty()) {
        painter.save();
        QFont small = d->m_theme.uiFont();
        small.setPixelSize(d->m_theme.value(QStringLiteral("--font-ui-smaller")).toInt());
        painter.setFont(small);
        painter.setPen(muted);
        const QRectF prev = view.x().pdfPrevRect.translated(left, top + view.textTop);
        const QRectF next = view.x().pdfNextRect.translated(left, top + view.textTop);
        if (view.x().pdfSlideIndex > 0)
            painter.drawText(prev, Qt::AlignVCenter | Qt::AlignLeft, tr("< Previous"));
        if (view.x().pdfSlideIndex + 1 < view.x().pdfSlideCount)
            painter.drawText(next, Qt::AlignVCenter | Qt::AlignRight, tr("Next >"));
        painter.drawText(QRectF(left, prev.top(), contentWidth(), prev.height()), Qt::AlignCenter,
                         view.x().pdfCounterText);
        painter.restore();
    }
}

namespace {

constexpr qreal kPdfDeadBand = 10;      ///< a click this near the slide's bottom edge is swallowed
constexpr qreal kPdfStickyShare = 0.25; ///< the slide's bottom share a just-used button keeps
constexpr qint64 kPdfStickyMs = 1000;   ///< ... for this long after its click

} // namespace

std::optional<int> MarkdownEditor::pdfNavHit(const BlockView &view, const QPointF &local) const
{
    if (!view.x().isPdfEmbed || view.x().pdfArgs.layout != PdfEmbedArgs::Layout::Slides || view.x().pdfCells.empty())
        return std::nullopt;
    const QRectF cell = view.x().pdfCells.front().rect;
    if (local.x() < cell.left() || local.x() > cell.right() || local.y() > view.height - view.textTop)
        return std::nullopt;
    // The left half of the row under the slide steps back, the right half
    // forward. Above it is the slide: a button just used keeps its half of
    // the bottom quarter for a moment, and a band of dead pixels guards the edge.
    const int side = local.x() < cell.center().x() ? -1 : 1;
    if (local.y() >= cell.bottom())
        return side;
    const auto last = d->m_pdfNavLast.constFind(view.span.range.start);
    if (last != d->m_pdfNavLast.cend() && last->direction == side
        && d->m_pdfNavNow() - last->at < kPdfStickyMs
        && local.y() >= cell.bottom() - cell.height() * kPdfStickyShare)
        return side;
    const QRectF band(cell.left(), cell.bottom() - kPdfDeadBand, cell.width(), kPdfDeadBand);
    return touchable(band).contains(local) ? std::optional<int>(0) : std::nullopt;
}

bool MarkdownEditor::pdfNavCanStep(const BlockView &view, int direction) const
{
    return direction < 0 ? view.x().pdfSlideIndex > 0 : view.x().pdfSlideIndex + 1 < view.x().pdfSlideCount;
}

bool MarkdownEditor::handlePdfEmbedClick(const BlockView &view, const QPoint &point, qreal left,
                                         qreal top)
{
    const std::optional<int> hit = pdfNavHit(view, QPointF(point) - QPointF(left, top + view.textTop));
    if (!hit)
        return false;
    // A hidden button's half and the dead band swallow the click, nothing more.
    if (*hit != 0 && pdfNavCanStep(view, *hit)) {
        const qsizetype key = view.span.range.start;
        pdfSlideStep(key, *hit);
        d->m_pdfNavLast.insert(key, {d->m_pdfNavNow(), *hit});
    }
    return true;
}

std::optional<Qt::CursorShape> MarkdownEditor::pdfNavCursorAt(const QPoint &point) const
{
    const qsizetype index = blockAtPoint(point);
    if (index < 0)
        return std::nullopt;
    const BlockView &view = d->m_blocks[size_t(index)];
    const QPointF origin(contentLeft(), view.y - verticalScrollBar()->value() + view.textTop);
    const QPointF local = QPointF(point) - origin;
    const std::optional<int> hit = pdfNavHit(view, local);
    if (!hit) {
        // Over a zoomed page a drag pans it.
        const bool onPage = view.x().isPdfEmbed && d->m_pdfZoom.contains(view.span.range.start)
                            && std::any_of(view.x().pdfCells.cbegin(), view.x().pdfCells.cend(),
                                           [&local](const BlockExtras::PdfCell &cell) { return cell.rect.contains(local); });
        return onPage ? std::optional<Qt::CursorShape>(Qt::OpenHandCursor) : std::nullopt;
    }
    return *hit != 0 && pdfNavCanStep(view, *hit) ? Qt::PointingHandCursor : Qt::ArrowCursor;
}

MarkdownEditor::PdfEmbedInfo MarkdownEditor::pdfEmbedInfoAt(qsizetype offset) const
{
    PdfEmbedInfo info;
    const qsizetype index = blockAt(offset);
    if (index < 0 || index >= qsizetype(d->m_blocks.size()))
        return info;
    const BlockView &view = d->m_blocks[size_t(index)];
    if (!view.x().isPdfEmbed)
        return info;
    info.isPdfEmbed = true;
    info.error = view.x().pdfError;
    info.pageCount = view.x().pdfPageCount;
    info.cellCount = int(view.x().pdfCells.size());
    info.layout = view.x().pdfArgs.layout;
    info.slideIndex = view.x().pdfSlideIndex;
    info.slideCount = view.x().pdfSlideCount;
    const Private::PdfZoom zoom = d->m_pdfZoom.value(view.span.range.start);
    info.zoom = zoom.zoom;
    info.pan = zoom.pan;
    if (!view.x().pdfCells.empty()) {
        info.renderedWidth = view.x().pdfCells.front().image.width();
        const QPointF origin(contentLeft(), view.y - verticalScrollBar()->value() + view.textTop);
        info.cellRect = view.x().pdfCells.front().rect.translated(origin);
        info.nextRect = view.x().pdfNextRect.translated(origin);
    }
    return info;
}

void MarkdownEditor::setPdfNavClock(std::function<qint64()> nowMs)
{
    d->m_pdfNavNow = std::move(nowMs);
}

void MarkdownEditor::pdfSlideStep(qsizetype offset, int direction)
{
    const qsizetype index = blockAt(offset);
    if (index < 0 || index >= qsizetype(d->m_blocks.size()))
        return;
    BlockView &view = d->m_blocks[size_t(index)];
    if (!view.x().isPdfEmbed || view.x().pdfSlideCount <= 0)
        return;
    const qsizetype key = view.span.range.start;
    const int current = d->m_pdfSlideIndex.value(key, 0);
    const int next = qBound(0, current + direction, view.x().pdfSlideCount - 1);
    if (next == current)
        return;
    d->m_pdfSlideIndex.insert(key, next);
    view.layout.reset();
    view.laidOut = false;
    view.heightEstimated = false;
    updateLayout();
    viewport()->update();
}

void MarkdownEditor::setPdfZoomCacheBudget(qint64 bytes)
{
    d->m_pdfZoomBudget = bytes;
}

qint64 MarkdownEditor::pdfZoomCacheBytes() const
{
    return d->m_pdfZoomBytes;
}

int MarkdownEditor::pdfCellAt(const QPoint &point, qsizetype *block, QPointF *blockLocal) const
{
    const qsizetype index = blockAtPoint(point);
    if (index < 0)
        return -1;
    const BlockView &view = d->m_blocks[size_t(index)];
    if (!view.x().isPdfEmbed)
        return -1;
    const QPointF local = QPointF(point)
                          - QPointF(contentLeft(), view.y - verticalScrollBar()->value() + view.textTop);
    for (size_t cell = 0; cell < view.x().pdfCells.size(); ++cell) {
        if (view.x().pdfCells[cell].rect.contains(local)) {
            *block = index;
            *blockLocal = local;
            return int(cell);
        }
    }
    return -1;
}

void MarkdownEditor::invalidatePdfBlock(qsizetype block)
{
    BlockView &view = d->m_blocks[size_t(block)];
    view.layout.reset();
    view.laidOut = false;
    view.heightEstimated = false;
}

bool MarkdownEditor::zoomPdfAt(const QPoint &point, qreal factor)
{
    qsizetype block = -1;
    QPointF local;
    const int cell = pdfCellAt(point, &block, &local);
    if (cell < 0)
        return false;
    const BlockView &view = d->m_blocks[size_t(block)];
    const QRectF rect = view.x().pdfCells[size_t(cell)].rect;
    const qsizetype key = view.span.range.start;
    Private::PdfZoom state = d->m_pdfZoom.value(key);
    qreal zoom = qBound<qreal>(1, state.zoom * factor, kPdfMaxZoom);
    if (qAbs(zoom - 1) < 1e-6)
        zoom = 1;
    if (zoom == state.zoom)
        return true; // at an end of the range: still this page's gesture
    // The content point under the pointer stays under it.
    const QPointF anchor = local - rect.topLeft();
    const QPointF under = state.pan + anchor / state.zoom;
    state.zoom = zoom;
    state.pan = zoom == 1 ? QPointF() : clampPdfPan(under - anchor / zoom, zoom, rect.size());
    if (zoom == 1) {
        d->m_pdfZoom.remove(key);
        invalidatePdfBlock(block); // back to the base raster
        updateLayout();
    } else {
        d->m_pdfZoom.insert(key, state);
        d->m_pdfZoomSettle.start();
    }
    viewport()->update();
    return true;
}

bool MarkdownEditor::wheelPdfCell(QWheelEvent *event)
{
    const bool ctrl = event->modifiers().testFlag(Qt::ControlModifier);
    if (!ctrl && d->m_pdfZoom.isEmpty())
        return false;
    const QPoint pos = event->position().toPoint();
    if (ctrl)
        return zoomPdfAt(pos, std::pow(kPdfWheelStep, event->angleDelta().y() / 120.0));
    // A plain vertical wheel always scrolls the note, zoomed or not: an
    // embed must never leave the page stuck. A horizontal wheel, or shift
    // with a vertical one, pans - the gesture a wide table answers to.
    qsizetype block = -1;
    QPointF local;
    if (pdfCellAt(pos, &block, &local) < 0 || !d->m_pdfZoom.contains(d->m_blocks[size_t(block)].span.range.start))
        return false;
    const QPoint pixels = event->pixelDelta();
    const QPoint degrees = event->angleDelta();
    const qreal dx = pixels.x() != 0 ? pixels.x() : degrees.x() / 2.0;
    const qreal dy = pixels.y() != 0 ? pixels.y() : degrees.y() / 2.0;
    const bool shift = event->modifiers().testFlag(Qt::ShiftModifier);
    if (qFuzzyIsNull(dx) && !(shift && !qFuzzyIsNull(dy)))
        return false;
    panPdfEmbed(block, QPointF(dx, shift ? dy : 0));
    return true;
}

void MarkdownEditor::panPdfEmbed(qsizetype block, const QPointF &delta)
{
    if (block < 0 || block >= qsizetype(d->m_blocks.size()))
        return;
    const BlockView &view = d->m_blocks[size_t(block)];
    const auto it = d->m_pdfZoom.find(view.span.range.start);
    if (it == d->m_pdfZoom.end() || view.x().pdfCells.empty())
        return;
    // The content follows the pointer, so the window moves the other way.
    it->pan = clampPdfPan(it->pan - delta / it->zoom, it->zoom, view.x().pdfCells.front().rect.size());
    viewport()->update();
}

bool MarkdownEditor::pressPdfCell(const QPoint &point, bool touch)
{
    qsizetype block = -1;
    QPointF local;
    d->m_pdfPress = {};
    if (pdfCellAt(point, &block, &local) < 0)
        return false;
    const BlockView &view = d->m_blocks[size_t(block)];
    // The slide controls win over a pan, so they keep working zoomed.
    if (pdfNavHit(view, local))
        return false;
    if (!d->m_pdfZoom.contains(view.span.range.start)) {
        // An ordinary click: it places the caret and reveals the source, as
        // ever. A double click's first click is one too, so remember it.
        d->m_pdfPress = {point, d->m_cursor, d->m_anchor, verticalScrollBar()->value()};
        return false;
    }
    // A zoomed page takes the press: a drag pans, and neither it nor a click
    // moves the caret or reveals the source.
    d->m_embedDrag = {block, point, point, false, true};
    if (touch && QScroller::hasScroller(viewport()))
        QScroller::scroller(viewport())->stop();
    return true;
}

bool MarkdownEditor::doubleClickPdfCell(const QPoint &point)
{
    qsizetype block = -1;
    QPointF local;
    int cell = pdfCellAt(point, &block, &local);
    const Private::PdfPress press = std::exchange(d->m_pdfPress, {});
    if (cell < 0 && press.cursor >= 0
        && (point - press.point).manhattanLength() < QApplication::startDragDistance()) {
        // The first click revealed the embed's source and the page is gone:
        // put the caret and the scroll back, which draws the page again.
        d->m_anchor = press.anchor;
        moveCursor(press.cursor, true);
        verticalScrollBar()->setValue(press.scroll);
        cell = pdfCellAt(point, &block, &local);
    }
    if (cell < 0 || pdfNavHit(d->m_blocks[size_t(block)], local))
        return false;
    const qreal zoom = d->m_pdfZoom.value(d->m_blocks[size_t(block)].span.range.start).zoom;
    return zoomPdfAt(point, zoom == 1 ? 2 : 1 / zoom); // 1 <-> 2, anchored at the pointer
}

bool MarkdownEditor::resetPdfZoom()
{
    if (d->m_pdfZoom.isEmpty())
        return false;
    d->m_pdfZoom.clear();
    for (qsizetype block = 0; block < qsizetype(d->m_blocks.size()); ++block) {
        if (d->m_blocks[size_t(block)].x().isPdfEmbed)
            invalidatePdfBlock(block);
    }
    updateLayout();
    viewport()->update();
    return true;
}

void MarkdownEditor::requestPdfZoomRenders()
{
    if (d->m_pdfZoom.isEmpty())
        return;
    const int scroll = verticalScrollBar()->value();
    // The visible zoomed pages must fit the budget together, or each
    // delivery would evict a page still on screen and ask for it again.
    qint64 visibleBytes = 0;
    for (const BlockView &view : d->m_blocks) {
        const auto zoom = view.extras && view.extras->isPdfEmbed ? d->m_pdfZoom.constFind(view.span.range.start) : d->m_pdfZoom.cend();
        if (zoom == d->m_pdfZoom.cend())
            continue;
        for (const BlockExtras::PdfCell &cell : view.x().pdfCells) {
            const QRectF rect = cell.rect.translated(0, view.y - scroll + view.textTop);
            const int base = int(cell.rect.width());
            const int wanted = pdfZoomWidth(base, zoom->zoom);
            if (rect.bottom() < 0 || rect.top() > viewport()->height() || wanted == base)
                continue;
            const qint64 bytes = qint64(wanted * qreal(wanted) * cell.pointSize.height()
                                        / qMax<qreal>(1, cell.pointSize.width()) * 4);
            if (visibleBytes > 0 && visibleBytes + bytes > d->m_pdfZoomBudget)
                return;
            visibleBytes += bytes;
            const QString key = pdfPageKey(view.x().pdfPath, view.x().pdfMtime, cell.page, wanted);
            if (d->m_pdfPageCache.contains(key))
                continue;
            ensurePdfPageAsync(view.x().pdfPath, view.x().pdfMtime, cell.page, wanted);
            d->m_pdfPageCache[key].zoomed = true;
        }
    }
}

bool MarkdownEditor::viewportEvent(QEvent *event)
{
    if (event->type() == QEvent::NativeGesture) {
        const auto *gesture = static_cast<QNativeGestureEvent *>(event);
        if (gesture->gestureType() == Qt::ZoomNativeGesture
            && zoomPdfAt(gesture->position().toPoint(), 1 + gesture->value())) {
            event->accept();
            return true;
        }
    }
    return QAbstractScrollArea::viewportEvent(event);
}

} // namespace noteAppC
