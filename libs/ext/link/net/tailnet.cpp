// SPDX-License-Identifier: GPL-3.0-or-later

#include "net/tailnet.h"

#include <nlohmann/json.hpp>

#include <cstdio>

namespace hearth {

namespace {

std::string run(const std::string& cmd, int* status = nullptr)
{
    std::string out;
    FILE* p = popen(cmd.c_str(), "r");
    if (!p)
        return out;
    char buf[4096];
    for (std::size_t n; (n = std::fread(buf, 1, sizeof buf, p)) > 0;)
        out.append(buf, n);
    const int rc = pclose(p);
    if (status)
        *status = rc;
    return out;
}

std::string quoted(const std::string& s)
{
    std::string q = "'";
    for (char c : s)
        q += c == '\'' ? std::string("'\\''") : std::string(1, c);
    return q + "'";
}

std::string trimDot(std::string s)
{
    if (!s.empty() && s.back() == '.')
        s.pop_back();
    return s;
}

} // namespace

std::optional<TailnetStatus> parseTailnetStatus(std::string_view json)
{
    const auto j = nlohmann::json::parse(json, nullptr, false);
    if (!j.is_object() || !j.contains("Self") || !j["Self"].is_object())
        return std::nullopt;
    TailnetStatus s;
    s.selfDNSname = trimDot(j["Self"].value("DNSName", ""));
    if (auto it = j.find("CertDomains"); it != j.end() && it->is_array())
        for (auto& d : *it)
            if (d.is_string())
                s.certDomains.push_back(d.get<std::string>());
    if (auto it = j.find("Peer"); it != j.end() && it->is_object())
        for (auto& [id, p] : it->items()) {
            if (!p.is_object())
                continue;
            TailnetPeer peer;
            peer.DNSname = trimDot(p.value("DNSName", ""));
            peer.online = p.value("Online", false);
            if (auto ips = p.find("TailscaleIPs"); ips != p.end() && ips->is_array())
                for (auto& ip : *ips)
                    if (ip.is_string() && ip.get<std::string>().find(':') == std::string::npos) {
                        peer.IP = ip.get<std::string>();
                        break;
                    }
            if (!peer.IP.empty())
                s.peers.push_back(peer);
        }
    return s;
}

std::optional<TailnetStatus> tailnetStatus()
{
    int rc = -1;
    const std::string out = run("timeout 5 tailscale status --json 2>/dev/null", &rc);
    if (rc != 0 || out.empty())
        return std::nullopt;
    return parseTailnetStatus(out);
}

std::optional<std::string> fetchTailnetCert(const std::filesystem::path& certFile, const std::filesystem::path& keyFile)
{
    auto status = tailnetStatus();
    if (!status || status->certDomains.empty())
        return std::nullopt;
    const std::string domain = status->certDomains.front();
    int rc = -1;
    run("timeout 60 tailscale cert --cert-file " + quoted(certFile.string()) + " --key-file " + quoted(keyFile.string())
            + " " + quoted(domain) + " >/dev/null 2>&1",
        &rc);
    std::error_code ec;
    if (rc != 0 || !std::filesystem::exists(certFile, ec) || !std::filesystem::exists(keyFile, ec))
        return std::nullopt;
    std::filesystem::permissions(keyFile, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
                                 std::filesystem::perm_options::replace, ec);
    return domain;
}

} // namespace hearth
