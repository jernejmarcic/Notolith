#include "DirWatcher.h"

#ifdef Q_OS_LINUX
#include <cerrno>
#include <sys/inotify.h>
#include <unistd.h>

#include <QSocketNotifier>

namespace noteAppC {

DirWatcher::DirWatcher(QObject *parent)
    : QObject(parent)
{
    m_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (m_fd >= 0) {
        m_notifier = new QSocketNotifier(m_fd, QSocketNotifier::Read, this);
        connect(m_notifier, &QSocketNotifier::activated, this, &DirWatcher::readEvents);
    }
}

DirWatcher::~DirWatcher()
{
    if (m_fd >= 0)
        close(m_fd);
}

QStringList DirWatcher::directories() const
{
    return m_wd.keys();
}

QStringList DirWatcher::addPaths(const QStringList &folders)
{
    QStringList failed;
    for (const QString &folder : folders) {
        if (m_wd.contains(folder))
            continue;
        const int wd = m_fd < 0 ? -1
                                : inotify_add_watch(m_fd, folder.toLocal8Bit().constData(),
                                                    IN_CLOSE_WRITE | IN_MOVED_TO | IN_MOVED_FROM | IN_CREATE | IN_DELETE
                                                        | IN_DELETE_SELF | IN_MOVE_SELF | IN_ONLYDIR);
        if (wd < 0) {
            failed << folder;
            continue;
        }
        m_wd.insert(folder, wd);
        m_folders.insert(wd, folder);
    }
    return failed;
}

void DirWatcher::removePaths(const QStringList &folders)
{
    for (const QString &folder : folders) {
        const int wd = m_wd.take(folder);
        if (wd == 0)
            continue;
        m_folders.remove(wd, folder);
        if (!m_folders.contains(wd)) // another folder (a symlink to it) may still need it
            inotify_rm_watch(m_fd, wd);
    }
}

void DirWatcher::drop(int wd)
{
    for (const QString &folder : m_folders.values(wd))
        m_wd.remove(folder);
    m_folders.remove(wd);
}

void DirWatcher::readEvents()
{
    alignas(inotify_event) char buffer[64 * 1024];
    for (;;) {
        const ssize_t size = read(m_fd, buffer, sizeof buffer);
        if (size <= 0)
            return; // EAGAIN: drained
        for (ssize_t at = 0; at < size;) {
            const auto *event = reinterpret_cast<const inotify_event *>(buffer + at);
            at += ssize_t(sizeof(inotify_event) + event->len);
            if (event->mask & IN_Q_OVERFLOW) {
                emit overflowed();
                continue;
            }
            const QStringList folders = m_folders.values(event->wd);
            if (event->mask & IN_IGNORED) {
                drop(event->wd);
            } else if (event->mask & (IN_DELETE_SELF | IN_MOVE_SELF)) {
                // The watch follows the inode, not the name: a moved folder is
                // watched again under its new name when its parent is read.
                for (const QString &folder : folders)
                    m_wd.remove(folder);
                m_folders.remove(event->wd);
                inotify_rm_watch(m_fd, event->wd);
                for (const QString &folder : folders)
                    emit directoryChanged(folder);
            } else if (event->len > 0 && (event->mask & IN_CLOSE_WRITE) && !(event->mask & IN_ISDIR)) {
                for (const QString &folder : folders)
                    emit fileChanged(folder + QLatin1Char('/') + QString::fromLocal8Bit(event->name));
            } else {
                for (const QString &folder : folders)
                    emit directoryChanged(folder);
            }
        }
    }
}

} // namespace noteAppC

#else // Q_OS_LINUX

namespace noteAppC {

DirWatcher::DirWatcher(QObject *parent)
    : QObject(parent)
{
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, &DirWatcher::directoryChanged);
}

DirWatcher::~DirWatcher() = default;

QStringList DirWatcher::directories() const
{
    return m_watcher.directories();
}

QStringList DirWatcher::addPaths(const QStringList &folders)
{
    return m_watcher.addPaths(folders);
}

void DirWatcher::removePaths(const QStringList &folders)
{
    if (!folders.isEmpty())
        m_watcher.removePaths(folders);
}

} // namespace noteAppC

#endif
