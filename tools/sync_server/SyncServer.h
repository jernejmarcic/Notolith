#pragma once

#include <QHttpServer>
#include <QObject>
#include <QString>

#include "ServerStore.h"

namespace noteAppC {

/// noteAppC-sync-server: the HTTP side of docs/SYNC.md. Every route is
/// documented there under "The journal protocol", "Device enrolment with
/// lockboxes" and "Password change" - this class is the routing table for
/// that document, ServerStore does the actual persisting. The server
/// never has a key: it stores and relays ciphertext and never inspects it.
class SyncServer : public QObject {
    Q_OBJECT

public:
    explicit SyncServer(ServerStore *store, QObject *parent = nullptr);

    /// Binds to `host:port`; `port` 0 lets the OS pick one (what the tests
    /// use - see actualPort()). `certFile`/`keyFile` empty means plain
    /// HTTP, for loopback tests only - docs/SYNC.md documents TLS as the
    /// expectation for anything that leaves loopback.
    bool listen(const QString &host, quint16 port, const QString &certFile = {}, const QString &keyFile = {});
    quint16 actualPort() const;

private:
    void registerRoutes();
    /// Parses "Bearer <hex token>" from the request and resolves it to an
    /// account, or an empty QByteArray if missing/invalid/expired.
    QByteArray authenticate(const QHttpServerRequest &request) const;

    QHttpServer m_server;
    ServerStore *m_store;
};

} // namespace noteAppC
