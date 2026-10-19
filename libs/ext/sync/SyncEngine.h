#pragma once

#include <functional>
#include <memory>

#include <QByteArray>
#include <QHash>
#include <QObject>
#include <QSet>
#include <QThread>
#include <QTimer>

#include "SecretStore.h"
#include "SyncClient.h"
#include "SyncScanner.h"
#include "Vault.h"

namespace noteAppC {

class SyncJournal;

/// The public face of sync (docs/SYNC.md): start/stop for a vault, sign-in
/// with a password-prompt callback, one status signal. Everything else -
/// the crypto, the journal, the scanner, the merge, the HTTP client - is
/// this class's private implementation detail, run entirely on its own
/// worker thread.
///
/// SyncEngine moves *itself* to `m_thread` in its constructor (the way a
/// QObject with no parent safely can) rather than splitting into a
/// public/worker pair: every method below queues onto that thread via
/// QMetaObject::invokeMethod and is safe to call from the UI thread, and
/// every signal is emitted from the worker thread, which Qt's queued
/// connections deliver back to whichever thread connected to them - a
/// UI slot connected the normal way needs nothing extra.
///
/// Driven by a debounced timer, not a loop: noteChanged() schedules a scan
/// a few seconds out and does no I/O itself (docs/PERFORMANCE.md's rule -
/// nothing on the keystroke path), and a scan that finds nothing changed
/// costs one stat() per vault file (SyncScanner) and no network call at
/// all.
class SyncEngine : public QObject {
    Q_OBJECT

public:
    /// NeedsPassword: no keys in the keyring for this device and the prompt
    /// gave no password - not a failure, the person has not signed in yet.
    enum class Status { Idle, SigningIn, Scanning, Syncing, WaitingForApproval, Error, NeedsPassword };
    Q_ENUM(Status)

    /// Returns the account password, or an empty string to cancel. Called
    /// from the engine's worker thread whenever nothing usable is cached
    /// for the session and the keyring holds no keys for this device
    /// (docs/SYNC.md "Keys kept in the keyring") - a UI supplies one that
    /// shows a modal on its own thread via a blocking queued invocation, or
    /// one that hands over a password it already asked for.
    using PasswordPrompt = std::function<QString(const QString &email)>;

    /// No QObject parent: SyncEngine moves itself to its own thread (see
    /// the class comment above), which Qt refuses for a parented object.
    /// Owned the way NoteHistory is (a std::shared_ptr in whatever holds
    /// it), not by Qt's parent/child tree.
    SyncEngine();
    ~SyncEngine() override;
    SyncEngine(const SyncEngine &) = delete;
    SyncEngine &operator=(const SyncEngine &) = delete;

    /// Starts syncing `vaultRoot` against `serverURL` for `email`. With
    /// `viaLockbox` true, this device never derives itemsKeys from the
    /// password even though it still authenticates with one (see
    /// docs/SYNC.md "Device enrolment with lockboxes") - it registers
    /// itself and waits for an already-approved device to seal it a
    /// lockbox. Safe to call from any thread.
    void start(const QString &vaultRoot, const QString &serverURL, const QString &email,
              PasswordPrompt prompt, bool viaLockbox = false);
    void stop();

    /// Called by the vault's file watcher/index on any change - schedules
    /// the next debounced scan. No I/O on this call.
    void noteChanged();

    /// Runs a cycle right away instead of waiting for the debounce or
    /// periodic timer - what the `sync:sync-now` command triggers, and
    /// what tests use so they are not stuck waiting out a multi-second
    /// timer for every assertion.
    void syncNow();

    /// Change the account password (docs/SYNC.md "Password change"): the
    /// caller must already be signed in this session.
    void changePassword(const QString &newPassword);

    /// Lets `deviceID` (as deviceAwaitingApproval named it) have this vault's
    /// keys: remembered for this vault on this device, and its lockboxes
    /// sealed on the spot. Only ever a person's decision.
    void approveDevice(const QByteArray &deviceID);

    /// Deletes this device's keys from the keyring (docs/SYNC.md "Keys kept
    /// in the keyring"), wipes the session's copies and stops the engine:
    /// the next start asks for the password again. A no-op without a
    /// running engine.
    void forgetStoredKeys();

