#include "Index.h"

#include <algorithm>
#include <future>
#include <map>
#include <optional>

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QUrl>
#include <QUuid>
#include <QVariant>

#include "Frontmatter.h"
#include "ObsidianSyntax.h"
#include "Parser.h"
#include "Vault.h"

namespace noteAppC {

namespace {
/// Bump when the schema changes, or when the parser reads a note differently
/// (4: a `:::` fence's attributes are no longer tags; 5: links inside
/// footnotes are read; 7: a task keeps the character in its box, not done or
/// not). The cache is derived
/// from the vault, so a mismatch drops it and rebuilds rather than migrating:
/// a wrong migration is a silently wrong index, and a rebuild costs one pass
/// over the notes.
constexpr int kSchemaVersion = 7;
} // namespace

namespace {

/// Obsidian normalises a link target before resolving: no extension, no
/// percent escapes, no surrounding space, case folded for comparison.
QString normaliseTarget(const QString &target)
{
    QString value = QUrl::fromPercentEncoding(target.trimmed().toUtf8());
    if (value.endsWith(QLatin1String(".md"), Qt::CaseInsensitive))
        value.chop(3);
    while (value.startsWith(QLatin1String("./")))
        value.remove(0, 2);
    return value;
}

QString foldCase(const QString &value)
{
    return value.toCaseFolded();
}

/// The one thing every target that resolves to a given note has in common: its
/// last path component, folded, without the extension. Obsidian resolves a
/// target by name, or by a path whose last component is that name, so nothing
/// whose last component differs can ever resolve to it - which is what lets
/// backlinks() ask the index a question instead of reading the whole table.
QString targetName(const QString &target)
{
    const QString value = normaliseTarget(target);
    const qsizetype slash = value.lastIndexOf(QLatin1Char('/'));
    return foldCase(slash < 0 ? value : value.mid(slash + 1));
}

/// Every name a note answers to: its link names, and its vault-relative path
/// with and without ".md", since a link may also spell those.
QStringList noteNames(const QString &relative, const Metadata &metadata)
{
    // A notebook (or an ink note) is not Markdown: Obsidian names such a file by its extension,
    // so `[[Analysis]]` still means Analysis.md and `[[Analysis.ipynb]]` the notebook.
    if (relative.endsWith(QLatin1String(".ipynb"), Qt::CaseInsensitive) || relative.endsWith(QLatin1String(".ink"), Qt::CaseInsensitive))
        return {QFileInfo(relative).fileName(), relative};
    QStringList names = metadata.linkNames();
    names << relative;
    if (relative.endsWith(QLatin1String(".md")))
        names << relative.chopped(3);
    names.removeDuplicates();
    return names;
}

/// An attachment answers to its file name and to its path, both with the
/// extension, which is how Obsidian resolves `![[image.png]]`.
QStringList attachmentNames(const QString &relative)
{
    QStringList names{QFileInfo(relative).fileName(), relative};
    names.removeDuplicates();
    return names;
}

/// Every statement one file's rows need, prepared once on one connection:
/// the indexer's Writer keeps one for a whole scan, put() makes one for a
/// note. Preparing each row's statement anew was most of a cold index's
/// write time (Qt rewrites the placeholders on every prepare).
class NoteRows {
public:
    explicit NoteRows(const QSqlDatabase &db)
        : m_findFile(db)
        , m_deleteContent(db)
        , m_deleteFile(db)
        , m_insertFile(db)
        , m_insertHeading(db)
        , m_insertLink(db)
        , m_insertTag(db)
        , m_insertBlock(db)
        , m_insertTask(db)
        , m_insertMetaNote(db)
        , m_insertProperty(db)
        , m_insertName(db)
        , m_insertContent(db)
    {
        m_findFile.prepare(QStringLiteral("SELECT id FROM files WHERE path = ?"));
        m_deleteContent.prepare(QStringLiteral("DELETE FROM content WHERE rowid = ?"));
        m_deleteFile.prepare(QStringLiteral("DELETE FROM files WHERE id = ?"));
        m_insertFile.prepare(
            QStringLiteral("INSERT INTO files(path, mtime, size, hash, words) VALUES(?, ?, ?, ?, ?)"));
        m_insertHeading.prepare(QStringLiteral(
            "INSERT INTO headings(file, level, text, start, finish) VALUES(?, ?, ?, ?, ?)"));
        m_insertLink.prepare(QStringLiteral(
            "INSERT INTO links(file, target, tname, subpath, alias, embed, wiki, external, start, "
            "finish, tstart, tfinish) VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)"));
        m_insertTag.prepare(QStringLiteral("INSERT INTO tags(file, name, start, finish) VALUES(?, ?, ?, ?)"));
        m_insertBlock.prepare(
            QStringLiteral("INSERT INTO blocks(file, block, start, finish) VALUES(?, ?, ?, ?)"));
        m_insertTask.prepare(
            QStringLiteral("INSERT INTO tasks(file, state, text, start, finish) VALUES(?, ?, ?, ?, ?)"));
        m_insertMetaNote.prepare(
            QStringLiteral("INSERT INTO meta_notes(file, start, finish) VALUES(?, ?, ?)"));
        m_insertProperty.prepare(
            QStringLiteral("INSERT INTO props(file, key, type, value) VALUES(?, ?, ?, ?)"));
        m_insertName.prepare(QStringLiteral("INSERT INTO names(file, name, folded) VALUES(?, ?, ?)"));
        m_insertContent.prepare(QStringLiteral("INSERT INTO content(rowid, path, body) VALUES(?, ?, ?)"));
    }

    /// Drops what the cache holds for `relative`. One indexed lookup first:
    /// on a cold build nothing is cached yet, and every note would otherwise
    /// pay for two deletes that match no rows.
    void erase(const QString &relative)
    {
        m_findFile.bindValue(0, relative);
        if (!m_findFile.exec() || !m_findFile.next())
            return;
        const qint64 fileID = m_findFile.value(0).toLongLong();
        m_findFile.finish();
        run(m_deleteContent, {fileID});
        run(m_deleteFile, {fileID}); // the rest cascade
    }

    bool insertNote(const QString &relative, const Metadata &metadata, const QString &body,
                    const QStringList &names, QString *error)
    {
        const qint64 fileID =
            insertFile({relative, metadata.mtime, metadata.size, metadata.hash, metadata.wordCount}, error);
        if (fileID < 0)
            return false;
        for (const Heading &heading : metadata.headings)
            run(m_insertHeading, {fileID, heading.level, heading.text, heading.range.start, heading.range.end});
        for (const Link &link : metadata.links) {
            run(m_insertLink, {fileID, link.target, link.external ? QString() : targetName(link.target),
                               link.subpath, link.alias, link.embed, link.wiki, link.external,
                               link.range.start, link.range.end, link.targetRange.start, link.targetRange.end});
        }
        for (const Tag &tag : metadata.tags)
            run(m_insertTag, {fileID, tag.name, tag.range.start, tag.range.end});
        for (const BlockId &block : metadata.blockIds)
            run(m_insertBlock, {fileID, block.id, block.range.start, block.range.end});
        for (const Task &task : metadata.tasks)
            run(m_insertTask, {fileID, QString(task.state), task.text, task.range.start, task.range.end});
        for (const Range &metaNote : metadata.metaNotes)
            run(m_insertMetaNote, {fileID, metaNote.start, metaNote.end});
        for (const Property &property : metadata.properties) {
            run(m_insertProperty,
                {fileID, property.key, int(property.type), property.values.join(QLatin1Char('\n'))});
        }
        insertNames(fileID, names);
        // The rowid is the file's id, so a delete is a rowid lookup rather
        // than a scan of the whole full-text table. That scan was quadratic:
        // 20 000 notes took 39.8 s to index, against a 5 s budget.
        run(m_insertContent, {fileID, relative, body});
        return true;
    }

    bool insertAttachment(const QString &relative, qint64 mtime, qint64 size, const QStringList &names,
                          QString *error)
    {
        // No hash and words -1: what tells an attachment from a note.
        const qint64 fileID = insertFile({relative, mtime, size, QVariant(), -1}, error);
        if (fileID < 0)
            return false;
        insertNames(fileID, names);
        return true;
    }

private:
    static bool run(QSqlQuery &query, const QVariantList &values)
    {
        for (int i = 0; i < values.size(); ++i)
            query.bindValue(i, values.at(i));
        return query.exec();
    }

