#include "SyncClient.h"

#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>

namespace noteAppC {

namespace {

QByteArray b64(const QByteArray &raw)
{
    return raw.toBase64();
}

QByteArray unb64(const QJsonObject &json, const char *key)
{
    return QByteArray::fromBase64(json.value(QLatin1String(key)).toString().toLatin1());
}

} // namespace

SyncClient::SyncClient(const QString &serverURL, QObject *parent)
    : QObject(parent)
    , m_baseURL(serverURL)
{
    if (m_baseURL.endsWith(QLatin1Char('/')))
        m_baseURL.chop(1);
}

SyncClient::Reply SyncClient::callRaw(const QByteArray &verb, const QString &path, const QByteArray &body,
                                      const char *contentType)
{
    QNetworkRequest request(QUrl(m_baseURL + path));
    request.setHeader(QNetworkRequest::ContentTypeHeader, QString::fromLatin1(contentType));
    if (!m_token.isEmpty())
        // Hex, not the raw bytes: a bearer token is random binary and an
        // HTTP header value has to stay ASCII-safe text.
        request.setRawHeader("Authorization", "Bearer " + m_token.toHex());

    QEventLoop loop;
    QNetworkReply *reply = nullptr;
    if (verb == "GET")
        reply = m_net.get(request);
    else if (verb == "POST")
        reply = m_net.post(request, body);
    else if (verb == "PUT")
        reply = m_net.put(request, body);
    else
        return {};
    connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    // A dead or unreachable server must not hang the sync thread forever -
    // sync is timer-driven and just tries again next cycle.
    QTimer::singleShot(15000, &loop, &QEventLoop::quit);
    loop.exec();

    Reply out;
    out.status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    out.body = reply->readAll();
    reply->deleteLater();
    if (!out.body.isEmpty()) {
        const QJsonDocument doc = QJsonDocument::fromJson(out.body);
        if (doc.isObject())
            out.json = doc.object();
    }
    return out;
}

SyncClient::Reply SyncClient::call(const QByteArray &verb, const QString &path, const QJsonObject &body)
{
    return callRaw(verb, path, QJsonDocument(body).toJson(QJsonDocument::Compact), "application/json");
}

SyncClient::Salt SyncClient::fetchSalt(const QString &email)
{
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("email"), email);
    const Reply reply = call("GET", QStringLiteral("/v1/salt?") + query.toString(QUrl::FullyEncoded));
    Salt out;
    out.ok = reply.status == 200;
    if (out.ok)
        out.salt = unb64(reply.json, "salt");
    return out;
}

SyncClient::Auth SyncClient::registerAccount(const QString &email, const QByteArray &salt,
                                             const QByteArray &loginKey)
{
    const Reply reply = call("POST", QStringLiteral("/v1/accounts"),
                             {{QStringLiteral("email"), email},
                              {QStringLiteral("salt"), QString::fromLatin1(b64(salt))},
                              {QStringLiteral("loginKey"), QString::fromLatin1(b64(loginKey))}});
    Auth out;
    out.ok = reply.status == 201;
    out.status = reply.status;
    out.error = reply.json.value(QStringLiteral("error")).toString();
    if (out.ok) {
        out.accountID = unb64(reply.json, "accountID");
        out.token = unb64(reply.json, "token");
    }
    return out;
}

SyncClient::Auth SyncClient::signIn(const QString &email, const QByteArray &loginKey)
{
    const Reply reply = call("POST", QStringLiteral("/v1/sessions"),
                             {{QStringLiteral("email"), email},
                              {QStringLiteral("loginKey"), QString::fromLatin1(b64(loginKey))}});
    Auth out;
    out.ok = reply.status == 200;
    out.status = reply.status;
    out.error = reply.json.value(QStringLiteral("error")).toString();
    if (out.ok) {
        out.accountID = unb64(reply.json, "accountID");
        out.token = unb64(reply.json, "token");
    }
    return out;
}

