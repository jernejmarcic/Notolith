#pragma once

#include <QLocalServer>
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
/// directory; the protocol is one line, the absolute path to open (empty for
/// "just show yourself"), answered by one line, "ok".
class SingleInstance : public QObject {
    Q_OBJECT

public:
    /// `$NOTEAPPC_INSTANCE_SOCKET` when set (benchmarks and tests that must
    /// not reach a Notolith someone is using), else `notolith.sock` in the
    /// runtime directory.
    static QString defaultName();

    /// Gives `argument` to the instance listening on `name`. False when none
    /// is - the common case, which costs one failed connect() - and the
    /// caller starts up as usual.
    static bool handOver(const QString &name, const QString &argument);

    /// Serves `window` on `name`, replacing a socket file left by a crash.
    SingleInstance(MainWindow *window, const QString &name);

    bool ok() const { return m_server.isListening(); }

private:
    MainWindow *m_window;
    QLocalServer m_server;
};

} // namespace noteAppC
