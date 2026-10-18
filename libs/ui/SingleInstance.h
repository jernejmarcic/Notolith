#pragma once

#include <QByteArray>
#include <QLocalServer>
#include <QLockFile>
#include <QObject>
#include <QString>

namespace noteAppC {

class MainWindow;

/// One running Notolith per user. A second launch - a note opened from the
/// file manager - hands what it was asked to open to the first and exits,
/// the way Obsidian does, instead of starting a second indexer, sync engine
/// and autosaver on the same files.
///
/// The first instance listens on a local socket in the user's runtime
/// directory; the protocol is one line, "token<TAB>path" (path empty for "just
/// show yourself"), answered by one line, "ok".
class SingleInstance : public QObject {
    Q_OBJECT

public:
    /// `$NOTEAPPC_INSTANCE_SOCKET` when set (benchmarks and tests that must
    /// not reach a Notolith someone is using), else `notolith.sock` in the
    /// runtime directory.
    static QString defaultName();

    explicit SingleInstance(const QString &name);

    /// Gives `argument` to the instance on this name. True when one took it.
    /// False means this process is the first: it holds the lock that keeps
    /// two launches in the same moment from both becoming "first", and the
    /// caller starts up and calls serve(). The common case costs a failed
    /// connect() and one lock file. `token` is the launcher's
    /// XDG_ACTIVATION_TOKEN, which lets the other window take focus.
    bool handOver(const QString &argument, const QByteArray &token = qgetenv("XDG_ACTIVATION_TOKEN"));

    /// Serves `window`: from here on, what other launches hand over opens in
    /// it. Replaces a socket file left by a crash.
    void serve(MainWindow *window);

    bool ok() const { return m_server.isListening(); }

private:
    QString m_name;
    QLockFile m_lock;
    QLocalServer m_server;
};

} // namespace noteAppC
