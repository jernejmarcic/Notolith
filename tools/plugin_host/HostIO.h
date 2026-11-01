// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej
#pragma once

#include <PluginProtocol.h>

namespace noteAppC {

/// The host process's end of the framed protocol (PluginProtocol.h) over
/// its own stdin/stdout. Deliberately not Qt's QFile/QSocketNotifier: the
/// host has nothing else to do while waiting for a frame, so a handful of
/// raw, blocking read()/write() calls on fd 0/an internal fd are simpler
/// than wiring up an event loop for a single-purpose process, and they are
/// what let a host-table call (plugin.h's noteappc_host, called synchronously
/// from inside a plugin) block for its answer without needing a thread.
///
/// Real stdout (fd 1) is duplicated aside at construction and then pointed
/// at /dev/null: a plugin's own stray fprintf(stdout, ...) - or anything
/// linked into it that logs there - must never land in the middle of a
/// length-prefixed frame. Only writeMessage()/request() ever write to the
/// saved fd.
class HostIO {
public:
    HostIO();

    /// Blocks until one full frame has arrived, a malformed one was skipped,
    /// or the pipe is gone. False on EOF/a fatal protocol error (an
    /// oversized frame) - the caller's main loop exits when this happens.
    bool readMessage(PluginMessage *out);

    void writeMessage(const PluginMessage &message);

    /// A synchronous round trip: writes a request and blocks, reading
    /// frames, until the matching response arrives. A "shutdown"
    /// notification seen while waiting sets shutdownRequested() and is
    /// otherwise ignored here - the caller's main loop notices it once this
    /// call returns. Any other stray frame (the app is not supposed to
    /// pipeline a second top-level request while one is outstanding) is
    /// logged to stderr and dropped. On a fatal IO error, synthesises an
    /// `io-error` failure response rather than blocking forever, so a
    /// host-table trampoline always gets an answer to hand back to the
    /// plugin.
    PluginMessage request(const QString &method, const QJsonObject &params);

    bool shutdownRequested() const { return m_shutdownRequested; }

private:
    PluginFrameReader m_reader;
    int m_writeFd = 1;
    qint64 m_nextRequestID = 1;
    bool m_shutdownRequested = false;
};

} // namespace noteAppC
