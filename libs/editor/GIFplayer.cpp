// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej Marčič
/// Animated GIFs in live preview, as Obsidian plays them.
///
/// Every image embed - `![[x.gif]]`, `![](x.gif)`, `<img src="x.gif">`, alone
/// in a block, inline in text, or in a table cell - is drawn from one QPixmap
/// (its first frame) by the code that draws any image. For a GIF that code
/// asks GIFframe() what to draw instead, and GIFframe() decides whether a
/// QMovie is worth running:
///
/// - only a file with more than one frame (a still GIF never makes a movie);
/// - only while the editor is shown and the embed is painted inside the
///   viewport, to the screen: a render to a PDF or a printer (any paint engine
///   but the raster one) draws the first frame;
/// - at most kMaxGIFmovies at once, the rest stay on their first frame;
/// - one movie per file, however many times the note embeds it.
///
/// A movie ends when its last embed leaves the viewport (scrolling, resizing,
/// an edit that removes it), when the editor is hidden, when the note or the
/// resolver changes, and when it finishes or fails: a GIF that played out
/// keeps its last frame and frees its slot, a broken one stays on its first.
/// Each frame repaints the embed's own rectangle and nothing else. A note with
/// no animated GIF never constructs a QMovie, a timer or a connection: m_GIFs
/// and m_GIFinfo stay empty.

#include "MarkdownEditor.h"
#include "ImageLoad.h"
#include "MarkdownEditor_p.h"

#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QPaintEngine>
#include <QPainter>
#include <QScrollBar>
#include <QTimer>

