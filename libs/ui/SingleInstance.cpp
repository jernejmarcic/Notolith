// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej Marčič
#include "SingleInstance.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QLocalSocket>
#include <QStandardPaths>
#include <QThread>

#include <cerrno>
#include <csignal>

#include "ConfigFile.h"
#include "MainWindow.h"
#include "Settings.h"
#include "StorageMigration.h"

namespace noteAppC {

namespace {

std::unique_ptr<QLockFile> lockFor(const QString &name)
{
    if (name.isEmpty())
        return nullptr; // QLockFile(".lock") would be a file in the current directory
    auto lock = std::make_unique<QLockFile>(name + QStringLiteral(".lock"));
    // Only a dead owner makes the lock stale: a window can run for months.
    lock->setStaleLockTime(0);
    return lock;
}

/// The vault the app would reopen with nothing named: the most recent one that is still there.
QString lastUsedVault()
{
    const QString last = Settings::load().recentVaults().value(0);
    return !last.isEmpty() && QFileInfo(last).isDir() ? last : QString();
}

bool deliver(const QString &name, const QString &argument, const QByteArray &token)
{
    if (name.isEmpty())
        return false;
    QLocalSocket socket;
    socket.connectToServer(name);
    // Nobody listening fails at once (ServerNotFound); the wait only
    // matters for an instance that is there.
    if (!socket.waitForConnected(100))
        return false;
    socket.write(token + '\t' + argument.toUtf8().replace('\n', ' ') + '\n');
    if (!socket.waitForBytesWritten(1000))
        return false;
    // Best effort: the line is already queued in the socket, so a
    // busy instance (opening a big vault) still gets it once it is
    // free again.
    socket.waitForReadyRead(1000);
    return true;
}

/// What the sockets of a build from before the `notolith` runtime directory start with, `$XDG_RUNTIME_DIR/notolith`;
/// empty when `$NOTEAPPC_INSTANCE_SOCKET` names them, as it did for that build too. Only this launch's own
/// runtime directory is looked in: a window of that build started from another login (ssh, cron: Qt's
/// `/tmp/runtime-<user>`) lives in a directory this launch cannot name, and its later writes are not seen here.
QString previousBase()
{
    if (!qgetenv("NOTEAPPC_INSTANCE_SOCKET").isEmpty())
        return {};
    const QString runtime = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    return QDir::isAbsolutePath(runtime) ? QDir::cleanPath(runtime) + QStringLiteral("/notolith") : QString();
}

/// Whether a live process holds `lockFile`. A lock that cannot be taken is a live window's, except when its
/// owner is gone and the lock could not be cleared away either (a full or read-only runtime directory), which
/// the error code does not tell from the first.
bool heldByLiveProcess(const QString &lockFile)
{
    // Those builds took the lock for as long as a window ran, and only a dead owner makes it stale.
    QLockFile lock(lockFile);
    lock.setStaleLockTime(0);
    if (lock.tryLock(0))
        return false;
    if (lock.error() != QLockFile::LockFailedError)
        return false;
    qint64 pid = 0;
    QString host, application;
    if (!lock.getLockInfo(&pid, &host, &application))
        return true; // being written right now: its owner is there
    return pid > 0 && (::kill(static_cast<pid_t>(pid), 0) == 0 || errno == EPERM);
}

/// The sockets that start with `base` and whose lock a live process holds.
QStringList openSockets(const QString &base)
{
    if (base.isEmpty())
        return {};
    const QFileInfo info(base);
    const QDir directory(info.absolutePath());
    QStringList open;
    for (const QString &file : directory.entryList({info.fileName() + QStringLiteral("-*.sock.lock")}, QDir::Files)) {
        if (heldByLiveProcess(directory.filePath(file)))
            open.append(directory.filePath(file).chopped(QStringLiteral(".lock").size()));
    }
    return open;
}

} // namespace

QString SingleInstance::baseName()
{
    const QByteArray override = qgetenv("NOTEAPPC_INSTANCE_SOCKET");
    if (!override.isEmpty())
        return QString::fromLocal8Bit(override);
    // $XDG_RUNTIME_DIR/notolith/instance-<id>.sock. The directory has to be this user's own and closed to
    // everyone else, or the sockets and locks in it are not; without one, they go in the state directory under
    // $HOME. Never a predictable name in /tmp, where another user could have made it first.
    for (const QString &directory : {appDir(XDGroot::Runtime), appDir(XDGroot::State)}) {
        if (ensurePrivateDir(directory) && isOwnPrivateDir(directory))
            return directory + QStringLiteral("/instance");
    }
    return {}; // no usable place: the launch is not coordinated with another
}

QString SingleInstance::nameForVault(const QString &vaultRoot)
{
    const QString base = baseName();
    if (base.isEmpty())
        return {};
    QString id = QStringLiteral("none");
    if (!vaultRoot.isEmpty()) {
        const QFileInfo info(vaultRoot);
        // canonicalFilePath() is empty for a folder that is not there: the cleaned path stands in.
        QString key = info.canonicalFilePath();
        if (key.isEmpty())
            key = QDir::cleanPath(info.absoluteFilePath());
        id = QString::fromLatin1(QCryptographicHash::hash(key.toUtf8(), QCryptographicHash::Sha256).toHex().left(16));
    }
    return base + QLatin1Char('-') + id + QStringLiteral(".sock");
}

SingleInstance::Launch SingleInstance::launchFor(const QString &argument)
{
    const LaunchTarget target = resolveLaunchTarget(argument);
    if (target.mode == OpenMode::Vault)
        return {nameForVault(target.vaultRoot), {}};
    const QString last = lastUsedVault();
    if (target.mode == OpenMode::Empty && !last.isEmpty())
        return {nameForVault(last), {}};
    // A loose note, or nothing and nothing to reopen: no vault of its own. The last-used vault's
    // window takes a loose note as a tab, as the one window always did, when it is running.
    return {nameForVault(QString()), last.isEmpty() ? QStringList() : QStringList{nameForVault(last)}};
}

SingleInstance::SingleInstance(const QString &name, const QStringList &alsoTry)
    : m_name(name)
    , m_alsoTry(alsoTry)
    , m_lock(lockFor(name))
{
}

SingleInstance::SingleInstance(const Launch &launch)
    : SingleInstance(launch.name, launch.alsoTry)
{
}

bool SingleInstance::deliverTo(const QString &name, const QString &argument, const QByteArray &token)
{
    return deliver(name, argument, token);
}

QStringList SingleInstance::previousBuildSockets()
{
    return openSockets(previousBase());
}

QStringList SingleInstance::ownSockets()
{
    return openSockets(baseName());
}

bool SingleInstance::handOverToPreviousBuild(const Launch &launch, const QString &argument, const QByteArray &token)
{
    const QStringList open = previousBuildSockets();
    if (open.isEmpty())
        return false;
    const QString base = baseName();
    QStringList candidates;
    for (const QString &name : QStringList{launch.name} + launch.alsoTry) {
        // The same name in the old place: only the part before the vault's id differs.
        if (!base.isEmpty() && name.startsWith(base))
            candidates.append(previousBase() + name.mid(base.size()));
    }
    // With the settings not moved yet the vault of a bare launch is not known: any open window will do.
    if (launch.name.endsWith(QLatin1String("-none.sock")))
        candidates += open;
    for (const QString &candidate : std::as_const(candidates)) {
        if (open.contains(candidate) && deliver(candidate, argument, token))
            return true;
    }
    return false;
}

SingleInstance::Outcome SingleInstance::reach(const QString &name, QLockFile &lock, const QString &argument,
                                              const QByteArray &token, int attempts, bool deliver)
{
    // Whoever holds the lock is running or about to listen, so keep trying
    // for as long as a window takes to come up; with the lock ours, nobody is.
    for (int attempt = 0; attempt < attempts; ++attempt) {
        if (deliver && deliverTo(name, argument, token))
            return Outcome::Delivered;
        if (lock.tryLock(0))
            return Outcome::Locked;
        if (attempt + 1 < attempts)
            QThread::msleep(50);
    }
    return Outcome::Stuck;
}

bool SingleInstance::handOver(const QString &argument, const QByteArray &token)
{
    for (const QString &other : std::as_const(m_alsoTry)) {
        if (deliverTo(other, argument, token))
            return true;
    }
    if (!m_lock)
        return false; // no place for a socket: start, unserved
    // A lock holder that never listens is stuck: start anyway.
    return reach(m_name, *m_lock, argument, token, 40, true) == Outcome::Delivered;
}

bool SingleInstance::moveTo(const QString &name, const QString &argument, const QByteArray &token)
{
    if (name == m_name)
        return true;
    if (name.isEmpty()) {
        m_server.close();
        m_lock.reset();
        m_name.clear();
        return true;
    }
    const bool handingOver = !argument.isNull();
    auto lock = lockFor(name);
    switch (reach(name, *lock, argument, token, handingOver ? 40 : 1, handingOver)) {
    case Outcome::Delivered:
        return false;
    case Outcome::Locked:
        // The new lock is held before the old name goes, so a launch for either finds an owner or
        // waits for one; the old socket closes, then the new one listens.
        m_server.close();
        m_lock = std::move(lock);
        m_name = name;
        if (m_window)
            listen();
        return true;
    case Outcome::Stuck:
        // Not ours to take: the old name goes, and the window carries on unserved.
        m_server.close();
        m_lock.reset();
        m_name.clear();
        return true;
    }
    return true;
}

void SingleInstance::serve(MainWindow *window)
{
    m_window = window;
    m_server.setSocketOptions(QLocalServer::UserAccessOption);
    connect(&m_server, &QLocalServer::newConnection, this, [this] {
        while (QLocalSocket *socket = m_server.nextPendingConnection()) {
            connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
            connect(socket, &QLocalSocket::readyRead, this, [this, socket] {
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
                m_window->openFromOtherInstance(QString::fromUtf8(line.mid(tab + 1)));
            });
        }
    });
    // We hold the lock, so a socket file here is a crash's leftover and
    // listen() replaces it.
    if (!m_name.isEmpty())
        listen();
    else
        qWarning("Notolith: no private directory for the single-instance sockets (the runtime directory and "
                 "the state directory are not this user's own and closed to others): a launch for this vault "
                 "will start another process");
}

void SingleInstance::listen()
{
    // The lock is ours, so a socket file here is a crash's leftover; it outlives a crash in the state directory.
    QLocalServer::removeServer(m_name);
    if (m_server.listen(m_name))
        return;
    // Unserved but holding the lock, every later launch for this vault would wait two seconds and
    // then start a second process on it: let the name go instead and say why.
    qWarning("Notolith: cannot listen on %s: %s", qPrintable(m_name), qPrintable(m_server.errorString()));
    m_lock.reset();
    m_name.clear();
}

StorageSettled settleStorage(const QString &argument, QStringList *lines, QStringList *warnings)
{
    if (storageMigrationNeed() == MigrationNeed::None)
        return StorageSettled::Carry;
    // Nothing moves under a window that is open: it keeps its files in memory and writes them back, so
    // what was moved would be overwritten by its next save, or left behind it. The launch goes to it when
    // it is the one for this vault. Settings are read only when there is a window to find (the files are not
    // moved yet).
    if (!SingleInstance::ownSockets().isEmpty()) {
        const SingleInstance::Launch launch = SingleInstance::launchFor(argument);
        if (SingleInstance(launch).handOver(argument))
            return StorageSettled::HandedOver;
        // Said, or an always-open window would put this off for ever without a word.
        const QString line = QStringLiteral("storage: files of the previous version are waiting; they move at a "
                                            "start with no Notolith window open");
        if (warnings)
            warnings->append(line);
        else
            qWarning().noquote() << line;
        return StorageSettled::Carry;
    }
    if (!SingleInstance::previousBuildSockets().isEmpty()) {
        if (SingleInstance::handOverToPreviousBuild(SingleInstance::launchFor(argument), argument))
            return StorageSettled::HandedOver;
        return StorageSettled::OlderWindowOpen;
    }
    migrateStorage(lines, warnings);
    return StorageSettled::Carry;
}

} // namespace noteAppC
