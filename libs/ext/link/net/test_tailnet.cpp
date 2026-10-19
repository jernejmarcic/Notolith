#include "net/node.h"
#include "net/tailnet.h"

#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <thread>

using namespace hearth;
using namespace std::chrono_literals;

TEST(Tailnet, ParsesStatus)
{
    const auto s = parseTailnetStatus(R"({
        "Self": {"DNSName": "laptop.tail0000.ts.net.", "TailscaleIPs": ["100.64.0.1"]},
        "CertDomains": ["laptop.tail0000.ts.net"],
        "Peer": {
            "a": {"DNSName": "friend.tail0001.ts.net.", "TailscaleIPs": ["fd7a::1", "100.64.0.2"], "Online": true},
            "b": {"DNSName": "phone.tail0000.ts.net.", "TailscaleIPs": [], "Online": true},
            "c": {"DNSName": "off.tail0000.ts.net.", "TailscaleIPs": ["100.1.2.3"], "Online": false}
        }})");
    ASSERT_TRUE(s);
    EXPECT_EQ(s->selfDNSname, "laptop.tail0000.ts.net");
    ASSERT_EQ(s->certDomains.size(), 1u);
    ASSERT_EQ(s->peers.size(), 2u); // the one without an IPv4 is skipped
    for (auto& p : s->peers)
        if (p.DNSname == "friend.tail0001.ts.net") {
            EXPECT_EQ(p.IP, "100.64.0.2");
            EXPECT_TRUE(p.online);
        }
    EXPECT_FALSE(parseTailnetStatus("not json"));
    EXPECT_FALSE(parseTailnetStatus("[]"));
}

TEST(Tailnet, SignalingOverTLS)
{
    const auto dir = std::filesystem::temp_directory_path() / "hearth-tls-test";
    std::filesystem::create_directories(dir);
    struct DirGuard {
        std::filesystem::path p;
        ~DirGuard() { std::filesystem::remove_all(p); }
    } dirGuard{dir};
    const auto cert = (dir / "c.pem").string(), key = (dir / "k.pem").string();
    const std::string cmd = "openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes -days 1 -subj /CN=localhost"
                            " -keyout " + key + " -out " + cert + " >/dev/null 2>&1";
    if (std::system(cmd.c_str()) != 0)
        GTEST_SKIP() << "openssl not available";
    const auto a = Identity::generate(), b = Identity::generate();
    Node::Config tls{.port = 0, .mDNS = false, .TLScertFile = cert, .TLSkeyFile = key, .TLShost = "localhost",
                     .TLSport = 0, .verifyTLS = false};
    Node na(a, {.port = 0, .mDNS = false, .verifyTLS = false}), nb(b, tls);
    std::atomic<int> up = 0;
    na.onPeerUp = [&](const PublicKey& pk) { up += pk == b.publicKey(); };
    na.start();
    nb.start();
    auto ep = nb.TLSendpoint();
    ASSERT_TRUE(ep);
    na.dial("127.0.0.1", ep->second, b.publicKey(), true);
    const auto end = std::chrono::steady_clock::now() + 10s;
    while (up == 0 && std::chrono::steady_clock::now() < end)
        std::this_thread::sleep_for(20ms);
    EXPECT_EQ(up, 1);
}
