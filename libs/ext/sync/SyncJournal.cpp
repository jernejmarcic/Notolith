#include "SyncJournal.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QSaveFile>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QUuid>

#include <atomic>

#include "SyncCrypto.h"

namespace noteAppC {

namespace {

std::atomic<int> g_stateMisses{0}; ///< see SyncJournal::stateMisses

bool exec(QSqlQuery &query, const QString &sql)
{
    return query.exec(sql);
}

} // namespace

QString SyncJournal::dataDirectoryFor(const QString &vaultRoot)
{
    // Same digest shape as NoteHistory::repositoryPathFor (SHA-256 of the
    // absolute path, hex, first 16 chars) - not shared code, because
    // libs/ext/history is a separate optional module that may not be built
    // at all, but the two are meant to agree on which directory names a
    // vault, so the three lines are duplicated deliberately rather than
    // coupled.
    const QByteArray digest =
        QCryptographicHash::hash(QDir(vaultRoot).absolutePath().toUtf8(), QCryptographicHash::Sha256)
            .toHex()
            .left(16);
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)
         + QStringLiteral("/noteAppC/sync/") + QString::fromLatin1(digest);
}

SyncJournal::SyncJournal(const QString &vaultRoot)
{
    open(vaultRoot);
}

SyncJournal::~SyncJournal()
{
    m_db = QSqlDatabase();
    if (QSqlDatabase::contains(m_connectionName))
        QSqlDatabase::removeDatabase(m_connectionName);
}

void SyncJournal::open(const QString &vaultRoot)
{
    m_dataDirectory = dataDirectoryFor(vaultRoot);
    QDir().mkpath(m_dataDirectory);
    QDir().mkpath(m_dataDirectory + QStringLiteral("/base"));

    m_connectionName = QStringLiteral("noteAppC-sync-%1")
                           .arg(reinterpret_cast<quintptr>(this), 0, 16);
    m_db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_connectionName);
    m_db.setDatabaseName(m_dataDirectory + QStringLiteral("/journal.db"));
    if (!m_db.open()) {
        m_error = m_db.lastError().text();
        return;
    }

    QSqlQuery q(m_db);
    exec(q, QStringLiteral("PRAGMA journal_mode=WAL"));
    exec(q, QStringLiteral("PRAGMA foreign_keys=ON"));
    const bool ok =
        exec(q, QStringLiteral("CREATE TABLE IF NOT EXISTS meta ("
                               "key TEXT PRIMARY KEY, value BLOB)"))
        && exec(q, QStringLiteral("CREATE TABLE IF NOT EXISTS entities ("
                                  "entityID BLOB PRIMARY KEY, path TEXT, entityType TEXT, "
                                  "itemsKeyID BLOB, wrappedNoteKey BLOB, "
                                  "localMtime INTEGER, localSize INTEGER, localHash BLOB, syncedHash BLOB, "
                                  "deleted INTEGER)"))
        && exec(q, QStringLiteral("CREATE INDEX IF NOT EXISTS entities_path ON entities(path)"))
        && exec(q, QStringLiteral("CREATE TABLE IF NOT EXISTS outbox ("
                                  "seq INTEGER PRIMARY KEY AUTOINCREMENT, entityID BLOB, recordID BLOB, "
                                  "itemsKeyID BLOB, wrappedNoteKey BLOB, payload BLOB, sent INTEGER DEFAULT 0)"))
        && exec(q, QStringLiteral("CREATE TABLE IF NOT EXISTS items_keys ("
                                  "itemsKeyID BLOB PRIMARY KEY, plaintext BLOB, active INTEGER)"))
        && exec(q, QStringLiteral("CREATE TABLE IF NOT EXISTS states ("
                                  "id INTEGER PRIMARY KEY AUTOINCREMENT, entityID BLOB, digest BLOB, "
                                  "keyedHash BLOB, path TEXT, text BLOB, own INTEGER, seq INTEGER DEFAULT 0)"))
        && exec(q, QStringLiteral("CREATE INDEX IF NOT EXISTS states_entity ON states(entityID, digest)"))
        && exec(q, QStringLiteral("CREATE TABLE IF NOT EXISTS parked ("
                                  "seq INTEGER PRIMARY KEY, entityID BLOB, recordID BLOB, deviceID BLOB, "
                                  "itemsKeyID BLOB, wrappedNoteKey BLOB, payload BLOB, path TEXT)"));
    if (!ok)
        m_error = q.lastError().text();
    // A journal from before records carried digests: the columns are added,
    // and a database that already has them answers "duplicate column", which
    // is the point and not an error.
    exec(q, QStringLiteral("ALTER TABLE entities ADD COLUMN baseDigest BLOB"));
    exec(q, QStringLiteral("ALTER TABLE entities ADD COLUMN headDigest BLOB"));
    exec(q, QStringLiteral("ALTER TABLE outbox ADD COLUMN digest BLOB"));
}

