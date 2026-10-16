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
    m_pool.setMaxThreadCount(1);
    m_debounce.setSingleShot(true);
    m_debounce.setInterval(300); // let a burst of writes settle
    connect(&m_debounce, &QTimer::timeout, this, &Indexer::rebuild);
    connect(&m_files, &QFileSystemWatcher::directoryChanged, this, &Indexer::onDirectoryChanged);
    connect(&m_files, &QFileSystemWatcher::fileChanged, this, &Indexer::reindex);
}

Indexer::~Indexer()
{
    stop(); // the scan posts to this object, and must not outlive it
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
    return m_scan.isRunning();
}

void Indexer::stop()
{
    ++m_scanNumber; // what it has queued already is dropped on arrival
    m_scan.waitForFinished();
    m_rescan = false;
    m_pending.clear();
}

void Indexer::setVault(const Vault &vault)
{
    stop();
    m_vault = vault;
    rewatch();
    if (!m_vault.isValid() || !m_index->open(m_vault.root()))
        return;
    rebuild();
}

void Indexer::setNotebookReader(NotebookReader reader)
{
    const bool changed = bool(reader) != bool(m_notebookReader);
    m_notebookReader = std::move(reader);
    if (changed && m_vault.isValid())
        rebuild(); // every notebook's row is of the other kind now, which the scan sees
}

bool Indexer::isNotebook(const QString &path) const
{
    return m_notebookReader && QFileInfo(path).suffix().compare(QLatin1String("ipynb"), Qt::CaseInsensitive) == 0;
}

void Indexer::rebuild()
{
    start({});
}

void Indexer::reindex(const QString &path)
{
    if (m_vault.isValid() && m_vault.contains(path) && (Vault::isNote(path) || isNotebook(path)))
        start({path});
}

void Indexer::start(const QStringList &paths)
{
    if (!m_vault.isValid() || !m_index->isOpen())
        return;
    if (m_scan.isRunning()) {
        if (paths.isEmpty())
            m_rescan = true;
        else
            m_pending += paths;
        return;
    }
    m_clock.start();
    m_indexed = 0;
    m_wrote = false;
    m_scan = QtConcurrent::run(&m_pool, [this, paths, scan = m_scanNumber.load(), vault = m_vault,
                                         database = m_index->databasePath(), notebooks = m_notebookReader] {
        runScan(paths, scan, vault, database, notebooks);
    });
}

void Indexer::runScan(const QStringList &only, quint64 scan, const Vault &vault, const QString &databasePath,
                      const NotebookReader &notebooks)
{
    // On the scan's thread: nothing here touches m_index, and what the
    // Index's thread must know goes to it in a Batch.
    const auto post = [this](const Batch &batch) {
        QMetaObject::invokeMethod(this, [this, batch] { apply(batch); }, Qt::QueuedConnection);
    };
    const auto cancelled = [this, scan] { return m_scanNumber.load() != scan; };
    Batch batch;
    batch.scan = scan;
    Index::Writer writer(databasePath, vault.root());
    if (!writer.isOpen()) {
        batch.last = true;
        post(batch);
        return;
    }

    QStringList notes = only;
    if (only.isEmpty()) {
        QStringList attachments;
        QDirIterator it(vault.root(), QDir::Files | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
        while (it.hasNext() && !cancelled()) {
            const QFileInfo info = it.nextFileInfo();
            const QString path = info.filePath();
            const QString relative = vault.relative(path);
            if (isIgnored(relative) || vault.isIgnored(relative))
                continue;
            // A notebook is a note while a reader is set: its cached row is stale in the other case too.
            const bool asNote = Vault::isNote(path)
                                || (notebooks && info.suffix().compare(QLatin1String("ipynb"), Qt::CaseInsensitive) == 0);
            if (writer.isCurrent(path, info.lastModified().toMSecsSinceEpoch(), info.size(), asNote))
                continue;
            (asNote ? notes : attachments) << path; // attachments so ![[image.png]] resolves
        }
        writer.begin();
        for (const QString &path : std::as_const(attachments)) {
            const QFileInfo info(path);
            batch.names.append({vault.relative(path),
                                writer.putAttachment(path, info.lastModified().toMSecsSinceEpoch(), info.size())});
        }
        // Anything the cache holds that is no longer on disk.
        for (const QString &known : writer.allPaths()) {
            if (!QFileInfo::exists(vault.absolute(known))) {
                writer.remove(vault.absolute(known));
                batch.names.append({known, {}});
            }
        }
        writer.commit();
    }
    batch.total = int(notes.size());

    // Parsed across the pool a chunk at a time, the next chunk parsing while
    // this one is written: the one writer is the slower half.
    constexpr qsizetype kChunk = 256;
    const auto parse = [&notes, &notebooks](qsizetype from) {
        return from < notes.size() ? QtConcurrent::mapped(notes.mid(from, kChunk),
                                                          [notebooks](const QString &path) {
                                                              return Indexer::parseFile(path, notebooks);
                                                          })
                                   : QFuture<Parsed>();
    };
    QFuture<Parsed> next = parse(0);
    for (qsizetype from = 0; from < notes.size() && !cancelled(); from += kChunk) {
        QFuture<Parsed> current = next;
        next = parse(from + kChunk);
        writer.begin();
        for (const Parsed &parsed : current.results()) {
            if (!parsed.ok)
                continue;
            batch.names.append({vault.relative(parsed.metadata.path), writer.put(parsed.metadata, parsed.body)});
            ++batch.done;
        }
        writer.commit();
        post(batch);
        batch.names.clear();
    }
    next.cancel();
    next.waitForFinished();
    if (cancelled())
        return;
    batch.last = true;
    post(batch);
}

void Indexer::apply(const Batch &batch)
{
    if (batch.scan != m_scanNumber.load())
        return; // from a scan stop() cancelled
    for (const auto &[relative, names] : batch.names)
        m_index->setNames(relative, names);
    m_wrote = m_wrote || !batch.names.isEmpty();
    m_indexed = batch.done;
    if (!batch.last) {
        emit progress(batch.done, batch.total);
        return;
    }
    m_scan.waitForFinished(); // it posted this as its last act
    // Once, for the whole scan: every pane refreshes on it. Not at all when
    // the vault was already indexed - open() said so.
    if (m_wrote)
        m_index->notifyChanged();
    emit finished(m_indexed, m_clock.elapsed());
    if (std::exchange(m_rescan, false))
        rebuild();
    else if (!m_pending.isEmpty())
        start(std::exchange(m_pending, {}));
}

Indexer::Parsed Indexer::parseFile(const QString &path, const NotebookReader &notebooks)
{
    Parsed parsed;
    Note note;
    if (notebooks && QFileInfo(path).suffix().compare(QLatin1String("ipynb"), Qt::CaseInsensitive) == 0) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly))
            return parsed;
        const std::optional<QString> text = notebooks(file.readAll());
        if (!text)
            return parsed; // not a notebook nbformat 4 can read: not indexed
        note.text = *text;
    } else if (!Vault::read(path, note)) {
        return parsed;
    }

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
