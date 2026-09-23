#include "PluginProtocol.h"

#include <QJsonDocument>
#include <QJsonParseError>

namespace noteAppC {

namespace {
constexpr char kType[] = "type";
constexpr char kTypeRequest[] = "req";
constexpr char kTypeResponse[] = "res";
constexpr char kTypeNotification[] = "note";
constexpr char kId[] = "id";
constexpr char kMethod[] = "method";
constexpr char kParams[] = "params";
constexpr char kOk[] = "ok";
constexpr char kResult[] = "result";
constexpr char kError[] = "error";
constexpr char kErrorCode[] = "code";
constexpr char kErrorMessage[] = "message";
} // namespace

PluginMessage PluginMessage::request(qint64 id, const QString &method, const QJsonObject &params)
{
    PluginMessage message;
    message.kind = Kind::Request;
    message.id = id;
    message.method = method;
    message.params = params;
    return message;
}

PluginMessage PluginMessage::notification(const QString &method, const QJsonObject &params)
{
    PluginMessage message;
    message.kind = Kind::Notification;
    message.method = method;
    message.params = params;
    return message;
}

PluginMessage PluginMessage::response(qint64 id, const QJsonObject &result)
{
    PluginMessage message;
    message.kind = Kind::Response;
    message.id = id;
    message.ok = true;
    message.result = result;
    return message;
}

PluginMessage PluginMessage::errorResponse(qint64 id, const QString &code, const QString &message)
{
    PluginMessage out;
    out.kind = Kind::Response;
    out.id = id;
    out.ok = false;
    out.errorCode = code;
    out.errorMessage = message;
    return out;
}

QByteArray plugin_protocol_encode(const PluginMessage &message)
{
    QJsonObject object;
    switch (message.kind) {
    case PluginMessage::Kind::Request:
        object.insert(QLatin1String(kType), QLatin1String(kTypeRequest));
        object.insert(QLatin1String(kId), message.id);
        object.insert(QLatin1String(kMethod), message.method);
        object.insert(QLatin1String(kParams), message.params);
        break;
    case PluginMessage::Kind::Notification:
        object.insert(QLatin1String(kType), QLatin1String(kTypeNotification));
        object.insert(QLatin1String(kMethod), message.method);
        object.insert(QLatin1String(kParams), message.params);
        break;
    case PluginMessage::Kind::Response:
        object.insert(QLatin1String(kType), QLatin1String(kTypeResponse));
        object.insert(QLatin1String(kId), message.id);
        object.insert(QLatin1String(kOk), message.ok);
        if (message.ok) {
            object.insert(QLatin1String(kResult), message.result);
        } else {
            QJsonObject error;
            error.insert(QLatin1String(kErrorCode), message.errorCode);
            error.insert(QLatin1String(kErrorMessage), message.errorMessage);
            object.insert(QLatin1String(kError), error);
        }
        break;
    }

    const QByteArray payload = QJsonDocument(object).toJson(QJsonDocument::Compact);
    // Big-endian length prefix, written by hand rather than through
    // QDataStream: this is the one place the wire format is decided, and a
    // stray QDataStream version mismatch between the app and an
    // independently-built host binary would be a much stranger bug to chase
    // than four explicit bytes.
    QByteArray frame;
    frame.reserve(4 + payload.size());
    const quint32 length = static_cast<quint32>(payload.size());
    frame.append(static_cast<char>((length >> 24) & 0xff));
    frame.append(static_cast<char>((length >> 16) & 0xff));
    frame.append(static_cast<char>((length >> 8) & 0xff));
    frame.append(static_cast<char>(length & 0xff));
    frame.append(payload);
    return frame;
}

PluginFrameReader::Status PluginFrameReader::next(PluginMessage *out)
{
    if (m_buffer.size() < 4)
        return Status::NeedMoreData;

    const auto byte = [this](int index) { return static_cast<quint32>(static_cast<quint8>(m_buffer.at(index))); };
    const quint32 length = (byte(0) << 24) | (byte(1) << 16) | (byte(2) << 8) | byte(3);
    if (length > static_cast<quint32>(maxFrameSize())) {
        // No amount of further feed()ing makes this frame valid; the buffer
        // is dropped so the caller's next NeedMoreData does not silently
        // keep accumulating gigabytes behind a bad length prefix. The
        // connection is not resynchronisable from here - the caller closes
        // the host.
        m_buffer.clear();
        return Status::FrameTooLarge;
    }
    if (m_buffer.size() < 4 + static_cast<qint64>(length))
        return Status::NeedMoreData;

    const QByteArray payload = m_buffer.mid(4, static_cast<int>(length));
    m_buffer.remove(0, 4 + static_cast<int>(length));

    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(payload, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject())
        return Status::MalformedJSON;

    const QJsonObject object = doc.object();
    const QString type = object.value(QLatin1String(kType)).toString();
    PluginMessage message;
    if (type == QLatin1String(kTypeRequest)) {
        message.kind = PluginMessage::Kind::Request;
        message.id = object.value(QLatin1String(kId)).toVariant().toLongLong();
        message.method = object.value(QLatin1String(kMethod)).toString();
        message.params = object.value(QLatin1String(kParams)).toObject();
    } else if (type == QLatin1String(kTypeNotification)) {
        message.kind = PluginMessage::Kind::Notification;
        message.method = object.value(QLatin1String(kMethod)).toString();
        message.params = object.value(QLatin1String(kParams)).toObject();
    } else if (type == QLatin1String(kTypeResponse)) {
        message.kind = PluginMessage::Kind::Response;
        message.id = object.value(QLatin1String(kId)).toVariant().toLongLong();
        message.ok = object.value(QLatin1String(kOk)).toBool();
        if (message.ok) {
            message.result = object.value(QLatin1String(kResult)).toObject();
        } else {
            const QJsonObject error = object.value(QLatin1String(kError)).toObject();
            message.errorCode = error.value(QLatin1String(kErrorCode)).toString();
            message.errorMessage = error.value(QLatin1String(kErrorMessage)).toString();
        }
    } else {
        return Status::MalformedJSON;
    }

    *out = message;
    return Status::Ok;
}

} // namespace noteAppC
