/// Everything `![[...]]` can point at that is not an image, a PDF, a
/// `.tex`, or audio/video: another note (whole, `#Heading` or `#^block`), a
/// file the window has registered a renderer for (`.canvas`, `.base` -
/// setFileEmbedRenderer), and any other file at all, which becomes a small
/// card rather than ever showing its bytes as text. Also where a block that
/// holds more than one embed, or an embed mixed with running text, gets its
/// embeds drawn inline - see the comment on collectTextEmbeds in
/// MarkdownEditor.h.

#include "MarkdownEditor.h"
#include "MarkdownEditor_p.h"

#include "Icons.h"
#ifdef NOTEAPPC_HAVE_EPUBCOVER
#include "EpubCover.h"
#endif
#include "PdfPage.h"
#include "Parser.h"
#include "Vault.h"

#include <algorithm>

#include <QDesktopServices>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QLocale>
#include <QPainter>
#include <QPainterPath>
#include <QRegularExpression>
#include <QScrollBar>
#include <QStringDecoder>
#include <QtConcurrent>
#include <QTextLine>
#include <QTimer>
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
    view.textTop = label.isEmpty() ? 0 : codeHeaderHeight();

    QString trimmed = body;
    while (trimmed.endsWith(QLatin1Char('\n')))
        trimmed.chop(1);
    QString shown = trimmed;
    shown.replace(QLatin1Char('\n'), QChar(QChar::LineSeparator));

    const BlockSpan span{monospace ? BlockSpan::Kind::Code : BlockSpan::Kind::Paragraph,
                         view.span.range, 0};
    view.map = DisplayMap::build(d->m_text, view.span.range, {view.span.range});
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
            label.clear(); // Obsidian titles an embed only when it has no subpath
            return block.join(QLatin1Char('\n')).trimmed();
        }
        ok = false;
        return QString();
    }

    // A heading, Markdown or HTML: from its line to the next heading at the
    // same level or shallower, or the end of the file - CommonMark's own
    // section rule (headingSection). `A#B#C` names the last level, as a
    // canvas card does, when the whole text matches no heading.
    Range section = headingSection(noteText, fragment);
    if (!section.isValid() && fragment.contains(QLatin1Char('#')))
        section = headingSection(noteText, fragment.section(QLatin1Char('#'), -1));
    if (!section.isValid()) {
        ok = false;
        return QString();
    }
    label = title + QStringLiteral(" > ") + fragment;
    return noteText.mid(section.start, section.length()).trimmed();
}

bool MarkdownEditor::noteEmbedContent(const QString &path, const QString &subpath, QString &label,
                                      QString &slice, QString &key, QImage &image)
{
    QFileInfo info(path);
    if (!info.isFile())
        return false;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return false;
    QString text = QString::fromUtf8(file.readAll());
    text.replace(QLatin1String("\r\n"), QLatin1String("\n"));

    bool ok = true;
    slice = noteEmbedSlice(text, subpath, info.completeBaseName(), label, ok);
    if (!ok)
        return false; // no such heading/block: falls back to the paragraph's plain link text

    // Formatted, as Obsidian draws it: the slice through an editor of its
    // own in reading view, drawn into an image a turn after this layout.
    // Until then, and when it cannot be drawn, the plain text.
    const int width = contentWidth() - 28;
    // Inside another embed's drawing a note that is already being drawn
    // around it (it embeds itself, or A embeds B embeds A) or one nested
    // kMaxEmbedDepth deep stays the plain text panel, as Obsidian stops at
    // five levels; nothing is drawn for it, so the recursion ends here.
    const QString canonical = QFileInfo(path).canonicalFilePath();
    if (d->m_embedAncestors.contains(canonical) || d->m_embedAncestors.size() >= kMaxEmbedDepth)
        return true;

    key = QStringLiteral("%1|%2|%3|%4|%5")
              .arg(path)
              .arg(info.lastModified().toMSecsSinceEpoch())
              .arg(subpath)
              .arg(width)
              .arg(embedThemeTag());
    auto it = d->m_fileEmbedCache.find(key);
    if (it == d->m_fileEmbedCache.end()) {
        FileEmbedCacheEntry entry;
        entry.generation = ++d->m_fileEmbedGeneration;
        if (!d->m_embedAncestors.isEmpty()) {
            // A nested editor is gone as soon as it has been drawn, so a
            // turn of the event loop would never come: draw this level now.
            entry.image = renderNoteEmbed(slice, width, canonical);
            entry.pending = false;
            it = d->m_fileEmbedCache.insert(key, entry);
        } else {
            it = d->m_fileEmbedCache.insert(key, entry);
            QTimer::singleShot(0, this, [this, key, generation = entry.generation, slice, width, canonical] {
                deliverFileEmbed(key, generation, renderNoteEmbed(slice, width, canonical));
            });
        }
    }
    if (!it->pending)
        image = it->image;
    return true;
}