    /// Where the device identity key is kept: the OS keyring unless one is
    /// set here. A test sets an in-memory one, so a run never writes to the
    /// user's real keyring, nor waits out a secret-service backend that does
    /// not answer. Process-wide; set it before any engine starts. Both
    /// functions run on engines' worker threads. `remove` may stay empty.
    using SecretStore = secrets::Store;
    static void setSecretStore(SecretStore store);

    /// Test seam: while set, records are written the way a client from before
    /// parents did - without `parent` and `hash`. Process-wide.
    static void setLegacyRecordFormatForTests(bool legacy);

signals:
    void statusChanged(noteAppC::SyncEngine::Status status, const QString &message);
    /// One pass of the sync cycle finished (pushed/pulled record counts;
    /// conflicts is how many records met an edit they could not be merged
    /// with - a conflict copy written, or this device's text replaced by the
    /// newer one - or a rename held back, or a note moved aside because another
    /// entity holds its path, this pass).
    void syncFinished(int pushed, int pulled, int conflicts);
    /// A device enrolling without the password wants this vault's keys.
    /// Nothing is sealed for it until approveDevice(): a person compares
    /// `fingerprint` with the code the new device shows. Once per device per
    /// session.
    void deviceAwaitingApproval(const QByteArray &deviceID, const QString &fingerprint);

private:
    void startOnWorker(QString vaultRoot, QString serverURL, QString email, PasswordPrompt prompt,
                       bool viaLockbox);
    void stopOnWorker();
    void runCycle();
    void setStatus(Status status, const QString &message = {});

    bool ensureSession();      ///< sign in / register, get a bearer token and vaultID
    bool loadDeviceIdentity(); ///< this device's X25519 keypair, from the OS keyring or freshly made
    bool deriveKeysFromPassword(QByteArray *salt, bool refreshSalt); ///< asks the prompt (NeedsPassword if none given), fetches the salt if missing or refreshSalt
    void adoptContentKey();    ///< contentKey is a per-device secret in journal meta, seeded once from the derived key
    bool loadStoredKeys();     ///< loginKey/wrapKey/contentKey from the keyring; false if absent or malformed
    void storeKeys() const;    ///< writes the session's three keys to the keyring (never the password)
    void dropStoredKeys() const;
    QString storedKeysName() const; ///< "keys/<server>/<device id>", this vault's entry
    bool ensureItemsKey();     ///< path 1 (wrapKey) or path 2 (lockbox), per m_viaLockbox
    void maintainEnrolment();  ///< seal lockboxes for approved devices, ask about the rest

    int pushOutbox();
    /// Empty, or why the pull stopped at a record it could not apply here
    /// (that record is pulled again next cycle).
    QString pullJournal(int *pulled, int *conflicts);
    /// False only when the record must be retried: a remote delete whose
    /// file could not be moved to the trash.
    bool applyRecord(const SyncClient::JournalRecord &record, int *pulled, int *conflicts, QString *error);
    /// Moves a deleted entity's file to the trash (unless edited locally) and
    /// forgets the entity. `parent` is the state the deleter saw (empty: not
    /// known); a delete older than this device's own sync point is ignored.
    /// False, with `error`, only when the trash move fails.
    bool applyRemoteDelete(const QByteArray &entityID, const QByteArray &parent, QString *error);
    /// Remote deletes that kept failing and stopped blocking the pull; kept in
    /// the journal's meta so a restart still retries them. Read from there
    /// once per cycle (loadPendingDeletes), written through by the setter.
    const QList<QByteArray> &pendingDeletes() const { return m_pendingDeletes; }
    void loadPendingDeletes();
    void setPendingDeletes(const QList<QByteArray> &ids);
    void retryPendingDeletes();
    QString pendingDeleteWarning() const; ///< empty when nothing is pending
    void refuse(const QString &path);     ///< names a path a record was dropped for (refusedWarning)
    QString refusedWarning() const;       ///< empty when no record was refused
    QString unreadableWarning() const;    ///< empty when no attachment was parked
    /// Fills *text (notes) or *raw (the rest) from the record's blob; an
    /// inline body is left in *text. False with *error: retry the record next
    /// cycle. False with *error empty: parked (fetch failures, in the
    /// journal's parked table; a blob this device cannot open, session-only
    /// in m_unreadable), skip it.
    bool loadRemoteContent(const SyncClient::JournalRecord &record, const QString &path, const QByteArray &blobAddress,
                           bool isNote, const QByteArray &noteKey, QString *text, QByteArray *raw, QString *error);
    /// Applies the parked records again, quietly: a fetch failure re-parks at
    /// once (no stall count, no Error status).
    void retryParked(int *pulled, int *conflicts);
    /// False when nothing was queued (a refused name, a note that cannot be
    /// read, a blob that would not upload).
    bool queueLocalChange(const SyncScanner::Change &change);
    /// Queues the entity as it is on disk right now as a local change (callers
    /// insert into m_toAnnounce; pullJournal() calls this once per entity).
    void announce(const QByteArray &entityID);

