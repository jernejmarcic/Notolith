/// Everything `![[...]]` can point at that is not an image, a PDF, a
/// `.tex`, or audio/video: another note (whole, `#Heading` or `#^block`), a
/// file the window has registered a renderer for (`.canvas`, `.base` -
/// setFileEmbedRenderer), and any other file at all, which becomes a small
/// card rather than ever showing its bytes as text. Also where a block that
/// holds more than one embed, or an embed mixed with running text, gets its
/// embeds actually rendered - see layoutEmbedRow's comment on BlockView in
/// MarkdownEditor.h and the one on collectTextEmbeds below for why that is a
/// trailing row rather than truly inline.

#include "MarkdownEditor.h"

#include "Icons.h"
#include "PdfPage.h"
#include "Vault.h"

#include <QDesktopServices>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QLocale>
#include <QPainter>
#include <QPainterPath>
#include <QRegularExpression>
#include <QStringDecoder>
#include <QTextLine>
#include <QUrl>

namespace noteAppC {

// --- a plain-text panel, shared by source files, notes and a failed .tex --

bool MarkdownEditor::layoutTextPanel(BlockView &view, const QString &label, const QString &body,
                                     bool monospace)
{
    view.embedLabel = label;
    view.embedPanel = true;
    view.layout.reset();
    view.cells.clear();
    view.textTop = codeHeaderHeight();

    QString trimmed = body;
    while (trimmed.endsWith(QLatin1Char('\n')))
        trimmed.chop(1);
    QString shown = trimmed;
    shown.replace(QLatin1Char('\n'), QChar(QChar::LineSeparator));

    const BlockSpan span{monospace ? BlockSpan::Kind::Code : BlockSpan::Kind::Paragraph,
                         view.span.range, 0};
    view.map = DisplayMap::build(m_text, view.span.range, {view.span.range});
    view.layout = std::make_unique<QTextLayout>(shown);
    view.layout->setCacheEnabled(true);
    view.layout->setFont(fontFor(span));

    QTextOption option;
    option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    view.layout->setTextOption(option);

    const qreal lineHeight = lineHeightFor(span);
    view.layout->beginLayout();
    qreal height = 0;
    while (true) {
        QTextLine line = view.layout->createLine();
        if (!line.isValid())
            break;
        line.setLineWidth(contentWidth() - 28);
        line.setPosition(QPointF(0, height));
        height += lineHeight;
    }
    view.layout->endLayout();

    view.height = qMax(height, lineHeight) + view.textTop;
    view.laidOut = true;
    return true;
}

// --- note transclusion -----------------------------------------------------

QString MarkdownEditor::noteEmbedSlice(const QString &noteText, const QString &subpath,
                                       const QString &title, QString &label, bool &ok) const
{
    ok = true;
    QString fragment = subpath;
    if (fragment.startsWith(QLatin1Char('#')))
        fragment.remove(0, 1);

    if (fragment.isEmpty()) {
        label = title;
        return noteText;
    }

    const QStringList lines = noteText.split(QLatin1Char('\n'));

    if (fragment.startsWith(QLatin1Char('^'))) {
        const QString id = fragment.mid(1);
        // A block reference: the paragraph whose last line ends in "^id" -
        // scanned by hand rather than through the parser, since Decorations
        // has no notion of another file's block IDs (they are Obsidian's own
        // convention, not something md4qt or this parser's Walk tracks).
        static const QRegularExpression marker(QStringLiteral("\\^([A-Za-z0-9-]+)\\s*$"));
        for (int i = 0; i < lines.size(); ++i) {
            const auto match = marker.match(lines.at(i));
            if (!match.hasMatch() || match.captured(1) != id)
                continue;
            int first = i;
            while (first > 0 && !lines.at(first - 1).trimmed().isEmpty())
                --first;
            QStringList block = lines.mid(first, i - first + 1);
            QString last = block.constLast();
            last.remove(marker);
            block[block.size() - 1] = last.trimmed();
            label = title + QStringLiteral(" > ^") + id;
            return block.join(QLatin1Char('\n')).trimmed();
        }
        ok = false;
        return QString();
    }

    // A heading: from a line whose text matches (trailing "#"s and space
    // trimmed, case-insensitive) to the next heading at the same level or
    // shallower, or the end of the file - CommonMark's own section rule.
    static const QRegularExpression heading(QStringLiteral("^(#{1,6})\\s+(.*?)\\s*#*\\s*$"));
    int start = -1, level = 0;
    for (int i = 0; i < lines.size(); ++i) {
        const auto match = heading.match(lines.at(i));
        if (match.hasMatch() && match.captured(2).compare(fragment, Qt::CaseInsensitive) == 0) {
            start = i;
            level = match.captured(1).size();
            break;
        }
    }
    if (start < 0) {
        ok = false;
        return QString();
    }
    int end = int(lines.size());
    for (int i = start + 1; i < lines.size(); ++i) {
        const auto match = heading.match(lines.at(i));
        if (match.hasMatch() && match.captured(1).size() <= level) {
            end = i;
            break;
        }
    }
    label = title + QStringLiteral(" > ") + fragment;
    return lines.mid(start, end - start).join(QLatin1Char('\n')).trimmed();
}

bool MarkdownEditor::layoutNoteEmbed(BlockView &view, const EmbedSpan &embed, const QString &path)
{
    QFileInfo info(path);
    if (!info.isFile())
        return false;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return false;
    QString text = QString::fromUtf8(file.readAll());
    text.replace(QLatin1String("\r\n"), QLatin1String("\n"));

    QString label;
    bool ok = true;
    const QString slice = noteEmbedSlice(text, embed.subpath, info.completeBaseName(), label, ok);
    if (!ok)
        return false; // no such heading/block: falls back to the paragraph's plain link text

    return layoutTextPanel(view, label, slice, false);
}

// --- a card for a file with no richer renderer ------------------------------

bool MarkdownEditor::isBinaryFile(const QString &path) const
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return false;
    const QByteArray sample = file.read(8000);
    if (sample.isEmpty())
        return false;
    if (sample.contains('\0'))
        return true;

