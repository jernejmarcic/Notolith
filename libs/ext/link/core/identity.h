#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace hearth {

class SignedDeviceCert; // proto/hearth.proto; the full type is only needed in core/identity.cpp

using Bytes = std::vector<std::uint8_t>;
using PublicKey = std::array<std::uint8_t, 32>;

// A device key's link to its account (the user, plan 2026-09-30). The account
// key signs deviceCertInput(account, device); createdMS and name are labels,
// signed only as part of a DeviceList (core/account.h). A device without a
// cert is its own account, which is what every identity made before accounts
// existed is: its user ID, contacts and safety numbers stay the same.
struct DeviceCert {
    PublicKey account{}, device{};
    std::int64_t createdMS = 0;
    std::string name;
    Bytes sig; // empty only for the implicit self-cert (account == device)
    bool operator==(const DeviceCert&) const = default;
};

// "hearth-device-v1" || account || device
Bytes deviceCertInput(const PublicKey& account, const PublicKey& device);
// The implicit self-cert (account == device, no signature), or signed by the account.
bool validCert(const DeviceCert& cert);

// The cert on the wire (Signal.cert and friends). fromWire checks shape only
// (key sizes); the signature is validCert's.
SignedDeviceCert toWire(const DeviceCert& cert);
std::optional<DeviceCert> fromWire(const SignedDeviceCert& cert);
// The account a signed cert chains `device` to, or nothing if it does not.
std::optional<PublicKey> certifiedAccount(const SignedDeviceCert& cert, const PublicKey& device);

// One Ed25519 key pair per device. Its account (the cert's, else the key
// itself) is the user's identity; the display name is only a label.
class Identity {
public:
    static Identity generate();
    // secretKey is libsodium's 64-byte Ed25519 secret key (seed + public key).
    static std::optional<Identity> fromSecretKey(std::span<const std::uint8_t> secretKey);

    const PublicKey& publicKey() const { return pk_; }
    std::span<const std::uint8_t> secretKey() const { return sk_; }
    Bytes sign(std::span<const std::uint8_t> msg) const;

    // The account this device speaks for: itself unless certified.
    const PublicKey& account() const { return cert_.account; }
    const DeviceCert& cert() const { return cert_; }
    // Only for this device; the signature is the receivers' to check (validCert).
    bool setCert(DeviceCert cert);

    ~Identity();
    Identity(Identity&&) noexcept;
    Identity& operator=(Identity&&) noexcept;
    Identity(const Identity&) = delete;
    Identity& operator=(const Identity&) = delete;

private:
    Identity() = default;
    PublicKey pk_{};
    std::array<std::uint8_t, 64> sk_{};
    DeviceCert cert_;
};

bool verify(const PublicKey& pk, std::span<const std::uint8_t> msg,
            std::span<const std::uint8_t> sig);

// "hearth-login-v2" || node || nonce || username: what the login key signs to
// prove possession of the password (or recovery code), and what a node checks
// the signature against. node is the key of the node being logged into (its
// signed hello), so a node cannot relay another node's challenge (A13).
Bytes loginProveInput(std::span<const std::uint8_t> nonce, const std::string& username, const PublicKey& node);

// 3-32 chars, lowercase ascii [a-z0-9._-] only (no confusables).
bool validUsernameShape(const std::string& name);

// Short hex fingerprint shown in UI and published in the mDNS TXT record.
std::string fingerprint(const PublicKey& pk);

// Same 30 digits on both sides regardless of who computes it; compared out of
// band to rule out a man in the middle on first contact.
std::string safetyNumber(const PublicKey& a, const PublicKey& b);

} // namespace hearth
