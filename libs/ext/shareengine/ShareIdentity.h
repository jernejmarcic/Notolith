// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <optional>

#include <QByteArray>
#include <QString>

#include "SyncCrypto.h"
#include "core/identity.h"

namespace noteAppC {

/// 32 bytes either way; toPK needs exactly 32 (callers check the size of
/// anything that came off the wire first).
QByteArray fromPK(const hearth::PublicKey &key);
hearth::PublicKey toPK(const QByteArray &key);

/// What this device is to the people it shares with: an Ed25519 key (what the
/// link authenticates and what signs frames and member lists) and an X25519
/// key (what a share key is sealed to). Neither is derived from the other, so
/// no conversion code exists.
struct ShareIdentity {
    hearth::Identity link;
    SyncCrypto::BoxKeyPair box;

    /// Keyring service "noteAppC-share", entry `key`: Ed25519 secret (64) ||
    /// X25519 secret (32) || X25519 public (32). Mints and stores one when
    /// the entry is absent or malformed. Nullopt only when the keyring
    /// neither has an entry nor accepts one. Runs a keyring job on the
    /// calling thread's event loop (up to 15 s when the backend hangs).
    static std::optional<ShareIdentity> load(const QString &key = QStringLiteral("device-identity-v1"));

    QByteArray publicKey() const { return fromPK(link.publicKey()); }
    QString fingerprint() const;
};

} // namespace noteAppC
