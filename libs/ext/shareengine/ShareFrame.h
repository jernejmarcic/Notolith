// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej Marčič
#pragma once

#include <optional>
#include <string>

#include <QByteArray>
#include <QString>

#include "ShareIdentity.h"
#include "share.pb.h"

namespace noteAppC::ShareFrame {

/// The largest envelope either side accepts or sends: the link's own cap
/// (libs/ext/link/net/node.cpp, maxMessageSize), checked before anything is
/// parsed.
constexpr int kMaxBytes = 1 << 20;
constexpr int kMaxNameChars = 64;
constexpr int kMaxMembers = 256;
constexpr int kBackendYrs = 1;
constexpr int kBackendNative = 2;

QByteArray be32(quint32 value);
QByteArray be64(quint64 value);

inline QByteArray toQ(const std::string &s) { return QByteArray(s.data(), qsizetype(s.size())); }
inline std::string toS(const QByteArray &b) { return std::string(b.constData(), size_t(b.size())); }

/// Text that came off the wire, made safe to show and to build a name from:
/// control, format and line/paragraph separator characters dropped, trimmed,
/// at most `maxChars` UTF-16 units (never splitting a surrogate pair).
QString cleanText(const QString &text, int maxChars);

/// Nullopt for anything over kMaxBytes (without looking inside it) or that
/// does not parse.
std::optional<notolith::share::Envelope> parseEnvelope(const QByteArray &bytes);

enum class Kind { Update, StateVector, Diff, Presence };
struct Opened {
    quint64 seq = 0;
    Kind kind = Kind::Update;
    QByteArray data;
};

/// An Envelope holding one Frame: `data` sealed under shareKey, bound to
/// (shareID, epoch, sender, seq), and signed by `me`.
QByteArray seal(const ShareIdentity &me, const QByteArray &shareID, quint32 epoch, quint64 seq,
                const QByteArray &shareKey, Kind kind, const QByteArray &data);

/// Every check a received Frame must pass, cheapest first and the signature
/// before any decryption: the share, the epoch, a sender that is the peer on
/// the other end of the link, a sequence number beyond `lastSeq`, the sender's
/// signature, the AEAD and a well-formed payload. Whether the sender is a
/// member is the caller's. On failure `*reason` (if given) says which.
std::optional<Opened> open(const notolith::share::Frame &frame, const QByteArray &shareKey, const QByteArray &shareID,
                           quint32 epoch, const QByteArray &linkPeer, quint64 lastSeq, QString *reason = nullptr);

/// The MemberList message bytes: `body` signed by the owner.
QByteArray signMembers(const hearth::Identity &owner, const notolith::share::MemberListBody &body);

/// The body, once the owner's signature verified and its shape is sane (ids
/// and keys of the right size, names short, no duplicate member, the owner a
/// member). Nothing inside is parsed before the signature checks.
std::optional<notolith::share::MemberListBody> verifyMembers(const notolith::share::MemberList &list,
                                                             const QByteArray &owner);
std::optional<notolith::share::MemberListBody> verifyMembers(const QByteArray &memberList, const QByteArray &owner);

} // namespace noteAppC::ShareFrame
