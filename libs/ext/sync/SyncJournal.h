#pragma once

#include <optional>

#include <QByteArray>
#include <QList>
#include <QSqlDatabase>
#include <QString>

namespace noteAppC {

/// The local half of the entity-change journal (docs/SYNC.md "The journal
/// protocol"): what has been synced, what is waiting to be pushed, the
/// itemsKeys this device holds, and the three-way merge base for every
/// note. Lives under $XDG_DATA_HOME/noteAppC/sync/<vault digest>/ - never
/// inside the vault, mirroring libs/ext/history's own rule (and reusing
/// NoteHistory::repositoryPathFor's digest so the two never disagree about
/// which directory names a vault).
///
/// Not thread-safe: like NoteHistory, one instance is used from one thread
/// (SyncEngine's worker thread owns it for the vault's lifetime).
class SyncJournal {
public:
    struct Entity {
        QByteArray entityID;      ///< stable, random, 16 B
        QString path;             ///< vault-relative, as last synced
        QString entityType;       ///< "note" | "attachment" | "folder"
        QByteArray itemsKeyID;    ///< which itemsKey wraps noteKey below
        QByteArray wrappedNoteKey;
        qint64 localMtime = 0;    ///< of the vault file, for the scanner's cheap check
        qint64 localSize = 0;
        QByteArray localHash;     ///< fingerprint of the last-synced plaintext
        bool deleted = false;     ///< last known state was a delete
    };

    struct OutboxRecord {
        qint64 seq = 0; ///< local outbox order, unrelated to the server's seq
        QByteArray entityID;
        QByteArray recordID;
        QByteArray itemsKeyID;
        QByteArray wrappedNoteKey;
        QByteArray payload; ///< sealed EntityRecord, ready to send as-is
    };

    struct ItemsKeyRecord {
        QByteArray itemsKeyID;
        QByteArray plaintext; ///< the 32 B key itself - see docs/SYNC.md
        bool active = false;
    };

    explicit SyncJournal(const QString &vaultRoot);
    ~SyncJournal();
    SyncJournal(const SyncJournal &) = delete;
    SyncJournal &operator=(const SyncJournal &) = delete;

    bool isOpen() const;
    QString error() const { return m_error; }

    /// Where this vault's sync state lives; public so tests can point it
    /// somewhere temporary and so SyncEngine can find the base/ directory.
    static QString dataDirectoryFor(const QString &vaultRoot);

    // --- small key/value state (device id, account, server URL, salt...) --

    void setMeta(const QString &key, const QByteArray &value);
    QByteArray meta(const QString &key) const;

    /// Generated once on first use and cached in meta("deviceID").
    QByteArray deviceID();

    qint64 lastPulledSeq() const;
    void setLastPulledSeq(qint64 seq);

    // --- entities: the steady state, as of the last successful sync -------

    std::optional<Entity> entity(const QByteArray &entityID) const;
    std::optional<Entity> entityForPath(const QString &path) const;
    QList<Entity> allEntities() const;
    void putEntity(const Entity &entity);
    void removeEntity(const QByteArray &entityID);

    // --- three-way merge base ------------------------------------------

    /// Empty when there is no base yet (a brand new entity).
    QString readBase(const QByteArray &entityID) const;
    void writeBase(const QByteArray &entityID, const QString &text);
    void removeBase(const QByteArray &entityID);

    // --- outbox: local changes waiting to be pushed ------------------

    void enqueue(const OutboxRecord &record);
    /// Oldest first, capped at `limit` (a push is one HTTP request's worth).
    QList<OutboxRecord> pendingOutbox(int limit) const;
    void markSent(const QList<QByteArray> &recordIDs);
    int outboxSize() const;

    // --- itemsKeys this device currently holds in the clear ------------

    void putItemsKey(const ItemsKeyRecord &key);
    std::optional<ItemsKeyRecord> activeItemsKey() const;
    std::optional<ItemsKeyRecord> itemsKey(const QByteArray &itemsKeyID) const;
    QList<ItemsKeyRecord> allItemsKeys() const;

private:
    void open(const QString &vaultRoot);
    QSqlDatabase m_db;
    QString m_connectionName;
    QString m_dataDirectory;
    QString m_error;
};

} // namespace noteAppC
