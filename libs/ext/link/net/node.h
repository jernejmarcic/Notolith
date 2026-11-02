// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "core/identity.h"

#include <functional>
#include <memory>
#include <string>

namespace rtc {
class PeerConnection;
}

namespace hearth {

// Turns an IPv4-mapped IPv6 address (::ffff:a.b.c.d, what a dual-stack listener
// hands back for an IPv4 client) into plain IPv4; identity otherwise.
std::string unmapV4(const std::string& host);
// Loopback, RFC1918 LAN, and the tailnet/CGNAT range (100.64.0.0/10 and its
// IPv6 ULA). Used to keep an ungated node off the public internet by default.
bool isPrivateHost(const std::string& host);

// Empty if `listen` is fine to bind without a gate (no address given, or a
// private/loopback/tailnet one); otherwise the message to print before
// refusing to start (D1/N12: no free pass for an explicit public --listen).
std::optional<std::string> publicListenRefusal(const std::string& listen, bool gated);

// ctl: small ordered app messages. sync: gossip. blob: file chunks (unordered).
enum class Channel : std::uint8_t { ctl, sync, blob };

// One authenticated peer connection per remote identity, over libdatachannel.
// Callbacks run on libdatachannel threads; set them before start().
class Node {
public:
    struct Config {
        std::uint16_t port = 47800; // 0 = any
        bool mDNS = true;
        bool tailscale = false;      // dial every online tailnet peer
        // HTTPS (wss) signaling, e.g. with the machine's Tailscale certificate.
        std::string TLScertFile, TLSkeyFile, TLShost;
        std::uint16_t TLSport = 47801;
        bool verifyTLS = true;
        // Bind signaling (plain and TLS) and ICE to one address, e.g. a WireGuard IP;
        // empty = every address. "0.0.0.0" / "::" bind all IPv4 / all addresses.
        std::string listen;
        // Local UDP ports WebRTC may use, so a firewall can allow exactly these.
        std::uint16_t UDPportBegin = 1024, UDPportEnd = 65535;
        // Internet mode: rate-limit table entries, one per distinct address
        // seen. Full table evicts the oldest address rather than refuse.
        std::size_t gatedAddressTableSize = 65536;
        // What mDNS publishes and browses: nodes of different serviceType never see each
        // other, which is how two apps sharing this layer stay out of each other's way.
        // The instance name is serviceNamePrefix + the key's fingerprint.
        std::string serviceType = "_hearth._tcp";
        std::string serviceNamePrefix = "hearth-";
        // Prefixes what the handshake signs ("<domain>-offer", -answer, -hello, -proof),
        // so nodes of different apps cannot complete a dial even on a shared network.
        // Both ends must agree; "hearth" is Hearth's wire format.
        std::string signatureDomain = "hearth";
    };

    Node(const Identity& self, Config config);
    ~Node();
    Node(const Node&) = delete;
    Node& operator=(const Node&) = delete;

    void start();
    std::uint16_t port() const;
    // Where peers reach our wss listener, if one is running.
    std::optional<std::pair<std::string, std::uint16_t>> TLSendpoint() const;
    const PublicKey& self() const;

    // Remembers the endpoint and keeps it connected. expected, if set, must match
    // the device that answers or the account its cert chains it to.
    void dial(const std::string& host, std::uint16_t port,
              std::optional<PublicKey> expected = std::nullopt, bool TLS = false);

    // Drops every link; dialed endpoints come back by themselves, each with a
    // fresh hello (accounts: after this device was certified).
    void reconnect();
    bool send(const PublicKey& peer, Channel channel, std::span<const std::uint8_t> data);
    std::size_t bufferedAmount(const PublicKey& peer, Channel channel) const;
    std::vector<PublicKey> peers() const; // device keys
    // The account a connected device's cert chains it to (from its signaling);
    // the key itself for a device without one, or one not connected.
    PublicKey accountOf(const PublicKey& peer) const;
    // A connected peer's address bucket (an IPv4 address or IPv6 /64), for
    // per-address limits; empty if not connected.
    std::string addressOf(const PublicKey& peer) const;
    std::shared_ptr<rtc::PeerConnection> connection(const PublicKey& peer) const;

    std::function<void(const PublicKey&)> onPeerUp;
    std::function<void(const PublicKey&)> onPeerDown;
    std::function<void(const PublicKey&, Channel, std::span<const std::uint8_t>)> onMessage;
    // Called for every peer connection before negotiation, on both sides, so the
    // same media tracks exist on both ends (matched by mid).
    std::function<void(const PublicKey&, rtc::PeerConnection&)> onSetupTracks;
    // Internet mode when set. An incoming connection must first send a 96-byte proof
    // (its key and a signature over our nonce; 192 bytes with its account and the
    // account's cert signature) within 5 s, before any protobuf is parsed, from a
    // device or account this accepts; stricter per-address limits apply. Called
    // with proven keys only (account == device without a cert), without the node's
    // lock held.
    std::function<bool(const PublicKey& device, const PublicKey& account)> allowPeer;

    // Password login: a gated node's own pre-proof
    // exchange, tried instead of the 96/192-byte proof above (an ungated
    // node: instead of the offer) when the first message is exactly 32 bytes (a zero-padded username, [a-z0-9._-],
    // 3-32 chars). Both callbacks must be set together, or neither - unset
    // (e.g. --no-password-login) makes such a frame fail exactly like a
    // malformed proof. Two more messages follow, all on the same raw
    // connection, none of them ever becoming a live peer connection:
    //   client -> node: 32 bytes, the username (onLoginBegin); 33 bytes with
    //                   a trailing 1 to log in with the recovery code instead
    //   node -> client: 16 (salt) + 32 (nonce) bytes (LoginChallenge)
    //   client -> node: 64 bytes, sig over loginProveInput(nonce, username, node key)
    //   node -> client: the serialized vault record, then the node closes.
    struct LoginChallenge {
        Bytes salt;           // real (net/node.h: 16 bytes) or a stable fake for an unknown name
        PublicKey loginPub{}; // real or a stable fake; a signature check just fails either way
    };
    // Called once per attempt with the username, its address bucket
    // (reusing the same per-address gate above) and whether it is the
    // recovery-code login; nullopt aborts the connection (rate-limited or
    // disabled), never to distinguish a known name from an unknown one - see
    // Relay::onLoginBegin.
    std::function<std::optional<LoginChallenge>(const std::string& username, const std::string& addressKey, bool recovery)>
        onLoginBegin;
    // Called only once the challenge signature verifies against loginPub (so
    // never for an unknown/fake name in practice); the bytes to hand back, or
    // nullopt to abort instead.
    std::function<std::optional<Bytes>(const std::string& username)> onLoginProved;

    struct Impl;

private:
    std::unique_ptr<Impl> d_;
};

} // namespace hearth