bool MarkdownEditor::layoutNoteEmbed(BlockView &view, const EmbedSpan &embed, const QString &path)
{
    QString label;
    QString slice;
    QString key;
    QImage image;
    if (!noteEmbedContent(path, embed.subpath, label, slice, key, image))
        return false;
    if (!key.isEmpty())
        view.imagePath = path; // deliverFileEmbed's way back to this block
    if (image.isNull())
        return layoutTextPanel(view, label, slice, false);

    view.embedLabel = label;
    view.embedPanel = true;
    view.layout.reset();
    view.cells.clear();
    view.textTop = label.isEmpty() ? 0 : codeHeaderHeight();
    hideWholeBlockText(view);
    const QPixmap pixmap = QPixmap::fromImage(image);
    const qreal ratio = pixmap.devicePixelRatio();
    view.image = pixmap;
    view.imageSize = QSize(qRound(pixmap.width() / ratio), qRound(pixmap.height() / ratio));
    view.height = view.imageSize.height() + view.textTop;
    view.laidOut = true;
    return true;
}

QImage MarkdownEditor::renderNoteEmbed(const QString &slice, int width, const QString &fromNote) const
{
    // ponytail: a long note is cut at kMaxNoteEmbedHeight where Obsidian
    // scrolls inside the embed. Notes embedded in the slice are drawn the
    // same way, now, one level down (layoutNoteEmbed); `fromNote` joined to
    // the ancestors is the cycle guard.
    constexpr int kMaxNoteEmbedHeight = 1200;
    MarkdownEditor nested;
    nested.d->m_embedAncestors = d->m_embedAncestors;
    if (!fromNote.isEmpty())
        nested.d->m_embedAncestors.append(fromNote);
    nested.setTheme(d->m_theme);
    nested.setMode(Mode::Reading);
    nested.setReadableLineLength(false);
    nested.setFeatures(d->m_features);
    nested.setLinkResolver(m_resolver);
    nested.setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    nested.setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    nested.resize(width, 200);
    nested.setNoteText(slice);
    if (nested.wordCount() < 0)
        return {}; // a slice big enough to parse off the thread stays text
    nested.resize(width, qBound(1, qCeil(nested.documentHeight()), kMaxNoteEmbedHeight));
    return nested.grab().toImage();
}

QString MarkdownEditor::embedThemeTag() const
{
    return d->m_theme.color(QStringLiteral("--background-primary")).name()
         + d->m_theme.color(QStringLiteral("--text-normal")).name() + QString::number(d->m_theme.textFont().pointSizeF());
}

// --- a card for a file with no richer renderer ------------------------------

