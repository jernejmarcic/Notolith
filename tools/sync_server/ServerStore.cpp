// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej
#include "ServerStore.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSqlError>
#include <QSqlQuery>

#include "SyncCrypto.h"

namespace noteAppC {

namespace {

bool exec(QSqlQuery &q, const QString &sql)
{
    return q.exec(sql);
}

} // namespace

ServerStore::ServerStore(const QString &dataDir)
    : m_dataDir(dataDir)
{
    QDir().mkpath(m_dataDir);
    QDir().mkpath(m_dataDir + QStringLiteral("/blobs"));
    // A fixed connection name would collide the moment a second ServerStore
    // exists in the same process (every test in tests/test_sync.cpp makes
    // one) - same reasoning as SyncJournal's per-instance name.
    m_connectionName =
        QStringLiteral("noteAppC-sync-server-%1").arg(reinterpret_cast<quintptr>(this), 0, 16);
    m_db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_connectionName);
    m_db.setDatabaseName(m_dataDir + QStringLiteral("/server.db"));
    if (!m_db.open()) {
        m_error = m_db.lastError().text();
        return;
    }
    QSqlQuery q(m_db);
    exec(q, QStringLiteral("PRAGMA journal_mode=WAL"));
    exec(q, QStringLiteral("PRAGMA foreign_keys=ON"));
    const bool ok =
        exec(q, QStringLiteral("CREATE TABLE IF NOT EXISTS accounts ("
                               "accountID BLOB PRIMARY KEY, email TEXT UNIQUE, salt BLOB, loginHash BLOB)"))
        && exec(q, QStringLiteral("CREATE TABLE IF NOT EXISTS sessions ("
                                  "tokenHash BLOB PRIMARY KEY, accountID BLOB, expiresAt INTEGER)"))
        && exec(q, QStringLiteral("CREATE TABLE IF NOT EXISTS vaults (vaultID BLOB PRIMARY KEY, accountID BLOB)"))
        && exec(q, QStringLiteral("CREATE TABLE IF NOT EXISTS devices ("
                                  "vaultID BLOB, deviceID BLOB, publicKey BLOB, approved INTEGER, "
                                  "PRIMARY KEY(vaultID, deviceID))"))
        // Which devices enrol by lockbox, and so wait for a person to approve
        // them; a database from before this column gains it here.
        && (exec(q, QStringLiteral("ALTER TABLE devices ADD COLUMN lockbox INTEGER DEFAULT 0")) || true)
        && exec(q, QStringLiteral("CREATE TABLE IF NOT EXISTS items_keys ("
                                  "vaultID BLOB, itemsKeyID BLOB, wrapped BLOB, PRIMARY KEY(vaultID, itemsKeyID))"))
        && exec(q, QStringLiteral("CREATE TABLE IF NOT EXISTS lockboxes ("
                                  "vaultID BLOB, forDevice BLOB, itemsKeyID BLOB, sealed BLOB, "
                                  "PRIMARY KEY(vaultID, forDevice, itemsKeyID))"))
        && exec(q, QStringLiteral("CREATE TABLE IF NOT EXISTS journal ("
                                  "seq INTEGER PRIMARY KEY AUTOINCREMENT, vaultID BLOB, entityID BLOB, "
                                  "recordID BLOB, deviceID BLOB, itemsKeyID BLOB, wrappedNoteKey BLOB, "
                                  "payload BLOB, UNIQUE(deviceID, recordID))"))
        && exec(q, QStringLiteral("CREATE INDEX IF NOT EXISTS journal_vault_seq ON journal(vaultID, seq)"));
    if (!ok)
        m_error = q.lastError().text();
}

ServerStore::~ServerStore()
{
    m_db = QSqlDatabase();
    if (QSqlDatabase::contains(m_connectionName))
        QSqlDatabase::removeDatabase(m_connectionName);
}

bool ServerStore::isOpen() const
{
    return m_db.isOpen();
}

QByteArray ServerStore::saltFor(const QString &email) const
{
    if (const auto found = account(email))
        return found->salt;
    // Deterministic but indistinguishable from a real salt to anyone
    // without the server's own key - docs/SYNC.md: the response shape
    // must not reveal which emails are registered.
    static const QByteArray serverKey = QCryptographicHash::hash(
        QByteArrayLiteral("noteAppC-sync-server-salt-key"), QCryptographicHash::Sha256);
    return SyncCrypto::contentAddress(serverKey, email.toUtf8()).left(SyncCrypto::kSaltBytes);
}

