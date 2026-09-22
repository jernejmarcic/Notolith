#include "Index.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QUrl>
#include <QUuid>
#include <QVariant>

namespace noteAppC {

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

    if (!createSchema())
        return false;

    loadNames();
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
                       "target TEXT, subpath TEXT, alias TEXT, embed INTEGER, wiki INTEGER, "
                       "external INTEGER, start INTEGER, finish INTEGER)"),
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
        insert(QStringLiteral("INSERT INTO links(file, target, subpath, alias, embed, wiki, "
                              "external, start, finish) VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?)"),
               {fileId, link.target, link.subpath, link.alias, link.embed, link.wiki,
                link.external, link.range.start, link.range.end});
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

    insert(QStringLiteral("INSERT INTO content(path, body) VALUES(?, ?)"), {path, body});

    if (!m_inBatch)
        emit changed();
}

void Index::remove(const QString &path)
{
    if (!m_db.isOpen())
        return;
    const QString relative = QDir(m_vaultRoot).relativeFilePath(path);

    QSqlQuery query(m_db);
    query.prepare(QStringLiteral("DELETE FROM files WHERE path = ?"));
    query.addBindValue(relative);
    query.exec();

    query.prepare(QStringLiteral("DELETE FROM content WHERE path = ?"));
    query.addBindValue(relative);
    query.exec();

    forgetNames(relative);
}

int Index::noteCount() const
{
    QSqlQuery query(m_db);
    query.exec(QStringLiteral("SELECT COUNT(*) FROM files"));
    return query.next() ? query.value(0).toInt() : 0;
}

QStringList Index::allPaths() const
{
    QStringList paths;
    QSqlQuery query(m_db);
    query.exec(QStringLiteral("SELECT path FROM files ORDER BY path"));
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
    const QString relative = QDir(m_vaultRoot).relativeFilePath(path);
    QList<Backlink> found;

    QSqlQuery query(m_db);
    query.prepare(QStringLiteral(
        "SELECT files.path, links.target, links.subpath, links.alias, links.embed, "
        "links.wiki, links.start, links.finish FROM links "
        "JOIN files ON files.id = links.file WHERE links.external = 0"));
    query.exec();
    while (query.next()) {
        const QString fromPath = query.value(0).toString();
        const QString target = query.value(1).toString();
        if (target.isEmpty())
            continue; // a link into the note's own headings
        if (resolve(target, QDir(m_vaultRoot).absoluteFilePath(fromPath)) != relative)
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
        "WHERE names.folded LIKE ? ORDER BY length(files.path), files.path LIMIT ?"));
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

} // namespace noteAppC