bool MarkdownEditor::isBinaryFile(const QString &path) const
{
    // An encrypted note is ciphertext: a card with its name, never its bytes.
    if (Vault::isEncryptedNote(path))
        return true;
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
    view.fileCardCover = QPixmap();
    if (path.endsWith(QLatin1String(".epub"), Qt::CaseInsensitive))
        view.fileCardCover = ensureEpubCoverAsync(path, info.lastModified().toMSecsSinceEpoch(), info.size());
    // A book with a cover gets a taller card: a cover is a portrait, an icon a square.
    const qreal kCardHeight = view.fileCardCover.isNull() ? 52 : 96;
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
    painter.fillPath(rounded, d->m_theme.color(QStringLiteral("--background-primary-alt")));

    const QColor muted = d->m_theme.color(QStringLiteral("--text-muted"));
    const QColor text = d->m_theme.color(QStringLiteral("--text-normal"));
    const qreal iconSize = card.height() - 20;
    QRectF iconRect(card.left() + 12, card.top() + 10, iconSize, iconSize);
    if (!view.fileCardCover.isNull()) {
        // The cover, fitted into the icon's square and centred in it.
        const QSizeF fitted = view.fileCardCover.deviceIndependentSize().scaled(iconRect.size(), Qt::KeepAspectRatio);
        const QRectF target(iconRect.center().x() - fitted.width() / 2, iconRect.top(), fitted.width(),
                            fitted.height());
        painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
        painter.drawPixmap(target, view.fileCardCover, QRectF(view.fileCardCover.rect()));
        iconRect.setWidth(fitted.width());
        iconRect.moveLeft(target.left());
    } else {
        const QIcon icon = Icons::icon(QStringLiteral("file"), muted, int(iconSize));
        if (!icon.isNull())
            icon.paint(&painter, iconRect.toRect());
    }

    const qreal textLeft = iconRect.right() + 12;
    QFont name = d->m_theme.uiFont();
    painter.setFont(name);
    painter.setPen(text);
    painter.drawText(QRectF(textLeft, card.top() + 6, card.width() - textLeft - 12, card.height() / 2),
                     Qt::AlignVCenter | Qt::AlignLeft, view.fileCardLabel);

    QFont small = d->m_theme.uiFont();
    small.setPixelSize(d->m_theme.value(QStringLiteral("--font-ui-smaller")).toInt());
    painter.setFont(small);
    painter.setPen(muted);
    painter.drawText(QRectF(textLeft, card.top() + card.height() / 2, card.width() - textLeft - 12,
                            card.height() / 2),
                     Qt::AlignVCenter | Qt::AlignLeft, view.fileCardSize);
    painter.restore();
}

QPixmap MarkdownEditor::ensureEpubCoverAsync(const QString &path, qint64 mtime, qint64 size)
{
#ifdef NOTEAPPC_HAVE_EPUBCOVER
    const QString key = QStringLiteral("%1|%2|%3").arg(path).arg(mtime).arg(size);
    const auto it = d->m_epubCoverCache.constFind(key);
    if (it != d->m_epubCoverCache.constEnd())
        return it->pending ? QPixmap() : it->cover;
    if (d->m_epubCoverCache.size() >= 64)
        d->m_epubCoverCache.clear(); // ponytail: a flush, not an LRU; a card asks again
    d->m_epubCoverCache.insert(key, EpubCoverEntry{true, QPixmap()});
    // Through m_lifeline, as the PDF pages: a read that outlives the editor is dropped.
    (void)QtConcurrent::run([this, lifeline = d->m_lifeline, key, path] {
        QImage cover = epubCover(path);
        lifeline->post(this, [this, key, path, cover = std::move(cover)]() mutable {
            deliverEpubCover(key, path, std::move(cover));
        });
    });
#else
    Q_UNUSED(path);
    Q_UNUSED(mtime);
    Q_UNUSED(size);
#endif
    return QPixmap();
}