bool SyncJournal::isOpen() const
{
    return m_db.isOpen();
}

void SyncJournal::setMeta(const QString &key, const QByteArray &value)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("INSERT INTO meta(key, value) VALUES(?, ?) "
                             "ON CONFLICT(key) DO UPDATE SET value=excluded.value"));
    q.addBindValue(key);
    q.addBindValue(value);
    q.exec();
}

QByteArray SyncJournal::meta(const QString &key) const
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT value FROM meta WHERE key=?"));
    q.addBindValue(key);
    if (q.exec() && q.next())
        return q.value(0).toByteArray();
    return {};
}

QByteArray SyncJournal::deviceID()
{
    QByteArray id = meta(QStringLiteral("deviceID"));
    if (id.size() != SyncCrypto::kIDBytes) {
        id = SyncCrypto::randomID();
        setMeta(QStringLiteral("deviceID"), id);
    }
    return id;
}

qint64 SyncJournal::lastPulledSeq() const
{
    const QByteArray raw = meta(QStringLiteral("lastPulledSeq"));
    return raw.isEmpty() ? 0 : raw.toLongLong();
}

void SyncJournal::setLastPulledSeq(qint64 seq)
{
    setMeta(QStringLiteral("lastPulledSeq"), QByteArray::number(seq));
}

namespace {

SyncJournal::Entity entityFromQuery(const QSqlQuery &q)
{
    SyncJournal::Entity entity;
    entity.entityID = q.value(0).toByteArray();
    entity.path = q.value(1).toString();
    entity.entityType = q.value(2).toString();
    entity.itemsKeyID = q.value(3).toByteArray();
    entity.wrappedNoteKey = q.value(4).toByteArray();
    entity.localMtime = q.value(5).toLongLong();
    entity.localSize = q.value(6).toLongLong();
    entity.localHash = q.value(7).toByteArray();
    entity.syncedHash = q.value(8).toByteArray();
    entity.deleted = q.value(9).toInt() != 0;
    entity.baseDigest = q.value(10).toByteArray();
    entity.headDigest = q.value(11).toByteArray();
    return entity;
}

constexpr const char *kEntityColumns = "entityID, path, entityType, itemsKeyID, wrappedNoteKey, localMtime, "
                                       "localSize, localHash, syncedHash, deleted, baseDigest, headDigest";

} // namespace

std::optional<SyncJournal::Entity> SyncJournal::entity(const QByteArray &entityID) const
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT %1 FROM entities WHERE entityID=?").arg(QLatin1String(kEntityColumns)));
    q.addBindValue(entityID);
    if (q.exec() && q.next())
        return entityFromQuery(q);
    return std::nullopt;
}

std::optional<SyncJournal::Entity> SyncJournal::entityForPath(const QString &path) const
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT %1 FROM entities WHERE path=? AND deleted=0")
                  .arg(QLatin1String(kEntityColumns)));
    q.addBindValue(path);
    if (q.exec() && q.next())
        return entityFromQuery(q);
    return std::nullopt;
}

