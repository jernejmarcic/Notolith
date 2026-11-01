#pragma once

// Remote images in notes (docs/OFFLINE.md), only in a NOTEAPPC_WITH_NETWORK build. One object for the
// process; every editor asks it and listens for the answer. It starts nothing by itself: a fetch begins
// with load(), which the editor calls for a click or for an origin the person allowed.
//
//   load(url): rules (RemoteImagePolicy) -> the decoder must be runnable, else no download -> the cache,
//   or a fetch (ImageFetcher) -> magic bytes -> cache -> notolith-imagedecode under bubblewrap ->
//   loaded(url, image) or failed(url, message).

#include "ImageFetcher.h"
#include "ImageStore.h"
#include "RemoteImagePolicy.h"

#include <QImage>
#include <QObject>
#include <QSet>
#include <QThreadPool>
#include <QUrl>

#include <deque>

namespace noteAppC::remoteimage {

class RemoteImages : public QObject {
    Q_OBJECT
public:
    struct Config {
        QString allowlistPath;
        QString cacheDir;
        QNetworkAccessManager *manager = nullptr; ///< a test's own; null makes one
    };
    explicit RemoteImages(const Config &config, QObject *parent = nullptr);
    /// The process's one, on the default state and cache directories.
    static RemoteImages &instance();
    /// For the tests: `instance()` answers with `replacement` (null restores the real one).
    static void useForTests(RemoteImages *replacement);

    bool isAllowed(const QUrl &url) const { return m_allowlist.contains(originOf(url)); }
    void allowOrigin(const QUrl &url) { m_allowlist.add(originOf(url)); }
    void clearAllowlist() { m_allowlist.clear(); }
    int allowedCount() const { return int(m_allowlist.origins().size()); }
    const ImageAllowlist &allowlist() const { return m_allowlist; }
    ImageCache &cache() { return m_cache; }
    ImageFetcher &fetcher() { return m_fetcher; }

    /// Starts loading `url` unless it is already on its way. Answers with loaded() or failed(), always
    /// from the event loop.
    void load(const QUrl &url);

signals:
    void loaded(const QUrl &url, const QImage &image);
    void failed(const QUrl &url, const QString &message);

private:
    void startFetch(const QUrl &url);
    void decode(const QUrl &url, const QByteArray &bytes);
    void answer(const QUrl &url, const QImage &image, const QString &error);
    void pump();

    ImageAllowlist m_allowlist;
    ImageCache m_cache;
    ImageFetcher m_fetcher;
    QThreadPool m_decoders;
    QSet<QUrl> m_pending; ///< every url on its way
    std::deque<QUrl> m_waiting;  ///< not yet fetching
    int m_fetching = 0;
};

} // namespace noteAppC::remoteimage
