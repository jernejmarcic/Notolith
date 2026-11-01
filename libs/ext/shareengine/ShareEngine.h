// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej
#pragma once

#include <functional>
#include <memory>
#include <optional>

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QLockFile>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QTimer>

#include "ShareEngineBase.h"
#include "ShareIdentity.h"

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
/// (a later layer) wires a note's editor to doc() and broadcast().
///
/// Lives on the UI thread, like every CrdtDoc it owns. The link's callbacks
/// arrive on its own threads and are queued here, so a test needs an event
/// loop (QTest::qWaitFor). Nothing is built until start(): the constructor
/// touches neither the keyring, the network nor the disk.
class ShareEngine final : public ShareEngineBase {
    Q_OBJECT
public:
    explicit ShareEngine(const QString &vaultRoot, QObject *parent = nullptr);
    ShareEngine(const QString &vaultRoot, Config config, QObject *parent = nullptr);
    ~ShareEngine() override; ///< persists every share, stops the link first, then drops the documents

    /// Loads (or mints) this device's keys, opens the link (Config::port, else
    /// any port) and loads the shares of this vault. False, with error(), when
    /// the keys or the link cannot be had. create() and join() call it.
    bool start() override;
    bool isRunning() const override;
    QString error() const override;
    QString fingerprint() const override;
    QByteArray publicKey() const override; ///< this device's key; empty before start()
    quint16 port() const override;

    // --- owner ---

    /// A new share of the note at `relativePath` holding `text`. Refused (null,
    /// *error) when that path is shared already or the text is too large to
    /// send in one message.
    std::optional<Created> create(const QString &relativePath, const QString &title, const QString &text, QString *error) override;
    /// A fresh invitation code, valid Config::inviteDays; it replaces the
    /// previous one. Empty unless this device owns the share.
    QString invite(const QByteArray &shareID) override;
    void approve(const QByteArray &shareID, const QByteArray &joiner) override;
    void refuse(const QByteArray &shareID, const QByteArray &joiner) override;
    /// Removes a member, rotates the share key and tells the rest. The removed
    /// device keeps what it already has (docs/SYNC.md's honesty about this).
    void revoke(const QByteArray &shareID, const QByteArray &member) override;

    // --- joiner ---

    /// Parses the code, dials its hosts and sends the join request once the
    /// owner is connected. The answer arrives as joinRequested() on the
    /// owner's side and joined() or failed() here. False, with *error, for a
    /// code that is malformed, expired or for another CRDT engine.
    bool join(const QString &inviteCode, const QString &name, QString *error) override;
    /// Connects to a device at an address, e.g. one that is not on mDNS. The
    /// expected key, when given, must be the one that answers.
    void dial(const QString &host, quint16 port, const QByteArray &expected = {}) override;

    // --- both ---

    /// Forgets the share on this device only; the others keep it.
    void stop(const QByteArray &shareID) override;
    /// Where the note lives in the vault (the joiner's copy is placed by the
    /// window, after joined(); a rename changes it).
    void setPath(const QByteArray &shareID, const QString &relativePath) override;

    // --- what a note's session uses ---

    CrdtDoc *doc(const QByteArray &shareID) const override;
    QList<ShareRecord> shares() const override;
    std::optional<ShareRecord> shareFor(const QString &relativePath) const override;
    /// Sends the update returned by doc(shareID)->localEdit() to every
    /// connected member, as one sealed, signed Frame each.
    void broadcast(const QByteArray &shareID, const QByteArray &update) override;
    int peersHere(const QByteArray &shareID) const override; ///< connected members besides this device
    /// `base` is the text the file on disk and the document agree on; the
    /// share's state is written now.
    void persist(const QByteArray &shareID, const QString &base) override;
    /// The text the file on disk and the document last agreed on, as persist()
    /// was given it ("" for an unknown share).
    QString base(const QByteArray &shareID) const override;
    /// Whether the store holds a share of this vault, which start() would load;
    /// reads the store only. Lets the window start the engine only when there
    /// is something to resume.
    bool hasStoredShares() const override;
    /// The members the owner has listed: (device key, label).
    QList<QPair<QByteArray, QString>> members(const QByteArray &shareID) const override;
    /// What both devices show for each other: compared aloud, it proves the
    /// key is the one meant. Empty before start().
    QString safetyNumberWith(const QByteArray &device) const override;
    static QString fingerprintOf(const QByteArray &device);
    QString deviceFingerprint(const QByteArray &device) const override { return fingerprintOf(device); }
    bool isConnected(const QByteArray &device) const override;

    /// Where this device's caret is in the share's text (UTF-16 offsets).
    /// Sent to the others at most ten times a second, and again with every
    /// resync so it does not expire; nothing of it is stored.
    void setCaret(const QByteArray &shareID, qsizetype anchor, qsizetype head) override;
    /// This device no longer shows the note: the others stop drawing its caret.
    void clearCaret(const QByteArray &shareID) override;
    /// Members besides this device that are connected, directly or (their
    /// caret arriving) through another member.
    int membersHere(const QByteArray &shareID) const override;
    bool isHere(const QByteArray &shareID, const QByteArray &device) const override;
    /// The other members' carets, as they last sent them.
    QList<Caret> carets(const QByteArray &shareID) const override;

    /// Per share; an empty id gives the engine-wide count, which also holds
    /// what belonged to no known share.
    Counters counters(const QByteArray &shareID = {}) const override;

    void releaseHeldFramesForTests() override;
    /// Test seam: a Presence claiming `author`, sent as this device to every keyed member.
    void sendPresenceForTests(const QByteArray &shareID, const QByteArray &author, quint64 counter, quint32 head) override;

    /// What the engine checks an invitation against; static so a test (and nothing else) can ask without an engine.
    static std::optional<InviteInfo> parseInvite(const QString &code, QString *error);
    std::optional<InviteInfo> parseInviteCode(const QString &code, QString *error) const override
    {
        return parseInvite(code, error);
    }

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
    enum class FrameKind { Update, StateVector, Diff, Presence };
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
    std::unique_ptr<QLockFile> m_linkLock; ///< this device's link is online once per machine, see start()
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
    QTimer m_presence; ///< single shot: this device's carets go out when it fires
    void sendCarets(bool all);
    void onPresence(Share &share, const QByteArray &from, const QByteArray &bytes);
};

} // namespace noteAppC
