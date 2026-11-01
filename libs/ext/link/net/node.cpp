// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej

#include "net/node.h"

#include "core/debuglog.h"
#include "link.pb.h"
#include "net/tailnet.h"

#include <avahi-client/client.h>
#include <avahi-client/lookup.h>
#include <avahi-client/publish.h>
#include <avahi-common/error.h>
#include <avahi-common/malloc.h>
#include <avahi-common/thread-watch.h>
#include <arpa/inet.h>
#include <rtc/rtc.hpp>
#include <sodium.h>

#include <algorithm>
#include <cstring>
#include <format>
#include <list>
#include <map>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace hearth {

// A dual-stack listener hands back IPv4 clients as ::ffff:a.b.c.d; turn that
// back into plain IPv4 so it buckets and filters the same as a real IPv4 peer.
std::string unmapV4(const std::string& host)
{
    in6_addr a6;
    if (inet_pton(AF_INET6, host.c_str(), &a6) == 1 && IN6_IS_ADDR_V4MAPPED(&a6)) {
        in_addr a4;
        std::memcpy(&a4, reinterpret_cast<const std::uint8_t*>(&a6) + 12, sizeof a4);
        char buf[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &a4, buf, sizeof buf);
        return buf;
    }
    return host;
}

// Loopback, RFC1918 LAN, and the CGNAT range Tailscale/Headscale use
// (100.64.0.0/10, plus its IPv6 ULA fd7a:115c:a1e0::/48 which fc00::/7 covers).
// Not a full "never routable on the internet" test - it is the same shape as
// HOSTING.md's tier-1 addresses, used to keep an ungated node off the public
// internet by default.
bool isPrivateHost(const std::string& hostIn)
{
    const std::string host = unmapV4(hostIn);
    in_addr a4;
    if (inet_pton(AF_INET, host.c_str(), &a4) == 1) {
        const auto* b = reinterpret_cast<const std::uint8_t*>(&a4.s_addr);
        return b[0] == 127                              // 127.0.0.0/8
            || b[0] == 10                                // 10.0.0.0/8
            || (b[0] == 172 && (b[1] & 0xf0) == 16)       // 172.16.0.0/12
            || (b[0] == 192 && b[1] == 168)               // 192.168.0.0/16
            || (b[0] == 100 && (b[1] & 0xc0) == 64);      // 100.64.0.0/10 (CGNAT/tailnet)
    }
    in6_addr a6;
    if (inet_pton(AF_INET6, host.c_str(), &a6) == 1) {
        const auto* b = a6.s6_addr;
        if (std::all_of(b, b + 15, [](std::uint8_t x) { return x == 0; }) && b[15] == 1)
            return true;                            // ::1
        if ((b[0] & 0xfe) == 0xfc)
            return true;                            // fc00::/7 (ULA, incl. Tailscale's fd7a:115c:a1e0::/48)
        if (b[0] == 0xfe && (b[1] & 0xc0) == 0x80)
            return true;                            // fe80::/10 (link-local)
        return false;
    }
    return false; // not a literal address (e.g. a hostname): don't call it private
}

std::optional<std::string> publicListenRefusal(const std::string& listen, bool gated)
{
    if (gated || listen.empty() || isPrivateHost(listen))
        return std::nullopt;
    return "--listen " + listen + " is a public address; refuses to start without --allow-keys (internet mode)";
}

