#include "Index.h"

#include <algorithm>

#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QUrl>
#include <QUuid>
#include <QVariant>

#include "Parser.h"
#include "Vault.h"

namespace noteAppC {

namespace {
/// Bump when the schema changes. The cache is derived from the vault, so a
/// mismatch drops it and rebuilds rather than migrating: a wrong migration is
/// a silently wrong index, and a rebuild costs one pass over the notes.
constexpr int kSchemaVersion = 3;
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

    m_db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_connectionName);
    m_db.setDatabaseName(m_databasePath);
    if (!m_db.open()) {
        m_error = m_db.lastError().text();
        return false;
    }

    QSqlQuery pragma(m_db);
    // WAL so a reader never blocks the indexer; NORMAL because the cache is
    // rebuildable from the vault at any time.
    pragma.exec(QStringLiteral("PRAGMA journal_mode = WAL"));
    pragma.exec(QStringLiteral("PRAGMA synchronous = NORMAL"));
    pragma.exec(QStringLiteral("PRAGMA foreign_keys = ON"));

    int version = 0;
    if (pragma.exec(QStringLiteral("PRAGMA user_version")) && pragma.next())
        version = pragma.value(0).toInt();
    if (version != kSchemaVersion)
        dropSchema();

    if (!createSchema())
        return false;
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
                       "done INTEGER, text TEXT, start INTEGER, finish INTEGER)"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS props ("
                       "file INTEGER NOT NULL REFERENCES files(id) ON DELETE CASCADE, "
                       "key TEXT, type INTEGER, value TEXT)"),
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

void Index::dropSchema()
{
    QSqlQuery query(m_db);
    for (const QString &table : {QStringLiteral("content"), QStringLiteral("names"),
                                 QStringLiteral("props"), QStringLiteral("tasks"),
                                 QStringLiteral("blocks"), QStringLiteral("tags"),
                                 QStringLiteral("links"), QStringLiteral("headings"),
                                 QStringLiteral("files")}) {
        query.exec(QStringLiteral("DROP TABLE IF EXISTS ") + table);
    }
}

void Index::loadNames()
{
    m_names.clear();
    m_pathNames.clear();

    QSqlQuery query(m_db);
    query.exec(QStringLiteral("SELECT files.path, names.name FROM names "
                              "JOIN files ON files.id = names.file"));
    while (query.next()) {
        const QString path = query.value(0).toString();
        const QString name = query.value(1).toString();
        m_names[foldCase(name)].append(path);
        m_pathNames[path].append(name);
    }
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
        auto it = m_names.find(foldCase(name));
        if (it != m_names.end()) {
            it->removeAll(path);
            if (it->isEmpty())
                m_names.erase(it);
        }
    }
    m_pathNames.remove(path);
}

void Index::indexNames(qint64 fileId, const QString &path, const QStringList &names)
{
    QSqlQuery query(m_db);
    for (const QString &name : names) {
        if (name.isEmpty())
            continue;
        query.prepare(QStringLiteral("INSERT INTO names(file, name, folded) VALUES(?, ?, ?)"));
        query.addBindValue(fileId);
        query.addBindValue(name);
        query.addBindValue(foldCase(name));
        query.exec();
        m_names[foldCase(name)].append(path);
        m_pathNames[path].append(name);
    }
}