QList<SyncJournal::Entity> SyncJournal::allEntities() const
{
    QList<Entity> out;
    QSqlQuery q(m_db);
    q.exec(QStringLiteral("SELECT %1 FROM entities").arg(QLatin1String(kEntityColumns)));
    while (q.next())
        out.append(entityFromQuery(q));
    return out;
}

void SyncJournal::putEntity(const Entity &entity)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "INSERT INTO entities(entityID, path, entityType, itemsKeyID, wrappedNoteKey, "
        "localMtime, localSize, localHash, syncedHash, deleted, baseDigest, headDigest) "
        "VALUES(?,?,?,?,?,?,?,?,?,?,?,?) "
        "ON CONFLICT(entityID) DO UPDATE SET path=excluded.path, entityType=excluded.entityType, "
        "itemsKeyID=excluded.itemsKeyID, wrappedNoteKey=excluded.wrappedNoteKey, "
        "localMtime=excluded.localMtime, localSize=excluded.localSize, localHash=excluded.localHash, "
        "syncedHash=excluded.syncedHash, deleted=excluded.deleted, "
        "baseDigest=excluded.baseDigest, headDigest=excluded.headDigest"));
    q.addBindValue(entity.entityID);
    q.addBindValue(entity.path);
    q.addBindValue(entity.entityType);
    q.addBindValue(entity.itemsKeyID);
    q.addBindValue(entity.wrappedNoteKey);
    q.addBindValue(entity.localMtime);
    q.addBindValue(entity.localSize);
    q.addBindValue(entity.localHash);
    q.addBindValue(entity.syncedHash);
    q.addBindValue(entity.deleted ? 1 : 0);
    q.addBindValue(entity.baseDigest);
    q.addBindValue(entity.headDigest);
    q.exec();
}

void SyncJournal::removeEntity(const QByteArray &entityID)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("DELETE FROM entities WHERE entityID=?"));
    q.addBindValue(entityID);
    q.exec();
    QSqlQuery states(m_db);
    states.prepare(QStringLiteral("DELETE FROM states WHERE entityID=?"));
    states.addBindValue(entityID);
    states.exec();
    removeBase(entityID);
}

void SyncJournal::addState(const QByteArray &entityID, const State &state)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("INSERT INTO states(entityID, digest, keyedHash, path, text, own, seq) "
                             "VALUES(?,?,?,?,?,?,?)"));
    q.addBindValue(entityID);
    q.addBindValue(state.digest);
    q.addBindValue(state.keyedHash);
    q.addBindValue(state.path);
    q.addBindValue(state.text.toUtf8());
    q.addBindValue(state.own ? 1 : 0);
    q.addBindValue(state.seq);
    q.exec();
    // Limit: 32 states per note; a device offline longer than that for one note falls back to a conflict copy, never to loss
    QSqlQuery prune(m_db);
    prune.prepare(QStringLiteral("DELETE FROM states WHERE entityID=? AND id NOT IN "
                                 "(SELECT id FROM states WHERE entityID=? ORDER BY id DESC LIMIT ?)"));
    prune.addBindValue(entityID);
    prune.addBindValue(entityID);
    prune.addBindValue(kStateHistory);
    prune.exec();
}

int SyncJournal::stateMisses() { return g_stateMisses; }
void SyncJournal::resetStateMisses() { g_stateMisses = 0; }

std::optional<SyncJournal::State> SyncJournal::state(const QByteArray &entityID, const QByteArray &digest) const
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT digest, keyedHash, path, text, own, seq FROM states "
                             "WHERE entityID=? AND digest=? ORDER BY id DESC LIMIT 1"));
    q.addBindValue(entityID);
    q.addBindValue(digest);
    if (!q.exec() || !q.next()) {
        if (!digest.isEmpty())
            ++g_stateMisses;
        return std::nullopt;
    }
    return State{q.value(0).toByteArray(), q.value(1).toByteArray(),       q.value(2).toString(),
                 QString::fromUtf8(q.value(3).toByteArray()), q.value(4).toInt() != 0, q.value(5).toLongLong()};
}