std::optional<ServerStore::Account> ServerStore::account(const QString &email) const
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT accountID, email, salt, loginHash FROM accounts WHERE email=?"));
    q.addBindValue(email);
    if (q.exec() && q.next())
        return Account{q.value(0).toByteArray(), q.value(1).toString(), q.value(2).toByteArray(),
                      q.value(3).toByteArray()};
    return std::nullopt;
}

std::optional<ServerStore::Account> ServerStore::accountByID(const QByteArray &accountID) const
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT accountID, email, salt, loginHash FROM accounts WHERE accountID=?"));
    q.addBindValue(accountID);
    if (q.exec() && q.next())
        return Account{q.value(0).toByteArray(), q.value(1).toString(), q.value(2).toByteArray(),
                      q.value(3).toByteArray()};
    return std::nullopt;
}

bool ServerStore::createAccount(const QString &email, const QByteArray &salt, const QByteArray &loginKey,
                                QByteArray *accountID, QString *error)
{
    if (account(email)) {
        if (error)
            *error = QStringLiteral("an account already exists for this email");
        return false;
    }
    const QByteArray hash = SyncCrypto::hashLoginKey(loginKey);
    if (hash.isEmpty()) {
        if (error)
            *error = QStringLiteral("could not hash the login key");
        return false;
    }
    const QByteArray id = SyncCrypto::randomID();
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("INSERT INTO accounts(accountID, email, salt, loginHash) VALUES(?,?,?,?)"));
    q.addBindValue(id);
    q.addBindValue(email);
    q.addBindValue(salt);
    q.addBindValue(hash);
    if (!q.exec()) {
        if (error)
            *error = q.lastError().text();
        return false;
    }
    if (accountID)
        *accountID = id;
    return true;
}

bool ServerStore::updatePassword(const QByteArray &accountID, const QByteArray &newSalt,
                                 const QByteArray &newLoginKey)
{
    const QByteArray hash = SyncCrypto::hashLoginKey(newLoginKey);
    if (hash.isEmpty())
        return false;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("UPDATE accounts SET salt=?, loginHash=? WHERE accountID=?"));
    q.addBindValue(newSalt);
    q.addBindValue(hash);
    q.addBindValue(accountID);
    return q.exec() && q.numRowsAffected() == 1;
}

bool ServerStore::loginRateLimited(const QString &email)
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    constexpr qint64 kWindowMs = 60'000;
    constexpr int kMaxAttempts = 10;
    QList<qint64> &attempts = m_loginAttempts[email];
    while (!attempts.isEmpty() && attempts.first() < now - kWindowMs)
        attempts.removeFirst();
    attempts.append(now);
    return attempts.size() > kMaxAttempts;
}

QByteArray ServerStore::createSession(const QByteArray &accountID)
{
    const QByteArray token = SyncCrypto::randomKey();
    const QByteArray hash = QCryptographicHash::hash(token, QCryptographicHash::Sha256);
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("INSERT INTO sessions(tokenHash, accountID, expiresAt) VALUES(?,?,?)"));
    q.addBindValue(hash);
    q.addBindValue(accountID);
    q.addBindValue(QDateTime::currentMSecsSinceEpoch() + qint64(30) * 24 * 3600 * 1000); // 30 days
    q.exec();
    return token;
}

std::optional<QByteArray> ServerStore::accountForToken(const QByteArray &token) const
{
    const QByteArray hash = QCryptographicHash::hash(token, QCryptographicHash::Sha256);
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT accountID, expiresAt FROM sessions WHERE tokenHash=?"));
    q.addBindValue(hash);
    if (q.exec() && q.next()) {
        if (q.value(1).toLongLong() < QDateTime::currentMSecsSinceEpoch())
            return std::nullopt;
        return q.value(0).toByteArray();
    }
    return std::nullopt;
}

QByteArray ServerStore::createVault(const QByteArray &accountID)
{
    const QByteArray id = SyncCrypto::randomID();
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("INSERT INTO vaults(vaultID, accountID) VALUES(?,?)"));
    q.addBindValue(id);
    q.addBindValue(accountID);
    q.exec();
    return id;
}

QList<QByteArray> ServerStore::vaultsFor(const QByteArray &accountID) const
{
    QList<QByteArray> out;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT vaultID FROM vaults WHERE accountID=?"));
    q.addBindValue(accountID);
    q.exec();
    while (q.next())
        out << q.value(0).toByteArray();
    return out;
}