namespace {

using Clock = std::chrono::steady_clock;
constexpr std::size_t channelCount = 3;

// Internet mode (allowPeer set). A proof is the dialer's key and its signature
// over "<domain>-proof" || our nonce || our key: fixed size, so nothing is parsed.
// A device of another account appends account || the account's cert signature.
constexpr std::size_t proofSize = 32 + 64, certProofSize = proofSize + 32 + 64;
constexpr auto proofTimeout = std::chrono::seconds(5); // TCP accept to proof, TLS and upgrade included
// Password login: sized to fall in neither proofSize nor
// certProofSize, so the first message dispatches unambiguously (node.h).
constexpr std::size_t loginUsernameSize = 32;
constexpr std::size_t loginProveSize = 64;
constexpr std::size_t signalingMessageSize = 64 * 1024; // signaling is a few KB of SDP
constexpr int gatedPerAddress = 4;                     // unproven connections per IPv4 address or IPv6 /64
constexpr int gatedUnproven = 64;                      // unproven connections in total
constexpr double gatedBurst = 20, gatedRefill = 0.5;   // new connections per address: burst, then per second

std::span<const std::uint8_t> asBytes(const std::string& s)
{
    return {reinterpret_cast<const std::uint8_t*>(s.data()), s.size()};
}

std::optional<PublicKey> toKey(const std::string& s)
{
    if (s.size() != 32)
        return std::nullopt;
    PublicKey pk;
    std::copy(s.begin(), s.end(), pk.begin());
    return pk;
}

std::string hex(const PublicKey& pk)
{
    char out[65];
    sodium_bin2hex(out, sizeof out, pk.data(), pk.size());
    return out;
}

std::optional<PublicKey> fromHex(const std::string& s)
{
    PublicKey pk;
    std::size_t len = 0;
    if (s.size() != 64 || sodium_hex2bin(pk.data(), pk.size(), s.data(), s.size(), nullptr, &len, nullptr) != 0
        || len != 32)
        return std::nullopt;
    return pk;
}

// libdatachannel's enums only know how to print themselves to a stream.
template <class T>
std::string str(T v)
{
    std::ostringstream o;
    o << v;
    return o.str();
}

// What ICE settled on: local and remote candidate, so a relayed or odd path shows.
void logPair(const PublicKey& pk, rtc::PeerConnection& pc)
{
    rtc::Candidate local, remote;
    if (pc.getSelectedCandidatePair(&local, &remote))
        debugLog("net", std::format("peer {} path {} -> {}", debugKey(pk), std::string(local), std::string(remote)));
}

Bytes signInput(const std::string& domain, std::string_view name, const std::string& nonce, const std::string& sdp)
{
    Bytes b(domain.begin(), domain.end());
    b.push_back('-');
    b.insert(b.end(), name.begin(), name.end());
    b.insert(b.end(), nonce.begin(), nonce.end());
    b.insert(b.end(), sdp.begin(), sdp.end());
    return b;
}

Bytes proofInput(const std::string& domain, const std::string& nonce, const PublicKey& server)
{
    Bytes b = signInput(domain, "proof", nonce, "");
    b.insert(b.end(), server.begin(), server.end());
    return b;
}

// Per-address key for limits: an IPv6 host usually owns a whole /64.
std::string addressKey(const std::string& hostIn)
{
    const std::string host = unmapV4(hostIn);
    in6_addr a6;
    if (inet_pton(AF_INET6, host.c_str(), &a6) == 1)
        return std::string(reinterpret_cast<const char*>(&a6), 8);
    return host;
}

std::string hostOf(const std::shared_ptr<rtc::WebSocket>& ws)
{
    const auto addr = ws->remoteAddress().value_or("");
    // IPv6 comes back bracketed ("[::1]:port") to disambiguate from the port's
    // colon; strip the brackets so inet_pton (and everything downstream) sees
    // a plain address instead of falling through to the opaque-string bucket.
    if (!addr.empty() && addr.front() == '[') {
        const auto close = addr.find(']');
        return close == std::string::npos ? addr.substr(1) : addr.substr(1, close - 1);
    }
    return addr.substr(0, addr.rfind(':'));
}

struct Bucket {
    double tokens = gatedBurst;
    Clock::time_point last = Clock::now();
};

std::string randomNonce()
{
    std::string n(32, '\0');
    randombytes_buf(n.data(), n.size());
    return n;
}

struct Peer {
    std::shared_ptr<rtc::PeerConnection> pc;
    PublicKey account{}; // from the peer's verified cert; its own key if it has none
    std::string address; // addressKey of its signaling host, for per-address limits
    std::array<std::shared_ptr<rtc::DataChannel>, channelCount> ch;
    bool outgoing = false;
    bool up = false;
    int open = 0;
    Clock::time_point started = Clock::now();
};

struct Endpoint {
    std::string host;
    std::uint16_t port;
    std::optional<PublicKey> pk;
    bool manual;
    bool TLS = false;
    Clock::time_point lastDial{};
    int failures = 0; // backoff: 5 s doubling to 5 min while unreachable
};

struct Handshake {
    std::shared_ptr<rtc::WebSocket> ws;
    bool outgoing;
    Endpoint* endpoint = nullptr;
    std::optional<PublicKey> expected;
    std::optional<PublicKey> peerPK;
    std::string myNonce = randomNonce();
    std::string peerNonce;
    std::shared_ptr<rtc::PeerConnection> pc;
    Clock::time_point started = Clock::now();
    bool done = false;
    bool gated = false;              // incoming in internet mode: proof first
    std::optional<PublicKey> proven; // written under the node's lock
    PublicKey provenAccount{};
    std::string address;             // addressKey of the remote host

    // Password login: set once onLoginBegin has answered, so the
    // next message is the prove frame instead of a fresh dispatch.
    std::optional<std::string> loginUsername;
    PublicKey loginPub{};
    std::string loginNonce;
};

} // namespace

struct Node::Impl {
    Node& q;
    const Identity& self;
    Config cfg;

    mutable std::mutex m;
    std::map<PublicKey, Peer> peers;
    std::list<Endpoint> endpoints;
    std::vector<std::shared_ptr<Handshake>> handshakes;
    std::vector<std::shared_ptr<void>> graveyard;
    std::unique_ptr<rtc::WebSocketServer> server, TLSserver;
    std::map<std::string, Bucket> buckets; // internet mode: new connections per address
    Clock::time_point lastTailnetScan{};
    std::jthread maintenance;

    AvahiThreadedPoll* poll = nullptr;
    AvahiClient* avahi = nullptr;
    AvahiEntryGroup* group = nullptr;
    AvahiServiceBrowser* browser = nullptr;

    Impl(Node& node, const Identity& id, Config c) : q(node), self(id), cfg(c) {}

    void bury(Peer& p)
    {
        for (auto& c : p.ch)
            if (c) {
                c->resetCallbacks();
                graveyard.push_back(c);
            }
        p.pc->resetCallbacks();
        p.pc->close();
        graveyard.push_back(p.pc);
    }

    void bury(Handshake& h)
    {
        h.done = true;
        if (h.ws) {
            h.ws->resetCallbacks();
            if (h.gated)
                h.ws->forceClose(); // no close handshake: a stranger gets no lingering socket
            else
                h.ws->close();
            graveyard.push_back(h.ws);
        }
    }

    void sendSignal(rtc::WebSocket& ws, const Signal& s)
    {
        const std::string wire = s.SerializeAsString();
        try {
            ws.send(reinterpret_cast<const std::byte*>(wire.data()), wire.size());
        } catch (const std::exception&) {
        }
    }