bool SyncJournal::sawPath(const QByteArray &entityID, const QByteArray &digest, const QString &path) const
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT 1 FROM states WHERE entityID=? AND digest=? AND path=? LIMIT 1"));
    q.addBindValue(entityID);
    q.addBindValue(digest);
    q.addBindValue(path);
    return q.exec() && q.next();
}

qint64 SyncJournal::firstSeq(const QByteArray &entityID) const
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT MIN(seq) FROM states WHERE entityID=? AND seq>0"));
    q.addBindValue(entityID);
    return q.exec() && q.next() ? q.value(0).toLongLong() : 0; // NULL reads as 0
}

void SyncJournal::setStateSeq(const QByteArray &entityID, const QByteArray &digest, qint64 seq)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("UPDATE states SET seq=? WHERE id=(SELECT MAX(id) FROM states "
                             "WHERE entityID=? AND digest=? AND own=1)"));
    q.addBindValue(seq);
    q.addBindValue(entityID);
    q.addBindValue(digest);
    q.exec();
}

QString SyncJournal::readBase(const QByteArray &entityID) const
{
    QFile f(m_dataDirectory + QStringLiteral("/base/") + QString::fromLatin1(entityID.toHex()));
    if (!f.open(QIODevice::ReadOnly))
        return {};
    return QString::fromUtf8(f.readAll());
}

void SyncJournal::writeBase(const QByteArray &entityID, const QString &text)
{
    // QSaveFile, not truncate-then-write: a crash in between would leave an
    // empty base, and the next cycle would turn a two-device edit into a
    // conflict copy instead of a clean three-way merge. On any failure the
    // previous base stays.
    QSaveFile f(m_dataDirectory + QStringLiteral("/base/") + QString::fromLatin1(entityID.toHex()));
    if (f.open(QIODevice::WriteOnly)) {
        f.write(text.toUtf8());
        f.commit();
    }
}

void SyncJournal::removeBase(const QByteArray &entityID)
{
    QFile::remove(m_dataDirectory + QStringLiteral("/base/") + QString::fromLatin1(entityID.toHex()));
}

void SyncJournal::enqueue(const OutboxRecord &record)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("INSERT INTO outbox(entityID, recordID, itemsKeyID, wrappedNoteKey, payload, digest) "
                             "VALUES(?,?,?,?,?,?)"));
    q.addBindValue(record.entityID);
    q.addBindValue(record.recordID);
    q.addBindValue(record.itemsKeyID);
    q.addBindValue(record.wrappedNoteKey);
    q.addBindValue(record.payload);
    q.addBindValue(record.digest);
    q.exec();
}

QList<SyncJournal::OutboxRecord> SyncJournal::pendingOutbox(int limit) const
{
    QList<OutboxRecord> out;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT seq, entityID, recordID, itemsKeyID, wrappedNoteKey, payload, digest "
                             "FROM outbox WHERE sent=0 ORDER BY seq ASC LIMIT ?"));
    q.addBindValue(limit);
    q.exec();
    while (q.next()) {
        OutboxRecord record;
        record.seq = q.value(0).toLongLong();
        record.entityID = q.value(1).toByteArray();
        record.recordID = q.value(2).toByteArray();
        record.itemsKeyID = q.value(3).toByteArray();
        record.wrappedNoteKey = q.value(4).toByteArray();
        record.payload = q.value(5).toByteArray();
        record.digest = q.value(6).toByteArray();
        out.append(record);
    }
    return out;
}

void SyncJournal::markSent(const QList<QByteArray> &recordIDs)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("DELETE FROM outbox WHERE recordID=?"));
    for (const QByteArray &id : recordIDs) {
        q.addBindValue(id);
        q.exec();
    }
}

int SyncJournal::outboxSize() const
{
    QSqlQuery q(m_db);
    q.exec(QStringLiteral("SELECT COUNT(*) FROM outbox WHERE sent=0"));
    return q.next() ? q.value(0).toInt() : 0;
}

