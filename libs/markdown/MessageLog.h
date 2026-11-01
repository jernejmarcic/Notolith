#pragma once

#include <QFile>
#include <QMutex>
#include <QString>

namespace noteAppC {

/// The application's log file: lines appended to `path`, the previous run's
/// file kept as `path.old` (one generation), and past `maxBytes` the running
/// file moved to `.old` the same way, so the newest lines always survive.
///
/// Every vault is its own process and they share the one file. A process
/// holds a shared lock on `lockPath` for as long as it has the log open, and
/// rotates at start only when it can take that lock exclusively - when nobody
/// else is writing - so a second window never takes the first one's log away.
class MessageLog {
public:
    MessageLog(const QString &path, const QString &lockPath, qint64 maxBytes = 5 << 20);
    ~MessageLog();
    MessageLog(const MessageLog &) = delete;
    MessageLog &operator=(const MessageLog &) = delete;

    bool isOpen() const { return m_file.isOpen(); }
    /// Appends `line` (which carries its own newline) and flushes it: a crash is what the log is for.
    /// Callable from any thread.
    void append(const QString &line);

private:
    void rotate();
    void reopen();

    QMutex m_mutex;
    QString m_path;
    QFile m_file;
    qint64 m_maxBytes;
    int m_lockFD = -1;
};

} // namespace noteAppC
