// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The Jupyter messaging protocol's wire format (jupyter-client's
// documentation, "Messaging in Jupyter"): a multipart ZeroMQ message of
// [identities...] <IDS|MSG> HMACsignature header parent_header metadata
// content [buffers...], each of header/parent_header/metadata/content a
// JSON object, and the signature an HMAC-SHA256 over their raw bytes as
// sent - never a re-serialized, "canonical" form, which is also why
// KernelMessage below keeps the raw bytes it was built or parsed from
// rather than only the parsed QJsonObject.

#include <QByteArray>
#include <QJsonObject>
#include <QList>
#include <QString>

namespace noteAppC::notebook {

/// The delimiter every wire message carries. Frames before it (as read off
/// a socket) are routing identities - a PUB topic on iopub, or a ROUTER
/// envelope - which this client never adds on its own send (it is always a
/// DEALER or SUB talking to exactly one kernel) but must still skip on
/// receive.
extern const QByteArray kMessageDelimiter;

/// One message, header/parent_header/metadata/content plus optional binary
/// buffers. `rawHeader` etc. are the exact JSON bytes signed or verified;
/// `header` etc. are those same bytes parsed, for reading fields. Building
/// a message with headerJSON()/build() keeps the two in sync; decodeMessage
/// keeps them in sync the other way, by parsing exactly the bytes it read.
struct KernelMessage {
    QByteArray rawHeader{"{}"};
    QByteArray rawParentHeader{"{}"};
    QByteArray rawMetadata{"{}"};
    QByteArray rawContent{"{}"};
    QList<QByteArray> buffers;

    QJsonObject header() const;
    QJsonObject parentHeader() const;
    QJsonObject metadata() const;
    QJsonObject content() const;

    QString msgType() const { return header().value(QStringLiteral("msg_type")).toString(); }
    QString msgID() const { return header().value(QStringLiteral("msg_id")).toString(); }
    /// The `msg_id` this reply/status is *for* - empty for a message with no
    /// parent (the very first thing a kernel sends is not a reply).
    QString parentMsgID() const { return parentHeader().value(QStringLiteral("msg_id")).toString(); }
};

/// A fresh header for `msgType`, stamped with a new `msg_id` and the current
/// UTC time, in `session` (this client's own session id, stable for its
/// lifetime) and `username`.
QJsonObject newHeader(const QString &msgType, const QString &session, const QString &username);

/// Builds a message from parsed JSON (the shape callers actually construct
/// requests with); `header` most often comes from newHeader().
KernelMessage buildMessage(const QJsonObject &header, const QJsonObject &parentHeader,
                           const QJsonObject &metadata, const QJsonObject &content,
                           const QList<QByteArray> &buffers = {});

/// HMAC-SHA256 over rawHeader+rawParentHeader+rawMetadata+rawContent, in
/// that order, hex-encoded lower-case - QMessageAuthenticationCode is the
/// whole implementation.
/// Empty `key` is never used by this app (every kernel it launches gets a
/// random one) but is handled the documented way regardless: no signature.
QByteArray signMessage(const KernelMessage &message, const QByteArray &key);

/// The multipart frames to send on a DEALER channel: no leading identity
/// frames, delimiter, signature, the four JSON parts, then any buffers.
QList<QByteArray> encodeMessage(const KernelMessage &message, const QByteArray &key);

/// Parses `frames`, as read whole off a socket (so callers on iopub, which
/// carries a leading PUB topic frame, strip that first), into `message` and
/// verifies its signature against `key`. False - and `message` untouched -
/// when the delimiter is missing, a part is not valid JSON, or the
/// signature does not match; `error`, if given, says which. A bad signature
/// must never be treated as a message from the kernel: see the
/// RejectsABadSignature test.
bool decodeMessage(const QList<QByteArray> &frames, const QByteArray &key, KernelMessage *message,
                    QString *error = nullptr);

} // namespace noteAppC::notebook
