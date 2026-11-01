#pragma once

#include <QByteArray>
#include <QLocalServer>
#include <QLockFile>
#include <QObject>
#include <QString>
#include <QStringList>

#include <memory>

namespace noteAppC {

class MainWindow;

/// One running Notolith per vault. A second launch for a vault that is open -
/// a note opened from the file manager - hands what it was asked to open to
/// the window that has it and exits, the way Obsidian does, instead of
/// starting a second indexer, sync engine and autosaver on the same files. A
/// launch for a vault nobody has open starts a process of its own, so two
/// vaults are two windows, two processes.
///
/// Each process listens on a local socket named after its vault (nameForVault)
/// in the user's runtime directory; the protocol is one line,
/// "token<TAB>path" (path empty for "just show yourself"), answered by one
/// line, "ok". A window that opens another vault stops serving the name of the
/// one it left and serves the new one (moveTo).
///
/// A launch with no vault - a bare `notolith`, or a note outside every vault -
/// goes to the instance of the vault the app would reopen (the most recent
/// in the settings) when that runs, so the loose note opens beside it as before;
/// else to the one process that has no vault; else a process starts, which
/// serves the no-vault name until its window opens a vault.
class SingleInstance : public QObject {
    Q_OBJECT

public:
    /// Whom a launch asks, and what it becomes when nobody answers.
    struct Launch {
        QString name;        ///< the instance this launch is the first of: its socket
        QStringList alsoTry; ///< instances that take the argument before it starts, in order
    };

    /// `$NOTEAPPC_INSTANCE_SOCKET` when set (benchmarks and tests that must
    /// not reach a Notolith someone is using), else `instance` in
    /// `$XDG_RUNTIME_DIR/notolith` (made 0700; used only while it is this user's own and closed to others,
    /// else the state directory takes its place): the part every per-vault socket name starts with. Empty when
    /// no private directory can be had, and then the names are empty too and nothing is coordinated. The
    /// override is a prefix, not a socket - a directory plus the start of a
    /// file name, `/tmp/run/bench`, gives `/tmp/run/bench-<id>.sock` - so an
    /// isolated run keeps every vault's name apart from the real ones too.
    static QString baseName();

    /// `<baseName>-<16 hex digits of a SHA-256 of the vault's canonical
    /// path>.sock`: the same vault however it is spelled (trailing slash,
    /// `..`, a symbolic link) has the same name. An empty root is the name of
    /// the process that has no vault, `<baseName>-none.sock`. The lock beside
    /// it is the same name plus `.lock`.
    static QString nameForVault(const QString &vaultRoot);

    /// Works out the vault `argument` will end in, the way the window will:
    /// its vault when it names one (resolveLaunchTarget), with nothing named
    /// the vault the app would reopen. Reads the settings once.
    static Launch launchFor(const QString &argument);

    /// The sockets of windows of a build from before the `notolith` runtime directory
    /// (`$XDG_RUNTIME_DIR/notolith-<id>.sock`) that are open: those whose lock is held, which only a
    /// live owner does. Empty when `$NOTEAPPC_INSTANCE_SOCKET` is set.
    static QStringList previousBuildSockets();

    /// Gives `argument` to the window of such a build that holds what `launch` names, the way handOver()
    /// would to one of this build; a launch whose vault is not known yet goes to any of them. Its wire
    /// protocol is the same. True when one took it.
    static bool handOverToPreviousBuild(const Launch &launch, const QString &argument,
                                        const QByteArray &token = qgetenv("XDG_ACTIVATION_TOKEN"));

    explicit SingleInstance(const QString &name, const QStringList &alsoTry = {});
    explicit SingleInstance(const Launch &launch);

    /// Gives `argument` to the instance this launch belongs to. True when one
    /// took it. False means this process is the first: it holds the lock that
    /// keeps two launches in the same moment from both becoming "first", and
    /// the caller starts up and calls serve(). The common case costs a failed
    /// connect() and one lock file. `token` is the launcher's
    /// XDG_ACTIVATION_TOKEN, which lets the other window take focus.
    bool handOver(const QString &argument, const QByteArray &token = qgetenv("XDG_ACTIVATION_TOKEN"));

    /// Gives `argument` to whoever is listening on `name` right now, once,
    /// without waiting for a process that is still starting. True when taken.
    bool deliverTo(const QString &name, const QString &argument, const QByteArray &token = {});

    /// Serves `window`: from here on, what other launches hand over opens in
    /// it. Replaces a socket file left by a crash.
    void serve(MainWindow *window);

    /// The window now has another vault: serve `name` instead of the old one.
    /// With an `argument` (not null), a process that already has `name` -
    /// running, or starting and holding its lock - is given the argument and
    /// the move is refused (false): the caller leaves the window as it was.
    /// With a null argument, as when a window loses its vault, `name` is taken
    /// if it is free; if another window holds it (it has no vault either) this
    /// one serves nothing - both are plain windows, and the next launch for
    /// "no vault" reaches the first. True otherwise, also when a lock holder
    /// never listens: the vault then opens without being served, as a launch
    /// starts anyway.
    bool moveTo(const QString &name, const QString &argument = QString(), const QByteArray &token = {});

    bool ok() const { return m_server.isListening(); }
    /// The socket this instance holds or serves; empty when it has none.
    QString name() const { return m_name; }

private:
    /// Listens on m_name; when that fails the lock and the name are given up (and the reason logged).
    void listen();
    enum class Outcome { Delivered, Locked, Stuck };
    /// Delivers `argument` to `name` or takes `lock`, for up to `attempts` tries 50 ms apart.
    Outcome reach(const QString &name, QLockFile &lock, const QString &argument, const QByteArray &token, int attempts,
                  bool deliver);

    QString m_name;
    QStringList m_alsoTry;
    std::unique_ptr<QLockFile> m_lock;
    QLocalServer m_server{this};
    MainWindow *m_window = nullptr;
};

} // namespace noteAppC