void Index::put(const Metadata &metadata, const QString &body)
{
    if (!m_db.isOpen())
        return;

    const QString path = QDir(m_vaultRoot).relativeFilePath(metadata.path);
    remove(metadata.path);

    QSqlQuery query(m_db);
    query.prepare(QStringLiteral(
        "INSERT INTO files(path, mtime, size, hash, words) VALUES(?, ?, ?, ?, ?)"));
    query.addBindValue(path);
    query.addBindValue(metadata.mtime);
    query.addBindValue(metadata.size);
    query.addBindValue(metadata.hash);
    query.addBindValue(metadata.wordCount);
    if (!query.exec()) {
        m_error = query.lastError().text();
        return;
    }
    const qint64 fileId = query.lastInsertId().toLongLong();

    const auto insert = [&](const QString &sql, const QVariantList &values) {
        QSqlQuery statement(m_db);
        statement.prepare(sql);
        for (const QVariant &value : values)
            statement.addBindValue(value);
        statement.exec();
    };

    for (const Heading &heading : metadata.headings) {
        insert(QStringLiteral("INSERT INTO headings(file, level, text, start, finish) "
                              "VALUES(?, ?, ?, ?, ?)"),
               {fileId, heading.level, heading.text, heading.range.start, heading.range.end});
    }
    for (const Link &link : metadata.links) {
        insert(QStringLiteral("INSERT INTO links(file, target, tname, subpath, alias, embed, "
                              "wiki, external, start, finish, tstart, tfinish) "
                              "VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)"),
               {fileId, link.target, link.external ? QString() : targetName(link.target),
                link.subpath, link.alias, link.embed, link.wiki,
                link.external, link.range.start, link.range.end,
                link.targetRange.start, link.targetRange.end});
    }
    for (const Tag &tag : metadata.tags) {
        insert(QStringLiteral("INSERT INTO tags(file, name, start, finish) VALUES(?, ?, ?, ?)"),
               {fileId, tag.name, tag.range.start, tag.range.end});
    }
    for (const BlockId &block : metadata.blockIds) {
        insert(QStringLiteral("INSERT INTO blocks(file, block, start, finish) VALUES(?, ?, ?, ?)"),
               {fileId, block.id, block.range.start, block.range.end});
    }
    for (const Task &task : metadata.tasks) {
        insert(QStringLiteral("INSERT INTO tasks(file, done, text, start, finish) "
                              "VALUES(?, ?, ?, ?, ?)"),
               {fileId, task.done, task.text, task.range.start, task.range.end});
    }
    for (const Property &property : metadata.properties) {
        insert(QStringLiteral("INSERT INTO props(file, key, type, value) VALUES(?, ?, ?, ?)"),
               {fileId, property.key, int(property.type), property.values.join(QLatin1Char('\n'))});
    }

    QStringList names = metadata.linkNames();
    // A link may also spell the vault-relative path, with or without folders.
    names << path;
    if (path.endsWith(QLatin1String(".md")))
        names << path.chopped(3);
    names.removeDuplicates();
    indexNames(fileId, path, names);

    // The rowid is the file's id, so a delete is a rowid lookup rather than a
    // scan of the whole full-text table. That scan was quadratic: 20 000 notes
    // took 39.8 s to index, against a 5 s budget.
    insert(QStringLiteral("INSERT INTO content(rowid, path, body) VALUES(?, ?, ?)"),
           {fileId, path, body});

    if (!m_inBatch)
        emit changed();
}

void Index::putAttachment(const QString &path, qint64 mtime, qint64 size)
{
    if (!m_db.isOpen())
        return;

    const QString relative = QDir(m_vaultRoot).relativeFilePath(path);
    remove(path);

    QSqlQuery query(m_db);
    query.prepare(QStringLiteral(
        "INSERT INTO files(path, mtime, size, hash, words) VALUES(?, ?, ?, NULL, -1)"));
    query.addBindValue(relative);
    query.addBindValue(mtime);
    query.addBindValue(size);
    if (!query.exec()) {
        m_error = query.lastError().text();
        return;
    }

    // An attachment answers to its file name and to its path, both with the
    // extension, which is how Obsidian resolves `![[image.png]]`.
    QStringList names{QFileInfo(relative).fileName(), relative};
    names.removeDuplicates();
    indexNames(query.lastInsertId().toLongLong(), relative, names);

    if (!m_inBatch)
        emit changed();
}

