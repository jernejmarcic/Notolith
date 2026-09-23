#include "PluginHost.h"

#include <noteappc/plugin.h>

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QProcess>
#include <QStandardPaths>
#include <QTimer>

#include <sys/resource.h>

namespace noteAppC {

namespace {

// Budgets from docs/PLUGINS.md: a command over this is flagged slow, a
// render over this is flagged slow. Neither kills anything by itself - only
// setRequestTimeoutMsForTesting()'s (or the production default's) hard
// timeout does that.
constexpr qint64 kSlowCommandMs = 100;
constexpr qint64 kSlowRenderMs = 1000;

// A 256 MiB address-space cap and a 64 open-file cap: generous for a
// command that counts words or renders a small image, tight enough that a
// runaway plugin cannot pressure the machine noteAppC runs on - the same
// shape as TexCompiler's rlimits (libs/render/TexCompiler.cpp), just a
// smaller ceiling because these are meant to be small out-of-process
// extras, not a full TeX engine.
constexpr rlim_t kMemoryLimitBytes = 256ULL * 1024 * 1024;
constexpr rlim_t kOpenFileLimit = 64;

QString permissionLabel(PluginPermission permission)
{
    return PluginManifest::permissionID(permission);
}

} // namespace

PluginHost::PluginHost(PluginManifest manifest, QSet<PluginPermission> grantedPermissions,
                       PluginAppCallbacks callbacks, QObject *parent)
    : QObject(parent)
    , m_manifest(std::move(manifest))
    , m_granted(std::move(grantedPermissions))
    , m_callbacks(std::move(callbacks))
{
}

PluginHost::~PluginHost()
{
    stop();
}

bool PluginHost::permissionGranted(PluginPermission permission) const
{
    return m_granted.contains(permission);
}

void PluginHost::setState(State state)
{
    if (m_state == state)
        return;
    m_state = state;
    emit stateChanged(state);
}

QString PluginHost::hostBinaryPath()
{
    // A test build points this straight at the binary ctest just built,
    // since the host tool and the test executable do not sit in sibling
    // directories the way an installed app and an installed host tool do.
    if (const QString overridden = qEnvironmentVariable("NOTEAPPC_PLUGIN_HOST"); !overridden.isEmpty())
        return overridden;
    const QString sibling =
        QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("noteAppC-plugin-host"));
    if (QFileInfo::exists(sibling))
        return sibling;
    return QStandardPaths::findExecutable(QStringLiteral("noteAppC-plugin-host"));
}

