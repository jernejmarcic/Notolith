#pragma once

#include <functional>
#include <list>

#include <QHash>
#include <QImage>
#include <QObject>
#include <QPair>
#include <QRect>
#include <QRectF>

namespace noteAppC::ink {

/// The tiles of the canvas (spec section 7, Rendering; R5): 512 x 512 device px images, rendered off the
/// UI thread, kept under a byte budget (the setting `ink.tileCacheMB`) by evicting the least recently
/// painted. A tile (tx, ty) is the cell of a grid laid over the canvas in "tile space": logical widget px
/// at the zoom the cache was last given by setScale (a cell is kTileSize / devicePixelRatio of them wide).
/// The cache never renders on the UI thread and knows nothing of documents: the canvas hands it a pure
/// job per tile (a snapshot of the strokes, no widget, no QPixmap) and rects in tile space.
class TileCache : public QObject {
    Q_OBJECT
public:
    static constexpr int kTileSize = 512; // device px
    static constexpr qsizetype kDefaultMaxBytes = 64 * 1024 * 1024; // R5: the setting `ink.tileCacheMB`, default 64
    explicit TileCache(qsizetype maxBytes = kDefaultMaxBytes, QObject *parent = nullptr);
    ~TileCache() override; ///< waits for the jobs in flight: they hold nothing of the cache, but their result must not outlive it

    qsizetype maxBytes() const { return m_maxBytes; }
    void setMaxBytes(qsizetype maxBytes); ///< lowering it evicts least-recently-painted tiles at once
    double zoom() const { return m_zoom; }
    double devicePixelRatio() const { return m_dpr; }
    /// A change of zoom makes every tile stale (the image stays, and still says what zoom it was rendered at, until
    /// replaced); a change of pixel ratio changes the size of a cell, so the old images belong to no cell and go.
    /// Either way a render in flight is discarded (and cancelled if it has not started).
    void setScale(double zoom, double devicePixelRatio);
    /// Tiles touching `tileSpaceRect` become stale. A render in flight for one is kept: when it finishes its image is
    /// stored, still stale (it holds everything before the change; the canvas asks again) - a continuous eraser sweep
    /// would otherwise keep a dense tile from ever finishing.
    void invalidate(const QRectF &tileSpaceRect);
    void invalidateAll();
    /// Renders in flight for cells outside `cells` (tile-space cell indices, inclusive) are cancelled if they have not
    /// started and their result dropped if they have: the pool is first-in first-out, and a render queued for a cell the
    /// view has left would run before the ones it is looking at (N1). The cell stays pending until the pool reaches it.
    void keepOnly(const QRect &cells);

    struct Lookup {
        const QImage *image = nullptr;
        bool fresh = false;
        double scale = 1.0; ///< the zoom the image was rendered at
    };
    /// UI thread. Never renders. `image` is null when the tile was never rendered; `fresh` is false when it is stale:
    /// the canvas then paints it scaled (blurry, instant) or the page background, and calls request().
    /// Non-const on purpose (R5): a hit moves the tile to the most-recently-painted end of the LRU list.
    Lookup find(int tx, int ty);
    bool isFresh(int tx, int ty) const;
    /// UI thread. Starts `job` on the thread pool unless this tile already has a render in flight (false then).
    /// The result is stored when it finishes (stale if the tile was invalidated meanwhile, dropped if the scale changed);
    /// tileReady is emitted either way, so the canvas paints again and asks again for a stale or dropped one.
    /// `job` runs on a pool thread: it must be pure data - it paints strokes and shapes from an immutable snapshot
    /// (their payloads are shared_ptr<const>), and touches no widget, no QPixmap and not the document.
    bool request(int tx, int ty, std::function<QImage()> job);
    bool isPending(int tx, int ty) const { return m_pending.contains({tx, ty}); }
    int pendingCount() const { return int(m_pending.size()); }
    qsizetype bytes() const { return m_bytes; }
    int count() const { return int(m_lru.size()); }
    int staleCount() const;

signals:
    void tileReady(int tx, int ty); ///< UI thread: the canvas does update(tileRect)

private:
    using Key = QPair<int, int>;
    struct Entry {
        Key key;
        QImage image;
        double scale = 1.0;
        bool fresh = false;
    };
    struct Pending;
    QRectF cell(const Key &key) const;
    void finish(const Key &key);
    void evict();

    qsizetype m_maxBytes;
    qsizetype m_bytes = 0;
    double m_zoom = 1.0;
    double m_dpr = 1.0;
    std::list<Entry> m_lru; ///< front: the most recently painted
    QHash<Key, std::list<Entry>::iterator> m_index;
    QHash<Key, Pending *> m_pending;
};

} // namespace noteAppC::ink
