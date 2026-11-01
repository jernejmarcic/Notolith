// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej
#pragma once

#include <QFileSystemWatcher>
#include <QHash>
#include <QMultiHash>
#include <QObject>
#include <QStringList>

class QSocketNotifier;

namespace noteAppC {

/// One watch per folder, as the Indexer needs. On Linux it is inotify read
/// directly (no thread, no timer: the fd is read only when the kernel has
/// something), because a folder watch from QFileSystemWatcher says nothing
/// about a file written in place by another program. Elsewhere it wraps
/// QFileSystemWatcher, which reports only directoryChanged().
class DirWatcher : public QObject {
    Q_OBJECT

public:
    explicit DirWatcher(QObject *parent = nullptr);
    ~DirWatcher() override;

    QStringList directories() const;
    /// Watches each folder; returns the ones that could not be (the
    /// inotify watch limit, no permission, gone).
    QStringList addPaths(const QStringList &folders);
    void removePaths(const QStringList &folders);

signals:
    /// An entry of `folder` appeared, went or was renamed; or the folder itself went.
    void directoryChanged(const QString &folder);
    /// A file in a watched folder was written and closed (Linux only).
    void fileChanged(const QString &path);
    /// Events were lost (the kernel's queue overflowed): everything may have changed.
    void overflowed();

private:
#ifdef Q_OS_LINUX
    void readEvents();
    void drop(int wd);
    int m_fd = -1;
    QSocketNotifier *m_notifier = nullptr;
    QHash<QString, int> m_wd;           ///< folder -> its watch
    QMultiHash<int, QString> m_folders; ///< watch -> folders (symlinks and moved folders share one)
#else
    QFileSystemWatcher m_watcher;
#endif
};

} // namespace noteAppC