QStringList PluginHost::sandboxArguments(QString *program) const
{
    const QString hostBinary = hostBinaryPath();
    const QString pluginDir = m_manifest.directory();
    const QString bwrap = QStandardPaths::findExecutable(QStringLiteral("bwrap"));

    if (bwrap.isEmpty()) {
        // No bubblewrap on this system: run the host directly. The rlimits
        // in the child-process modifier still apply either way; only the
        // filesystem/namespace isolation is lost. TexCompiler falls back
        // the same way.
        *program = hostBinary;
        return {QStringLiteral("--plugin"), pluginDir};
    }

    // An installed noteAppC-plugin-host lives under /usr and is already
    // covered by the /usr bind below; a build-tree one (every dev/test run,
    // since HANDOFF.md requires building under /tmp) does not, so its own
    // directory is bound read-only too - the same reasoning as the plugin's
    // own directory, and for the same reason it must come *after* the tmpfs
    // mount on /tmp: bwrap applies mounts in argument order, and a bind
    // placed before `--tmpfs /tmp` is a path under /tmp, so it gets buried
    // by the tmpfs bwrap mounts on top of it right afterwards. This is not
    // a theoretical ordering nit - it is exactly what happens to every test
    // and dev run in this repo, and the first version of this function got
    // it backwards.
    const QString hostBinaryDir = QFileInfo(hostBinary).absolutePath();

    *program = bwrap;
    QStringList args = {
        QStringLiteral("--ro-bind"), QStringLiteral("/usr"), QStringLiteral("/usr"),
        QStringLiteral("--ro-bind"), QStringLiteral("/etc"), QStringLiteral("/etc"),
        QStringLiteral("--symlink"), QStringLiteral("usr/bin"), QStringLiteral("/bin"),
        QStringLiteral("--symlink"), QStringLiteral("usr/lib"), QStringLiteral("/lib"),
        QStringLiteral("--symlink"), QStringLiteral("usr/lib"), QStringLiteral("/lib64"),
        QStringLiteral("--symlink"), QStringLiteral("usr/bin"), QStringLiteral("/sbin"),
        QStringLiteral("--proc"), QStringLiteral("/proc"),
        QStringLiteral("--dev"), QStringLiteral("/dev"),
        QStringLiteral("--tmpfs"), QStringLiteral("/tmp"),
        // Read-only: a plugin's own directory is data (its library, its
        // plugin.ini, any assets it shipped), never a place it writes.
        QStringLiteral("--ro-bind"), pluginDir, pluginDir,
        QStringLiteral("--ro-bind"), hostBinaryDir, hostBinaryDir,
        QStringLiteral("--unshare-user-try"), QStringLiteral("--unshare-ipc"),
        QStringLiteral("--unshare-pid"), QStringLiteral("--unshare-uts"),
        QStringLiteral("--unshare-cgroup-try"),
    };
    // The one namespace TexCompiler always drops and this does not: shared
    // only when the user granted the `network` capability to this plugin.
    if (!permissionGranted(PluginPermission::Network))
        args << QStringLiteral("--unshare-net");
    args << QStringLiteral("--clearenv") << QStringLiteral("--setenv") << QStringLiteral("PATH")
        << QStringLiteral("/usr/bin:/bin")
        // No home is bound, so nothing needs one - but some libraries a
        // plugin links (locale data, font config) fall over with $HOME
        // unset rather than just missing a directory, so it points at the
        // sandbox's own private, empty tmpfs instead of leaking the real one.
        << QStringLiteral("--setenv") << QStringLiteral("HOME") << QStringLiteral("/tmp")
        << QStringLiteral("--die-with-parent") << QStringLiteral("--new-session")
        << QStringLiteral("--chdir") << pluginDir << QStringLiteral("--") << hostBinary
        << QStringLiteral("--plugin") << pluginDir;
    return args;
}

void PluginHost::ensureStarted()
{
    if (m_state == State::Running || m_state == State::Starting)
        return;
    if (m_hasFailed) {
        const qint64 backoffMs = qMin<qint64>(30000, 500LL << qMin(m_timing.consecutiveCrashes, 6));
        if (m_sinceFailure.elapsed() < backoffMs)
            return; // still backing off - caller's request fails below, no process to write to
    }

    m_process = std::make_unique<QProcess>();
    m_reader = PluginFrameReader();
    QString program;
    const QStringList arguments = sandboxArguments(&program);
    m_process->setProgram(program);
    m_process->setArguments(arguments);
    // Between fork and exec, so it applies whether or not bwrap is in the
    // chain (rlimits survive execve) - see TexCompiler::compile for the
    // pattern this copies.
    m_process->setChildProcessModifier([] {
        rlimit memory{kMemoryLimitBytes, kMemoryLimitBytes};
        setrlimit(RLIMIT_AS, &memory);
        rlimit files{kOpenFileLimit, kOpenFileLimit};
        setrlimit(RLIMIT_NOFILE, &files);
    });

    connect(m_process.get(), &QProcess::readyReadStandardOutput, this, &PluginHost::handleReadyRead);
    connect(m_process.get(), &QProcess::finished, this,
           [this](int exitCode, QProcess::ExitStatus exitStatus) {
               handleFinished(exitCode, static_cast<int>(exitStatus));
           });
    // Never let the plugin's own stderr (its crash messages, a leaked
    // fprintf) reach anything but the log - it must never be mistaken for
    // protocol bytes, which only ever travel over stdout.
    connect(m_process.get(), &QProcess::readyReadStandardError, this, [this] {
        const QString text = QString::fromUtf8(m_process->readAllStandardError()).trimmed();
        if (!text.isEmpty() && m_callbacks.log)
            m_callbacks.log(m_manifest.id(), text);
    });

    setState(State::Starting);
    QElapsedTimer startClock;
    startClock.start();
    m_process->start();

    QJsonArray permissions;
    for (const PluginPermission permission : m_granted)
        permissions.append(permissionLabel(permission));
    QJsonObject params;
    params.insert(QStringLiteral("apiVersion"), NOTEAPPC_PLUGIN_API);
    params.insert(QStringLiteral("permissions"), permissions);
    sendRequest(QStringLiteral("init"), params,
               [this, startClock](bool ok, const QJsonObject &, const QString &error) mutable {
                   if (!ok) {
                       if (m_callbacks.log)
                           m_callbacks.log(m_manifest.id(), QStringLiteral("init failed: ") + error);
                       return; // handleFinished (the process exits on a failed init) does the rest
                   }
                   m_timing.lastStartMs = startClock.elapsed();
                   m_timing.consecutiveCrashes = 0;
                   m_hasFailed = false;
                   setState(State::Running);
               });
}

