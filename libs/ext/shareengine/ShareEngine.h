#pragma once

#include <functional>
#include <memory>
#include <optional>

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QTimer>

#include "CrdtDoc.h"
#include "ShareIdentity.h"
#include "ShareStore.h"

namespace hearth {
class Node;
}

namespace notolith::share {
class Join;
class Welcome;
class Rekey;
class Refused;
class Frame;
}

namespace noteAppC {

/// Sharing notes peer to peer (docs/SHARE.md "The engine"): invitations,
/// membership, and the CRDT updates of every shared note of one vault, over
/// the vendored link layer. No editor and no window in here - ShareSession
/// (a later lane) wires a note's editor to doc() and broadcast().
///
/// Lives on the UI thread, like every CrdtDoc it owns. The link's callbacks
/// arrive on its own threads and are queued here, so a test needs an event
/// loop (QTest::qWaitFor). Nothing is built until start(): the constructor
/// touches neither the keyring, the network nor the disk.
class ShareEngine : public QObject {
    Q_OBJECT
public:
    struct Config {
        quint16 port = 47810;                  ///< busy: any port is taken instead
        bool mDNS = true;
        QString identityKey = QStringLiteral("device-identity-v1"); ///< keyring entry of this device's keys
        QStringList hosts;                     ///< advertised in invites; empty = this machine's IPv4 addresses
        QString storeDirectory;                ///< empty = ShareStore::directoryFor(vaultRoot)
        int inviteDays = 7;
        /// A state vector goes to every connected member this often, and
        /// `settleMs` after the last change came in: what a peer lacks (a lost
        /// frame, or a yrs update left pending behind an edit that has arrived
        /// since - see docs/SHARE.md) is then sent. Nothing is sent when the
        /// peer already has it all but one small frame.
        int resyncMs = 30000;
        int settleMs = 3000;
        /// The engine of the shares this device creates; empty = Yrs when the
        /// build has it, else Native. A joiner always takes the owner's.
        std::optional<CrdtDoc::Backend> backend;
        /// Test seam: true queues the outgoing Frames of that share until
        /// releaseHeldFramesForTests().
        std::function<bool(const QByteArray &shareID)> holdFramesForTests;
    };

    explicit ShareEngine(const QString &vaultRoot, QObject *parent = nullptr);
    ShareEngine(const QString &vaultRoot, Config config, QObject *parent = nullptr);
    ~ShareEngine() override; ///< persists every share, stops the link first, then drops the documents

    /// Loads (or mints) this device's keys, opens the link (Config::port, else
    /// any port) and loads the shares of this vault. False, with error(), when
    /// the keys or the link cannot be had. create() and join() call it.
    bool start();
    bool isRunning() const;
    QString error() const;
    QString fingerprint() const;
    quint16 port() const;

    // --- owner ---

    struct Created {
        QByteArray shareID;
        QString inviteCode;
    };
    /// A new share of the note at `relativePath` holding `text`. Refused (null,
    /// *error) when that path is shared already or the text is too large to
    /// send in one message.
    std::optional<Created> create(const QString &relativePath, const QString &title, const QString &text, QString *error);
    /// A fresh invitation code, valid Config::inviteDays; it replaces the
    /// previous one. Empty unless this device owns the share.
    QString invite(const QByteArray &shareID);
    void approve(const QByteArray &shareID, const QByteArray &joiner);
    void refuse(const QByteArray &shareID, const QByteArray &joiner);
    /// Removes a member, rotates the share key and tells the rest. The removed
    /// device keeps what it already has (docs/SYNC.md's honesty about this).
    void revoke(const QByteArray &shareID, const QByteArray &member);

    // --- joiner ---

    /// Parses the code, dials its hosts and sends the join request once the
    /// owner is connected. The answer arrives as joinRequested() on the
    /// owner's side and joined() or failed() here. False, with *error, for a
    /// code that is malformed, expired or for another CRDT engine.
    bool join(const QString &inviteCode, const QString &name, QString *error);
    /// Connects to a device at an address, e.g. one that is not on mDNS. The
    /// expected key, when given, must be the one that answers.
    void dial(const QString &host, quint16 port, const QByteArray &expected = {});

    // --- both ---

    /// Forgets the share on this device only; the others keep it.
    void stop(const QByteArray &shareID);
    /// Where the note lives in the vault (the joiner's copy is placed by the
    /// window, after joined(); a rename changes it).
    void setPath(const QByteArray &shareID, const QString &relativePath);

    // --- what a note's session uses ---

