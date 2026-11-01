// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej

#include "net/node.h"

#include "core/debuglog.h"
#include "link.pb.h"

#include <gtest/gtest.h>
#include <rtc/rtc.hpp>

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string_view>
#include <mutex>
#include <thread>

#include <unistd.h>

using namespace hearth;
using namespace std::chrono_literals;

namespace {

bool waitFor(const std::function<bool()>& pred, std::chrono::milliseconds limit = 10s)
{
    const auto end = std::chrono::steady_clock::now() + limit;
    while (!pred()) {
        if (std::chrono::steady_clock::now() > end)
            return false;
        std::this_thread::sleep_for(20ms);
    }
    return true;
}

struct Inbox {
    std::mutex m;
    std::vector<std::pair<Channel, std::string>> got;
    void attach(Node& n)
    {
        n.onMessage = [this](const PublicKey&, Channel c, std::span<const std::uint8_t> d) {
            std::lock_guard lock(m);
            got.emplace_back(c, std::string(d.begin(), d.end()));
        };
    }
    std::size_t size()
    {
        std::lock_guard lock(m);
        return got.size();
    }
};

std::span<const std::uint8_t> bytes(const std::string& s)
{
    return {reinterpret_cast<const std::uint8_t*>(s.data()), s.size()};
}

Node::Config local() { return {.port = 0, .mDNS = false}; }

} // namespace

TEST(Node, ConnectsAndExchangesOnAllChannels)
{
    const auto a = Identity::generate(), b = Identity::generate();
    Node na(a, local()), nb(b, local());
    std::atomic<int> upA = 0, upB = 0;
    na.onPeerUp = [&](const PublicKey& pk) { upA += pk == b.publicKey(); };
    nb.onPeerUp = [&](const PublicKey& pk) { upB += pk == a.publicKey(); };
    Inbox inA, inB;
    inA.attach(na);
    inB.attach(nb);
    na.start();
    nb.start();
    na.dial("127.0.0.1", nb.port());
    ASSERT_TRUE(waitFor([&] { return upA == 1 && upB == 1; }));

    EXPECT_TRUE(na.send(b.publicKey(), Channel::ctl, bytes("c")));
    EXPECT_TRUE(na.send(b.publicKey(), Channel::sync, bytes("s")));
    const std::string big(512 * 1024, 'x');
    EXPECT_TRUE(na.send(b.publicKey(), Channel::blob, bytes(big)));
    EXPECT_TRUE(nb.send(a.publicKey(), Channel::ctl, bytes("back")));
    ASSERT_TRUE(waitFor([&] { return inB.size() == 3 && inA.size() == 1; }));
    EXPECT_EQ(inA.got[0].second, "back");
    EXPECT_EQ(na.peers(), std::vector<PublicKey>{b.publicKey()});
    EXPECT_NE(na.connection(b.publicKey()), nullptr);
}

// The debug log says who and when, never what: message bytes and whole keys stay out of it.
TEST(Node, DebugLogNamesPeersByPrefixAndLeaksNoMessages)
{
    const auto dir = std::filesystem::temp_directory_path() / ("hearth-node-log-" + std::to_string(::getpid()));
    std::filesystem::remove_all(dir);
    setDebugLogPath((dir / "hearth.log").string());
    ASSERT_TRUE(startDebugLog());
    const auto a = Identity::generate(), b = Identity::generate();
    {
        Node na(a, local()), nb(b, local());
        std::atomic<int> up = 0;
        na.onPeerUp = [&](const PublicKey&) { ++up; };
        na.start();
        nb.start();
        na.dial("127.0.0.1", nb.port());
        ASSERT_TRUE(waitFor([&] { return up == 1; }));
        EXPECT_TRUE(na.send(b.publicKey(), Channel::ctl, bytes("hunter2 is my password")));
    }
    stopDebugLog();
    std::stringstream buf;
    buf << std::ifstream(dir / "hearth.log").rdbuf();
    const std::string log = buf.str();
    for (const PublicKey* pk : {&a.publicKey(), &b.publicKey()}) {
        std::string full;
        char two[3];
        for (auto byte : *pk)
            full += (std::snprintf(two, sizeof two, "%02x", byte), two);
        EXPECT_NE(log.find("peer " + full.substr(0, 8) + " up"), std::string::npos) << log;
        EXPECT_EQ(log.find(full.substr(0, 16)), std::string::npos); // never more than the 8-digit prefix
    }
    EXPECT_NE(log.find("[net] dial 127.0.0.1:"), std::string::npos);
    EXPECT_EQ(log.find("hunter2"), std::string::npos);
    std::filesystem::remove_all(dir);
    setDebugLogPath({});
}

