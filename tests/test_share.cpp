#include <atomic>
#include <functional>
#include <memory>

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QMutexLocker>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QtTest/QTest>

#include "SecretStore.h"
#include "ShareEngine.h"
#include "ShareFrame.h"
#include "ShareIdentity.h"
#include "ShareStore.h"
#include "core/debuglog.h"
#include "net/node.h"

// The sharing engine over loopback: two or three engines in one process, and a
// bare link Node ("RawMember") that can put any bytes on the wire. Nothing
// here touches the real keyring (an in-memory store, main()), mDNS, port 47810
// or anything but 127.0.0.1; store directories are temporary, XDG_DATA_HOME
// and XDG_STATE_HOME point into the build directory (tests/CMakeLists.txt).
// QTest::qWaitFor + ASSERT: QTRY_* returns silently under gtest.

using namespace noteAppC;
using namespace notolith::share;

namespace {

QHash<QString, QByteArray> &memoryStore()
{
    static QHash<QString, QByteArray> store;
    return store;
}

bool waitFor(const std::function<bool()> &pred, int ms = 15000)
{
    return QTest::qWaitFor(pred, ms);
}

/// One engine with its own store directory and identity key, and a record of
/// what it signalled.
struct Peer {
    struct Request {
        QByteArray shareID, joiner;
        QString name, safety;
    };

    QTemporaryDir dir;
    QString key;
    ShareEngine::Config config;
    std::unique_ptr<ShareEngine> engine;
    QList<Request> requests;
    QList<QPair<QByteArray, QString>> joined;
    QStringList failures;
    QList<CrdtEdit> edits;
    int remoteBatches = 0;

    explicit Peer(const QString &identityKey, ShareEngine::Config base = {})
        : key(identityKey)
        , config(std::move(base))
    {
        config.port = 0;
        config.mDNS = false;
        config.identityKey = identityKey;
        config.hosts = {QStringLiteral("127.0.0.1")};
        config.storeDirectory = dir.path();
        build();
    }
    Peer(const Peer &) = delete;
    Peer &operator=(const Peer &) = delete;

    void build()
    {
        engine = std::make_unique<ShareEngine>(QStringLiteral("/nonexistent-vault"), config);
        QObject::connect(engine.get(), &ShareEngine::joinRequested, engine.get(),
                         [this](const QByteArray &id, const QByteArray &joiner, const QString &name, const QString &safety) {
                             requests.append({id, joiner, name, safety});
                         });
        QObject::connect(engine.get(), &ShareEngine::joined, engine.get(),
                         [this](const QByteArray &id, const QString &title) { joined.append({id, title}); });
        QObject::connect(engine.get(), &ShareEngine::failed, engine.get(),
                         [this](const QByteArray &, const QString &message) { failures.append(message); });
        QObject::connect(engine.get(), &ShareEngine::remoteEdits, engine.get(),
                         [this](const QByteArray &, const QList<CrdtEdit> &e) {
                             ++remoteBatches;
                             edits.append(e);
                         });
    }

    /// Destroys the engine (persisting it) and builds a new one on the same store and keys.
    void restart()
    {
        engine.reset();
        build();
    }

    QByteArray publicKey() const { return ShareIdentity::load(key)->publicKey(); }

    QString text(const QByteArray &id) const
    {
        CrdtDoc *doc = engine->doc(id);
        return doc ? doc->text() : QString();
    }

    /// A local edit the way a note's session makes one.
    void type(const QByteArray &id, qsizetype at, qsizetype removed, const QString &inserted)
    {
        engine->broadcast(id, engine->doc(id)->localEdit(at, removed, inserted));
    }
};

/// A bare link Node with its own keys, to say anything to an engine.
struct RawMember {
    ShareIdentity me;
    std::unique_ptr<hearth::Node> node;
    QMutex mutex;
    QList<QByteArray> inbox;
    std::atomic<bool> up = false;

    explicit RawMember(const QString &key)
        : me(std::move(*ShareIdentity::load(key)))
    {
        hearth::Node::Config conf;
        conf.port = 0;
        conf.mDNS = false;
        conf.serviceType = "_notolith._tcp";
        conf.serviceNamePrefix = "notolith-";
        conf.signatureDomain = "notolith";
        node = std::make_unique<hearth::Node>(me.link, conf);
        node->onPeerUp = [this](const hearth::PublicKey &) { up = true; };
        node->onPeerDown = [this](const hearth::PublicKey &) { up = false; };
        node->onMessage = [this](const hearth::PublicKey &, hearth::Channel, std::span<const std::uint8_t> d) {
            QMutexLocker lock(&mutex);
            inbox.append(QByteArray(reinterpret_cast<const char *>(d.data()), qsizetype(d.size())));
        };
        node->start();
    }
    ~RawMember() { node.reset(); }

    QByteArray key() const { return me.publicKey(); }

    bool connectTo(const Peer &owner)
    {
        node->dial("127.0.0.1", owner.engine->port(), toPK(owner.publicKey()));
        return waitFor([&] { return up.load(); });
    }

    bool send(const QByteArray &bytes, hearth::Channel channel = hearth::Channel::ctl)
    {
        const auto peers = node->peers();
        return !peers.empty()
            && node->send(peers.front(), channel, {reinterpret_cast<const std::uint8_t *>(bytes.constData()), size_t(bytes.size())});
    }

