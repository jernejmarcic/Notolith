#include "NoteCrypt.h"

#include <QFileInfo>

#include <sodium.h>

#ifdef Q_OS_LINUX
#include <sys/prctl.h>
#endif

namespace noteAppC::crypt {

namespace {

constexpr char kMagic[] = "NAPCLOCK";
constexpr int kMagicSize = 8;
constexpr quint8 kVersion = 1;
constexpr quint8 kAlgorithm = 1;
constexpr int kSaltSize = crypto_pwhash_SALTBYTES; // 16
constexpr int kNonceSize = crypto_aead_xchacha20poly1305_ietf_NPUBBYTES; // 24
constexpr int kHeaderSize = kMagicSize + 1 + 1 + 4 + 4 + kSaltSize + kNonceSize;
constexpr int kKeySize = crypto_aead_xchacha20poly1305_ietf_KEYBYTES; // 32

void putU32(QByteArray &out, quint32 value)
{
    for (int i = 0; i < 4; ++i)
        out.append(char((value >> (8 * i)) & 0xff));
}

quint32 getU32(const QByteArray &in, int at)
{
    quint32 value = 0;
    for (int i = 0; i < 4; ++i)
        value |= quint32(quint8(in.at(at + i))) << (8 * i);
    return value;
}

} // namespace

bool isEncryptedNote(const QString &path)
{
    return QFileInfo(path).suffix().compare(encryptedSuffix(), Qt::CaseInsensitive) == 0;
}

QString encryptedSuffix()
{
    return QStringLiteral("mdlock");
}

bool available()
{
    static const bool ready = sodium_init() >= 0;
    return ready;
}

Key::~Key()
{
    if (m_bytes)
        sodium_free(m_bytes); // zeroes it first
}

Key::Key(Key &&other) noexcept
    : m_bytes(std::exchange(other.m_bytes, nullptr))
    , m_salt(std::move(other.m_salt))
    , m_params(other.m_params)
{
}

Key &Key::operator=(Key &&other) noexcept
{
    if (this != &other) {
        if (m_bytes)
            sodium_free(m_bytes);
        m_bytes = std::exchange(other.m_bytes, nullptr);
        m_salt = std::move(other.m_salt);
        m_params = other.m_params;
    }
    return *this;
}

QByteArray newSalt()
{
    QByteArray salt(kSaltSize, Qt::Uninitialized);
    if (available())
        randombytes_buf(salt.data(), size_t(salt.size()));
    return salt;
}

Key derive(const QString &password, const QByteArray &salt, Params params)
{
    Key key;
    if (!available() || salt.size() != kSaltSize)
        return key;
    // Outside the guarded page only for as long as the call takes.
    QByteArray utf8 = password.toUtf8();
    auto *bytes = static_cast<unsigned char *>(sodium_malloc(kKeySize));
    if (bytes
        && crypto_pwhash(bytes, kKeySize, utf8.constData(), size_t(utf8.size()),
                         reinterpret_cast<const unsigned char *>(salt.constData()), params.opslimit,
                         size_t(params.memlimitKiB) * 1024, crypto_pwhash_ALG_ARGON2ID13)
               == 0) {
        key.m_bytes = bytes;
        key.m_salt = salt;
        key.m_params = params;
    } else if (bytes) {
        sodium_free(bytes);
    }
    sodium_memzero(utf8.data(), size_t(utf8.size()));
    return key;
}

std::optional<FileHeader> header(const QByteArray &file)
{
    if (file.size() < kHeaderSize + int(crypto_aead_xchacha20poly1305_ietf_ABYTES)
        || !file.startsWith(QByteArray(kMagic, kMagicSize)) || quint8(file.at(kMagicSize)) != kVersion
        || quint8(file.at(kMagicSize + 1)) != kAlgorithm)
        return std::nullopt;
    FileHeader out;
    out.params.opslimit = getU32(file, kMagicSize + 2);
    out.params.memlimitKiB = getU32(file, kMagicSize + 6);
    out.salt = file.mid(kMagicSize + 10, kSaltSize);
    // A file cannot make this process derive with absurd parameters.
    if (out.params.opslimit < crypto_pwhash_OPSLIMIT_MIN || out.params.opslimit > 64
        || size_t(out.params.memlimitKiB) * 1024 < crypto_pwhash_MEMLIMIT_MIN
        || out.params.memlimitKiB > 4u * 1024 * 1024)
        return std::nullopt;
    return out;
}

QByteArray seal(const Key &key, const QByteArray &plaintext)
{
    if (!key.isValid())
        return {};
    QByteArray file;
    file.reserve(kHeaderSize + plaintext.size() + int(crypto_aead_xchacha20poly1305_ietf_ABYTES));
    file.append(kMagic, kMagicSize);
    file.append(char(kVersion));
    file.append(char(kAlgorithm));
    putU32(file, key.params().opslimit);
    putU32(file, key.params().memlimitKiB);
    file.append(key.salt());
    QByteArray nonce(kNonceSize, Qt::Uninitialized);
    randombytes_buf(nonce.data(), size_t(nonce.size()));
    file.append(nonce);

    const int headerSize = int(file.size());
    file.resize(headerSize + plaintext.size() + int(crypto_aead_xchacha20poly1305_ietf_ABYTES));
    unsigned long long written = 0;
    crypto_aead_xchacha20poly1305_ietf_encrypt(
        reinterpret_cast<unsigned char *>(file.data()) + headerSize, &written,
        reinterpret_cast<const unsigned char *>(plaintext.constData()), size_t(plaintext.size()),
        reinterpret_cast<const unsigned char *>(file.constData()), size_t(headerSize), // the header, as AD
        nullptr, reinterpret_cast<const unsigned char *>(nonce.constData()), key.bytes());
    file.resize(headerSize + int(written));
    return file;
}

std::optional<QByteArray> open(const Key &key, const QByteArray &file)
{
    const std::optional<FileHeader> head = header(file);
    if (!key.isValid() || !head || head->salt != key.salt())
        return std::nullopt;
    const auto *nonce = reinterpret_cast<const unsigned char *>(file.constData()) + kHeaderSize - kNonceSize;
    const qsizetype bodySize = file.size() - kHeaderSize;
    QByteArray plain(bodySize - qsizetype(crypto_aead_xchacha20poly1305_ietf_ABYTES), Qt::Uninitialized);
    unsigned long long length = 0;
    if (crypto_aead_xchacha20poly1305_ietf_decrypt(
            reinterpret_cast<unsigned char *>(plain.data()), &length, nullptr,
            reinterpret_cast<const unsigned char *>(file.constData()) + kHeaderSize, size_t(bodySize),
            reinterpret_cast<const unsigned char *>(file.constData()), size_t(kHeaderSize), nonce, key.bytes())
        != 0)
        return std::nullopt;
    plain.resize(qsizetype(length));
    return plain;
}

void wipe(QByteArray &bytes)
{
    if (!bytes.isEmpty())
        sodium_memzero(bytes.data(), size_t(bytes.size()));
    bytes.clear();
}

void setDumpable(bool dumpable)
{
#ifdef Q_OS_LINUX
    prctl(PR_SET_DUMPABLE, dumpable ? 1 : 0);
#else
    Q_UNUSED(dumpable);
#endif
}

} // namespace noteAppC::crypt
