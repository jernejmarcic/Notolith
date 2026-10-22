#include "TileCache.h"

#include <QFutureWatcher>
#include <QtConcurrent>

namespace noteAppC::ink {

struct TileCache::Pending {
    QFutureWatcher<QImage> *watcher = nullptr;
    bool discard = false; ///< invalidated or rescaled while it rendered: the result shows old content
    double scale = 1.0;   ///< the zoom it renders for
};

TileCache::TileCache(qsizetype maxBytes, QObject *parent) : QObject(parent), m_maxBytes(maxBytes) {}

TileCache::~TileCache()
{
    for (Pending *p : std::as_const(m_pending)) {
        p->watcher->disconnect(this);
        p->watcher->waitForFinished(); // the job may use things its owner is about to destroy
        delete p->watcher;
        delete p;
    }
}

QRectF TileCache::cell(const Key &key) const
{
    const double side = kTileSize / m_dpr;
    return QRectF(key.first * side, key.second * side, side, side);
}

void TileCache::setMaxBytes(qsizetype maxBytes)
{
    m_maxBytes = maxBytes;
    evict();
}

void TileCache::setScale(double zoom, double devicePixelRatio)
{
    if (qFuzzyCompare(zoom, m_zoom) && qFuzzyCompare(devicePixelRatio, m_dpr))
        return;
    if (!qFuzzyCompare(devicePixelRatio, m_dpr)) { // another cell size: the images fit no cell
        m_lru.clear();
        m_index.clear();
        m_bytes = 0;
    }
    m_zoom = zoom;
    m_dpr = devicePixelRatio;
    invalidateAll();
}

void TileCache::invalidate(const QRectF &tileSpaceRect)
{
    for (Entry &e : m_lru)
        if (e.fresh && cell(e.key).intersects(tileSpaceRect))
            e.fresh = false;
    for (auto it = m_pending.begin(); it != m_pending.end(); ++it)
        if (cell(it.key()).intersects(tileSpaceRect))
            it.value()->discard = true;
}

void TileCache::invalidateAll()
{
    for (Entry &e : m_lru)
        e.fresh = false;
    for (Pending *p : std::as_const(m_pending))
        p->discard = true;
}

TileCache::Lookup TileCache::find(int tx, int ty)
{
    const auto it = m_index.constFind({tx, ty});
    if (it == m_index.cend())
        return {};
    m_lru.splice(m_lru.begin(), m_lru, *it); // iterators stay valid
    const Entry &e = m_lru.front();
    return {&e.image, e.fresh, e.scale};
}

bool TileCache::isFresh(int tx, int ty) const
{
    const auto it = m_index.constFind({tx, ty});
    return it != m_index.cend() && (*it)->fresh;
}

int TileCache::staleCount() const
{
    int n = 0;
    for (const Entry &e : m_lru)
        n += !e.fresh;
    return n;
}

bool TileCache::request(int tx, int ty, std::function<QImage()> job)
{
    const Key key{tx, ty};
    if (m_pending.contains(key))
        return false;
    auto *p = new Pending;
    p->scale = m_zoom;
    p->watcher = new QFutureWatcher<QImage>(this);
    connect(p->watcher, &QFutureWatcherBase::finished, this, [this, key] { finish(key); });
    p->watcher->setFuture(QtConcurrent::run(std::move(job)));
    m_pending.insert(key, p);
    return true;
}

void TileCache::finish(const Key &key)
{
    Pending *p = m_pending.take(key);
    if (!p)
        return;
    if (!p->discard) {
        QImage image = p->watcher->result();
        const auto found = m_index.find(key);
        if (found != m_index.end()) {
            m_bytes -= (*found)->image.sizeInBytes();
            m_lru.splice(m_lru.begin(), m_lru, *found);
        } else {
            m_lru.push_front(Entry{key, {}, 1.0, false});
            m_index.insert(key, m_lru.begin());
        }
        Entry &e = m_lru.front();
        e.image = std::move(image);
        e.scale = p->scale;
        e.fresh = true;
        m_bytes += e.image.sizeInBytes();
    }
    p->watcher->deleteLater();
    delete p;
    evict(); // after the pending entry is gone: its own tile may now be the one to go
    emit tileReady(key.first, key.second);
}

void TileCache::evict()
{
    auto it = m_lru.end();
    while (m_bytes > m_maxBytes && it != m_lru.begin()) {
        --it;
        if (m_pending.contains(it->key))
            continue; // a render is in flight for it
        m_bytes -= it->image.sizeInBytes();
        m_index.remove(it->key);
        it = m_lru.erase(it);
    }
}

} // namespace noteAppC::ink
