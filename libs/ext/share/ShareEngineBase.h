#pragma once

#include <functional>
#include <optional>

#include <QByteArray>
#include <QList>
#include <QObject>
#include <QPair>
#include <QString>
#include <QStringList>

#include "CrdtDoc.h"
#include "ShareStore.h"

namespace noteAppC {

/// What a share engine is started with (ShareEngineBase::Config). Namespace
/// scope so ModuleHost.h can name it without this header.
struct ShareEngineConfig {
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

/// What the window and the share dialogs see of the sharing engine
/// (docs/SHARE.md "The engine"). The engine itself (ShareEngine, with the
/// protobuf, Abseil and datachannel it needs) lives in the notolith-share
/// module, loaded on first use; this is the one declaration of its surface
/// that both sides compile, so the window's pointer-to-member connects match
/// the object the module hands back. Its moc is in noteAppC_sharebase only
/// (the executable's); the module links the headers and resolves it from the
/// executable, like ModuleView. A member added here and not in ShareEngine is
/// a compile error (this class is abstract), so the two cannot drift.
///
/// Lives on the UI thread, like every CrdtDoc it owns.
class ShareEngineBase : public QObject {
    Q_OBJECT
public:
    using Config = ShareEngineConfig;

    explicit ShareEngineBase(QObject *parent = nullptr)
        : QObject(parent)
    {
    }

    /// Loads (or mints) this device's keys, opens the link (Config::port, else
    /// any port) and loads the shares of this vault. False, with error(), when
    /// the keys or the link cannot be had. create() and join() call it.
    virtual bool start() = 0;
    virtual bool isRunning() const = 0;
    virtual QString error() const = 0;
    virtual QString fingerprint() const = 0;
    virtual QByteArray publicKey() const = 0; ///< this device's key; empty before start()
    virtual quint16 port() const = 0;

    // --- owner ---

    struct Created {
        QByteArray shareID;
        QString inviteCode;
    };
    /// A new share of the note at `relativePath` holding `text`. Refused (null,
    /// *error) when that path is shared already or the text is too large to
    /// send in one message.
    virtual std::optional<Created> create(const QString &relativePath, const QString &title, const QString &text,
                                          QString *error) = 0;
    /// A fresh invitation code, valid Config::inviteDays; it replaces the
    /// previous one. Empty unless this device owns the share.
    virtual QString invite(const QByteArray &shareID) = 0;
    virtual void approve(const QByteArray &shareID, const QByteArray &joiner) = 0;
    virtual void refuse(const QByteArray &shareID, const QByteArray &joiner) = 0;
    /// Removes a member, rotates the share key and tells the rest. The removed
    /// device keeps what it already has (docs/SYNC.md's honesty about this).
    virtual void revoke(const QByteArray &shareID, const QByteArray &member) = 0;

    // --- joiner ---

    /// Parses the code, dials its hosts and sends the join request once the
    /// owner is connected. The answer arrives as joinRequested() on the
    /// owner's side and joined() or failed() here. False, with *error, for a
    /// code that is malformed, expired or for another CRDT engine.
    virtual bool join(const QString &inviteCode, const QString &name, QString *error) = 0;
    /// Connects to a device at an address, e.g. one that is not on mDNS. The
    /// expected key, when given, must be the one that answers.
    virtual void dial(const QString &host, quint16 port, const QByteArray &expected = {}) = 0;

    // --- both ---

    /// Forgets the share on this device only; the others keep it.
    virtual void stop(const QByteArray &shareID) = 0;
    /// Where the note lives in the vault (the joiner's copy is placed by the
    /// window, after joined(); a rename changes it).
    virtual void setPath(const QByteArray &shareID, const QString &relativePath) = 0;

    // --- what a note's session uses ---

    virtual CrdtDoc *doc(const QByteArray &shareID) const = 0;
    virtual QList<ShareRecord> shares() const = 0;
    virtual std::optional<ShareRecord> shareFor(const QString &relativePath) const = 0;
    /// Sends the update returned by doc(shareID)->localEdit() to every
    /// connected member, as one sealed, signed Frame each.
    virtual void broadcast(const QByteArray &shareID, const QByteArray &update) = 0;
    virtual int peersHere(const QByteArray &shareID) const = 0; ///< connected members besides this device
    /// `base` is the text the file on disk and the document agree on; the
    /// share's state is written now.
    virtual void persist(const QByteArray &shareID, const QString &base) = 0;
    /// The text the file on disk and the document last agreed on, as persist()
    /// was given it ("" for an unknown share).
    virtual QString base(const QByteArray &shareID) const = 0;
    /// Whether the store holds a share of this vault, which start() would load;
    /// reads the store only.
    virtual bool hasStoredShares() const = 0;
    /// The members the owner has listed: (device key, label).
    virtual QList<QPair<QByteArray, QString>> members(const QByteArray &shareID) const = 0;
    /// What both devices show for each other: compared aloud, it proves the
    /// key is the one meant. Empty before start().
    virtual QString safetyNumberWith(const QByteArray &device) const = 0;
    /// The short name of a device key (ShareEngine::fingerprintOf), for a window that holds no engine class.
    virtual QString deviceFingerprint(const QByteArray &device) const = 0;
    virtual bool isConnected(const QByteArray &device) const = 0;

    /// Where this device's caret is in the share's text (UTF-16 offsets, as
    /// the document has the text now). Sent to the others at most ten times a
    /// second, and again with every resync so it does not expire; it goes out
    /// both as offsets (an old build reads those) and as positions on the
    /// characters (CrdtDoc::encodePosition), which stay right when other
    /// members' edits are still arriving.
    virtual void setCaret(const QByteArray &shareID, qsizetype anchor, qsizetype head) = 0;
    /// This device no longer shows the note: the others stop drawing its caret.
    virtual void clearCaret(const QByteArray &shareID) = 0;
    /// Members besides this device that are connected, directly or (their
    /// caret arriving) through another member.
    virtual int membersHere(const QByteArray &shareID) const = 0;
    virtual bool isHere(const QByteArray &shareID, const QByteArray &device) const = 0;
    struct Caret {
        QByteArray device;
        QString name;
        qsizetype anchor = 0, head = 0;
    };
    /// The other members' carets, as they last sent them: on the characters
    /// they named when this replica has them (so an edit that arrived since
    /// has moved them), else at the offsets they sent.
    virtual QList<Caret> carets(const QByteArray &shareID) const = 0;

    struct Counters {
        int dropped = 0;       ///< messages refused: malformed, wrong sender, replayed, tampered...
        int accepted = 0;      ///< frames that passed every check
        QString lastReason;
    };
    /// Per share; an empty id gives the engine-wide count, which also holds
    /// what belonged to no known share.
    virtual Counters counters(const QByteArray &shareID = {}) const = 0;

    virtual void releaseHeldFramesForTests() = 0;
    /// Test seam: a Presence claiming `author`, sent as this device to every keyed member.
    virtual void sendPresenceForTests(const QByteArray &shareID, const QByteArray &author, quint64 counter,
                                      quint32 head) = 0;

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
    /// ShareEngine::parseInvite, for a window that holds no engine class: the join dialog's live check.
    virtual std::optional<InviteInfo> parseInviteCode(const QString &code, QString *error) const = 0;

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
    /// A member's caret moved, arrived, or went (they left, or said nothing for a minute).
    void presenceChanged(const QByteArray &shareID);
    void failed(const QByteArray &shareID, const QString &message);
};

} // namespace noteAppC