bool ServerStore::vaultBelongsTo(const QByteArray &vaultID, const QByteArray &accountID) const
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT 1 FROM vaults WHERE vaultID=? AND accountID=?"));
    q.addBindValue(vaultID);
    q.addBindValue(accountID);
    return q.exec() && q.next();
}

void ServerStore::registerDevice(const QByteArray &vaultID, const QByteArray &deviceID, const QByteArray &publicKey,
                                 bool wantsLockbox)
{
    QSqlQuery exists(m_db);
    exists.prepare(QStringLiteral("SELECT 1 FROM devices WHERE vaultID=? AND deviceID=?"));
    exists.addBindValue(vaultID);
    exists.addBindValue(deviceID);
    if (exists.exec() && exists.next()) {
        // Already registered - idempotent, but a device may switch paths.
        QSqlQuery update(m_db);
        update.prepare(QStringLiteral("UPDATE devices SET lockbox=? WHERE vaultID=? AND deviceID=?"));
        update.addBindValue(wantsLockbox ? 1 : 0);
        update.addBindValue(vaultID);
        update.addBindValue(deviceID);
        update.exec();
        return;
    }

    QSqlQuery count(m_db);
    count.prepare(QStringLiteral("SELECT COUNT(*) FROM devices WHERE vaultID=?"));
    count.addBindValue(vaultID);
    count.exec();
    const bool firstDevice = count.next() && count.value(0).toInt() == 0;

    QSqlQuery insert(m_db);
    insert.prepare(
        QStringLiteral("INSERT INTO devices(vaultID, deviceID, publicKey, approved, lockbox) VALUES(?,?,?,?,?)"));
    insert.addBindValue(vaultID);
    insert.addBindValue(deviceID);
    insert.addBindValue(publicKey);
    insert.addBindValue(firstDevice ? 1 : 0);
    insert.addBindValue(wantsLockbox ? 1 : 0);
    insert.exec();
}

QList<ServerStore::Device> ServerStore::devices(const QByteArray &vaultID) const
{
    QList<Device> out;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT deviceID, publicKey, approved, lockbox FROM devices WHERE vaultID=?"));
    q.addBindValue(vaultID);
    q.exec();
    while (q.next())
        out << Device{q.value(0).toByteArray(), q.value(1).toByteArray(), q.value(2).toInt() != 0,
                      q.value(3).toInt() != 0};
    return out;
}

bool ServerStore::isApproved(const QByteArray &vaultID, const QByteArray &deviceID) const
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT approved FROM devices WHERE vaultID=? AND deviceID=?"));
    q.addBindValue(vaultID);
    q.addBindValue(deviceID);
    return q.exec() && q.next() && q.value(0).toInt() != 0;
}

bool ServerStore::putItemsKey(const QByteArray &vaultID, const QByteArray &itemsKeyID, const QByteArray &wrapped)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("INSERT INTO items_keys(vaultID, itemsKeyID, wrapped) VALUES(?,?,?) "
                             "ON CONFLICT(vaultID, itemsKeyID) DO UPDATE SET wrapped=excluded.wrapped"));
    q.addBindValue(vaultID);
    q.addBindValue(itemsKeyID);
    q.addBindValue(wrapped);
    return q.exec();
}

QList<ServerStore::WrappedItemsKey> ServerStore::itemsKeys(const QByteArray &vaultID) const
{
    QList<WrappedItemsKey> out;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT itemsKeyID, wrapped FROM items_keys WHERE vaultID=?"));
    q.addBindValue(vaultID);
    q.exec();
    while (q.next())
        out << WrappedItemsKey{q.value(0).toByteArray(), q.value(1).toByteArray()};
    return out;
}

bool ServerStore::replaceItemsKeys(const QByteArray &vaultID, const QList<WrappedItemsKey> &keys)
{
    if (!m_db.transaction())
        return false;
    for (const WrappedItemsKey &key : keys) {
        if (!putItemsKey(vaultID, key.itemsKeyID, key.wrapped)) {
            m_db.rollback();
            return false;
        }
    }
    return m_db.commit();
}