    // The same heuristic `file(1)` and git's own diff use: a NUL byte, or
    // enough control bytes outside \t\n\r, says binary; otherwise, bytes
    // that do not decode as valid UTF-8 say the same. Plain ASCII text
    // passes both checks trivially.
    int suspicious = 0;
    for (unsigned char byte : sample) {
        if (byte == '\t' || byte == '\n' || byte == '\r')
            continue;
        if (byte < 0x20 || byte == 0x7f)
            ++suspicious;
    }
    if (suspicious * 10 > sample.size())
        return true;

    QStringDecoder decoder(QStringDecoder::Utf8);
    const QString decoded = decoder(sample);
    Q_UNUSED(decoded);
    return decoder.hasError();
}

bool MarkdownEditor::layoutFileCard(BlockView &view, const EmbedSpan &embed, const QString &path)
{
    Q_UNUSED(embed);
    QFileInfo info(path);
    if (!info.exists())
        return false;

    view.isFileCard = true;
    view.fileCardPath = path;
    view.fileCardLabel = info.fileName();
    view.fileCardSize = QLocale().formattedDataSize(info.size());
    view.image = QPixmap();
    view.layout.reset();
    view.cells.clear();
    view.embedPanel = false;
    constexpr qreal kCardHeight = 52;
    view.fileCardRect = QRectF(0, 0, contentWidth(), kCardHeight);
    view.height = kCardHeight;
    hideWholeBlockText(view);
    view.laidOut = true;
    return true;
}

void MarkdownEditor::paintFileCard(QPainter &painter, const BlockView &view, qreal left,
                                   qreal top) const
{
    const QRectF card = view.fileCardRect.translated(left, top);
    painter.save();
    QPainterPath rounded;
    rounded.addRoundedRect(card, 6, 6);
    painter.fillPath(rounded, m_theme.color(QStringLiteral("--background-primary-alt")));

    const QColor muted = m_theme.color(QStringLiteral("--text-muted"));
    const QColor text = m_theme.color(QStringLiteral("--text-normal"));
    const qreal iconSize = card.height() - 20;
    const QIcon icon = Icons::icon(QStringLiteral("file"), muted, int(iconSize));
    const QRectF iconRect(card.left() + 12, card.top() + 10, iconSize, iconSize);
    if (!icon.isNull())
        icon.paint(&painter, iconRect.toRect());

    const qreal textLeft = iconRect.right() + 12;
    QFont name = m_theme.uiFont();
    painter.setFont(name);
    painter.setPen(text);
    painter.drawText(QRectF(textLeft, card.top() + 6, card.width() - textLeft - 12, card.height() / 2),
                     Qt::AlignVCenter | Qt::AlignLeft, view.fileCardLabel);

    QFont small = m_theme.uiFont();
    small.setPixelSize(m_theme.value(QStringLiteral("--font-ui-smaller")).toInt());
    painter.setFont(small);
    painter.setPen(muted);
    painter.drawText(QRectF(textLeft, card.top() + card.height() / 2, card.width() - textLeft - 12,
                            card.height() / 2),
                     Qt::AlignVCenter | Qt::AlignLeft, view.fileCardSize);
    painter.restore();
}

