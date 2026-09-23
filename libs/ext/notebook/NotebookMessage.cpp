#include "NotebookMessage.h"

#include <QDateTime>
#include <QJsonDocument>
#include <QMessageAuthenticationCode>
#include <QUuid>

namespace noteAppC::notebook {

const QByteArray kMessageDelimiter = QByteArrayLiteral("<IDS|MSG>");

namespace {

QByteArray compact(const QJsonObject &object)
{
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

QJsonObject parse(const QByteArray &bytes)
{
    return QJsonDocument::fromJson(bytes).object();
}

} // namespace

QJsonObject KernelMessage::header() const { return parse(rawHeader); }
QJsonObject KernelMessage::parentHeader() const { return parse(rawParentHeader); }
QJsonObject KernelMessage::metadata() const { return parse(rawMetadata); }
QJsonObject KernelMessage::content() const { return parse(rawContent); }

QJsonObject newHeader(const QString &msgType, const QString &session, const QString &username)
{
    QJsonObject header;
    header.insert(QStringLiteral("msg_id"), QUuid::createUuid().toString(QUuid::WithoutBraces));
    header.insert(QStringLiteral("msg_type"), msgType);
    header.insert(QStringLiteral("username"), username);
    header.insert(QStringLiteral("session"), session);
    header.insert(QStringLiteral("date"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    header.insert(QStringLiteral("version"), QStringLiteral("5.3"));
    return header;
}

KernelMessage buildMessage(const QJsonObject &header, const QJsonObject &parentHeader,
                           const QJsonObject &metadata, const QJsonObject &content,
                           const QList<QByteArray> &buffers)
{
    KernelMessage message;
    message.rawHeader = compact(header);
    message.rawParentHeader = compact(parentHeader);
    message.rawMetadata = compact(metadata);
    message.rawContent = compact(content);
    message.buffers = buffers;
    return message;
}

QByteArray signMessage(const KernelMessage &message, const QByteArray &key)
{
    if (key.isEmpty())
        return {};
    QMessageAuthenticationCode hmac(QCryptographicHash::Sha256, key);
    hmac.addData(message.rawHeader);
    hmac.addData(message.rawParentHeader);
    hmac.addData(message.rawMetadata);
    hmac.addData(message.rawContent);
    return hmac.result().toHex();
}

QList<QByteArray> encodeMessage(const KernelMessage &message, const QByteArray &key)
{
    QList<QByteArray> frames{kMessageDelimiter,     signMessage(message, key),
                             message.rawHeader,       message.rawParentHeader,
                             message.rawMetadata,     message.rawContent};
    frames.append(message.buffers);
    return frames;
}

bool decodeMessage(const QList<QByteArray> &frames, const QByteArray &key, KernelMessage *message,
                   QString *error)
{
    const auto fail = [error](const QString &why) {
        if (error)
            *error = why;
        return false;
    };

    const qsizetype delim = frames.indexOf(kMessageDelimiter);
    if (delim < 0)
        return fail(QStringLiteral("no <IDS|MSG> delimiter"));
    if (frames.size() < delim + 6)
        return fail(QStringLiteral("message truncated after the delimiter"));

    const QByteArray &signature = frames.at(delim + 1);
    KernelMessage parsed;
    parsed.rawHeader = frames.at(delim + 2);
    parsed.rawParentHeader = frames.at(delim + 3);
    parsed.rawMetadata = frames.at(delim + 4);
    parsed.rawContent = frames.at(delim + 5);
    for (qsizetype i = delim + 6; i < frames.size(); ++i)
        parsed.buffers.append(frames.at(i));

    const QByteArray expected = signMessage(parsed, key);
    // Constant-time-ish by using QByteArray's own compare is not essential
    // here (the key is per-kernel and random, not a shared secret defending
    // a remote endpoint over the network - this client only ever talks to a
    // kernel it launched itself, over a unix socket only it and that kernel
    // can reach), but verifying it at all is: an unsigned or mis-signed
    // frame must never be treated as real.
    if (!key.isEmpty() && signature != expected)
        return fail(QStringLiteral("signature mismatch"));

    QJsonParseError jsonError;
    for (const QByteArray *part : {&parsed.rawHeader, &parsed.rawParentHeader, &parsed.rawMetadata,
                                   &parsed.rawContent}) {
        QJsonDocument::fromJson(*part, &jsonError);
        if (jsonError.error != QJsonParseError::NoError)
            return fail(QStringLiteral("malformed JSON: %1").arg(jsonError.errorString()));
    }

    *message = std::move(parsed);
    return true;
}

} // namespace noteAppC::notebook