TEST(Node, SignatureDomainIsolatesApps)
{
    const auto a = Identity::generate(), b = Identity::generate(), c = Identity::generate();
    auto cfg = [](const char* domain) {
        Node::Config k = local();
        k.signatureDomain = domain;
        return k;
    };
    Node na(a, cfg("appone")), nb(b, cfg("apptwo")), nc(c, cfg("appone"));
    std::atomic<int> aSawB = 0, aSawC = 0;
    na.onPeerUp = [&](const PublicKey& pk) {
        aSawB += pk == b.publicKey();
        aSawC += pk == c.publicKey();
    };
    na.start();
    nb.start();
    nc.start();
    na.dial("127.0.0.1", nb.port());
    na.dial("127.0.0.1", nc.port());
    ASSERT_TRUE(waitFor([&] { return aSawC == 1; }));
    std::this_thread::sleep_for(3s); // b had every chance to complete a dial by now
    EXPECT_EQ(aSawB, 0);
}

TEST(Node, PeerDownAndReconnect)
{
    const auto a = Identity::generate(), b = Identity::generate();
    Node na(a, local());
    std::atomic<int> up = 0, down = 0;
    na.onPeerUp = [&](const PublicKey&) { ++up; };
    na.onPeerDown = [&](const PublicKey& pk) { down += pk == b.publicKey(); };
    na.start();
    std::uint16_t port;
    {
        Node nb(b, local());
        nb.start();
        port = nb.port();
        na.dial("127.0.0.1", port);
        ASSERT_TRUE(waitFor([&] { return up == 1; }));
    }
    ASSERT_TRUE(waitFor([&] { return down == 1; }, 40s));
    Node nb2(b, {.port = port, .mDNS = false});
    nb2.start();
    EXPECT_TRUE(waitFor([&] { return up == 2; }, 20s));
}

TEST(Node, SimultaneousDialYieldsOneConnection)
{
    const auto a = Identity::generate(), b = Identity::generate();
    Node na(a, local()), nb(b, local());
    std::atomic<int> upA = 0, upB = 0;
    na.onPeerUp = [&](const PublicKey&) { ++upA; };
    nb.onPeerUp = [&](const PublicKey&) { ++upB; };
    na.start();
    nb.start();
    na.dial("127.0.0.1", nb.port());
    nb.dial("127.0.0.1", na.port());
    ASSERT_TRUE(waitFor([&] { return upA >= 1 && upB >= 1; }));
    std::this_thread::sleep_for(2s);
    EXPECT_EQ(na.peers().size(), 1u);
    EXPECT_EQ(nb.peers().size(), 1u);
    EXPECT_TRUE(na.send(b.publicKey(), Channel::ctl, bytes("x")));
}

TEST(Node, ExpectedIdentityEnforced)
{
    const auto a = Identity::generate(), b = Identity::generate();
    Node na(a, local()), nb(b, local());
    std::atomic<int> up = 0;
    na.onPeerUp = [&](const PublicKey&) { ++up; };
    na.start();
    nb.start();
    na.dial("127.0.0.1", nb.port(), Identity::generate().publicKey());
    std::this_thread::sleep_for(3s);
    EXPECT_EQ(up, 0);
}

// A raw WebSocket client that forges or replays the offer signature.
class Forger : public testing::TestWithParam<int> {};