    qint64 insertFile(const QVariantList &values, QString *error)
    {
        if (!run(m_insertFile, values)) {
            if (error)
                *error = m_insertFile.lastError().text();
            return -1;
        }
        return m_insertFile.lastInsertId().toLongLong();
    }

    void insertNames(qint64 fileID, const QStringList &names)
    {
        for (const QString &name : names) {
            if (!name.isEmpty())
                run(m_insertName, {fileID, name, foldCase(name)});
        }
    }

    QSqlQuery m_findFile;
    QSqlQuery m_deleteContent;
    QSqlQuery m_deleteFile;
    QSqlQuery m_insertFile;
    QSqlQuery m_insertHeading;
    QSqlQuery m_insertLink;
    QSqlQuery m_insertTag;
    QSqlQuery m_insertBlock;
    QSqlQuery m_insertTask;
    QSqlQuery m_insertMetaNote;
    QSqlQuery m_insertProperty;
    QSqlQuery m_insertName;
    QSqlQuery m_insertContent;
};

} // namespace

Index::Index(QObject *parent)
    : QObject(parent)
    , m_connectionName(QStringLiteral("noteAppC-index-%1")
                           .arg(QUuid::createUuid().toString(QUuid::WithoutBraces)))
{
}

Index::~Index()
{
    close();
}

QString Index::cachePathFor(const QString &vaultRoot)
{
    const QString root = QDir(vaultRoot).absolutePath();
    const QByteArray digest =
        QCryptographicHash::hash(root.toUtf8(), QCryptographicHash::Sha256).toHex().left(16);
    const QString base = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    return QStringLiteral("%1/vaults/%2/index.db").arg(base, QString::fromLatin1(digest));
}

bool Index::open(const QString &vaultRoot)
{
    close();

    m_vaultRoot = QDir(vaultRoot).absolutePath();
    m_databasePath = cachePathFor(m_vaultRoot);
    if (!QDir().mkpath(QFileInfo(m_databasePath).absolutePath())) {
        m_error = QStringLiteral("cannot create %1").arg(QFileInfo(m_databasePath).absolutePath());
        return false;
    }

    // A cache written by another schema version is deleted, not migrated:
    // it is rebuildable from the vault at any time, and dropping its tables
    // in place failed silently (a DROP while the version query's statement
    // was still open is "table is locked"), after which createSchema() tripped
    // over the old columns and every backlink query ran against a table
    // without them - for good, since the indexer never ran either.
    if (QFileInfo::exists(m_databasePath) && storedVersion(m_databasePath) != kSchemaVersion) {
        for (const QString &suffix : {QString(), QStringLiteral("-wal"), QStringLiteral("-shm")})
            QFile::remove(m_databasePath + suffix);
    }

    m_db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_connectionName);
    m_db.setDatabaseName(m_databasePath);
    if (!m_db.open()) {
        m_error = m_db.lastError().text();
        close();
        return false;
    }

    QSqlQuery pragma(m_db);
    // WAL so a reader never blocks the indexer; NORMAL because the cache is
    // rebuildable from the vault at any time.
    pragma.exec(QStringLiteral("PRAGMA journal_mode = WAL"));
    pragma.exec(QStringLiteral("PRAGMA synchronous = NORMAL"));
    pragma.exec(QStringLiteral("PRAGMA foreign_keys = ON"));

    if (!createSchema()) {
        close(); // isOpen() must not say yes to a database nothing can use
        return false;
    }
    pragma.exec(QStringLiteral("PRAGMA user_version = %1").arg(kSchemaVersion));

    loadNames();
    // A different database is changed contents to anyone reading this one:
    // when the vault is already indexed, the indexer writes nothing and this
    // is the only notice a pane gets (the local graph stayed empty).
    emit changed();
    return true;
}

void Index::close()
{
    if (m_db.isOpen())
        m_db.close();
    if (QSqlDatabase::contains(m_connectionName)) {
        m_db = QSqlDatabase();
        QSqlDatabase::removeDatabase(m_connectionName);
    }
    m_names.clear();
    m_pathNames.clear();
    m_nameCounts.clear();
    m_linkTargetsValid = m_aliasesValid = false;
}

bool Index::isOpen() const
{
    return m_db.isOpen();
}

