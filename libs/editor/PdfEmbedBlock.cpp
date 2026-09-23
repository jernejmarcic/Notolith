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

#include "PdfPage.h"

#include <QDesktopServices>
#include <QFile>
#include <QFileInfo>
#include <QPainter>
#include <QPainterPath>
#include <QUrl>
#include <QtConcurrent>

namespace noteAppC {

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
        QPixmap pixmap = m_images.value(key);
        if (pixmap.isNull()) {
            const QImage image = m_math.render(latex, m_theme.textFont().pixelSize() * 1.2,
                                               m_theme.color(QStringLiteral("--text-normal")),
                                               contentWidth(), ratio);
            if (image.isNull())
                return false; // falls back to showing the source, as layoutMath does
            pixmap = QPixmap::fromImage(image);
            m_images.insert(key, pixmap);
        }
        const int width = int(pixmap.width() / pixmap.devicePixelRatio());
        const int height = int(pixmap.height() / pixmap.devicePixelRatio());
        view.image = pixmap;
        view.imageSize = QSize(width, height);
        view.imageCentred = true;
        view.imagePath = path;
        view.layout.reset();
        view.cells.clear();
        view.height = height + 12;
        view.laidOut = true;
        return true;
    }

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

void MarkdownEditor::pdfMetadata(const QString &path, qint64 mtime, int &count,
                                 QList<QSizeF> &sizes) const
{
    const QString key = QStringLiteral("%1|%2").arg(path).arg(mtime);
    auto it = m_pdfMetaCache.find(key);
    if (it == m_pdfMetaCache.end()) {
        PdfMeta meta;
        meta.count = PdfPage::pageCount(path);
        meta.sizes.reserve(meta.count);
        for (int page = 0; page < meta.count; ++page)
            meta.sizes.append(PdfPage::pageSize(path, page));
        it = m_pdfMetaCache.insert(key, meta);
    }
    count = it->count;
    sizes = it->sizes;
}

bool MarkdownEditor::buildPdfBlock(BlockView &view, const QString &pdfPath, const QString &label,
                                   const PdfEmbedArgs &args, int requestedWidth)
{
    view.isPdfEmbed = true;
    view.pdfPath = pdfPath;
    view.pdfArgs = args;
    view.pdfCells.clear();
    view.pdfError.clear();
    view.pdfPrevRect = QRectF();
    view.pdfNextRect = QRectF();
    view.pdfCounterText.clear();
    view.image = QPixmap();
    view.layout.reset();
    view.cells.clear();
    view.embedLabel = label;
    view.embedPanel = true;
    view.textTop = codeHeaderHeight();

    if (!args.ok()) {
        view.pdfError = args.error;
        view.height = view.textTop + 24;
        view.laidOut = true;
        return true;
    }

    const qint64 mtime = QFileInfo(pdfPath).lastModified().toSecsSinceEpoch();
    view.pdfMtime = mtime;

    int count = 0;
    QList<QSizeF> sizes;
    pdfMetadata(pdfPath, mtime, count, sizes);
    view.pdfPageCount = count;
    if (count <= 0) {
        view.pdfError = tr("could not open as a PDF");
        view.height = view.textTop + 24;
        view.laidOut = true;
        return true;
    }

    const QList<int> pages = resolvePdfPages(args, count);
    if (pages.isEmpty()) {
        view.pdfError = tr("no pages to show");
        view.height = view.textTop + 24;
        view.laidOut = true;
        return true;
    }

    const int fullWidth = qMin(requestedWidth > 0 ? requestedWidth : contentWidth(), contentWidth());
    const auto pointSizeOf = [&sizes](int page) {
        return page >= 0 && page < sizes.size() ? sizes.at(page) : QSizeF(612, 792);
    };

    if (view.pdfArgs.layout == PdfEmbedArgs::Layout::Slides) {
        const qsizetype key = view.span.range.start;
        const int index = qBound(0, m_pdfSlideIndex.value(key, 0), int(pages.size()) - 1);
        m_pdfSlideIndex.insert(key, index);
        view.pdfSlideIndex = index;
        view.pdfSlideCount = int(pages.size());
        view.pdfCounterText = tr("%1 / %2").arg(index + 1).arg(pages.size());

        const int page = pages.at(index);
        const QSizeF pointSize = pointSizeOf(page);
        const int cellHeight = args.height > 0
                                   ? args.height
                                   : int(qreal(fullWidth) * pointSize.height()
                                         / qMax<qreal>(1, pointSize.width()));
        BlockView::PdfCell cell;
        cell.page = page;
        cell.rect = QRectF(0, 0, fullWidth, cellHeight);
        cell.pointSize = pointSize;
        cell.image = ensurePdfPageAsync(pdfPath, mtime, page, fullWidth);
        view.pdfCells.push_back(cell);

        constexpr qreal kNavHeight = 26;
        view.pdfPrevRect = QRectF(0, cellHeight + 6, 70, kNavHeight);
        view.pdfNextRect = QRectF(fullWidth - 70, cellHeight + 6, 70, kNavHeight);
        view.height = view.textTop + cellHeight + kNavHeight + 12;
    } else if (view.pdfArgs.layout == PdfEmbedArgs::Layout::Grid) {
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
                BlockView::PdfCell cell;
                cell.page = page;
                cell.rect = QRectF((i - rowStart) * (cellWidth + gap), y, cellWidth, cellHeight);
                cell.pointSize = pointSize;
                cell.image = ensurePdfPageAsync(pdfPath, mtime, page, int(cellWidth));
                view.pdfCells.push_back(cell);
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
            BlockView::PdfCell cell;
            cell.page = page;
            cell.rect = QRectF(0, y, fullWidth, cellHeight);
            cell.pointSize = pointSize;
            cell.image = ensurePdfPageAsync(pdfPath, mtime, page, fullWidth);
            view.pdfCells.push_back(cell);
            y += cellHeight + gap;
        }
        view.height = view.textTop + qMax<qreal>(0, y - gap);
    }

    view.laidOut = true;
    return true;
}