    QList<Envelope> received()
    {
        QMutexLocker lock(&mutex);
        QList<Envelope> out;
        for (const QByteArray &b : std::as_const(inbox))
            if (auto e = ShareFrame::parseEnvelope(b))
                out.append(*e);
        return out;
    }
    QList<QByteArray> rawInbox()
    {
        QMutexLocker lock(&mutex);
        return inbox;
    }
    int count(Envelope::MCase kind)
    {
        int n = 0;
        for (const Envelope &e : received())
            n += e.m_case() == kind;
        return n;
    }
    std::optional<Envelope> firstOf(Envelope::MCase kind)
    {
        for (const Envelope &e : received())
            if (e.m_case() == kind)
                return e;
        return std::nullopt;
    }
};

CrdtDoc::Backend backendEnum(int tag)
{
    return tag == 1 ? CrdtDoc::Backend::Yrs : CrdtDoc::Backend::Native;
}

QByteArray tokenOf(const QString &code)
{
    const auto json = QByteArray::fromBase64(code.mid(QStringLiteral("notolith-share:").size()).toLatin1(), QByteArray::Base64UrlEncoding);
    return QByteArray::fromHex(QJsonDocument::fromJson(json).object().value(QLatin1String("token")).toString().toLatin1());
}

QByteArray joinEnvelope(const QByteArray &shareID, const QByteArray &proof, const QByteArray &boxKey, const QString &name, int backend)
{
    Envelope e;
    Join *j = e.mutable_join();
    j->set_share_id(ShareFrame::toS(shareID));
    j->set_proof(ShareFrame::toS(proof));
    j->set_box_key(ShareFrame::toS(boxKey));
    j->set_name(name.toStdString());
    j->set_backend(uint32_t(backend));
    return ShareFrame::toQ(e.SerializeAsString());
}

/// The invite's token and backend, read straight from the code (an expired code
/// does not parse, and the tests need those too).
QByteArray validJoin(const RawMember &raw, const QString &code, const QByteArray &shareID, int backend, const QString &name = QStringLiteral("raw"))
{
    return joinEnvelope(shareID, SyncCrypto::contentAddress(tokenOf(code), raw.key()), raw.me.box.publicKey, name, backend);
}

struct Opened {
    QByteArray shareKey, state;
    quint32 epoch = 0;
};

/// What a joiner does with a Welcome, written out again from the wire format so
/// the tests do not lean on the engine's own reading of it.
std::optional<Opened> openWelcome(const RawMember &raw, const Welcome &w, const QByteArray &owner)
{
    bool ok = false;
    const QByteArray opened = SyncCrypto::openBox(raw.me.box, ShareFrame::toQ(w.sealed_key()), &ok);
    if (!ok || opened.size() != 36)
        return std::nullopt;
    Opened out;
    for (int i = 0; i < 4; ++i)
        out.epoch = (out.epoch << 8) | quint8(opened[i]);
    out.shareKey = opened.mid(4);
    const QByteArray ad = SyncCrypto::buildAD("sw", {ShareFrame::toQ(w.share_id()), ShareFrame::be32(out.epoch), owner, raw.key()});
    const QByteArray plain = SyncCrypto::open(out.shareKey, ShareFrame::toQ(w.box()), ad, &ok);
    WelcomeBody body;
    if (!ok || !body.ParseFromArray(plain.constData(), int(plain.size())))
        return std::nullopt;
    out.state = ShareFrame::toQ(body.state());
    return out;
}

/// owner shares "note.md"; `joiner` asks in and is approved.
QByteArray shareWith(Peer &owner, Peer &joiner, const QString &text, const QString &title = QStringLiteral("Title"))
{
    QString error;
    const auto created = owner.engine->create(QStringLiteral("note.md"), title, text, &error);
    EXPECT_TRUE(created) << error.toStdString();
    if (!created)
        return {};
    const qsizetype before = owner.requests.size();
    EXPECT_TRUE(joiner.engine->join(created->inviteCode, QStringLiteral("Bob"), &error)) << error.toStdString();
    EXPECT_TRUE(waitFor([&] { return owner.requests.size() > before; }));
    if (owner.requests.size() <= before)
        return {};
    owner.engine->approve(created->shareID, owner.requests.last().joiner);
    EXPECT_TRUE(waitFor([&] { return joiner.engine->doc(created->shareID) != nullptr; }));
    return created->shareID;
}

/// A further joiner into an existing share.
void addJoiner(Peer &owner, Peer &joiner, const QByteArray &id)
{
    const qsizetype before = owner.requests.size();
    QString error;
    ASSERT_TRUE(joiner.engine->join(owner.engine->invite(id), QStringLiteral("Carol"), &error)) << error.toStdString();
    ASSERT_TRUE(waitFor([&] { return owner.requests.size() > before; }));
    owner.engine->approve(id, owner.requests.last().joiner);
    ASSERT_TRUE(waitFor([&] { return joiner.engine->doc(id) != nullptr; }));
}

} // namespace

// --- pure pieces ---------------------------------------------------------------

TEST(ShareCrypto, EqualIsExactAndConstantTimeByShape)
{
    ASSERT_TRUE(SyncCrypto::init());
    EXPECT_TRUE(SyncCrypto::equal("abc", "abc"));
    EXPECT_TRUE(SyncCrypto::equal({}, {}));
    EXPECT_FALSE(SyncCrypto::equal("abc", "abd"));
    EXPECT_FALSE(SyncCrypto::equal("abc", "ab"));
    EXPECT_FALSE(SyncCrypto::equal("", "a"));
    EXPECT_FALSE(SyncCrypto::equal(QByteArray(32, 'a') + "x", QByteArray(32, 'a') + "y"));
}

TEST(ShareIdentityTest, IsMintedOnceAndReplacesAMalformedEntry)
{
    memoryStore().clear();
    const auto first = ShareIdentity::load(QStringLiteral("id1"));
    ASSERT_TRUE(first);
    const auto again = ShareIdentity::load(QStringLiteral("id1"));
    ASSERT_TRUE(again);
    EXPECT_EQ(first->publicKey(), again->publicKey());
    EXPECT_EQ(first->box.publicKey, again->box.publicKey);
    EXPECT_NE(first->publicKey(), ShareIdentity::load(QStringLiteral("id2"))->publicKey());
    EXPECT_FALSE(first->fingerprint().isEmpty());

    // wrong length, and the right length with a public half that is not the secret's
    memoryStore().insert(QStringLiteral("id1"), QByteArray(100, 'x'));
    const auto replaced = ShareIdentity::load(QStringLiteral("id1"));
    ASSERT_TRUE(replaced);
    EXPECT_NE(replaced->publicKey(), first->publicKey());
    EXPECT_EQ(memoryStore().value(QStringLiteral("id1")).size(), 128);
    QByteArray bad = memoryStore().value(QStringLiteral("id1"));
    bad.replace(96, 32, SyncCrypto::generateBoxKeyPair().publicKey);
    memoryStore().insert(QStringLiteral("id1"), bad);
    const auto healed = ShareIdentity::load(QStringLiteral("id1"));
    ASSERT_TRUE(healed);
    EXPECT_NE(healed->publicKey(), replaced->publicKey());
}

TEST(ShareIdentityTest, IsNullWhenTheKeyringRefusesEverything)
{
    secrets::setStore({[](const QString &, QByteArray *) { return false; }, [](const QString &, const QByteArray &) { return false; },
                       [](const QString &) { return false; }});
    EXPECT_FALSE(ShareIdentity::load(QStringLiteral("nothing")));
    secrets::setStore({[](const QString &key, QByteArray *value) {
                           if (!memoryStore().contains(key))
                               return false;
                           *value = memoryStore().value(key);
                           return true;
                       },
                       [](const QString &key, const QByteArray &value) { return memoryStore().insert(key, value) != memoryStore().end(); },
                       [](const QString &key) { return memoryStore().remove(key) > 0; }});
}

