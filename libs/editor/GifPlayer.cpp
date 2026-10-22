/// Animated GIFs in live preview, as Obsidian plays them.
///
/// Every image embed - `![[x.gif]]`, `![](x.gif)`, `<img src="x.gif">`, alone
/// in a block, inline in text, or in a table cell - is drawn from one QPixmap
/// (its first frame) by the code that draws any image. For a GIF that code
/// asks gifFrame() what to draw instead, and gifFrame() decides whether a
/// QMovie is worth running:
///
/// - only a file with more than one frame (a still GIF never makes a movie);
/// - only while the editor is shown and the embed is painted inside the
///   viewport, on screen (not into a PDF or a grab of the whole note);
/// - at most kMaxGifMovies at once, the rest stay on their first frame;
/// - one movie per file, however many times the note embeds it.
///
/// A movie ends when its last embed leaves the viewport (scrolling, resizing,
/// an edit that removes it), when the editor is hidden, and when the note or
/// the resolver changes. Each frame repaints the embed's own rectangle and
/// nothing else. A note with no animated GIF never constructs a QMovie, a
/// timer or a connection: m_gifs and m_gifAnimated stay empty.

#include "MarkdownEditor.h"
#include "MarkdownEditor_p.h"

#include <QImageReader>
#include <QPainter>
#include <QScrollBar>
#include <QTimer>

namespace noteAppC {

namespace {

constexpr int kMaxGifMovies = 8;

} // namespace

/// Ends one movie. It can be the one whose frameChanged is running this very
/// call, so it is stopped and cut loose now but freed a turn of the event loop later.
static void releaseMovie(QObject *owner, std::shared_ptr<QMovie> movie)
{
    movie->stop();
    movie->disconnect(owner);
    QTimer::singleShot(0, owner, [movie = std::move(movie)]() mutable { movie.reset(); });
}

QPixmap MarkdownEditor::gifFrame(const QPainter &painter, const QString &path, const QRectF &target,
                                 const QPixmap &still) const
{
    if (!path.endsWith(QLatin1String(".gif"), Qt::CaseInsensitive))
        return still;
    if (const auto playing = d->m_gifs.constFind(path); playing != d->m_gifs.cend()) {
        const QPixmap frame = (*playing)->currentPixmap();
        return frame.isNull() ? still : frame;
    }
    // Only what is on screen starts: the editor shown, painting to it, with
    // the embed inside the viewport and a slot free.
    if (!viewport()->isVisible() || painter.device() != viewport()
        || !target.intersects(QRectF(viewport()->rect())) || d->m_gifs.size() >= kMaxGifMovies)
        return still;
    auto known = d->m_gifAnimated.find(path);
    if (known == d->m_gifAnimated.end())
        known = d->m_gifAnimated.insert(path, QImageReader(path, "gif").imageCount() > 1);
    if (!known.value())
        return still;

    auto movie = std::make_shared<QMovie>(path, QByteArrayLiteral("gif"));
    if (!movie->isValid())
        return still;
    auto *self = const_cast<MarkdownEditor *>(this);
    connect(movie.get(), &QMovie::frameChanged, self, [self, path] { self->gifFrameChanged(path); });
    d->m_gifs.insert(path, movie);
    movie->start();
    const QPixmap frame = movie->currentPixmap();
    return frame.isNull() ? still : frame;
}

QList<QRect> MarkdownEditor::gifRectsInView(const QString &path) const
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
        for (const EmbedCell &cell : view.embedCells) {
            if (cell.openPath == path && !cell.image.isNull())
                add(cell.rect.translated(left, top));
        }
        for (const Cell &cell : view.cells) {
            for (const EmbedCell &embed : cell.embeds) {
                if (embed.openPath == path && !embed.image.isNull())
                    add(embed.rect.translated(cellTextOrigin(view, cell) + QPointF(left, top)));
            }
        }
        if (view.imagePath == path && !view.image.isNull()) {
            const qreal x = view.imageCentred ? left + (contentWidth() - view.imageSize.width()) / 2.0 : left;
            add(QRectF(x, top + view.textTop, view.imageSize.width(), view.imageSize.height()));
        }
    }
    return found;
}

void MarkdownEditor::gifFrameChanged(const QString &path)
{
    if (!d->m_gifs.contains(path))
        return;
    const QList<QRect> rects = gifRectsInView(path);
    if (rects.isEmpty()) {
        releaseMovie(this, d->m_gifs.take(path)); // its embed is gone or off screen
        return;
    }
    for (const QRect &rect : rects)
        viewport()->update(rect);
}

void MarkdownEditor::pruneGifs()
{
    if (d->m_gifs.isEmpty())
        return;
    for (const QString &path : d->m_gifs.keys()) {
        if (gifRectsInView(path).isEmpty())
            releaseMovie(this, d->m_gifs.take(path));
    }
}

void MarkdownEditor::clearGifs()
{
    const auto movies = std::exchange(d->m_gifs, {});
    for (const auto &movie : movies)
        releaseMovie(this, movie);
    d->m_gifAnimated.clear();
}

int MarkdownEditor::gifMovieCount() const
{
    return int(d->m_gifs.size());
}

int MarkdownEditor::gifFrameOf(const QString &path) const
{
    const auto playing = d->m_gifs.constFind(path);
    return playing == d->m_gifs.cend() ? -1 : (*playing)->currentFrameNumber();
}

} // namespace noteAppC
