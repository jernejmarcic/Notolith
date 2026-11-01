// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej
#pragma once

#include <functional>

#include <QByteArray>
#include <QList>

namespace noteAppC {

/// Every libsodium call sync makes, in one place, so nothing else in
/// libs/ext/sync links <sodium.h> directly. See docs/SYNC.md for the key
/// hierarchy and the parameters below, and why each one was picked.
///
/// Nothing here is thread-affine; every function is a pure transform over
/// the bytes it is given. Caller-owned: nothing is cached, nothing is
/// zeroed automatically - use wipe() on anything key-shaped once done with
/// it (a QByteArray's own destructor does not scrub its heap buffer).
class SyncCrypto {
public:
    static constexpr int kKeyBytes = 32;     ///< AEAD keys, itemsKey, noteKey, masterKey, wrapKey...
    static constexpr int kSaltBytes = 16;    ///< crypto_pwhash_SALTBYTES
    static constexpr int kIDBytes = 16;      ///< entityID, recordID, deviceID, itemsKeyID
    static constexpr int kAddressBytes = 32; ///< keyed content address
    static constexpr int kBoxPublicBytes = 32;
    static constexpr int kBoxSecretBytes = 32;

    /// sodium_init(), once, before anything else in this class is called.
    /// Safe to call more than once. Returns false only if libsodium itself
    /// failed to initialise (never happens in practice; checked anyway,
    /// because every key derived after a silent failure would be garbage).
    static bool init();

    // --- key hierarchy (docs/SYNC.md "Key hierarchy") --------------------

    static QByteArray randomSalt();
    static QByteArray randomKey();  ///< 32 random bytes: itemsKey, noteKey
    static QByteArray randomID();   ///< 16 random bytes: entityID, recordID, deviceID, itemsKeyID

    /// Standard Notes 004 Argon2id parameters: opslimit 5, memlimit 64 MiB,
    /// Argon2id (single-lane, which is all libsodium's Argon2id ever runs).
    /// `password` is UTF-8. Empty on failure (out-of-memory is the only
    /// realistic cause at 64 MiB).
    static QByteArray deriveMasterKey(const QByteArray &password, const QByteArray &salt);

    /// The three fixed contexts a masterKey is ever split into. Each must
    /// be passed to deriveSubkey() exactly as spelled (8 bytes, checked by
    /// crypto_kdf_derive_from_key itself, which crashes suggestively if it
    /// is not - length-fixed C arrays here so a typo is a compile error).
    static constexpr char kContextLogin[9] = "login001";
    static constexpr char kContextWrap[9] = "wrapkey1";
    static constexpr char kContextContent[9] = "content1";
    static QByteArray deriveSubkey(const QByteArray &masterKey, const char context[9]);

    // --- AEAD records (docs/SYNC.md "Wrapping (AEAD)") --------------------

    /// Builds associated data as version-byte + two-letter tag + every part
    /// concatenated, so every caller's AD is unambiguous about which field
    /// is which (a bare concatenation of variable-length fields would let
    /// "ab"+"c" collide with "a"+"bc"). See docs/SYNC.md for each call
    /// site's tag and fields.
    static QByteArray buildAD(const char tag[2], const QList<QByteArray> &parts);

    /// nonce || ciphertext || tag, one string. seal() cannot fail (random
    /// nonce, libsodium's own encrypt only fails on absurd lengths). open()
    /// returns an empty QByteArray and sets *ok=false on any failure -
    /// wrong key, tampered ciphertext, or AD that does not match what it
    /// was sealed with (a swapped record between two entities fails here).
    static QByteArray seal(const QByteArray &key, const QByteArray &plaintext, const QByteArray &ad);
    static QByteArray open(const QByteArray &key, const QByteArray &sealed, const QByteArray &ad, bool *ok);

    // --- content addressing and padding ------------------------------

    /// Keyed BLAKE2b-256 over the padded plaintext. Keyed so a party
    /// without contentKey cannot precompute the address of a guessed
    /// plaintext - see docs/SYNC.md "Content addresses" for what this does
    /// and does not hide.
    static QByteArray contentAddress(const QByteArray &contentKey, const QByteArray &paddedPlaintext);

    /// The same BLAKE2b-256 with no key: a state's name inside a sealed
    /// record (docs/SYNC.md "Conflict rules"). Unkeyed on purpose - a
    /// keyed digest sealed into a record could only be checked by the
    /// device that wrote it, and contentKey is per device.
    static QByteArray digest(const QByteArray &plaintext);

