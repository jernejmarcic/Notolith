#pragma once

#include <atomic>
#include <functional>
#include <optional>

#include <QElapsedTimer>
#include <QFileSystemWatcher>
#include <QFuture>
#include <QObject>
#include <QThreadPool>
#include <QTimer>

#include "Index.h"
#include "Vault.h"

namespace noteAppC {

/// Builds and maintains the Index: a scan on a thread of its own that walks
/// the vault, parses across the pool and writes through an Index::Writer,
/// then incremental updates driven by a file-system watcher. The Index's own
/// thread only takes each chunk's names and progress, and one changed() at
/// the end: writing each note there held a cold index's event loop for
/// seconds.
class Indexer : public QObject {
    Q_OBJECT

public:
    explicit Indexer(Index *index, QObject *parent = nullptr);
    ~Indexer() override;

    /// Opens the cache for `vault` and starts a scan. Files whose mtime and
    /// size already match the cache are skipped, so a warm start is cheap.
    /// An invalid vault stops everything.
    void setVault(const Vault &vault);
    /// The whole vault: every file the cache does not hold as it is on disk,
    /// and every note no longer there. Queued behind a scan in progress.
    void rebuild();
    void reindex(const QString &path);
    bool isRunning() const;
    /// Cancels a scan in progress and waits for it, dropping what it had not
    /// yet reported: afterwards the Index can be closed or reopened.
    void stop();

    /// A Jupyter notebook's text, from the bytes of its file; nullopt when the
    /// bytes are not one. With a reader set, `.ipynb` files are notes: indexed
    /// by that text (so search, links, tags and the quick switcher see what the
    /// note shows when opened), named by their full file name like any file
    /// that is not Markdown (`[[Analysis.ipynb]]`). Without one (the default)
    /// they are attachments, as before. Called on the scan's pool threads.
    /// Changing it rescans, so the notebooks already in the cache change kind.
    using NotebookReader = std::function<std::optional<QString>(const QByteArray &)>;
    void setNotebookReader(NotebookReader reader);

    /// Files the vault never indexes: the config directory, the trash, and
    /// anything dot-prefixed.
    static bool isIgnored(const QString &relativePath);

signals:
    void progress(int done, int total);
    void finished(int indexed, qint64 milliseconds);

private:
    struct Parsed {
        Metadata metadata;
        QString body;
        bool ok = false;
    };
    static Parsed parseFile(const QString &path, const NotebookReader &notebooks);
    bool isNotebook(const QString &path) const;

    /// What a scan tells the Index's thread, a chunk at a time.
    struct Batch {
        quint64 scan = 0; ///< stale once stop() has moved m_scanNumber on
        QList<QPair<QString, QStringList>> names; ///< vault-relative path, its names now (none: gone)
        int done = 0;
        int total = 0;
        bool last = false;
    };
    /// `paths` empty means the whole vault.
    void start(const QStringList &paths);
    void runScan(const QStringList &only, quint64 scan, const Vault &vault, const QString &databasePath,
                 const NotebookReader &notebooks);
    void apply(const Batch &batch);
    void rewatch();
    void onDirectoryChanged(const QString &path);

    Index *m_index = nullptr;
    Vault m_vault;
    QThreadPool m_pool; ///< the scan's one thread
    QFuture<void> m_scan;
    std::atomic<quint64> m_scanNumber{0};
    bool m_wrote = false;  ///< this scan changed the cache: changed() is due at its end
    bool m_rescan = false; ///< the vault changed during a scan, so walk it again after
    QStringList m_pending; ///< notes changed during a scan, reindexed after it
    NotebookReader m_notebookReader;
    QFileSystemWatcher m_files;
    QTimer m_debounce;
    QElapsedTimer m_clock;
    int m_indexed = 0;
};

} // namespace noteAppC
