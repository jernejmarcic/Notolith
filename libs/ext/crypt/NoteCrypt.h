#pragma once

#include <QByteArray>
#include <QString>

#include <optional>

namespace noteAppC::crypt {

/// A note encrypted on disk: `Name.mdlock` in place of `Name.md`. Its text
/// only ever exists decrypted in memory (docs/ENCRYPTION.md).
///
/// The file, all of it bound into the AEAD tag so no byte of it can change
/// unnoticed:
///
///     "NAPCLOCK"  8   magic
///     version     1   1
///     algorithm   1   1 = Argon2id v1.3 + XChaCha20-Poly1305-IETF
///     opslimit    4   Argon2id passes, little endian
///     memlimit    4   Argon2id memory in KiB, little endian
///     salt       16   Argon2id salt
///     nonce      24   random, never reused under one key
///     body        *   the note's UTF-8 bytes, encrypted, then its 16-byte tag
bool isEncryptedNote(const QString &path);
QString encryptedSuffix(); ///< "mdlock"

/// How hard the password is stretched. Stored in every file, so it can be
/// raised later without breaking old notes.
struct Params {
    quint32 opslimit = 3;                  ///< libsodium's MODERATE
    quint32 memlimitKiB = 256 * 1024;      ///< 256 MiB, libsodium's MODERATE
};

/// A key derived from a password, in memory libsodium locks out of swap and
/// wipes when it goes. Move only.
class Key {
public:
    Key() = default;
    ~Key();
    Key(Key &&other) noexcept;
    Key &operator=(Key &&other) noexcept;
    Key(const Key &) = delete;
    Key &operator=(const Key &) = delete;

    bool isValid() const { return m_bytes != nullptr; }
    const QByteArray &salt() const { return m_salt; }
    Params params() const { return m_params; }
    const unsigned char *bytes() const { return m_bytes; }

private:
    friend Key derive(const QString &password, const QByteArray &salt, Params params);
    unsigned char *m_bytes = nullptr;
    QByteArray m_salt;
    Params m_params;
};

/// False when libsodium could not start; nothing here works then.
bool available();
QByteArray newSalt();
/// Argon2id over the password's UTF-8 bytes. Invalid when the derivation
/// fails (out of memory for the params asked).
Key derive(const QString &password, const QByteArray &salt, Params params = {});

/// What a file was sealed with: the salt and params its key needs. Empty
/// when the bytes are not an encrypted note.
struct FileHeader {
    QByteArray salt;
    Params params;
};
std::optional<FileHeader> header(const QByteArray &file);

/// The whole file for `plaintext` under `key`, with a fresh nonce.
QByteArray seal(const Key &key, const QByteArray &plaintext);
/// The plaintext, or nothing when the key is wrong or any byte of the file
/// was changed.
std::optional<QByteArray> open(const Key &key, const QByteArray &file);

/// While anything is unlocked, the process cannot be dumped to disk (Linux:
/// no core file, no ptrace attach by another user's process).
void setDumpable(bool dumpable);

} // namespace noteAppC::crypt