    /// Smallest bucket that fits `size` (docs/SYNC.md's table), and the
    /// pad()/unpad() pair built on it (ISO/IEC 7816-4 via sodium_pad).
    static qint64 bucketFor(qint64 size);
    static QByteArray pad(const QByteArray &plaintext);
    /// `bucketSize` must be the value pad() rounded up to (its own output
    /// length) - unpad() does not re-derive it, so a caller stores/sends
    /// the padded length alongside the ciphertext, which it already must
    /// for a normal AEAD decrypt.
    static QByteArray unpad(const QByteArray &padded, qint64 bucketSize);

    // --- secretstream, for blobs above one chunk ---------------------

    static constexpr qint64 kStreamChunk = 64 * 1024;
    static constexpr int kStreamHeaderBytes = 24; ///< crypto_secretstream_xchacha20poly1305_HEADERBYTES
    static constexpr int kStreamTagBytes = 17;    ///< crypto_secretstream_xchacha20poly1305_ABYTES (per chunk)

    /// Reads plaintext chunks from `next` (an empty QByteArray signals end
    /// of input) until exhausted, writes framed ciphertext chunks
    /// (secretstream's own header first) to `sink`. `ad` binds the blob's
    /// identity into every chunk. False only on an internal libsodium
    /// failure (never seen in practice - included for completeness and so
    /// the caller has somewhere to route "something is very wrong").
    static bool sealStream(const QByteArray &key, const QByteArray &ad,
                           const std::function<QByteArray()> &next,
                           const std::function<void(const QByteArray &)> &sink);
    /// Reverse of sealStream(). `next` yields ciphertext chunks exactly as
    /// sealStream() framed them (first call must be the header). Returns
    /// false - and calls `sink` with nothing further - the moment a chunk
    /// fails to verify, or if the stream ends without a TAG_FINAL chunk (a
    /// truncated download must not be handed to the caller as if it were
    /// the whole file).
    static bool openStream(const QByteArray &key, const QByteArray &ad,
                           const std::function<QByteArray()> &next,
                           const std::function<void(const QByteArray &)> &sink);

    /// Whole-buffer convenience over sealStream()/openStream(), for the
    /// common case of "encrypt this blob" rather than streaming chunk by
    /// chunk - this is what SyncEngine and the blob store actually call.
    /// The framed bytes are self-delimiting at fixed kStreamChunk
    /// boundaries (a full ciphertext chunk is always exactly
    /// kStreamChunk + kStreamTagBytes bytes; whatever is left at the end,
    /// shorter than that, is the final chunk), so no length prefix is
    /// needed on top.
    static QByteArray sealBlob(const QByteArray &key, const QByteArray &ad, const QByteArray &plaintext);
    static QByteArray openBlob(const QByteArray &key, const QByteArray &ad, const QByteArray &framed, bool *ok);

    // --- X25519 sealed-box lockboxes ----------------------------------

    struct BoxKeyPair {
        QByteArray publicKey;
        QByteArray secretKey;
    };
    static BoxKeyPair generateBoxKeyPair();
    static QByteArray sealToBox(const QByteArray &publicKey, const QByteArray &plaintext);
    static QByteArray openBox(const BoxKeyPair &pair, const QByteArray &sealed, bool *ok);
    /// A device's public key as a code a person can compare between two
    /// screens: BLAKE2b, 80 bits, "3F2A-91C0-5B7E-0D44-A1C9". Approving a
    /// device whose code matches is what keeps a server from slipping in a
    /// key of its own.
    static QString fingerprint(const QByteArray &publicKey);

    // --- server-side login key storage --------------------------------

    /// crypto_pwhash_str of a login key: interactive limits, since the
    /// login key already carries 256 bits of Argon2id-derived entropy -
    /// this is defence in depth on a stolen accounts table, not the
    /// primary stretch (docs/SYNC.md "Deriving masterKey" / "Subkeys").
    /// Used by tools/sync_server, declared here so both sides of the
    /// protocol agree on the one wrapper around crypto_pwhash_str.
    static QByteArray hashLoginKey(const QByteArray &loginKey);
    static bool verifyLoginKey(const QByteArray &hash, const QByteArray &loginKey);

    /// Constant-time equality (length first: lengths are not secret). For
    /// comparing anything a peer proves it knows - never operator==.
    static bool equal(const QByteArray &a, const QByteArray &b);

    /// sodium_memzero in place. A QByteArray's own destructor does not
    /// scrub its heap buffer, so anything key- or password-shaped is
    /// wiped explicitly once no longer needed.
    static void wipe(QByteArray &bytes);
};

} // namespace noteAppC