bool MarkdownEditor::handleFileCardClick(const BlockView &view, const QPoint &point, qreal left,
                                         qreal top) const
{
    if (!view.isFileCard)
        return false;
    if (!view.fileCardRect.translated(left, top).contains(point))
        return false;
    QDesktopServices::openUrl(QUrl::fromLocalFile(view.fileCardPath));
    return true;
}

// --- .canvas, .base and anything else the window hooked up -----------------

bool MarkdownEditor::layoutHookedFileEmbed(BlockView &view, const EmbedSpan &embed,
                                           const QString &path)
{
    const QString suffix = QFileInfo(path).suffix().toLower();
    auto rendererIt = m_fileEmbedRenderers.find(suffix);
    if (rendererIt == m_fileEmbedRenderers.end())
        return false;

    const int width = qMin(embed.width > 0 ? embed.width : contentWidth(), contentWidth());
    const qint64 mtime = QFileInfo(path).lastModified().toSecsSinceEpoch();
    const QString key = QStringLiteral("%1|%2|%3|%4")
                            .arg(path)
                            .arg(mtime)
                            .arg(embed.subpath)
                            .arg(width);

    auto it = m_fileEmbedCache.find(key);
    if (it == m_fileEmbedCache.end()) {
        FileEmbedCacheEntry entry;
        entry.pending = true;
        entry.generation = ++m_fileEmbedGeneration;
        it = m_fileEmbedCache.insert(key, entry);
        const quint64 generation = entry.generation;
        const FileEmbedRenderer renderer = rendererIt.value();
        // May run synchronously or on any thread - deliverFileEmbed is the
        // only place that touches m_fileEmbedCache or a BlockView, and it
        // always does so marshalled onto this object's own thread, the same
        // contract setQueryRunner documents for its own deliver callback.
        // `guard` is m_alive's weak_ptr, checked before either lambda
        // touches `this` - see ~MarkdownEditor's comment for why a
        // QueuedConnection's own "dropped if destroyed" guarantee is not
        // enough when the call to invokeMethod itself can race a delete.
        const std::weak_ptr<char> guard = m_alive;
        renderer(path, embed.subpath, width, [this, guard, key, generation](QImage image) {
            if (guard.expired())
                return;
            QMetaObject::invokeMethod(
                this,
                [this, guard, key, generation, image = std::move(image)]() mutable {
                    if (guard.expired())
                        return;
                    deliverFileEmbed(key, generation, std::move(image));
                },
                Qt::QueuedConnection);
        });
    }

    view.embedLabel = QFileInfo(path).fileName();
    view.embedPanel = true;
    view.layout.reset();
    view.cells.clear();
    view.textTop = codeHeaderHeight();
    view.imagePath = path; // deliverFileEmbed's own way to find this block again
    hideWholeBlockText(view);

    if (it->pending || it->image.isNull()) {
        view.image = QPixmap();
        view.height = view.textTop + 140; // a placeholder box until the renderer answers
        view.laidOut = true;
        return true;
    }

    const QPixmap pixmap = QPixmap::fromImage(it->image);
    const int height = int(qreal(width) * pixmap.height() / qMax(1, pixmap.width()));
    view.image = pixmap;
    view.imageSize = QSize(width, height);
    view.height = height + view.textTop;
    view.laidOut = true;
    return true;
}