    void attachCert(Signal& s) const
    {
        if (self.account() != self.publicKey())
            *s.mutable_cert() = toWire(self.cert());
    }

    // The account a hello, offer or answer says pk speaks for; empty if its cert does not chain.
    static std::optional<PublicKey> accountFrom(const Signal& s, const PublicKey& pk)
    {
        return s.has_cert() ? certifiedAccount(s.cert(), pk) : std::optional(pk);
    }

    // Must hold m. Registers a connecting peer and wires its callbacks.
    std::shared_ptr<rtc::PeerConnection> makePeer(const PublicKey& pk, bool outgoing, const PublicKey& account)
    {
        rtc::Configuration conf;
        conf.disableAutoNegotiation = true;
        conf.maxMessageSize = 1 << 20;
        conf.mtu = 1200; // fits inside WireGuard's 1280 (Tailscale) with room to spare
        if (!cfg.listen.empty() && cfg.listen != "0.0.0.0" && cfg.listen != "::")
            conf.bindAddress = cfg.listen;
        conf.portRangeBegin = cfg.UDPportBegin;
        conf.portRangeEnd = cfg.UDPportEnd;
        auto pc = std::make_shared<rtc::PeerConnection>(conf);
        Peer& p = peers[pk];
        p = Peer{};
        p.pc = pc;
        p.outgoing = outgoing;
        p.account = account;
        rtc::PeerConnection* raw = pc.get();
        pc->onStateChange([this, pk, raw](rtc::PeerConnection::State st) {
            using S = rtc::PeerConnection::State;
            if (debugLogOn())
                debugLog("net", std::format("peer {} connection {}", debugKey(pk), str(st)));
            if (st == S::Disconnected || st == S::Failed || st == S::Closed)
                lost(pk, raw, "connection " + str(st));
        });
        if (debugLogOn())
            pc->onIceStateChange([pk, raw](rtc::PeerConnection::IceState st) {
                debugLog("net", std::format("peer {} ICE {}", debugKey(pk), str(st)));
                if (st == rtc::PeerConnection::IceState::Connected || st == rtc::PeerConnection::IceState::Completed)
                    logPair(pk, *raw);
            });
        if (q.onSetupTracks)
            q.onSetupTracks(pk, *pc);
        return pc;
    }

    // Must hold m. Negotiated channels: both sides create them with fixed ids.
    void makeChannels(const PublicKey& pk)
    {
        Peer& p = peers[pk];
        rtc::PeerConnection* raw = p.pc.get();
        for (std::size_t i = 0; i < channelCount; ++i) {
            rtc::DataChannelInit init;
            init.negotiated = true;
            init.id = std::uint16_t(i);
            init.reliability.unordered = Channel(i) == Channel::blob;
            auto ch = p.pc->createDataChannel(std::to_string(i), init);
            ch->onOpen([this, pk, raw] { channelOpen(pk, raw); });
            ch->onMessage(
                [this, pk, i](rtc::binary data) {
                    if (q.onMessage)
                        q.onMessage(pk, Channel(i), {reinterpret_cast<const std::uint8_t*>(data.data()), data.size()});
                },
                [](rtc::string) {});
            p.ch[i] = ch;
        }
    }

