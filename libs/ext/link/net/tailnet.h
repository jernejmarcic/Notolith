#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace hearth {

struct TailnetPeer {
    std::string DNSname; // MagicDNS name without the trailing dot
    std::string IP;      // first Tailscale IPv4
    bool online = false;
};

struct TailnetStatus {
    std::string selfDNSname;
    std::vector<std::string> certDomains; // empty unless HTTPS is enabled for the tailnet
    std::vector<TailnetPeer> peers;       // includes nodes shared in from friends' tailnets
};

std::optional<TailnetStatus> parseTailnetStatus(std::string_view json);
// `tailscale status --json`; empty if Tailscale is missing or not running.
std::optional<TailnetStatus> tailnetStatus();
// Gets or renews this machine's HTTPS certificate (Let's Encrypt, via tailscaled).
// Needs HTTPS enabled for the tailnet and `sudo tailscale set --operator=$USER` once.
// Returns the certificate's domain.
std::optional<std::string> fetchTailnetCert(const std::filesystem::path& certFile, const std::filesystem::path& keyFile);

} // namespace hearth