TEST(ShareFrameTest, ATamperedFrameIsDropped)
{
    const auto me = ShareIdentity::load(QStringLiteral("frame-me"));
    ASSERT_TRUE(me);
    const QByteArray id = SyncCrypto::randomID(), key = SyncCrypto::randomKey(), peer = me->publicKey();
    const auto frameOf = [&](const QByteArray &bytes) { return ShareFrame::parseEnvelope(bytes)->frame(); };
    const Frame good = frameOf(ShareFrame::seal(*me, id, 3, 5, key, ShareFrame::Kind::Update, "payload"));

    QString why;
    const auto opened = ShareFrame::open(good, key, id, 3, peer, 4, &why);
    ASSERT_TRUE(opened) << why.toStdString();
    EXPECT_EQ(opened->data, "payload");
    EXPECT_EQ(opened->seq, 5u);
    EXPECT_TRUE(opened->kind == ShareFrame::Kind::Update);
    const auto sv = ShareFrame::open(frameOf(ShareFrame::seal(*me, id, 3, 6, key, ShareFrame::Kind::StateVector, "sv")), key, id, 3, peer, 4);
    ASSERT_TRUE(sv);
    EXPECT_TRUE(sv->kind == ShareFrame::Kind::StateVector);

    Frame flippedBox = good;
    std::string box = flippedBox.box();
    box[box.size() / 2] ^= 1;
    flippedBox.set_box(box);
    EXPECT_FALSE(ShareFrame::open(flippedBox, key, id, 3, peer, 4));

    Frame flippedSig = good;
    std::string sig = flippedSig.sig();
    sig[10] ^= 1;
    flippedSig.set_sig(sig);
    EXPECT_FALSE(ShareFrame::open(flippedSig, key, id, 3, peer, 4));
    Frame shortSig = good;
    shortSig.set_sig(sig.substr(0, 63));
    EXPECT_FALSE(ShareFrame::open(shortSig, key, id, 3, peer, 4));

    // validly signed, sealed under another key: only the AEAD refuses it
    const Frame otherKey = frameOf(ShareFrame::seal(*me, id, 3, 5, SyncCrypto::randomKey(), ShareFrame::Kind::Update, "payload"));
    EXPECT_FALSE(ShareFrame::open(otherKey, key, id, 3, peer, 4));

    EXPECT_FALSE(ShareFrame::open(good, key, id, 4, peer, 4, &why)); // another epoch
    EXPECT_EQ(why, QStringLiteral("wrong epoch"));
    EXPECT_FALSE(ShareFrame::open(good, key, SyncCrypto::randomID(), 3, peer, 4));
    EXPECT_FALSE(ShareFrame::open(good, key, id, 3, SyncCrypto::randomKey(), 4)); // the sender is not the link peer
    EXPECT_FALSE(ShareFrame::open(good, key, id, 3, peer, 5)); // not beyond lastSeq
    EXPECT_FALSE(ShareFrame::open(good, key, id, 3, peer, 9));
    Frame shortSender = good;
    shortSender.set_sender("abc");
    EXPECT_FALSE(ShareFrame::open(shortSender, key, id, 3, QByteArray("abc"), 4));
}

TEST(ShareFrameTest, MembersNeedTheOwnersSignature)
{
    const auto owner = ShareIdentity::load(QStringLiteral("members-owner"));
    const auto other = ShareIdentity::load(QStringLiteral("members-other"));
    ASSERT_TRUE(owner && other);
    MemberListBody body;
    body.set_share_id(ShareFrame::toS(SyncCrypto::randomID()));
    body.set_epoch(1);
    body.set_owner(ShareFrame::toS(owner->publicKey()));
    body.set_backend(1);
    Member *m = body.add_members();
    m->set_key(ShareFrame::toS(owner->publicKey()));
    m->set_box_key(ShareFrame::toS(owner->box.publicKey));
    m->set_name("Owner");

    const QByteArray signedList = ShareFrame::signMembers(owner->link, body);
    const auto verified = ShareFrame::verifyMembers(signedList, owner->publicKey());
    ASSERT_TRUE(verified);
    EXPECT_EQ(verified->members_size(), 1);
    EXPECT_FALSE(ShareFrame::verifyMembers(signedList, other->publicKey())); // not signed by that key
    EXPECT_FALSE(ShareFrame::verifyMembers(ShareFrame::signMembers(other->link, body), owner->publicKey()));
    QByteArray tampered = signedList;
    tampered[10] = char(tampered[10] ^ 1);
    EXPECT_FALSE(ShareFrame::verifyMembers(tampered, owner->publicKey()));

    // a list the owner signed that is malformed anyway
    MemberListBody duplicate = body;
    *duplicate.add_members() = body.members(0);
    EXPECT_FALSE(ShareFrame::verifyMembers(ShareFrame::signMembers(owner->link, duplicate), owner->publicKey()));
    MemberListBody noOwner = body;
    noOwner.mutable_members(0)->set_key(ShareFrame::toS(other->publicKey()));
    EXPECT_FALSE(ShareFrame::verifyMembers(ShareFrame::signMembers(owner->link, noOwner), owner->publicKey()));
    MemberListBody longName = body;
    longName.mutable_members(0)->set_name(std::string(65, 'n'));
    EXPECT_FALSE(ShareFrame::verifyMembers(ShareFrame::signMembers(owner->link, longName), owner->publicKey()));
}

TEST(ShareFrameTest, TextFromTheWireIsCleaned)
{
    EXPECT_EQ(ShareFrame::cleanText(QStringLiteral("  a\nb\x07 c") + QChar(0x202e) + QStringLiteral("  "), 64), QStringLiteral("ab c"));
    EXPECT_EQ(ShareFrame::cleanText(QStringLiteral("abcdef"), 3), QStringLiteral("abc"));
    // never half of a surrogate pair
    EXPECT_EQ(ShareFrame::cleanText(QStringLiteral("ab\U0001F600"), 3), QStringLiteral("ab"));
    EXPECT_TRUE(ShareFrame::cleanText(QStringLiteral("\n\t"), 10).isEmpty());
}

