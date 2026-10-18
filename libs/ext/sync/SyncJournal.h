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
        QByteArray localHash;     ///< fingerprint of what SyncScanner last saw on disk - scanner dedup only
        /// Keyed fingerprint of the content as of the last confirmed sync
        /// (the text of the last record applied, or of this device's own
        /// record once it came back - see SyncEngine::applyRecord). Keyed
        /// with contentKey (a per-device secret, journal meta "contentKey");
        /// baseDigest below is unkeyed. Deliberately a separate field from
        /// localHash and updated only by applyRecord, never by
        /// queueLocalChange: the two answer different questions ("have I
        /// already queued this content to push" vs "do local and the
        /// three-way merge base actually agree") and updating them
        /// together let a device's own local edit look "unchanged" to its
        /// own merge check the moment it was scanned, before the push
        /// that edit caused had even reached the server - a real bug
        /// caught by tests/test_sync.cpp's disjoint-line-merge case,
        /// where it silently discarded the other device's edit.
        QByteArray syncedHash;
        bool deleted = false;     ///< last known state was a delete
        /// Digest (SyncCrypto::digest) of what base/<entityID> holds: the
        /// last confirmed sync point. Moves only in SyncEngine::applyRecord.
        /// Empty on a row from before records carried digests.
        QByteArray baseDigest;
        /// Digest of the last state this device enqueued or applied: the
        /// parent the next record it writes carries.
        QByteArray headDigest;
    };

    /// One state of an entity this device applied from the server or wrote
    /// itself - what a record's `parent` names (docs/SYNC.md "Conflict rules").
    struct State {
        QByteArray digest;
        QByteArray keyedHash; ///< contentAddress of the same content: syncedHash once confirmed
        QString path;         ///< the entity's local path after this state was applied
        QString text;         ///< note body; empty for attachments
        bool own = false;     ///< enqueued here (true) or applied from the server
        qint64 seq = 0;       ///< server seq; 0 = not pushed or not yet acknowledged
    };

    struct OutboxRecord {
        qint64 seq = 0; ///< local outbox order, unrelated to the server's seq
        QByteArray entityID;
        QByteArray recordID;
        QByteArray itemsKeyID;
        QByteArray wrappedNoteKey;
        QByteArray payload; ///< sealed EntityRecord, ready to send as-is
        QByteArray digest;  ///< the record's content digest, to put the server seq on its state
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

    // --- states: the last kStateHistory per entity, merge bases for records
    //     written against a state this device has since moved past ------

    static constexpr int kStateHistory = 32;
    /// Inserts, then prunes the entity to its newest kStateHistory rows.
    void addState(const QByteArray &entityID, const State &state);
    /// The newest row with that digest.
    std::optional<State> state(const QByteArray &entityID, const QByteArray &digest) const;
    /// Debug counter, process-wide: how many state() calls found no row for
    /// a non-empty digest. A miss means the history was too short (or the
    /// engine looked in the wrong place); tests assert it stays 0.
    static int stateMisses();
    static void resetStateMisses();
    /// Whether this entity's local path was `path` after the state `digest`.
    bool sawPath(const QByteArray &entityID, const QByteArray &digest, const QString &path) const;
    /// Own rows only: a state applied from the server already has its seq.
    void setStateSeq(const QByteArray &entityID, const QByteArray &digest, qint64 seq);

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
