#pragma once

#include <atomic>
#include <functional>
#include <optional>

#include <QElapsedTimer>
#include <QFileSystemWatcher>
#include <QHash>
#include <QFuture>
#include <QObject>
#include <QThreadPool>
#include <QTimer>

#include "Index.h"
#include "Parser.h"
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
    /// Brings the index up to date with what is on disk now, for at most
    /// `milliseconds`: a change the watcher has not acted on yet is acted on
    /// now, and the scans queued or running are waited for. Only the indexer's
    /// own results are delivered meanwhile, no other event. Returns at once
    /// when nothing is pending. For a caller about to read links the index
    /// must already know, such as a rename's rewrite.
    void waitIdle(int milliseconds);
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

    /// Meta notes are indexed (for `meta:` search) only while
    /// `options.metaNotes` is on, by its `open`/`close`; the rest of `options`
    /// is ignored. Turning it on or changing the delimiters re-parses every
    /// note, unchanged ones too; turning it off drops the rows without a scan.
    void setMetaNotes(const ParseOptions &options);

    /// Files the vault never indexes: the config directory, the trash, and
    /// anything dot-prefixed.
    static bool isIgnored(const QString &relativePath);

    /// Some folder could not be watched (fs.inotify.max_user_watches, or no
    /// permission): changes made outside the app are then found by a rescan
    /// every 30 s instead of at once.
    bool pollsForChanges() const { return m_poll.isActive(); }

signals:
    void progress(int done, int total);
    void finished(int indexed, qint64 milliseconds);
    /// Once, when pollsForChanges() turns on; `folders` could not be watched.
    void watchesFailed(int folders);

private:
    struct Parsed {
        Metadata metadata;
        QString body;
        bool ok = false;
    };
    static Parsed parseFile(const QString &path, const NotebookReader &notebooks, const ParseOptions &options);
    bool isNotebook(const QString &path) const;
    void clearMetaNotes(); ///< the rows and the key, at once

    /// What a scan tells the Index's thread, a chunk at a time.
    struct Batch {
        quint64 scan = 0; ///< stale once stop() has moved m_scanNumber on
        QList<QPair<QString, QStringList>> names; ///< vault-relative path, its names now (none: gone)
        int done = 0;
        int total = 0;
        bool last = false;
        /// A whole-vault walk's directories with their mtimes when it read them,
        /// so the watches come from the scan's own walk (rewatch() is the fallback).
        QHash<QString, qint64> directories;
    };
    /// `paths` empty means the whole vault; `force` re-parses notes the cache
    /// already holds as they are on disk.
    void start(const QStringList &paths, bool force = false);
    void runScan(const QStringList &only, quint64 scan, const Vault &vault, const QString &databasePath,
                 const NotebookReader &notebooks, const ParseOptions &options, bool force);
    void apply(const Batch &batch);
    void rewatch();
    void watch(const QHash<QString, qint64> &directories);
    void addWatches(const QStringList &directories); ///< addPaths, and the poll for what it could not watch
    void onDirectoryChanged(const QString &path);

    Index *m_index = nullptr;
    Vault m_vault;
    QThreadPool m_pool; ///< the scan's one thread
    QFuture<void> m_scan;
    std::atomic<quint64> m_scanNumber{0};
    bool m_active = false; ///< a scan is running, or its last batch is not applied yet
    bool m_wrote = false;  ///< this scan changed the cache: changed() is due at its end
    bool m_rescan = false; ///< the vault changed during a scan, so walk it again after
    bool m_forceRescan = false; ///< and that walk re-parses every note
    ParseOptions m_scanOptions; ///< what the running scan parses with
    bool m_scanSetsKey = false; ///< it re-parses every note, so its end records m_scanOptions
    bool m_clearMetaNotes = false; ///< switched off during a scan: its end clears the rows
    ParseOptions m_parseOptions; ///< only the meta-note fields set
    QStringList m_pending; ///< notes changed during a scan, reindexed after it
    NotebookReader m_notebookReader;
    QFileSystemWatcher m_files;
    QTimer m_debounce;
    QTimer m_poll; ///< runs only while some folder has no watch
    QElapsedTimer m_clock;
    int m_indexed = 0;
};

} // namespace noteAppC