bool Index::createSchema()
{
    static const QStringList statements = {
        QStringLiteral("CREATE TABLE IF NOT EXISTS files ("
                       "id INTEGER PRIMARY KEY, path TEXT UNIQUE NOT NULL, "
                       "mtime INTEGER, size INTEGER, hash BLOB, words INTEGER)"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS headings ("
                       "file INTEGER NOT NULL REFERENCES files(id) ON DELETE CASCADE, "
                       "level INTEGER, text TEXT, start INTEGER, finish INTEGER)"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS links ("
                       "file INTEGER NOT NULL REFERENCES files(id) ON DELETE CASCADE, "
                       "target TEXT, tname TEXT, subpath TEXT, alias TEXT, embed INTEGER, "
                       "wiki INTEGER, external INTEGER, start INTEGER, finish INTEGER, "
                       "tstart INTEGER, tfinish INTEGER)"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS tags ("
                       "file INTEGER NOT NULL REFERENCES files(id) ON DELETE CASCADE, "
                       "name TEXT, start INTEGER, finish INTEGER)"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS blocks ("
                       "file INTEGER NOT NULL REFERENCES files(id) ON DELETE CASCADE, "
                       "block TEXT, start INTEGER, finish INTEGER)"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS tasks ("
                       "file INTEGER NOT NULL REFERENCES files(id) ON DELETE CASCADE, "
                       "state TEXT, text TEXT, start INTEGER, finish INTEGER)"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS props ("
                       "file INTEGER NOT NULL REFERENCES files(id) ON DELETE CASCADE, "
                       "key TEXT, type INTEGER, value TEXT)"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS meta_notes ("
                       "file INTEGER NOT NULL REFERENCES files(id) ON DELETE CASCADE, "
                       "start INTEGER, finish INTEGER)"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS state (key TEXT PRIMARY KEY, value TEXT)"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS names ("
                       "file INTEGER NOT NULL REFERENCES files(id) ON DELETE CASCADE, "
                       "name TEXT, folded TEXT)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS links_target ON links(target)"),
        // Backlinks are looked up by the target's last component, never by the
        // whole spelling, so that is what has to be indexed.
        QStringLiteral("CREATE INDEX IF NOT EXISTS links_tname ON links(tname)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS tags_name ON tags(name)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS names_folded ON names(folded)"),
        QStringLiteral("CREATE VIRTUAL TABLE IF NOT EXISTS content USING fts5("
                       "path UNINDEXED, body, "
                       "tokenize = 'unicode61 remove_diacritics 2')"),
    };

    QSqlQuery query(m_db);
    for (const QString &statement : statements) {
        if (!query.exec(statement)) {
            m_error = query.lastError().text();
            return false;
        }
    }

    return true;
}

int Index::storedVersion(const QString &databasePath)
{
    const QString name = QStringLiteral("noteAppC-version-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    int version = 0;
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name);
        db.setDatabaseName(databasePath);
        if (db.open()) {
            QSqlQuery query(db);
            if (query.exec(QStringLiteral("PRAGMA user_version")) && query.next())
                version = query.value(0).toInt();
        }
    }
    QSqlDatabase::removeDatabase(name);
    return version;
}

void Index::loadNames()
{
    m_names.clear();
    m_pathNames.clear();
    m_nameCounts.clear();
    m_linkTargetsValid = m_aliasesValid = false;

    QSqlQuery query(m_db);
    query.exec(QStringLiteral("SELECT files.path, names.name FROM names "
                              "JOIN files ON files.id = names.file"));
    while (query.next()) {
        const QString path = query.value(0).toString();
        const QString name = query.value(1).toString();
        m_names[foldCase(name)].append(path);
        m_pathNames[path].append(name);
        ++m_nameCounts[name];
    }
}

bool Index::isCurrent(const QString &path, qint64 mtime, qint64 size) const
{
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral("SELECT mtime, size FROM files WHERE path = ?"));
    query.addBindValue(QDir(m_vaultRoot).relativeFilePath(path));
    if (!query.exec() || !query.next())
        return false;
    return query.value(0).toLongLong() == mtime && query.value(1).toLongLong() == size;
}

void Index::forgetNames(const QString &path)
{
    for (const QString &name : std::as_const(m_pathNames[path])) {
        const auto counted = m_nameCounts.find(name);
        if (counted != m_nameCounts.end() && --counted->second == 0)
            m_nameCounts.erase(counted);
        auto it = m_names.find(foldCase(name));
        if (it != m_names.end()) {
            it->removeAll(path);
            if (it->isEmpty())
                m_names.erase(it);
        }
    }
    m_pathNames.remove(path);
}

void Index::rememberNames(const QString &path, const QStringList &names)
{
    for (const QString &name : names) {
        if (name.isEmpty())
            continue;
        m_names[foldCase(name)].append(path);
        m_pathNames[path].append(name);
        ++m_nameCounts[name];
    }
}

void Index::setNames(const QString &relative, const QStringList &names)
{
    const QStringList before = m_pathNames.value(relative);
    forgetNames(relative);
    rememberNames(relative, names);
    // A save that leaves the note's names alone leaves both lists alone.
    if (m_pathNames.value(relative) != before)
        m_linkTargetsValid = m_aliasesValid = false;
}

void Index::beginBatch()
{
    if (!m_inBatch && m_db.isOpen()) {
        m_db.transaction();
        m_inBatch = true;
    }
}

void Index::endBatch()
{
    if (m_inBatch) {
        m_db.commit();
        m_inBatch = false;
        emit changed();
    }
}

void Index::put(const Metadata &metadata, const QString &body)
{
    if (!m_db.isOpen())
        return;
    const QString path = QDir(m_vaultRoot).relativeFilePath(metadata.path);
    NoteRows rows(m_db);
    rows.erase(path);
    const QStringList before = m_pathNames.value(path);
    forgetNames(path);
    const QStringList names = noteNames(path, metadata);
    if (!rows.insertNote(path, metadata, body, names, &m_error)) {
        m_linkTargetsValid = m_aliasesValid = false;
        return;
    }
    rememberNames(path, names);
    if (m_pathNames.value(path) != before)
        m_linkTargetsValid = m_aliasesValid = false;
    if (!m_inBatch)
        emit changed();
}

void Index::clearMetaNotes()
{
    if (!m_db.isOpen())
        return;
    QSqlQuery(m_db).exec(QStringLiteral("DELETE FROM meta_notes"));
    notifyChanged();
}

QString Index::metaNotesKey() const
{
    if (!m_db.isOpen())
        return {};
    QSqlQuery query(m_db);
    query.exec(QStringLiteral("SELECT value FROM state WHERE key = 'metaNotes'"));
    return query.next() ? query.value(0).toString() : QString();
}

ParseOptions Index::parseOptions() const
{
    ParseOptions options;
    QString key = metaNotesKey();
    // Indexer's key: the meta-note delimiters, then "\ntasks" for task states.
    const QString tasks = QStringLiteral("\ntasks");
    options.taskStates = key.endsWith(tasks);
    if (options.taskStates)
        key.chop(tasks.size());
    const qsizetype split = key.indexOf(QLatin1Char('\n'));
    if (split > 0) {
        options.metaNotes = true;
        options.open = key.left(split);
        options.close = key.mid(split + 1);
    }
    return options;
}

void Index::setMetaNotesKey(const QString &key)
{
    if (!m_db.isOpen())
        return;
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral("INSERT OR REPLACE INTO state(key, value) VALUES('metaNotes', ?)"));
    query.addBindValue(key);
    query.exec();
}

void Index::remove(const QString &path)
{
    if (!m_db.isOpen())
        return;
    const QString relative = QDir(m_vaultRoot).relativeFilePath(path);
    NoteRows(m_db).erase(relative);
    forgetNames(relative);
    m_linkTargetsValid = m_aliasesValid = false;
}

struct Index::Writer::Private {
    QString connection;
    QString vaultRoot;
    QSqlDatabase db;
    std::optional<NoteRows> rows;
    QSqlQuery current;

    QString relative(const QString &path) const { return QDir(vaultRoot).relativeFilePath(path); }
};

Index::Writer::Writer(const QString &databasePath, const QString &vaultRoot)
    : d(std::make_unique<Private>())
{
    d->vaultRoot = vaultRoot;
    d->connection =
        QStringLiteral("noteAppC-writer-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    d->db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), d->connection);
    d->db.setDatabaseName(databasePath);
    if (!d->db.open())
        return;
    // WAL is the file's own, set by open(); these two are the connection's.
    QSqlQuery pragma(d->db);
    pragma.exec(QStringLiteral("PRAGMA synchronous = NORMAL"));
    pragma.exec(QStringLiteral("PRAGMA foreign_keys = ON"));
    d->rows.emplace(d->db);
    d->current = QSqlQuery(d->db);
    d->current.prepare(QStringLiteral("SELECT mtime, size, words FROM files WHERE path = ?"));
}

Index::Writer::~Writer()
{
    d->rows.reset();
    d->current = QSqlQuery();
    d->db.close();
    d->db = QSqlDatabase();
    QSqlDatabase::removeDatabase(d->connection);
}

bool Index::Writer::isOpen() const
{
    return d->db.isOpen();
}

bool Index::Writer::isCurrent(const QString &path, qint64 mtime, qint64 size, bool asNote)
{
    d->current.bindValue(0, d->relative(path));
    const bool same = d->current.exec() && d->current.next() && d->current.value(0).toLongLong() == mtime
                   && d->current.value(1).toLongLong() == size && (d->current.value(2).toLongLong() >= 0) == asNote;
    d->current.finish();
    return same;
}

QStringList Index::Writer::allPaths()
{
    QStringList paths;
    QSqlQuery query(d->db);
    query.exec(QStringLiteral("SELECT path FROM files WHERE words >= 0 ORDER BY path"));
    while (query.next())
        paths << query.value(0).toString();
    return paths;
}

void Index::Writer::begin()
{
    d->db.transaction();
}

void Index::Writer::commit()
{
    d->db.commit();
}

QStringList Index::Writer::put(const Metadata &metadata, const QString &body)
{
    const QString relative = d->relative(metadata.path);
    d->rows->erase(relative);
    const QStringList names = noteNames(relative, metadata);
    return d->rows->insertNote(relative, metadata, body, names, nullptr) ? names : QStringList();
}

QStringList Index::Writer::putAttachment(const QString &path, qint64 mtime, qint64 size)
{
    const QString relative = d->relative(path);
    d->rows->erase(relative);
    const QStringList names = attachmentNames(relative);
    return d->rows->insertAttachment(relative, mtime, size, names, nullptr) ? names : QStringList();
}

void Index::Writer::remove(const QString &path)
{
    d->rows->erase(d->relative(path));
}

int Index::contentRowCount() const
{
    QSqlQuery query(m_db);
    query.exec(QStringLiteral("SELECT COUNT(*) FROM content"));
    return query.next() ? query.value(0).toInt() : 0;
}

int Index::noteCount() const
{
    QSqlQuery query(m_db);
    // Attachments live in the same table (so links resolve) but are not notes.
    query.exec(QStringLiteral("SELECT COUNT(*) FROM files WHERE words >= 0"));
    return query.next() ? query.value(0).toInt() : 0;
}

int Index::attachmentCount() const
{
    QSqlQuery query(m_db);
    query.exec(QStringLiteral("SELECT COUNT(*) FROM files WHERE words < 0"));
    return query.next() ? query.value(0).toInt() : 0;
}

QStringList Index::allPaths() const
{
    QStringList paths;
    QSqlQuery query(m_db);
    // Notes only: attachments are in `files` so links resolve, but they are
    // not something the quick switcher or a rescan should walk.
    query.exec(QStringLiteral("SELECT path FROM files WHERE words >= 0 ORDER BY path"));
    while (query.next())
        paths << query.value(0).toString();
    return paths;
}

QStringList Index::allTags() const
{
    QStringList tags;
    QSqlQuery query(m_db);
    query.exec(QStringLiteral("SELECT DISTINCT name FROM tags ORDER BY name"));
    while (query.next())
        tags << query.value(0).toString();
    return tags;
}

QList<Index::PropertyCount> Index::propertyCounts() const
{
    QList<PropertyCount> counts;
    QSqlQuery query(m_db);
    query.exec(QStringLiteral("SELECT key, MIN(type), COUNT(DISTINCT file) FROM props GROUP BY key ORDER BY key"));
    while (query.next())
        counts.append({query.value(0).toString(), Property::Type(query.value(1).toInt()), query.value(2).toInt()});
    return counts;
}

QString Index::resolve(const QString &target, const QString &fromPath) const
{
    return resolveIn(m_names, m_vaultRoot, target, fromPath);
}

QString Index::resolveIn(const QHash<QString, QStringList> &names, const QString &vaultRoot,
                         const QString &target, const QString &fromPath)
{
    QString wanted = normaliseTarget(target);
    if (wanted.isEmpty())
        return {};

    // An absolute path inside the vault is just a vault-relative one.
    if (wanted.startsWith(vaultRoot + QLatin1Char('/')))
        wanted = wanted.mid(vaultRoot.size() + 1);

    const QString fromRelative =
        fromPath.isEmpty() ? QString() : QDir(vaultRoot).relativeFilePath(fromPath);

    // A target with a slash is a path: try it relative to the linking note
    // first, then relative to the vault root, the way Obsidian does.
    if (wanted.contains(QLatin1Char('/')) || wanted.startsWith(QLatin1String(".."))) {
        QStringList attempts;
        if (!fromRelative.isEmpty()) {
            attempts << QDir::cleanPath(QFileInfo(fromRelative).path() + QLatin1Char('/') + wanted);
        }
        attempts << QDir::cleanPath(wanted);
        for (const QString &attempt : std::as_const(attempts)) {
            const QStringList hit = names.value(foldCase(attempt));
            if (!hit.isEmpty())
                return hit.first();
        }
    }

    QStringList candidates = names.value(foldCase(wanted));
    // [[Secret]] still finds Secret.md once it is Secret.mdlock.
    if (candidates.isEmpty())
        candidates = names.value(foldCase(wanted + QStringLiteral(".mdlock")));
    if (candidates.isEmpty())
        return {};
    if (candidates.size() == 1)
        return candidates.first();

    // Ambiguous: Obsidian prefers a note in the linking note's own folder,
    // then the shallowest path, then alphabetical order for stability.
    const QString fromFolder = QFileInfo(fromRelative).path();
    QString best;
    int bestDepth = std::numeric_limits<int>::max();
    for (const QString &candidate : candidates) {
        if (!fromPath.isEmpty() && QFileInfo(candidate).path() == fromFolder)
            return candidate;
        const int depth = candidate.count(QLatin1Char('/'));
        if (depth < bestDepth || (depth == bestDepth && candidate < best)) {
            bestDepth = depth;
            best = candidate;
        }
    }
    return best;
}

QList<Link> Index::outgoingLinks(const QString &path) const
{
    QList<Link> links;
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral(
        "SELECT target, subpath, alias, embed, wiki, external, start, finish FROM links "
        "JOIN files ON files.id = links.file WHERE files.path = ? ORDER BY start"));
    query.addBindValue(QDir(m_vaultRoot).relativeFilePath(path));
    query.exec();
    while (query.next()) {
        Link link;
        link.target = query.value(0).toString();
        link.subpath = query.value(1).toString();
        link.alias = query.value(2).toString();
        link.embed = query.value(3).toBool();
        link.wiki = query.value(4).toBool();
        link.external = query.value(5).toBool();
        link.range = {query.value(6).toLongLong(), query.value(7).toLongLong()};
        links.append(link);
    }
    return links;
}

QList<Backlink> Index::backlinks(const QString &path) const
{
    const QDir vault(m_vaultRoot);
    const QString relative = vault.relativeFilePath(path);
    QList<Backlink> found;

    // Only links whose target ends in this note's name can resolve to it, so
    // the index answers that and resolve() runs on the handful that come back.
    // Reading every link in the vault instead made opening one note cost time
    // proportional to the whole vault: 4.7 ms on 500 notes, 14.6 ms on 2 000.
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral(
        "SELECT files.path, links.target, links.subpath, links.alias, links.embed, "
        "links.wiki, links.start, links.finish FROM links "
        "JOIN files ON files.id = links.file WHERE links.external = 0 AND links.tname = ?"));
    query.addBindValue(targetName(QFileInfo(relative).fileName()));
    query.exec();
    while (query.next()) {
        const QString fromPath = query.value(0).toString();
        const QString target = query.value(1).toString();
        if (target.isEmpty())
            continue; // a link into the note's own headings
        if (resolve(target, vault.absoluteFilePath(fromPath)) != relative)
            continue;

        Backlink backlink;
        backlink.fromPath = fromPath;
        backlink.link.target = target;
        backlink.link.subpath = query.value(2).toString();
        backlink.link.alias = query.value(3).toString();
        backlink.link.embed = query.value(4).toBool();
        backlink.link.wiki = query.value(5).toBool();
        backlink.link.range = {query.value(6).toLongLong(), query.value(7).toLongLong()};
        found.append(backlink);
    }
    return found;
}

QStringList Index::notesWithTag(const QString &tag) const
{
    QStringList paths;
    QSqlQuery query(m_db);
    // A tag matches its own nested children, as in Obsidian.
    query.prepare(QStringLiteral(
        "SELECT DISTINCT files.path FROM tags JOIN files ON files.id = tags.file "
        "WHERE tags.name = ? OR tags.name LIKE ? ORDER BY files.path"));
    query.addBindValue(tag);
    query.addBindValue(tag + QStringLiteral("/%"));
    query.exec();
    while (query.next())
        paths << query.value(0).toString();
    return paths;
}

QHash<QString, int> Index::tagCounts() const
{
    QHash<QString, int> counts;
    QSqlQuery query(m_db);
    query.exec(QStringLiteral("SELECT DISTINCT file, name FROM tags ORDER BY file"));
    // Per note, the set of paths it counts towards - itself and every
    // '/'-ancestor - so a note tagged `a/b` and `a/c` adds 1 to `a`, not 2.
    QSet<QString> paths;
    qint64 file = -1;
    const auto flush = [&] {
        for (const QString &path : std::as_const(paths))
            ++counts[path];
        paths.clear();
    };
    while (query.next()) {
        if (const qint64 next = query.value(0).toLongLong(); next != file) {
            flush();
            file = next;
        }
        const QString name = query.value(1).toString();
        for (qsizetype slash = name.indexOf(QLatin1Char('/')); slash > 0;
             slash = name.indexOf(QLatin1Char('/'), slash + 1))
            paths.insert(name.left(slash));
        paths.insert(name);
    }
    flush();
    return counts;
}

QStringList Index::findByName(const QString &query, int limit) const
{
    QStringList paths;
    QSqlQuery statement(m_db);
    statement.prepare(QStringLiteral(
        "SELECT DISTINCT files.path FROM names JOIN files ON files.id = names.file "
        "WHERE names.folded LIKE ? AND files.words >= 0 "
        "ORDER BY length(files.path), files.path LIMIT ?"));
    statement.addBindValue(QStringLiteral("%%%1%%").arg(foldCase(query)));
    statement.addBindValue(limit);
    statement.exec();
    while (statement.next())
        paths << statement.value(0).toString();
    return paths;
}

QStringList Index::linkTargets() const
{
    if (m_linkTargetsValid)
        return m_linkTargetsCache;
    // An alias is offered as `Note|alias` (aliases()), so it is not also a
    // plain name - unless it is some note's own name, which stays completable.
    QSet<QString> aliasNames;
    for (const auto &entry : aliases()) {
        if (entry.first != QFileInfo(entry.second).fileName())
            aliasNames.insert(entry.first);
    }
    for (auto it = aliasNames.begin(); it != aliasNames.end();) {
        bool isNoteName = false;
        for (const QString &path : m_names.value(foldCase(*it))) {
            isNoteName = isNoteName || ((path.endsWith(QLatin1String(".md")) || path.endsWith(QLatin1String(".mdlock")))
                                        && QFileInfo(path).completeBaseName() == *it);
        }
        it = isNoteName ? aliasNames.erase(it) : std::next(it);
    }
    // From the names kept in memory, already in order, not a second pass over
    // the table: sorting them on every name change was most of the cost.
    // ponytail: ordered by UTF-16 unit, which differs from the table's byte
    // order only between U+E000-U+FFFF and characters outside the BMP.
    QStringList names;
    names.reserve(qsizetype(m_nameCounts.size()));
    for (const auto &entry : m_nameCounts) {
        if (!aliasNames.contains(entry.first))
            names << entry.first;
    }
    m_linkTargetsCache = names;
    m_linkTargetsValid = true;
    return names;
}

QList<QPair<QString, QString>> Index::aliases() const
{
    if (m_aliasesValid)
        return m_aliasesCache;
    QList<QPair<QString, QString>> found;
    QSqlQuery query(m_db);
    query.exec(QStringLiteral("SELECT files.path, props.type, props.value FROM props "
                              "JOIN files ON files.id = props.file "
                              "WHERE lower(props.key) IN ('aliases', 'alias')"));
    while (query.next()) {
        // The name a link to the note can use: its basename, or its path
        // when another note answers to that basename (the rule a rename
        // follows for the links it rewrites).
        const QString path = query.value(0).toString();
        const QString base = QFileInfo(path).completeBaseName();
        const QString note = basenameStillUnique(base, path)
                                 ? base : path.chopped(QFileInfo(path).suffix().size() + 1);
        // The parser's rule (Frontmatter.cpp): a plain string is a comma list.
        const bool list = query.value(1).toInt() == int(Property::Type::List);
        for (const QString &piece : query.value(2).toString().split(list ? QLatin1Char('\n') : QLatin1Char(','),
                                                                     Qt::SkipEmptyParts)) {
            if (!piece.trimmed().isEmpty())
                found.append({piece.trimmed(), note});
        }
    }
    m_aliasesCache = found;
    m_aliasesValid = true;
    return found;
}

QStringList Index::fragmentsOf(const QString &path, bool blocks, const QStringList &parents) const
{
    QStringList found;
    QList<Heading> headings;
    QSqlQuery query(m_db);
    query.prepare(QString(QStringLiteral("SELECT %1 FROM %2 JOIN files ON files.id = %2.file "
                                         "WHERE files.path = ? ORDER BY start"))
                      .arg(blocks ? QStringLiteral("block") : QStringLiteral("level, text"),
                           blocks ? QStringLiteral("blocks") : QStringLiteral("headings")));
    query.addBindValue(path);
    query.exec();
    while (query.next()) {
        if (blocks)
            found << query.value(0).toString();
        else
            headings.append({query.value(0).toInt(), query.value(1).toString(), {}, {}});
    }
    if (!blocks) {
        for (const qsizetype i : ObsidianSyntax::headingsUnder(headings, parents))
            found << headings.at(i).text;
    }
    return found;
}

// --- search (libs/index/Search.h has the grammar and the evaluator) -------

/// The FTS5-narrow-then-confirm engine behind both search() and
/// unlinkedMentions(): they differ only in what they do with a match, not in
/// how one is found, so the DB-connection and candidate-gathering code lives
/// here once. Calls `onMatch` for up to `limit` candidates that satisfy
/// `query`, each with the NoteContext evaluate() was run against (so a
/// caller can rebuild a snippet, mask offsets, or whatever else it needs
/// without a second trip to the database).
///
/// Opens its own connection rather than reusing `m_db`, never touching any
/// other member: QSqlDatabase requires a connection be used only from the
/// thread that opened it, and this has to be safe to call from a
/// QtConcurrent worker so a search over a large vault never costs the UI
/// thread anything. WAL mode (set once in open()) is what lets this reader
/// run alongside the indexer's writer.
void Index::eachMatch(const QString &databasePath, const SearchQuery &query, int limit,
                      const std::function<void(NoteContext &&, SearchOutcome &&)> &onMatch)
{
    if (!query.isValid())
        return;

    const QString connectionName =
        QStringLiteral("noteAppC-search-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);
        db.setDatabaseName(databasePath);
        if (!db.open()) {
            QSqlDatabase::removeDatabase(connectionName);
            return;
        }

        // Tags, headings and tasks are loaded whole rather than filtered to
        // just these candidates (no IN-list, no bind-parameter ceiling to
        // worry about) - ponytail: a full scan of three small tables per
        // call. Skipped entirely when the query never reads them, which is
        // the common case (a handful of bare words never touches any of the
        // three), so the cost that remains scales with how much of the vault
        // actually uses tags/headings/tasks, not with the query.
        const FieldNeeds needs = query.fieldNeeds();
        QHash<qint64, QList<NoteTag>> tagsByFile;
        QHash<qint64, QList<NoteHeading>> headingsByFile;
        QHash<qint64, QList<NoteTask>> tasksByFile;
        QHash<qint64, QList<NoteProperty>> propertiesByFile;
        QHash<qint64, QList<Range>> metaNotesByFile;

        if (needs.tags) {
            QSqlQuery tagQuery(db);
            tagQuery.exec(QStringLiteral("SELECT file, name, start, finish FROM tags"));
            while (tagQuery.next()) {
                tagsByFile[tagQuery.value(0).toLongLong()].append(
                    NoteTag{tagQuery.value(1).toString(),
                           Range{tagQuery.value(2).toLongLong(), tagQuery.value(3).toLongLong()}});
            }
        }
        if (needs.headings) {
            QSqlQuery headingQuery(db);
            headingQuery.exec(QStringLiteral("SELECT file, level, start, finish FROM headings"));
            while (headingQuery.next()) {
                headingsByFile[headingQuery.value(0).toLongLong()].append(NoteHeading{
                    headingQuery.value(1).toInt(),
                    Range{headingQuery.value(2).toLongLong(), headingQuery.value(3).toLongLong()}});
            }
        }
        if (needs.tasks) {
            QSqlQuery taskQuery(db);
            taskQuery.exec(QStringLiteral("SELECT file, state, start, finish FROM tasks"));
            while (taskQuery.next()) {
                const QString state = taskQuery.value(1).toString();
                tasksByFile[taskQuery.value(0).toLongLong()].append(
                    NoteTask{state.isEmpty() ? QLatin1Char(' ') : state.at(0),
                            Range{taskQuery.value(2).toLongLong(), taskQuery.value(3).toLongLong()}});
            }
        }

        if (needs.metaNotes) {
            QSqlQuery metaQuery(db);
            metaQuery.exec(QStringLiteral("SELECT file, start, finish FROM meta_notes ORDER BY file, start"));
            while (metaQuery.next()) {
                metaNotesByFile[metaQuery.value(0).toLongLong()].append(
                    Range{metaQuery.value(1).toLongLong(), metaQuery.value(2).toLongLong()});
            }
        }

        if (needs.properties) {
            QSqlQuery propertyQuery(db);
            propertyQuery.exec(QStringLiteral("SELECT file, key, value FROM props"));
            while (propertyQuery.next()) {
                propertiesByFile[propertyQuery.value(0).toLongLong()].append(
                    NoteProperty{propertyQuery.value(1).toString(),
                                 propertyQuery.value(2).toString().split(QLatin1Char('\n'))});
            }
        }

        // Candidates in path order, but without their bodies: ORDER BY makes
        // SQLite copy every selected column of every match into its sorter, so
        // a common word over 20 000 notes sorted 20 000 bodies to confirm the
        // first 200 (18 ms). Bodies are read one by one below, only until
        // `limit` notes have matched.
        const QString fts = query.ftsExpression();
        QSqlQuery statement(db);
        statement.setForwardOnly(true);
        bool ok = false;
        if (!fts.isEmpty()) {
            statement.prepare(QStringLiteral(
                "SELECT rowid, path FROM content WHERE content MATCH ? ORDER BY path"));
            statement.addBindValue(fts);
            ok = statement.exec();
        }
        if (fts.isEmpty() || !ok) {
            // No restriction could be derived (a bare -word, path:/tag:-only
            // query, a lone /regex/) - every note is a candidate, and
            // evaluate() below is what actually confirms each one.
            statement.exec(QStringLiteral("SELECT rowid, path FROM content ORDER BY path"));
        }
        QSqlQuery bodyQuery(db);
        bodyQuery.prepare(QStringLiteral("SELECT body FROM content WHERE rowid = ?"));

        int delivered = 0;
        while (delivered < limit && statement.next()) {
            const qint64 id = statement.value(0).toLongLong();
            bodyQuery.bindValue(0, id);
            if (!bodyQuery.exec() || !bodyQuery.next())
                continue;

            NoteContext note;
            note.path = statement.value(1).toString();
            note.text = bodyQuery.value(0).toString();
            bodyQuery.finish();
            note.tags = tagsByFile.value(id);
            note.headings = headingsByFile.value(id);
            note.tasks = tasksByFile.value(id);
            note.properties = propertiesByFile.value(id);
            note.metaNotes = metaNotesByFile.value(id);

            SearchOutcome outcome = query.evaluate(note);
            if (!outcome.matched)
                continue;
            std::sort(outcome.matches.begin(), outcome.matches.end(),
                      [](const SearchMatch &a, const SearchMatch &b) { return a.offset < b.offset; });

            ++delivered;
            onMatch(std::move(note), std::move(outcome));
        }
    }
    QSqlDatabase::removeDatabase(connectionName);
}

QList<SearchResult> Index::searchIn(const QString &databasePath, const QString &queryText, int limit, QString *error)
{
    QList<SearchResult> results;
    if (queryText.trimmed().isEmpty())
        return results;

    const SearchQuery query = SearchQuery::parse(queryText);
    if (!query.isValid()) {
        if (error)
            *error = query.error();
        return results;
    }

    eachMatch(databasePath, query, limit, [&results](NoteContext &&note, SearchOutcome &&outcome) {
        SearchResult result;
        result.path = note.path;
        result.matches = outcome.matches;
        result.matchCount = int(result.matches.size());
        const SearchSnippet snippet = buildSnippet(note.text, result.matches);
        result.snippet = snippet.text;
        result.snippetHighlights = snippet.highlights;
        results.append(std::move(result));
    });
    return results;
}

QList<SearchResult> Index::unlinkedMentions(const QString &path, int limit) const
{
    QList<SearchResult> results;
    if (!m_db.isOpen())
        return results;

    const QString relative = QDir(m_vaultRoot).relativeFilePath(path);

    // The note's own title and aliases, re-read from the file rather than
    // trusted from the index - "the file is the truth". From its front
    // matter alone: a whole parse was 130 ms for a 1 MB note, each time the
    // backlinks pane showed it. And only as much of the file as holds the
    // front matter is read: decoding all of an 11 MB notebook was 60 ms.
    Note note;
    {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly))
            return results;
        QByteArray bytes = file.read(16 * 1024);
        note = Vault::decode(bytes);
        const Range head = Frontmatter::find(note.text);
        const bool complete = !note.text.startsWith(QLatin1String("---"))
                              || (head.isValid() && note.text.at(head.end - 1) == QLatin1Char('\n'));
        if (!file.atEnd() && !complete)
            note = Vault::decode(bytes + file.readAll());
    }
    Metadata own;
    own.path = path;
    Frontmatter::parse(note.text, Frontmatter::find(note.text), own);
    const QStringList titles = own.linkNames();
    if (titles.isEmpty())
        return results;

    QStringList quoted;
    quoted.reserve(titles.size());
    for (const QString &title : titles) {
        QString escaped = title;
        escaped.replace(QLatin1Char('"'), QStringLiteral("\\\""));
        quoted << QStringLiteral("\"%1\"").arg(escaped);
    }
    const SearchQuery query = SearchQuery::parse(quoted.join(QStringLiteral(" OR ")));
    if (!query.isValid())
        return results;

    // Every link that already resolves to this note, so its own [[...]] (or
    // [text](...)) bytes can be masked out of what the search below finds -
    // otherwise a note that already links here would also show up as an
    // "unlinked" mention, since the link text itself contains the title.
    QHash<QString, QList<Range>> linkedRanges;
    for (const Backlink &backlink : backlinks(path))
        linkedRanges[backlink.fromPath].append(backlink.link.range);

    // +1 of slack for the note's own row, which is always dropped below and
    // would otherwise silently cost one real result.
    eachMatch(m_databasePath, query, limit + 1, [&](NoteContext &&candidate, SearchOutcome &&outcome) {
        if (candidate.path == relative || results.size() >= limit)
            return;

        const QList<Range> masked = linkedRanges.value(candidate.path);
        QList<SearchMatch> kept;
        for (const SearchMatch &match : outcome.matches) {
            const Range span{match.offset, match.offset + match.length};
            const bool insideAnExistingLink = std::any_of(
                masked.cbegin(), masked.cend(),
                [&span](const Range &range) { return range.overlaps(span); });
            if (!insideAnExistingLink)
                kept.append(match);
        }
        if (kept.isEmpty())
            return; // every occurrence here was already a linked mention

        SearchResult result;
        result.path = candidate.path;
        result.matches = kept;
        result.matchCount = int(kept.size());
        const SearchSnippet snippet = buildSnippet(candidate.text, kept);
        result.snippet = snippet.text;
        result.snippetHighlights = snippet.highlights;
        results.append(std::move(result));
    });
    return results;
}

bool Index::basenameStillUnique(const QString &basename, const QString &excludePath,
                                const QString &alsoExclude) const
{
    const QStringList candidates = m_names.value(foldCase(basename));
    for (const QString &candidate : candidates) {
        if (candidate != excludePath && candidate != alsoExclude)
            return false;
    }
    return true;
}

QString Index::rebasedTarget(const QString &oldTarget, const QString &oldRelative,
                             const QString &newRelative) const
{
    const bool dotSlash = oldTarget.startsWith(QLatin1String("./"));
    QString working = dotSlash ? oldTarget.mid(2) : oldTarget;
    const bool hadExt = working.endsWith(QLatin1String(".md"), Qt::CaseInsensitive);
    if (hadExt)
        working.chop(3);

    const QString oldBase = QFileInfo(oldRelative).completeBaseName();
    const QString newBase = QFileInfo(newRelative).completeBaseName();
    const QString newFull = newRelative.endsWith(QLatin1String(".md"), Qt::CaseInsensitive)
                                ? newRelative.chopped(3)
                                : newRelative;

    // A target with no slash that named the old note by its bare basename
    // stays a basename, provided the new one is still unambiguous - exactly
    // Obsidian's "shortest path when possible" rule. Anything else (already a
    // path, or some shape this pass does not specifically recognise) becomes
    // the new vault-relative path.
    QString rebased;
    if (!working.contains(QLatin1Char('/')) && foldCase(working) == foldCase(oldBase))
        rebased = basenameStillUnique(newBase, oldRelative, newRelative) ? newBase : newFull;
    else
        rebased = newFull;

    if (hadExt)
        rebased += QLatin1String(".md");
    if (dotSlash)
        rebased.prepend(QLatin1String("./"));
    return rebased;
}

int Index::updateLinksAfterRename(const QString &oldRelative, const QString &newRelative,
                                  const Vault &vault, QString *error, const QStringList &extraCandidates)
{
    if (!m_db.isOpen()) {
        if (error)
            *error = QStringLiteral("index is not open");
        return -1;
    }

    // One pass over every internal link to find what actually resolves to
    // the renamed note - resolution depends on the linking note's own folder,
    // so this cannot be a plain string match against `target`.
    struct Hit {
        QString subpath;
        bool wiki = true;
        Range targetRange;
        QString resolved; // the note the link pointed at, old path
        QString movedTo;  // the same note after the rename
        bool own = false; // a link written in a moved note, to a file that stays
    };

    // Where a path ends up after the rename; "" for one that does not move.
    const auto movedPath = [&](const QString &path) -> QString {
        if (path == oldRelative)
            return newRelative;
        if (path.startsWith(oldRelative + QLatin1Char('/')))
            return newRelative + path.mid(oldRelative.size());
        return {};
    };

    // Whether one internal link, in the note at `fromPath` (old path), has
    // to change, and what it resolved to. Used on the index's rows to pick
    // the notes to open, and again on the links parsed from each note's text.
    const auto classify = [&](const QString &fromPath, const QString &target, bool wiki, Hit &hit) {
        if (target.isEmpty())
            return false; // a link into the note's own headings, nothing to resolve

        // oldRelative is a note or a folder: a folder's notes move with it,
        // each to the same spot under the new name.
        QString resolved = resolve(target, vault.absolute(fromPath));
        QString movedTo = movedPath(resolved);
        bool own = false;
        if (movedTo.isEmpty()) {
            // Not a link to what moved. A Markdown link written in a moved
            // note is relative to its folder, so it must be rewritten to
            // reach the same file from the new one, note or attachment alike
            // (neither the index nor Obsidian's name lookup covers a path to
            // an image). A wikilink is found by name and stays as it is.
            // ponytail: only a path that exists relative to the old folder;
            // one that resolved by name alone (a bare `a.md`) is left to
            // that lookup.
            if (wiki || movedPath(fromPath).isEmpty() || target.startsWith(QLatin1Char('/')))
                return false;
            resolved = QDir::cleanPath(QFileInfo(fromPath).path() + QLatin1Char('/') + target);
            if (resolved.startsWith(QLatin1String("..")) || !movedPath(resolved).isEmpty()
                || !QFileInfo(vault.absolute(resolved)).isFile())
                return false;
            movedTo = resolved;
            own = true;
        }
        hit.resolved = resolved;
        hit.movedTo = movedTo;
        hit.own = own;
        return true;
    };

    // The index lags a save, so its rows only say which notes to open; the
    // offsets to splice at come from the text read below.
    QStringList candidates;
    QSet<QString> extra; // those only the caller named
    {
        QSqlQuery query(m_db);
        query.exec(QStringLiteral("SELECT files.path, links.target, links.wiki FROM links "
                                  "JOIN files ON files.id = links.file WHERE links.external = 0"));
        QSet<QString> seen;
        Hit unused;
        while (query.next()) {
            const QString fromPath = query.value(0).toString();
            if (!seen.contains(fromPath) && classify(fromPath, query.value(1).toString(), query.value(2).toBool(), unused)) {
                seen.insert(fromPath);
                candidates.append(fromPath);
            }
        }
        // Their links are found in the text read, like the others'.
        for (const QString &path : extraCandidates) {
            if (!seen.contains(path)) {
                seen.insert(path);
                extra.insert(path);
                candidates.append(path);
            }
        }
    }

    beginBatch();
    const Parser parser;
    const ParseOptions options = parseOptions();
    int rewritten = 0;
    QStringList skippedLossy;
    QStringList skippedUnreadable;

    const auto reindex = [&](const QString &absolutePath, const QString &text) {
        Metadata metadata = parser.parse(text, absolutePath, options);
        const QFileInfo info(absolutePath);
        metadata.mtime = info.lastModified().toMSecsSinceEpoch();
        metadata.size = info.size();
        metadata.hash = QCryptographicHash::hash(text.toUtf8(), QCryptographicHash::Sha1);
        put(metadata, text);
    };

    for (const QString &fromPath : std::as_const(candidates)) {
        // The renamed note (or every note under the renamed folder) has
        // already moved on disk by the time this runs; every other linking
        // note is still where the index found it.
        const QString actualRelative = movedPath(fromPath).isEmpty() ? fromPath : movedPath(fromPath);
        const QString absolutePath = vault.absolute(actualRelative);

        Note note;
        if (!Vault::read(absolutePath, note)) {
            // Gone or unreadable since the index looked (the file has already
            // moved): one note's links must not undo the rest. A note only
            // named by the caller may simply be gone, which is no news.
            if (!extra.contains(fromPath))
                skippedUnreadable.append(actualRelative);
            continue;
        }
        // Write would refuse it; leave its bytes alone and say so.
        if (note.lossy) {
            skippedLossy.append(actualRelative);
            continue;
        }

        QList<Hit> hits;
        for (const Link &link : parser.parse(note.text, absolutePath, options).links) {
            Hit hit;
            if (link.external || !classify(fromPath, link.target, link.wiki, hit))
                continue;
            hit.subpath = link.subpath;
            hit.wiki = link.wiki;
            hit.targetRange = link.targetRange;
            hits.append(hit);
        }
        if (hits.isEmpty())
            continue; // the links to it were removed since the index last looked
        const QString textBefore = note.text;

        // Last link first, so an earlier link's offsets stay valid while a
        // later one's replacement text changes length.
        std::sort(hits.begin(), hits.end(), [](const Hit &a, const Hit &b) {
            return a.targetRange.start > b.targetRange.start;
        });

        for (const Hit &hit : std::as_const(hits)) {
            // A wikilink's targetRange is the target alone. A Markdown link's
            // covers the whole URL, subpath included, since that is one
            // contiguous node in md4qt; only the part before it is the target.
            if (hit.targetRange.length() < 0 || hit.targetRange.end > note.text.size())
                continue; // stale offset - the file moved under us; a rebuild will catch it

            // A CommonMark `<path with spaces.md>` target: the brackets stay
            // where they are and only what is inside them is rewritten.
            const QString whole = note.text.mid(hit.targetRange.start, hit.targetRange.length());
            const bool angled = !hit.wiki && whole.size() >= 2 && whole.startsWith(QLatin1Char('<'))
                                && whole.endsWith(QLatin1Char('>'));
            const qsizetype rawStart = hit.targetRange.start + (angled ? 1 : 0);
            const qsizetype rawLength = (hit.wiki ? hit.targetRange.length()
                                                  : hit.targetRange.length() - hit.subpath.length())
                                        - (angled ? 2 : 0);
            if (rawLength < 0)
                continue;
            const QString raw = note.text.mid(rawStart, rawLength);
            const QString decoded = hit.wiki ? raw : QUrl::fromPercentEncoding(raw.toUtf8());

            // A Markdown link spelled as a path from its note's folder stays
            // one: from the folder the linking note is in now to where the
            // file is now. (A bare name keeps rebasedTarget's name rule.)
            const QString fromFolder = QFileInfo(fromPath).path();
            const auto stem = [](QString path) {
                if (path.endsWith(QLatin1String(".md"), Qt::CaseInsensitive))
                    path.chop(3);
                return path.toCaseFolded();
            };
            const bool pathLink =
                !hit.wiki
                && (hit.own || (decoded.contains(QLatin1Char('/'))
                                && stem(QDir::cleanPath(fromFolder + QLatin1Char('/') + decoded)) == stem(hit.resolved)));
            if (pathLink) {
                QString relativeTarget = QDir(vault.absolute(QFileInfo(actualRelative).path()))
                                             .relativeFilePath(vault.absolute(hit.movedTo));
                if (!hit.own && !decoded.endsWith(QLatin1String(".md"), Qt::CaseInsensitive)
                    && relativeTarget.endsWith(QLatin1String(".md"), Qt::CaseInsensitive))
                    relativeTarget.chop(3);
                if (relativeTarget == decoded)
                    continue; // both ends moved together: not a byte to touch
                // Percent-encoded as it was, or literal when it was written
                // so (inside angle brackets a space needs no escape).
                const bool literal = !raw.contains(QLatin1Char('%'))
                                     && (angled || !relativeTarget.contains(QRegularExpression(QStringLiteral("[ ()<>]"))));
                QString written = literal ? relativeTarget
                                          : QString::fromUtf8(QUrl::toPercentEncoding(relativeTarget, "/"));
                note.text.replace(rawStart, rawLength, written);
                continue;
            }

            QString rebased = rebasedTarget(decoded, hit.resolved, hit.movedTo);
            if (!hit.wiki && !(angled && !raw.contains(QLatin1Char('%'))))
                rebased = QString::fromUtf8(QUrl::toPercentEncoding(rebased, "/"));

            note.text.replace(rawStart, rawLength, rebased);
        }

        if (note.text == textBefore)
            continue; // a sibling link in a moved folder: still right, nothing to write

        if (!Vault::write(absolutePath, note, error)) {
            endBatch();
            return -1;
        }

        reindex(absolutePath, note.text);
        ++rewritten;
    }

    // The renamed note's own row still sits at the old path until it is
    // refreshed here - whether or not anything else linked to it. Done last,
    // so the uniqueness checks above saw the pre-rename set of names.
    if (oldRelative != newRelative) {
        remove(vault.absolute(oldRelative));
        Note note;
        if (Vault::read(vault.absolute(newRelative), note))
            reindex(vault.absolute(newRelative), note.text);
    }

    endBatch();
    if (error && (!skippedLossy.isEmpty() || !skippedUnreadable.isEmpty())) {
        QStringList notes;
        if (!skippedLossy.isEmpty())
            notes << QStringLiteral("not UTF-8, so its links were left as they are: %1")
                         .arg(skippedLossy.join(QStringLiteral(", ")));
        if (!skippedUnreadable.isEmpty())
            notes << QStringLiteral("could not be read, so its links were left as they are: %1")
                         .arg(skippedUnreadable.join(QStringLiteral(", ")));
        *error = notes.join(QStringLiteral("; "));
    }
    return rewritten;
}

// --- graph view (libs/ext/graph) ------------------------------------------
// Added for the optional graph module; see the matching block at the end of
// Index.h. Two queries total, matching the "no per-note queries" budget the
// rest of Index already holds itself to (put()/remove() are the exception,
// and only because a write genuinely is per-note).

namespace {

/// The two graph queries over `db`, links resolved against `names`.
Index::GraphData readGraphData(QSqlDatabase &db, const QString &vaultRoot,
                               const QHash<QString, QStringList> &names)
{
    Index::GraphData result;
    // Files, with each one's tags concatenated in the same query - the
    // second query below is the only other one this method runs. Char 30
    // (ASCII record separator) is not a character a tag can contain.
    QSqlQuery files(db);
    files.exec(QStringLiteral(
        "SELECT files.path, files.words, GROUP_CONCAT(tags.name, char(30)) FROM files "
        "LEFT JOIN tags ON tags.file = files.id GROUP BY files.id ORDER BY files.path"));
    while (files.next()) {
        Index::GraphFileInfo file;
        file.path = files.value(0).toString();
        file.isAttachment = files.value(1).toInt() < 0;
        const QString tags = files.value(2).toString();
        if (!tags.isEmpty())
            file.tags = tags.split(QChar(30), Qt::SkipEmptyParts);
        result.files.append(file);
    }

    const QDir vaultDir(vaultRoot);
    QSqlQuery links(db);
    links.exec(QStringLiteral(
        "SELECT src.path, links.target FROM links "
        "JOIN files AS src ON src.id = links.file WHERE links.external = 0"));
    while (links.next()) {
        Index::GraphLinkInfo link;
        link.fromPath = links.value(0).toString();
        link.targetName = links.value(1).toString();
        // resolveIn() is an in-memory hash lookup (see m_names), not a query,
        // so running it once per link here still holds the "no per-note
        // queries" budget.
        link.toPath = Index::resolveIn(names, vaultRoot, link.targetName, vaultDir.absoluteFilePath(link.fromPath));
        result.links.append(link);
    }
    return result;
}

} // namespace

Index::GraphData Index::graphData() const
{
    if (!m_db.isOpen())
        return {};
    QSqlDatabase db = m_db;
    return readGraphData(db, m_vaultRoot, m_names);
}

Index::GraphSnapshot Index::graphSnapshot() const
{
    return GraphSnapshot{m_databasePath, m_vaultRoot, m_names};
}

Index::GraphData Index::graphDataIn(const GraphSnapshot &snapshot)
{
    GraphData result;
    const QString connectionName =
        QStringLiteral("noteAppC-graph-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);
        db.setDatabaseName(snapshot.databasePath);
        if (db.open())
            result = readGraphData(db, snapshot.vaultRoot, snapshot.names);
    }
    QSqlDatabase::removeDatabase(connectionName);
    return result;
}

// --- Bases (libs/ext/bases) ---------------------------------------------
// Added for the optional Bases module; see the block comment in Index.h.
// Read-only, and never called from the indexing path - only when a .base
// file is opened, so it costs nothing in a build without the module and
// nothing at startup in a build with it.

namespace {

/// Runs `read` on a connection of its own, opened and closed on the calling
/// thread (QSqlDatabase connections are per thread).
template <typename Read>
void withConnection(const QString &databasePath, Read read)
{
    const QString connectionName =
        QStringLiteral("noteAppC-bases-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);
        db.setDatabaseName(databasePath);
        if (db.open())
            read(db);
    }
    QSqlDatabase::removeDatabase(connectionName);
}

} // namespace

QList<Index::BaseNote> Index::notesForBases(const QString &databasePath)
{
    QList<BaseNote> notes;
    if (databasePath.isEmpty())
        return notes;

    QHash<qint64, qsizetype> rowOf; // file id -> index into notes
    withConnection(databasePath, [&](const QSqlDatabase &db) {
        QSqlQuery files(db);
        files.setForwardOnly(true); // no row cache: each row is read once
        // Attachments carry no properties and are not what a base lists. In
        // path order, what a view with no `sort:` shows: the rowid order was
        // the order the indexer happened to walk the disk in.
        files.exec(QStringLiteral("SELECT id, path, mtime, size FROM files WHERE words >= 0 ORDER BY path"));
        while (files.next()) {
            BaseNote note;
            note.path = files.value(1).toString();
            note.mtime = files.value(2).toLongLong();
            note.size = files.value(3).toLongLong();
            rowOf.insert(files.value(0).toLongLong(), notes.size());
            notes.append(std::move(note));
        }
    });

    // The three child tables are 60 000 to 100 000 rows each and the cost is
    // a QVariant per cell, so each is read on its own thread and connection
    // (WAL lets readers run side by side) into a list indexed like `notes`,
    // then moved in: 49 ms of queries on 20 000 notes become the slowest one.
    // std::async rather than the global pool: this may itself run on that
    // pool, and waiting on a task queued behind it would be a deadlock.
    const qsizetype rows = notes.size();
    const auto readEach = [&](const QString &sql, auto onRow) {
        withConnection(databasePath, [&](const QSqlDatabase &db) {
            QSqlQuery query(db);
            query.setForwardOnly(true);
            query.exec(sql);
            while (query.next()) {
                const auto it = rowOf.constFind(query.value(0).toLongLong());
                if (it != rowOf.constEnd())
                    onRow(it.value(), query);
            }
        });
    };
    QList<QList<Property>> properties(rows);
    QList<QStringList> tags(rows);
    QList<QStringList> linkTargets(rows);
    auto propsTask = std::async(std::launch::async, [&] {
        readEach(QStringLiteral("SELECT file, key, type, value FROM props"),
                 [&](qsizetype row, const QSqlQuery &query) {
                     Property property;
                     property.key = query.value(1).toString();
                     property.type = static_cast<Property::Type>(query.value(2).toInt());
                     // put() joins a list's values with '\n'; an empty join would
                     // come back as one empty string instead of no values at all.
                     const QString value = query.value(3).toString();
                     if (!value.isEmpty())
                         property.values = value.split(QLatin1Char('\n'));
                     properties[row].append(std::move(property));
                 });
    });
    auto tagsTask = std::async(std::launch::async, [&] {
        readEach(QStringLiteral("SELECT DISTINCT file, name FROM tags"),
                 [&](qsizetype row, const QSqlQuery &query) { tags[row].append(query.value(1).toString()); });
    });
    readEach(QStringLiteral("SELECT file, target FROM links WHERE external = 0 AND target != ''"),
             [&](qsizetype row, const QSqlQuery &query) { linkTargets[row].append(query.value(1).toString()); });
    propsTask.get();
    tagsTask.get();

    for (qsizetype row = 0; row < rows; ++row) {
        notes[row].properties = std::move(properties[row]);
        notes[row].tags = std::move(tags[row]);
        notes[row].linkTargets = std::move(linkTargets[row]);
    }
    return notes;
}

} // namespace noteAppC
