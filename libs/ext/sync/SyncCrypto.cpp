// SPDX-License-Identifier: GPL-3.0-or-later
#include "SyncCrypto.h"

#include <array>

#include <sodium.h>

namespace noteAppC {

namespace {

/// The padding bucket table (docs/SYNC.md "Padding"): journal payloads sit
/// in the first two, blobs use the rest, up to 512 MiB.
constexpr std::array<qint64, 11> kBuckets = {
    1024, 4096, 16384, 65536, 262144, 1 << 20, 4 << 20, 16 << 20, 64 << 20, 256 << 20, 512 << 20,
};

} // namespace

bool SyncCrypto::init()
{
    return sodium_init() >= 0;
}

QByteArray SyncCrypto::randomSalt()
{
    QByteArray salt(kSaltBytes, Qt::Uninitialized);
    randombytes_buf(salt.data(), size_t(salt.size()));
    return salt;
}

QByteArray SyncCrypto::randomKey()
{
    QByteArray key(kKeyBytes, Qt::Uninitialized);
    randombytes_buf(key.data(), size_t(key.size()));
    return key;
}

QByteArray SyncCrypto::randomID()
{
    QByteArray id(kIDBytes, Qt::Uninitialized);
    randombytes_buf(id.data(), size_t(id.size()));
    return id;
}

QByteArray SyncCrypto::deriveMasterKey(const QByteArray &password, const QByteArray &salt)
{
    if (salt.size() != kSaltBytes)
        return {};
    QByteArray out(kKeyBytes, Qt::Uninitialized);
    // Standard Notes 004: opslimit 5, memlimit 64 MiB, Argon2id. libsodium's
    // Argon2id is single-lane, matching the spec's "1 lane" with nothing to
    // set for it.
    const int rc = crypto_pwhash(reinterpret_cast<unsigned char *>(out.data()), out.size(),
                                 password.constData(), size_t(password.size()),
                                 reinterpret_cast<const unsigned char *>(salt.constData()), 5,
                                 64ull * 1024 * 1024, crypto_pwhash_ALG_ARGON2ID13);
    if (rc != 0)
        return {};
    return out;
}

QByteArray SyncCrypto::deriveSubkey(const QByteArray &masterKey, const char context[9])
{
    if (masterKey.size() != kKeyBytes)
        return {};
    QByteArray out(kKeyBytes, Qt::Uninitialized);
    const int rc = crypto_kdf_derive_from_key(reinterpret_cast<unsigned char *>(out.data()), size_t(out.size()),
                                              0, context,
                                              reinterpret_cast<const unsigned char *>(masterKey.constData()));
    if (rc != 0)
        return {};
    return out;
}

QByteArray SyncCrypto::buildAD(const char tag[2], const QList<QByteArray> &parts)
{
    QByteArray ad;
    ad.append(char(1)); // protocol version
    ad.append(tag, 2);
    for (const QByteArray &part : parts)
        ad += part;
    return ad;
}

QByteArray SyncCrypto::seal(const QByteArray &key, const QByteArray &plaintext, const QByteArray &ad)
{
    if (key.size() != kKeyBytes)
        return {};
    QByteArray nonce(crypto_aead_xchacha20poly1305_ietf_NPUBBYTES, Qt::Uninitialized);
    randombytes_buf(nonce.data(), size_t(nonce.size()));
    QByteArray cipher(plaintext.size() + crypto_aead_xchacha20poly1305_ietf_ABYTES, Qt::Uninitialized);
    unsigned long long cipherLen = 0;
    crypto_aead_xchacha20poly1305_ietf_encrypt(
        reinterpret_cast<unsigned char *>(cipher.data()), &cipherLen,
        reinterpret_cast<const unsigned char *>(plaintext.constData()), (unsigned long long)plaintext.size(),
        reinterpret_cast<const unsigned char *>(ad.constData()), (unsigned long long)ad.size(), nullptr,
        reinterpret_cast<const unsigned char *>(nonce.constData()),
        reinterpret_cast<const unsigned char *>(key.constData()));
    cipher.resize(int(cipherLen));
    return nonce + cipher;
}

QByteArray SyncCrypto::open(const QByteArray &key, const QByteArray &sealed, const QByteArray &ad, bool *ok)
{
    if (ok)
        *ok = false;
    constexpr int nonceBytes = crypto_aead_xchacha20poly1305_ietf_NPUBBYTES;
    if (key.size() != kKeyBytes || sealed.size() < nonceBytes + crypto_aead_xchacha20poly1305_ietf_ABYTES)
        return {};
    const QByteArray nonce = sealed.left(nonceBytes);
    const QByteArray cipher = sealed.mid(nonceBytes);
    QByteArray plain(cipher.size(), Qt::Uninitialized);
    unsigned long long plainLen = 0;
    const int rc = crypto_aead_xchacha20poly1305_ietf_decrypt(
        reinterpret_cast<unsigned char *>(plain.data()), &plainLen, nullptr,
        reinterpret_cast<const unsigned char *>(cipher.constData()), (unsigned long long)cipher.size(),
        reinterpret_cast<const unsigned char *>(ad.constData()), (unsigned long long)ad.size(),
        reinterpret_cast<const unsigned char *>(nonce.constData()),
        reinterpret_cast<const unsigned char *>(key.constData()));
    if (rc != 0)
        return {};
    plain.resize(int(plainLen));
    if (ok)
        *ok = true;
    return plain;
}

QByteArray SyncCrypto::contentAddress(const QByteArray &contentKey, const QByteArray &paddedPlaintext)
{
    QByteArray out(kAddressBytes, Qt::Uninitialized);
    crypto_generichash(reinterpret_cast<unsigned char *>(out.data()), size_t(out.size()),
                       reinterpret_cast<const unsigned char *>(paddedPlaintext.constData()),
                       size_t(paddedPlaintext.size()),
                       reinterpret_cast<const unsigned char *>(contentKey.constData()),
                       size_t(contentKey.size()));
    return out;
}

QByteArray SyncCrypto::digest(const QByteArray &plaintext)
{
    return contentAddress({}, plaintext);
}

QString SyncCrypto::fingerprint(const QByteArray &publicKey)
{
    QByteArray digest(10, Qt::Uninitialized);
    crypto_generichash(reinterpret_cast<unsigned char *>(digest.data()), size_t(digest.size()),
                       reinterpret_cast<const unsigned char *>(publicKey.constData()), size_t(publicKey.size()),
                       nullptr, 0);
    QStringList groups;
    const QString hex = QString::fromLatin1(digest.toHex().toUpper());
    for (qsizetype i = 0; i < hex.size(); i += 4)
        groups << hex.mid(i, 4);
    return groups.join(QLatin1Char('-'));
}

qint64 SyncCrypto::bucketFor(qint64 size)
{
    for (const qint64 bucket : kBuckets) {
        if (size < bucket) // strictly less: pad() needs room for at least one padding byte
            return bucket;
    }
    return kBuckets.back();
}

QByteArray SyncCrypto::pad(const QByteArray &plaintext)
{
    const qint64 bucket = bucketFor(plaintext.size());
    QByteArray buf = plaintext;
    buf.resize(int(bucket));
    size_t paddedLen = 0;
    if (sodium_pad(&paddedLen, reinterpret_cast<unsigned char *>(buf.data()), size_t(plaintext.size()),
                   size_t(bucket), size_t(bucket))
        != 0)
        return {};
    buf.resize(int(paddedLen));
    return buf;
}

QByteArray SyncCrypto::unpad(const QByteArray &padded, qint64 bucketSize)
{
    size_t unpaddedLen = 0;
    if (sodium_unpad(&unpaddedLen, reinterpret_cast<const unsigned char *>(padded.constData()),
                     size_t(padded.size()), size_t(bucketSize))
        != 0)
        return {};
    return padded.left(int(unpaddedLen));
}

bool SyncCrypto::sealStream(const QByteArray &key, const QByteArray &ad, const std::function<QByteArray()> &next,
                            const std::function<void(const QByteArray &)> &sink)
{
    if (key.size() != kKeyBytes)
        return false;
    crypto_secretstream_xchacha20poly1305_state state;
    QByteArray header(crypto_secretstream_xchacha20poly1305_HEADERBYTES, Qt::Uninitialized);
    if (crypto_secretstream_xchacha20poly1305_init_push(&state, reinterpret_cast<unsigned char *>(header.data()),
                                                        reinterpret_cast<const unsigned char *>(key.constData()))
        != 0)
        return false;
    sink(header);

    QByteArray chunk = next();
    while (true) {
        const QByteArray following = chunk.isEmpty() ? QByteArray() : next();
        const unsigned char tag = following.isEmpty() ? crypto_secretstream_xchacha20poly1305_TAG_FINAL
                                                       : crypto_secretstream_xchacha20poly1305_TAG_MESSAGE;
        QByteArray cipher(chunk.size() + int(crypto_secretstream_xchacha20poly1305_ABYTES), Qt::Uninitialized);
        unsigned long long cipherLen = 0;
        crypto_secretstream_xchacha20poly1305_push(
            &state, reinterpret_cast<unsigned char *>(cipher.data()), &cipherLen,
            reinterpret_cast<const unsigned char *>(chunk.constData()), (unsigned long long)chunk.size(),
            reinterpret_cast<const unsigned char *>(ad.constData()), (unsigned long long)ad.size(), tag);
        cipher.resize(int(cipherLen));
        sink(cipher);
        if (tag == crypto_secretstream_xchacha20poly1305_TAG_FINAL)
            break;
        chunk = following;
    }
    return true;
}

bool SyncCrypto::openStream(const QByteArray &key, const QByteArray &ad, const std::function<QByteArray()> &next,
                            const std::function<void(const QByteArray &)> &sink)
{
    if (key.size() != kKeyBytes)
        return false;
    const QByteArray header = next();
    if (header.size() != crypto_secretstream_xchacha20poly1305_HEADERBYTES)
        return false;
    crypto_secretstream_xchacha20poly1305_state state;
    if (crypto_secretstream_xchacha20poly1305_init_pull(&state, reinterpret_cast<const unsigned char *>(header.constData()),
                                                        reinterpret_cast<const unsigned char *>(key.constData()))
        != 0)
        return false;

    for (;;) {
        const QByteArray cipher = next();
        if (cipher.isEmpty())
            return false; // ended without TAG_FINAL: truncated or tampered
        QByteArray plain(cipher.size(), Qt::Uninitialized);
        unsigned long long plainLen = 0;
        unsigned char tag = 0;
        const int rc = crypto_secretstream_xchacha20poly1305_pull(
            &state, reinterpret_cast<unsigned char *>(plain.data()), &plainLen, &tag,
            reinterpret_cast<const unsigned char *>(cipher.constData()), (unsigned long long)cipher.size(),
            reinterpret_cast<const unsigned char *>(ad.constData()), (unsigned long long)ad.size());
        if (rc != 0)
            return false;
        plain.resize(int(plainLen));
        sink(plain);
        if (tag == crypto_secretstream_xchacha20poly1305_TAG_FINAL)
            return true;
    }
}

QByteArray SyncCrypto::sealBlob(const QByteArray &key, const QByteArray &ad, const QByteArray &plaintext)
{
    QByteArray framed;
    qsizetype offset = 0;
    const bool ok = sealStream(
        key, ad,
        [&]() -> QByteArray {
            if (offset >= plaintext.size())
                return {};
            const QByteArray chunk = plaintext.mid(int(offset), int(kStreamChunk));
            offset += chunk.size();
            return chunk;
        },
        [&](const QByteArray &c) { framed += c; });
    return ok ? framed : QByteArray();
}

QByteArray SyncCrypto::openBlob(const QByteArray &key, const QByteArray &ad, const QByteArray &framed, bool *ok)
{
    if (ok)
        *ok = false;
    QByteArray plain;
    qsizetype pos = 0;
    bool headerSent = false;
    const bool streamOk = openStream(
        key, ad,
        [&]() -> QByteArray {
            if (!headerSent) {
                headerSent = true;
                if (framed.size() < kStreamHeaderBytes)
                    return {};
                const QByteArray header = framed.left(kStreamHeaderBytes);
                pos = kStreamHeaderBytes;
                return header;
            }
            const qsizetype remaining = framed.size() - pos;
            if (remaining <= 0)
                return {};
            // A full non-final chunk is always exactly this many bytes;
            // whatever is left once fewer than that remain is the final,
            // possibly shorter, chunk - see the header comment.
            const qsizetype full = kStreamChunk + kStreamTagBytes;
            const qsizetype take = remaining > full ? full : remaining;
            const QByteArray chunk = framed.mid(int(pos), int(take));
            pos += chunk.size();
            return chunk;
        },
        [&](const QByteArray &c) { plain += c; });
    if (!streamOk)
        return {};
    if (ok)
        *ok = true;
    return plain;
}

SyncCrypto::BoxKeyPair SyncCrypto::generateBoxKeyPair()
{
    BoxKeyPair pair;
    pair.publicKey.resize(kBoxPublicBytes);
    pair.secretKey.resize(kBoxSecretBytes);
    crypto_box_keypair(reinterpret_cast<unsigned char *>(pair.publicKey.data()),
                       reinterpret_cast<unsigned char *>(pair.secretKey.data()));
    return pair;
}

QByteArray SyncCrypto::sealToBox(const QByteArray &publicKey, const QByteArray &plaintext)
{
    if (publicKey.size() != kBoxPublicBytes)
        return {};
    QByteArray out(plaintext.size() + crypto_box_SEALBYTES, Qt::Uninitialized);
    crypto_box_seal(reinterpret_cast<unsigned char *>(out.data()),
                    reinterpret_cast<const unsigned char *>(plaintext.constData()), size_t(plaintext.size()),
                    reinterpret_cast<const unsigned char *>(publicKey.constData()));
    return out;
}

QByteArray SyncCrypto::openBox(const BoxKeyPair &pair, const QByteArray &sealed, bool *ok)
{
    if (ok)
        *ok = false;
    if (pair.publicKey.size() != kBoxPublicBytes || pair.secretKey.size() != kBoxSecretBytes
        || sealed.size() < crypto_box_SEALBYTES)
        return {};
    QByteArray plain(sealed.size() - crypto_box_SEALBYTES, Qt::Uninitialized);
    const int rc = crypto_box_seal_open(reinterpret_cast<unsigned char *>(plain.data()),
                                        reinterpret_cast<const unsigned char *>(sealed.constData()),
                                        size_t(sealed.size()),
                                        reinterpret_cast<const unsigned char *>(pair.publicKey.constData()),
                                        reinterpret_cast<const unsigned char *>(pair.secretKey.constData()));
    if (rc != 0)
        return {};
    if (ok)
        *ok = true;
    return plain;
}

QByteArray SyncCrypto::hashLoginKey(const QByteArray &loginKey)
{
    QByteArray out(crypto_pwhash_STRBYTES, Qt::Uninitialized);
    const int rc = crypto_pwhash_str(out.data(), loginKey.constData(), size_t(loginKey.size()),
                                     crypto_pwhash_OPSLIMIT_INTERACTIVE, crypto_pwhash_MEMLIMIT_INTERACTIVE);
    if (rc != 0)
        return {};
    out.resize(int(qstrlen(out.constData()))); // crypto_pwhash_str is a NUL-terminated C string
    return out;
}

bool SyncCrypto::verifyLoginKey(const QByteArray &hash, const QByteArray &loginKey)
{
    // crypto_pwhash_str_verify wants a NUL-terminated string; hash never
    // contains an embedded NUL (it is ASCII, libsodium's own str format).
    return crypto_pwhash_str_verify(hash.constData(), loginKey.constData(), size_t(loginKey.size())) == 0;
}

bool SyncCrypto::equal(const QByteArray &a, const QByteArray &b)
{
    return a.size() == b.size()
        && sodium_memcmp(a.constData(), b.constData(), size_t(a.size())) == 0;
}

void SyncCrypto::wipe(QByteArray &bytes)
{
    if (!bytes.isEmpty())
        sodium_memzero(bytes.data(), size_t(bytes.size()));
}

} // namespace noteAppC