namespace noteAppC {

namespace {

constexpr int kMaxGIFmovies = 8;

} // namespace

/// Ends one movie. It can be the one whose frameChanged is running this very
/// call, so it is stopped and cut loose now but freed a turn of the event loop later.
static void releaseMovie(QObject *owner, std::shared_ptr<QMovie> movie)
{
    movie->stop();
    movie->disconnect(owner);
    QTimer::singleShot(0, owner, [movie = std::move(movie)]() mutable { movie.reset(); });
}

QPixmap MarkdownEditor::GIFframe(const QPainter &painter, const QString &path, const QRectF &target,
                                 const QPixmap &still) const
{
    if (!path.endsWith(QLatin1String(".gif"), Qt::CaseInsensitive))
        return still;
    // Only the shown editor painting itself to the screen plays: a PDF or a
    // printer render runs this same paint code through another paint engine.
    if (!viewport()->isVisible() || painter.device() != viewport() || !painter.paintEngine()
        || painter.paintEngine()->type() != QPaintEngine::Raster)
        return still;
    if (const auto playing = d->m_GIFs.constFind(path); playing != d->m_GIFs.cend()) {
        const QPixmap frame = (*playing)->currentPixmap();
        return frame.isNull() ? still : frame;
    }
    if (!target.intersects(QRectF(viewport()->rect())))
        return still;

    // What is known of the file stays valid while its mtime does: one scan
    // for the frame count per file version, not one per paint.
    auto &info = d->m_GIFinfo[path];
    const qint64 mtime = QFileInfo(path).lastModified().toMSecsSinceEpoch();
    if (!info.known || info.mtime != mtime) {
        info = {};
        info.known = true;
        info.mtime = mtime;
        // Only a file whose bytes say GIF plays, whatever its name; the reader gets no other format to try.
        QFile file(path);
        if (file.open(QIODevice::ReadOnly) && imageFormat(file.peek(512)) == "gif") {
            QImageReader reader(&file, "gif");
            reader.setAutoDetectImageFormat(false);
            info.animated = reader.imageCount() > 1;
        }
    }
    if (!info.ended.isNull())
        return info.ended;
    if (!info.animated || info.broken || d->m_GIFs.size() >= kMaxGIFmovies)
        return still;

    // QMovie has no switch for autodetection; the file is a GIF by its bytes (above), so "gif" is what it tries.
    auto movie = std::make_shared<QMovie>(path, QByteArrayLiteral("gif"));
    if (!movie->isValid()) {
        info.broken = true;
        return still;
    }
    auto *self = const_cast<MarkdownEditor *>(this);
    connect(movie.get(), &QMovie::finished, self, [self, path] { self->GIFstopped(path, true); });
    connect(movie.get(), &QMovie::error, self, [self, path] { self->GIFstopped(path, false); });
    d->m_GIFs.insert(path, movie);
    movie->start();
    // After start(): its first frameChanged would repaint from inside this paint.
    connect(movie.get(), &QMovie::frameChanged, self, [self, path] { self->GIFframeChanged(path); });
    const QPixmap frame = movie->currentPixmap();
    return frame.isNull() ? still : frame;
}

QList<QRect> MarkdownEditor::GIFrectsInView(const QString &path) const
{
    // Where the painters put a GIF: the same origins paintBlocks hands them.
    QList<QRect> found;
    const qreal scroll = verticalScrollBar()->value();
    const qreal left = contentLeft();
    const QRectF window = viewport()->rect();
    const auto add = [&](const QRectF &rect) {
        if (rect.intersects(window))
            found.append(rect.toAlignedRect());
    };
    const auto first = std::partition_point(d->m_blocks.cbegin(), d->m_blocks.cend(),
                                            [scroll](const BlockView &view) { return view.bottomY() - scroll < 0; });
    for (auto it = first; it != d->m_blocks.cend(); ++it) {
        const BlockView &view = *it;
        const qreal top = view.y - scroll;
        if (top > window.bottom())
            break;
        if (view.tabHidden || view.foldHidden || view.folded)
            continue;
        for (const EmbedCell &cell : view.x().embedCells) {
            // The painter animates a plain image only (paintEmbedCellList).
            if (cell.openPath == path && !cell.image.isNull() && !cell.panel && !cell.isNote)
                add(cell.rect.translated(left, top));
        }
        // A table clips its cells to the readable column, and skips the ones
        // scrolled out of it sideways (paintTable).
        const QRectF tableWindow(left, top, contentWidth(), view.height);
        const bool clipped = view.sideWidth > contentWidth();
        for (const Cell &cell : view.x().cells) {
            if (clipped) {
                const QRectF box = cell.rect.translated(left - view.sideScroll, top);
                if (box.right() < tableWindow.left() - 2 || box.left() > tableWindow.right() + 2)
                    continue;
            }
            for (const EmbedCell &embed : cell.embeds) {
                if (embed.openPath != path || embed.image.isNull() || embed.panel || embed.isNote)
                    continue;
                const QRectF rect = embed.rect.translated(cellTextOrigin(view, cell) + QPointF(left, top));
                add(clipped ? rect.intersected(tableWindow) : rect);
            }
        }
        if (view.x().imagePath == path && !view.x().image.isNull()) {
            const qreal x = view.x().imageCentred ? left + (contentWidth() - view.x().imageSize.width()) / 2.0 : left;
            add(QRectF(x, top + view.textTop, view.x().imageSize.width(), view.x().imageSize.height()));
        }
    }
    return found;
}

void MarkdownEditor::GIFframeChanged(const QString &path)
{
    if (!d->m_GIFs.contains(path))
        return;
    const QList<QRect> rects = GIFrectsInView(path);
    if (rects.isEmpty()) {
        releaseMovie(this, d->m_GIFs.take(path)); // its embed is gone or off screen
        return;
    }
    for (const QRect &rect : rects)
        viewport()->update(rect);
}

void MarkdownEditor::GIFstopped(const QString &path, bool finished)
{
    const auto playing = d->m_GIFs.constFind(path);
    if (playing == d->m_GIFs.cend())
        return;
    auto &info = d->m_GIFinfo[path];
    if (finished)
        info.ended = (*playing)->currentPixmap(); // a GIF that played out stays on its last frame
    else
        info.broken = true; // a truncated or unreadable file: its first frame from now on
    const QList<QRect> rects = GIFrectsInView(path);
    releaseMovie(this, d->m_GIFs.take(path));
    for (const QRect &rect : rects)
        viewport()->update(rect);
}

void MarkdownEditor::pruneGIFs()
{
    if (d->m_GIFs.isEmpty())
        return;
    for (const QString &path : d->m_GIFs.keys()) {
        if (GIFrectsInView(path).isEmpty())
            releaseMovie(this, d->m_GIFs.take(path));
    }
}

void MarkdownEditor::clearGIFs()
{
    const auto movies = std::exchange(d->m_GIFs, {});
    for (const auto &movie : movies)
        releaseMovie(this, movie);
    // What the files are is kept (mtime says when that stops being true); what
    // happened to them in this showing - played out, failed - starts over.
    for (auto &info : d->m_GIFinfo) {
        info.ended = QPixmap();
        info.broken = false;
    }
}

int MarkdownEditor::GIFmovieCount() const
{
    return int(d->m_GIFs.size());
}

int MarkdownEditor::tableCellsPainted() const
{
    return d->m_tableCellsPainted;
}

int MarkdownEditor::GIFframeOf(const QString &path) const
{
    const auto playing = d->m_GIFs.constFind(path);
    return playing == d->m_GIFs.cend() ? -1 : (*playing)->currentFrameNumber();
}

} // namespace noteAppC
