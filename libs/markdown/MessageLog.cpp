// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej Marčič
#include "MessageLog.h"

#include <QFileInfo>
#include <QMutexLocker>

#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace noteAppC {

namespace {

/// Held while a log past its cap is rotated. A byte-range lock on the lock file, one byte past the flock()
/// the processes share the log with (the two kinds do not see each other), so two processes that both
/// found the log full take turns: the second looks again, finds the first one's fresh file, and leaves it.
struct RotationLock {
    int fd;
    explicit RotationLock(int lockFD) : fd(lockFD) { lock(F_WRLCK); }
    ~RotationLock() { lock(F_UNLCK); }
    void lock(short type) const
    {
        if (fd < 0)
            return;
        struct flock request = {};
        request.l_type = type;
        request.l_whence = SEEK_SET;
        request.l_start = 1;
        request.l_len = 1;
        while (::fcntl(fd, F_SETLKW, &request) != 0 && errno == EINTR) {
        }
    }
};

} // namespace

MessageLog::MessageLog(const QString &path, const QString &lockPath, qint64 maxBytes)
    : m_path(path)
    , m_maxBytes(maxBytes)
{
    m_lockFD = ::open(lockPath.toLocal8Bit().constData(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (m_lockFD >= 0) {
        // Exclusive means nobody else has the log open, so this run starts a fresh one. Then shared, for as
        // long as the log is open: the wait is another process finishing its own rotation.
        if (::flock(m_lockFD, LOCK_EX | LOCK_NB) == 0)
            rotate();
        ::flock(m_lockFD, LOCK_SH);
    }
    reopen();
}

MessageLog::~MessageLog()
{
    m_file.close();
    if (m_lockFD >= 0)
        ::close(m_lockFD);
}

void MessageLog::rotate()
{
    const QString old = m_path + QStringLiteral(".old");
    QFile::remove(old);
    QFile::rename(m_path, old);
}

void MessageLog::reopen()
{
    // Created 0600 by open(), never wider for a moment; an older file is narrowed to match.
    const int fd = ::open(m_path.toLocal8Bit().constData(), O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0)
        return;
    ::fchmod(fd, 0600);
    if (!m_file.open(fd, QIODevice::WriteOnly | QIODevice::Append, QFileDevice::AutoCloseHandle))
        ::close(fd);
}

void MessageLog::append(const QString &line)
{
    const QMutexLocker lock(&m_mutex);
    if (!m_file.isOpen())
        return;
    if (m_file.size() > m_maxBytes) {
        m_file.close();
        {
            // The file by its name, looked at again under the lock: another process may have moved it
            // already, and then this one only follows.
            const RotationLock hold(m_lockFD);
            if (QFileInfo(m_path).size() > m_maxBytes)
                rotate();
        }
        reopen();
        if (!m_file.isOpen())
            return;
    }
    m_file.write(line.toUtf8());
    m_file.flush();
}

} // namespace noteAppC
