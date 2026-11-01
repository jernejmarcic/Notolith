// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej
#include "ShareIdentity.h"

#include <cassert>
#include <cstring>

#include "SecretStore.h"

namespace noteAppC {

namespace {

const QString kService = QStringLiteral("noteAppC-share");
constexpr int kLinkSecretBytes = 64;
constexpr int kBlobBytes = kLinkSecretBytes + SyncCrypto::kBoxSecretBytes + SyncCrypto::kBoxPublicBytes;

std::optional<ShareIdentity> fromBlob(const QByteArray &blob)
{
    if (blob.size() != kBlobBytes)
        return std::nullopt;
    const auto *p = reinterpret_cast<const std::uint8_t *>(blob.constData());
    auto link = hearth::Identity::fromSecretKey({p, size_t(kLinkSecretBytes)});
    if (!link)
        return std::nullopt;
    SyncCrypto::BoxKeyPair box{blob.mid(kLinkSecretBytes + SyncCrypto::kBoxSecretBytes), blob.mid(kLinkSecretBytes, SyncCrypto::kBoxSecretBytes)};
    // A public half that does not belong to the secret half would fail every
    // join much later, so prove the pair once here.
    bool ok = false;
    const QByteArray probe = SyncCrypto::randomKey();
    SyncCrypto::openBox(box, SyncCrypto::sealToBox(box.publicKey, probe), &ok);
    if (!ok)
        return std::nullopt;
    return ShareIdentity{std::move(*link), std::move(box)};
}

} // namespace

QByteArray fromPK(const hearth::PublicKey &key)
{
    return QByteArray(reinterpret_cast<const char *>(key.data()), qsizetype(key.size()));
}

hearth::PublicKey toPK(const QByteArray &key)
{
    assert(key.size() == 32);
    hearth::PublicKey out{};
    std::memcpy(out.data(), key.constData(), std::min<size_t>(out.size(), size_t(key.size())));
    return out;
}

QString ShareIdentity::fingerprint() const
{
    return QString::fromStdString(hearth::fingerprint(link.publicKey()));
}

std::optional<ShareIdentity> ShareIdentity::load(const QString &key)
{
    if (!SyncCrypto::init())
        return std::nullopt;
    QByteArray blob;
    if (secrets::read(kService, key, &blob)) {
        auto found = fromBlob(blob);
        SyncCrypto::wipe(blob);
        if (found)
            return found;
    }
    ShareIdentity fresh{hearth::Identity::generate(), SyncCrypto::generateBoxKeyPair()};
    const auto secret = fresh.link.secretKey();
    blob = QByteArray(reinterpret_cast<const char *>(secret.data()), qsizetype(secret.size()))
         + fresh.box.secretKey + fresh.box.publicKey;
    const bool stored = secrets::write(kService, key, blob);
    SyncCrypto::wipe(blob);
    if (!stored)
        return std::nullopt;
    return fresh;
}

} // namespace noteAppC
