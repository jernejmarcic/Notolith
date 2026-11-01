// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej
#include "SyncServer.h"

#include <QFile>
#include <QHttpServerRequest>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRandomGenerator>
#include <QSslCertificate>
#include <QSslKey>
#include <QSslServer>
#include <QTcpServer>
#include <QUrlQuery>

#include "SyncCrypto.h"

namespace noteAppC {

namespace {

using Method = QHttpServerRequest::Method;
using StatusCode = QHttpServerResponse::StatusCode;

QByteArray unb64(const QJsonObject &o, const char *key)
{
    return QByteArray::fromBase64(o.value(QLatin1String(key)).toString().toLatin1());
}

QString b64(const QByteArray &raw)
{
    return QString::fromLatin1(raw.toBase64());
}

QJsonObject bodyOf(const QHttpServerRequest &request)
{
    return QJsonDocument::fromJson(request.body()).object();
}

QByteArray hexArg(const QString &s)
{
    return QByteArray::fromHex(s.toLatin1());
}

QHttpServerResponse errorResponse(StatusCode status, const QString &message)
{
    return QHttpServerResponse(QJsonObject{{QStringLiteral("error"), message}}, status);
}

} // namespace

SyncServer::SyncServer(ServerStore *store, QObject *parent)
    : QObject(parent)
    , m_store(store)
{
    registerRoutes();
}

bool SyncServer::listen(const QString &host, quint16 port, const QString &certFile, const QString &keyFile)
{
    if (!certFile.isEmpty() && !keyFile.isEmpty()) {
#if QT_CONFIG(ssl)
        auto *tcp = new QSslServer(this);
        QSslConfiguration config = QSslConfiguration::defaultConfiguration();
        const QList<QSslCertificate> chain = QSslCertificate::fromPath(certFile);
        QFile keyIn(keyFile);
        if (!keyIn.open(QIODevice::ReadOnly))
            return false;
        const QSslKey key(keyIn.readAll(), QSsl::Rsa);
        config.setLocalCertificateChain(chain);
        config.setPrivateKey(key);
        tcp->setSslConfiguration(config);
        if (!tcp->listen(QHostAddress(host), port))
            return false;
        return m_server.bind(tcp);
#else
        return false;
#endif
    }
    auto *tcp = new QTcpServer(this);
    if (!tcp->listen(QHostAddress(host), port))
        return false;
    return m_server.bind(tcp);
}

quint16 SyncServer::actualPort() const
{
    const QList<QTcpServer *> servers = m_server.servers();
    return servers.isEmpty() ? 0 : servers.first()->serverPort();
}

QByteArray SyncServer::authenticate(const QHttpServerRequest &request) const
{
    const QByteArray header = request.value("Authorization");
    if (!header.startsWith("Bearer "))
        return {};
    const QByteArray token = QByteArray::fromHex(header.mid(7));
    const auto accountID = m_store->accountForToken(token);
    return accountID ? *accountID : QByteArray();
}

void SyncServer::registerRoutes()
{
    m_server.route("/v1/salt", Method::Get, [this](const QHttpServerRequest &request) {
        const QString email = QUrlQuery(request.query()).queryItemValue(QStringLiteral("email"), QUrl::FullyDecoded);
        return QHttpServerResponse(QJsonObject{{QStringLiteral("salt"), b64(m_store->saltFor(email))}});
    });

    m_server.route("/v1/accounts", Method::Post, [this](const QHttpServerRequest &request) {
        const QJsonObject body = bodyOf(request);
        const QString email = body.value(QStringLiteral("email")).toString();
        const QByteArray salt = unb64(body, "salt");
        const QByteArray loginKey = unb64(body, "loginKey");
        if (email.isEmpty() || salt.size() != SyncCrypto::kSaltBytes || loginKey.isEmpty())
            return errorResponse(StatusCode::BadRequest, QStringLiteral("missing or malformed field"));
        QByteArray accountID;
        QString error;
        if (!m_store->createAccount(email, salt, loginKey, &accountID, &error))
            return errorResponse(StatusCode::Conflict, error);
        const QByteArray token = m_store->createSession(accountID);
        return QHttpServerResponse(
            QJsonObject{{QStringLiteral("accountID"), b64(accountID)}, {QStringLiteral("token"), b64(token)}},
            StatusCode::Created);
    });

    m_server.route("/v1/sessions", Method::Post, [this](const QHttpServerRequest &request) {
        const QJsonObject body = bodyOf(request);
        const QString email = body.value(QStringLiteral("email")).toString();
        const QByteArray loginKey = unb64(body, "loginKey");
        if (m_store->loginRateLimited(email))
            return errorResponse(StatusCode::TooManyRequests, QStringLiteral("too many attempts, slow down"));
        const auto account = m_store->account(email);
        // Same error either way (no account vs wrong key): the response
        // must not tell an attacker which one it was.
        if (!account || !SyncCrypto::verifyLoginKey(account->loginHash, loginKey))
            return errorResponse(StatusCode::Unauthorized, QStringLiteral("invalid email or login key"));
        const QByteArray token = m_store->createSession(account->accountID);
        return QHttpServerResponse(QJsonObject{{QStringLiteral("accountID"), b64(account->accountID)},
                                               {QStringLiteral("token"), b64(token)}});
    });

    m_server.route("/v1/account/password", Method::Post, [this](const QHttpServerRequest &request) {
        const QByteArray accountID = authenticate(request);
        if (accountID.isEmpty())
            return errorResponse(StatusCode::Unauthorized, QStringLiteral("sign in first"));
        const QJsonObject body = bodyOf(request);
        const QByteArray vaultID = unb64(body, "vaultID");
        const QByteArray newSalt = unb64(body, "newSalt");
        const QByteArray newLoginKey = unb64(body, "newLoginKey");
        if (!m_store->vaultBelongsTo(vaultID, accountID))
            return errorResponse(StatusCode::Forbidden, QStringLiteral("not your vault"));
        QList<ServerStore::WrappedItemsKey> keys;
        for (const QJsonValue &v : body.value(QStringLiteral("itemsKeys")).toArray()) {
            const QJsonObject o = v.toObject();
            keys << ServerStore::WrappedItemsKey{unb64(o, "itemsKeyID"), unb64(o, "wrapped")};
        }
        if (!m_store->updatePassword(accountID, newSalt, newLoginKey)
            || !m_store->replaceItemsKeys(vaultID, keys))
            return errorResponse(StatusCode::InternalServerError, QStringLiteral("could not update"));
        return QHttpServerResponse(StatusCode::Ok);
    });

    m_server.route("/v1/vaults", Method::Post, [this](const QHttpServerRequest &request) {
        const QByteArray accountID = authenticate(request);
        if (accountID.isEmpty())
            return errorResponse(StatusCode::Unauthorized, QStringLiteral("sign in first"));
        const QByteArray vaultID = m_store->createVault(accountID);
        return QHttpServerResponse(QJsonObject{{QStringLiteral("vaultID"), b64(vaultID)}}, StatusCode::Created);
    });

    m_server.route("/v1/vaults", Method::Get, [this](const QHttpServerRequest &request) {
        const QByteArray accountID = authenticate(request);
        if (accountID.isEmpty())
            return errorResponse(StatusCode::Unauthorized, QStringLiteral("sign in first"));
        QJsonArray array;
        for (const QByteArray &id : m_store->vaultsFor(accountID))
            array.append(b64(id));
        return QHttpServerResponse(QJsonObject{{QStringLiteral("vaults"), array}});
    });

    m_server.route("/v1/vaults/<arg>/journal", Method::Post,
                   [this](const QString &vaultIDHex, const QHttpServerRequest &request) {
                       const QByteArray accountID = authenticate(request);
                       const QByteArray vaultID = hexArg(vaultIDHex);
                       if (accountID.isEmpty() || !m_store->vaultBelongsTo(vaultID, accountID))
                           return errorResponse(StatusCode::Forbidden, QStringLiteral("not your vault"));
                       QList<ServerStore::JournalRow> rows;
                       for (const QJsonValue &v : bodyOf(request).value(QStringLiteral("records")).toArray()) {
                           const QJsonObject o = v.toObject();
                           ServerStore::JournalRow r;
                           r.entityID = unb64(o, "entityID");
                           r.recordID = unb64(o, "recordID");
                           r.deviceID = unb64(o, "deviceID");
                           r.itemsKeyID = unb64(o, "itemsKeyID");
                           r.wrappedNoteKey = unb64(o, "wrappedNoteKey");
                           r.payload = unb64(o, "payload");
                           rows << r;
                       }
                       const QList<qint64> seqs = m_store->appendJournal(vaultID, rows);
                       QJsonArray array;
                       for (qint64 seq : seqs)
                           array.append(seq);
                       return QHttpServerResponse(QJsonObject{{QStringLiteral("seqs"), array}});
                   });

    m_server.route("/v1/vaults/<arg>/journal", Method::Get,
                   [this](const QString &vaultIDHex, const QHttpServerRequest &request) {
                       const QByteArray accountID = authenticate(request);
                       const QByteArray vaultID = hexArg(vaultIDHex);
                       if (accountID.isEmpty() || !m_store->vaultBelongsTo(vaultID, accountID))
                           return errorResponse(StatusCode::Forbidden, QStringLiteral("not your vault"));
                       const QUrlQuery query(request.query());
                       const qint64 since = query.queryItemValue(QStringLiteral("since")).toLongLong();
                       int limit = query.queryItemValue(QStringLiteral("limit")).toInt();
                       if (limit <= 0 || limit > 500)
                           limit = 500;
                       QJsonArray array;
                       for (const ServerStore::JournalRow &r : m_store->journalSince(vaultID, since, limit)) {
                           array.append(QJsonObject{{QStringLiteral("seq"), double(r.seq)},
                                                    {QStringLiteral("entityID"), b64(r.entityID)},
                                                    {QStringLiteral("recordID"), b64(r.recordID)},
                                                    {QStringLiteral("deviceID"), b64(r.deviceID)},
                                                    {QStringLiteral("itemsKeyID"), b64(r.itemsKeyID)},
                                                    {QStringLiteral("wrappedNoteKey"), b64(r.wrappedNoteKey)},
                                                    {QStringLiteral("payload"), b64(r.payload)}});
                       }
                       return QHttpServerResponse(QJsonObject{{QStringLiteral("records"), array}});
                   });

    m_server.route(
        "/v1/vaults/<arg>/blobs/<arg>", Method::Put,
        [this](const QString &vaultIDHex, const QString &addressHex, const QHttpServerRequest &request) {
            const QByteArray accountID = authenticate(request);
            const QByteArray vaultID = hexArg(vaultIDHex);
            if (accountID.isEmpty() || !m_store->vaultBelongsTo(vaultID, accountID))
                return errorResponse(StatusCode::Forbidden, QStringLiteral("not your vault"));
            if (!m_store->putBlob(vaultID, hexArg(addressHex), request.body()))
                return QHttpServerResponse(StatusCode::InternalServerError);
            return QHttpServerResponse(StatusCode::Created);
        });

    m_server.route(
        "/v1/vaults/<arg>/blobs/<arg>", Method::Get,
        [this](const QString &vaultIDHex, const QString &addressHex, const QHttpServerRequest &request) {
            const QByteArray accountID = authenticate(request);
            const QByteArray vaultID = hexArg(vaultIDHex);
            if (accountID.isEmpty() || !m_store->vaultBelongsTo(vaultID, accountID))
                return QHttpServerResponse(StatusCode::Forbidden);
            const auto bytes = m_store->getBlob(vaultID, hexArg(addressHex));
            if (!bytes)
                return QHttpServerResponse(StatusCode::NotFound);
            return QHttpServerResponse(QByteArrayLiteral("application/octet-stream"), *bytes);
        });

    m_server.route("/v1/vaults/<arg>/devices", Method::Post,
                   [this](const QString &vaultIDHex, const QHttpServerRequest &request) {
                       const QByteArray accountID = authenticate(request);
                       const QByteArray vaultID = hexArg(vaultIDHex);
                       if (accountID.isEmpty() || !m_store->vaultBelongsTo(vaultID, accountID))
                           return errorResponse(StatusCode::Forbidden, QStringLiteral("not your vault"));
                       const QJsonObject body = bodyOf(request);
                       m_store->registerDevice(vaultID, unb64(body, "deviceID"), unb64(body, "publicKey"),
                                               body.value(QStringLiteral("lockbox")).toBool());
                       return QHttpServerResponse(StatusCode::Ok);
                   });

    m_server.route("/v1/vaults/<arg>/devices", Method::Get,
                   [this](const QString &vaultIDHex, const QHttpServerRequest &request) {
                       const QByteArray accountID = authenticate(request);
                       const QByteArray vaultID = hexArg(vaultIDHex);
                       if (accountID.isEmpty() || !m_store->vaultBelongsTo(vaultID, accountID))
                           return errorResponse(StatusCode::Forbidden, QStringLiteral("not your vault"));
                       QJsonArray array;
                       for (const ServerStore::Device &d : m_store->devices(vaultID))
                           array.append(QJsonObject{{QStringLiteral("deviceID"), b64(d.deviceID)},
                                                    {QStringLiteral("publicKey"), b64(d.publicKey)},
                                                    {QStringLiteral("approved"), d.approved},
                                                    {QStringLiteral("lockbox"), d.wantsLockbox}});
                       return QHttpServerResponse(QJsonObject{{QStringLiteral("devices"), array}});
                   });

    m_server.route("/v1/vaults/<arg>/itemskeys", Method::Post,
                   [this](const QString &vaultIDHex, const QHttpServerRequest &request) {
                       const QByteArray accountID = authenticate(request);
                       const QByteArray vaultID = hexArg(vaultIDHex);
                       if (accountID.isEmpty() || !m_store->vaultBelongsTo(vaultID, accountID))
                           return errorResponse(StatusCode::Forbidden, QStringLiteral("not your vault"));
                       const QJsonObject body = bodyOf(request);
                       if (!m_store->putItemsKey(vaultID, unb64(body, "itemsKeyID"), unb64(body, "wrapped")))
                           return QHttpServerResponse(StatusCode::InternalServerError);
                       return QHttpServerResponse(StatusCode::Ok);
                   });

    m_server.route(
        "/v1/vaults/<arg>/itemskeys", Method::Get,
        [this](const QString &vaultIDHex, const QHttpServerRequest &request) {
            const QByteArray accountID = authenticate(request);
            const QByteArray vaultID = hexArg(vaultIDHex);
            if (accountID.isEmpty() || !m_store->vaultBelongsTo(vaultID, accountID))
                return errorResponse(StatusCode::Forbidden, QStringLiteral("not your vault"));
            // Not gated by device approval: an authenticated caller has
            // already proved it derived the correct loginKey from the
            // account password, which means it can derive wrapKey too -
            // withholding the wrapped itemsKeys here would not stop it,
            // only require it to take the lockbox path instead (see
            // docs/SYNC.md "Device enrolment with lockboxes"). Approval
            // matters for *that* path, not this one.
            QJsonArray array;
            for (const ServerStore::WrappedItemsKey &k : m_store->itemsKeys(vaultID))
                array.append(
                    QJsonObject{{QStringLiteral("itemsKeyID"), b64(k.itemsKeyID)}, {QStringLiteral("wrapped"), b64(k.wrapped)}});
            return QHttpServerResponse(QJsonObject{{QStringLiteral("itemsKeys"), array}});
        });

    m_server.route("/v1/vaults/<arg>/lockboxes", Method::Post,
                   [this](const QString &vaultIDHex, const QHttpServerRequest &request) {
                       const QByteArray accountID = authenticate(request);
                       const QByteArray vaultID = hexArg(vaultIDHex);
                       if (accountID.isEmpty() || !m_store->vaultBelongsTo(vaultID, accountID))
                           return errorResponse(StatusCode::Forbidden, QStringLiteral("not your vault"));
                       const QJsonObject body = bodyOf(request);
                       m_store->putLockbox(vaultID, unb64(body, "forDevice"), unb64(body, "itemsKeyID"),
                                          unb64(body, "sealed"));
                       return QHttpServerResponse(StatusCode::Ok);
                   });

    m_server.route(
        "/v1/vaults/<arg>/lockboxes", Method::Get,
        [this](const QString &vaultIDHex, const QHttpServerRequest &request) {
            const QByteArray accountID = authenticate(request);
            const QByteArray vaultID = hexArg(vaultIDHex);
            if (accountID.isEmpty() || !m_store->vaultBelongsTo(vaultID, accountID))
                return errorResponse(StatusCode::Forbidden, QStringLiteral("not your vault"));
            const QByteArray forDevice =
                hexArg(QUrlQuery(request.query()).queryItemValue(QStringLiteral("forDevice")));
            QJsonArray array;
            for (const ServerStore::Lockbox &box : m_store->lockboxesFor(vaultID, forDevice))
                array.append(
                    QJsonObject{{QStringLiteral("itemsKeyID"), b64(box.itemsKeyID)}, {QStringLiteral("sealed"), b64(box.sealed)}});
            return QHttpServerResponse(QJsonObject{{QStringLiteral("lockboxes"), array}});
        });
}

} // namespace noteAppC
