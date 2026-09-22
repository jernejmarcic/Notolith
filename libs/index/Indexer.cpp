#include "Indexer.h"

#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QtConcurrent>

#include "Parser.h"

namespace noteAppC {

namespace {

/// One parser per worker thread: md4qt parsers are cheap but not shareable.
Parser &threadParser()
{
    static thread_local Parser parser;
    return parser;
}

} // namespace

Indexer::Indexer(Index *index, QObject *parent)
    : QObject(parent)
    , m_index(index)
{
    m_debounce.setSingleShot(true);
    m_debounce.setInterval(300); // let a burst of writes settle
    connect(&m_debounce, &QTimer::timeout, this, &Indexer::rebuild);

    connect(&m_watcher, &QFutureWatcher<Parsed>::resultsReadyAt, this, &Indexer::onResultsReady);
    connect(&m_watcher, &QFutureWatcher<Parsed>::finished, this, &Indexer::onScanFinished);
    connect(&m_files, &QFileSystemWatcher::directoryChanged, this, &Indexer::onDirectoryChanged);
    connect(&m_files, &QFileSystemWatcher::fileChanged, this, &Indexer::reindex);
}

Indexer::~Indexer()
{
    if (m_watcher.isRunning()) {
        m_watcher.cancel();
        m_watcher.waitForFinished();
    }
}

bool Indexer::isIgnored(const QString &relativePath)
{
    for (const QStringView part : QStringView(relativePath).split(QLatin1Char('/'))) {
        if (part.startsWith(QLatin1Char('.')))
            return true;
    }
    return false;
}

bool Indexer::isRunning() const
{
    return m_watcher.isRunning();
}

void Indexer::setVault(const Vault &vault)
{
    m_vault = vault;
    if (!m_vault.isValid())
        return;
    if (!m_index->open(m_vault.root()))
        return;
    rewatch();
    rebuild();
}

void Indexer::rebuild()
{
    if (!m_vault.isValid() || m_watcher.isRunning())
        return;

    QStringList paths;
    QStringList attachments;
    QDirIterator it(m_vault.root(), QDir::Files | QDir::NoDotAndDotDot,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString path = it.next();
        const QString relative = m_vault.relative(path);
        if (isIgnored(relative) || m_vault.isIgnored(relative))
            continue;
        const QFileInfo info(path);
        if (m_index->isCurrent(path, info.lastModified().toMSecsSinceEpoch(), info.size()))
            continue;
        if (Vault::isNote(path))
            paths << path;
        else
            attachments << path; // so ![[image.png]] resolves
    }

    if (!attachments.isEmpty()) {
        m_index->beginBatch();
        for (const QString &path : std::as_const(attachments)) {
            const QFileInfo info(path);
            m_index->putAttachment(path, info.lastModified().toMSecsSinceEpoch(), info.size());
        }
        m_index->endBatch();
    }

    // Anything the cache holds that is no longer on disk.
    for (const QString &known : m_index->allPaths()) {
        if (!QFileInfo::exists(m_vault.absolute(known)))
            m_index->remove(m_vault.absolute(known));
    }

    scan(paths);
}

void Indexer::reindex(const QString &path)
{
    if (!m_vault.isValid() || !m_vault.contains(path) || !Vault::isNote(path))
        return;
    if (m_watcher.isRunning()) {
        m_debounce.start();
        return;
    }
    scan({path});
}

void Indexer::scan(const QStringList &paths)
{
    m_clock.start();
    m_indexed = 0;
    m_total = int(paths.size());

    if (paths.isEmpty()) {
        // Queued, so "nothing to do" still reaches a caller that connects
        // right before asking for the scan.
        QMetaObject::invokeMethod(this, [this] { emit finished(0, 0); }, Qt::QueuedConnection);
        return;
    }

    m_index->beginBatch();
    m_watcher.setFuture(QtConcurrent::mapped(paths, &Indexer::parseFile));
}

Indexer::Parsed Indexer::parseFile(const QString &path)
{
    Parsed parsed;
    Note note;
    if (!Vault::read(path, note))
        return parsed;

    const QFileInfo info(path);
    parsed.metadata = threadParser().parse(note.text, path);
    parsed.metadata.mtime = info.lastModified().toMSecsSinceEpoch();
    parsed.metadata.size = info.size();
    parsed.metadata.hash =
        QCryptographicHash::hash(note.text.toUtf8(), QCryptographicHash::Sha1);
    parsed.body = note.text;
    parsed.ok = true;
    return parsed;
}

void Indexer::onResultsReady(int begin, int end)
{
    for (int i = begin; i < end; ++i) {
        const Parsed &parsed = m_watcher.resultAt(i);
        if (!parsed.ok)
            continue;
        m_index->put(parsed.metadata, parsed.body);
        ++m_indexed;
    }
    emit progress(m_indexed, m_total);
}

void Indexer::onScanFinished()
{
    m_index->endBatch();
    emit finished(m_indexed, m_clock.elapsed());
}

void Indexer::rewatch()
{
    if (!m_files.directories().isEmpty())
        m_files.removePaths(m_files.directories());
    if (!m_vault.isValid())
        return;

    QStringList directories{m_vault.root()};
    QDirIterator it(m_vault.root(), QDir::Dirs | QDir::NoDotAndDotDot,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString path = it.next();
        const QString relative = m_vault.relative(path);
        if (!isIgnored(relative) && !m_vault.isIgnored(relative))
            directories << path;
    }
    // ponytail: one inotify watch per directory, which tops out around 8k.
    // fanotify, or watching only the open folders, if a vault ever hits it.
    m_files.addPaths(directories);
}

void Indexer::onDirectoryChanged(const QString &)
{
    m_debounce.start();
}

} // namespace noteAppC
