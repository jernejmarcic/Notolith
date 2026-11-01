// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej Marčič
#include "HostIO.h"

#include <fcntl.h>
#include <unistd.h>

namespace noteAppC {

HostIO::HostIO()
{
    m_writeFd = dup(STDOUT_FILENO);
    const int devNull = open("/dev/null", O_WRONLY);
    if (devNull >= 0) {
        dup2(devNull, STDOUT_FILENO);
        close(devNull);
    }
}

bool HostIO::readMessage(PluginMessage *out)
{
    while (true) {
        PluginMessage message;
        const PluginFrameReader::Status status = m_reader.next(&message);
        if (status == PluginFrameReader::Status::Ok) {
            *out = message;
            return true;
        }
        if (status == PluginFrameReader::Status::FrameTooLarge)
            return false; // unrecoverable - the app already closed us out logically
        // MalformedJSON: the frame was already consumed by next(); loop
        // around to see whether a later, valid frame is already buffered
        // before blocking on more bytes.
        if (status == PluginFrameReader::Status::MalformedJSON)
            continue;

        char buffer[65536];
        const ssize_t n = ::read(STDIN_FILENO, buffer, sizeof(buffer));
        if (n <= 0)
            return false; // EOF or error: the app process is gone
        m_reader.feed(QByteArray(buffer, static_cast<int>(n)));
    }
}

void HostIO::writeMessage(const PluginMessage &message)
{
    const QByteArray frame = plugin_protocol_encode(message);
    qint64 offset = 0;
    while (offset < frame.size()) {
        const ssize_t n = ::write(m_writeFd, frame.constData() + offset, static_cast<size_t>(frame.size() - offset));
        if (n <= 0)
            return; // broken pipe - the next readMessage() will report EOF and the host exits
        offset += n;
    }
}

PluginMessage HostIO::request(const QString &method, const QJsonObject &params)
{
    const qint64 id = m_nextRequestID++;
    writeMessage(PluginMessage::request(id, method, params));

    while (true) {
        PluginMessage message;
        if (!readMessage(&message))
            return PluginMessage::errorResponse(id, QStringLiteral("io-error"),
                                                QStringLiteral("lost the connection to the app"));
        if (message.kind == PluginMessage::Kind::Response && message.id == id)
            return message;
        if (message.kind == PluginMessage::Kind::Notification) {
            if (message.method == QLatin1String("shutdown"))
                m_shutdownRequested = true;
            continue; // a vault.event notification arriving mid-wait is simply queued out - dropped, since
                      // nothing here is subscribed to it; the main loop only ever sees those between requests
        }
        // A request from the app, or a response to some other id, while we
        // are blocked on our own nested request: the app does not
        // pipeline, so this should not happen. Nothing useful to do with it
        // here - it is not this call's answer - so it is dropped rather than
        // risking a deadlock trying to service it from inside a plugin call.
    }
}

} // namespace noteAppC
