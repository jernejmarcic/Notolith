#include "ShareEngine.h"

#include <algorithm>
#include <format>
#include <utility>

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkInterface>
#include <QRandomGenerator>
#include <QStandardPaths>
#include <QSysInfo>

#include "ShareFrame.h"
#include "core/debuglog.h"
#include "net/node.h"

namespace noteAppC {

using namespace notolith::share;
using ShareFrame::kBackendNative;
using ShareFrame::kBackendYrs;
using ShareFrame::kMaxBytes;
using ShareFrame::toQ;
using ShareFrame::toS;

namespace {

constexpr int kMaxAwaiting = 16;       ///< join requests waiting for the owner, per share
constexpr int kMaxTitleChars = 100;
constexpr int kMaxReasonChars = 200;
constexpr int kMaxInviteChars = 8192;
constexpr int kMaxInviteHosts = 16;
constexpr int kStateHeadroom = 4096;   ///< what a Welcome carries besides the document's state
const QString kInvitePrefix = QStringLiteral("notolith-share:");
const QString kBackendMismatch = QStringLiteral("the other side's Notolith uses a different CRDT engine");

std::optional<CrdtDoc::Backend> backendOf(int tag)
{
    if (tag == kBackendYrs && CrdtDoc::available(CrdtDoc::Backend::Yrs))
        return CrdtDoc::Backend::Yrs;
    if (tag == kBackendNative && CrdtDoc::available(CrdtDoc::Backend::Native))
        return CrdtDoc::Backend::Native;
    return std::nullopt;
}

/// "Try both": Yrs when this build has it, else Native, unless the config names one.
int preferredBackend(std::optional<CrdtDoc::Backend> configured)
{
    if (configured)
        return *configured == CrdtDoc::Backend::Yrs ? kBackendYrs : kBackendNative;
    return CrdtDoc::available(CrdtDoc::Backend::Yrs) ? kBackendYrs : kBackendNative;
}

/// "host:port" of a private (loopback, LAN, tailnet) address. Sharing is for
/// those: a code or a record that sends this device to a public address is
/// refused rather than dialled.
std::optional<ShareEngine::InviteInfo::Host> privateHost(const QString &text)
{
    const qsizetype colon = text.lastIndexOf(QLatin1Char(':'));
    bool ok = false;
    const int port = colon > 0 ? text.mid(colon + 1).toInt(&ok) : 0;
    const QString host = text.left(colon);
    if (!ok || port < 1 || port > 65535 || host.isEmpty() || host.size() > 255 || !hearth::isPrivateHost(host.toStdString()))
        return std::nullopt;
    return ShareEngine::InviteInfo::Host{host, quint16(port)};
}

/// A Presence whose head is this: its author closed the note.
constexpr quint32 kCaretGone = 0xFFFFFFFFu;
/// A CrdtDoc position is a few bytes; more is not one. The Yrs decoder has its own copy of this
/// number (MAX_POSITION_BYTES in yrs-ffi/src/lib.rs); the Native one needs none, its reader is bounded.
constexpr size_t kMaxPositionBytes = 64;

/// A frame of ours has a reader there: it has sent us one we could open, so
/// it holds this epoch's key. Presence waits for that, so a member still
/// waiting for a rekey does not count ten refused frames a second.
bool keyedWith(const auto &share, const QByteArray &member)
{
    return share.lastSeq.contains(member);
}

quint64 newClientID()
{
    return QRandomGenerator::system()->generate64() & ((quint64(1) << 53) - 1);
}

std::span<const std::uint8_t> bytesOf(const QByteArray &b)
{
    return {reinterpret_cast<const std::uint8_t *>(b.constData()), size_t(b.size())};
}

QByteArray secretEpochAndKey(quint32 epoch, const QByteArray &key)
{
    return ShareFrame::be32(epoch) + key;
}

/// The unix second now.
qint64 nowSeconds()
{
    return QDateTime::currentSecsSinceEpoch();
}

QString localName()
{
    const QString name = ShareFrame::cleanText(QSysInfo::machineHostName(), ShareFrame::kMaxNameChars);
    return name.isEmpty() ? QStringLiteral("device") : name;
}

} // namespace

struct ShareEngine::Share {
    struct Member {
        QByteArray key, boxKey;
        QString name;
    };
    struct Awaiting {
        QByteArray key, boxKey;
        QString name;
    };

    ShareRecord record;
    std::unique_ptr<CrdtDoc> doc;
    QList<Member> members;
    QString base; ///< the text the file on disk and the document last agreed on
    QHash<QByteArray, quint64> lastSeq, nextSeq; ///< per peer, per connection
    QList<Awaiting> awaiting;
    Counters counters;
    struct Seen {
        qsizetype anchor = 0, head = 0;
        QByteArray anchorPos, headPos; ///< the same places by their characters; empty from an old build
        quint64 counter = 0;
        qint64 at = 0; ///< ms since epoch it arrived
        bool gone = false; ///< its author closed the note: kept for the counter
    };
    QHash<QByteArray, Seen> carets; ///< by author
    std::optional<std::pair<qsizetype, qsizetype>> myCaret;
    QByteArray myAnchorPos, myHeadPos; ///< myCaret by its characters (CrdtDoc::encodePosition)
    // Starts at the clock, so an author that restarted is not taken for a
    // replay by a member who only hears it through the owner (no peerDown there).
    quint64 caretCounter = quint64(QDateTime::currentMSecsSinceEpoch());
    bool caretDue = false;
    bool dirty = false;
    bool changedSinceExchange = false; ///< a remote update arrived since the last state-vector round

