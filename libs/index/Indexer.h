// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej Marčič
#pragma once

#include <atomic>
#include <functional>
#include <optional>

#include <QElapsedTimer>
#include <QHash>
#include <QFuture>
#include <QObject>
#include <QSet>
#include <QThreadPool>
#include <QTimer>

#include "DirWatcher.h"
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
    /// The open vault was trusted or not: its regex ignore filters count from the next scan, which this starts
    /// when that changes what they do.
    void setVaultTrusted(bool trusted);
    /// The whole vault: every file the cache does not hold as it is on disk,
    /// and every note no longer there. Queued behind a scan in progress.
    void rebuild();
    void reindex(const QString &path);
    /// A scan is running, or its last batch is not applied yet.
    bool isRunning() const;
    /// The notes (absolute paths) a reindex() has asked for that the Index does
    /// not hold as saved yet: the running scan's, and those queued behind it.
    /// A rename reads them from disk as candidates, so a note saved a moment ago
    /// has its link rewritten without waiting for the scan. While a whole-vault
    /// scan runs or waits, the Index may not hold any note yet (a new vault):
    /// then it is every note of the vault, walked here.
    QStringList pathsInFlight() const;
    /// A whole-vault scan runs or waits: the Index may not know every note yet.
    bool isScanningWholeVault() const { return m_active && (m_scanWhole || m_rescan); }
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

    /// The same for any file kind the host can read as text: a reader
    /// per lower-case suffix, set for each of `suffixes`; an empty one clears
    /// them. `setNotebookReader` is this for "ipynb". Rescans
    /// when a suffix gains or loses its reader and the vault is valid. A file
    /// over `maxBytes` is never read (0: no limit; the default is Vault::kMaxNoteBytes, the
    /// same cap a Markdown note has), so a hostile multi-GB file costs nothing.
    using ForeignReader = NotebookReader;
    struct ForeignSource {
        ForeignReader read;
        qint64 maxBytes = Vault::kMaxNoteBytes;
    };
    void setForeignReader(const QStringList &suffixes, ForeignReader reader, qint64 maxBytes = Vault::kMaxNoteBytes);

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

    /// Vault-relative paths the scans skipped for being over their size limit (32 MiB for a note or a
    /// notebook), sorted; each was logged once. Not indexed, so not searchable; opening one says why not.
    QStringList tooLargeFiles() const
    {
        QStringList files(m_tooLarge.cbegin(), m_tooLarge.cend());
        files.sort();
        return files;
    }

    /// How many scans have walked the whole vault, for the tests: a save or an
    /// outside change reads only the folders the watcher named.
    int wholeScans() const { return m_wholeScans; }

signals:
    void progress(int done, int total);
    void finished(int indexed, qint64 milliseconds);
    /// Once, when pollsForChanges() turns on; `folders` could not be watched.
    void watchesFailed(int folders);
    /// Something came or went under a watched folder (any file, not only notes), or
    /// events were lost. Raw, a burst raises many: the window debounces it and
    /// asks its open notes whether their links and tables are still there.
    void vaultFilesChanged();

private:
    struct Parsed {
        Metadata metadata;
        QString body;
        bool ok = false;
        bool tooLarge = false; ///< not ok because the file is over its size limit
    };
    static Parsed parseFile(const QString &path, const QHash<QString, ForeignSource> &foreign, const ParseOptions &options);
    bool isForeign(const QString &path) const; ///< a suffix with a reader set
    void clearMetaNotes(); ///< the rows and the key, at once

    /// What a scan tells the Index's thread, a chunk at a time.
    struct Batch {
        quint64 scan = 0; ///< stale once stop() has moved m_scanNumber on
        QList<QPair<QString, QStringList>> names; ///< vault-relative path, its names now (none: gone)
        QStringList tooLarge; ///< vault-relative, skipped for size in this chunk
        int done = 0;
        int total = 0;
        bool last = false;
        /// A whole-vault walk's directories with their mtimes when it read them,
        /// so the watches come from the scan's own walk (rewatch() is the fallback).
        /// `partial`: only folders found new by a walk of some changed folders,
        /// to be added to the watches, none dropped.
        QHash<QString, qint64> directories;
        bool partial = false;
    };
    /// `paths` and `dirs` both empty means the whole vault. Otherwise the notes
    /// `paths`, and the entries of the folders `dirs` (a folder new to the
    /// watcher whole): what the watcher reported. `force` re-parses notes the
    /// cache already holds as they are on disk.
    void start(const QStringList &paths, bool force = false, const QStringList &dirs = {});
    void runScan(const QStringList &only, const QStringList &dirs, const QSet<QString> &watched, quint64 scan,
                 const Vault &vault, const QString &databasePath, const QHash<QString, ForeignSource> &foreign,
                 const ParseOptions &options, bool force);
    void flushChanges();
    void apply(const Batch &batch);
    void rewatch();
    void watch(const QHash<QString, qint64> &directories, bool partial);
    void addWatches(const QStringList &directories); ///< addPaths, and the poll for what it could not watch
    void onDirectoryChanged(const QString &path);
    void onFileChanged(const QString &path);

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
    QSet<QString> m_changedDirs; ///< folders the watcher reported, until the debounce hands them to a scan
    QSet<QString> m_changedFiles; ///< notes it saw written in place, the same
    QSet<QString> m_queuedDirs;  ///< folders handed over during a scan, read after it
    QStringList m_scanPaths; ///< what the running scan was asked for; empty for the whole vault
    bool m_scanWhole = false; ///< the running scan walks the whole vault
    QHash<QString, ForeignSource> m_foreignReaders; ///< by lower-case suffix
    QSet<QString> m_tooLarge; ///< see tooLargeFiles()
    DirWatcher m_files;
    QTimer m_debounce;
    QTimer m_poll; ///< runs only while some folder has no watch
    QElapsedTimer m_clock;
    int m_indexed = 0;
    int m_wholeScans = 0;
};

} // namespace noteAppC