bool SyncClient::changePassword(const QByteArray &vaultID, const QByteArray &newSalt, const QByteArray &newLoginKey,
                                const QList<QPair<QByteArray, QByteArray>> &rewrappedItemsKeys)
{
    QJsonArray keys;
    for (const auto &[id, wrapped] : rewrappedItemsKeys)
        keys.append(QJsonObject{{QStringLiteral("itemsKeyID"), QString::fromLatin1(b64(id))},
                                {QStringLiteral("wrapped"), QString::fromLatin1(b64(wrapped))}});
    const Reply reply = call("POST", QStringLiteral("/v1/account/password"),
                             {{QStringLiteral("vaultID"), QString::fromLatin1(b64(vaultID))},
                              {QStringLiteral("newSalt"), QString::fromLatin1(b64(newSalt))},
                              {QStringLiteral("newLoginKey"), QString::fromLatin1(b64(newLoginKey))},
                              {QStringLiteral("itemsKeys"), keys}});
    return reply.status == 200;
}

SyncClient::Created SyncClient::createVault()
{
    const Reply reply = call("POST", QStringLiteral("/v1/vaults"));
    Created out;
    out.ok = reply.status == 201;
    if (out.ok)
        out.id = unb64(reply.json, "vaultID");
    return out;
}

QList<QByteArray> SyncClient::listVaults()
{
    QList<QByteArray> out;
    const Reply reply = call("GET", QStringLiteral("/v1/vaults"));
    for (const QJsonValue &v : reply.json.value(QStringLiteral("vaults")).toArray())
        out << QByteArray::fromBase64(v.toString().toLatin1());
    return out;
}

SyncClient::PushResult SyncClient::pushJournal(const QByteArray &vaultID, const QList<JournalRecord> &records)
{
    QJsonArray array;
    for (const JournalRecord &r : records) {
        array.append(QJsonObject{{QStringLiteral("entityID"), QString::fromLatin1(b64(r.entityID))},
                                 {QStringLiteral("recordID"), QString::fromLatin1(b64(r.recordID))},
                                 {QStringLiteral("deviceID"), QString::fromLatin1(b64(r.deviceID))},
                                 {QStringLiteral("itemsKeyID"), QString::fromLatin1(b64(r.itemsKeyID))},
                                 {QStringLiteral("wrappedNoteKey"), QString::fromLatin1(b64(r.wrappedNoteKey))},
                                 {QStringLiteral("payload"), QString::fromLatin1(b64(r.payload))}});
    }
    const Reply reply = call("POST",
                             QStringLiteral("/v1/vaults/%1/journal").arg(QString::fromLatin1(vaultID.toHex())),
                             {{QStringLiteral("records"), array}});
    PushResult out;
    out.ok = reply.status == 200;
    if (out.ok) {
        for (const QJsonValue &v : reply.json.value(QStringLiteral("seqs")).toArray())
            out.seqs << v.toInteger();
    }
    return out;
}

SyncClient::PullResult SyncClient::pullJournal(const QByteArray &vaultID, qint64 since, int limit)
{
    const Reply reply =
        call("GET", QStringLiteral("/v1/vaults/%1/journal?since=%2&limit=%3")
                        .arg(QString::fromLatin1(vaultID.toHex()))
                        .arg(since)
                        .arg(limit));
    PullResult out;
    out.ok = reply.status == 200;
    if (!out.ok)
        return out;
    for (const QJsonValue &v : reply.json.value(QStringLiteral("records")).toArray()) {
        const QJsonObject o = v.toObject();
        JournalRecord r;
        r.seq = o.value(QStringLiteral("seq")).toInteger();
        r.entityID = unb64(o, "entityID");
        r.recordID = unb64(o, "recordID");
        r.deviceID = unb64(o, "deviceID");
        r.itemsKeyID = unb64(o, "itemsKeyID");
        r.wrappedNoteKey = unb64(o, "wrappedNoteKey");
        r.payload = unb64(o, "payload");
        out.records << r;
    }
    return out;
}

bool SyncClient::putBlob(const QByteArray &vaultID, const QByteArray &address, const QByteArray &bytes)
{
    const Reply reply = callRaw("PUT",
                                QStringLiteral("/v1/vaults/%1/blobs/%2")
                                    .arg(QString::fromLatin1(vaultID.toHex()), QString::fromLatin1(address.toHex())),
                                bytes);
    return reply.status == 200 || reply.status == 201;
}