    const Member *member(const QByteArray &key) const
    {
        for (const Member &m : members)
            if (m.key == key)
                return &m;
        return nullptr;
    }
};

struct ShareEngine::PendingJoin {
    QByteArray shareID, owner, token;
    QString name;
    int backend = 0;
    QStringList hosts; ///< the invite's, kept in the record
};

ShareEngine::ShareEngine(const QString &vaultRoot, QObject *parent)
    : ShareEngine(vaultRoot, Config{}, parent)
{
}

ShareEngine::ShareEngine(const QString &vaultRoot, Config config, QObject *parent)
    : ShareEngineBase(parent)
    , m_vaultRoot(vaultRoot)
    , m_config(std::move(config))
    , m_store(m_config.storeDirectory.isEmpty() ? ShareStore::directoryFor(vaultRoot) : m_config.storeDirectory)
{
    m_persist.setSingleShot(true);
    m_persist.setInterval(2000);
    connect(&m_persist, &QTimer::timeout, this, &ShareEngine::saveDirty);
    m_resync.setInterval(m_config.resyncMs);
    connect(&m_resync, &QTimer::timeout, this, [this] {
        exchangeStates(false);
        sendCarets(true); // a heartbeat: carets older than a minute are dropped
    });
    m_presence.setSingleShot(true);
    m_presence.setInterval(100);
    connect(&m_presence, &QTimer::timeout, this, [this] { sendCarets(false); });
    m_settle.setSingleShot(true);
    m_settle.setInterval(m_config.settleMs);
    connect(&m_settle, &QTimer::timeout, this, [this] { exchangeStates(true); });
}

ShareEngine::~ShareEngine()
{
    m_persist.stop();
    m_resync.stop();
    m_settle.stop();
    m_presence.stop();
    for (const auto &share : std::as_const(m_shares))
        saveShare(*share);
    m_node.reset(); // joins the link's threads: no callback reaches this object after it
}

bool ShareEngine::isRunning() const
{
    return m_node != nullptr;
}

QString ShareEngine::error() const
{
    return m_error;
}

QString ShareEngine::fingerprint() const
{
    return m_me ? m_me->fingerprint() : QString();
}

QString ShareEngine::safetyNumberWith(const QByteArray &device) const
{
    if (!m_me || device.size() != 32)
        return {};
    return QString::fromStdString(hearth::safetyNumber(m_me->link.publicKey(), toPK(device)));
}

QString ShareEngine::fingerprintOf(const QByteArray &device)
{
    return device.size() == 32 ? QString::fromStdString(hearth::fingerprint(toPK(device))) : QString();
}

bool ShareEngine::isConnected(const QByteArray &device) const
{
    return m_up.contains(device);
}

QByteArray ShareEngine::publicKey() const
{
    return m_me ? m_me->publicKey() : QByteArray();
}

quint16 ShareEngine::port() const
{
    return m_node ? m_node->port() : 0;
}

bool ShareEngine::start()
{
    if (m_node)
        return true;
    m_error.clear();
    // A device is online once: a peer keeps one connection per identity, so a second Notolith
    // process (one per vault) with the same keys would split its peers between two links, and
    // both would publish one mDNS name. The first keeps the link; the others say so. The lock
    // is per identity key and, with a store directory of its own (tests), lives in it.
    if (!m_linkLock) {
        const QString directory = m_config.storeDirectory.isEmpty()
            ? QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + QStringLiteral("/noteAppC/share")
            : m_config.storeDirectory;
        QDir().mkpath(directory);
        const QByteArray id =
            QCryptographicHash::hash(m_config.identityKey.toUtf8(), QCryptographicHash::Sha256).toHex().left(16);
        auto lock = std::make_unique<QLockFile>(directory + QStringLiteral("/link-") + QString::fromLatin1(id)
                                                + QStringLiteral(".lock"));
        lock->setStaleLockTime(0); // only a dead owner frees it: a window can run for months
        if (!lock->tryLock(0)) {
            m_error = tr("Sharing is already running in another Notolith window: this device's link can be "
                         "online once. Close that window, or turn sharing off in it, then reopen this vault to share from here.");
            return false;
        }
        m_linkLock = std::move(lock);
    }
    const auto fail = [this](const QString &why) {
        m_error = why;
        m_linkLock.reset();
        return false;
    };
    m_me = ShareIdentity::load(m_config.identityKey);
    if (!m_me)
        return fail(tr("The keyring would not keep this device's sharing keys."));
    // A log belongs in the state directory (XDG), beside notolith.log; GenericStateLocation is Qt 6.7+.
#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
    const QString state = QStandardPaths::writableLocation(QStandardPaths::GenericStateLocation);
#else
    const QString state = qEnvironmentVariable("XDG_STATE_HOME", QDir::homePath() + QStringLiteral("/.local/state"));
#endif
    hearth::setDebugLogPath((state + QStringLiteral("/noteAppC/link.log")).toStdString());
    QFile::remove(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + QStringLiteral("/noteAppC/share/link.log")); // the old place

    // Callbacks run on the link's threads and hand their arguments over by value
    // (the span is only valid inside the callback).
    const auto open = [this](quint16 port) {
        hearth::Node::Config conf;
        conf.port = port;
        conf.mDNS = m_config.mDNS;
        conf.serviceType = "_notolith._tcp";
        conf.serviceNamePrefix = "notolith-";
        conf.signatureDomain = "notolith";
        auto node = std::make_unique<hearth::Node>(m_me->link, conf);
        node->onPeerUp = [this](const hearth::PublicKey &pk) {
            QMetaObject::invokeMethod(this, [this, key = fromPK(pk)] { peerUp(key); }, Qt::QueuedConnection);
        };
        node->onPeerDown = [this](const hearth::PublicKey &pk) {
            QMetaObject::invokeMethod(this, [this, key = fromPK(pk)] { peerDown(key); }, Qt::QueuedConnection);
        };
        node->onMessage = [this](const hearth::PublicKey &pk, hearth::Channel, std::span<const std::uint8_t> data) {
            QMetaObject::invokeMethod(
                this,
                [this, key = fromPK(pk), bytes = QByteArray(reinterpret_cast<const char *>(data.data()), qsizetype(data.size()))] {
                    handleMessage(key, bytes);
                },
                Qt::QueuedConnection);
        };
        node->start(); // throws on a busy port
        return node;
    };
    try {
        m_node = open(m_config.port);
    } catch (const std::exception &) {
        try {
            if (m_config.port == 0)
                throw;
            m_node = open(0);
        } catch (const std::exception &e) {
            return fail(tr("Sharing could not start its network link: %1").arg(QString::fromUtf8(e.what())));
        }
    }

    m_resync.start();
    for (const ShareRecord &record : m_store.all()) {
        QString why;
        if (!adoptRecord(record, &why) && hearth::debugLogOn())
            hearth::debugLog("share", std::format("skipped a stored share: {}", why.toStdString()));
        // Back to the owner where it was last invited from: mDNS finds it on the
        // LAN, this on the tailnet and other subnets.
        // Limit: the invite's addresses only; a moved owner is found by mDNS or a new invite.
        if (!record.owned && m_shares.contains(record.shareID))
            for (const QString &text : record.hosts)
                if (const auto host = privateHost(text))
                    m_node->dial(host->host.toStdString(), host->port, toPK(record.owner));
    }
    return true;
}

/// Brings a stored (or just minted) record to life: members verified, document
/// loaded. A state that does not load gives a fresh replica id and an empty
/// document - what the file holds folds in when a note attaches.
bool ShareEngine::adoptRecord(ShareRecord record, QString *why)
{
    const auto backend = backendOf(record.backend);
    if (!backend) {
        *why = kBackendMismatch;
        return false;
    }
    const auto body = ShareFrame::verifyMembers(record.members, record.owner);
    if (!body || toQ(body->share_id()) != record.shareID) {
        *why = QStringLiteral("member list does not verify");
        return false;
    }
    auto share = std::make_shared<Share>();
    share->doc = CrdtDoc::create(*backend, record.clientID);
    const QByteArray state = m_store.state(record.shareID);
    if (!share->doc || state.isEmpty() || !share->doc->loadState(state)) {
        record.clientID = newClientID();
        share->doc = CrdtDoc::create(*backend, record.clientID);
        share->dirty = true;
        if (!share->doc) {
            *why = QStringLiteral("no CRDT engine");
            return false;
        }
    }
    for (const Member &m : body->members())
        share->members.append({toQ(m.key()), toQ(m.box_key()), ShareFrame::cleanText(QString::fromStdString(m.name()), ShareFrame::kMaxNameChars)});
    record.epoch = body->epoch();
    record.owned = m_me && record.owner == m_me->publicKey();
    share->base = m_store.base(record.shareID);
    share->record = std::move(record);
    m_shares.insert(share->record.shareID, share);
    if (share->dirty)
        markDirty(*share);
    return true;
}

// --- owner -----------------------------------------------------------------

std::optional<ShareEngine::Created> ShareEngine::create(const QString &relativePath, const QString &title, const QString &text,
                                                        QString *error)
{
    const auto refuse = [&](const QString &message) {
        if (error)
            *error = message;
        return std::optional<Created>();
    };
    if (!start())
        return refuse(m_error);
    if (shareFor(relativePath))
        return refuse(tr("This note is shared already."));
    const int tag = preferredBackend(m_config.backend);
    const auto backend = backendOf(tag);
    if (!backend)
        return refuse(kBackendMismatch);

    ShareRecord record;
    record.shareID = SyncCrypto::randomID();
    record.owner = m_me->publicKey();
    record.key = SyncCrypto::randomKey();
    record.epoch = 1;
    record.backend = tag;
    record.clientID = newClientID();
    record.relativePath = relativePath;
    record.title = ShareFrame::cleanText(title, kMaxTitleChars);
    record.owned = true;

    auto doc = CrdtDoc::create(*backend, record.clientID);
    if (!doc)
        return refuse(kBackendMismatch);
    if (!text.isEmpty())
        doc->localEdit(0, 0, text);
    const QByteArray state = doc->encodeState();
    if (state.size() > kMaxBytes - kStateHeadroom)
        return refuse(tr("This note is too large to share yet."));

    auto share = std::make_shared<Share>();
    share->record = record;
    share->doc = std::move(doc);
    share->members.append({record.owner, m_me->box.publicKey, localName()});
    share->base = text;
    resign(*share);
    m_shares.insert(record.shareID, share);
    saveShare(*share);
    return Created{record.shareID, inviteCodeFor(*share)};
}

void ShareEngine::resign(Share &share)
{
    MemberListBody body;
    body.set_share_id(toS(share.record.shareID));
    body.set_epoch(share.record.epoch);
    body.set_owner(toS(share.record.owner));
    body.set_backend(uint32_t(share.record.backend));
    for (const Share::Member &m : share.members) {
        Member *member = body.add_members();
        member->set_key(toS(m.key));
        member->set_box_key(toS(m.boxKey));
        member->set_name(m.name.toStdString());
    }
    share.record.members = ShareFrame::signMembers(m_me->link, body);
}

QString ShareEngine::invite(const QByteArray &shareID)
{
    const auto it = m_shares.constFind(shareID);
    if (it == m_shares.constEnd() || !(*it)->record.owned || !m_node)
        return {};
    return inviteCodeFor(**it);
}

QString ShareEngine::inviteCodeFor(Share &share)
{
    const QByteArray token = SyncCrypto::randomKey();
    const qint64 expires = nowSeconds() + qint64(m_config.inviteDays) * 86400;
    m_invites.insert(share.record.shareID, {token, expires});

    QStringList hosts = m_config.hosts;
    if (hosts.isEmpty()) {
        for (const QHostAddress &a : QNetworkInterface::allAddresses())
            if (a.protocol() == QAbstractSocket::IPv4Protocol && !a.isLoopback())
                hosts.append(a.toString());
        hosts.append(QStringLiteral("127.0.0.1"));
    }
    QJsonArray list;
    for (const QString &h : std::as_const(hosts))
        list.append(QStringLiteral("%1:%2").arg(h).arg(m_node->port()));
    const QJsonObject o{
        {QStringLiteral("v"), 1},
        {QStringLiteral("id"), QString::fromLatin1(share.record.shareID.toHex())},
        {QStringLiteral("owner"), QString::fromLatin1(share.record.owner.toHex())},
        {QStringLiteral("token"), QString::fromLatin1(token.toHex())},
        {QStringLiteral("hosts"), list},
        {QStringLiteral("exp"), double(expires)},
        {QStringLiteral("backend"), share.record.backend},
    };
    return kInvitePrefix
         + QString::fromLatin1(QJsonDocument(o).toJson(QJsonDocument::Compact)
                                   .toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
}

void ShareEngine::approve(const QByteArray &shareID, const QByteArray &joiner)
{
    const auto it = m_shares.constFind(shareID);
    if (it == m_shares.constEnd() || !(*it)->record.owned)
        return;
    Share &share = **it;
    for (qsizetype i = 0; i < share.awaiting.size(); ++i) {
        if (share.awaiting[i].key != joiner)
            continue;
        const Share::Awaiting pending = share.awaiting.takeAt(i);
        if (share.members.size() >= ShareFrame::kMaxMembers) {
            emit failed(shareID, tr("This share has as many members as it can have."));
            return;
        }
        share.members.append({pending.key, pending.boxKey, pending.name});
        resign(share);
        sendWelcome(share, joiner);
        for (const Share::Member &m : std::as_const(share.members))
            if (m.key != m_me->publicKey() && m.key != joiner && m_up.contains(m.key))
                sendRekey(share, m.key, false);
        saveShare(share);
        emit membersChanged(shareID);
        return;
    }
}

void ShareEngine::refuse(const QByteArray &shareID, const QByteArray &joiner)
{
    const auto it = m_shares.constFind(shareID);
    if (it == m_shares.constEnd() || !(*it)->record.owned)
        return;
    Share &share = **it;
    for (qsizetype i = 0; i < share.awaiting.size(); ++i) {
        if (share.awaiting[i].key == joiner) {
            share.awaiting.removeAt(i);
            sendRefused(joiner, shareID, tr("The owner declined."));
            return;
        }
    }
}

void ShareEngine::revoke(const QByteArray &shareID, const QByteArray &member)
{
    const auto it = m_shares.constFind(shareID);
    if (it == m_shares.constEnd() || !(*it)->record.owned || member == (*it)->record.owner)
        return;
    Share &share = **it;
    bool found = false;
    for (qsizetype i = 0; i < share.members.size(); ++i) {
        if (share.members[i].key == member) {
            share.members.removeAt(i);
            found = true;
            break;
        }
    }
    if (!found)
        return;
    share.lastSeq.remove(member);
    share.nextSeq.remove(member);
    share.record.key = SyncCrypto::randomKey();
    ++share.record.epoch;
    resign(share);
    for (const Share::Member &m : std::as_const(share.members))
        if (m.key != m_me->publicKey() && m_up.contains(m.key))
            sendRekey(share, m.key, true);
    saveShare(share);
    emit membersChanged(shareID);
}

// --- joiner ----------------------------------------------------------------

std::optional<ShareEngine::InviteInfo> ShareEngine::parseInvite(const QString &code, QString *error)
{
    const auto refuse = [&](const QString &message) {
        if (error)
            *error = message;
        return std::optional<InviteInfo>();
    };
    const QString trimmed = code.trimmed();
    if (!trimmed.startsWith(kInvitePrefix) || trimmed.size() > kMaxInviteChars)
        return refuse(tr("This is not a sharing invitation."));
    const auto decoded = QByteArray::fromBase64Encoding(trimmed.mid(kInvitePrefix.size()).toLatin1(),
                                                        QByteArray::Base64UrlEncoding | QByteArray::AbortOnBase64DecodingErrors);
    if (!decoded)
        return refuse(tr("This invitation is damaged."));
    const QJsonObject o = QJsonDocument::fromJson(*decoded).object();
    const auto field = [&](const char *name, int size) {
        const QByteArray v = QByteArray::fromHex(o.value(QLatin1String(name)).toString().toLatin1());
        return v.size() == size ? v : QByteArray();
    };
    InviteInfo info;
    info.shareID = field("id", SyncCrypto::kIDBytes);
    info.owner = field("owner", 32);
    info.token = field("token", SyncCrypto::kKeyBytes);
    info.expires = qint64(o.value(QLatin1String("exp")).toDouble());
    info.backend = o.value(QLatin1String("backend")).toInt();
    if (o.value(QLatin1String("v")).toInt() != 1 || info.shareID.isEmpty() || info.owner.isEmpty() || info.token.isEmpty())
        return refuse(tr("This invitation is damaged."));
    if (info.expires <= nowSeconds())
        return refuse(tr("This invitation has expired."));
    if (!backendOf(info.backend))
        return refuse(kBackendMismatch);
    const QJsonArray hosts = o.value(QLatin1String("hosts")).toArray();
    if (hosts.isEmpty() || hosts.size() > kMaxInviteHosts)
        return refuse(tr("This invitation has no address to connect to."));
    for (const QJsonValue &value : hosts) {
        const auto host = privateHost(value.toString());
        if (!host)
            return refuse(tr("This invitation names an address that sharing will not connect to."));
        info.hosts.append(*host);
    }
    return info;
}

bool ShareEngine::join(const QString &inviteCode, const QString &name, QString *error)
{
    const auto fail = [&](const QString &message) {
        if (error)
            *error = message;
        return false;
    };
    QString why;
    const auto info = parseInvite(inviteCode, &why);
    if (!info)
        return fail(why);
    if (!start())
        return fail(m_error);
    if (m_shares.contains(info->shareID))
        return fail(tr("You are in this share already."));
    if (info->owner == m_me->publicKey())
        return fail(tr("You own this share."));

    auto pending = std::make_shared<PendingJoin>();
    pending->shareID = info->shareID;
    pending->owner = info->owner;
    pending->token = info->token;
    pending->name = ShareFrame::cleanText(name, ShareFrame::kMaxNameChars);
    if (pending->name.isEmpty())
        pending->name = localName();
    pending->backend = info->backend;
    for (const InviteInfo::Host &h : info->hosts)
        pending->hosts.append(h.host + QLatin1Char(':') + QString::number(h.port));
    m_pendingJoins.insert(info->shareID, pending);

    // On a LAN mDNS has usually connected the two before the code is pasted, and
    // peerUp will not fire again.
    const auto peers = m_node->peers();
    if (std::ranges::any_of(peers, [&](const hearth::PublicKey &pk) { return fromPK(pk) == info->owner; }))
        sendJoin(*pending);
    for (const InviteInfo::Host &h : info->hosts)
        m_node->dial(h.host.toStdString(), h.port, toPK(info->owner));
    return true;
}

void ShareEngine::dial(const QString &host, quint16 port, const QByteArray &expected)
{
    if (!m_node)
        return;
    m_node->dial(host.toStdString(), port,
                 expected.size() == 32 ? std::optional<hearth::PublicKey>(toPK(expected)) : std::nullopt);
}

void ShareEngine::sendJoin(const PendingJoin &pending)
{
    Envelope envelope;
    Join *join = envelope.mutable_join();
    join->set_share_id(toS(pending.shareID));
    // The proof binds the token to the key this device will be known by; the owner
    // recomputes it from the link's authenticated peer key, never from the message.
    join->set_proof(toS(SyncCrypto::contentAddress(pending.token, m_me->publicKey())));
    join->set_box_key(toS(m_me->box.publicKey));
    join->set_name(pending.name.toStdString());
    join->set_backend(uint32_t(pending.backend));
    sendTo(pending.owner, false, toQ(envelope.SerializeAsString()));
}

void ShareEngine::stop(const QByteArray &id)
{
    const QByteArray shareID = id; // the caller's may live in what is removed here
    if (m_shares.remove(shareID)) {
        m_store.remove(shareID);
        m_invites.remove(shareID);
    }
    m_pendingJoins.remove(shareID);
}

void ShareEngine::setPath(const QByteArray &shareID, const QString &relativePath)
{
    const auto it = m_shares.constFind(shareID);
    if (it == m_shares.constEnd())
        return;
    (*it)->record.relativePath = relativePath;
    markDirty(**it);
}

// --- what a note's session uses ----------------------------------------------

CrdtDoc *ShareEngine::doc(const QByteArray &shareID) const
{
    const auto it = m_shares.constFind(shareID);
    return it == m_shares.constEnd() ? nullptr : (*it)->doc.get();
}

QList<ShareRecord> ShareEngine::shares() const
{
    QList<ShareRecord> out;
    for (const auto &share : m_shares)
        out.append(share->record);
    return out;
}

std::optional<ShareRecord> ShareEngine::shareFor(const QString &relativePath) const
{
    for (const auto &share : m_shares)
        if (!relativePath.isEmpty() && share->record.relativePath == relativePath)
            return share->record;
    return std::nullopt;
}

QList<QPair<QByteArray, QString>> ShareEngine::members(const QByteArray &shareID) const
{
    QList<QPair<QByteArray, QString>> out;
    const auto it = m_shares.constFind(shareID);
    if (it != m_shares.constEnd())
        for (const Share::Member &m : (*it)->members)
            out.append({m.key, m.name});
    return out;
}

int ShareEngine::peersHere(const QByteArray &shareID) const
{
    const auto it = m_shares.constFind(shareID);
    if (it == m_shares.constEnd() || !m_me)
        return 0;
    int n = 0;
    for (const Share::Member &m : (*it)->members)
        n += m.key != m_me->publicKey() && m_up.contains(m.key);
    return n;
}

void ShareEngine::broadcast(const QByteArray &shareID, const QByteArray &update)
{
    const auto it = m_shares.constFind(shareID);
    if (it == m_shares.constEnd() || update.isEmpty())
        return;
    if (m_config.holdFramesForTests && m_config.holdFramesForTests(shareID)) {
        m_held.append({shareID, update});
        return;
    }
    fan(**it, update, {});
}

void ShareEngine::releaseHeldFramesForTests()
{
    const auto held = std::exchange(m_held, {});
    for (const auto &[shareID, update] : held)
        if (const auto it = m_shares.constFind(shareID); it != m_shares.constEnd())
            fan(**it, update, {});
}

void ShareEngine::fan(Share &share, const QByteArray &update, const QByteArray &except)
{
    markDirty(share);
    for (const Share::Member &m : std::as_const(share.members))
        if (m.key != m_me->publicKey() && m.key != except && m_up.contains(m.key))
            sendFrame(share, m.key, FrameKind::Update, update);
}

void ShareEngine::persist(const QByteArray &shareID, const QString &base)
{
    const auto it = m_shares.constFind(shareID);
    if (it == m_shares.constEnd())
        return;
    (*it)->base = base;
    saveShare(**it);
}

QString ShareEngine::base(const QByteArray &shareID) const
{
    const auto it = m_shares.constFind(shareID);
    return it == m_shares.constEnd() ? QString() : (*it)->base;
}

bool ShareEngine::hasStoredShares() const
{
    return !m_shares.isEmpty() || !m_store.all().isEmpty();
}

ShareEngine::Counters ShareEngine::counters(const QByteArray &shareID) const
{
    if (shareID.isEmpty())
        return m_counters;
    const auto it = m_shares.constFind(shareID);
    return it == m_shares.constEnd() ? Counters{} : (*it)->counters;
}

void ShareEngine::exchangeStates(bool onlyChanged)
{
    for (const auto &sharePtr : std::as_const(m_shares)) {
        Share &share = *sharePtr;
        if (onlyChanged && !share.changedSinceExchange)
            continue;
        share.changedSinceExchange = false;
        for (const Share::Member &m : std::as_const(share.members))
            if (m.key != m_me->publicKey() && m_up.contains(m.key))
                sendFrame(share, m.key, FrameKind::StateVector, share.doc->stateVector());
    }
}

void ShareEngine::saveShare(Share &share)
{
    m_store.save(share.record, share.doc->encodeState(), share.base);
    share.dirty = false;
}

void ShareEngine::saveDirty()
{
    for (const auto &share : std::as_const(m_shares))
        if (share->dirty)
            saveShare(*share);
}

void ShareEngine::markDirty(Share &share)
{
    share.dirty = true;
    if (!m_persist.isActive())
        m_persist.start();
}

void ShareEngine::drop(Share *share, const QString &reason, const QByteArray &from)
{
    ++m_counters.dropped;
    m_counters.lastReason = reason;
    if (share) {
        ++share->counters.dropped;
        share->counters.lastReason = reason;
    }
    if (hearth::debugLogOn())
        hearth::debugLog("share", std::format("dropped a message from {}: {}",
                                              from.size() == 32 ? hearth::debugKey(bytesOf(from)) : std::string("?"),
                                              reason.toStdString()));
}

// --- the link --------------------------------------------------------------

bool ShareEngine::sendTo(const QByteArray &key, bool onSyncChannel, const QByteArray &envelope)
{
    if (!m_node || key.size() != 32 || envelope.size() > kMaxBytes)
        return false;
    return m_node->send(toPK(key), onSyncChannel ? hearth::Channel::sync : hearth::Channel::ctl, bytesOf(envelope));
}

bool ShareEngine::sendFrame(Share &share, const QByteArray &to, FrameKind kind, const QByteArray &data)
{
    const quint64 seq = ++share.nextSeq[to];
    const QByteArray envelope = ShareFrame::seal(*m_me, share.record.shareID, share.record.epoch, seq, share.record.key,
                                                 kind == FrameKind::StateVector ? ShareFrame::Kind::StateVector
                                                 : kind == FrameKind::Diff     ? ShareFrame::Kind::Diff
                                                 : kind == FrameKind::Presence ? ShareFrame::Kind::Presence
                                                                               : ShareFrame::Kind::Update,
                                                 data);
    if (envelope.size() > kMaxBytes) {
        emit failed(share.record.shareID, tr("A change is too large to send."));
        return false;
    }
    return sendTo(to, true, envelope);
}

void ShareEngine::sendRefused(const QByteArray &to, const QByteArray &shareID, const QString &reason)
{
    Envelope envelope;
    envelope.mutable_refused()->set_share_id(toS(shareID));
    envelope.mutable_refused()->set_reason(reason.toStdString());
    sendTo(to, false, toQ(envelope.SerializeAsString()));
}

void ShareEngine::sendWelcome(Share &share, const QByteArray &joiner)
{
    const Share::Member *member = share.member(joiner);
    if (!member)
        return;
    WelcomeBody body;
    if (!body.mutable_members()->ParseFromArray(share.record.members.constData(), int(share.record.members.size())))
        return; // our own signed list: cannot happen
    body.set_state(toS(share.doc->encodeState()));
    body.set_title(share.record.title.toStdString());
    const QByteArray ad = SyncCrypto::buildAD("sw", {share.record.shareID, ShareFrame::be32(share.record.epoch), share.record.owner, joiner});
    Envelope envelope;
    Welcome *welcome = envelope.mutable_welcome();
    welcome->set_share_id(toS(share.record.shareID));
    welcome->set_sealed_key(toS(SyncCrypto::sealToBox(member->boxKey, secretEpochAndKey(share.record.epoch, share.record.key))));
    welcome->set_box(toS(SyncCrypto::seal(share.record.key, toQ(body.SerializeAsString()), ad)));
    const QByteArray bytes = toQ(envelope.SerializeAsString());
    if (bytes.size() > kMaxBytes) {
        emit failed(share.record.shareID, tr("This note has grown too large to hand to a new member."));
        return;
    }
    sendTo(joiner, true, bytes);
}

void ShareEngine::sendRekey(Share &share, const QByteArray &to, bool withKey)
{
    const Share::Member *member = share.member(to);
    if (!member)
        return;
    Envelope envelope;
    Rekey *rekey = envelope.mutable_rekey();
    rekey->set_share_id(toS(share.record.shareID));
    rekey->set_epoch(share.record.epoch);
    if (withKey)
        rekey->set_sealed_key(toS(SyncCrypto::sealToBox(member->boxKey, secretEpochAndKey(share.record.epoch, share.record.key))));
    if (!rekey->mutable_members()->ParseFromArray(share.record.members.constData(), int(share.record.members.size())))
        return;
    sendTo(to, true, toQ(envelope.SerializeAsString()));
}

void ShareEngine::peerUp(const QByteArray &key)
{
    m_up.insert(key);
    for (const auto &pending : std::as_const(m_pendingJoins))
        if (pending->owner == key)
            sendJoin(*pending);
    for (const auto &sharePtr : std::as_const(m_shares)) {
        Share &share = *sharePtr;
        if (!share.member(key))
            continue;
        share.lastSeq.remove(key);
        share.nextSeq.remove(key);
        // Both sides send what they have seen; each answers with what the other lacks.
        sendFrame(share, key, FrameKind::StateVector, share.doc->stateVector());
        // A member that missed a rotation while away gets the key again (idempotent).
        if (share.record.owned)
            sendRekey(share, key, true);
        emit peersChanged(share.record.shareID);
    }
}

void ShareEngine::peerDown(const QByteArray &key)
{
    m_up.remove(key);
    for (const auto &sharePtr : std::as_const(m_shares)) {
        Share &share = *sharePtr;
        share.lastSeq.remove(key);
        share.nextSeq.remove(key);
        if (share.member(key))
            emit peersChanged(share.record.shareID);
        if (share.carets.remove(key))
            emit presenceChanged(share.record.shareID);
    }
}

void ShareEngine::setCaret(const QByteArray &shareID, qsizetype anchor, qsizetype head)
{
    const auto it = m_shares.constFind(shareID);
    if (it == m_shares.constEnd())
        return;
    Share &share = **it;
    // Same offsets is not always the same characters: a remote edit that replaced the text just before
    // the caret leaves its offset alone and its old positions on deleted characters.
    if (share.myCaret && share.myCaret->first == anchor && share.myCaret->second == head
        && (!share.doc
            || (share.doc->resolvePosition(share.myAnchorPos) == anchor && share.doc->resolvePosition(share.myHeadPos) == head)))
        return;
    share.myCaret = std::pair(anchor, head);
    share.myAnchorPos = share.doc ? share.doc->encodePosition(anchor) : QByteArray();
    share.myHeadPos = share.doc ? share.doc->encodePosition(head) : QByteArray();
    share.caretDue = true;
    if (!m_presence.isActive())
        m_presence.start();
}

void ShareEngine::clearCaret(const QByteArray &shareID)
{
    const auto it = m_shares.constFind(shareID);
    if (it == m_shares.constEnd() || !(*it)->myCaret)
        return;
    (*it)->myCaret.reset();
    (*it)->caretDue = true;
    sendCarets(false); // now: stop() may forget the share right after
}

void ShareEngine::sendPresenceForTests(const QByteArray &shareID, const QByteArray &author, quint64 counter, quint32 head)
{
    const auto it = m_shares.constFind(shareID);
    if (it == m_shares.constEnd() || !m_me)
        return;
    notolith::share::Presence presence;
    presence.set_author(toS(author));
    presence.set_counter(counter);
    presence.set_anchor(head);
    presence.set_head(head);
    const QByteArray bytes = toQ(presence.SerializeAsString());
    for (const Share::Member &m : std::as_const((*it)->members))
        if (m.key != m_me->publicKey() && keyedWith(**it, m.key))
            sendFrame(**it, m.key, FrameKind::Presence, bytes);
}

bool ShareEngine::isHere(const QByteArray &shareID, const QByteArray &device) const
{
    if (m_up.contains(device))
        return true;
    const auto it = m_shares.constFind(shareID);
    if (it == m_shares.constEnd())
        return false;
    const auto seen = (*it)->carets.constFind(device);
    return seen != (*it)->carets.constEnd() && !seen->gone;
}

int ShareEngine::membersHere(const QByteArray &shareID) const
{
    const auto it = m_shares.constFind(shareID);
    if (it == m_shares.constEnd() || !m_me)
        return 0;
    int n = 0;
    for (const Share::Member &m : (*it)->members)
        n += m.key != m_me->publicKey() && isHere(shareID, m.key);
    return n;
}

void ShareEngine::sendCarets(bool all)
{
    if (!m_me)
        return;
    for (const auto &sharePtr : std::as_const(m_shares)) {
        Share &share = *sharePtr;
        if (all) {
            // Carets nobody has refreshed for a minute: their device is gone without a word.
            const qint64 stale = QDateTime::currentMSecsSinceEpoch() - 60000;
            const qsizetype before = share.carets.size();
            share.carets.removeIf([stale](const auto &entry) { return entry.value().at < stale; });
            if (share.carets.size() != before)
                emit presenceChanged(share.record.shareID);
        }
        // A cleared caret goes out once (as "gone"); the heartbeat repeats a shown one only.
        if (!share.caretDue && !(all && share.myCaret))
            continue;
        share.caretDue = false;
        notolith::share::Presence presence;
        presence.set_author(toS(m_me->publicKey()));
        presence.set_counter(++share.caretCounter);
        if (share.myCaret) {
            // Where the caret's characters are now: edits that arrived since
            // setCaret() moved them, and an old build only reads the offsets.
            qsizetype anchor = share.myCaret->first, head = share.myCaret->second;
            if (share.doc) {
                if (const qsizetype at = share.doc->resolvePosition(share.myAnchorPos); at >= 0)
                    anchor = at;
                if (const qsizetype at = share.doc->resolvePosition(share.myHeadPos); at >= 0)
                    head = at;
            }
            presence.set_anchor(quint32(qMax<qsizetype>(0, anchor)));
            presence.set_head(quint32(qMax<qsizetype>(0, head)));
            presence.set_anchor_pos(toS(share.myAnchorPos));
            presence.set_head_pos(toS(share.myHeadPos));
        } else {
            presence.set_anchor(kCaretGone);
            presence.set_head(kCaretGone);
        }
        const QByteArray bytes = toQ(presence.SerializeAsString());
        for (const Share::Member &m : std::as_const(share.members))
            if (m.key != m_me->publicKey() && keyedWith(share, m.key))
                sendFrame(share, m.key, FrameKind::Presence, bytes);
    }
}

void ShareEngine::onPresence(Share &share, const QByteArray &from, const QByteArray &bytes)
{
    notolith::share::Presence presence;
    if (!presence.ParseFromArray(bytes.constData(), int(bytes.size())))
        return drop(&share, QStringLiteral("presence does not decode"), from);
    const QByteArray author = toQ(presence.author());
    // A member speaks for itself; only the owner, the hub members reach each
    // other through, passes on someone else's.
    if (author == m_me->publicKey() || !share.member(author) || (author != from && from != share.record.owner))
        return;
    Share::Seen &seen = share.carets[author];
    if (presence.counter() <= seen.counter)
        return; // a copy that came round another way, or a late one
    const bool gone = presence.head() == kCaretGone;
    // A position is a few bytes; anything longer is not one of ours.
    const auto position = [](const std::string &bytes) {
        return bytes.size() <= kMaxPositionBytes ? toQ(bytes) : QByteArray();
    };
    seen = {qsizetype(presence.anchor()), qsizetype(presence.head()), position(presence.anchor_pos()),
            position(presence.head_pos()), presence.counter(), QDateTime::currentMSecsSinceEpoch(), gone};
    // Members need not be connected to each other: pass it on, once.
    for (const Share::Member &m : std::as_const(share.members))
        if (m.key != m_me->publicKey() && m.key != from && m.key != author && keyedWith(share, m.key))
            sendFrame(share, m.key, FrameKind::Presence, bytes);
    emit presenceChanged(share.record.shareID);
}

QList<ShareEngine::Caret> ShareEngine::carets(const QByteArray &shareID) const
{
    QList<Caret> out;
    const auto it = m_shares.constFind(shareID);
    if (it == m_shares.constEnd())
        return out;
    for (auto entry = (*it)->carets.constBegin(); entry != (*it)->carets.constEnd(); ++entry)
        if (const Share::Member *m = (*it)->member(entry.key()); m && !entry->gone) {
            // By characters when the author sent them and this replica has them, else by offset.
            const CrdtDoc *doc = (*it)->doc.get();
            qsizetype anchor = entry->anchor, head = entry->head;
            if (doc && !entry->anchorPos.isEmpty())
                if (const qsizetype at = doc->resolvePosition(entry->anchorPos); at >= 0)
                    anchor = at;
            if (doc && !entry->headPos.isEmpty())
                if (const qsizetype at = doc->resolvePosition(entry->headPos); at >= 0)
                    head = at;
            out.append({entry.key(), m->name, anchor, head});
        }
    return out;
}

void ShareEngine::handleMessage(const QByteArray &key, const QByteArray &bytes)
{
    const auto envelope = ShareFrame::parseEnvelope(bytes);
    if (!envelope) {
        drop(nullptr, bytes.size() > kMaxBytes ? QStringLiteral("message too large") : QStringLiteral("malformed message"), key);
        return;
    }
    switch (envelope->m_case()) {
    case Envelope::kJoin:
        return onJoin(key, envelope->join());
    case Envelope::kWelcome:
        return onWelcome(key, envelope->welcome());
    case Envelope::kRekey:
        return onRekey(key, envelope->rekey());
    case Envelope::kRefused:
        return onRefused(key, envelope->refused());
    case Envelope::kFrame:
        return onFrame(key, envelope->frame());
    case Envelope::M_NOT_SET:
        break;
    }
    drop(nullptr, QStringLiteral("empty message"), key);
}

void ShareEngine::onJoin(const QByteArray &key, const Join &join)
{
    const QByteArray shareID = toQ(join.share_id());
    const auto it = m_shares.constFind(shareID);
    if (it == m_shares.constEnd() || !(*it)->record.owned)
        return drop(nullptr, QStringLiteral("join for a share this device does not own"), key);
    Share &share = **it;
    // Approved while it was away and the Welcome was lost: answer again, no prompt.
    if (share.member(key))
        return sendWelcome(share, key);

    const auto invite = m_invites.constFind(shareID);
    if (invite == m_invites.constEnd())
        return drop(&share, QStringLiteral("join without an open invitation"), key);
    // Silent on a wrong proof: nothing tells a stranger whether a share exists.
    if (!SyncCrypto::equal(SyncCrypto::contentAddress(invite->first, key), toQ(join.proof())))
        return drop(&share, QStringLiteral("join with a wrong invitation proof"), key);
    if (invite->second <= nowSeconds())
        return sendRefused(key, shareID, tr("This invitation has expired."));
    if (int(join.backend()) != share.record.backend)
        return sendRefused(key, shareID, kBackendMismatch);
    if (join.box_key().size() != SyncCrypto::kBoxPublicBytes)
        return drop(&share, QStringLiteral("join with a malformed key"), key);
    for (const Share::Awaiting &a : std::as_const(share.awaiting))
        if (a.key == key)
            return; // asked already; the owner has the question
    if (share.awaiting.size() >= kMaxAwaiting)
        return drop(&share, QStringLiteral("too many join requests"), key);

    QString name = ShareFrame::cleanText(QString::fromStdString(join.name()), ShareFrame::kMaxNameChars);
    if (name.isEmpty())
        name = QString::fromStdString(hearth::fingerprint(toPK(key)));
    share.awaiting.append({key, toQ(join.box_key()), name});
    emit joinRequested(shareID, key, name, QString::fromStdString(hearth::safetyNumber(m_me->link.publicKey(), toPK(key))));
}

void ShareEngine::onWelcome(const QByteArray &key, const Welcome &welcome)
{
    const QByteArray shareID = toQ(welcome.share_id());
    const auto pendingIt = m_pendingJoins.constFind(shareID);
    if (pendingIt == m_pendingJoins.constEnd() || (*pendingIt)->owner != key || m_shares.contains(shareID))
        return drop(nullptr, QStringLiteral("a welcome that was not asked for"), key);
    const PendingJoin pending = **pendingIt;
    const auto fail = [&](const QString &why) {
        drop(nullptr, why, key);
        emit failed(shareID, tr("The invitation could not be completed (%1).").arg(why));
    };

    bool ok = false;
    QByteArray opened = SyncCrypto::openBox(m_me->box, toQ(welcome.sealed_key()), &ok);
    if (!ok || opened.size() != 4 + SyncCrypto::kKeyBytes)
        return fail(QStringLiteral("the share key does not open"));
    const quint32 epoch = (quint32(quint8(opened[0])) << 24) | (quint32(quint8(opened[1])) << 16) | (quint32(quint8(opened[2])) << 8)
                        | quint32(quint8(opened[3]));
    const QByteArray shareKey = opened.mid(4);
    SyncCrypto::wipe(opened);

    const QByteArray ad = SyncCrypto::buildAD("sw", {shareID, ShareFrame::be32(epoch), key, m_me->publicKey()});
    QByteArray plain = SyncCrypto::open(shareKey, toQ(welcome.box()), ad, &ok);
    if (!ok)
        return fail(QStringLiteral("the welcome does not decrypt"));
    WelcomeBody body;
    const bool parsed = body.ParseFromArray(plain.constData(), int(plain.size()));
    SyncCrypto::wipe(plain);
    if (!parsed)
        return fail(QStringLiteral("the welcome is malformed"));
    const auto list = ShareFrame::verifyMembers(body.members(), key);
    if (!list || toQ(list->share_id()) != shareID || list->epoch() != epoch || int(list->backend()) != pending.backend)
        return fail(QStringLiteral("the member list does not verify"));
    bool listed = false;
    for (const Member &m : list->members())
        listed |= toQ(m.key()) == m_me->publicKey() && toQ(m.box_key()) == m_me->box.publicKey;
    if (!listed)
        return fail(QStringLiteral("this device is not on the member list"));

    ShareRecord record;
    record.shareID = shareID;
    record.owner = key;
    record.key = shareKey;
    record.members = toQ(body.members().SerializeAsString());
    record.epoch = epoch;
    record.backend = pending.backend;
    record.clientID = newClientID();
    record.title = ShareFrame::cleanText(QString::fromStdString(body.title()), kMaxTitleChars);
    record.owned = false;
    record.hosts = pending.hosts;
    const auto backend = backendOf(record.backend);
    auto doc = backend ? CrdtDoc::create(*backend, record.clientID) : nullptr;
    if (!doc || !doc->loadState(toQ(body.state())))
        return fail(QStringLiteral("the note could not be loaded"));

    auto share = std::make_shared<Share>();
    share->record = record;
    for (const Member &m : list->members())
        share->members.append({toQ(m.key()), toQ(m.box_key()), ShareFrame::cleanText(QString::fromStdString(m.name()), ShareFrame::kMaxNameChars)});
    share->base = doc->text();
    share->doc = std::move(doc);
    m_shares.insert(shareID, share);
    m_pendingJoins.remove(shareID);
    saveShare(*share);
    emit joined(shareID, record.title);
    emit membersChanged(shareID);
}

void ShareEngine::onRekey(const QByteArray &key, const Rekey &rekey)
{
    const QByteArray shareID = toQ(rekey.share_id());
    const auto it = m_shares.constFind(shareID);
    if (it == m_shares.constEnd())
        return drop(nullptr, QStringLiteral("rekey for an unknown share"), key);
    Share &share = **it;
    if (key != share.record.owner || share.record.owned)
        return drop(&share, QStringLiteral("rekey from someone but the owner"), key);
    const auto list = ShareFrame::verifyMembers(rekey.members(), share.record.owner);
    if (!list || toQ(list->share_id()) != shareID || list->epoch() != rekey.epoch() || list->epoch() < share.record.epoch)
        return drop(&share, QStringLiteral("rekey that does not verify"), key);
    QByteArray newKey = share.record.key;
    if (!rekey.sealed_key().empty()) {
        bool ok = false;
        QByteArray opened = SyncCrypto::openBox(m_me->box, toQ(rekey.sealed_key()), &ok);
        if (!ok || opened.size() != 4 + SyncCrypto::kKeyBytes
            || !SyncCrypto::equal(opened.left(4), ShareFrame::be32(list->epoch())))
            return drop(&share, QStringLiteral("rekey whose key does not open"), key);
        newKey = opened.mid(4);
        SyncCrypto::wipe(opened);
    } else if (list->epoch() != share.record.epoch) {
        return drop(&share, QStringLiteral("rekey for a new epoch without a key"), key);
    }

    const bool rotated = list->epoch() != share.record.epoch;
    share.record.epoch = list->epoch();
    share.record.key = newKey;
    share.record.members = toQ(rekey.members().SerializeAsString());
    share.members.clear();
    for (const Member &m : list->members())
        share.members.append({toQ(m.key()), toQ(m.box_key()), ShareFrame::cleanText(QString::fromStdString(m.name()), ShareFrame::kMaxNameChars)});
    saveShare(share);
    emit membersChanged(shareID);
    if (!share.member(m_me->publicKey())) {
        emit failed(shareID, tr("You are no longer a member of this share."));
        return;
    }
    // Frames of the new epoch that arrived ahead of this were dropped: ask again.
    if (rotated)
        for (const Share::Member &m : std::as_const(share.members))
            if (m.key != m_me->publicKey() && m_up.contains(m.key))
                sendFrame(share, m.key, FrameKind::StateVector, share.doc->stateVector());
}

void ShareEngine::onRefused(const QByteArray &key, const Refused &refused)
{
    const QByteArray shareID = toQ(refused.share_id());
    const auto it = m_pendingJoins.constFind(shareID);
    if (it == m_pendingJoins.constEnd() || (*it)->owner != key)
        return drop(nullptr, QStringLiteral("a refusal that was not asked for"), key);
    m_pendingJoins.erase(it);
    emit failed(shareID, ShareFrame::cleanText(QString::fromStdString(refused.reason()), kMaxReasonChars));
}

void ShareEngine::onFrame(const QByteArray &key, const Frame &frame)
{
    const QByteArray shareID = toQ(frame.share_id());
    const auto it = m_shares.constFind(shareID);
    if (it == m_shares.constEnd())
        return drop(nullptr, QStringLiteral("frame for an unknown share"), key);
    Share &share = **it;
    if (!share.member(key))
        return drop(&share, QStringLiteral("frame from a device that is not a member"), key);
    QString why;
    const auto opened = ShareFrame::open(frame, share.record.key, shareID, share.record.epoch, key, share.lastSeq.value(key), &why);
    if (!opened) {
        drop(&share, why, key);
        if (why == QLatin1String("wrong epoch"))
            share.counters.lastReason = tr("waiting for the owner");
        return;
    }
    share.lastSeq[key] = opened->seq;
    ++share.counters.accepted;
    ++m_counters.accepted;
    if (opened->kind == ShareFrame::Kind::Presence)
        return onPresence(share, key, opened->data);
    if (opened->kind == ShareFrame::Kind::StateVector) {
        const QByteArray diff = share.doc->diffSince(opened->data);
        if (!diff.isEmpty())
            sendFrame(share, key, FrameKind::Diff, diff);
        return;
    }
    bool ok = false;
    const QList<CrdtEdit> edits = share.doc->applyUpdate(opened->data, &ok);
    if (!ok)
        return drop(&share, QStringLiteral("update does not decode"), key);
    markDirty(share);
    // A change that is not the answer to our own state vector: its sender may have
    // sent more that depends on it, or it waits on something not here yet; ask again.
    if (opened->kind == ShareFrame::Kind::Update) {
        share.changedSinceExchange = true;
        m_settle.start();
    }
    if (edits.isEmpty())
        return;
    // Members need not be connected to each other: pass a change on, once. A
    // second arrival changes nothing and so stops here.
    fan(share, opened->data, key);
    emit remoteEdits(shareID, edits);
}

} // namespace noteAppC
