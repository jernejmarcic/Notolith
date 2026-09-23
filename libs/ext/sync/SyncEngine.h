#pragma once

#include <functional>
#include <memory>

#include <QByteArray>
#include <QObject>
#include <QThread>
#include <QTimer>

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
    enum class Status { Idle, SigningIn, Scanning, Syncing, WaitingForApproval, Error };
    Q_ENUM(Status)

    /// Returns the account password, or an empty string to cancel. Called
    /// from the engine's worker thread whenever nothing usable is cached
    /// for the session (docs/SYNC.md "Deriving masterKey") - a UI supplies
    /// one that shows a modal on its own thread via a blocking queued
    /// invocation; see the report for the exact snippet.
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

signals:
    void statusChanged(noteAppC::SyncEngine::Status status, const QString &message);
    /// One pass of the sync cycle finished (pushed/pulled record counts;
    /// conflicts is how many notes got a conflict copy written this pass).
    void syncFinished(int pushed, int pulled, int conflicts);

private:
    void startOnWorker(QString vaultRoot, QString serverURL, QString email, PasswordPrompt prompt,
                       bool viaLockbox);
    void stopOnWorker();
    void runCycle();
    void setStatus(Status status, const QString &message = {});

    bool ensureSession();      ///< sign in / register, get a bearer token and vaultID
    bool loadDeviceIdentity(); ///< this device's X25519 keypair, from the OS keyring or freshly made
    bool ensureItemsKey();     ///< path 1 (wrapKey) or path 2 (lockbox), per m_viaLockbox
    void maintainEnrolment();  ///< seal lockboxes for any pending device, if we can

    int pushOutbox();
    void pullJournal(int *pulled, int *conflicts);
    void applyRecord(const SyncClient::JournalRecord &record, int *pulled, int *conflicts);
    void queueLocalChange(const SyncScanner::Change &change);

    QThread m_thread;
    QTimer *m_debounce = nullptr; ///< a change kicks this, single-shot, a few seconds out
    QTimer *m_periodic = nullptr; ///< catches remote changes even with no local edits

    std::unique_ptr<SyncJournal> m_journal;
    std::unique_ptr<SyncClient> m_client;
    Vault m_vault;
    QString m_email;
    PasswordPrompt m_prompt;
    bool m_viaLockbox = false;
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
};

} // namespace noteAppC

Q_DECLARE_METATYPE(noteAppC::SyncEngine::Status)