void PluginHost::stop()
{
    if (!m_process)
        return;
    if (m_process->state() != QProcess::NotRunning) {
        sendMessage(PluginMessage::notification(QStringLiteral("shutdown")));
        m_process->waitForFinished(500);
        if (m_process->state() != QProcess::NotRunning) {
            m_process->kill();
            m_process->waitForFinished(500);
        }
    }
    failAllPending(QStringLiteral("plugin host stopped"));
    m_process.reset();
    setState(State::Stopped);
}

qint64 PluginHost::sendRequest(const QString &method, const QJsonObject &params, const ResultCallback &callback)
{
    const qint64 id = m_nextRequestID++;
    Pending pending;
    pending.callback = callback;
    pending.method = method;
    pending.clock.start();
    pending.timer = new QTimer(this);
    pending.timer->setSingleShot(true);
    connect(pending.timer, &QTimer::timeout, this, [this, id] {
        const auto it = m_pending.find(id);
        if (it == m_pending.end())
            return;
        const ResultCallback callback = it->callback;
        const QString method = it->method;
        QTimer *timer = it->timer;
        m_pending.erase(it);
        timer->deleteLater();
        if (callback)
            callback(false, {}, QStringLiteral("%1 timed out").arg(method));
        // A hung plugin never answers on its own; killing here is what
        // "a host that hangs is killed after the timeout and restarted on
        // next use" (docs/PLUGINS.md) means in code. handleFinished() does
        // the backoff bookkeeping once the kill lands.
        if (m_process)
            m_process->kill();
    });
    pending.timer->start(m_requestTimeoutMs);
    m_pending.insert(id, pending);
    sendMessage(PluginMessage::request(id, method, params));
    return id;
}

void PluginHost::runCommand(const QString &commandID, const ResultCallback &callback)
{
    ensureStarted();
    if (!m_process) {
        if (callback)
            callback(false, {}, QStringLiteral("plugin host is restarting, try again shortly"));
        return;
    }
    QJsonObject params;
    params.insert(QStringLiteral("commandID"), commandID);
    sendRequest(QStringLiteral("command.run"), params, callback);
}

void PluginHost::renderCodeBlock(const QString &language, const QString &source, int widthPx,
                                 double devicePixelRatio, const QString &theme,
                                 const ResultCallback &callback)
{
    ensureStarted();
    if (!m_process) {
        if (callback)
            callback(false, {}, QStringLiteral("plugin host is restarting, try again shortly"));
        return;
    }
    QJsonObject params;
    params.insert(QStringLiteral("language"), language);
    params.insert(QStringLiteral("source"), source);
    params.insert(QStringLiteral("width"), widthPx);
    params.insert(QStringLiteral("devicePixelRatio"), devicePixelRatio);
    params.insert(QStringLiteral("theme"), theme);
    QElapsedTimer clock;
    clock.start();
    sendRequest(QStringLiteral("codeBlock.render"), params,
               [this, callback, clock](bool ok, const QJsonObject &result, const QString &error) mutable {
                   m_timing.lastRenderMs = clock.elapsed();
                   m_timing.slow = m_timing.slow || m_timing.lastRenderMs > kSlowRenderMs;
                   if (callback)
                       callback(ok, result, error);
               });
}

