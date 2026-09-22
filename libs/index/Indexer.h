#pragma once

#include <QElapsedTimer>
#include <QFileSystemWatcher>
#include <QFutureWatcher>
#include <QObject>
#include <QTimer>

#include "Index.h"
#include "Vault.h"

namespace noteAppC {

/// Builds and maintains the Index: a parallel cold scan across the thread
/// pool, then incremental updates driven by a file-system watcher.
class Indexer : public QObject {
    Q_OBJECT

public:
    explicit Indexer(Index *index, QObject *parent = nullptr);
    ~Indexer() override;

    /// Opens the cache for `vault` and starts a scan. Files whose mtime and
    /// size already match the cache are skipped, so a warm start is cheap.
    void setVault(const Vault &vault);
    void rebuild();
    void reindex(const QString &path);
    bool isRunning() const;

    /// Files the vault never indexes: the config directory, the trash, and
    /// anything dot-prefixed.
    static bool isIgnored(const QString &relativePath);

signals:
    void progress(int done, int total);
    void finished(int indexed, qint64 milliseconds);

private:
    void scan(const QStringList &paths);
    void onResultsReady(int begin, int end);
    void onScanFinished();
    void rewatch();
    void onDirectoryChanged(const QString &path);

    struct Parsed {
        Metadata metadata;
        QString body;
        bool ok = false;
    };
    static Parsed parseFile(const QString &path);

    Index *m_index = nullptr;
    Vault m_vault;
    QFutureWatcher<Parsed> m_watcher;
    QFileSystemWatcher m_files;
    QTimer m_debounce;
    QElapsedTimer m_clock;
    int m_indexed = 0;
    int m_total = 0;
};

} // namespace noteAppC