    CrdtDoc *doc(const QByteArray &shareID) const;
    QList<ShareRecord> shares() const;
    std::optional<ShareRecord> shareFor(const QString &relativePath) const;
    /// Sends the update returned by doc(shareID)->localEdit() to every
    /// connected member, as one sealed, signed Frame each.
    void broadcast(const QByteArray &shareID, const QByteArray &update);
    int peersHere(const QByteArray &shareID) const; ///< connected members besides this device
    /// `base` is the text the file on disk and the document agree on; the
    /// share's state is written now.
    void persist(const QByteArray &shareID, const QString &base);
    /// The text the file on disk and the document last agreed on, as persist()
    /// was given it ("" for an unknown share).
    QString base(const QByteArray &shareID) const;
    /// Whether the store holds a share of this vault, which start() would load;
    /// reads the store only. Lets the window start the engine only when there
    /// is something to resume.
    bool hasStoredShares() const;
    /// The members the owner has listed: (device key, label).
    QList<QPair<QByteArray, QString>> members(const QByteArray &shareID) const;

    struct Counters {
        int dropped = 0;       ///< messages refused: malformed, wrong sender, replayed, tampered...
        int accepted = 0;      ///< frames that passed every check
        QString lastReason;
    };
    /// Per share; an empty id gives the engine-wide count, which also holds
    /// what belonged to no known share.
    Counters counters(const QByteArray &shareID = {}) const;

    void releaseHeldFramesForTests();

    /// What an invitation code says, once checked: prefix, version, sizes,
    /// not expired, a CRDT engine this build has, hosts that parse and are
    /// private (loopback, LAN, tailnet) addresses.
    struct InviteInfo {
        struct Host {
            QString host;
            quint16 port = 0;
        };
        QByteArray shareID, owner, token;
        QList<Host> hosts;
        qint64 expires = 0; ///< unix seconds
        int backend = 0;
    };
    static std::optional<InviteInfo> parseInvite(const QString &code, QString *error);

signals:
    /// Remote changes already applied to doc(), in order, each on the text the
    /// previous one left (CrdtDoc::applyUpdate).
    void remoteEdits(const QByteArray &shareID, const QList<noteAppC::CrdtEdit> &edits);
    /// A device holding the invite asks in; show `safetyNumber` and call
    /// approve() or refuse().
    void joinRequested(const QByteArray &shareID, const QByteArray &joiner, const QString &name, const QString &safetyNumber);
    /// This device is in: the document is loaded; place the note and setPath().
    void joined(const QByteArray &shareID, const QString &title);
    void membersChanged(const QByteArray &shareID);
    void peersChanged(const QByteArray &shareID);
    void failed(const QByteArray &shareID, const QString &message);

private:
    struct Share;
    struct PendingJoin;

    void peerUp(const QByteArray &key);
    void peerDown(const QByteArray &key);
    void handleMessage(const QByteArray &key, const QByteArray &bytes);
    void onJoin(const QByteArray &key, const notolith::share::Join &message);
    void onWelcome(const QByteArray &key, const notolith::share::Welcome &message);
    void onRekey(const QByteArray &key, const notolith::share::Rekey &message);
    void onRefused(const QByteArray &key, const notolith::share::Refused &message);
    void onFrame(const QByteArray &key, const notolith::share::Frame &message);

    /// `onSyncChannel`: the channel Frames travel on, so a Rekey or a Welcome
    /// stays in order with the Frames around it; the rest goes on ctl.
    bool sendTo(const QByteArray &key, bool onSyncChannel, const QByteArray &envelope);
    enum class FrameKind { Update, StateVector, Diff };
    bool sendFrame(Share &share, const QByteArray &to, FrameKind kind, const QByteArray &data);
    void sendJoin(const PendingJoin &join);
    void sendWelcome(Share &share, const QByteArray &joiner);
    void sendRekey(Share &share, const QByteArray &to, bool withKey);
    void sendRefused(const QByteArray &to, const QByteArray &shareID, const QString &reason);
    void fan(Share &share, const QByteArray &update, const QByteArray &except);
    void resign(Share &share);
    void exchangeStates(bool onlyChanged);
    void saveShare(Share &share);
    void saveDirty();
    void markDirty(Share &share);
    void drop(Share *share, const QString &reason, const QByteArray &from = {});
    bool adoptRecord(ShareRecord record, QString *why);
    QString inviteCodeFor(Share &share);

    QString m_vaultRoot;
    Config m_config;
    ShareStore m_store;
    std::optional<ShareIdentity> m_me;
    std::unique_ptr<hearth::Node> m_node;
    QString m_error;
    QHash<QByteArray, std::shared_ptr<Share>> m_shares;
    QHash<QByteArray, QPair<QByteArray, qint64>> m_invites; ///< shareID -> (token, expiry in unix seconds)
    QHash<QByteArray, std::shared_ptr<PendingJoin>> m_pendingJoins; ///< by shareID
    QSet<QByteArray> m_up;
    QList<QPair<QByteArray, QByteArray>> m_held;
    Counters m_counters;
    QTimer m_persist;
    QTimer m_resync;  ///< repeating
    QTimer m_settle;  ///< single shot, restarted by every change that comes in
};

} // namespace noteAppC