TEST(ShareStoreTest, KeepsARecordPrivatelyAndSkipsABrokenOne)
{
    QTemporaryDir dir;
    ShareStore store(dir.path() + QStringLiteral("/store"));
    ShareRecord r;
    r.shareID = SyncCrypto::randomID();
    r.owner = SyncCrypto::randomKey();
    r.key = SyncCrypto::randomKey();
    r.members = QByteArray("signed list");
    r.epoch = 7;
    r.backend = 1;
    r.clientID = 123456789012345ULL;
    r.relativePath = QStringLiteral("Notes/a.md");
    r.title = QStringLiteral("A title");
    r.owned = true;
    ASSERT_TRUE(store.save(r, "state bytes", QStringLiteral("base text é")));

    const auto all = store.all();
    ASSERT_EQ(all.size(), 1);
    EXPECT_EQ(all[0].shareID, r.shareID);
    EXPECT_EQ(all[0].owner, r.owner);
    EXPECT_EQ(all[0].key, r.key);
    EXPECT_EQ(all[0].members, r.members);
    EXPECT_EQ(all[0].epoch, 7u);
    EXPECT_EQ(all[0].clientID, r.clientID);
    EXPECT_EQ(all[0].relativePath, r.relativePath);
    EXPECT_EQ(all[0].title, r.title);
    EXPECT_TRUE(all[0].owned);
    EXPECT_EQ(store.state(r.shareID), "state bytes");
    EXPECT_EQ(store.base(r.shareID), QStringLiteral("base text é"));

    const QString file = store.directory() + QLatin1Char('/') + QString::fromLatin1(r.shareID.toHex()) + QStringLiteral("/share.json");
    const QFileDevice::Permissions others = QFileDevice::ReadGroup | QFileDevice::WriteGroup | QFileDevice::ExeGroup
                                          | QFileDevice::ReadOther | QFileDevice::WriteOther | QFileDevice::ExeOther;
    EXPECT_TRUE(QFile::permissions(file) & QFileDevice::ReadOwner);
    EXPECT_FALSE(QFile::permissions(file) & others) << "the key file is for its owner only";
    EXPECT_FALSE(QFile::permissions(store.directory()) & others);

    QDir().mkpath(store.directory() + QStringLiteral("/garbage"));
    ASSERT_TRUE(QFile(store.directory() + QStringLiteral("/garbage/share.json")).open(QIODevice::WriteOnly));
    EXPECT_EQ(store.all().size(), 1);

    store.remove(QByteArray()); // an empty id must not take the whole store with it
    EXPECT_EQ(store.all().size(), 1);
    store.remove(r.shareID);
    EXPECT_TRUE(store.all().isEmpty());
}

TEST(ShareFrameTest, AnEnvelopeOverTheCapIsRefusedBeforeItIsParsed)
{
    // a perfectly good Envelope, just too big: only the size check can refuse it
    Envelope big;
    big.mutable_refused()->set_reason(std::string(ShareFrame::kMaxBytes + 10, 'x'));
    const QByteArray bytes = ShareFrame::toQ(big.SerializeAsString());
    ASSERT_GT(bytes.size(), ShareFrame::kMaxBytes);
    EXPECT_FALSE(ShareFrame::parseEnvelope(bytes));
    Envelope fits;
    fits.mutable_refused()->set_reason("fine");
    EXPECT_TRUE(ShareFrame::parseEnvelope(ShareFrame::toQ(fits.SerializeAsString())));
    EXPECT_FALSE(ShareFrame::parseEnvelope(QByteArray(200, '\xff')));
    EXPECT_FALSE(ShareFrame::parseEnvelope(QByteArray(ShareFrame::kMaxBytes, '\0')));
}

// --- the engine over loopback --------------------------------------------------

ShareEngine::Config neverStarted(const QString &store)
{
    ShareEngine::Config config;
    config.port = 0;
    config.mDNS = false;
    config.identityKey = QStringLiteral("never");
    config.storeDirectory = store;
    return config;
}

TEST(ShareEngineTest, NothingIsConstructedBeforeStart)
{
    memoryStore().clear();
    QTemporaryDir dir;
    const QString store = dir.path() + QStringLiteral("/never-made");
    {
        ShareEngine engine(QStringLiteral("/nonexistent-vault"),
                           neverStarted(store));
        EXPECT_FALSE(engine.isRunning());
        EXPECT_EQ(engine.port(), 0);
        EXPECT_TRUE(engine.fingerprint().isEmpty());
        EXPECT_TRUE(engine.shares().isEmpty());
    }
    EXPECT_FALSE(QDir(store).exists());
    EXPECT_TRUE(memoryStore().isEmpty()); // no key minted, none read
}

TEST(ShareEngineTest, ABusyPortFallsBackToAnyPort)
{
    QTcpServer busy;
    ASSERT_TRUE(busy.listen(QHostAddress::Any, 0));
    Peer peer(QStringLiteral("busy"));
    peer.config.port = busy.serverPort();
    peer.build();
    ASSERT_TRUE(peer.engine->start()) << peer.engine->error().toStdString();
    EXPECT_TRUE(peer.engine->isRunning());
    EXPECT_NE(peer.engine->port(), 0);
    EXPECT_NE(peer.engine->port(), busy.serverPort());
}

