// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej Marčič

#include "core/debuglog.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <mutex>

namespace hearth {

namespace {

namespace fs = std::filesystem;

struct Log {
    std::mutex m;
    std::string path;
    std::FILE* file = nullptr;
    std::size_t cap = 20u << 20, size = 0;
};

Log& log()
{
    static Log l;
    return l;
}

std::string defaultPath()
{
    // The spec ignores a relative XDG_STATE_HOME; with no usable HOME either there is nowhere to log.
    const char* state = std::getenv("XDG_STATE_HOME");
    const char* home = std::getenv("HOME");
    if (state && state[0] == '/')
        return (fs::path(state) / "hearth" / "hearth.log").string();
    if (home && home[0] == '/')
        return (fs::path(home) / ".local" / "state" / "hearth" / "hearth.log").string();
    return {};
}

// Must hold l.m.
bool openLocked(Log& l)
{
    if (l.path.empty())
        l.path = defaultPath();
    if (l.path.empty())
        return false;
    const fs::path dir = fs::path(l.path).parent_path();
    std::error_code ec;
    if (!dir.empty() && fs::create_directories(dir, ec))
        fs::permissions(dir, fs::perms::owner_all, fs::perm_options::replace, ec);
    const int fd = ::open(l.path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
    if (fd < 0)
        return false;
    ::fchmod(fd, 0600); // a file from an older run keeps whatever mode it had otherwise
    struct stat st{};
    ::fstat(fd, &st);
    l.size = std::size_t(st.st_size);
    l.file = ::fdopen(fd, "a");
    if (!l.file)
        ::close(fd);
    return l.file != nullptr;
}

std::string stamp()
{
    const auto now = std::chrono::system_clock::now();
    const auto t = std::chrono::system_clock::to_time_t(now);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;
    std::tm tm{};
    localtime_r(&t, &tm);
    char buf[40];
    const std::size_t n = std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%S", &tm);
    std::snprintf(buf + n, sizeof buf - n, ".%03d", int(ms));
    std::strftime(buf + n + 4, sizeof buf - n - 4, "%z", &tm);
    return buf;
}

} // namespace

namespace detail {
std::atomic<bool> debugLogEnabled = [] {
    for (const char* name : {"HEARTH_DEBUG", "LINK_DEBUG"}) // LINK_DEBUG: apps vendoring the link layer
        if (const char* e = std::getenv(name); e && *e && std::string_view(e) != "0")
            return true;
    return false;
}();
}

void setDebugLogPath(std::string path)
{
    Log& l = log();
    std::lock_guard lock(l.m);
    if (l.file) {
        std::fclose(l.file);
        l.file = nullptr;
    }
    l.path = std::move(path);
}

std::string debugLogPath()
{
    Log& l = log();
    std::lock_guard lock(l.m);
    return l.path.empty() ? defaultPath() : l.path;
}

bool startDebugLog(std::size_t maxBytes)
{
    Log& l = log();
    std::lock_guard lock(l.m);
    l.cap = maxBytes;
    if (!l.file && !openLocked(l))
        return false;
    detail::debugLogEnabled = true;
    return true;
}

void stopDebugLog()
{
    Log& l = log();
    detail::debugLogEnabled = false;
    std::lock_guard lock(l.m);
    if (l.file)
        std::fclose(l.file);
    l.file = nullptr;
}

void debugLog(const char* tag, std::string_view msg)
{
    if (!debugLogOn())
        return;
    std::string line = stamp() + " [" + tag + "] ";
    for (char c : msg)
        line += c == '\n' || c == '\r' ? ' ' : c; // one line per event
    line += '\n';
    Log& l = log();
    std::lock_guard lock(l.m);
    if (!l.file && !openLocked(l))
        return; // HEARTH_DEBUG=1 but the state directory is unwritable: stay silent
    if (l.size + line.size() > l.cap && l.size > 0) {
        std::fclose(l.file);
        std::error_code ec;
        fs::rename(l.path, l.path + ".1", ec);
        l.file = nullptr;
        if (!openLocked(l))
            return;
    }
    std::fwrite(line.data(), 1, line.size(), l.file);
    std::fflush(l.file); // a crash is what the log is for
    l.size += line.size();
}

std::string debugKey(std::span<const std::uint8_t> key)
{
    static constexpr char digits[] = "0123456789abcdef";
    std::string out;
    for (std::size_t i = 0; i < 4 && i < key.size(); ++i) {
        out += digits[key[i] >> 4];
        out += digits[key[i] & 15];
    }
    return out;
}

} // namespace hearth