void MarkdownEditor::deliverEpubCover(const QString &key, const QString &path, QImage cover)
{
    // A book with no cover is cached as a null pixmap, so it is asked once.
    d->m_epubCoverCache.insert(key, EpubCoverEntry{false, cover.isNull() ? QPixmap() : QPixmap::fromImage(cover)});
    if (cover.isNull())
        return; // the card stays as laid out
    bool touched = false;
    for (BlockView &block : d->m_blocks) {
        if (!block.isFileCard || block.fileCardPath != path)
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

QString MarkdownEditor::fileEmbedCacheKey(const QString &path, const QString &subpath, int width) const
{
    const qint64 mtime = QFileInfo(path).lastModified().toSecsSinceEpoch();
    return QStringLiteral("%1|%2|%3|%4|%5")
        .arg(path)
        .arg(mtime)
        .arg(subpath)
        .arg(width)
        .arg(embedThemeTag()); // the colours are drawn into the image
}

bool MarkdownEditor::layoutHookedFileEmbed(BlockView &view, const EmbedSpan &embed,
                                           const QString &path)
{
    const QString suffix = QFileInfo(path).suffix().toLower();
    auto rendererIt = d->m_fileEmbedRenderers.find(suffix);
    if (rendererIt == d->m_fileEmbedRenderers.end())
        return false;

    const int width = qMin(embed.width > 0 ? embed.width : contentWidth(), contentWidth());
    const QString key = fileEmbedCacheKey(path, embed.subpath, width);

    auto it = d->m_fileEmbedCache.find(key);
    if (it == d->m_fileEmbedCache.end()) {
        FileEmbedCacheEntry entry;
        entry.pending = true;
        entry.generation = ++d->m_fileEmbedGeneration;
        it = d->m_fileEmbedCache.insert(key, entry);
        const quint64 generation = entry.generation;
        const FileEmbedRenderer renderer = rendererIt.value();
        // May run synchronously or on any thread - deliverFileEmbed is the
        // only place that touches m_fileEmbedCache or a BlockView, and it
        // always does so marshalled onto this object's own thread, the same
        // contract setQueryRunner documents for its own deliver callback.
        renderer(path, embed.subpath, width, [this, lifeline = d->m_lifeline, key, generation](QImage image) {
            lifeline->post(this, [this, key, generation, image = std::move(image)]() mutable {
                deliverFileEmbed(key, generation, std::move(image));
            });
        });
    }

    view.embedLabel = QFileInfo(path).fileName();
    view.embedPanel = true;
    view.layout.reset();
    view.cells.clear();
    view.textTop = codeHeaderHeight();
    view.imagePath = path; // deliverFileEmbed's own way to find this block again
    view.embedSubpath = embed.subpath;
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
    auto it = d->m_fileEmbedCache.find(key);
    if (it == d->m_fileEmbedCache.end() || it->generation != generation)
        return;
    it->pending = false;
    it->image = image;

    const QString path = key.section(QLatin1Char('|'), 0, 0);
    bool touched = false;
    for (BlockView &block : d->m_blocks) {
        const bool inlineCell = std::any_of(block.embedCells.begin(), block.embedCells.end(),
                                            [&](const EmbedCell &cell) { return cell.cacheKey == key; });
        if (!inlineCell && (block.imagePath != path || block.isPdfEmbed || block.isFileCard
                            || !block.mediaPath.isEmpty()))
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

// --- live interaction over a hooked embed's own image -----------------------

QRectF MarkdownEditor::hookedEmbedImageRect(const BlockView &view) const
{
    const qreal top = view.y - verticalScrollBar()->value();
    return QRectF(contentLeft(), top + view.textTop, view.imageSize.width(), view.imageSize.height());
}

QRect MarkdownEditor::hookedEmbedImageRectAt(qsizetype offset) const
{
    const qsizetype index = blockAt(offset);
    if (index < 0 || index >= qsizetype(d->m_blocks.size()))
        return {};
    const BlockView &view = d->m_blocks[size_t(index)];
    if (!view.embedPanel || view.image.isNull() || view.imagePath.isEmpty())
        return {};
    return hookedEmbedImageRect(view).toRect();
}

qsizetype MarkdownEditor::hookedEmbedBlockAt(const QPoint &viewportPoint) const
{
    for (qsizetype i = 0; i < qsizetype(d->m_blocks.size()); ++i) {
        const BlockView &view = d->m_blocks[size_t(i)];
        if (!view.embedPanel || view.image.isNull() || view.imagePath.isEmpty()
            || view.isPdfEmbed || view.isFileCard || !view.mediaPath.isEmpty())
            continue;
        if (!d->m_fileEmbedInputHandlers.contains(QFileInfo(view.imagePath).suffix().toLower()))
            continue;
        if (hookedEmbedImageRect(view).contains(viewportPoint))
            return i;
    }
    return -1;
}

QPoint MarkdownEditor::embedLocalPoint(qsizetype blockIndex, const QPoint &viewportPoint) const
{
    return viewportPoint - hookedEmbedImageRect(d->m_blocks[size_t(blockIndex)]).topLeft().toPoint();
}

bool MarkdownEditor::applyFileEmbedInput(qsizetype blockIndex, const FileEmbedInputEvent &event)
{
    if (blockIndex < 0 || size_t(blockIndex) >= d->m_blocks.size())
        return false; // the gesture outlived a relayout - vanishingly rare, and harmless to drop
    BlockView &view = d->m_blocks[size_t(blockIndex)];
    auto it = d->m_fileEmbedInputHandlers.find(QFileInfo(view.imagePath).suffix().toLower());
    if (it == d->m_fileEmbedInputHandlers.end())
        return false;

    const FileEmbedInputResult result = it.value()(view.imagePath, view.embedSubpath, event);
    if (!result.image.isNull()) {
        view.image = QPixmap::fromImage(result.image);
        // Kept in the same cache entry layoutHookedFileEmbed reads from, so
        // an unrelated edit elsewhere in the note - which rebuilds every
        // BlockView from scratch - relays this one out from the view state
        // just interacted with, not the stale image it was cold-rendered
        // with.
        const QString key = fileEmbedCacheKey(view.imagePath, view.embedSubpath, view.imageSize.width());
        if (auto cacheIt = d->m_fileEmbedCache.find(key); cacheIt != d->m_fileEmbedCache.end())
            cacheIt->image = result.image;
        viewport()->update();
    }
    if (!result.openPath.isEmpty())
        emit fileEmbedActivated(result.openPath);
    return !result.image.isNull() || !result.openPath.isEmpty();
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
    for (const EmbedSpan &embed : d->m_decorations.embeds) {
        if (&embed == sole)
            continue;
        if (!view.span.range.contains(embed.range.start)
            || !view.span.range.contains(embed.range.end))
            continue;
        // The caret inside one shows its source, to edit it.
        if (view.active && d->m_cursor > embed.range.start && d->m_cursor < embed.range.end)
            continue;
        found.append(&embed);
        // The "!" stays, as the carrier prepareTextEmbeds reserves the
        // cell's room on - so the embed's own syntax markers ("![[", "]]"),
        // already hidden, go first, or the carrier would be the next
        // visible character after it.
        hidden.removeIf([&](const Range &marker) {
            return marker.start >= embed.range.start && marker.end <= embed.range.end;
        });
        hidden.append({embed.range.start + 1, embed.range.end});
    }
}

MarkdownEditor::EmbedCell MarkdownEditor::buildEmbedCell(const EmbedSpan &embed,
                                                                     qreal maxWidth)
{
    EmbedCell cell;
    cell.source = embed.range;

    const QString path = m_resolver ? m_resolver(embed.target) : QString();
    // An `<img>` whose file is not there says what its author wrote in `alt`,
    // on a card no wider than a caption needs.
    const auto missingCard = [&](const QString &name) {
        cell.isFileCard = true;
        cell.label = embed.html && !embed.alias.isEmpty() ? embed.alias : name;
        cell.rect = QRectF(0, 0, embed.html ? qMin<qreal>(maxWidth, 220) : maxWidth, 44);
        return cell;
    };
    if (path.isEmpty())
        return missingCard(embed.target);

    const QString suffix = QFileInfo(path).suffix().toLower();
    static const QByteArrayList images = QImageReader::supportedImageFormats();
    // An <img> is an image or it is nothing: never a PDF page or a note card.
    if (embed.html && (suffix == QLatin1String("pdf") || !images.contains(suffix.toUtf8())))
        return missingCard(QFileInfo(path).fileName());
    // See the same guard in layoutEmbed: some installs can technically
    // decode a PDF as a flat image, but a PDF page always goes through the
    // richer, page-aware path below instead.
    if (suffix != QLatin1String("pdf") && images.contains(suffix.toUtf8())) {
        QPixmap pixmap = d->m_images.value(path);
        if (pixmap.isNull()) {
            pixmap = QPixmap(path);
            if (!pixmap.isNull())
                d->m_images.insert(path, pixmap);
        }
        if (!pixmap.isNull()) {
            // |width still applies to a cell in a row - a reader who asked
            // for a small thumbnail should get one here too, not the row's
            // whole share just because it is sharing a line with another
            // embed.
            // |width and `width=` apply to a cell in a row too, capped by its share.
            const QSizeF size = embedImageSize(embed, pixmap.size(), maxWidth);
            const qreal width = size.width();
            const qreal height = size.height();
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
    if (d->m_features.PDFembeds && suffix == QLatin1String("pdf")) {
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

    if (d->m_features.fileEmbeds && Vault::isNote(path)) {
        cell.isNote = true;
        cell.openPath = embed.target;
        // The content a sole note embed shows, as a block in the paragraph
        // as in Obsidian; the card stands in for it until the drawing is
        // ready (deliverFileEmbed lays this block out again) and when the
        // heading or block is not there.
        QString label;
        QString slice;
        QImage image;
        noteEmbedContent(path, embed.subpath, label, slice, cell.cacheKey, image);
        if (!image.isNull()) {
            cell.image = QPixmap::fromImage(image);
            const qreal ratio = cell.image.devicePixelRatio();
            cell.rect = QRectF(0, 0, cell.image.width() / ratio, cell.image.height() / ratio);
            return cell;
        }
        cell.isFileCard = true;
        cell.label = QFileInfo(path).completeBaseName();
        cell.rect = QRectF(0, 0, maxWidth, 44);
        return cell;
    }

    QFileInfo info(path);
    if (embed.html)
        return missingCard(info.fileName()); // an <img> of a file Qt cannot read
    cell.isFileCard = true;
    cell.label = info.fileName();
    cell.sizeText = info.exists() ? QLocale().formattedDataSize(info.size()) : QString();
    cell.openPath = path;
    cell.rect = QRectF(0, 0, maxWidth, 44);
    return cell;
}

// A template only because EmbedCell is private to the editor.
template <typename Cell>
static qreal cellHeight(const Cell &cell)
{
    return cell.rect.height() + (cell.panel && !cell.label.isEmpty() ? 18 : 0);
}

void MarkdownEditor::prepareTextEmbeds(BlockView &view, const QList<const EmbedSpan *> &embeds,
                                       QList<QTextLayout::FormatRange> &formats)
{
    view.embedCells.clear();
    if (embeds.isEmpty())
        return;

    // ponytail: one width budget for the whole block; a full-width cell on
    // an indented line (a list item, a nested callout) runs past its right
    // edge. Per-line budgets if anyone embeds full-width files in lists.
    const qreal maxWidth = contentWidth() - (view.callout ? 28 : 0);
    const QFontMetricsF metrics(view.layout->font());
    for (const EmbedSpan *embed : embeds) {
        EmbedCell cell = buildEmbedCell(*embed, maxWidth);
        // The carrier is the embed's first character: "!" for `![..]`, "<"
        // for an `<img>`.
        const qreal carrier = metrics.horizontalAdvance(d->m_text.at(embed->range.start));
        QTextCharFormat format;
        format.setForeground(Qt::transparent);
        format.setFontLetterSpacingType(QFont::AbsoluteSpacing);
        format.setFontLetterSpacing(cell.rect.width() - carrier);
        formats.append({int(view.map.toDisplay(embed->range.start)), 1, format});
        view.embedCells.push_back(std::move(cell));
    }
}

qreal MarkdownEditor::textEmbedHeightOn(const BlockView &view, const QTextLine &line) const
{
    qreal tallest = 0;
    for (const EmbedCell &cell : view.embedCells) {
        const qsizetype carrier = view.map.toDisplay(cell.source.start);
        if (carrier >= line.textStart() && carrier < line.textStart() + line.textLength())
            tallest = qMax(tallest, cellHeight(cell));
    }
    return tallest;
}

void MarkdownEditor::placeTextEmbeds(BlockView &view, qreal lineHeight)
{
    // The same origin the paint step draws the text at.
    const qreal indent = view.callout ? 14 : (view.embedPanel ? 2 : 0);
    for (EmbedCell &cell : view.embedCells) {
        const int carrier = int(view.map.toDisplay(cell.source.start));
        const QTextLine line = view.layout->lineForTextPosition(carrier);
        if (!line.isValid())
            continue;
        // A tall cell sits on the line's text bottom (the line grew above
        // it to make room); a short one is centred on the text.
        const qreal height = cellHeight(cell);
        const qreal y = height > lineHeight ? line.y() + lineHeight - height
                                            : line.y() + (lineHeight - height) / 2;
        cell.rect.moveTo(indent + line.cursorToX(carrier), view.textTop + y);
    }
}

void MarkdownEditor::paintEmbedCells(QPainter &painter, const BlockView &view, qreal left,
                                     qreal top) const
{
    paintEmbedCellList(painter, view.embedCells, left, top);
}

void MarkdownEditor::paintEmbedCellList(QPainter &painter, const std::vector<EmbedCell> &cells,
                                        qreal left, qreal top) const
{
    if (cells.empty())
        return;

    const QColor muted = d->m_theme.color(QStringLiteral("--text-muted"));
    const QColor text = d->m_theme.color(QStringLiteral("--text-normal"));
    for (const EmbedCell &cell : cells) {
        const QRectF rect = cell.rect.translated(left, top);
        if (cell.isFileCard) {
            painter.save();
            QPainterPath rounded;
            rounded.addRoundedRect(rect, 6, 6);
            painter.fillPath(rounded, d->m_theme.color(QStringLiteral("--background-primary-alt")));
            const qreal iconSize = rect.height() - 16;
            const QIcon icon = Icons::icon(cell.isNote ? QStringLiteral("file-text")
                                                        : QStringLiteral("file"),
                                           muted, int(iconSize));
            const QRectF iconRect(rect.left() + 8, rect.top() + 8, iconSize, iconSize);
            if (!icon.isNull())
                icon.paint(&painter, iconRect.toRect());
            QFont font = d->m_theme.uiFont();
            painter.setFont(font);
            painter.setPen(text);
            const qreal textLeft = iconRect.right() + 8;
            const QFontMetrics metrics(font);
            // textLeft is in painter coordinates, so the room left is
            // measured to the card's own right edge (a card at x = 240 has
            // no negative width).
            const qreal room = rect.right() - 8 - textLeft;
            painter.drawText(QRectF(textLeft, rect.top(), room, rect.height()),
                             Qt::AlignVCenter | Qt::AlignLeft,
                             metrics.elidedText(cell.label, Qt::ElideMiddle, int(room)));
            painter.restore();
            continue;
        }
        if (cell.image.isNull()) {
            painter.fillRect(rect, muted);
            continue;
        }
        // A GIF cell plays; the rest (and a GIF with no slot) is the one pixmap.
        painter.drawPixmap(rect.toRect(), cell.panel || cell.isNote
                                              ? cell.image
                                              : gifFrame(painter, cell.openPath, rect, cell.image));
        if (cell.panel && !cell.label.isEmpty()) {
            painter.save();
            QFont small = d->m_theme.uiFont();
            small.setPixelSize(d->m_theme.value(QStringLiteral("--font-ui-smaller")).toInt());
            painter.setFont(small);
            painter.setPen(muted);
            painter.drawText(QRectF(rect.left(), rect.bottom() + 2, rect.width(), 16),
                             Qt::AlignLeft, cell.label);
            painter.restore();
        }
    }
}

const MarkdownEditor::EmbedCell *MarkdownEditor::embedCellOf(const BlockView &view, int cell,
                                                            QPointF &origin) const
{
    origin = {};
    if (cell < 0)
        return nullptr;
    if (size_t(cell) < view.embedCells.size())
        return &view.embedCells[size_t(cell)];
    size_t rest = size_t(cell) - view.embedCells.size();
    for (const Cell &tableCell : view.cells) {
        if (rest < tableCell.embeds.size()) {
            origin = cellTextOrigin(view, tableCell);
            return &tableCell.embeds[rest];
        }
        rest -= tableCell.embeds.size();
    }
    return nullptr;
}

QRectF MarkdownEditor::embedCellRectAt(qsizetype offset, int cell) const
{
    const qsizetype index = blockAt(offset);
    if (index < 0 || index >= qsizetype(d->m_blocks.size()))
        return {};
    QPointF origin;
    const EmbedCell *found = embedCellOf(d->m_blocks[size_t(index)], cell, origin);
    return found ? found->rect.translated(origin) : QRectF();
}

QString MarkdownEditor::embedCellLabelAt(qsizetype offset, int cell) const
{
    const qsizetype index = blockAt(offset);
    if (index < 0 || index >= qsizetype(d->m_blocks.size()))
        return {};
    QPointF origin;
    const EmbedCell *found = embedCellOf(d->m_blocks[size_t(index)], cell, origin);
    return found ? found->label : QString();
}

bool MarkdownEditor::isFileCardAt(qsizetype offset) const
{
    const qsizetype index = blockAt(offset);
    if (index < 0 || index >= qsizetype(d->m_blocks.size()))
        return false;
    return d->m_blocks[size_t(index)].isFileCard;
}

bool MarkdownEditor::fileCardHasCoverAt(qsizetype offset) const
{
    const qsizetype index = blockAt(offset);
    return index >= 0 && index < qsizetype(d->m_blocks.size()) && !d->m_blocks[size_t(index)].fileCardCover.isNull();
}

bool MarkdownEditor::isMediaCardAt(qsizetype offset) const
{
    const qsizetype index = blockAt(offset);
    return index >= 0 && index < qsizetype(d->m_blocks.size()) && !d->m_blocks[size_t(index)].mediaPath.isEmpty();
}

int MarkdownEditor::embedCellCountAt(qsizetype offset) const
{
    const qsizetype index = blockAt(offset);
    if (index < 0 || index >= qsizetype(d->m_blocks.size()))
        return 0;
    const BlockView &view = d->m_blocks[size_t(index)];
    size_t count = view.embedCells.size();
    for (const Cell &cell : view.cells)
        count += cell.embeds.size();
    return int(count);
}

QString MarkdownEditor::panelTextAt(qsizetype offset) const
{
    const qsizetype index = blockAt(offset);
    if (index < 0 || index >= qsizetype(d->m_blocks.size()))
        return QString();
    const BlockView &view = d->m_blocks[size_t(index)];
    return view.layout ? view.layout->text() : QString();
}

QString MarkdownEditor::panelLabelAt(qsizetype offset) const
{
    const qsizetype index = blockAt(offset);
    return index < 0 || index >= qsizetype(d->m_blocks.size()) ? QString() : d->m_blocks[size_t(index)].embedLabel;
}

QImage MarkdownEditor::embedImageAt(qsizetype offset) const
{
    const qsizetype index = blockAt(offset);
    if (index < 0 || index >= qsizetype(d->m_blocks.size()))
        return {};
    const BlockView &view = d->m_blocks[size_t(index)];
    return view.embedPanel ? view.image.toImage() : QImage();
}

QSize MarkdownEditor::embedImageSizeAt(qsizetype offset) const
{
    const qsizetype index = blockAt(offset);
    if (index < 0 || index >= qsizetype(d->m_blocks.size()))
        return {};
    const BlockView &view = d->m_blocks[size_t(index)];
    return view.embedPanel && !view.image.isNull() ? view.imageSize : QSize();
}

bool MarkdownEditor::handleEmbedCellClick(const BlockView &view, const QPoint &point, qreal left,
                                          qreal top)
{
    for (const EmbedCell &cell : view.embedCells) {
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
