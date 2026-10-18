#include "Indexer.h"

#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QFile>
#include <QSet>
#include <QStringDecoder>
#include <QtConcurrent>

#include "Parser.h"

namespace noteAppC {

namespace {

/// Index::metaNotesKey() for these options.
QString metaNotesKey(const ParseOptions &options)
{
    return options.metaNotes ? options.open + QLatin1Char('\n') + options.close : QString();
}

/// One parser per worker thread: md4qt parsers are cheap but not shareable.
Parser &threadParser()
{
    static thread_local Parser parser;
    return parser;
}

/// Vault::read() shows each byte of a note that is not valid UTF-8 as U+FFFD,
/// which no search for the note's own words can match. For the index, give each
/// such byte the Latin-1 letter it most likely was (a Latin-1 export's "caf\xE9"
/// is "caf\u00E9"); the note on disk and in the editor stays as it is.
/// ponytail: Latin-1, not Windows-1252, so 0x80-0x9F are control characters;
/// and only when each U+FFFD stands for exactly one bad byte (Qt may show a cut
/// multi-byte sequence as one), else the text is left as it was, so the offsets
/// the index keeps still match what the editor shows.
void readInvalidBytesAsLatin1(const QString &path, Note &note)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return;
    const QByteArray bytes = file.readAll();
    QString units; // one character per U+FFFD the decode of `bytes` produced
    for (qsizetype i = note.bom ? 3 : 0; i < bytes.size();) {
        const uchar lead = uchar(bytes.at(i));
        const qsizetype length = lead < 0x80 ? 1 : lead >= 0xC2 && lead <= 0xDF ? 2 : lead >= 0xE0 && lead <= 0xEF ? 3
                                                                                : lead >= 0xF0 && lead <= 0xF4 ? 4 : 0;
        QStringDecoder decoder(QStringDecoder::Utf8, QStringDecoder::Flag::Stateless);
        const QString piece = length > 0 && i + length <= bytes.size() ? decoder(QByteArrayView(bytes).sliced(i, length))
                                                                        : QString();
        if (length > 0 && !piece.isEmpty() && !decoder.hasError()) {
            if (piece == QChar(0xFFFD)) // really in the file
                units += piece;
            i += length;
        } else {
            units += QChar(lead); // Latin-1: the byte is the code point
            ++i;
        }
    }
    if (note.text.count(QChar(0xFFFD)) != units.size())
        return;
    qsizetype next = 0;
    for (QChar &c : note.text) {
        if (c.unicode() == 0xFFFD)
            c = units.at(next++);
    }
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
    m_poll.setInterval(30'000);
    connect(&m_poll, &QTimer::timeout, this, &Indexer::rebuild);
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
    m_forceRescan = false;
    m_clearMetaNotes = false; // the key still says what the rows are: setVault() compares it
    m_pending.clear();
}

void Indexer::setVault(const Vault &vault)
{
    stop();
    m_vault = vault;
    if (!m_files.directories().isEmpty())
        m_files.removePaths(m_files.directories());
    // The scan's walk hands its directories to watch(), so the UI thread does
    // not walk the vault a second time (6.7 ms on 20 000 notes, every window).
    if (!m_vault.isValid() || !m_index->open(m_vault.root())) {
        rewatch();
        return;
    }
    if (m_index->metaNotesKey() == metaNotesKey(m_parseOptions)) {
        rebuild();
    } else if (!m_parseOptions.metaNotes) {
        clearMetaNotes();
        rebuild();
    } else {
        start({}, true); // the cache's runs are by other delimiters, or none: every note again
    }
}

void Indexer::setNotebookReader(NotebookReader reader)
{
    const bool changed = bool(reader) != bool(m_notebookReader);
    m_notebookReader = std::move(reader);
    if (changed && m_vault.isValid())
        rebuild(); // every notebook's row is of the other kind now, which the scan sees
}

void Indexer::setMetaNotes(const ParseOptions &options)
{
    ParseOptions next;
    next.metaNotes = options.metaNotes;
    if (next.metaNotes) {
        next.open = options.open;
        next.close = options.close;
    }
    if (metaNotesKey(next) == metaNotesKey(m_parseOptions))
        return;
    m_parseOptions = next;
    if (!m_vault.isValid() || !m_index->isOpen())
        return;
    if (next.metaNotes) {
        m_clearMetaNotes = false;
        start({}, true);
    } else if (m_scan.isRunning()) {
        m_clearMetaNotes = true; // its writer holds the database: cleared when it ends
    } else {
        clearMetaNotes();
    }
}

void Indexer::clearMetaNotes()
{
    m_clearMetaNotes = false;
    m_index->clearMetaNotes();
    m_index->setMetaNotesKey({});
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

void Indexer::start(const QStringList &paths, bool force)
{
    if (!m_vault.isValid() || !m_index->isOpen())
        return;
    if (m_scan.isRunning()) {
        if (paths.isEmpty()) {
            m_rescan = true;
            m_forceRescan = m_forceRescan || force;
        } else
            m_pending += paths;
        return;
    }
    m_clock.start();
    m_indexed = 0;
    m_wrote = false;
    m_scanOptions = m_parseOptions;
    m_scanSetsKey = force && paths.isEmpty();
    m_scan = QtConcurrent::run(&m_pool, [this, paths, scan = m_scanNumber.load(), vault = m_vault,
                                         database = m_index->databasePath(), notebooks = m_notebookReader,
                                         options = m_parseOptions, force] {
        runScan(paths, scan, vault, database, notebooks, options, force);
    });
}

void Indexer::runScan(const QStringList &only, quint64 scan, const Vault &vault, const QString &databasePath,
                      const NotebookReader &notebooks, const ParseOptions &options, bool force)
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
        Batch directories;
        directories.scan = scan;
        directories.directories.insert(vault.root(), QFileInfo(vault.root()).lastModified().toMSecsSinceEpoch());
        QDirIterator it(vault.root(), QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
        while (it.hasNext() && !cancelled()) {
            const QFileInfo info = it.nextFileInfo();
            const QString path = info.filePath();
            const QString relative = vault.relative(path);
            if (isIgnored(relative) || vault.isIgnored(relative))
                continue;
            if (info.isDir()) {
                // Its mtime as listed, before its entries are read: anything
                // created in it later shows as a newer mtime in watch().
                directories.directories.insert(path, info.lastModified().toMSecsSinceEpoch());
                continue;
            }
            // A notebook is a note while a reader is set: its cached row is stale in the other case too.
            const bool asNote = Vault::isNote(path)
                                || (notebooks && info.suffix().compare(QLatin1String("ipynb"), Qt::CaseInsensitive) == 0);
            if (!force && writer.isCurrent(path, info.lastModified().toMSecsSinceEpoch(), info.size(), asNote))
                continue;
            (asNote ? notes : attachments) << path; // attachments so ![[image.png]] resolves
        }
        if (!cancelled())
            post(directories);
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
    const auto parse = [&notes, &notebooks, &options](qsizetype from) {
        return from < notes.size() ? QtConcurrent::mapped(notes.mid(from, kChunk),
                                                          [notebooks, options](const QString &path) {
                                                              return Indexer::parseFile(path, notebooks, options);
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
    if (!batch.directories.isEmpty()) {
        watch(batch.directories);
        return;
    }
    for (const auto &[relative, names] : batch.names)
        m_index->setNames(relative, names);
    m_wrote = m_wrote || !batch.names.isEmpty();
    m_indexed = batch.done;
    if (!batch.last) {
        emit progress(batch.done, batch.total);
        return;
    }
    m_scan.waitForFinished(); // it posted this as its last act
    if (!m_parseOptions.metaNotes && (m_clearMetaNotes || m_scanOptions.metaNotes)) {
        clearMetaNotes(); // switched off while it ran: what it or the cache held is stale
    } else if (m_scanSetsKey) {
        // Every note was parsed by the options it started with. Changed since,
        // another forced pass is queued and sets the key again.
        m_index->setMetaNotesKey(metaNotesKey(m_scanOptions));
    }
    // Once, for the whole scan: every pane refreshes on it. Not at all when
    // the vault was already indexed - open() said so.
    if (m_wrote)
        m_index->notifyChanged();
    emit finished(m_indexed, m_clock.elapsed());
    if (std::exchange(m_rescan, false))
        start({}, std::exchange(m_forceRescan, false));
    else if (!m_pending.isEmpty())
        start(std::exchange(m_pending, {}));
}

Indexer::Parsed Indexer::parseFile(const QString &path, const NotebookReader &notebooks, const ParseOptions &options)
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
    } else if (note.lossy) {
        readInvalidBytesAsLatin1(path, note);
    }

    const QFileInfo info(path);
    parsed.metadata = threadParser().parse(note.text, path, options);
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
    m_poll.stop(); // a new vault: its own watches decide
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
    addWatches(directories);
}

void Indexer::addWatches(const QStringList &directories)
{
    // A folder gone since the walk is no loss; one that is there and still
    // has no watch would never be noticed changing, so poll instead.
    int failed = 0;
    for (const QString &path : m_files.addPaths(directories)) {
        if (QFileInfo(path).isDir())
            ++failed;
    }
    if (failed > 0 && !m_poll.isActive()) {
        m_poll.start();
        emit watchesFailed(failed);
    }
}

void Indexer::watch(const QHash<QString, qint64> &directories)
{
    // Only the difference: a save rescans the vault, and re-adding every watch
    // each time would cost what the walk saved.
    const QStringList watchedList = m_files.directories();
    const QSet<QString> watched(watchedList.cbegin(), watchedList.cend());
    QStringList gone;
    for (const QString &path : watchedList) {
        if (!directories.contains(path))
            gone << path;
    }
    if (!gone.isEmpty())
        m_files.removePaths(gone);
    QStringList added;
    for (auto it = directories.cbegin(); it != directories.cend(); ++it) {
        if (!watched.contains(it.key()))
            added << it.key();
    }
    // ponytail: one inotify watch per directory, which tops out around 8k.
    if (!added.isEmpty())
        addWatches(added);
    // Between the walk reading a directory and its watch starting, a file can
    // arrive unseen; its directory's mtime moved, so walk again.
    for (const QString &path : std::as_const(added)) {
        if (QFileInfo(path).lastModified().toMSecsSinceEpoch() != directories.value(path)) {
            m_debounce.start();
            break;
        }
    }
}

void Indexer::onDirectoryChanged(const QString &)
{
    m_debounce.start();
}

} // namespace noteAppC
