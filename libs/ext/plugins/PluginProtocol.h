#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QString>

#include <optional>

namespace noteAppC {

/// The wire format between the app and one `noteAppC-plugin-host` process
/// (docs/PLUGINS.md, Tier 2): a 4-byte big-endian length prefix followed by
/// that many bytes of compact UTF-8 JSON. Big-endian so a frame's first
/// bytes are legible in a hex dump without remembering the machine's
/// endianness - the payload itself never crosses a machine boundary, so
/// there is no interop reason either way.
///
/// Both directions use the same three message kinds. A request expects
/// exactly one response carrying the same id; a notification expects none.
/// Either side can send a request - the app asks the host to run a command
/// or render a block, and the host asks the app to read a note, write one,
/// or show a notice, which is how noteappc_host's synchronous C calls
/// (plugin.h) turn into a round trip without either side needing threads.
struct PluginMessage {
    enum class Kind { Request, Response, Notification };
    Kind kind = Kind::Notification;
    qint64 id = 0;    ///< Request/Response only; matches a request to its reply.
    QString method;   ///< e.g. "command.run", "note.read", "vault.event".
    QJsonObject params; ///< Request/Notification payload.
    bool ok = true;     ///< Response only.
    QJsonObject result; ///< Response, ok == true.
    QString errorCode;  ///< Response, ok == false - "permission-denied", "not-found", ...
    QString errorMessage;

    static PluginMessage request(qint64 id, const QString &method, const QJsonObject &params = {});
    static PluginMessage notification(const QString &method, const QJsonObject &params = {});
    static PluginMessage response(qint64 id, const QJsonObject &result = {});
    static PluginMessage errorResponse(qint64 id, const QString &code, const QString &message);
};

/// Serialises one message to a length-prefixed frame ready to write to a
/// pipe.
QByteArray plugin_protocol_encode(const PluginMessage &message);

/// Accumulates bytes read off a pipe (which may arrive split anywhere, or
/// several frames at once) and hands back whole messages as they complete.
/// One instance per direction per host process; not thread-safe, matching
/// every other piece of this protocol, which is single-threaded by design.
class PluginFrameReader {
public:
    enum class Status {
        NeedMoreData, ///< no complete frame yet - not an error
        Ok,
        FrameTooLarge, ///< length prefix exceeds maxFrameSize(); the stream is unusable, close it
        MalformedJSON, ///< the frame's bytes did not parse as a JSON object
    };

    /// The largest single frame accepted, 32 MiB - generous enough for a
    /// base64 code-block image at a 4K width and still a firm ceiling
    /// against a runaway or hostile host writing garbage length prefixes.
    static constexpr qint64 maxFrameSize() { return 32LL * 1024 * 1024; }

    void feed(const QByteArray &bytes) { m_buffer.append(bytes); }

    /// Pops the oldest complete message. `NeedMoreData` leaves the buffer
    /// untouched so a later feed() can complete it; any other status
    /// consumes the malformed/oversized frame so the reader can try to
    /// resynchronise on the next one (best-effort only - a corrupted length
    /// prefix cannot be resynchronised at all, and the caller should treat
    /// FrameTooLarge as fatal for the connection).
    Status next(PluginMessage *out);

private:
    QByteArray m_buffer;
};

} // namespace noteAppC