    QThread m_thread;
    QTimer *m_debounce = nullptr; ///< a change kicks this, single-shot, a few seconds out
    QTimer *m_periodic = nullptr; ///< catches remote changes even with no local edits

    std::unique_ptr<SyncJournal> m_journal;
    std::unique_ptr<SyncClient> m_client;
    Vault m_vault;
    QString m_email;
    PasswordPrompt m_prompt;
    bool m_viaLockbox = false;
    QString m_serverURL;
    bool m_storeTried = false;     ///< the keyring was asked once this start; a missing entry is not asked again
    bool m_keysFromStore = false;  ///< the keyring holds this session's keys (loaded from it, or stored once the server accepted them)
    bool m_forgotten = false;      ///< forgetStoredKeys() ran: stopping reports NeedsPassword, not Idle
    bool m_running = false;
    /// True for the whole duration of one runCycle(). A cycle spends most
    /// of its time inside a nested QEventLoop (SyncClient's HTTP calls,
    /// loadDeviceIdentity()'s keychain job) - the *same* worker thread's
    /// event loop, so a stop() or the destructor's
    /// Qt::BlockingQueuedConnection to stopOnWorker() would otherwise be
    /// delivered and processed reentrantly, mid-cycle, nulling m_journal
    /// and m_client out from under the call chain that is about to resume
    /// and use them. Caught by tests/test_sync.cpp under
    /// AddressSanitizer: a cycle stuck waiting on the OS keychain,
    /// interrupted by the test's own engine destructor firing on a
    /// timeout. stopOnWorker() checks this and defers the actual teardown
    /// - see its own comment.
    bool m_inCycle = false;

    // Session-only key material (docs/SYNC.md "Deriving masterKey") -
    // never written to disk, zeroed in stopOnWorker().
    QByteArray m_loginKey;
    QByteArray m_wrapKey;
    QByteArray m_contentKey;
    QByteArray m_vaultID;
    QByteArray m_boxSecretKey; ///< this device's lockbox identity, cached from the OS keyring
    QByteArray m_boxPublicKey;
    QList<QByteArray> m_pendingDeletes; ///< cache of the journal's pendingDeletes meta, loaded each cycle
    QHash<QByteArray, int> m_deleteFailures; ///< entityID -> cycles its remote delete has blocked the pull
    QHash<QByteArray, int> m_fetchStalls; ///< recordID -> cycles its blob has blocked the pull
    QStringList m_unreadable; ///< paths of records whose blob this device cannot open, kept for the session (fetch failures live in the journal)
    bool m_retryingParked = false; ///< retryParked() is applying: a fetch failure parks at once
    bool m_parkedNow = false;      ///< loadRemoteContent() parked the record applyRecord() was just given
    QStringList m_refusedPaths; ///< paths of records dropped for leaving the vault, kept for the session so the message stays
    QSet<QByteArray> m_toAnnounce; ///< entities whose on-disk text goes out as a record when this pull finishes
    QSet<QByteArray> m_announced; ///< devices deviceAwaitingApproval already named this session
};

} // namespace noteAppC

Q_DECLARE_METATYPE(noteAppC::SyncEngine::Status)