void MarkdownEditor::deliverFileEmbed(const QString &key, quint64 generation, QImage image)
{
    auto it = m_fileEmbedCache.find(key);
    if (it == m_fileEmbedCache.end() || it->generation != generation)
        return;
    it->pending = false;
    it->image = image;

    const QString path = key.section(QLatin1Char('|'), 0, 0);
    bool touched = false;
    for (BlockView &block : m_blocks) {
        if (block.imagePath != path || block.isPdfEmbed || block.isFileCard
            || !block.mediaPath.isEmpty())
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

// --- several embeds sharing a block, or one mixed with running text --------

void MarkdownEditor::collectTextEmbeds(const BlockView &view, QList<Range> &hidden,
                                       QList<const EmbedSpan *> &found) const
{
    // An embed alone in its block already had its own chance through
    // layoutEmbed; if that failed - a disabled feature, a path that does not
    // resolve, a missing file - the right fallback is the paragraph's own
    // plain link text, the way Obsidian shows an embed it cannot resolve
    // either, not an upgrade to a trailing file card.
    const EmbedSpan *sole = soleEmbedOf(view);
    for (const EmbedSpan &embed : m_decorations.embeds) {
        if (&embed == sole)
            continue;
        if (!view.span.range.contains(embed.range.start)
            || !view.span.range.contains(embed.range.end))
            continue;
        found.append(&embed);
        hidden.append(embed.range);
    }
}

MarkdownEditor::BlockView::EmbedCell MarkdownEditor::buildEmbedCell(const EmbedSpan &embed,
                                                                     qreal maxWidth)
{
    BlockView::EmbedCell cell;
    cell.source = embed.range;

    const QString path = m_resolver ? m_resolver(embed.target) : QString();
    if (path.isEmpty()) {
        cell.isFileCard = true;
        cell.label = embed.target;
        cell.rect = QRectF(0, 0, maxWidth, 44);
        return cell;
    }

    const QString suffix = QFileInfo(path).suffix().toLower();
    static const QByteArrayList images = QImageReader::supportedImageFormats();
    // See the same guard in layoutEmbed: some installs can technically
    // decode a PDF as a flat image, but a PDF page always goes through the
    // richer, page-aware path below instead.
    if (suffix != QLatin1String("pdf") && images.contains(suffix.toUtf8())) {
        QPixmap pixmap = m_images.value(path);
        if (pixmap.isNull()) {
            pixmap = QPixmap(path);
            if (!pixmap.isNull())
                m_images.insert(path, pixmap);
        }
        if (!pixmap.isNull()) {
            // |width still applies to a cell in a row - a reader who asked
            // for a small thumbnail should get one here too, not the row's
            // whole share just because it is sharing a line with another
            // embed.
            const qreal width = qMin<qreal>(maxWidth, embed.width > 0 ? embed.width : pixmap.width());
            const qreal height = width * pixmap.height() / qMax(1, pixmap.width());
            cell.image = pixmap;
            cell.rect = QRectF(0, 0, width, height);
            cell.openPath = path;
            return cell;
        }
    }

    // A PDF's first requested page, through the same async cache a sole PDF
    // embed uses - a thumbnail here is never rendered twice just because the
    // same file also appears as a sole embed elsewhere in the note. The full
    // page-argument grammar (ranges, slides, a grid) is still there in the
    // fragment for a reader to move the embed onto its own line and get.
    if (m_features.PDFembeds && suffix == QLatin1String("pdf")) {
        const qint64 mtime = QFileInfo(path).lastModified().toSecsSinceEpoch();
        const PdfEmbedArgs args = parsePdfEmbedArgs(embed.subpath);
        int count = 0;
        QList<QSizeF> sizes;
        pdfMetadata(path, mtime, count, sizes);
        if (count > 0) {
            const QList<int> pages = resolvePdfPages(args, count);
            const int page = pages.isEmpty() ? 0 : pages.first();
            const QSizeF pointSize = page < sizes.size() ? sizes.at(page) : QSizeF(612, 792);
            const qreal width = maxWidth;
            const qreal height = width * pointSize.height() / qMax<qreal>(1, pointSize.width());
            cell.image = ensurePdfPageAsync(path, mtime, page, int(width));
            cell.rect = QRectF(0, 0, width, height);
            cell.label = QFileInfo(path).fileName();
            cell.panel = true;
            cell.openPath = path;
            return cell;
        }
    }

    if (m_features.fileEmbeds && Vault::isNote(path)) {
        cell.isFileCard = true;
        cell.isNote = true;
        cell.label = QFileInfo(path).completeBaseName();
        cell.openPath = embed.target;
        cell.rect = QRectF(0, 0, maxWidth, 44);
        return cell;
    }

    QFileInfo info(path);
    cell.isFileCard = true;
    cell.label = info.fileName();
    cell.sizeText = info.exists() ? QLocale().formattedDataSize(info.size()) : QString();
    cell.openPath = path;
    cell.rect = QRectF(0, 0, maxWidth, 44);
    return cell;
}

void MarkdownEditor::appendEmbedRows(BlockView &view, const QList<const EmbedSpan *> &embeds)
{
    view.embedCells.clear();
    if (embeds.isEmpty())
        return;

    qreal y = view.height;
    constexpr qreal rowGap = 8;
    constexpr qreal cellGap = 8;
    qsizetype i = 0;
    while (i < embeds.size()) {
        qsizetype j = i + 1;
        // Embeds with nothing but the source's own inline whitespace between
        // them - no newline - share a row, the way Obsidian shows several
        // embeds written on one line side by side.
        while (j < embeds.size()
              && !m_text.mid(embeds.at(j - 1)->range.end,
                             embeds.at(j)->range.start - embeds.at(j - 1)->range.end)
                      .contains(QLatin1Char('\n')))
            ++j;

        const qsizetype count = j - i;
        const qreal cellWidth = (contentWidth() - cellGap * (count - 1)) / qreal(count);
        qreal rowHeight = 0;
        qreal x = 0;
        for (qsizetype k = i; k < j; ++k) {
            BlockView::EmbedCell cell = buildEmbedCell(*embeds.at(k), cellWidth);
            cell.rect.moveTo(x, y);
            rowHeight = qMax(rowHeight, cell.rect.height());
            x += cellWidth + cellGap;
            view.embedCells.push_back(std::move(cell));
        }
        y += rowHeight + rowGap;
        i = j;
    }
    view.height = y;
}

void MarkdownEditor::paintEmbedCells(QPainter &painter, const BlockView &view, qreal left,
                                     qreal top) const
{
    if (view.embedCells.empty())
        return;

    const QColor muted = m_theme.color(QStringLiteral("--text-muted"));
    const QColor text = m_theme.color(QStringLiteral("--text-normal"));
    for (const BlockView::EmbedCell &cell : view.embedCells) {
        const QRectF rect = cell.rect.translated(left, top);
        if (cell.isFileCard) {
            painter.save();
            QPainterPath rounded;
            rounded.addRoundedRect(rect, 6, 6);
            painter.fillPath(rounded, m_theme.color(QStringLiteral("--background-primary-alt")));
            const qreal iconSize = rect.height() - 16;
            const QIcon icon = Icons::icon(cell.isNote ? QStringLiteral("file-text")
                                                        : QStringLiteral("file"),
                                           muted, int(iconSize));
            const QRectF iconRect(rect.left() + 8, rect.top() + 8, iconSize, iconSize);
            if (!icon.isNull())
                icon.paint(&painter, iconRect.toRect());
            QFont font = m_theme.uiFont();
            painter.setFont(font);
            painter.setPen(text);
            const qreal textLeft = iconRect.right() + 8;
            const QFontMetrics metrics(font);
            painter.drawText(QRectF(textLeft, rect.top(), rect.width() - textLeft - 8, rect.height()),
                             Qt::AlignVCenter | Qt::AlignLeft,
                             metrics.elidedText(cell.label, Qt::ElideMiddle,
                                                int(rect.width() - textLeft - 8)));
            painter.restore();
            continue;
        }
        if (cell.image.isNull()) {
            painter.fillRect(rect, muted);
            continue;
        }
        painter.drawPixmap(rect.toRect(), cell.image);
        if (cell.panel && !cell.label.isEmpty()) {
            painter.save();
            QFont small = m_theme.uiFont();
            small.setPixelSize(m_theme.value(QStringLiteral("--font-ui-smaller")).toInt());
            painter.setFont(small);
            painter.setPen(muted);
            painter.drawText(QRectF(rect.left(), rect.bottom() + 2, rect.width(), 16),
                             Qt::AlignLeft, cell.label);
            painter.restore();
        }
    }
}

bool MarkdownEditor::isFileCardAt(qsizetype offset) const
{
    const qsizetype index = blockAt(offset);
    if (index < 0 || index >= qsizetype(m_blocks.size()))
        return false;
    return m_blocks[size_t(index)].isFileCard;
}

int MarkdownEditor::embedCellCountAt(qsizetype offset) const
{
    const qsizetype index = blockAt(offset);
    if (index < 0 || index >= qsizetype(m_blocks.size()))
        return 0;
    return int(m_blocks[size_t(index)].embedCells.size());
}

QString MarkdownEditor::panelTextAt(qsizetype offset) const
{
    const qsizetype index = blockAt(offset);
    if (index < 0 || index >= qsizetype(m_blocks.size()))
        return QString();
    const BlockView &view = m_blocks[size_t(index)];
    return view.layout ? view.layout->text() : QString();
}

bool MarkdownEditor::handleEmbedCellClick(const BlockView &view, const QPoint &point, qreal left,
                                          qreal top)
{
    for (const BlockView::EmbedCell &cell : view.embedCells) {
        if (!cell.rect.translated(left, top).contains(point))
            continue;
        if (cell.isNote)
            emit linkActivated(cell.openPath);
        else if (!cell.openPath.isEmpty())
            QDesktopServices::openUrl(QUrl::fromLocalFile(cell.openPath));
        return true;
    }
    return false;
}

} // namespace noteAppC