void SyncJournal::park(const SyncClient::JournalRecord &record, const QString &path)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("INSERT OR REPLACE INTO parked(seq, entityID, recordID, deviceID, itemsKeyID, "
                             "wrappedNoteKey, payload, path) VALUES(?,?,?,?,?,?,?,?)"));
    q.addBindValue(record.seq);
    q.addBindValue(record.entityID);
    q.addBindValue(record.recordID);
    q.addBindValue(record.deviceID);
    q.addBindValue(record.itemsKeyID);
    q.addBindValue(record.wrappedNoteKey);
    q.addBindValue(record.payload);
    q.addBindValue(path);
    q.exec();
}

QList<SyncJournal::ParkedRecord> SyncJournal::parkedRecords() const
{
    QList<ParkedRecord> out;
    QSqlQuery q(m_db);
    q.exec(QStringLiteral("SELECT seq, entityID, recordID, deviceID, itemsKeyID, wrappedNoteKey, payload, path "
                          "FROM parked ORDER BY seq ASC"));
    while (q.next()) {
        ParkedRecord parked;
        parked.record.seq = q.value(0).toLongLong();
        parked.record.entityID = q.value(1).toByteArray();
        parked.record.recordID = q.value(2).toByteArray();
        parked.record.deviceID = q.value(3).toByteArray();
        parked.record.itemsKeyID = q.value(4).toByteArray();
        parked.record.wrappedNoteKey = q.value(5).toByteArray();
        parked.record.payload = q.value(6).toByteArray();
        parked.path = q.value(7).toString();
        out.append(parked);
    }
    return out;
}

void SyncJournal::unpark(qint64 seq)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("DELETE FROM parked WHERE seq=?"));
    q.addBindValue(seq);
    q.exec();
}

void SyncJournal::unparkEntity(const QByteArray &entityID)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("DELETE FROM parked WHERE entityID=?"));
    q.addBindValue(entityID);
    q.exec();
}

void SyncJournal::putItemsKey(const ItemsKeyRecord &key)
{
    QSqlQuery q(m_db);
    if (key.active) {
        QSqlQuery clear(m_db);
        clear.exec(QStringLiteral("UPDATE items_keys SET active=0"));
    }
    q.prepare(QStringLiteral("INSERT INTO items_keys(itemsKeyID, plaintext, active) VALUES(?,?,?) "
                             "ON CONFLICT(itemsKeyID) DO UPDATE SET plaintext=excluded.plaintext, "
                             "active=excluded.active"));
    q.addBindValue(key.itemsKeyID);
    q.addBindValue(key.plaintext);
    q.addBindValue(key.active ? 1 : 0);
    q.exec();
}

std::optional<SyncJournal::ItemsKeyRecord> SyncJournal::activeItemsKey() const
{
    QSqlQuery q(m_db);
    q.exec(QStringLiteral("SELECT itemsKeyID, plaintext FROM items_keys WHERE active=1 LIMIT 1"));
    if (q.next())
        return ItemsKeyRecord{q.value(0).toByteArray(), q.value(1).toByteArray(), true};
    return std::nullopt;
}

std::optional<SyncJournal::ItemsKeyRecord> SyncJournal::itemsKey(const QByteArray &itemsKeyID) const
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT itemsKeyID, plaintext, active FROM items_keys WHERE itemsKeyID=?"));
    q.addBindValue(itemsKeyID);
    q.exec();
    if (q.next())
        return ItemsKeyRecord{q.value(0).toByteArray(), q.value(1).toByteArray(), q.value(2).toInt() != 0};
    return std::nullopt;
}

QList<SyncJournal::ItemsKeyRecord> SyncJournal::allItemsKeys() const
{
    QList<ItemsKeyRecord> out;
    QSqlQuery q(m_db);
    q.exec(QStringLiteral("SELECT itemsKeyID, plaintext, active FROM items_keys"));
    while (q.next())
        out.append(ItemsKeyRecord{q.value(0).toByteArray(), q.value(1).toByteArray(), q.value(2).toInt() != 0});
    return out;
}

} // namespace noteAppC