TEST(ShareEngineTest, TheInviteCodeRoundTrips)
{
    Peer owner(QStringLiteral("a"));
    QString error;
    const auto created = owner.engine->create(QStringLiteral("note.md"), QStringLiteral("T"), QStringLiteral("x"), &error);
    ASSERT_TRUE(created) << error.toStdString();

    const auto info = ShareEngine::parseInvite(created->inviteCode, &error);
    ASSERT_TRUE(info) << error.toStdString();
    EXPECT_EQ(info->shareID, created->shareID);
    EXPECT_EQ(info->owner, owner.publicKey());
    EXPECT_EQ(info->token.size(), 32);
    ASSERT_EQ(info->hosts.size(), 1);
    EXPECT_EQ(info->hosts[0].host, QStringLiteral("127.0.0.1"));
    EXPECT_EQ(info->hosts[0].port, owner.engine->port());
    EXPECT_GT(info->expires, QDateTime::currentSecsSinceEpoch() + 6 * 86400);
    EXPECT_NE(info->backend, 0);
    EXPECT_NE(owner.engine->invite(created->shareID), created->inviteCode); // a fresh token each time

    const QString prefix = QStringLiteral("notolith-share:");
    const auto json = QJsonDocument::fromJson(
        QByteArray::fromBase64(created->inviteCode.mid(prefix.size()).toLatin1(), QByteArray::Base64UrlEncoding)).object();
    const auto codeWith = [&](const QString &field, const QJsonValue &value) {
        QJsonObject o = json;
        o.insert(field, value);
        return prefix + QString::fromLatin1(QJsonDocument(o).toJson(QJsonDocument::Compact)
                                                .toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
    };
    EXPECT_FALSE(ShareEngine::parseInvite(QStringLiteral("notolith-shar:") + created->inviteCode.mid(prefix.size()), &error));
    EXPECT_FALSE(ShareEngine::parseInvite(QStringLiteral("hello"), &error));
    EXPECT_FALSE(ShareEngine::parseInvite(prefix + QStringLiteral("!!!not base64!!!"), &error));
    EXPECT_FALSE(ShareEngine::parseInvite(codeWith(QStringLiteral("v"), 2), &error));
    EXPECT_FALSE(ShareEngine::parseInvite(codeWith(QStringLiteral("id"), QString::fromLatin1(QByteArray(15, 'a').toHex())), &error));
    EXPECT_FALSE(ShareEngine::parseInvite(codeWith(QStringLiteral("owner"), QString::fromLatin1(QByteArray(31, 'a').toHex())), &error));
    EXPECT_FALSE(ShareEngine::parseInvite(codeWith(QStringLiteral("token"), QString::fromLatin1(QByteArray(16, 'a').toHex())), &error));
    EXPECT_FALSE(ShareEngine::parseInvite(codeWith(QStringLiteral("exp"), 1000), &error));
    EXPECT_FALSE(ShareEngine::parseInvite(codeWith(QStringLiteral("backend"), 99), &error));
    EXPECT_FALSE(ShareEngine::parseInvite(codeWith(QStringLiteral("hosts"), QJsonArray{QStringLiteral("127.0.0.1")}), &error));
    EXPECT_FALSE(ShareEngine::parseInvite(codeWith(QStringLiteral("hosts"), QJsonArray{QStringLiteral("127.0.0.1:0")}), &error));
    EXPECT_FALSE(ShareEngine::parseInvite(codeWith(QStringLiteral("hosts"), QJsonArray{QStringLiteral("8.8.8.8:47810")}), &error))
        << "a public address must not be dialled";
    EXPECT_TRUE(ShareEngine::parseInvite(codeWith(QStringLiteral("hosts"), QJsonArray{QStringLiteral("192.168.1.5:47810")}), &error));
}

TEST(ShareEngineTest, TwoEnginesConvergeOverLoopback)
{
    Peer owner(QStringLiteral("a")), joiner(QStringLiteral("b"));
    const QByteArray id = shareWith(owner, joiner, QStringLiteral("hello"), QStringLiteral("My note"));
    ASSERT_FALSE(id.isEmpty());
    ASSERT_TRUE(waitFor([&] { return !joiner.joined.isEmpty(); }));
    EXPECT_EQ(joiner.joined[0].first, id);
    EXPECT_EQ(joiner.joined[0].second, QStringLiteral("My note"));
    EXPECT_EQ(joiner.text(id), QStringLiteral("hello"));
    ASSERT_EQ(owner.requests.size(), 1);
    EXPECT_EQ(owner.requests[0].name, QStringLiteral("Bob"));
    EXPECT_EQ(owner.requests[0].joiner, joiner.publicKey());
    EXPECT_FALSE(owner.requests[0].safety.isEmpty());
    EXPECT_EQ(owner.engine->members(id).size(), 2);
    EXPECT_EQ(joiner.engine->members(id).size(), 2);
    ASSERT_TRUE(waitFor([&] { return owner.engine->peersHere(id) == 1 && joiner.engine->peersHere(id) == 1; }));

    owner.type(id, 5, 0, QStringLiteral(" world"));
    ASSERT_TRUE(waitFor([&] { return joiner.text(id) == QStringLiteral("hello world"); }));
    ASSERT_FALSE(joiner.edits.isEmpty());
    EXPECT_EQ(joiner.edits.last().inserted, QStringLiteral(" world"));
    EXPECT_EQ(joiner.edits.last().at, 5);

    joiner.type(id, 0, 0, QStringLiteral(">> "));
    ASSERT_TRUE(waitFor([&] { return owner.text(id) == QStringLiteral(">> hello world"); }));
    EXPECT_EQ(joiner.text(id), owner.text(id));
    EXPECT_EQ(owner.edits.last().inserted, QStringLiteral(">> "));
    EXPECT_EQ(owner.engine->counters(id).dropped, 0);
    EXPECT_EQ(joiner.engine->counters(id).dropped, 0);
}

TEST(ShareEngineTest, JoinNeedsTheOwnersApproval)
{
    Peer owner(QStringLiteral("a")), joiner(QStringLiteral("b"));
    QString error;
    const auto created = owner.engine->create(QStringLiteral("note.md"), QStringLiteral("T"), QStringLiteral("secret text"), &error);
    ASSERT_TRUE(created) << error.toStdString();
    ASSERT_TRUE(joiner.engine->join(created->inviteCode, QStringLiteral("Bob"), &error));
    ASSERT_TRUE(waitFor([&] { return !owner.requests.isEmpty(); }));
    QTest::qWait(400);
    EXPECT_EQ(joiner.engine->doc(created->shareID), nullptr);
    EXPECT_TRUE(joiner.engine->shares().isEmpty());
    EXPECT_TRUE(joiner.joined.isEmpty());
    EXPECT_EQ(owner.engine->members(created->shareID).size(), 1);

    owner.engine->approve(created->shareID, owner.requests[0].joiner);
    ASSERT_TRUE(waitFor([&] { return joiner.engine->doc(created->shareID) != nullptr; }));
    ASSERT_EQ(joiner.engine->shares().size(), 1);
    EXPECT_EQ(joiner.engine->shares()[0].key, owner.engine->shares()[0].key);
    EXPECT_EQ(owner.engine->members(created->shareID).size(), 2);
}

TEST(ShareEngineTest, ARefusedJoinerIsToldSo)
{
    Peer owner(QStringLiteral("a")), joiner(QStringLiteral("b"));
    QString error;
    const auto created = owner.engine->create(QStringLiteral("note.md"), QStringLiteral("T"), QStringLiteral("t"), &error);
    ASSERT_TRUE(created);
    ASSERT_TRUE(joiner.engine->join(created->inviteCode, QStringLiteral("Bob"), &error));
    ASSERT_TRUE(waitFor([&] { return !owner.requests.isEmpty(); }));
    owner.engine->refuse(created->shareID, owner.requests[0].joiner);
    ASSERT_TRUE(waitFor([&] { return !joiner.failures.isEmpty(); }));
    EXPECT_EQ(joiner.engine->doc(created->shareID), nullptr);
    EXPECT_EQ(owner.engine->members(created->shareID).size(), 1);
}

TEST(ShareEngineTest, AWrongInviteTokenIsSilentlyDropped)
{
    Peer owner(QStringLiteral("a"));
    QString error;
    const auto created = owner.engine->create(QStringLiteral("note.md"), QStringLiteral("T"), QStringLiteral("t"), &error);
    ASSERT_TRUE(created);
    RawMember raw(QStringLiteral("raw"));
    ASSERT_TRUE(raw.connectTo(owner));
    const int backend = owner.engine->shares()[0].backend;
    ASSERT_TRUE(raw.send(joinEnvelope(created->shareID, SyncCrypto::contentAddress(SyncCrypto::randomKey(), raw.key()),
                                      raw.me.box.publicKey, QStringLiteral("raw"), backend)));
    ASSERT_TRUE(waitFor([&] { return owner.engine->counters(created->shareID).dropped == 1; }));
    QTest::qWait(300);
    EXPECT_TRUE(owner.requests.isEmpty());
    EXPECT_TRUE(raw.rawInbox().isEmpty()) << "an answer to a wrong proof is an oracle";
    EXPECT_EQ(owner.engine->members(created->shareID).size(), 1);

    // and the right proof is accepted into the queue, so the above was the proof's doing
    ASSERT_TRUE(raw.send(validJoin(raw, created->inviteCode, created->shareID, backend)));
    ASSERT_TRUE(waitFor([&] { return owner.requests.size() == 1; }));
}

TEST(ShareEngineTest, AnExpiredInviteIsRefused)
{
    ShareEngine::Config config;
    config.inviteDays = 0; // expires the moment it is made
    Peer owner(QStringLiteral("a"), config);
    Peer joiner(QStringLiteral("b"));
    QString error;
    const auto created = owner.engine->create(QStringLiteral("note.md"), QStringLiteral("T"), QStringLiteral("t"), &error);
    ASSERT_TRUE(created) << error.toStdString();
    EXPECT_FALSE(joiner.engine->join(created->inviteCode, QStringLiteral("Bob"), &error));
    EXPECT_FALSE(error.isEmpty());
    EXPECT_FALSE(joiner.engine->isRunning()) << "a refused code starts nothing";

    RawMember raw(QStringLiteral("raw"));
    ASSERT_TRUE(raw.connectTo(owner));
    ASSERT_TRUE(raw.send(validJoin(raw, created->inviteCode, created->shareID, owner.engine->shares()[0].backend)));
    ASSERT_TRUE(waitFor([&] { return raw.count(Envelope::kRefused) == 1; }));
    EXPECT_TRUE(owner.requests.isEmpty());
    EXPECT_EQ(owner.engine->members(created->shareID).size(), 1);
}

TEST(ShareEngineTest, BackendMismatchIsRefused)
{
    Peer owner(QStringLiteral("a"));
    QString error;
    const auto created = owner.engine->create(QStringLiteral("note.md"), QStringLiteral("T"), QStringLiteral("t"), &error);
    ASSERT_TRUE(created);
    RawMember raw(QStringLiteral("raw"));
    ASSERT_TRUE(raw.connectTo(owner));
    ASSERT_TRUE(raw.send(validJoin(raw, created->inviteCode, created->shareID, 99)));
    ASSERT_TRUE(waitFor([&] { return raw.count(Envelope::kRefused) == 1; }));
    EXPECT_TRUE(owner.requests.isEmpty());
}

TEST(ShareEngineTest, AStrangerIsIgnored)
{
    Peer owner(QStringLiteral("a"));
    QString error;
    const auto created = owner.engine->create(QStringLiteral("note.md"), QStringLiteral("T"), QStringLiteral("original"), &error);
    ASSERT_TRUE(created);
    const ShareRecord record = owner.engine->shares()[0];
    RawMember raw(QStringLiteral("raw"));
    ASSERT_TRUE(raw.connectTo(owner));

    // sealed with the real key and epoch, signed by itself: only membership stops it
    auto doc = CrdtDoc::create(backendEnum(record.backend), 99);
    ASSERT_TRUE(doc->loadState(owner.engine->doc(created->shareID)->encodeState()));
    const QByteArray update = doc->localEdit(0, 0, QStringLiteral("evil "));
    ASSERT_FALSE(update.isEmpty());
    ASSERT_TRUE(raw.send(ShareFrame::seal(raw.me, created->shareID, record.epoch, 1, record.key, ShareFrame::Kind::Update, update),
                         hearth::Channel::sync));
    ASSERT_TRUE(waitFor([&] { return owner.engine->counters(created->shareID).dropped == 1; }));
    EXPECT_EQ(owner.text(created->shareID), QStringLiteral("original"));
    EXPECT_EQ(owner.remoteBatches, 0);

    owner.type(created->shareID, 8, 0, QStringLiteral(" and more"));
    QTest::qWait(400);
    EXPECT_TRUE(raw.rawInbox().isEmpty()) << "the owner talks to members only";
    EXPECT_EQ(owner.engine->peersHere(created->shareID), 0);
}

TEST(ShareEngineTest, ApprovalSurvivesTheJoinerReconnecting)
{
    Peer owner(QStringLiteral("a")), joiner(QStringLiteral("b"));
    QString error;
    const auto created = owner.engine->create(QStringLiteral("note.md"), QStringLiteral("T"), QStringLiteral("shared text"), &error);
    ASSERT_TRUE(created);
    ASSERT_TRUE(joiner.engine->join(created->inviteCode, QStringLiteral("Bob"), &error));
    ASSERT_TRUE(waitFor([&] { return owner.requests.size() == 1; }));
    joiner.restart(); // the joiner closes before the owner has answered; pending joins are not kept
    owner.engine->approve(created->shareID, owner.requests[0].joiner);
    EXPECT_EQ(owner.engine->members(created->shareID).size(), 2);
    QTest::qWait(200);
    EXPECT_EQ(joiner.engine->doc(created->shareID), nullptr);

    ASSERT_TRUE(joiner.engine->join(created->inviteCode, QStringLiteral("Bob"), &error)) << error.toStdString();
    ASSERT_TRUE(waitFor([&] { return joiner.engine->doc(created->shareID) != nullptr; }));
    EXPECT_EQ(joiner.text(created->shareID), QStringLiteral("shared text"));
    EXPECT_EQ(owner.requests.size(), 1) << "an approved member is not asked about again";
    owner.type(created->shareID, 0, 0, QStringLiteral(">"));
    ASSERT_TRUE(waitFor([&] { return joiner.text(created->shareID) == QStringLiteral(">shared text"); }));
}

TEST(ShareEngineTest, AReplayedFrameIsAppliedOnce)
{
    Peer owner(QStringLiteral("a"));
    QString error;
    const auto created = owner.engine->create(QStringLiteral("note.md"), QStringLiteral("T"), QStringLiteral("base"), &error);
    ASSERT_TRUE(created);
    const int backend = owner.engine->shares()[0].backend;
    RawMember raw(QStringLiteral("raw"));
    ASSERT_TRUE(raw.connectTo(owner));
    ASSERT_TRUE(raw.send(validJoin(raw, created->inviteCode, created->shareID, backend)));
    ASSERT_TRUE(waitFor([&] { return owner.requests.size() == 1; }));
    owner.engine->approve(created->shareID, raw.key());
    ASSERT_TRUE(waitFor([&] { return raw.count(Envelope::kWelcome) == 1; }));
    const auto welcome = openWelcome(raw, raw.firstOf(Envelope::kWelcome)->welcome(), owner.publicKey());
    ASSERT_TRUE(welcome);
    EXPECT_EQ(welcome->epoch, 1u);
    EXPECT_EQ(welcome->shareKey, owner.engine->shares()[0].key);

    auto doc = CrdtDoc::create(backendEnum(backend), 4242);
    ASSERT_TRUE(doc->loadState(welcome->state));
    EXPECT_EQ(doc->text(), QStringLiteral("base"));
    const QByteArray update = doc->localEdit(4, 0, QStringLiteral("+raw"));
    const QByteArray frame = ShareFrame::seal(raw.me, created->shareID, welcome->epoch, 1, welcome->shareKey, ShareFrame::Kind::Update, update);
    ASSERT_TRUE(raw.send(frame, hearth::Channel::sync));
    ASSERT_TRUE(waitFor([&] { return owner.remoteBatches == 1; }));
    ASSERT_TRUE(raw.send(frame, hearth::Channel::sync)); // the very same bytes again
    ASSERT_TRUE(waitFor([&] { return owner.engine->counters(created->shareID).dropped == 1; }));
    QTest::qWait(200);
    EXPECT_EQ(owner.remoteBatches, 1);
    EXPECT_EQ(owner.text(created->shareID), QStringLiteral("base+raw"));
}

TEST(ShareEngineTest, ARevokedPeerCannotReadAfterRotation)
{
    Peer owner(QStringLiteral("a")), a(QStringLiteral("b")), b(QStringLiteral("c"));
    const QByteArray id = shareWith(owner, a, QStringLiteral("start"));
    ASSERT_FALSE(id.isEmpty());
    addJoiner(owner, b, id);
    ASSERT_TRUE(waitFor([&] { return owner.engine->peersHere(id) == 2; }));

    // A's change reaches B though A and B are not connected: the owner passes it on
    a.type(id, 5, 0, QStringLiteral(" A"));
    ASSERT_TRUE(waitFor([&] { return b.text(id) == QStringLiteral("start A") && owner.text(id) == QStringLiteral("start A"); }));

    const QByteArray bKey = b.publicKey();
    const quint32 epochBefore = owner.engine->shares()[0].epoch;
    owner.engine->revoke(id, bKey);
    ASSERT_TRUE(waitFor([&] { return a.engine->shares()[0].epoch == epochBefore + 1; }));
    EXPECT_EQ(owner.engine->shares()[0].epoch, epochBefore + 1);
    EXPECT_NE(owner.engine->shares()[0].key, b.engine->shares()[0].key);
    EXPECT_EQ(owner.engine->members(id).size(), 2);
    EXPECT_EQ(a.engine->members(id).size(), 2);

    owner.type(id, 0, 0, QStringLiteral("O:"));
    ASSERT_TRUE(waitFor([&] { return a.text(id) == QStringLiteral("O:start A"); }));
    QTest::qWait(300);
    EXPECT_EQ(b.text(id), QStringLiteral("start A")) << "the revoked device reads nothing new";
    EXPECT_EQ(b.engine->shares()[0].epoch, epochBefore);

    // and what it still writes goes nowhere
    b.type(id, 0, 0, QStringLiteral("B:"));
    ASSERT_TRUE(waitFor([&] { return owner.engine->counters(id).dropped > 0; }));
    QTest::qWait(200);
    EXPECT_EQ(owner.text(id), QStringLiteral("O:start A"));
    EXPECT_EQ(a.text(id), QStringLiteral("O:start A"));
}

TEST(ShareEngineTest, OfflineEditsOnBothSidesMergeOnReconnect)
{
    Peer owner(QStringLiteral("a")), joiner(QStringLiteral("b"));
    const QByteArray id = shareWith(owner, joiner, QStringLiteral("middle"));
    ASSERT_FALSE(id.isEmpty());
    ASSERT_TRUE(waitFor([&] { return owner.engine->peersHere(id) == 1; }));
    joiner.restart();
    ASSERT_TRUE(waitFor([&] { return owner.engine->peersHere(id) == 0; }));
    owner.type(id, 0, 0, QStringLiteral("owner "));

    ASSERT_TRUE(joiner.engine->start()) << joiner.engine->error().toStdString();
    ASSERT_NE(joiner.engine->doc(id), nullptr) << "the share comes back from the store";
    EXPECT_EQ(joiner.text(id), QStringLiteral("middle"));
    joiner.type(id, 6, 0, QStringLiteral(" joiner"));
    EXPECT_EQ(joiner.text(id), QStringLiteral("middle joiner"));
    EXPECT_EQ(owner.text(id), QStringLiteral("owner middle"));

    joiner.engine->dial(QStringLiteral("127.0.0.1"), owner.engine->port(), owner.publicKey());
    ASSERT_TRUE(waitFor([&] { return owner.text(id) == joiner.text(id) && owner.text(id).contains(QStringLiteral("owner")) && owner.text(id).contains(QStringLiteral("joiner")); }));
    EXPECT_EQ(owner.text(id), joiner.text(id));
}

TEST(ShareEngineTest, ShareStateSurvivesARestart)
{
    Peer owner(QStringLiteral("a"));
    QString error;
    const auto created = owner.engine->create(QStringLiteral("Notes/note.md"), QStringLiteral("Kept"), QStringLiteral("one"), &error);
    ASSERT_TRUE(created);
    owner.engine->setPath(created->shareID, QStringLiteral("Notes/moved.md"));
    owner.engine->doc(created->shareID)->localEdit(3, 0, QStringLiteral(" two"));
    owner.engine->persist(created->shareID, QStringLiteral("agreed on disk"));
    owner.engine->doc(created->shareID)->localEdit(7, 0, QStringLiteral(" three")); // only the destructor sees this one
    const ShareRecord before = owner.engine->shares()[0];

    owner.restart();
    ASSERT_TRUE(owner.engine->start()) << owner.engine->error().toStdString();
    ASSERT_EQ(owner.engine->shares().size(), 1);
    const ShareRecord after = owner.engine->shares()[0];
    EXPECT_EQ(after.shareID, before.shareID);
    EXPECT_EQ(after.clientID, before.clientID);
    EXPECT_EQ(after.key, before.key);
    EXPECT_EQ(after.epoch, before.epoch);
    EXPECT_EQ(after.members, before.members);
    EXPECT_EQ(after.owner, before.owner);
    EXPECT_EQ(after.backend, before.backend);
    EXPECT_EQ(after.title, QStringLiteral("Kept"));
    EXPECT_EQ(after.relativePath, QStringLiteral("Notes/moved.md"));
    EXPECT_TRUE(after.owned);
    EXPECT_EQ(owner.text(created->shareID), QStringLiteral("one two three"));
    EXPECT_EQ(ShareStore(owner.dir.path()).base(created->shareID), QStringLiteral("agreed on disk"));
    EXPECT_TRUE(owner.engine->shareFor(QStringLiteral("Notes/moved.md")));
    EXPECT_FALSE(owner.engine->shareFor(QStringLiteral("Notes/note.md")));

    QString again;
    EXPECT_FALSE(owner.engine->create(QStringLiteral("Notes/moved.md"), QStringLiteral("x"), QStringLiteral("y"), &again));
    EXPECT_FALSE(again.isEmpty());
}

TEST(ShareEngineTest, OversizedAndMalformedEnvelopesAreDropped)
{
    Peer owner(QStringLiteral("a")), joiner(QStringLiteral("b"));
    const QByteArray id = shareWith(owner, joiner, QStringLiteral("alive"));
    ASSERT_FALSE(id.isEmpty());
    const ShareRecord record = owner.engine->shares()[0];
    RawMember raw(QStringLiteral("raw"));
    ASSERT_TRUE(raw.connectTo(owner));

    const int before = owner.engine->counters().dropped;
    ASSERT_TRUE(raw.send(QByteArray(ShareFrame::kMaxBytes, '\0')));    // as big as the link lets it be
    ASSERT_TRUE(raw.send(QByteArray(200, '\xff')));                    // not protobuf
    Envelope shortSender;                                              // a Frame whose sender is 3 bytes
    shortSender.mutable_frame()->set_share_id(ShareFrame::toS(id));
    shortSender.mutable_frame()->set_epoch(record.epoch);
    shortSender.mutable_frame()->set_sender("abc");
    shortSender.mutable_frame()->set_seq(1);
    shortSender.mutable_frame()->set_box("x");
    shortSender.mutable_frame()->set_sig(std::string(64, 's'));
    ASSERT_TRUE(raw.send(ShareFrame::toQ(shortSender.SerializeAsString()), hearth::Channel::sync));
    ASSERT_TRUE(waitFor([&] { return owner.engine->counters().dropped == before + 3; }));
    QTest::qWait(300);
    EXPECT_EQ(owner.engine->counters().dropped, before + 3);

    owner.type(id, 5, 0, QStringLiteral(" still"));
    ASSERT_TRUE(waitFor([&] { return joiner.text(id) == QStringLiteral("alive still"); })) << "the engine kept working";
}

TEST(ShareEngineTest, TitlesAndNamesFromThePeerAreSanitised)
{
    Peer owner(QStringLiteral("a")), joiner(QStringLiteral("b"));
    QString error;
    const auto created = owner.engine->create(QStringLiteral("note.md"), QStringLiteral("../x\n") + QChar(0x202e) + QStringLiteral("/y"), QStringLiteral("t"), &error);
    ASSERT_TRUE(created);
    ASSERT_TRUE(joiner.engine->join(created->inviteCode, QStringLiteral("Bo\nb\x07 the 1st"), &error));
    ASSERT_TRUE(waitFor([&] { return owner.requests.size() == 1; }));
    EXPECT_EQ(owner.requests[0].name, QStringLiteral("Bob the 1st"));
    owner.engine->approve(created->shareID, owner.requests[0].joiner);
    ASSERT_TRUE(waitFor([&] { return !joiner.joined.isEmpty(); }));
    EXPECT_EQ(joiner.joined[0].second, QStringLiteral("../x/y")) << "control and bidi characters are gone; turning it into a file name is the window's";
    EXPECT_FALSE(joiner.joined[0].second.contains(QLatin1Char('\n')));
}

TEST(ShareEngineTest, ShareKeyIsNeverLoggedOrSentInClear)
{
    Peer owner(QStringLiteral("a")), joiner(QStringLiteral("b"));
    ASSERT_TRUE(owner.engine->start());
    ASSERT_TRUE(hearth::startDebugLog());
    const QString logPath = QString::fromStdString(hearth::debugLogPath());
    const QString text = QStringLiteral("TOP-SECRET-NOTE-TEXT-7731");

    QString error;
    const auto created = owner.engine->create(QStringLiteral("note.md"), QStringLiteral("T"), text, &error);
    ASSERT_TRUE(created);
    const QByteArray key = owner.engine->shares()[0].key;
    RawMember spy(QStringLiteral("spy")); // a member that records every envelope it is sent
    ASSERT_TRUE(spy.connectTo(owner));
    ASSERT_TRUE(spy.send(validJoin(spy, created->inviteCode, created->shareID, owner.engine->shares()[0].backend)));
    ASSERT_TRUE(waitFor([&] { return owner.requests.size() == 1; }));
    owner.engine->approve(created->shareID, spy.key());
    ASSERT_TRUE(waitFor([&] { return spy.count(Envelope::kWelcome) == 1; }));
    owner.type(created->shareID, text.size(), 0, QStringLiteral(" MORE-SECRET-TEXT-5519"));
    ASSERT_TRUE(waitFor([&] { return spy.count(Envelope::kFrame) >= 1; }));
    owner.engine->revoke(created->shareID, spy.key());
    QTest::qWait(300);
    hearth::stopDebugLog();

    for (const QByteArray &bytes : spy.rawInbox()) {
        EXPECT_FALSE(bytes.contains(key)) << "the share key crossed the wire in clear";
        EXPECT_FALSE(bytes.contains("TOP-SECRET")) << "note text crossed the wire in clear";
        EXPECT_FALSE(bytes.contains("MORE-SECRET"));
    }
    QFile log(logPath);
    ASSERT_TRUE(log.open(QIODevice::ReadOnly)) << logPath.toStdString();
    const QByteArray logged = log.readAll();
    EXPECT_FALSE(logged.isEmpty()) << "the log was off, so this checked nothing";
    EXPECT_FALSE(logged.contains(key.toHex()));
    EXPECT_FALSE(logged.contains(key));
    EXPECT_FALSE(logged.contains("SECRET"));
    EXPECT_FALSE(logged.contains(owner.engine->shares()[0].shareID.toHex())) << "a share id is a secret-ish handle too";
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    // Never the real keyring (a silent secret-service costs 15 s a call).
    secrets::setStore({[](const QString &key, QByteArray *value) {
                           if (!memoryStore().contains(key))
                               return false;
                           *value = memoryStore().value(key);
                           return true;
                       },
                       [](const QString &key, const QByteArray &value) { return memoryStore().insert(key, value) != memoryStore().end(); },
                       [](const QString &key) { return memoryStore().remove(key) > 0; }});
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