SyncClient::BlobResult SyncClient::getBlob(const QByteArray &vaultID, const QByteArray &address)
{
    const Reply reply = callRaw("GET",
                                QStringLiteral("/v1/vaults/%1/blobs/%2")
                                    .arg(QString::fromLatin1(vaultID.toHex()), QString::fromLatin1(address.toHex())),
                                {});
    BlobResult out;
    out.ok = reply.status == 200;
    out.bytes = reply.body;
    return out;
}

bool SyncClient::registerDevice(const QByteArray &vaultID, const QByteArray &deviceID, const QByteArray &publicKey,
                                bool wantsLockbox)
{
    const Reply reply =
        call("POST", QStringLiteral("/v1/vaults/%1/devices").arg(QString::fromLatin1(vaultID.toHex())),
            {{QStringLiteral("deviceID"), QString::fromLatin1(b64(deviceID))},
             {QStringLiteral("publicKey"), QString::fromLatin1(b64(publicKey))},
             {QStringLiteral("lockbox"), wantsLockbox}});
    return reply.status == 200;
}

QList<SyncClient::DeviceInfo> SyncClient::listDevices(const QByteArray &vaultID)
{
    QList<DeviceInfo> out;
    const Reply reply =
        call("GET", QStringLiteral("/v1/vaults/%1/devices").arg(QString::fromLatin1(vaultID.toHex())));
    for (const QJsonValue &v : reply.json.value(QStringLiteral("devices")).toArray()) {
        const QJsonObject o = v.toObject();
        out << DeviceInfo{unb64(o, "deviceID"), unb64(o, "publicKey"), o.value(QStringLiteral("lockbox")).toBool()};
    }
    return out;
}

bool SyncClient::putLockbox(const QByteArray &vaultID, const QByteArray &forDevice, const QByteArray &itemsKeyID,
                            const QByteArray &sealed)
{
    const Reply reply =
        call("POST", QStringLiteral("/v1/vaults/%1/lockboxes").arg(QString::fromLatin1(vaultID.toHex())),
            {{QStringLiteral("forDevice"), QString::fromLatin1(b64(forDevice))},
             {QStringLiteral("itemsKeyID"), QString::fromLatin1(b64(itemsKeyID))},
             {QStringLiteral("sealed"), QString::fromLatin1(b64(sealed))}});
    return reply.status == 200;
}

QList<SyncClient::Lockbox> SyncClient::listLockboxes(const QByteArray &vaultID, const QByteArray &forDevice)
{
    QList<Lockbox> out;
    const Reply reply = call("GET", QStringLiteral("/v1/vaults/%1/lockboxes?forDevice=%2")
                                        .arg(QString::fromLatin1(vaultID.toHex()), QString::fromLatin1(forDevice.toHex())));
    for (const QJsonValue &v : reply.json.value(QStringLiteral("lockboxes")).toArray()) {
        const QJsonObject o = v.toObject();
        out << Lockbox{unb64(o, "itemsKeyID"), unb64(o, "sealed")};
    }
    return out;
}

bool SyncClient::putItemsKey(const QByteArray &vaultID, const QByteArray &itemsKeyID, const QByteArray &wrapped)
{
    const Reply reply =
        call("POST", QStringLiteral("/v1/vaults/%1/itemskeys").arg(QString::fromLatin1(vaultID.toHex())),
            {{QStringLiteral("itemsKeyID"), QString::fromLatin1(b64(itemsKeyID))},
             {QStringLiteral("wrapped"), QString::fromLatin1(b64(wrapped))}});
    return reply.status == 200;
}

QList<SyncClient::WrappedItemsKey> SyncClient::fetchItemsKeys(const QByteArray &vaultID, const QByteArray &asDevice)
{
    QList<WrappedItemsKey> out;
    const Reply reply = call("GET", QStringLiteral("/v1/vaults/%1/itemskeys?device=%2")
                                        .arg(QString::fromLatin1(vaultID.toHex()), QString::fromLatin1(asDevice.toHex())));
    for (const QJsonValue &v : reply.json.value(QStringLiteral("itemsKeys")).toArray()) {
        const QJsonObject o = v.toObject();
        out << WrappedItemsKey{unb64(o, "itemsKeyID"), unb64(o, "wrapped")};
    }
    return out;
}

} // namespace noteAppC
