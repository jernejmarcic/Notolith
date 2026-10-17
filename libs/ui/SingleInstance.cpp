#include "SingleInstance.h"

#include <QLocalSocket>
#include <QStandardPaths>

#include "MainWindow.h"

namespace noteAppC {

QString SingleInstance::defaultName()
{
    const QByteArray override = qgetenv("NOTEAPPC_INSTANCE_SOCKET");
    if (!override.isEmpty())
        return QString::fromLocal8Bit(override);
    return QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation)
           + QStringLiteral("/notolith.sock");
}

bool SingleInstance::handOver(const QString &name, const QString &argument)
{
    QLocalSocket socket;
    socket.connectToServer(name);
    // Nobody listening fails at once (ServerNotFound); the wait only matters
    // for an instance that is there.
    if (!socket.waitForConnected(100))
        return false;
    socket.write(argument.toUtf8().replace('\n', ' ') + '\n');
    if (!socket.waitForBytesWritten(1000))
        return false;
    // Best effort: the line is already queued in the socket, so a busy
    // instance (opening a big vault) still gets it once it is free again.
    socket.waitForReadyRead(1000);
    return true;
}

SingleInstance::SingleInstance(MainWindow *window, const QString &name)
    : QObject(window)
    , m_window(window)
{
    m_server.setSocketOptions(QLocalServer::UserAccessOption);
    connect(&m_server, &QLocalServer::newConnection, this, [this] {
        while (QLocalSocket *socket = m_server.nextPendingConnection()) {
            connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
            connect(socket, &QLocalSocket::readyRead, this, [this, socket] {
                if (!socket->canReadLine())
                    return;
                const QString argument = QString::fromUtf8(socket->readLine()).trimmed();
                socket->write("ok\n");
                socket->disconnectFromServer();
                m_window->openFromOtherInstance(argument);
            });
        }
    });
    // listen() replaces a socket file left by a crash (a live one is
    // never reached: handOver() ran first).
    m_server.listen(name);
}

} // namespace noteAppC
