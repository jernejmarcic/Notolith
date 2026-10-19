#include "core/identity.h"

#include "link.pb.h"

#include <sodium.h>

#include <algorithm>
#include <stdexcept>
#include <string_view>

namespace hearth {

namespace {
void ensureSodium()
{
    static const int rc = sodium_init();
    if (rc < 0)
        throw std::runtime_error("sodium_init failed");
}
} // namespace

Identity Identity::generate()
{
    ensureSodium();
    Identity id;
    crypto_sign_keypair(id.pk_.data(), id.sk_.data());
    id.cert_.account = id.cert_.device = id.pk_;
    return id;
}

std::optional<Identity> Identity::fromSecretKey(std::span<const std::uint8_t> secretKey)
{
    ensureSodium();
    if (secretKey.size() != crypto_sign_SECRETKEYBYTES)
        return std::nullopt;
    // Rebuild the pair from the seed half and require the stored public half to
    // match; a mismatch means the blob is corrupt, not a key we should sign with.
    // (crypto_sign_ed25519_sk_to_pk only copies bytes 32..63, so it can't check this.)
    Identity id;
    crypto_sign_seed_keypair(id.pk_.data(), id.sk_.data(), secretKey.data());
    if (sodium_memcmp(id.pk_.data(), secretKey.data() + 32, 32) != 0)
        return std::nullopt;
    id.cert_.account = id.cert_.device = id.pk_;
    return id;
}

Identity::~Identity() { sodium_memzero(sk_.data(), sk_.size()); }

Identity::Identity(Identity&& o) noexcept : pk_(o.pk_), sk_(o.sk_), cert_(std::move(o.cert_))
{
    sodium_memzero(o.sk_.data(), o.sk_.size());
}

Identity& Identity::operator=(Identity&& o) noexcept
{
    if (this != &o) {
        pk_ = o.pk_;
        sk_ = o.sk_;
        cert_ = std::move(o.cert_);
        sodium_memzero(o.sk_.data(), o.sk_.size());
    }
    return *this;
}

Bytes Identity::sign(std::span<const std::uint8_t> msg) const
{
    Bytes sig(crypto_sign_BYTES);
    crypto_sign_detached(sig.data(), nullptr, msg.data(), msg.size(), sk_.data());
    return sig;
}

bool Identity::setCert(DeviceCert cert)
{
    if (cert.device != pk_)
        return false;
    cert_ = std::move(cert);
    return true;
}

Bytes deviceCertInput(const PublicKey& account, const PublicKey& device)
{
    static constexpr std::string_view label = "hearth-device-v1";
    Bytes b(label.begin(), label.end());
    b.insert(b.end(), account.begin(), account.end());
    b.insert(b.end(), device.begin(), device.end());
    return b;
}

bool validCert(const DeviceCert& c)
{
    if (c.account == c.device && c.sig.empty())
        return true;
    return verify(c.account, deviceCertInput(c.account, c.device), c.sig);
}

bool verify(const PublicKey& pk, std::span<const std::uint8_t> msg,
            std::span<const std::uint8_t> sig)
{
    ensureSodium();
    if (sig.size() != crypto_sign_BYTES)
        return false;
    return crypto_sign_verify_detached(sig.data(), msg.data(), msg.size(), pk.data()) == 0;
}

std::string fingerprint(const PublicKey& pk)
{
    ensureSodium();
    std::uint8_t h[8];
    crypto_generichash(h, sizeof h, pk.data(), pk.size(), nullptr, 0);
    char hex[sizeof h * 2 + 1];
    sodium_bin2hex(hex, sizeof hex, h, sizeof h);
    return hex;
}

std::string safetyNumber(const PublicKey& a, const PublicKey& b)
{
    ensureSodium();
    const auto& lo = std::min(a, b);
    const auto& hi = std::max(a, b);
    std::uint8_t h[30];
    crypto_generichash_state st;
    crypto_generichash_init(&st, nullptr, 0, sizeof h);
    crypto_generichash_update(&st, lo.data(), lo.size());
    crypto_generichash_update(&st, hi.data(), hi.size());
    crypto_generichash_final(&st, h, sizeof h);
    std::string out;
    for (std::size_t i = 0; i < sizeof h; ++i) {
        if (i && i % 5 == 0)
            out += ' ';
        out += char('0' + h[i] % 10); // ponytail: mod-10 bias ~2%, fine for a visual compare
    }
    return out;
}

bool validUsernameShape(const std::string& name)
{
    if (name.size() < 3 || name.size() > 32)
        return false;
    return std::ranges::all_of(name, [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
    });
}

SignedDeviceCert toWire(const DeviceCert& c)
{
    SignedDeviceCert w;
    w.set_account(std::string(c.account.begin(), c.account.end()));
    w.set_device(std::string(c.device.begin(), c.device.end()));
    w.set_created_ms(c.createdMS);
    w.set_name(c.name);
    w.set_sig(std::string(c.sig.begin(), c.sig.end()));
    return w;
}

std::optional<DeviceCert> fromWire(const SignedDeviceCert& w)
{
    if (w.account().size() != 32 || w.device().size() != 32 || w.name().size() > 64)
        return std::nullopt;
    DeviceCert c{{}, {}, w.created_ms(), w.name(), Bytes(w.sig().begin(), w.sig().end())};
    std::copy(w.account().begin(), w.account().end(), c.account.begin());
    std::copy(w.device().begin(), w.device().end(), c.device.begin());
    return c;
}

std::optional<PublicKey> certifiedAccount(const SignedDeviceCert& w, const PublicKey& device)
{
    auto c = fromWire(w);
    if (!c || c->device != device || !validCert(*c))
        return std::nullopt;
    return c->account;
}

Bytes loginProveInput(std::span<const std::uint8_t> nonce, const std::string& username, const PublicKey& node)
{
    static constexpr std::string_view label = "hearth-login-v2";
    Bytes b(label.begin(), label.end());
    b.insert(b.end(), node.begin(), node.end());
    b.insert(b.end(), nonce.begin(), nonce.end());
    b.insert(b.end(), username.begin(), username.end());
    return b;
}

} // namespace hearth