QPixmap MarkdownEditor::ensurePdfPageAsync(const QString &path, qint64 mtime, int page, int width)
{
    const QString key = QStringLiteral("%1|%2|%3|%4").arg(path).arg(mtime).arg(page).arg(width);
    auto it = m_pdfPageCache.find(key);
    if (it != m_pdfPageCache.end())
        return it->pending ? QPixmap() : it->image;

    PdfPageCacheEntry entry;
    entry.pending = true;
    entry.generation = ++m_pdfGeneration;
    it = m_pdfPageCache.insert(key, entry);
    const quint64 generation = entry.generation;

    (void)QtConcurrent::run([this, key, generation, path, page, width] {
        QImage image = PdfPage::render(path, page, width);
        QMetaObject::invokeMethod(
            this,
            [this, key, generation, path, image = std::move(image)]() mutable {
                deliverPdfPage(key, generation, path, std::move(image));
            },
            Qt::QueuedConnection);
    });
    return QPixmap();
}

void MarkdownEditor::deliverPdfPage(const QString &key, quint64 generation, const QString &path,
                                    QImage image)
{
    auto it = m_pdfPageCache.find(key);
    // A newer request for the same (path, mtime, page, width) landed first -
    // cannot happen today (nothing re-requests before delivery), kept for
    // the same reason QueryBlock's own generation guard is kept: so a future
    // caller that does re-request stays safe by construction.
    if (it == m_pdfPageCache.end() || it->generation != generation)
        return;
    it->pending = false;
    it->image = image.isNull() ? QPixmap() : QPixmap::fromImage(image);

    bool touched = false;
    for (BlockView &block : m_blocks) {
        if (!block.isPdfEmbed || block.pdfPath != path)
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
    auto it = m_texCache.find(key);
    if (it != m_texCache.end())
        return *it;

    TexCacheEntry entry;
    entry.pending = true;
    entry.generation = ++m_texGeneration;
    it = m_texCache.insert(key, entry);
    const quint64 generation = entry.generation;

    (void)QtConcurrent::run([this, key, generation, path] {
        TexCompiler::Result result = TexCompiler::compile(path);
        QMetaObject::invokeMethod(
            this,
            [this, key, generation, result = std::move(result)]() mutable {
                deliverTexCompiled(key, generation, std::move(result));
            },
            Qt::QueuedConnection);
    });
    return *it;
}

void MarkdownEditor::deliverTexCompiled(const QString &key, quint64 generation,
                                        TexCompiler::Result result)
{
    auto it = m_texCache.find(key);
    if (it == m_texCache.end() || it->generation != generation)
        return;
    it->result = std::move(result);
    it->pending = false;

    // Every block currently showing a .tex embed's placeholder or error
    // needs a chance to re-check, whichever .tex file this was - relaying
    // out all of them is simpler than tracking each block's own source path
    // for what is, in practice, at most a handful of .tex embeds in a note.
    bool touched = false;
    for (BlockView &block : m_blocks) {
        if (!block.isPdfEmbed)
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
    const QColor muted = m_theme.color(QStringLiteral("--text-muted"));
    const QColor placeholder = m_theme.color(QStringLiteral("--background-modifier-border"));

    if (!view.pdfError.isEmpty()) {
        painter.save();
        painter.setFont(m_theme.textFont());
        painter.setPen(muted);
        painter.drawText(QRectF(left, top + view.textTop, contentWidth(),
                                qMax<qreal>(24, view.height - view.textTop)),
                         Qt::AlignLeft | Qt::TextWordWrap, view.pdfError);
        painter.restore();
        return;
    }

    for (const BlockView::PdfCell &cell : view.pdfCells) {
        const QRectF rect = cell.rect.translated(left, top + view.textTop);
        if (cell.image.isNull()) {
            painter.fillRect(rect, placeholder);
            painter.save();
            painter.setPen(muted);
            painter.drawText(rect, Qt::AlignCenter, tr("Rendering…"));
            painter.restore();
            continue;
        }
        painter.drawPixmap(rect.toRect(), cell.image);
    }

    if (view.pdfArgs.layout == PdfEmbedArgs::Layout::Slides && !view.pdfCells.empty()) {
        painter.save();
        QFont small = m_theme.uiFont();
        small.setPixelSize(m_theme.value(QStringLiteral("--font-ui-smaller")).toInt());
        painter.setFont(small);
        painter.setPen(muted);
        const QRectF prev = view.pdfPrevRect.translated(left, top + view.textTop);
        const QRectF next = view.pdfNextRect.translated(left, top + view.textTop);
        if (view.pdfSlideIndex > 0)
            painter.drawText(prev, Qt::AlignVCenter | Qt::AlignLeft, tr("< Previous"));
        if (view.pdfSlideIndex + 1 < view.pdfSlideCount)
            painter.drawText(next, Qt::AlignVCenter | Qt::AlignRight, tr("Next >"));
        painter.drawText(QRectF(left, prev.top(), contentWidth(), prev.height()), Qt::AlignCenter,
                         view.pdfCounterText);
        painter.restore();
    }
}

bool MarkdownEditor::handlePdfEmbedClick(const BlockView &view, const QPoint &point, qreal left,
                                         qreal top)
{
    if (!view.isPdfEmbed || view.pdfArgs.layout != PdfEmbedArgs::Layout::Slides)
        return false;
    const QPointF local = QPointF(point) - QPointF(left, top + view.textTop);
    if (view.pdfPrevRect.adjusted(-4, -4, 4, 4).contains(local)) {
        pdfSlideStep(view.span.range.start, -1);
        return true;
    }
    if (view.pdfNextRect.adjusted(-4, -4, 4, 4).contains(local)) {
        pdfSlideStep(view.span.range.start, 1);
        return true;
    }
    return false;
}

MarkdownEditor::PdfEmbedInfo MarkdownEditor::pdfEmbedInfoAt(qsizetype offset) const
{
    PdfEmbedInfo info;
    const qsizetype index = blockAt(offset);
    if (index < 0 || index >= qsizetype(m_blocks.size()))
        return info;
    const BlockView &view = m_blocks[size_t(index)];
    if (!view.isPdfEmbed)
        return info;
    info.isPdfEmbed = true;
    info.error = view.pdfError;
    info.pageCount = view.pdfPageCount;
    info.cellCount = int(view.pdfCells.size());
    info.layout = view.pdfArgs.layout;
    info.slideIndex = view.pdfSlideIndex;
    info.slideCount = view.pdfSlideCount;
    return info;
}

void MarkdownEditor::pdfSlideStep(qsizetype offset, int direction)
{
    const qsizetype index = blockAt(offset);
    if (index < 0 || index >= qsizetype(m_blocks.size()))
        return;
    BlockView &view = m_blocks[size_t(index)];
    if (!view.isPdfEmbed || view.pdfSlideCount <= 0)
        return;
    const qsizetype key = view.span.range.start;
    const int current = m_pdfSlideIndex.value(key, 0);
    const int next = qBound(0, current + direction, view.pdfSlideCount - 1);
    if (next == current)
        return;
    m_pdfSlideIndex.insert(key, next);
    view.layout.reset();
    view.laidOut = false;
    view.heightEstimated = false;
    updateLayout();
    viewport()->update();
}

} // namespace noteAppC
