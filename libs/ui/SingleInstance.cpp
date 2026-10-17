#include "SingleInstance.h"

#include <QLocalSocket>
#include <QStandardPaths>
#include <QThread>

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

SingleInstance::SingleInstance(const QString &name)
    : m_name(name)
    , m_lock(name + QStringLiteral(".lock"))
{
    // Only a dead owner makes the lock stale: a window can run for months.
    m_lock.setStaleLockTime(0);
}

bool SingleInstance::handOver(const QString &argument, const QByteArray &token)
{
    // Whoever holds the lock is running or about to listen, so keep trying
    // for as long as a window takes to come up; with the lock ours, nobody is.
    for (int attempt = 0; attempt < 40; ++attempt) {
        QLocalSocket socket;
        socket.connectToServer(m_name);
        // Nobody listening fails at once (ServerNotFound); the wait only
        // matters for an instance that is there.
        if (socket.waitForConnected(100)) {
            socket.write(token + '\t' + argument.toUtf8().replace('\n', ' ') + '\n');
            if (!socket.waitForBytesWritten(1000))
                return false;
            // Best effort: the line is already queued in the socket, so a
            // busy instance (opening a big vault) still gets it once it is
            // free again.
            socket.waitForReadyRead(1000);
            return true;
        }
        if (m_lock.tryLock(0))
            return false;
        QThread::msleep(50);
    }
    return false; // a lock holder that never listens is stuck: start anyway
}

void SingleInstance::serve(MainWindow *window)
{
    m_server.setSocketOptions(QLocalServer::UserAccessOption);
    connect(&m_server, &QLocalServer::newConnection, this, [this, window] {
        while (QLocalSocket *socket = m_server.nextPendingConnection()) {
            connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
            connect(socket, &QLocalSocket::readyRead, this, [window, socket] {
                if (!socket->canReadLine())
                    return;
                QByteArray line = socket->readLine();
                line.chop(1); // the newline; a trim would also eat the tab of an empty path
                socket->write("ok\n");
                socket->disconnectFromServer();
                const qsizetype tab = line.indexOf('\t');
                // Qt's Wayland plugin reads this when the window asks for
                // focus, which a compositor only grants with the token.
                if (tab > 0)
                    qputenv("XDG_ACTIVATION_TOKEN", line.left(tab));
                window->openFromOtherInstance(QString::fromUtf8(line.mid(tab + 1)));
            });
        }
    });
    // We hold the lock, so a socket file here is a crash's leftover and
    // listen() replaces it.
    m_server.listen(m_name);
}

} // namespace noteAppC