void Index::remove(const QString &path)
{
    if (!m_db.isOpen())
        return;
    const QString relative = QDir(m_vaultRoot).relativeFilePath(path);

    // One indexed lookup first: on a cold build nothing is cached yet, so
    // every note would otherwise pay for two deletes that match no rows.
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral("SELECT id FROM files WHERE path = ?"));
    query.addBindValue(relative);
    if (!query.exec() || !query.next()) {
        forgetNames(relative);
        return;
    }
    const qint64 fileID = query.value(0).toLongLong();

    query.prepare(QStringLiteral("DELETE FROM content WHERE rowid = ?"));
    query.addBindValue(fileID);
    query.exec();

    query.prepare(QStringLiteral("DELETE FROM files WHERE id = ?"));
    query.addBindValue(fileID);
    query.exec();

    forgetNames(relative);
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

QString Index::resolve(const QString &target, const QString &fromPath) const
{
    QString wanted = normaliseTarget(target);
    if (wanted.isEmpty())
        return {};

    // An absolute path inside the vault is just a vault-relative one.
    if (wanted.startsWith(m_vaultRoot + QLatin1Char('/')))
        wanted = wanted.mid(m_vaultRoot.size() + 1);

    const QString fromRelative =
        fromPath.isEmpty() ? QString() : QDir(m_vaultRoot).relativeFilePath(fromPath);

    // A target with a slash is a path: try it relative to the linking note
    // first, then relative to the vault root, the way Obsidian does.
    if (wanted.contains(QLatin1Char('/')) || wanted.startsWith(QLatin1String(".."))) {
        QStringList attempts;
        if (!fromRelative.isEmpty()) {
            attempts << QDir::cleanPath(QFileInfo(fromRelative).path() + QLatin1Char('/') + wanted);
        }
        attempts << QDir::cleanPath(wanted);
        for (const QString &attempt : std::as_const(attempts)) {
            const QStringList hit = m_names.value(foldCase(attempt));
            if (!hit.isEmpty())
                return hit.first();
        }
    }

    const QStringList candidates = m_names.value(foldCase(wanted));
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
    QStringList names;
    QSqlQuery query(m_db);
    query.exec(QStringLiteral("SELECT DISTINCT name FROM names ORDER BY name"));
    while (query.next())
        names << query.value(0).toString();
    return names;
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
void Index::eachMatch(const SearchQuery &query, int limit,
                      const std::function<void(NoteContext &&, SearchOutcome &&)> &onMatch) const
{
    if (!query.isValid())
        return;

    const QString connectionName =
        QStringLiteral("noteAppC-search-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);
        db.setDatabaseName(m_databasePath);
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
            taskQuery.exec(QStringLiteral("SELECT file, done, start, finish FROM tasks"));
            while (taskQuery.next()) {
                tasksByFile[taskQuery.value(0).toLongLong()].append(
                    NoteTask{taskQuery.value(1).toBool(),
                            Range{taskQuery.value(2).toLongLong(), taskQuery.value(3).toLongLong()}});
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

QList<SearchResult> Index::search(const QString &queryText, int limit, QString *error) const
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

    eachMatch(query, limit, [&results](NoteContext &&note, SearchOutcome &&outcome) {
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
    // trusted from the index - "the file is the truth", and this only runs
    // once per note the backlinks pane opens, not on every keystroke.
    Note note;
    if (!Vault::read(path, note))
        return results;
    const Parser parser;
    const QStringList titles = parser.parse(note.text, path).linkNames();
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
    eachMatch(query, limit + 1, [&](NoteContext &&candidate, SearchOutcome &&outcome) {
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

bool Index::basenameStillUnique(const QString &basename, const QString &excludePath) const
{
    const QStringList candidates = m_names.value(foldCase(basename));
    for (const QString &candidate : candidates) {
        if (candidate != excludePath)
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
        rebased = basenameStillUnique(newBase, oldRelative) ? newBase : newFull;
    else
        rebased = newFull;

    if (hadExt)
        rebased += QLatin1String(".md");
    if (dotSlash)
        rebased.prepend(QLatin1String("./"));
    return rebased;
}

int Index::updateLinksAfterRename(const QString &oldRelative, const QString &newRelative,
                                  const Vault &vault, QString *error)
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
    };
    QHash<QString, QList<Hit>> perFile;

    QSqlQuery query(m_db);
    query.exec(QStringLiteral(
        "SELECT files.path, links.target, links.subpath, links.wiki, links.tstart, "
        "links.tfinish FROM links JOIN files ON files.id = links.file "
        "WHERE links.external = 0"));
    while (query.next()) {
        const QString fromPath = query.value(0).toString();
        const QString target = query.value(1).toString();
        if (target.isEmpty())
            continue; // a link into the note's own headings, nothing to resolve
        if (query.isNull(4) || query.isNull(5))
            continue; // indexed before tstart/tfinish existed; rebuild will fix it

        const QString fromAbsolute = vault.absolute(fromPath);
        if (resolve(target, fromAbsolute) != oldRelative)
            continue;

        Hit hit;
        hit.subpath = query.value(2).toString();
        hit.wiki = query.value(3).toBool();
        hit.targetRange = {query.value(4).toLongLong(), query.value(5).toLongLong()};
        perFile[fromPath].append(hit);
    }

    beginBatch();
    const Parser parser;
    int rewritten = 0;

    const auto reindex = [&](const QString &absolutePath, const QString &text) {
        Metadata metadata = parser.parse(text, absolutePath);
        const QFileInfo info(absolutePath);
        metadata.mtime = info.lastModified().toMSecsSinceEpoch();
        metadata.size = info.size();
        metadata.hash = QCryptographicHash::hash(text.toUtf8(), QCryptographicHash::Sha1);
        put(metadata, text);
    };

    for (auto it = perFile.constBegin(); it != perFile.constEnd(); ++it) {
        const QString fromPath = it.key();
        // The renamed note itself has already moved on disk by the time this
        // runs; every other linking note is still where the index found it.
        const QString actualRelative = fromPath == oldRelative ? newRelative : fromPath;
        const QString absolutePath = vault.absolute(actualRelative);

        Note note;
        if (!Vault::read(absolutePath, note, error)) {
            endBatch();
            return -1;
        }

        QList<Hit> hits = it.value();
        // Last link first, so an earlier link's offsets stay valid while a
        // later one's replacement text changes length.
        std::sort(hits.begin(), hits.end(), [](const Hit &a, const Hit &b) {
            return a.targetRange.start > b.targetRange.start;
        });

        for (const Hit &hit : std::as_const(hits)) {
            // A wikilink's targetRange is the target alone. A Markdown link's
            // covers the whole URL, subpath included, since that is one
            // contiguous node in md4qt; only the part before it is the target.
            const qsizetype rawLength =
                hit.wiki ? hit.targetRange.length() : hit.targetRange.length() - hit.subpath.length();
            if (rawLength < 0 || hit.targetRange.start + rawLength > note.text.size())
                continue; // stale offset - the file moved under us; a rebuild will catch it

            const QString rawSlice = note.text.mid(hit.targetRange.start, rawLength);
            const QString decoded =
                hit.wiki ? rawSlice : QUrl::fromPercentEncoding(rawSlice.toUtf8());

            QString rebased = rebasedTarget(decoded, oldRelative, newRelative);
            if (!hit.wiki)
                rebased = QString::fromUtf8(QUrl::toPercentEncoding(rebased, "/"));

            note.text.replace(hit.targetRange.start, rawLength, rebased);
        }

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
    return rewritten;
}

// --- graph view (libs/ext/graph) ------------------------------------------
// Added for the optional graph module; see the matching block at the end of
// Index.h. Two queries total, matching the "no per-note queries" budget the
// rest of Index already holds itself to (put()/remove() are the exception,
// and only because a write genuinely is per-note).

Index::GraphData Index::graphData() const
{
    GraphData result;
    if (!m_db.isOpen())
        return result;

    // Files, with each one's tags concatenated in the same query - the
    // second query below is the only other one this method runs. Char 30
    // (ASCII record separator) is not a character a tag can contain.
    QSqlQuery files(m_db);
    files.exec(QStringLiteral(
        "SELECT files.path, files.words, GROUP_CONCAT(tags.name, char(30)) FROM files "
        "LEFT JOIN tags ON tags.file = files.id GROUP BY files.id ORDER BY files.path"));
    while (files.next()) {
        GraphFileInfo file;
        file.path = files.value(0).toString();
        file.isAttachment = files.value(1).toInt() < 0;
        const QString tags = files.value(2).toString();
        if (!tags.isEmpty())
            file.tags = tags.split(QChar(30), Qt::SkipEmptyParts);
        result.files.append(file);
    }

    const QDir vaultDir(m_vaultRoot);
    QSqlQuery links(m_db);
    links.exec(QStringLiteral(
        "SELECT src.path, links.target FROM links "
        "JOIN files AS src ON src.id = links.file WHERE links.external = 0"));
    while (links.next()) {
        GraphLinkInfo link;
        link.fromPath = links.value(0).toString();
        link.targetName = links.value(1).toString();
        // resolve() is an in-memory hash lookup (see m_names), not a query,
        // so running it once per link here still holds the "no per-note
        // queries" budget.
        link.toPath = resolve(link.targetName, vaultDir.absoluteFilePath(link.fromPath));
        result.links.append(link);
    }

    return result;
}

// --- Bases (libs/ext/bases) ---------------------------------------------
// Added for the optional Bases module; see the block comment in Index.h.
// Read-only, and never called from the indexing path - only when a .base
// file is opened, so it costs nothing in a build without the module and
// nothing at startup in a build with it.

QList<Index::BaseNote> Index::allNotesForBases() const
{
    QList<BaseNote> notes;
    if (!m_db.isOpen())
        return notes;

    QHash<qint64, qsizetype> rowOf; // file id -> index into notes
    QSqlQuery files(m_db);
    // Attachments carry no properties and are not what a base lists.
    files.exec(QStringLiteral("SELECT id, path, mtime, size FROM files WHERE words >= 0"));
    while (files.next()) {
        BaseNote note;
        const qint64 id = files.value(0).toLongLong();
        note.path = files.value(1).toString();
        note.mtime = files.value(2).toLongLong();
        note.size = files.value(3).toLongLong();
        rowOf.insert(id, notes.size());
        notes.append(std::move(note));
    }

    QSqlQuery props(m_db);
    props.exec(QStringLiteral("SELECT file, key, type, value FROM props"));
    while (props.next()) {
        const auto it = rowOf.constFind(props.value(0).toLongLong());
        if (it == rowOf.constEnd())
            continue;
        Property property;
        property.key = props.value(1).toString();
        property.type = static_cast<Property::Type>(props.value(2).toInt());
        // put() joins a list's values with '\n'; an empty join would come
        // back as one empty string instead of no values at all.
        const QString value = props.value(3).toString();
        if (!value.isEmpty())
            property.values = value.split(QLatin1Char('\n'));
        notes[it.value()].properties.append(property);
    }

    QSqlQuery tags(m_db);
    tags.exec(QStringLiteral("SELECT DISTINCT file, name FROM tags"));
    while (tags.next()) {
        const auto it = rowOf.constFind(tags.value(0).toLongLong());
        if (it != rowOf.constEnd())
            notes[it.value()].tags.append(tags.value(1).toString());
    }

    QSqlQuery links(m_db);
    links.exec(QStringLiteral("SELECT file, target FROM links WHERE external = 0 AND target != ''"));
    while (links.next()) {
        const auto it = rowOf.constFind(links.value(0).toLongLong());
        if (it != rowOf.constEnd())
            notes[it.value()].linkTargets.append(links.value(1).toString());
    }

    return notes;
}

} // namespace noteAppC