    void channelOpen(const PublicKey& pk, rtc::PeerConnection* raw)
    {
        bool nowUp = false, outgoing = false;
        long long ms = 0;
        {
            std::lock_guard lock(m);
            auto it = peers.find(pk);
            if (it == peers.end() || it->second.pc.get() != raw)
                return;
            if (++it->second.open == int(channelCount) && !it->second.up) {
                nowUp = it->second.up = true;
                outgoing = it->second.outgoing;
                ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - it->second.started).count();
            }
        }
        if (nowUp && debugLogOn()) {
            debugLog("net", std::format("peer {} up ({}, {} ms from first signaling)", debugKey(pk),
                                        outgoing ? "we dialled" : "they dialled", ms));
            logPair(pk, *raw);
        }
        if (nowUp && q.onPeerUp)
            q.onPeerUp(pk);
    }

    // pk by value: bury() resets the callback whose capture a reference would point into.
    void lost(PublicKey pk, rtc::PeerConnection* raw, std::string_view why)
    {
        bool wasUp = false;
        {
            std::lock_guard lock(m);
            auto it = peers.find(pk);
            if (it == peers.end() || it->second.pc.get() != raw)
                return;
            wasUp = it->second.up;
            if (debugLogOn())
                debugLog("net", std::format("peer {} down ({}): {}", debugKey(pk), wasUp ? "was up" : "never up", why));
            bury(it->second);
            peers.erase(it);
        }
        if (wasUp && q.onPeerDown)
            q.onPeerDown(pk);
    }

    void watch(const std::shared_ptr<Handshake>& h)
    {
        std::weak_ptr<Handshake> weak = h;
        h->ws->onMessage(
            [this, weak](rtc::binary data) {
                if (auto hs = weak.lock())
                    onSignal(hs, {reinterpret_cast<const std::uint8_t*>(data.data()), data.size()});
            },
            [](rtc::string) {});
    }

    void abort(Handshake& h)
    {
        std::lock_guard lock(m);
        bury(h);
    }

    void onSignal(const std::shared_ptr<Handshake>& h, std::span<const std::uint8_t> data)
    {
        if (h->done)
            return abort(*h);
        if (h->gated && !h->proven) {
            if (h->loginUsername)
                return onLoginProve(*h, data);
            if ((data.size() == loginUsernameSize || data.size() == loginUsernameSize + 1) && q.onLoginBegin)
                return onLoginBegin(*h, data);
            return onProof(*h, data);
        }
        // Ungated: the same login frames in place of the offer. Every Signal
        // carries a 32-byte key and a 32-byte nonce, so none is this short.
        if (!h->gated && !h->outgoing && !h->peerPK && q.onLoginBegin
            && (h->loginUsername || data.size() == loginUsernameSize || data.size() == loginUsernameSize + 1))
            return h->loginUsername ? onLoginProve(*h, data) : onLoginBegin(*h, data);
        Signal s;
        if (!s.ParseFromArray(data.data(), int(data.size())))
            return abort(*h);
        const auto pk = toKey(s.public_key());
        if (!pk || *pk == self.publicKey() || s.nonce().size() != 32 || (h->proven && *pk != *h->proven))
            return abort(*h);
        if (h->outgoing && !h->peerPK)
            return onHello(h, *pk, s);
        if (h->outgoing)
            return onAnswer(*h, *pk, s);
        onOffer(h, *pk, s);
    }

    // Internet mode, first message: exactly a proof, from a key allowPeer accepts.
    void onProof(Handshake& h, std::span<const std::uint8_t> data)
    {
        if (data.size() != proofSize && data.size() != certProofSize)
            return abort(h);
        PublicKey pk, account;
        std::copy_n(data.begin(), pk.size(), pk.begin());
        account = pk;
        if (data.size() == certProofSize) {
            std::copy_n(data.begin() + proofSize, account.size(), account.begin());
            const DeviceCert cert{account, pk, 0, {}, Bytes(data.begin() + proofSize + 32, data.end())};
            if (account == pk || !validCert(cert))
                return abort(h);
        }
        if (pk == self.publicKey() || !verify(pk, proofInput(cfg.signatureDomain, h.myNonce, self.publicKey()), data.subspan(pk.size(), 64))
            || !q.allowPeer(pk, account))
            return abort(h);
        std::lock_guard lock(m);
        h.proven = pk;
        h.provenAccount = account;
    }

    // Password login, first message: exactly a username, zero-padded to
    // loginUsernameSize (net/node.h). Always answers when onLoginBegin does
    // (a known or a stable fake salt/pubkey alike), so a node never signals
    // which usernames exist by whether it replies.
    void onLoginBegin(Handshake& h, std::span<const std::uint8_t> data)
    {
        // 33 bytes: the recovery code's login (a trailing 1), A11.
        const bool recovery = data.size() == loginUsernameSize + 1;
        if (recovery && data.back() != 1)
            return abort(h);
        if (recovery)
            data = data.first(loginUsernameSize);
        std::size_t len = 0;
        while (len < data.size() && data[len])
            ++len;
        for (std::size_t i = len; i < data.size(); ++i)
            if (data[i])
                return abort(h); // stray bytes after the zero terminator
        const std::string username(reinterpret_cast<const char*>(data.data()), len);
        if (!validUsernameShape(username))
            return abort(h);
        auto challenge = q.onLoginBegin(username, h.address, recovery);
        if (!challenge || challenge->salt.size() != crypto_pwhash_SALTBYTES)
            return abort(h); // disabled or rate-limited; never distinguishable from a bad frame
        h.loginUsername = username;
        h.loginPub = challenge->loginPub;
        h.loginNonce = randomNonce();
        Bytes resp = challenge->salt;
        resp.insert(resp.end(), h.loginNonce.begin(), h.loginNonce.end());
        try {
            h.ws->send(reinterpret_cast<const std::byte*>(resp.data()), resp.size());
        } catch (const std::exception&) {
            abort(h);
        }
    }

    // Password login, second message: exactly a signature over
    // loginProveInput(nonce, username) by the login key onLoginBegin
    // vouched for. Success or failure, the connection ends here - a login
    // never becomes a live peer connection.
    void onLoginProve(Handshake& h, std::span<const std::uint8_t> data)
    {
        if (data.size() != loginProveSize
            || !verify(h.loginPub, loginProveInput(asBytes(h.loginNonce), *h.loginUsername, self.publicKey()), data))
            return abort(h);
        auto vault = q.onLoginProved(*h.loginUsername);
        if (!vault)
            return abort(h);
        try {
            h.ws->send(reinterpret_cast<const std::byte*>(vault->data()), vault->size());
        } catch (const std::exception&) {
        }
        abort(h);
    }

    void onHello(const std::shared_ptr<Handshake>& h, const PublicKey& pk, const Signal& s)
    {
        // expected may name the account (an invite's owner) rather than one of its devices.
        const auto account = accountFrom(s, pk);
        if (!account || !s.sdp().empty() || (h->expected && *h->expected != pk && *h->expected != *account))
            return abort(*h);
        if (debugLogOn())
            debugLog("net", std::format("hello from {}", debugKey(pk)));
        if (!s.sig().empty()) { // a signed hello: an internet-mode node wants our proof first
            const Bytes sig = self.sign(proofInput(cfg.signatureDomain, s.nonce(), pk));
            Bytes proof(self.publicKey().begin(), self.publicKey().end());
            proof.insert(proof.end(), sig.begin(), sig.end());
            if (self.account() != self.publicKey()) {
                proof.insert(proof.end(), self.account().begin(), self.account().end());
                proof.insert(proof.end(), self.cert().sig.begin(), self.cert().sig.end());
            }
            try {
                h->ws->send(reinterpret_cast<const std::byte*>(proof.data()), proof.size());
            } catch (const std::exception&) {
                return abort(*h);
            }
        }
        {
            std::lock_guard lock(m);
            if (h->endpoint)
                h->endpoint->pk = pk;
            if (peers.contains(pk))
                return bury(*h);
            h->peerPK = pk;
            h->peerNonce = s.nonce();
            h->pc = makePeer(pk, true, *account);
            peers[pk].address = addressKey(hostOf(h->ws));
            makeChannels(pk);
        }
        std::weak_ptr<Handshake> weak = h;
        h->pc->onGatheringStateChange([this, weak, pk](rtc::PeerConnection::GatheringState st) {
            if (debugLogOn())
                debugLog("net", std::format("peer {} gathering {}", debugKey(pk), str(st)));
            auto hs = weak.lock();
            if (!hs || st != rtc::PeerConnection::GatheringState::Complete)
                return;
            Signal offer;
            offer.set_public_key(self.publicKey().data(), 32);
            offer.set_nonce(hs->myNonce);
            offer.set_sdp(std::string(*hs->pc->localDescription()));
            const Bytes sig = self.sign(signInput(cfg.signatureDomain, "offer", hs->peerNonce, offer.sdp()));
            offer.set_sig(sig.data(), sig.size());
            offer.set_port(server->port());
            attachCert(offer);
            sendSignal(*hs->ws, offer);
        });
        h->pc->setLocalDescription(rtc::Description::Type::Offer);
    }

    void onAnswer(Handshake& h, const PublicKey& pk, const Signal& s)
    {
        if (pk != *h.peerPK || !verify(pk, signInput(cfg.signatureDomain, "answer", h.myNonce, s.sdp()), asBytes(s.sig())))
            return abort(h);
        if (debugLogOn())
            debugLog("net", std::format("answer from {}", debugKey(pk)));
        try {
            h.pc->setRemoteDescription(rtc::Description(s.sdp(), rtc::Description::Type::Answer));
        } catch (const std::exception&) {
            lost(pk, h.pc.get(), "unusable answer");
        }
        abort(h);
    }

    void onOffer(const std::shared_ptr<Handshake>& h, const PublicKey& pk, const Signal& s)
    {
        const auto account = accountFrom(s, pk);
        if (!account || (h->proven && *account != h->provenAccount)
            || !verify(pk, signInput(cfg.signatureDomain, "offer", h->myNonce, s.sdp()), asBytes(s.sig())))
            return abort(*h);
        if (debugLogOn())
            debugLog("net", std::format("offer from {}", debugKey(pk)));
        std::shared_ptr<rtc::PeerConnection> pc;
        {
            std::lock_guard lock(m);
            if (auto it = peers.find(pk); it != peers.end()) {
                // Simultaneous dial: the connection started by the lower key wins.
                if (it->second.up || (it->second.outgoing && self.publicKey() < pk))
                    return bury(*h);
                bury(it->second);
                peers.erase(it);
            }
            pc = makePeer(pk, false, *account);
            peers[pk].address = h->address.empty() ? addressKey(hostOf(h->ws)) : h->address;
            makeChannels(pk);
            if (auto addr = h->ws->remoteAddress(); addr && s.port())
                addEndpoint(addr->substr(0, addr->rfind(':')), std::uint16_t(s.port()), pk, false);
        }
        const std::string peerNonce = s.nonce();
        std::weak_ptr<rtc::WebSocket> weakWS = h->ws;
        rtc::PeerConnection* raw = pc.get();
        pc->onGatheringStateChange([this, weakWS, peerNonce, raw, pk](rtc::PeerConnection::GatheringState st) {
            if (debugLogOn())
                debugLog("net", std::format("peer {} gathering {}", debugKey(pk), str(st)));
            auto ws = weakWS.lock();
            if (!ws || st != rtc::PeerConnection::GatheringState::Complete)
                return;
            Signal answer;
            answer.set_public_key(self.publicKey().data(), 32);
            answer.set_nonce(randomNonce());
            answer.set_sdp(std::string(*raw->localDescription()));
            const Bytes sig = self.sign(signInput(cfg.signatureDomain, "answer", peerNonce, answer.sdp()));
            answer.set_sig(sig.data(), sig.size());
            attachCert(answer);
            sendSignal(*ws, answer);
        });
        try {
            pc->setRemoteDescription(rtc::Description(s.sdp(), rtc::Description::Type::Offer));
            pc->setLocalDescription(rtc::Description::Type::Answer);
        } catch (const std::exception&) {
            lost(pk, raw, "unusable offer");
            abort(*h);
        }
    }

    // Must hold m. Endpoints are keyed by address: several may claim one key
    // (mDNS is unauthenticated), and the one that proves the key wins at dial time.
    Endpoint* addEndpoint(const std::string& host, std::uint16_t port, std::optional<PublicKey> pk, bool manual,
                          bool TLS = false)
    {
        for (auto& e : endpoints)
            if (e.host == host && e.port == port && e.TLS == TLS) {
                if (pk && !e.manual)
                    e.pk = pk;
                e.manual = e.manual || manual;
                return &e;
            }
        if (!manual && endpoints.size() >= 256)
            return nullptr;
        return &endpoints.emplace_back(Endpoint{host, port, pk, manual, TLS});
    }

    // Must hold m.
    void dialNow(Endpoint& e)
    {
        e.lastDial = Clock::now();
        auto h = std::make_shared<Handshake>();
        h->outgoing = true;
        h->endpoint = &e;
        h->expected = e.pk;
        rtc::WebSocketConfiguration wsConf;
        wsConf.disableTlsVerification = !cfg.verifyTLS;
        h->ws = std::make_shared<rtc::WebSocket>(wsConf);
        handshakes.push_back(h);
        watch(h);
        if (debugLogOn()) {
            const std::string where = std::format("{}:{}{}", e.host, e.port, e.TLS ? " (wss)" : "");
            debugLog("net", std::format("dial {} attempt {}{}", where, e.failures, e.pk ? " expecting " + debugKey(*e.pk) : ""));
            h->ws->onError([where](std::string err) { debugLog("net", "signaling " + where + " error: " + err); });
            h->ws->onClosed([where] { debugLog("net", "signaling " + where + " closed"); });
        }
        try {
            h->ws->open(std::string(e.TLS ? "wss://" : "ws://") + e.host + ":" + std::to_string(e.port) + "/");
        } catch (const std::exception&) {
            bury(*h);
        }
    }

    void tick()
    {
        std::vector<std::shared_ptr<void>> dead;
        std::lock_guard lock(m);
        const auto now = Clock::now();
        for (auto it = peers.begin(); it != peers.end();)
            if (!it->second.up && now - it->second.started > std::chrono::seconds(20)) {
                if (debugLogOn())
                    debugLog("net", std::format("peer {} gave up: not up 20 s after signaling began", debugKey(it->first)));
                bury(it->second);
                it = peers.erase(it);
            } else {
                ++it;
            }
        std::erase_if(handshakes, [&](auto& h) {
            if (!h->done
                && (now - h->started > std::chrono::seconds(30) || (h->gated && !h->proven && now - h->started > proofTimeout)))
                bury(*h);
            return h->done;
        });
        std::erase_if(buckets, [&](auto& b) { return now - b.second.last > std::chrono::seconds(60); });
        for (auto& e : endpoints) {
            if (e.pk && peers.contains(*e.pk)) {
                e.failures = 0;
                continue;
            }
            if (!e.manual && e.pk && !(self.publicKey() < *e.pk))
                continue;
            // Addresses someone typed in stay eager; discovered ones may not run Notolith at all.
            const auto cap = e.manual ? std::chrono::seconds(10) : std::chrono::seconds(300);
            if (now - e.lastDial < std::min(std::chrono::seconds(5) * (1 << std::min(e.failures, 6)), cap))
                continue;
            ++e.failures;
            dialNow(e);
        }
        dead.swap(graveyard);
        // dead is destroyed after the lock is released (declared first).
    }

    // Must hold m. Internet mode: per-address rate and concurrency, and a global
    // cap on unproven connections. Proven members are not counted.
    bool admit(const Handshake& h)
    {
        const auto now = Clock::now();
        auto it = buckets.find(h.address);
        if (it == buckets.end()) {
            if (buckets.size() >= cfg.gatedAddressTableSize) {
                // Full table: evict the address that has been quiet longest
                // rather than refuse a new one outright - a spread-out scan
                // (one /48, or a few dozen addresses) would otherwise wedge
                // every slot open and lock out any address it hasn't tried yet.
                // Limit: O(table size) scan, only on the full-table path.
                auto oldest = std::min_element(buckets.begin(), buckets.end(),
                                               [](auto& a, auto& b) { return a.second.last < b.second.last; });
                if (oldest != buckets.end())
                    buckets.erase(oldest);
            }
            it = buckets.emplace(h.address, Bucket{}).first;
        }
        Bucket& b = it->second;
        b.tokens = std::min(gatedBurst, b.tokens + std::chrono::duration<double>(now - b.last).count() * gatedRefill);
        b.last = now;
        if (b.tokens < 1)
            return false;
        b.tokens -= 1;
        int fromAddress = 0, unproven = 0;
        for (auto& o : handshakes)
            if (!o->done && o->gated && !o->proven && !o->ws->isClosed()) {
                ++unproven;
                fromAddress += o->address == h.address;
            }
        return fromAddress < gatedPerAddress && unproven < gatedUnproven;
    }

    void startAvahi()
    {
        poll = avahi_threaded_poll_new();
        int err = 0;
        avahi = avahi_client_new(avahi_threaded_poll_get(poll), AvahiClientFlags(0), &Impl::clientEvent, this, &err);
        if (!avahi) {
            avahi_threaded_poll_free(poll);
            poll = nullptr;
            return;
        }
        avahi_threaded_poll_start(poll);
    }

    void stopAvahi()
    {
        if (!poll)
            return;
        avahi_threaded_poll_stop(poll);
        avahi_client_free(avahi);
        avahi_threaded_poll_free(poll);
        poll = nullptr;
    }

    static void clientEvent(AvahiClient* c, AvahiClientState state, void* userdata)
    {
        auto* d = static_cast<Impl*>(userdata);
        if (state != AVAHI_CLIENT_S_RUNNING || d->group)
            return;
        // Limit: IPv4 only; IPv6 link-local needs scope ids in the WebSocket URL
        d->group = avahi_entry_group_new(c, nullptr, nullptr);
        const std::string name = d->cfg.serviceNamePrefix + fingerprint(d->self.publicKey());
        const std::string txt = "pk=" + hex(d->self.publicKey());
        if (d->group
            && avahi_entry_group_add_service(d->group, AVAHI_IF_UNSPEC, AVAHI_PROTO_INET, AvahiPublishFlags(0),
                                             name.c_str(), d->cfg.serviceType.c_str(), nullptr, nullptr, d->server->port(),
                                             txt.c_str(), nullptr) == 0)
            avahi_entry_group_commit(d->group);
        d->browser = avahi_service_browser_new(c, AVAHI_IF_UNSPEC, AVAHI_PROTO_INET, d->cfg.serviceType.c_str(), nullptr,
                                               AvahiLookupFlags(0), &Impl::browseEvent, d);
    }

    static void browseEvent(AvahiServiceBrowser* b, AvahiIfIndex iface, AvahiProtocol proto, AvahiBrowserEvent event,
                            const char* name, const char* type, const char* domain, AvahiLookupResultFlags,
                            void* userdata)
    {
        if (event == AVAHI_BROWSER_NEW)
            avahi_service_resolver_new(avahi_service_browser_get_client(b), iface, proto, name, type, domain,
                                       AVAHI_PROTO_INET, AvahiLookupFlags(0), &Impl::resolveEvent, userdata);
    }

    static void resolveEvent(AvahiServiceResolver* r, AvahiIfIndex, AvahiProtocol, AvahiResolverEvent event,
                             const char*, const char*, const char*, const char*, const AvahiAddress* address,
                             std::uint16_t port, AvahiStringList* txt, AvahiLookupResultFlags, void* userdata)
    {
        auto* d = static_cast<Impl*>(userdata);
        if (event == AVAHI_RESOLVER_FOUND) {
            std::optional<PublicKey> pk;
            if (AvahiStringList* item = avahi_string_list_find(txt, "pk")) {
                char *key = nullptr, *value = nullptr;
                if (avahi_string_list_get_pair(item, &key, &value, nullptr) == 0 && value)
                    pk = fromHex(value);
                avahi_free(key);
                avahi_free(value);
            }
            char host[AVAHI_ADDRESS_STR_MAX];
            avahi_address_snprint(host, sizeof host, address);
            if (pk && *pk != d->self.publicKey()) {
                std::lock_guard lock(d->m);
                d->addEndpoint(host, port, pk, false);
            }
        }
        avahi_service_resolver_free(r);
    }
};

