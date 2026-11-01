#include "MessageLog.h"

#include <QFileInfo>
#include <QMutexLocker>

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

namespace noteAppC {

MessageLog::MessageLog(const QString &path, const QString &lockPath, qint64 maxBytes)
    : m_maxBytes(maxBytes)
{
    m_file.setFileName(path);
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
    const QString old = m_file.fileName() + QStringLiteral(".old");
    QFile::remove(old);
    QFile::rename(m_file.fileName(), old);
}

void MessageLog::reopen()
{
    if (m_file.open(QIODevice::WriteOnly | QIODevice::Append))
        m_file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
}

void MessageLog::append(const QString &line)
{
    const QMutexLocker lock(&m_mutex);
    if (!m_file.isOpen())
        return;
    if (m_file.size() > m_maxBytes) {
        m_file.close();
        // The file by its name: another process may have moved it already, and then this one only follows.
        if (QFileInfo(m_file.fileName()).size() > m_maxBytes)
            rotate();
        reopen();
        if (!m_file.isOpen())
            return;
    }
    m_file.write(line.toUtf8());
    m_file.flush();
}

} // namespace noteAppC
