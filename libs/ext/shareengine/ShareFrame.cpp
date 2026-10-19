#include "ShareFrame.h"

#include <span>

#include <QSet>

namespace noteAppC::ShareFrame {

using namespace notolith::share;

namespace {

std::span<const std::uint8_t> view(const QByteArray &b)
{
    return {reinterpret_cast<const std::uint8_t *>(b.constData()), size_t(b.size())};
}

QByteArray membersInput(const std::string &body)
{
    return QByteArrayLiteral("notolith-members-v1") + toQ(body);
}

QByteArray frameInput(const QByteArray &shareID, quint32 epoch, const QByteArray &sender, quint64 seq, const QByteArray &box)
{
    return QByteArrayLiteral("notolith-frame-v1") + shareID + be32(epoch) + sender + be64(seq) + box;
}

QByteArray frameAD(const QByteArray &shareID, quint32 epoch, const QByteArray &sender, quint64 seq)
{
    return SyncCrypto::buildAD("sf", {shareID, be32(epoch), sender, be64(seq)});
}

bool fail(QString *reason, const char *why)
{
    if (reason)
        *reason = QString::fromLatin1(why);
    return false;
}

} // namespace

QByteArray be32(quint32 value)
{
    QByteArray out(4, Qt::Uninitialized);
    for (int i = 0; i < 4; ++i)
        out[i] = char((value >> (24 - 8 * i)) & 0xff);
    return out;
}

QByteArray be64(quint64 value)
{
    QByteArray out(8, Qt::Uninitialized);
    for (int i = 0; i < 8; ++i)
        out[i] = char((value >> (56 - 8 * i)) & 0xff);
    return out;
}

QString cleanText(const QString &text, int maxChars)
{
    QString out;
    out.reserve(std::min<qsizetype>(text.size(), maxChars + 1));
    for (const QChar c : text) {
        const QChar::Category cat = c.category();
        if (cat == QChar::Other_Control || cat == QChar::Other_Format || cat == QChar::Separator_Line
            || cat == QChar::Separator_Paragraph)
            continue;
        out += c;
    }
    out = out.trimmed();
    if (out.size() > maxChars) {
        qsizetype cut = maxChars;
        if (out.at(cut - 1).isHighSurrogate())
            --cut;
        out.truncate(cut);
        out = out.trimmed();
    }
    return out;
}

std::optional<Envelope> parseEnvelope(const QByteArray &bytes)
{
    if (bytes.size() > kMaxBytes)
        return std::nullopt;
    Envelope envelope;
    if (!envelope.ParseFromArray(bytes.constData(), int(bytes.size())))
        return std::nullopt;
    return envelope;
}

QByteArray seal(const ShareIdentity &me, const QByteArray &shareID, quint32 epoch, quint64 seq,
                const QByteArray &shareKey, Kind kind, const QByteArray &data)
{
    Payload payload;
    if (kind == Kind::Update)
        payload.set_update(toS(data));
    else
        payload.set_state_vector(toS(data));
    const QByteArray sender = me.publicKey();
    const QByteArray box = SyncCrypto::seal(shareKey, toQ(payload.SerializeAsString()), frameAD(shareID, epoch, sender, seq));
    const auto sig = me.link.sign(view(frameInput(shareID, epoch, sender, seq, box)));
    Envelope envelope;
    Frame *frame = envelope.mutable_frame();
    frame->set_share_id(toS(shareID));
    frame->set_epoch(epoch);
    frame->set_sender(toS(sender));
    frame->set_seq(seq);
    frame->set_box(toS(box));
    frame->set_sig(std::string(sig.begin(), sig.end()));
    return toQ(envelope.SerializeAsString());
}

std::optional<Opened> open(const Frame &frame, const QByteArray &shareKey, const QByteArray &shareID, quint32 epoch,
                           const QByteArray &linkPeer, quint64 lastSeq, QString *reason)
{
    if (shareKey.size() != SyncCrypto::kKeyBytes || linkPeer.size() != 32) {
        fail(reason, "bad arguments");
        return std::nullopt;
    }
    if (toQ(frame.share_id()) != shareID) {
        fail(reason, "another share");
        return std::nullopt;
    }
    if (frame.epoch() != epoch) {
        fail(reason, "wrong epoch");
        return std::nullopt;
    }
    const QByteArray sender = toQ(frame.sender());
    if (sender.size() != 32 || sender != linkPeer) {
        fail(reason, "sender is not the peer");
        return std::nullopt;
    }
    if (frame.seq() <= lastSeq) {
        fail(reason, "replayed or out of order");
        return std::nullopt;
    }
    const QByteArray box = toQ(frame.box());
    const std::string &sig = frame.sig();
    if (sig.size() != 64
        || !hearth::verify(toPK(sender), view(frameInput(shareID, epoch, sender, frame.seq(), box)),
                           {reinterpret_cast<const std::uint8_t *>(sig.data()), sig.size()})) {
        fail(reason, "bad signature");
        return std::nullopt;
    }
    bool ok = false;
    const QByteArray plain = SyncCrypto::open(shareKey, box, frameAD(shareID, epoch, sender, frame.seq()), &ok);
    if (!ok) {
        fail(reason, "does not decrypt");
        return std::nullopt;
    }
    Payload payload;
    if (!payload.ParseFromArray(plain.constData(), int(plain.size())) || payload.kind_case() == Payload::KIND_NOT_SET) {
        fail(reason, "bad payload");
        return std::nullopt;
    }
    Opened opened;
    opened.seq = frame.seq();
    opened.kind = payload.kind_case() == Payload::kUpdate ? Kind::Update : Kind::StateVector;
    opened.data = toQ(opened.kind == Kind::Update ? payload.update() : payload.state_vector());
    return opened;
}

QByteArray signMembers(const hearth::Identity &owner, const MemberListBody &body)
{
    MemberList list;
    list.set_body(body.SerializeAsString());
    const auto sig = owner.sign(view(membersInput(list.body())));
    list.set_sig(std::string(sig.begin(), sig.end()));
    return toQ(list.SerializeAsString());
}

std::optional<MemberListBody> verifyMembers(const MemberList &list, const QByteArray &owner)
{
    if (owner.size() != 32 || list.sig().size() != 64 || list.body().size() > kMaxBytes)
        return std::nullopt;
    const QByteArray input = membersInput(list.body());
    if (!hearth::verify(toPK(owner), view(input),
                        {reinterpret_cast<const std::uint8_t *>(list.sig().data()), list.sig().size()}))
        return std::nullopt;
    MemberListBody body;
    if (!body.ParseFromString(list.body()))
        return std::nullopt;
    if (toQ(body.owner()) != owner || body.share_id().size() != SyncCrypto::kIDBytes
        || (body.backend() != kBackendYrs && body.backend() != kBackendNative) || body.members_size() < 1
        || body.members_size() > kMaxMembers)
        return std::nullopt;
    QSet<QByteArray> seen;
    for (const Member &m : body.members()) {
        if (m.key().size() != 32 || m.box_key().size() != SyncCrypto::kBoxPublicBytes
            || QString::fromStdString(m.name()).size() > kMaxNameChars || seen.contains(toQ(m.key())))
            return std::nullopt;
        seen.insert(toQ(m.key()));
    }
    if (!seen.contains(owner))
        return std::nullopt;
    return body;
}

std::optional<MemberListBody> verifyMembers(const QByteArray &memberList, const QByteArray &owner)
{
    MemberList list;
    if (memberList.size() > kMaxBytes || !list.ParseFromArray(memberList.constData(), int(memberList.size())))
        return std::nullopt;
    return verifyMembers(list, owner);
}

} // namespace noteAppC::ShareFrame