TEST_P(Forger, RejectsBadOffer)
{
    const auto b = Identity::generate(), claimed = Identity::generate(), attacker = Identity::generate();
    Node nb(b, local());
    std::atomic<int> up = 0;
    nb.onPeerUp = [&](const PublicKey&) { ++up; };
    nb.start();

    rtc::PeerConnection pc;
    std::string sdp;
    std::atomic<bool> gathered = false;
    // The callback must be in place before createDataChannel: that call starts the negotiation and
    // gathering on libdatachannel's own thread, and a starved test thread would miss Complete.
    pc.onGatheringStateChange([&](rtc::PeerConnection::GatheringState s) {
        if (s == rtc::PeerConnection::GatheringState::Complete) {
            sdp = std::string(*pc.localDescription());
            gathered = true;
        }
    });
    auto dc = pc.createDataChannel("x");
    ASSERT_TRUE(waitFor([&] { return gathered.load(); }, 30s)); // ICE gathering crawls on a loaded host

    auto ws = std::make_shared<rtc::WebSocket>();
    std::atomic<bool> closed = false, sent = false;
    ws->onClosed([&] { closed = true; });
    ws->onMessage(
        [&](rtc::binary data) {
            if (sent.exchange(true))
                return; // only answer the hello
            Signal hello;
            if (!hello.ParseFromArray(data.data(), int(data.size())))
                return;
            const std::string nonce = GetParam() == 1 ? std::string(32, 'r') : hello.nonce();
            const Identity& signer = GetParam() == 0 ? attacker : claimed;
            Bytes input{'h', 'e', 'a', 'r', 't', 'h', '-', 'o', 'f', 'f', 'e', 'r'};
            input.reserve(input.size() + nonce.size() + sdp.size());
            input.insert(input.end(), nonce.begin(), nonce.end());
            input.insert(input.end(), sdp.begin(), sdp.end());
            Signal offer;
            offer.set_public_key(claimed.publicKey().data(), 32);
            offer.set_nonce(std::string(32, 'n'));
            offer.set_sdp(sdp);
            const Bytes sig = signer.sign(input);
            offer.set_sig(sig.data(), sig.size());
            const std::string wire = offer.SerializeAsString();
            ws->send(reinterpret_cast<const std::byte*>(wire.data()), wire.size());
        },
        [](rtc::string) {});
    ws->open("ws://127.0.0.1:" + std::to_string(nb.port()) + "/");
    if (GetParam() == 2) { // control: a correct offer is accepted, socket stays open for the answer
        EXPECT_FALSE(waitFor([&] { return closed.load(); }, 2s));
        return;
    }
    EXPECT_TRUE(waitFor([&] { return closed.load(); }, 30s));
    EXPECT_EQ(up, 0);
    EXPECT_TRUE(nb.peers().empty());
}

// 0: signed by a different key than claimed. 1: replayed against a stale nonce.
// 2: correctly signed control.
INSTANTIATE_TEST_SUITE_P(Node, Forger, testing::Values(0, 1, 2));

// These two publish real mDNS services on the LAN, so they are opt-in: HEARTH_LIVE_MDNS=1 and a
// running avahi-daemon. Automated runs never touch the network beyond loopback.
constexpr const char* kLiveMDNSskip =
    "publishes real mDNS services on the LAN; opt in with HEARTH_LIVE_MDNS=1 (needs avahi-daemon)";
bool liveMDNS()
{
    const char* v = std::getenv("HEARTH_LIVE_MDNS");
    return v && std::string_view(v) == "1" && std::system("systemctl is-active --quiet avahi-daemon") == 0;
}

TEST(Node, DiscoversOverMDNS)
{
    if (!liveMDNS())
        GTEST_SKIP() << kLiveMDNSskip;
    const auto a = Identity::generate(), b = Identity::generate();
    Node na(a, {.port = 0, .mDNS = true}), nb(b, {.port = 0, .mDNS = true});
    std::atomic<int> up = 0;
    na.onPeerUp = [&](const PublicKey& pk) { up += pk == b.publicKey(); };
    na.start();
    nb.start();
    EXPECT_TRUE(waitFor([&] { return up == 1; }, 20s));
}

// Two apps on one LAN, each with its own serviceType. The positive control (a finds b, same
// type) proves mDNS works here, so c staying unseen is not just a quiet network.
TEST(Node, ServiceTypeIsolatesApps)
{
    if (!liveMDNS())
        GTEST_SKIP() << kLiveMDNSskip;
    const auto a = Identity::generate(), b = Identity::generate(), c = Identity::generate();
    Node na(a, {.port = 0, .mDNS = true, .serviceType = "_hearthtest-one._tcp", .serviceNamePrefix = "one-"});
    Node nb(b, {.port = 0, .mDNS = true, .serviceType = "_hearthtest-one._tcp", .serviceNamePrefix = "one-"});
    Node nc(c, {.port = 0, .mDNS = true, .serviceType = "_hearthtest-two._tcp", .serviceNamePrefix = "two-"});
    std::atomic<int> aSawB = 0, aSawC = 0, cSawA = 0, cSawB = 0;
    na.onPeerUp = [&](const PublicKey& pk) {
        aSawB += pk == b.publicKey();
        aSawC += pk == c.publicKey();
    };
    nc.onPeerUp = [&](const PublicKey& pk) {
        cSawA += pk == a.publicKey();
        cSawB += pk == b.publicKey();
    };
    na.start();
    nb.start();
    nc.start();
    ASSERT_TRUE(waitFor([&] { return aSawB == 1; }, 20s));
    std::this_thread::sleep_for(4s); // c had every chance to be found by now
    EXPECT_EQ(aSawC, 0);
    EXPECT_EQ(cSawA + cSawB, 0);
}