Node::Node(const Identity& self, Config config) : d_(std::make_unique<Impl>(*this, self, config))
{
    if (debugLogOn()) { // libdatachannel and libjuice: ICE candidates, pairs, state. The logger is process-wide.
        static std::once_flag once;
        std::call_once(once, [] {
            rtc::InitLogger(rtc::LogLevel::Info, [](rtc::LogLevel level, std::string msg) {
                static constexpr const char* names[] = {"none", "fatal", "error", "warn", "info", "debug", "verbose"};
                debugLog("rtc", std::format("{} {}", names[std::size_t(level)], msg));
            });
        });
    }
}

Node::~Node()
{
    d_->maintenance = {};
    d_->stopAvahi();
    if (d_->server)
        d_->server->stop();
    if (d_->TLSserver)
        d_->TLSserver->stop();
    std::lock_guard lock(d_->m);
    for (auto& [pk, p] : d_->peers)
        d_->bury(p);
    for (auto& h : d_->handshakes)
        d_->bury(*h);
    d_->peers.clear();
    d_->handshakes.clear();
}

void Node::start()
{
    const bool gated = bool(allowPeer);
    rtc::WebSocketServer::Configuration conf;
    conf.port = d_->cfg.port;
    if (!d_->cfg.listen.empty())
        conf.bindAddress = d_->cfg.listen;
    // Gated or not, only signaling crosses this socket: a few KB of SDP, and a
    // connection that never upgrades to a WebSocket is dropped.
    conf.connectionTimeout = std::chrono::duration_cast<std::chrono::milliseconds>(proofTimeout);
    conf.maxMessageSize = signalingMessageSize;
    try {
        d_->server = std::make_unique<rtc::WebSocketServer>(conf);
    } catch (const std::exception&) {
        throw std::runtime_error("port " + std::to_string(conf.port) + " is already in use (is another Notolith already using it?)");
    }
    auto accept = [d = d_.get(), gated](std::shared_ptr<rtc::WebSocket> ws) {
        auto h = std::make_shared<Handshake>();
        h->outgoing = false;
        h->ws = ws;
        h->gated = gated;
        {
            std::lock_guard lock(d->m);
            // Signaling needs no identity yet, so bound what a stranger can hold open.
            const std::string host = hostOf(ws);
            h->address = addressKey(host);
            const auto fromHost = std::count_if(d->handshakes.begin(), d->handshakes.end(), [&](auto& o) {
                return !o->done && !o->outgoing && o->ws->remoteAddress().value_or("").starts_with(host + ":");
            });
            // N9/N12/D1: without a gate, this node has no proof check and no
            // per-key allow list at all, so never accept a connection from a
            // public address - only LAN, loopback and tailnet/CGNAT reach it,
            // matching the LAN/tailnet-friend use this project is meant for.
            if (gated ? !d->admit(*h) : d->handshakes.size() >= 128 || fromHost >= 8 || !isPrivateHost(host)) {
                if (debugLogOn())
                    debugLog("net", "signaling connection from " + host + " refused");
                ws->forceClose();
                d->graveyard.push_back(ws);
                return;
            }
            if (debugLogOn())
                debugLog("net", "signaling connection from " + host);
            d->handshakes.push_back(h);
        }
        d->watch(h);
        std::weak_ptr<Handshake> weak = h;
        ws->onOpen([d, weak] {
            auto hs = weak.lock();
            if (!hs)
                return;
            Signal hello;
            hello.set_public_key(d->self.publicKey().data(), 32);
            hello.set_nonce(hs->myNonce);
            d->attachCert(hello);
            if (hs->gated) { // the signature marks the hello: send a proof first
                const Bytes sig = d->self.sign(signInput(d->cfg.signatureDomain, "hello", hs->myNonce, ""));
                hello.set_sig(sig.data(), sig.size());
            }
            d->sendSignal(*hs->ws, hello);
        });
    };
    d_->server->onClient(accept);
    if (!d_->cfg.TLScertFile.empty()) {
        try {
            rtc::WebSocketServer::Configuration tls;
            tls.port = d_->cfg.TLSport;
            tls.enableTls = true;
            tls.certificatePemFile = d_->cfg.TLScertFile;
            tls.keyPemFile = d_->cfg.TLSkeyFile;
            tls.bindAddress = conf.bindAddress;
            tls.connectionTimeout = conf.connectionTimeout;
            tls.maxMessageSize = conf.maxMessageSize;
            d_->TLSserver = std::make_unique<rtc::WebSocketServer>(tls);
            d_->TLSserver->onClient(accept);
        } catch (const std::exception&) {
            d_->TLSserver.reset(); // bad or missing certificate: plain signaling only
        }
    }
    if (d_->cfg.mDNS)
        d_->startAvahi();
    d_->maintenance = std::jthread([d = d_.get()](std::stop_token stop) {
        while (!stop.stop_requested()) {
            if (d->cfg.tailscale && Clock::now() - d->lastTailnetScan > std::chrono::seconds(30)) {
                d->lastTailnetScan = Clock::now();
                if (auto st = tailnetStatus()) { // outside the lock: runs a process
                    std::lock_guard lock(d->m);
                    for (auto& p : st->peers) {
                        if (!p.online)
                            continue;
                        d->addEndpoint(p.IP, d->cfg.port ? d->cfg.port : 47800, std::nullopt, false);
                        if (!p.DNSname.empty())
                            d->addEndpoint(p.DNSname, d->cfg.TLSport, std::nullopt, false, true);
                    }
                }
            }
            d->tick();
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
    });
}

std::uint16_t Node::port() const { return d_->server ? d_->server->port() : 0; }

const PublicKey& Node::self() const { return d_->self.publicKey(); }

std::optional<std::pair<std::string, std::uint16_t>> Node::TLSendpoint() const
{
    if (!d_->TLSserver || d_->cfg.TLShost.empty())
        return std::nullopt;
    return std::pair{d_->cfg.TLShost, d_->TLSserver->port()};
}

void Node::dial(const std::string& host, std::uint16_t port, std::optional<PublicKey> expected, bool TLS)
{
    std::lock_guard lock(d_->m);
    if (Endpoint* e = d_->addEndpoint(host, port, expected, true, TLS)) {
        e->lastDial = {};
        if (expected)
            e->pk = expected;
    }
}

void Node::reconnect()
{
    std::vector<std::pair<PublicKey, rtc::PeerConnection*>> all;
    {
        std::lock_guard lock(d_->m);
        for (auto& [pk, p] : d_->peers)
            all.emplace_back(pk, p.pc.get());
    }
    for (auto& [pk, raw] : all)
        d_->lost(pk, raw, "reconnect requested");
}

bool Node::send(const PublicKey& peer, Channel channel, std::span<const std::uint8_t> data)
{
    std::shared_ptr<rtc::DataChannel> ch;
    {
        std::lock_guard lock(d_->m);
        auto it = d_->peers.find(peer);
        if (it == d_->peers.end() || !it->second.up)
            return false;
        ch = it->second.ch[std::size_t(channel)];
    }
    try {
        ch->send(reinterpret_cast<const std::byte*>(data.data()), data.size());
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

std::size_t Node::bufferedAmount(const PublicKey& peer, Channel channel) const
{
    std::lock_guard lock(d_->m);
    auto it = d_->peers.find(peer);
    return it == d_->peers.end() || !it->second.up ? 0 : it->second.ch[std::size_t(channel)]->bufferedAmount();
}

std::vector<PublicKey> Node::peers() const
{
    std::lock_guard lock(d_->m);
    std::vector<PublicKey> out;
    for (auto& [pk, p] : d_->peers)
        if (p.up)
            out.push_back(pk);
    return out;
}

PublicKey Node::accountOf(const PublicKey& peer) const
{
    std::lock_guard lock(d_->m);
    auto it = d_->peers.find(peer);
    return it == d_->peers.end() ? peer : it->second.account;
}

std::string Node::addressOf(const PublicKey& peer) const
{
    std::lock_guard lock(d_->m);
    auto it = d_->peers.find(peer);
    return it == d_->peers.end() ? std::string() : it->second.address;
}

std::shared_ptr<rtc::PeerConnection> Node::connection(const PublicKey& peer) const
{
    std::lock_guard lock(d_->m);
    auto it = d_->peers.find(peer);
    return it == d_->peers.end() || !it->second.up ? nullptr : it->second.pc;
}

} // namespace hearth