void ServerStore::putLockbox(const QByteArray &vaultID, const QByteArray &forDevice, const QByteArray &itemsKeyID,
                             const QByteArray &sealed)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("INSERT INTO lockboxes(vaultID, forDevice, itemsKeyID, sealed) VALUES(?,?,?,?) "
                             "ON CONFLICT(vaultID, forDevice, itemsKeyID) DO UPDATE SET sealed=excluded.sealed"));
    q.addBindValue(vaultID);
    q.addBindValue(forDevice);
    q.addBindValue(itemsKeyID);
    q.addBindValue(sealed);
    q.exec();

    // An existing device sealing something for a pending one is what
    // "vouched for" means here (docs/SYNC.md) - approve it.
    QSqlQuery approve(m_db);
    approve.prepare(QStringLiteral("UPDATE devices SET approved=1 WHERE vaultID=? AND deviceID=?"));
    approve.addBindValue(vaultID);
    approve.addBindValue(forDevice);
    approve.exec();
}

QList<ServerStore::Lockbox> ServerStore::lockboxesFor(const QByteArray &vaultID, const QByteArray &forDevice) const
{
    QList<Lockbox> out;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT itemsKeyID, sealed FROM lockboxes WHERE vaultID=? AND forDevice=?"));
    q.addBindValue(vaultID);
    q.addBindValue(forDevice);
    q.exec();
    while (q.next())
        out << Lockbox{q.value(0).toByteArray(), q.value(1).toByteArray()};
    return out;
}

QList<qint64> ServerStore::appendJournal(const QByteArray &vaultID, const QList<JournalRow> &records)
{
    QList<qint64> seqs;
    m_db.transaction();
    for (const JournalRow &r : records) {
        QSqlQuery existing(m_db);
        existing.prepare(QStringLiteral("SELECT seq FROM journal WHERE deviceID=? AND recordID=?"));
        existing.addBindValue(r.deviceID);
        existing.addBindValue(r.recordID);
        if (existing.exec() && existing.next()) {
            seqs << existing.value(0).toLongLong(); // idempotent retry - same seq as the first attempt
            continue;
        }
        QSqlQuery insert(m_db);
        insert.prepare(QStringLiteral("INSERT INTO journal(vaultID, entityID, recordID, deviceID, itemsKeyID, "
                                      "wrappedNoteKey, payload) VALUES(?,?,?,?,?,?,?)"));
        insert.addBindValue(vaultID);
        insert.addBindValue(r.entityID);
        insert.addBindValue(r.recordID);
        insert.addBindValue(r.deviceID);
        insert.addBindValue(r.itemsKeyID);
        insert.addBindValue(r.wrappedNoteKey);
        insert.addBindValue(r.payload);
        insert.exec();
        seqs << insert.lastInsertId().toLongLong();
    }
    m_db.commit();
    return seqs;
}

QList<ServerStore::JournalRow> ServerStore::journalSince(const QByteArray &vaultID, qint64 since, int limit) const
{
    QList<JournalRow> out;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT seq, entityID, recordID, deviceID, itemsKeyID, wrappedNoteKey, payload "
                             "FROM journal WHERE vaultID=? AND seq>? ORDER BY seq ASC LIMIT ?"));
    q.addBindValue(vaultID);
    q.addBindValue(since);
    q.addBindValue(limit);
    q.exec();
    while (q.next()) {
        JournalRow r;
        r.seq = q.value(0).toLongLong();
        r.entityID = q.value(1).toByteArray();
        r.recordID = q.value(2).toByteArray();
        r.deviceID = q.value(3).toByteArray();
        r.itemsKeyID = q.value(4).toByteArray();
        r.wrappedNoteKey = q.value(5).toByteArray();
        r.payload = q.value(6).toByteArray();
        out << r;
    }
    return out;
}

QString ServerStore::blobPath(const QByteArray &vaultID, const QByteArray &address) const
{
    const QString addressHex = QString::fromLatin1(address.toHex());
    return QStringLiteral("%1/blobs/%2/%3/%4")
        .arg(m_dataDir, QString::fromLatin1(vaultID.toHex()), addressHex.left(2), addressHex);
}

bool ServerStore::putBlob(const QByteArray &vaultID, const QByteArray &address, const QByteArray &bytes)
{
    const QString path = blobPath(vaultID, address);
    if (QFile::exists(path))
        return true; // content-addressed: identical bytes already stored
    QDir().mkpath(QFileInfo(path).path());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly))
        return false;
    return f.write(bytes) == bytes.size();
}

std::optional<QByteArray> ServerStore::getBlob(const QByteArray &vaultID, const QByteArray &address) const
{
    QFile f(blobPath(vaultID, address));
    if (!f.open(QIODevice::ReadOnly))
        return std::nullopt;
    return f.readAll();
}

} // namespace noteAppC