void PluginHost::notifyVaultEvent(const QString &event, const QString &path)
{
    if (!m_process || m_state != State::Running)
        return;
    QJsonObject params;
    params.insert(QStringLiteral("event"), event);
    params.insert(QStringLiteral("path"), path);
    sendMessage(PluginMessage::notification(QStringLiteral("vault.event"), params));
}

void PluginHost::sendMessage(const PluginMessage &message)
{
    if (!m_process)
        return;
    m_process->write(plugin_protocol_encode(message));
}

void PluginHost::handleReadyRead()
{
    m_reader.feed(m_process->readAllStandardOutput());
    while (true) {
        PluginMessage message;
        const PluginFrameReader::Status status = m_reader.next(&message);
        if (status == PluginFrameReader::Status::NeedMoreData)
            return;
        if (status == PluginFrameReader::Status::MalformedJSON) {
            if (m_callbacks.log)
                m_callbacks.log(m_manifest.id(), QStringLiteral("malformed frame from plugin host, dropped"));
            continue;
        }
        if (status == PluginFrameReader::Status::FrameTooLarge) {
            if (m_callbacks.log)
                m_callbacks.log(m_manifest.id(), QStringLiteral("oversized frame from plugin host, closing it"));
            m_process->kill();
            return;
        }
        dispatchIncoming(message);
    }
}

void PluginHost::dispatchIncoming(const PluginMessage &message)
{
    switch (message.kind) {
    case PluginMessage::Kind::Response: {
        const auto it = m_pending.find(message.id);
        if (it == m_pending.end())
            return; // a response to a request we already timed out and forgot
        const ResultCallback callback = it->callback;
        QTimer *timer = it->timer;
        const qint64 elapsedMs = it->clock.elapsed();
        const QString method = it->method;
        m_pending.erase(it);
        timer->stop(); // otherwise its already-scheduled timeout still fires once before deleteLater lands
        timer->deleteLater();
        if (method == QLatin1String("command.run")) {
            m_timing.lastCommandMs = elapsedMs;
            m_timing.slow = m_timing.slow || elapsedMs > kSlowCommandMs;
        }
        if (callback)
            callback(message.ok, message.result, message.errorMessage);
        break;
    }
    case PluginMessage::Kind::Request:
        handleHostRequest(message);
        break;
    case PluginMessage::Kind::Notification:
        if (message.method == QLatin1String("notice.show")) {
            if (m_callbacks.showNotice)
                m_callbacks.showNotice(message.params.value(QStringLiteral("message")).toString());
        } else if (message.method == QLatin1String("log")) {
            if (m_callbacks.log)
                m_callbacks.log(m_manifest.id(), message.params.value(QStringLiteral("message")).toString());
        }
        break;
    }
}

void PluginHost::refuseHostRequest(qint64 requestId, const QString &method, PluginPermission permission)
{
    const QString label = permissionLabel(permission);
    if (m_callbacks.log)
        m_callbacks.log(m_manifest.id(), QStringLiteral("refused %1: %2 not granted").arg(method, label));
    emit permissionRefused(method, label);
    sendMessage(PluginMessage::errorResponse(requestId, QStringLiteral("permission-denied"),
                                             label + QStringLiteral(" not granted")));
}

