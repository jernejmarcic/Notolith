// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace hearth {

// Opt-in diagnostic log for troubleshooting a real call (README, "Debug log").
// Off unless HEARTH_DEBUG=1 (or LINK_DEBUG=1) is in the environment or startDebugLog() ran, and
// then a hot path costs one relaxed load: guard anything that formats a string
// with `if (debugLogOn())`. Never log message text, file names, passwords,
// recovery or link codes, vault data or keys; name a peer with debugKey().
namespace detail {
extern std::atomic<bool> debugLogEnabled;
}
inline bool debugLogOn() { return detail::debugLogEnabled.load(std::memory_order_relaxed); }

// Where the log goes; default $XDG_STATE_HOME/hearth/hearth.log (~/.local/state), and no log when neither is an absolute path.
// The default is computed lazily at the first write, so a path set before anything
// logs (even with HEARTH_DEBUG/LINK_DEBUG on) is the one used. An app vendoring the
// link layer must call setDebugLogPath() at startup, or it logs into Hearth's directory.
void setDebugLogPath(std::string path);
std::string debugLogPath();

// Opens the file (mode 0600, directory 0700) and turns the log on. Past maxBytes
// the file becomes <path>.1 (one old file kept) and a fresh one starts.
bool startDebugLog(std::size_t maxBytes = 20u << 20);
void stopDebugLog();

// One line: ISO-8601 local time with milliseconds, [tag], msg. Thread-safe.
// While on without a file yet (HEARTH_DEBUG=1), the default path is opened.
void debugLog(const char* tag, std::string_view msg);

// First 8 hex digits of a public key: all a log may say about who a peer is.
std::string debugKey(std::span<const std::uint8_t> key);

} // namespace hearth