void PluginHost::handleHostRequest(const PluginMessage &request)
{
    const QString &method = request.method;
    const auto reply = [this, &request](const QJsonObject &result) {
        sendMessage(PluginMessage::response(request.id, result));
    };

    if (method == QLatin1String("note.activePath")) {
        if (!permissionGranted(PluginPermission::ReadNotes))
            return refuseHostRequest(request.id, method, PluginPermission::ReadNotes);
        QJsonObject result;
        result.insert(QStringLiteral("path"), m_callbacks.activeNotePath ? m_callbacks.activeNotePath() : QString());
        return reply(result);
    }
    if (method == QLatin1String("note.activeText")) {
        if (!permissionGranted(PluginPermission::ReadNotes))
            return refuseHostRequest(request.id, method, PluginPermission::ReadNotes);
        QJsonObject result;
        result.insert(QStringLiteral("text"), m_callbacks.activeNoteText ? m_callbacks.activeNoteText() : QString());
        return reply(result);
    }
    if (method == QLatin1String("note.read")) {
        if (!permissionGranted(PluginPermission::ReadNotes))
            return refuseHostRequest(request.id, method, PluginPermission::ReadNotes);
        const QString path = request.params.value(QStringLiteral("path")).toString();
        QJsonObject result;
        result.insert(QStringLiteral("text"), m_callbacks.readNote ? m_callbacks.readNote(path) : QString());
        return reply(result);
    }
    if (method == QLatin1String("note.list")) {
        if (!permissionGranted(PluginPermission::ReadNotes))
            return refuseHostRequest(request.id, method, PluginPermission::ReadNotes);
        QJsonArray paths;
        if (m_callbacks.listNotes) {
            for (const QString &path : m_callbacks.listNotes())
                paths.append(path);
        }
        QJsonObject result;
        result.insert(QStringLiteral("paths"), paths);
        return reply(result);
    }
    if (method == QLatin1String("note.replaceSelection")) {
        if (!permissionGranted(PluginPermission::WriteNotes))
            return refuseHostRequest(request.id, method, PluginPermission::WriteNotes);
        const QString text = request.params.value(QStringLiteral("text")).toString();
        const bool ok = m_callbacks.replaceSelection && m_callbacks.replaceSelection(text);
        QJsonObject result;
        result.insert(QStringLiteral("ok"), ok);
        return reply(result);
    }
    if (method == QLatin1String("note.write")) {
        if (!permissionGranted(PluginPermission::WriteNotes))
            return refuseHostRequest(request.id, method, PluginPermission::WriteNotes);
        const QString path = request.params.value(QStringLiteral("path")).toString();
        const QString text = request.params.value(QStringLiteral("text")).toString();
        const bool ok = m_callbacks.writeNote && m_callbacks.writeNote(path, text);
        QJsonObject result;
        result.insert(QStringLiteral("ok"), ok);
        return reply(result);
    }
    if (method == QLatin1String("clipboard.read")) {
        if (!permissionGranted(PluginPermission::Clipboard))
            return refuseHostRequest(request.id, method, PluginPermission::Clipboard);
        QJsonObject result;
        result.insert(QStringLiteral("text"), m_callbacks.clipboardRead ? m_callbacks.clipboardRead() : QString());
        return reply(result);
    }
    if (method == QLatin1String("clipboard.write")) {
        if (!permissionGranted(PluginPermission::Clipboard))
            return refuseHostRequest(request.id, method, PluginPermission::Clipboard);
        const QString text = request.params.value(QStringLiteral("text")).toString();
        const bool ok = m_callbacks.clipboardWrite && m_callbacks.clipboardWrite(text);
        QJsonObject result;
        result.insert(QStringLiteral("ok"), ok);
        return reply(result);
    }

    sendMessage(PluginMessage::errorResponse(request.id, QStringLiteral("unknown-method"),
                                             QStringLiteral("unknown method ") + method));
}

void PluginHost::failAllPending(const QString &error)
{
    const QHash<qint64, Pending> pending = m_pending;
    m_pending.clear();
    for (const Pending &entry : pending) {
        entry.timer->stop();
        entry.timer->deleteLater();
        if (entry.callback)
            entry.callback(false, {}, error);
    }
}

void PluginHost::handleFinished(int /*exitCode*/, int /*exitStatus*/)
{
    failAllPending(QStringLiteral("plugin host exited"));
    m_timing.consecutiveCrashes += 1;
    m_hasFailed = true;
    m_sinceFailure.start();
    setState(State::Crashed);
}

} // namespace noteAppC
